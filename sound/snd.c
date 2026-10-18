/* snd.c - sound entry points: module loading, RPC, SIF DMA, the per-field tick that runs the IOP and the SPU2,
 * WAV recording and (when not headless) playback through XAudio2 (snd_out.c). */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include "iop.h"
#include "snd.h"
#include "hw.h"

#define FIELD_FRAMES 800      /* 48000 Hz / 60 fields */

static snd_config cfg;
static FILE *wav;
static u32 wav_bytes;
static s16 field[FIELD_FRAMES * 2];
static int loaded_kce;

/* movie audio (48 kHz stereo) mixed over the SPU2 output, 800 frames per field */
#define MOVIE_RING (48000 * 4)
static s16 movie_ring[MOVIE_RING * 2];
static u32 movie_rd, movie_wr;

uint32_t snd_movie_pcm(const int16_t *pcm, uint32_t frames)
{
    u32 n = 0;
    while (n < frames && movie_wr - movie_rd < MOVIE_RING) {
        u32 k = movie_wr % MOVIE_RING;
        movie_ring[k * 2] = pcm[n * 2]; movie_ring[k * 2 + 1] = pcm[n * 2 + 1];
        movie_wr++; n++;
    }
    return n;
}

uint32_t snd_movie_queued(void) { return movie_wr - movie_rd; }
void snd_movie_stop(void) { movie_rd = movie_wr = 0; }

int snd_out_open(void);
void snd_out_close(void);
void snd_out_push(const s16 *pcm, int frames);

void iop_log(int level, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    hw_log(level, "%s", buf);
}

static void wav_header(FILE *f, u32 data)
{
    u8 h[44];
    u32 v;
    memcpy(h, "RIFF", 4); v = 36 + data; memcpy(h + 4, &v, 4); memcpy(h + 8, "WAVEfmt ", 8);
    v = 16; memcpy(h + 16, &v, 4);
    { u16 fmt = 1, ch = 2, bits = 16, align = 4; memcpy(h + 20, &fmt, 2); memcpy(h + 22, &ch, 2); memcpy(h + 32, &align, 2); memcpy(h + 34, &bits, 2); }
    v = 48000; memcpy(h + 24, &v, 4); v = 48000 * 4; memcpy(h + 28, &v, 4);
    memcpy(h + 36, "data", 4); memcpy(h + 40, &data, 4);
    fseek(f, 0, SEEK_SET);
    fwrite(h, 1, 44, f);
    fseek(f, 0, SEEK_END);
}

int snd_init(const snd_config *c)
{
    if (c) cfg = *c;
    iop_cpu_reset();
    iop_hw_reset();
    iop_hle_reset();
    spu2_reset();
    loaded_kce = 0;
    { extern u32 iop_trace_pc; const char *t = getenv("DOTR_IOP_TRACE"); if (t) iop_trace_pc = (u32)strtoul(t, NULL, 16); }
    if (cfg.wav_path) {
        wav = fopen(cfg.wav_path, "wb");
        if (wav) { wav_bytes = 0; wav_header(wav, 0); }
        else hw_log(1, "sound: cannot write %s", cfg.wav_path);
    }
    if (!cfg.headless && !snd_out_open()) hw_log(1, "sound: no audio device; running silent");
    return 1;
}

void snd_shutdown(void)
{
    if (getenv("DOTR_SND_TRACE")) {
        int c;
        for (c = 0; c < 2; c++) {
            u32 b = 0x1F900000u + (u32)c * 0x400, x = 0x1F900760u + (u32)c * 0x28;
            iop_log(2, "core%d MMIX=%04x ATTR=%04x ADMAS=%04x MVOL=%04x/%04x EVOL=%04x/%04x AVOL=%04x/%04x BVOL=%04x/%04x",
                    c, spu2_read(b + 0x198), spu2_read(b + 0x19A), spu2_read(b + 0x1B0), spu2_read(x), spu2_read(x + 2),
                    spu2_read(x + 4), spu2_read(x + 6), spu2_read(x + 8), spu2_read(x + 0xA), spu2_read(x + 0xC), spu2_read(x + 0xE));
        }
    }
    { extern u32 spu2_adma_short; if (spu2_adma_short) hw_log(1, "sound: the music stream ran dry %u times (256-sample blocks)", spu2_adma_short); }
    if (!cfg.headless) snd_out_close();
    if (wav) { wav_header(wav, wav_bytes); fclose(wav); wav = NULL; }
}

