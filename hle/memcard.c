/*
 * libmc (sceMc*) on folders (runtime/memcard.c). Every call finishes at once; the next sceMcSync reports it.
 *
 * Sony's names for the decomp's func_ entries (libmc order; from the game's call sequences):
 *   func_00193808 sceMcInit       func_00193BA8 sceMcOpen      func_00193DD0 sceMcSeek     func_00193F38 sceMcRead
 *   func_00194058 sceMcWrite      func_00194248 sceMcSync      func_00194380 sceMcGetInfo  func_00194578 sceMcGetDir
 *   func_00194758 sceMcChdir      func_001948A8 sceMcFormat    func_00194970 sceMcDelete   func_00194B50 sceMcSetFileInfo
 *   func_00194D28 sceMcRename     func_00194E90 sceMcUnformat  func_00194F58 sceMcGetEntSpace
 */
#include "hle.h"

static s32 hle_mc_cmd, hle_mc_result, hle_mc_pending;

static s32 hle_mc_done(s32 cmd, s32 result)
{
    hle_mc_cmd = cmd;
    hle_mc_result = result;
    hle_mc_pending = 1;
    return 0;
}

HLE_FUNC s32 func_00193808(void) { return 0; }                                           /* sceMcInit */

HLE_FUNC s32 func_00194380(s32 port, s32 slot, s32 *type, s32 *free, s32 *format)       /* sceMcGetInfo */
{
    s32 t, f, fm, r;
    (void)slot;
    r = rt_mc_getinfo(port, &t, &f, &fm);
    if (type) *type = t;
    if (free) *free = f;
    if (format) *format = fm;
    return hle_mc_done(1, r);
}

HLE_FUNC s32 func_00193BA8(s32 port, s32 slot, s8 *name, s32 mode)                      /* sceMcOpen */
{
    (void)slot;
    return hle_mc_done(2, rt_mc_open(port, (const char *)name, mode));
}

HLE_FUNC s32 sceMcClose(s32 fd) { return hle_mc_done(3, rt_mc_close(fd)); }

HLE_FUNC s32 func_00193DD0(s32 fd, s32 offset, s32 origin, s32 unused)                   /* sceMcSeek */
{
    (void)unused;
    return hle_mc_done(4, rt_mc_seek(fd, offset, origin));
}

HLE_FUNC s32 func_00193F38(s32 fd, s32 *buf, s32 size) { return hle_mc_done(5, rt_mc_read(fd, buf, size)); }       /* sceMcRead */
HLE_FUNC s32 func_00194058(s32 fd, void *buf, s32 size) { return hle_mc_done(6, rt_mc_write(fd, buf, size)); }     /* sceMcWrite */
HLE_FUNC s32 sceMcFlush(s32 fd) { (void)fd; return hle_mc_done(10, 0); }
HLE_FUNC s32 sceMcMkdir(s32 port, s32 slot, void *name) { (void)slot; return hle_mc_done(11, rt_mc_mkdir(port, (const char *)name)); }

HLE_FUNC s32 func_00194758(s32 port, s32 slot, void *name, void *old)                    /* sceMcChdir */
{
    (void)slot;
    return hle_mc_done(12, rt_mc_chdir(port, (const char *)name, (char *)old));
}

HLE_FUNC s32 func_00194578(s32 port, s32 slot, void *name, u32 mode, s32 max, void *table)  /* sceMcGetDir */
{
    (void)slot;
    /* mode 0 = first call; the folder layer lists everything at once, so a continuation finds nothing more */
    return hle_mc_done(13, mode ? 0 : rt_mc_getdir(port, (const char *)name, max, table));
}

HLE_FUNC s32 func_00194B50(s32 port, s32 slot, s8 *name, void *info, s32 flags)          /* sceMcSetFileInfo */
{
    (void)port; (void)slot; (void)name; (void)info; (void)flags;
    return hle_mc_done(14, 0);
}

HLE_FUNC s32 func_00194970(s32 port, s32 slot, s8 *name) { (void)slot; return hle_mc_done(15, rt_mc_delete(port, (const char *)name)); }  /* sceMcDelete */
HLE_FUNC s32 func_001948A8(s32 port, s32 slot) { (void)port; (void)slot; return hle_mc_done(16, 0); }      /* sceMcFormat */
HLE_FUNC s32 func_00194E90(s32 port, s32 slot) { (void)port; (void)slot; return hle_mc_done(17, 0); }      /* sceMcUnformat */
HLE_FUNC s32 func_00194F58(s32 port, s32 slot, s8 *path) { (void)port; (void)slot; (void)path; return hle_mc_done(18, 0x1E00); }  /* sceMcGetEntSpace */

HLE_FUNC s32 func_00194D28(s32 port, s32 slot, s8 *from, void *to)                       /* sceMcRename */
{
    (void)port; (void)slot; (void)from; (void)to;
    HLE_TODO("sceMcRename");
    return hle_mc_done(19, -4);
}

/* mode 0 waits, 1 checks; 1 = a command finished (cmd, result filled), -1 = nothing pending */
HLE_FUNC s32 func_00194248(s32 mode, s32 *cmd, s32 *result)                              /* sceMcSync */
{
    (void)mode;
    if (!hle_mc_pending) return -1;
    hle_mc_pending = 0;
    if (cmd) *cmd = hle_mc_cmd;
    if (result) *result = hle_mc_result;
    return 1;
}
