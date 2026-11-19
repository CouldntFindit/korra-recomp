/*
 * xinput_stub.c - a synthetic Xbox 360 pad for the Windows host.
 *
 * The engine polls four pads through XInputGetState (guest FUN_007606d0); on a
 * desktop with no controller every call returns ERROR_DEVICE_NOT_CONNECTED, so
 * the title screen's "press start" never happens and no menu can be walked.
 * This shim answers pad 0 as connected and driven by the environment, leaving
 * pads 1-3 disconnected:
 *
 *   KORRA_PAD=1            pad 0 connected, no buttons (sticks centred)
 *   KORRA_PAD_BITS=0x1000  hold those buttons (XINPUT_GAMEPAD_* bits)
 *   KORRA_PAD_AUTO=1       tap A for ~120 ms every 1.2 s, START every 6 s, so a
 *                          run can be driven to the menu and beyond untouched
 *
 * The state struct is the standard XINPUT_STATE (dwPacketNumber + GAMEPAD);
 * the engine's own remap into its 0x24-byte block happens in guest code, so
 * nothing here knows the engine layout. Both imports are by ordinal
 * (XINPUT1_3.dll ordinals 2 and 3), which is why the shim names carry the
 * ordinal: see the shim-by-ordinal match in native32_bind.
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "native32.h"

#define PAD_A      0x1000u
#define PAD_B      0x2000u
#define PAD_X      0x4000u
#define PAD_Y      0x8000u
#define PAD_START  0x0010u
#define PAD_BACK   0x0020u

#define XINPUT_DEVICE_NOT_CONNECTED 1167u

typedef struct {
    uint32_t dwPacketNumber;
    uint16_t wButtons;
    uint8_t  bLeftTrigger, bRightTrigger;
    int16_t  sThumbLX, sThumbLY, sThumbRX, sThumbRY;
} xinput_state;

static int pad_enabled(void) {
    static int cached = -1;
    if (cached < 0) cached = getenv("KORRA_PAD") != NULL;
    return cached;
}

static int pad_auto(void) {
    static int cached = -1;
    if (cached < 0) cached = getenv("KORRA_PAD_AUTO") != NULL;
    return cached;
}

static unsigned pad_held_bits(void) {
    static unsigned cached = 0;
    static int loaded;
    if (!loaded) {
        const char *s = getenv("KORRA_PAD_BITS");
        loaded = 1;
        if (s) cached = (unsigned)strtoul(s, NULL, 0);
    }
    return cached;
}

/* KORRA_PAD_TAP=<ms>[,<ms>...] presses A once for 150 ms at each time: a
 * single controlled press, so a run can leave the "press start" prompt and stop
 * at the main menu instead of being walked through it by repeating taps. */
static uint16_t pad_single_taps(uint32_t ms) {
    static uint32_t taps[8];
    static int n = -1;
    if (n < 0) {
        const char *s = getenv("KORRA_PAD_TAP");
        n = 0;
        while (s && *s && n < 8) {
            taps[n++] = (uint32_t)strtoul(s, (char **)&s, 0);
            while (*s == ',' || *s == ' ') s++;
        }
    }
    for (int i = 0; i < n; i++)
        if (ms >= taps[i] && ms < taps[i] + 150) return PAD_A;
    return 0;
}

/* Scripted taps: a fixed timeline, so a run is repeatable. A frame is ~16 ms,
 * and the engine reads the pad once per frame. The timeline starts well past
 * the intro movies (they take ~40 s on this machine), because a tap consumed by
 * a movie is a tap the title screen never sees. */
static uint16_t pad_scripted_buttons(uint32_t ms) {
    if (ms < 40000) return 0;
    /* One short A tap every 1.5 s after the title screen, so "press start",
     * then the highlighted menu entry, then any confirm prompt are all
     * answered without knowing the menu layout. */
    if ((ms - 40000) % 1500 < 120) return PAD_A;
    if ((ms - 40000) % 9000 > 4000 && (ms - 40000) % 9000 < 4120) return PAD_START;
    return 0;
}

static uint32_t pad_uptime_ms(void) {
    static LONGLONG t0;
    LARGE_INTEGER f, t;
    if (!t0) {
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&t);
        t0 = t.QuadPart ? t.QuadPart : 1;
        return 0;
    }
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&f);
    return (uint32_t)((t.QuadPart - t0) * 1000 / (f.QuadPart ? f.QuadPart : 1));
}

/* The shim convention: arguments at g_esp+4, result in g_eax, and the shim
 * removes exactly what the real stdcall function would remove (ret + args).
 * native32 measures the purge against the shim, so both counts must be right. */

/* XInputGetState(DWORD dwUserIndex, XINPUT_STATE* pState) - 2 args, 8 bytes. */
static void shim_XInputGetState(void) {
    uint32_t *a = (uint32_t *)(uintptr_t)(g_esp + 4);
    DWORD index = a[0];
    xinput_state *state = (xinput_state *)(uintptr_t)a[1];
    uint16_t bits;
    MEMORY_BASIC_INFORMATION mbi;

    if (!pad_enabled() || index != 0 || !state ||
        !VirtualQuery(state, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT ||
        (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) ||
        (size_t)((const char *)mbi.BaseAddress + mbi.RegionSize - (const char *)state) < sizeof *state) {
        g_eax = XINPUT_DEVICE_NOT_CONNECTED;
    } else {
        bits = (uint16_t)pad_held_bits();
        if (pad_auto()) bits |= pad_scripted_buttons(pad_uptime_ms());
        bits |= pad_single_taps(pad_uptime_ms());
        state->dwPacketNumber++;
        state->wButtons = bits;
        state->bLeftTrigger = state->bRightTrigger = 0;
        state->sThumbLX = state->sThumbLY = state->sThumbRX = state->sThumbRY = 0;
        g_eax = 0;   /* ERROR_SUCCESS */
    }
    g_esp += 4 + 2 * 4;
}

/* XInputSetState(DWORD dwUserIndex, XINPUT_VIBRATION* pVibration) - 2 args. */
static void shim_XInputSetState(void) {
    uint32_t *a = (uint32_t *)(uintptr_t)(g_esp + 4);
    (void)a;
    g_eax = (pad_enabled() && a[0] == 0) ? 0u : XINPUT_DEVICE_NOT_CONNECTED;
    g_esp += 4 + 2 * 4;
}

static const native32_shim_t g_xinput_shims[] = {
    { "XINPUT1_3.dll!#2", shim_XInputGetState },
    { "XINPUT1_3.dll!#3", shim_XInputSetState },
    { "#2", shim_XInputGetState },     /* any other XInput build's ordinal 2 */
    { "#3", shim_XInputSetState },
};

const native32_shim_t *xinput_stub_shims(int *count) {
    if (count) *count = (int)(sizeof g_xinput_shims / sizeof g_xinput_shims[0]);
    return g_xinput_shims;
}
