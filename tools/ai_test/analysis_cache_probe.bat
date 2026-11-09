@echo off
setlocal
call "%~dp0..\setup-msvc.cmd" || exit /b 1
set ROOT=%~dp0..\..
set OUT=%ROOT%\build\analysis-cache-probe
set ORTINC=%ROOT%\..\..\third_party\onnxruntime\build\native\include
if not exist "%OUT%" mkdir "%OUT%" || exit /b 1
if not exist "%OUT%\SanLight_AI" powershell -NoProfile -Command "$ErrorActionPreference='Stop'; $target=(Resolve-Path -LiteralPath '%ROOT%\build\SanLight_AI').Path; New-Item -ItemType Junction -Path '%OUT%\SanLight_AI' -Target $target | Out-Null" || exit /b 1
cd /d "%OUT%" || exit /b 1
cl /nologo /O2 /EHsc /utf-8 /D _WINDOWS /I "%ORTINC%" "%ROOT%\tools\ai_test\analysis_cache_probe.cpp" "%ROOT%\src\SL_AI.cpp" "%ROOT%\src\SL_Track.cpp" "%ROOT%\src\SL_Surface.cpp" /Fe:analysis_cache_probe.exe || exit /b 1
if "%~1"=="" exit /b 0
analysis_cache_probe.exe %*
exit /b %errorlevel%
