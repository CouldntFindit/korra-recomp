# Builds the Windows host (32-bit, MSVC x86) without CMake.
#
#   powershell -ExecutionPolicy Bypass -File scripts\build_host.ps1 [-Clean] [-Jobs 8]
#
# Requires the VS 2022 Build Tools + C++ x86 toolset. Point KORRA_MSVC at the
# install root if it is not <workspace>\msvc (see docs/TOOLCHAIN.md).
param(
    [switch]$Clean,
    [int]$Jobs = 8
)
$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$msvc = if ($env:KORRA_MSVC) { $env:KORRA_MSVC } else { Join-Path (Split-Path -Parent $root) "..\msvc" | Resolve-Path | ForEach-Object { $_.Path } }
$vcvars = Join-Path $msvc "VC\Auxiliary\Build\vcvarsall.bat"
if (-not (Test-Path $vcvars)) { throw "vcvarsall.bat not found at $vcvars (set KORRA_MSVC)" }

$pcrecomp = if ($env:PCRECOMP) { (Resolve-Path $env:PCRECOMP).Path } else { (Resolve-Path (Join-Path (Split-Path -Parent $root) "pcrecomp")).Path }
$gen = Join-Path $root "src\recomp\gen"
if (-not (Test-Path (Join-Path $gen "recomp_dispatch.c"))) {
    throw "generated code missing; run scripts\run_korra_lift.py lift first"
}

$obj = Join-Path $root "build\obj"
$exe = Join-Path $root "build\korra_recomp.exe"
if ($Clean -and (Test-Path $obj)) { Remove-Item -Recurse -Force $obj }
New-Item -ItemType Directory -Force -Path $obj, (Join-Path $root "build") | Out-Null

$sources = @(
    (Join-Path $root "src\host\main.c"),
    (Join-Path $root "src\host\shims.c"),
    (Join-Path $pcrecomp "runtime\native32\native32.c"),
    (Join-Path $pcrecomp "runtime\recomp32\image_loader.c"),
    (Join-Path $pcrecomp "runtime\recomp32\recomp_trace.c")
) + (Get-ChildItem $gen -Filter "recomp_*.c" | ForEach-Object { $_.FullName })

$rsp = Join-Path $root "build\sources.rsp"
$sources | ForEach-Object { '"' + $_ + '"' } | Set-Content -Encoding ascii $rsp

$cl = @(
    "cl /nologo /c /O1 /MT /W1 /MP$Jobs",
    "/wd4996 /wd4244 /wd4146 /wd4102 /wd4013 /wd4101 /wd4267 /wd4018 /wd4305 /wd4310 /wd4054 /wd4065 /wd4204",
    "/D_CRT_SECURE_NO_WARNINGS /DWIN32_LEAN_AND_MEAN /DNDEBUG",
    "/I`"$root\src\host`" /I`"$gen`" /I`"$pcrecomp\runtime\recomp32`" /I`"$pcrecomp\runtime\native32`"",
    "/Fo`"$obj\\`"",
    "@`"$rsp`""
) -join " "

$libs = "kernel32.lib user32.lib gdi32.lib advapi32.lib shell32.lib ole32.lib oleaut32.lib winmm.lib dbghelp.lib synchronization.lib"
$link = "link /nologo /OUT:`"$exe`" /SUBSYSTEM:CONSOLE /DYNAMICBASE:NO /FIXED /BASE:0x60000000 /STACK:16777216 /MAP:`"$root\build\korra_recomp.map`" `"$obj\*.obj`" $libs"

$script = Join-Path $root "build\build.cmd"
@(
    "@echo off",
    "call `"$vcvars`" x86 >nul || exit /b 1",
    "cd /d `"$root`"",
    $cl,
    "if errorlevel 1 exit /b 1",
    $link
) | Set-Content -Encoding ascii $script

Write-Host "compiling $($sources.Count) sources with $Jobs jobs..."
$sw = [Diagnostics.Stopwatch]::StartNew()
& cmd.exe /c $script
$code = $LASTEXITCODE
$sw.Stop()
Write-Host ("build finished in {0:N0}s (exit {1})" -f $sw.Elapsed.TotalSeconds, $code)
if ($code -eq 0) { Write-Host "output: $exe" }
exit $code
