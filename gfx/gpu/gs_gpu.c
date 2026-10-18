/* gs_gpu.c - the GS on the GPU (Direct3D 11): device, GS memory buffer, transfers, the TV read-out.
 * Shaders are compiled at build time (fxc) into headers in the build folder. */
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <stdlib.h>
#include <string.h>
#include <d3d11_3.h>
#include "gs_gpu.h"
#include "upload_cs.h"
#include "readout_cs.h"
#include "readout_tex_cs.h"
#include "copy_cs.h"
#include "clutload_cs.h"
#include "draw_vs.h"
#include "draw_ps.h"
#include "draw_ps_direct.h"
#include "../gs_fast.h"

int gs_gpu_on, gs_gpu_only;

static ID3D11Device *dev;
static ID3D11DeviceContext *ctx;
static ID3D11Buffer *mem, *mem_stage;                 /* GS memory (4 MB of words) and its read-back copy */
static ID3D11UnorderedAccessView *mem_uav;
static ID3D11ShaderResourceView *mem_srv;
static ID3D11ComputeShader *cs_upload, *cs_readout, *cs_copy, *cs_readout_tex, *cs_clut;
static ID3D11UnorderedAccessView *clut_uav;
static ID3D11Texture2D *pic_tex;                      /* the window's picture */
static ID3D11UnorderedAccessView *pic_tex_uav;
static ID3D11Buffer *cbuf;                            /* constants, rewritten per dispatch */
static ID3D11Buffer *vals;                            /* transfer pixels */
static ID3D11ShaderResourceView *vals_srv;
static u32 vals_cap, vals_pos;                        /* a ring: appended to, discarded only when it wraps */
static int no_overwrite;                              /* the driver lets dynamic SRV buffers be appended to */
#define VALS_RING (4u << 20)
#define PRIM_RING 32768
static int prim_pos, prim_mapped, vals_mapped;          /* a dynamic buffer's first map must discard */
static ID3D11Buffer *pic, *pic_stage;                 /* the TV picture (up to 1024 x 1024) */
static ID3D11UnorderedAccessView *pic_uav;

/* drawing */
#define BATCH_MAX 4096
#define CLUT_SLOTS 4096
static ID3D11VertexShader *vs_draw;
static ID3D11PixelShader *ps_draw;
static ID3D11RasterizerState *rs_draw;
static int conservative;                              /* rs_draw covers every touched pixel */
static ID3D11Buffer *prim_buf, *clut_buf, *tmem;      /* queued primitives, CLUT versions, memory as textures see it */
static ID3D11ShaderResourceView *prim_srv, *clut_srv, *tmem_srv;
static gpu_prim *batch;
static int nbatch;
static u8 tdirty[16384];                              /* blocks written since tmem last copied them */
static u32 clut_next, clut_used;                      /* ring of CLUT versions */
static u32 clut_cur;                                  /* the version the next primitives use */
static struct { u32 cbp, cpsm, csm, csa, n, cbw, cou, cov, lo, hi; int valid; } clut_prev;   /* the last load */

#define PIC_MAX (1024 * 1024)

static void set_constants(const void *data, UINT bytes);

/* DOTR_GPU_STATS=1: what the GPU path costs the game thread, logged at shutdown */
enum { ST_FLUSH, ST_UPLOAD, ST_COPY, ST_TSYNC, ST_CLUT, ST_READOUT, ST_N };
static const char *st_name[ST_N] = { "draw batches", "uploads", "local copies", "texture copies", "CLUT versions", "read-outs" };
static double st_ms[ST_N];
static unsigned st_n[ST_N];
static unsigned long long st_prims;
static int st_on = -1;
static double st_now(void) { LARGE_INTEGER f, t; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t); return (double)t.QuadPart * 1000.0 / (double)f.QuadPart; }
#define ST_BEGIN double st_t0 = st_on ? st_now() : 0
#define ST_END(k) do { if (st_on) { st_ms[k] += st_now() - st_t0; st_n[k]++; } } while (0)

static ID3D11Buffer *make_buffer(UINT bytes, UINT bind, UINT misc, D3D11_USAGE usage, UINT cpu, UINT stride)
{
    D3D11_BUFFER_DESC d;
    ID3D11Buffer *b = NULL;
    memset(&d, 0, sizeof d);
    d.ByteWidth = bytes; d.BindFlags = bind; d.MiscFlags = misc; d.Usage = usage; d.CPUAccessFlags = cpu;
    d.StructureByteStride = stride;
    if (FAILED(ID3D11Device_CreateBuffer(dev, &d, NULL, &b))) return NULL;
    return b;
}

/* Time the game thread spends blocked on the GPU (read-backs that wait for its work): gfx's timing report */
double gs_gpu_wait_ms;
static HRESULT map_read(ID3D11Resource *r, D3D11_MAPPED_SUBRESOURCE *m)
{
    LARGE_INTEGER t0, t1, f;
    HRESULT hr;
    QueryPerformanceCounter(&t0);
    hr = ID3D11DeviceContext_Map(ctx, r, 0, D3D11_MAP_READ, 0, m);
    QueryPerformanceCounter(&t1); QueryPerformanceFrequency(&f);
    gs_gpu_wait_ms += (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart;
    return hr;
}

static ID3D11UnorderedAccessView *raw_uav(ID3D11Buffer *b, UINT words)
{
    D3D11_UNORDERED_ACCESS_VIEW_DESC u;
    ID3D11UnorderedAccessView *v = NULL;
    memset(&u, 0, sizeof u);
    u.Format = DXGI_FORMAT_R32_TYPELESS; u.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    u.Buffer.NumElements = words; u.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
    if (FAILED(ID3D11Device_CreateUnorderedAccessView(dev, (ID3D11Resource *)b, &u, &v))) return NULL;
    return v;
}

static ID3D11ShaderResourceView *raw_srv(ID3D11Buffer *b, UINT words)
{
    D3D11_SHADER_RESOURCE_VIEW_DESC s;
    ID3D11ShaderResourceView *v = NULL;
    memset(&s, 0, sizeof s);
    s.Format = DXGI_FORMAT_R32_TYPELESS; s.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
    s.BufferEx.NumElements = words; s.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
    if (FAILED(ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)b, &s, &v))) return NULL;
    return v;
}

