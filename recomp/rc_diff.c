/*
 * rc_diff.c - R3 check: every translated module function against the reference interpreter.
 *
 * For each module: its code + data image (rcgen.py images) goes into the arena at 0x01E00000; each function runs
 * from the same random states on the interpreter and as translated C; registers, FPU, memory and the sequence of
 * main-program calls must be equal. Runs that hit the budget in the interpreter (random loop counts) are skipped.
 *
 * usage: rc_diff <image dir> [states-per-function] [module | first last]   (windowless; exit 0 = no difference)
 *        first/last = module index range (0..761), to split the run over several processes.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rc_interp.h"

uint8_t *rc_test_arena;
uint64_t rc_test_ticks, rc_test_budget;
jmp_buf rc_test_jmp;

extern const RcModule *const rc_modules[];
extern const unsigned rc_nmodules;

#define BASE 0x01E00000u

#ifdef RC_DIFF_TRACE
/* debugging build: -DRC_DIFF_TRACE -DRC_TRACE(pc)=rc_tr(1,pc) -DRC_INTERP_TRACE(pc)=rc_tr(0,pc), RCGEN_TRACE=1 */
static uint32_t trbuf[2][200000];
static unsigned trn[2];
void rc_tr(int w, uint32_t pc) { if (trn[w] < 200000) trbuf[w][trn[w]++] = pc; }
void rc_trc_entry(int w);
void rc_trc_dump(void);
static void tr_report(void)
{
    rc_trc_dump();
    unsigned i = 0, j = 0;
    /* the translation traces a branch and its delay slot together; skip a delay slot the interpreter nullified */
    while (i < trn[0] && j < trn[1]) {
        if (trbuf[0][i] == trbuf[1][j]) { i++; j++; continue; }
        if (j + 1 < trn[1] && trbuf[0][i] == trbuf[1][j + 1]) { j++; continue; }
        break;
    }
    printf("   trace: %u / %u steps, first difference at interp #%u %08X, trans #%u %08X\n", trn[0], trn[1], i,
           i < trn[0] ? trbuf[0][i] : 0, j, j < trn[1] ? trbuf[1][j] : 0);
    for (unsigned k = i > 12 ? i - 12 : 0; k < i + 4 && k < trn[0]; k++) printf("     i %08X\n", trbuf[0][k]);
    for (unsigned k = j > 12 ? j - 12 : 0; k < j + 4 && k < trn[1]; k++) printf("     t %08X\n", trbuf[1][k]);
}
#define TR_RESET(w) (trn[w] = 0, rc_trc_entry(w))
#else
#define TR_RESET(w) ((void)0)
#define tr_report() ((void)0)
#endif
#define BUDGET 20000

static uint8_t *image;
static uint32_t image_len;
static const RcModule *cur;
static uint64_t calllog;
static int bad_jump;

uint32_t rc_test_fetch(uint32_t pc)
{
    uint32_t o = pc - BASE, w = 0;
    if (o + 4 <= image_len) memcpy(&w, image + o, 4);
    return w;
}

static uint64_t mix(uint64_t h, uint64_t v)
{
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return h * 0xFF51AFD7ED558CCDull;
}

void rc_call_ext(RcCpu *c, uint32_t addr)
{
    uint64_t h = mix(addr, (uint32_t)c->r[4]);
    for (int i = 5; i <= 7; i++) h = mix(h, (uint32_t)c->r[i]);
    h = mix(h, c->f[12].u);
    calllog = mix(calllog, h);
    c->r[2] = h % 7 ? (uint64_t)(h % 5) : (uint64_t)(int64_t)(int32_t)(uint32_t)h;
    c->r[3] = (uint64_t)(int64_t)(int32_t)(uint32_t)(h >> 32);
    c->f[0].f = (float)(int32_t)(h >> 40) / 1024.0f;
}

void rc_dispatch(RcCpu *c, uint32_t addr)
{
    for (uint32_t i = 0; i < cur->nfuncs; i++)
        if (cur->funcs[i].addr == addr) { cur->funcs[i].fn(c); return; }
    if (addr >= BASE && addr < BASE + 0x8000) { bad_jump = 1; return; }
    rc_call_ext(c, addr);
}

void rc_bad_jump(RcCpu *c, uint32_t from, uint32_t to) { (void)c; (void)from; (void)to; bad_jump = 1; }

static uint64_t rs = 0x2545F4914F6CDD1Dull;
static uint64_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }

static uint64_t rnd_gpr(void)
{
    switch (rnd() % 6) {
    case 0: return rnd() % 8;
    case 1: return (uint64_t)(int64_t)(int32_t)(uint32_t)rnd();
    case 2: return 0x01E10000u + (rnd() % 0x4000) * 4;            /* module work memory */
    case 3: return 0x01ED0000u + (rnd() % 0x1000) * 4;
    case 4: return BASE + 0x100 + (rnd() % 0x1000) * 4;           /* the module's own data */
    default: return rnd() % 64;
    }
}

static void rnd_state(RcCpu *c)
{
    memset(c, 0, sizeof *c);
    for (int i = 1; i < 32; i++) { c->r[i] = rnd_gpr(); c->rh[i] = rnd(); }
    c->r[29] = 0x01EF8000u;
    c->hi = rnd(); c->lo = rnd();
    for (int i = 0; i < 32; i++) {
        union { float f; uint32_t u; } v;
        v.f = (float)((int32_t)(rnd() % 4001) - 2000) / 16.0f;
        c->f[i].u = v.u;
    }
    c->acc.f = 1.0f;
}

