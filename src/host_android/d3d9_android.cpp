/* d3d9_android.cpp - the D3D9 -> DXVK bridge for a 32-bit guest on arm64.
 *
 * Compiled only when KORRA_WITH_DXVK is on; d3d9_stub.cpp answers the same
 * symbols in the no-DXVK bring-up build.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef KORRA_WITH_DXVK
#define KORRA_WITH_DXVK 0
#endif

#if KORRA_WITH_DXVK

#include <d3d9.h>

/* DXVK's minimal d3d9.h omits this fourcc; the engine probes it (quirks.md 3). */
#ifndef D3DFMT_NULL
#define D3DFMT_NULL ((D3DFORMAT)0x4C4C554E)
#endif

#include "host_android.h"

/* ---------------------------------------------------------- `this` on x86
 * The lifted code is 32-bit x86, where a COM call is just a call: the object
 * pointer travels either in ECX (thiscall, what a C++ compiler emits for
 * IDirect3D9) or as the first stack argument (what a hand-written __stdcall
 * vtable call emits). This engine is MSVC-built but its D3D9 calls are not
 * uniform - sub_007597C0 pushes the object and leaves ECX holding a local
 * (docs/FINDINGS.md). Decide once per call, at dispatch, and shift A32/RET by
 * the same amount, so no thunk has to care.
 *
 * g_d9_stack_this is 0 for the ECX convention and 1 when the object was the
 * first pushed argument. ka_vtable_note_import() sets it from the shim name;
 * see spine_android.c's recomp_lookup_import. */
static thread_local int g_d9_stack_this;
static inline uint32_t d9_a32(int i) {
    return MEM32(g_esp + 4u + 4u * (uint32_t)(i + g_d9_stack_this));
}
#undef A32
#define A32(i) d9_a32(i)
#undef RET
#define RET(eax, argc) do { \
        g_eax = (uint32_t)(eax); \
        g_esp += 4u + 4u * (uint32_t)((argc) + g_d9_stack_this); \
        return; } while (0)

extern "C" {
struct ANativeWindow;
/* dxvk-native exports the D3D9 entry point as a plain C symbol. */
IDirect3D9 *__stdcall Direct3DCreate9(UINT SDKVersion);
void dxvkAndroidSetWindow(ANativeWindow *window);
HWND dxvkAndroidGetWindowHandle(void);
}

/* Registered as a single named shim (ka_shim_register only takes arrays). */
extern "C" uint32_t shim_add_named(const char *name, recomp_func_t fn);