#define RELEASE(p, T) do { if (p) { T##_Release(p); (p) = NULL; } } while (0)

static void stats_log(void);
void gs_gpu_stats_field(void)
{
    static unsigned fields;
    if (st_on > 0 && ++fields % 1000 == 0) { hw_log(2, "gpu stats after %u fields:", fields); stats_log(); }
}

static void stats_log(void)
{
    if (st_on > 0) {
        int k;
        for (k = 0; k < ST_N; k++) hw_log(2, "gpu stats: %-15s %8u calls %9.1f ms", st_name[k], st_n[k], st_ms[k]);
        hw_log(2, "gpu stats: %llu primitives", st_prims);
    }
}

void gs_gpu_shutdown(void)
{
    stats_log();
    RELEASE(prim_srv, ID3D11ShaderResourceView); RELEASE(clut_srv, ID3D11ShaderResourceView); RELEASE(tmem_srv, ID3D11ShaderResourceView);
    RELEASE(prim_buf, ID3D11Buffer); RELEASE(clut_buf, ID3D11Buffer); RELEASE(tmem, ID3D11Buffer);
    RELEASE(vs_draw, ID3D11VertexShader); RELEASE(ps_draw, ID3D11PixelShader); RELEASE(rs_draw, ID3D11RasterizerState);
    free(batch); batch = NULL; nbatch = 0; prim_pos = 0; vals_pos = 0; prim_mapped = vals_mapped = 0;
    clut_next = clut_used = clut_cur = 0; clut_prev.valid = 0;
    RELEASE(pic_uav, ID3D11UnorderedAccessView); RELEASE(pic, ID3D11Buffer); RELEASE(pic_stage, ID3D11Buffer);
    RELEASE(vals_srv, ID3D11ShaderResourceView); RELEASE(vals, ID3D11Buffer); vals_cap = 0;
    RELEASE(cbuf, ID3D11Buffer);
    RELEASE(cs_upload, ID3D11ComputeShader); RELEASE(cs_readout, ID3D11ComputeShader); RELEASE(cs_copy, ID3D11ComputeShader);
    RELEASE(cs_clut, ID3D11ComputeShader); RELEASE(clut_uav, ID3D11UnorderedAccessView);
    RELEASE(cs_readout_tex, ID3D11ComputeShader); RELEASE(pic_tex_uav, ID3D11UnorderedAccessView); RELEASE(pic_tex, ID3D11Texture2D);
    RELEASE(mem_srv, ID3D11ShaderResourceView); RELEASE(mem_uav, ID3D11UnorderedAccessView);
    RELEASE(mem, ID3D11Buffer); RELEASE(mem_stage, ID3D11Buffer);
    RELEASE(ctx, ID3D11DeviceContext); RELEASE(dev, ID3D11Device);
    gs_gpu_on = gs_gpu_only = 0;
}

/* gpu_stepping=direct|exact (DOTR_GPU_STEPPING until DotR-PC.ini carries it): triangle pixels evaluated directly
 * (the default: the in-game duel frame is ~3.4 ms faster), or stepped along the row as the exact renderer does
 * (bit-identical GS memory; the same pictures on the reference frames) */
static int gpu_stepping_direct(void)
{
    const char *s = getenv("DOTR_GPU_STEPPING");
    return !(s && !strcmp(s, "exact"));
}

