/*
 * rc_unit.c - R1 check: every instruction kind and control-flow pattern, interpreter vs translation.
 *
 * 1. Known answers: a few hand-computed EE results run on the interpreter (so interpreter and translator cannot
 *    share one wrong idea about divide-by-zero, 32-bit sign extension, saturation, lq alignment...).
 * 2. Differential: each generated test (rctest_gen.py) runs from the same random states on the interpreter and as
 *    translated C; registers, hi/lo, FPU, memory and the sequence of main-program calls must all be equal.
 *
 * usage: rc_unit [states-per-test]   (windowless; exit code 0 = pass)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rc_interp.h"

uint8_t *rc_test_arena;
uint64_t rc_test_ticks, rc_test_budget;
jmp_buf rc_test_jmp;

extern const RcUnitTest rc_unit_tests[];
extern const unsigned rc_unit_ntests;

static const uint32_t *code;
static uint32_t ncode;
static const RcFunc *funcs;
static uint32_t nfuncs;
static uint64_t calllog;
static int bad_jump;

#define BASE 0x01E00000u

uint32_t rc_test_fetch(uint32_t pc)
{
    uint32_t i = (pc - BASE) / 4;
    return i < ncode ? code[i] : 0;
}

static uint64_t mix(uint64_t h, uint64_t v)
{
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return h * 0xFF51AFD7ED558CCDull;
}

/* A main-program function, as the tests see it: a deterministic function of its arguments. */
void rc_call_ext(RcCpu *c, uint32_t addr)
{
    uint64_t h = mix(addr, c->r[4]);
    for (int i = 5; i <= 7; i++) h = mix(h, (uint32_t)c->r[i]);
    h = mix(h, c->f[12].u);
    calllog = mix(calllog, h);
    c->r[2] = (uint64_t)(int64_t)(int32_t)(uint32_t)h;
    c->r[3] = (uint64_t)(int64_t)(int32_t)(uint32_t)(h >> 32);
    c->f[0].f = (float)(int32_t)(h >> 40) / 1024.0f;
}

void rc_dispatch(RcCpu *c, uint32_t addr)
{
    for (uint32_t i = 0; i < nfuncs; i++)
        if (funcs[i].addr == addr) { funcs[i].fn(c); return; }
    if (addr >= BASE && addr < BASE + 0x8000) { bad_jump = 1; return; }
    rc_call_ext(c, addr);
}

void rc_bad_jump(RcCpu *c, uint32_t from, uint32_t to) { (void)c; (void)from; (void)to; bad_jump = 1; }

/* ---- random states ------------------------------------------------------------------------------------------ */

static uint64_t rs = 88172645463325252ull;
static uint64_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }

static uint64_t rnd_gpr(void)
{
    switch (rnd() % 7) {
    case 0: return rnd();
    case 1: return (uint64_t)(int64_t)(int32_t)(uint32_t)rnd();
    case 2: return rnd() % 16;
    case 3: return (uint64_t)-(int64_t)(rnd() % 16);
    case 4: return 0x01E10000u + (rnd() % 0x8000) * 4;       /* a pointer into module work memory */
    case 5: return 0x80000000u >> (rnd() % 32);
    default: return (uint64_t)(int64_t)(int32_t)(0x80000000u | (uint32_t)rnd());
    }
}

static uint32_t rnd_fpr(void)
{
    static const float k[] = {0.0f, -0.0f, 1.0f, -1.0f, 0.5f, 3.25f, -1000.0f, 1e30f, -1e-30f, 65536.0f};
    union { float f; uint32_t u; } v;
    switch (rnd() % 4) {
    case 0: v.f = k[rnd() % 10]; return v.u;
    case 1: return (uint32_t)rnd();                           /* any bits: also feeds cvt.s.w */
    case 2: return (uint32_t)(rnd() % 64);
    default: v.f = (float)((int32_t)(rnd() % 20001) - 10000) / 64.0f; return v.u;
    }
}

static void rnd_state(RcCpu *c)
{
    memset(c, 0, sizeof *c);
    for (int i = 1; i < 32; i++) { c->r[i] = rnd_gpr(); c->rh[i] = rnd(); }
    c->r[29] = 0x01EF0000u;                                   /* sp: stack well inside the arena */
    c->hi = rnd_gpr(); c->lo = rnd_gpr(); c->hi1 = rnd_gpr(); c->lo1 = rnd_gpr();
    for (int i = 0; i < 32; i++) c->f[i].u = rnd_fpr();
    c->acc.u = rnd_fpr();
    c->fcc = (uint32_t)(rnd() & 1);
}

/* ---- known answers ------------------------------------------------------------------------------------------ */

static int kat_fail;

