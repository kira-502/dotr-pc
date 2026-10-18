/* movie_run.h - play one PSS movie at the game's pace (two vblanks per video frame), with the original player's
 * skip rule. The file is streamed: a worker thread reads it in pieces, demuxes and decodes a few frames ahead,
 * so nothing big is read or decoded on the game thread. The host supplies the clock, the pad, the screen, the
 * speakers and the file. */
#ifndef DOTR_MOVIE_RUN_H
#define DOTR_MOVIE_RUN_H

#include <stdint.h>

typedef struct movie_host {
    void (*wait_vblank)(void *user);                                 /* returns after the next vblank */
    uint32_t (*pad)(void *user);                                     /* SzPad_VSync + SzPad_Check(0,1,0xFFFF) */
    void (*show)(void *user, const uint32_t *rgba, int w, int h);    /* NULL rgba = movie over; rgba stays valid
                                                                        until the next show */
    uint32_t (*audio)(void *user, const int16_t *pcm, uint32_t frames); /* frames accepted */
    uint32_t (*audio_queued)(void *user);
    void (*idle)(void *user);                                        /* while waiting on the worker (may be NULL) */
    uint32_t (*fields)(void *user);                                  /* fields since boot (NULL: count the waits) */
    void *user;
} movie_host;

/* The file: its size and a reader callable from any thread (1 = ok). Reads start at a 2048-byte aligned offset
 * and are whole sectors; the last one may run past size (dst has room for it). */
typedef struct movie_source {
    int (*read)(void *user, uint32_t offset, uint32_t size, void *dst);
    void *user;
    uint32_t size;
} movie_source;

/* Returns the last pad value (what the game's movie() returned); -1 when the data is not a movie or Windows'
 * decoder is missing. Skips like the original: newly pressed buttons in mask 0x8E0, after 10 frames, only when
 * with_inter. */
int movie_play_source(const movie_source *src, int with_audio, int with_inter, const movie_host *h);
/* What the last movie_play_source did (for the log). */
typedef struct movie_stats {
    int frames, skipped, stalls;     /* frames shown, skipped by a button, times the picture waited on the worker */
    int late;                        /* frames shown at once because the field clock was already past them */
    double first_ms, stall_ms;       /* wait for the first frame; longest wait after it */
    uint32_t audio_frames;           /* sound frames handed to the host */
} movie_stats;
extern movie_stats movie_last;

/* The same from a file in memory (tools). */
int movie_run(const uint8_t *pss, uint32_t size, int with_audio, int with_inter, const movie_host *h);

#endif
