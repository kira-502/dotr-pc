/*
 * Modules (overlays): the runtime side of lane R's recompiled module code (recomp/rc_port.h).
 *
 * The game's loader still reads a module (b001.bin ...) from the disc to 0x01E00000 and calls mwOverlayInit, which
 * the port generator patches to call rt_overlay_switch first (gen/rules.json). Lane R then writes a 5-byte x86 jump
 * over each entry of the module's code; those bytes must be executable, so the overlay area is made RWX here.
 *
 * Without the recompiler in the build (runtime/rc_stub.c answers -1) nothing is written and a call into module
 * code returns 0 instead (runtime/crash.c), so the menus run without module effects.
 */
#include <windows.h>
#include <string.h>
#include "runtime.h"
#include "hle_rt.h"
#include "rc_port.h"

#define OVERLAY_BASE 0x01E00000u
#define OVERLAY_SIZE 0x00010000u     /* code + data of the largest module; effect work areas start at +0x10000 */

int rt_overlay_switch(void)
{
    DWORD old;
    int r;
    char name[17];
    memcpy(name, (const void *)(uintptr_t)(OVERLAY_BASE + 0x20), 16);
    name[16] = 0;
    if (!VirtualProtect((void *)(uintptr_t)OVERLAY_BASE, OVERLAY_SIZE, PAGE_EXECUTE_READWRITE, &old))
        rt_fatal("overlay area not executable (error %lu)", GetLastError());
    {
        extern double rt_hitch_switch_s;
        double t0 = rt_seconds();
        r = rc_overlay_switch();
        rt_hitch_switch_s += rt_seconds() - t0;
    }
    if (r < 0) {
        /* no recompiler linked: keep the area non-executable so a call into it is caught and skipped */
        VirtualProtect((void *)(uintptr_t)OVERLAY_BASE, OVERLAY_SIZE, PAGE_READWRITE, &old);
        rt_log(2, "module %s loaded (not translated in this build: its calls are skipped)", name);
        return 0;
    }
    if (r == 0) rt_fatal("module %s has no translated code", name);
    FlushInstructionCache(GetCurrentProcess(), (void *)(uintptr_t)OVERLAY_BASE, OVERLAY_SIZE);
    rt_log(2, "module %s switched in", rc_current_module() ? rc_current_module() : name);
    return 1;
}

void rc_fatal(const char *module, uint32_t from, uint32_t to)
{
    rt_fatal("module %s: jump from %08X to %08X, which is neither its own code nor a known game function",
             module ? module : "?", from, to);
}
