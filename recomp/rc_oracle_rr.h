/*
 * rc_oracle_rr.h (included by rc_oracle.c) - the module oracle with record/replay at the module->main boundary.
 *
 * The real call runs the translated module function as always; every call it makes into the main program is
 * recorded: its arguments, its results and the bytes of PS2
 * RAM / scratchpad it changed. Memory is then put back and the module's ORIGINAL code runs on the interpreter: its
 * calls into unsafe functions are not executed but matched against the recording (address and arguments must be
 * the same, in the same order), the recorded memory writes are applied and the recorded results returned. Finally v0/v1/f0 and every byte are compared, and memory is set back to
 * the real call's result. So each runtime effect happens exactly once. A check during which a vblank happened (the
 * real call yielded) is dropped.
 */
#include "hw.h"
int rc_main_nargs(uint32_t addr);

int rc_rr_on;
enum { RR_OFF, RR_RECORD, RR_REPLAY };
static int rr_mode, rr_depth, rr_bad;
typedef struct RrCall {
    uint32_t addr, a[8], f[8];
    uint64_t v0, v1;
    uint32_t f0, mem_off, mem_len;
} RrCall;
#define RR_MAXCALLS 4096
#define RR_MEMBUF (32u << 20)
static RrCall rr_calls[RR_MAXCALLS];
static unsigned rr_n, rr_i;
static uint8_t *rr_mem, *rr_csnap, *rr_cspr;  /* recorded writes: runs of [u32 addr][u32 len][bytes] */
static uint32_t rr_mem_used;
static char rr_why[200];

/* Every module->main call is recorded and replayed: main-program code can keep state outside PS2 memory (the VU0
 * and matrix-stack emulation, the runtime), so running it a second time for the interpreter would not see the same
 * world. The interpreter pass then executes module code only, which is what this oracle checks. */
static int rr_unsafe(uint32_t addr)
{
    (void)addr;
    return 1;
}

static void rr_put(const void *p, uint32_t n)
{
    if (rr_mem_used + n > RR_MEMBUF) { rr_bad = 1; return; }
    memcpy(rr_mem + rr_mem_used, p, n);
    rr_mem_used += n;
}

/* append the runs where `now` differs from `was` (size n, PS2 address base) */
static void rr_diff(const uint8_t *was, const uint8_t *now, uint32_t n, uint32_t base)
{
    for (uint32_t blk = 0; blk < n && !rr_bad; blk += 4096) {
        uint32_t end = blk + 4096 > n ? n : blk + 4096;
        if (!memcmp(was + blk, now + blk, end - blk)) continue;
        for (uint32_t o = blk; o < end;) {
            uint32_t s, a, len;
            if (was[o] == now[o]) { o++; continue; }
            s = o;
            while (o < end && was[o] != now[o]) o++;
            a = base + s;
            len = o - s;
            rr_put(&a, 4);
            rr_put(&len, 4);
            rr_put(now + s, len);
        }
    }
}

int rc_rr_pre(RcCpu *c, uint32_t addr)
{
    if (rr_mode == RR_RECORD) {
        if (rr_depth) { rr_depth++; return 0; }         /* nested (a module callback): runs normally */
        if (!rr_unsafe(addr)) return 0;
        if (rr_n >= RR_MAXCALLS) { rr_bad = 1; return 0; }
        RrCall *k = &rr_calls[rr_n];
        k->addr = addr;
        for (int i = 0; i < 8; i++) k->a[i] = (uint32_t)c->r[4 + i];
        for (int i = 0; i < 8; i++) k->f[i] = c->f[12 + i].u;
        memcpy(rr_csnap, (void *)(uintptr_t)RAM_LO, RAM_HI - RAM_LO);
        memcpy(rr_cspr, (void *)(uintptr_t)SPR, SPR_SZ);
        rr_depth = 1;
        return 0;
    }
    if (rr_mode == RR_REPLAY) {
        if (!rr_unsafe(addr)) return 0;
        if (rr_i >= rr_n) {
            if (!rr_bad) snprintf(rr_why, sizeof rr_why, "the original makes an extra call %08X %s", addr,
                                  rc_oracle_name(addr) ? rc_oracle_name(addr) : "?");
            rr_bad = 2;
            c->r[2] = 0;
            return 1;
        }
        RrCall *k = &rr_calls[rr_i++];
        /* only the arguments the callee declares: a module may leave garbage in the other registers */
        int na = rc_main_nargs(addr), ni = na < 0 ? 4 : na & 15, nf = na < 0 ? 0 : na >> 4;
        int same = k->addr == addr;
        for (int i = 0; i < ni && i < 8 && same; i++) same = k->a[i] == (uint32_t)c->r[4 + i];
        for (int i = 0; i < nf && i < 8 && same; i++) same = k->f[i] == c->f[12 + i].u;
        if (!same && !rr_bad) {
            snprintf(rr_why, sizeof rr_why, "call #%u: translation %08X(%X,%X,%X,%X), original %08X(%X,%X,%X,%X)",
                     rr_i, k->addr, k->a[0], k->a[1], k->a[2], k->a[3], addr, (uint32_t)c->r[4],
                     (uint32_t)c->r[5], (uint32_t)c->r[6], (uint32_t)c->r[7]);
            rr_bad = 2;
        }
        for (uint32_t o = k->mem_off; o < k->mem_off + k->mem_len;) {   /* the call's writes */
            uint32_t a, len;
            memcpy(&a, rr_mem + o, 4);
            memcpy(&len, rr_mem + o + 4, 4);
            memcpy((void *)(uintptr_t)a, rr_mem + o + 8, len);
            o += 8 + len;
        }
        c->r[2] = k->v0;
        c->r[3] = k->v1;
        c->f[0].u = k->f0;
        return 1;
    }
    return 0;
}

