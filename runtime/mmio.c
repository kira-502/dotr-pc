/*
 * Virtual EE/GS hardware for register accesses (gen/mmio.py rewrites them into these calls).
 *
 * The register pages are plain memory at their PS2 addresses (the loader maps them), so a value written is a
 * value read back. On top of that:
 *   DMA channels: writing CHCR with STR set starts the transfer through lane G (gfx_dma_kick); it completes
 *                 at once, so STR reads back clear and D_STAT gets the channel's bit.
 *   GS privileged registers (0x12000000..): go to lane G (gfx_gs_priv_write / gfx_gs_priv_read).
 *   VIF1 / GIF FIFOs: a quadword written to the FIFO is sent as a one-quadword DMA.
 *   VIF/GIF status: always idle.  Timers: count from real time (T0 at the NTSC hblank rate).
 */
#include <windows.h>
#include <string.h>
#include <stdlib.h>
#include "runtime.h"
#include "hle_rt.h"

#define REG32(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define REG64(a) (*(volatile uint64_t *)(uintptr_t)(a))

/* DOTR_TRACE_GFX=N: from frame N on, log 3000 DMA starts and GS register writes (why is a frame black?) */
static int trace_left = -1;
static unsigned trace_from;
static int trace_on(void)
{
    if (trace_left < 0) {
        char v[16];
        trace_left = GetEnvironmentVariableA("DOTR_TRACE_GFX", v, sizeof v) ? 3000 : 0;
        trace_from = (unsigned)atoi(v);
    }
    if (rt_vblanks() < trace_from) return 0;
    return trace_left > 0 && trace_left--;
}

static int dma_channel_of(uint32_t base)
{
    switch (base) {
    case 0x10008000: return HW_DMA_VIF0;
    case 0x10009000: return HW_DMA_VIF1;
    case 0x1000A000: return HW_DMA_GIF;
    case 0x1000B000: return HW_DMA_IPU_FROM;
    case 0x1000B400: return HW_DMA_IPU_TO;
    case 0x1000C000: return HW_DMA_SIF0;
    case 0x1000C400: return HW_DMA_SIF1;
    case 0x1000C800: return HW_DMA_SIF2;
    case 0x1000D000: return HW_DMA_SPR_FROM;
    case 0x1000D400: return HW_DMA_SPR_TO;
    }
    return -1;
}

static void dma_start(uint32_t base, uint32_t chcr)
{
    hw_dma_kick k;
    int ch = dma_channel_of(base);
    static uint32_t warned;
    memset(&k, 0, sizeof k);
    k.channel = ch;
    k.mode = (chcr >> 2) & 3;
    k.tte = (chcr >> 6) & 1;
    k.madr = REG32(base + 0x10);
    k.qwc = REG32(base + 0x20) & 0xFFFF;
    k.tadr = REG32(base + 0x30);
    k.sadr = REG32(base + 0x80);
    if (trace_on()) {
        hw_log(2, "trace: dma ch%d chcr %08X madr %08X qwc %u tadr %08X", ch, chcr, k.madr, k.qwc, k.tadr);
        if (k.mode == 1) {      /* chain: the first tags (DMAtag + the two VIF codes beside it) */
            uint32_t a = k.tadr & 0x01FFFFF0u;
            int i;
            for (i = 0; i < 6 && a < 0x02000000; i++) {
                uint32_t *q = (uint32_t *)(uintptr_t)a;
                uint32_t id = (q[0] >> 28) & 7, qwc = q[0] & 0xFFFF, addr = q[1] & 0x01FFFFF0u;
                hw_log(2, "trace:   tag %08X: %08X %08X %08X %08X", a, q[0], q[1], q[2], q[3]);
                if (id == 7 || id == 0) break;                 /* end / refe */
                a = id == 1 ? a + 16 + qwc * 16 : id == 2 ? addr : a + 16;   /* cnt / next / ref,refs */
            }
        }
    }
    if (!gfx_dma_kick(&k) && !(warned & (1u << ch))) {
        warned |= 1u << ch;
        hw_log(1, "mmio: DMA channel %d started, nobody handles it yet (chcr %08X)", ch, chcr);
    }
    REG32(base) = chcr & ~0x100u;               /* done: STR clear */
    REG32(base + 0x20) = 0;
    REG32(0x1000E010) |= 1u << ch;               /* D_STAT channel interrupt status */
    hw_raise_dmac(ch);
}

static double t0_start;

/* Timer 1 (T1_COUNT 0x10000800, T1_MODE 0x10000810) on the port's field clock: 262.5 hblanks a field. RhBase
 * (main_056) sets it to count hblanks and zeroes it after each flip; RhBase_FlipBuffer reads it to tell a frame that
 * took two fields (its result goes to SzGame_SetFrameSpeed), SzFile_DoRequest gives file work 0x40 hblanks a frame,
 * SzGame_GetHSyncCount. A count that never moved made every frame look on time (slow motion when the PC is late)
 * and would loop forever in any wait for it. */
