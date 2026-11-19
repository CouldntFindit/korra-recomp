/*
 * steam_stub.h - Steamworks stand-in for a Steam-free build.
 *
 * The game imports seven flat functions from steam_api.dll (pre-1.37 API):
 *   SteamAPI_Init, SteamAPI_Shutdown, SteamAPI_RunCallbacks,
 *   SteamAPI_RegisterCallback, SteamAPI_UnregisterCallback,
 *   SteamUser, SteamUserStats
 * and the seven are the whole Steam surface of the binary.
 *
 * Calling conventions, read off the call sites rather than assumed:
 *   SteamAPI_*               __cdecl   (the caller does "add esp,8" after
 *                                       RegisterCallback; a shim that popped
 *                                       the arguments itself would corrupt the
 *                                       guest stack - see docs/xaudio2_callsites.md)
 *   ISteamUser::GetSteamID   __thiscall with a hidden return pointer:
 *                            "mov ecx,this / push &id / call [vtbl+8]" and the
 *                            caller reads the id through EAX.
 */
#ifndef KORRA_STEAM_STUB_H
#define KORRA_STEAM_STUB_H

#include "native32.h"

/* 1 = log interface/slot use (default), 0 = quiet. */
void steam_stub_set_verbose(int level);

/* Non-NULL object pointers valid for the life of the process. */
void *steam_stub_user_object(void);
void *steam_stub_stats_object(void);

/* Import shims this module contributes to native32_bind (the seven above). */
const native32_shim_t *steam_stub_shims(int *count);

#endif /* KORRA_STEAM_STUB_H */
