/* Write a 5-byte x86 jump at every PS2 function address, so function pointers found in the game's own data
 * (PS2 addresses) call the native functions directly. */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "runtime.h"

int tramp_install(char *err, int errlen)
{
    uint32_t i, lo = 0xFFFFFFFFu, hi = 0;
    DWORD old;
    if (ps2_functab_count == 0) { snprintf(err, errlen, "no game functions in this build"); return 0; }
    for (i = 0; i < ps2_functab_count; i++) {
        if (ps2_functab[i].addr < lo) lo = ps2_functab[i].addr;
        if (ps2_functab[i].addr + 5 > hi) hi = ps2_functab[i].addr + 5;
    }
    if (!VirtualProtect(PS2_PTR(lo), hi - lo, PAGE_EXECUTE_READWRITE, &old)) {
        snprintf(err, errlen, "could not make the code area executable (Windows error %lu)", GetLastError());
        return 0;
    }
    for (i = 0; i < ps2_functab_count; i++) {
        uint8_t *at = (uint8_t *)PS2_PTR(ps2_functab[i].addr);
        int32_t rel = (int32_t)((uintptr_t)ps2_functab[i].native - ((uintptr_t)at + 5));
        at[0] = 0xE9;
        memcpy(at + 1, &rel, 4);
    }
    FlushInstructionCache(GetCurrentProcess(), PS2_PTR(lo), hi - lo);
    return 1;
}
