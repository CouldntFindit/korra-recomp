# Port quirks & hook addresses (Track C — Ghidra triage)

**Source artifact:** `LoK.exe`, SHA-256 `497eedaf07af54241ae2357fb9c57908e5947c2e547de4e1363e116ce9f4f683`
(REA opened a byte-identical copy, `work/LoK.analysis.exe` — same digest).
**Tool:** Ghidra 12.2 headless via REA, default auto-analysis, ephemeral (no saved DB).
**Date:** 2026-11-19.
**Addressing:** PE32, image base `0x400000`, so **`VA = RVA + 0x400000`**. Every address below is a VA.
`FUN_xxxxxxxx` names are Ghidra's, i.e. *unresolved* — they are not symbols, just entry points.

> Verifiability: every claim here is one REA call away —
> `search_strings`, `xrefs`, `procedure_pseudo_code`, `read_function_instructions`.
> Re-open the same exe and re-run; addresses are stable because the binary is frozen.

---

## 0. Headline corrections for the plan

| Plan assumption (§2 / §9 Track C) | What the binary actually does |
|---|---|
| `framerate_d` / `framerate_n` are a data-driven fixed-timestep config and the "first hook target for 60 fps" | **Wrong.** They are **CRI Sofdec MPEG movie-stream header fields** (frame-rate numerator/denominator of a movie file), read next to `mpeg_codec`, `total_frames`, `ixsize`, `alpha_type`, `pre_padding`, `metadata_count`, `color_space`. Nothing to do with the render loop. |
| `ProgressFrameRateType` / `DrawFrameRateType` / `MotionFrameRateType` are global frame-rate config | **Wrong.** They are **per-action animation-field names** in the character action/motion parameter table (siblings: `LaunchFrame`, `AttackTime`, `HoldTime`, `ReleaseTime`, `VibSeq`, `EndCameraInterFrame`, …). They select which *animation* rate class an action uses, not the game's frame rate. |
| (not in plan) | **The game already has a built-in 30/60 fps switch** in its frame limiter. That is the real 60 fps hook. See §4. |

The string anchors from §2 are real and useful (they identify subsystems), but they are **not** the render-rate control.

---

## 1. `framerate_d` / `framerate_n` — movie (Sofdec) stream config

| What | Where |
|---|---|
| `"framerate_d"` | `0x9114c0` (12 bytes) |
| `"framerate_n"` | `0x9114cc` (12 bytes) |

Both strings are used as **key lookups in consecutive slots of the same struct**:

```
FUN_007dd5e0(param_1, param_2)              ; 0x7dd5e0 — xrefs 0x7dd60d / 0x7dd61d
  h = FUN_008d8d0e(*(param_1+0x1124), *EAX, EAX[1])   ; open the movie's config blob
  ctx[10] = FUN_008d235a(h, 0, "mpeg_codec")
  ctx[4]  = FUN_008d235a(h, 0, "framerate_n")
  ctx[5]  = FUN_008d235a(h, 0, "framerate_d")
  ctx[6]  = FUN_008d235a(h, 0, "total_frames")
  ctx[0]  = FUN_008d235a(h, 0, "width")   ctx[1] = "height"   (…ixsize, pre_padding,
            mat_width, mat_height, scrn_width, mpeg_dcprec, metadata_{count,size},
            disp_width, disp_height, color_space, max_picture_size, alpha_type)
```

`FUN_007dfd40` (`0x7dfd40`) is the sibling/variant path (same keys, `E12042000M:Unknown alpha type`
assert on bad `alpha_type`, `color_space != 0 → 1`, `max_picture_size < 1 → 0xffffffff`).

**Struct layout produced (per-movie, `undefined4` array in a Sofdec context object):**

| idx | field | notes |
|---|---|---|
| 0/1 | width / height | decoded frame size |
| 2/3 | disp_width / disp_height | 0 ⇒ fall back to width/height |
| 4/5 | framerate_n / framerate_d | **numerator / denominator**, `n/d` fps |
| 6 | total_frames | |
| 7/8 | mat_width / mat_height | 0 ⇒ fall back |
| 9 | scrn_width | |
| 0xa | mpeg_codec | |
| 0xb | mpeg_dcprec | |
| 0xc/0xd | metadata_count / metadata_size | |
| 0xe | pre_padding | default `0x40` when absent |
| 0xf | color_space-derived flag | `(color_space == 2)` |
| 0x10 | max_picture_size | `-1` when absent |

