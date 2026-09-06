param(
    [Parameter(Mandatory = $true)][string]$Fixture,
    [Parameter(Mandatory = $true)][string]$BridgeOutput,
    [Parameter(Mandatory = $true)][string]$Log
)

$ErrorActionPreference = 'Stop'
$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Log) | Out-Null
Set-Content -LiteralPath $Log -Value ''

function Invoke-Native {
    param(
        [Parameter(Mandatory = $true)][string]$Exe,
        [Parameter(Mandatory = $true)][string[]]$ArgumentList
    )
    & $Exe @ArgumentList 2>&1 | Tee-Object -FilePath $Log -Append | Write-Host
    $ExitCode = $LASTEXITCODE
    if ($ExitCode -ne 0) { throw "$Exe failed with exit code $ExitCode" }
}

try {
    Invoke-Native -Exe 'python' -ArgumentList @(
        '-m',
        'unittest',
        'discover',
        '-s',
        "$RepoRoot/tools/ci/tests",
        '-p',
        'test_dwarfs_reader_package.py'
    )
    Invoke-Native -Exe 'python' -ArgumentList @(
        "$RepoRoot/tools/ci/prepare-dwarfs-reader.py",
        '--output',
        $BridgeOutput
    )
    if (-not (Test-Path -LiteralPath $Fixture -PathType Leaf)) {
        throw "DwarFS fixture is missing: $Fixture"
    }
    $Build = Join-Path $env:RUNNER_TEMP 'prebuilt-dwarfs-reader-smoke'
    Invoke-Native -Exe 'cmake' -ArgumentList @(
        '-S',
        "$RepoRoot/src/IpaSimulator/dwarfs-reader-smoke",
        '-B',
        $Build,
        '-G',
        'Ninja',
        '-DCMAKE_BUILD_TYPE=Release'
    )
    Invoke-Native -Exe 'cmake' -ArgumentList @(
        '--build',
        $Build,
        '--target',
        'DwarfsRuntimeRootStoreSmoke',
        '--verbose'
    )
    Invoke-Native -Exe (Join-Path $Build 'bin/DwarfsRuntimeRootStoreSmoke.exe') -ArgumentList @(
        $BridgeOutput,
        $Fixture
    )
    'Validated pinned DwarFS reader with current-source smoke; reader compilation: none.' |
        Tee-Object -FilePath $Log -Append | Write-Host
}
catch {
    ($_ | Format-List * -Force | Out-String) | Tee-Object -FilePath $Log -Append | Write-Host
    throw
}
