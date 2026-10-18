/* pss.h - PS2 PSS movies (an MPEG-2 program stream): split into the MPEG-2 video stream and the PCM audio. */
#ifndef DOTR_PSS_H
#define DOTR_PSS_H

#include <stdint.h>

typedef struct pss_chunk { uint32_t offset, size; int64_t pts; } pss_chunk;   /* pts in 90 kHz ticks, -1 = none */

typedef struct pss_audio_fmt {
    int type;          /* 1 = 16-bit PCM */
    int rate, channels;
    int interleave;    /* bytes per channel block */
    uint32_t data_size;
} pss_audio_fmt;

typedef struct pss_movie {
    uint8_t *video; uint32_t video_size;          /* concatenated video elementary stream */
    pss_chunk *vchunks; int nvchunks;              /* where each PES payload landed, with its PTS */
    uint8_t *audio; uint32_t audio_size;           /* audio stream bytes after the SShd/SSbd headers */
    pss_audio_fmt afmt;
    int64_t first_audio_pts;
    int width, height;
    double fps;                                    /* from the sequence header */
} pss_movie;

/* Streaming demux: the file is fed in pieces of any size and each packet is handed out as soon as it is whole. */
typedef struct pss_sink {
    void (*video)(void *user, const uint8_t *es, uint32_t n, int64_t pts);        /* one PES payload */
    void (*audio_fmt)(void *user, const pss_audio_fmt *f, int64_t pts);            /* once, from SShd */
    void (*audio)(void *user, const uint8_t *data, uint32_t n);                    /* channel-interleaved PCM */
    void *user;
} pss_sink;

typedef struct pss_stream {
    pss_sink sink;
    uint8_t *buf; uint32_t n, cap;                 /* unparsed bytes (at most one partial packet + a new piece) */
    int seen_shd;                                  /* 0 no SShd yet, 1 audio flowing, 2 SSbd size reached */
    int ended;
    uint32_t audio_left;                           /* SSbd data size still to come (0 = unknown) */
} pss_stream;

void pss_stream_init(pss_stream *s, const pss_sink *k);
/* Returns 0 once the end code is reached (later pieces are ignored), -1 out of memory, 1 otherwise. */
int  pss_stream_feed(pss_stream *s, const uint8_t *data, uint32_t n);
void pss_stream_free(pss_stream *s);
/* Size and frame rate from a sequence header in the first bytes of the video stream; 1 when found. */
int  pss_sequence_header(const uint8_t *es, uint32_t n, int *w, int *h, double *fps);

/* Demux a whole PSS file in memory (tools). Returns 1 on success. */
int  pss_demux(const uint8_t *data, uint32_t size, pss_movie *m);
void pss_free(pss_movie *m);
/* Interleaved stereo s16 from the audio blocks: frames written, at most max_frames. */
uint32_t pss_audio_pcm(const pss_movie *m, int16_t *out, uint32_t max_frames);

#endif
