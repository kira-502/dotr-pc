/* capture.c - F12 frame capture and headless replay.
 *
 * A capture holds the complete graphics state at the start of one frame (GS registers + 4 MB GS memory, VIF1,
 * VU1 memories and registers, GIF paths) followed by every word the DMA delivered to VIF1 and GIF PATH3 during
 * that frame, the privileged GS register writes and the read-backs. Replaying it needs no game memory, so a
 * capture from the owner's PC reproduces the frame exactly on any build with the same state layout.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "gfx_internal.h"
#include "gfx.h"
#include "gs_fast.h"
#include "gpu/gs_gpu.h"

#define CAP_MAGIC   "DOTRGCAP"
#define CAP_VERSION 1u

enum { EV_VIF1 = 1, EV_GIF3 = 2, EV_PRIV = 3, EV_STORE = 4, EV_KICK = 5, EV_END = 6 };

static u8 *buf;
static size_t len, cap;
static int armed, recording, replaying;
static u32 field_no, rec_field, auto_from, auto_left;   /* DOTR_GFX_CAPTURE_AT=field[,count][;field[,count]...] */
static const char *auto_next;                          /* the windows not started yet */

static void put(const void *p, size_t n)
{
    if (len + n > cap) {
        size_t nc = cap ? cap * 2 : (8u << 20);
        u8 *nb;
        while (nc < len + n) nc *= 2;
        nb = (u8 *)realloc(buf, nc);
        if (!nb) { recording = 0; return; }
        buf = nb; cap = nc;
    }
    memcpy(buf + len, p, n);
    len += n;
}

static void put_u32(u32 v) { put(&v, 4); }

static void event(u32 type, const void *p, u32 n)
{
    if (!recording) return;
    put_u32(type);
    put_u32(n);
    put(p, n);
}

static void snapshot_state(void);

static void snapshot(void)
{
    gs_flush();
    if (gs_gpu_only) gs_gpu_read_mem(gs_vram);
    snapshot_state();
}

static void snapshot_state(void)
{
    len = 0;
    put(CAP_MAGIC, 8);
    put_u32(CAP_VERSION);
    put_u32((u32)sizeof gs); put(&gs, sizeof gs);
    put_u32(GS_MEM_BYTES); put(gs_vram, GS_MEM_BYTES);
    put_u32((u32)sizeof vif1); put(&vif1, sizeof vif1);
    put_u32((u32)sizeof vu1); put(&vu1, sizeof vu1);
    put_u32(gif_state_size()); put(gif_state_ptr(), gif_state_size());
}

static char to_dir[400], to_name[32];   /* gfx_capture_to / gfx_capture_now: where the next file goes */
static int no_picture;                  /* gfx_capture_now: no picture read-out (it may wait on the GPU) */

static void finish_file(void)
{
    char path[512], bmp[520];
    const char *dir = gfx_capture_dir();
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    FILE *f;
    const u32 *img; int w, h;
    put_u32(EV_END); put_u32(0);
    if (to_dir[0]) {
        snprintf(path, sizeof path, "%s\\%s.gcap", to_dir, to_name);
        to_dir[0] = 0;
    } else
    snprintf(path, sizeof path, "%s%sdotr-%04d%02d%02d-%02d%02d%02d-f%05u.gcap", dir ? dir : "", dir ? "\\" : "",
             tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec, rec_field);
    f = fopen(path, "wb");
    if (!f) { hw_log(0, "capture: cannot write %s", path); return; }
    fwrite(buf, 1, len, f);
    fclose(f);
    hw_log(2, "capture: saved %s (%u KB)", path, (unsigned)(len >> 10));
    snprintf(bmp, sizeof bmp, "%.*s.bmp", (int)(strlen(path) - 5), path);
    if (!no_picture && gfx_display_frame(&img, &w, &h)) gfx_write_bmp(bmp, img, w, h);
}

void capture_request(void) { armed = 1; }

void gfx_capture_to(const char *dir)
{
    snprintf(to_dir, sizeof to_dir, "%s", dir ? dir : "");
    snprintf(to_name, sizeof to_name, "gs-capture");
    armed = 1;
}

/* From a crash or hang report: no wait on the drawing threads (they may be what hung), at most ~2 s for the GPU
 * (its memory is left out, the CPU copy written instead, if it does not answer), and a fault inside is swallowed. */
int gfx_capture_now(const char *dir)
{
    int ok = 0;
    __try {
        recording = 0;   /* a capture in progress is dropped: its buffer becomes this one */
        snprintf(to_dir, sizeof to_dir, "%s", dir ? dir : "");
        snprintf(to_name, sizeof to_name, "gs-state");
        rec_field = field_no;
        if (gs_gpu_only && !gs_gpu_read_mem_timeout(gs_vram, 2000))
            hw_log(1, "capture: the GPU did not answer in 2 s: GS state written from the CPU copy");
        snapshot_state();
        no_picture = 1;
        finish_file();
        no_picture = 0;
        ok = 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        to_dir[0] = 0;
        no_picture = 0;
        ok = 0;
    }
    return ok;
}
int capture_recording(void) { return recording; }

