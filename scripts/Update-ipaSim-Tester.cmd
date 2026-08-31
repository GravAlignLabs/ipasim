@echo off
setlocal EnableExtensions DisableDelayedExpansion
title ipaSim Windows Tester Updater

rem Run the actual updater from a temporary copy so the installed updater can
rem safely replace itself when a new tester package is extracted.
if /I "%~1"=="--worker" goto :worker

set "WORKER=%TEMP%\ipasim-updater-%RANDOM%-%RANDOM%.cmd"
copy /Y "%~f0" "%WORKER%" >nul
if errorlevel 1 (
  echo ERROR: Could not create temporary updater copy.
  pause
  exit /b 1
)

call "%WORKER%" --worker "%~dp0"
set "RESULT=%ERRORLEVEL%"
del /Q "%WORKER%" >nul 2>&1
exit /b %RESULT%

:worker
setlocal EnableExtensions EnableDelayedExpansion
set "ROOT=%~2"
if not defined ROOT exit /b 64
if not "!ROOT:~-1!"=="\" set "ROOT=!ROOT!\"

set "BUILD_URL=https://raw.githubusercontent.com/GravAlignLabs/ipasim/master/tester/windows/latest/BUILD.txt"
set "ZIP_URL=https://raw.githubusercontent.com/GravAlignLabs/ipasim/master/tester/windows/latest/ipasim-ipa-tester.zip"
set "BUILD_TEMP=%TEMP%\ipasim-BUILD-%RANDOM%-%RANDOM%.txt"
set "ZIP_TEMP=%TEMP%\ipasim-tester-%RANDOM%-%RANDOM%.zip"
set "STAGE=%TEMP%\ipasim-stage-%RANDOM%-%RANDOM%"
set "ZIP_FINAL=!ROOT!ipasim-ipa-tester.zip"
set "BUILD_FINAL=!ROOT!BUILD.txt"

cls
echo.
echo ========================================
echo   Updating ipaSim Windows Tester
echo ========================================
echo.
echo Install folder:
echo   !ROOT!
echo.

where powershell.exe >nul 2>&1
if errorlevel 1 (
  echo ERROR: Windows PowerShell was not found.
  goto :fail
)

call :download "!BUILD_URL!" "!BUILD_TEMP!" "BUILD.txt"
if errorlevel 1 goto :fail

if not exist "!BUILD_TEMP!" (
  echo ERROR: BUILD.txt was not downloaded.
  goto :fail
)

echo.
echo Published checkpoint:
type "!BUILD_TEMP!"
echo.

set "EXPECTED_HASH="
for /f "tokens=1,* delims==" %%A in ('findstr /B /C:"zip_sha256=" "!BUILD_TEMP!"') do set "EXPECTED_HASH=%%B"
if not defined EXPECTED_HASH (
  echo ERROR: BUILD.txt does not contain zip_sha256.
  goto :fail
)

call :download "!ZIP_URL!" "!ZIP_TEMP!" "tester ZIP"
if errorlevel 1 goto :fail

if not exist "!ZIP_TEMP!" (
  echo ERROR: Tester ZIP was not downloaded.
  goto :fail
)

for %%F in ("!ZIP_TEMP!") do set "ZIP_SIZE=%%~zF"
echo.
echo Downloaded !ZIP_SIZE! bytes.
if !ZIP_SIZE! LSS 100000 (
  echo ERROR: Downloaded ZIP is unexpectedly small.
  goto :fail
)

echo.
echo [3/5] Verifying SHA256...
set "IPASIM_ZIP_TEMP=!ZIP_TEMP!"
for /f "usebackq delims=" %%H in (`powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "(Get-FileHash -LiteralPath $env:IPASIM_ZIP_TEMP -Algorithm SHA256).Hash.ToLowerInvariant()"`) do set "ACTUAL_HASH=%%H"

if not defined ACTUAL_HASH (
  echo ERROR: Could not calculate ZIP SHA256.
  goto :fail
)

echo Expected: !EXPECTED_HASH!
echo Actual:   !ACTUAL_HASH!
if /I not "!ACTUAL_HASH!"=="!EXPECTED_HASH!" (
  echo ERROR: SHA256 mismatch. Existing tester was not replaced.
  goto :fail
)

echo SHA256 verified.

