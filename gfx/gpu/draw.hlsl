// draw.hlsl - GS primitives on the GPU, to the exact renderer's results (gs_draw.c). The GPU rasterizes each
// primitive's bounding rectangle; the pixel shader decides coverage with the same integer edge functions, rebuilds
// the attribute values in the same arithmetic order as tri_rows/sprite_rows, and runs put_pixel against GS
// memory through a rasterizer ordered view, so pixels land in primitive order as on the GS.
#include "gs_mem.hlsli"

struct Vtx { int x, y; uint z; int r, g, b, a, f; float s, t, q; uint u, v; };
struct Prim {
    int kind, x0, y0, x1, y1;
    uint fbp, fbw, fpsm, fbmsk, zbp, zpsm, zmsk, zmax;
    int ate, atst, aref, afail, date, datm, zte, ztst;
    int abe, a, b, c, d, fix, pabe, fba, colclamp, dthe;
    uint dimx_lo, dimx_hi;
    int tme, fge, iip, fst, tfx, tcc, wms, wmt, minu, maxu, minv, maxv, mmag;
    uint tbp, tbw, tpsm, tw, th, cpsm, csa, ta0, ta1, aem, fogr, fogg, fogb, clut_slot, scanmsk, pad;
    Vtx v0, v1, v2, vflat;
    uint inv_lo, inv_hi, zstep_lo, zstep_hi;
    float fstep[10];
    int tl0, tl1, tl2, d0, d1, d2, nf;
    int spx0, spx1, sy0, sy1;
    float st0, st1, sdu, stx0;
    int mxl, lcm, lodk, lodl, min_lin;
    uint mtbp[6], mtbw[6];
};

StructuredBuffer<Prim> prims : register(t0);
StructuredBuffer<uint> cluts : register(t1);   // 256 words per slot: gs.clut's 512 halfwords
ByteAddressBuffer tmem : register(t2);         // GS memory as textures see it (copied before they are drawn from)
RasterizerOrderedByteAddressBuffer mem : register(u0);

struct VSOut { float4 pos : SV_Position; nointerpolation uint prim : PRIM; };

cbuffer drawc : register(b0) { uint conservative, prim_base; };   // conservative: every touched pixel; prim_base: the batch in the ring

// Six vertices per primitive: the bounding rectangle as two triangles. With conservative rasterization a pixel
// on the edge two triangles share would come twice, so each primitive is one triangle there instead: a GS
// triangle as itself (its sample points, the pixel corners, put at the pixel centres), a rectangle as a triangle
// twice its size (the shader keeps to the rectangle), and the other three vertices go off-screen.
VSOut vs_main(uint vid : SV_VertexID)
{
    VSOut o;
    uint p = prim_base + vid / 6, k = vid % 6;
    int x0 = prims[p].x0, y0 = prims[p].y0, x1 = prims[p].x1, y1 = prims[p].y1;
    float fx, fy;
    if (conservative) {
        if (k >= 3 || x1 <= x0 || y1 <= y0) { fx = -8192.0f; fy = -8192.0f; }
        else if (prims[p].kind == 0) {
            int vx = prims[p].v0.x, vy = prims[p].v0.y;
            if (k == 1) { vx = prims[p].v1.x; vy = prims[p].v1.y; }
            if (k == 2) { vx = prims[p].v2.x; vy = prims[p].v2.y; }
            fx = (float)vx / 16.0f + 0.5f; fy = (float)vy / 16.0f + 0.5f;
        } else {
            fx = (float)(k == 1 ? x0 + 2 * (x1 - x0) : x0);
            fy = (float)(k == 2 ? y0 + 2 * (y1 - y0) : y0);
        }
    } else {
        int x = (k == 1 || k == 4 || k == 5) ? x1 : x0;
        int y = (k == 2 || k == 3 || k == 5) ? y1 : y0;
        if (x1 <= x0 || y1 <= y0) { x = x0; y = y0; }
        fx = (float)x; fy = (float)y;
    }
    o.pos = float4(fx / 1024.0f - 1.0f, 1.0f - fy / 1024.0f, 0.0f, 1.0f);
    o.prim = p;
    return o;
}

