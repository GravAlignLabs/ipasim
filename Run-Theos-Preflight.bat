@echo off
setlocal
title ipaSim Local Theos SDK Preflight

where wsl.exe >nul 2>&1
if errorlevel 1 (
    echo WSL is not available on this computer.
    echo Install WSL with Ubuntu, then run this file again.
    echo.
    pause
    exit /b 2
)

set "REPO=%~dp0"
echo ========================================
echo   ipaSim Local Theos SDK Preflight
echo ========================================
echo.
echo This runs the repository's pinned SDK preflight inside WSL.
echo Successful header shards are cached and reused on later runs.
echo.

wsl.exe bash -lc "cd \"$(wslpath -a '%REPO%')\" && exec ./tools/compat_surface/run_theos_preflight.sh"
set "RC=%ERRORLEVEL%"

echo.
if "%RC%"=="0" (
    echo Local Theos SDK preflight completed successfully.
) else (
    echo Local Theos SDK preflight stopped with exit code %RC%.
    echo The cache and diagnostic logs were preserved under out\local-theos-preflight.
)
echo.
pause
exit /b %RC%
