/* iop_hle.c - the IOP kernel, high level: the calls the sound driver imports (sysmem, loadcore, intrman, sifcmd,
 * stdio, thbase, vblank), threads that run until they block, interrupt dispatch, and the SIF RPC server table. */
#include <stdarg.h>
#include "iop_hle.h"

enum { L_SYSMEM = 1, L_LOADCORE, L_INTRMAN, L_SIFCMD, L_STDIO, L_THBASE, L_VBLANK, L_SYSCLIB, L_THEVENT, L_SIFMAN };

#define HEAP_END    0x001F6000u   /* stacks above: RPC 0x1F6000-0x1FA000, interrupts to 0x1FC000, main to 0x1FF000 */
#define MAIN_STACK  0x001FF000u
#define INTR_STACK  0x001FC000u
#define RPC_STACK   0x001FA000u

/* one heap for module images, AllocSysMemory and sceSifAllocIopHeap, as on the IOP (its kernel takes the first
 * 128 KB): about 1.8 MB, the stacks sit above HEAP_END. Blocks are freed again (FreeSysMemory,
 * sceSifFreeIopHeap): the sound driver allocates ~800 KB each time the game starts its sound system, which it
 * stops and starts again around every movie. */
#define HEAP_START 0x00020000u
#define MAX_BLOCKS 256
typedef struct block { u32 addr, size; } block;
static block used_b[MAX_BLOCKS];   /* allocated blocks, sorted by address */
static int nused;

typedef struct ithread { u32 entry, stack_top, gp, arg; int state; } ithread;   /* state 0 free, 1 ready, 2 ran */
static ithread threads[16];
static int cur_thread;

typedef struct handler { u32 func, arg, gp; int used; } handler;
static handler intr_h[64];
static u64 intr_enabled, intr_pending;
static int cpu_intr_on, in_intr, suspended;
static handler vbl_h[8];

typedef struct rpcsrv { u32 sid, func, buf, gp; } rpcsrv;
static rpcsrv servers[8];
static int nservers;

void iop_hle_reset(void)
{
    memset(threads, 0, sizeof threads);
    memset(intr_h, 0, sizeof intr_h);
    memset(vbl_h, 0, sizeof vbl_h);
    memset(servers, 0, sizeof servers);
    nservers = 0;
    intr_enabled = intr_pending = 0;
    cpu_intr_on = 1; in_intr = 0; suspended = 0;
    cur_thread = 0;
    nused = 0;
}

u32 iop_hle_main_stack(void) { return MAIN_STACK; }

/* type 0: the lowest free place, 1: the highest, 2: exactly at `at` (AllocSysMemory's types) */
static u32 heap_alloc(u32 size, int type, u32 at)
{
    u32 best = 0;
    int i, pos = -1;
    size = (size + 0xFF) & ~0xFFu;
    if (!size || nused >= MAX_BLOCKS) return 0;
    for (i = 0; i <= nused; i++) {                   /* the gap before block i (or after the last) */
        u32 lo = i ? used_b[i - 1].addr + used_b[i - 1].size : HEAP_START;
        u32 hi = i < nused ? used_b[i].addr : HEAP_END;
        u32 a;
        if (hi < lo || hi - lo < size) continue;
        if (type == 2) {
            a = at & ~0xFFu;
            if (a < lo || a + size > hi) continue;
        } else a = type == 1 ? hi - size : lo;
        best = a; pos = i;
        if (type != 1) break;                        /* lowest: the first fit; highest: the last one */
    }
    if (pos < 0) return 0;
    memmove(&used_b[pos + 1], &used_b[pos], (size_t)(nused - pos) * sizeof(block));
    used_b[pos].addr = best; used_b[pos].size = size;
    nused++;
    return best;
}

u32 iop_alloc(u32 size)
{
    u32 a = heap_alloc(size, 0, 0);
    if (!a) iop_log(0, "iop: out of IOP memory (%u bytes asked)", size);
    return a;
}

int iop_free(u32 addr)
{
    int i;
    addr &= IOP_RAM_SIZE - 1;
    for (i = 0; i < nused; i++) {
        if (used_b[i].addr == addr) {
            memmove(&used_b[i], &used_b[i + 1], (size_t)(nused - i - 1) * sizeof(block));
            nused--;
            return 0;
        }
    }
    iop_log(1, "iop: free of %06x, which is not allocated", addr);
    return -1;
}

