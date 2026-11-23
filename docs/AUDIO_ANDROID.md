# Audio on Android - the AAudio side (M4 hand-off to M3)

Written by the M4 (Windows audio) session on 2026-11-21 as a hand-off. **It was
picked up on 2026-11-23 and is now integrated and measured on the device**: the
Android XAudio2 shim exists (`src/host_android/xaudio2_android.c`), the AAudio
sink runs, and the CRI movie audio is audible with the numbers in §6. The Wwise
voice is now fed as well: the missing piece was the **voice callback** the shim
never invoked (the engine's `OnVoiceProcessingPassStart` is the feed and
`OnBufferEnd` credits the mixer), fixed with an XAudio2-style worker thread and
measured as continuous menu music - see `docs/audio.md` §10.9. Sections 1-7 are
kept as written: the contract did not change, and each pitfall below cost
exactly what it says.

Read `docs/audio.md` section 9 first: it is the reason the host provides a sink
at all. The engine (Wwise, inside the lifted C) decodes and mixes; the host only
has to hand the mixed PCM to a device and tell the engine when it has been
consumed.

## 1. The contract

```
engine thread                       sink thread (one, owned by the device)
  SubmitSourceBuffer(PCM slice)  --> mixer queue
  GetState() / OnBufferEnd       <-- mixer retires the slice
                                     render_fn(float *dst, int frames)  <-- sink
```

* `render_fn` fills **interleaved stereo float32 at 48000 Hz**, exactly
  `frames * 2` floats. It must not block, allocate, log per block, or enter
  guest code.
* The sink owns the only audio thread. It asks for exactly the frames the
  device wants and does nothing else.
* The queue between engine and sink is the only shared state. On Android that
  is a `pthread_mutex` (recursive) around the mixer's queue, not the machine
  lock: the AAudio callback thread must never take the machine lock.
* Retirement is the engine's clock: when the sink has *played* a slice, the
  mixer pops it, updates `SamplesPlayed`/`BuffersQueued`, and performs the
  guest's `OnBufferEnd` body. CRI waits on that counter, so this is not
  optional (see the 2026-11-20 ledger entry: without it the boot froze).

`src/host/audio_sink.h` is the platform-neutral contract: start/stop/live/
backend/get_stats/note_underrun plus `korra_audio_stats`.

## 2. What exists today

| file | platform | state |
|---|---|---|
| `src/host/audio_sink.h` | both | the contract above |
| `src/host/wasapi_sink.c` | Windows only (`#ifndef __ANDROID__`) | built, run, measured: WASAPI shared event-driven with `AUTOCONVERTPCM`, waveOut fallback, null-pacing fallback, WAV dump, per-second peak/RMS/underrun log |
| `src/host/aaudio_sink.c` | Android only (`#ifdef __ANDROID__`) | built into the APK, run on the S24, measured (§6); device-rate resampler added |
| `src/host_android/xaudio2_android.c` | Android host | the 2.7 XAudio2 shim **and** the mixer - the port of `xaudio2_stub.c`, measured against it |
| `src/host/xaudio2_stub.c` | Windows host | the 2.7 XAudio2 shim **and** the mixer (slice queue, resampling, matrix, retirement) - the reference the Android shim matches |

`aaudio_sink.c` in one paragraph: `start()` builds an AAudio stream
(exclusive first, shared on refusal, low-latency, float32, 48 kHz, 2 ch,
capacity 2x256 frames), captures the *actual* channel count / rate / format,
requests start, then opens the WAV dump. The data callback calls `render_fn`
into the buffer when the device really is stereo float32, otherwise into a
scratch buffer converted to the device format (I16 path included). It logs one
line per second with frames/peak/RMS/quiet, counts underruns, and writes 16-bit
48 kHz stereo WAV to `$KORRA_AUDIO_DUMP` with the header patched every second
so a killed run still yields a readable file.

Compile check used (NDK 28.2.13676358, no warnings):

