/* vu0_rt.h - the small runtime the generated vu0_funcs.c is written against: EE registers, memory access at PS2
 * addresses (identity-mapped), EE FPU semantics, MMI lane moves, COP2 moves. */
#ifndef DOTR_VU0_RT_H
#define DOTR_VU0_RT_H

#include <math.h>
#include <xmmintrin.h>
#include "vu0.h"
#include "../vu_math.h"

#define R(n)    ee.r[n].lo
#define F(n)    ee.f[n]
#define SX32(x) ((u64)(s64)(s32)(u32)(x))

/* ---- memory: PS2 address == host pointer; EE hardware registers are not reachable from this code ---- */

static inline u8 *mp(u32 a)
{
    static u8 sink[16];
    u32 top = a >> 28;
    if (top == 2 || top == 3) a = ps2_phys(a);
    if (top == 1) {
        /* VIF0 registers (10003800..) and FIFO (10004000..): only sceVpu0Reset writes them, to reset VIF0 and load
         * its default CYCLE/MASK/MODE/ITOP. Nothing in the main program or the modules runs VU0 micro code (no
         * VCALLMS, no VIF0 DMA), so that state is never read and the writes are accepted as-is. */
        if ((a & 0xFFFFFF00u) != 0x10003800u && (a & 0xFFFFF000u) != 0x10004000u)
            hw_log(1, "vu0: hardware access at %08x ignored", a);
        memset(sink, 0, 16); return sink;
    }
    return (u8 *)(uintptr_t)a;
}
static inline u32 ld32(u32 a) { u32 v; memcpy(&v, mp(a), 4); return v; }
static inline u16 ld16(u32 a) { u16 v; memcpy(&v, mp(a), 2); return v; }
static inline u8  ld8(u32 a)  { return *mp(a); }
/* No alignment masking on 64/128-bit accesses: the PS2 ignores the low address bits because its data is always
 * aligned, but in the port the game's locals (e.g. an f32 matrix[16] on a fiber stack) may be only 4-aligned and
 * masking would read the wrong memory. */
static inline u64 ld64(u32 a) { u64 v; memcpy(&v, mp(a), 8); return v; }
static inline void st32(u32 a, u32 v) { memcpy(mp(a), &v, 4); }
static inline void st16(u32 a, u16 v) { memcpy(mp(a), &v, 2); }
static inline void st8(u32 a, u8 v)   { *mp(a) = v; }
static inline void st64(u32 a, u64 v) { memcpy(mp(a), &v, 8); }
static inline void ldq(int rt, u32 a) { const u8 *p = mp(a); memcpy(&ee.r[rt].lo, p, 8); memcpy(&ee.r[rt].hi, p + 8, 8); }
static inline void stq(u32 a, int rt) { u8 *p = mp(a); memcpy(p, &ee.r[rt].lo, 8); memcpy(p + 8, &ee.r[rt].hi, 8); }

static inline float u2f(u32 u) { float f; memcpy(&f, &u, 4); return f; }
static inline u32 f2u(float f) { u32 u; memcpy(&u, &f, 4); return u; }
static inline float ldf(u32 a) { return u2f(ld32(a)); }
static inline void stf(u32 a, float f) { st32(a, f2u(f)); }

/* ---- COP2 moves ------------------------------------------------------------------------------------ */

static inline void lqc2(int vf, u32 a) { if (vf) memcpy(&vu0.vf[vf], mp(a), 16); }
static inline void sqc2(u32 a, int vf) { memcpy(mp(a), &vu0.vf[vf], 16); }
static inline void qmtc2(int rt, int vf) { if (vf) { memcpy(&vu0.vf[vf].u[0], &ee.r[rt].lo, 8); memcpy(&vu0.vf[vf].u[2], &ee.r[rt].hi, 8); } }
static inline void qmfc2(int rt, int vf) { if (rt) { memcpy(&ee.r[rt].lo, &vu0.vf[vf].u[0], 8); memcpy(&ee.r[rt].hi, &vu0.vf[vf].u[2], 8); } }

/* ---- MMI ------------------------------------------------------------------------------------------- */

