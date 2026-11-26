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
`sound\` and `movie\`). The selection must be the **game root** - the folder that
contains both `GameData\` and `LoK.exe` - because the exe's data sections are
part of the image the lift reads. Picking `GameData` itself is rejected by name
before anything is copied.

- Copy target: `getFilesDir()/game/` →
  `/data/user/0/org.korra.recomp/files/game/`. Internal, not external: Android
  16+ denies the app `fopen` on files adb pushed into its own external files
  dir (`EACCES`), so internal storage is the one path that serves both the
  picker and the dev `run-as` loop. No storage permission is needed.
- Validate first: the folder must have a `GameData` directory and `LoK.exe`;
  `GameData` must have `data000.cpk` + `data001.cpk`. The message names what is
  missing, and the picker re-opens on a tap.
- Space: free space is compared against the measured source size (minus what is
  already on disk) plus 128 MB slack. The check runs after the target directory
  exists; `File.getUsableSpace()` on a path that does not exist answers 0, which
  is how the first version could never pass the gate after `pm clear`.
- The copy is Java-side on a worker thread, one
  `ContentResolver.openInputStream` per document (not `DocumentFile` chunk
  APIs), 1 MiB buffers. Progress is on a 400 ms byte clock with a progress bar
  and a rate, because the first two files are 265 MB and 1.6 GB and a file-count
  tick would look frozen. `GameData` and `LoK.exe` are timed separately in the
  log.
- Resume: a file whose target already exists with the source's exact size is
  skipped. `install.complete` is written only after a complete pass, so an
  interrupted copy is offered again next launch; the tree URI is persisted at
  pick time and re-checked against `getPersistedUriPermissions()`, so the same
  interrupted copy resumes without the picker, including after a reboot.
- The native side never sees a `content://` URI. It gets one plain directory
  path, because the file layer it hosts (CRI FS over POSIX `open`) cannot read a
  SAF document.
- The marker + `LoK.exe` + both CPKs are the "installed" test;
  `GameDataInstaller.isInstalled` and the native log line both report it.
  `scripts\android_run.ps1` writes the same marker after its `run-as` copy, so
  the dev tree is never mistaken for a first run. Device measurements of the
  flow are in section 10.8.


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

---

## 10. What the M3 session actually added (2026-11-20)

This section is the implementation map. Everything above it is still the plan;
this is the code that exists, with the device measurements that shaped it.

### 10.1 The device, measured

`tools/android/vk_probe.c`, run by `scripts/android_probe.ps1` (NDK clang,
arm64 API 29, pushed to `/data/local/tmp`, no APK):

- Galaxy S24 `SM-S921B` / Exynos 2400 / Android 17 (SDK 37);
- Vulkan **1.3.304**, driver `Samsung Xclipse 940`, **SPAL 25.3.3** (driverID 21);
- **`textureCompressionBC = 0`**, while BC1/BC2/BC3 are sampled/blit in both
  tiling modes. DXVK's stock `VK_TRUE` for that feature fails `vkCreateDevice`,
  so `tools/dxvk_prepare.py` gates it (and the other optional bits) on
  `supported.*`;
- **`D24S8` has no format features** on this driver; `D32F` is depth-attach but
  not stencil. Depth format selection is the first thing to read in the DXVK log
  if the first frame is black;
- everything else DXVK asks for is present: geometryShader, multiview,
  clip/cull distances, depthClamp, fillModeNonSolid, descriptorIndexing,
  timelineSemaphore, dynamicRendering, synchronization2, transform feedback.

### 10.2 Files

| Path | What it is |
|---|---|
| `tools/android/vk_probe.c` | standalone device probe (features, formats, memory, queues) |
| `scripts/android_probe.ps1` | build one C probe with the NDK, push, run, pull artefacts |
| `src/host_android/host_android.h` | the host contract (memory, shims, files, services) |
| `src/host_android/memory_android.c` | 4 GiB PROT_NONE guest window, guest heap, stacks, logging |
| `src/host_android/spine_android.c` | register file, machine lock, dispatch, shim registry, threads, TLS |
| `src/host_android/pe_android.c` | map the image's data at its VAs, bind the IAT to shim VAs |
| `src/host_android/fs_android.c` | POSIX file layer behind `CreateFileA` (root + guest cwd) |
| `src/host_android/kernel32_android.c` | 121 KERNEL32 imports, exact stdcall purges |
| `src/host_android/user32_android.c` | window tokens, message queue, WndProc dispatch |
| `src/host_android/misc_android.c` | gdi32, shell32, winmm, ole32/oleaut32, steam, XInput, DirectInput, D3DX table |
| `src/host_android/input_android.c` | touch → `XINPUT_GAMEPAD` zones and packet counter |
| `src/host_android/main_android.c` | boot order and the shell's three entry points |
| `src/host_android/d3d9_android.cpp` | guest-COM → DXVK bridge, vtable thunks, lock staging |
| `src/host_android/d3d9_stub.cpp` | the no-DXVK build (rung 2: stops at `Direct3DCreate9`) |
| `dxvk_android/CMakeLists.txt` | DXVK-native build recipe (D3D9-only source set) |
| `dxvk_android/android_wsi.cpp` | `VK_KHR_android_surface` WSI, 1280x720 buffer geometry, token HWND |
| `tools/dxvk_prepare.py` | arm64/Android replacements, shader headers, `vulkan_android.h` |
| `scripts/android_run.ps1` | one-command device loop: install, `run-as` copy of the game tree into internal storage, start, wait, pull `log.txt`/logcat/exit-info |

