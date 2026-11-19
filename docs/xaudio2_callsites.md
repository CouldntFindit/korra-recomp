# XAudio2 and steam_api host shims — call-site map and ABI evidence

Track D deliverable. Everything below was read out of the shipped `LoK.exe`
(SHA-256 `497eedaf…f4f683`, image base `0x400000`); nothing here comes from SDK
headers, because the SDK only ships XAudio2 2.9 and this game uses **2.7**.

Regenerate the mechanical parts with:

```powershell
python tools\host_import_map.py --exe work\LoK.analysis.exe --json work\host_import_map.json
```

The code that consumes this map is `src/host/xaudio2_stub.c` and
`src/host/steam_stub.c`; `tests/selftest_host_shims.c` drives both through real
x86 call thunks and checks every stack purge listed here.

---

## 1. Which COM objects the game creates

Only four `CoCreateInstance` sites exist in the whole image
(`call dword ptr [IAT 0x8FD308]`). Each GUID appears exactly once in `.rdata`.

| Site | CLSID | IID | What it is |
|---|---|---|---|
| `0x7B22CB` | `{5a508685-a254-4fba-9b82-9a24b00306af}` @`0x908E60` (or `{db05ea35-0329-4d4b-a53a-6dead03d3852}` @`0x908E70` for the debug engine) | `{8bcf1f58-9fe7-4583-8ac6-e2adc465c8bb}` @`0x908E50` | **XAudio2 2.7** (`IID_IXAudio2`) |
| `0x88AAFC` | `{3901cc3f-84b5-4fa4-ba35-aa8172b8a09b}` @`0x903574` | `{c50a7e93-f395-4834-9ef6-7fa99de50966}` @`0x903584` | **DirectSound8** (`IDirectSound8`) |
| `0x870246` | `{bcde0395-e52f-467c-8e3d-c4579291692e}` @`0x91D934` | `{a95664d2-9614-4f35-a746-de8db63617e6}` @`0x91D944` | **WASAPI** `IMMDeviceEnumerator` |
| `0x75FFB3` | `{4590f811-1d3a-11d0-891f-00aa004b2e24}` | `{dc12a687-737f-11cf-884d-00aa004b2e24}` | WMI `IWbemLocator` — XInput VID/PID enumeration, not audio |

The CLSID choice at `0x7B22CB` is decided by `(flags & 1)`, i.e. the XAudio2
`XAUDIO2_DEBUG_ENGINE` bit — the wrapper is a hand-rolled `XAudio2Create`.

## 2. The wrapper every consumer goes through

`FUN_007B22A8` (`0x7B22A8`, 92 bytes) — `HRESULT XAudio2CreateLike(void **out, UINT32 flags, UINT32 processor)`:

```c
clsid = (flags & 1) ? &CLSID_XAudio2Debug : &CLSID_XAudio2;
hr = CoCreateInstance(clsid, NULL, CLSCTX_INPROC_SERVER, &IID_IXAudio2, &xa);
if (SUCCEEDED(hr)) {
    hr = xa->Initialize(flags, processor);   /* vtbl+0x14, 3 stack dwords */
    if (FAILED(hr)) xa->Release();           /* vtbl+0x08 */
    else *out = xa;
}
```

Callers — **two independent consumers**, which is why the stub covers both:

| Caller | Address | Consumer |
|---|---|---|
| `FUN_007B2E24` | `0x7B2E85` | CRI NcVoice XAudio2 1.04.01 device module |
| `FUN_0088B0C0` | `0x88B0DE` | a second device class: own mastering voice, float32 source voice, 8-slot ring. Sibling of the DirectSound8 factory `FUN_008793F0`; its runtime owner (Wwise backend vs Platinum `Hw` layer) is **not proven here**. |

## 3. IXAudio2 vtable (2.7 order), slot by slot

`this` is the **first stack argument** (`__stdcall`), not ECX — proven by the
code shape `mov eax,[esi] / mov ecx,[eax] / push eax / call [ecx+0x64]`, where
`eax` is pushed *and* its vtable is loaded into `ecx`. A `thiscall` reading of
these sites would put `this` in the wrong place.

