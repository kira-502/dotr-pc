/* gs_fast.c - speed-ups for the software GS that keep its results exact:
 *   - decoded-texture cache (RGBA8888, CLUT and TEXA applied), invalidated when GS memory under it changes;
 *   - separable address tables for 32/16-bit frame and z buffers (the GS layouts are bit interleavings, so
 *     address(x, y) = yo[y] + xo[x] modulo the memory size). */
#include <stdlib.h>
#include <string.h>
#include "gfx_internal.h"
#include "gs_fast.h"
#include "gs_par.h"

/* ---- written-memory tracking ----------------------------------------------------------------------- */

#define TC_ENTRIES 48

typedef struct tc_entry {
    int used;
    u32 tbp, tbw, psm, w, h, cpsm, csa, clut_gen, texa;   /* clut_gen: palette content hash */
    u32 blk_lo, blk_hi;      /* conservative block range the texture occupies */
    u32 lru;
    u32 *data;
    u32 band_h;              /* rows per decoded band (the format's block height) */
    u32 band_ok[256];        /* per band, the column chunks decoded so far: a primitive decodes only what it can sample */
    u32 cw;                  /* columns per chunk: a multiple of the block width, at most 32 chunks */
} tc_entry;

static tc_entry tc[TC_ENTRIES];
#define GRAVE_MAX 64
static void *grave[GRAVE_MAX];  /* replaced texture buffers and address tables queued primitives may still use */
static int ngrave;

void gs_tex_free_retired(void)
{
    while (ngrave) free(grave[--ngrave]);
}
static u32 tc_lru;
static u32 tc_lo = 0xFFFFFFFFu, tc_hi;  /* union of the entries' ranges, for a quick reject */
u32 gs_clut_gen = 1;
u32 gs_write_gen;
u32 gs_tex_decodes, gs_tex_invalidations;   /* counters for gfxbench */

static void tc_bounds(void)
{
    int i;
    tc_lo = 0xFFFFFFFFu; tc_hi = 0;
    for (i = 0; i < TC_ENTRIES; i++)
        if (tc[i].used) {
            if (tc[i].blk_lo < tc_lo) tc_lo = tc[i].blk_lo;
            if (tc[i].blk_hi > tc_hi) tc_hi = tc[i].blk_hi;
        }
}

void gs_mark_written(u32 blk_lo, u32 blk_hi)
{
    int i, any = 0;
    gs_write_gen++;
    if (blk_hi < tc_lo || blk_lo > tc_hi) return;
    for (i = 0; i < TC_ENTRIES; i++)
        if (tc[i].used && !(blk_hi < tc[i].blk_lo || blk_lo > tc[i].blk_hi)) { tc[i].used = 0; any = 1; gs_tex_invalidations++; }
    if (any) tc_bounds();
}

void gs_invalidate_all(void)
{
    int i;
    gs_write_gen++;
    for (i = 0; i < TC_ENTRIES; i++) tc[i].used = 0;
    tc_bounds();
    gs_clut_gen++;
}

/* Page geometry of a format: width/height in pixels, and how many 64-pixel units one page row spans. */
static void page_dims(u32 psm, u32 *pw, u32 *ph, u32 *bwdiv)
{
    switch (gs_psm_bpp(psm)) {
    case 32: *pw = 64; *ph = 32; *bwdiv = 1; break;
    case 16: *pw = 64; *ph = 64; *bwdiv = 1; break;
    case 8:  *pw = 128; *ph = 64; *bwdiv = 2; break;
    default: *pw = 128; *ph = 128; *bwdiv = 2; break;
    }
}

/* Conservative block range of a rectangle of a buffer (whole pages; wraps are treated as "everything"). */
void gs_rect_blocks(u32 psm, u32 bp, u32 bw, u32 x0, u32 y0, u32 x1, u32 y1, u32 *lo, u32 *hi)
{
    u32 pw, ph, d, ppr, first, last;
    page_dims(psm, &pw, &ph, &d);
    ppr = bw / d ? bw / d : 1;
    first = bp + ((y0 / ph) * ppr + x0 / pw) * 32;
    last = bp + ((y1 / ph) * ppr + x1 / pw) * 32 + 31;
    if (last >= 16384) { *lo = 0; *hi = 16383; return; }
    *lo = first; *hi = last;
}

