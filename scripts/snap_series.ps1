# Start the host, screenshot at several times, so a still frame can be told from
# a progressing boot.
#   powershell -ExecutionPolicy Bypass -File scripts\snap_series.ps1 -Seconds "6,15,30"
param(
    [string]$Times = "6,15,30",
    [string]$Game = ""
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
}
"@
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root "build\korra_recomp.exe"
if (-not $Game) { $Game = Join-Path (Split-Path -Parent (Split-Path -Parent $root)) "The Legend of Korra" }
New-Item -ItemType Directory -Force -Path (Join-Path $root "build\shots") | Out-Null

$p = Start-Process -FilePath $exe -ArgumentList "`"$Game`"" -PassThru `
     -RedirectStandardOutput (Join-Path $root "build\run.out") -RedirectStandardError (Join-Path $root "build\run.err")

$last = 0
foreach ($t in ($Times -split "," | ForEach-Object { [int]$_ })) {
    Start-Sleep -Seconds ($t - $last); $last = $t
    if ($p.HasExited) { Write-Host "process exited at ${t}s (code $($p.ExitCode))"; break }
    $wins = [W]::Windows([uint32]$p.Id)
    if ($wins.Count -eq 0) { Write-Host "${t}s: no window"; continue }
    $r = New-Object W+RECT
    [void][W]::GetWindowRect($wins[0], [ref]$r)
    $b = [System.Drawing.Rectangle]::FromLTRB($r.Left, $r.Top, $r.Right, $r.Bottom)
    $bmp = New-Object System.Drawing.Bitmap $b.Width, $b.Height
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($b.Location, [System.Drawing.Point]::Empty, $b.Size)
    $path = Join-Path $root ("build\shots\{0:D2}s.png" -f $t)
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bmp.Dispose()
    Write-Host "${t}s: $path"
}
if (-not $p.HasExited) { $p.Kill(); $p.WaitForExit() }
Write-Host "exit code: $($p.ExitCode)"
