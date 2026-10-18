/* mpeg2.h - MPEG-2 video decoding through Windows' own decoder (Media Foundation MFT, part of Windows; no
 * download). Frames come out as RGBA8. */
#ifndef DOTR_MPEG2_H
#define DOTR_MPEG2_H

#include <stdint.h>

typedef struct mpeg2_dec mpeg2_dec;

/* Called for each decoded frame in display order: rgba is w*h pixels, pts in 100 ns units (-1 if unknown). */
typedef void (*mpeg2_frame_fn)(void *user, const uint32_t *rgba, int w, int h, int64_t pts);

mpeg2_dec *mpeg2_open(int width, int height, double fps, mpeg2_frame_fn fn, void *user);
/* Feed elementary-stream bytes; pts90 = PTS of this chunk in 90 kHz ticks, -1 if none. 1 = ok. */
int  mpeg2_feed(mpeg2_dec *d, const uint8_t *data, uint32_t size, int64_t pts90);
void mpeg2_drain(mpeg2_dec *d);   /* end of stream: deliver the remaining frames */
void mpeg2_close(mpeg2_dec *d);
const char *mpeg2_error(void);

#endif
