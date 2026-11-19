# Drives the host window with real foreground input so the menu can be walked,
# screenshotting as it goes. SetForegroundWindow alone is refused for a process
# that does not own the foreground; AttachThreadInput borrows the foreground
# thread's input queue, which is the documented way through that lock, and the
# engine only sees DirectInput keyboard data while its window is foreground.
#
#   powershell -ExecutionPolicy Bypass -File scripts\probe_input.ps1 [-Wait 50] [-Steps enter,enter,esc]
param(
    [int]$Wait = 50,
    [string]$Steps = "enter,enter,enter",
    [int]$StepWait = 6,
    [string]$Prefix = "input",
    [string]$Game = ""
)
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public class F {
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, IntPtr p);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr SetFocus(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
    [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool attach);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint f, int dx, int dy, uint d, UIntPtr e);
    public struct RECT { public int Left, Top, Right, Bottom; }
    public static List<IntPtr> Windows(uint pid) {
        var found = new List<IntPtr>();
        EnumWindows((h, l) => {
            uint wp; GetWindowThreadProcessId(h, out wp);
            RECT r;
            if (wp == pid && IsWindowVisible(h) && GetWindowRect(h, out r)
                && (r.Right - r.Left) > 200 && (r.Bottom - r.Top) > 150) found.Add(h);
            return true;
        }, IntPtr.Zero);
        return found;
    }
    public static void Force(IntPtr h) {
        // Windows only lets a process take the foreground if it is attached to
        // the input queue of the thread that currently owns it. Attaching to the
        // target instead does nothing (the target is not the foreground).
        IntPtr fg = GetForegroundWindow();
        uint fgTid = fg == IntPtr.Zero ? 0 : GetWindowThreadProcessId(fg, IntPtr.Zero);
        uint me = GetCurrentThreadId();
        bool attached = false;
        if (fgTid != 0 && fgTid != me) attached = AttachThreadInput(me, fgTid, true);
        BringWindowToTop(h);
        SetForegroundWindow(h);
        SetFocus(h);
        // Force the z-order as well: the game draws into a D3D swapchain and a
        // CopyFromScreen capture takes whatever is on top at those coordinates.
        SetWindowPos(h, new IntPtr(-1), 0, 0, 0, 0, 0x0001 | 0x0002 | 0x0040);
        if (attached) AttachThreadInput(me, fgTid, false);
    }
    public static void Key(ushort vk) {
        keybd_event((byte)vk, 0, 0, UIntPtr.Zero);
        System.Threading.Thread.Sleep(90);
        keybd_event((byte)vk, 0, 2, UIntPtr.Zero);
    }
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
    public static void Click(int x, int y) {
        SetCursorPos(x, y);
        System.Threading.Thread.Sleep(150);
        mouse_event(0x0002, 0, 0, 0, UIntPtr.Zero);
        System.Threading.Thread.Sleep(90);
        mouse_event(0x0004, 0, 0, 0, UIntPtr.Zero);
    }
}
"@
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root "build\korra_recomp.exe"
if (-not $Game) { $Game = Join-Path (Split-Path -Parent (Split-Path -Parent $root)) "The Legend of Korra" }