static void kat(const char *what, const uint32_t *w, uint32_t n, RcCpu *c, int reg, uint64_t want)
{
    code = w; ncode = n; nfuncs = 0;
    for (uint32_t i = 0; i < n; i++) memcpy(rc_test_arena + i * 4, &w[i], 4);
    int st = rc_interp(c, BASE, BASE, BASE + 4 * n, 1000, NULL);
    uint64_t got = reg >= 100 ? (reg == 100 ? c->lo : reg == 101 ? c->hi : c->rh[reg - 200]) : c->r[reg];
    if (st != RC_INTERP_DONE || got != want) {
        printf("KAT FAIL %s: status %d got %016llX want %016llX\n", what, st, (unsigned long long)got,
               (unsigned long long)want);
        kat_fail++;
    }
}

#define JR_RA 0x03E00008u
static void known_answers(void)
{
    RcCpu c;
    memset(&c, 0, sizeof c);
    /* div $a0, $a1 with a1 = 0: lo = -1 (a0 >= 0), hi = a0 */
    uint32_t div[] = {0x0085001Au, JR_RA, 0};
    c.r[4] = 5; c.r[5] = 0; kat("div 5/0 lo", div, 3, &c, 100, (uint64_t)-1);
    c.r[4] = 5; c.r[5] = 0; kat("div 5/0 hi", div, 3, &c, 101, 5);
    c.r[4] = (uint64_t)-5; c.r[5] = 0; kat("div -5/0 lo", div, 3, &c, 100, 1);
    c.r[4] = 0xFFFFFFFF80000000ull; c.r[5] = (uint64_t)-1; kat("div min/-1 lo", div, 3, &c, 100, 0xFFFFFFFF80000000ull);
    c.r[4] = 0xFFFFFFFF80000000ull; c.r[5] = (uint64_t)-1; kat("div min/-1 hi", div, 3, &c, 101, 0);
    c.r[4] = 7; c.r[5] = (uint64_t)-2; kat("div 7/-2 lo", div, 3, &c, 100, (uint64_t)-3);
    c.r[4] = 7; c.r[5] = (uint64_t)-2; kat("div 7/-2 hi", div, 3, &c, 101, 1);
    /* mult $v0, $a0, $a1 (EE 3-operand): 0x7FFFFFFF * 2 -> lo = sext(0xFFFFFFFE), hi = 0, v0 = lo */
    uint32_t mult[] = {0x00851018u, JR_RA, 0};
    c.r[4] = 0x7FFFFFFF; c.r[5] = 2; kat("mult v0", mult, 3, &c, 2, 0xFFFFFFFFFFFFFFFEull);
    c.r[4] = 0x7FFFFFFF; c.r[5] = 2; kat("mult hi", mult, 3, &c, 101, 0);
    /* multu $a0, $a1: 0xFFFFFFFF^2 = 0xFFFFFFFE_00000001 */
    uint32_t multu[] = {0x00850019u, JR_RA, 0};
    c.r[4] = 0xFFFFFFFF; c.r[5] = 0xFFFFFFFF; kat("multu lo", multu, 3, &c, 100, 1);
    c.r[4] = 0xFFFFFFFF; c.r[5] = 0xFFFFFFFF; kat("multu hi", multu, 3, &c, 101, 0xFFFFFFFFFFFFFFFEull);
    /* addiu $v0, $a0, 1 with a0 = 0x1_00000001: 32-bit add, sign-extended -> 2 */
    uint32_t addiu[] = {0x24820001u, JR_RA, 0};
    c.r[4] = 0x100000001ull; kat("addiu wraps to 32 bits", addiu, 3, &c, 2, 2);
    c.r[4] = 0x7FFFFFFF; kat("addiu overflow sign-extends", addiu, 3, &c, 2, 0xFFFFFFFF80000000ull);
    /* sra $v0, $a0, 4 on 0x80000000 -> 0xF8000000 sign-extended */
    uint32_t sra[] = {0x00041103u, JR_RA, 0};
    c.r[4] = 0x80000000u; kat("sra", sra, 3, &c, 2, 0xFFFFFFFFF8000000ull);
    /* dsra32 $v0, $a0, 0 */
    uint32_t dsra32[] = {0x0004103Fu, JR_RA, 0};
    c.r[4] = 0x8000000100000000ull; kat("dsra32", dsra32, 3, &c, 2, 0xFFFFFFFF80000001ull);
    /* slt $v0, $a0, $a1 is a 64-bit compare */
    uint32_t slt[] = {0x0085102Au, JR_RA, 0};
    c.r[4] = 0x100000000ull; c.r[5] = 1; kat("slt 64-bit", slt, 3, &c, 2, 0);
    /* lui $v0, 0x8000 -> sign-extended */
    uint32_t lui[] = {0x3C028000u, JR_RA, 0};
    kat("lui", lui, 3, &c, 2, 0xFFFFFFFF80000000ull);
    /* paddub $v0, $a0, $a1 saturates per byte */
    uint32_t paddub[] = {0x70851628u, JR_RA, 0};
    c.r[4] = 0xF0; c.r[5] = 0x20; c.rh[4] = 0x80; c.rh[5] = 0x7F;
    kat("paddub low", paddub, 3, &c, 2, 0xFF);
    c.r[4] = 0xF0; c.r[5] = 0x20; c.rh[4] = 0x80; c.rh[5] = 0x7F;
    kat("paddub high", paddub, 3, &c, 202, 0xFF);
    /* sq $a0, 0x13($sp) then ld $v0, 0x10($sp): sq ignores the low 4 address bits */
    uint32_t sq[] = {0x7FA40013u, 0xDFA20010u, JR_RA, 0};
    c.r[29] = 0x01EF0000u; c.r[4] = 0x1122334455667788ull; kat("sq aligns", sq, 4, &c, 2, 0x1122334455667788ull);
    /* bnel not taken skips its delay slot: bnel $zero,$zero,+1 ; addiu $v0,$zero,1 ; jr ra ; nop */
    uint32_t bnel[] = {0x54000001u, 0x24020001u, JR_RA, 0};
    c.r[2] = 0; kat("bnel nullifies", bnel, 4, &c, 2, 0);
}

