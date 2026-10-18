/*
 * Reports (the owner's "elite logging", dotr-big/LOGGING-PLAN.md step 1):
 *
 *   - a flight recorder: the last ~60 s of events in memory (every log line, game-mode changes, duel start/end, one
 *     timing line per field), written out only in a report;
 *   - Reports\<time>-<kind>\ next to the exe on a crash, a watchdog hang, F12 in the window or the pad's right stick
 *     pushed LEFT: REPORT.txt (a plain-English first line, the game state, the call path for crashes/hangs, the last
 *     60 s), picture.png and the GS capture (lane C's gfx_save_picture / gfx_capture_to / gfx_capture_now); the
 *     newest 20 report folders are kept;
 *   - names in that first line come from the player's own disc: the game's message table in the loaded SLUS_205.15
 *     is decoded at run time (card 0x140 + id, duelist 0xC4 + n), so dotr.dll carries no game text.
 */
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include "runtime.h"
#include "hle_rt.h"

/* lane C's gfx hooks (gfx/gfx.h) */
int gfx_save_picture(const char *png_path);
void gfx_capture_to(const char *dir);
int gfx_capture_now(const char *dir);
void gfx_set_f12_handler(void (*fn)(void));

unsigned long long rt_game_cpu_us(void);              /* runtime/crash.c */
const char *rc_current_module(void);                  /* recomp/ */
#ifndef DOTR_NO_GAME
extern long long SzGame_GetMode(int which);
extern int SzGame_GetMenuNum(int which);
extern int SzDuel_Xax2New(int card);
#endif

#define DUEL 0x00317490u
#define U8(a)  (*(volatile uint8_t *)(uintptr_t)(a))
#define U16(a) (*(volatile uint16_t *)(uintptr_t)(a))

/* ---- the game's text, from the loaded executable ---------------------------------------------------------- */
#define MSG_OFFS   0x002D19D0u
#define MSG_STREAM 0x002D49D4u
#define MSG_COUNT  ((MSG_STREAM - MSG_OFFS) / 4)

static const struct { unsigned short glyph; char ch; } glyph_map[] = {       /* half-width glyphs (expert/tools) */
    {0x70, ' '}, {0xA2, '-'}, {0xA4, '.'}, {0x02, ','}, {0x9B, '#'}, {0x98, '0'}, {0x8F, '1'}, {0x90, '2'},
    {0x91, '3'}, {0x92, '4'}, {0x93, '5'}, {0x94, '6'}, {0x95, '7'}, {0x96, '8'}, {0x97, '9'}, {0x8D, '('},
    {0x8E, ')'}, {0x99, '.'}, {0x9A, '"'}, {0x9F, '\''}, {0x151, '?'}, {0x9E, '&'}, {0x152, ':'}, {0x170, ','},
    {0x16A, '['}, {0x16B, ']'}, {0xA5, '/'}, {0xA0, '('}, {0xA1, ')'}, {0x9C, '$'}, {0x9D, '%'},
};

static void put_glyph(unsigned w, char *out, int *n, int cap)
{
    unsigned c = w & 0x1FFF;
    char ch = 0;
    int i;
    if (w & 0x2000) {
        if (c >= 0x56 && c < 0x56 + 26) ch = (char)('A' + c - 0x56);
        else if (c >= 0x73 && c < 0x73 + 26) ch = (char)('a' + c - 0x73);
        else for (i = 0; i < (int)(sizeof glyph_map / sizeof glyph_map[0]); i++) if (glyph_map[i].glyph == c) ch = glyph_map[i].ch;
    }
    if (!ch) ch = '?';
    if (*n < cap - 1) out[(*n)++] = ch;
}

static int msg_one(unsigned q, char *out, int *n, int cap, int depth, int *end);

