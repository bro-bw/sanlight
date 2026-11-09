@echo off
setlocal
call "%~dp0..\setup-msvc.cmd" || exit /b 1
if not exist "%~dp0..\..\build" mkdir "%~dp0..\..\build"
cd /d "%~dp0..\..\build" || exit /b 1
cl /nologo /O2 /EHsc "..\tools\ai_test\motion_job_test.cpp" /Fe:motion_job_test.exe || exit /b 1
motion_job_test.exe
exit /b %errorlevel%
