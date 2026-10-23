# Adds an SPDX licence header to every source file that does not have one.
#
# MIT requires the copyright notice to travel with the code, and SPDX is the
# machine-readable form the rest of the toolchain understands. The header goes
# on line 1 so each file keeps whatever it already had: a #pragma once, a
# leading comment block, or a copyright of its own.
#
# Idempotent: a file that already carries SPDX-License-Identifier is skipped.

param(
    [string]$Root = "."
)

$ErrorActionPreference = "Stop"

$targets = @()
$targets += Get-ChildItem -Path "$Root\src"         -Recurse -File -Include *.cpp,*.h
$targets += Get-ChildItem -Path "$Root\include"     -Recurse -File -Include *.h
$targets += Get-ChildItem -Path "$Root\tests"       -Recurse -File -Include *.cpp
$targets += Get-ChildItem -Path "$Root\asio_driver" -Recurse -File -Include *.cpp,*.h
$targets += Get-ChildItem -Path "$Root\driver"      -Recurse -File -Include *.c,*.cpp,*.h

# // works in both C and C++ and every toolchain this project targets accepts it.
$header = @(
    "// SPDX-License-Identifier: MIT",
    "// Copyright (c) 2026 OpenAES67 contributors",
    "//",
    "// Every behaviour here is traceable to a published standard (AES67-2018,",
    "// RFC 3550, RFC 4566, RFC 2974, IEEE 1588) or to a reproducible",
    "// measurement recorded in docs/FINDINGS.md."
)

$added = 0
$skipped = 0

foreach ($f in $targets) {
    $raw = [System.IO.File]::ReadAllText($f.FullName)
    if ($raw -match "SPDX-License-Identifier") {
        $skipped++
        continue
    }
    # Preserve the file's original line ending so the diff stays clean.
    $eol = if ($raw.Contains("`r`n")) { "`r`n" } else { "`n" }
    [System.IO.File]::WriteAllText($f.FullName, ($header -join $eol) + $eol + $eol + $raw)
    $added++
}

Write-Host "SPDX header added to $added file(s); $skipped already had one"
if ($added -eq 0 -and $skipped -eq 0) {
    throw "no source files found -- check -Root"
}