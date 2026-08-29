@echo off
setlocal

if "%~1"=="" (
  echo ipaSim generic IPA tester
  echo.
  echo Drag an .ipa file onto this .cmd file,
  echo or run:
  echo   Test-Ipa.cmd "C:\path\to\App.ipa"
  echo.
  echo To continue through iOS system dependencies, supply a runtime root whose
  echo top level contains System and usr:
  echo   Test-Ipa.cmd "C:\path\to\App.ipa" "C:\path\to\RuntimeRoot"
  echo.
  echo For public bug reports, use the repository synthetic IPA workflow instead
  echo of sharing output from a private application.
  echo.
  pause
  exit /b 64
)

if "%~2"=="" (
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Test-Ipa.ps1" "%~1"
) else (
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Test-Ipa.ps1" "%~1" "%~2"
)
set "result=%ERRORLEVEL%"

echo.
if not "%result%"=="0" (
  echo ipaSim tester stopped with exit code %result%.
) else (
  echo ipaSim tester checkpoint passed.
)

pause
exit /b %result%
