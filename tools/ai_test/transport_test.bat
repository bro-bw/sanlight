@echo off
setlocal
if not defined ONNXRUNTIME_INCLUDE set "ONNXRUNTIME_INCLUDE=%~dp0..\..\deps\onnxruntime\include"
call "%~dp0..\setup-msvc.cmd" || exit /b 1
if not exist "%~dp0..\..\build" mkdir "%~dp0..\..\build"
cd /d "%~dp0..\..\build" || exit /b 1
cl /nologo /O2 /EHsc /utf-8 /D _WINDOWS /I "%ONNXRUNTIME_INCLUDE%" "..\tools\ai_test\transport_test.cpp" "..\src\SL_AI.cpp" "..\src\SL_Track.cpp" "..\src\SL_Transport.cpp" /Fe:transport_test.exe || exit /b 1
transport_test.exe
exit /b %errorlevel%
