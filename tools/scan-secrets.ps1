# Scans the working tree for the six categories that must never ship:
#   credentials, personal data, local absolute paths, build/temp artefacts,
#   test junk, and machine identifiers.
#
# The audit that produced this script found real leaks (a private key, real NIC
# address and MAC, 38 debug logs, 75 unreferenced screenshots), so the checks are
# reproducible rather than remembered. Exit code 0 = clean, 1 = findings.
#
# Rules are deliberately narrow: a scanner that cries wolf gets ignored, so each
# rule matches something that has actually leaked or is unambiguous.
param(
    [string] $Root = (Split-Path $PSScriptRoot -Parent),
    [switch] $Verbose
)

$ErrorActionPreference = 'Stop'
$excludeDirs = @('build', 'build-qt', 'dist', 'out', 'node_modules', '__pycache__', '.git')
$codeExt = @('.cpp', '.h', '.hpp', '.c', '.bat', '.ps1', '.py', '.cmake', '.inf')
$docExt = @('.md', '.txt', '.conf', '.sdp', '.json', '.yml', '.yaml')
$findings = New-Object System.Collections.ArrayList

function Add-Finding([string] $kind, [string] $file, [int] $line, [string] $text) {
    [void]$findings.Add([pscustomobject]@{ Kind = $kind; File = $file; Line = $line; Text = $text })
}

function Get-Files([string[]] $extensions) {
    Get-ChildItem $Root -Recurse -File -Force -ErrorAction SilentlyContinue | Where-Object {
        $rel = $_.FullName.Substring($Root.Length + 1)
        # Any path segment, not just the first: tools/__pycache__ is as much a
        # build artefact as build/ is.
        $parts = $rel.Split('\')
        -not ($parts | Where-Object { $excludeDirs -contains $_ }) -and
        ($extensions -contains $_.Extension.ToLower()) -and
        $_.Length -lt 2MB
    }
}

# Rules. Each one has produced at least one real finding in this repository.
$rules = @(
    @{ Kind = 'credential'; Ext = ($codeExt + $docExt)
       Re = 'BEGIN (RSA |OPENSSH |EC |PGP )?PRIVATE KEY' }
    @{ Kind = 'credential'; Ext = ($codeExt + $docExt)
       Re = '(AKIA[0-9A-Z]{16}|ghp_[A-Za-z0-9]{30,}|sk-[A-Za-z0-9]{20,}|xox[baprs]-[A-Za-z0-9-]{10,})' }
    @{ Kind = 'credential'; Ext = ($codeExt + $docExt)
       Re = '(?i)\b(password|passwd|passphrase|api[_-]?key|access[_-]?key|client[_-]?secret|auth[_-]?token)\b\s*[:=]\s*["'']?[A-Za-z0-9!@#$%^&*_\-\.]{6,}' }
    @{ Kind = 'personal'; Ext = ($codeExt + $docExt)
       Re = 'C:\\Users\\(?!Public\\|Public$|<|%|\.\.\.)[^\\\s"'']+' }
    @{ Kind = 'personal'; Ext = ($codeExt + $docExt)
       Re = '[A-Za-z0-9._%+-]+@(?!example\.(invalid|com))[A-Za-z0-9.-]+\.[A-Za-z]{2,}' }
    @{ Kind = 'personal'; Ext = ($codeExt + $docExt)
       Re = '(?<!\d)1[3-9]\d{9}(?!\d)' }
    @{ Kind = 'path'; Ext = $codeExt
       Re = '"[A-Za-z]:\\\\[^"\r\n]{4,}"' }
    @{ Kind = 'path'; Ext = ($codeExt + $docExt)
       Re = '[A-Za-z]:\\[^\r\n"'']*dante项目' }
    @{ Kind = 'path'; Ext = ($codeExt + $docExt)
       Re = '[A-Za-z]:\\git-portable' }
    @{ Kind = 'path'; Ext = ($codeExt + $docExt)
       Re = '\bDESKTOP-[A-Z0-9]{6,}|\bLAPTOP-[A-Z0-9]{6,}' }
)

foreach ($rule in $rules) {
    foreach ($file in Get-Files $rule.Ext) {
        $rel = $file.FullName.Substring($Root.Length + 1)
        $text = [System.IO.File]::ReadAllText($file.FullName, [System.Text.Encoding]::UTF8)
        foreach ($m in [regex]::Matches($text, $rule.Re)) {
            $line = ($text.Substring(0, $m.Index) -split "`n").Count
            Add-Finding $rule.Kind $rel $line $m.Value
        }
    }
}

# Artefacts: anything that looks like build output or a debug log outside the
# ignored directories, plus signing material and archives that can carry secrets.
$junkExt = @('.tmp', '.bak', '.orig', '.rej', '.swp', '.log', '.err', '.obj', '.pdb', '.ilk', '.pyc', '.pfx', '.cer', '.pvk', '.snk')
foreach ($file in Get-ChildItem $Root -Recurse -File -Force -ErrorAction SilentlyContinue) {
    $rel = $file.FullName.Substring($Root.Length + 1)
    $parts = $rel.Split('\')
    if ($parts | Where-Object { $excludeDirs -contains $_ }) { continue }
    if ($junkExt -contains $file.Extension.ToLower()) {
        Add-Finding 'artefact' $rel 0 $file.Extension
    } elseif ($file.Name -in @('Thumbs.db', 'desktop.ini', '.DS_Store')) {
        Add-Finding 'artefact' $rel 0 $file.Name
    }
}

# A test that writes to a fixed path is a portability defect as well as a leak:
# it passes on the author's machine and fails everywhere else.
foreach ($file in Get-Files @('.cpp', '.h')) {
    if ($file.Name -notmatch 'test|selftest') { continue }
    $rel = $file.FullName.Substring($Root.Length + 1)
    $text = [System.IO.File]::ReadAllText($file.FullName, [System.Text.Encoding]::UTF8)
    foreach ($m in [regex]::Matches($text, '"[A-Za-z]:\\\\[^"]*"')) {
        $line = ($text.Substring(0, $m.Index) -split "`n").Count
        Add-Finding 'test-path' $rel $line $m.Value
    }
}

if ($findings.Count -eq 0) {
    Write-Host '  scan: no credentials, personal data, local paths, artefacts or test junk' -ForegroundColor Green
    exit 0
}

Write-Host "  scan: $($findings.Count) finding(s)" -ForegroundColor Red
foreach ($group in ($findings | Group-Object Kind)) {
    Write-Host "  [$($group.Name)] $($group.Count)" -ForegroundColor Yellow
    $group.Group | Select-Object -First 10 | ForEach-Object {
        $where = if ($_.Line -gt 0) { "$($_.File):$($_.Line)" } else { $_.File }
        $what = if ($_.Text.Length -gt 60) { $_.Text.Substring(0, 60) + '...' } else { $_.Text }
        Write-Host "    $where  $what"
    }
    if ($group.Count -gt 10) {
        Write-Host "    ... $($group.Count - 10) more"
    }
}
exit 1
