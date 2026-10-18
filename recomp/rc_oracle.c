/*
 * rc_oracle.c - the main-program oracle: a function's ORIGINAL EE code (read from SLUS_205.15 on the disc) runs on
 * the reference interpreter, and the port's native C function runs from the same memory state with the same
 * arguments; results and every changed byte of PS2 memory are compared.
 *
 * Shallow mode (deep = 0): only the function itself is interpreted; every call it makes goes to the native callee
 * (through the rcx_ thunks), so a difference points at this function's port, not at its callees.
 * Deep mode (deep = 1): callees inside the game's code are interpreted too (stops on VU0 or syscall code).
 *
 * Memory: PS2 RAM 0x00100000..0x01FFFFFF and the scratchpad are compared. The interpreter's stack is the calling
 * fiber's module stack (0x02000000.., outside the compared range); native code runs on the x86 stack.
 * After the check, memory is left exactly as the native run left it, so the game goes on unaffected.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <xmmintrin.h>

#include "rc_port.h"
#include "rc_interp.h"

#define RAM_LO 0x00100000u
#define RAM_HI 0x02000000u
#define SPR    0x70000000u
#define SPR_SZ 0x4000u

typedef struct RcMainFunc { uint32_t addr, size; const char *name; } RcMainFunc;
extern const RcMainFunc rc_main_extents[];
extern const unsigned rc_nmain_extents;
extern const uint32_t rc_native_only[];
extern const unsigned rc_nnative_only;
const RcFunc *rc_main_func(uint32_t addr);
int rc_main_ret(uint32_t addr);
RcCpu *rc_cpu_current(void);

static uint8_t *elf;
static uint32_t nseg, seg_va[8], seg_sz[8], seg_off[8];
static uint8_t *snap, *after, *spr_snap, *spr_after;
static int restore_pre;
static unsigned check_mxcsr;                    /* MXCSR at the start of the last check (traps armed?) */
/* While a check runs the runtime must not yield (rt_poll_slow returns at once) and must call rc_oracle_abort()
 * instead of blocking: the check is then abandoned, memory restored, and that function is never checked again. */
int rc_oracle_busy;
/* After a longjmp out of an abandoned check: the runtime may have been between a masked re-run and its single-step
 * re-arm (runtime/eefloat.c), which is now lost: arm the EE float traps again (invalid, divide-by-zero, overflow
 * unmasked; FTZ/DAZ on; flags clear). */
static void rc_oracle_rearm(void) { _mm_setcsr((_mm_getcsr() | 0x8040u) & ~0x06BFu); }
unsigned rc_oracle_checks;
static jmp_buf abort_jb;
static uint8_t *hle_set;                        /* functions the runtime provides (native_only.txt): 1 bit per word */
static uint32_t missing_target, hle_called;     /* during a check: a call with no thunk / first runtime-provided callee */

/* every call the interpreter makes to native code during a check comes through here (rc_dispatch) */
void rc_oracle_note_call(uint32_t addr, int known)
{
    uint32_t k = (addr - 0x00100000u) / 4;
    if (!known) { missing_target = addr; longjmp(abort_jb, 2); }
    if (!hle_called && hle_set && addr >= 0x00100000u && addr < 0x00300000u && (hle_set[k / 8] >> (k % 8)) & 1)
        hle_called = addr;
}
void rc_oracle_abort(void) { if (rc_oracle_busy) longjmp(abort_jb, 1); }
static uint8_t *native_set;
static uint8_t *unsafe_set;
static void build_unsafe_set(void);
static int deep_active;
static void build_native_set(void);                         /* 1: leave memory as it was before the check (the hook) */

static uint8_t mod_img[0x8000];                 /* the current module's original bytes (module oracle) */
static int mod_img_ok;
static int (*g_find)(const char *, uint32_t *, uint32_t *);
static int (*g_read)(uint64_t, uint32_t, void *);

uint32_t rc_oracle_fetch(uint32_t pc)
{
    if (pc - 0x01E00000u < sizeof mod_img) {
        uint32_t w = 0;
        if (mod_img_ok) memcpy(&w, mod_img + (pc - 0x01E00000u), 4);
        return w;
    }
    for (uint32_t i = 0; i < nseg; i++)
        if (pc - seg_va[i] < seg_sz[i]) {
            uint32_t w;
            memcpy(&w, elf + seg_off[i] + (pc - seg_va[i]), 4);
            return w;
        }
    return 0;
}