echo.
echo [4/5] Extracting verified package to staging...
if exist "!STAGE!" rmdir /S /Q "!STAGE!" >nul 2>&1
mkdir "!STAGE!" >nul 2>&1
if errorlevel 1 (
  echo ERROR: Could not create staging folder.
  goto :fail
)

set "IPASIM_STAGE=!STAGE!"
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "$ErrorActionPreference='Stop'; Expand-Archive -LiteralPath $env:IPASIM_ZIP_TEMP -DestinationPath $env:IPASIM_STAGE -Force"
if errorlevel 1 (
  echo ERROR: Verified ZIP could not be extracted.
  goto :fail
)

for %%F in (
  "Arm64Smoke.exe"
  "IpaSimDarwinHost.dll"
  "IpaProbe.exe"
  "libIpaSimLibrary.dll"
  "libunicorn.dll"
  "Test-Ipa.cmd"
  "Test-Ipa.ps1"
) do (
  if not exist "!STAGE!\%%~F" (
    echo ERROR: Expected tester file is missing from the ZIP:
    echo   %%~F
    goto :fail
  )
)

echo.
echo [5/5] Installing verified tester...
xcopy "!STAGE!\*" "!ROOT!" /E /I /Y /Q >nul
if errorlevel 2 (
  echo ERROR: Could not copy the tester into the install folder.
  goto :fail
)

copy /Y "!BUILD_TEMP!" "!BUILD_FINAL!" >nul
if errorlevel 1 (
  echo ERROR: Could not update BUILD.txt.
  goto :fail
)

move /Y "!ZIP_TEMP!" "!ZIP_FINAL!" >nul
if errorlevel 1 (
  echo ERROR: Tester installed, but the verified ZIP could not be saved locally.
  goto :fail
)

rmdir /S /Q "!STAGE!" >nul 2>&1
del /Q "!BUILD_TEMP!" >nul 2>&1

set "IPASIM_ZIP_TEMP="
set "IPASIM_STAGE="

echo.
echo ========================================
echo   ipaSim tester updated successfully
echo ========================================
echo.
echo Saved ZIP:
echo   !ZIP_FINAL!
echo.
echo SHA256:
echo   !ACTUAL_HASH!
echo.
echo Double-click Test-Ipa.cmd to run the tester.
echo.
pause
exit /b 0

:download
set "DOWNLOAD_URL=%~1"
set "DOWNLOAD_PATH=%~2"
set "DOWNLOAD_NAME=%~3"
set "IPASIM_DOWNLOAD_URL=%DOWNLOAD_URL%?cache=%RANDOM%%RANDOM%%RANDOM%"
set "IPASIM_DOWNLOAD_PATH=%DOWNLOAD_PATH%"

if exist "%DOWNLOAD_PATH%" del /Q "%DOWNLOAD_PATH%" >nul 2>&1

echo [download] %DOWNLOAD_NAME%
echo   %DOWNLOAD_URL%

powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "$ErrorActionPreference='Stop'; $ProgressPreference='SilentlyContinue'; Invoke-WebRequest -UseBasicParsing -Uri $env:IPASIM_DOWNLOAD_URL -OutFile $env:IPASIM_DOWNLOAD_PATH"
if errorlevel 1 (
  echo ERROR: Could not download %DOWNLOAD_NAME%.
  exit /b 1
)

if not exist "%DOWNLOAD_PATH%" (
  echo ERROR: Download completed without creating %DOWNLOAD_NAME%.
  exit /b 1
)

for %%F in ("%DOWNLOAD_PATH%") do echo   received %%~zF bytes
exit /b 0

:fail
set "IPASIM_ZIP_TEMP=!ZIP_TEMP!"
if exist "!BUILD_TEMP!" del /Q "!BUILD_TEMP!" >nul 2>&1
if exist "!ZIP_TEMP!" del /Q "!ZIP_TEMP!" >nul 2>&1
if exist "!STAGE!" rmdir /S /Q "!STAGE!" >nul 2>&1
set "IPASIM_ZIP_TEMP="
set "IPASIM_STAGE="

echo.
echo ========================================
echo   Update FAILED
echo ========================================
echo.
echo The existing tester ZIP was not replaced unless installation had already
echo reached the final copy step. The published tester remains available at:
echo   !ZIP_URL!
echo.
pause
exit /b 1
