/*
 * sif.h - the interface between lane P (the EE side: HLE of Sony's SIF library) and lane C's sound module
 * (sound/: the IOP emulator running the disc's IRX drivers, SPU2, audio output).
 *
 * The game talks to the IOP only through SIF: it loads IRX modules from the disc, binds RPC servers
 * (KCEJEAST's sound commands via SDRPC_*, Sony's SDRDRV), calls them, and DMAs data (sound banks, music PCM)
 * into IOP memory. Lane P's HLE turns those calls into these entry points. All calls are synchronous and come
 * from the game thread. Change by agreement; log changes at the bottom.
 */
#ifndef DOTR_SIF_H
#define DOTR_SIF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- lane C implements -------------------------------------------------------------------------- */

typedef struct snd_config {
    int headless;            /* 1: no audio device (tests, headless runs) */
    const char *wav_path;    /* not NULL: also write everything played to this .wav */
} snd_config;
int  snd_init(const snd_config *cfg);    /* 1 = ok */
void snd_shutdown(void);

/* Load an IRX the game asked for (path as the game gives it, e.g. "cdrom0:\\IOP\\KCEJEAST.IRX;1"); the file's
 * bytes come from the disc (lane P reads them). Returns the module's IOP id (> 0), or < 0 when not handled
 * (lane P then answers "loaded" so the game goes on). */
int  snd_iop_load_module(const char *path, const uint8_t *irx, uint32_t size, const char *args, uint32_t args_len);

/* A bound RPC server is reachable: returns 1 when the IOP side registered server `sid`. */
int  snd_rpc_bound(uint32_t sid);

/* sceSifCallRpc, synchronous: send `ssize` bytes from EE address `send`, receive up to `rsize` bytes at EE
 * address `recv`. Returns 0 on success, < 0 when there is no such server. */
int  snd_rpc_call(uint32_t sid, uint32_t fno, uint32_t send, uint32_t ssize, uint32_t recv, uint32_t rsize);

/* sceSifSetDma from the EE into IOP memory: `size` bytes from EE address `src` to IOP address `iop_addr`. */
void snd_sif_dma_to_iop(uint32_t iop_addr, uint32_t src, uint32_t size);

/* IOP memory the EE allocates with sceSifAllocIopHeap: returns an IOP address, 0 if none. */
uint32_t snd_iop_alloc(uint32_t size);

/* Once per vblank, after the game's vblank work (IOP time slice, SPU2 mixing toward the audio device). */
void snd_vblank(void);

/* ---- change log ----------------------------------------------------------------------------------- */
/* 2026-10-17 lane P: first proposal.
 * 2026-10-17 lane C agreed, added snd_init/snd_shutdown; snd_vblank advances the IOP + SPU2 one NTSC field
 *   (800 samples at 48 kHz); snd_rpc_call returns 0 ok / <0 error, send/recv are EE addresses; the game's one
 *   server is 0x05730601 (KCEJEAST, SDRPC: 0x40 bytes out, 0x10 back); lane C takes LIBSD/SDRDRV/KCEJEAST,
 *   pad and memory-card modules stay with lane P.
 * 2026-10-17 lane P: signatures follow lane C's sound/snd.h (EE addresses as uint32_t, irx as uint8_t). When
 *   sound/ is not in the build, runtime/snd_stub.c answers: every server bound, replies zeroed. */

#ifdef __cplusplus
}
#endif

#endif
