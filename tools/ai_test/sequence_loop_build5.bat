@echo off
setlocal
call "%~dp0..\setup-msvc.cmd" || exit /b 1
if not exist "%~dp0..\..\build" mkdir "%~dp0..\..\build"
cd /d "%~dp0..\..\build" || exit /b 1
cl /nologo /O2 /EHsc /utf-8 /DSL_BASELINE_BUILD5 "..\tools\ai_test\sequence_loop.cpp" "..\..\..\..\versions\v1.3-build5\src\SL_Track.cpp" "..\..\..\..\versions\v1.3-build5\src\SL_Surface.cpp" /Fe:sequence_loop_build5.exe || exit /b 1
sequence_loop_build5.exe %*
exit /b %errorlevel%
