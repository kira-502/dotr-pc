/*
 * gfx_internal.h - shared state of the DotR PC graphics core (lane G).
 *
 * Pipeline (all synchronous, run inside gfx_dma_kick):
 *   DMA (dma.c) -> VIF1 (vif1.c) -> VU1 (vu1.c) -> XGKICK -> GIF PATH1
 *                              \-> DIRECT/DIRECTHL ------> GIF PATH2
 *   DMA GIF channel ----------------------------------------> GIF PATH3
 *   GIF (gif.c) -> GS registers (gs.c) -> rasterizer (gs_draw.c) -> GS memory (gs_mem.c)
 *
 * Written from the public GS/VU/VIF user's-manual behaviour. No emulator code is copied.
 */
#ifndef DOTR_GFX_INTERNAL_H
#define DOTR_GFX_INTERNAL_H

#include <stdint.h>
#include <string.h>
#include "hw.h"

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
typedef int64_t  s64;

typedef struct { u32 w[4]; } qword;

/* ---- GS local memory (gs_mem.c) -------------------------------------------------------------------- */

#define GS_MEM_BYTES  (4u * 1024u * 1024u)
#define GS_MEM_WORDS  (GS_MEM_BYTES / 4u)

enum {
    PSMCT32 = 0x00, PSMCT24 = 0x01, PSMCT16 = 0x02, PSMCT16S = 0x0A,
    PSMT8 = 0x13, PSMT4 = 0x14, PSMT8H = 0x1B, PSMT4HL = 0x24, PSMT4HH = 0x2C,
    PSMZ32 = 0x30, PSMZ24 = 0x31, PSMZ16 = 0x32, PSMZ16S = 0x3A
};

extern u32 *gs_vram;   /* GS_MEM_WORDS words */

void gs_mem_init(void);
/* Address of pixel (x,y) of a buffer at block base bp (256-byte units), width bw (64-pixel units).
 * 32/24-bit formats: word index.  16-bit: halfword index.  8-bit: byte index.  4-bit: nibble index. */
u32  gs_addr32(u32 bp, u32 bw, u32 x, u32 y);
u32  gs_addr32z(u32 bp, u32 bw, u32 x, u32 y);
u32  gs_addr16(u32 bp, u32 bw, u32 x, u32 y);
u32  gs_addr16s(u32 bp, u32 bw, u32 x, u32 y);
u32  gs_addr16z(u32 bp, u32 bw, u32 x, u32 y);
u32  gs_addr16sz(u32 bp, u32 bw, u32 x, u32 y);
u32  gs_addr8(u32 bp, u32 bw, u32 x, u32 y);
u32  gs_addr4(u32 bp, u32 bw, u32 x, u32 y);

/* Read/write one pixel in any PSM (raw bits, no colour expansion). */
u32  gs_read_pixel(u32 psm, u32 bp, u32 bw, u32 x, u32 y);
void gs_write_pixel(u32 psm, u32 bp, u32 bw, u32 x, u32 y, u32 v);
int  gs_store_pixel(u32 psm, u32 bp, u32 bw, u32 x, u32 y, u32 v);   /* no cache mark; 1 = memory changed */
int  gs_psm_bpp(u32 psm);

/* ---- GS registers and drawing (gs.c, gs_draw.c) ---------------------------------------------------- */

typedef struct gs_vertex {
    s32 x, y;        /* 12.4 fixed, after XYOFFSET (window coordinates * 16) */
    u32 z;
    u8  r, g, b, a;
    float q;
    float s, t;
    u32 u, v;        /* 10.4 fixed */
    u8  f;
} gs_vertex;

typedef struct gs_ctx {
    u64 tex0, tex1, tex2, clamp, xyoffset, miptbp1, miptbp2, scissor, alpha, test, fba, frame, zbuf;
} gs_ctx;

typedef struct gs_state {
    /* general registers */
    u64 prim, rgbaq, st, uv, fog, prmodecont, prmode, texclut, scanmsk, texa, fogcol, texflush,
        dimx, dthe, colclamp, pabe, bitbltbuf, trxpos, trxreg, trxdir, signal, finish, label;
    gs_ctx ctx[2];
    float  q;                  /* RGBAQ.Q latched */
    /* vertex queue */
    gs_vertex vq[3];
    int    vcount;             /* vertices queued for the current primitive */
    int    fan_first;          /* triangle fan: index of the first vertex is kept in vq[0] */
    /* CLUT buffer (1 KB: 256 entries x 32 bits, or 512 x 16 bits stored as 16-bit in the low halves) */
    u16    clut[512];
    u32    cbp0, cbp1;         /* CLD bookkeeping (CBP of the last load by CLD 4/5) */
    /* host->local transfer */
    int    trx_active;
    u32    trx_x, trx_y;       /* current position inside the rectangle */
    u32    trx_partial[3];     /* leftover bytes for 24-bit/4-bit packing */
    int    trx_partial_bytes;
    /* privileged registers */
    u64 pmode, smode1, smode2, srfsh, synch1, synch2, syncv, dispfb[2], display[2], extbuf, extdata, extwrite,
        bgcolor, csr, imr, busdir, siglblid;
} gs_state;

extern gs_state gs;

