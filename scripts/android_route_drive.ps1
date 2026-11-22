# OCR-driven touch route: walks the game from the attract screen into the New
# Game intro cutscene and then lets it run, taking a screenshot and reading the
# screen at every step.
#
# Why not a fixed timeline: the engine polls XInput slowly, so a 0 ms `input tap`
# is usually missed (a press must hold ~1 s), and a missed press shifts every
# later step. Worse, the exit dialog's default is NO and the main menu's last
# item is Exit, so a blind "down + A" can walk into "Do you really want to exit
# the game?" (that is exactly what the save session's screenshots show). This
# driver reads the screen with Windows' built-in OCR (Windows.Media.Ocr) and
# decides each press from what is actually displayed.
#
#   powershell -ExecutionPolicy Bypass -File scripts\android_route_drive.ps1 -Seconds 600
param(
    [int]$Seconds = 600,
    [string]$Package = "org.korra.recomp",
    [string]$Prefix = "drive",
    [switch]$NoInstall
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
& $adb shell svc power stayon true | Out-Null
& $adb shell input keyevent KEYCODE_WAKEUP | Out-Null
& $adb shell am force-stop $Package | Out-Null
Start-Sleep -Seconds 1
& $adb shell am start -n "$Package/.MainActivity" | Out-Null
$t0 = Get-Date
$script:elapsed = 0
$lastState = ""
$cooldownUntil = 0
$lastText = ""
$submenuTries = 0
$nextMeminfo = 0
$nextHeartbeat = 0

$died = $false

while (((Get-Date) - $t0).TotalSeconds -lt $Seconds) {
    $script:elapsed = ((Get-Date) - $t0).TotalSeconds
    $pidof = ((Adb-Run @('shell', 'pidof', $Package)) -join "").Trim()
    if (-not $pidof) { $died = $true; Note "process ended (pidof empty)"; break }

    if ($script:elapsed -ge $nextMeminfo) {
        "--- {0:N0}s ---" -f $script:elapsed | Out-File -Append -FilePath (Join-Path $logs "meminfo.txt") -Encoding ascii
        Adb-Run @('shell', 'dumpsys', 'meminfo', $Package) | Out-File -Append -FilePath (Join-Path $logs "meminfo.txt") -Encoding ascii
        $nextMeminfo = $script:elapsed + 30
    }

    if ($script:elapsed -lt $cooldownUntil) { Start-Sleep -Milliseconds 700; continue }

    $shot = Scan-Shot
    $text = Get-ScreenText $shot
    # OCR mangles the logo fonts ("PRESS ANY BUTTON" comes back as
    # "PK ESS ANY BUT TON"), so classify on a letters-and-digits-only copy.
    $flat = ($text.ToLowerInvariant() -replace '[^a-z0-9]', '')

    $state = "unknown"
    if ($flat -match "pressany|anybutt") { $state = "attract" }
    elseif ($flat -match "autosave|savemdi") { $state = "autosave-notice" }
    elseif ($flat -match "stronglyrecommended|xbox360controller") { $state = "pad-notice" }
    elseif ($flat -match "reallywanttoexit|unsavedprogress") { $state = "exit-confirm" }
    elseif ($flat -match "resetyourgameprogress|challengesyour") { $state = "newgame-confirm" }
    elseif ($flat -match "continue" -and $flat -match "newgame") { $state = "story-submenu" }
    elseif ($flat -match "startstorymode") { $state = "main-menu-story" }
    elseif ($flat -match "probending") { $state = "main-menu" }
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
        "attract"         { Note "act: A (leave attract)"; Press-A }
        "autosave-notice" { Note "act: A (dismiss auto-save notice)"; Press-A }
        "pad-notice"      { Note "act: A (dismiss pad notice)"; Press-A }
        "main-menu-story" { Note "act: A (open Story)"; Press-A }
        "main-menu"       { Note "act: stick up (select Story)"; Stick-Up }
        "story-submenu"   {
            if ($t -match "start a new game") { Note "act: A (New Game)"; Press-A; $submenuTries = 0 }
            elseif ($submenuTries -ge 3) { Note "act: A anyway (no description read)"; Press-A; $submenuTries = 0 }
            else { Note ("act: stick down (Continue -> New Game), try {0}" -f ($submenuTries + 1)); Stick-Down; $submenuTries++ }
        }
        "newgame-confirm" { Note "act: stick left (NO -> YES) then A"; Stick-Left; Start-Sleep -Milliseconds 400; Press-A }
        "difficulty"      { Note "act: A (confirm difficulty)"; Press-A }
        "exit-confirm"    {
            # The default is NO: A cancels. Never send LEFT here.
            Note "act: A (NO - cancel the exit dialog)"
            Press-A
        }
        default           { Start-Sleep -Milliseconds 900 }
    }
    $cooldownUntil = $script:elapsed + 2.6
}

if (-not $died) { Note ("still alive at {0:N0}s" -f ((Get-Date) - $t0).TotalSeconds) }
$final = Keep-Shot "final"
Note ("final screenshot {0}" -f $final)

# ---- artifacts ----------------------------------------------------------
Adb-Run @('pull', "/sdcard/Android/data/$Package/files/log.txt", (Join-Path $logs "log.txt")) | Out-Null
Adb-Run @('logcat', '-d', '-s', 'Korra:D', 'DXVK:I', 'AndroidRuntime:E', 'DEBUG:E', 'libc:F', '*:S') |
    Out-File -FilePath (Join-Path $logs "logcat.txt") -Encoding ascii
Adb-Run @('logcat', '-b', 'crash', '-d') | Out-File -FilePath (Join-Path $logs "crash.txt") -Encoding ascii
Adb-Run @('shell', 'dumpsys', 'activity', 'exit-info', $Package) |
    Out-File -FilePath (Join-Path $logs "exit-info.txt") -Encoding ascii
Note "artifacts pulled"