/* ---- texture cache --------------------------------------------------------------------------------- */

static u32 expand16(u32 c, u32 ta0, u32 ta1, u32 aem)
{
    u32 r = (c & 0x1F) << 3, g = ((c >> 5) & 0x1F) << 3, b = ((c >> 10) & 0x1F) << 3, a;
    if (c & 0x8000) a = ta1;
    else a = (aem && !(c & 0x7FFF)) ? 0 : ta0;
    return r | (g << 8) | (b << 16) | (a << 24);
}

/* Storage unit address of a pixel in each format: words (32-bit), halfwords (16-bit), bytes (8-bit), nibbles (4-bit). */
static u32 unit_addr(u32 psm, u32 bp, u32 bw, u32 x, u32 y)
{
    switch (psm) {
    case PSMZ32: case PSMZ24: return gs_addr32z(bp, bw, x, y);
    case PSMCT16:  return gs_addr16(bp, bw, x, y);
    case PSMCT16S: return gs_addr16s(bp, bw, x, y);
    case PSMZ16:   return gs_addr16z(bp, bw, x, y);
    case PSMZ16S:  return gs_addr16sz(bp, bw, x, y);
    case PSMT8:    return gs_addr8(bp, bw, x, y);
    case PSMT4:    return gs_addr4(bp, bw, x, y);
    default:       return gs_addr32(bp, bw, x, y);   /* CT32, CT24, T8H, T4HL, T4HH */
    }
}

/* Pixel layout inside one block, the same for every block of a format: unit offset of (x, y) from the block's
 * first unit. 32-bit blocks are 8x8, 16-bit 16x8, 8-bit 16x16, 4-bit 32x16. */
typedef struct { u32 psm, bw, bh; u32 off[32 * 16]; } block_layout;

static const block_layout *layout_of(u32 psm)
{
    static block_layout cache[16];
    static int n;
    block_layout *l;
    u32 x, y, a0;
    int i;
    for (i = 0; i < n; i++) if (cache[i].psm == psm) return &cache[i];
    l = &cache[n < 16 ? n++ : 15];
    l->psm = psm;
    switch (gs_psm_bpp(psm)) {
    case 16: l->bw = 16; l->bh = 8; break;
    case 8:  l->bw = psm == PSMT8 ? 16 : 8; l->bh = psm == PSMT8 ? 16 : 8; break;
    case 4:  l->bw = psm == PSMT4 ? 32 : 8; l->bh = psm == PSMT4 ? 16 : 8; break;
    default: l->bw = 8; l->bh = 8; break;
    }
    a0 = unit_addr(psm, 0, 1, 0, 0);
    for (y = 0; y < l->bh; y++)
        for (x = 0; x < l->bw; x++) l->off[y * l->bw + x] = unit_addr(psm, 0, 1, x, y) - a0;
    return l;
}

/* raw value of the unit at address a (the read side of gs_read_pixel, without the address math) */
static __forceinline u32 unit_read(u32 psm, u32 a)
{
    switch (psm) {
    case PSMCT24: case PSMZ24: return gs_vram[a] & 0xFFFFFFu;
    case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S: return ((const u16 *)gs_vram)[a];
    case PSMT8: return ((const u8 *)gs_vram)[a];
    case PSMT4: return (((const u8 *)gs_vram)[a >> 1] >> ((a & 1) * 4)) & 0xF;
    case PSMT8H: return gs_vram[a] >> 24;
    case PSMT4HL: return (gs_vram[a] >> 24) & 0xF;
    case PSMT4HH: return gs_vram[a] >> 28;
    default: return gs_vram[a];
    }
}

typedef struct dec_job {
    const tc_entry *t;
    const block_layout *l;
    u32 ta0, ta1, aem;
    u32 pal[256];
    u32 band[256];   /* first row of each band to decode */
    u32 chunks[256]; /* and its column chunks */
} dec_job;