namespace {

enum Kind {
    K_NONE = 0,
    K_D3D9,
    K_DEVICE,
    K_SWAPCHAIN,
    K_TEXTURE,
    K_SURFACE,
    K_VBUFFER,
    K_IBUFFER,
    K_CUBETEX,
    K_VOLTEX,
    K_VS,
    K_PS,
    K_VDECL,
    K_STATEBLOCK,
    K_QUERY,
    K_D3DXCT,
};

struct Obj {
    int kind = K_NONE;
    void *p = nullptr;
    uint32_t owner = 0;          /* device token, for GetDevice */
    uint32_t token = 0;          /* this object's guest token */
    uint32_t lock_va = 0;        /* guest staging buffer while locked */
    void *lock_host = nullptr;   /* DXVK's mapped pointer while locked */
    uint32_t lock_size = 0;
    uint32_t lock_pitch = 0;
};

Obj *g_objs;
uint32_t g_obj_count;
uint32_t g_obj_cap;
uint32_t g_vt[32][192];          /* per kind: guest vtable VAs */
int g_vt_built[32];
uint32_t g_d3d_token;
uint32_t g_device_token;

/* ---------------------------------------------------------------- helpers */
void *P(uint32_t va) { return va ? ka_host(va) : nullptr; }

Obj *obj_of(uint32_t token) {
    if (!token) return nullptr;
    /* Tokens are guest VAs from the guest heap, not a dense index: search the
     * table. A few hundred live objects, a handful of lookups per draw call. */
    for (uint32_t i = 0; i < g_obj_count; i++)
        if (g_objs[i].token == token) return &g_objs[i];
    return nullptr;
}

/* Called by recomp_lookup_import before every shim runs. For a synthetic COM
 * slot ("kind:index") decide the convention; for anything else clear it, so a
 * stale stack-this cannot leak into Direct3DCreate9 or a kernel32 call. At
 * dispatch the guest's args are already pushed and the dummy return address is
 * not, so MEM32(g_esp) is what A32(0) will be. */
extern "C" void ka_vtable_note_import(const char *name) {
    if (name && strchr(name, ':'))
        g_d9_stack_this = obj_of(MEM32(g_esp)) ? 1 : 0;
    else
        g_d9_stack_this = 0;
}

/* Grow the table instead of walking off its end. Nothing ever frees a slot
 * (Release is a no-op), so without this the 4097th token wrote straight
 * through the neighbouring BSS: kind/manual/signaled of the kernel object
 * table came back as 0xB400006E... host pointers and a pool worker's
 * blocking wait turned into a 700k/s spin (docs/FINDINGS.md 2026-11-21). */
static void objs_ensure(uint32_t need) {
    uint32_t cap;
    Obj *grown;
    if (need <= g_obj_cap) return;
    cap = g_obj_cap ? g_obj_cap : 4096;
    while (cap < need) cap *= 2;
    grown = (Obj *)calloc(cap, sizeof(Obj));
    if (!grown) {
        ka_log("d3d9: object table grow to %u failed", cap);
        return;
    }
    if (g_objs) memcpy(grown, g_objs, (size_t)g_obj_count * sizeof(Obj));
    free(g_objs);
    g_objs = grown;
    g_obj_cap = cap;
    ka_log("d3d9: object table grew to %u entries", cap);
}

uint32_t new_token(Kind kind, void *p, uint32_t owner) {
    uint32_t i, idx, obj;
    /* The same COM object is the same token. GetRenderTarget and
     * GetDepthStencilSurface were minting a fresh one on every call, and the
     * engine calls both every frame: 1-2 table slots and 12 bytes of guest
     * heap per frame leaked, and the table filled in ~40 s. */
    if (!p) return 0;
    for (i = 0; i < g_obj_count; i++)
        if (g_objs[i].kind == kind && g_objs[i].p == p) return g_objs[i].token;
    objs_ensure(g_obj_count + 1);
    idx = g_obj_count++;
    obj = ka_guest_alloc(12, 4);
    g_objs[idx].kind = kind;
    g_objs[idx].p = p;
    g_objs[idx].owner = owner;
    g_objs[idx].token = obj;
    MEM32(obj) = 0;
    MEM32(obj + 4) = (uint32_t)kind;
    MEM32(obj + 8) = idx;
    /* vtable VA is per kind, independent of the instance */
    MEM32(obj) = g_vt[kind][0];
    return obj;
}

/* Stack convention: the retaddr is already pushed, so A32(0) is at g_esp+4. */
Obj *self_obj() {
    return obj_of(g_d9_stack_this ? MEM32(g_esp + 4u) : g_ecx);
}
uint32_t self_kind() { Obj *o = self_obj(); return o ? (uint32_t)o->kind : 0; }

#define SELF_AS(T, K) \
    (reinterpret_cast<T *>(self_obj() && self_obj()->kind == (K) ? self_obj()->p : nullptr))

/* The guest is 32-bit: its D3DPRESENT_PARAMETERS is the SDK's 56 bytes with a
 * 4-byte HWND. dxvk-native's HWND is a host pointer, so its struct is 64 bytes
 * on arm64 and MUST NOT be overlaid on guest memory - measured on device, the
 * 8-byte HWND shifted every field after SwapEffect, so `Windowed` read 75
 * (D3DFMT_D24S8) and the depth format read 0, and DXVK refused the device with
 * D3DERR_NOTAVAILABLE. Convert at every boundary instead. */
struct KaPP {
    uint32_t width, height, format, count;
    uint32_t msaa, quality;
    uint32_t swap_effect, hwnd;
    uint32_t windowed, auto_depth, depth_format, flags;
    uint32_t refresh, interval;
};
static_assert(sizeof(KaPP) == 56, "guest D3DPRESENT_PARAMETERS must be 56 bytes");

void pp_from_guest(const KaPP *g, D3DPRESENT_PARAMETERS *h) {
    memset(h, 0, sizeof *h);
    h->BackBufferWidth = g->width;
    h->BackBufferHeight = g->height;
    h->BackBufferFormat = (D3DFORMAT)g->format;
    h->BackBufferCount = g->count;
    h->MultiSampleType = (D3DMULTISAMPLE_TYPE)g->msaa;
    h->MultiSampleQuality = g->quality;
    h->SwapEffect = (D3DSWAPEFFECT)g->swap_effect;
    h->Windowed = (WINBOOL)g->windowed;
    h->EnableAutoDepthStencil = (WINBOOL)g->auto_depth;
    h->AutoDepthStencilFormat = (D3DFORMAT)g->depth_format;
    h->Flags = g->flags;
    h->FullScreen_RefreshRateInHz = g->refresh;
    h->PresentationInterval = g->interval;
    h->hDeviceWindow = dxvkAndroidGetWindowHandle();
    /* The doc's rule: never let BackBufferCount be 0 - with one image the CPU
     * can run at most a frame ahead and Present blocks the game thread. */
    if (h->BackBufferCount < 2) h->BackBufferCount = 2;
}

void pp_to_guest(const D3DPRESENT_PARAMETERS *h, KaPP *g) {
    g->width = h->BackBufferWidth;
    g->height = h->BackBufferHeight;
    g->format = (uint32_t)h->BackBufferFormat;
    g->count = h->BackBufferCount;
    g->msaa = (uint32_t)h->MultiSampleType;
    g->quality = h->MultiSampleQuality;
    g->swap_effect = (uint32_t)h->SwapEffect;
    g->windowed = (uint32_t)h->Windowed;
    g->auto_depth = (uint32_t)h->EnableAutoDepthStencil;
    g->depth_format = (uint32_t)h->AutoDepthStencilFormat;
    g->flags = h->Flags;
    g->refresh = h->FullScreen_RefreshRateInHz;
    g->interval = h->PresentationInterval;
}

/* ------------------------------------------------------- IUnknown / shared */
void u_QueryInterface() { RET(0x80004002u, 2); }   /* E_NOINTERFACE */
void u_AddRef() { RET(1, 0); }
void u_Release() { RET(0, 0); }                    /* objects live for the process */

void r_GetDevice() {
    Obj *o = self_obj();
    RET(o ? o->owner : 0, 0);
}
void r_SetPrivateData() { RET(0, 4); }
void r_GetPrivateData() { RET(0x80004005u, 3); }
void r_FreePrivateData() { RET(0, 1); }
void r_SetPriority() { RET(0, 1); }
void r_GetPriority() { RET(0, 0); }
void r_PreLoad() { RET(0, 0); }
void r_GetType() {    switch (self_kind()) {
    case K_TEXTURE:   RET(3, 0);   /* D3DRTYPE_TEXTURE */
    case K_CUBETEX:   RET(5, 0);
    case K_VOLTEX:    RET(4, 0);
    case K_SURFACE:   RET(1, 0);
    case K_VBUFFER:   RET(6, 0);
    case K_IBUFFER:   RET(7, 0);
    case K_VS:        RET(8, 0);
    case K_PS:        RET(9, 0);
    case K_SWAPCHAIN: RET(15, 0);
    default:          RET(0, 0);
    }
}

/* IDirect3DBaseTexture9 */
/* SetLOD/GetLOD are slots 11 and 12 - the two the table was missing, which
 * shifted every derived texture method by two: the engine's LockRect (slot 19)
 * landed on AddDirtyRect, and UnlockRect (slot 20) was past the end of the
 * table, so the guest called a NULL function pointer (ICALL unresolved VA 0
 * from 0x756840, signal 11, 13 ms after the first Present). */
void bt_SetLOD() {
    Obj *o = self_obj();
    if (o && o->kind == K_TEXTURE)
        reinterpret_cast<IDirect3DTexture9 *>(o->p)->SetLOD(A32(0));
    RET(0, 1);
}
void bt_GetLOD() {
    Obj *o = self_obj();
    if (!o) RET(0, 0);
    if (o->kind == K_TEXTURE)
        RET(reinterpret_cast<IDirect3DTexture9 *>(o->p)->GetLOD(), 0);
    if (o->kind == K_CUBETEX)
        RET(reinterpret_cast<IDirect3DCubeTexture9 *>(o->p)->GetLOD(), 0);
    RET(0, 0);
}
void bt_GetLevelCount() {
    Obj *o = self_obj();
    if (!o) RET(0, 0);
    if (o->kind == K_TEXTURE)
        RET(reinterpret_cast<IDirect3DTexture9 *>(o->p)->GetLevelCount(), 0);
    if (o->kind == K_CUBETEX)
        RET(reinterpret_cast<IDirect3DCubeTexture9 *>(o->p)->GetLevelCount(), 0);
    RET(0, 0);
}
void bt_SetAutoGenFilterType() {
    Obj *o = self_obj();
    if (o && o->kind == K_TEXTURE)
        reinterpret_cast<IDirect3DTexture9 *>(o->p)->SetAutoGenFilterType((D3DTEXTUREFILTERTYPE)A32(0));
    RET(0, 1);
}
void bt_GetAutoGenFilterType() { RET(0, 0); }
void bt_GenerateMipSubLevels() { RET(0, 0); }

/* ------------------------------------------------------------ IDirect3D9 */
void i9_GetAdapterCount() {
    RET(SELF_AS(IDirect3D9, K_D3D9)->GetAdapterCount(), 0);
}
void i9_GetAdapterIdentifier() {
    D3DADAPTER_IDENTIFIER9 *out = (D3DADAPTER_IDENTIFIER9 *)P(A32(2));
    RET(SELF_AS(IDirect3D9, K_D3D9)->GetAdapterIdentifier(A32(0), A32(1), out), 3);
}
void i9_GetAdapterModeCount() {
    RET(SELF_AS(IDirect3D9, K_D3D9)->GetAdapterModeCount(A32(0), (D3DFORMAT)A32(1)), 2);
}
void i9_EnumAdapterModes() {
    RET(SELF_AS(IDirect3D9, K_D3D9)->EnumAdapterModes(A32(0), (D3DFORMAT)A32(1), A32(2),
                                                     (D3DDISPLAYMODE *)P(A32(3))), 4);
}
void i9_GetAdapterDisplayMode() {
    RET(SELF_AS(IDirect3D9, K_D3D9)->GetAdapterDisplayMode(A32(0),
                                                           (D3DDISPLAYMODE *)P(A32(1))), 2);
}
void i9_CheckDeviceType() {
    RET(SELF_AS(IDirect3D9, K_D3D9)->CheckDeviceType(A32(0), (D3DDEVTYPE)A32(1),
                                                     (D3DFORMAT)A32(2), (D3DFORMAT)A32(3),
                                                     A32(4)), 5);
}
void i9_CheckDeviceFormat() {
    HRESULT hr = SELF_AS(IDirect3D9, K_D3D9)->CheckDeviceFormat(
        A32(0), (D3DDEVTYPE)A32(1), (D3DFORMAT)A32(2), A32(3), (D3DRESOURCETYPE)A32(4),
        (D3DFORMAT)A32(5));
    if ((D3DFORMAT)A32(5) == D3DFMT_NULL || (D3DFORMAT)A32(2) == D3DFMT_NULL)
        ka_log("d3d9: CheckDeviceFormat(D3DFMT_NULL) -> 0x%08X", (unsigned)hr);
    RET((uint32_t)hr, 6);
}
void i9_CheckDeviceMultiSampleType() {
    RET(SELF_AS(IDirect3D9, K_D3D9)->CheckDeviceMultiSampleType(
            A32(0), (D3DDEVTYPE)A32(1), (D3DFORMAT)A32(2), A32(3),
            (D3DMULTISAMPLE_TYPE)A32(4), (DWORD *)P(A32(5))), 6);
}
void i9_CheckDepthStencilMatch() {
    RET(SELF_AS(IDirect3D9, K_D3D9)->CheckDepthStencilMatch(
            A32(0), (D3DDEVTYPE)A32(1), (D3DFORMAT)A32(2), (D3DFORMAT)A32(3),
            (D3DFORMAT)A32(4)), 5);
}
void i9_CheckDeviceFormatConversion() {
    RET(SELF_AS(IDirect3D9, K_D3D9)->CheckDeviceFormatConversion(
            A32(0), (D3DDEVTYPE)A32(1), (D3DFORMAT)A32(2), (D3DFORMAT)A32(3)), 4);
}
void i9_GetDeviceCaps() {
    RET(SELF_AS(IDirect3D9, K_D3D9)->GetDeviceCaps(A32(0), (D3DDEVTYPE)A32(1),
                                                   (D3DCAPS9 *)P(A32(2))), 3);
}
void i9_CreateDevice() {
    const KaPP *gpp = (const KaPP *)P(A32(4));
    D3DPRESENT_PARAMETERS pp;
    D3DPRESENT_PARAMETERS *ppp = nullptr;
    IDirect3DDevice9 *dev = nullptr;
    uint32_t out = A32(5);
    HRESULT hr;

    if (gpp) {
        pp_from_guest(gpp, &pp);
        ppp = &pp;
        ka_log("d3d9: CreateDevice adapter=%u type=%u hwnd=%08X flags=0x%X "
               "pp=%ux%u fmt=%u windowed=%u autodepth=%u depthfmt=%u "
               "count=%u swap=%u interval=0x%08X",
               A32(0), A32(1), A32(2), A32(3),
               gpp->width, gpp->height, gpp->format, gpp->windowed,
               gpp->auto_depth, gpp->depth_format, gpp->count,
               gpp->swap_effect, gpp->interval);
    }
    hr = SELF_AS(IDirect3D9, K_D3D9)->CreateDevice(
        A32(0), (D3DDEVTYPE)A32(1), dxvkAndroidGetWindowHandle(), A32(3), ppp, &dev);
    ka_log("d3d9: CreateDevice -> 0x%08X device=%p", (unsigned)hr, (void *)dev);
    if (out) MEM32(out) = (hr == D3D_OK && dev) ? new_token(K_DEVICE, dev, 0) : 0;
    RET((uint32_t)hr, 6);
}

/* -------------------------------------------------------- device helpers */
#define DEV (reinterpret_cast<IDirect3DDevice9 *>(self_obj()->p))
#define RET_HR(call, argc) do { HRESULT _hr = (call); RET((uint32_t)_hr, argc); } while (0)

void d_TestCooperativeLevel() { RET(0, 0); }
void d_GetAvailableTextureMem() { RET(512u << 20, 0); }
void d_EvictManagedResources() { RET(0, 0); }
void d_GetDirect3D() {
    if (A32(0)) MEM32(A32(0)) = g_d3d_token;
    RET(0, 1);
}
void d_GetDeviceCaps() {
    RET_HR(DEV->GetDeviceCaps((D3DCAPS9 *)P(A32(0))), 1);
}
void d_GetDisplayMode() {
    RET_HR(DEV->GetDisplayMode(A32(0), (D3DDISPLAYMODE *)P(A32(1))), 2);
}
void d_GetCreationParameters() {
    RET_HR(DEV->GetCreationParameters((D3DDEVICE_CREATION_PARAMETERS *)P(A32(0))), 1);
}
void d_SetCursorProperties() { RET(0, 3); }
void d_SetCursorPosition() { RET(0, 3); }
void d_ShowCursor() { RET(0, 1); }
void d_CreateAdditionalSwapChain() {
    IDirect3DSwapChain9 *sc = nullptr;
    const KaPP *gpp = (const KaPP *)P(A32(0));
    D3DPRESENT_PARAMETERS pp;
    HRESULT hr;
    if (gpp) pp_from_guest(gpp, &pp);
    hr = DEV->CreateAdditionalSwapChain(gpp ? &pp : nullptr, &sc);
    if (A32(1)) MEM32(A32(1)) = SUCCEEDED(hr) ? new_token(K_SWAPCHAIN, sc, g_ecx) : 0;
    RET((uint32_t)hr, 2);
}
void d_GetSwapChain() {
    IDirect3DSwapChain9 *sc = nullptr;
    HRESULT hr = DEV->GetSwapChain(A32(0), &sc);
    if (A32(1)) MEM32(A32(1)) = SUCCEEDED(hr) ? new_token(K_SWAPCHAIN, sc, g_ecx) : 0;
    RET((uint32_t)hr, 2);
}
void d_GetNumberOfSwapChains() { RET(DEV->GetNumberOfSwapChains(), 0); }
void d_Reset() {
    const KaPP *gpp = (const KaPP *)P(A32(0));
    D3DPRESENT_PARAMETERS pp;
    HRESULT hr;
    if (gpp) {
        pp_from_guest(gpp, &pp);
        ka_log("d3d9: Reset %ux%u fmt=%u windowed=%u depthfmt=%u", gpp->width,
               gpp->height, gpp->format, gpp->windowed, gpp->depth_format);
    }
    hr = DEV->Reset(gpp ? &pp : nullptr);
    ka_log("d3d9: Reset -> 0x%08X", (unsigned)hr);
    RET((uint32_t)hr, 1);
}
void d_Present() {
    HRESULT hr = DEV->Present((const RECT *)P(A32(0)), (const RECT *)P(A32(1)),
                              (HWND)(uintptr_t)A32(2), (const RGNDATA *)P(A32(3)));
    RET((uint32_t)hr, 4);
}
void d_GetBackBuffer() {
    IDirect3DSurface9 *s = nullptr;
    HRESULT hr = DEV->GetBackBuffer(A32(0), A32(1), (D3DBACKBUFFER_TYPE)A32(2), &s);
    if (A32(3)) MEM32(A32(3)) = SUCCEEDED(hr) ? new_token(K_SURFACE, s, g_ecx) : 0;
    RET((uint32_t)hr, 4);
}
void d_GetRasterStatus() { RET(DEV->GetRasterStatus(A32(0), (D3DRASTER_STATUS *)P(A32(1))), 2); }
void d_SetDialogBoxMode() { RET(0, 1); }
void d_SetGammaRamp() { DEV->SetGammaRamp(A32(0), A32(1), (const D3DGAMMARAMP *)P(A32(2))); RET(0, 3); }
void d_GetGammaRamp() { DEV->GetGammaRamp(A32(0), (D3DGAMMARAMP *)P(A32(1))); RET(0, 2); }

void d_CreateTexture() {
    IDirect3DTexture9 *t = nullptr;
    HRESULT hr = DEV->CreateTexture(A32(0), A32(1), A32(2), A32(3), (D3DFORMAT)A32(4),
                                    (D3DPOOL)A32(5), &t, (HANDLE *)P(A32(6)));
    if (A32(7)) MEM32(A32(7)) = SUCCEEDED(hr) ? new_token(K_TEXTURE, t, g_ecx) : 0;
    if (FAILED(hr)) ka_log("d3d9: CreateTexture %ux%u fmt=%u -> 0x%08X", A32(0), A32(1), A32(4), (unsigned)hr);
    RET((uint32_t)hr, 8);
}
void d_CreateVolumeTexture() {
    IDirect3DVolumeTexture9 *t = nullptr;
    HRESULT hr = DEV->CreateVolumeTexture(A32(0), A32(1), A32(2), A32(3), A32(4),
                                          (D3DFORMAT)A32(5), (D3DPOOL)A32(6), &t,
                                          (HANDLE *)P(A32(7)));
    if (A32(8)) MEM32(A32(8)) = SUCCEEDED(hr) ? new_token(K_VOLTEX, t, g_ecx) : 0;
    RET((uint32_t)hr, 9);
}
void d_CreateCubeTexture() {
    IDirect3DCubeTexture9 *t = nullptr;
    HRESULT hr = DEV->CreateCubeTexture(A32(0), A32(1), A32(2), (D3DFORMAT)A32(3),
                                        (D3DPOOL)A32(4), &t, (HANDLE *)P(A32(5)));
    if (A32(6)) MEM32(A32(6)) = SUCCEEDED(hr) ? new_token(K_CUBETEX, t, g_ecx) : 0;
    RET((uint32_t)hr, 7);
}
void d_CreateVertexBuffer() {
    IDirect3DVertexBuffer9 *b = nullptr;
    /* The engine pushes `this` last (sub_00757ED0 at 0x757F4C does
     * push 0; push &out; push 1; push 0; push 0; push size; push token),
     * so ka_vtable_note_import sees a token at g_esp and sets
     * g_d9_stack_this = 1: A32(0) is the first real parameter and the
     * out pointer is A32(4) with pSharedHandle at A32(5). Passing them
     * the other way wrote the token into the (zero) pShared slot, the
     * engine then read a NULL ppVertexBuffer and faulted at guest VA 0
     * (recomp_0040.c:21646). */
    HRESULT hr = DEV->CreateVertexBuffer(A32(0), A32(1), A32(2), (D3DPOOL)A32(3), &b,
                                         (HANDLE *)P(A32(5)));
    if (A32(4)) MEM32(A32(4)) = SUCCEEDED(hr) ? new_token(K_VBUFFER, b, g_ecx) : 0;
    RET((uint32_t)hr, 6);
}
void d_CreateIndexBuffer() {
    IDirect3DIndexBuffer9 *b = nullptr;
    HRESULT hr = DEV->CreateIndexBuffer(A32(0), A32(1), (D3DFORMAT)A32(2), (D3DPOOL)A32(3),
                                        &b, (HANDLE *)P(A32(5)));
    /* Same argument order as CreateVertexBuffer: the engine pushes the out
     * pointer last-but-one and pSharedHandle last, so A32(4) is ppIndexBuffer
     * and A32(5) is pShared. Writing the token to A32(5) left the engine's
     * pointer NULL and it faulted at guest VA 0 right after dev:27. */
    if (A32(4)) MEM32(A32(4)) = SUCCEEDED(hr) ? new_token(K_IBUFFER, b, g_ecx) : 0;
    RET((uint32_t)hr, 6);
}
static int g_crt_log, g_cds_log;   /* token-contract logs, capped */
/* 8 arguments: (w, h, fmt, ms, msq, lockable, ppSurface, pSharedHandle).
 * The engine pushes `this` last here too (same codegen as CreateVertexBuffer),
 * so the first real argument is A32(0) and ppSurface is A32(6) with
 * pSharedHandle at A32(7). Writing the token through A32(7) - the pSharedHandle
 * slot, which the engine leaves NULL - skipped the store entirely: the engine
 * kept a NULL render-target surface, later handed it to SetRenderTarget(0, 0),
 * DXVK answered D3DERR_INVALIDCALL, the 0x756B00 gate bailed and the technique
 * install never copied its descriptor (measured [shader+0x30]+4 == 0 on the
 * device, a live surface on Windows). */
void d_CreateRenderTarget() {
    IDirect3DSurface9 *s = nullptr;
    HRESULT hr = DEV->CreateRenderTarget(A32(0), A32(1), (D3DFORMAT)A32(2),
                                         (D3DMULTISAMPLE_TYPE)A32(3), A32(4), A32(5), &s,
                                         (HANDLE *)P(A32(7)));
    uint32_t tok = SUCCEEDED(hr) ? new_token(K_SURFACE, s, g_ecx) : 0;
    if (A32(6)) MEM32(A32(6)) = tok;
    if (g_crt_log < 6) {
        g_crt_log++;
        ka_log("d3d9: CreateRenderTarget %ux%u fmt=%u -> 0x%08X out=%08X shared=%08X tok=%08X",
               A32(0), A32(1), A32(2), (unsigned)hr, A32(6), A32(7), tok);
    }
    RET((uint32_t)hr, 8);
}
void d_CreateDepthStencilSurface() {
    IDirect3DSurface9 *s = nullptr;
    HRESULT hr = DEV->CreateDepthStencilSurface(A32(0), A32(1), (D3DFORMAT)A32(2),
                                                (D3DMULTISAMPLE_TYPE)A32(3), A32(4), A32(5),
                                                &s, (HANDLE *)P(A32(7)));
    uint32_t tok = SUCCEEDED(hr) ? new_token(K_SURFACE, s, g_ecx) : 0;
    if (A32(6)) MEM32(A32(6)) = tok;
    if (g_cds_log < 6) {
        g_cds_log++;
        ka_log("d3d9: CreateDepthStencilSurface %ux%u fmt=%u -> 0x%08X out=%08X shared=%08X tok=%08X",
               A32(0), A32(1), A32(2), (unsigned)hr, A32(6), A32(7), tok);
    }
    RET((uint32_t)hr, 8);
}
/* Interface arguments are guest TOKENS, not pointers. P(token) hands DXVK a
 * fake IDirect3DSurface9 whose vtable slot is guest memory; the first method it
 * calls faults at NULL+small (measured: addr=0x24 inside
 * D3D9DeviceEx::StretchRect from sub_00756580). Resolve every interface
 * argument through obj_of() and pass the real object, nullptr when unknown. */
static IDirect3DSurface9 *surf_arg(int i) {
    Obj *o = obj_of(A32(i));
    return o ? (IDirect3DSurface9 *)o->p : nullptr;
}
static IDirect3DBaseTexture9 *tex_arg(int i) {
    Obj *o = obj_of(A32(i));
    return o ? (IDirect3DBaseTexture9 *)o->p : nullptr;
}
void d_UpdateSurface() { RET_HR(DEV->UpdateSurface(surf_arg(0), (const RECT *)P(A32(1)), surf_arg(2), (const POINT *)P(A32(3))), 4); }
void d_UpdateTexture() { RET_HR(DEV->UpdateTexture(tex_arg(0), tex_arg(1)), 2); }
void d_GetRenderTargetData() { RET_HR(DEV->GetRenderTargetData(surf_arg(0), surf_arg(1)), 2); }
void d_GetFrontBufferData() { RET_HR(DEV->GetFrontBufferData(A32(0), surf_arg(1)), 2); }
void d_StretchRect() {
    RET_HR(DEV->StretchRect(surf_arg(0), (const RECT *)P(A32(1)),
                            surf_arg(2), (const RECT *)P(A32(3)),
                            (D3DTEXTUREFILTERTYPE)A32(4)), 5);
}
void d_ColorFill() {
    RET_HR(DEV->ColorFill(surf_arg(0), (const RECT *)P(A32(1)), A32(2)), 3);
}
void d_CreateOffscreenPlainSurface() {
    IDirect3DSurface9 *s = nullptr;
    HRESULT hr = DEV->CreateOffscreenPlainSurface(A32(0), A32(1), (D3DFORMAT)A32(2),
                                                  (D3DPOOL)A32(3), &s, (HANDLE *)P(A32(5)));
    if (A32(4)) MEM32(A32(4)) = SUCCEEDED(hr) ? new_token(K_SURFACE, s, g_ecx) : 0;
    RET((uint32_t)hr, 6);
}

/* Object-token out parameters: the caller's pointer holds a guest token. */
/* HRESULT failure log for the four calls in the technique-install gate
 * (sub_00756B00 / sub_00756600). They bail on any negative HRESULT with `js`,
 * so a silently failing shim turns into a skipped technique install and a NULL
 * technique pointer two frames later. Cap the lines: the gate runs per frame. */
static int g_hr_log_rt, g_hr_log_ds, g_hr_log_rs, g_hr_log_bb;
void d_SetRenderTarget() {
    Obj *o = obj_of(A32(1));
    uint32_t gtok = A32(1);
    HRESULT hr = DEV->SetRenderTarget(A32(0), o ? (IDirect3DSurface9 *)o->p : nullptr);
    if (g_hr_log_rt < 40) {
        g_hr_log_rt++;
        ka_log("d3d9: SetRenderTarget(%u, tok=%08X %s) -> 0x%08X", A32(0), gtok,
               o ? "known" : "UNKNOWN", (unsigned)hr);
    }
    RET((uint32_t)hr, 2);
}
void d_GetRenderTarget() {
    IDirect3DSurface9 *s = nullptr;
    HRESULT hr = DEV->GetRenderTarget(A32(0), &s);
    if (A32(1)) MEM32(A32(1)) = SUCCEEDED(hr) ? new_token(K_SURFACE, s, g_ecx) : 0;
    RET((uint32_t)hr, 2);
}
void d_SetDepthStencilSurface() {
    Obj *o = obj_of(A32(0));
    RET_HR(DEV->SetDepthStencilSurface(o ? (IDirect3DSurface9 *)o->p : nullptr), 1);
}
void d_GetDepthStencilSurface() {
    IDirect3DSurface9 *s = nullptr;
    HRESULT hr = DEV->GetDepthStencilSurface(&s);
    if (FAILED(hr) && g_hr_log_ds < 12) {
        g_hr_log_ds++;
        ka_log("d3d9: GetDepthStencilSurface -> 0x%08X surf=%p", (unsigned)hr, (void *)s);
    }
    if (A32(0)) MEM32(A32(0)) = SUCCEEDED(hr) ? new_token(K_SURFACE, s, g_ecx) : 0;
    RET((uint32_t)hr, 1);
}
void d_BeginScene() { RET(0, 0); }
void d_EndScene() { RET(0, 0); }
void d_Clear() {
    RET_HR(DEV->Clear(A32(0), (const D3DRECT *)P(A32(1)), A32(2), A32(3), (float)A32(4), A32(5)), 6);
}
void d_SetTransform() {
    RET_HR(DEV->SetTransform((D3DTRANSFORMSTATETYPE)A32(0), (const D3DMATRIX *)P(A32(1))), 2);
}
void d_GetTransform() {
    RET_HR(DEV->GetTransform((D3DTRANSFORMSTATETYPE)A32(0), (D3DMATRIX *)P(A32(1))), 2);
}
void d_MultiplyTransform() {
    RET_HR(DEV->MultiplyTransform((D3DTRANSFORMSTATETYPE)A32(0), (const D3DMATRIX *)P(A32(1))), 2);
}
void d_SetViewport() { RET_HR(DEV->SetViewport((const D3DVIEWPORT9 *)P(A32(0))), 1); }
void d_GetViewport() { RET_HR(DEV->GetViewport((D3DVIEWPORT9 *)P(A32(0))), 1); }
void d_SetMaterial() { RET_HR(DEV->SetMaterial((const D3DMATERIAL9 *)P(A32(0))), 1); }
void d_GetMaterial() { RET_HR(DEV->GetMaterial((D3DMATERIAL9 *)P(A32(0))), 1); }
void d_SetLight() { RET_HR(DEV->SetLight(A32(0), (const D3DLIGHT9 *)P(A32(1))), 2); }
void d_GetLight() { RET_HR(DEV->GetLight(A32(0), (D3DLIGHT9 *)P(A32(1))), 2); }
void d_LightEnable() { RET_HR(DEV->LightEnable(A32(0), A32(1) != 0), 2); }
void d_GetLightEnable() { RET_HR(DEV->GetLightEnable(A32(0), (BOOL *)P(A32(1))), 2); }
void d_SetClipPlane() { RET_HR(DEV->SetClipPlane(A32(0), (const float *)P(A32(1))), 2); }
void d_GetClipPlane() { RET_HR(DEV->GetClipPlane(A32(0), (float *)P(A32(1))), 2); }
void d_SetRenderState() {
    RET_HR(DEV->SetRenderState((D3DRENDERSTATETYPE)A32(0), A32(1)), 2);
}
void d_GetRenderState() {
    HRESULT hr = DEV->GetRenderState((D3DRENDERSTATETYPE)A32(0), (DWORD *)P(A32(1)));
    if (FAILED(hr) && g_hr_log_rs < 12) {
        g_hr_log_rs++;
        ka_log("d3d9: GetRenderState(%u) -> 0x%08X", A32(0), (unsigned)hr);
    }
    RET((uint32_t)hr, 2);
}
void d_CreateStateBlock() {
    IDirect3DStateBlock9 *sb = nullptr;
    HRESULT hr = DEV->CreateStateBlock((D3DSTATEBLOCKTYPE)A32(0), &sb);
    if (A32(1)) MEM32(A32(1)) = SUCCEEDED(hr) ? new_token(K_STATEBLOCK, sb, g_ecx) : 0;
    RET((uint32_t)hr, 2);
}
void d_BeginStateBlock() { RET(0, 0); }
void d_EndStateBlock() {
    IDirect3DStateBlock9 *sb = nullptr;
    HRESULT hr = DEV->EndStateBlock(&sb);
    if (A32(0)) MEM32(A32(0)) = SUCCEEDED(hr) ? new_token(K_STATEBLOCK, sb, g_ecx) : 0;
    RET((uint32_t)hr, 1);
}
void d_SetClipStatus() { RET_HR(DEV->SetClipStatus((const D3DCLIPSTATUS9 *)P(A32(0))), 1); }
void d_GetClipStatus() { RET_HR(DEV->GetClipStatus((D3DCLIPSTATUS9 *)P(A32(0))), 1); }
void d_GetTexture() {
    IDirect3DBaseTexture9 *t = nullptr;
    HRESULT hr = DEV->GetTexture(A32(0), &t);
    if (A32(1)) MEM32(A32(1)) = SUCCEEDED(hr) && t ? new_token(K_TEXTURE, t, g_ecx) : 0;
    RET((uint32_t)hr, 2);
}
void d_SetTexture() {
    Obj *o = obj_of(A32(1));
    RET_HR(DEV->SetTexture(A32(0), o ? (IDirect3DBaseTexture9 *)o->p : nullptr), 2);
}
void d_GetTextureStageState() {
    RET_HR(DEV->GetTextureStageState(A32(0), (D3DTEXTURESTAGESTATETYPE)A32(1), (DWORD *)P(A32(2))), 3);
}
void d_SetTextureStageState() {
    RET_HR(DEV->SetTextureStageState(A32(0), (D3DTEXTURESTAGESTATETYPE)A32(1), A32(2)), 3);
}
void d_GetSamplerState() {
    RET_HR(DEV->GetSamplerState(A32(0), (D3DSAMPLERSTATETYPE)A32(1), (DWORD *)P(A32(2))), 3);
}
void d_SetSamplerState() {
    RET_HR(DEV->SetSamplerState(A32(0), (D3DSAMPLERSTATETYPE)A32(1), A32(2)), 3);
}
void d_ValidateDevice() { RET_HR(DEV->ValidateDevice((DWORD *)P(A32(0))), 1); }
void d_SetPaletteEntries() { RET(0, 2); }
void d_GetPaletteEntries() { RET(0, 2); }
void d_SetCurrentTexturePalette() { RET(0, 1); }
void d_GetCurrentTexturePalette() { RET(0, 1); }
void d_SetScissorRect() { RET_HR(DEV->SetScissorRect((const RECT *)P(A32(0))), 1); }
void d_GetScissorRect() { RET_HR(DEV->GetScissorRect((RECT *)P(A32(0))), 1); }
void d_SetSoftwareVertexProcessing() { RET(0, 1); }
void d_GetSoftwareVertexProcessing() { RET(0, 0); }
void d_SetNPatchMode() { RET(0, 1); }
void d_GetNPatchMode() { RET(0, 0); }

void d_DrawPrimitive() {
    RET_HR(DEV->DrawPrimitive((D3DPRIMITIVETYPE)A32(0), A32(1), A32(2)), 3);
}
void d_DrawIndexedPrimitive() {
    RET_HR(DEV->DrawIndexedPrimitive((D3DPRIMITIVETYPE)A32(0), A32(1), A32(2), A32(3),
                                     A32(4), A32(5)), 6);
}
void d_DrawPrimitiveUP() {
    RET_HR(DEV->DrawPrimitiveUP((D3DPRIMITIVETYPE)A32(0), A32(1), P(A32(2)), A32(3)), 4);
}
void d_DrawIndexedPrimitiveUP() {
    RET_HR(DEV->DrawIndexedPrimitiveUP((D3DPRIMITIVETYPE)A32(0), A32(1), A32(2), A32(3),
                                       P(A32(4)), (D3DFORMAT)A32(5), P(A32(6)), A32(7)), 8);
}
void d_ProcessVertices() { RET(0, 6); }
void d_CreateVertexDeclaration() {
    IDirect3DVertexDeclaration9 *d = nullptr;
    HRESULT hr = DEV->CreateVertexDeclaration((const D3DVERTEXELEMENT9 *)P(A32(0)), &d);
    if (A32(1)) MEM32(A32(1)) = SUCCEEDED(hr) ? new_token(K_VDECL, d, g_ecx) : 0;
    RET((uint32_t)hr, 2);
}
void d_SetVertexDeclaration() {
    Obj *o = obj_of(A32(0));
    RET_HR(DEV->SetVertexDeclaration(o ? (IDirect3DVertexDeclaration9 *)o->p : nullptr), 1);
}
void d_GetVertexDeclaration() {
    IDirect3DVertexDeclaration9 *d = nullptr;
    HRESULT hr = DEV->GetVertexDeclaration(&d);
    if (A32(0)) MEM32(A32(0)) = SUCCEEDED(hr) ? new_token(K_VDECL, d, g_ecx) : 0;
    RET((uint32_t)hr, 1);
}
void d_SetFVF() { RET_HR(DEV->SetFVF(A32(0)), 1); }
void d_GetFVF() { RET_HR(DEV->GetFVF((DWORD *)P(A32(0))), 1); }
void d_CreateVertexShader() {
    IDirect3DVertexShader9 *s = nullptr;
    const void *code = P(A32(0));
    static int n;
    if (n++ < 4)
        ka_log("d3d9: CreateVertexShader code=%p f=%08X out=%08X",
               code, A32(0), A32(1));
    HRESULT hr = DEV->CreateVertexShader((const DWORD *)code, &s);
    if (A32(1)) MEM32(A32(1)) = SUCCEEDED(hr) ? new_token(K_VS, s, g_ecx) : 0;
    RET((uint32_t)hr, 2);
}
void d_SetVertexShader() {
    Obj *o = obj_of(A32(0));
    RET_HR(DEV->SetVertexShader(o ? (IDirect3DVertexShader9 *)o->p : nullptr), 1);
}
void d_GetVertexShader() {
    IDirect3DVertexShader9 *s = nullptr;
    HRESULT hr = DEV->GetVertexShader(&s);
    if (A32(0)) MEM32(A32(0)) = SUCCEEDED(hr) ? new_token(K_VS, s, g_ecx) : 0;
    RET((uint32_t)hr, 1);
}
void d_SetVertexShaderConstantF() {
    RET_HR(DEV->SetVertexShaderConstantF(A32(0), (const float *)P(A32(1)), A32(2)), 3);
}
void d_GetVertexShaderConstantF() {
    RET_HR(DEV->GetVertexShaderConstantF(A32(0), (float *)P(A32(1)), A32(2)), 3);
}
void d_SetVertexShaderConstantI() {
    RET_HR(DEV->SetVertexShaderConstantI(A32(0), (const int *)P(A32(1)), A32(2)), 3);
}
void d_GetVertexShaderConstantI() {
    RET_HR(DEV->GetVertexShaderConstantI(A32(0), (int *)P(A32(1)), A32(2)), 3);
}
void d_SetVertexShaderConstantB() {
    RET_HR(DEV->SetVertexShaderConstantB(A32(0), (const BOOL *)P(A32(1)), A32(2)), 3);
}
void d_GetVertexShaderConstantB() {
    RET_HR(DEV->GetVertexShaderConstantB(A32(0), (BOOL *)P(A32(1)), A32(2)), 3);
}
void d_SetStreamSource() {
    Obj *o = obj_of(A32(1));
    RET_HR(DEV->SetStreamSource(A32(0), o ? (IDirect3DVertexBuffer9 *)o->p : nullptr,
                                A32(2), A32(3)), 4);
}
void d_GetStreamSource() {
    IDirect3DVertexBuffer9 *b = nullptr;
    UINT offset = 0, stride = 0;
    HRESULT hr = DEV->GetStreamSource(A32(0), &b, &offset, &stride);
    if (A32(1)) MEM32(A32(1)) = SUCCEEDED(hr) && b ? new_token(K_VBUFFER, b, g_ecx) : 0;
    if (A32(2)) MEM32(A32(2)) = offset;
    if (A32(3)) MEM32(A32(3)) = stride;
    RET((uint32_t)hr, 4);
}
void d_SetStreamSourceFreq() { RET_HR(DEV->SetStreamSourceFreq(A32(0), A32(1)), 2); }
void d_GetStreamSourceFreq() { RET_HR(DEV->GetStreamSourceFreq(A32(0), (UINT *)P(A32(1))), 2); }
void d_SetIndices() {
    Obj *o = obj_of(A32(0));
    RET_HR(DEV->SetIndices(o ? (IDirect3DIndexBuffer9 *)o->p : nullptr), 1);
}
void d_GetIndices() {
    IDirect3DIndexBuffer9 *b = nullptr;
    HRESULT hr = DEV->GetIndices(&b);
    if (A32(0)) MEM32(A32(0)) = SUCCEEDED(hr) && b ? new_token(K_IBUFFER, b, g_ecx) : 0;
    RET((uint32_t)hr, 1);
}
void d_CreatePixelShader() {
    IDirect3DPixelShader9 *s = nullptr;
    const void *code = P(A32(0));
    static int n;
    if (n++ < 4)
        ka_log("d3d9: CreatePixelShader code=%p f=%08X out=%08X",
               code, A32(0), A32(1));
    HRESULT hr = DEV->CreatePixelShader((const DWORD *)code, &s);
    if (A32(1)) MEM32(A32(1)) = SUCCEEDED(hr) ? new_token(K_PS, s, g_ecx) : 0;
    RET((uint32_t)hr, 2);
}
void d_SetPixelShader() {
    Obj *o = obj_of(A32(0));
    RET_HR(DEV->SetPixelShader(o ? (IDirect3DPixelShader9 *)o->p : nullptr), 1);
}
void d_GetPixelShader() {
    IDirect3DPixelShader9 *s = nullptr;
    HRESULT hr = DEV->GetPixelShader(&s);
    if (A32(0)) MEM32(A32(0)) = SUCCEEDED(hr) ? new_token(K_PS, s, g_ecx) : 0;
    RET((uint32_t)hr, 1);
}
void d_SetPixelShaderConstantF() {
    RET_HR(DEV->SetPixelShaderConstantF(A32(0), (const float *)P(A32(1)), A32(2)), 3);
}
void d_GetPixelShaderConstantF() {
    RET_HR(DEV->GetPixelShaderConstantF(A32(0), (float *)P(A32(1)), A32(2)), 3);
}
void d_SetPixelShaderConstantI() { RET(0, 3); }
void d_GetPixelShaderConstantI() { RET(0, 3); }
void d_SetPixelShaderConstantB() { RET(0, 3); }
void d_GetPixelShaderConstantB() { RET(0, 3); }
void d_DrawRectPatch() { RET(0, 3); }   /* DrawRectPatch(Handle, pNumSegs, pInfo) */
void d_DrawTriPatch() { RET(0, 3); }
void d_DeletePatch() { RET(0, 1); }
void d_CreateQuery() {
    IDirect3DQuery9 *q = nullptr;
    HRESULT hr = DEV->CreateQuery((D3DQUERYTYPE)A32(0), &q);
    if (A32(1)) MEM32(A32(1)) = SUCCEEDED(hr) ? new_token(K_QUERY, q, g_ecx) : 0;
    RET((uint32_t)hr, 2);
}

/* ------------------------------------------------------------- swapchain */
void s_Present() {
    RET_HR(SELF_AS(IDirect3DSwapChain9, K_SWAPCHAIN)->Present(
            (const RECT *)P(A32(0)), (const RECT *)P(A32(1)), (HWND)(uintptr_t)A32(2), (const RGNDATA *)P(A32(3)), 0), 4);
}
void s_GetFrontBufferData() { RET_HR(SELF_AS(IDirect3DSwapChain9, K_SWAPCHAIN)->GetFrontBufferData(surf_arg(0)), 1); }
void s_GetBackBuffer() {
    IDirect3DSurface9 *s = nullptr;
    HRESULT hr = SELF_AS(IDirect3DSwapChain9, K_SWAPCHAIN)->GetBackBuffer(A32(0), (D3DBACKBUFFER_TYPE)A32(1), &s);
    uint32_t tok = 0;
    if (A32(2)) {
        MEM32(A32(2)) = SUCCEEDED(hr) ? (tok = new_token(K_SURFACE, s, g_ecx)) : 0;
    }
    if (g_hr_log_bb < 20) {
        g_hr_log_bb++;
        ka_log("d3d9: sc GetBackBuffer(%u,%u) -> 0x%08X surf=%p tok=%08X", A32(0), A32(1),
               (unsigned)hr, (void *)s, tok);
    }
    RET((uint32_t)hr, 3);
}
void s_GetRasterStatus() { RET_HR(SELF_AS(IDirect3DSwapChain9, K_SWAPCHAIN)->GetRasterStatus((D3DRASTER_STATUS *)P(A32(0))), 1); }
void s_GetDisplayMode() { RET_HR(SELF_AS(IDirect3DSwapChain9, K_SWAPCHAIN)->GetDisplayMode((D3DDISPLAYMODE *)P(A32(0))), 1); }
void s_GetDevice() { RET(self_obj() ? self_obj()->owner : 0, 0); }
void s_GetPresentParameters() {
    void *dst = P(A32(0));
    D3DPRESENT_PARAMETERS pp;
    HRESULT hr;
    memset(&pp, 0, sizeof pp);
    hr = SELF_AS(IDirect3DSwapChain9, K_SWAPCHAIN)->GetPresentParameters(&pp);
    if (SUCCEEDED(hr) && dst) pp_to_guest(&pp, (KaPP *)dst);
    RET((uint32_t)hr, 1);
}

/* --------------------------------------------------------------- textures */
#define TEX (reinterpret_cast<IDirect3DTexture9 *>(self_obj()->p))
void t_GetLevelDesc() { RET_HR(TEX->GetLevelDesc(A32(0), (D3DSURFACE_DESC *)P(A32(1))), 2); }
void t_GetSurfaceLevel() {
    IDirect3DSurface9 *s = nullptr;
    HRESULT hr = TEX->GetSurfaceLevel(A32(0), &s);
    if (A32(1)) MEM32(A32(1)) = SUCCEEDED(hr) ? new_token(K_SURFACE, s, g_ecx) : 0;
    RET((uint32_t)hr, 2);
}
void t_LockRect() {
    Obj *o = self_obj();
    D3DLOCKED_RECT lr{};
    D3DSURFACE_DESC desc{};
    HRESULT hr = TEX->LockRect(A32(0), &lr, (const RECT *)P(A32(2)), A32(3));
    if (SUCCEEDED(hr)) {
        TEX->GetLevelDesc(A32(0), &desc);
        uint32_t size = lr.Pitch * desc.Height;
        if (o->lock_va) ka_guest_free(o->lock_va);   /* lock without unlock */
        o->lock_va = ka_guest_alloc(size, 16);
        o->lock_host = lr.pBits;
        o->lock_size = size;
        o->lock_pitch = lr.Pitch;
        uint32_t out = A32(1);
        if (out) {
            MEM32(out) = lr.Pitch;
            MEM32(out + 4) = o->lock_va;
        }
    } else {
        if (A32(1)) { MEM32(A32(1)) = 0; MEM32(A32(1) + 4) = 0; }
        ka_log("d3d9: Texture::LockRect(%u) -> 0x%08X", A32(0), (unsigned)hr);
    }
    RET((uint32_t)hr, 4);
}
void t_UnlockRect() {
    Obj *o = self_obj();
    if (o->lock_host && o->lock_va)
        memcpy(o->lock_host, ka_host(o->lock_va), o->lock_size);
    /* The staging buffer is ours: LockRect allocates it and the guest's copy
     * goes back through it, so it dies with the lock. It used to be leaked -
     * a 1280x720 surface lock is 921,600 bytes, the engine locks a surface
     * every frame, and the leak filled the 1 GiB guest heap in ~780 locks;
     * the engine's next memcpy then wrote through the NULL the exhausted
     * allocator returned (docs/FINDINGS.md, 2026-11-21). */
    if (o->lock_va) { ka_guest_free(o->lock_va); o->lock_va = 0; }
    o->lock_host = nullptr;
    o->lock_size = 0;
    RET_HR(TEX->UnlockRect(A32(0)), 1);
}
void t_AddDirtyRect() { RET_HR(TEX->AddDirtyRect((const RECT *)P(A32(0))), 1); }

/* --------------------------------------------------------------- surfaces */
#define SURF (reinterpret_cast<IDirect3DSurface9 *>(self_obj()->p))
void s_GetDesc() { RET_HR(SURF->GetDesc((D3DSURFACE_DESC *)P(A32(0))), 1); }
void s_LockRect() {
    Obj *o = self_obj();
    D3DLOCKED_RECT lr{};
    D3DSURFACE_DESC desc{};
    /* (UINT Flags, D3DLOCKED_RECT *pLockedRect, const RECT *pRect) */
    HRESULT hr = SURF->LockRect(&lr, (const RECT *)P(A32(2)), A32(0));
    if (SUCCEEDED(hr)) {
        SURF->GetDesc(&desc);
        uint32_t size = lr.Pitch * desc.Height;
        if (o->lock_va) ka_guest_free(o->lock_va);   /* lock without unlock */
        o->lock_va = ka_guest_alloc(size, 16);
        o->lock_host = lr.pBits;
        o->lock_size = size;
        o->lock_pitch = lr.Pitch;
        uint32_t out = A32(1);
        if (out) { MEM32(out) = lr.Pitch; MEM32(out + 4) = o->lock_va; }
    }
    RET((uint32_t)hr, 3);
}
void s_UnlockRect() {
    Obj *o = self_obj();
    if (o->lock_host && o->lock_va)
        memcpy(o->lock_host, ka_host(o->lock_va), o->lock_size);
    if (o->lock_va) { ka_guest_free(o->lock_va); o->lock_va = 0; }   /* see t_UnlockRect */
    o->lock_host = nullptr;
    o->lock_size = 0;
    RET_HR(SURF->UnlockRect(), 0);
}
void s_GetDC() { RET_HR(SURF->GetDC((HDC *)P(A32(0))), 1); }
void s_ReleaseDC() { RET_HR(SURF->ReleaseDC((HDC)(uintptr_t)A32(0)), 1); }
void s_GetContainer() { RET(0x80004005u, 2); }

/* ------------------------------------------------------ vertex/index buffers */
void b_GetDesc() {
    if (self_obj()->kind == K_VBUFFER)
        RET_HR(reinterpret_cast<IDirect3DVertexBuffer9 *>(self_obj()->p)->GetDesc((D3DVERTEXBUFFER_DESC *)P(A32(0))), 1);
    RET_HR(reinterpret_cast<IDirect3DIndexBuffer9 *>(self_obj()->p)->GetDesc((D3DINDEXBUFFER_DESC *)P(A32(0))), 1);
}
void b_Lock() {
    Obj *o = self_obj();
    void *host = nullptr;
    HRESULT hr;
    /* Lock(OffsetToLock, SizeToLock, ppbData, Flags). The engine's call site
     * (sub_00757ED0 at 0x757F63) pushes Flags last, then ppbData, Size and
     * Offset, with `this` as arg0 - so with the stack shift A32(2) is the
     * guest's ppbData and A32(3) is Flags. This used to pass A32(2) - a stack
     * address - as Flags, DXVK answered D3DERR_INVALIDCALL, and the engine's
     * failure path released the buffer and wrote through a NULL pointer
     * (crash at guest VA 0). */
    if (o->kind == K_VBUFFER)
        hr = reinterpret_cast<IDirect3DVertexBuffer9 *>(o->p)->Lock(A32(0), A32(1), &host, A32(3));
    else
        hr = reinterpret_cast<IDirect3DIndexBuffer9 *>(o->p)->Lock(A32(0), A32(1), &host, A32(3));
    if (SUCCEEDED(hr)) {
        if (o->lock_va) ka_guest_free(o->lock_va);   /* lock without unlock */
        o->lock_va = ka_guest_alloc(A32(1) ? A32(1) : 0x10000, 16);
        o->lock_host = host;
        o->lock_size = A32(1);
    }
    if (A32(2)) MEM32(A32(2)) = SUCCEEDED(hr) ? o->lock_va : 0;
    RET((uint32_t)hr, 4);
}
void b_Unlock() {
    Obj *o = self_obj();
    if (o->lock_host && o->lock_va && o->lock_size)
        memcpy(o->lock_host, ka_host(o->lock_va), o->lock_size);
    if (o->lock_va) { ka_guest_free(o->lock_va); o->lock_va = 0; }   /* see t_UnlockRect */
    o->lock_host = nullptr;
    o->lock_size = 0;
    if (o->kind == K_VBUFFER)
        RET_HR(reinterpret_cast<IDirect3DVertexBuffer9 *>(o->p)->Unlock(), 0);
    RET_HR(reinterpret_cast<IDirect3DIndexBuffer9 *>(o->p)->Unlock(), 0);
}

/* ------------------------------------------------------------------ shaders */
void sh_GetFunction() {
    void *data = nullptr;
    UINT size = 0;
    HRESULT hr;
    if (self_obj()->kind == K_VS)
        hr = reinterpret_cast<IDirect3DVertexShader9 *>(self_obj()->p)->GetFunction(&data, &size);
    else
        hr = reinterpret_cast<IDirect3DPixelShader9 *>(self_obj()->p)->GetFunction(&data, &size);
    if (A32(0)) memcpy(P(A32(0)), data, size);
    if (A32(1)) MEM32(A32(1)) = size;
    RET((uint32_t)hr, 2);
}
void sh_GetDevice() { RET(self_obj() ? self_obj()->owner : 0, 0); }

/* ------------------------------------------------------- vertex declarations */
void vd_GetDeclaration() {
    UINT size = 0;
    auto *d = reinterpret_cast<IDirect3DVertexDeclaration9 *>(self_obj()->p);
    HRESULT hr = d->GetDeclaration(nullptr, &size);
    if (A32(0) && A32(1) && MEM32(A32(1)) >= size) d->GetDeclaration((D3DVERTEXELEMENT9 *)P(A32(0)), &size);
    if (A32(1)) MEM32(A32(1)) = size;
    RET((uint32_t)hr, 2);
}

/* ------------------------------------------------------------ state blocks */
void sb_Capture() { RET_HR(reinterpret_cast<IDirect3DStateBlock9 *>(self_obj()->p)->Capture(), 0); }
void sb_Apply() { RET_HR(reinterpret_cast<IDirect3DStateBlock9 *>(self_obj()->p)->Apply(), 0); }

/* ------------------------------------------------------------------ queries */
void q_GetData() {
    void *data = nullptr;
    DWORD size = A32(1);
    HRESULT hr = reinterpret_cast<IDirect3DQuery9 *>(self_obj()->p)->GetData(P(A32(0)), size, A32(2));
    (void)data;
    RET((uint32_t)hr, 3);
}
void q_Issue() { RET_HR(reinterpret_cast<IDirect3DQuery9 *>(self_obj()->p)->Issue(A32(0)), 1); }
void q_GetType() { RET(reinterpret_cast<IDirect3DQuery9 *>(self_obj()->p)->GetType(), 0); }
void q_GetDataSize() { RET(reinterpret_cast<IDirect3DQuery9 *>(self_obj()->p)->GetDataSize(), 0); }

/* ------------------------------------------------- D3DX9 constant tables */
/* The engine's minimum-shader init creates a vertex shader (dev:91), asks D3DX
 * for its constant table and treats a missing table as fatal: on device it
 * released the shader, released DInput, tore the pools down, and quit with
 * ExitProcess(0) - no frame, no message box. D3DX answers by parsing the CTAB
 * comment chunk that fxc embeds in the bytecode; this does the same, so the
 * engine's parameter binding sees the register set/index it expects. */
struct DxConst {
    char     name[96];
    uint32_t name_va;
    uint32_t reg_set, reg_index, reg_count;
    uint32_t rows, cols, elements, members;
    uint32_t bytes;
    uint32_t default_va;
};
struct DxConstTable {
    uint32_t creator_va;
    uint32_t version;
    uint32_t count;
    DxConst  c[64];
};
/* The comment payload starts with the 'CTAB' fourcc; every offset inside it
 * (creator, constant_info, name, typeinfo, defval) is relative to that fourcc,
 * so the header keeps it as its first field. Reading size as the first field
 * made constants land on the version token (0xFFFE0300) and the table was
 * rejected, which is why the first parse reported "no CTAB". */
struct DxCtabHdr  { uint32_t ctab, size, creator, version, constants, constant_info, flags, target; };
struct DxCtabInfo { uint32_t name; uint16_t regset, regindex, regcount, reserved; uint32_t typeinfo; uint32_t defval; };
struct DxCtabType { uint16_t klass, type, rows, cols, elements, members; uint32_t memberinfo; };

static uint32_t dx_guest_str(const char *s) {
    uint32_t n = (uint32_t)strlen(s) + 1u;
    uint32_t va = ka_guest_alloc(n, 1);
    if (va) memcpy(ka_host(va), s, n);
    return va;
}

/* Walk the instruction stream for the comment token whose payload begins with
 * 'CTAB'. Comment tokens are (dword_count << 16) | 0xFFFE; 0x0000FFFF is END. */
static uint32_t dx_scanned;   /* tokens walked by the last dx_find_ctab */

static const uint8_t *dx_find_ctab(const uint8_t *code) {
    const uint32_t *tok = (const uint32_t *)code;
    uint32_t i;
    static int dumped;
    if (dumped < 4) {
        dumped++;
        /* The first calls: what does the engine actually hand us? A real shader
         * blob starts with a version token (low word 0xFFFE, e.g. 0xFFFF0300)
         * and carries its constants as a 'CTAB' comment. */
        ka_log("d3dx: code at %p: %08X %08X %08X %08X %08X %08X %08X %08X",
               (const void *)code, tok[0], tok[1], tok[2], tok[3],
               tok[4], tok[5], tok[6], tok[7]);
    }
    dx_scanned = 0;
    for (i = 1; i < (1u << 16); i++) {
        uint32_t t = tok[i];
        dx_scanned = i;
        uint32_t lo = t & 0xFFFFu;
        if (lo == 0xFFFFu) return nullptr;
        if (lo == 0xFFFEu) {
            uint32_t words = (t >> 16) & 0x7FFFu;
            const uint8_t *p = (const uint8_t *)(tok + i + 1);
            if (words >= 2 && *(const uint32_t *)p == 0x42415443u) return p;  /* 'CTAB' */
            i += words;
        }
    }
    return nullptr;
}

static DxConstTable *dx_parse_ctab(const uint8_t *code) {
    const uint8_t *ctab = dx_find_ctab(code);
    const DxCtabHdr *h;
    DxConstTable *t;
    uint32_t i;
    if (!ctab) return nullptr;
    h = (const DxCtabHdr *)ctab;
    if (h->ctab != 0x42415443u || h->size < 0x1Cu) return nullptr;
    /* A shader with no uniforms still carries a CTAB with constants == 0; D3DX
     * returns a valid (empty) table for it. Rejecting zero here made such a
     * shader report "no CTAB", which aborted the engine's shader init. */
    if (h->constants > 64) return nullptr;
    t = (DxConstTable *)calloc(1, sizeof *t);
    if (!t) return nullptr;
    t->version = h->version;
    t->creator_va = h->creator ? dx_guest_str((const char *)(ctab + h->creator)) : 0;
    t->count = h->constants;
    for (i = 0; i < t->count; i++) {
        const DxCtabInfo *ci = (const DxCtabInfo *)(ctab + h->constant_info + i * sizeof(DxCtabInfo));
        const DxCtabType *ti = (const DxCtabType *)(ctab + ci->typeinfo);
        DxConst *c = &t->c[i];
        strncpy(c->name, (const char *)(ctab + ci->name), sizeof c->name - 1);
        c->name_va = dx_guest_str(c->name);
        c->reg_set = ci->regset;
        c->reg_index = ci->regindex;
        c->reg_count = ci->regcount;
        c->rows = ti->rows;
        c->cols = ti->cols;
        c->elements = ti->elements;
        c->members = ti->members;
        c->bytes = ti->rows * ti->cols * (ti->elements ? ti->elements : 1u) * 4u;
        /* Copy the default out of the bytecode: the engine may free the shader
         * blob after binding, and SetDefaults must still have valid data. */
        if (ci->defval && !ti->members && c->bytes && c->bytes <= 64) {
            uint32_t va = ka_guest_alloc(c->bytes, 4);
            if (va) {
                memcpy(ka_host(va), ctab + ci->defval, c->bytes);
                c->default_va = va;
            }
        }
    }
    ka_log("d3dx: constant table: version=0x%08X constants=%u creator=%s",
           t->version, t->count, t->creator_va ? (const char *)ka_host(t->creator_va) : "");
    return t;
}

static DxConstTable *ct_self(void) {
    Obj *o = self_obj();
    return (o && o->kind == K_D3DXCT) ? (DxConstTable *)o->p : nullptr;
}
static int ct_index(uint32_t h) { return (h >= 1 && h <= 64) ? (int)(h - 1) : -1; }

void ct_GetDesc() {                       /* D3DXCONSTANTTABLE_DESC *pDesc */
    DxConstTable *t = ct_self();
    uint32_t out = A32(0);
    if (!t || !out) RET(t ? 0x8876086Cu : 0x8876086Cu, 1);
    MEM32(out + 0) = t->creator_va;
    MEM32(out + 4) = t->version;
    MEM32(out + 8) = t->count;
    RET(0, 1);
}
void ct_GetConstant() {                   /* hConstant, UINT Index */
    DxConstTable *t = ct_self();
    uint32_t idx = A32(1);
    if (!t) RET(0, 2);
    RET(idx < t->count ? idx + 1 : 0, 2);
}
void ct_GetConstantByName() {             /* hConstant, LPCSTR pName */
    DxConstTable *t = ct_self();
    const char *name = (const char *)P(A32(1));
    if (!t || !name) RET(0, 2);
    for (uint32_t i = 0; i < t->count; i++)
        if (strcmp(t->c[i].name, name) == 0) RET(i + 1, 2);
    RET(0, 2);
}
void ct_GetConstantElement() {            /* hConstant, UINT Index */
    DxConstTable *t = ct_self();
    uint32_t h = A32(0), idx = A32(1);
    if (!t) RET(0, 2);
    RET(idx == 0 ? h : 0, 2);
}
void ct_GetConstantDesc() {               /* hConstant, D3DXCONSTANT_DESC *pDesc, UINT *pCount */
    DxConstTable *t = ct_self();
    uint32_t h = A32(0), out = A32(1), countp = A32(2);
    int idx = ct_index(h);
    if (!t) RET(0x8876086Cu, 3);
    if (idx < 0 || (uint32_t)idx >= t->count) {
        if (countp) MEM32(countp) = 0;
        RET(0x80070057u, 3);
    }
    if (out) {
        DxConst &c = t->c[idx];
        MEM32(out + 0)  = c.name_va;
        MEM32(out + 4)  = c.reg_set;
        MEM32(out + 8)  = c.reg_index;
        MEM32(out + 12) = c.reg_count;
        MEM32(out + 16) = c.rows;
        MEM32(out + 20) = c.cols;
        MEM32(out + 24) = c.elements;
        MEM32(out + 28) = c.members;
        MEM32(out + 32) = c.bytes;
        MEM32(out + 36) = 0;               /* StructMemberInfo */
        MEM32(out + 40) = c.default_va;
    }
    if (countp) MEM32(countp) = 1;
    RET(0, 3);
}
void ct_GetSamplerIndex() { RET(0xFFFFFFFFu, 1); }   /* not a sampler */
void ct_SetDefaults() { RET(0, 1); }

/* ------------------------------------------------------------ vtable build */
struct Slot { const char *name; recomp_func_t fn; };

#define S(n, f) { n, f }

const Slot kBase[] = {
    S("IUnknown.QueryInterface", u_QueryInterface),
    S("IUnknown.AddRef", u_AddRef),
    S("IUnknown.Release", u_Release),
    { nullptr, nullptr }
};

const Slot kResource[] = {
    S("Res.GetDevice", r_GetDevice),
    S("Res.SetPrivateData", r_SetPrivateData),
    S("Res.GetPrivateData", r_GetPrivateData),
    S("Res.FreePrivateData", r_FreePrivateData),
    S("Res.SetPriority", r_SetPriority),
    S("Res.GetPriority", r_GetPriority),
    S("Res.PreLoad", r_PreLoad),
    S("Res.GetType", r_GetType),
    { nullptr, nullptr }
};

/* Build a vtable whose entries are (prefix:index) shims. With shared ==
 * nullptr the whole list comes from `rest` (used for the interfaces that begin
 * with their own method at slot 0 of the combined list). */
int build_vtable(Kind kind, const char *prefix, const Slot *shared, const Slot *rest) {
    int n = 0;
    uint32_t vt = ka_guest_alloc(192 * 4, 4);
    if (shared) {
        for (const Slot *s = shared; s->name; s++, n++) {
            char name[96];
            snprintf(name, sizeof name, "%s:%d", prefix, n);
            uint32_t va = ka_shim_va(name);
            if (!va) va = shim_add_named(name, s->fn);
            MEM32(vt + (uint32_t)n * 4) = va;
        }
    }
    for (const Slot *s = rest; s->name; s++, n++) {
        char name[96];
        snprintf(name, sizeof name, "%s:%d", prefix, n);
        uint32_t va = ka_shim_va(name);
        if (!va) va = shim_add_named(name, s->fn);
        MEM32(vt + (uint32_t)n * 4) = va;
    }
    g_vt[kind][0] = vt;
    g_vt_built[kind] = 1;
    /* Slots past n are left 0, so a call to one dispatches to VA 0 (ICALL
     * unresolved + signal 11). Log the count so a short table is visible. */
    ka_log("d3d9: vtable %s has %d slots", prefix, n);
    return n;
}

} /* namespace */

