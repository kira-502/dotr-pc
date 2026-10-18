/* spu2.c - the PS2 sound processor: 2 cores x 24 voices (ADPCM, pitch, ADSR, volume sweeps, noise, pitch
 * modulation), sound RAM transfers, the AutoDMA (streaming PCM) inputs, and the mixer chain
 * core 0 -> core 1 -> output at 48 kHz. Written from the public SPU/SPU2 register documentation.
 * Not yet: reverb (the wet path is silent), IRQ on address. */
#include "iop.h"

u16 spu2_mem[0x100000];

static u16 regs[0x400];                 /* raw register file, by (addr & 0x7FF) / 2 */
#define R16(off) regs[((off) & 0x7FF) >> 1]

typedef struct voice {
    u32 addr;          /* current ADPCM block (word address) */
    u32 lsa;           /* loop start */
    u32 counter;       /* pitch counter, 12 fractional bits */
    s16 dec[28];       /* decoded block */
    int dec_valid;
    s16 hist[4];       /* last 4 output samples for interpolation */
    s32 p1, p2;        /* ADPCM history */
    int phase;         /* 0 off, 1 attack, 2 decay, 3 sustain, 4 release */
    s32 env;           /* envelope level 0..0x7FFF */
    u32 env_cnt;
    s16 out;           /* last output (for pitch modulation of the next voice) */
    int flags;         /* flags byte of the current block */
    int lsa_set;       /* LSA written by the program since key on */
    s32 volx[2];
} voice;

typedef struct core {
    voice v[24];
    u32 endx;
    s16 adma[2][256];
    int adma_pos;      /* 256 = empty */
    s32 out_l, out_r;
    u32 noise, noise_cnt;
    u16 statx;         /* bit 7: last DMA finished; cleared when ATTR is written */
    u32 rev_pos;       /* reverb work area pointer (word address) */
    int rev_phase;     /* reverb runs at half rate: accumulate two inputs, emit one result twice */
    s32 rev_in_l, rev_in_r, rev_out_l, rev_out_r;
} core;

static core cores[2];

void spu2_reset(void)
{
    memset(regs, 0, sizeof regs);
    memset(cores, 0, sizeof cores);
    memset(spu2_mem, 0, sizeof spu2_mem);
    cores[0].adma_pos = cores[1].adma_pos = 256;
    cores[0].noise = cores[1].noise = 1;
}

/* ---- register addresses ---------------------------------------------------------------------------- */

static u32 cbase(int c) { return (u32)c * 0x400u; }
static u32 xbase(int c) { return 0x760u + (u32)c * 0x28u; }
static void wr32(u32 off, u32 v) { R16(off) = (u16)(v >> 16); R16(off + 2) = (u16)v; }

int spu2_adma_active(int c) { return (R16(cbase(c) + 0x1B0) >> c) & 1; }

/* ---- envelope -------------------------------------------------------------------------------------- */

/* One step of the PS envelope generator. rate: 7 bits; dir: +1/-1; expo: exponential mode. */
static void env_step(s32 *level, u32 *cnt, int rate, int dir, int expo)
{
    int shift = rate >> 2, step = (dir > 0) ? 7 - (rate & 3) : -8 + (rate & 3);
    u32 cycles = 1u << (shift > 11 ? shift - 11 : 0);
    s32 sv = step << (shift < 11 ? 11 - shift : 0);
    if (expo && dir > 0 && *level > 0x6000) cycles <<= 2;
    if (expo && dir < 0) sv = (sv * *level) >> 15;
    if (++*cnt < cycles) return;
    *cnt = 0;
    *level += sv;
    if (*level < 0) *level = 0;
    if (*level > 0x7FFF) *level = 0x7FFF;
}

