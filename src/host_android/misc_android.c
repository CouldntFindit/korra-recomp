/* misc_android.c - the smaller DLLs: gdi32, shell32, winmm, ole32, oleaut32,
 * steam_api, XInput/DirectInput and the four D3DX entry points.
 *
 * Two of these need per-slot COM behaviour (steam interfaces, DirectInput):
 * a generic "return 0" body is only safe where the arity is right, so each
 * reachable slot has its own thunk with the exact `RET(eax, argc)` purge. That
 * was the class of bug that cost every long Windows run (AGENTS.md ledger).
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "host_android.h"

/* --------------------------------------------------------------- gdi32 */
static void g_GetDeviceCaps(void) {
    switch (A32(1)) {
    case 8:  RET(1280, 2);    /* HORZRES */
    case 10: RET(720, 2);     /* VERTRES */
    case 12: RET(32, 2);      /* BITSPIXEL */
    case 14: RET(1, 2);       /* PLANES */
    default: RET(0, 2);
    }
}
static void g_GetObjectA(void) {
    if (A32(2)) memset(ka_host(A32(2)), 0, A32(1));
    RET(0, 3);
}

/* ------------------------------------------------------------- shell32 */
static void s_SHGetSpecialFolderPathA(void) {
    uint32_t out = A32(1);
    if (out) {
        const char *p = ka_fs_root();
        size_t n = strlen(p);
        if (n > 200) n = 200;
        memcpy(ka_host(out), p, n);
        ((char *)ka_host(out))[n] = 0;
    }
    RET(1, 4);
}

/* --------------------------------------------------------------- winmm */
static void w_timeBeginPeriod(void) { RET(0, 1); }
static void w_timeEndPeriod(void) { RET(0, 1); }
static void w_timeGetTime(void) {
    extern uint32_t ka_mono_ms(void);
    RET(ka_mono_ms(), 0);
}

/* ---------------------------------------------------------------- ole32 */
static void o_CoInitializeEx(void) { RET(0, 2); }
static void o_CoInitialize(void) { RET(0, 1); }
static void o_CoUninitialize(void) { RETV(0); }
static void o_OleInitialize(void) { RET(0, 1); }
static void o_OleUninitialize(void) { RETV(0); }
static void o_CoSetProxyBlanket(void) { RET(0, 8); }
static void o_CreateStreamOnHGlobal(void) { RET(0x80004005u, 3); }

/* CLSIDs the engine asks for (docs/audio.md section 12): XAudio2 2.7. Audio is
 * M4; until the 2.7 object exists, answering CLASS_E_CLASSNOTREG makes CRI take
 * its documented silent fallback instead of running a half-implemented device. */
static const uint8_t kClsidXAudio2[16] = {
    0x85, 0x86, 0x50, 0x5a, 0xa2, 0x54, 0xba, 0x4f,
    0x9b, 0x82, 0x9a, 0x24, 0xb0, 0x03, 0x06, 0xaf
};
static void o_CoCreateInstance(void) {
    uint32_t clsid = A32(0), out = A32(4);
    int match = 1;
    for (int i = 0; i < 16; i++)
        if (MEM8(clsid + (uint32_t)i) != kClsidXAudio2[i]) { match = 0; break; }
    if (out) MEM32(out) = 0;
    RET(match ? 0x80004005u : 0x80040154u, 5);  /* E_FAIL / REGDB_E_CLASSNOTREG */
}

/* ------------------------------------------------------------- oleaut32 */
static void a_SysAllocString(void) {
    const uint16_t *src = AWSTR(0);
    uint32_t bytes = 4, len = 0;
    uint32_t p;
    if (src) { while (src[len]) len++; bytes = 4 + (len + 1) * 2; }
    p = ka_guest_alloc(bytes, 4);
    if (p) {
        MEM32(p) = bytes - 4;
        for (uint32_t i = 0; i < len; i++) ((uint16_t *)ka_host(p + 4))[i] = src[i];
        ((uint16_t *)ka_host(p + 4))[len] = 0;
        RET(p + 4, 1);
    }
    RET(0, 1);
}
static void a_SysFreeString(void) {
    uint32_t bstr = A32(0);
    if (bstr) ka_guest_free(bstr - 4);
    RETV(1);
}
static void a_OleLoadPicture(void) { RET(0x80004005u, 5); }

