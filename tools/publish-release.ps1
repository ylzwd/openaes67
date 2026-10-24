# Assembles the release archives from a CI run and uploads them as a GitHub
# release.
#
# The project already has a release script (tools/release.ps1) that does the
# whole thing: bump, build, gate, wait for CI, tag, package, publish, verify.
# This script is the part of it that can run without the rest -- it takes a CI
# run that has already produced the artifacts, assembles the archives the same
# way release.ps1 does, and creates the release.
#
# The two are kept in step by tools/verify-claims.ps1, which asserts that the
# notice is still copied into every archive and that the GUI archive still
# refuses to be built without the Qt licence text.
#
# Usage:
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\publish-release.ps1 `
#              -Version 0.1.1 -RunId 40063036041
param(
    # The version to publish. Must match the tag that already exists.
    [Parameter(Mandatory = $true)][string]$Version,
    # The CI run whose artifacts to package.
    [Parameter(Mandatory = $true)][long]$RunId,
    # Skip the upload and only produce the archives.
    [switch]$NoUpload
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$stage = Join-Path $env:TEMP "openaes67-publish-$Version"
$proxy = 'http://127.0.0.1:7890'

if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Path $stage | Out-Null

# ---------------------------------------------------------------- credentials
$tokenFile = 'C:\Users\Public\oa67-token.txt'
if (-not (Test-Path $tokenFile)) { throw "no token at $tokenFile" }
$token = [System.IO.File]::ReadAllText($tokenFile).Trim()
$headers = @{
    "Authorization"  = "Bearer $token"
    "Accept"         = "application/vnd.github+json"
    "X-GitHub-Api-Version" = "2022-11-28"
    "User-Agent"     = "openaes67-release"
}
$proxyObj = New-Object System.Net.WebProxy($proxy)
[System.Net.ServicePointManager]::SecurityProtocol = [System.Net.SecurityProtocolType]::Tls12

function Get-Api([string]$uri) {
    Invoke-RestMethod -Uri $uri -Headers $headers -Proxy $proxyObj -Method Get
}

# ---------------------------------------------------------------- the run
Write-Host "=== the CI run ==="
$run = Get-Api "https://api.github.com/repos/ylzwd/openaes67/actions/runs/$RunId"
if ($run.status -ne 'completed') { throw "run $RunId is still $($run.status)" }
if ($run.conclusion -ne 'success') { throw "run $RunId concluded $($run.conclusion)" }
Write-Host "  sha       : $($run.head_sha)"
Write-Host "  concluded : $($run.conclusion)"

# The tag has to point at what was built, or the release would ship artifacts
# from a commit the tag does not name.
$tagSha = (Get-Api "https://api.github.com/repos/ylzwd/openaes67/git/ref/tags/v$Version").object.sha
Write-Host "  tag v$Version: $tagSha"
if ($tagSha -ne $run.head_sha) {
    throw ("the tag v{0} points at {1} but the run built {2} -- move the tag first" -f `
           $Version, $tagSha.Substring(0,7), $run.head_sha.Substring(0,7))
}