static void adsr(int c, int n, voice *v)
{
    u16 a1 = R16(cbase(c) + n * 0x10 + 6), a2 = R16(cbase(c) + n * 0x10 + 8);
    switch (v->phase) {
    case 1:
        env_step(&v->env, &v->env_cnt, (a1 >> 8) & 0x7F, +1, a1 >> 15);
        if (v->env >= 0x7FFF) { v->phase = 2; v->env_cnt = 0; }
        break;
    case 2: {
        s32 sl = ((a1 & 0xF) + 1) * 0x800;
        env_step(&v->env, &v->env_cnt, ((a1 >> 4) & 0xF) << 2, -1, 1);
        if (v->env <= sl) { v->phase = 3; v->env_cnt = 0; }
        break;
    }
    case 3:
        env_step(&v->env, &v->env_cnt, (a2 >> 6) & 0x7F, (a2 & 0x4000) ? -1 : +1, a2 >> 15);
        break;
    case 4:
        env_step(&v->env, &v->env_cnt, (a2 & 0x1F) << 2, -1, (a2 >> 5) & 1);
        if (v->env <= 0) { v->env = 0; v->phase = 0; }
        break;
    default: break;
    }
}

/* Volume register: fixed (bit 15 clear: 15-bit value * 2) or a sweep that moves the current volume. */
static s32 vol_step(u16 reg, s32 *cur, u32 *cnt)
{
    if (!(reg & 0x8000)) { *cur = (s16)(reg << 1); return *cur; }
    {
        int dir = (reg & 0x2000) ? -1 : +1, expo = (reg >> 14) & 1;
        s32 lvl = *cur < 0 ? -*cur : *cur;
        env_step(&lvl, cnt, reg & 0x7F, dir, expo);
        *cur = (reg & 0x1000) ? -lvl : lvl;
        return *cur;
    }
}

/* ---- ADPCM ----------------------------------------------------------------------------------------- */

static const s32 filt[5][2] = { { 0, 0 }, { 60, 0 }, { 115, -52 }, { 98, -55 }, { 122, -60 } };

static void decode_block(voice *v)
{
    u32 a = v->addr & 0xFFFFF;
    u16 h = spu2_mem[a];
    int shift = h & 0xF, f = (h >> 4) & 7, i;
    if (f > 4) f = 4;
    if (shift > 12) shift = 9;
    v->flags = h >> 8;
    for (i = 0; i < 28; i++) {
        u16 w = spu2_mem[(a + 1 + i / 4) & 0xFFFFF];
        s32 s = (s16)(((w >> ((i & 3) * 4)) & 0xF) << 12) >> shift;
        s += (v->p1 * filt[f][0] + v->p2 * filt[f][1] + 32) >> 6;
        if (s > 32767) s = 32767;
        if (s < -32768) s = -32768;
        v->p2 = v->p1; v->p1 = s;
        v->dec[i] = (s16)s;
    }
    if ((v->flags & 4) && !v->lsa_set) v->lsa = a;
    v->dec_valid = 1;
}

static s16 interp(const s16 *h, u32 frac)  /* 4-point cubic between h[1] and h[2]; frac 0..4095 */
{
    float t = (float)frac / 4096.0f;
    float p0 = h[0], p1 = h[1], p2 = h[2], p3 = h[3];
    float r = p1 + 0.5f * t * (p2 - p0 + t * (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3 + t * (3.0f * (p1 - p2) + p3 - p0)));
    if (r > 32767.0f) r = 32767.0f;
    if (r < -32768.0f) r = -32768.0f;
    return (s16)r;
}

/* The decoder walks a block sample by sample; keep the position in the block separately. */
typedef struct vpos { int i; } vpos;
static vpos pos[2][24];

static void voice_advance(int c, int n, voice *v)
{
    vpos *p = &pos[c][n];
    if (!v->dec_valid) { decode_block(v); p->i = 0; }
    v->hist[0] = v->hist[1]; v->hist[1] = v->hist[2]; v->hist[2] = v->hist[3];
    v->hist[3] = v->dec[p->i];
    if (++p->i >= 28) {
        p->i = 0;
        v->dec_valid = 0;
        if (v->flags & 1) {                       /* loop end */
            cores[c].endx |= 1u << n;
            if (v->flags & 2) v->addr = v->lsa;   /* repeat */
            else { v->phase = 4; v->env = 0; v->addr += 8; }
        } else {
            v->addr += 8;
        }
    }
}