/* words decoded from stream index p until an end flag (msgdump.py's trace) */
static void msg_trace(unsigned p, char *out, int *n, int cap, int depth)
{
    unsigned k = 0;
    if (depth > 8) return;
    for (; k < 4096; ) {
        unsigned w = U16(MSG_STREAM + 2 * (p + k)), c = w & 0x1FFF;
        if (w & 0x4000) {
            unsigned ref = U16(MSG_STREAM + 2 * (p + k + 1));
            int count = (int)(c & 0x3F), endhit = 0;
            unsigned q = *(volatile int32_t *)(uintptr_t)(MSG_OFFS + 4 * (ref & 0x3FFF)) + ((c >> 6) & 0x7F);
            while (count > 0) {
                int m = msg_one(q, out, n, cap, depth + 1, &endhit);
                if (endhit) break;
                count -= m;
                q += m;
            }
            k += 2;
            if (ref & 0x8000) return;
            continue;
        }
        put_glyph(w, out, n, cap);
        k++;
        if (w & 0x8000) return;
    }
}

static int msg_one(unsigned q, char *out, int *n, int cap, int depth, int *end)
{
    unsigned w = U16(MSG_STREAM + 2 * q), c = w & 0x1FFF;
    if (depth > 8) { *end = 1; return 1; }
    if (w & 0x4000) {
        unsigned ref = U16(MSG_STREAM + 2 * (q + 1));
        int count = (int)(c & 0x3F), endhit = 0;
        unsigned r = *(volatile int32_t *)(uintptr_t)(MSG_OFFS + 4 * (ref & 0x3FFF)) + ((c >> 6) & 0x7F);
        while (count > 0) {
            int m = msg_one(r, out, n, cap, depth + 1, &endhit);
            if (endhit) break;
            count -= m;
            r += m;
        }
        *end = (ref & 0x8000) != 0;
        return 2;
    }
    put_glyph(w, out, n, cap);
    *end = (w & 0x8000) != 0;
    return 1;
}

/* message id -> text (as far as the glyph table goes; '?' for others) */
static const char *msg_text(unsigned id, char *out, int cap)
{
    int n = 0;
    out[0] = 0;
    if (id >= MSG_COUNT) return out;
    msg_trace((unsigned)*(volatile int32_t *)(uintptr_t)(MSG_OFFS + 4 * id), out, &n, cap, 0);
    while (n > 0 && out[n - 1] == ' ') n--;
    out[n] = 0;
    return out;
}

static const char *card_name(int card, char *out, int cap)
{
    if (card < 0 || card >= 0x356) { snprintf(out, cap, "card %d", card); return out; }
    return msg_text(0x140u + (unsigned)card, out, cap);
}

/* duelists 0-21 have names (messages 0xC4..0xD9; 0xDA on are places); 22-31 are computer players without one
 * (23-25: the Custom Duel decks' computers) */
static const char *duelist_name(int n, char *out, int cap)
{
    if (n >= 0 && n <= 21) return msg_text(0xC4u + (unsigned)n, out, cap);
    snprintf(out, cap, n >= 23 && n <= 25 ? "the Custom Duel computer (%d)" : "computer player %d", n);
    return out;
}

/* ---- the flight recorder ----------------------------------------------------------------------------------- */
#define REC_N    8192
#define REC_TEXT 152
static struct { unsigned field; char text[REC_TEXT]; } g_rec[REC_N];
static volatile LONG g_rec_next;

void report_record(const char *fmt, ...)
{
    LONG i = InterlockedIncrement(&g_rec_next) - 1;
    va_list ap;
    fp_state fs = fp_save();
    g_rec[i & (REC_N - 1)].field = rt_vblanks();
    va_start(ap, fmt);
    vsnprintf(g_rec[i & (REC_N - 1)].text, REC_TEXT, fmt, ap);
    va_end(ap);
    fp_restore(fs);
}