int gs_gpu_init(void)
{
    static const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    D3D11_FEATURE_DATA_D3D11_OPTIONS2 o2;
    D3D11_FEATURE_DATA_D3D11_OPTIONS dop;
    D3D11_FEATURE_DATA_DOUBLES dbl;
    if (gs_gpu_on) return 1;
    if (FAILED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, levels, 2, D3D11_SDK_VERSION, &dev, NULL, &ctx))) {
        hw_log(1, "gpu: no Direct3D 11 GPU; the exact renderer draws");
        return 0;
    }
    memset(&o2, 0, sizeof o2);
    if (FAILED(ID3D11Device_CheckFeatureSupport(dev, D3D11_FEATURE_D3D11_OPTIONS2, &o2, sizeof o2)) || !o2.ROVsSupported) {
        hw_log(1, "gpu: this GPU or driver has no rasterizer ordered views; the exact renderer draws");
        gs_gpu_shutdown();
        return 0;
    }
    memset(&dop, 0, sizeof dop); memset(&dbl, 0, sizeof dbl);
    if (FAILED(ID3D11Device_CheckFeatureSupport(dev, D3D11_FEATURE_DOUBLES, &dbl, sizeof dbl)) || !dbl.DoublePrecisionFloatShaderOps ||
        FAILED(ID3D11Device_CheckFeatureSupport(dev, D3D11_FEATURE_D3D11_OPTIONS, &dop, sizeof dop)) ||
        !dop.ExtendedDoublesShaderInstructions) {
        hw_log(1, "gpu: this GPU or driver has no double-precision shader maths; the exact renderer draws");
        gs_gpu_shutdown();
        return 0;
    }
    no_overwrite = dop.MapNoOverwriteOnDynamicBufferSRV;
    mem = make_buffer(GS_MEM_BYTES, D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE,
                      D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, D3D11_USAGE_DEFAULT, 0, 0);
    mem_stage = make_buffer(GS_MEM_BYTES, 0, 0, D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ, 0);
    pic = make_buffer(PIC_MAX * 4, D3D11_BIND_UNORDERED_ACCESS, D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS,
                      D3D11_USAGE_DEFAULT, 0, 0);
    pic_stage = make_buffer(PIC_MAX * 4, 0, 0, D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ, 0);
    cbuf = make_buffer(256, D3D11_BIND_CONSTANT_BUFFER, 0, D3D11_USAGE_DYNAMIC, D3D11_CPU_ACCESS_WRITE, 0);
    tmem = make_buffer(GS_MEM_BYTES, D3D11_BIND_SHADER_RESOURCE, D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, D3D11_USAGE_DEFAULT, 0, 0);
    prim_buf = make_buffer(PRIM_RING * sizeof(gpu_prim), D3D11_BIND_SHADER_RESOURCE, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED,
                           D3D11_USAGE_DYNAMIC, D3D11_CPU_ACCESS_WRITE, sizeof(gpu_prim));
    clut_buf = make_buffer(CLUT_SLOTS * 1024, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
                           D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, D3D11_USAGE_DEFAULT, 0, 4);
    batch = (gpu_prim *)malloc(BATCH_MAX * sizeof(gpu_prim));
    if (!mem || !mem_stage || !pic || !pic_stage || !cbuf || !tmem || !prim_buf || !clut_buf || !batch) goto fail;
    {
        D3D11_SHADER_RESOURCE_VIEW_DESC s;
        D3D11_RASTERIZER_DESC r;
        memset(&s, 0, sizeof s);
        s.Format = DXGI_FORMAT_UNKNOWN; s.ViewDimension = D3D11_SRV_DIMENSION_BUFFER; s.Buffer.NumElements = PRIM_RING;
        if (FAILED(ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)prim_buf, &s, &prim_srv))) goto fail;
        s.Buffer.NumElements = CLUT_SLOTS * 256;
        if (FAILED(ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)clut_buf, &s, &clut_srv))) goto fail;
        {
            D3D11_UNORDERED_ACCESS_VIEW_DESC u;
            memset(&u, 0, sizeof u);
            u.Format = DXGI_FORMAT_UNKNOWN; u.ViewDimension = D3D11_UAV_DIMENSION_BUFFER; u.Buffer.NumElements = CLUT_SLOTS * 256;
            if (FAILED(ID3D11Device_CreateUnorderedAccessView(dev, (ID3D11Resource *)clut_buf, &u, &clut_uav))) goto fail;
        }
        tmem_srv = raw_srv(tmem, GS_MEM_WORDS);
        if (!tmem_srv) goto fail;
        memset(&r, 0, sizeof r);
        r.FillMode = D3D11_FILL_SOLID; r.CullMode = D3D11_CULL_NONE; r.DepthClipEnable = FALSE;
        conservative = 0;
        if (o2.ConservativeRasterizationTier >= D3D11_CONSERVATIVE_RASTERIZATION_TIER_1) {
            ID3D11Device3 *d3 = NULL;
            if (SUCCEEDED(ID3D11Device_QueryInterface(dev, &IID_ID3D11Device3, (void **)&d3))) {
                D3D11_RASTERIZER_DESC2 r2;
                ID3D11RasterizerState2 *s2 = NULL;
                memset(&r2, 0, sizeof r2);
                r2.FillMode = D3D11_FILL_SOLID; r2.CullMode = D3D11_CULL_NONE; r2.DepthClipEnable = FALSE;
                r2.ConservativeRaster = D3D11_CONSERVATIVE_RASTERIZATION_MODE_ON;
                if (SUCCEEDED(ID3D11Device3_CreateRasterizerState2(d3, &r2, &s2))) {
                    rs_draw = (ID3D11RasterizerState *)s2;
                    conservative = 1;
                }
                ID3D11Device3_Release(d3);
            }
        }
        if (!rs_draw && FAILED(ID3D11Device_CreateRasterizerState(dev, &r, &rs_draw))) goto fail;
        if (FAILED(ID3D11Device_CreateVertexShader(dev, g_draw_vs, sizeof g_draw_vs, NULL, &vs_draw)) ||
            FAILED(gpu_stepping_direct()
                       ? ID3D11Device_CreatePixelShader(dev, g_draw_ps_direct, sizeof g_draw_ps_direct, NULL, &ps_draw)
                       : ID3D11Device_CreatePixelShader(dev, g_draw_ps, sizeof g_draw_ps, NULL, &ps_draw)))
            goto fail;
    }
    mem_uav = raw_uav(mem, GS_MEM_WORDS); mem_srv = raw_srv(mem, GS_MEM_WORDS); pic_uav = raw_uav(pic, PIC_MAX);
    if (!mem_uav || !mem_srv || !pic_uav) goto fail;
    if (FAILED(ID3D11Device_CreateComputeShader(dev, g_upload_cs, sizeof g_upload_cs, NULL, &cs_upload)) ||
        FAILED(ID3D11Device_CreateComputeShader(dev, g_readout_cs, sizeof g_readout_cs, NULL, &cs_readout)) ||
        FAILED(ID3D11Device_CreateComputeShader(dev, g_copy_cs, sizeof g_copy_cs, NULL, &cs_copy)) ||
        FAILED(ID3D11Device_CreateComputeShader(dev, g_readout_tex_cs, sizeof g_readout_tex_cs, NULL, &cs_readout_tex)) ||
        FAILED(ID3D11Device_CreateComputeShader(dev, g_clutload_cs, sizeof g_clutload_cs, NULL, &cs_clut)))
        goto fail;
    gs_gpu_on = 1;
    st_on = getenv("DOTR_GPU_STATS") != NULL;
    hw_log(2, "gpu: GS memory on the GPU (triangle pixels %s)", gpu_stepping_direct() ? "evaluated directly" : "stepped as the exact renderer");
    return 1;
fail:
    hw_log(1, "gpu: could not set up the GPU renderer; the exact renderer draws");
    gs_gpu_shutdown();
    return 0;
}

/* blocks the GPU wrote that gs_vram does not have yet (gs_gpu_sync_cpu) */
static u8 cstale[16384];

