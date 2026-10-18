/* pss.c - PSS demuxer: pack headers, PES packets 0xE0 (video) and 0xBD (private stream 1: PS2 audio, a 4-byte
 * sub-stream header, then an "SShd" format block and an "SSbd" data block of channel-interleaved PCM).
 * One parser for both uses: streaming (pss_stream_feed, the game) and whole files (pss_demux, the tools). */
#include <stdlib.h>
#include <string.h>
#include "pss.h"

static uint32_t be16(const uint8_t *p) { return ((uint32_t)p[0] << 8) | p[1]; }
static uint32_t le32(const uint8_t *p) { return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static int64_t pts_of(const uint8_t *b)
{
    return ((int64_t)((b[0] >> 1) & 7) << 30) | ((int64_t)b[1] << 22) | ((int64_t)(b[2] >> 1) << 15) |
           ((int64_t)b[3] << 7) | (b[4] >> 1);
}

static int grow(uint8_t **buf, uint32_t *cap, uint32_t need)
{
    if (need <= *cap) return 1;
    {
        uint32_t nc = *cap ? *cap : (1u << 20);
        uint8_t *nb;
        while (nc < need) nc *= 2;
        nb = (uint8_t *)realloc(*buf, nc);
        if (!nb) return 0;
        *buf = nb; *cap = nc;
    }
    return 1;
}

/* ---- the packet parser ----------------------------------------------------------------------------- */

static void packet(pss_stream *s, uint32_t sid, const uint8_t *body, uint32_t len)
{
    uint32_t hl;
    int64_t pts;
    const uint8_t *pl;
    uint32_t pn;
    if (sid != 0xE0 && sid != 0xBD) return;
    if (len < 3) return;
    hl = body[2];
    if (3 + hl > len) return;
    pts = (body[1] & 0x80) && hl >= 5 ? pts_of(body + 3) : -1;
    pl = body + 3 + hl;
    pn = len - 3 - hl;
    if (sid == 0xE0) {
        if (pn && s->sink.video) s->sink.video(s->sink.user, pl, pn, pts);
        return;
    }
    if (pn <= 4 || pl[0] != 0xFF || pl[1] != 0xA0) return;        /* audio sub-stream only */
    pl += 4; pn -= 4;
    if (!s->seen_shd && pn >= 40 && !memcmp(pl, "SShd", 4)) {
        pss_audio_fmt f;
        uint32_t shd = 8 + le32(pl + 4);                               /* skip the SShd block */
        memset(&f, 0, sizeof f);
        f.type = (int)le32(pl + 8);
        f.rate = (int)le32(pl + 12);
        f.channels = (int)le32(pl + 16);
        f.interleave = (int)le32(pl + 20);
        if (shd > pn) shd = pn;
        pl += shd; pn -= shd;
        if (pn >= 8 && !memcmp(pl, "SSbd", 4)) { f.data_size = le32(pl + 4); pl += 8; pn -= 8; }
        s->seen_shd = 1;
        s->audio_left = f.data_size;
        if (s->sink.audio_fmt) s->sink.audio_fmt(s->sink.user, &f, pts);
    }
    if (s->seen_shd != 1) return;                                   /* none yet, or all of it delivered */
    if (s->audio_left) {                                            /* the SSbd size: what follows is padding */
        if (pn >= s->audio_left) { pn = s->audio_left; s->seen_shd = 2; }
        else s->audio_left -= pn;
    }
    if (pn && s->sink.audio) s->sink.audio(s->sink.user, pl, pn);
}

/* Parse what is whole in d[0..n); returns the bytes used (the rest is a partial packet). */
static uint32_t parse(pss_stream *s, const uint8_t *d, uint32_t n)
{
    uint32_t i = 0;
    while (i + 6 <= n && !s->ended) {
        uint32_t sid, len;
        if (d[i] || d[i + 1] || d[i + 2] != 1) { i++; continue; }
        sid = d[i + 3];
        if (sid == 0xBA) {                                          /* pack header (MPEG-2) */
            uint32_t pl;
            if ((d[i + 4] & 0xC0) == 0x40) {
                if (i + 14 > n) break;
                pl = 14 + (d[i + 13] & 7);
            } else pl = 12;
            if (i + pl > n) break;
            i += pl;
            continue;
        }
        if (sid == 0xB9) { s->ended = 1; i += 4; break; }           /* end of stream */
        len = be16(d + i + 4);
        if (i + 6 + len > n) break;
        if (s->seen_shd == 2 && sid == 0xBD) { i += 6 + len; continue; }  /* audio past its declared size */
        packet(s, sid, d + i + 6, len);
        i += 6 + len;
    }
    return i;
}

void pss_stream_init(pss_stream *s, const pss_sink *k)
{
    memset(s, 0, sizeof *s);
    s->sink = *k;
}

int pss_stream_feed(pss_stream *s, const uint8_t *data, uint32_t n)
{
    uint32_t used;
    if (s->ended) return 0;
    if (!s->n) {                                                    /* nothing carried: parse in place */
        used = parse(s, data, n);
        if (s->ended) return 0;
        data += used; n -= used;
        if (!n) return 1;
        if (!grow(&s->buf, &s->cap, n)) return -1;
        memcpy(s->buf, data, n);
        s->n = n;
        return 1;
    }
    if (!grow(&s->buf, &s->cap, s->n + n)) return -1;
    memcpy(s->buf + s->n, data, n);
    s->n += n;
    used = parse(s, s->buf, s->n);
    memmove(s->buf, s->buf + used, s->n - used);
    s->n -= used;
    return s->ended ? 0 : 1;
}

void pss_stream_free(pss_stream *s)
{
    free(s->buf);
    memset(s, 0, sizeof *s);
}

int pss_sequence_header(const uint8_t *es, uint32_t n, int *w, int *h, double *fps)
{
    static const double rates[16] = { 0, 23.976, 24, 25, 29.97, 30, 50, 59.94, 60, 0, 0, 0, 0, 0, 0, 0 };
    uint32_t k;
    for (k = 0; k + 8 < n; k++) {
        const uint8_t *s = es + k;
        if (!s[0] && !s[1] && s[2] == 1 && s[3] == 0xB3) {
            *w = (int)((s[4] << 4) | (s[5] >> 4));
            *h = (int)(((s[5] & 15) << 8) | s[6]);
            *fps = rates[s[7] & 15];
            return 1;
        }
    }
    return 0;
}

/* ---- whole files (tools) --------------------------------------------------------------------------- */

typedef struct whole { pss_movie *m; uint32_t vcap, acap, ccap; int oom; } whole;

static void w_video(void *u, const uint8_t *es, uint32_t n, int64_t pts)
{
    whole *w = (whole *)u;
    pss_movie *m = w->m;
    if (!grow(&m->video, &w->vcap, m->video_size + n)) { w->oom = 1; return; }
    if ((m->nvchunks + 1) * sizeof(pss_chunk) > w->ccap) {
        pss_chunk *nc;
        uint32_t c = w->ccap ? w->ccap * 2 : 4096 * sizeof(pss_chunk);
        nc = (pss_chunk *)realloc(m->vchunks, c);
        if (!nc) { w->oom = 1; return; }
        m->vchunks = nc; w->ccap = c;
    }
    m->vchunks[m->nvchunks].offset = m->video_size;
    m->vchunks[m->nvchunks].size = n;
    m->vchunks[m->nvchunks].pts = pts;
    m->nvchunks++;
    memcpy(m->video + m->video_size, es, n);
    m->video_size += n;
}

static void w_fmt(void *u, const pss_audio_fmt *f, int64_t pts) { whole *w = (whole *)u; w->m->afmt = *f; w->m->first_audio_pts = pts; }

static void w_audio(void *u, const uint8_t *data, uint32_t n)
{
    whole *w = (whole *)u;
    pss_movie *m = w->m;
    if (!grow(&m->audio, &w->acap, m->audio_size + n)) { w->oom = 1; return; }
    memcpy(m->audio + m->audio_size, data, n);
    m->audio_size += n;
}

int pss_demux(const uint8_t *d, uint32_t size, pss_movie *m)
{
    whole w;
    pss_sink k;
    pss_stream s;
    memset(m, 0, sizeof *m);
    m->first_audio_pts = -1;
    memset(&w, 0, sizeof w);
    w.m = m;
    k.video = w_video; k.audio_fmt = w_fmt; k.audio = w_audio; k.user = &w;
    pss_stream_init(&s, &k);
    pss_stream_feed(&s, d, size);
    pss_stream_free(&s);
    if (w.oom) { pss_free(m); return 0; }
    pss_sequence_header(m->video, m->video_size, &m->width, &m->height, &m->fps);
    return m->video_size > 0;
}

void pss_free(pss_movie *m)
{
    free(m->video); free(m->vchunks); free(m->audio);
    memset(m, 0, sizeof *m);
}

uint32_t pss_audio_pcm(const pss_movie *m, int16_t *out, uint32_t max_frames)
{
    uint32_t il = (uint32_t)(m->afmt.interleave > 0 ? m->afmt.interleave : 0x200), pos = 0, n = 0;
    if (m->afmt.type != 1 || m->afmt.channels != 2) return 0;
    while (pos + il * 2 <= m->audio_size && n + il / 2 <= max_frames) {
        const int16_t *l = (const int16_t *)(m->audio + pos), *r = (const int16_t *)(m->audio + pos + il);
        uint32_t k;
        for (k = 0; k < il / 2; k++) { out[(n + k) * 2] = l[k]; out[(n + k) * 2 + 1] = r[k]; }
        n += il / 2;
        pos += il * 2;
    }
    return n;
}