**Port impact:** none for M2/M5 frame rate. Only matters if/when USM movies are enabled
(M2.12 currently stubs movies out). `0x6c4b2a…` also uses `"HeaderFps"` (`0x96a8ac`, 12 xrefs)
in the same movie layer — the USM/movie data path lives around `0x6c4b00–0x6c7a00`.

---

## 2. `*FrameRateType` — per-action animation rate class

| String | Address |
|---|---|
| `ProgressFrameRateType` | `0x96a604` |
| `DrawFrameRateType` | `0x96a63c` |
| `MotionFrameRateType` | `0x96a664` |
| (`VariableFrameType`, `GraphicMode`, `SettingParamIntArgs`, `EndCameraInterFrame`, `EndCameraResetPattern` are in the same table) | `0x96a6xx` |

All three are referenced from **one function**, `FUN_006b4440` (`0x6b4440`), which is a *field-binder*
driven by a name→offset lookup object:

```
(*(*obj + 0x9c))(&out, param_2, "ProgressFrameRateType");  if (out != -1) (*(*obj + 0xe0))(out, value@+0);
(*(*obj + 0x9c))(&out, param_2, "MotionFrameRateType");    if (out != -1) (*(*obj + 0xe0))(out, value@+1);
(*(*obj + 0x9c))(&out, param_2, "GraphicMode");            if (out != -1) (*(*obj + 0xe0))(out, value@+2);
(*(*obj + 0x9c))(&out, param_2, "DrawFrameRateType");      if (out != -1) (*(*obj + 0xe0))(out, value@+3);
(*(*obj + 0x9c))(&out, param_2, "VariableFrameType");      if (out != -1) (*(*obj + 0xe0))(out, value@+4);
… "EndCameraInterFrame" → setter +0xec (float), "EndCameraResetPattern" → +0xe0,
  "SettingParamIntArgs" → +0x100(handle, ptr, 4)   (int array setter)
```

* vtable `+0x9c` = `findField(name) → handle/-1`, `+0xe0` = `setInt`, `+0xec` = `setFloat`,
  `+0x100` = `setArray`.
* Sole caller `FUN_006b42f0` (`0x6b42f0`) wraps it: get the action param object
  (`FUN_0068e050(param_1, …)`) → bind all fields into a stack object with vtable `PTR_FUN_00942384`
  → return 1. So this is **"apply one action's setting block"**, used while building an action.
* The same reflective binder pattern appears all over the binary (`m_AmbCol`, `m_DirAng`,
  `m_screenResolution`, `m_resolutionMode`, …) and is the engine's serialisation/reflection core.
  **Port note:** these field-name tables are a gift — they name struct fields and carry
  `{name, type(code), offset, elemSize, count, …}` tuples, so a script can dump near-final
  struct layouts. A future track could harvest them into a header set.

**FrameRateType semantics:** an enum choosing *how an action's animation advances*
(progress-driven / draw-driven / motion-driven). It is read per-action from its setting block;
values are per-action data, not a global. **Not a 60 fps hook.**

---

## 3. D3D9 device creation and presentation parameters

### 3.1 Call graph (top → bottom)

| VA | Function | Role |
|---|---|---|
| `0x758d40` | `FUN_00758d40` | Ctor of the D3D-device wrapper (`*obj = &PTR_FUN_0096cb04`), zeros +0x80…+0x94, calls 0x758e40 |
| `0x758e40` | `FUN_00758e40` | Zero-init; **`memset(obj+0x2c, 0, 0x38)` ⇒ D3DPRESENT_PARAMETERS is at obj+0x2c** |
| `0x758d90` | `FUN_00758d90` | Scalar deleting dtor |
| **`0x758f70`** | **`FUN_00758f70`** | **Builds D3DPRESENT_PARAMETERS (see 3.2)** |
| `0x7593c0` | `FUN_007593c0` | Window style + `CreateDevice` (+ HAL→REF fallback) |
| **`0x7599d0`** | **`FUN_007599d0`** | **Device init (vtable slot; no direct callers — called virtually)** |
| `0x7597c0` | `FUN_007597c0` | `EnumAdapterModes` for `D3DFMT_X8R8G8B8` (22) → mode list at obj+0xac, count obj+0xb0 |
| `0x759940` | `FUN_00759940` | Picks the enumerated mode closest to `(w, h, 0x3c)` — **60 Hz** |
| `0x759180` | `FUN_00759180` | Capability probes (see 3.4) |
| `0x759d40` | `FUN_00759d40` | **`IDirect3DDevice9::Reset(obj+0x2c)`** + object re-create (mode change) |