| Off | Method | Stack dwords (incl. `this`) | Guest site(s) | Notes |
|---|---|---|---|---|
| `0x04` | `AddRef` | 1 | `0x7B2ECF`, `0x88B0FC` | re-using an existing device (`DAT_009F7694`) |
| `0x08` | `Release` | 1 | `0x7B22FC`, `0x7B23A3`, `0x88B7E0` | |
| `0x0C` | `GetDeviceCount(UINT32*)` | 2 | `0x7B2EE6` | 0 devices ⇒ CRI gives up (see §5) |
| `0x10` | `GetDeviceDetails(UINT32, XAUDIO2_DEVICE_DETAILS*)` | 3 | `0x88B141` only — CRI never asks | Hw mixer reads `OutputFormat.nChannels` at **+0x406** |
| `0x14` | `Initialize(UINT32 flags, UINT32 processor)` | 3 | `0x7B22E3` | processor = `0xFFFFFFFF` |
| `0x18` | `RegisterForCallbacks(IXAudio2EngineCallback*)` | 2 | `0x7B2EA7` (CRI cb `0x009B38A0`), `0x88B126` (Hw cb) | never invoked by the stub |
| `0x1C` | `UnregisterForCallbacks` | 2 | `0x7B2392`, `0x88B7D7` | |
| `0x20` | `CreateSourceVoice(pp, fmt, flags, ratio, cb, sends, effects)` | 8 | `0x7B2C60`, `0x88B2A8` | float32 `WAVE_FORMAT_EXTENSIBLE` 48 kHz |
| `0x24` | `CreateSubmixVoice` | — | *(not called)* | implemented anyway |
| `0x28` | `CreateMasteringVoice(pp, inCh, inRate, flags, szDeviceId, chain)` | 7 | `0x7B2F50`, `0x88B116` | inCh 0, rate 48000, device id = index `DAT_009F76A8` (0) |
| `0x2C` | `StartEngine` | — | *(not called; XAudio2 starts on first voice)* | |
| `0x30` | `StopEngine` | — | *(not called)* | |
| `0x34` | `CommitChanges(UINT32 opset)` | 2 | `0x7B237A`, `0x7B25EE`, `0x7B2657` | opset `0x7FFFFFFF` (`DAT_009B3898`) |
| `0x38` | `GetPerformanceData` | 2 | `0x88B795`, `0x88B8D1` (probable — base register not resolved) | |
| `0x3C` | `SetDebugConfiguration` | — | *(not called)* | |

## 4. Voice vtables

`IXAudio2Voice` (19 slots, `0x00`–`0x48`) and `IXAudio2SourceVoice`
(+10 slots, `0x4C`–`0x70`). The voices are **not** `IUnknown`-derived: slot
`0x00` is `GetVoiceDetails` and `0x48` is `DestroyVoice`, which is only
consistent with the 2.7 layout (with `IUnknown` they would shift by 12 bytes).

| Off | Method | Dwords | Guest site(s) |
|---|---|---|---|
| `0x00` | `GetVoiceDetails` | 2 | `0x7B2F82` (mastering, channels → `DAT_009F76AC`) |
| `0x30` | `SetVolume(float, opset)` | 3 | `0x88AE0C`, `0x88AF43`, `0x88B2CA` |
| `0x40` | `SetOutputMatrix(dst, srcCh, dstCh, levels, opset)` | 6 | `0x7B29B5` (CRI downmix, `FUN_007B27E2`) |
| `0x48` | `DestroyVoice` | 1 | `0x7B235F`, `0x7B269C`, `0x88AF14`, `0x88AFBA`, `0x88B6ED`, `0x88B7BF` |
| `0x4C` | `Start(flags, opset)` | 3 | `0x7B27A8`, `0x7B2D28`, `0x88ACDA`, `0x88ADE6`, `0x88B6BA` |
| `0x50` | `Stop(flags, opset)` | 3 | `0x7B2661`, `0x7B27AD`, `0x7B2D4D`, `0x7B3056` |
| `0x54` | `SubmitSourceBuffer(const XAUDIO2_BUFFER*, const XAUDIO2_BUFFER_WMA*)` | 3 | `0x7B2757`, `0x88B4D8`, `0x88B77D` |
| `0x58` | `FlushSourceBuffers` | 1 | `0x7B2669`, `0x7B2D55`, `0x7B305E` |
| `0x5C` | `Discontinuity` | 1 | `0x7B2E02` |
| `0x64` | `GetState(XAUDIO2_VOICE_STATE*)` | 2 | `0x7B267F`, `0x7B26C9`, `0x7B2DC7`, `0x88B454` |
| `0x68` | `SetFrequencyRatio(float, opset)` | 3 | `0x7B27D3` |

