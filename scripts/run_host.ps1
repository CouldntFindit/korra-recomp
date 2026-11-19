# Runs the host for a bounded time and prints what it said.
#
#   powershell -ExecutionPolicy Bypass -File scripts\run_host.ps1 [-Seconds 30] [-Game "C:\path\to\The Legend of Korra"]
#
# The guest normally never returns, so the script kills the process after the
# timeout and shows stdout, stderr and the tail of the game-folder log. A crash
# shows up as a non-zero exit code with the runtime's report in the log.
param(
    [int]$Seconds = 30,
    [string]$Game = ""
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root "build\korra_recomp.exe"
if (-not (Test-Path $exe)) { throw "no $exe - run scripts\build_host.ps1 first" }
if (-not $Game) { $Game = Join-Path (Split-Path -Parent (Split-Path -Parent $root)) "The Legend of Korra" }

$out = Join-Path $root "build\run.out"
$err = Join-Path $root "build\run.err"
$p = Start-Process -FilePath $exe -ArgumentList "`"$Game`"" -PassThru -RedirectStandardOutput $out -RedirectStandardError $err
$exited = $p.WaitForExit($Seconds * 1000)
if (-not $exited) { $p.Kill(); $p.WaitForExit() }

Write-Host ("== exit: {0}  ({1})" -f $p.ExitCode, $(if ($exited) { "exited" } else { "killed after ${Seconds}s" }))
Write-Host "== stdout"
Get-Content $out -ErrorAction SilentlyContinue | Select-Object -Last 30
Write-Host "== stderr"
Get-Content $err -ErrorAction SilentlyContinue | Select-Object -Last 40
$log = Join-Path $Game "korra_host.log"
if (Test-Path $log) {
    Write-Host "== log tail ($log)"
    Get-Content $log | Select-Object -Last 40
}
