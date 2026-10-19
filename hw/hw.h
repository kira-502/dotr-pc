/*
 * hw.h - the interface between lane P (platform/runtime) and lane G (graphics) of the DotR PC port.
 *
 * Model: the process is 32-bit and PS2 RAM sits at its console addresses, so a PS2
 * address IS a pointer: PS2_PTR(0x00317490) is the byte at 0x00317490. The scratchpad is at 0x70000000 and the
 * uncached mirrors (0x2xxxxxxx / 0x3xxxxxxx) alias main RAM. Graphics reads DMA chains, packets and textures
 * straight out of that memory; nothing is copied across this interface.
 *
 * Direction of calls:
 *   runtime -> gfx : gfx_*  (implemented by lane G in gfx/)
 *   gfx -> runtime : hw_*   (implemented by lane P in runtime/)
 *
 * Every gfx_* call is made from the game thread (one at a time); gfx never calls back into game code, only hw_*.
 * Change this file only by agreement between the two lanes; note each change in the log at the bottom.
 */
#ifndef DOTR_HW_H
#define DOTR_HW_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint8_t  hw_u8;
typedef uint16_t hw_u16;
typedef uint32_t hw_u32;
typedef uint64_t hw_u64;

/* ---- memory ---------------------------------------------------------------------------------------- */

#define PS2_RAM_BASE      0x00000000u
#define PS2_RAM_SIZE      0x02000000u   /* 32 MB; the port maps 0x00100000..0x01FFFFFF */
#define PS2_SPR_BASE      0x70000000u   /* scratchpad, 16 KB */
#define PS2_SPR_SIZE      0x00004000u

/* A PS2 address as a host pointer (identity mapping). */
#define PS2_PTR(addr)     ((void *)(uintptr_t)(hw_u32)(addr))

/* Strip the uncached / uncached-accelerated mirror bits from an EE RAM address (0x2xxxxxxx, 0x3xxxxxxx).
 * Scratchpad addresses (0x70000000..) and addresses outside main RAM are returned unchanged. */
static inline hw_u32 ps2_phys(hw_u32 addr)
{
    hw_u32 top = addr & 0xF0000000u;
    if ((top == 0x20000000u || top == 0x30000000u) && (addr & 0x0FFFFFFFu) < PS2_RAM_SIZE)
        return addr & 0x0FFFFFFFu;
    return addr;
}

/* ---- DMA ------------------------------------------------------------------------------------------- */

/* EE DMA channels used by DotR (PS2 numbering). */
enum {
    HW_DMA_VIF0     = 0,
    HW_DMA_VIF1     = 1,
    HW_DMA_GIF      = 2,
    HW_DMA_IPU_FROM = 3,
    HW_DMA_IPU_TO   = 4,
    HW_DMA_SIF0     = 5,
    HW_DMA_SIF1     = 6,
    HW_DMA_SIF2     = 7,
    HW_DMA_SPR_FROM = 8,
    HW_DMA_SPR_TO   = 9
};

/* One DMA kick, as the game (through Sony's sceDmaSend family, replaced by the runtime) programs it.
 * mode: 0 = normal (madr/qwc), 1 = source chain (tadr), 2 = interleave (unused by DotR).
 * tte : 1 = transfer the DMA tag's upper 64 bits to the peripheral (VIF1 chains use it).
 * madr/tadr are EE addresses (may carry mirror bits; may point into the scratchpad when spr is set).
 * The transfer runs to completion inside the call (the GS/VU1 work it causes may finish later; see busy). */
typedef struct hw_dma_kick {
    int    channel;
    int    mode;
    int    tte;
    hw_u32 madr;
    hw_u32 qwc;
    hw_u32 tadr;
    hw_u32 sadr;     /* scratchpad address (D8_SADR / D9_SADR) for channels 8/9 */
} hw_dma_kick;

/* ---- gfx side (lane G implements) ------------------------------------------------------------------ */

typedef struct gfx_config {
    int headless;        /* 1: no window (unit tests, capture replay, M0 self-test) */
    int window_scale;    /* initial window size as a multiple of 640x448; 0 = default */
    const char *capture_dir; /* where F12 captures go; NULL = next to the exe */
    int no_draw;         /* 1: keep DMA/VIF/GS register and memory state but run no VU1 programs and draw no
                          * primitives (fast headless bot runs without --shots); ignored while a capture is armed */
} gfx_config;

