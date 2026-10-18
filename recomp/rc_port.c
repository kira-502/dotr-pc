/*
 * rc_port.c - lane R's side of rc_port.h: overlay switch, entry into translated code, indirect-call dispatch.
 * The tables come from generated files: rc_modules.c (rcgen.py all), rc_entries.c and rc_exits.c (rcthunks.py).
 */
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include "rc_port.h"

void rc_oracle_note_call(uint32_t addr, int known);
void ee_float_add_code(uintptr_t lo, uintptr_t hi);     /* runtime/eefloat.c */
void rc_oracle_module_switch(const RcModule *m);
int rc_oracle_module_call(const RcModule *m, const RcFunc *f, RcCpu *c, const uint32_t *args);

#define MOD_LO 0x01E00000u
#define MOD_HI 0x01E08000u

extern const RcModule *const rc_modules[];
extern const unsigned rc_nmodules;

typedef struct RcEntry { uint32_t addr; void *stub; } RcEntry;
extern const RcEntry rc_entry_stubs[];          /* sorted by addr; one per address that starts a function anywhere */
extern const unsigned rc_nentry_stubs;
extern const RcFunc rc_main_funcs[];            /* sorted by addr; rcx_ thunk of every main function with a prototype */
extern const unsigned rc_nmain_funcs;

static const RcModule *cur;

const char *rc_current_module(void) { return cur ? cur->name : NULL; }

static const RcFunc *find_func(const RcFunc *t, unsigned n, uint32_t addr)
{
    unsigned lo = 0, hi = n;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (t[mid].addr < addr) lo = mid + 1;
        else hi = mid;
    }
    return lo < n && t[lo].addr == addr ? &t[lo] : NULL;
}

static void *find_stub(uint32_t addr)
{
    unsigned lo = 0, hi = rc_nentry_stubs;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (rc_entry_stubs[mid].addr < addr) lo = mid + 1;
        else hi = mid;
    }
    return lo < rc_nentry_stubs && rc_entry_stubs[lo].addr == addr ? rc_entry_stubs[lo].stub : NULL;
}

int rc_overlay_switch(void)
{
    char name[32];
    const char *hdr = (const char *)(uintptr_t)(MOD_LO + 0x20);
    unsigned i, n;
    for (n = 0; n < sizeof name - 1 && hdr[n] && hdr[n] != '.'; n++) name[n] = hdr[n];
    name[n] = 0;
    cur = NULL;
    for (i = 0; i < rc_nmodules; i++)
        if (!strcmp(rc_modules[i]->name, name)) { cur = rc_modules[i]; break; }
    if (!cur) { rc_fatal(name, 0, MOD_LO); return 0; }
    {
        /* once: the translated module code goes under the runtime's EE float rule (x/0 gives
         * +-FLT_MAX, never inf/NaN), like the native game code */
        static int ee_done;
        if (!ee_done) {
            uintptr_t lo = (uintptr_t)-1, hi = 0;
            unsigned j, q;
            for (j = 0; j < rc_nmodules; j++)
                for (q = 0; q < rc_modules[j]->nfuncs; q++) {
                    uintptr_t a = (uintptr_t)rc_modules[j]->funcs[q].fn;
                    if (a < lo) lo = a;
                    if (a > hi) hi = a;
                }
            ee_float_add_code(lo, hi + 0x10000);                    /* + the last function's body */
            /* (the interpreter applies the same rule itself: rc_interp_port.c, RC_INTERP_EE_FLOAT) */
            ee_done = 1;
        }
    }
    for (i = 0; i < cur->nfuncs; i++) {
        uint32_t a = cur->funcs[i].addr;
        uint32_t room = i + 1 < cur->nfuncs ? cur->funcs[i + 1].addr - a : 8;
        void *stub = find_stub(a);
        uint8_t *at = (uint8_t *)(uintptr_t)a;
        int32_t rel;
        if (room < 5 || !stub) continue;               /* rcthunks.py reports any function this would skip */
        rel = (int32_t)((uintptr_t)stub - ((uintptr_t)at + 5));
        at[0] = 0xE9;
        memcpy(at + 1, &rel, 4);
    }
    rc_oracle_module_switch(cur);               /* module oracle (DOTR_RC_ORACLE=1): the original bytes, from the disc */
    return 1;
}

