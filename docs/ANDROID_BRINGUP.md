# Android bring-up plan (M3)

**Status:** M3 prep, pre-gate. Written on branch `m3-prep` (worktree
`recomp-work\korra-recomp-android`) while the M2 session owns the Windows host.
Nothing here has been run on a phone; every claim that needs a device is marked
as such.

**What exists after this branch:**
- a Gradle project in `android/` that builds an arm64-v8a APK, installs, and
  logs (shell only — the recompiled guest is not linked yet);
- the installed toolchain, with exact versions, in `docs/TOOLCHAIN.md`;
- a measured assessment of the `pcmpistri` arm64 risk (section 7) plus a
  differential harness that proves what is and is not portable.

**What deliberately does not exist yet:** the Android host (`src/host_android/`),
the DXVK integration, the touch HUD, and any device-verified frame. Those start
after the M2 gate.

---

## 1. Why the shell is not the hard part

The reference ports make this look like a UI problem. It is not: the work is a
32-bit x86 guest on a 64-bit ARM host (memory model, ABI, DXVK) plus four
Android-specific gaps (input, file picker, log, frame pacing). The shell choice
only decides who owns the window and the input queue.

Reference material studied for this plan (patterns only; no code copied):

| Source | What it settles | Licence |
|---|---|---|
| `gamboozino/ngbrecomp` | Android arm64 lessons: TLS slot, ADPF, signal stacks, SAF flow, log file, 16 KiB pages | GPL-3.0, adaptable with attribution |
| `codepdbh/nfsu2-recompiled` (`ports/dxvk_android/`, `android/`) | the only public D3D9→DXVK-native Android port of a static recomp in this workspace | **no LICENSE file** — study only, re-derive |
| DXVK native 1.9.2b (`Joshua-Ashton/dxvk-native`) | D3D9 implementation, zlib | usable directly, keep the notice |
| `MikeSemicolonD/RogueSquadron64Recomp` | agent workflow only (its `AGENTS.md`); it is not in this workspace and has no Android port | GPL-3.0 |

`ngbrecomp` note: it does **not** use Gradle. It builds with `javac` + `d8` +
`aapt2` + `apksigner` from `packaging/android/build.sh` and runs an SDL2
Activity. That is a valid choice for one developer on Linux; this project uses
Gradle because the project is published source-only and builders should get a
working build from Android Studio without a hand-written packaging script.

## 2. GameActivity vs NativeActivity

**Decision: GameActivity** (`androidx.games:games-activity`, pinned 4.4.2).

| | GameActivity | NativeActivity + `native_app_glue` |
|---|---|---|
| Window | owns a `SurfaceView`, exposes `ANativeWindow` through the glue | `ANativeWindow` arrives in `APP_CMD_INIT_WINDOW` |
| Input | delivers `AInputEvent`s to the native thread, with filters and IME/soft-keyboard support | delivers raw events; IME is effectively unusable |
| Lifecycle | `APP_CMD_*` plus explicit pause/resume hooks | `APP_CMD_*` only |
| Java side | an `AppCompatActivity` subclass — where the SAF picker lives | you write your own Activity anyway for SAF |
| Dependencies | AndroidX + AppCompat | none (but still needs an Activity for the picker) |
| Status | Google's recommended shell for new native games | not deprecated, but documented as legacy for games |

The deciding argument is not the surface or the input API: it is that this port
needs a Java Activity **regardless**, for the first-run file picker. Once that
is true, NativeActivity's only advantage (no Java) is gone, and GameActivity
gives a maintained input path instead of a hand-rolled one.

Cost accepted, stated honestly: AndroidX pulls ~9 MB of DEX into the APK
(`classes*.dex` dominate the 9.8 MB debug APK) and adds a Gradle dependency the
reference ports do not have. `nfsu2-recompiled` uses `native_app_glue`; if
GameActivity ever gets in the way, the fallback is a one-file change: replace
`jni_bridge.cpp`'s `android_main` with an `ANativeActivity_onCreate` and point
the manifest at `android.app.NativeActivity`. The rest of the plan (host,
DXVK, touch) does not depend on the shell.

Implementation details that cost time if rediscovered:
- The library name comes from manifest meta-data `android.app.lib_name`; there
  is no `getLibraryName()` override in 4.4.2 (`GameActivity` reads the
  meta-data and calls `System.loadLibrary`).
- GameActivity extends `AppCompatActivity`, so an AppCompat theme is mandatory
  and `androidx.appcompat` must be an explicit dependency (the AAR's POM
  declares none).
- `GameActivity_onCreate` **and** `JNI_OnLoad` must be forced into the `.so`
  (`-uGameActivity_onCreate -uJNI_OnLoad`). Neither is referenced by our code;
  without the second flag the app builds and then dies at first call with
  `UnsatisfiedLinkError`, because `JNI_OnLoad` is what registers the Activity's
  own native methods. CMake de-duplicates repeated `-u` tokens, so the flags
  must be written as single tokens.

