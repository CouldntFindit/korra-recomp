/*
 * selftest_host_shims.c - proves the steam and XAudio2 stubs match the ABI the
 * guest uses, without the game and without the lifted code.
 *
 * The one thing that silently corrupts a lifted guest is a wrong stack purge:
 * native32_bridge moves g_esp by however far the callee moved esp, so a method
 * that pops one dword too few leaves the guest's ESP four bytes high. Every
 * call below therefore goes through real x86 thunks that push the guest's
 * arguments, put `this` in ECX the way the guest does, and measure the purge
 * exactly as native_bridge does - then check it against the number read off
 * the call site in docs/xaudio2_callsites.md.
 *
 * Build + run: powershell -File scripts\run_selftest.ps1
 */
#include <windows.h>
#include <mmreg.h>
#include <stdio.h>
#include <string.h>
#include "native32.h"
#include "../src/host/xaudio2_stub.h"
#include "../src/host/steam_stub.h"

/* native32.c is not linked here; the stubs only touch eax/esp. */
uint32_t g_eax, g_ecx, g_edx, g_esp, g_ebx, g_esi, g_edi, g_ebp;

static int g_fail, g_pass;

#define CHECK(cond, what)                                                      \
    do {                                                                       \
        if (cond) { g_pass++; }                                                \
        else { g_fail++; printf("  FAIL %s (%s:%d)\n", what, __FILE__, __LINE__); } \
    } while (0)

/* ------------------------------------------------------------- guest call
 * c_call: push args[0..nargs-1] (args[0] nearest the return address), set
 * ECX = obj, call fn, record EAX and the distance ESP travelled. Mirrors
 * native_bridge, which copies the guest's argument slots and measures the
 * callee's purge. */
typedef struct {
    const void *fn;
    void *obj;
    uint32_t nargs;
    const uint32_t *args;
    uint32_t ret;
    uint32_t purge;
} call_req;

static call_req *g_req;

static void __declspec(naked) c_call(void) {
    __asm {
        push esi
        push ebx
        mov  ebx, dword ptr [g_req]
        mov  eax, dword ptr [ebx + 8]
        mov  esi, dword ptr [ebx + 12]
    push_loop:
        test eax, eax
        jz   do_call
        sub  eax, 1
        push dword ptr [esi + eax * 4]
        jmp  push_loop
    do_call:
        mov  esi, esp                 ; base: first argument slot
        mov  ecx, dword ptr [ebx + 4] ; this
        call dword ptr [ebx]          ; the vtable entry / shim
        mov  edx, esp
        sub  edx, esi                 ; bytes the callee popped
        mov  dword ptr [ebx + 20], edx
        mov  dword ptr [ebx + 16], eax
        pop  ebx
        pop  esi
        ret
    }
}

static uint32_t guest_call(void *obj, const void *fn, uint32_t nargs, const uint32_t *args,
                           uint32_t expect_purge, const char *what) {
    call_req r;
    memset(&r, 0, sizeof r);
    r.fn = fn; r.obj = obj; r.nargs = nargs; r.args = args;
    g_req = &r;
    c_call();
    if (r.purge != expect_purge) {
        g_fail++;
        printf("  FAIL %s: purge %u, expected %u (stack would desync)\n",
               what, r.purge, expect_purge);
    } else {
        g_pass++;
    }
    return r.ret;
}

/* --------------------------------------------------------------- helpers */
static const void **vtbl_of(void *obj) { return *(const void ***)obj; }
static void *slot(void *obj, unsigned offset) { return (void *)vtbl_of(obj)[offset / 4]; }

/* A WAVEFORMATEXTENSIBLE float32 48 kHz, exactly what FUN_007b2a95 builds. */
static WAVEFORMATEXTENSIBLE make_fmt(unsigned channels) {
    WAVEFORMATEXTENSIBLE f;
    memset(&f, 0, sizeof f);
    f.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    f.Format.nChannels = (WORD)channels;
    f.Format.nSamplesPerSec = 48000;
    f.Format.wBitsPerSample = 32;
    f.Format.nBlockAlign = (WORD)(channels * 4);
    f.Format.nAvgBytesPerSec = 48000 * channels * 4;
    f.Format.cbSize = 22;
    f.Samples.wValidBitsPerSample = 32;
    f.dwChannelMask = channels == 2 ? 3 : 0x3F;
    f.SubFormat.Data1 = 0x00000003;   /* KSDATAFORMAT_SUBTYPE_IEEE_FLOAT */
    f.SubFormat.Data2 = 0x0000;
    f.SubFormat.Data3 = 0x0010;
    f.SubFormat.Data4[0] = 0x80; f.SubFormat.Data4[1] = 0x00;
    f.SubFormat.Data4[2] = 0x00; f.SubFormat.Data4[3] = 0xaa;
    f.SubFormat.Data4[4] = 0x00; f.SubFormat.Data4[5] = 0x38;
    f.SubFormat.Data4[6] = 0x9b; f.SubFormat.Data4[7] = 0x71;
    return f;
}

