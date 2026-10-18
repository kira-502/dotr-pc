/* gfx.h - lane G extras beyond hw.h: test hooks, frame read-out, capture replay (for tests and tools). */
#ifndef DOTR_GFX_H
#define DOTR_GFX_H

#include "hw.h"

/* When set before gfx_init, EE RAM (32 MB) and the scratchpad (16 KB) are these buffers instead of the PS2
 * addresses (unit tests, the replay tool). */
extern hw_u8 *gfx_test_ram;
extern hw_u8 *gfx_test_spr;

/* The picture the TV would show now (RGBA8, alpha 255). Returns 0 if the display is off. The buffer is
 * owned by gfx and stays valid until the next call. */
int gfx_display_frame(const hw_u32 **rgba, int *w, int *h);

/* Movies: show this RGBA picture at the next vblanks instead of the GS output (NULL = back to the GS). The
 * buffer must stay valid until replaced. */
void gfx_movie_frame(const hw_u32 *rgba, int w, int h);

/* Keep the window answering (its messages) during a long wait on the game thread. */
void gfx_pump(void);

/* Write a 24-bit BMP. Returns 1 on success. */
int gfx_write_bmp(const char *path, const hw_u32 *rgba, int w, int h);

/* Replay a capture file headlessly: restores the GS/VIF1/VU1/GIF state of the frame start and feeds the recorded
 * streams. Returns the number of events replayed, or -1 if the file is not a capture. */
int gfx_replay_capture(const char *path);

/* Reports (runtime's report folders): the picture the window shows now as an RGB PNG (either renderer; a movie frame
 * when one is on). Returns 1 on success. Game thread. */
int gfx_save_picture(const char *png_path);

/* An F12-style GS capture of the next field, written when that field ends as <dir>\gs-capture.gcap (with
 * gs-capture.bmp, the picture at its end). Returns at once. */
void gfx_capture_to(const char *dir);

/* Crash/hang: the GS state only (memory, registers, VIF1, VU1, GIF; no traffic), written now as <dir>\gs-state.gcap;
 * it replays to the last picture. Crash-safe: no wait on the drawing threads, at most ~2 s for the GPU (then the CPU
 * copy is written), faults inside swallowed. Returns 1 on success. */
int gfx_capture_now(const char *dir);

/* F12 in the window calls fn (game thread, at a vblank) instead of the capture above; NULL = the plain capture. */
void gfx_set_f12_handler(void (*fn)(void));

/* Raw GS memory (4 MB) for tests. */
hw_u32 *gfx_gs_vram(void);

#endif
