/* movdump - headless movie check on the owner's disc: demux a PSS, decode the video with Windows' MPEG-2 decoder,
 * save some frames as PNG, the audio as WAV, and compare the timing with the stream's own timestamps.
 *   movdump <game.iso> <MOVIE/OPMV_US.PSS> <out-dir> [prefix]
 * Reads the disc; writes only into out-dir. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../pss.h"
#include "../mpeg2.h"
#include "../../sound/tools/iso9660.h"

/* ---- tiny PNG writer (stored deflate blocks) ------------------------------------------------------- */

static unsigned crc_table[256];
static unsigned crc32_of(unsigned c, const unsigned char *p, size_t n)
{
    size_t i;
    if (!crc_table[1]) {
        unsigned k, j;
        for (k = 0; k < 256; k++) { unsigned v = k; for (j = 0; j < 8; j++) v = (v & 1) ? 0xEDB88320u ^ (v >> 1) : v >> 1; crc_table[k] = v; }
    }
    c = ~c;
    for (i = 0; i < n; i++) c = crc_table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return ~c;
}
static void put_be32(FILE *f, unsigned v) { unsigned char b[4] = { (unsigned char)(v >> 24), (unsigned char)(v >> 16), (unsigned char)(v >> 8), (unsigned char)v }; fwrite(b, 1, 4, f); }
static void chunk(FILE *f, const char *type, const unsigned char *data, unsigned n)
{
    unsigned c;
    put_be32(f, n);
    fwrite(type, 1, 4, f);
    if (n) fwrite(data, 1, n, f);
    c = crc32_of(0, (const unsigned char *)type, 4);
    c = crc32_of(c, data, n);
    put_be32(f, c);
}
static int write_png(const char *path, const unsigned *rgba, int w, int h)
{
    size_t raw_n = (size_t)(w * 3 + 1) * h, blocks = (raw_n + 65534) / 65535, z_n = 2 + raw_n + blocks * 5 + 4, o = 0, i;
    unsigned char *raw = (unsigned char *)malloc(raw_n), *z = (unsigned char *)malloc(z_n), ihdr[13];
    unsigned a = 1, b = 0;
    FILE *f = fopen(path, "wb");
    int x, y;
    if (!f || !raw || !z) return 0;
    for (y = 0; y < h; y++) {
        unsigned char *r = raw + (size_t)y * (w * 3 + 1);
        r[0] = 0;
        for (x = 0; x < w; x++) { unsigned p = rgba[y * w + x]; r[1 + x * 3] = (unsigned char)p; r[2 + x * 3] = (unsigned char)(p >> 8); r[3 + x * 3] = (unsigned char)(p >> 16); }
    }
    z[o++] = 0x78; z[o++] = 0x01;
    for (i = 0; i < raw_n; i += 65535) {
        unsigned n = (unsigned)(raw_n - i > 65535 ? 65535 : raw_n - i);
        z[o++] = (unsigned char)(i + n >= raw_n); z[o++] = (unsigned char)n; z[o++] = (unsigned char)(n >> 8);
        z[o++] = (unsigned char)~n; z[o++] = (unsigned char)(~n >> 8);
        memcpy(z + o, raw + i, n); o += n;
    }
    for (i = 0; i < raw_n; i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
    z[o++] = (unsigned char)(b >> 8); z[o++] = (unsigned char)b; z[o++] = (unsigned char)(a >> 8); z[o++] = (unsigned char)a;
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    ihdr[0] = (unsigned char)(w >> 24); ihdr[1] = (unsigned char)(w >> 16); ihdr[2] = (unsigned char)(w >> 8); ihdr[3] = (unsigned char)w;
    ihdr[4] = (unsigned char)(h >> 24); ihdr[5] = (unsigned char)(h >> 16); ihdr[6] = (unsigned char)(h >> 8); ihdr[7] = (unsigned char)h;
    ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    chunk(f, "IHDR", ihdr, 13);
    chunk(f, "IDAT", z, (unsigned)o);
    chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(raw); free(z);
    return 1;
}

static void write_wav(const char *path, const short *pcm, unsigned frames, int rate)
{
    FILE *f = fopen(path, "wb");
    unsigned data = frames * 4, v;
    unsigned char h[44];
    if (!f) return;
    memcpy(h, "RIFF", 4); v = 36 + data; memcpy(h + 4, &v, 4); memcpy(h + 8, "WAVEfmt ", 8); v = 16; memcpy(h + 16, &v, 4);
    { unsigned short fmt = 1, ch = 2, bits = 16, al = 4; memcpy(h + 20, &fmt, 2); memcpy(h + 22, &ch, 2); memcpy(h + 32, &al, 2); memcpy(h + 34, &bits, 2); }
    v = (unsigned)rate; memcpy(h + 24, &v, 4); v = (unsigned)rate * 4; memcpy(h + 28, &v, 4);
    memcpy(h + 36, "data", 4); memcpy(h + 40, &data, 4);
    fwrite(h, 1, 44, f); fwrite(pcm, 4, frames, f); fclose(f);
}

/* ---- frames ---------------------------------------------------------------------------------------- */

typedef struct ctx { const char *dir, *prefix; int n; int64_t first_t, last_t; double fps; int saved; } ctx;

static void on_frame(void *user, const uint32_t *rgba, int w, int h, int64_t t)
{
    ctx *c = (ctx *)user;
    static const double at[] = { 1, 5, 15, 30, 60, 90 };
    double sec;
    int k;
    if (c->first_t < 0) c->first_t = t;
    c->last_t = t;
    sec = c->n / (c->fps > 0 ? c->fps : 29.97);
    for (k = 0; k < 6; k++) {
        if (c->saved == k && sec >= at[k]) {
            char path[600];
            snprintf(path, sizeof path, "%s\\%s frame at %02d s.png", c->dir, c->prefix, (int)at[k]);
            write_png(path, rgba, w, h);
            c->saved++;
        }
    }
    c->n++;
}

int main(int argc, char **argv)
{
    unsigned size, frames, i;
    unsigned char *data;
    pss_movie m;
    mpeg2_dec *d;
    ctx c;
    short *pcm;
    char path[600];
    const char *prefix = argc > 4 ? argv[4] : "DotR-PC movie check";
    if (argc < 4) { fprintf(stderr, "usage: movdump <game.iso> <MOVIE/OPMV_US.PSS> <out-dir> [prefix]\n"); return 1; }
    if (!iso_open(argv[1])) { fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }
    data = iso_read_file(argv[2], &size);
    if (!data) { fprintf(stderr, "%s not found on the disc\n", argv[2]); return 1; }
    if (!pss_demux(data, size, &m)) { fprintf(stderr, "not a PSS movie\n"); return 1; }
    printf("video %dx%d at %.3f fps, %u bytes in %d packets; audio type %d, %d Hz, %d ch, interleave 0x%x, %u bytes\n",
           m.width, m.height, m.fps, m.video_size, m.nvchunks, m.afmt.type, m.afmt.rate, m.afmt.channels, m.afmt.interleave, m.audio_size);
    /* audio */
    pcm = (short *)malloc((size_t)m.audio_size + 16);
    frames = pss_audio_pcm(&m, pcm, m.audio_size / 4);
    snprintf(path, sizeof path, "%s\\%s audio.wav", argv[3], prefix);
    write_wav(path, pcm, frames, m.afmt.rate ? m.afmt.rate : 48000);
    /* video */
    memset(&c, 0, sizeof c);
    c.dir = argv[3]; c.prefix = prefix; c.first_t = -1; c.fps = m.fps;
    d = mpeg2_open(m.width, m.height, m.fps, on_frame, &c);
    if (!d) { fprintf(stderr, "decoder: %s\n", mpeg2_error()); return 1; }
    for (i = 0; i < (unsigned)m.nvchunks; i++)
        if (!mpeg2_feed(d, m.video + m.vchunks[i].offset, m.vchunks[i].size, m.vchunks[i].pts)) { fprintf(stderr, "decoder: %s\n", mpeg2_error()); break; }
    mpeg2_drain(d);
    mpeg2_close(d);
    {
        double vid_s = c.n / (m.fps > 0 ? m.fps : 29.97), aud_s = (double)frames / (m.afmt.rate ? m.afmt.rate : 48000);
        int64_t last_pts = -1;
        for (i = 0; i < (unsigned)m.nvchunks; i++) if (m.vchunks[i].pts > last_pts) last_pts = m.vchunks[i].pts;
        printf("decoded %d frames = %.2f s; audio %.2f s; stream timestamps: video %.2f..%.2f s, audio starts %.2f s\n",
               c.n, vid_s, aud_s, m.vchunks[0].pts / 90000.0, last_pts / 90000.0, m.first_audio_pts / 90000.0);
        printf("decoder clock: first frame %.3f s, last frame %.3f s; %d frames saved\n", c.first_t / 1e7, c.last_t / 1e7, c.saved);
    }
    return 0;
}
