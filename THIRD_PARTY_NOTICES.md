# Third-Party Notices

This repository contains only original code plus the credits below. **No game
content is distributed here, and no generated recompiled code is distributed
here** — builders produce both locally from their own copy of the game (see
`README.md` and `docs/PUBLISHING.md`).

Licenses below were checked against upstream on **2026-11-19** (repository
metadata and the license text itself, not a recollection) and extended on
**2026-11-20** with the Android build tooling. Where upstream has no
license file, this file says so plainly instead of guessing.

## 1. Used at build time or linked into the result

| Component | Upstream | License | How this project uses it |
|---|---|---|---|
| **pcrecomp** | <https://github.com/sp00nznet/pcrecomp> | MIT | The x86-32 → C static recompiler that produces all lifted code. Pinned at upstream commit `35548b8` plus a local patch series (`docs/TOOLCHAIN.md`). Cloned as a sibling directory; **no runtime header or source from it is vendored in this repository** — the build reads it from `..\pcrecomp` (`PCRECOMP`). Its MIT notice applies to the generated code and to the build. |
| **DXVK** / DXVK-Native | <https://github.com/doitsujin/dxvk> | zlib | Direct3D 9 → Vulkan translation, for the Windows swap and for Android (M3, not integrated yet). Built as a dependency; not vendored here. |
| **libadrenotools** | <https://github.com/bylaws/libadrenotools> | BSD-2-Clause | Optional loading of Turnip/custom Vulkan drivers on Adreno devices (M3). Dependency only. |
| **Turnip (Mesa)** | <https://gitlab.freedesktop.org/mesa/mesa> | MIT | Optional Vulkan driver fallback on Adreno. Shipped as a prebuilt driver blob at build time, never in this repository. |
| **SIMDe** | <https://github.com/simd-everywhere/simde> | MIT | Optional portable intrinsics if SIMD lowering needs them for arm64. Not currently referenced by any committed source file; it is also the reference implementation proposed for the `pcmpistri` arm64 fix (`docs/ANDROID_BRINGUP.md` §7.5). |
| **AndroidX GameActivity** | <https://developer.android.com/games/agdk/game-activity> | Apache-2.0 | `androidx.games:games-activity:4.4.2`, the Activity + native input/lifecycle glue used by the Android shell. Gradle dependency, not vendored. |
| **AndroidX AppCompat** | <https://developer.android.com/jetpack/androidx/releases/appcompat> | Apache-2.0 | `androidx.appcompat:appcompat:1.8.0`; GameActivity extends `AppCompatActivity`, so the theme and runtime are required. Gradle dependency, not vendored. |
| **Gradle wrapper** | <https://gradle.org> | Apache-2.0 | `android/gradle/wrapper/gradle-wrapper.jar` (pinned to Gradle 8.14.3) is committed so a fresh clone builds with `gradlew` and Android Studio does not silently regenerate it. It is unmodified upstream tooling, not a build product of this project. |
| **ngbrecomp** | <https://github.com/gamboozino/ngbrecomp> | GPL-3.0 | Methodology and Android arm64 engineering notes (`docs/TECHNICAL.md`: TLS slot trick, perf-hint API, 256 KiB signal stacks, Turnip via libadrenotools). Adapting GPL-3.0 code into this GPL-3.0 project is licence-compatible; anything taken from it stays under GPL-3.0. |
| **RogueSquadron64Recomp** | <https://github.com/MikeSemicolonD/RogueSquadron64Recomp> | GPL-3.0 | Agent-workflow model: crash-triage trees, dead-end ledger, skills. `AGENTS.md` here is written for this project from that pattern. |
| **totk-pc-recomp** | <https://github.com/valentderah/totk-pc-recomp> | MIT | Reference for scaled-lifter architecture and the source-only release model. Documents only; no code taken. |

## 2. Used as development oracles (not linked, not shipped)

These parsed or decoded the game's own files while we wrote our readers. None of
their code is in the build, and none of their output is committed.

