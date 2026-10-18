@echo off
rem The oracle sweep: one headless bot duel on the oracle build (build_oracle.cmd); every native main-program function
rem the duel calls is checked (first DOTR_ORACLE_CALLS calls, default 3) against its original EE code.
rem usage: run_oracle.cmd [seed=1] [deep: 1 = interpret callees too]   -> build\Oracle\bin\rc_oracle.log
setlocal
set SEED=%1
if "%SEED%"=="" set SEED=1
if "%2"=="1" (set DOTR_ORACLE_DEEP=1) else (set DOTR_ORACLE_DEEP=)
if not "%3"=="" set DOTR_ORACLE_RANGE=%3
set BIN=%~dp0..\build\Oracle\bin
set ISO=%~dp0..\..\ISOs\Yu-Gi-Oh! The Duelists of the Roses (USA).iso
cd /d "%BIN%"
start "" /LOW /B /WAIT "%BIN%\DotR.exe" --boot --headless --fast --bot-duel %SEED% --bot-log "%BIN%\oracle-seed-%SEED%.log" --iso "%ISO%"
echo exit %errorlevel%
