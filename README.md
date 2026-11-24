# The Legend of Korra â€” native recompilation (PC â†’ Windows / Android ARM64)

Static recompilation of the 2014 PC release of **The Legend of Korra**
(PlatinumGames / Activision, `LoK.exe`, x86-32, Direct3D 9) into portable C.

```
LoK.exe â”€â”€pcrecomp lift32â”€â”€â–¶ generated C â”€â”€â–¶ Win32 compatibility runtime
        â”€â”€â–¶ Direct3D 9 (DXVK-Native / Vulkan) â”€â”€â–¶ Windows x86 and Android arm64
```

No emulator, no interpreter for the game's own code: every function of the
original binary is translated ahead of time, and this project supplies the host
side (Win32, D3D9 surface, XInput, XAudio2, CRI/Wwise audio, Steam stubs).

> **Work in progress.** The Windows path now boots, renders and plays: no Steam,
> text renders, and the first area runs at 39-58 fps (30 fps cap is the Android
> default). On Android, the S24 (Xclipse 940, stock driver) now reaches the
> title screen, main menu, intro cutscene and the tutorial area with working
> touch input, and the save path works â€” the frame-rate/thermal campaign for the
> first playable area is next. The milestone table and timeline below are the
> source of truth.

> **This repository contains no game content.** You must own and supply your own
> copy of the PC game. Builds are produced locally by each builder from their own
> files; nothing derived from the game binary is published here.

## Status

*Last updated 2026-11-24. Current focus: M4 acceptance - the Windows voice feed is at parity, the mix's burst is identified and its NaN tail guarded; the device intro cutscene's own audio and the 6 ch movie voice's huge finite samples are the next two items (all M3 rungs are recorded closed; the 30 fps gate line is still open for a scope decision).*