// ---- frame / z buffer words (buf_get / buf_put: raw 32-bit words, or 16-bit halfwords) ----------------------
bool is16(uint psm) { return psm == PSMCT16 || psm == PSMCT16S || psm == PSMZ16 || psm == PSMZ16S; }

uint buf_get(uint psm, uint bp, uint bw, int x, int y)
{
    uint xx = (uint)x & 2047, yy = (uint)y & 2047, a;
    if (is16(psm)) {
        a = gs_addr16x(bp, bw, xx, yy, (psm & 8) ? 1u : 0u, (psm & 0x30) == 0x30 ? 24u : 0u);
        return (mem.Load((a >> 1) * 4) >> ((a & 1) * 16)) & 0xFFFF;
    }
    a = ((psm & 0x30) == 0x30) ? gs_addr32z(bp, bw, xx, yy) : gs_addr32(bp, bw, xx, yy);
    return mem.Load(a * 4);
}

void buf_put(uint psm, uint bp, uint bw, int x, int y, uint v)
{
    uint xx = (uint)x & 2047, yy = (uint)y & 2047, a, old;
    if (is16(psm)) {
        a = gs_addr16x(bp, bw, xx, yy, (psm & 8) ? 1u : 0u, (psm & 0x30) == 0x30 ? 24u : 0u);
        mem.InterlockedAnd((a >> 1) * 4, ~(0xFFFFu << ((a & 1) * 16)), old);
        mem.InterlockedOr((a >> 1) * 4, (v & 0xFFFF) << ((a & 1) * 16), old);
        return;
    }
    a = ((psm & 0x30) == 0x30) ? gs_addr32z(bp, bw, xx, yy) : gs_addr32(bp, bw, xx, yy);
    mem.Store(a * 4, v);
}

// ---- textures (texel_slow over the texture copy of memory) ----------------------------------------------------
uint tread(uint psm, uint bp, uint bw, uint x, uint y)   // gs_read_pixel
{
    uint word, shift, mask;
    gs_locate(psm, bp, bw, x, y, word, shift, mask);
    return (tmem.Load(word * 4) >> shift) & mask;
}

uint expand16(Prim p, uint c)
{
    uint r = (c & 0x1F) << 3, g = ((c >> 5) & 0x1F) << 3, b = ((c >> 10) & 0x1F) << 3, a;
    if (c & 0x8000) a = p.ta1;
    else a = (p.aem && !(c & 0x7FFF)) ? 0 : p.ta0;
    return r | (g << 8) | (b << 16) | (a << 24);
}

uint clut16(uint slot, uint i) { return (cluts[slot * 256 + (i >> 1)] >> ((i & 1) * 16)) & 0xFFFF; }

uint clut_lookup(Prim p, uint idx, bool is4)
{
    if (p.cpsm == PSMCT16 || p.cpsm == PSMCT16S) {
        uint i = is4 ? (p.csa * 16 + idx) & 511 : idx & 255;
        return expand16(p, clut16(p.clut_slot, i));
    } else {
        uint i = is4 ? ((p.csa & 15) * 16 + idx) & 255 : idx & 255;
        return clut16(p.clut_slot, i) | (clut16(p.clut_slot, i + 256) << 16);
    }
}

uint texel_at(Prim p, uint tbp, uint tbw, int x, int y)
{
    uint v, xx = (uint)x, yy = (uint)y;
    switch (p.tpsm) {
    case PSMCT32: case PSMZ32:
        return tread(p.tpsm, tbp, tbw, xx, yy);
    case PSMCT24: case PSMZ24:
        v = tread(p.tpsm, tbp, tbw, xx, yy);
        return v | (((p.aem && !(v & 0xFFFFFF)) ? 0 : p.ta0) << 24);
    case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S:
        return expand16(p, tread(p.tpsm, tbp, tbw, xx, yy));
    case PSMT8: case PSMT8H:
        return clut_lookup(p, tread(p.tpsm, tbp, tbw, xx, yy), false);
    case PSMT4: case PSMT4HL: case PSMT4HH:
        return clut_lookup(p, tread(p.tpsm, tbp, tbw, xx, yy), true);
    default:
        return tread(PSMCT32, tbp, tbw, xx, yy);
    }
}

