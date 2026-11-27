# Starts the host, waits, and screenshots its window (or the whole screen if the
# window cannot be found). Used as proof for the M2.7 "first frame" gate.
#
#   powershell -ExecutionPolicy Bypass -File scripts\snap_window.ps1 [-Seconds 20] [-Out build\shot.png]
param(
    [int]$Seconds = 20,
    [string]$Out = "build\shot.png",
    [string]$Game = "",
    [int]$ProcessId = 0
)
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public class W {
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll", CharSet=CharSet.Auto)] public static extern int GetWindowText(IntPtr h, System.Text.StringBuilder s, int n);
    public struct RECT { public int Left, Top, Right, Bottom; }

    // Every visible top-level window of `pid`, largest first.
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
}
"@
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root "build\korra_recomp.exe"
if (-not $Game) { $Game = Join-Path (Split-Path -Parent (Split-Path -Parent $root)) "The Legend of Korra" }

# -Pid captures the window of a game that is already running (the oracle runs
# started by build\win_run.ps1) instead of starting one; nothing is killed and
# -Seconds is ignored, so several shots can be taken of the same run.
if ($ProcessId) {
    $p = Get-Process -Id $ProcessId -ErrorAction Stop
} else {
    $p = Start-Process -FilePath $exe -ArgumentList "`"$Game`"" -PassThru -RedirectStandardOutput (Join-Path $root "build\run.out") -RedirectStandardError (Join-Path $root "build\run.err")
    Write-Host "waiting $Seconds s for the window..."
    Start-Sleep -Seconds $Seconds
}
$p.Refresh()
$h = [IntPtr]::Zero
$wins = [W]::Windows([uint32]$p.Id)
if ($wins.Count -gt 0) { $h = $wins[0] }
if ($h -eq [IntPtr]::Zero) {
    $h = $p.MainWindowHandle
}
if ($h -eq [IntPtr]::Zero) {
    Write-Host "no main window; capturing the whole screen"
    $b = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
} else {
    $r = New-Object W+RECT
    [void][W]::GetWindowRect($h, [ref]$r)
    $b = [System.Drawing.Rectangle]::FromLTRB($r.Left, $r.Top, $r.Right, $r.Bottom)
    Write-Host ("window 0x{0:X} at {1},{2} {3}x{4}" -f [int64]$h, $r.Left, $r.Top, $b.Width, $b.Height)
}
if ($b.Width -le 0 -or $b.Height -le 0) { $b = [System.Drawing.Rectangle]::new(0, 0, 1920, 1080) }

$bmp = New-Object System.Drawing.Bitmap $b.Width, $b.Height
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($b.Location, [System.Drawing.Point]::Empty, $b.Size)
$path = if ([System.IO.Path]::IsPathRooted($Out)) { $Out } else { Join-Path $root $Out }
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $path) | Out-Null
$bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $bmp.Dispose()
Write-Host "saved $path"

if (-not $ProcessId -and -not $p.HasExited) { $p.Kill(); $p.WaitForExit() }
Write-Host "exit code: $($p.ExitCode)"

