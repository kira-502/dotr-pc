@echo off
rem The oracle build of the port (every native main-program function first calls rc_oracle_hook) -> build\Oracle\bin
rem usage: build_oracle.cmd [ninja jobs=4]
setlocal
set J=%1
if "%J%"=="" set J=4
set VS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools
call "%VS%\VC\Auxiliary\Build\vcvars32.bat" >nul || exit /b 1
set PATH=%VS%\VC\Tools\Llvm\bin;%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%
set OUT=%~dp0..\build\Oracle
cmake -S "%~dp0.." -B "%OUT%" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_LINKER=lld-link -DDOTR_ORACLE=ON >nul || exit /b 1
cmake --build "%OUT%" --target DotR dotr -- -j %J% || exit /b 1
