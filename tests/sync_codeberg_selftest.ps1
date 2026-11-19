# Exercises scripts\sync_codeberg.ps1 against a throwaway local bare repository.
#
#   powershell -ExecutionPolicy Bypass -File tests\sync_codeberg_selftest.ps1 [-Keep]
#
# The real repository is never touched: a sandbox git repo in %TEMP% gets copies
# of the script and of tools\check_content.py, and a bare repo stands in for
# Codeberg (hence -AnyRemote). Cases: no remote, dry run, host check, content
# guard, dirty tree, first push, idempotent re-run, non-fast-forward refusal.
param([switch]$Keep)
$ErrorActionPreference = "Continue"

$repo = Split-Path -Parent $PSScriptRoot
$tmp = Join-Path ([IO.Path]::GetTempPath()) ("korra-sync-" + [guid]::NewGuid().ToString("N").Substring(0, 8))
$sandbox = Join-Path $tmp "work"
$mirror = Join-Path $tmp "mirror.git"
$other = Join-Path $tmp "other"

$script:failed = 0
$script:passed = 0
function Check([string]$Name, [bool]$Ok, [string]$Detail) {
    if ($Ok) {
        $script:passed++
        Write-Host ("  PASS  " + $Name) -ForegroundColor Green
    } else {
        $script:failed++
        Write-Host ("  FAIL  " + $Name) -ForegroundColor Red
        if ($Detail) { Write-Host ("        " + ($Detail -replace "`r?`n", "`n        ")) }
    }
}
function Run-Sync([string[]]$SyncArgs) {
    $out = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $sandbox "scripts\sync_codeberg.ps1") @SyncArgs 2>&1
    return @{ Code = $LASTEXITCODE; Text = (($out | Out-String).TrimEnd()) }
}
function New-Commit([string]$Dir, [string]$Message) {
    & git -C $Dir add -A 2>&1 | Out-Null
    & git -C $Dir -c user.name=test -c user.email=test@example.invalid commit -q -m $Message 2>&1 | Out-Null
}
function Tip([string]$Dir) { return ("$( (& git -C $Dir rev-parse HEAD) )").Trim() }

