/*
 * Headless bot duels: computer against computer, fixed seed, a log per duel, loud stops.
 *
 *   DotR.exe --boot --headless --fast --bot-duel <seed> [--bot-decks <a>,<b>] [--bot-map <n>] [--bot-log <file>]
 *
 * No menus: Main_Init starts the duel instead of the logos (hle/botduel.c, patched in by gen/rules.json), set up
 * like a Custom Duel with the game's three fixed decks (0, 1, 2; from the seed unless --bot-decks) and their
 * computer duelists. The duel start in SzGame_ModeInit calls rt_bot_duel_start (seeds the game's random generator)
 * and rt_bot_kind; the duel end in SzDuel_Act calls rt_bot_duel_end. Everything else is read from the duel state at
 * its PS2 address each frame:
 *   D_00317490 duel state, 0x350 bytes per side: life points u16 +0x33A, side kind u8 +0x33C (<0x20 computer,
 *   100/101 pad 0/1); +0xD50 winner (0, 1, 2 = draw), +0xD51 reason, +0xD53 turn count, +0xD54 turn owner.
 *   The random state is the u64 at *(u32 *)0x0028FA98 + 0xA8 (newlib's rand, seeded by srand = func_0019A5B0).
 *
 * Exit codes: 0 duel finished, 5 hang (no new turn for 20 game minutes), 6 impossible state, 7 the duel never
 * started, plus the runtime's own (3 crash, 4 no frames).
 */
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include "runtime.h"
#include "hle_rt.h"

#define DUEL        0x00317490u
#define SIDE(s)     (DUEL + (uint32_t)(s) * 0x350u)
#define LP(s)       (*(volatile uint16_t *)(uintptr_t)(SIDE(s) + 0x33A))
#define KIND(s)     (*(volatile uint8_t *)(uintptr_t)(SIDE(s) + 0x33C))
#define WINNER      (*(volatile uint8_t *)(uintptr_t)(DUEL + 0xD50))
#define REASON      (*(volatile uint8_t *)(uintptr_t)(DUEL + 0xD51))
#define TURN        (*(volatile uint8_t *)(uintptr_t)(DUEL + 0xD53))
#define OWNER       (*(volatile uint8_t *)(uintptr_t)(DUEL + 0xD54))
#define STATE_SIZE  0xD70u
#define RAND_STATE  ((volatile uint64_t *)(uintptr_t)(*(volatile uint32_t *)(uintptr_t)0x0028FA98u + 0xA8u))

#define HANG_FRAMES (60u * 60u * 20u)        /* 20 minutes of game time without a new turn */
#define START_FRAMES (60u * 60u * 2u)        /* the duel must start within 2 game minutes of boot */

static int g_on;
static unsigned long long g_seed;
static int g_com = -1;
static int g_deck[2] = { -1, -1 }, g_map = 46;
static int g_const[2] = { -1, -1 }, g_duelist[2] = { -1, -1 };   /* --bot-const a,b (decks 0..50), --bot-duelists x,y */    /* map 46: the first Custom Duel field */
static FILE *g_log;
static int g_started;
static unsigned g_start_frame, g_last_turn_frame;
static int g_last_turn = -1;
static int g_trace;
static int g_auto = -2;     /* --auto-duel [id] (dev builds): -2 off, -1 each human seat plays as the opponent's AI, else that AI */
static unsigned long long g_cpu_start;   /* the game thread's CPU time at the duel start (us) */
unsigned long long rt_game_cpu_us(void);   /* runtime/crash.c */
static int g_pad0;          /* --bot-pad0: side 0 played through pad 0 by the script below instead of the game's AI */

