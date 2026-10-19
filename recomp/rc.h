/*
 * rc.h - the CPU state and memory access used by translated module code (lane R).
 *
 * A translated module function is `void f(RcCpu *c)`: arguments and results live in c, exactly where the EE keeps
 * them (a0-a3/t0-t3 in r[4..11], v0/v1 in r[2..3], f12-f19 in f[12..19], f0 in f[0], sp in r[29]). The PS2 stack
 * is ordinary PS2 memory, so stack frames stay where the module code expects them.
 *
 * Registers are 128-bit on the EE. Only lq/sq/paddub use the upper 64 bits, so they are kept apart in rh[].
 * Every other instruction writes the low 64 bits only and leaves rh[] alone, as on the EE.
 *
 * Memory: RC_ADDR(a) turns a PS2 address into a host pointer. In the port it is the identity (PS2 RAM sits at its
 * console addresses, hw.h PS2_PTR); the tests define it before including this file to point into an arena.
 */
#ifndef DOTR_RC_H
#define DOTR_RC_H

#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef union RcFpr { float f; uint32_t u; int32_t i; } RcFpr;

typedef struct RcCpu {
    uint64_t r[32];     /* GPR, low 64 bits */
    uint64_t rh[32];    /* GPR, high 64 bits (lq/sq/paddub) */
    uint64_t hi, lo;    /* pipeline 0 */
    uint64_t hi1, lo1;  /* pipeline 1 (mult1/div1) */
    RcFpr f[32];
    RcFpr acc;          /* FPU accumulator (adda/suba/mula/madd) */
    uint32_t fcc;       /* FPU condition bit (c.*.s / bc1*) */
} RcCpu;

#ifndef RC_ADDR
#define RC_ADDR(a) ((uint8_t *)(uintptr_t)(uint32_t)(a))
#endif

/* Unaligned-safe, alias-safe accessors (x86 allows unaligned loads; the EE code is aligned anyway). */
typedef int8_t   __attribute__((may_alias)) rc_s8;
typedef uint8_t  __attribute__((may_alias)) rc_u8;
typedef int16_t  __attribute__((may_alias, aligned(1))) rc_s16;
typedef uint16_t __attribute__((may_alias, aligned(1))) rc_u16;
typedef int32_t  __attribute__((may_alias, aligned(1))) rc_s32;
typedef uint32_t __attribute__((may_alias, aligned(1))) rc_u32;
typedef uint64_t __attribute__((may_alias, aligned(1))) rc_u64;

#define RC_S8(a)  (*(rc_s8  *)RC_ADDR(a))
#define RC_U8(a)  (*(rc_u8  *)RC_ADDR(a))
#define RC_S16(a) (*(rc_s16 *)RC_ADDR(a))
#define RC_U16(a) (*(rc_u16 *)RC_ADDR(a))
#define RC_S32(a) (*(rc_s32 *)RC_ADDR(a))
#define RC_U32(a) (*(rc_u32 *)RC_ADDR(a))
#define RC_U64(a) (*(rc_u64 *)RC_ADDR(a))

/* The EE never traps on an integer divide. By zero: lo = (rs < 0) ? 1 : -1, hi = rs.
 * INT_MIN / -1: lo = INT_MIN, hi = 0. Results are sign-extended 32-bit values. */
static inline void rc_div(uint64_t *lo, uint64_t *hi, uint64_t rs, uint64_t rt)
{
    int32_t n = (int32_t)rs, d = (int32_t)rt;
    if (d == 0) { *lo = (int64_t)(n < 0 ? 1 : -1); *hi = (int64_t)n; }
    else if (n == INT32_MIN && d == -1) { *lo = (int64_t)INT32_MIN; *hi = 0; }
    else { *lo = (int64_t)(n / d); *hi = (int64_t)(n % d); }
}

/* EE mult/mult1 (3-operand form): hi:lo = rs * rt (signed 32x32), both halves sign-extended; returns lo for rd. */
static inline uint64_t rc_mult(uint64_t *lo, uint64_t *hi, uint64_t rs, uint64_t rt)
{
    int64_t p = (int64_t)(int32_t)rs * (int64_t)(int32_t)rt;
    *lo = (int64_t)(int32_t)(uint32_t)p;
    *hi = (int64_t)(int32_t)(uint32_t)((uint64_t)p >> 32);
    return *lo;
}

