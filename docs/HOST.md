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
| M2.9 XAudio2 | open | `CoCreateInstance` goes to the real COM; `XAudio2_7.dll` exists on this machine, so the pre-2.8 CLSIDs may just work — if not, stub it |
| M2.10 CPK | expected free | CRI File System is lifted code; real `CreateFileA`/`ReadFile` serve `GameData\*.cpk`, CRILAYLA decompression runs in the guest |
| M2.11 menus/input | — | XInput by ordinal 2/3 resolves to `xinput1_3.dll` |
| M2.12 USM | open | movies will be attempted for real first; skip only if they hang |

## Crash protocol

See `AGENTS.md` §5 for the triage trees. Two runtime aids are built in:

- `native32_dump_icalls(n)` — unresolved indirect calls, printed when the guest
  returns (and available for a crash report).
- the import ring buffer — the last native calls, with caller VA and result, so
  a fault is attributable to "this import, from this lifted function".

Feed any unresolved ICALL VA back through `tools/…` seeds and relift only that
function, so coverage gains are permanent.
