/* gs.c - GS general registers, vertex queue, CLUT loads, image transfers, privileged registers. */
#include <stdlib.h>
#include <stdio.h>
#include "gfx_internal.h"
#include "gs_fast.h"
#include "gpu/gs_gpu.h"

gs_state gs;
static int gs_trace(void);

#define BITS(v, lo, n) ((u32)(((v) >> (lo)) & ((1ull << (n)) - 1)))

void gs_reset(void)
{
    memset(&gs, 0, sizeof gs);
    gs.q = 1.0f;
    gs.prmodecont = 1;
    gs.csr = 0x551B0000ull;  /* GS revision/ID bits as the console reports them; FIFO empty */
}

/* ---- CLUT ------------------------------------------------------------------------------------------ */

static void clut_load(u64 tex0)
{
    u32 psm = BITS(tex0, 20, 6), cbp = BITS(tex0, 37, 14), cpsm = BITS(tex0, 51, 4), csm = BITS(tex0, 55, 1);
    u32 csa = BITS(tex0, 56, 5);
    u32 n = (psm == PSMT8 || psm == PSMT8H) ? 256 : 16;
    u32 i;
    int ct16 = (cpsm == PSMCT16 || cpsm == PSMCT16S);
    if (!(psm == PSMT8 || psm == PSMT8H || psm == PSMT4 || psm == PSMT4HL || psm == PSMT4HH))
        return;
    {
        u32 lo, hi;
        if (csm == 0) gs_rect_blocks(ct16 ? cpsm : PSMCT32, cbp, 1, 0, 0, 15, 15, &lo, &hi);
        else { lo = 0; hi = 16383; }
        gs_sync_blocks(lo, hi);
        /* the GPU loads its own copy from GS memory as it has it (no read-back to the CPU) */
        if (gs_gpu_on)
            gs_gpu_clut_load(cbp, cpsm, csm, csa, n, BITS(gs.texclut, 0, 6), BITS(gs.texclut, 6, 6),
                             BITS(gs.texclut, 12, 10), lo, hi);
    }
    gs_clut_gen++;
    for (i = 0; i < n; i++) {
        u32 x, y, c, e;
        if (csm == 0) {
            if (n == 256) {
                /* CSM1: 16x16 block of entries; index bits 3 and 4 are swapped in memory order */
                u32 j = (i & 0xE7) | ((i & 8) << 1) | ((i & 16) >> 1);
                x = j & 15; y = j >> 4;
            } else {
                x = i & 7; y = i >> 3;
            }
            c = gs_read_pixel(cpsm == PSMCT32 || cpsm == PSMCT24 ? PSMCT32 : cpsm, cbp, 1, x, y);
        } else {
            /* CSM2: one row of 16-bit entries at (COU*16, COV) in a CBW-wide buffer */
            u32 cbw = BITS(gs.texclut, 0, 6), cou = BITS(gs.texclut, 6, 6), cov = BITS(gs.texclut, 12, 10);
            c = gs_read_pixel(ct16 ? cpsm : PSMCT16, cbp, cbw, cou * 16 + i, cov);
            ct16 = 1;
        }
        if (ct16) {
            e = (n == 16 ? csa * 16 + i : i) & 511;
            gs.clut[e] = (u16)c;
        } else {
            e = (n == 16 ? (csa & 15) * 16 + i : i) & 255;
            gs.clut[e] = (u16)c;
            gs.clut[e + 256] = (u16)(c >> 16);
        }
    }
}

static void tex0_written(u64 v)
{
    u32 cld = BITS(v, 61, 3), cbp = BITS(v, 37, 14);
    switch (cld) {
    case 0: return;
    case 1: break;
    case 2: gs.cbp0 = cbp; break;
    case 3: gs.cbp1 = cbp; break;
    case 4: if (gs.cbp0 == cbp) return; gs.cbp0 = cbp; break;
    case 5: if (gs.cbp1 == cbp) return; gs.cbp1 = cbp; break;
    default: return;
    }
    clut_load(v);
}

/* ---- vertices -------------------------------------------------------------------------------------- */

static u64 prim_attr(void)
{
    return (gs.prmodecont & 1) ? gs.prim : ((gs.prim & 7) | (gs.prmode & 0x7F8));
}

