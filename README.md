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

*Last updated 2026-11-19.*

| Milestone | What it means | State |
|---|---|---|
| M0 | Environment, toolchain pin, repo bootstrap, PE gate | **done** (2026-11-19) |
| M1 | Seeds + function catalog + full lift to C | **done**: 37,029/37,029 functions, 0 lift failures — see [`docs/LIFT_REPORT.md`](docs/LIFT_REPORT.md) |
| M2 | Boots to the main menu on Windows (no Steam, silent, movies skipped) | **D3D9 device, first frame and the intro movies render**; the boot then stalls at the movie-to-menu transition, and the silent XAudio2 stub blocks it when enabled. Ladder state in [`docs/HOST.md`](docs/HOST.md), log in [`docs/FINDINGS.md`](docs/FINDINGS.md) |
| M3 | Boots and plays the first area on an Android arm64 device | not started (NDK not installed) |
| M4 | Audio: Wwise + CRI behind a host mixer | spec ready ([`docs/audio.md`](docs/audio.md)), implementation not started |
| M5 | Gameplay validation, framerate hooks, full playthrough | not started |

Parallel research tracks (each has its own document):

| Track | Scope | State |
|---|---|---|
| A | Audio stack inventory → the M4 spec | **done + independently confirmed** (44/44 assertions) — [`docs/audio.md`](docs/audio.md) |
| B | CPK archives + USM movies → M2.10 and optional movies | **done + independently confirmed** (45/45) — [`docs/assets.md`](docs/assets.md) |
| C | Ghidra subsystem triage → M2 hooks and M5 frame pacing | **done** — [`docs/quirks.md`](docs/quirks.md) |
| D | Host shims: `steam_api` + silent XAudio2 2.7 COM | **done**, selftest 54/54 — [`docs/xaudio2_callsites.md`](docs/xaudio2_callsites.md) |
| E | Publishing: Codeberg mirror, README/notices upkeep | **done** (publishing half; Winlator fallback not started) — [`docs/PUBLISHING.md`](docs/PUBLISHING.md) |

Exact toolchain pin and per-milestone findings: [`docs/TOOLCHAIN.md`](docs/TOOLCHAIN.md),
[`docs/FINDINGS.md`](docs/FINDINGS.md). Working rules for contributors and
agents: [`AGENTS.md`](AGENTS.md).

## Building

Everything is generated from *your* sale copy:

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
the Android target (not set up yet). Details in
[`docs/TOOLCHAIN.md`](docs/TOOLCHAIN.md).

## Publishing

Releases are **source-only** — no APKs, no binaries, no game data, no generated
recompiled code. A mirror script exists for when a second copy is wanted:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\sync_codeberg.ps1 -DryRun   # check
powershell -ExecutionPolicy Bypass -File scripts\sync_codeberg.ps1           # push
```

**No mirror is configured today** — the project is local-only by decision of the
project owner, and the script exits harmlessly when no remote exists. When one is
wanted, it refuses a dirty tree, a non-Codeberg host, a non-fast-forward and any
tracked file the content guard rejects. Policy, one-time setup, release checklist
and troubleshooting: [`docs/PUBLISHING.md`](docs/PUBLISHING.md).

## Legal

Unofficial fan interoperability project. Not affiliated with Activision,
PlatinumGames, Nickelodeon or Viacom. The Legend of Korra is a trademark of its
respective owners. Original code here is GPL-3.0; see [`LICENSE`](LICENSE) and
[`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).