### 3.2 The presentation parameters, decoded

`FUN_00758f70` writes, in order (instruction addresses are exact hook points):

| Instr | Store | D3DPRESENT_PARAMETERS field | Value |
|---|---|---|---|
| `0x75900a` | `[obj+0x2c]` | `BackBufferWidth` | requested width |
| `0x759010` | `[obj+0x30]` | `BackBufferHeight` | requested height |
| `0x759027` | `[obj+0x34]` | `BackBufferFormat` | **`0x16` = D3DFMT_X8R8G8B8** (hard-coded) |
| `0x75902e` | `[obj+0x38]` | `BackBufferCount` | **`0`** (let the runtime choose 1) |
| `0x759005` | `[obj+0x3c]` | `MultiSampleType` | `(DWORD*)0x96cc7c[i]` — table `{0, 2, 4}` (0x96cc7c–0x96cc87) |
| `0x758fcf/0x758ff7` | `[obj+0x40]` | `MultiSampleQuality` | best quality found by the probe loop, else `0` |
| `0x759020` | `[obj+0x44]` | `SwapEffect` | **`1` = D3DSWAPEFFECT_DISCARD** |
| `0x759016` | `[obj+0x48]` | `hDeviceWindow` | `*(obj+0x28)` = the game HWND |
| `0x759045`/`0x75905f` | `[obj+0x4c]` | `Windowed` | **`1` in windowed, `0` in fullscreen** |
| `0x759035` | `[obj+0x50]` | `EnableAutoDepthStencil` | **`0`** (depth is not the D3D auto one — they render to their own depth RTs; see 3.4) |
| `0x759019` | `[obj+0x54]` | `AutoDepthStencilFormat` | **`0x4b` = D3DFMT_D24S8** (set but unused while +0x50 == 0) |
| `0x75903c` | `[obj+0x58]` | `Flags` | **`1` = D3DPRESENTFLAG_LOCKABLE_BACKBUFFER** |
| `0x759066`/`0x75904c` | `[obj+0x5c]` | `FullScreen_RefreshRateInHz` | fullscreen: the picked mode's refresh; windowed: `0` |
| `0x759069` | `[obj+0x60]` | `PresentationInterval` | fullscreen: **`1` = D3DPRESENT_INTERVAL_ONE (VSync ON)** |
| `0x759053` | `[obj+0x60]` | `PresentationInterval` | windowed: **`0x80000000` = D3DPRESENT_INTERVAL_IMMEDIATE (VSync OFF)** |

**Answer to "vsync behaviour":** *fullscreen* = synced (interval one), *windowed* = **vsync off**
(immediate). Both branches are in the same function; the `Windowed` flag decides.

### 3.3 `Direct3DCreate9` and `CreateDevice`

| VA | Instruction | Meaning |
|---|---|---|
| **`0x7599ef`** | `CALL Direct3DCreate9(0x20)` | SDK version **32** (`D3D_SDK_VERSION`); result stored at `obj+0x20` |
| `0x7594a9` | `CALL [EDX+0x40]` | `CreateDevice(0, 1 /*HAL*/, hWnd, 0x44, obj+0x2c, obj+0x24)` |
| `0x7594c5` | `CALL [EAX+0x40]` | retry with `BehaviorFlags = 0x24` |
| `0x7594de` | `CALL [ECX+0x40]` | last resort: `DeviceType = 2 /*REF*/`, flags `0x24` |
| `0x759481` | `CALL [ECX+0x38]` | `IDirect3D9::GetDeviceCaps(0, 1, &caps)` — gate for the flag choice |

