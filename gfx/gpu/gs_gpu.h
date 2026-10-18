/* gs_gpu.h - the GS on the GPU (Direct3D 11): GS local memory lives in a GPU buffer and the GPU does the work
 * the exact CPU renderer does, to the same results. gs_gpu_on is set only when the device has what it needs
 * (rasterizer ordered views); otherwise the exact renderer is used. */
#ifndef DOTR_GS_GPU_H
#define DOTR_GS_GPU_H

#include "../gfx_internal.h"

extern int gs_gpu_on;
extern double gs_gpu_wait_ms;   /* ms the game thread waited for GPU read-backs (timing) */
extern int gs_gpu_only;   /* the GPU draws and the CPU does not (renderer=gpu); else the GPU only mirrors (checks) */

int  gs_gpu_init(void);           /* 1 when the GPU renderer can run on this PC (logs why not) */
void gs_gpu_shutdown(void);
void gs_gpu_load_mem(void);       /* all of GS memory from gs_vram (start, capture replay) */
void gs_gpu_read_mem(u32 *dst);   /* all of GS memory back to the CPU (checks, captures) */
int  gs_gpu_read_mem_timeout(u32 *dst, unsigned ms);   /* the same, giving up after ms (crash reports): 1 = read */
void gs_gpu_sync_cpu(u32 lo, u32 hi);   /* renderer=gpu: blocks lo..hi of gs_vram brought up to the GPU's */
/* a CLUT load (gs.c clut_load) on the GPU, from its GS memory: the next primitives use the new version */
void gs_gpu_clut_load(u32 cbp, u32 cpsm, u32 csm, u32 csa, u32 n, u32 cbw, u32 cou, u32 cov, u32 lo, u32 hi);

/* a host->local transfer: count pixel values (one u32 each, already unpacked) at the transfer's linear pixel
 * index first, in the destination rectangle BITBLTBUF/TRXPOS/TRXREG describe */
void gs_gpu_upload(u32 dbp, u32 dbw, u32 dpsm, u32 dsax, u32 dsay, u32 rrw, u32 first, const u32 *vals, u32 count);

/* a local->local transfer with the BITBLTBUF/TRXPOS/TRXREG values (as trx_local_to_local) */
void gs_gpu_copy(u32 sbp, u32 sbw, u32 spsm, u32 ssax, u32 ssay, u32 dbp, u32 dbw, u32 dpsm, u32 dsax, u32 dsay,
                 u32 dir, u32 rrw, u32 rrh);

/* One primitive for the GPU: the drawing state and setup the exact renderer uses for it, laid out word by word
 * as the shader's struct (gpu/draw.hlsl). Triangles carry tri_rows' per-primitive values, sprites sprite_rows'. */
typedef struct gpu_vtx { s32 x, y; u32 z; s32 r, g, b, a, f; float s, t, q; u32 u, v; } gpu_vtx;   /* 13 words */
typedef struct gpu_prim {
    s32 kind, x0, y0, x1, y1;                  /* 0 triangle, 1 sprite, 2 one pixel; pixels [x0,x1) x [y0,y1) */
    u32 fbp, fbw, fpsm, fbmsk, zbp, zpsm, zmsk, zmax;
    s32 ate, atst, aref, afail, date, datm, zte, ztst;
    s32 abe, a, b, c, d, fix, pabe, fba, colclamp, dthe;
    u32 dimx_lo, dimx_hi;                      /* the 4x4 dither matrix, 4 signed bits each, row-major */
    s32 tme, fge, iip, fst, tfx, tcc, wms, wmt, minu, maxu, minv, maxv, mmag;
    u32 tbp, tbw, tpsm, tw, th, cpsm, csa, ta0, ta1, aem, fogr, fogg, fogb, clut_slot, scanmsk, pad;
    gpu_vtx v[3];                              /* triangle: a, b, c in tri_rows' order; sprite: v0, v1 */
    gpu_vtx vflat;                             /* the vertex flat shading takes its colour from */
    u32 inv_lo, inv_hi, zstep_lo, zstep_hi;    /* doubles: 1/area, z step per pixel */
    float fstep[10];
    s32 tl0, tl1, tl2, d0, d1, d2, nf;         /* top-left flags, edge steps per pixel, attributes stepped */
    s32 spx0, spx1, sy0, sy1;                  /* sprite: first/end pixel column, rows in 1/16 units */
    float st0, st1, sdu, stx0;
    s32 mxl, lcm, lodk, lodl, min_lin;         /* mip levels: count, LOD rule (TEX1), minification filter linear */
    u32 mtbp[6], mtbw[6];                      /* levels 1-6: base and width (MIPTBP1/2) */
} gpu_prim;

void gs_gpu_add_prim(const gpu_prim *p, u32 tex_lo, u32 tex_hi, const u32 *wlo, const u32 *whi, int nw);
u32  gs_gpu_clut_slot(void);   /* gs.clut on the GPU (a new slot when it changed) */
void gs_gpu_flush(void);       /* draw what is queued */
void gs_gpu_stats_field(void);   /* once per field: DOTR_GPU_STATS=1 logs the GPU path's costs every 1000 */
void gs_gpu_finish(void);      /* ... and wait until the GPU has done everything (timing) */

/* the TV picture (as gs_display_read) into rgba, w x h */
void gs_gpu_readout(u32 *rgba, int w, int h);

/* The window draws with the same device: the TV picture read out into a texture (w x h at its top left) that the
 * window copies into its back buffer. */
void *gs_gpu_d3d_device(void);          /* ID3D11Device *, NULL when the GPU renderer is off */
void *gs_gpu_picture(int w, int h);     /* ID3D11Texture2D * (R8G8B8A8 typeless), read out now */
int  gs_gpu_picture_read(u32 *rgba, int w, int h);   /* that texture back to the CPU (checks) */

#endif
