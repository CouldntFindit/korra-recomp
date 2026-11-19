# Builds and runs the host-shim selftest (x86, MSVC) without the lifted code.
#
#   powershell -ExecutionPolicy Bypass -File scripts\run_selftest.ps1
#
# It compiles only src\host\xaudio2_stub.c + src\host\steam_stub.c plus the test
# harness, then runs the binary. The test drives the vtables with real x86
# thunks that put `this` in ECX and measure the stack purge, so a wrong
# __stdcall parameter count fails here instead of corrupting the guest later.
param([switch]$Keep)
$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$msvc = if ($env:KORRA_MSVC) { $env:KORRA_MSVC } else { Join-Path (Split-Path -Parent $root) "..\msvc" | Resolve-Path | ForEach-Object { $_.Path } }
$vcvars = Join-Path $msvc "VC\Auxiliary\Build\vcvarsall.bat"
if (-not (Test-Path $vcvars)) { throw "vcvarsall.bat not found at $vcvars (set KORRA_MSVC)" }

$pcrecomp = if ($env:PCRECOMP) { (Resolve-Path $env:PCRECOMP).Path } else { (Resolve-Path (Join-Path (Split-Path -Parent $root) "pcrecomp")).Path }
$out = Join-Path $root "build\selftest"
New-Item -ItemType Directory -Force -Path $out | Out-Null

$cl = "cl /nologo /O1 /MT /W3 /D_CRT_SECURE_NO_WARNINGS /DWIN32_LEAN_AND_MEAN " +
      "/I$root\src\host /I$pcrecomp\runtime\recomp32 /I$pcrecomp\runtime\native32 " +
      "/Fo$out\ /Fe$out\selftest.exe " +
      "tests\selftest_host_shims.c src\host\xaudio2_stub.c src\host\steam_stub.c " +
      "/link ole32.lib user32.lib"

$cmd = Join-Path $out "build.cmd"
@(
    "@echo off",
    "call `"$vcvars`" x86 >nul || exit /b 1",
    "cd /d `"$root`"",
    $cl,
    "if errorlevel 1 exit /b 1",
    "`"$out\selftest.exe`"",
    "exit /b %ERRORLEVEL%"
) | Set-Content -Encoding ascii $cmd

& cmd.exe /c $cmd
$code = $LASTEXITCODE
if (-not $Keep) { Remove-Item (Join-Path $out "*.obj") -Force -ErrorAction SilentlyContinue }
exit $code
