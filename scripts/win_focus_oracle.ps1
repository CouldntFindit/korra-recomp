# win_focus_oracle.ps1 - the Windows oracle for the backgrounding/lifecycle fix.
#
# What it measures: with REAL D3D9, does the engine take its device-lost path
# when the game window stops being usable, and does it survive? The gate probe
# (src/host/gate_probe.c, KORRA_GATE=1) prints the engine's own device-state
# manager once a second; `flag` is [mgr+0x6C], raised by its TestCooperativeLevel
# (sub_00759C90) when the device status is D3DERR_DEVICELOST/DEVICENOTRESET and
# read by the frame-setup gate. So flag=1 while the device is lost and flag=0
# after a successful Reset is the Win32 behaviour the Android bridge has to
# mirror.
#
# The Android side of the proof is scripts\android_lifecycle_test.ps1.
#
#   powershell -ExecutionPolicy Bypass -File scripts\win_focus_oracle.ps1
#   powershell -ExecutionPolicy Bypass -File scripts\win_focus_oracle.ps1 -AwaySeconds 30
#
# It takes the display: the game runs exclusive-fullscreen (the engine's own
# mode), and the process is killed at the end. Run only one game instance.
param(
    [int]$AwaySeconds = 20,
    [int]$BootSeconds = 70,
    [int]$AfterSeconds = 45
)
$ErrorActionPreference = "Continue"
$root = Split-Path -Parent $PSScriptRoot
$workspace = Split-Path (Split-Path $root -Parent) -Parent
$game = Join-Path $workspace "The Legend of Korra"
$logs = Join-Path $root "logs"
New-Item -ItemType Directory -Force -Path $logs | Out-Null
$out = Join-Path $logs "win_focus_oracle.txt"
Remove-Item $out -ErrorAction SilentlyContinue
$exe = Join-Path $root "build\run_focus_oracle.exe"

if (-not (Test-Path (Join-Path $root "build\korra_recomp.exe"))) {
    throw "build\korra_recomp.exe missing - run scripts\build_host.ps1 first"
}
if (-not (Test-Path $game)) { throw "game folder not found: $game" }

Add-Type @"
using System;
using System.Runtime.InteropServices;
public class FWin {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x,int y,int cx,int cy,uint f);
  [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool f);
  [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
  public struct RECT { public int Left, Top, Right, Bottom; }
  public static IntPtr Find(uint pid) { IntPtr best=IntPtr.Zero; int area=0;
    EnumWindows((h,l)=>{ uint wp; GetWindowThreadProcessId(h, out wp); RECT r; GetWindowRect(h, out r);
      int a=(r.Right-r.Left)*(r.Bottom-r.Top); if (wp==pid && a>area) { area=a; best=h; } return true; }, IntPtr.Zero);
    return best; }
  public static void Fore(IntPtr h) {
    uint fgp; GetWindowThreadProcessId(GetForegroundWindow(), out fgp);
    uint tgt; GetWindowThreadProcessId(h, out tgt);
    AttachThreadInput(GetCurrentThreadId(), fgp, true);
    AttachThreadInput(GetCurrentThreadId(), tgt, true);
    ShowWindow(h, 9);
    SetWindowPos(h, new IntPtr(-1), 0,0,0,0, 0x0001|0x0002|0x0040);
    BringWindowToTop(h); SetForegroundWindow(h);
    AttachThreadInput(GetCurrentThreadId(), tgt, false);
    AttachThreadInput(GetCurrentThreadId(), fgp, false);
  }
}
"@
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms

function Note([string]$m) {
    $line = "[{0:HH:mm:ss}] {1}" -f (Get-Date), $m
    Add-Content -Path $out -Value $line -Encoding UTF8
    Write-Host $line
}
function Gate-Lines([string]$log, [int]$n) {
    if (-not (Test-Path $log)) { return @() }
    Select-String -Path $log -Pattern '^\[gate\] m=' | Select-Object -Last $n |
        ForEach-Object { $_.Line }
}
function Shot([string]$name) {
    $vs = [System.Windows.Forms.SystemInformation]::VirtualScreen
    $bmp = New-Object System.Drawing.Bitmap $vs.Width, $vs.Height
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($vs.Left, $vs.Top, 0, 0, (New-Object System.Drawing.Size($vs.Width, $vs.Height)))
    $bmp.Save((Join-Path $logs $name), [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bmp.Dispose()
}

Copy-Item (Join-Path $root "build\korra_recomp.exe") $exe -Force
$env:KORRA_GATE = "1"
$p = Start-Process -FilePath $exe -ArgumentList "`"$game`"" -PassThru `
     -RedirectStandardOutput (Join-Path $root "build\focus.out") `
     -RedirectStandardError (Join-Path $root "build\focus.err")
Note "started $($p.Id) (KORRA_GATE=1), waiting $BootSeconds s for the attract screen"
Start-Sleep -Seconds $BootSeconds
$log = Get-ChildItem $game -Filter "korra_host-*.log" | Sort-Object LastWriteTime |
       Select-Object -Last 1
Note "log: $($log.Name)"
$h = [FWin]::Find([uint32]$p.Id)
Note "hwnd=$h alive=$(-not $p.HasExited)"
Shot "win_focus_1_before.png"
Note ("gate before: " + ((Gate-Lines $log.FullName 3) -join " ;; "))

# Away: minimize the device window. An exclusive-fullscreen D3D9 window that is
# minimized loses its device; a foreground that is really taken away keeps it
# lost until it comes back.
[void][FWin]::ShowWindow($h, 6)   # SW_MINIMIZE
Note "minimized; waiting $AwaySeconds s"
Start-Sleep -Seconds $AwaySeconds
Shot "win_focus_2_away.png"
Note ("alive while away: $(-not $p.HasExited)")
Note ("gate away: " + ((Gate-Lines $log.FullName 6) -join " ;; "))

# Back: restore and foreground, then watch the flag every 5 s. If the restore
# does not take the foreground (foreground != hwnd below), the device stays lost
# and flag stays 1 - still a valid measurement of the engine's lost path.
[void][FWin]::Fore($h)
Note "restored + foreground"
for ($i = 0; $i -lt [Math]::Max(1, [int]($AfterSeconds / 5)); $i++) {
    Start-Sleep -Seconds 5
    Note ("gate +{0}s: {1}" -f (($i + 1) * 5), ((Gate-Lines $log.FullName 1) -join ""))
}
Shot "win_focus_3_after.png"
Note ("alive after restore: $(-not $p.HasExited)")
$fg = [FWin]::GetForegroundWindow()
Note "foreground=$fg target=$h"

if (-not $p.HasExited) { $p.Kill(); Note "killed" } else {
    Note "process exited on its own: code=$($p.ExitCode)"
}
Start-Sleep -Seconds 1
Note ("tail: " + (((Get-Content $log.FullName -Tail 3) -join " ;; ")))