static s16 run_voice(int c, int n, voice *v, s16 prev_out)
{
    u32 vb = cbase(c) + (u32)n * 0x10;
    u32 pitch = R16(vb + 4);
    u32 pmon = (u32)R16(cbase(c) + 0x180) | ((u32)R16(cbase(c) + 0x182) << 16);
    u32 non = (u32)R16(cbase(c) + 0x184) | ((u32)R16(cbase(c) + 0x186) << 16);
    s32 s;
    if (!v->phase) return 0;
    if (n && (pmon >> n & 1)) pitch = (u32)(((s32)pitch * (32768 + prev_out)) >> 15);
    if (pitch > 0x4000) pitch = 0x4000;                   /* 3FFFh and up play as 4000h (4x) */
    if (non >> n & 1) s = (s16)cores[c].noise;
    else s = interp(v->hist, v->counter & 0xFFF);
    v->counter += pitch;
    while (v->counter >= 0x1000) {
        v->counter -= 0x1000;
        voice_advance(c, n, v);
    }
    adsr(c, n, v);
    s = (s * v->env) >> 15;
    return (s16)s;
}

static void key_on(int c, int n)
{
    voice *v = &cores[c].v[n];
    u32 vb = cbase(c) + 0x1C0 + (u32)n * 0xC;
    v->addr = (((u32)R16(vb) << 16) | R16(vb + 2)) & 0xFFFFF;
    v->lsa = (((u32)R16(vb + 4) << 16) | R16(vb + 6)) & 0xFFFFF;
    v->lsa_set = 0;
    v->counter = 0;
    v->dec_valid = 0;
    v->p1 = v->p2 = 0;
    memset(v->hist, 0, sizeof v->hist);
    v->phase = 1; v->env = 0; v->env_cnt = 0;
    pos[c][n].i = 0;
    cores[c].endx &= ~(1u << n);
}

static void key_off(int c, int n)
{
    voice *v = &cores[c].v[n];
    if (v->phase) { v->phase = 4; v->env_cnt = 0; }
}

/* ---- registers ------------------------------------------------------------------------------------- */

u16 spu2_read(u32 a)
{
    u32 off = a & 0x7FF;
    int c = off >= 0x400 && off < 0x760 ? 1 : (off >= 0x788 && off < 0x7B0 ? 1 : 0);
    u32 lo = off - cbase(c);
    if (off < 0x760) {
        if (lo < 0x180 && (lo & 0xF) == 0xA) return (u16)cores[c].v[lo >> 4].env;        /* ENVX */
        if (lo < 0x180 && (lo & 0xF) == 0xC) return (u16)cores[c].v[lo >> 4].volx[0];
        if (lo < 0x180 && (lo & 0xF) == 0xE) return (u16)cores[c].v[lo >> 4].volx[1];
        if (lo >= 0x1C0 && lo < 0x2E0 && ((lo - 0x1C0) % 0xC) >= 8) {                     /* NAX */
            voice *v = &cores[c].v[(lo - 0x1C0) / 0xC];
            return ((lo - 0x1C0) % 0xC) == 8 ? (u16)(v->addr >> 16) : (u16)v->addr;
        }
        if (lo == 0x340) return (u16)cores[c].endx;
        if (lo == 0x342) return (u16)(cores[c].endx >> 16);
        if (lo == 0x344) return cores[c].statx;                                             /* STATX (never busy) */
        if (lo == 0x1A0 || lo == 0x1A2 || lo == 0x1A4 || lo == 0x1A6) return 0;
    }
    return R16(off);
}

