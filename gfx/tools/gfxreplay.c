/* gfxreplay - replay an F12 capture headlessly and write what the TV showed.
 *   gfxreplay <capture.gcap> <out.bmp> [reference.bmp]
 * With a reference picture it prints how many pixels differ (exit code 2 if any).
 * DOTR_REPLAY_LOOPS=N replays the frame N more times and prints the time per frame (speed on real frames);
 * with DOTR_GFX_PROFILE=1 (and DOTR_GFX_THREADS=1) it also prints the hottest code, to look up in gfxreplay.map.
 * DOTR_REPLAY_PEEK=psm,bp,bw,x,y,w,h (hex) prints a GS memory rectangle at the end of the frame (raw values). */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../gfx_internal.h"
#include "../gs_fast.h"
#include "../gpu/gs_gpu.h"
#include "gfx.h"
#include "prof.h"

static int read_bmp(const char *path, hw_u32 **out, int *w, int *h)
{
    FILE *f = fopen(path, "rb");
    unsigned char hdr[54];
    int stride, y, x;
    unsigned char *row;
    if (!f || fread(hdr, 1, 54, f) != 54 || hdr[0] != 'B' || hdr[28] != 24) { if (f) fclose(f); return 0; }
    memcpy(w, hdr + 18, 4); memcpy(h, hdr + 22, 4);
    stride = (*w * 3 + 3) & ~3;
    *out = (hw_u32 *)malloc((size_t)*w * *h * 4);
    row = (unsigned char *)malloc((size_t)stride);
    for (y = *h - 1; y >= 0; y--) {
        if (fread(row, 1, (size_t)stride, f) != (size_t)stride) break;
        for (x = 0; x < *w; x++)
            (*out)[y * *w + x] = 0xFF000000u | row[x * 3 + 2] | ((hw_u32)row[x * 3 + 1] << 8) | ((hw_u32)row[x * 3] << 16);
    }
    free(row);
    fclose(f);
    return 1;
}

