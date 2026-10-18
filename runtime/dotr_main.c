/* dotr.dll entry: called by DotR.exe after it mapped PS2 memory. */
#include <windows.h>
#include <commdlg.h>
#include <stdio.h>
#include <string.h>
#include <float.h>
#include <xmmintrin.h>
#include "runtime.h"
#include "sif.h"
#include "hle_rt.h"
#include "rc_port.h"
#include <xmmintrin.h>

hw_u32 hw_vblank_count(void) { return rt_vblanks(); }

static wchar_t g_exe_dir[MAX_PATH];

static void exe_dir(void)
{
    wchar_t *slash;
    GetModuleFileNameW(NULL, g_exe_dir, MAX_PATH);
    slash = wcsrchr(g_exe_dir, L'\\');
    if (slash) slash[1] = 0;
}

static int has_arg(const wchar_t *cmd, const wchar_t *arg) { return wcsstr(cmd, arg) != NULL; }

static int g_debug_menu;     /* --debug-menu (DOTR_DEV builds only): boot into the game's developer menu */

/* rules.json: the mode the memory-card check hands over to (10 = title) */
int rt_first_mode(int mode) { return g_debug_menu ? -1 : mode; }

/* DotR-PC.ini "renderer=gpu|exact", "gpu_stepping=direct|exact" and "saves=<folder>" become DOTR_RENDERER /
 * DOTR_GPU_STEPPING (for gfx_init) and DOTR_SAVES (runtime/memcard.c); an environment variable already set wins */
static void ini_to_env(void)
{
    static const char *const keys[][2] = { { "renderer=", "DOTR_RENDERER" }, { "gpu_stepping=", "DOTR_GPU_STEPPING" },
                                           { "saves=", "DOTR_SAVES" } };
    wchar_t ini[MAX_PATH];
    char line[256];
    FILE *f;
    int k;
    swprintf(ini, MAX_PATH, L"%sDotR-PC.ini", g_exe_dir);
    if (!(f = _wfopen(ini, L"r"))) return;
    while (fgets(line, sizeof line, f)) {
        char *e = line + strlen(line);
        while (e > line && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ')) *--e = 0;
        for (k = 0; k < (int)(sizeof keys / sizeof keys[0]); k++) {
            size_t n = strlen(keys[k][0]);
            if (!strncmp(line, keys[k][0], n) && line[n] && !getenv(keys[k][1])) {
                _putenv_s(keys[k][1], line + n);
                hw_log(2, "DotR-PC.ini: %s", line);
            }
        }
    }
    fclose(f);
}

static void hex20(const uint8_t *h, char *out)
{
    int i;
    for (i = 0; i < 20; i++) sprintf(out + 2 * i, "%02x", h[i]);
}

/* 1 = the image opens and holds the USA game executable (SLUS_205.15, checksum); it stays open */
static int disc_ok(const wchar_t *path, int *wrong)
{
    uint32_t lba, size;
    uint8_t *buf, h[20];
    char sum[41];
    int ok;
    if (!path[0] || !iso_open(path)) return 0;
    if (!iso_find("SLUS_205.15", &lba, &size) || size > 16u << 20 || !(buf = malloc(size))) { *wrong = 1; return 0; }
    ok = iso_read((uint64_t)lba * 2048, size, buf);
    if (ok) sha1(buf, size, h);
    free(buf);
    if (ok) hex20(h, sum);
    if (!ok || strcmp(sum, SLUS_SHA1) != 0) { *wrong = 1; return 0; }
    return 1;
}

static void ini_read(const char *key, wchar_t *out)
{
    wchar_t ini[MAX_PATH];
    char line[1024];
    size_t n = strlen(key);
    FILE *f;
    out[0] = 0;
    swprintf(ini, MAX_PATH, L"%sDotR-PC.ini", g_exe_dir);
    if (!(f = _wfopen(ini, L"r"))) return;
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, key, n) == 0) {
            char *e = line + strlen(line);
            while (e > line && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ')) *--e = 0;
            MultiByteToWideChar(CP_UTF8, 0, line + n, -1, out, MAX_PATH);
            break;
        }
    }
    fclose(f);
}

