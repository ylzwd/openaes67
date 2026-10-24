# Signs the Windows binaries with Authenticode.
#
# What this is for: removing the "Windows protected your PC" prompt for users. That
# prompt is driven by SmartScreen reputation, which only accrues to a certificate
# chained to a publicly trusted CA -- a self-signed certificate changes nothing for
# anyone who has not installed it. So this script does two separate jobs:
#
#   -Sign     sign with whatever certificate you point it at (test or real)
#   -VerifyOnly  report what the files currently carry, and fail if anything expected
#                is unsigned
#
# A release pipeline should always end with -VerifyOnly on the packaged files: the
# failure mode of a signing step that silently did nothing is a release that looks
# signed in the notes and warns on every user's machine.
param(
    [Parameter(Mandatory = $true)][string[]] $Path,
    # Certificate selection: either a thumbprint in Cert:\CurrentUser\My (or LocalMachine),
    # or a PFX plus the name of an environment variable holding its password
    # (a password on the command line ends up in the process list and in logs).
    [string] $Thumbprint,
    [string] $PfxPath,
    [string] $PfxPasswordEnv = 'OPENAES67_PFX_PASSWORD',
    [string] $TimestampUrl = 'http://timestamp.digicert.com',
    [switch] $VerifyOnly,
    # For pipeline tests with a self-signed certificate: the signature is present but
    # the chain is not trusted, which is a failure for a release and expected in a test.
    [switch] $AllowUntrusted,
    [switch] $AllowUnsigned
)

$ErrorActionPreference = 'Stop'

function Resolve-SignTool {
    $roots = @(
        "${env:ProgramFiles(x86)}\Windows Kits\10\bin",
        "$env:ProgramFiles\Windows Kits\10\bin"
    ) | Where-Object { $_ -and (Test-Path $_) }
    $found = foreach ($root in $roots) {
        Get-ChildItem $root -Recurse -Filter signtool.exe -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -match 'x64' } |
            Sort-Object FullName -Descending
    }
    if (-not $found) {
        throw "signtool.exe not found. Install the Windows SDK (Windows Kits\10\bin\x64)."
    }
    return $found[0].FullName
}

# `-File script.ps1 -Path a.exe,b.exe` hands the comma through as one string, so split
# it here rather than making callers wrap each path in its own -Path.
if ($Path.Count -eq 1 -and $Path[0] -match '[,;]') {
    $Path = $Path[0] -split '[,;]' | Where-Object { $_.Trim() } | ForEach-Object { $_.Trim() }
}

$signtool = Resolve-SignTool
$files = foreach ($p in $Path) {
    $item = Get-Item -LiteralPath $p -ErrorAction SilentlyContinue
    if (-not $item) { throw "no such file: $p" }
    $item
}
if (-not $files) { throw 'nothing to sign' }

Write-Host "signtool: $signtool"

if (-not $VerifyOnly) {
    if ($PfxPath) {
        if (-not (Test-Path $PfxPath)) { throw "no such PFX: $PfxPath" }
        $password = [Environment]::GetEnvironmentVariable($PfxPasswordEnv)
        if (-not $password) {
            throw "environment variable $PfxPasswordEnv is empty; the PFX password is not accepted on the command line"
        }
        Write-Host "certificate: $PfxPath (password from `$$PfxPasswordEnv)"
        foreach ($file in $files) {
            Write-Host "signing $($file.Name)"
            & $signtool sign /fd SHA256 /f $PfxPath /p $password `
                /tr $TimestampUrl /td SHA256 $file.FullName | Out-Host
            if ($LASTEXITCODE -ne 0) { throw "signtool sign failed for $($file.Name)" }
        }
    } elseif ($Thumbprint) {
        $clean = $Thumbprint -replace '[^0-9A-Fa-f]', ''
        $cert = Get-ChildItem Cert:\CurrentUser\My, Cert:\LocalMachine\My -ErrorAction SilentlyContinue |
            Where-Object { $_.Thumbprint -eq $clean } | Select-Object -First 1
        if (-not $cert) { throw "no certificate with thumbprint $clean in CurrentUser or LocalMachine" }
        Write-Host ("certificate: {0} (expires {1:yyyy-MM-dd})" -f $cert.Subject, $cert.NotAfter)
        foreach ($file in $files) {
            Write-Host "signing $($file.Name)"
            & $signtool sign /fd SHA256 /sha1 $clean /tr $TimestampUrl /td SHA256 $file.FullName | Out-Host
            if ($LASTEXITCODE -ne 0) {
                throw "signtool sign failed for $($file.Name) (is the certificate in a store signtool can read?)"
            }
        }
    } else {
        throw 'give either -Thumbprint or -PfxPath (or use -VerifyOnly)'
    }
}

# Verification is the point of the whole exercise: ask the OS, not signtool's exit code
# alone, and report the signer so a mis-signed release is visible in the log.
$failures = 0
foreach ($file in $files) {
    $sig = Get-AuthenticodeSignature -FilePath $file.FullName
    $subject = if ($sig.SignerCertificate) { $sig.SignerCertificate.Subject } else { '(none)' }
    Write-Host ("{0,-34} {1,-18} {2}" -f $file.Name, $sig.Status, $subject)
    switch ($sig.Status) {
        'Valid' { }
        'NotSigned' {
            if (-not $AllowUnsigned) { $failures++ }
        }
        'UnknownError' {
            # The usual cause is a certificate that is not in a trusted root store:
            # the signature is there and intact, but nobody can verify who made it.
            Write-Host ("  {0}: signature present, chain not trusted (self-signed?)" -f $file.Name)
            if (-not $AllowUntrusted) { $failures++ }
        }
        default { $failures++ }
    }
}

if ($failures -gt 0) {
    Write-Host "FAILED: $failures file(s) unsigned or not validly signed" -ForegroundColor Red
    exit 1
}
Write-Host 'signature check passed'
