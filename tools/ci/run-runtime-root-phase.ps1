param(
    [Parameter(Mandatory = $true)]
    [string]$TesterRoot,

    [Parameter(Mandatory = $true)]
    [string]$Log,

    [Parameter(Mandatory = $true)]
    [string]$Image,
    [Parameter(Mandatory = $true)]
    [string]$ExpectedImageSha256,

    [Parameter(Mandatory = $true)]
    [Int64]$ExpectedImageSize = 0,

    [Parameter(Mandatory = $true)]
    [string]$ReaderBridge
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

function Invoke-RequiredNative {
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

function Invoke-ProbeForBoundary {
    param(
        [Parameter(Mandatory = $true)][string]$Label,
        [Parameter(Mandatory = $true)][string]$Exe,
        [Parameter(Mandatory = $true)][string[]]$Arguments
    )

    Write-Log ''
    Write-Log "===== $Label ====="
    $RawLines = @(& $Exe @Arguments 2>&1)
    $ExitCode = $LASTEXITCODE
    $Lines = @()
    foreach ($RawLine in $RawLines) {
        $Line = [string]$RawLine
        $Lines += $Line
        $Line | Tee-Object -FilePath $Log -Append | Write-Host
    }

    $RelevantErrors = @($Lines | Where-Object {
        $_ -match '^Error:' -and $_ -ne 'Error: unsupported ARM64 relocation.'
    })
    $SymbolBoundary = $RelevantErrors |
        Where-Object { $_ -match '^Error: symbol .+ was not found for library ordinal -?\d+\.$' } |
        Select-Object -First 1
    $FixupBoundary = $RelevantErrors |
        Where-Object { $_ -match '^Error: cannot apply chained fixups for ' } |
        Select-Object -First 1

    if ($ExitCode -eq 0) {
        $CanonicalBoundary = '<success>'
    }
    elseif ($SymbolBoundary -and $FixupBoundary) {
        $CanonicalBoundary = "$SymbolBoundary || $FixupBoundary"
    }
    elseif ($FixupBoundary) {
        $CanonicalBoundary = $FixupBoundary
    }
    elseif ($SymbolBoundary) {
        $CanonicalBoundary = $SymbolBoundary
    }
    elseif ($RelevantErrors.Count -gt 0) {
        $CanonicalBoundary = $RelevantErrors[0]
    }
    else {
        throw "$Label exited with code $ExitCode without a diagnosable non-relocation loader/runtime boundary."
    }

    Write-Log "$Label exit code: $ExitCode"
    Write-Log "$Label canonical boundary: $CanonicalBoundary"
    return [pscustomobject]@{
        ExitCode = [int]$ExitCode
        Boundary = [string]$CanonicalBoundary
    }
}

function Publish-BoundaryOutputs {
    param(
        [Parameter(Mandatory = $true)]$Result
    )
    $BoundaryBytes = [System.Text.Encoding]::UTF8.GetBytes([string]$Result.Boundary)
    $BoundaryBase64 = [Convert]::ToBase64String($BoundaryBytes)
    if ($env:GITHUB_OUTPUT) {
        "exit_code=$($Result.ExitCode)" | Add-Content -LiteralPath $env:GITHUB_OUTPUT
        "boundary_b64=$BoundaryBase64" | Add-Content -LiteralPath $env:GITHUB_OUTPUT
        "boundary_text=$($Result.Boundary)" | Add-Content -LiteralPath $env:GITHUB_OUTPUT
    }
}

try {
    if (-not (Test-Path -LiteralPath $TesterRoot -PathType Container)) {
        throw "exact-head tester root is missing: $TesterRoot"
    }

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
    $Apps = @(Get-ChildItem -LiteralPath $Payload -Directory | Where-Object {
        $_.Name.EndsWith('.app', [System.StringComparison]::OrdinalIgnoreCase)
    })
    if ($Apps.Count -ne 1) {
        throw "expected exactly one Payload/*.app, found $($Apps.Count)"
    }

    $MachOMagics = @(
        'CFFAEDFE',
        'FEEDFACF',
        'CEFAEDFE',
        'FEEDFACE',
        'CAFEBABE',
        'BEBAFECA',
        'CAFEBABF',
        'BFBAFECA'
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

    Invoke-RequiredNative $Arm64Smoke

    foreach ($Required in @($Image, $ReaderBridge)) {
        if (-not $Required) {
            throw 'DwarFS backend requires -Image and -ReaderBridge.'
        }
        if (-not (Test-Path -LiteralPath $Required -PathType Leaf)) {
            throw "required RuntimeRoot acceptance input is missing: $Required"
        }
    }

    $ExpectedSha = $ExpectedImageSha256.Trim().ToLowerInvariant()
    if ($ExpectedSha -notmatch '^[0-9a-f]{64}$') {
        throw "expected DwarFS image SHA-256 is invalid: '$ExpectedImageSha256'"
    }
    if ($ExpectedImageSize -le 0) {
        throw "expected DwarFS image size is invalid: $ExpectedImageSize"
    }

    $ActualImageSha = (Get-FileHash -LiteralPath $Image -Algorithm SHA256).Hash.ToLowerInvariant()
    $ActualImageSize = (Get-Item -LiteralPath $Image).Length
    if ($ActualImageSha -ne $ExpectedSha) {
        throw "RuntimeRoot.dwarfs SHA-256 mismatch: expected $ExpectedSha got $ActualImageSha"
    }
    if ($ActualImageSize -ne $ExpectedImageSize) {
        throw "RuntimeRoot.dwarfs size mismatch: expected $ExpectedImageSize got $ActualImageSize"
    }
    Write-Log "RuntimeRoot.dwarfs verified: $ActualImageSha ($ActualImageSize bytes)"

    $Result = Invoke-ProbeForBoundary `
        -Label 'DwarFS RuntimeRoot probe' `
        -Exe $Probe.FullName `
        -Arguments @($AppExecutable, '--runtime-root-dwarfs', $Image, $ReaderBridge)

    Publish-BoundaryOutputs $Result
    if ($Result.ExitCode -eq 0) {
        Write-Log 'DwarFS RuntimeRoot storage acceptance passed: the Mach-O load completed.'
    }
    else {
        $LoaderBoundaryPattern = '^Error: symbol .+ was not found for library ordinal -?\d+\. \|\| Error: cannot apply chained fixups for /'
        if ($Result.Boundary -notmatch $LoaderBoundaryPattern) {
            throw "DwarFS RuntimeRoot stopped before a proven loader symbol/fixup boundary: '$($Result.Boundary)'."
        }
        Write-Log "DwarFS RuntimeRoot storage acceptance passed at a real loader symbol/fixup boundary: $($Result.Boundary)"
    }
    Write-Log 'Full DwarFS RuntimeRoot storage/loader acceptance passed on real Windows without mounting or extracting the DwarFS RuntimeRoot.'
}
catch {
    ($_ | Format-List * -Force | Out-String) | Tee-Object -FilePath $Log -Append | Write-Host
    throw
}