`BehaviorFlags` decode: `0x44 = D3DCREATE_HARDWARE_VERTEXPROCESSING(0x40) | D3DCREATE_MULTITHREADED(0x04)`,
`0x24 = D3DCREATE_SOFTWARE_VERTEXPROCESSING(0x20) | D3DCREATE_MULTITHREADED(0x04)`.
The choice is a threshold compare on one dword of the `GetDeviceCaps` result:
`CMP dword [EBP-0x8C], 0xFFFE0300` at `0x759488`, `JC` to `0x7594b0` (software path).
The caps buffer is ~`0xC8` bytes (`D3DCAPS9`) and `[EBP-0x8C]` sits ~`0xC4` into it, which is
where the **pixel-shader version** lives; the threshold `0xFFFE0300` is just under
`D3DPS_VERSION(3,0)` = `0xFFFF0300`, so the practical meaning is **"PS 3.0 capable ⇒ hardware VP
(`0x44`), else software VP (`0x24`) with a REF-device fallback"**. (Field identification is
*derived* from the struct offset, not stated by the code — treat as inferred.)

Display mode selection: `FUN_007597c0` enumerates `X8R8G8B8` modes, `FUN_00759940`
(`0x759940`) picks the entry minimising `|w-want| + |h-want| + |hz-60|` and stores it at
`obj+0xa8` (so the **fullscreen refresh is forced to the nearest 60 Hz mode**).

Reset path (`0x759d40`): `CALL [device+0x40]` = `IDirect3DDevice9::Reset(obj+0x2c)`. It treats
`0x88760868` (`D3DERR_DEVICELOST`) as "retry", counts failures at `obj+0x7c` (gives up after 10).

### 3.4 Capability probes in `0x759180` (matters for DXVK/Vulkan!)

| VA | Call | Detail |
|---|---|---|
| `0x7591xx` | `IDirect3DDevice9::GetDeviceCaps` (`[dev+0x1c]`) | tier at `obj+0xb8` = 1…4 from a caps dword |
| — | `IDirect3D9::CheckDeviceFormat(0,1, X8R8G8B8, usage=2, RType=3, D3DFMT_D24S8=0x4d)` | depth-stencil support ⇒ `obj+0xb4 = 1` |
| — | `IDirect3D9::CheckDeviceFormat(…, 0x16, 1, 1, 0x4c4c554E)` | **`D3DFMT_NULL` ('NULL' fourcc = `0x4c4c554e`) render-target support** |
| — | `IDirect3D9::CheckDeviceMultiSampleType` (`[d3d+0x2c]`) × 6 per level | loop over the 3 table levels `{0,2,4}` × formats `{X8R8G8B8(0x16), A8R8G8B8(0x15), D24S8(0x4b)}` × mask/no-mask; per-level support mask at `obj+0xbc…` |

**Port warning:** the renderer *knows about* `D3DFMT_NULL` render targets, and it deliberately
uses `EnableAutoDepthStencil = 0`, i.e. it manages depth surfaces itself. DXVK-on-Vulkan
emulates `D3DFMT_NULL` by discarding writes — verify that path when M2.6/M2.7 render the first
frame; a NULL-RT misuse shows up exactly like the "huge address / stale GPU handle" crash class.

---

## 4. Frame pacing — `QueryPerformanceCounter` loop (**the 60 fps hook**)

Three functions, all in the same file and easy to patch:

### 4.1 `FUN_007547b0` — `GetTick()`, `0x7547b0`

```
QueryPerformanceCounter(&qpc);
delta = qpc - *(uint64*)0x00db4838          ; epoch, set at startup
return ROUND(delta * *(double*)0x00db4874 * 3.0);
```

* `0x00db4874` = `double` = `1.0 / QPC_frequency` (converted once at init).
* The `* 3.0` means **the returned unit is 1/3 millisecond** (sub-ms tick). `ROUND` ⇒ integer.
* Instruction sites: `0x7547xx` (single `QueryPerformanceCounter` call at the top).

### 4.2 `FUN_00754840` — `WaitForNextFrame()`, `0x754840` (the limiter)

```
if (!obj->shouldRun())                 ; [obj+0x28] vtable +0x18
    return 0;
if ((DAT_00e4d01c & 4) == 0) {         ; 0x00e4d01c bit2 = "disable frame limiter"
    elapsed = GetTick() - obj->lastTick;          ; obj+0x5c
    while (elapsed < obj->interval) {             ; obj+0x58
        Sleep((obj->interval - elapsed) / 3);     ; 0x7548a4 CALL EDI
        elapsed = GetTick() - obj->lastTick;
    }
}
obj->lastTick = GetTick();
return 1;
```

