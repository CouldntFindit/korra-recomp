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
6. **Never commit machine-identifying paths.** No absolute home-directory paths
   (a Windows profile path or a POSIX home path), no cloud-sync folder names, no
   localized Documents folder names. Use `<workspace>`, `<Documents>`,
   `%LOCALAPPDATA%` / `$env:LOCALAPPDATA%`, or derive the location at runtime
   (the Android scripts show the pattern). `tools/check_content.py` scans file
   contents on every commit and in CI; `tests/check_content_selftest.ps1`
   proves the patterns fire.

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

- 2026-11-21 — "WASAPI is not available on this machine" — **it was never
  tried.** `sink_thread` switched on `KORRA_AUDIO_BACKEND` with
  `case WASAPI: ... default: break;`, and the auto value is -1, so it fell
  straight to the `falling back to waveOut` line without attempting WASAPI at
  all. A missing log line was the only symptom; the backend itself works
  (shared, event-driven, `AUTOCONVERTPCM`, 1056-frame buffer, 22 ms). Log the
  backend you *chose* before opening it, and never let `default:` mean
  "do nothing".
- 2026-11-21 — treating `IXAudio2Voice::SetOutputMatrix` as a no-op because
  "CRI computes the downmix itself" — **the engine's matrix is the mix.** With
  invented 0.7071/0.5 coefficients the dump measured RMS -16.1 dBFS and peak
  **0.0 dBFS** (clipped); storing the engine's 2x2 (stereo voice) and 6x2 (5.1
  voice) matrices and applying them gives peak -9.8 dBFS. 11 dB of error that
  only a measurement caught. Install the levels, do not re-derive them.
- 2026-11-21 — a log line written as prefix + body in two `fwrite`s — the sink
  thread and the guest thread interleaved them into lines like
  `[xa2] [xa2] SubmitSourceBuffer... voice ... ran dry`. Same lesson as the
  runtime trace earlier: build the whole line in one buffer and write it once
  (`vsnprintf` + one `fwrite` + `fflush`).
- 2026-11-21 — "the title screen is silent, so the sink is broken" — **it is
  not the sink.** The Hw/Wwise device is created, its 2 ch float voice is
  started from `0x0073EC80`, and then nothing is ever submitted to it: no
  event is posted on any screen this build reaches, and an XAudio2 source voice
  is push-only. The movie/boot audio (CRI's device, a full create/teardown per
  movie) is the only thing the engine feeds, and it comes out at -26.8 dBFS RMS
  through the sink. Do not chase Wwise silence until something posts an event.
