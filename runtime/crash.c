/*
 * Crash reporting and EE-compatible integer division.
 *
 * The EE never traps on integer division (x/0 gives LO = x<0 ? 1 : -1, HI = x; 0x80000000/-1 gives 0x80000000),
 * x86 does. The game divides in 349 places and its C was written for the EE, so a vectored handler finishes
 * a faulting 32-bit div/idiv the EE way and continues. Any other fault is logged with the game function it
 * happened in, then the run stops.
 */
#include <windows.h>
#include <stdio.h>
#include "runtime.h"
#include "rc_port.h"

static HANDLE g_game_thread;                                    /* the watchdog's view of the game thread */
void report_crash(const char *callpath, int on_game_thread);   /* runtime/report.c */
void report_hang(const char *callpath);

#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")

/* exact function name (and source line) for a native address, from dotr.pdb */
static void where(uintptr_t a, char *out, int n)
{
    static int inited;
    char buf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *s = (SYMBOL_INFO *)buf;
    DWORD64 disp = 0;
    IMAGEHLP_LINE64 line;
    DWORD ld = 0;
    if (!inited) { SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME); SymInitialize(GetCurrentProcess(), NULL, TRUE); inited = 1; }
    memset(buf, 0, sizeof buf);
    s->SizeOfStruct = sizeof(SYMBOL_INFO);
    s->MaxNameLen = 255;
    if (!SymFromAddr(GetCurrentProcess(), a, &disp, s)) { snprintf(out, n, "%08lX", (unsigned long)a); return; }
    line.SizeOfStruct = sizeof line;
    if (SymGetLineFromAddr64(GetCurrentProcess(), a, &ld, &line)) {
        const char *f = strrchr(line.FileName, '\\');
        snprintf(out, n, "%s+0x%llX (%s:%lu)", s->Name, disp, f ? f + 1 : line.FileName, line.LineNumber);
    } else snprintf(out, n, "%s+0x%llX", s->Name, disp);
}

static const ps2_func *nearest(uintptr_t eip)
{
    const ps2_func *best = NULL;
    uint32_t i;
    for (i = 0; i < ps2_functab_count; i++) {
        uintptr_t n = (uintptr_t)ps2_functab[i].native;
        if (n <= eip && (!best || n > (uintptr_t)best->native)) best = &ps2_functab[i];
    }
    return best;
}

/* length of a ModRM operand (+ SIB + displacement) starting at p (after the opcode) */
static int modrm_len(const uint8_t *p)
{
    int mod = p[0] >> 6, rm = p[0] & 7, n = 1;
    if (mod != 3 && rm == 4) n++;                          /* SIB */
    if (mod == 0 && rm == 5) n += 4;
    else if (mod == 0 && rm == 4 && (p[1] & 7) == 5) n += 4;
    else if (mod == 1) n += 1;
    else if (mod == 2) n += 4;
    return n;
}

/* PS2 low memory. The EE kernel's RAM (0x00000000..0x0007FFFF) and the space below the game's ELF are readable on
 * the PS2, and the game reads there through null pointers (TaLogic007_IsRevivePlay reads byte 3 when a grave card
 * has no info: the matched asm does it too). Windows cannot map the first 64 KB and the loader sits above it, so
 * such an access faults here. The faulting instruction is run once more with its base register moved into a
 * private 1 MB block standing in for that memory (zero to start with, writes kept), then the register is put
 * back. Logged once per place. */
#define LOW_SIZE 0x00100000u
static uint8_t *g_low;                     /* the stand-in block (+4 KB for wide accesses at its end) */
static uintptr_t g_dll_base;
static __declspec(thread) int t_low_reg = -1;
static __declspec(thread) DWORD t_low_saved;

static DWORD *ctx_reg(CONTEXT *c, int r)
{
    switch (r) {
    case 0: return &c->Eax; case 1: return &c->Ecx; case 2: return &c->Edx; case 3: return &c->Ebx;
    case 4: return &c->Esp; case 5: return &c->Ebp; case 6: return &c->Esi; default: return &c->Edi;
    }
}

/* the register that holds the memory operand's base in the instruction at p (index when it has no base and a
 * scale of 1), or -1 */