/* ---- interrupts ------------------------------------------------------------------------------------ */

void iop_raise_intr(int intr) { if (intr >= 0 && intr < 64) intr_pending |= 1ull << intr; }

void iop_check_intr(void)
{
    int i;
    if (!cpu_intr_on || in_intr || suspended) return;
    for (i = 0; i < 64; i++) {
        u64 b = 1ull << i;
        if ((intr_pending & b) && (intr_enabled & b) && intr_h[i].used) {
            u32 gp = iop_gp;
            intr_pending &= ~b;
            in_intr = 1;
            iop_gp = intr_h[i].gp;
            iop_call(intr_h[i].func, intr_h[i].arg, 0, 0, 0, INTR_STACK);
            iop_gp = gp;
            in_intr = 0;
        }
    }
}

void iop_vblank(void)
{
    int i;
    for (i = 0; i < 8; i++) {
        if (vbl_h[i].used) {
            u32 gp = iop_gp, r;
            in_intr = 1;
            iop_gp = vbl_h[i].gp;
            r = iop_call(vbl_h[i].func, vbl_h[i].arg, 0, 0, 0, INTR_STACK);
            iop_gp = gp;
            in_intr = 0;
            if (r == 0) vbl_h[i].used = 0;   /* a handler returning 0 removes itself */
        }
    }
    iop_check_intr();
}

/* ---- threads --------------------------------------------------------------------------------------- */

void iop_run_threads(void)
{
    int i, again = 1;
    while (again) {
        again = 0;
        for (i = 1; i < 16; i++) {
            if (threads[i].state == 1) {
                int prev = cur_thread;
                u32 gp = iop_gp;
                threads[i].state = 2;
                cur_thread = i;
                iop_gp = threads[i].gp;
                iop_call(threads[i].entry, threads[i].arg, 0, 0, 0, threads[i].stack_top);
                iop_gp = gp;
                cur_thread = prev;
                again = 1;
            }
        }
    }
}

/* ---- RPC ------------------------------------------------------------------------------------------- */

int iop_rpc_find(u32 sid, u32 *buf)
{
    int i;
    for (i = 0; i < nservers; i++) if (servers[i].sid == sid) { if (buf) *buf = servers[i].buf; return 1; }
    return 0;
}

u32 iop_rpc_call(u32 sid, u32 fno, u32 size)
{
    int i;
    for (i = 0; i < nservers; i++) {
        if (servers[i].sid == sid) {
            u32 gp = iop_gp, r;
            iop_gp = servers[i].gp;
            r = iop_call(servers[i].func, fno, servers[i].buf, size, 0, RPC_STACK);
            iop_gp = gp;
            iop_check_intr();
            return r;
        }
    }
    return 0;
}

/* ---- printf from IOP memory ------------------------------------------------------------------------ */

static void iop_printf(u32 fmt_addr, const u32 *args, int nargs)
{
    char out[512], spec[16];
    u32 o = 0, ai = 0, f = fmt_addr;
    for (;;) {
        char c = (char)iop_read8(f++);
        if (!c || o > sizeof out - 64) break;
        if (c != '%') { out[o++] = c; continue; }
        {
            u32 k = 0;
            spec[k++] = '%';
            for (;;) {
                c = (char)iop_read8(f++);
                spec[k++] = c;
                if (!c || strchr("diouxXcspf%", c) || k > 12) break;
            }
            spec[k] = 0;
            if (c == '%') { out[o++] = '%'; continue; }
            if (!c) break;
            if (c == 's') {
                char s[128];
                u32 sa = ai < (u32)nargs ? args[ai++] : 0, n = 0;
                while (n < sizeof s - 1 && (s[n] = (char)iop_read8(sa + n)) != 0) n++;
                s[n] = 0;
                o += (u32)snprintf(out + o, sizeof out - o, spec, s);
            } else {
                u32 v = ai < (u32)nargs ? args[ai++] : 0;
                o += (u32)snprintf(out + o, sizeof out - o, spec, v);
            }
        }
    }
    out[o < sizeof out ? o : sizeof out - 1] = 0;
    while (o && (out[o - 1] == '\n' || out[o - 1] == '\r')) out[--o] = 0;
    if (o) iop_log(2, "iop: %s", out);
}

/* ---- the call table -------------------------------------------------------------------------------- */

static u32 stack_arg(int n) { return iop_read32(iop.r[29] + 16 + (u32)(n - 4) * 4); }

