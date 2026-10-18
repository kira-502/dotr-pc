@echo off
rem Lane R checks in one go, at low priority: build rc_unit + rc_diff, run R1 (instruction tests) and R3 (every
rem module function vs the interpreter, 4 random states each). Results: build\rc_unit.txt, build\rc_diff.txt.
rem usage: run_r3.cmd [states-per-function]
setlocal
set ST=%1
if "%ST%"=="" set ST=4
set ROOT=%~dp0..
start "" /LOW /B /WAIT cmd /c "%~dp0build_rc.cmd" rc_unit rc_diff || exit /b 1
start "" /LOW /B /WAIT cmd /c ""%ROOT%\build\Release\bin\rc_unit.exe" > "%ROOT%\build\rc_unit.txt" 2>&1"
type "%ROOT%\build\rc_unit.txt" | findstr /b "rc_unit:"
start "" /LOW /B /WAIT cmd /c ""%ROOT%\build\Release\bin\rc_diff.exe" "%ROOT%\build\Release\recomp\img" %ST% > "%ROOT%\build\rc_diff.txt" 2>&1"
type "%ROOT%\build\rc_diff.txt"
