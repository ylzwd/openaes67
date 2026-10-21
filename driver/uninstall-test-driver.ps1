<#
.SYNOPSIS
    Removes the OpenAES67 test driver, its package and its test certificate.

.DESCRIPTION
    The reverse of install-test-driver.ps1. Run from an ELEVATED PowerShell.
    It removes the device node, deletes the driver package from the driver
    store, and removes the test certificate from the two machine stores it was
    imported into.

    It does NOT change boot configuration: turning test signing off is a boot
    security change, so the exact command is printed instead of executed.
#>
[CmdletBinding()]
param()

$ErrorActionPreference = 'Continue'

function Write-Step([string]$text) { Write-Host "== $text" -ForegroundColor Cyan }
function Write-Ok([string]$text)   { Write-Host "   OK   $text" -ForegroundColor Green }
function Write-Warn([string]$text) { Write-Host "   WARN $text" -ForegroundColor Yellow }

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = New-Object Security.Principal.WindowsPrincipal($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host 'FAIL not elevated. Right-click PowerShell -> "Run as administrator".' -ForegroundColor Red
    exit 2
}

# ------------------------------------------------------------------- device
Write-Step 'removing the device node'
$instances = Get-PnpDevice -ErrorAction SilentlyContinue |
             Where-Object { $_.FriendlyName -match 'OpenAES67' -or
                            $_.InstanceId -match 'OpenAes67Audio' }
if ($instances) {
    foreach ($d in $instances) {
        & pnputil /remove-device "$($d.InstanceId)" 2>&1 | Out-Null
        if ($LASTEXITCODE -eq 0) {
            Write-Ok "removed $($d.InstanceId)"
        } else {
            Write-Warn "could not remove $($d.InstanceId) (exit $LASTEXITCODE)"
        }
    }
} else {
    Write-Ok 'no OpenAES67 device present'
}

# ------------------------------------------------------------------ package
Write-Step 'deleting the driver package'
$published = & pnputil /enum-drivers 2>&1 | Out-String
# pnputil groups output per package: Published Name / Original Name / Provider / Class
$blocks = [regex]::Matches($published, '(?s)Published Name:\s*(\S+).*?Original Name:\s*(\S+)')
$removed = 0
foreach ($b in $blocks) {
    $pub = $b.Groups[1].Value
    $orig = $b.Groups[2].Value
    if ($orig -match 'openaes67') {
        & pnputil /delete-driver $pub /uninstall /force 2>&1 | Out-Null
        if ($LASTEXITCODE -eq 0) {
            Write-Ok "deleted $pub ($orig)"
            ++$removed
        } else {
            Write-Warn "could not delete $pub (exit $LASTEXITCODE)"
        }
    }
}
if ($removed -eq 0) { Write-Ok 'no openaes67 package in the driver store' }

# -------------------------------------------------------------- certificate
Write-Step 'removing the test certificate'
$cerPath = Join-Path (Join-Path $PSScriptRoot 'out') 'openaes67-test.cer'
if (Test-Path $cerPath) {
    $cer = [System.Security.Cryptography.X509Certificates.X509Certificate2]::new($cerPath)
    foreach ($store in @('Root', 'TrustedPublisher')) {
        $path = "Cert:\LocalMachine\$store"
        $match = Get-ChildItem $path -ErrorAction SilentlyContinue |
                 Where-Object { $_.Thumbprint -eq $cer.Thumbprint }
        if ($match) {
            Remove-Item $match.PSPath -Force
            Write-Ok "${store}: removed $($cer.Thumbprint)"
        } else {
            Write-Ok "${store}: not present"
        }
    }
} else {
    Write-Warn "certificate file not found at $cerPath; remove it by hand if needed"
}

Write-Host @'

Still on your side (boot security is not changed automatically):
  bcdedit /set testsigning off     # elevated, then reboot
  Re-enable Secure Boot in UEFI if you turned it off.
'@ -ForegroundColor Cyan
exit 0
