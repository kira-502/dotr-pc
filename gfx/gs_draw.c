/* gs_draw.c - software GS rasterizer: points, lines, triangles, sprites; texture, fog, tests, blending. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <emmintrin.h>
#include "gfx_internal.h"
#include "gs_fast.h"
#include "gs_par.h"
#include "gpu/gs_gpu.h"

#define BITS(v, lo, n) ((u32)(((v) >> (lo)) & ((1ull << (n)) - 1)))

/* Everything the per-pixel pipeline needs, decoded once per primitive. */
typedef struct draw_env {
    /* primitive */
    int tme, fge, abe, fst, iip, aa1;
    /* frame / z */
    u32 fbp, fbw, fpsm, fbmsk;
    u32 zbp, zpsm, zmsk;
    /* test */
    int ate, atst, aref, afail, date, datm, zte, ztst;
    /* scissor */
    int scx0, scx1, scy0, scy1;
    /* alpha */
    int a, b, c, d, fix, pabe, fba, colclamp, dthe, scanmsk;
    s8 dimx[4][4];
    /* texture */
    u32 tbp, tbw, tpsm, tw, th, tcc, tfx, cpsm, csa;
    int wms, wmt, minu, maxu, minv, maxv;
    int mmag, mmin, lcm, mxl, lodk, lodl;
    u32 mtbp[7], mtbw[7];      /* the mip levels' base and width (MIPTBP1/2, as at the kick) */
    u32 ta0, ta1, aem;
    u8 fogr, fogg, fogb;
    u32 zmax;
    /* fast paths */
    const gs_fbtab *fbt, *zbt;
    const u32 *tex[7];
    int texw[7], texh[7];
    int par;        /* 1 while rows run on several threads: no cache fills */
    const u32 *fast_tex;   /* base level, when its wrap modes are plain repeat/clamp: the fast sampler */
    int ty0, ty1, tx0, tx1;   /* base-level rows and columns this primitive can sample (decoded before it draws) */
    int fast_w, fast_h, min_lin;
} draw_env;

static void env_setup(draw_env *e)
{
    u64 attr = (gs.prmodecont & 1) ? gs.prim : ((gs.prim & 7) | (gs.prmode & 0x7F8));
    const gs_ctx *c = &gs.ctx[(attr >> 9) & 1];
    int i, j;
    e->iip = (int)((attr >> 3) & 1);
    e->tme = (int)((attr >> 4) & 1);
    e->fge = (int)((attr >> 5) & 1);
    e->abe = (int)((attr >> 6) & 1);
    e->aa1 = (int)((attr >> 7) & 1);
    e->fst = (int)((attr >> 8) & 1);

    e->fbp = BITS(c->frame, 0, 9) * 32; e->fbw = BITS(c->frame, 16, 6); e->fpsm = BITS(c->frame, 24, 6);
    e->fbmsk = (u32)(c->frame >> 32);
    e->zbp = BITS(c->zbuf, 0, 9) * 32; e->zpsm = BITS(c->zbuf, 24, 4) | 0x30; e->zmsk = BITS(c->zbuf, 32, 1);
    e->zmax = (e->zpsm == PSMZ32) ? 0xFFFFFFFFu : (e->zpsm == PSMZ24) ? 0xFFFFFFu : 0xFFFFu;

    e->ate = BITS(c->test, 0, 1); e->atst = BITS(c->test, 1, 3); e->aref = BITS(c->test, 4, 8);
    e->afail = BITS(c->test, 12, 2); e->date = BITS(c->test, 14, 1); e->datm = BITS(c->test, 15, 1);
    e->zte = BITS(c->test, 16, 1); e->ztst = BITS(c->test, 17, 2);

    e->scx0 = BITS(c->scissor, 0, 11); e->scx1 = BITS(c->scissor, 16, 11);
    e->scy0 = BITS(c->scissor, 32, 11); e->scy1 = BITS(c->scissor, 48, 11);

    e->a = BITS(c->alpha, 0, 2); e->b = BITS(c->alpha, 2, 2); e->c = BITS(c->alpha, 4, 2); e->d = BITS(c->alpha, 6, 2);
    e->fix = BITS(c->alpha, 32, 8);
    e->pabe = (int)(gs.pabe & 1);
    e->fba = (int)(c->fba & 1);
    e->colclamp = (int)(gs.colclamp & 1);
    e->dthe = (int)(gs.dthe & 1);
    e->scanmsk = (int)(gs.scanmsk & 3);   /* as at the kick: queued rows are drawn later */
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++) {
            int v = (int)BITS(gs.dimx, i * 16 + j * 4, 3);
            e->dimx[i][j] = (s8)(v >= 4 ? v - 8 : v);
        }

    e->tbp = BITS(c->tex0, 0, 14); e->tbw = BITS(c->tex0, 14, 6); e->tpsm = BITS(c->tex0, 20, 6);
    e->tw = BITS(c->tex0, 26, 4); e->th = BITS(c->tex0, 30, 4);
    if (e->tw > 10) e->tw = 10;
    if (e->th > 10) e->th = 10;
    e->tcc = BITS(c->tex0, 34, 1); e->tfx = BITS(c->tex0, 35, 2);
    e->cpsm = BITS(c->tex0, 51, 4); e->csa = BITS(c->tex0, 56, 5);
    e->wms = BITS(c->clamp, 0, 2); e->wmt = BITS(c->clamp, 2, 2);
    e->minu = BITS(c->clamp, 4, 10); e->maxu = BITS(c->clamp, 14, 10);
    e->minv = BITS(c->clamp, 24, 10); e->maxv = BITS(c->clamp, 34, 10);
    e->lcm = BITS(c->tex1, 0, 1); e->mxl = BITS(c->tex1, 2, 3); e->mmag = BITS(c->tex1, 5, 1);
    e->mmin = BITS(c->tex1, 6, 3); e->lodl = BITS(c->tex1, 19, 2);
    { int k = (int)BITS(c->tex1, 32, 12); e->lodk = k >= 2048 ? k - 4096 : k; }
    for (i = 1; i <= 6; i++) {
        u64 src = i <= 3 ? c->miptbp1 : c->miptbp2;
        int sh = ((i - 1) % 3) * 20;
        e->mtbp[i] = BITS(src, sh, 14); e->mtbw[i] = BITS(src, sh + 14, 6);
    }
    e->ta0 = BITS(gs.texa, 0, 8); e->aem = BITS(gs.texa, 15, 1); e->ta1 = BITS(gs.texa, 32, 8);
    e->fogr = (u8)gs.fogcol; e->fogg = (u8)(gs.fogcol >> 8); e->fogb = (u8)(gs.fogcol >> 16);
    e->fbt = gs_fbtab_get(e->fbp, e->fbw, e->fpsm);
    e->zbt = gs_fbtab_get(e->zbp, e->fbw, e->zpsm);
    for (i = 0; i < 7; i++) e->tex[i] = NULL;
    e->par = 0;
    e->fast_tex = NULL;
    e->ty0 = 0; e->ty1 = 0x7FFFFFFF; e->tx0 = 0; e->tx1 = 0x7FFFFFFF;
}

/* ---- frame / z buffer access (table fast path, exact fallback) -------------------------------------- */

static __forceinline u32 buf_get(const gs_fbtab *t, u32 psm, u32 bp, u32 bw, int x, int y)
{
    if (t) {
        u32 a = (t->yo[y & 2047] + t->xo[x & 2047]) & t->mask;
        return t->mask == GS_MEM_WORDS - 1 ? gs_vram[a] : ((u16 *)gs_vram)[a];
    }
    return gs_read_pixel(psm, bp, bw, (u32)x, (u32)y);
}

/* write the full storage unit (32-bit word or 16-bit halfword); 24-bit formats pass the merged word */
static __forceinline void buf_put(const gs_fbtab *t, u32 psm, u32 bp, u32 bw, int x, int y, u32 v)
{
    if (t) {
        u32 a = (t->yo[y & 2047] + t->xo[x & 2047]) & t->mask;
        if (t->mask == GS_MEM_WORDS - 1) gs_vram[a] = v; else ((u16 *)gs_vram)[a] = (u16)v;
        return;
    }
    gs_write_pixel(psm == PSMCT24 ? PSMCT32 : psm == PSMZ24 ? PSMZ32 : psm, bp, bw, (u32)x, (u32)y, v);
}

#define FB_GET(e, x, y) buf_get((e)->fbt, (e)->fpsm, (e)->fbp, (e)->fbw, (x), (y))
#define FB_PUT(e, x, y, v) buf_put((e)->fbt, (e)->fpsm, (e)->fbp, (e)->fbw, (x), (y), (v))
#define Z_GET(e, x, y) buf_get((e)->zbt, (e)->zpsm, (e)->zbp, (e)->fbw, (x), (y))
#define Z_PUT(e, x, y, v) buf_put((e)->zbt, (e)->zpsm, (e)->zbp, (e)->fbw, (x), (y), (v))

/* ---- texture --------------------------------------------------------------------------------------- */

static u32 expand16(const draw_env *e, u32 c)
{
    u32 r = (c & 0x1F) << 3, g = ((c >> 5) & 0x1F) << 3, b = ((c >> 10) & 0x1F) << 3, a;
    if (c & 0x8000) a = e->ta1;
    else a = (e->aem && !(c & 0x7FFF)) ? 0 : e->ta0;
    return r | (g << 8) | (b << 16) | (a << 24);
}