static void mark_stale(u32 lo, u32 hi)
{
    if (hi > 16383) hi = 16383;
    if (lo <= hi) memset(cstale + lo, 1, hi - lo + 1);
}

void gs_gpu_load_mem(void)
{
    if (!gs_gpu_on || !gs_vram) return;
    gs_gpu_flush();
    ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)mem, 0, NULL, gs_vram, 0, 0);
    ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)tmem, 0, NULL, gs_vram, 0, 0);
    memset(tdirty, 0, sizeof tdirty);
    memset(cstale, 0, sizeof cstale);
    clut_prev.valid = 0;
}

static void clut_touch(u32 lo, u32 hi);

static void mark_dirty(u32 lo, u32 hi)
{
    if (hi > 16383) hi = 16383;
    if (lo <= hi) memset(tdirty + lo, 1, hi - lo + 1);
    clut_touch(lo, hi);
}

/* Bring the texture copy of blocks lo..hi up to date: what the queued primitives draw lands first. */
static void tmem_sync(u32 lo, u32 hi)
{
    u32 b = lo;
    if (hi > 16383) hi = 16383;
    while (b <= hi) {
        u32 e;
        D3D11_BOX box;
        if (!tdirty[b]) { b++; continue; }
        for (e = b; e + 1 <= hi && tdirty[e + 1]; e++) ;
        gs_gpu_flush();
        {
        ST_BEGIN;
        box.left = b * 256; box.right = (e + 1) * 256; box.top = 0; box.bottom = 1; box.front = 0; box.back = 1;
        ID3D11DeviceContext_CopySubresourceRegion(ctx, (ID3D11Resource *)tmem, 0, b * 256, 0, 0, (ID3D11Resource *)mem, 0, &box);
        ST_END(ST_TSYNC);
        }
        memset(tdirty + b, 0, e - b + 1);
        b = e + 1;
    }
}

static int batch_writes(u32 lo, u32 hi);
static void clut_touch(u32 lo, u32 hi);

/* CLUT versions: every load on the GPU makes the next slot (the previous one plus what this load brings), read from
 * GS memory as the GPU has it, so renderer=gpu needs no read-back of memory the GPU drew (an effect target shares
 * its pages with the game's CLUTs: three GPU stalls a frame before). The same load again over unchanged blocks keeps
 * the slot. */

void gs_gpu_clut_load(u32 cbp, u32 cpsm, u32 csm, u32 csa, u32 n, u32 cbw, u32 cou, u32 cov, u32 lo, u32 hi)
{
    struct { u32 cbp, cpsm, csm, csa, n, cbw, cou, cov, src, dst, pad0, pad1; } k;
    int same;
    ID3D11ShaderResourceView *ns = NULL;
    ID3D11UnorderedAccessView *nu = NULL;
    if (!gs_gpu_on) return;
    if (hi > 16383) hi = 16383;
    same = clut_prev.valid && clut_prev.cbp == cbp && clut_prev.cpsm == cpsm && clut_prev.csm == csm &&
           clut_prev.csa == csa && clut_prev.n == n && clut_prev.cbw == cbw && clut_prev.cou == cou && clut_prev.cov == cov;
    if (same) return;   /* unchanged since: clut_prev.valid drops when its blocks are written (clut_touch) */
    if (batch_writes(lo, hi)) gs_gpu_flush();   /* queued draws write its source: they land first */
    if (clut_used >= CLUT_SLOTS - 1) gs_gpu_flush();   /* queued primitives may still use the slot it reuses */
    {
    ST_BEGIN;
    memset(&k, 0, sizeof k);
    k.cbp = cbp; k.cpsm = cpsm; k.csm = csm; k.csa = csa; k.n = n; k.cbw = cbw; k.cou = cou; k.cov = cov;
    k.src = clut_cur; k.dst = clut_next;
    set_constants(&k, sizeof k);
    ID3D11DeviceContext_CSSetShader(ctx, cs_clut, NULL, 0);
    ID3D11DeviceContext_CSSetConstantBuffers(ctx, 0, 1, &cbuf);
    ID3D11DeviceContext_CSSetShaderResources(ctx, 0, 1, &mem_srv);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(ctx, 0, 1, &clut_uav, NULL);
    ID3D11DeviceContext_Dispatch(ctx, 4, 1, 1);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(ctx, 0, 1, &nu, NULL);
    ID3D11DeviceContext_CSSetShaderResources(ctx, 0, 1, &ns);
    ST_END(ST_CLUT);
    }
    clut_cur = clut_next;
    clut_next = (clut_next + 1) % CLUT_SLOTS;
    clut_used++;
    clut_prev.cbp = cbp; clut_prev.cpsm = cpsm; clut_prev.csm = csm; clut_prev.csa = csa; clut_prev.n = n;
    clut_prev.cbw = cbw; clut_prev.cou = cou; clut_prev.cov = cov; clut_prev.lo = lo; clut_prev.hi = hi; clut_prev.valid = 1;
}

/* a GPU write to blocks lo..hi: the last CLUT load's source may have changed */
static void clut_touch(u32 lo, u32 hi)
{
    if (clut_prev.valid && !(hi < clut_prev.lo || lo > clut_prev.hi)) clut_prev.valid = 0;
}

u32 gs_gpu_clut_slot(void) { return clut_cur; }

/* The raster-ordered writes keep primitives in order per screen pixel only. One batch therefore holds one frame/z
 * layout per memory area: a primitive that writes blocks the batch writes through another layout (base, width,
 * format; the duel's block wipe draws a 320-wide and then a 640-wide view of one buffer) starts a new batch. */
#define BW_N 16
static u64 bw_key;
static u32 bw_lo[BW_N], bw_hi[BW_N];
static int bw_n;

static int batch_writes(u32 lo, u32 hi)
{
    int j;
    if (!nbatch) return 0;
    for (j = 0; j < bw_n; j++) if (!(hi < bw_lo[j] || lo > bw_hi[j])) return 1;
    return 0;
}

