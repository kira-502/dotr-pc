/*
 * rc_interp.c - a small EE (R5900) interpreter for the module instruction set: the reference the translator is
 * tested against. It is written separately from rcgen.py and does not use rc.h's helpers, so a mistake in one
 * shows up as a difference rather than being shared.
 *
 * It runs real EE control flow: jal sets $ra and jumps, jr jumps to the register, delay slots and branch-likely
 * nullification as on the CPU. A jump outside [mod_lo, mod_hi) is a call into the main program: it goes to
 * rc_call_ext and execution continues at $ra. The run ends when control returns to RC_INTERP_SENTINEL.
 */
#include "rc.h"
#include "rc_interp.h"

/* FPU arithmetic. The port build (RC_INTERP_EE_FLOAT) follows the runtime's EE rule (runtime/eefloat.c, applied to
 * the game's code and the translated modules): no infinity or NaN, x/0 = +-FLT_MAX by the sign of a*b, overflow
 * clamps to +-FLT_MAX, a NaN becomes 0. The tests compare against the translation compiled without that rule, so
 * there it is plain IEEE. */
#ifdef RC_INTERP_EE_FLOAT
#include <float.h>
/* exactly what the runtime's trap handler does: the EE result only where x86 would raise invalid, divide-by-zero or
 * overflow (and the trap is armed); a quiet NaN or an infinity coming in passes through as on x86 */
static uint32_t fbits(float x) { uint32_t u; memcpy(&u, &x, 4); return u; }
static int f_nan(float x) { return (fbits(x) & 0x7FFFFFFFu) > 0x7F800000u; }
static int f_snan(float x) { return f_nan(x) && !(fbits(x) & 0x00400000u); }
static int f_inf(float x) { return (fbits(x) & 0x7FFFFFFFu) == 0x7F800000u; }
static float ee_clampf(float x)
{
    if (x != x) return 0.0f;
    if (x > FLT_MAX) return FLT_MAX;
    if (x < -FLT_MAX) return -FLT_MAX;
    return x;
}
static float ee_arith(int op, float a, float b)
{
    float r;
    if (op == 3 && b == 0.0f && !f_nan(a) && !f_inf(a))        /* x/0 and 0/0 trap: +-FLT_MAX by the sign of a*b */
        return ((fbits(a) ^ fbits(b)) >> 31) ? -FLT_MAX : FLT_MAX;
    r = op == 0 ? a + b : op == 1 ? a - b : op == 2 ? a * b : a / b;
    if (f_snan(a) || f_snan(b)) return ee_clampf(r);           /* invalid (signalling NaN in) */
    if (f_nan(a) || f_nan(b)) return r;                       /* quiet NaN in: no trap, x86 result */
    if (f_nan(r)) return ee_clampf(r);                        /* invalid: inf-inf, 0*inf, inf/inf -> 0 */
    if (f_inf(r) && !f_inf(a) && !f_inf(b)) return ee_clampf(r);   /* overflow */
    return r;
}
#define FADD(a, b) ee_arith(0, (a), (b))
#define FSUB(a, b) ee_arith(1, (a), (b))
#define FMUL(a, b) ee_arith(2, (a), (b))
#define FDIV(a, b) ee_arith(3, (a), (b))
#else
#define FADD(a, b) ((a) + (b))
#define FSUB(a, b) ((a) - (b))
#define FMUL(a, b) ((a) * (b))
#define FDIV(a, b) ((a) / (b))
#endif

/* RC_INTERP_EXTERNAL(pc): code at pc is run natively instead (the oracle's deep mode); never for the entry */
#ifndef RC_INTERP_EXTERNAL
#define RC_INTERP_EXTERNAL(pc) 0
#endif

#ifndef RC_FETCH
#define RC_FETCH(pc) RC_U32(pc)
#endif

#define S32(x) ((int32_t)(uint32_t)(x))
#define SXT(x) ((uint64_t)(int64_t)(int32_t)(uint32_t)(x))

static void wr(RcCpu *c, int n, uint64_t v) { if (n) c->r[n] = v; }

static void mul_signed(uint64_t *lo, uint64_t *hi, uint64_t a, uint64_t b)
{
    int64_t p = (int64_t)S32(a) * (int64_t)S32(b);
    uint32_t l = (uint32_t)((uint64_t)p & 0xFFFFFFFFu), h = (uint32_t)((uint64_t)p >> 32);
    *lo = SXT(l); *hi = SXT(h);
}

