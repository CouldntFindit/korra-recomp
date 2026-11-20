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

Corrections the pass found (both now fixed in `docs/assets.md`, commit `91cce7b`):

- "1.87 GB → 2.05 GB, ~9 %" was wrong arithmetic: it is **~10 %** (2,053,247,987
  vs 1,865,341,798 = +10.1 %).
- §6.2 gave 697 s but no frame total; the measured total is **17,095 pictures**
  (697.4 s) — the per-file table already summed to it.

Also tightened: §2 now states the base rule as an explicit formula
(`base = ContentOffset − 0x800`) instead of only showing the derived value.

## 2026-11-19 — Track E: publishing (Codeberg mirror, README, notices)

Commit `4a1c868`. New: `scripts/sync_codeberg.ps1`, `tests/sync_codeberg_selftest.ps1`,
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
(`...\korradecomp\Korra`, the path the engine opens `SystemData.dat` under) does
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
- not the save directory (removed `...\korradecomp\Korra`, no change), not the
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
