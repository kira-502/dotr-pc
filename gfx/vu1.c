/* vu1.c - VU1 micro-mode interpreter.
 *
 * Timing model: one instruction pair per cycle plus stalls. An FMAC (or lower VF-writing) result is ready 4 cycles
 * after issue; an instruction that reads a VF register before it is ready stalls (ACC is forwarded: accumulate
 * chains such as MULA, MADDA, MADDA, MADD issue one per cycle). MAC/status/clip flags
 * become visible 4 cycles after the instruction that set them. Q is ready 7 (DIV, SQRT) or 13 (RSQRT) cycles
 * after issue; reading it earlier gives the old Q. P (EFU) likewise, with the documented latencies.
 * Arithmetic: VU floats have no Inf/NaN/denormals (handled by clamping, flush to zero) and round toward zero.
 */
#include <math.h>
#include <float.h>
#include <xmmintrin.h>
#include <stdio.h>
#include <stdlib.h>
#include "gfx_internal.h"

vu1_state vu1;

/* ---- pipeline state -------------------------------------------------------------------------------- */

#define REG_ACC 32
/* Cycle times inside the interpreter are the low 32 bits of vu1.cycles (32-bit arithmetic is much cheaper on this
 * 32-bit build); they are compared wrap-safely, and ready times in the past are brought up to the start cycle of
 * every program, so no stored time is ever more than a program's length away from now. */
typedef u32 cyc_t;
#define CYC_LE(a, b) ((s32)((a) - (b)) <= 0)
#define CYC_GT(a, b) ((s32)((a) - (b)) > 0)
static cyc_t vf_ready[33];        /* cycle at which VF reg (or ACC) can be read without a stall */

#define FQ_N 64                    /* pending flag events (they land lazily, in batches) */
typedef struct flag_ev { cyc_t ready; u32 mac; u32 clip; int has_mac, has_clip; } flag_ev;
static flag_ev fq[FQ_N];
static int fq_head, fq_n;
static u32 clip_latest;            /* clip value including pending CLIPs (for the shift chain) */

static u32 q_pending; static cyc_t q_ready; static int q_busy;
static u32 p_pending; static cyc_t p_ready; static int p_busy;
static u32 qflag_pending;          /* D/I status bits from the pending DIV */
static int xk_mode;                /* lockstep check: 1 hash XGKICK packets, 2 hash and do not draw them */
static u32 xk_hash;

/* Q and P land when their latency has passed; every instruction checks (FMACs read Q and I). */
static inline void qp_update_ex(cyc_t now, int all)
{
    if (!(q_busy | p_busy)) return;
    if (q_busy && (all || CYC_LE(q_ready, now))) {
        vu1.q = q_pending;
        vu1.status = (vu1.status & ~0x30u) | qflag_pending | (qflag_pending << 6);
        q_busy = 0;
    }
    if (p_busy && (all || CYC_LE(p_ready, now))) { vu1.p = p_pending; p_busy = 0; }
}

static inline void qp_update(cyc_t now) { qp_update_ex(now, 0); }

/* MAC, status and clip results land lazily: only an instruction that reads (or sets) them, a full event ring and
 * the end of a program bring them up to date, which gives the same values as landing every cycle (the D/I bits Q
 * sets and the bits MAC results set do not overlap, so their order does not matter). */
static inline void flags_update_ex(cyc_t now, int all)
{
    if (!fq_n && !q_busy && !p_busy) return;
    while (fq_n && (all || CYC_LE(fq[fq_head].ready, now))) {
        flag_ev *e = &fq[fq_head];
        if (e->has_mac) {
            u32 m = e->mac, sb = (m & 0x000F ? 1u : 0) | (m & 0x00F0 ? 2u : 0) | (m & 0x0F00 ? 4u : 0) | (m & 0xF000 ? 8u : 0);
            vu1.status = (vu1.status & ~0xFu) | sb | (sb << 6);   /* Z S U O, and their sticky copies */
            vu1.mac = m;
        }
        if (e->has_clip) vu1.clip = e->clip;
        fq_head = (fq_head + 1) & (FQ_N - 1);
        fq_n--;
    }
    qp_update_ex(now, all);
}

static inline void flags_update(cyc_t now) { flags_update_ex(now, 0); }
static inline void flags_update_all(void) { flags_update_ex(0, 1); }

static inline void flags_push(cyc_t ready, int has_mac, u32 mac, int has_clip, u32 clip)
{
    flag_ev *e;
    if (fq_n == FQ_N) flags_update(ready - 4);   /* the issue cycle: at most 4 events are younger */
    if (fq_n == FQ_N) flags_update_all();
    e = &fq[(fq_head + fq_n) & (FQ_N - 1)];
    e->ready = ready; e->has_mac = has_mac; e->mac = mac; e->has_clip = has_clip; e->clip = clip;
    fq_n++;
}

void vu1_reset(void)
{
    memset(&vu1, 0, sizeof vu1);
    vu1.vf[0].f[3] = 1.0f;
    memset(vf_ready, 0, sizeof vf_ready);
    fq_head = fq_n = 0;
    clip_latest = 0;
    q_busy = p_busy = 0;
    vu1.q = 0x3F800000u;
    vu1.r = 0x3F800000u;
    vu1_micro_changed();
}

#include "vu_math.h"

/* ---- decoding helpers ------------------------------------------------------------------------------ */

#define DEST(w)  (((w) >> 21) & 0xF)
#define FT(w)    (((w) >> 16) & 0x1F)
#define FS(w)    (((w) >> 11) & 0x1F)
#define FD(w)    (((w) >> 6) & 0x1F)
#define HASC(d, i) (((d) >> (3 - (i))) & 1)

static inline s32 imm11(u32 w) { s32 v = (s32)(w & 0x7FF); return v >= 0x400 ? v - 0x800 : v; }
static inline s32 imm5(u32 w)  { s32 v = (s32)((w >> 6) & 0x1F); return v >= 0x10 ? v - 0x20 : v; }
static inline u32 imm15(u32 w) { return ((w >> 10) & 0x7800) | (w & 0x7FF); }
static inline u32 imm24(u32 w) { return w & 0xFFFFFF; }
static inline u32 imm12(u32 w) { return ((w >> 10) & 0x800) | (w & 0x7FF); }

/* ---- upper instructions ---------------------------------------------------------------------------- */

typedef struct upper_res {
    int writes_vf, writes_acc, sets_mac, sets_clip;
    int reg;          /* destination VF */
    vu_vec val;
    u32 dest;
    u32 mac;
    u32 clip;
} upper_res;

enum { OP_ADD, OP_SUB, OP_MADD, OP_MSUB, OP_MUL, OP_MAX, OP_MINI };

/* Single-precision result of one lane when no clamping is involved: under the round-toward-zero, flush-to-zero
 * mode the run loop sets, float arithmetic truncates exactly as the double path below does (the float grid is a
 * subset of the double grid). Returns 0 when the lane needs the exact path: a result at or beyond FLT_MAX (an
 * overflow, which this rounding mode turns into FLT_MAX), or a zero that is not an exact one (a hidden underflow). */
