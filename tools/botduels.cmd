@echo off
rem Headless bot duels: N computer-vs-computer duels with seeds S..S+N-1, one log each.
rem usage: tools\botduels.cmd [count=10] [first seed=1] [out dir=build\botduels]
rem Each duel is its own DotR.exe run (fresh state, same seed -> same duel). Compare two builds by comparing the
rem logs: every turn line carries a hash of the whole duel state.
setlocal enabledelayedexpansion
set ROOT=%~dp0..\
set BIN=%ROOT%build\Release\bin
set N=%~1
if "%N%"=="" set N=10
set S=%~2
if "%S%"=="" set S=1
set OUT=%~3
if "%OUT%"=="" set OUT=%ROOT%build\botduels
if not exist "%OUT%" mkdir "%OUT%"
set /a LAST=S+N-1
set FAILS=0
for /l %%s in (%S%,1,%LAST%) do (
  "%BIN%\DotR.exe" --boot --headless --fast --bot-duel %%s --bot-log "%OUT%\seed-%%s.log" > "%OUT%\seed-%%s.out" 2>&1
  set RC=!errorlevel!
  if !RC! neq 0 (
    set /a FAILS+=1
    echo seed %%s: FAILED, exit !RC! ^(see %OUT%\seed-%%s.log and .out^)
  ) else (
    findstr /c:"STOP finished" "%OUT%\seed-%%s.log" >nul
    if errorlevel 1 (
      set /a FAILS+=1
      echo seed %%s: FAILED, the run ended without a duel result
    ) else (
      for /f "delims=" %%l in ('findstr /c:"STOP finished" "%OUT%\seed-%%s.log"') do echo seed %%s: %%l
    )
  )
)
echo.
echo %N% duels, %FAILS% failed. Logs: %OUT%
if %FAILS% neq 0 exit /b 1
exit /b 0
