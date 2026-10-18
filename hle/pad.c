/*
 * libpad (scePad*): a DualShock 2 on each connected port, already in analog + pressure mode and always stable.
 * The buttons come from runtime/pad.c (XInput, keyboard, or a --pad-script).
 */
#include "hle.h"

static s32 hle_pad_mode[2] = { 4, 4 };     /* pad id: 4 digital until the game asks for 7 (DualShock) */
static s32 hle_pad_press[2];

static s32 hle_pad_ok(s32 port) { return port >= 0 && port < 2 && rt_pad_connected(port); }

HLE_FUNC s32 scePadInit(s32 a0) { (void)a0; return 1; }
HLE_FUNC s32 scePadInit2(s32 a0) { (void)a0; return 1; }
HLE_FUNC s32 scePadEnd(void) { return 1; }
HLE_FUNC s32 scePadPortOpen(s32 port, s32 slot, void *addr) { (void)slot; (void)addr; return port >= 0 && port < 2; }
HLE_FUNC s32 scePadPortClose(s32 port, s32 slot) { (void)port; (void)slot; return 1; }
HLE_FUNC s32 scePadGetState(s32 port, s32 slot) { (void)slot; return hle_pad_ok(port) ? 6 : 0; }   /* 6 stable, 0 disconnected */
HLE_FUNC u8 scePadGetReqState(s32 port, s32 slot) { (void)port; (void)slot; return 0; }           /* request complete */
HLE_FUNC s32 scePadSetReqState(s32 port, s32 slot, s32 state) { (void)port; (void)slot; (void)state; return 1; }
HLE_FUNC s32 scePadGetPortMax(void) { return 2; }
HLE_FUNC s32 scePadGetSlotMax(s32 port) { (void)port; return 1; }
HLE_FUNC s32 scePadGetModVersion(void) { return 0x0200; }
HLE_FUNC s32 scePadGetFrameCount(s32 port, s32 slot) { (void)port; (void)slot; return (s32)rt_vblank_count(); }

/* term 1 current id, 2 current extended id, 3 current mode offset, 4 id table (offs -1: its length) */
HLE_FUNC s32 scePadInfoMode(s32 port, s32 slot, s32 term, s32 offs)
{
    (void)slot;
    if (!hle_pad_ok(port)) return 0;
    switch (term) {
    case 1: return hle_pad_mode[port];
    case 2: return 0;
    case 3: return hle_pad_mode[port] == 7;
    case 4: return offs == -1 ? 2 : offs == 0 ? 4 : offs == 1 ? 7 : 0;
    }
    return 0;
}

HLE_FUNC s32 scePadSetMainMode(s32 port, s32 slot, s32 offs, s32 lock)
{
    (void)slot; (void)lock;
    if (port < 0 || port > 1) return 0;
    hle_pad_mode[port] = offs == 1 ? 7 : 4;
    return 1;
}

HLE_FUNC s32 scePadInfoPressMode(s32 port, s32 slot) { (void)slot; return hle_pad_ok(port); }
HLE_FUNC s32 scePadEnterPressMode(s32 port, s32 slot) { (void)slot; if (port >= 0 && port < 2) hle_pad_press[port] = 1; return 1; }
HLE_FUNC s32 scePadExitPressMode(s32 port, s32 slot) { (void)slot; if (port >= 0 && port < 2) hle_pad_press[port] = 0; return 1; }
HLE_FUNC s32 scePadGetButtonMask(s32 port, s32 slot) { (void)port; (void)slot; return 0xFFFF; }
HLE_FUNC s32 scePadSetButtonInfo(s32 port, s32 slot, s32 info) { (void)port; (void)slot; (void)info; return 1; }
HLE_FUNC s32 scePadInfoAct(s32 port, s32 slot, s32 term, s32 offs) { (void)port; (void)slot; (void)term; (void)offs; return 0; }
HLE_FUNC s32 scePadInfoComb(s32 port, s32 slot, s32 actno, s32 term) { (void)port; (void)slot; (void)actno; (void)term; return 0; }
HLE_FUNC s32 scePadSetActAlign(s32 port, s32 slot, u8 *data) { (void)port; (void)slot; (void)data; return 1; }
HLE_FUNC s32 scePadSetActDirect(s32 port, s32 slot, u8 *data) { (void)port; (void)slot; (void)data; return 1; }   /* no rumble yet */
HLE_FUNC s32 scePadSetVrefParam(s32 port, s32 slot, u8 *data) { (void)port; (void)slot; (void)data; return 1; }
HLE_FUNC s32 scePadSetWarningLevel(s32 level) { (void)level; return 0; }

/* DotR reads into a 20-byte buffer: status, id, buttons, 4 axes, 12 pressures */
HLE_FUNC s32 scePadRead(s32 port, s32 slot, u8 *data)
{
    (void)slot;
    if (port < 0 || port > 1 || !rt_pad_read(port, data)) return 0;
    if (hle_pad_mode[port] != 7) { data[1] = 0x41; return 4; }
    if (!hle_pad_press[port]) { data[1] = 0x73; return 8; }
    return 20;
}
