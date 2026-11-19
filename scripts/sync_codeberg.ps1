# Mirrors this repository to Codeberg -- source only, never binaries.
#
#   powershell -ExecutionPolicy Bypass -File scripts\sync_codeberg.ps1 [-DryRun]
#
# What it does, in order (any failure stops it before a byte leaves the machine):
#   1. refuses a dirty tree, so the mirror is exactly a commit that exists here
#   2. runs tools\check_content.py over every tracked path (game content,
#      generated code, build artifacts, >20 MiB files)
#   3. insists the remote host is codeberg.org
#   4. fetches and refuses a non-fast-forward (the mirror is never forced)
#   5. pushes HEAD:refs/heads/<branch> plus annotated tags, then verifies the
#      mirror's tip equals the local commit
#
# Configuration (env vars also work, so a scheduled task needs no arguments):
#   KORRA_CODEBERG_REMOTE   remote name           (default: "codeberg")
#   KORRA_CODEBERG_BRANCH   branch to mirror       (default: current branch)
#
# Exit codes: 0 ok / 1 dirty tree or content guard / 2 remote not configured
#             3 wrong host / 4 not a fast-forward / 5 git or push failure
param(
    [string]$Remote = "",
    [string]$Branch = "",
    [string]$RemoteUrl = "",
    [switch]$AnyRemote,
    [switch]$DryRun
)
Set-StrictMode -Version 2
$ErrorActionPreference = "Continue"

$root = Split-Path -Parent $PSScriptRoot
$logDir = Join-Path $root "logs"
$logFile = Join-Path $logDir "sync_codeberg.log"

function Say([string]$Text) { Write-Host $Text }
function Write-Log([string]$Text) {
    try {
        if (-not (Test-Path $logDir)) { New-Item -ItemType Directory -Force -Path $logDir | Out-Null }
        $line = (Get-Date -Format "yyyy-MM-dd HH:mm:ss") + "  " + $Text + "`n"
        [IO.File]::AppendAllText($logFile, $line)
    } catch {
        # logging is a convenience; never let it break a mirror run
    }
}
function Fail([int]$Code, [string]$Text) {
    Write-Host ("sync_codeberg: " + $Text) -ForegroundColor Red
    Write-Log ("FAILED (" + $Code + "): " + $Text)
    exit $Code
}
# Named Invoke-Git on purpose: a function called `Git` would shadow git.exe and
# recurse into itself (found the hard way, see the selftest).
function Invoke-Git([string[]]$GitArgs, [switch]$AllowFail) {
    $out = & git @GitArgs 2>&1
    $code = $LASTEXITCODE
    if ($code -ne 0 -and -not $AllowFail) {
        Fail 5 ("git " + ($GitArgs -join " ") + " failed (" + $code + "):`n" + (($out | Out-String).TrimEnd()))
    }
    return @{ Code = $code; Out = @($out) }
}
function Text([string[]]$Lines) { return (($Lines | Out-String).Trim()) }
function Url-Host([string]$Url) {
    # ssh://git@codeberg.org/u/r.git | git@codeberg.org:u/r.git | https://codeberg.org/u/r.git
    $m = [regex]::Match($Url, '^(?:[A-Za-z+]+://)?(?:[^@/]+@)?([^:/]+)')
    if ($m.Success) { return $m.Groups[1].Value.ToLowerInvariant() }
    return ""
}
function Url-Web([string]$Url) {
    # best-effort human URL for the log line; anything odd stays readable
    $t = $Url -replace '^[A-Za-z+]+://', ''
    $t = $t -replace '^[^@/]+@', ''
    $t = $t -replace '^([^:/\\]+):(?![/\\])', '$1/'
    $t = $t -replace '\.git$', ''
    if ($t -notmatch '^https?://') { $t = 'https://' + $t }
    return $t
}

