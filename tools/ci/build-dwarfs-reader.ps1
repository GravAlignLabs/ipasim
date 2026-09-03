param(
    [Parameter(Mandatory = $true)][string]$Fixture,
    [Parameter(Mandatory = $true)][string]$BridgeOutput,
    [Parameter(Mandatory = $true)][string]$Log
)

$ErrorActionPreference = 'Stop'
$Version = if ($env:DWARFS_VERSION) { $env:DWARFS_VERSION } else { '0.15.7' }
$SourceSha = if ($env:DWARFS_SOURCE_SHA256) { $env:DWARFS_SOURCE_SHA256 } else { '363c7fdbf7bad490a6b8d63186da8643c1aeb17ca54cce1193d7b0ebc57bc6bd' }
$VcpkgBaseline = if ($env:VCPKG_BASELINE) { $env:VCPKG_BASELINE } else { '62159a45e18f3a9ac0548628dcaf74fcb60c6ff9' }

New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Log) | Out-Null
Set-Content -LiteralPath $Log -Value ''

function Write-Log([string]$Message) {
    $Message | Tee-Object -FilePath $Log -Append | Write-Host
}

function Invoke-Native {
    param(
        [Parameter(Mandatory = $true)][string]$Exe,
        [Parameter(ValueFromRemainingArguments = $true)][string[]]$Arguments
    )
    & $Exe @Arguments 2>&1 | Tee-Object -FilePath $Log -Append | Write-Host
    $ExitCode = $LASTEXITCODE
    if ($ExitCode -ne 0) {
        throw "$Exe failed with exit code $ExitCode"
    }
}