double rt_field_phase(void);
static uint32_t t1_base;
static double t1_at;

static double hblanks_now(void)
{
    return ((double)rt_vblanks() + rt_field_phase()) * 262.5;
}

static uint32_t t1_count(void)
{
    static const double per_hblank[4] = { 147456000.0 / 15734.0, 147456000.0 / 15734.0 / 16.0,
                                          147456000.0 / 15734.0 / 256.0, 1.0 };
    uint32_t mode = REG32(0x10000810);
    double e = hblanks_now() - t1_at;
    if (!(mode & 0x80) || e < 0) return t1_base & 0xFFFF;          /* CUE off: stopped */
    return (t1_base + (uint32_t)(uint64_t)(e * per_hblank[mode & 3])) & 0xFFFF;
}

unsigned long long hw_mmio_r(unsigned int addr, int size)
{
    if (addr >= 0x12000000 && addr < 0x12002000) {
        uint64_t v = gfx_gs_priv_read(addr & ~7u);
        if (size < 8) v >>= (addr & 4) * 8;
        return size == 4 ? (uint32_t)v : size == 2 ? (uint16_t)v : size == 1 ? (uint8_t)v : v;
    }
    switch (addr) {
    case 0x10000000:   /* T0_COUNT: NTSC hblank clock (15734 Hz) */
        if (!t0_start) t0_start = rt_seconds();
        return (uint32_t)((rt_seconds() - t0_start) * 15734.0) & 0xFFFF;
    case 0x10000800:   /* T1_COUNT */
        return t1_count();
    case 0x10003020:   /* GIF_STAT: idle */
    case 0x10003C00:   /* VIF1_STAT: idle */
    case 0x10003800:   /* VIF0_STAT */
        return 0;
    }
    switch (size) {
    case 1: return *(volatile uint8_t *)(uintptr_t)addr;
    case 2: return *(volatile uint16_t *)(uintptr_t)addr;
    case 4: return REG32(addr);
    default: return REG64(addr);
    }
}

void hw_mmio_w(unsigned int addr, unsigned long long v, int size)
{
    uint32_t stat_before;
    if (addr >= 0x12000000 && addr < 0x12002000) {
        uint32_t a8 = addr & ~7u;
        uint64_t cur = REG64(a8);
        if (size == 8) cur = v;
        else {
            int sh = (addr & 4) * 8;
            uint64_t mask = (size == 4 ? 0xFFFFFFFFull : size == 2 ? 0xFFFFull : 0xFFull) << sh;
            cur = (cur & ~mask) | ((v << sh) & mask);
        }
        REG64(a8) = cur;
        if (trace_on()) hw_log(2, "trace: gs %08X = %016llX", a8, (unsigned long long)cur);
        gfx_gs_priv_write(a8, cur);
        return;
    }
    if (addr == 0x10000800 || addr == 0x10000810) {   /* T1: a new count, or a new mode from the count so far */
        uint32_t now = addr == 0x10000800 ? (uint32_t)v & 0xFFFF : t1_count();
        if (addr == 0x10000810) REG32(addr) = (uint32_t)v & ~0xC00u;   /* the flag bits are written as 1 to clear */
        t1_base = now;
        t1_at = hblanks_now();
        REG32(0x10000800) = now;
        return;
    }
    stat_before = REG32(0x1000E010);
    switch (size) {
    case 1: *(volatile uint8_t *)(uintptr_t)addr = (uint8_t)v; break;
    case 2: *(volatile uint16_t *)(uintptr_t)addr = (uint16_t)v; break;
    case 4: REG32(addr) = (uint32_t)v; break;
    default: REG64(addr) = v; break;
    }
    if (addr == 0x1000E010) {                   /* D_STAT: a 1 clears a status bit (0-15), toggles a mask bit (16-31) */
        REG32(addr) = (stat_before & ~((uint32_t)v & 0xFFFFu)) ^ ((uint32_t)v & 0xFFFF0000u);
        return;
    }
    if ((addr & 0xFF) == 0 && dma_channel_of(addr & ~0xFFu) >= 0 && ((uint32_t)v & 0x100))
        dma_start(addr & ~0xFFu, (uint32_t)v);
}

void hw_mmio_w128(unsigned int addr, const void *value)
{
    static __declspec(align(16)) uint8_t qw[16];
    hw_dma_kick k;
    memcpy(qw, value, 16);
    memcpy((void *)(uintptr_t)addr, value, 16);
    if (addr == 0x10005000 || addr == 0x10006000) {   /* VIF1 / GIF FIFO: one quadword */
        memset(&k, 0, sizeof k);
        k.channel = addr == 0x10005000 ? HW_DMA_VIF1 : HW_DMA_GIF;
        k.madr = (uint32_t)(uintptr_t)qw;
        k.qwc = 1;
        gfx_dma_kick(&k);
    }
}

void *hw_mmio_p(unsigned int addr)
{
    return (void *)(uintptr_t)addr;
}