static inline int fast_lane(int op, float a, float b, float acc, u32 *out, u32 *fl)
{
    float r, p;
    u32 u;
    switch (op) {
    case OP_ADD: r = a + b; if (r == 0.0f && a != -b) return 0; break;
    case OP_SUB: r = a - b; if (r == 0.0f && a != b) return 0; break;
    case OP_MUL: r = a * b; if (r == 0.0f && a != 0.0f && b != 0.0f) return 0; break;
    case OP_MADD: case OP_MSUB:
        p = a * b;
        memcpy(&u, &p, 4);
        if ((u & 0x7FFFFFFFu) >= 0x7F7FFFFFu || (p == 0.0f && a != 0.0f && b != 0.0f)) return 0;
        r = op == OP_MADD ? acc + p : acc - p;
        if (r == 0.0f && (op == OP_MADD ? acc != -p : acc != p)) return 0;
        break;
    default: return 0;
    }
    memcpy(&u, &r, 4);
    if ((u & 0x7FFFFFFFu) >= 0x7F7FFFFFu) return 0;   /* round-toward-zero overflow gives FLT_MAX, not Inf */
    *out = u;
    *fl = (r == 0.0f ? 1u : 0u) | ((u >> 31) << 1);
    return 1;
}

/* All four lanes at once with SSE under the same rounding mode. Returns 0 (nothing written) when any written lane
 * needs the exact path: an input or result at FLT_MAX or beyond, or a zero that is not an exact one. */
static inline __m128 vf_in4(__m128i x)
{
    __m128i e = _mm_and_si128(x, _mm_set1_epi32(0x7F800000));
    __m128i inf = _mm_cmpeq_epi32(e, _mm_set1_epi32(0x7F800000));
    __m128i clamped = _mm_or_si128(_mm_and_si128(x, _mm_set1_epi32((int)0x80000000)), _mm_set1_epi32(0x7F7FFFFF));
    return _mm_castsi128_ps(_mm_or_si128(_mm_and_si128(inf, clamped), _mm_andnot_si128(inf, x)));
}

/* vf_in on four lanes, bit for bit: a zero exponent gives a signed zero, an Inf/NaN exponent the signed FLT_MAX */
static inline __m128 vf_in4_exact(__m128i x)
{
    __m128i sign = _mm_and_si128(x, _mm_set1_epi32((int)0x80000000));
    __m128i e = _mm_and_si128(x, _mm_set1_epi32(0x7F800000));
    __m128i zero = _mm_cmpeq_epi32(e, _mm_setzero_si128()), inf = _mm_cmpeq_epi32(e, _mm_set1_epi32(0x7F800000));
    __m128i r = _mm_or_si128(_mm_andnot_si128(_mm_or_si128(zero, inf), x), _mm_and_si128(zero, sign));
    r = _mm_or_si128(r, _mm_and_si128(inf, _mm_or_si128(sign, _mm_set1_epi32(0x7F7FFFFF))));
    return _mm_castsi128_ps(r);
}

static inline __m128i big4(__m128 v)   /* lanes at or beyond FLT_MAX in magnitude */
{
    __m128i m = _mm_and_si128(_mm_castps_si128(v), _mm_set1_epi32(0x7FFFFFFF));
    return _mm_cmpgt_epi32(m, _mm_set1_epi32(0x7F7FFFFE));
}

static __forceinline int fmac_vec(upper_res *r, int op, u32 dest, const vu_vec *fs, const float *tv)
{
    static const u8 rev4[16] = { 0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15 };
    __m128 a = vf_in4(_mm_loadu_si128((const __m128i *)fs->u)), b = _mm_loadu_ps(tv), z = _mm_setzero_ps(), res;
    __m128i bad;
    int lanes = rev4[dest & 15], zm, sm;   /* bit i = lane i written */
    switch (op) {
    case OP_ADD:
        res = _mm_add_ps(a, b);
        bad = _mm_andnot_si128(_mm_castps_si128(_mm_cmpeq_ps(a, _mm_sub_ps(z, b))), _mm_castps_si128(_mm_cmpeq_ps(res, z)));
        break;
    case OP_SUB:
        res = _mm_sub_ps(a, b);
        bad = _mm_andnot_si128(_mm_castps_si128(_mm_cmpeq_ps(a, b)), _mm_castps_si128(_mm_cmpeq_ps(res, z)));
        break;
    case OP_MUL:
        res = _mm_mul_ps(a, b);
        bad = _mm_and_si128(_mm_castps_si128(_mm_cmpeq_ps(res, z)),
                            _mm_castps_si128(_mm_and_ps(_mm_cmpneq_ps(a, z), _mm_cmpneq_ps(b, z))));
        break;
    case OP_MADD: case OP_MSUB: {
        __m128 acc = vf_in4(_mm_loadu_si128((const __m128i *)vu1.acc.u)), p = _mm_mul_ps(a, b);
        bad = _mm_or_si128(big4(p), _mm_and_si128(_mm_castps_si128(_mm_cmpeq_ps(p, z)),
                                                 _mm_castps_si128(_mm_and_ps(_mm_cmpneq_ps(a, z), _mm_cmpneq_ps(b, z)))));
        if (op == OP_MADD) {
            res = _mm_add_ps(acc, p);
            bad = _mm_or_si128(bad, _mm_andnot_si128(_mm_castps_si128(_mm_cmpeq_ps(acc, _mm_sub_ps(z, p))),
                                                     _mm_castps_si128(_mm_cmpeq_ps(res, z))));
        } else {
            res = _mm_sub_ps(acc, p);
            bad = _mm_or_si128(bad, _mm_andnot_si128(_mm_castps_si128(_mm_cmpeq_ps(acc, p)),
                                                     _mm_castps_si128(_mm_cmpeq_ps(res, z))));
        }
        break;
    }
    default: return 0;
    }
    bad = _mm_or_si128(bad, big4(res));
    if (_mm_movemask_ps(_mm_castsi128_ps(bad)) & lanes) return 0;
    {
        __m128i keep = _mm_set_epi32(lanes & 8 ? -1 : 0, lanes & 4 ? -1 : 0, lanes & 2 ? -1 : 0, lanes & 1 ? -1 : 0);
        _mm_storeu_si128((__m128i *)r->val.u, _mm_and_si128(_mm_castps_si128(res), keep));
    }
    zm = _mm_movemask_ps(_mm_cmpeq_ps(res, z)) & lanes;
    sm = _mm_movemask_ps(res) & lanes;
    r->mac = (u32)rev4[zm] | ((u32)rev4[sm] << 4);
    return 1;
}

