@echo off
rem Build and run lane R's windowless checks: build_rc.cmd [target ...]  (default: rc_unit)
setlocal
set VS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools
call "%VS%\VC\Auxiliary\Build\vcvars32.bat" >nul || exit /b 1
set PATH=%VS%\VC\Tools\Llvm\bin;%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%
set OUT=%~dp0..\build\Release
cmake -S "%~dp0.." -B "%OUT%" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_LINKER=lld-link >nul || exit /b 1
set T=%*
if "%T%"=="" set T=rc_unit
cmake --build "%OUT%" --target %T% || exit /b 1
