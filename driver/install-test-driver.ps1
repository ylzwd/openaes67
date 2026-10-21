<#
.SYNOPSIS
    Installs the OpenAES67 test-signed audio driver package.

.DESCRIPTION
    Run this from an ELEVATED PowerShell. It performs the four steps that a
    test-signed kernel driver needs, checking each precondition first so a
    failure tells you exactly which one is missing:

      1. Secure Boot must be OFF (test signing is ignored while it is on).
      2. Test signing must be ON (bcdedit /set testsigning on, then reboot).
      3. The self-signed test certificate must be trusted (LocalMachine Root and
         TrustedPublisher).
      4. The package must be present and its signature must verify.

    Nothing is installed if a precondition fails.

.NOTES
    This changes boot security settings and installs a root certificate. Both
    are deliberate, local, and reversible with uninstall-test-driver.ps1.
#>
[CmdletBinding()]
param(
    # Skips the certificate import (use when it is already trusted).
    [switch]$SkipCertTrust
)

$ErrorActionPreference = 'Stop'

function Write-Step([string]$text) { Write-Host "== $text" -ForegroundColor Cyan }
function Write-Ok([string]$text)   { Write-Host "   OK   $text" -ForegroundColor Green }
function Write-Warn([string]$text) { Write-Host "   WARN $text" -ForegroundColor Yellow }
function Write-Bad([string]$text)  { Write-Host "   FAIL $text" -ForegroundColor Red }

# ---------------------------------------------------------------- elevation
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = New-Object Security.Principal.WindowsPrincipal($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Bad 'not elevated. Right-click PowerShell -> "Run as administrator".'
    exit 2
}

$root = Split-Path -Parent $PSScriptRoot
$outDir = Join-Path $PSScriptRoot 'out'
$audioDir = Join-Path $outDir 'audio'
$cerPath = Join-Path $outDir 'openaes67-test.cer'
$infPath = Join-Path $audioDir 'openaes67_audio.inf'
$catPath = Join-Path $audioDir 'openaes67_audio.cat'

# ------------------------------------------------------------- 1. Secure Boot
Write-Step 'precondition 1: Secure Boot'
$secureBoot = $null
try {
    $secureBoot = Confirm-SecureBootUEFI -ErrorAction Stop
} catch {
    $state = Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\SecureBoot\State' `
                             -ErrorAction SilentlyContinue
    if ($state) { $secureBoot = [bool]$state.UEFISecureBootEnabled }
}
if ($secureBoot -eq $true) {
    Write-Bad 'Secure Boot is ON. Test-signed drivers cannot load while it is on.'
    Write-Host @'
   Do this first:
     1. Reboot into UEFI/BIOS setup and disable Secure Boot.
     2. In an elevated prompt:  bcdedit /set testsigning on
     3. Reboot. Windows shows a "Test Mode" watermark when it worked.
   Then run this script again.
'@ -ForegroundColor Yellow
    exit 3
}
$secureBootText = if ($null -eq $secureBoot) { 'unknown (assumed off)' } else { 'off' }
Write-Ok ("Secure Boot: {0}" -f $secureBootText)

# ------------------------------------------------------------ 2. test signing
Write-Step 'precondition 2: test signing'
$bcd = & bcdedit /enum '{current}' 2>&1 | Out-String
if ($bcd -notmatch 'testsigning\s+Yes') {
    Write-Bad 'test signing is OFF.'
    Write-Host '   Run in an elevated prompt:  bcdedit /set testsigning on' -ForegroundColor Yellow
    Write-Host '   Then reboot before running this script again.' -ForegroundColor Yellow
    exit 4
}
Write-Ok 'test signing: on'

# ------------------------------------------------------------ 3. package files
Write-Step 'precondition 3: driver package'
foreach ($f in @($infPath, $catPath, (Join-Path $audioDir 'openaes67_audio.sys'))) {
    if (-not (Test-Path $f)) {
        Write-Bad "missing $f (run driver\build-driver.bat)"
        exit 5
    }
}
Write-Ok 'package present'

# ------------------------------------------------------------ 4. certificate
if (-not $SkipCertTrust) {
    Write-Step 'trusting the test certificate'
    if (-not (Test-Path $cerPath)) {
        Write-Bad "missing $cerPath (run driver\build-driver.bat)"
        exit 6
    }
    $cer = Get-PfxCertificate -FilePath $cerPath -ErrorAction SilentlyContinue
    if (-not $cer) { $cer = [System.Security.Cryptography.X509Certificates.X509Certificate2]::new($cerPath) }
    Write-Host ("   certificate: {0}  thumbprint {1}" -f $cer.Subject, $cer.Thumbprint)

    foreach ($store in @('Root', 'TrustedPublisher')) {
        $target = "Cert:\LocalMachine\$store"
        $existing = Get-ChildItem $target | Where-Object { $_.Thumbprint -eq $cer.Thumbprint }
        if ($existing) {
            Write-Ok "${store}: already trusted"
        } else {
            Import-Certificate -FilePath $cerPath -CertStoreLocation $target | Out-Null
            Write-Ok "${store}: imported"
        }
    }
}

# ------------------------------------------------------------ 5. verify + install
Write-Step 'verifying the catalog signature'
$signtool = Get-ChildItem 'C:\Program Files (x86)\Windows Kits\10\bin' -Recurse `
                          -Filter signtool.exe -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -match 'x64' } |
            Sort-Object FullName -Descending | Select-Object -First 1
if ($signtool) {
    & $signtool.FullName verify /pa $catPath 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) {
        Write-Bad 'signature still does not verify. The certificate may not match the package.'
        exit 7
    }
    Write-Ok 'signature verifies'
} else {
    Write-Warn 'signtool not found; skipping verification'
}

Write-Step 'installing the driver package'
$pnputil = & pnputil /add-driver $infPath /install 2>&1 | Out-String
Write-Host $pnputil.TrimEnd()
if ($LASTEXITCODE -ne 0) {
    Write-Bad "pnputil failed with exit code $LASTEXITCODE"
    exit 8
}

# ------------------------------------------------------------ 6. what appeared
Write-Step 'devices after install'
$devices = Get-PnpDevice -Class MEDIA -ErrorAction SilentlyContinue |
           Where-Object { $_.FriendlyName -match 'OpenAES67' }
if ($devices) {
    $devices | ForEach-Object { Write-Ok ("{0}  [{1}]" -f $_.FriendlyName, $_.Status) }
} else {
    Write-Warn 'no OpenAES67 device appeared. Check Event Viewer -> System for a driver load error.'
}

Write-Host @'

Next:
  openaes67 list-devices                       # the endpoint should now exist
  openaes67 recv --iface <ip> --device openaes67 ...

Known limitation of this package:
  the INF registers only KSCATEGORY_RENDER, so the device appears as a
  PLAYBACK endpoint. Recording applications (Audition) need a capture endpoint,
  which is the next driver milestone: KSCATEGORY_CAPTURE plus a capture
  miniport that drains the capture ring.
'@ -ForegroundColor Cyan
exit 0
