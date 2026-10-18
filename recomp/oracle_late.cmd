@echo off
rem Oracle on the late, passive stretch of one duel: checks start at a frame and take more calls per function.
rem usage: oracle_late.cmd <seed> <from frame> [calls=20]   (runs in build\Oracle\late\w<seed>, a copy of the oracle build)
setlocal
set SEED=%1
set DOTR_ORACLE_FROM=%2
set DOTR_ORACLE_CALLS=%3
if "%DOTR_ORACLE_CALLS%"=="" set DOTR_ORACLE_CALLS=20
set W=%~dp0..\build\Oracle\late\w%SEED%
set DOTR_ORACLE_LOG=%W%\oracle-late-%SEED%.log
set ISO=%~dp0..\..\ISOs\Yu-Gi-Oh! The Duelists of the Roses (USA).iso
cd /d "%W%"
start "" /LOW /B /WAIT "%W%\DotR.exe" --boot --headless --fast --bot-duel %SEED% --bot-log "%W%\bot-%SEED%.log" --iso "%ISO%" > "%W%\run.out" 2>&1
echo seed %SEED% exit %errorlevel%