static void vertex_kick(u64 xyz, int with_fog, int draw)
{
    gs_vertex *v;
    u64 attr = prim_attr();
    const gs_ctx *c = &gs.ctx[(attr >> 9) & 1];
    u32 type = (u32)(gs.prim & 7);
    int need;
    gs_vertex nv;

    nv.x = (s32)(xyz & 0xFFFF) - (s32)BITS(c->xyoffset, 0, 16);
    nv.y = (s32)((xyz >> 16) & 0xFFFF) - (s32)BITS(c->xyoffset, 32, 16);
    if (with_fog) {
        nv.z = (u32)((xyz >> 32) & 0xFFFFFF);
        nv.f = (u8)(xyz >> 56);
    } else {
        nv.z = (u32)(xyz >> 32);
        nv.f = (u8)(gs.fog >> 56);
    }
    nv.r = (u8)gs.rgbaq; nv.g = (u8)(gs.rgbaq >> 8); nv.b = (u8)(gs.rgbaq >> 16); nv.a = (u8)(gs.rgbaq >> 24);
    { u32 qb = (u32)(gs.rgbaq >> 32); memcpy(&nv.q, &qb, 4); }
    { u32 sb = (u32)gs.st, tb = (u32)(gs.st >> 32); memcpy(&nv.s, &sb, 4); memcpy(&nv.t, &tb, 4); }
    nv.u = BITS(gs.uv, 0, 14); nv.v = BITS(gs.uv, 16, 14);

    switch (type) {
    case 0: need = 1; break;
    case 1: case 2: case 6: need = 2; break;
    case 3: case 4: case 5: need = 3; break;
    default: return;
    }
    v = gs.vq;
    v[gs.vcount++] = nv;
    if (gs.vcount < need)
        return;
    if (draw && gs_trace()) {
        u64 pr = (gs.prmodecont & 1) ? gs.prim : gs.prmode;
        const gs_ctx *c = &gs.ctx[(pr >> 9) & 1];
        int i;
        char buf[256];
        int o = snprintf(buf, sizeof buf, "prim %u tme %u fst %u abe %u ctx %u frame %016llx tex0 %016llx tex1 %016llx test %llx zbuf %llx alpha %llx scissor %u-%u,%u-%u |",
                         type, (u32)(pr >> 4) & 1, (u32)(pr >> 8) & 1, (u32)(pr >> 6) & 1, (u32)(pr >> 9) & 1,
                         (unsigned long long)c->frame, (unsigned long long)c->tex0, (unsigned long long)c->tex1, (unsigned long long)c->test, (unsigned long long)c->zbuf, (unsigned long long)c->alpha, BITS(c->scissor, 0, 11), BITS(c->scissor, 16, 11),
                         BITS(c->scissor, 32, 11), BITS(c->scissor, 48, 11));
        for (i = 0; i < need && o < (int)sizeof buf - 48; i++)   /* window coordinates (offset already applied) */
            o += snprintf(buf + o, sizeof buf - (size_t)o, " (%.1f,%.1f uv %u,%u a%u)", v[i].x / 16.0, v[i].y / 16.0,
                          v[i].u >> 4, v[i].v >> 4, v[i].a);
        hw_log(2, "%s", buf);
    }
    if (draw) {
        static long long max_prims = -2;   /* DOTR_GS_MAXPRIM=n: draw only the first n primitives (bisecting a replay) */
        if (max_prims == -2) max_prims = getenv("DOTR_GS_MAXPRIM") ? atoll(getenv("DOTR_GS_MAXPRIM")) : -1;
        if (max_prims < 0) gs_draw_prim(v, need);
        else if (max_prims > 0) { max_prims--; gs_draw_prim(v, need); }
    }
    /* advance the queue */
    switch (type) {
    case 2: v[0] = v[1]; gs.vcount = 1; break;               /* line strip */
    case 4: v[0] = v[1]; v[1] = v[2]; gs.vcount = 2; break;  /* triangle strip */
    case 5: v[1] = v[2]; gs.vcount = 2; break;               /* triangle fan keeps v[0] */
    default: gs.vcount = 0; break;
    }
}

/* ---- transfers ------------------------------------------------------------------------------------- */

static int trx_changed;   /* the running host->local transfer has changed GS memory */

