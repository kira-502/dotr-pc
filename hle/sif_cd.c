/*
 * SIF (the EE <-> IOP link) and the CD/DVD library, as DotR uses them. Sound modules and their RPC go to lane
 * C's IOP (runtime/sif.c, hw/sif.h); other modules and resets report success; the disc is read from the ISO.
 *
 * Sony's names for the decomp's func_ entries (identified from their code and the game's calls):
 *   func_0017E240 sceSifInitRpc        func_00183A38 sceSifRebootIop      func_00183A00 sceSifSyncIop
 *   func_00191778 sceCdInit             func_00191C50 sceCdMmode           func_00191A58 sceCdDiskReady
 *   func_00191D18 sceCdRead             func_00191EF8 sceCdGetError        func_00190A90 sceCdCallback
 *   func_00183538 sceSifLoadModule
 */
#include "hle.h"

/* ---- SIF / IOP ------------------------------------------------------------------------------------ */

HLE_FUNC void func_0017E240(s32 mode) { (void)mode; }                       /* sceSifInitRpc */
HLE_FUNC s32 func_00183A38(char *path)                                       /* sceSifRebootIop */
{
    rt_log(2, "IOP reboot with %s (IOP not emulated: ok)", path ? path : "(null)");
    return 1;
}
HLE_FUNC s32 func_00183A00(void) { return 1; }                               /* sceSifSyncIop */
HLE_FUNC s32 sceSifInitIopHeap(void) { return 0; }
HLE_FUNC s32 sceSifLoadFileReset(void) { return 0; }
HLE_FUNC s32 sceFsReset(void) { return 0; }

HLE_FUNC s32 sceSifAllocIopHeap(s32 size) { return (s32)rt_iop_alloc((u32)size); }

/* sceSifLoadModule(path, arg length, args): IRX bytes come from the disc; sound modules run in lane C's IOP */
HLE_FUNC s32 func_00183538(s32 path, s32 arg_len, s32 args)
{
    return rt_iop_load_module((const char *)path, (const char *)args, (u32)arg_len);
}

/* sceSifClientData: rpcd {paddr, pid, tid, mode} at 0, command 0x10, buff 0x14, cbuff 0x18, end_function 0x1C,
 * end_param 0x20, serve 0x24. Bound = serve not 0; the server id is kept in `command` (unused by the game). */
HLE_FUNC s32 sceSifBindRpc(void *bd, s32 sid, s32 mode)
{
    u32 *c = (u32 *)bd;
    (void)mode;
    c[4] = (u32)sid;
    c[9] = rt_rpc_bound((u32)sid) ? 0x00001000u : 0;
    return 0;
}

HLE_FUNC s32 sceSifCallRpc(s32 *client, s32 rpc_number, s32 mode, s32 *sendbuf, s32 ssize, s32 *recvbuf, s32 rsize,
                           s32 end_func, s32 end_param)
{
    u32 sid = ((u32 *)client)[4];
    (void)mode;
    if (rt_rpc_call(sid, (u32)rpc_number, (u32)sendbuf, (u32)ssize, (u32)recvbuf, (u32)rsize) < 0)
        rt_fatal("SIF RPC %04X to server %08X, which is not there", rpc_number, sid);
    if (end_func) ((void (*)(s32))(u32)end_func)(end_param);
    return 0;
}

HLE_FUNC s32 sceSifCheckStatRpc(void *client) { (void)client; return 0; }   /* calls finish at once */

/* sceSifDmaData {data (EE), addr (IOP), size, mode}; transfers finish at once */
static s32 hle_sif_dma_id;
HLE_FUNC int sceSifSetDma(int *d, int n)
{
    int i;
    for (i = 0; i < n; i++, d += 4) rt_sif_dma((u32)d[1], (u32)d[0], (u32)d[2]);
    return ++hle_sif_dma_id;
}
HLE_FUNC int sceSifDmaStat(int id) { (void)id; return -1; }

/* ---- CD/DVD ---------------------------------------------------------------------------------------- */

typedef struct {
    u32 lsn;
    u32 size;
    char name[16];
    u8 date[8];
} hle_cd_file;

static void (*hle_cd_cb)(s32);
static s32 hle_cd_error;
static s32 hle_cd_pending;          /* a read finished: run the callback at the next vblank */

static void hle_cd_vblank(void)
{
    if (hle_cd_pending) {
        hle_cd_pending = 0;
        if (hle_cd_cb) hle_cd_cb(1);   /* SCECdFuncRead */
    }
}

HLE_FUNC s32 func_00191778(s32 mode)                                         /* sceCdInit */
{
    static int hooked;
    (void)mode;
    if (!hooked) { rt_on_vblank(hle_cd_vblank); hooked = 1; }
    return 1;
}
HLE_FUNC s32 func_00191C50(s32 media) { (void)media; return 1; }            /* sceCdMmode */
HLE_FUNC s32 func_00191A58(s32 mode) { (void)mode; return 2; }              /* sceCdDiskReady: SCECdComplete */
HLE_FUNC s32 func_00191EF8(void) { return hle_cd_error; }                       /* sceCdGetError */
HLE_FUNC s32 func_00190A90(void (*fn)(void))                                 /* sceCdCallback */
{
    hle_cd_cb = (void (*)(s32))fn;
    return 0;
}
HLE_FUNC s32 sceCdInitEeCB(s32 prio, void *stack, s32 size) { (void)prio; (void)stack; (void)size; return 1; }

