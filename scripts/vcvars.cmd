@echo off
rem Locates the newest Visual Studio (or Build Tools) with the x64 C++ toolset and enters an x64
rem dev environment. Skipped by the build scripts when already in a Developer Command Prompt.
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSINSTALL="
if exist "%VSWHERE%" (
  for /f "usebackq tokens=*" %%i in (`call "%%VSWHERE%%" -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%i"
)
if not defined VSINSTALL (
  echo Visual Studio 2022+ or Build Tools with the "Desktop development with C++" workload was not found. 1>&2
  echo Install it, or run this from a "x64 Native Tools Command Prompt". 1>&2
  exit /b 1
)
call "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
if not defined VCINSTALLDIR (
  echo Failed to initialize the MSVC environment from "%VSINSTALL%". 1>&2
  exit /b 1
)
exit /b 0
