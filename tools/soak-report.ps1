# Summarises a soak run produced by soak64.ps1.
#
# The point is a verdict, not a data dump: did anything drift, leak, or drop?
param(
    [string]$OutDir = (Join-Path $env:TEMP 'openaes67-soak')
)
$ErrorActionPreference = 'Continue'

function Line($text) { "  " + $text }

"=== soak report: $OutDir ==="

# --- how long did it actually run -------------------------------------------
$soakLog = Join-Path $OutDir 'soak.txt'
if (Test-Path $soakLog) {
    $soakLines = Get-Content $soakLog -Encoding UTF8
    if ($soakLines.Count -gt 0) { Line $soakLines[0] }
    if ($soakLines.Count -gt 1) { Line $soakLines[$soakLines.Count - 1] }
}

# --- downlink (tx -> bridge) ------------------------------------------------
$bridgeLog = Join-Path $OutDir 'bridge.log'
$lost = $null
if (Test-Path $bridgeLog) {
    $text = Get-Content $bridgeLog -Encoding UTF8
    $text | Select-String 'ring:|mean=' | Select-Object -Last 3 | ForEach-Object { Line $_.Line.Trim() }
    $means = @()
    foreach ($l in $text) {
        if ($l -match 'mean=\s*(\d+)') { $means += [int]$Matches[1] }
    }
    if ($means.Count -ge 2) {
        $sorted = $means | Sort-Object
        $p95 = $sorted[[int]($sorted.Count * 0.95)]
        $stats = $means | Measure-Object -Average -Minimum -Maximum
        Line ("buffer: min {0} mean {1:N0} p95 {2} max {3} frames ({4} samples)" -f
              $stats.Minimum, $stats.Average, $p95, $stats.Maximum, $means.Count)
        $first = ($means | Select-Object -First 8 | Measure-Object -Average).Average
        $last = ($means | Select-Object -Last 8 | Measure-Object -Average).Average
        Line ("buffer drift: first {0:N0} -> last {1:N0} ({2:+0;-0} frames)" -f
              $first, $last, ($last - $first))
    }
    $text | Select-String 'dropped' | Select-Object -Last 1 | ForEach-Object { Line $_.Line.Trim() }
}

# --- uplink loss at both ends ----------------------------------------------
foreach ($pair in @(@('start', 'beginning'), @('end', 'end'))) {
    $log = Join-Path $OutDir ("up_{0}.log" -f $pair[0])
    if (Test-Path $log) {
        $packets = (Get-Content $log -Encoding UTF8 | Select-String 'packets\s*:\s*(\d+)' |
                    Select-Object -First 1)
        $lostLine = (Get-Content $log -Encoding UTF8 | Select-String 'lost\s*:\s*(\d+)' |
                     Select-Object -First 1)
        # Do not reuse $Matches across pipelines: it belongs to the last -match.
        $packetText = if ($packets -and $packets.Line -match '(\d+)') { $Matches[1] } else { '?' }
        $l = if ($lostLine) { $lostLine.Line.Trim() } else { 'lost: ?' }
        Line ("uplink {0}: {1} packets, {2}" -f $pair[1], $packetText, $l)
    }
}

# --- ASIO host --------------------------------------------------------------
$asioLog = Join-Path $OutDir 'asio.log'
if (Test-Path $asioLog) {
    Get-Content $asioLog -Encoding UTF8 | Select-String 'callbacks|input peak|ALL PASS|FAILED' |
        ForEach-Object { Line $_.Line.Trim() }
}

# --- process growth ---------------------------------------------------------
$csv = Join-Path $OutDir 'samples.csv'
if (Test-Path $csv) {
    # The last row can be sampled after the workers exited (value -1); drift must
    # be measured between the first and last *live* samples.
    $rows = @(Import-Csv $csv | Where-Object { [double]$_.bridge_ws_mb -gt 0 })
    if ($rows.Count -ge 2) {
        $first = $rows[0]
        $last = $rows[$rows.Count - 1]
        foreach ($name in @('tx', 'bridge', 'asio')) {
            $w0 = [double]$first."${name}_ws_mb"
            $w1 = [double]$last."${name}_ws_mb"
            $h0 = [int]$first."${name}_handles"
            $h1 = [int]$last."${name}_handles"
            if ($w0 -lt 0) { continue }
            Line ("{0,-6}: working set {1,7:N1} -> {2,7:N1} MB ({3:+0.0;-0.0} MB), handles {4} -> {5} ({6:+0;-0})" -f
                  $name, $w0, $w1, ($w1 - $w0), $h0, $h1, ($h1 - $h0))
        }
        Line ("samples: {0} minutes" -f $rows.Count)
    }
}

# --- verdict ----------------------------------------------------------------
$problems = @()
if (Test-Path $csv) {
    $rows = @(Import-Csv $csv | Where-Object { [double]$_.bridge_ws_mb -gt 0 })
    if ($rows.Count -ge 2) {
        foreach ($name in @('tx', 'bridge', 'asio')) {
            $w0 = [double]$rows[0]."${name}_ws_mb"
            $w1 = [double]$rows[$rows.Count - 1]."${name}_ws_mb"
            $h0 = [int]$rows[0]."${name}_handles"
            $h1 = [int]$rows[$rows.Count - 1]."${name}_handles"
            if ($w0 -lt 0) { continue }
            if ($w1 - $w0 -gt 20) { $problems += "$name grew $([math]::Round($w1 - $w0,1)) MB" }
            if ($h1 - $h0 -gt 100) { $problems += "$name leaked $($h1 - $h0) handles" }
        }
    }
}
foreach ($name in @('start', 'end')) {
    $log = Join-Path $OutDir ("up_{0}.log" -f $name)
    if (Test-Path $log) {
        $line = Get-Content $log -Encoding UTF8 | Select-String 'lost\s*:\s*(\d+)' | Select-Object -First 1
        if ($line -and $line.Line -match '(\d+)') {
            $n = [int]$Matches[1]
            if ($n -gt 0) { $problems += "uplink lost $n at $name" }
        }
    }
}
if ($problems.Count -eq 0) {
    "`nVERDICT: clean (no growth, no uplink loss)"
} else {
    "`nVERDICT: " + ($problems -join '; ')
}