/* -------------------------------------------------------------- steam_api */
/* Steam identifiers are 64-bit and every flat-API import is __cdecl: the guest
 * pops its own arguments, so a steam shim only removes the return address. */
static void st_Init(void) { g_eax = 1; g_esp += 4; }
static void st_Shutdown(void) { g_eax = 0; g_esp += 4; }
static void st_RunCallbacks(void) { g_eax = 0; g_esp += 4; }
static void st_RegisterCallback(void) { g_eax = 0; g_esp += 4; }
static void st_UnregisterCallback(void) { g_eax = 0; g_esp += 4; }

#define STEAM_FAKE_ID 0x0110000100000001ull

static void st_User(void) {
    extern uint32_t ka_steam_user_object(void);
    g_eax = ka_steam_user_object();
    g_esp += 4;
}
static void st_UserStats(void) {
    extern uint32_t ka_steam_stats_object(void);
    g_eax = ka_steam_stats_object();
    g_esp += 4;
}

/* ISteamUser slots (pre-1.37: no IUnknown base) */
static void u_GetHSteamUser(void) { RET(1, 0); }
static void u_BLoggedOn(void) { RET(1, 0); }
static void u_GetSteamID(void) {
    uint32_t out = A32(0);
    if (out) { MEM32(out) = (uint32_t)STEAM_FAKE_ID; MEM32(out + 4) = (uint32_t)(STEAM_FAKE_ID >> 32); }
    RET(out, 1);
}

/* ISteamUserStats slots 0..15 (2014 layout, docs/xaudio2_callsites.md) */
static uint32_t g_empty;
static void s_Request(void) { RET(1, 0); }
static void s_GetStat(void) {
    uint32_t out = A32(1);
    if (out) MEM32(out) = 0;
    RET(1, 2);
}
static void s_SetStat(void) { RET(1, 2); }
static void s_UpdateAvg(void) { RET(1, 4); }
static void s_GetAchievement(void) {
    uint32_t out = A32(1);
    if (out) MEM8(out) = 0;
    RET(1, 2);
}
static void s_OneName(void) { RET(1, 1); }
static void s_AchTime(void) {
    if (A32(1)) MEM8(A32(1)) = 0;
    if (A32(2)) MEM32(A32(2)) = 0;
    RET(1, 3);
}
static void s_Store(void) { RET(1, 0); }
static void s_Icon(void) { RET(0, 1); }
static void s_DisplayAttr(void) {
    extern uint32_t ka_guest_empty_string(void);
    RET(ka_guest_empty_string(), 2);
}
static void s_Indicate(void) { RET(0, 3); }
static void s_Num(void) { RET(0, 0); }
static void s_Name(void) {
    extern uint32_t ka_guest_empty_string(void);
    RET(ka_guest_empty_string(), 1);
}
static void s_notimpl(void) {
    ka_log("steam: unimplemented stats slot reached from 0x%08X", g_cur_func);
    RET(0, 0);
}

/* ------------------------------------------------------------- XInput */
static void x_GetState(void) {
    uint32_t idx = A32(0), out = A32(1);
    ka_pad_state pad;
    if (idx != 0 || !out) RET(1167u, 2);        /* ERROR_DEVICE_NOT_CONNECTED */
    ka_pad_read(&pad);
    memset(ka_host(out), 0, 16);
    MEM32(out) = ka_hat_packet();
    MEM16(out + 4) = (uint16_t)pad.buttons;
    MEM8(out + 6) = pad.lt;
    MEM8(out + 7) = pad.rt;
    *(int16_t *)ka_host(out + 8) = pad.lx;
    *(int16_t *)ka_host(out + 10) = pad.ly;
    *(int16_t *)ka_host(out + 12) = pad.rx;
    *(int16_t *)ka_host(out + 14) = pad.ry;
    RET(0, 2);
}
static void x_SetState(void) { RET(0, 2); }

