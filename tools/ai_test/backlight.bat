@echo off
setlocal
if not defined ONNXRUNTIME_INCLUDE set "ONNXRUNTIME_INCLUDE=%~dp0..\..\deps\onnxruntime\include"
set "CL=%CL% /D_WINDOWS"
call "%~dp0..\setup-msvc.cmd" || exit /b 1
if not exist "%~dp0..\..\build" mkdir "%~dp0..\..\build"
cd /d "%~dp0..\..\build" || exit /b 1
if not defined AE_SDK_ROOT set "AE_SDK_ROOT=%~dp0..\..\deps\AfterEffectsSDK"
set "SDK=%AE_SDK_ROOT%\Examples"
cl /nologo /EHsc /utf-8 /D MSWindows /D WIN32 /I "%SDK%\Headers" /I "%SDK%\Headers\SP" /I "%SDK%\Util" /I "%ONNXRUNTIME_INCLUDE%" "..\tools\ai_test\backlight.cpp" "..\src\SL_AI.cpp" "..\src\SL_Track.cpp" "..\src\SL_Temporal.cpp" "%SDK%\Util\AEGP_SuiteHandler.cpp" "%SDK%\Util\MissingSuiteError.cpp" /Fe:backlight_test.exe || exit /b 1
backlight_test.exe
exit /b %errorlevel%
