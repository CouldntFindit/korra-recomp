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

> **Work in progress.** The Windows path now boots, renders and plays: no Steam,
> text renders, and the first area runs at 39-58 fps (30 fps cap is the Android
> default). On Android, the S24 (Xclipse 940, stock driver) now reaches the
> title screen, main menu, intro cutscene and the tutorial area with working
> touch input, and the save path works — the frame-rate/thermal campaign for the
> first playable area is next. The milestone table and timeline below are the
> source of truth.

> **This repository contains no game content.** You must own and supply your own
> copy of the PC game. Builds are produced locally by each builder from their own
> files; nothing derived from the game binary is published here.

## Status

*Last updated 2026-11-22. Current focus: M4 audio, then the deferred performance block (all M3 rungs are recorded closed; the 30 fps gate line awaits the owner's rescope).*

| Milestone | What it means | State |
|---|---|---|
| M0 | Environment, toolchain pin, repo bootstrap, PE gate | **done** (2026-11-19) |
| M1 | Seeds + function catalog + full lift to C | **done**: 37,327/37,327 functions, 0 lift failures — see [`docs/LIFT_REPORT.md`](docs/LIFT_REPORT.md) |
| M2 | Boots to the main menu on Windows (no Steam, silent, movies skipped) | **done (2026-11-20)** — boots without Steam, text renders, new game runs into the first area at 39-58 fps uncapped; the crash and missing-text defects had one cause (the lifter's `pcmpistri` flag model). Log: [`docs/FINDINGS.md`](docs/FINDINGS.md) |
| M3 | Boots and plays the first area on an Android arm64 device | **in progress** - on the S24: title screen, main menu, New Game, difficulty select and the intro cutscene render, touch → XInput works end-to-end, the save path is closed (2026-11-22), post-cutscene stability is closed (2026-11-22), and the own-copy install is proven end to end (`docs/ANDROID_BRINGUP.md` §10.8): the picker opens on a fresh install, validation rejects a wrong folder before copying, 2.84 GB copies in 15-20 s with resume after a kill or a reboot, and the installed tree boots to the title. Remaining to the gate: the 30 fps frame-rate/thermal target for the first playable area (the measured ceiling is the single machine lock and the rescope is the owner's decision) |
| M4 | Audio: Wwise + CRI behind a host mixer | **sink done + measured (2026-11-21)** - engine keeps its Wwise/CRI mixer; host supplies XAudio2 2.7 + a WASAPI sink. Acceptance 1 passes (RMS -28.14 dBFS, peak -9.77 dBFS); test 2 fails because no Wwise event is posted, tests 3-6 blocked - [`docs/audio.md`](docs/audio.md) |
| M5 | Gameplay validation, framerate hooks, full playthrough | not started |

Parallel research tracks (each has its own document):

| Track | Scope | State |
|---|---|---|
| A | Audio stack inventory → the M4 spec | **done + independently confirmed** (44/44 assertions) — [`docs/audio.md`](docs/audio.md) |
| B | CPK archives + USM movies → M2.10 and optional movies | **done + independently confirmed** (45/45) — [`docs/assets.md`](docs/assets.md) |
| C | Ghidra subsystem triage → M2 hooks and M5 frame pacing | **done** — [`docs/quirks.md`](docs/quirks.md) |
| D | Host shims: `steam_api` + XAudio2 2.7 COM + WASAPI sink | **done**, selftest 60/60 - [`docs/xaudio2_callsites.md`](docs/xaudio2_callsites.md) |
| E | Publishing: mirror script, README/notices upkeep | **done** — [`docs/PUBLISHING.md`](docs/PUBLISHING.md) |

Exact toolchain pin and per-milestone findings: [`docs/TOOLCHAIN.md`](docs/TOOLCHAIN.md),
[`docs/FINDINGS.md`](docs/FINDINGS.md). Working rules for contributors and
agents: [`AGENTS.md`](AGENTS.md).

## Timeline

| Date | Milestone |
|---|---|
| 19.11.2026 | Project bootstrap: `LoK.exe` triaged (PE32 x86, no DRM), pcrecomp pinned as `korra-base`, the full binary lifted to portable C with zero lift failures; repository published as work-in-progress |
| 20.11.2026 | Windows gate (M2) passed: boots without Steam, text renders, the first area playable at 39-58 fps uncapped; lifter defects fixed on the way (`pcmpistri` flags, jump-table arms, Steam stats ABI) |
| 20.11.2026 | Android scaffold merged: GameActivity shell, SAF install flow, DXVK-native preparation, guest-memory window |
| 21.11.2026 | **First frame on the S24**: full graphics init over Vulkan — 191/191 imports bound, 37,327 functions dispatched, first `Present` with rendered content; fixed en route: emulated CPUID (Korra-0010), asset-pool teardown join, texture vtable slots, critical-section deadlock, object-table overflow, per-frame staging leak |
| 22.11.2026 | **Title screen, menu and cutscene on the S24**: touch→XInput drives title → menu → New Game → difficulty → intro cutscene; the last graphics blocker (an unimplemented cube-texture D3DX path that left the technique cache empty) fixed; 160 s / 8,664-frame runs, zero crashes. Next: the save path, then the first playable area |
| 22.11.2026 | **Save path closed on the S24**: the save device's path (`SHGetSpecialFolderPathA(CSIDL_PERSONAL)` + `Korra/`) now resolves to the directory the engine asked for instead of being re-homed under `GameData` - `files/game/Korra/GameData.dat` (7,856 bytes), New Game passes the save step into the intro cutscene, and the rofs boot notice the empty save directory caused is gone. The same probe on the Windows oracle agrees line for line |
| 22.11.2026 | **Post-cutscene stability on the S24**: the `reason=5` SIGABRT was a thread cap, not memory - a guest thread cost 4 MiB of the 256 MiB stack region, so the 65th guest thread aborted with no guest `[crash]` line (1.03 GB of the reported 1.2 GB RSS was Graphics/dmabuf; the native heap was 15 MB). A guest stack is now 1 MiB (the exe's own stack reserve) and returns to a free list when its thread ends. New Game walks the intro cutscene into the tutorial area, and the run survives 600 s with 72 live guest threads and 29 MiB of the stack region in use |
| 22.11.2026 | **Own-copy install (rung d) closed on the S24**: a fresh install opens the system folder picker by itself, validates that the picked folder is the game root (`LoK.exe` + `GameData\`) before copying a byte, and copies 820 files / 2.84 GB into internal storage in 15-20 s with progress and per-item timings. Interrupted copies resume by size-matched skip, including after a reboot (the persisted tree grant is re-checked); the installed tree boots to the title screen, the save path and Continue work in it, and the second launch goes straight into the game. The dev `run-as` path is unchanged |

## Building

Everything is generated from your own copy:

```powershell
# 1. Toolchain: the pinned pcrecomp (commit + patch series, see docs/TOOLCHAIN.md)
git clone https://github.com/sp00nznet/pcrecomp.git ..\pcrecomp
cd ..\pcrecomp; git checkout -b korra-base 35548b8
#    then apply the nfsu2 patch series 0001-0014 + the MP2 SEH patch
#    (recipe and patch list: docs\TOOLCHAIN.md)

# 2. Analysis copy of the game binary. LoK.exe's relocation directory is already
#    valid, so this verifies and copies it byte-for-byte; it would repair a
#    binary that is not. The copy lands in work\ (gitignored).
python tools\prepare_analysis_exe.py "D:\Games\The Legend of Korra\LoK.exe" work\LoK.analysis.exe

# 3. Seeds, catalog, lift (about 15 minutes on this machine)
python scripts\run_korra_lift.py catalog work\LoK.analysis.exe work\seeds_all.json work\catalog.json
python scripts\run_korra_lift.py lift    work\LoK.analysis.exe work\catalog.json src\recomp\gen

# 4. Windows host (needs the x86 C++ toolset)
powershell -ExecutionPolicy Bypass -File scripts\build_host.ps1 -Jobs 8
build\korra_recomp.exe "D:\Games\The Legend of Korra"
```

Requirements: Python 3.12+ with `capstone` and `pefile` for the committed tools
(`unicorn` / `keystone-engine` are used by pcrecomp's own self-checks); Visual
Studio 2022 with the x86 C++ toolset for the Windows host; Android NDK r27+ for
the Android target (r28c is the tested version; setup and versions in
[`docs/TOOLCHAIN.md`](docs/TOOLCHAIN.md)). Details in
[`docs/TOOLCHAIN.md`](docs/TOOLCHAIN.md).

## Publishing

Releases are **source-only** — no APKs, no binaries, no game data, no generated
recompiled code. This GitHub repository is the project's home and its hosted
copy; it is pushed at each milestone. A Codeberg failover script is kept in the
tree in case a second host is ever wanted:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\sync_codeberg.ps1 -DryRun   # check
powershell -ExecutionPolicy Bypass -File scripts\sync_codeberg.ps1           # push
```

It refuses a dirty tree, a non-Codeberg host, a non-fast-forward and any tracked
file the content guard rejects. Policy, setup, release checklist and
troubleshooting: [`docs/PUBLISHING.md`](docs/PUBLISHING.md).

## Legal

Unofficial fan interoperability project. Not affiliated with Activision,
PlatinumGames, Nickelodeon or Viacom. The Legend of Korra is a trademark of its
respective owners. Original code here is GPL-3.0; see [`LICENSE`](LICENSE) and
[`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).

## Development

This project is developed with AI coding agents under human direction. Most of
the tooling, documentation and analysis here was agent-produced, then verified —
independently, against the shipped game files — and merged at the owner's
decision. The verification runs and their results are recorded in
[`docs/FINDINGS.md`](docs/FINDINGS.md).
