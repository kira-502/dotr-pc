/* gfx.c - lane G entry points: init, vblank (present + capture), frame read-out, BMP writer. */
#include <stdio.h>
#include <stdlib.h>
#include "gfx_internal.h"
#include "gfx.h"
#include "display.h"
#include "gs_par.h"
#include "gs_fast.h"
#include "gpu/gs_gpu.h"
#include <string.h>

static gfx_config cfg;
int gfx_no_draw;
static const u32 *movie_rgba;   /* a movie frame shown instead of the GS output (movie/movie_play.c) */
static int movie_w, movie_h;
static u32 *frame_rgba;
static int frame_cap;
static int frame_w, frame_h, frame_ok;   /* the last picture read out, still right while nothing changed */
static u32 frame_gen;
static void (*f12_handler)(void);   /* the runtime's report key (gfx_set_f12_handler) */
void gfx_set_f12_handler(void (*fn)(void)) { f12_handler = fn; }
static u64 frame_key[7];
static unsigned frame_reused;

int gfx_init(const gfx_config *c)
{
    if (c) cfg = *c;
    gfx_no_draw = (cfg.no_draw || getenv("DOTR_GFX_NO_DRAW")) && !getenv("DOTR_GFX_CAPTURE_AT");
    gs_flush();
    gs_mem_init();
    gs_reset();
    gif_reset();
    vif1_reset();
    vu1_reset();
    gs_par_init(0);
    {   /* renderer=gpu|exact (DOTR_RENDERER; the runtime sets it from DotR-PC.ini): in a window the GPU draws unless
         * told exact; headless runs and tests use the exact renderer unless told gpu. Without a GPU that can,
         * the exact renderer draws. */
        const char *r = getenv("DOTR_RENDERER");
        int want_gpu = r ? !strcmp(r, "gpu") : !cfg.headless;
        if (want_gpu && gs_gpu_init()) {
            gs_gpu_only = 1;
            gs_gpu_load_mem();
            hw_log(2, "gfx: the GPU renderer draws");
        } else {
            hw_log(2, "gfx: the exact renderer draws");
        }
    }
    if (gs_gpu_only) display_use_device(gs_gpu_d3d_device());
    if (!cfg.headless && !display_open(cfg.window_scale ? cfg.window_scale : 2)) {
        hw_log(0, "gfx: could not open the display window");
        return 0;
    }
    hw_log(2, "gfx: ready (%s%s)", cfg.headless ? "headless" : "window", gfx_no_draw ? ", no drawing" : "");
    return 1;
}

void gfx_shutdown(void)
{
    gs_par_shutdown();
    gs_gpu_shutdown();
    if (!cfg.headless) display_close();
    free(frame_rgba);
    frame_rgba = NULL;
    frame_cap = 0;
}

hw_u32 *gfx_gs_vram(void) { return gs_vram; }

int gfx_display_frame(const hw_u32 **rgba, int *w, int *h)
{
    int fw, fh;
    u64 key[7];
    if (movie_rgba) { *rgba = movie_rgba; *w = movie_w; *h = movie_h; return 1; }   /* a movie is on the TV */
    /* nothing drawn or uploaded since the last read-out (it landed everything) and the same display settings:
     * the same picture (the game draws at 30 fps, so every other field) */
    key[0] = gs.pmode; key[1] = gs.dispfb[0]; key[2] = gs.dispfb[1]; key[3] = gs.display[0]; key[4] = gs.display[1];
    key[5] = gs.smode2; key[6] = gs.bgcolor;
    if (frame_ok && frame_gen == gs_write_gen && !memcmp(key, frame_key, sizeof key)) {
        *rgba = frame_rgba; *w = frame_w; *h = frame_h;
        frame_reused++;
        return 1;
    }
    frame_ok = 0;
    gs_flush();
    if (!gs_display_size(&fw, &fh)) return 0;
    if (fw * fh > frame_cap) {
        free(frame_rgba);
        frame_cap = fw * fh;
        frame_rgba = (u32 *)malloc((size_t)frame_cap * 4);
        if (!frame_rgba) { frame_cap = 0; return 0; }
    }
    if (gs_gpu_only) gs_gpu_readout(frame_rgba, fw, fh);
    else gs_display_read(frame_rgba, fw, fh);
    *rgba = frame_rgba; *w = fw; *h = fh;
    frame_w = fw; frame_h = fh; frame_gen = gs_write_gen; memcpy(frame_key, key, sizeof key); frame_ok = 1;
    return 1;
}

