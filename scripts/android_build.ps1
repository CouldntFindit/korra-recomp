# Android build helper.
#
#   powershell -ExecutionPolicy Bypass -File scripts\android_build.ps1
#   powershell -ExecutionPolicy Bypass -File scripts\android_build.ps1 -Clean
#   powershell -ExecutionPolicy Bypass -File scripts\android_build.ps1 -Install   # adb install -r
#
# Why a script and not just gradlew: this machine's JAVA_HOME points at a JDK 8
# install, AGP needs 17+, and the SDK lives at the project root rather than in
# the default per-user location. The script pins both, checks that the SDK
# packages from docs/TOOLCHAIN.md are present, and writes android\local.properties.

param(
    [switch]$Clean,
    [switch]$Install,
    [string]$Sdk = $(if ($env:ANDROID_HOME) { $env:ANDROID_HOME }
                    elseif ($env:ANDROID_SDK_ROOT) { $env:ANDROID_SDK_ROOT }
                    else { "C:\Users\yigit\OneDrive\Belgeler\korradecomp\android-sdk" }),
    [string]$Jdk = $(if ($env:KORRA_JDK) { $env:KORRA_JDK }
                    else { "C:\Program Files\Eclipse Adoptium\jdk-21.0.12.101-hotspot" })
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$android = Join-Path $root "android"

if (-not (Test-Path (Join-Path $android "gradlew.bat"))) {
    throw "android\gradlew.bat missing - this is the wrong checkout"
}
if (-not (Test-Path $Jdk)) {
    throw "JDK not found at '$Jdk'. Install JDK 17+ or set KORRA_JDK."
}
if (-not (Test-Path (Join-Path $Sdk "ndk\28.2.13676358"))) {
    throw "NDK 28.2.13676358 not found under '$Sdk'. See docs/TOOLCHAIN.md for the sdkmanager command."
}

$env:JAVA_HOME = $Jdk
$env:ANDROID_HOME = $Sdk

$localProperties = Join-Path $android "local.properties"
$escaped = $Sdk -replace "\\", "\\" -replace ":", "\:"
Set-Content -Path $localProperties -Value "sdk.dir=$escaped" -Encoding ASCII

Push-Location $android
try {
    if ($Clean) {
        & .\gradlew.bat clean --no-daemon
        if ($LASTEXITCODE -ne 0) { throw "gradle clean failed" }
    }
    & .\gradlew.bat :app:assembleDebug --no-daemon
    if ($LASTEXITCODE -ne 0) { throw "gradle assembleDebug failed" }

    $apk = Join-Path $android "app\build\outputs\apk\debug\app-debug.apk"
    if (-not (Test-Path $apk)) { throw "APK not produced at $apk" }
    $size = (Get-Item $apk).Length
    # Decimal MB, so it matches the numbers in docs/TOOLCHAIN.md.
    Write-Host ("APK: {0} ({1:N1} MB)" -f $apk, ($size / 1e6))

    if ($Install) {
        $adb = Join-Path $Sdk "platform-tools\adb.exe"
        $devices = & $adb devices | Select-String -Pattern "device$"
        if (-not $devices) { throw "no adb device attached" }
        & $adb install -r $apk
        if ($LASTEXITCODE -ne 0) { throw "adb install failed" }
        Write-Host "installed on device"
    }
} finally {
    Pop-Location
}
