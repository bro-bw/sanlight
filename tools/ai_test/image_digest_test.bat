@echo off
setlocal
call "%~dp0..\setup-msvc.cmd" || exit /b 1
if not exist "%~dp0..\..\build" mkdir "%~dp0..\..\build"
cd /d "%~dp0..\..\build" || exit /b 1
cl /nologo /O2 /EHsc "..\tools\ai_test\image_digest_test.cpp" /Fe:image_digest_test.exe || exit /b 1
image_digest_test.exe || exit /b 1
cl /nologo /O2 /EHsc /D SL_TEST_NO_IMAGE_DIGEST "..\tools\ai_test\image_digest_test.cpp" /Fe:image_digest_failure_test.exe || exit /b 1
image_digest_failure_test.exe
exit /b %errorlevel%