/* the game modes in plain words (SzGame_ModeAct's switch) */
static const char *mode_words(int m)
{
    switch (m) {
    case -1: case -2: return "the developer menu";
    case 0: return "the developer duel test";
    case 1: return "a duel";
    case 5: return "the 3D battle test";
    case 6: return "deck building";
    case 7: return "the 3D card viewer";
    case 8: return "the trade screen";
    case 9: return "Custom Duel";
    case 10: return "the title screen and its menu";
    case 11: return "the campaign map";
    case 12: return "a debug screen";
    case 13: return "the map editor";
    case 14: return "a duelist's face screen";
    case 15: return "the slot machine";
    case 18: return "the card test";
    case 19: return "the camera test";
    case 36: return "the memory card check";
    }
    return "a game screen";
}

static void report_make(const char *kind, const char *why, const char *callpath, int picture, int capture);
static int g_mode = 0x7FFFFFFF, g_duel_live;
static unsigned long long g_cpu_last;
static double g_wall_last;

static int game_mode(void)
{
#ifndef DOTR_NO_GAME
    return (int)SzGame_GetMode(0);
#else
    return 0;
#endif
}

/* every field (runtime/threads.c vblank): timing line, mode changes, duel start/end */
void report_vblank(void)
{
    unsigned long long cpu = rt_game_cpu_us();
    double wall = rt_seconds();
    int m = game_mode(), turn = U8(DUEL + 0xD53);
    if (g_wall_last > 0)
        report_record("field: game thread %llu.%llu ms, %u ms wall", (cpu - g_cpu_last) / 1000, (cpu - g_cpu_last) / 100 % 10,
                      (unsigned)((wall - g_wall_last) * 1000.0));
    g_cpu_last = cpu;
    g_wall_last = wall;
#ifdef DOTR_DEV
    {   /* DOTR_REPORT_AT=<field> (dev builds): a test report at that field, as F12 would make it */
        static int at = -2;
        if (at == -2) { const char *e = getenv("DOTR_REPORT_AT"); at = e ? atoi(e) : -1; }
        if (at > 0 && rt_vblanks() == (unsigned)at) report_make("test", "A test report", NULL, 1, 1);
    }
    {   /* DOTR_CRASH_AT=<field> (dev builds): a deliberate crash on the game thread (checks the crash report) */
        static int at = -2;
        if (at == -2) { const char *e = getenv("DOTR_CRASH_AT"); at = e ? atoi(e) : -1; }
        if (at > 0 && rt_vblanks() == (unsigned)at) *(volatile int *)(uintptr_t)0x7FFF0000u = 1;
    }
#endif
    if (m != g_mode) {
        report_record("mode %d -> %d (%s)", g_mode == 0x7FFFFFFF ? 0 : g_mode, m, mode_words(m));
        g_mode = m;
    }
    if (m == 1 && !g_duel_live && turn > 0) {
        char a[64], b[64];
        g_duel_live = 1;
        report_record("duel under way: %s (%u LP) vs %s (%u LP), turn %d",
                      U8(DUEL + 0x33C) < 0x20 ? duelist_name(U8(DUEL + 0x33C), a, sizeof a) : "the player", U16(DUEL + 0x33A),
                      U8(DUEL + 0x350 + 0x33C) < 0x20 ? duelist_name(U8(DUEL + 0x350 + 0x33C), b, sizeof b) : "the player",
                      U16(DUEL + 0x350 + 0x33A), turn);
    } else if (m != 1 && g_duel_live) {
        g_duel_live = 0;
        report_record("duel over: winner %d, reason %d, LP %u vs %u, turn %d", U8(DUEL + 0xD50), U8(DUEL + 0xD51),
                      U16(DUEL + 0x33A), U16(DUEL + 0x350 + 0x33A), turn);
    }
}

/* ---- report folders ------------------------------------------------------------------------------------------ */
static char g_exe[MAX_PATH];

static void exe_folder(void)
{
    char *s;
    if (g_exe[0]) return;
    GetModuleFileNameA(NULL, g_exe, MAX_PATH);
    s = strrchr(g_exe, '\\');
    if (s) s[1] = 0;
}