static int load_image(const char *dir, const char *name)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%s.bin", dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    image_len = (uint32_t)fread(image, 1, 0x40000, f);
    fclose(f);
    return 1;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: rc_diff <image dir> [states] [module]\n"); return 2; }
    int states = argc > 2 ? atoi(argv[2]) : 8;
    const char *only = argc == 4 ? argv[3] : NULL;
    int verbose = getenv("RC_DIFF_VERBOSE") != NULL;
    unsigned first = argc > 4 ? (unsigned)atoi(argv[3]) : 0, last = argc > 4 ? (unsigned)atoi(argv[4]) : ~0u;
    rc_test_arena = malloc(RC_TEST_SPAN);
    uint8_t *base_mem = calloc(1, RC_TEST_SPAN), *mem1 = malloc(RC_TEST_SPAN);
    image = malloc(0x40000);
    if (!rc_test_arena || !base_mem || !mem1 || !image) return 2;
    unsigned long long nf = 0, runs = 0, compared = 0, aborted = 0, wild = 0, fails = 0, ffail = 0;
    for (unsigned m = 0; m < rc_nmodules; m++) {
        cur = rc_modules[m];
        if (only && strcmp(only, cur->name)) continue;
        if (m < first || m > last) continue;
        printf("module %u %s (%u functions)\n", m, cur->name, cur->nfuncs);
        fflush(stdout);
        if (!load_image(argv[1], cur->name)) { printf("no image for %s\n", cur->name); return 2; }
        /* each module's random states depend only on its name, so any run (split, single module) repeats them */
        rs = 0x2545F4914F6CDD1Dull;
        for (const char *p = cur->name; *p; p++) rs = (rs ^ (uint8_t)*p) * 0x100000001B3ull;
        if (!rs) rs = 1;
        for (uint32_t i = 0; i <= RC_TEST_MASK; i++) base_mem[i] = (uint8_t)(rnd() >> 29);
        memcpy(base_mem, image, image_len);
        for (uint32_t k = 0; k < cur->nfuncs; k++) {
            const RcFunc *fn = &cur->funcs[k];
            int bad = 0;
            nf++;
            for (int s = 0; s < states && !bad; s++) {
                RcCpu st, c1, c2;
                rnd_state(&st);
                memcpy(rc_test_arena, base_mem, RC_TEST_SPAN);
                c1 = st; calllog = 0; bad_jump = 0; TR_RESET(0);
                int r1 = rc_interp(&c1, fn->addr, BASE, BASE + 0x8000, BUDGET, NULL);
                uint64_t log1 = calllog;
                int bj1 = bad_jump;
                memcpy(mem1, rc_test_arena, RC_TEST_SPAN);

                memcpy(rc_test_arena, base_mem, RC_TEST_SPAN);
                c2 = st; c2.r[31] = RC_INTERP_SENTINEL; calllog = 0; bad_jump = 0; TR_RESET(1);
                rc_test_ticks = 0; rc_test_budget = 2 * BUDGET;
                int r2 = setjmp(rc_test_jmp) ? 1 : 0;
                if (!r2) fn->fn(&c2);
                runs++;
                if (r1 == RC_INTERP_BUDGET || bj1) { aborted++; continue; }
                /* a random state's pointer used as code: the interpreter runs data, the translation stops */
                if (r1 == RC_INTERP_BAD || bad_jump) { wild++; continue; }
                compared++;
                const char *why = NULL;
                if (r2) why = "translation over budget";
                else if (log1 != calllog) why = "main-program calls differ";
                else if (memcmp(&c1, &c2, sizeof c1)) why = "CPU state differs";
                else if (memcmp(mem1, rc_test_arena, RC_TEST_SPAN)) why = "memory differs";
                if (why) {
                    if (ffail < 40) { printf("FAIL %s %08X (state %d): %s\n", cur->name, fn->addr, s, why); fflush(stdout); }
                    if (verbose) {
                        tr_report();
                        for (int r = 0; r < 32; r++) {
                            if (c1.r[r] != c2.r[r]) printf("   r%-2d interp %016llX  trans %016llX\n", r,
                                (unsigned long long)c1.r[r], (unsigned long long)c2.r[r]);
                            if (c1.rh[r] != c2.rh[r]) printf("   rh%-2d interp %016llX  trans %016llX\n", r,
                                (unsigned long long)c1.rh[r], (unsigned long long)c2.rh[r]);
                            if (c1.f[r].u != c2.f[r].u) printf("   f%-2d interp %08X  trans %08X\n", r, c1.f[r].u, c2.f[r].u);
                        }
                        if (c1.hi != c2.hi || c1.lo != c2.lo || c1.hi1 != c2.hi1 || c1.lo1 != c2.lo1) printf("   hi/lo differ\n");
                        if (c1.acc.u != c2.acc.u || c1.fcc != c2.fcc) printf("   acc/fcc differ\n");
                        for (uint32_t o = 0, shown = 0; o <= RC_TEST_MASK && shown < 8; o++)
                            if (mem1[o] != rc_test_arena[o]) {
                                printf("   mem %05X (PS2 ..%05X) base %02X interp %02X trans %02X\n", o, o, base_mem[o],
                                       mem1[o], rc_test_arena[o]);
                                shown++;
                            }
                        printf("   state: a0 %08X a1 %08X a2 %08X a3 %08X\n", (uint32_t)st.r[4], (uint32_t)st.r[5],
                               (uint32_t)st.r[6], (uint32_t)st.r[7]);
                    }
                    ffail++;
                    bad = 1;
                }
            }
            fails += bad;
        }
    }
    printf("rc_diff: %llu functions, %llu runs, %llu compared, %llu skipped (interpreter over budget), "
           "%llu skipped (wild jump), %llu functions differ\n", nf, runs, compared, aborted, wild, fails);
    return fails ? 1 : 0;
}
