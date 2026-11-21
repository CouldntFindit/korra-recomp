# Audio on Android - the AAudio side (M4 hand-off to M3)

Written by the M4 (Windows audio) session on 2026-11-21. The Windows sink is
built, run and measured; the Android sink is **code only** - it has been
compile-checked with the NDK for arm64-v8a but has never been linked into the
Android target and has never run on a device. Do not read this file as evidence
that audio works on Android.

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
| `src/host/aaudio_sink.c` | Android only (`#ifdef __ANDROID__`) | compile-checked, **not** built into the APK, never run |
| `src/host/xaudio2_stub.c` | Windows host | the 2.7 XAudio2 shim **and** the mixer (slice queue, resampling, matrix, retirement) - the reference the Android shim must match |

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

1. Copy `src/host/audio_sink.h` and `src/host/aaudio_sink.c` into
   `src/host_android/`. The CMake `file(GLOB KORRA_HOST_SOURCES ...)` already
   picks up every `.c`/`.cpp` there, so no new source line is needed.
   Do **not** copy `wasapi_sink.c`; it compiles to nothing off Windows.
2. Add the AAudio library to the link. `korra_guest` is an OBJECT library, so
   the flag belongs on the final target:

   ```cmake
   target_link_libraries(korra PRIVATE aaudio)
   ```

3. `minSdkVersion` must be 26 or higher (AAudio). Check
   `android/app/build.gradle` before enabling.
4. Add `xaudio2_android.c` (section 3) to `src/host_android/` and register its
   shim(s) where `main_android.c` registers the other HLE tables - the only
   entry that matters is `CoCreateInstance`, and it must intercept
   `CLSID_XAudio2` before falling through to the real thing.
5. Call `korra_audio_sink_start(xa2_render_audio)` from the first submit, and
   `korra_audio_sink_stop()` from the Activity/teardown path (`aaudio_sink.c`
   does not register an `atexit` handler; the Windows sink does).
6. Nothing else in the Android target changes. D3D9, the spine and the
   interrupt/timer work are untouched by this hand-off.

## 5. Knobs

Shared with Windows:

| variable | meaning |
|---|---|
| `KORRA_AUDIO_DUMP=<path>` | write 16-bit 48 kHz stereo WAV. On device: `/data/local/tmp/korra.wav`, then `adb pull`. Header is patched every second, so a killed run is still readable |
| `KORRA_AUDIO_LOG=1` | (Windows) per-second sink line; AAudio logs per second unconditionally via logcat |

Windows-only today: `KORRA_AUDIO_BACKEND=wasapi|waveout|null|none`,
`KORRA_AUDIO_LATENCY_MS`, `KORRA_AUDIO_CENSUS`. If M3 wants a backend switch on
Android, add `KORRA_AUDIO_BACKEND=aaudio|null` to `korra_audio_sink_start()`.

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

Tests 2-6 need a posted Wwise event. On Windows those are **not reachable
today**: the game never posts one on any screen it can reach, and the
title-to-menu transition dies upstream of audio (see `docs/FINDINGS.md`,
2026-11-21, frame-pass-queue fault). Treat that as a separate blocker, not as
an audio defect: the sink has nothing to play, which is why the title is quiet.

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
  does not need.
* **The WAV dump is the evidence**, not "I heard it". It is the only artifact
  that survives `adb pull` after a crash.
