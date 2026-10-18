/* movie_run.c - movie playback: a worker thread reads the file in pieces, demuxes it and decodes the video a few
 * frames ahead into a small ring; the game thread shows one frame per two vblanks, keeps the sound queue a little
 * ahead of the picture, and skips on a newly pressed button as the original. Memory use is a few frames and
 * the sound demuxed ahead of the picture, whatever the file size. */
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include "movie_run.h"
#include "pss.h"
#include "mpeg2.h"

#define NSLOT 8                 /* decoded frames ahead (one is the frame on screen) */
#define CHUNK (256u * 1024u)    /* bytes per read; a multiple of the 2048-byte sector */
#define AHEAD (4u << 20)        /* video bytes demuxed ahead of the decoder: the movies mux their sound up to
                                   a second or so after the picture, so the reader runs well ahead of the
                                   pictures to have each frame's sound in time (~3 s at the opening's rate) */

typedef struct vpkt { struct vpkt *next; uint32_t n; int64_t pts; uint8_t data[1]; } vpkt;

typedef struct mstream {
    movie_source src;
    int with_audio;
    HANDLE thread;
    CRITICAL_SECTION cs;
    CONDITION_VARIABLE cv;
    volatile LONG stop;
    int done, failed;
    /* worker only */
    pss_stream ps;
    mpeg2_dec *dec;
    vpkt *vq_head, *vq_tail; uint32_t vq_bytes;        /* demuxed video waiting for the decoder */
    uint8_t ablk[0x2000]; uint32_t ablk_n, il;
    /* frames: slots [freed, wr) are in use, rd is the next one to show */
    uint32_t *slot[NSLOT];
    int sw[NSLOT], sh[NSLOT];
    unsigned wr, rd, freed;
    /* sound: stereo frames [pcm_rd, pcm_n) not yet handed to the host */
    int16_t *pcm; uint32_t pcm_n, pcm_cap, pcm_rd;
    int audio_ok;
    int64_t apts, vpts;
} mstream;

/* ---- worker ---------------------------------------------------------------------------------------- */

static void on_frame(void *user, const uint32_t *rgba, int w, int h, int64_t pts)
{
    mstream *m = (mstream *)user;
    unsigned k;
    (void)pts;
    EnterCriticalSection(&m->cs);
    while (m->wr - m->freed >= NSLOT && !m->stop) SleepConditionVariableCS(&m->cv, &m->cs, INFINITE);
    LeaveCriticalSection(&m->cs);
    if (m->stop) return;
    k = m->wr % NSLOT;                                  /* free: only this thread writes it until wr moves */
    if (!m->slot[k] || m->sw[k] != w || m->sh[k] != h) {
        free(m->slot[k]);
        m->slot[k] = (uint32_t *)malloc((size_t)w * h * 4);
        if (!m->slot[k]) return;
        m->sw[k] = w; m->sh[k] = h;
    }
    memcpy(m->slot[k], rgba, (size_t)w * h * 4);
    EnterCriticalSection(&m->cs);
    m->wr++;
    WakeAllConditionVariable(&m->cv);
    LeaveCriticalSection(&m->cs);
}

static void s_video(void *user, const uint8_t *es, uint32_t n, int64_t pts)
{
    mstream *m = (mstream *)user;
    vpkt *p = (vpkt *)malloc(sizeof(vpkt) + n);
    if (!p) { m->failed = 1; return; }
    p->next = NULL; p->n = n; p->pts = pts;
    memcpy(p->data, es, n);
    if (m->vq_tail) m->vq_tail->next = p; else m->vq_head = p;
    m->vq_tail = p;
    m->vq_bytes += n;
}

static void decode(mstream *m, vpkt *p)
{
    if (!m->dec) {
        int w, h;
        double fps;
        if (!pss_sequence_header(p->data, p->n, &w, &h, &fps)) return;   /* nothing decodable before the first header */
        m->dec = mpeg2_open(w, h, fps, on_frame, m);
        if (!m->dec) { m->failed = 1; return; }
    }
    if (p->pts >= 0 && m->vpts < 0) { EnterCriticalSection(&m->cs); m->vpts = p->pts; LeaveCriticalSection(&m->cs); }
    mpeg2_feed(m->dec, p->data, p->n, p->pts);
}