void spu2_write(u32 a, u16 v)
{
    u32 off = a & 0x7FF;
    int c = off >= 0x400 && off < 0x760 ? 1 : 0;
    u32 lo = off - cbase(c);
    R16(off) = v;
    if (off >= 0x760) return;
    switch (lo) {
    case 0x1A0: case 0x1A2: {   /* KON */
        int k;
        for (k = 0; k < 16; k++) if (v >> k & 1) key_on(c, k + (lo == 0x1A2 ? 16 : 0));
        break;
    }
    case 0x1A4: case 0x1A6: {   /* KOFF */
        int k;
        for (k = 0; k < 16; k++) if (v >> k & 1) key_off(c, k + (lo == 0x1A6 ? 16 : 0));
        break;
    }
    case 0x1AC: {               /* data port: write at TSA, TSA advances */
        u32 tsa = (((u32)R16(cbase(c) + 0x1A8) << 16) | R16(cbase(c) + 0x1AA)) & 0xFFFFF;
        spu2_mem[tsa] = v;
        tsa = (tsa + 1) & 0xFFFFF;
        wr32(cbase(c) + 0x1A8, tsa);
        break;
    }
    case 0x19A: cores[c].statx = 0; break;                 /* ATTR: DMA mode / reset */
    case 0x340: cores[c].endx &= ~(u32)v; break;           /* writing ENDX clears */
    case 0x342: cores[c].endx &= ~((u32)v << 16); break;
    default:
        if (lo >= 0x1C0 && lo < 0x2E0 && ((lo - 0x1C0) % 0xC) >= 4 && ((lo - 0x1C0) % 0xC) < 8)
            cores[c].v[(lo - 0x1C0) / 0xC].lsa_set = 1, cores[c].v[(lo - 0x1C0) / 0xC].lsa =
                (((u32)R16(cbase(c) + 0x1C0 + ((lo - 0x1C0) / 0xC) * 0xC + 4) << 16) |
                 R16(cbase(c) + 0x1C0 + ((lo - 0x1C0) / 0xC) * 0xC + 6)) & 0xFFFFF;
        break;
    }
}

/* A channel 4/7 transfer into sound RAM at the core's TSA (the core is the one in DMA mode in ATTR). */
void spu2_dma_write_core(int c, const u8 *src, u32 bytes)
{
    u32 tsa = (((u32)R16(cbase(c) + 0x1A8) << 16) | R16(cbase(c) + 0x1AA)) & 0xFFFFF, i;
    for (i = 0; i + 1 < bytes; i += 2) {
        u16 w; memcpy(&w, src + i, 2);
        spu2_mem[tsa] = w;
        tsa = (tsa + 1) & 0xFFFFF;
    }
    wr32(cbase(c) + 0x1A8, tsa);
    cores[c].statx |= 0x80;
}

/* The other direction (SPU RAM -> IOP RAM), from the core's TSA. */
void spu2_dma_read_core(int c, u8 *dst, u32 bytes)
{
    u32 tsa = (((u32)R16(cbase(c) + 0x1A8) << 16) | R16(cbase(c) + 0x1AA)) & 0xFFFFF, i;
    for (i = 0; i + 1 < bytes; i += 2) {
        memcpy(dst + i, &spu2_mem[tsa], 2);
        tsa = (tsa + 1) & 0xFFFFF;
    }
    wr32(cbase(c) + 0x1A8, tsa);
    cores[c].statx |= 0x80;
}

void spu2_dma_done(int c) { cores[c].statx |= 0x80; }

/* ---- reverb ------------------------------------------------------------------------------------------
 * The PS-family reverb (two IIR "same side" and "cross side" reflections, four comb taps, two all-pass
 * stages) on the core's work area ESA..EEA in sound RAM, at half the output rate. Register names as SPU2. */

static inline s32 clamp16(s32 v);

