@echo off
setlocal
call "%~dp0..\setup-msvc.cmd" || exit /b 1
if not exist "%~dp0..\..\build" mkdir "%~dp0..\..\build"
cd /d "%~dp0..\..\build" || exit /b 1
cl /nologo /O2 /EHsc /utf-8 /D _WINDOWS "..\tools\ai_test\surface_test.cpp" "..\src\SL_Track.cpp" "..\src\SL_Surface.cpp" /Fe:surface_test.exe || exit /b 1
surface_test.exe
exit /b %errorlevel%