static int mem_base_reg(const uint8_t *p)
{
    int op, mod, rm, b, i;
    while (*p == 0x66 || *p == 0x67 || *p == 0xF2 || *p == 0xF3 || *p == 0xF0 || *p == 0x2E || *p == 0x3E ||
           *p == 0x26 || *p == 0x36 || *p == 0x64 || *p == 0x65) p++;
    op = *p++;
    if (op == 0x0F) {
        op = *p++;
        if (op == 0x38 || op == 0x3A) p++;
        else if ((op >= 0x05 && op <= 0x0B) || (op >= 0x30 && op <= 0x37) || op == 0x77 || (op >= 0x80 && op <= 0x8F) ||
                 op == 0xA0 || op == 0xA1 || op == 0xA2 || op == 0xA8 || op == 0xA9 || op == 0xAA || (op >= 0xC8 && op <= 0xCF))
            return -1;
    } else if (!((op & 0xC4) == 0x00 || op == 0x62 || op == 0x63 || op == 0x69 || op == 0x6B ||
                 (op >= 0x80 && op <= 0x8F) || op == 0xC0 || op == 0xC1 || (op >= 0xC4 && op <= 0xC7) ||
                 (op >= 0xD0 && op <= 0xD3) || (op >= 0xD8 && op <= 0xDF) || op == 0xF6 || op == 0xF7 ||
                 op == 0xFE || op == 0xFF)) {
        return -1;                                     /* no ModRM: string ops, absolute moffs, push/pop */
    }
    mod = p[0] >> 6; rm = p[0] & 7;
    if (mod == 3) return -1;
    if (rm != 4) return (mod == 0 && rm == 5) ? -1 : rm;
    b = p[1] & 7; i = (p[1] >> 3) & 7;
    if (!(mod == 0 && b == 5)) return (i != 4 && i == b) ? -1 : b;
    return (i != 4 && (p[1] >> 6) == 0) ? i : -1;
}

static int low_memory_access(EXCEPTION_POINTERS *e)
{
    EXCEPTION_RECORD *x = e->ExceptionRecord;
    CONTEXT *c = e->ContextRecord;
    MEMORY_BASIC_INFORMATION mbi;
    static uintptr_t seen[256];
    int r, i;
    if (x->ExceptionCode == EXCEPTION_SINGLE_STEP && t_low_reg >= 0) {
        DWORD *v = ctx_reg(c, t_low_reg);
        if (*v == t_low_saved + (DWORD)(uintptr_t)g_low) *v = t_low_saved;   /* unless the instruction loaded it */
        t_low_reg = -1;
        c->EFlags &= ~0x100u;
        return 1;
    }
    if (x->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || x->NumberParameters < 2 || !g_low ||
        x->ExceptionInformation[0] == 8 || x->ExceptionInformation[1] >= LOW_SIZE)
        return 0;
    if (!VirtualQuery((void *)c->Eip, &mbi, sizeof mbi) || (uintptr_t)mbi.AllocationBase != g_dll_base) return 0;
    if ((r = mem_base_reg((const uint8_t *)c->Eip)) < 0 || r == 4) return 0;
    for (i = 0; i < 256 && seen[i] && seen[i] != c->Eip; i++) {}
    if (i < 256 && !seen[i]) {
        const ps2_func *f = nearest(c->Eip);
        char w[400];
        seen[i] = c->Eip;
        where(c->Eip, w, sizeof w);
        hw_log(1, "%s of PS2 low memory %08lX in %s (PS2 %08X) at %s: served from the stand-in block, frame %u",
               x->ExceptionInformation[0] ? "write" : "read", (unsigned long)x->ExceptionInformation[1],
               f ? f->name : "?", f ? f->addr : 0, w, rt_vblanks());
    }
    t_low_reg = r;
    t_low_saved = *ctx_reg(c, r);
    *ctx_reg(c, r) += (DWORD)(uintptr_t)g_low;
    c->EFlags |= 0x100u;                         /* one step, then the register goes back (EXCEPTION_SINGLE_STEP) */
    return 1;
}

/* what the game was doing, for a crash or hang line: its mode (SzGame_GetMode: 1 duel, 6 deck, 10 title, 36 memory
 * card check, -1 developer menu ...) and the module switched in */
#ifndef DOTR_NO_GAME
extern long long SzGame_GetMode(int which);
#endif
static const char *game_state(void)
{
    static char s[128];
    const char *m = rc_current_module();
#ifndef DOTR_NO_GAME
    snprintf(s, sizeof s, "game mode %d, module %s", (int)SzGame_GetMode(0), m ? m : "none");
#else
    snprintf(s, sizeof s, "module %s", m ? m : "none");
#endif
    return s;
}

/* Developer aid (bot duels, DOTR_BOT_WATCH=<hex address>): a hardware write watch on 4 bytes of PS2 memory; each
 * instruction that writes there is logged once (who stores x86 leftovers into game state). */
static uint32_t g_watch_addr;
void crash_watch_write(uint32_t addr)
{
    g_watch_addr = addr;
    __debugbreak();                              /* the handler below arms DR0 on this thread */
}

