@echo off
setlocal
set "HERE=%~dp0"

echo ========================================
echo   ipaSim Development Tester Updater
echo ========================================
echo.

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%HERE%Update-Tester.ps1"
set "EXITCODE=%ERRORLEVEL%"

echo.
if "%EXITCODE%"=="0" (
    echo Tester update completed successfully.
) else (
    echo Tester update failed with exit code %EXITCODE%.
    echo The existing tester files were left in place unless a verified package
    echo had already begun replacing them.
)

pause
exit /b %EXITCODE%