static __forceinline void fmac(upper_res *r, int op, u32 dest, const vu_vec *fs, const float *tv, int to_acc, int reg)
{
    int i;
    r->dest = dest; r->mac = 0; r->sets_mac = (op != OP_MAX && op != OP_MINI);
    r->writes_vf = !to_acc && reg != 0; r->writes_acc = to_acc; r->reg = reg;
    if (op <= OP_MUL && fmac_vec(r, op, dest, fs, tv)) return;
    for (i = 0; i < 4; i++) {
        float a, b;
        double res = 0;
        u32 fl = 0;
        if (!HASC(dest, i)) { r->val.u[i] = 0; continue; }
        a = vf_in(fs->u[i]); b = tv[i];
        if (op <= OP_MUL && fast_lane(op, a, b, (op == OP_MADD || op == OP_MSUB) ? vf_in(vu1.acc.u[i]) : 0.0f,
                                      &r->val.u[i], &fl)) {
            r->mac |= mac_bits(i, fl);
            continue;
        }
        switch (op) {
        case OP_ADD:  res = (double)a + b; break;
        case OP_SUB:  res = (double)a - b; break;
        case OP_MUL:  res = (double)a * b; break;
        case OP_MADD: { u32 mf, mu = vd_out((double)a * b, &mf); res = (double)vf_in(vu1.acc.u[i]) + vf_in(mu); break; }
        case OP_MSUB: { u32 mf, mu = vd_out((double)a * b, &mf); res = (double)vf_in(vu1.acc.u[i]) - vf_in(mu); break; }
        case OP_MAX: case OP_MINI: {
            /* compare as VU floats; keep the exact bits of the chosen operand */
            u32 su = fs->u[i], tu;
            int pick_s = (op == OP_MAX) ? (a >= b) : (a <= b);
            memcpy(&tu, &tv[i], 4);
            r->val.u[i] = pick_s ? su : tu;
            continue;
        }
        }
        r->val.u[i] = vd_out(res, &fl);
        r->mac |= mac_bits(i, fl);
    }
}

static __forceinline void exec_upper(u32 w, upper_res *r, int *reads, int *nreads)
{
    u32 op = w & 0x3F, dest = DEST(w), ft = FT(w), fs = FS(w), fd = FD(w);
    float tv[4];
    int i;
    const vu_vec *vs = &vu1.vf[fs], *vt = &vu1.vf[ft];
    r->writes_vf = r->writes_acc = r->sets_mac = r->sets_clip = 0;
    *nreads = 0;
#define BCAST(x) do { float _b = vf_in(x); tv[0] = tv[1] = tv[2] = tv[3] = _b; } while (0)
#define FULLT() _mm_storeu_ps(tv, vf_in4_exact(_mm_loadu_si128((const __m128i *)vt->u)))
#define RD(x) (reads[(*nreads)++] = (x))
    if (op < 0x3C) {
        u32 bc = w & 3;
        int kind = -1, acc = 0;
        RD(fs);
        if (op < 0x1C) {
            BCAST(vt->u[bc]); RD(ft);
            switch (op >> 2) {
            case 0: kind = OP_ADD; break; case 1: kind = OP_SUB; break; case 2: kind = OP_MADD; break;
            case 3: kind = OP_MSUB; break; case 4: kind = OP_MAX; break; case 5: kind = OP_MINI; break;
            case 6: kind = OP_MUL; break;
            }
        } else {
            switch (op) {
            case 0x1C: BCAST(vu1.q); kind = OP_MUL; break;
            case 0x1D: BCAST(vu1.i); kind = OP_MAX; break;
            case 0x1E: BCAST(vu1.i); kind = OP_MUL; break;
            case 0x1F: BCAST(vu1.i); kind = OP_MINI; break;
            case 0x20: BCAST(vu1.q); kind = OP_ADD; break;
            case 0x21: BCAST(vu1.q); kind = OP_MADD; break;
            case 0x22: BCAST(vu1.i); kind = OP_ADD; break;
            case 0x23: BCAST(vu1.i); kind = OP_MADD; break;
            case 0x24: BCAST(vu1.q); kind = OP_SUB; break;
            case 0x25: BCAST(vu1.q); kind = OP_MSUB; break;
            case 0x26: BCAST(vu1.i); kind = OP_SUB; break;
            case 0x27: BCAST(vu1.i); kind = OP_MSUB; break;
            case 0x28: FULLT(); RD(ft); kind = OP_ADD; break;
            case 0x29: FULLT(); RD(ft); kind = OP_MADD; break;
            case 0x2A: FULLT(); RD(ft); kind = OP_MUL; break;
            case 0x2B: FULLT(); RD(ft); kind = OP_MAX; break;
            case 0x2C: FULLT(); RD(ft); kind = OP_SUB; break;
            case 0x2D: FULLT(); RD(ft); kind = OP_MSUB; break;
            case 0x2E: { /* OPMSUB: fd.xyz = ACC - fs.yzx * ft.zxy */
                vu_vec a; float sv[4];
                RD(ft); RD(REG_ACC);
                a.u[0] = vs->u[1]; a.u[1] = vs->u[2]; a.u[2] = vs->u[0]; a.u[3] = vs->u[3];
                sv[0] = vf_in(vt->u[2]); sv[1] = vf_in(vt->u[0]); sv[2] = vf_in(vt->u[1]); sv[3] = vf_in(vt->u[3]);
                fmac(r, OP_MSUB, dest, &a, sv, 0, (int)fd);
                return;
            }
            case 0x2F: FULLT(); RD(ft); kind = OP_MINI; break;
            default: return;
            }
        }
        if (kind == OP_MADD || kind == OP_MSUB) RD(REG_ACC);
        fmac(r, kind, dest, vs, tv, acc, (int)fd);
        return;
    }
    {
        u32 ext = ((w >> 4) & 0x7C) | (w & 3), bc = w & 3;
        int kind = -1, acc = 1;
        RD(fs);
        if (ext < 0x10 || (ext >= 0x18 && ext < 0x1C)) {
            BCAST(vt->u[bc]); RD(ft);
            switch (ext >> 2) {
            case 0: kind = OP_ADD; break; case 1: kind = OP_SUB; break; case 2: kind = OP_MADD; break;
            case 3: kind = OP_MSUB; break; case 6: kind = OP_MUL; break;
            }
        } else switch (ext) {
        case 0x10: case 0x11: case 0x12: case 0x13: { /* ITOF */
            static const float sc[4] = { 1.0f, 1.0f / 16.0f, 1.0f / 4096.0f, 1.0f / 32768.0f };
            r->writes_vf = ft != 0; r->reg = (int)ft; r->dest = dest;
            for (i = 0; i < 4; i++) r->val.f[i] = HASC(dest, i) ? (float)vs->s[i] * sc[ext & 3] : 0;
            return;
        }
        case 0x14: case 0x15: case 0x16: case 0x17: { /* FTOI */
            static const float sc[4] = { 1.0f, 16.0f, 4096.0f, 32768.0f };
            r->writes_vf = ft != 0; r->reg = (int)ft; r->dest = dest;
            for (i = 0; i < 4; i++) {
                double d = (double)vf_in(vs->u[i]) * sc[ext & 3];
                s32 v = d >= 2147483647.0 ? 0x7FFFFFFF : d <= -2147483648.0 ? (s32)0x80000000 : (s32)d;
                r->val.s[i] = HASC(dest, i) ? v : 0;
            }
            return;
        }
        case 0x1C: BCAST(vu1.q); kind = OP_MUL; break;
        case 0x1D: /* ABS */
            r->writes_vf = ft != 0; r->reg = (int)ft; r->dest = dest;
            for (i = 0; i < 4; i++) r->val.u[i] = vs->u[i] & 0x7FFFFFFFu;
            return;
        case 0x1E: BCAST(vu1.i); kind = OP_MUL; break;
        case 0x1F: { /* CLIP fs.xyz against ft.w */
            float wv = fabsf(vf_in(vt->u[3])), x = vf_in(vs->u[0]), y = vf_in(vs->u[1]), z = vf_in(vs->u[2]);
            u32 c = 0;
            RD(ft);
            if (x > wv) c |= 1; if (x < -wv) c |= 2;
            if (y > wv) c |= 4; if (y < -wv) c |= 8;
            if (z > wv) c |= 16; if (z < -wv) c |= 32;
            clip_latest = ((clip_latest << 6) | c) & 0xFFFFFF;
            r->sets_clip = 1; r->clip = clip_latest;
            return;
        }
        case 0x20: BCAST(vu1.q); kind = OP_ADD; break;
        case 0x21: BCAST(vu1.q); kind = OP_MADD; break;
        case 0x22: BCAST(vu1.i); kind = OP_ADD; break;
        case 0x23: BCAST(vu1.i); kind = OP_MADD; break;
        case 0x24: BCAST(vu1.q); kind = OP_SUB; break;
        case 0x25: BCAST(vu1.q); kind = OP_MSUB; break;
        case 0x26: BCAST(vu1.i); kind = OP_SUB; break;
        case 0x27: BCAST(vu1.i); kind = OP_MSUB; break;
        case 0x28: FULLT(); RD(ft); kind = OP_ADD; break;
        case 0x29: FULLT(); RD(ft); kind = OP_MADD; break;
        case 0x2A: FULLT(); RD(ft); kind = OP_MUL; break;
        case 0x2C: FULLT(); RD(ft); kind = OP_SUB; break;
        case 0x2D: FULLT(); RD(ft); kind = OP_MSUB; break;
        case 0x2E: { /* OPMULA: ACC.xyz = fs.yzx * ft.zxy */
            vu_vec a; float sv[4];
            RD(ft);
            a.u[0] = vs->u[1]; a.u[1] = vs->u[2]; a.u[2] = vs->u[0]; a.u[3] = vs->u[3];
            sv[0] = vf_in(vt->u[2]); sv[1] = vf_in(vt->u[0]); sv[2] = vf_in(vt->u[1]); sv[3] = vf_in(vt->u[3]);
            fmac(r, OP_MUL, dest, &a, sv, 1, 0);
            return;
        }
        case 0x2F: *nreads = 0; return; /* NOP */
        default: return;
        }
        if (kind == OP_MADD || kind == OP_MSUB) RD(REG_ACC);
        if (kind >= 0) fmac(r, kind, dest, vs, tv, acc, 0);
    }
#undef BCAST
#undef FULLT
#undef RD
}

