/* user32_android.c - the window/message core the engine and D3D9 need.
 *
 * There is no real window: the shell owns the ANativeWindow. An HWND here is a
 * token (0x7001...) that keeps the guest's 32-bit fields valid, and the
 * client/window rects report the render size the engine asks for (1280x720 on
 * this title). Window procedures are kept in a table so SendMessageA and
 * DispatchMessageA can enter them with hle_call_guest - the WndProc is lifted
 * guest code, not host code.
 *
 * The message queue is mostly empty on purpose: input reaches the game through
 * XInput (the engine polls it), not through WM_ messages. PeekMessageA therefore
 * answers "nothing" unless a quit was posted.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "host_android.h"

#define MAX_WINDOWS 8
typedef struct {
    int used;
    uint32_t hwnd, proc, w, h, style;
} ka_win;
static ka_win g_win[MAX_WINDOWS];
static uint32_t g_last_hwnd;
static int g_quit;

static ka_win *win_of(uint32_t h) {
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (g_win[i].used && g_win[i].hwnd == h) return &g_win[i];
    return NULL;
}

static void u_UnregisterClassA(void) { RET(1, 2); }

static void u_CreateWindowExA(void) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!g_win[i].used) {
            g_win[i].used = 1;
            g_win[i].hwnd = 0x7001u + (uint32_t)i;
            g_win[i].w = 1280;
            g_win[i].h = 720;
            g_win[i].style = A32(3);
            g_last_hwnd = g_win[i].hwnd;
            RET(g_win[i].hwnd, 12);
        }
    }
    RET(0, 12);
}

/* The engine registers its WndProc with RegisterClassExA before creating the
 * window; remember it so DispatchMessageA works without a real class atom. */
static uint32_t g_wndproc;
void ka_user32_set_wndproc(uint32_t proc) { g_wndproc = proc; }
uint32_t ka_user32_wndproc(void) { return g_wndproc; }

static void u_DestroyWindow(void) {
    ka_win *w = win_of(A32(0));
    if (w) w->used = 0;
    RET(1, 1);
}
static void u_DefWindowProcA(void) { RET(0, 4); }
static void u_CallWindowProcW(void) {
    uint32_t args[4] = { A32(1), A32(2), A32(3), A32(4) };
    RET(hle_call_guest(A32(0), 4, args), 5);
}
static void u_DispatchMessageA(void) {
    uint32_t msg = A32(0), proc = g_wndproc;
    if (!msg || !proc) RET(0, 1);
    {
        uint32_t args[4] = { MEM32(msg + 0), MEM32(msg + 4), MEM32(msg + 8), MEM32(msg + 12) };
        RET(hle_call_guest(proc, 4, args), 1);
    }
}
static void u_TranslateMessage(void) { RET(0, 1); }
static void u_PeekMessageA(void) {
    uint32_t out = A32(0);
    if (g_quit) {
        g_quit = 0;
        if (out) { memset(ka_host(out), 0, 28); MEM32(out + 4) = 0x12; }  /* WM_QUIT */
        RET(1, 5);
    }
    RET(0, 5);
}
static void u_PostQuitMessage(void) { g_quit = 1; RETV(1); }
static void u_SendMessageA(void) {
    uint32_t proc = g_wndproc;
    uint32_t args[4] = { A32(0), A32(1), A32(2), A32(3) };
    if (!proc) RET(0, 4);
    RET(hle_call_guest(proc, 4, args), 4);
}
static void u_GetClientRect(void) {
    uint32_t r = A32(1);
    ka_win *w = win_of(A32(0));
    if (r) {
        MEM32(r + 0) = 0; MEM32(r + 4) = 0;
        MEM32(r + 8) = w ? w->w : 1280;
        MEM32(r + 12) = w ? w->h : 720;
    }
    RET(1, 2);
}
static void u_GetWindowRect(void) {
    uint32_t r = A32(1);
    ka_win *w = win_of(A32(0));
    if (r) {
        MEM32(r + 0) = 0; MEM32(r + 4) = 0;
        MEM32(r + 8) = w ? w->w : 1280;
        MEM32(r + 12) = w ? w->h : 720;
    }
    RET(1, 2);
}
static void u_AdjustWindowRect(void) { RET(1, 3); }
static void u_GetSystemMetrics(void) {
    switch (A32(0)) {
    case 0: RET(1280, 1);     /* SM_CXSCREEN */
    case 1: RET(720, 1);      /* SM_CYSCREEN */
    case 4: case 5: RET(0, 1);
    case 32: case 33: RET(0, 1);
    default: RET(0, 1);
    }
}
static void u_GetCursorPos(void) {
    if (A32(0)) { MEM32(A32(0)) = 640; MEM32(A32(0) + 4) = 360; }
    RET(1, 1);
}
static void u_SetCursorPos(void) { RET(1, 2); }
static void u_ClipCursor(void) { RET(1, 1); }
static void u_ScreenToClient(void) { RET(1, 2); }
static void u_ClientToScreen(void) { RET(1, 2); }
static void u_GetDesktopWindow(void) { RET(0x7000, 0); }
static void u_GetForegroundWindow(void) { RET(g_last_hwnd ? g_last_hwnd : 0x7000, 0); }
static void u_ShowWindow(void) { RET(1, 2); }
static void u_SetWindowPos(void) { RET(1, 7); }
static void u_GetWindowLongA(void) {
    ka_win *w = win_of(A32(0));
    RET(w ? w->style : 0, 2);
}
static void u_SetWindowLongA(void) {
    ka_win *w = win_of(A32(0));
    uint32_t old = w ? w->style : 0;
    if (w) w->style = A32(2);
    RET(old, 3);
}
/* UTF-16 -> UTF-8 for log lines. Windows only passes BMP text in this title,
 * but surrogate pairs are cheap to handle correctly and a half-handed string
 * would be the one that matters. */
