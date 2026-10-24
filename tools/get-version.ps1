# Prints the project version, taken from include/openaes67/version.h.
#
# One place decides the version, everything else asks. Before this existed, four
# scripts and one resource script each carried their own literal, and the shipped
# executable ended up claiming 0.5.0.0 while the binary printed 0.1.1.
#
# Written for two kinds of caller:
#   PowerShell: . .\tools\get-version.ps1;  $v = Get-ProjectVersion
#   batch:      for /f "delims=" %%v in ('powershell -NoProfile -File tools\get-version.ps1') do set V=%%v
param(
    # Emit the four-part form used by Windows version resources and INF DriverVer
    # (0.1.1 becomes 0.1.1.0).
    [switch] $Numeric,
    [switch] $MajorMinorPatchOnly
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$header = Join-Path $root 'include\openaes67\version.h'

function Get-ProjectVersion {
    param([switch] $Numeric)

    $path = Join-Path (Split-Path -Parent $PSScriptRoot) 'include\openaes67\version.h'
    if (-not (Test-Path $path)) { throw "cannot find version header: $path" }

    $text = [System.IO.File]::ReadAllText($path, [System.Text.Encoding]::UTF8)

    if ($Numeric) {
        # The numeric form is what app.rc and stampinf need; it is gated against the
        # string form by verify-claims.ps1, so the two cannot drift silently.
        $m = [regex]::Match($text, 'OPENAES67_VERSION_NUMERIC\s+(\d+)\s*,\s*(\d+)\s*,\s*(\d+)(?:\s*,\s*(\d+))?')
        if (-not $m.Success) { throw 'OPENAES67_VERSION_NUMERIC is missing from version.h' }
        $build = if ($m.Groups[4].Success) { $m.Groups[4].Value } else { '0' }
        return ($m.Groups[1].Value + '.' + $m.Groups[2].Value + '.' + $m.Groups[3].Value + '.' + $build)
    }

    $m = [regex]::Match($text, '#define\s+OPENAES67_VERSION\s+"([0-9]+\.[0-9]+\.[0-9]+)"')
    if (-not $m.Success) { throw 'OPENAES67_VERSION is missing from version.h' }
    return $m.Groups[1].Value
}

if ($MyInvocation.InvocationName -ne '.') {
    if (-not (Test-Path $header)) { throw "cannot find version header: $header" }
    if ($Numeric) { Get-ProjectVersion -Numeric } else { Get-ProjectVersion }
}