/* The GPU renderer's picture straight to the window: read out on the GPU only when something changed (as
 * gfx_display_frame decides), then copied into the back buffer. */
static void present_gpu(void)
{
    static void *tex;
    static int tw, th;
    static u32 gen;
    static u64 last[7];
    u64 key[7];
    int fw, fh;
    key[0] = gs.pmode; key[1] = gs.dispfb[0]; key[2] = gs.dispfb[1]; key[3] = gs.display[0]; key[4] = gs.display[1];
    key[5] = gs.smode2; key[6] = gs.bgcolor;
    if (tex && gen == gs_write_gen && !memcmp(key, last, sizeof key)) {
        frame_reused++;
    } else {
        tex = NULL;
        gs_flush();
        if (!gs_display_size(&fw, &fh)) return;
        tex = gs_gpu_picture(fw, fh);
        if (!tex) return;
        tw = fw; th = fh; gen = gs_write_gen; memcpy(last, key, sizeof key);
    }
    display_present_texture(tex, tw, th);
}

/* Every ~5 s in a window, one log line with the real speed: fields per second, the slowest field, and what the
 * picture read-out and Present cost per field. */
static void speed_line(double t_field_start, double t_display)
{
    static double t0, last, slowest, disp_sum, disp_max;
    static unsigned n, reused0;
    double now = display_now_ms();
    if (!t0) { t0 = last = now; reused0 = frame_reused; return; }
    if (t_field_start - last > slowest) slowest = t_field_start - last;
    last = t_field_start;
    disp_sum += t_display;
    if (t_display > disp_max) disp_max = t_display;
    n++;
    if (now - t0 >= 5000.0) {
        hw_log(2, "speed: %.1f fields/s, slowest field %.1f ms, picture+present %.2f ms per field (max %.1f), "
               "%u%% of fields reused the last picture", n * 1000.0 / (now - t0), slowest, disp_sum / n, disp_max,
               (frame_reused - reused0) * 100u / n);
        t0 = now; slowest = disp_sum = disp_max = 0; n = 0; reused0 = frame_reused;
    }
}

/* DOTR_GAME_PROFILE=<from field>[,<fields>]: sample the game thread's instruction pointer every millisecond from that
 * field on (default 600 fields) and write gameprof.txt in the working folder: hottest 16-byte buckets in dotr.dll
 * (resolve with its PDB). A measuring tool only; off unless the variable is set. */
