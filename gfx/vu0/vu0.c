/* vu0.c - VU0 macro mode: COP2 instructions executed one at a time with immediate results (the EE interlocks,
 * so no pipeline is visible to macro-mode code). Same float semantics as VU1 (vu_math.h). */
#include <math.h>
#include <float.h>
#include "vu0.h"
#include "../vu_math.h"

vu0_state vu0;
ee_regs ee;
__declspec(align(16)) u8 vu0_stack[0x10000];
int vu0_depth;
unsigned int vu0_host_csr;

void vu0_reset(void)
{
    memset(&vu0, 0, sizeof vu0);
    vu0.vf[0].f[3] = 1.0f;
    vu0.q = 0x3F800000u;
    vu0.r = 0x3F800000u;
}

#define DEST(w)  (((w) >> 21) & 0xF)
#define FT(w)    (((w) >> 16) & 0x1F)
#define FS(w)    (((w) >> 11) & 0x1F)
#define FD(w)    (((w) >> 6) & 0x1F)
#define HASC(d, i) (((d) >> (3 - (i))) & 1)

enum { K_ADD, K_SUB, K_MADD, K_MSUB, K_MUL, K_MAX, K_MINI };

static void set_flags(u32 mac)
{
    u32 st = vu0.status & ~0xFu;
    if (mac & 0x000F) st |= 1;
    if (mac & 0x00F0) st |= 2;
    if (mac & 0x0F00) st |= 4;
    if (mac & 0xF000) st |= 8;
    st |= (st & 0xF) << 6;
    vu0.mac = mac;
    vu0.status = st;
}

/* fd/ACC = op(fs, t) per component; t is already broadcast/selected. */
static void fmac(int kind, u32 dest, const vu_vec *fs, const float *t, vu_vec *out)
{
    vu_vec res = *out;
    u32 mac = 0;
    int i;
    for (i = 0; i < 4; i++) {
        float a, b;
        double d = 0;
        u32 fl;
        if (!HASC(dest, i)) continue;
        a = vf_in(fs->u[i]); b = t[i];
        switch (kind) {
        case K_ADD: d = (double)a + b; break;
        case K_SUB: d = (double)a - b; break;
        case K_MUL: d = (double)a * b; break;
        case K_MADD: { u32 mf, mu = vd_out((double)a * b, &mf); d = (double)vf_in(vu0.acc.u[i]) + vf_in(mu); break; }
        case K_MSUB: { u32 mf, mu = vd_out((double)a * b, &mf); d = (double)vf_in(vu0.acc.u[i]) - vf_in(mu); break; }
        case K_MAX: case K_MINI: {
            u32 tu; memcpy(&tu, &t[i], 4);
            res.u[i] = ((kind == K_MAX) ? (a >= b) : (a <= b)) ? fs->u[i] : tu;
            continue;
        }
        }
        res.u[i] = vd_out(d, &fl);
        mac |= mac_bits(i, fl);
    }
    if (kind != K_MAX && kind != K_MINI) set_flags(mac);
    *out = res;
}

static void bcast(float *t, u32 bits) { float b = vf_in(bits); t[0] = t[1] = t[2] = t[3] = b; }
static void full(float *t, const vu_vec *v) { int i; for (i = 0; i < 4; i++) t[i] = vf_in(v->u[i]); }

static void fix0(void)
{
    vu0.vf[0].u[0] = vu0.vf[0].u[1] = vu0.vf[0].u[2] = 0;
    vu0.vf[0].f[3] = 1.0f;
    vu0.vi[0] = 0;
}

static u32 q_out(double d, int *flag)
{
    u32 fl;
    *flag = 0;
    return vd_out(d, &fl);
}

