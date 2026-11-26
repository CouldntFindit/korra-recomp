# android_lifecycle_test.ps1 - HOME / resume test for the Backgrounding session.
#
# Drives the game from the attract screen into gameplay (OCR-driven, same
# classifier as android_route_drive.ps1), then optionally opens the in-game
# pause menu with START, sends HOME, waits, and re-launches the activity.
# Screenshots, the in-app log, logcat (tag Korra + the crash buffer) and the
# exit reason are written under logs\ with the -Prefix tag.
#
#   powershell -ExecutionPolicy Bypass -File scripts\android_lifecycle_test.ps1 `
#       -Prefix life1 -Install -HomeSeconds 15
#
# Why OCR: the port's XInput path polls slowly, a 0 ms `input tap` is missed,
# and a missed press shifts every later step (see android_route_drive.ps1).
param(
    [string]$Prefix = "life",
    [string]$Package = "org.korra.recomp",
    [int]$Seconds = 420,          # route budget before giving up
    [int]$HomeSeconds = 15,       # time spent on the launcher between HOME and resume
    [int]$ResumeSeconds = 120,    # budget to get back to a game screen after resume
    [switch]$Install,
    [switch]$NoInstall,
    [switch]$PauseBefore,         # open the pause menu (START) before HOME
    [switch]$SkipHome,            # route + pause only (control run)
    [switch]$RunProbe             # set debug.korra.wsi/lifecycle/watch on device
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
$driveLog = Join-Path $logs "$Prefix`_console.txt"
Remove-Item $driveLog -ErrorAction SilentlyContinue

function Adb-Run {
    param([string[]]$argv)
    $prev = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try { & $adb @argv 2>&1 } finally { $ErrorActionPreference = $prev }
}

# ---- OCR (Windows.Media.Ocr), same block as android_route_drive.ps1 -------
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
    for ($try = 0; $try -lt 3; $try++) {
        $stream = $null; $bitmap = $null
        try {
            $file = Await ([Windows.Storage.StorageFile]::GetFileFromPathAsync($path)) ([Windows.Storage.StorageFile])
            $stream = Await ($file.OpenAsync([Windows.Storage.FileAccessMode]::Read)) ([Windows.Storage.Streams.IRandomAccessStream])
            $decoder = Await ([Windows.Graphics.Imaging.BitmapDecoder]::CreateAsync($stream)) ([Windows.Graphics.Imaging.BitmapDecoder])
            $bitmap = Await ($decoder.GetSoftwareBitmapAsync()) ([Windows.Graphics.Imaging.SoftwareBitmap])
            $res = Await ($script:ocr.RecognizeAsync($bitmap)) ([Windows.Media.Ocr.OcrResult])
            $text = ($res.Lines | ForEach-Object { $_.Text }) -join " | "
            $bitmap.Dispose(); $stream.Dispose()
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

# ---- gestures (2340x1080 landscape; zones from input_android.c) -----------
function Press-A { Adb-Run @('shell', 'input', 'swipe', '1170', '540', '1170', '540', '1100') | Out-Null }
function Press-Start { Adb-Run @('shell', 'input', 'swipe', '400', '200', '400', '200', '1100') | Out-Null }
function Stick-Down { Adb-Run @('shell', 'input', 'swipe', '421', '540', '421', '840', '1500') | Out-Null }
function Stick-Left { Adb-Run @('shell', 'input', 'swipe', '585', '700', '120', '700', '1500') | Out-Null }

$shotN = 0
$scanPath = Join-Path $logs "_${Prefix}_scan.png"
function Scan-Shot {
    Adb-Run @('shell', 'screencap', '-p', '/sdcard/korra_life.png') | Out-Null
    Adb-Run @('pull', '/sdcard/korra_life.png', $scanPath) | Out-Null
    if (-not (Test-Path $scanPath)) { throw "screencap produced no file" }
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
function Classify([string]$text) {
    $flat = ($text.ToLowerInvariant() -replace '[^a-z0-9]', '')
    if ($flat -match "pressany|anybutt") { return "attract" }
    if ($flat -match "untilfull|weekendbrief|swipeup|vodafone|turkcell") { return "lockscreen" }
    if ($flat -match "autosave") { return "autosave-notice" }
    if ($flat -match "stronglyrecommended|xbox360controller") { return "pad-notice" }
    if ($flat -match "reallywanttoexit|unsavedprogress") { return "exit-confirm" }
    if ($flat -match "resetyourgameprogress|challengesyour") { return "newgame-confirm" }
    if ($flat -match "lightattack|heavyattack|jump") { return "gameplay" }
    # Pause menu: "Resume / Restart from checkpoint / Options / Quit".
    if ($flat -match "resume|resumegame|restartfrom|retryfrom") { return "pause-menu" }
    if ($flat -match "startstorymode|storymode") { return "main-menu-story" }
    if ($flat -match "probendin|pptions|options") { return "main-menu" }
    if ($flat -match "newgame|startanewgame|stoay") { return "story-submenu" }
    if ($flat -match "casual|extreme|difficulty") { return "difficulty" }
    if ($flat.Length -lt 3) { return "blank" }
    return "unknown"
}
function Get-Logcat([string]$name) {
    $path = Join-Path $logs $name
    Adb-Run @('logcat', '-d', '-v', 'threadtime', '-s', 'Korra:*', 'KorraWsi:*', 'DXVK:*',
              'DEBUG:*', 'libc:*') | Out-File -FilePath $path -Encoding utf8
    return $path
}

# ---- setup ---------------------------------------------------------------
$apkPath = Join-Path $root "android\app\build\outputs\apk\debug\app-debug.apk"
if ($Install -and -not $NoInstall) {
    if (-not (Test-Path $apkPath)) { throw "APK not found: $apkPath" }
    Adb-Run @('install', '-r', $apkPath) | Out-Host
}
Adb-Run @('shell', 'svc', 'power', 'stayon', 'true') | Out-Null
Adb-Run @('shell', 'settings', 'put', 'system', 'screen_off_timeout', '1800000') | Out-Null
Adb-Run @('shell', 'input', 'keyevent', 'KEYCODE_WAKEUP') | Out-Null
Adb-Run @('shell', 'wm', 'dismiss-keyguard') | Out-Null
if ($RunProbe) {
    foreach ($p in @('debug.korra.wsi', 'debug.korra.lifecycle')) {
        Adb-Run @('shell', 'setprop', $p, '1') | Out-Null
    }
    Adb-Run @('shell', 'setprop', 'debug.korra.watch', 'E4D060') | Out-Null
}
Adb-Run @('logcat', '-c') | Out-Null
Adb-Run @('shell', 'am', 'force-stop', $Package) | Out-Null
Start-Sleep -Seconds 1
Adb-Run @('shell', 'am', 'start', '-n', "$Package/.MainActivity") | Out-Null
Start-Sleep -Seconds 3
$t0 = Get-Date
$script:elapsed = 0
$lastState = ""
$reachedDifficulty = $false
$submenuTries = 0
$cooldownUntil = 0
$pauseShot = $null

function Route-Step {
    # One classified screen step. Returns the state it acted on.
    $script:shot = Scan-Shot
    $text = Get-ScreenText $script:shot
    $state = Classify $text
    if ($state -ne $lastState) {
        $kept = Keep-Shot $state
        Note ("state={0} shot={1} text={2}" -f $state, (Split-Path -Leaf $kept),
              ($text.Substring(0, [Math]::Min(160, $text.Length))))
        $script:lastState = $state
    }
    switch ($state) {
        "lockscreen"      { Adb-Run @('shell', 'wm', 'dismiss-keyguard') | Out-Null; Adb-Run @('shell', 'input', 'keyevent', 'KEYCODE_MENU') | Out-Null }
        "attract"         { Press-A }
        "autosave-notice" { Press-A }
        "pad-notice"      { Press-A }
        "main-menu-story" { Press-A }
        "main-menu"       { Press-A }
        "story-submenu"   {
            if ($submenuTries -ge 3) { Press-A; $submenuTries = 0 }
            else { Press-A; $submenuTries = 0 }   # Continue is the default entry
        }
        "newgame-confirm" { Stick-Left; Start-Sleep -Milliseconds 400; Press-A }
        "difficulty"      { $script:reachedDifficulty = $true; Press-A }
        "pause-menu"      { }
        "gameplay"        { }
        default           { Start-Sleep -Milliseconds 900 }
    }
    return $state
}

# ---- route to gameplay ---------------------------------------------------
$state = "unknown"
while ($script:elapsed -lt $Seconds) {
    $script:elapsed = ((Get-Date) - $t0).TotalSeconds
    if ($script:elapsed -lt $cooldownUntil) { Start-Sleep -Milliseconds 700; continue }
    $state = Route-Step
    if ($state -eq "gameplay") { break }
    if ($script:reachedDifficulty -and $script:elapsed -gt 240) {
        Note "HUD not read after the cutscene - treating the screen as gameplay"
        $state = "gameplay"
        break
    }
    $cooldownUntil = $script:elapsed + 2.6
}

if ($state -ne "gameplay") {
    Note "FAIL: never reached gameplay in $Seconds s (last state $state)"
    Get-Logcat "$Prefix`_logcat.txt" | Out-Null
    exit 2
}
Note "REACHED gameplay"
Start-Sleep -Seconds 6
$script:shot = Scan-Shot
Keep-Shot "gameplay" | ForEach-Object { Note "shot: $(Split-Path -Leaf $_)" }

if ($PauseBefore) {
    Note "act: START (open the in-game pause menu)"
    Press-Start
    Start-Sleep -Seconds 3
    $script:shot = Scan-Shot
    $t = Get-ScreenText $script:shot
    $pauseShot = Keep-Shot "pause-before"
    Note ("pause-before state={0} shot={1} text={2}" -f (Classify $t),
          (Split-Path -Leaf $pauseShot), ($t.Substring(0, [Math]::Min(160, $t.Length))))
    Note "act: START (leave the pause menu)"
    Press-Start
    Start-Sleep -Seconds 3
}

if ($SkipHome) {
    Note "SkipHome: no HOME sent"
    Get-Logcat "$Prefix`_logcat.txt" | Out-Null
    exit 0
}

# ---- HOME ----------------------------------------------------------------
$pidBefore = ((Adb-Run @('shell', 'pidof', $Package)) -join "").Trim()
Adb-Run @('shell', 'input', 'keyevent', 'KEYCODE_HOME') | Out-Null
$homeT0 = Get-Date
Note "act: HOME sent (pid=$pidBefore)"
Start-Sleep -Seconds 3
$script:shot = Scan-Shot
Keep-Shot "home" | ForEach-Object { Note "shot: $(Split-Path -Leaf $_)" }
Get-Logcat "$Prefix`_logcat_home.txt" | Out-Null
Start-Sleep -Seconds ([Math]::Max(0, $HomeSeconds - 3))
$script:elapsed = ((Get-Date) - $t0).TotalSeconds
Note ("HOME window over after {0:N0}s" -f ((Get-Date) - $homeT0).TotalSeconds)

# ---- resume --------------------------------------------------------------
Adb-Run @('shell', 'am', 'start', '-n', "$Package/.MainActivity") | Out-Null
Start-Sleep -Seconds 3
$pidAfter = ((Adb-Run @('shell', 'pidof', $Package)) -join "").Trim()
Note "act: am start (pid=$pidAfter, was $pidBefore)"
$resumeT0 = Get-Date
$resumeState = "unknown"
while (((Get-Date) - $resumeT0).TotalSeconds -lt $ResumeSeconds) {
    $script:elapsed = ((Get-Date) - $t0).TotalSeconds
    $script:shot = Scan-Shot
    $text = Get-ScreenText $script:shot
    $resumeState = Classify $text
    if ($resumeState -eq "gameplay" -or $resumeState -eq "pause-menu") { break }
    if ($resumeState -eq "attract" -or $resumeState -eq "main-menu-story") {
        # The window came back but the game was restarted from the top: that is
        # a failure of the resume path, not a pass - record it and stop.
        Keep-Shot "resume-restarted" | Out-Null
        Note "RESUME went back to state=$resumeState (game restarted)"
        break
    }
    Start-Sleep -Seconds 2
}
$script:shot = Scan-Shot
$resumeText = Get-ScreenText $script:shot
$kept = Keep-Shot "resume-$resumeState"
Note ("resume state={0} shot={1} text={2}" -f $resumeState, (Split-Path -Leaf $kept),
      ($resumeText.Substring(0, [Math]::Min(200, $resumeText.Length))))

# A gameplay screen should answer START with the pause menu; that is the proof
# that the engine is alive and interactive after the round trip.
if ($resumeState -eq "gameplay") {
    Note "act: START (pause menu after resume)"
    Press-Start
    Start-Sleep -Seconds 3
    $script:shot = Scan-Shot
    $t = Get-ScreenText $script:shot
    $kept = Keep-Shot "pause-after"
    Note ("pause-after state={0} shot={1} text={2}" -f (Classify $t), (Split-Path -Leaf $kept),
          ($t.Substring(0, [Math]::Min(200, $t.Length))))
    Adb-Run @('shell', 'input', 'keyevent', 'KEYCODE_BACK') | Out-Null
    Start-Sleep -Seconds 2
}

# ---- evidence ------------------------------------------------------------
$pidEnd = ((Adb-Run @('shell', 'pidof', $Package)) -join "").Trim()
Note "final pid=$pidEnd (alive=$([bool]$pidEnd))"
Get-Logcat "$Prefix`_logcat.txt" | Out-Null
Adb-Run @('logcat', '-b', 'crash', '-d') | Out-File -FilePath (Join-Path $logs "$Prefix`_crash.txt") -Encoding utf8
Adb-Run @('shell', 'dumpsys', 'activity', 'exit-info', $Package) |
    Out-File -FilePath (Join-Path $logs "$Prefix`_exit.txt") -Encoding utf8
Adb-Run @('shell', 'run-as', $Package, 'cat', 'files/game/../log.txt') | Out-Null
# The in-app log lives in the app's external files dir.
Adb-Run @('pull', "/sdcard/Android/data/$Package/files/log.txt", (Join-Path $logs "$Prefix`_log.txt")) | Out-Null
$exit = Select-String -Path (Join-Path $logs "$Prefix`_exit.txt") -Pattern 'reason=|status=' |
        ForEach-Object { $_.Line.Trim() }
Note ("exit-info: {0}" -f ($exit -join " / "))