void capture_begin_frame(void)
{
    if (replaying) return;
    if (field_no == 0) auto_next = getenv("DOTR_GFX_CAPTURE_AT");
    if (!auto_left && auto_next && *auto_next) {   /* next capture window: "field[,count]", windows split by ';' */
        char *end;
        auto_from = (u32)strtoul(auto_next, &end, 10);
        auto_left = *end == ',' ? (u32)strtoul(end + 1, &end, 10) : 1;
        auto_next = strchr(end, ';') ? strchr(end, ';') + 1 : NULL;
    }
    if (auto_left && field_no >= auto_from) { auto_left--; armed = 1; }
    field_no++;
    if (recording) { recording = 0; finish_file(); }
    if (armed) { armed = 0; recording = 1; rec_field = field_no - 1; snapshot(); }
}

void capture_note_kick(const hw_dma_kick *k) { event(EV_KICK, k, (u32)sizeof *k); }

void capture_note_priv(u32 addr, u64 v)
{
    u32 p[3] = { addr, (u32)v, (u32)(v >> 32) };
    event(EV_PRIV, p, sizeof p);
}

void capture_note_store(u64 bitbltbuf, u64 trxpos, u64 trxreg, u32 dst, u32 qwc)
{
    u64 p[4] = { bitbltbuf, trxpos, trxreg, (u64)dst | ((u64)qwc << 32) };
    event(EV_STORE, p, sizeof p);
}

void capture_note_stream(int kind, const u32 *words, u32 n)
{
    event(kind == 0 ? EV_VIF1 : EV_GIF3, words, n * 4);
}

/* ---- replay ---------------------------------------------------------------------------------------- */

static int get_blob(FILE *f, void *dst, u32 want)
{
    u32 n;
    if (fread(&n, 4, 1, f) != 1 || n != want) return 0;
    return fread(dst, 1, n, f) == n;
}

int gfx_replay_capture(const char *path)
{
    FILE *f = fopen(path, "rb");
    char magic[8];
    u32 ver, type, n;
    int events = 0;
    long max_ev;
    u8 *data = NULL;
    size_t dcap = 0;
    if (!f) return -1;
    if (fread(magic, 1, 8, f) != 8 || memcmp(magic, CAP_MAGIC, 8) || fread(&ver, 4, 1, f) != 1 || ver != CAP_VERSION) {
        fclose(f); return -1;
    }
    if (!gs_vram) gs_mem_init();
    if (!get_blob(f, &gs, sizeof gs) || !get_blob(f, gs_vram, GS_MEM_BYTES) || !get_blob(f, &vif1, sizeof vif1) ||
        !get_blob(f, &vu1, sizeof vu1) || !get_blob(f, gif_state_ptr(), gif_state_size())) {
        fclose(f); return -1;
    }
    gs_flush();
    gs_invalidate_all();
    gs_gpu_load_mem();
    vu1_micro_changed();
    replaying = 1;
    gs_in_dma++;
    {
        const char *e = getenv("DOTR_REPLAY_MAXEV");   /* replay only the first n events (bisecting) */
        max_ev = e ? atol(e) : -1;
    }
    while (fread(&type, 4, 1, f) == 1 && fread(&n, 4, 1, f) == 1) {
        if (type == EV_END || (max_ev >= 0 && events >= max_ev)) break;
        if (n > dcap) { u8 *nd = (u8 *)realloc(data, n); if (!nd) break; data = nd; dcap = n; }
        if (fread(data, 1, n, f) != n) break;
        if (getenv("DOTR_REPLAY_EVLOG")) hw_log(2, "replay event %d: type %u, %u bytes", events, type, n);
        switch (type) {
        case EV_VIF1: vif1_write((const u32 *)data, n / 4); break;
        case EV_GIF3: gif_write(GIF_PATH3, (const qword *)data, n / 16); break;
        case EV_PRIV: { u32 *p = (u32 *)data; gfx_gs_priv_write(p[0], p[1] | ((u64)p[2] << 32)); break; }
        case EV_STORE: {
            u64 *p = (u64 *)data;
            gs.bitbltbuf = p[0]; gs.trxpos = p[1]; gs.trxreg = p[2];
            if (gfx_test_ram) gs_local_to_host((u32)p[3], (u32)(p[3] >> 32));
            break;
        }
        default: break;
        }
        events++;
    }
    gs_in_dma--;
    gs_flush();
    replaying = 0;
    free(data);
    fclose(f);
    return events;
}
