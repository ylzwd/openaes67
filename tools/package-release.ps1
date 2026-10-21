#requires -Version 5.1
<#
.SYNOPSIS
    Builds the release archive for OpenAES67.

.DESCRIPTION
    Collects the CLI, the self-test binary, documentation and the Python tools
    into dist\openaes67-<version>-win64.zip and prints its SHA-256.

    The GUI is only included with -WithGui, because it needs the Qt 6 runtime
    DLLs (about 60 MB) and most users of the command-line path do not want them.

.PARAMETER Version
    Version string used in the archive name. Defaults to the value in
    CMakeLists.txt, or 0.5.0 when it cannot be read.

.PARAMETER WithGui
    Also package openaes67_gui.exe together with its Qt runtime.

.PARAMETER SkipBuild
    Package whatever is already in build\ and build-qt\ without rebuilding.
#>
[CmdletBinding()]
param(
    [string]$Version,
    [switch]$WithGui,
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Push-Location $root
try {
    if (-not $Version) {
        $cmakeLists = Join-Path $root 'CMakeLists.txt'
        if (Test-Path $cmakeLists) {
            $m = Select-String -Path $cmakeLists -Pattern 'VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)' |
                 Select-Object -First 1
            if ($m) { $Version = $m.Matches[0].Groups[1].Value }
        }
        if (-not $Version) { $Version = '0.5.0' }
    }
    Write-Host "packaging OpenAES67 $Version"

    if (-not $SkipBuild) {
        Write-Host 'building (build.bat)'
        & cmd /c (Join-Path $root 'build.bat') | Out-Null
        if ($LASTEXITCODE -ne 0) { throw 'build.bat failed' }
    }

    $cli = Join-Path $root 'build\openaes67.exe'
    $selfTest = Join-Path $root 'build\selftest.exe'
    foreach ($f in $cli, $selfTest) {
        if (-not (Test-Path $f)) { throw "missing build output: $f" }
    }

    # Refuse to package a binary that does not pass its own tests.
    Write-Host 'running self-test'
    $out = & $selfTest 2>&1
    if (-not ($out | Select-String 'ALL PASS')) {
        $out | ForEach-Object { Write-Host "  $_" }
        throw 'self-test did not report ALL PASS'
    }

    $name = "openaes67-$Version-win64"
    $dist = Join-Path $root 'dist'
    $stage = Join-Path $dist $name
    if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
    New-Item -ItemType Directory -Path $stage -Force | Out-Null

    Copy-Item $cli      (Join-Path $stage 'openaes67.exe')
    Copy-Item $selfTest (Join-Path $stage 'openaes67_selftest.exe')
    $logo = Join-Path $root 'logo.png'
    if (Test-Path $logo) { Copy-Item $logo (Join-Path $stage 'logo.png') }
    # Icon sources: the .ico is generated from logo.png by tools\make-icon.ps1.
    $assets = Join-Path $root 'assets'
    if (Test-Path $assets) {
        $assetStage = Join-Path $stage 'assets'
        New-Item -ItemType Directory -Path $assetStage -Force | Out-Null
        # Recursive: assets\png holds the per-size PNG exports. Files starting
        # with underscore are scratch output from make-icon.ps1.
        Get-ChildItem $assets -Recurse -File | Where-Object { $_.Name -notlike "_*" } |
            ForEach-Object {
                $rel = $_.FullName.Substring($assets.Length).TrimStart('\')
                $dst = Join-Path $assetStage $rel
                $dstDir = Split-Path $dst -Parent
                if (-not (Test-Path $dstDir)) { New-Item -ItemType Directory -Path $dstDir -Force | Out-Null }
                Copy-Item $_.FullName $dst
            }
    }
    $mk = Join-Path $root 'tools\make-icon.ps1'
    if (Test-Path $mk) {
        # The stage\tools directory is created later for the Python tools, so make
        # sure it exists before copying into it.
        $toolStage = Join-Path $stage 'tools'
        if (-not (Test-Path $toolStage)) { New-Item -ItemType Directory -Path $toolStage -Force | Out-Null }
        Copy-Item $mk (Join-Path $toolStage 'make-icon.ps1')
    }
    foreach ($doc in 'README.md', 'README.zh-CN.md', 'LICENSE', 'CONTRIBUTING.md') {
        $src = Join-Path $root $doc
        if (Test-Path $src) { Copy-Item $src (Join-Path $stage $doc) }
    }

    $docsOut = Join-Path $stage 'docs'
    New-Item -ItemType Directory -Path $docsOut -Force | Out-Null
    Copy-Item (Join-Path $root 'docs\FINDINGS.md') $docsOut
    Get-ChildItem (Join-Path $root 'docs') -Filter *.png | ForEach-Object {
        Copy-Item $_.FullName $docsOut
    }

    $toolsOut = Join-Path $stage 'tools'
    New-Item -ItemType Directory -Path $toolsOut -Force | Out-Null
    Get-ChildItem (Join-Path $root 'tools') -Filter *.py | ForEach-Object {
        Copy-Item $_.FullName $toolsOut
    }

    # Driver sources always ship: they are part of the project even though the
    # binary needs the WDK and a Secure Boot decision to load.
    $drvOut = Join-Path $stage 'driver'
    New-Item -ItemType Directory -Path $drvOut -Force | Out-Null
    $drvSrc = Join-Path $root 'driver'
    foreach ($f in 'openaes67_audio.cpp', 'openaes67_audio.inf', 'openaes67_probe.c',
                   'openaes67_probe.inf', 'build-driver.bat', 'README.md') {
        $src = Join-Path $drvSrc $f
        if (Test-Path $src) { Copy-Item $src $drvOut }
    }
    # Prebuilt signed package, when the machine that packaged it had the WDK.
    $drvPkg = Join-Path $root 'driver\out\audio'
    if (Test-Path $drvPkg) {
        $pkgDst = Join-Path $drvOut 'prebuilt'
        New-Item -ItemType Directory -Path $pkgDst -Force | Out-Null
        Get-ChildItem $drvPkg -File | ForEach-Object { Copy-Item $_.FullName $pkgDst }
    }

    if ($WithGui) {
        $guiDir = Join-Path $root 'build-qt\Release'
        $gui = Join-Path $guiDir 'openaes67_gui.exe'
        if (Test-Path $gui) {
            Write-Host 'including GUI and Qt runtime'
            Copy-Item $gui (Join-Path $stage 'openaes67_gui.exe')
            $qtDlls = 'Qt6Core.dll', 'Qt6Gui.dll', 'Qt6Widgets.dll', 'Qt6Network.dll'
            foreach ($dll in $qtDlls) {
                $p = Join-Path $guiDir $dll
                if (Test-Path $p) { Copy-Item $p $stage }
            }
            $platforms = Join-Path $guiDir 'platforms'
            if (Test-Path $platforms) {
                Copy-Item $platforms (Join-Path $stage 'platforms') -Recurse
            }
        } else {
            Write-Warning "GUI not found at $gui; packaging without it"
        }
    }

    $zip = Join-Path $dist "$name.zip"
    if (Test-Path $zip) { Remove-Item $zip -Force }
    Compress-Archive -Path $stage -DestinationPath $zip -CompressionLevel Optimal

    $hash = (Get-FileHash $zip -Algorithm SHA256).Hash
    $size = '{0:N0}' -f (Get-Item $zip).Length
    Write-Host ''
    Write-Host "archive : $zip"
    Write-Host "size    : $size bytes"
    Write-Host "sha256  : $hash"
    Write-Host ''
    Write-Host 'contents:'
    Get-ChildItem $stage -Recurse -File | ForEach-Object {
        $rel = $_.FullName.Substring($stage.Length + 1)
        Write-Host ("  {0,-42} {1,10:N0}" -f $rel, $_.Length)
    }
}
finally {
    Pop-Location
}
