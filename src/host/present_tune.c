/*
 * present_tune.c - read the D3D9 present path, and rewrite it when asked.
 *
 * The first-area report is "3 fps / 298 ms", and perf_probe.c shows why it is
 * not the frame limiter: the limiter interval is 50 (60 fps, DAT_00e4d210 = 0)
 * and the steady frame is 35-37 ms, while single IDirect3DDevice9::Present
 * calls intermittently block for 100-570 ms. On a 1280x720 backbuffer with an
 * idle GPU that is a present-path question, not a hardware one, so this file
 * answers it directly:
 *
 *   KORRA_PRESENT=1           log every CreateDevice/Reset parameter block
 *   KORRA_PRESENT=windowed    ... and make it a window (no exclusive mode)
 *   KORRA_PRESENT=immediate   ... and Present with no vsync wait
 *   KORRA_PRESENT=nolockable  ... and clear D3DPRESENTFLAG_LOCKABLE_BACKBUFFER
 *   KORRA_PRESENT=nomsaa      ... and turn multisampling off
 *
 * The options combine: KORRA_PRESENT=windowed,immediate,nomsaa.
 *
 * COM methods have no name in the runtime's table (only bridged *imports* do),
 * so this cannot match "d3d9.dll!CreateDevice" by name: quirk_probe.c tried
 * exactly that and never fired, which is why the game has been stuck in
 * exclusive fullscreen and a second instance cannot create a device at all.
 * The call is identified structurally instead - the argument slots of
 * IDirect3D9::CreateDevice and a sanity check on the parameter block - the same
 * way draw_probe.c finds the device.
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "native32.h"

/* D3DPRESENT_PARAMETERS, 0x38 bytes (the size the engine memsets in
 * FUN_00758e40): */
#define D3DPP_WIDTH        0x00u
#define D3DPP_HEIGHT       0x04u
#define D3DPP_BBFORMAT     0x08u
#define D3DPP_BBCOUNT      0x0Cu
#define D3DPP_MSAATYPE     0x10u
#define D3DPP_MSAAQUAL     0x14u
#define D3DPP_SWAPEFFECT   0x18u
#define D3DPP_HWND         0x1Cu
#define D3DPP_WINDOWED     0x20u
#define D3DPP_AUTODEPTH    0x24u
#define D3DPP_AFORMAT      0x28u
#define D3DPP_FLAGS        0x2Cu
#define D3DPP_REFRESH      0x30u
#define D3DPP_INTERVAL     0x34u
#define D3DPP_SIZE         0x38u

#define D3DPRESENTFLAG_LOCKABLE_BACKBUFFER 0x00000001u
#define D3DPRESENT_INTERVAL_IMMEDIATE     0x80000000u

typedef struct { int log, windowed, immediate, nolockable, nomsaa; } opts_t;
static opts_t g_o;
static int    g_seen;    /* parameter blocks logged */
static native32_pre_hook_t g_prev;

static const uint32_t k_formats[] = {
    0u,        /* D3DFMT_UNKNOWN                 */
    21u, 22u, 23u, 24u, 25u,           /* A8R8G8B8, X8R8G8B8, R5G6B5, X1R5G5B5, A1R5G5B5 */
    32u, 33u, 34u, 35u, 36u,           /* A8B8G8R8, X8B8G8R8 (+unused)             */
    50u, 51u,                          /* D16, D24X8                                */
    75u, 76u, 77u, 78u, 80u, 81u,      /* D24S8, D32, D15S1, D24X4S4, D16_LOCKABLE, D32F_LOCKABLE */
    82u                                /* D24FS8                                   */
};

