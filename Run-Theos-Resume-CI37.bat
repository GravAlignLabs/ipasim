@echo off
setlocal
title ipaSim Resume Theos Preflight from CI Run 37

set "REPO=%~dp0"
set "RUN_ID=33534466454"
set "RUN_NUMBER=37"
set "CI_DIR=%REPO%out\local-theos-preflight\ci-run-%RUN_ID%"

where wsl.exe >nul 2>&1
if errorlevel 1 (
    echo WSL is not available on this computer.
    echo Install WSL with Ubuntu, then run this file again.
    echo.
    pause
    exit /b 2
)

where gh.exe >nul 2>&1
if errorlevel 1 (
    echo GitHub CLI is required once to download the 32 successful CI shard artifacts.
    echo Install it from an Administrator Command Prompt with:
    echo.
    echo   winget install --id GitHub.cli
    echo.
    echo Then run: gh auth login
    echo and double-click this file again.
    echo.
    pause
    exit /b 2
)

gh.exe auth status >nul 2>&1
if errorlevel 1 (
    echo GitHub CLI is not authenticated.
    echo Run this once and then retry:
    echo.
    echo   gh auth login
    echo.
    pause
    exit /b 2
)

echo ========================================
echo   ipaSim Local Theos Resume
echo ========================================
echo.
echo Source: GitHub Theos SDK Preflight run #%RUN_NUMBER%
echo Run ID: %RUN_ID%
echo Header stage: 32/32 successful CI manifests
echo Local work: merge, AAPCS64, Win64, bridge, adapters, planner, semantic routes
echo.

if not exist "%CI_DIR%" (
    echo [1/2] Downloading CI run #%RUN_NUMBER% header artifacts once...
    mkdir "%CI_DIR%" >nul 2>&1
    gh.exe run download %RUN_ID% -R GravAlignLabs/ipasim -p "theos-sdk-header-shard-*" -D "%CI_DIR%"
    if errorlevel 1 (
        echo.
        echo Failed to download CI artifacts.
        pause
        exit /b 1
    )
) else (
    echo [1/2] Reusing downloaded CI run #%RUN_NUMBER% artifacts.
)

echo [2/2] Starting local downstream pipeline in WSL...
echo.
wsl.exe bash -lc "cd \"$(wslpath -a '%REPO%')\" && ./tools/compat_surface/run_theos_preflight.sh prepare && exec out/local-theos-preflight/venv/bin/python tools/compat_surface/run_theos_from_ci.py --run-id %RUN_ID% --run-number %RUN_NUMBER%"
set "RC=%ERRORLEVEL%"

echo.
if "%RC%"=="0" (
    echo Local downstream Theos preflight completed successfully.
) else (
    echo Local downstream Theos preflight stopped with exit code %RC%.
    echo Diagnostic logs are preserved under out\local-theos-preflight\output.
)
echo.
pause
exit /b %RC%