static s32 rv_addr(int c, u32 off24)
{
    core *k = &cores[c];
    u32 esa = (((u32)R16(cbase(c) + 0x2E0) << 16) | R16(cbase(c) + 0x2E2)) & 0xFFFFF;
    u32 eea = (((u32)R16(cbase(c) + 0x33C) << 16) | 0xFFFF) & 0xFFFFF;
    u32 size, a;
    if (eea <= esa) return -1;
    size = eea - esa + 1;
    a = k->rev_pos + off24;
    while (a > eea) a -= size;
    return (s32)a;
}

static u32 rv_off(int c, u32 reg) { return (((u32)R16(cbase(c) + reg) << 16) | R16(cbase(c) + reg + 2)) & 0xFFFFF; }

static s32 rv_rd(int c, u32 reg, int minus)
{
    s32 a = rv_addr(c, rv_off(c, reg) - (u32)minus);
    return a < 0 ? 0 : (s16)spu2_mem[a];
}

static void rv_wr(int c, u32 reg, s32 v)
{
    s32 a = rv_addr(c, rv_off(c, reg));
    if (a < 0) return;
    if (v > 32767) v = 32767;
    if (v < -32768) v = -32768;
    spu2_mem[a] = (u16)(s16)v;
}

static inline s32 rmul(s32 a, s32 b) { return (a * b) >> 15; }

static void reverb_tick(int c, s32 lin, s32 rin, s32 *lout, s32 *rout)
{
    core *k = &cores[c];
    u32 x = xbase(c);
    s32 vIIR = (s16)R16(x + 0x14), vCOMB1 = (s16)R16(x + 0x16), vCOMB2 = (s16)R16(x + 0x18), vCOMB3 = (s16)R16(x + 0x1A);
    s32 vCOMB4 = (s16)R16(x + 0x1C), vWALL = (s16)R16(x + 0x1E), vAPF1 = (s16)R16(x + 0x20), vAPF2 = (s16)R16(x + 0x22);
    s32 vLIN = (s16)R16(x + 0x24), vRIN = (s16)R16(x + 0x26);
    u32 dAPF1 = rv_off(c, 0x2E4), dAPF2 = rv_off(c, 0x2E8);
    s32 L = rmul(lin, vLIN), R = rmul(rin, vRIN), t, la, ra;
    s32 esa = (s32)((((u32)R16(cbase(c) + 0x2E0) << 16) | R16(cbase(c) + 0x2E2)) & 0xFFFFF);
    /* same-side and cross-side reflections */
    t = rmul(L + rmul(rv_rd(c, 0x304, 0), vWALL) - rv_rd(c, 0x2EC, 1), vIIR) + rv_rd(c, 0x2EC, 1); rv_wr(c, 0x2EC, t);
    t = rmul(R + rmul(rv_rd(c, 0x308, 0), vWALL) - rv_rd(c, 0x2F0, 1), vIIR) + rv_rd(c, 0x2F0, 1); rv_wr(c, 0x2F0, t);
    t = rmul(L + rmul(rv_rd(c, 0x328, 0), vWALL) - rv_rd(c, 0x30C, 1), vIIR) + rv_rd(c, 0x30C, 1); rv_wr(c, 0x30C, t);
    t = rmul(R + rmul(rv_rd(c, 0x324, 0), vWALL) - rv_rd(c, 0x310, 1), vIIR) + rv_rd(c, 0x310, 1); rv_wr(c, 0x310, t);
    /* comb taps */
    la = rmul(vCOMB1, rv_rd(c, 0x2F4, 0)) + rmul(vCOMB2, rv_rd(c, 0x2FC, 0)) + rmul(vCOMB3, rv_rd(c, 0x314, 0)) + rmul(vCOMB4, rv_rd(c, 0x31C, 0));
    ra = rmul(vCOMB1, rv_rd(c, 0x2F8, 0)) + rmul(vCOMB2, rv_rd(c, 0x300, 0)) + rmul(vCOMB3, rv_rd(c, 0x318, 0)) + rmul(vCOMB4, rv_rd(c, 0x320, 0));
    /* two all-pass stages */
    {
        s32 a1l = rv_addr(c, rv_off(c, 0x32C) - dAPF1), a1r = rv_addr(c, rv_off(c, 0x330) - dAPF1);
        s32 a2l = rv_addr(c, rv_off(c, 0x334) - dAPF2), a2r = rv_addr(c, rv_off(c, 0x338) - dAPF2);
        s32 m1l = a1l < 0 ? 0 : (s16)spu2_mem[a1l], m1r = a1r < 0 ? 0 : (s16)spu2_mem[a1r];
        s32 m2l, m2r;
        la -= rmul(vAPF1, m1l); rv_wr(c, 0x32C, la); la = rmul(la, vAPF1) + m1l;
        ra -= rmul(vAPF1, m1r); rv_wr(c, 0x330, ra); ra = rmul(ra, vAPF1) + m1r;
        m2l = a2l < 0 ? 0 : (s16)spu2_mem[a2l]; m2r = a2r < 0 ? 0 : (s16)spu2_mem[a2r];
        la -= rmul(vAPF2, m2l); rv_wr(c, 0x334, la); la = rmul(la, vAPF2) + m2l;
        ra -= rmul(vAPF2, m2r); rv_wr(c, 0x338, ra); ra = rmul(ra, vAPF2) + m2r;
    }
    *lout = clamp16(la);
    *rout = clamp16(ra);
    /* advance through the work area */
    {
        u32 eea = (((u32)R16(cbase(c) + 0x33C) << 16) | 0xFFFF) & 0xFFFFF;
        if ((u32)esa < eea) {
            k->rev_pos++;
            if (k->rev_pos > eea || k->rev_pos < (u32)esa) k->rev_pos = (u32)esa;
        }
    }
}

