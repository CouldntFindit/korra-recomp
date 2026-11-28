# Findings log (append-only)

## 2026-11-19 — M0/M1 session

### Environment

- `git` absent → installed 2.56.0.4 via winget (user scope, no admin needed after prompt).
- MSVC **absent**: `vswhere.exe` exists, `%ProgramFiles(x86)%\Microsoft Visual Studio\2022` is an empty shell, no `cl.exe` on disk, no clang, no Rust. → M2 blocked until VS 2022 + "Desktop development with C++" (x86 toolset) is installed.
- Android SDK/NDK absent; Java 21 present. → M3 blocked.
- Python 3.15.1 with capstone 5.0.7, unicorn 2.1.4, pefile 2024.8.26.

### Toolchain pin (correction to the plan)

- Plan's `sp00znet/pcrecomp` was a typo: the repo is **`sp00nznet/pcrecomp`**.
- The `nfsu2-fixes` branch **no longer exists** and its commit `b278e8a` is not
  fetchable (`upload-pack: not our ref`). pcrecomp rewrites history.
- Usable base found from nfsu2's own `docs/PCRECOMP_BASELINE.md`: **`35548b8`**
  (2026-10-04). All 14 nfsu2 patches and the MP2 SEH patch apply cleanly there.
- Current `main` (2026-11-18) is *not* a drop-in: only patch 0001 applies, the
  rest conflict. Pinned base is `korra-base` = `35548b8` + 0001–0014 + MP2-0001.

### PE facts (confirmed independently of the earlier triage)

- SHA-256 matches the plan: `497eedaf…4f683`, 6,764,032 bytes.
- Sections: `.text` 0x4fb9ba vsize (entropy 6.565), `.rdata` 0xb3288, `.data`
  0x50e688, `.rsrc`, `.reloc` 0x7d952.
- **Base-relocation directory is already valid** (RVA 0xac4000, chain 0x544c4,
  1,420 blocks, 166,265 HIGHLOW entries + 697 ABSOLUTE paddings). Unlike NFSU2,
  `LoK.exe` needs **no repair** — the M0 gate is a byte-identical copy.
  `prepare_analysis_exe.py` handles both cases and verified the copy hashes equal.
- 194 imports in 12 DLLs — matches the plan.
- RTTI: 1,367 `.?AV` names, 1,880 CompleteObjectLocators.

### Seeds (M1)

| Source | Result |
|---|---|
| `vtable_scan.py` | 1,996 vtable candidates (.rdata 1,951 / .data 45), 8,569 distinct methods, slots min 3 / median 7 / max 860 → `work/vtable_seeds.json` |
| `rtti.py` | 1,369 type descriptors, 1,880 COLs → 1,880 vtables, 1,225 classes, 4,735 virtual methods (4,452 attributable) → `work/rtti_seeds.json`, `work/rtti.json` |
| merged | **8,865 unique seeds** → `work/seeds_all.json` |

Rich C++ surface is a good sign: every vtable slot is a known function entry,
which is exactly the class of function the prologue/branch scans miss.

### Catalog (M1)

| Stage | Result |
|---|---|
| Discovery rounds | 6 to a fixpoint |
| Functions | **37,029** (`start` 25,188 / `alias` 11,841 / thunks 107) |
| C++ EH stubs folded as aliases | 1,958 |
| Data-pointer-only functions | 2,146 |
| Branch targets given a body | 3,020 |
| Wall time | 622.7 s |

Korra lands at 37k functions vs NFSU2's 27.7k — the extra ~9k is the alias bodies
(labels inside other functions that still need a dispatchable entry) plus a much
larger C++ virtual surface (1,880 vtables).

### Lift (M1 gate: PASSED)

| Stage | Result |
|---|---|
| Functions lifted | **37,029 / 37,029** |
| Failures | **0** |
| Output | 75 C files, 12,016,872 lines, 613 MB |
| Wall time | 229.5 s |

No Korra-specific lifter fix was needed — the NFSU2 patch series covered the
whole `.text` as-is. Details: `docs/LIFT_REPORT.md`.

## 2026-11-19 — M2.1 / M2.2 (Windows host)

### Compile (M2.1) — PASSED

- 75 TUs, 613 MB, **517 s** with `cl /O1 /MP8` (start to link error).
- **Gotcha, fixed in our driver:** every generated file must `#define
  RECOMP_GENERATED_CODE` *before* `#include "recomp_types.h"`. That macro is what
  maps `eax`/`ecx`/`esp`… onto the runtime's register file; without it each TU
  dies with ~100 `C2065: undeclared identifier` errors. Relift after the fix: 290 s.

### Link (M2.2) — PASSED

- **Only one unresolved symbol: `recomp_unimpl`.** It is *declared* by
  `recomp_types.h` ("a host may define it; this weak default prints and aborts")
  but no runtime file defines it — MSVC has no weak symbols, so every host must.
  Written in `src/host/shims.c`: prints the guest VA + instruction, dumps the last
  indirect calls, aborts.
- Output: `korra_recomp.exe`, 131.7 MB, x86, subsystem 3.
- **`/BASE:0x70000000` does not load on this machine** — the loader fails with
  error 308 (*"the subsystem needed to support the image type is not present"*)
  for a 126 MB image at that base. `0x60000000` and `0x10000000` both load; we
  use **0x60000000** (image 0x60000000–0x67DEEFFF; the guest needs only
  0x400000–0xF42000, so there is no overlap either way). `hello.exe` and a 126 MB
  blob exe with the *same* flags both ran at 0x70000000 — it is the combination
  of that base and this image, not the flags alone.

### First run

```
korra: game dir  ...\The Legend of Korra
korra: dispatch   37029 functions
korra: mapped     0x00400000-0x00F42000
korra: entering guest at 0x0077C3B7
[bind] 0x00400000: 185 native, 0 guest, 9 shimmed, 0 unresolved
```

**All 194 imports resolved, 0 unresolved, Steam stubbed (9 = 7 steam + GetProcAddress
+ built-in CreateThread).** The guest then reached `0x779FF0` and stopped on:

```
[unimpl] 0x00779FF0: repz ret
```

`F3 C3` — `repz ret`, an MSVC branch-prediction hint on RET, emitted for the
once-called CRT startup paths. The CPU ignores REP on RET; capstone reports it as
part of the mnemonic, so `lift32` missed its `ret` case and emitted
`RECOMP_UNIMPL`. Patched locally in `lift32._lift_instruction` (repz/repne/rep
prefixes on ret normalize to `ret`), committed to the pcrecomp tree as
`1a8cb93`, and it relifts to `RECOMP_FLAGS_OUT(); esp += 4; RECOMP_RETURN();`.
Only **1 occurrence in 12 M lines**, and it was on the boot path — a good example
of why a lift failure count of 0 does not mean "nothing was unimplemented".

### Known instruction debt (measured, not guessed)

`grep RECOMP_UNIMPL` over the 12 M lines: **801 sites**. The lift reported 0
*failures*, so these are instructions the lifter consciously has no case for —
they abort only if reached. Top groups:

| Instruction | Sites |
|---|---|
| `fcompi st(1)` / `fcomi st(1)` / `fucompi st(1)` (x87 compare-and-pop) | 430 |
| `lock cmpxchg r/m, reg` (dword/byte, several bases) | 152 |
| `lock cmpxchg8b` | 10 |
| `insb`/`insd`/`outsb` (port I/O) | 34+ (likely dead in the PC build) |
| `pcmpistri` (SSE4.2 string compare) | 10 |
| `ljmp`/`int 0x29` junk from data decoded as code | 13 |

`fcomi*` and `lock cmpxchg` are real, hot-path instructions; expect to add them
to the lifter as Korra patches during M5. They are not blocking M2.

### Lifter patches needed by Korra (found by running, not by lifting)

| Patch | Symptom | Scope |
|---|---|---|
| `repz ret` = `ret` | CRT startup aborts on `F3 C3` at 0x779FF0 | 1 site, on the boot path |
| `cmpxchg` / `cmpxchg8b` | aborts at 0x5218F7 (`lock cmpxchg byte ptr [edx], cl`) — MSVC's interlocked ops, thread-safe statics, `/MT` lazy init | 162 sites |
| `fcomi`/`fcompi`/`fucomi`/`fucompi` | capstone spells FCOMI/FCOMIP this way; the lifter only knew `fcomip` | 430 sites, every optimised float compare |

RECOMP_UNIMPL sites after the first two fixes: 801 → 627 (the rest are
`insb`/`outsb` port I/O, `pcmpistri`, and data decoded as code).

### Host-level blockers found by running (M2.3/M2.4)

Each one was invisible in the lift stats and obvious from a run + trace.

| Symptom | Cause | Fix |
|---|---|---|
| `[Hw::rofs::fst] ファイルサイズテーブルの作成に失敗しました` in `message_box.txt`, then exit | `GetModuleFileNameA` returned the **host** path (`…\build\korra_recomp.exe`), so the engine looked for `…\build\GameData\*.cpk` | shim `GetModuleFileNameA/W`, `GetModuleHandleA/W` |
| window created, then orderly `SteamAPI_Shutdown` + `UnregisterClassA` + `ExitProcess(0)` with **no** file or D3D call ever made | `DirectInput8Create` returned `E_INVALIDARG` because the guest passed its load base `0x400000` as `hInstance` | the guest's "own module" is the **host HMODULE** (`GetModuleHandleA(NULL)`), not `0x400000`; `GetModuleFileName*` still answers with LoK.exe's path |
| dies the moment a named thread starts | MSVC thread-name idiom: `RaiseException(0x406D1388)` with no debugger to catch it | VEH returns `EXCEPTION_CONTINUE_EXECUTION` for that code |
| `[unimpl] 0x0077BCB7: bsf eax, ecx` | BSF/BSR absent from the lifter | lifter patch + `RECOMP_BSF/BSR` helpers in `recomp_types.h` (written as loops, not intrinsics: the same generated C compiles on MSVC x86 and clang arm64) |

Unhandled exceptions now print code, faulting address, guest function, and the
last 8 indirect calls (`korra_install_fault_handlers` in `runtime_glue.c`).

Lifter gaps found by running, in order: `repz ret` → `cmpxchg`/`cmpxchg8b` →
`fcomi`/`fcompi` → `bsf` → `pcmpistri`. `RECOMP_UNIMPL` sites in the generated
code: **801 → 114**. What remains is port I/O (`insb`/`insd`, dead in the PC
build), and bytes from data decoded as code (`ljmp`, `int 0x29`, `das`, `aas`).

### `pcmpistri` (SSE4.2 string compare) — how it was done

MSVC's CRT uses it for its `strlen`/`strchr`/`strcmp` fast paths, so it is on the
boot path. `tests/pcmpistri_probe.c` + `tests/pcmpistri_selftest.c` compare an
implementation against the **host CPU's own instruction** (`_mm_cmpistri` and
friends): the host is x86 with SSE4.2, so the oracle is free and exact.

The implicit-length rules turned out to be subtle enough that a from-the-SDM
re-derivation did not match the hardware (36,397 mismatches in 200k cases), so:

- on `_MSC_VER && _M_IX86` the runtime **calls the real instruction** (switch over
  the imm8 values MSVC emits) — exact by construction, 200k/200k cases pass;
- the portable body is kept for arm64 and is **not yet verified**; validate it on
  the phone with the same differential test before trusting M3 output.

### M2.6 / M2.7 — D3D9 device and the first frame: PASSED

Proof (not committed — a game frame is game content; captures live in
`build/shots/`, gitignored):

| Shot | What it shows |
|---|---|
| `build/shot.png` (30 s) | 3D logo scene rendering: orange parallelograms on white |
| `build/shots/20s.png` (audio disabled) | **an intro movie playing**, with its subtitle and progress bar |

The device comes up on the real `d3d9.dll`; the engine creates textures through
`D3DXCreateTexture` / `D3DXCreateTextureFromFileInMemoryEx` (DDS from the CPKs),
opens `GameData\movie_logo\*.usm` and renders them.

### M2.8 / M2.9 — Steam and XAudio2 stubs: live, but the audio stub blocks boot

- 13–14 shims bound, 181–184 native imports, **0 unresolved**.
- Steam: `SteamAPI_Init` → true, `SteamUser()`/`ISteamUser::GetSteamID` answered,
  `SteamUserStats()` answered.
- XAudio2: `CoCreateInstance(XAudio2)` → stub IXAudio2 2.7, `Initialize` → 0,
  `RegisterForCallbacks` → 0, `CreateSourceVoice` → a silent voice.
- **The stub is where the boot stops when it is enabled.** With audio on, the
  frame freezes on the first logo and the process stays alive; the trace shows an
  engine thread polling a critical section forever and `RegisterForCallbacks`
  never firing (no `OnBufferEnd`, no buffer progress). With
  `KORRA_NO_AUDIO=1` (added to `shims.c`; it leaves `CoCreateInstance` alone so
  audio init fails like a machine with no device) the intro movies **play**.
  → The XAudio2 stub needs buffer completion/progress callbacks; that is Track D
  territory (`src/host/xaudio2_stub.c`), reported here rather than edited.

### After the movies: window closes and the process waits

With audio disabled the movies play, then the window is destroyed and the process
sits at ~1.2 M trace lines doing nothing but `Enter/LeaveCriticalSection` and
`WaitForSingleObject`. No `[fatal]`, no `[unimpl]`, no `ExitProcess`. Different
runs reach different depths before this (one exited at 80 s), which smells like a
race in the movie→menu transition rather than a hard fault. Next session: watch
`sub_007F7EF0`/`sub_007F7F90` (the polling pair) and the movie-thread join with
`recomp_trace --watch`, and check whether the menu's asset group ever gets
requested.

### Target devices named by the user: **Galaxy S24 (Exynos 2400, Xclipse/RDNA)** and
  **Galaxy S25 (Snapdragon 8 Elite, Adreno)**. Requirement: do not ship an
  Adreno-only build. Plan: DXVK-Native over stock Vulkan 1.1+ (works on both),
  with Turnip/libadrenotools as an *option* on Adreno, and no Xclipse-specific
  path required. Verify on both before calling M3 done.

## 2026-11-19 — Track C (Ghidra subsystem triage)

Read-only Ghidra 12.2 pass over `LoK.exe` (REA headless, ephemeral, no DB kept).
Output: **`docs/quirks.md`** (~70-address hook index, decoded constants, port notes).

- **Plan correction:** `framerate_d` / `framerate_n` are **CRI Sofdec movie-header
  fields** (fps numerator/denominator), read with `mpeg_codec`, `total_frames`,
  `ixsize`, `alpha_type`, `color_space` in `FUN_007dd5e0` / `FUN_007dfd40`. They are
  *not* the render-rate config. Likewise `Progress/Draw/MotionFrameRateType` are
  per-action animation fields (`FUN_006b4440` binder), not a global frame rate.
- **The engine ships its own 30/60 fps switch:** `FUN_0074c580` (0x74c580) picks
  `1/30` vs `1/60` from `DAT_00e4d210`; the limiter `FUN_00754840` (0x754840)
  sleeps against `[frameMgr+0x58]` in **1/3-ms units** (100 = 30 fps, 50 = 60 fps);
  `GetTick()` `FUN_007547b0` (0x7547b0) is QPC x `1/freq` x 3. `DAT_00e4d01c` bit 2
  disables the limiter. Main loop `FUN_00540e10` (0x540e10).
- **D3D9:** `Direct3DCreate9(0x20)` at 0x7599ef; presentation params built at
  0x758f70 — X8R8G8B8, DISCARD, `EnableAutoDepthStencil=0` (own depth RTs),
  D3DPRESENTFLAG_LOCKABLE_BACKBUFFER, MSAA `{0,2,4}`; **VSync ON in fullscreen,
  OFF (INTERVAL_IMMEDIATE) windowed**; fullscreen refresh pinned to nearest 60 Hz.
  `CreateDevice` HAL (0x44) -> HAL (0x24) -> **REF** fallback. The renderer also
  probes **`D3DFMT_NULL`** render targets — relevant to the DXVK/Vulkan step.
- **Input:** XInput poll `FUN_007606d0` (0x7606d0) for 4 pads, 0x24-byte state
  blocks, full remap to engine button bits + edge detection at 0xe4e554/558/55c —
  the M3 touch HUD should feed exactly this layout. DInput8 init 0x760320.
  One shared HWND global `0xe4e52c` for D3D + DInput + window styles: set it once.
- `OleLoadPicture` (0x75e530) is only the **window-icon loader** -> stub it.
- Ghidra reports 21,132 procedures vs pcrecomp's 37,029; expected — Ghidra's default
  analysis does not chase data-referenced function pointers. No contradiction.
- Not verified: which value `DAT_00e4d210` settles on at runtime (the focus branch
  reads inverted), and whether the simulation step is tied to the limiter interval.
  Both need one log line on the first working Windows build. See `docs/quirks.md` §8.

## 2026-11-19 - Track A (audio stack inventory)

Read-only pass over `GameData\sound\` (783 files / 171.6 MB) plus `LoK.exe`
string literals. Output: **`docs/audio.md`** (inventory, container formats, the
bank->event map, and the M4 host-mixer spec) and `tools/audio_inventory.py`
(stdlib-only: `scan` / `wem` / `bank` / `wai` / `names` / `prefix`).

- **Everything is Wwise 2013.1/2** (bank generator version `0x58`): 779 `.wem`
  (777 Vorbis, 1 IMA-ADPCM, 1 PCM16), `Init.bnk`, `BGM.bnk`, `WwiseInfo.wai`,
  `SeFootstep.bin`. **No HCA/ADX/ACB content exists in `sound\`** - the CRI
  NcVoice/HCA-MX strings in the exe are linked but unused for content, so M4
  needs no HCA decoder (CPK interior still unconfirmed).
- Content: 6,257 s (104 min) of audio. Music is 4/5/6-channel (quad / 5.0 / 5.1)
  at 48 kHz; voice is mono 32 kHz, English(US) only.
- **`BGM.bnk` media are prefetch heads, not audio.** 119 of 120 DIDX entries have
  a RIFF header claiming more bytes than stored, and for all 95 that also exist as
  `stream\<id>.wem` the bank copy is a byte-identical prefix. Only the single
  in-memory source is complete. A host that serves bank media alone plays ~1 s of
  every cue.
- **Wwise IDs are FNV-1 32 of the lowercase name** (verified: `init` ->
  1355168291, `bgm` -> 412724365). Hashing every string literal in `LoK.exe`
  named **977 of 2581** events in `WwiseInfo.wai` and **97 of 295** music events.
- `WwiseInfo.wai` = 16-byte header + 2,581 x 20-byte records (u32 id, f32 value,
  3 flag words), sorted by id, contains all 295 `BGM.bnk` events -> it is the
  game-wide event table, not a music table.
- Music graph: 295 events -> 695 actions (187 Play) -> 86 distinct targets
  (51 segments / 60 playlists / 76 switch containers) -> 192 tracks -> 150 media
  (125 present in `stream\`). 18 game-sync groups drive the switch containers;
  only `bgm_sync_ssbgm1` is named in the exe.
- **Gap for Track B:** `core.bnk` exists inside `data000.cpk` (found in the CPK
  TOC string table). It must own the other ~2,286 events - the `SeFootstep.bin`
  media IDs and the 25 bank-only music media are not on disk under `sound\`.
- M4 recommendation: keep the engine's own Wwise runtime and implement the
  **XAudio2 COM object**; declare 2.0/48 kHz on the mastering voice and let the
  engine downmix. Re-implementing Wwise is now specified but is a weeks-scale
  detour (`docs/audio.md` section 9.2).
- Not verified: the `.wai` flag-bit meanings, the exact `CLSID_XAudio2` + interface
  set (log the `CoCreateInstance` at M2.9), and whether `data001.cpk` carries
  further banks.

## 2026-11-19 - Track A verification pass (second session)

Independent re-derivation of `docs/audio.md` before M4 depends on it: own HIRC
resolver over a `wwiser -d xml` dump, own RIFF/DIDX/`.wai` parsers, vgmstream
r2168 as the only decode oracle. Everything the doc claims reproduced (779 wems /
codec split / 1,456 + 80 HIRC objects / 150 media / 95 byte-identical bank
prefixes / FNV ids / 97 named music events). Added as `docs/audio.md` section 12;
three pointers inserted into sections 4, 9.1 and 11.

New facts (section 12.2):

- **XAudio2 version is 2.7, located by GUID in `LoK.exe`**: `IID_IXAudio2`
  `8bcf1f58-9fe7-4583-8ac6-e2adc465c8bb` @0x507C50, `CLSID_XAudio2`
  `5a508685-a254-4fba-9b82-9a24b00306af` @0x507C60, `CLSID_XAudio2_Debug`
  `db05ea35-0329-4d4b-a53a-6dead03d3852` @0x507C70; the 2.8 IID is absent. The
  2.7 vtable carries `GetDeviceCount` / `GetDeviceDetails` / `Initialize` *before*
  `RegisterForCallbacks`, so a shim laid out from a 2.8 header is shifted three
  slots - the concrete trap for M2.9/M4. `CoCreateInstance(CLSID_XAudio2 2.7)` is
  only registered where the DirectX June-2010 runtime exists, so both Windows and
  Android need the host hook rather than real XAudio2.
- `Init.bnk` STMG budget: `fVolumeThreshold` -80 dB, `maxNumVoicesLimitInternal`
  **256**, 11 state groups, 9 explicit transitions + 1000/3000 ms defaults.
- **Correction:** the music switch containers use 14 distinct game-sync groups
  (18 argument instances), none named; `bgm_sync_ssbgm1` (= 1190818745) is one of
  the 11 global *state* groups, not a switch group.
- The only two non-Vorbis media are a matched 25.000 s placeholder pair
  (`1055586713` IMA-ADPCM -> track 260164643 / segment 2169841 / event 4198189333;
  `82801749` PCM16 -> track 933817339 / segment 38066807 / event 823604040), both
  isolated, both RMS -3.9 dBFS.
- `stream\English(US)\416126653.wem` is a **genuinely silent** asset (all samples
  0, 2.407 s): not a decode failure, and one of the legitimate "no sound" cases.
- 25 bank-only media ids listed in section 12.3 for Track B to check against the
  CPK manifest (`95719221` is the single complete in-memory source).

## 2026-11-19 — Track B: CPK archives + USM movies

Full detail in `docs/assets.md`; this is the summary.

- **Manifest (1,213 entries, no extraction):** `data000.cpk` 331 entries / 265.2 MB,
  `data001.cpk` 882 / 1.60 GB — 1.87 GB stored, **2.05 GB extracted**. Extensions:
  `.dat` 668, `.dtt` 523, `.evn` 12, `.bxm` 5, `.evt` 4, `.xml` 1. 57 zero-byte
  placeholders. 672 CRILAYLA-compressed entries (128 MB stored).
- **CRILAYLA:** all **672/672** decode to exactly their TOC `ExtractSize`, 0 errors
  and 0 size mismatches (in-memory pass over both archives). Decoder: backwards
  bitstream, 13-bit offset, tiered length `2 -> +3 -> +5 -> +8*255`, trailing
  0x100 verbatim prefix.
- **Container quirks:** UTF offsets are relative to `table + 8`; TOC `FileOffset`
  needs base `ContentOffset - 0x800` (gapless 0x800-aligned chain that ends exactly
  at `EtocOffset`, 0 gaps over 1,213 entries). `CpkMode = 1` is **not** encryption.
- **Payload magics:** `DAT\0` x1,148 (named sub-file bundle: header, 4CC array,
  name table, data), `BXM\0` x5, `vfxShadersetting.xml`, 2 `*ShaderSign.dat`.
- **USM:** `movie\CS0041.usm` demuxed to MPEG-1 1280x720 @ 23.976 (65 frames) +
  ADX 5.1 48 kHz, muxed with ffmpeg to MP4 and verified by ffprobe (65 frames,
  2.711 s, 6 ch). All 34 movies parse: 1280x720 MPEG-1, ADX 48 kHz, 800 MB, 697 s,
  no encryption, only `@SFV`/`@SFA` chunks. Chunk `frame_time` is **1/3000 s
  ticks, not ms** - take durations from the codec.
- **Answer to Track A's open item:** the missing Wwise banks are **inside the
  CPKs** - 62 DAT payloads contain a `BKHD` bank sub-file (45 in `data000`, 17 in
  `data001`), generator version `0x58` (= same Wwise build as `sound\*.bnk`), and
  `bank_id == FNV-1 32 of the lowercase asset stem` for 61/62 (`pl/pl00f1` aliases
  `pl0000`). IDs listed in `docs/assets.md` section 5.1.
- **Tooling:** `tools/cpk_manifest.py`, `tools/cpk_extract.py`, `tools/usm_demux.py`
  (stdlib only). **CriCodecs is unusable here**: PyPI ships no wheels and the
  source fails on MSVC 14.44 (`C1083 'flat_map'`, then `C3615/C7595` consteval trig
  in the HCA tables). Our readers were cross-checked against its source semantics.
- Not verified: DAT sub-file table internals beyond 4CC/name/length, per-entry CPK
  `CRC` algorithm, audio/video length drift in `CS0071A/B`/`CS00P2`/`CS00EP`.

## 2026-11-19 - Track D: steam_api + XAudio2 host shims

### Deliverables

- `src/host/steam_stub.c` / `.h` - the seven `steam_api.dll` imports plus real
  `ISteamUser` / `ISteamUserStats` objects (the previous skeleton returned a
  64-byte zero buffer, so the first `GetSteamID` would have called slot 0 of a
  zero vtable).
- `src/host/xaudio2_stub.c` / `.h` - silent `IXAudio2` 2.7 behind a
  `CoCreateInstance` shim, with a QPC playback clock so `GetState` drains.
- `docs/xaudio2_callsites.md` - the call-site map + ABI evidence (the M4 spec input).
- `tools/host_import_map.py` - regenerates the evidence (GUIDs, IAT slots,
  `FF 15` sites, indirect-call sweep, callers) without Ghidra.
- `tests/selftest_host_shims.c` + `scripts/run_selftest.ps1` - **54/54 pass**.
  Drives both vtables through real x86 thunks that set ECX/stack the way the
  guest does and asserts the exact purge, so an ABI error fails here instead of
  corrupting the lifted guest.

All `src/host/*.c` compile (`cl /c`). Full link is M2's gate; not re-run here.

### Findings

- **XAudio2 2.7, COM, two consumers.** `CoCreateInstance(CLSID_XAudio2
  {5a508685-a254-4fba-9b82-9a24b00306af} | CLSID_XAudio2Debug
  {db05ea35-0329-4d4b-a53a-6dead03d3852}, IID_IXAudio2
  {8bcf1f58-9fe7-4583-8ac6-e2adc465c8bb})` inside `FUN_007B22A8` (`0x7B22A8`).
  Called from CRI NcVoice init (`0x7B2E85`) **and** from the engine's own Hw
  audio device (`0x88B0DE`, `FUN_0088B0C0`). Each GUID appears once in `.rdata`.
- **ABI: `__stdcall` with `this` on the stack**, not `thiscall` - the code is
  `mov eax,[esi] / mov ecx,[eax] / push eax / call [ecx+0x64]`. `GetState` takes
  one argument in 2.7 (`&state` only). Both would desync the guest stack if
  declared the obvious way.
- **`XAUDIO2_DEVICE_DETAILS`**: `DeviceID[256]`, `DisplayName[256]`, `Role`,
  `OutputFormat`; the Hw mixer reads `OutputFormat.nChannels` at **+0x406** to
  pick a stereo (3) or 5.1 (0x3f) channel mask.
- **Callbacks are not required for silence.** CRI's `OnBufferEnd` (`0x7B2A24`)
  only increments a counter; feeding is driven by `GetState().BuffersQueued < 4`
  (`FUN_007B26B7`). The stub keeps the queue draining with a virtual clock and
  never re-enters guest code.
- **"Silent" is not only XAudio2.** DirectSound8 is created through
  `CoCreateInstance` @`0x88AAFC` (`FUN_0088AAE0`, Hw factory `FUN_008793F0`) and
  WASAPI `IMMDeviceEnumerator` @`0x870246` (`FUN_008701F0`). Both still go to the
  real `ole32`, so a Windows build can be audible. M2 must pick one.
- **steam_api bug fixed:** `SteamAPI_RegisterCallback`/`UnregisterCallback` are
  `__cdecl` (guest does `add esp,8`), so the old shim's `g_esp += 12` was 8 bytes
  too much. The shims now pop only the return address.
- **Only `ISteamUser::GetSteamID` is ever called** (slot 2), `__thiscall` with a
  hidden return pointer; `SteamUserStats()` is only null-checked.

### Not verified

- That M2.8/M2.9 work in a real boot (needs the M2 session's run; the
  `[xa2]`/`[steam]` log lines are the check).
- Whether the Hw mixer needs `OnBufferEnd` for its 8-slot ring (it polls
  `GetState` at `0x88B454`; if it stalls, the fix is a guest callback
  trampoline, not a different queue model).
- `GetPerformanceData` sites `0x88B795`/`0x88B8D1` by offset only.

## 2026-11-19 — Track B: confirmation pass (second run, 45/45 assertions)

Re-derived every Track B number from `GameData\` (throwaway script, temp-only; no
notes trusted). All headline claims hold:

- CPK sizes/entry counts/stored/extract totals byte-for-byte; extension **and**
  directory counts each sum to the entry count (331 / 882) in both archives.
- `FileOffset` base rule re-checked as a property, not a sample: both archives
  form a **gapless 0x800-aligned chain ending exactly at `EtocOffset`**.
- **All 182 + 490 = 672** CRILAYLA entries decoded again, every one matching its
  `ExtractSize` (345 s), with `DAT\0` x1,148 / `BXM\0` x5 / 57 zero-byte / 3
  outliers reproduced exactly.
- Movies: 34 files / 799,975,616 B / 17,265 `@SFV` + 21,058 `@SFA` chunks,
  17,095 pictures, 697.4 s, all 1280x720, fps set {23.976, 29.97, 30, 59.94},
  ADX only 48 kHz 2ch/6ch, **0 bad padding chunks**.
- In-CPK Wwise banks: 62 (45 + 17), generator `0x58` except `pl/plffff.dat`
  `0x48`, `bank_id == fnv1(stem)` for 61/62 (`pl/pl00f1` = `fnv1("pl0000")`).

Corrections the pass found (both now fixed in `docs/assets.md`, commit `4bd99ef`):

- "1.87 GB → 2.05 GB, ~9 %" was wrong arithmetic: it is **~10 %** (2,053,247,987
  vs 1,865,341,798 = +10.1 %).
- §6.2 gave 697 s but no frame total; the measured total is **17,095 pictures**
  (697.4 s) — the per-file table already summed to it.

Also tightened: §2 now states the base rule as an explicit formula
(`base = ContentOffset − 0x800`) instead of only showing the derived value.

## 2026-11-19 — Track E: publishing (Codeberg mirror, README, notices)

Commit `6973a3d`. New: `scripts/sync_codeberg.ps1`, `tests/sync_codeberg_selftest.ps1`,
`docs/PUBLISHING.md`; refreshed `README.md`, `THIRD_PARTY_NOTICES.md`,
`AGENTS.md` §3. `check_content` clean on all 44 tracked files.

### The mirror script

One command, five refusals, no force path: dirty tree (1), a tracked path the
content guard rejects (1), a host that is not `codeberg.org` (3), a
non-fast-forward (4), a failed push or a post-push SHA mismatch (5). `-DryRun`
still fetches, so the history decision is real. Config through
`KORRA_CODEBERG_REMOTE` / `KORRA_CODEBERG_BRANCH`; every run appends to the
gitignored `logs/sync_codeberg.log`.

`tests/sync_codeberg_selftest.ps1` builds a sandbox repo in `%TEMP%`, puts a bare
repo where Codeberg would be, and drives nine cases: no remote, host check, dry
run, tracked `.cpk`, dirty tree, first push, idempotent re-run, fast-forward,
non-fast-forward. **9/9 pass** (two runs, before and after the commit).

### Two bugs the selftest caught

- A helper named `Git` **shadowed `git.exe`** — PowerShell resolves a function
  before an application, so `& git` called the function again until "call depth
  overflow". Renamed `Invoke-Git`.
- `@(Invoke-Git ...).Out` binds the hashtable rather than its `Out` member under
  `Set-StrictMode -Version 2`; empty results arrived as `$null` and `.Count`
  threw. Pipelines are wrapped now, not members. Both were invisible when the
  script ran on an already-clean repo with a real remote.

### Notices re-checked against upstream (2026-11-19)

Corrections to what earlier notes assumed:

- `libadrenotools` is **BSD-2-Clause**, not BSD-3.
- **CriCodecs has no license file** — no `LICENSE`/`COPYING`, no `license` field
  in `pyproject.toml`. It stays "studied only" (and never built here).
- **Steamless is CC BY-NC-ND 4.0** — now listed as excluded, never adapt it. Moot
  in practice: the binary has no DRM.
- `wwiser` has no license file either (dev oracle only).
- `JJL772/d3dx9` is ToGL/MIT-style but **unused**: native32 links the real
  `d3dx9_43`/`d3d9` (M2.2).
- `usmbreak` is 0BSD and unused; `ngbrecomp` + `RogueSquadron64Recomp` GPL-3.0;
  `totk-pc-recomp` MIT; `awesome-decomp-games` CC0; pcrecomp MIT; vgmstream ISC;
  capstone BSD-3; pefile MIT; Ghidra Apache-2.0; ffmpeg LGPL-2.1+.

No pcrecomp runtime header is vendored here, so its MIT notice covers generated
code and the build only; the nfsu2 patch series is applied from a sibling clone
and never redistributed.

### README

Now states M2 as **in progress** with the true rung state (lifted C compiles in
517 s, links against real system DLLs, engine startup reached, **no first frame**),
adds a tracks table A–E, and fixes two stale claims: this binary's relocation
directory needs no repair, and the build section now shows
`scripts\build_host.ps1` instead of stopping at the lift.

### Not verified / not done

- **No mirror is live.** The checkout has no remotes and `ssh -T
  git@codeberg.org` answers `Permission denied (publickey)`; the key is not
  registered. The script's no-remote path prints the three setup steps. Everything
  up to `git push` is exercised by the selftest against a local bare repo; the
  network edge (Codeberg's SSH server, repository permissions) is untested.
- No CI. The guard and the selftest are local commands.

## 2026-11-20 - M2: the XAudio2 boot freeze, a lifter gap, and the title screen

### The freeze: a completion that was never dispatched

Track D concluded that CRI's `OnBufferEnd` (guest `0x7B2A24`) was not needed for
M2 because the feed loop is `GetState`-driven (`FUN_007B26B7`: submit while
`BuffersQueued < 4`). The first half is true. The second half is not:

- With the silent stub, the boot froze immediately after CRI created its first
  source voice (`[xa2] CreateSourceVoice ... -> #2`). `KORRA_NO_AUDIO=1` bypassed
  it, which is what made it look like "the stub blocks the boot".
- `KORRA_XA2_VERBOSE=2` showed the shape of the wait: the device pump called
  `CommitChanges(0x7FFFFFFF)` about 50 times a second forever, and `GetState` was
  never called again. Retirement only ran inside `GetState`, so 68 submitted
  buffers never retired, and one guest worker thread spun on a lock
  (`Enter/LeaveCriticalSection` around `0x00A9BD18`) while the engine waited.
- CRI submits every buffer with the *same* `pBufferContext` (a u32 counter at
  `0x11C9130C` in that run), and the guest's `OnBufferEnd` body is exactly
  `inc dword ptr [pBufferContext]` (`0x7B2A22..0x7B2A2A`). Nothing else in the
  system moves that counter.

Fix (`src/host/xaudio2_stub.c`): retirement is now dispatched from **every**
device entry point, `CommitChanges` included, and every retired buffer performs
the callback's observed effect - `*(u32 *)pBufferContext += 1` - when the voice's
callback vtable has the CRI `OnBufferEnd` at slot 4 (`0x7B2A24`). Other callbacks
are counted and logged once per voice instead of being guessed at. The lifted
callback body is deliberately *not* entered: `native32_call_guest` from inside a
COM method returns with `esp` advanced by its own arguments and the caller's
callee-saved registers replaced by the callee's, while the lifted caller is mid
`call [ecx+0x54]`. M4 can hang a real audio thread here, which is where re-entry
belongs.

Verification:

- `tests/selftest_host_shims.c` extended: the fake callback object now carries
  CRI's real vtable shape (slot 4 = `0x7B2A24`), and two checks assert the
  counter increments - once via `GetState`, once via `CommitChanges` with no
  `GetState` at all (the freeze case). **60/60 pass**, and the two new calls are
  also purge-checked.
- Full boot with `KORRA_NO_AUDIO` unset: the freeze is gone. The engine now
  creates and tears down one audio device per movie, the intro movies render, and
  the title/loading screen appears. New knobs: `KORRA_XA2_VERBOSE=0..3`.

### A lifter gap that the movies concealed

Once audio worked, the guest returned further and hit an unimplemented
instruction: `[unimpl] 0x00660D25: lock xadd dword ptr [eax], esi` (a shared
refcount release), which aborts by design. `xadd` was the only missing
interlocked form left (`cmpxchg`/`cmpxchg8b` were already implemented).

- `pcrecomp/tools/lift/lift32.py` gained `xadd` (temp = dst; dst += src;
  src = temp; flags as ADD; exact single-threaded under the runtime's machine
  lock). Three sites in the image: `0x5209E5`, `0x5A5605`, `0x660D25`.
- Relift: 37,029/37,029, **0 failures**, 220 s, 12,019,200 lines. Full rebuild
  467 s (85 host+runtime+generated TUs, `/MP12`). `RECOMP_UNIMPL` sites 114 -> 111.
  What remains is string I/O (`insb`/`insd`), SSE4 (`palignr`, `addsubps`,
  `movshdup`/`movsldup`, `pinsrb`) and legacy junk (`aas`, `das`, `arpl`,
  `bound`, `cli`, `hlt`, `int`, `ljmp`) - none reached yet.

### Build tooling added while chasing this

- `scripts/build_host.ps1 -Trace` builds a separate `build\obj-trace` with
  `-DRECOMP_TRACE` (the runtime's `--calltrace`, `--firsthit`, `--argtrace`,
  `--watch`, `--poison`), so a diagnostic build and the normal one can coexist.
  The two object trees cannot be shared: `RECOMP_ENTER` is a macro in every
  generated translation unit.
- `scripts/run_host.ps1` gained `-Extra` (runtime arguments) and `-Exe`.
- `src/host/quirk_probe.c` (new, `KORRA_QUIRK_PROBE=1`): samples the guest's own
  frame-manager data once a second - `DAT_00e4d210`, `DAT_00e4d214`,
  `[frameMgr+0x58]`, `DAT_00e4d01c`, the `0xe4e52c` HWND, window focus, and the
  measured frames/s + frame-interval min/mean/max from `[frameMgr+0x5c]`.

## 2026-11-20 (continued) - input, the title screen, and the new-game crash

### The title screen is alive; the menus are not

With the XAudio2 fix in place the intro movies play and the title screen appears.
It looked static and black earlier only because nothing could press anything: the
engine polls four XInput pads and no controller exists on this machine, so the
"press start" prompt waited forever while the render loop ran at a measured
59 fps.

Two additions made it observable:

- `src/host/xinput_stub.c` answers pad 0 as connected (`KORRA_PAD=1`, plus
  `KORRA_PAD_BITS` / `KORRA_PAD_AUTO`). The imports are by ordinal, so
  `native32_bind` learned to match a shim named `<dll>!#<ordinal>`.
- `scripts/probe_input.ps1` takes the foreground properly (AttachThreadInput to
  the *current* foreground thread) and screenshots each step. Injected keyboard
  and mouse input does **not** reach the engine even with the window foreground
  and focused; the pad does. That is a finding for M2.11, not a bug to chase:
  the DInput keyboard path is not wired yet.

With the pad the title screen animates: black + logo, then the parchment
background with an "A" button glyph (`build/shots/44s.png`, `52s.png`). Menu
*text* is still missing - the art, the button glyph and the selection brush
stroke draw, the glyphs do not. Not yet diagnosed; D3D9/D3DX report no failures
(every `D3DXCreateTexture*` returns S_OK, all 506 `D3DXGetShaderConstantTable`
calls parse), so it is not a rejected resource.

### The new-game transition crashes, deterministically

An A press leaves the title screen, the screen fades to black, and the process
dies with an access violation on every run. Two things were needed to see it:

- The guest's static CRT calls `SetUnhandledExceptionFilter` during startup and
  replaced the host's, so the [fatal] report never ran. That import is shimmed
  now; the first crash was located through the Windows event log
  (`0xC0000005` at offset `0x02aede67`, i.e. VA `0x62AEDE67` with `/BASE` fixed),
  which the map file resolves to lifted `sub_00603C00`.
- `run_until_exit.ps1` reports how a run ended.

The report now reads:

```
[fatal] exception 0xC0000005 at 0x62B070E2, guest func 0x00603C00
[fatal] read of 0x00000000
```

`--watch 0x00603C00` shows a single entry, argument 0, then the fault: the
function's case 0 does

```c
p = MEM32(0xCA3E58);
if (p) eax = MEM32(p + 0x2CD4); else eax = MEM32(0);   /* 0x603C23 */
call [eax + 0x1C]
```

so the engine dereferences a manager it expects to exist. `0xCA3E58` is only
ever written by the message handler `sub_0058DAB0` at `0x58DC33`, from a list
search that stores **0** when no object with type marker `0x9DEE90` is linked,
and cleared by `sub_0058E730` at `0x58E79C`. `--poison 0xCA3E58` reports no
change during the whole menu-to-new-game run: the pointer is null from the
moment the title screen exists, and the null check in the guest is only there
because the original code trusts its own state (the null path dereferences 0 in
the shipped binary too).

So the question for the next session is narrow and state-shaped: which object is
`0xCA3E58` supposed to point at, and why is it not in the list when the new-game
flow starts? The candidates that have *not* been ruled out are the ones this port
still fakes: the pad (now synthetic), the sound device (silent), and the Steam
interfaces (stubs). Nothing in the file, D3D9 or lift layers is implicated.

### Measurements taken on this build (M2.13, quirks section 8)

`KORRA_QUIRK_PROBE=1` samples the engine's own frame data once a second:

```
[probe] focus=1 mode=0(60fps) applied=0 interval=50(60fps) frames=58.8/s
        mean=17.0ms min=13.6ms max=21.0ms flags=0x00000000 hwnd=0x0003...
```

- **`DAT_00e4d210` is 0 while the window is focused**, i.e. 60 fps, and
  `[frameMgr+0x58]` is 50 (1/3 ms units = 60 Hz). The focus branch does not flip
  a focused window to 30 fps. Quirks section 8 item 1 is answered.
- `DAT_00e4d01c` is 0 (limiter enabled); it gained bit 1 only while unfocused.
- The `0xe4e52c` HWND global is populated by the engine itself.
- Item 3 (does 60 fps double the logic rate) is still open: it needs the game to
  run a scene, not a title screen.

## 2026-11-20 (late) - the original binary as an oracle, and what the box really is

### `LoK.exe` runs on this machine

The shipped binary is DRM-free and `steam_api.dll` is in the game folder, so the
original can be started next to the port. It does not get far - it stops on a
parchment message box that reads **"Wrong SteamID. Failed to load data."** - but
that single frame answers two questions the port could not answer alone:

- the parchment rectangle with the corner ornaments that the port draws **is the
  message box**, and in the original it contains **text**;
- so the port's "invisible text" is exactly one defect: the box's panel and its
  ornaments render, the glyphs inside it do not. Font glyphs are drawn, clipped
  or positioned by per-glyph maths, and that is what fails; every fixed-shape
  element (panel, ornaments, logo, brush stroke, button glyphs) renders.

Running the original is now the cheapest oracle for any later "is this the game
or the port?" question. Note it needs a real Steam login to go past that box, so
it is an oracle for the *pre-menu* state only.

### The new-game crash is a state divergence, not a crash class

Everything up to the HUD is now accounted for:

- the crash is `UIPlHpBendingIcon` (vtable `0x96270C`) reading `0xCA3E58` and
  dereferencing it as `BehaviorPlayer*` - the missing object is a `BehaviorPlayer`
  (`0x9DEE90` is the value slot 1 of vtable `0x94B284` returns);
- `sub_00503BD0` -> `sub_00507940` (the BehaviorPlayer constructor) **does run**
  (`--firsthit` logs it before the crash), so the player is spawned;
- `0xCA3E58` has exactly two writers, both inside the loader: `0x58DC33` sets it
  from a list search, `0x58E79C` clears it. `--poison 0xCA3E58` shows it never
  changes from zero;
- the setter is the `0x710` branch of the loader dispatcher `sub_0058DAB0`, whose
  only caller is the app state machine `sub_0053BE60` (state word `0xCA3838`).
  `--poison 0xCA3838` shows the machine cycling 0-1-2-3-5-6-7-8-9 normally, and
  `--poison 0xE38AB0` shows only two step ids ever dispatched: `0x0F00` and
  `0x0A10`. **`0x710` is never dispatched.**

So the loader never reaches the step that publishes the player, while the HUD is
already updating. Creating the missing save directory
(`<Documents>\Korra`, the path the engine opens `SystemData.dat` under) does
not change it, and `KORRA_NO_AUDIO=1` does not either.

### What the next session should do

This needs an RE pass, not more runs:

1. Read `sub_0053BE60` state by state (jump table `0x53C160`) and find which
   condition keeps the machine from the state that queues `0x710`. Its guards are
   `0xCA3CE0 & 0x10000000`, `0xCA3A94/0xCA3A90/0xCA3A8C/0xCA3A88`,
   `0xCA3AC0/0xCA3ABC/0xCA3AB8` - all inside the app struct Track C mapped.
2. Find who queues step ids into `0xE38AB0` (`0x5C1BE2`, `0x5C2CF4`, `0x5C2D74`,
   `0x5C2DBA`, `0x5C2F70`, `0x53FD98`) and why `0x710` is never among them.
3. For the text: find the glyph-draw function (it consumes the same message-box
   strings) and dump the quad it builds; a sane quad points at the texture, a
   degenerate one at the lifted per-glyph maths.

## 2026-11-20 (night) - the new-game crash, mechanically

The earlier entry reduced the crash to "step 0x710 is never dispatched". That was
the right observation about the wrong mechanism. Reading the dispatcher and the
state machine end to end (lifted C plus one instrumented run) gives the actual
chain, and it is shorter than it looked.

### What the step dispatcher actually is

`sub_0053BE60` is the **cutscene/movie sequencer** (its only caller is
`sub_00541870`, the USM player state machine, and its state 8 compares the
current step name against `"Cutscene_01"`, `"Cutscene_02"`,
`"Tree of Time - Part 2"` and fires the `se_stage_start` sound). Once per cycle
its state 3 does:

    sub_005C1AC0()                  ; rotate the step registers
    sub_0058DAB0(0xE38AB0)          ; apply the step to the scene

so `0xE38AB0` is the **pending step id**, `0xE38A84` the current one, `0xE38A8C`
its name string, and `0xCA3838` the sequencer's own 0..9 state.

`sub_0058DAB0(step)` then:

- `0x710` only selects a *priority* by comparing the step name with `"Port"`,
  `"Start"`, `"PBL"` (10 / 20 / 0) and stores it in a local that nothing reads
  again. **It is not the branch that fills the player pointer.**
- `0xF00..0xF02` **return immediately** - an explicit no-op family.
- **Every other id** falls through to the common tail, which is the part that
  matters:

      sub_005201D0("Player", 0x10000 + mode, &s)  ; scene object for the Player
      if (that == 0) return                       ; nothing published
      iface = obj[0x48]                           ; the Player's behaviour slot
      walk iface's type chain for 0x9DEE90        ; BehaviorPlayer's type token
      0xCA3E58 = match or 0                       ; <- the global HUD reads
      0xE381F8 &= ~0xC0                           ; clear the "hide HUD" bits
      sub_0058EE10()                              ; build the stage UI

`sub_0058EE10` opens with `if (0xCA3E58 == 0) return;`, so a *missing player*
does not build the stage UI. The HUD that crashes is therefore built elsewhere,
and `sub_0058DC70(step)` is how it becomes visible: for each step family it ORs
bits into `0xE381F8` - `0x110`/`0xA10` set `0x80` (hide, cutscene), `0x210`/`0x310`
set low bits, `0x410` `0x3`, `0x510` `0x7`, `0x610` `0xF`, **`0x710` `0x1F`**
(the full HP / bending icon set), `0x810` `0x3F`.

### Why the crash is exactly a null player

`sub_00603C00` (the `UIPlHpBendingIcon` update, mode 0) is:

    0x00603C23  eax = [0xCA3E58]
    0x00603C2A  if (eax == 0) goto 0x00603C3C
    0x00603C2C      ecx = eax + 0x2CD4 ; call [[ecx]+0x1C]
    0x00603C3C  ecx = 0                 <- the "null" branch
    0x00603C3E      eax = [ecx]         <- read of 0x00000000: the reported fault
    0x00603C40      call [[eax]+0x1C]

The shipped binary has the same null dereference, so this is a *state invariant*
the game trusts, not a missing null check. The port reaches the HUD update with
`0xCA3E58 == 0`.

### What one instrumented run showed (`KORRA_TRACE_CRASH=1`)

Menu phase, mode `0xCA3AC0 = 2`, current step `0xF00`, name `"START"`:

    publish  step=0x00000F00   -> returns immediately (0xF00 is a no-op id)
    ui-flags step=0x00000F00   -> falls through the tag chain
    0xE381F8: 0 -> 0x40

i.e. in the menu the scene is never re-published and `0xCA3E58` is never
written. The earlier poison runs also saw `0xA10` dispatched once - that is the
new-game cutscene step, and it takes the *common tail* above: it clears the
"hide HUD" bits, publishes whatever `obj[0x48]` is, and that is null. The HUD
becomes visible and the next `sub_00603C00` reads `0xCA3E58 == 0`.

So the defect is one specific datum, not a dispatch gate:

> when the new-game cutscene step is applied, the Player scene object's `+0x48`
> is either null or is not a `BehaviorPlayer` (its type chain does not contain
> `0x9DEE90`), so the publish stores 0 and the HUD still un-hides.

That is the next instrument: the crash probe already prints the publish with its
step id; the missing piece is `obj[0x48]` and the first two links of its type
chain, which is a `--chase`-shaped read at `0x0058DAB0`.

### Blocker hit while verifying: both binaries now quit during startup

From ~03:24 every run - the port **and the original `LoK.exe`** - ends with
`ExitProcess(0)` about 0.4 s in, before the audio device exists, and the guest
CRT then runs its destructor table. `[exit] ExitProcess` now dumps the entry ring
and the guest stack, which is how the shutdown table was identified.

Facts collected before stopping:

- it is **not** our code: the unmodified original fails identically, and the port
  failed the same way with and without the new probe;
- it started intermittently (~03:16, roughly 1 boot in 6) and became consistent
  at ~03:24; two clean boots at 03:20 and 03:23:48 bracket the change;
- not the save directory (removed `<Documents>\Korra`, no change), not the
  shim count (174 native / 20 shimmed with `KORRA_PAD`, 177/17 without - that
  difference is just the synthetic-pad shims), not windowed-vs-fullscreen;
- the desktop moved from 1920x1080 to **1920x1200 on the Intel UHD iGPU**; the
  machine is a hybrid laptop (RTX 5050 + Intel) and both engines pick their D3D9
  adapter from the desktop's, so a display/GPU-mode change is the leading
  suspect and is outside this repo;
- audio endpoints still enumerate (Realtek/NVIDIA/Steam streaming/SteelSeries
  virtual devices all `OK`).

Dead ends: rebuilding the trace target, `KORRA_PAD` on/off, windowed/fullscreen,
deleting the save directory, waiting 12 s between runs. None changed it.

### Harness that came out of this (kept)

- `src/host/quirk_probe.c`: `KORRA_TRACE_CRASH=1` - a timeline of the dwords that
  decide this crash (`0xCA3E58`, `0xE381F8`, `0xCA3AC0`, `0xE562E4`, `0xE38A84`,
  `0xE38AB0`), sampled at every function entry so an A->B->A round trip inside one
  frame is visible (which `--poison` cannot see), plus named sites (publish,
  stage-UI, release, ui-flags, step queue, HUD ctor/update, player spawn) that
  print ECX, the step name and the surrounding globals.
- `src/host/exit_probe.c`: `ExitProcess` now dumps the entry ring and the guest
  stack. A clean `exit(0)` used to leave no evidence at all.
- `scripts/run_until_crash.ps1`: boots, focuses, sends the one Escape that skips
  the intro movies, and **retries boots that abort in the first second** - so an
  unattended run does not spend its budget on a dead window.
- `scripts/probe_input.ps1`: `-Exe` / `-Extra`, so the trace build can be driven
  by the same harness.

### Resolution of the instrumented run: the lookup returns 0, the object appears one call later

Two more runs with the probe extended to the lookup itself settle it. The
new-game sequence, in order, with `KORRA_PAD=1 KORRA_PAD_AUTO=1`:

    publish  step=0x00000A10  mode=0  name="START"     <- cutscene step applied
    scene-lookup 0x005201D0   mode=0                   <- sub_005201D0("Player", 0x10000)
    player-spawn 0x00503BD0                            <- so the player is created NOW
    (0xE381F8 stays 0 -> then 0x80 via the ui-flags pass)
    hud-ctor  0x00602D80
    hud-update 0x00603C00   player=0x00000000          <- crash

and the two markers that only run on the success path **never appear**:

- `0040F6F0` (the publish's tail, reached only when the scene object was non-null)
- `0058EE10` (stage UI, reached only when the player was non-null)

So the publish did **not** "fail to find the player inside a stage object". It
never got a stage object at all: `sub_005201D0("Player", 0x10000 + mode, …)`
returned 0, and `0x0058DBC4` sent it straight to the epilogue. Then
`0x00503BD0` -> `0x00507940` created the `BehaviorPlayer` in the *next* call, and
nothing re-published it: the pending step id had already been consumed
(`0xE38AB0` is set to -1 by `sub_005C1AC0` before the dispatch), so no later
sequencer cycle runs the publish.

That makes the defect an **ordering one**, and it is one frame wide:

> the Player scene object is created one call *after* the publish that looks for
> it. Because the publish is the only writer of `0xCA3E58`, the global stays 0
> for the rest of the cutscene and the HUD - which is constructed right after -
> dereferences it.

Next step, and it is narrow: find who is supposed to create the "Player" scene
object *before* the lookup (it is not `0x00503BD0`; that one runs later by
design), and why it never ran on the way into `Cutscene_01`. The tooling is in
place: `KORRA_TRACE_CRASH=1` prints `scene-lookup`, `publish`, `publish-tail`,
`stage-ui` and the step name, so a candidate fix is confirmed by `publish-tail`
appearing with a non-zero player.

## 2026-11-20 - text defect: the glyph chain breaks before the draw call

The port draws every fixed-shape UI element and no glyphs. The question was
where the chain breaks: font asset -> glyph rasterisation -> texture creation ->
draw call. Runtime evidence now answers it: **no glyph-shaped draw is ever
submitted, and no 2D font atlas texture is ever created**. The draw call, the
alpha test, the texture format and texture locking are all ruled out.

Tool: `src/host/draw_probe.c` (new, host-only, inert unless `KORRA_DRAWPATH` is
set). It rides `native32_post_hook`, which the runtime calls after every bridged
native call - imports and COM methods alike - so every IDirect3DDevice9 call the
lifted engine makes is observed with its argument slots and result.

| Knob | Output |
|---|---|
| `KORRA_DRAWPATH=1` | per-frame draws/state, one line per distinct draw shape |
| `KORRA_DRAWPATH_FROM=<frame>` | per-frame texture histogram, printed only when it changes |
| `KORRA_DRAWPATH_FRAME=<frame>` | every draw of one frame |
| `KORRA_DRAWPATH_TEX=1` | first-draw pixel survey of each texture (see the warning below) |

### Four things that had to be right before it could see anything

Each of these produced a *plausible* but wrong answer, so they are recorded here
rather than left in the tool:

1. `d3d9.dll`'s `SizeOfImage` was read from `lfanew[0x18]` (SizeOfStackReserve)
   instead of `lfanew[0x14]`. The range came out as 0x40000 instead of
   0x170000, so every device method address fell outside it.
2. The device this d3d9 returns has a **heap-allocated thunk table** (vtable
   `0x3880A8FC`, entries in `d3d9.dll`). "The vtable is inside d3d9.dll" is
   false, and rejecting it hides the device completely.
3. `IID_IDirect3DDevice9` was hand-typed and wrong in its last four bytes
   (`A43B0D92B9E4` instead of the SDK's `A43B0D82B9EB`), so every
   `QueryInterface` answered `E_NOINTERFACE` and the device never matched. The
   probe now takes the GUID from `d3d9.h` (copied; `DEFINE_GUID` only declares).
4. **COM methods have no name in the runtime's table** - only bridged *imports*
   are named. `quirk_probe.c`'s `KORRA_WINDOWED` rewrite looks for
   `"d3d9.dll!CreateDevice"` by name, so it never fires; the window stays
   fullscreen and a second game instance cannot create a device at all, which
   makes two parallel debug sessions impossible unless the first one is windowed
   by other means. The probe identifies the call structurally instead
   (BehaviorFlags in {0x44,0x24}, DeviceType in {HAL,REF}, a readable
   `D3DPRESENT_PARAMETERS`, interface written to `args[6]`).

### What the census shows at the title screen (frame 6000, 60 fps)

```
[draw] DEVICE 0x365C0BC0 vtbl 0x365C3B1C (Present=69F802A0 EndScene=69F06670
        SetRenderState=69FC4FC0 SetTexture=69FBF450 DrawPrimitiveUP=69FBA750 ...)
[draw] frame 6000 draws=22 up=0 vb=22 prims=52 texs=8 rt=3E982B20/30
        begins=1 ends=1 fvf=0x0 vs=0x56F5D7C0 ps=0x00000000
        abl=1 src=5 dst=6 zen=0 atest=1 afunc=1
```

- 22 draws per frame, **all single quads** (`type=5`, `prims=2`), all from two
  leaves: `sub_00757200` (`DrawPrimitive`) and `sub_00757240`
  (`DrawIndexedPrimitive`). Those two are the engine's textured-quad leaves -
  they draw the movies, the logo, the ornaments and the brush stroke alike, so
  the caller does not separate text from art.
- Distinct textures drawn per frame: three 1920x1080 UI layers, `320x176` x6,
  `80x48` x3, `48x32` x5, plus DDS art loaded through
  `D3DXCreateTextureFromFileInMemoryEx`. During the transitions a `256x1`
  gradient strip appears.
- **No small square atlas is ever drawn**, and no draw uses a texture that was
  created blank. A glyph batch (many quads sharing one atlas, or one quad per
  glyph sharing one atlas) is absent from every frame of a 175 s run.
- Every 256x256/1024x1024/128x128 texture the engine creates is paired with
  `CreateRenderTarget` and is `0x72` (A16B16G16R16) or a shadow/effect target -
  not a font page.

So the text path stops at one of the first two steps: the font asset read (the
engine's `cStringDevice`/`createCharSetManager()` strings and the `fontshader`
shader name are in the binary, and `core/coreui.dat`+`.dtt` are the UI tables),
or the per-glyph layout that would emit those quads.

### Not verified

- Whether the *string* is empty. An empty or unformatted string produces zero
  glyphs and exactly this picture, and it is the cheapest remaining hypothesis:
  every UI label in the game would be affected, while panels, ornaments and art
  (which are not string-driven) would not. The message box the original shows
  ("Wrong SteamID. Failed to load data.") is a formatted string, so a failed
  string lookup in the port would leave the panel and drop the text.
- Which function builds the text run. That needs the `cStringDevice` /
  `CharSetManager` call graph, i.e. the REA/Ghidra lock this session does not
  hold.
- The pixel survey (`KORRA_DRAWPATH_TEX=1`) was written and is *not* in the
  reported runs: locking a texture from inside the bridge hook killed one run
  (process gone, no fault report, at the first draw). Do not enable it together
  with a long session until that is understood.

### Knob note for the next session

`sig#N` lines dedupe on (kind, prims, texture, FVF, caller) and print the first
3 hits; use `KORRA_DRAWPATH_FROM` for the per-frame histogram, which is what
shows a texture *appearing* (the change that a text layer would make).

### One disproved hypothesis (recorded so it is not retried)

A Turkish Windows reports ANSI codepage 1254, and the engine imports
`GetACP`/`GetOEMCP`/`IsValidCodePage`. Forcing `GetACP` to 1252 for a whole
session (`KORRA_FORCE_ACP=1252`, a host-side rewrite of the guest's return
value) changed nothing on screen, and the log shows **`GetACP` is called exactly
once**, during CRT startup - the text path never asks for the codepage. Not the
cause.
## 2026-11-20 (later) - the crash is a missing asset, and the chain is closed

Following the ordering gap back to the lookup found the actual cause. The
publish is not fighting the sequencer; the Player object is **rejected**.

The whole chain, each step verified by a probe line in one run:

    005201D0  lookup-body  name="Player"  id=0x00010000
    0073E700  factory      -> allocated
    0051F460  obj-init      obj=0x0E220D80
    0051F4B0  obj-fill      obj=0x0E220D80
      00503BD0  player-spawn            <- the BehaviourPlayer is constructed here
      00492D90  activate     obj=0x0E461B60   <- BehaviorPlayer vtable slot 8
        004FB4C0  gate1   -> ok
        004E85A0  gate2   -> 0
    0051F8B0  obj-reject    obj=0x0E220D80   <- destroyed, +0x48 stays 0

`sub_0051F4B0` sets the field the publish searches (`[edi+0x48]`) from
`sub_004F9CE0`, and `sub_004F9CE0` throws the freshly created player away when
`vtable[0x20]()` returns 0. That slot is `0x00492D90` (BehaviorPlayer vtable
`0x94B284` slot 8), whose second gate is:

    sub_004E85A0(player):
        eax = sub_006AAB10(player, "pl\\pl00f0.dat")   ; find the loaded asset
        if (!eax) return 0                              <- this is what returns 0
        ...

`0x94AC4C` is the literal **`pl\pl00f0.dat`**, the player's asset bundle, and
`sub_006AAB10` is "find the loaded asset by path" (`sub_006AB310` builds the key,
`sub_006AB250` does the lookup). Gate 2 fails because that bundle is not in the
resource registry when the player is activated.

**The file is not missing from the install.** `pl00f0.dat` (and `pl00f1..f3`) is
in `data000.cpk`, so this is a load-side failure, not a packaging one. Note also
that `sub_006AAB10` with this path is called from exactly two places, both inside
the player's own setup (`0x004E3460`, `0x004E85A0`) - nothing else ever asks for
it, so the load has to be registered by the generic scene/manifest path rather
than by a dedicated player-asset load.

So the defect chain is:

> the player's asset bundle is never registered -> `sub_004E85A0` returns 0 ->
> `sub_00492D90` reports failure -> `sub_004F9CE0` destroys the player ->
> `[edi+0x48]` stays 0 -> `sub_0051F4B0` rejects the scene object ->
> `sub_005201D0` returns 0 -> the publish stores nothing -> `0xCA3E58` stays 0 ->
> the HUD update dereferences null.

Next step, and it is a different subsystem: who is supposed to register
`pl\pl00f0.dat` (the scene/level asset install path), and why that registration
never happens in the port. `KORRA_TRACE_CRASH=1` now prints `asset-lookup` lines
for every `sub_006AAB10` call, so the same run will show which assets *are*
requested next to the one that is not.

### Probe additions

`scene-lookup`, `publish-tail`, `lookup-body`, `factory`, `obj-init`, `obj-fill`,
`cache-link`, `obj-reject`, `activate`, `gate1`, `gate2`, `asset-lookup` - each
with the object or path it is about, all off unless `KORRA_TRACE_CRASH` is set.
One gotcha worth keeping: the first stack argument is at `esp+4`, not `esp+8`
(the call has just pushed the return address); reading the wrong slot silently
filters everything out as "not a pointer".
### The resource registry is asked about one path in the whole run, and only to look it up

Printing every key the resource manager is built for (`0x006AB310`, deduplicated)
and every find (`0x006AAB10`) over a full run to the crash gives one line each:

    asset-lookup "pl\pl00f0.dat"
    asset-key    "pl\pl00f0.dat"

So `pl\pl00f0.dat` is **the only path the manager is ever asked about**, and it is
only ever a *find*. Nothing offers that manager an asset to install, which is why
the find returns 0 and the whole chain above collapses:

    no asset registered -> 0x4E85A0 (gate 2) returns 0 -> 0x492D90 reports failure
    -> 0x4F9CE0 destroys the player -> [edi+0x48] stays 0 -> 0x51F4B0 rejects the
    scene object -> 0x5201D0 returns 0 -> the publish stores nothing -> 0xCA3E58
    stays 0 -> the HUD update dereferences NULL

The file is in the container (`pl00f0.dat` sits in `data000.cpk` with
`pl00f1..f3`), so the remaining defect is entirely on the install side: whatever
registers a loaded asset under its logical path does not run for the player (and,
by this measurement, for anything else that would have reached this manager).

What this does **not** prove, stated plainly: the manager may not be the only
registry in the engine, so "nothing is installed anywhere" is not concluded - only
that `0x6AB310`/`0x6AAB10` see exactly one path. If the installer builds keys a
different way, the next instrument is its insert call rather than this pair.

Next: find the insert side of that manager and the caller that should have run
before the player is activated. The probe already prints the whole chain, so a
candidate fix is confirmed by `asset-key` appearing *before* `asset-lookup` for
the same path, then by `publish-tail` with a non-zero player.

## 2026-11-20 (crash session) - the asset registry is full; gate 2 fails resolving a sub-resource

The previous entry concluded that nothing ever offers the resource manager an
asset, so the player's bundle is never registered. A memory probe now shows the
opposite: the registry is complete at the moment the player is rejected, and the
failure is one step further in. The earlier conclusion came from a probe that
counted key builds and finds; it never read the pool itself.

Tool: `src/host/scene_probe.c` (new, host-only, inert unless `KORRA_TRACE_SCENE`
is set). It installs itself through a `.CRT$XIU` initializer, so it needs no
edit to `main.c` and no build-script entry. It reads the guest directly from
`native32_post_hook` and prints only on change:

```
[scene] flow=0x3 inst=0x1 list=0x00000001 gate=0x1 early=0xC7DE134
        player=0x00000000 step=0xFFFFFFFF mode=0 pool=56
[scene] pool 56: "core\core.dat" id=1 st=8 pl=0x1046D320 rc=1 typ=1 tmr=14914488 | ...
[scene] bundle "pl\pl00f0.dat" rec=0x0B8520DC payload=0x10FD00E0
        head= 00544144 00000242 00000020 00000928 ...
```

### What the pool actually holds when the crash happens

A run to the crash (`KORRA_PAD=1 KORRA_PAD_AUTO=1`) leaves **56-62 records**:

| key | id | state | payload |
|---|---|---|---|
| `core\core.dat`, `core\core.dtt`, `core\coreeff.*`, `core\coreui.*`, `core\coreph.dat` | 1-7 | 8 | non-NULL |
| `pl\pl00f0.dat` | 8 | **8** | `0x10FD00E0` |
| `it\it00*.dat/.dtt` | 9-17 | 8 | non-NULL |
| `ui\ui_core.dat`, `ui\ui_hud.dat`, `ui\ui_title.dat`, ... | 18-32 | 8 | non-NULL |
| `et\et*.dat/.dtt`, `et\eta*.dat` | 33-54 | 8 | non-NULL |
| `pl\pl0000.*`, `pl\pl0001.dat`, `pl\pl0100.*` | 55-59 | 8 | non-NULL |
| `pha\pa10.*`, `ui\ui_sub.dat`, ... | 60-67 | 8 | non-NULL |

State 8 is the **only** state the find accepts, not a marker of "not ready":
the byte table at `0x6AAB7C` maps every state 0-7,9-11 to the "return 0" case
`0x6AAB62` and state 8 to `0x6AAB4F`, which returns `record+0x30`. So
`sub_006AAB10("pl\pl00f0.dat")` cannot be the step that fails.

The payload of `pl\pl00f0.dat` is the **raw DAT container**, not a parsed
object: its header is `DAT\0`, `0x242` (578 sub-files), `0x20`, `0x928`, ...
which is byte-identical to the file extracted from `data000.cpk`
(3,543,040 B, CRILAYLA, entry `pl/pl00f0.dat`).

### So gate 2 fails on its second step

`sub_004E85A0(player)` has two exits to zero:

1. `sub_006AAB10(player, "pl\pl00f0.dat")` - measured to succeed (above);
2. `sub_00637BC0(&out, &payload)` with the literal at `0x941F7C`, which is the
   suffix `_battleParameter.bin` (`0x941F70` is the separate string `No`, so the
   two are adjacent, not one string).

The shipped bundle **contains** `pl0000_battleParameter.bin` - one occurrence,
at offset 20,246 of the extracted `pl00f0.dat`, in the container's name table. So
the sub-resource exists on disk and in the loaded container; the *resolve* is
what returns 0 in the port. `sub_00637BC0` builds a name with `sub_00746880`,
whose only callee is `0x77A1D0` (string helper), then indexes a packed handle
(`handle >> 28` selects a table, low 28 bits an entry) and returns 0 when the
table index, the entry count or the slot is out of range.

### The scene pipeline, for the record

`--firsthit` over `0x53B000-0x540200` plus `--poison` on the two state words:

- GameTask (`0x53F2B0`, registered as `"GameTask"` at `0x540D91` into the table
  at `0xDC0A08`) runs once and never leaves `0x53F300`'s pump loop
  (`0x53FBA0`/`0x53FCE0` are never entered), so the loop is still spinning at
  the crash.
- The scene flow `[0xE38A54]` cycles `0 -> 1 -> ... -> 0x11 -> 0` and starts a
  second cycle: a retry loop, not a stall.
- The installer state `[0xDBC02C]` only ever reaches 1. `0x53EB40` (build the
  8 slot requests), `0x53EBB0` (run them) and `0x53DCC0` (per-slot request, the
  insert side of the registry) are **never entered**. Flow `0x13`, the only
  writer of installer state 2, is never dispatched: flow 9 (`0x5C15F0`) takes
  its "nothing requested" branch (`[0xDC09F0]` is 1, not 0x100/0x101, and
  `[0xE38A50]` bit 0x10 is clear) and resets through `0xE -> 0x15 -> 0x10 -> 9`.

That is why the insert path is silent, but it is **not** the crash: the player's
bundle is installed by the core/scene bundles at boot, long before the cutscene
step runs. Treating the missing `0x53DCC0` as the crash's cause would send the
next session at a stage slot that the crash does not need.

### Not verified

- Which branch inside `0x637BC0` returns 0: the container parse (`[edi]`,
  `[edx+4]`, `[edx+8]`) or the name match in `0x746880`/`0x77A1D0`.
- Whether the UI/font lookup uses the same resolver. It is the obvious
  shared-root candidate for the missing-glyph defect (same registry, same
  state 8, same resolver family), but that is a hypothesis, not a measurement.
- Whether the original binary resolves the same sub-resource: it cannot be
  probed with these tools.

### Tool notes that cost time

- `--poison` is **one** dword and `--firsthit` is **one** window: the last
  instance wins, silently, so passing several narrows the trace without saying
  so. Use one of each per run.
- The host redirects stderr into `<game>\korra_host-*.log`, so `build\run.err`
  stays empty and all runtime trace output is in that log.

## 2026-11-20 (crash session, second round) - FIXED: the crash was a lifter defect in pcmpistri

The player's asset gate was not a data problem and not a missing install. `strstr`
(0x77A1D0) could never return a match, so `sub_00637BC0`'s sub-resource resolve
returned -1 and gate 2 rejected every player. The cause is in the lifter, and it
is now fixed: new game starts, the cutscene plays, and the first area is
playable.

### What the original code does

`strstr` dispatches on `__isa_available` (`cmp dword ptr [0x9f4d20], 1` at
0x77A1DB). This CPU reports 2, so the SSE4.2 path at 0x77A2EB runs. It decides
with **four** flags:

| site | instruction | branch | meaning |
|---|---|---|---|
| 0x77A34F | `pcmpistri xmm0, [eax-0x10], 0x0C` | `ja` | CF=0 and ZF=0: no match, no NUL in the block -> next block |
| 0x77A358 | (same flags) | `jae` | CF=0: no match and the block ended -> return NULL |
| 0x77A38B | `pcmpistri xmm1, [esi-0x10], 0x0C` | `jno` | OF = IntRes2[0]: a match starts at this position |
| 0x77A392 | (same flags) | `jns` | SF = IntRes2[15] |

`PCMPISTRI` defines CF, ZF, SF and OF (SF = IntRes2[15], OF = IntRes2[0]; AF and
PF are undefined). The lifter carried only ZF and CF, and emitted the following
branches through a synthetic compare with `_flag_a = ZF?0:1, _flag_b = 0`, which
evaluates to `ja -> (ZF==0)`, `jae -> 1`, `jno -> 1`, `jns -> 1`
(`CMP_NO` is literally `1`). So:

- `jae` was always taken: the scan returned NULL as soon as a 16-byte block
  contained a NUL character;
- `jno` was always taken: the needle-verify loop always rejected its candidate.

Either one alone makes a positive result impossible on this CPU. The engine's
other string work did not notice because those callers only need "not found".

### Why it surfaced as the new-game crash

`sub_00637BC0` hardcodes the needle (`push 0x941F7C` = `_battleParameter.bin`)
and resolves it inside the loaded DAT container. All 11 of its call sites are
`find asset -> resolve battle parameters`, so every character's battle data was
missing. `sub_004E85A0` (gate 2 of player activation) treats a failed resolve as
a rejected player, the player object is destroyed, `0xCA3E58` stays 0 and the HUD
update dereferences NULL.

### The fix (pcrecomp `b0b2ce2`, Korra-0004)

- `recomp_pcmpistri` reports all four defined flags: hardware `_mm_cmpistrs` /
  `_mm_cmpistro` on x86, and SF = IntRes2[15], OF = IntRes2[0] in the portable
  body.
- New `recomp_pcmp_eflags()` packs them into a packed EFLAGS.
- `lift32.py` emits that packed state after `pcmpistri` (`_flag_k = FK_EFLAGS`),
  so every following `jcc` compiles to `recomp_cond(FK_EFLAGS, ...)` and reads
  the instruction's real flags.

On x86 this is exact by construction: the instruction runs on the host CPU and
the branch reads its actual flags. Relift: 37,029/37,029, 0 failures, 246 s, 6
`pcmpistri` sites (the `strncmp` family in `recomp_0042.c` included). Full
rebuild 472 s.

### Verification (one run, `KORRA_PAD=1 KORRA_PAD_AUTO=1 KORRA_TRACE_SCENE=1`)

- No crash for 321 s (the defect used to fault at ~70 s, always at
  `0x00603C00`).
- `0xCA3E58` becomes **non-zero** (`player=0x0D3EAB60`): the player was accepted,
  so gate 2 passed and the resolve worked.
- Scene flow now reaches values that were unreachable before: `0x12` (stage
  request) -> `0x14` -> `0x16`. `0x14` only advances when the installer state is
  **4**, i.e. the slot installer (`0x53EB40` -> `0x53EBB0` -> `0x53DCC0`) ran to
  completion. The registry grows from 58 to 94 records while the stage loads.
- **Gameplay reached**: `build\shots\newgame-312s.png` shows Korra in the first
  area with two enemies and the tutorial prompts ("Light Attack X", "Heavy
  Attack Y", "Jump A").
- **Text renders**, including the same message box the pre-fix build drew as a
  black rectangle (`build\shots\acp1252.png` at 04:32 vs `newgame-101s.png` for
  the panel and its text). See the shared-root note below.

### Shared root with the text defect - confirmed

The text session's chain (font asset / string lookup -> glyph layout -> draw)
runs through the same CRT string routines. With `strstr` fixed, the title
screen's message box and the in-game tutorial prompts draw their strings, so the
missing-glyph defect and the missing battle parameters have **one** cause: the
`pcmpistri` flag model. That is measured, not inferred: no draw-path or font
change was made in this session.

### Open, measured, not explained

- The in-game frame rate in that run was **3 fps (298 ms/frame)** according to
  the machine's overlay, while the menus measured ~59 fps earlier. Not
  investigated here; the quirk probe (`KORRA_QUIRK_PROBE=1`) logs frames/s and
  the frame interval, and is the first thing to run next.
- The stray `--poison`/`--firsthit` traps from the previous entry still apply.

### Not verified

- A New Game played past the tutorial prompts (the pad script only taps A and
  START).
- Behaviour on a CPU where `__isa_available` is 2 but the ambient `strstr` path
  differs (arm64 uses the portable body, which now also carries SF/OF but is
  still unverified against hardware).

## 2026-11-20 - M3 close: portable pcmpistri fixed and verified; FCOMI*/LAHF audit fix

Scope: the pinned `pcrecomp` tree, `tests/`, and the `m3-prep` merge. No host
file was edited and no game run was made (the perf session owns those).

### STEP 0 - merge and re-validation

- `m3-prep` (forked at `3b037e6`, before the SF/OF fix and the relift) was
  rebased onto `origin/main` (`03dfc01`) with no conflicts, pushed, and
  fast-forwarded into `main`: `origin/main` = `e1f8537`.
- `tools/scan_pcmpistri.py` on the shipped exe still reports 6 sites:
  `0x77A34F`/`0x77A38B` imm8 `0x0C`, `0x77C33B` imm8 `0x40`,
  `0x77CF3D`/`0x77CF53`/`0x77CF88` imm8 `0x0D`. The current lifted tree has 28
  instances (12 `recomp_0041.c`, 16 `recomp_0042.c`) and all 28 already use
  `recomp_pcmp_eflags` / `FK_EFLAGS`.
- `tests/run_pcmpistri_diff.ps1` still reproduced both defects: `0x0C`
  28,480/470,000 (6.06 %), `0x0D` 469,592/470,000 (99.9 %), `0x40` 0. So the
  defects are in the **portable body**, not the relift.
- The "lifter drops SF/OF" note is **stale**: the lowering carries all four
  bits and x86 takes them from the CPU. The portable body's own SF was wrong
  in a different way (see below).

### The fix (Korra-0005, `pcrecomp` `dc18f83`)

`recomp_pcmpistri` is rewritten against the SDM rules: four data formats, four
aggregations, four polarity modes, both index directions, and the real flag
definitions. Notes worth keeping:

- **Equal-ordered does not window to `b`'s null.** The match condition is "all
  valid `a` elements equal `b[j+i]`, with `j+i <= 15`"; an empty needle matches
  every position (all 16 bits set, index 0, CF=1). A non-empty needle may only
  start at `j <= len(b) - len(a)` when `b` has a null, or at any `j <= 15` when
  `b` has none (partial comparison at the edge). The old
  `i + len1 <= len2` rule is what produced the 6.06 % divergence.
- **Equal-each sets bits to 1 past the longer operand's end**, and 0 between
  the two lengths. Modelling it as a plain byte-wise equality with `b` masking
  is wrong even when the data is null-padded.
- **Masked positive == positive**; masked negative is a negative XOR limited
  to `b`'s valid lanes. Polarity must be applied before CF/OF are derived.
- **SF is "any null element in `a`"** (ZF is the same for `b`), not
  `IntRes2[15]`. The harness had been hashing only `flags & 3`, which hid it.
- **Word mode (`0x0D`) returns the element index 0..7, 8 for no match.** The
  lifted code at `0x77CF53` applies `lea edx,[edx+ecx*2]`; pre-scaling in the
  body would double it.
- No relift: the header is compiled by every TU, so a rebuild is enough. The
  Windows host build was not run (perf session owns it).

### Verification

`tests/run_pcmpistri_diff.ps1`, 470,000 cases x 16 imm8 = **7,520,000 tuples**,
Win32 SSE4.2 vs the x64 portable body, comparing index and ZF/CF/SF/OF:

```
PASS: portable body agrees with hardware on all 7520000 tuples (16 imm8 values)
PASS: 21 Korra site-shaped cases agree (0x0C strstr, 0x0D wide string, 0x40 strrchr)
```

The site-shaped cases are new (`tests/pcmpistri_sites.c`) and use the shapes
the six sites see: asset paths/class names for the byte `strstr`, the same names
as UTF-16 for the wide path, and a single searched byte for the `strrchr` msb
form. Named sites: `0x77A34F`/`0x77A38B` in `sub_0077A1D0` (`strstr`),
`0x77CF3D`/`0x77CF53`/`0x77CF88` in the wide-string family
(`sub_0077CEA8`/`sub_0077CF2B`/…; word index then `*2`), `0x77C33B` in
`sub_0077C280` (msb byte scan).

### Audit: other partially modelled flag writes

Evidence: a capstone pass over every catalog block (1,495,735 unique
instructions, 271,473 unique blocks) tracking the last flag writer and the next
flag consumer. Instructions whose flags are *undefined* on hardware (DIV/IDIV;
BSF/BSR except ZF; 1-operand MUL/IMUL except CF/OF; SHLD/SHRD OF; logic AF;
shift count > 1 OF) are deliberately not modelled and are not defects.

- **Fixed: `FCOMI/FCOMIP/FUCOMI/FUCOMIP` + `LAHF`** (Korra-0006, `5cfccf3`).
  The `i` forms write ZF/PF/CF and clear OF/SF/AF, but the lifter published
  only the lazy `'fcom'` tuple, so `LAHF` (and `PUSHFD`) reported the flags of
  an earlier instruction. Eight real functions use MSVC's
  `fucompi st(1); fstp st(0); lahf; test ah, 0x44; jnp` float-zero test
  (`0x8523E9`, `0x8524A7`, `0x864164`, `0x8641F1`, `0x866911`, `0x8788B4`,
  `0x884DE2`, `0x897823`). The i-forms now publish `FK_EFLAGS` via
  `recomp_fcomi_eflags`; the plain FCOM/FCOMP/FUCOM/FUCOMP forms are untouched.
  Codegen change, so the tree was relifted (37,029/37,029, 0 failures, 259 s).
  The 11 changed generated files (0053-0063) compile as one TU with the host
  flags. **Not runtime-verified** - no game run from this session.
- **Not a defect: `imul r32, r/m32, imm32` then `jbe`** at `0x4E7180`
  (`sub_004E7062`/`sub_004E70C1`/`sub_004E712B` - three overlapping catalog
  entries). The surrounding bytes decode to nonsense (`push 0x6869004E`,
  `add bh, dh`) with ASCII-looking immediates, i.e. a misdecoded data region,
  not code. No fix.
- **No sites** for: `shl/shr by 1` followed by an OF condition; `bt*` followed
  by a direct CF reader (`adc`/`sbb`/`rcl`/`rcr`); rotate followed by an OF
  condition. Those remain latent traps for the next binary, not Korra defects.

### Not verified / open

- The arm64 body is validated against one Intel CPU, not against arm64 silicon;
  no device run exists.
- The FCOMI fix has no game-level run yet. First check for the perf session:
  a float `== 0` test at the eight addresses above, and a rebuild before any
  further profiling (the generated tree changed).
- `tools/scan_pcmpistri.py` counts static sites only; the 28 lifted instances
  are catalog aliases.

## 2026-11-20 (later) - first-area performance: where the 298 ms goes, and why it is not the game

Owner report: menus ~59 fps, first area ~3 fps / 298 ms, cause unknown, M2 gate held
until it is explained. This session owns that defect. Runs use `build\run_perf.exe`
(copy of the 03dfc01 build); `run_text.exe` was never touched.

New instruments (host-only, inert unless their env var is set):

| file | knob | what it measures |
|---|---|---|
| `src/host/perf_probe.c` | `KORRA_PERF=1` | per frame: wall time from `[frameMgr+0x5c]`, process CPU, time inside every bridged Win32 call with the lifted caller, d3d9 device Present count, main-thread EIP samples; per second: present/s, CPU%, top callees of the main thread *and of every other guest thread*, and a census of all 24 guest threads |
| `src/host/present_tune.c` | `KORRA_PRESENT=1` | reads (and can rewrite) the `D3DPRESENT_PARAMETERS` block of the first `IDirect3D9::CreateDevice`, identified structurally because COM methods have no name in the runtime's table |

### The limiter is not the cause, and the numbers say so per frame

Every run, every frame, from boot to the first area:

    [perf] cfg int=50 mode=0 flags=0x0

`[frameMgr+0x58]` = 50 (1/3-ms units = 60 fps), `DAT_00e4d210` = 0 (60 fps branch),
`0xE4D01C` bit 2 clear (limiter enabled). The value never changes, so the guest is
never asking for a 300 ms frame. The limiter also cannot produce one: it exits its
`Sleep` loop as soon as the elapsed time reaches the interval, so its worst case is
one 15.6 ms timer tick.

### What a first-area frame actually is

Steady first area (overlay detached): 17.5-25 ms/frame, 40-58 fps, ~2,200 bridged
calls per frame, process CPU 1.5-1.9 cores across 24 guest threads. The main
thread's own breakdown for a 25 ms frame:

    dt=25ms  nat=22ms   WaitForSingleObject 16-21ms in 34 calls (caller 0x740CC0)
                        InterlockedCompareExchange 9-14ms in 400-670 calls
                        Enter/LeaveCriticalSection ~2-4ms in ~400 calls
                        PeekMessageA/DispatchMessageA ~1ms

So the frame is the main thread **waiting for the engine's own job threads** (34
waits, worst single 17.7 ms) while those threads finish the frame's work. That is
the port's structural cost, not the GPU: the runtime runs all lifted guest code
under one machine lock, so the frame is bounded by the *sum* of every thread's
guest work rather than by the slowest one. `InterlockedCompareExchange` at
caller `0x741410`/`0x74F7D0` is the engine's own spinlock, and each spin iteration
crosses the bridge (machine lock released and re-taken), which is why 650 of them
cost 9-14 ms.

### The 298 ms is one Present call, and it belongs to the overlay

`IDirect3DDevice9::Present` (device vtable slot 17, caller `0x759E70`) blocks for
**52-581 ms, median 301 ms, always returning S_OK**, on ~1.2 % of frames. That is
exactly the number the owner's overlay reported (298.3 ms), and it is the whole
"3 fps": a frame whose Present takes 300 ms is a frame at 3 fps.

Three configurations, same build, same scenario:

| overlay stack | Present > 50 ms | first-area fps |
|---|---|---|
| MSI Afterburner + RTSS attached | 100-570 ms stalls, every few seconds | 22-48, drops to ~3 fps while stalled |
| RTSS alone (Afterburner not running) | **0** | 41-58 |
| neither | **0** | 40-58 |

Clearing `D3DPRESENTFLAG_LOCKABLE_BACKBUFFER` (137 stalls in 290 s with it
cleared) changed nothing, so it is not a backbuffer-lock path in the driver. The
stall is inside the overlay's in-process Present hook: the port's call itself
succeeds, and nothing else in the process is holding the device at that moment
(the "other threads" accounting shows no d3d9 call of any length during a stall).

The engine's own present path is sane and was measured, not assumed:
`1280x720, X8R8G8B8, BackBufferCount 0, MSAA 0, SwapEffect DISCARD, Windowed 1,
AutoDepth 0, D24S8, flags 0x1, refresh 0, PresentationInterval IMMEDIATE` - i.e.
windowed, no vsync, so neither vsync nor exclusive fullscreen is involved.

### Consequences for the gate

- The M2-critical claim "the first area runs at 3 fps" is **wrong as stated**: with
  the measuring overlay detached the same build holds 40-58 fps in the first area,
  with no frame over 50 ms except the engine's own job-thread waits.
- The remaining headroom question is the port's, not the machine's: one machine
  lock for all guest threads, ~2,200 bridged calls per frame, and a spinlock whose
  every iteration pays a full bridge round trip. Fixing that belongs to the
  runtime/lifter layer (`pcrecomp`), not to a host shim.
- A separate, unrelated crash ends long runs: the scripted pad walks into a menu
  that calls an unimplemented `ISteamUser` slot, the stub returns 0, and the engine
  dereferences `[0x11]` (guest `0x0076C590`). Not on the performance path.

### Traps found here (also in AGENTS.md section 6)

- A thread census that iterates `CreateToolhelp32Snapshot` **must skip its own
  thread**: `SuspendThread(me)` never returns, and the sampler dies silently while
  the game keeps running - a whole run's data lost to a dead probe.
- `IDirect3D9::CreateDevice` detection by "BehaviorFlags has vertex-processing
  bits + a plausible parameter block" is **too loose**: it matched unrelated COM
  calls, rewrote a guest structure whose slot 4 read `0xDEAD0000` (the runtime's
  dummy return address), and crashed the boot. Match the engine's exact values
  (0x44 then 0x24, DeviceType HAL/REF) and act only on the first match.
- The first-area fps depends on the *measuring tool*: an overlay that hooks
  Present can add 300 ms per frame. Measure with `KORRA_PERF=1` (present/s is
  counted at the real device call) before believing an OSD number.

## 2026-11-20 (later) - first-area follow-up: two levers measured, neither moves it

Owner ruling after the overlay finding: aim for a solid 30-40 fps now, 60 later.
So this pass asked whether the frame is limited by the runtime's per-call cost.

Result first: **the first area already holds 39-58 fps, average 45, with no
second below 30 and no Present over 50 ms** (140 s of gameplay, overlay
detached). The experiments below are recorded as negatives so nobody repeats
them.

| build | first area (t >= 150 s) | min | max | avg | < 30 fps | Present > 50 ms |
|---|---|---|---|---|---|---|
| `/O1`, `volatile`, before Korra-0007 | 135 s | 39 | 58 | 44.9 | 0 s | 0 |
| `/O1`, `volatile`, after Korra-0007 | 135 s | 39 | 58 | 45.3 | 0 s | 0 |
| `/O1`, `RECOMP_MEM_QUAL` empty | 140 s | 40 | 58 | 45.7 | 0 s | 0 |

- **Korra-0007 (`pcrecomp` `82495cb`) is neutral.** The machine-lock hand-over
  does 3 `TlsGetValue` calls and a `GetLastError` clobber per bridge crossing,
  and woke all waiters on every release even with an empty queue; replacing the
  slot with a `__declspec(thread)` pointer, waking only when a ticket is queued,
  and spinning 4096 times before sleeping changed the frame time by nothing
  measurable. Kept because the removals are unconditional, recorded because it
  is not a win.
- **Dropping `volatile` from guest memory is neutral too** (45.7 vs 45.3),
  although the binary shrinks 131.8 MB -> 109.5 MB (17 %): the compiler merges
  and keeps loads, which is the point of the `RECOMP_MEM_QUAL` note in the
  runtime header, but the frame does not care. It stays an opt-in switch
  (`scripts\build_host.ps1 -Fast`, separate `build\obj-fast` tree and
  `build\korra_recomp_fast.exe`), not the default: the default build keeps the
  conservative semantics that have been validated by every run so far.
- **`/O2` cannot be used at all**: `recomp_0021.c` and `recomp_0035.c` (~90 MB
  each) die with `C1002: out of heap space in pass 2` and `cl` raises `D8040`;
  `/Zm1200` does not help. Recorded in the AGENTS ledger so it is not retried.
- **What the frame actually is**, unchanged by all of the above: 25 ms with the
  main thread waiting 16-21 ms on 34 `ReleaseSemaphore`/`WaitForSingleObject`
  handshakes to the engine's worker threads (callers `0x740CC0`, `0x741410`).
  Those workers run lifted guest code, and the runtime serialises all lifted
  code under one machine lock, so the frame costs the *sum* of the workers'
  guest work. Neither faster instructions nor a cheaper hand-over changes that,
  which points at the single-lock design, not at codegen.
- **Still unexplained, harmless at this target:** the first area alternates
  between ~17.5 ms frames (57 fps) and ~25 ms frames (40 fps) on a regular ~9 s
  cycle with the *same* ~2,200 bridged calls per frame. Candidates left are the
  Windows scheduler placing 24 guest threads on P/E cores, a guest periodic task,
  or the game's own auto-save. It never drops below 39 fps, so it does not gate
  the 30-40 target; a run with affinity control and the `mhz` column that
  `perf_probe` now prints would settle it.
- **What a real 60 needs (recorded, not attempted):** the register file
  (`g_eax`, `g_esp`, the x87/SSE state) is global and `mach_enter`/`mach_leave`
  save and reload it around every native call so that another guest thread can
  run in the gap. Making that state thread-local is what would let the engine's
  24 threads run on 24 cores instead of one at a time. That is a runtime
  redesign affecting every pcrecomp host, not a host fix, and it does not block
  M3.

### Frame-rate cap: 30 / 40 / 60, switchable at runtime (`src/host/fps_cap.c`)

The engine has no 40 fps setting, but its limiter interval is just a field:
`[frameMgr+0x58]` in 1/3-ms units (50 = 60 fps, 100 = 30 fps, docs/quirks.md
section 4). The host now writes that field at the requested rate and re-applies
it whenever the engine recomputes it (which happens when `DAT_00e4d210`
changes): `interval = round(3000 / fps)`.

`F9` cycles 30 -> 40 -> 60 -> engine default during play; `KORRA_FPS=30|40|60`
sets the starting rate, unset means the engine's own. No cap is baked in: the
cap is a comfort/measurement knob, not a fix, and it is off unless asked for.

Verified in a single run (probe log, per-second `present/s`):

| cap | measured | notes |
|---|---|---|
| 30 | 28-29/s, never above 30 | rock solid; CPU clock drops to 1520 MHz in the idle tail of the sleep and it still holds |
| 40 | 31-39/s, average 37.5 | cannot exceed 40; dips only where the uncapped game itself drops below 40 |
| 60 | 40-57/s, average 45.4 | the engine's own target, same as uncapped |
| engine default | 40-57/s, average 45.3 | `DAT_00e4d210` = 0, so 60 |

Also measured: the 40<->57 fps oscillation is **not** a CPU power state - the
per-second `mhz=` column alternates 1520/2400 with no correlation to the frame
rate in either direction, including while pinned at 30 fps. The oscillation
stays unexplained and harmless at this bar (it never goes below ~39 uncapped).

## 2026-11-20 (later) - two correctness defects behind the perf work, both fixed and verified

The perf pass left two things open that were *not* performance problems. Both are
now closed, and both were found by looking at the log rather than by guessing.

### 1. memcpy silently copied nothing (lifter catalog, `pcrecomp` Korra-0008)

`sub_0077A440` is the CRT's SSE4.2 memcpy. Its remainder dispatch is
`jmp dword ptr [ecx*4 + 0x77A8C4]` with `ecx` from **-7 to 0**, so seven of the
table's eight arms sit *below* the displacement. `disasm32.table_entries` read a
table only upward, so those arms never became block leaders, the lifted switch
fell through to `RECOMP_ITAIL`, and the dispatch table has no entry for a
mid-function address: the jump resolved to nothing and the call returned with
**no copy performed**. The log line was `ITAIL: unresolved VA 0x0077A8E0 from
0x0077A440`, seen twice in one gameplay run.

Fix: `table_entries` now also reads the contiguous run of code addresses below
the displacement (stop at the first non-code entry, refuse an entry that points
into the table itself, keep every arm within 0x40000 of the table - the same
extent cap the linear walk uses). `generate.py` already did this for extent
discovery; the disassembler did not, and the catalog is built from the
disassembler.

Effect: catalog **37,029 -> 37,327** functions (+298 reachable only through those
arms), lift **37,327/37,327, 0 failures**, 12,652,837 lines, 118.8 s.
`sub_0077A440` now has 30 arms and 102 blocks. Verified in a 368 s run: **zero
`ITAIL: unresolved` lines** (previously two, plus whatever was resolved to the
wrong place silently).

### 2. every long run died at ~5 minutes (Steam stub ABI, `src/host/steam_stub.c`)

`sub_0076C1C0` enumerates the engine's own achievement table (`0x9CF92C`, stride
`0x190`, entries `"ACHIEVEMENT_0"`, `"ACHIEVEMENT_1"`, ...) and calls

    ISteamUserStats::GetAchievement(const char *name, bool *achieved)

- slot 6, vtable offset 0x18, **two** stack arguments. The stub's
  `ISteamUserStats` object answered every slot with the `ISteamUser`
  **one-argument** body, so the call returned NULL *and* popped 4 bytes instead of
  8. The guest's `esp` ended 4 short, and the next member read faulted at `[0x11]`
  in `sub_0076C590` (guest `0x0076C590`, host `0x63D66xxx`), after
  `[steam] ISteamUser: unimplemented slot reached`.

Fix: the stats object now has the real 2014 `ISteamUserStats` shape, each slot
its own thunk with the matching `ret N` - `GetAchievement` (slot 6) answers
"query ok, not unlocked", `GetStat` zeroes its out parameter, `SetStat` /
`SetAchievement` / `ClearAchievement` / `StoreStats` accept and ignore, the string
getters return `""`. Unreachable slots keep a loud log and consume one argument.

Verified: the same scripted run that died at 292-320 s every time now reaches
**t=368 s** and is still playing (`build\shots\perf13_26_noop.png`, t=312 s).

### Frame-rate cap and the logic-rate question

`KORRA_FPS=40` for this run. First area (t >= 150 s, 218 one-second samples):
**min 29, max 39, average 37.6** present/s - the cap holds.

Not verified, stated so nobody assumes otherwise: whether the simulation is
delta-timed. The engine ships a 30 fps mode of its own (`DAT_00e4d210`), so 30 is
a supported rate, and the loop reads a QPC clock each iteration; if any subsystem
instead runs one fixed step per frame, gameplay would be ~33 % slow at 40 fps.
The one test that settles it is timing a known-duration event (an intro movie, or
the auto-save notice) at cap 30 vs cap 60 - wall-clock identical means
time-based.

### Delta-timing test: the sim advances on wall time, not on frames (`perf_probe` `KORRA_PERF_WATCH`)

Question: does the logic step tie to the frame rate (so a cap changes game speed),
or to the clock (so any cap is valid)?

Method: same binary, same scripted path, only `KORRA_FPS` differs; log a sim-driven
state change with its wall clock and frame number (`KORRA_PERF_WATCH=0xE38A84`,
the sequencer's current step id). The boot chain is the one fixed-length event both
runs share: menu step `0xF00` is reached by the engine itself, with no input.

| cap | menu step `0xF00` reached | frame | frames/s over the boot |
|---|---|---|---|
| 60 | **25.2 s** | 953 | 37.8 |
| 30 | **34.1 s** | 577 | 16.9 |

Read it against the two hypotheses: 2.24x fewer frames per second, so a
frame-stepped chain would have taken ~2.24x longer (56 s) and a purely time-based
one exactly 25.2 s. Measured 34.1 s - **between** the two, i.e. neither.

That is what a mixed boot looks like: asset loading and movie playback are
time-based (that is the bulk of it), and a smaller part - fades and a couple of UI
waits - is counted in frames. It is **not** frame-stepping: if the sim itself were
frame-counted, the whole chain would have scaled with the frame rate, and the
game's own shipped 30 fps mode would run at half speed, which it does not.

Two further data points, both consistent and both honest about their limits:

- the auto-save notice measured by pixel (a 120x40 patch of its pale paper) lasted
  **5.48 s at cap 60 and 4.75 s at cap 30** - but in that window both runs were
  boot-bound at ~17 fps, so the measurement cannot discriminate and is not claimed
  as evidence either way;
- a menu-to-cutscene interval is **not** usable: the transition is triggered by the
  scripted pad tap, so it measures the tap, not the game.

**Conclusion for the port:** the logic advances on wall-clock time, so 30, 40 and
60 are all valid caps; the residual frame-counted UI waits make the engine's own
two rates (30 and 60) the safest choices, and 30 the default on Android - it is
also the cheapest thermally and the one the shipped engine already defines.
`KORRA_FPS` stays a knob (F9 cycles 30/40/60/engine on the Windows host).

What would settle the *gameplay* sim beyond doubt: watch a monotonic game-time
float and compare its advance per wall second at cap 30 vs 60. That needs the
address, which this pass did not hunt; the boot-chain result above is the evidence
the decision rests on.

## 2026-11-20 - M3 session: Step 0 device confirmation and Step 1 silicon differential

Both steps are device-measured, not planned.

### Step 0 - the device

| Item | Value |
|---|---|
| `adb devices` | `<device-serial>  device  product:e1sxtur model:SM_S921B device:e1s transport_id:1` |
| Model / SoC | Galaxy S24 (SM-S921B, `e1s`), Exynos 2400 (`s5e9945`) |
| Android | 17, SDK 37, build `CP2A.260605.016.S921BXXSIEZJ1`, patch 2026-10-05 |
| ABI | arm64-v8a |
| Vulkan | **1.3.304** (`VK_KHR_android_surface` + `VK_EXT_debug_utils` in the instance) |
| Driver | `Samsung Xclipse 940`, vendor `0x144D`, device `0x2600200`, `driverID 21`, **SPAL 25.3.3** (`git 9d9e5e5dc2`) |
| SurfaceFlinger | `(Samsung Xclipse 940) on Vulkan 1.3.304`, GLES 3.2 driver 25.6.4 |

Probe: `tools/android/vk_probe.c`, built and run by `scripts/android_probe.ps1`
(NDK clang, arm64 API 29, pushed to `/data/local/tmp`, no APK). Full output is in
§ "Vulkan feature probe" below.

**The feature that changes the plan: `textureCompressionBC` is 0 on this driver.**
BC1/BC2/BC3 are *sampled* and *blit-src* (both tiling modes) but never a colour
attachment. Everything else DXVK wants is present: geometryShader, tessellation,
multiview, shaderClipDistance/shaderCullDistance, depthClamp, fillModeNonSolid,
samplerAnisotropy, samplerYcbcr, timelineSemaphore, bufferDeviceAddress,
descriptorIndexing, drawIndirectCount, dynamicRendering, synchronization2,
maintenance4, transform feedback, vertex divisor, depth clip enable,
robustness2, custom border colour, pipeline library. Limits: 16384 max 2D/FB,
8 colour attachments, 16 viewports, 8 clip/cull distances, 2 heaps (5.3 GB +
64 MB MTL), 165 device extensions.

Also probed and **unsupported**: `D24S8` has *no* format features; `D32F` is
depth-attach + sampled, `D32S8` was not in this run's list. This is the
`D3DFMT_NULL`/depth path to watch in the DXVK log - it fails as a black/no-depth
frame, not as a shader error (same class as `docs/quirks.md` §3 warns about).

#### Vulkan feature probe (verbatim, trimmed to the rows that gate DXVK)

```
instance api=1.1.0 / physical 1.3.304
[device 0] Samsung Xclipse 940
  driverID=21 driverInfo=SPAL Driver version: 25.3.3, git hash: 9d9e5e5dc2
  feature textureCompressionBC              0
  feature textureCompressionETC2            1
  feature textureCompressionASTC_LDR        1
  feature geometryShader                    1
  feature depthClamp                        1
  feature fillModeNonSolid                  1
  feature shaderClipDistance                1 / shaderCullDistance 1
  feature multiViewport 1 / independentBlend 1 / sampleRateShading 1
  vk11 multiview=1 shaderDrawParameters=1
  vk12 timelineSemaphore=1 bufferDeviceAddress=1 descriptorIndexing=1
       drawIndirectCount=1 scalarBlockLayout=1 hostQueryReset=1
  vk13 dynamicRendering=1 synchronization2=1 maintenance4=1
  fmt X8R8G8B8/A8R8G8B8   SAMPLED STORAGE COLOR_ATTACH BLIT_SRC BLIT_DST LINEAR
  fmt D24S8               (none)
  fmt D32F                SAMPLED DEPTH_ATTACH BLIT_SRC BLIT_DST
  fmt DXT1/3/5 (BC1/2/3)  SAMPLED BLIT_SRC (optimal), SAMPLED BLIT_SRC (linear)
```

### Step 1 - `pcmpistri` and `FCOMI` on arm64 silicon

The x86 oracle is the hardware run from `tests/run_pcmpistri_diff.ps1` (Win32
intrinsics). The arm64 run is the same source compiled with the NDK and executed
on the S24 (`scripts/android_probe.ps1`), so the portable body is compared
against the *instruction itself*, not against the x64 build of the same body.

| Harness | Inputs | arm64 result |
|---|---|---|
| `tests/pcmpistri_diff.c` | 470,000 tuples x 16 imm8 = **7,520,000** | **0 differences**, index + ZF/CF/SF/OF, byte-identical 60,160,000-byte result files |
| `tests/pcmpistri_sites.c` | the six sites' shapes (0x0C strstr, 0x0D wide, 0x40 strrchr) | **21/21 identical** to the x86 hardware table |
| `tests/fcomi_diff.c` | 200,576 cases (24 fixed values cross product incl. NaN/inf/denormals + LCG bits) | `model_eflags_hash=E92A81FC`, `model_ah_hash=077DC5FC` = the x86 hardware hashes; the LAHF/`test ah,0x44`/`jnp` idiom agrees on every case |

The three site imm8 values the binary uses are covered with the same inputs that
the game's CRT path feeds them, and the FCOMI harness compares the exact
EFLAGS image the lifter publishes. Step 1 is green on the target silicon; nothing
was patched in the runtime as a result.

## The CRT's R6009 is a `WideCharToMultiByte` contract bug, not an environment problem (2026-11-21)

The first Android boots reached the lifted CRT and then printed

```
[msgbox] Microsoft Visual C++ Runtime Library: Runtime Error!
Program: /data/user/0/org.korra.recomp/files/game/LoK.exe
```

and exited 255. The message text is the CRT's own `_NMSG_WRITE` table, and the
table is in the exe, so the failing code can be read rather than guessed:

| Address | What it is |
|---|---|
| VA `0x8FF8F8` | CRT message table: 0x17 entries of `{int code; wchar_t *text}`, 8 bytes each |
| RVA `0x4FF0C8` | `R6009`: "not enough space for environment" |
| `sub_0077EA5E` | table lookup (`recomp_0042.c:54139`) |
| `sub_0077EA82` | `_NMSG_WRITE(code)` (`recomp_0042.c:54196`) |
| `sub_0077E425` | message then `_exit(code)` - the `_amsg_exit` shape |
| `0x77C490` | `push 9` + `_amsg_exit` inside the entry function at `0x77C3B7` |
| `sub_00782B48` | `_setenvp` count/walk; returns -1 when the env pointer is NULL |
| `sub_00782D50` | `GetEnvironmentStringsW` -> `WideCharToMultiByte` -> heap block |

`sub_00782D50` queries the ANSI size with `cchWideChar = 1` for the empty
environment (the block is a single NUL, plus the terminator). Our shim stopped
at the first NUL and answered **0**, then refused to write into the zero-byte
allocation, so the env pointer became NULL, `__setenvp` returned -1, and the CRT
reported R6009.

Windows converts **exactly** `cch` units when the count is explicit, including
embedded NULs, and converts up to and including the terminator when it is
negative; a size query therefore returns at least 1 for an empty string. Both
`WideCharToMultiByte` and `MultiByteToWideChar` now do that, and CP1252 mapping
replaced the `& 0x7F` mask.

Two other startup defects were found with the same evidence and fixed:

- `GetCPInfo` wrote `MaxCharSize = 2` as a DWORD for CP1252 (it is a BYTE at +0
  and must be 1), which puts the CRT on its DBCS path.
- `GetSystemInfo` wrote `dwAllocationGranularity` at +24 and `wProcessorLevel` at
  +28; `SYSTEM_INFO` has them at +28 and +32.

An audit of every stdcall shim against the SDK also produced these fixes before
the next boot: `RegisterClassExA` now reads `lpfnWndProc` at `WNDCLASSEXA + 8`
(was +12, i.e. `cbClsExtra`, so no WndProc was ever registered),
`LCMapStringW`/`CompareStringW` read their arguments in the documented order,
`IDirectInput8` has per-slot arities instead of one shared thunk (QueryInterface
pops 2, GetDeviceStatus 1, RunControlPanel/Initialize 2), `DrawRectPatch` pops 3,
and `IDirect3DSurface9::LockRect` takes `(Flags, pLockedRect, pRect)` - the
staging implementation had flags and the out-structure swapped.

`tools/check_shim_arity.py` (own tool, compares against pcrecomp's win32hle)
found the earlier `GetStringTypeW` over-pop; the audit above covers the symbols
that have no reference counterpart.

## 2026-11-21 - M3 asset-load deadlock: what the device log says, what Windows says, and the one run that decides

The phone was away, so this session was evidence only: the device log from the
last boot, the Windows build of the same binary, the shim sources, and a probe
built for the next device run. Nothing here is called "fixed".

### The device run ends inside the pool worker, not in the spin

`logs/log.txt` (177,149,654 bytes, 3,688,251 lines) is 99.6 % one repeated line:
**3,674,552** `ICALL: unresolved VA 0x00000000 from 0x007414E0`, first at line
3689 and running to the last line of the file. Everything else is ~6,900 lines.

`sub_007414E0` is the pool worker loop, and the flood is its unguarded job call:

```
7414f3  mov eax,[edi+0xa8] / test / je        ; guarded start hook
741502  mov ebx,[0x8fd0a4]                    ; WaitForSingleObject
741508  push -1 / push [esi+8] / call ebx     ; wait on the slot's posted semaphore
74150f  test byte [edi+8],2 / jne 74153c      ; manager shutdown bits
741515  push [esi+0x14] / mov eax,[esi+0x10] / push 0 / call eax   ; <-- fn == NULL
741522  push 0 / push 1 / push [esi+0xc] / call [0x8fd0a8]         ; ReleaseSemaphore(done)
74152f  push -1 / push [esi+8] / call ebx     ; wait again, same esp as the head wait
741536  test byte [edi+8],2 / je 741515        ; loop
```

The thread that floods is `fs=346315E0` = **token 265**, the last of the fifth
`CreateThread` batch, `param=346301E0`. Its recorded end:

```
2658  [w346315E0] WaitForSingleObject esp=0xC27FFF34
      wait: h=00000012 kind=3 manual=0 signaled=0 ms=4294967295 main=0   <- blocks on a semaphore
3688  wait: h=00000012 woke after 6 polls fs=346315E0
3689  ICALL: unresolved VA 0x00000000 from 0x007414E0
3690  [import] ReleaseSemaphore esp=0xC27FFF30 ebp=0xC27FFF4C
3692  [import] WaitForSingleObject esp=0xC27FFF34 ebp=0xC27FFF4C
3694  wait: h=00000000 notobj ms=4294967295 fs=346315E0   -> returns at once, forever
```

So the worker woke into a slot whose job function `[esi+0x10]` was **0**, called
it, released the done semaphore, and then found `[esi+8]` — the posted
semaphore — also **0**. From that point the wait is not a wait: `obj_get(0)`
rejects it, nothing maps it, and the guest loops as fast as the machine lock
allows. **The terminal state is a slot whose two handles are gone, not a spin.**

The `[spin] InterlockedExchange` lines are elsewhere in the file and unrelated:
only 200 in the whole log, all between lines 1395 and 1637, from `0x008D116E`,
and the main thread's spins retire as soon as a worker wakes (`woke after 45
polls` at 1558). The `0x2139780C` spin is a symptom of a worker not running
yet, and on this run it is short-lived.

### The workers, named

Ten `CreateThread` calls in the log, all sharing the CRT bootstrap
`va=0x0077CAEE` (so that VA names the CRT, not the worker body):

| tokens | created | first object waited on | group |
|---|---|---|---|
| 256-260 | not suspended, four different call sites | manual-reset events `0x2 0x3 0x4 0x5 0x6` | file/data pool |
| 261-265 | `suspended=1`, one call site `esp=0xC03FF504` | semaphores `0x8 0xA 0xC 0xE 0x12` | asset pool |

All 14 `CreateSemaphoreA` calls come from one frame (`esp=0xC03FF584
ebp=0xC03FF5C8`): the five pool semaphore pairs. The last `ReleaseSemaphore`
before the 6-poll wake are five releases at `esp=0xC03FFF48`, each followed by
`OpenThread`, `WaitForSingleObject`, and two `CloseHandle` — a join-and-destroy
sequence, and the same frame accounts for the 14 `CloseHandle esp=0xC03FFF04`.

### Windows ground truth (the discriminator)

`build\run_deadlock.exe` (own copy of `korra_recomp.exe`) with `KORRA_TRACE=1`,
25 s, killed: the game boots (37,327 functions dispatched, guest entered at
`0x0077C3B7`) and reaches the menu. Trace:
`The Legend of Korra\korra_host-20261121-042213.log`.

- Every pool semaphore is `CreateSemaphoreA(NULL, 0, 1, NULL)` — initial count
  0, maximum 1 — from `sub_00740F30` and `sub_00740580`.
- Workers are created by `sub_007415F0` (five, `_beginthreadex(... ,
  CREATE_SUSPENDED, &[ecx+8])`, `ResumeThread`, `CloseHandle`). It does **not**
  fill the job function; something else does, before the first post.
- The pump `sub_00741410` clamps the batch to capacity `[esi+4]`, takes the
  manager's own spin lock (`InterlockedCompareExchange([esi+0x10],1,0)`, so IAT
  `0x8FD01C` **is** InterlockedCompareExchange — the earlier assumption is
  confirmed, not retired), stores the count at `[esi+0xc]`, then for **every**
  slot of the pool does `ReleaseSemaphore([slot + 0x18i + 8], 1, NULL)` and
  `WaitForSingleObject([slot + 0x18i + 0xc])`. Slot stride 0x18, +8 posted, +0xc
  done.
- In practice five posted semaphores (`0x4B0 0x524 0x510 0x4F4 0x4DC`) and five
  done semaphores (`0x50C 0x528 0x520 0x4F0 0x494`) cycle thousands of times;
  each worker releases its done semaphore from inside `sub_007414E0`.
- **`OpenThread` is never called on Windows in 25 s**, and no worker ever calls a
  NULL job function.
- Handle values are recycled on Windows too (`0x510` is a closed thread handle
  that later becomes a semaphore), so reuse by itself is not the defect.

Windows therefore never posts a slot it did not fill, and it never takes the
join-and-destroy path the device run took.

### The difference the shims make

- `k_OpenThread` is `RET(0xFFFFFFFEu, 3)` — a constant that belongs to no table.
  `k_WaitForSingleObject` cannot resolve it, so **the engine's join returns
  immediately** and the teardown is free to close the pool's semaphores while
  five workers are still parked. That is the whole H2 mechanism: the join is
  the only thing on this path that Windows has and we do not.
- `k_CloseHandle` sets `used = 0` and `obj_new` returns the first free slot, so
  a closed semaphore's value immediately becomes the next object created. The
  worker then waits on a value that is either 0 (nothing created yet) or the
  wrong object.
- `k_WaitForSingleObject(h=0)` has no object and no thread token, so it returns
  WAIT_OBJECT_0 in zero time and the guest spins. Nothing is signalled and
  nothing logs it.

### Where the two hypotheses stand

**H2 (shim semantics) is now the leading hypothesis**, on the discriminator
agreed before the run: Windows posts every slot filled and never joins, the
device posted a slot whose function was zero and destroyed the pool around live
workers. **H1 (structural: a first-use slot stays `fn=0` and a stretched race
kicks it) is not supported by anything in the log** — the pump on Windows has no
unfilled-post path, and the device's terminal state is best explained by the
objects being closed under the workers.

This is narrowing, not closing. Two readings still fit the same log:

- H2a: the pool was torn down before the wake (join no-op), and the worker woke
  into freed handles. Fix: a real `OpenThread`, and a `CloseHandle` that keeps a
  finished thread's record.
- H2b: the wake at 3688 was a legitimate post, but the slot had never been
  filled because on the device the fill happens after the first post. Fix:
  a spin gate (Korra-0009).

`Korra-0009` stays on the backlog. The spin-gate item is about the machine lock
starving workers, which is a real hazard, but it is not what ended this run.

### The probe in the next device build

`src/host_android/kernel32_android.c` and `spine_android.c`, diagnostics only:

- `sem: h=… init=… max=… esp=… ebp=…` on every `CreateSemaphoreA` — the
  create-sem caller-to-handle map (24 records).
- `semrel: h=… n=… esp=… ebp=… fs=…` on the first 60 releases — who posts.
- `close: h=… sem|event|mutex esp=… ebp=… fs=…` on the first 40 closes — which
  object a value stood for when it was handed on.
- `openthread: tid=… access=… esp=… ebp=…` — whether the teardown really joins.
- `slot: h=… param=… cap=… post=… done=… fn=… arg=… w18=… w1c=… mgr=… mgrflags=…`
  on every wake whose block names a semaphore this host created, and always
  when the job function is zero (capped at 400 lines) — the worker's parameter
  block at the moment it was let through, through `ka_thread_self_param()`. It
  cannot ride on the 40-wake budget: the file pool's early wakes spend that
  long before the asset pool's critical one.
- `h=0` waits are logged 20 times and then throttled with a 1 ms sleep. The wait
  can never succeed on that handle, so the sleep cannot hide a real wake; it
  keeps a 22 s device log readable instead of 3.6 M ICALL lines.

Built with NDK clang (syntax check on both files) and
`gradlew --no-daemon -PkorraLinkGuest=ON -PkorraWithDxvk=ON
-PkorraDxvkSource=<absolute prepared tree> :app:assembleDebug`: BUILD
SUCCESSFUL in 31 s, APK 372,232,750 bytes, and all six format strings are
present in the stripped `libkorra.so`. The prepared DXVK path must be
**absolute**; `..\..\reference\dxvk-native` fails `dxvk_android/CMakeLists.txt`
because the cache variable stays relative.

### What the next device run decides

Read in this order:

1. `slot: … fn=00000000` at the wake, after `openthread:` and `close:` lines for
   the pool's semaphores → **H2a**: the join no-op destroyed the pool under live
   workers. The fix is `OpenThread` -> the thread's token (CreateThread already
   writes that same value as `lpThreadId`) plus a `CloseHandle` that retires a
   finished thread instead of recycling it.
2. `slot: … fn=00000000` with no preceding `openthread:`/`close:` and a live
   `mgrflags` → **H2b/H1**: the engine posted an unfilled slot on the device
   only. Then the spin gate (Korra-0009) becomes the fix, and that fact forces
   the backlog item.
3. `sem:` lines whose `h=` are not the five pairs the log expects, or a `close:`
   whose kind is `not an object`, point at table collision rather than either
   hypothesis, and the 24-record map names the aliasing handle directly.

### bsr on a zero source (pcrecomp `e887ef2`, Korra-0003b)

`recomp_bsf32`/`recomp_bsr32` shifted a zero operand forever
(`while (!(v & 0x80000000u)) v <<= 1;`). The engine's SSE2 `strchr` reaches
`bsr` with a zero mask on a 16-byte block that holds neither the byte nor a NUL,
which is its normal case, so the CRT search path could hang with the machine
lock held. Both helpers now return 0 with a `recomp_bsf_zero_hits` /
`recomp_bsr_zero_hits` counter when the source is zero. `recomp_types.h` only,
no relift.

Verification: a standalone probe with the pinned MSVC x86, old body versus new,
`bsr(0)`. The old body was still running at the 2 s budget and had to be killed;
the new body exited 0 and printed `bsr new: bsr(0) = 0`. The defect is proven at
function level. **The on-device attribution is not verified** — the surviving
device log was already built with the guard, so there is no before/after device
run for it. It is deliberately not numbered Korra-0009, which stays reserved for
the spin gate.


## 2026-11-21 - M3 graphics-init abort: Windows ground truth names the 3 MB pool, not 16 MB

Taking over the on-device abort (Android framework start `sub_00847BB0` returns
0x34) with a Windows parity probe, because on Windows the host calls the real OS
and the same boot reaches the menu.

### What the probe is

`src/host/m3_probe.c`, compiled into the Windows host, off unless
`KORRA_TRACE_M3=1`. It chains the runtime's `native32_pre_hook` /
`native32_post_hook` and prints the same snapshots the Android host prints in
`u_GetForegroundWindow` ("pre:") and `u_ClipCursor` ("post:"), plus:

- `cfg=` - the whole 0x38-byte config block at 0xCA1FCC, not just 0xCA1FD8;
- `mgr=`, `fallback cell=`, `a0=` and `fb=` with the cumulative allocator
  fields (+38/+3C/+40/+44/+48/+54/+58/+5C), every non-empty free class, the
  total free nodes/sum/largest and the used-chunk walk - the same lines
  `ka_pool_dump()` prints on the device;
- `budget 0x... -> 0x...` - 0xCA1FD8 polled at every import dispatch, so the
  writer is bracketed between two import boundaries (g_cur_func names the
  function);
- `giant:` for any VirtualAlloc/HeapAlloc of 4 MB or more.

`--poison` is not used: it needs a `-DRECOMP_TRACE` build and only samples one
dword at import dispatches anyway; the probe's own poll is the same measurement
without the second build.

### The chain, read from the binary

Callers are unique (decoded per catalog function, direct `call rel32`):

    0x0076486C -> 0x007657D0 -> 0x00847A30 (GetForegroundWindow)
                -> 0x00847BB0(0xDB8AE8, 0xDB8AA0) -> 0x008478C0 -> 0x00829140

`sub_007657D0` is the answer to "who writes 0xCA1FD8":

    007657D4  push 0xDB8AE8 ; call 0x8479C0     ; defaults into the struct
    007657E3  mov dword ptr [0xDB8AF4], 0x300000 ; +0xC = 3 MB, HARD-CODED
    007657F7  call 0x847A30                     ; GetForegroundWindow
    00765806  mov dword ptr [0xDB8AC8], 0x280000
    00765810  call 0x847BB0                     ; framework start

and `sub_00847BB0` copies its argument over 0xCA1FCC (0x847C24, `rep movsd`
0xE dwords), so 0xCA1FCC+0xC - i.e. 0xCA1FD8 - is 0x00300000 at the decision.
`sub_008479C0` (the defaults filler) is what puts 0x01000000 there when the
caller passes NULL, which is the only path that would ask for 16 MB.

Windows reference state at the post anchor (20 s run, menu reached):

    cfg=0:00000000 4:000000FF 8:000000FF c:00300000 10:3F800000 14:00040000
        18:FFFFFFFF 1c:00000000 20:00000001 24:00040000 28:00010000
        2c:00000000 30:00000000 34:00000003
    post: slot(9B4390)=3  budget=0x00300000
    a0: +58=19, free[71] (fl=17 sl=3) n=1 largest=8173768
    fb: +58=19, free[73] (fl=18 sl=1) n=1 largest=11594904

### The class arithmetic, from the real instruction bytes

`sub_0073E9C0` smears the request (`edx|>>1|>>2|>>4|>>8|>>16`), takes
`popcount` (0x73E4A0 is a literal bit count, not bsr), `p = popcount`, adds
`1 << (p-2)` to the request and calls `0x73E500`, which returns
`fl = max(p-6, 0)` and `sl = (index & (2^(p-1)-1)) >> (p-3)`; the free head is
`[pool+0x40][sl + fl*4]`. The refusal is `fl >= [pool+0x58]` (0x73EA9D ->
0x73EC66, a plain `xor eax,eax; ret 0xc`).

- 3 MB with align 0x40: esi=0x300043, p=22, class (fl=17, sl=3) = index 71 -
  exactly where a0 holds its 8,173,768-byte chunk. **Served.**
- 16 MB: esi=0x01000043, p=25, fl=21 >= 19. **Refused without a search.**

### Correction to the takeover brief

The brief says the device asks for 16 MB because 0xCA1FD8 is 0. That is wrong:
0xCA1FD8 is 0 at the pre anchor on **both** platforms (the struct copy happens
after GetForegroundWindow), and the 0x300000 is a guest immediate, not a config
read or a host value, so the device asks for 3 MB too. The exit-time
`state: CA1FD8=00000000` proves nothing either: the later `post:` dumps show the
manager and the pool globals are zeroed during teardown, which is what the
ExitProcess snapshot sees.

So the device fails at class 71 (3 MB) in a0, and Windows serves the same
request from a single free chunk. What differs is the allocator's *state* on the
device, not the request. The candidate branches are now:
(a) device `[a0+0x58]` below 17,
(b) device free list has no usable node at index 71 (different carve),
(c) the list was corrupted by a guest write from one of our shims.
`ka_pool_dump()` answers all three in one device run.

### Not verified

- The device-side 3 MB request is inferred from the guest code path (unique
  callers, unconditional stores). The device has not yet printed `cfg=` at the
  post anchor.
- The ~326 MB of giants is normal engine behaviour: Windows allocates the same
  four sizes (0x05200000, 0x00A40000, 0x04FA0000, 0x09A20000) through the real
  OS, so no shim is inflating a size.

## 2026-11-21 - M3 graphics-init abort: it is CPUID, not the allocator (0x4C, not 0x34)

**One line:** the device aborted framework start at `0x847BDE` - "this CPU has
no SSE" - because `CPUID_impl` was a no-op on non-MSVC hosts. With
`pcrecomp` Korra-0010 (`09b3465`) framework start returns 3, the 3 MB pool is
created exactly as on Windows, and `dev:17 Present` runs for the first time on
the phone.

### The abort code was never 0x34

`sub_00847BB0` has four exits, and only the last one needs the pool:

```
00847BCA  call 0x871D90            ; singleton at 0xCA2424
00847BCF  call [eax]  slot 1 with (1)
00847BDB  test al,al / jne 0x847BE7
00847BDE    mov eax,0x4C ; ret     ; slot 1 answered 0
00847BE7  call 0x828F90 / test al,al
00847BF0    mov eax,0x4A ; ret     ; byte [0xCA1EF4] == 0  (device: 01)
00847BF9  cmp dword [0xCA2990],0
00847C02    mov eax,0x4B ; ret     ; 0xCA2990 == 0          (device: 21EE9028)
00847C23  rep movsd 0xE dwords, 0xDB8AE8 -> 0xCA1FCC   ; <- never executed
00847C48  cmp byte [0xCA2020],0 / jne 0x847C98
00847C56  call 0x008478C0 (step 1) ; 0x34 only when THIS create fails
```

On the device the three `post:` dumps per run showed `CA1EF4=01` and
`CA2990=21EE9028`, so 0x4A and 0x4B are out; `9B4390` stayed `FFFFFFFF`, so the
0x34 exit was never reached either. The remaining exit is **0x4C**, and the
structural proof is the struct copy: at the post anchor the device's `cfg=`
block (`0xCA1FCC`, 14 dwords) was **all zeros** while the caller's `src=`
(`0xDB8AE8`) was fully prepared - `0,FF,FF,300000,3F800000,40000,FFFFFFFF,0,1,
40000,10000,0,0,0`. A scan of the whole 0xCA1F40..0xCA2060 window found no copy
of it anywhere. The lifted `rep movsd` (recomp_0056.c, `_df = 1`, plain
`memmove`) is correct, so the copy simply was not reached: execution left at
one of the three early-outs.

The previous entry's allocator analysis is therefore a **red herring for this
blocker**: the pool create was never called on the device, and Windows' 16 MB
vs 3 MB question never arose. The allocator facts stay in the ledger because
they are correct and step 1 will need them the moment something fails later.

### What 0x4C tests

`0x871D90` lazily builds a CPU-information object at `0xCA2424`
(vtable `0x91D994`, guard dword `0xCA2434`). `sub_00871C60` zeroes `+4/+8/+0xC`
and then executes `cpuid` leaves 0, 1, `0x80000000`, `0x80000006`. Slot 1
(`0x00871D70`) is four instructions wide:

```
mov eax,[ecx+4] ; and eax,[ebp+8] ; neg eax ; sbb eax,eax ; neg eax ; ret 4
```

i.e. `return (feature_word & mask) != 0`. Framework start calls it with
mask 1, and bit 0 is set by `test edx,0x02000000` (CPUID.1.EDX bit 25, SSE);
bits 1/2/3 come from SSE2 (EDX 26), SSE3 (ECX 0) and SSSE3 (ECX 9).

### Root cause

`runtime/recomp32/recomp_types.h`, `CPUID_impl`:

```c
#ifdef _MSC_VER
    __cpuidex(info, eax_val, ecx_val); ...
#else
    (void)eax_val; (void)ebx_val; (void)ecx_val; (void)edx_val;   /* nothing */
#endif
```

On arm64 the guest's registers kept their inputs, so leaf 0 returned
max-leaf 0, `sub_00871C60` skipped leaf 1 (`jl 0x871CC9`), the feature word
stayed 0, and slot 1 answered 0. The Windows host never saw this because it
runs the real `cpuid`.

### Fix (pcrecomp Korra-0010, `09b3465`)

The non-MSVC branch now answers like a 2008-era Intel x86: vendor leaf 0
(max leaf 0xC), leaf 1 (`family 6 model 60`, EDX `0x078BFBFF` -
FPU..CMOV, MMX, FXSR, SSE, SSE2; ECX `0x00980201` - SSE3, SSSE3, SSE4.1,
SSE4.2, POPCNT), leaf 7 empty, `0x80000000` = `0x80000008`, `0x80000001` =
SYSCALL|NX|LM, brand string, `0x80000006` = 1 MB L2 / 64 B line. No CX16 and
no AVX (neither is lifted). The `g_cpuid_*` masks still apply, so a port can
force the older paths. LoK contains no `pmulld`/`pblendvb`/`popcnt`/`crc32`
sites at all, so the advertised set only steers branches that are already
lifted; SSE4.2 is advertised because Windows takes the CRT's
`pcmpistri` path, which Korra-0004 already fixed.

### Device evidence after the fix (run 2026-11-21 16:17)

Same build, same APK path, one `android_run.ps1 -Seconds 25 -NoGameCopy`:

- `post: 9B4390=00000003 9B44E8=00000003 CA2020=1 CA1EEC=5` - framework start
  returned 3 and two more pool records were used.
- `post cfg=... c:00300000 ... 34:00000003` and `post win 00CA1FD0:
  000000FF 000000FF 00300000 ...` - the struct copy landed, `0xCA1FD8` is
  3 MB.
- `post a0 used[15] 2211B984 sz=3145740 own=00300000` and
  `used[14] ... sz=2621468 own=00280010` - the very chunks Windows shows at
  the same point (`sz=3145740 own=00300000`), i.e. the 3 MB pool came from
  a0's 8,173,768-byte chunk and the free list re-carved to 2,284,632 bytes,
  the same shape as the Windows reference.
- `[import] dev:17 ... f=0x00759E70` four times between 16:17:03.190 and
  16:17:03.366, with `Format VK_FORMAT_R8G8B8A8_UNORM`, `Present mode
  VK_PRESENT_MODE_MAILBOX_KHR`, `Buffer size 1280x720`, `Image count 6`.
  **This is the first Present on the phone.**

Then, 13 ms after the fourth Present, a new and unrelated fault:

```
[import] tex:19 ... f=0x007567A0
ICALL: unresolved VA 0x00000000 from 0x00756840
[crash] signal 11 ... guest_func=0x00737970 esp=0xDEAD0000 eax=1 ebx=1 esi=0x00E1D84C edi=0xDEAD0000
```

`0x756840` calls a NULL function pointer right after a texture call
(`0x7567A0` = a texture-stage setter). This is the next rung, not a regression:
the fill code never ran before because framework start aborted.

### Not verified

- No screenshot yet: the run crashes 13 ms after the first Present, so the
  frame was never captured. `dev:17` returning is measured; that pixels reached
  the screen is not.
- The CPUID object's device fields (`[0xCA2424+4]`) were not dumped directly.
  The 0x4C exit is proven structurally (cfg all-zero + src prepared + the two
  other exits excluded by measured globals), and the fix's effect is proven by
  the copy landing and by Present, but the intermediate `cpuid` values were not
  read back on the phone.
- The new crash at `0x756840` has no diagnosis yet: the unresolved VA is 0, so
  either a vtable slot was never lifted, an out-parameter our texture shims
  leave NULL is being called, or a stage object was never constructed.

## 2026-11-21 — M3: the phone renders; a two-slot texture vtable and a machine-lock deadlock

Device: SM-S921B (Xclipse 940), phone serial <device-serial>, APK built from
`ea585596d` plus the fixes below. Three separate defects, each proven by a run
that reached one step further.

### 1. Texture vtable was two slots short (commit `a75f4b7720f5`)

`d3d9: vtable tex has 20 slots` before the fix. `IDirect3DBaseTexture9` in
`d3d9.h` begins with `SetLOD`/`GetLOD`; the host table went straight from
`IDirect3DResource9` to `GetLevelCount`, so every texture slot from
`GetLevelCount` up was two low. The engine locks a texture through slot 19:

    [import] tex:19 esp=0xC03FFECC ebp=0xC03FFEEC f=0x007567A0
    ICALL: unresolved VA 0x00000000 from 0x00756840
    [crash] signal 11 code=2 addr=0x6c2f902000 guest_func=0x00737970 ...

`0x007567A0` is `call [eax+0x4c]` = `LockRect`, `0x00756840` is
`call [eax+0x50]` = `UnlockRect`; with a 20-entry table slot 19 answered with
`AddDirtyRect` and slot 20 was past the end, i.e. a NULL function pointer. The
crash followed the fourth Present by 13 ms.

Fix: `bt_SetLOD`/`bt_GetLOD` shims, table now 22 entries
(`d3d9: vtable tex has 22 slots`), and `build_vtable` logs its count so the
next mismatch is visible in the log instead of only in a crash. Device run
after it: `dev:17` x3, `tex:19` dispatched, 0 ICALL, 0 crash, no ExitProcess.

### 2. A guest thread deadlocked while holding the machine lock (commit `c26262cb47b8`)

After the vtable fix the run froze after three Presents:

    wd: stalled fs=34645840 depth=1 func=00867E90 imports=40961 back=23356 esp=C3BFFF90

forty times over 150 s, `dev:17` stuck at 3. The stall probe had been
signalling the *main* thread (idle, blocked) and naming nothing; pointed at the
machine holder it captured the real stack, resolved against the unstripped
`libkorra.so` (`llvm-addr2line -f -C -e <so> <pc-base>`):

    ka_host_bt             spine_android.c:264
    k_EnterCriticalSection  kernel32_android.c:397      <- pthread_mutex_lock
    sub_00867E90            recomp_0058.c:47829        <- frame tick
    sub_0087EEE0
    hle_call_guest
    thread_trampoline

Guest stack at the same moment: `[esp]=0xDEAD0000` (bridge return address),
`[esp+4]=0x00CA2034` — the frame tick's critical section, pushed just before
the `call [0x8FD02C]` at `0x867E9A`. So the shim blocked inside
`pthread_mutex_lock` while holding the machine lock, and the thread that owns
that critical section could not run to release it. This is the same class of
bug as the H2a teardown race: a shim that blocks must not hold the machine.

Fix: `k_EnterCriticalSection` trylocks (uncontended and recursive calls keep
the old cost), and only when the lock is held elsewhere unwinds the whole
recursion depth with `mach_leave()`, waits, and re-enters. Proof: the 40
watchdog lines disappear (`wd: stalled = 0`), `cs: contended slot=...` shows
the new path is taken, and the same run reaches `dev:17` x5 with
`tex:19` x2 and no crash.

### 3. Critical-section table: recycle, and 256 was too small

Two limits in the same table surfaced while the frame loop ran:
`DeleteCriticalSection` was a no-op (slots never freed), and a measured run
made 239 `InitializeCriticalSection` plus 18
`InitializeCriticalSectionAndSpinCount` calls — 257 into 256 slots — then
`FATAL: out of critical sections` right after the frame loop started. The
delete path now destroys the mutex and clears the guest's slot word; `MAX_CS`
is 4096. The 256 -> 4096 change is proven (the abort is gone); the recycle
path is not exercised yet, because that same run made **zero** delete calls.

### Evidence of rendering

| run | presents | unique colours in the screencap |
|---|---|---|
| black baseline (before these fixes) | 3 | 1 (pure black) |
| vtable + deadlock fix | 5 | 104,334 |
| plus CS table 4096 | 13 | 132,003 |

The screencap of the 13-present run is 2340x1080 landscape, mean luminance
172 — a near-uniform light grey frame with a dark band at both edges, so
frames are being presented but the picture is not yet identifiable game
content. No title screen has been captured.

### Not verified

- What the presented frame actually contains, and whether the render thread
  keeps up: 13 presents in ~45 s is far from a frame rate, and the run then
  logged `lifecycle: pause` / `lifecycle: low memory` and lost its window. The
  cause of the pause is not established.
- The `DeleteCriticalSection` recycle path (no delete calls in the measured
  run).
- Cube and volume textures still have no host vtable; they would dispatch to
  VA 0 exactly as the 2D texture did. No cube/volume texture has been created
  on the phone so far.
## 2026-11-21 - M3: two host-table defects in a row - the D3D9 object table overflow, then the lock staging leak

The run that froze at ~40 s after thirteen presents (`docs/FINDINGS.md`,
critical-section entry above) was not a deadlock in the engine. Two host
defects, one behind the other.

### 1. `new_token()` wrote past `g_objs` into the kernel object table

The freeze: two asset-pool workers spinning at 700,000 `WaitForSingleObject`
calls a second on handles `0x0A` and `0x0C`, with `wd: stalled
func=00867E90` - the frame tick - never advancing. A repeat-wait probe in
`k_WaitForSingleObject` showed the handles still resolved to `g_obj`
entries, but the entries themselves read `kind=0xB400006E,
manual=0x20000650, signaled=1`: the two halves of a host heap pointer
(the DXVK device prints as `0xb400006e63478dd0`), not an object. `used`,
`maxcount` and `owned` survived, so a 16-byte aligned store had landed at
`&g_obj[0] + 0x100` in one run and `g_obj + 0` in another. `signaled`
reading 1 is why a blocking wait returned at once and the workers spun.

`llvm-nm -S --numeric-sort` on the unstripped `libkorra.so` placed
`g_objs` (the D3D9 token table, `0x30000` bytes = 4096 x 48) at
`0xacf0688` and `g_obj` (the kernel object table, `0x1c00`) at
`0xad27cd0` - `g_obj` starts 0x7688 bytes past the end of `g_objs`, i.e.
631 entries. `new_token()` did `g_objs[g_obj_count++]` with no bound
check and `IUnknown::Release` is `RET(0,0)` ("objects live for the
process"), so nothing ever freed a slot. `d_GetRenderTarget()` and
`d_GetDepthStencilSurface()` minted a **fresh token on every call**, and
the engine calls both every frame: about 4096 tokens in 40 s, and entry
4096 + 631 wrote `g_objs[idx].p` - a host pointer - over `g_obj`'s
`kind`/`manual`/`signaled`.

The import ring (`ka_ring_note`/`ka_ring_dump`, newest 8 non-wait imports
with their `g_cur_func` and `g_fs_base`) and `ka_obj_check()`, called on
every import dispatch, are what named it: the corruption was first seen
on the thread doing `EnterCriticalSection`/`LeaveCriticalSection` around
`0x8D1990`/`0x8D19D0`, and the check caught the write within 8 imports of
its occurrence.

Fix (`954648543d8d`): the table grows (`objs_ensure`, doubling from 4096,
`calloc` + `memcpy`, logs `d3d9: object table grew to N entries`) so it
can never write outside itself, and `new_token` returns the existing
token when the same kind and COM pointer are already registered - the
same object gets the same token, which is what D3D9 promises - which
stops the per-frame churn and its 12-byte guest token leak at the source.
Run `logs/log7.txt` (90 s): `obj: entry` corruption 0 (was 1), `wd:
stalled` 0 (was 40), `dev:17` 13, `tex:19` 10, `ICALL` 0, and the table
grew once, at startup.

### 2. The texture/surface/buffer lock staging buffer was never freed

With the spin gone the run reached the same thirteen presents and then
died differently: `guest heap exhausted (want 921600 bytes at
0x5FFFC800)` three times, then a signal 11 in the CRT's `memcpy`
(`sub_0077D660`, `recomp_0042.c:50142`) with `edi=0` - the engine
memcpy'ing into the NULL the exhausted allocator returned. 921,600 is
exactly 1280 x 720.

`t_LockRect`, `s_LockRect` and `b_Lock` each allocate a guest staging
buffer (`o->lock_va = ka_guest_alloc(size, 16)`) and the matching Unlock
only copied the guest's writes back before dropping `lock_host` - the
staging buffer was **never freed**, so every unlocked surface leaked its
full size. The engine locks a 1280x720 surface per frame; ~780 locks
fills the 1 GiB guest heap.

Fix: `t_UnlockRect`, `s_UnlockRect` and `b_Unlock` now `ka_guest_free`
the staging buffer and clear `lock_va`/`lock_size`, and each Lock frees a
previous staging buffer before allocating a new one (a lock without an
unlock still leaked). Run `logs/log8.txt` (75 s): `heap exhausted` 0,
`crash` 2, `dev:17` 13, `tex:19` 10, `ICALL` 0, `wd: stalled` 0,
`obj: entry` 0.

### The next fault, recorded and not yet fixed

```
d3dx: TextureFromFileInMemoryEx failed 0x8876086C (size 16512)
[crash] signal 11 code=2 addr=0x6b53f6f000 guest_func=0x0053C630
        esp=0xCDFFFF34 ebp=0xCDFFFF3C eax=1 ecx=0 edx=2 ebx=esi=0x2147D1D4 edi=0x00740D50
[crash] pc=0x6C5E480188 off=0x226E188 lr=0x6C5E480138 sp=0x6B3809A9B0
```
`addr2line` puts the fault inside the lifted `sub_0053C630`
(`recomp_0016.c`), which installs per-index callbacks at
`[0xDC0AD4]+0x10/+0x14`, `+0x28`, `+0x58`, `+0x5C`, calls `sub_00741410`,
then calls through `[0xDBE130]->vt+0x10` and `[0xDC0140]->vt+0x10`
(`RECOMP_ICALL`). The guest `esp` (0xCDFFFF34) is at the very top of the
guest stack region while other guest threads sit near 0xC03FF... - read
that as a stack-pointer setup defect for a freshly created thread before
blaming the D3DX failure, since `d3dx: TextureFromFileInMemoryEx failed
0x8876086C` (D3DERR_INVALIDCALL) is the line right before the fault and a
texture the engine expected is now NULL.

**Not verified:** no title screen or first area yet - every screenshot of
a live run so far is either black or the launcher (the app exits during
the run). The game still stops at thirteen presents; the frame burst is
not yet a frame loop, so the 30 fps default and the touch->XInput mapping
are untouched. Tested on the owner's S24 only (`<device-serial>`).

## 2026-11-21 - M3: D3DX puts D3DX_DEFAULT in Width/Height, and the shim forwarded it

`d3dx: TextureFromFileInMemoryEx failed 0x8876086C` was ours, not the engine's.
Three boot textures (256x256 DXT1 43,832 B, 1024x1024 DXT5 1,048,704 B, 128x128
DXT5 16,512 B) are loaded with Width = Height = D3DX_DEFAULT (0xFFFFFFFF),
Format = D3DFMT_UNKNOWN (0) and Pool = D3DPOOL_DEFAULT (1). The shim passed all
four straight to `CreateTexture`, which answered D3DERR_INVALIDCALL for a
4294967295x4294967295 zero-format texture.

Fix in `src/host_android/d3d9_android.cpp` (`dds_upload`): D3DX_DEFAULT and 0 in
Width/Height now mean "take it from the DDS header"; a zero Format is derived
from the header's pixelformat (DDPF_FOURCC -> the fourcc *is* the D3DFORMAT,
DDPF_RGB -> A8R8G8B8/X8R8G8B8/R8G8B8/R5G6B5); DEFAULT/SCRATCH pools upload
through a SYSTEMMEM staging texture and `UpdateTexture`, because a default-pool
texture cannot be locked. The header fields are read at `hdr+19`
(dwFlags, dwFourCC, dwRGBBitCount) - an earlier attempt read `hdr+20` and kept
`fmt=0`, which is how the off-by-one was found.

Proof, one device run (`logs/log12.txt`, 44 s): `TextureFromFileInMemoryEx
failed = 0`, `dds 256x256 mips=9 pf=00000004 fourcc=31545844 -> w=256 h=256
fmt=827611204 pool=1` (DXT1), `1024x1024 ... fmt=894720068` (DXT5),
`256x16 pf=00000041 bits=32 -> fmt=21` (A8R8G8B8); `crash = 2`, `dev:17 = 13`,
`wd: stalled = 0`, `heap exhausted = 0`, `obj: entry = 0`.

Not verified: no title screen or menu is identifiable in `logs/s3.png` yet; the
crash that follows (below) is a separate defect; the cube/volume texture vtables
are still absent and this path did not mint one.

## 2026-11-21 - M3: the phone renders at 30 fps, then faults on a NULL technique pointer

Two corrections and one new fault, measured on the S24 (Xclipse 940, DXVK-native,
MAILBOX, 1280x720) with the build from commit 4a7bdc3.

**1. The present count in earlier entries was a trace artefact.** [import] tracing
stops at 12,000 lines (g_trace_left in spine_android.c; logs/log12.txt, line 14,757
carries "(trace limit; set KORRA_TRACE_IMPORTS=0 to silence)"). Every "dev:17 = N"
figure in this file therefore counts presents only until the trace budget ran out,
not presents in the run. The real present rate is DXVK's own per-present block
("Presenter: Actual swap chain properties", 7 lines): 1,314 of them in a 44 s run,
i.e. about 30 fps sustained on the device. The 30 fps default limiter works and the
frame loop is not stalling; the "bursty presents" reading was the trace cap.

**Correction (2026-11-21, later the same day).** Those 1,314 blocks were *not*
per-present logging. The block is emitted immediately above
`vkCreateSwapchainKHR` inside `Presenter::recreateSwapChain`
(`vulkan_presenter.cpp:104-236`), which destroys the old chain first - so one block
is one real swap chain creation, and the game was rebuilding its swap chain on
every single Present while reporting identical properties
(R8G8B8A8_UNORM, MAILBOX, 1280x720). The 30 fps figure was still right, but it was
30 fps *with* a swap chain per frame. Cause and fix are in the entry below
("the swap chain is no longer rebuilt on every present"); with the fix a 32 s run
creates 3 swap chains instead of ~1,000.

**2. The run now ends in a guest fault at about 1,300 frames.** After the D3DX fix
(4a7bdc3) nothing refuses in the log before the fault: no missing shim, no failed
D3DX load, no heap exhaustion.

    [crash] signal 11 code=2 addr=0x6b532a2004 guest_func=0x007492B0 esp=0xC2BFFD58
    eax=0 ecx=0x00E4D064 edx=2 esi=0x20027120 edi=0

The fault address is g_mem_base + 4: the guest dereferences a NULL pointer at +4.
sub_007492B0 (recomp_0039.c) does mov eax,[ebp+0x10]; mov edx,[eax+4] on its third
argument before testing it, so the engine never expects NULL there. The chain is

    sub_0072EED0:  esi = [edi+0x30]        ; the object's field, NULL
                   call 0x748AB0(edi, "0x9DF3F0", 0)
                   edx = esi; ecx = edi; call 0x736A20
    sub_00736A20:  edi = edx               ; fastcall arg 2
                   push "0x9E5754"; push edi; push 0; push ecx
                   call 0x7492B0           ; arg3 = edi = NULL -> fault

so the source is the object field at +0x30 of the object handed to sub_0072EED0,
read before any check. That function has no direct caller in the generated tree
(only its definition at recomp_0038.c:57650); it is entered through the vtable
dispatcher, so the object type has to be named from the dispatch site rather than
from a call chain. Not yet identified: which create leaves +0x30 NULL, and whether
that field is a technique/material pointer this host should have produced.

Not verified: that +0x30 is meant to be non-NULL on Windows (the Windows reference
run reaches the menu without this fault, but +0x30 was not sampled); the steady-state
frame rate after this fault is fixed; the "lifecycle: pause / low memory" kill seen in
earlier runs is untouched by this entry.

## 2026-11-21 - M3: the swap chain is no longer rebuilt on every present

The 1,314 creations in log12 carried identical properties every time, so the
`m_dirty` path at Present entry was not the trigger (window, vsync and dialog
stable; `UpdatePresentRegion` false after the first frame). That left the two
silent failure loops. The pinned DXVK tree now logs both, plus a surface-caps dump
that only prints when the caps change (`tools/dxvk_prepare.py`, so it is
reproducible): 22 s on the device, `logs/log.txt`, 615 creations.

- The site is `SynchronizePresent`: `vkQueuePresentKHR` returns
  **VK_SUBOPTIMAL_KHR (`1000001003`)** on every present, and DXVK recreates the
  chain on any non-`VK_SUCCESS` result. The acquire prefetch stayed successful;
  the single `VK_ERROR_NATIVE_WINDOW_IN_USE_KHR` at startup recovered by itself.
- Surface caps: `currentTransform = ROTATE_90 (2)` with `supportedTransforms =
  511`, `currentExtent 1280x720`, `minImageCount 4` (5 after the first present),
  `maxImageCount 64`, `supportedCompositeAlpha 8`. The S24 panel is
  portrait-native and the activity is `sensorLandscape`, so the hint is the
  display rotation.
- **H1, the assumed fix, is wrong for the picture.** `preTransform =
  caps.currentTransform` (the brief's suggestion) does stop the churn - 3
  creations, 0 present failures - but the frame comes out rotated 90 degrees:
  the matched-preTransform splash is the churned splash rotated 90 (mean abs
  difference 0.8/255 rotated, 15.2 unrotated). The compositor's rotation is
  exactly what makes the game's landscape frame upright on this panel, so
  `preTransform` has to stay `IDENTITY`. (`ANativeWindow_setBuffersTransform`
  with `TRANSFORM_IDENTITY` changed nothing: caps still report transform 2 and the
  picture still matched H1 frame for frame.)
- **H2, the fix: the result code is the lie, not the surface.** Keep
  `preTransform = IDENTITY` and only honour `VK_SUBOPTIMAL_KHR` /
  `VK_ERROR_OUT_OF_DATE_KHR` from present when the surface really changed.
  `Presenter::isSwapChainStale()` re-queries the capabilities and compares extent,
  transform and `minImageCount` with the ones the chain was created against; the
  recreate path is untouched for every other failure.

Measured with H2 (`logs/log_h3.txt`, 32 s, 2,290 frames until the NULL fault
below): **3 swap chain creations** (down from ~1,000), 2 recreates (the
first-Present dirty path, and the `NATIVE_WINDOW_IN_USE` recovery), 0 present
failures, one warn line naming the ignored result, and the splash frame pixel
identical to the pre-fix picture. The run ends in the same NULL fault at ~2,300
frames, so the churn is no longer what limits it.

Not verified: a >2 minute run (the NULL fault still ends it); a genuinely stale
surface (the staleness check covers it by construction, but nothing has exercised
it - rotating the device is the obvious test); the first-area frame rate.


## 2026-11-21 - M3: the NULL technique pointer is one entry of the frame's pass queue

The crash (see the entry above) is now named end to end. The chain, all of it
inside lifted code with no import in it:

    sub_00541400(0xE4D064)          frame submit (recomp_0016.c:44944)
      -> sub_00748580               releases frame resources, clears +0x30
      -> sub_0074F740               walks the pass queue
           entry {fn=0x0074F8F0, arg=0x22EC4E80, key=0x6B100000}
      -> sub_0074F8F0               thunk: call [obj->vtable + 4]
      -> sub_0072EED0               the render pass itself (recomp_0038.c:57650)
           esi = [device + 0x30]    <- NULL
      -> sub_00736A20               push esi as arg3, 0x9E5754 as arg4
      -> sub_007492B0               edx = [arg3 + 4] before any test  <- fault

`sub_00541400` is called from `sub_005414B0` only, which passes the literal
`0x00E4D064`: a frame/render-context singleton whose real base is `0xE4D060`
(`[0xE4D064]` = the device object `0x20027120`, `+0x20/+0x24` = 1280x720).
The queue head is `[0xDC0F3C]` (gate `[0xDC0F68]`, 62 entries by the time of
the crash); entries are `{fn, arg, key, next, key2}`, inserted by
`sub_0074F7D0` through the allocator at `[[0xDC0F08+0x58]+8]`, and the walk
decodes a class/sub/index triple out of `key`. The one entry that ends the run
was dumped at crash time: `fn=0074F8F0 arg=22EC4E80 next=2044BE80 key=6B100000`.

Its `arg` is a 0x80-byte heap object with vtable `0x0096C88C`
(`[0]=00631DF0 [1]=0072EED0 [2]=0099FAC8 [3]=0073DD00 [4]=00735550`),
constructed in `sub_00732920` by `sub_0073E700(&out, 0x80, ...)` and published
as the global at `0xE46940`; the only literal store of that vtable is
`recomp_0038.c:62838`. So the faulting pass is that object's slot 1.

Two measurements made the remaining question small. First, a sampler
(`debug.korra.watchlog`, below) sitting in `recomp_lookup` and reading eight
dwords at `0xE4D094` on every indirect transfer shows **`[0xE4D064+0x30]` is
never written by any guest code in a 45 s run** - it is zero at the first
sample and zero at the crash, while `+0x34` (`0xE4D098`) flips 1/0 with
`from=00748CB0` (draw) and `from=00749020` (frame reset). The "cleared just
before the walk" reading of `sub_00748580` is therefore dead: the pass never
sees anything but NULL. Second, `sub_007492B0` would bail anyway - with
`arg4 = 0x9E5754`, `ecx = [0x9E5754] = 0` fails its `edx && ecx` test, so the
call is a no-op on every machine. The deref, not the missing value, is what
kills the run: a NULL `+0x30` should mean "nothing to draw for this pass".

So the open question is exactly one: on a machine where this run reaches the
same point, is `+0x30` non-NULL (and which function wrote it), or does the
queue not contain this entry yet? The Android host has no reference for that,
so the Windows host was given the same two probes (`src/host/watch_probe.c`,
`KORRA_ICALL` / `KORRA_WATCHLOG`, called from `recomp_lookup_manual`) and the
build is ready (`build/korra_recomp.exe`); the run was not made in this
session. If Windows writes the field, the sampler prints the writer's VA and
the fix layer is whatever leaves it unset here; if Windows faults at
`0x7492B0` too, the field is legitimately NULL and the layer is the pass queue
(an entry registered for a pass whose target was never set).

New host tooling, all off unless asked for (`adb shell setprop`, hex, no `0x`):
`debug.korra.icall` (caller + first three stack arguments of any indirect
transfer to a VA), `debug.korra.icallfrom` (every target dispatched from a VA),
`debug.korra.watchlog` (eight dwords sampled on every indirect transfer, each
change logged with the function that was running), `debug.korra.watch` (four
lines of memory at a VA plus its pointee, printed by the crash handler, together
with a walk of the pass queue at `[0xDC0F3C]`).

Two tooling corrections worth keeping. `work/LoK.analysis.exe` **does not have
the layout of the loaded image**: guest `0x96C88C` holds the vtable above, but
that VA in the analysis copy is UTF-16 text; the same bytes sit at file offset
`0x56B68C`. Dump statics from the original `LoK.exe` through a pefile section
map. And `g_esp` inside `recomp_lookup` already includes the pushed arguments -
`MEM32(g_esp+0)`, `+4`, `+8` are the callee's first three args (verified against
a live frame).

Not verified: what Windows holds at `+0x30` (the oracle run above); whether the
crash also happens on Windows; that the entry is present on Windows at the same
frame count; the first-area frame rate; anything after frame 2,321.


## 2026-11-21 - M4 audio: the Windows sink, the matrix that *was* the mix, and the event still not posted

Route A is now code, not a plan. The lifted engine keeps Wwise/CRI and mixes;
the host supplies `IXAudio2` 2.7 (`src/host/xaudio2_stub.c`, now a real mixer)
and one output thread (`src/host/wasapi_sink.c`, WASAPI shared, event-driven,
`AUTOCONVERTPCM`, with waveOut and a QPC-paced null backend behind it). The
contract is `src/host/audio_sink.h`; run it from `build\run_m4.exe`.

**Acceptance test 1 passes, with numbers.** Run
`KORRA_XA2_VERBOSE=1 KORRA_AUDIO_DUMP=work\m4_final.wav build\run_m4.exe`,
log `korra_host-20261121-204119.log`: sink `WASAPI shared: 2 ch, 48000 Hz,
32 bits, buffer 1056 frames (22.0 ms), engine-converted 48k float`, first
audible block at 0.032 s, dump **59.0 s**, overall RMS **-31.1 dBFS**, steady
window 3-22 s RMS **-26.8 dBFS**, peak **-9.77 dBFS** (no clipping), underruns
4 (first feed at 5.9 s and the 21.7/21.9/22.2 s movie device teardown) - **0
per minute between 6 s and 21 s**, 0 `OnVoiceError`. Threshold was RMS >
-40 dBFS. The dump is 48 kHz 16-bit stereo and its header is patched once a
second, so a killed run still yields a readable file.

**The defect worth the session: `SetOutputMatrix` is the mix.** The first dump
measured RMS -16.1 dBFS with **peak 0.0 dBFS** - clipped. The mixer had treated
`SetOutputMatrix` as a no-op ("CRI computes the downmix itself") and applied
its own invented 0.7071/0.5 coefficients on top. The engine really installs a
2x2 matrix for its stereo voice and a 6x2 for the 5.1 voice; storing and
applying them moved the peak from +2.7 dBFS to -9.77 dBFS. An 11 dB error that
a laptop speaker hides and a measurement does not.

**Which device the game uses, measured (72 s, verbose 3).** DirectSound8 is
never created; the only non-XAudio2 `CoCreateInstance` in the run is
`MMDeviceEnumerator` at boot (`0x008701F0`, a notification client, not a
stream). Movie/boot audio is CRI's device: a full create/teardown per movie
about 6.5 s apart, and that is the audio from 0 to 25 s. The Hw/Wwise device is
created, `CreateMasteringVoice(2 ch)`, `RegisterForCallbacks`,
`CreateSourceVoice(2 ch float mask 0x3)`, `Start` from `0x0073EC80` - and then
it is never submitted to, never queried, never destroyed. An XAudio2 source
voice is push-only, so no event means no audio.

**So tests 2-6 remain unproven, and the reason is upstream of audio.** No
Wwise event is posted on any screen this build reaches. The title screen runs
at 29-30 fps with the pad polled and posts nothing; pressing A leaves the title
around 40 s onto a static black screen at ~57 ticks/s with `present=0/s` - the
frame-pass-queue fault at the end of this file, not a sink defect. Posting
events from the host is also not yet possible: the image's only name hash
(`0x00847B10`) is FNV-1 over the lowercased name and reproduces every ID in
`docs/audio.md` 9.4 exactly, but it is reached only from wide-string filename
helpers (`0x007685C0`, `0x00768C60`, callers `0x007672A2`, `0x00766E02`), and
the `Hw::Wwise::Command::PostEventWork` RTTI name (file offset `0x5F178C`) has
no pointer anywhere in the image. That is the next lead.

**Android side: written, compile-checked, never run.** `src/host/aaudio_sink.c`
(AAudio exclusive then shared, 48 kHz stereo float, I16 conversion, WAV dump)
compiles clean with NDK 28.2.13676358 for `aarch64-linux-android26`; it is not
in any CMake target and has never touched a device. `docs/AUDIO_ANDROID.md` is
the hand-off: the contract, the eight XAudio2 semantics that cost defects on
Windows, the exact integration steps, and the pitfall list. The Android host
has no XAudio2 shim at all yet, so M3's job is the device; the sink is ready.

**Docs moved.** `docs/audio.md` gained section 10 (this status) and the old
sections 10/11/12 are now 11/12/13, with every `§` reference in the document
rewritten to match. Any older note that cites "audio.md section 12" means the
second-session verification, which is now section 13.

## 2026-11-21 - M3 Task 2: the field is real, Windows names its writer, and a NULL slot alone does not fault

The previous Task-2 entry said "`[0xE4D064 + 0x30]` is never written by any guest
code". **Wrong, and the mistake was the probe, not the game.** The device sampler
logs a watched dword only when its *value changes*; a slot that is written with
the same value every time is invisible to it. Three phone-free probes settled the
question on the Windows host, which is the reference this build is compared
against.

**1. The writer of `0xE4D094` is `sub_00748CB0`, and the store is a 5x `movq`.**
`src/host/wprot_probe.c` (new, `KORRA_WPROT=<hex VA>`) makes the page holding the
dword read-only, catches the write fault in a vectored handler, single-steps the
store with `EFlags.TF`, and logs the lifted function that contains it. Every line
of a 100 s run reads `writer=00748CB0`. A `MEM32(x + 0x30) = ...` grep cannot see
it because the engine copies a 0x28-byte descriptor with five qword moves
(`0x00748D9D`-`0x00748DCB`): `[edi+0..0x20]` -> `[ctx+0x30..0x58]`. So
`ctx+0x30` is a **technique pointer** and `ctx+0x34` is its **valid** flag, written
as one 8-byte pair; `{0, 1}` is a legitimate engine state (`sub_00748A30`, which
0x748A9A calls with a NULL technique). The descriptor is built by
`sub_00748AB0(ctx, technique, arg2)` and the technique is never computed by the
renderer: it is one of the engine's technique-table globals - `0xCA39E8`,
`0xCA3A00`, `0xCA39F4`, `0xCA39FC`, `0xCA39DC`, `0xE1DAA0` - read at the call
sites in `recomp_0016.c` (0x54D8xx-0x54E7xx). The Windows values the sampler
reported are exactly that table: `0xE1DA54`, `0xE1DA6C` (= `0xE1DA54 + 0x18`,
the 0x18-stride array element `sub_005599D0` selects), `0x9DF3D8`, `0x9DF450`,
`0x9DF480`, `0xE1D860`.

**2. The table that fills them is per-frame, and the frame setup has a skip
gate.** `sub_005599D0` is called from one site only, `0x00541578` inside
`sub_00541510` (`recomp_0016.c:45150-45245`), and it writes the whole technique
table with literal pointers. `sub_00541510` reaches it only if this is false:

```c
eax = [0xE4D060];
if (eax && [eax+0x28] && (icall [[[eax+0x28]] + 0x88])() != 0) return;  /* skip */
```

`[eax+0x28]` is the device-state manager; slot 34 is `sub_0075A140`, a one-line
getter `return [this+0x6C]`, and `[mgr+0x6C]` is the manager's **"device is lost -
skip the frame"** flag: `sub_00759C90` (its slot 3) sets it when the stored status
equals `D3DERR_DEVICELOST (0x88760868)` or `D3DERR_DEVICENOTRESET (0x88760869)`,
and `sub_00759D10` (slot 4) is its SUCCEEDED companion. When the flag is up, the
technique table is not initialised, `sub_00552290`/`sub_00778550`/`sub_0055C7C0`/
`sub_00541590`/`sub_00553FA0` do not run either.

**3. The reference values, measured.** `src/host/gate_probe.c` (new, `KORRA_GATE=1`)
prints one line a second: `m=[0xE4D060] mgr=[m+0x28] flag=[mgr+0x6C]
status=[[mgr+0x24]] ctx30=[0xE4D094] tce=[0xCA3CE0]`. Clean 50 s run
(`korra_host-20261121-203324.log`, zero `[crash]`): `flag=1` only while the device
is coming up (seconds 3-5), then **`flag=0` forever**, `tce=0` (bit 24 recording
never sets, so the flush `0x74F740` runs **every frame**), and `ctx30` cycling
over `0x9DF3D8 / 0x00E1DA6C / 0x00E1DA54 / 0`. With `KORRA_ICALLFROM=74F740` the
same walk was dispatched 200+ times in 55 s to many different pass functions
(`0x401020`, `0x5501A0`, `0x54D2D0`, `0x5506A0`, `0x54DCC0`, `0x550AA0`), so the
flushed entry is the ordinary path, not a boot-time rarity; and `KORRA_WPROT`
counted 200+ technique installs in the same window, all non-zero. Android's
"never written, only `+0x34` toggles" is therefore consistent with the install
*running* while the technique it copies is NULL.

**4. A NULL slot is necessary but not sufficient.** `KORRA_GATE_FORCE=1` holds
`[mgr+0x6C] = 1`, and `KORRA_GATE_AT=<ms>` starts doing so after a normal load.
Shut from t=0: 50 s, `flag=1`, `ctx30=0`, no crash (the game never reaches a level
load, so nothing is queued). Shut at t=25 s after a normal 25 s: 75 s with
`flag=1`, `ctx30=0`, `tce=0` and the per-frame flush, **still no crash**. So the
device fault needs the entry `{fn=0x74F8F0, key=0x6B100000}` to be *in the queue*
at the same time the slot is NULL - on Windows the two never coincide.

**5. Layer decision, and why no fix is committed.** The lift, the runtime and the
generated tree are identical on both platforms: `android/app/src/main/cpp/CMakeLists.txt`
compiles `${KORRA_REPO_ROOT}/src/recomp/gen/*.c` with the same pinned pcrecomp
headers. So the divergence is in `src/host_android/` or in DXVK, and three
candidates remain, none distinguishable without the phone:

- the frame-setup skip flag `[mgr+0x6C]` is 1 on the device (then the table is
  never initialised);
- the technique install's own gate `sub_00756B00` (vtable slot 0x3C of the device
  object, measured target = `0x756B00`) returns 0 there: it bails if `[this+4]`
  is NULL or if any of four `sub_00756600` resolutions fail, and those use COM
  calls at `[ecx+0x14]` and `[ecx+0x94]` whose failures are tested with `js`, i.e.
  a negative HRESULT from a host answer;
- `sub_005599D0` runs and the six reset siblings
  (`0x556330/0x5563C2/0x556421/0x556480/0x55667B/0x55675C`) empty the table again
  before the flush.

A guest-side patch would be a guess about which of the three it is, so none was
made. `d_TestCooperativeLevel` in `src/host_android/d3d9_android.cpp` already
answers `D3D_OK`, so that particular shim is not the lier without measurement.

**6. New debug tooling, all off unless the environment asks for it.**
`src/host/wprot_probe.c` (`KORRA_WPROT=<hex VA>`: the exact lifted function that
stores to a dword - page-read-only, vectored handler, TrapFlag single-step);
`src/host/gate_probe.c` (`KORRA_GATE=1` census of `flag`/`status`/`ctx30`/`tce`,
`KORRA_GATE_FORCE=<n>` and `KORRA_GATE_AT=<ms>` to hold the gate shut, diagnostic
only); `KORRA_ICALLFROM=<hex VA>` in `src/host/watch_probe.c` (every indirect
transfer *dispatched from* a VA - the target-filter `KORRA_ICALL` cannot see a
vtable slot whose target lives on a heap object). The `[wlog]` line cap went from
400 to 3000, because 400 lines hid the interesting second.

**Verified:** the Windows build still boots, creates the device, draws and walks
the pass queue every frame (clean 50 s reference, zero `[crash]`), so the M2 gate
state is unchanged by this session; `tests/sync_codeberg_selftest.ps1` 9/9;
`selftest_host_shims` 60 passed / 0 failed when linked against the parallel
session's `wasapi_sink.c` (`scripts/run_selftest.ps1` itself is temporarily red
because their in-progress `xaudio2_stub.c` calls `korra_audio_sink_*`, which only
that untracked file defines - not this session's change).

**What only a device can prove**, in one run when the phone is back: is `flag`
(`[mgr+0x6C]`) 0 there, is `[0xCA39E8]` non-zero there, and does
`[0xE4D094]` change? The three answers pick one of the three candidates above and
name the fix. Until then Task 2 is **named, not fixed**, and this entry
supersedes the two Task-2 readings above ("never written" and "would have bailed
anyway" - the second is right about the *bail*, wrong about it happening before
the deref).

## 2026-11-21 - M4 verification session 2: the menu was always reachable; 2-6 are engine-side

**1. Shared host selftest - green; the earlier red was a script gap, not an
untracked file.** `scripts\run_selftest.ps1` compiled only
`tests\selftest_host_shims.c`, `src\host\xaudio2_stub.c`,
`src\host\steam_stub.c`. Since `65936bdb2137` the stub calls
`korra_audio_sink_start/live/note_underrun`, which live in the **committed**
`src\host\wasapi_sink.c`, so the link failed with `LNK2019` x3 on a clean tree.
Fix: add `src\host\wasapi_sink.c` to the compile line and `winmm.lib` to the
link line. Green twice: `60 passed, 0 failed`, exit 0, sink
`WASAPI shared: 2 ch, 48000 Hz, 32 bits, buffer 1056 frames (22.0 ms)`,
`XAudio2 stub stats: 2 created, 1 destroyed, 1 live, 2 buffers, 8192 frames`.

**2. The A-press black screen was the machine, not the M3 pass queue. Retracted.**
The previous session's title->A "static black screen, 57 ticks/s, present=0/s"
was a game that never owned the foreground: a D3D9 exclusive-fullscreen process
that is not foreground never calls `Present`. Re-run with the window forced
foreground 8 s after launch (`AttachThreadInput` + `ShowWindow` +
`SetForegroundWindow` + `SetFocus`), and the same `build\run_m4b.exe` reaches a
real menu:

* Intel UHD (desktop 1920x1200): window 1920x1080, `present==tick` 44-58/s,
  29-30/s on the title; title mean ~90/255, menu mean ~160/255; menu up from
  ~50 s and parked 52-105 s; OCR of `work\...\stop_64s.png` = `Story`,
  `Options`, `Exit` (`korra_host-20261121-232611.log`).
* NVIDIA RTX 5050 via per-app `GpuPreference=2`
  (`HKCU\Software\Microsoft\DirectX\UserGpuPreferences`): `nvidia-smi` lists the
  pid at every sample, the GPU Engine counter shows luid `...0x123C5` beside the
  Intel `...0x12023`, guest `eip` sits in `nvd3dum.dll`; 57-58 fps,
  `present=present`, menu at 50 s (`korra_host-20261121-233050.log`). So the
  per-app preference works, but a dGPU-preferred run that is **not** focused
  shows no window at all - the same no-Present condition, not a cross-adapter
  limit.
* Gate/device state stayed clean in both (`flag=0`, `ctx30` cycling
  `0x009DF3D8`/`0x00E1DA54`/`0`, `tce=0`, no `[crash]`), matching the Windows
  oracle: there is no pass-queue fault to bisect here, and `65936bdb2137` was not
  bisected because the regression did not reproduce.

**3. Acceptance tests, measured.** `KORRA_AUDIO_DUMP` + `KORRA_AUDIO_LOG=1`:

* Test 2 (menu music `bgm_TitleMenu`) - **FAIL**. Menu parked 52-105 s on Intel
  (`work\m4b_stop.wav`, 137.0 s): 0-30 s is movie audio (peak -11.95 / -10.05 /
  -9.77 dBFS, RMS -28.99 / -27.67 / -27.87 per 10 s window), **30-137 s is
  digital silence** (peak -240.00 dBFS, RMS -999). dGPU run
  (`work\m4b_dgpu2.wav`, 71.0 s): 0-30 s peak -9.77 dBFS, **30-71 s all-zero**.
  Sink `quiet` 40.1 s -> 89.1 s across the parked menu, `underruns` 1-3 total,
  `OnVoiceError` 0.
* Tests 3-6 (longest cue, stinger over a cue, pause/resume, voice) -
  **BLOCKED**, not failed: each needs a posted cue or the event-post path, and
  nothing is posted.
* Root cause is narrow: in the entire menu session there are **4**
  `CreateSourceVoice` and **3** `SubmitSourceBuffer` calls - voice #1 is the
  Hw/Wwise voice (0.2 s, mask 0x3, started from `0x0073EC80`) and the rest are
  the three CRI movie devices (2.3 / 12.5 / 22.0 s). After 22.7 s no voice is
  created, fed, queried or destroyed, and Wwise's voice is never destroyed. The
  banks are **not** the problem: `KORRA_TRACE_FILE=1` shows the guest opening
  `GameData\sound\bgm\BGM.bnk`, `GameData\sound\WwiseInfo.wai`,
  `GameData\sound\Init.bnk` and `GameData\sound\SeFootstep.bin` from disk, all
  `attrs=present -> handle=0x...`. The missing piece is the event post itself.

**4. Input quirk.** A single `KORRA_PAD_TAP` at 46.0 s left the title unchanged
(frames at 44 s and 50 s identical, mean ~90), while taps at 44.0 + 45.5 s
reached the menu. Repeat the A tap at ~0.5-1.5 s intervals; `KORRA_PAD_AUTO=1`
(closest thing to a mashed pad) is the reliable version. Injected keyboard input
remains invisible to DirectInput.

**5. Not verified.** No event was posted by the engine on any screen reached, so
tests 3-6 have no measurement at all - nothing here says whether the mixer would
handle a 5.1 stream, a stinger, a state mute or a 32 kHz voice correctly. The
dGPU session lasted 71 s and parked on the menu 50-56 s only. All numbers are
from this hybrid laptop's panel on the Intel adapter (dGPU via per-app
preference), none from the phone. The harness (`work\m4b_run.ps1`) is gitignored
and not committed.

**Next lead (unchanged):** the Wwise event-post entry point - find the code that
posts an event (the `WwiseInfo.wai` IDs are compile-time constants in the
guest, so a watch on one of them or on the queue object is a better handle than
the `PostEventWork` RTTI name), then the same three assertions as before.

## 2026-11-22 - Task 2 root cause: two D3D9 shim argument contracts, both host-side

Device: Galaxy S24 (Exynos 2400, Xclipse 940, stock Vulkan 1.3.304), DXVK-native,
build 656a371. Probes: `debug.korra.gate=1`
(`src/host_android/gate_probe_android.c`, three lines every 250 ms) plus the
`[hr]` trampoline in `spine_android.c`; the Windows reference ran the same
census from `src/host/gate_probe.c` with `KORRA_GATE=1`.

**The field.** On Windows `[shader+0x30]+4` holds a live D3D9 interface pointer
(0x40508780 / 0x40E174E0). On the device it was 0 in every run. That pointer is
what `sub_00756600`'s cached branch pushes into `SetRenderTarget(0, ...)`.

**Defect 1 - ppSurface / pSharedHandle swapped.** `d_CreateRenderTarget` and
`d_CreateDepthStencilSurface` wrote the guest token through `A32(7)` (the
engine's NULL pSharedHandle slot) and passed `A32(6)` as pSharedHandle. Every
call returned S_OK, so nothing logged a failure, but the engine's surface
pointer stayed 0. With `ppSurface = A32(6)`:
`CreateRenderTarget 1280x720 fmt=21 -> 0x00000000 out=200C41A4 shared=00000000
tok=3463FB20`, and the next census line reads
`[gate] obj 3a00 ... o34=3463FB20`.

**Defect 2 - interface tokens passed as pointers.** StretchRect, ColorFill,
UpdateSurface, UpdateTexture, GetRenderTargetData, GetFrontBufferData and
swapchain GetFrontBufferData cast guest tokens straight to interface pointers;
DXVK then dereferenced guest memory as a COM object (addr=0x24 inside
`D3D9DeviceEx::StretchRect`, caller `sub_00756580`). They now resolve through
`obj_of()` like the other setters always did.

**Measured chain before the fixes.** `[shader+0x30]+4 == 0` ->
`SetRenderTarget(0, 0)` -> DXVK returns D3DERR_INVALIDCALL 0x8876086C exactly as
real d3d9 does -> the 0x756B00 gate (`js` after 00756600) bails -> sub_00748CB0
skips the 5x movq -> `[0xE4D094]` stays 0 -> the pass flush dereferences NULL+4
in sub_007492B0. 117 such failures per 40 s run; the `[hr]` census found no other
negative HRESULT in the whole session (3 false positives from `SteamUser:2`
returning a pointer with the sign bit set).

**After both fixes (run 12).** `[hr]` is down to those 3 false positives, every
SetRenderTarget failure is gone, `[0xE4D094]` reaches 0xE1DA54 for the first
time, and `edx` at the crash holds 0xE1DA54. The crash is still sub_007492B0 at
the pass flush, and the last gate sample before it reads
`ctx30=00000000` again: something between the install and the flush clears the
slot (candidates: the six reset siblings in sub_00556330, or the clear path
sub_00749020). Not yet identified.

**Not proven.** The >2 min stability run, a reach past the flush, the title
screen capture, first-area fps, SAF install. Nothing was re-tested on Windows
after these fixes.

Committed: 656a371 (shims), f170e9a (probes).

## 2026-11-22 - M4 audio acceptance with gameplay: the cutscene is a CRI movie, Wwise is still never fed

Scope: `docs/audio.md` 9.4 tests 2-7, re-run with real gameplay on
`build\run_m4c.exe` (SHA256 `7C286C2251CE8968E3E9F6D02129AA56DD03049BBC7D0E7AFC299D5131528DC7`).
No host behaviour was changed; one logging line was added to
`src/host/xaudio2_stub.c` (`GetState` polls at verbose 2, `751f85b10dd7`).

**The route, which is why tests 3-6 were "BLOCKED".** Every run ever made pressed
A on the STORY submenu's first item, which is `Continue` - a save exists from the
first play session, so the boot's auto-save notice is not a warning, it is a
fact. Continue loads a checkpoint at the tutorial and never plays the intro.
New Game is the second item: `DOWN` (0x0002), `A`, then the confirm dialog
("Starting a New Game will reset your game progress, results, and Collection
data. Are you sure?", YES/NO) defaults to **NO**, so `LEFT` (0x0004) then `A`,
then the difficulty screen, then `A`. Recorded in `docs/audio.md` 10.6.

**Measured, two runs (g4 181 s, g5 408 s; dumps `work\g4_newgame.wav`,
`work\g5_area.wav`, 16-bit 48 kHz stereo, half-second blocks with a -70 dBFS
gate):**

- boot movies 0-24 s, peak -9.77 dBFS;
- menu 25-78.5 s: **exact silence** (test 2 still FAIL);
- intro cutscene 79-103.5 s (g4) / 77.5-103.5 s (g5): **24.5 / 26.0 s continuous,
  peak -7.09 dBFS, RMS ~-31 dBFS** - the first non-silent gameplay-phase audio in
  the port, and it comes from **CRI's 6 ch float movie device** through the
  engine's own 6x2 matrix (CreateSourceVoice 6 ch at 80.19 s, first submit
  80.72 s, destroyed 106.7 s). The 5.1 path is therefore measured, not argued;
- everything after 104 s: exact silence for 300 s, no audio call at all. A device
  is created at 109.87 s and destroyed 44 ms later without a source voice.

**Decisive for 10.5:** the cutscene music is not `bgm_cs41_skip`. The 94.88 s
stream `GameData\sound\stream\1012706428.wem` is never opened; the cutscene is a
movie. And Wwise voice #1 (2 ch float, created 0.2 s, started 3.2 s from
`0x0073E700`) receives **no `SubmitSourceBuffer`, no `Start`, and now with the new
logging, no `GetState` at all** across menus (35-78 s), the cutscene (80-106 s)
and 300 s after it. Wwise opens two 30 s streamed `.wem` files when the title
appears (`128216764`, `678887150`), so events do run - the break is between
Wwise's renderer and the voice the host provides.

**Underruns:** 1 at boot, 2 at 81 s, **89 by 104 s** and constant after - 63
`ran dry` lines between 83.8 and 106.5 s. The movie feed submits 800-frame
buffers (16.7 ms) from a GetState loop running at the guest's ~20 fps against a
1056-frame device buffer.

**New Game does not reach the first area in this build.** After the cutscene the
frame is pure black (mean 0.0, pixel-identical) at 134/160/185 s and at
131/312/412 s; 17 A presses over 280 s change nothing; the guest's audio thread
still calls `CommitChanges` ~50/s, so the process is alive; the scene probe stays
at `flow=0x9 inst=0x1 step=0xFFFFFFFF mode=0` with the pool climbing 63 -> 91 and
stopping. The Resume path does reach a tutorial area (dark screen, `Jump` hint),
which is what the perf session measured as "first area".

**Open, not mine to fix here:** the current build + `KORRA_PAD_AUTO` freezes the
guest at t=54-55 s (tick 0/s, natcalls 0, all 24 guest threads parked in ntdll
waits, sink thread alive), while the 03dfc01 copy `build\run_perf.exe` survives
the same input to 300 s (and, with START every 9 s, backs out of gameplay to the
main menu). Not an ExitProcess (`exit_probe` printed nothing) and no fault line.

Committed: 751f85b10dd7 (GetState logging), bba3704 (`docs/audio.md` 10.5, 10.5.1,
10.6).

## 2026-11-22 - M3 Task 2 root cause: the technique cache was never filled (D3DX cube gap)

Device: Galaxy S24 (Exynos 2400, Xclipse 940, stock Vulkan 1.3.304), DXVK-native.
Probes: `debug.korra.gate=1` (`rec`/`pkg` lines, `gate_probe_android.c`),
`debug.korra.watchlog`, `debug.korra.watch`, `debug.korra.icallfrom`, plus the
same `rec`/`pkg` lines in `src/host/gate_probe.c` for the Windows reference.

**The previous hypothesis was wrong, and the crash's own registers say so.** The
run dies at `sub_007492B0` with `ecx=00E4D064`, `eax=00000000`, `edx=00E1DA54`,
`edi=00000500` and `[crash] addr = guest base + 4`. Decoding the fault:
`0x7492C0: mov edx,[eax+4]` with `eax = [ebp+0x10]`, i.e. the **technique
argument** is NULL - nothing reads `[0xE4D094]` on this path. Resolving the
crash's own `lr` against the unstripped `libkorra.so`
(`llvm-addr2line -e ... 0x23403d0`) names the caller exactly:
`sub_00550280`, recomp_0016.c:104336, call site `0x00550321`.

**The guard that produces the NULL.**

```c
/* 0x5502FF, sub_00550280 */
if ([0xE46E90] != 0 && [0xDC0F74] != 0) {
    eax = ([0xE4CCF8] > 0xB) ? [0xE4CCF4] + 0xDC : 0;   /* entry 11 of 15 */
    sub_007492B0(ctx, 3, eax, 0xE4B5CC);                /* eax = 0 -> [NULL+4] */
}
```

`[0xE4CCF4..0xE4CCFC]` is a three-dword cache `{table, count, flag}` filled once
per session by `sub_0073C4A0(0xDC0988)`, the boot loader at `0x53F94A` that
resolves two resources out of the package built by `sub_006AACB0` -
`pre_texture.wta` (`0x95E738`) and `pre_texture.wtp` (`0x95E728`). Each is a
15-entry `0x14`-byte table; entry 11 is what the recorder binds, hence `> 0xB`.

**Measured, both hosts, same census.**

| | device (before) | Windows |
|---|---|---|
| `[gate] rec` | `ccf0=0 ccf4=0 ccf8=0 ccfc=0` all session | `ccf0=1 ccf4=0B3F5EE4 ccf8=0F ccfc=1` |
| `[gate] pkg` | `d984=2 d988=26173400 head0=500674C9 ...` | `d984=2 d988=115FC320 head0=500674C9 ...` |

The package bytes are identical on both (`head0/4/8/10` match dword for dword),
so the two name lookups cannot be the difference: the **fill** is. Watching
`0xDA7340` (the loader's registry lock, `+08` = `0xDA7348`, entry 0) showed
`sub_007516C0` creating an entry, then `sub_0074CDE0` being entered once per
record. `debug.korra.icallfrom=754000` then named the single call under it:
`[icall] from=00754000 target=00759B20`.

**The gap.** `sub_00759B20` is the per-record bind; when `[desc+0xC] == 5`
(D3DRTYPE_CUBETEXTURE) it calls `0x8FD2E8` and otherwise `0x8FD2E4`. Resolving
the IAT: `D3DXCreateCubeTextureFromFileInMemoryEx` and
`D3DXCreateTextureFromFileInMemoryEx`. The Android shim answered the cube one
with "not implemented" + `D3DERR_INVALIDCALL`:

```
d3dx: D3DXCreateCubeTextureFromFileInMemoryEx not implemented   (1 per boot)
```

One failed record is enough: `sub_00751A90` returns 0, `sub_0074CDE0`,
`sub_00754000` and `sub_007516C0` all return 0 with it, the registry entry is
never written, and the cache stays `{0,0,0}`. The `[hr]` trampoline could not
see it: that one only wraps synthetic COM slots (names with `:`), not plain
d3dx imports.

**Fix (`d0b2cda0ec69`).** Implement it: `dds_upload_cube` in
`src/host_android/d3d9_android.cpp` walks the same header as `dds_upload`, checks
`DDSCAPS2_CUBEMAP` (dword 27), creates a cube in the requested pool (through a
SYSTEMMEM staging copy and `UpdateTexture` when the pool cannot be locked), and
uploads the six faces, skipping each face's mip chain with `dds_mip_bytes`. The
token goes through `ppCubeTexture` = `A32(13)` (this signature has no height),
never through EAX.

**Evidence after the fix.** Same run command, stock driver:

- five `d3dx: dds cube ...` uploads (128x128 DXT1, 16x16 DXT5, ...);
- `[gate] rec e46e90=... ccf0=00000001 ccf4=20043E04 ccf8=0000000F ccfc=00000001`
  - the Windows shape, `count=15`;
- **160 s, 8664 frames (54 fps), zero `[crash]`, one brief `flag=1` sample**
  (device-lost during a swapchain change), no pause/LMK event: the >2-minute
  question is closed for an idle boot;
- touch -> XInput works: a 1-1.2 s press of the virtual A zone advances
  `PRESS ANY BUTTON` -> auto-save notice -> main menu -> STORY -> New Game ->
  difficulty; the intro cutscene renders with its Skip prompt. A 50 ms
  `input tap` does **not** work: the engine polls XInput far less often than it
  draws (44 polls in 70 s), so the button must be held for about a second.

**Not verified / open.** The first playable area (rung f): after the cutscene
the process dies twice, once `reason=5 APP CRASH(NATIVE) status=6` (SIGABRT,
1.2 GB RSS, no guest `[crash]` line) and once `reason=3 LOW_MEMORY`, and a
separate "Failed to save data." dialog blocks New Game on some boots. The save
path is a real host gap - `k_CreateFileA` ignored `dwCreationDisposition`, so
`open()` never passed `O_CREAT` and no new save could be created; that contract
is now implemented (`ka_file_open_ex`, `SetEndOfFile`, `FlushFileBuffers`,
`dda67c92be2d`..`work`), but no save file appeared and **no file open or mkdir
failed**, so the engine bails before it touches the filesystem; the next session
should trace the save routine's first step (the `Korra` directory at
`GameData/Korra` exists and is empty). The SAF install rung (d) is untouched.
Nothing was re-tested on Windows after the cube change (it was already correct
there).


## 2026-11-22 - the save lands where the engine asked: a host path is absolute (save-path session)

Device: Galaxy S24 (Exynos 2400, Xclipse 940, stock Vulkan), DXVK-native. Base
HEAD `56b20b22f18f`; commits `baad46bc89ea` (probe), `a8c28fbcf962` (fix). Probes:
`KORRA_TRACE_SAVE=1` (`src/host/save_probe.c`, chained onto
`native32_post_hook`) and `debug.korra.trace_save=1`
(`src/host_android/save_probe_android.c`); both print the call, its arguments,
the resolved host path and the answer in one line, so the two logs diff.

**The save path, derived from the original exe (no analysis-exe layout).**
`0x76CB50(buf)` asks `SHGetSpecialFolderPathA(NULL, buf, CSIDL_PERSONAL=5,
FALSE)`, then builds `"%s/%s/"` (`0x95C214`) with `"Korra"` (`0x943FA4`) and
appends `"%sSystemData.dat"` / `"%sGameData.dat"` / `"%sSlotData_%d.dat"`
(`0x96DC30` / `0x96DC0C` / `0x96DC1C`) according to `[0xE4F474]`
(-2 / -1 / slot). The existence test `0x76CC40` appends `"%s*.*"`
(`0x96DC44`) and goes through the CRT's `_findfirst` (`0x77D512 ->
FindFirstFileExA`). The writer is the state machine `0x76D0E0` (state word
`[0xE4F470]`): state 0 = build the directory (`0x56BE90`), test it, `mkdir`,
build the file, `CreateFileA(GENERIC_WRITE, CREATE_ALWAYS)`, `WriteFile`.

**Same state, two hosts (cold: no `Korra` directory).** Windows oracle
(`work\m4b_run.ps1`, pad timeline 44 s A / 47 A / 53 A / 61.8 A / 67.8 DOWN /
69.8 A / 73.2 LEFT / 75.2 A / 80.2 A, `<Documents>\Korra` renamed away):

```
[save] SHGetSpecialFolderPathA hwnd=0 csidl=5 create=0 -> ret=1 path="<Documents>"
[save] CreateFileA path="<Documents>/Korra/SystemData.dat" access=0x80000000 share=0x00000000 disp=3 flags=0x00000080 -> 0xFFFFFFFF last=3
[save] CreateFileA path="<Documents>/Korra/GameData.dat"  access=0x80000000 share=0x00000000 disp=3 flags=0x00000080 -> 0xFFFFFFFF last=3
[save] CreateDirectoryA path="<Documents>/Korra/" -> ret=1 last=3
[save] CreateFileA path="<Documents>/Korra/GameData.dat" access=0x40000000 share=0x00000000 disp=2 flags=0x00000080 -> 0x00000BC4 last=0
[save] WriteFile h=0x00000BC4 n=7856 -> ret=1 put=7856 last=0
```

Device, probe-only build (`devsave2`), same cold state:

```
[save] SHGetSpecialFolderPathA hwnd=0 csidl=5 create=0 -> ret=1 path="/data/user/0/org.korra.recomp/files/game"
[save] FindFirstFileExA pat="/data/user/0/org.korra.recomp/files/game/Korra/*.*" -> 0xFFFFFFFF last=2 (host=".../game/GameData//Korra")
[save] CreateDirectoryA path="/data/user/0/org.korra.recomp/files/game/Korra/" -> 0x00000001 (host=".../game/GameData//Korra/")
[save] CreateFileA path="/data/user/0/org.korra.recomp/files/game/Korra/GameData.dat" access=0x40000000 disp=2 -> 0x01000002 (host=".../game/GameData//Korra/GameData.dat")
[save] WriteFile h=0x01000002 n=7856 -> ret=1 count=7856
```

**No call answered differently.** The divergence is the translation: the guest
string is the same expression on both hosts, and Windows opens exactly it,
while `ka_fs_resolve` dropped the host-root prefix and re-joined the guest cwd
(`GameData`) - the `(host=...)` field above is the evidence. So the save device
created and wrote `<root>/GameData/Korra/`, inside the rofs tree; the next
boot's `[Hw::rofs::fst]` mount then enumerated that empty directory, failed to
build its file-size table and raised the boot notice
(`[msgbox] ... [Hw::rofs::fst] ...`, one per boot at `C:\GameData\Korra\*`).
Both reported symptoms - the save "not appearing" and the boot notice - are
this one mis-resolution.

**Fix (`a8c28fbcf962`, `src/host_android/fs_android.c`).** A path that already starts
with the host root is host-absolute: strip repeated copies of the root (a leaked
path can arrive twice) and put exactly one back; never re-home it under the
guest cwd. Drive paths (`C:\...`) and cwd-relative paths are unchanged.

**Proof ladder.**
1. Divergence named above: `CreateDirectoryA`/`CreateFileA` on
   `<root>/Korra/...` resolving to `<root>/GameData//Korra/...` on the device,
   to the exact string on Windows.
2. Cold device run after the fix (`devsave3`, 7,334 frames): `CreateDirectoryA
   host=".../game/Korra/" -> 1`, `CreateFileA host=".../game/Korra/GameData.dat"
   -> 0x01000002`, `WriteFile 7856`; **zero `[msgbox]` in the whole run**.
3. File list, pulled with `run-as`:
   `files/game/Korra/GameData.dat`, **7856 bytes**, 2026-11-22 12:50; and
   `files/game/GameData/` no longer contains a `Korra` directory.
4. Second boot (`devboot2`, same APK, save present): **0 msgbox**, **0**
   `C:\GameData\Korra` enumerations (Windows never enumerates it either), and
   the boot read takes the Windows branch - `SystemData.dat` missing
   (`last=2`), `GameData.dat` opens (`0x01000002`, `last=0`). The informational
   auto-save notice ("This game features an Auto-Save function...") still shows,
   as it does on Windows; the rofs error notice is gone.
5. New Game on the device, walked with screenshot+OCR (the touch zone offers no
   D-pad, so the left stick does DOWN/LEFT): title -> auto-save notice -> main
   menu -> Story -> stick-down to `New Game` ("Start a new game.") -> A ->
   difficulty ("...challenges", "Extreme") -> A -> **intro cutscene plays**
   (OCR "CREATED BY / BAYAN KONIETZKO / MICHAEL DANTE DIMARTINO"), 9,987
   frames, no "Failed to save data." anywhere.

**Correction to the hand-off.** The save was not bailing before the filesystem.
The probe-only build already wrote 7856 bytes - into `GameData/Korra`. Two
things hid it: the `[import]` trace is capped at 12,000 lines (`spine_android.c`,
reached at log line 14,823), so the write-side calls after that point cannot
appear in a device log at all; and the expected directory was checked as
`<root>/Korra` while the host had re-homed it. The r11 `*.*` enumeration at
4,416 frames was the save step running at the end of a run that was cut short.

**Windows after the change (shared-probe rule).** `scripts\run_selftest.ps1`
60/60 (the probe files are `src/host/`). The smoke run (`build\run_save.exe`,
95 s, same pad timeline) shows both boot reads succeeding with the save present
(`SystemData.dat` -> `0x0000071C`, `GameData.dat` -> `0x000009A8`, `last=0`), no
new save write (nothing to autosave), and no `[crash]`; the window is black by
86 s with no GPU work - the known post-cutscene state (flow=0x9), unchanged and
not chased. The Windows save directory is untouched by the fix
(`fs_android.c` is Android-only).

**Not verified.** The `SystemData.dat` write (only `GameData.dat` is written in
this flow on both hosts, so the cold-boot `last` differs from Windows only by
`2` vs `3` - the same branch); the save *slot* files (`SlotData_%d.dat`) and the
erase/delete path; a save/load round-trip through Continue (the boot read of the
written `GameData.dat` succeeds, but nothing was resumed from it); the SAF
install rung; and whether the auto-save writes anything new after gameplay
progress (this session only exercised the cold-boot autosave and New Game).
Blocker #2 (post-cutscene SIGABRT / LOW_MEMORY) untouched, recorded only.

## 2026-11-22 - the post-cutscene death was a thread cap, not memory: 64 guest stacks of 4 MiB filled the 256 MiB region

Device: Galaxy S24 (Exynos 2400, Xclipse 940, stock Vulkan), DXVK-native. Base
HEAD `503f93653f85`; commits `6a2bd5877f93` (probe: 1 Hz sampler, crash-buffer pull, route
replay) and `c4ba953bbbec` (fix).

**The death, named.** Two tombstones - 12:58 (uptime 166 s) and 13:59 (uptime
329 s) - end in the same five frames:

```
#00 libc.so            abort+160
#01 libkorra.so        ka_thread_attach+292
#02 libkorra.so        thread_trampoline
#03 libc.so            __pthread_start+184
#04 libc.so            __start_thread+68
```

and the guest log's last lines before the abort are

```
thread: create va=0077CAEE param=3466A370 tok=4159 suspended=1
thread: resume tok=4159 prev=1
guest stack space exhausted
FATAL: cannot create guest stack/TIB (CreateThread)
```

`ka_thread_attach` asked for a 4 MiB guest stack, the 256 MiB stack region
(`0xC0000000-0xD0000000`) was full, and the old code called `abort()`. That is a
host death, which is why there was never a guest `[crash]` line: that handler
covers SIGSEGV/SIGBUS/SIGILL/SIGFPE only, by design.

**Trajectory (1 Hz sampler, capture run 13:54-13:59, 19,473 shell frames /
9,046 presents).** Two growth phases - boot, and the level load:

| t | guest threads | stack-hi | VmRSS | presents |
|---|---|---|---|---|
| 0 s | 0 | 0xC0000000 | 124 MB | 0 |
| 11 s | 26 | 0xC6C00000 | 229 MB | 378 |
| 33 s | 50 | 0xCCC00000 | 137 MB | 1,129 |
| 44 s | 55 | 0xCE000000 | 186 MB | 1,547 |
| 288 s | 55 | 0xCE000000 | 109 MB | ~8,200 |
| 298 s | 63 | 0xD0000000 (full) | 121 MB | 8,536 |
| 327 s | 64 | 0xD0000000 | 118 MB | 9,046 |

Death at t=329 s: the 64th `CreateThread` was the one that could not get a
stack. `thread: exit` count over the whole run: 0 - the engine's workers are
pool threads that never return, so every thread cost 4 MiB for the life of the
process. The same census in the pre-fix logs (`thread: create` 64 =
`thread: attach` 64 minus the main thread's one) is what named the site.

**The 1.2 GB was not the cause.** `dumpsys meminfo` on the same run:
TOTAL PSS 1,286,134 kB / RSS 1,193,259 kB, of which **Graphics 1,080,959 kB
(1.03 GB, dmabuf)**, Native Heap 14,736 kB, TOTAL SWAP PSS 151,227 kB. The
in-app VmRSS never passed 122 MB (VmHWM 249 MB). The `reason=3 LOW_MEMORY`
record (02:54:33, `rss=258MB`, `importance=400`) is a cached background process
lmkd reclaimed, not this death. `android:largeHeap` would not have moved either
number: the abort was a fixed capacity wall, not an allocation failure.

**Fix (`c4ba953bbbec`, `memory_android.c` + `spine_android.c` + `kernel32_android.c`).**
Decision-table row: "abort in host malloc/allocator -> memory_android.c
budget/limits; the sampler names the growth site" - the sampler named the site
as the per-thread stack, and the growth as thread count, not bytes.

- A guest stack is 1 MiB: LoK.exe's own `SizeOfStackReserve` (PE optional
  header `0x100000`). `CreateThread(dwStackSize=0)` means exactly that reserve
  on Windows, so 4 MiB was four times the guest's contract and capped the
  process at 64 concurrent guest threads.
- `ka_thread_detach` (called when the entry point returns and from
  `ExitThread`) returns the stack and the TIB; `ka_stack_new` reuses released
  blocks LIFO. The region now has to hold the threads that are alive, not every
  thread ever created.
- The thread table is 256 records (16 KiB) and `CloseHandle` retires a
  closed+done record, as Windows drops the id once the thread ends and the last
  handle closes. A full table is now a guest-visible `CreateThread` failure
  (`INVALID_HANDLE` + `ERROR_NOT_ENOUGH_MEMORY`), never `abort()`.

**Proof.** Same New Game route, OCR-driven so the state is known at every step
(`scripts/android_route_drive.ps1`): attract 40 s -> auto-save notice 43 s ->
controller notice 51 s -> main menu 84 s -> Story submenu with New Game
highlighted 89 s -> difficulty screen (Extreme) 114 s -> intro cutscene
118-203 s -> **tutorial area with its control HUD at 206 s** (OCR: "Light
Attack | Heavy Attack | Jump"). Run length 600 s, still alive; final sampler
line at t=599 s:

```
[mem] frames=37248 presents=10271 rss=87940kB hwm=309192kB heap=35740000
      alloc=108228/871475K max=161611776 free=9/14K
      stack=C1D00000/D0000000 sfree=8/8192K thr=72/72 peak=72
      flow=00000009 inst=00000001 step=FFFFFFFF mode=00000000
```

- 72 live guest threads and peak 72, past the old 64-thread wall; the stack
  region is at 29 MiB of its 256 MiB, and 8 stacks sit on the free list
  (recycled from threads that ended through `ExitThread`, which logs no
  `thread: exit` line).
- Zero `guest stack space exhausted`, zero `thread: cannot create`, zero
  `thread: table full`; `adb logcat -b crash` empty; the process ended only at
  600 s by the session's own `am force-stop` -> `reason=10 (USER REQUESTED)`,
  and no `reason=5`/`reason=3` record exists after 13:59.
- 271 s past the t=329 s point that killed the pre-fix builds, with the
  transcript still advancing (37,248 shell frames, 10,271 presents; presents
  slow to ~10/s in the tutorial area, which is a frame-rate question, not a
  survival one).
- The save path still writes: the New Game start opened `Korra/SystemData.dat`
  (missing, `last=2`, the Windows branch), read `GameData.dat`, then
  `CreateFileA(GENERIC_WRITE, CREATE_ALWAYS)` + `WriteFile 7856`; the file on
  device is 7,856 B at 14:30, zero `[msgbox]`, zero "Failed to save".

**Route state for the hand-off.** On the device the New Game route now reaches
the first playable area: the Story submenu had **New Game already highlighted**
("Start a new game."), the difficulty screen shows **Extreme** highlighted and
A confirms, the intro cutscene plays (Skip prompt), and the tutorial area
follows with the control HUD. The scene words during and after it are
`flow=0x9 inst=0x1 step=0xFFFFFFFF mode=0` - the same shape the Windows oracle
reports after the cutscene. That is worth reading twice: on Windows `flow=0x9`
was recorded as "stuck after the cutscene" because the frame was pure black,
but the device reaches the tutorial area in exactly that state, so `flow=0x9`
is a legitimately reachable state and the Windows black frame is a render
divergence, not a failed transition. Not re-run on Windows this session (no
shared code changed).

The earlier fixed-timeline run measured the other half of the route: its stick
deflection never landed, so the Story submenu selected **Continue** and the
game *read* `Korra/GameData.dat` (7,856 B) instead of writing it - the same
checkpoint route the M4 session measured. Both routes now survive (that run
died only at 64 threads).

**Not verified.** Whether the tutorial area is interactive past its HUD (no
input was sent after t=206 s; the >=5-minute fps/thermal campaign is the next
session), the Windows re-run of the New Game route, the `reason=3` conditions
(cached-process LMK, not reproduced), lifecycle pause/resume (no pause or LMK
event occurred in either run), and the still-open save fields from the previous
entry (`SlotData_%d.dat`, `SystemData.dat` write, erase path, Continue
round-trip, SAF).


## 2026-11-22 - first-area frame time, decomposed: 92% of a 124 ms frame is the render thread waiting on its serialized workers, not the GPU

Device: Galaxy S24 (SM-S921B, Exynos 2400 / `erd9945`, Xclipse 940, stock
Vulkan 1.3.304, Android 17 / API 37). Base HEAD `4c80cec`; commits `3d9825d`
(probe + cap + ADPF) and `8c85f57` (route driver). No lifted code, no pcrecomp
or DXVK patch: everything here is `src/host_android` and the shell.

The question this session owned: the tutorial area presents ~10/s, the M3 gate
wants 30. The instrument answers *where* the 100 ms sits, and the answer is not
the GPU, not the swap chain, and not the frame cap.

**The `[mem]` `frames=` column is not a frame count.** It is
`ka_shell_frames()`: the shell's 16 ms `ALooper_pollOnce` tick
(`android/app/src/main/cpp/jni_bridge.cpp` -> `korra_guest_frame`). It reads
~60/s from boot to shutdown whatever the game does, so "frames/s high while
presents sag" from the old line was reading the shell, not the guest. Presents
(`g_presents`, counted inside `IDirect3DDevice9::Present`) is the only rate in
that line. The new probe prints the guest frame itself.

### The instrument (`3d9825d`)

- `src/host_android/frame_probe_android.c`, `debug.korra.perf=1`, one line a
  second from the Present path (no thread of its own):
  `dt=avg/min/max`, time inside `Present`, time in the engine's 1 ms
  `WaitForSingleObject` polls, time blocked re-acquiring the machine lock, time
  blocked on guest critical sections, the residual, bridged calls/frame, the
  limiter's own fields and process CPU. `debug.korra.perf.frames=1` adds one
  line per frame slower than `debug.korra.perf.slow` (ms).
- It writes three channels: fd 2 (the shell's pump), its own `files/perf.txt`
  (O_APPEND) and logcat tag `KorraPerf`. That is not belt-and-braces: one run
  lost every line after `t=266s` while the game kept rendering (below).
- `src/host_android/fps_cap_android.c` - the port's cap on the engine's own
  limiter, `debug.korra.fps` (default 30, `0` = engine's 30/60 mode), the
  `src/host/fps_cap.c` pattern: `[frameMgr+0x58] = round(3000/fps)` = 100 for
  30 fps, re-applied from the Present path after `FUN_0074c580` recomputes it.
- `perf_android.c`: the ADPF session had never been created - `createSession`
  was passed an empty thread list, hence `session=no` in every log since the
  scaffold. It now passes the calling thread, logs the rate it hints, takes its
  deadline from `debug.korra.fps`, and reports the guest frame's work
  (`korra_perf_guest_frame`), not the shell poll's microseconds.

### Where the frame goes (tutorial area, two runs agreeing within 2 ms)

| bucket | dec1 (130 s play) | camp2 (330 s play) | share |
|---|---|---|---|
| frame `dt` (present-to-present) | 127.0 ms | 123.9 ms | 100 % |
| inside `Present` (DXVK + WSI) | 1.9 ms | 1.8 ms | **1.5 %** |
| engine `WaitForSingleObject` polls | 116.2 ms | 113.3 ms | **91 %** |
| - of which 1 ms `nanosleep` (42 polls/frame, max arg 1 ms) | 54.3 ms | 52.3 ms | 42 % |
| - of which blocked re-entering the machine lock | 62.3 ms | 61.3 ms | **49 %** |
| residual lifted work on the render thread | 8.6 ms | 8.4 ms | 7 % |
| bridged calls / frame (all guest threads) | 7,493 | 6,843 | - |
| `InterlockedCompareExchange` / frame (engine spinlock) | 1,050 | 9,398 | - |
| process CPU | 99 % of 1 core | 99 % of 1 core | - |
| guest threads | 72 | 56 | - |

Matched row of the hand-off table: **"presents cheap, guest loop slow ->
hot shim fixable; if the ceiling is structural (the single machine lock),
measure it"**. Measured: the render thread spends 116 of 124 ms waiting on the
engine's own worker threads, and the whole process burns exactly one core's
worth of CPU (99 %) in a 124 ms frame. That is the single machine lock doing
what it says: every guest thread's lifted work is serialized, so the frame is
the *sum* of all of it, and the port's throughput is total serialized CPU per
frame - not the GPU, not vsync, not the cap.

For scale, the Windows oracle on the same content (`docs/FINDINGS.md`
2026-11-20): 17.5-25 ms frames, 40-58 fps, ~2,200 bridged calls/frame, 24 guest
threads, main thread waiting 16-21 ms of a 25 ms frame on 34
`WaitForSingleObject` calls. Same shape, 5x the wall time: the device does ~3x
the per-frame CPU on a core that is slower, with 2-3x the threads to serialize.
The call counts are not directly comparable: the Windows probe counted the main
thread's own bridged calls, this one counts every guest thread's in the frame
interval (`ka_import_calls()` is process-wide).

**The 30 fps cap is active and costs nothing here.** `int=100` in every sample,
`[fps] limiter interval 50 -> 100 (cap 30 fps)` at boot, and in the tutorial the
sleep histogram's largest requested value is **1 ms** - the limiter's
`while (elapsed < interval) Sleep((interval-elapsed)/3)` never runs its body
when the frame is already 124 ms. The cap paces the menus (22-30 presents/s,
sleep args up to 21-24 ms) and is a no-op in the first area.

### The one fix tried, and why it is not in the tree

Hypothesis from the table: the 42 x 1 ms polls per frame are a host-shim cost,
and an event-driven `WaitForSingleObject` (a condvar per object, broadcast by
`SetEvent`/`ReleaseSemaphore`) would remove 32 of the 42 lock hand-overs per
frame. Implemented, built, run: **the frame became 77 s** (submenu, stalling),
and even the menus went from 34 ms to 94 ms. Cause: over-waking. A broadcast
wakes every waiter of an object, and for auto-reset events/semaphores that turns
one signal into several waiters deciding they were signalled - the engine's job
pump saw slots complete out of order and retried. Reverted in full, ledgered in
`AGENTS.md` section 6. A one-signal-one-waiter rework (futex-style token per
wait) is the shape of any retry, not another broadcast.

### The campaign (>=5 minutes of active play, 30 fps default)

`scripts\android_route_drive.ps1` now detects the tutorial HUD
("Light Attack / Heavy Attack / Jump") and drives held-stick swipes and
1.2-1.4 s button holds for `-PlaySeconds`, with a screencap + OCR every 25 s and
`dumpsys battery` + `dumpsys thermalservice` every 30 s from the PC side. Run
camp2: New Game -> Extreme -> cutscene -> HUD at 172 s, then **330 s of active
play, 186 gestures, 13 screenshots, process alive at 503 s**.

| metric (318 s in the tutorial, 3,159 presents) | value |
|---|---|
| presents/s | 9.9 |
| fps min / avg / max (per-second averages) | 3.9 / 9.4 / 18.5 |
| frame dt avg (min-max) | 123.9 ms (47-315) |
| frames > 50 ms | 3,154 (99.8 %) |
| frames > 100 ms | 1,191 (37.7 %) |
| limiter interval seen | 100 in every sample |
| process CPU | 99 % of one core |
| VmRSS / HWM at the end | 116 MB / 310 MB |
| guest threads (peak) | 56 (56) |
| battery temperature | 37.0 -> 39.7 C |
| thermal status (`dumpsys thermalservice`) | 0 (NONE) -> 1 (LIGHT) at ~255 s, never higher |
| battery level | 86 -> 89 % (USB charging) |
| `logcat -b crash` | empty |
| exit | `reason=10 (USER REQUESTED)` after the session's own force-stop |

No throttling: presents/s is flat across the 330 s and the temperature is
within 3 C of the start, so the fps number is the port's, not the thermals'.

### Interactivity, measured

The HUD alone is not proof that input reaches the player. The driver ends with
two shots 3 s apart with **no input**, then two shots while the stick is held
right (two 1.4 s swipes), and the view's best horizontal shift is measured:

| pair | best shift | score at that shift | score at 0 |
|---|---|---|---|
| idle A -> idle B (no input) | **+0 px** | 8.1 | 8.1 |
| move A -> move B (2 held-right swipes) | **+78 px** | 10.5 | 11.8 |

The camera pans under held-stick input and not while idle; mean per-channel
frame difference over the 318 s of play is 31.6/31.5 (G/B) against 7.1/9.3 for
the idle pair, i.e. the player traversed the area. The HUD stayed on screen for
all 13 samples (one sample OCR'd as blank under a flash effect, one as
"N ...tac").

### Traps (also in `AGENTS.md` section 6)

- **A log that stops mid-run is not a crash.** Run `fix1` has no line after
  `t=266s` in `log.txt` *or* in logcat, while the game kept rendering (whole
  screen pixel diffs of the play screenshots; the process answered `pidof` to
  the end and exited on our force-stop). The shell's log pump is a single
  thread; when it stops, fd 2 goes with it. The probe's own `perf.txt` channel
  was added because of this and produced the camp2 numbers when the same build
  logged normally.
- The route's Story submenu classifier required "Continue" and it OCRs as
  "Cpntinue" about half the time: one run sat on that screen for 200 s with
  "New Game" plainly visible. Classify on the item that survives OCR.
- A locked phone gets OCR'd as gameplay input: run camp1 pressed A at the lock
  screen for 190 s ("vodafone TR ... 59 m until full"). `wm dismiss-keyguard`
  before launch, and a lockscreen text match as a state.
- The `[mem]` `frames=` column is the shell tick (see above). Do not read it as
  guest frame rate.

### Not verified / open

- The Windows oracle was **not re-run** this session: the Windows host still has
  no working synthetic input path (DInput ignores injected keys; `KORRA_PAD_AUTO`
  is out of scope and known to regress), so the fresh render state of the
  `flow=0x9` frame and a fresh New Game run on Windows remain open. The
  2026-11-20 numbers are the Windows reference used above.
- Whether a one-signal-one-waiter wait removes enough of the 61 ms lock re-entry
  to matter (it is 49 % of the frame, so a correct implementation is the obvious
  next lever - but it is a runtime/shim redesign, and the 30-vs-structural
  decision is the owner's).
- The log-pump stall's cause (reproduced once in four runs, not again).
- Continue route, lifecycle pause/resume, and the in-game pause menu were not
  exercised this session.
- fps min 3.9/s is a per-second average of frames that reached 315 ms; no
  frame-time histogram beyond the >50/>100 ms counters is kept.


## 2026-11-22 - rung (d) closes: the first-run SAF picker installs the game root and the phone boots from it

Device: Galaxy S24 (SM-S921B, Exynos 2400, Xclipse 940, stock Vulkan 1.3.304,
Android 17 / API 37). Base HEAD `6076b25`; commits `260a690` (install flow) and
this docs commit.

**What the launch path did before (measured first).** After `adb shell pm
clear`, the activity drew a static prompt and waited for a tap - the picker
never opened by itself (`logs/fresh_old3.png`). The space gate could not pass
either: it asked `File.getUsableSpace()` on `files/game` before that directory
existed, and the device answered **0 bytes** ("Not enough free space: 2.60 GB
needed, 0 MB available", `logs/fresh_old2.png`). Validation also ran *after*
the copy, so a `GameData`-only pick copied 2.7 GB and then threw. One detail
worth recording: the dev tree on the device carried an `install.complete`
marker dated 2026-11-21 whose byte count matches a complete 820-file pass, and
no pulled log documents that copy - the picker flow had been run at least once
before this session without being written up, which is why the app had been
booting without a picker.

**The flow now** (`260a690`; Java only, the native hand-off is unchanged):

- launch: installed = `install.complete` + `LoK.exe` + both CPKs. Missing -> the
  picker opens 400 ms after `onCreate`. A partial tree whose persisted tree
  grant still resolves resumes without the picker.
- validate before copying (name what is missing, copy nothing): the folder needs
  a `GameData` directory **and** `LoK.exe`; `GameData` needs `data000.cpk` and
  `data001.cpk`. Source size is measured in the same pass, for the space check
  and the bar.
- copy: one `ContentResolver.openInputStream` per document (not `DocumentFile`
  chunk APIs), 1 MiB buffers; a target file with the source's exact size is
  skipped (resume); progress on a 400 ms byte clock with a bar and a rate, and
  `GameData` / `LoK.exe` timed separately in the log.
- space: measured source size minus what is already on disk, plus 128 MB slack,
  checked after the target directory exists.
- grant: `takePersistableUriPermission` at pick time, URI in SharedPreferences,
  re-checked against `getPersistedUriPermissions()` on every launch.

**Measured, two fresh installs** (source = a game tree kept in shared storage,
selected through the system picker; `pm clear` before each startup):

| step | run 1 | run 2 |
|---|---|---|
| picker opened by itself | 400 ms after onCreate | 400 ms after onCreate |
| validation | 820 files, 2,844,528,439 B | same |
| `GameData` copy | 15,609 ms, 182 MB/s | 19,473 ms, 146 MB/s |
| `LoK.exe` | 20 ms | 27 ms |
| result | 820 files, 0 EACCES | 820 files, 0 EACCES |
| ready -> `guest: entry 0x0077C3B7` | 24 ms | 26 ms |
| title screen | captured (`logs/saf_title1.png`) | ~50 s after the next launch (`logs/saf2_07_second_launch_title2.png`) |

A clean marker reads `files=820 skipped=0 bytes=2844528439 total=2844528439
seconds=15`. The copy rate is page-cache warm (the tree had just been written to
the same partition); a cold source will be slower, but it is still I/O-bound
seconds, not the "minutes" the plan assumed.

**A wrong pick is rejected before anything is copied.** Run 2's first selection
had no `GameData`: validation failed in 34 ms, logged "That folder has no
GameData folder. Pick the folder that contains GameData and LoK.exe.", and the
next pick installed. Zero bytes were copied.

**Resume, measured twice.** Deleting the marker and two files and relaunching:
the new process logged "install: resuming an interrupted copy from
content://...", copied 2 files, skipped 818, finished in **523 ms** (marker
`skipped=818`) - no picker. Repeated after `adb reboot`: the persisted grant
survived, 1 file + `LoK.exe` copied in **3,298 ms**, marker `skipped=819`, then
the title screen. That is the scoped-storage re-check after reboot, measured.

**Second launch** (also re-run on the final binary). `install: probed
.../LoK.exe - ready (internal)`, `game data present at .../files/game`, no
picker, loading screen at 3 s, title at ~50 s
(`logs/saf2_07_second_launch_title2.png`).

**Save path in the SAF-installed tree.** The OCR-driven New Game route (Story ->
New Game -> Extreme -> intro cutscene) reached the tutorial HUD at 260 s.
`CreateDirectoryA <root>/Korra/`, `CreateFileA(GENERIC_WRITE, CREATE_ALWAYS)
<root>/Korra/GameData.dat`, `WriteFile 7856`; the file is 7,856 B on disk. The
next boot has zero `[msgbox]` and zero `[Hw::rofs::fst]` lines and reaches the
title screen (`logs/saf2_08_boot2_attract.png`).

**Continue (opportunistic).** The Story submenu highlighted "Continue - Resume
the game from where you stopped playing"; A loaded the checkpoint straight into
gameplay ("FIGHT!" over the tree arena, `logs/cont_04_after_continue.png`) and
`GameData.dat` was rewritten at 23:56. So the checkpoint route works in the
SAF-installed tree as well.

**Lifecycle pause/resume (opportunistic): it crashes, and not by LMK.** HOME
while in gameplay, 15 s later: `lifecycle: pause` -> `window: destroyed` ->
`swapchain: recreate n=3` -> `D3D9SwapChainEx: Failed to recreate swap chain`
(`88760870` at `0x759E70`) -> `[crash] signal 11 ... guest_func=0x007524C0`;
exit-info `reason=2 (SIGNALED) status=11`, importance 400. The engine cannot
survive losing its swap chain on backgrounding; the in-game pause menu could
not be reached because that death comes first. Open, not touched this session.

**Dev path kept.** `scripts\android_run.ps1` writes the same `install.complete`
marker after its `run-as` copy and keys its "already installed" test on it, so a
dev tree is not covered by the picker. The native probe is unchanged.

**Caveat on the picker interaction.** The taps that walked the system picker in
these runs were not produced by a script - logcat has no `input` command from
this session at those moments, and the device has Microsoft Phone Link running
(which can inject input). The app's own log records, in order, the picker
opening, a rejected selection, the validated selection, the copy and the boot,
so the path is proven end to end; a fully scripted picker walk is not.

**Not verified.** A cold (non-page-cache) copy rate; the storage-full path (the
`SpaceProblem` branch is implemented, not triggered - the device had 22 GB
free); the picker on a device without a documents provider; a fully scripted
picker navigation; the in-game pause menu (unreachable while the backgrounding
crash stands); slot saves / erase / `SystemData.dat` (unchanged from the save
entry); and the Windows oracle for anything here (no shared code changed).



## 2026-11-23 - M4 audio on the S24: the AAudio sink is fed, and the Wwise voice never prefills

The Android host now answers `CoCreateInstance(CLSID_XAudio2)` with a real
IXAudio2 2.7 device, mixes what the engine submits, and hands it to AAudio. The
CRI movie audio that was silent on the phone is audible and measured. The Wwise
2 ch float voice is still never fed, and this time the break has a name and a
call path, not a guess.

**What was actually wrong on Android: the CLSID never matched.** The device
request is `{5A508685-A254-4FBA-9B82-9A24B00306AF}`, and the constant in
`misc_android.c` had Data2 written as `A2 54` instead of `54 A2` (a GUID's
Data1..Data3 are little-endian). Every XAudio2 request therefore fell through to
`REGDB_E_CLASSNOTREG`, which looks exactly like "the engine never asked for a
device". With the bytes fixed the log says
`CoCreateInstance -> IXAudio2 2.7`, `CreateMasteringVoice(in=0 ch, 48000 Hz)`,
`CreateSourceVoice(2 ch ... mask 0x3)` for the Hw device and, per movie,
`CreateSourceVoice(6 ch, 48000 Hz, float, mask 0x0)` + `SetOutputMatrix 6 x 2`
for CRI, followed by real submits.

**The sink, as built.** `src/host_android/xaudio2_android.c` is the Android twin
of the measured Windows shim: guest COM objects whose vtables are arrays of
shim VAs (`XA2dev:N` / `XA2voice:N` / `XA2src:N`), the same slice queue,
resampling of 32 kHz sources, the engine's own `SetOutputMatrix`, and retirement
that performs CRI's `OnBufferEnd` body (`*(u32*)ctx += 1`, atomically).
`src/host/aaudio_sink.c` gained the two things the hand-off said were missing: a
device-rate resampler with a carried position (44.1 kHz is now a working path,
not a warning) and cached stream parameters so the callback never calls AAudio.
Buffer size is `debug.korra.audio.buffer` (default 512 -> the device rounds to
576 frames, 12.0 ms); capacity is 3072, burst 96.

**Measured on the S24 (stock driver).** `debug.korra.audio.dump=1` +
`debug.korra.xa2=1`, 70-210 s runs:

| metric | value |
|---|---|
| stream | `opened: 2 ch, 48000 Hz, buffer 576 frames (12.0 ms, capacity 3072), burst 96, format 2` |
| first audible block | 1.0 s, peak -15.7 dBFS |
| sink 3-26 s | peak **-9.8 dBFS**, RMS **-27.7 .. -28.8 dBFS** |
| dump | 207 s, 16-bit 48 kHz stereo; boot movies peak -14.7/-9.8 dBFS |
| 6 ch movie through the engine's 6x2 matrix | peak -9.77/-10.05 dBFS, RMS -21.6 dBFS |
| AAudio xruns | 3 (all at 7 s), then 0 new |
| mixer-dry blocks | 96 in the first 26 s (see below) |

The Windows reference for the same phase is peak -9.77 dBFS / RMS -26.8 dBFS,
so the level through the device's own 6x2 matrix matches.

**Two more measured defects, both fixed.**
1. *The dump kept only its last second.* `dump_patch()` seeks to 0, writes the
   44-byte header and left the stream at offset 44, so every following block
   overwrote the file's start (a 138 s run produced a 192,044-byte file whose
   header claimed 138 s). Patched to seek back to the end. This is a
   hand-off-code defect, not an AAudio one.
2. *Out-of-range samples at slice edges.* When the queue ran dry, the mixer
   clamped `pos` backwards to `played` (the Windows code does this); with a live
   sink that re-reads a buffer whose `OnBufferEnd` has already fired, and CRI is
   free to refill it. Measured: `voice 2 out of range: l=-20071.188` at
   `pos=58064`. The cursor is now monotonic (`next_end = pos`, never backwards),
   and that window no longer reproduces - but **the fix is not the whole cause**:
   the same probe still fires on the final build, twice in a 50 s run
   (`l=-8.637` at 18.2 s and `l=-39504.168` at 24.9 s, `pos=5208`, `q=3`), always
   on channel 0 of the 6 ch voice with the other channels near zero. Two
   candidates remain and the probe cannot yet tell them apart: a torn refill of
   the guest buffer we are reading, or the engine handing over an `AudioBytes`
   region whose tail is not decoded PCM. One frame per ~25 s, inaudible in the
   RMS (-21.6 dBFS around it) and a click at worst; recorded open.

**Underruns are real and worse than Windows.** The engine submits 800-frame
(16.7 ms) buffers with a 4-deep ring; the phone's guest loop runs at ~30 fps
while its audio thread polls `GetState` ~315 times a second, so a 12-22 ms
device buffer runs dry in bursts: 96 mixer-dry events in 26 s against 0 in the
same window on Windows. AAudio's own xrun counter stays at 3. Raising the
device buffer to 1056 frames (22.0 ms) did not remove them. This is the
fill-latency half of the perf block the owner deferred, not a mixing defect: the
samples and levels are right.

**The Wwise break, named.** The Hw output device is a C++ object whose
constructor is `0x0088B000`; it installs the vtable at `0x0091EC80` and an
embedded output interface at `object+0x28` with vptr `0x0091ECC8`. Its IXAudio2
init is vtable slot `0x34` = `0x0088B0C0` (reached from the device factory
`0x008793F0`), which creates the 2 ch float source voice at `0x0088B2A8` and
stores it at `object+0x3C`; `0x0088B6B0` (slot `0x04`) starts it - the log line
`Start(1) from 0x0088B6B0`. The *feed* is `FUN_0088B440`, the first slot of the
embedded interface: it reads the voice's `GetState` (slot `0x64`) and, while
`[this+0x40] != BuffersQueued`, submits `[this+0x44] << 10` bytes from an
8-slot ring at `[this+0x20 .. 0x3C]` through slot `0x54`.

Nothing ever calls it, and the reason is one branch in the init. After
`SetVolume` the init reads the game's sound table:

```
0x0088B2CD  edi = [0xCA243C]              ; entry count
0x0088B2D7  je 0x88B33D                   ; no entries -> skip the prefill
0x0088B2D9  ebx = [0xCA2438]              ; array of 0x38-byte entries
0x0088B2E2  loop: eax = [ecx] | [ecx+4]   ; the 64-bit key at entry+0x10
0x0088B2E7  je  ...next entry / skip      ; key == 0 -> not active
0x0088B30A  cmp [[entry+4]+0x20], 1       ; cfg state
0x0088B314  [esi+0x44] = word[[entry+4]+0x44]   ; frames per buffer
0x0088B31B  [esi+0x68] = [[entry+4]+0x68]       ; queue depth
0x0088B330  call 0x88B720                 ; submit one buffer, N times
```

When it jumps to `0x88B33D`, `object+0x68` (the top-up target, read as
`[this+0x40]` by the feed) stays 0 and `object+0x6C` (frames per buffer) stays 0.
The feed's condition is then `0 != 0`: a permanent no-op. That is exactly what
the logs show - zero `GetState` and zero `SubmitSourceBuffer` on the voice for
400 s, on both platforms.

Measured on the Windows reference (same guest code, `KORRA_XA2_VERBOSE=2`, the
probe added to `src/host/xaudio2_stub.c` reads the guest globals directly):

```
[xa2] 3.8 Hw sound table: ptr=0D73F5D0 count=1
[xa2] 3.8   entry 0: key=0000000000000000 cfg=00000000
[xa2] 539.5 sound table: count=1 entry0 key=0000000000000000 (from 0x007B25DC)
```

and the key never changes through a 100 s run to the title. **The device now
measures the same thing**: with the probe in the Android shim,
`debug.korra.xa2=2` prints `Hw sound table: ptr=2241C670 count=1` and
`entry 0: key=0000000000000000 cfg=00000000` at `CreateSourceVoice`, i.e. count 1
and an inactive entry, exactly as on Windows (the earlier watchlog had only
proven the allocated head). So the device is created at 0.0-0.4 s with the table
inert, and its prefill is skipped; the game's own sound manager never activates
the entry on any screen this build reaches.

**Why no host fix.** The prefill needs `frames per buffer` and `queue depth`
from the entry's config object; inventing them would be inventing the mix
(recorded in the ledger, and the same mistake cost +11 dB on Windows). The
correct next step is the missing *activation* of the sound-table entry - a
Wwise/`Hw::Sound`-side job, i.e. the layer above this device - which is where
the feed's caller also lives (no code path calls `FUN_0088B440` in these runs).
The device itself is correct and now answers everything it is asked.

**Windows re-test after the shared-tree change.** `scripts/run_selftest.ps1`:
60 passed, 0 failed. Acceptance re-run on the final binary
(`build/run_m4e.exe`, 75 s, WASAPI shared): buffer 1056 frames (22.0 ms),
first audible block 0.032 s, 3-22 s **peak -9.77 dBFS, RMS -27.16 dBFS**,
2 underruns by 20 s (the first feed and the movie teardown), 0 per minute in
steady state, 0 `OnVoiceError`; the Wwise voice is still created, started and
never fed. The only shared-tree changes are `aaudio_sink.c` (compiles to
nothing on Windows) and the sound-table probe in `xaudio2_stub.c`.

**Not verified.** The intro cutscene on the device: the scripted New Game route
ran to its cutscene screenshots but the taps did not leave the title (all
mean ~91, the title's own brightness) and the dump is silent after the boot
movies, so the 5.1 path is proven on device only by the boot movie's 6 ch
stream. Audio after the cutscene (no Wwise event is posted). The 44.1 kHz
resampler path: the S24 hands back 48000 Hz, so the resampler compiled and
stayed idle. Underruns under load (see above). The rare out-of-range samples
(above: 2 in 50 s; the rewind fix removed one cause, not all). The feed's
caller and the sound-table activation are named, not fixed.

## 2026-11-23 - M4 activation: the Wwise feed was always there; the host never invoked the voice callback

The Wwise 2 ch float voice now plays. The break was not the game's sound table:
the table was live, and the feed the previous session looked for is the engine's
`IXAudio2VoiceCallback`, which the Android shim never invoked. Input is not
regressed; the earlier "the taps did not leave the title" run did not reproduce.

**Input, on the current audio build.** Two OCR route runs on the same APK
(`scripts\android_route_drive.ps1`, `debug.korra.memtrace=1`) walk
attract -> auto-save notice -> pad notice -> main menu -> Story -> New Game ->
confirm -> loading -> tutorial HUD, first at 201 s and again at 212 s; held taps
and 1.4 s stick swipes advance every menu. The 19:19 screenshots that motivated
the regression check are all the attract screen and no route log from that run
survives, so the earlier reading is not reproducible on this build. The route
does stall on the Story submenu in some runs (OCR reads "Continue" as
"Cpntinue"); that is the known driver weakness, not input.

**The sound table is not inert.** `0x0088B2E2` builds the 64-bit key and
`je 0x88B2F3` when it is **zero**, so key 0 is the *selector* for the default
output device, not an "inactive" marker. The cfg pointer at entry+4 is stored
by the insert path after the device's Init returns (`0x872815`), so a probe
taken inside `CreateSourceVoice` must see cfg=0. Sampling from `CommitChanges`
shows the live entry:

```
[table] voice-create: count=1 e0 key=0000000000000000 cfg=00000000 state=0 ring=0 depth=0
[table] commit:       count=1 e0 key=0000000000000000 cfg=2241C6C0 state=1 ring=4 depth=4
```

`cfg` is the Hw device; `state=1` is the base-constructor tag from `0x879260`;
`ring=4 depth=4` is the engine's mixer having produced four 1024-frame buffers.
Nothing in the table gates the feed.

**The break: the voice callback.** `0x88B2A8` calls `CreateSourceVoice` with
`pCallback = device+0x28` (vtable `0x0091ECC8`). That object is the game's
`IXAudio2VoiceCallback`, and its slots are the missing call path:

| slot | method | guest body | what it does |
|---|---|---|---|
| 0 | `OnVoiceProcessingPassStart` | `0x0088B440` | `GetState`, then submit `[device+0x68] - BuffersQueued` buffers from the 8-slot ring |
| 4 | `OnBufferEnd` | `0x0088B3F0` | `InterlockedDecrement(device+0x68)` and `SetEvent` to wake the mixer |
| 1/2 | pass-end / stream-end | `0x401D90` | `ret 4` |
| 3/5 | buffer-start / loop-end | `0x4F5A70` | `ret 8` |
| 6 | voice-error | `0x411330` | `ret 0xC` |

Every shipped body takes `this` as its **first stack argument** (slot 0 and
slot 4 both end in `ret 8`), which is exactly the host shim's COM model. The
shim only performed CRI's completion counter in C and logged every other
callback as "other", so the Hw voice's callback was never called:
`debug.korra.icall=88B440` recorded **zero** feed calls over a 255 s route run
while the mixer held four buffers and the voice had `BuffersQueued=0`.

**The fix (`src/host_android/xaudio2_android.c`).** An XAudio2-style worker
thread now calls the callback object: slot 4 once per buffer the sink retired
(counted by the mixer, never called from the sink callback), and slot 0 only
when the engine's credit counter at `device+0x68` is ahead of the voice's queue
and by no more than the ring depth. The guard is load-bearing: the guest loop
re-reads the counter on every iteration (`while (credits - BuffersQueued != 0)
submit`), so when the host samples faster than the mixer refills, one tick with
`credits < queued` makes the loop run until the counter wraps. Measured without
it: **8,669,699** submitted buffers in 90 s, 843 MB of log, and frame times of
1.76 s with 1.7 s waiting for the machine lock. With it: credits oscillate 4<->3,
`qcount` 2..4, zero queue-full events, and the feed's back-edge yields stop.

**Measured after the fix (S24, stock driver, 360 s route, `debug.korra.audio.dump=1`,
`debug.korra.xa2=2`).** The Hw voice is fed from 152-418 ms:
`GetState(1): poll #1 ... from 0x0088B440`, then `SubmitSourceBuffer(1): first
buffer 8192 bytes, 1024 frames (48000 Hz, 2 ch, float)`; the dump has continuous
menu/title audio from 73 s to the New Game at 322 s (peak -7.5..0 dBFS,
RMS -30..-35 dBFS) and continues through the loading/cutscene transition
(322-359 s, RMS -17..-25 dBFS). The 5.1 path is still the boot movie's 6 ch
stream: no new CRI voice is created at the in-engine intro. AAudio xruns 13 (all
early), mixer-dry 259 in 359 s.

**New break exposed by the fix: the Wwise mix contains NaNs/huge floats.** The
sink's own meter (float, before the 16-bit dump) reports `peak=47.3 dBFS` in one
run and `peak=186.6 dBFS rms=nan` in another once the Hw voice plays; the boot
movies before it are clean (-14.7/-11.9 dBFS). The 16-bit dump therefore clips
0.23 % of samples in 73-250 s (longest run 1442 samples) and 0.34 % in
322-359 s. The one-shot mixer probe (`fabsf > 2.0`) caught only two modest
values (`-6.7` on the 6 ch voice at 16.7 s, `-2.6` on the Hw voice at 44.3 s)
because a NaN comparison is false. This is the "partially decoded/overwritten
`AudioBytes`" half of the old out-of-range question, now with a large signal;
the next step is to log the slice `[start,end)` and `i0` with the value, and
classify NaN/Inf in the probe. The Windows shim has the same missing callback
(it is unfed too); that divergence is recorded, not closed.

**Not verified.** The in-engine intro cutscene was reached only at 322 s, with
10 s left before the run ended, so its 5.1/6 ch measurement is not taken; no CRI
6 ch voice is created on that path (the old "cutscene = CRI movie" reading holds
only for the boot movies). The 44.1 kHz resampler is still idle. The NaN source
is named only by the sink meter; no slice-level evidence yet. Windows acceptance
was not re-run: no `src/host/` file changed.

## 2026-11-23 - M4 activation follow-up: the huge Wwise sample is an integer (2^31) in the guest ring

The new mixer probe (first three per voice, NaN/Inf included, with the slice
bounds and the raw word) names the source of the sink's `peak=186.6 dBFS
rms=nan`:

```
voice 1 bad sample l=2.14748e+09 r=-2.14748e+09 (pos 2120029.0,
  slice [2119850,2120874), i0=2120029, raw=4F000000, ch 2, bits 32,
  float 1, matrix 0, q=3, data 2242A770)
voice 1 bad sample l=-223.184 r=267.632 (pos 2120031.0, ... raw=C35F2F1A)
voice 1 bad sample l=2.14748e+09 r=-2.14748e+09 (pos 2120032.0, ... raw=4F000000)
```

`0x4F000000` is exactly 2^31 as a float and 1325400064 as a signed integer;
the negative channel is `0xCF000000`. These are *integer bit patterns inside
the guest's float PCM ring*, at a slice-relative offset of 179 frames, with
valid floats on either side - not a level and not a NaN from the decoder. The
6 ch CRI voice shows the same shape earlier (`l=-2.13552`, raw `C0BFD0C4`).
The slice-relative position and the 4-word burst point at the engine's mixer
(or a buffer region it does not fill) rather than at the host read: `i0` is
inside `[start,end)`, the format is the engine's own float32 (the GUID at
`0x9090FC` is `0300...` = IEEE_FLOAT), and the host reads exactly the word the
game wrote.

Next: identify the 4-word burst in the engine's mixer - a ring-slot header, a
limiter sentinel, or a partially filled `AudioBytes` tail. Until then, the sink
meter and the dump remain the evidence, and the `|x| > 2` one-shot is replaced
by first-three-plus-non-finite with the slice bounds (this probe).

A deterministic manual menu walk (8 held A taps through the notices/menus) then
reached the tutorial HUD in about 100 s; the log still shows no 6 ch
`CreateSourceVoice` after the boot movies (last at 24.3 s), so the intro path on
this build is not a CRI 6 ch movie. The 5.1 stream therefore remains proven on
device only by the boot movie, and the intro cutscene's own audio is the 2 ch
Wwise mix (measured through the loading transition above).

## 2026-11-24 - M4 parity: the Windows shim feeds the Hw voice too, and its mix is clean

The activation fix was host-side, so the Windows shim had exactly the same hole it
had on Android: it performed CRI's completion counter in C, classified the Hw
device's callback as "other" and never invoked either slot. The Hw voice was
created and started on the dGPU as well and never queued. It is now fed by the
same model as the Android shim - a host worker thread that is the twin of
XAudio2's own callback thread - and the Windows mix measures clean where the
device's does not (next entry).

**The port (`src/host/xaudio2_stub.c`).** A third callback kind,
`XA2_CB_HW_FEED`, is recognised at `CreateSourceVoice` by vtable slot 4 ==
`0x0088B3F0` (the object the device registers is `device+0x28`, so `cb+0x40` is
the credit counter at `device+0x68`). Retirement on the sink thread only does
`InterlockedIncrement(cb_retired)`; nothing enters guest code from there. A
worker thread (`xa2_cb_ensure_thread`, started when such a voice is created)
runs every 10 ms and, with the machine held:

* calls slot 4 (`OnBufferEnd`) once per retired buffer (capped at the 8-deep
  ring), so the engine's mixer is credited and woken;
* re-reads `credits` and `BuffersQueued` **after** those calls and calls slot 0
  (`OnVoiceProcessingPassStart`, the feed `0x0088B440`) only when
  `credits > queued` and by no more than the ring depth.

The guard matters for the reason recorded on Android: the guest loop re-reads
the counter every iteration, and a tick with `credits < queued` runs until the
32-bit counter wraps (8,669,699 submits in 90 s). Calls go through
`native32_call_guest`, which the runtime already supports from a foreign thread
(`mach_enter` gives it a guest stack and TIB on first entry); the ledger's
"do not re-enter the guest from inside a COM method" warning is about the
lifted caller's registers, not about a host thread with its own machine state.
`tests/selftest_host_shims.c` gained a `native32_call_guest` recorder so the
harness links; `scripts\run_selftest.ps1`: **60 passed, 0 failed**.

**Measured on the dGPU (RTX 5050, WASAPI shared, `KORRA_AUDIO_DUMP`,
`KORRA_AUDIO_LOG=1`, `KORRA_XA2_VERBOSE=2`, fixed pad taps).**

| metric | run A (97 s) | run B (174 s) |
|---|---|---|
| `CreateSourceVoice` 2 ch float mask 0x3 | 1.0 s, `cb=0D723648 Hw feed (slot 4 credits, slot 0 submits)` | 1.0 s, same |
| callback thread | started 1.2 s | started 1.2 s |
| first Hw submit | 655 ms, 8192 B = 1024 frames, ctx=00000000 | 799 ms, same |
| feed state | `cb tick(#1): credits=4 qcount=2..4` | same, 0 queue-full, 0 dry on voice #1 |
| title/menu music | **66-97 s continuous**, peak -17.8 .. -27.4, RMS -30.1 .. -38.4 dBFS | **66-174 s continuous**, peak -6.8 .. -27.6, RMS -24.9 .. -45.5 dBFS |
| 6 ch CRI voices | boot movies only (4.2 s on) | boot movies + 105.8 s and 136.4 s (Stop/Flush/Destroy at `0x7B2D0E`, 800-frame buffers) |
| clipped frames in the dump | 0 of 4.66 M | **0 of 8.35 M** |
| underruns | 16 by 20 s (boot movies), 0 from 30 s to the end | 19 by 30 s (boot movies), **0 from 30-100 s** (Hw voice only), 460 in the 74 s CRI-movie window (100-174 s, 6/s) |
| `OnVoiceError` | 0 | 0 |

The menu music level matches the device's post-fix band (peak -7.5..0, RMS
-30..-35), so the shared matrix/level contract holds on both hosts. Run B's
under-run cluster is the **CRI movie feed**, not the Hw voice: every `ran dry`
line names voice `684CB7D0` (a CRI 6 ch movie device, 800-frame buffers, sink
blocks of 480 frames), and none names voice #1. The same defect is in the
recorded gameplay run (89 underruns in a movie minute); this machine-day puts it
at 6/s. An A/B against `build\run_m4e.exe` (the binary without the callback) was
run on the same tap timeline: its boot-movie window gives **21** dry events
against **19** here, so the callback thread does not regress the movie feed in
the window both binaries reach; the later window is unreachable there (its
Wwise voice is never fed, so 30-159 s is exact silence).

**`docs/audio.md` 9.4, re-measured.** Test 2 is no longer blocked: menu/title
music plays from 66 s on both runs at the level above (the host cannot see Wwise
event ids, so *which* cue it is is not proven). Test 3's format clause is
measured again - the 6 ch CRI voices go through the engine's own 6x2 matrix
(peak -7.13 dBFS at 126 s) - but its own terms (the 94.9 s `bgm_cs41_skip`
stream) are still unmet, and this run had no file tracing, so "the stream was
never opened" is not re-asserted. Tests 4, 5 and 6 remain unproven for engine
reasons, not host reasons: 4 and 5 are Wwise-internal (a one-shot over a running
cue, and pause/resume, are not observable at the XAudio2 boundary), and no
32 kHz mono voice was created in 174 s. Test 7 passes on formats and errors; its
underrun clause fails inside a movie minute, as above.

**Also in this session (diagnostics).** The Windows mixer gained the Android
one-shot bad-sample probe (first three per voice, NaN/Inf included, with the
slice bounds, the raw word and a 32-word hex window of the ring), and the
Android probe gained the same window plus an FNV hash of the slice taken at
`SubmitSourceBuffer` and re-taken at the probe (`REWRITTEN since submit`).

**Not verified.** The device-side numbers in the next entry are the S24's; this
entry's are Windows only. Whether the CRI movie feed's 6/s is the worker
thread's contention or the pre-existing deadline defect is not separated in the
window only the new binary reaches.

## 2026-11-24 - M4 burst: it is a NaN tail the engine has not committed, not an integer

The `2^31` the sink meter reported is a **symptom**. The slice-level probe (the
32-word ring window plus a whole-slice class profile, both added this session)
caught the words themselves on the S24:

```
voice 1 bad sample l=nan r=nan (pos 1941381.0, slice [1940891,1941915), i0=1941381,
  rel=490, raw=00000000, ch 2, bits 32, float 1, matrix 0, q=3, rate 48000,
  ratio 1.000, data 2242C770, hash 38516DC5 (at submit A5275264 - REWRITTEN since submit))
  slice profile: 1024 frames - zero 491 [0..490], non-finite 533 [491..1023], |x|>2 0,
  finite 0, q=3 pos=1941381
```

Four facts, in order of how much they settle:

1. **The words are NaN, not integers.** `0x7FC00000` is the quiet NaN; the
   `0x4F000000`/`0xCF000000` pair the earlier probe saw is what the *engine's
   own* limiter makes of it - a float-to-int saturating convert (`cvtps2dq`,
   present at `0x88A999`-`0x88A9BB` inside a `mulps/cvtps2dq/packssdw`
   conversion loop) clamps NaN to `INT_MIN`, and `cvtdq2ps` turns that back into
   exactly `-2^31` (`0xCF000000`) or `+2^31` (`0x4F000000`). The ring holds NaN;
   the full-scale clipping in the dump is the engine's own saturation of it,
   downstream of the ring.
2. **It is not inside committed frames: it is the tail of a ring slot.** The
   1024-frame slice is 491 frames of exact zeros followed by 533 non-finite
   frames that run to the slice end (`491 + 533 = 1024`). No finite sample is in
   that buffer at the moment of the probe.
3. **The engine rewrote the buffer while it was still queued and being mixed.**
   The FNV taken at `SubmitSourceBuffer` (`A5275264`) differs from the one taken
   at the probe (`38516DC5`), and the probe reads the map's head slice (`q=3`,
   `pos` 490 frames inside it). So this is not a decoder level and not a torn
   *sample*: the host is reading memory the engine has not finished committing.
4. **The audible effect is a burst, not a level.** The 679 s dump from the same
   build clips 0.2223 % of frames in 212 events (0.31/s); the first is at
   51.449 s, where the probe fires, and the seconds around it read peak
   0.00 dBFS against -11..-15 dBFS of music either side. The 6 ch CRI voice also
   produced one finite out-of-range sample (-6.9, hash unchanged) - that is the
   old, already-recorded single-sample case, a different thing.

**Decision table, applied.** This is the "partially filled `AudioBytes` tail the
engine never commits" row. What the host can prove about the words is narrow and
exact: in the engine's own format (IEEE float32, the GUID at `0x9090FC`), NaN is
not a PCM value at all. So the host may zero exactly the non-finite words - no
finite value is touched, no level is invented - and the engine's frame count
stays the authority for everything else. It may **not** clamp, attenuate or skip
the range: the tail is the engine's buffer, and the dropouts it causes are its
business. The next entry is that guard, measured.

**Still open (named, not solved).** *Why* the tail holds NaN: the mixer writes
into the slot while the host reads it, and the region it has not written yet
holds whatever was there before - this run, NaN. Where those NaN bits enter (a
ring slot poisoned once by a decoder, or an uninitialised fill in the engine) is
not established, and with credits counted per buffer (1024 frames, from the sound
table's cfg) the host cannot know how many frames the mixer has committed. The
`ring=4 depth=4` the table reports against the feed's 8-slot cursor (`cmp ax,8`)
is the shape to check next: with 4 slots in flight and a refill 4 ahead, a credit
delivered while the host still reads a slot rewrites exactly the buffer being
mixed.

## 2026-11-24 - M4 burst follow-up: the NaN guard, measured (and what it does not fix)

The decision table's row 2 licenses exactly one host action: zero the words that
the engine's own float32 format says are not PCM, touch nothing else, and
re-measure. That is now in `xv_sample_ch` (Android): a non-finite float is
returned as silence, counted per voice, and the first three per voice are logged
with the word, the slice-relative index and the running count. No finite value is
clamped, attenuated or skipped.

**Measured, same APK lineage, 114 s run with the dump and `debug.korra.xa2=1`:**

| metric | before the guard | after the guard |
|---|---|---|
| clipped frames in the dump | 72468 of 32.6 M over 679 s (**0.2223 %**) | 1586 of 5.47 M over 114 s (**0.0290 %**) |
| clip events | 212 (0.31/s) | **5** |
| first event | 51.449 s (the probe's first NaN) | 19.427 s (2 frames) |
| clamp bursts in the music window | every ~0.3 s | none |
| sink meter | `peak=186.6 dBFS rms=nan` (earlier run), `47.3 dBFS` (another) | `peak=377.6 dBFS rms=307.4` - **not clean** |
| xruns | 13 early | **0** in 114 s |

So the NaN bursts are gone: 212 events become 5, and the music window no longer
clips. The meter is nevertheless **not** clean, and the reason is a *different*
source that the guard must not touch: the 6 ch CRI movie voice carries a few
**finite** values around 1e18 (`l=7.55e18 r=0.00033` at 19.6 s, `l=104.8` at
26.4 s, hash unchanged since submit), which is ~377 dBFS and clips 0.014 % of
frames. Those are finite words inside a committed buffer, so under the same rule
they stay in the mix; the item was already recorded as the 6 ch voice's
out-of-range samples and it is still open.

**RMS honesty.** Zeroing the clamped frames *lowers* a dump's RMS by construction
(they were full-scale): in the 679 s before-dump, RMS as written is -25.91 dBFS
and -32.51 dBFS with the clamped frames zeroed. The music's level is the second
number; the first was inflated by the artifact. Per-second music RMS in the guard
run sits in the same -25 .. -38 dBFS band as before, i.e. no audible level moved
- but "RMS unchanged" cannot be asserted across two different routes, so the
honest statement is: no finite sample changed, and the artifact's energy is gone.

**Not fixed, named.** Why the slot's uncommitted tail holds NaN, and the 6 ch
movie voice's finite ~1e18 samples. Both need the engine's own fill bookkeeping
(what it wrote, how many frames, into which slot) - the host sees only the
declared `AudioBytes`. The guard is a *format* rule, not a fix for either.

## 2026-11-24 - M4 close-out: the device intro cutscene is the 2 ch Wwise mix, and the tail is a *fill boundary*

Two measurements finished the acceptance items.

**The device intro cutscene, measured.** The OCR route (this time it read the
Story submenu correctly) walked attract -> notices -> menu -> Story -> New Game
-> confirm NO->YES -> difficulty -> loading and reached the tutorial HUD at
163 s; the log shows `CreateSourceVoice` only at 1.0 s (2 ch float, mask 0x3,
the Hw voice) and at 11.2 s / 23.3 s (6 ch CRI, the boot movies). **No 6 ch voice
is created after 24.2 s**, so the in-engine intro is fed by the Hw voice's 2 ch
Wwise mix - the corrected fact stands, and the 5.1 proof remains the boot
movie's 6 ch stream. Levels in the cutscene/loading window (109-163 s of a 199 s
dump): continuous, RMS -13 .. -35 dBFS with peaks at 0.00 dBFS where the
stale-tail artifact below clips. AAudio xruns 0 in that window.

**The tail is a *fill boundary*, and the numbers say so.** A second slice profile
from a later buffer, this time with no NaN at all:

```
slice profile: 1024 frames - zero 0 [-1..0], non-finite 0 [-1..0], |x|>2 688, finite 336
ring words rel 328..: 3D08F1FF 3CC6B1D6 3C883F9B 3C2C88BB 3B8A8D5B BB82C079 BC414BAA
  BC8793B2 409E91BC 409E91BC 409E91BC ... (688 x 409E91BC, to the slice end)
```

`0x409E91BC` is the constant 4.9553 repeated 688 times, i.e. the *previous*
content of the slot, unchanged to the end of the declared slice - exactly the
shape of the NaN case (491 zeros, then 533 NaN). Two different buffers, two
different stale contents, the same geometry: the feed declares 1024 frames per
submit (its sound-table cfg gives `8 << 10` bytes) and the mixer commits fewer,
so the host mixes a tail that is not this buffer's audio.

That also explains why the meter climbs after the NaN guard: the stale tail is
sometimes NaN (now zeroed) and sometimes a finite leftover like 4.955, which the
guard must not touch. The audible effect is the same clipping, 0.029 % of frames.

**What this leaves open, precisely.** The engine's *committed* frame count per
buffer is not exposed to the host (credits are per buffer); the mixer's own fill
cursor lives inside its object. Until that is readable, a host-side "skip the
tail" would be inventing a boundary. The two candidate boundaries - `ring=4`
slots against the feed's 8-slot cursor, and a credit delivered while the host
reads the slot - are the next thing to measure, not to guess.

## 2026-11-25 - save completeness: FindFirstFile's wildcards, the first `SystemData.dat` write, and the slot/erase paths the shipping build never asks for

Device: Galaxy S24 (Exynos 2400, Xclipse 940, stock Vulkan), APK built from this
tree with `-PkorraLinkGuest=ON -PkorraWithDxvk=ON`. Windows oracle: the dGPU
host. Base HEAD `3031a6d`; commits `bd43a0e` (matcher), `71e09ce` (probe).

**The defect, replayed without a run.** The save device tests its directory with
`"%s*.*"` (`0x76CC40`) before every write, and the shim's `wild_match`
(`src/host_android/kernel32_android.c`) answered **false for every `*.*`**: its
`if (*m == '*' && !star)` guard made a second `*` unmatchable, so the mask
advanced to `.` and never matched a name. A second, independent wrong answer:
any mask with two stars (`*Data*`, `*a*`) also answered false. Replaying the old
algorithm over a Korra-like directory (a.jpg, data000.cpk, GameData.dat, nofile,
README, SlotData_0.dat, SystemData.dat):

| mask | Windows `FindFirstFileA` (measured) | old matcher | new matcher |
|---|---|---|---|
| `*` | all 7 names | correct | correct |
| `*.*` | all 7, dotless names included | **none** | all 7 |
| `*.dat` | GameData, SlotData_0, SystemData | correct | correct |
| `*.jpg` | a.jpg | correct | correct |
| `?` | 1-character names only | correct | correct |
| `GameData.dat` (literal) | exact name | correct | correct |
| `*Data*` | the four names containing "data", case-insensitive | **none** | the four |
| `SlotData_?.dat` | SlotData_0.dat | correct | correct |
| `*.` | extensionless names only (nofile, README) | **none** | extensionless |
| `*.*dat` | the three .dat files | **none** | the three |
| `GameData.*` | `GameData.dat` only (the measured set has no dotless `GameData`; the DOS rule matches one if it exists) | **none** | `GameData.dat` (and a dotless `GameData` if it existed) |
| `GF?e*` | no match | correct | correct |

The Windows column is a real `FindFirstFileA` oracle (a small MSVC program over
the same names), not an assumption; it also returned "." and ".." for the
wildcard masks, which the shim filters by design and the engine's rofs mount
skips (directories), so the answers agree on files.

**The fix (`bd43a0e`, `src/host_android/kernel32_android.c`).** Measured Windows
semantics for one path component: `*` any run, `?` one character, comparison
case-insensitive (the FAT/NTFS rule - the asset tree holds `BGM.bnk` and
`data000.cpk` under masks with mixed case), plus the DOS legacy that
`FindFirstFileA` still implements: `NAME.*` also matches `NAME` with no
extension (which is why `*.*` matches everything), and a mask ending in a bare
`.` matches only extensionless names. The FindFirstFile contract is completed
too: a failed search now *sets* the guest's last error (`ERROR_FILE_NOT_FOUND` 2
for no match, `ERROR_PATH_NOT_FOUND` 3 for a missing directory, 5 for denied)
instead of only printing one, and `FindNextFileA` already set
`ERROR_NO_MORE_FILES` 18 after the last entry.

**Device, before and after at the same call site.** Before (`logs/saf_save_log.txt`,
t=223 s; `GameData.dat` had been written 148 s earlier):

```
fs: FindFirstFileA(<root>/Korra/*.*) -> <root>/Korra mask=*.*
[save] FindFirstFileExA pat="<root>/Korra/*.*" -> 0xFFFFFFFF last=2 first="" (host="<root>/Korra")
[save] CreateDirectoryA path="<root>/Korra/" -> 0x00000001
[save] CreateFileA path="<root>/Korra/GameData.dat" ... disp=2 -> 0x01000002
```

After (this session's run, an Options-menu save with `GameData.dat` already
present; log kept as `logs/opt_live6.txt`):

```
fs: FindFirstFileA(<root>/Korra/*.*) -> <root>/Korra mask=*.*
[save] FindFirstFileExA pat="<root>/Korra/*.*" -> 0x02000001 last=0 first="GameData.dat" (host="<root>/Korra")
[save] CreateFileA path="<root>/Korra/GameData.dat" access=0x40000000 share=0x00000000 disp=2 -> 0x01000002
[save] WriteFile h=0x01000002 n=7856 -> ret=1 count=7856 last=0
[save] FindFirstFileExA pat="<root>/Korra/*.*" -> 0x02000001 last=0 first="GameData.dat"
[save] CreateFileA path="<root>/Korra/SystemData.dat" access=0x40000000 share=0x00000000 disp=2 -> 0x01000002
[save] WriteFile h=0x01000002 n=16 -> ret=1 count=16 last=0
```

No `CreateDirectoryA` at either step now: the existence test answers with the
file that is there. The directory afterwards (`run-as ls -la`, sizes and hashes
pulled from the device): `GameData.dat` 7,856 B `fb030a2a6e44f19c...`,
`SystemData.dat` 16 B `36a7601077d59cd2...`. Zero `[msgbox]` in the run and in
the next boot.

**`SystemData.dat` measured for the first time, and the shipping set named.**
The trigger is the **Options menu**, derived from the original exe: the class is
`UIOptionMenu` (`0x5D7F40`, RTTI method name) and its state 7 (`0x5D837B`) is
the only caller of `req_save(5)` (`0x632F20`). `req_save` sets
`[0xCA6C28]=3` and `[0xCA6C20]=1` (reason 5 is the only value that sets the
latter), and the save manager's full-save tick (`0x632330`) writes `GameData.dat`
and then `SystemData.dat`. The value-change test is `0x5D96C0`: it compares the
live settings (`this+0xe4..0x118`) with the snapshot taken on entry
(`this+0xac..0xe0`) and returns non-zero when any of them moved, so a settings
change plus leaving the menu is the trigger. On the device: main menu ->
Options -> Camera -> Vibration ON -> OFF -> B -> B, and the two writes above are
the result. The next boot opens `SystemData.dat` with `last=0`
(`0x01000002` where it used to be `last=2`). The owner's own `<Documents>/Korra`
from the original game holds exactly `GameData.dat` 7,856 B and
`SystemData.dat` 16 B, so the shipping save set is those two files, and the
device's 16-byte file matches the original's size.

**What asks for a save (original exe, not the analysis copy).** `[0xE4F474]` is
the selector: -2 = `SystemData.dat`, -1 = `GameData.dat`, otherwise
`SlotData_%d.dat` (`0x76CB50`), and `[0xE4F46C]` is the operation: 2 = load
(`0x76CEA0`), 3 = write (`0x76D0E0`), 1 = slot scan (`0x76CCD0`), 4 = erase
(`0x76D2F0`). The request functions `0x76B8A0`/`0x76B8F0`/`0x76B940`/`0x76B990`
are the four (System/Game x load/save) entry points; `req_save(reason)`
(`0x632F20`) is called from `0x4D16D4`, `0x5D8380` (reason 5),
`0x5E4DF8`, `0x5E5335`, `0x5FA289`, `0x64F37E`, and twelve gameplay sites write
`[0xCA6C28]=3` directly (`0x53F1D1` is the boot's SystemData load with 1;
`0x57DD27`, `0x5CA688`, `0x5CA77B`, `0x5D3644`, `0x5D37AB`, `0x5D3A05`,
`0x5DD9FC`, `0x5EA4D4`, `0x5F1D1F`, `0x5F7A63`, `0x5F9236`, `0x64EB9C`).
There is **no manual Save/Load/Delete UI**: `UIPauseMenu`'s four methods
(`0x5D5F80`, `0x5D6150`, `0x5D6330`, `0x5D66B0`) call nothing in the save
device, the only UI caller of `req_save` is `UIOptionMenu.run`, and the strings
are `Save Data` (`0x96479C`), `UIConfirmSaveIconDialog` / `UISaveIcon` (the
saving indicator), with no Load/Delete menu string anywhere in the image.

**`SlotData_%d.dat` and the erase path are not in the shipping flow.** The slot
scan op 1 (`0x76CCD0`) is the only code that walks `SlotData_%d.dat` (it opens
each, reads a 4-byte header and `inc [0xE4F474]`), and the erase op 4
(`0x76D2F0`, whose `DeleteFileA` at `0x76D398` is the save device's only delete)
is the only code that removes one. Both are dispatched from `[0xE4F46C]`, and
every write to that global in the whole image is 0, 2 or 3 (`0x76B8CF`,
`0x76B91F` = 2; `0x76B96F`, `0x76B9CA` = 3; `0x76CB36`, `0x8F5368` = 0) - so ops
1 and 4 are unreachable and the selector is only ever -2 or -1. The other
`DeleteFileA` site (`0x797FBE`) and the only `MoveFileA` (`0x7981CD`) are
outside the save device. Recording this instead of inventing a UI: there is no
slot save and no erase to exercise in this build, which is also why the
owner's original save directory has no `SlotData_*.dat`.

**Windows oracle.** A save request with `GameData.dat` present was measured on
the Windows host (`korra_host-20251125-200043.log`, blind pad timeline): the
same `FindFirstFileExA(<Documents>/Korra/*.*)` answered a **valid search
handle**, then `CreateFileA(.../GameData.dat, GENERIC_WRITE, CREATE_ALWAYS)` and
`WriteFile 7856`. The probe's `first=` field was empty on every Windows line
because `src/host/save_probe.c` read `args[1]` for `FindFirstFileExA`, whose
`lpFindFileData` is `args[2]`; fixed in `71e09ce`, so the two hosts' lines are
now comparable field for field. The Windows **SystemData** line does not exist
this session: the engine's menus navigate on the left stick (the device's route
is a stick swipe; the same screens), while the Windows pad stub exposes
`wButtons` only and centres both sticks - sixteen dpad presses (`0x0001`) moved
the cursor nowhere. That is an input-injection gap on the oracle, not a save
defect, and it is recorded as not reached rather than worked around.

**Not verified.** A device Continue run after the fix (the phone dropped off USB
before it started - `adb devices` empty mid-session); autosave after gameplay
progress (the twelve gameplay `[0xCA6C28]=3` sites and the six `req_save` call
sites other than the Options one); the Windows `SystemData.dat` line; and a slot
or erase flow, which the code says does not exist.

**Addendum (same session, after the USB link came back).** The Continue path was
re-run on the device with the fix in place and `SystemData.dat` present: the
route title -> A -> A -> main menu -> Story -> Resume loaded the checkpoint and
requested its save at the **same call site** that answered
`0xFFFFFFFF first=""` at t=223 s on 2026-11-22 - now
`FindFirstFileExA(<root>/Korra/*.*) -> 0x02000001 last=0 first="GameData.dat"`,
no `CreateDirectoryA`, then `CreateFileA(.../GameData.dat) disp=2` and
`WriteFile 7856`. Files afterwards: `GameData.dat` 7,856 B (mtime 21:04),
`SystemData.dat` untouched at 16 B, zero `[msgbox]`; scene words
`flow=00000009 inst=00000001 step=FFFFFFFF mode=00000000` and the frame is not
black (mean 79/255 over a 58x27 grid, no OCR text) - the same post-load state
the earlier Continue runs reported, so Continue survives the new save type.
Still not reached: an autosave driven by **gameplay progress** (the twelve
`[0xCA6C28]=3` sites) - the write above is the load's own request, and a 250 s
Continue run that never left the menu produced none - and the Windows
`SystemData.dat` line.


## 2026-11-25 - coordinator verification: the save set is accepted; two items stay open

Independently re-checked against the tree and the device, not the report:

- **Tip and commits.** `main == origin/main` at `a11cf8a` ("docs(save): Continue
  re-measured at the same call site after the matcher fix"), tree clean;
  `bd43a0e` (matcher), `71e09ce` (probe), `720c86e` and `a11cf8a` (docs)
  present; no file outside the save scope.
- **Matcher, against a real Windows oracle.** A fresh `FindFirstFileA` oracle
  over the same seven names (`a.jpg`, `data000.cpk`, `GameData.dat`, `nofile`,
  `README`, `SlotData_0.dat`, `SystemData.dat`) agrees with the new matcher on
  every row: `*`, `*.*`, `*.dat`, `*.jpg`, `?`, literal, `*Data*`,
  `SlotData_?.dat`, `*.`, `*.*dat`, `GF?e*`; case-insensitive. The one table
  caveat is `GameData.*`: the measured set has no dotless `GameData`, so
  Windows returns only `GameData.dat`; the DOS rule matches a dotless `GameData`
  only when such a file exists. No code change.
- **Slot/erase, statically.** A capstone scan of the original exe finds writes
  to `[0xE4F46C]` of 2, 2, 3, 3, 0 - no 1 (slot scan) or 4 (erase); exactly the
  twelve `[0xCA6C28]=3` sites the entry lists. The `UILoad` string hit is
  `UILoadIcon` (a loading/saving indicator), so the "no Load/Delete menu"
  conclusion stands.
- **Device files.** `logs/opt_live6.txt` writes `SystemData.dat` 16 B after the
  Options change; `logs/cont_live2.txt` boots with `SystemData.dat` `last=0`,
  `FindFirstFileExA ... first="GameData.dat"`, no `CreateDirectoryA`, and
  `WriteFile 7856`. Live `run-as` listing: `GameData.dat` 7,856 B (mtime
  21:04), `SystemData.dat` 16 B (19:57). The owner's original
  `<Documents>/Korra` holds exactly the same two sizes; the hashes differ, as
  expected for a different save state.
- **Selftest/guard.** `scripts\run_selftest.ps1` 60/60; `tools\check_content.py`
  exit 0.

Corrections recorded, not silently patched: the `GameData.*` row above; the
`71e09ce` probe fix has not been exercised on Windows (the post-fix
`korra_host-20261125-204926.log` reaches only the asset `FindFirstFileA`
lines), so it stays unproven until an Options run prints `first=`; and the
2026-11-24 "the mixer commits fewer" wording is an inference - the committed
frame count is not exposed - now worded as "leaves a tail it did not rewrite in
that cycle" in `docs/audio.md`, README and MASTER_STATE.

Open: the Windows SystemData oracle (left-stick injection in
`src/host/xinput_stub.c`; the stub zeroes both sticks) and an autosave driven
by gameplay progress (the twelve `[0xCA6C28]=3` sites are named, none
exercised). The shipping save set itself is closed.


## 2026-11-26 - the Windows oracle walks the Options menu on the left stick, and the tutorial's checkpoint save is pinned to `hap::HapSaveActionArea`

Device: Galaxy S24 (Exynos 2400, Xclipse 940, stock Vulkan), `<device-serial>`
present throughout; Windows oracle: the dGPU host. Base HEAD `361e8ca` (one
docs commit above the hand-off's `a11cf8a`; the tip subject was verified).
Commit `530e885` adds the left stick; the route/docs commit follows. Logs are in
`logs/` (gitignored): `win_opts_stick2.txt`, `win_vib1.txt`,
`win_boot_after_save.txt`, `sauto2_drive.txt`, `sauto3_drive.txt`,
`scont1_drive.txt`, and the pulled device `log.txt`.

**The Windows gap was the pad's missing left stick, not the game.** The menus
navigate on the left stick (`FUN_007606d0` remaps `sThumbLX/LY` into its 0x24-byte
pad block), while `src/host/xinput_stub.c` wrote `wButtons` only and set all four
`sThumb*` to 0. `530e885` adds `KORRA_PAD_STICK=lx,ly` (a constant deflection),
optional `lx ly` fields on the `KORRA_PAD_CMD` timeline, and bumps
`dwPacketNumber` when the stick changes (the engine edge-detects on it). The
default is still 0/0 and the knob is inert without `KORRA_PAD=1`.

**The menu walk, measured.** A 150 ms pulse at `sThumbLY=-22000` moves the main
menu cursor **exactly one item** (`Story -> Pro-Bending -> Options`, each read
from its description: `Start Story Mode.`, `Complete Story Mode to unlock.`,
`Change game settings.`); an 1,800 ms hold auto-repeats and ran Story -> Exit in
one gesture, which is why the first attempt overshot. The Options root is
`Controller / Camera / Sound / Display`; the Controller page's `Vibration` row
starts `ON`. The leave path is `UIOptionMenu` state 4: `0x5D96C0` compares fourteen
live settings (`+0xE4..+0x118`) with the entry snapshot (`+0xAC..+0xE0`) and only
a **net difference** sets state 7 (`0x5D837B`, `req_save(5)`). The first attempt
toggled `ON -> OFF -> ON`, ended equal to the snapshot, and wrote nothing; one
horizontal pulse `ON -> OFF`, then B (Controller -> root) and B (leave), wrote.

**The Windows save lines (first time on Windows).**

```
[save] SHGetSpecialFolderPathA hwnd=0 csidl=5 create=0 -> ret=1 path="<Documents>"
[save] FindFirstFileExA pat="<Documents>/Korra/*.*" -> 0x5B25DF08 last=1008 first="."
[save] FindClose h=0x5B25DF08 -> ret=1
[save] CreateFileA path="<Documents>/Korra/GameData.dat" access=0x40000000 share=0x00000000 disp=2 flags=0x00000080 -> 0x00000984 last=183
[save] WriteFile h=0x00000984 n=7856 -> ret=1 put=7856 last=183
[save] FindFirstFileExA pat="<Documents>/Korra/*.*" -> 0x5B25E008 last=1008 first="."
[save] CreateFileA path="<Documents>/Korra/SystemData.dat" access=0x40000000 share=0x00000000 disp=2 -> 0x00000980 last=183
[save] WriteFile h=0x00000980 n=16 -> ret=1 put=16 last=183
```

`first="."` is the real Windows answer: `FindFirstFileExA` returns the `.`/`..`
entries, which the device shim filters because the engine's rofs mount skips
directories (hence the device's `first="GameData.dat"`). It is the first
**non-empty** `first=` in a Windows log, so the `71e09ce` `args[2]` fix is
exercised and the two hosts' lines are now comparable field for field. The
`last=183` on the write lines is `ERROR_ALREADY_EXISTS` from `CREATE_ALWAYS` on
an existing file, not a failure (`ret=1`, `put=7856`). Disk after the run:
`GameData.dat` 7,856 B and `SystemData.dat` 16 B; the next boot opened
SystemData with `last=0`
(`CreateFileA ... disp=3 -> 0x00000780 last=0`). `<Documents>/Korra` was
restored from a backup afterwards.

**The gameplay-save sites, statically (original exe).** The twelve direct
`[0xCA6C28]=3` writes belong to: `hap::HapSaveActionArea` (`0x64EB9C`, method
`0x64EA50`, RTTI COL `0x9988CC` -> TypeDescriptor `0x9EF1F0`); `cProBendingPhase`
(`0x5CA688`, `0x5CA77B`, window messages `0x110`/`0x711`); the `UITitleMenu`
flow (`0x5D3644`, `0x5D37AB`, `0x5D3A05`, methods called by `0x5D2980`); a UI
scene transition (`0x5DD9FC`, `0x5DD861`); `UIStatusViewBending` (`0x5EA4D4`,
called by `0x5E8C30`); `UIShop` (`0x5F1D1F`, called by `0x5EFCA0`); and
`UIAllResult` (`0x5F7A63`, `0x5F9236`). The six `req_save` callers are
`0x4D16D4`, `UIOptionMenu` `0x5D8380` (reason 5), `UIChapterSelect` `0x5E4DF8`
and `0x5E5335`, `0x5FA289`, and `hap::ConditionResult` `0x64F37E`. Every direct
site is the same inlined block: require `[0xE4F44C]!=0` (the save device was
opened), `[0xDB8BB8]==0` (no save in flight) and `[0xCA6C28]==0`, call
`0x632A20` to snapshot the player state, then set `[0xCA6C28]=3`,
`[0xCA6C2C]=0`, `[0xCA6C18]=0`.

`HapSaveActionArea` is the level checkpoint: `0x64EA50` switches on the event id
at `[this+0x14]`; for `0x45ce422b` it requires `[0xCA6C28]==0` and
`[0xCA3D10]&0x4000 == 0` (gameplay, not the menu), copies the player transform to
`0xE3EE00`/`0xE3EE10`, checks the save device and in-flight flags, calls
`0x632A20`, and writes the save. The other event, `0x4b7b6e51`, is the area's
activation branch and does not save.

**Device, the checkpoint exercised.** `sauto3` (New Game, 198 s,
`debug.korra.icall=64EA50`, `debug.korra.watchlog=CA6C28`,
`debug.korra.trace_save=1`) shows the dispatcher calling the area twice and the
save immediately after:

```
[icall] target=0064EA50 from=00641150 esp=C16FFDF0 ... args=C16FFE20 230E18C0 C16FFE0C
[icall] target=0064EA50 from=00641150 esp=C16FFDF0 ... args=C16FFE20 230E18C0 C16FFE0C
[wlog] +00 = 00000003 (was 00000000) from=0063E900
[wlog] +04 = 00000000 (was 00000004) from=0063E900
[wlog] +04 = 00000001 (was 00000000) from=0056BE90
[save] SHGetSpecialFolderPathA ... path="/data/user/0/org.korra.recomp/files/game"
[save] FindFirstFileExA pat=".../game/Korra/*.*" -> 0x02000001 last=0 first="GameData.dat"
[save] CreateFileA path=".../game/Korra/GameData.dat" access=0x40000000 disp=2 -> 0x01000002
[save] WriteFile h=0x01000002 n=7856 -> ret=1 count=7856
```

No `CreateDirectoryA`. The two entries are the area's activation and save
events; the `watchlog from=0063E900` is the next dispatch after the write
(`0x63E900` is a resource constructor), so the **icall probe, not `from=`, pins
the site**. The device file after the run: `GameData.dat` 7,856 B
(2026-11-26 19:59, md5 `8efbde66...`), `SystemData.dat` untouched at 16 B; this
is the level's start checkpoint, i.e. a real gameplay autosave written through
`HapSaveActionArea`, not the Options (reason 5) or the menu's own request.

**Continue after the write.** `scont1` (Resume) boots with SystemData `last=0`,
opens `GameData.dat` `disp=3 -> last=0`, loads the checkpoint, and reaches the
tutorial HUD (`Light Attack / Heavy Attack / Jump`) at 155 s with
`flow=00000009 inst=00000001 step=FFFFFFFF`; the load then rewrites the same
`GameData.dat` 7,856 B at `0x64EA50`. So the autosave round-trips.

**Negative.** `sauto2` then played 481 s (258 held gestures, 19 OCR-verified HUD
frames) in the tutorial and produced **no second** `[0xCA6C28]=3`: the checkpoint
this level activates is at its start, and a mid-level progress checkpoint was
not reached on the measured route. The site (`0x64EB9C`), the dispatcher
(`0x641150`), the event (`0x45ce422b`) and the condition are named, so a longer
or objective-driven run is the next attempt rather than a guess.

**Route fixes on the device driver.** `scripts/android_route_drive.ps1`: the
main-menu action was "stick up to select Story", which wraps the cursor to Exit
and cycles forever (measured, 60 s); Story is the default entry, so it now
confirms with A. The main-menu classifier also accepts the OCR form
`Sta Story Mode` (`storymode`), and a 3 s start wait stops `pidof` reading a
just-launched app as dead (`am start` can print "unknown error code 10" while
launching). `-Resume` presses A on the Story submenu's default Continue.

**Corrections recorded, not silently patched.** The `GameData.*` table row in
the 2026-11-25 entry is corrected: the measured set has no dotless `GameData`, so
Windows returns only `GameData.dat`; the DOS rule matches a dotless one only if
it exists. `71e09ce` is now verified on Windows (`first="."`, a valid
`WIN32_FIND_DATAA` name), with the dot-entry caveat above.

**Not verified.** A checkpoint after mid-level gameplay progress; the Windows
New Game/Continue route (the Windows run above stops at the Options menu, so its
save is the reason-5 one); the exact event id the area receives at call time
(the icall arguments are pointers; the id is read from `[this+0x14]`, and the
class/event path is static); a slot or erase flow (proven absent); the M4 audio
follow-ups (unchanged).


## 2026-11-26 - coordinator verification: the Windows oracle and the checkpoint autosave are accepted

Independently re-checked against the tree and the run logs:

- **Tip and commits.** `main == origin/main` at `ef59f79` ("docs(save): record
  the Windows SystemData oracle and the checkpoint autosave site"), tree clean;
  `530e885` (left stick), `932ea0e` (route driver), `ef59f79` (docs) present;
  files in scope.
- **Windows.** The save lines are in `<game>/korra_host-20261126-193434.log`,
  not in `logs/win_vib1.txt` (which is the driver console and names that game
  log): `FindFirstFileExA ... first="."`, `WriteFile 7856`, `SystemData.dat`
  `CreateFileA` + `WriteFile 16`; the boot log `...193704.log` opens
  `SystemData.dat` `last=0`. `<Documents>/Korra` was restored (original
  mtimes).
- **Device.** `logs/logcat.txt` at 19:59:13 carries `[icall] target=0064EA50
  from=00641150` twice, `[wlog] +00 = 00000003`, `FindFirstFileExA ...
  first="GameData.dat"` and `WriteFile 7856`; the live `run-as` files are
  7,856 B and 16 B; `scont1` reaches the tutorial HUD at 155 s.
- **Negative.** `sauto2` played 481 s with no second `[0xCA6C28]=3`; the sauto3
  game log was overwritten by scont1, so its lines are preserved only in
  logcat. Per-run game logs (`log.txt`) must be copied before the next launch.
- **Selftest/guard.** `scripts\run_selftest.ps1` 60/60; `tools\check_content.py`
  exit 0.
- **Docs.** README status/M3, this entry, `docs/ANDROID_BRINGUP.md` §10.9,
  AGENTS ledger; MASTER_STATE §3 carries the 2026-11-26 block/row and the
  corrected 2026-11-25 superseded sentences. No code changed in this
  verification.

The mid-level checkpoint remains a recorded negative, not a blocker; the static
site map suggests the tutorial level saves at its start only.

## 2026-11-26 - M4 close-out: the Hw fill count is block-granular, the 6 ch values are the engine's own hand-off, and the 6x2 downmix was transposed

Base HEAD `4c1eb88` (one docs commit above the hand-off's `ef59f79`; the tip
subject was verified); the phone was confirmed with `adb devices` at the start
and at each run. Commits `d0ccc32` (the probe: `hw block`, per-channel frame
words and per-channel slice spans, verbose 2, off by default) and `57e730e` (the
matrix order). Logs (gitignored): `logs/m4fix_dev_log.txt` + `logs/m4fix_dev.wav`
(S24, 157 s), `logs/m4close_dev.wav` (the pre-fix device dump, 152 s),
`work/m4close_win.wav` / `work/m4close_win_fix.wav` (Windows 115 s each), and
the two Windows runtime logs in `<game>` (`korra_host-20261126-204548.log`,
`-205132.log`). **Process failure, recorded:** the pre-fix device *log* was
overwritten by the next run's pull (the driver always writes `logs/log.txt`);
its numbers below are from this session's record and its dump survives. Copy
`log.txt` to a per-run name immediately from now on.

**STEP 0c / 1 - the engine's fill bookkeeping is real and block-granular.** The
Hw output device (0x0088B000; callback object `device+0x28`, vtable 0x0091ECC8)
renders one block per mixer tick. `0x008872D0` zeroes `device+0x1E` and calls
`0x00887200`, which mixes and writes the block's frame count there from
`[mixer+0x424]` (configured 0x400 = 1024 at `0x00887343`/`0x008874BF`, also
`0x0088663F`). The fill `0x0088B510` (vtable 0x0091EC80 slot 7) copies exactly
that many frames from the mix buffer at `device+0x10` into the ring slot at the
fill cursor `device+0x44`, clamped by `minps`/`maxps` against +/-2^31 (the
constants at 0x91EC60/0x91EC70); a zero count jumps to slot 8 (`0x0088B5F0`),
which zero-fills the whole slot. The feed `0x0088B440` submits through
`device+0x46` with `AudioBytes = block_align << 10` always. `device+0x68` is
**not** a frame count: each fill `InterlockedIncrement`s it and `OnBufferEnd`
`InterlockedDecrement`s it (IAT 0x8FD014/0x8FD018), i.e. filled-but-unretired
blocks.

The probe logs all of it at every submit. S24, 147 s: **4604 blocks count=1024,
2502 count=0, no other value**; Windows, 115 s: **4102 count=1024, 2133 count=0,
no other value** (the post-fix runs repeat it: 5079/2272 and 3961/2126). Both
hosts therefore show the same geometry: the declared fill boundary is the
*block* - a full 1024-frame block or silence - and never a partial frame count.
The 491-zero/533-NaN and 336-finite/688-x-`409E91BC` tails are inside blocks the
engine's own bookkeeping calls full, so a host "skip the tail" would still be
inventing a boundary. What the count does settle: with count=1024 the fill
rewrites the *whole* slot from the engine's mix buffer, so the stale tail is
copied out of that buffer - the engine's own mixer left part of its block
unwritten for that tick. The fill's skip path (`0x0088B534`, taken when
`[device+4]` and `[0xca28e4]` are both non-zero) and the mixer's block
completion are the two candidates for how; neither is traced.

**STEP 2 - the 6 ch finite samples are committed frames of the engine's own
decode, and device-only.** In the 152 s pre-fix run the slice probe fired three
times; two were the 6 ch CRI movie voice, and every occurrence has the FNV taken
at `SubmitSourceBuffer` **equal** to the one at the probe (`BACE1F9C`,
`319ACCCF`, `6411CEF4`) - the host read exactly what the engine handed over:

| time | voice | slice | geometry |
|---|---|---|---|
| 15.579 s | 6 ch CRI | `[179200,180000)` | channel 0 only, 34 frames `[80..117]`, `C0BFD0C4` (-5.99) x4, max 7.39; other channels <= 0.21 |
| 24.649 s | 6 ch CRI | `[64000,64800)` | **all six** channels, contiguous (c0/c1/c4/c5 `[424..797]`, c3 `[392..772]`), c2 hits exactly **2000.0** (`44FA0000`) at rel 408, c1 max 393 |
| 39.985 s | 2 ch Hw | `[1888960,1889984)` | 29 frames, -2.2..-2.8 inside a count=1024 block (a legitimately hot mix) |

Windows at the same two movies (6 ch voices at 12.9 s and 23.2 s): **0 bad
samples**, dump clips **0 of 12.67 M** samples. The device clips 34276 of
14.59 M (0.2349 %) and, after the matrix fix, 34906 of 15.07 M (0.2316 %) -
unchanged. Per second both hosts play the *same* music at the *same* RMS
(73 s -34.0 vs -33.7 dBFS; 77 s -29.9 vs -29.8) while the device's peaks reach
0.00 dBFS in many menu seconds (74-88 s) where Windows peaks -17..-26: short
full-scale bursts on top of identical music. Those are **finite** stale values
(the NaN guard must not touch them, and does not). The post-fix device run had
its own 6 ch event at 15.273 s (channel 0, 2 frames, `5FD0E000` = 3.0e19), i.e.
the stale region moves between runs - the content is not deterministic decoder
output.

So the 6 ch values are committed frames produced by the engine's movie path,
at the same content Windows plays cleanly; the device's remaining menu clicks
are the same block-granular stale tail with finite content. Decision table,
applied: row 2 (uninitialised content but no sub-block count) and the
committed-frames row - record, no host skip/filter; the underruns are untouched.

**The 6x2 downmix was transposed (host defect, fixed).** The engine installs
`6 x 2 -> 1.000 0.000 0.707 0.000 0.707 0.000 0.000 1.000 0.707 0.000 0.000
0.707`. In XAudio2's documented order (`DestinationChannels x SourceChannels`,
row-major, `level[dest][src]`) that is the textbook downmix L = FL + 0.707 C +
0.707 BL, R = FR + 0.707 C + 0.707 BR; read source-major it put LFE at 1.0 into
the right channel and the front-right speaker into the left. Both shims now
index `matrix[dest * src_ch + src]` (a 2x2 diagonal matrix is unaffected, so the
2 ch CRI/menu path cannot move). A/B, same route and taps: Windows 0 clipped
frames before and after, movie window peak -11.4..-14.1 -> -12.0..-16.6 dBFS and
RMS -25.6..-29.1 -> -24.6..-28.1; device movie window RMS -26.1/-25.8/-27.8 ->
-25.4/-24.0/-28.0 dBFS and total clipping 0.2349 % -> 0.2316 %. No gross
level change (peaks within ~2.5 dB, RMS within ~1 dB); the imaging is now the
engine's.

**Also measured.** The two hosts' `credits` differ in shape (device constant 4;
Windows 1..4) while `qcount` and the block geometry match; the Hw voice feeds
through the boot movies and the menu on both. `scripts\run_selftest.ps1` 60/60
before and after the matrix change; content guard clean on both commits.

**Not verified.** Where the NaN/stale words first enter the engine's mix buffer
(the two candidates above); whether the 6 ch regions are a CRI decode/submit
ordering effect (the unchanged hash rules out "rewritten while queued", not
"incomplete when submitted"); a longer device A/B of the matrix change; the
deferred movie-window underruns; §9.4 4-6 (unchanged).

**One lead on the 6 ch region's contents (observation, not established).** Read
as **int32**, the words of the 24.649 s burst all sit at +/-0.5 FS (c2
`44FA0000` = 0.539 FS, `409F726A` = 0.505, `42135937` = 0.516, `4173FC4A` =
0.511, `C20AF104` = -0.484, `C12CB6D0` = -0.509), while as float they are 2000.0,
4.98, 36.8, 15.25, -34.7 and -10.8. A decoded float waveform does not produce
that int32 clustering; leftover integer PCM in a slot that was not cleared
does. So the next check is whether the 6 ch ring slots are zeroed between
movies (the 2 ch Hw path is: `0x0088B5F0` zero-fills a whole slot on a zero
count), not to filter anything.

## 2026-11-26 (backgrounding/lifecycle session) - HOME no longer kills the process: the bridge now answers D3D9's device-lost contract and the WSI keeps one app window

Base HEAD `f0f0a29` (the expected `64f1522` is in its history, one docs-wording
commit above it; tree clean, `main == origin/main`). Phone `<device-serial>`,
`adb devices` at the start and before each run. Commits `38011aa` (probes +
driver), `1f732ed` (exception support in the guest build), `06ea6d0` (the fix),
then this docs commit.

**The defect, reproduced twice on the pre-fix build (`base1`, `base2`).** HOME
during gameplay, ~15 s, `am start`; both runs are identical:

```
lifecycle: pause
lifecycle: focus lost
window: destroyed
[wsi] unbind win=0xb4000074755f2c80 token=1001 gen=1 surfaces=1
warn:  swapchain: recreate n=3 vsync=0
[wsi] surface create req=1001 cur=0 win=0x0 -> SURFACE_LOST
err:   D3D9SwapChainEx: Failed to recreate swap chain
[life] present win=0 hr=88760870 n=21
[hr] dev:17 -> 88760870 at 00759E70
[life] tcl win=0 hr=00000000 n=21
lifecycle: low memory
[crash] signal 11 code=2 addr=0x71d3420008 guest_func=0x007524C0 ... eax=0
```

Exit-info `reason=2 (SIGNALED) status=11`, a new pid after `am start` (the old
process was gone), `adb logcat -b crash` empty of anything else. The destroyed
object is the shell's `ANativeWindow` (`APP_CMD_TERM_WINDOW`): the presenter
still held the `VkSurfaceKHR` and HWND token built from it, and `[wsi]` shows
the surface request carrying that old token while the WSI has no window at all
(`cur=0 win=0x0`), so `wsi::createSurface` answers `VK_ERROR_SURFACE_LOST_KHR`,
`D3D9SwapChainEx::RecreateSwapChain` throws, and `D3D9SwapChainEx::Present`
converts it to **`D3DERR_DEVICEREMOVED` (0x88760870)**.

**Why 0x88760870 kills it (guest code read, not patched).** `sub_00759E70` -
the engine's Present wrapper, the caller of `dev:17` - branches on success and
exactly `0x88760868` (`D3DERR_DEVICELOST`) at `0x759E94..0x759E9F`; anything
else returns 0, and the caller's teardown releases a render object that is
still referenced: the SIGSEGV at `0x7524C0` is `mov ecx,[esi]; mov eax,[ecx];
call [eax+8]` with a freed `ecx` (`0x3464B0E0` in run 1, `0x3464A160` in the
later run). The engine *does* implement the lost/reset contract: `sub_00759C90`
(its `TestCooperativeLevel`, manages `[mgr+0x6C]`, the skip flag the frame
setup gate reads, and `[mgr+0x74]`) maps `0x88760868`/`0x88760869` onto it, and
`sub_00759D40` calls `IDirect3DDevice9::Reset(pp)` and clears the flag at
`0x759E35` on success. The bridge answered `D3D_OK` from TestCooperativeLevel
unconditionally, so that path was never entered.

**Fix (one behaviour change, three parts; commits below).**
- `dxvk_android/android_wsi.cpp`: **one stable HWND token for the app's life**
  (Win32 keeps one window per app) plus a **bind generation** (`0` while no
  window) for the bridge. DXVK must not see a window *change*: with one token
  per bind, the presenter created a second `vk::Presenter` while the first
  `VkSurfaceKHR` was still alive, and `vkCreateAndroidSurfaceKHR` answered
  `VK_ERROR_OUT_OF_HOST_MEMORY` on the Xclipse driver (measured: that
  intermediate build aborted with `reason=5 status=6`). `createSurface` still
  refuses when no window is bound.
- `src/host_android/d3d9_android.cpp`: `TestCooperativeLevel` = DEVICELOST with
  no window, DEVICENOTRESET until a Reset against a live window succeeds, else
  D3D_OK. `Present` (device and swapchain slots) = DEVICELOST **without
  reaching DXVK** while lost. `Reset` = DEVICELOST with no window; otherwise
  DXVK Reset with the app's token, and the generation is adopted on success. On
  resume DXVK's own surface-loss path rebuilds the swap chain against the new
  `ANativeWindow`.
- `d3d9_android.cpp` + `android/app/src/main/cpp/CMakeLists.txt`:
  `D3D9DeviceEx::Reset` does not catch `dxvk::DxvkError`; the escaping throw
  reached `std::terminate` (`libc++abi: terminating due to uncaught exception
  of type dxvk::DxvkError`, `reason=5 status=6`). Reset/Present now go through
  `d9_call`, which answers DEVICELOST instead of aborting; `korra_guest` gets
  `-fexceptions` (the module's `cppFlags` disable them).

**Proof (device, final build).** `fix4`, `fix5`, `fix6`, `fix9`, `fix10` and
`fix11` - six runs with the fix (three of them with a pause-menu walk before
HOME; `fix11` is the final artifact with a 20 s HOME) and the final artifact.
The sequence is the same every time:

```
lifecycle: pause
lifecycle: focus lost
window: destroyed
[life] present gen=0 hr=88760868       <- DEVICELOST, DXVK never called
[life] tcl     gen=0 hr=88760868
-- 15-22 s on the launcher --
lifecycle: resume
window: created 2340x1080 format=4
[life] census gen=2/1 flag=00000001 need=00000000 ok=00000000  <- engine's skip flag up
[life] tcl gen=2 hr=88760869           <- DEVICENOTRESET
d3d9: Reset 1280x720 fmt=22 windowed=1 depthfmt=75
warn: swapchain: recreate n=3 / n=4    <- DXVK rebuilds on the new ANativeWindow
d3d9: Reset -> 0x00000000
[life] tcl gen=2 hr=00000000
[life] census gen=2/2 flag=00000000 need=00000001 ok=00000001 presents=3040 -> 3450
```

Same pid before and after; screenshots `fix9_19_resume-gameplay.png` (HUD,
"Light Attack / Heavy Attack / Jump"), `fix9_20_pause-after.png` (pause menu:
"Resume / Close the Pause Menu and return to the game / Options / Quit Game /
Back"), `fix9_21_after-pause.png` (HUD again after START), and `fix5`/`fix6`/
`fix9` also open and close the pause menu *before* HOME. An explicit force-stop
after the round trip gives `reason=10 (USER REQUESTED) subreason=21 (FORCE
STOP)` with no new `reason=2`/`5`; `adb logcat -b crash` is empty in every run
(`fix5`, `fix6`, `fix9`, `fix10` crash files are 0 bytes). Audio keeps streaming
through the pause (`[xa2] hw block` every ~25 ms) and the route uses the
Continue checkpoint, so boot/save/audio are exercised, not assumed.

**Windows oracle: real D3D9 behaves the same way.** `scripts\win_focus_oracle.ps1`
runs the current Windows build (`build\korra_recomp.exe`) with `KORRA_GATE=1`,
minimizes the game's device window for 20 s, then restores it. Two runs agree:
`flag=0` before and **during** the absence (Present is not called while the
window is minimized), then **`flag=00000001`** every second for 45 s after the
restore, with the technique slot `ctx30=00000000` - i.e. the engine itself
enters the device-lost/skip state on Win32 focus loss and survives it (the
restore did not take foreground in either run: `foreground` != the game hwnd,
so the exclusive-fullscreen device stays lost). That is the contract the
Android bridge now answers; the device's recovery (flag back to 0 within ~1 s
of the window returning) is the same path with a window that does come back.

**One residual, not hidden.** The first run of the fix (`fix3`; same code as
`fix4`-`fix6`, before the one-second census was added) survived the round trip
but presented nothing for the 50 s the test watched: its first post-resume
Present arrived **77 s** after the window bind, and after a successful Reset
the main thread went back to a steady `sound\stream\*.wem` load pass with
`presents=3273` frozen. Five later runs did not reproduce it (recovery in
1-3 s). Whether that run would have recovered after its load pass is **not
verified**; the parallel on Windows is that flag=1 can persist as long as the
window is not really foreground. `fix9`/`fix10` crash buffers are empty; the
same is true for `fix5`/`fix6`.

**Also measured / no regression.** `scripts\run_selftest.ps1` **60/60** with
the final source; `tests\check_content_selftest.ps1` 6/6; the content guard ran
on every commit. The only Android build change is `-fexceptions` on
`korra_guest` and the two host files; no DXVK patch, no lifted-guest change, no
audio/save/performance code touched.

**Not verified.** A second HOME cycle in one process, a HOME during a loading
screen or a movie, `APP_CMD_TERM_WINDOW` without `APP_CMD_PAUSE`, a long
(>2 min) background with the app's process cached (LMK), the presenter case
where the driver reports a dead window's surface as healthy (then DXVK's
A005 staleness check would keep a dead swap chain), the `fix3` stall's
recovery, and the Windows focus oracle *with* the game really foregrounded
(the restore in both runs left another window in front).

## 2026-11-27 - Stage 0: the Android wait objects now follow Windows semantics, and a host selftest proves it

Base HEAD `f969207` verified by subject (tree clean, `main == origin/main`); the
phone was confirmed with `adb devices` at the start and before each run. Every
device build used the real guest (`gradlew --no-daemon -PkorraLinkGuest=ON
-PkorraWithDxvk=ON -PkorraDxvkSource=<absolute prepared tree> :app:assembleDebug`)
and every log shows `guest: entry 0x0077C3B7`, never `guest: STUB`. Commits
`3b69c12` (harness + state-machine extraction, behaviour unchanged), `8da2caf`
(the semantics fix) and this docs commit. The pinned pcrecomp tree, DXVK and all
lifted guest code are untouched.

### Step 0 - what the engine actually uses (from the probes already in the file)

Budgets in the current host: 120 `wait:` records, 60 `setevent:`, 60
`semrel:`, 40 `close:`, 24 `sem:`. One boot fills the wait/semrel/setevent
budgets, so a row below is "measured in the boot window", not "the whole run".

| primitive | created | waited | released | max n | evidence |
|---|---|---|---|---|---|
| event, auto-reset | not measurable (no create probe; `CreateEventA/W` is silent) | 16 of 120 probed waits (`kind=1 manual=0`) | 60 `SetEvent` records, all `event` | n/a | `wait: h=... kind=1 manual=0 ...`; `setevent: h=... event` |
| event, manual-reset | not measurable (same) | 13 of 120 (`kind=1 manual=1`) | same 60 records, all `event` | n/a | `wait: h=... kind=1 manual=1 ...` |
| mutex | 0 measured (no create probe; no `close: ... mutex`) | **0 of 120** (`kind=2` never appears) | 0 measured (`ReleaseMutex` has no probe) | n/a | absence in all 120 `wait:` lines and all `close:` lines |
| semaphore | **24 per boot**, every one `init=0 max=1` | 91-92 of 120 (`kind=3`), all `ms=4294967295` (INFINITE) | 60 records, every one `n=1` (budget exhausted during boot; `n>1` not observed) | 1 | `sem: h=00000006 init=0 max=1` ... (24 lines); `semrel: h=... n=1`; `wait: h=... kind=3 manual=0 ...` |
| close | - | - | 40 records: 27 `not an object` + 13 `event` (another run: 14 + 3) | - | `close: h=...` followed by `sem`, `event` or `not an object` |
| OpenThread | - | - | 0 calls in these runs (the probe exists; older logcat captures hold 12 and 4) | - | `openthread:` absent |
| WaitForMultipleObjectsEx | - | not measurable (imported per `docs/PE_TRIAGE.md`, but no runtime probe) | - | - | no probe line; call count unknown |

The `sem:` handle map is five pairs plus a second manager: `0x6-0x13`,
`0x1E-0x25`, `0x2D-0x2E` - the asset/file pools of `sub_00741410` (pump,
releases every slot's posted semaphore and waits on its done semaphore) and
`sub_007414E0` (worker, waits `[+8]`, releases `[+c]`), as recorded in the
kernel32 pool note and the 2026-11-21 deadlock entry. A semaphore wait is
therefore the hot path (91-92 of 120 probed waits) and it is always INFINITE;
an event wait is 27-29 of 120.

What no surviving log can answer, and is *not* guessed below: event and mutex
creation counts (no probe), `ReleaseSemaphore` with `n>1` after the boot window
(the 60-record budget is spent before the asset pool runs), and the number of
`WaitForMultipleObjectsEx` call sites (no probe at all).

### The harness (`3b69c12`)

- `src/host_android/wait_objects.{c,h}`: the object table, create/get/close and
  every state decision, extracted from `kernel32_android.c`. The shims stay in
  kernel32 and keep their stdcall ABI and every probe line (including
  `ka_obj_check()`, which remains a lock-free read-only integrity probe). The
  1 ms sleep is a callback: the shim passes `guest_sleep_ms` (which unwinds and
  re-enters the machine lock), the test passes plain `nanosleep`; a non-guest
  pthread never touches the machine-lock unwind. Every state change happens
  under `g_obj_lock`, the lock is never held across a sleep, and the handle is
  re-resolved after every poll (a closed slot is recycled by `ka_obj_new`).
- `src/host_android/wait_selftest.c`: `debug.korra.waittest=1` runs 18
  scenarios before the guest thread starts, one
  `[wait-selftest] <name> PASS|FAIL <detail>` line each, then
  `[wait-selftest] N/M PASS`. Two host pthreads carry the multi-waiter cases;
  joins are bounded (bionic has no `pthread_timedjoin_np`) so a semantics
  failure is reported instead of hanging the boot.
- This commit is a pure extraction: a wait still consumed the signal bit and
  left a semaphore's count alone, `ReleaseSemaphore` had no max clamp and always
  succeeded, and wait-all consumed what it scanned.

Baseline on the S24, unchanged semantics (`logs/stage0_base1_log.txt`):

```
[wait-selftest] S3 FAIL release over max: ret 1, last error 0, count 2 (first ret 1)
[wait-selftest] S5 FAIL each wait consumes one: counts 2 then 2, third wait 258
[wait-selftest] X2 FAIL wait-all: partial 258 kept 0, complete 258 consumed 0/0
[wait-selftest] 15/18 PASS
```

`S3`, `S5` and `X2` name the three defects the plan predicted (no max clamp, no
count consumed, wait-all eating a signal it does not return). The other fifteen
already held under the old code, including the auto-reset one-credit rule and
the manual-reset stay-signaled rule.

### The fix (`8da2caf`) - one line per semantic change

- A semaphore is its count: a successful wait takes exactly one and
  `signaled = count > 0`, test-and-consume under `g_obj_lock` (S5).
- `ReleaseSemaphore` clamps: `count + n > maxcount` answers 0 with
  `ERROR_TOO_MANY_POSTS` (298), leaves the count and `lPreviousCount` alone, and
  a success reports the count before the add (S3, S4).
- `SetEvent`/`ResetEvent` apply to events only and stay one bit; no accumulation
  while signaled (A4).
- A manual-reset event stays signaled until `ResetEvent`; a wait does not
  consume it (M1-M3).
- wait-all checks all objects under one lock and consumes nothing until all are
  ready, then one from each; wait-any consumes only the returned index (X2).
- Mutex ownership is left as it was, with a comment: Step 0 measured zero mutex
  waits, so it is implemented consistently but **not exercised**.

Unchanged on purpose: the 1 ms poll and its machine-lock unwind, the `h == 0`
diagnostic, the thread/file handle paths, `ka_thread_wait`, and the probe
formats. No wait list, no condvar, no broadcast - the 2026-11-22 77 s frame was
a broadcast, and Stage 0 only proves observability through the existing poll.

### Proof ladder

1. **Semantics (device).** `[wait-selftest] 18/18 PASS` with
   `debug.korra.waittest=1`; the three failures above now report
   `S3 PASS release over max: ret 0, last error 298, count 1`,
   `S5 PASS each wait consumes one: counts 1 then 0, third wait 258`,
   `X2 PASS wait-all: partial 258 kept 1, complete 0 consumed 0/0`.
2. **Repeatability.** Two distinct runs of the same APK (`stage0_fix1_log.txt`,
   `stage0_fix2_log.txt`): `18/18 PASS` each, and the scenario lines are
   identical after normalising the measured milliseconds (0 differences, no
   order dependence). `stage0_fix3_log.txt` is a byte-identical copy of fix2
   (SHA-256 `C10D477B8CBDEE90`; fix1 is `101105AE58A669EF`), so it is not
   evidence of a third launch. *Corrected 2026-11-27: the earlier text claimed
   three runs, the third after an explicit `am force-stop`; the two distinct runs
   differ only in the measured milliseconds (0 normalised differences).*
3. **Engine.** `scripts\android_route_drive.ps1` reached the tutorial HUD at
   237 s (`stage0drive_25_gameplay.png`, OCR "Light Attack / Heavy Attack /
   Jump") and played a 62 s campaign (33 gestures, 4 shots). The pool started:
   24 `sem:` creates, 56 threads attached, no `[crash]` line, `adb logcat -b
   crash` empty (0 bytes), our own force-stop gives `reason=10 (USER REQUESTED)
   subreason=21 (FORCE STOP)`, and `files/perf.txt` holds 305 frame-probe lines.
4. **No regression.** Save pair: `GameData.dat` 7,856 B + `SystemData.dat` 16 B.
   *Corrected 2026-11-27: `GameData.dat` was rewritten by this run's checkpoint
   (7,856 B, 2026-11-27 01:26); `SystemData.dat` is present from the 2026-11-25
   Options run (16 B). The earlier text called both files the pair written by the
   checkpoint autosave.* Continue: the Story submenu OCRs "Resume the
   game from where you stopped playing." with Continue as the default entry, and
   pressing it reaches the HUD at 146 s (`stage0cont_14_gameplay.png`), no New
   Game. Hw voice fed all run (14,913 `hw block` lines) and the NaN guard is
   unchanged: it zeroed `7FC00000` words and logged
   `voice 1: non-finite word in the mix (...), zeroed - N so far`. HOME at
   145 s, 20 s on the launcher, `am start`: same pid 4632, HUD back at 191 s,
   the pause menu before and after (Resume / Close the Pause Menu / Options /
   Quit Game / Back), and gameplay again after closing it. Touch reached the
   player: the route's held-stick gestures and the idle-vs-move screenshot pair.
   `scripts\run_selftest.ps1` **60 passed, 0 failed**; the content guard was
   clean on every commit (7 files, then 2).
5. **Stage 0 frame baseline, no claim.** Same route/driver/device/cap
   (`debug.korra.fps=30`, `int=100` in every sample). Gameplay window t=237-319
   (83 s span carried by 75 one-second samples, 566 frames, mean `dt` 146.5 ms,
   6.8 fps = 566 / 83). *Corrected 2026-11-27: the earlier "75 s, 566 frames,
   7.5 fps" read the sample count as seconds; 7.5 is 566 / 75 samples, not a
   rate.*

| bucket | this run (83 s, 75 samples) | 2026-11-22 reference (330 s) |
|---|---|---|
| frame `dt` | 146.5 ms (per-second min 52.8, max 570.0) | 123.9-127.0 ms |
| inside `Present` | 1.80 ms | 1.8-1.9 ms |
| `WaitForSingleObject` total | 136.5 ms | 113.3-116.2 ms |
| - 1 ms `nanosleep` | 55.2 ms | 52.3-54.3 ms |
| - blocked re-entering the machine lock | 81.7 ms | 61.3-62.3 ms |
| residual lifted work | 7.8 ms | 8.4-8.6 ms |
| bridged calls / frame | 7,488 | 6,843-7,493 |
| process CPU / guest threads | 101 % / 56 | 99 % / 56-72 |
| frames > 50 ms / > 100 ms | 566 / 349 | 3,154 / 1,191 of 3,159 |

The first 22 s after the HUD averaged 96.7 ms (sleep 45.3, lock 44.9); the last
20 s 132.7 ms; the cutscene before it 73.0 ms. **No 30 fps claim**: this is an
83 s window (75 one-second samples) with 33 gestures against the reference's
330 s and 186, so the per-second averages move with the scene and the player's
position. Both runs
still show the same shape (Present ~2 ms, waits > 90 % of the frame, one core,
~40 polls/frame), and the sleep and lock buckets follow the frame rather than
one having been exchanged for the other. The Stage 1 session owns the 30 fps
measurement.

### Environment notes (not code)

- The fixed-tap route (`scripts\android_run.ps1 -Route newgame`) did not advance
  past the attract screen on this build: its 0 ms `input tap` gestures are not
  sampled by the poll-based pad. The same route with the driver's 1.1-1.5 s
  holds works (HUD at 237 s). The base build behaved the same, so this is not a
  Stage 0 change; input was out of scope and was not modified.
- The adb link wedged twice after long runs (`adb devices` empty while Windows
  still enumerated the phone, once `offline`); `adb kill-server` +
  `start-server` recovered it both times, and both runs' crash buffers were
  empty, so neither was an app death.

### Not verified

- Mutex waits and ownership (zero measured; no scenario; marked unexercised).
- `WaitForMultipleObjectsEx` engine call sites (no probe; semantics proven only
  by the selftest).
- Event and mutex creation counts; `ReleaseSemaphore` with `n>1` in the engine
  (only `n=1` observed before the probe budget ran out).
- Stage 1 (wake queue), any 30 fps claim, a 330 s campaign on this build, other
  devices, and the Windows long-run. The Windows oracle was not re-run: no
  `src/host` file changed in this stage.

## 2026-11-27 - coordinator verification: Stage 0 accepted after the record was corrected

Independently re-checked before acceptance: `3b69c12`, `8da2caf` and
`d92a4c0` are pushed, `main == origin/main`, tree clean. The base log fails
exactly S3/S5/X2 (`15/18`); two distinct post-fix logs pass `18/18` with zero
normalised differences, and `stage0_fix3_log.txt` is a byte-identical copy of
`fix2` (SHA-256 `C10D477B8CBDEE90` against `101105AE58A669EF`), so the earlier
"three runs" was rejected and corrected in `d92a4c0`. The frame window
recomputes to 566 frames over the t=237-319 span (83 s carried by 75 one-second
samples) = 6.8 fps, not 7.5. Live device: `GameData.dat` 7,856 B (rewritten
2026-11-27 01:26), `SystemData.dat` 16 B (2026-11-25 Options run), the crash
buffer empty. `run_selftest.ps1` 60/60; content guard 128 files clean and its
selftest 6/6. The code was read: a semaphore wait consumes one count under
`g_obj_lock`, `ReleaseSemaphore` clamps at `maxcount` with 298, auto-reset is
one bit, manual-reset stays signaled, wait-all checks then consumes one per
object, the handle is re-resolved after every poll, and there is no wake list,
condvar or broadcast. Stage 0 is closed. Stage 1 (one-signal-one-waiter) is
next; its before/after must use a matched window, because the Stage 0 sample is
61.7 % of frames over 100 ms against the reference's 37.7 %.

## 2026-11-27 - Stage 1 attempt: the one-signal-one-waiter queue works, then kills the boot (reverted)

Base HEAD `33d8442` verified by subject (tree clean, `main == origin/main`); the
phone was confirmed with `adb devices` before every run. Every device build used
the real guest (`gradlew --no-daemon -PkorraLinkGuest=ON -PkorraWithDxvk=ON
-PkorraDxvkSource=<absolute prepared tree> :app:assembleDebug`) and every log
shows `guest: entry 0x0077C3B7`, never `guest: STUB`. No pcrecomp, DXVK, lifted
code or `src/host` change; the six `src/host_android` files were reverted to
`33d8442` at the end of the session, so **none of the queue ships**.

The result in one line: the queue removes the shim cost completely (the 1 ms
sleep bucket 53.5 -> 0.6 ms/frame, the machine-lock re-entry bucket 58.2 -> 3.1
ms/frame) and the tutorial frame falls 119.8/106.5 -> 85.8 ms in the matched
window, but it misses the 33.3 ms target, **and 2 of the 5 launches of the queue
build died with SIGSEGV in guest code**. Reverted in full as a losing fix
(AGENTS section 6).

### Step 0 - the engine's waits, counted from the probes

The existing probes (not new code) answer what the engine waits on; the only
thing that needed a probe was `WaitForMultipleObjectsEx`.

- **120 probed waits: 90 semaphore (`kind=3`), 30 event (`kind=1`), 0 mutex**,
  all `ms=4294967295` (INFINITE). 24 semaphores created per boot, every one
  `init=0 max=1`, handles `0x6-0x13`, `0x1E-0x25`, `0x2D-0x2E`. The worker
  `sub_007414E0` waits on `0x8-0x12` (`wait: rep ... caller=0x007414E0`); the
  render/menu thread waits on events `0x2-0x5`, `0x19`, `0x1B`, `0x26`
  (`wait: rep ... caller=0x008D1F78`).
- **Per frame on the presenting thread** (matched baseline window, below): 44.3
  `nanosleep(1)` calls and 9.73 `WaitForSingleObject` calls that polled; the
  sleep histogram's largest requested value is 1 ms, so every one of them is the
  engine's wait poll, not the frame limiter.
- **`WaitForMultipleObjectsEx` has 32 static call instructions** in the generated
  C (IAT slot `0x8FD024`: 35 references = 32 `RECOMP_ICALL(MEM32(0x8FD024))` +
  3 `mov reg, MEM32(0x8FD024)` before an indirect call), in 32 distinct lifted
  functions. **At runtime it is a boot/loading path**: the Stage 1 probe logged
  the first 20 calls, every one `count=2|3 waitall=0 ms=INFINITE`, the last of
  them by `t=77 s`; no `n=10000` line appeared in a 336 s run, so fewer than
  10,000 calls in that run (<= 30/s) against ~13 waits/frame on the render
  thread. The multi-object wait therefore keeps the 1 ms poll as the correct
  fallback, and the queue leaves it alone.

### The wake channel, decided before coding

A **per-object condvar (CLOCK_MONOTONIC) plus a waiter count**, not a counting
token: a token can outlive a timed-out waiter and drift above the waiter count
(the documented over-wake failure), while a condvar has no token to drift, and
`pthread_cond_signal` cannot over-wake. The state, not the wake, carries the
signal: the waiter tests and consumes under the object lock, enqueues, drops the
lock, releases the full machine-lock recursion through the shim's gate,
re-takes the lock and **re-tests immediately before blocking**, so a set that
lands in the window is consumed by the re-test instead of being lost. Wake
budget: auto-reset one signal if `waiters > 0`, manual-reset one broadcast (the
only all-wake), semaphore `min(released, waiters)`, reset none, close a
teardown broadcast (the waiter re-tests, finds no object and answers 258).
`INFINITE` is a 1 s slice loop so the shim's 2000 ms "main thread still waiting"
probe keeps its trigger without a 1 ms poll.

### The queue (`src/host_android/wait_objects.{c,h}` + the shim's gate)

Implemented as described, with the machine-lock plumbing in
`kernel32_android.c` (`wait_gate_unlock`/`wait_gate_relock`, the same full-depth
unwind `guest_sleep_ms` uses) and a new frame-probe bucket `blk` for the time a
wait is parked on the wake channel. `wait_selftest.c` gained Q1-Q4 (no lost
wake through a test-only pre-block hook, 9 waiters/8 sets -> 8 wakes and 1
timeout, `release(3)` with 2 waiters -> 2 wakes and count 1, and 1,000 set/wait
round trips over two auto-reset events); the probe prints one `[waitq]` line a
second with the cumulative wait/wake counters. **Semantics were correct in
isolation**: `[wait-selftest] 22/22 PASS` on two distinct launches, identical
after normalising the measured milliseconds (0 differences), with
`Q1 hook 1, wake 0 at 0 ms`, `Q2 8 woke, 1 timed out, worst set->wake 2 ms`,
`Q3 count 3->1, next wait 0, count 0`, `Q4 2000 round trips in 19-27 ms` and a
cumulative `spurious=0` over 2,038 queue waits. Nothing here lost a wake.

### The matched baseline (Stage 0 build) and the queue's frame

Same route (`android_route_drive.ps1`), same window definition (the play
campaign the driver starts at the tutorial HUD: `-PlaySeconds 120`, fps cap 30,
`int=100` in every sample), same device. The window is anchored in the probe's
own clock by mapping the driver's elapsed time through the thermal anchors and
the `Korra` logcat timestamps, not by assuming the two clocks agree.

| | base1 | base2 | queue (fix2) |
|---|---|---|---|
| window (perf `t=`) | 143-262 (120 s) | 148-269 (122 s) | 217-338 (122 s) |
| one-second samples / frames | 112 / 1,004 | 115 / 1,147 | 116 / 1,429 |
| gestures in the window | 69 | 66 | 45 |
| frame `dt` | **119.8 ms** | **106.5 ms** | **85.8 ms** |
| per-second p10/p50/p90 | 84.1/105.2/241.7 | 74.2/93.0/233.5 | 38.4/135.3/172.3 |
| per-frame min/max | 55.8/363.0 | 53.2/363.2 | 18.5/288.6 |
| `Present` | 1.35 | 1.14 | 1.17 |
| `WaitForSingleObject` | 111.25 | 99.22 | 75.17 |
| - 1 ms `nanosleep` | **53.50** | **51.55** | **0.63** |
| - blocked re-entering the lock | 58.16 | 47.99 | **3.13** |
| - blocked on the wake channel (`blk`) | - | - | **72.76** |
| residual lifted work | 6.74 | 5.82 | 8.08 |
| bridged calls/frame | 7,680 | 7,465 | 7,068 |
| frames >50 / >100 ms | 100 % / 41.1 % | 100 % / 29.7 % | 53.2 % / 35.8 % |
| fps (frames / span) | 8.37 | 9.40 | 11.71 |
| process CPU / threads / `int` | 99 % / 56 / 100 | 93 % / 56 / 100 | 123 % / 56 / 100 |

The two baseline rows are 12 % apart, so both are reported; both queue-vs-
baseline comparisons are made row by row. Distribution of the same window:

| window | seconds <=33.3 ms | 33-50 | 50-100 | >100 |
|---|---|---|---|---|
| base1 | 0 | 0 | 51 | 61 |
| base2 | 0 | 0 | 68 | 47 |
| queue | 0 | 26 | 12 | 78 |

Two sub-windows matter more than the average, because the campaign's frame cost
is scene-weighted and the player covers the same gesture cycle at a different
rate when the frame rate changes (69 gestures/120 s at 8.4 fps, 45 at 11.7):

- **Menus** (t=80-200 queue vs t=80-130 baseline): 39.3 ms vs 51.7 ms, sleep
  7.43 vs 25.19, lock 1.83 vs 21.31; 25.5 vs 19.1 fps.
- **Light part of the area** (queue, t=309-338): 41.8 ms (24.4 fps), `blk` 34.75,
  lock 1.09, residual 4.47, 11.7 % of frames >50 ms.
- **Streaming/loading part** (queue, t=233-307): 135-195 ms, `blk` 115-165,
  calls/frame 7,000-10,000.

So the poll and its lock re-entry are gone (that half of the plan is proven),
the frame is 24-28 % faster in the matched window and ~2x faster in the menus,
and **the 30 fps target is not met**: the remaining 72.8 ms is the render thread
parked on its workers, i.e. the serialized worker work behind the global machine
lock - Stage 2's problem, not Stage 1's. The engine never had more than one
waiter on an object (`[waitq] peak=1` over 954,091 waits), so a per-object
signal is structurally a wake-one and the 2026-11-22 broadcast class cannot
recur.

### The failure that reverted it

**2 of 5 launches of the queue build died with SIGSEGV**, both in `src/recomp`
generated guest code, both `reason=2 (SIGNALED) status=11`:

| run | died | fault | symbolized site | what the registers say |
|---|---|---|---|---|
| `s1fix1` | t=106 s, 5 s into the menu -> cutscene load | `signal 11 code=2 addr=0x71d4e75004`, `guest_func=0x00829560`, `esi=0` | `recomp_0054.c:92924` `edx = MEM32(esi + 4)` | the 2nd pushed argument of `unlink_00829560` is **NULL** - the link at the freed block's header. Refined by the 2026-11-27 forensics entry below: the path is the *pool* free (`sub_00828D80 -> sub_00829880 -> unlink_00829560`), not a generic heap walk |
| `s1camp` | t=79 s, right after the Story submenu's New Game press | `signal 11 addr=0x728a61ca80`, `guest_func=0x008AF460`, `edx=0xC0FFFA80`, `ecx=0xBD5A0000` | `recomp_0063.c:8934` `MEM32(edx + ecx * 4) = eax` | `ecx` is an out-of-range index into the caller's `_alloca` scratch array, and it is a **float bit pattern**. Refined below: the shape is a canonical-Huffman code constructor walking its own length table; the register globals lag the faulting instruction, so the values are the pre-call ones |

Both crashes are *inside lifted guest code*, the object table was intact
(no `obj: entry` line), **no wait failed or timed out** in either run
(`[waitq] timeouts=0 fail=0 spurious=0`), and the crash buffer was empty (the
guest's own handler wrote the lines above). The other three launches of the same
build (two selftest launches, 40 s each, and `s1fix2`, 336 s) survived; the two
baseline launches survived. The queue's own semantics are not implicated by any
counter - what changed is *when* guest threads run.

**Hypothesis at the time (now tested, see the forensics entry below):** the 1 ms
poll was acting as a latency floor. With it gone, the menu/render loop runs 2-3x
faster while the asset pool and its file reads do not, so the engine consumes
state the loader has not published yet. The two fault sites are broadly
consistent with "speed exposes a pre-existing race", but the supporting evidence
quoted here was wrong: the `slot: fn=00000000` dumps are **not** pool slots (they
are the engine's per-thread `CreateThread` parameter blocks, see the correction
below), so they show nothing about a pool wake. The forensics session reproduced
crash 1 at 100 us with **no queue code in the tree**, which settles that the
queue is not the root cause - the 1 ms floor is what hides it. A real fix still
needs the race named (see the new entry).

### Reverted, and the reverted build checked

`git checkout` of the six `src/host_android` files, rebuilt with the same flags,
`adb install -r`, then:

- **Boot and save resume**: the Story submenu's Continue (its default entry)
  opened `SystemData.dat` (16 B) and `GameData.dat` with `last=0`, rewrote
  `GameData.dat` (`disp=2`) and reached the tutorial HUD at 101 s
  (`s1cont_09_gameplay.png`), no crash - i.e. the save the *queue* build's
  checkpoint wrote (7,856 B at 03:05) round-trips on the reverted build.
- **Audio**: the Hw voice is fed in both builds (16,613 `hw block` lines in the
  336 s queue run, 7,840 in the Continue run), and the NaN guard is unchanged -
  it zeroed `7FC00000` words in voice 1 (`non-finite word in the mix (...),
  zeroed - N so far`, 12 in the queue run, 6 in the Continue run).
- **HOME -> resume with the pause menu**: pause menu at 136 s (Resume / Close the
  Pause Menu / Options / Quit Game / Back), HOME at 136 s, 17 s on the launcher,
  `am start` -> **same pid 8535**, HUD back at 178 s, the same pause menu after
  (`s1life_18_pause-after.png`) and gameplay again after closing it.
- **Windows `scripts\run_selftest.ps1`: 60 passed, 0 failed.**
- **Install**: `adb install -r` of the rebuilt APK + the internal game tree
  (marker + `LoK.exe` + both CPKs) is what every run above booted from; the SAF
  picker path itself was not re-run (it would wipe the save pair) and is
  untouched by this session.
- Content guard clean on the docs commit.

### Not verified / open

- **The 30 fps target is not met** (85.8 ms in the matched window; 41.8 ms in
  the light part of the area). The measured residual is the render thread
  blocked on serialized worker work, which is the Stage 2 question.
- **The two SIGSEGVs are not explained**, only sited and symbolized. They are
  the reason the queue does not ship. 5 launches is a small sample.
- A >=300 s campaign on the queue build: the campaign launch (`s1camp`) crashed
  at 79 s, so there is no queue-build campaign row (min/avg/max fps, RSS,
  thermals).
- The `WaitForMultipleObjectsEx` runtime count is bounded only as ">= 20 and
  < 10,000 in 336 s"; the probe logged the first 20 only.
- Mutex waits (still zero measured), other devices, 60 fps, and the Windows
  long-run are untouched.

## 2026-11-27 - Stage 1 forensics: crash 1 is pre-existing, crash 2 is pre-existing by disassembly, and the 1 ms poll was hiding them

Base `6eec79e` verified by subject (clean tree, `main == origin/main`), device
`<device-serial>` (SM_S921B) confirmed with `adb devices` before every run. Work
is on branch `stage1-forensics`: **diagnostics only** - no queue, no guest
patch, no pcrecomp/DXVK/WSI/audio/save change. Every launch used the real guest
(`guest: entry 0x0077C3B7`, never `guest: STUB`).

The result in one line: **the 1 ms poll was the mask, not the queue.** Crash 1
(`0x00829560`) reproduces with **no queue code in the tree** once the poll's
sleep is shortened to 100 us (1 of 5 launches), and 0 of 5 launches at the
shipping 1 ms reproduce it. The queue is an accelerator of a pre-existing guest
race, not its cause. Crash 2 (`0x008AF460`) was **not** reproduced without the
queue in this session; it is a pre-existing *path* by disassembly only.

### The diagnostics that made this answerable

Two commits on `stage1-forensics` (both inert in a normal run):

- `3b3ad5c` - the crash handler prints `guest_tid`, the thread's
  `CreateThread` parameter block, `[ebp+4]`, eight dwords at `esp`, the
  eight stdcall argument slots at `[ebp+8]` and `ka_ring_dump("crash")`;
  every read is range-checked against the image, the 1 GiB heap window and the
  stack region first, so a bad read cannot kill the tombstone.
- `7e00bb0` - `debug.korra.wait_us` (default 1000) replaces the poll's 1 ms
  sleep in `k_WaitForSingleObject`/`k_WaitForMultipleObjectsEx` with a
  microsecond sleep (0 = `sched_yield`), with the same full machine-lock
  unwind; `k_Sleep` stays in ms. The slot probe now requires `MEM32(param)`
  to be a heap pointer, i.e. a real pool slot.

The device `.so` is unstripped and was built in this session, so a crash
`pc`/`lr` resolves exactly (`llvm-addr2line` over
`build/intermediates/cxx/Debug/1z725m3q/obj/arm64-v8a/libkorra.so`). That is
the difference between "symbolized by eye" and "the line the fault is on".

### Crash 1, named end to end

Reproduced as `s1w100_4`: `wait_us=100`, t=294 s (guest perf clock),
mid-play in the first area, `reason=2 status=11`:

`[crash] signal 11 code=2 addr=0x71d5473004 guest_func=0x00829560 esp=0xC0EFFE98 ebp=0xC0EFFEA4 eax=0 ecx=0x24 edx=0x10000 ebx=0x2241B9E0 esi=0 edi=0x3FC90FD7`

| item | evidence |
|---|---|
| fault instruction | `pc = libkorra+0x9728570` -> `sub_00829560`, `recomp_0054.c:92924` = `edx = MEM32(esi + 0x4)` |
| fault address | `addr = libkorra+4` (`libkorra base = pc - off = 0x72DD489000`), i.e. `[NULL+4]` |
| function | `sub_00829560` = the engine allocator's **unlink**: bin index from `[node+4] & ~3` (`cmp 0x80` / `bsr` / `shr`), then `[node+0xC]->+8` / `[node+8]->+0xC` list surgery and a treemap store at `[pool + idx*4 + 0x74]` |
| caller | `lr = libkorra+0x9722760` = the return from the first `bl sub_00829560@plt` at guest `0x008298B5`, inside `sub_00829880` (exactly one caller: `0x00828DB5` in `sub_00828D80`) |
| caller role | `sub_00828D80` = **pool free**: `EnterCriticalSection([0xCA1EF0] + arg0*0x48 + 0x20)`, size := `sub_008297E0(block)`, `[pool+0x40] -= size+4`, `free(pool->region [edi+0x38], block)`, `LeaveCriticalSection` |
| free role | `sub_00829880`: `block==0 -> return`; `chunk = block-8`; `size = [chunk+4]`; if `size & 2` then `node = [chunk]` and `unlink(pool, node)` - **with no NULL check on node** (the original bytes have none: `008298B1 mov esi,[esi] / 008298B3 push esi / 008298B5 call`) |
| bad value | `node = 0`: the freed block's header dword at `+0` is NULL while its size word has bit 1 set. The stack dump confirms it independently of the register file: `[ebp+0xC] = 0` at the callee |
| thread | `guest_tid=4109`, `param=0x221212BC`, created at boot as `thread: create va=0087EEE0` (the game's loader thread; its proc calls an import through `[0x8FD0B8]`, stores `[0xCA291C]`, then loops). `esp=0xC0EFFE98` = the 15th 1 MiB guest-stack tile (`0xC0E00000..0xC0F00000`), ~0x1A8 below its top |
| ring | `EnterCriticalSection`/`LeaveCriticalSection` at `0x00828D80` (plus `0x00767F50`, `0x00764BD0`) - the pool-free path, as the caller says |
| expected invariant | a chunk whose size word carries bit 1 must have a non-NULL `[chunk]` link; the engine hands `free` a block whose header link is 0 |

Nothing in the wait layer is implicated: no `obj: entry`, no `notobj`, no
`h=0` wait, crash buffer 0 B, and this build has no queue at all.

### Crash 2, re-read

`s1camp` (queue build, t=79 s, the moment the Story submenu's selection moved
to New Game) faulted at `0x008AF460`, `recomp_0063.c:8934`
`MEM32(edx + ecx*4) = eax`. Read from the bytes, not from the dump:

- `sub_008AF460` is a **canonical-Huffman code constructor**, not a string
  formatter: `arg0` is a byte array of code lengths (read until 0),
  `arg1` is the symbol count (`< 2` returns 0), `arg2` is the output array
  of 8-byte `{code, len}` records, and the function keeps a 33-entry
  `next_code[]`/count table in its own frame (`[ebp-0x98 ..]`, loops bounded
  by `0x21`), computes `next_code[len] = (next_code[len-1]+count[len-1])<<1`
  and writes each code OR'ed with `0x80000000`; `sub_008AF443` is its 16-bit
  sibling (`or ax,0x8000`).
- caller `sub_008AF0F0` (xrefs `0x008AFFC1`, `0x008B018E`): checks
  `[edi+0x14]==4`, then calls `sub_008AF460` twice - once with a heap
  scratch from `[ebx+4]`, then with an `_alloca(8*[edi+8]-8)` **stack**
  buffer (`mov esi,esp`) passed as `arg2`. The crash's `[ebp+0x10]`
  (`0xC0FFFA80`) is exactly that `esp`, so the second call is the one that
  died.
- the index is `ecx = [ebp-0xC] + 2*esi` with `esi` itself loaded from
  `arg2[2*esi+bit]`. Every read follows a write of the same slot *provided*
  the code-length byte is in 0..32; a byte outside that range reads past the
  33-entry local table into the function's own locals/argument slots, and the
  walk then indexes the caller's alloca buffer out of range - which is where a
  recycled stack's leftovers (floats: `0xBD5A0000`, `0x5EAD0000`) become an
  array index.

**The register dump cannot be read as the faulting operands.** The generated
code keeps values in host registers between sync points, so the file lags the
instruction: in `s1camp` `g_ebx` holds a float the function never had, and in
`s1w100_4` the pushed `edi` is `0x3FC90FD7` (1.5707964f = pi/2). What is
exact is the host `pc` (symbol + line) and the fault address; the stack's
*argument slots* are the values the callee actually reads.

### The control and the sweep

Same route (`android_route_drive.ps1`, cap 30, `int=100` in every sample),
same device, same build (only the `wait_us` property differs between the two
groups; one launch per row, ~300-390 s wall, `perf t` = guest probe clock).

| run | wait_us | guest `[crash]` | site | perf t | frames |
|---|---|---|---|---|---|
| s1c1 | 1000 | 0 | - | 294 | 19,261 |
| s1c2 | 1000 | 0 | - | 326 | 20,945 |
| s1c3 | 1000 | 0 | - (one 64 s frame, a separate stall) | 328 | 23,165 |
| s1c4 | 1000 | 0 | - | 257 | 21,608 |
| s1c5 | 1000 | 0 | - | 293 | 18,989 |
| s1w100_1 | 100 | 0 | - | 282 | 18,649 |
| s1w100_2 | 100 | 0 | - | 335 | 21,439 |
| s1w100_3 | 100 | 0 | - | 365 | 23,270 |
| **s1w100_4** | **100** | **1** | **0x00829560** | 294 | 19,320 |
| s1w100_5 | 100 | 0 | - | 285 | 18,326 |

With the previous session that makes **0 crashes in 9 launches at 1 ms** and
**1 crash in 5 launches at 100 us**, and the 100 us crash is the *same site* as
`s1fix1` in the queue build. The 0 us set was not run: 100 us was already not
clean, and the plan made it conditional on 100 being clean.

That the knob acted is visible in the probe's sleep bucket (mean over
`perf t=150..280`): 57.2 / 54.8 / 46.0 / 60.8 ms at 1000 us against 27.0 /
30.8 / 24.2 / 25.3 / 35.3 ms at 100 us - the groups do not overlap. `dt` does
not separate (102-199 ms in both groups), so **no performance claim** is made
here; the frame probe's sleep histogram is ms-keyed and therefore cannot show a
sub-millisecond quantum.

### The `slot:` dumps, corrected

The Stage 1 entry's sentence "the `slot: fn=00000000` dumps ... show the pool
already being woken before a slot's job function is filled" was **wrong and is
withdrawn**. The probe read `ka_thread_self_param()`, the engine's per-thread
`CreateThread` parameter block, whenever that block sat in the heap. It does:
`param = 0x34627140 + 0x1400*n` is exactly the `thread: create ...
param=` sequence (0x34627140, 0x34628540, 0x34629940, ...), and
`MEM32(param) = 0x1000` is not a heap object. So `cap`/`post`/`done`/
`fn`/`arg` were fields of a different struct, `fn=0` was that block's
`+0x10`, and the identical 400 lines (the cap) appear in the *baseline* runs
too. With the corrected guard (`MEM32(param)` must be a heap pointer) the old
400-line artifact is gone, but the count is **not** 0: `s1c2/3/4` and
`s1w100_1/2/4/5` each report 400 real pool blocks (`mgr=0x20000970`, `cap=4`,
`post=0xD36`, `fn=0`), while `s1c1`/`s1c5` report 0. They occur in the stable
1 ms control runs as well, so they are **not crash-specific**; whether they are
the asset or file pool, and whether `fn=0` is normal at that point, is **open**.
They still say nothing about publish order.

### Conclusion

- **The queue is not causal.** Crash 1 reproduces without a line of queue code
  at a shorter poll quantum. Crash 2 sits on a pre-existing *path* by
  disassembly (a code-length table walk) but was not reproduced without the
  queue. Neither input is a wait result. Every queue counter was clean
  (`timeouts=0 fail=0 spur=0 peak=1`), the object table was intact, and the
  crash buffer was empty.
- **The 1 ms poll is the mask.** It is a 1 ms latency floor on every wake; remove
  it (queue *or* `wait_us`) and a pre-existing guest race gets a much wider
  window. The mechanism is not yet named: the fault is a NULL free-list link in
  the pool, and the thread is the boot-created loader `0x0087EEE0`.
- **A same-class pointer/float collision appears without the queue:** a
  backgrounded launch of the diagnostics build (not a route sample) died at
  `0x007E63C0` dereferencing `[arg0+4] -> [0+0x1C]` with
  `arg0 = 0x41EFEFD2` (30.0f) read from an object field. 1 of 1 in that
  invalid launch, and it did not recur in the 10 route runs. Reported as an
  observation, not a result.

### Recommendation

Do not retry the queue and do not start Stage 2 yet. The next block is to name
the pool race with the tool that now exists: keep `wait_us=100` (cheap
accelerator, 1/5), and add **one** probe at the pool free entry
(`0x00828D80`) that logs `arg0` (pool index), the block, `[block-8]`,
`[block-4]` and the pool's `[+0x38]`/`[+0x40]` before the free, plus the
same block at the *next* allocation of that size. A block whose header link is 0
at free time but non-zero at allocation time is a double free or a
use-after-free; a block that is 0 at both is an uninitialised header. That
distinction names the layer (guest engine vs host) without touching the queue.
Then run the same load on the Windows oracle at `KORRA_WATCH` around
`0x00828D80` to see whether the guest produces the same header there.

### Not verified / open

- The race's mechanism is **not named**; only the site, the caller chain, the
  thread and the masked-vs-unmasked behaviour are.
- 1/5 at 100 us against 0/9 at 1 ms is a small sample (Fisher exact p ~ 0.36);
  the *rate* is not established. What is established is that the crash does not
  need the queue.
- Crash 2 was not reproduced in this session (no queue build, and the 0 us set
  was not run), so its index's origin is derived from the disassembly, not from
  a fresh fault with the new probes.
- The `wait_us` sweep changes *finite* timeout durations (one poll counts as
  1 ms in the state machine); every wait on this route is INFINITE, so no
  measured answer changed, but the knob is not a shipping candidate.
- The 30 fps target is untouched by this session; the Windows oracle comparison
  and the crisper pool probe are the next step.

## 2026-11-27 - Pool-free probe: the header invariant holds at every observed free, so the crash is a rare timing state, not a routine double free

Base HEAD `f9278d0` (privacy `2057f2e` plus the record correction) verified by
subject; the probe is branch `stage1-poolprobe`, commit `8eed187`, **diagnostics
only** (no queue, no guest patch, no pcrecomp/DXVK/WSI/audio/save change).
Device `<device-serial>` confirmed with `adb devices` before every run; every
launch used the real guest (`guest: entry 0x0077C3B7`, never `guest: STUB`).

**The probe** (`src/host_android/pool_probe_android.c`, behind
`debug.korra.poolprobe=1`): at the pool free's own `EnterCriticalSection`
(`g_cur_func == 0x00828D80`) it reads `[ebp+8]` (pool index), `[ebp+0xC]`
(block), `[block-8]`, `[block-4]`, `[block]`, the record
`[0xCA1EF0] + idx*0x48`, `[pool+0x38]` (region) and `[pool+0x40]` (free
bytes); it derives the unlink's bin from `([block-4] & ~3)` and walks
`[region+0x74+bin*4]` (fd at +8, region as sentinel) to test whether the chunk
is already on its free list. The last 256 frees are kept in a ring and dumped
by the crash handler. Host-only, no relift.

**One field is only valid when it is marked valid.** The first probe version also
dereferenced the header link `[block-8]` to read the previous chunk's size. In
`sub_00829880` that dword is the previous physical chunk **only when bit 1 of
the size word is set**; otherwise it is don't-care. On a free with bit 1 clear
the probe followed `0x20000002` and faulted at guest `0x20000006` (heap base+6,
an uncommitted page). That was the probe, not the guest. Fixed: the link is
recorded as data and only checked for plausibility (4-aligned, heap, below the
chunk), never dereferenced.

**Header state, 100 us against 1 ms** (same route, cap 30, `int=100`, one launch
per row; `frees` = `[poolprobe]` lines; `bit1_bad` = size bit 1 set with an
implausible link; `self` = the chunk was already on its bin):

| run | wait_us | frees | size bit1 | bit1_bad | link 0 | link0+bit1 | self | guest crash |
|---|---|---|---|---|---|---|---|---|
| w100_1 | 100 | 4,000 (cap) | 1,164 | 0 | 338 | 0 | 0 | 0 |
| w100_2 | 100 | 26,925 | 8,971 | 0 | 1,418 | 0 | 0 | 0 |
| w100_3 | 100 | 32,650 | 10,468 | 0 | 1,687 | 0 | 0 | 0 |
| w100_4 | 100 | 31,230 | 10,367 | 0 | 1,234 | 0 | 0 | 0 |
| w100_5 | 100 | 31,101 | 10,358 | 0 | 1,721 | 0 | 0 | 0 |
| w100_6 | 100 | 24,367 | 7,667 | 0 | 1,096 | 0 | 0 | 0 |
| w100_7 | 100 | 23,036 | 7,402 | 0 | 1,113 | 0 | 0 | 0 |
| w100_8 (partial, 131 s) | 100 | 13,101 | 4,186 | 0 | 949 | 0 | 0 | 0 |
| w1000_1 | 1000 | 23,273 | 7,707 | 0 | 1,033 | 0 | 0 | 0 |
| w1000_2 | 1000 | 22,003 | 6,522 | 0 | 1,102 | 0 | 0 | 0 |
| **total** | | **231,686** | **78,812** | **0** | **10,691** | **0** | **0** | **0** |

A zero link is **normal** when bit 1 is clear: it says the previous chunk is in
use, so the field is not read. Every free with bit 1 set (78,812 in total, of
which 60,583 at 100 us) had a plausible 4-aligned heap link, and no chunk was on
its size bin at free. So in every observed free the header invariant the unlink
needs (`bit 1 set => non-NULL valid link`) held.

**Rate.** 0 crashes in 8 launches at 100 us (seven full ~400 s runs and one
partial) and 0 in 2 launches at 1 ms. With the previous session that is **1
crash in 13 launches at 100 us** and **0 in 11 at 1 ms**. The crash did not
reproduce here, so the probe did not capture the faulting free's own header.

**Windows oracle.** `build\run_poolprobe.exe` (the `-Trace` build) with the pad
timeline reached the same Continue load: `Korra/GameData.dat` opened
(`disp=3`, then `disp=2`, `last=183`), `[scene] flow` went
`9 A B C D 10 11` and then `12 14 16`, and `player=0x0E142B60` was non-NULL.
`--watch 0x00828D80 --watchspan -8 16` gives `[ecx-0x20..]`; on entry ecx is
the block only sometimes, so the header is readable only when `ecx == args[1]`:
**268 such frees, 7 with bit 1, all links plausible, 3 link-0 with bit 1 clear,
0 invalid** - the same state as the device. The watch itself then faulted at
86.7 s reading `[ecx-0x20]` with `ecx=C0000002` (a wild register, not the
block), so that tombstone is instrumentation, not a guest fault.

**Conclusion.** Not a routine double free (0 of 186,410 device frees and 0 of
268 Windows-readable frees were already on the free list) and not a routine
uninitialised header (a zero link is legal exactly when bit 1 is clear; every
bit-1 free had a valid link). The measured state fits the *timing window* row:
the header is normal at every ordinary free and the fault state appears only in
the rare timing that also crashes. Keep the 1 ms poll as the shipping workaround
(0/11); the queue stays out. Candidate next step: a host-side guard at the free
that, when bit 1 is set and the link is implausible, logs and clears bit 1
before the guest unlinks (still no guest patch), plus a watch on `[block-8]`
from allocation to free to name the writer. **Not verified:** the faulting
free's own header (not captured), the writer, the rate (1/13), Stage 2, 30 fps.

## 2026-11-27 - Pool writer watch: the header is correct at allocation and at the free entry in 303k device frees, the fault state is proven sufficient, and the candidate guard is proven to skip it

Base: `f9278d0` (the branch point; main's docs tip was `b2bd13e`). All probe
work on branch `stage1-poolprobe` (`22b4676`, `0789f56`, `07409a9`,
`19255d3`, pushed),
**diagnostics only** (no queue, no Stage 2, no guest patch, no
pcrecomp/DXVK/WSI/audio/save change). Device `<device-serial>` (SM-S921B) confirmed
with `adb devices` before every run; every launch logged `guest: entry
0x0077C3B7`, never `guest: STUB`.

**The Windows watch was biased and is fixed.** The earlier oracle used
`--watch 0x00828D80 --watchspan -8 16`, i.e. the object under ECX; on entry ECX
is only sometimes the block (the log has `ecx=00000001`, `ecx=C0000002`), so its
268 readable frees were the callers that happened to leave the block there, and
the span dump itself died at 86.7 s reading `0xBFFFFFE2` (instrumentation, not
guest state). `src/host/pool_watch.c` (`KORRA_POOLWATCH=1`, trace build) reads
`args[1]` = MEM32(esp+8) and guards every span dword with `VirtualQuery`; it
also snapshots the header at the allocator's own `LeaveCriticalSection`
(`sub_00829000`, block in EDI). **Unbiased Windows sample:** 12,500 frees across
two runs (11,500 + 1,000), **4,107 with size bit 1, every one a plausible and -
on the 1,000-free run, where the strict test was live - the true previous
physical chunk (105/105)**, 845 zero links all with bit 1 clear, 0 invalid, 0
size-0. The Continue load was **not** reached in either run (the flow stayed at
the main menu; `GameData.dat` opened disp=3 then disp=2, the autosave), so the
last session's 17:30 oracle log remains the only Windows load sample.

**Pool geometry, re-derived from the disassembly** (the earlier `+8` was
wrong): the next chunk is `chunk + 4 + (size & ~3)` (`sub_008295F0` 0x829608:
`ecx = edi + 4; ecx += esi`; the free's writes at 0x8298A0/0x8298A5 land at
`next+4` and `next`). In `[c+4]`, bit 0 = this chunk is free (free sets it,
allocator clears it at 0x82996F) and bit 1 = the previous physical chunk is
free (free sets it on the next chunk; allocator clears it at 0x82996A);
`[c]` is that previous chunk, and `fd`/`bk` are at `+8`/`+0xC`. `bit1 set with
[c] == 0` is therefore the inconsistent state the crash needs. The first strict
test used `- 8` and reported `strict_bad` for every bit-1 free - an instrument
defect, fixed in `19255d3`; the corrected run reads 105/105 `strict_ok`.

**Device allocation-to-free watch (`debug.korra.poolprobe=1`).** Every pool
block is snapshotted at the allocator's `LeaveCriticalSection`, and each free is
sampled three times: at its `EnterCriticalSection` (pre-lock), after the pool
mutex is taken, and at its `LeaveCriticalSection` (post-free); the ring is
dumped on a crash with all three plus the allocation snapshot. 11 launches (one
validation plus the 10-run batch, `debug.korra.wait_us=100`, same OCR Continue
route into gameplay) read **303,104 frees**: `locked_chg=0` in all of them (the
header is byte-identical before the pool lock and after it is acquired),
`alloc_bad=0` (no block was ever handed out with bit 1 and a non-chunk link),
`size_chg=0`, `link_zeroed=0`, `SUSPECT=0`, **0 crashes**; 50,757 of those frees
had size bit 1, all with a valid link. Allocation snapshots covered 94.5 % of
frees (the remainder are blocks the allocator's LeaveCS hook did not see; cause
not identified). With the prior session that is 0 crashes in 10 launches at
100 us against the 1/13 prior rate - not a contradiction, and not evidence of
absence.

**The fault state is sufficient, and the guard skips it (controlled A/B).**
`debug.korra.poolinject=<free #>` forces the crash's header (`[block-8]=0`,
`[block-4] |= 2`) on that one free and lets the real free path run into it.
Guard off, injected at free #8000: `[poolprobe] INJECT blk=221EF648 seq=8000`,
then `[crash] signal 11 ... guest_func=0x00829560 ... esi=0x00000000`, exit
`reason=2 (SIGNALED) status=11`, on `tid=4109` (the boot loader) - the historical
crash reproduced deterministically from exactly that header state. Guard on,
same injection: `[poolprobe] GUARD blk=221EF940 hdr=00000000/0000000E ->
size=0000000C`, the free skipped the unlink, the run continued **5,312 further
frees** and roughly three minutes of gameplay (HUD, play gestures, 11,891 `hw
block` audio lines, the checkpoint autosave) and ended only on our force-stop
(`reason=10`). The ring's #8000
record carries `strict_bad=1` (the injected free) while every natural bit-1 free
in the same run is `strict_ok`.

**A clean guard-on run without injection** (`gw100`, 352 s, 23,552 frees)
showed no regression: 7,486/7,486 bit-1 links strict-valid, 16,471 `hw block`
audio lines, the checkpoint autosave, crash buffer empty, and it ended on our
force-stop (`reason=10`); with no fault state the guard never fired, so this
run says the check is inert, not that it fixes anything.

**Conclusion.** The header invariant holds at allocation and at the free entry
in every observed free, and nothing changes while a free waits for the pool
lock, so the engine's own writer was not observed at this sample size - the
window is rarer than ~300k frees. The controlled injection proves the fault
state alone is enough to produce the exact crash and that a host-side check at
the free entry can prevent it, but a guard that fires only on a state this
sample never reached is **not** a measured fix for the engine's race. Keep the
1 ms poll as the shipping workaround; the guard stays a candidate for the owner
to gate (it writes one bit of guest memory, patches no code, and skips only an
unlink the invariant already calls invalid). No queue, no Stage 2, no 30 fps
claim.

**Not verified / open.** The writer itself (no observation in 303,104 frees);
the real fault rate at 100 us (0/10 this session, 1/13 before); a long
guard-on run through the *engine's own* fault (the A/B used the injected state);
the Windows Continue load (menu-only sample this session); the allocation
snapshot misses (5.5 %); mutex waits, `WaitForMultipleObjectsEx`, Stage 2, 30
fps.

## 2026-11-28 - Windows oracle: the t=55 s freeze and the flow=0x9 black frame do not reproduce; DInput polls the keyboard and never sees injected keys

Session scope: Windows only (no device attached by design), HEAD `8129b87` verified
by subject, diagnostics branch untouched, no pcrecomp change. One new host probe
(`src\host\oracle_probe.c`, off by default) plus a `-ProcessId` mode in
`scripts\snap_window.ps1`; `src\host\xinput_stub.c` gained the off-by-default
`KORRA_KEYS` mapping.

### 1. The t=54-55 s freeze - not reproduced, and not a build regression

The recorded instance (`logs\disc_*` were this session; the 2026-11-22 log is
`korra_host-20261122-002531.log`, `run_m4c.exe`) is real: last `[perf]` line
`t=136s 1s tick=0/s present=0/s cpu=5% natcalls=0 eip: native:WaitForSingleObject
x109`; `[perf] STALL tick unchanged for 81s`; every one of the 24 guest threads
sampled inside ntdll (`ntdll+0x7837C` / `+0x7A1CC` / `+0x7890C`), main thread in
the same frame, the sink thread still printing every second; **no** `[wprot]`,
`[wlog]` or `[crash]` line anywhere in the log, no ExitProcess. The last
per-frame line before it is `t=32.6s f=1646 dt=78.6ms present=2` - i.e. the tick
was healthy to 54 s and then stopped dead.

Four 90 s runs on this build with `KORRA_PERF=1`:

| run | env | last `[perf]` | froze |
|---|---|---|---|
| a | (none) | `t=88.5s f=2942 dt=68.7ms present=2` | no |
| b | `KORRA_PAD=1` | `t=88.4s f=3130 dt=77.0ms present=2` | no |
| c | `KORRA_PAD=1 KORRA_PAD_AUTO=1` | `t=88.7s f=2958 dt=51.9ms present=1` | no |
| d | `KORRA_PAD=1 KORRA_PAD_AUTO=1`, a copy **named** `run_m4c.exe` | `t=89.0s f=3313 dt=53.4ms present=1` | no |

**Correction (2026-11-28, later the same day).** Run (d) does **not** show "not a
build regression". The log's header is `korra_recomp 2026-11-22 00:25:31`;
`build\run_m4c.exe` was written `22.11.2026 00:41:18`, i.e. **16 minutes after
the log it was supposed to reproduce**. Same name, later build: the exact
freezing binary is not on disk, so the freeze's build dependence is untested
rather than disproved.

What the four runs do support, and no more: the event is **intermittent** and was
**not detected** at ~**27 % power**. At the historical rate (1 crash in 13 runs,
2026-11-27 device data) four clean runs happen `1 - (12/13)^4 = 27 %` of the time
by chance; 0 of 4 is therefore expected just over a quarter of the time and says
nothing about absence. The freeze is **still open**, instrumented
(`KORRA_STALLPROBE=1`: pending handle, creating call, last signaller) so the next
occurrence names its wait/signal pair instead of parking 24 threads anonymously.


### 2. `flow=0x9` black frame - not reproduced (the area renders)

The New Game route from `docs/audio.md` 10.6 as a `KORRA_PAD_SEQ` timeline
(44 s A, 47 s A, 53 s A, 61.8 s A, 67.8 s DOWN, 69.8 s A, 73.2 s LEFT, 75.2 s A,
80.2 s A). One run reports the state with `KORRA_TRACE_SCENE=1`:

```
[scene] flow=0x9 inst=0x1 list=0x00000001 gate=0x1 early=0xDAF6134
        player=0x00000000 step=0xFFFFFFFF mode=2 pool=34
```

Note `mode=2` (the 2026-11-22 entry recorded `mode=0`) and `pool=34` (that entry
said 63 -> 91); the shape is the same.

A second run of the identical timeline, screenshotted at 100 / 112 / 119 s, has
**mean 76.0 / 76.5 / 76.5** (0-255, 8-pixel stride) - the post-cutscene area
renders. The 2026-11-22 black frame (mean 0.0, pixel-identical at 134/160/185 s)
did not reproduce, which is what the "does not reproduce in 2 runs" branch of the
plan expects. **Closed as a negative / capture artifact.** Not verified: a
same-run scene line *and* screenshot (the scene probe was displaced by the new
probe in that run - see below), and any device comparison (no device in this
session, by design).

### 3. Keyboard automation - the engine polls DirectInput every frame and the injection never reaches it

`KORRA_DINPUTPROBE=1` on the New Game route, whole run:

```
[dinput] DINPUT8.dll#-1 this=60000000                 DirectInput8Create(hInstance,0x800,...)
[dinput] DINPUT8.dll#4/#3  this=01712064              CreateDevice, EnumDevices
[dinput] DINPUT8.dll#11    this=017EE7FC  a1=...      SetDataFormat  (keyboard)
[dinput] DINPUT8.dll#13    this=017EE7FC  a1=07A30A0A SetCooperativeLevel 0x0A
[dinput] DINPUT8.dll#11/#13 this=017EE50C             the same for the mouse
[dinput] DINPUT8.dll#7     this=017EE7FC  ret=0       Acquire -> DI_OK
[dinput] DINPUT8.dll#9     this=017EE7FC  cb=256      GetDeviceState x3963 in 117 s
[dinput]   state cb=256 first8=00 00 00 00 00 00 00 00
[dinput] DINPUT8.dll#9     this=017EE50C  cb=20       mouse x3963
```

So: two devices, both **acquired**, both polled **every frame** in immediate mode
(no `#10` GetDeviceData anywhere), and the keyboard's first poll is all zero.

**Correction (2026-11-28, later the same day).** That probe sampled the state
buffer only on a site's *first* poll, so "first8" above is all it ever proved.
The probe now samples every poll (`408ba43`) and the question is decidable:

| run, `KORRA_DINPUTPROBE=1` | keyboard site (slot 9, cb=256) | mouse site (cb=20) |
|---|---|---|
| 120 s, Enter 300 ms at t=46 s, DOWN 300 ms at t=52 s | `first8=00...` and **`or8=0x00` for the whole run** | `first8=00...`, `or8=0xFF` at t=1.6 s |

The mouse site is the instrument's own check: its OR changed *after* the first
poll, so later polls are sampled; the keyboard's OR never left zero, including
during both holds. **Conclusion: zero while held - the injection does not reach
the DInput state this engine polls.** That is the 2026-11-20 dead-end, now
measured at the state buffer itself, and "never acquired" is ruled out as its
cause (Acquire returns DI_OK). A free-standing check in the same session shows
`keybd_event` -> `GetAsyncKeyState` works (`0x8001` while held), so the failure
is not the injection API.

Branch: map the keys onto pad 0 in the XInput stub (`KORRA_KEYS=1`, off by
default), because the pad path is the one this engine provably acts on.

**The mapping is UNPROVEN, and one observation leans against it.** The scene
globals do not move at the menus (a keys-only run holds `flow=0x9` from t=42 s),
so `KORRA_TRACE_SCENE` cannot see a menu walk. The engine's own pad accumulator
(`0xE4E558`, watched with `KORRA_WATCHLOG=0xE4E554`) never changed during a run
that held Enter and DOWN, while the same watch did capture the boot writes to the
controller table at `0xE4E564..0xE4E570` - so the watch works and the key press
did not reach the engine's pad path in that run. The driver never forced the game
to the foreground (it relied on exclusive fullscreen), which is the first thing
to fix before calling `KORRA_KEYS` working. Screen capture of the live game still
blocks (`CopyFromScreen` on the exclusive-fullscreen flip chain never returns at
30-50 fps), so no picture proof exists either.


### Instrument defect found on the way (and fixed in the probe)

A probe that chains `native32_post_hook` to "the hook that was installed" is
captured by `draw_probe.c`'s watchdog, which re-installs its own post hook every
500 ms. The chain then closes into a cycle and the boot dies 0.2 s in with
`0xC00000FD` at the first instruction of the hook (resolved against
`build\korra_recomp.map`: `_sp_post`), which reads exactly like a guest stack
overflow. `perf_probe.c` already avoids this by not chaining; the new probe does
the same plus a per-thread depth guard. The same watchdog also silently displaces
*any* other post-hook probe: with `KORRA_DRAWPATH=1` in the chain, `KORRA_TRACE_SCENE`
produced zero `[scene]` lines and the draw census stayed `frames=0 draws=0`
while counting 11.6 M bridged calls. Do not combine the scene probe with the draw
probe on this host until the chain is a list rather than a single pointer.

### Budget actuals

Wall clock ~165 min (target 120, ceiling 180). **12 Windows runs** (budget 10):
4 freeze discriminators, 3 flow=0x9 runs (one died at 0.2 s on the hook cycle),
1 boot test of the same, 3 keyboard runs (2 lost to the capture hang, 1 with the
scene probe displaced), 1 early flow run. **3 builds** (budget 3): baseline+probe,
probe fix, keys+census. **3 commits**. Selftest 60/60 after the host changes;
content guard clean.

**Not verified:** the key-only walk and its pad control; a same-run scene line +
screenshot at flow=0x9; any device comparison; the freeze's wait/signal pair
(never occurred); whether `KORRA_WINDOWED` actually took effect in the attempt
(no window-vs-fullscreen check in that run).

## 2026-11-28 (second session) - the DInput state probe is fixed, and the keyboard question is decided at the state buffer

The 2026-11-28 census above recorded the state buffer only when a `(target, this)`
site was first seen (the existing-site path returned after `calls++`), so `state_or`
was the first poll forever and its "NONZERO" line could not fire. Fixed in
`408ba43`: slots 9/10 sample every poll, the buffer is validated once and only
again if the engine moves it, so the steady path is an 8-byte read and eight ORs
(no VirtualQuery, no stdio, no allocation; inert when the probe is off). The
duplicate tentative `t_depth` is gone.

Instrument check, same run: the mouse site (`cb=20`) prints `first8=00...` and
`or8=0xFF` at t=1.6 s - a poll *after* the first changed the accumulator, so the
per-poll path is live.

**STEP 1 (120 s, `KORRA_DINPUTPROBE=1`, Enter 300 ms at t=46 s, DOWN 300 ms at
t=52 s, foreground by exclusive fullscreen):**

| observation | keyboard site (slot 9, cb=256) | conclusion |
|---|---|---|
| `first8` | `00 00 00 00 00 00 00 00` | first poll zero (all the old probe proved) |
| `or8` after both 300 ms holds | **`0x00`** | zero while held: the injection never appears in the DInput state this engine polls |

`keybd_event` -> `GetAsyncKeyState` in the same session returns `0x8001` while
held, so the injection API works; the gap is between the OS key state and this
engine's DInput state buffer. Acquire still returns `DI_OK`, so "never acquired"
is not the cause.

**STEP 2 (keys-only walk, log-visible signal): unproven.** `KORRA_KEYS=1
KORRA_TRACE_SCENE=1` with Enter/DOWN/Enter shows no `[scene]` change after the
boot transition to `flow=0x9` at t=42 s - the menu screens do not move those
globals, so the scene line cannot see a walk. A second signal was tried: the
engine's own pad accumulator (`0xE4E558`, `KORRA_WATCHLOG=0xE4E554`) did not
change during a run that held Enter and DOWN, while the same watch captured the
boot writes at `0xE4E564..0xE4E570` (values 0,1,2,3) - the watch works, so the
key press did not reach the pad path in that run. The driver never forced the
game to the foreground; that is the first thing to fix before trusting
`KORRA_KEYS`. No picture proof exists either (`CopyFromScreen` still blocks on the
live exclusive-fullscreen chain). `KORRA_KEYS` stays off by default and unproven.

Runs this session: 3 (budget 3). Builds: 1. Commits: `408ba43` (probe),
this docs commit. Selftest 60/60; content guard clean.