/* 85.3 ms of stereo float32 = 4096 frames at 48 kHz, i.e. 32768 bytes.
 * Long enough that "still queued at 15 ms" cannot be scheduler noise. */
#define TEST_FRAMES 4096u

/* ------------------------------------------------------- XAudio2 sequence
 * The order follows FUN_007b2e24 -> FUN_007b2a95 -> FUN_007b26b7. */
static void test_xaudio2(void) {
    void *dev = NULL, *master = NULL, *src = NULL;
    GUID other = { 0x3901cc3f, 0x84b5, 0x4fa4, { 0xba, 0x35, 0xaa, 0x81, 0x72, 0xb8, 0xa0, 0x9b } };
    HRESULT hr;
    UINT32 count = 0, args[8];
    struct { UINT32 flags, channels, rate; } details;
    BYTE details_raw[0x42C];
    WAVEFORMATEXTENSIBLE fmt = make_fmt(2);
    BYTE audio[TEST_FRAMES * 8];
    struct { UINT32 flags, bytes; const void *data; UINT32 pb, pl, lb, ll, lc; const void *ctx; } buf;
    struct { const void *ctx; UINT32 queued; unsigned long long played; } state;
    int ctx_token = 0;

    printf("XAudio2:\n");
    memset(audio, 0, sizeof audio);
    memset(&buf, 0, sizeof buf);
    buf.bytes = sizeof audio;
    buf.data = audio;
    buf.ctx = &ctx_token;

    /* A non-XAudio2 CLSID must be refused so the shim forwards it to ole32. */
    CHECK(xa2_stub_create_instance(&other, 1, &xa2_iid_ixaudio2, &dev) ==
              (HRESULT)0x80040111L && dev == NULL,
          "foreign CLSID refused");

    hr = xa2_stub_create_instance(&xa2_clsid_xaudio2, 1, &xa2_iid_ixaudio2, &dev);
    CHECK(hr == 0 && dev != NULL, "CoCreateInstance(CLSID_XAudio2)");
    if (!dev) return;

    /* Initialize(flags, processor): 2 args + this -> ret + 12 */
    args[0] = (UINT32)(uintptr_t)dev;
    args[1] = 0;
    args[2] = 0xFFFFFFFFu;   /* XAUDIO2_DEFAULT_PROCESSOR */
    CHECK(guest_call(dev, slot(dev, 0x14), 3, args, 12, "Initialize") == 0, "Initialize -> S_OK");

    args[0] = (UINT32)(uintptr_t)dev;
    args[1] = (UINT32)(uintptr_t)&count;
    CHECK(guest_call(dev, slot(dev, 0x0C), 2, args, 8, "GetDeviceCount") == 0 && count == 1,
          "GetDeviceCount -> 1");

    args[0] = (UINT32)(uintptr_t)dev;
    args[1] = 0x009b38a0;    /* the guest's CriNcvXAudio2EngineCallback object */
    CHECK(guest_call(dev, slot(dev, 0x18), 2, args, 8, "RegisterForCallbacks") == 0,
          "RegisterForCallbacks -> S_OK");

    args[0] = (UINT32)(uintptr_t)dev;
    args[1] = 0;                       /* device index */
    args[2] = (UINT32)(uintptr_t)details_raw;
    CHECK(guest_call(dev, slot(dev, 0x10), 3, args, 12, "GetDeviceDetails") == 0,
          "GetDeviceDetails -> S_OK");
    /* FUN_0088b0c0 reads the channel count at +0x406 (DeviceID[256] +
     * DisplayName[256] + Role, then WAVEFORMATEX). */
    CHECK(*(WORD *)((BYTE *)details_raw + 0x406) == 2,
          "XAUDIO2_DEVICE_DETAILS.nChannels is at 0x406 (Hw mixer's read)");

    /* CreateMasteringVoice(&master, 0, 48000, 0, deviceIndex=0, chain=0) */
    args[0] = (UINT32)(uintptr_t)dev;
    args[1] = (UINT32)(uintptr_t)&master;
    args[2] = 0;
    args[3] = 48000;
    args[4] = 0;
    args[5] = 0;
    args[6] = 0;
    CHECK(guest_call(dev, slot(dev, 0x28), 7, args, 28, "CreateMasteringVoice") == 0 && master,
          "CreateMasteringVoice -> object");

    if (master) {
        args[0] = (UINT32)(uintptr_t)master;
        args[1] = (UINT32)(uintptr_t)&details;
        CHECK(guest_call(master, slot(master, 0x00), 2, args, 8, "GetVoiceDetails") == 0,
              "GetVoiceDetails -> S_OK");
        CHECK(details.channels == 2 && details.rate == 48000, "mastering voice reports 2ch/48k");
    }

    /* CreateSourceVoice(pp, fmt, flags=0, ratio, cb, sends, chain) */
    args[0] = (UINT32)(uintptr_t)dev;
    args[1] = (UINT32)(uintptr_t)&src;
    args[2] = (UINT32)(uintptr_t)&fmt;
    args[3] = 0;
    { union { float f; UINT32 u; } r; r.f = 2.0f; args[4] = r.u; }
    args[5] = 0x009b38a4;    /* the guest's voice callback object */
    args[6] = 0;
    args[7] = 0;
    CHECK(guest_call(dev, slot(dev, 0x20), 8, args, 32, "CreateSourceVoice") == 0 && src,
          "CreateSourceVoice -> object");
    if (!src) return;

    args[0] = (UINT32)(uintptr_t)dev;
    args[1] = 0x7FFFFFFF;    /* the OperationSet CRI passes */
    CHECK(guest_call(dev, slot(dev, 0x34), 2, args, 8, "CommitChanges") == 0,
          "CommitChanges -> S_OK");

    /* Submit while stopped: queued but not playing. */
    args[0] = (UINT32)(uintptr_t)src;
    args[1] = (UINT32)(uintptr_t)&buf;
    args[2] = 0;
    CHECK(guest_call(src, slot(src, 0x54), 3, args, 12, "SubmitSourceBuffer") == 0,
          "SubmitSourceBuffer -> S_OK");

    args[0] = (UINT32)(uintptr_t)src;
    args[1] = (UINT32)(uintptr_t)&state;
    memset(&state, 0xAA, sizeof state);
    guest_call(src, slot(src, 0x64), 2, args, 8, "GetState(stopped)");
    CHECK(state.queued == 1 && state.played == 0, "stopped voice holds the buffer");

    /* Start(flags=0, opset) and let the virtual clock run. */
    args[0] = (UINT32)(uintptr_t)src;
    args[1] = 0;
    args[2] = 0x7FFFFFFF;
    CHECK(guest_call(src, slot(src, 0x4C), 3, args, 12, "Start") == 0, "Start -> S_OK");

    Sleep(15);   /* 1024 frames @48k = 21.3 ms */
    args[0] = (UINT32)(uintptr_t)src;
    args[1] = (UINT32)(uintptr_t)&state;
    guest_call(src, slot(src, 0x64), 2, args, 8, "GetState(playing 15ms)");
    CHECK(state.played > 0, "SamplesPlayed advances while playing");
    CHECK(state.queued == 1, "buffer still queued at 15 ms");

    Sleep(120);
    guest_call(src, slot(src, 0x64), 2, args, 8, "GetState(playing 120ms)");
    CHECK(state.played >= TEST_FRAMES, "buffer retired after its duration");
    CHECK(state.queued == 0, "BuffersQueued drains (the engine can feed again)");
    CHECK(state.ctx == &ctx_token, "pCurrentBufferContext is the guest's context");

    /* Same calls at the end of a stream: CRI does this on voice teardown. */
    args[0] = (UINT32)(uintptr_t)src;
    args[1] = 0;
    args[2] = 0;
    CHECK(guest_call(src, slot(src, 0x50), 3, args, 12, "Stop") == 0, "Stop -> S_OK");
    args[0] = (UINT32)(uintptr_t)src;
    CHECK(guest_call(src, slot(src, 0x58), 1, args, 4, "FlushSourceBuffers") == 0,
          "FlushSourceBuffers -> S_OK");
    CHECK(guest_call(src, slot(src, 0x5C), 1, args, 4, "Discontinuity") == 0,
          "Discontinuity -> S_OK");
    guest_call(src, slot(src, 0x48), 1, args, 4, "DestroyVoice");   /* returns void */

    args[0] = (UINT32)(uintptr_t)dev;
    guest_call(dev, slot(dev, 0x08), 1, args, 4, "Release");
}

