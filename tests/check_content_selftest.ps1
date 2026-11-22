# Self-test for tools/check_content.py: the content guard must reject
# machine-identifying paths and accept the placeholder forms the repo uses.
# The fixture strings are assembled from fragments so this file does not
# itself publish the patterns it tests.
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$py = (Get-Command python -ErrorAction SilentlyContinue).Source
if (-not $py) { throw "python not found on PATH" }
$check = Join-Path $root "tools\check_content.py"
$work = Join-Path $env:TEMP ("content-selftest-" + [guid]::NewGuid())
New-Item -ItemType Directory -Force -Path $work | Out-Null

$bs = [char]92
$win = "C:" + $bs + "Users" + $bs + "someone" + $bs + "game"
$winFwd = "C:/" + "Users" + "/someone/game"
$cloud = "One" + "Drive"
$loc = "Bel" + "geler"
$posix = "/" + "home" + "/someone/game"

$cases = @(
    @{ name = "win-profile.md"; text = $win; reject = $true },
    @{ name = "win-profile-fwd.md"; text = $winFwd; reject = $true },
    @{ name = "cloud-sync.md"; text = ("lives in " + $cloud + " sync"); reject = $true },
    @{ name = "documents.md"; text = ("the " + $loc + $bs + "Korra folder"); reject = $true },
    @{ name = "home.md"; text = $posix; reject = $true },
    @{ name = "clean.md"; text = '<workspace>\android-sdk %LOCALAPPDATA%\Python $env:LOCALAPPDATA'; reject = $false }
)

$fail = 0
foreach ($c in $cases) {
    $p = Join-Path $work $c.name
    Set-Content -Path $p -Value $c.text -Encoding ASCII
    & $py $check $p *> $null
    $code = $LASTEXITCODE
    $ok = ($c.reject -and $code -eq 1) -or ((-not $c.reject) -and $code -eq 0)
    if ($ok) {
        Write-Host ("ok   {0}" -f $c.name)
    } else {
        Write-Host ("FAIL {0}: expected reject={1}, exit={2}" -f $c.name, $c.reject, $code)
        $fail++
    }
}
Remove-Item $work -Recurse -Force
if ($fail) { throw "$fail case(s) failed" }
Write-Host "check_content selftest: $($cases.Count)/$($cases.Count) passed"
