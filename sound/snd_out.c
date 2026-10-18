/* snd_out.c - play the SPU2's 48 kHz stereo output through XAudio2 (part of Windows). One buffer per field,
 * a small queue; when the game runs ahead of the sound card a field is dropped, when behind a gap is heard.
 *
 * The game's clock is the PS2's 59.94 Hz field rate and every field brings 800 frames (48000 / 60), so the card
 * would drain 48 frames a second faster than they come and click every ~17 s: the voice plays at 59.94/60 of its
 * rate, nudged by up to 0.3% to keep the queue near its target (a pitch change nobody hears). A lost device
 * (headphones unplugged, driver restart) is noticed through the engine callback and the output is reopened. */
#define COBJMACROS
#include <windows.h>
#include <xaudio2.h>
#include "iop.h"

#define NBUF 10
#define FIELD 800
#define TARGET 5          /* fields queued in the steady state (83 ms): one slow field of up to ~80 ms (the
                             slowest the M1g log shows is 75 ms) still finds sound queued, no gap */

static IXAudio2 *xa;
static IXAudio2MasteringVoice *master;
static IXAudio2SourceVoice *src;
static s16 bufs[NBUF][FIELD * 2];
static int next_buf, retry_in;
static volatile LONG lost;

/* engine callback: only OnCriticalError matters */
static void STDMETHODCALLTYPE cb_pass_start(IXAudio2EngineCallback *t) { (void)t; }
static void STDMETHODCALLTYPE cb_pass_end(IXAudio2EngineCallback *t) { (void)t; }
static void STDMETHODCALLTYPE cb_error(IXAudio2EngineCallback *t, HRESULT e) { (void)t; (void)e; InterlockedExchange(&lost, 1); }
static IXAudio2EngineCallbackVtbl cb_vtbl = { cb_pass_start, cb_pass_end, cb_error };
static IXAudio2EngineCallback cb = { &cb_vtbl };

/* Playback speed for a queue of `queued` fields: the 59.94/60 base, nudged toward TARGET queued fields. */
float snd_out_ratio(int queued)
{
    float r = (59.94f / 60.0f) * (1.0f + 0.001f * (float)(queued - TARGET));
    return r < 0.996f ? 0.996f : r > 1.002f ? 1.002f : r;
}

/* For the headless test: the XAudio2 engine (no device, nothing played) with our callback registered. */
int snd_out_engine_check(void)
{
    IXAudio2 *e = NULL;
    if (FAILED(CoInitializeEx(NULL, COINIT_MULTITHREADED))) { /* already initialised another way: fine */ }
    if (FAILED(XAudio2Create(&e, 0, XAUDIO2_DEFAULT_PROCESSOR))) return 0;
    if (FAILED(IXAudio2_RegisterForCallbacks(e, &cb))) { IXAudio2_Release(e); return 0; }
    IXAudio2_UnregisterForCallbacks(e, &cb);
    IXAudio2_Release(e);
    return 1;
}

void snd_out_close(void)
{
    if (src) { IXAudio2SourceVoice_Stop(src, 0, XAUDIO2_COMMIT_NOW); IXAudio2SourceVoice_DestroyVoice(src); src = NULL; }
    if (master) { IXAudio2MasteringVoice_DestroyVoice(master); master = NULL; }
    if (xa) { IXAudio2_UnregisterForCallbacks(xa, &cb); IXAudio2_Release(xa); xa = NULL; }
}

int snd_out_open(void)
{
    WAVEFORMATEX wf;
    if (FAILED(CoInitializeEx(NULL, COINIT_MULTITHREADED))) { /* already initialised another way: fine */ }
    lost = 0;
    if (FAILED(XAudio2Create(&xa, 0, XAUDIO2_DEFAULT_PROCESSOR))) { xa = NULL; return 0; }
    IXAudio2_RegisterForCallbacks(xa, &cb);
    if (FAILED(IXAudio2_CreateMasteringVoice(xa, &master, 2, 48000, 0, NULL, NULL, AudioCategory_GameEffects))) {
        master = NULL; snd_out_close(); return 0;
    }
    memset(&wf, 0, sizeof wf);
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = 2;
    wf.nSamplesPerSec = 48000;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = 4;
    wf.nAvgBytesPerSec = 48000 * 4;
    if (FAILED(IXAudio2_CreateSourceVoice(xa, &src, &wf, 0, 2.0f, NULL, NULL, NULL))) { src = NULL; snd_out_close(); return 0; }
    IXAudio2SourceVoice_SetFrequencyRatio(src, 59.94f / 60.0f, XAUDIO2_COMMIT_NOW);
    IXAudio2SourceVoice_Start(src, 0, XAUDIO2_COMMIT_NOW);
    next_buf = 0;
    return 1;
}

void snd_out_push(const s16 *pcm, int frames)
{
    XAUDIO2_VOICE_STATE st;
    XAUDIO2_BUFFER b;
    float ratio;
    if (lost) {   /* the device went away: drop the engine, try a fresh one about once a second */
        snd_out_close();
        lost = 0;
        retry_in = 1;
    }
    if (!src) {   /* no device (none at start, or lost): look for one about once a second */
        if (!retry_in) retry_in = 60;
        if (--retry_in == 0 && !snd_out_open()) retry_in = 60;
        if (!src) return;
    }
    if (frames <= 0 || frames > FIELD) return;
    IXAudio2SourceVoice_GetState(src, &st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
    if (st.BuffersQueued >= NBUF - 2) return;          /* running ahead: drop this field */
    ratio = snd_out_ratio((int)st.BuffersQueued);
    IXAudio2SourceVoice_SetFrequencyRatio(src, ratio, XAUDIO2_COMMIT_NOW);
    memcpy(bufs[next_buf], pcm, (size_t)frames * 4);
    memset(&b, 0, sizeof b);
    b.AudioBytes = (UINT32)frames * 4;
    b.pAudioData = (const BYTE *)bufs[next_buf];
    IXAudio2SourceVoice_SubmitSourceBuffer(src, &b, NULL);
    next_buf = (next_buf + 1) % NBUF;
}
