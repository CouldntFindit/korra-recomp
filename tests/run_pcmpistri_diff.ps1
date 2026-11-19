# Builds tests/pcmpistri_diff.c twice (Win32 vs x64) and compares the results.
#
# The Win32 build compiles recomp_pcmpistri() into the real SSE4.2 intrinsic
# path; the x64 build falls through to the portable body that arm64 will use
# (the guard is `defined(_MSC_VER) && defined(_M_IX86)`). Same source, same CPU,
# so a difference is a bug in the portable body.
#
#   powershell -ExecutionPolicy Bypass -File tests\run_pcmpistri_diff.ps1
#
# Requires the pinned pcrecomp tree: %PCRECOMP% or ..\pcrecomp.

param(
    [string]$Pcrecomp = $(if ($env:PCRECOMP) { $env:PCRECOMP } else { "..\pcrecomp" }),
    [switch]$Keep
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

if (-not (Test-Path $Pcrecomp)) { throw "pcrecomp not found at $Pcrecomp (set PCRECOMP)" }
$include = Join-Path (Resolve-Path $Pcrecomp) "runtime\recomp32"
if (-not (Test-Path (Join-Path $include "recomp_types.h"))) {
    throw "runtime/recomp32/recomp_types.h not found under $Pcrecomp"
}
$src = Join-Path $PSScriptRoot "pcmpistri_diff.c"
$work = Join-Path $env:TEMP "opencode\pcmpistri"
New-Item -ItemType Directory -Force -Path $work | Out-Null

# vcvarsall from the project-local Build Tools install (see docs/TOOLCHAIN.md).
$vcvars = "C:\Users\yigit\OneDrive\Belgeler\korradecomp\msvc\VC\Auxiliary\Build\vcvarsall.bat"
if (-not (Test-Path $vcvars)) {
    $vcvars = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
}
if (-not (Test-Path $vcvars)) { throw "vcvarsall.bat not found - run scripts\build_host.ps1 once to locate it" }

function Build([string]$arch, [string]$exe) {
    $cmd = "`"$vcvars`" $arch >nul && cd /d `"$work`" && cl /nologo /W3 /O2 /D_CRT_SECURE_NO_WARNINGS /I`"$include`" `"$src`" /Fe:`"$exe`""
    cmd /c $cmd | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "build failed ($arch)" }
}

$exe32 = Join-Path $work "diff32.exe"
$exe64 = Join-Path $work "diff64.exe"
Build "x86" $exe32
Build "x64" $exe64

$bin32 = Join-Path $work "out32.bin"
$bin64 = Join-Path $work "out64.bin"
& $exe32 $bin32 | Tee-Object -Variable r32 | Write-Host
& $exe64 $bin64 | Tee-Object -Variable r64 | Write-Host

$h32 = ($r32 | Select-String 'hash=').Line
$h64 = ($r64 | Select-String 'hash=').Line
$equal = -not (Compare-Object $h32 $h64)

$cmp = @"
import sys
a = open(sys.argv[1], 'rb').read()
b = open(sys.argv[2], 'rb').read()
if len(a) != len(b):
    print('size mismatch: %d vs %d' % (len(a), len(b))); sys.exit(1)
KORRA = (0x0C, 0x0D, 0x40)
name = {0x00:'ubyte equal-any lsb', 0x04:'ubyte ranges lsb',
        0x08:'ubyte equal-each lsb', 0x0C:'ubyte equal-ordered lsb',
        0x0D:'UWORD equal-ordered lsb', 0x10:'ubyte equal-any neg lsb',
        0x14:'ubyte ranges neg lsb', 0x18:'ubyte equal-each neg lsb',
        0x1C:'ubyte equal-ordered neg lsb', 0x20:'ubyte equal-any msb',
        0x30:'ubyte equal-any neg msb', 0x38:'ubyte equal-each neg msb',
        0x40:'ubyte equal-any masked msb', 0x44:'ubyte ranges masked msb',
        0x48:'ubyte equal-each masked msb', 0x4C:'ubyte equal-ordered masked msb'}
stats = {}
first = {}
for i in range(0, len(a), 8):
    ra, rb = a[i:i+8], b[i:i+8]
    if ra[:3] == rb[:3]:
        continue
    imm = ra[3]
    st = stats.setdefault(imm, [0, 0, 0])
    st[0] += 1
    if ra[:2] != rb[:2]:
        st[1] += 1
    if ra[2] != rb[2]:
        st[2] += 1
    if imm not in first:
        first[imm] = (ra, rb)
print()
print('imm   differing   index   flags  used-by-Korra  meaning')
for imm in sorted(stats):
    d, ix, fl = stats[imm]
    print('0x%02X %10d %7d %7d  %-13s %s'
          % (imm, d, ix, fl, 'YES' if imm in KORRA else '-', name.get(imm, '?')))
agree = [v for v in (0x00,0x04,0x08,0x0C,0x0D,0x10,0x14,0x18,0x1C,0x20,0x30,0x38,0x40,0x44,0x48,0x4C)
         if v not in stats]
print('agreeing imm8 values: %s' % ', '.join('0x%02X' % v for v in agree))
for imm in sorted(first):
    ra, rb = first[imm]
    case = ra[4] | (ra[5] << 8) | (ra[6] << 16)
    print('first diff imm=0x%02X case=%d  x86 idx=%d zf=%d cf=%d  |  portable idx=%d zf=%d cf=%d'
          % (imm, case, ra[0] | (ra[1] << 8), ra[2] & 1, (ra[2] >> 1) & 1,
             rb[0] | (rb[1] << 8), rb[2] & 1, (rb[2] >> 1) & 1))
bad_korra = [v for v in KORRA if v in stats]
print()
if bad_korra:
    print('FAIL: portable body disagrees with hardware for Korra imm8 %s'
          % ', '.join('0x%02X' % v for v in bad_korra))
    sys.exit(1)
print('PASS: portable body agrees for every Korra imm8 (0x0C, 0x0D, 0x40)')
sys.exit(0)
"@
$py = Join-Path $work "compare.py"
Set-Content -Path $py -Value $cmp -Encoding UTF8
python $py $bin32 $bin64
$rc = $LASTEXITCODE

if (-not $Keep) { Remove-Item $bin32, $bin64 -ErrorAction SilentlyContinue }
if ($rc -ne 0) { exit 1 }
if (-not $equal) { Write-Host "per-imm hash lines differ (see tuple diff above)"; exit 1 }
Write-Host "PASS: both builds agree on every tuple"
exit 0
