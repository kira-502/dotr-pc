/* gfxbench - how fast the software GS draws typical DotR work (windowless). */
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include "../gfx_internal.h"
#include "gfx.h"
#include "prof.h"

static qword pk[8192];
static int npk;
static void q_put(u64 lo, u64 hi) { pk[npk].w[0] = (u32)lo; pk[npk].w[1] = (u32)(lo >> 32); pk[npk].w[2] = (u32)hi; pk[npk].w[3] = (u32)(hi >> 32); npk++; }
static void ad(u32 reg, u64 v) { q_put(v, reg); }
static void ad_begin(u32 n) { q_put((u64)n | (1ull << 15) | (1ull << 60), 0xE); }
static void flush(void) { gif_write(GIF_PATH3, pk, (u32)npk); npk = 0; }

int main(void)
{
    gfx_config cfg = { 1, 0, NULL };
    double t0, t1;
    int i, x, y, frames = 10;
    gfx_test_ram = (u8 *)calloc(32u << 20, 1);
    gfx_test_spr = (u8 *)calloc(16384, 1);
    gfx_init(&cfg);
    for (y = 0; y < 256; y++) for (x = 0; x < 256; x++) gs_write_pixel(PSMT8, 0x2400, 4, x, y, (x ^ y) & 255);
    for (i = 0; i < 256; i++) gs_write_pixel(PSMCT32, 0x2800, 1, i & 15, i >> 4, 0x80000000u | (i * 0x010101u));
    ad_begin(8);
    ad(0x4C, 0 | (10ull << 16));
    ad(0x4E, 0x80);
    ad(0x40, (u64)639 << 16 | ((u64)447 << 48));
    ad(0x47, (1ull << 16) | (1ull << 17));
    ad(0x42, 0 | (1 << 2) | (0 << 4) | (1 << 6));
    ad(0x06, 0x2400 | (4ull << 14) | ((u64)PSMT8 << 20) | (8ull << 26) | (8ull << 30) | (1ull << 34) | ((u64)0x2800 << 37) | (1ull << 61));   /* above the z buffer */
    ad(0x14, 1 << 5);
    ad(0x46, 1);
    flush();

    /* 0: cost breakdown on one full-screen sprite */
    {
        static const struct { const char *name; u64 prim; u64 tex1; } v[] = {
            { "flat fill", 6, 0 }, { "flat + blend", 6 | (1 << 6), 0 }, { "texture nearest", 6 | (1 << 4) | (1 << 8), 0 },
            { "texture bilinear", 6 | (1 << 4) | (1 << 8), 1 << 5 }, { "bilinear + blend", 6 | (1 << 4) | (1 << 6) | (1 << 8), 1 << 5 } };
        int k;
        for (k = 0; k < 5; k++) {
            ad_begin(1); ad(0x14, v[k].tex1); flush();
            t0 = now_ms();
            for (i = 0; i < 20; i++) {
                ad_begin(6);
                ad(0x00, v[k].prim);
                ad(0x01, 0x80808080ull | (0x3F800000ull << 32));
                ad(0x03, 0); ad(0x05, 0);
                ad(0x03, (256 * 16) | ((u64)(256 * 16) << 16));
                ad(0x05, (640 * 16) | ((u64)(448 * 16) << 16));
                flush();
            }
            t1 = now_ms();
            printf("  %-18s %.1f ns/pixel\n", v[k].name, (t1 - t0) * 1e6 / (20.0 * 640 * 448));
        }
        ad_begin(1); ad(0x14, 1 << 5); flush();
    }

    /* 1: twelve full-screen textured, blended, bilinear sprites (2D screens: backgrounds, windows, cards) */
    t0 = now_ms();
    gs_in_dma++;   /* batch like one DMA transfer */
    for (i = 0; i < frames * 12; i++) {
        ad_begin(6);
        ad(0x00, 6 | (1 << 4) | (1 << 6) | (1 << 8));
        ad(0x01, 0x80808080ull | (0x3F800000ull << 32));
        ad(0x03, 0); ad(0x05, 0);
        ad(0x03, (256 * 16) | ((u64)(256 * 16) << 16));
        ad(0x05, (640 * 16) | ((u64)(448 * 16) << 16));
        flush();
    }
    gs_in_dma--; gs_flush();
    t1 = now_ms();
    printf("12 full-screen sprites (3.4 M pixels): %.2f ms per frame\n", (t1 - t0) / frames);

    /* 1b: triangle cost by feature (single batch of 4000) */
    prof_start();
    {
        static const struct { const char *name; u64 prim; u64 tex1; } v[] = {
            { "tri gouraud", 3 | (1 << 3), 0 }, { "tri + texture nearest", 3 | (1 << 3) | (1 << 4), 0 },
            { "tri + texture bilinear", 3 | (1 << 3) | (1 << 4), 1 << 5 } };
        int k;
        for (k = 0; k < 3; k++) {
            ad_begin(1); ad(0x14, v[k].tex1); flush();
            t0 = now_ms();
            gs_in_dma++;
            for (i = 0; i < 4000; i++) {
                int bx = (i * 37) % 600, by = (i * 53) % 420;
                ad_begin(10);
                ad(0x00, v[k].prim);
                ad(0x01, 0x80FF8040ull | (0x3F800000ull << 32)); ad(0x02, 0); ad(0x05, (u64)(bx * 16) | ((u64)(by * 16) << 16) | (100ull << 32));
                ad(0x01, 0x8040FF80ull | (0x3F800000ull << 32)); ad(0x02, 0x3F000000ull); ad(0x05, (u64)((bx + 30) * 16) | ((u64)(by * 16) << 16) | (100ull << 32));
                ad(0x01, 0x808040FFull | (0x3F800000ull << 32)); ad(0x02, 0x3F0000003F000000ull); ad(0x05, (u64)(bx * 16) | ((u64)((by + 25) * 16) << 16) | (100ull << 32));
                flush();
            }
            gs_in_dma--; gs_flush();
            t1 = now_ms();
            printf("  %-24s %.2f ms per 4000\n", v[k].name, t1 - t0);
        }
        ad_begin(1); ad(0x14, 1 << 5); flush();
    }
    prof_stop();
    { extern unsigned gs_tex_decodes, gs_tex_invalidations; printf("texture decodes %u, invalidations %u\n", gs_tex_decodes, gs_tex_invalidations); }

    /* 2: 4000 small perspective-textured gouraud triangles with z test (3D duel field, monsters) */
    t0 = now_ms();
    gs_in_dma++;
    for (i = 0; i < frames * 4000; i++) {
        int bx = (i * 37) % 600, by = (i * 53) % 420;
        ad_begin(10);
        ad(0x00, 3 | (1 << 3) | (1 << 4));
        ad(0x01, 0x80FF8040ull | (0x3F800000ull << 32)); ad(0x02, 0); ad(0x05, (u64)(bx * 16) | ((u64)(by * 16) << 16) | (100ull << 32));
        ad(0x01, 0x8040FF80ull | (0x3F800000ull << 32)); ad(0x02, 0x3F000000ull); ad(0x05, (u64)((bx + 30) * 16) | ((u64)(by * 16) << 16) | (100ull << 32));
        ad(0x01, 0x808040FFull | (0x3F800000ull << 32)); ad(0x02, 0x3F0000003F000000ull); ad(0x05, (u64)(bx * 16) | ((u64)((by + 25) * 16) << 16) | (100ull << 32));
        flush();
    }
    gs_in_dma--; gs_flush();
    t1 = now_ms();
    printf("4000 textured triangles (1.5 M pixels): %.2f ms per frame\n", (t1 - t0) / frames);
    {
        int n = 0;
        for (y = 0; y < 448; y++) for (x = 0; x < 640; x++) if (gs_read_pixel(PSMZ32, 0x80 * 32, 10, x, y) == 100) n++;
        printf("check: %d pixels carry the triangles' z\n", n);
    }
    return 0;
}
