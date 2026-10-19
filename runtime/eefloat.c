/*
 * The EE's floating point on the PC (single precision, SSE).
 *
 * The EE FPU never produces an infinity or a NaN: a division by zero gives +-FLT_MAX (the sign of the operands'
 * product), an overflow clamps to +-FLT_MAX, the square root of a negative number is the root of its magnitude,
 * a float -> int conversion out of range saturates, and denormals count as zero. x86 makes infinities and NaNs
 * instead, and they spread (a 0/0 in the camera code turned whole camera matrices into NaN).
 *
 * So on the game's thread the SSE traps for invalid, divide-by-zero and overflow are unmasked (and denormals are
 * flushed). A trapping scalar or packed single-precision instruction inside the game's code gets the EE's result
 * written into its destination here and is skipped. Anywhere else (the runtime, graphics, the C library on the
 * same thread) the instruction runs again with the traps masked, x86 style, and they are armed again after it.
 */
#include <windows.h>
#include <float.h>
#include <math.h>
#include <string.h>
#include <xmmintrin.h>
#include "runtime.h"

#define MXCSR_TRAPS   0x0680u      /* invalid (0x80), divide-by-zero (0x200), overflow (0x400) masks */
#define MXCSR_FLAGS   0x003Fu
#define MXCSR_FTZ_DAZ 0x8040u

static uintptr_t g_lo, g_hi;       /* the game's code (from the PS2-address function table) */
static int g_rearm, g_trace;
static struct { uintptr_t lo, hi; } g_extra[8];    /* more code under the EE rule (ee_float_add_code) */
static int g_nextra;

/* code outside the function table that runs EE maths too: lane R's translated modules and its interpreter */
void ee_float_add_code(uintptr_t lo, uintptr_t hi)
{
    if (g_nextra < 8 && lo < hi) { g_extra[g_nextra].lo = lo; g_extra[g_nextra].hi = hi; g_nextra++; }
}

static int ee_code(uintptr_t a)
{
    int i;
    if (a >= g_lo && a < g_hi) return 1;
    for (i = 0; i < g_nextra; i++)
        if (a >= g_extra[i].lo && a < g_extra[i].hi) return 1;
    return 0;
}

void ee_float_init(void)
{
    unsigned i;
    g_lo = (uintptr_t)-1; g_hi = 0;
    for (i = 0; i < ps2_functab_count; i++) {
        uintptr_t a = (uintptr_t)ps2_functab[i].native;
        if (a < g_lo) g_lo = a;
        if (a > g_hi) g_hi = a;
    }
    g_hi += 0x10000;               /* the last function's body */
    g_trace = GetEnvironmentVariableA("DOTR_FP_TRACE", NULL, 0) != 0;
    _mm_setcsr((_mm_getcsr() | MXCSR_FTZ_DAZ) & ~MXCSR_TRAPS);
}

static float ee_clamp(float x)
{
    if (x != x) return 0.0f;                                   /* (no NaN can reach here from EE results) */
    if (x > FLT_MAX) return FLT_MAX;
    if (x < -FLT_MAX) return -FLT_MAX;
    return x;
}

/* The EE FPU's ADD.S: rounded toward zero (an overflow then gives FLT_MAX by itself). Used only where the PC's
 * round-to-nearest changes what the game does (gen/rules.json): the camera's yaw wrap fmod(2pi + yaw, 2pi) of a
 * camera straight above its look point gives 3FC90FDC (just above pi/2) on the PC but 3FC90FD8 (below) on the PS2;
 * the fixed angle is then 16384 instead of 16383 and the board camera's look direction loses its tiny sideways part
 * (exactly 0 at RAM 0x01FE04F0 on the PS2's layout: BUG-8, PCSX2's "Zoom Glitch" patch puts 1e-5 there). */
float ee_add_rz(float a, float b)
{
    unsigned csr = _mm_getcsr();
    float r;
    _mm_setcsr(csr | 0x6000u | MXCSR_TRAPS);
    r = _mm_cvtss_f32(_mm_add_ss(_mm_set_ss(a), _mm_set_ss(b)));
    _mm_setcsr(csr);
    return r;
}

/* one lane, EE style */
static float ee_op(int op, float a, float b)
{
    switch (op) {
    case 0x58: return ee_clamp(a + b);
    case 0x5C: return ee_clamp(a - b);
    case 0x59: return ee_clamp(a * b);
    case 0x5E:
        if (b == 0.0f) return ((_copysignf(1.0f, a) * _copysignf(1.0f, b)) < 0) ? -FLT_MAX : FLT_MAX;
        return ee_clamp(a / b);
    case 0x51: return sqrtf(fabsf(b));
    case 0x5D: return a < b ? a : b;                           /* min / max: operands are already finite */
    case 0x5F: return a > b ? a : b;
    }
    return a;
}

/* effective address of a ModRM memory operand (32-bit addressing); *len = bytes of ModRM + SIB + displacement */
static uintptr_t ea(const uint8_t *p, const CONTEXT *c, int *len)
{
    static const int regoff[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };
    DWORD r[8];
    int mod = p[0] >> 6, rm = p[0] & 7, n = 1;
    uintptr_t a = 0;
    (void)regoff;
    r[0] = c->Eax; r[1] = c->Ecx; r[2] = c->Edx; r[3] = c->Ebx; r[4] = c->Esp; r[5] = c->Ebp; r[6] = c->Esi; r[7] = c->Edi;
    if (rm == 4) {
        int sib = p[1], base = sib & 7, idx = (sib >> 3) & 7, sc = sib >> 6;
        n = 2;
        if (idx != 4) a += (uintptr_t)r[idx] << sc;
        if (base == 5 && mod == 0) { a += *(const int32_t *)(p + 2); n += 4; }
        else a += r[base];
    } else if (rm == 5 && mod == 0) {
        a = *(const uint32_t *)(p + 1); n += 4;
        *len = n;
        return a;
    } else {
        a = r[rm];
    }
    if (mod == 1) { a += (int8_t)p[n]; n += 1; }
    else if (mod == 2) { a += *(const int32_t *)(p + n); n += 4; }
    *len = n;
    return a;
}