void i9_RegisterSoftwareDevice() { RET(0x80004001u, 1); }   /* E_NOTIMPL */
void i9_GetAdapterMonitor() { RET(1, 1); }                  /* the fake HMONITOR */

/* The tables below must be in the exact interface order of d3d9.h (which is
 * DXVK's, and the guest's own headers' order). IDirect3D9 has
 * RegisterSoftwareDevice at slot 3 and CreateDevice last; the Ex methods come
 * after it in IDirect3D9Ex and must NOT be inserted here. */
static const Slot kD3D9[] = {
    S("RegisterSoftwareDevice", i9_RegisterSoftwareDevice),
    S("GetAdapterCount", i9_GetAdapterCount),
    S("GetAdapterIdentifier", i9_GetAdapterIdentifier),
    S("GetAdapterModeCount", i9_GetAdapterModeCount),
    S("EnumAdapterModes", i9_EnumAdapterModes),
    S("GetAdapterDisplayMode", i9_GetAdapterDisplayMode),
    S("CheckDeviceType", i9_CheckDeviceType),
    S("CheckDeviceFormat", i9_CheckDeviceFormat),
    S("CheckDeviceMultiSampleType", i9_CheckDeviceMultiSampleType),
    S("CheckDepthStencilMatch", i9_CheckDepthStencilMatch),
    S("CheckDeviceFormatConversion", i9_CheckDeviceFormatConversion),
    S("GetDeviceCaps", i9_GetDeviceCaps),
    S("GetAdapterMonitor", i9_GetAdapterMonitor),
    S("CreateDevice", i9_CreateDevice),
    { nullptr, nullptr }
};