void gs_reset(void);
void gs_write_reg(u32 reg, u64 value);          /* A+D register numbers 0x00..0x63 */
void gs_write_hwreg(const u32 *words, u32 nwords); /* image data (HWREG / GIF IMAGE), 32-bit words */
void gs_local_to_host(u32 dst_addr, u32 qwc);
void gs_draw_prim(const gs_vertex *v, int n);   /* n = vertices of one primitive (1, 2, 3; sprite 2) */
void vu1_stats_field(void);                     /* DOTR_VU1_STATS: VU1 work per field (vu1.c) */
void gs_census_field(void);
int gfx_write_png(const char *path, const u32 *rgba, int w, int h);   /* RGBA PNG, alpha kept (tools) */                     /* DOTR_GS_CENSUS: drawing-mode survey lines (gs_draw.c) */
/* Primitives may be queued and drawn in batches by several threads; gs_flush draws everything queued.
 * Anything that reads or writes GS memory outside drawing calls it first. gs_in_dma > 0 lets a whole DMA
 * transfer batch up (it flushes at its end). */
void gs_flush(void);
extern int gs_in_dma;
void gs_flush_async(void);            /* start drawing the queue on the workers, keep going (gs_draw.c) */
void gs_sync_blocks(u32 lo, u32 hi);  /* wait for queued drawing into GS blocks lo..hi */
extern int gfx_no_draw;   /* gfx_config.no_draw (or DOTR_GFX_NO_DRAW), off while captures are armed */

/* Display read-out (gs_display.c): fills rgba (width*height) with what the TV shows; returns 0 if nothing. */
int  gs_display_size(int *w, int *h);
void gs_display_read(u32 *rgba, int w, int h);

/* ---- GIF (gif.c) ----------------------------------------------------------------------------------- */

enum { GIF_PATH1 = 0, GIF_PATH2 = 1, GIF_PATH3 = 2 };
void gif_reset(void);
/* Feed quadwords to a path. Returns the number of qwords consumed (PATH1 stops at EOP). */
u32  gif_write(int path, const qword *q, u32 nq);
int  gif_path_idle(int path);  /* 1 = between packets (last tag had EOP and its data is done) */
int  gif_path_ended(int path); /* 1 = the last packet's EOP tag and its data are done */
void *gif_state_ptr(void);     /* raw path state, for captures */
u32  gif_state_size(void);

/* ---- VU1 (vu1.c) ----------------------------------------------------------------------------------- */

#define VU1_MEM_BYTES   0x4000u
#define VU1_MICRO_BYTES 0x4000u

typedef union { float f[4]; u32 u[4]; s32 s[4]; } vu_vec;

typedef struct vu1_state {
    vu_vec vf[32];
    u16    vi[16];
    vu_vec acc;
    u32    q, p, i, r;         /* raw bits */
    u32    status, mac, clip;  /* current (visible) flags */
    u32    tpc;
    u8     mem[VU1_MEM_BYTES];
    u8     micro[VU1_MICRO_BYTES];
    int    running;
    u64    cycles;             /* total cycles executed */
} vu1_state;

extern vu1_state vu1;

void vu1_reset(void);
void vu1_micro_changed(void);        /* micro memory was written (MPG, capture restore) */
void vu1_run(u32 start_pc);          /* run to the E bit (MSCAL/MSCALF) */
void vu1_continue(void);             /* MSCNT */
/* Disassemble one instruction pair at word address pc (for traces and tests). */
void vu1_disasm(u32 upper, u32 lower, char *out, size_t outsz);

/* ---- VIF1 (vif1.c) --------------------------------------------------------------------------------- */

typedef struct vif1_state {
    u32 row[4], col[4], mask;
    u32 cycle_cl, cycle_wl, mode;
    u32 base, ofst, tops, top, itops, itop;
    int dbf;
    u32 mark;
    int mskpath3;
    /* command in progress */
    u32 code;           /* current VIFcode */
    u32 remain;         /* words still expected for the current command (0 = waiting for a VIFcode) */
    u32 num;            /* unpack: vectors still to write */
    u32 addr;           /* unpack/mpg destination (bytes for unpack, instructions for mpg) */
    u32 cl;             /* unpack: position in the write cycle */
    u32 wbuf[4];        /* partial data word buffer (unpack / direct qword / mpg pair) */
    u32 wbuf_n;
    u32 pend_bits;      /* unpack: bit reservoir for sub-word formats */
    int pend_nbits;
    int stalled;        /* i-bit interrupt stall */
} vif1_state;

extern vif1_state vif1;

void vif1_reset(void);
void vif1_write(const u32 *words, u32 n);

/* ---- DMA (dma.c) ----------------------------------------------------------------------------------- */

void *gfx_ee_ptr(u32 addr);           /* EE address (RAM, mirrors, scratchpad) -> host pointer */

/* ---- capture (capture.c) --------------------------------------------------------------------------- */

void capture_begin_frame(void);        /* called at vblank: closes the previous frame's record */
void capture_note_kick(const hw_dma_kick *k);
void capture_note_priv(u32 addr, u64 v);
void capture_note_store(u64 bitbltbuf, u64 trxpos, u64 trxreg, u32 dst, u32 qwc);
void capture_request(void);            /* F12: record the next whole frame */
int  capture_recording(void);
void capture_note_stream(int kind, const u32 *words, u32 n);  /* raw data the DMA delivered */
const char *gfx_capture_dir(void);

#endif
