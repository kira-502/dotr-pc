/* dma.c - EE DMA for the graphics channels: source-chain walker (with TTE) feeding VIF1 and GIF PATH3. */
#include "gfx_internal.h"
#include "gfx.h"

/* Test hook: when set, EE RAM / scratchpad live in these buffers instead of at their PS2 addresses. */
u8 *gfx_test_ram;   /* 32 MB */
u8 *gfx_test_spr;   /* 16 KB */

/* Every address in the port's process is valid memory: PS2 RAM at its console addresses, the scratchpad at
 * 0x70000000, and natively compiled data (Sony packet buffers in dotr.dll, game thread stacks at 0x02000000+)
 * anywhere else. Bit 31 is the DMA tag's SPR flag; the 0x2/0x3 mirror bits only matter inside the 32 MB of RAM. */
void *gfx_ee_ptr(u32 addr)
{
    if ((addr & 0x80000000u) || (addr >= PS2_SPR_BASE && addr < PS2_SPR_BASE + PS2_SPR_SIZE)) {
        u32 o = addr & (PS2_SPR_SIZE - 1);
        return gfx_test_spr ? gfx_test_spr + o : PS2_PTR(PS2_SPR_BASE + o);
    }
    addr = ps2_phys(addr);
    if (gfx_test_ram)
        return addr < PS2_RAM_SIZE ? gfx_test_ram + addr : NULL;
    return addr ? PS2_PTR(addr) : NULL;
}

static void deliver(int channel, const u32 *w, u32 nwords)
{
    if (!nwords) return;
    if (channel == HW_DMA_VIF1) {
        capture_note_stream(0, w, nwords);
        vif1_write(w, nwords);
    } else {
        capture_note_stream(1, w, nwords);
        gif_write(GIF_PATH3, (const qword *)w, nwords / 4);
    }
}

static void deliver_mem(int channel, u32 addr, u32 qwc)
{
    /* data may cross the end of the scratchpad or RAM region: copy qword by qword when needed */
    while (qwc) {
        const u32 *p = (const u32 *)gfx_ee_ptr(addr);
        u32 n = qwc;
        if (!p) { hw_log(0, "dma%d: bad address %08x", channel, addr); return; }
        if ((addr & 0x80000000u) || (addr >= PS2_SPR_BASE && addr < PS2_SPR_BASE + PS2_SPR_SIZE)) {
            u32 left = (PS2_SPR_SIZE - (addr & (PS2_SPR_SIZE - 1))) / 16;
            if (n > left) n = left;
        }
        deliver(channel, p, n * 4);
        addr += n * 16;
        qwc -= n;
    }
}

static void run_chain(int channel, u32 tadr, int tte)
{
    u32 stack[2];
    int sp = 0, guard = 0;
    for (;;) {
        const u32 *t = (const u32 *)gfx_ee_ptr(tadr);
        u32 qwc, id, addr;
        int end = 0;
        if (!t) { hw_log(0, "dma%d: bad tag address %08x", channel, tadr); return; }
        qwc = t[0] & 0xFFFF;
        id = (t[0] >> 28) & 7;
        addr = t[1] & 0xFFFFFFF0u;   /* bit 31 = SPR */
        if (tte && channel == HW_DMA_VIF1)
            deliver(channel, t + 2, 2);
        switch (id) {
        case 0: /* refe */ deliver_mem(channel, addr, qwc); end = 1; break;
        case 1: /* cnt */  deliver_mem(channel, tadr + 16, qwc); tadr += 16 + qwc * 16; break;
        case 2: /* next */ deliver_mem(channel, tadr + 16, qwc); tadr = addr; break;
        case 3: case 4: /* ref, refs */ deliver_mem(channel, addr, qwc); tadr += 16; break;
        case 5: /* call */
            deliver_mem(channel, tadr + 16, qwc);
            if (sp < 2) stack[sp++] = tadr + 16 + qwc * 16;
            else { hw_log(0, "dma%d: call stack overflow", channel); end = 1; }
            tadr = addr;
            break;
        case 6: /* ret */
            deliver_mem(channel, tadr + 16, qwc);
            if (sp) tadr = stack[--sp]; else end = 1;
            break;
        default: /* end */ deliver_mem(channel, tadr + 16, qwc); end = 1; break;
        }
        if (end) return;
        if (++guard > 1000000) { hw_log(0, "dma%d: chain did not end", channel); return; }
    }
}

/* Scratchpad channels: plain copies between RAM and the scratchpad (normal mode; toSPR also in chain mode). */
static void spr_copy(int to_spr, u32 ram, u32 sadr, u32 qwc)
{
    while (qwc--) {
        u8 *r = (u8 *)gfx_ee_ptr(ram), *s = (u8 *)gfx_ee_ptr(0x70000000u | (sadr & (PS2_SPR_SIZE - 16)));
        if (!r) { hw_log(0, "dma spr: bad address %08x", ram); return; }
        if (to_spr) memcpy(s, r, 16); else memcpy(r, s, 16);
        ram += 16; sadr += 16;
    }
}

static void spr_chain(u32 tadr, u32 sadr, int tte)
{
    int guard = 0;
    for (;;) {
        const u32 *t = (const u32 *)gfx_ee_ptr(tadr);
        u32 qwc, id, addr;
        if (!t) return;
        qwc = t[0] & 0xFFFF; id = (t[0] >> 28) & 7; addr = t[1] & 0xFFFFFFF0u;
        if (tte) { spr_copy(1, tadr, sadr, 1); sadr += 16; }
        switch (id) {
        case 0: spr_copy(1, addr, sadr, qwc); return;
        case 1: spr_copy(1, tadr + 16, sadr, qwc); sadr += qwc * 16; tadr += 16 + qwc * 16; break;
        case 2: spr_copy(1, tadr + 16, sadr, qwc); sadr += qwc * 16; tadr = addr; break;
        case 3: case 4: spr_copy(1, addr, sadr, qwc); sadr += qwc * 16; tadr += 16; break;
        default: spr_copy(1, tadr + 16, sadr, qwc); return;
        }
        if (++guard > 100000) return;
    }
}

int gfx_dma_kick(const hw_dma_kick *k)
{
    capture_note_kick(k);
    switch (k->channel) {
    case HW_DMA_VIF1: case HW_DMA_GIF:
        gs_in_dma++;
        if (k->mode == 1) run_chain(k->channel, k->tadr, k->tte);
        else deliver_mem(k->channel, k->madr, k->qwc);
        gs_in_dma--;
        gs_flush_async();
        hw_raise_dmac(k->channel);
        return 1;
    case HW_DMA_SPR_TO:
        if (k->mode == 1) spr_chain(k->tadr, k->sadr, k->tte);
        else spr_copy(1, k->madr, k->sadr, k->qwc);
        hw_raise_dmac(k->channel);
        return 1;
    case HW_DMA_SPR_FROM:
        spr_copy(0, k->madr, k->sadr, k->qwc);
        hw_raise_dmac(k->channel);
        return 1;
    default:
        return 0;
    }
}

int gfx_dma_busy(int channel) { (void)channel; return 0; }
