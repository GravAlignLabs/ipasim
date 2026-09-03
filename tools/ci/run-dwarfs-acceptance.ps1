param(
    [Parameter(Mandatory = $true)][string]$Image,
    [Parameter(Mandatory = $true)][string]$ExpectedImageSha256,
    [Parameter(Mandatory = $true)][Int64]$ExpectedImageSize,
    [Parameter(Mandatory = $true)][string]$TesterRoot,
    [Parameter(Mandatory = $true)][string]$ReaderBridge,
    [Parameter(Mandatory = $true)][string]$Log
)

$ErrorActionPreference = 'Stop'
$AwsCommit = if ($env:AWS_IPA_COMMIT) { $env:AWS_IPA_COMMIT } else { '58e48234db510bd4fbf643643e8808c5d6a13845' }
$AwsBlob = if ($env:AWS_IPA_BLOB) { $env:AWS_IPA_BLOB } else { '06a33a39286ffd7c9d300c5924750b6f97c4e346' }
$AwsSizeText = if ($env:AWS_IPA_SIZE) { $env:AWS_IPA_SIZE } else { '2138167' }
$AwsSize = [Int64]$AwsSizeText

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
    foreach ($Required in @($Image, $ReaderBridge)) {
        if (-not (Test-Path -LiteralPath $Required -PathType Leaf)) {
            throw "required DwarFS acceptance input is missing: $Required"
        }
    }
    if (-not (Test-Path -LiteralPath $TesterRoot -PathType Container)) {
        throw "exact-head tester root is missing: $TesterRoot"
    }

    $ActualImageSha = (Get-FileHash -LiteralPath $Image -Algorithm SHA256).Hash.ToLowerInvariant()
    $ExpectedSha = $ExpectedImageSha256.Trim().ToLowerInvariant()
    $ActualImageSize = (Get-Item -LiteralPath $Image).Length
    if ($ActualImageSha -ne $ExpectedSha) {
        throw "RuntimeRoot.dwarfs SHA-256 mismatch: expected $ExpectedSha got $ActualImageSha"
    }
    if ($ActualImageSize -ne $ExpectedImageSize) {
        throw "RuntimeRoot.dwarfs size mismatch: expected $ExpectedImageSize got $ActualImageSize"
    }
    Write-Log "RuntimeRoot.dwarfs verified: $ActualImageSha ($ActualImageSize bytes)"

    $Probe = Get-ChildItem -Path $TesterRoot -Filter IpaProbe.exe -Recurse -File | Select-Object -First 1
    if ($null -eq $Probe) {
        throw 'exact-head tester does not contain IpaProbe.exe'
    }
    $Arm64Smoke = Join-Path $Probe.Directory.FullName 'Arm64Smoke.exe'
    if (-not (Test-Path -LiteralPath $Arm64Smoke -PathType Leaf)) {
        throw "exact-head tester does not contain Arm64Smoke.exe beside IpaProbe.exe: $($Probe.Directory.FullName)"
    }
    Write-Log "Exact-head probe: $($Probe.FullName)"

    $Ipa = Join-Path $env:RUNNER_TEMP 'AWSDeviceFarmSample.ipa'
    $Url = "https://raw.githubusercontent.com/aws-samples/aws-device-farm-sample-app-for-ios/$AwsCommit/prebuilt/prebuiltSampleApp.ipa"
    Invoke-WebRequest -Uri $Url -OutFile $Ipa
    $ActualIpaSize = (Get-Item -LiteralPath $Ipa).Length
    if ($ActualIpaSize -ne $AwsSize) {
        throw "AWS sample IPA size mismatch: expected $AwsSize got $ActualIpaSize"
    }
    $ActualBlob = (& git hash-object -- $Ipa).Trim().ToLowerInvariant()
    if ($LASTEXITCODE -ne 0) {
        throw 'git hash-object failed for the AWS sample IPA'
    }
    if ($ActualBlob -ne $AwsBlob) {
        throw "AWS sample IPA blob mismatch: expected $AwsBlob got $ActualBlob"
    }
    Write-Log "Pinned AWS IPA verified at upstream blob $ActualBlob."

    $Extract = Join-Path $env:RUNNER_TEMP 'aws-ipa-extracted'
    if (Test-Path -LiteralPath $Extract) {
        Remove-Item -LiteralPath $Extract -Recurse -Force
    }
    New-Item -ItemType Directory -Force -Path $Extract | Out-Null
    $Zip = Join-Path $env:RUNNER_TEMP 'AWSDeviceFarmSample.zip'
    Copy-Item -LiteralPath $Ipa -Destination $Zip -Force
    Expand-Archive -LiteralPath $Zip -DestinationPath $Extract -Force

    $Payload = Join-Path $Extract 'Payload'
    $Apps = @(Get-ChildItem -LiteralPath $Payload -Directory | Where-Object { $_.Name.EndsWith('.app', [System.StringComparison]::OrdinalIgnoreCase) })
    if ($Apps.Count -ne 1) {
        throw "expected exactly one Payload/*.app, found $($Apps.Count)"
    }

    # Locate the app executable by Mach-O magic rather than by a Windows path
    # convention or hardcoded bundle name. Only top-level app-bundle files are
    # candidates; nested frameworks are intentionally excluded.
    $MachOMagics = @(
        'CFFAEDFE', # MH_MAGIC_64 little-endian bytes
        'FEEDFACF', # MH_CIGAM_64 bytes
        'CEFAEDFE', # MH_MAGIC little-endian bytes
        'FEEDFACE', # MH_CIGAM bytes
        'CAFEBABE', # FAT_MAGIC bytes
        'BEBAFECA', # FAT_CIGAM bytes
        'CAFEBABF', # FAT_MAGIC_64 bytes
        'BFBAFECA'  # FAT_CIGAM_64 bytes
    )
    $MachOCandidates = @()
    foreach ($File in Get-ChildItem -LiteralPath $Apps[0].FullName -File) {
        $Stream = [System.IO.File]::OpenRead($File.FullName)
        try {
            $Bytes = New-Object byte[] 4
            $Read = $Stream.Read($Bytes, 0, 4)
        }
        finally {
            $Stream.Dispose()
        }
        if ($Read -ne 4) {
            continue
        }
        $Hex = ($Bytes | ForEach-Object { $_.ToString('X2') }) -join ''
        if ($Hex -in $MachOMagics) {
            $MachOCandidates += $File
        }
    }
    if ($MachOCandidates.Count -ne 1) {
        $Names = ($MachOCandidates | ForEach-Object { $_.Name }) -join ', '
        throw "expected exactly one top-level Mach-O app executable, found $($MachOCandidates.Count): $Names"
    }
    $AppExecutable = $MachOCandidates[0].FullName
    Write-Log "Pinned IPA executable: $AppExecutable"

    Invoke-Native $Arm64Smoke
    Invoke-Native $Probe.FullName $AppExecutable '--runtime-root-dwarfs' $Image $ReaderBridge
    Write-Log 'Full DwarFS RuntimeRoot probe completed successfully on real Windows.'
}
catch {
    ($_ | Format-List * -Force | Out-String) | Tee-Object -FilePath $Log -Append | Write-Host
    throw
}
