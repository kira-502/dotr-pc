# DotR-PC

A native Windows port of *Yu-Gi-Oh! The Duelists of the Roses* (PlayStation 2, USA release SLUS-20515), built from
the game's matching decompilation.

**Not affiliated with or endorsed by Konami.** This repository contains no game data: no pictures, sounds, movies,
text or game code. You need your own copy of the game, the USA disc, as a disc image (`.iso`). The program reads the
game from that image at run time; the build reads the game's executable and code modules from it too.

## How it works

```
the decompilation (C that compiles to the PS2 game, separate repository)
        |  gen/  : the port generator turns its C into C that compiles for 32-bit Windows
        v
  native game code  +  recomp/ : the 762 code modules (card effects, battle animations), translated from MIPS to C
        |
        v
  runtime/ + hle/ : the PS2 the game expects, answered on the PC: memory map at the console's addresses, threads,
                    timers, DMA, memory cards, pad, disc, Sony library calls
  gfx/            : DMA/VIF/VU1/GIF/GS: an exact software renderer and a Direct3D 11 one, the window
  sound/, movie/  : the IOP and its SPU2 sound driver (the game's own IOP code), the PSS movies
```

`DotR.exe` is a tiny loader that reserves the PS2 address range and loads `dotr.dll`, which holds the game and the
PC layer.

## Status

The game boots and plays on the PC: menus, the campaign, duels, battle animations, movies, sound, memory cards. It
has not been played through end to end in every part. Duels between the game's computer players are deterministic
and checked against the original PS2 code by an "oracle" that runs the original MIPS code beside the port. Expect
bugs; see CONTRIBUTING.md.

## Building

See [BUILDING.md](BUILDING.md). In short: Windows with the Visual Studio 2022 Build Tools (clang-cl), WSL with the
decompilation's toolchain, the decompilation at the commit in `gen/DECOMP_COMMIT`, and your disc image; then
`full-build.cmd your.iso`.

## Developing

[docs/DEVELOPING.md](docs/DEVELOPING.md): the developer switches (developer menu, computer-vs-computer duels,
automatic duels, pad scripts, headless runs, pictures, reports), the logs, and the checks every change should pass
(`tools/gate.cmd`).

## Layout

| folder | what |
|---|---|
| `loader/` | `DotR.exe` (console) and the window build: reserve PS2 memory, load `dotr.dll` |
| `gen/` | the port generator: decomp C (pinned commit) -> compilable PC C, its rules (`rules.json`) and checks |
| `runtime/` | memory map, ELF loading from the disc, threads, timers, virtual hardware registers, pad, memory cards, crash and hang reports, bot duels, pad scripts |
| `hle/` | Sony library calls answered on the PC (kernel, SIF/IOP, CD, pad, memory card, C library) |
| `recomp/` | the module recompiler (MIPS -> C) and its checks, including the oracle |
| `gfx/` | graphics: DMA/VIF1/VU1/GIF/GS, renderers, window, VU0 maths, F12 capture and replay |
| `sound/`, `movie/` | IOP emulator + SPU2, movie player |
| `mcimport/` | imports a PS2 memory-card save into the PC's memory-card folders |
| `tests/`, `tools/` | windowless checks, the gate, release scripts |

## Licence

CC0 1.0 (public domain dedication), like the decompilation: see [LICENSE](LICENSE). Third-party parts keep their
own licences: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). The licence covers this project's own work only; it
grants no rights in the game or in Sony's libraries.

No builds are published here. Build it yourself from your own disc.