* `obj+0x58` = **target frame interval in 1/3-ms units**: `50` = 60 fps, `100` = 30 fps.
* `obj+0x5c` = previous tick.
* Bit `4` of the global flag dword `0x00e4d01c` disables limiting entirely (dev/benchmark switch).
  Other code tests bits 0 and 1 of the same dword (e.g. `0x540e10`, `0x754867`).

### 4.3 `FUN_0074c580` — per-frame driver, `0x74c580` (**30/60 switch lives here**)

```
if (DAT_00e4d060 == 0) return 0;                       ; app/frame-manager singleton
ok = WaitForNextFrame(&wakeFlag, DAT_00e4d060 + 0xc);
if (wakeFlag) { for each of DAT_00e4d238 subsystem slots (stride 0xc): FUN_0074c690(...) }
FUN_00749b60(...);
if (ok && DAT_00e4d210 != DAT_00e4d214) {              ; requested mode != applied mode
    double step = (DAT_00e4d210 == 1) ? 0.033333335   ; 30 fps
                                       : 0.016666668; ; 60 fps
    *(uint*)(DAT_00e4d060 + 0x58) = ROUND(step * 1000.0 * 3.0);   ; 100 or 50
    DAT_00e4d214 = DAT_00e4d210;
}
return ok;
```

* **`DAT_00e4d210` (`0x00e4d210`) = frame-rate mode: `1` ⇒ 30 fps, anything else ⇒ 60 fps.**
  `DAT_00e4d214` (`0x00e4d214`) = last applied value (a cached mirror, written in ~45 places).
* The engine therefore **already knows how to run at 60 fps** — this is the same code Max Payne 2
  and NFSU2 needed to be *taught*; Korra has it built in. Excellent news for M5.
* Writers of `0x00e4d210`: `0x540e70`, `0x540e82`, `0x540e8c` (all inside `FUN_00540e10`),
  plus resets at `0x74bd36`, `0x74c46f`, `0x74c61e`, `0x74c678`.

### 4.4 `FUN_00540e10` — the main loop, `0x540e10`

```
while (true) {
    focused = ((A & 0x10000000) && (A & 0x40000000));      ; A = *(uint*)0x00ca3ce0
    FUN_0068d640(!focused);                                ; audio/pause on focus change
    if ((*(uint*)0x00ca3d04 & 0x400000) == 0 && !focused)  DAT_00e4d210 = 0;  ; 60 fps
    else if (DAT_00e4d060)                                 DAT_00e4d210 = 1;  ; 30 fps
    … per-frame update (FUN_0074f1a0, FUN_0074c4f0, FUN_0076ab10, …) …
    FUN_0074f630(table, 5 or 2);                           ; subsystem batch
    if (FUN_0074c580() == 0) DAT_00db484c = 1;             ; frame failed ⇒ quit flag
}
```

Call chain to the entry: `FUN_005409a0` (`0x5409a0`) → `FUN_005409c0()` (init) →
**`FUN_00540e10` (main loop)** → `FUN_00541020()` (shutdown).

⚠️ **Unresolved detail:** the two branches above read oddly — *focused* selects **30 fps**,
*unfocused* selects **60 fps**. I could not confirm what `0x00ca3ce0` (`0x10000000`/`0x40000000`)
and `0x00ca3d04` (`0x400000`) actually mean; they are read in ~50 places, several of them in the
very first `.text` pages, so they may not be pure focus flags. **Do not build the 60 fps patch on
this branch** — see §6 recommendations, and confirm the live value of `DAT_00e4d210` on the first
working Windows build (one `printf` at `0x74c5xx` settles it in a minute).

---

## 5. Input polling

### 5.1 XInput — `FUN_007606d0`, `0x7606d0`

* `for (i = 0; i < 4; i++) XInputGetState((&DAT_00e4e564)[i], &state)` — call site `0x7606fe`.
  `DAT_00e4e564` is the **4-entry controller-index table** (so a remap point exists).
* Per-pad state block, stride `0x24`, base `this`:
  `+0x00` connected, `+0x04/+0x08` left stick x/y, `+0x0c/+0x10` right stick x/y (all
  `/32768.0f`, y negated), `+0x14/+0x18` triggers (`/255.0f`), `+0x1c` **remapped button bits**.
