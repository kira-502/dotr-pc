/* vu_math.h - VU float semantics shared by the VU1 interpreter and the VU0 macro-mode engine. */
#ifndef DOTR_VU_MATH_H
#define DOTR_VU_MATH_H

#include <string.h>
#include "gfx_internal.h"

static inline float vf_in(u32 u)
{
    float f;
    u32 e = (u >> 23) & 0xFF;
    if (e == 0) u &= 0x80000000u;
    else if (e == 0xFF) u = (u & 0x80000000u) | 0x7F7FFFFFu;
    memcpy(&f, &u, 4);
    return f;
}

/* Convert a result to VU format and report its flags: bit0 Z, bit1 S, bit2 U, bit3 O. */
static inline u32 vf_out(float f, u32 *fl)
{
    u32 u, e;
    memcpy(&u, &f, 4);
    e = (u >> 23) & 0xFF;
    *fl = 0;
    if (e == 0xFF) { u = (u & 0x80000000u) | 0x7FFFFFFFu; *fl |= 8; }
    else if (e == 0) { if (u & 0x7FFFFFu) *fl |= 4 | 1; else *fl |= 1; u &= 0x80000000u; }
    if (u & 0x80000000u) *fl |= 2;
    return u;
}

/* An exact-ish double result to VU format: overflow -> +-max with O, underflow -> +-0 with U and Z. Under the
 * round-toward-zero mode the double->float conversion truncates, as the VU does. */
static inline u32 vd_out(double d, u32 *fl)
{
    float f;
    *fl = 0;
    if (d > 3.4028234663852886e38 || d < -3.4028234663852886e38) {
        *fl = 8 | (d < 0 ? 2 : 0);
        return (d < 0 ? 0x80000000u : 0) | 0x7FFFFFFFu;
    }
    if (d != 0 && d < 1.1754943508222875e-38 && d > -1.1754943508222875e-38) {
        *fl = 4 | 1 | (d < 0 ? 2 : 0);
        return d < 0 ? 0x80000000u : 0;
    }
    f = (float)d;
    return vf_out(f, fl);
}

static inline u32 mac_bits(int comp, u32 fl)
{
    u32 bit = 1u << (3 - comp), m = 0;
    if (fl & 1) m |= bit;
    if (fl & 2) m |= bit << 4;
    if (fl & 4) m |= bit << 8;
    if (fl & 8) m |= bit << 12;
    return m;
}

#endif
