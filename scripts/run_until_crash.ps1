# Runs the host until it crashes or the budget runs out, retrying the boots that
# quit during startup.
#
# The engine sometimes ends the process with code 0 about 0.4 s in, before the
# audio device exists (see docs/FINDINGS.md 2026-11-20). That is a boot-time
# abort, not the defect under test, so a run only counts once the XAudio2 device
# has been created. Kill + retry keeps an unattended run honest instead of
# spending the budget screenshotting a dead window.
#
# It also does the two things the engine needs before it will leave the intro
# movies: focus (DirectInput only reports keys to the foreground window) and one
# Escape after a second.
#
#   powershell -ExecutionPolicy Bypass -File scripts\run_until_crash.ps1 -Seconds 120 -Exe build\korra_recomp_trace.exe -Extra "--calltrace build\ct.txt"
param(
    [int]$Seconds = 120,
    [string]$Game = "",
    [string]$Exe = "build\korra_recomp.exe",
    [string]$Extra = "",
    [int]$Attempts = 4,
    [int]$BootCheckSeconds = 8
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root $Exe
if (-not $Game) { $Game = Join-Path (Split-Path -Parent (Split-Path -Parent $root)) "The Legend of Korra" }
$arglist = "`"$Game`""
if ($Extra) { $arglist = "$arglist $Extra" }

function Latest-Log {
    Get-ChildItem (Join-Path $Game "korra_host-*.log") -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime | Select-Object -Last 1
}

Add-Type @"
using System;
using System.Runtime.InteropServices;
public class K {
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr SetFocus(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
    [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool attach);
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, IntPtr p);
    public static void Force(IntPtr h) {
        IntPtr fg = GetForegroundWindow();
        uint fgTid = fg == IntPtr.Zero ? 0 : GetWindowThreadProcessId(fg, IntPtr.Zero);
        uint me = GetCurrentThreadId();
        bool attached = fgTid != 0 && fgTid != me && AttachThreadInput(me, fgTid, true);
        SetForegroundWindow(h);
        SetFocus(h);
        if (attached) AttachThreadInput(me, fgTid, false);
    }
    public static void Esc() {
        keybd_event(0x1B, 0, 0, UIntPtr.Zero);
        System.Threading.Thread.Sleep(90);
        keybd_event(0x1B, 0, 2, UIntPtr.Zero);
    }
}
"@

for ($attempt = 1; $attempt -le $Attempts; $attempt++) {
    $p = Start-Process -FilePath $exe -ArgumentList $arglist -PassThru `
         -RedirectStandardOutput (Join-Path $root "build\run.out") `
         -RedirectStandardError  (Join-Path $root "build\run.err")

    # Focus + one Escape: that is what skips the startup movies.
    $win = [IntPtr]::Zero
    for ($i = 0; $i -lt 40 -and $win -eq [IntPtr]::Zero; $i++) {
        Start-Sleep -Milliseconds 250
        if ($p.HasExited) { break }
        $win = (Get-Process -Id $p.Id -ErrorAction SilentlyContinue).MainWindowHandle
        if ($win -eq $null) { $win = [IntPtr]::Zero }
    }
    if ($win -ne [IntPtr]::Zero) {
        [K]::Force($win)
        Start-Sleep -Seconds 1
        [K]::Esc()
    }

    Start-Sleep -Seconds $BootCheckSeconds
    $log = Latest-Log
    $booted = $log -and (Select-String -Path $log.FullName -Pattern 'CoCreateInstance\(XAudio2' -Quiet)
    if (-not $booted) {
        if (-not $p.HasExited) { $p.Kill(); $p.WaitForExit() }
        $why = if ($p.HasExited) { "exit 0x{0:X8}" -f [uint32]$p.ExitCode } else { "killed" }
        Write-Host ("attempt {0}: boot aborted ({1}), retrying" -f $attempt, $why)
        continue
    }

    $sw = [Diagnostics.Stopwatch]::StartNew()
    $left = [Math]::Max(1, $Seconds - $BootCheckSeconds)
    $exited = $p.WaitForExit($left * 1000)
    $sw.Stop()
    if ($exited) {
        Write-Host ("attempt {0}: exited after {1:N1}s  code=0x{2:X8}" -f $attempt,
                    ($sw.Elapsed.TotalSeconds + $BootCheckSeconds), [uint32]$p.ExitCode)
    } else {
        Write-Host ("attempt {0}: still running after {1}s" -f $attempt, $Seconds)
        $p.Kill(); $p.WaitForExit()
    }
    $log = Latest-Log
    Write-Host "log: $($log.FullName)"
    $fatal = Select-String -Path $log.FullName -Pattern '\[fatal\]'
    if ($fatal) { $fatal | Select-Object -First 3 | ForEach-Object { Write-Host $_.Line } }
    exit 0
}
Write-Host "every attempt aborted during boot"
exit 1
