# Contributing

Changes are welcome: bug fixes, missing PS2 behaviour, speed, tools, docs.

- **Keep the game out of the repository.** No pictures, captures, sounds, movies, text, disc files, saves or
  generated game code (`gen\out`, recompiler output). Everything game-derived is made at build or run time from the
  player's own disc.
- **Fix at the right layer.** Bugs in the decompiled C belong in the decompilation (separate repository) or, when the
  C matches the PS2 but needs a PC-only change, in `gen/rules.json` with a `why`. PS2 behaviour the game relies on
  belongs in `runtime/`, `hle/`, `gfx/`, `sound/`.
- **Behave like the PS2.** When unsure what the PS2 does, read the decompilation and the original code; the oracle
  (`recomp/`) compares the port with the original MIPS code.
- **Run the gate** (`tools\gate.cmd build\Release\bin`, see docs/DEVELOPING.md) and say in your change what it
  showed. A change in duel results needs its reason.
- **No GPL code** copied in (the project is CC0). Code from other projects only with a licence that allows it, noted in
  THIRD_PARTY_NOTICES.md.
- **No builds** in issues, pull requests or releases: people build from their own disc.

Send changes as pull requests with a short description of what changed and why.
