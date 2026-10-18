/*
 * SIF services for the HLE layer: IRX loads read from the disc and handed to lane C's sound module (hw/sif.h),
 * RPC calls, EE -> IOP DMA and IOP heap. Modules sound/ does not take (pad, memory card, CD/FS) report
 * "loaded": lane P answers their calls in hle/.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "runtime.h"
#include "hle_rt.h"
#include "sif.h"

static int g_next_mod_id = 0x100;

int rt_iop_load_module(const char *path, const char *args, unsigned int args_len)
{
    const char *p = strchr(path, ':');
    unsigned int lba, size;
    uint8_t *irx;
    int id;
    p = p ? p + 1 : path;
    if (!rt_disc_find(p, &lba, &size)) {
        rt_log(strstr(path, "KCEJEAST") ? 0 : 1, "IOP module %s not on the disc: answered loaded", path);
        return g_next_mod_id++;
    }
    irx = (uint8_t *)malloc((size + 2047) & ~2047u);
    if (!irx || !rt_disc_read(lba, (size + 2047) / 2048, irx)) {
        free(irx);
        rt_fatal("IOP module %s could not be read from the disc", path);
    }
    id = snd_iop_load_module(path, irx, size, args, args_len);
    free(irx);
    /* -2: the sound driver (KCEJEAST.IRX) is there but did not start. SDRPC_Init would wait for its RPC server
     * forever, a silent hang at boot: stop with the reason instead */
    if (id == -2) rt_fatal("the sound driver %s did not start (IOP module from the disc)", path);
    if (id < 0) {
        rt_log(2, "IOP module %s (%u bytes): not a sound module, answered loaded", path, size);
        return g_next_mod_id++;
    }
    rt_log(2, "IOP module %s (%u bytes): loaded by sound, id %d", path, size, id);
    return id;
}

int rt_rpc_bound(unsigned int sid) { return snd_rpc_bound(sid); }

int rt_rpc_call(unsigned int sid, unsigned int fno, unsigned int send, unsigned int ssize, unsigned int recv,
                unsigned int rsize)
{
    return snd_rpc_call(sid, fno, send, ssize, recv, rsize);
}

void rt_sif_dma(unsigned int iop_addr, unsigned int src, unsigned int size)
{
    snd_sif_dma_to_iop(iop_addr, src, size);
}

unsigned int rt_iop_alloc(unsigned int size)
{
    uint32_t a = snd_iop_alloc(size);
    if (!a) rt_log(0, "IOP heap: %u bytes not available", size);
    return a;
}
