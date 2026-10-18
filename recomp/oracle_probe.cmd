@echo off
rem One short oracle run (bot duel seed 1, 40 frames) checking only functions in a range: for bisecting a check that
rem leaves state behind. usage: oracle_probe.cmd lo-hi   (hex, e.g. 100000-200000); prints crash/stuck/ok
setlocal
set DOTR_ORACLE_RANGE=%1
set DOTR_ORACLE_DEEP=
set BIN=%~dp0..\build\Oracle\bin
set ISO=%~dp0..\..\ISOs\Yu-Gi-Oh! The Duelists of the Roses (USA).iso
cd /d "%BIN%"
start "" /LOW /B /WAIT "%BIN%\DotR.exe" --boot --headless --fast --frames 40 --bot-duel 1 --bot-log "%BIN%\probe.log" --iso "%ISO%" > "%BIN%\probe.out" 2>&1
findstr /c:"crashed" /c:"stuck" "%BIN%\probe.out" >nul && (echo BAD %1) || (echo OK %1)