static uint64_t state_hash(void)
{
    const uint8_t *p = (const uint8_t *)(uintptr_t)DUEL;
    uint64_t h = 1469598103934665603ull;     /* FNV-1a */
    uint32_t i;
    for (i = 0; i < STATE_SIZE; i++) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

static void out(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    fp_state fs = fp_save();
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    hw_log(2, "bot: %s", buf);
    if (g_log) { fprintf(g_log, "%s\n", buf); fflush(g_log); }
    fp_restore(fs);
}

/* DOTR_BOT_DUMP=1: the hashed duel state, 32 bytes a line, at the duel's end; =2 also at every turn line
 * (compare two builds that play the same) */
static void dump_state(void)
{
    const uint8_t *p = (const uint8_t *)(uintptr_t)DUEL;
    uint32_t i, j;
    for (i = 0; i < STATE_SIZE; i += 32) {
        char line[80];
        for (j = 0; j < 32 && i + j < STATE_SIZE; j++) sprintf(line + 2 * j, "%02x", p[i + j]);
        out("state %04x %s", i, line);
    }
}

static void stop(int code, const char *why)
{
    out("STOP %s (frame %u)", why, rt_vblanks());
    con_printf("DotR-PC bot duel: %s\n", why);
    if (g_log) fclose(g_log);
    log_close();
    ExitProcess((UINT)code);
}

static void check(void)
{
    int s;
    for (s = 0; s < 2; s++) {
        if (LP(s) > 9999) { char m[96]; snprintf(m, sizeof m, "impossible: side %d has %u life points", s, LP(s)); stop(6, m); }
        if (KIND(s) >= 0x20 && !(s == 0 && g_pad0 && KIND(s) == 100)) {
            char m[96]; snprintf(m, sizeof m, "impossible: side %d has kind %u", s, KIND(s)); stop(6, m);
        }
    }
    if (OWNER > 1) { char m[64]; snprintf(m, sizeof m, "impossible: turn owner %u", OWNER); stop(6, m); }
}

void rt_bot_duel_end(int winner, int reason);

/* Side 0's player (the human seat, pad 0). The game's AI loops in that seat (each turn runs into its 600-input cap),
 * so a plain script plays it instead: on its turn press
 * Circle (the hand: "Hand" on the duel screen), Cross four times (pick the first card, place it, confirm),
 * then Start to end the turn, and go
 * round that plan again until the turn passes. Deterministic: it only looks at the duel state and the frame count. */
#define ACT (*(volatile uint8_t *)(uintptr_t)(DUEL + 0xD57))
static const char *const turn_plan[] = { "circle", "cross", "cross", "cross", "cross", "start" };
#define PLAN_N ((int)(sizeof turn_plan / sizeof turn_plan[0]))
int rt_bot_pad(char *buttons, int size)
{
    static int last_owner = -1, step;
    static unsigned next_frame;
    unsigned now = rt_vblanks();
    if (!g_on || !g_pad0 || !g_started || g_auto != -2) return 0;
    if (OWNER != last_owner) { last_owner = OWNER; step = 0; next_frame = now + 60; }
    snprintf(buttons, size, "-");
    if (OWNER != 0 || now < next_frame) return 1;
    if (now < next_frame + 6) {                 /* hold the button for 6 fields */
        snprintf(buttons, size, "%s", turn_plan[step]);
        return 1;
    }
    step = (step + 1) % PLAN_N;                 /* the turn did not pass: go round the plan again */
    next_frame = now + (step ? 30 : 90);
    return 1;
}

static void bot_vblank(void)
{
    unsigned now = rt_vblanks();
    if (!g_started) {
        if (now > START_FRAMES) stop(7, "the duel never started");
        return;
    }
    /* SzDuel_Act state 0x54.. is the end sequence; its result screen (0x5C) waits for a human's button, which a
     * computer-vs-computer duel never gives: the result is final here */
    if (*(volatile uint8_t *)(uintptr_t)(DUEL + 0xD57) >= 0x54) { rt_bot_duel_end(WINNER, REASON); return; }
    check();
    /* one line per hand-over: the turn count ticks before SzDuel_InitTurn passes the turn to the other side, so
     * a line on the count alone always showed the old owner */
    if ((TURN << 8 | OWNER) != g_last_turn) {
        g_last_turn = TURN << 8 | OWNER;
        g_last_turn_frame = now;
        out("turn %3u owner %u  lp %4u %4u  frame %6u  hash %016llx", TURN, OWNER, LP(0), LP(1), now,
            (unsigned long long)state_hash());
        {
            const char *d = getenv("DOTR_BOT_DUMP");
            if (d && *d == '2') dump_state();
        }
    } else if (now - g_last_turn_frame > HANG_FRAMES) {
        stop(5, "hang: no new turn for 20 game minutes");
    } else if (g_trace && (now - g_start_frame) % 300 == 0) {
        /* --bot-trace: where a slow turn is (SzDuel_Act state +0xD57, the duel's sub-step words +0xC5C/+0xC60) */
        out("  frame %6u  act %3u  owner %u  first %u  kinds %u %u  c5c %x  c60 %x  lp %4u %4u  hash %016llx", now,
            *(volatile uint8_t *)(uintptr_t)(DUEL + 0xD57), OWNER, *(volatile uint8_t *)(uintptr_t)(DUEL + 0xD52),
            KIND(0), KIND(1), *(volatile uint32_t *)(uintptr_t)(DUEL + 0xC5C),
            *(volatile uint32_t *)(uintptr_t)(DUEL + 0xC60), LP(0), LP(1), (unsigned long long)state_hash());
    }
}

/* boot: --bot-duel <seed> [--bot-com <id>] [--bot-log <file>] */
int rt_bot_setup(const wchar_t *cmd)
{
    const wchar_t *p = wcsstr(cmd, L"--bot-duel ");
    wchar_t path[MAX_PATH];
#ifdef DOTR_DEV
    {
        const wchar_t *a = wcsstr(cmd, L"--auto-duel");
        if (a) {
            a += 11;
            while (*a == L' ') a++;
            g_auto = (*a >= L'0' && *a <= L'9') ? _wtoi(a) : -1;
            hw_log(2, "auto-duel: the game's AI plays every human seat (%s)", g_auto < 0 ? "as the opponent's AI" : "one AI");
        }
    }
#endif
    if (!p) return 1;
    g_on = 1;
    g_seed = _wcstoui64(p + 11, NULL, 0);
    if ((p = wcsstr(cmd, L"--bot-com ")) != NULL) g_com = _wtoi(p + 10);
    if ((p = wcsstr(cmd, L"--bot-decks ")) != NULL) swscanf(p + 12, L"%d,%d", &g_deck[0], &g_deck[1]);
    if ((p = wcsstr(cmd, L"--bot-map ")) != NULL) g_map = _wtoi(p + 10);
    if ((p = wcsstr(cmd, L"--bot-const ")) != NULL) swscanf(p + 12, L"%d,%d", &g_const[0], &g_const[1]);
    if ((p = wcsstr(cmd, L"--bot-duelists ")) != NULL) swscanf(p + 15, L"%d,%d", &g_duelist[0], &g_duelist[1]);
    if (g_const[0] > 50 || g_const[1] > 50) { con_printf("DotR-PC: --bot-const takes decks 0..50\n"); return 0; }
    g_trace = wcsstr(cmd, L"--bot-trace") != NULL;
    g_pad0 = wcsstr(cmd, L"--bot-pad0") != NULL;
    if ((p = wcsstr(cmd, L"--bot-log ")) != NULL) {
        int n = 0;
        p += 10;
        while (*p == L' ') p++;
        if (*p == L'"') { p++; while (p[n] && p[n] != L'"' && n < MAX_PATH - 1) n++; }   /* a path with spaces */
        else { while (p[n] && p[n] != L' ' && n < MAX_PATH - 1) n++; }
        wcsncpy(path, p, n);
        path[n] = 0;
        g_log = _wfopen(path, L"w");
    } else {
        swprintf(path, MAX_PATH, L"botduel-%llu.log", g_seed);
        g_log = _wfopen(path, L"w");
    }
    if (!g_log) { con_printf("DotR-PC: bot duel log not writable: %ls\n", path); return 0; }
    /* the opening movie plays inside the game's mode code, so the main loop (and the duel start) waits for it:
     * Start skips it as a player would. Nothing else reads the pad: both duel sides are the computer. */
    rt_pad_script_add(600, "start");
    rt_pad_script_add(606, "-");
    rt_on_vblank(bot_vblank);
    return 1;
}

/* hle/botduel.c, from Main_Init: which fixed decks (0..2) and field; 0 = not a bot run (boot normally) */
/* the computer duelist that goes with a built-in deck: deck 26+n is duelist n's (the Custom Duel menu,
 * big_func_002354A0.c), 48-50 are the Custom Duel decks of duelists 23-25; decks 0-25 have none (-1) */
static int duelist_of(int deck) { return deck >= 26 && deck <= 47 ? deck - 26 : deck >= 48 && deck <= 50 ? deck - 25 : -1; }

/* hle/botduel.c: the built-in decks (SzData_SetupFromConstDeck 0..50), the computer duelists and the field */
int rt_bot_choice(int *deck0, int *deck1, int *duel0, int *duel1, int *map)
{
    int i;
    if (!g_on) return 0;
    for (i = 0; i < 2; i++) {
        int d = g_deck[i] >= 0 && g_deck[i] < 3 ? g_deck[i] : (int)(i ? g_seed / 3 % 3 : g_seed % 3);
        *(i ? deck1 : deck0) = g_const[i] >= 0 ? g_const[i] : 48 + d;
    }
    *duel0 = g_duelist[0] >= 0 ? g_duelist[0] : duelist_of(*deck0);
    *duel1 = g_duelist[1] >= 0 ? g_duelist[1] : duelist_of(*deck1);
    if (*duel0 < 0) *duel0 = *duel1 >= 0 ? *duel1 : 0;               /* a deck with no duelist: the other side's */
    if (*duel1 < 0) *duel1 = *duel0;
    *map = g_map;
    out("starting the duel: decks %d vs %d, duelists %d vs %d, field %d (frame %u)", *deck0, *deck1, *duel0, *duel1,
        *map, rt_vblanks());
    return 1;
}

/* called by SzGame_ModeInit (duel mode) before the sides are set and SzDuel_Init runs */
void rt_bot_duel_start(void)
{
    if (!g_on) return;
    *RAND_STATE = g_seed & 0xFFFFFFFFull;     /* what srand(seed) does (func_0019A5B0) */
    g_started = 1;
    g_start_frame = g_last_turn_frame = rt_vblanks();
    g_cpu_start = rt_game_cpu_us();
    out("duel start: seed %llu, frame %u", g_seed, g_start_frame);
    {
        const char *w = getenv("DOTR_BOT_WATCH");
        void crash_watch_write(uint32_t addr);
        if (w && *w == '*') {          /* "*PTR+OFF": the word at PTR (PS2 address, hex) plus OFF */
            char *e;
            uint32_t ptr = (uint32_t)strtoul(w + 1, &e, 16);
            uint32_t off = *e == '+' ? (uint32_t)strtoul(e + 1, NULL, 16) : 0;
            crash_watch_write(*(volatile uint32_t *)(uintptr_t)ptr + off);
        } else if (w) crash_watch_write((uint32_t)strtoul(w, NULL, 16));
    }
}

/* a side's kind for SzDuel_SetPlayer: in a bot duel a human side (pad 0/1) becomes the computer */
int rt_bot_kind(int side, int kind, int other)
{
    int k = kind;
    if (!g_on) {
        if (g_auto == -2 || kind < 0x20) return kind;          /* --auto-duel: a human seat becomes the computer */
        k = g_auto >= 0 ? g_auto : other < 0x20 ? other : 0;
        hw_log(2, "auto-duel: side %d (pad %d) plays as computer %d", side, kind - 100, k);
        return k;
    }
    if (side == 0 && g_pad0 && g_auto == -2) {
        out("side 0: kind %d -> pad 0 (scripted)", kind);
        return 100;
    }
    if (side == 0 && g_pad0) {                  /* --auto-duel test: a human seat, which --auto-duel hands to the AI */
        k = g_auto >= 0 ? g_auto : other < 0x20 ? other : 0;
        out("side 0: kind %d -> pad 0 -> auto-duel computer %d", kind, k);
        return k;
    }
    if (kind < 0x20) return kind;
    if (g_com >= 0) k = g_com;
    else k = other < 0x20 ? other : 0;
    out("side %d: kind %d -> computer %d", side, kind, k);
    return k;
}

/* called by SzDuel_Act as the duel ends (winner 0/1, 2 = draw) */
void rt_bot_duel_end(int winner, int reason)
{
    char m[160];
    if (!g_on) return;
    if (winner < 0 || winner > 2) { snprintf(m, sizeof m, "impossible: winner %d", winner); stop(6, m); }
    if (getenv("DOTR_BOT_DUMP")) dump_state();
    out("duel end: winner %s, reason %d, turns %u, lp %u %u, frames %u, hash %016llx",
        winner == 2 ? "draw" : winner ? "side 1" : "side 0", reason, TURN, LP(0), LP(1),
        rt_vblanks() - g_start_frame, (unsigned long long)state_hash());
    {   /* the gate's speed check: the game thread's own CPU time per field of the duel (a busy PC does not count) */
        unsigned f = rt_vblanks() - g_start_frame;
        out("cpu: %llu us per field (game thread, %u fields)", (rt_game_cpu_us() - g_cpu_start) / (f ? f : 1), f);
    }
    snprintf(m, sizeof m, "finished: %s", winner == 2 ? "draw" : winner ? "side 1 won" : "side 0 won");
    out("STOP %s", m);
    con_printf("DotR-PC bot duel: %s\n", m);
    if (g_log) fclose(g_log);
    g_log = NULL;
    rt_stop_ok("bot duel finished");
}
