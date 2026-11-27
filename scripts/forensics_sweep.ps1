param(
    [string]$TagPrefix = "s1c",
    [int]$First = 2,
    [int]$Count = 4,
    [int]$WaitUs = 1000,
    [int]$Seconds = 380,
    [int]$PlaySeconds = 120
)
$ErrorActionPreference = "Continue"
$repo = Split-Path -Parent $PSScriptRoot
$adb = Join-Path (Split-Path (Split-Path $repo -Parent) -Parent) "android-sdk\platform-tools\adb.exe"
$logs = Join-Path $repo "logs"
& $adb shell "setprop debug.korra.wait_us $WaitUs" | Out-Null
$got = (& $adb shell getprop debug.korra.wait_us) -join ""
Write-Host ("wait_us={0} (device reports {1})" -f $WaitUs, $got)

for ($i = $First; $i -lt ($First + $Count); $i++) {
    $tag = "{0}{1}" -f $TagPrefix, $i
    Write-Host ("==== run {0} : {1} ====" -f $i, $tag)
    & $adb shell input keyevent KEYCODE_WAKEUP | Out-Null
    & $adb shell wm dismiss-keyguard | Out-Null
    & $adb shell input keyevent KEYCODE_HOME | Out-Null
    Start-Sleep -Seconds 2
    $t0 = Get-Date
    & powershell -ExecutionPolicy Bypass -File (Join-Path $repo "scripts\android_route_drive.ps1") `
        -Seconds $Seconds -PlaySeconds $PlaySeconds -Prefix $tag -NoInstall 2>&1 |
        Tee-Object -FilePath (Join-Path $logs ("run_{0}.console.txt" -f $tag)) | Out-Null
    $elapsed = [int]((Get-Date) - $t0).TotalSeconds
    foreach ($n in @("log", "perf", "crash", "logcat", "exit-info", "route_drive")) {
        $src = Join-Path $logs "$n.txt"
        if (Test-Path $src) { Copy-Item $src (Join-Path $logs ("{0}_{1}.txt" -f $tag, $n)) -Force }
    }
    $crash = 0
    $line = ""
    if (Test-Path (Join-Path $logs ("{0}_log.txt" -f $tag))) {
        $hits = Select-String -Path (Join-Path $logs ("{0}_log.txt" -f $tag)) -Pattern '\[crash\] signal'
        $crash = @($hits).Count
        if ($crash) { $line = $hits[0].Line }
    }
    $crashBuf = 0
    if (Test-Path (Join-Path $logs ("{0}_crash.txt" -f $tag))) {
        $crashBuf = (Get-Item (Join-Path $logs ("{0}_crash.txt" -f $tag))).Length
    }
    $state = (& $adb shell pidof org.korra.recomp) -join ""
    Write-Host ("RESULT {0}: wall={1}s guestCrashes={2} crashBuffer={3}B stillRunning={4}" -f $tag, $elapsed, $crash, $crashBuf, [bool]$state)
    if ($crash) { Write-Host ("  " + $line) }
    & $adb shell am force-stop org.korra.recomp | Out-Null
    Start-Sleep -Seconds 3
}
Write-Host "sweep loop done"