`GetState` takes **one** argument here (no `Flags`): every site pushes only
`&state` plus `this`. That is the pre-2.8 signature, so the stub declares it
with two parameters — a three-parameter declaration would pop four extra bytes
and desynchronise the guest stack.

`XAUDIO2_VOICE_STATE` is `{ void *pCurrentBufferContext; UINT32 BuffersQueued;
UINT64 SamplesPlayed; }` — 16 bytes, and the guest reads `BuffersQueued` at +4.

### The two callback objects (guest code, never called by the stub)

| Object | Vtable | Slots |
|---|---|---|
| `CriNcvXAudio2EngineCallback` (`.?AVCriNcvXAudio2EngineCallback@@`) | `0x00908F7C` | `OnProcessingPassStart` → `0x7B2A05` (ret), `OnProcessingPassEnd` → `0x7B2A05`, `OnCriticalError` → `0x7B2A08` (logs) — **no IUnknown base**, 3 slots |
| `CriNcvXAudio2VoiceCallback` (`.?AVCriNcvXAudio2VoiceCallback@@`) | `0x00908FD8` | 7 slots; **`OnBufferEnd` = `0x7B2A24` = `*(UINT32 *)pBufferContext += 1`** |

`OnBufferEnd` only bumps a counter. CRI's feed loop is driven by
`GetState().BuffersQueued` (`FUN_007B26B7` submits while queued < 4), so the stub
does not have to call back into guest code to keep the engine fed — that is why
M2 can be silent without a guest-thread trampoline. M4 keeps the same vtables
and adds a real sink; if a callback is ever needed it must be invoked with
`native32_call_guest`, never by a raw call.

## 5. What the engine does with a failing or missing device

`FUN_007B2E24` (CRI device init) has three observable fallbacks:

1. `GetDeviceCount() == 0` → `W2010122202:No sound devices are available.` → silent module.
2. `CreateMasteringVoice` fails → `E2009042302:Failed to create mastering voice…` → silent module.
3. Anything left with no mastering voice →
   `W2010122204:XAudio2 API is not available. Atom library operates without sound output.`
   and the module table (`PTR_FUN_009B3818`) is pointed at the Pseudo (silent) module.

So a stub that simply made `CoCreateInstance` fail would also run the game
silently — but it takes the engine down a code path the shipping build never
uses. The stub instead behaves like a real device, which is the safer default
and the base M4 needs.

Also note the failure string that names the real risk:
`E2009042301:Failed to create XAudio2 object … The version of DirectX SDK might
be different from Atom library's.` — CRI 1.04.01 is 2.7-only. **Do not point it
at a 2.8/2.9 CLSID**; `CLSID_XAudio2` (2.6/2.7) is the only correct one.

## 6. M2 silence is not only XAudio2 (correction to the plan's §2 wording)

The plan says "audio shim = one COM factory returning a host mixer". The
binary disagrees in one important way: there are **three** audio device paths.

| Path | Created by | State in a Steam-free host |
|---|---|---|
| XAudio2 2.7 | `FUN_007B22A8` ← CRI NcVoice + Hw mixer | **shimmed silent** (`src/host/xaudio2_stub.c`) |
| DirectSound8 | `CoCreateInstance` at `0x88AAFC` ← `FUN_0088AAE0` from the Hw device factory `FUN_008793F0` | forwarded to the real `ole32` today — **audible** on Windows |
| WASAPI `IMMDeviceEnumerator` | `0x870246` in `FUN_008701F0` (device notifications for a mixer) | forwarded — real device enumeration |

