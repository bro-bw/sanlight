@echo off
setlocal
if not defined ONNXRUNTIME_INCLUDE set "ONNXRUNTIME_INCLUDE=%~dp0..\..\deps\onnxruntime\include"
call "%~dp0..\setup-msvc.cmd" || exit /b 1
if not exist "%~dp0..\..\build\ai-cache-tests" mkdir "%~dp0..\..\build\ai-cache-tests"
cd /d "%~dp0..\..\build\ai-cache-tests"
if not exist "SanLight_AI" mklink /J "SanLight_AI" "%~dp0..\..\build\SanLight_AI" >nul || exit /b 1
set ORTINC=..\%ONNXRUNTIME_INCLUDE%
cl /nologo /O2 /EHsc /utf-8 /D _WINDOWS /D SL_AI_BASELINE8 /I "%ORTINC%" "..\..\tools\ai_test\ai_cache_test.cpp" /Fe:ai_cache_baseline8.exe || exit /b 1
ai_cache_baseline8.exe old-reference.bin || exit /b 1
cl /nologo /O2 /EHsc /utf-8 /D _WINDOWS /I "%ORTINC%" "..\..\tools\ai_test\ai_cache_test.cpp" /Fe:ai_cache_test.exe || exit /b 1
ai_cache_test.exe old-reference.bin
exit /b %errorlevel%