const char *rc_oracle_name(uint32_t addr)
{
    unsigned lo = 0, hi = rc_nmain_extents;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (rc_main_extents[mid].addr <= addr) lo = mid + 1;
        else hi = mid;
    }
    if (lo && addr - rc_main_extents[lo - 1].addr < rc_main_extents[lo - 1].size) return rc_main_extents[lo - 1].name;
    return NULL;
}

static const RcMainFunc *extent(uint32_t addr)
{
    for (unsigned i = 0; i < rc_nmain_extents; i++)
        if (rc_main_extents[i].addr == addr) return &rc_main_extents[i];
    return NULL;
}

int rc_oracle_init(int (*find)(const char *, uint32_t *, uint32_t *), int (*read)(uint64_t, uint32_t, void *),
                   char *msg, int len)
{
    uint32_t lba, size, phoff, phnum, phent;
    g_find = find;
    g_read = read;
    if (elf) return 1;
    if (!find("\\SLUS_205.15", &lba, &size)) { snprintf(msg, len, "SLUS_205.15 not on the disc"); return 0; }
    elf = (uint8_t *)malloc(size);
    if (!elf) { snprintf(msg, len, "out of memory"); return 0; }
    if (!read((uint64_t)lba * 2048, size, elf)) { snprintf(msg, len, "SLUS_205.15 read failed"); return 0; }
    memcpy(&phoff, elf + 0x1C, 4);
    phent = elf[0x2A] | elf[0x2B] << 8;
    phnum = elf[0x2C] | elf[0x2D] << 8;
    for (uint32_t i = 0; i < phnum && nseg < 8; i++) {
        const uint8_t *ph = elf + phoff + i * phent;
        uint32_t type, off, va, filesz;
        memcpy(&type, ph, 4); memcpy(&off, ph + 4, 4); memcpy(&va, ph + 8, 4); memcpy(&filesz, ph + 16, 4);
        if (type != 1 || off + filesz > size) continue;
        seg_va[nseg] = va; seg_sz[nseg] = filesz; seg_off[nseg] = off; nseg++;
    }
    snprintf(msg, len, "oracle: SLUS_205.15 %u bytes, %u code/data segments", size, nseg);
    return nseg != 0;
}


/* The live part of the calling fiber's x86 stack (the hooked function's frame and its callers', above the hook's
 * own frame): native callers hand out pointers to their locals, so it is snapshotted and compared like PS2 RAM. */
#define XS_MAX 0x400000u                        /* the whole fixed stack area (loader_abi.h) */
static uintptr_t xs_lo, xs_hi;
static uint8_t xs_snap[XS_MAX], xs_after[XS_MAX];
static __declspec(align(16)) uint8_t oracle_stack[0x40000];   /* the interpreted code's EE stack during a check */

static void save(uint8_t *ram, uint8_t *spr)
{
    memcpy(ram, (void *)(uintptr_t)RAM_LO, RAM_HI - RAM_LO);
    memcpy(spr, (void *)(uintptr_t)SPR, SPR_SZ);
    if (xs_hi > xs_lo) memcpy(ram == snap ? xs_snap : xs_after, (void *)xs_lo, xs_hi - xs_lo);
}

static void load(const uint8_t *ram, const uint8_t *spr)
{
    memcpy((void *)(uintptr_t)RAM_LO, ram, RAM_HI - RAM_LO);
    memcpy((void *)(uintptr_t)SPR, spr, SPR_SZ);
    if (xs_hi > xs_lo) memcpy((void *)xs_lo, ram == snap ? xs_snap : xs_after, xs_hi - xs_lo);
}

/* The module oracle (rc_oracle_mod.h) reuses the check with its own native code (the translation), code range and
 * name; ret 5 = compare v0, v1 and f0 exactly. */
static void (*ov_native)(RcCpu *);
static uint32_t ov_lo, ov_hi;
static const char *ov_name;