static void w2u8(const uint16_t *src, char *dst, size_t cap) {
    size_t o = 0;
    if (!dst || cap == 0) return;
    dst[0] = 0;
    if (!src) return;
    while (*src && o + 4 < cap) {
        uint32_t c = *src++;
        if (c >= 0xD800 && c <= 0xDBFF && *src >= 0xDC00 && *src <= 0xDFFF) {
            c = 0x10000 + ((c - 0xD800) << 10) + (*src++ - 0xDC00);
        }
        if (c < 0x80) {
            dst[o++] = (char)c;
        } else if (c < 0x800) {
            dst[o++] = (char)(0xC0 | (c >> 6));
            dst[o++] = (char)(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            dst[o++] = (char)(0xE0 | (c >> 12));
            dst[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
            dst[o++] = (char)(0x80 | (c & 0x3F));
        } else {
            dst[o++] = (char)(0xF0 | (c >> 18));
            dst[o++] = (char)(0x80 | ((c >> 12) & 0x3F));
            dst[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
            dst[o++] = (char)(0x80 | (c & 0x3F));
        }
    }
    dst[o] = 0;
}

static void u_MessageBoxA(void) {
    const char *text = ASTR(1), *cap = ASTR(2);
    ka_log("[msgbox] %s: %s", cap ? cap : "", text ? text : "");
    RET(1, 4);
}
static void u_MessageBoxW(void) {
    char text[1024], cap[256];
    w2u8(AWSTR(1), text, sizeof text);
    w2u8(AWSTR(2), cap, sizeof cap);
    ka_log("[msgbox] %s: %s", cap, text);
    RET(1, 4);
}
static void u_ShowCursor(void) { RET(1, 1); }
static void u_SwapMouseButton(void) { RET(0, 1); }
static void u_LoadCursorA(void) { RET(0x7100, 2); }
static void u_LoadIconA(void) { RET(0x7101, 2); }
static void u_UpdateWindow(void) { RET(1, 1); }
static void u_RegisterDeviceNotificationW(void) { RET(0, 3); }
static void u_UnregisterDeviceNotificationW(void) { RET(1, 1); }
static void u_UnregisterDeviceNotification(void) { RET(1, 1); }
static void u_GetDC(void) { RET(0x7200, 1); }
static void u_BeginPaint(void) {
    uint32_t ps = A32(1);
    if (ps) {
        memset(ka_host(ps), 0, 64);
        MEM32(ps + 0) = 0x7200;   /* hdc */
    }
    RET(0x7200, 2);
}
static void u_EndPaint(void) { RET(1, 2); }
/* A short, honest mode list: the surface we render into, and nothing else.
 * Windows answers FALSE once iModeNum is past the last mode, which is how a
 * caller's enumeration loop ends - answering TRUE for every index (as this
 * did) traps the engine in `EnumDisplaySettingsA` forever (found on device,
 * docs/FINDINGS.md). ENUM_CURRENT_SETTINGS (0xFFFFFFFF) is mode 0 here. */
static void u_EnumDisplaySettingsA(void) {
    uint32_t index = A32(1);
    uint32_t dm = A32(2);
    static int logged;
    if (!logged) {
        ka_log("display: EnumDisplaySettingsA(iModeNum=%u) -> 1280x720x32@60", index);
        logged = 1;
    }
    if (!dm || (index != 0xFFFFFFFFu && index != 0)) RET(0, 3);
    memset(ka_host(dm), 0, 156);
    MEM32(dm + 36) = 156;        /* dmSize */
    MEM32(dm + 40) = 0x400000;   /* dmFields: PELSWIDTH|PELSHEIGHT|BPP|FREQ */
    MEM32(dm + 104) = 32;        /* dmBitsPerPel */
    MEM32(dm + 108) = 1280;      /* dmPelsWidth */
    MEM32(dm + 112) = 720;       /* dmPelsHeight */
    MEM32(dm + 120) = 60;        /* dmDisplayFrequency */
    RET(1, 3);
}

/* RegisterClassExA keeps the WndProc from WNDCLASSEXA.lpfnWndProc: cbSize 0,
 * style 4, lpfnWndProc 8. (Reading +12 got cbClsExtra, so no WndProc was ever
 * registered and DispatchMessageA/SendMessageA went nowhere.) */
static void u_RegClassReal(void) {
    uint32_t cls = A32(0);
    if (cls) g_wndproc = MEM32(cls + 8);
    RET(1, 1);
}

const ka_shim ka_user32_shims[] = {
    { "RegisterClassExA", u_RegClassReal },
    { "UnregisterClassA", u_UnregisterClassA },
    { "CreateWindowExA", u_CreateWindowExA },
    { "DestroyWindow", u_DestroyWindow },
    { "DefWindowProcA", u_DefWindowProcA },
    { "CallWindowProcW", u_CallWindowProcW },
    { "DispatchMessageA", u_DispatchMessageA },
    { "TranslateMessage", u_TranslateMessage },
    { "PeekMessageA", u_PeekMessageA },
    { "PostQuitMessage", u_PostQuitMessage },
    { "SendMessageA", u_SendMessageA },
    { "GetClientRect", u_GetClientRect },
    { "GetWindowRect", u_GetWindowRect },
    { "AdjustWindowRect", u_AdjustWindowRect },
    { "GetSystemMetrics", u_GetSystemMetrics },
    { "GetCursorPos", u_GetCursorPos },
    { "SetCursorPos", u_SetCursorPos },
    { "ClipCursor", u_ClipCursor },
    { "ScreenToClient", u_ScreenToClient },
    { "ClientToScreen", u_ClientToScreen },
    { "GetDesktopWindow", u_GetDesktopWindow },
    { "GetForegroundWindow", u_GetForegroundWindow },
    { "ShowWindow", u_ShowWindow },
    { "SetWindowPos", u_SetWindowPos },
    { "GetWindowLongA", u_GetWindowLongA },
    { "GetWindowLongW", u_GetWindowLongA },
    { "SetWindowLongA", u_SetWindowLongA },
    { "SetWindowLongW", u_SetWindowLongA },
    { "MessageBoxA", u_MessageBoxA },
    { "MessageBoxW", u_MessageBoxW },
    { "ShowCursor", u_ShowCursor },
    { "SwapMouseButton", u_SwapMouseButton },
    { "LoadCursorA", u_LoadCursorA },
    { "LoadIconA", u_LoadIconA },
    { "UpdateWindow", u_UpdateWindow },
    { "RegisterDeviceNotificationW", u_RegisterDeviceNotificationW },
    { "UnregisterDeviceNotificationW", u_UnregisterDeviceNotificationW },
    { "UnregisterDeviceNotification", u_UnregisterDeviceNotification },
    { "GetDC", u_GetDC },
    { "BeginPaint", u_BeginPaint },
    { "EndPaint", u_EndPaint },
    { "EnumDisplaySettingsA", u_EnumDisplaySettingsA },
    { NULL, NULL }
};
