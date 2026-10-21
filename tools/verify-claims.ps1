#!/usr/bin/env pwsh
<#
.SYNOPSIS
    Checks that the numbers and build flags stated in the documentation still
    match the repository.

.DESCRIPTION
    Written after two "I edited the build file" reports turned out to be wrong:
    a string replacement silently did not match, the script exited 0, and the
    claim went into docs/FINDINGS.md. Reading the value back is the only reliable
    way to know a claim holds.

    Every check prints PASS or FAIL with the observed value, and the script exits
    non-zero if anything fails, so it can gate a commit.

.EXAMPLE
    pwsh tools/verify-claims.ps1
#>
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$cliExe = Join-Path $root 'build\openaes67.exe'
$failures = 0

function Check($name, $ok, $detail) {
    if ($ok) {
        Write-Host ("  [PASS] {0,-52} {1}" -f $name, $detail)
    } else {
        Write-Host ("  [FAIL] {0,-52} {1}" -f $name, $detail) -ForegroundColor Red
        $script:failures++
    }
}

function Read-Text($relative) {
    $path = Join-Path $root $relative
    if (-not (Test-Path $path)) { return '' }
    return [System.IO.File]::ReadAllText($path)
}

Write-Host "OpenAES67 claim verification"
    # --- every option the code accepts must appear in `help` -------------------
    # Documentation and --help are two entry points to the same product; a stale
    # one is a defect. This caught --domain, --exclusive, --print-sdp and
    # --in-device going missing during a help rewrite.
    $mainSrc = Get-Content (Join-Path $root 'src\main.cpp') -Raw -Encoding UTF8
    $accepted = [regex]::Matches($mainSrc, 'optEq\(a,\s*"(--[a-z][a-z0-9-]*)"') |
        ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique
    $helpText = (& $cliExe help 2>&1) -join "`n"
    $helpMissing = @($accepted | Where-Object { $helpText -notmatch [regex]::Escape($_) })
    Check 'help text lists every option' ($helpMissing.Count -eq 0) (
        "missing: " + ($helpMissing -join ', '))


    # --- LICENSE is the canonical MIT text ------------------------------------
    # This file is the one document whose corruption is silent and legal, so it is
    # checked byte-for-byte: it exists, it says MIT, it has no BOM, it uses LF, and
    # it ends with a newline (POSIX text file).
    $licensePath = Join-Path $root 'LICENSE'
    $licenseOk = $false
    $licenseDetail = 'LICENSE missing'
    if (Test-Path $licensePath) {
        $raw = [System.IO.File]::ReadAllBytes($licensePath)
        $licenseText = [System.Text.Encoding]::UTF8.GetString($raw)
        $noBom = -not ($raw.Length -ge 3 -and $raw[0] -eq 0xEF -and $raw[1] -eq 0xBB -and $raw[2] -eq 0xBF)
        $startsWithMit = $licenseText.StartsWith("MIT License")
        $hasCopyright = $licenseText -match 'Copyright \(c\) \d{4} .+'
        $noCr = -not $licenseText.Contains([char]13)
        $endsWithNewline = $raw.Length -gt 0 -and $raw[$raw.Length - 1] -eq 10
        $mitGrant = $licenseText.Contains('Permission is hereby granted, free of charge')
        $mitWarranty = $licenseText.Contains('THE SOFTWARE IS PROVIDED "AS IS"')
        $licenseOk = $noBom -and $startsWithMit -and $hasCopyright -and $noCr -and
                     $endsWithNewline -and $mitGrant -and $mitWarranty
        $licenseDetail = "bom=$(-not $noBom) mit=$startsWithMit copyright=$hasCopyright crlf=$(-not $noCr) eol=$endsWithNewline"
    }
    Check 'LICENSE is the canonical MIT text' $licenseOk $licenseDetail

    # --- no credentials, personal data, local paths or artefacts --------------
    # The audit that produced tools/scan-secrets.ps1 found a private key, the
    # real NIC address and MAC, 38 debug logs and 75 unreferenced screenshots in
    # the tree. Run the same rules on every build so it cannot happen again.
    $scanScript = Join-Path $root 'tools\scan-secrets.ps1'
    $scanOutput = & powershell -NoProfile -ExecutionPolicy Bypass -File $scanScript 2>&1
    $scanClean = $LASTEXITCODE -eq 0
    Check 'no secrets or local paths in the tree' $scanClean (
        ($scanOutput | Select-Object -First 3) -join ' / ')
Write-Host ""

# ---- build flags -----------------------------------------------------------
$buildBat = Read-Text 'build.bat'
Check 'build.bat enables optimisation' ($buildBat -match '/O2') 'expects /O2'
Check 'build.bat enables /W4'          ($buildBat -match '/W4') 'expects /W4'
Check 'build.bat enables /utf-8'       ($buildBat -match '/utf-8') 'expects /utf-8'