int rc_oracle_run(uint32_t addr, const uint64_t *iargs, int ni, const float *fargs, int nf, int deep,
                  RcOracleResult *r)
{
    const RcFunc *nf_ = ov_native ? NULL : rc_main_func(addr);
    void (*native)(RcCpu *) = ov_native ? ov_native : nf_ ? nf_->fn : NULL;
    const RcMainFunc *ext = ov_native ? NULL : extent(addr);
    RcCpu *base = rc_cpu_current();
    static RcCpu cn, ci;                        /* static: they survive a longjmp back from an abandoned check */
    const char *name = ov_native ? ov_name : rc_oracle_name(addr);
    int st;
    memset(r, 0, sizeof *r);
    if (!snap) {                                /* the snapshots (62 MB) only when the oracle is used */
        snap = (uint8_t *)malloc(RAM_HI - RAM_LO);
        after = (uint8_t *)malloc(RAM_HI - RAM_LO);
        spr_snap = (uint8_t *)malloc(SPR_SZ);
        spr_after = (uint8_t *)malloc(SPR_SZ);
        if (!snap || !after || !spr_snap || !spr_after) { free(snap); snap = NULL; base = NULL; }
    }
    if (!elf || !native || (!ext && !ov_native) || !base) {
        r->status = -1;
        snprintf(r->msg, sizeof r->msg, "%08X %s: %s", addr, name ? name : "?",
                 !elf ? "oracle not initialised" : !native ? "no native thunk" : !ext ? "no extent" : "no EE fiber");
        return r->status;
    }
    memset(&cn, 0, sizeof cn);
    cn.r[29] = (uint32_t)(uintptr_t)(oracle_stack + sizeof oracle_stack - 64);   /* private: outside the compared ranges */
    for (int i = 0; i < ni && i < 8; i++) cn.r[4 + i] = iargs[i];
    for (int i = 8; i < ni && i < 16; i++)      /* the 9th argument on: the caller's $sp + 0, + 8, ... */
        memcpy(oracle_stack + sizeof oracle_stack - 64 + 8 * (i - 8), &iargs[i], 8);
    for (int i = 0; i < nf && i < 8; i++) cn.f[12 + i].f = fargs[i];
    ci = cn;

    if (deep && !native_set) build_native_set();
    check_mxcsr = _mm_getcsr();
    save(snap, spr_snap);
    rc_oracle_checks++;
    rc_oracle_busy = 1;
    missing_target = hle_called = 0;
    int why = setjmp(abort_jb);
    if (why) rc_oracle_rearm();
    if (why == 2) {                             /* the interpreter called something with no native thunk */
        rc_oracle_busy = 0;
        deep_active = 0;
        load(snap, spr_snap);
        r->status = 3;
        snprintf(r->msg, sizeof r->msg, "%08X %s: calls %08X %s, which has no native thunk; not checked", addr, name,
                 missing_target, rc_oracle_name(missing_target) ? rc_oracle_name(missing_target) : "?");
        return r->status;
    }
    if (why) {
        rc_oracle_busy = 0;
        deep_active = 0;
        load(snap, spr_snap);                   /* nothing of the check stays; the real call runs from here */
        if (native_set) {
            uint32_t k = (addr - 0x00100000u) / 4;
            if (addr >= 0x00100000u && addr < 0x00300000u) native_set[k / 8] |= (uint8_t)(1u << (k % 8));
        }
        r->status = 3;
        snprintf(r->msg, sizeof r->msg, "%08X %s: would block or yield, not checked", addr, name);
        return r->status;
    }
    native(&cn);
    save(after, spr_after);
    load(snap, spr_snap);

    deep_active = deep;
    st = ov_native ? rc_interp(&ci, addr, ov_lo, ov_hi, 50000000, &r->steps)
                   : rc_interp(&ci, addr, deep ? 0x00100000u : addr, deep ? 0x00300000u : addr + ext->size, 50000000,
                               &r->steps);
    deep_active = 0;
    rc_oracle_busy = 0;

    r->v0_native = cn.r[2]; r->v0_interp = ci.r[2];
    r->f0_native = cn.f[0].u; r->f0_interp = ci.f[0].u;
    if (st != RC_INTERP_DONE) {
        r->status = 2;
        snprintf(r->msg, sizeof r->msg, "%08X %s: the interpreter stopped (%s) after %llu steps", addr, name,
                 st == RC_INTERP_BUDGET ? "budget" : "unsupported instruction", (unsigned long long)r->steps);
    } else {
        const uint8_t *now = (const uint8_t *)(uintptr_t)RAM_LO;
        for (uint32_t o = 0; o < RAM_HI - RAM_LO; o++)
            if (now[o] != after[o]) {
                if (r->nbytes < 8) { r->first[r->nbytes] = RAM_LO + o; r->native_b[r->nbytes] = after[o]; r->interp_b[r->nbytes] = now[o]; }
                r->nbytes++;
            }
        for (uintptr_t a = xs_lo; xs_hi > xs_lo && a < xs_hi; a++)
            if (*(const uint8_t *)a != xs_after[a - xs_lo]) {
                if (r->nbytes < 8) { r->first[r->nbytes] = (uint32_t)a; r->native_b[r->nbytes] = xs_after[a - xs_lo]; r->interp_b[r->nbytes] = *(const uint8_t *)a; }
                r->nbytes++;
            }
        for (uint32_t o = 0; o < SPR_SZ; o++)
            if (((const uint8_t *)(uintptr_t)SPR)[o] != spr_after[o]) {
                if (r->nbytes < 8) { r->first[r->nbytes] = SPR + o; r->native_b[r->nbytes] = spr_after[o]; r->interp_b[r->nbytes] = ((const uint8_t *)(uintptr_t)SPR)[o]; }
                r->nbytes++;
            }
        int ret = ov_native ? 5 : rc_main_ret(addr), rdiff;
        rdiff = ret == 5 ? (cn.r[2] != ci.r[2] || cn.r[3] != ci.r[3] || cn.f[0].u != ci.f[0].u)
              : ret == 1 ? (uint32_t)r->v0_native != (uint32_t)r->v0_interp
              : ret == 2 ? r->f0_native != r->f0_interp
              : ret == 3 ? r->v0_native != r->v0_interp : 0;
        r->status = r->nbytes || rdiff ? 1 : 0;
        if (r->status) {
            if (hle_called) r->status = 4;      /* differs, but a runtime-provided callee ran twice (heap, semaphores) */
            int n = ret == 2
                ? snprintf(r->msg, sizeof r->msg, "%08X %s DIFFERS: f0 native %08X interp %08X; %u bytes differ",
                           addr, name, r->f0_native, r->f0_interp, r->nbytes)
                : ret == 3
                ? snprintf(r->msg, sizeof r->msg, "%08X %s DIFFERS: v0 native %016llX interp %016llX; %u bytes differ",
                           addr, name, (unsigned long long)r->v0_native, (unsigned long long)r->v0_interp, r->nbytes)
                : snprintf(r->msg, sizeof r->msg, "%08X %s DIFFERS: v0 native %08X interp %08X; %u bytes differ",
                           addr, name, (uint32_t)r->v0_native, (uint32_t)r->v0_interp, r->nbytes);
            if (hle_called && n < (int)sizeof r->msg - 60)
                n += snprintf(r->msg + n, sizeof r->msg - n, " [calls runtime-provided %s]",
                              rc_oracle_name(hle_called) ? rc_oracle_name(hle_called) : "?");
            for (uint32_t k = 0; k < r->nbytes && k < 8 && n < (int)sizeof r->msg - 40; k++)
                n += snprintf(r->msg + n, sizeof r->msg - n, "%s %08X n%02X/i%02X", k ? "," : ":", r->first[k],
                              r->native_b[k], r->interp_b[k]);
        } else {
            snprintf(r->msg, sizeof r->msg, "%08X %s same (%s %08X, %llu steps)", addr, name, ret == 2 ? "f0" : "v0",
                     ret == 2 ? r->f0_native : (uint32_t)r->v0_native, (unsigned long long)r->steps);
        }
    }
    if (restore_pre) load(snap, spr_snap);      /* the hook: the real call runs next, from the same state */
    else load(after, spr_after);                /* the game goes on from the native result */
    return r->status;
}

