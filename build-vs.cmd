@echo off
setlocal
call "%~dp0tools\vs-environment.cmd"
if errorlevel 1 goto setup_error
pushd "%~dp0"
"%HFT_CMAKE%" --preset vs2026 "-DCMAKE_GENERATOR_INSTANCE=%HFT_VS_ROOT%"
if errorlevel 1 goto failed
"%HFT_CMAKE%" --build --preset vs2026-release --parallel 4 %*
if errorlevel 1 goto failed
popd
exit /b 0
:failed
popd
exit /b 1
:setup_error
echo Cannot find Visual Studio 2026 with C++ tools and bundled CMake.
exit /b 1