static int plausible_params(uint32_t p) {
    MEMORY_BASIC_INFORMATION mbi;
    const uint32_t *v;
    int i, fmt_ok = 0;

    if (!p) return 0;
    if (!VirtualQuery((const void *)(uintptr_t)p, &mbi, sizeof mbi)) return 0;
    if (mbi.State != MEM_COMMIT) return 0;
    if ((DWORD)mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return 0;
    if ((size_t)((const char *)mbi.BaseAddress + mbi.RegionSize - (const char *)(uintptr_t)p) < D3DPP_SIZE)
        return 0;

    v = (const uint32_t *)(uintptr_t)p;
    if (v[D3DPP_WINDOWED / 4] > 1u) return 0;
    if (v[D3DPP_SWAPEFFECT / 4] > 1u) return 0;             /* DISCARD/COPY only */
    if (v[D3DPP_BBCOUNT / 4] > 3u) return 0;
    if (v[D3DPP_AUTODEPTH / 4] > 1u) return 0;
    if (v[D3DPP_FLAGS / 4] & ~0x3u) return 0;               /* LOCKABLE | DISCARD_DEPTH */
    if (v[D3DPP_WIDTH / 4] < 320u || v[D3DPP_WIDTH / 4] > 8192u) return 0;
    if (v[D3DPP_HEIGHT / 4] < 200u || v[D3DPP_HEIGHT / 4] > 8192u) return 0;
    switch (v[D3DPP_MSAATYPE / 4]) {
    case 0u: case 2u: case 4u: case 8u: case 16u: break;
    default: return 0;
    }
    if (v[D3DPP_MSAAQUAL / 4] > 16u) return 0;
    switch (v[D3DPP_INTERVAL / 4]) {
    case 0u: case 1u: case 2u: case 4u: case 0x80000000u: break;
    default: return 0;
    }
    if (v[D3DPP_REFRESH / 4] != 0u && (v[D3DPP_REFRESH / 4] < 50u || v[D3DPP_REFRESH / 4] > 240u))
        return 0;
    if (v[D3DPP_AFORMAT / 4] != 0u) {
        static const uint32_t k_depths[] = { 50u, 51u, 75u, 76u, 77u, 78u, 80u, 81u, 82u };
        int d_ok = 0;
        for (i = 0; i < (int)(sizeof k_depths / sizeof k_depths[0]); i++)
            if (v[D3DPP_AFORMAT / 4] == k_depths[i]) { d_ok = 1; break; }
        if (!d_ok) return 0;
    }
    for (i = 0; i < (int)(sizeof k_formats / sizeof k_formats[0]); i++)
        if (v[D3DPP_BBFORMAT / 4] == k_formats[i]) { fmt_ok = 1; break; }
    return fmt_ok;
}

/* IDirect3D9::CreateDevice(Adapter, DeviceType, hFocus, BehaviorFlags,
 *                          pPresentationParameters, ppDevice)
 *   args[0] = this, args[4] = BehaviorFlags, args[5] = parameters.
 * BehaviorFlags carries the vertex-processing choice: the engine passes 0x44
 * then 0x24 (HAL, then REF) per docs/quirks.md, so those two exact values are
 * the match. Anything looser than that misfires on unrelated COM calls - a
 * first, loose version of this rewrote a guest structure whose slots merely
 * looked like a present block and crashed the boot (argument slot 4 read as
 * BehaviorFlags, 0xDEAD0000 - the runtime's dummy return address - as the
 * present interval). */
static int is_create_device(const uint32_t *args) {
    if (args[4] != 0x44u && args[4] != 0x24u) return 0;
    if (args[2] != 1u && args[2] != 2u) return 0;   /* D3DDEVTYPE HAL / REF */
    return plausible_params(args[5]);
}

static void tune_params(uint32_t p) {
    uint32_t *v = (uint32_t *)(uintptr_t)p;
    if (g_o.windowed) {
        v[D3DPP_WINDOWED / 4] = 1;
        v[D3DPP_REFRESH / 4] = 0;
    }
    if (g_o.immediate) v[D3DPP_INTERVAL / 4] = D3DPRESENT_INTERVAL_IMMEDIATE;
    if (g_o.nolockable) v[D3DPP_FLAGS / 4] &= ~D3DPRESENTFLAG_LOCKABLE_BACKBUFFER;
    if (g_o.nomsaa) {
        v[D3DPP_MSAATYPE / 4] = 0;
        v[D3DPP_MSAAQUAL / 4] = 0;
    }
}

static void log_params(const char *what, uint32_t p) {
    const uint32_t *v = (const uint32_t *)(uintptr_t)p;
    fprintf(stderr,
            "[present] %s %ux%u fmt=%u bb=%u msaa=%ux%u swap=%u windowed=%u "
            "autodepth=%u afmt=%u flags=0x%X refresh=%u interval=0x%X\n",
            what,
            v[D3DPP_WIDTH / 4], v[D3DPP_HEIGHT / 4], v[D3DPP_BBFORMAT / 4],
            v[D3DPP_BBCOUNT / 4], v[D3DPP_MSAATYPE / 4], v[D3DPP_MSAAQUAL / 4],
            v[D3DPP_SWAPEFFECT / 4], v[D3DPP_WINDOWED / 4],
            v[D3DPP_AUTODEPTH / 4], v[D3DPP_AFORMAT / 4],
            v[D3DPP_FLAGS / 4], v[D3DPP_REFRESH / 4], v[D3DPP_INTERVAL / 4]);
    fflush(stderr);
}

static void tune_pre(uint32_t target, uint32_t *args) {
    if (g_prev) g_prev(target, args);
    if (!g_o.log) return;
    /* Only the first match is acted on: the engine creates one device, and a
     * later false positive is exactly what corrupted a boot's data. */
    if (g_seen > 0) return;
    if (!is_create_device(args)) return;
    log_params("CreateDevice", args[5]);
    g_seen++;
    tune_params(args[5]);
    log_params("  applied  ", args[5]);
}

static void __cdecl present_tune_ctor(void) {
    const char *s = getenv("KORRA_PRESENT");
    char buf[160];
    if (!s) return;
    _snprintf(buf, sizeof buf, ",%s,", s);
    buf[sizeof buf - 1] = 0;
    g_o.log = 1;
    g_o.windowed = strstr(buf, ",windowed,") != NULL;
    g_o.immediate = strstr(buf, ",immediate,") != NULL;
    g_o.nolockable = strstr(buf, ",nolockable,") != NULL;
    g_o.nomsaa = strstr(buf, ",nomsaa,") != NULL;

    if (native32_pre_hook && native32_pre_hook != tune_pre) g_prev = native32_pre_hook;
    native32_pre_hook = tune_pre;
    fprintf(stderr,
            "[present] present tuning on (log=1 windowed=%d immediate=%d "
            "nolockable=%d nomsaa=%d)\n",
            g_o.windowed, g_o.immediate, g_o.nolockable, g_o.nomsaa);
    fflush(stderr);
}

/* A CRT initializer: no edit to main.c, no thread (an initializer that calls
 * CreateThread faults before main - AGENTS.md ledger). */
#pragma section(".CRT$XIU", read)
__declspec(allocate(".CRT$XIU")) static void (__cdecl *g_present_ctor)(void) = present_tune_ctor;
