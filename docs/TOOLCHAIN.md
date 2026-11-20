# Toolchain pin (verified 2026-11-19)

`pcrecomp` moves fast and deletes branches, so the pin is a **commit**, not a
branch name. The `nfsu2-fixes` branch named in older notes no longer exists and
its commit `b278e8a` is not fetchable — this pin supersedes it.

| Item | Value |
|---|---|
| Repo | https://github.com/sp00nznet/pcrecomp (note: `sp00nznet`, not `sp00z`) |
| Base commit | `35548b8` — *disasm32: straight-line code longer than the scan window is decoded to its end (#50)*, upstream HEAD on 2026-10-04 |
| Branch used here | `korra-base` (local) |
| Branch head | **`9459f22`** (2026-11-20) - `35548b8` + patches `0001-0014` + MP2-0001 + Korra-0001-0008 |
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
| **Korra-0001** (ours, `1a8cb93`) | `lift32`: REP/REPNE prefix on RET is a hint, not an unimplemented instruction (`F3 C3` in LoK's CRT startup) |
| **Korra-0002** (ours, `346a509`) | `lift32`: `cmpxchg` / `cmpxchg8b` (MSVC interlocked ops, 162 sites) and capstone's `fcomi`/`fcompi`/`fucomi`/`fucompi` names (430 sites) |
| **Korra-0003** (ours, `d3f7803`) | `lift32` + `recomp_types.h`: `bsf` / `bsr` and portable `RECOMP_BSF/BSR` |
| **Korra-0004** (ours, `b0b2ce2`) | `lift32` + `recomp_types.h`: `pcmpistri` carries **SF and OF** as well as ZF/CF, packed into `FK_EFLAGS` via `recomp_pcmp_eflags`. Without it every `ja`/`jae`/`jno`/`jns` after a `pcmpistri` was a constant, so MSVC's SSE4.2 `strstr` could never accept a match (6 sites in LoK; 0 lift failures) |
| **Korra-0005** (ours, `dc18f83`) | `recomp_types.h`: portable `pcmpistri` rewritten — all four data formats (word mode `0x0D` included), the equal-ordered validity window (empty needle ⇒ index 0/CF=1; needle must fit before `b`'s null), the equal-each lane mask along the longer operand, masked-polarity XOR on `b`'s valid lanes, and hardware ZF/CF/SF/OF (SF = "any null in `a`", not IntRes2[15]). Word mode returns the element index; the lifted code applies its own `*2`. **No relift** — recompile only |
| **Korra-0006** (ours, `5cfccf3`) | `lift32` + `recomp_types.h`: `FCOMI/FCOMIP/FUCOMI/FUCOMIP` publish their EFLAGS image via `recomp_fcomi_eflags`, so `LAHF`/`PUSHFD` after the compare report ZF/PF/CF instead of the flags of an earlier instruction (8 real `fucompi; fstp st(0); lahf` sites). **Codegen change - requires a relift** (37,029/37,029, 0 failures) |
| **Korra-0007** (ours, `82495cb`) | `native32.c` runtime only: the per-thread machine state is a `__declspec(thread)` pointer instead of a `TlsGetValue` slot (three API calls and a LastError clobber per bridge crossing), `mach_unlock` wakes only when a ticket is actually queued, and a waiter spins 4096 times before sleeping. **No relift** - recompile only. Measured neutral in Korra's first area (45.3 fps avg vs 44.9) |
| **Korra-0008** (ours, `9459f22`) | `disasm32.py`: `table_entries` reads a jump table **below** its displacement as well as above it. A negative index (CRT memcpy: `jmp [ecx*4 + 0x77A8C4]`, ecx -7..0) keeps every arm below, so those switches lifted to `RECOMP_ITAIL` and resolved to nothing at runtime - Korra's memcpy remainder silently copied nothing. Guards: contiguous code run only, no entry pointing into the table itself, every arm within 0x40000 of the table. **Re-catalog + relift**: 37,029 -> **37,327** functions (+298 reachable only through those arms), 37,327/37,327 lifted, 0 failures |

## Local environment

| Item | State |
|---|---|
| Python | 3.15.1 (`%LOCALAPPDATA%\Python\pythoncore-3.15-64\python.exe`) |
| capstone / unicorn / pefile | 5.0.7 / 2.1.4 / 2024.8.26 — installed |
| Git | 2.56.0.4 (installed 2026-11-19 via `winget install Git.Git`) |
| MSVC | **VS 2022 Build Tools 17.14.45**, MSVC **14.44.35207**, Windows SDK **10.0.26100.0** — installed to `<workspace>\msvc` (custom path, at the user's request so it can be deleted as one folder) |
| CMake / Ninja | none — `scripts/build_host.ps1` drives `cl`/`link` directly (vcvarsall x86 + response file) |
| Android SDK / NDK | **installed 2026-11-20** — see the "Android toolchain" section below (blocks M3) |
| Java | OpenJDK 21.0.12 (Gradle-capable) |
| Ghidra | 12.2 PUBLIC at `%LOCALAPPDATA%\Programs\ghidra_12.2_PUBLIC` (optional) |

Reference toolchain in the source projects: MSVC 14.51 x86, Android NDK
28.2.13676358, LLVM/clang 22 via `clang32.bat` (`--target=i686-pc-windows-msvc`).
Match MSVC 14.51 if the lift hits codegen errors on a newer toolset.

## Android toolchain (M3, installed 2026-11-20)

Installed at the project root, the same way MSVC was, so the whole SDK can be
deleted as one folder: `<workspace>\android-sdk`.
Cloud sync is disabled on this machine, so keeping it inside the workspace
does not upload ~4 GB.

| Item | Exact version | Path |
|---|---|---|
| Command-line tools | `1.1.16919734` (Android CLI; `sdkmanager` reports `cmdline-tools 23.0.0`, deprecated but working) | `android-sdk\cmdline-tools\latest` |
| platform-tools | `37.0.3` | `android-sdk\platform-tools` (`adb.exe`) |
| SDK platform | `android-36`, package revision 2 | `android-sdk\platforms\android-36` |
| Build tools | `36.1.0` | `android-sdk\build-tools\36.1.0` |
| NDK | **`28.2.13676358` (r28c)** — the same release the nfsu2 reference toolchain uses | `android-sdk\ndk\28.2.13676358` |
| CMake (SDK package) | `3.31.5` | `android-sdk\cmake\3.31.5` |
| JDK (Gradle) | Temurin `21.0.12.101` | `C:\Program Files\Eclipse Adoptium\jdk-21.0.12.101-hotspot` |
| Gradle | `8.14.3` (wrapper, committed) | `android\gradle\wrapper` |
| Android Gradle Plugin | `8.13.2` | `android\build.gradle.kts` |
| GameActivity / AppCompat | `4.4.2` / `1.8.0` | `android\app\build.gradle.kts` |

Reproduce the SDK install (PowerShell; the `/` package form matters, see the
notes below):

```powershell
$sdk = "<workspace>\android-sdk"
$env:JAVA_HOME = "C:\Program Files\Eclipse Adoptium\jdk-21.0.12.101-hotspot"
Invoke-WebRequest -UseBasicParsing `
  -Uri "https://dl.google.com/android/repository/commandlinetools-win-16111833_latest.zip" `
  -OutFile "$env:TEMP\cmdline-tools.zip"
Expand-Archive "$env:TEMP\cmdline-tools.zip" "$env:TEMP\cmdline-unpack"
Move-Item "$env:TEMP\cmdline-unpack\cmdline-tools" "$sdk\cmdline-tools\latest"
& "$sdk\cmdline-tools\latest\bin\sdkmanager.bat" --licenses   # answer y to all
& "$sdk\cmdline-tools\latest\bin\sdkmanager.bat" `
    "platform-tools" "platforms/android-36" "build-tools/36.1.0" `
    "ndk/28.2.13676358" "cmake/3.31.5"
```

Three notes worth their kilobytes:

1. `sdkmanager.bat` re-parses its arguments through `cmd`, which splits on `;`,
   so `"ndk;28.2.13676358"` arrives as two packages ("Package ndk not found").
   Use the `/` form shown above.
2. `JAVA_HOME` on this machine points at a **JDK 8** install
   (`%USERPROFILE%\Downloads\jdk8\...`), which AGP cannot use. `java` on PATH is
   JDK 21, so the build works only when `JAVA_HOME` is set — `scripts\android_build.ps1`
   does it, and any manual `gradlew` run needs it too.
3. The installed command-line tools warn that `sdkmanager` is deprecated in
   favour of the new `android` binary. The warning is harmless; the old tool is
   what Google's install docs still describe. AGP 8.13 also prints "SDK XML
   version 4" warnings against these tools — also harmless.

Verified build on this machine (2026-11-20):

```
cd android
.\gradlew.bat :app:assembleDebug
BUILD SUCCESSFUL in 1m 56s (cold, 38 tasks)
app/build/outputs/apk/debug/app-debug.apk   9.8 MB
lib/arm64-v8a/libkorra.so                   73 KB, PT_LOAD aligned 0x4000
```

Android-specific plan, shell choice, DXVK path and the `pcmpistri` assessment:
`docs/ANDROID_BRINGUP.md`.