/* DotR-PC.ini keeps its other lines; the iso= line becomes this path */
static void ini_write_iso(const wchar_t *path)
{
    wchar_t ini[MAX_PATH];
    char keep[8192] = "", line[1024], p8[MAX_PATH * 3];
    FILE *f;
    swprintf(ini, MAX_PATH, L"%sDotR-PC.ini", g_exe_dir);
    if ((f = _wfopen(ini, L"r"))) {
        while (fgets(line, sizeof line, f))
            if (strncmp(line, "iso=", 4) && strlen(keep) + strlen(line) < sizeof keep - 1) strcat(keep, line);
        fclose(f);
    }
    WideCharToMultiByte(CP_UTF8, 0, path, -1, p8, sizeof p8, NULL, NULL);
    if ((f = _wfopen(ini, L"w"))) {
        fprintf(f, "iso=%s\n%s", p8, keep);
        fclose(f);
    }
}

/* The disc image: --iso "path", any .iso next to the program, DotR-PC.ini "iso=", an ISOs folder up to four levels up
 * (*Duelists of the Roses*.iso). Only the USA disc counts. In the window build a missing or wrong image brings a
 * message and a file dialog; the choice goes into DotR-PC.ini. 1 = found and left open. */
static int find_iso(const wchar_t *cmd, wchar_t *out)
{
    const wchar_t *p = wcsstr(cmd, L"--iso ");
    wchar_t pat[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    int wrong = 0, u;
    static const wchar_t *const ups[] = { L"..\\ISOs\\", L"..\\..\\ISOs\\", L"..\\..\\..\\ISOs\\",
                                          L"..\\..\\..\\..\\ISOs\\" };
    if (p) {
        int n = 0;
        p += 6;
        while (*p == L' ') p++;
        if (*p == L'"') { p++; while (p[n] && p[n] != L'"' && n < MAX_PATH - 1) n++; }
        else { while (p[n] && p[n] != L' ' && n < MAX_PATH - 1) n++; }
        wcsncpy(out, p, n); out[n] = 0;
        if (disc_ok(out, &wrong)) return 1;
    }
    swprintf(pat, MAX_PATH, L"%s*.iso", g_exe_dir);
    if ((h = FindFirstFileW(pat, &fd)) != INVALID_HANDLE_VALUE) {
        do {
            swprintf(out, MAX_PATH, L"%s%s", g_exe_dir, fd.cFileName);
            if (disc_ok(out, &wrong)) { FindClose(h); return 1; }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    ini_read("iso=", out);
    if (disc_ok(out, &wrong)) return 1;
    for (u = 0; u < 4; u++) {
        swprintf(pat, MAX_PATH, L"%s%s*Duelists of the Roses*.iso", g_exe_dir, ups[u]);
        if ((h = FindFirstFileW(pat, &fd)) != INVALID_HANDLE_VALUE) {
            swprintf(out, MAX_PATH, L"%s%s%s", g_exe_dir, ups[u], fd.cFileName);
            FindClose(h);
            if (disc_ok(out, &wrong)) return 1;
        }
    }
    while (rt_gui) {
        OPENFILENAMEW ofn;
        wchar_t pick[MAX_PATH] = L"";
        MessageBoxW(NULL, wrong ?
            L"The disc image found next to the game is not the right one.\n\n"
            L"This game needs your own disc image of Yu-Gi-Oh! The Duelists of the Roses, the USA version "
            L"(SLUS-20515), as an .iso file.\n\nPress OK and choose that .iso file. Next time it is found by itself." :
            L"The game needs your own disc image of Yu-Gi-Oh! The Duelists of the Roses, the USA version "
            L"(SLUS-20515), as an .iso file.\n\nPut the .iso file in the same folder as this program, or press OK "
            L"and choose it. Next time it is found by itself.",
            L"Yu-Gi-Oh! The Duelists of the Roses", MB_OK | MB_ICONINFORMATION);
        memset(&ofn, 0, sizeof ofn);
        ofn.lStructSize = sizeof ofn;
        ofn.lpstrFilter = L"Disc image (*.iso)\0*.iso\0All files\0*.*\0";
        ofn.lpstrFile = pick;
        ofn.nMaxFile = MAX_PATH;
        ofn.lpstrTitle = L"Choose your Yu-Gi-Oh! The Duelists of the Roses (USA) disc image";
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (!GetOpenFileNameW(&ofn)) { rt_gui_quiet = 1; return 0; }    /* cancelled: no second message */
        wrong = 0;
        if (disc_ok(pick, &wrong)) { wcscpy(out, pick); ini_write_iso(pick); return 1; }
        wrong = 1;
    }
    return 0;
}

#ifndef DOTR_NO_GAME
/* a pure game function (main_000.c), called both directly and through its PS2 address 0x00100FF0 */
extern float GCalc_Decrease(float a, float b, float c);
extern float ShFx_GetDrawSize(float size, float depth);
/* The EE's float rules in game code (runtime/eefloat.c): ShFx_GetDrawSize(1, 0) divides 1 by 0, which the EE turns
 * into +FLT_MAX (x86: +inf). */
static LONG CALLBACK float_veh(EXCEPTION_POINTERS *e)
{
    return ee_float_exception(e) ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
}

static int float_check(void)
{
    /* view 0 for a moment: a scratch object at the top of PS2 RAM with screen distance 800 (+0x44), so the result
     * is 1/0 * 800/800 = +FLT_MAX on the EE and +inf on x86 */
    volatile uint32_t *view0 = (volatile uint32_t *)PS2_PTR(0x0031BEB0);
    uint8_t *obj = (uint8_t *)PS2_PTR(0x01FFFF00), saved_obj[0x48];
    uint32_t saved_view = *view0;
    unsigned csr = _mm_getcsr();
    void *h;
    float got;
    int ok;
    memcpy(saved_obj, obj, sizeof saved_obj);
    *(float *)(obj + 0x44) = 800.0f;
    *view0 = 0x01FFFF00u;
    h = AddVectoredExceptionHandler(1, float_veh);
    ee_float_init();
    got = ShFx_GetDrawSize(1.0f, 0.0f);
    _mm_setcsr(csr);
    RemoveVectoredExceptionHandler(h);
    *view0 = saved_view;
    memcpy(obj, saved_obj, sizeof saved_obj);
    ok = got == FLT_MAX;
    con_printf("%s EE float rules in game code: 1/0 gives the largest float, not infinity (ShFx_GetDrawSize: %g)\n",
               ok ? "  [ok]  " : "  [FAIL]", got);
    return ok;
}

static int call_check(void)
{
    typedef float (*fn)(float, float, float);
    float direct = GCalc_Decrease(15.0f, 0.0f, 10.0f);
    float via_ps2 = ((fn)PS2_PTR(0x00100FF0))(15.0f, 0.0f, 10.0f);
    int ok = direct == via_ps2 && direct == 1.5f;
    con_printf("%s calling game code through its PS2 address (GCalc_Decrease at 00100FF0): %g, expected 1.5\n",
               ok ? "  [ok]  " : "  [FAIL]", via_ps2);
    if (!float_check()) ok = 0;
    if (crash_low_memory_check()) {
        con_printf("  [ok]   PS2 low memory: a null-pointer read gives 0, a write there reads back (no crash)\n");
    } else {
        con_printf("  [FAIL] PS2 low memory: a null-pointer read or write is not served\n");
        ok = 0;
    }
    return ok;
}
#else
static int call_check(void) { return 1; }
#endif

/* M0: prove the memory layout, the disc, the executable and the linked game code, with no window. */
static int selftest(dotr_memory_map *map, const wchar_t *cmd)
{
    wchar_t iso[MAX_PATH];
    char err[256], sum[41];
    elf_info info;
    int fails = 0;

    con_printf("DotR-PC self-test\n\n");
    if (map->ok) {
        con_printf("  [ok]   PS2 memory reserved at the console's addresses (RAM 0x00100000-0x01FFFFFF + scratchpad 0x70000000)\n");
    } else {
        con_printf("  [FAIL] PS2 memory layout: step %u, Windows error %u\n", map->error_step, map->error_code);
        fails++;
    }
    if (!find_iso(cmd, iso)) {
        con_printf("  [FAIL] no disc image of the USA game found (last tried: %ls)\n", iso);
        con_printf("         put the .iso next to DotR.exe, or  iso=<full path>  in DotR-PC.ini\n");
        return 1;
    }
    con_printf("  [ok]   disc image: %ls\n", iso);
    if (!map->ok) {
        con_printf("  [skip] game executable (needs the memory layout)\n");
        return 1;
    }
    if (!elf_load_from_iso("SLUS_205.15", &info, err, sizeof err)) {
        con_printf("  [FAIL] game executable: %s\n", err);
        return 1;
    }
    hex20(info.sha1, sum);
    if (strcmp(sum, SLUS_SHA1) == 0) {
        con_printf("  [ok]   game executable SLUS_205.15 (US retail, checksum matches)\n");
    } else {
        con_printf("  [FAIL] SLUS_205.15 checksum %s, expected %s (other release?)\n", sum, SLUS_SHA1);
        fails++;
    }
    con_printf("  [ok]   loaded into PS2 memory at %08X..%08X, entry %08X (%u overlay placeholders)\n",
               info.image_lo, info.image_hi, info.entry, info.placeholder_segments);
    if (!tramp_install(err, sizeof err)) {
        con_printf("  [FAIL] game code: %s\n", err);
        fails++;
    } else {
        con_printf("  [ok]   game code: %u PS2 functions compiled from our C as PC code, %u placeholders for the\n"
                   "         Sony library parts the PC layer replaces next (%u of 3,463 reachable by PS2 address)\n",
                   ps2_functab_native, ps2_functab_count - ps2_functab_native, ps2_functab_count);
        fails += !call_check();
    }
    con_printf("\n%s\n", fails ? "RESULT: FAIL (see above)" : "RESULT: OK");
    return fails ? 1 : 0;
}

#ifndef DOTR_NO_GAME
extern int ps2_main(void);   /* the game's main(), renamed in the objects */
static void game_main(void *arg)
{
    (void)arg;
    hw_log(2, "game main() starts");
    ps2_main();
    rt_stop_ok("game main() returned");
}

/* --rc-check: every duel module through the game's mwOverlayInit (lane R's recomp/rc_check.c), no game loop */
static void rc_check_main(void *arg)
{
    char msg[256];
    int bad;
    (void)arg;
    bad = rc_check_modules(iso_find, iso_read, msg, sizeof msg);
    con_printf("DotR-PC rc-check: %s\n", msg);
    if (bad) rt_fatal("rc-check failed (%d modules)", bad);
    if (!rc_oracle_init(iso_find, iso_read, msg, sizeof msg)) rt_fatal("rc-check: %s", msg);
    con_printf("DotR-PC rc-check: %s\n", msg);
    {
        /* the oracle on a few plain functions: original EE code vs the native port, same state */
        static const float f3[] = {15.0f, 0.0f, 10.0f};
        static const uint64_t i2[][2] = {{0, 0}, {1, 5}, {3, 17}};
        RcOracleResult r;
        int i, differ = 0;
        differ += rc_oracle_run(0x00100FF0u, NULL, 0, f3, 3, 0, &r) != 0;   /* GCalc_Decrease */
        con_printf("  oracle: %s\n", r.msg);
        for (i = 0; i < 3; i++) {
            differ += rc_oracle_run(0x00104520u, i2[i], 2, NULL, 0, 0, &r) != 0;   /* LkCheckFlag */
            con_printf("  oracle: %s\n", r.msg);
        }
        for (i = 1; i <= 2; i++) {      /* litodp (soft double from an integer): double results in one 64-bit $v0 */
            uint64_t a = (uint64_t)i;
            differ += rc_oracle_run(0x001A2EB0u, &a, 1, NULL, 0, 0, &r) != 0;
            con_printf("  oracle: %s\n", r.msg);
        }
        {   /* ShFx_GetDrawSize(500, 0): the EE float rule must give FLT_MAX on both sides */
            static const float fz[] = {500.0f, 0.0f};
            con_printf("  oracle: MXCSR before %04X\n", _mm_getcsr());
            differ += rc_oracle_run(0x001018E0u, NULL, 0, fz, 2, 0, &r) != 0;
            con_printf("  oracle: %s; MXCSR after %04X\n", r.msg, _mm_getcsr());
        }
        if (differ) rt_fatal("rc-check: the oracle found %d differences", differ);
    }
    rt_stop_ok("rc-check passed");
}

static int rc_check_boot(dotr_memory_map *map, const wchar_t *cmd)
{
    wchar_t iso[MAX_PATH];
    char err[256];
    elf_info info;
    if (!map->ok) { con_printf("DotR-PC: PS2 memory layout not available (run --selftest)\n"); return 1; }
    if (!find_iso(cmd, iso)) { con_printf("DotR-PC: no disc image of the USA game found (last tried: %ls)\n", iso); return 1; }
    if (!elf_load_from_iso("SLUS_205.15", &info, err, sizeof err)) { con_printf("DotR-PC: %s\n", err); return 1; }
    if (!tramp_install(err, sizeof err)) { con_printf("DotR-PC: %s\n", err); return 1; }
    rt_headless = 1;
    crash_install();
    rt_threads_run(rc_check_main, NULL);
    return 0;
}

/* --shots N: the displayed frame every N vblanks as shots\frame_NNNNN.bmp next to the exe (headless checks) */
int gfx_display_frame(const hw_u32 **rgba, int *w, int *h);
int gfx_write_bmp(const char *path, const hw_u32 *rgba, int w, int h);
static unsigned g_shot_every;
static void shot_vblank(void)
{
    const hw_u32 *px;
    int w, h;
    char path[MAX_PATH];
    unsigned n = rt_vblanks();
    static int readout_each = -1;
    extern int gfx_no_draw;
    if (readout_each < 0) readout_each = getenv("DOTR_READOUT_EACH") != NULL;
    if (readout_each && !gfx_no_draw) gfx_display_frame(&px, &w, &h);   /* a window's per-field cost, for timing */
    if (!g_shot_every || n % g_shot_every || !gfx_display_frame(&px, &w, &h)) return;
    snprintf(path, sizeof path, "%lsshots\\frame_%05u.bmp", g_exe_dir, n);
    if (!gfx_write_bmp(path, px, w, h)) hw_log(1, "shots: cannot write %s", path);
    {
        /* how much of GS memory holds anything: tells "nothing drawn" from "drawn but not displayed" */
        extern hw_u32 *gfx_gs_vram(void);
        const hw_u32 *v = gfx_gs_vram();
        unsigned i, nz = 0, lit = 0;
        for (i = 0; i < 4u * 1024 * 1024 / 4; i++) nz += v[i] != 0;
        for (i = 0; i < (unsigned)(w * h); i++) lit += (px[i] & 0xFFFFFF) != 0;
        hw_log(2, "shots: %s %dx%d, %u lit pixels, GS memory %u of 1048576 words non-zero", path, w, h, lit, nz);
    }
}
#endif

/* Run the game. --headless: no window (tests), --fast: no 60 Hz pacing, --frames N: stop after N vblanks,
 * --shots N: save the picture every N frames, --wav out.wav: record the sound. */
static int boot(dotr_memory_map *map, const wchar_t *cmd)
{
#ifdef DOTR_NO_GAME
    (void)map; (void)cmd;
    con_printf("DotR-PC: this build has no game code\n");
    return 1;
#else
    wchar_t iso[MAX_PATH];
    char err[256];
    elf_info info;
    gfx_config gc;
    const wchar_t *fr = wcsstr(cmd, L"--frames ");
    if (!map->ok) { con_printf("DotR-PC: PS2 memory layout not available (run --selftest)\n"); return 1; }
    if (!find_iso(cmd, iso)) { con_printf("DotR-PC: no disc image of the USA game found (last tried: %ls)\n", iso); return 1; }
    if (!elf_load_from_iso("SLUS_205.15", &info, err, sizeof err)) { con_printf("DotR-PC: %s\n", err); return 1; }
    if (!tramp_install(err, sizeof err)) { con_printf("DotR-PC: %s\n", err); return 1; }
    if (!rc_oracle_init(iso_find, iso_read, err, sizeof err))    /* original code for 3 module call targets */
        hw_log(1, "%s", err);
    rt_fast = has_arg(cmd, L"--fast");
#ifdef DOTR_DEV
    g_debug_menu = has_arg(cmd, L"--debug-menu");
#endif
    if (fr) rt_frame_limit = (unsigned)_wtoi(fr + 9);
    memset(&gc, 0, sizeof gc);
    gc.headless = has_arg(cmd, L"--headless");
    rt_headless = gc.headless;              /* watchdog: headless runs stop on a hang, the window keeps going */
    gc.no_draw = gc.headless && !wcsstr(cmd, L"--shots ");    /* nobody looks: keep GS state, skip drawing */
    crash_install();
    ini_to_env();
    if (!gfx_init(&gc)) { con_printf("DotR-PC: graphics could not start\n"); return 1; }
    {
        void report_init(void);
        report_init();                              /* F12 / stick-left reports, the flight recorder (runtime/report.c) */
    }
    {
        snd_config sc;
        static char wav[MAX_PATH];     /* sound keeps the path */
        const wchar_t *w = wcsstr(cmd, L"--wav ");
        sc.headless = gc.headless;
        sc.wav_path = NULL;
        if (w && swscanf(w + 6, L"%259S", wav) == 1) sc.wav_path = wav;    /* --wav out.wav: record the sound */
        hw_log(2, "boot: graphics up, starting sound");
        if (!snd_init(&sc)) hw_log(1, "sound could not start; running silent");
    }
    {
        /* --pad-script <file> (port 0), --pad-script2 <file> (port 1): runtime/padscript.c */
        static const wchar_t *const opt[2] = { L"--pad-script ", L"--pad-script2 " };
        int port;
        for (port = 0; port < 2; port++) {
            const wchar_t *ps = wcsstr(cmd, opt[port]);
            wchar_t script[MAX_PATH];
            int n = 0, r;
            if (!ps) continue;
            ps += wcslen(opt[port]);               /* a plain path, or a "quoted path with spaces" */
            while (*ps == L' ') ps++;
            if (*ps == L'"') { ps++; while (ps[n] && ps[n] != L'"' && n < MAX_PATH - 1) n++; }
            else { while (ps[n] && ps[n] != L' ' && n < MAX_PATH - 1) n++; }
            wcsncpy(script, ps, n); script[n] = 0;
            if (!script[0]) continue;
            r = rt_pad_script_load_port(script, port);
            if (r == 0) { con_printf("DotR-PC: pad script not readable: %ls\n", script); return 1; }
            if (r < 0) { con_printf("DotR-PC: pad script %ls line %d: not understood\n", script, -r); return 1; }
        }
    }
    if (!rt_bot_setup(cmd)) return 1;
    {
        const wchar_t *s = wcsstr(cmd, L"--shots ");
        if (s && (g_shot_every = (unsigned)_wtoi(s + 8)) != 0) {
            wchar_t dir[MAX_PATH];
            swprintf(dir, MAX_PATH, L"%sshots", g_exe_dir);
            CreateDirectoryW(dir, NULL);
            rt_on_vblank(shot_vblank);
        }
    }
    hw_log(2, "boot: %ls, frames %u, %s%s%s", iso, rt_frame_limit, gc.headless ? "headless" : "window",
           rt_fast ? ", fast" : "", g_debug_menu ? ", developer menu" : "");
    rt_threads_run(game_main, NULL);
    return 0;
#endif
}

__declspec(dllexport) int __cdecl dotr_main(dotr_memory_map *map, const wchar_t *cmd)
{
    wchar_t logpath[MAX_PATH];
    int rc;
    exe_dir();
    swprintf(logpath, MAX_PATH, L"%sDotR-PC.log", g_exe_dir);
    log_open(logpath);
    rt_gui = getenv("DOTR_GUI") != NULL;
    if (map->abi_version != DOTR_LOADER_ABI) {
        con_printf("DotR-PC: DotR.exe and dotr.dll are from different builds\n");
        return 4;
    }
    if (has_arg(cmd, L"--selftest")) {
        rc = selftest(map, cmd);
#ifndef DOTR_NO_GAME
    } else if (has_arg(cmd, L"--rc-check")) {
        rc = rc_check_boot(map, cmd);
#endif
    } else {
        rc = boot(map, cmd);
    }
    log_close();
    if (rc) rt_problem_box();
    return rc;
}
