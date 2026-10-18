/* gfxboot - windowless check of the graphics core on the game's own data.
 *   gfxboot <SLUS_205.15> [-d]
 * Loads the executable's segments into a scratch RAM, sends the VU1 microcode chain at D_00286980 through
 * DMA -> VIF1 (as RhBase_Init does), and reports what landed in VU1 and what the boot program did.
 * With -d it disassembles VU1 micro memory. Reads the owner's file; writes nothing. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../gfx_internal.h"
#include "gfx.h"

int main(int argc, char **argv)
{
    gfx_config cfg = { 1, 0, NULL };
    FILE *f;
    u8 *elf;
    long size;
    u32 phoff, i, used = 0;
    u16 phnum;
    hw_dma_kick k;
    if (argc < 2) { fprintf(stderr, "usage: gfxboot <SLUS_205.15> [-d]\n"); return 1; }
    f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }
    fseek(f, 0, SEEK_END); size = ftell(f); fseek(f, 0, SEEK_SET);
    elf = (u8 *)malloc((size_t)size);
    if (fread(elf, 1, (size_t)size, f) != (size_t)size) { fprintf(stderr, "read failed\n"); return 1; }
    fclose(f);
    gfx_test_ram = (u8 *)calloc(32u << 20, 1);
    gfx_test_spr = (u8 *)calloc(16384, 1);
    gfx_init(&cfg);
    memcpy(&phoff, elf + 0x1C, 4); memcpy(&phnum, elf + 0x2C, 2);
    for (i = 0; i < phnum; i++) {
        u32 ph[8];
        memcpy(ph, elf + phoff + i * 32, 32);
        if (ph[0] == 1 && ph[4] && ph[2] + ph[4] <= (32u << 20))
            memcpy(gfx_test_ram + ph[2], elf + ph[1], ph[4]);
    }
    memset(&k, 0, sizeof k);
    k.channel = HW_DMA_VIF1; k.mode = 1; k.tte = 1; k.tadr = 0x286980;
    gfx_dma_kick(&k);
    for (i = 0; i < VU1_MICRO_BYTES / 8; i++) {
        u32 lo, hi;
        memcpy(&lo, vu1.micro + i * 8, 4); memcpy(&hi, vu1.micro + i * 8 + 4, 4);
        if (lo || hi) used++;
    }
    printf("VU1 micro memory: %u of 2048 instructions loaded\n", used);
    printf("boot program: ran %llu cycles, stopped at %03x\n", (unsigned long long)vu1.cycles, vu1.tpc);
    printf("VIF1: base=%u offset=%u tops=%u itops=%u cycle=%u/%u\n", vif1.base, vif1.ofst, vif1.tops, vif1.itops,
           vif1.cycle_cl, vif1.cycle_wl);
    for (i = 1; i < 16; i++) if (vu1.vi[i]) printf("  vi%02u = %04x\n", i, vu1.vi[i]);
    for (i = 1; i < 32; i++)
        if (vu1.vf[i].u[0] | vu1.vf[i].u[1] | vu1.vf[i].u[2] | vu1.vf[i].u[3])
            printf("  vf%02u = %g %g %g %g\n", i, (double)vu1.vf[i].f[0], (double)vu1.vf[i].f[1], (double)vu1.vf[i].f[2], (double)vu1.vf[i].f[3]);
    if (argc > 2 && !strcmp(argv[2], "-d")) {
        for (i = 0; i < VU1_MICRO_BYTES / 8; i++) {
            u32 lo, hi;
            char line[160];
            memcpy(&lo, vu1.micro + i * 8, 4); memcpy(&hi, vu1.micro + i * 8 + 4, 4);
            if (!lo && !hi) continue;
            vu1_disasm(hi, lo, line, sizeof line);
            printf("%04x  %08x %08x  %s\n", i * 8, hi, lo, line);
        }
    }
    return 0;
}