Push-Location $root
try {
    if ((Invoke-Git @("rev-parse", "--git-dir") -AllowFail).Code -ne 0) {
        Fail 5 ("not a git repository: " + $root)
    }

    # ---------------------------------------------------------------- remote
    $remoteName = $Remote
    if (-not $remoteName -and $env:KORRA_CODEBERG_REMOTE) { $remoteName = $env:KORRA_CODEBERG_REMOTE }
    if (-not $remoteName) {
        $names = @((Invoke-Git @("remote")).Out | ForEach-Object { "$_".Trim() } | Where-Object { $_ })
        if ($names -contains "codeberg") { $remoteName = "codeberg" }
    }
    if (-not $remoteName) {
        Write-Host "sync_codeberg: no Codeberg remote configured yet." -ForegroundColor Yellow
        Write-Host ""
        Write-Host "  1. create the mirror (once, in the Codeberg web UI):"
        Write-Host "       https://codeberg.org/repo/create    owner=<you>  name=korra-recomp"
        Write-Host "       Visibility: public.  Do NOT initialise it with a README."
        Write-Host ""
        Write-Host "  2. register the key you push with (ssh -T git@codeberg.org must greet you), then:"
        Write-Host "       git remote add codeberg ssh://git@codeberg.org/<you>/korra-recomp.git"
        Write-Host ""
        Write-Host "  3. re-run:"
        Write-Host "       powershell -ExecutionPolicy Bypass -File scripts\sync_codeberg.ps1 -DryRun"
        Write-Host ""
        Write-Host "  Full walk-through: docs/PUBLISHING.md"
        exit 2
    }

    $url = $RemoteUrl
    if (-not $url) {
        $urlOut = Invoke-Git @("remote", "get-url", $remoteName) -AllowFail
        if ($urlOut.Code -ne 0) { Fail 2 ("remote '" + $remoteName + "' does not exist") }
        $url = (("$($urlOut.Out)")).Trim()
    }
    $remoteHost = Url-Host $url
    if (-not $AnyRemote -and $remoteHost -ne "codeberg.org") {
        Fail 3 ("remote '" + $remoteName + "' points at '" + $remoteHost + "', not codeberg.org. " +
                "Use -AnyRemote only for a throwaway test repository.")
    }

    # ---------------------------------------------------------------- branch
    $branchName = $Branch
    if (-not $branchName -and $env:KORRA_CODEBERG_BRANCH) { $branchName = $env:KORRA_CODEBERG_BRANCH }
    if (-not $branchName) { $branchName = (("$((Invoke-Git @("rev-parse", "--abbrev-ref", "HEAD")).Out)")).Trim() }
    if (-not $branchName -or $branchName -eq "HEAD") { Fail 5 "detached HEAD: check out the branch you want to mirror" }

    $head = (("$((Invoke-Git @("rev-parse", "HEAD")).Out)")).Trim()
    $subject = (("$((Invoke-Git @("log", "-1", "--pretty=%s")).Out)")).Trim()

    # ------------------------------------------------------- tree must be clean
    # A mirror is "the commit", not "the commit plus whatever is lying around".
    $status = @((Invoke-Git @("status", "--porcelain")).Out | Where-Object { "$_".Trim() })
    if ($status.Count -gt 0) {
        Write-Host "sync_codeberg: working tree is not clean:" -ForegroundColor Red
        $status | ForEach-Object { Write-Host ("  " + "$_") }
        Fail 1 "commit, stash or ignore those paths first (a mirror must equal a commit)"
    }

    # ------------------------------------------------------------ content guard
    $tracked = @((Invoke-Git @("ls-files")).Out | Where-Object { "$_".Trim() })
    if ($tracked.Count -eq 0) { Fail 1 "no tracked files: refusing to mirror an empty repository" }
    $python = Get-Command python -ErrorAction SilentlyContinue
    if (-not $python) { Fail 5 "python is not on PATH (needed for tools\check_content.py)" }
    $guard = Join-Path $root "tools\check_content.py"
    if (-not (Test-Path $guard)) { Fail 5 ("missing content guard: " + $guard) }

    $chunkSize = 100
    for ($i = 0; $i -lt $tracked.Count; $i += $chunkSize) {
        $last = [Math]::Min($i + $chunkSize - 1, $tracked.Count - 1)
        $chunk = @($tracked[$i..$last])
        $g = & python $guard @chunk 2>&1
        if ($LASTEXITCODE -ne 0) {
            $g | ForEach-Object { Write-Host "$_" }
            Fail 1 "the content guard rejected tracked files -- nothing was pushed"
        }
    }

    # ---------------------------------------------------------------- fetch
    Say ("sync_codeberg: " + $tracked.Count + " tracked files clean; " + $branchName + " @ " +
         $head.Substring(0, 8) + "  " + $subject)
    $remoteRef = "refs/remotes/" + $remoteName + "/" + $branchName
    $fetch = Invoke-Git @("fetch", "--quiet", $remoteName) -AllowFail
    if ($fetch.Code -ne 0) {
        $sshHost = "git@" + $remoteHost
        Fail 5 ("cannot reach '" + $remoteName + "' (" + $url + "):`n" + (Text $fetch.Out) +
                "`n  check: ssh -T " + $sshHost + "   (and that the repository exists)")
    }

    $remoteSha = ""
    $has = Invoke-Git @("rev-parse", "--verify", "--quiet", $remoteRef) -AllowFail
    if ($has.Code -eq 0) { $remoteSha = ("$($has.Out)").Trim() }

    if ($remoteSha -eq $head) {
        Say ("sync_codeberg: mirror already at " + $head.Substring(0, 8) + " -- nothing to push")
        Write-Log ("noop branch=" + $branchName + " sha=" + $head)
        exit 0
    }

    if ($remoteSha) {
        $ff = Invoke-Git @("merge-base", "--is-ancestor", $remoteSha, $head) -AllowFail
        if ($ff.Code -ne 0) {
            $counts = Invoke-Git @("rev-list", "--left-right", "--count", ("HEAD..." + $remoteRef)) -AllowFail
            Fail 4 ("mirror has commits you do not have; refusing a non-fast-forward.`n" +
                    "  ahead/behind (local/remote)  " + (Text $counts.Out) + "`n" +
                    "  The mirror is read-only history. If it was never meant to diverge, re-create it; " +
                    "otherwise merge the remote branch explicitly and re-run.")
        }
        $n = Invoke-Git @("rev-list", "--count", ($remoteSha + ".." + $head))
        Say ("sync_codeberg: fast-forward, " + ("$($n.Out)").Trim() + " commit(s) to send")
    } else {
        Say "sync_codeberg: first push to an empty mirror"
    }

    if ($DryRun) {
        Say ("sync_codeberg: -DryRun: would push " + $branchName + " -> " + $url)
        Write-Log ("dryrun branch=" + $branchName + " sha=" + $head + " url=" + $url)
        exit 0
    }

    # ---------------------------------------------------------------- push
    $push = Invoke-Git @("push", "--porcelain", $remoteName, ("HEAD:refs/heads/" + $branchName))
    ($push.Out | Out-String) -split "`r?`n" | Where-Object { $_ -match '\S' } | ForEach-Object { Write-Host ("  " + $_) }

    $tags = @((Invoke-Git @("tag", "-l")).Out | Where-Object { "$_".Trim() })
    if ($tags.Count -gt 0) {
        $pt = Invoke-Git @("push", "--quiet", $remoteName, "--tags") -AllowFail
        if ($pt.Code -ne 0) { Write-Host ("  tags not pushed: " + (Text $pt.Out)) -ForegroundColor Yellow }
    }

    # ---------------------------------------------------------------- verify
    $null = Invoke-Git @("fetch", "--quiet", $remoteName) -AllowFail
    $after = ("$((Invoke-Git @("rev-parse", "--verify", "--quiet", $remoteRef)).Out)").Trim()
    if ($after -ne $head) {
        Fail 5 ("pushed, but " + $remoteRef + " reads '" + $after + "' instead of " + $head +
                " -- check the mirror by hand")
    }
    Say ("sync_codeberg: mirror verified at " + $head.Substring(0, 8) + "  " + (Url-Web $url))
    Write-Log ("pushed branch=" + $branchName + " sha=" + $head + " url=" + $url)
    exit 0
}
finally {
    Pop-Location
}