/* ---- lower instructions ---------------------------------------------------------------------------- */

typedef struct lower_res {
    int writes_vf; int reg; vu_vec val; u32 dest;   /* delayed VF write (4-cycle result) */
    int branch; u32 target;
    int vi_reg; u16 vi_val; int writes_vi;
} lower_res;

static inline u8 *vmem(u32 qaddr) { return vu1.mem + ((qaddr * 16) & (VU1_MEM_BYTES - 1)); }

static float efu_p(u32 ext, const vu_vec *v, u32 fsf)
{
    float x = vf_in(v->u[0]), y = vf_in(v->u[1]), z = vf_in(v->u[2]), w = vf_in(v->u[3]), s = vf_in(v->u[fsf]);
    switch (ext) {
    case 0x70: return x * x + y * y + z * z;
    case 0x71: { float d = x * x + y * y + z * z; return d == 0 ? FLT_MAX : 1.0f / d; }
    case 0x72: return sqrtf(x * x + y * y + z * z);
    case 0x73: { float d = sqrtf(x * x + y * y + z * z); return d == 0 ? FLT_MAX : 1.0f / d; }
    case 0x74: return x == 0 ? 0 : atanf(y / x);
    case 0x75: return x == 0 ? 0 : atanf(z / x);
    case 0x76: return x + y + z + w;
    case 0x78: return sqrtf(fabsf(s));
    case 0x79: { float d = sqrtf(fabsf(s)); return d == 0 ? FLT_MAX : 1.0f / d; }
    case 0x7A: return s == 0 ? FLT_MAX : 1.0f / s;
    case 0x7C: return sinf(s);
    case 0x7D: return atanf(s);
    case 0x7E: return expf(-s);
    default: return 0;
    }
}

static int efu_latency(u32 ext)
{
    switch (ext) {
    case 0x70: return 11; case 0x71: return 18; case 0x72: return 18; case 0x73: return 24;
    case 0x74: case 0x75: case 0x7D: return 54; case 0x76: return 12; case 0x78: return 12;
    case 0x79: return 18; case 0x7A: return 12; case 0x7C: return 29; case 0x7E: return 44;
    default: return 1;
    }
}