## 3. What the scaffold does today (and what it proves)

```
android/
  settings.gradle.kts, build.gradle.kts, gradle.properties, gradlew(.bat)
  app/build.gradle.kts           AGP 8.13.2, compileSdk 36, minSdk 29, arm64-v8a
  app/src/main/AndroidManifest.xml
  app/src/main/java/org/korra/recomp/
      MainActivity.java          GameActivity shell + SAF first-run flow
      GameDataInstaller.java     SAF tree walk + copy into app storage
  app/src/main/cpp/
      CMakeLists.txt             two options: KORRA_LINK_GUEST, KORRA_WITH_DXVK
      jni_bridge.cpp             android_main(), placeholder guest entry
      log_android.c              log.txt + logcat pump
      install_android.c          install hand-off (JNI)
      perf_android.c             ADPF + ANativeWindow_setFrameRate via dlsym
```

Verified on this machine (2026-11-20):

| Check | Result |
|---|---|
| `gradlew :app:assembleDebug` | BUILD SUCCESSFUL, 1 m 56 s cold, APK 9.8 MB |
| ABIs in the APK | `lib/arm64-v8a/libkorra.so` (73 KB) + `libc++_shared.so` |
| Exported symbols | `android_main`, `GameActivity_onCreate`, `JNI_OnLoad`, both `Java_org_korra_recomp_MainActivity_*` |
| 16 KiB page alignment | all `PT_LOAD` aligned `0x4000` (`llvm-readelf -lW`) |
| `recomp_types.h` cross-compile | arm64 clang, API 29: compiles (portable `pcmpistri` path selected) |
| On-device run | **not verified — no device was attached to this machine** |

The frame loop logs once a second:

```
shell: android_main started (API 34, abi arm64)
shell: external files /storage/emulated/0/Android/data/org.korra.recomp/files
perf: adpf=yes setFrameRate=yes session=yes
window: created 2400x1080 format=1
guest: STUB - nothing linked. GameData=.../files/game/GameData window=0x...
```

That last line is the point of the scaffold: a shell build can never be
mistaken for a working port, in `log.txt` or in logcat.

### First-run install

