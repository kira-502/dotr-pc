/* sndbgm - headless check of the sound path on the owner's disc: loads IOP/KCEJEAST.IRX into the emulated IOP,
 * then does what the game's EE code does to play background music (SD_InitializeDevice, then the AutoDMA
 * streaming of SD_vAutoDMASequentialWatchDog / _iDecodeLPCM) and writes the SPU2 output to a .wav.
 *   sndbgm <game.iso> <SOUND/BGM/00/0001.PCM> <seconds> <out.wav>
 * Reads the disc; writes only the .wav. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../snd.h"

#include "iso9660.h"

/* ---- the EE side of the protocol ------------------------------------------------------------------- */

#define SID 0x05730601u
static __declspec(align(16)) unsigned int sendbuf[16], recvbuf[4];

static unsigned rpc(unsigned cmd, unsigned a1, unsigned a2, unsigned a3)
{
    memset(sendbuf, 0, sizeof sendbuf);
    sendbuf[0] = (unsigned)(uintptr_t)sendbuf;
    sendbuf[1] = a1; sendbuf[2] = a2; sendbuf[3] = a3;
    memset(recvbuf, 0, sizeof recvbuf);
    snd_rpc_call(SID, cmd, (unsigned)(uintptr_t)sendbuf, 0x40, (unsigned)(uintptr_t)recvbuf, 0x10);
    return recvbuf[0];
}

/* _iDecodeLPCM: 16384 stereo frames at 24 kHz -> one 0x20000-byte AutoDMA block (512 B left, 512 B right) */
static void decode_block(const short *src, unsigned frames_avail, unsigned *pos, short *dst)
{
    int row, col;
    for (row = 0; row < 128; row++) {
        short *l = dst + row * 512, *r = l + 256;
        for (col = 0; col < 128; col++) {
            short sl = 0, sr = 0;
            if (*pos < frames_avail) { sl = src[*pos * 2]; sr = src[*pos * 2 + 1]; }
            (*pos)++;
            l[col * 2] = l[col * 2 + 1] = sl;
            r[col * 2] = r[col * 2 + 1] = sr;
        }
    }
}

void hw_log(int level, const char *fmt, ...);

int main(int argc, char **argv)
{
    snd_config cfg = { 1, NULL };
    unsigned char *irx, *pcm;
    unsigned irx_size, pcm_size, iopbuf, pos = 0, frames, half = 0, last_flag = 0, f, fields;
    static __declspec(align(16)) short block[0x10000];
    if (argc < 5) { fprintf(stderr, "usage: sndbgm <game.iso> <SOUND/BGM/xx/yyyy.PCM> <seconds> <out.wav>\n"); return 1; }
    if (!iso_open(argv[1])) { fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }
    irx = iso_read_file("IOP/KCEJEAST.IRX", &irx_size);
    pcm = iso_read_file(argv[2], &pcm_size);
    if (!irx || !pcm) { fprintf(stderr, "file not found on the disc\n"); return 1; }
    frames = pcm_size / 4;
    cfg.wav_path = argv[4];
    snd_init(&cfg);
    if (snd_iop_load_module("cdrom0:\\IOP\\KCEJEAST.IRX;1", irx, irx_size, NULL, 0) < 0) { fprintf(stderr, "KCEJEAST did not load\n"); return 1; }
    if (!snd_rpc_bound(SID)) { fprintf(stderr, "the sound driver registered no RPC server\n"); return 1; }
    /* SD_InitializeDevice */
    rpc(0xF005, 0, 0, 0);
    iopbuf = rpc(0xF002, 0xC1800, 0, 0);
    printf("IOP sound buffer at %06x\n", iopbuf);
    {
        static const unsigned bank[12] = { 0x5010, 0x9F810, 0x199810, 0x1CB810, 0x1FD810, 0x8F810, 0x9F810, 0xAF810,
                                           0xBF810, 0xD7810, 0xEF810, 0x107810 };
        unsigned i;
        for (i = 0; i < 12; i++) rpc(0x5F10, i, bank[i], 0);
    }
    rpc(0x5F12, 0x150000, 0x38000, 0);
    rpc(0x5000, 0, 0, 0);
    /* first two blocks, then start the stream (SD_iStartAutoDMA) */
    decode_block((const short *)pcm, frames, &pos, block);
    snd_sif_dma_to_iop(iopbuf, (unsigned)(uintptr_t)block, 0x20000);
    decode_block((const short *)pcm, frames, &pos, block);
    snd_sif_dma_to_iop(iopbuf + 0x20000, (unsigned)(uintptr_t)block, 0x20000);
    rpc(0x5005, 0x80, 0x80, 0);         /* SD_BGMSetSourceLevel(0x80, 0x80), as the game does */
    rpc(0x1020, iopbuf, 0x40000, 1);
    fields = (unsigned)(atof(argv[3]) * 60.0);
    for (f = 0; f < fields; f++) {
        unsigned st;
        snd_vblank();
        rpc(0x5002, 0, 0, 0);
        st = recvbuf[2];
        if (st && ((st ^ last_flag) & 0x01000000u)) {
            last_flag = st;
            decode_block((const short *)pcm, frames, &pos, block);
            snd_sif_dma_to_iop(iopbuf + half * 0x20000, (unsigned)(uintptr_t)block, 0x20000);
            half ^= 1;
        }
        if (f % 60 == 0) {
            extern unsigned spu2_adma_short;
            printf("  field %u: status %08x, %u of %u source frames sent, %u stream gaps\n", f, st, pos, frames, spu2_adma_short);
        }
    }
    snd_shutdown();
    printf("wrote %s (%u fields)\n", argv[4], fields);
    return 0;
}