/* ---- differential ------------------------------------------------------------------------------------------- */

int main(int argc, char **argv)
{
    int states = argc > 1 ? atoi(argv[1]) : 24;
    rc_test_arena = malloc(RC_TEST_SPAN);
    uint8_t *base_mem = calloc(1, RC_TEST_SPAN), *mem1 = malloc(RC_TEST_SPAN);
    if (!rc_test_arena || !base_mem || !mem1) return 2;
    memset(rc_test_arena, 0, RC_TEST_MASK + 1);
    known_answers();

    for (uint32_t i = 0; i <= RC_TEST_MASK; i++) base_mem[i] = (uint8_t)(rnd() >> 24);
    unsigned fails = 0, runs = 0, compared = 0, aborted = 0;
    for (unsigned t = 0; t < rc_unit_ntests; t++) {
        const RcUnitTest *u = &rc_unit_tests[t];
        int tfail = 0;
        for (int s = 0; s < states && tfail < 3; s++) {
            RcCpu st, c1, c2;
            rnd_state(&st);
            code = u->words; ncode = u->nwords; funcs = u->funcs; nfuncs = u->nfuncs;
            memcpy(base_mem, u->words, u->nwords * 4);

            memcpy(rc_test_arena, base_mem, RC_TEST_SPAN);
            c1 = st; calllog = 0; bad_jump = 0;
            uint64_t used = 0;
            int r1 = rc_interp(&c1, BASE, BASE, BASE + 4 * u->nwords, 4000, &used);
            uint64_t log1 = calllog;
            memcpy(mem1, rc_test_arena, RC_TEST_SPAN);

            memcpy(rc_test_arena, base_mem, RC_TEST_SPAN);
            c2 = st; c2.r[31] = RC_INTERP_SENTINEL; calllog = 0; bad_jump = 0;
            rc_test_ticks = 0; rc_test_budget = 4000;
            int r2 = setjmp(rc_test_jmp) ? 1 : 0;
            if (!r2) u->funcs[0].fn(&c2);
            runs++;
            if (r1 == RC_INTERP_BAD) { printf("FAIL %s: interpreter hit an unknown instruction\n", u->name); tfail++; continue; }
            if (r1 == RC_INTERP_BUDGET) { aborted++; continue; }
            compared++;
            const char *why = NULL;
            if (r2) why = "translation ran out of budget";
            else if (bad_jump) why = "bad jump";
            else if (log1 != calllog) why = "main-program calls differ";
            else if (memcmp(&c1, &c2, sizeof c1)) why = "CPU state differs";
            else if (memcmp(mem1, rc_test_arena, RC_TEST_SPAN)) why = "memory differs";
            if (why) {
                printf("FAIL %s (state %d): %s\n", u->name, s, why);
                for (int r = 0; r < 32; r++) {
                    if (c1.r[r] != c2.r[r]) printf("   r%-2d interp %016llX  trans %016llX\n", r,
                                                   (unsigned long long)c1.r[r], (unsigned long long)c2.r[r]);
                    if (c1.rh[r] != c2.rh[r]) printf("   rh%-2d interp %016llX  trans %016llX\n", r,
                                                     (unsigned long long)c1.rh[r], (unsigned long long)c2.rh[r]);
                    if (c1.f[r].u != c2.f[r].u) printf("   f%-2d interp %08X  trans %08X\n", r, c1.f[r].u, c2.f[r].u);
                }
                if (c1.hi != c2.hi || c1.lo != c2.lo || c1.hi1 != c2.hi1 || c1.lo1 != c2.lo1) printf("   hi/lo differ\n");
                if (c1.acc.u != c2.acc.u || c1.fcc != c2.fcc) printf("   acc/fcc differ\n");
                tfail++;
            }
        }
        if (tfail) fails++;
    }
    printf("rc_unit: %u known-answer failures; %u tests, %u runs, %u compared, %u over budget (both stopped), "
           "%u tests failed\n", kat_fail, rc_unit_ntests, runs, compared, aborted, fails);
    return (kat_fail || fails) ? 1 : 0;
}
