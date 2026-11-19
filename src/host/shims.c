/*
 * shims.c - hand-written import bodies that run inside the lifted model.
 *
 * native32 routes every other import to the real Windows API (the host is a
 * 32-bit process and the guest image is mapped at its own base, so a Win32
 * struct the guest builds is already what Windows expects). Only the things
 * Windows cannot answer for us live here:
 *
 *   steam_api.dll   no Steam client: the 7 imported functions are answered by
 *                   this file so the game never sees a missing export.
 *   GetProcAddress  must not hand out the raw address of a shimmed function,
 *                   or a module that resolves steam_api at run time bypasses
 *                   the shim and lands in a broken steam_api.dll.
 *
 * A shim runs with the guest's registers live: arguments are at g_esp+4 and
 * the shim removes them itself (it stands in for `ret n`).
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "native32.h"

/* A non-NULL object for SteamUser()/SteamUserStats(). The game only checks it
 * for NULL before calling through it; nothing is called in the Steam-free path
 * today, and if something is, the fault names the function (see log.txt). */
static uint8_t g_steam_iface[64];

#define ARGS ((uint32_t *)(uintptr_t)(g_esp + 4))

static void shim_SteamAPI_Init(void) {
    fprintf(stderr, "[steam] SteamAPI_Init -> 1 (stubbed)\n");
    g_eax = 1;
    g_esp += 4;
}

static void shim_SteamAPI_Shutdown(void) {
    g_eax = 0;
    g_esp += 4;
}

static void shim_SteamAPI_RunCallbacks(void) {
    g_eax = 0;
    g_esp += 4;
}

static void shim_SteamAPI_RegisterCallback(void) {
    g_eax = 0;
    g_esp += 4 + 2 * 4; /* (callback*, id) cdecl */
}

static void shim_SteamAPI_UnregisterCallback(void) {
    g_eax = 0;
    g_esp += 4 + 2 * 4;
}

static void shim_SteamUser(void) {
    static int logged;
    if (!logged++) fprintf(stderr, "[steam] SteamUser() -> stub object\n");
    g_eax = (uint32_t)(uintptr_t)g_steam_iface;
    g_esp += 4;
}

static void shim_SteamUserStats(void) {
    static int logged;
    if (!logged++) fprintf(stderr, "[steam] SteamUserStats() -> stub object\n");
    g_eax = (uint32_t)(uintptr_t)g_steam_iface;
    g_esp += 4;
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

static native32_shim_t g_shims[] = {
    { "SteamAPI_Init", shim_SteamAPI_Init },
    { "SteamAPI_Shutdown", shim_SteamAPI_Shutdown },
    { "SteamAPI_RunCallbacks", shim_SteamAPI_RunCallbacks },
    { "SteamAPI_RegisterCallback", shim_SteamAPI_RegisterCallback },
    { "SteamAPI_UnregisterCallback", shim_SteamAPI_UnregisterCallback },
    { "SteamUser", shim_SteamUser },
    { "SteamUserStats", shim_SteamUserStats },
    { "GetProcAddress", shim_GetProcAddress },
};

int korra_shim_count(void) { return (int)(sizeof g_shims / sizeof g_shims[0]); }
native32_shim_t *korra_shims(void) { return g_shims; }