static const Slot kDevice[] = {
    S("TestCooperativeLevel", d_TestCooperativeLevel),
    S("GetAvailableTextureMem", d_GetAvailableTextureMem),
    S("EvictManagedResources", d_EvictManagedResources),
    S("GetDirect3D", d_GetDirect3D),
    S("GetDeviceCaps", d_GetDeviceCaps),
    S("GetDisplayMode", d_GetDisplayMode),
    S("GetCreationParameters", d_GetCreationParameters),
    S("SetCursorProperties", d_SetCursorProperties),
    S("SetCursorPosition", d_SetCursorPosition),
    S("ShowCursor", d_ShowCursor),
    S("CreateAdditionalSwapChain", d_CreateAdditionalSwapChain),
    S("GetSwapChain", d_GetSwapChain),
    S("GetNumberOfSwapChains", d_GetNumberOfSwapChains),
    S("Reset", d_Reset),
    S("Present", d_Present),
    S("GetBackBuffer", d_GetBackBuffer),
    S("GetRasterStatus", d_GetRasterStatus),
    S("SetDialogBoxMode", d_SetDialogBoxMode),
    S("SetGammaRamp", d_SetGammaRamp),
    S("GetGammaRamp", d_GetGammaRamp),
    S("CreateTexture", d_CreateTexture),
    S("CreateVolumeTexture", d_CreateVolumeTexture),
    S("CreateCubeTexture", d_CreateCubeTexture),
    S("CreateVertexBuffer", d_CreateVertexBuffer),
    S("CreateIndexBuffer", d_CreateIndexBuffer),
    S("CreateRenderTarget", d_CreateRenderTarget),
    S("CreateDepthStencilSurface", d_CreateDepthStencilSurface),
    S("UpdateSurface", d_UpdateSurface),
    S("UpdateTexture", d_UpdateTexture),
    S("GetRenderTargetData", d_GetRenderTargetData),
    S("GetFrontBufferData", d_GetFrontBufferData),
    S("StretchRect", d_StretchRect),
    S("ColorFill", d_ColorFill),
    S("CreateOffscreenPlainSurface", d_CreateOffscreenPlainSurface),
    S("SetRenderTarget", d_SetRenderTarget),
    S("GetRenderTarget", d_GetRenderTarget),
    S("SetDepthStencilSurface", d_SetDepthStencilSurface),
    S("GetDepthStencilSurface", d_GetDepthStencilSurface),
    S("BeginScene", d_BeginScene),
    S("EndScene", d_EndScene),
    S("Clear", d_Clear),
    S("SetTransform", d_SetTransform),
    S("GetTransform", d_GetTransform),
    S("MultiplyTransform", d_MultiplyTransform),
    S("SetViewport", d_SetViewport),
    S("GetViewport", d_GetViewport),
    S("SetMaterial", d_SetMaterial),
    S("GetMaterial", d_GetMaterial),
    S("SetLight", d_SetLight),
    S("GetLight", d_GetLight),
    S("LightEnable", d_LightEnable),
    S("GetLightEnable", d_GetLightEnable),
    S("SetClipPlane", d_SetClipPlane),
    S("GetClipPlane", d_GetClipPlane),
    S("SetRenderState", d_SetRenderState),
    S("GetRenderState", d_GetRenderState),
    S("CreateStateBlock", d_CreateStateBlock),
    S("BeginStateBlock", d_BeginStateBlock),
    S("EndStateBlock", d_EndStateBlock),
    S("SetClipStatus", d_SetClipStatus),
    S("GetClipStatus", d_GetClipStatus),
    S("GetTexture", d_GetTexture),
    S("SetTexture", d_SetTexture),
    S("GetTextureStageState", d_GetTextureStageState),
    S("SetTextureStageState", d_SetTextureStageState),
    S("GetSamplerState", d_GetSamplerState),
    S("SetSamplerState", d_SetSamplerState),
    S("ValidateDevice", d_ValidateDevice),
    S("SetPaletteEntries", d_SetPaletteEntries),
    S("GetPaletteEntries", d_GetPaletteEntries),
    S("SetCurrentTexturePalette", d_SetCurrentTexturePalette),
    S("GetCurrentTexturePalette", d_GetCurrentTexturePalette),
    S("SetScissorRect", d_SetScissorRect),
    S("GetScissorRect", d_GetScissorRect),
    S("SetSoftwareVertexProcessing", d_SetSoftwareVertexProcessing),
    S("GetSoftwareVertexProcessing", d_GetSoftwareVertexProcessing),
    S("SetNPatchMode", d_SetNPatchMode),
    S("GetNPatchMode", d_GetNPatchMode),
    S("DrawPrimitive", d_DrawPrimitive),
    S("DrawIndexedPrimitive", d_DrawIndexedPrimitive),
    S("DrawPrimitiveUP", d_DrawPrimitiveUP),
    S("DrawIndexedPrimitiveUP", d_DrawIndexedPrimitiveUP),
    S("ProcessVertices", d_ProcessVertices),
    S("CreateVertexDeclaration", d_CreateVertexDeclaration),
    S("SetVertexDeclaration", d_SetVertexDeclaration),
    S("GetVertexDeclaration", d_GetVertexDeclaration),
    S("SetFVF", d_SetFVF),
    S("GetFVF", d_GetFVF),
    S("CreateVertexShader", d_CreateVertexShader),
    S("SetVertexShader", d_SetVertexShader),
    S("GetVertexShader", d_GetVertexShader),
    S("SetVertexShaderConstantF", d_SetVertexShaderConstantF),
    S("GetVertexShaderConstantF", d_GetVertexShaderConstantF),
    S("SetVertexShaderConstantI", d_SetVertexShaderConstantI),
    S("GetVertexShaderConstantI", d_GetVertexShaderConstantI),
    S("SetVertexShaderConstantB", d_SetVertexShaderConstantB),
    S("GetVertexShaderConstantB", d_GetVertexShaderConstantB),
    S("SetStreamSource", d_SetStreamSource),
    S("GetStreamSource", d_GetStreamSource),
    S("SetStreamSourceFreq", d_SetStreamSourceFreq),
    S("GetStreamSourceFreq", d_GetStreamSourceFreq),
    S("SetIndices", d_SetIndices),
    S("GetIndices", d_GetIndices),
    S("CreatePixelShader", d_CreatePixelShader),
    S("SetPixelShader", d_SetPixelShader),
    S("GetPixelShader", d_GetPixelShader),
    S("SetPixelShaderConstantF", d_SetPixelShaderConstantF),
    S("GetPixelShaderConstantF", d_GetPixelShaderConstantF),
    S("SetPixelShaderConstantI", d_SetPixelShaderConstantI),
    S("GetPixelShaderConstantI", d_GetPixelShaderConstantI),
    S("SetPixelShaderConstantB", d_SetPixelShaderConstantB),
    S("GetPixelShaderConstantB", d_GetPixelShaderConstantB),
    S("DrawRectPatch", d_DrawRectPatch),
    S("DrawTriPatch", d_DrawTriPatch),
    S("DeletePatch", d_DeletePatch),
    S("CreateQuery", d_CreateQuery),
    { nullptr, nullptr }
};