static void exec_special(u32 w)
{
    u32 ext = ((w >> 4) & 0x7C) | (w & 3), dest = DEST(w), ft = FT(w), fs = FS(w);
    u32 fsf = (w >> 21) & 3, ftf = (w >> 23) & 3, bc = w & 3;
    vu_vec *vs = &vu0.vf[fs], *vt = &vu0.vf[ft], tmp;
    float t[4];
    int i;
    if (ext < 0x10 || (ext >= 0x18 && ext < 0x1C)) {
        static const int k[7] = { K_ADD, K_SUB, K_MADD, K_MSUB, -1, -1, K_MUL };
        bcast(t, vt->u[bc]);
        fmac(k[ext >> 2], dest, vs, t, &vu0.acc);
        return;
    }
    switch (ext) {
    case 0x10: case 0x11: case 0x12: case 0x13: {
        static const float sc[4] = { 1.0f, 1.0f / 16.0f, 1.0f / 4096.0f, 1.0f / 32768.0f };
        tmp = *vt;
        for (i = 0; i < 4; i++) if (HASC(dest, i)) tmp.f[i] = (float)vs->s[i] * sc[ext & 3];
        if (ft) *vt = tmp;
        break;
    }
    case 0x14: case 0x15: case 0x16: case 0x17: {
        static const float sc[4] = { 1.0f, 16.0f, 4096.0f, 32768.0f };
        tmp = *vt;
        for (i = 0; i < 4; i++) if (HASC(dest, i)) {
            double d = (double)vf_in(vs->u[i]) * sc[ext & 3];
            tmp.s[i] = d >= 2147483647.0 ? 0x7FFFFFFF : d <= -2147483648.0 ? (s32)0x80000000 : (s32)d;
        }
        if (ft) *vt = tmp;
        break;
    }
    case 0x1C: bcast(t, vu0.q); fmac(K_MUL, dest, vs, t, &vu0.acc); break;
    case 0x1D: tmp = *vt; for (i = 0; i < 4; i++) if (HASC(dest, i)) tmp.u[i] = vs->u[i] & 0x7FFFFFFFu; if (ft) *vt = tmp; break;
    case 0x1E: bcast(t, vu0.i); fmac(K_MUL, dest, vs, t, &vu0.acc); break;
    case 0x1F: {
        float wv = fabsf(vf_in(vt->u[3])), x = vf_in(vs->u[0]), y = vf_in(vs->u[1]), z = vf_in(vs->u[2]);
        u32 c = 0;
        if (x > wv) c |= 1; if (x < -wv) c |= 2;
        if (y > wv) c |= 4; if (y < -wv) c |= 8;
        if (z > wv) c |= 16; if (z < -wv) c |= 32;
        vu0.clip = ((vu0.clip << 6) | c) & 0xFFFFFF;
        break;
    }
    case 0x20: bcast(t, vu0.q); fmac(K_ADD, dest, vs, t, &vu0.acc); break;
    case 0x21: bcast(t, vu0.q); fmac(K_MADD, dest, vs, t, &vu0.acc); break;
    case 0x22: bcast(t, vu0.i); fmac(K_ADD, dest, vs, t, &vu0.acc); break;
    case 0x23: bcast(t, vu0.i); fmac(K_MADD, dest, vs, t, &vu0.acc); break;
    case 0x24: bcast(t, vu0.q); fmac(K_SUB, dest, vs, t, &vu0.acc); break;
    case 0x25: bcast(t, vu0.q); fmac(K_MSUB, dest, vs, t, &vu0.acc); break;
    case 0x26: bcast(t, vu0.i); fmac(K_SUB, dest, vs, t, &vu0.acc); break;
    case 0x27: bcast(t, vu0.i); fmac(K_MSUB, dest, vs, t, &vu0.acc); break;
    case 0x28: full(t, vt); fmac(K_ADD, dest, vs, t, &vu0.acc); break;
    case 0x29: full(t, vt); fmac(K_MADD, dest, vs, t, &vu0.acc); break;
    case 0x2A: full(t, vt); fmac(K_MUL, dest, vs, t, &vu0.acc); break;
    case 0x2C: full(t, vt); fmac(K_SUB, dest, vs, t, &vu0.acc); break;
    case 0x2D: full(t, vt); fmac(K_MSUB, dest, vs, t, &vu0.acc); break;
    case 0x2E: { /* OPMULA: ACC.xyz = fs.yzx * ft.zxy */
        vu_vec a = *vs;
        a.u[0] = vs->u[1]; a.u[1] = vs->u[2]; a.u[2] = vs->u[0];
        t[0] = vf_in(vt->u[2]); t[1] = vf_in(vt->u[0]); t[2] = vf_in(vt->u[1]); t[3] = vf_in(vt->u[3]);
        fmac(K_MUL, dest, &a, t, &vu0.acc);
        break;
    }
    case 0x2F: break;  /* NOP */
    /* lower-unit specials */
    case 0x30: tmp = *vt; for (i = 0; i < 4; i++) if (HASC(dest, i)) tmp.u[i] = vs->u[i]; if (ft) *vt = tmp; break;   /* MOVE */
    case 0x31: { vu_vec s = *vs; tmp = *vt;                                                                            /* MR32 */
        u32 r4[4] = { s.u[1], s.u[2], s.u[3], s.u[0] };
        for (i = 0; i < 4; i++) if (HASC(dest, i)) tmp.u[i] = r4[i];
        if (ft) *vt = tmp; break; }
    case 0x34: { u32 a = (vu0.vi[fs] * 16u) & 0xFFF; tmp = *vt; for (i = 0; i < 4; i++) if (HASC(dest, i)) memcpy(&tmp.u[i], vu0.mem + a + i * 4, 4);
        if (ft) *vt = tmp; if (fs) vu0.vi[fs]++; break; }                                                             /* LQI */
    case 0x35: { u32 a = (vu0.vi[ft] * 16u) & 0xFFF; for (i = 0; i < 4; i++) if (HASC(dest, i)) memcpy(vu0.mem + a + i * 4, &vs->u[i], 4);
        if (ft) vu0.vi[ft]++; break; }                                                                                /* SQI */
    case 0x36: { if (fs) vu0.vi[fs]--; { u32 a = (vu0.vi[fs] * 16u) & 0xFFF; tmp = *vt;                               /* LQD */
        for (i = 0; i < 4; i++) if (HASC(dest, i)) memcpy(&tmp.u[i], vu0.mem + a + i * 4, 4); if (ft) *vt = tmp; } break; }
    case 0x37: { if (ft) vu0.vi[ft]--; { u32 a = (vu0.vi[ft] * 16u) & 0xFFF;                                         /* SQD */
        for (i = 0; i < 4; i++) if (HASC(dest, i)) memcpy(vu0.mem + a + i * 4, &vs->u[i], 4); } break; }
    case 0x38: case 0x39: case 0x3A: {  /* DIV, SQRT, RSQRT */
        float a = vf_in(vs->u[fsf]), b = vf_in(vt->u[ftf]);
        u32 st = vu0.status & ~0x30u;
        int f;
        if (ext == 0x38) {
            if (b == 0) { st |= (a == 0) ? 0x10 : 0x20; vu0.q = (((vs->u[fsf] ^ vt->u[ftf]) & 0x80000000u) | 0x7FFFFFFFu); }
            else vu0.q = q_out((double)a / b, &f);
        } else if (ext == 0x39) {
            if (b < 0) st |= 0x10;
            vu0.q = q_out(sqrt(fabs((double)b)), &f);
        } else {
            double sq = sqrt(fabs((double)b));
            if (b < 0) st |= 0x10;
            if (sq == 0) { st |= (a == 0) ? 0x10 : 0x20; vu0.q = (vs->u[fsf] & 0x80000000u) | 0x7FFFFFFFu; }
            else vu0.q = q_out((double)a / sq, &f);
        }
        vu0.status = st | ((st & 0x30) << 6) | (vu0.status & 0xC00);
        break;
    }
    case 0x3B: break;  /* WAITQ */
    case 0x3C: if (ft) vu0.vi[ft] = (u16)vs->u[fsf]; break;                       /* MTIR */
    case 0x3D: tmp = *vt; for (i = 0; i < 4; i++) if (HASC(dest, i)) tmp.s[i] = (s16)vu0.vi[fs]; if (ft) *vt = tmp; break; /* MFIR */
    case 0x3E: { u32 a = (vu0.vi[fs] * 16u) & 0xFFF; for (i = 0; i < 4; i++) if (HASC(dest, i)) { u32 v; memcpy(&v, vu0.mem + a + i * 4, 4);
        if (ft) vu0.vi[ft] = (u16)v; break; } break; }                                                               /* ILWR */
    case 0x3F: { u32 a = (vu0.vi[fs] * 16u) & 0xFFF; for (i = 0; i < 4; i++) if (HASC(dest, i)) { u32 v = vu0.vi[ft];
        memcpy(vu0.mem + a + i * 4, &v, 4); } break; }                                                               /* ISWR */
    case 0x40: case 0x41: {  /* RNEXT, RGET */
        if (ext == 0x40) {
            u32 x = vu0.r;
            int bit = ((x >> 4) ^ (x >> 22)) & 1;
            vu0.r = (((x << 1) | (u32)bit) & 0x7FFFFF) | 0x3F800000u;
        }
        tmp = *vt;
        for (i = 0; i < 4; i++) if (HASC(dest, i)) tmp.u[i] = vu0.r;
        if (ft) *vt = tmp;
        break;
    }
    case 0x42: vu0.r = (vs->u[fsf] & 0x7FFFFF) | 0x3F800000u; break;
    case 0x43: vu0.r = ((vu0.r ^ vs->u[fsf]) & 0x7FFFFF) | 0x3F800000u; break;
    default: hw_log(1, "vu0: unhandled COP2 special %08x", w); break;
    }
}

