# Building DotR-PC

You need your own USA disc of the game as a disc image (`Yu-Gi-Oh! The Duelists of the Roses (USA).iso`, the
executable `SLUS_205.15` inside has sha1 `bee9fc13d12ab2818822d905d820320fc6427f23`).

## Prerequisites

- **Windows 10/11 (x64)** with the **Visual Studio 2022 Build Tools** (workload "Desktop development with C++",
  including "C++ Clang tools for Windows" and CMake/Ninja). `build.cmd` expects them in the default folder
  (`C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools`); edit the `VS=` line if yours differ.
- **Python 3.10+** for Windows (the generator and the checks).
- **WSL** with **Ubuntu 24.04**: the decompilation builds there.
- **The decompilation** (separate repository, CC0) at the commit in `gen/DECOMP_COMMIT`, built: follow its README
  (its toolchain is set up by **decomp-kit**: `setup/toolchain.sh` fetches binutils, the Metrowerks and EE-GCC
  compilers, splat and the rest from the public sources it lists, such as decomp.me). Put your `SLUS_205.15` in its
  `assets/`, tell it where your disc image is (its README says how), then `make split`, `make split-modules`,
  `make check`.

## Steps

1. Make a read-only copy of the decompilation at the pinned commit (the port never writes into it), e.g.
   `git clone <decomp> ~/decomp/port_pin_<commit> && cd ~/decomp/port_pin_<commit> && git checkout <commit>`, copy
   your `assets/` in, build it (`make check`).
2. If your copy is not at `~/decomp/port_pin_<commit>`, set `DOTR_DECOMP` to its WSL path first
   (`set DOTR_DECOMP=/home/you/dotr-decomp`).
3. From a Windows command prompt in this folder:

       full-build.cmd "D:\path\to\Yu-Gi-Oh! The Duelists of the Roses (USA).iso"

   It snapshots the decompilation into `gen\decomp` (`gen/fetch_decomp.sh`), runs the generator (`gen/porter.py` ->
   `gen\out`) and the struct-layout check (`gen/bflayout.py`), builds (`build.cmd` -> `build\Release\bin`) and runs
   the windowless checks.
4. The module recompiler: `recomp\build_rc.cmd` (see `recomp/README.md`); `full-build.cmd` and `build.cmd` pick up
   its output when it is there.
5. Run: `build\Release\bin\DotR.exe --iso "D:\path\to\your.iso"` (console) or `DotR_GUI.exe` (window build; it finds
   an `.iso` next to it or asks for one once).

Once `gen\out` exists, `build.cmd` alone rebuilds. `build.cmd Package` builds without the developer switches (what a
player gets).

## Checks

- `DotR.exe --selftest --iso <iso>`: memory layout, the disc, the game executable's checksum, game code reachable.
- `DotR.exe --rc-check --iso <iso>`: all 762 code modules load and run translated.
- `tools\gate.cmd build\Release\bin`: everything above, the unit tests, the picture replays and three
  computer-vs-computer duels compared with `tools/gate/m1g-baseline.txt` (results and speed). See docs/DEVELOPING.md.

The picture step of the gate needs reference captures made from your own build first (they are game pictures, so
they are not in this repository): see `tools/gate/pictures.cmd`.
