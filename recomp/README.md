# recomp/ — the module recompiler (lane R)

Translates the 762 duel modules (EE/R5900 MIPS, 8,620 functions, 3.03 M instructions) into C, so they run natively
in the port (PORT-PLAN.md 4.3). Module decompilation stays off: this is automatic translation.

## Input (never committed)

`recomp/input/` is a read-only copy of the decomp's split: `func/<module>/*.s` (from `asm/matchings/mod` and
`asm/nonmatchings/mod`) and `data/` (from `asm/data/mod`), plus `COMMIT`. It is derived from the game, so it stays
gitignored, like the generated C in `recomp/out/`. Make it by copying those three folders out of the decomp's
`asm/` folder; nothing is written into the decomp.

The decoder reads the instruction words from the `.s` comments; the disassembly text is only a cross-check.

## Files

| file | what |
|---|---|
| `rcgen.py` | the translator. `check` decodes everything and compares with spimdisasm; `module <m>` prints one module; `all <dir>` writes every module |
| `rc.h` | `RcCpu` (the EE registers as translated code sees them), memory accessors, divide/multiply helpers, `RCX()` |
| `rc_interp.c/.h` | small reference interpreter, written apart from the translator |
| `rctest_gen.py`, `rc_unit.c`, `rc_test.h` | R1 tests: every instruction kind and control-flow pattern, interpreter vs translation |
| `build_rc.cmd` | build lane R targets (`build_rc.cmd rc_unit`) |
| `run_r3.cmd` | build and run R1 + R3 at low priority (results in `build/rc_unit.txt`, `build/rc_diff.txt`) |
| `rc_diff.c` | R3: every module function vs the interpreter, from random states |
| `rc_port.h` | R4 interface with lane P (v1, agreed) |
| `rc_port.c` | overlay switch, entry into translated code, per-fiber state, indirect-call dispatch |
| `rcthunks.py` | R4 tables: `rcx_` thunks module->main from `gen/out/include/functions.h`, entry stubs main->module |
| `rcaudit.py` | static crash audit (build round, 18 Oct): `rcaudit.py ../gen/out input <SLUS_205.15> <iso>`; runs nothing |
| `rc_testhost.c` | `rc_unit` / `rc_diff` as a test `dotr.dll` (`build/Release/rctest/{unit,diff}`) run by the port's loader |

## How the translation works

- One C function per EE function: `void rc_<module>_<addr>(RcCpu *c)`. Registers live in `c`, the stack is PS2
  memory, so frames sit where the module code expects them.
- Branches are gotos; the delay slot runs where the EE runs it (condition computed first; branch-likely runs it only
  when taken). A branch into a delay slot gets its own copy.
- `jal` inside the module = a direct C call; outside it = `RCX(addr)`, the main-program thunk (lane P, R4).
  `jalr` and computed tail calls go through `rc_dispatch`. `jr $reg` with a jump table = a C `switch`.
