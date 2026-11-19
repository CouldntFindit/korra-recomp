# AGENTS.md — working rules for this repository

Written for an AI agent picking this project up cold. Read this file, then
`docs/FINDINGS.md` and `docs/TOOLCHAIN.md`. Every claim below is verified or
explicitly marked unverified.

## 1. Hard rules

1. **Never commit game content.** `LoK.exe`, `GameData\`, `*.cpk`, `*.usm`,
   extracted assets, generated C (`src/recomp/gen/`, `work/`) are gitignored and
   `tools/check_content.py` rejects them. If a guard fails, fix the ignore rules —
   never `--no-verify` your way past it.
2. **Never publish binaries.** Source-only releases (no APKs, no compiled
   recomp code). Builders compile locally from their own copy.
3. **Do not copy code from `nfsu2-recompiled` or `maxpayne2pc-androidevolved`.**
   Neither has a LICENSE file. Study their approach, re-derive the code, credit
   them in `THIRD_PARTY_NOTICES.md`. `ngbrecomp` (GPL-3.0) may be adapted directly.
4. **Pin your toolchain.** `pcrecomp` moves fast and rewrites history (branch
   deletions are normal). Record the exact base commit + patch series in
   `docs/TOOLCHAIN.md` and keep a mirror of the pinned tree.
5. **One hypothesis at a time.** A crash is not "fixed" until a build runs and
   the same input reaches one step further. Record what you tried in the
   dead-end ledger even when it fails.

## 2. Layout

```
config/            lift configuration for this game (seeds, exclusions, hooks)
docs/              TOOLCHAIN.md (pin), FINDINGS.md (append-only log), PUBLISHING.md
scripts/           run_korra_lift.py (catalog/lift driver), sync_codeberg.ps1
src/host/          Win32 + D3D9 + XInput + XAudio2 compatibility host (ours)
src/recomp/gen/    generated C  — GITIGNORED, never committed
tools/             triage_pe.py, prepare_analysis_exe.py, check_content.py
tests/             host-shim selftest, mirror selftest
work/              analysis exe, seeds, catalog, logs — GITIGNORED
```

Everything derived from the game binary lives in `work/` or `src/recomp/gen/`.

**`src/host/` is a shared tree.** More than one work stream writes there (bring-up:
`main.c`, `shims.c`, `runtime_glue.c`; audio/Steam: `steam_stub.*`, `xaudio2_stub.*`).
Each concern owns its own file, and both build paths glob `src/host/*.c` — so a new
file never needs a build-script edit, and nobody has to merge into somebody else's
file. Do not rewrite a file you do not own; add a shim to `shims.c` instead.

## 3. Commands that matter

```powershell
# PE report (committed output, no game bytes)
python tools\triage_pe.py "The Legend of Korra\LoK.exe" --json work\triage.json

# analysis copy: restores the relocation directory pcrecomp needs
python tools\prepare_analysis_exe.py "The Legend of Korra\LoK.exe" work\LoK.analysis.exe

# seeds -> catalog -> generated C (PCRECOMP env var points at the pinned tree)
$env:PCRECOMP = "..\pcrecomp"
python scripts\run_korra_lift.py seeds   work\LoK.analysis.exe work\
python scripts\run_korra_lift.py catalog work\LoK.analysis.exe work\seeds_all.json work\catalog.json
python scripts\run_korra_lift.py lift    work\LoK.analysis.exe work\catalog.json src\recomp\gen

# publishing: mirror to Codeberg (source only). -DryRun checks, the bare form pushes.
powershell -ExecutionPolicy Bypass -File scripts\sync_codeberg.ps1 -DryRun
powershell -ExecutionPolicy Bypass -File scripts\sync_codeberg.ps1
```

The mirror script refuses a dirty tree, a foreign host, a non-fast-forward and
anything the content guard rejects. **Never force-push it** — re-create the
mirror instead. Policy and setup: `docs/PUBLISHING.md`; its own regression test:
`powershell -ExecutionPolicy Bypass -File tests\sync_codeberg_selftest.ps1`.

## 4. Bring-up ladder (Windows host first, Android only after)

Each rung = build, run, log, fix, tick in `docs/FINDINGS.md`. Do not skip rungs;
a silent failure at a later rung usually means an earlier one is half-done.

1. lifted C compiles (MSVC x86)
2. links against host runtime + imports resolve
3. static CRT init runs (heap, timers, TLS, threads)
4. entry point → `WinMain` reached (log line)
5. window created (`RegisterClassExA` / `CreateWindowExA`)
6. D3D9 device created
7. **first frame** (screenshot as proof)
8. `SteamAPI_Init` stubs live (7 functions, no callbacks)
9. XAudio2 COM stub returns a silent device
10. CRI file layer serves `GameData\*.cpk` paths
11. menus respond to keyboard/XInput
12. USM movies skipped

Gate: boots to the main menu on Windows without Steam, silent, movies skipped.

## 5. Crash triage trees

Start from the fault address, then the last log line, then the coverage snapshot.

| Symptom | First hypothesis | Check |
|---|---|---|
| Deref of `NULL + small offset` | member read on an object that was never constructed (factory returned 0) | which import/com returns null at that point; stub it to return a token object |
| Deref of a huge address (`0x8xxxxxxx`, `0xCDCDCDCD`-like) | stale GPU/sound handle after a device reset, or an uninitialised struct | find the handle's source in the log; compare against D3D9 object lifetimes |
| Call through `NULL` / `call [eax+8]` with garbage | unmapped function pointer: vtable slot not covered by the lift, or a vtable seeded by RTTI but never scanned | look the VA up in `work/catalog.json`; if absent, add seed and relift |
| Assertion dialog / `int 3` | unimplemented host behaviour reached in a debug build | log + skip; never patch the guest's check away before understanding it |
| Crash only in a hook or only after N seconds | hook ABI mismatch or stack imbalance (`__stdcall` vs `__cdecl`) | verify the import's calling convention in `pe_analyze` output |
| Frame renders but nothing advances / game runs at 30 fps | frame limiter, **not** `framerate_d`/`framerate_n` (those are movie fields — see `docs/quirks.md` §1) | log `DAT_00e4d210` and `[frameMgr+0x58]`; limiter is `FUN_00754840` (0x754840), 30/60 switch in `FUN_0074c580` (0x74c580) |
| Silence instead of crash | audio thread died first | check XAudio2 COM stub return codes before blaming the engine |

**Recovery loop for uncovered indirect calls:** run with coverage reporting,
collect the missed VAs, add them as seeds (or `RECOMP_STRICT_ICALL` traps), relift
only the affected module, rebuild. Keep `config/seeds_extra.json` under version
control so the gain is permanent.

## 6. Dead-end ledger

Append only; newest last. Format: date — attempt — outcome — next idea.

- 2026-11-19 — Compiling the generated C straight from `run_korra_lift.py` —
  **failed** with ~100 `C2065: 'esp'/'eax' undeclared` per file. Cause: the
  driver wrote `recomp_types.h` without `#define RECOMP_GENERATED_CODE`, and that
  macro is what maps register names onto the runtime's register file. Fix in the
  driver (not in the runtime): emit `#define RECOMP_GENERATED_CODE` + `<math.h>`,
  `<string.h>` per generated file. Relift is 290 s, so it is cheaper to fix the
  driver and relift than to patch headers.
- 2026-11-19 — First `build_host.ps1` run also proved the toolchain: the
  Windows SDK was missing even after the Build Tools install; the SDK came later
  via `winsdksetup` as part of the same install. Always check
  `C:\Program Files (x86)\Windows Kits\10\Include` before blaming flags.


## 7. Reporting

- Update `docs/FINDINGS.md` after every rung/milestone with the numbers
  (functions lifted, failures, frame time, device).
- A change is done when a build ran and the log/screenshot proves it. State what
  was *not* verified.
- Keep the status table in `README.md` honest.