static void s_fmt(void *user, const pss_audio_fmt *f, int64_t pts)
{
    mstream *m = (mstream *)user;
    /* the disc's movies are 16-bit PCM, 48 kHz stereo, 0x200-byte channel blocks */
    m->audio_ok = m->with_audio && f->type == 1 && f->channels == 2 && f->rate == 48000 &&
                  f->interleave > 0 && (uint32_t)f->interleave * 2 <= sizeof m->ablk && !(f->interleave & 1);
    m->il = (uint32_t)f->interleave;
    EnterCriticalSection(&m->cs);
    m->apts = pts;
    LeaveCriticalSection(&m->cs);
}

static void pcm_put(mstream *m, const int16_t *l, const int16_t *r, uint32_t n)
{
    uint32_t k;
    EnterCriticalSection(&m->cs);
    if (m->pcm_rd && m->pcm_rd * 2 >= m->pcm_n) {       /* drop what the host already has */
        memmove(m->pcm, m->pcm + (size_t)m->pcm_rd * 2, (size_t)(m->pcm_n - m->pcm_rd) * 4);
        m->pcm_n -= m->pcm_rd;
        m->pcm_rd = 0;
    }
    if (m->pcm_n + n > m->pcm_cap) {
        uint32_t nc = m->pcm_cap ? m->pcm_cap * 2 : 48000 * 4;
        int16_t *np;
        while (nc < m->pcm_n + n) nc *= 2;
        np = (int16_t *)realloc(m->pcm, (size_t)nc * 4);
        if (!np) { LeaveCriticalSection(&m->cs); return; }
        m->pcm = np; m->pcm_cap = nc;
    }
    for (k = 0; k < n; k++) { m->pcm[(m->pcm_n + k) * 2] = l ? l[k] : 0; m->pcm[(m->pcm_n + k) * 2 + 1] = r ? r[k] : 0; }
    m->pcm_n += n;
    LeaveCriticalSection(&m->cs);
}

static void s_audio(void *user, const uint8_t *data, uint32_t n)
{
    mstream *m = (mstream *)user;
    if (!m->audio_ok) return;
    while (n) {
        uint32_t take = m->il * 2 - m->ablk_n;
        if (take > n) take = n;
        memcpy(m->ablk + m->ablk_n, data, take);
        m->ablk_n += take; data += take; n -= take;
        if (m->ablk_n == m->il * 2) {                   /* one block: il bytes of left, il bytes of right */
            pcm_put(m, (const int16_t *)m->ablk, (const int16_t *)(m->ablk + m->il), m->il / 2);
            m->ablk_n = 0;
        }
    }
}

static DWORD WINAPI worker(LPVOID p)
{
    mstream *m = (mstream *)p;
    uint8_t *buf = (uint8_t *)malloc(CHUNK);
    pss_sink k;
    uint32_t off;
    int eof = 0;
    k.video = s_video; k.audio_fmt = s_fmt; k.audio = s_audio; k.user = m;
    pss_stream_init(&m->ps, &k);
    if (!buf) m->failed = 1;
    off = 0;
    while (!m->stop && !m->failed) {
        vpkt *p;
        /* read and demux ahead (the sound goes straight to the host's queue) */
        /* before the first picture, one piece: the first frame comes quickly; then the full lead */
        while (buf && !eof && m->vq_bytes < (m->wr ? AHEAD : CHUNK) && !m->stop && !m->failed) {
            uint32_t n = m->src.size - off < CHUNK ? m->src.size - off : CHUNK;
            if (!n) { eof = 1; break; }
            if (!m->src.read(m->src.user, off, (n + 2047) & ~2047u, buf)) { m->failed = 1; break; }
            off += n;
            if (pss_stream_feed(&m->ps, buf, n) <= 0) eof = 1;
        }
        /* decode one packet (waits in on_frame while the frame ring is full) */
        p = m->vq_head;
        if (!p) break;
        m->vq_head = p->next;
        if (!m->vq_head) m->vq_tail = NULL;
        m->vq_bytes -= p->n;
        if (!m->stop) decode(m, p);
        free(p);
    }
    while (m->vq_head) { vpkt *p = m->vq_head; m->vq_head = p->next; free(p); }
    m->vq_tail = NULL;
    if (m->dec && !m->stop && !m->failed) mpeg2_drain(m->dec);
    if (m->dec) mpeg2_close(m->dec);
    m->dec = NULL;
    pss_stream_free(&m->ps);
    free(buf);
    EnterCriticalSection(&m->cs);
    m->done = 1;
    WakeAllConditionVariable(&m->cv);
    LeaveCriticalSection(&m->cs);
    return 0;
}