void gs_gpu_add_prim(const gpu_prim *p, u32 tex_lo, u32 tex_hi, const u32 *wlo, const u32 *whi, int nw)
{
    int i, j;
    u64 key;
    if (!gs_gpu_on) return;
    if (p->tme) tmem_sync(tex_lo, tex_hi);   /* it samples memory as it is now, as the exact renderer does */
    key = (u64)p->fbp | ((u64)p->fbw << 14) | ((u64)p->fpsm << 20) | ((u64)p->zbp << 26) | ((u64)p->zpsm << 40) |
          ((u64)(p->zte && !p->zmsk) << 46);
    if (nbatch && key != bw_key)
        for (i = 0; i < nw && nbatch; i++)
            for (j = 0; j < bw_n; j++)
                if (!(whi[i] < bw_lo[j] || wlo[i] > bw_hi[j])) { gs_gpu_flush(); break; }
    if (nbatch == BATCH_MAX) gs_gpu_flush();
    if (!nbatch) bw_n = 0;
    bw_key = key;
    batch[nbatch++] = *p;
    for (i = 0; i < nw; i++) {
        mark_dirty(wlo[i], whi[i]);
        mark_stale(wlo[i], whi[i]);
        if (bw_n == BW_N) {   /* full: fold everything into the first range */
            for (j = 1; j < BW_N; j++) { if (bw_lo[j] < bw_lo[0]) bw_lo[0] = bw_lo[j]; if (bw_hi[j] > bw_hi[0]) bw_hi[0] = bw_hi[j]; }
            bw_n = 1;
        }
        bw_lo[bw_n] = wlo[i]; bw_hi[bw_n] = whi[i]; bw_n++;
    }
}

/* GS memory back to dst, giving up after ms milliseconds (a crash report must not hang on a wedged GPU): 1 = read */
int gs_gpu_read_mem_timeout(u32 *dst, unsigned ms)
{
    D3D11_QUERY_DESC qd;
    D3D11_MAPPED_SUBRESOURCE m;
    D3D11_BOX box = { 0, 0, 0, GS_MEM_BYTES, 1, 1 };
    ID3D11Query *q = NULL;
    BOOL done = FALSE;
    DWORD t0 = GetTickCount();
    if (!gs_gpu_on) return 0;
    gs_gpu_flush();
    ID3D11DeviceContext_CopySubresourceRegion(ctx, (ID3D11Resource *)mem_stage, 0, 0, 0, 0, (ID3D11Resource *)mem, 0, &box);
    qd.Query = D3D11_QUERY_EVENT; qd.MiscFlags = 0;
    if (FAILED(ID3D11Device_CreateQuery(dev, &qd, &q))) return 0;
    ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous *)q);
    ID3D11DeviceContext_Flush(ctx);
    while (ID3D11DeviceContext_GetData(ctx, (ID3D11Asynchronous *)q, &done, sizeof done, 0) == S_FALSE) {
        if (GetTickCount() - t0 > ms) { ID3D11Query_Release(q); return 0; }
        Sleep(1);
    }
    ID3D11Query_Release(q);
    if (FAILED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)mem_stage, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m))) return 0;
    memcpy(dst, m.pData, GS_MEM_BYTES);
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)mem_stage, 0);
    return 1;
}

void gs_gpu_finish(void)
{
    D3D11_QUERY_DESC qd;
    ID3D11Query *q = NULL;
    BOOL done = FALSE;
    if (!gs_gpu_on) return;
    gs_gpu_flush();
    qd.Query = D3D11_QUERY_EVENT; qd.MiscFlags = 0;
    if (FAILED(ID3D11Device_CreateQuery(dev, &qd, &q))) return;
    ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous *)q);
    ID3D11DeviceContext_Flush(ctx);
    while (ID3D11DeviceContext_GetData(ctx, (ID3D11Asynchronous *)q, &done, sizeof done, 0) == S_FALSE) YieldProcessor();
    ID3D11Query_Release(q);
}

void gs_gpu_flush(void)
{
    D3D11_MAPPED_SUBRESOURCE m;
    D3D11_VIEWPORT vp;
    ID3D11ShaderResourceView *srv[3];
    ID3D11ShaderResourceView *nsrv[3] = { NULL, NULL, NULL };
    ID3D11UnorderedAccessView *nuav = NULL;
    if (!gs_gpu_on || !nbatch) return;
    {
    ST_BEGIN;
    st_prims += (unsigned)nbatch;
    {
        D3D11_MAP mode = D3D11_MAP_WRITE_NO_OVERWRITE;
        if (!no_overwrite || !prim_mapped || prim_pos + nbatch > PRIM_RING) { mode = D3D11_MAP_WRITE_DISCARD; prim_pos = 0; }
        prim_mapped = 1;
        if (FAILED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)prim_buf, 0, mode, 0, &m))) { nbatch = 0; return; }
        memcpy((gpu_prim *)m.pData + prim_pos, batch, (size_t)nbatch * sizeof(gpu_prim));
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)prim_buf, 0);
    }
    vp.TopLeftX = 0; vp.TopLeftY = 0; vp.Width = 2048; vp.Height = 2048; vp.MinDepth = 0; vp.MaxDepth = 1;
    {
        u32 c[4] = { (u32)conservative, (u32)prim_pos, 0, 0 };
        set_constants(c, sizeof c);
        ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 1, &cbuf);
    }
    ID3D11DeviceContext_IASetInputLayout(ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(ctx, vs_draw, NULL, 0);
    ID3D11DeviceContext_VSSetShaderResources(ctx, 0, 1, &prim_srv);
    ID3D11DeviceContext_PSSetShader(ctx, ps_draw, NULL, 0);
    srv[0] = prim_srv; srv[1] = clut_srv; srv[2] = tmem_srv;
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 3, srv);
    ID3D11DeviceContext_RSSetState(ctx, rs_draw);
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
    ID3D11DeviceContext_OMSetRenderTargetsAndUnorderedAccessViews(ctx, 0, NULL, NULL, 0, 1, &mem_uav, NULL);
    ID3D11DeviceContext_Draw(ctx, (UINT)nbatch * 6, 0);
    prim_pos += nbatch;
    ID3D11DeviceContext_OMSetRenderTargetsAndUnorderedAccessViews(ctx, 0, NULL, NULL, 0, 1, &nuav, NULL);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 3, nsrv);
    ID3D11DeviceContext_VSSetShaderResources(ctx, 0, 1, nsrv);
    nbatch = 0;
    clut_used = 0;
    ST_END(ST_FLUSH);
    }
}

