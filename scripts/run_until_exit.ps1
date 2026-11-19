# Runs the host until it ends (or the timeout), then prints the exit code.
# An empty code means the process is still running; a negative one is the
# Win32 exception code the process died with (e.g. -1073741819 = 0xC0000005).
#
#   powershell -ExecutionPolicy Bypass -File scripts\run_until_exit.ps1 [-Seconds 120]
param(
    [int]$Seconds = 120,
    [string]$Game = "",
    [string]$Exe = "build\korra_recomp.exe",
    [string]$Extra = ""
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root $Exe
if (-not $Game) { $Game = Join-Path (Split-Path -Parent (Split-Path -Parent $root)) "The Legend of Korra" }
$arglist = "`"$Game`""
if ($Extra) { $arglist = "$arglist $Extra" }
$p = Start-Process -FilePath $exe -ArgumentList $arglist -PassThru `
     -RedirectStandardOutput (Join-Path $root "build\run.out") -RedirectStandardError (Join-Path $root "build\run.err")
$sw = [Diagnostics.Stopwatch]::StartNew()
$exited = $p.WaitForExit($Seconds * 1000)
$sw.Stop()
if ($exited) {
    Write-Host ("exited after {0:N1}s  ExitCode={1}  (0x{2:X8})" -f $sw.Elapsed.TotalSeconds,
                $p.ExitCode, [uint32]$p.ExitCode)
} else {
    Write-Host ("still running after {0:N1}s" -f $sw.Elapsed.TotalSeconds)
    $p.Kill(); $p.WaitForExit()
}
$log = Get-ChildItem (Join-Path $Game "korra_host-*.log") -ErrorAction SilentlyContinue |
       Sort-Object LastWriteTime | Select-Object -Last 1
if ($log) { Write-Host "log: $($log.FullName)"; Get-Content $log.FullName | Select-Object -Last 8 }
