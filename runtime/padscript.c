/*
 * Pad scripts (headless tests): --pad-script <file> drives port 0, --pad-script2 <file> port 1. One program per
 * port, run a step per field (vblank). Lines, '#' starts a comment:
 *
 *   <frame> <buttons>            hold <buttons> from field <frame> on (the old format; "-" = nothing)
 *   press <buttons> [n]          hold n fields (default 6), then let go 6 fields
 *   hold <buttons> <n>           hold n fields, then back to what was held before
 *   wait <n>                     n fields
 *   wait mode <m>                until the game mode (SzGame_GetMode: 1 duel, 10 title, -1 developer menu ...) is m
 *   wait dialog | wait nodialog  until a dialog/story text window is (not) on screen
 *   wait turn me                 until it is this port's side's turn in a duel (port 0 = side 0)
 *   press <buttons> until mode <m>   press again and again (6 on, 6 off) until the mode is m
 *   log <text>                   a line in DotR-PC.log
 *   end                          stop the run (exit 0)
 *
 * Every wait takes "timeout <n>" at the end (fields; default 3600 = one game minute). A timeout stops the run with
 * exit code 8 and a log line naming the script, the line and the game mode. Buttons: names joined by '+'
 * (cross, circle, square, triangle, up, down, left, right, l1, r1, l2, r2, l3, r3, start, select).
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "runtime.h"
#include "hle_rt.h"

unsigned rt_pad_buttons_of(const char *buttons);     /* runtime/pad.c */

enum { C_AT, C_PRESS, C_HOLD, C_WAIT, C_MODE, C_DIALOG, C_NODIALOG, C_TURN, C_PRESS_UNTIL, C_LOG, C_END };

typedef struct {
    int op, line;
    unsigned buttons, n, timeout;
    int mode;
    char text[96];
} cmd_t;

typedef struct {
    cmd_t *cmd;
    int n, cap, pc, phase, on;
    unsigned held, out, t0;
    char name[64];
} prog_t;

static prog_t g_prog[2];
static unsigned g_dialog_frame = ~0u;

/* rules.json: the dialog and story text window draws call this */
void rt_dialog_drawn(void) { g_dialog_frame = rt_vblanks(); }

/* open: a duel / campaign-map dialog slot in use (SzDuel_DrawDialog_Pipe draws while one of the 4 slots at duel state
 * +0xCB2 is not 0x18), or a story event window drawn in the last 2 fields (Taku_Camp_Event_Window, rules.json) */
static int dialog_open(void)
{
    volatile uint8_t *slot = (volatile uint8_t *)(uintptr_t)(0x00317490u + 0xCB2);
    int i;
    for (i = 0; i < 4; i++) if (slot[i] != 0x18) return 1;
    return g_dialog_frame != ~0u && rt_vblanks() - g_dialog_frame <= 2;
}

#ifndef DOTR_NO_GAME
extern long long SzGame_GetMode(int which);
static int game_mode(void) { return (int)SzGame_GetMode(0); }
#else
static int game_mode(void) { return 0; }
#endif

static int my_turn(int port)
{
    volatile uint8_t *owner = (volatile uint8_t *)(uintptr_t)(0x00317490u + 0xD54);
    return game_mode() == 1 && *owner == (uint8_t)port;
}

static cmd_t *add(prog_t *p, int op, int line)
{
    cmd_t *c;
    if (p->n == p->cap) {
        p->cap = p->cap ? p->cap * 2 : 64;
        p->cmd = realloc(p->cmd, (size_t)p->cap * sizeof *p->cmd);
        if (!p->cmd) return NULL;
    }
    c = &p->cmd[p->n++];
    memset(c, 0, sizeof *c);
    c->op = op;
    c->line = line;
    c->n = 6;
    c->timeout = 3600;
    return c;
}

static void stop_timeout(prog_t *p, const cmd_t *c)
{
    char m[200];
    snprintf(m, sizeof m, "pad script %s line %d timed out at frame %u (game mode %d)", p->name, c->line, rt_vblanks(),
             game_mode());
    hw_log(0, "%s", m);
    con_printf("DotR-PC: %s\n", m);
    log_close();
    rt_exit(8);
}

