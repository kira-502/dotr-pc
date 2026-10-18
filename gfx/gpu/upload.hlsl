// upload.hlsl - a host->local image transfer into GS memory: one thread per pixel, the same stores as
// gs_store_pixel. Pixels that share a word are merged with atomics.
#include "gs_mem.hlsli"

cbuffer trx : register(b0)
{
    uint dbp, dbw, dpsm, rrw;
    uint dsax, dsay, first, count;   // first: the transfer's linear pixel index of vals[base]
    uint base, pad0, pad1, pad2;
};
StructuredBuffer<uint> vals : register(t0);
RWByteAddressBuffer mem : register(u0);

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint i = id.x, l, x, y, word, shift, mask;
    if (i >= count) return;
    l = first + i;
    x = (dsax + l % rrw) & 2047;
    y = (dsay + l / rrw) & 2047;
    gs_locate(dpsm, dbp, dbw, x, y, word, shift, mask);
    if (mask == 0xFFFFFFFFu) {
        mem.Store(word * 4, vals[base + i]);
    } else {
        uint old;
        mem.InterlockedAnd(word * 4, ~(mask << shift), old);
        mem.InterlockedOr(word * 4, (vals[base + i] & mask) << shift, old);
    }
}
