@echo off
rem The module oracle on real game states: one headless bot duel on the normal build with DOTR_RC_ORACLE=1; every
rem module function entered from native code (first DOTR_ORACLE_CALLS calls) is checked against its original code.
rem usage: run_modoracle.cmd [seed=1]   -> build\Release\bin\rc_oracle.log (MOD-DIFF / mod-same / mod-skip lines)
setlocal
set SEED=%1
if "%SEED%"=="" set SEED=1
set DOTR_RC_ORACLE=1
set BIN=%~dp0..\build\Release\bin
set ISO=%~dp0..\..\ISOs\Yu-Gi-Oh! The Duelists of the Roses (USA).iso
cd /d "%BIN%"
if exist rc_oracle.log del rc_oracle.log
start "" /LOW /B /WAIT "%BIN%\DotR.exe" --boot --headless --fast --bot-duel %SEED% --bot-log "%BIN%\modoracle-seed-%SEED%.log" --iso "%ISO%"
echo exit %errorlevel%
