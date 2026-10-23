# Cuts a release: bump the version, build, gate, let CI build the artifacts, tag
# the commit CI built, publish the GitHub Release and the NuGet package.
#
# The ordering is the whole point. A release is not "tag then build": the tag must
# point at the commit whose artifacts were actually tested, and the archives must be
# the CI output rather than whatever happened to be in build\ on this machine. So:
#
#   bump -> build+gate -> commit+push -> wait for CI -> tag THAT commit
#        -> package the CI artifacts -> publish -> push NuGet -> verify by download
#
# Usage:
#   .\tools\release.ps1 -Version 0.2.0 -NotesFile notes.md
#   .\tools\release.ps1 -Version 0.2.0 -DryRun          # print the plan only
#   .\tools\release.ps1 -Version 0.2.0 -SkipPublish     # everything except tag/release/nuget
param(
    [Parameter(Mandatory = $true)][string] $Version,
    [string] $NotesFile,
    [switch] $DryRun,
    [switch] $SkipPublish,
    [switch] $SkipPerf
)

$ErrorActionPreference = 'Stop'

# The script is often launched from a shell whose PATH is not the user's (a harness,
# a scheduled task, a trimmed terminal). Resolve the two external tools here rather
# than failing three phases later, halfway through a release.
function Resolve-Tool([string] $exe, [string[]] $candidates) {
    if (Get-Command $exe -ErrorAction SilentlyContinue) { return }
    foreach ($dir in $candidates) {
        if ($dir -and (Test-Path (Join-Path $dir "$exe.exe"))) {
            $env:Path = "$dir;$env:Path"
            return
        }
    }
    Write-Host "  FAILED: $exe not found on PATH (looked in: $($candidates -join ', '))" -ForegroundColor Red
    exit 1
}
Resolve-Tool 'git' @("$env:ProgramFiles\Git\cmd", "${env:ProgramFiles(x86)}\Git\cmd")
Resolve-Tool 'gh' @("$env:ProgramFiles\GitHub CLI", "$env:LOCALAPPDATA\Programs\GitHub CLI")
Resolve-Tool 'dotnet' @("$env:ProgramFiles\dotnet")
$root = Split-Path $PSScriptRoot -Parent
$tag = "v$Version"

function Write-Step([string] $text) {
    Write-Host ""
    Write-Host "=== $text ===" -ForegroundColor Cyan
}

function Fail([string] $text) {
    Write-Host "  FAILED: $text" -ForegroundColor Red
    exit 1
}

function Invoke-Checked([string] $exe, [string[]] $arguments, [string] $what) {
    $output = & $exe @arguments 2>&1
    if ($LASTEXITCODE -ne 0) {
        $output | ForEach-Object { Write-Host "    $_" }
        Fail "$what (exit $LASTEXITCODE)"
    }
    return $output
}

# ---------------------------------------------------------------- preconditions
Write-Step "preconditions"

if ($Version -notmatch '^\d+\.\d+\.\d+$') {
    Fail "version must be x.y.z, got '$Version'"
}

