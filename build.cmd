@echo off
rem Build DotR-PC (32-bit, clang-cl, Ninja) with the VS 2022 Build Tools already on this PC.
rem usage: build.cmd [Release|Debug|Package]   -> build\<config>\bin
rem Package = Release without the developer switches (--debug-menu), for tools\make-release.ps1
setlocal
set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=Release
set BUILD_TYPE=%CONFIG%
set DEV=ON
if /i "%CONFIG%"=="Package" (set BUILD_TYPE=Release& set DEV=OFF)
set VS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools
call "%VS%\VC\Auxiliary\Build\vcvars32.bat" >nul || exit /b 1
set PATH=%VS%\VC\Tools\Llvm\bin;%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%
set SRC=%~dp0
set OUT=%~dp0build\%CONFIG%
cmake -S "%SRC%." -B "%OUT%" -G Ninja -DCMAKE_BUILD_TYPE=%BUILD_TYPE% -DDOTR_DEV=%DEV% -DCMAKE_C_COMPILER=clang-cl -DCMAKE_LINKER=lld-link || exit /b 1
cmake --build "%OUT%" || exit /b 1
