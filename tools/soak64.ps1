# 30 min 64-channel duplex soak.
#
# Runs tx -> bridge -> ASIO host tone, and samples the processes every minute:
# working set, handle count, thread count. Uplink loss is sampled with short
# captures at the start and near the end (a full-length capture would hold
# 64 ch * 48 kHz * 2 B * 30 min = ~11 GB in memory, which `rx --out` does).
param(
    [int]$Minutes = 30,
    # Derived from the script location: the repository path is not ASCII, and
    # Windows PowerShell 5.1 reads a BOM-less script as ANSI, which turned the
    # literal path into garbage and made Start-Process report "file not found".
    [string]$Root = (Split-Path $PSScriptRoot -Parent),
    [string]$OutDir = (Join-Path $env:TEMP 'openaes67-soak')
)
$ErrorActionPreference = 'Continue'
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$exe = Join-Path $Root 'build\openaes67.exe'
$asio = Join-Path $Root 'asio_driver\out\asio_host_test.exe'
$seconds = $Minutes * 60

Get-Process openaes67, openaes67_gui, asio_host_test -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500

$stamp = Get-Date -Format 'HH:mm:ss'
"[{0}] soak start: {1} min, 64 ch @ 5 ms" -f $stamp, $Minutes | Tee-Object -FilePath "$OutDir\soak.txt"

# 64 channels at 5 ms is 30720 B/packet: loopback and a local NIC handle it, but
# the packets are fragmented. That is deliberate -- it exercises the same path a
# real deployment uses.
$tx = Start-Process -FilePath $exe -ArgumentList @(
    'tx', '--iface', '192.168.1.10', '--group', '239.69.8.8', '--port', '5110',
    '--channels', '64', '--ptime', '5', '--seconds', "$seconds"
) -PassThru -WindowStyle Hidden -RedirectStandardOutput "$OutDir\tx.log"

Start-Sleep -Milliseconds 400
$bridge = Start-Process -FilePath $exe -ArgumentList @(
    'bridge', '--iface', '192.168.1.10', '--group', '239.69.8.8', '--port', '5110',
    '--send-group', '239.69.9.9', '--send-port', '5090',
    '--channels', '64', '--ptime', '5', '--shm', 'OpenAES67Audio', '--seconds', "$seconds"
) -PassThru -WindowStyle Hidden -RedirectStandardOutput "$OutDir\bridge.log"

Start-Sleep -Seconds 1
# Absolute DLL path: the host was started with a working directory of its own
# folder, so a relative "out\..." resolved inside out\out and LoadLibrary failed
# with error 126 -- which silently turned the soak into a half-duplex one.
$asioDll = Join-Path (Split-Path $asio) 'openaes67_asio64.dll'
$asioProc = Start-Process -FilePath $asio -ArgumentList @("$seconds", $asioDll, 'tone') `
    -WorkingDirectory (Split-Path $asio) -PassThru -WindowStyle Hidden `
    -RedirectStandardOutput "$OutDir\asio.log"

# One short uplink capture at the start and one near the end.
function Sample-Uplink([string]$name) {
    $p = Start-Process -FilePath $exe -ArgumentList @(
        'rx', '--iface', '192.168.1.10', '--group', '239.69.9.9', '--port', '5090',
        '--channels', '64', '--ptime', '5', '--seconds', '20', '--out', "$OutDir\up_$name.wav"
    ) -PassThru -WindowStyle Hidden -RedirectStandardOutput "$OutDir\up_$name.log"
    [void]$p.WaitForExit(40000)
}

$csv = "$OutDir\samples.csv"
'elapsed_s,tx_ws_mb,tx_handles,bridge_ws_mb,bridge_handles,asio_ws_mb,asio_handles' |
    Set-Content -Path $csv -Encoding UTF8

Sample-Uplink 'start'

$sw = [System.Diagnostics.Stopwatch]::StartNew()
for ($i = 1; $i -le $Minutes; $i++) {
    # The end-to-end uplink capture has to finish while the bridge is still up, so
    # it starts one minute before the workers' own deadline.
    if ($i -eq $Minutes - 1) { Sample-Uplink 'end' }
    Start-Sleep -Seconds 60
    $row = @($sw.Elapsed.TotalSeconds)
    foreach ($p in @($tx, $bridge, $asioProc)) {
        $proc = Get-Process -Id $p.Id -ErrorAction SilentlyContinue
        if ($proc) {
            $row += [math]::Round($proc.WorkingSet64 / 1MB, 1)
            $row += $proc.HandleCount
        } else {
            $row += -1
            $row += -1
        }
    }
    ($row -join ',') | Add-Content -Path $csv -Encoding UTF8
    "[{0}] sample {1}/{2}" -f (Get-Date -Format 'HH:mm:ss'), $i, $Minutes |
        Add-Content -Path "$OutDir\soak.txt"
}

# Let the workers finish on their own so their final summaries are written.
[void]$bridge.WaitForExit(120000)
if (Get-Process -Id $tx.Id -ErrorAction SilentlyContinue) { Stop-Process -Id $tx.Id -Force }
if (Get-Process -Id $asioProc.Id -ErrorAction SilentlyContinue) { Stop-Process -Id $asioProc.Id -Force }
Get-Process openaes67, asio_host_test -ErrorAction SilentlyContinue | Stop-Process -Force

"[{0}] soak done" -f (Get-Date -Format 'HH:mm:ss') | Add-Content -Path "$OutDir\soak.txt"
