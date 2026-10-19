@echo off
rem One command from the pinned decomp to a checked build (windowless checks only, no game window).
rem usage: full-build.cmd [disc.iso]
rem   1. snapshot the pinned decomp (WSL Ubuntu-24.04, read-only; DOTR_DECOMP = its WSL path,
rem      default ~/decomp/port_pin_32a9475b) -> gen\decomp
rem   2. port generator -> gen\out
rem   3. build (build.cmd) -> build\Release\bin
rem   4. checks: self-test, gfx_tests, gfx_vu0_tests, snd_tests, gfxboot on SLUS_205.15 from the disc,
rem      headless boot for 300 frames
setlocal
set ROOT=%~dp0
set PY=C:\Program Files\Python311\python.exe
set ISO=%~1
if "%ISO%"=="" set ISO=%ROOT%..\ISOs\Yu-Gi-Oh! The Duelists of the Roses (USA).iso
set BIN=%ROOT%build\Release\bin
rem the build translates the VU1 programs from the same disc (gfx/CMakeLists.txt)
set DOTR_ISO=%ISO%
if "%DOTR_DECOMP%"=="" set DOTR_DECOMP=~/decomp/port_pin_32a9475b

echo === 1/4 decomp snapshot
for /f "usebackq delims=" %%p in (`wsl -d Ubuntu-24.04 wslpath -a "%ROOT%."`) do set WROOT=%%p
wsl -d Ubuntu-24.04 bash "%WROOT%/gen/fetch_decomp.sh" %DOTR_DECOMP% "%WROOT%/gen/decomp" || goto fail

echo === 2/4 port generator
"%PY%" -I "%ROOT%gen\porter.py" || goto fail
rem every struct (bit-fields included) laid out as on the EE
"%PY%" -I "%ROOT%gen\bflayout.py" || goto fail

echo === 3/4 build
call "%ROOT%build.cmd" Release || goto fail

echo === 4/4 checks
"%BIN%\DotR.exe" --selftest --iso "%ISO%" || goto fail
"%BIN%\gfx_tests.exe" || goto fail
"%BIN%\gfx_vu0_tests.exe" || goto fail
"%BIN%\snd_tests.exe" || goto fail
"%PY%" -I "%ROOT%tools\iso_get.py" "%ISO%" SLUS_205.15 "%ROOT%build\SLUS_205.15" || goto fail
"%BIN%\gfxboot.exe" "%ROOT%build\SLUS_205.15" || goto fail
"%BIN%\DotR.exe" --boot --headless --fast --frames 300 --iso "%ISO%" || goto fail

echo.
echo FULL BUILD OK: %BIN%
exit /b 0
:fail
echo.
echo FULL BUILD FAILED (see the step above)
exit /b 1