static u32 clut_lookup(const draw_env *e, u32 idx, int is4)
{
    if (e->cpsm == PSMCT16 || e->cpsm == PSMCT16S) {
        u32 i = is4 ? (e->csa * 16 + idx) & 511 : idx & 255;
        return expand16(e, gs.clut[i]);
    } else {
        u32 i = is4 ? ((e->csa & 15) * 16 + idx) & 255 : idx & 255;
        return gs.clut[i] | ((u32)gs.clut[i + 256] << 16);
    }
}

/* One texel in RGBA8888 (alpha in the top byte, 0..255 as stored; 0x80 = 1.0). */
static u32 texel_slow(const draw_env *e, u32 tbp, u32 tbw, int x, int y)
{
    u32 v;
    switch (e->tpsm) {
    case PSMCT32: case PSMZ32:
        return gs_read_pixel(e->tpsm, tbp, tbw, x, y);
    case PSMCT24: case PSMZ24:
        v = gs_read_pixel(e->tpsm, tbp, tbw, x, y);
        return v | (((e->aem && !(v & 0xFFFFFF)) ? 0 : e->ta0) << 24);
    case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S:
        return expand16(e, gs_read_pixel(e->tpsm, tbp, tbw, x, y));
    case PSMT8: case PSMT8H:
        return clut_lookup(e, gs_read_pixel(e->tpsm, tbp, tbw, x, y), 0);
    case PSMT4: case PSMT4HL: case PSMT4HH:
        return clut_lookup(e, gs_read_pixel(e->tpsm, tbp, tbw, x, y), 1);
    default:
        return gs_read_pixel(PSMCT32, tbp, tbw, x, y);
    }
}

/* Decoded texture of a mip level, fetched once per primitive. */
static const u32 *level_tex(draw_env *e, int lod, u32 tbp, u32 tbw, int w, int h)
{
    static int nocache = -1;   /* DOTR_GS_NOTEXCACHE=1: every texel read straight from GS memory (checking the cache) */
    if (nocache < 0) nocache = getenv("DOTR_GS_NOTEXCACHE") != NULL;
    if (nocache) return NULL;
    if (lod < 0) lod = 0;
    if (lod > 6) lod = 6;
    if (e->par) return (e->tex[lod] && e->texw[lod] == w && e->texh[lod] == h) ? e->tex[lod] : NULL;
    if (!e->tex[lod] || e->texw[lod] != w || e->texh[lod] != h) {
        e->tex[lod] = gs_texture(tbp, tbw, e->tpsm, (u32)w, (u32)h, e->cpsm, e->csa,
                                 e->ta0 | (e->ta1 << 8) | (e->aem << 16),
                                 lod == 0 ? (u32)e->ty0 : 0, lod == 0 ? (u32)e->ty1 : (u32)h - 1,
                                 lod == 0 ? (u32)e->tx0 : 0, lod == 0 ? (u32)e->tx1 : (u32)w - 1);
        e->texw[lod] = w; e->texh[lod] = h;
    }
    return e->tex[lod];
}

static int wrap(int c, int size, int mode, int mn, int mx)
{
    switch (mode) {
    case 0: return c & (size - 1);
    case 1: return c < 0 ? 0 : c >= size ? size - 1 : c;
    case 2: return c < mn ? mn : c > mx ? mx : c;
    default: return (c & mn) | mx;
    }
}

static void level_params(const draw_env *e, int lod, u32 *tbp, u32 *tbw, int *w, int *h)
{
    *w = 1 << e->tw; *h = 1 << e->th; *tbp = e->tbp; *tbw = e->tbw;
    if (lod > 0) {
        if (lod > 6) lod = 6;
        *tbp = e->mtbp[lod];
        *tbw = e->mtbw[lod];
        *w >>= lod; *h >>= lod;
        if (*w < 1) *w = 1;
        if (*h < 1) *h = 1;
    }
}

/* Fill the texture cache for every level this primitive may sample, before rows go parallel. */
static void tex_prefetch(draw_env *e)
{
    int lod;
    e->fast_tex = NULL;
    for (lod = 0; lod <= e->mxl && lod <= 6; lod++) {
        u32 tbp, tbw; int w, h;
        level_params(e, lod, &tbp, &tbw, &w, &h);
        level_tex(e, lod, tbp, tbw, w, h);
    }
    if (e->tex[0] && e->wms <= 1 && e->wmt <= 1) {
        e->fast_tex = e->tex[0];
        e->fast_w = 1 << e->tw; e->fast_h = 1 << e->th;
    }
    e->min_lin = (e->mmin == 1 || e->mmin == 4 || e->mmin == 5);
}

static __forceinline int ifloor(float f) { int i = (int)f; return i - (f < (float)i); }

/* Base-level sampling with repeat or clamp wrapping, in fixed point (4 fractional bits, as the GS filters). */
static inline int fwrap(int c, int size, int mode) { return mode == 0 ? c & (size - 1) : c < 0 ? 0 : c >= size ? size - 1 : c; }

static __forceinline u32 sample_fast(const draw_env *e, float u, float v, int linear)
{
    const u32 *tx = e->fast_tex;
    int w = e->fast_w, h = e->fast_h;
    if (!linear) {
        int x = ifloor(u), y = ifloor(v);
        return tx[fwrap(y, h, e->wmt) * w + fwrap(x, w, e->wms)];
    } else {
        int fu = ifloor((u - 0.5f) * 16.0f), fv = ifloor((v - 0.5f) * 16.0f);
        int x0 = fu >> 4, y0 = fv >> 4, au = fu & 15, av = fv & 15;
        int xa = fwrap(x0, w, e->wms), xb = fwrap(x0 + 1, w, e->wms);
        const u32 *ra = tx + fwrap(y0, h, e->wmt) * w, *rb = tx + fwrap(y0 + 1, h, e->wmt) * w;
        u32 t00 = ra[xa], t10 = ra[xb], t01 = rb[xa], t11 = rb[xb];
        /* all four weights sum to 256, so each 16-bit lane (R|B, G|A) stays below 65536: two channels per
         * multiply, with the same result as channel by channel */
        u32 w00 = (u32)((16 - au) * (16 - av)), w10 = (u32)(au * (16 - av)), w01 = (u32)((16 - au) * av), w11 = (u32)(au * av);
        u32 m = 0x00FF00FFu;
        u32 lrb = ((t00 & m) * w00 + (t10 & m) * w10 + (t01 & m) * w01 + (t11 & m) * w11) >> 8 & m;
        u32 ga = (((t00 >> 8) & m) * w00 + ((t10 >> 8) & m) * w10 + ((t01 >> 8) & m) * w01 + ((t11 >> 8) & m) * w11) & ~m;
        return lrb | ga;
    }
}

static u32 sample(draw_env *e, float u, float v, int lod)
{
    int w, h;
    u32 tbp, tbw;
    int linear;
    const u32 *tx;
    level_params(e, lod, &tbp, &tbw, &w, &h);
    if (lod > 0) {
        u /= (float)(1 << lod);
        v /= (float)(1 << lod);
        linear = (e->mmin == 1 || e->mmin == 4 || e->mmin == 5);
    } else {
        linear = (lod < 0) ? e->mmag : (e->mmin == 1 || e->mmin == 4 || e->mmin == 5);
    }
    tx = level_tex(e, lod, tbp, tbw, w, h);
    /* the region modes can reach beyond the texture's size: the GS then reads the GS memory there, which the
     * decoded copy does not hold */
#define TEXEL(xx, yy) (tx && (unsigned)(xx) < (unsigned)w && (unsigned)(yy) < (unsigned)h ? tx[(yy) * w + (xx)] \
                                                                                           : texel_slow(e, tbp, tbw, (xx), (yy)))
    if (!linear) {
        int x = wrap((int)floorf(u), w, e->wms, e->minu, e->maxu);
        int y = wrap((int)floorf(v), h, e->wmt, e->minv, e->maxv);
        return TEXEL(x, y);
    } else {
        float fu = u - 0.5f, fv = v - 0.5f;
        int x0 = (int)floorf(fu), y0 = (int)floorf(fv);
        int au = (int)((fu - (float)x0) * 16.0f), av = (int)((fv - (float)y0) * 16.0f);
        int xa = wrap(x0, w, e->wms, e->minu, e->maxu), xb = wrap(x0 + 1, w, e->wms, e->minu, e->maxu);
        int ya = wrap(y0, h, e->wmt, e->minv, e->maxv), yb = wrap(y0 + 1, h, e->wmt, e->minv, e->maxv);
        u32 t00 = TEXEL(xa, ya), t10 = TEXEL(xb, ya);
        u32 t01 = TEXEL(xa, yb), t11 = TEXEL(xb, yb);
        u32 out = 0;
        int k;
        for (k = 0; k < 32; k += 8) {
            int c00 = (t00 >> k) & 0xFF, c10 = (t10 >> k) & 0xFF, c01 = (t01 >> k) & 0xFF, c11 = (t11 >> k) & 0xFF;
            int top = c00 * (16 - au) + c10 * au, bot = c01 * (16 - au) + c11 * au;
            int c = (top * (16 - av) + bot * av) >> 8;
            out |= (u32)c << k;
        }
        return out;
    }
#undef TEXEL
}

/* ---- the pixel pipeline ---------------------------------------------------------------------------- */

static inline int clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