- EE details: 32-bit results sign-extended to 64; mult/mult1/maddu write rd; integer divide never traps
  (`rc_div`); lq/sq ignore the low 4 address bits; paddub (Metrowerks' 128-bit move) keeps the upper half in `rh[]`;
  FPU accumulator ops with no fused multiply-add (`-ffp-contract=off`).

## Status

- R1: decoder = spimdisasm on all 3,034,386 instructions; all 8,620 functions translate; `rc_unit`: 22 known
  answers + 4,300 generated tests × 24 random states, 0 differences. A mutation check (three planted translator
  bugs) is caught by 208 tests.
- R2: all 762 modules compile with `/W3 -Werror`, 0 warnings.
- R3 (`run_r3.py`): every one of the 8,620 module functions against the interpreter from 4 random states each
  (6 processes, about a minute): 29,680 runs, 28,737 compared, 0 functions differ (runs where the interpreter hit
  its budget, or a random pointer was used as code, are skipped and counted). The bench is checked too: the first
  full runs found only bench faults (a loop with no budget tick through a jump table, NaN payload order, the
  interpreter looping on a garbage `$ra`, the arena's spare tail not reset), each fixed and rerun.
- R4: the port links all 762 translated modules + thunks (`rcthunks.py`: 3,394 exits, 1,383 entry stubs; the 3
  module calls into the middle of main-program functions run their original code on the interpreter).
  `DotR.exe --rc-check`: all 762 modules through the game's own mwOverlayInit. Bot duels run with module code live.
- Oracle (`rc_oracle.c`, rc_port.h v5): a main-program function's ORIGINAL EE code (from SLUS_205.15) on the
  interpreter vs the native port, from the same live state, at every call hook of an oracle build
  (`build_oracle.cmd`, `run_oracle.cmd <seed> [1=deep]`, log `build/Oracle/bin/rc_oracle.log`). Seed 1 full duel:
  1,663 checks agree; the differences found (missing returns, an uninitialised AI array, bit-field layout, x86
  stack overflows of address-taken locals) went to lane P.
- 4 functions end on a branch whose delay slot lies past the module's text (b293, b564, b717, c022; all past the
  module end in `config/modules.tsv`): the slot is taken as a nop and listed in the translator notes.

## The oracle tools: how to run them, how to read them

The oracle runs a function's ORIGINAL EE code on `rc_interp` and compares it with what the port does, from the
same live game state. Two kinds:

- **Main-program oracle** (needs the oracle build: `recomp\build_oracle.cmd` → `build\Oracle\bin`; every native
  function calls `rc_oracle_hook` first, generator hook `-DDOTR_ORACLE=ON`). The native function runs, memory is put
  back, the original runs on the interpreter (its calls go to the native callees; `DOTR_ORACLE_DEEP=1` interprets
  game callees too), and v0/f0 plus every changed byte of PS2 RAM, scratchpad and the live x86 stack are compared;
  then the game goes on from the real call. The first `DOTR_ORACLE_CALLS` (default 3) calls of each function are
  checked.
- **Module oracle** (normal build, `DOTR_RC_ORACLE=1`, `recomp\run_modoracle.cmd <seed>` → `build\Release\bin\
  rc_oracle.log`): at each native→module entry the translated function makes the real call while its calls into the
  main program are recorded (arguments, results, memory writes); then the module's original code runs on the
  interpreter with those calls replayed, and the results are compared.

Running:

| what | command | log |
|---|---|---|
| sweep many seeds (idle priority, 5 at a time) | `python recomp\oracle_sweep.py 1 31 5` or `... 2,8,24 - 5` (`deep` as 4th arg) | `build\Oracle\sweep\oracle-<seed>.log`; summary on stdout lists functions not in its KNOWN list first |
| one seed | `recomp\run_oracle.cmd <seed> [1]` | `build\Oracle\bin\rc_oracle.log` |
| a late stretch of a duel | `recomp\oracle_late.cmd <seed> <from frame> [calls]` (`DOTR_ORACLE_FROM`) | `build\Oracle\late\w<seed>\` |
| bisect a check that breaks the game | `recomp\oracle_probe.cmd lo-hi` (`DOTR_ORACLE_RANGE`, 40 frames) | prints OK/BAD |
| quick self-check | `DotR.exe --rc-check` (modules through mwOverlayInit + a few oracle checks, MXCSR) | console |

Each line: `same` / `DIFF` / `diff-hle` / `skip` (module oracle: `mod-same` / `MOD-DIFF` / `mod-skip`). A DIFF line
gives the first 8 differing bytes as `addr nNN/iNN` (native / interpreter), the call's arguments, the x86 stack range
and the MXCSR at the check.

What is noise (expected, nothing to fix):

- `std`, `__sinit`: libc FILE records hold native callback addresses.
- `diff-hle`: the function called something the runtime provides (heap, semaphores...), which ran twice.
- Differences only in bytes the ORIGINAL fills from its own stack leftovers: struct copies of fields never set
  (RH2_DATA_IN*, MATRIX_XYZ_ININ33 w, SzView_V2IV's return); the port starts those at zero, the interpreter shows the
  EE's garbage. Recognisable: interpreter bytes look random or like 0x80/0x3F80 patterns, native bytes are 0.
- Ta_IsExzodia / TaLogic_IsExzodia_0016CFF0 return values: garbage on both sides, callers only test == 4.
- `skip`: would block/yield, VU0/syscall code, no native thunk — not checked.

What is real (and what each class was):

- native returns garbage where the EE returns a value → a missing `return` (func_00198A88);
- the same bytes shifted by 2 or 16 bits → struct/bit-field layout (SzDuel_InitCardInfo);
- native ±inf or NaN where the interpreter gives ±FLT_MAX or a finite value → the EE float rule not applied (the
  MXCSR mask, 0x0580 vs 0x0680); check the `mxcsr` column: 9FFD means traps were masked;
- a crash or 1.0f return address inside native code → an address-taken local smaller than what a callee writes
  (fixed by the generator's 16-byte slack rule);
- native writes an x86 stack address (0x021xxxxx) into game data → a pointer to a native local escaping.

Every finding was handed to lane P; their fixes went into lane/plat on 17 Oct.

The module oracle is NOT finished: on seed 1 it ran ~250 checks (about 105 agree) and none of its differences has
been confirmed as a translator bug; known bench gaps are main-program calls that write through pointers into the
module's own EE stack (0x021xxxxx, outside the snapshot) and arguments a module leaves as garbage in registers.
Treat its MOD-DIFF lines as leads, not findings. The proof that the translation is right is R3 (≈860,000 random
states, 0 differences) plus the live duels.

Last full main-program sweep (seeds 1-31 on lane/plat bd4129f, 17 Oct): all seeds finish; only noise and the items
above remained, all with lane P.

## Build round (18 Oct): the 762 modules never crash — what was checked, from the code

`rcaudit.py` reads the module asm, the port's `gen/out`, the decomp's original main-program asm, the ELF and the
disc; it runs nothing. Results on main fa971fc + lane/recomp:

Checked, clean:

- Every jump table entry lies in the one function that switches on it (the split's tables run on into zeros; harmless).
- Every module data word and every built address (lui/addiu, lui/ori) that points into module text is a function
  start (has an entry stub); no module code reads its own text as data (so the entry jumps written over it are safe).
- The main program's effect tables (`D_00292FF0`, 1,855 entries): each of the 761 entries whose disc slot holds a
  module is a function start of exactly that module (checked by reading the slot's name from BMODULE/CMODULE/MMODULE.MRG
  through `SzDuel_Xax2New`); the other 922 slots are empty on the disc.
- Main -> module: every module function the main program can reach (effect entries, data pointers, built addresses;
  control-flow analysis, first read before write, calls clobber after their delay slot) reads integer arguments only,
  at most 8, as the entry stubs pass. (The old "b489 reads f12" note is an internal call, not an entry.)
- Module -> main: all 306 call targets against the original code's own argument reads: no target reads an argument the
  port's prototype does not pass (printf's varargs save is the only one; its 6 module calls pass `%x` or nothing).
- Indirect calls: no computed `jr` outside a jump table; `jalr` targets come only from the pointers above; 3,451 of
  3,457 main functions have a thunk, and no module reaches the other 6 (sceGifPkOpenGifTag, sceVif1PkOpenGifTag,
  sceVu0ViewScreenMatrix, func_001A2340, func_001A3008, RhGeom_GetScreenMatrix).
- No translator stubs: 0 `rc_bad_jump` sites, 0 untranslatable functions, 0 undecoded words.
- The 3 module calls into the middle of main functions (run on the interpreter): all three sit in dead module fragments
  (b069 01E01D80, b562 01E01E00, c035 01E01580: no prologue, nothing branches, falls or points there).
- Stack: deepest static module chain 0xAD0 bytes (b033) of the 64 KB per fiber; no recursion inside any module.
- Unaligned access: the EE code is aligned; lq/sq mask the low 4 bits as the EE does; x86 takes the rest.

Fixed (rcthunks.py):

- Five main functions the port declares `void` whose value the modules use (on the EE the last call's v0/f0 stays):
  GRotTransPers_3 / _4 (384 call sites read v0), LkSetAngleXY_m90, SzEffect_GetRadius (f0, 17 sites), _d_utod (u32 to
  soft double in v0; the port's own body computes nothing, only modules call it). Hand thunks with the original body.
- 45 targets reach an `ee_` twin at their PS2 address that takes 32 bits of a 64-bit parameter; the modules pass the
  full 64-bit register. They are now called by name with all 64 bits: sceGifPkAddGsAD / sceVif1PkAddGsAD (the upper
  32 bits of GS register values were cut: effect picture bugs), the 64-bit divide and soft-float helpers, the
  `*_Pipe` draw functions, SzFile_ReadRequest*, the Sz*_Set*Flag setters... (renames.txt applied).

Left for a test round (only running can show these):

- ShFx_SetUV's default case (mode >= 10) multiplies whatever t1/t2 the caller left; the port's C can't reproduce
  caller garbage. Modules pass modes below 10 at the sites read; a picture difference there would show in effects.
- An empty effect slot reached at run time stops with a clear rc_fatal line (unknown module ""); on the PS2 it would
  run zeros. The code gives no way to reach one, but only play can prove it.
- The module oracle stays unfinished (see above); R3 + rc_unit + rc-check pass on this tree.

Checks run this round (windowless, through a DotR.exe loader next to the build's dotr.dll):
`--rc-check` (762 modules through mwOverlayInit: passed), `rctest\unit` (4,300 tests, 0 failed).
