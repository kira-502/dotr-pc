/* movie_play.c - replaces the game's movie_play (main_068.c; Sony's sample MPEG player on the IPU) in the PC
 * port: the same arguments and return value, played by movie_run.c through gfx and sound. The file is streamed
 * from the disc by movie_run's worker thread; the game thread never reads or decodes it. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "movie_run.h"
#include "gfx.h"
#include "snd.h"
#include "hw.h"

/* from the runtime (lane P) and the game itself */
extern int rt_disc_find(const char *path, unsigned int *lba, unsigned int *size);
extern int rt_disc_read(unsigned int lba, unsigned int sectors, void *dst);
extern int sceGsSyncV(int mode);
extern void SzPad_Status(void);
extern int SzPad_VSync(void);
extern void SzPad_Act(void);
extern int SzPad_Check(int port, int mode, int mask);

static void host_wait(void *u) { (void)u; sceGsSyncV(0); }
static uint32_t host_pad(void *u) { (void)u; SzPad_Status(); SzPad_VSync(); SzPad_Act(); return (uint32_t)SzPad_Check(0, 1, 0xFFFF); }
static void host_show(void *u, const uint32_t *rgba, int w, int h) { (void)u; gfx_movie_frame(rgba, w, h); }
static uint32_t host_audio(void *u, const int16_t *pcm, uint32_t n) { (void)u; return snd_movie_pcm(pcm, n); }
static uint32_t host_queued(void *u) { (void)u; return snd_movie_queued(); }
/* waiting on the worker (the first frame): keep the window answering, run no fields (a scripted run's frame
 * numbers stay the same) */
static void host_idle(void *u) { (void)u; gfx_pump(); }
static uint32_t host_fields(void *u) { (void)u; return hw_vblank_count(); }

/* The worker reads the disc while the game thread waits in this player. SzGame_PlayMovie (main_037.c) finishes
 * every file request and stops the sound system before calling movie_play, so nothing else reads the disc
 * meanwhile. */
static int disc_read(void *user, uint32_t off, uint32_t size, void *dst)
{
    unsigned int lba = *(unsigned int *)user;
    return rt_disc_read(lba + off / 2048u, size / 2048u, dst);
}

int movie_play(const char *path, int with_audio, int with_inter)
{
    movie_host h = { host_wait, host_pad, host_show, host_audio, host_queued, host_idle, host_fields, NULL };
    movie_source s;
    const char *p = strchr(path, ':');
    unsigned int lba, size;
    int r;
    char alt[64];
    /* checks only: DOTR_MOVIE_FILE=EDMV_A5 plays that movie wherever the game plays one (the endings and DUMMY
     * are otherwise reached only at the end of the story, or never) */
    if (getenv("DOTR_MOVIE_FILE")) {
        snprintf(alt, sizeof alt, "\\MOVIE\\%s.PSS;1", getenv("DOTR_MOVIE_FILE"));
        path = alt; p = NULL;
    }
    if (!rt_disc_find(p ? p + 1 : path, &lba, &size)) { hw_log(1, "movie: %s not on the disc", path); return 0; }
    s.read = disc_read; s.user = &lba; s.size = size;
    r = movie_play_source(&s, with_audio, with_inter, &h);
    /* integers only: the C runtime's float formatting masks the game thread's EE float traps */
    hw_log(r < 0 ? 0 : 2, "movie: %s %s: %d frames%s, first frame after %d ms, %d waits (longest %d ms), %d late, %u.%u s of sound",
           path, r < 0 ? "did not play" : "played", movie_last.frames, movie_last.skipped ? " (skipped)" : "",
           (int)movie_last.first_ms, movie_last.stalls, (int)movie_last.stall_ms, movie_last.late,
           movie_last.audio_frames / 48000u, movie_last.audio_frames % 48000u / 4800u);
    snd_movie_stop();
    gfx_movie_frame(NULL, 0, 0);
    return r < 0 ? 0 : r;
}
