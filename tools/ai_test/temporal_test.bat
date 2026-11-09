@echo off
setlocal
call "%~dp0..\setup-msvc.cmd" || exit /b 1
if not exist "%~dp0..\..\build" mkdir "%~dp0..\..\build"
cd /d "%~dp0..\..\build" || exit /b 1
cl /nologo /O2 /EHsc /W3 "..\tools\ai_test\temporal_test.cpp" "..\src\SL_Track.cpp" "..\src\SL_Temporal.cpp" /Fe:temporal_test.exe || exit /b 1
temporal_test.exe
exit /b %errorlevel%