# ---------------------------------------------------------------- artifacts
Write-Host ""
Write-Host "=== downloading the artifacts ==="
$artifacts = (Get-Api "https://api.github.com/repos/ylzwd/openaes67/actions/runs/$RunId/artifacts").artifacts
foreach ($a in $artifacts) {
    $out = Join-Path $stage "$($a.name).zip"
    Invoke-WebRequest -Uri "https://api.github.com/repos/ylzwd/openaes67/actions/artifacts/$($a.id)/zip" `
        -Headers $headers -Proxy $proxyObj -OutFile $out -MaximumRedirection 5
    $dest = Join-Path $stage $a.name
    Expand-Archive -Path $out -DestinationPath $dest -Force
    Write-Host ("  {0,-24} {1,12:N0} bytes" -f $a.name, (Get-Item $out).Length)
}

# ---------------------------------------------------------------- package
# The layout matches tools/release.ps1: the CLI and the sound card in one
# archive, the GUI in another, each with the licence and the notices.
function New-Archive([string]$name, [bool]$withGui) {
    $pkg = Join-Path $stage $name
    New-Item -ItemType Directory -Path $pkg -Force | Out-Null

    Copy-Item (Join-Path $stage 'openaes67-win64\openaes67.exe') $pkg
    Copy-Item (Join-Path $stage 'openaes67-win64\openaes67_selftest.exe') $pkg

    $asio = Join-Path $pkg 'asio'
    New-Item -ItemType Directory -Path $asio | Out-Null
    Copy-Item (Join-Path $stage 'openaes67-asio64\out\openaes67_asio64.dll') $asio
    Copy-Item (Join-Path $stage 'openaes67-asio64\register-asio-driver.bat') $asio
    Copy-Item (Join-Path $stage 'openaes67-asio64\unregister-asio-driver.bat') $asio
    Copy-Item (Join-Path $root 'packaging\asio-readme.md') (Join-Path $asio 'README.md')

    foreach ($f in 'README.md', 'README.zh-CN.md', 'LICENSE', 'SECURITY.md',
                   'CONTRIBUTING.md', 'openaes67.example.conf', 'logo.png') {
        Copy-Item (Join-Path $root $f) $pkg
    }
    New-Item -ItemType Directory -Path (Join-Path $pkg 'docs') | Out-Null
    Copy-Item (Join-Path $root 'docs\*.md') (Join-Path $pkg 'docs')
    # Root-level copy: LGPL section 6 wants the licence of a redistributed
    # library to travel with the archive, and a reader who never opens docs/
    # would otherwise miss it.
    Copy-Item (Join-Path $root 'docs\THIRD-PARTY-NOTICES.md') (Join-Path $pkg 'THIRD-PARTY-NOTICES.md')

    if ($withGui) {
        $guiSrc = Join-Path $stage 'openaes67-gui-win64'
        Get-ChildItem $guiSrc | ForEach-Object { Copy-Item $_.FullName $pkg -Recurse -Force }
        $licences = Get-ChildItem $pkg -Recurse -ErrorAction SilentlyContinue |
                    Where-Object { $_.Name -eq 'LICENSES' -and $_.PSIsContainer }
        if (-not $licences) {
            throw ("no LICENSES directory in the GUI archive -- the Qt runtime is LGPL and its " +
                   "licence text must ship with it")
        }
        $text = Get-ChildItem $licences[0].FullName -Recurse -File |
                Where-Object { $_.Name -match 'LGPL|GPL' }
        if (-not $text) { throw "the LICENSES directory holds no licence text" }
        Write-Host "  Qt licence: $($text[0].Name)"
    }

    $zip = Join-Path $stage "$name.zip"
    if (Test-Path $zip) { Remove-Item $zip -Force }
    Compress-Archive -Path $pkg -DestinationPath $zip -CompressionLevel Optimal
    $hash = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
    Write-Host ("  {0,-36} {1,7:N2} MB  {2}" -f (Split-Path $zip -Leaf), ((Get-Item $zip).Length / 1MB), $hash.Substring(0,16))
    return [pscustomobject]@{ Path = $zip; Hash = $hash; Name = (Split-Path $zip -Leaf) }
}

Write-Host ""
Write-Host "=== assembling the archives ==="
$cli = New-Archive "openaes67-$Version-win64" $false
$gui = New-Archive "openaes67-$Version-gui-win64" $true

$shaFile = Join-Path $stage 'sha256.txt'
@(
    "$($cli.Hash)  $($cli.Name)",
    "$($gui.Hash)  $($gui.Name)"
) | Set-Content -Path $shaFile -Encoding UTF8
Write-Host "  sha256.txt written"

if ($NoUpload) {
    Write-Host ""
    Write-Host "  --NoUpload: stopping before the release"
    exit 0
}

# ---------------------------------------------------------------- release
Write-Host ""
Write-Host "=== creating the release ==="
$notes = @"
$((git -C $root log --format='%s' "v0.1.0..v$Version" 2>$null | Select-Object -First 12) -join "`n")

Built by CI from commit $((Get-Api "https://api.github.com/repos/ylzwd/openaes67/actions/runs/$RunId").head_sha.Substring(0,7)).
The archives carry the MSVC runtime and, in the GUI variant, the Qt 6 runtime;
their licences travel with them in LICENSES/ and THIRD-PARTY-NOTICES.md.

Verify a download against sha256.txt before running it.
"@

$body = @{
    tag_name         = "v$Version"
    name             = "OpenAES67 $Version"
    body             = $notes
    draft            = $false
    prerelease       = $false
} | ConvertTo-Json -Depth 4

$release = Invoke-RestMethod -Uri "https://api.github.com/repos/ylzwd/openaes67/releases" `
    -Headers $headers -Proxy $proxyObj -Method Post -Body ([System.Text.Encoding]::UTF8.GetBytes($body)) `
    -ContentType "application/json; charset=utf-8"
Write-Host "  created: $($release.html_url)"

foreach ($asset in @($cli, $gui)) {
    $name = $asset.Name
    $bytes = [System.IO.File]::ReadAllBytes($asset.Path)
    $uploadUri = "https://uploads.github.com/repos/ylzwd/openaes67/releases/$($release.id)/assets?name=$name"
    $up = Invoke-RestMethod -Uri $uploadUri -Headers $headers -Proxy $proxyObj -Method Post `
        -Body $bytes -ContentType "application/octet-stream"
    Write-Host ("  uploaded {0,-36} {1}" -f $up.name, $up.browser_download_url)
}

Write-Host ""
Write-Host "  release: $($release.html_url)"