static void divide(uint64_t *lo, uint64_t *hi, uint64_t a, uint64_t b)
{
    int32_t n = S32(a), d = S32(b);
    if (d == 0) { *lo = n >= 0 ? 0xFFFFFFFFFFFFFFFFull : 1; *hi = SXT((uint32_t)n); return; }
    if ((uint32_t)n == 0x80000000u && d == -1) { *lo = 0xFFFFFFFF80000000ull; *hi = 0; return; }
    *lo = SXT((uint32_t)(n / d)); *hi = SXT((uint32_t)(n % d));
}

int rc_interp(RcCpu *c, uint32_t entry, uint32_t mod_lo, uint32_t mod_hi, uint64_t budget, uint64_t *used)
{
    uint32_t pc = entry, npc = entry + 4;
    uint32_t sp0 = (uint32_t)c->r[29];
    uint64_t n = 0;
    c->r[31] = RC_INTERP_SENTINEL;
    for (;;) {
        /* back at the sentinel with the entry $sp = the function returned. The same address reached any other way
         * (a function pointer that happens to hold it) is an ordinary jump out of the module, as in the translation */
        if (pc == RC_INTERP_SENTINEL && (uint32_t)c->r[29] == sp0) { if (used) *used = n; return RC_INTERP_DONE; }
        if (pc < mod_lo || pc >= mod_hi || (n && RC_INTERP_EXTERNAL(pc))) {
            /* left the module: a call (or tail call) into the main program, which returns to $ra */
            if (++n > budget) { if (used) *used = n; return RC_INTERP_BUDGET; }
            rc_call_ext(c, pc);
            pc = (uint32_t)c->r[31]; npc = pc + 4;
            continue;
        }
        if (++n > budget) { if (used) *used = n; return RC_INTERP_BUDGET; }
#ifdef RC_INTERP_TRACE
        RC_INTERP_TRACE(pc);
#endif
        uint32_t w = RC_FETCH(pc);
        uint32_t cur = pc;
        pc = npc; npc += 4;
        int op = w >> 26, rs = (w >> 21) & 31, rt = (w >> 16) & 31, rd = (w >> 11) & 31, sa = (w >> 6) & 31;
        int fn = w & 63;
        int32_t simm = (int16_t)(w & 0xFFFF);
        uint32_t uimm = w & 0xFFFF;
        uint64_t A = c->r[rs], B = c->r[rt];
        uint32_t addr = (uint32_t)A + (uint32_t)simm;
        uint32_t btarget = cur + 4 + ((uint32_t)simm << 2);
        int taken = -1, likely = 0;
        RcFpr *f = c->f;
        int fs = rd, ft = rt, fd = sa;
        switch (op) {
        case 0x00:
            switch (fn) {
            case 0x00: wr(c, rd, SXT((uint32_t)B << sa)); break;
            case 0x02: wr(c, rd, SXT((uint32_t)B >> sa)); break;
            case 0x03: wr(c, rd, SXT((uint32_t)(S32(B) >> sa))); break;
            case 0x04: wr(c, rd, SXT((uint32_t)B << (A & 31))); break;
            case 0x06: wr(c, rd, SXT((uint32_t)B >> (A & 31))); break;
            case 0x07: wr(c, rd, SXT((uint32_t)(S32(B) >> (A & 31)))); break;
            case 0x08: npc = (uint32_t)A; break;                                     /* jr */
            case 0x09: wr(c, rd, cur + 8); npc = (uint32_t)A; break;                 /* jalr */
            case 0x0A: if (B == 0) wr(c, rd, A); break;
            case 0x0B: if (B != 0) wr(c, rd, A); break;
            case 0x10: wr(c, rd, c->hi); break;
            case 0x12: wr(c, rd, c->lo); break;
            case 0x18: mul_signed(&c->lo, &c->hi, A, B); wr(c, rd, c->lo); break;
            case 0x19: {
                uint64_t p = (uint64_t)(uint32_t)A * (uint64_t)(uint32_t)B;
                c->lo = SXT((uint32_t)p); c->hi = SXT((uint32_t)(p >> 32)); wr(c, rd, c->lo); break;
            }
            case 0x1A: divide(&c->lo, &c->hi, A, B); break;
            case 0x21: wr(c, rd, SXT((uint32_t)A + (uint32_t)B)); break;
            case 0x23: wr(c, rd, SXT((uint32_t)A - (uint32_t)B)); break;
            case 0x24: wr(c, rd, A & B); break;
            case 0x25: wr(c, rd, A | B); break;
            case 0x26: wr(c, rd, A ^ B); break;
            case 0x27: wr(c, rd, ~(A | B)); break;
            case 0x2A: wr(c, rd, (int64_t)A < (int64_t)B ? 1 : 0); break;
            case 0x2B: wr(c, rd, A < B ? 1 : 0); break;
            case 0x38: wr(c, rd, B << sa); break;
            /* main-program code only (the oracle); modules never use these */
            case 0x0F: break;                                                        /* sync */
            case 0x14: wr(c, rd, B << (A & 63)); break;                              /* dsllv */
            case 0x16: wr(c, rd, B >> (A & 63)); break;                              /* dsrlv */
            case 0x1B: {                                                             /* divu: never traps */
                uint32_t n = (uint32_t)A, d = (uint32_t)B;
                c->lo = d ? SXT(n / d) : 0xFFFFFFFFFFFFFFFFull;
                c->hi = d ? SXT(n % d) : SXT(n);
                break;
            }
            case 0x20: wr(c, rd, SXT((uint32_t)A + (uint32_t)B)); break;             /* add (no overflow trap here) */
            case 0x2D: wr(c, rd, A + B); break;                                      /* daddu */
            case 0x2F: wr(c, rd, A - B); break;                                      /* dsubu */
            case 0x3A: wr(c, rd, B >> sa); break;                                    /* dsrl */
            case 0x3C: wr(c, rd, B << (sa + 32)); break;
            case 0x3E: wr(c, rd, B >> (sa + 32)); break;
            case 0x3F: wr(c, rd, (uint64_t)((int64_t)B >> (sa + 32))); break;
            default: return RC_INTERP_BAD;
            }
            break;
        case 0x01:
            switch (rt) {
            case 0: taken = (int64_t)A < 0; break;
            case 1: taken = (int64_t)A >= 0; break;
            case 2: taken = (int64_t)A < 0; likely = 1; break;
            case 3: taken = (int64_t)A >= 0; likely = 1; break;
            default: return RC_INTERP_BAD;
            }
            break;
        case 0x02: npc = (cur & 0xF0000000u) | ((w & 0x3FFFFFFu) << 2); break;          /* j (main code) */
        case 0x03: c->r[31] = cur + 8; npc = (cur & 0xF0000000u) | ((w & 0x3FFFFFFu) << 2); break;
        case 0x08: wr(c, rt, SXT((uint32_t)A + (uint32_t)simm)); break;              /* addi (main code) */
        case 0x10: break;                                                            /* COP0 (ei/di, main code): no-op */
        case 0x04: taken = A == B; break;
        case 0x05: taken = A != B; break;
        case 0x06: taken = (int64_t)A <= 0; break;
        case 0x07: taken = (int64_t)A > 0; break;
        case 0x14: taken = A == B; likely = 1; break;
        case 0x15: taken = A != B; likely = 1; break;
        case 0x16: taken = (int64_t)A <= 0; likely = 1; break;
        case 0x17: taken = (int64_t)A > 0; likely = 1; break;
        case 0x09: wr(c, rt, SXT((uint32_t)A + (uint32_t)simm)); break;
        case 0x0A: wr(c, rt, (int64_t)A < (int64_t)simm ? 1 : 0); break;
        case 0x0B: wr(c, rt, A < (uint64_t)(int64_t)simm ? 1 : 0); break;
        case 0x0C: wr(c, rt, A & uimm); break;
        case 0x0D: wr(c, rt, A | uimm); break;
        case 0x0E: wr(c, rt, A ^ uimm); break;
        case 0x0F: wr(c, rt, SXT(uimm << 16)); break;
        case 0x19: wr(c, rt, A + (uint64_t)(int64_t)simm); break;
        case 0x20: wr(c, rt, (uint64_t)(int64_t)*(int8_t *)RC_ADDR(addr)); break;
        case 0x21: { int16_t v; memcpy(&v, RC_ADDR(addr), 2); wr(c, rt, (uint64_t)(int64_t)v); break; }
        case 0x23: { int32_t v; memcpy(&v, RC_ADDR(addr), 4); wr(c, rt, (uint64_t)(int64_t)v); break; }
        case 0x24: wr(c, rt, *(uint8_t *)RC_ADDR(addr)); break;
        case 0x25: { uint16_t v; memcpy(&v, RC_ADDR(addr), 2); wr(c, rt, v); break; }
        case 0x37: { uint64_t v; memcpy(&v, RC_ADDR(addr), 8); wr(c, rt, v); break; }
        case 0x1E: {
            uint64_t v[2]; memcpy(v, RC_ADDR(addr & ~15u), 8); memcpy(v + 1, RC_ADDR((addr & ~15u) + 8), 8);
            if (rt) { c->r[rt] = v[0]; c->rh[rt] = v[1]; }
            break;
        }
        case 0x28: { uint8_t v = (uint8_t)B; memcpy(RC_ADDR(addr), &v, 1); break; }
        case 0x29: { uint16_t v = (uint16_t)B; memcpy(RC_ADDR(addr), &v, 2); break; }
        case 0x2B: { uint32_t v = (uint32_t)B; memcpy(RC_ADDR(addr), &v, 4); break; }
        case 0x3F: memcpy(RC_ADDR(addr), &B, 8); break;
        case 0x1F: {
            uint64_t h = c->rh[rt];
            if (rt == 0) h = 0;
            memcpy(RC_ADDR(addr & ~15u), &B, 8); memcpy(RC_ADDR((addr & ~15u) + 8), &h, 8);
            break;
        }
        case 0x31: memcpy(&f[ft].u, RC_ADDR(addr), 4); break;
        case 0x39: memcpy(RC_ADDR(addr), &f[ft].u, 4); break;
        case 0x11:
            if (rs == 4) { f[fs].u = (uint32_t)B; break; }
            if (rs == 8) {
                if (rt > 3) return RC_INTERP_BAD;
                taken = (rt & 1) ? (c->fcc != 0) : (c->fcc == 0);
                likely = rt >> 1;
                break;
            }
            if (rs == 0x14 && fn == 0x20) { f[fd].f = (float)f[fs].i; break; }
            if (rs != 0x10) return RC_INTERP_BAD;
            switch (fn) {
            case 0x00: f[fd].f = RC_FNAN(FADD(f[fs].f, f[ft].f)); break;
            case 0x01: f[fd].f = RC_FNAN(FSUB(f[fs].f, f[ft].f)); break;
            case 0x02: f[fd].f = RC_FNAN(FMUL(f[fs].f, f[ft].f)); break;
            case 0x03: f[fd].f = RC_FNAN(FDIV(f[fs].f, f[ft].f)); break;
            case 0x06: f[fd].u = f[fs].u; break;
            case 0x07: f[fd].u = f[fs].u ^ 0x80000000u; break;
            case 0x18: c->acc.f = RC_FNAN(FADD(f[fs].f, f[ft].f)); break;
            case 0x19: c->acc.f = RC_FNAN(FSUB(f[fs].f, f[ft].f)); break;
            case 0x1A: c->acc.f = RC_FNAN(FMUL(f[fs].f, f[ft].f)); break;
            case 0x1C: { volatile float m = RC_FNAN(FMUL(f[fs].f, f[ft].f)); f[fd].f = RC_FNAN(FADD(c->acc.f, m)); break; }
            case 0x1D: { volatile float m = RC_FNAN(FMUL(f[fs].f, f[ft].f)); f[fd].f = RC_FNAN(FSUB(c->acc.f, m)); break; }  /* msub.s */
            case 0x32: c->fcc = f[fs].f == f[ft].f; break;
            case 0x34: c->fcc = f[fs].f < f[ft].f; break;
            case 0x36: c->fcc = f[fs].f <= f[ft].f; break;
            default: return RC_INTERP_BAD;
            }
            break;
        case 0x1C:
            if (fn == 0x28 && sa == 0x18) {                                          /* paddub */
                uint8_t a[16], b[16], o[16];
                memcpy(a, &c->r[rs], 8); memcpy(a + 8, &c->rh[rs], 8);
                memcpy(b, &c->r[rt], 8); memcpy(b + 8, &c->rh[rt], 8);
                if (rs == 0) memset(a, 0, 16);
                if (rt == 0) memset(b, 0, 16);
                for (int i = 0; i < 16; i++) { int v = a[i] + b[i]; o[i] = v > 255 ? 255 : (uint8_t)v; }
                if (rd) { memcpy(&c->r[rd], o, 8); memcpy(&c->rh[rd], o + 8, 8); }
                break;
            }
            switch (fn) {
            case 0x01: {                                                             /* maddu */
                uint64_t acc = ((uint64_t)(uint32_t)c->hi << 32) | (uint32_t)c->lo;
                acc += (uint64_t)(uint32_t)A * (uint64_t)(uint32_t)B;
                c->lo = SXT((uint32_t)acc); c->hi = SXT((uint32_t)(acc >> 32)); wr(c, rd, c->lo); break;
            }
            case 0x10: wr(c, rd, c->hi1); break;
            case 0x12: wr(c, rd, c->lo1); break;
            case 0x18: mul_signed(&c->lo1, &c->hi1, A, B); wr(c, rd, c->lo1); break;
            case 0x1A: divide(&c->lo1, &c->hi1, A, B); break;
            default: return RC_INTERP_BAD;
            }
            break;
        default:
            return RC_INTERP_BAD;
        }
        if (taken >= 0) {
            if (taken) npc = btarget;
            else if (likely) { pc = npc; npc += 4; }                                  /* nullify the delay slot */
        }
    }
}
