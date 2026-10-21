#requires -Version 5.1
<#
.SYNOPSIS
    Initialises the git repository and creates the first commit.

.DESCRIPTION
    Run this once on a machine that has git installed. The repository content is
    already release-ready: LICENSE, README (English + Chinese), CONTRIBUTING,
    .gitattributes, .gitignore and the CI workflow are all in place.

    The script refuses to run when git is missing, when a repository already
    exists, or when the working tree contains build artefacts.
#>
[CmdletBinding()]
param(
    [string]$Branch = 'main',
    [string]$Message = 'OpenAES67 0.1.0: AES67 send/receive, PTP sync, rate control, GUI'
)

# 'Continue', not 'Stop': git writes routine warnings to stderr and PowerShell
# 5.1 turns those into terminating errors under 'Stop'. Every failure path below
# checks $LASTEXITCODE and throws explicitly instead.
$ErrorActionPreference = 'Continue'

$root = Split-Path -Parent $PSScriptRoot
Push-Location $root
try {
    # Accept git from PATH, or a portable extraction (this machine has no
    # system-wide git: winget could not reach github.com, so a portable copy was
    # on PATH, or from a portable unpack (for example under %LOCALAPPDATA%).
    $git = Get-Command git -ErrorAction SilentlyContinue
    if (-not $git) {
        foreach ($candidate in 'git.exe',
                               "$env:ProgramFiles\Git\cmd\git.exe") {
            if (Test-Path $candidate) {
                $env:Path = (Split-Path $candidate -Parent) + ';' + $env:Path
                $git = Get-Command git -ErrorAction SilentlyContinue
                break
            }
        }
    }
    if (-not $git) {
        throw 'git is not installed; install it first (winget install Git.Git), or put PortableGit''s cmd directory on PATH'
    }
    Write-Host "using $($git.Source)"
    if (Test-Path (Join-Path $root '.git')) {
        throw 'a git repository already exists here'
    }

    # Build output must not end up in the first commit.
    foreach ($dir in 'build', 'build-qt', 'dist') {
        if (Test-Path (Join-Path $root $dir)) {
            Write-Host "note: $dir\ exists and is ignored by .gitignore"
        }
    }

    # Git is invoked through cmd /c on purpose. PowerShell 5.1 turns a native
    # program's stderr output into a terminating error when the preference is
    # Stop, and git writes routine warnings there ("CRLF will be replaced by
    # LF") thanks to .gitattributes. Routing through cmd keeps warnings as
    # warnings and lets $LASTEXITCODE decide success.
    function Invoke-Git([string]$arguments) {
        $line = 'git ' + $arguments
        & cmd /c $line 2>&1 | ForEach-Object { Write-Host "  $_" }
        if ($LASTEXITCODE -ne 0) { throw "git failed: $line" }
    }

    Invoke-Git "init -b $Branch"
    Invoke-Git 'add -A'

    $staged = & cmd /c 'git diff --cached --name-only' 2>&1
    $bad = $staged | Where-Object { $_ -match '^(build|build-qt|dist)/' }
    if ($bad) {
        throw "build artefacts were staged: $($bad -join ', ')"
    }

    Invoke-Git ('-c user.name=OpenAES67 -c user.email=noreply@example.invalid commit -m "' + $Message + '"')

    Write-Host ''
    Write-Host "repository initialised on branch '$Branch'"
    & cmd /c "git log --oneline -1" 2>&1 | Write-Host
    Write-Host ''
    Write-Host 'next steps:'
    Write-Host '  git remote add origin <url>'
    Write-Host '  git push -u origin main'
}
finally {
    Pop-Location
}
