/*
 * Controllers for scePadRead (hle/pad.c): an XInput pad per port, and the keyboard on port 0 while the game
 * window has focus. --pad-script / --pad-script2 feed port 0 / 1 from a script instead (headless checks, testers):
 * runtime/padscript.c.
 *
 * Keyboard: arrows = d-pad, Z = Cross, X = Circle, A = Square, S = Triangle, Q/W = L1/R1, E/R = L2/R2,
 * Enter = Start, Right Shift = Select.
 */
#include <windows.h>
#include <xinput.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "runtime.h"
#include "hle_rt.h"

/* DualShock 2 button bits, active high here (the pad reply inverts them) */
enum {
    B_SELECT = 0x0001, B_L3 = 0x0002, B_R3 = 0x0004, B_START = 0x0008, B_UP = 0x0010, B_RIGHT = 0x0020,
    B_DOWN = 0x0040, B_LEFT = 0x0080, B_L2 = 0x0100, B_R2 = 0x0200, B_L1 = 0x0400, B_R1 = 0x0800,
    B_TRIANGLE = 0x1000, B_CIRCLE = 0x2000, B_CROSS = 0x4000, B_SQUARE = 0x8000
};

static const struct { const char *name; unsigned bit; } names[] = {
    {"select", B_SELECT}, {"l3", B_L3}, {"r3", B_R3}, {"start", B_START}, {"up", B_UP}, {"right", B_RIGHT},
    {"down", B_DOWN}, {"left", B_LEFT}, {"l2", B_L2}, {"r2", B_R2}, {"l1", B_L1}, {"r1", B_R1},
    {"triangle", B_TRIANGLE}, {"circle", B_CIRCLE}, {"cross", B_CROSS}, {"square", B_SQUARE},
};

typedef DWORD (WINAPI *xinput_get_t)(DWORD, XINPUT_STATE *);
static xinput_get_t xget;
static int xtried;

static struct { unsigned frame, buttons; } *script;
static int script_n, script_cap, script_on;

/* "cross+up" -> button bits; "-" -> none */
static unsigned buttons_of(const char *buttons)
{
    char btn[200];
    char *p, *ctx = NULL;
    unsigned b = 0;
    snprintf(btn, sizeof btn, "%s", buttons);
    for (p = strtok_s(btn, "+", &ctx); p; p = strtok_s(NULL, "+", &ctx)) {
        int i;
        for (i = 0; i < (int)(sizeof names / sizeof names[0]); i++)
            if (!_stricmp(p, names[i].name)) b |= names[i].bit;
    }
    return b;
}

/* one script line: from `frame` on, port 0 holds `buttons` (names joined by '+', "-" for none) */
int rt_pad_script_add(unsigned frame, const char *buttons)
{
    unsigned b = buttons_of(buttons);
    if (script_n == script_cap) {
        script_cap = script_cap ? script_cap * 2 : 64;
        script = realloc(script, (size_t)script_cap * sizeof *script);
        if (!script) return 0;
    }
    script[script_n].frame = frame;
    script[script_n].buttons = b;
    script_n++;
    script_on = 1;
    return 1;
}

unsigned rt_pad_buttons_of(const char *buttons) { return buttons_of(buttons); }

/* --pad-script: port 0 from a script file (runtime/padscript.c: frame lines and the v2 commands) */
int rt_pad_script_load(const wchar_t *path) { return rt_pad_script_load_port(path, 0); }

static unsigned script_buttons(void)
{
    unsigned now = rt_vblanks(), b = 0;
    int i;
    for (i = 0; i < script_n && script[i].frame <= now; i++) b = script[i].buttons;
    return b;
}

static unsigned keyboard(void)
{
    static const struct { int vk; unsigned bit; } map[] = {
        {VK_UP, B_UP}, {VK_DOWN, B_DOWN}, {VK_LEFT, B_LEFT}, {VK_RIGHT, B_RIGHT}, {'Z', B_CROSS},
        {'X', B_CIRCLE}, {'A', B_SQUARE}, {'S', B_TRIANGLE}, {'Q', B_L1}, {'W', B_R1}, {'E', B_L2},
        {'R', B_R2}, {VK_RETURN, B_START}, {VK_RSHIFT, B_SELECT},
    };
    unsigned b = 0;
    int i;
    if (!gfx_window_focused()) return 0;
    for (i = 0; i < (int)(sizeof map / sizeof map[0]); i++)
        if (GetAsyncKeyState(map[i].vk) & 0x8000) {
            if (map[i].vk == VK_RETURN && (GetAsyncKeyState(VK_MENU) & 0x8000)) continue;   /* Alt+Enter: full screen */
            b |= map[i].bit;
        }
    return b;
}