static u32 fb_read(const draw_env *e, int x, int y, int *ad)
{
    u32 d = FB_GET(e, x, y);
    switch (e->fpsm) {
    case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S:
        *ad = (d & 0x8000) ? 0x80 : 0;
        return ((d & 0x1F) << 3) | (((d >> 5) & 0x1F) << 11) | (((d >> 10) & 0x1F) << 19);
    case PSMCT24: case PSMZ24:
        *ad = 0x80;
        return d & 0xFFFFFF;
    default:
        *ad = (int)(d >> 24);
        return d & 0xFFFFFF;
    }
}

typedef struct pix_in {
    int r, g, b, a;
    u32 z;
    int f;
    float u, v;     /* texel coordinates (already divided, in texels) */
    int lod;
} pix_in;

static __forceinline void pixel_dest(draw_env *e, int x, int y, int r, int g, int b, int a, u32 z, int write_fb,
                                      int write_z, int write_rgb_only);

/* The source side of a pixel: texture, fog and the alpha test. Returns 0 when the alpha test drops it. */
static __forceinline int pixel_src(draw_env *e, const pix_in *p, int *pr, int *pg, int *pb, int *pa, int *write_fb,
                                   int *write_z, int *write_rgb_only)
{
    int r = p->r, g = p->g, b = p->b, a = p->a;

    /* texture + texture function */
    if (e->tme) {
        u32 t = (p->lod <= 0 && e->fast_tex) ? sample_fast(e, p->u, p->v, p->lod < 0 ? e->mmag : e->min_lin) : sample(e, p->u, p->v, p->lod);
        int tr = t & 0xFF, tg = (t >> 8) & 0xFF, tb = (t >> 16) & 0xFF, ta = t >> 24;
        switch (e->tfx) {
        case 0:  /* modulate */
            r = clamp255((r * tr) >> 7); g = clamp255((g * tg) >> 7); b = clamp255((b * tb) >> 7);
            a = e->tcc ? clamp255((a * ta) >> 7) : a;
            break;
        case 1:  /* decal */
            r = tr; g = tg; b = tb; a = e->tcc ? ta : a;
            break;
        case 2:  /* highlight */
            r = clamp255(((r * tr) >> 7) + a); g = clamp255(((g * tg) >> 7) + a); b = clamp255(((b * tb) >> 7) + a);
            a = e->tcc ? clamp255(ta + a) : a;
            break;
        default: /* highlight2 */
            r = clamp255(((r * tr) >> 7) + a); g = clamp255(((g * tg) >> 7) + a); b = clamp255(((b * tb) >> 7) + a);
            a = e->tcc ? ta : a;
            break;
        }
    }
    /* fog */
    if (e->fge) {
        int f = p->f;
        r = (f * r + (255 - f) * e->fogr) >> 8;
        g = (f * g + (255 - f) * e->fogg) >> 8;
        b = (f * b + (255 - f) * e->fogb) >> 8;
    }
    /* alpha test */
    if (e->ate) {
        int pass;
        switch (e->atst) {
        case 0: pass = 0; break;
        case 1: pass = 1; break;
        case 2: pass = a < e->aref; break;
        case 3: pass = a <= e->aref; break;
        case 4: pass = a == e->aref; break;
        case 5: pass = a >= e->aref; break;
        case 6: pass = a > e->aref; break;
        default: pass = a != e->aref; break;
        }
        if (!pass) {
            switch (e->afail) {
            case 0: return 0;
            case 1: *write_z = 0; break;
            case 2: *write_fb = 0; break;
            default: *write_z = 0; *write_rgb_only = 1; break;
            }
        }
    }
    *pr = r; *pg = g; *pb = b; *pa = a;
    return 1;
}

static __forceinline void put_pixel(draw_env *e, int x, int y, pix_in *p)
{
    int r, g, b, a, write_fb = 1, write_z = !e->zmsk, write_rgb_only = 0;
    if (x < e->scx0 || x > e->scx1 || y < e->scy0 || y > e->scy1)
        return;
    if (e->scanmsk == 2 && !(y & 1)) return;
    if (e->scanmsk == 3 && (y & 1)) return;
    if (!pixel_src(e, p, &r, &g, &b, &a, &write_fb, &write_z, &write_rgb_only)) return;
    pixel_dest(e, x, y, r, g, b, a, p->z, write_fb, write_z, write_rgb_only);
}

/* The destination side: destination alpha and depth tests, blending, dithering, the writes. */
static __forceinline void pixel_dest(draw_env *e, int x, int y, int r, int g, int b, int a, u32 z, int write_fb,
                                      int write_z, int write_rgb_only)
{
    u32 zaddr_v = 0;
    int ad = 0, is16;
    u32 dcol = 0;
    is16 = (e->fpsm == PSMCT16 || e->fpsm == PSMCT16S || e->fpsm == PSMZ16 || e->fpsm == PSMZ16S);
    /* destination alpha test */
    if (e->date && e->fpsm != PSMCT24 && e->fpsm != PSMZ24) {
        u32 d = FB_GET(e, x, y);
        int bit = is16 ? (int)((d >> 15) & 1) : (int)((d >> 31) & 1);
        if (bit != e->datm)
            return;
    }
    /* depth test */
    if (z > e->zmax) z = e->zmax;
    if (e->zte) {
        if (e->ztst == 0) return;
        if (e->ztst >= 2) {
            zaddr_v = Z_GET(e, x, y);
            if (e->zpsm == PSMZ24) zaddr_v &= 0xFFFFFF;
            if (e->ztst == 2 ? z < zaddr_v : z <= zaddr_v)
                return;
        }
    } else {
        write_z = 0;
    }
    /* blending */
    if (e->abe && !(e->pabe && !(a & 0x80))) {
        int cs[3] = { r, g, b }, cd[3], out[3], k;
        int cc;
        dcol = fb_read(e, x, y, &ad);
        cd[0] = dcol & 0xFF; cd[1] = (dcol >> 8) & 0xFF; cd[2] = (dcol >> 16) & 0xFF;
        cc = e->c == 0 ? a : e->c == 1 ? ad : e->fix;
        for (k = 0; k < 3; k++) {
            int va = e->a == 0 ? cs[k] : e->a == 1 ? cd[k] : 0;
            int vb = e->b == 0 ? cs[k] : e->b == 1 ? cd[k] : 0;
            int vd = e->d == 0 ? cs[k] : e->d == 1 ? cd[k] : 0;
            out[k] = (((va - vb) * cc) >> 7) + vd;
        }
        r = out[0]; g = out[1]; b = out[2];
    }
    if (e->dthe && is16) {
        int dv = e->dimx[y & 3][x & 3];
        r += dv; g += dv; b += dv;
    }
    if (e->colclamp) { r = clamp255(r); g = clamp255(g); b = clamp255(b); }
    else { r &= 0xFF; g &= 0xFF; b &= 0xFF; }
    if (e->fba) a |= 0x80;

    if (write_fb) {
        u32 msk = e->fbmsk, out, old;
        if (write_rgb_only) msk |= 0xFF000000u;
        if (is16) {
            u32 m16 = ((msk >> 3) & 0x1F) | (((msk >> 11) & 0x1F) << 5) | (((msk >> 19) & 0x1F) << 10) | ((msk >> 31) << 15);
            out = (u32)((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10) | ((a & 0x80) << 8));
            if (m16) {
                old = FB_GET(e, x, y);
                out = (out & ~m16) | (old & m16);
            }
        } else {
            out = (u32)r | ((u32)g << 8) | ((u32)b << 16) | ((u32)(a & 0xFF) << 24);
            if (e->fpsm == PSMCT24 || e->fpsm == PSMZ24) msk |= 0xFF000000u;
            if (msk) {
                old = e->fbt ? FB_GET(e, x, y) : gs_read_pixel(e->fpsm == PSMCT24 ? PSMCT32 : e->fpsm == PSMZ24 ? PSMZ32 : e->fpsm,
                                    e->fbp, e->fbw, (u32)x, (u32)y);
                out = (out & ~msk) | (old & msk);
            }
        }
        FB_PUT(e, x, y, out);
    }
    if (write_z) {
        if (e->zpsm == PSMZ24) z = (Z_GET(e, x, y) & 0xFF000000u) | (z & 0xFFFFFF);
        Z_PUT(e, x, y, z);
    }
}

/* ---- primitives ------------------------------------------------------------------------------------ */

static float vtx_q(const gs_vertex *v) { return v->q == 0.0f ? 1.0f : v->q; }

/* Texel coordinates and LOD for a pixel, from interpolated attributes. */
/* log2|q| as the GPU renderer computes it too (exponent plus the mantissa's fraction, piecewise linear like the
 * GS's own LOD unit), so both renderers pick the same mip level */
static float lod_log2(float q)
{
    u32 u;
    memcpy(&u, &q, 4);
    u &= 0x7FFFFFFFu;
    return (float)((int)(u >> 23) - 127) + (float)(u & 0x7FFFFFu) * (1.0f / 8388608.0f);
}

static void tex_coords(draw_env *e, pix_in *p, float s, float t, float q, float uu, float vv)
{
    if (e->fst) {
        p->u = uu / 16.0f;
        p->v = vv / 16.0f;
        p->lod = (e->mxl == 0) ? -1 : 0;
        if (e->mxl && e->lcm) {
            int lod = (e->lodk + 8) >> 4;
            p->lod = lod <= 0 ? -1 : lod > e->mxl ? e->mxl : lod;
        }
    } else {
        float qq = q == 0.0f ? 1.0f : q, rq = 1.0f / qq;
        p->u = s * rq * (float)(1 << e->tw);
        p->v = t * rq * (float)(1 << e->th);
        p->lod = -1;
        if (e->mxl) {
            float lodf;
            if (e->lcm) lodf = (float)e->lodk / 16.0f;
            else lodf = -lod_log2(qq) * (float)(1 << e->lodl) + (float)e->lodk / 16.0f;
            if (lodf > 0.0f) {
                int l = (int)(lodf + 0.5f);
                p->lod = l > e->mxl ? e->mxl : (l == 0 ? 0 : l);
            }
        }
    }
}

