/*
 * steam_stub.c - the seven steam_api imports and the two interface objects.
 *
 * Evidence (docs/xaudio2_callsites.md, "Steam" section):
 *   - steam_api.dll exports exactly seven names in the import table
 *     (SteamAPI_Init/Shutdown/RunCallbacks/RegisterCallback/UnregisterCallback,
 *     SteamUser, SteamUserStats) - the flat pre-1.37 API.
 *   - SteamAPI_RegisterCallback is called as __cdecl: the caller cleans up
 *     ("push 0x14b / push eax / call / add esp,8"). The previous hand stub in
 *     shims.c popped both arguments itself, which would have left g_esp 8
 *     bytes short and corrupted the lifted stack.
 *   - The only interface method the game ever calls is ISteamUser +0x08,
 *     GetSteamID (slot 2: GetHSteamUser 0, BLoggedOn 4, GetSteamID 8 - the
 *     pre-1.37 ISteamUser has no IUnknown base). It is a __thiscall with a
 *     hidden return pointer:
 *         0x632195  call [SteamUser]
 *         0x63219b  mov  edx,[eax]        ; vtable
 *         0x63219d  lea  ecx,[ebp-0x498]  ; &id
 *         0x6321a3  push ecx              ; hidden return pointer
 *         0x6321a4  mov  ecx,eax          ; this
 *         0x6321a6  call [edx+8]
 *         0x6321a9  mov  ecx,[eax]        ; reads the id through EAX
 *     The same pattern appears at 0x6321ea, 0x6326ea, 0x633006.
 *   - SteamUserStats() is only null-checked (0x76c0f0); no method of it is
 *     called anywhere in the binary.
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "native32.h"
#include "steam_stub.h"

#define STEAM_ARGS ((uint32_t *)(uintptr_t)(g_esp + 4))

static int g_verbose = 1;
static unsigned g_calls_registered;

void steam_stub_set_verbose(int level) { g_verbose = level; }

static void steam_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("[steam] ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    fflush(stderr);
    va_end(ap);
}

/* Fake but well-formed CSteamID: universe public, type individual, instance
 * desktop, account id 1 -> 0x0110000100000001. A zero id is "invalid" in the
 * SDK and some game code asserts on it; this one is merely unknown. */
#define STEAM_FAKE_ID 0x0110000100000001ull

/* =========================================================== import shims
 * The shims run inside the lifted model: arguments at g_esp+4, and the shim
 * removes exactly what the real function would have removed. Steam's flat C
 * API is __cdecl, so that is only the return address. */

static void shim_SteamAPI_Init(void) {
    if (g_verbose) steam_log("SteamAPI_Init -> true (no Steam client)");
    g_eax = 1;
    g_esp += 4;
}

static void shim_SteamAPI_Shutdown(void) {
    if (g_verbose) steam_log("SteamAPI_Shutdown");
    g_eax = 0;
    g_esp += 4;
}

static void shim_SteamAPI_RunCallbacks(void) {
    g_eax = 0;
    g_esp += 4;
}

/* void __cdecl SteamAPI_RegisterCallback(CCallbackBase *cb, int id) */
static void shim_SteamAPI_RegisterCallback(void) {
    uint32_t *a = STEAM_ARGS;
    g_calls_registered++;
    if (g_verbose > 1)
        steam_log("SteamAPI_RegisterCallback(cb=%08X, id=%d) - dropped",
                  a[0], (int)a[1]);
    g_eax = 0;
    g_esp += 4;              /* cdecl: the guest does its own "add esp,8" */
}

static void shim_SteamAPI_UnregisterCallback(void) {
    uint32_t *a = STEAM_ARGS;
    if (g_verbose > 1)
        steam_log("SteamAPI_UnregisterCallback(cb=%08X) - dropped", a[0]);
    g_eax = 0;
    g_esp += 4;              /* cdecl */
}

static void shim_SteamUser(void) {
    if (g_verbose) steam_log("SteamUser() -> stub ISteamUser %p", steam_stub_user_object());
    g_eax = (uint32_t)(uintptr_t)steam_stub_user_object();
    g_esp += 4;
}

static void shim_SteamUserStats(void) {
    if (g_verbose) steam_log("SteamUserStats() -> stub ISteamUserStats %p", steam_stub_stats_object());
    g_eax = (uint32_t)(uintptr_t)steam_stub_stats_object();
    g_esp += 4;
}

/* ======================================================= ISteamUser object
 * Method ABI: __thiscall (this in ECX). Methods that return a class by value
 * additionally push a hidden return pointer as their first stack argument.
 * Instead of a register-shuffling naked thunk per method, every entry is
 *
 *     mov edx, [esp+4]     ; first stack argument -> EDX (the 2nd register arg)
 *     jmp impl             ; __fastcall: ECX = this, EDX = arg1, stack unchanged
 *
 * and the C body is declared __fastcall with an extra leading stack parameter
 * that aliases the same slot, so MSVC's own epilogue pops exactly the dwords
 * the guest pushed. The body signature is therefore
 *
 *     void *__fastcall impl(void *self, void *arg1, void *arg1_on_the_stack)
 *
 * with self in ECX, arg1 in EDX, and arg1 repeated at [esp+4]. At entry the
 * stack is [ret][arg1][arg2]...; the body returns with "ret 4", which leaves
 * the guest's ESP exactly as the original callee would have.
 */
#define STEAM_VT_1(name, body)                                  \
    static void __declspec(naked) name(void) {                  \
        __asm { mov edx, dword ptr [esp + 4] }                  \
        __asm { jmp body }                                      \
    }