static __forceinline void exec_lower(u32 w, u32 pc, lower_res *r, int *reads, int *nreads, cyc_t *cycle)
{
    u32 op = w >> 25;
    u32 it = FT(w), is = FS(w), dest = DEST(w);
    u16 *vi = vu1.vi;
    int i;
    r->writes_vf = r->branch = r->writes_vi = 0;
    *nreads = 0;
#define RD(x) (reads[(*nreads)++] = (int)(x))
#define SETVI(reg, v) do { r->writes_vi = (reg) != 0; r->vi_reg = (int)(reg); r->vi_val = (u16)(v); } while (0)
    if (op == 0x40) {
        u32 sop = w & 0x3F;
        if (sop < 0x3C) {
            u32 id = FD(w);
            switch (sop) {
            case 0x30: SETVI(id, vi[is] + vi[it]); break;
            case 0x31: SETVI(id, vi[is] - vi[it]); break;
            case 0x32: SETVI(it, vi[is] + imm5(w)); break;
            case 0x34: SETVI(id, vi[is] & vi[it]); break;
            case 0x35: SETVI(id, vi[is] | vi[it]); break;
            }
            return;
        }
        {
            u32 ext = ((w >> 4) & 0x7C) | (w & 3);
            u32 fsf = (w >> 21) & 3, ftf = (w >> 23) & 3;
            const vu_vec *vs = &vu1.vf[is];
            switch (ext) {
            case 0x30: /* MOVE ft, fs */
                RD(is);
                r->writes_vf = it != 0; r->reg = (int)it; r->dest = dest; r->val = *vs;
                break;
            case 0x31: /* MR32 */
                RD(is);
                r->writes_vf = it != 0; r->reg = (int)it; r->dest = dest;
                r->val.u[0] = vs->u[1]; r->val.u[1] = vs->u[2]; r->val.u[2] = vs->u[3]; r->val.u[3] = vs->u[0];
                break;
            case 0x34: /* LQI ft, (is++) */
                r->writes_vf = it != 0; r->reg = (int)it; r->dest = dest; memcpy(&r->val, vmem(vi[is]), 16);
                SETVI(is, vi[is] + 1);
                break;
            case 0x35: /* SQI fs, (it++) */
                RD(is);
                { u32 *m = (u32 *)vmem(vi[it]); for (i = 0; i < 4; i++) if (HASC(dest, i)) m[i] = vs->u[i]; }
                SETVI(it, vi[it] + 1);
                break;
            case 0x36: /* LQD ft, (--is) */
                SETVI(is, vi[is] - 1);
                r->writes_vf = it != 0; r->reg = (int)it; r->dest = dest; memcpy(&r->val, vmem((u16)(vi[is] - 1)), 16);
                break;
            case 0x37: /* SQD fs, (--it) */
                RD(is);
                { u32 *m = (u32 *)vmem((u16)(vi[it] - 1)); for (i = 0; i < 4; i++) if (HASC(dest, i)) m[i] = vs->u[i]; }
                SETVI(it, vi[it] - 1);
                break;
            case 0x38: case 0x39: case 0x3A: { /* DIV / SQRT / RSQRT */
                float a = vf_in(vs->u[fsf]), b = vf_in(vu1.vf[it].u[ftf]), res;
                u32 fl = 0, out, dummy;
                RD(is); RD(it);
                if (q_busy) { if (CYC_GT(q_ready, *cycle)) *cycle = q_ready; flags_update(*cycle); }
                if (ext == 0x38) {
                    if (b == 0) { fl = (a == 0) ? 0x10 : 0x20; res = ((vs->u[fsf] ^ vu1.vf[it].u[ftf]) & 0x80000000u) ? -FLT_MAX : FLT_MAX; }
                    else res = a / b;
                } else if (ext == 0x39) {
                    if (b < 0) fl = 0x10;
                    res = sqrtf(fabsf(b));
                } else {
                    float sq = sqrtf(fabsf(b));
                    if (b < 0) fl = 0x10;
                    if (sq == 0) { fl = (a == 0) ? 0x10 : 0x20; res = (vs->u[fsf] & 0x80000000u) ? -FLT_MAX : FLT_MAX; }
                    else res = a / sq;
                }
                out = vf_out(res, &dummy);
                if (fabsf(res) >= FLT_MAX) out = (out & 0x80000000u) | 0x7FFFFFFFu;
                q_pending = out; qflag_pending = fl; q_busy = 1;
                q_ready = *cycle + (ext == 0x3A ? 13 : 7);
                break;
            }
            case 0x3B: /* WAITQ */
                if (q_busy && CYC_GT(q_ready, *cycle)) *cycle = q_ready;
                break;
            case 0x3C: /* MTIR it, fs.fsf */
                RD(is);
                SETVI(it, vs->u[fsf] & 0xFFFF);
                break;
            case 0x3D: /* MFIR ft, is */
                r->writes_vf = it != 0; r->reg = (int)it; r->dest = dest;
                for (i = 0; i < 4; i++) r->val.s[i] = (s16)vi[is];
                break;
            case 0x3E: /* ILWR it, (is) */
                { u32 *m = (u32 *)vmem(vi[is]); for (i = 0; i < 4; i++) if (HASC(dest, i)) { SETVI(it, m[i] & 0xFFFF); break; } }
                break;
            case 0x3F: /* ISWR it, (is) */
                { u32 *m = (u32 *)vmem(vi[is]); for (i = 0; i < 4; i++) if (HASC(dest, i)) m[i] = vi[it]; }
                break;
            case 0x40: case 0x41: { /* RNEXT / RGET */
                if (ext == 0x40) {
                    u32 x = vu1.r;
                    int bit = ((x >> 4) ^ (x >> 22)) & 1;
                    x = (x << 1) | (u32)bit;
                    vu1.r = (x & 0x7FFFFF) | 0x3F800000u;
                }
                r->writes_vf = it != 0; r->reg = (int)it; r->dest = dest;
                for (i = 0; i < 4; i++) r->val.u[i] = vu1.r;
                break;
            }
            case 0x42: RD(is); vu1.r = (vs->u[fsf] & 0x7FFFFF) | 0x3F800000u; break;       /* RINIT */
            case 0x43: RD(is); vu1.r = ((vu1.r ^ vs->u[fsf]) & 0x7FFFFF) | 0x3F800000u; break; /* RXOR */
            case 0x64: /* MFP */
                r->writes_vf = it != 0; r->reg = (int)it; r->dest = dest;
                for (i = 0; i < 4; i++) r->val.u[i] = vu1.p;
                break;
            case 0x68: SETVI(it, vif1.top); break;
            case 0x69: SETVI(it, vif1.itop); break;
            case 0x6C: { /* XGKICK is */
                u32 a = (vi[is] * 16u) & (VU1_MEM_BYTES - 1);
                /* PATH1 reads a whole packet (until EOP) straight from VU1 memory, wrapping at its end */
                u32 n = (VU1_MEM_BYTES - a) / 16;
                if (xk_mode) {   /* lockstep check: hash the packet as PATH1 would read it */
                    u32 q = a, k, j;
                    for (k = 0; k < 64; k++) {
                        const u32 *t = (const u32 *)(vu1.mem + q);
                        u32 nl = t[0] & 0x7FFF, flg = (t[1] >> 26) & 3, nreg = (t[1] >> 28) ? (t[1] >> 28) : 16;
                        u32 len = flg == 0 ? nl * nreg : flg == 1 ? (nl * nreg + 1) / 2 : nl, eop = (t[0] >> 15) & 1;
                        for (j = 0; j < (1 + len) * 4; j++)
                            xk_hash = (xk_hash ^ ((const u32 *)vu1.mem)[(q / 4 + j) & (VU1_MEM_BYTES / 4 - 1)]) * 16777619u;
                        q = (q + (1 + len) * 16) & (VU1_MEM_BYTES - 1);
                        if (eop) break;
                    }
                    if (xk_mode == 2) break;
                }
                if (gif_write(GIF_PATH1, (const qword *)(vu1.mem + a), n) == n && !gif_path_ended(GIF_PATH1) && a)
                    gif_write(GIF_PATH1, (const qword *)vu1.mem, a / 16);
                break;
            }
            case 0x7B: /* WAITP */
                if (p_busy && CYC_GT(p_ready, *cycle)) *cycle = p_ready;
                break;
            default:
                if (ext >= 0x70 && ext <= 0x7E) {
                    u32 dummy;
                    RD(is);
                    if (p_busy) { if (CYC_GT(p_ready, *cycle)) *cycle = p_ready; flags_update(*cycle); }
                    p_pending = vf_out(efu_p(ext, vs, fsf), &dummy);
                    p_ready = *cycle + (cyc_t)efu_latency(ext);
                    p_busy = 1;
                }
                break;
            }
        }
        return;
    }
    switch (op) {
    case 0x00: /* LQ ft, imm(is) */
        r->writes_vf = it != 0; r->reg = (int)it; r->dest = dest;
        memcpy(&r->val, vmem((u16)(vi[is] + imm11(w))), 16);
        break;
    case 0x01: /* SQ fs, imm(it) */
        RD(is);
        { u32 *m = (u32 *)vmem((u16)(vi[it] + imm11(w))); for (i = 0; i < 4; i++) if (HASC(dest, i)) m[i] = vu1.vf[is].u[i]; }
        break;
    case 0x04: /* ILW it, imm(is) */
        { u32 *m = (u32 *)vmem((u16)(vi[is] + imm11(w))); for (i = 0; i < 4; i++) if (HASC(dest, i)) { SETVI(it, m[i] & 0xFFFF); break; } }
        break;
    case 0x05: /* ISW it, imm(is) */
        { u32 *m = (u32 *)vmem((u16)(vi[is] + imm11(w))); for (i = 0; i < 4; i++) if (HASC(dest, i)) m[i] = vi[it]; }
        break;
    case 0x08: SETVI(it, vi[is] + imm15(w)); break;
    case 0x09: SETVI(it, vi[is] - imm15(w)); break;
    case 0x10: flags_update(*cycle); SETVI(1, (vu1.clip & 0xFFFFFF) == imm24(w)); break;
    case 0x11: clip_latest = imm24(w); flags_push(*cycle + 4, 0, 0, 1, clip_latest); break;
    case 0x12: flags_update(*cycle); SETVI(1, (vu1.clip & imm24(w)) != 0); break;
    case 0x13: flags_update(*cycle); SETVI(1, ((vu1.clip | imm24(w)) & 0xFFFFFF) == 0xFFFFFF); break;
    case 0x14: flags_update(*cycle); SETVI(it, (vu1.status & 0xFFF) == imm12(w)); break;
    case 0x15: flags_update(*cycle); vu1.status = (vu1.status & 0x3F) | (imm12(w) & 0xFC0); break;
    case 0x16: flags_update(*cycle); SETVI(it, vu1.status & imm12(w)); break;
    case 0x17: flags_update(*cycle); SETVI(it, ((vu1.status | imm12(w)) & 0xFFF) == 0xFFF); break;
    case 0x18: flags_update(*cycle); SETVI(it, (vu1.mac & 0xFFFF) == vi[is]); break;
    case 0x1A: flags_update(*cycle); SETVI(it, vu1.mac & vi[is]); break;
    case 0x1B: flags_update(*cycle); SETVI(it, ((vu1.mac | vi[is]) & 0xFFFF) == 0xFFFF); break;
    case 0x1C: flags_update(*cycle); SETVI(it, vu1.clip & 0xFFF); break;
    case 0x20: r->branch = 1; r->target = pc + 1 + (u32)imm11(w); break;
    case 0x21: r->branch = 1; r->target = pc + 1 + (u32)imm11(w); SETVI(it, pc + 2); break;
    case 0x24: r->branch = 1; r->target = vi[is]; break;
    case 0x25: r->branch = 1; r->target = vi[is]; SETVI(it, pc + 2); break;
    case 0x28: if (vi[it] == vi[is]) { r->branch = 1; r->target = pc + 1 + (u32)imm11(w); } break;
    case 0x29: if (vi[it] != vi[is]) { r->branch = 1; r->target = pc + 1 + (u32)imm11(w); } break;
    case 0x2C: if ((s16)vi[is] < 0) { r->branch = 1; r->target = pc + 1 + (u32)imm11(w); } break;
    case 0x2D: if ((s16)vi[is] > 0) { r->branch = 1; r->target = pc + 1 + (u32)imm11(w); } break;
    case 0x2E: if ((s16)vi[is] <= 0) { r->branch = 1; r->target = pc + 1 + (u32)imm11(w); } break;
    case 0x2F: if ((s16)vi[is] >= 0) { r->branch = 1; r->target = pc + 1 + (u32)imm11(w); } break;
    default: break;
    }
#undef RD
#undef SETVI
}