movie_stats movie_last;

static double now_ms(void)
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

/* ---- game thread ----------------------------------------------------------------------------------- */

/* Sound that starts later than the picture (by the stream's timestamps) gets that much silence first; sound
 * that starts earlier loses its head. Done once, when both start times are known; called under m->cs. */
static void audio_align(mstream *m)
{
    int64_t d;
    if (m->apts < 0 || m->vpts < 0) return;
    d = (m->apts - m->vpts) * 48000 / 90000;
    m->apts = -1;
    if (d > 48000 || d < -48000) return;                /* not a start offset: leave the sound as it is */
    if (d < 0) {
        uint32_t drop = (uint32_t)-d, q = m->pcm_n - m->pcm_rd;
        m->pcm_rd += drop < q ? drop : q;
    } else if (d > 0) {
        uint32_t q = m->pcm_n - m->pcm_rd, need = q + (uint32_t)d, cap = need > m->pcm_cap ? need : m->pcm_cap;
        int16_t *np = (int16_t *)calloc(cap, 4);
        if (!np) return;
        if (q) memcpy(np + (size_t)d * 2, m->pcm + (size_t)m->pcm_rd * 2, (size_t)q * 4);
        free(m->pcm);
        m->pcm = np; m->pcm_cap = cap; m->pcm_n = need; m->pcm_rd = 0;
    }
}

/* Sound follows the field clock: 800 stereo frames a field from the first picture on, kept four fields ahead
 * (sound demuxed late, after a slow disc read, still comes in time). */
static void feed_audio(mstream *m, const movie_host *h, uint32_t elapsed, uint32_t *sent)
{
    uint32_t want = (elapsed + 4) * 800u;
    EnterCriticalSection(&m->cs);
    audio_align(m);
    while (*sent < want && m->pcm_rd < m->pcm_n && h->audio_queued(h->user) < 4800) {
        uint32_t n = want - *sent;
        if (n > m->pcm_n - m->pcm_rd) n = m->pcm_n - m->pcm_rd;
        n = h->audio(h->user, m->pcm + (size_t)m->pcm_rd * 2, n);
        if (!n) break;
        m->pcm_rd += n;
        *sent += n;
    }
    LeaveCriticalSection(&m->cs);
}

static uint32_t fields_now(const movie_host *h, uint32_t own) { return h->fields ? h->fields(h->user) : own; }