static inline uint64_t rc_multu(uint64_t *lo, uint64_t *hi, uint64_t rs, uint64_t rt)
{
    uint64_t p = (uint64_t)(uint32_t)rs * (uint32_t)rt;
    *lo = (int64_t)(int32_t)(uint32_t)p;
    *hi = (int64_t)(int32_t)(uint32_t)(p >> 32);
    return *lo;
}

static inline uint64_t rc_maddu(uint64_t *lo, uint64_t *hi, uint64_t rs, uint64_t rt)
{
    uint64_t a = ((uint64_t)(uint32_t)*hi << 32) | (uint32_t)*lo;
    a += (uint64_t)(uint32_t)rs * (uint32_t)rt;
    *lo = (int64_t)(int32_t)(uint32_t)a;
    *hi = (int64_t)(int32_t)(uint32_t)(a >> 32);
    return *lo;
}

/* paddub: 16 unsigned bytes, saturating add. With rt = $zero it is a plain 128-bit move. */
static inline void rc_paddub(RcCpu *c, int d, int s, int t)
{
    uint8_t a[16], b[16], o[16];
    memcpy(a, &c->r[s], 8); memcpy(a + 8, &c->rh[s], 8);
    memcpy(b, &c->r[t], 8); memcpy(b + 8, &c->rh[t], 8);
    for (int i = 0; i < 16; i++) { unsigned v = (unsigned)a[i] + b[i]; o[i] = (uint8_t)(v > 255 ? 255 : v); }
    if (d) { memcpy(&c->r[d], o, 8); memcpy(&c->rh[d], o + 8, 8); }
}

/* Calls leaving the module. Provided by the runtime (lane P) or by the tests. */
void rc_call_ext(RcCpu *c, uint32_t addr);    /* a main-program function (jal outside the module) */
void rc_dispatch(RcCpu *c, uint32_t addr);    /* jalr / computed tail call: any code address */
void rc_bad_jump(RcCpu *c, uint32_t from, uint32_t to);

/* RCX(00104520) = call main-program function 0x00104520. The port binds it to the generated thunk rcx_00104520;
 * a build that defines RC_GENERIC_EXT (the tests) sends every call through rc_call_ext instead. */
#ifdef RC_GENERIC_EXT
#define RCX(a) rc_call_ext(c, 0x##a##u)
#else
#define RCX(a) rcx_##a(c)
#endif

/* RC_TICK() sits on every backward branch. The tests use it as a loop budget; in the port it is RC_POLL()
 * (rc_port.h: lets interrupts and other EE threads run while a module loop waits). */
#ifndef RC_TICK
#ifdef RC_PORT_BUILD
extern unsigned int rt_poll_n;
void rt_poll_slow(void);
#define RC_TICK() ((++rt_poll_n & 0xFFF) ? (void)0 : rt_poll_slow())
#else
#define RC_TICK() ((void)0)
#endif
#endif
/* RC_ENTER() starts every translated function (the tests count it too, so deep recursion stops in budget). */
#ifndef RC_ENTER
#define RC_ENTER() ((void)0)
#endif

/* RC_FNAN(x) wraps every FPU arithmetic result. Which NaN payload x86 returns depends on operand order, which
 * the compiler may swap (the EE FPU has no NaNs at all), so the tests make every NaN the default one; the port
 * leaves the value alone. */
#ifndef RC_FNAN
#define RC_FNAN(x) (x)
#endif

#ifndef RC_TRACE
#define RC_TRACE(pc) ((void)0)
#endif

/* One translated function in a module's table. */
typedef struct RcFunc { uint32_t addr; void (*fn)(RcCpu *); } RcFunc;
typedef struct RcModule { const char *name; const RcFunc *funcs; uint32_t nfuncs; } RcModule;

#ifdef __cplusplus
}
#endif
#endif
