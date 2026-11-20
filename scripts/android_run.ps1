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
    [string]$GameSource = "C:\Users\yigit\OneDrive\Belgeler\korradecomp\The Legend of Korra",
    [switch]$NoInstall,
    [switch]$NoGameCopy,
    [switch]$NoLogcat
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$sdk = "C:\Users\yigit\OneDrive\Belgeler\korradecomp\android-sdk"
$adb = Join-Path $sdk "platform-tools\adb.exe"
if (-not (Test-Path $adb)) { throw "adb not found at $adb" }

function Adb { & $adb @args; if ($LASTEXITCODE -ne 0) { throw "adb $args failed" } }

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

& $adb logcat -c
& $adb shell am force-stop $Package
& $adb shell am start -n "$Package/.MainActivity" | Out-Host
Start-Sleep -Seconds $Seconds
& $adb shell am force-stop $Package

# log.txt lives in the external files dir, which only the APP writes; reading it
# over adb is allowed, so pull it directly.
$remote = "/sdcard/Android/data/$Package/files/log.txt"
& $adb pull $remote (Join-Path $logs "log.txt") | Select-Object -Last 1 | Out-Host
if (-not $NoLogcat) {
    & $adb logcat -d -s Korra:D DXVK:I AndroidRuntime:E DEBUG:E libc:F "*:S" > (Join-Path $logs "logcat.txt")
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
