/*
 * The parts of Sony's libdma/libgraph that cannot run natively as they are: they start DMA by writing channel
 * registers through a pointer (sceDmaSend & co.), wait for the vertical blank (sceGsSyncV), or read the GS back.
 * Everything else in those libraries (main_026.c, main_027.c) is compiled from the decomp and talks to the
 * virtual hardware (runtime/mmio.c).
 */
#include "hle.h"

#define R32(a) (*(volatile u32 *)(a))
void hw_mmio_w(unsigned int addr, unsigned long long value, int size);

static void hle_dma_start(void *chan, u32 mode, u32 dir)
{
    u32 base = (u32)chan;
    u32 chcr = R32(base);
    chcr = (chcr & ~0x0Du) | (mode << 2) | dir | 0x100;
    hw_mmio_w(base, chcr, 4);
}

HLE_FUNC void sceDmaSend(void *chan, void *tag)
{
    u32 base = (u32)chan;
    if (R32(base + 0x30) != 0xFFFFFFFFu) R32(base + 0x30) = (u32)tag;
    R32(base + 0x20) = 0;
    hle_dma_start(chan, 1, 1);
}

HLE_FUNC void sceDmaSendN(void *chan, void *addr, s32 qwc)
{
    u32 base = (u32)chan;
    if (R32(base + 0x10) != 0xFFFFFFFFu) R32(base + 0x10) = (u32)addr;
    R32(base + 0x20) = (u32)qwc;
    hle_dma_start(chan, 0, 1);
}

HLE_FUNC void sceDmaSendI(void *chan, void *addr, s32 qwc)
{
    u32 base = (u32)chan;
    R32(base + 0x10) = (u32)addr;
    R32(base + 0x20) = (u32)qwc;
    hle_dma_start(chan, 2, 1);
}

HLE_FUNC void sceDmaRecv(void *chan) { R32((u32)chan + 0x20) = 0; hle_dma_start(chan, 1, 0); }
HLE_FUNC void sceDmaRecvN(void *chan, void *addr, s32 qwc)
{
    R32((u32)chan + 0x10) = (u32)addr;
    R32((u32)chan + 0x20) = (u32)qwc;
    hle_dma_start(chan, 0, 0);
}
HLE_FUNC void sceDmaRecvI(void *chan, void *addr, s32 qwc)
{
    R32((u32)chan + 0x10) = (u32)addr;
    R32((u32)chan + 0x20) = (u32)qwc;
    hle_dma_start(chan, 2, 0);
}

/* transfers finish inside the start call */
HLE_FUNC s32 sceDmaSync(void *chan, s32 mode, s32 timeout) { (void)chan; (void)mode; (void)timeout; return 0; }
HLE_FUNC s32 sceDmaWatch(void *chan, void *addr, s32 mode, s32 timeout)
{
    (void)chan; (void)addr; (void)mode; (void)timeout;
    return 0;
}

/* sceGsSyncV: wait for the next vertical blank; returns the field now displayed (0 even, 1 odd) */
HLE_FUNC s32 sceGsSyncV(s32 mode)
{
    (void)mode;
    rt_wait_vblank();
    return (s32)(rt_vblank_count() & 1);
}

/* sceGsExecStoreImage: the GS rectangle described by a sceGsSetDefStoreImage packet (VIF codes, GIF tag, then
 * BITBLTBUF +0x20, TRXPOS +0x30, TRXREG +0x40) copied to EE memory at dst by lane C's gfx (hw/hw.h).
 * RhBase_CopyFrameBuffer is its only caller. */
void gfx_gs_store_image(unsigned long long bitbltbuf, unsigned long long trxpos, unsigned long long trxreg,
                        unsigned int dst_addr, unsigned int qwc);
HLE_FUNC s32 sceGsExecStoreImage(void *image, void *dst)
{
    const u8 *p = (const u8 *)image;
    u64 bitbltbuf = *(const u64 *)(p + 0x20), trxpos = *(const u64 *)(p + 0x30), trxreg = *(const u64 *)(p + 0x40);
    u32 w = (u32)trxreg & 0xFFF, h = (u32)(trxreg >> 32) & 0xFFF, psm = (u32)(bitbltbuf >> 24) & 0x3F, bytes;
    switch (psm) {
    case 0x00: case 0x30: bytes = w * h * 4; break;               /* PSMCT32, PSMZ32 */
    case 0x01: case 0x31: bytes = w * h * 3; break;               /* PSMCT24, PSMZ24 */
    case 0x02: case 0x0A: case 0x32: case 0x3A: bytes = w * h * 2; break;   /* 16-bit */
    case 0x13: bytes = w * h; break;                              /* PSMT8 */
    case 0x14: bytes = (w * h) / 2; break;                        /* PSMT4 */
    default: rt_log(1, "sceGsExecStoreImage: pixel format %02X not handled", psm); return 0;
    }
    {
        static int told;
        if (!told++) rt_log(2, "sceGsExecStoreImage: %ux%u psm %02X to %08X (%u bytes)", w, h, psm, (u32)dst, bytes);
    }
    gfx_gs_store_image(bitbltbuf, trxpos, trxreg, (u32)dst & 0x0FFFFFFF, (bytes + 15) / 16);
    return 1;
}

/* libpkt: packet = { current write pointer, base, open tag }; the decomp keeps its body as GCC asm */
HLE_FUNC void sceGifPkInit(void *p0, u64 p1)
{
    u32 *p = (u32 *)p0;
    p[0] = (u32)p1;
    p[2] = 0;
    p[1] = (u32)p1;
}

/* debug only: dumps the frame buffer to 0x04000000, outside RAM. Never wanted on the PC */
HLE_FUNC void SzDebug_OutputFrameBuffer(void) {}
