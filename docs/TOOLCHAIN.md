# Toolchain pin (verified 2026-11-19)

`pcrecomp` moves fast and deletes branches, so the pin is a **commit**, not a
branch name. The `nfsu2-fixes` branch named in older notes no longer exists and
its commit `b278e8a` is not fetchable — this pin supersedes it.

| Item | Value |
|---|---|
| Repo | https://github.com/sp00nznet/pcrecomp (note: `sp00nznet`, not `sp00z`) |
| Base commit | `35548b8` — *disasm32: straight-line code longer than the scan window is decoded to its end (#50)*, upstream HEAD on 2026-10-04 |
| Branch used here | `korra-base` (local) |
| Patch series | `nfsu2-recompiled/patches/pcrecomp/0001…0014` — all apply cleanly, in order, on `35548b8` |
| Extra patch | `maxpayne2/patches/pcrecomp/0001-seh-frames-and-or-xor-conditions.patch` — applies cleanly on top |
| pcrecomp `main` @ 2026-11-18 | **not usable**: only patch 0001 applies, the rest conflict (the runtime was restructured: `native32` split, `difftest.py` moved). Do not "just upgrade". |

Reproduce:

```powershell
git clone https://github.com/sp00nznet/pcrecomp.git ..\pcrecomp
cd ..\pcrecomp
git checkout -b korra-base 35548b8
Get-ChildItem ..\nfsu2-recompiled\patches\pcrecomp\*.patch | Sort-Object Name |
    ForEach-Object { git apply $_.FullName }
git apply ..\maxpayne2\patches\pcrecomp\0001-seh-frames-and-or-xor-conditions.patch
```

What the patch series buys us (from its own subjects):

| Patch | Effect |
|---|---|
| 0001 | `pe_analyze`: a code range is the adjacent code section |
| 0002 | `lift32`: MMX/SSE/SSE2 through one lane-generic emitter |
| 0003 | `RECOMP_STRICT_ICALL`: unresolved indirect calls get routed/reported |
| 0004 | `lift32`: `lahf` from lazy flag state; `fnstenv`/`fldenv` |
| 0005 | `RECOMP_BACKEDGE` before backward branches (guest thread fairness) |
| 0006/0007 | `native32` pre/post call hooks, `native32_add_name` |
| 0008 | `fnstsw`/`fnstenv` report x87 TOP; fair native scheduling |
| 0009 | explicit sign tests for signed conditions (MSVC `/O1` correctness) |
| 0010 | `RECOMP_NATIVE_RANGE`: run a VA range natively (bisection) |
| 0011 | `native32`: save SSE state (xmm + mxcsr) per guest thread |
| 0012 | optional non-volatile guest memory model |
| 0013 | `RECOMP_HOOK`: host hooks before chosen instructions |
| 0014 | `RECOMP_X87_WINDOW`: sliding window FPU state |
| MP2-0001 | SEH frames; `or`/`xor` condition semantics |

## Local environment

| Item | State |
|---|---|
| Python | 3.15.1 (`%LOCALAPPDATA%\Python\pythoncore-3.15-64\python.exe`) |
| capstone / unicorn / pefile | 5.0.7 / 2.1.4 / 2024.8.26 — installed |
| Git | 2.56.0.4 (installed 2026-11-19 via `winget install Git.Git`) |
| MSVC | **not installed** — no `vcvarsall`, no `cl.exe` anywhere. Blocks M2 |
| Android SDK / NDK | **not installed**. Blocks M3 |
| Java | OpenJDK 21.0.12 (Gradle-capable) |
| Ghidra | 12.2 PUBLIC at `%LOCALAPPDATA%\Programs\ghidra_12.2_PUBLIC` (optional) |

Reference toolchain in the source projects: MSVC 14.51 x86, Android NDK
28.2.13676358, LLVM/clang 22 via `clang32.bat` (`--target=i686-pc-windows-msvc`).
Match MSVC 14.51 if the lift hits codegen errors on a newer toolset.
