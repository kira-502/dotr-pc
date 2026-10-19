/*
 * The marker build: finds stack packets read past their end (a packet header on the stack whose reader takes more
 * words than the C type holds).
 *
 * Generated with PORTER_SLACK_MARK=1, every local the stack slack wraps (gen/stackslack.py) has its 16 slack bytes
 * set to 0xA5 when it is declared. On the EE such a packet's tail (TEX0/TEX1, an alpha word) was the next local in
 * the frame; on the PC that next local is somewhere else and the reader sees the marker instead. Two checks:
 *   - the packet readers themselves (ExSortPrim_Pipe, ExSortNode_Pipe): every word their header asks for, checked
 *     before they copy it, logged once per caller with the caller's name;
 *   - every DMA chain the game starts (runtime/mmio.c): any quadword holding the marker, logged with the tag.
 * In a normal build the readers never call in here and the DMA check is off.
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "runtime.h"

void crash_where(uintptr_t a, char *out, int n);       /* runtime/crash.c */
const char *rc_current_module(void);                   /* recomp/ */

#define MARK32 0xA5A5A5A5u

static int marked(const uint32_t *w)
{
    return w[0] == MARK32 || w[1] == MARK32;
}

static void found(const char *reader, const void *ret, const void *hdr, unsigned off, unsigned flags, int count)
{
    static const void *seen[64];
    static int nseen;
    char w[400];
    int i;
    for (i = 0; i < nseen; i++)
        if (seen[i] == ret) return;
    if (nseen < 64) seen[nseen++] = ret;
    crash_where((uintptr_t)ret, w, sizeof w);
    hw_log(1, "mark: %s reads the slack after its packet at +0x%X (header %08X, flags %04X, count %d), from %s, "
           "frame %u, module %s", reader, off, (unsigned)(uintptr_t)hdr, flags, count, w, rt_vblanks(),
           rc_current_module() ? rc_current_module() : "none");
}

/* ExSortPrim_Pipe: u16 count, u16 flags, u32, u64 registers, u64 data[count] + alpha / TEX0, TEX1 / MIPTBP1 */
void rt_mark_prim(const void *prim, void *ret)
{
    const uint8_t *p = prim;
    unsigned count = *(const uint16_t *)p, flags = *(const uint16_t *)(p + 2), i, last;
    if ((flags & 0xF) == 7) return;
    last = count;                                       /* words data[0 .. last-1] are read */
    if (flags & 0x200) last = count + 1;
    if (flags & 0x20) last = count + ((flags & 0x4000) ? 4 : 3);
    for (i = 0; i < last; i++) {
        if (i == count && !(flags & 0x200)) continue;   /* the alpha slot is only read with 0x200 */
        if (marked((const uint32_t *)(p + 0x10 + 8 * i))) { found("ExSortPrim_Pipe", ret, prim, 0x10 + 8 * i, flags, count); return; }
    }
}

/* ExSortNode_Pipe: u16 count, u16 type, u32, u64 registers, data[0] alpha, data[1..2] TEX0/TEX1, data[3] MIPTBP1 */
void rt_mark_node(const void *header, void *ret)
{
    const uint8_t *p = header;
    unsigned type = *(const uint16_t *)(p + 2);
    if ((type & 0xF) == 7) return;
    if ((type & 0x200) && marked((const uint32_t *)(p + 0x10))) { found("ExSortNode_Pipe", ret, header, 0x10, type, -1); return; }
    if ((type & 0x20) && (marked((const uint32_t *)(p + 0x18)) || marked((const uint32_t *)(p + 0x20)) ||
                          ((type & 0x4000) && marked((const uint32_t *)(p + 0x28)))))
        found("ExSortNode_Pipe", ret, header, 0x18, type, -1);
}

/* one line per register (the GIF A+D word beside the marker) and module: the same packet comes every field */
static int fresh(uint32_t key)
{
    static uint32_t seen[64];
    static int n;
    int i;
    for (i = 0; i < n; i++)
        if (seen[i] == key) return 0;
    if (n < 64) seen[n++] = key;
    return 1;
}

/* runtime/mmio.c, DOTR_SLACK_MARK=1: a DMA chain about to start; any quadword holding the marker is logged */
void rt_mark_dma(int channel, uint32_t madr, uint32_t qwc, uint32_t tadr, int mode)
{
    static int on = -1, told;
    uint32_t a, stack[2];
    int sp = 0, n = 0;
    if (on < 0) on = GetEnvironmentVariableA("DOTR_SLACK_MARK", NULL, 0) != 0;
    if (!on || told >= 40) return;
#define SCAN(from, q) do { uint32_t s_ = (from) & 0x01FFFFF0u, k_; \
        for (k_ = 0; k_ < (q) && s_ + 16 * k_ < 0x02000000u; k_++) { \
            const uint32_t *w_ = (const uint32_t *)(uintptr_t)(s_ + 16 * k_); \
            if (((w_[0] == MARK32 && w_[1] == MARK32) || (w_[2] == MARK32 && w_[3] == MARK32)) && \
                fresh(w_[2] ^ (uint32_t)(uintptr_t)rc_current_module())) { \
                told++; \
                hw_log(1, "mark: DMA ch%d data %08X: %08X %08X %08X %08X (chain tag %08X), frame %u, module %s", \
                       channel, s_ + 16 * k_, w_[0], w_[1], w_[2], w_[3], a, rt_vblanks(), \
                       rc_current_module() ? rc_current_module() : "none"); \
                return; } } } while (0)
    a = madr;
    if (mode != 1) { if (!(madr & 0x80000000u)) SCAN(madr, qwc); return; }
    a = tadr & 0x01FFFFF0u;
    while (n++ < 100000 && a < 0x02000000u && !(tadr & 0x80000000u)) {
        const uint32_t *q = (const uint32_t *)(uintptr_t)a;
        uint32_t id = (q[0] >> 28) & 7, c = q[0] & 0xFFFF, addr = q[1];
        switch (id) {
        case 0: if (!(addr & 0x80000000u)) SCAN(addr, c); return;                       /* refe */
        case 1: SCAN(a + 16, c); a = a + 16 + c * 16; break;                           /* cnt */
        case 2: SCAN(a + 16, c); a = addr & 0x01FFFFF0u; break;                        /* next */
        case 3: case 4: if (!(addr & 0x80000000u)) SCAN(addr, c); a += 16; break;      /* ref, refs */
        case 5: SCAN(a + 16, c); if (sp < 2) stack[sp++] = a + 16 + c * 16; a = addr & 0x01FFFFF0u; break;  /* call */
        case 6: SCAN(a + 16, c); if (!sp) return; a = stack[--sp]; break;              /* ret */
        default: SCAN(a + 16, c); return;                                              /* end */
        }
    }
#undef SCAN
}
