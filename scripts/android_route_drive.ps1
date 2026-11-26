# OCR-driven touch route: walks the game from the attract screen into the New
# Game intro cutscene, then drives ACTIVE play in the first area while sampling
# thermals from the PC side.
#
# Why not a fixed timeline: the engine polls XInput slowly, so a 0 ms `input tap`
# is usually missed (a press must hold ~1 s), and a missed press shifts every
# later step. Worse, the exit dialog's default is NO and the main menu's last
# item is Exit, so a blind "down + A" can walk into "Do you really want to exit
# the game?" (that is exactly what the save session's screenshots show). This
# driver reads the screen with Windows' built-in OCR (Windows.Media.Ocr) and
# decides each press from what is actually displayed.
#
# Once the tutorial HUD is on screen the driver stops reading every step and
# cycles held-stick swipes and button holds back to back for -PlaySeconds, with a
# screencap/OCR every -PlayShotSeconds to prove the state is still gameplay and a
# battery/thermalservice dump every -ThermalSeconds. The fps numbers come from
# the in-app probe (`setprop debug.korra.perf 1`), not from here.
#
#   powershell -ExecutionPolicy Bypass -File scripts\android_route_drive.ps1 -Seconds 900 -PlaySeconds 360 -Install
param(
    [int]$Seconds = 600,
    [int]$PlaySeconds = 330,
    [int]$PlayShotSeconds = 25,
    [int]$ThermalSeconds = 30,
    # The port's frame cap (debug.korra.fps): 30 = the port default, 0 = the
    # engine's own 30/60 mode, 60 = the Windows-equivalent target.
    [int]$Fps = 30,
    [string]$Package = "org.korra.recomp",
    [string]$Prefix = "drive",
    [switch]$NoInstall,
    [switch]$Install,
    # Continue (the Story submenu's default entry) instead of New Game. The
    # checkpoint load rewrites GameData.dat, so the save probe shows the load's
    # own request; use it to prove the written save resumes.
    [switch]$Resume
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$sdk = if ($env:ANDROID_HOME) { $env:ANDROID_HOME }
       elseif ($env:ANDROID_SDK_ROOT) { $env:ANDROID_SDK_ROOT }
       else { Join-Path (Split-Path (Split-Path $root -Parent) -Parent) "android-sdk" }
$adb = Join-Path $sdk "platform-tools\adb.exe"
if (-not (Test-Path $adb)) { throw "adb not found at $adb" }
$logs = Join-Path $root "logs"
New-Item -ItemType Directory -Force -Path $logs | Out-Null
$driveLog = Join-Path $logs "route_drive.txt"
Remove-Item $driveLog -ErrorAction SilentlyContinue

function Adb-Run {
    # Takes an ARRAY: a native adb call through & with an array never parses
    # `-p`/`-d`/`-s` as PowerShell parameters (a function's remaining-args
    # parameter does, and that silently dropped `screencap -p` once).
    param([string[]]$argv)
    $prev = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try { & $adb @argv 2>&1 } finally { $ErrorActionPreference = $prev }
}

# ---- OCR (Windows.Media.Ocr) --------------------------------------------
Add-Type -AssemblyName System.Runtime.WindowsRuntime
$asTaskGeneric = ([System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
    $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and
    $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1' })[0]
function Await($op, $type) {
    $m = $asTaskGeneric.MakeGenericMethod($type)
    $t = $m.Invoke($null, @($op))
    $t.Wait(-1) | Out-Null
    $t.Result
}
[Windows.Storage.StorageFile, Windows.Storage, ContentType = WindowsRuntime] | Out-Null
[Windows.Graphics.Imaging.BitmapDecoder, Windows.Graphics, ContentType = WindowsRuntime] | Out-Null
[Windows.Media.Ocr.OcrEngine, Windows.Foundation, ContentType = WindowsRuntime] | Out-Null
$ocr = [Windows.Media.Ocr.OcrEngine]::TryCreateFromUserProfileLanguages()
if (-not $ocr) { throw "no Windows OCR engine available" }
function Get-ScreenText([string]$path) {
    # The WinRT stream/decoder/bitmap are disposable native objects; a loop that
    # never releases them faulted after ~30 iterations (2026-11-22). Retry with
    # a fresh engine rather than killing a ten-minute run over one bad frame.
    for ($try = 0; $try -lt 3; $try++) {
        $stream = $null
        $bitmap = $null
        try {
            $file = Await ([Windows.Storage.StorageFile]::GetFileFromPathAsync($path)) ([Windows.Storage.StorageFile])
            $stream = Await ($file.OpenAsync([Windows.Storage.FileAccessMode]::Read)) ([Windows.Storage.Streams.IRandomAccessStream])
            $decoder = Await ([Windows.Graphics.Imaging.BitmapDecoder]::CreateAsync($stream)) ([Windows.Graphics.Imaging.BitmapDecoder])
            $bitmap = Await ($decoder.GetSoftwareBitmapAsync()) ([Windows.Graphics.Imaging.SoftwareBitmap])
            $res = Await ($script:ocr.RecognizeAsync($bitmap)) ([Windows.Media.Ocr.OcrResult])
            $text = ($res.Lines | ForEach-Object { $_.Text }) -join " | "
            $bitmap.Dispose()
            $stream.Dispose()
            return $text
        } catch {
            if ($bitmap) { try { $bitmap.Dispose() } catch { } }
            if ($stream) { try { $stream.Dispose() } catch { } }
            $script:ocr = [Windows.Media.Ocr.OcrEngine]::TryCreateFromUserProfileLanguages()
            Start-Sleep -Milliseconds 500
        }
    }
    return ""
}

# ---- gestures -----------------------------------------------------------
# Every press is a 1.1 s hold at one point: a swipe with identical start and
# end is a long press, and one process carries the whole gesture.
function Press-A { Adb-Run @('shell', 'input', 'swipe', '1170', '540', '1170', '540', '1100') | Out-Null }
function Stick-Down { Adb-Run @('shell', 'input', 'swipe', '421', '540', '421', '840', '1500') | Out-Null }
function Stick-Up { Adb-Run @('shell', 'input', 'swipe', '421', '800', '421', '500', '1500') | Out-Null }
function Stick-Left { Adb-Run @('shell', 'input', 'swipe', '585', '700', '120', '700', '1500') | Out-Null }

# ---- in-game play gestures ----------------------------------------------
# Zones from src/host_android/input_android.c, landscape 2340x1080:
#   left third, lower two thirds   left stick (origin = where the GRAB lands)
#   right third, lower two thirds  right stick (camera)
#   top-right 2x2 cluster          Y north-west, B north-east, X south-west,
#                                  A south-east
# A hold has to outlast the engine's own ~1 s poll, so sticks are 1400 ms swipes
# and buttons 1100-1300 ms holds (a 0 ms tap is usually missed).
function Play-Right { Adb-Run @('shell', 'input', 'swipe', '300', '800', '760', '800', '1400') | Out-Null }
function Play-Left  { Adb-Run @('shell', 'input', 'swipe', '760', '800', '300', '800', '1400') | Out-Null }
function Play-Up    { Adb-Run @('shell', 'input', 'swipe', '420', '880', '420', '560', '1400') | Out-Null }
function Play-Down  { Adb-Run @('shell', 'input', 'swipe', '420', '560', '420', '880', '1400') | Out-Null }
function Play-A     { Adb-Run @('shell', 'input', 'swipe', '2180', '380', '2180', '380', '1200') | Out-Null }
function Play-X     { Adb-Run @('shell', 'input', 'swipe', '1900', '380', '1900', '380', '1200') | Out-Null }
function Play-Y     { Adb-Run @('shell', 'input', 'swipe', '1900', '160', '1900', '160', '1200') | Out-Null }
function Play-B     { Adb-Run @('shell', 'input', 'swipe', '2180', '160', '2180', '160', '1200') | Out-Null }

# One step of the play cycle: movement, then a bit of combat, then camera. The
# order is fixed so two runs are comparable, and every gesture is held long
# enough for a slow poll to see it.
function Invoke-PlayStep([int]$i) {
    switch ($i % 10) {
        0 { Play-Right }
        1 { Play-X }
        2 { Play-Right }
        3 { Play-A }
        4 { Play-Up }
        5 { Play-Y }
        6 { Play-Left }
        7 { Play-X }
        8 { Play-Down }
        9 { Play-B }
    }
}

$shotN = 0
$scanPath = Join-Path $logs "_scan.png"
function Scan-Shot {
    Adb-Run @('shell', 'screencap', '-p', '/sdcard/korra_drive.png') | Out-Null
    Adb-Run @('pull', '/sdcard/korra_drive.png', $scanPath) | Out-Null
    if (-not (Test-Path $scanPath)) { throw "screencap produced no file (check the adb call)" }
    return $scanPath
}
function Keep-Shot([string]$tag) {
    $script:shotN++
    $local = Join-Path $logs ("{0}_{1:D2}_{2}.png" -f $Prefix, $script:shotN, $tag)
    Copy-Item $scanPath $local -Force
    return $local
}

function Note([string]$msg) {
    $line = "[{0,4:N0}s] {1}" -f $script:elapsed, $msg
    Add-Content -Path $driveLog -Value $line -Encoding UTF8
    Write-Host $line
}

# ---- run ----------------------------------------------------------------
$apkPath = Join-Path $root "android\app\build\outputs\apk\debug\app-debug.apk"
if ($Install -and -not $NoInstall) {
    if (-not (Test-Path $apkPath)) { throw "APK not found: $apkPath" }
    Adb-Run @('install', '-r', $apkPath) | Out-Host
}
& $adb shell svc power stayon true | Out-Null
& $adb shell settings put system screen_off_timeout 1800000 | Out-Null
& $adb shell input keyevent KEYCODE_WAKEUP | Out-Null
& $adb shell wm dismiss-keyguard | Out-Null
# The probes this run is measured with. A run's log must say what it was asked
# for, so the values are read back, not assumed.
& $adb shell setprop debug.korra.memtrace 1 | Out-Null
& $adb shell setprop debug.korra.perf 1 | Out-Null
& $adb shell setprop debug.korra.fps $Fps | Out-Null
Start-Sleep -Milliseconds 300
$fpsRead = ((Adb-Run @('shell', 'getprop', 'debug.korra.fps')) -join "").Trim()
Note ("props: memtrace=1 perf=1 fps={0}" -f $fpsRead)
& $adb shell am force-stop $Package | Out-Null
Start-Sleep -Seconds 1
& $adb shell am start -n "$Package/.MainActivity" | Out-Null
# `am start` can print "unknown error code 10" while still launching, and the
# process needs a moment to appear; checking pidof in the same breath read the
# app as dead at 0 s (2026-11-26).
Start-Sleep -Seconds 3
$t0 = Get-Date
$script:elapsed = 0
$lastState = ""
$cooldownUntil = 0
$lastText = ""
$submenuTries = 0
$nextMeminfo = 0
$nextHeartbeat = 0
$nextThermal = 0
$reachedDifficulty = $false

# route = read the screen and decide a press; play = cycle held gestures in the
# first area (the campaign), no OCR per step; done = campaign finished.
$mode = "route"
$playT0 = 0
$playIndex = 0
$playShots = 0
$nextPlayShot = 0

$died = $false

while (((Get-Date) - $t0).TotalSeconds -lt $Seconds) {
    $script:elapsed = ((Get-Date) - $t0).TotalSeconds
    $pidof = ((Adb-Run @('shell', 'pidof', $Package)) -join "").Trim()
    if (-not $pidof -and $script:elapsed -gt 3) { $died = $true; Note "process ended (pidof empty)"; break }

    # Thermal curve from the PC side: dumpsys battery (temperature, level,
    # charging) and thermalservice (per-sensor severity/status), every 30 s.
    if ($script:elapsed -ge $nextThermal) {
        $therm = Join-Path $logs "thermals.txt"
        "--- {0:N0}s t={1:HH:mm:ss} ---" -f $script:elapsed, (Get-Date) |
            Out-File -Append -FilePath $therm -Encoding ascii
        Adb-Run @('shell', 'dumpsys', 'battery') |
            Out-File -Append -FilePath $therm -Encoding ascii
        Adb-Run @('shell', 'dumpsys', 'thermalservice') |
            Out-File -Append -FilePath $therm -Encoding ascii
        $nextThermal = $script:elapsed + $ThermalSeconds
    }

    if ($script:elapsed -ge $nextMeminfo) {
        "--- {0:N0}s ---" -f $script:elapsed | Out-File -Append -FilePath (Join-Path $logs "meminfo.txt") -Encoding ascii
        Adb-Run @('shell', 'dumpsys', 'meminfo', $Package) | Out-File -Append -FilePath (Join-Path $logs "meminfo.txt") -Encoding ascii
        $nextMeminfo = $script:elapsed + 60
    }

    if ($mode -eq "play") {
        if (($script:elapsed - $playT0) -ge $PlaySeconds) {
            Note ("play campaign complete: {0:N0}s active, {1} gestures, {2} shots" -f `
                  ($script:elapsed - $playT0), $playIndex, $playShots)
            $mode = "done"
            break
        }
        Invoke-PlayStep $playIndex
        $playIndex++
        # Every PlayShotSeconds: prove the state is still gameplay (a dialogue,
        # menu or death screen would mean the input is not reaching the player)
        # and keep a frame for the move/advance evidence.
        if ($script:elapsed -ge $nextPlayShot) {
            $shot = Scan-Shot
            $text = Get-ScreenText $shot
            $flat = ($text.ToLowerInvariant() -replace '[^a-z0-9]', '')
            $playShots++
            $kept = Keep-Shot ("play{0:D2}" -f $playShots)
            Note ("play {0:N0}s gesture={1} shot={2} text={3}" -f `
                  ($script:elapsed - $playT0), $playIndex, (Split-Path -Leaf $kept),
                  ($text.Substring(0, [Math]::Min(140, $text.Length))))
            $nextPlayShot = $script:elapsed + $PlayShotSeconds
            if ($flat -match "reallywanttoexit|unsavedprogress|newgame|probending|difficulty") {
                Note "play: left gameplay (menu/dialog) - back to the route"
                $mode = "route"
                $lastState = ""
            }
        }
        continue
    }

    if ($script:elapsed -lt $cooldownUntil) { Start-Sleep -Milliseconds 700; continue }

    $shot = Scan-Shot
    $text = Get-ScreenText $shot
    # OCR mangles the logo fonts ("PRESS ANY BUTTON" comes back as
    # "PK ESS ANY BUT TON"), so classify on a letters-and-digits-only copy.
    $flat = ($text.ToLowerInvariant() -replace '[^a-z0-9]', '')

    $state = "unknown"
    if ($flat -match "pressany|anybutt") { $state = "attract" }
    # The lock screen is not the game: its clock/carrier text was read as
    # gameplay input for 190 s in one run (2026-11-22) while the driver pressed
    # A at it. Dismiss it and wait for the app's own first frame.
    elseif ($flat -match "untilfull|weekendbrief|swipeup|vodafone|turkcell|t[0-9]rksel") { $state = "lockscreen" }
    elseif ($flat -match "autosave|savemdi") { $state = "autosave-notice" }
    elseif ($flat -match "stronglyrecommended|xbox360controller") { $state = "pad-notice" }
    elseif ($flat -match "reallywanttoexit|unsavedprogress") { $state = "exit-confirm" }
    elseif ($flat -match "resetyourgameprogress|challengesyour") { $state = "newgame-confirm" }
    # The tutorial area's control HUD: "Light Attack / Heavy Attack / Jump".
    elseif ($flat -match "lightattack|heavyattack|jump") { $state = "gameplay" }
    # Main menu: the Story entry plus its siblings. OCR mangles the logo font,
    # so "Pro-Bending" comes back as "Pro-Bendin" and "Options" as "pptions".
    # OCR mangles "Start Story Mode." into "Sta Story Mode"/"Start Story Model";
    # the flattened copy still keeps "storymode", so match that too.
    elseif ($flat -match "startstorymode|storymode") { $state = "main-menu-story" }
    elseif ($flat -match "probendin|pptions|options") { $state = "main-menu" }
    # Story submenu: Continue / New Game / END. Do NOT require "Continue": it
    # reads as "Cpntinue" about half the time (measured 2026-11-22, the route
    # then sat on this screen for 200 s), while "New Game" survives.
    elseif ($flat -match "newgame|startanewgame|stoay") { $state = "story-submenu" }
    elseif ($flat -match "casual|extreme|difficulty") { $state = "difficulty" }
    elseif ($flat.Length -lt 3) { $state = "blank" }

    if ($state -ne $lastState) {
        $kept = Keep-Shot $state
        Note ("state={0} shot={1} text={2}" -f $state, (Split-Path -Leaf $kept), ($text.Substring(0, [Math]::Min(160, $text.Length))))
        $lastState = $state
    } elseif ($script:elapsed -ge $nextHeartbeat) {
        Note ("hold: state={0} text={1}" -f $state, ($text.Substring(0, [Math]::Min(100, $text.Length))))
        $nextHeartbeat = $script:elapsed + 20
    }
    $lastText = $text

    switch ($state) {
        "lockscreen"      { Note "act: dismiss keyguard"; Adb-Run @('shell', 'wm', 'dismiss-keyguard') | Out-Null; Adb-Run @('shell', 'input', 'keyevent', 'KEYCODE_MENU') | Out-Null }
        "attract"         { Note "act: A (leave attract)"; Press-A }
        "autosave-notice" { Note "act: A (dismiss auto-save notice)"; Press-A }
        "pad-notice"      { Note "act: A (dismiss pad notice)"; Press-A }
        "main-menu-story" { Note "act: A (open Story)"; Press-A }
        # The main menu opens with Story selected. Pressing up here wraps the
        # cursor to Exit (the last item), and repeating it cycles forever while
        # the OCR keeps the state at "main-menu" - measured 2026-11-26, the
        # sauto1 run spent 60 s doing that. Confirm the default instead.
        "main-menu"       { Note "act: A (Story is the default entry)"; Press-A }
        "story-submenu"   {
            if ($Resume) { Note "act: A (Continue is the default entry)"; Press-A; $submenuTries = 0 }
            elseif ($text -match "start a new game") { Note "act: A (New Game)"; Press-A; $submenuTries = 0 }
            elseif ($submenuTries -ge 3) { Note "act: A anyway (no description read)"; Press-A; $submenuTries = 0 }
            else { Note ("act: stick down (Continue -> New Game), try {0}" -f ($submenuTries + 1)); Stick-Down; $submenuTries++ }
        }
        "newgame-confirm" { Note "act: stick left (NO -> YES) then A"; Stick-Left; Start-Sleep -Milliseconds 400; Press-A }
        "difficulty"      { Note "act: A (confirm difficulty)"; $reachedDifficulty = $true; Press-A }
        "gameplay"        {
            Note "act: HUD read - start the active-play campaign"
            $mode = "play"
            $playT0 = $script:elapsed
            $nextPlayShot = $script:elapsed + 5
            $playIndex = 0
        }
        "exit-confirm"    {
            # The default is NO: A cancels. Never send LEFT here.
            Note "act: A (NO - cancel the exit dialog)"
            Press-A
        }
        default           {
            # After the difficulty screen the intro cutscene runs ~85 s. If the
            # OCR never reads the HUD, start the campaign at 240 s anyway: by
            # then the route has been through the cutscene on every run so far.
            if ($reachedDifficulty -and $script:elapsed -gt 240) {
                Note "act: HUD not read after the cutscene - starting the campaign anyway"
                $mode = "play"
                $playT0 = $script:elapsed
                $nextPlayShot = $script:elapsed + 5
                $playIndex = 0
            } else {
                Start-Sleep -Milliseconds 900
            }
        }
    }
    $cooldownUntil = $script:elapsed + 2.6
}

if (-not $died) { Note ("still alive at {0:N0}s" -f ((Get-Date) - $t0).TotalSeconds) }

# Interactivity evidence: two shots with NO input, then two while the stick is
# held right, so the idle-vs-move difference can be measured instead of argued.
if ($playShots -gt 0 -and -not $died) {
    Note "evidence: idle 2 shots, then held-stick 2 shots"
    Start-Sleep -Seconds 3
    Scan-Shot | Out-Null; Note ("idle A {0}" -f (Split-Path -Leaf (Keep-Shot "idleA")))
    Start-Sleep -Seconds 3
    Scan-Shot | Out-Null; Note ("idle B {0}" -f (Split-Path -Leaf (Keep-Shot "idleB")))
    Play-Right
    Scan-Shot | Out-Null; Note ("move A {0}" -f (Split-Path -Leaf (Keep-Shot "moveA")))
    Play-Right
    Scan-Shot | Out-Null; Note ("move B {0}" -f (Split-Path -Leaf (Keep-Shot "moveB")))
}

$final = Keep-Shot "final"
Note ("final screenshot {0}" -f $final)

# ---- artifacts ----------------------------------------------------------
# End the run deliberately so exit-info says reason=10 (USER REQUESTED) and not
# a speculative LMK kill; the crash buffer is pulled right after.
Adb-Run @('shell', 'am', 'force-stop', $Package) | Out-Null
Start-Sleep -Milliseconds 800
Adb-Run @('pull', "/sdcard/Android/data/$Package/files/log.txt", (Join-Path $logs "log.txt")) | Out-Null
# The frame probe's own channel (src/host_android/frame_probe_android.c): it
# keeps writing even if the shell's log pump stops, which happened once mid-run.
Adb-Run @('pull', "/sdcard/Android/data/$Package/files/perf.txt", (Join-Path $logs "perf.txt")) | Out-Null
Adb-Run @('logcat', '-d', '-s', 'Korra:D', 'DXVK:I', 'AndroidRuntime:E', 'DEBUG:E', 'libc:F', '*:S') |
    Out-File -FilePath (Join-Path $logs "logcat.txt") -Encoding ascii
Adb-Run @('logcat', '-b', 'crash', '-d') | Out-File -FilePath (Join-Path $logs "crash.txt") -Encoding ascii
Adb-Run @('shell', 'dumpsys', 'activity', 'exit-info', $Package) |
    Out-File -FilePath (Join-Path $logs "exit-info.txt") -Encoding ascii
Note "artifacts pulled"
