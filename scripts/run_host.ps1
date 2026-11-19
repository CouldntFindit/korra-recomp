# Runs the host for a bounded time and prints what it said.
#
#   powershell -ExecutionPolicy Bypass -File scripts\run_host.ps1 [-Seconds 30] [-Game "C:\path\to\The Legend of Korra"] [-Extra "-CaughtTrace ..."] [-Exe build\korra_recomp_trace.exe]
#
# The guest normally never returns, so the script kills the process after the
# timeout and shows stdout, stderr and the tail of the game-folder log. A crash
# shows up as a non-zero exit code with the runtime's report in the log.
param(
    [int]$Seconds = 30,
    [string]$Game = "",
    [string]$Extra = "",
    [string]$Exe = "build\korra_recomp.exe"
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root $Exe
if (-not (Test-Path $exe)) { throw "no $exe - run scripts\build_host.ps1 first" }
if (-not $Game) { $Game = Join-Path (Split-Path -Parent (Split-Path -Parent $root)) "The Legend of Korra" }

$out = Join-Path $root "build\run.out"
$err = Join-Path $root "build\run.err"
$arglist = "`"$Game`""
if ($Extra) { $arglist = "$arglist $Extra" }
$p = Start-Process -FilePath $exe -ArgumentList $arglist -PassThru -RedirectStandardOutput $out -RedirectStandardError $err
$exited = $p.WaitForExit($Seconds * 1000)
if (-not $exited) { $p.Kill(); $p.WaitForExit() }

Write-Host ("== exit: {0}  ({1})" -f $p.ExitCode, $(if ($exited) { "exited" } else { "killed after ${Seconds}s" }))
Write-Host "== stdout"
Get-Content $out -ErrorAction SilentlyContinue | Select-Object -Last 30
Write-Host "== stderr"
Get-Content $err -ErrorAction SilentlyContinue | Select-Object -Last 40
$log = Get-ChildItem (Join-Path $Game "korra_host-*.log") -ErrorAction SilentlyContinue |
       Sort-Object LastWriteTime | Select-Object -Last 1
if ($log) {
    Write-Host "== log tail ($($log.FullName))"
    Get-Content $log.FullName | Select-Object -Last 40
} else {
    $legacy = Join-Path $Game "korra_host.log"
    if (Test-Path $legacy) { Write-Host "== log tail ($legacy)"; Get-Content $legacy | Select-Object -Last 40 }
}