/* Tell the texture cache the whole destination rectangle of the host->local transfer changed. */
static void trx_mark_dest(void)
{
    u32 dbp = BITS(gs.bitbltbuf, 32, 14), dbw = BITS(gs.bitbltbuf, 48, 6), dpsm = BITS(gs.bitbltbuf, 56, 6), lo, hi;
    u32 dsax = BITS(gs.trxpos, 32, 11), dsay = BITS(gs.trxpos, 48, 11);
    u32 rrw = BITS(gs.trxreg, 0, 12), rrh = BITS(gs.trxreg, 32, 12);
    if (!rrw || !rrh) return;
    if (dsax + rrw > 2048 || dsay + rrh > 2048) { lo = 0; hi = 16383; }   /* wraps: everything */
    else gs_rect_blocks(dpsm, dbp, dbw, dsax, dsay, dsax + rrw - 1, dsay + rrh - 1, &lo, &hi);
    gs_mark_written(lo, hi);
}

static void trx_put_pixel(u32 v)
{
    u32 dbp = BITS(gs.bitbltbuf, 32, 14), dbw = BITS(gs.bitbltbuf, 48, 6), dpsm = BITS(gs.bitbltbuf, 56, 6);
    u32 dsax = BITS(gs.trxpos, 32, 11), dsay = BITS(gs.trxpos, 48, 11);
    u32 rrw = BITS(gs.trxreg, 0, 12), rrh = BITS(gs.trxreg, 32, 12);
    if (!gs.trx_active)
        return;
    /* renderer=gpu: gs_vram may lag what the GPU drew, so "same as before" cannot be told here: every upload changes */
    if ((gs_store_pixel(dpsm, dbp, dbw, (dsax + gs.trx_x) & 2047, (dsay + gs.trx_y) & 2047, v) || gs_gpu_only) && !trx_changed) {
        trx_changed = 1;   /* the first real change: decoded copies of this rectangle are stale from here on */
        trx_mark_dest();
    }
    if (++gs.trx_x >= rrw) {
        gs.trx_x = 0;
        if (++gs.trx_y >= rrh) {
            gs.trx_active = 0;
        }
    }
}

/* 32/16-bit transfers: the frame buffer address tables give each pixel's address in two lookups; the same stores
 * as trx_put_pixel. These layouts add the base pointer to the block number, so one table built at base 0 serves
 * every destination of a buffer width. 0 when the transfer is not one of those. */
static int trx_table_write(const u32 *words, u32 nwords, u32 dpsm, int bpp)
{
    u32 dbp = BITS(gs.bitbltbuf, 32, 14), dbw = BITS(gs.bitbltbuf, 48, 6);
    u32 dsax = BITS(gs.trxpos, 32, 11), dsay = BITS(gs.trxpos, 48, 11);
    u32 rrw = BITS(gs.trxreg, 0, 12), rrh = BITS(gs.trxreg, 32, 12);
    u32 i, n = bpp == 16 ? nwords * 2 : nwords, base = dbp * (bpp == 16 ? 128u : 64u);
    const gs_fbtab *t;
    if (dpsm != PSMCT32 && dpsm != PSMZ32 && dpsm != PSMCT16 && dpsm != PSMCT16S && dpsm != PSMZ16 && dpsm != PSMZ16S)
        return 0;
    t = gs_fbtab_get(0, dbw, dpsm);
    if (!t) return 0;
    i = 0;
    while (i < n && gs.trx_active) {   /* row by row */
        u32 row = t->yo[(dsay + gs.trx_y) & 2047] + base, x0 = dsax + gs.trx_x, run = rrw - gs.trx_x, k, diff = 0;
        if (run > n - i) run = n - i;
        if (bpp == 16) {
            u16 *h = (u16 *)gs_vram;
            for (k = 0; k < run; k++, i++) {
                u32 a = (row + t->xo[(x0 + k) & 2047]) & t->mask;
                u16 v = (u16)(words[i >> 1] >> ((i & 1) * 16));
                diff |= h[a] ^ v;
                h[a] = v;
            }
        } else {
            for (k = 0; k < run; k++, i++) {
                u32 a = (row + t->xo[(x0 + k) & 2047]) & t->mask;
                diff |= gs_vram[a] ^ words[i];
                gs_vram[a] = words[i];
            }
        }
        if ((diff || gs_gpu_only) && !trx_changed) {   /* renderer=gpu: see trx_put_pixel */
            trx_changed = 1;   /* the first real change: decoded copies of this rectangle are stale from here on */
            trx_mark_dest();
        }
        gs.trx_x += run;
        if (gs.trx_x >= rrw) {
            gs.trx_x = 0;
            if (++gs.trx_y >= rrh) gs.trx_active = 0;
        }
    }
    return 1;
}