Write-Host ("sandbox: " + $tmp)
try {
    New-Item -ItemType Directory -Force -Path (Join-Path $sandbox "scripts"), (Join-Path $sandbox "tools") | Out-Null
    Copy-Item (Join-Path $repo "scripts\sync_codeberg.ps1") (Join-Path $sandbox "scripts\sync_codeberg.ps1")
    Copy-Item (Join-Path $repo "tools\check_content.py") (Join-Path $sandbox "tools\check_content.py")

    # A .gitignore with the same shape as the real one, so the sandbox has a
    # generated tree to hide and the guard has something to compare against.
    @(
        "work/",
        "src/recomp/gen/",
        "game_files/",
        "logs/",
        "build/",
        "*.cpk",
        "*.usm",
        "*.exe"
    ) | Set-Content -Encoding ascii (Join-Path $sandbox ".gitignore")
    "# sandbox" | Set-Content -Encoding ascii (Join-Path $sandbox "README.md")
    New-Item -ItemType Directory -Force -Path (Join-Path $sandbox "docs") | Out-Null
    "notes" | Set-Content -Encoding ascii (Join-Path $sandbox "docs\notes.md")

    & git -C $sandbox init -q
    & git -C $sandbox checkout -q -b main
    & git -C $sandbox config user.name test
    & git -C $sandbox config user.email test@example.invalid
    New-Commit $sandbox "initial"
    & git init -q --bare $mirror

    $env:KORRA_CODEBERG_REMOTE = ""

    # 0. no remote configured -> 2, and the output is a set of instructions
    $r = Run-Sync @()
    Check "no remote: exit 2 + setup instructions" ($r.Code -eq 2 -and $r.Text -match "git remote add codeberg") $r.Text

    & git -C $sandbox remote add codeberg $mirror
    $localHead = Tip $sandbox

    # 1. host check: a local path is not codeberg.org
    $r = Run-Sync @("-Remote", "codeberg")
    Check "host check: refuses a non-Codeberg remote (3)" ($r.Code -eq 3 -and $r.Text -match "not codeberg.org") $r.Text

    # 2. dry run on a clean tree -> 0, nothing pushed
    $r = Run-Sync @("-Remote", "codeberg", "-AnyRemote", "-DryRun")
    $emptyMirror = ((& git -C $mirror rev-parse --verify --quiet refs/heads/main 2>&1) | Out-String).Trim()
    Check "dry run: exit 0, empty mirror stays empty" ($r.Code -eq 0 -and $r.Text -match "would push" -and -not $emptyMirror) ($r.Text + "`nmirror tip: '" + $emptyMirror + "'")

    # 3. content guard: a force-added .cpk must stop the run before any push
    "stolen" | Set-Content -Encoding ascii (Join-Path $sandbox "evil.cpk")
    & git -C $sandbox add -f evil.cpk
    & git -C $sandbox commit -q -m "smuggle a cpk"
    $r = Run-Sync @("-Remote", "codeberg", "-AnyRemote")
    Check "content guard: rejects a tracked .cpk (1)" ($r.Code -eq 1 -and $r.Text -match "evil\.cpk") $r.Text
    & git -C $sandbox reset --hard -q HEAD~1
    Remove-Item (Join-Path $sandbox "evil.cpk") -Force -ErrorAction SilentlyContinue

    # 4. dirty tree: an untracked, non-ignored file means the mirror is not a commit
    "draft" | Set-Content -Encoding ascii (Join-Path $sandbox "scratch.txt")
    $r = Run-Sync @("-Remote", "codeberg", "-AnyRemote")
    Check "dirty tree: refuses to mirror (1)" ($r.Code -eq 1 -and $r.Text -match "not clean") $r.Text
    Remove-Item (Join-Path $sandbox "scratch.txt") -Force

    # 5. first push lands, and the mirror tip equals the local commit
    $r = Run-Sync @("-Remote", "codeberg", "-AnyRemote")
    $mirrored = ("$( (& git -C $mirror rev-parse refs/heads/main) )").Trim()
    Check "first push: mirror tip == local HEAD" ($r.Code -eq 0 -and $mirrored -eq $localHead -and $r.Text -match "verified") ($r.Text + "`nmirror: " + $mirrored + " local: " + $localHead)

    # 6. re-run is a no-op
    $r = Run-Sync @("-Remote", "codeberg", "-AnyRemote")
    Check "idempotent: second run pushes nothing (0)" ($r.Code -eq 0 -and $r.Text -match "nothing to push") $r.Text

    # 7. new local commit -> fast-forward, mirror follows
    "more" | Set-Content -Encoding ascii (Join-Path $sandbox "docs\more.md")
    New-Commit $sandbox "second"
    $localHead = Tip $sandbox
    $r = Run-Sync @("-Remote", "codeberg", "-AnyRemote")
    $mirrored = ("$( (& git -C $mirror rev-parse refs/heads/main) )").Trim()
    Check "fast-forward: one commit sent, mirror follows" ($r.Code -eq 0 -and $mirrored -eq $localHead -and $r.Text -match "1 commit") ($r.Text + "`nmirror: " + $mirrored + " local: " + $localHead)

    # 8. someone else pushed to the mirror -> refuse, never force
    & git clone -q -b main $mirror $other
    & git -C $other config user.name other
    & git -C $other config user.email other@example.invalid
    "theirs" | Set-Content -Encoding ascii (Join-Path $other "theirs.md")
    New-Commit $other "mirror-side commit"
    & git -C $other push -q origin HEAD:refs/heads/main
    $r = Run-Sync @("-Remote", "codeberg", "-AnyRemote")
    Check "non-fast-forward: refuses to force (4)" ($r.Code -eq 4 -and $r.Text -match "refusing a non-fast-forward") $r.Text
}
finally {
    if (-not $Keep) { Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue }
    else { Write-Host ("kept: " + $tmp) }
}

Write-Host ("sync_codeberg selftest: {0}/{1} passed" -f $script:passed, ($script:passed + $script:failed))
if ($script:failed -gt 0) { exit 1 }
exit 0
