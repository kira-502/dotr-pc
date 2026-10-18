@echo off
rem The merge gate: tools\gate.cmd <build folder>   (see tools\gate.ps1). Exit 0 OK, 2 duels differ from M1g, 1 failed.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0gate.ps1" %*
exit /b %errorlevel%
