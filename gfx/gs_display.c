/* gs_display.c - the PCRTC: read the two display circuits out of GS memory and merge them. */
#include "gfx_internal.h"
#include "gs_fast.h"

#define BITS(v, lo, n) ((u32)(((v) >> (lo)) & ((1ull << (n)) - 1)))

typedef struct circ {
    int on;
    u32 fbp, fbw, psm, dbx, dby;
    int dx, dy, magh, magv, dw, dh;
    const gs_fbtab *t;   /* address tables for 16/32-bit buffers, else NULL */
} circ;

static void circ_get(circ *c, int n)
{
    u64 fb = gs.dispfb[n], d = gs.display[n];
    c->on = (int)((gs.pmode >> n) & 1);
    c->fbp = BITS(fb, 0, 9) * 32; c->fbw = BITS(fb, 9, 6); c->psm = BITS(fb, 15, 5);
    c->dbx = BITS(fb, 32, 11); c->dby = BITS(fb, 43, 11);
    c->dx = (int)BITS(d, 0, 12); c->dy = (int)BITS(d, 12, 11);
    c->magh = (int)BITS(d, 23, 4) + 1; c->magv = (int)BITS(d, 27, 2) + 1;
    c->dw = (int)BITS(d, 32, 12) + 1; c->dh = (int)BITS(d, 44, 11) + 1;
    c->t = NULL;
}

/* Frame mode on an interlaced display reads every line; field mode reads half the lines, line-doubled. */
static int field_mode(void) { return (gs.smode2 & 1) && (gs.smode2 & 2); }

int gs_display_size(int *w, int *h)
{
    circ c[2];
    int i, bw = 0, bh = 0;
    circ_get(&c[0], 0); circ_get(&c[1], 1);
    for (i = 0; i < 2; i++) {
        if (!c[i].on) continue;
        if (c[i].dw / c[i].magh > bw) bw = c[i].dw / c[i].magh;
        if (c[i].dh / c[i].magv > bh) bh = c[i].dh / c[i].magv;
    }
    if (!bw || !bh) return 0;
    if (bw > 1024) bw = 1024;
    if (bh > 1024) bh = 1024;
    *w = bw; *h = bh;
    return 1;
}

static u32 to_rgba(u32 psm, u32 v)
{
    switch (psm) {
    case PSMCT16: case PSMCT16S:
        return ((v & 0x1F) << 3) | (((v >> 5) & 0x1F) << 11) | (((v >> 10) & 0x1F) << 19) | ((v & 0x8000) ? 0x80000000u : 0);
    case PSMCT24:
        return (v & 0xFFFFFF) | 0x80000000u;
    default:
        return v;
    }
}

/* Buffer column (or row) a circuit shows at output column x (row y), in video clock units relative to the
 * reference circuit; -1 when it shows nothing there. */
static int circ_col(const circ *c, const circ *ref, int x)
{
    int vx = x * ref->magh + ref->dx - c->dx, cx;
    if (vx < 0) return -1;
    cx = vx / c->magh;
    if (cx >= c->dw / c->magh) return -1;
    return (int)((c->dbx + cx) & 2047);
}

static int circ_row(const circ *c, const circ *ref, int y, int fieldm)
{
    int vy = y * ref->magv + ref->dy - c->dy, cy;
    if (vy < 0) return -1;
    cy = vy / c->magv;
    if (cy >= c->dh / c->magv) return -1;
    if (fieldm) cy >>= 1;
    return (int)((c->dby + cy) & 2047);
}

static u32 circ_fetch(const circ *c, int cx, int cy)
{
    if (c->t) {
        u32 a = (c->t->yo[cy] + c->t->xo[cx]) & c->t->mask;
        return to_rgba(c->psm, c->t->mask == GS_MEM_WORDS - 1 ? gs_vram[a] : ((const u16 *)gs_vram)[a]);
    }
    return to_rgba(c->psm, gs_read_pixel(c->psm, c->fbp, c->fbw, (u32)cx, (u32)cy));
}

void gs_display_read(u32 *rgba, int w, int h)
{
    static int col[2][1024];   /* w <= 1024 (gs_display_size) */
    circ c[2];
    const circ *ref;
    int x, y, fieldm = field_mode();
    int mmod = (int)((gs.pmode >> 5) & 1), slbg = (int)((gs.pmode >> 7) & 1), alp = (int)BITS(gs.pmode, 8, 8);
    u32 bg = (u32)(gs.bgcolor & 0xFFFFFF);
    circ_get(&c[0], 0); circ_get(&c[1], 1);
    if (c[0].on) c[0].t = gs_fbtab_get(c[0].fbp, c[0].fbw, c[0].psm);
    if (c[1].on) c[1].t = gs_fbtab_get(c[1].fbp, c[1].fbw, c[1].psm);
    ref = c[0].on ? &c[0] : &c[1];
    if (c[0].on && c[1].on) {
        /* the reference is the circuit that starts first on screen */
        if (c[1].dx < c[0].dx || c[1].dy < c[0].dy) ref = &c[1];
    }
    for (x = 0; x < w; x++) {   /* the column each circuit shows at each output column, worked out once */
        col[0][x] = c[0].on ? circ_col(&c[0], ref, x) : -1;
        col[1][x] = c[1].on ? circ_col(&c[1], ref, x) : -1;
    }
    for (y = 0; y < h; y++) {
        int r0 = c[0].on ? circ_row(&c[0], ref, y, fieldm) : -1, r1 = c[1].on ? circ_row(&c[1], ref, y, fieldm) : -1;
        for (x = 0; x < w; x++) {
            u32 p1 = 0, p2 = bg, out;
            int h1 = r0 >= 0 && col[0][x] >= 0;
            if (h1) p1 = circ_fetch(&c[0], col[0][x], r0);
            if (!slbg && r1 >= 0 && col[1][x] >= 0) p2 = circ_fetch(&c[1], col[1][x], r1);
            if (h1) {
                int a = mmod ? alp : (int)((p1 >> 24) * 2 > 255 ? 255 : (p1 >> 24) * 2);
                int k;
                out = 0;
                if (a == 255) out = p1 & 0xFFFFFF;
                else for (k = 0; k < 24; k += 8) {
                    u32 v = ((p1 >> k) & 0xFF) * (u32)a + ((p2 >> k) & 0xFF) * (u32)(255 - a);
                    out |= ((v + 1 + (v >> 8)) >> 8) << k;   /* v / 255 for every v up to 255 * 255 */
                }
            } else {
                out = p2 & 0xFFFFFF;
            }
            rgba[y * w + x] = out | 0xFF000000u;
        }
    }
}