#include <windows.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
#define GP_N (1u << 22)
static HANDLE gp_thread;
static volatile int gp_on;
static unsigned *gp_hits, gp_other, gp_from, gp_left;
static uintptr_t gp_base;
static DWORD WINAPI gp_sampler(LPVOID a)
{
    CONTEXT c;
    (void)a;
    timeBeginPeriod(1);
    while (gp_on) {
        Sleep(1);
        if (SuspendThread(gp_thread) == (DWORD)-1) continue;
        c.ContextFlags = CONTEXT_CONTROL;
        if (GetThreadContext(gp_thread, &c)) {
            uintptr_t o = ((uintptr_t)c.Eip - gp_base) >> 4;
            if (o < GP_N) gp_hits[o]++; else gp_other++;
        }
        ResumeThread(gp_thread);
    }
    return 0;
}
static void gp_dump(void)
{
    unsigned i, k, total = gp_other;
    FILE *f;
    gp_on = 0;
    Sleep(20);
    for (i = 0; i < GP_N; i++) total += gp_hits[i];
    if (!(f = fopen("gameprof.txt", "w"))) return;
    fprintf(f, "game thread profile: %u samples, dotr.dll at %08x, outside dotr.dll %.1f%%\n", total,
            (unsigned)gp_base, total ? 100.0 * gp_other / total : 0);
    for (i = k = 0; i < GP_N; i++)   /* every bucket hit, unsorted (the resolver sums them by function) */
        if (gp_hits[i]) { fprintf(f, "  %08x %6u\n", (unsigned)(gp_base + i * 16), gp_hits[i]); k++; }
    fclose(f);
}
static void gp_field(void)
{
    static int state = -1;
    if (state < 0) {
        const char *e = getenv("DOTR_GAME_PROFILE");
        state = 0;
        if (e) { gp_from = (unsigned)atoi(e); gp_left = strchr(e, ',') ? (unsigned)atoi(strchr(e, ',') + 1) : 600; state = 1; }
    }
    if (state == 1 && !gp_from--) {
        HMODULE m = NULL;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)(void *)gp_field, &m);
        gp_base = (uintptr_t)m;
        gp_hits = (unsigned *)calloc(GP_N, 4);
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &gp_thread, 0, FALSE,
                        DUPLICATE_SAME_ACCESS);
        gp_on = 1;
        CreateThread(NULL, 0, gp_sampler, NULL, 0, NULL);
        hw_log(2, "gfx: game thread profile started");
        state = 2;
    } else if (state == 2 && !--gp_left) {
        gp_dump();
        hw_log(2, "gfx: game thread profile written to gameprof.txt");
        state = 3;
    }
}

/* DOTR_GFX_TIMING=<from>,<to>: one log line at field <to> with the game thread's CPU time and its wait on GPU
 * read-backs per drawn field (a field in which something reached GS memory) since field <from>. CPU time and
 * wait, not wall time: a busy PC does not move them much (the gate's speed check). */
static void timing_field(void)
{
    static int state = -1;
    static u32 from, to, n, drawn, gen;
    static HANDLE th;
    static u64 cpu0;
    static double wait0;
    FILETIME c, e, k, u;
    if (state < 0) {
        const char *s = getenv("DOTR_GFX_TIMING");
        state = 0;
        if (s && sscanf(s, "%u,%u", &from, &to) == 2 && to > from) {
            state = 1;
            DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &th, 0, FALSE, DUPLICATE_SAME_ACCESS);
        }
    }
    if (!state || state == 3) return;
    n++;
    if (gs_write_gen != gen) { gen = gs_write_gen; if (state == 2) drawn++; }
    if (state == 1 && n >= from) {
        GetThreadTimes(th, &c, &e, &k, &u);
        cpu0 = ((u64)k.dwHighDateTime << 32 | k.dwLowDateTime) + ((u64)u.dwHighDateTime << 32 | u.dwLowDateTime);
        wait0 = gs_gpu_wait_ms;
        drawn = 0;
        state = 2;
    } else if (state == 2 && n >= to) {
        u64 cpu;
        GetThreadTimes(th, &c, &e, &k, &u);
        cpu = ((u64)k.dwHighDateTime << 32 | k.dwLowDateTime) + ((u64)u.dwHighDateTime << 32 | u.dwLowDateTime) - cpu0;
        hw_log(2, "timing: fields %u-%u, %u drawn, game thread CPU %.2f ms per drawn field, GPU wait %.2f ms per drawn field",
               from, to, drawn, drawn ? (double)cpu / 10000.0 / drawn : 0.0, drawn ? (gs_gpu_wait_ms - wait0) / drawn : 0.0);
        state = 3;
    }
}