The picker is `ACTION_OPEN_DOCUMENT_TREE` (not `ACTION_OPEN_DOCUMENT`, which
ngbrecomp uses for its single ISO): Korra ships a directory
(`GameData\` with `data000.cpk` 265 MB, `data001.cpk` 1.60 GB, plus loose
`sound\` and `movie\`); the engine opens several files by path at different
times. Both picker shapes are accepted — the game root that *contains*
`GameData`, or `GameData` itself — because that is the mistake users make.

- Copy target: `getExternalFilesDir(null)/game/GameData/` →
  `/storage/emulated/0/Android/data/org.korra.recomp/files/game/GameData/`.
  No storage permission is needed for that path on API 29+.
- Refuse under 2.6 GB free (`REQUIRED_BYTES`), checked before copying.
- The copy is Java-side on a worker thread with toasts every 32 files; a
  `install.complete` marker is written only after a complete pass, so a killed
  copy is retried on next launch.
- The native side never sees a `content://` URI. It gets one plain directory
  path, because the file layer it will host (CRI FS over POSIX `open`) cannot
  read a SAF document.
- The marker + both CPKs are the "installed" test; `GameDataInstaller.isInstalled`
  and the native log line both report it.

### Logging

`Android/data/org.korra.recomp/files/log.txt`, truncated per launch, every line
mirrored to logcat under the tag `Korra`. `stdout`/`stderr` are redirected
through a pipe into the same pump, so host `printf`s and DXVK's own logging
(once it is built against this) land in the file too.

```powershell
adb logcat -s Korra
adb pull /sdcard/Android/data/org.korra.recomp/files/log.txt
```

### Frame pacing

`perf_android.c` resolves two optional APIs with `dlsym` so the minSdk 29 build
still runs on older libandroid:

- `APerformanceHint_*` (ADPF, API 33): a hint session with a 16.67 ms target,
  `reportActualWorkDuration` per frame. ngbrecomp's measured effect was
  50 ms → 18 ms per frame on a Pixel 9a, i.e. the governor was the whole
  difference.
- `ANativeWindow_setFrameRate` (API 30): asks for 60 Hz on a 120 Hz panel.

Both are wired but idle in the scaffold (no guest). The M3 session should feed
`reportActualWorkDuration` from the real frame, and switch the target between
30 and 60 Hz from `DAT_00e4d210` (`docs/quirks.md` §4) instead of hard-coding.

## 4. How the lifted C and the host get compiled by the NDK

### What the guest actually is

`src/recomp/gen/` is 12 M lines of plain C (75 files, 613 MB). Per function the
lifter emits straight-line C with a **global register file** (`g_eax`, `g_esp`,
…) and **guest memory accesses through macros**:

```c
#define ADDR(va) ((uintptr_t)(uint32_t)(va) + g_mem_base)
#define MEM32(a) (*(RECOMP_MEM_QUAL uint32_t *)ADDR(a))
```

That is why the lift also works when compiled for arm64: it is C, not assembly.
But three assumptions need an explicit answer on a 64-bit POSIX host.

**4.1 Guest address space.** `g_mem_base` is a `ptrdiff_t` offset from guest VA
to host address, and every guest pointer stays a `uint32_t`. The guest expects
the image at its original base (`0x400000`), and `VirtualAlloc` returns guest
VAs that the engine stores in 32-bit fields. The Android host must therefore:

1. reserve a guest window large enough for the whole 32-bit space
   (`mmap(NULL, 4 GiB, PROT_NONE, MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE)`),
2. place the image inside it and set `g_mem_base = window - 0x400000`,
3. serve the guest's `VirtualAlloc`/`HeapAlloc` from a bump/free-list allocator
   *inside* that window, so every returned pointer is a valid guest VA.

`MAP_NORESERVE` matters: 4 GiB reserved is not 4 GiB of physical RAM, and the
phone will refuse the latter. This is the pattern nfsu2 documents
(`docs/ANDROID_PORT.md`); re-derive it, do not copy it. A `PROT_NONE` window
also gives a cheap fault signal for out-of-range guest pointers (the crash
triage tree in `AGENTS.md` §5 assumes that class of failure).

**4.2 32-bit guest on 64-bit host.** Two rules:
- never store a host pointer in guest memory; host objects (DXVK devices, file
  handles, threads) are addressed by 32-bit tokens or kept in host-side tables;
- the ABI at the guest/host boundary is still x86 **stdcall/cdecl with the
  guest stack**: the lifted code computes `esp` itself. Import trampolines must
  pop exactly what the guest expects (the `steam_api` `__cdecl` vs `__stdcall`
  bug in `docs/xaudio2_callsites.md` is the same class of error).

**4.3 Compiler settings.** Compile the generated C with the flags the Windows
build already proved (see `android/app/src/main/cpp/CMakeLists.txt`):

```
-O2 -fwrapv -fno-strict-aliasing -ffp-contract=off -w
-DRECOMP_GENERATED_CODE          (emitted by the lift driver, not by CMake)
```

`-fwrapv` because the guest relies on two's-complement overflow; `-fno-strict-aliasing`
because guest memory is one untyped byte array behind `volatile` (or nothing, if
`RECOMP_MEM_QUAL` is overridden); `-ffp-contract=off` because x87/SSE sequences
must not fuse into FMA and change rounding. Do not enable LTO for the guest in
the first arm64 build: it makes "which function is this" impossible in a crash
report, and the 613 MB compile already takes minutes.

### 4.4 What the host has to be replaced with

`src/host/*.c` is Windows-native by design (`native32`: real `USER32`/`GDI32`/
`D3D9` imports, VEH, TLS callbacks). It cannot compile for Android and must not
be edited in place — the M2 session owns it. The Android host is a **separate
tree**, `src/host_android/`, with the same shape (one concern per file, globbed
by the build). Suggested split, mirroring the existing ownership rule:

| File | Replaces | Notes |
|---|---|---|
| `main_android.c` | `src/host/main.c` | sets `g_mem_base`, maps the image, calls the entry VA |
| `memory_android.c` | native32's image/heap setup | 4 GiB window, guest `VirtualAlloc` |
| `fs_android.c` | `CreateFileA`/`ReadFile` | POSIX `open`/`read`, `\`→`/`, root = `<files>/game` |
| `window_android.c` | `RegisterClassExA`/`CreateWindowExA` | HWND/HINSTANCE as opaque tokens; real `ANativeWindow` behind them |
| `input_android.c` | `XInputGetState` ord 2 / `DInput8Create` | touch HUD → `XINPUT_GAMEPAD` at the import boundary |
| `d3d9_bridge.cpp` | real `d3d9.dll` | guest COM vtables → DXVK (section 5.4) |
| `audio_android.c` | `xaudio2_stub.c` | keep the stub; M4 adds AAudio |
| `steam_android.c` | `steam_stub.c` | reuse the same 7 stubs |

**Feed input at the import boundary, not into engine internals.** Korra reads
XInput through the import and then remaps into its own 0x24-byte pad state
(`docs/quirks.md` §5). A host `XInputGetState` that fills a normal
`XINPUT_GAMEPAD` therefore gets the engine's remap and edge detection for free;
writing the engine's internal structure directly would duplicate the edge
logic that lives at `0xe4e554/558/55c`.

### 4.5 Build graph

`android/app/src/main/cpp/CMakeLists.txt` has two switches, both `OFF` today:

- `KORRA_LINK_GUEST=ON` globs `src/recomp/gen/*.c` (gitignored) and
  `src/host_android/*.c|cpp` into `libkorra.so`, with
  `-I ../pcrecomp/runtime/recomp32`. It fails loudly if either tree is missing,
  so a "guest build" that silently contains no guest cannot happen.
- `KORRA_WITH_DXVK=ON` adds a **prepared** DXVK tree (section 5.3) and links
  `dxvk_android vulkan`.

Nothing else changes between the scaffold and the real build. The M3 session's
first commit is therefore small: `src/host_android/`, the two flags, and the
replaced `korra_guest_*` stubs.

### 4.6 Expected failure order (cheapest first)

1. Guest memory window (`g_mem_base`, `VirtualAlloc`) — crashes immediately,
   trivially visible in the log.
2. Lifted C compiles for arm64 (it should: integer-only, already portable) —
   then runs into `pcmpistri` (section 7).
3. Import trampolines: stack imbalance shows up as a crash in an unrelated
   function a few frames later. One shim at a time.
4. COM bridge (D3D9) — the real work; see 5.4.
5. DXVK feature gaps (BC formats, multiview, clip distances) — visible as a
   black frame or a device-creation failure, and logged.

## 5. D3D9 → Vulkan on the two target devices

Targets: **S24 / Exynos 2400 / Xclipse 940** (AMD RDNA3, stock driver only) and
**S25 / Snapdragon 8 Elite / Adreno 830** (stock driver or Turnip). Both expose
Vulkan 1.3 on current firmware; the plan only requires 1.1. Verify on each
device with a probe before trusting either (section 5.6).

### 5.1 What Korra asks of D3D9

From `docs/quirks.md` §3 and the import list: exactly **one** D3D9 entry point
(`Direct3DCreate9`) and four D3DX calls, everything else through Platinum's
internal `Hw` HAL. The device is created with X8R8G8B8, `DISCARD`,
`D3DPRESENTFLAG_LOCKABLE_BACKBUFFER`, `EnableAutoDepthStencil=0`, and
`PresentationInterval` = vsync-on in fullscreen / immediate in windowed. The
renderer also **probes `D3DFMT_NULL` render targets** (`0x759180`). That is a
short, testable D3D9 surface — the risk is concentrated in the bridge, not in
D3D9 coverage.

### 5.2 Which DXVK

Use **DXVK native** (`Joshua-Ashton/dxvk-native`), not the Windows build: it is
the D3D9 implementation built for POSIX hosts, and it exports
`Direct3DCreate9` as a plain C symbol. zlib licence, usable directly; keep the
licence text in `THIRD_PARTY_NOTICES.md` when the tree is actually vendored or
downloaded.

Pin: `native-1.9.2b`, commit `c8dc91fabd00cac11d697ccf07426e798393cd40`. This is
the version the only public Android DXVK-native/D3D9 recomp port is built on,
so a failure has a known-good reference. Do not float this tag.

### 5.3 How it reaches the app

1. **Prepare, don't patch by hand.** A small script produces a build tree from
   a clean clone: string replacements for the POSIX/ARM assumptions (x86
   intrinsics → `__builtin_ctz`/`yield`, `_mm_load_ps` → scalar, `__android_log_print`
   in the logger, conditional feature flags) plus the Android WSI file. A hash
   marker written by the script and checked by CMake keeps a stale tree from
   being built silently. (This is exactly what `prepare.py` does in
   `nfsu2-recompiled`; re-derive the recipe, credit the idea.)
2. **Static link into `libkorra.so`.** One `.so` is simpler than shipping a
   `libdxvk_d3d9.so` and loading it by name, and it keeps `vkGetInstanceProcAddr`
   selection in one process. Build only `src/{util,dxso,spirv,vulkan,dxvk,d3d9}`.
3. **Custom WSI.** DXVK's upstream Linux WSI is Xlib. Android needs
   `VK_KHR_android_surface`:
   - `DxvkPlatformExts::getInstanceExtensions()` → `VK_KHR_surface` +
     `VK_KHR_android_surface`;
   - surface creation via `vkCreateAndroidSurfaceKHR` with the `ANativeWindow`
     from the shell, resolved **through the loader the instance came from**
     (not a second copy of `vkGetInstanceProcAddr`);
   - `ANativeWindow_acquire` on bind, release on surface destroy; recreate the
     surface when the window changes (background/foreground);
   - D3D9's `Reset` maps to `ANativeWindow_setBuffersGeometry` (render scale).
4. **The HWND is a token.** DXVK's platform layer takes an `HWND`; nothing in
   the app has one. Use a monotonic generation counter (so a stale handle is
   detectably stale) and a fixed fake monitor handle. Never a pointer.
5. **Driver selection.** Patch DXVK's loader indirection so the instance's
   `vkGetInstanceProcAddr` can come from either `libvulkan.so` (default) or
   `libadrenotools` when a user-supplied Turnip is selected. This is required
   for the S25 fallback and irrelevant on the S24 (section 5.5).

### 5.4 The bridge the guest cannot skip

DXVK returns C++ objects whose vtables contain 64-bit host pointers. The guest
holds 32-bit pointers in guest memory and calls through them. Both cannot be
true of the same object, so a bridge is mandatory:

- `Direct3DCreate9` returns a **guest token**, not the pointer.
- The host publishes a page of **guest-callable thunks** at a fixed guest VA
  (one per interface method). Each thunk loads the interface kind + method slot
  from its own address, switches to host code, decodes the arguments the guest
  pushed, calls the real DXVK method with the real object, converts the result
  back (HRESULT, out-parameters into guest memory), and returns with the
  correct stack discipline.
- Guest-visible output pointers (textures, vertex buffers, surfaces) are
  tokens; lock/unlock paths copy through the guest window.

There is no way around this and it is where the M3 time will go. Two design
rules from the failure modes in the reference port:
- **Never let a host pointer reach the guest** (the "stale GPU handle" crash
  class in `AGENTS.md` §5 is exactly this).
- Keep the token→object table in one file with logging on insert/remove, so a
  crash on a freed token names the object.

### 5.5 Device-specific decisions

| | S24 (Exynos 2400, Xclipse 940) | S25 (Snapdragon 8 Elite, Adreno 830) |
|---|---|---|
| Driver | Samsung stock Vulkan only — **no Turnip** (Turnip is Adreno-only) | stock Adreno, or Turnip loaded with `libadrenotools` |
| Expected profile | RDNA3: BC formats, multiview, clip distances all present | Adreno: BC support varies by driver; check, and fall back to CPU DXT decode |
| Risk | vendor driver bugs are unfixable from the app; no fallback | driver is replaceable (Turnip), which is the escape hatch |

This is why the build must not be Adreno-shaped: the S24 has no Turnip path at
all, so every optional Vulkan feature has to be queried and gated, never
assumed. Conversely, on the S25 keep Turnip as the fallback when the stock
driver rejects a feature DXVK needs.

**BC (DXT) textures.** `D3DXCreateTextureFromFileInMemoryEx` and the game's
textures mean BC1/BC3 reach DXVK. Query
`VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT` for `BC1_RGBA`/`BC3_RGBA`; if absent,
enable a CPU decode path for the lock/upload cases the game uses (nfsu2 has one
for 2D/cube uploads; re-derive it). The plan says X8R8G8B8 is the dominant
format, so this is a fallback, not the main path.

**`D3DFMT_NULL`.** Korra probes it before trusting first-frame output
(`docs/quirks.md` §3). Under DXVK, log the `CheckDeviceFormat(D3DFMT_NULL)`
result and the render-target creation; a failure here looks like a stale GPU
handle, not like a shader error (same note as the M2 handoff).

**Presentation.** Windowed mode asks for `D3DPRESENT_INTERVAL_IMMEDIATE`
(vsync off), which maps to `VK_PRESENT_MODE_IMMEDIATE_KHR`/`MAILBOX` where
available. Most Android surfaces guarantee only `FIFO`. Request mailbox,
fall back to FIFO, and let the engine's own limiter (`0x754840`, 1/3-ms units)
do the pacing. Force `BackBufferCount >= 2` in the bridge: with 0 the CPU can
run at most one frame ahead, which is the classic "present blocks the game
thread" stall on mobile.

**Pipeline cache.** Set `DXVK_STATE_CACHE_PATH` to
`<files>/cache/dxvk/` and `DXVK_STATE_CACHE=1`. First launch on a phone compiles
hundreds of pipelines; without a persistent cache every launch pays it, and
Adreno/Turnip caches are device- and driver-version-specific, so validate the
cache header (vendor id, device id, pipeline-cache UUID) and discard on mismatch.

### 5.6 Verification order (device)

1. Shell APK installs and logs (already true here).
2. `KORRA_LINK_GUEST=ON` with the guest starting but D3D stubbed: log
   `WinMain`/device-creation entry and stop before `CreateDevice`.
3. DXVK linked, instance + physical device selected, `vkCreateAndroidSurfaceKHR`
   succeeds, swapchain created (log format, present mode, image count).
4. `Direct3DCreate9` + `CreateDevice` through the bridge; log the returned
   HRESULT and the chosen adapter.
5. First frame on device (`ANativeWindow` lock/queue path or swapchain present).
6. Touch HUD, then the movie→menu step (the M2 blocker, section 8).

## 6. Bionic gaps to plan for

From `ngbrecomp`'s `docs/TECHNICAL.md` and confirmed against the NDK headers:

| Gap | Consequence | Plan |
|---|---|---|
| `memfd_create` is API 30 | alias to `syscall(SYS_memfd_create)` on API 29, or use `shm_open` | needed only if a shim wants anonymous files |
| no `pthread_cancel` | cannot kill a guest thread from outside | cooperative stop flag; never cancel |
| `backtrace` is API 33 | crash reports have addresses only | `_Unwind_Backtrace` + `dladdr`, as ngbrecomp does |
| 16 KiB signal stacks (libc default) | fault handler overflow on a guest spill | `sigaltstack` with 256 KiB per guest thread |
| ELF TLS in a shared library | per-access resolver call, ~11 % on a mid phone | `TLS_SLOT_APP` (API 29+, hence minSdk 29) or a plain global |
| 16 KiB page devices | a 4 KiB-aligned `.so` fails to load | already set: `max-page-size=16384`, uncompressed libs |
| `AInputEvent` timing | touch is an event stream, not a poll | sample per frame into `XINPUT_GAMEPAD`, do not replay events |

Also: `libc++_shared.so` is in the APK (73 KB lib + 1.25 MB runtime). For release
builds switch to `-DANDROID_STL=c++_static` once only one `.so` links C++; until
DXVK is in, shared is the smaller risk (one copy instead of one per library).

## 7. `pcmpistri` on arm64 — measured risk, not a guess

### 7.1 What the runtime does

`pcrecomp/runtime/recomp32/recomp_types.h` has two bodies behind one signature:

- `#if defined(_MSC_VER) && defined(_M_IX86)` → `_mm_cmpistri`, `_mm_cmpistrz`,
  `_mm_cmpistrc`, i.e. **the real SSE4.2 instruction**, in a `switch` over the
  16 imm8 values `0x00 0x04 0x08 0x0C 0x0D 0x10 0x14 0x18 0x1C 0x20 0x30 0x38
  0x40 0x44 0x48 0x4C`. (The switch is required: these intrinsics need an
  immediate, so a variable imm8 does not compile.)
- everything else (so: **arm64**) → a portable C body. **Fixed 2026-11-20**
  (`recomp_types.h`, Korra-0005): it now implements all four data formats
  (unsigned/signed bytes/words), all four aggregations, all four polarity modes
  and both index directions, and returns ZF/CF/SF/OF. The old body (unsigned
  bytes, implicit lengths, ZF/CF only) is what the measurements below recorded.

The header's own comment used to say the portable body was "NOT yet verified
against the hardware". That is resolved: `tests/run_pcmpistri_diff.ps1` compares
7,520,000 tuples and all four flags, and passes.

### 7.2 The six sites in LoK.exe

`python tools\scan_pcmpistri.py "…\LoK.exe"` (new, read-only):

| VA | imm8 | format | aggregation | polarity | index | lifted instances |
|---|---|---|---|---|---|---|
| `0x77A34F` | `0x0C` | unsigned **bytes** | equal-ordered | positive | lsb | 12 (`recomp_0041.c`) |
| `0x77A38B` | `0x0C` | unsigned bytes | equal-ordered | positive | lsb | (same group) |
| `0x77C33B` | `0x40` | unsigned bytes | equal-any | positive | **msb** | 1 (`recomp_0042.c:1213`) |
| `0x77CF3D` | `0x0D` | unsigned **words** | equal-ordered | positive | lsb | 15 (`recomp_0042.c`) |
| `0x77CF53` | `0x0D` | unsigned words | equal-ordered | positive | lsb | " |
| `0x77CF88` | `0x0D` | unsigned words | equal-ordered | positive | lsb | " |

28 lifted instances come from 6 static sites because the catalog contains alias
bodies. Two independent confirmations that `0x0D` is **word** data:
- imm8[1:0] = `01` = unsigned words in Intel's control-byte layout;
- the lifted code right after `0x77CF53` is `edx = edx + ecx * 2` — the returned
  element index is scaled by two.

These are the CRT's own string primitives (`strstr`-shaped at `0x77A34F`,
`strrchr`-shaped at `0x77C33B`, wide-string at `0x77CF3D`) and they are on the
boot path: the Windows build needed the lifter patch at all because MSVC's CRT
startup runs them.

### 7.3 Measured divergence

`tests\run_pcmpistri_diff.ps1` builds `tests\pcmpistri_diff.c` twice from the
same source — **Win32** (MSVC + `_M_IX86` → intrinsics/hardware) and **x64**
(guard false → the portable body) — runs both on the same CPU (Intel
i5-13450HX), and diffs 7.52 M tuples: 470,000 input pairs × 16 imm8 values.
Input classes: ASCII with NUL terminators, adversarial no-NUL/repeated,
random bytes with random NULs, and needles copied out of the haystack.

| imm8 | meaning | differing tuples | of 470,000 | Korra uses |
|---|---|---|---|---|
| `0x00` | ubyte equal-any lsb | 0 | — | no |
| `0x04` | ubyte ranges lsb | 0 | — | no |
| `0x08` | ubyte equal-each lsb | 193,125 | 41.1 % | no |
| **`0x0C`** | **ubyte equal-ordered lsb** | **28,480** | **6.06 %** | **yes** |
| **`0x0D`** | **uword equal-ordered lsb** | **469,592** | **99.9 %** | **yes** |
| `0x10` | ubyte equal-any negative | 0 | — | no |
| `0x14` | ubyte ranges negative | 0 | — | no |
| `0x18` | ubyte equal-each negative | 1,150 | 0.24 % | no |
| `0x1C` | ubyte equal-ordered negative | 28,681 | 6.1 % | no |
| `0x20` | ubyte equal-any msb | 0 | — | no |
| `0x30` | ubyte equal-any neg msb | 128,311 | 27.3 % | no |
| `0x38` | ubyte equal-each neg msb | 23,595 | 5.0 % | no |
| **`0x40`** | **ubyte equal-any masked msb** | **0** | **—** | **yes** |
| `0x44` | ubyte ranges masked msb | 0 | — | no |
| `0x48` | ubyte equal-each masked msb | 344,655 | 73.3 % | no |
| `0x4C` | ubyte equal-ordered masked msb | 28,888 | 6.1 % | no |

**Measurement before the fix** (2026-11-20, same day; kept as the evidence that
drove it). After Korra-0005 the same harness reports **0 differences for all 16
imm8 values**. At the time it read:

Verdict: the portable body is **not** equivalent to the instruction. For this
binary:

- **`0x0D` is wrong by construction** — words are read as bytes, so almost
  every result differs (index, ZF and CF).
- **`0x0C` is wrong for one class**: when the needle does not fit the operand
  window, the hardware sets index 0 and CF=1 (verified directly: empty needle
  vs any haystack → index 0, CF=1; needle vs empty haystack → index 0, CF=1),
  while the portable body's `len1 > 0 && i + len1 <= len2` rule reports "no
  match" (index 16, CF=0). The measured rate (28,480/470,000 = 6.06 %) matches
  the probability that the needle's first byte is NUL in that generator
  (1/16 = 6.25 %), and the first differing case is exactly `a[0]=0`.
- **`0x40` agrees with hardware on every tuple** — equal-any over bytes is the
  aggregation the portable body gets right.
- The equal-each family diverges too, but Korra has no equal-each site; treat it
  as a trap for the next binary, not as an M3 blocker.

### 7.4 The second problem: SF and OF (fixed)

**Status: stale as written, then fixed in two steps.** The text below records
what this section originally claimed; the corrected facts follow it.

The original claim was that `lift32.py` lowers `pcmpistri` to `_cf` + a fake
`cmp` that reproduces **ZF and CF only**, with the comment that SF and OF "are
left out only because no generated site branches on them".

That was wrong for this binary, and it was fixed on 2026-11-20 in Korra-0004
(`pcrecomp` `b0b2ce2`): site `0x77A38B` branches `jno`/`jns` on exactly SF/OF,
so the lowering now packs all four into `FK_EFLAGS` through
`recomp_pcmp_eflags`. On x86 the four bits are the CPU's own intrinsics, so
every following branch is exact. The current lifted tree has 28 `pcmpistri`
instances (12 in `recomp_0041.c`, 16 in `recomp_0042.c`) and all 28 use the
packed-EFLAGS form — **"the lifter drops SF/OF" is stale and should not be
re-quoted.**

The second, separate error was in the **portable** body: it modelled
`SF = IntRes2[15]`. Hardware defines SF as **"any null element in `a`"** (and
ZF as "any null element in `b`"); probe evidence: `a="ab"`, `b="xaby"` gives
SF=1 on hardware while `IntRes2[15]` is 0. Korra-0005 fixes that and the
differential harness now compares all four flags, not just ZF/CF.

The original text, for the record:

> At `0x77A38B` the lifted code is `... CMP_NO(_flag_a, _flag_b) ...` and the
> artificial subtraction makes both constant, so the first branch is always
> taken. On hardware, OF = `IntRes2[0]`, which is 1 when the needle matches at
> offset 0. Why the port still works: the always-taken branch lands on the CRT's
> byte-wise path... That explains the Windows build, but it is not a guarantee.

That reasoning is superseded: the `jno` branch no longer sees a constant.

### 7.5 Options for arm64 (ranked)

**A. Fix the portable body in the runtime header (recommended) — IMPLEMENTED 2026-11-20.**
One file (`recomp_types.h`), no relift: rebuild only. Work items:
1. honour imm8[1:0] — unsigned bytes **and unsigned words** (Korra needs words);
2. implement the hardware validity rule for equal-ordered (index 0 / CF=1 when
   the needle cannot fit, including an empty needle);
3. compute SF and OF (and ZF/CF) as the instruction defines them. The lowering
   already carried four bits (Korra-0004); the missing piece was the portable
   body's SF rule (`IntRes2[15]` instead of "any null in `a`");
4. verify with the harness below, and add it to the repo's test commands.

Result: `recomp_types.h` (`dc18f83`) plus the harness extension, 7,520,000
tuples × index/ZF/CF/SF/OF pass; the 21 site-shaped cases pass. SIMDe was read
as a reference but its `cmpistri` is unimplemented upstream, so the body is
derived from the SDM rules and validated against this CPU. QEMU's helper was
read for the validity rules and re-derived, not copied.

**B. Use SIMDe for the whole helper.** `simde_mm_cmpistri/z/c/s` exist (MIT,
header-only) and are the same call shape. Force `SIMDE_NO_NATIVE` on arm64 so
the portable path is selected. Costs a third-party dependency and a notice; the
win is that the aggregation/validity rules are already written and tested by a
large project. Validate with the same harness (build SIMDe with
`SIMDE_NO_NATIVE` x64 vs the intrinsics x86) before adopting.

**C. NEON fast path.** Only after correctness. The portable loop is O(256)
worst case per call and `0x0C`/`0x0D` sit in `strstr`/wide-string primitives; a
`vceqq_u8`-based equal-any and a 16-lane equal-ordered are straightforward, but
measure first on device — the boot path calls these a few hundred times, not
millions.

**Rejected: intercept the guest CRT functions** (map the guest's `strstr`/
`wcsstr` to Bionic's). It would bypass the semantics question for those two
callers, but it silently changes code the lift considers data, breaks the
"lifted C is the program" invariant, and would have to be redone for every new
CRT entry point.

### 7.6 How to accept the fix

```powershell
# 1. site inventory (read-only, committed)
python tools\scan_pcmpistri.py "The Legend of Korra\LoK.exe"

# 2. differential: hardware (Win32) vs portable (x64), 7.52 M tuples
powershell -ExecutionPolicy Bypass -File tests\run_pcmpistri_diff.ps1
# 2026-11-20: PASS for all 16 imm8 values, index and ZF/CF/SF/OF included,
# plus 21 site-shaped cases (0x0C strstr, 0x0D wide string, 0x40 strrchr)
```

The acceptance condition is met for every imm8 value (not only Korra's three),
with all four defined flags compared, and the harness also exercises the six
sites' input shapes (`tests/pcmpistri_sites.c`). What this evidence still does
not cover: it validates the *algorithm* against one Intel CPU (i5-13450HX),
not against arm64 silicon, and no device run exists yet. The remaining risk
after a green harness is a compiler/optimiser difference on arm64 — integer C
only, so this is small, but the first arm64 build should still log the six
sites' results once and compare against the x86 log for the same boot path.

## 8. M3 task list (after the M2 gate)

Ordered, each with a gate. Nothing here touches `work/`, `src/recomp/gen/`, or
`src/host/` owned by the M2 session.

1. **Freeze the M2 tree.** Record the commit; copy the proven host behaviour
   (window creation order, HWND global `0xe4e52c`, `OleLoadPicture` stub, CPK
   root) into `docs/ANDROID_BRINGUP.md` as the Android contract.
2. **`src/host_android/` skeleton** — memory window + POSIX file layer + no-op
   window. Gate: the guest starts, logs its module path, and stops before
   `Direct3DCreate9`.
3. **`pcmpistri`** (section 7) — fix the runtime body in pcrecomp, harness green
   for 0x0C/0x0D/0x40. Gate: the CRT boot path on device logs the same strings
   as the Windows build.
4. **DXVK prepare script + static build** — no guest yet. Gate: a device-side
   probe creates an instance, picks a physical device, creates an
   `VK_KHR_android_surface` and a swapchain, then exits.
5. **D3D9 bridge** — `Direct3DCreate9` + `CreateDevice` + `Reset` + present.
   Gate: engine's device creation succeeds on device and the swapchain presents
   a cleared frame.
6. **First frame** — screenshot or `ScreenRecord` proof.
7. **Touch HUD** — `XINPUT_GAMEPAD` from touches, hide on physical pad connect.
8. **Movies/menu transition** — the same blocker the M2 session is on
   (`KORRA_NO_AUDIO=1` path, menu input). Share findings, do not duplicate.
9. **Pacing + perf** — ADPF target from `DAT_00e4d210`, 60 fps outside menus.
10. **Gate M3**: boots, first area playable, silent, touch works, on both test
    devices (or explicitly on one plus a written reason for the other).

## 9. Not verified / open

- No Android device was attached during this work: nothing above was run on a
  phone, including the APK.
- The scaffold's GameData copy was not exercised against a real device picker;
  the SAF walk is written against `DocumentsContract` but only compiled, not
  run.
- Xclipse 940 / Adreno 830 Vulkan version and optional features are stated from
  the devices' public specs and must be probed on the units.
- DXVK feature policy on those two drivers (BC, multiview, clip distances,
  `D3DFMT_NULL`) is a plan, not a measurement.
- The `0x0D` word path is only proven to be word-based from the instruction and
  the `ecx*2` scaling; which CRT entry point uses it (`wcsstr`-shaped) is an
  inference.
- The SF/OF analysis is static; the claim that the byte-wise fallback makes the
  Windows build correct is an explanation, not a test.
