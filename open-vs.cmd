@echo off
setlocal
call "%~dp0tools\vs-environment.cmd"
if errorlevel 1 goto setup_error
set "HFT_SOLUTION=%~dp0build\vs2026\hft.slnx"
if not exist "%HFT_SOLUTION%" set "HFT_SOLUTION=%~dp0build\vs2026\hft.sln"
if not exist "%HFT_SOLUTION%" (
  echo Generate the solution first by running build-vs.cmd.
  pause
  exit /b 1
)
echo Save your work and close existing HFT Visual Studio windows before using this launcher.
start "" "%HFT_VS_ROOT%\Common7\IDE\devenv.exe" "%HFT_SOLUTION%"
exit /b 0
:setup_error
echo Cannot find Visual Studio 2026 with C++ tools and bundled CMake.
pause
exit /b 1