typedef struct sprite_job {
    draw_env *e;
    const gs_vertex *v1;
    int px0, px1;
    s32 y0, y1;
    float t0, t1, du, tx0;
} sprite_job;

static void sprite_rows(void *ctx, int ya, int yb)
{
    const sprite_job *j = (const sprite_job *)ctx;
    draw_env le = *j->e, *e = &le;   /* a local copy: the frame buffer writes cannot alias it */
    pix_in p;
    int x, y;
    p.r = j->v1->r; p.g = j->v1->g; p.b = j->v1->b; p.a = j->v1->a; p.z = j->v1->z; p.f = j->v1->f;
    p.lod = -1;
    p.u = p.v = 0;
    if (!e->tme) {   /* every pixel has the same source colour, fog and alpha test result */
        int r, g, b, a, write_fb = 1, write_z = !e->zmsk, write_rgb_only = 0;
        int x0 = j->px0 < e->scx0 ? e->scx0 : j->px0, x1 = j->px1 > e->scx1 + 1 ? e->scx1 + 1 : j->px1;
        if (!pixel_src(e, &p, &r, &g, &b, &a, &write_fb, &write_z, &write_rgb_only)) return;
        for (y = ya; y < yb; y++) {
            if (y < e->scy0 || y > e->scy1) continue;
            if (e->scanmsk == 2 && !(y & 1)) continue;
            if (e->scanmsk == 3 && (y & 1)) continue;
            for (x = x0; x < x1; x++) pixel_dest(e, x, y, r, g, b, a, p.z, write_fb, write_z, write_rgb_only);
        }
        return;
    }
    for (y = ya; y < yb; y++) {
        float ty = (j->y1 == j->y0) ? j->t0 : j->t0 + (j->t1 - j->t0) * (float)((y << 4) - j->y0) / (float)(j->y1 - j->y0);
        for (x = j->px0; x < j->px1; x++) {
            if (e->tme) {
                float tx = j->tx0 + j->du * (float)(x - j->px0);
                p.u = tx / 16.0f; p.v = ty / 16.0f;
                p.lod = -1;
                if (e->mxl && e->lcm) {
                    int lod = (e->lodk + 8) >> 4;
                    p.lod = lod <= 0 ? -1 : lod > e->mxl ? e->mxl : lod;
                }
            }
            put_pixel(e, x, y, &p);
        }
    }
}

/* Fill a sprite job; returns the covered rows [*ry0, *ry1). */
static void setup_sprite(draw_env *e, const gs_vertex *v0, const gs_vertex *v1, sprite_job *j, int *ry0, int *ry1)
{
    s32 x0 = v0->x, x1 = v1->x, y0 = v0->y, y1 = v1->y;
    float u0, u1, t0, t1;
    int px0, px1, py0, py1, x = 0, y = 0;
    if (e->fst) {
        u0 = (float)v0->u; u1 = (float)v1->u; t0 = (float)v0->v; t1 = (float)v1->v;
    } else {
        u0 = v0->s / vtx_q(v0) * (float)(1 << e->tw) * 16.0f;
        u1 = v1->s / vtx_q(v1) * (float)(1 << e->tw) * 16.0f;
        t0 = v0->t / vtx_q(v0) * (float)(1 << e->th) * 16.0f;
        t1 = v1->t / vtx_q(v1) * (float)(1 << e->th) * 16.0f;
    }
    if (x1 < x0) { s32 tt = x0; float tf = u0; x0 = x1; x1 = tt; u0 = u1; u1 = tf; }
    if (y1 < y0) { s32 tt = y0; float tf = t0; y0 = y1; y1 = tt; t0 = t1; t1 = tf; }
    px0 = (x0 + 15) >> 4; px1 = (x1 + 15) >> 4;   /* covers [px0, px1) */
    py0 = (y0 + 15) >> 4; py1 = (y1 + 15) >> 4;
    if (px0 < e->scx0) px0 = e->scx0;
    if (py0 < e->scy0) py0 = e->scy0;
    if (px1 > e->scx1 + 1) px1 = e->scx1 + 1;
    if (py1 > e->scy1 + 1) py1 = e->scy1 + 1;
    j->e = e; j->v1 = v1; j->px0 = px0; j->px1 = px1; j->y0 = y0; j->y1 = y1; j->t0 = t0; j->t1 = t1;
    j->du = (x1 == x0) ? 0.0f : (u1 - u0) * 16.0f / (float)(x1 - x0);
    j->tx0 = (x1 == x0) ? u0 : u0 + (u1 - u0) * (float)((px0 << 4) - x0) / (float)(x1 - x0);
    *ry0 = py0; *ry1 = px1 > px0 ? py1 : py0;
    (void)x; (void)y;
}

typedef struct tri_job {
    draw_env *e;
    const gs_vertex *a, *b, *c, *v2;
    int minx, maxx;
    double inv;
} tri_job;

static void tri_rows(void *ctx, int ya, int yb);

static int setup_tri(draw_env *e, const gs_vertex *v0, const gs_vertex *v1, const gs_vertex *v2, tri_job *j,
                     int *ry0, int *ry1)
{
    s64 area = (s64)(v1->x - v0->x) * (v2->y - v0->y) - (s64)(v2->x - v0->x) * (v1->y - v0->y);
    const gs_vertex *a = v0, *b = v1, *c = v2;
    int minx, maxx, miny, maxy, x, y;
    double inv;
    pix_in p;
    if (area == 0)
        return 0;
    if (area < 0) { b = v2; c = v1; area = -area; }
    minx = a->x; if (b->x < minx) minx = b->x; if (c->x < minx) minx = c->x;
    maxx = a->x; if (b->x > maxx) maxx = b->x; if (c->x > maxx) maxx = c->x;
    miny = a->y; if (b->y < miny) miny = b->y; if (c->y < miny) miny = c->y;
    maxy = a->y; if (b->y > maxy) maxy = b->y; if (c->y > maxy) maxy = c->y;
    minx = (minx + 15) >> 4; maxx = (maxx + 15) >> 4;
    miny = (miny + 15) >> 4; maxy = (maxy + 15) >> 4;
    if (minx < e->scx0) minx = e->scx0;
    if (miny < e->scy0) miny = e->scy0;
    if (maxx > e->scx1 + 1) maxx = e->scx1 + 1;
    if (maxy > e->scy1 + 1) maxy = e->scy1 + 1;
    inv = 1.0 / (double)area;
    j->e = e; j->a = a; j->b = b; j->c = c; j->v2 = v2; j->minx = minx; j->maxx = maxx; j->inv = inv;
    *ry0 = miny; *ry1 = maxx > minx ? maxy : miny;
    (void)x; (void)y; (void)p;
    return 1;
}

static s64 floor_div(s64 a, s64 b) { s64 q = a / b; return (a % b != 0 && a < 0) ? q - 1 : q; }   /* b > 0 */

/* Narrow the steps n (in [*lo, *hi]) for which an edge function W + d*n passes: > 0, or 0 on a top-left edge. */
static void edge_span(s64 w, s64 d, int tl, s64 *lo, s64 *hi)
{
    s64 t = tl ? 0 : 1;   /* passes when W + d*n >= t */
    if (d == 0) { if (w < t) *hi = *lo - 1; return; }
    if (d > 0) { s64 n = -floor_div(w - t, d); if (n > *lo) *lo = n; }   /* n >= ceil((t - W) / d) */
    else { s64 n = floor_div(w - t, -d); if (n < *hi) *hi = n; }
}

static void tri_rows(void *ctx, int ya, int yb)
{
    const tri_job *j = (const tri_job *)ctx;
    draw_env le = *j->e, *e = &le;   /* a local copy: the frame buffer writes cannot alias it */
    const gs_vertex *a = j->a, *b = j->b, *c = j->c, *v2 = j->v2;
    int minx = j->minx, maxx = j->maxx, x, y, k;
    double inv = j->inv;
    pix_in p;
    /* attributes as planes over the edge functions: value = (w0*A_a + w1*A_b + w2*A_c) / area; per pixel to
     * the right the w's change by d0/d1/d2, so each attribute changes by a constant step */
    /* per-pixel steps of each attribute; z keeps double precision (24-32 bits), the rest are floats */
    double za = (double)a->z, zb = (double)b->z, zc = (double)c->z, zstep, zcur = 0;
    float fa[10], fb[10], fc[10], fstep[10], fcur[10];
    double da[10], db[10], dc[10];
    int nf = e->tme ? 10 : 5;
    static int closed_form = -1;
    if (closed_form < 0) closed_form = !(getenv("DOTR_GS_STEPPING") && getenv("DOTR_GS_STEPPING")[0] == '1');
#define TOPLEFT(p0, p1) (((p1)->y == (p0)->y && (p1)->x > (p0)->x) || ((p1)->y < (p0)->y))
    int tl0 = TOPLEFT(b, c), tl1 = TOPLEFT(c, a), tl2 = TOPLEFT(a, b);
#undef TOPLEFT
    s64 d0 = -(s64)(c->y - b->y) * 16, d1 = -(s64)(a->y - c->y) * 16, d2 = -(s64)(b->y - a->y) * 16;
#define SETF(arr, vx) (arr[0] = (vx)->r, arr[1] = (vx)->g, arr[2] = (vx)->b, arr[3] = (vx)->a, arr[4] = (vx)->f, \
                       arr[5] = (vx)->s, arr[6] = (vx)->t, arr[7] = (vx)->q, arr[8] = (float)(vx)->u, arr[9] = (float)(vx)->v)
    SETF(fa, a); SETF(fb, b); SETF(fc, c);