* Button remap (`wButtons` → engine bit): `0x0004→0x1`, `0x0008→0x2`, `0x0002→0x4`, `0x0001→0x8`,
  `0x1000→0x10`, `0x2000→0x20`, `0x4000→0x40`, `0x8000→0x80`, `0x0010→0x100`, `0x0020→0x200`,
  `0x0100→0x400`, `0x0040→0x1000`, `0x0200→0x2000`, `0x8000(high byte)→0x8000`.
* Edge detection: `newly = (prev ^ cur) & cur` → `_DAT_00e4e554`; `prev` kept in `DAT_00e4e55c`;
  `DAT_00e4e558` accumulates. **This is the "just pressed" source the UI reads.**
* **Android mapping for M3 touch HUD:** feed exactly this bit layout through the XInput shim;
  no game code changes needed.

### 5.2 DirectInput 8 — `FUN_00760320`, `0x760320`

```
FUN_007602d0();                                   ; teardown previous
DAT_00e4e81c = DAT_00e4e52c;                      ; HWND
DirectInput8Create(GetModuleHandleA(0), 0x800, &IID_IDirectInput8, &DAT_00e4e818, 0);  ; 0x760349
(*dinput)->EnumDevices(0 /*DI8DEVCLASS_ALL*/, FUN_00760270, &DAT_00e4e818, 1 /*ATTACHEDONLY*/);
```

Keyboard + mouse are created from the enumeration callback `FUN_00760270`.
`DAT_00e4e52c` (HWND) is the same global used by D3D (`[obj+0x28]`), i.e. one window handle for
everything — set it once in the host and both subsystems are happy.

---

## 6. Port / hook recommendations

### M2 (bring-up)
1. **No D3DX/D3D9 shims needed for linking** (native32 imports are real) — already known.
2. Set `0x00e4e52c` (HWND) before device init; D3D, DInput and the style code all read it.
3. Expect `CreateDevice` with `BackBufferFormat = X8R8G8B8`, `SwapEffect = DISCARD`,
   `Flags = LOCKABLE_BACKBUFFER`, no auto depth-stencil. DXVK handles all of those; the
   `LOCKABLE_BACKBUFFER` flag costs a readback — measure, and consider dropping it in the
   shim if it is a stutter source on mobile.
4. `OleLoadPicture` (`FUN_0075e530`, `0x75e530`, call at `0x75e61d`): loads a picture through
   `OleInitialize` + `CreateStreamOnHGlobal` + `OleLoadPicture` + `IPicture::get_Handle` +
   `GetObjectA(…, 0x54, …)` — i.e. the **window icon** (`icon.ico` sits next to the exe).
   Stub it: return a 32x32 BGRA bitmap or nothing. Do not let it fall through (it calls
   `OleInitialize`, which needs OLE and will fail the same way on the host).

### M5 (60 fps / frame pacing)
Preferred, in increasing invasiveness:

| # | Hook | Patch | Risk |
|---|---|---|---|
| 1 | `0x74c5xx` (`FUN_0074c580`) | force `DAT_00e4d214` to never equal `DAT_00e4d210` ⇒ the 60 fps branch re-applies `+0x58 = 50` every frame | fights the loop in §4.4 that re-writes `DAT_00e4d210`; needs the real focus semantics first |
| 2 | `0x00e4d210` | write `0` (≠1) after every write, or patch the compare in `FUN_0074c580` | same |
| 3 | `0x75487c` / `0x75492a` | patch the compare of `[ESI+0x58]` so the limiter is skipped | uncapped fps, `Sleep` path never taken; costs power on mobile |
| 4 | `0x754867` | force `DAT_00e4d01c |= 4` (limiter disabled) | cleanest "no limiter" switch; also disables the vsync-ish pacing |
| 5 | present params `0x759053`/`0x759069` | windowed already uses `INTERVAL_IMMEDIATE` | for Android: keep immediate, let the swapchain pace |

**First measurement to take on the working Windows build:** log `DAT_00e4d210`,
`*(uint*)(DAT_00e4d060+0x58)` and the loop's real frame delta. If `0x58` is already `50` at
runtime, the game is running at 60 fps *by design* and M5's frame-rate work is mostly about
*matching logic step to render step*, not unlocking anything.

Also verify: which value of `DAT_00e4d210` the game settles on while the window is focused.
That single number decides whether the story "Korra PC is 30 fps locked" is true or false.
(The 3x-millisecond tick and the explicit 30/60 pair strongly suggest *the engine supports both*.)