/* A module calling into the middle of a main-program function (3 call sites: the native port has no entry there):
 * run the original EE code from that address on the interpreter, on the module's own state; calls out of the
 * containing function go to the native callees. Needs rc_oracle_init done at boot. */
void rc_interp_main(RcCpu *c, uint32_t addr)
{
    unsigned lo = 0, hi = rc_nmain_extents;
    const RcMainFunc *f = NULL;
    uint64_t used;
    int st;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (rc_main_extents[mid].addr <= addr) lo = mid + 1;
        else hi = mid;
    }
    if (lo && addr - rc_main_extents[lo - 1].addr < rc_main_extents[lo - 1].size) f = &rc_main_extents[lo - 1];
    if (!elf || !f) { rc_fatal(rc_current_module(), (uint32_t)c->r[31] - 8, addr); return; }
    st = rc_interp(c, addr, f->addr, f->addr + f->size, 100000000, &used);
    if (st != RC_INTERP_DONE) rc_fatal(rc_current_module(), (uint32_t)c->r[31] - 8, addr);
}

/* The widened oracle: an oracle build of the port calls this at the entry of every native main-program function
 * with its own arguments (generator hook, lane P). The first few calls of each function (DOTR_ORACLE_CALLS, default
 * 3) are checked from that exact state; memory is then put back, and the real call goes on. Results go to
 * rc_oracle.log: one line per check (same / DIFFERS / interpreter stopped). */