static void read_back(ID3D11Buffer *src, ID3D11Buffer *stage, void *dst, UINT bytes)
{
    D3D11_MAPPED_SUBRESOURCE m;
    D3D11_BOX box = { 0, 0, 0, bytes, 1, 1 };
    ID3D11DeviceContext_CopySubresourceRegion(ctx, (ID3D11Resource *)stage, 0, 0, 0, 0, (ID3D11Resource *)src, 0, &box);
    if (SUCCEEDED(map_read((ID3D11Resource *)stage, &m))) {
        memcpy(dst, m.pData, bytes);
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)stage, 0);
    }
}

/* Blocks the GPU wrote that gs_vram does not have yet (renderer=gpu: the CPU does not draw). What still reads GS
 * memory on the CPU (CLUT loads, local->local copies) brings its blocks back first: gs_gpu_sync_cpu. */

void gs_gpu_sync_cpu(u32 lo, u32 hi)
{
    u32 b;
    if (!gs_gpu_on || !gs_gpu_only || !gs_vram) return;
    if (hi > 16383) hi = 16383;
    for (b = lo; b <= hi; b++) {
        u32 e;
        D3D11_BOX box;
        D3D11_MAPPED_SUBRESOURCE m;
        if (!cstale[b]) continue;
        for (e = b; e + 1 <= hi && cstale[e + 1]; e++) ;
        gs_gpu_flush();
        box.left = b * 256; box.right = (e + 1) * 256; box.top = 0; box.bottom = 1; box.front = 0; box.back = 1;
        ID3D11DeviceContext_CopySubresourceRegion(ctx, (ID3D11Resource *)mem_stage, 0, b * 256, 0, 0, (ID3D11Resource *)mem, 0, &box);
        if (SUCCEEDED(map_read((ID3D11Resource *)mem_stage, &m))) {
            memcpy((u8 *)gs_vram + b * 256, (const u8 *)m.pData + b * 256, (e + 1 - b) * 256);
            ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)mem_stage, 0);
        }
        memset(cstale + b, 0, e - b + 1);
        b = e;
    }
}

void gs_gpu_read_mem(u32 *dst)
{
    if (!gs_gpu_on) return;
    gs_gpu_flush();
    read_back(mem, mem_stage, dst, GS_MEM_BYTES);
    if (dst == gs_vram) memset(cstale, 0, sizeof cstale);
}

static void set_constants(const void *data, UINT bytes)
{
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)cbuf, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, data, bytes);
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)cbuf, 0);
    }
}

static void unbind_cs(void)
{
    ID3D11UnorderedAccessView *nu = NULL;
    ID3D11ShaderResourceView *ns = NULL;
    ID3D11DeviceContext_CSSetUnorderedAccessViews(ctx, 0, 1, &nu, NULL);
    ID3D11DeviceContext_CSSetShaderResources(ctx, 0, 1, &ns);
}

void gs_gpu_upload(u32 dbp, u32 dbw, u32 dpsm, u32 dsax, u32 dsay, u32 rrw, u32 first, const u32 *v, u32 count)
{
    struct { u32 dbp, dbw, dpsm, rrw, dsax, dsay, first, count, base, pad0, pad1, pad2; } k;
    D3D11_MAP mode = D3D11_MAP_WRITE_NO_OVERWRITE;
    D3D11_MAPPED_SUBRESOURCE m;
    if (!gs_gpu_on || !count || !rrw) return;
    gs_gpu_flush();
    {
    ST_BEGIN;
    {   /* the rows this part of the transfer writes */
        u32 lo = 0, hi = 16383, r0 = first / rrw, r1 = (first + count - 1) / rrw;
        if (dsax + rrw <= 2048 && dsay + r1 < 2048)
            gs_rect_blocks(dpsm, dbp, dbw, dsax, dsay + r0, dsax + rrw - 1, dsay + r1, &lo, &hi);
        mark_dirty(lo, hi);
    }
    if (count > VALS_RING) {   /* more than the ring: in pieces */
        u32 done;
        for (done = 0; done < count; done += VALS_RING)
            gs_gpu_upload(dbp, dbw, dpsm, dsax, dsay, rrw, first + done, v + done, count - done < VALS_RING ? count - done : VALS_RING);
        return;
    }
    if (!vals_cap) {
        u32 cap = VALS_RING;
        D3D11_SHADER_RESOURCE_VIEW_DESC s;
        RELEASE(vals_srv, ID3D11ShaderResourceView); RELEASE(vals, ID3D11Buffer); vals_cap = 0;
        vals = make_buffer(cap * 4, D3D11_BIND_SHADER_RESOURCE, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, D3D11_USAGE_DYNAMIC,
                           D3D11_CPU_ACCESS_WRITE, 4);
        if (!vals) return;
        memset(&s, 0, sizeof s);
        s.Format = DXGI_FORMAT_UNKNOWN; s.ViewDimension = D3D11_SRV_DIMENSION_BUFFER; s.Buffer.NumElements = cap;
        if (FAILED(ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)vals, &s, &vals_srv))) return;
        vals_cap = cap;
    }
    if (!no_overwrite || !vals_mapped || vals_pos + count > vals_cap) { mode = D3D11_MAP_WRITE_DISCARD; vals_pos = 0; }
    vals_mapped = 1;
    if (FAILED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)vals, 0, mode, 0, &m))) return;
    memcpy((u32 *)m.pData + vals_pos, v, count * 4);
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)vals, 0);
    memset(&k, 0, sizeof k);
    k.dbp = dbp; k.dbw = dbw; k.dpsm = dpsm; k.rrw = rrw; k.dsax = dsax; k.dsay = dsay; k.first = first; k.count = count;
    k.base = vals_pos;
    vals_pos += count;
    set_constants(&k, sizeof k);
    ID3D11DeviceContext_CSSetShader(ctx, cs_upload, NULL, 0);
    ID3D11DeviceContext_CSSetConstantBuffers(ctx, 0, 1, &cbuf);
    ID3D11DeviceContext_CSSetShaderResources(ctx, 0, 1, &vals_srv);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(ctx, 0, 1, &mem_uav, NULL);
    ID3D11DeviceContext_Dispatch(ctx, (count + 63) / 64, 1, 1);
    unbind_cs();
    ST_END(ST_UPLOAD);
    }
}