int main(int argc, char **argv)
{
    gfx_config cfg = { 1, 0, NULL };
    const hw_u32 *img;
    int w, h, n;
    if (argc < 3) { fprintf(stderr, "usage: gfxreplay <capture.gcap> <out.bmp> [reference.bmp]\n"); return 1; }
    gfx_test_ram = (hw_u8 *)calloc(32u << 20, 1);
    gfx_test_spr = (hw_u8 *)calloc(16384, 1);
    gfx_init(&cfg);
    if (getenv("DOTR_GPU_CHECK") && !gs_gpu_init()) { fprintf(stderr, "gpu: not available\n"); return 1; }
    n = gfx_replay_capture(argv[1]);
    if (n < 0) { fprintf(stderr, "%s: not a capture\n", argv[1]); return 1; }
    if (getenv("DOTR_GPU_CHECK")) {   /* GS memory and the TV picture on the GPU against the CPU's */
        static u32 gm[GS_MEM_WORDS];
        u32 i, bad = 0, first = 0;
        gs_flush();
        gs_gpu_read_mem(gm);
        for (i = 0; i < GS_MEM_WORDS; i++) if (gm[i] != gs_vram[i]) {
            if (!bad) first = i;
            if (bad < (getenv("DOTR_GPU_CHECK_ALL") ? 1000000u : 8u)) printf("gpu check: word %u (block %u) gpu %08x cpu %08x\n", i, i / 64, gm[i], gs_vram[i]);
            bad++;
        }
        printf("gpu check: GS memory %u of %u words differ (first at word %u)\n", bad, GS_MEM_WORDS, first);
        if (gfx_display_frame(&img, &w, &h)) {
            u32 *gp = (u32 *)malloc((size_t)w * h * 4);
            int k, pd = 0;
            gs_gpu_readout(gp, w, h);
            for (k = 0; k < w * h; k++) if (gp[k] != img[k]) pd++;
            printf("gpu check: picture %dx%d, %d pixels differ\n", w, h, pd);
            /* the texture the window presents */
            pd = 0;
            if (gs_gpu_picture(w, h) && gs_gpu_picture_read(gp, w, h)) {
                for (k = 0; k < w * h; k++) if (gp[k] != img[k]) pd++;
                printf("gpu check: window picture texture, %d pixels differ\n", pd);
            } else printf("gpu check: window picture texture not available\n");
            free(gp);
        }
    }
    if (getenv("DOTR_REPLAY_DUMP")) {   /* the whole GS memory at the end of the frame, raw (comparing runs) */
        FILE *f = fopen(getenv("DOTR_REPLAY_DUMP"), "wb");
        gs_flush();
        if (gs_gpu_only) gs_gpu_read_mem(gs_vram);   /* renderer=gpu: memory is on the GPU */
        if (f) { fwrite(gs_vram, 4, GS_MEM_WORDS, f); fclose(f); }
    }
    if (!gfx_display_frame(&img, &w, &h)) { fprintf(stderr, "display is off at the end of the frame\n"); return 1; }
    gfx_write_bmp(argv[2], img, w, h);
    if (getenv("DOTR_REPLAY_PEEK")) {
        unsigned psm, bp, bw, x0, y0, pw, ph, x, y, nz = 0;
        if (sscanf(getenv("DOTR_REPLAY_PEEK"), "%x,%x,%x,%x,%x,%x,%x", &psm, &bp, &bw, &x0, &y0, &pw, &ph) == 7) {
            for (y = y0; y < y0 + ph; y++) {
                if (y < y0 + 16) printf("peek y %3u:", y);
                for (x = x0; x < x0 + pw; x++) {
                    u32 v = gs_read_pixel(psm, bp, bw, x, y);
                    if (v) nz++;
                    if (y < y0 + 16 && x < x0 + 32) printf(psm >= 0x13 ? "%x" : " %x", v);
                }
                if (y < y0 + 16) printf("\n");
            }
            printf("peek: %u of %u values nonzero\n", nz, pw * ph);
            if (getenv("DOTR_REPLAY_PEEK_FILE")) {   /* the whole rectangle, raw u32 per pixel, row by row */
                FILE *pf = fopen(getenv("DOTR_REPLAY_PEEK_FILE"), "wb");
                for (y = y0; pf && y < y0 + ph; y++)
                    for (x = x0; x < x0 + pw; x++) { u32 v = gs_read_pixel(psm, bp, bw, x, y); fwrite(&v, 4, 1, pf); }
                if (pf) fclose(pf);
            }
        }
    }
    printf("replayed %d events, picture %dx%d -> %s\n", n, w, h, argv[2]);
    if (getenv("DOTR_REPLAY_LOOPS")) {
        int loops = atoi(getenv("DOTR_REPLAY_LOOPS")), i;
        LARGE_INTEGER f, t0, t1;
        double best = 1e9, sum = 0;
        ULONG64 c0, c1, cbest = ~0ull;   /* this thread's own CPU cycles: steadier than wall time on a busy PC */
        QueryPerformanceFrequency(&f);
        prof_start();
        for (i = 0; i < loops; i++) {
            double ms;
            QueryThreadCycleTime(GetCurrentThread(), &c0);
            QueryPerformanceCounter(&t0);
            gfx_replay_capture(argv[1]);
            if (gs_gpu_only) gs_gpu_finish();   /* the GPU's part of the frame counts too */
            QueryPerformanceCounter(&t1);
            QueryThreadCycleTime(GetCurrentThread(), &c1);
            ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart;
            sum += ms;
            if (ms < best) best = ms;
            if (c1 - c0 < cbest) cbest = c1 - c0;
        }
        prof_stop();
        if (loops > 0) printf("%d replays: best %.2f ms, average %.2f ms per frame; main thread best %.1f Mcycles\n",
                              loops, best, sum / loops, (double)cbest / 1e6);
    }
    if (getenv("DOTR_REPLAY_READOUT")) {   /* the window's per-field cost: reading the TV picture out of GS memory */
        int loops = atoi(getenv("DOTR_REPLAY_READOUT")), i, rw, rh;
        const hw_u32 *pic;
        ULONG64 c0, c1, cbest = ~0ull;
        for (i = 0; i < loops; i++) {
            gs_write_gen++;   /* as if something was drawn: a real read-out each time */
            QueryThreadCycleTime(GetCurrentThread(), &c0);
            gfx_display_frame(&pic, &rw, &rh);
            QueryThreadCycleTime(GetCurrentThread(), &c1);
            if (c1 - c0 < cbest) cbest = c1 - c0;
        }
        if (loops > 0) printf("%d picture readouts: best %.2f Mcycles each\n", loops, (double)cbest / 1e6);
    }
    if (argc > 3) {
        hw_u32 *ref; int rw, rh, i, diff = 0;
        if (!read_bmp(argv[3], &ref, &rw, &rh)) { fprintf(stderr, "cannot read %s\n", argv[3]); return 1; }
        if (rw != w || rh != h) { printf("size differs: %dx%d vs %dx%d\n", w, h, rw, rh); return 2; }
        for (i = 0; i < w * h; i++) if ((ref[i] & 0xFFFFFF) != (img[i] & 0xFFFFFF)) diff++;
        printf("%d pixels differ from %s\n", diff, argv[3]);
        return diff ? 2 : 0;
    }
    return 0;
}