int movie_play_source(const movie_source *src, int with_audio, int with_inter, const movie_host *h)
{
    mstream *m = (mstream *)calloc(1, sizeof *m);
    uint32_t pad = 0, pad_old = 0, pcm_sent = 0, own = 0, f0 = 0;
    int shown = 0, skipped = 0;
    double t0 = now_ms();
    if (!m) return -1;
    memset(&movie_last, 0, sizeof movie_last);
    m->src = *src;
    m->with_audio = with_audio;
    m->apts = m->vpts = -1;
    InitializeCriticalSection(&m->cs);
    InitializeConditionVariable(&m->cv);
    m->thread = CreateThread(NULL, 0, worker, m, 0, NULL);
    if (!m->thread) { DeleteCriticalSection(&m->cs); free(m); return -1; }
    for (;;) {
        const uint32_t *frame;
        int fw, fh;
        /* the next frame; wait for the worker when it is behind (the first frame, or a slow disc) */
        double tw = now_ms();
        EnterCriticalSection(&m->cs);
        if (m->rd == m->wr && !m->done && shown) movie_last.stalls++;
        while (m->rd == m->wr && !m->done) {
            SleepConditionVariableCS(&m->cv, &m->cs, 5);
            if (h->idle && m->rd == m->wr && !m->done) {
                LeaveCriticalSection(&m->cs);
                h->idle(h->user);
                EnterCriticalSection(&m->cs);
            }
        }
        if (m->rd == m->wr) { LeaveCriticalSection(&m->cs); break; }   /* the end */
        frame = m->slot[m->rd % NSLOT];
        fw = m->sw[m->rd % NSLOT]; fh = m->sh[m->rd % NSLOT];
        m->freed = m->rd;                                /* the frame shown before this one goes back */
        m->rd++;
        WakeAllConditionVariable(&m->cv);
        LeaveCriticalSection(&m->cs);
        if (!shown) movie_last.first_ms = now_ms() - t0;
        else if (now_ms() - tw > movie_last.stall_ms) movie_last.stall_ms = now_ms() - tw;
        if (!shown) f0 = fields_now(h, own);
        if (frame) h->show(h->user, frame, fw, fh);
        shown++;
        feed_audio(m, h, fields_now(h, own) - f0, &pcm_sent);
        /* frame n is on screen for fields 2n-2 and 2n-1 after the first (30 fps on the 60 Hz field clock); the
         * pad is read every field as the original. Fields the runtime ran elsewhere (catching up after a slow
         * one) count too: a frame that is already late is shown at once and replaced, so the picture keeps
         * time with the sound. */
        if (fields_now(h, own) - f0 > (uint32_t)shown * 2) movie_last.late++;
        while (fields_now(h, own) - f0 < (uint32_t)shown * 2) {
            uint32_t pushed;
            h->wait_vblank(h->user);
            own++;
            feed_audio(m, h, fields_now(h, own) - f0, &pcm_sent);
            pad = h->pad(h->user) & 0xFFFF;
            pushed = pad & (pad_old ^ pad) & 0x8E0;
            pad_old = pad;
            if (with_inter && pushed && shown > 10) { skipped = 1; break; }
        }
        if (skipped) break;
    }
    movie_last.frames = shown;
    movie_last.skipped = skipped;
    movie_last.audio_frames = pcm_sent;
    h->show(h->user, NULL, 0, 0);
    InterlockedExchange(&m->stop, 1);
    EnterCriticalSection(&m->cs);
    WakeAllConditionVariable(&m->cv);
    LeaveCriticalSection(&m->cs);
    WaitForSingleObject(m->thread, INFINITE);
    CloseHandle(m->thread);
    { int i; for (i = 0; i < NSLOT; i++) free(m->slot[i]); }
    free(m->pcm);
    DeleteCriticalSection(&m->cs);
    free(m);
    return shown ? (int)pad : -1;
}

/* ---- a file in memory ------------------------------------------------------------------------------ */

typedef struct memfile { const uint8_t *d; uint32_t n; } memfile;

static int mem_read(void *user, uint32_t off, uint32_t size, void *dst)
{
    memfile *f = (memfile *)user;
    if (off >= f->n) return 0;
    memcpy(dst, f->d + off, size < f->n - off ? size : f->n - off);
    return 1;
}

int movie_run(const uint8_t *data, uint32_t size, int with_audio, int with_inter, const movie_host *h)
{
    memfile f;
    movie_source s;
    f.d = data; f.n = size;
    s.read = mem_read; s.user = &f; s.size = size;
    return movie_play_source(&s, with_audio, with_inter, h);
}