static uint8_t oracle_counts[(0x00300000u - 0x00100000u) / 4];
static int in_hook, oracle_limit = -1, oracle_deep;
static unsigned oracle_lo = 0, oracle_hi = 0xFFFFFFFFu, oracle_from;
unsigned int hw_vblank_count(void);
static unsigned oracle_same, oracle_diff, oracle_stop;
static FILE *oracle_log;

void rc_oracle_hook(uint32_t addr, int ni, const uint64_t *iargs, int nf, const float *fargs)
{
    RcOracleResult r;
    uint32_t i = (addr - 0x00100000u) / 4;
    if (in_hook || !elf || i >= sizeof oracle_counts) return;
    if (oracle_limit < 0) {
        const char *e = getenv("DOTR_ORACLE_CALLS");
        oracle_limit = e ? atoi(e) : 3;
        oracle_deep = getenv("DOTR_ORACLE_DEEP") != NULL;
        e = getenv("DOTR_ORACLE_FROM");                 /* start checking only from this frame (vblank) */
        if (e) oracle_from = (unsigned)atoi(e);
        e = getenv("DOTR_ORACLE_RANGE");                /* lo-hi in hex: check only functions in [lo, hi) */
        if (e) sscanf(e, "%x-%x", &oracle_lo, &oracle_hi);
        oracle_log = fopen(getenv("DOTR_ORACLE_LOG") ? getenv("DOTR_ORACLE_LOG") : "rc_oracle.log", "w");
    }
    if (oracle_counts[i] >= oracle_limit || addr < oracle_lo || addr >= oracle_hi) return;
    if (oracle_from && hw_vblank_count() < oracle_from) return;
    if (!native_set) build_native_set();
    if (!unsafe_set) build_unsafe_set();
    if (native_set && (native_set[i / 8] >> (i % 8)) & 1) return;   /* replaced by design, or VU0/syscall code */
    if (unsafe_set && (unsafe_set[i / 8] >> (i % 8)) & 1) return;   /* reaches the runtime: cannot be re-run */
    oracle_counts[i]++;
    in_hook = 1;
    restore_pre = 1;
    uintptr_t stack_lo, stack_hi;
    {
        /* EE threads run on fibers whose stacks sit in the loader's fixed area 0x02000000..0x023FFFFF; snapshot from
         * just above the hook's frame to the end of that area (the TIB's StackBase is not kept per fiber) */
        xs_lo = (uintptr_t)__builtin_frame_address(0) + 8;     /* above the hook's saved frame and return address */
        xs_hi = 0x02400000u;
        if (xs_lo < 0x02000000u || xs_hi <= xs_lo || xs_hi - xs_lo > XS_MAX) xs_lo = xs_hi = 0;
        stack_lo = xs_lo; stack_hi = xs_hi;
    }
    rc_oracle_run(addr, iargs, ni, fargs, nf, oracle_deep, &r);
    xs_lo = xs_hi = 0;
    restore_pre = 0;
    in_hook = 0;
    if (r.status == 0) oracle_same++;
    else if (r.status == 1 || r.status == 4) oracle_diff++;
    else oracle_stop++;
    if (oracle_log) {
        fprintf(oracle_log, "%s #%u: %s", r.status == 0 ? "same" : r.status == 1 ? "DIFF" : r.status == 4 ? "diff-hle" : "skip",
                oracle_counts[i], r.msg);
        if (r.status == 1 || r.status == 4) {           /* the call, for reproducing it */
            fprintf(oracle_log, " | args");
            for (int k = 0; k < ni && k < 8; k++) fprintf(oracle_log, " %llX", (unsigned long long)iargs[k]);
            for (int k = 0; k < nf && k < 8; k++) fprintf(oracle_log, " %gf", fargs[k]);
            fprintf(oracle_log, " | x86 stack %08X..%08X | mxcsr %04X", (uint32_t)stack_lo, (uint32_t)stack_hi,
                    check_mxcsr);
        }
        fputc('\n', oracle_log);
        fflush(oracle_log);
    }
}