/* keep the newest `keep` entries (names sort by time) matching pattern in folder */
static int cmp_names(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }
void report_prune(const char *folder, const char *pattern, int keep, int dirs)
{
    static char names[256][MAX_PATH];
    char pat[MAX_PATH], path[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    int n = 0, i;
    snprintf(pat, MAX_PATH, "%s\\%s", folder, pattern);
    if ((h = FindFirstFileA(pat, &fd)) == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == '.' || !(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != !dirs) continue;
        if (n < 256) snprintf(names[n++], MAX_PATH, "%s", fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    qsort(names, (size_t)n, MAX_PATH, cmp_names);
    for (i = 0; i < n - keep; i++) {
        snprintf(path, MAX_PATH, "%s\\%s", folder, names[i]);
        if (dirs) {
            char sub[MAX_PATH];
            snprintf(pat, MAX_PATH, "%s\\*", path);
            if ((h = FindFirstFileA(pat, &fd)) != INVALID_HANDLE_VALUE) {
                do {
                    if (fd.cFileName[0] == '.') continue;
                    snprintf(sub, MAX_PATH, "%s\\%s", path, fd.cFileName);
                    DeleteFileA(sub);
                } while (FindNextFileA(h, &fd));
                FindClose(h);
            }
            RemoveDirectoryA(path);
        } else {
            DeleteFileA(path);
        }
    }
}

/* the plain-English first line */
static void first_line(const char *what, char *out, int cap)
{
    int m = game_mode();
    char a[64], b[64], c[64];
    const char *mod = rc_current_module();
    if (m == 1) {
        int k0 = U8(DUEL + 0x33C), k1 = U8(DUEL + 0x350 + 0x33C);
        const char *opp = k1 < 0x20 ? duelist_name(k1, a, sizeof a) : k0 < 0x20 ? duelist_name(k0, a, sizeof a) : "another player";
        snprintf(out, cap, "%s in a duel against %s, turn %d, life points %u to %u", what, opp, U8(DUEL + 0xD53),
                 U16(DUEL + 0x33A), U16(DUEL + 0x350 + 0x33A));
    } else {
        snprintf(out, cap, "%s in %s", what, mode_words(m));
    }
#ifndef DOTR_NO_GAME
    /* b### / m### modules belong to a card: file = SzDuel_Xax2New(card) in SzEffect_LoadModule (main_050.c), so
     * card = the card list's entry ###; c### is an effect number, not a card (lane G) */
    if (mod && (mod[0] == 'b' || mod[0] == 'm') && atoi(mod + 1) > 0) {
        int card = SzDuel_Xax2New(atoi(mod + 1));
        if (card != 0x3E7) {
            size_t l = strlen(out);
            snprintf(out + l, cap - (int)l, ", while %s's effect or attack was playing", card_name(card, c, sizeof c));
        }
    }
#endif
    (void)b;
}

static volatile LONG g_in_report;
static DWORD g_last_manual;

/* kind: "crash", "hang", "F12", "stick"; why: the first-line lead-in; callpath: lines for crashes/hangs or NULL;
 * picture: 1 = gfx_save_picture is safe here (the game thread); capture: 0 none, 1 next field, 2 now (crash/hang) */
static void report_make(const char *kind, const char *why, const char *callpath, int picture, int capture)
{
    char dir[MAX_PATH], path[MAX_PATH], line[400];
    SYSTEMTIME t;
    FILE *f;
    LONG i, n;
    unsigned now = rt_vblanks(), from = now > 3600 ? now - 3600 : 0;
    fp_state fs;
    if (InterlockedExchange(&g_in_report, 1)) return;     /* one at a time; a crash inside a report writes no other */
    fs = fp_save();
    exe_folder();
    GetLocalTime(&t);
    snprintf(dir, MAX_PATH, "%sReports", g_exe);
    CreateDirectoryA(dir, NULL);
    report_prune(dir, "*", 19, 1);
    snprintf(dir, MAX_PATH, "%sReports\\%04d-%02d-%02d_%02d-%02d-%02d-%s", g_exe, t.wYear, t.wMonth, t.wDay, t.wHour,
             t.wMinute, t.wSecond, kind);
    CreateDirectoryA(dir, NULL);
    snprintf(path, MAX_PATH, "%s\\REPORT.txt", dir);
    first_line(why, line, sizeof line);
    hw_log(0, "report: %s -> %s", line, dir);
    if ((f = fopen(path, "w")) != NULL) {
        const char *mod = rc_current_module();
        int m = game_mode();
        fprintf(f, "%s.\n\n", line);
        fprintf(f, "Please send this whole folder to E (the bugs hub). DotR-PC.log next to the game has the full session.\n\n");
        fprintf(f, "== The game at that moment\n");
        fprintf(f, "field %u, game mode %d (%s), module %s%s\n", now, m, mode_words(m), mod ? mod : "none",
                mod && mod[0] == 'c' ? " (an effect module: effect number, not a card)" : "");
        if (m == 1 || g_duel_live) {
            char a[64], b[64];
            int s;
            fprintf(f, "duel: turn %d, side %d to play, state %02X\n", U8(DUEL + 0xD53), U8(DUEL + 0xD54), U8(DUEL + 0xD57));
            for (s = 0; s < 2; s++) {
                int k = U8(DUEL + 0x350 * s + 0x33C);
                fprintf(f, "  side %d: %s, %u life points\n", s,
                        k < 0x20 ? duelist_name(k, a, sizeof a) : (snprintf(b, sizeof b, "player (pad %d)", k - 100), b),
                        U16(DUEL + 0x350 * s + 0x33A));
            }
        }
#ifndef DOTR_NO_GAME
        fprintf(f, "menu numbers: %d %d %d %d %d\n", SzGame_GetMenuNum(0), SzGame_GetMenuNum(1), SzGame_GetMenuNum(2),
                SzGame_GetMenuNum(3), SzGame_GetMenuNum(4));
#endif
        if (callpath && callpath[0]) fprintf(f, "\n== Where the game was (call path)\n%s\n", callpath);
        fprintf(f, "\n== The last 60 seconds (field: event)\n");
        n = g_rec_next;
        for (i = n > REC_N ? n - REC_N : 0; i < n; i++) {
            if (g_rec[i & (REC_N - 1)].field < from) continue;
            if (!strncmp(g_rec[i & (REC_N - 1)].text, "field:", 6) && now - g_rec[i & (REC_N - 1)].field > 600) continue;
            fprintf(f, "%6u: %s\n", g_rec[i & (REC_N - 1)].field, g_rec[i & (REC_N - 1)].text);
        }
        fclose(f);
    }
    if (picture) {
        snprintf(path, MAX_PATH, "%s\\picture.png", dir);
        gfx_save_picture(path);
    }
    if (capture == 1) gfx_capture_to(dir);
    else if (capture == 2) gfx_capture_now(dir);
    fp_restore(fs);
    InterlockedExchange(&g_in_report, 0);
}

/* F12 (lane C calls it on the game thread) and the right stick pushed left (runtime/pad.c, the game thread):
 * at most one every 3 seconds */
static void manual(const char *kind, const char *why)
{
    DWORD now = GetTickCount();
    if (g_last_manual && now - g_last_manual < 3000) return;
    g_last_manual = now;
    report_make(kind, why, NULL, 1, 1);
}
static void on_f12(void) { manual("F12", "F12 pressed"); }
void report_stick(void) { manual("stick", "The right stick pushed left"); }

/* runtime/crash.c: on the crashing thread; the call path lines are in the recorder already */
void report_crash(const char *callpath, int on_game_thread)
{
    report_make("crash", "The game crashed", callpath, on_game_thread, 2);
}

/* runtime/crash.c watchdog thread, the game thread suspended: no picture (gfx belongs to the game thread), the GS
 * state through lane C's crash-safe gfx_capture_now (it gives up on a wedged GPU after 2 s) */
void report_hang(const char *callpath)
{
    report_make("hang", "The game stopped answering", callpath, 0, 2);
}
void report_init(void)
{
    gfx_set_f12_handler(on_f12);
    report_record("session start");
}
