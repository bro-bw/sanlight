@echo off
setlocal
call "%~dp0..\setup-msvc.cmd" || exit /b 1
if not exist "%~dp0..\..\build" mkdir "%~dp0..\..\build"
cd /d "%~dp0..\..\build" || exit /b 1
cl /nologo /O2 /EHsc /utf-8 "..\tools\ai_test\track_compare.cpp" "..\tools\ai_test\track_build5.cpp" "..\src\SL_Track.cpp" /Fe:track_compare.exe || exit /b 1
track_compare.exe %*
exit /b %errorlevel%
