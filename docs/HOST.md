# Windows host (M2)

The host is deliberately thin. `pcrecomp`'s **native32** runtime does the hard
part: the host process is 32-bit and the guest image is mapped at its own base,
so guest pointers and Win32 structs are already the host's, and every import
that is not stubbed calls the **real Windows API**. The bridge measures the
callee's stack purge on each call, so `stdcall`, `cdecl`, `thiscall` and COM all
work without an argc table. Callbacks (WndProc, thread starts, hooks) come back
in through a VEH trampoline because the mapped code pages are non-executable.

```
src/host/main.c   map LoK.exe at 0x400000 -> bind imports -> native32_call_guest(0x77C3B7)
src/host/shims.c  7 steam_api functions + GetProcAddress (shim-aware)
```

## Build

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build_host.ps1 -Jobs 8
# -> build\korra_recomp.exe
```

- Toolchain: VS 2022 Build Tools x86 (`KORRA_MSVC` overrides the install root),
  pcrecomp at `..\pcrecomp` (`PCRECOMP` overrides).
- 75 generated files, 12.0 M lines, 613 MB. First build is long; `/MP8` and
  `/O1` are deliberate (pervasive `/O2` on this volume is not worth the time).
- `CMakeLists.txt` describes the same build if CMake is ever installed. The
  PowerShell script is the authoritative path today.

## Run

```powershell
build\korra_recomp.exe "C:\path\to\The Legend of Korra"
# or set KORRA_GAME_DIR, or default ../../The Legend of Korra from the repo
```

Log: `korra_host.log` in the game folder (stderr is duplicated into it, so the
runtime's crash report lands there too).

## Ladder and what is already answered

| Rung | Status | Note |
|---|---|---|
| M2.1 compiles | run `build_host.ps1` | 37k functions, no per-function opt-outs yet |
| M2.2 links | script links directly | **no D3DX/D3D9 shims needed** — imports resolve to the system DLLs (`d3dx9_43.dll`, `d3d9.dll`, `xinput1_3.dll`, `dinput8.dll` all present) |
| M2.3 CRT | — | the guest's own static CRT runs lifted; heap/timers/threads go to real Windows |
| M2.4 entry → WinMain | — | entry VA 0x77C3B7 |
| M2.5 window | — | `RegisterClassExA`/`CreateWindowExA` are real API calls |
| M2.6 D3D9 device | — | real `d3d9.dll`; DXVK-native swap comes after it boots |
| M2.7 first frame | — | screenshot as proof |
| M2.8 Steam | **done** | 7 shims in `shims.c`, `SteamAPI_Init` → 1 |
| M2.9 XAudio2 | **done** | `xaudio2_stub.c` is a real 2.7 device: it retires buffers on a QPC clock and performs CRI's `OnBufferEnd` counter increment (guest `0x7B2A24`), retire triggered from every device entry point. `KORRA_XA2_VERBOSE=0..3` |
| M2.10 CPK | **works** | CRI File System runs lifted; every `CreateFileA`/`ReadFile` in a full boot succeeds, both CPKs and the four loose sound files included |
| M2.11 menus/input | **partial** | the title screen animates and the pad drives it (`xinput_stub.c`: `KORRA_PAD=1`, `KORRA_PAD_AUTO=1`); menu *text* does not render yet, and injected keyboard/mouse input does not reach the engine |
| M2.12 USM | **works** | the CRI Sofdec player plays the intro and logo movies for real; nothing needed stubbing |

Known open blocker: the new-game transition dies with `0xC0000005` in lifted
`sub_00603C00` (null manager at `0xCA3E58`) - see `docs/FINDINGS.md`,
2026-11-20.

## Diagnostics

Everything below is opt-in and off by default.

| Knob | Effect |
|---|---|
| `KORRA_XA2_VERBOSE=0..3` | XAudio2 stub logging, 3 = queue-length changes |
| `KORRA_QUIRK_PROBE=1` | once per second: `DAT_00e4d210`, `[frameMgr+0x58]`, flags, HWND, measured frames/s |
| `KORRA_PAD=1` [`KORRA_PAD_BITS=0x1000`] [`KORRA_PAD_AUTO=1`] [`KORRA_PAD_STICK=lx,ly`] | synthetic pad 0; the stick is centred unless `KORRA_PAD_STICK` says otherwise (raw XInput units, Y up) |
| `KORRA_PAD_CMD=<file>` | screen-driven holds: `<seq> <bits> <hold ms> [<lx> <ly>]`; a new seq starts one hold, the optional left-stick value is held with it |
| `KORRA_TRACE_FILE=1` / `KORRA_TRACE_D3D=1` | `CreateFileA` arguments + result; failed d3d9 results and every d3dx call |
| `KORRA_TRACE=1` | one line per bridged native call |
| `KORRA_DRAWPATH=1` | D3D9 draw census: per-frame draws/state + one line per distinct draw shape (`_FROM=<frame>` adds a per-frame texture histogram, `_FRAME=<frame>` dumps one frame's draws, `_TEX=1` adds a pixel survey that is known to kill a run) |
| `KORRA_FORCE_ACP=<n>` | rewrites the guest's `GetACP`/`GetOEMCP` result (codepage experiments) |
| `build_host.ps1 -Trace` | separate `build\obj-trace` with `-DRECOMP_TRACE` (`--calltrace`, `--firsthit`, `--argtrace`, `--watch`, `--poison`) |

Logs are per run (`korra_host-YYYYMMDD-HHMMSS.log` in the game folder), so a
manual play session and an automated run never overwrite each other.
`scripts\run_until_exit.ps1` reports how a run ended; `scripts\probe_input.ps1`
drives the window and screenshots each step.

## Crash protocol

See `AGENTS.md` §5 for the triage trees. Two runtime aids are built in:

- `native32_dump_icalls(n)` — unresolved indirect calls, printed when the guest
  returns (and available for a crash report).
- the import ring buffer — the last native calls, with caller VA and result, so
  a fault is attributable to "this import, from this lifted function".

Feed any unresolved ICALL VA back through `tools/…` seeds and relift only that
function, so coverage gains are permanent.