static const Slot kSwapChain[] = {
    S("Present", s_Present),
    S("GetFrontBufferData", s_GetFrontBufferData),
    S("GetBackBuffer", s_GetBackBuffer),
    S("GetRasterStatus", s_GetRasterStatus),
    S("GetDisplayMode", s_GetDisplayMode),
    S("GetDevice", s_GetDevice),
    S("GetPresentParameters", s_GetPresentParameters),
    { nullptr, nullptr }
};

static const Slot kTexture[] = {
    S("GetLevelDesc", t_GetLevelDesc),
    S("GetSurfaceLevel", t_GetSurfaceLevel),
    S("LockRect", t_LockRect),
    S("UnlockRect", t_UnlockRect),
    S("AddDirtyRect", t_AddDirtyRect),
    { nullptr, nullptr }
};

static const Slot kSurface[] = {
    S("GetDesc", s_GetDesc),
    S("LockRect", s_LockRect),
    S("UnlockRect", s_UnlockRect),
    S("GetDC", s_GetDC),
    S("ReleaseDC", s_ReleaseDC),
    S("GetContainer", s_GetContainer),
    { nullptr, nullptr }
};

/* IDirect3DVertexBuffer9 / IDirect3DIndexBuffer9: IUnknown (3) +
 * IDirect3DResource9 (8: GetDevice, Set/GetPrivateData, FreePrivateData,
 * Set/GetPriority, PreLoad, GetType) = slot 11 belongs to the derived
 * interface, and the header's declaration order is Lock, Unlock, GetDesc.
 * With GetDesc first, the engine's Lock at [vtable+0x2C] (slot 11) landed on
 * b_GetDesc, which returned D3DERR_INVALIDCALL for the zero desc pointer,
 * so the engine's vertex-buffer fill took its release path and later wrote
 * through a NULL pointer (crash at guest VA 0). */