static inline void w4(int r, u32 *w) { memcpy(w, &ee.r[r].lo, 8); memcpy(w + 2, &ee.r[r].hi, 8); }
static inline void w4set(int r, const u32 *w) { if (r) { memcpy(&ee.r[r].lo, w, 8); memcpy(&ee.r[r].hi, w + 2, 8); } }
static inline void mmi_pextlw(int rd, int rs, int rt) { u32 s[4], t[4], d[4]; w4(rs, s); w4(rt, t); d[0] = t[0]; d[1] = s[0]; d[2] = t[1]; d[3] = s[1]; w4set(rd, d); }
static inline void mmi_pextuw(int rd, int rs, int rt) { u32 s[4], t[4], d[4]; w4(rs, s); w4(rt, t); d[0] = t[2]; d[1] = s[2]; d[2] = t[3]; d[3] = s[3]; w4set(rd, d); }
static inline void mmi_pcpyld(int rd, int rs, int rt) { u64 lo = ee.r[rt].lo, hi = ee.r[rs].lo; if (rd) { ee.r[rd].lo = lo; ee.r[rd].hi = hi; } }
static inline void mmi_pcpyud(int rd, int rs, int rt) { u64 lo = ee.r[rs].hi, hi = ee.r[rt].hi; if (rd) { ee.r[rd].lo = lo; ee.r[rd].hi = hi; } }
static inline void mmi_por(int rd, int rs, int rt) { u64 lo = ee.r[rs].lo | ee.r[rt].lo, hi = ee.r[rs].hi | ee.r[rt].hi; if (rd) { ee.r[rd].lo = lo; ee.r[rd].hi = hi; } }
static inline void mmi_paddub(int rd, int rs, int rt)
{
    u8 s[16], t[16], d[16]; int i;
    memcpy(s, &ee.r[rs].lo, 8); memcpy(s + 8, &ee.r[rs].hi, 8);
    memcpy(t, &ee.r[rt].lo, 8); memcpy(t + 8, &ee.r[rt].hi, 8);
    for (i = 0; i < 16; i++) { int v = s[i] + t[i]; d[i] = (u8)(v > 255 ? 255 : v); }
    if (rd) { memcpy(&ee.r[rd].lo, d, 8); memcpy(&ee.r[rd].hi, d + 8, 8); }
}

/* ---- integer divide (the EE never traps) ----------------------------------------------------------- */

static inline void ee_div(s32 a, s32 b)
{
    if (b == 0) { ee.lo = SX32(a < 0 ? 1 : -1); ee.hi = SX32(a); return; }
    if (a == (s32)0x80000000 && b == -1) { ee.lo = SX32(a); ee.hi = 0; return; }
    ee.lo = SX32(a / b); ee.hi = SX32(a % b);
}
static inline void ee_divu(u32 a, u32 b)
{
    if (b == 0) { ee.lo = SX32(0xFFFFFFFFu); ee.hi = SX32(a); return; }
    ee.lo = SX32(a / b); ee.hi = SX32(a % b);
}

/* ---- EE FPU: no Inf/NaN/denormals, round toward zero ----------------------------------------------- */

static inline float fpu_in(float f) { return vf_in(f2u(f)); }
static inline float fpu_res(double d) { u32 fl; return u2f(vd_out(d, &fl)); }
static inline float fpu_add(float a, float b) { return fpu_res((double)fpu_in(a) + fpu_in(b)); }
static inline float fpu_sub(float a, float b) { return fpu_res((double)fpu_in(a) - fpu_in(b)); }
static inline float fpu_mul(float a, float b) { return fpu_res((double)fpu_in(a) * fpu_in(b)); }
static inline float fpu_div(float a, float b)
{
    float x = fpu_in(a), y = fpu_in(b);
    if (y == 0) return u2f(((f2u(a) ^ f2u(b)) & 0x80000000u) | 0x7FFFFFFFu);
    return fpu_res((double)x / y);
}
static inline float fpu_sqrt(float a) { double x = fpu_in(a); return fpu_res(sqrt(x < 0 ? -x : x)); }
static inline s32 fpu_cvtw(float a)
{
    double x = fpu_in(a);
    return x >= 2147483647.0 ? 0x7FFFFFFF : x <= -2147483648.0 ? (s32)0x80000000 : (s32)x;
}

/* ---- entry from native code / calls into native code ----------------------------------------------- */

#define VU0_STACK_BYTES 0x10000
extern u8 vu0_stack[VU0_STACK_BYTES];
extern int vu0_depth;

#define VU0_ENTER() \
    ee_regs saved_; unsigned int csr_ = _mm_getcsr(); \
    memcpy(&saved_, &ee, sizeof ee); \
    if (vu0_depth++ == 0) { R(29) = SX32((u32)(uintptr_t)(vu0_stack + VU0_STACK_BYTES - 64)); vu0_host_csr = csr_; } \
    _mm_setcsr((csr_ & ~0x6000u) | 0x6000u | 0x8040u)

#define VU0_LEAVE() \
    do { vu0_depth--; memcpy(&ee, &saved_, sizeof ee); _mm_setcsr(csr_); } while (0)

/* Native code runs with the host's own rounding mode; registers survive because every entry saves them. */
#define VU0_NATIVE_BEGIN() unsigned int ncsr_ = _mm_getcsr(); _mm_setcsr(vu0_host_csr)
#define VU0_NATIVE_END()   _mm_setcsr(ncsr_)
extern unsigned int vu0_host_csr;

#endif