void vu0_cop2(u32 w)
{
    u32 op = w & 0x3F, dest = DEST(w), ft = FT(w), fs = FS(w), fd = FD(w), bc = w & 3;
    vu_vec *vs = &vu0.vf[fs], *vt = &vu0.vf[ft], tmp;
    float t[4];
    if (((w >> 25) & 0x7F) != 0x25) { hw_log(1, "vu0: not a COP2 macro op %08x", w); return; }
    if (op >= 0x3C) { exec_special(w); fix0(); return; }
    tmp = vu0.vf[fd];
    if (op < 0x1C) {
        static const int k[7] = { K_ADD, K_SUB, K_MADD, K_MSUB, K_MAX, K_MINI, K_MUL };
        bcast(t, vt->u[bc]);
        fmac(k[op >> 2], dest, vs, t, &tmp);
    } else switch (op) {
    case 0x1C: bcast(t, vu0.q); fmac(K_MUL, dest, vs, t, &tmp); break;
    case 0x1D: bcast(t, vu0.i); fmac(K_MAX, dest, vs, t, &tmp); break;
    case 0x1E: bcast(t, vu0.i); fmac(K_MUL, dest, vs, t, &tmp); break;
    case 0x1F: bcast(t, vu0.i); fmac(K_MINI, dest, vs, t, &tmp); break;
    case 0x20: bcast(t, vu0.q); fmac(K_ADD, dest, vs, t, &tmp); break;
    case 0x21: bcast(t, vu0.q); fmac(K_MADD, dest, vs, t, &tmp); break;
    case 0x22: bcast(t, vu0.i); fmac(K_ADD, dest, vs, t, &tmp); break;
    case 0x23: bcast(t, vu0.i); fmac(K_MADD, dest, vs, t, &tmp); break;
    case 0x24: bcast(t, vu0.q); fmac(K_SUB, dest, vs, t, &tmp); break;
    case 0x25: bcast(t, vu0.q); fmac(K_MSUB, dest, vs, t, &tmp); break;
    case 0x26: bcast(t, vu0.i); fmac(K_SUB, dest, vs, t, &tmp); break;
    case 0x27: bcast(t, vu0.i); fmac(K_MSUB, dest, vs, t, &tmp); break;
    case 0x28: full(t, vt); fmac(K_ADD, dest, vs, t, &tmp); break;
    case 0x29: full(t, vt); fmac(K_MADD, dest, vs, t, &tmp); break;
    case 0x2A: full(t, vt); fmac(K_MUL, dest, vs, t, &tmp); break;
    case 0x2B: full(t, vt); fmac(K_MAX, dest, vs, t, &tmp); break;
    case 0x2C: full(t, vt); fmac(K_SUB, dest, vs, t, &tmp); break;
    case 0x2D: full(t, vt); fmac(K_MSUB, dest, vs, t, &tmp); break;
    case 0x2E: { /* OPMSUB: fd.xyz = ACC - fs.yzx * ft.zxy */
        vu_vec a = *vs;
        a.u[0] = vs->u[1]; a.u[1] = vs->u[2]; a.u[2] = vs->u[0];
        t[0] = vf_in(vt->u[2]); t[1] = vf_in(vt->u[0]); t[2] = vf_in(vt->u[1]); t[3] = vf_in(vt->u[3]);
        fmac(K_MSUB, dest, &a, t, &tmp);
        break;
    }
    case 0x2F: full(t, vt); fmac(K_MINI, dest, vs, t, &tmp); break;
    case 0x30: if (FD(w)) vu0.vi[FD(w)] = (u16)(vu0.vi[fs] + vu0.vi[ft]); return;      /* VIADD id, is, it */
    case 0x31: if (FD(w)) vu0.vi[FD(w)] = (u16)(vu0.vi[fs] - vu0.vi[ft]); return;      /* VISUB */
    case 0x32: { s32 im = (s32)((w >> 6) & 0x1F); if (im >= 16) im -= 32; if (ft) vu0.vi[ft] = (u16)(vu0.vi[fs] + im); return; } /* VIADDI */
    case 0x34: if (FD(w)) vu0.vi[FD(w)] = vu0.vi[fs] & vu0.vi[ft]; return;              /* VIAND */
    case 0x35: if (FD(w)) vu0.vi[FD(w)] = vu0.vi[fs] | vu0.vi[ft]; return;              /* VIOR */
    case 0x38: case 0x39: hw_log(1, "vu0: VCALLMS not supported (%08x)", w); return;
    default: hw_log(1, "vu0: unhandled COP2 op %08x", w); return;
    }
    if (fd) vu0.vf[fd] = tmp;
    fix0();
}

u32 vu0_cfc2(u32 reg)
{
    switch (reg) {
    case 16: return vu0.status;
    case 17: return vu0.mac;
    case 18: return vu0.clip;
    case 20: return vu0.r;
    case 21: return vu0.i;
    case 22: return vu0.q;
    case 29: return 0;     /* VPU-STAT: VU0 and VU1 idle */
    default: return reg < 16 ? vu0.vi[reg] : 0;
    }
}

void vu0_ctc2(u32 reg, u32 v)
{
    switch (reg) {
    case 16: vu0.status = (vu0.status & 0x3F) | (v & 0xFC0); break;  /* only the sticky bits are writable */
    case 18: vu0.clip = v & 0xFFFFFF; break;
    case 20: vu0.r = (v & 0x7FFFFF) | 0x3F800000u; break;
    case 21: vu0.i = v; break;
    case 22: vu0.q = v; break;
    case 28: if (v & 2) { /* reset VU0 */ } break;
    default: if (reg >= 1 && reg < 16) vu0.vi[reg] = (u16)v; break;
    }
}
