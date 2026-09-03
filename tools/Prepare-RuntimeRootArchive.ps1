[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$RuntimeRoot,

    [string]$OutputArchive,

    [string]$Repository = 'GravAlignLabs/ipasim',

    [string]$ArchiveUrl,

    [string]$ArchiveAuthHeader,

    [switch]$Rotate
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Require-Command {
    param([Parameter(Mandatory = $true)][string]$Name)

    $command = Get-Command $Name -ErrorAction SilentlyContinue
    if (-not $command) {
        throw "Required command '$Name' was not found on PATH."
    }
    return $command.Source
}

function Resolve-SevenZip {
    $command = Get-Command '7z.exe' -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }

    $candidates = @()
    if ($env:ProgramFiles) {
        $candidates += (Join-Path $env:ProgramFiles '7-Zip\7z.exe')
    }
    if (${env:ProgramFiles(x86)}) {
        $candidates += (Join-Path ${env:ProgramFiles(x86)} '7-Zip\7z.exe')
    }

    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return $candidate
        }
    }

    throw '7-Zip was not found. Install 7-Zip before running this helper.'
}

function Set-GitHubActionsSecretFromStdin {
    param(
        [Parameter(Mandatory = $true)][string]$Gh,
        [Parameter(Mandatory = $true)][string]$Repo,
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Value
    )

    $Value | & $Gh secret set $Name --repo $Repo --app actions
    if ($LASTEXITCODE -ne 0) {
        throw "gh secret set failed for $Name with exit code $LASTEXITCODE."
    }
}

$resolvedRuntimeRoot = (Resolve-Path -LiteralPath $RuntimeRoot).Path
if (-not (Test-Path -LiteralPath $resolvedRuntimeRoot -PathType Container)) {
    throw "RuntimeRoot is not a directory: $resolvedRuntimeRoot"
}

foreach ($requiredDirectory in @('System', 'usr')) {
    $requiredPath = Join-Path $resolvedRuntimeRoot $requiredDirectory
    if (-not (Test-Path -LiteralPath $requiredPath -PathType Container)) {
        throw "RuntimeRoot is missing required directory '$requiredDirectory': $requiredPath"
    }
}

$runtimeParent = Split-Path -Parent $resolvedRuntimeRoot
$runtimeLeaf = Split-Path -Leaf $resolvedRuntimeRoot
if (-not $OutputArchive) {
    $OutputArchive = Join-Path $runtimeParent 'RuntimeRoot.7z'
}
$OutputArchive = [System.IO.Path]::GetFullPath($OutputArchive)

$sevenZip = Resolve-SevenZip
$gh = Require-Command -Name 'gh.exe'

& $gh auth status --hostname github.com
if ($LASTEXITCODE -ne 0) {
    throw 'GitHub CLI is not authenticated. Run: gh auth login'
}

# Ask gh to emit one secret name per line instead of parsing its JSON in
# PowerShell. This is stable for zero, one, or many secrets and avoids
# StrictMode property-access failures caused by CLI/PowerShell JSON shape
# differences.
$existingSecretNames = @(& $gh secret list --repo $Repository --app actions --json name --jq '.[].name')
if ($LASTEXITCODE -ne 0) {
    throw "Unable to list GitHub Actions secrets for $Repository."
}
$existingSecretNames = @(
    $existingSecretNames |
        ForEach-Object { "$($_)".Trim() } |
        Where-Object { $_ }
)

if (($existingSecretNames -contains 'IPASIM_RUNTIME_ARCHIVE_PASSWORD') -and -not $Rotate) {
    throw "IPASIM_RUNTIME_ARCHIVE_PASSWORD already exists for $Repository. Refusing to rotate it implicitly. Re-run with -Rotate only when intentionally replacing the encrypted RuntimeRoot archive and cache."
}

if ((Test-Path -LiteralPath $OutputArchive -PathType Leaf) -and -not $Rotate) {
    throw "Archive already exists: $OutputArchive. Refusing to overwrite it implicitly. Use -Rotate only when intentionally replacing the RuntimeRoot archive."
}