```
clang --target=aarch64-linux-android26 -std=c11 -O2 -Wall -Wextra \
      -Wno-unused-parameter -c src/host/aaudio_sink.c -Isrc/host
```

## 3. What M3 still has to write

The Android host has **no XAudio2 shim at all**. `src/host_android/` contains
d3d9, kernel32, user32, fs, memory, input and spine, but nothing audio. So the
port is not "add a sink": it is "add the device, then the sink is ready".

`src/host/xaudio2_stub.c` is the reference implementation and it is *not*
portable as-is. Replacements:

| Windows idiom in `xaudio2_stub.c` | Android replacement |
|---|---|
| `windows.h` types (`UINT32`, `HRESULT`, `GUID`) | local typedefs; GUIDs as 16-byte structs (values are in `docs/audio.md` 9.1) |
| `CRITICAL_SECTION` + lazy init | `pthread_mutex_t` recursive, initialized in the shim's init |
| `xa2_readable()` via `VirtualQuery` | not needed: `src/host_android` maps the whole 32-bit space (`KA_WINDOW_SIZE`), so `ka_host(va)` is always a valid pointer; keep a `va >= 0x10000` sanity check |
| `QueryPerformanceCounter` clock | `clock_gettime(CLOCK_MONOTONIC)`; only used by the no-sink fallback |
| `fprintf(stderr, ...)` logging | `ka_log()` / `__android_log_print` |
| `native32_call_guest()` | `hle_call_guest(va, nargs, args)` from `host_android.h` |
| `__stdcall` COM methods with `this` as the **first stack argument** | the ARM64 port must use the C calling convention (first argument in `x0`); the *guest* x86 `stdcall` calling convention only exists inside the lifted C, which calls the shim through the Win32 HLE table in `kernel32_android.c` - so the shim entry points are called from C, not from guest `push`es |

Keep these semantics exactly (each one cost a defect on Windows):

1. Answer **both** CLSIDs (`CLSID_XAudio2`, `CLSID_XAudio2_Debug`) and
   `IID_IXAudio2`; `E_NOINTERFACE` for anything else.
2. `GetDeviceCount()` returns 1 (CRI refuses 0 and goes silent).
3. The mastering voice must report **2 ch / 48000 Hz** even when the engine
   asks for 0 channels, so the engine downmixes 5.1 and quad itself.
4. `SetOutputMatrix` is **authoritative**: the engine installs a 2x2 matrix for
   its stereo voice and a 6x2 for the 5.1 voice. Applying invented
   coefficients instead put the mix +11 dB hot and clipped (`+2.7 dBFS`).
   Store the levels and use them in the downmix.
5. `GetState` is the 2.7 shape: `GetState(XAUDIO2_VOICE_STATE *)`, one
   argument; `SamplesPlayed` and `BuffersQueued` must be honest or CRI either
   stalls (late) or drops audio (early).
6. Accept the engine's format: 16-bit PCM or float32, mono through 5.1, 32 kHz
   or 48 kHz, `WAVEFORMATEX` or `WAVEFORMATEXTENSIBLE`. The mixer resamples to
   48 kHz; do not ask the engine to change rate.
7. Start the sink lazily on the first `SubmitSourceBuffer` with frames > 1
   (the Windows file does this in `xa2_ensure_sink()`), not on device creation:
   the game creates and destroys a whole XAudio2 device per movie.
8. Never enter guest code from the mixer. The callback body that matters is a
   plain `*(u32 *)pBufferContext += 1`; do it in C.

## 4. Integration steps (exact)

Done 2026-11-23, as written except where noted:

1. `src/host/audio_sink.h` and `src/host/aaudio_sink.c` are **not** copied into
   `src/host_android/`: the CMake target lists them from `src/host` directly
   (one copy, no drift) and adds `src/host` to the include path.