$cmake = Read-Text 'CMakeLists.txt'
Check 'CMakeLists enables /W4'         ($cmake -match 'add_compile_options\(/W4') 'expects add_compile_options(/W4 ...)'
Check 'CMakeLists enables /utf-8'      ($cmake -match '/utf-8') 'expects /utf-8'

# ---- self-test count -------------------------------------------------------
$selftestExe = Join-Path $root 'build\selftest.exe'
if (Test-Path $selftestExe) {
    $output = & $selftestExe 2>&1
    $count = ($output | Select-String '\[PASS\]|\[FAIL\]').Count
    $allPass = [bool]($output | Select-String 'ALL PASS')
    Check 'self-test passes' $allPass "$count assertions"
    foreach ($doc in @('README.md', 'CONTRIBUTING.md')) {
        $text = Read-Text $doc
        $m = [regex]::Match($text, '(\d+)\s+assertions')
        if ($m.Success) {
            $documented = [int]$m.Groups[1].Value
            Check "$doc assertion count" ($documented -eq $count) "doc=$documented actual=$count"
        }
    }
} else {
    Check 'self-test binary exists' $false 'build it first (build.bat)'
}

# ---- CLI options documented vs implemented ---------------------------------
$declared = @()
$mainCpp = Read-Text 'src\main.cpp'
foreach ($m in [regex]::Matches($mainCpp, 'optEq\(a,\s*"(--[a-z0-9-]+)"')) {
    $declared += $m.Groups[1].Value
}
$guiCpp = Read-Text 'src\gui\main_gui.cpp'
foreach ($m in [regex]::Matches($guiCpp, 'args\[i\]\s*==\s*"(--[a-z0-9-]+)"')) {
    $declared += $m.Groups[1].Value
}
$declared = $declared | Sort-Object -Unique

$readme = Read-Text 'README.md'
$documented = @()
foreach ($m in [regex]::Matches($readme, '(--[a-z][a-z0-9-]*)')) {
    $documented += $m.Groups[1].Value
}
$documented = $documented | Sort-Object -Unique

# Options that belong to tools, not to this CLI.
$notOurCli = @('--build', '--config', '--one-step', '--rate-ppm', '--skew-ms',
               '--sync-ms', '--seconds', '--domain', '--iface')
$ghosts = $documented | Where-Object { $declared -notcontains $_ -and $notOurCli -notcontains $_ }
Check 'README names only real CLI options' ($ghosts.Count -eq 0) `
      $(if ($ghosts) { "unknown: $($ghosts -join ', ')" } else { "$($documented.Count) documented, all implemented" })

$undocumented = $declared | Where-Object { $documented -notcontains $_ }
Check 'every CLI option is documented' ($undocumented.Count -eq 0) `
      $(if ($undocumented) { "missing: $($undocumented -join ', ')" } else { "$($declared.Count) options documented" })

# ---- repository policies ---------------------------------------------------
$sources = Get-ChildItem (Join-Path $root 'src'), (Join-Path $root 'include'),
                          (Join-Path $root 'tools'), (Join-Path $root 'tests'),
                          (Join-Path $root 'driver'), (Join-Path $root 'docs') `
                          -Recurse -File -Include *.c, *.cpp, *.h, *.py, *.md, *.ps1, *.bat, *.inf `
                          -ErrorAction SilentlyContinue |
           Where-Object { $_.FullName -notmatch '\\out\\' }

# Build the needle at run time so this script does not match itself.
$needle = 'VS' + 'C'
$vsc = $sources | Where-Object { $_.Name -ne 'verify-claims.ps1' } | Select-String -Pattern $needle -CaseSensitive
Check 'no forbidden branding token' ($vsc.Count -eq 0) "$($vsc.Count) found"

$todos = $sources | Where-Object { $_.Extension -in '.c', '.cpp', '.h' } |
         Select-String -Pattern 'TODO|FIXME|XXX' -CaseSensitive
Check 'no TODO/FIXME in code' ($todos.Count -eq 0) "$($todos.Count) found"

$activation = $sources | Where-Object { $_.Extension -in '.c', '.cpp', '.h' } |
              Select-String -Pattern 'activation|licen[cs]e key|ProductKey|WinVerifyTrust'
Check 'no activation or licence-key logic' ($activation.Count -eq 0) "$($activation.Count) found"

Write-Host ""
# Also emit a machine-readable summary line: Write-Host output cannot be captured
# by a caller's variable, the success stream can.
if ($failures -eq 0) {
    Write-Host "ALL CLAIMS HOLD"
    Write-Output "ALL CLAIMS HOLD"
    exit 0
}


Write-Host "$failures CLAIM(S) BROKEN" -ForegroundColor Red
Write-Output "$failures CLAIM(S) BROKEN"
exit 1