uint texel(Prim p, int x, int y) { return texel_at(p, p.tbp, p.tbw, x, y); }

int wrapc(int c, int size, int mode, int mn, int mx)
{
    if (mode == 0) return c & (size - 1);
    if (mode == 1) return c < 0 ? 0 : c >= size ? size - 1 : c;
    if (mode == 2) return c < mn ? mn : c > mx ? mx : c;
    return (c & mn) | mx;
}

// sample(): lod -1 magnifies (MMAG), 0 and up minify (MMIN linear or not) from that mip level (gs_draw.c)
uint sample_tex(Prim p, precise float u, precise float v, int lod)
{
    int w = 1 << p.tw, h = 1 << p.th;
    uint tbp = p.tbp, tbw = p.tbw;
    bool lin = lod < 0 ? p.mmag != 0 : p.min_lin != 0;
    if (lod > 0) {
        int l = min(lod, 6);
        tbp = p.mtbp[l - 1]; tbw = p.mtbw[l - 1];
        w = max(w >> l, 1); h = max(h >> l, 1);
        u = u / (float)(1 << lod); v = v / (float)(1 << lod);
    }
    if (!lin) {
        int x = wrapc((int)floor(u), w, p.wms, p.minu, p.maxu);
        int y = wrapc((int)floor(v), h, p.wmt, p.minv, p.maxv);
        return texel_at(p, tbp, tbw, x, y);
    } else {
        precise float fu = u - 0.5f, fv = v - 0.5f;
        int x0 = (int)floor(fu), y0 = (int)floor(fv);
        precise float ru = (fu - (float)x0) * 16.0f, rv = (fv - (float)y0) * 16.0f;
        int au = (int)ru, av = (int)rv;
        int xa = wrapc(x0, w, p.wms, p.minu, p.maxu), xb = wrapc(x0 + 1, w, p.wms, p.minu, p.maxu);
        int ya = wrapc(y0, h, p.wmt, p.minv, p.maxv), yb = wrapc(y0 + 1, h, p.wmt, p.minv, p.maxv);
        uint t00 = texel_at(p, tbp, tbw, xa, ya), t10 = texel_at(p, tbp, tbw, xb, ya);
        uint t01 = texel_at(p, tbp, tbw, xa, yb), t11 = texel_at(p, tbp, tbw, xb, yb);
        uint outv = 0;
        int k;
        for (k = 0; k < 32; k += 8) {
            int c00 = (t00 >> k) & 0xFF, c10 = (t10 >> k) & 0xFF, c01 = (t01 >> k) & 0xFF, c11 = (t11 >> k) & 0xFF;
            int top = c00 * (16 - au) + c10 * au, bot = c01 * (16 - au) + c11 * au;
            int c = (top * (16 - av) + bot * av) >> 8;
            outv |= (uint)c << k;
        }
        return outv;
    }
}

// float a / b correctly rounded, as the CPU's SSE divide: double division then rounding to float is exact for floats
float fdiv(float a, float b) { precise double q = (double)a / (double)b; return (float)q; }

// ---- the pixel pipeline (put_pixel) -----------------------------------------------------------------------------
int clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

int dimx(Prim p, int x, int y)
{
    uint i = (uint)((y & 3) * 4 + (x & 3)), w = i < 8 ? p.dimx_lo : p.dimx_hi, n = (w >> ((i & 7) * 4)) & 0xF;
    return n >= 8 ? (int)n - 16 : (int)n;
}

// the mip level tex_coords picks (gs_draw.c), log2 as lod_log2
float lod_log2(float q)
{
    uint u = asuint(q) & 0x7FFFFFFFu;
    precise float e = (float)((int)(u >> 23) - 127), m = (float)(u & 0x7FFFFFu) * (1.0f / 8388608.0f);
    precise float r = e + m;
    return r;
}