static const Slot kBuffer[] = {
    S("Lock", b_Lock),
    S("Unlock", b_Unlock),
    S("GetDesc", b_GetDesc),
    { nullptr, nullptr }
};

static const Slot kShader[] = {
    S("GetFunction", sh_GetFunction),
    S("GetDevice", sh_GetDevice),
    { nullptr, nullptr }
};

static const Slot kDecl[] = {
    S("GetDeclaration", vd_GetDeclaration),
    { nullptr, nullptr }
};

static const Slot kStateBlock[] = {
    S("Capture", sb_Capture),
    S("Apply", sb_Apply),
    { nullptr, nullptr }
};

static const Slot kQuery[] = {
    S("GetData", q_GetData),
    S("Issue", q_Issue),
    S("GetType", q_GetType),
    S("GetDataSize", q_GetDataSize),
    { nullptr, nullptr }
};

/* ID3DXConstantTable, in d3dx9shader.h order after IUnknown. */
static const Slot kConstTable[] = {
    S("GetDesc", ct_GetDesc),
    S("GetConstant", ct_GetConstant),
    S("GetConstantByName", ct_GetConstantByName),
    S("GetConstantElement", ct_GetConstantElement),
    S("GetConstantDesc", ct_GetConstantDesc),
    S("GetSamplerIndex", ct_GetSamplerIndex),
    S("SetDefaults", ct_SetDefaults),
    { nullptr, nullptr }
};

