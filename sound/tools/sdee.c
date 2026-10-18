/* sdee.c - the EE side of the game's sound protocol, as the tools need it: boot KCEJEAST from the disc, send
 * SDRPC commands (server 05730601: 0x40 bytes out, 0x10 back), and SD_InitializeDevice. Mirrors main_002.c /
 * main_003.c of the decomp. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "../snd.h"
#include "iso9660.h"
#include "sdee.h"

#define SID 0x05730601u
static __declspec(align(16)) unsigned int sendbuf[16];
__declspec(align(16)) unsigned int sdee_reply[4];

unsigned sdee_rpc(unsigned cmd, unsigned a1, unsigned a2, unsigned a3, unsigned a4)
{
    memset(sendbuf, 0, sizeof sendbuf);
    sendbuf[0] = (unsigned)(uintptr_t)sendbuf;
    sendbuf[1] = a1; sendbuf[2] = a2; sendbuf[3] = a3; sendbuf[4] = a4;
    memset(sdee_reply, 0, sizeof sdee_reply);
    snd_rpc_call(SID, cmd, (unsigned)(uintptr_t)sendbuf, 0x40, (unsigned)(uintptr_t)sdee_reply, 0x10);
    return sdee_reply[0];
}

int sdee_boot(void)
{
    unsigned size;
    unsigned char *irx = iso_read_file("IOP/KCEJEAST.IRX", &size);
    if (!irx) { fprintf(stderr, "IOP/KCEJEAST.IRX not found on the disc\n"); return 0; }
    if (snd_iop_load_module("cdrom0:\\IOP\\KCEJEAST.IRX;1", irx, size, NULL, 0) < 0) { fprintf(stderr, "KCEJEAST did not load\n"); return 0; }
    if (!snd_rpc_bound(SID)) { fprintf(stderr, "the sound driver registered no RPC server\n"); return 0; }
    return 1;
}

unsigned sdee_init_device(void)
{
    static const unsigned bank[12] = { 0x5010, 0x9F810, 0x199810, 0x1CB810, 0x1FD810, 0x8F810, 0x9F810, 0xAF810,
                                       0xBF810, 0xD7810, 0xEF810, 0x107810 };
    unsigned i, iopbuf;
    sdee_rpc(0xF005, 0, 0, 0, 0);
    iopbuf = sdee_rpc(0xF002, 0xC1800, 0, 0, 0);
    for (i = 0; i < 12; i++) sdee_rpc(0x5F10, i, bank[i], 0, 0);
    sdee_rpc(0x5F12, 0x150000, 0x38000, 0, 0);
    sdee_rpc(0x5000, 0, 0, 0, 0);
    return iopbuf;
}

/* SD_vTransferSEGrpToIopProc, chunked path (states 30-36): header word 1 + 0x800 = bytes to send */
int sdee_load_group(unsigned iopbuf, int slot, const unsigned char *grp, unsigned size)
{
    unsigned total, sent = 0, guard = 0;
    memcpy(&total, grp + 4, 4);
    total += 0x800;
    if (total > size) total = size;
    while (sent < total) {
        unsigned chunk = total - sent > 0x80800 ? 0x80800 : total - sent;
        snd_sif_dma_to_iop(iopbuf + 0x40000, (unsigned)(uintptr_t)(grp + sent), chunk);
        sent += chunk;
        sdee_rpc(0x5102, (unsigned)slot, iopbuf + 0x40000, chunk, sent == chunk ? 0 : sent - chunk - 0x800);
        /* wait while the driver uploads (reply byte 6 is its busy flag) */
        do {
            snd_vblank();
            sdee_rpc(0x5002, 0, 0, 0, 0);
            if (++guard > 6000) { fprintf(stderr, "sound group upload did not finish\n"); return 0; }
        } while (((unsigned char *)sdee_reply)[6]);
    }
    return 1;
}
