// clutload.hlsl - a CLUT load (gs.c clut_load) done on the GPU from GS memory as the GPU has it: the new CLUT
// version (slot dst) is the previous one (slot src) with the entries this load brings. One thread per word of
// gs.clut's 512 halfwords (two entries' halves), so no two threads write the same word.
#include "gs_mem.hlsli"

cbuffer k : register(b0)
{
    uint cbp, cpsm, csm, csa;
    uint n, cbw, cou, cov;
    uint src, dst, pad0, pad1;
};
ByteAddressBuffer mem : register(t0);
RWStructuredBuffer<uint> cluts : register(u0);

uint read_px(uint psm, uint bp, uint bw, uint x, uint y)   // gs_read_pixel
{
    uint word, shift, mask;
    gs_locate(psm, bp, bw, x, y, word, shift, mask);
    return (mem.Load(word * 4) >> shift) & mask;
}

uint entry(uint i)   // the CLUT entry i of this load, as clut_load reads it
{
    bool ct16 = cpsm == PSMCT16 || cpsm == PSMCT16S;
    if (csm == 0) {
        uint x, y;
        if (n == 256) { uint j = (i & 0xE7) | ((i & 8) << 1) | ((i & 16) >> 1); x = j & 15; y = j >> 4; }
        else { x = i & 7; y = i >> 3; }
        return read_px(ct16 ? cpsm : PSMCT32, cbp, 1, x, y);
    }
    return read_px(ct16 ? cpsm : PSMCT16, cbp, cbw, cou * 16 + i, cov);
}

uint half_of(uint h, uint old)   // halfword h of gs.clut after the load
{
    bool ct16 = csm != 0 || cpsm == PSMCT16 || cpsm == PSMCT16S;
    if (ct16) {
        uint base = n == 16 ? (csa * 16) & 511 : 0, i = (h - base) & 511;
        return i < n ? entry(i) & 0xFFFF : old;
    } else {
        uint e = h & 255, base = n == 16 ? (csa & 15) * 16 : 0, i = (e - base) & 255;
        if (i >= n) return old;
        return h < 256 ? entry(i) & 0xFFFF : entry(i) >> 16;
    }
}

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint w = id.x, old, lo, hi;
    if (w >= 256) return;
    old = cluts[src * 256 + w];
    lo = half_of(w * 2, old & 0xFFFF);
    hi = half_of(w * 2 + 1, old >> 16);
    cluts[dst * 256 + w] = lo | (hi << 16);
}
