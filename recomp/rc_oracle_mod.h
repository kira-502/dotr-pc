/*
 * rc_oracle_mod.h (included by rc_oracle.c) - the module oracle: R3 on real game states. With DOTR_RC_ORACLE=1, the
 * first calls (DOTR_ORACLE_CALLS, default 3) of each module function entered from native code are checked: the
 * translated function runs, memory is put back, the module's ORIGINAL EE code (read from the disc when the module is
 * switched in) runs on the interpreter from the same state, and v0/v1/f0 plus every changed byte are compared.
 * Results go to rc_oracle.log as "mod-same" / "MOD-DIFF" / "mod-skip". Functions whose call tree reaches the
 * runtime (or an indirect call) are not checked, as in the main-program oracle.
 */
typedef struct RcDiscModuleO { const char *name, *mrg; uint32_t offset; } RcDiscModuleO;
extern const RcDiscModuleO rc_disc_modules[];
extern const unsigned rc_ndisc_modules;

static int mod_oracle = -1;
static const RcModule *mod_cur;
static uint8_t *mod_counts, *mod_unsafe;
static unsigned mod_same, mod_diff, mod_skip;

static int mod_oracle_on(void)
{
    if (mod_oracle < 0) mod_oracle = elf && getenv("DOTR_RC_ORACLE") != NULL;
    return mod_oracle;
}

static int mod_find_mrg(const char *mrg, uint32_t *lba)
{
    static const char *dirs[] = {"\\", "\\DATA\\", "\\MODULE\\", "\\BIN\\"};
    char path[64];
    uint32_t size;
    for (unsigned i = 0; i < sizeof dirs / sizeof dirs[0]; i++) {
        snprintf(path, sizeof path, "%s%s", dirs[i], mrg);
        if (g_find(path, lba, &size)) return 1;
    }
    return 0;
}

/* called by rc_overlay_switch once the module is selected */
void rc_oracle_module_switch(const RcModule *m)
{
    uint32_t lba;
    if (!mod_oracle_on()) return;
    mod_cur = m;
    mod_img_ok = 0;
    free(mod_counts);
    free(mod_unsafe);
    mod_counts = (uint8_t *)calloc(m->nfuncs ? m->nfuncs : 1, 1);
    mod_unsafe = (uint8_t *)calloc(m->nfuncs ? m->nfuncs : 1, 1);
    if (!mod_counts || !mod_unsafe) return;
    for (unsigned i = 0; i < rc_ndisc_modules; i++) {
        if (strcmp(rc_disc_modules[i].name, m->name)) continue;
        if (mod_find_mrg(rc_disc_modules[i].mrg, &lba) &&
            g_read((uint64_t)lba * 2048 + rc_disc_modules[i].offset, sizeof mod_img, mod_img))
            mod_img_ok = 1;
        break;
    }
    if (!mod_img_ok) return;
    if (!native_set) build_native_set();
    if (!unsafe_set) build_unsafe_set();
    /* unsafe module functions: an indirect call, or a call out of the module into anything unsafe; then to a fixed
     * point through the calls inside the module */
    for (uint32_t k = 0; k < m->nfuncs; k++) {
        uint32_t lo = m->funcs[k].addr, hi = k + 1 < m->nfuncs ? m->funcs[k + 1].addr : 0x01E08000u;
        for (uint32_t a = lo; a < hi && !mod_unsafe[k]; a += 4) {
            uint32_t w = rc_oracle_fetch(a), t;
            if ((w >> 26) == 0 && (w & 63) == 9) mod_unsafe[k] = 1;                         /* jalr */
            if ((w >> 26) != 3) continue;                                                    /* jal */
            t = (a & 0xF0000000u) | ((w & 0x3FFFFFFu) << 2);
            if (t - 0x01E00000u < 0x8000u) continue;
            if (t < 0x00100000u || t >= 0x00300000u || !unsafe_set || BIT(unsafe_set, t) || !rc_main_func(t))
                mod_unsafe[k] = 1;
        }
    }
    for (int changed = 1; changed;) {
        changed = 0;
        for (uint32_t k = 0; k < m->nfuncs; k++) {
            uint32_t lo = m->funcs[k].addr, hi = k + 1 < m->nfuncs ? m->funcs[k + 1].addr : 0x01E08000u;
            if (mod_unsafe[k]) continue;
            for (uint32_t a = lo; a < hi; a += 4) {
                uint32_t w = rc_oracle_fetch(a), t;
                if ((w >> 26) != 3) continue;
                t = (a & 0xF0000000u) | ((w & 0x3FFFFFFu) << 2);
                if (t - 0x01E00000u >= 0x8000u) continue;
                for (uint32_t j = 0; j < m->nfuncs; j++)
                    if (m->funcs[j].addr == t && mod_unsafe[j]) { mod_unsafe[k] = 1; changed = 1; break; }
                if (mod_unsafe[k]) break;
            }
        }
    }
    {
        uint32_t safe = 0;
        if (!oracle_log) oracle_log = fopen(getenv("DOTR_ORACLE_LOG") ? getenv("DOTR_ORACLE_LOG") : "rc_oracle.log", "w");
        for (uint32_t k = 0; k < m->nfuncs; k++) safe += !mod_unsafe[k];
        if (oracle_log) {
            fprintf(oracle_log, "module %s: %u of %u functions checkable\n", m->name, safe, m->nfuncs);
            fflush(oracle_log);
        }
    }
}