void gfx_vblank(int field)
{
    const u32 *img;
    int w, h;
    (void)field;
    gp_field();
    {   /* DOTR_GPU_SYNC_EACH_FIELD=1 (renderer=gpu): the CPU copy of GS memory refreshed every field (bisecting) */
        static int sync = -1;
        if (sync < 0) sync = getenv("DOTR_GPU_SYNC_EACH_FIELD") != NULL;
        if (sync && gs_gpu_only) gs_gpu_read_mem(gs_vram);
    }
    {   /* DOTR_WATCH_WORD=<word>: log every field where that GS memory word changed (bisecting renderer differences) */
        static long watch = -2;
        static u32 last;
        if (watch == -2) { watch = getenv("DOTR_WATCH_WORD") ? strtol(getenv("DOTR_WATCH_WORD"), NULL, 0) : -1; last = 0xDEADBEEFu; }
        if (watch >= 0 && watch < (long)GS_MEM_WORDS) {
            gs_flush();
            if (gs_gpu_only) gs_gpu_read_mem(gs_vram);
            if (gs_vram[watch] != last) { hw_log(2, "watch: word %ld = %08x", watch, gs_vram[watch]); last = gs_vram[watch]; }
        }
    }
    timing_field();
    gs_census_field();
    vu1_stats_field();
    capture_begin_frame();
    if (gs_gpu_on) gs_gpu_stats_field();
    if (cfg.headless) return;
    {
        double t = display_now_ms();
        /* a field that was not drawn (fast-forward, runtime/threads.c) keeps the last picture on screen */
        if (!gfx_no_draw) {
            if (gs_gpu_only && !movie_rgba) present_gpu();
            else if (gfx_display_frame(&img, &w, &h)) display_present(img, w, h);
        }
        speed_line(t, display_now_ms() - t);
    }
    display_pump();
    if (display_take_f12()) { if (f12_handler) f12_handler(); else capture_request(); }
}

void gfx_pump(void) { if (!cfg.headless) display_pump(); }

void gfx_movie_frame(const hw_u32 *rgba, int w, int h) { movie_rgba = rgba; movie_w = w; movie_h = h; }

int gfx_window_focused(void) { return cfg.headless ? 0 : display_focused(); }
int gfx_quit_requested(void) { return cfg.headless ? 0 : display_quit(); }

const char *gfx_capture_dir(void) { return cfg.capture_dir; }

/* ---- reports: the picture as PNG, the F12 key ----------------------------------------------------------- */

