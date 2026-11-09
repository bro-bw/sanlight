@echo off
setlocal
call "%~dp0..\setup-msvc.cmd" || exit /b 1
if not exist "%~dp0..\..\build" mkdir "%~dp0..\..\build"
cd /d "%~dp0..\..\build" || exit /b 1
if not defined AE_SDK_ROOT set "AE_SDK_ROOT=%~dp0..\..\deps\AfterEffectsSDK"
set "SDK=%AE_SDK_ROOT%\Examples"
set FLAGS=
set NAME=queue_host
if "%~1"=="build7" set FLAGS=/D SL_TEST_BUILD7
if "%~1"=="build7" set NAME=queue_host_build7
cl /nologo /O2 /EHsc /utf-8 /D MSWindows /D WIN32 /D _WINDOWS %FLAGS% /I "%SDK%\Headers" /I "%SDK%\Headers\SP" /I "%SDK%\Util" "..\tools\ai_test\queue_host.cpp" "%SDK%\Util\AEGP_SuiteHandler.cpp" "%SDK%\Util\MissingSuiteError.cpp" /Fe:%NAME%.exe || exit /b 1
%NAME%.exe %NAME%-output.bin || exit /b 1
if "%~1"=="build7" exit /b 0
set "SANLIGHT_CACHE_DIR=%CD%\seek-%RANDOM%-%RANDOM%"
if exist "%SANLIGHT_CACHE_DIR%" exit /b 1
for %%s in (cold warm unrelated mixed) do (
    %NAME%.exe --seek-%%s %NAME%-output.bin || exit /b 1
)
set "SANLIGHT_CACHE_DIR=%CD%\late-%RANDOM%-%RANDOM%"
if exist "%SANLIGHT_CACHE_DIR%" exit /b 1
%NAME%.exe --seek-late %NAME%-output.bin
exit /b %errorlevel%
