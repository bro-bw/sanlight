@echo off
setlocal
call "%~dp0..\setup-msvc.cmd" || exit /b 1
if not exist "%~dp0..\..\build\prefix-tests" mkdir "%~dp0..\..\build\prefix-tests"
cd /d "%~dp0..\..\build\prefix-tests" || exit /b 1
cl /nologo /EHsc /O2 /utf-8 "..\..\tools\ai_test\prefix_memo_test.cpp" /Fe:prefix_memo_test.exe || exit /b 1
prefix_memo_test.exe
exit /b %errorlevel%