int    gfx_init(const gfx_config *cfg);      /* 1 = ok */
void   gfx_shutdown(void);

/* Channels VIF1, GIF, SPR->VIF1 paths. Returns 0 when the channel is not one gfx handles. */
int    gfx_dma_kick(const hw_dma_kick *k);
/* 1 while the channel (or the path it feeds: VU1 program, GS transfer) is still busy. A synchronous
 * implementation always returns 0. */
int    gfx_dma_busy(int channel);

/* GS privileged registers, by their EE address 0x12000000..0x12001FFF (PMODE, SMODE*, DISPFB*, DISPLAY*,
 * EXTBUF, EXTDATA, EXTWRITE, BGCOLOR, CSR 0x12001000, IMR 0x12001010, BUSDIR 0x12001040, SIGLBLID 0x12001080). */
void   gfx_gs_priv_write(hw_u32 addr, hw_u64 value);
hw_u64 gfx_gs_priv_read(hw_u32 addr);

/* GS -> EE read-back (sceGsExecStoreImage, local->host): copy the rectangle described by the GS registers
 * BITBLTBUF/TRXPOS/TRXREG (raw 64-bit values) to EE memory at dst_addr, qwc quadwords. */
void   gfx_gs_store_image(hw_u64 bitbltbuf, hw_u64 trxpos, hw_u64 trxreg, hw_u32 dst_addr, hw_u32 qwc);

/* Vertical blank: called by the runtime at 60 Hz (NTSC fields), field = 0/1. gfx presents the displayed frame
 * buffer here (per PMODE/DISPFB/DISPLAY) and polls its window. */
void   gfx_vblank(int field);

/* Window state for the runtime (input focus, quit). Headless: focused = 0, quit = 0. */
int    gfx_window_focused(void);
int    gfx_quit_requested(void);

/* ---- runtime side (lane P implements) -------------------------------------------------------------- */

/* Log a line to the port log (DotR-PC.log next to the exe). level: 0 error, 1 warn, 2 info, 3 debug. */
void   hw_log(int level, const char *fmt, ...);

/* Interrupt sources gfx may raise; the runtime runs the game's registered handlers on the game thread at the
 * next safe point (EE INTC numbering). */
enum { HW_INTC_GS = 0, HW_INTC_VBLANK_START = 2, HW_INTC_VBLANK_END = 3, HW_INTC_VIF1 = 5, HW_INTC_VU1 = 7 };
void   hw_raise_intc(int source);
/* DMA channel completion (D_STAT bit), for handlers added with AddDmacHandler. */
void   hw_raise_dmac(int channel);

/* Monotonic frame/time base the runtime keeps (vblank count since boot). */
hw_u32 hw_vblank_count(void);

/* A whole file from the owner's disc by the game's path ("cdrom0:\\MOVIE\\OPMV_US.PSS;1" or "\\MOVIE\\..."):
 * malloc'd bytes (free with free(); same CRT, everything links into dotr.dll), NULL if missing. */
void  *hw_disc_read_file(const char *path, hw_u32 *size);

/* ---- change log ------------------------------------------------------------------------------------ */
/* 2026-10-17 lane P: first proposal.
 * 2026-10-17 lane G accepted. Agreed: madr/tadr in 0x70000000.. (or a chain tag's SPR bit) means scratchpad;
 *   gfx_dma_kick is synchronous and VU1 programs run to their E-bit inside it, so gfx_dma_busy returns 0;
 *   gfx raises HW_INTC_GS on SIGNAL/FINISH/LABEL (per CSR/IMR), HW_INTC_VIF1 on a VIF i-bit, hw_raise_dmac at
 *   chain end when the tag IRQ bit/CIE asks; F12 is caught in the gfx window.
 * 2026-10-17 lane G asked, lane P agreed: sadr added; gfx runs channels 8/9 (RAM<->scratchpad copies).
 * 2026-10-17 lane C asked (movies), lane P added: hw_disc_read_file. */

#ifdef __cplusplus
}
#endif

#endif /* DOTR_HW_H */