try {
    if (-not (Test-Path -LiteralPath $Fixture -PathType Leaf)) {
        throw "DwarFS fixture is missing: $Fixture"
    }

    $SourceArchive = Join-Path $env:RUNNER_TEMP "dwarfs-$Version.tar.xz"
    $SourceRoot = Join-Path $env:RUNNER_TEMP 'dwarfs-source'
    $DwarfsBuild = Join-Path $env:RUNNER_TEMP 'dwarfs-build'
    $DwarfsPrefix = Join-Path $env:RUNNER_TEMP 'dwarfs-prefix'
    $VcpkgRoot = Join-Path $env:RUNNER_TEMP 'vcpkg-dwarfs'
    $VcpkgInstalled = Join-Path $env:RUNNER_TEMP 'vcpkg-install-dwarfs'
    $BridgeBuild = Join-Path $env:RUNNER_TEMP 'ipasim-dwarfs-reader-build'

    Invoke-WebRequest -Uri "https://github.com/mhx/dwarfs/releases/download/v$Version/dwarfs-$Version.tar.xz" -OutFile $SourceArchive
    $ActualSourceSha = (Get-FileHash -Algorithm SHA256 $SourceArchive).Hash.ToLowerInvariant()
    if ($ActualSourceSha -ne $SourceSha) {
        throw "DwarFS source SHA-256 mismatch: expected $SourceSha got $ActualSourceSha"
    }

    foreach ($Path in @($SourceRoot, $DwarfsBuild, $DwarfsPrefix, $VcpkgRoot, $VcpkgInstalled, $BridgeBuild)) {
        if (Test-Path -LiteralPath $Path) {
            Remove-Item -LiteralPath $Path -Recurse -Force
        }
    }
    New-Item -ItemType Directory -Force -Path $SourceRoot | Out-Null
    Invoke-Native tar -xf $SourceArchive -C $SourceRoot
    $DwarfsSource = Get-ChildItem -Path $SourceRoot -Filter CMakeLists.txt -Recurse -File |
        Where-Object { Test-Path (Join-Path $_.Directory.FullName 'vcpkg.json') } |
        Select-Object -First 1 -ExpandProperty DirectoryName
    if ([string]::IsNullOrWhiteSpace($DwarfsSource)) {
        throw 'Pinned DwarFS source archive did not contain the expected CMake/vcpkg project root.'
    }
    Write-Log "DwarFS source: $DwarfsSource"

    New-Item -ItemType Directory -Force -Path $VcpkgRoot | Out-Null
    Invoke-Native git -C $VcpkgRoot init
    Invoke-Native git -C $VcpkgRoot remote add origin https://github.com/microsoft/vcpkg.git
    # DwarFS manifest versioning reads historical port trees. Do not use a
    # shallow clone here: the earlier depth-1 experiment failed at git read-tree.
    Invoke-Native git -C $VcpkgRoot fetch origin $VcpkgBaseline
    Invoke-Native git -C $VcpkgRoot checkout --detach FETCH_HEAD
    Invoke-Native (Join-Path $VcpkgRoot 'bootstrap-vcpkg.bat') -disableMetrics

    $env:VCPKG_DEFAULT_BINARY_CACHE = Join-Path $env:RUNNER_TEMP 'vcpkg-bincache'
    New-Item -ItemType Directory -Force -Path $env:VCPKG_DEFAULT_BINARY_CACHE | Out-Null
    $Toolchain = Join-Path $VcpkgRoot 'scripts\buildsystems\vcpkg.cmake'
    $Ninja = (Get-Command ninja.exe -ErrorAction Stop).Source
    Write-Log "Ninja: $Ninja"

    $DwarfsConfigure = @(
        '-S', $DwarfsSource,
        '-B', $DwarfsBuild,
        '-G', 'Ninja',
        "-DCMAKE_MAKE_PROGRAM=$Ninja",
        '-DCMAKE_BUILD_TYPE=Release',
        "-DCMAKE_INSTALL_PREFIX=$DwarfsPrefix",
        "-DCMAKE_TOOLCHAIN_FILE=$Toolchain",
        '-DVCPKG_TARGET_TRIPLET=x64-windows-static',
        "-DVCPKG_INSTALLED_DIR=$VcpkgInstalled",
        '-DWITH_LIBDWARFS=ON',
        '-DWITH_TOOLS=OFF',
        '-DWITH_FUSE_DRIVER=OFF',
        '-DWITH_DESKTOP_INTEGRATION=OFF',
        '-DWITH_TESTS=OFF',
        '-DWITH_BENCHMARKS=OFF',
        '-DWITH_ALL_BENCHMARKS=OFF',
        '-DWITH_FUZZ=OFF',
        '-DWITH_MAN_OPTION=OFF',
        '-DWITH_UNIVERSAL_BINARY=OFF',
        '-DWITH_FUSE_EXTRACT_BINARY=OFF',
        '-DWITH_PXATTR=OFF',
        '-DWITH_EXAMPLE=OFF',
        '-DWITH_DEV_TOOLS=OFF',
        '-DENABLE_STACKTRACE=OFF'
    )
    Invoke-Native cmake @DwarfsConfigure
    Invoke-Native cmake --build $DwarfsBuild --target install

    $Config = Get-ChildItem -Path $DwarfsPrefix -Filter dwarfs-config.cmake -Recurse -File | Select-Object -First 1
    if ($null -eq $Config) {
        throw 'Pinned DwarFS source build completed without installing dwarfs-config.cmake.'
    }
    Write-Log "Installed DwarFS CMake package: $($Config.FullName)"

    $ProofConfigure = @(
        '-S', "$env:GITHUB_WORKSPACE\src\IpaSimulator\dwarfs-reader",
        '-B', $BridgeBuild,
        '-G', 'Ninja',
        "-DCMAKE_MAKE_PROGRAM=$Ninja",
        '-DCMAKE_BUILD_TYPE=Release',
        "-DCMAKE_TOOLCHAIN_FILE=$Toolchain",
        '-DVCPKG_TARGET_TRIPLET=x64-windows-static',
        "-DVCPKG_INSTALLED_DIR=$VcpkgInstalled",
        '-DVCPKG_MANIFEST_MODE=OFF',
        "-DCMAKE_PREFIX_PATH=$DwarfsPrefix"
    )
    Invoke-Native cmake @ProofConfigure
    Invoke-Native cmake --build $BridgeBuild --target DwarfsRuntimeRootStoreSmoke

    $Bridge = Join-Path $BridgeBuild 'bin\IpaSimDwarfsReader.dll'
    $Smoke = Join-Path $BridgeBuild 'bin\DwarfsRuntimeRootStoreSmoke.exe'
    foreach ($Required in @($Bridge, $Smoke)) {
        if (-not (Test-Path -LiteralPath $Required -PathType Leaf)) {
            throw "required reader-smoke output is missing: $Required"
        }
    }

    Invoke-Native $Smoke $Bridge $Fixture

    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $BridgeOutput) | Out-Null
    Copy-Item -LiteralPath $Bridge -Destination $BridgeOutput -Force
    Write-Log "Validated DwarFS reader bridge: $BridgeOutput"
}
catch {
    ($_ | Format-List * -Force | Out-String) | Tee-Object -FilePath $Log -Append | Write-Host
    throw
}