#undef SETF
    for (k = 0; k < 10; k++) { da[k] = fa[k]; db[k] = fb[k]; dc[k] = fc[k]; }
    zstep = ((double)d0 * za + (double)d1 * zb + (double)d2 * zc) * inv;
    for (k = 0; k < nf; k++) fstep[k] = (float)(((double)d0 * fa[k] + (double)d1 * fb[k] + (double)d2 * fc[k]) * inv);
    p.lod = -1; p.u = p.v = 0;
    if (!e->iip) { p.r = v2->r; p.g = v2->g; p.b = v2->b; p.a = v2->a; }
    for (y = ya; y < yb; y++) {
        s32 py = y << 4, px0 = minx << 4;
        /* edge functions at the row's first pixel; top-left fill rule */
        s64 w0 = (s64)(c->x - b->x) * (py - b->y) - (s64)(c->y - b->y) * (px0 - b->x) - d0;
        s64 w1 = (s64)(a->x - c->x) * (py - c->y) - (s64)(a->y - c->y) * (px0 - c->x) - d1;
        s64 w2 = (s64)(b->x - a->x) * (py - a->y) - (s64)(b->y - a->y) * (px0 - a->x) - d2;
        int inside = 0, xe;
        /* the covered pixels of the row are minx-1+n for the steps n where all three edge functions pass */
        s64 nlo = 1, nhi = maxx - minx;
        edge_span(w0, d0, tl0, &nlo, &nhi);
        edge_span(w1, d1, tl1, &nlo, &nhi);
        edge_span(w2, d2, tl2, &nlo, &nhi);
        if (nlo > nhi) continue;
        w0 += d0 * (nlo - 1); w1 += d1 * (nlo - 1); w2 += d2 * (nlo - 1);
        xe = minx + (int)nhi;
        for (x = minx + (int)nlo - 1; x < xe; x++) {
            w0 += d0; w1 += d1; w2 += d2;
            if (!inside || closed_form) {
                /* every pixel from its own edge functions (the GPU renderer's formula), or the row's first pixel
                 * exactly and then steps (DOTR_GS_STEPPING=1, the older way) */
                double dw0 = (double)w0, dw1 = (double)w1, dw2 = (double)w2;
                __m128d v0 = _mm_set1_pd(dw0), v1 = _mm_set1_pd(dw1), v2 = _mm_set1_pd(dw2), vi = _mm_set1_pd(inv);
                zcur = (dw0 * za + dw1 * zb + dw2 * zc) * inv;
                for (k = 0; k < nf; k += 2) {   /* two attributes at a time, the same double operations */
                    __m128d s = _mm_add_pd(_mm_add_pd(_mm_mul_pd(v0, _mm_loadu_pd(da + k)), _mm_mul_pd(v1, _mm_loadu_pd(db + k))),
                                           _mm_mul_pd(v2, _mm_loadu_pd(dc + k)));
                    __m128 f = _mm_cvtpd_ps(_mm_mul_pd(s, vi));
                    _mm_storel_pi((__m64 *)(fcur + k), f);
                }
                inside = 1;
            } else {
                zcur += zstep;
                fcur[0] += fstep[0]; fcur[1] += fstep[1]; fcur[2] += fstep[2]; fcur[3] += fstep[3]; fcur[4] += fstep[4];
                if (nf > 5) {
                    fcur[5] += fstep[5]; fcur[6] += fstep[6]; fcur[7] += fstep[7]; fcur[8] += fstep[8]; fcur[9] += fstep[9];
                }
            }
            if (e->iip) { p.r = (int)fcur[0]; p.g = (int)fcur[1]; p.b = (int)fcur[2]; p.a = (int)fcur[3]; }
            p.z = zcur >= 4294967295.0 ? 0xFFFFFFFFu : zcur <= 0 ? 0 : (u32)zcur;
            p.f = (int)fcur[4];
            if (e->tme) {
                if (e->fst) tex_coords(e, &p, 0, 0, 1, fcur[8], fcur[9]);
                else tex_coords(e, &p, fcur[5], fcur[6], fcur[7], 0, 0);
            }
            put_pixel(e, x, y, &p);
        }
    }
}

#define ROW_MINE(y, fk, fn) ((fn) == 0 || (((unsigned)(y) >> 3) % (unsigned)(fn)) == (unsigned)(fk))   /* 8-row strips */

/* set while the GPU renderer collects a line's or point's pixels (each becomes a one-pixel GPU primitive) */
static void (*pix_sink)(draw_env *e, int x, int y, const pix_in *p);
#define LINE_PIXEL(e, x, y, p) do { if (pix_sink) pix_sink((e), (x), (y), (p)); else put_pixel((e), (x), (y), (p)); } while (0)

static void draw_line(draw_env *e, const gs_vertex *v0, const gs_vertex *v1, int fk, int fn)
{
    s32 dx = v1->x - v0->x, dy = v1->y - v0->y;
    s32 adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    int steps = ((adx > ady ? adx : ady) + 15) >> 4, i;
    pix_in p;
    if (steps == 0) steps = 1;
    for (i = 0; i < steps; i++) {
        double t = (double)i / steps;
        int x = (int)((v0->x + dx * t + 8) / 16.0), y = (int)((v0->y + dy * t + 8) / 16.0);
        const gs_vertex *cv = e->iip ? NULL : v1;
        if (!(e->aa1 && e->abe) && !ROW_MINE(y, fk, fn)) continue;
        if (cv) { p.r = cv->r; p.g = cv->g; p.b = cv->b; p.a = cv->a; }
        else {
            p.r = (int)(v0->r + (v1->r - v0->r) * t); p.g = (int)(v0->g + (v1->g - v0->g) * t);
            p.b = (int)(v0->b + (v1->b - v0->b) * t); p.a = (int)(v0->a + (v1->a - v0->a) * t);
        }
        p.z = (u32)((double)v0->z + ((double)v1->z - (double)v0->z) * t);
        p.f = (int)(v0->f + (v1->f - v0->f) * t);
        if (e->tme) {
            if (e->fst)
                tex_coords(e, &p, 0, 0, 1, (float)(v0->u + ((double)v1->u - v0->u) * t), (float)(v0->v + ((double)v1->v - v0->v) * t));
            else
                tex_coords(e, &p, (float)(v0->s + (v1->s - v0->s) * t), (float)(v0->t + (v1->t - v0->t) * t),
                           (float)(v0->q + (v1->q - v0->q) * t), 0, 0);
        }
        if (e->aa1 && e->abe) {
            /* antialiased (AA1): the two pixels on either side of the ideal line along the minor axis, each with its
             * coverage (0..128) as the alpha the blend uses */
            int xmajor = adx >= ady;
            double m = xmajor ? (v0->y + dy * t) : (v0->x + dx * t);   /* minor coordinate, 1/16 pixel */
            int lo = (int)floor(m / 16.0), cov_hi = (int)((m - lo * 16.0) * 8.0 + 0.5), k;
            int mj = xmajor ? x : y;
            if (cov_hi > 128) cov_hi = 128;
            if (!ROW_MINE(xmajor ? lo : y, fk, fn) && !ROW_MINE(xmajor ? lo + 1 : y, fk, fn)) continue;
            for (k = 0; k < 2; k++) {
                int cov = k ? cov_hi : 128 - cov_hi, px = xmajor ? mj : lo + k, py = xmajor ? lo + k : mj;
                pix_in q = p;
                if (!cov || !ROW_MINE(py, fk, fn)) continue;
                q.a = cov;
                LINE_PIXEL(e, px, py, &q);
            }
            continue;
        }
        LINE_PIXEL(e, x, y, &p);
    }
}

static void draw_point(draw_env *e, const gs_vertex *v, int fk, int fn)
{
    pix_in p;
    int y = (v->y + 8) >> 4;
    if (!ROW_MINE(y, fk, fn)) return;
    p.r = v->r; p.g = v->g; p.b = v->b; p.a = v->a; p.z = v->z; p.f = v->f;
    p.lod = -1;
    if (e->tme) {
        if (e->fst) tex_coords(e, &p, 0, 0, 1, (float)v->u, (float)v->v);
        else tex_coords(e, &p, v->s, v->t, v->q, 0, 0);
    }
    LINE_PIXEL(e, (v->x + 8) >> 4, y, &p);
}

/* Tell the texture cache which blocks this primitive can write (its bounding box in the frame and z buffers). */
static void prim_ranges(const draw_env *e, const gs_vertex *v, int n, int pend, u32 *rlo, u32 *rhi, int *nr);

static void mark_prim(const draw_env *e, const gs_vertex *v, int n) { prim_ranges(e, v, n, 0, NULL, NULL, NULL); }
static void mark_prim_ranges(const draw_env *e, const gs_vertex *v, int n) { prim_ranges(e, v, n, 1, NULL, NULL, NULL); }

static void pend_add(u32 lo, u32 hi);

