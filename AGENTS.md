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
- 2026-11-20 — "CRI's `OnBufferEnd` is not needed for M2" (Track D's conclusion,
  from the feed loop being `GetState`-driven) — **wrong on the second half of the
  cycle**. Feeding is GetState-driven, but once the feed stops CRI waits on the
  counter that only `OnBufferEnd` bumps, and it holds the engine's audio lock
  while it waits: the boot froze right after the first source voice, with the
  device pump calling `CommitChanges` ~50x/s and `GetState` never called again.
  Retirement on the QPC clock existed but only ran inside `GetState`, so it never
  ran again either. Fix in `src/host/xaudio2_stub.c`: retire (and perform
  `*(u32 *)pBufferContext += 1`, the whole body of guest 0x7B2A24) from every
  device entry point, `CommitChanges` included. Do not "fix" this by making
  `CoCreateInstance` fail: CRI then takes its silent fallback and the engine
  never exercises the path the shipping build uses.
- 2026-11-20 — Entering the lifted `OnBufferEnd` with `native32_call_guest` from
  inside a COM method — **rejected before trying it**: the bridge call returns
  with the machine's `esp` advanced by its own arguments and every register
  holding the callee's values, while the lifted caller is mid `call [ecx+0x54]`
  and expects callee-saved registers intact. The counter increment is the whole
  callback body, so perform it in C. Guest re-entry belongs on a real audio
  thread (M4), not inside a method the guest called.
- 2026-11-20 — `lock xadd dword ptr [eax], esi` at `0x660D25` **aborted the boot**
  (`RECOMP_UNIMPL`) once the movies were past: a shared-refcount release. Added
  `xadd` to `pcrecomp/tools/lift/lift32.py` (temp = dst; dst += src; src = temp;
  flags as ADD; single-threaded semantics are exact under the machine lock,
  like `cmpxchg`), relifted (220 s, 37,029/37,029, 0 failures), rebuilt. Remaining
  unimplemented sites went 114 -> 111. Check the site list before relifting:
  `Select-String -Path src\recomp\gen\*.c -Pattern RECOMP_UNIMPL`.
- 2026-11-20 — Two `build_host.ps1` runs at once (one background, one foreground)
  on the same `build\obj` — **corrupted an .obj in both directions** (`LNK1136`,
  then `LNK1104`) and cost two 7-minute rebuilds. The script is not more than one
  invocation at a time; `-Trace` now uses `build\obj-trace` so a diagnostic build
  and a normal one can coexist.
- 2026-11-20 — Chasing "`data001.cpk` fails to open" — **never happened.** The
  `[native]` trace printed its line in two `fprintf` calls, so with several guest
  threads the return half of one call landed on another call's line, and a
  successful `CreateFileA` looked like it returned 0. The trace now builds one
  line and prints it once; `KORRA_TRACE_FILE` prints arguments *and* result in a
  single write for the same reason. Read a two-part log line as untrustworthy
  before believing it.
- 2026-11-20 — Driving the menus with `keybd_event`/`SendInput` and window
  messages — **does nothing**, even with the window foreground, focused and
  topmost. The engine's keyboard path is DirectInput and it does not see injected
  input in this build; the XInput pad does. Do not spend time on synthetic
  keyboard input until the DInput device path is wired (M2.11).
- 2026-11-20 — "The device vtable lives in `d3d9.dll`, so an object whose first
  dword is not in the module range is not a D3D9 interface" — **false for this
  d3d9**. `IDirect3D9::CreateDevice` returns a device whose vtable is a
  heap-allocated thunk table (`0x3880A8FC` in the measured run) whose *entries*
  are d3d9 code. Requiring the table itself to be inside the module silently
  classified the device as "not ours". Check the entries, not the table.