void iop_hle_call(u32 code)
{
    u32 lib = code >> 8, idx = code & 0xFF;
    u32 *r = iop.r;
    u32 a0 = r[4], a1 = r[5], a2 = r[6], a3 = r[7], v0 = 0;
    switch (lib) {
    case L_SYSMEM:
        if (idx == 4) {                            /* AllocSysMemory(type, size, addr) */
            v0 = heap_alloc(a1, (int)(a0 <= 2 ? a0 : 0), a2);
            if (!v0) iop_log(0, "iop: out of IOP memory (%u bytes asked)", a1);
        }
        else if (idx == 5) v0 = (u32)iop_free(a0); /* FreeSysMemory(addr) */
        break;
    case L_LOADCORE: break;                        /* FlushIcache/FlushDcache/RegisterLibraryEntries */
    case L_INTRMAN:
        switch (idx) {
        case 4: if (a0 < 64) { intr_h[a0].func = a2; intr_h[a0].arg = a3; intr_h[a0].gp = r[28]; intr_h[a0].used = 1; } break;
        case 5: if (a0 < 64) intr_h[a0].used = 0; break;
        case 6: if (a0 < 64) intr_enabled |= 1ull << a0; break;
        case 7: if (a0 < 64) intr_enabled &= ~(1ull << a0); if (a1) iop_write32(a1, a0); break;
        case 8: cpu_intr_on = 0; break;
        case 9: cpu_intr_on = 1; break;
        case 17: if (a0) iop_write32(a0, (u32)suspended); suspended = 1; break;  /* CpuSuspendIntr */
        case 18: suspended = (int)(a0 & 1); break;                                /* CpuResumeIntr */
        case 23: v0 = (u32)in_intr; break;                                        /* QueryIntrContext */
        default: iop_log(1, "iop: intrman %u not handled", idx); break;
        }
        break;
    case L_SIFCMD:
        switch (idx) {
        case 14: break;                                                  /* sceSifInitRpc */
        case 17:                                                         /* sceSifRegisterRpc(sd, sid, func, buf, ...) */
            if (nservers < 8) {
                servers[nservers].sid = a1; servers[nservers].func = a2; servers[nservers].buf = a3;
                servers[nservers].gp = r[28];
                nservers++;
                iop_log(2, "iop: RPC server %08x registered", a1);
            }
            break;
        case 19: break;                                                  /* sceSifSetRpcQueue */
        case 22: iop_halt = 1; break;                                    /* sceSifRpcLoop: the thread waits here */
        default: iop_log(1, "iop: sifcmd %u not handled", idx); break;
        }
        break;
    case L_STDIO:
        if (idx == 4) { u32 args[8] = { a1, a2, a3, stack_arg(4), stack_arg(5), stack_arg(6), stack_arg(7), stack_arg(8) }; iop_printf(a0, args, 8); }
        break;
    case L_THBASE:
        switch (idx) {
        case 4: { /* CreateThread(param): attr, option, entry, stacksize, priority */
            int i;
            for (i = 1; i < 16 && threads[i].state; i++) ;
            if (i < 16) {
                u32 ss = iop_read32(a0 + 12);
                u32 st = iop_alloc(ss ? ss : 0x1000);
                threads[i].entry = iop_read32(a0 + 8);
                threads[i].stack_top = st + (ss ? ss : 0x1000) - 16;
                threads[i].gp = r[28];
                threads[i].state = 3;   /* created, not started */
                v0 = (u32)i;
            } else v0 = (u32)-1;
            break;
        }
        case 6: if (a0 < 16 && threads[a0].state == 3) { threads[a0].state = 1; threads[a0].arg = a1; } break; /* StartThread */
        case 20: v0 = (u32)cur_thread; break;                                                                   /* GetThreadId */
        default: iop_log(1, "iop: thbase %u not handled", idx); break;
        }
        break;
    case L_VBLANK:
        if (idx == 8) {   /* RegisterVblankHandler(edge, priority, handler, arg) */
            int i;
            for (i = 0; i < 8 && vbl_h[i].used; i++) ;
            if (i < 8) { vbl_h[i].func = a2; vbl_h[i].arg = a3; vbl_h[i].gp = r[28]; vbl_h[i].used = 1; }
        }
        break;
    default:
        iop_log(1, "iop: kernel call %u.%u not handled", lib, idx);
        break;
    }
    r[2] = v0;
}
