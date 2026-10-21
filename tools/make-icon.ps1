<#
.SYNOPSIS
    Generates the complete icon set from logo.png: .ico files and PNGs, in every
    size the front end (window, tray, About box) and the back end (exe, installer,
    docs) need.

.DESCRIPTION
    Output layout

        assets\openaes67.ico             application icon, mark only, 16-256
        assets\openaes67-full.ico        logo with the word, 16-256 (docs, installer)
        assets\png\openaes67-<n>.png     mark only, transparent, 16-512
        assets\png\openaes67-full-<n>.png    logo with the word, transparent

    Three things make this non-trivial:

      * The source logo is an opaque white bitmap (24bpp, no alpha). White is keyed
        out so icons do not carry a white tile on a dark taskbar.
      * The logo is a mark plus the word "OpenAES67". At 16-24 px the word is an
        illegible smudge, so the *application* icon uses the mark alone at every
        size; the full logo is kept as a separate file for large placements.
      * Sizes below 256 are stored as DIB entries and only 256 as PNG. All-PNG is
        legal and Explorer reads it, but System.Drawing (and therefore any .NET
        tool) fails to extract a specific size from such a file.

    Run after changing logo.png:
        powershell -ExecutionPolicy Bypass -File tools\make-icon.ps1

    Verify the result with:
        powershell -ExecutionPolicy Bypass -File tools\check-icons.ps1