$passwordBytes = New-Object byte[] 32
$rng = [System.Security.Cryptography.RandomNumberGenerator]::Create()
try {
    $rng.GetBytes($passwordBytes)
}
finally {
    $rng.Dispose()
}
$password = -join ($passwordBytes | ForEach-Object { $_.ToString('x2') })

$archiveCreated = $false
try {
    if (Test-Path -LiteralPath $OutputArchive -PathType Leaf) {
        Remove-Item -LiteralPath $OutputArchive -Force
    }

    Write-Host "Creating encrypted RuntimeRoot archive: $OutputArchive"
    Write-Host 'The generated password is not printed or written to the repository.'

    Push-Location $runtimeParent
    try {
        & $sevenZip a -t7z $OutputArchive $runtimeLeaf '-mhe=on' '-mx=7' "-p$password" | Out-Host
        if ($LASTEXITCODE -ne 0) {
            throw "7-Zip failed with exit code $LASTEXITCODE."
        }
    }
    finally {
        Pop-Location
    }

    if (-not (Test-Path -LiteralPath $OutputArchive -PathType Leaf)) {
        throw "7-Zip reported success but the archive was not created: $OutputArchive"
    }
    $archiveCreated = $true

    $archiveSha = (Get-FileHash -LiteralPath $OutputArchive -Algorithm SHA256).Hash.ToLowerInvariant()
    $shaFile = "$OutputArchive.sha256"
    "$archiveSha  $([System.IO.Path]::GetFileName($OutputArchive))" | Set-Content -LiteralPath $shaFile -Encoding ascii

    Write-Host "Encrypted archive SHA-256: $archiveSha"
    Write-Host "Writing GitHub Actions secrets to $Repository through authenticated gh CLI..."

    Set-GitHubActionsSecretFromStdin -Gh $gh -Repo $Repository -Name 'IPASIM_RUNTIME_ARCHIVE_PASSWORD' -Value $password
    Set-GitHubActionsSecretFromStdin -Gh $gh -Repo $Repository -Name 'IPASIM_RUNTIME_ARCHIVE_SHA256' -Value $archiveSha

    if ($ArchiveUrl) {
        Set-GitHubActionsSecretFromStdin -Gh $gh -Repo $Repository -Name 'IPASIM_RUNTIME_ARCHIVE_URL' -Value $ArchiveUrl.Trim()
    }

    if ($ArchiveAuthHeader) {
        Set-GitHubActionsSecretFromStdin -Gh $gh -Repo $Repository -Name 'IPASIM_RUNTIME_ARCHIVE_AUTH_HEADER' -Value $ArchiveAuthHeader
    }

    Write-Host ''
    Write-Host 'RuntimeRoot bootstrap complete.'
    Write-Host "  Archive: $OutputArchive"
    Write-Host "  SHA file: $shaFile"
    Write-Host '  GitHub secret: IPASIM_RUNTIME_ARCHIVE_PASSWORD configured'
    Write-Host '  GitHub secret: IPASIM_RUNTIME_ARCHIVE_SHA256 configured'
    if ($ArchiveUrl) {
        Write-Host '  GitHub secret: IPASIM_RUNTIME_ARCHIVE_URL configured'
    }
    else {
        Write-Warning 'IPASIM_RUNTIME_ARCHIVE_URL was not set. A URL is still required on the first CI cache miss. Re-run this helper with -ArchiveUrl or set that secret separately before retrying the trusted workflow.'
    }
    if ($ArchiveAuthHeader) {
        Write-Host '  GitHub secret: IPASIM_RUNTIME_ARCHIVE_AUTH_HEADER configured'
    }

    Write-Host ''
    Write-Host 'The password is now stored in GitHub Actions and is intentionally not displayed.'
    Write-Host 'Normal CI runs will reuse it automatically; no interactive password entry is required.'
}
catch {
    if ($archiveCreated) {
        Write-Warning "Bootstrap failed after creating $OutputArchive. If the password secret was not successfully stored, re-run with -Rotate to create a fresh archive/password pair."
    }
    throw
}
finally {
    [Array]::Clear($passwordBytes, 0, $passwordBytes.Length)
    $password = $null
}
