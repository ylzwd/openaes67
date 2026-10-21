<#
.SYNOPSIS
    Verifies the generated icon set by parsing the files directly.

.DESCRIPTION
    This does not trust tools\make-icon.ps1: it re-reads every .ico as bytes and
    checks the container and each entry, then checks every PNG's dimensions and
    transparency. It exists because an .ico can look fine in Explorer while being
    subtly wrong -- a wrong biHeight, an offset past the end of the file, or an
    entry stored as PNG where a DIB was expected (which breaks System.Drawing).

    Run:
        powershell -ExecutionPolicy Bypass -File tools\check-icons.ps1

    Exit code 0 = everything passed.
#>
[CmdletBinding()]
param(
    [string]$AssetDir = ''
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$root = Split-Path -Parent $PSScriptRoot
if (-not $AssetDir) { $AssetDir = Join-Path $root 'assets' }

$failures = New-Object System.Collections.Generic.List[string]
$checks = 0

function Assert-True([bool]$condition, [string]$message) {
    $script:checks++
    if (-not $condition) { $script:failures.Add($message) }
}

function Read-U16([byte[]]$bytes, [int]$offset) {
    return [int]$bytes[$offset] -bor ([int]$bytes[$offset + 1] -shl 8)
}

function Read-U32([byte[]]$bytes, [int]$offset) {
    return [long]$bytes[$offset] -bor ([long]$bytes[$offset + 1] -shl 8) -bor
           ([long]$bytes[$offset + 2] -shl 16) -bor ([long]$bytes[$offset + 3] -shl 24)
}

function Test-Ico([string]$path) {
    $bytes = [System.IO.File]::ReadAllBytes($path)
    $name = Split-Path $path -Leaf
    Write-Host ("`n{0}  ({1:N0} bytes)" -f $name, $bytes.Length)

    Assert-True ($bytes.Length -ge 6) "$name : file too short"
    Assert-True ((Read-U16 $bytes 0) -eq 0) "$name : reserved field is not 0"
    Assert-True ((Read-U16 $bytes 2) -eq 1) "$name : type is not 1 (icon)"
    $count = Read-U16 $bytes 4
    Assert-True ($count -ge 1) "$name : no entries"
    Assert-True ($bytes.Length -ge (6 + 16 * $count)) "$name : directory truncated"

    $sizes = @()
    for ($i = 0; $i -lt $count; $i++) {
        $base = 6 + 16 * $i
        $w = [int]$bytes[$base]
        $h = [int]$bytes[$base + 1]
        $bpp = Read-U16 $bytes ($base + 6)
        $size = Read-U32 $bytes ($base + 8)
        $offset = Read-U32 $bytes ($base + 12)

        if ($w -eq 0 -and $h -eq 0) { $w = 256; $h = 256 }
        Assert-True ($w -eq $h) "$name : entry $i is not square ($w x $h)"
        Assert-True ($bpp -eq 32) "$name : entry $i bpp is $bpp, expected 32"
        Assert-True (($offset + $size) -le $bytes.Length) "$name : entry $i runs past the file"
        Assert-True ($size -gt 0) "$name : entry $i is empty"

        $isPng = ($bytes[$offset] -eq 0x89 -and $bytes[$offset + 1] -eq 0x50 -and
                  $bytes[$offset + 2] -eq 0x4E -and $bytes[$offset + 3] -eq 0x47)
        if ($w -ge 256) {
            Assert-True $isPng "$name : entry $i (${w}px) should be PNG-encoded"
        } else {
            # DIB: BITMAPINFOHEADER with height doubled to cover the AND mask.
            Assert-True (-not $isPng) "$name : entry $i (${w}px) should be a DIB, not PNG"
            $biSize = Read-U32 $bytes $offset
            $biWidth = Read-U32 $bytes ($offset + 4)
            $biHeight = Read-U32 $bytes ($offset + 8)
            Assert-True ($biSize -eq 40) "$name : entry $i biSize is $biSize, expected 40"
            Assert-True ($biWidth -eq $w) "$name : entry $i biWidth $biWidth != $w"
            Assert-True ($biHeight -eq (2 * $w)) "$name : entry $i biHeight $biHeight != $($w*2)"
        }
        $sizes += $w
        Write-Host ("  {0,3}px  {1,7:N0} B  {2}" -f $w, $size, $(if ($isPng) { 'PNG' } else { 'DIB' }))
    }

    Assert-True (($sizes | Sort-Object -Unique).Count -eq $sizes.Count) "$name : duplicate sizes"
    $expected = @(16, 20, 24, 32, 40, 48, 64, 96, 128, 256)
    foreach ($s in $expected) {
        Assert-True ($sizes -contains $s) "$name : missing ${s}px entry"
    }
    Write-Host ("  sizes: {0}" -f ($sizes -join ', '))
}

function Test-Png([string]$path) {
    $name = Split-Path $path -Leaf
    $img = [System.Drawing.Image]::FromFile($path)
    Assert-True ($img.Width -eq $img.Height) "$name : not square ($($img.Width)x$($img.Height))"

    $bmp = New-Object System.Drawing.Bitmap($img)
    $corner = $bmp.GetPixel(0, 0)
    $center = $bmp.GetPixel([int]($bmp.Width / 2), [int]($bmp.Height / 2))
    Assert-True ($corner.A -eq 0) "$name : corner is not transparent (A=$($corner.A))"
    Assert-True ($center.A -gt 0) "$name : centre is fully transparent, icon looks empty"
    $bmp.Dispose(); $img.Dispose()
}

Write-Host '=== .ico files ==='
foreach ($name in 'openaes67.ico', 'openaes67-full.ico') {
    $p = Join-Path $AssetDir $name
    if (-not (Test-Path $p)) { $failures.Add("$name : missing"); continue }
    Test-Ico $p
}

Write-Host "`n=== PNG files ==="
$pngDir = Join-Path $AssetDir 'png'
if (-not (Test-Path $pngDir)) {
    $failures.Add('png directory missing')
} else {
    $pngs = Get-ChildItem $pngDir -Filter *.png | Sort-Object Name
    Write-Host ("  {0} files" -f $pngs.Count)
    foreach ($f in $pngs) { Test-Png $f.FullName }
    # 11 sizes x 2 variants
    Assert-True ($pngs.Count -eq 22) "png : expected 22 files, found $($pngs.Count)"
}

Write-Host "`n=== Application icon in the executables ==="
$exes = @(
    (Join-Path $root 'build\openaes67.exe'),
    (Join-Path $root 'build\selftest.exe'),
    (Join-Path $root 'build-qt\Release\openaes67_gui.exe')
)
foreach ($exe in $exes) {
    if (-not (Test-Path $exe)) { continue }
    $vi = (Get-Item $exe).VersionInfo
    Assert-True ([bool]$vi.ProductName) "$(Split-Path $exe -Leaf) : no ProductName resource"
    $icon = [System.Drawing.Icon]::ExtractAssociatedIcon($exe)
    Assert-True ($null -ne $icon) "$(Split-Path $exe -Leaf) : no icon resource"
    if ($icon) {
        Write-Host ("  {0,-20} icon {1}x{2}  {3} {4}" -f (Split-Path $exe -Leaf),
            $icon.Width, $icon.Height, $vi.ProductName, $vi.FileVersion)
        $icon.Dispose()
    }
}

Write-Host ''
if ($failures.Count -eq 0) {
    Write-Host ("ICONS OK ({0} checks)" -f $checks)
    exit 0
}
Write-Host ("ICONS FAILED ({0} checks, {1} failures)" -f $checks, $failures.Count)
$failures | ForEach-Object { Write-Host "  - $_" }
exit 1