/* Bands i0..i1-1 of the job: block by block, one exact address per block, the fixed in-block layout for its
 * pixels. Reads only GS memory and the job, so its bands can run on several threads. */
static void decode_bands(void *ctx, int i0, int i1)
{
    const dec_job *j = (const dec_job *)ctx;
    const tc_entry *t = j->t;
    const block_layout *l = j->l;
    const u32 *pal = j->pal;
    u32 bx, ta0 = j->ta0, ta1 = j->ta1, aem = j->aem;
    int i;
    for (i = i0; i < i1; i++) {
        u32 by = j->band[i], c;
        for (bx = 0; bx < t->w; bx += l->bw) {
            if (bx % t->cw == 0) {   /* skip chunks not asked for */
                c = bx / t->cw;
                if (!((j->chunks[i] >> c) & 1)) { bx += t->cw - l->bw; continue; }
            }
            u32 base = unit_addr(t->psm, t->tbp, t->tbw, bx, by), xx, yy;
            u32 xn = t->w - bx < l->bw ? t->w - bx : l->bw, yn = t->h - by < l->bh ? t->h - by : l->bh;
            for (yy = 0; yy < yn; yy++) {
                const u32 *off = l->off + yy * l->bw;
                u32 *out = t->data + (by + yy) * t->w + bx;
                switch (t->psm) {
                case PSMCT24: case PSMZ24:
                    for (xx = 0; xx < xn; xx++) {
                        u32 v = unit_read(PSMCT24, base + off[xx]);
                        out[xx] = v | (((aem && !v) ? 0 : ta0) << 24);
                    }
                    break;
                case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S:
                    for (xx = 0; xx < xn; xx++) out[xx] = expand16(unit_read(PSMCT16, base + off[xx]), ta0, ta1, aem);
                    break;
                case PSMT8:
                    for (xx = 0; xx < xn; xx++) out[xx] = pal[unit_read(PSMT8, base + off[xx])];
                    break;
                case PSMT4:
                    for (xx = 0; xx < xn; xx++) out[xx] = pal[unit_read(PSMT4, base + off[xx])];
                    break;
                case PSMT8H: case PSMT4HL: case PSMT4HH:
                    for (xx = 0; xx < xn; xx++) out[xx] = pal[unit_read(t->psm, base + off[xx]) & 255];
                    break;
                default:
                    for (xx = 0; xx < xn; xx++) out[xx] = unit_read(PSMCT32, base + off[xx]);
                    break;
                }
            }
        }
    }
}

/* Decode what is not decoded yet of rows y0..y1, columns x0..x1. Many bands at once (the frame buffer read back
 * as a texture) are split over the drawing threads when they are idle. */
static void decode(tc_entry *t, u32 y0, u32 y1, u32 x0, u32 x1)
{
    static dec_job j;   /* the game thread's only */
    u32 by, c0 = x0 / t->cw, c1 = x1 / t->cw;
    u32 need = (c1 >= 31 ? 0xFFFFFFFFu : (2u << c1) - 1) & ~((1u << c0) - 1);
    int n = 0;
    const block_layout *l = layout_of(t->psm);
    int is_pal = (t->psm == PSMT8 || t->psm == PSMT8H || t->psm == PSMT4 || t->psm == PSMT4HL || t->psm == PSMT4HH);
    j.t = t; j.l = l;
    j.ta0 = t->texa & 0xFF; j.ta1 = (t->texa >> 8) & 0xFF; j.aem = t->texa >> 16;
    for (by = y0 / l->bh * l->bh; by <= y1 && by < t->h; by += l->bh) {   /* bands already decoded are skipped */
        u32 todo = need & ~t->band_ok[by / l->bh];
        if (!todo) continue;
        t->band_ok[by / l->bh] |= todo;
        gs_tex_decodes++;
        j.band[n] = by; j.chunks[n] = todo; n++;
    }
    if (!n) return;   /* the usual case: everything it samples is decoded already */
    if (is_pal) {
        u32 np = (t->psm == PSMT8 || t->psm == PSMT8H) ? 256 : 16, i;
        for (i = 0; i < np; i++) {
            if (t->cpsm == PSMCT16 || t->cpsm == PSMCT16S) {
                u32 k = np == 16 ? (t->csa * 16 + i) & 511 : i & 255;
                j.pal[i] = expand16(gs.clut[k], j.ta0, j.ta1, j.aem);
            } else {
                u32 k = np == 16 ? ((t->csa & 15) * 16 + i) & 255 : i & 255;
                j.pal[i] = gs.clut[k] | ((u32)gs.clut[k + 256] << 16);
            }
        }
    }
    if (gs_par_busy()) decode_bands(&j, 0, n);   /* the workers are drawing a batch: no waiting for them */
    else gs_par_rows(0, n, (int)(t->w * l->bh), decode_bands, &j);
}

