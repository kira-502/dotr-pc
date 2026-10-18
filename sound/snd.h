/* snd.h - lane G's sound entry points. The runtime's interface is hw/sif.h (agreed with lane P); this header adds
 * what the tools and tests use on top. */
#ifndef DOTR_SND_H
#define DOTR_SND_H

#include <stdint.h>
#include "sif.h"

/* For tools/tests: the last field's samples (interleaved stereo s16) and how many frames. */
const int16_t *snd_last_field(int *frames);

/* Give back a block from snd_iop_alloc (0 ok, -1 not allocated). The game frees IOP heap only in Sony's movie
 * player (main_069), which movie/ replaces, so nothing calls this yet. */
int snd_iop_free(uint32_t addr);

/* Movies: queue 48 kHz stereo PCM to be mixed over the sound output (returns frames accepted), how much is
 * still queued, and drop the queue. */
uint32_t snd_movie_pcm(const int16_t *pcm, uint32_t frames);
uint32_t snd_movie_queued(void);
void     snd_movie_stop(void);

#endif