- 2026-11-21 — finding Wwise's `PostEvent` by its name hash — **closed, not
  solved.** Event IDs are FNV-1 over the lowercased name and `0x00847B10`
  reproduces every ID in `docs/audio.md` 9.4 exactly, but its only callers are
  wide-string filename helpers (`0x007685C0`/`0x00768C60` via `0x007672A2`,
  `0x00766E02`) with extension stripping - the file/package layer, not events.
  The `Hw::Wwise::Command::PostEventWork` RTTI name (file offset `0x5F178C`,
  followed by `StopEventWork`, `ObjectSwitchWork`, `ReleaseEventWork`) has no
  pointer to it anywhere in the image, so there is no TypeDescriptor chain to
  follow. Next: locate the class through its code (the command queue's vtable),
  not through RTTI.
- 2026-11-22 - Building the device APK with `scripts\android_build.ps1` and
  expecting the game - **it produces the STUB shell.** The script runs plain
  `:app:assembleDebug`, and `KORRA_LINK_GUEST`/`KORRA_WITH_DXVK` default to OFF,
  so `libkorra.so` logs `guest: STUB - nothing linked` and the window stays
  black (the guest never starts, the frame counter still climbs). A device run
  in that state looks exactly like "the shim change broke rendering". Use
  `android\gradlew.bat --no-daemon -PkorraLinkGuest=ON -PkorraWithDxvk=ON
  -PkorraDxvkSource=<abs reference\dxvk-native> :app:assembleDebug`; check the
  first log lines for `guest: entry 0x77C3B7` (not `guest: STUB`) before
  believing anything else in the log.
- 2026-11-22 - Reading the device's save step from the `[import]` trace -
  **impossible past the cap.** `spine_android.c` stops printing imports after
  12,000 (`g_trace_left`), which is log line ~14,800; the save device's calls
  are routinely later, so their absence there is not evidence that they did not
  run. The save probe (`debug.korra.trace_save=1`) is uncapped and is the place
  to look. Same trap in reverse: an empty directory + `FindFirstFileExA` is a
  legitimate Windows failure (`ERROR_FILE_NOT_FOUND`), not a host defect.
- 2026-11-22 - "The post-cutscene SIGABRT is memory pressure (1.2 GB RSS)" -
  **wrong layer.** `dumpsys meminfo` splits that 1.19 GB into 1.03 GB Graphics
  (dmabuf), 15 MB native heap and ~110 MB anonymous; the in-app VmRSS never
  passed 122 MB. The abort was a thread cap: a guest thread took 4 MiB of the
  256 MiB stack region, so the 65th `ka_thread_attach` found the region full and
  `abort()`ed. Read the category rows, then the `thr`/`stack` columns of the
  1 Hz sampler, before calling a death OOM.
- 2026-11-22 - Driving a menu route with `adb shell input motionevent DOWN` then
  `MOVE`/`UP` in separate invocations - **does not deflect a stick.** Only the
  DOWN arrives; the cursor never moves, the route silently selects the first
  item (it loaded Continue instead of New Game) and nothing notices if the
  screenshots are stale. Use one `input swipe x1 y1 x2 y2 <ms>` per gesture, and
  diff each step against a reference screenshot.
- 2026-11-22 - `Adb-Soft shell screencap -p ...` - **`-p` binds to the
  PowerShell function, not to adb**, so the capture never ran and every pull
  returned the first screenshot (identical md5s should have been the tell). Run
  adb directly with `$ErrorActionPreference = 'Continue'` around it.
- 2026-11-22 - Replacing `WaitForSingleObject`'s 1 ms poll loop with a per-object
  condvar (broadcast on `SetEvent`/`ReleaseSemaphore`) to remove the render
  thread's 42 lock hand-overs per frame - **made the frame 60x worse**: 77 s per
  frame on the Story submenu and 94 ms on the menus (34 ms before). The poll
  loop is a shim cost worth attacking - it is 42 polls/frame and 61 of the
  frame's 124 ms is blocked re-entering the machine lock inside it - but a
  broadcast is the wrong primitive: it wakes every waiter, and for auto-reset
  events/semaphores several waiters then decide they were signalled, so the
  engine's pool pump sees slots complete out of order and retries. Reverted
  (the `src/host_android` tree carries the probe only). Next: one signal must
  wake exactly one waiter (a token/futex-style hand-off per wait), and measure
  the tutorial frame again before touching the menus.
- 2026-11-22 - A device run whose `log.txt` AND logcat both stop mid-run -
  **not a crash.** Run `fix1` has no line after `t=266 s` in either channel
  while the game kept rendering (play screenshots differ across the whole
  screen, `pidof` answers, the process ends only on our force-stop). The shell's
  single log-pump thread is the only writer to fd 2; when it stops, both channels
  go with it. `frame_probe_android.c` now also keeps its own append fd
  (`files/perf.txt`) and writes logcat directly, which is what produced the
  next run's numbers. Do not read a silent log as a dead process: check
  `pidof`, and pull `perf.txt` too.
- 2026-11-22 - Classifying the Story submenu by "Continue" and "New Game" -
  **parked the route for 200 s** on a screen that plainly showed "New Game":
  Windows OCR reads "Continue" as "Cpntinue" about half the time. Classify on
  the strings that survive (the item, the description), never on both.
- 2026-11-22 - Starting the route with the phone locked - the driver OCR'd the
  lock screen ("vodafone TR ... 59 m until full") as a game screen and pressed A
  at it for 190 s. `adb shell wm dismiss-keyguard` before `am start`, and treat
  lock-screen text as its own state.
- 2026-11-22 - the `frames=` column of the `[mem]` line - **it is the shell's
  16 ms `ALooper_pollOnce` tick, not guest frames.** It reads ~60/s from boot to
  shutdown whatever the game does, so "frames/s high, presents low" says
  nothing about the guest loop. Use the frame probe's `dt`/`n`.
- 2026-11-23 - "The engine never asks for XAudio2 on Android" - **the CLSID bytes
  were swapped.** `CLSID_XAudio2` `{5A508685-A254-...}` needs Data2 as `54 A2`
  (GUID fields are little-endian); the constant had `A2 54`, so every
  `CoCreateInstance` fell through to `REGDB_E_CLASSNOTREG` and the log looked
  exactly like "no device was requested". Prove a CLSID match by printing the
  16 bytes (the shim now does, once) - never by the absence of a later call.
- 2026-11-23 - "The dump is empty because AAudio failed" - **the WAV header
  patch rewound the file.** `dump_patch()` seeks to 0, writes 44 bytes and left
  the stream there, so every subsequent block overwrote the file's start; a
  138 s run pulled as 192 KB while its header claimed 138 s. Patch the header,
  then `fseek(..., SEEK_END)`. A dump whose header and file size disagree is
  the tell.
- 2026-11-23 - "One huge sample in the mix is the engine's decoder" - **it was
  the mixer re-reading a refilled buffer.** When the queue ran dry the mixer
  clamped `pos` back to `played` (the Windows code does this). With a live sink
  that rewinds into a slice whose `OnBufferEnd` has already fired, and CRI is
  free to refill it; the measured torn float was `l=-20071` at a slice edge.
  Keep the cursor monotonic. One out-of-range line per voice, logged from the
  mixer, is what named it.
- 2026-11-23 - "Wwise is silent because no event is posted" - **the device is
  silent because its prefill is skipped; the feed never runs.** The Hw output
  device is `0x0088B000` (vtable `0x0091EC80`, embedded output interface at
  `+0x28` with vtable `0x0091ECC8`) - locate it through its code, not RTTI. Its
  init (`0x0088B0C0`, vtable slot `0x34`, from the factory `0x008793F0`) only
  prefills the 8-slot ring if the game's sound table (`[0xCA243C]` count,
  `[0xCA2438]` 0x38-byte entries) has an entry whose 64-bit key at `+0x10` is
  non-zero; the entry exists (count 1) and stays key 0 on both hosts, so
  `device+0x68` (the feed's `[this+0x40]`) is 0 and `FUN_0088B440`'s top-up is
  `0 != 0` forever. Do not "fix" this in the host: the prefill needs the
  entry's frame size and depth. Next lead is what activates that entry.
- 2026-11-23 - Calling `AAudioStream_requestStart` twice (a bad edit left the
  call in the block it had just been moved into) - **`-895
  AAUDIO_ERROR_INVALID_STATE`**, the sink closed itself and the run went silent
  with the mixer's clock fallback working fine. Read the `requestStart failed`
  line before blaming the device; the failure is loud and exact.
- 2026-11-23 - "The monotonic cursor fixed the torn sample" - **half right.** The
  rewind into a refilled CRI buffer was real and is gone, but the same
  out-of-range probe still fires on the final build: 2 samples in a 50 s device
  run (`l=-8.637` at 18.2 s, `l=-39504.168` at 24.9 s, always channel 0 of the
  6 ch voice with the other channels near zero). Do not read "a clean run" as
  "fixed": one 70 s run was clean and the next 50 s run was not. The probe stays
  one-shot per voice; next step is to log the slice `[start,end)` and `i0` in the
  same line to tell a torn refill from a partially decoded `AudioBytes` region.