#>
[CmdletBinding()]
param(
    [string]$Source = '',
    [string]$AssetDir = '',
    # Sizes stored inside the .ico files. 20 and 40 matter on 125%/150% DPI.
    [int[]]$IcoSizes = @(16, 20, 24, 32, 40, 48, 64, 96, 128, 256),
    # Sizes exported as standalone PNGs, for docs, README and installer art.
    [int[]]$PngSizes = @(16, 20, 24, 32, 40, 48, 64, 96, 128, 256, 512)
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$root = Split-Path -Parent $PSScriptRoot
if (-not $Source)   { $Source   = Join-Path $root 'logo.png' }
if (-not $AssetDir) { $AssetDir = Join-Path $root 'assets' }
if (-not (Test-Path $Source)) { throw "logo not found: $Source" }

$logo = New-Object System.Drawing.Bitmap($Source)
Write-Host "source : $Source ($($logo.Width)x$($logo.Height))"

# --- key the outer white background out to transparency ---------------------
#
# Only white that is CONNECTED TO THE BORDER is removed. Keying every near-white
# pixel instead also punched out the mark's inner white panel, leaving the middle
# of the icon transparent -- visible as a dark hole on a dark taskbar, and caught
# by tools\check-icons.ps1 ("centre is fully transparent, icon looks empty").
function ConvertTo-TransparentBackground([System.Drawing.Bitmap]$bitmap) {
    $w = $bitmap.Width
    $h = $bitmap.Height
    $out = New-Object System.Drawing.Bitmap($w, $h,
        [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)

    $isWhite = New-Object 'bool[]' ($w * $h)
    for ($y = 0; $y -lt $h; $y++) {
        for ($x = 0; $x -lt $w; $x++) {
            $p = $bitmap.GetPixel($x, $y)
            $isWhite[$y * $w + $x] = ($p.R -ge 245 -and $p.G -ge 245 -and $p.B -ge 245)
        }
    }

    # Flood fill from the border over white pixels only.
    $outside = New-Object 'bool[]' ($w * $h)
    $queue = New-Object System.Collections.Generic.Queue[int]
    for ($x = 0; $x -lt $w; $x++) {
        # Parentheses matter: in an array literal the comma binds tighter than
        # subtraction, so "@(0, $h - 1)" builds the array 0,$h and then subtracts.
        foreach ($y in @(0, ($h - 1))) {
            $idx = $y * $w + $x
            if ($isWhite[$idx] -and -not $outside[$idx]) { $outside[$idx] = $true; $queue.Enqueue($idx) }
        }
    }
    for ($y = 0; $y -lt $h; $y++) {
        foreach ($x in @(0, ($w - 1))) {
            $idx = $y * $w + $x
            if ($isWhite[$idx] -and -not $outside[$idx]) { $outside[$idx] = $true; $queue.Enqueue($idx) }
        }
    }
    while ($queue.Count -gt 0) {
        $idx = $queue.Dequeue()
        $x = $idx % $w
        $y = [int][Math]::Floor($idx / $w)
        foreach ($d in @(@(1, 0), @(-1, 0), @(0, 1), @(0, -1))) {
            $nx = $x + $d[0]
            $ny = $y + $d[1]
            if ($nx -lt 0 -or $ny -lt 0 -or $nx -ge $w -or $ny -ge $h) { continue }
            $nIdx = $ny * $w + $nx
            if ($isWhite[$nIdx] -and -not $outside[$nIdx]) {
                $outside[$nIdx] = $true
                $queue.Enqueue($nIdx)
            }
        }
    }

    for ($y = 0; $y -lt $h; $y++) {
        for ($x = 0; $x -lt $w; $x++) {
            $idx = $y * $w + $x
            if ($outside[$idx]) {
                $out.SetPixel($x, $y, [System.Drawing.Color]::Transparent)
            } else {
                $p = $bitmap.GetPixel($x, $y)
                $out.SetPixel($x, $y, [System.Drawing.Color]::FromArgb(255, $p.R, $p.G, $p.B))
            }
        }
    }
    return $out
}

# --- locate the mark (the part above the word) -------------------------------
function Get-MarkBounds([System.Drawing.Bitmap]$bitmap) {
    $gap = -1
    for ($y = [int]($bitmap.Height * 0.3); $y -lt $bitmap.Height; $y++) {
        $dark = 0
        for ($x = 0; $x -lt $bitmap.Width; $x++) {
            $p = $bitmap.GetPixel($x, $y)
            if (($p.R + $p.G + $p.B) -lt 600) { $dark++ }
        }
        if ($dark -le 2) { $gap = $y; break }
    }
    if ($gap -le 0) { $gap = $bitmap.Height }

    $left = $bitmap.Width; $right = -1; $top = -1; $bottom = -1
    for ($y = 0; $y -lt $gap; $y++) {
        for ($x = 0; $x -lt $bitmap.Width; $x++) {
            $p = $bitmap.GetPixel($x, $y)
            if (($p.R + $p.G + $p.B) -lt 600) {
                if ($x -lt $left)  { $left = $x }
                if ($x -gt $right) { $right = $x }
                if ($top -lt 0)    { $top = $y }
                $bottom = $y
            }
        }
    }
    if ($right -lt $left -or $bottom -lt $top) {
        return (New-Object System.Drawing.Rectangle -ArgumentList 0, 0, $bitmap.Width, $gap)
    }
    $markWidth = ([int]$right - [int]$left) + 1
    $markHeight = ([int]$bottom - [int]$top) + 1
    return (New-Object System.Drawing.Rectangle -ArgumentList `
        ([int]$left), ([int]$top), $markWidth, $markHeight)
}

# --- square canvas with a transparent margin ---------------------------------
function New-SquareCanvas([System.Drawing.Image]$image, [double]$marginRatio) {
    $inner = [Math]::Max($image.Width, $image.Height)
    $side = [int][Math]::Ceiling($inner * (1.0 + 2.0 * $marginRatio))
    $canvas = New-Object System.Drawing.Bitmap($side, $side,
        [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($canvas)
    $g.Clear([System.Drawing.Color]::Transparent)
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $dx = [int](($side - $image.Width) / 2)
    $dy = [int](($side - $image.Height) / 2)
    $g.DrawImage($image, $dx, $dy, $image.Width, $image.Height)
    $g.Dispose()
    return $canvas
}

# --- render one size from a square source ------------------------------------
function New-SizedBitmap([System.Drawing.Image]$square, [int]$size) {
    $bmp = New-Object System.Drawing.Bitmap($size, $size,
        [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.Clear([System.Drawing.Color]::Transparent)
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.DrawImage($square, 0, 0, $size, $size)
    $g.Dispose()
    return $bmp
}

# --- DIB entry: BITMAPINFOHEADER + bottom-up BGRA + 1bpp AND mask ------------
function ConvertTo-DibEntry([System.Drawing.Bitmap]$bitmap) {
    $size = $bitmap.Width
    $ms = New-Object System.IO.MemoryStream
    $bw = New-Object System.IO.BinaryWriter($ms)
    $maskRow = [int]([Math]::Ceiling($size / 32.0) * 4)
    $bw.Write([UInt32]40)                        # biSize
    $bw.Write([Int32]$size)                      # biWidth
    $bw.Write([Int32]($size * 2))                # biHeight (XOR + AND)
    $bw.Write([UInt16]1)                         # biPlanes
    $bw.Write([UInt16]32)                        # biBitCount
    $bw.Write([UInt32]0)                         # BI_RGB
    $bw.Write([UInt32]($size * $size * 4 + $maskRow * $size))
    $bw.Write([Int32]0); $bw.Write([Int32]0)
    $bw.Write([UInt32]0); $bw.Write([UInt32]0)
    for ($y = $size - 1; $y -ge 0; $y--) {
        for ($x = 0; $x -lt $size; $x++) {
            $px = $bitmap.GetPixel($x, $y)
            $bw.Write([byte]$px.B); $bw.Write([byte]$px.G)
            $bw.Write([byte]$px.R); $bw.Write([byte]$px.A)
        }
    }
    for ($y = $size - 1; $y -ge 0; $y--) {
        $row = New-Object 'byte[]' $maskRow
        for ($x = 0; $x -lt $size; $x++) {
            if ($bitmap.GetPixel($x, $y).A -lt 128) {
                $idx = [int][Math]::Floor($x / 8)
                $row[$idx] = $row[$idx] -bor [byte](0x80 -shr ($x % 8))
            }
        }
        $bw.Write($row)
    }
    $bw.Flush()
    $bytes = $ms.ToArray()
    $bw.Dispose(); $ms.Dispose()
    return $bytes
}

# --- write a multi-size .ico ------------------------------------------------
function Write-Ico([System.Drawing.Image]$square, [int[]]$sizes, [string]$path) {
    $entries = @()
    foreach ($s in $sizes) {
        $bmp = New-SizedBitmap $square $s
        if ($s -lt 256) {
            $data = ConvertTo-DibEntry $bmp
        } else {
            $ms = New-Object System.IO.MemoryStream
            $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
            $data = $ms.ToArray()
            $ms.Dispose()
        }
        $entries += , @($s, $data)
        $bmp.Dispose()
    }

    $buffer = New-Object System.IO.MemoryStream
    $writer = New-Object System.IO.BinaryWriter($buffer)
    $writer.Write([UInt16]0)                     # reserved
    $writer.Write([UInt16]1)                     # type: icon
    $writer.Write([UInt16]$entries.Count)
    $offset = 6 + 16 * $entries.Count
    foreach ($entry in $entries) {
        $size = [int]$entry[0]
        $data = [byte[]]$entry[1]
        $dim = [byte]$(if ($size -ge 256) { 0 } else { $size })   # 0 means 256
        $writer.Write($dim); $writer.Write($dim)
        $writer.Write([byte]0); $writer.Write([byte]0)            # palette, reserved
        $writer.Write([UInt16]1); $writer.Write([UInt16]32)       # planes, bpp
        $writer.Write([UInt32]$data.Length)
        $writer.Write([UInt32]$offset)
        $offset += $data.Length
    }
    foreach ($entry in $entries) { $writer.Write([byte[]]$entry[1]) }
    $writer.Flush()

    [System.IO.File]::WriteAllBytes($path, $buffer.ToArray())
    $writer.Dispose(); $buffer.Dispose()
    Write-Host ("ico    : {0} ({1} bytes, {2} sizes: {3})" -f (Split-Path $path -Leaf),
        (Get-Item $path).Length, $entries.Count, ($sizes -join ', '))
}

# --- build the two sources ---------------------------------------------------
$keyed = ConvertTo-TransparentBackground $logo
$bounds = Get-MarkBounds $logo
$mark = $keyed.Clone([System.Drawing.Rectangle]$bounds, $keyed.PixelFormat)
Write-Host ("mark   : {0}x{1} at {2},{3}" -f $bounds.Width, $bounds.Height, $bounds.X, $bounds.Y)

# The mark gets a little breathing room; the full logo gets almost none so the
# word stays as large as possible.
$markSquare = New-SquareCanvas $mark 0.06
$fullSquare = New-SquareCanvas $keyed 0.02
$mark.Dispose(); $keyed.Dispose(); $logo.Dispose()

if (-not (Test-Path $AssetDir)) { New-Item -ItemType Directory -Path $AssetDir -Force | Out-Null }
$pngDir = Join-Path $AssetDir 'png'
if (-not (Test-Path $pngDir)) { New-Item -ItemType Directory -Path $pngDir -Force | Out-Null }

Write-Host ''
Write-Ico $markSquare $IcoSizes (Join-Path $AssetDir 'openaes67.ico')
Write-Ico $fullSquare $IcoSizes (Join-Path $AssetDir 'openaes67-full.ico')

foreach ($s in $PngSizes) {
    $bmp = New-SizedBitmap $markSquare $s
    $bmp.Save((Join-Path $pngDir ("openaes67-{0}.png" -f $s)), [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()

    $bmp = New-SizedBitmap $fullSquare $s
    $bmp.Save((Join-Path $pngDir ("openaes67-full-{0}.png" -f $s)), [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
}
Write-Host ("png    : {0} files in {1}" -f (2 * $PngSizes.Count), $pngDir)

$markSquare.Dispose(); $fullSquare.Dispose()

$check = New-Object System.Drawing.Icon((Join-Path $AssetDir 'openaes67.ico'))
Write-Host ("done   : app icon loads as {0}x{1}" -f $check.Width, $check.Height)
$check.Dispose()