/* ---- the run loop ---------------------------------------------------------------------------------- */

static void write_vec(vu_vec *d, const vu_vec *s, u32 dest)
{
    int i;
    for (i = 0; i < 4; i++) if (HASC(dest, i)) d->u[i] = s->u[i];
}

/* One instruction pair: everything the hardware does in its cycle(s). The interpreter calls it in a loop; the
 * translated programs (vu1x/) call it with each address's constant instruction words, so the compiler specializes
 * it per instruction - same code, same results. Returns 1 when the program has ended. */
typedef struct runst {
    u32 pc, branch_target;
    int ebit_pending, branch_pending, guard;
    cyc_t cycle, start;
} runst;

/* skip: VF registers (bit 32 = ACC) whose stall check the translator proved unnecessary at this pair; 0 = check all.
 * lin: the translator proved no branch or E-bit is pending on entry and this pair has neither (the next pc is pc+1).
 * nomac: the translator proved no instruction ever sees this pair's MAC result (gen_vu1x.py dead_mac): not queued. */
/* the upper instruction broadcasts Q (MULq ADDq MADDq SUBq MSUBq and their ACC forms) */
static __forceinline int upper_reads_q(u32 hi)
{
    u32 op = hi & 0x3F, ext = ((hi >> 4) & 0x7C) | (hi & 3);
    if (op < 0x3C) return op == 0x1C || op == 0x20 || op == 0x21 || op == 0x24 || op == 0x25;
    return ext == 0x1C || ext == 0x20 || ext == 0x21 || ext == 0x24 || ext == 0x25;
}

static __forceinline int pair_step(runst *st, u32 pc, u32 lo, u32 hi, u64 skip, int lin, int nomac)
{
    upper_res ur;
    lower_res lr;
    int ureads[4], lreads[4], nu = 0, nl = 0, k;
    cyc_t stall_to, cyc = st->cycle;   /* a local: the build's no-strict-aliasing would reload it after every store */

    /* NOP / NOP (common padding): no reads, no writes, one cycle */
    if ((hi & 0x87FFFFFFu) == 0x000002FFu && lo == 0x8000033Cu) {
        st->cycle = cyc + 1;
        lr.branch = 0;
        goto advance;
    }

    /* decode reads first (both units read the register file before either writes). Q and P land lazily like the
     * flags: only a pair that reads them brings them up to date here; DIV/EFU/WAITQ/WAITP and the flag reads see to
     * their own, and run_end lands the rest. */
    if (upper_reads_q(hi) || (!(hi & 0x80000000u) && lo >> 25 == 0x40 && (((lo >> 4) & 0x7C) | (lo & 3)) == 0x64 &&
                              (lo & 0x3F) >= 0x3C))
        qp_update(cyc);
    exec_upper(hi, &ur, ureads, &nu);
    if (!(hi & 0x80000000u))
        exec_lower(lo, pc, &lr, lreads, &nl, &cyc);
    else
        lr.writes_vf = lr.branch = lr.writes_vi = 0;
    /* stalls on not-yet-ready VF sources (not ACC: the FMAC pipeline forwards it, so MULA/MADDA/MADD chains issue
     * back to back; stalling them made the battle clip program read the sign flag of a later instruction, BUG-9) */
    stall_to = cyc;
    for (k = 0; k < nu; k++)
        if (ureads[k] && ureads[k] != REG_ACC && !((skip >> ureads[k]) & 1) && CYC_GT(vf_ready[ureads[k]], stall_to))
            stall_to = vf_ready[ureads[k]];
    for (k = 0; k < nl; k++)
        if (lreads[k] && !((skip >> lreads[k]) & 1) && CYC_GT(vf_ready[lreads[k]], stall_to)) stall_to = vf_ready[lreads[k]];
    cyc = stall_to;
    /* the pair issues when its stalls end (a WAITQ in the lower slot included): an upper instruction that reads Q
     * sees a DIV result that landed while it waited (the battle clipper's MULq + WAITQ pair, BUG-11) */
    if (q_busy && upper_reads_q(hi) && CYC_LE(q_ready, cyc)) {
        qp_update(cyc);
        nu = 0;
        exec_upper(hi, &ur, ureads, &nu);
    }

    /* write back (writes to VF0 and VI0 are never made: every result path skips register 0) */
    if (ur.writes_vf) { write_vec(&vu1.vf[ur.reg], &ur.val, ur.dest); vf_ready[ur.reg] = cyc + 4; }
    if (ur.writes_acc) { write_vec(&vu1.acc, &ur.val, ur.dest); vf_ready[REG_ACC] = cyc + 4; }
    if (nomac) ur.sets_mac = 0;
    if (ur.sets_mac || ur.sets_clip)
        flags_push(cyc + 4, ur.sets_mac, ur.mac, ur.sets_clip, ur.clip);
    if (lr.writes_vf && !(ur.writes_vf && ur.reg == lr.reg)) {
        write_vec(&vu1.vf[lr.reg], &lr.val, lr.dest);
        vf_ready[lr.reg] = cyc + 4;
    }
    if (lr.writes_vi) vu1.vi[lr.vi_reg] = lr.vi_val;
    if (hi & 0x80000000u) vu1.i = lo;
    st->cycle = cyc + 1;

advance:
    if (lin) {
        st->pc = (pc + 1) & 0x7FF;
        if (++st->guard > 4000000) { hw_log(0, "vu1: program at %03x did not end", vu1.tpc); return 1; }
        return 0;
    }
    if (st->ebit_pending) { st->pc = st->branch_pending ? st->branch_target & 0x7FF : (pc + 1) & 0x7FF; return 1; }
    if (hi & 0x40000000u) st->ebit_pending = 1;
    if (st->branch_pending) { st->pc = st->branch_target & 0x7FF; st->branch_pending = 0; }
    else st->pc = (pc + 1) & 0x7FF;
    if (lr.branch) { st->branch_pending = 1; st->branch_target = lr.target; }
    if (++st->guard > 4000000) { hw_log(0, "vu1: program at %03x did not end", vu1.tpc); return 1; }
    return 0;
}