static int watch_exception(EXCEPTION_POINTERS *e)
{
    CONTEXT *c = e->ContextRecord;
    static uintptr_t seen[16];
    int i;
    if (!g_watch_addr) return 0;
    if (e->ExceptionRecord->ExceptionCode == EXCEPTION_BREAKPOINT && *(uint8_t *)c->Eip == 0xCC) {
        c->Dr0 = g_watch_addr;
        c->Dr7 = (c->Dr7 & ~0x000F0003u) | 0x1u | (1u << 16) | (3u << 18);   /* L0, write, 4 bytes */
        c->Eip += 1;
        hw_log(2, "watch: writes to %08X", g_watch_addr);
        return 1;
    }
    if (e->ExceptionRecord->ExceptionCode == EXCEPTION_SINGLE_STEP && (c->Dr6 & 1)) {
        c->Dr6 &= ~0xFu;
        if (getenv("DOTR_BOT_WATCH_NONZERO") && !*(volatile uint32_t *)(uintptr_t)g_watch_addr) return 1;
        for (i = 0; i < 16 && seen[i] && seen[i] != c->Eip; i++) {}
        if (i < 16 && !seen[i]) {
            char w[400];
            seen[i] = c->Eip;
            where(c->Eip, w, sizeof w);
            hw_log(2, "watch: %08X = %08X written at %s, frame %u", g_watch_addr,
                   *(volatile uint32_t *)(uintptr_t)g_watch_addr, w, rt_vblanks());
        }
        return 1;
    }
    return 0;
}

