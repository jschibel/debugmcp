@echo off
rem Builds the debug test targets into tests\targets\bin (Debug, /Zi, /Od).
setlocal
cd /d "%~dp0"
if not defined VCINSTALLDIR call "%~dp0..\..\scripts\vcvars.cmd" || exit /b 1
if not exist bin mkdir bin
cl /nologo /Zi /Od /EHsc /MDd /std:c++17 /Fobin\ /Fdbin\plugin.pdb plugin.cpp /LD /Febin\plugin.dll /link /DEBUG:FULL /INCREMENTAL:NO || exit /b 1
cl /nologo /Zi /Od /EHsc /MDd /std:c++17 /Fobin\ /Fdbin\basic_vc.pdb basic.cpp /Febin\basic.exe /link /DEBUG:FULL || exit /b 1
cl /nologo /Zi /Od /EHsc /MDd /std:c++17 /Fobin\ /Fdbin\threads_vc.pdb threads.cpp /Febin\threads.exe /link /DEBUG:FULL || exit /b 1
rem Optimized build to exercise inline frames.
cl /nologo /Zi /O2 /EHsc /MD /std:c++17 /Fobin\opt_ /Fdbin\basic_opt_vc.pdb basic.cpp /Febin\basic_opt.exe /link /DEBUG:FULL || exit /b 1
echo targets built
