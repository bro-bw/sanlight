@echo off
setlocal
call "%~dp0..\setup-msvc.cmd" || exit /b 1
if not exist "%~dp0..\..\build\surface-memory-tests" mkdir "%~dp0..\..\build\surface-memory-tests"
cd /d "%~dp0..\..\build\surface-memory-tests"
cl /nologo /O2 /EHsc /utf-8 /D _WINDOWS "..\..\tools\ai_test\surface_memory_compare.cpp" "..\..\tools\ai_test\track_build8_memory.cpp" "..\..\tools\ai_test\surface_build8_memory.cpp" "..\..\src\SL_Track.cpp" "..\..\src\SL_Surface.cpp" /Fe:surface_memory_compare.exe || exit /b 1
surface_memory_compare.exe || exit /b 1
cl /nologo /O2 /EHsc /utf-8 /D _WINDOWS "..\..\tools\ai_test\surface_test.cpp" "..\..\src\SL_Track.cpp" "..\..\src\SL_Surface.cpp" /Fe:surface_test.exe || exit /b 1
surface_test.exe
exit /b %errorlevel%