static LONG CALLBACK veh(EXCEPTION_POINTERS *e)
{
    DWORD code = e->ExceptionRecord->ExceptionCode;
    CONTEXT *c = e->ContextRecord;
    const uint8_t *ip = (const uint8_t *)c->Eip;
    if (low_memory_access(e)) return EXCEPTION_CONTINUE_EXECUTION;
    if (watch_exception(e)) return EXCEPTION_CONTINUE_EXECUTION;
    if (code == EXCEPTION_INT_DIVIDE_BY_ZERO || code == EXCEPTION_INT_OVERFLOW) {
        int pre = 0;
        while (ip[pre] == 0x66 || ip[pre] == 0x2E || ip[pre] == 0x3E) pre++;
        if (ip[pre] == 0xF7 && ((ip[pre + 1] >> 3) & 7) >= 6) {
            int is_signed = ((ip[pre + 1] >> 3) & 7) == 7;
            int32_t x = (int32_t)c->Eax;
            if (code == EXCEPTION_INT_OVERFLOW) { c->Eax = 0x80000000u; c->Edx = 0; }
            else if (is_signed) { c->Eax = x < 0 ? 1u : 0xFFFFFFFFu; c->Edx = (DWORD)x; }
            else { c->Eax = 0xFFFFFFFFu; c->Edx = (DWORD)x; }
            c->Eip += pre + 1 + modrm_len(ip + pre + 1);
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    /* the EE's floating point: SSE traps on the game's thread (runtime/eefloat.c) */
    if (ee_float_exception(e)) return EXCEPTION_CONTINUE_EXECUTION;
    /* A call into PS2 module code (an overlay loaded at 0x01E00000.. by mwOverlayInit) that has no PC version
     * yet: the module recompiler (recomp/) provides those. Until then the call returns 0, logged once per
     * address, so menus and the rest of the game go on without that effect. */
    if (code == EXCEPTION_ACCESS_VIOLATION && e->ExceptionRecord->NumberParameters > 1 &&
        e->ExceptionRecord->ExceptionInformation[0] == 8 && c->Eip == e->ExceptionRecord->ExceptionInformation[1] &&
        c->Eip >= 0x00100000 && c->Eip < 0x02000000 && !IsBadReadPtr((void *)c->Esp, 4)) {
        static uintptr_t seen[64];
        int i;
        for (i = 0; i < 64 && seen[i] && seen[i] != c->Eip; i++) {}
        if (i < 64 && !seen[i]) {
            seen[i] = c->Eip;
            hw_log(1, "module code at %08lX not ported yet: call skipped (returns 0)", c->Eip);
        }
        c->Eip = *(DWORD *)c->Esp;
        c->Esp += 4;
        c->Eax = 0;
        c->Edx = 0;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_ILLEGAL_INSTRUCTION ||
        code == EXCEPTION_INT_DIVIDE_BY_ZERO || code == EXCEPTION_STACK_OVERFLOW ||
        code == EXCEPTION_PRIV_INSTRUCTION) {
        const ps2_func *f = nearest(c->Eip);
        ULONG_PTR addr = e->ExceptionRecord->NumberParameters > 1 ? e->ExceptionRecord->ExceptionInformation[1] : 0;
        con_printf("DotR-PC crashed: exception %08lX at %08lX in %s+0x%lX (PS2 %08X), address %08lX, frame %u\n",
                   code, c->Eip, f ? f->name : "?", f ? (unsigned long)(c->Eip - (uintptr_t)f->native) : 0,
                   f ? f->addr : 0, (unsigned long)addr, rt_vblanks());
        hw_log(0, "CRASH %08lX eip=%08lX in %s esp=%08lX addr=%08lX, frame %u, %s", code, c->Eip, f ? f->name : "?",
               c->Esp, (unsigned long)addr, rt_vblanks(), game_state());
        {
            char w[400];
            static char cp[4096];          /* the call path for the report */
            int i, shown = 0, n;
            where(c->Eip, w, sizeof w);
            con_printf("   at %s\n", w);
            hw_log(0, "   at %s", w);
            n = snprintf(cp, sizeof cp, "exception %08lX in %s, address %08lX\n  at %s\n", code, f ? f->name : "?",
                         (unsigned long)addr, w);
            /* frame-pointer chain first, then return-address-looking stack words */
            for (i = 0; i < 512 && shown < 24; i++) {
                uintptr_t v;
                MEMORY_BASIC_INFORMATION mbi;
                if (IsBadReadPtr((void *)(c->Esp + 4 * i), 4)) break;
                v = *(uintptr_t *)(c->Esp + 4 * i);
                if (v < 0x10000 || !VirtualQuery((void *)v, &mbi, sizeof mbi) || mbi.Type != MEM_IMAGE ||
                    !(mbi.Protect & (PAGE_EXECUTE_READ | PAGE_EXECUTE | PAGE_EXECUTE_READWRITE)))
                    continue;
                where(v, w, sizeof w);
                if (!strncmp(w, "Sym", 3)) continue;
                con_printf("   from %s\n", w);
                hw_log(0, "   from %s", w);
                if (n < (int)sizeof cp - 1) n += snprintf(cp + n, sizeof cp - n, "  from %s\n", w);
                shown++;
            }
            report_crash(cp, GetCurrentThreadId() == GetThreadId(g_game_thread));
        }
        log_close();
        rt_exit(3);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

/* Watchdog: if no vblank happens for 5 seconds the game is stuck (usually polling something the PC layer does
 * not provide yet). Report the function the game thread is in, a few return addresses, and stop. */

/* CPU time the game thread has used, in 100 ns units */
static ULONGLONG game_cpu(void)
{
    FILETIME a, b, k, u;
    if (!GetThreadTimes(g_game_thread, &a, &b, &k, &u)) return 0;
    return ((ULONGLONG)k.dwHighDateTime << 32 | k.dwLowDateTime) + ((ULONGLONG)u.dwHighDateTime << 32 | u.dwLowDateTime);
}

/* the game thread's CPU time so far, microseconds (bot duels: the gate's speed check) */
unsigned long long rt_game_cpu_us(void) { return game_cpu() / 10; }

/* Stuck = no new frame while the game thread used 5 s of its own CPU (15 s before the first field: boot loads),
 * i.e. a loop that never ends. Only the game thread's own CPU counts: a busy PC, Idle priority (a runnable but
 * starved thread) or sleep are not the game's hang. A wait on another fiber never stops the frames (the scheduler
 * runs the vblank whenever no fiber is ready), so it cannot look stuck here.
 * Headless and test runs stop with exit 4; the owner's window logs the report once and keeps running. */
int rt_headless;
static DWORD WINAPI watchdog(void *p)
{
    hw_u32 last = 0;
    unsigned last_checks = 0;
    ULONGLONG cpu0 = 0, cpu;
    int reported = 0, have = 0;
    (void)p;
    for (;;) {
        Sleep(500);
        cpu = game_cpu();
        if (rt_vblanks() != last || rc_oracle_checks != last_checks || !have) {    /* oracle checks are progress too */
            if (reported) hw_log(0, "STUCK over: frames again from frame %u", rt_vblanks());
            last = rt_vblanks(); last_checks = rc_oracle_checks; cpu0 = cpu; have = 1; reported = 0; continue;
        }
        if (reported || cpu - cpu0 < (rt_vblanks() ? 50000000ull : 150000000ull)) continue;
        {
            CONTEXT c;
            const ps2_func *f;
            int i, n;
            static char cp[4096];
            memset(&c, 0, sizeof c);
            c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
            SuspendThread(g_game_thread);
            GetThreadContext(g_game_thread, &c);
            f = nearest(c.Eip);
            if (rt_headless)
                con_printf("DotR-PC stuck: no frame (game busy %u s) at frame %u, in %s (PS2 %08X), eip %08lX\n",
                           (unsigned)((cpu - cpu0) / 10000000ull), rt_vblanks(), f ? f->name : "?", f ? f->addr : 0, c.Eip);
            hw_log(0, "STUCK (busy %u s) at frame %u in %s eip=%08lX, %s", (unsigned)((cpu - cpu0) / 10000000ull),
                   rt_vblanks(), f ? f->name : "?", c.Eip, game_state());
            /* return addresses on the stack that point into game code */
            n = snprintf(cp, sizeof cp, "game busy %u s without a new field\n  in %s (PS2 %08X)\n",
                         (unsigned)((cpu - cpu0) / 10000000ull), f ? f->name : "?", f ? f->addr : 0);
            for (i = 0; i < 256; i++) {
                uintptr_t v;
                if (IsBadReadPtr((void *)(c.Esp + 4 * i), 4)) break;
                v = *(uintptr_t *)(c.Esp + 4 * i);
                f = nearest(v);
                if (f && v - (uintptr_t)f->native < 0x4000) {
                    if (rt_headless) con_printf("   called from %s (PS2 %08X)\n", f->name, f->addr);
                    hw_log(0, "   from %s", f->name);
                    if (n < (int)sizeof cp - 1) n += snprintf(cp + n, sizeof cp - n, "  from %s (PS2 %08X)\n", f->name, f->addr);
                }
            }
            report_hang(cp);                     /* the game thread is still suspended: no picture, GS state only */
            if (rt_headless) {
                log_close();
                rt_exit(4);
            }
            ResumeThread(g_game_thread);         /* the window: the report is in the log, the game goes on */
            reported = 1;
        }
    }
}

/* Where the calling thread is right now (function + line, then return addresses on its stack), to the log:
 * a --frames run that ends shows what the game was doing. */
void crash_log_here(const char *why)
{
    char w[400];
    int i, shown = 0;
    uintptr_t esp = (uintptr_t)(w + sizeof w);   /* above this frame: the buffer itself holds stale words */
    hw_log(2, "%s; the game was in:", why);
    for (i = 0; i < 2048 && shown < 20; i++) {
        uintptr_t v;
        MEMORY_BASIC_INFORMATION mbi;
        if (IsBadReadPtr((void *)(esp + 4 * i), 4)) break;
        v = *(uintptr_t *)(esp + 4 * i);
        if (v < 0x10000 || !VirtualQuery((void *)v, &mbi, sizeof mbi) || mbi.Type != MEM_IMAGE ||
            !(mbi.Protect & (PAGE_EXECUTE_READ | PAGE_EXECUTE | PAGE_EXECUTE_READWRITE)))
            continue;
        where(v, w, sizeof w);
        if (!strncmp(w, "Sym", 3)) continue;
        hw_log(2, "   from %s", w);
        shown++;
    }
}

static void low_init(void)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (g_low) return;
    if (VirtualQuery((void *)veh, &mbi, sizeof mbi)) g_dll_base = (uintptr_t)mbi.AllocationBase;
    g_low = VirtualAlloc(NULL, LOW_SIZE + 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
}

static LONG CALLBACK low_veh(EXCEPTION_POINTERS *e)
{
    return low_memory_access(e) ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
}

/* self-test: a null-pointer read (byte 3, as TaLogic007_IsRevivePlay does) and a write + read back at 0x40 */
int crash_low_memory_check(void)
{
    /* the pointers themselves volatile: loaded into a register, as game code holds them */
    volatile uint8_t *volatile p3 = (volatile uint8_t *)(uintptr_t)3, *volatile p40 = (volatile uint8_t *)(uintptr_t)0x40;
    volatile uint32_t *volatile q = (volatile uint32_t *)(uintptr_t)0x40;
    void *h;
    int a, b;
    low_init();
    h = AddVectoredExceptionHandler(1, low_veh);
    a = *p3;
    *q = 0x12345678u;
    b = *p40;
    *q = 0;
    RemoveVectoredExceptionHandler(h);
    return a == 0 && b == 0x78;
}

void crash_install(void)
{
    low_init();
    AddVectoredExceptionHandler(1, veh);
    ee_float_init();                          /* the EE's float rules on this (the game's) thread */
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_game_thread, 0, FALSE,
                    DUPLICATE_SAME_ACCESS);
    CreateThread(NULL, 0, watchdog, NULL, 0, NULL);
}
