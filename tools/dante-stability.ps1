<#
.SYNOPSIS
    Diagnoses and fixes the measured causes of Dante Controller instability on
    this machine.

.DESCRIPTION
    Default (no -Apply) is read-only: it reports what it finds and changes nothing.

    With -Apply it performs only the changes whose cause was measured, and every
    change is reversible (each step prints its own undo command).

    Measured causes on this machine (2026-10-18):

      1. Google Pinyin IME crash.  Every DanteController crash in the event log
         names the same faulting module:
             GooglePinyin2.ime, version 2.7.25.128
             Exception code 0xc000041d (fatal exception in a user-mode callback)
         The IME is registered system-wide at
             HKLM\SYSTEM\CurrentControlSet\Control\Keyboard Layouts\E0200804
         even though it is NOT in the user's input-method list (Microsoft Pinyin
         is). Renaming the .ime stops it being injected into any process.

      2. No firewall rules for Dante traffic.  Discovery is mDNS (UDP 5353) plus
         Audinate's own UDP ports; without rules an inbound-blocking profile
         silently swallows device announcements.

      3. USB gigabit NIC with "WOL Link Power Saving" enabled.  The Dante
         interface on this machine is an ASIX USB adapter; link power saving can
         drop the link mid-stream.

.PARAMETER Apply
    Perform the changes. Without it the script only reports.

.PARAMETER SkipIme
    Do not touch the IME even with -Apply.

.PARAMETER SkipFirewall
    Do not add firewall rules even with -Apply.

.PARAMETER SkipNic
    Do not touch NIC power settings even with -Apply.
#>
[CmdletBinding()]
param(
    [switch]$Apply,
    [switch]$SkipIme,
    [switch]$SkipFirewall,
    [switch]$SkipNic
)

$ErrorActionPreference = 'Continue'

function Write-Section($text) {
    Write-Host ""
    Write-Host "=== $text ===" -ForegroundColor Cyan
}

function Test-Admin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    (New-Object Security.Principal.WindowsPrincipal($id)).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)
}

$isAdmin = Test-Admin
Write-Host "OpenAES67 / Dante stability check"
Write-Host ("  admin: {0}   mode: {1}" -f $isAdmin, $(if ($Apply) { 'APPLY' } else { 'report only' }))

# ---------------------------------------------------------------- 1. IME crash
Write-Section "1. Dante Controller crashes and the faulting module"

$since = (Get-Date).AddDays(-14)
$crashes = Get-WinEvent -FilterHashtable @{
    LogName = 'Application'; ProviderName = 'Application Error'; StartTime = $since
} -ErrorAction SilentlyContinue | Where-Object { $_.Message -match 'DanteController' }

if (-not $crashes) {
    Write-Host "  no DanteController crash events in the last 14 days" -ForegroundColor Green
} else {
    Write-Host ("  {0} crash event(s) in 14 days" -f $crashes.Count)
    $modules = @{}
    foreach ($c in $crashes) {
        if ($c.Message -match 'Faulting module name:\s*([^,\r\n]+)') {
            $m = $Matches[1].Trim()
            if ($modules.ContainsKey($m)) { $modules[$m]++ } else { $modules[$m] = 1 }
        }
    }
    foreach ($k in $modules.Keys) {
        $colour = if ($k -match '\.ime$') { 'Yellow' } else { 'Gray' }
        Write-Host ("    {0,-28} {1} time(s)" -f $k, $modules[$k]) -ForegroundColor $colour
    }
    $crashes | Select-Object -First 3 | ForEach-Object {
        $code = if ($_.Message -match 'Exception code:\s*(\S+)') { $Matches[1] } else { '?' }
        Write-Host ("    e.g. {0}  exception {1}" -f $_.TimeCreated.ToString('MM-dd HH:mm'), $code)
    }
}

$imeKey = 'HKLM:\SYSTEM\CurrentControlSet\Control\Keyboard Layouts\E0200804'
$imeFile = $null
if (Test-Path $imeKey) {
    $imeFile = (Get-ItemProperty $imeKey -ErrorAction SilentlyContinue).'IME File'
    Write-Host ("  registered IME: {0} -> {1}" -f 'E0200804', $imeFile) -ForegroundColor Yellow
} else {
    Write-Host "  Google Pinyin IME key not present (already removed)" -ForegroundColor Green
}