static void hwreg_cpu(const u32 *words, u32 nwords)
{
    u32 dpsm = BITS(gs.bitbltbuf, 56, 6);
    {
        u32 dbp = BITS(gs.bitbltbuf, 32, 14), dbw = BITS(gs.bitbltbuf, 48, 6), lo, hi;
        u32 dsax = BITS(gs.trxpos, 32, 11), dsay = BITS(gs.trxpos, 48, 11);
        u32 rrw = BITS(gs.trxreg, 0, 12), rrh = BITS(gs.trxreg, 32, 12);
        gs_rect_blocks(dpsm, dbp, dbw, dsax, dsay, dsax + (rrw ? rrw - 1 : 0), dsay + (rrh ? rrh - 1 : 0), &lo, &hi);
        gs_sync_blocks(lo, hi);
    }
    int bpp = gs_psm_bpp(dpsm);
    u32 i;
    if (gs.trx_active && trx_table_write(words, nwords, dpsm, bpp)) return;
    if (dpsm == PSMCT24 || dpsm == PSMZ24) {
        /* 3 bytes per pixel, packed across words */
        for (i = 0; i < nwords && gs.trx_active; i++) {
            int k;
            for (k = 0; k < 4; k++) {
                gs.trx_partial[0] |= ((words[i] >> (k * 8)) & 0xFF) << (gs.trx_partial_bytes * 8);
                if (++gs.trx_partial_bytes == 3) {
                    trx_put_pixel(gs.trx_partial[0]);
                    gs.trx_partial[0] = 0;
                    gs.trx_partial_bytes = 0;
                }
            }
        }
        return;
    }
    for (i = 0; i < nwords && gs.trx_active; i++) {
        u32 w = words[i];
        int k;
        switch (bpp) {
        case 32: trx_put_pixel(w); break;
        case 16: trx_put_pixel(w & 0xFFFF); trx_put_pixel(w >> 16); break;
        case 8:  for (k = 0; k < 4; k++) trx_put_pixel((w >> (k * 8)) & 0xFF); break;
        case 4:  for (k = 0; k < 8; k++) trx_put_pixel((w >> (k * 4)) & 0xF); break;
        }
    }
}

/* The same pixels to GS memory on the GPU: unpacked into one value per pixel, from the transfer position the CPU
 * started at to where it stopped. */
static void hwreg_gpu(const u32 *words, u32 nwords, u32 l0, u32 partial0, int partial_bytes0)
{
    static u32 *pix;
    static u32 cap;
    u32 dbp = BITS(gs.bitbltbuf, 32, 14), dbw = BITS(gs.bitbltbuf, 48, 6), dpsm = BITS(gs.bitbltbuf, 56, 6);
    u32 dsax = BITS(gs.trxpos, 32, 11), dsay = BITS(gs.trxpos, 48, 11);
    u32 rrw = BITS(gs.trxreg, 0, 12), rrh = BITS(gs.trxreg, 32, 12);
    u32 l1 = gs.trx_active ? gs.trx_y * rrw + gs.trx_x : rrw * rrh, n = l1 - l0, i, k = 0;
    int bpp = gs_psm_bpp(dpsm);
    if (!n || l1 < l0) return;
    if (n > cap) {
        u32 *p = (u32 *)realloc(pix, (size_t)n * 4);
        if (!p) return;
        pix = p; cap = n;
    }
    if (dpsm == PSMCT24 || dpsm == PSMZ24) {
        u32 part = partial0;
        int nb = partial_bytes0, b;
        for (i = 0; i < nwords && k < n; i++)
            for (b = 0; b < 4 && k < n; b++) {
                part |= ((words[i] >> (b * 8)) & 0xFF) << (nb * 8);
                if (++nb == 3) { pix[k++] = part; part = 0; nb = 0; }
            }
    } else {
        for (i = 0; i < nwords && k < n; i++) {
            u32 w = words[i];
            int b;
            switch (bpp) {
            case 16: for (b = 0; b < 2 && k < n; b++) pix[k++] = (w >> (b * 16)) & 0xFFFF; break;
            case 8:  for (b = 0; b < 4 && k < n; b++) pix[k++] = (w >> (b * 8)) & 0xFF; break;
            case 4:  for (b = 0; b < 8 && k < n; b++) pix[k++] = (w >> (b * 4)) & 0xF; break;
            default: pix[k++] = w; break;
            }
        }
    }
    gs_gpu_upload(dbp, dbw, dpsm, dsax, dsay, rrw, l0, pix, k);
}

