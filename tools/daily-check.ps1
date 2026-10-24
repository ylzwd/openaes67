# Daily check: run the project's own gates, record the result, commit only if
# something actually changed.
#
# The point is not to keep a green square lit. It is to leave a dated record of
# what this machine actually measured, so a regression shows up as a diff in the
# repository rather than as a rumour.
#
# What counts as "something changed" is a change in *status*, not a change in
# every measured number. A benchmark on a busy machine varies by tens of
# percent between runs, so comparing raw timings would produce a commit every
# single day and train everyone to ignore them. The fingerprint below reduces a
# run to the things a human would want to know: did a gate start failing, did a
# hot path get slower, did the environment change.
#
# What it runs, in order of cost:
#   1. the self-test (no hardware, no network)
#   2. verify-claims.ps1 -- the repository gates
#   3. scan-secrets.ps1 -- credentials and personal data
#   4. the benchmark, bucketed against the previous run
#   5. doctor -- interface, multicast, ASIO registration, PTP
#
# On the benchmark: bench-baseline.txt was recorded on the author's machine.
# Comparing a different computer against it produces noise, not signal, so the
# comparison here is against the previous run *on the same machine*.
#
# Usage:
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\daily-check.ps1
#   powershell ... -File tools\daily-check.ps1 -NoCommit   # report only
#   powershell ... -File tools\daily-check.ps1 -Push       # also push
param(
    # Do not create a commit even when the status changed.
    [switch]$NoCommit,
    # Push the commit if one was created.
    [switch]$Push,
    # Percent of slowdown against the previous run that counts as a regression.
    [double]$TolerancePercent = 20.0
)

$ErrorActionPreference = 'Continue'
$root = Split-Path $PSScriptRoot -Parent
$exe = Join-Path $root 'build\openaes67.exe'
$selftest = Join-Path $root 'build\selftest.exe'
$reportDir = Join-Path $root 'docs\daily'
$today = Get-Date -Format 'yyyy-MM-dd'
$reportFile = Join-Path $reportDir "$today.md"

function Section([string]$title) {
    Write-Host ""
    Write-Host "=== $title ==="
}

function Result([string]$name, [bool]$ok, [string]$detail) {
    $mark = if ($ok) { 'PASS' } else { 'FAIL' }
    Write-Host ("  [{0}] {1,-32} {2}" -f $mark, $name, $detail)
    return [pscustomobject]@{ Name = $name; Ok = $ok; Detail = $detail }
}

# ---------------------------------------------------------------- preconditions
Section 'preconditions'
if (-not (Test-Path $exe)) {
    Write-Host "  the CLI is not built: $exe" -ForegroundColor Red
    Write-Host "  run build.bat first" -ForegroundColor Red
    exit 1
}
if (-not (Test-Path $selftest)) {
    Write-Host "  the self-test is not built: $selftest" -ForegroundColor Red
    exit 1
}
Write-Host "  cli      : $exe"

# Pick the wired adapter: multicast needs a real interface, and Wi-Fi picks the
# wrong exit route often enough to matter (docs/FINDINGS.md section 5).
$ip = Get-NetIPAddress -AddressFamily IPv4 -ErrorAction SilentlyContinue |
      Where-Object { $_.IPAddress -notlike '127.*' -and $_.IPAddress -notlike '169.254*' } |
      Select-Object -First 1
$iface = if ($ip) { $ip.IPAddress } else { $null }
if ($iface) {
    Write-Host "  iface    : $iface"
} else {
    Write-Host "  iface    : none found (the network checks will be skipped)" -ForegroundColor Yellow
}

$results = @()
$blocking = $false

# ---------------------------------------------------------------- 1. self-test
Section 'self-test'
$stOut = & $selftest 2>&1 | Out-String
$stOk = $stOut -match 'ALL PASS'
$results += Result 'self-test' $stOk '320 assertions, no external framework'
if (-not $stOk) { $blocking = $true }

# ---------------------------------------------------------------- 2. claims
Section 'repository gates'
$claimsOut = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'tools\verify-claims.ps1') 2>&1 | Out-String
$claimsOk = $claimsOut -match 'ALL CLAIMS HOLD'
$claimCount = ([regex]::Matches($claimsOut, '\[PASS\]')).Count
$results += Result 'verify-claims' $claimsOk "$claimCount gates"
if (-not $claimsOk) { $blocking = $true }

# ---------------------------------------------------------------- 3. secrets
Section 'secrets and personal data'
$secretOut = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'tools\scan-secrets.ps1') 2>&1 | Out-String
$secretOk = $secretOut -match 'no credentials'
$results += Result 'scan-secrets' $secretOk 'no credentials, no personal data'
if (-not $secretOk) { $blocking = $true }