void gs_gpu_copy(u32 sbp, u32 sbw, u32 spsm, u32 ssax, u32 ssay, u32 dbp, u32 dbw, u32 dpsm, u32 dsax, u32 dsay,
                 u32 dir, u32 rrw, u32 rrh)
{
    struct { u32 sbp, sbw, spsm, ssax, ssay, dbp, dbw, dpsm, dsax, dsay, dir, rrw, rrh, serial, pad0, pad1; } k;
    u32 slo, shi, dlo, dhi, n = rrw * rrh;
    if (!gs_gpu_on || !n) return;
    gs_gpu_flush();
    /* overlapping source and destination: the CPU's pixel order matters, so one thread walks it */
    gs_rect_blocks(spsm, sbp, sbw, ssax, ssay, ssax + rrw - 1, ssay + rrh - 1, &slo, &shi);
    gs_rect_blocks(dpsm, dbp, dbw, dsax, dsay, dsax + rrw - 1, dsay + rrh - 1, &dlo, &dhi);
    mark_dirty(dlo, dhi);
    mark_stale(dlo, dhi);
    memset(&k, 0, sizeof k);
    k.sbp = sbp; k.sbw = sbw; k.spsm = spsm; k.ssax = ssax; k.ssay = ssay; k.dbp = dbp; k.dbw = dbw; k.dpsm = dpsm;
    k.dsax = dsax; k.dsay = dsay; k.dir = dir; k.rrw = rrw; k.rrh = rrh;
    k.serial = !(shi < dlo || slo > dhi) || ssax + rrw > 2048 || ssay + rrh > 2048 || dsax + rrw > 2048 || dsay + rrh > 2048;
    set_constants(&k, sizeof k);
    ID3D11DeviceContext_CSSetShader(ctx, cs_copy, NULL, 0);
    ID3D11DeviceContext_CSSetConstantBuffers(ctx, 0, 1, &cbuf);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(ctx, 0, 1, &mem_uav, NULL);
    ID3D11DeviceContext_Dispatch(ctx, k.serial ? 1 : (n + 63) / 64, 1, 1);
    unbind_cs();
    st_n[ST_COPY]++;
}

#define BITS(v, lo, n) ((u32)(((v) >> (lo)) & ((1ull << (n)) - 1)))

void *gs_gpu_d3d_device(void) { return gs_gpu_on ? (void *)dev : NULL; }

/* the constants readout.hlsl takes, from the PCRTC registers (as gs_display.c's circ_get) */
struct pcrtc_k { struct { s32 on, fbp, fbw, psm, dbx, dby, dx, dy, magh, magv, dw, dh; } c[2]; s32 ref1, fieldm, mmod, slbg, alp, w, h; u32 bg; };
static void pcrtc_constants(struct pcrtc_k *k, int w, int h);

void *gs_gpu_picture(int w, int h)
{
    struct pcrtc_k k;
    if (!gs_gpu_on || w <= 0 || h <= 0 || w > 1024 || h > 1024) return NULL;
    if (!pic_tex) {
        D3D11_TEXTURE2D_DESC t;
        D3D11_UNORDERED_ACCESS_VIEW_DESC u;
        memset(&t, 0, sizeof t);
        t.Width = 1024; t.Height = 1024; t.MipLevels = 1; t.ArraySize = 1; t.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
        t.SampleDesc.Count = 1; t.Usage = D3D11_USAGE_DEFAULT; t.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        if (FAILED(ID3D11Device_CreateTexture2D(dev, &t, NULL, &pic_tex))) return NULL;
        memset(&u, 0, sizeof u);
        u.Format = DXGI_FORMAT_R8G8B8A8_UINT; u.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        if (FAILED(ID3D11Device_CreateUnorderedAccessView(dev, (ID3D11Resource *)pic_tex, &u, &pic_tex_uav))) {
            RELEASE(pic_tex, ID3D11Texture2D);
            return NULL;
        }
    }
    gs_gpu_flush();
    pcrtc_constants(&k, w, h);
    set_constants(&k, sizeof k);
    ID3D11DeviceContext_CSSetShader(ctx, cs_readout_tex, NULL, 0);
    ID3D11DeviceContext_CSSetConstantBuffers(ctx, 0, 1, &cbuf);
    ID3D11DeviceContext_CSSetShaderResources(ctx, 0, 1, &mem_srv);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(ctx, 0, 1, &pic_tex_uav, NULL);
    ID3D11DeviceContext_Dispatch(ctx, (UINT)(w + 7) / 8, (UINT)(h + 7) / 8, 1);
    unbind_cs();
    return pic_tex;
}