| Component | Upstream | License | Role |
|---|---|---|---|
| **vgmstream** | <https://github.com/vgmstream/vgmstream> | ISC (see upstream `COPYING`; some bundled decoders carry their own notices) | Independent decode oracle for Wwise `.wem` / Vorbis output (Track A). |
| **wwiser** | <https://github.com/bnnm/wwiser> | **No license file at time of study** | Wwise `.bnk` HIRC dump used to cross-check our bank/event walk (Track A). Studied only; no code or data redistributed. |
| **ffmpeg / ffprobe** | <https://ffmpeg.org> | LGPL-2.1-or-later (GPL parts only in GPL builds) | Validated the USM demuxer's MPEG-1/ADX output (Track B). Command-line use only. |
| **Ghidra** | <https://github.com/NationalSecurityAgency/ghidra> | Apache-2.0 | Offline subsystem triage of `LoK.exe` (Track C). No Ghidra code or database is published; only the address-level notes in `docs/quirks.md`. |
| **pefile** | <https://github.com/erocarrera/pefile> | MIT | Imported by `tools/*.py` (PE parsing). |
| **capstone** | <https://github.com/capstone-engine/capstone> | BSD-3-Clause (custom-worded `LICENSE.TXT`) | Imported by `tools/*.py` (disassembly for the import map). |

Both Python modules are GPL-3.0-compatible, so importing them does not change
this project's license.

## 3. Studied only — no code taken

Kept here because "we looked at it" is information a future maintainer needs, and
because three of these are **not** usable as sources.

| Component | Upstream | License status | Why it is listed |
|---|---|---|---|
| **nfsu2-recompiled** | <https://github.com/codepdbh/nfsu2-recompiled> | **No license file** (README states MIT for the author's own code) | Bring-up ladder, headless-validation pattern, and the pcrecomp patch series `0001–0014`. The patches are applied at build time from a sibling clone; **they are not redistributed in this repository** and no source file here is a copy of theirs. |
| **maxpayne2pc-androidevolved** | <https://github.com/codepdbh/maxpayne2pc-androidevolved> | **No license file** | `desktop/pc_host/` validation pattern and one pcrecomp patch (SEH frames, or/xor conditions). Studied only. |
| **CriCodecs** | <https://github.com/Youjose/CriCodecs> | **No license file** — no `LICENSE`/`COPYING` and no `license` field in `pyproject.toml`; the Python package also publishes no wheels | CPK/USM/HCA format semantics. It does not build on this machine (MSVC STL/`consteval` failures), so our stdlib readers in `tools/cpk_*.py` and `tools/usm_demux.py` are independent implementations checked against the format as documented there. Nothing from it is linked or copied. |
| **JJL772/d3dx9** | <https://github.com/JJL772/d3dx9> | ToGL license (MIT-style, Valve 2011-2014 + J. Lorelli 2021) | A D3DX9 reimplementation considered for the four `d3dx9_43` imports. **Not needed**: `native32` links the real system DLLs (M2.2), so no D3DX shim ships. |
| **Steamless** | <https://github.com/atom0s/Steamless> | **CC BY-NC-ND 4.0** | The plan listed it as a standby for SteamStub. The game has **no DRM** (verified in triage), so it is not used. Note the license: NonCommercial + NoDerivatives means its code must never be adapted here. |
| **Goldberg / gse_fork** | <https://github.com/alex47exe/gse_fork> | LGPL-3.0 (upstream Goldberg) | Behavioural reference only for the 7-function `steam_api.dll` stub. The stub in `src/host/steam_stub.c` is hand-written from the measured call sites (`docs/xaudio2_callsites.md`); no Goldberg code is included. |
| **usmbreak** | <https://github.com/beer-psi/usmbreak> | 0BSD | Considered for USM demuxing. Not used: `tools/usm_demux.py` is ours. |
| **awesome-decomp-games** | <https://github.com/SubmitGame/awesome-decomp-games> | CC0-1.0 | Scene index; used to confirm this is the first Korra recompilation project. |

## 4. Excluded on purpose

- **No APKs, binaries, or compiled recomp code** are published (source-only
  releases; see `docs/PUBLISHING.md`).
- **No game assets**: `LoK.exe`, `GameData\`, `*.cpk`, `*.usm`, decoded audio or
  textures are rejected by `tools/check_content.py` and are absent from every
  commit.
- **No unlicensed or NonCommercial source is adapted.** Where a component has no
  license file (nfsu2, maxpayne2, wwiser, CriCodecs) or a NonCommercial/NoDeriv
  license (Steamless), this project re-derived the behaviour instead of copying
  code.

## 5. What this project itself is licensed under

Original code in this repository: **GPL-3.0** — see [`LICENSE`](LICENSE).

## 6. Trademarks and disclaimer

The Legend of Korra is a trademark of Viacom/Nickelodeon. The 2014 PC game was
developed by PlatinumGames and published by Activision. This project is an
unofficial interoperability research effort, not affiliated with, endorsed by or
supported by any of them. You must own the PC game to build it; no game content
is provided.
