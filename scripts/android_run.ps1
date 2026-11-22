# Installs the debug APK, makes sure the game tree is in the app's INTERNAL
# storage, starts the game, waits, then pulls log.txt, logcat and the exit
# record. The whole device loop in one command:
#
#   powershell -ExecutionPolicy Bypass -File scripts\android_run.ps1 -Seconds 30
#
# Why internal storage: on Android 16+ the app cannot open files that adb
# pushed into its own external files dir (EACCES), so the dev install lives in
# /data/user/0/<pkg>/files/game and is copied there with `run-as`. The SAF
# picker remains the user-facing path.
#
# The game tree is expected in $GameSource (default: the local install root)
# and is pushed to /data/local/tmp once, then copied with run-as.

param(
    [int]$Seconds = 30,
    [string]$Apk = "android\app\build\outputs\apk\debug\app-debug.apk",
    [string]$Package = "org.korra.recomp",
    [string]$GameSource = "<workspace>\The Legend of Korra",
    [switch]$NoInstall,
    [switch]$NoGameCopy,
    [switch]$NoLogcat,
    # Let the process die on its own: no force-stop at the end, so the crash
    # buffer and the exit record describe what the app did, not what we did.
    [switch]$NoStop,
    # PC-side RSS sampler: dumpsys meminfo every $MeminfoSeconds while the run
    # lasts, so the trajectory survives a hard death (in-app sampler optional).
    [switch]$Meminfo,
    [int]$MeminfoSeconds = 2,
    # Replay the touch route that reaches gameplay (see docs/FINDINGS.md
    # 2026-11-22 save entry). newgame: title -> notice -> menu -> Story ->
    # New Game -> confirm NO->YES -> difficulty -> intro cutscene.
    [ValidateSet("", "newgame", "continue")]
    [string]$Route = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$sdk = "<workspace>\android-sdk"
$adb = Join-Path $sdk "platform-tools\adb.exe"
if (-not (Test-Path $adb)) { throw "adb not found at $adb" }

function Adb { & $adb @args; if ($LASTEXITCODE -ne 0) { throw "adb $args failed" } }

# adb writes transfer progress to stderr; under PowerShell 5.1's EAP=Stop that
# surfaces as a NativeCommandError even when the command succeeded (a route
# screenshot pull killed a whole run once). Soft calls keep the output they are
# asked for and drop the noise.
function Adb-Soft {
    param([Parameter(ValueFromRemainingArguments = $true)]$argv)
    $prev = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try { & $adb @argv 2>&1 } finally { $ErrorActionPreference = $prev }
}

$devices = (& $adb devices) -split "`n" | Where-Object { $_ -match "\tdevice$" }
if (-not $devices) { throw "no device attached (adb devices is empty); connect the phone and retry" }

$apkPath = if ([System.IO.Path]::IsPathRooted($Apk)) { $Apk } else { Join-Path $root $Apk }
if (-not $NoInstall) {
    if (-not (Test-Path $apkPath)) { throw "APK not found: $apkPath" }
    & $adb install -r $apkPath
    if ($LASTEXITCODE -ne 0) { throw "adb install failed" }
}

# --- game tree: push to /data/local/tmp (world readable), then run-as copy ---
if (-not $NoGameCopy) {
    $hasGame = (& $adb shell "run-as $Package sh -c 'test -f files/game/GameData/main.data && echo yes || echo no'" 2>$null) -join ""
    if ($hasGame -notmatch "yes") {
        Write-Host "game tree: copying into internal storage (first run, ~3 GB)"
        & $adb shell "mkdir -p /data/local/tmp/korra"
        foreach ($item in @("LoK.exe", "GameData")) {
            $src = Join-Path $GameSource $item
            if (-not (Test-Path $src)) { throw "missing in game source: $src" }
            $remote = & $adb shell "run-as $Package sh -c 'test -e files/game/$item && echo yes || echo no'" 2>$null
            if (($remote -join "") -match "yes") { Write-Host "  $item already installed"; continue }
            Write-Host "  pushing $item"
            & $adb push $src "/data/local/tmp/korra/$item" | Select-Object -Last 1 | Out-Host
            & $adb shell "run-as $Package sh -c 'mkdir -p files/game && cp -r /data/local/tmp/korra/$item files/game/'"
            if ($LASTEXITCODE -ne 0) { throw "run-as copy of $item failed" }
        }
        & $adb shell "run-as $Package sh -c 'ls -l files/game; du -sh files/game/GameData'" | Out-Host
    } else {
        Write-Host "game tree: already installed"
    }
}

$logs = Join-Path $root "logs"
New-Item -ItemType Directory -Force -Path $logs | Out-Null
$memPath = Join-Path $logs "meminfo.txt"
if ($Meminfo) { Remove-Item $memPath -ErrorAction SilentlyContinue }
$startedAt = Get-Date

& $adb logcat -c
# The activity pauses (and the guest stops) the moment the screen goes off, so
# a run must keep the screen on for its whole length.
& $adb shell svc power stayon true | Out-Null
& $adb shell input keyevent KEYCODE_WAKEUP | Out-Null
& $adb shell settings put system screen_off_timeout 1800000 | Out-Null
& $adb shell am force-stop $Package
& $adb shell am start -n "$Package/.MainActivity" | Out-Host

# Wait, sampling RSS from the PC side and watching for the process to end. A
# hard death must end the wait immediately: pulling logcat after a timeout
# alone is how the r11 death lost its abort message (the pull ended 5 min
# before the SIGABRT).
# --- optional touch route -------------------------------------------------
# Coordinates are landscape (2340x1080). A tap anywhere that is not a stick or
# the button cluster is A (input_android.c); the stick is a drag inside the
# left third, lower two thirds. Timings match the save session's screenshots
# (logs\devsave3_*.png) so a replay can be diffed against that run.
$routeSteps = @()
if ($Route -eq "newgame") {
    $routeSteps = @(
        @{ t = 44;  k = "shot"; a = "01_pre" },
        @{ t = 45;  k = "tap";  a = "1170 540" },
        @{ t = 47;  k = "shot"; a = "02_titleA" },
        @{ t = 51;  k = "tap";  a = "1170 540" },
        @{ t = 53;  k = "shot"; a = "03_noticeA" },
        @{ t = 57;  k = "tap";  a = "1170 540" },
        @{ t = 59;  k = "shot"; a = "04_menuA" },
        @{ t = 63;  k = "tap";  a = "1170 540" },
        @{ t = 65;  k = "shot"; a = "05_storyA" },
        @{ t = 70;  k = "down"; a = "" },
        @{ t = 72;  k = "shot"; a = "06_down" },
        @{ t = 75;  k = "tap";  a = "1170 540" },
        @{ t = 77;  k = "shot"; a = "07_newgameA" },
        @{ t = 81;  k = "left"; a = "" },
        @{ t = 83;  k = "shot"; a = "08_left" },
        @{ t = 86;  k = "tap";  a = "1170 540" },
        @{ t = 88;  k = "shot"; a = "09_diffA" },
        @{ t = 92;  k = "tap";  a = "1170 540" },
        @{ t = 94;  k = "shot"; a = "10_casualA" },
        @{ t = 105; k = "shot"; a = "11_cutscene" },
        @{ t = 125; k = "shot"; a = "12_cutscene" },
        @{ t = 145; k = "shot"; a = "13_postcut" },
        @{ t = 170; k = "shot"; a = "14_postcut" },
        @{ t = 200; k = "shot"; a = "15_postcut" },
        @{ t = 240; k = "shot"; a = "16_postcut" },
        @{ t = 300; k = "shot"; a = "17_postcut" },
        @{ t = 380; k = "shot"; a = "18_postcut" }
    )
} elseif ($Route -eq "continue") {
    $routeSteps = @(
        @{ t = 44;  k = "shot"; a = "01_pre" },
        @{ t = 45;  k = "tap";  a = "1170 540" },
        @{ t = 51;  k = "tap";  a = "1170 540" },
        @{ t = 57;  k = "tap";  a = "1170 540" },
        @{ t = 63;  k = "tap";  a = "1170 540" },
        @{ t = 68;  k = "shot"; a = "02_continueA" },
        @{ t = 90;  k = "shot"; a = "03_area" },
        @{ t = 130; k = "shot"; a = "04_area" },
        @{ t = 200; k = "shot"; a = "05_area" },
        @{ t = 300; k = "shot"; a = "06_area" },
        @{ t = 380; k = "shot"; a = "07_area" }
    )
}
function Invoke-RouteStep($step) {
    switch ($step.k) {
        "tap"  { $xy = $step.a -split " "; & $adb shell input tap $xy[0] $xy[1] | Out-Null }
        # A held stick deflection: DOWN at the anchor, MOVE, hold, UP. One
        # `input` process per event keeps the pointer id stable (same virtual
        # touchscreen device), which is what the engine's level signal needs.
        "down" {
            & $adb shell input motionevent DOWN 421 594 | Out-Null
            & $adb shell input motionevent MOVE 421 778 | Out-Null
            Start-Sleep -Milliseconds 1200
            & $adb shell input motionevent UP 421 778 | Out-Null
        }
        "left" {
            & $adb shell input motionevent DOWN 515 670 | Out-Null
            & $adb shell input motionevent MOVE 140 670 | Out-Null
            Start-Sleep -Milliseconds 1200
            & $adb shell input motionevent UP 140 670 | Out-Null
        }
        "shot" {
            $local = Join-Path $logs ("route_" + $step.a + ".png")
            Adb-Soft shell screencap -p /sdcard/korra_route.png | Out-Null
            Adb-Soft pull /sdcard/korra_route.png $local | Out-Null
            Write-Host ("route: shot {0}" -f $step.a)
        }
    }
}

$deadline = (Get-Date).AddSeconds($Seconds)
$died = $false
while ((Get-Date) -lt $deadline) {
    $elapsed = ((Get-Date) - $startedAt).TotalSeconds
    foreach ($step in $routeSteps) {
        if (-not $step.done -and $elapsed -ge $step.t) {
            $step.done = $true
            if (-not $died) { Invoke-RouteStep $step }
        }
    }
    Start-Sleep -Seconds ([Math]::Max(1, $MeminfoSeconds))
    $now = Get-Date
    if ($Meminfo) {
        "--- {0:N0}s t={1:HH:mm:ss} ---" -f ($now - $startedAt).TotalSeconds, $now |
            Out-File -Append -FilePath $memPath -Encoding ascii
        Adb-Soft shell dumpsys meminfo $Package |
            Out-File -Append -FilePath $memPath -Encoding ascii
    }
    $pidof = ((Adb-Soft shell pidof $Package) -join "").Trim()
    if (-not $pidof) {
        $died = $true
        Write-Host ("process ended at t={0:N0}s (pidof empty)" -f ($now - $startedAt).TotalSeconds)
        break
    }
}
if (-not $died) { Write-Host ("still alive at t={0:N0}s" -f ((Get-Date) - $startedAt).TotalSeconds) }
if (-not $NoStop) { & $adb shell am force-stop $Package }

# log.txt lives in the external files dir, which only the APP writes; reading it
# over adb is allowed, so pull it directly.
$remote = "/sdcard/Android/data/$Package/files/log.txt"
Adb-Soft pull $remote (Join-Path $logs "log.txt") | Out-Null
if (-not $NoLogcat) {
    & $adb logcat -d -s Korra:D DXVK:I AndroidRuntime:E DEBUG:E libc:F "*:S" > (Join-Path $logs "logcat.txt")
    # The abort message + backtrace live in the crash buffer only; without this
    # pull a SIGABRT has no frames at all (the guest [crash] handler does not
    # cover SIGABRT by design: an abort is a host-side death).
    & $adb logcat -b crash -d > (Join-Path $logs "crash.txt")
    & $adb shell dumpsys activity exit-info $Package > (Join-Path $logs "exit-info.txt")
}

Write-Host ""
Write-Host "--- log.txt (tail) ---"
if (Test-Path (Join-Path $logs "log.txt")) { Get-Content (Join-Path $logs "log.txt") -Tail 80 }
Write-Host ""
if (-not $NoLogcat) {
    Write-Host "--- logcat Korra/DXVK (tail) ---"
    Get-Content (Join-Path $logs "logcat.txt") -Tail 30
    Write-Host ""
    Write-Host "--- exit info ---"
    Get-Content (Join-Path $logs "exit-info.txt") -Tail 10
}
