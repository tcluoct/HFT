@echo off
rem Called by the launchers: changes only their process environment.
rem Some parent applications omit the standard Windows folder variables.
if not defined SystemRoot for /f "tokens=2,*" %%A in ('reg query "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion" /v SystemRoot 2^>nul') do set "SystemRoot=%%B"
if not defined SystemRoot exit /b 1
if not defined windir set "windir=%SystemRoot%"
if not defined SystemDrive set "SystemDrive=%SystemRoot:~0,2%"
if not defined ComSpec set "ComSpec=%SystemRoot%\System32\cmd.exe"
if not defined ProgramData set "ProgramData=%SystemDrive%\ProgramData"
if not defined ALLUSERSPROFILE set "ALLUSERSPROFILE=%ProgramData%"
if not defined ProgramFiles set "ProgramFiles=%SystemDrive%\Program Files"
if not defined ProgramFiles(x86) set "ProgramFiles(x86)=%SystemDrive%\Program Files (x86)"
if not defined ProgramW6432 set "ProgramW6432=%ProgramFiles%"
if not defined CommonProgramFiles set "CommonProgramFiles=%ProgramFiles%\Common Files"
if not defined CommonProgramFiles(x86) set "CommonProgramFiles(x86)=%ProgramFiles(x86)%\Common Files"
if not defined CommonProgramW6432 set "CommonProgramW6432=%ProgramW6432%\Common Files"
if not defined LOCALAPPDATA set "LOCALAPPDATA=%USERPROFILE%\AppData\Local"
if not defined APPDATA set "APPDATA=%USERPROFILE%\AppData\Roaming"
rem Do not reuse workers started with the broken inherited environment.
set "MSBUILDDISABLENODEREUSE=1"
set "HFT_VS_ROOT="
set "HFT_VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%HFT_VSWHERE%" exit /b 1
set "HFT_VS_LIST=%TEMP%\hft-vs-%RANDOM%-%RANDOM%.txt"
"%HFT_VSWHERE%" -latest -version "[18.0,19.0)" -products Microsoft.VisualStudio.Product.Community Microsoft.VisualStudio.Product.Professional Microsoft.VisualStudio.Product.Enterprise -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath > "%HFT_VS_LIST%"
for /f "usebackq delims=" %%I in ("%HFT_VS_LIST%") do set "HFT_VS_ROOT=%%I"
del "%HFT_VS_LIST%"
if not defined HFT_VS_ROOT exit /b 1
set "HFT_CMAKE=%HFT_VS_ROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if not exist "%HFT_CMAKE%" exit /b 1
exit /b 0