/* ---- the mixer ------------------------------------------------------------------------------------- */

extern u32 iop_dma_pull(int core, u8 *dst, u32 n);

u32 spu2_adma_short;   /* AutoDMA fetches that found the stream not running (gaps) */

static void adma_fetch(int c)
{
    core *k = &cores[c];
    u8 buf[1024];
    u32 got = iop_dma_pull(c, buf, sizeof buf);
    if (got < sizeof buf) { memset(buf + got, 0, sizeof buf - got); spu2_adma_short++; }
    memcpy(k->adma[0], buf, 512);
    memcpy(k->adma[1], buf + 512, 512);
    k->adma_pos = 0;
}

static inline s32 clamp16(s32 v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : v; }
static inline s32 vmul(s32 s, s32 vol) { return (s * vol) >> 15; }

/* Noise: a 16-bit shift register clocked by a timer of (0x20000 >> shift) counted down by 4..7 per sample. */
static void noise_tick(core *k, u16 attr)
{
    s32 shift = (attr >> 10) & 0xF, step = ((attr >> 8) & 3) + 4, t = (s32)k->noise_cnt;
    u32 n = k->noise, fb = ((n >> 15) ^ (n >> 12) ^ (n >> 11) ^ (n >> 10) ^ 1) & 1;
    t -= step;
    if (t < 0) {
        k->noise = (n << 1 | fb) & 0xFFFF;
        t += 0x20000 >> shift;
        if (t < 0) t += 0x20000 >> shift;
    }
    k->noise_cnt = (u32)t;
}

