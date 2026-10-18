@echo off
rem Compile the layout probes with clang-cl (the PC build's compiler and flags); keep each object's .data.
rem usage: layout_pc.cmd <work_dir>
setlocal enabledelayedexpansion
set VS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools
call "%VS%\VC\Auxiliary\Build\vcvars32.bat" >nul
set PATH=%VS%\VC\Tools\Llvm\bin;%PATH%
set INC=%~dp0out\include
set N=0
set F=0
for %%c in ("%~1\pc\*.c") do (
  clang-cl /nologo /c /TC /FIport.h /I"!INC!" -fwrapv -fno-strict-aliasing -fno-builtin -Wno-everything /Fo"%%~dpnc.obj" "%%c" > "%%~dpnc.log" 2>&1
  if errorlevel 1 (
    set /a F+=1
    echo PC FAIL: %%~nc
  ) else (
    llvm-objcopy -O binary --only-section=.data "%%~dpnc.obj" "%%~dpnc.bin"
    set /a N+=1
  )
)
echo clang-cl probes: !N! ok, !F! failed
