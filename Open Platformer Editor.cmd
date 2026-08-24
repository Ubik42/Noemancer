@echo off
setlocal
pushd "%~dp0" || exit /b 1
set "verifyArg="
if /I "%NOEMANCER_LAUNCH_VERIFY_ONLY%"=="1" set "verifyArg=-VerifyOnly"
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoLogo -NoProfile -ExecutionPolicy Bypass -File ".\scripts\launch-platformer.ps1" -Mode Editor %verifyArg%
if errorlevel 1 (
  echo.
  echo Failed to open the Noemancer Platformer project.
  pause
  popd
  exit /b 1
)
popd
exit /b 0
