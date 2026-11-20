# Builds the Windows host (32-bit, MSVC x86) without CMake.
#
#   powershell -ExecutionPolicy Bypass -File scripts\build_host.ps1 [-Clean] [-Jobs 8] [-Trace] [-Fast]
#
# -Trace builds a separate object tree (build\obj-trace) with -DRECOMP_TRACE,
# which turns on the runtime's per-entry diagnostics (--calltrace, --firsthit,
# --argtrace, --watch; see recomp_trace.h). Guest code must be rebuilt for it,
# because RECOMP_ENTER is a macro in every generated translation unit, so the
# two trees cannot share objects. The normal tree is left alone.
#
# -Fast builds into build\obj-fast with /O1 + RECOMP_MEM_QUAL empty (guest
# memory accesses stop being `volatile`), for measuring per-instruction cost.
# /O2 was tried first and is not usable: two generated translation units
# (recomp_0021.c, recomp_0035.c, ~90 MB each) exhaust the compiler heap in pass
# two (C1002) even with /Zm1200. It is a different object tree on purpose: the
# two cannot share objects.
param(
    [switch]$Clean,
    [int]$Jobs = 8,
    [switch]$Trace,
    [switch]$Fast
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
if ($Trace) { $obj = Join-Path $root "build\obj-trace" }
if ($Fast) { $obj = Join-Path $root "build\obj-fast" }
$exe = Join-Path $root "build\korra_recomp.exe"
if ($Trace) { $exe = Join-Path $root "build\korra_recomp_trace.exe" }
if ($Fast) { $exe = Join-Path $root "build\korra_recomp_fast.exe" }
if ($Clean -and (Test-Path $obj)) { Remove-Item -Recurse -Force $obj }
New-Item -ItemType Directory -Force -Path $obj, (Join-Path $root "build") | Out-Null

$sources = @(Get-ChildItem (Join-Path $root "src\host") -Filter "*.c" | ForEach-Object { $_.FullName }) + @(
    (Join-Path $pcrecomp "runtime\native32\native32.c"),
    (Join-Path $pcrecomp "runtime\recomp32\image_loader.c"),
    (Join-Path $pcrecomp "runtime\recomp32\recomp_trace.c")
) + (Get-ChildItem $gen -Filter "recomp_*.c" | ForEach-Object { $_.FullName })

$rsp = Join-Path $root "build\sources.rsp"
$sources | ForEach-Object { '"' + $_ + '"' } | Set-Content -Encoding ascii $rsp

# Incremental by default: a translation unit is recompiled only when its .c is
# newer than its .obj. -Clean forces the full 75-TU rebuild (about 7 minutes).
if (-not $Clean) {
    $stale = @($sources | Where-Object {
        $o = Join-Path $obj ((Split-Path $_ -Leaf) -replace '\.c$', '.obj')
        (-not (Test-Path $o)) -or ((Get-Item $o).LastWriteTime -lt (Get-Item $_).LastWriteTime)
    })
} else {
    $stale = $sources
}
if (-not $Clean) {
    $stale | ForEach-Object { '"' + $_ + '"' } | Set-Content -Encoding ascii $rsp
}

$cl = @(
    $(if ($Fast) { "cl /nologo /c /O1 /Oi /MT /W1 /MP$Jobs /Zm1200" } else { "cl /nologo /c /O1 /MT /W1 /MP$Jobs" }),
    "/wd4996 /wd4244 /wd4146 /wd4102 /wd4013 /wd4101 /wd4267 /wd4018 /wd4305 /wd4310 /wd4054 /wd4065 /wd4204",
    "/D_CRT_SECURE_NO_WARNINGS /DWIN32_LEAN_AND_MEAN /DNDEBUG",
    # -Fast drops the `volatile` on every guest memory access. The runtime
    # header (recomp_types.h, RECOMP_MEM_QUAL) documents this as the intended
    # configuration for a host that compiles the lifted tree with optimisation:
    # the per-back-edge opaque call still forces a reload in a guest spin-wait,
    # so no flag written by another thread can be cached across a loop.
    $(if ($Fast) { "/DRECOMP_MEM_QUAL=" } else { "" }),
    $(if ($Trace) { "/DRECOMP_TRACE" } else { "" }),
    "/I`"$root\src\host`" /I`"$gen`" /I`"$pcrecomp\runtime\recomp32`" /I`"$pcrecomp\runtime\native32`"",
    "/Fo`"$obj\\`"",
    "@`"$rsp`""
) -join " "

$libs = "kernel32.lib user32.lib gdi32.lib advapi32.lib shell32.lib ole32.lib oleaut32.lib winmm.lib dbghelp.lib synchronization.lib powrprof.lib"
$map = Join-Path $root "build\korra_recomp.map"
if ($Fast) { $map = Join-Path $root "build\korra_recomp_fast.map" }
$link = "link /nologo /OUT:`"$exe`" /SUBSYSTEM:CONSOLE /DYNAMICBASE:NO /FIXED /BASE:0x60000000 /STACK:16777216 /MAP:`"$map`" `"$obj\*.obj`" $libs"

$script = Join-Path $root "build\build.cmd"
$lines = @("@echo off", "call `"$vcvars`" x86 >nul || exit /b 1", "cd /d `"$root`"")
if ($stale.Count -gt 0) { $lines += $cl; $lines += "if errorlevel 1 exit /b 1" }
$lines += $link
$lines | Set-Content -Encoding ascii $script

Write-Host "compiling $($stale.Count) of $($sources.Count) sources with $Jobs jobs..."
$sw = [Diagnostics.Stopwatch]::StartNew()
& cmd.exe /c $script
$code = $LASTEXITCODE
$sw.Stop()
Write-Host ("build finished in {0:N0}s (exit {1})" -f $sw.Elapsed.TotalSeconds, $code)
if ($code -eq 0) { Write-Host "output: $exe" }
exit $code