static int xinput(int port, unsigned *b, unsigned char axes[4])
{
    XINPUT_STATE s;
    WORD w;
    if (!xtried) {
        HMODULE m = LoadLibraryW(L"xinput1_4.dll");
        if (!m) m = LoadLibraryW(L"xinput9_1_0.dll");
        if (m) xget = (xinput_get_t)GetProcAddress(m, "XInputGetState");
        xtried = 1;
    }
    if (!xget || xget((DWORD)port, &s) != ERROR_SUCCESS) return 0;
    w = s.Gamepad.wButtons;
    if (w & XINPUT_GAMEPAD_DPAD_UP) *b |= B_UP;
    if (w & XINPUT_GAMEPAD_DPAD_DOWN) *b |= B_DOWN;
    if (w & XINPUT_GAMEPAD_DPAD_LEFT) *b |= B_LEFT;
    if (w & XINPUT_GAMEPAD_DPAD_RIGHT) *b |= B_RIGHT;
    if (w & XINPUT_GAMEPAD_START) *b |= B_START;
    if (w & XINPUT_GAMEPAD_BACK) *b |= B_SELECT;
    if (w & XINPUT_GAMEPAD_LEFT_THUMB) *b |= B_L3;
    if (w & XINPUT_GAMEPAD_RIGHT_THUMB) *b |= B_R3;
    if (w & XINPUT_GAMEPAD_LEFT_SHOULDER) *b |= B_L1;
    if (w & XINPUT_GAMEPAD_RIGHT_SHOULDER) *b |= B_R1;
    if (w & XINPUT_GAMEPAD_A) *b |= B_CROSS;
    if (w & XINPUT_GAMEPAD_B) *b |= B_CIRCLE;
    if (w & XINPUT_GAMEPAD_X) *b |= B_SQUARE;
    if (w & XINPUT_GAMEPAD_Y) *b |= B_TRIANGLE;
    if (s.Gamepad.bLeftTrigger > 64) *b |= B_L2;
    if (s.Gamepad.bRightTrigger > 64) *b |= B_R2;
    axes[0] = (unsigned char)((s.Gamepad.sThumbRX >> 8) + 128);
    axes[1] = (unsigned char)(127 - (s.Gamepad.sThumbRY >> 8));
    axes[2] = (unsigned char)((s.Gamepad.sThumbLX >> 8) + 128);
    axes[3] = (unsigned char)(127 - (s.Gamepad.sThumbLY >> 8));
    return 1;
}

int rt_pad_connected(int port)
{
    unsigned b = 0;
    unsigned char axes[4];
    if (port == 0 || rt_pad_script_buttons(port, &b)) return 1;   /* the keyboard or a script is always there */
    return !script_on && xinput(port, &b, axes);
}

/* the 20-byte DualShock 2 reply in pressure mode: status, id, buttons (active low), 4 axes, 12 pressures */
int rt_pad_read(int port, unsigned char *buf)
{
    static const unsigned pressure_bit[12] = { B_RIGHT, B_LEFT, B_UP, B_DOWN, B_TRIANGLE, B_CIRCLE, B_CROSS,
                                               B_SQUARE, B_L1, B_R1, B_L2, B_R2 };
    unsigned b = 0;
    unsigned char axes[4] = { 0x80, 0x80, 0x80, 0x80 };
    int i;
    char bot[32];
    if (port == 0 && rt_bot_pad(bot, sizeof bot)) {
        b = buttons_of(bot);                 /* a bot duel's scripted player (runtime/botduel.c) */
    } else if (rt_pad_script_buttons(port, &b)) {
        /* --pad-script / --pad-script2 (runtime/padscript.c); a bot duel's own presses (Start for the movie) too */
        if (port == 0 && script_on) b |= script_buttons();
    } else if (script_on) {
        if (port != 0) return 0;
        b = script_buttons();
    } else {
        int pad = xinput(port, &b, axes);
        if (port == 0) {
            b |= keyboard();
            /* hold to fast-forward: Tab, or the right stick pushed right (the game never reads that axis) */
            rt_fast_forward = (gfx_window_focused() && (GetAsyncKeyState(VK_TAB) & 0x8000)) || axes[0] >= 0xE0;
            {   /* the right stick pushed LEFT (the game never reads that axis): "report this moment" (runtime/report.c) */
                static int held;
                void report_stick(void);
                if (axes[0] <= 0x20 && pad) { if (!held) report_stick(); held = 1; } else held = 0;
            }
        }
        else if (!pad) return 0;
    }
    memset(buf, 0, 20);
    buf[1] = 0x79;
    buf[2] = (unsigned char)~b;
    buf[3] = (unsigned char)~(b >> 8);
    memcpy(buf + 4, axes, 4);
    for (i = 0; i < 12; i++) buf[8 + i] = (b & pressure_bit[i]) ? 0xFF : 0;
    return 1;
}