int tri_lod(Prim p, float q)
{
    if (p.fst) {
        int lod = p.mxl == 0 ? -1 : 0;
        if (p.mxl && p.lcm) { lod = (p.lodk + 8) >> 4; lod = lod <= 0 ? -1 : (lod > p.mxl ? p.mxl : lod); }
        return lod;
    }
    if (p.mxl) {
        precise float qq = q == 0.0f ? 1.0f : q;
        precise float lodf;
        if (p.lcm) lodf = (float)p.lodk / 16.0f;
        else lodf = -lod_log2(qq) * (float)(1 << p.lodl) + (float)p.lodk / 16.0f;
        if (lodf > 0.0f) { int l = (int)(lodf + 0.5f); return l > p.mxl ? p.mxl : l; }
    }
    return -1;
}

void put_pixel(Prim p, int x, int y, int r, int g, int b, int a, uint z, int f, float u, float v, int lod)
{
    int write_fb = 1, write_z = !p.zmsk, write_rgb_only = 0, ad = 0;
    bool fb16;
    uint zaddr_v = 0;
    if ((p.scanmsk & 3) == 2 && !(y & 1)) return;
    if ((p.scanmsk & 3) == 3 && (y & 1)) return;
    if (p.tme) {
        uint t = sample_tex(p, u, v, lod);
        int tr = t & 0xFF, tg = (t >> 8) & 0xFF, tb = (t >> 16) & 0xFF, ta = t >> 24;
        if (p.tfx == 0) {
            r = clamp255((r * tr) >> 7); g = clamp255((g * tg) >> 7); b = clamp255((b * tb) >> 7);
            a = p.tcc ? clamp255((a * ta) >> 7) : a;
        } else if (p.tfx == 1) {
            r = tr; g = tg; b = tb; a = p.tcc ? ta : a;
        } else if (p.tfx == 2) {
            r = clamp255(((r * tr) >> 7) + a); g = clamp255(((g * tg) >> 7) + a); b = clamp255(((b * tb) >> 7) + a);
            a = p.tcc ? clamp255(ta + a) : a;
        } else {
            r = clamp255(((r * tr) >> 7) + a); g = clamp255(((g * tg) >> 7) + a); b = clamp255(((b * tb) >> 7) + a);
            a = p.tcc ? ta : a;
        }
    }
    if (p.fge) {
        r = (f * r + (255 - f) * (int)p.fogr) >> 8;
        g = (f * g + (255 - f) * (int)p.fogg) >> 8;
        b = (f * b + (255 - f) * (int)p.fogb) >> 8;
    }
    if (p.ate) {
        bool passed;
        switch (p.atst) {
        case 0: passed = false; break;
        case 1: passed = true; break;
        case 2: passed = a < p.aref; break;
        case 3: passed = a <= p.aref; break;
        case 4: passed = a == p.aref; break;
        case 5: passed = a >= p.aref; break;
        case 6: passed = a > p.aref; break;
        default: passed = a != p.aref; break;
        }
        if (!passed) {
            if (p.afail == 0) return;
            if (p.afail == 1) write_z = 0;
            else if (p.afail == 2) write_fb = 0;
            else { write_z = 0; write_rgb_only = 1; }
        }
    }
    fb16 = is16(p.fpsm);
    if (p.date && p.fpsm != PSMCT24 && p.fpsm != PSMZ24) {
        uint dv = buf_get(p.fpsm, p.fbp, p.fbw, x, y);
        int bit = fb16 ? (int)((dv >> 15) & 1) : (int)((dv >> 31) & 1);
        if (bit != p.datm) return;
    }
    if (z > p.zmax) z = p.zmax;
    if (p.zte) {
        if (p.ztst == 0) return;
        if (p.ztst >= 2) {
            zaddr_v = buf_get(p.zpsm, p.zbp, p.fbw, x, y);
            if (p.zpsm == PSMZ24) zaddr_v &= 0xFFFFFF;
            if (p.ztst == 2 ? z < zaddr_v : z <= zaddr_v) return;
        }
    } else {
        write_z = 0;
    }
    if (p.abe && !(p.pabe && !(a & 0x80))) {
        uint dcol, d = buf_get(p.fpsm, p.fbp, p.fbw, x, y);
        int cs[3], cd[3], outc[3], k, cc;
        if (fb16) {   // fb_read
            ad = (d & 0x8000) ? 0x80 : 0;
            dcol = ((d & 0x1F) << 3) | (((d >> 5) & 0x1F) << 11) | (((d >> 10) & 0x1F) << 19);
        } else if (p.fpsm == PSMCT24 || p.fpsm == PSMZ24) {
            ad = 0x80; dcol = d & 0xFFFFFF;
        } else {
            ad = (int)(d >> 24); dcol = d & 0xFFFFFF;
        }
        cs[0] = r; cs[1] = g; cs[2] = b;
        cd[0] = dcol & 0xFF; cd[1] = (dcol >> 8) & 0xFF; cd[2] = (dcol >> 16) & 0xFF;
        cc = p.c == 0 ? a : p.c == 1 ? ad : p.fix;
        for (k = 0; k < 3; k++) {
            int va = p.a == 0 ? cs[k] : p.a == 1 ? cd[k] : 0;
            int vb = p.b == 0 ? cs[k] : p.b == 1 ? cd[k] : 0;
            int vd = p.d == 0 ? cs[k] : p.d == 1 ? cd[k] : 0;
            outc[k] = (((va - vb) * cc) >> 7) + vd;
        }
        r = outc[0]; g = outc[1]; b = outc[2];
    }
    if (p.dthe && fb16) {
        int dv = dimx(p, x, y);
        r += dv; g += dv; b += dv;
    }
    if (p.colclamp) { r = clamp255(r); g = clamp255(g); b = clamp255(b); }
    else { r &= 0xFF; g &= 0xFF; b &= 0xFF; }
    if (p.fba) a |= 0x80;
    if (write_fb) {
        uint msk = p.fbmsk, outv, old;
        if (write_rgb_only) msk |= 0xFF000000u;
        if (fb16) {
            uint m16 = ((msk >> 3) & 0x1F) | (((msk >> 11) & 0x1F) << 5) | (((msk >> 19) & 0x1F) << 10) | ((msk >> 31) << 15);
            outv = (uint)((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10) | ((a & 0x80) << 8));
            if (m16) {
                old = buf_get(p.fpsm, p.fbp, p.fbw, x, y);
                outv = (outv & ~m16) | (old & m16);
            }
        } else {
            outv = (uint)r | ((uint)g << 8) | ((uint)b << 16) | ((uint)(a & 0xFF) << 24);
            if (p.fpsm == PSMCT24 || p.fpsm == PSMZ24) msk |= 0xFF000000u;
            if (msk) {
                old = buf_get(p.fpsm, p.fbp, p.fbw, x, y);
                outv = (outv & ~msk) | (old & msk);
            }
        }
        buf_put(p.fpsm, p.fbp, p.fbw, x, y, outv);
    }
    if (write_z) {
        if (p.zpsm == PSMZ24) z = (buf_get(p.zpsm, p.zbp, p.fbw, x, y) & 0xFF000000u) | (z & 0xFFFFFF);
        buf_put(p.zpsm, p.zbp, p.fbw, x, y, z);
    }
}

