@echo off
rem Use an existing x64 developer prompt, VCVARS64, or a standard VS install.
where cl.exe >nul 2>nul && exit /b 0
if defined VCVARS64 (
  if not exist "%VCVARS64%" (
    echo VCVARS64 does not exist. 1>&2
    exit /b 1
  )
  call "%VCVARS64%" >nul
  goto check
)
set "SL_VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%SL_VSWHERE%" for /f "usebackq tokens=*" %%i in (`"%SL_VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do call "%%i\VC\Auxiliary\Build\vcvars64.bat" >nul
:check
where cl.exe >nul 2>nul && exit /b 0
echo Open an x64 Native Tools Command Prompt or set VCVARS64. 1>&2
exit /b 1