/* ------------------------------------------------------------------ Steam
 * SteamAPI_* are __cdecl shims: they read g_esp+4 and pop only the return
 * address, because the guest emits its own "add esp,8". */
static void test_steam(void) {
    int n = 0, i;
    const native32_shim_t *shims = steam_stub_shims(&n);
    void *user, *stats;
    unsigned long long id = 0;
    uint32_t stack[8], args[1];

    printf("Steam:\n");
    CHECK(n == 7, "seven steam_api shims");

    for (i = 0; i < n; i++) {
        if (!strcmp(shims[i].name, "SteamAPI_Init")) {
            uint32_t before;
            memset(stack, 0, sizeof stack);
            stack[0] = 0xDEADBEEF;     /* the guest's return address */
            g_esp = (uint32_t)(uintptr_t)&stack[0];
            before = g_esp;
            shims[i].fn();
            CHECK(g_eax == 1, "SteamAPI_Init -> true");
            CHECK(g_esp == before + 4, "SteamAPI_Init pops only cdecl's return address");
        }
        if (!strcmp(shims[i].name, "SteamAPI_RegisterCallback")) {
            uint32_t before;
            memset(stack, 0, sizeof stack);
            stack[0] = 0xDEADBEEF;     /* ret */
            stack[1] = 0x00401234;     /* CCallbackBase* */
            stack[2] = 0x14B;          /* callback id 331, as at 0x75dbe0 */
            g_esp = (uint32_t)(uintptr_t)&stack[0];
            before = g_esp;
            shims[i].fn();
            CHECK(g_esp == before + 4, "RegisterCallback pops only the return address (cdecl)");
        }
        if (!strcmp(shims[i].name, "SteamUser")) {
            g_esp = (uint32_t)(uintptr_t)&stack[0];
            stack[0] = 0xDEADBEEF;
            shims[i].fn();
            user = (void *)(uintptr_t)g_eax;
            CHECK(user != NULL, "SteamUser -> object");
        }
        if (!strcmp(shims[i].name, "SteamUserStats")) {
            g_esp = (uint32_t)(uintptr_t)&stack[0];
            stack[0] = 0xDEADBEEF;
            shims[i].fn();
            stats = (void *)(uintptr_t)g_eax;
            CHECK(stats != NULL, "SteamUserStats -> object");
        }
    }

    user = steam_stub_user_object();
    /* GetSteamID: thiscall, one hidden-return-pointer argument (0x6321a6). */
    args[0] = (UINT32)(uintptr_t)&id;
    {
        uint32_t ret = guest_call(user, slot(user, 0x08), 1, args, 4, "GetSteamID");
        CHECK(ret == (uint32_t)(uintptr_t)&id, "GetSteamID returns the hidden pointer in EAX");
        CHECK(id == 0x0110000100000001ull, "GetSteamID wrote a well-formed id");
    }
    /* BLoggedOn: thiscall, no stack arguments. */
    CHECK(guest_call(user, slot(user, 0x04), 0, args, 0, "BLoggedOn") == 1,
          "BLoggedOn -> 1");
    CHECK(guest_call(user, slot(user, 0x00), 0, args, 0, "GetHSteamUser") == 1,
          "GetHSteamUser -> non-zero");
}

int main(void) {
    xa2_stub_set_verbose(0);
    steam_stub_set_verbose(0);

    test_xaudio2();
    test_steam();

    xa2_stub_stats st;
    xa2_stub_get_stats(&st);
    printf("\nXAudio2 stub stats: %u created, %u destroyed, %u live, "
           "%u buffers, %llu frames, %u cocreate hits\n",
           st.voices_created, st.voices_destroyed, st.voices_live,
           st.buffers_submitted, st.frames_played, st.cocreate_hits);

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