void rc_oracle_totals(unsigned *same, unsigned *differ, unsigned *skipped)
{
    *same = oracle_same; *differ = oracle_diff; *skipped = oracle_stop;
}

/* ---- deep mode: callees interpreted too, except code the interpreter cannot run (VU0, syscalls, the library
 * instructions it does not know), which is called natively (native_set: 1 bit per word of 0x00100000..0x002FFFFF)
 * ------------------------------------------------------------------------------------------------------------ */

static int insn_supported(uint32_t w)
{
    uint32_t op = w >> 26, rs = (w >> 21) & 31, rt = (w >> 16) & 31, sa = (w >> 6) & 31, fn = w & 63;
    switch (op) {
    case 0x00:
        return fn == 0x00 || fn == 0x02 || fn == 0x03 || fn == 0x04 || fn == 0x06 || fn == 0x07 || fn == 0x08 ||
               fn == 0x09 || fn == 0x0A || fn == 0x0B || fn == 0x0F || fn == 0x10 || fn == 0x12 || fn == 0x14 ||
               fn == 0x16 || fn == 0x18 || fn == 0x19 || fn == 0x1A || fn == 0x1B || fn == 0x20 || fn == 0x21 ||
               fn == 0x23 || fn == 0x24 || fn == 0x25 || fn == 0x26 || fn == 0x27 || fn == 0x2A || fn == 0x2B ||
               fn == 0x2D || fn == 0x2F || fn == 0x38 || fn == 0x3A || fn == 0x3C || fn == 0x3E || fn == 0x3F;
    case 0x01: return rt <= 3;
    case 0x11:
        if (rs == 4) return 1;
        if (rs == 8) return rt <= 3;
        if (rs == 0x14) return fn == 0x20;
        if (rs == 0x10)
            return fn <= 3 || fn == 6 || fn == 7 || fn == 0x18 || fn == 0x19 || fn == 0x1A || fn == 0x1C ||
                   fn == 0x1D || fn == 0x32 || fn == 0x34 || fn == 0x36;
        return 0;
    case 0x1C: return (fn == 0x28 && sa == 0x18) || fn == 0x01 || fn == 0x10 || fn == 0x12 || fn == 0x18 || fn == 0x1A;
    case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x07: case 0x08: case 0x09: case 0x0A: case 0x0B:
    case 0x0C: case 0x0D: case 0x0E: case 0x0F: case 0x10: case 0x14: case 0x15: case 0x16: case 0x17: case 0x19:
    case 0x1E: case 0x1F: case 0x20: case 0x21: case 0x23: case 0x24: case 0x25: case 0x28: case 0x29: case 0x2B:
    case 0x31: case 0x37: case 0x39: case 0x3F:
        return 1;
    default:
        return 0;
    }
}