static void prim_ranges(const draw_env *e, const gs_vertex *v, int n, int pend, u32 *rlo, u32 *rhi, int *nr)
{
    int i, x0 = 0x7FFFFFFF, y0 = 0x7FFFFFFF, x1 = -0x7FFFFFFF, y1 = -0x7FFFFFFF;
    u32 lo, hi;
    for (i = 0; i < n; i++) {
        int x = v[i].x >> 4, y = v[i].y >> 4;
        if (x < x0) x0 = x;
        if (y < y0) y0 = y;
        if (x + 1 > x1) x1 = x + 1;
        if (y + 1 > y1) y1 = y + 1;
    }
    if (x0 < e->scx0) x0 = e->scx0;
    if (y0 < e->scy0) y0 = e->scy0;
    if (x1 > e->scx1) x1 = e->scx1;
    if (y1 > e->scy1) y1 = e->scy1;
    if (x1 < x0 || y1 < y0) return;
    gs_rect_blocks(e->fpsm, e->fbp, e->fbw, (u32)x0, (u32)y0, (u32)x1, (u32)y1, &lo, &hi);
    gs_mark_written(lo, hi);
    if (pend) pend_add(lo, hi);
    if (nr) { rlo[*nr] = lo; rhi[*nr] = hi; (*nr)++; }
    if (e->zte && !e->zmsk) {
        gs_rect_blocks(e->zpsm, e->zbp, e->fbw, (u32)x0, (u32)y0, (u32)x1, (u32)y1, &lo, &hi);
        gs_mark_written(lo, hi);
        if (pend) pend_add(lo, hi);
        if (nr) { rlo[*nr] = lo; rhi[*nr] = hi; (*nr)++; }
    }
}

/* ---- batching: primitives queue up and are drawn by all threads, each owning interleaved 8-row strips -- */

#define QMAX   2048
#define STRIP  8      /* ROW_MINE assumes 8 */

typedef struct qprim {
    draw_env e;
    gs_vertex v[3];
    int is_tri, is_line, y0, y1;   /* is_line: 1 line, 2 point */
    union { sprite_job s; tri_job t; } j;
} qprim;

static qprim *qbuf[2], *queue;         /* the queue being filled is qbuf[qcur] */
static int qcur, qn;
static u32 pend_lo[32], pend_hi[32];   /* GS blocks the queued primitives write */
static int npend;
/* the batch the workers are drawing while the next one fills (gs_flush_async) */
typedef struct qbatch { qprim *q; int n; } qbatch;
static qbatch inflight;
static u32 ipend_lo[32], ipend_hi[32];
static int inpend;
int gs_in_dma;

static void flush_worker(void *ctx, int k, int n)
{
    const qbatch *b = (const qbatch *)ctx;
    int i;
    for (i = 0; i < b->n; i++) {
        qprim *q = &b->q[i];
        if (q->is_line) {   /* every worker walks the line, drawing only its own strips: order is kept */
            if (q->is_line == 1) draw_line(&q->e, &q->v[0], &q->v[1], k, n);
            else draw_point(&q->e, &q->v[0], k, n);
            continue;
        }
        int c, c0 = q->y0 / STRIP, c1 = (q->y1 + STRIP - 1) / STRIP;
        int first = c0 + ((k - c0 % n) % n + n) % n;
        for (c = first; c < c1; c += n) {
            int ya = c * STRIP, yb = ya + STRIP;
            if (ya < q->y0) ya = q->y0;
            if (yb > q->y1) yb = q->y1;
            if (q->is_tri) tri_rows(&q->j.t, ya, yb); else sprite_rows(&q->j.s, ya, yb);
        }
    }
}

static void wait_inflight(void)
{
    if (!inflight.n) return;
    gs_par_wait();
    inflight.n = 0;
    inpend = 0;
}

/* Everything queued is drawn when this returns. */
void gs_flush(void)
{
    qbatch b;
    gs_gpu_flush();
    wait_inflight();
    if (!qn) { gs_tex_free_retired(); return; }
    b.q = queue; b.n = qn;
    gs_par_all(flush_worker, &b);
    qn = 0;
    npend = 0;
    gs_tex_free_retired();
}

/* Hand the queue to the workers and go on filling the other one (VU1 keeps running while they draw). */
void gs_flush_async(void)
{
    wait_inflight();
    if (!qn) return;
    if (gs_par_threads() < 2) { gs_flush(); return; }
    if (!qbuf[qcur ^ 1]) qbuf[qcur ^ 1] = (qprim *)malloc(sizeof(qprim) * QMAX);
    if (!qbuf[qcur ^ 1]) { gs_flush(); return; }
    inflight.q = queue; inflight.n = qn;
    memcpy(ipend_lo, pend_lo, sizeof pend_lo); memcpy(ipend_hi, pend_hi, sizeof pend_hi); inpend = npend;
    gs_par_all_async(flush_worker, &inflight);
    qcur ^= 1; queue = qbuf[qcur];
    qn = 0;
    npend = 0;
}

static void pend_add(u32 lo, u32 hi)
{
    if (npend == 32) {
        int i;
        for (i = 1; i < 32; i++) { if (pend_lo[i] < pend_lo[0]) pend_lo[0] = pend_lo[i]; if (pend_hi[i] > pend_hi[0]) pend_hi[0] = pend_hi[i]; }
        npend = 1;
    }
    pend_lo[npend] = lo; pend_hi[npend] = hi; npend++;
}

static int pend_overlaps(u32 lo, u32 hi)
{
    int i;
    for (i = 0; i < npend; i++) if (!(hi < pend_lo[i] || lo > pend_hi[i])) return 1;
    for (i = 0; i < inpend; i++) if (!(hi < ipend_lo[i] || lo > ipend_hi[i])) return 1;
    return 0;
}

/* Before GS memory blocks lo..hi are read or written outside the queue (a CLUT load, an image transfer),
 * queued or in-flight primitives that draw into them must land. */
void gs_sync_blocks(u32 lo, u32 hi)
{
    if ((qn || inflight.n) && pend_overlaps(lo, hi)) gs_flush();
}

/* Base-level texels a coordinate range [lo, hi] can reach, widened for bilinear and rounding, when it is a plain
 * window of the texture (inside it, or clamped); otherwise *a, *b are left as they are (the whole texture). */
static void tex_window(float lo, float hi, int size, int mode, int *a, int *b)
{
    int x0 = (int)floorf(lo) - 2, x1 = (int)floorf(hi) + 2;
    if (mode == 1) { if (x0 < 0) x0 = 0; if (x1 > size - 1) x1 = size - 1; }
    else if (x0 < 0 || x1 > size - 1) return;
    if (x0 > x1) return;
    *a = x0; *b = x1;
}

/* Base-level rows and columns a primitive can sample: its vertices' U and V (texture coordinates interpolate
 * between them, perspective included). Repeat beyond the texture, the region modes, mipmaps or odd Q values take
 * the whole texture. */
static void tex_rows(draw_env *e, const gs_vertex *v, int n)
{
    int w = 1 << e->tw, h = 1 << e->th, i;
    float ulo = 1e30f, uhi = -1e30f, vlo = 1e30f, vhi = -1e30f;
    e->ty0 = 0; e->ty1 = 0x7FFFFFFF; e->tx0 = 0; e->tx1 = 0x7FFFFFFF;
    if (e->mxl) return;
    for (i = 0; i < n; i++) {
        float s, t;
        if (e->fst) { s = (float)v[i].u / 16.0f; t = (float)v[i].v / 16.0f; }
        else {
            if (!(v[i].q > 0.0f)) return;
            s = v[i].s / v[i].q * (float)w;
            t = v[i].t / v[i].q * (float)h;
        }
        if (!(s > -1e6f && s < 1e6f && t > -1e6f && t < 1e6f)) return;
        if (s < ulo) ulo = s;
        if (s > uhi) uhi = s;
        if (t < vlo) vlo = t;
        if (t > vhi) vhi = t;
    }
    if (e->wmt <= 1) tex_window(vlo, vhi, h, e->wmt, &e->ty0, &e->ty1);
    if (e->wms <= 1) tex_window(ulo, uhi, w, e->wms, &e->tx0, &e->tx1);
}

/* The threads split a batch by screen rows, so every primitive in it must map rows to memory the same way: one
 * that writes memory the queued ones write, through another frame or z layout (base, width, format; a 320-wide
 * and then a 640-wide view of one buffer in the duel's block wipe), waits for them to land. */
static u64 q_layout;
static void flush_if_layout_changes(const draw_env *e, const gs_vertex *v, int n)
{
    u64 key = (u64)e->fbp | ((u64)e->fbw << 14) | ((u64)e->fpsm << 20) | ((u64)e->zbp << 26) | ((u64)e->zpsm << 40) |
              ((u64)(e->zte && !e->zmsk) << 46);
    if ((qn || inflight.n) && key != q_layout) {
        u32 lo[2], hi[2];
        int nr = 0, i;
        prim_ranges(e, v, n, 0, lo, hi, &nr);
        for (i = 0; i < nr; i++) if (pend_overlaps(lo[i], hi[i])) { gs_flush(); break; }
    }
    q_layout = key;
}

/* Before a primitive samples a texture, earlier queued primitives that draw into its memory must land. */
static void flush_if_texture_pending(const draw_env *e)
{
    int lod;
    if (!qn && !inflight.n) return;
    for (lod = 0; lod <= e->mxl && lod <= 6; lod++) {
        u32 tbp, tbw, lo, hi; int w, h;
        level_params(e, lod, &tbp, &tbw, &w, &h);
        gs_rect_blocks(e->tpsm, tbp, tbw, 0, 0, (u32)w - 1, (u32)h - 1, &lo, &hi);
        if (pend_overlaps(lo, hi)) { gs_flush(); return; }
    }
}