static unsigned int run_begin(runst *st)
{
    unsigned int csr = _mm_getcsr();
    /* round toward zero, FTZ, DAZ, and every exception masked: the game thread arms SSE traps for the EE's float
     * rules (runtime/eefloat.c), but VU results are checked and clamped here (fast_lane falls back on any overflow),
     * and a trap per overflowing lane made overflow-heavy scenes crawl (230 ms pictures in a story scene) */
    _mm_setcsr((csr & ~0x6000u & ~0x3Fu) | 0x6000u | 0x8040u | 0x1F80u);
    memset(st, 0, sizeof *st);
    st->pc = vu1.tpc & 0x7FF;
    st->cycle = st->start = (cyc_t)vu1.cycles;
    vu1.vf[0].u[0] = vu1.vf[0].u[1] = vu1.vf[0].u[2] = 0; vu1.vf[0].f[3] = 1.0f;
    vu1.vi[0] = 0;
    {   /* ready times already past are "now": keeps every stored time close to the cycle counter */
        int i;
        for (i = 0; i < 33; i++) if (CYC_GT(st->cycle, vf_ready[i])) vf_ready[i] = st->cycle;
    }
    vu1.running = 1;
    return csr;
}

/* DOTR_VU1_STATS=<fields>: every that many fields, the VU1 programs run, instruction pairs executed and cycles */
static u64 st_runs, st_pairs, st_cycles;
void vu1_stats_field(void)
{
    static int every = -1;
    static u32 n;
    if (every < 0) every = getenv("DOTR_VU1_STATS") ? atoi(getenv("DOTR_VU1_STATS")) : 0;
    if (every <= 0 || ++n % (u32)every) return;
    hw_log(2, "vu1 stats: %u fields: %llu programs, %llu pairs, %llu cycles (%.0f pairs, %.0f cycles per field)", (u32)every,
           st_runs, st_pairs, st_cycles, (double)st_pairs / every, (double)st_cycles / every);
    st_runs = st_pairs = st_cycles = 0;
}

static void run_end(const runst *st, unsigned int csr)
{
    st_runs++; st_pairs += (u32)st->guard; st_cycles += (u32)(st->cycle - st->start);
    flags_update_all();   /* let pending results land */
    vu1.cycles += (u32)(st->cycle - st->start);
    vu1.tpc = st->pc;
    vu1.running = 0;
    _mm_setcsr(csr);
}

void vu1_disasm(u32 hi, u32 lo, char *out, size_t outsz);

/* DOTR_VU1_TRACE=<n> (with DOTR_VU1X=0): logs each program run's number and start, and for run n every instruction
 * pair with the registers and data memory it changed (digging into one batch of a capture replay) */
static void run_traced(runst *st)
{
    static vu1_state before;
    char d[160];
    for (;;) {
        u32 lo, hi, pc = st->pc, k;
        int stop;
        memcpy(&lo, vu1.micro + pc * 8, 4);
        memcpy(&hi, vu1.micro + pc * 8 + 4, 4);
        before = vu1;
        stop = pair_step(st, pc, lo, hi, 0, 0, 0);
        vu1_disasm(hi, lo, d, sizeof d);
        hw_log(2, "vu1t %04x c%llu %s", pc * 8, (unsigned long long)(st->cycle - st->start), d);
        for (k = 0; k < 32; k++)
            if (memcmp(&before.vf[k], &vu1.vf[k], 16))
                hw_log(2, "vu1t     vf%02u = %g %g %g %g  (%08x %08x %08x %08x)", k, vu1.vf[k].f[0], vu1.vf[k].f[1],
                       vu1.vf[k].f[2], vu1.vf[k].f[3], vu1.vf[k].u[0], vu1.vf[k].u[1], vu1.vf[k].u[2], vu1.vf[k].u[3]);
        for (k = 0; k < 16; k++)
            if (before.vi[k] != vu1.vi[k]) hw_log(2, "vu1t     vi%02u = %04x", k, vu1.vi[k]);
        if (before.clip != vu1.clip) hw_log(2, "vu1t     clip = %06x", vu1.clip);
        for (k = 0; k < VU1_MEM_BYTES; k += 16)
            if (memcmp(before.mem + k, vu1.mem + k, 16)) {
                const float *f = (const float *)(vu1.mem + k);
                const u32 *w = (const u32 *)(vu1.mem + k);
                hw_log(2, "vu1t     mem %03x = %g %g %g %g  (%08x %08x %08x %08x)", k / 16, f[0], f[1], f[2], f[3],
                       w[0], w[1], w[2], w[3]);
            }
        if (stop) break;
    }
}

static void run_interp(void)
{
    runst st;
    unsigned int csr = run_begin(&st);
    static long trace_run = -2, run_no;
    if (trace_run == -2) trace_run = getenv("DOTR_VU1_TRACE") ? atol(getenv("DOTR_VU1_TRACE")) : -1;
    if (trace_run >= 0) {
        hw_log(2, "vu1t run %ld at %04x", run_no, st.pc * 8);
        if (run_no++ == trace_run) { run_traced(&st); run_end(&st, csr); return; }
    }
    for (;;) {
        u32 lo, hi;
        memcpy(&lo, vu1.micro + st.pc * 8, 4);
        memcpy(&hi, vu1.micro + st.pc * 8 + 4, 4);
        if (pair_step(&st, st.pc, lo, hi, 0, 0, 0)) break;
    }
    run_end(&st, csr);
}

