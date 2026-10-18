/*
 * rc_check.c - windowless R4 check, run by the runtime (DotR.exe --rc-check): every module goes through the game's
 * own loading path. Its 32 KB are read from the disc (BMODULE/CMODULE/MMODULE.MRG) into 0x01E00000 and the game's
 * native mwOverlayInit is called through its PS2 address, exactly as SzEffect/TaEffect do. That runs
 * rt_overlay_switch -> rc_overlay_switch (module found by its header name, entry jumps written), then the module's
 * init table: native code calling translated module code through the jumps and stubs, which in turn calls back
 * into the game through the rcx_ thunks.
 */
#include <stdio.h>
#include <string.h>
#include "rc_port.h"

#define MOD_LO 0x01E00000u

typedef struct RcDiscModule { const char *name, *mrg; uint32_t offset; } RcDiscModule;
extern const RcDiscModule rc_disc_modules[];
extern const unsigned rc_ndisc_modules;
extern unsigned rc_stat_entries, rc_stat_dispatch;

typedef int (*rc_find_fn)(const char *path, uint32_t *lba, uint32_t *size);
typedef int (*rc_read_fn)(uint64_t offset, uint32_t size, void *dst);

static int find_mrg(rc_find_fn find, const char *mrg, uint32_t *lba)
{
    static const char *dirs[] = {"\\", "\\DATA\\", "\\MODULE\\", "\\BIN\\"};
    char path[64];
    uint32_t size;
    for (unsigned i = 0; i < sizeof dirs / sizeof dirs[0]; i++) {
        snprintf(path, sizeof path, "%s%s", dirs[i], mrg);
        if (find(path, lba, &size)) return 1;
    }
    return 0;
}

int rc_check_modules(rc_find_fn find, rc_read_fn read, char *msg, int len)
{
    void (*overlay_init)(int32_t, int32_t) = (void (*)(int32_t, int32_t))(uintptr_t)0x0017B410u;  /* mwOverlayInit */
    uint8_t *mod = (uint8_t *)(uintptr_t)MOD_LO;
    const char *last_mrg = NULL;
    uint32_t lba = 0;
    unsigned ok = 0, bad = 0;
    msg[0] = 0;
    for (unsigned i = 0; i < rc_ndisc_modules; i++) {
        const RcDiscModule *m = &rc_disc_modules[i];
        const char *now;
        if (!last_mrg || strcmp(last_mrg, m->mrg)) {
            if (!find_mrg(find, m->mrg, &lba)) { snprintf(msg, len, "%s not found on the disc", m->mrg); return -1; }
            last_mrg = m->mrg;
        }
        if (!read((uint64_t)lba * 2048 + m->offset, 0x8000, mod)) {
            snprintf(msg, len, "%s: read failed", m->name);
            return -1;
        }
        overlay_init((int32_t)MOD_LO, *(int32_t *)(uintptr_t)(MOD_LO + 0x1C) - (int32_t)MOD_LO);
        now = rc_current_module();
        if (!now || strcmp(now, m->name) || mod[0x80] != 0xE9) {
            if (!bad) snprintf(msg, len, "%s: after mwOverlayInit the current module is %s, entry byte %02X", m->name,
                               now ? now : "(none)", mod[0x80]);
            bad++;
        } else {
            ok++;
        }
    }
    if (!bad)
        snprintf(msg, len, "%u modules loaded from the disc and initialised through mwOverlayInit; native code entered "
                 "translated module code %u times, translated code made %u indirect calls", ok, rc_stat_entries,
                 rc_stat_dispatch);
    return (int)bad;
}
