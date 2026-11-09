@echo off
setlocal
call "%~dp0..\setup-msvc.cmd" || exit /b 1
if not exist "%~dp0..\..\build" mkdir "%~dp0..\..\build"
cd /d "%~dp0..\..\build" || exit /b 1
cl /nologo /O2 /EHsc /utf-8 "..\tools\ai_test\video_decode.cpp" /Fe:video_decode.exe /link mfplat.lib mfreadwrite.lib mfuuid.lib ole32.lib propsys.lib || exit /b 1
video_decode.exe %1 %2
exit /b %errorlevel%