void gs_write_hwreg(const u32 *words, u32 nwords)
{
    u32 rrw = BITS(gs.trxreg, 0, 12), l0 = gs.trx_y * rrw + gs.trx_x, partial0 = gs.trx_partial[0];
    int active = gs.trx_active, partial_bytes0 = gs.trx_partial_bytes;
    hwreg_cpu(words, nwords);
    if (gs_gpu_on && active) hwreg_gpu(words, nwords, l0, partial0, partial_bytes0);
}

static void trx_local_to_local(void)
{
    gs_flush();
    u32 sbp = BITS(gs.bitbltbuf, 0, 14), sbw = BITS(gs.bitbltbuf, 16, 6), spsm = BITS(gs.bitbltbuf, 24, 6);
    u32 dbp = BITS(gs.bitbltbuf, 32, 14), dbw = BITS(gs.bitbltbuf, 48, 6), dpsm = BITS(gs.bitbltbuf, 56, 6);
    u32 ssax = BITS(gs.trxpos, 0, 11), ssay = BITS(gs.trxpos, 16, 11);
    u32 dsax = BITS(gs.trxpos, 32, 11), dsay = BITS(gs.trxpos, 48, 11), dir = BITS(gs.trxpos, 59, 2);
    u32 rrw = BITS(gs.trxreg, 0, 12), rrh = BITS(gs.trxreg, 32, 12);
    u32 x, y;
    gs_gpu_copy(sbp, sbw, spsm, ssax, ssay, dbp, dbw, dpsm, dsax, dsay, dir, rrw, rrh);
    if (gs_gpu_only) return;   /* renderer=gpu: GS memory is the GPU's (gs_vram is brought back when read) */
    for (y = 0; y < rrh; y++) {
        u32 yy = (dir & 2) ? rrh - 1 - y : y;
        for (x = 0; x < rrw; x++) {
            u32 xx = (dir & 1) ? rrw - 1 - x : x;
            u32 v = gs_read_pixel(spsm, sbp, sbw, (ssax + xx) & 2047, (ssay + yy) & 2047);
            gs_write_pixel(dpsm, dbp, dbw, (dsax + xx) & 2047, (dsay + yy) & 2047, v);
        }
    }
}

void gs_local_to_host(u32 dst_addr, u32 qwc)
{
    gs_flush();
    if (gs_gpu_only) gs_gpu_read_mem(gs_vram);
    u32 sbp = BITS(gs.bitbltbuf, 0, 14), sbw = BITS(gs.bitbltbuf, 16, 6), spsm = BITS(gs.bitbltbuf, 24, 6);
    u32 ssax = BITS(gs.trxpos, 0, 11), ssay = BITS(gs.trxpos, 16, 11);
    u32 rrw = BITS(gs.trxreg, 0, 12), rrh = BITS(gs.trxreg, 32, 12);
    int bpp = gs_psm_bpp(spsm);
    u32 total = qwc * 16, outpos = 0, x = 0, y = 0;
    u8 *dst = (u8 *)gfx_ee_ptr(dst_addr);
    u32 acc = 0; int accbits = 0;
    if (!dst || !rrw)
        return;
    while (outpos < total && y < rrh) {
        u32 v = gs_read_pixel(spsm, sbp, sbw, (ssax + x) & 2047, (ssay + y) & 2047);
        if (spsm == PSMCT24 || spsm == PSMZ24) {
            dst[outpos++] = (u8)v; if (outpos < total) dst[outpos++] = (u8)(v >> 8);
            if (outpos < total) dst[outpos++] = (u8)(v >> 16);
        } else if (bpp >= 8) {
            int k;
            for (k = 0; k < bpp / 8 && outpos < total; k++)
                dst[outpos++] = (u8)(v >> (k * 8));
        } else {
            acc |= (v & 0xF) << accbits;
            accbits += 4;
            if (accbits == 8) { dst[outpos++] = (u8)acc; acc = 0; accbits = 0; }
        }
        if (++x >= rrw) { x = 0; y++; }
    }
}

