# Third-party notices

This repository's own files are CC0 1.0 (see LICENSE). It contains no third-party source code.

**What a build contains.** A build compiles the C of the game's decompilation (a separate CC0 repository). Some of
those C functions (Sony SDK library functions and C library functions) were adapted there from other public
decompilation projects under the MIT licence, and some C library functions come from newlib (BSD-style licences).
Their licence texts are in the decompilation's `THIRD_PARTY_NOTICES.md` and must accompany builds that contain them.

**Run-time and build-time tools** fetched or used by the build, under their own licences: the Visual Studio Build
Tools / clang-cl / LLVM (lld), CMake, Ninja, Python, and in WSL the decompilation's toolchain (binutils, splat, and
the PS2 compilers that decomp-kit downloads from decomp.me). The program uses Windows system libraries (Direct3D 11,
XAudio2, Media Foundation, XInput).

These licences cover the projects' own work. They grant no rights in the game, in Sony's SDK libraries, or in any
Konami property.