#include "vu1x/vu1x_glue.h"

void vu1_run(u32 start_pc) { vu1.tpc = start_pc & 0x7FF; vu1x_run(); }
void vu1_continue(void) { vu1x_run(); }

/* ---- disassembler ---------------------------------------------------------------------------------- */

static const char *dests(u32 d)
{
    static char b[8][6]; static int k;
    char *s = b[k++ & 7], *p = s;
    if (d & 8) *p++ = 'x'; if (d & 4) *p++ = 'y'; if (d & 2) *p++ = 'z'; if (d & 1) *p++ = 'w';
    *p = 0;
    return s;
}

void vu1_disasm(u32 hi, u32 lo, char *out, size_t outsz)
{
    static const char *bcn = "xyzw";
    static const char *u3c[0x30] = {
        "ADDA", "ADDA", "ADDA", "ADDA", "SUBA", "SUBA", "SUBA", "SUBA", "MADDA", "MADDA", "MADDA", "MADDA",
        "MSUBA", "MSUBA", "MSUBA", "MSUBA", "ITOF0", "ITOF4", "ITOF12", "ITOF15", "FTOI0", "FTOI4", "FTOI12", "FTOI15",
        "MULA", "MULA", "MULA", "MULA", "MULAq", "ABS", "MULAi", "CLIP", "ADDAq", "MADDAq", "ADDAi", "MADDAi",
        "SUBAq", "MSUBAq", "SUBAi", "MSUBAi", "ADDA", "MADDA", "MULA", "?", "SUBA", "MSUBA", "OPMULA", "NOP" };
    static const char *u0[0x30] = {
        "ADD", "ADD", "ADD", "ADD", "SUB", "SUB", "SUB", "SUB", "MADD", "MADD", "MADD", "MADD", "MSUB", "MSUB", "MSUB", "MSUB",
        "MAX", "MAX", "MAX", "MAX", "MINI", "MINI", "MINI", "MINI", "MUL", "MUL", "MUL", "MUL", "MULq", "MAXi", "MULi", "MINIi",
        "ADDq", "MADDq", "ADDi", "MADDi", "SUBq", "MSUBq", "SUBi", "MSUBi", "ADD", "MADD", "MUL", "MAX", "SUB", "MSUB", "OPMSUB", "MINI" };
    char ub[64], lb[64];
    u32 op = hi & 0x3F;
    if (op < 0x30) {
        if (op < 0x1C) snprintf(ub, sizeof ub, "%s%c.%s vf%02u, vf%02u, vf%02u%c", u0[op], bcn[op & 3], dests(DEST(hi)), FD(hi), FS(hi), FT(hi), bcn[op & 3]);
        else snprintf(ub, sizeof ub, "%s.%s vf%02u, vf%02u, vf%02u", u0[op], dests(DEST(hi)), FD(hi), FS(hi), FT(hi));
    } else if (op >= 0x3C) {
        u32 ext = ((hi >> 4) & 0x7C) | (hi & 3);
        if (ext < 0x30) snprintf(ub, sizeof ub, "%s.%s vf%02u, vf%02u", u3c[ext], dests(DEST(hi)), FT(hi), FS(hi));
        else snprintf(ub, sizeof ub, "?u%02x", ext);
    } else snprintf(ub, sizeof ub, "?u%02x", op);
    if (hi & 0x80000000u) { float f; memcpy(&f, &lo, 4); snprintf(lb, sizeof lb, "LOI %g", (double)f); }
    else {
        static const char *l1[0x30] = {   /* lo >> 25 */
            "LQ", "SQ", 0, 0, "ILW", "ISW", 0, 0, "IADDIU", "ISUBIU", 0, 0, 0, 0, 0, 0,
            "FCEQ", "FCSET", "FCAND", "FCOR", "FSEQ", "FSSET", "FSAND", "FSOR", "FMEQ", 0, "FMAND", "FMOR", "FCGET", 0, 0, 0,
            "B", "BAL", 0, 0, "JR", "JALR", 0, 0, "IBEQ", "IBNE", 0, 0, "IBLTZ", "IBGTZ", "IBLEZ", "IBGEZ" };
        static const char *s1[0x40] = { [0x30] = "IADD", [0x31] = "ISUB", [0x32] = "IADDI", [0x34] = "IAND", [0x35] = "IOR" };
        static const char *s2[0x80] = { [0x30] = "MOVE", [0x31] = "MR32", [0x34] = "LQI", [0x35] = "SQI", [0x36] = "LQD",
            [0x37] = "SQD", [0x38] = "DIV", [0x39] = "SQRT", [0x3A] = "RSQRT", [0x3B] = "WAITQ", [0x3C] = "MTIR",
            [0x3D] = "MFIR", [0x3E] = "ILWR", [0x3F] = "ISWR", [0x40] = "RNEXT", [0x41] = "RGET", [0x42] = "RINIT",
            [0x43] = "RXOR", [0x64] = "MFP", [0x68] = "XTOP", [0x69] = "XITOP", [0x6C] = "XGKICK", [0x70] = "ESADD",
            [0x71] = "ERSADD", [0x72] = "ELENG", [0x73] = "ERLENG", [0x74] = "EATANxy", [0x75] = "EATANxz",
            [0x76] = "ESUM", [0x78] = "ESQRT", [0x79] = "ERSQRT", [0x7A] = "ERCPR", [0x7B] = "WAITP", [0x7C] = "ESIN",
            [0x7D] = "EATAN", [0x7E] = "EEXP" };
        u32 lop = lo >> 25, e2 = ((lo >> 4) & 0x7C) | (lo & 3), fsf = (lo >> 21) & 3, ftf = (lo >> 23) & 3;
        const char *nm = lop < 0x30 ? l1[lop] : lop == 0x40 ? ((lo & 0x3F) < 0x3C ? s1[lo & 0x3F] : s2[e2]) : 0;
        (void)fsf; (void)ftf;
        if (lo == 0x8000033Cu) snprintf(lb, sizeof lb, "NOP");
        else if (nm && lop == 0x40 && (lo & 0x3F) < 0x3C) snprintf(lb, sizeof lb, "%s vi%02u, vi%02u, vi%02u", nm, FD(lo), FS(lo), FT(lo));
        else if (nm && lop == 0x40) snprintf(lb, sizeof lb, "%s.%s ft=%u fs=%u (fsf %c ftf %c)", nm, dests(DEST(lo)), FT(lo), FS(lo), bcn[fsf], bcn[ftf]);
        else if (nm) snprintf(lb, sizeof lb, "%s.%s it=%u is=%u imm=%d imm24=%06x", nm, dests(DEST(lo)), FT(lo), FS(lo), imm11(lo), lo & 0xFFFFFF);
        else if (lop == 0x40) snprintf(lb, sizeof lb, "lower40 ext=%02x it=%u is=%u", e2, FT(lo), FS(lo));
        else snprintf(lb, sizeof lb, "lower%02x it=%u is=%u imm=%d", lop, FT(lo), FS(lo), imm11(lo));
    }
    snprintf(out, outsz, "%-36s %s%s", ub, lb, (hi & 0x40000000u) ? "  [E]" : "");
}