HLE_FUNC s32 sceCdSearchFile(void *out, const char *name)
{
    hle_cd_file *f = (hle_cd_file *)out;
    unsigned int lba, size;
    if (!rt_disc_find(name, &lba, &size)) {
        rt_log(1, "sceCdSearchFile: %s not on the disc", name);
        return 0;
    }
    f->lsn = lba;
    f->size = size;
    return 1;
}

HLE_FUNC s32 func_00191D18(s32 lsn, s32 sectors, s32 buf, void *mode)       /* sceCdRead */
{
    (void)mode;
    hle_cd_error = rt_disc_read((u32)lsn, (u32)sectors, (void *)buf) ? 0 : 0x32;
    if (hle_cd_error) rt_log(0, "sceCdRead lsn %d x %d failed", lsn, sectors);
    hle_cd_pending = 1;
    return 1;
}

/* ---- sceOpen / sceLseek / sceRead / sceClose / sceIoctl: the game's second file path ------------------------
 * lSzFile_DoRequestMain (main_054.c) uses them for a file outside its table of 97 disc files, RhFile_ExistFile
 * to test one. Files come from the disc image ("cdrom0:\\DIR\\NAME;1"); a missing one gets -1 and one log line,
 * as on the PS2 (an empty "success" would load as an empty buffer far from the cause). Reads finish at once, so
 * sceIoctl(fd, 1 = status) answers 0 (done). */
typedef struct { u32 lba, size, pos; int used; } hle_fio;
static hle_fio hle_fio_tab[16];

static hle_fio *hle_fio_get(u32 fd)
{
    return fd >= 3 && fd < 16 && hle_fio_tab[fd].used ? &hle_fio_tab[fd] : NULL;
}

HLE_FUNC s32 func_0017FA98(void *name, s32 flags, ...)                     /* sceOpen */
{
    const char *n = (const char *)name, *c = n ? strchr(n, ':') : NULL;
    unsigned int lba, size;
    u32 fd;
    if (!n || (flags & 3) != 1 && (flags & 3) != 0) {
        rt_log(1, "sceOpen %s flags %X: only reading from the disc is possible", n ? n : "(null)", flags);
        return -1;
    }
    if (!rt_disc_find(c ? c + 1 : n, &lba, &size)) {
        rt_log(1, "sceOpen %s: not on the disc", n);
        return -1;
    }
    for (fd = 3; fd < 16 && hle_fio_tab[fd].used; fd++) {}
    if (fd == 16) { rt_log(0, "sceOpen %s: no free file handle (16 open)", n); return -1; }
    hle_fio_tab[fd].lba = lba;
    hle_fio_tab[fd].size = size;
    hle_fio_tab[fd].pos = 0;
    hle_fio_tab[fd].used = 1;
    return (s32)fd;
}

HLE_FUNC s32 func_0017FD20(u32 fd)                                           /* sceClose */
{
    hle_fio *f = hle_fio_get(fd);
    if (!f) return -1;
    f->used = 0;
    return 0;
}

HLE_FUNC s32 func_0017FEA0(u32 fd, s32 offset, s32 whence)                  /* sceLseek */
{
    hle_fio *f = hle_fio_get(fd);
    s32 base;
    if (!f) return -1;
    base = whence == 0 ? 0 : whence == 1 ? (s32)f->pos : whence == 2 ? (s32)f->size : -1;
    if (base < 0 || base + offset < 0) return -1;
    f->pos = (u32)(base + offset);
    return (s32)f->pos;
}

HLE_FUNC s32 func_001800D8(s32 fd, s32 buf, s32 nbyte)                      /* sceRead */
{
    static u8 sector[2048];
    hle_fio *f = hle_fio_get((u32)fd);
    u8 *dst = (u8 *)buf;
    u32 left, done = 0;
    if (!f || nbyte < 0) return -1;
    left = f->pos >= f->size ? 0 : f->size - f->pos;
    if ((u32)nbyte < left) left = (u32)nbyte;
    while (done < left) {
        u32 off = (f->pos + done) & 2047, k = 2048 - off;
        if (k > left - done) k = left - done;
        if (!rt_disc_read(f->lba + ((f->pos + done) >> 11), 1, sector)) {
            rt_log(0, "sceRead: disc sector %u could not be read", f->lba + ((f->pos + done) >> 11));
            return -1;
        }
        memcpy(dst + done, sector + off, k);
        done += k;
    }
    f->pos += done;
    return (s32)done;
}

HLE_FUNC s32 func_00180608(u32 fd, s32 request, void *argp)                 /* sceIoctl */
{
    if (!hle_fio_get(fd)) return -1;
    if (request == 1 && argp) *(s32 *)argp = 0;                               /* SCE_CDVD status: done */
    return 0;
}