/* 1 = handled (EE result written, instruction skipped) */
static int emulate(CONTEXT *c)
{
    const uint8_t *ip = (const uint8_t *)c->Eip;
    uint8_t *xmm = c->ExtendedRegisters + 160;
    int pre = 0, scalar = 0, op, reg, n, lanes, i;
    float a[4], b[4];
    if (ip[0] == 0xF3) { scalar = 1; pre = 1; }
    if (ip[pre] != 0x0F) return 0;
    op = ip[pre + 1];
    if (op == 0x2C || op == 0x2D) {                           /* cvt(t)ss2si r32, xmm/m32: saturate */
        float v;
        DWORD *dst;
        const uint8_t *m = ip + pre + 2;
        int greg = (m[0] >> 3) & 7;
        if (!scalar) return 0;
        if ((m[0] >> 6) == 3) { memcpy(&v, xmm + 16 * (m[0] & 7), 4); n = 1; }
        else memcpy(&v, (const void *)ea(m, c, &n), 4);
        dst = greg == 0 ? &c->Eax : greg == 1 ? &c->Ecx : greg == 2 ? &c->Edx : greg == 3 ? &c->Ebx :
              greg == 4 ? &c->Esp : greg == 5 ? &c->Ebp : greg == 6 ? &c->Esi : &c->Edi;
        if (v != v) *dst = 0;
        else if (v >= 2147483647.0f) *dst = 0x7FFFFFFF;
        else if (v <= -2147483648.0f) *dst = 0x80000000u;
        else *dst = (DWORD)(int32_t)(op == 0x2C ? v : nearbyintf(v));
        c->Eip += pre + 2 + n;
        return 1;
    }
    if (op != 0x58 && op != 0x59 && op != 0x5C && op != 0x5E && op != 0x51 && op != 0x5D && op != 0x5F) return 0;
    {
        const uint8_t *m = ip + pre + 2;
        reg = (m[0] >> 3) & 7;
        lanes = scalar ? 1 : 4;
        memcpy(a, xmm + 16 * reg, 16);
        if ((m[0] >> 6) == 3) { memcpy(b, xmm + 16 * (m[0] & 7), 16); n = 1; }
        else memcpy(b, (const void *)ea(m, c, &n), 4 * lanes);
        for (i = 0; i < lanes; i++) a[i] = ee_op(op, a[i], b[i]);
        memcpy(xmm + 16 * reg, a, 4 * lanes);
        c->Eip += pre + 2 + n;
    }
    return 1;
}

/* every vblank on the game thread: traps found masked are armed again (logged the first few times, with the frame,
 * to find what masked them; the oracle's abandoned checks can leave a re-arm undone) */
void ee_float_check_armed(void)
{
    static int told;
    unsigned csr = _mm_getcsr();
    g_rearm = 0;            /* a re-arm is due one instruction after its masked re-run: by a vblank it is stale */
    if (!(csr & MXCSR_TRAPS)) return;
    if (told < 4) { told++; hw_log(1, "fp: traps found masked (MXCSR %04X) at frame %u: armed again", csr, rt_vblanks()); }
    _mm_setcsr((csr | MXCSR_FTZ_DAZ) & ~(MXCSR_TRAPS | MXCSR_FLAGS));
}

/* from the crash handler: 1 = the exception was ours and is dealt with */
int ee_float_exception(EXCEPTION_POINTERS *e)
{
    DWORD code = e->ExceptionRecord->ExceptionCode;
    CONTEXT *c = e->ContextRecord;
    uint32_t *mxcsr = (uint32_t *)(c->ExtendedRegisters + 24);
    if (code == 0xC00002B4 /* STATUS_FLOAT_MULTIPLE_FAULTS */ || code == 0xC00002B5 /* ..._TRAPS */ ||
        code == EXCEPTION_FLT_DIVIDE_BY_ZERO ||
        code == EXCEPTION_FLT_INVALID_OPERATION || code == EXCEPTION_FLT_OVERFLOW) {
        *mxcsr &= ~MXCSR_FLAGS;
        if (g_trace) {                      /* DOTR_FP_TRACE=1: each place once */
            static uintptr_t seen[512];
            int i;
            for (i = 0; i < 512 && seen[i] && seen[i] != c->Eip; i++) {}
            if (i < 512 && !seen[i]) {
                seen[i] = c->Eip;
                hw_log(1, "fp: EE rule at %08lX (%s)", c->Eip, ee_code(c->Eip) ? "game" : "other");
            }
        }
        if (ee_code(c->Eip) && emulate(c)) return 1;
        *mxcsr |= MXCSR_TRAPS;              /* not game code (or not an instruction handled here): x86 result */
        c->EFlags |= 0x100;
        g_rearm = 1;
        return 1;
    }
    if (code == EXCEPTION_SINGLE_STEP && g_rearm) {
        *mxcsr &= ~(MXCSR_TRAPS | MXCSR_FLAGS);
        c->EFlags &= ~0x100u;
        g_rearm = 0;
        return 1;
    }
    return 0;
}