#define STEAM_VT_0(name, body)                                  \
    static void __declspec(naked) name(void) {                  \
        __asm { jmp body }                                      \
    }

typedef void *(__fastcall *steam_vt_fn)(void *);

/* --- slots 0..2: the only ones that can matter ------------------------- */
static UINT32 __fastcall user_GetHSteamUser(void *self) {
    (void)self;
    return 1;   /* any non-zero handle */
}

static int __fastcall user_BLoggedOn(void *self) {
    (void)self;
    return 1;   /* "logged on" so the game does not disable Steam features */
}

static void *__fastcall user_GetSteamID(void *self, void *out, void *out_dup) {
    (void)self; (void)out_dup;
    if (out) *(unsigned long long *)out = STEAM_FAKE_ID;
    if (g_verbose) steam_log("ISteamUser::GetSteamID -> %p = 0x%llX", out,
                             (unsigned long long)STEAM_FAKE_ID);
    return out;   /* MSVC returns the hidden pointer in EAX */
}

STEAM_VT_0(user_vt_GetHSteamUser, user_GetHSteamUser)
STEAM_VT_0(user_vt_BLoggedOn, user_BLoggedOn)
STEAM_VT_1(user_vt_GetSteamID, user_GetSteamID)

/* --- slots 3..9: documented ISteamUser shape, never reached in this binary
 * (evidence above). If the game ever grows a new call site, these answers are
 * harmless; the log line names the slot.
 * AdvertiseGame(CSteamID, uint32, uint16) returns void; the 64-bit id makes
 * three 4-byte slots, so it needs its own shape and is left unimplemented. */
static void *__fastcall user_unimplemented(void *self, void *arg1, void *arg1_dup) {
    (void)self; (void)arg1; (void)arg1_dup;
    if (g_verbose)
        steam_log("ISteamUser: unimplemented slot reached (1 stack argument) - returning 0. "
                  "Add its real signature in steam_stub.c");
    return NULL;
}
STEAM_VT_1(user_vt_unimpl, user_unimplemented)

static const steam_vt_fn g_user_vtbl[10] = {
    /* 0x00 GetHSteamUser      */ (steam_vt_fn)user_vt_GetHSteamUser,
    /* 0x04 BLoggedOn          */ (steam_vt_fn)user_vt_BLoggedOn,
    /* 0x08 GetSteamID         */ (steam_vt_fn)user_vt_GetSteamID,
    /* 0x0C BIsBehindNAT       */ (steam_vt_fn)user_vt_unimpl,
    /* 0x10 AdvertiseGame      */ (steam_vt_fn)user_vt_unimpl,
    /* 0x14 RequestEncryptedAppTicket */ (steam_vt_fn)user_vt_unimpl,
    /* 0x18 GetEncryptedAppTicket     */ (steam_vt_fn)user_vt_unimpl,
    /* 0x1C GetGameBadgeLevel  */ (steam_vt_fn)user_vt_unimpl,
    /* 0x20 GetPlayerSteamLevel*/ (steam_vt_fn)user_vt_unimpl,
    /* 0x24 RequestStoreAuthURL*/ (steam_vt_fn)user_vt_unimpl,
};

typedef struct { const steam_vt_fn *vtbl; UINT32 tag; } steam_object;

static steam_object g_user = { g_user_vtbl, 0x49535552u };  /* "ISUR" */
static steam_object g_stats = { NULL, 0x49535354u };        /* "ISST" */

/* ================================================== ISteamUserStats object
 * Never called by this binary. Every slot answers "0" and logs, so a future
 * call site is visible in the log instead of crashing. The arity is unknown by
 * construction, so an unimplemented_stats entry consumes one argument - the
 * shape of every plausible first call (RequestCurrentStats() takes none, so
 * even that one is only reached through a call site that does not exist). */
static void *__fastcall stats_unimplemented(void *self, void *arg1, void *arg1_dup) {
    (void)self; (void)arg1; (void)arg1_dup;
    if (g_verbose)
        steam_log("ISteamUserStats: method called but stubbed - returning 0. "
                  "Add the real signature in steam_stub.c");
    return NULL;
}

#define STATS_SLOTS 40
static steam_vt_fn g_stats_vtbl[STATS_SLOTS];

void *steam_stub_user_object(void) { return &g_user; }

void *steam_stub_stats_object(void) {
    int i;
    if (!g_stats.vtbl) {
        for (i = 0; i < STATS_SLOTS; i++) g_stats_vtbl[i] = (steam_vt_fn)user_vt_unimpl;
        g_stats.vtbl = g_stats_vtbl;
    }
    return &g_stats;
}

/* ================================================================ shims */
static const native32_shim_t g_shims[] = {
    { "SteamAPI_Init", shim_SteamAPI_Init },
    { "SteamAPI_Shutdown", shim_SteamAPI_Shutdown },
    { "SteamAPI_RunCallbacks", shim_SteamAPI_RunCallbacks },
    { "SteamAPI_RegisterCallback", shim_SteamAPI_RegisterCallback },
    { "SteamAPI_UnregisterCallback", shim_SteamAPI_UnregisterCallback },
    { "SteamUser", shim_SteamUser },
    { "SteamUserStats", shim_SteamUserStats },
};

const native32_shim_t *steam_stub_shims(int *count) {
    if (count) *count = (int)(sizeof g_shims / sizeof g_shims[0]);
    return g_shims;
}
