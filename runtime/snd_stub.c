/* Stand-in for lane C's sound module (hw/sif.h) when sound/ is not part of the build: silent, every RPC
 * server answers with a zeroed reply. */
#include <string.h>
#include "runtime.h"
#include "sif.h"

int snd_init(const snd_config *cfg) { (void)cfg; return 1; }
void snd_shutdown(void) {}
int snd_iop_load_module(const char *path, const uint8_t *irx, uint32_t size, const char *args, uint32_t args_len)
{
    (void)path; (void)irx; (void)size; (void)args; (void)args_len;
    return -1;
}
int snd_rpc_bound(uint32_t sid) { (void)sid; return 1; }
int snd_rpc_call(uint32_t sid, uint32_t fno, uint32_t send, uint32_t ssize, uint32_t recv, uint32_t rsize)
{
    (void)sid; (void)fno; (void)send; (void)ssize;
    if (recv && rsize) memset(PS2_PTR(recv), 0, rsize);
    return 0;
}
void snd_sif_dma_to_iop(uint32_t iop_addr, uint32_t src, uint32_t size) { (void)iop_addr; (void)src; (void)size; }
uint32_t snd_iop_alloc(uint32_t size)
{
    static uint32_t top = 0x00100000;      /* addresses only: nothing reads IOP memory without sound/ */
    uint32_t a = top;
    top += (size + 0xFF) & ~0xFFu;
    return a;
}
void snd_vblank(void) {}
