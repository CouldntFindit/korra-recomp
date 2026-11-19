# The Legend of Korra — native recompilation (PC → Windows / Android ARM64)

Static recompilation of the 2014 PC release of **The Legend of Korra**
(PlatinumGames / Activision, `LoK.exe`, x86-32, Direct3D 9) into portable C.

```
LoK.exe ──pcrecomp lift32──▶ generated C ──▶ Win32 compatibility runtime
        ──▶ Direct3D 9 (DXVK-Native / Vulkan) ──▶ Windows x86 and Android arm64
```

No emulator, no interpreter for the game's own code: every function of the
original binary is translated ahead of time, and this project supplies the host
side (Win32, D3D9 surface, XInput, XAudio2, CRI/Wwise audio, Steam stubs).

> **This repository contains no game content.** You must own and supply your own
> copy of the PC game. Builds are produced locally by each builder from their own
> files; nothing derived from the game binary is published here.

## Status

| Milestone | What it means | State |
|---|---|---|
| M0 | Environment, toolchain pin, repo bootstrap, PE gate | **done** (2026-11-19) |
| M1 | Seeds + function catalog + full lift to C | **done**: 37,029/37,029 functions, 0 failures — see [`docs/LIFT_REPORT.md`](docs/LIFT_REPORT.md) |
| M2 | Boots to the main menu on Windows (no Steam, silent, movies skipped) | in progress |
| M3 | Boots and plays the first area on an Android arm64 device | not started |
| M4 | Audio (Wwise + CRI behind a host mixer) | not started |
| M5 | Gameplay validation, framerate hooks, full playthrough | not started |

Per-milestone findings and the exact toolchain pin: [`docs/TOOLCHAIN.md`](docs/TOOLCHAIN.md),
[`docs/FINDINGS.md`](docs/FINDINGS.md).

## Building

Everything is generated from *your* sale copy:

```powershell
# 1. Toolchain (mirror of the pinned pcrecomp, patches in docs/TOOLCHAIN.md)
git clone https://github.com/sp00nznet/pcrecomp.git ..\pcrecomp

# 2. Analysis copy of the game binary (relocation directory restored)
python tools\prepare_analysis_exe.py "D:\Games\The Legend of Korra\LoK.exe" work\LoK.analysis.exe

# 3. Seeds, catalog, lift
python scripts\run_korra_lift.py catalog work\LoK.analysis.exe work\seeds_all.json work\catalog.json
python scripts\run_korra_lift.py lift    work\LoK.analysis.exe work\catalog.json src\recomp\gen
```

Requirements: Python 3.12+ with `capstone`, `unicorn`, `pefile`, `keystone-engine`;
Visual Studio 2022 with the x86 C++ toolset (Windows host) and/or Android NDK
r27+ (Android target). Details in [`docs/TOOLCHAIN.md`](docs/TOOLCHAIN.md).

## Legal

Unofficial fan interoperability project. Not affiliated with Activision,
PlatinumGames, Nickelodeon or Viacom. The Legend of Korra is a trademark of its
respective owners. Original code here is GPL-3.0; see
[`LICENSE`](LICENSE) and [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).