static void mix_core(int c, s32 ext_l, s32 ext_r, s32 *out_l, s32 *out_r)
{
    core *k = &cores[c];
    u32 b = cbase(c), x = xbase(c);
    u32 vl = R16(b + 0x188) | ((u32)R16(b + 0x18A) << 16), vel = R16(b + 0x18C) | ((u32)R16(b + 0x18E) << 16);
    u32 vr = R16(b + 0x190) | ((u32)R16(b + 0x192) << 16), ver = R16(b + 0x194) | ((u32)R16(b + 0x196) << 16);
    u16 mmix = R16(b + 0x198), attr = R16(b + 0x19A);
    s32 dl = 0, dr = 0, wl = 0, wr = 0, in_l = 0, in_r = 0;
    s16 prev = 0;
    int n;
    static u32 vcnt[2][24][2];
    noise_tick(k, attr);
    for (n = 0; n < 24; n++) {
        voice *v = &k->v[n];
        s32 s, l, r;
        if (!v->phase) { prev = 0; v->out = 0; continue; }
        s = run_voice(c, n, v, prev);
        prev = (s16)s;
        v->out = (s16)s;
        l = vmul(s, vol_step(R16(b + n * 0x10), &v->volx[0], &vcnt[c][n][0]));
        r = vmul(s, vol_step(R16(b + n * 0x10 + 2), &v->volx[1], &vcnt[c][n][1]));
        if (vl >> n & 1) dl += l;
        if (vr >> n & 1) dr += r;
        if (vel >> n & 1) wl += l;
        if (ver >> n & 1) wr += r;
    }
    if (!(attr & 0x4000)) dl = dr = wl = wr = 0;      /* ATTR bit 14 clear: voices muted (the inputs still pass) */
    if (!(mmix & 0x800)) dl = 0;
    if (!(mmix & 0x400)) dr = 0;
    if (!(mmix & 0x200)) wl = 0;
    if (!(mmix & 0x100)) wr = 0;
    /* AutoDMA (sound data) input */
    if (spu2_adma_active(c)) {
        if (k->adma_pos >= 256) adma_fetch(c);
        in_l = vmul(k->adma[0][k->adma_pos], (s16)R16(x + 0xC));
        in_r = vmul(k->adma[1][k->adma_pos], (s16)R16(x + 0xE));
        k->adma_pos++;
    }
    if (mmix & 0x80) dl += in_l;
    if (mmix & 0x40) dr += in_r;
    if (mmix & 0x20) wl += in_l;
    if (mmix & 0x10) wr += in_r;
    /* external input (core 0's output into core 1) */
    ext_l = vmul(ext_l, (s16)R16(x + 0x8));
    ext_r = vmul(ext_r, (s16)R16(x + 0xA));
    if (mmix & 0x8) dl += ext_l;
    if (mmix & 0x4) dr += ext_r;
    if (mmix & 0x2) wl += ext_l;
    if (mmix & 0x1) wr += ext_r;
    /* reverb (ATTR bit 7: effects on), at half rate */
    {
        s32 rl = 0, rr = 0;
        if (attr & 0x80) {
            k->rev_in_l += wl; k->rev_in_r += wr;
            if (k->rev_phase ^= 1) {
                reverb_tick(c, clamp16(k->rev_in_l / 2), clamp16(k->rev_in_r / 2), &k->rev_out_l, &k->rev_out_r);
                k->rev_in_l = k->rev_in_r = 0;
            }
            rl = vmul(k->rev_out_l, (s16)R16(x + 4));
            rr = vmul(k->rev_out_r, (s16)R16(x + 6));
        }
        dl += rl; dr += rr;
    }
    {
        static s32 mv[2][2]; static u32 mc[2][2];
        *out_l = clamp16(vmul(clamp16(dl), vol_step(R16(x + 0), &mv[c][0], &mc[c][0])));
        *out_r = clamp16(vmul(clamp16(dr), vol_step(R16(x + 2), &mv[c][1], &mc[c][1])));
        if (!(attr & 0x8000)) *out_l = *out_r = 0;      /* ATTR bit 15 clear: the core is off */
        R16(x + 0x10) = (u16)mv[c][0]; R16(x + 0x12) = (u16)mv[c][1];
    }
}

void spu2_render(s16 *out, int frames)
{
    int i;
    for (i = 0; i < frames; i++) {
        s32 l0, r0, l1, r1;
        mix_core(0, 0, 0, &l0, &r0);
        mix_core(1, l0, r0, &l1, &r1);
        out[i * 2] = (s16)l1;
        out[i * 2 + 1] = (s16)r1;
    }
}