/* The palette a texture uses, as a content hash: a game that reloads the same CLUT before every draw keeps its
 * decoded textures. Memoized on the CLUT generation. */
static u32 clut_hash(u32 psm, u32 cpsm, u32 csa)
{
    static u32 m_gen, m_psm, m_cpsm, m_csa, m_hash;
    u32 n = (psm == PSMT8 || psm == PSMT8H) ? 256 : 16, i, h = 2166136261u;
    if (m_gen == gs_clut_gen && m_psm == n && m_cpsm == cpsm && m_csa == csa) return m_hash;
    for (i = 0; i < n; i++) {
        u32 v;
        if (cpsm == PSMCT16 || cpsm == PSMCT16S) v = gs.clut[(n == 16 ? csa * 16 + i : i) & 511];
        else { u32 k = (n == 16 ? (csa & 15) * 16 + i : i) & 255; v = gs.clut[k] | ((u32)gs.clut[k + 256] << 16); }
        h = (h ^ v) * 16777619u;
    }
    m_gen = gs_clut_gen; m_psm = n; m_cpsm = cpsm; m_csa = csa; m_hash = h | 1;
    return m_hash;
}

const u32 *gs_texture(u32 tbp, u32 tbw, u32 psm, u32 w, u32 h, u32 cpsm, u32 csa, u32 texa, u32 y0, u32 y1,
                      u32 x0, u32 x1)
{
    int i, free_i = -1, old_i = 0;
    int is_pal = (psm == PSMT8 || psm == PSMT8H || psm == PSMT4 || psm == PSMT4HL || psm == PSMT4HH);
    int uses_texa = (psm == PSMCT24 || psm == PSMZ24 || psm == PSMCT16 || psm == PSMCT16S || psm == PSMZ16 ||
                     psm == PSMZ16S || (is_pal && (cpsm == PSMCT16 || cpsm == PSMCT16S)));
    u32 cg;
    tc_entry *t;
    if (!uses_texa) texa = 0;
    if (!is_pal) { cpsm = 0; csa = 0; }
    cg = is_pal ? clut_hash(psm, cpsm, csa) : 0;
    if (y1 >= h) y1 = h - 1;
    if (y0 > y1) y0 = 0;
    if (x1 >= w) x1 = w - 1;
    if (x0 > x1) x0 = 0;
    for (i = 0; i < TC_ENTRIES; i++) {
        t = &tc[i];
        if (!t->used) { if (free_i < 0) free_i = i; continue; }
        if (t->tbp == tbp && t->tbw == tbw && t->psm == psm && t->w == w && t->h == h && t->cpsm == cpsm &&
            t->csa == csa && t->clut_gen == cg && t->texa == texa) {
            t->lru = ++tc_lru;
            decode(t, y0, y1, x0, x1);
            return t->data;
        }
        if (tc[i].lru < tc[old_i].lru) old_i = i;
    }
    t = &tc[free_i >= 0 ? free_i : old_i];
    if (t->data) {   /* queued or in-flight primitives may still read it: freed once they have all landed */
        if (ngrave == GRAVE_MAX) gs_flush();
        grave[ngrave++] = t->data;
        t->data = NULL;
    }
    t->data = (u32 *)malloc((size_t)w * h * 4);
    if (!t->data) { t->used = 0; return NULL; }
    t->tbp = tbp; t->tbw = tbw; t->psm = psm; t->w = w; t->h = h; t->cpsm = cpsm; t->csa = csa;
    t->clut_gen = cg; t->texa = texa;
    gs_rect_blocks(psm, tbp, tbw, 0, 0, w - 1, h - 1, &t->blk_lo, &t->blk_hi);
    t->band_h = layout_of(psm)->bh;
    for (t->cw = layout_of(psm)->bw; w / t->cw > 32; t->cw *= 2) ;
    memset(t->band_ok, 0, sizeof t->band_ok);
    decode(t, y0, y1, x0, x1);
    t->used = 1;
    t->lru = ++tc_lru;
    tc_bounds();
    return t->data;
}

