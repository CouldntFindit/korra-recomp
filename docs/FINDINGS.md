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