// ---- triangles: tri_rows -----------------------------------------------------------------------------------------
// floor(a / b) for b > 0, exact: a double quotient corrected with exact products (all values are integers < 2^53)
double floor_div(double a, double b)
{
    precise double q = a / b;
    precise double r;
    int qi;
    q = clamp(q, -1.0e6, 1.0e6);
    qi = (int)q;
    r = a - (double)qi * b;
    while (r < 0.0) { qi--; r += b; }
    while (r >= b) { qi++; r -= b; }
    return (double)qi;
}

void edge_span(double w, double d, int tl, inout double lo, inout double hi)
{
    double t = tl ? 0.0 : 1.0;
    if (d == 0.0) { if (w < t) hi = lo - 1.0; return; }
    if (d > 0.0) { double n = -floor_div(w - t, d); if (n > lo) lo = n; }
    else { double n = floor_div(w - t, -d); if (n < hi) hi = n; }
}

void attrs(Vtx v, out float fa[10])
{
    fa[0] = (float)v.r; fa[1] = (float)v.g; fa[2] = (float)v.b; fa[3] = (float)v.a; fa[4] = (float)v.f;
    fa[5] = v.s; fa[6] = v.t; fa[7] = v.q; fa[8] = (float)v.u; fa[9] = (float)v.v;
}