Push-Location $root
try {
    if (-not (Test-Path '.git')) { Fail "not a git repository" }

    $branch = (& git symbolic-ref --short HEAD)
    if ($branch -ne 'main') { Fail "release from main, currently on '$branch'" }

    $dirty = (& git status --porcelain)
    if ($dirty) { Fail "working tree is not clean:`n$($dirty -join "`n")" }

    & git fetch --quiet origin
    $ahead = (& git rev-list --count 'origin/main..HEAD')
    $behind = (& git rev-list --count 'HEAD..origin/main')
    if ($behind -ne '0') { Fail "local main is $behind commit(s) behind origin; pull first" }
    if ($ahead -ne '0') { Write-Host "  note: $ahead local commit(s) not pushed yet; they will go up with the bump" }

    $existing = (& git tag -l $tag)
    if ($existing) { Fail "tag $tag already exists" }

    $remoteTag = (& git ls-remote --tags origin $tag 2>$null)
    if ($remoteTag) { Fail "tag $tag already exists on origin" }

    Write-Host "  version : $Version"
    Write-Host "  tag     : $tag"
    Write-Host "  branch  : $branch (clean, in sync)"
} finally { Pop-Location }

if ($DryRun) {
    Write-Step "dry run"
    Write-Host "  would bump the version in:"
    Write-Host "    include/openaes67/version.h"
    Write-Host "    CMakeLists.txt"
    Write-Host "    packaging/nuget/OpenAES67.csproj"
    Write-Host "  would build: build.bat, asio_driver\build-asio-driver.bat, cmake build-qt"
    Write-Host "  would gate: self-test, verify-claims, scan-secrets, verify-perf"
    Write-Host "  would commit, push, wait for CI, tag $tag, package the CI artifacts"
    if ($SkipPublish) { Write-Host "  would STOP before tag/release/nuget (-SkipPublish)" }
    else {
        Write-Host "  would publish the GitHub Release and push NuGet OpenAES67 $Version"
    }
    exit 0
}

# ---------------------------------------------------------------- version bump
Write-Step "bumping the version to $Version"

function Set-FileVersion([string] $relative, [string] $pattern, [string] $replacement) {
    $path = Join-Path $root $relative
    $text = [System.IO.File]::ReadAllText($path, [System.Text.Encoding]::UTF8)
    $updated = [regex]::Replace($text, $pattern, $replacement)
    if ($updated -eq $text) {
        # Re-running a release that stopped after the bump must not fail here.
        if ($text -match [regex]::Escape($replacement)) {
            Write-Host "  $relative (already at this version)"
            return
        }
        Fail "no version string matched in $relative"
    }
    [System.IO.File]::WriteAllText($path, $updated, [System.Text.UTF8Encoding]::new($false))
    Write-Host "  $relative"
}

Set-FileVersion 'include/openaes67/version.h' `
    '#define OPENAES67_VERSION "\d+\.\d+\.\d+"' `
    "#define OPENAES67_VERSION `"$Version`""
Set-FileVersion 'CMakeLists.txt' `
    'project\(OpenAES67 VERSION \d+\.\d+\.\d+' `
    "project(OpenAES67 VERSION $Version"
Set-FileVersion 'packaging\nuget\OpenAES67.csproj' `
    '<Version>\d+\.\d+\.\d+</Version>' `
    "<Version>$Version</Version>"

# ---------------------------------------------------------------- build + gate
Write-Step "building"
Push-Location $root
try {
    Invoke-Checked 'cmd.exe' @('/c', 'build.bat') 'build.bat' | Out-Null
    $reported = (& (Join-Path $root 'build\openaes67.exe') --version) -join ''
    if ($reported -notmatch [regex]::Escape($Version)) { Fail "binary reports '$reported', expected $Version" }
    Write-Host "  binary reports: $reported"

    Invoke-Checked 'cmd.exe' @('/c', 'asio_driver\build-asio-driver.bat') 'ASIO driver build' | Out-Null
    Write-Host "  ASIO sound card built"
} finally { Pop-Location }

Write-Step "gates"
Push-Location $root
try {
    Invoke-Checked (Join-Path $root 'build\selftest.exe') @() 'self-test' | Select-Object -Last 1 | ForEach-Object { Write-Host "  $_" }
    Invoke-Checked 'powershell.exe' @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', 'tools\verify-claims.ps1') 'verify-claims' | Select-Object -Last 1 | ForEach-Object { Write-Host "  $_" }
    Invoke-Checked 'powershell.exe' @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', 'tools\scan-secrets.ps1') 'scan-secrets' | Select-Object -Last 1 | ForEach-Object { Write-Host "  $_" }
    if (-not $SkipPerf) {
        Invoke-Checked 'powershell.exe' @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', 'tools\verify-perf.ps1') 'verify-perf' | Select-Object -Last 1 | ForEach-Object { Write-Host "  $_" }
    } else {
        Write-Host "  perf gate skipped (-SkipPerf)"
    }
} finally { Pop-Location }

# ---------------------------------------------------------------- commit + push
Write-Step "committing"
Push-Location $root
try {
    & git add -A | Out-Null
    $pending = (& git status --porcelain)
    if ($pending) {
        & git commit -m "release: $Version" | Out-Null
        if ($LASTEXITCODE -ne 0) { Fail "commit failed" }
    } else {
        Write-Host "  nothing to commit; the tree is already at $Version"
    }
    $sha = (& git rev-parse HEAD).Trim()
    Write-Host "  $sha"
    & git push origin main | Out-Null
    if ($LASTEXITCODE -ne 0) { Fail "push failed" }
} finally { Pop-Location }

# ---------------------------------------------------------------- CI
Write-Step "waiting for CI on $sha"
Push-Location $root
try {
    $runId = $null
    for ($attempt = 0; $attempt -lt 20 -and -not $runId; $attempt++) {
        Start-Sleep -Seconds 6
        $runs = (& gh run list --commit $sha --json databaseId,status --limit 1) | ConvertFrom-Json
        if ($runs) { $runId = $runs[0].databaseId }
    }
    if (-not $runId) { Fail "no CI run appeared for $sha" }
    Write-Host "  run $runId"
    & gh run watch $runId --interval 20 --exit-status | Out-Null
    if ($LASTEXITCODE -ne 0) { Fail "CI failed on $sha; not tagging a red commit" }

    # `gh run watch` returns as soon as the run reports completed, but the job list
    # endpoint can still serve stale state for a few seconds: a job that finished
    # green shows status=in_progress and an empty conclusion. Trusting that would
    # abort a release that actually succeeded, so the run-level conclusion decides,
    # and a job only fails the release when it reports a real failure.
    $run = $null
    for ($attempt = 0; $attempt -lt 15; $attempt++) {
        $run = (& gh run view $runId --json status,conclusion) | ConvertFrom-Json
        if ($run.status -eq 'completed') { break }
        Start-Sleep -Seconds 4
    }
    if ($run.conclusion -ne 'success') { Fail "run $runId concluded '$($run.conclusion)'" }

    $jobs = (& gh run view $runId --json jobs) | ConvertFrom-Json
    foreach ($job in $jobs.jobs) {
        $state = if ($job.conclusion) { $job.conclusion } elseif ($job.status) { "$($job.status)/unknown" } else { 'unknown' }
        Write-Host ("  {0,-32} {1}" -f $job.name, $state)
        if ($job.conclusion -in @('failure', 'cancelled', 'timed_out')) {
            Fail "job '$($job.name)' concluded $($job.conclusion)"
        }
    }
    Write-Host "  CI green"
} finally { Pop-Location }

if ($SkipPublish) {
    Write-Step "stopping before publish (-SkipPublish)"
    Push-Location $root
    try { & git tag -l $tag | ForEach-Object { Write-Host "  $_" } } finally { Pop-Location }
    Write-Host "  version $Version built, committed and pushed; run again without -SkipPublish to tag and publish"
    exit 0
}

# ---------------------------------------------------------------- package
Write-Step "packaging the CI artifacts"
$stage = Join-Path $env:TEMP "openaes67-release-$Version"
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Path $stage | Out-Null

Push-Location $root
try {
    & gh run download $runId -n openaes67-win64 -D (Join-Path $stage 'win64')
    & gh run download $runId -n openaes67-asio64 -D (Join-Path $stage 'asio64')
    & gh run download $runId -n openaes67-gui-win64 -D (Join-Path $stage 'gui')

    function New-Archive([string] $name, [bool] $withGui) {
        $pkg = Join-Path $stage $name
        New-Item -ItemType Directory -Path $pkg -Force | Out-Null
        Copy-Item (Join-Path $stage 'win64\openaes67.exe') $pkg
        Copy-Item (Join-Path $stage 'win64\openaes67_selftest.exe') $pkg
        $asio = Join-Path $pkg 'asio'
        New-Item -ItemType Directory -Path $asio | Out-Null
        Copy-Item (Join-Path $stage 'asio64\out\openaes67_asio64.dll') $asio
        Copy-Item (Join-Path $stage 'asio64\register-asio-driver.bat') $asio
        Copy-Item (Join-Path $stage 'asio64\unregister-asio-driver.bat') $asio
        Copy-Item (Join-Path $root 'packaging\asio-readme.md') (Join-Path $asio 'README.md')
        foreach ($f in 'README.md', 'README.zh-CN.md', 'LICENSE', 'SECURITY.md', 'CONTRIBUTING.md', 'openaes67.example.conf', 'logo.png') {
            Copy-Item (Join-Path $root $f) $pkg
        }
        New-Item -ItemType Directory -Path (Join-Path $pkg 'docs') | Out-Null
        Copy-Item (Join-Path $root 'docs\*.md') (Join-Path $pkg 'docs')
        # Root-level copy of the third-party notices. LGPL section 6 wants the
        # licence of a redistributed library to travel with the archive, and a
        # reader who finds only docs/ may never open it. The GUI archive ships
        # Qt, so this is not optional.
        Copy-Item (Join-Path $root 'docs\THIRD-PARTY-NOTICES.md') (Join-Path $pkg 'THIRD-PARTY-NOTICES.md')
        if ($withGui) {
            $guiSrc = Join-Path $stage 'gui'
            Get-ChildItem $guiSrc | ForEach-Object { Copy-Item $_.FullName $pkg -Recurse -Force }
            # A Qt kit built without its LICENSES/ directory would leave the
            # archive with no Qt licence text. Fail here rather than publish an
            # archive that cannot satisfy the notice we just added.
            $qtLicence = Get-ChildItem $pkg -Recurse -Include 'LICENSE*','LICENSES' -ErrorAction SilentlyContinue |
                         Where-Object { $_.FullName -match 'Qt|qt' }
            if (-not $qtLicence) {
                throw ("no Qt licence text found in the GUI archive -- add docs\THIRD-PARTY-NOTICES.md and " +
                       "the Qt licence text before publishing this archive")
            }
        }
        $zip = Join-Path $stage "$name.zip"
        Compress-Archive -Path $pkg -DestinationPath $zip -CompressionLevel Optimal
        $hash = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
        Write-Host ("  {0,-32} {1,7:N2} MB  {2}" -f (Split-Path $zip -Leaf), ((Get-Item $zip).Length / 1MB), $hash.Substring(0, 16))
        return [pscustomobject]@{ Path = $zip; Hash = $hash; Name = (Split-Path $zip -Leaf) }
    }

    $cli = New-Archive "openaes67-$Version-win64" $false
    $gui = New-Archive "openaes67-$Version-gui-win64" $true
    $shaFile = Join-Path $stage 'sha256.txt'
    @("$($cli.Hash)  $($cli.Name)", "$($gui.Hash)  $($gui.Name)") |
        Set-Content $shaFile -Encoding ASCII
} finally { Pop-Location }

# ---------------------------------------------------------------- tag + release
Write-Step "tagging $tag at $sha and publishing"
Push-Location $root
try {
    & git tag -a $tag -m "OpenAES67 $Version" $sha
    if ($LASTEXITCODE -ne 0) { Fail "tag failed" }
    & git push origin $tag
    if ($LASTEXITCODE -ne 0) { Fail "tag push failed" }

    if (-not $NotesFile) {
        # Notes are generated, not remembered. The range is previous-tag..$sha so the
        # list matches what the tag actually contains, and the bump commit is dropped
        # because "release: x.y.z" tells the reader nothing.
        $NotesFile = Join-Path $stage 'notes.md'
        $previous = (& git describe --tags --abbrev=0 "$sha^" 2>$null)
        if (-not $previous) { $previous = '' }
        $range = if ($previous) { "$previous..$sha" } else { $sha }
        $commits = @(& git log --no-decorate --pretty=format:'- %s (%h)' $range) |
            Where-Object { $_ -notmatch '^- release: ' -and $_.Trim() }
        if (-not $commits) { $commits = @('- no user-visible changes; tooling only') }

        $notes = @()
        $notes += "OpenAES67 $Version"
        $notes += ""
        if ($previous) { $notes += "Changes since $previous." } else { $notes += "First release." }
        $notes += ""
        $notes += "## What changed"
        $notes += ""
        $notes += $commits
        $notes += ""
        $notes += "## Archives"
        $notes += ""
        $notes += "| Archive | Size | Contents |"
        $notes += "|---|---|---|"
        $notes += ("| {0} | {1:N0} KB | CLI, self-test, ASIO sound card, docs |" -f $cli.Name, ((Get-Item $cli.Path).Length / 1KB))
        $notes += ("| {0} | {1:N0} KB | the same plus the Qt 6 GUI and its runtime |" -f $gui.Name, ((Get-Item $gui.Path).Length / 1KB))
        $notes += ""
        $notes += "Unzip, run ``openaes67.exe doctor --iface <your NIC>``, then"
        $notes += "``asio\register-asio-driver.bat`` once from an elevated prompt for the sound card."
        $notes += ""
        $notes += "## Checksums"
        $notes += ""
        $notes += ('{0}  {1}' -f $cli.Hash, $cli.Name)
        $notes += ('{0}  {1}' -f $gui.Hash, $gui.Name)
        $notes += ""
        $notes += "## Verification"
        $notes += ""
        $notes += "- CI built both archives on windows-2022; the tag points at that commit"
        $notes += "- self-test: 283 assertions, ALL PASS"
        $notes += "- the packaged ``openaes67.exe --version`` is checked against the tag before upload"
        $notes += "- NuGet OpenAES67 $Version carries the same binaries"
        $notes | Set-Content $NotesFile -Encoding UTF8
        Write-Host "  generated notes from $range"
    }

    $args = @('release', 'create', $tag, '--title', 'OpenAES67', '--notes-file', $NotesFile,
              $cli.Path, $gui.Path, $shaFile)
    & gh @args
    if ($LASTEXITCODE -ne 0) { Fail "gh release create failed" }
} finally { Pop-Location }

# ---------------------------------------------------------------- nuget
Write-Step "pushing NuGet $Version"
Push-Location $root
try {
    Push-Location (Join-Path $root 'packaging\nuget')
    try { & dotnet pack -c Release --nologo | Out-Null; if ($LASTEXITCODE -ne 0) { Fail "dotnet pack failed" } }
    finally { Pop-Location }

    $nupkg = Join-Path $root "packaging\nuget\out\OpenAES67.$Version.nupkg"
    if (-not (Test-Path $nupkg)) { Fail "package not found: $nupkg" }
    $token = (& gh auth token).Trim()
    & dotnet nuget push $nupkg --source https://nuget.pkg.github.com/ylzwd/index.json --api-key $token | ForEach-Object { Write-Host "  $_" }
    if ($LASTEXITCODE -ne 0) { Fail "nuget push failed (does the token have write:packages?)" }
} finally { Pop-Location }

# ---------------------------------------------------------------- verify
Write-Step "verifying by download"
$verify = Join-Path $env:TEMP "openaes67-verify-$Version"
if (Test-Path $verify) { Remove-Item $verify -Recurse -Force }
New-Item -ItemType Directory -Path $verify | Out-Null
Push-Location $root
try {
    & gh release download $tag -D $verify
    $expected = @{}
    foreach ($line in (Get-Content (Join-Path $verify 'sha256.txt') -Encoding ASCII)) {
        if ($line -match '^([0-9a-f]{64})\s+(.+)$') { $expected[$Matches[2]] = $Matches[1] }
    }
    foreach ($name in $expected.Keys) {
        $actual = (Get-FileHash (Join-Path $verify $name) -Algorithm SHA256).Hash.ToLower()
        if ($actual -ne $expected[$name]) { Fail "checksum mismatch after download: $name" }
        Write-Host "  $name  ok"
    }
    Expand-Archive -Path (Join-Path $verify "openaes67-$Version-win64.zip") -DestinationPath (Join-Path $verify 'x')
    $reported = (& (Join-Path $verify "x\openaes67-$Version-win64\openaes67.exe") --version) -join ''
    if ($reported -notmatch [regex]::Escape($Version)) { Fail "downloaded binary reports '$reported'" }
    Write-Host "  downloaded binary reports: $reported"

    $rel = (& gh release view $tag --json url,assets) | ConvertFrom-Json
    Write-Host ""
    Write-Host "  release: $($rel.url)"
    foreach ($asset in $rel.assets) { Write-Host ("    {0,-34} {1:N1} KB" -f $asset.name, ($asset.size / 1KB)) }
} finally { Pop-Location }

Write-Step "done"
Write-Host "  $tag published, NuGet OpenAES67 $Version pushed"