/* ---- register writes ------------------------------------------------------------------------------- */

/* DOTR_GS_TRACE=1: every transfer start and every primitive to the log (for replays of captures) */
static int gs_trace(void)
{
    static int on = -1;
    if (on < 0) on = getenv("DOTR_GS_TRACE") != NULL;
    return on;
}

void gs_write_reg(u32 reg, u64 v)
{
    switch (reg) {
    case 0x00: gs.prim = v; gs.vcount = 0; break;
    case 0x01: gs.rgbaq = v; break;
    case 0x02: gs.st = v; break;
    case 0x03: gs.uv = v; break;
    case 0x04: vertex_kick(v, 1, 1); break;
    case 0x05: vertex_kick(v, 0, 1); break;
    case 0x06: case 0x07: gs.ctx[reg - 6].tex0 = v; tex0_written(v); break;
    case 0x08: case 0x09: gs.ctx[reg - 8].clamp = v; break;
    case 0x0A: gs.fog = v; break;
    case 0x0C: vertex_kick(v, 1, 0); break;
    case 0x0D: vertex_kick(v, 0, 0); break;
    case 0x14: case 0x15: gs.ctx[reg - 0x14].tex1 = v; break;
    case 0x16: case 0x17: {
        /* TEX2: replaces PSM and the CLUT fields of TEX0 */
        gs_ctx *c = &gs.ctx[reg - 0x16];
        u64 keep = (0x3FFFull) | (0x3Full << 14) | (0xFull << 26) | (0xFull << 30) | (1ull << 34) | (3ull << 35);
        c->tex0 = (c->tex0 & keep) | (v & ~keep);
        c->tex2 = v;
        tex0_written(c->tex0);
        break;
    }
    case 0x18: case 0x19: gs.ctx[reg - 0x18].xyoffset = v; break;
    case 0x1A: gs.prmodecont = v; break;
    case 0x1B: gs.prmode = v; break;
    case 0x1C: gs.texclut = v; break;
    case 0x22: gs.scanmsk = v; break;
    case 0x34: case 0x35: gs.ctx[reg - 0x34].miptbp1 = v; break;
    case 0x36: case 0x37: gs.ctx[reg - 0x36].miptbp2 = v; break;
    case 0x3B: gs.texa = v; break;
    case 0x3D: gs.fogcol = v; break;
    case 0x3F: gs.texflush = v; break;
    case 0x40: case 0x41: gs.ctx[reg - 0x40].scissor = v; break;
    case 0x42: case 0x43: gs.ctx[reg - 0x42].alpha = v; break;
    case 0x44: gs.dimx = v; break;
    case 0x45: gs.dthe = v; break;
    case 0x46: gs.colclamp = v; break;
    case 0x47: case 0x48: gs.ctx[reg - 0x47].test = v; break;
    case 0x49: gs.pabe = v; break;
    case 0x4A: case 0x4B: gs.ctx[reg - 0x4A].fba = v; break;
    case 0x4C: case 0x4D: gs.ctx[reg - 0x4C].frame = v; break;
    case 0x4E: case 0x4F: gs.ctx[reg - 0x4E].zbuf = v; break;
    case 0x50: gs.bitbltbuf = v; break;
    case 0x51: gs.trxpos = v; break;
    case 0x52: gs.trxreg = v; break;
    case 0x53:
        if (gs_trace())
            hw_log(2, "trx dir %u bitbltbuf %016llx trxpos %016llx trxreg %016llx", (u32)(v & 3),
                   (unsigned long long)gs.bitbltbuf, (unsigned long long)gs.trxpos, (unsigned long long)gs.trxreg);
        gs.trxdir = v;
        gs.trx_x = gs.trx_y = 0;
        gs.trx_partial[0] = 0; gs.trx_partial_bytes = 0;
        switch (v & 3) {
        case 0: gs.trx_active = BITS(gs.trxreg, 0, 12) && BITS(gs.trxreg, 32, 12); trx_changed = 0; break;
        case 1: gs.trx_active = 0; break;  /* local->host is driven by gfx_gs_store_image */
        case 2: gs.trx_active = 0; trx_local_to_local(); break;
        default: gs.trx_active = 0; break;
        }
        break;
    case 0x54: { u32 w[2] = { (u32)v, (u32)(v >> 32) }; gs_write_hwreg(w, 2); break; }
    case 0x60:  /* SIGNAL: ID bits under the mask */
        {
            u32 id = (u32)v, mask = (u32)(v >> 32);
            u32 cur = (u32)gs.siglblid;
            gs.siglblid = (gs.siglblid & 0xFFFFFFFF00000000ull) | ((cur & ~mask) | (id & mask));
            gs.csr |= 1;
            if (!(gs.imr & 0x100)) hw_raise_intc(HW_INTC_GS);
        }
        break;
    case 0x61:
        gs_flush();
        gs.csr |= 2;
        if (!(gs.imr & 0x200)) hw_raise_intc(HW_INTC_GS);
        break;
    case 0x62:
        {
            u32 id = (u32)v, mask = (u32)(v >> 32);
            u32 cur = (u32)(gs.siglblid >> 32);
            gs.siglblid = (gs.siglblid & 0xFFFFFFFFull) | ((u64)((cur & ~mask) | (id & mask)) << 32);
        }
        break;
    default: break;
    }
}

