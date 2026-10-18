/* mpeg2_mf.c - Windows' MPEG-2 video decoder (Media Foundation transform msmpeg2vdec, 32-bit, part of Windows)
 * used as a plain function: bytes in, RGBA frames out. No window, no player. */
#define COBJMACROS
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <stdio.h>
#include <stdlib.h>
#include "mpeg2.h"

struct mpeg2_dec {
    IMFTransform *mft;
    mpeg2_frame_fn fn;
    void *user;
    int w, h;            /* output frame size */
    LONG stride;
    uint32_t *rgba;
    int provides_samples;
    DWORD out_size;
};

static char err[160];
const char *mpeg2_error(void) { return err; }
static int fail(const char *what, HRESULT hr) { snprintf(err, sizeof err, "%s failed (0x%08lx)", what, (unsigned long)hr); return 0; }

static int set_output_type(mpeg2_dec *d)
{
    DWORD i;
    for (i = 0; ; i++) {
        IMFMediaType *t = NULL;
        GUID sub;
        HRESULT hr = IMFTransform_GetOutputAvailableType(d->mft, 0, i, &t);
        if (FAILED(hr)) return fail("no NV12 output type", hr);
        if (SUCCEEDED(IMFMediaType_GetGUID(t, &MF_MT_SUBTYPE, &sub)) && IsEqualGUID(&sub, &MFVideoFormat_NV12)) {
            UINT64 fs = 0;
            UINT32 st = 0;
            hr = IMFTransform_SetOutputType(d->mft, 0, t, 0);
            if (SUCCEEDED(IMFMediaType_GetUINT64(t, &MF_MT_FRAME_SIZE, &fs))) { d->w = (int)(fs >> 32); d->h = (int)(UINT32)fs; }
            d->stride = SUCCEEDED(IMFMediaType_GetUINT32(t, &MF_MT_DEFAULT_STRIDE, &st)) ? (LONG)st : d->w;
            IMFMediaType_Release(t);
            if (FAILED(hr)) return fail("SetOutputType", hr);
            {
                MFT_OUTPUT_STREAM_INFO si;
                IMFTransform_GetOutputStreamInfo(d->mft, 0, &si);
                d->provides_samples = (si.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
                d->out_size = si.cbSize;
            }
            free(d->rgba);
            d->rgba = (uint32_t *)malloc((size_t)d->w * d->h * 4);
            return d->rgba != NULL;
        }
        IMFMediaType_Release(t);
    }
}

mpeg2_dec *mpeg2_open(int width, int height, double fps, mpeg2_frame_fn fn, void *user)
{
    MFT_REGISTER_TYPE_INFO in = { MFMediaType_Video, MFVideoFormat_MPEG2 };
    IMFActivate **acts = NULL;
    UINT32 n = 0, i;
    IMFMediaType *t = NULL;
    mpeg2_dec *d;
    HRESULT hr;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    hr = MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
    if (FAILED(hr)) { fail("MFStartup", hr); return NULL; }
    hr = MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER,
                   &in, NULL, &acts, &n);
    if (FAILED(hr) || !n) { fail("finding the Windows MPEG-2 decoder", hr); return NULL; }
    d = (mpeg2_dec *)calloc(1, sizeof *d);
    d->fn = fn; d->user = user;
    hr = IMFActivate_ActivateObject(acts[0], &IID_IMFTransform, (void **)&d->mft);
    for (i = 0; i < n; i++) IMFActivate_Release(acts[i]);
    CoTaskMemFree(acts);
    if (FAILED(hr)) { fail("ActivateObject", hr); free(d); return NULL; }
    MFCreateMediaType(&t);
    IMFMediaType_SetGUID(t, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    IMFMediaType_SetGUID(t, &MF_MT_SUBTYPE, &MFVideoFormat_MPEG2);
    IMFMediaType_SetUINT64(t, &MF_MT_FRAME_SIZE, ((UINT64)width << 32) | (UINT32)height);
    if (fps > 0) {
        UINT32 num = (UINT32)(fps * 1001.0 + 0.5), den = 1001;
        if (fps == 30.0 || fps == 25.0 || fps == 24.0 || fps == 60.0 || fps == 50.0) { num = (UINT32)fps; den = 1; }
        IMFMediaType_SetUINT64(t, &MF_MT_FRAME_RATE, ((UINT64)num << 32) | den);
    }
    hr = IMFTransform_SetInputType(d->mft, 0, t, 0);
    IMFMediaType_Release(t);
    if (FAILED(hr)) { fail("SetInputType", hr); mpeg2_close(d); return NULL; }
    if (!set_output_type(d)) { mpeg2_close(d); return NULL; }
    IMFTransform_ProcessMessage(d->mft, MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    IMFTransform_ProcessMessage(d->mft, MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return d;
}

static inline uint8_t clamp8(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

/* NV12 (BT.601, studio range, as MPEG-2 video) -> RGBA */
static void nv12_to_rgba(mpeg2_dec *d, const BYTE *y_plane, LONG pitch, int plane_h)
{
    int x, y;
    const BYTE *uv_plane = y_plane + (size_t)pitch * plane_h;
    for (y = 0; y < d->h; y++) {
        const BYTE *yr = y_plane + (size_t)pitch * y, *uvr = uv_plane + (size_t)pitch * (y >> 1);
        uint32_t *o = d->rgba + (size_t)y * d->w;
        for (x = 0; x < d->w; x++) {
            int c = (yr[x] - 16) * 298, u = uvr[x & ~1] - 128, v = uvr[(x & ~1) + 1] - 128;
            int r = (c + 409 * v + 128) >> 8, g = (c - 100 * u - 208 * v + 128) >> 8, b = (c + 516 * u + 128) >> 8;
            o[x] = clamp8(r) | ((uint32_t)clamp8(g) << 8) | ((uint32_t)clamp8(b) << 16) | 0xFF000000u;
        }
    }
}

static void pull_outputs(mpeg2_dec *d)
{
    for (;;) {
        MFT_OUTPUT_DATA_BUFFER ob;
        DWORD status = 0;
        HRESULT hr;
        IMFSample *own = NULL;
        memset(&ob, 0, sizeof ob);
        if (!d->provides_samples) {
            IMFMediaBuffer *b = NULL;
            MFCreateSample(&own);
            MFCreateMemoryBuffer(d->out_size ? d->out_size : (DWORD)(d->w * d->h * 2), &b);
            IMFSample_AddBuffer(own, b);
            IMFMediaBuffer_Release(b);
            ob.pSample = own;
        }
        hr = IMFTransform_ProcessOutput(d->mft, 0, 1, &ob, &status);
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            if (own) IMFSample_Release(own);
            if (!set_output_type(d)) return;
            continue;
        }
        if (FAILED(hr)) { if (own) IMFSample_Release(own); if (ob.pEvents) IMFCollection_Release(ob.pEvents); return; }
        if (ob.pSample) {
            IMFMediaBuffer *buf = NULL;
            IMF2DBuffer *b2 = NULL;
            LONGLONG t = -1;
            BYTE *p; LONG pitch;
            IMFSample_GetSampleTime(ob.pSample, &t);
            IMFSample_ConvertToContiguousBuffer(ob.pSample, &buf);
            if (SUCCEEDED(IMFMediaBuffer_QueryInterface(buf, &IID_IMF2DBuffer, (void **)&b2)) &&
                SUCCEEDED(IMF2DBuffer_Lock2D(b2, &p, &pitch))) {
                nv12_to_rgba(d, p, pitch, d->h);
                IMF2DBuffer_Unlock2D(b2);
                d->fn(d->user, d->rgba, d->w, d->h, t);
            } else {
                DWORD len = 0;
                if (SUCCEEDED(IMFMediaBuffer_Lock(buf, &p, NULL, &len))) {
                    LONG pt = d->stride > 0 ? d->stride : d->w;
                    nv12_to_rgba(d, p, pt, (int)(len / (DWORD)pt * 2 / 3));
                    IMFMediaBuffer_Unlock(buf);
                    d->fn(d->user, d->rgba, d->w, d->h, t);
                }
            }
            if (b2) IMF2DBuffer_Release(b2);
            if (buf) IMFMediaBuffer_Release(buf);
            if (ob.pSample != own) IMFSample_Release(ob.pSample);
        }
        if (own) IMFSample_Release(own);
        if (ob.pEvents) IMFCollection_Release(ob.pEvents);
    }
}

int mpeg2_feed(mpeg2_dec *d, const uint8_t *data, uint32_t size, int64_t pts90)
{
    IMFSample *s = NULL;
    IMFMediaBuffer *b = NULL;
    BYTE *p;
    HRESULT hr;
    MFCreateMemoryBuffer(size, &b);
    IMFMediaBuffer_Lock(b, &p, NULL, NULL);
    memcpy(p, data, size);
    IMFMediaBuffer_Unlock(b);
    IMFMediaBuffer_SetCurrentLength(b, size);
    MFCreateSample(&s);
    IMFSample_AddBuffer(s, b);
    IMFMediaBuffer_Release(b);
    if (pts90 >= 0) IMFSample_SetSampleTime(s, pts90 * 1000 / 9);
    for (;;) {
        hr = IMFTransform_ProcessInput(d->mft, 0, s, 0);
        if (hr == MF_E_NOTACCEPTING) { pull_outputs(d); continue; }
        break;
    }
    IMFSample_Release(s);
    if (FAILED(hr)) return fail("ProcessInput", hr);
    pull_outputs(d);
    return 1;
}

void mpeg2_drain(mpeg2_dec *d)
{
    IMFTransform_ProcessMessage(d->mft, MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
    IMFTransform_ProcessMessage(d->mft, MFT_MESSAGE_COMMAND_DRAIN, 0);
    pull_outputs(d);
}

void mpeg2_close(mpeg2_dec *d)
{
    if (!d) return;
    if (d->mft) IMFTransform_Release(d->mft);
    free(d->rgba);
    free(d);
    MFShutdown();
}
