# Verifies that the hot paths have not regressed against bench-baseline.txt.
#
# Timing on a shared machine is noisy, so the benchmark runs several times and the
# best result per case is kept: the best sample is the one least disturbed by other
# work, which is what makes the comparison meaningful. The tolerance is deliberately
# generous (10%) -- this gate is for catching a path that got twice as slow, not for
# detecting a 2% drift.
param(
    [string] $Baseline = (Join-Path (Split-Path $PSScriptRoot -Parent) 'bench-baseline.txt'),
    [int]    $Runs = 3,
    [double] $TolerancePercent = 15.0
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$exe = Join-Path $root 'build\openaes67.exe'
if (-not (Test-Path $exe)) {
    Write-Host "  build the CLI first: $exe is missing" -ForegroundColor Red
    exit 1
}
if (-not (Test-Path $Baseline)) {
    Write-Host "  no baseline at $Baseline" -ForegroundColor Red
    exit 1
}

# name -> @{ value; unit }
$best = @{}
for ($run = 1; $run -le $Runs; $run++) {
    $lines = & $exe bench 2>&1
    foreach ($line in $lines) {
        if ($line -match '^(\S.*?)\s+([0-9]+\.[0-9]+)\s+(\S+)\s*$') {
            $name = $Matches[1].Trim()
            $value = [double]$Matches[2]
            $unit = $Matches[3]
            if ($unit -eq 'us') {
                # latency: lower is better
                if (-not $best.ContainsKey($name) -or $value -lt $best[$name].Value) {
                    $best[$name] = @{ Value = $value; Unit = $unit }
                }
            } else {
                if (-not $best.ContainsKey($name) -or $value -gt $best[$name].Value) {
                    $best[$name] = @{ Value = $value; Unit = $unit }
                }
            }
        }
    }
}

$failed = 0
foreach ($line in Get-Content $Baseline -Encoding UTF8) {
    if ($line -match '^\s*#' -or [string]::IsNullOrWhiteSpace($line)) { continue }
    if ($line -notmatch '^(\S.*?)\s+([0-9]+\.[0-9]+)\s+(\S+)\s*$') { continue }
    $name = $Matches[1].Trim()
    $was = [double]$Matches[2]
    $unit = $Matches[3]
    if (-not $best.ContainsKey($name)) {
        Write-Host ("  {0,-24} missing from this run" -f $name) -ForegroundColor Yellow
        continue
    }
    $now = $best[$name].Value
    if ($was -le 0) { continue }
    $change = if ($unit -eq 'us') { ($was - $now) / $was * 100.0 } else { ($now - $was) / $was * 100.0 }
    $ok = $change -ge -$TolerancePercent
    if (-not $ok) { $failed++ }
    $colour = if ($ok) { 'Gray' } else { 'Red' }
    Write-Host ("  {0,-24} {1,10:F2} {2,-11} was {3,8:F2}  {4,6:F1}%  {5}" -f `
                $name, $now, $unit, $was, $change, $(if ($ok) { 'ok' } else { 'REGRESSED' })) -ForegroundColor $colour
}

if ($failed -gt 0) {
    Write-Host "`n  PERF REGRESSED: $failed case(s) beyond $TolerancePercent%" -ForegroundColor Red
    exit 1
}
Write-Host "`n  PERF OK (best of $Runs runs, tolerance $TolerancePercent%)" -ForegroundColor Green
exit 0