/* ---- per-fiber state ---------------------------------------------------------------------------------------- */

static DWORD fls = FLS_OUT_OF_INDEXES;

void rc_thread_init(uint32_t ps2_stack_top)
{
    RcCpu *c;
    if (fls == FLS_OUT_OF_INDEXES) fls = FlsAlloc(NULL);
    if (FlsGetValue(fls)) return;
    c = (RcCpu *)calloc(1, sizeof *c);
    if (!c) { rc_fatal("rc_thread_init", 0, ps2_stack_top); return; }
    c->r[29] = ps2_stack_top;
    FlsSetValue(fls, c);
}

RcCpu *rc_cpu_current(void) { return fls == FLS_OUT_OF_INDEXES ? NULL : (RcCpu *)FlsGetValue(fls); }

const RcFunc *rc_main_func(uint32_t addr) { return find_func(rc_main_funcs, rc_nmain_funcs, addr); }

extern const unsigned char rc_main_rets[], rc_main_args[];
int rc_main_nargs(uint32_t addr)             /* integers | floats << 4, or -1 */
{
    const RcFunc *f = find_func(rc_main_funcs, rc_nmain_funcs, addr);
    return f ? rc_main_args[f - rc_main_funcs] : -1;
}
int rc_main_ret(uint32_t addr)
{
    const RcFunc *f = find_func(rc_main_funcs, rc_nmain_funcs, addr);
    return f ? rc_main_rets[f - rc_main_funcs] : -1;
}

static RcCpu *rc_cpu(void)
{
    RcCpu *c = fls == FLS_OUT_OF_INDEXES ? NULL : (RcCpu *)FlsGetValue(fls);
    if (!c) rc_fatal("rc_thread_init not called on this fiber", 0, 0);
    return c;
}

/* Entry from native code (called by the generated stubs rc_e_<addr>). */
unsigned rc_stat_entries, rc_stat_dispatch;     /* counts for rc_check.c */

uint64_t rc_enter(uint32_t addr, const uint32_t *args)
{
    RcCpu *c = rc_cpu();
    rc_stat_entries++;
    const RcFunc *f = cur ? find_func(cur->funcs, cur->nfuncs, addr) : NULL;
    int i;
    if (!f) { rc_fatal(rc_current_module(), 0, addr); return 0; }
    if (rc_oracle_module_call(cur, f, c, args)) return c->r[2];   /* module oracle: made the call, checked */
    for (i = 0; i < 8; i++) c->r[4 + i] = (uint64_t)(int64_t)(int32_t)args[i];
    f->fn(c);
    return c->r[2];
}

void rc_dispatch(RcCpu *c, uint32_t addr)
{
    const RcFunc *f;
    rc_stat_dispatch++;
    if (addr >= MOD_LO && addr < MOD_HI)
        f = cur ? find_func(cur->funcs, cur->nfuncs, addr) : NULL;
    else
        f = find_func(rc_main_funcs, rc_nmain_funcs, addr);
    if (rc_oracle_busy) rc_oracle_note_call(addr, f != NULL);   /* inside an oracle check: classify, never stop */
    if (!f) { rc_fatal(rc_current_module(), (uint32_t)c->r[31] - 8, addr); return; }
    f->fn(c);
}

void rc_call_ext(RcCpu *c, uint32_t addr) { rc_dispatch(c, addr); }

void rc_bad_jump(RcCpu *c, uint32_t from, uint32_t to) { (void)c; rc_fatal(rc_current_module(), from, to); }