### 10.3 Build

```powershell
# once per dxvk-native clone
python tools\dxvk_prepare.py ..\reference\dxvk-native

cd android
.\gradlew.bat --no-daemon -PkorraLinkGuest=ON -PkorraWithDxvk=ON `
  -PkorraDxvkSource="..\..\reference\dxvk-native" :app:assembleDebug
```

`KORRA_LINK_GUEST` without `KORRA_WITH_DXVK` is the bring-up rung that must stop
at `Direct3DCreate9` with an explicit log line; it is not a playable build.

### 10.4 Design decisions worth not re-deriving

- **Guest tokens, never pointers.** Every D3D9/steam/DInput object the guest holds
  is a guest VA whose first dword is a guest vtable of shim VAs. `recomp_lookup_import`
  resolves the slot; the thunk reads `this` from ECX.
- **Lock staging.** `LockRect`/`Lock` cannot return DXVK's host pointer in 32 bits,
  so the bridge keeps DXVK's lock open, hands the guest a staging buffer inside
  the guest window, and copies it back on unlock. This is the only way vertex and
  texture upload work on a 64-bit host.
- **Per-slot arity.** Every COM thunk ends with its own `RET(eax, argc)`. A shared
  "return 0" body is what corrupted `g_esp` in the Windows host (AGENTS.md ledger);
  the same rule applies here.
- **Window size.** The WSI sets `ANativeWindow_setBuffersGeometry(1280, 720)` and
  reports 1280x720 to DXVK; the compositor scales. Full-panel swapchains are four
  times the blit cost for no visible gain on this title.
- **Installer.** `LoK.exe` is part of the install (its data sections are the
  image the lift reads), so the picker must be the game root, not `GameData`
  itself. The Java installer now says so and copies both.

### 10.5 The first device boots (2026-11-21)

`LoK.exe` loads, all 191 named imports bind, the CRT initialises, and the engine
reaches its own code. Getting there took four host bugs, each found from a
symbolised tombstone or from the exe itself:

1. **`A32()` read guest addresses as host pointers.** Every shim argument lives
   on the guest stack (a VA), so `*(uint32_t *)addr` faulted at `0xc03fff94`
   (`g_esp=0xC03FFFC0`). It now goes through `MEM32`, with `APTR`/`ASTR`/`AWSTR`
   for host pointers.
2. **The shim registry kept `const char *` to caller stack buffers**, so vtable
   names turned into garbage and an unimplemented import logged as
   `FATAL: unimplemented import  (0xF0000C50)`. `shim_add()` now `strdup`s.
3. **`GetStringTypeW` popped 5 arguments for a 4-argument API**, turning a
   character count into a pointer and faulting in `memset(ptr, 0, 0x807FF3D0*2)`.
   `tools/check_shim_arity.py` now compares every shared symbol against
   pcrecomp's `win32hle`.
4. **The CRT reported R6009 and exited 255.** Not an environment problem: the
   `_setenvp` path queries `WideCharToMultiByte` with an explicit count of 1 for
   the empty (double-NUL) environment block, and our shim stopped at the NUL and
   answered 0. See `docs/FINDINGS.md` for the `_NMSG_WRITE` table walk and the
   four functions involved.

Install detection moved to **internal** storage: Android 17 refuses to let the
app `fopen` files that adb pushed into its own external files dir (`EACCES`), so
the dev install is `/data/user/0/org.korra.recomp/files/game`, copied there with
`run-as`. `scripts/android_run.ps1` does install, `run-as` copy, start, pull and
exit-info in one command. The SAF picker is unchanged as the user path.

### 10.6 Still open

- No frame yet. The next boot should reach D3D9 device creation; the log lines to
  read are `Direct3DCreate9`, DXVK's adapter/feature log, and the chosen depth
  format (`D24S8` has no features on this driver, so a `D32F` fallback is
  expected).
- `GameData` (2.7 GB) still has to be in the app's internal tree before asset
  loading can proceed; `scripts/android_run.ps1` will copy it on first run.
- `D3DXGetShaderConstantTable` and the cube-texture path are stubs that return
  `D3DERR_INVALIDCALL`; `D3DXCreateTextureFromFileInMemoryEx` implements the DDS
  top level only.
- Audio is the documented CRI silent fallback on Android (the XAudio2 2.7 COM
  object is M4).
- Present-mode order (IMMEDIATE -> MAILBOX -> FIFO) is DXVK's choice; the probe
  of what the surface actually grants is logged at swapchain creation.

### 10.7 Reading a device death: sampler, crash buffer, route replay

A native death leaves three artifacts and the old recipe only pulled one of
them. Use all three or the report will name the wrong layer:

1. **The 1 Hz sampler.** `adb shell setprop debug.korra.memtrace 1` before the
   run. `src/host_android/memtrace_android.c` writes one line a second:
   `[mem] t=.. frames=.. presents=.. rss=..kB hwm=..kB heap=.. alloc=.. free=..
   stack=hi/end sfree=.. thr=live/used peak=.. flow=.. inst=.. step=.. mode=..`
   The `thr` and `stack` columns are what turned "1.2 GB RSS" into a thread cap;
   the scene words are the same four `src/host/scene_probe.c` prints on Windows,
   so a device run and an oracle run diff line for line.
2. **The crash buffer.** `adb logcat -b crash -d > logs\crash.txt` on every run.
   The guest `[crash]` handler deliberately covers only SIGSEGV/SIGBUS/SIGILL/
   SIGFPE: a SIGABRT is a host abort, so without the crash buffer the death has
   no frames at all. `scripts/android_run.ps1` now pulls it and stops waiting the
   moment `pidof` goes empty (the r11 death lost its abort message because the
   pull happened five minutes after the crash).
3. **The route, with screenshots that are actually fresh.** `-Route newgame`
   replays the save session's touch timeline. Check `route_*.png` against
   `logs\devsave3_*.png`: a stale screenshot looks like a working route. Two
   traps cost a run each: `input motionevent DOWN/MOVE/UP` as three separate adb
   invocations does not hold a pointer (use one `input swipe`), and
   `Adb-Soft shell screencap -p ...` binds `-p` to the PowerShell function, not
   to adb.
4. **The frame probe.** `adb shell setprop debug.korra.perf 1` makes
   `src/host_android/frame_probe_android.c` write one line a second from the
   Present path: `dt` (present-to-present), time inside `Present`, time in the
   engine's 1 ms `WaitForSingleObject` polls, time blocked re-acquiring the
   machine lock, guest critical sections, the residual, bridged calls/frame,
   the limiter's `[frameMgr+0x58]`/`+0x5c`, `DAT_00e4d210` and process CPU.
   `debug.korra.perf.frames 1` adds slow-frame lines (`debug.korra.perf.slow`,
   default 50 ms). It writes fd 2 **and** its own `files/perf.txt` and logcat
   tag `KorraPerf`: one run lost every line from the shell's log pump at
   `t=266 s` while the game kept rendering, so pull `perf.txt` too and never
   read a silent log as a dead process (`pidof`, then the screenshots).
   The frame cap is `debug.korra.fps` (default 30, `0` = the engine's own
   30/60 mode); `[fps] limiter interval 50 -> 100 (cap 30 fps)` in the log is
   the proof it was applied.
5. **The wait-object selftest.** `adb shell setprop debug.korra.waittest 1`
   makes `src/host_android/wait_selftest.c` run its object scenarios on the
   host's main pthread *before* the guest thread starts. It prints one
   `[wait-selftest] <scenario> PASS|FAIL <detail>` line per case and a final
   `[wait-selftest] N/M PASS`; the scenarios cover auto-reset and manual-reset
   events, semaphore counts and `ReleaseSemaphore` bounds, timeouts and
   `WaitForMultipleObjects`. It drives the same state machine as the shims
   (`src/host_android/wait_objects.c`) with a plain `nanosleep`, never
   `guest_sleep_ms`, because these are host pthreads and the guest sleep
   unwinds the machine lock. The prop must be cleared before a performance run.

RSS is not one number. `dumpsys meminfo`'s TOTAL RSS of 1.19 GB was 1.03 GB
**Graphics** (dmabuf), 15 MB native heap and ~110 MB of anonymous pages; the
in-app VmRSS never exceeded 122 MB. Read the category rows before calling a
death memory pressure.

### 10.8 The first-run SAF install, measured (2026-11-22)

Rung (d) of the M3 gate. Two fresh installs (`adb shell pm clear` before each)
from a game tree the user keeps in shared storage; the folder was selected
through the system picker, not by the dev scripts.

**What the shell did before this session, measured.** With no `files/game`, the
activity drew a static prompt and waited for a tap; the picker never opened by
itself. The space gate could not pass after `pm clear` either: it read
`File.getUsableSpace()` on `files/game` before that directory existed, and the
device answered **0 bytes** ("Not enough free space: 2.60 GB needed, 0 MB
available"). Validation also ran after the copy, so a `GameData`-only pick
copied 2.7 GB and then threw.

**The flow now** (Java: `MainActivity`, `GameDataInstaller`; native hand-off
unchanged):

- launch: installed = `install.complete` + `LoK.exe` + both CPKs. Missing → the
  picker opens 400 ms after `onCreate`. A partial tree with a live persisted
  grant resumes without the picker.
- validate before copying (name what is missing, nothing copied): game root
  needs a `GameData` directory and `LoK.exe`; `GameData` needs `data000.cpk` and
  `data001.cpk`. Source size is measured in the same pass.
- copy: 1 MiB buffered `openInputStream` per document, skip when the target's
  size equals the source's, byte-clock progress (400 ms) + bar + rate, and
  `GameData` / `LoK.exe` timed separately.
- grant: `takePersistableUriPermission` at pick time, URI in SharedPreferences,
  re-checked on every launch.

**Numbers.**

| step | run 1 | run 2 |
|---|---|---|
| picker opened by itself | 400 ms after onCreate | 400 ms after onCreate |
| validation | 820 files, 2,844,528,439 B | same |
| `GameData` copy | 15,609 ms, 182 MB/s | 19,473 ms, 146 MB/s |
| `LoK.exe` | 20 ms | 27 ms |
| copy result | 820 files, 0 EACCES | 820 files, 0 EACCES |
| ready → `guest: entry 0x0077C3B7` | 24 ms | 26 ms |

The marker after a clean install is
`files=820 skipped=0 bytes=2844528439 total=2844528439 seconds=15`.

**A wrong pick is rejected before the copy (measured).** Run 2's first selection
had no `GameData`: validation failed in 34 ms with "That folder has no GameData
folder. Pick the folder that contains GameData and LoK.exe." and the next pick
installed. Zero bytes were copied.

**Resume is real, not claimed.** Deleting the marker and two files, then
relaunching, logged "install: resuming an interrupted copy from
content://…", copied 2 files, skipped 818, finished in 523 ms
(`skipped=818` in the marker) - no picker. Repeated after `adb reboot`: the
persisted grant survived, 1 file + `LoK.exe` copied in 3,298 ms,
`skipped=819`, then the title screen. This is the scoped-storage re-check after
reboot, measured.

**Second launch.** `install: probed …/LoK.exe - ready (internal)`, `game data
present at …/files/game`, no picker, title screen at ~50 s.

**Save path in the SAF-installed tree.** The OCR-driven New Game route (Story →
New Game → Extreme → intro cutscene) reached the tutorial HUD at 260 s;
`CreateDirectoryA <root>/Korra/`, `CreateFileA(GENERIC_WRITE, CREATE_ALWAYS)`
`<root>/Korra/GameData.dat`, `WriteFile 7856`; on disk 7,856 B. The next boot
has zero `[msgbox]` and zero `[Hw::rofs::fst]` lines.

**Dev path.** `scripts\android_run.ps1` writes the same `install.complete`
marker after its `run-as` copy and keys its "already installed" test on it, so a
dev tree is not covered by the picker. `install_android.c`'s native probe is
unchanged.

**Caveat on the picker interaction.** The taps that walked the system picker in
these runs were not produced by a script (logcat shows no `input` command from
this session at those moments; the device also has Microsoft Phone Link running,
which can inject input). The app's own log records the picker open, the rejected
selection, the validated selection, the copy and the boot in order, so the
install path is proven end to end; a fully scripted picker walk is not.

**Also measured (opportunistic).** Continue loads the checkpoint straight into
gameplay ("FIGHT!") and rewrites `Korra/GameData.dat`. Backgrounding the game
during gameplay kills the process: `window: destroyed` -> `Failed to recreate
swap chain` (`88760870`) -> SIGSEGV at guest `0x7524C0`, exit
`reason=2 (SIGNALED) status=11` - a swapchain-lifecycle death, not an LMK kill.
The in-game pause menu is unreachable until that is fixed.



### 10.9 The save files, and the FindFirstFile wildcard contract (2026-11-25)

**The device (original exe, not the analysis copy).** `0x76CB50` builds the save
path: `SHGetSpecialFolderPathA(NULL, buf, CSIDL_PERSONAL=5, FALSE)` then
`"%s/%s/"` with `"Korra"`, and appends `SystemData.dat` / `GameData.dat` /
`SlotData_%d.dat` according to `[0xE4F474]` (-2 / -1 / slot).
`[0xE4F46C]` is the operation: 1 = slot scan (`0x76CCD0`), 2 = load
(`0x76CEA0`), 3 = write (`0x76D0E0`), 4 = erase (`0x76D2F0`). The writer's
state 0 checks the directory with `"%s*.*"` (`0x76CC40` -> `_findfirst` ->
`FindFirstFileExA`), makes the directory only if that check fails, then
`CreateFileA(GENERIC_WRITE, CREATE_ALWAYS)` + `WriteFile`.

**What ships.** `GameData.dat` (7,856 B) and `SystemData.dat` (16 B) - the
owner's original save directory holds exactly those two and nothing else.
`SlotData_%d.dat` is reachable only through op 1 and the erase only through
op 4, and every write to `[0xE4F46C]` in the image is 0, 2 or 3, so neither is
ever requested by this build. There is no manual Save/Load/Delete UI:
`UIPauseMenu`'s methods call nothing in the save device, the only UI caller of
`req_save` (`0x632F20`) is `UIOptionMenu.run` state 7 (`0x5D837B`, reason 5 =
"settings also"), and the only related strings are `Save Data` (0x96479C) and
the saving indicator (`UISaveIcon` / `UIConfirmSaveIconDialog`).

**The wildcard contract the shim has to keep.** The save check uses `*.*`, and
Windows answers it with every name - dotless ones included. `wild_match` in
`src/host_android/kernel32_android.c` must therefore implement, for one path
component: `*` any run (several in one mask), `?` one character, comparison
case-insensitive, `NAME.*` also matching `NAME`, and a bare `NAME.` matching
only extensionless names. `FindFirstFile` must return a handle plus the first
`WIN32_FIND_DATAA`, `INVALID_HANDLE_VALUE` + `ERROR_FILE_NOT_FOUND` (2) when
nothing matches, `ERROR_PATH_NOT_FOUND` (3) when the directory is missing, and
`FindNextFileA` must end with `ERROR_NO_MORE_FILES` (18) - the engine's
`Hw::rofs::fst` mount dies if the enumeration is one entry long. A matcher that
answers `*.*` with nothing makes the save device believe its own directory is
empty: it re-creates it and rewrites `GameData.dat` on every save request, and
the save/slot list shows nothing. Measured before/after and the full mask table
are in `docs/FINDINGS.md` (2026-11-25).

**Device evidence.** With `GameData.dat` present the boot/save probe prints
`FindFirstFileExA(<root>/Korra/*.*) -> <handle> last=0 first="GameData.dat"`,
no `CreateDirectoryA` follows, and a settings change in Options writes
`GameData.dat` 7,856 B and `SystemData.dat` 16 B; the next boot opens
`SystemData.dat` with `last=0`. The checkpoint save is
`hap::HapSaveActionArea` (`0x64EA50`, write at `0x64EB9C`): the level dispatcher
(`0x641150`) delivers event `0x45ce422b`, the area snapshots the player at
`0x632A20` and the writer produces `GameData.dat` 7,856 B with no
`CreateDirectoryA`; the load is reached at the tutorial's level start
(`docs/FINDINGS.md`, 2026-11-26) and Continue resumes it.

**Windows now walks the same menus.** `src/host/xinput_stub.c` gained
`KORRA_PAD_STICK=lx,ly` and optional left-stick fields on the `KORRA_PAD_CMD`
timeline; a 150 ms pulse moves one menu item (an 1,800 ms hold auto-repeats).
The Options -> Controller -> Vibration `ON -> OFF`, then B, B, wrote
`GameData.dat` 7,856 B and `SystemData.dat` 16 B in `<Documents>/Korra`, the
next boot opened `SystemData.dat` with `last=0`, and the probe's `first=` field
printed the real Windows dot entry (`first="."`; the device shim filters `.`/`..`,
which is why the device shows `GameData.dat`). That exercises `71e09ce` on
Windows. The save is only requested on a **net** settings difference
(`0x5D96C0`): toggling ON -> OFF -> ON writes nothing.