Consequence for M2.9's acceptance ("game runs silent"): stubbing XAudio2 covers
the CRI and Hw XAudio2 devices, but if the engine selects its DirectSound8 or a
WASAPI output, the host will make noise. That is not a bug in the shim; it is a
decision M2 has to make explicitly (accept sound, or stub the other two as
well). Track A's `docs/audio.md` recommends keeping the engine's own mixer, so
the cheapest correct choice for M2 is usually **leave DS8/WASAPI alone** and
only require that no crash comes from the audio stack.

## 7. steam_api (seven functions, flat pre-1.37 API)

| Import | IAT VA | Calling convention | Call site |
|---|---|---|---|
| `SteamAPI_Init` | `0x8FD328` | `__cdecl` | `0x75DCFB` (`FUN_0075DCD0`) |
| `SteamAPI_Shutdown` | `0x8FD32C` | `__cdecl` | `0x75DC7F`, `0x75DCD9` |
| `SteamAPI_RunCallbacks` | `0x8FD324` | `__cdecl` | `0x75DD36` |
| `SteamAPI_RegisterCallback` | `0x8FD320` | `__cdecl` | `0x75DBF0` (`push 0x14B / push eax / call / add esp,8`) |
| `SteamAPI_UnregisterCallback` | `0x8FD31C` | `__cdecl` | `0x75DCB1` (`push eax / call / add esp,4`) |
| `SteamUser` | `0x8FD330` | `__cdecl` | 6 sites: `0x632195`, `0x6321EA`, `0x6326EA`, `0x633006`, `0x75DD09`, `0x76C10F` |
| `SteamUserStats` | `0x8FD318` | `__cdecl` | `0x76C118` (stored, never called through) |

**Bug fixed by this track:** the original `shims.c` popped the two arguments of
`RegisterCallback`/`UnregisterCallback` itself. Both are `__cdecl` — the caller
emits `add esp,8` — so that shim would have left `g_esp` 8 bytes too high and
corrupted the lifted stack on the first call. The shims now pop only the return
address, and the selftest asserts the exact `g_esp` delta.

**Interface use.** The only method call on either interface in the entire binary
is `ISteamUser::GetSteamID` (slot 2, `+0x08`):

```
0x632195  call [SteamUser]
0x63219B  mov  edx,[eax]          ; vtable
0x63219D  lea  ecx,[ebp-0x498]    ; &id
0x6321A3  push ecx                ; hidden return pointer
0x6321A4  mov  ecx,eax            ; this  (thiscall!)
0x6321A6  call [edx+8]
0x6321A9  mov  ecx,[eax]          ; the id is read through EAX
```

That is a **`__thiscall` with a hidden return pointer** — the opposite of the
XAudio2 convention, in the same binary. The stub implements it as
`__fastcall(self /*ecx*/, out /*edx*/, out_dup /*stack*/)` behind a two
instruction `mov edx,[esp+4] / jmp impl` thunk, so MSVC's own epilogue pops
exactly one dword (`ret 4`) as the original callee would.

`ISteamUserStats()` is only null-checked (`FUN_0076C0F0`), so its vtable answers
"0" for every slot and logs if that ever changes.

## 8. Open items this map hands to M4 / M2

1. **`0x88B8E2` → `[edx+0x148]`**: the Hw mixer dispatches into its own object at
   +0x148; not explored here (not needed for the stub, relevant if Hw audio is
   made audible).
2. **`GetPerformanceData` sites** (`0x88B795`, `0x88B8D1`) were classified by
   offset only; the base register was not resolved. Our implementation zeroes
   the buffer, which is safe either way.
3. **Callback necessity** is proven for CRI (GetState-driven) but only *inferred*
   for the Hw mixer: it also calls `GetState` (`0x88B454`) and never reads a
   counter that `OnBufferEnd` writes. If M2's log shows the Hw mixer stopping
   after its first 8 buffers, the fix is a guest callback trampoline, not a
   different queue model.
4. **`XAUDIO2_BUFFER.LoopCount` / `XAUDIO2_BUFFER_WMA`** are ignored by the stub;
   neither consumer passes a loop count in the paths observed. Re-check if
   looping music ever comes out short.
5. **Sample rate**: both consumers ask for 48000; `DAT_009B43B0` is read straight
   into `CreateMasteringVoice` by the Hw mixer and was not resolved to a
   constant. If it is ever not 48000, the stub follows the argument, not this doc.