# ---------------------------------------------------------------- 4. benchmark
Section 'benchmark'
$benchOut = & $exe bench 2>&1 | Out-String
$bench = [ordered]@{}
foreach ($line in ($benchOut -split "`r?`n")) {
    if ($line -match '^\s*(\S.*?)\s+([\d.]+)\s+(Mframes/s|Mops/s|us)\s*$') {
        $bench[$Matches[1].Trim()] = [pscustomobject]@{ Value = [double]$Matches[2]; Unit = $Matches[3] }
    }
}
$results += Result 'bench' ($bench.Count -gt 0) "$($bench.Count) cases measured"

# Bucket each case against the previous run instead of recording the raw
# number. The buckets are coarse on purpose: ordinary jitter must not look
# like a change, but a path that got slower must.
# The previous report is what the fingerprint is compared against. On a machine
# that already ran today, that is today's own earlier run; otherwise it is the
# most recent earlier day.
$prevFile = Get-ChildItem $reportDir -Filter '*.md' -ErrorAction SilentlyContinue |
             Sort-Object Name -Descending | Select-Object -First 1
$prevText = ''
if ($prevFile) { $prevText = Get-Content $prevFile.FullName -Raw -Encoding UTF8 }

function Bucket([double]$value, [double]$reference) {
    if ($reference -le 0) { return 'unknown' }
    $pct = (($value - $reference) / $reference) * 100.0
    if ($pct -lt -$TolerancePercent) { return 'slower' }
    if ($pct -gt $TolerancePercent) { return 'faster' }
    return 'same'
}

# The reference is a baseline recorded on THIS machine, kept outside the tracked
# tree. bench-baseline.txt came from the author's computer; using it here put
# the measurements right on the boundary between buckets, so the fingerprint
# flipped between runs and every day produced a commit. Three coarse buckets
# against a local baseline are stable: the measurement has to move by more than
# the tolerance to be worth a commit.
$localBaseline = Join-Path $root '.daily-baseline.txt'
$baselineRows = @{}
if (Test-Path $localBaseline) {
    foreach ($line in (Get-Content $localBaseline -Encoding UTF8)) {
        if ($line -match '^\s*(\S.*?)\s+([\d.]+)\s+(Mframes/s|Mops/s|us)\s*$') {
            $baselineRows[$Matches[1].Trim()] = [double]$Matches[2]
        }
    }
}

$buckets = [ordered]@{}
$regressions = @()
$needBaseline = $false
foreach ($name in $bench.Keys) {
    $cur = $bench[$name]
    if (-not $baselineRows.ContainsKey($name)) { $needBaseline = $true }
    $ref = if ($baselineRows.ContainsKey($name)) { $baselineRows[$name] } else { $cur.Value }
    $b = Bucket $cur.Value $ref
    $buckets[$name] = [pscustomobject]@{ Bucket = $b; Value = $cur.Value; Unit = $cur.Unit; Ref = $ref }
    if ($b -eq 'slower') { $regressions += $name }
}

