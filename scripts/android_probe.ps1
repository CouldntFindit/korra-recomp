# Builds one C probe for arm64 Android with the NDK, pushes it, runs it on the
# attached device and prints its output. Standalone (no Gradle, no APK): the
# probes in tools/android/ are device diagnostics, not part of the game.
#
#   powershell -ExecutionPolicy Bypass -File scripts\android_probe.ps1 -Name vk_probe
#   powershell -ExecutionPolicy Bypass -File scripts\android_probe.ps1 -Name pcmpistri_diff `
#       -Source tests\pcmpistri_diff.c -Include ..\pcrecomp\runtime\recomp32 `
#       -Args /data/local/tmp/arm64.bin -Pull /data/local/tmp/arm64.bin -PullTo work\arm64.bin
#
# Credentials: none. The device must be attached (adb devices) and the NDK path
# below must exist (docs/TOOLCHAIN.md).

param(
    [Parameter(Mandatory = $true)][string]$Name,
    [string]$Source,
    [string[]]$Include = @(),
    [string]$Args = "",
    [string[]]$Pull = @(),
    [string[]]$PullTo = @(),
    [int]$RunTimeoutSec = 600
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$sdk = "C:\Users\yigit\OneDrive\Belgeler\korradecomp\android-sdk"
$adb = Join-Path $sdk "platform-tools\adb.exe"
$ndkRoot = Join-Path $sdk "ndk"
$ndk = (Get-ChildItem $ndkRoot -Directory | Sort-Object Name -Descending | Select-Object -First 1).FullName
$clang = Join-Path $ndk "toolchains\llvm\prebuilt\windows-x86_64\bin\clang.exe"
if (-not (Test-Path $clang)) { throw "NDK clang not found: $clang" }
if (-not (Test-Path $adb)) { throw "adb not found: $adb" }

if (-not $Source) { $Source = Join-Path $root "tools\android\$Name.c" }
if (-not (Test-Path $Source)) { throw "probe source not found: $Source" }

$outDir = Join-Path $env:TEMP "opencode\korra_android_probe"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$exe = Join-Path $outDir $Name

$inc = @()
foreach ($i in $Include) {
    $p = if ([System.IO.Path]::IsPathRooted($i)) { $i } else { Join-Path $root $i }
    $inc += "-I$p"
}

$api = 29
$cmd = @(
    $clang, "--target=aarch64-linux-android$api", "-O2", "-std=c11", "-pie", "-fPIE",
    "-Wall", "-Wextra", "-Wno-unused-parameter", "-o", $exe
) + $inc + @($Source, "-lvulkan", "-llog", "-lz", "-lm")
& $cmd[0] $cmd[1..($cmd.Count - 1)]
if ($LASTEXITCODE -ne 0) { throw "compile failed ($Name)" }
Write-Host ("built {0} ({1:N0} bytes) for arm64 API {2}" -f $Name, (Get-Item $exe).Length, $api)

& $adb push $exe "/data/local/tmp/$Name" | Out-Host
& $adb shell "chmod 755 /data/local/tmp/$Name"
if ($LASTEXITCODE -ne 0) { throw "push/chmod failed" }

Write-Host "--- device output: /data/local/tmp/$Name $Args ---"
$sw = [Diagnostics.Stopwatch]::StartNew()
& $adb shell "/data/local/tmp/$Name $Args"
$rc = $LASTEXITCODE
$sw.Stop()
Write-Host ("--- exit {0} in {1:N1} s ---" -f $rc, $sw.Elapsed.TotalSeconds)

for ($k = 0; $k -lt $Pull.Count; $k++) {
    $dest = if ($k -lt $PullTo.Count -and $PullTo[$k]) {
        if ([System.IO.Path]::IsPathRooted($PullTo[$k])) { $PullTo[$k] } else { Join-Path $root $PullTo[$k] }
    } else { Join-Path $outDir (Split-Path -Leaf $Pull[$k]) }
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $dest) | Out-Null
    & $adb pull $Pull[$k] $dest | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "pull failed: $($Pull[$k])" }
    Write-Host ("pulled {0} -> {1} ({2:N0} bytes)" -f $Pull[$k], $dest, (Get-Item $dest).Length)
}

if ($rc -ne 0) { exit $rc }
