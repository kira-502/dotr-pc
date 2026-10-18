/* gs_fast.h - texture cache and address tables (gs_fast.c). */
#ifndef DOTR_GS_FAST_H
#define DOTR_GS_FAST_H

#include "gfx_internal.h"

/* Decoded texture (RGBA8888 with CLUT/TEXA applied), w*h texels; NULL only when out of memory. Rows y0..y1 of
 * columns x0..x1 are guaranteed decoded (other texels may not be yet): pass 0, h - 1, 0, w - 1 for all of it. */
const u32 *gs_texture(u32 tbp, u32 tbw, u32 psm, u32 w, u32 h, u32 cpsm, u32 csa, u32 texa, u32 y0, u32 y1,
                      u32 x0, u32 x1);

/* Tell the cache which GS memory blocks changed; gs_invalidate_all after a bulk change (reset, capture). */
void gs_mark_written(u32 blk_lo, u32 blk_hi);
void gs_invalidate_all(void);
/* Free replaced texture buffers; only when nothing is queued or being drawn (gs_flush calls it). */
void gs_tex_free_retired(void);
void gs_rect_blocks(u32 psm, u32 bp, u32 bw, u32 x0, u32 y0, u32 x1, u32 y1, u32 *lo, u32 *hi);
extern u32 gs_clut_gen;
extern u32 gs_write_gen;   /* bumped on every change of GS memory the cache is told about */

typedef struct gs_fbtab {
    int valid;
    u32 bp, bw, psm, mask;
    u32 xo[2048], yo[2048];
} gs_fbtab;

/* Address tables for a 32/16-bit buffer (word index for 32-bit, halfword index for 16-bit); NULL for other
 * formats. address = (Y[y] + X[x]) & mask. */
const gs_fbtab *gs_fbtab_get(u32 bp, u32 bw, u32 psm);

#endif