static void build_native_set(void)
{
    native_set = (uint8_t *)calloc(1, (0x00300000u - 0x00100000u) / 4 / 8);
    if (!native_set) return;
    hle_set = (uint8_t *)calloc(1, (0x00300000u - 0x00100000u) / 4 / 8);
    for (unsigned i = 0; i < rc_nnative_only; i++) {       /* replaced or stubbed in the port by design */
        uint32_t a = rc_native_only[i], k = (a - 0x00100000u) / 4;
        if (a < 0x00100000u || a >= 0x00300000u) continue;
        native_set[k / 8] |= (uint8_t)(1u << (k % 8));
        if (hle_set) hle_set[k / 8] |= (uint8_t)(1u << (k % 8));
    }
    for (unsigned i = 0; i < rc_nmain_extents; i++) {
        const RcMainFunc *f = &rc_main_extents[i];
        if (f->addr < 0x00100000u || f->addr >= 0x00300000u) continue;
        for (uint32_t a = f->addr; a < f->addr + f->size; a += 4)
            if (!insn_supported(rc_oracle_fetch(a))) {
                uint32_t k = (f->addr - 0x00100000u) / 4;
                native_set[k / 8] |= (uint8_t)(1u << (k % 8));
                break;
            }
    }
}

/* (unsafe_set) functions whose call tree reaches anything the runtime provides or replaces (files, semaphores, heap,
 * graphics, sound, hardware) or makes an indirect call: re-running them would repeat effects outside PS2 memory,
 * so the hook never checks them. Computed from the original code's jal targets, to a fixed point. */
#define BIT(set, a) (((set)[(((a) - 0x00100000u) / 4) / 8] >> ((((a) - 0x00100000u) / 4) % 8)) & 1)
#define SETBIT(set, a) ((set)[(((a) - 0x00100000u) / 4) / 8] |= (uint8_t)(1u << ((((a) - 0x00100000u) / 4) % 8)))
static void build_unsafe_set(void)
{
    int changed = 1;
    unsafe_set = (uint8_t *)calloc(1, (0x00300000u - 0x00100000u) / 4 / 8);
    if (!unsafe_set || !hle_set) return;
    for (unsigned i = 0; i < rc_nmain_extents; i++) {
        const RcMainFunc *f = &rc_main_extents[i];
        if (f->addr < 0x00100000u || f->addr >= 0x00300000u) continue;
        if (BIT(hle_set, f->addr)) { SETBIT(unsafe_set, f->addr); continue; }
        for (uint32_t a = f->addr; a < f->addr + f->size; a += 4) {
            uint32_t w = rc_oracle_fetch(a);
            if ((w >> 26) == 0 && (w & 63) == 9) { SETBIT(unsafe_set, f->addr); break; }      /* jalr */
            if ((w >> 26) == 0 && (w & 63) == 8 && ((w >> 21) & 31) != 31) {                  /* jr not $ra: */
                uint32_t t = 0;                                                                /* switch or tail */
                (void)t;
            }
        }
    }
    while (changed) {
        changed = 0;
        for (unsigned i = 0; i < rc_nmain_extents; i++) {
            const RcMainFunc *f = &rc_main_extents[i];
            if (f->addr < 0x00100000u || f->addr >= 0x00300000u || BIT(unsafe_set, f->addr)) continue;
            for (uint32_t a = f->addr; a < f->addr + f->size; a += 4) {
                uint32_t w = rc_oracle_fetch(a), t;
                if ((w >> 26) != 3 && (w >> 26) != 2) continue;                                /* jal / j */
                t = (a & 0xF0000000u) | ((w & 0x3FFFFFFu) << 2);
                if (t >= f->addr && t < f->addr + f->size) continue;
                if (t < 0x00100000u || t >= 0x00300000u || BIT(unsafe_set, t)) {
                    SETBIT(unsafe_set, f->addr);
                    changed = 1;
                    break;
                }
            }
        }
    }
}

int rc_oracle_external(uint32_t pc)
{
    uint32_t k = (pc - 0x00100000u) / 4;
    if (!deep_active || !native_set || pc < 0x00100000u || pc >= 0x00300000u) return 0;
    return (native_set[k / 8] >> (k % 8)) & 1 && rc_main_func(pc) != NULL;
}

#include "rc_oracle_mod.h"
#include "rc_oracle_rr.h"
