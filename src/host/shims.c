/*
 * shims.c - the host's import-shim table.
 *
 * native32 routes every import to the real Windows API (the host is a 32-bit
 * process and the guest image is mapped at its own base, so a Win32 struct the
 * guest builds is already what Windows expects). Three things Windows cannot
 * answer for us are collected here:
 *
 *   steam_api.dll     no Steam client behind this build: seven functions,
 *                     answered by steam_stub.c
 *   XAudio2           no audio device (M2 is silent): CoCreateInstance with
 *                     CLSID_XAudio2 gets an in-process IXAudio2 2.7 from
 *                     xaudio2_stub.c; every other CLSID still goes to ole32
 *   GetProcAddress    must not hand out the raw address of a shimmed function,
 *                     or a module that resolves steam_api at run time bypasses
 *                     the shim and lands in a broken steam_api.dll
 *
 * A shim runs with the guest's registers live: arguments at g_esp+4 and the
 * shim removes exactly what the real function would have removed (stdcall
 * callee pops, cdecl caller pops). The per-function evidence for those counts
 * is in docs/xaudio2_callsites.md.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "native32.h"
#include "steam_stub.h"
#include "xaudio2_stub.h"

#define ARGS ((uint32_t *)(uintptr_t)(g_esp + 4))

/* Where the guest thinks it is. The engine derives its data directory from
 * GetModuleFileNameA(NULL) and opens <dir>\GameData\*.cpk from there; without
 * this it gets the host exe's path (...\build\korra_recomp.exe), then fails
 * with "[Hw::rofs::fst] file size table creation failed" and exits. */
static char g_guest_module[MAX_PATH] = "LoK.exe";
/* The HINSTANCE the guest sees for itself. It must be the *host's* module
 * handle: APIs that take an hInstance (DirectInput8Create, RegisterClassEx with
 * its own class) validate it, and 0x400000 -- the guest's load base -- is not a
 * loader handle, so DirectInput8Create returns E_INVALIDARG and the engine
 * shuts down before it ever creates a device. GetModuleFileName still answers
 * with the guest's path, so the engine finds its own GameData\. */
static uint32_t g_guest_hmodule;

void korra_set_guest_module(const char *path) {
    if (path) {
        strncpy(g_guest_module, path, MAX_PATH - 1);
        g_guest_module[MAX_PATH - 1] = 0;
    }
    g_guest_hmodule = (uint32_t)(uintptr_t)GetModuleHandleA(NULL);
}

static int is_guest_module(uint32_t hmod) {
    return hmod == 0 || hmod == g_guest_hmodule || hmod == 0x400000u;
}

static void shim_GetModuleFileNameA(void) {
    uint32_t hmod = ARGS[0], buf = ARGS[1], size = ARGS[2], n = 0;
    if (is_guest_module(hmod)) {
        size_t len = strlen(g_guest_module);
        n = (uint32_t)(len < size ? len : (size ? size - 1 : 0));
        if (n) memcpy((void *)(uintptr_t)buf, g_guest_module, n);
        if (size) ((char *)(uintptr_t)buf)[n] = 0;
    } else {
        n = GetModuleFileNameA((HMODULE)(uintptr_t)hmod, (LPSTR)(uintptr_t)buf, size);
    }
    g_eax = n;
    g_esp += 4 + 3 * 4;
}

static void shim_GetModuleFileNameW(void) {
    uint32_t hmod = ARGS[0], buf = ARGS[1], size = ARGS[2], n = 0;
    if (is_guest_module(hmod)) {
        wchar_t wide[MAX_PATH];
        MultiByteToWideChar(CP_ACP, 0, g_guest_module, -1, wide, MAX_PATH);
        size_t len = wcslen(wide);
        n = (uint32_t)(len < size ? len : (size ? size - 1 : 0));
        if (n) memcpy((void *)(uintptr_t)buf, wide, n * sizeof(wchar_t));
        if (size) ((wchar_t *)(uintptr_t)buf)[n] = 0;
    } else {
        n = GetModuleFileNameW((HMODULE)(uintptr_t)hmod, (LPWSTR)(uintptr_t)buf, size);
    }
    g_eax = n;
    g_esp += 4 + 3 * 4;
}

/* The guest's own module handle is its load base, not the host's HMODULE. */
static void shim_GetModuleHandleA(void) {
    uint32_t name = ARGS[0];
    if (!name) { g_eax = g_guest_hmodule; g_esp += 4 + 4; return; }
    g_eax = (uint32_t)(uintptr_t)GetModuleHandleA((LPCSTR)(uintptr_t)name);
    g_esp += 4 + 4;
}

static void shim_GetModuleHandleW(void) {
    uint32_t name = ARGS[0];
    if (!name) { g_eax = g_guest_hmodule; g_esp += 4 + 4; return; }
    g_eax = (uint32_t)(uintptr_t)GetModuleHandleW((LPCWSTR)(uintptr_t)name);
    g_esp += 4 + 4;
}

/* Resolve through the shim table first, so a run-time lookup gets the shim VA
 * (native32_shim_va) and not the native address behind it. */
static void shim_GetProcAddress(void) {
    uint32_t hmod = ARGS[0], nameva = ARGS[1], va = 0;
    if (nameva >= 0x10000u) {
        const char *name = (const char *)(uintptr_t)nameva;
        va = native32_shim_va(name);
        if (!va) va = (uint32_t)(uintptr_t)GetProcAddress((HMODULE)(uintptr_t)hmod, name);
    } else {
        va = (uint32_t)(uintptr_t)GetProcAddress((HMODULE)(uintptr_t)hmod, (LPCSTR)(uintptr_t)nameva);
    }
    g_eax = va;
    g_esp += 4 + 2 * 4;
}

static native32_shim_t g_shims[32];
static int g_shim_n;

static void add(const native32_shim_t *table, int n) {
    int i;
    for (i = 0; i < n && g_shim_n < (int)(sizeof g_shims / sizeof g_shims[0]); i++)
        g_shims[g_shim_n++] = table[i];
}

static void build(void) {
    int n;
    const native32_shim_t *t;
    if (g_shim_n) return;
    t = steam_stub_shims(&n);  add(t, n);
    /* KORRA_NO_AUDIO=1 leaves CoCreateInstance alone, so the engine's audio
     * init fails the way it would on a machine with no sound device. A boot
     * that gets further without audio means the stub (not the rest of the port)
     * is what the game is waiting on. */
    if (!getenv("KORRA_NO_AUDIO")) {
        t = xa2_stub_shims(&n); add(t, n);
    }
    g_shims[g_shim_n].name = "GetProcAddress";
    g_shims[g_shim_n].fn = shim_GetProcAddress;
    g_shim_n++;
    g_shims[g_shim_n].name = "GetModuleFileNameA";
    g_shims[g_shim_n].fn = shim_GetModuleFileNameA;
    g_shim_n++;
    g_shims[g_shim_n].name = "GetModuleFileNameW";
    g_shims[g_shim_n].fn = shim_GetModuleFileNameW;
    g_shim_n++;
    g_shims[g_shim_n].name = "GetModuleHandleA";
    g_shims[g_shim_n].fn = shim_GetModuleHandleA;
    g_shim_n++;
    g_shims[g_shim_n].name = "GetModuleHandleW";
    g_shims[g_shim_n].fn = shim_GetModuleHandleW;
    g_shim_n++;
}

int korra_shim_count(void) { build(); return g_shim_n; }
native32_shim_t *korra_shims(void) { build(); return g_shims; }