/* ---- privileged registers (EE side) ---------------------------------------------------------------- */

void gfx_gs_priv_write(hw_u32 addr, hw_u64 v)
{
    capture_note_priv(addr, v);
    switch (addr & 0x1FF0) {
    case 0x0000: gs.pmode = v; break;
    case 0x0010: gs.smode1 = v; break;
    case 0x0020: gs.smode2 = v; break;
    case 0x0030: gs.srfsh = v; break;
    case 0x0040: gs.synch1 = v; break;
    case 0x0050: gs.synch2 = v; break;
    case 0x0060: gs.syncv = v; break;
    case 0x0070: gs.dispfb[0] = v; break;
    case 0x0080: gs.display[0] = v; break;
    case 0x0090: gs.dispfb[1] = v; break;
    case 0x00A0: gs.display[1] = v; break;
    case 0x00B0: gs.extbuf = v; break;
    case 0x00C0: gs.extdata = v; break;
    case 0x00D0: gs.extwrite = v; break;
    case 0x00E0: gs.bgcolor = v; break;
    case 0x1000:
        /* CSR: writing 1 to SIGNAL/FINISH/HSINT/VSINT/EDWINT clears them; RESET bit resets the GS */
        gs.csr &= ~(v & 0x1F);
        if (v & 0x200) {
            u64 keep = gs.csr & ~0x1Full;
            gs.vcount = 0;
            gs.csr = keep;
        }
        if (v & 0x100) gs.csr |= 0x100; else gs.csr &= ~0x100ull;  /* FLUSH */
        break;
    case 0x1010: gs.imr = v; break;
    case 0x1040: gs.busdir = v; break;
    case 0x1080: gs.siglblid = v; break;
    default: break;
    }
}

hw_u64 gfx_gs_priv_read(hw_u32 addr)
{
    switch (addr & 0x1FF0) {
    case 0x1000: {
        /* FIELD bit toggles with the vblank count; FIFO reports empty */
        u64 v = (gs.csr & ~(0x3ull << 14)) | (1ull << 14);
        if (hw_vblank_count() & 1) v |= 0x2000; else v &= ~0x2000ull;
        return v;
    }
    case 0x1010: return gs.imr;
    case 0x1040: return gs.busdir;
    case 0x1080: return gs.siglblid;
    case 0x0000: return gs.pmode;
    case 0x0020: return gs.smode2;
    case 0x0070: return gs.dispfb[0];
    case 0x0080: return gs.display[0];
    case 0x0090: return gs.dispfb[1];
    case 0x00A0: return gs.display[1];
    case 0x00E0: return gs.bgcolor;
    default: return 0;
    }
}

void gfx_gs_store_image(hw_u64 bitbltbuf, hw_u64 trxpos, hw_u64 trxreg, hw_u32 dst_addr, hw_u32 qwc)
{
    capture_note_store(bitbltbuf, trxpos, trxreg, dst_addr, qwc);
    gs.bitbltbuf = bitbltbuf;
    gs.trxpos = trxpos;
    gs.trxreg = trxreg;
    gs_local_to_host(dst_addr, qwc);
}
