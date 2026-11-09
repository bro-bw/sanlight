@echo off
setlocal
call "%~dp0..\setup-msvc.cmd" || exit /b 1
if not exist "%~dp0..\..\build" mkdir "%~dp0..\..\build"
cd /d "%~dp0..\..\build" || exit /b 1
cl /nologo /O2 /EHsc /utf-8 "..\tools\ai_test\parallel_test.cpp" /Fe:parallel_test.exe || exit /b 1
parallel_test.exe
exit /b %errorlevel%