/* ---- separable address tables ---------------------------------------------------------------------- */

#define FT_SLOTS 32
static gs_fbtab *ft[FT_SLOTS];      /* tables never change once built: queued primitives may hold them */
static u32 ft_lru[FT_SLOTS], ft_clock;
static int ft_last;

static u32 addr_of(u32 psm, u32 bp, u32 bw, u32 x, u32 y)
{
    switch (psm) {
    case PSMCT32: case PSMCT24: return gs_addr32(bp, bw, x, y);
    case PSMZ32: case PSMZ24:   return gs_addr32z(bp, bw, x, y);
    case PSMCT16:  return gs_addr16(bp, bw, x, y);
    case PSMCT16S: return gs_addr16s(bp, bw, x, y);
    case PSMZ16:   return gs_addr16z(bp, bw, x, y);
    default:       return gs_addr16sz(bp, bw, x, y);
    }
}

/* address(x, y) = yo[y] + xo[x] (mod memory size): X[x] = a(0, x, 0) - a(0, 0, 0), Y[y] = a(bp, 0, y). Valid
 * because every 32/16-bit layout (z ones included: their XOR 24 flips one block bit per axis) interleaves x and
 * y bits; gfx_tests checks it against the exact functions. */
static void build(gs_fbtab *t)
{
    u32 i, a00 = addr_of(t->psm, 0, t->bw, 0, 0);
    for (i = 0; i < 2048; i++) {
        t->xo[i] = addr_of(t->psm, 0, t->bw, i, 0) - a00;
        t->yo[i] = addr_of(t->psm, t->bp, t->bw, 0, i);
    }
    t->mask = gs_psm_bpp(t->psm) == 16 ? (GS_MEM_WORDS * 2 - 1) : (GS_MEM_WORDS - 1);
}

const gs_fbtab *gs_fbtab_get(u32 bp, u32 bw, u32 psm)
{
    int i;
    gs_fbtab *t;
    if (gs_psm_bpp(psm) < 16 || psm == PSMT8H || psm == PSMT4HL || psm == PSMT4HH)
        return NULL;
    t = ft[ft_last];
    if (t && t->bp == bp && t->bw == bw && t->psm == psm) return t;
    {
        int lru = 0;
        for (i = 0; i < FT_SLOTS; i++) {
            t = ft[i];
            if (t && t->bp == bp && t->bw == bw && t->psm == psm) { ft_lru[i] = ++ft_clock; ft_last = i; return t; }
            if (!t) { lru = i; break; }
            if (ft_lru[i] < ft_lru[lru]) lru = i;
        }
        i = lru;
    }
    if (ft[i]) {   /* retired, freed once nothing queued can use it */
        if (ngrave == GRAVE_MAX) gs_flush();
        grave[ngrave++] = ft[i];
        ft[i] = NULL;
    }
    t = (gs_fbtab *)malloc(sizeof *t);
    if (!t) return NULL;   /* the exact address path is used instead */
    t->bp = bp; t->bw = bw; t->psm = psm;
    build(t);
    t->valid = 1;
    ft[i] = t;
    ft_lru[i] = ++ft_clock;
    ft_last = i;
    return t;
}