- 2026-11-20 — Hand-typing COM IIDs from memory — **cost two hours**. The typed
  `IID_IDirect3DDevice9` was wrong in its last four bytes (`A43B0D92B9E4` vs the
  SDK's `A43B0D82B9EB`), so `QueryInterface` answered `E_NOINTERFACE` on a real
  device and the conclusion "the engine never calls a device" looked measured.
  Copy GUIDs out of `d3d9.h` (and remember `DEFINE_GUID` only declares them: use
  local `static const GUID` values or link `guid.lib`).
- 2026-11-20 — **`KORRA_WINDOWED` never worked.** `quirk_probe.c` rewrites
  `D3DPRESENT_PARAMETERS` when the call target is named
  `"d3d9.dll!CreateDevice"`, but the runtime names only bridged *imports*; COM
  methods have no name (`native32_name` returns NULL). So the game stayed
  exclusive-fullscreen and **a second instance of the game cannot create a
  device** (`ExitProcess` right after `SteamAPI_Init`, no window, no fault), which
  made two parallel debug sessions kill each other's runs. Until this is fixed,
  serialise game runs between sessions. The draw probe identifies
  `CreateDevice` structurally instead (BehaviorFlags in {0x44,0x24}, DeviceType
  in {HAL,REF}, readable 0x38-byte params, interface written to `args[6]`).
- 2026-11-20 — Reading a texture back with `IDirect3DTexture9::LockRect` from
  inside the bridge post-hook (first-draw pixel survey) — **killed the process**
  at the first draw with no fault report in the log (subsequent runs with the
  survey off survived 175 s). It is behind `KORRA_DRAWPATH_TEX=1` now and off by
  default; do not combine it with a long session until the mechanism is
  understood.
- 2026-11-20 — "The Turkish system codepage (1254) is why the engine draws no
  text" — **disproved**. `KORRA_FORCE_ACP=1252` (new knob in `draw_probe.c`,
  rewrites the guest's `GetACP`/`GetOEMCP` result) ran a full session to the
  title screen: picture unchanged, text still absent, and `GetACP` is called
  exactly **once** (during CRT startup) — the engine's text path never asks for
  it. Do not re-try codepage forcing.
- 2026-11-20 — "The resource manager is never given an asset, so the player's
  bundle is never registered" — **disproved by reading the pool instead of its
  call sites.** At the moment the player is rejected the pool holds 56-62
  records, including `pl\pl00f0.dat` in state 8 (the *only* state the find
  accepts) with a non-NULL payload that is the raw DAT container. Gate 2 fails
  one step later, in `sub_00637BC0` resolving `_battleParameter.bin`, which the
  shipped bundle does contain. Counting key builds and finds is not evidence
  about what is registered: read `[0xE4563C]`/`[0xE45644]`, or use
  `src/host/scene_probe.c` (`KORRA_TRACE_SCENE=1`).
- 2026-11-20 — `--poison` takes **one** address and `--firsthit` **one** window;
  the last one passed wins silently (`g_poison` is a single dword,
  `g_fh_lo`/`g_fh_hi` a single pair, and `g_fh_seen` is re-`calloc`ed per
  argument). A run with four `--firsthit` windows reports only the fourth, and
  one with three `--poison` addresses only the third — which looks exactly like
  "that function never ran". One of each per run.
- 2026-11-20 — runtime trace output is **not** in `build\run.err`: the host
  redirects stderr into `<game>\korra_host-*.log`, so an empty `run.err` does
  not mean the trace was silent.
- 2026-11-20 — "SF and OF from `pcmpistri` are not carried because no generated
  site branches on them" (the comment in `lift32.py`) — **wrong, and it cost the
  new-game crash**. Site `0x77A38B` in the CRT's SSE4.2 `strstr` branches
  `jno`/`jns` on exactly those, and with `_flag_a/_flag_b` synthesized from
  ZF/CF they became constants: `jno` always taken, so the needle-verify loop
  always rejected its candidate. `CMP_NO` is literally `1` and `jno`/`jns` read
  bits the model never had. Fixed by packing CF/ZF/SF/OF into `FK_EFLAGS`
  (`recomp_pcmp_eflags`, pcrecomp `b0b2ce2`). Before adding "no site branches on
  X", grep the *original* disassembly for the jcc form, not the lifted text.
- 2026-11-20 — calling guest code inline from `native32_post_hook` on a private
  stack (to read a function's return value) **hangs the process** before `main`
  flushes, and starting the same experiment from a worker thread starves on the
  machine lock (the guest only yields at depth 1, which the game loop never
  reaches). Do not re-try either; read the *original instructions* with capstone
  instead, which settled this defect in one pass.
- 2026-11-20 — a `.CRT$XIU` initializer that calls `CreateThread` faults before
  `main` (exit `0xC0000005`, no log file). Initializers may install hooks; start
  threads later.
- 2026-11-20 — a thread census that suspends every thread from
  `CreateToolhelp32Snapshot` **hangs the probe** when it reaches its own thread:
  `SuspendThread(me)` never returns, so the sampler stops silently and the game
  keeps running. The run then looks like "the game stalled" instead of "the probe
  died". Skip `GetCurrentThreadId()` before suspending anything.
- 2026-11-20 — identifying `IDirect3D9::CreateDevice` as "BehaviorFlags has
  vertex-processing bits plus a plausible `D3DPRESENT_PARAMETERS`" — **too
  loose**. It matched unrelated COM calls on the 4600th frame, rewrote a guest
  structure whose slot 4 held `0xDEAD0000` (the runtime's dummy return address),
  and crashed the boot with a write to `0xFFFFFFF4`. Match the engine's exact
  BehaviorFlags (`0x44`, then `0x24`), require `DeviceType` HAL/REF, validate
  every field of the parameter block, and act on the **first** match only.
- 2026-11-20 — reading first-area fps from an On-Screen-Display overlay — **the
  overlay is part of the measurement**. With MSI Afterburner/RTSS attached,
  `IDirect3DDevice9::Present` blocks 52-581 ms (median 301 ms, always `S_OK`) on
  ~1.2 % of frames; with the stack detached the same build holds 40-58 fps and no
  Present exceeds 50 ms. The 298 ms "3 fps" reading was that stall. Count presents
  at the device call (`src/host/perf_probe.c`, `KORRA_PERF=1`) before trusting an
  OSD number. `D3DPRESENTFLAG_LOCKABLE_BACKBUFFER` is **not** the mechanism
  (clearing it changed nothing).
- 2026-11-20 — building the generated tree with `/O2` — **does not compile**:
  `recomp_0021.c` and `recomp_0035.c` (~90 MB each) die with `C1002: out of heap
  space in pass 2`, and `cl` raises it to `D8040`. `/Zm1200` does not help; do
  not retry `/O2` on these translation units. `scripts\build_host.ps1 -Fast`
  therefore uses `/O1` plus `RECOMP_MEM_QUAL` empty, which is the part of the
  optimisation that matters for this code (every guest memory access was
  `volatile`) and compiles in the same time as the normal build.
- 2026-11-20 — making the machine-lock hand-over cheaper (thread-local state
  instead of `TlsGetValue`, `WakeByAddressAll` only when a ticket is queued,
  longer spin) — **correct but neutral**: 45.3 fps in the first area vs 44.9
  before, same min/max. The frame is not limited by the hand-over. Kept as
  unconditionally cheaper code (`pcrecomp` Korra-0007), and it is a reminder to
  measure a "obviously faster" change before writing it up as a win.
- 2026-11-20 — "the catalog is already correct, so a re-catalog will not change
  anything" — **wrong**. `disasm32.table_entries` read a jump table only
  *upward* from the displacement, so a table indexed by a negative register (CRT
  memcpy: `jmp [ecx*4 + 0x77A8C4]`, ecx -7..0) yielded one arm or none. The
  arms that never entered the catalog are not block leaders, so their switch
  lifted to `RECOMP_ITAIL`, the dispatch table cannot resolve a mid-function
  address, and the memcpy remainder **returned without copying**. Re-cataloguing
  with the downward scan (`pcrecomp` Korra-0008) found 298 more functions
  (37,029 -> 37,327) and removed the unresolved dispatches. A re-catalog is
  cheap (528 s) and its diff is a real signal: read it before assuming the
  cataloger version does not matter.
- 2026-11-20 — answering a stubbed COM vtable with **one** body for every slot —
  **cost every long run**. `ISteamUserStats::GetAchievement(name, bool*)` takes
  two stack arguments, but the stats object answered with the one-argument
  `ISteamUser` body: it returned NULL *and* popped 4 bytes instead of 8, leaving
  the guest `esp` 4 short and faulting the next member read at `[0x11]` (guest
  `0x0076C590`) about 5 minutes into every run. A COM vtable needs a per-slot
  entry whose `ret N` matches that method's arity; a generic "return 0" body is
  only safe for slots you know are unreachable.
- 2026-11-21 — `recomp_yield_hook` as the fix for the on-device spin at
  `InterlockedExchange(0x2139780C)` — **fired three times and then never again**.
  The spinning loop has no instrumented back edge, so the hook cannot run there.
  Do not re-try; instrument the spin's own shim instead (it is already in
  `k_InterlockedExchange`).
- 2026-11-21 — releasing the machine lock every 4096 import dispatches, to let
  the starved pool workers run — **the spin was unchanged**. On the device the
  lock is held across a loop that calls no import at all, so a cadence tied to
  imports never reaches the loop. The device log later showed the spin was not
  the failure: the run dies when a pool worker calls a NULL job function and
  then waits on handle 0 (see `docs/FINDINGS.md`, 2026-11-21 deadlock entry).
- 2026-11-21 — `-PkorraDxvkSource=..\..\reference\dxvk-native` — **fails at CMake
  configure** with "DXVK tree not prepared", because the value is a CMake cache
  `PATH` and stays relative to the *build* directory. Pass an absolute path.



## 7. Reporting

- Update `docs/FINDINGS.md` after every rung/milestone with the numbers
  (functions lifted, failures, frame time, device).
- A change is done when a build ran and the log/screenshot proves it. State what
  was *not* verified.
- Keep the status table in `README.md` honest.
