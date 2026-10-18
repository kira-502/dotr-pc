/* sndse - headless check of the sound effects on the owner's disc: boots the game's sound driver, loads one
 * sound group (system/system0.grp, card/card00.grp, ...) into a slot the way the game does, then plays every
 * sound in it one after another (SD_SEPlayOnlyOnce) and writes the result to a .wav.
 *   sndse <game.iso> <SOUND/SYSTEM/SYSTEM0.GRP> <slot 0..3> <out.wav> [seconds per sound]
 * Reads the disc; writes only the .wav. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../snd.h"
#include "iso9660.h"
#include "sdee.h"

int main(int argc, char **argv)
{
    snd_config cfg = { 1, NULL };
    unsigned char *grp;
    unsigned size, iopbuf, count, i, f, fields_each;
    int slot, loud_sounds = 0;
    if (argc < 5) { fprintf(stderr, "usage: sndse <game.iso> <SOUND/SYSTEM/SYSTEM0.GRP> <slot> <out.wav> [seconds]\n"); return 1; }
    if (!iso_open(argv[1])) { fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }
    {
        /* "SOUND/MONSTER/SEGRP.MRG@n": the n-th monster group, in the archive's fixed 0x80800-byte slots
         * (lSzFile_SetReadRequestParam: offset = SzDuel_Xax2New(monster) * 0x80800) */
        char path[256];
        const char *at = strchr(argv[2], '@');
        unsigned whole;
        snprintf(path, sizeof path, "%.*s", at ? (int)(at - argv[2]) : (int)strlen(argv[2]), argv[2]);
        grp = iso_read_file(path, &whole);
        if (!grp) { fprintf(stderr, "%s not found on the disc\n", path); return 1; }
        size = whole;
        if (at) {
            unsigned n = (unsigned)atoi(at + 1), off = n * 0x80800u;
            if (off >= whole) { fprintf(stderr, "slot %u is past the end of the archive\n", n); return 1; }
            grp += off;
            size = whole - off < 0x80800u ? whole - off : 0x80800u;
        }
    }
    slot = atoi(argv[3]);
    fields_each = (unsigned)((argc > 5 ? atof(argv[5]) : 1.5) * 60.0);
    memcpy(&count, grp, 4);
    cfg.wav_path = argv[4];
    snd_init(&cfg);
    if (!sdee_boot()) return 1;
    iopbuf = sdee_init_device();
    if (!sdee_load_group(iopbuf, slot, grp, size)) return 1;
    printf("%s: %u sounds in slot %d\n", argv[2], count, slot);
    for (i = 0; i < count; i++) {
        int peak = 0, frames, k;
        /* SD_SEPlayOnlyOnce(slot, id, 0) -> 0x5220 with the bytes the EE packs */
        unsigned b0 = (unsigned)slot | ((i & 0xFF) << 8) | ((i & 0xFF) << 16);
        sdee_rpc(0x5220, b0, 0, 0, 0);
        for (f = 0; f < fields_each; f++) {
            const short *pcm;
            snd_vblank();
            sdee_rpc(0x5002, 0, 0, 0, 0);
            pcm = snd_last_field(&frames);
            for (k = 0; k < frames * 2; k++) if (abs(pcm[k]) > peak) peak = abs(pcm[k]);
        }
        sdee_rpc(0x5201, (unsigned)slot, 0xFFFFFFFFu, 0, 0);   /* SD_SEStop(slot, -1): looping sounds end here */
        for (f = 0; f < 15; f++) { snd_vblank(); sdee_rpc(0x5002, 0, 0, 0, 0); }
        if (peak > 64) loud_sounds++;
        printf("  sound %2u: peak %5d\n", i, peak);
    }
    snd_shutdown();
    printf("%d of %u sounds made sound; wrote %s\n", loud_sounds, count, argv[4]);
    return 0;
}
