param(
    [string]$ReleaseTag = 'development-tester'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$Repository = 'GravAlignLabs/ipasim'
$AssetName = 'ipasim-ipa-tester.zip'
$ChecksumAssetName = "$AssetName.sha256"
$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
$ReleaseBase = "https://github.com/$Repository/releases/download/$ReleaseTag"
$TempRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("ipasim-tester-update-" + [guid]::NewGuid().ToString('N'))
$ZipPath = Join-Path $TempRoot $AssetName
$ChecksumPath = Join-Path $TempRoot $ChecksumAssetName
$ExpandedPath = Join-Path $TempRoot 'expanded'

function Assert-RequiredTesterFiles {
    param([Parameter(Mandatory = $true)][string]$Root)

    $required = @(
        'Arm64Smoke.exe',
        'IpaProbe.exe',
        'IpaSimDarwinHost.dll',
        'Test-Ipa.cmd',
        'Test-Ipa.ps1',
        'Update-Tester.cmd',
        'Update-Tester.ps1',
        'BUILD.txt'
    )

    foreach ($name in $required) {
        $path = Join-Path $Root $name
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "Downloaded tester is incomplete. Missing: $name"
        }
    }

    if (-not (Get-ChildItem -LiteralPath $Root -Filter '*IpaSimLibrary.dll' -File | Select-Object -First 1)) {
        throw 'Downloaded tester is incomplete. IpaSimLibrary DLL was not found.'
    }
    if (-not (Get-ChildItem -LiteralPath $Root -Filter '*unicorn.dll' -File | Select-Object -First 1)) {
        throw 'Downloaded tester is incomplete. Unicorn DLL was not found.'
    }
}

try {
    New-Item -ItemType Directory -Path $TempRoot -Force | Out-Null
    New-Item -ItemType Directory -Path $ExpandedPath -Force | Out-Null

    Write-Host "Checking ipaSim development tester release: $ReleaseTag"
    Invoke-WebRequest -UseBasicParsing -Uri "$ReleaseBase/$AssetName" -OutFile $ZipPath
    Invoke-WebRequest -UseBasicParsing -Uri "$ReleaseBase/$ChecksumAssetName" -OutFile $ChecksumPath

    $expectedLine = (Get-Content -LiteralPath $ChecksumPath -Raw).Trim()
    $expectedHash = ($expectedLine -split '\s+')[0].Trim().ToUpperInvariant()
    if ($expectedHash -notmatch '^[0-9A-F]{64}$') {
        throw 'Release checksum file did not contain a valid SHA-256 digest.'
    }

    $actualHash = (Get-FileHash -LiteralPath $ZipPath -Algorithm SHA256).Hash.ToUpperInvariant()
    if ($actualHash -ne $expectedHash) {
        throw "Tester download checksum mismatch. Expected $expectedHash, got $actualHash."
    }

    Expand-Archive -LiteralPath $ZipPath -DestinationPath $ExpandedPath -Force
    Assert-RequiredTesterFiles -Root $ExpandedPath

    Write-Host 'Download verified. Updating tester files...'
    foreach ($item in Get-ChildItem -LiteralPath $ExpandedPath -File) {
        Copy-Item -LiteralPath $item.FullName -Destination (Join-Path $Here $item.Name) -Force
    }

    Write-Host ''
    Write-Host 'ipaSim tester updated successfully.'
    $buildFile = Join-Path $Here 'BUILD.txt'
    if (Test-Path -LiteralPath $buildFile) {
        Get-Content -LiteralPath $buildFile | ForEach-Object { Write-Host "  $_" }
    }
}
finally {
    if (Test-Path -LiteralPath $TempRoot) {
        Remove-Item -LiteralPath $TempRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}