$script:h = [IntPtr]::Zero
function Shot([string]$name) {
    # Re-find the window every time: the engine re-creates it when the display
    # mode changes, and a stale handle captures a 1x1 bitmap.
    $w = [F]::Windows([uint32]$p.Id)
    if ($w.Count -eq 0) { Write-Host "shot ${name}: no window"; return }
    $best = [IntPtr]::Zero; $bestArea = 0
    foreach ($cand in $w) {
        $rc = New-Object F+RECT
        if ([F]::GetWindowRect($cand, [ref]$rc)) {
            $area = ($rc.Right - $rc.Left) * ($rc.Bottom - $rc.Top)
            if ($area -gt $bestArea) { $bestArea = $area; $best = $cand }
        }
    }
    if ($best -eq [IntPtr]::Zero) { Write-Host "shot ${name}: no measurable window"; return }
    $script:h = $best
    $r = New-Object F+RECT
    [void][F]::GetWindowRect($script:h, [ref]$r)
    if (($r.Right - $r.Left) -lt 64 -or ($r.Bottom - $r.Top) -lt 64) {
        Write-Host ("shot {0}: window 0x{1:X} rect {2},{3} {4}x{5} - skipped" -f $name,
                    [int64]$script:h, $r.Left, $r.Top, ($r.Right - $r.Left), ($r.Bottom - $r.Top))
        return
    }
    $b = [System.Drawing.Rectangle]::FromLTRB($r.Left, $r.Top, $r.Right, $r.Bottom)
    $bmp = New-Object System.Drawing.Bitmap $b.Width, $b.Height
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($b.Location, [System.Drawing.Point]::Empty, $b.Size)
    $path = Join-Path $root ("build\shots\{0}_{1}.png" -f $Prefix, $name)
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $path) | Out-Null
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bmp.Dispose()
    Write-Host ("saved {0} ({1}x{2})" -f $path, $b.Width, $b.Height)
}

$p = Start-Process -FilePath $exe -ArgumentList "`"$Game`"" -PassThru `
     -RedirectStandardOutput (Join-Path $root "build\run.out") -RedirectStandardError (Join-Path $root "build\run.err")
# Wait for the window to exist (polling, not a fixed sleep): the engine creates
# it once the CRT and D3D9 are up, which is fast but not instant.
for ($i = 0; $i -lt 120; $i++) {
    Start-Sleep -Milliseconds 250
    if ($p.HasExited) { Write-Host "process exited before the window existed (code $($p.ExitCode))"; exit 1 }
    $wins = [F]::Windows([uint32]$p.Id)
    if ($wins.Count -gt 0) { break }
}
if ($wins.Count -eq 0) { Write-Host "no window"; $p.Kill(); exit 1 }
$script:h = $wins[0]
[F]::Force($script:h)
Start-Sleep -Seconds 1
[F]::Key(0x1B)      # Escape skips the startup movies
Write-Host "sent Escape to skip the intro movies"
Start-Sleep -Milliseconds 700
[F]::Force($script:h)
$fg = [F]::GetForegroundWindow()
Write-Host ("window 0x{0:X} foreground 0x{1:X} focused={2}" -f [int64]$script:h, [int64]$fg, ($fg -eq $script:h))
Shot "00_boot"

$i = 1
foreach ($step in ($Steps -split ",")) {
    if ($p.HasExited) { Write-Host "process exited at step $step (code $($p.ExitCode))"; break }
    switch ($step.Trim()) {
        "enter" { [F]::Key(0x0D) }
        "space" { [F]::Key(0x20) }
        "esc"   { [F]::Key(0x1B) }
        "up"    { [F]::Key(0x26) }
        "down"  { [F]::Key(0x28) }
        "left"  { [F]::Key(0x25) }
        "right" { [F]::Key(0x27) }
        "click" {
            $r = New-Object F+RECT
            [void][F]::GetWindowRect($script:h, [ref]$r)
            [F]::Click([int](($r.Left + $r.Right) / 2), [int](($r.Top + $r.Bottom) / 2))
        }
        "move" {
            $r = New-Object F+RECT
            [void][F]::GetWindowRect($script:h, [ref]$r)
            [F]::SetCursorPos([int]($r.Left + ($r.Right - $r.Left) * 0.15),
                              [int]($r.Top + ($r.Bottom - $r.Top) * 0.25))
        }
        "move2" {
            $r = New-Object F+RECT
            [void][F]::GetWindowRect($script:h, [ref]$r)
            [F]::SetCursorPos([int]($r.Left + ($r.Right - $r.Left) * 0.75),
                              [int]($r.Top + ($r.Bottom - $r.Top) * 0.60))
        }
        default { Write-Host "unknown step $step" }
    }
    Write-Host "step $i : $step"
    Start-Sleep -Seconds $StepWait
    Shot ("{0:D2}_{1}" -f $i, $step)
    $i++
}
if ($p.HasExited) { Write-Host "process ended (code $($p.ExitCode))" }
else { Write-Host "still running"; $p.Kill(); $p.WaitForExit() }
