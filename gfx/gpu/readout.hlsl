// readout.hlsl - the PCRTC on the GPU: the TV picture from the two display circuits, as gs_display_read makes it.
#include "gs_mem.hlsli"

struct circ { int on, fbp, fbw, psm, dbx, dby, dx, dy, magh, magv, dw, dh; };
cbuffer pcrtc : register(b0)
{
    circ c0, c1;
    int ref1, fieldm, mmod, slbg;   // ref1: circuit 1 is the reference
    int alp, w, h;
    uint bg;
};
ByteAddressBuffer mem : register(t0);
#ifdef TO_TEXTURE
RWTexture2D<uint4> outt : register(u0);    // the window's picture (R8G8B8A8_UINT view)
#else
RWByteAddressBuffer outp : register(u0);   // w*h RGBA words (R in the low byte), alpha 255
#endif

uint to_rgba(uint psm, uint v)
{
    if (psm == PSMCT16 || psm == PSMCT16S)
        return ((v & 0x1F) << 3) | (((v >> 5) & 0x1F) << 11) | (((v >> 10) & 0x1F) << 19) | ((v & 0x8000) ? 0x80000000u : 0u);
    if (psm == PSMCT24) return (v & 0xFFFFFF) | 0x80000000u;
    return v;
}

// the buffer pixel a circuit shows at output (x, y), or 0 when it shows nothing there
bool circ_pixel(circ c, circ r, int x, int y, out uint v)
{
    int vx = x * r.magh + r.dx - c.dx, vy = y * r.magv + r.dy - c.dy, cx, cy;
    uint word, shift, mask;
    v = 0;
    if (vx < 0 || vy < 0) return false;
    cx = vx / c.magh; cy = vy / c.magv;
    if (cx >= c.dw / c.magh || cy >= c.dh / c.magv) return false;
    if (fieldm) cy >>= 1;
    cx = (c.dbx + cx) & 2047; cy = (c.dby + cy) & 2047;
    gs_locate((uint)c.psm, (uint)c.fbp, (uint)c.fbw, (uint)cx, (uint)cy, word, shift, mask);
    v = to_rgba((uint)c.psm, (mem.Load(word * 4) >> shift) & mask);
    return true;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    int x = (int)id.x, y = (int)id.y, k;
    uint p1 = 0, p2 = bg, outv = 0;
    bool h1;
    circ r = c0;
    if (ref1) r = c1;
    if (x >= w || y >= h) return;
    h1 = c0.on && circ_pixel(c0, r, x, y, p1);
    if (!slbg && c1.on) { if (!circ_pixel(c1, r, x, y, p2)) p2 = bg; }
    if (h1) {
        uint a = mmod ? (uint)alp : min((p1 >> 24) * 2, 255u);
        if (a == 255) outv = p1 & 0xFFFFFF;
        else for (k = 0; k < 24; k += 8) {
            uint v = ((p1 >> k) & 0xFF) * a + ((p2 >> k) & 0xFF) * (255 - a);
            outv |= ((v + 1 + (v >> 8)) >> 8) << k;
        }
    } else {
        outv = p2 & 0xFFFFFF;
    }
#ifdef TO_TEXTURE
    outt[uint2(x, y)] = uint4(outv & 0xFF, (outv >> 8) & 0xFF, (outv >> 16) & 0xFF, 255);
#else
    outp.Store((y * w + x) * 4, outv | 0xFF000000u);
#endif
}