/* one field of a program: runs commands until one has to wait */
static void step(prog_t *p)
{
    unsigned now = rt_vblanks();
    int guard;
    p->out = p->held;
    for (guard = 0; guard < 256 && p->pc < p->n; guard++) {
        cmd_t *c = &p->cmd[p->pc];
        if (p->phase == 0) { p->t0 = now; p->phase = 1; }
        switch (c->op) {
        case C_AT:
            if (now < c->n) return;
            p->held = p->out = c->buttons;
            break;
        case C_PRESS:
            if (now - p->t0 < c->n) { p->out = c->buttons; return; }
            if (now - p->t0 < c->n + 6) { p->out = p->held & ~c->buttons; return; }
            break;
        case C_HOLD:
            if (now - p->t0 < c->n) { p->out = c->buttons; return; }
            break;
        case C_WAIT:
            if (now - p->t0 < c->n) return;
            break;
        case C_MODE: case C_DIALOG: case C_NODIALOG: case C_TURN: case C_PRESS_UNTIL: {
            int done = c->op == C_MODE || c->op == C_PRESS_UNTIL ? game_mode() == c->mode
                     : c->op == C_DIALOG ? dialog_open() : c->op == C_NODIALOG ? !dialog_open()
                     : my_turn((int)(p - g_prog));
            if (!done) {
                if (now - p->t0 >= c->timeout) stop_timeout(p, c);
                if (c->op == C_PRESS_UNTIL && (now - p->t0) % 12 < 6) p->out = p->held | c->buttons;
                return;
            }
            break;
        }
        case C_LOG:
            hw_log(2, "pad script %s: %s (frame %u, mode %d)", p->name, c->text, now, game_mode());
            break;
        case C_END:
            hw_log(2, "pad script %s: end (frame %u, mode %d)", p->name, now, game_mode());
            rt_stop_ok("pad script end");
            return;
        }
        p->pc++;
        p->phase = 0;
        p->out = p->held;
    }
}

static void pad_vblank(void)
{
    if (g_prog[0].on) step(&g_prog[0]);
    if (g_prog[1].on) step(&g_prog[1]);
}

static int parse_timeout(const char *s, unsigned *t)
{
    const char *k = strstr(s, "timeout");
    if (k) *t = (unsigned)strtoul(k + 7, NULL, 10);
    return 1;
}

/* load a script for a port: 0 = could not read it, -line = a line it does not understand */
int rt_pad_script_load_port(const wchar_t *path, int port)
{
    prog_t *p = &g_prog[port & 1];
    FILE *f = _wfopen(path, L"r");
    char line[256];
    int ln = 0;
    static int hooked;
    const wchar_t *base = wcsrchr(path, L'\\');
    if (!f) return 0;
    snprintf(p->name, sizeof p->name, "%ls", base ? base + 1 : path);
    while (fgets(line, sizeof line, f)) {
        char a[64] = "", b[200] = "", c[64] = "", d[64] = "";
        unsigned fr;
        cmd_t *x;
        char *nl = strchr(line, '\n');
        ln++;
        if (nl) *nl = 0;
        if ((nl = strchr(line, '\r')) != NULL) *nl = 0;
        if (line[0] == '#' || sscanf(line, "%63s", a) != 1) continue;
        sscanf(line, "%63s %199s %63s %63s", a, b, c, d);
        if (sscanf(line, "%u %199s", &fr, b) == 2) {
            if (!(x = add(p, C_AT, ln))) break;
            x->n = fr;
            x->buttons = rt_pad_buttons_of(b);
        } else if (!_stricmp(a, "press") && !_stricmp(c, "until") && !_stricmp(d, "mode")) {
            if (!(x = add(p, C_PRESS_UNTIL, ln))) break;
            x->buttons = rt_pad_buttons_of(b);
            sscanf(line, "%*s %*s %*s %*s %d", &x->mode);
            parse_timeout(line, &x->timeout);
        } else if (!_stricmp(a, "press") || !_stricmp(a, "hold")) {
            if (!(x = add(p, !_stricmp(a, "press") ? C_PRESS : C_HOLD, ln))) break;
            x->buttons = rt_pad_buttons_of(b);
            if (c[0]) x->n = (unsigned)strtoul(c, NULL, 10);
        } else if (!_stricmp(a, "wait") && !_stricmp(b, "mode")) {
            if (!(x = add(p, C_MODE, ln))) break;
            x->mode = atoi(c);
            parse_timeout(line, &x->timeout);
        } else if (!_stricmp(a, "wait") && (!_stricmp(b, "dialog") || !_stricmp(b, "nodialog"))) {
            if (!(x = add(p, !_stricmp(b, "dialog") ? C_DIALOG : C_NODIALOG, ln))) break;
            parse_timeout(line, &x->timeout);
        } else if (!_stricmp(a, "wait") && !_stricmp(b, "turn") && !_stricmp(c, "me")) {
            if (!(x = add(p, C_TURN, ln))) break;
            parse_timeout(line, &x->timeout);
        } else if (!_stricmp(a, "wait") && b[0] >= '0' && b[0] <= '9') {
            if (!(x = add(p, C_WAIT, ln))) break;
            x->n = (unsigned)strtoul(b, NULL, 10);
        } else if (!_stricmp(a, "log")) {
            if (!(x = add(p, C_LOG, ln))) break;
            snprintf(x->text, sizeof x->text, "%s", line + 4);
        } else if (!_stricmp(a, "end")) {
            if (!add(p, C_END, ln)) break;
        } else {
            fclose(f);
            return -ln;
        }
    }
    fclose(f);
    p->on = 1;
    if (!hooked) { hooked = 1; rt_on_vblank(pad_vblank); }
    return 1;
}

/* rt_pad_read: the buttons a script holds on that port now (0 = no script there) */
int rt_pad_script_buttons(int port, unsigned *b)
{
    if (port < 0 || port > 1 || !g_prog[port].on) return 0;
    *b = g_prog[port].out;
    return 1;
}