/* DirectInput8: enumerate no devices, answer everything else with a failure.
 * The engine's keyboard path then finds nothing (correct: a phone has no
 * keyboard) and the XInput path keeps working.
 *
 * Each vtable slot pops its own argument count. A single shared thunk cannot
 * do that: IDirectInput8 is QueryInterface(2), AddRef(0), Release(0),
 * CreateDevice(3), EnumDevices(4), GetDeviceStatus(1), RunControlPanel(2),
 * Initialize(2). */
static void d_QueryInterface(void) {
    uint32_t out = A32(1);
    if (out) MEM32(out) = 0;
    RET(0x80004002u, 2);              /* E_NOINTERFACE */
}
static void d_AddRef(void) { RET(1, 0); }
static void d_Release(void) { RET(1, 0); }
static void d_GetDeviceStatus(void) { RET(0x80004005u, 1); }
static void d_RunControlPanel(void) { RET(0x80004005u, 2); }
static void d_Initialize(void) { RET(0x80004005u, 2); }
/* EnumDevices(dwDevType, callback, ref, dwFlags) is 4 args - popping 5 ate the
 * caller's local on every DirectInput init (found on device, docs/quirks.md 5.2). */
static void d_EnumDevices(void) { RET(0, 4); }
/* CreateDevice(rguid, out, outer) is 3. */
static void d_CreateDevice(void) { RET(0x80070057u, 3); }  /* E_INVALIDARG */
static void di_DirectInput8Create(void) { RET(ka_dinput_object(), 5); }

/* --------------------------------------------------------------- D3DX9 */
static void dx_CreateTexture(void) {
    extern int ka_d3dx_create_texture(void);
    RET(ka_d3dx_create_texture(), 8);
}
static void dx_TextureFromMemory(void) {
    extern int ka_d3dx_texture_from_memory(void);
    RET(ka_d3dx_texture_from_memory(), 15);
}
static void dx_CubeFromMemory(void) {
    extern int ka_d3dx_cube_from_memory(void);
    RET(ka_d3dx_cube_from_memory(), 14);
}
static void dx_GetShaderConstantTable(void) {
    extern int ka_d3dx_constant_table(void);
    RET(ka_d3dx_constant_table(), 2);
}

/* ------------------------------------------------------------- the tables */
const ka_shim ka_gdi32_shims[] = {
    { "GetDeviceCaps", g_GetDeviceCaps },
    { "GetObjectA", g_GetObjectA },
    { NULL, NULL }
};

const ka_shim ka_misc_shims[] = {
    { "SHGetSpecialFolderPathA", s_SHGetSpecialFolderPathA },
    { "timeBeginPeriod", w_timeBeginPeriod },
    { "timeEndPeriod", w_timeEndPeriod },
    { "timeGetTime", w_timeGetTime },
    { "CoInitializeEx", o_CoInitializeEx },
    { "CoInitialize", o_CoInitialize },
    { "CoUninitialize", o_CoUninitialize },
    { "OleInitialize", o_OleInitialize },
    { "OleUninitialize", o_OleUninitialize },
    { "CoSetProxyBlanket", o_CoSetProxyBlanket },
    { "CreateStreamOnHGlobal", o_CreateStreamOnHGlobal },
    { "CoCreateInstance", o_CoCreateInstance },
    { "SysAllocString", a_SysAllocString },
    { "SysFreeString", a_SysFreeString },
    { "OleLoadPicture", a_OleLoadPicture },
    { NULL, NULL }
};

const ka_shim ka_steam_shims[] = {
    { "SteamAPI_Init", st_Init },
    { "SteamAPI_Shutdown", st_Shutdown },
    { "SteamAPI_RunCallbacks", st_RunCallbacks },
    { "SteamAPI_RegisterCallback", st_RegisterCallback },
    { "SteamAPI_UnregisterCallback", st_UnregisterCallback },
    { "SteamUser", st_User },
    { "SteamUserStats", st_UserStats },
    { NULL, NULL }
};