| Milestone | What it means | State |
|---|---|---|
| M0 | Environment, toolchain pin, repo bootstrap, PE gate | **done** (2026-11-19) |
| M1 | Seeds + function catalog + full lift to C | **done**: 37,327/37,327 functions, 0 lift failures â€” see [`docs/LIFT_REPORT.md`](docs/LIFT_REPORT.md) |
| M2 | Boots to the main menu on Windows (no Steam, silent, movies skipped) | **done (2026-11-20)** â€” boots without Steam, text renders, new game runs into the first area at 39-58 fps uncapped; the crash and missing-text defects had one cause (the lifter's `pcmpistri` flag model). Log: [`docs/FINDINGS.md`](docs/FINDINGS.md) |
| M3 | Boots and plays the first area on an Android arm64 device | **in progress** - on the S24: title screen, main menu, New Game, difficulty select and the intro cutscene render, touch â†’ XInput works end-to-end, the save path is closed (2026-11-22), post-cutscene stability is closed (2026-11-22), and the own-copy install is proven end to end (`docs/ANDROID_BRINGUP.md` Â§10.8): the picker opens on a fresh install, validation rejects a wrong folder before copying, 2.84 GB copies in 15-20 s with resume after a kill or a reboot, and the installed tree boots to the title. Remaining to the gate: the 30 fps frame-rate/thermal target for the first playable area (the measured ceiling is the single machine lock, so the target is still open for review) |
| M4 | Audio: Wwise + CRI behind a host mixer | **both hosts feed the voice (2026-11-24)** - engine keeps its Wwise/CRI mixer; the host supplies XAudio2 2.7 + WASAPI (Windows) and XAudio2 2.7 + AAudio (Android). The missing piece was the engine's `IXAudio2VoiceCallback` (`OnVoiceProcessingPassStart` = the feed, `OnBufferEnd` = the mixer credit); both shims invoke it from a worker thread with the same credit guard. Device: Hw voice fed from 152 ms, menu music continuous (RMS -30..-35 dBFS) and audio through the New Game. Windows: voice fed from 655 ms, title/menu music from 66 s (peak -6.8..-27.6, RMS -24.9..-45.5 dBFS), 0 clipped frames in 8.35 M, selftest 60/60. The mix's burst is identified - NaN in the uncommitted tail of a ring slot the engine rewrites while queued, which its own limiter turns into +/-2^31 - and the host now zeros exactly the non-finite words (clipped frames 0.22 % -> 0.03 %, clip events 212 -> 5). Open: the 6 ch movie voice's finite ~1e18 samples (meter still 377 dBFS), the device intro cutscene's own audio, the CRI feed's movie-window underruns - [`docs/audio.md`](docs/audio.md) 10.10 |
| M5 | Gameplay validation, framerate hooks, full playthrough | not started |

Parallel research tracks (each has its own document):

| Track | Scope | State |
|---|---|---|
| A | Audio stack inventory â†’ the M4 spec | **done + independently confirmed** (44/44 assertions) â€” [`docs/audio.md`](docs/audio.md) |
| B | CPK archives + USM movies â†’ M2.10 and optional movies | **done + independently confirmed** (45/45) â€” [`docs/assets.md`](docs/assets.md) |
| C | Ghidra subsystem triage â†’ M2 hooks and M5 frame pacing | **done** â€” [`docs/quirks.md`](docs/quirks.md) |
| D | Host shims: `steam_api` + XAudio2 2.7 COM + WASAPI sink | **done**, selftest 60/60 - [`docs/xaudio2_callsites.md`](docs/xaudio2_callsites.md) |
| E | Publishing: mirror script, README/notices upkeep | **done** â€” [`docs/PUBLISHING.md`](docs/PUBLISHING.md) |

Exact toolchain pin and per-milestone findings: [`docs/TOOLCHAIN.md`](docs/TOOLCHAIN.md),
[`docs/FINDINGS.md`](docs/FINDINGS.md). Working rules for contributors and
agents: [`AGENTS.md`](AGENTS.md).

## Timeline

| Date | Milestone |
|---|---|
| 19.11.2026 | Project bootstrap: `LoK.exe` triaged (PE32 x86, no DRM), pcrecomp pinned as `korra-base`, the full binary lifted to portable C with zero lift failures; repository published as work-in-progress |
| 20.11.2026 | Windows gate (M2) passed: boots without Steam, text renders, the first area playable at 39-58 fps uncapped; lifter defects fixed on the way (`pcmpistri` flags, jump-table arms, Steam stats ABI) |
| 20.11.2026 | Android scaffold merged: GameActivity shell, SAF install flow, DXVK-native preparation, guest-memory window |
| 21.11.2026 | **First frame on the S24**: full graphics init over Vulkan â€” 191/191 imports bound, 37,327 functions dispatched, first `Present` with rendered content; fixed en route: emulated CPUID (Korra-0010), asset-pool teardown join, texture vtable slots, critical-section deadlock, object-table overflow, per-frame staging leak |
| 22.11.2026 | **Title screen, menu and cutscene on the S24**: touchâ†’XInput drives title â†’ menu â†’ New Game â†’ difficulty â†’ intro cutscene; the last graphics blocker (an unimplemented cube-texture D3DX path that left the technique cache empty) fixed; 160 s / 8,664-frame runs, zero crashes. Next: the save path, then the first playable area |
| 22.11.2026 | **Save path closed on the S24**: the save device's path (`SHGetSpecialFolderPathA(CSIDL_PERSONAL)` + `Korra/`) now resolves to the directory the engine asked for instead of being re-homed under `GameData` - `files/game/Korra/GameData.dat` (7,856 bytes), New Game passes the save step into the intro cutscene, and the rofs boot notice the empty save directory caused is gone. The same probe on the Windows oracle agrees line for line |
| 22.11.2026 | **Post-cutscene stability on the S24**: the `reason=5` SIGABRT was a thread cap, not memory - a guest thread cost 4 MiB of the 256 MiB stack region, so the 65th guest thread aborted with no guest `[crash]` line (1.03 GB of the reported 1.2 GB RSS was Graphics/dmabuf; the native heap was 15 MB). A guest stack is now 1 MiB (the exe's own stack reserve) and returns to a free list when its thread ends. New Game walks the intro cutscene into the tutorial area, and the run survives 600 s with 72 live guest threads and 29 MiB of the stack region in use |
| 22.11.2026 | **Own-copy install (rung d) closed on the S24**: a fresh install opens the system folder picker by itself, validates that the picked folder is the game root (`LoK.exe` + `GameData\`) before copying a byte, and copies 820 files / 2.84 GB into internal storage in 15-20 s with progress and per-item timings. Interrupted copies resume by size-matched skip, including after a reboot (the persisted tree grant is re-checked); the installed tree boots to the title screen, the save path and Continue work in it, and the second launch goes straight into the game. The dev `run-as` path is unchanged |
| 23.11.2026 | **Audio on the S24**: the Android host answers `CLSID_XAudio2` with a real IXAudio2 2.7 device (`src/host_android/xaudio2_android.c`), mixes the engine's slices with the engine's own output matrix and hands them to AAudio (`src/host/aaudio_sink.c`). The CRI movie audio that was silent on the phone is audible and measured: peak -9.8 dBFS, RMS -27.7 dBFS, 0 AAudio xruns after boot. Three defects were found by measurement and fixed: a byte-swapped GUID field that made every XAudio2 request answer `REGDB_E_CLASSNOTREG`, a WAV dump whose header patch rewound the file each second, and a mix cursor that rewound into a buffer CRI had already been told it could refill. The Wwise voice is still unfed, and the break now has a call path: the Hw output device's prefill is gated on an *active* entry in the game's sound table (`0xCA2438`/`0xCA243C`), which stays inert (key 0) - `docs/audio.md` Â§10.9 |
| 23.11.2026 | **The Wwise voice is fed on the S24**: the missing caller was the engine's own `IXAudio2VoiceCallback` - slot 0 (`0x0088B440`, OnVoiceProcessingPassStart) tops the source voice up from the 8-slot ring, slot 4 (`0x0088B3F0`, OnBufferEnd) decrements the mixer's credit counter and wakes it. The Android shim now runs an XAudio2-style worker that delivers both, guarded by `credits > queued` (without the guard the guest loop submitted 8.7M buffers in 90 s). Measured: Hw voice fed from 152-418 ms, continuous menu music in the dump from 73 s to the New Game (RMS -30..-35 dBFS), and the route's held taps still reach the tutorial. The old "inert sound table" reading was wrong (key 0 selects the default device, cfg is stored after Init). Open: NaNs/huge floats in the Wwise mix make the sink report peak 186.6 dBFS / rms nan and clip 0.2-0.3 % of the dump; the 5.1 cutscene is not yet measured on device - `docs/audio.md` Â§10.9, `docs/FINDINGS.md` |
| 24.11.2026 | **M4 acceptance: parity and the burst** - the Windows shim invokes the engine's IXAudio2VoiceCallback like the Android one (slot 4 per retired buffer, slot 0 with the credit guard), so the oracle's Wwise voice is fed too: title/menu music from 66 s, 0 clipped frames of 8.35 M, selftest 60/60, docs/audio.md \u00a79.4 test 2 measured instead of blocked. The mix's 2^31 burst is identified: quiet NaN in the *uncommitted tail* of a ring slot the engine rewrites while the host still mixes it (the slice's hash changes between submit and probe), which the engine's own saturating float-to-int limiter turns into +/-2^31. The host now zeroes exactly the non-finite words - not PCM in the engine's own IEEE float format - and leaves every finite value alone: clipped frames 0.2223 % -> 0.0290 %, clip events 212 -> 5, 0 xruns |

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

Releases are **source-only** â€” no APKs, no binaries, no game data, no generated
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
the tooling, documentation and analysis here was agent-produced, then verified â€”
independently, against the shipped game files - and merged after review. The
verification runs and their results are recorded in
[`docs/FINDINGS.md`](docs/FINDINGS.md).