int snd_iop_load_module(const char *path, const uint8_t *irx, uint32_t size, const char *args, uint32_t args_len)
{
    const char *base = path;
    const char *p;
    for (p = path; *p; p++) if (*p == '\\' || *p == '/' || *p == ':') base = p + 1;
    /* the game's sound driver; LIBSD/SDRDRV are loaded by the game but never called (KCEJEAST has its own
     * SPU library), so they are accepted without running */
    if (!_strnicmp(base, "KCEJEAST.IRX", 12)) {
        if (iop_load_irx(irx, size, args, args_len) < 0) return -2;
        loaded_kce = 1;
        return 1;
    }
    if (!_strnicmp(base, "LIBSD.IRX", 9) || !_strnicmp(base, "SDRDRV.IRX", 10)) return 2;
    return -1;
}

int snd_rpc_bound(uint32_t sid) { return iop_rpc_find(sid, NULL); }

int snd_rpc_call(uint32_t sid, uint32_t fno, uint32_t send, uint32_t ssize, uint32_t recv, uint32_t rsize)
{
    u32 buf, reply;
    if (!iop_rpc_find(sid, &buf)) return -1;
    if (send && ssize) memcpy(iop_ptr(buf), PS2_PTR(send), ssize);
    if (getenv("DOTR_SND_TRACE") && fno != 0x5002)
        iop_log(2, "rpc %04x buf=%06x args %x %x %x %x", fno, buf, iop_read32(buf + 4), iop_read32(buf + 8),
                iop_read32(buf + 12), iop_read32(buf + 16));
    reply = iop_rpc_call(sid, fno, ssize);
    if (recv && rsize) {
        if (reply) memcpy(PS2_PTR(recv), iop_ptr(reply), rsize);
        else memset(PS2_PTR(recv), 0, rsize);
    }
    return 0;
}

void snd_sif_dma_to_iop(uint32_t iop_addr, uint32_t src, uint32_t size)
{
    if ((iop_addr & (IOP_RAM_SIZE - 1)) + size > IOP_RAM_SIZE) { hw_log(0, "sound: SIF DMA past IOP RAM"); return; }
    memcpy(iop_ptr(iop_addr), PS2_PTR(src), size);
}

uint32_t snd_iop_alloc(uint32_t size) { return iop_alloc(size); }

int snd_iop_free(uint32_t addr) { return iop_free(addr); }

void snd_vblank(void)
{
    int i;
    iop_vblank();
    /* render in small slices so DMA-complete interrupts re-arm AutoDMA inside the field, as on hardware */
    for (i = 0; i < FIELD_FRAMES; i += 32) {
        spu2_render(field + i * 2, 32);
        iop_hw_tick();
    }
    for (i = 0; i < FIELD_FRAMES && movie_rd != movie_wr; i++, movie_rd++) {
        u32 k = movie_rd % MOVIE_RING;
        s32 l = field[i * 2] + movie_ring[k * 2], r = field[i * 2 + 1] + movie_ring[k * 2 + 1];
        field[i * 2] = (s16)(l > 32767 ? 32767 : l < -32768 ? -32768 : l);
        field[i * 2 + 1] = (s16)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
    }
    if (wav) { fwrite(field, 4, FIELD_FRAMES, wav); wav_bytes += FIELD_FRAMES * 4; }
    if (!cfg.headless) snd_out_push(field, FIELD_FRAMES);
}

const int16_t *snd_last_field(int *frames) { *frames = FIELD_FRAMES; return field; }