/* ---- the GPU renderer (gpu/): the same primitive, set up as tri_rows / sprite_rows set it up ------------------- */

static void gpu_vtx_of(gpu_vtx *o, const gs_vertex *v)
{
    o->x = v->x; o->y = v->y; o->z = v->z; o->r = v->r; o->g = v->g; o->b = v->b; o->a = v->a; o->f = v->f;
    o->s = v->s; o->t = v->t; o->q = v->q; o->u = v->u; o->v = v->v;
}

/* Returns 1 to queue, -1 when there is nothing to draw, 2 for a point or line (drawn pixel by pixel). */
static int gpu_prim_of(draw_env *e, const gs_vertex *v, u32 type, gpu_prim *p)
{
    int i, j, ry0, ry1;
    memset(p, 0, sizeof *p);
    p->fbp = e->fbp; p->fbw = e->fbw; p->fpsm = e->fpsm; p->fbmsk = e->fbmsk;
    p->zbp = e->zbp; p->zpsm = e->zpsm; p->zmsk = (u32)e->zmsk; p->zmax = e->zmax;
    p->ate = e->ate; p->atst = e->atst; p->aref = e->aref; p->afail = e->afail; p->date = e->date; p->datm = e->datm;
    p->zte = e->zte; p->ztst = e->ztst;
    p->abe = e->abe; p->a = e->a; p->b = e->b; p->c = e->c; p->d = e->d; p->fix = e->fix; p->pabe = e->pabe; p->fba = e->fba;
    p->colclamp = e->colclamp; p->dthe = e->dthe;
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++) {
            u32 k = (u32)(i * 4 + j), nib = (u32)e->dimx[i][j] & 0xF;
            if (k < 8) p->dimx_lo |= nib << (k * 4); else p->dimx_hi |= nib << ((k - 8) * 4);
        }
    p->tme = e->tme; p->fge = e->fge; p->iip = e->iip; p->fst = e->fst; p->tfx = (s32)e->tfx; p->tcc = (s32)e->tcc;
    p->wms = e->wms; p->wmt = e->wmt; p->minu = e->minu; p->maxu = e->maxu; p->minv = e->minv; p->maxv = e->maxv;
    p->mmag = e->mmag;
    p->mxl = e->mxl; p->lcm = e->lcm; p->lodk = e->lodk; p->lodl = e->lodl; p->min_lin = (e->mmin == 1 || e->mmin == 4 || e->mmin == 5);
    { int i_; for (i_ = 0; i_ < 6; i_++) { p->mtbp[i_] = e->mtbp[i_ + 1]; p->mtbw[i_] = e->mtbw[i_ + 1]; } }
    p->tbp = e->tbp; p->tbw = e->tbw; p->tpsm = e->tpsm; p->tw = e->tw; p->th = e->th; p->cpsm = e->cpsm; p->csa = e->csa;
    p->ta0 = e->ta0; p->ta1 = e->ta1; p->aem = e->aem; p->fogr = e->fogr; p->fogg = e->fogg; p->fogb = e->fogb;
    p->scanmsk = (u32)e->scanmsk;
    if (type == 6) {
        sprite_job sj;
        setup_sprite(e, &v[0], &v[1], &sj, &ry0, &ry1);
        p->kind = 1;
        p->x0 = sj.px0; p->x1 = sj.px1; p->y0 = ry0; p->y1 = ry1;
        p->spx0 = sj.px0; p->spx1 = sj.px1; p->sy0 = sj.y0; p->sy1 = sj.y1;
        p->st0 = sj.t0; p->st1 = sj.t1; p->sdu = sj.du; p->stx0 = sj.tx0;
        gpu_vtx_of(&p->v[0], &v[0]); gpu_vtx_of(&p->v[1], &v[1]);
        return 1;
    }
    if (type >= 3 && type <= 5) {
        tri_job tj;
        const gs_vertex *a, *b, *c;
        double za, zb, zc, zstep, inv;
        float fa[10], fb[10], fc[10];
        s64 d0, d1, d2;
        int k;
        if (!setup_tri(e, &v[0], &v[1], &v[2], &tj, &ry0, &ry1)) return -1;   /* no area: nothing drawn */
        a = tj.a; b = tj.b; c = tj.c; inv = tj.inv;
        p->kind = 0;
        p->x0 = tj.minx; p->x1 = tj.maxx; p->y0 = ry0; p->y1 = ry1;
        gpu_vtx_of(&p->v[0], a); gpu_vtx_of(&p->v[1], b); gpu_vtx_of(&p->v[2], c); gpu_vtx_of(&p->vflat, tj.v2);
        /* tri_rows' per-primitive values, the same expressions */
        za = (double)a->z; zb = (double)b->z; zc = (double)c->z;
#define TOPLEFT(p0, p1) (((p1)->y == (p0)->y && (p1)->x > (p0)->x) || ((p1)->y < (p0)->y))
        p->tl0 = TOPLEFT(b, c); p->tl1 = TOPLEFT(c, a); p->tl2 = TOPLEFT(a, b);
#undef TOPLEFT
        d0 = -(s64)(c->y - b->y) * 16; d1 = -(s64)(a->y - c->y) * 16; d2 = -(s64)(b->y - a->y) * 16;
        p->d0 = (s32)d0; p->d1 = (s32)d1; p->d2 = (s32)d2;
#define SETF(arr, vx) (arr[0] = (vx)->r, arr[1] = (vx)->g, arr[2] = (vx)->b, arr[3] = (vx)->a, arr[4] = (vx)->f, \
                       arr[5] = (vx)->s, arr[6] = (vx)->t, arr[7] = (vx)->q, arr[8] = (float)(vx)->u, arr[9] = (float)(vx)->v)
        SETF(fa, a); SETF(fb, b); SETF(fc, c);
#undef SETF
        p->nf = e->tme ? 10 : 5;
        zstep = ((double)d0 * za + (double)d1 * zb + (double)d2 * zc) * inv;
        for (k = 0; k < p->nf; k++) p->fstep[k] = (float)(((double)d0 * fa[k] + (double)d1 * fb[k] + (double)d2 * fc[k]) * inv);
        memcpy(&p->inv_lo, &inv, 8);
        memcpy(&p->zstep_lo, &zstep, 8);
        return 1;
    }
    p->kind = 2;
    return 2;
}

static gpu_prim gpu_pix_tmpl;
static u32 gpu_pix_tlo, gpu_pix_thi, gpu_pix_wlo[2], gpu_pix_whi[2];
static int gpu_pix_nw;

/* one pixel of a point or line, with the values draw_line / draw_point give put_pixel */
static void gpu_pixel(draw_env *e, int x, int y, const pix_in *px)
{
    gpu_prim p = gpu_pix_tmpl;
    if (x < e->scx0 || x > e->scx1 || y < e->scy0 || y > e->scy1) return;   /* put_pixel's scissor */
    p.x0 = x; p.y0 = y; p.x1 = x + 1; p.y1 = y + 1;
    p.v[0].r = px->r; p.v[0].g = px->g; p.v[0].b = px->b; p.v[0].a = px->a; p.v[0].z = px->z; p.v[0].f = px->f;
    p.v[0].s = e->tme ? px->u : 0.0f; p.v[0].t = e->tme ? px->v : 0.0f;
    p.v[0].q = (float)px->lod;   /* the level draw_line/draw_point chose */
    gs_gpu_add_prim(&p, gpu_pix_tlo, gpu_pix_thi, gpu_pix_wlo, gpu_pix_whi, gpu_pix_nw);
}

/* The coordinates wrap() can give along one axis: the texture's size for repeat and clamp, the region for the
 * region modes (whatever the texture's size). */
static void texel_reach(int mode, u32 size, u32 mn, u32 mx, u32 *lo, u32 *hi)
{
    *lo = 0; *hi = size - 1;
    if (mode == 2) { *lo = mn < mx ? mn : mx; *hi = mn < mx ? mx : mn; }
    else if (mode == 3) { *lo = mx; *hi = mn | mx; }
}

/* A region mode that reaches past the texture's size: those texels come straight from GS memory and the palette
 * (texel_slow), which later transfers and CLUT loads change, so a queued primitive would read them too late. */
static int reads_beyond_texture(const draw_env *e)
{
    u32 x0, x1, y0, y1;
    if (e->wms < 2 && e->wmt < 2) return 0;
    texel_reach(e->wms, 1u << e->tw, (u32)e->minu, (u32)e->maxu, &x0, &x1);
    texel_reach(e->wmt, 1u << e->th, (u32)e->minv, (u32)e->maxv, &y0, &y1);
    return x1 >= (1u << e->tw) || y1 >= (1u << e->th);
}

