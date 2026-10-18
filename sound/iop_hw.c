/* iop_hw.c - the IOP hardware the sound driver touches: DMA channel 4 (IOP RAM -> SPU2 core 0) in normal and
 * AutoDMA (streaming) use, channel 7 (core 1) the same way; everything else (bus timing, DPCR/DICR) is stored and
 * read back. Transfers to SPU RAM finish at once; AutoDMA data is pulled by the SPU2 as it plays. */
#include <stdlib.h>
#include "iop.h"

typedef struct dmach { u32 madr, bcr, chcr; u32 left; } dmach;
static dmach ch4, ch7;
static u32 regs[0x1000];   /* backing store for other registers, by (addr - 0x1F801000) / 4 */

void iop_hw_reset(void)
{
    memset(&ch4, 0, sizeof ch4);
    memset(&ch7, 0, sizeof ch7);
    memset(regs, 0, sizeof regs);
}

static u32 bytes_of(u32 bcr)
{
    u32 bs = bcr & 0xFFFF, cnt = bcr >> 16;
    if (!bs) bs = 0x10000;
    if (!cnt) cnt = 1;
    return bs * 4 * cnt;
}

static void start(dmach *c, int intr)
{
    c->left = bytes_of(c->bcr);
    if (getenv("DOTR_SND_TRACE")) iop_log(2, "dma%d start madr=%06x bytes=%x chcr=%08x adma=%d", intr == 36 ? 4 : 7,
                                          c->madr, c->left, c->chcr, spu2_adma_active(intr == 36 ? 0 : 1));
    if (spu2_adma_active(intr == 36 ? 0 : 1)) return;      /* streaming: the SPU2 pulls the data */
    if ((c->madr & (IOP_RAM_SIZE - 1)) + c->left > IOP_RAM_SIZE) iop_log(0, "iop: SPU2 DMA past IOP RAM (%06x + %x)", c->madr, c->left);
    else if (c->chcr & 1) spu2_dma_write_core(intr == 36 ? 0 : 1, iop_ptr(c->madr), c->left);
    else spu2_dma_read_core(intr == 36 ? 0 : 1, iop_ptr(c->madr), c->left);
    c->madr += c->left;
    c->left = 0;
    c->chcr &= ~0x01000000u;
    iop_raise_intr(intr);
}

u32 iop_dma_pull(int core, u8 *dst, u32 n)
{
    dmach *c = core ? &ch7 : &ch4;
    u32 take;
    if (!(c->chcr & 0x01000000u) || !c->left) return 0;
    take = n < c->left ? n : c->left;
    if ((c->madr & (IOP_RAM_SIZE - 1)) + take > IOP_RAM_SIZE) take = IOP_RAM_SIZE - (c->madr & (IOP_RAM_SIZE - 1));
    memcpy(dst, iop_ptr(c->madr), take);
    c->madr += take;
    c->left -= take;
    if (!c->left) {
        c->chcr &= ~0x01000000u;
        spu2_dma_done(core);
        iop_raise_intr(core ? 40 : 36);
    }
    return take;
}

void iop_hw_tick(void) { iop_check_intr(); }

static u32 *dma_reg(u32 a, int *which)
{
    switch (a) {
    case 0x1F8010C0: *which = 4; return &ch4.madr;
    case 0x1F8010C4: *which = 4; return &ch4.bcr;
    case 0x1F8010C8: *which = 4; return &ch4.chcr;
    case 0x1F801500: *which = 7; return &ch7.madr;
    case 0x1F801504: *which = 7; return &ch7.bcr;
    case 0x1F801508: *which = 7; return &ch7.chcr;
    default: *which = 0; return NULL;
    }
}

u32 iop_hw_read(u32 a, int size)
{
    int w;
    u32 *p = dma_reg(a & ~3u, &w), v;
    if (a == 0x1F801070 || a == 0x1F801074) return 0;           /* I_STAT / I_MASK: the kernel is HLE */
    if (p) v = *p;
    else v = regs[((a - 0x1F801000u) >> 2) & 0xFFF];
    if (size == 2) return (a & 2) ? v >> 16 : v & 0xFFFF;
    if (size == 1) return (v >> ((a & 3) * 8)) & 0xFF;
    return v;
}

void iop_hw_write(u32 a, u32 v, int size)
{
    int w;
    u32 *p = dma_reg(a & ~3u, &w);
    u32 *q = p ? p : &regs[((a - 0x1F801000u) >> 2) & 0xFFF];
    if (size == 2) *q = (a & 2) ? (*q & 0xFFFF) | (v << 16) : (*q & 0xFFFF0000u) | (v & 0xFFFF);
    else if (size == 1) { u32 sh = (a & 3) * 8; *q = (*q & ~(0xFFu << sh)) | ((v & 0xFF) << sh); }
    else *q = v;
    if (p && (a & ~3u) == (w == 4 ? 0x1F8010C8u : 0x1F801508u) && (*q & 0x01000000u))
        start(w == 4 ? &ch4 : &ch7, w == 4 ? 36 : 40);
}