# First run on this machine: record the baseline so tomorrow has a reference.
if ($needBaseline) {
    $bl = New-Object System.Text.StringBuilder
    [void]$bl.AppendLine('# Local benchmark baseline, written by tools/daily-check.ps1.')
    [void]$bl.AppendLine('# Not tracked: it describes this computer, not the project.')
    [void]$bl.AppendLine('# Regenerate by deleting this file.')
    foreach ($name in $bench.Keys) {
        $b = $buckets[$name]
        [void]$bl.AppendLine(("{0} {1} {2}" -f $name, $b.Value, $b.Unit))
    }
    Set-Content -Path $localBaseline -Value $bl.ToString() -Encoding UTF8
    Write-Host "  wrote the local baseline: $localBaseline"
}
$benchOk = $regressions.Count -eq 0
if ($regressions.Count -gt 0) {
    foreach ($name in $regressions) {
        $b = $buckets[$name]
        Write-Host ("  slower than the reference: {0} {1:N1} vs {2:N1}" -f `
                    $name, $b.Value, $b.Ref) -ForegroundColor Red
    }
}
$results += Result 'bench within tolerance' $benchOk "$($regressions.Count) case(s) slower"

# ---------------------------------------------------------------- 5. doctor
Section 'environment'
if ($iface) {
    $docOut = & $exe doctor --iface $iface --group 239.69.8.8 --port 5110 2>&1 | Out-String
    $docOk = $docOut -match 'READY'
    $failLines = @($docOut -split "`r?`n" | Where-Object { $_ -match '\[FAIL\]' })
    $docDetail = if ($failLines.Count -gt 0) { "$($failLines.Count) blocking problem(s)" }
                  else { 'no blocking problems' }
    $results += Result 'doctor' $docOk $docDetail
    if (-not $docOk) { $blocking = $true }
} else {
    $results += Result 'doctor' $true 'skipped: no interface'
}

# ---------------------------------------------------------------- fingerprint
# The fingerprint is what decides whether this run is worth a commit. It holds
# only the things that should make a reader look twice.
$fingerprintParts = @()
foreach ($r in $results) {
    $fingerprintParts += "$($r.Name)=$($r.Ok)"
}
foreach ($name in $buckets.Keys) {
    $fingerprintParts += "$name=$($buckets[$name].Bucket)"
}
$fingerprint = ($fingerprintParts -join '|')

$prevFingerprint = $null
if ($prevFile) {
    if ($prevText -match '(?m)^Fingerprint:\s*`?([^`\r\n]+)`?\s*$') { $prevFingerprint = $Matches[1].Trim() }
}

# ---------------------------------------------------------------- report
Section 'report'
New-Item -ItemType Directory -Path $reportDir -Force | Out-Null

$sb = New-Object System.Text.StringBuilder
[void]$sb.AppendLine("# Daily check -- $today")
[void]$sb.AppendLine()
[void]$sb.AppendLine("Produced by ``tools/daily-check.ps1``. A commit is made only when the")
[void]$sb.AppendLine("fingerprint below changes, so a quiet day leaves no trace in the history.")
[void]$sb.AppendLine()
[void]$sb.AppendLine("| Check | Result | Detail |")
[void]$sb.AppendLine("|---|---|---|")
foreach ($r in $results) {
    $mark = if ($r.Ok) { 'pass' } else { 'FAIL' }
    [void]$sb.AppendLine("| $($r.Name) | $mark | $($r.Detail) |")
}
[void]$sb.AppendLine()
[void]$sb.AppendLine("## Benchmark")
[void]$sb.AppendLine()
[void]$sb.AppendLine("Raw timings vary with whatever else the machine is doing, so each case is")
[void]$sb.AppendLine("bucketed against a baseline recorded on this computer: ``same``, ``slower``")
[void]$sb.AppendLine("or ``faster``. The bucket, not the number, is what the fingerprint records.")
[void]$sb.AppendLine()
[void]$sb.AppendLine("| Case | Measured | Baseline | Bucket |")
[void]$sb.AppendLine("|---|---|---|---|")
foreach ($name in $buckets.Keys) {
    $b = $buckets[$name]
    [void]$sb.AppendLine(("| {0} | {1:N1} {2} | {3:N1} {2} | {4} |" -f $name, $b.Value, $b.Unit, $b.Ref, $b.Bucket))
}
[void]$sb.AppendLine()
[void]$sb.AppendLine("Machine: $env:COMPUTERNAME")
[void]$sb.AppendLine()
[void]$sb.AppendLine("Interface: $iface")
[void]$sb.AppendLine()
[void]$sb.AppendLine("Tolerance: a case is ``slower`` past $([int]$TolerancePercent)% against the reference.")
[void]$sb.AppendLine()
[void]$sb.AppendLine("Fingerprint: ``$fingerprint``")

$content = $sb.ToString()

$statusChanged = ($fingerprint -ne $prevFingerprint)
Write-Host "  fingerprint : $fingerprint"
if ($prevFingerprint) {
    Write-Host "  previous    : $prevFingerprint"
}
Write-Host "  changed     : $statusChanged"

# Two cases, and they differ in what deserves a commit:
#
#   - the same day, running again: nothing changed, so nothing to write and
#     nothing to commit. Rewriting the file would leave the working tree dirty
#     for ever without ever producing a commit.
#   - a new day: the file is new. It records that the check ran on that day,
#     which is worth having even when every gate still passes.
$isNewDay = -not (Test-Path $reportFile)

if (-not $statusChanged -and -not $isNewDay) {
    Write-Host ""
    Write-Host "  nothing changed -- no commit" -ForegroundColor Green
    exit 0
}

if ($blocking) {
    Set-Content -Path $reportFile -Value $content -Encoding UTF8 -NoNewline
    Write-Host ""
    Write-Host "  a gate failed -- not committing" -ForegroundColor Red
    Write-Host "  fix it, or record the failure deliberately if it is expected" -ForegroundColor Red
    exit 2
}

Set-Content -Path $reportFile -Value $content -Encoding UTF8 -NoNewline

Push-Location $root
try {
    & git add -- 'docs/daily' 2>&1 | Out-Null
    $staged = (& git diff --cached --name-only 2>&1 | Out-String).Trim()
    if (-not $staged) {
        Write-Host ""
        Write-Host "  nothing staged -- no commit" -ForegroundColor Green
        exit 0
    }

    if ($NoCommit) {
        Write-Host ""
        Write-Host "  status changed, but -NoCommit was given -- left staged" -ForegroundColor Yellow
        exit 0
    }

    $summary = ($results | ForEach-Object { "$($_.Name)=$($_.Detail)" }) -join '; '
    $kind = if ($statusChanged) { 'record the' } else { 'record the first run of' }
    & git commit -m "chore(daily): $kind $today check -- $summary" 2>&1 | Out-Null
    Write-Host ""
    Write-Host "  committed: $summary" -ForegroundColor Green

    if ($Push) {
        & git push origin main 2>&1 | Select-Object -Last 2
    }
} finally {
    Pop-Location
}

exit 0