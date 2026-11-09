@echo off
setlocal
if not defined AE_SDK_ROOT set "AE_SDK_ROOT=%~dp0deps\AfterEffectsSDK"
set "SDK=%AE_SDK_ROOT%"
set SRC=%~dp0src
set OUT=%~dp0build
set "DML_FLAGS="
if /I "%~1"=="gpu" (
 set "OUT=%~dp0build\gpu"
 set "DML_FLAGS=/D SANLIGHT_EXPERIMENTAL_DML"
)
if not defined ONNXRUNTIME_INCLUDE set "ONNXRUNTIME_INCLUDE=%~dp0deps\onnxruntime\include"
set "ORTINC=%ONNXRUNTIME_INCLUDE%"
if not exist "%OUT%" mkdir "%OUT%"
cd /d "%OUT%" || exit /b 1
call "%~dp0tools\setup-msvc.cmd" || exit /b 1
if not exist "%SDK%\Examples\Headers\AE_Effect.h" (
  echo Set AE_SDK_ROOT to your After Effects SDK directory. 1>&2
  exit /b 1
)
if not exist "%ORTINC%\onnxruntime_cxx_api.h" (
  echo Set ONNXRUNTIME_INCLUDE to your ONNX Runtime header directory. 1>&2
  exit /b 1
)
if not defined OPENCV_FLAGS_FILE set "OPENCV_FLAGS_FILE=%~dp0deps\native-motion-deps\flags.cmd"
if not exist "%OPENCV_FLAGS_FILE%" (
  echo Set OPENCV_FLAGS_FILE to a command file defining CVINC and CVLIBS. 1>&2
  exit /b 1
)
call "%OPENCV_FLAGS_FILE%" || exit /b 1
if not defined CVINC exit /b 1
if not defined CVLIBS exit /b 1
cl /nologo /utf-8 /I "%SDK%\Examples\Headers" /EP "%SRC%\SanLightPiPL.r" > SanLightPiPL.rr || exit /b 1
"%SDK%\Examples\Resources\PiPLtool.exe" SanLightPiPL.rr SanLightPiPL.rrc || exit /b 1
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\Normalize-PiPL.ps1" SanLightPiPL.rrc || exit /b 1
cl /nologo /D MSWindows /EP SanLightPiPL.rrc > SanLightPiPL.rc || exit /b 1
rc /nologo /fo SanLightPiPL.res SanLightPiPL.rc || exit /b 1
cl /nologo /LD /O2 /EHsc /MT /std:c++17 /GR- /W3 /source-charset:utf-8 /execution-charset:.936 ^
 /D MSWindows /D WIN32 /D _WINDOWS /D SANLIGHT_DIS_MOTION %DML_FLAGS% %CVINC% ^
 /I "%SDK%\Examples\Headers" /I "%SDK%\Examples\Headers\SP" /I "%SDK%\Examples\Util" /I "%ORTINC%" ^
 "%SRC%\SanLight.cpp" "%SRC%\SL_AI.cpp" "%SRC%\SL_Track.cpp" "%SRC%\SL_Temporal.cpp" "%SRC%\SL_Transport.cpp" "%SRC%\SL_Surface.cpp" "%SRC%\SL_DenseMotion.cpp" ^
 "%SDK%\Examples\Util\AEGP_SuiteHandler.cpp" "%SDK%\Examples\Util\MissingSuiteError.cpp" SanLightPiPL.res ^
 /link %CVLIBS% /OUT:SanLight_v1_3.aex /DLL /MACHINE:X64 || exit /b 1
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\Check-PiPL.ps1" "%OUT%\SanLight_v1_3.aex" || exit /b 1
echo Built SanLight_v1_3.aex
