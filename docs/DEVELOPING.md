# Developing DotR-PC

## Runs without a window

    DotR.exe --boot --headless [--fast] [--frames N] [--shots K] [--wav out.wav] [--iso disc.iso] [switches below]

- `--headless`: no window; without `--shots` nothing is drawn (the GS state is kept, fast).
- `--fast`: no real-time pacing: fields (vblanks) come as fast as the game runs. Deterministic: the same run gives
  the same result on any PC (whole fields only; every local starts at zero in the generated code).
- `--frames N`: stop cleanly after N fields (exit 0).
- `--shots K`: draw, and save the displayed picture every K fields as `shots\frame_NNNNN.bmp` next to the exe.
- `--wav file`: record the sound.
- `--selftest`, `--rc-check`: the windowless checks (see BUILDING.md).
- `DotR-PC.ini` next to the exe: `renderer=gpu|exact`, `gpu_stepping=direct|exact`, `iso=<path>`, `saves=<folder>`.

Exit codes: 0 fine, 1 a start-up problem, 3 crash, 4 hang (headless), 5-7 bot-duel stops (below), 8 a pad-script
timeout.

## Developer switches (developer builds; `build.cmd Package` leaves them out)

- `--debug-menu`: after the memory-card check the game opens its own developer menu ("PREVIEW": card duel, 3D battle,
  deck building, 3D view, trade, Custom Duel, title, campaign, slot machine, BGM/SE tests).
- `--bot-duel <seed>`: a computer-vs-computer duel straight from boot, deterministic per seed, with a log
  (`--bot-log file`): one line per turn (life points, a hash of the duel state), the result, and the game thread's CPU
  time per field. `--bot-decks a,b` (the three Custom Duel decks 0-2), `--bot-const a,b` (any built-in deck 0-50),
  `--bot-duelists x,y`, `--bot-map n`, `--bot-com id`, `--bot-pad0` (seat 0 played by pad 0), `--bot-trace`.
  Exit 5: no new turn for 20 game minutes; 6: an impossible state; 7: the duel never started.
- `--auto-duel [id]`: in every duel (campaign, Custom Duel, ...) each human seat is played by the game's own AI (the
  opponent's duelist, or AI `id`).
- `--pad-script file` (port 0), `--pad-script2 file` (port 1): scripted controllers. Lines (`#` = comment):

      <frame> <buttons>              hold buttons from that field on ("-" = none; names joined by '+')
      press <buttons> [n]            hold n fields (default 6), then let go 6
      hold <buttons> <n>             hold n fields
      wait <n>                       n fields
      wait mode <m>                  until the game mode is m (1 duel, 10 title, 11 campaign map, -1 developer menu)
      wait dialog | wait nodialog    until a dialog or story text window is (not) shown
      wait turn me                   until it is this port's side's turn in a duel
      press <buttons> until mode <m> press repeatedly until the mode is m
      log <text>                     a line in DotR-PC.log
      end                            stop the run (exit 0)

  Any wait takes `timeout <n>` (fields, default 3600); a timeout ends the run with exit 8 and a line naming the
  script, line and game mode. Buttons: cross circle square triangle up down left right l1 r1 l2 r2 l3 r3 start select.

Environment variables for digging (all `DOTR_*`; grep `getenv` for the full list), for example:
`DOTR_BOT_DUMP=1|2` (duel-state dump at the end / every turn), `DOTR_BOT_WATCH=<addr>` or `*<ptr>+<off>` (hardware
write watch on PS2 memory, logs each writer), `DOTR_REPORT_AT=<field>` / `DOTR_CRASH_AT=<field>` (test report /
crash), `DOTR_GAME_PROFILE`, `DOTR_GS_CENSUS`, `DOTR_TRACE_GFX`, `DOTR_SND_TRACE`, `DOTR_IOP_TRACE`, `DOTR_FP_TRACE`.

## Logs and reports

- `DotR-PC.log` next to the exe (the current session) and `Logs\DotR-PC-<time>.log` (the last 20 sessions).
- `Reports\<time>-<kind>\` (the last 20): made by a crash, a hang, F12 in the window or the pad's right stick pushed
  left. `REPORT.txt` starts with a plain sentence (what the game was doing, names read from the player's disc), then
  the game state, the call path for crashes and hangs, the last 60 seconds of events; plus `picture.png` and a GS
  capture (`gs-capture.gcap` / `gs-state.gcap`), which `gfxreplay` replays.

## The gate (run it before you send a change)

    tools\gate.cmd build\Release\bin

On a scratch copy: self-test, rc-check, the unit tests, the picture replays (`tools/gate/pictures.cmd`; they need
references made from your own disc), and three bot duels (seeds in `tools/gate/m1g-baseline.txt`) compared with the
baseline: winner, turns, life points, frames, state hash, and the game thread's CPU time per field (fails above
1.5x). Exit 0 OK, 2 duel results differ (allowed only with the reason, and where play changed, the oracle agreeing),
1 failed. The baseline was recorded on the maintainers' PC; record your own once if your timings differ.

## The oracle

`recomp/` can run the game's original MIPS code beside the native port function by function (`cmake -DDOTR_ORACLE=ON`
builds, `recomp/oracle_sweep.py`): every difference is a bug or known noise. See `recomp/README.md`.

## The generator

`gen/porter.py` reads the decompilation snapshot (`gen\decomp`) and writes `gen\out`. Its fixes to the decompiled C
live in `gen/rules.json` (each with its reason); EE struct layouts are checked against the PS2 compiler's by
`gen/bflayout.py`. Never edit `gen\out` by hand.