extern "C" {

static void d3d_Direct3DCreate9(void) {
    UINT sdk = A32(0);
    IDirect3D9 *d3d = Direct3DCreate9(sdk);
    ka_log("d3d9: Direct3DCreate9(sdk=%u) -> %p", sdk, (void *)d3d);
    if (!d3d) RET(0, 1);
    g_d3d_token = new_token(K_D3D9, d3d, 0);
    RET(g_d3d_token, 1);
}

const ka_shim ka_d3d9_shims[] = {
    { "Direct3DCreate9", d3d_Direct3DCreate9 },
    { NULL, NULL }
};

uint32_t shim_add_named(const char *name, recomp_func_t fn) {
    ka_shim one[2] = { { name, fn }, { NULL, NULL } };
    return ka_shim_register(one) ? ka_shim_va(name) : 0;
}

int ka_d3d9_init(void) {
    ka_shim_register(ka_d3d9_shims);
    build_vtable(K_D3D9, "d3d9", kBase, kD3D9);
    build_vtable(K_DEVICE, "dev", kBase, kDevice);
    build_vtable(K_SWAPCHAIN, "sc", kBase, kSwapChain);
    /* A texture's vtable is IUnknown + IDirect3DResource9 + IDirect3DBaseTexture9
     * (SetLOD, GetLOD, GetLevelCount..GenerateMipSubLevels = 6 entries) +
     * IDirect3DTexture9. DXVK's layout is the d3d9.h one, and the base/resource
     * slots are answered generically. */
    static const Slot texBase[] = {
        S("SetLOD", bt_SetLOD),
        S("GetLOD", bt_GetLOD),
        S("GetLevelCount", bt_GetLevelCount),
        S("SetAutoGenFilterType", bt_SetAutoGenFilterType),
        S("GetAutoGenFilterType", bt_GetAutoGenFilterType),
        S("GenerateMipSubLevels", bt_GenerateMipSubLevels),
        { nullptr, nullptr }
    };
    static const Slot texRest[] = {
        S("GetLevelDesc", t_GetLevelDesc), S("GetSurfaceLevel", t_GetSurfaceLevel),
        S("LockRect", t_LockRect), S("UnlockRect", t_UnlockRect),
        S("AddDirtyRect", t_AddDirtyRect), { nullptr, nullptr }
    };
    (void)texBase; (void)texRest;
    {
        /* IUnknown (3) + resource (8) + base texture (4) + texture (5) */
        static Slot all[24];
        int n = 0;
        for (const Slot *s = kBase; s->name; s++, n++) all[n] = *s;
        for (const Slot *s = kResource; s->name; s++, n++) all[n] = *s;
        for (const Slot *s = texBase; s->name; s++, n++) all[n] = *s;
        for (const Slot *s = texRest; s->name; s++, n++) all[n] = *s;
        all[n].name = nullptr;
        build_vtable(K_TEXTURE, "tex", nullptr, all);
    }
    {
        static Slot all[24];
        int n = 0;
        for (const Slot *s = kBase; s->name; s++, n++) all[n] = *s;
        for (const Slot *s = kResource; s->name; s++, n++) all[n] = *s;
        for (const Slot *s = kSurface; s->name; s++, n++) all[n] = *s;
        all[n].name = nullptr;
        build_vtable(K_SURFACE, "surf", nullptr, all);
    }
    {
        static Slot all[24];
        int n = 0;
        for (const Slot *s = kBase; s->name; s++, n++) all[n] = *s;
        for (const Slot *s = kResource; s->name; s++, n++) all[n] = *s;
        for (const Slot *s = kBuffer; s->name; s++, n++) all[n] = *s;
        all[n].name = nullptr;
        build_vtable(K_VBUFFER, "vb", nullptr, all);
        build_vtable(K_IBUFFER, "ib", nullptr, all);
    }
    {
        static Slot all[24];
        int n = 0;
        for (const Slot *s = kBase; s->name; s++, n++) all[n] = *s;
        for (const Slot *s = kResource; s->name; s++, n++) all[n] = *s;
        for (const Slot *s = kShader; s->name; s++, n++) all[n] = *s;
        all[n].name = nullptr;
        build_vtable(K_VS, "vs", nullptr, all);
        build_vtable(K_PS, "ps", nullptr, all);
    }
    {
        static Slot all[16];
        int n = 0;
        for (const Slot *s = kBase; s->name; s++, n++) all[n] = *s;
        for (const Slot *s = kResource; s->name; s++, n++) all[n] = *s;
        for (const Slot *s = kDecl; s->name; s++, n++) all[n] = *s;
        all[n].name = nullptr;
        build_vtable(K_VDECL, "vd", nullptr, all);
    }
    {
        static Slot all[8];
        int n = 0;
        for (const Slot *s = kBase; s->name; s++, n++) all[n] = *s;
        for (const Slot *s = kStateBlock; s->name; s++, n++) all[n] = *s;
        all[n].name = nullptr;
        build_vtable(K_STATEBLOCK, "sb", nullptr, all);
    }
    {
        static Slot all[12];
        int n = 0;
        for (const Slot *s = kBase; s->name; s++, n++) all[n] = *s;
        for (const Slot *s = kQuery; s->name; s++, n++) all[n] = *s;
        all[n].name = nullptr;
        build_vtable(K_QUERY, "q", nullptr, all);
    }
    {
        static Slot all[12];
        int n = 0;
        for (const Slot *s = kBase; s->name; s++, n++) all[n] = *s;
        for (const Slot *s = kConstTable; s->name; s++, n++) all[n] = *s;
        all[n].name = nullptr;
        build_vtable(K_D3DXCT, "ct", nullptr, all);
    }
    ka_log("d3d9: bridge ready (DXVK-native, Android WSI, %u shims)", ka_shim_count());
    return 0;
}

void ka_d3d9_window_changed(void) {
    /* Called by the shell each frame. The window itself is pushed by
     * jni_bridge through dxvkAndroidSetWindow when it changes. */
}

int ka_d3d9_frame_presented(void) { return 0; }
void ka_d3d9_shutdown(void) {}

/* --------------------------------------------------------------- D3DX9 */
/* CreateTexture: D3DXCreateTexture(dev, w, h, levels, usage, format, pool, out) */
int ka_d3dx_create_texture(void) {
    Obj *dev = obj_of(A32(0));
    IDirect3DTexture9 *t = nullptr;
    if (!dev || dev->kind != K_DEVICE) return (int)0x8876086Cu;
    HRESULT hr = reinterpret_cast<IDirect3DDevice9 *>(dev->p)->CreateTexture(
        A32(1), A32(2), A32(3), A32(4), (D3DFORMAT)A32(5), (D3DPOOL)A32(6), &t, nullptr);
    if (A32(7)) MEM32(A32(7)) = SUCCEEDED(hr) ? new_token(K_TEXTURE, t, A32(0)) : 0;
    return (int)hr;
}

/* Minimal DDS upload: header + top level (+ mips if the file has them). */
static int dds_upload(IDirect3DDevice9 *dev, const uint8_t *data, uint32_t size,
                      uint32_t width, uint32_t height, D3DFORMAT format, D3DPOOL pool,
                      IDirect3DTexture9 **out) {
    uint32_t w = width, h = height, offset = 128;
    const uint32_t *hdr = (const uint32_t *)(data + 4);
    const uint32_t *pf = hdr + 19;      /* DDS_PIXELFORMAT: flags, fourcc, bits, masks */
    IDirect3DTexture9 *t = nullptr, *staging = nullptr;
    HRESULT hr;
    /* Which branch refuses, and with what: the engine's D3DX call is the same
     * 15-argument shape on both platforms, so a refusal here has to be named
     * by our own shim's arguments, not guessed from the guest register dump. */
    if (size < 128 || memcmp(data, "DDS ", 4) != 0) {
        ka_log("d3dx: dds reject size=%u magic=%02X%02X%02X%02X", size,
               unsigned(size > 3 ? data[0] : 0), unsigned(size > 3 ? data[1] : 0),
               unsigned(size > 3 ? data[2] : 0), unsigned(size > 3 ? data[3] : 0));
        return (int)0x8876086Cu;
    }
    /* D3DX_DEFAULT (0xFFFFFFFF) means "take it from the file", and so does a
     * zero; the engine passes D3DX_DEFAULT for both axes and D3DFMT_UNKNOWN
     * for the format. Forwarding them verbatim made CreateTexture see
     * 4294967295x4294967295 with fmt=0 and answer D3DERR_INVALIDCALL - the
     * measured refusal behind the sub_0053C630 fault (3 textures per boot,
     * docs/FINDINGS.md 2026-11-21). */
    if (!w || w == 0xFFFFFFFFu) w = hdr[3];
    if (!h || h == 0xFFFFFFFFu) h = hdr[2];
    /* DDS_PIXELFORMAT dwords: 0 flags, 1 fourcc, 2 bit count, 3..5 RGB masks. */
    if ((uint32_t)format == 0) {
        if (pf[0] & 4u)
            format = (D3DFORMAT)pf[1];              /* DXT1/3/5 fourcc IS the D3DFORMAT */
        else if (pf[0] & 0x40u)
            format = pf[2] == 32u ? ((pf[0] & 1u) ? D3DFMT_A8R8G8B8 : D3DFMT_X8R8G8B8)
                   : pf[2] == 24u ? D3DFMT_R8G8B8 : D3DFMT_R5G6B5;
    }
    ka_log("d3dx: dds %ux%u mips=%u pf=%08X fourcc=%08X bits=%u -> w=%u h=%u fmt=%u pool=%u",
           hdr[3], hdr[2], hdr[6], pf[0], pf[1], pf[2], w, h, (unsigned)format, (unsigned)pool);
    if (!w || !h) return (int)0x8876086Cu;
    hr = dev->CreateTexture(w, h, 1, 0, format, pool, &t, nullptr);
    if (FAILED(hr)) {
        ka_log("d3dx: CreateTexture %ux%u fmt=%u pool=%u -> 0x%08X",
               w, h, (unsigned)format, (unsigned)pool, (unsigned)hr);
        return (int)hr;
    }
    /* A DEFAULT- or SCRATCH-pool texture cannot be locked. Fill a SYSTEMMEM
     * copy and let UpdateTexture move it, which is what D3DX itself does. */
    {
        IDirect3DTexture9 *dst = t;
        if (pool == D3DPOOL_DEFAULT || pool == D3DPOOL_SCRATCH) {
            hr = dev->CreateTexture(w, h, 1, 0, format, D3DPOOL_SYSTEMMEM, &staging, nullptr);
            if (FAILED(hr)) {
                ka_log("d3dx: staging CreateTexture -> 0x%08X", (unsigned)hr);
                t->Release();
                return (int)hr;
            }
            dst = staging;
        }
        D3DSURFACE_DESC desc{};
        D3DLOCKED_RECT lr{};
        dst->GetLevelDesc(0, &desc);
        hr = dst->LockRect(0, &lr, nullptr, 0);
        if (SUCCEEDED(hr)) {
            uint32_t bytes = lr.Pitch * desc.Height;
            uint32_t avail = size > offset ? size - offset : 0;
            memcpy(lr.pBits, data + offset, bytes < avail ? bytes : avail);
            dst->UnlockRect(0);
        } else {
            ka_log("d3dx: LockRect pool=%u -> 0x%08X", (unsigned)pool, (unsigned)hr);
        }
        if (staging) {
            if (SUCCEEDED(hr)) hr = dev->UpdateTexture(staging, t);
            staging->Release();
        }
    }
    if (FAILED(hr)) {
        ka_log("d3dx: upload %ux%u fmt=%u failed 0x%08X", w, h, (unsigned)format, (unsigned)hr);
        t->Release();
        return (int)hr;
    }
    *out = t;
    return (int)hr;
}

/* DDS cube upload: the same header walk as dds_upload, plus the six-face
 * payload. A DDS cube stores +X, -X, +Y, -Y, +Z, -Z back to back and each face
 * carries its own mip chain, so face n starts after every mip of face n-1.
 *
 * Why this exists (2026-11-22): the engine resolves its technique table from
 * pre_texture.wta/.wtp at boot, and one of those fifteen entries is a cube map.
 * With no cube entry point the shim answered D3DERR_INVALIDCALL, the table
 * build failed silently, [0xE4CCF4]/[0xE4CCF8] stayed {0,0}, the recorder
 * (sub_00550280 at 0x5502FF) then passed NULL where it wanted entry 11, and
 * the flush at sub_007492B0 faulted reading [NULL+4]. */
static uint32_t dds_mip_bytes(uint32_t w, uint32_t h, const uint32_t *pf) {
    if (pf[0] & 4u) {                    /* D3DDPF_FOURCC: block-compressed */
        uint32_t block = (pf[1] == 0x31545844u /* 'DXT1' */) ? 8u : 16u;
        return ((w + 3u) / 4u) * ((h + 3u) / 4u) * block;
    }
    {
        uint32_t bpp = pf[2] ? pf[2] : 32u;
        return (((w * bpp) + 31u) / 32u) * 4u * h;
    }
}

static int dds_upload_cube(IDirect3DDevice9 *dev, const uint8_t *data, uint32_t size,
                           uint32_t edge, uint32_t format, D3DPOOL pool,
                           IDirect3DCubeTexture9 **out) {
    const uint32_t *hdr = (const uint32_t *)(data + 4);
    const uint32_t *pf = hdr + 19;
    uint32_t w = edge, offset = 128, mips, face, level, avail, bytes;
    IDirect3DCubeTexture9 *t = nullptr, *staging = nullptr, *dst;
    HRESULT hr;

    if (size < 128 || memcmp(data, "DDS ", 4) != 0) {
        ka_log("d3dx: dds cube reject size=%u magic=%02X%02X%02X%02X", size,
               unsigned(size > 3 ? data[0] : 0), unsigned(size > 3 ? data[1] : 0),
               unsigned(size > 3 ? data[2] : 0), unsigned(size > 3 ? data[3] : 0));
        return (int)0x8876086Cu;
    }
    if (!w || w == 0xFFFFFFFFu) w = hdr[3];            /* Size = the header's width */
    if (format == 0) {
        if (pf[0] & 4u)
            format = pf[1];
        else if (pf[0] & 0x40u)
            format = pf[2] == 32u ? ((pf[0] & 1u) ? D3DFMT_A8R8G8B8 : D3DFMT_X8R8G8B8)
                   : pf[2] == 24u ? D3DFMT_R8G8B8 : D3DFMT_R5G6B5;
    }
    /* DDSCAPS2_CUBEMAP (0x200) is at header offset 112 -> dword 27. A 2D file
     * reaching the cube entry point is refused as D3DX does. */
    if (!w || !(hdr[27] & 0x200u)) {
        ka_log("d3dx: dds cube refuse w=%u caps2=%08X fmt=%u", w, hdr[27], format);
        return (int)0x8876086Cu;
    }
    mips = hdr[6] ? hdr[6] : 1u;
    ka_log("d3dx: dds cube %ux%u mips=%u pf=%08X fourcc=%08X bits=%u -> fmt=%u pool=%u",
           hdr[3], hdr[3], mips, pf[0], pf[1], pf[2], format, (unsigned)pool);

    /* Level count 1, matching dds_upload: only the top level is filled, and
     * creating the mips unfilled would hand the sampler garbage. */
    hr = dev->CreateCubeTexture(w, 1, 0, (D3DFORMAT)format, pool, &t, nullptr);
    if (FAILED(hr)) {
        ka_log("d3dx: CreateCubeTexture %ux%u fmt=%u pool=%u -> 0x%08X",
               w, w, format, (unsigned)pool, (unsigned)hr);
        return (int)hr;
    }
    dst = t;
    if (pool == D3DPOOL_DEFAULT || pool == D3DPOOL_SCRATCH) {
        hr = dev->CreateCubeTexture(w, 1, 0, (D3DFORMAT)format, D3DPOOL_SYSTEMMEM,
                                    &staging, nullptr);
        if (FAILED(hr)) {
            ka_log("d3dx: staging CreateCubeTexture -> 0x%08X", (unsigned)hr);
            t->Release();
            return (int)hr;
        }
        dst = staging;
    }
    for (face = 0; face < 6 && SUCCEEDED(hr); face++) {
        D3DSURFACE_DESC desc{};
        D3DLOCKED_RECT lr{};
        dst->GetLevelDesc(0, &desc);
        hr = dst->LockRect((D3DCUBEMAP_FACES)face, 0, &lr, nullptr, 0);
        if (FAILED(hr)) {
            ka_log("d3dx: cube LockRect face %u pool=%u -> 0x%08X",
                   face, (unsigned)pool, (unsigned)hr);
            break;
        }
        bytes = lr.Pitch * desc.Height;
        avail = size > offset ? size - offset : 0;
        memcpy(lr.pBits, data + offset, bytes < avail ? bytes : avail);
        dst->UnlockRect((D3DCUBEMAP_FACES)face, 0);
        for (level = 0; level < mips; level++) {
            uint32_t mw = w >> level, mh = w >> level;
            offset += dds_mip_bytes(mw ? mw : 1u, mh ? mh : 1u, pf);
        }
    }
    if (SUCCEEDED(hr) && staging) hr = dev->UpdateTexture(staging, t);
    if (staging) staging->Release();
    if (FAILED(hr)) {
        ka_log("d3dx: cube upload %ux%u fmt=%u failed 0x%08X",
               w, w, format, (unsigned)hr);
        t->Release();
        return (int)hr;
    }
    *out = t;
    return 0;
}

/* D3DXCreateTextureFromFileInMemoryEx:
 * device, src, size, w, h, levels, usage, format, pool, filter, mipfilter,
 * colorkey, srcinfo, palette, out */
int ka_d3dx_texture_from_memory(void) {
    Obj *dev = obj_of(A32(0));
    IDirect3DTexture9 *t = nullptr;
    int hr;
    if (!dev || dev->kind != K_DEVICE) return (int)0x8876086Cu;
    hr = dds_upload(reinterpret_cast<IDirect3DDevice9 *>(dev->p),
                    (const uint8_t *)P(A32(1)), A32(2), A32(3), A32(4),
                    (D3DFORMAT)A32(7), (D3DPOOL)A32(8), &t);
    if (FAILED(hr)) ka_log("d3dx: TextureFromFileInMemoryEx failed 0x%08X (size %u)",
                           (unsigned)hr, A32(2));
    if (A32(14)) MEM32(A32(14)) = SUCCEEDED(hr) && t ? new_token(K_TEXTURE, t, A32(0)) : 0;
    return hr;
}

int ka_d3dx_cube_from_memory(void) {
    Obj *dev = obj_of(A32(0));
    IDirect3DCubeTexture9 *t = nullptr;
    int hr;
    if (!dev || dev->kind != K_DEVICE) return (int)0x8876086Cu;
    hr = dds_upload_cube(reinterpret_cast<IDirect3DDevice9 *>(dev->p),
                         (const uint8_t *)P(A32(1)), A32(2), A32(3),
                         (uint32_t)A32(6), (D3DPOOL)A32(7), &t);
    if (FAILED(hr))
        ka_log("d3dx: CubeTextureFromFileInMemoryEx failed 0x%08X (size %u)",
               (unsigned)hr, A32(2));
    /* ppCubeTexture is the last argument and the 2D entry point's minus one:
     * this signature has no height. Returning the token in EAX instead (the
     * mistake that cost the DirectInput shim a run) leaves the engine's slot
     * at 0 while the HRESULT says S_OK. */
    if (A32(13)) MEM32(A32(13)) = SUCCEEDED(hr) && t ? new_token(K_CUBETEX, t, A32(0)) : 0;
    return hr;
}

int ka_d3dx_constant_table(void) {
    const uint8_t *code = (const uint8_t *)P(A32(0));
    DxConstTable *t;
    if (!code) {
        if (A32(1)) MEM32(A32(1)) = 0;
        return (int)0x8876086Cu;                 /* D3DERR_INVALIDCALL */
    }
    t = dx_parse_ctab(code);
    if (!t) {
        ka_log("d3dx: D3DXGetShaderConstantTable: no CTAB in bytecode %p (%u tokens)",
               (const void *)code, dx_scanned);
        if (A32(1)) MEM32(A32(1)) = 0;
        return (int)0x8876086Cu;
    }
    if (A32(1)) MEM32(A32(1)) = new_token(K_D3DXCT, t, 0);
    return 0;
}

} /* extern "C" */



#endif /* KORRA_WITH_DXVK */