static u32 crc_tab[256];
static u32 png_crc(const u8 *p, size_t n, u32 c)
{
    size_t i;
    if (!crc_tab[1]) {
        u32 k, j, v;
        for (k = 0; k < 256; k++) { for (v = k, j = 0; j < 8; j++) v = (v & 1) ? 0xEDB88320u ^ (v >> 1) : v >> 1; crc_tab[k] = v; }
    }
    for (i = 0; i < n; i++) c = crc_tab[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c;
}

static void png_chunk(FILE *f, const char *type, const u8 *data, u32 n)
{
    u8 b[4];
    u32 c;
    b[0] = (u8)(n >> 24); b[1] = (u8)(n >> 16); b[2] = (u8)(n >> 8); b[3] = (u8)n;
    fwrite(b, 1, 4, f);
    fwrite(type, 1, 4, f);
    if (n) fwrite(data, 1, n, f);
    c = png_crc((const u8 *)type, 4, 0xFFFFFFFFu);
    c = png_crc(data, n, c) ^ 0xFFFFFFFFu;
    b[0] = (u8)(c >> 24); b[1] = (u8)(c >> 16); b[2] = (u8)(c >> 8); b[3] = (u8)c;
    fwrite(b, 1, 4, f);
}

/* An RGB PNG with stored (uncompressed) deflate blocks: no compressor needed, ~1 MB for 640x448. */
static int write_png(const char *path, const u32 *rgba, int w, int h)
{
    static const u8 sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    size_t row = (size_t)w * 3 + 1, raw_n = row * (size_t)h, nblk = (raw_n + 65534) / 65535, z_n = 2 + raw_n + nblk * 5 + 4;
    u8 *raw = (u8 *)malloc(raw_n), *z = (u8 *)malloc(z_n), ihdr[13];
    size_t i, o = 0, done = 0;
    u32 a = 1, b = 0;
    int x, y, ok = 0;
    FILE *f;
    if (!raw || !z) { free(raw); free(z); return 0; }
    for (y = 0; y < h; y++) {
        u8 *r = raw + row * (size_t)y;
        r[0] = 0;   /* filter: none */
        for (x = 0; x < w; x++) {
            u32 c = rgba[(size_t)y * w + x];
            r[1 + x * 3] = (u8)c; r[2 + x * 3] = (u8)(c >> 8); r[3 + x * 3] = (u8)(c >> 16);
        }
    }
    z[o++] = 0x78; z[o++] = 0x01;
    while (done < raw_n) {
        u32 len = (u32)(raw_n - done > 65535 ? 65535 : raw_n - done);
        z[o++] = (u8)(done + len == raw_n);
        z[o++] = (u8)len; z[o++] = (u8)(len >> 8); z[o++] = (u8)~len; z[o++] = (u8)(~len >> 8);
        memcpy(z + o, raw + done, len); o += len; done += len;
    }
    for (i = 0; i < raw_n; i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
    z[o++] = (u8)(b >> 8); z[o++] = (u8)b; z[o++] = (u8)(a >> 8); z[o++] = (u8)a;
    ihdr[0] = (u8)(w >> 24); ihdr[1] = (u8)(w >> 16); ihdr[2] = (u8)(w >> 8); ihdr[3] = (u8)w;
    ihdr[4] = (u8)(h >> 24); ihdr[5] = (u8)(h >> 16); ihdr[6] = (u8)(h >> 8); ihdr[7] = (u8)h;
    ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;   /* 8-bit RGB, no interlace */
    f = fopen(path, "wb");
    if (f) {
        fwrite(sig, 1, 8, f);
        png_chunk(f, "IHDR", ihdr, 13);
        png_chunk(f, "IDAT", z, (u32)o);
        png_chunk(f, "IEND", NULL, 0);
        ok = !ferror(f);
        fclose(f);
    }
    free(raw); free(z);
    return ok;
}

int gfx_save_picture(const char *png_path)
{
    const u32 *img;
    int w, h;
    if (!gfx_display_frame(&img, &w, &h) || w <= 0 || h <= 0) return 0;
    return write_png(png_path, img, w, h);
}

int gfx_write_bmp(const char *path, const hw_u32 *rgba, int w, int h)
{
    FILE *f = fopen(path, "wb");
    int stride = (w * 3 + 3) & ~3, y, x;
    u8 hdr[54] = { 'B', 'M' };
    u8 *row;
    u32 size = 54u + (u32)(stride * h);
    if (!f) return 0;
    memcpy(hdr + 2, &size, 4);
    hdr[10] = 54; hdr[14] = 40;
    memcpy(hdr + 18, &w, 4);
    memcpy(hdr + 22, &h, 4);
    hdr[26] = 1; hdr[28] = 24;
    fwrite(hdr, 1, 54, f);
    row = (u8 *)calloc(1, (size_t)stride);
    for (y = h - 1; y >= 0; y--) {
        for (x = 0; x < w; x++) {
            u32 p = rgba[y * w + x];
            row[x * 3 + 0] = (u8)(p >> 16);
            row[x * 3 + 1] = (u8)(p >> 8);
            row[x * 3 + 2] = (u8)p;
        }
        fwrite(row, 1, (size_t)stride, f);
    }
    free(row);
    fclose(f);
    return 1;
}
