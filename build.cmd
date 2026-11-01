@echo off
rem Builds bin\debugmcp.exe. Usage: build.cmd [debug]
rem Works from any directory and any shell (cmd, PowerShell, Developer Prompt).
setlocal
cd /d "%~dp0"
if not defined VCINSTALLDIR call "%~dp0scripts\vcvars.cmd" || exit /b 1
if not exist bin mkdir bin
if not exist build mkdir build
set OPT=/O2 /MT /DNDEBUG
if /i "%~1"=="debug" set OPT=/Od /MTd /Zi
cl /nologo /std:c++20 /EHsc /W3 /utf-8 %OPT% /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
   /Fobuild\ /Fdbuild\debugmcp.pdb ^
   src\main.cpp src\engine.cpp src\engine_ops.cpp src\util.cpp ^
   /Fe:bin\debugmcp.exe /link /DEBUG version.lib || exit /b 1
echo built bin\debugmcp.exe
rem First build: fetch the DbgEng runtime (WinDbg engine) next to the binary.
if not exist runtime\dbgeng\dbgeng.dll (
  powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\setup-runtime.ps1" || (
    echo WARNING: DbgEng runtime setup failed; debugmcp will fall back to System32's reduced dbgeng.dll.
    echo          Fix the issue above and run: powershell -ExecutionPolicy Bypass -File scripts\setup-runtime.ps1
  )
)
exit /b 0