void rc_rr_post(RcCpu *c, uint32_t addr)
{
    if (rr_mode != RR_RECORD || !rr_depth) return;
    if (rr_depth > 1) { rr_depth--; return; }
    RrCall *k = &rr_calls[rr_n];
    if (k->addr != addr) return;
    k->v0 = c->r[2];
    k->v1 = c->r[3];
    k->f0 = c->f[0].u;
    k->mem_off = rr_mem_used;
    rr_diff(rr_csnap, (const uint8_t *)(uintptr_t)RAM_LO, RAM_HI - RAM_LO, RAM_LO);
    rr_diff(rr_cspr, (const uint8_t *)(uintptr_t)SPR, SPR_SZ, SPR);
    k->mem_len = rr_mem_used - k->mem_off;
    rr_n++;
    rc_oracle_checks++;                         /* progress for the runtime's stuck watchdog: recording is slow */
    rr_depth = 0;
}

/* called by rc_enter: 1 = it made the real call itself (with the check around it), 0 = rc_enter makes it */
int rc_oracle_module_call(const RcModule *m, const RcFunc *f, RcCpu *c, const uint32_t *args)
{
    static RcCpu cpre, ci;
    static RcOracleResult r;
    uint64_t steps = 0;
    uint32_t k, vb;
    int st = -1, why;
    if (!mod_oracle_on() || m != mod_cur || !mod_img_ok || in_hook || rc_oracle_busy || rr_mode) return 0;
    k = (uint32_t)(f - m->funcs);
    if (oracle_limit < 0) {
        const char *e = getenv("DOTR_ORACLE_CALLS");
        oracle_limit = e ? atoi(e) : 3;
        oracle_deep = getenv("DOTR_ORACLE_DEEP") != NULL;
        oracle_log = fopen(getenv("DOTR_ORACLE_LOG") ? getenv("DOTR_ORACLE_LOG") : "rc_oracle.log", "w");
    }
    if (mod_counts[k] >= oracle_limit) return 0;
    if (!snap) {
        snap = (uint8_t *)malloc(RAM_HI - RAM_LO);
        after = (uint8_t *)malloc(RAM_HI - RAM_LO);
        spr_snap = (uint8_t *)malloc(SPR_SZ);
        spr_after = (uint8_t *)malloc(SPR_SZ);
    }
    if (!rr_mem) {
        rr_mem = (uint8_t *)malloc(RR_MEMBUF);
        rr_csnap = (uint8_t *)malloc(RAM_HI - RAM_LO);
        rr_cspr = (uint8_t *)malloc(SPR_SZ);
    }
    if (!snap || !after || !spr_snap || !spr_after || !rr_mem || !rr_csnap || !rr_cspr) return 0;
    mod_counts[k]++;
    for (int i = 0; i < 8; i++) c->r[4 + i] = (uint64_t)(int64_t)(int32_t)args[i];
    cpre = *c;
    xs_lo = xs_hi = 0;
    save(snap, spr_snap);
    vb = hw_vblank_count();

    /* 1: the real call, recording its calls into unsafe main-program functions */
    in_hook = 1;
    rr_mode = RR_RECORD;
    rr_n = rr_i = rr_mem_used = 0;
    rr_bad = rr_depth = 0;
    rr_why[0] = 0;
    rc_rr_on = 1;
    f->fn(c);
    rc_rr_on = 0;
    rr_mode = RR_OFF;
    memset(&r, 0, sizeof r);
    if (hw_vblank_count() != vb || rr_bad) {
        in_hook = 0;
        mod_skip++;
        if (oracle_log) {
            fprintf(oracle_log, "mod-skip #%u: %s_%08X: %s\n", mod_counts[k], m->name, f->addr,
                    rr_bad ? "recording overflowed" : "the real call yielded (a vblank ran)");
            fflush(oracle_log);
        }
        return 1;
    }

    /* 2: memory back, the original code on the interpreter, replaying the recorded calls */
    save(after, spr_after);
    load(snap, spr_snap);
    ci = cpre;
    rr_mode = RR_REPLAY;
    rc_rr_on = 1;
    rc_oracle_busy = 1;
    rc_oracle_checks++;
    missing_target = 0;
    why = setjmp(abort_jb);
    if (why) rc_oracle_rearm();
    if (!why) st = rc_interp(&ci, f->addr, 0x01E00000u, 0x01E08000u, 50000000, &steps);
    rc_oracle_busy = 0;
    rc_rr_on = 0;
    rr_mode = RR_OFF;

    /* 3: compare with the real call, then leave memory as the real call left it */
    if (!why && st == RC_INTERP_DONE) {
        const uint8_t *now = (const uint8_t *)(uintptr_t)RAM_LO;
        for (uint32_t o = 0; o < RAM_HI - RAM_LO; o++)
            if (now[o] != after[o]) {
                if (r.nbytes < 8) { r.first[r.nbytes] = RAM_LO + o; r.native_b[r.nbytes] = after[o]; r.interp_b[r.nbytes] = now[o]; }
                r.nbytes++;
            }
        for (uint32_t o = 0; o < SPR_SZ; o++)
            if (((const uint8_t *)(uintptr_t)SPR)[o] != spr_after[o]) {
                if (r.nbytes < 8) { r.first[r.nbytes] = SPR + o; r.native_b[r.nbytes] = spr_after[o]; r.interp_b[r.nbytes] = ((const uint8_t *)(uintptr_t)SPR)[o]; }
                r.nbytes++;
            }
    }
    load(after, spr_after);
    in_hook = 0;
    if (why || st != RC_INTERP_DONE) {
        mod_skip++;
        snprintf(r.msg, sizeof r.msg, "%s_%08X: %s", m->name, f->addr,
                 why == 2 ? "the original calls something with no native thunk" : why ? "the original would block"
                 : st == RC_INTERP_BUDGET ? "interpreter budget" : "unsupported instruction");
        r.status = 3;
    } else if (rr_bad || rr_i != rr_n || r.nbytes || ci.r[2] != c->r[2] || ci.r[3] != c->r[3] || ci.f[0].u != c->f[0].u) {
        int n;
        mod_diff++;
        r.status = 1;
        n = snprintf(r.msg, sizeof r.msg, "%s_%08X DIFFERS: v0 trans %08X orig %08X; %u bytes; calls %u recorded, %u replayed%s%s",
                     m->name, f->addr, (uint32_t)c->r[2], (uint32_t)ci.r[2], r.nbytes, rr_n, rr_i,
                     rr_why[0] ? "; " : "", rr_why);
        for (uint32_t q = 0; q < r.nbytes && q < 8 && n < (int)sizeof r.msg - 40; q++)
            n += snprintf(r.msg + n, sizeof r.msg - n, "%s %08X t%02X/o%02X", q ? "," : ":", r.first[q], r.native_b[q],
                          r.interp_b[q]);
    } else {
        mod_same++;
        snprintf(r.msg, sizeof r.msg, "%s_%08X same (v0 %08X, %u calls replayed, %llu steps)", m->name, f->addr,
                 (uint32_t)c->r[2], rr_n, (unsigned long long)steps);
    }
    if (oracle_log) {
        fprintf(oracle_log, "%s #%u: %s", r.status == 0 ? "mod-same" : r.status == 1 ? "MOD-DIFF" : "mod-skip",
                mod_counts[k], r.msg);
        if (r.status == 1) fprintf(oracle_log, " | args %X %X %X %X", args[0], args[1], args[2], args[3]);
        fputc('\n', oracle_log);
        fflush(oracle_log);
    }
    return 1;
}