#ifdef CLOSED_FORM
// Each pixel's values straight from the edge functions at that pixel (the formula tri_rows uses for a row's first
// pixel), with no stepping along the row.
void tri_pixel(Prim p, int x, int y)
{
    Vtx A = p.v0, B = p.v1, C = p.v2;
    precise double inv = asdouble(p.inv_lo, p.inv_hi);
    precise double py = (double)(y * 16), px = (double)(x * 16);
    precise double w0 = (double)(C.x - B.x) * (py - (double)B.y) - (double)(C.y - B.y) * (px - (double)B.x);
    precise double w1 = (double)(A.x - C.x) * (py - (double)C.y) - (double)(A.y - C.y) * (px - (double)C.x);
    precise double w2 = (double)(B.x - A.x) * (py - (double)A.y) - (double)(B.y - A.y) * (px - (double)A.x);
    float fa[10], fb[10], fc[10];
    precise float fcur[10];
    precise double zcur;
    uint zp;
    int k;
    if (x < p.x0 || x >= p.x1 || y < p.y0 || y >= p.y1) discard;   // setup_tri's box, clipped to the scissor
    if (w0 < 0.0 || w1 < 0.0 || w2 < 0.0 || (w0 == 0.0 && !p.tl0) || (w1 == 0.0 && !p.tl1) || (w2 == 0.0 && !p.tl2)) discard;
    attrs(A, fa); attrs(B, fb); attrs(C, fc);
    zcur = (w0 * (double)A.z + w1 * (double)B.z + w2 * (double)C.z) * inv;
    for (k = 0; k < p.nf; k++) {
        precise double sv = (w0 * (double)fa[k] + w1 * (double)fb[k] + w2 * (double)fc[k]) * inv;
        fcur[k] = (float)sv;
    }
    {
        int r, g, b, a, f;
        float u = 0, v = 0;
        if (p.iip) { r = (int)fcur[0]; g = (int)fcur[1]; b = (int)fcur[2]; a = (int)fcur[3]; }
        else { r = p.vflat.r; g = p.vflat.g; b = p.vflat.b; a = p.vflat.a; }
        zp = zcur >= 4294967295.0 ? 0xFFFFFFFFu : zcur <= 0.0 ? 0u : (uint)zcur;
        f = (int)fcur[4];
        if (p.tme) {
            if (p.fst) { u = fcur[8] / 16.0f; v = fcur[9] / 16.0f; }
            else {
                precise float qq = fcur[7] == 0.0f ? 1.0f : fcur[7];
                precise float rq = fdiv(1.0f, qq);
                u = fcur[5] * rq * (float)(1 << p.tw);
                v = fcur[6] * rq * (float)(1 << p.th);
            }
        }
        put_pixel(p, x, y, r, g, b, a, zp, f, u, v, p.tme ? tri_lod(p, fcur[7]) : -1);
    }
}
#else
void tri_pixel(Prim p, int x, int y)
{
    Vtx A = p.v0, B = p.v1, C = p.v2;
    precise double inv = asdouble(p.inv_lo, p.inv_hi), zstep = asdouble(p.zstep_lo, p.zstep_hi);
    precise double py = (double)(y * 16), px0 = (double)(p.x0 * 16);
    precise double w0 = (double)(C.x - B.x) * (py - (double)B.y) - (double)(C.y - B.y) * (px0 - (double)B.x) - (double)p.d0;
    precise double w1 = (double)(A.x - C.x) * (py - (double)C.y) - (double)(A.y - C.y) * (px0 - (double)C.x) - (double)p.d1;
    precise double w2 = (double)(B.x - A.x) * (py - (double)A.y) - (double)(B.y - A.y) * (px0 - (double)A.x) - (double)p.d2;
    double nlo = 1.0, nhi = (double)(p.x1 - p.x0), n = (double)(x - p.x0 + 1);
    float fa[10], fb[10], fc[10];
    precise float fcur[10];
    precise double zcur, dw0, dw1, dw2;
    uint zp;
    int k, i, steps;
    edge_span(w0, (double)p.d0, p.tl0, nlo, nhi);
    edge_span(w1, (double)p.d1, p.tl1, nlo, nhi);
    edge_span(w2, (double)p.d2, p.tl2, nlo, nhi);
    if (y < p.y0 || y >= p.y1 || n < nlo || n > nhi) discard;
    // the first covered pixel of the row: exact start values, then the per-pixel steps up to x
    dw0 = w0 + (double)p.d0 * nlo; dw1 = w1 + (double)p.d1 * nlo; dw2 = w2 + (double)p.d2 * nlo;
    attrs(A, fa); attrs(B, fb); attrs(C, fc);
    zcur = (dw0 * (double)A.z + dw1 * (double)B.z + dw2 * (double)C.z) * inv;
    for (k = 0; k < 10; k++) {
        precise double s = (dw0 * (double)fa[k] + dw1 * (double)fb[k] + dw2 * (double)fc[k]) * inv;
        fcur[k] = (float)s;
    }
    steps = (int)(n - nlo);
    for (i = 0; i < steps; i++) {
        zcur += zstep;
        for (k = 0; k < p.nf; k++) fcur[k] += p.fstep[k];
    }
    {
        int r, g, b, a, f;
        float u = 0, v = 0;
        if (p.iip) { r = (int)fcur[0]; g = (int)fcur[1]; b = (int)fcur[2]; a = (int)fcur[3]; }
        else { r = p.vflat.r; g = p.vflat.g; b = p.vflat.b; a = p.vflat.a; }
        zp = zcur >= 4294967295.0 ? 0xFFFFFFFFu : zcur <= 0.0 ? 0u : (uint)zcur;
        f = (int)fcur[4];
        if (p.tme) {
            if (p.fst) { u = fcur[8] / 16.0f; v = fcur[9] / 16.0f; }
            else {
                precise float qq = fcur[7] == 0.0f ? 1.0f : fcur[7];
                precise float rq = fdiv(1.0f, qq);
                u = fcur[5] * rq * (float)(1 << p.tw);
                v = fcur[6] * rq * (float)(1 << p.th);
            }
        }
        put_pixel(p, x, y, r, g, b, a, zp, f, u, v, p.tme ? tri_lod(p, fcur[7]) : -1);
    }
}