const ka_shim ka_input_shims[] = {
    { "XInputGetState", x_GetState },
    { "XInputSetState", x_SetState },
    { "DirectInput8Create", di_DirectInput8Create },
    { NULL, NULL }
};

const ka_shim ka_d3dx_shims[] = {
    { "D3DXCreateTexture", dx_CreateTexture },
    { "D3DXCreateTextureFromFileInMemoryEx", dx_TextureFromMemory },
    { "D3DXCreateCubeTextureFromFileInMemoryEx", dx_CubeFromMemory },
    { "D3DXGetShaderConstantTable", dx_GetShaderConstantTable },
    { NULL, NULL }
};

/* --------------------------------------------------- COM objects in guest memory */
/* An object is [vtable VA][tag]. The vtable is a guest array of shim VAs, so the
 * guest's `mov eax,[ecx]; call [eax+4*n]` resolves through recomp_lookup_import
 * with no host pointer ever reaching a 32-bit field. */
static uint32_t make_object(const ka_shim *slots, int count, const char *prefix, uint32_t tag) {
    uint32_t vt = ka_guest_alloc((uint32_t)count * 4, 4);
    uint32_t obj = ka_guest_alloc(8, 4);
    for (int i = 0; i < count; i++) {
        char name[96];
        snprintf(name, sizeof name, "%s:%d", prefix, i);
        {
            uint32_t va = ka_shim_va(name);
            if (!va) va = ka_shim_stub(name);
            MEM32(vt + (uint32_t)i * 4) = va;
        }
    }
    MEM32(obj) = vt;
    MEM32(obj + 4) = tag;
    return obj;
}

static uint32_t g_empty_str;
uint32_t ka_guest_empty_string(void) {
    if (!g_empty_str) g_empty_str = ka_guest_strdup("");
    return g_empty_str;
}

static uint32_t g_user_obj, g_stats_obj;
uint32_t ka_steam_user_object(void) {
    static const ka_shim slots[] = {
        { "SteamUser:0", u_GetHSteamUser }, { "SteamUser:1", u_BLoggedOn },
        { "SteamUser:2", u_GetSteamID },    { NULL, NULL }
    };
    if (!g_user_obj) {
        /* Register the slots before make_object looks them up. */
        ka_shim_register(slots);
        g_user_obj = make_object(slots, 3, "SteamUser", 0x49535552u);
    }
    return g_user_obj;
}
uint32_t ka_steam_stats_object(void) {
    static const ka_shim slots[] = {
        { "SteamStats:0", s_Request }, { "SteamStats:1", s_GetStat },
        { "SteamStats:2", s_GetStat }, { "SteamStats:3", s_SetStat },
        { "SteamStats:4", s_SetStat }, { "SteamStats:5", s_UpdateAvg },
        { "SteamStats:6", s_GetAchievement }, { "SteamStats:7", s_OneName },
        { "SteamStats:8", s_OneName }, { "SteamStats:9", s_AchTime },
        { "SteamStats:10", s_Store }, { "SteamStats:11", s_Icon },
        { "SteamStats:12", s_DisplayAttr }, { "SteamStats:13", s_Indicate },
        { "SteamStats:14", s_Num }, { "SteamStats:15", s_Name },
        { NULL, NULL }
    };
    if (!g_stats_obj) {
        ka_shim_register(slots);
        g_stats_obj = make_object(slots, 16, "SteamStats", 0x49535354u);
    }
    return g_stats_obj;
}

static uint32_t g_dinput_obj;
uint32_t ka_dinput_object(void) {
    static const ka_shim slots[] = {
        { "DInput:0", d_QueryInterface }, { "DInput:1", d_AddRef },
        { "DInput:2", d_Release },
        { "DInput:3", d_CreateDevice }, { "DInput:4", d_EnumDevices },
        { "DInput:5", d_GetDeviceStatus }, { "DInput:6", d_RunControlPanel },
        { "DInput:7", d_Initialize },
        { NULL, NULL }
    };
    if (!g_dinput_obj) {
        ka_shim_register(slots);
        g_dinput_obj = make_object(slots, 8, "DInput", 0x44494E50u);
    }
    return g_dinput_obj;
}

