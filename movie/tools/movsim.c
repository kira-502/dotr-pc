/* movsim - run the in-game movie loop (movie_run) headless with a simulated 60 Hz clock, pad and sound queue:
 * checks pacing (vblanks per frame), that the sound never runs dry, and the original skip rule.
 *   movsim <game.iso> <MOVIE/OPMV_US.PSS> [skip-at-field]
 * Reads the disc; writes nothing. */
#include <stdio.h>
#include <stdlib.h>
#include "../movie_run.h"
#include "../../sound/tools/iso9660.h"

typedef struct sim { unsigned vblanks, frames, queued, dry_fields, skip_at; int over; } sim;

static void s_wait(void *u)
{
    sim *s = (sim *)u;
    s->vblanks++;
    if (s->frames > 2 && !s->over) {               /* the sound device takes 800 frames per field */
        if (s->queued < 800) s->dry_fields++;
        s->queued = s->queued > 800 ? s->queued - 800 : 0;
    }
}
static uint32_t s_pad(void *u) { sim *s = (sim *)u; return (s->skip_at && s->vblanks >= s->skip_at) ? 0x20 : 0; }
static void s_show(void *u, const uint32_t *rgba, int w, int h) { sim *s = (sim *)u; (void)w; (void)h; if (rgba) s->frames++; else s->over = 1; }
static uint32_t s_audio(void *u, const int16_t *pcm, uint32_t n) { sim *s = (sim *)u; (void)pcm; s->queued += n; return n; }
static uint32_t s_queued(void *u) { return ((sim *)u)->queued; }

int main(int argc, char **argv)
{
    unsigned size;
    unsigned char *data;
    sim s = { 0 };
    movie_host h = { s_wait, s_pad, s_show, s_audio, s_queued, NULL, NULL, &s };
    int r;
    if (argc < 3) { fprintf(stderr, "usage: movsim <game.iso> <MOVIE/x.PSS> [skip-at-field]\n"); return 1; }
    if (!iso_open(argv[1])) return 1;
    data = iso_read_file(argv[2], &size);
    if (!data) { fprintf(stderr, "not found\n"); return 1; }
    s.skip_at = argc > 3 ? (unsigned)atoi(argv[3]) : 0;
    r = movie_run(data, size, 1, 1, &h);
    printf("%s: %u frames shown over %u fields (%.2f fields/frame, %.2f s at 60 Hz), sound dry in %u fields, returned %d\n",
           argv[2], s.frames, s.vblanks, s.frames ? (double)s.vblanks / s.frames : 0, s.vblanks / 59.94, s.dry_fields, r);
    return 0;
}