#endif

// ---- sprites: sprite_rows ----------------------------------------------------------------------------------------
void sprite_pixel(Prim p, int x, int y)
{
    float u = 0, v = 0;
    if (p.tme) {
        precise float ty = (p.sy1 == p.sy0) ? p.st0
            : p.st0 + fdiv((p.st1 - p.st0) * (float)((y << 4) - p.sy0), (float)(p.sy1 - p.sy0));
        precise float tx = p.stx0 + p.sdu * (float)(x - p.spx0);
        u = tx / 16.0f; v = ty / 16.0f;
    }
    {   // sprite_rows: magnification unless the LOD is constant (LCM)
        int lod = -1;
        if (p.mxl && p.lcm) { lod = (p.lodk + 8) >> 4; lod = lod <= 0 ? -1 : (lod > p.mxl ? p.mxl : lod); }
        put_pixel(p, x, y, p.v1.r, p.v1.g, p.v1.b, p.v1.a, p.v1.z, p.v1.f, u, v, lod);
    }
}

void ps_main(VSOut i)
{
    Prim p = prims[i.prim];
    int x = (int)i.pos.x, y = (int)i.pos.y;
    if (x < p.x0 || x >= p.x1 || y < p.y0 || y >= p.y1) discard;   // the primitive's box (clipped to the scissor)
    if (p.kind == 0) tri_pixel(p, x, y);
    else if (p.kind == 1) sprite_pixel(p, x, y);
    else put_pixel(p, x, y, p.v0.r, p.v0.g, p.v0.b, p.v0.a, p.v0.z, p.v0.f, p.v0.s, p.v0.t, (int)p.v0.q);   // a point or line pixel (q: its level)
}
