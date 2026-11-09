@echo off
setlocal
call "%~dp0..\setup-msvc.cmd" || exit /b 1
if not exist "%~dp0..\..\build" mkdir "%~dp0..\..\build"
cd /d "%~dp0..\..\build" || exit /b 1
if not defined AE_SDK_ROOT set "AE_SDK_ROOT=%~dp0..\..\deps\AfterEffectsSDK"
set "SDK=%AE_SDK_ROOT%\Examples"
cl /nologo /O2 /EHsc /utf-8 /D MSWindows /D WIN32 /D _WINDOWS /I "%SDK%\Headers" /I "%SDK%\Headers\SP" "..\tools\ai_test\sequence_test.cpp" /Fe:sequence_test.exe || exit /b 1
sequence_test.exe
exit /b %errorlevel%
