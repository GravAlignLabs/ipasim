param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string]$IpaPath,

    [Parameter(Mandatory = $false, Position = 1)]
    [string]$RuntimeRoot
)

$ErrorActionPreference = 'Stop'

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$probe = Join-Path $scriptDir 'IpaProbe.exe'
$smoke = Join-Path $scriptDir 'Arm64Smoke.exe'

if (-not (Test-Path -LiteralPath $IpaPath -PathType Leaf)) {
    throw "IPA not found: $IpaPath"
}
if (-not (Test-Path -LiteralPath $probe -PathType Leaf)) {
    throw 'IpaProbe.exe is missing from the tester package.'
}
if (-not (Test-Path -LiteralPath $smoke -PathType Leaf)) {
    throw 'Arm64Smoke.exe is missing from the tester package.'
}

if ($RuntimeRoot) {
    if (-not (Test-Path -LiteralPath $RuntimeRoot -PathType Container)) {
        throw "iOS runtime root not found: $RuntimeRoot"
    }
    $RuntimeRoot = (Resolve-Path -LiteralPath $RuntimeRoot).Path
}

function Test-MachOFile {
    param([Parameter(Mandatory = $true)][string]$Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return $false
    }

    $stream = [System.IO.File]::OpenRead($Path)
    try {
        if ($stream.Length -lt 4) {
            return $false
        }
        $bytes = New-Object byte[] 4
        [void]$stream.Read($bytes, 0, 4)
        $hex = ($bytes | ForEach-Object { $_.ToString('X2') }) -join ''
        return $hex -in @(
            'CFFAEDFE', # MH_MAGIC_64 little-endian
            'CEFAEDFE', # MH_MAGIC little-endian
            'FEEDFACF', # MH_CIGAM_64
            'FEEDFACE', # MH_CIGAM
            'CAFEBABE', # FAT_MAGIC
            'BEBAFECA', # FAT_CIGAM
            'CAFEBABF', # FAT_MAGIC_64
            'BFBAFECA'  # FAT_CIGAM_64
        )
    }
    finally {
        $stream.Dispose()
    }
}

Write-Host '[1/3] Verifying AArch64 execution through Unicorn...'
& $smoke
$smokeResult = $LASTEXITCODE
if ($smokeResult -ne 0) {
    Write-Error "AArch64 execution failed with exit code $smokeResult. IPA loader test will not be attempted."
    exit $smokeResult
}

$work = Join-Path $env:TEMP ('ipasim-ipa-' + [guid]::NewGuid().ToString('N'))
$zip = Join-Path $work 'input.zip'
$expanded = Join-Path $work 'expanded'

try {
    New-Item -ItemType Directory -Path $expanded -Force | Out-Null
    Copy-Item -LiteralPath $IpaPath -Destination $zip

    Write-Host '[2/3] Extracting IPA...'
    Expand-Archive -LiteralPath $zip -DestinationPath $expanded -Force

    $payload = Join-Path $expanded 'Payload'
    if (-not (Test-Path -LiteralPath $payload -PathType Container)) {
        throw 'IPA does not contain a Payload directory.'
    }

    $apps = @(Get-ChildItem -LiteralPath $payload -Directory | Where-Object { $_.Name -like '*.app' })
    if ($apps.Count -ne 1) {
        throw "Expected exactly one top-level .app bundle in Payload; found $($apps.Count)."
    }
    $appRoot = $apps[0].FullName

    $candidates = @(Get-ChildItem -LiteralPath $appRoot -File | Where-Object { Test-MachOFile $_.FullName })
    if ($candidates.Count -eq 0) {
        throw 'No top-level Mach-O executable was found in the .app bundle.'
    }

    # Prefer the conventional executable whose filename matches the .app bundle
    # basename, then fall back to the only top-level Mach-O if unambiguous.
    $bundleBase = [System.IO.Path]::GetFileNameWithoutExtension($apps[0].Name)
    $binary = $candidates | Where-Object { $_.Name -eq $bundleBase } | Select-Object -First 1
    if (-not $binary) {
        if ($candidates.Count -ne 1) {
            throw "Multiple top-level Mach-O files were found and the app executable could not be identified unambiguously."
        }
        $binary = $candidates[0]
    }

    Write-Host '[3/3] Exercising ipaSim DynamicLoader against the IPA app image...'
    if ($RuntimeRoot) {
        Write-Host '      iOS runtime root supplied.'
        & $probe $binary.FullName $RuntimeRoot
    } else {
        Write-Host '      No iOS runtime root supplied; the loader will report the first required system image.'
        & $probe $binary.FullName
    }
    $probeResult = $LASTEXITCODE

    Write-Host ''
    Write-Host 'Checkpoint results:'
    Write-Host '  AArch64 execution: PASS'
    Write-Host "  IPA loader: exit $probeResult"

    if ($probeResult -eq 0) {
        Write-Host 'IPA passed the current loader checkpoint.'
        Write-Host 'Application launch is intentionally not claimed by this loader-only tester.'
        exit 0
    }

    Write-Host 'IPA reached the real ipaSim loader and stopped before application launch.'
    Write-Host 'For public issue reports, reproduce the same boundary with the repository synthetic IPA workflow rather than posting private IPA output.'
    exit $probeResult
}
finally {
    if (Test-Path -LiteralPath $work) {
        Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
    }
}