---

## 7. Address index (all VAs, image base `0x400000`)

| VA | Name | Kind |
|---|---|---|
| `0x9114c0` | `"framerate_d"` | string |
| `0x9114cc` | `"framerate_n"` | string |
| `0x96a604` | `"ProgressFrameRateType"` | string |
| `0x96a63c` | `"DrawFrameRateType"` | string |
| `0x96a664` | `"MotionFrameRateType"` | string |
| `0x96a8ac` | `"HeaderFps"` | string |
| `0x7dd5e0` | movie config reader (framerate_d/n #1) | func |
| `0x7dfd40` | movie config reader (framerate_d/n #2) | func |
| `0x6b4440` | action-setting field binder (*FrameRateType) | func |
| `0x6b42f0` | action-setting apply wrapper | func |
| `0x758d40` / `0x758e40` / `0x758d90` | D3D wrapper ctor / zero-init / dtor | func |
| **`0x758f70`** | **present-params builder (stores `0x2c…0x60`)** | func |
| **`0x7593c0`** | **window style + CreateDevice (+fallbacks)** | func |
| **`0x7599d0`** | **device init, `Direct3DCreate9` at `0x7599ef`** | func |
| `0x7597c0` / `0x759940` / `0x759180` | enum modes / pick nearest 60 Hz / caps probes | func |
| `0x759d40` | `Reset(presentParams)` path | func |
| `0x96cc7c` | MSAA sample-type table `{0,2,4}` | data |
| `0x8fd254` / `0x8fd250` | `GetWindowLongA` / `GetWindowRect` IAT thunks | iat |
| `0x8fd2d8` / `0x8fd2cc` / `0x8fd2c4` | `Direct3DCreate9` / `XInputGetState` / `timeGetTime` IAT | iat |
| `0x8fd000` / `0x8fd1fc` | `DirectInput8Create` / `OleLoadPicture` IAT | iat |
| **`0x7547b0`** | **`GetTick()` (QPC → 1/3 ms)** | func |
| **`0x754840`** | **`WaitForNextFrame()` (Sleep limiter)** | func |
| **`0x74c580`** | **per-frame driver + 30/60 switch** | func |
| **`0x540e10`** | **main loop** | func |
| `0x5409a0` / `0x5409c0` / `0x541020` | main entry / init / shutdown | func |
| `0xe4d210` | **frame-rate mode (1 = 30 fps)** | data |
| `0xe4d214` | applied-mode cache | data |
| `0xe4d060` | app/frame-manager singleton ptr (+0x58 interval, +0x5c last tick) | data |
| `0xe4d01c` | app flag dword (bit 2 = disable frame limiter) | data |
| `0xdb4838` / `0xdb4874` | QPC epoch / `1.0/freq` | data |
| **`0x7606d0`** | **XInput poll, 4 pads, 0x24 stride, edge detect** | func |
| `0xe4e564` | pad-index table (4 dwords) | data |
| `0xe4e554/558/55c` | pressed / pressed-edge / previous-edge | data |
| **`0x760320`** | **`DirectInput8Create` + EnumDevices** | func |
| `0x760270` | DInput device enumeration callback | func |
| `0xe4e52c` | HWND global (D3D + DInput) | data |
| `0x75e530` | `OleLoadPicture` icon loader | func |
| `0x96cc7c` | MSAA table | data |

---

## 8. Open questions (carry into M2/M5)

1. `DAT_00e4d210` live value while focused — decides the whole 60 fps story (§4.4).
2. What `0x00ca3ce0` bits `0x10000000`/`0x40000000` and `0x00ca3d04` bit `0x400000` mean.
3. Whether the *simulation* step is fixed (tied to `+0x58`) or scaled — i.e. does 60 fps
   double logic rate? Test: log the action/motion update delta at 30 vs 60.
4. `D3DFMT_NULL` render-target usage (`CheckDeviceFormat` probe at `0x7591xx`) — find the
   actual CreateRenderTarget(NULL) sites before M3, DXVK/Vulkan is where that bites.
5. Where the *persistent* display settings (resolution/windowed) are stored — not in the game
   folder (`GameData/` holds only `data000.cpk`, `data001.cpk`, `movie/`, `movie_logo/`,
   `sound/`), so they are registry/`%APPDATA%`-side. Not needed for M2 (defaults are fine).