2. `target_link_libraries(korra PRIVATE aaudio)` on the final target - done.
3. `minSdkVersion` is 29 (AAudio needs 26) - done.
4. `src/host_android/xaudio2_android.c` is picked up by the existing
   `file(GLOB ...)`; `CoCreateInstance` in `misc_android.c` hands the XAudio2
   CLSIDs to `ka_xaudio2_create_instance()` before falling through.
5. The sink starts from the first `SubmitSourceBuffer` (`ka_xa2_render` is the
   render function) and `korra_guest_stop()` calls `korra_audio_sink_stop()`.
6. Nothing else in the Android target changed.

## 5. Knobs

Shared with Windows:

| variable | meaning |
|---|---|
| `KORRA_AUDIO_DUMP=<path>` | write 16-bit stereo WAV at the device rate. On device: `/data/local/tmp/korra.wav`, then `adb pull`. Header is patched every second, so a killed run is still readable |
| `KORRA_AUDIO_LOG=1` | (Windows) per-second sink line; AAudio logs per second unconditionally via logcat |

Android properties (`adb shell setprop ...`, read once at start):

| property | meaning |
|---|---|
| `debug.korra.audio.dump=1` | dump to `KORRA_AUDIO_DIR/korra_audio.wav` (the shell sets that to the app's own external files dir, which `adb pull` can read) |
| `debug.korra.audio.dump=<path>` | dump to that literal path |
| `debug.korra.audio.buffer=<frames>` | device buffer request, default 512 (the S24 rounds it to 576, 12.0 ms); `1024` gives 1056 frames, 22.0 ms |
| `debug.korra.audio.backend=null` | open no device; the XAudio2 shim then retires buffers on its clock and CRI still feeds |
| `debug.korra.xa2=0..3` | shim log level: 1 voice create/destroy/submit-first, 2 every state poll and the sound-table probe, 3 every queue-length change |

Windows-only today: `KORRA_AUDIO_BACKEND=wasapi|waveout|null|none`,
`KORRA_AUDIO_LATENCY_MS`, `KORRA_AUDIO_CENSUS`.

## 6. Acceptance on the device (same list as `docs/audio.md` 9.4)

Silence is not success. In order:

1. `KORRA_AUDIO_DUMP=/data/local/tmp/korra.wav`, run to the title, pull it and
   measure: `RMS > -40 dBFS` (Windows reaches `-26.9 dBFS` in the movie/boot
   phase with peak `-9.8 dBFS`). A 0-byte or all-zero file is a failure even if
   the game "runs".
2. Menu music: `bgm_TitleMenu` (60855595), 49.7 s.
3. Longest cue: `bgm_cs41_skip` (1165345047), 94.9 s, 5.1 source downmixed.
4. One-shot over a running cue: `bgm_Levelup` (3367338161) over the cue.
5. Control-only event: `bgm_Sync_Pause` (803349658) then `bgm_Sync_Resume`
   (2335748301) - the music pauses and resumes, it does not stop.
6. Voice: `korra_vo_cont_yes` (14045797), 32 kHz mono, once.
7. Logs: every `CreateSourceVoice` format printed, zero `OnVoiceError`, and
   underruns below 1/minute in steady state (Windows: 0/min between 6 s and
   21 s; the 4 counted underruns are the first feed and the per-movie device
   teardown).

**As measured on the S24 (2026-11-23).** Test 1 passes on the device:
`opened: 2 ch, 48000 Hz, buffer 576 frames (12.0 ms, capacity 3072), burst 96`,
first audible block at 1.0 s (peak -15.7 dBFS), 3-26 s peak **-9.8 dBFS** /
RMS **-27.7 .. -28.8 dBFS**, dump 207 s of 16-bit 48 kHz stereo, AAudio xruns 3
(at 7 s) then 0 new. Format clause of test 7 passes (every voice format logged,
0 `OnVoiceError`); its underrun clause does **not**: 96 mixer-dry blocks in the
first 26 s, because the guest's ~30 fps feed cannot keep a 22 ms buffer fed
(the 1056-frame setting was measured too and did not remove them). Tests 2-6
remain blocked by the Wwise feed, whose break is now named (see below).

Tests 2-6 need a posted Wwise event. The title/menu now prove that events were
posted and mixed all along: the voice was never *queued* because the shim did
not invoke the engine's `IXAudio2VoiceCallback`. With the callback worker in
`xaudio2_android.c` (see `docs/audio.md` §10.9) the Hw voice is fed from
152-418 ms and the 360 s route's dump has continuous menu music from 73 s to
the New Game at 322 s (peak -7.5..0 dBFS, RMS -30..-35 dBFS), then the loading
transition at RMS -17..-25 dBFS. The 5.1 cutscene remains unmeasured on the
device (the route had 10 s left when it reached the New Game, and no new CRI
6 ch voice is created on that path). The Windows oracle is still unfed: its
shim has the same missing invocation. The sink's float meter also reports
`peak=186.6 dBFS rms=nan` in one run once the Wwise mix plays (the boot movies
are clean), and the dump clips 0.2-0.3 % of samples - a new, open break in the
mix data (NaN/Inf is invisible to the `|x| > 2` probe).

## 7. Pitfalls, all learned the hard way

* **Do not invent the downmix.** The engine's `SetOutputMatrix` is the mix.
* **Do not call the guest from the mix callback.** On Windows the attempt was
  rejected before it was tried (register/stack discipline); on Android the
  machine lock makes it worse.
* **Do not hold the mixer lock while calling into AAudio.** Lock, copy to the
  device buffer, unlock.
* **AAudio may refuse exclusive** (calls, notifications, another app). Falling
  back to shared is normal, not an error.
* **AAudio may return a rate other than 48000** (44.1 kHz is common) and I16
  instead of float. The I16 path is implemented; the rate path is *not* - the
  sink logs a WARNING and M3 must add a resampler (the Windows sink has one:
  `convert_block()` in `wasapi_sink.c`, carried position across blocks).
* **Buffer 2x256 frames** is the starting point from `docs/audio.md` 9.3; raise
  it if logcat shows underruns, but every doubling adds latency the engine
  does not need. Measured 2026-11-23: 576 frames (12 ms) and 1056 frames (22 ms)
  both leave ~4 mixer-dry blocks per second during the movies; the limit is the
  guest's feed cadence, not this buffer.
* **The WAV dump is the evidence**, not "I heard it". It is the only artifact
  that survives `adb pull` after a crash.
* **Patch the WAV header, then seek back to the end.** `dump_patch()` that
  writes 44 bytes at offset 0 and leaves the stream there makes every later
  block overwrite the file's start; a 138 s run then pulled as 192 KB whose
  header claimed 138 s. This was in the hand-off code and only a pull caught it.
* **Never move the mix cursor backwards.** The Windows mixer clamps `pos` to
  `played` when the queue empties; with a live sink that re-reads a slice whose
  `OnBufferEnd` has already fired, and CRI is free to refill it. Measured as one
  torn float (`-20071`) at a slice edge. `next_end = pos`, never `pos = played`.
  That fixed one cause, not the symptom: rare out-of-range samples still appear
  (2 in a 50 s device run, channel 0 of the 6 ch voice) and the probe cannot yet
  tell a torn refill from a partially decoded buffer. Keep the probe.
* **A COM vtable's slot count is the ABI.** Each thunk must pop exactly
  `1 + params`; `GetState` in 2.7 is one argument. The Android shim's slots are
  the same 16/19/29 layout as the Windows one for this reason.
* **A GUID is not a byte string you can copy from the text form.** Data1..Data3
  are little-endian: `{...-A254-...}` is `54 A2`. Writing `A2 54` made every
  XAudio2 request answer `REGDB_E_CLASSNOTREG` - indistinguishable from "the
  engine never asked".