int gs_gpu_picture_read(u32 *rgba, int w, int h)
{
    D3D11_TEXTURE2D_DESC t;
    ID3D11Texture2D *st = NULL;
    D3D11_MAPPED_SUBRESOURCE m;
    int y, ok = 0;
    ID3D11Texture2D *bb = NULL;
    D3D11_BOX box = { 0, 0, 0, 0, 0, 1 };
    if (!pic_tex || w <= 0 || h <= 0) return 0;
    /* through a texture shaped like the window's back buffer (R8G8B8A8_UNORM), with the copy the window makes */
    ID3D11Texture2D_GetDesc(pic_tex, &t);
    t.Format = DXGI_FORMAT_R8G8B8A8_UNORM; t.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(ID3D11Device_CreateTexture2D(dev, &t, NULL, &bb))) return 0;
    box.right = (UINT)w; box.bottom = (UINT)h;
    ID3D11DeviceContext_CopySubresourceRegion(ctx, (ID3D11Resource *)bb, 0, 0, 0, 0, (ID3D11Resource *)pic_tex, 0, &box);
    t.Usage = D3D11_USAGE_STAGING; t.BindFlags = 0; t.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(ID3D11Device_CreateTexture2D(dev, &t, NULL, &st))) { ID3D11Texture2D_Release(bb); return 0; }
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)st, (ID3D11Resource *)bb);
    ID3D11Texture2D_Release(bb);
    if (SUCCEEDED(map_read((ID3D11Resource *)st, &m))) {
        for (y = 0; y < h; y++) memcpy(rgba + y * w, (const u8 *)m.pData + (size_t)y * m.RowPitch, (size_t)w * 4);
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)st, 0);
        ok = 1;
    }
    ID3D11Texture2D_Release(st);
    return ok;
}

static void pcrtc_constants(struct pcrtc_k *k, int w, int h)
{
    int i;
    memset(k, 0, sizeof *k);
    for (i = 0; i < 2; i++) {
        u64 fb = gs.dispfb[i], d = gs.display[i];
        k->c[i].on = (s32)((gs.pmode >> i) & 1);
        k->c[i].fbp = (s32)(BITS(fb, 0, 9) * 32); k->c[i].fbw = (s32)BITS(fb, 9, 6); k->c[i].psm = (s32)BITS(fb, 15, 5);
        k->c[i].dbx = (s32)BITS(fb, 32, 11); k->c[i].dby = (s32)BITS(fb, 43, 11);
        k->c[i].dx = (s32)BITS(d, 0, 12); k->c[i].dy = (s32)BITS(d, 12, 11);
        k->c[i].magh = (s32)BITS(d, 23, 4) + 1; k->c[i].magv = (s32)BITS(d, 27, 2) + 1;
        k->c[i].dw = (s32)BITS(d, 32, 12) + 1; k->c[i].dh = (s32)BITS(d, 44, 11) + 1;
    }
    k->ref1 = !k->c[0].on || (k->c[1].on && (k->c[1].dx < k->c[0].dx || k->c[1].dy < k->c[0].dy));
    k->fieldm = (gs.smode2 & 1) && (gs.smode2 & 2);
    k->mmod = (s32)((gs.pmode >> 5) & 1); k->slbg = (s32)((gs.pmode >> 7) & 1); k->alp = (s32)BITS(gs.pmode, 8, 8);
    k->w = w; k->h = h; k->bg = (u32)(gs.bgcolor & 0xFFFFFF);
}

void gs_gpu_readout(u32 *rgba, int w, int h)
{
    struct circ { s32 on, fbp, fbw, psm, dbx, dby, dx, dy, magh, magv, dw, dh; };
    struct { struct circ c[2]; s32 ref1, fieldm, mmod, slbg, alp, w, h; u32 bg; } k;
    int i;
    if (!gs_gpu_on || w <= 0 || h <= 0 || w * h > PIC_MAX) return;
    gs_gpu_flush();
    {
    ST_BEGIN;
    memset(&k, 0, sizeof k);
    for (i = 0; i < 2; i++) {   /* as gs_display.c's circ_get */
        u64 fb = gs.dispfb[i], d = gs.display[i];
        struct circ *c = &k.c[i];
        c->on = (s32)((gs.pmode >> i) & 1);
        c->fbp = (s32)(BITS(fb, 0, 9) * 32); c->fbw = (s32)BITS(fb, 9, 6); c->psm = (s32)BITS(fb, 15, 5);
        c->dbx = (s32)BITS(fb, 32, 11); c->dby = (s32)BITS(fb, 43, 11);
        c->dx = (s32)BITS(d, 0, 12); c->dy = (s32)BITS(d, 12, 11);
        c->magh = (s32)BITS(d, 23, 4) + 1; c->magv = (s32)BITS(d, 27, 2) + 1;
        c->dw = (s32)BITS(d, 32, 12) + 1; c->dh = (s32)BITS(d, 44, 11) + 1;
    }
    k.ref1 = !k.c[0].on || (k.c[1].on && (k.c[1].dx < k.c[0].dx || k.c[1].dy < k.c[0].dy));
    k.fieldm = (gs.smode2 & 1) && (gs.smode2 & 2);
    k.mmod = (s32)((gs.pmode >> 5) & 1); k.slbg = (s32)((gs.pmode >> 7) & 1); k.alp = (s32)BITS(gs.pmode, 8, 8);
    k.w = w; k.h = h; k.bg = (u32)(gs.bgcolor & 0xFFFFFF);
    set_constants(&k, sizeof k);
    ID3D11DeviceContext_CSSetShader(ctx, cs_readout, NULL, 0);
    ID3D11DeviceContext_CSSetConstantBuffers(ctx, 0, 1, &cbuf);
    ID3D11DeviceContext_CSSetShaderResources(ctx, 0, 1, &mem_srv);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(ctx, 0, 1, &pic_uav, NULL);
    ID3D11DeviceContext_Dispatch(ctx, (UINT)(w + 7) / 8, (UINT)(h + 7) / 8, 1);
    unbind_cs();
    read_back(pic, pic_stage, rgba, (UINT)(w * h * 4));
    ST_END(ST_READOUT);
    }
}