$imePath = if ($imeFile) { Join-Path $env:WINDIR ("System32\" + $imeFile) } else { $null }
$imePresent = $imePath -and (Test-Path $imePath)
if ($imePresent) {
    $size = [math]::Round((Get-Item $imePath).Length / 1KB, 1)
    Write-Host ("  file present: {0} ({1} KB)" -f $imePath, $size)
}

if ($imePresent -and -not $SkipIme) {
    if ($Apply) {
        if (-not $isAdmin) {
            Write-Host "  SKIP: renaming the .ime needs an elevated shell" -ForegroundColor Red
        } else {
            $backup = "$imePath.disabled"
            try {
                if (Test-Path $backup) { Remove-Item $backup -Force -ErrorAction Stop }
                Rename-Item -Path $imePath -NewName (Split-Path $backup -Leaf) -ErrorAction Stop
                Write-Host "  renamed to $backup" -ForegroundColor Green
                Write-Host "  undo: Rename-Item '$backup' '$imePath'"
            } catch {
                Write-Host ("  failed: {0}" -f $_.Exception.Message) -ForegroundColor Red
            }
        }
    } else {
        Write-Host "  would rename it to *.disabled (stops the IME being injected)" -ForegroundColor Yellow
    }
}

# ----------------------------------------------------------------- 2. firewall
Write-Section "2. Firewall rules for Dante discovery and audio"

$ports = @(
    @{ Name = 'Dante Discovery UDP'; Proto = 'UDP'; Ports = @(5353, 4440, 4441, 4442, 4443, 4444, 8700, 8701, 8702, 8703, 8704, 8705, 8708, 8800, 9875) }
    @{ Name = 'Dante Discovery TCP'; Proto = 'TCP'; Ports = @(4440, 4441, 4442, 4443, 4444, 8700, 8701, 8702, 8703, 8704, 8705, 8708) }
)

$existing = Get-NetFirewallRule -ErrorAction SilentlyContinue |
    Where-Object { $_.DisplayName -like 'OpenAES67 Dante*' }

if ($existing) {
    Write-Host ("  {0} OpenAES67 rule(s) already present" -f @($existing).Count) -ForegroundColor Green
} else {
    Write-Host "  no rules for Dante ports found" -ForegroundColor Yellow
}

if (-not $existing -and -not $SkipFirewall) {
    if ($Apply) {
        if (-not $isAdmin) {
            Write-Host "  SKIP: firewall rules need an elevated shell" -ForegroundColor Red
        } else {
            foreach ($spec in $ports) {
                try {
                    New-NetFirewallRule -DisplayName ("OpenAES67 {0}" -f $spec.Name) `
                        -Direction Inbound -Action Allow -Protocol $spec.Proto `
                        -LocalPort $spec.Ports -Profile Any -ErrorAction Stop | Out-Null
                    Write-Host ("  added {0} ({1} ports)" -f $spec.Name, $spec.Ports.Count) -ForegroundColor Green
                } catch {
                    Write-Host ("  failed {0}: {1}" -f $spec.Name, $_.Exception.Message) -ForegroundColor Red
                }
            }
            Write-Host "  undo: Get-NetFirewallRule -DisplayName 'OpenAES67 Dante*' | Remove-NetFirewallRule"
        }
    } else {
        Write-Host "  would add inbound allow rules for those UDP/TCP ports" -ForegroundColor Yellow
    }
}

# ---------------------------------------------------------------------- 3. NIC
Write-Section "3. Network adapter power settings"

$danteIp = '192.168.1.10'
$adapter = Get-NetAdapter -ErrorAction SilentlyContinue | Where-Object {
    (Get-NetIPAddress -InterfaceIndex $_.ifIndex -AddressFamily IPv4 -ErrorAction SilentlyContinue |
        Where-Object { $_.IPAddress -eq $danteIp })
}

if (-not $adapter) {
    Write-Host "  could not find the adapter holding $danteIp" -ForegroundColor Yellow
} else {
    Write-Host ("  Dante interface: {0} ({1})" -f $adapter.Name, $adapter.InterfaceDescription)
    Write-Host ("  status: {0}  link: {1}" -f $adapter.Status, $adapter.LinkSpeed)

    $adv = Get-NetAdapterAdvancedProperty -Name $adapter.Name -ErrorAction SilentlyContinue
    if ($adv) {
        foreach ($name in @('WOL Link Power Saving', 'Energy-Efficient Ethernet', 'Green Ethernet',
                            'Interrupt Moderation Rate', 'JumboPacket')) {
            $p = $adv | Where-Object { $_.DisplayName -eq $name }
            if ($p) {
                $flag = if ($name -eq 'WOL Link Power Saving' -and $p.DisplayValue -match '启用|Enabled|On') { 'Yellow' } else { 'Gray' }
                Write-Host ("    {0,-28} = {1}" -f $name, $p.DisplayValue) -ForegroundColor $flag
            }
        }
    }

    $pm = Get-NetAdapterPowerManagement -Name $adapter.Name -ErrorAction SilentlyContinue
    if ($pm) {
        Write-Host ("    AllowComputerToTurnOffDevice = {0}" -f $pm.AllowComputerToTurnOffDevice)
    }

    $wol = $adv | Where-Object { $_.DisplayName -eq 'WOL Link Power Saving' }
    if ($wol -and $wol.DisplayValue -match '启用|Enabled|On' -and -not $SkipNic) {
        if ($Apply) {
            if (-not $isAdmin) {
                Write-Host "  SKIP: changing adapter properties needs an elevated shell" -ForegroundColor Red
            } else {
                try {
                    Set-NetAdapterAdvancedProperty -Name $adapter.Name -DisplayName 'WOL Link Power Saving' `
                        -DisplayValue '禁用' -ErrorAction Stop
                    Write-Host "  disabled WOL Link Power Saving" -ForegroundColor Green
                    Write-Host ("  undo: Set-NetAdapterAdvancedProperty -Name '{0}' -DisplayName 'WOL Link Power Saving' -DisplayValue '启用'" -f $adapter.Name)
                } catch {
                    Write-Host ("  failed: {0}" -f $_.Exception.Message) -ForegroundColor Red
                }
            }
        } else {
            Write-Host "  would disable WOL Link Power Saving (USB NICs can drop the link)" -ForegroundColor Yellow
        }
    }
}

# ------------------------------------------------------------------ 4. summary
Write-Section "4. Other stability factors on this machine"

$all = Get-NetAdapter -ErrorAction SilentlyContinue
Write-Host ("  adapters: {0}" -f $all.Count)
foreach ($a in $all) {
    $ips = (Get-NetIPAddress -InterfaceIndex $a.ifIndex -AddressFamily IPv4 -ErrorAction SilentlyContinue |
        ForEach-Object { $_.IPAddress }) -join ','
    $note = ''
    if ($a.Status -ne 'Up') { $note = ' (disconnected - can confuse interface selection)' }
    Write-Host ("    [{0}] {1} {2}{3}" -f $a.Status, $a.Name, $ips, $note)
}

$multi = @($all | Where-Object { $_.Status -eq 'Up' }).Count
if ($multi -gt 1) {
    Write-Host "  more than one adapter is up: pin the Dante interface in" -ForegroundColor Yellow
    Write-Host "  Dante Controller > File > Configure Dante Interfaces"
}

$svc = Get-Service DanteDiscovery -ErrorAction SilentlyContinue
if ($svc) {
    $colour = if ($svc.Status -eq 'Running') { 'Green' } else { 'Red' }
    Write-Host ("  DanteDiscovery service: {0}" -f $svc.Status) -ForegroundColor $colour
} else {
    Write-Host "  DanteDiscovery service not installed" -ForegroundColor Red
}

Write-Host ""
if (-not $Apply) {
    Write-Host "Report only. Re-run with -Apply from an elevated shell to make the changes." -ForegroundColor Cyan
}
