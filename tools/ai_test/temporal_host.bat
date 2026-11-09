@echo off
setlocal
if not defined ONNXRUNTIME_INCLUDE set "ONNXRUNTIME_INCLUDE=%~dp0..\..\deps\onnxruntime\include"
call "%~dp0..\setup-msvc.cmd" || exit /b 1
if not exist "%~dp0..\..\build" mkdir "%~dp0..\..\build"
cd /d "%~dp0..\..\build" || exit /b 1
if not defined AE_SDK_ROOT set "AE_SDK_ROOT=%~dp0..\..\deps\AfterEffectsSDK"
set "SDK=%AE_SDK_ROOT%\Examples"
cl /nologo /O2 /EHsc /utf-8 /D MSWindows /D WIN32 /D _WINDOWS /I "%SDK%\Headers" /I "%SDK%\Headers\SP" /I "%SDK%\Util" /I "%ONNXRUNTIME_INCLUDE%" "..\tools\ai_test\temporal_host.cpp" "..\src\SL_AI.cpp" "..\src\SL_Track.cpp" "..\src\SL_Temporal.cpp" "..\src\SL_Transport.cpp" "..\src\SL_Surface.cpp" "%SDK%\Util\AEGP_SuiteHandler.cpp" "%SDK%\Util\MissingSuiteError.cpp" /Fe:temporal_host.exe || exit /b 1
temporal_host.exe
exit /b %errorlevel%