static void gpu_queue_prim(draw_env *e, const gs_vertex *v, u32 type, int nv)
{
    gpu_prim p;
    u32 wlo[2], whi[2], tlo = 1, thi = 0;
    int nw = 0, i;
    int r = gpu_prim_of(e, v, type, &p);
    if (r < 0) return;
    if (e->tme) {
        u32 x0, x1, y0, y1;
        p.clut_slot = gs_gpu_clut_slot();
        /* the texels it can reach: the region modes go beyond the texture's size (wrap()), into GS memory the GPU
         * must see as it is now (an effect in the monster battles clamps V to 896..1023 of a 256-high texture) */
        texel_reach(e->wms, 1u << e->tw, (u32)e->minu, (u32)e->maxu, &x0, &x1);
        texel_reach(e->wmt, 1u << e->th, (u32)e->minv, (u32)e->maxv, &y0, &y1);
        gs_rect_blocks(e->tpsm, e->tbp, e->tbw, x0, y0, x1, y1, &tlo, &thi);
        for (i = 1; i <= e->mxl && i <= 6; i++) {   /* the mip levels it may sample too */
            u32 lw = (1u << e->tw) >> i, lh = (1u << e->th) >> i, llo, lhi;
            gs_rect_blocks(e->tpsm, e->mtbp[i], e->mtbw[i], 0, 0, (lw ? lw : 1) - 1, (lh ? lh : 1) - 1, &llo, &lhi);
            if (llo < tlo) tlo = llo;
            if (lhi > thi) thi = lhi;
        }
    }
    prim_ranges(e, v, nv, 0, wlo, whi, &nw);
    if (r == 2) {   /* the CPU walks the line as draw_line does; the GPU gets its pixels in order */
        draw_env le = *e;
        gpu_pix_tmpl = p; gpu_pix_tlo = tlo; gpu_pix_thi = thi; gpu_pix_nw = nw;
        memcpy(gpu_pix_wlo, wlo, sizeof wlo); memcpy(gpu_pix_whi, whi, sizeof whi);
        unsigned int csr = _mm_getcsr();
        _mm_setcsr(0x1F80u | (csr & 0x3Fu));   /* as the exact renderer walks it (gs_par.c) */
        pix_sink = gpu_pixel;
        if (type == 0) draw_point(&le, &v[0], 0, 0); else draw_line(&le, &v[0], &v[1], 0, 0);
        pix_sink = NULL;
        _mm_setcsr(csr);
        return;
    }
    gs_gpu_add_prim(&p, tlo, thi, wlo, whi, nw);
}

/* DOTR_GS_CENSUS=<fields>: every that many fields, one log line per distinct drawing mode since the last report
 * (primitive kind, PRIM bits, ALPHA, TEST, texture formats and filters, CLAMP, frame/z formats and masks, dither,
 * clamping, FBA, PABE, SCANMSK, scissor, which renderer drew it) with its count. A survey tool, off by default. */
#define CENSUS_N 2048
static struct { char key[400]; unsigned n; } census[CENSUS_N];
static int census_used, census_on = -1;

static void census_add(u32 type, int path)
{
    u64 pr = (gs.prmodecont & 1) ? gs.prim : gs.prmode;
    const gs_ctx *c = &gs.ctx[(pr >> 9) & 1];
    int tme = (int)((pr >> 4) & 1), i, full;
    char k[400];
    full = BITS(c->scissor, 0, 11) == 0 && BITS(c->scissor, 32, 11) == 0 &&
           BITS(c->scissor, 16, 11) >= (BITS(c->frame, 16, 6) * 64 - 1 < 639 ? BITS(c->frame, 16, 6) * 64 - 1 : 639);
    snprintf(k, sizeof k,
             "prim %u iip %u tme %d fge %u abe %u aa1 %u fst %u | alpha %u%u%u%u fix %u | test ate %u atst %u afail %u "
             "date %u/%u zte %u ztst %u | tex psm %02x cpsm %02x csm %u tfx %u tcc %u mxl %u mmag %u mmin %u lcm %u | "
             "clamp %u/%u | frame psm %02x fbmsk %d | zbuf psm %02x zmsk %u | dthe %u colclamp %u fba %u pabe %u "
             "scanmsk %u | scissor %s | %s",
             type, (u32)(pr >> 3) & 1, tme, (u32)(pr >> 5) & 1, (u32)(pr >> 6) & 1, (u32)(pr >> 7) & 1, (u32)(pr >> 8) & 1,
             BITS(c->alpha, 0, 2), BITS(c->alpha, 2, 2), BITS(c->alpha, 4, 2), BITS(c->alpha, 6, 2),
             BITS(c->alpha, 4, 2) == 2 ? BITS(c->alpha, 32, 8) : 0,
             BITS(c->test, 0, 1), BITS(c->test, 1, 3), BITS(c->test, 12, 2), BITS(c->test, 14, 1), BITS(c->test, 15, 1),
             BITS(c->test, 16, 1), BITS(c->test, 17, 2),
             tme ? BITS(c->tex0, 20, 6) : 0, tme ? BITS(c->tex0, 51, 4) : 0, tme ? BITS(c->tex0, 55, 1) : 0,
             tme ? BITS(c->tex0, 35, 2) : 0, tme ? BITS(c->tex0, 34, 1) : 0, tme ? BITS(c->tex1, 2, 3) : 0,
             tme ? BITS(c->tex1, 5, 1) : 0, tme ? BITS(c->tex1, 6, 3) : 0, tme ? BITS(c->tex1, 0, 1) : 0,
             tme ? BITS(c->clamp, 0, 2) : 0, tme ? BITS(c->clamp, 2, 2) : 0,
             BITS(c->frame, 24, 6), (c->frame >> 32) != 0, BITS(c->zbuf, 24, 4) | 0x30, BITS(c->zbuf, 32, 1),
             (u32)(gs.dthe & 1), (u32)(gs.colclamp & 1), (u32)(c->fba & 1), (u32)(gs.pabe & 1), (u32)(gs.scanmsk & 3),
             full ? "full" : "part", path == 2 ? "gpu" : path == 1 ? "exact-queued" : "exact-now");
    for (i = 0; i < census_used; i++) if (!strcmp(census[i].key, k)) { census[i].n++; return; }
    if (census_used < CENSUS_N) { memcpy(census[census_used].key, k, sizeof k); census[census_used++].n = 1; }
}

void gs_census_field(void)
{
    static u32 fields, every;
    int i;
    if (census_on <= 0) return;
    if (!every) every = (u32)census_on;
    if (++fields % every) return;
    for (i = 0; i < census_used; i++) hw_log(2, "census %u: %6u x %s", fields, census[i].n, census[i].key);
    census_used = 0;
}

void gs_draw_prim(const gs_vertex *v, int n)
{
    if (gfx_no_draw) return;
    draw_env e;
    u32 type = (u32)(gs.prim & 7);
    if (census_on < 0) census_on = getenv("DOTR_GS_CENSUS") ? atoi(getenv("DOTR_GS_CENSUS")) : 0;
    if (census_on > 0) census_add(type, gs_gpu_only ? 2 : (type != 7 && gs_par_threads() > 1) ? 1 : 0);
    env_setup(&e);
    if (gs_gpu_on && type != 7) {
        gpu_queue_prim(&e, v, type, type == 0 ? 1 : (type <= 2 || type == 6) ? 2 : 3);
        if (gs_gpu_only) return;
    }
    if (type != 7 && gs_par_threads() > 1 && !(e.tme && reads_beyond_texture(&e))) {
        qprim *q;
        int ok, nv = type == 0 ? 1 : (type <= 2 || type == 6) ? 2 : 3;
        if (!queue) queue = qbuf[qcur] = (qprim *)malloc(sizeof(qprim) * QMAX);
        if (queue) {
            if (e.tme) { flush_if_texture_pending(&e); tex_rows(&e, v, nv); tex_prefetch(&e); }
            flush_if_layout_changes(&e, v, nv);
            mark_prim_ranges(&e, v, nv);
            q = &queue[qn];
            q->e = e;
            q->e.par = 1;
            memcpy(q->v, v, sizeof(gs_vertex) * nv);
            q->is_line = 0;
            if (type <= 2) { q->is_tri = 0; q->is_line = type == 0 ? 2 : 1; q->y0 = 0; q->y1 = 1; ok = 1; }
            else if (type == 6) { q->is_tri = 0; setup_sprite(&q->e, &q->v[0], &q->v[1], &q->j.s, &q->y0, &q->y1); ok = 1; }
            else { q->is_tri = 1; ok = setup_tri(&q->e, &q->v[0], &q->v[1], &q->v[2], &q->j.t, &q->y0, &q->y1); }
            if (ok && q->y1 > q->y0) qn++;
            if (qn == QMAX) gs_flush_async();
            if (!gs_in_dma) gs_flush();
            (void)n;
            return;
        }
    }
    gs_flush();
    mark_prim(&e, v, type == 0 ? 1 : (type <= 2 || type == 6) ? 2 : 3);
    if (e.tme) { if (type >= 3) tex_rows(&e, v, type == 6 ? 2 : 3); tex_prefetch(&e); }
    e.par = (type >= 3);   /* triangles and sprites may run their rows in parallel */
    switch (type) {
    case 0: case 1: case 2: {   /* on this thread: in the drawing threads' SSE mode (gs_par.c) */
        unsigned int csr = _mm_getcsr();
        _mm_setcsr(0x1F80u | (csr & 0x3Fu));
        if (type == 0) draw_point(&e, &v[0], 0, 0); else draw_line(&e, &v[0], &v[1], 0, 0);
        _mm_setcsr(csr);
        break;
    }
    case 3: case 4: case 5: {
        tri_job j; int y0, y1;
        if (setup_tri(&e, &v[0], &v[1], &v[2], &j, &y0, &y1)) gs_par_rows(y0, y1, j.maxx - j.minx, tri_rows, &j);
        break;
    }
    case 6: {
        sprite_job j; int y0, y1;
        setup_sprite(&e, &v[0], &v[1], &j, &y0, &y1);
        gs_par_rows(y0, y1, j.px1 - j.px0, sprite_rows, &j);
        break;
    }
    default: break;
    }
    (void)n;
}
