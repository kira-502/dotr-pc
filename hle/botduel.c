/*
 * Bot duels, game side (runtime/botduel.c has the rest): once the game has loaded (logos showing), the main loop
 * starts a duel the way the Custom Duel menu does (main_041.c): each side gets a built-in deck (by default one of
 * the three Custom Duel decks) and the computer duelist that belongs to it, then SzGame_Push(1, 1).
 */
#include "hle.h"

int rt_bot_choice(int *deck0, int *deck1, int *duel0, int *duel1, int *map);

#define BOT_START_FRAME 400      /* the boot's file loads are done by then (the Konami logo is up) */

/* called by Main_LoopProc every frame before SzGame_ModeAct (gen/rules.json) */
void rt_bot_tick(void)
{
    static int started;
    int d0, d1, c0, c1, map;
    if (started || rt_vblank_count() < BOT_START_FRAME || !rt_bot_choice(&d0, &d1, &c0, &c1, &map)) return;
    started = 1;
    SzData_SetupFromConstDeck(0, d0);       /* built-in decks 0..50 (the Custom Duel decks are 48-50) ... */
    SzData_SetupFromConstDeck(1, d1);
    SzGame_SetMenuNum(2, 0);
    SzGame_SetMenuNum(1, map);
    SzGame_SetMenuNum(3, 1000 + c0);        /* ... and their computer duelists */
    SzGame_SetMenuNum(4, 1000 + c1);
    SzGame_SetMenuNum(28, 0);
    SzGame_SetMenuNum(29, 0);
    SzGame_Push(1, 1);
}
