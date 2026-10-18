// copy.hlsl - a local->local transfer inside GS memory, as trx_local_to_local does it: gs_read_pixel from the
// source format, gs_write_pixel into the destination format. serial = 1 runs the CPU's exact order in one thread
// (source and destination overlap); otherwise one thread per pixel.
#include "gs_mem.hlsli"

cbuffer l2l : register(b0)
{
    uint sbp, sbw, spsm, ssax;
    uint ssay, dbp, dbw, dpsm;
    uint dsax, dsay, dir, rrw;
    uint rrh, serial, pad0, pad1;
};
RWByteAddressBuffer mem : register(u0);

void copy_pixel(uint xx, uint yy)
{
    uint sw, ss, sm, dw, ds, dm, v, old;
    gs_locate(spsm, sbp, sbw, (ssax + xx) & 2047, (ssay + yy) & 2047, sw, ss, sm);
    v = (mem.Load(sw * 4) >> ss) & sm;
    gs_locate(dpsm, dbp, dbw, (dsax + xx) & 2047, (dsay + yy) & 2047, dw, ds, dm);
    if (dm == 0xFFFFFFFFu) {
        mem.Store(dw * 4, v);
    } else {
        mem.InterlockedAnd(dw * 4, ~(dm << ds), old);
        mem.InterlockedOr(dw * 4, (v & dm) << ds, old);
    }
}

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (serial) {
        uint x, y;
        if (id.x != 0) return;
        for (y = 0; y < rrh; y++)
            for (x = 0; x < rrw; x++)
                copy_pixel((dir & 1) ? rrw - 1 - x : x, (dir & 2) ? rrh - 1 - y : y);
        return;
    }
    if (id.x >= rrw * rrh) return;
    copy_pixel(id.x % rrw, id.x / rrw);
}
