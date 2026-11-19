/*
 * xaudio2_stub.c - a silent IXAudio2 2.7 for The Legend of Korra's CRI NcVoice.
 *
 * What the guest does (docs/xaudio2_callsites.md has every address):
 *   FUN_007b22a8  CoCreateInstance(CLSID_XAudio2*, IID_IXAudio2), then
 *                 vtbl+0x14 Initialize(flags, 0xFFFFFFFF)  -- the wrapper is
 *                 shared by two consumers:
 *     FUN_007b2e24  CRI NcVoice device init: vtbl+0x18 RegisterForCallbacks
 *                   (engine cb), vtbl+0x0C GetDeviceCount, vtbl+0x28
 *                   CreateMasteringVoice(&v,0,48000,0,deviceIndex,0), then
 *                   vtbl+0x00 GetVoiceDetails on it
 *     FUN_0088b0c0  the engine's own Hw audio device: vtbl+0x28 CreateMastering
 *                   Voice, vtbl+0x18 RegisterForCallbacks, vtbl+0x10
 *                   GetDeviceDetails (reads OutputFormat.nChannels at +0x406),
 *                   vtbl+0x20 CreateSourceVoice (float32 extensible), vtbl+0x30
 *                   SetVolume, then voice+0x64 GetState / +0x54 SubmitSourceBuffer
 *                   / +0x4C Start / +0x48 DestroyVoice
 *   FUN_007b2a95  CRI CreateSourceVoice(pp, WAVEFORMATEXTENSIBLE float32
 *                 48 kHz, 0, ratio, voice cb, sends, 0), then SetOutputMatrix
 *   FUN_007b26b7  CRI voice+0x64 GetState(&state), voice+0x54 SubmitSourceBuffer
 *   FUN_007b2638  CRI voice+0x50 Stop(0,0), +0x58 FlushSourceBuffers, +0x48
 *                 DestroyVoice
 *
 * Calling convention: the SDK declares every XAudio2 method __stdcall
 * (STDMETHODCALLTYPE), so `this` is the first *stack* argument, not ECX. The
 * shipped code proves it: "mov eax,[esi] / mov ecx,[eax] / push eax / call
 * [ecx+0x64]". Every entry below is a plain __stdcall function whose first
 * parameter is the object; native32_bridge measures the callee's purge and
 * keeps g_esp in step, so each parameter list must match the place's pushes.
 *
 * The source voice keeps a virtual playback clock (QPC * sample rate *
 * frequency ratio). Buffers retire as their frames elapse, so
 * GetState().BuffersQueued drains and CRI's 4-deep submit ring keeps moving
 * instead of stalling on a voice that never consumes anything. No guest
 * callback is invoked: CRI's OnBufferEnd (FUN_007b2a24) only increments a
 * counter and its feed loop is driven by GetState. M4 hangs a real sink and a
 * device clock here.
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
#include <mmreg.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include "native32.h"
#include "xaudio2_stub.h"

/* ------------------------------------------------------------------ GUIDs
 * Read out of LoK.exe .rdata (single copy of each; evidence in the callsite
 * doc). Kept here rather than from a header because the SDK ships only 2.9. */
const GUID xa2_clsid_xaudio2 =
    { 0x5a508685, 0xa254, 0x4fba, { 0x9b, 0x82, 0x9a, 0x24, 0xb0, 0x03, 0x06, 0xaf } };
const GUID xa2_clsid_xaudio2_debug =
    { 0xdb05ea35, 0x0329, 0x4d4b, { 0xa5, 0x3a, 0x6d, 0xea, 0xd0, 0x3d, 0x38, 0x52 } };
const GUID xa2_iid_ixaudio2 =
    { 0x8bcf1f58, 0x9fe7, 0x4583, { 0x8a, 0xc6, 0xe2, 0xad, 0xc4, 0x65, 0xc8, 0xbb } };

static const GUID xa2_iid_iunknown =
    { 0x00000000, 0x0000, 0x0000, { 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };

/* --------------------------------------------------------------- ABI types
 * Hand-written XAudio2 2.7 layouts: the Windows SDK only ships 2.9 headers
 * (CreateMasteringVoice and GetState take different parameters there), so
 * taking the definitions from the SDK would define the wrong ABI. The offsets
 * in the comments are the ones the guest actually calls. */
typedef struct { UINT32 CreationFlags, InputChannels, InputSampleRate; } xa2_voice_details;
typedef struct { const void *pCurrentBufferContext; UINT32 BuffersQueued; UINT64 SamplesPlayed; } xa2_voice_state;
typedef struct {
    UINT32 Flags, AudioBytes;
    const BYTE *pAudioData;
    UINT32 PlayBegin, PlayLength, LoopBegin, LoopLength, LoopCount;
    const void *pContext;
} xa2_buffer;
typedef struct { UINT32 OutputCount; const void *ppOutputVoices; } xa2_voice_sends;
typedef struct { UINT32 EffectCount; const void *ppEffectDescriptors; } xa2_effect_chain;
typedef struct { UINT32 Type; float Frequency, OneOverQ; } xa2_filter_parameters;
/* XAUDIO2_DEVICE_DETAILS as 2.7 defines it: DeviceID[256] then DisplayName[256],
 * then Role, then the format. Getting this wrong is not cosmetic: the Hw mixer
 * (FUN_0088b0c0) reads OutputFormat.Format.nChannels at offset 0x406 to pick a
 * stereo (3) or 5.1 (0x3f) channel mask. */
typedef struct {
    WCHAR DeviceID[256];
    WCHAR DisplayName[256];
    UINT32 Role;
    WAVEFORMATEXTENSIBLE OutputFormat;
} xa2_device_details;

/* --------------------------------------------------------------- objects */
#define XA2_MAGIC        0x32415858u   /* "XXA2" */
#define XA2_KIND_SOURCE  0x53524356u
#define XA2_KIND_MASTER  0x4D535456u
#define XA2_KIND_SUBMIX  0x534D5856u

#define XA2_MAX_VOICES   32
#define XA2_QUEUE_MAX    8

#define XA2_HR_OK        ((HRESULT)0)
#define XA2_HR_INVALID   ((HRESULT)0x80070057L)   /* E_INVALIDARG */
#define XA2_HR_NOIFACE   ((HRESULT)0x80004002L)   /* E_NOINTERFACE */
#define XA2_HR_NOMEM     ((HRESULT)0x8007000EL)   /* E_OUTOFMEMORY */
#define XA2_HR_NOCLASS   ((HRESULT)0x80040111L)   /* CLASS_E_CLASSNOTAVAILABLE */

typedef void (__stdcall *xa2_fn)(void);   /* one vtable entry, called by the guest */

typedef struct xa2_voice {
    const xa2_fn *vtbl;
    UINT32 magic, kind;
    UINT32 rate, channels, block_align;
    float ratio, volume;
    int running;
    UINT32 qhead, qcount;
    UINT64 played;         /* frames retired so far */
    UINT64 base;           /* frames at t0, start of the current run */
    UINT64 t0;             /* QPC at start of the current run */
    UINT64 next_end;       /* frame cursor for the next submitted buffer */
    const void *last_ctx;
    struct { UINT64 end; const void *ctx; } q[XA2_QUEUE_MAX];
} xa2_voice;

typedef struct {
    const xa2_fn *vtbl;
    UINT32 magic, refs;
    UINT32 initialized;
    UINT32 engine_cb;         /* guest IXAudio2EngineCallback* - stored, never called */
    xa2_voice *master;
} xa2_device;

static xa2_device g_device;
static xa2_voice g_voices[XA2_MAX_VOICES];
static xa2_stub_stats g_stats;
static int g_verbose = 1;
static LONGLONG g_qpf = 1;

/* ------------------------------------------------------------- plumbing */
static CRITICAL_SECTION g_lock;
static volatile LONG g_lock_state;      /* 0 = cold, 1 = starting, 2 = ready */

static void xa2_lock(void) {
    if (g_lock_state != 2) {
        if (InterlockedCompareExchange(&g_lock_state, 1, 0) == 0) {
            InitializeCriticalSection(&g_lock);
            if (g_qpf <= 1) {
                LARGE_INTEGER f;
                QueryPerformanceFrequency(&f);
                g_qpf = f.QuadPart ? f.QuadPart : 1;
            }
            InterlockedExchange(&g_lock_state, 2);
        } else {
            while (g_lock_state != 2) SwitchToThread();
        }
    }
    EnterCriticalSection(&g_lock);
}
static void xa2_unlock(void) { LeaveCriticalSection(&g_lock); }

void xa2_stub_set_verbose(int level) { g_verbose = level; }

void xa2_stub_get_stats(xa2_stub_stats *out) {
    if (!out) return;
    xa2_lock();
    *out = g_stats;
    xa2_unlock();
}

static void xa2_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("[xa2] ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    fflush(stderr);
    va_end(ap);
}

/* A guest pointer we are about to dereference (CLSID, WAVEFORMATEX, buffer).
 * The guest runs in this process, so the only question is whether the page is
 * committed and readable. */
static int xa2_readable(const void *p, size_t bytes) {
    MEMORY_BASIC_INFORMATION mbi;
    const char *c = (const char *)p;
    if (!p || (uintptr_t)p < 0x10000) return 0;
    if (!VirtualQuery(c, &mbi, sizeof mbi)) return 0;
    if (mbi.State != MEM_COMMIT) return 0;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return 0;
    return (size_t)((const char *)mbi.BaseAddress + mbi.RegionSize - c) >= bytes;
}

/* ------------------------------------------------------------ virtual clock */
static UINT64 xa2_voice_now(const xa2_voice *v) {
    LARGE_INTEGER t;
    double seconds, frames;
    if (!v->running) return v->played;
    QueryPerformanceCounter(&t);
    seconds = (double)(t.QuadPart - (LONGLONG)v->t0) / (double)g_qpf;
    if (seconds < 0.0) seconds = 0.0;
    frames = seconds * (double)v->rate * (double)(v->ratio > 0.0f ? v->ratio : 1.0f);
    return v->base + (UINT64)(frames + 0.5);
}

static void xa2_voice_retire(xa2_voice *v, UINT64 now) {
    while (v->qcount) {
        UINT64 end = v->q[v->qhead].end;
        if (now < end) break;
        g_stats.frames_played += end - v->played;
        v->played = end;
        v->last_ctx = v->q[v->qhead].ctx;
        v->qhead = (v->qhead + 1) % XA2_QUEUE_MAX;
        v->qcount--;
    }
}

static xa2_voice *xa2_voice_slot(void) {
    int i;
    for (i = 0; i < XA2_MAX_VOICES; i++)
        if (g_voices[i].magic != XA2_MAGIC) return &g_voices[i];
    return NULL;
}

static int xa2_voice_valid(const void *self) {
    const xa2_voice *v = (const xa2_voice *)self;
    return v && xa2_readable(v, sizeof *v) && v->magic == XA2_MAGIC;
}
static int xa2_device_valid(const void *self) {
    return self == &g_device && g_device.magic == XA2_MAGIC;
}

/* ------------------------------------------------------- method prototypes
 * Written out so the vtable arrays below can name them before their bodies.
 * The parameter counts are the call-site push counts from the callsite doc:
 *   dev_*      IXAudio2         slot * 4
 *   voice_*    IXAudio2Voice   slot * 4  (0x00..0x48)
 *   src_*      IXAudio2SourceVoice additions (0x4C..0x70)
 */
static ULONG   __stdcall ukn_AddRef(void *self);
static ULONG   __stdcall ukn_Release(void *self);

static HRESULT __stdcall dev_QueryInterface(xa2_device *self, const GUID *riid, void **ppv);
static HRESULT __stdcall dev_GetDeviceCount(xa2_device *self, UINT32 *count);
static HRESULT __stdcall dev_GetDeviceDetails(xa2_device *self, UINT32 index, xa2_device_details *details);
static HRESULT __stdcall dev_Initialize(xa2_device *self, UINT32 flags, UINT32 processor);
static HRESULT __stdcall dev_RegisterForCallbacks(xa2_device *self, const void *cb);
static HRESULT __stdcall dev_UnregisterForCallbacks(xa2_device *self, const void *cb);
static HRESULT __stdcall dev_CreateSourceVoice(xa2_device *self, void **pp, const WAVEFORMATEX *fmt,
                                               UINT32 flags, float max_ratio, const void *cb,
                                               const xa2_voice_sends *sends, const xa2_effect_chain *chain);
static HRESULT __stdcall dev_CreateSubmixVoice(xa2_device *self, void **pp, UINT32 in_ch, UINT32 in_rate,
                                               UINT32 flags, UINT32 stage, const xa2_voice_sends *sends,
                                               const xa2_effect_chain *chain);
static HRESULT __stdcall dev_CreateMasteringVoice(xa2_device *self, void **pp, UINT32 in_ch, UINT32 in_rate,
                                                  UINT32 flags, const void *device_id,
                                                  const xa2_effect_chain *chain);
static HRESULT __stdcall dev_StartEngine(xa2_device *self);
static HRESULT __stdcall dev_StopEngine(xa2_device *self);
static HRESULT __stdcall dev_CommitChanges(xa2_device *self, UINT32 opset);
static void    __stdcall dev_GetPerformanceData(xa2_device *self, void *data);
static void    __stdcall dev_SetDebugConfiguration(xa2_device *self, const void *cfg, const void *reserved);

static HRESULT __stdcall voice_GetVoiceDetails(xa2_voice *self, xa2_voice_details *details);
static HRESULT __stdcall voice_SetOutputVoices(xa2_voice *self, const xa2_voice_sends *sends);
static HRESULT __stdcall voice_SetEffectChain(xa2_voice *self, const xa2_effect_chain *chain);
static HRESULT __stdcall voice_EnableEffect(xa2_voice *self, UINT32 index, UINT32 opset);
static HRESULT __stdcall voice_DisableEffect(xa2_voice *self, UINT32 index, UINT32 opset);
static void    __stdcall voice_GetEffectState(xa2_voice *self, UINT32 index, BOOL *enabled);
static HRESULT __stdcall voice_SetEffectParameters(xa2_voice *self, UINT32 index, const void *params,
                                                   UINT32 size, UINT32 opset);
static HRESULT __stdcall voice_GetEffectParameters(xa2_voice *self, UINT32 index, void *params, UINT32 size);
static HRESULT __stdcall voice_SetFilterParameters(xa2_voice *self, const xa2_filter_parameters *params, UINT32 opset);
static void    __stdcall voice_GetFilterParameters(xa2_voice *self, xa2_filter_parameters *params);
static HRESULT __stdcall voice_SetOutputFilterParameters(xa2_voice *self, const void *dest,
                                                         const xa2_filter_parameters *params, UINT32 opset);
static void    __stdcall voice_GetOutputFilterParameters(xa2_voice *self, const void *dest,
                                                         xa2_filter_parameters *params);
static HRESULT __stdcall voice_SetVolume(xa2_voice *self, float volume, UINT32 opset);
static void    __stdcall voice_GetVolume(xa2_voice *self, float *volume);
static HRESULT __stdcall voice_SetChannelVolumes(xa2_voice *self, UINT32 channels, const float *volumes, UINT32 opset);
static void    __stdcall voice_GetChannelVolumes(xa2_voice *self, UINT32 channels, float *volumes);
static HRESULT __stdcall voice_SetOutputMatrix(xa2_voice *self, const void *dest, UINT32 src_ch,
                                               UINT32 dst_ch, const float *levels, UINT32 opset);
static void    __stdcall voice_GetOutputMatrix(xa2_voice *self, const void *dest, UINT32 src_ch,
                                               UINT32 dst_ch, float *levels);
static void    __stdcall voice_DestroyVoice(xa2_voice *self);

static HRESULT __stdcall src_Start(xa2_voice *self, UINT32 flags, UINT32 opset);
static HRESULT __stdcall src_Stop(xa2_voice *self, UINT32 flags, UINT32 opset);
static HRESULT __stdcall src_SubmitSourceBuffer(xa2_voice *self, const xa2_buffer *buf, const void *wma);
static HRESULT __stdcall src_FlushSourceBuffers(xa2_voice *self);
static HRESULT __stdcall src_Discontinuity(xa2_voice *self);
static HRESULT __stdcall src_ExitLoop(xa2_voice *self, UINT32 opset);
static void    __stdcall src_GetState(xa2_voice *self, xa2_voice_state *state);
static HRESULT __stdcall src_SetFrequencyRatio(xa2_voice *self, float ratio, UINT32 opset);
static void    __stdcall src_GetFrequencyRatio(xa2_voice *self, float *ratio);
static HRESULT __stdcall src_SetSourceSampleRate(xa2_voice *self, UINT32 rate);

/* ----------------------------------------------------------------- vtables
 * Offsets are index*4 and follow XAudio2 2.7 exactly. Each line was checked
 * against the call site listed in docs/xaudio2_callsites.md. */
#define VT(x) (xa2_fn)(x)

static const xa2_fn g_device_vtbl[16] = {
    /* 0x00 */ VT(dev_QueryInterface),
    /* 0x04 */ VT(ukn_AddRef),
    /* 0x08 */ VT(ukn_Release),
    /* 0x0C */ VT(dev_GetDeviceCount),
    /* 0x10 */ VT(dev_GetDeviceDetails),
    /* 0x14 */ VT(dev_Initialize),
    /* 0x18 */ VT(dev_RegisterForCallbacks),
    /* 0x1C */ VT(dev_UnregisterForCallbacks),
    /* 0x20 */ VT(dev_CreateSourceVoice),
    /* 0x24 */ VT(dev_CreateSubmixVoice),
    /* 0x28 */ VT(dev_CreateMasteringVoice),
    /* 0x2C */ VT(dev_StartEngine),
    /* 0x30 */ VT(dev_StopEngine),
    /* 0x34 */ VT(dev_CommitChanges),
    /* 0x38 */ VT(dev_GetPerformanceData),
    /* 0x3C */ VT(dev_SetDebugConfiguration),
};

static const xa2_fn g_voice_vtbl_mastering[19] = {
    /* 0x00 */ VT(voice_GetVoiceDetails),
    /* 0x04 */ VT(voice_SetOutputVoices),
    /* 0x08 */ VT(voice_SetEffectChain),
    /* 0x0C */ VT(voice_EnableEffect),
    /* 0x10 */ VT(voice_DisableEffect),
    /* 0x14 */ VT(voice_GetEffectState),
    /* 0x18 */ VT(voice_SetEffectParameters),
    /* 0x1C */ VT(voice_GetEffectParameters),
    /* 0x20 */ VT(voice_SetFilterParameters),
    /* 0x24 */ VT(voice_GetFilterParameters),
    /* 0x28 */ VT(voice_SetOutputFilterParameters),
    /* 0x2C */ VT(voice_GetOutputFilterParameters),
    /* 0x30 */ VT(voice_SetVolume),
    /* 0x34 */ VT(voice_GetVolume),
    /* 0x38 */ VT(voice_SetChannelVolumes),
    /* 0x3C */ VT(voice_GetChannelVolumes),
    /* 0x40 */ VT(voice_SetOutputMatrix),
    /* 0x44 */ VT(voice_GetOutputMatrix),
    /* 0x48 */ VT(voice_DestroyVoice),
};

static const xa2_fn g_voice_vtbl_source[29] = {
    /* 0x00 */ VT(voice_GetVoiceDetails),
    /* 0x04 */ VT(voice_SetOutputVoices),
    /* 0x08 */ VT(voice_SetEffectChain),
    /* 0x0C */ VT(voice_EnableEffect),
    /* 0x10 */ VT(voice_DisableEffect),
    /* 0x14 */ VT(voice_GetEffectState),
    /* 0x18 */ VT(voice_SetEffectParameters),
    /* 0x1C */ VT(voice_GetEffectParameters),
    /* 0x20 */ VT(voice_SetFilterParameters),
    /* 0x24 */ VT(voice_GetFilterParameters),
    /* 0x28 */ VT(voice_SetOutputFilterParameters),
    /* 0x2C */ VT(voice_GetOutputFilterParameters),
    /* 0x30 */ VT(voice_SetVolume),
    /* 0x34 */ VT(voice_GetVolume),
    /* 0x38 */ VT(voice_SetChannelVolumes),
    /* 0x3C */ VT(voice_GetChannelVolumes),
    /* 0x40 */ VT(voice_SetOutputMatrix),
    /* 0x44 */ VT(voice_GetOutputMatrix),
    /* 0x48 */ VT(voice_DestroyVoice),
    /* 0x4C */ VT(src_Start),
    /* 0x50 */ VT(src_Stop),
    /* 0x54 */ VT(src_SubmitSourceBuffer),
    /* 0x58 */ VT(src_FlushSourceBuffers),
    /* 0x5C */ VT(src_Discontinuity),
    /* 0x60 */ VT(src_ExitLoop),
    /* 0x64 */ VT(src_GetState),
    /* 0x68 */ VT(src_SetFrequencyRatio),
    /* 0x6C */ VT(src_GetFrequencyRatio),
    /* 0x70 */ VT(src_SetSourceSampleRate),
};

/* ---------------------------------------------------------------- helpers */
static xa2_voice *xa2_voice_alloc(UINT32 kind) {
    xa2_voice *v = xa2_voice_slot();
    if (!v) return NULL;
    memset(v, 0, sizeof *v);
    v->vtbl = (kind == XA2_KIND_SOURCE) ? g_voice_vtbl_source : g_voice_vtbl_mastering;
    v->magic = XA2_MAGIC;
    v->kind = kind;
    v->volume = 1.0f;
    v->ratio = 1.0f;
    v->rate = 48000;
    g_stats.voices_created++;
    g_stats.voices_live++;
    return v;
}

static void xa2_voice_kill(xa2_voice *v) {
    if (!v || v->magic != XA2_MAGIC) return;
    if (g_device.master == v) g_device.master = NULL;
    v->magic = 0;
    g_stats.voices_destroyed++;
    if (g_stats.voices_live) g_stats.voices_live--;
}

/* --------------------------------------------------------- IUnknown parts */
static ULONG __stdcall ukn_AddRef(void *self) {
    ULONG r = 0;
    xa2_lock();
    if (xa2_device_valid(self)) r = ++g_device.refs;
    else if (xa2_voice_valid(self)) r = 2;          /* voices are not refcounted */
    xa2_unlock();
    return r;
}

static ULONG __stdcall ukn_Release(void *self) {
    ULONG r = 0;
    xa2_lock();
    if (xa2_device_valid(self)) {
        r = g_device.refs ? --g_device.refs : 0;
    } else if (xa2_voice_valid(self)) {
        xa2_voice_kill((xa2_voice *)self);
    }
    xa2_unlock();
    if (g_verbose > 1) xa2_log("Release(%p) -> %lu", self, r);
    return r;
}

/* ------------------------------------------------------- IXAudio2 methods */
static HRESULT __stdcall dev_QueryInterface(xa2_device *self, const GUID *riid, void **ppv) {
    if (!xa2_device_valid(self) || !ppv) return XA2_HR_INVALID;
    if (riid && (IsEqualGUID(riid, &xa2_iid_ixaudio2) || IsEqualGUID(riid, &xa2_iid_iunknown))) {
        *ppv = self;
        InterlockedIncrement((LONG *)&g_device.refs);
        return XA2_HR_OK;
    }
    *ppv = NULL;
    return XA2_HR_NOIFACE;
}

static HRESULT __stdcall dev_GetDeviceCount(xa2_device *self, UINT32 *count) {
    if (!xa2_device_valid(self) || !count) return XA2_HR_INVALID;
    /* CRI refuses 0 devices ("No sound devices are available",
     * W2010122202) and falls back to its silent module. */
    *count = 1;
    if (g_verbose > 1) xa2_log("GetDeviceCount -> 1");
    return XA2_HR_OK;
}

static HRESULT __stdcall dev_GetDeviceDetails(xa2_device *self, UINT32 index, xa2_device_details *details) {
    if (!xa2_device_valid(self) || !details) return XA2_HR_INVALID;
    if (index != 0) return XA2_HR_INVALID;
    memset(details, 0, sizeof *details);
    wcsncpy(details->DeviceID, L"korra-recomp-silent", 255);
    wcsncpy(details->DisplayName, L"korra-recomp silent device", 255);
    details->Role = 0;   /* GlobalDefaultDevice */
    details->OutputFormat.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    details->OutputFormat.Format.nChannels = 2;
    details->OutputFormat.Format.nSamplesPerSec = 48000;
    details->OutputFormat.Format.wBitsPerSample = 32;
    details->OutputFormat.Format.nBlockAlign = 8;
    details->OutputFormat.Format.nAvgBytesPerSec = 48000 * 8;
    details->OutputFormat.Format.cbSize = 22;
    details->OutputFormat.Samples.wValidBitsPerSample = 32;
    details->OutputFormat.dwChannelMask = 3;   /* FL | FR */
    details->OutputFormat.SubFormat.Data1 = 0x00000003;
    details->OutputFormat.SubFormat.Data2 = 0x0000;
    details->OutputFormat.SubFormat.Data3 = 0x0010;
    details->OutputFormat.SubFormat.Data4[0] = 0x80;
    details->OutputFormat.SubFormat.Data4[1] = 0x00;
    details->OutputFormat.SubFormat.Data4[2] = 0x00;
    details->OutputFormat.SubFormat.Data4[3] = 0xaa;
    details->OutputFormat.SubFormat.Data4[4] = 0x00;
    details->OutputFormat.SubFormat.Data4[5] = 0x38;
    details->OutputFormat.SubFormat.Data4[6] = 0x9b;
    details->OutputFormat.SubFormat.Data4[7] = 0x71;
    if (g_verbose > 1) xa2_log("GetDeviceDetails(0) -> silent 2ch 48k");
    return XA2_HR_OK;
}

static HRESULT __stdcall dev_Initialize(xa2_device *self, UINT32 flags, UINT32 processor) {
    if (!xa2_device_valid(self)) return XA2_HR_INVALID;
    g_device.initialized = 1;
    if (g_verbose) xa2_log("Initialize(flags=0x%X, processor=0x%X) -> 0", flags, processor);
    return XA2_HR_OK;
}

static HRESULT __stdcall dev_RegisterForCallbacks(xa2_device *self, const void *cb) {
    if (!xa2_device_valid(self)) return XA2_HR_INVALID;
    g_device.engine_cb = (UINT32)(uintptr_t)cb;
    if (g_verbose) xa2_log("RegisterForCallbacks(%p) -> 0 (never called back)", cb);
    return XA2_HR_OK;
}

static HRESULT __stdcall dev_UnregisterForCallbacks(xa2_device *self, const void *cb) {
    if (!xa2_device_valid(self)) return XA2_HR_INVALID;
    if (g_device.engine_cb == (UINT32)(uintptr_t)cb) g_device.engine_cb = 0;
    if (g_verbose) xa2_log("UnregisterForCallbacks(%p) -> 0", cb);
    return XA2_HR_OK;
}

static HRESULT __stdcall dev_CreateMasteringVoice(xa2_device *self, void **pp, UINT32 in_ch,
                                                  UINT32 in_rate, UINT32 flags, const void *device_id,
                                                  const xa2_effect_chain *chain) {
    xa2_voice *v;
    (void)device_id; (void)chain; (void)flags;
    if (!xa2_device_valid(self) || !pp) return XA2_HR_INVALID;
    *pp = NULL;
    xa2_lock();
    if (g_device.master) { *pp = g_device.master; xa2_unlock(); return XA2_HR_OK; }
    v = xa2_voice_alloc(XA2_KIND_MASTER);
    if (!v) { xa2_unlock(); xa2_log("CreateMasteringVoice: no free voice slot"); return XA2_HR_NOMEM; }
    v->channels = in_ch ? in_ch : 2;      /* guest asks for 0 = device default */
    v->rate = in_rate ? in_rate : 48000;
    v->block_align = v->channels * 4;
    g_device.master = v;
    *pp = v;
    xa2_unlock();
    if (g_verbose)
        xa2_log("CreateMasteringVoice(in=%u ch, %u Hz) -> %u ch (silent)",
                in_ch, in_rate, v->channels);
    return XA2_HR_OK;
}

static HRESULT __stdcall dev_CreateSubmixVoice(xa2_device *self, void **pp, UINT32 in_ch, UINT32 in_rate,
                                               UINT32 flags, UINT32 stage, const xa2_voice_sends *sends,
                                               const xa2_effect_chain *chain) {
    xa2_voice *v;
    (void)stage; (void)sends; (void)chain; (void)flags;
    if (!xa2_device_valid(self) || !pp) return XA2_HR_INVALID;
    xa2_lock();
    v = xa2_voice_alloc(XA2_KIND_SUBMIX);
    if (!v) { xa2_unlock(); return XA2_HR_NOMEM; }
    v->channels = in_ch ? in_ch : 2;
    v->rate = in_rate ? in_rate : 48000;
    v->block_align = v->channels * 4;
    *pp = v;
    xa2_unlock();
    if (g_verbose) xa2_log("CreateSubmixVoice(%u ch, %u Hz, stage %u) -> silent", in_ch, in_rate, stage);
    return XA2_HR_OK;
}

static HRESULT __stdcall dev_CreateSourceVoice(xa2_device *self, void **pp, const WAVEFORMATEX *fmt,
                                               UINT32 flags, float max_ratio, const void *cb,
                                               const xa2_voice_sends *sends, const xa2_effect_chain *chain) {
    xa2_voice *v;
    (void)sends; (void)chain; (void)flags; (void)cb;
    if (!xa2_device_valid(self) || !pp) return XA2_HR_INVALID;
    *pp = NULL;
    if (!fmt || !xa2_readable(fmt, sizeof(WAVEFORMATEX))) {
        xa2_log("CreateSourceVoice: unreadable WAVEFORMATEX %p", (const void *)fmt);
        return XA2_HR_INVALID;
    }

    xa2_lock();
    v = xa2_voice_alloc(XA2_KIND_SOURCE);
    if (!v) { xa2_unlock(); xa2_log("CreateSourceVoice: no free voice slot"); return XA2_HR_NOMEM; }
    v->rate = fmt->nSamplesPerSec ? fmt->nSamplesPerSec : 48000;
    v->channels = fmt->nChannels ? fmt->nChannels : 2;
    v->block_align = fmt->nBlockAlign ? fmt->nBlockAlign : v->channels * 4;
    *pp = v;
    xa2_unlock();
    if (g_verbose)
        xa2_log("CreateSourceVoice(%u ch, %u Hz, %u bits, tag 0x%04X, maxratio %.3f) -> #%d (silent)",
                fmt->nChannels, fmt->nSamplesPerSec, fmt->wBitsPerSample, fmt->wFormatTag,
                max_ratio, (int)(v - g_voices));
    return XA2_HR_OK;
}

static HRESULT __stdcall dev_StartEngine(xa2_device *self) {
    if (!xa2_device_valid(self)) return XA2_HR_INVALID;
    if (g_verbose > 1) xa2_log("StartEngine() -> 0");
    return XA2_HR_OK;
}
static HRESULT __stdcall dev_StopEngine(xa2_device *self) {
    if (!xa2_device_valid(self)) return XA2_HR_INVALID;
    if (g_verbose) xa2_log("StopEngine()");
    return XA2_HR_OK;
}
static HRESULT __stdcall dev_CommitChanges(xa2_device *self, UINT32 opset) {
    if (!xa2_device_valid(self)) return XA2_HR_INVALID;
    if (g_verbose > 1) xa2_log("CommitChanges(0x%X) -> 0", opset);
    return XA2_HR_OK;
}
static void __stdcall dev_GetPerformanceData(xa2_device *self, void *data) {
    (void)self;
    /* XAUDIO2_PERFORMANCE_DATA (2.7) is 0x60 bytes; only write if the guest's
     * buffer is really there. */
    if (data && xa2_readable(data, 0x60)) memset(data, 0, 0x60);
}
static void __stdcall dev_SetDebugConfiguration(xa2_device *self, const void *cfg, const void *reserved) {
    (void)self; (void)cfg; (void)reserved;
}

/* ---------------------------------------------------------- IXAudio2Voice */
static HRESULT __stdcall voice_GetVoiceDetails(xa2_voice *self, xa2_voice_details *details) {
    if (!xa2_voice_valid(self) || !details) return XA2_HR_INVALID;
    details->CreationFlags = 0;
    details->InputChannels = self->channels;
    details->InputSampleRate = self->rate;
    return XA2_HR_OK;
}
static HRESULT __stdcall voice_SetOutputVoices(xa2_voice *self, const xa2_voice_sends *sends) {
    (void)sends;
    return xa2_voice_valid(self) ? XA2_HR_OK : XA2_HR_INVALID;
}
static HRESULT __stdcall voice_SetEffectChain(xa2_voice *self, const xa2_effect_chain *chain) {
    (void)chain;
    return xa2_voice_valid(self) ? XA2_HR_OK : XA2_HR_INVALID;
}
static HRESULT __stdcall voice_EnableEffect(xa2_voice *self, UINT32 index, UINT32 opset) {
    (void)index; (void)opset;
    return xa2_voice_valid(self) ? XA2_HR_OK : XA2_HR_INVALID;
}
static HRESULT __stdcall voice_DisableEffect(xa2_voice *self, UINT32 index, UINT32 opset) {
    (void)index; (void)opset;
    return xa2_voice_valid(self) ? XA2_HR_OK : XA2_HR_INVALID;
}
static void __stdcall voice_GetEffectState(xa2_voice *self, UINT32 index, BOOL *enabled) {
    (void)self; (void)index;
    if (enabled) *enabled = FALSE;
}
static HRESULT __stdcall voice_SetEffectParameters(xa2_voice *self, UINT32 index, const void *params,
                                                   UINT32 size, UINT32 opset) {
    (void)index; (void)params; (void)size; (void)opset;
    return xa2_voice_valid(self) ? XA2_HR_OK : XA2_HR_INVALID;
}
static HRESULT __stdcall voice_GetEffectParameters(xa2_voice *self, UINT32 index, void *params, UINT32 size) {
    (void)index;
    if (params && size && xa2_readable(params, size)) memset(params, 0, size);
    return xa2_voice_valid(self) ? XA2_HR_OK : XA2_HR_INVALID;
}
static HRESULT __stdcall voice_SetFilterParameters(xa2_voice *self, const xa2_filter_parameters *params, UINT32 opset) {
    (void)params; (void)opset;
    return xa2_voice_valid(self) ? XA2_HR_OK : XA2_HR_INVALID;
}
static void __stdcall voice_GetFilterParameters(xa2_voice *self, xa2_filter_parameters *params) {
    (void)self;
    if (params) memset(params, 0, sizeof *params);
}
static HRESULT __stdcall voice_SetOutputFilterParameters(xa2_voice *self, const void *dest,
                                                         const xa2_filter_parameters *params, UINT32 opset) {
    (void)dest; (void)params; (void)opset;
    return xa2_voice_valid(self) ? XA2_HR_OK : XA2_HR_INVALID;
}
static void __stdcall voice_GetOutputFilterParameters(xa2_voice *self, const void *dest,
                                                      xa2_filter_parameters *params) {
    (void)self; (void)dest;
    if (params) memset(params, 0, sizeof *params);
}
static HRESULT __stdcall voice_SetVolume(xa2_voice *self, float volume, UINT32 opset) {
    (void)opset;
    if (!xa2_voice_valid(self)) return XA2_HR_INVALID;
    xa2_lock();
    self->volume = volume;
    xa2_unlock();
    return XA2_HR_OK;
}
static void __stdcall voice_GetVolume(xa2_voice *self, float *volume) {
    if (volume) *volume = xa2_voice_valid(self) ? self->volume : 0.0f;
}
static HRESULT __stdcall voice_SetChannelVolumes(xa2_voice *self, UINT32 channels, const float *volumes, UINT32 opset) {
    (void)channels; (void)volumes; (void)opset;
    return xa2_voice_valid(self) ? XA2_HR_OK : XA2_HR_INVALID;
}
static void __stdcall voice_GetChannelVolumes(xa2_voice *self, UINT32 channels, float *volumes) {
    UINT32 i;
    (void)self;
    for (i = 0; volumes && i < channels; i++) volumes[i] = 1.0f;
}
static HRESULT __stdcall voice_SetOutputMatrix(xa2_voice *self, const void *dest, UINT32 src_ch,
                                               UINT32 dst_ch, const float *levels, UINT32 opset) {
    (void)dest; (void)src_ch; (void)dst_ch; (void)levels; (void)opset;
    /* CRI computes the downmix itself (FUN_007b27e2) and installs it here. */
    return xa2_voice_valid(self) ? XA2_HR_OK : XA2_HR_INVALID;
}
static void __stdcall voice_GetOutputMatrix(xa2_voice *self, const void *dest, UINT32 src_ch,
                                            UINT32 dst_ch, float *levels) {
    UINT32 i, n = src_ch * dst_ch;
    (void)self; (void)dest;
    for (i = 0; levels && i < n; i++) levels[i] = 0.0f;
}
static void __stdcall voice_DestroyVoice(xa2_voice *self) {
    if (!xa2_voice_valid(self)) return;
    xa2_lock();
    if (g_verbose) xa2_log("DestroyVoice(%p)", (void *)self);
    xa2_voice_kill(self);
    xa2_unlock();
}

/* ------------------------------------------------- IXAudio2SourceVoice */
static HRESULT __stdcall src_Start(xa2_voice *self, UINT32 flags, UINT32 opset) {
    LARGE_INTEGER t;
    (void)flags; (void)opset;
    if (!xa2_voice_valid(self)) return XA2_HR_INVALID;
    xa2_lock();
    if (!self->running) {
        self->base = self->played;
        QueryPerformanceCounter(&t);
        self->t0 = (UINT64)t.QuadPart;
        self->running = 1;
    }
    xa2_unlock();
    if (g_verbose > 1) xa2_log("Start(%p) -> 0", (void *)self);
    return XA2_HR_OK;
}

static HRESULT __stdcall src_Stop(xa2_voice *self, UINT32 flags, UINT32 opset) {
    (void)flags; (void)opset;
    if (!xa2_voice_valid(self)) return XA2_HR_INVALID;
    xa2_lock();
    if (self->running) {
        self->played = xa2_voice_now(self);
        self->running = 0;
    }
    xa2_unlock();
    if (g_verbose > 1) xa2_log("Stop(%p) -> 0", (void *)self);
    return XA2_HR_OK;
}

static HRESULT __stdcall src_SubmitSourceBuffer(xa2_voice *self, const xa2_buffer *buf, const void *wma) {
    UINT64 frames, start, end;
    UINT32 total, play_begin, play_len;
    (void)wma;
    if (!xa2_voice_valid(self) || !buf) return XA2_HR_INVALID;
    if (!xa2_readable(buf, sizeof *buf)) return XA2_HR_INVALID;

    total = self->block_align ? buf->AudioBytes / self->block_align : 0;
    play_begin = buf->PlayBegin;
    play_len = buf->PlayLength;
    if (play_begin > total) play_begin = total;
    if (play_len == 0) play_len = total - play_begin;
    if (play_len > total - play_begin) play_len = total - play_begin;
    frames = play_len ? play_len : 1;   /* a zero-length buffer must still retire */

    xa2_lock();
    if (self->qcount == XA2_QUEUE_MAX) {
        /* Never happens with CRI's 4-deep ring; drop the oldest to stay alive. */
        self->qhead = (self->qhead + 1) % XA2_QUEUE_MAX;
        self->qcount--;
        if (g_verbose) xa2_log("SubmitSourceBuffer: queue overflow, dropped one");
    }
    start = self->next_end > self->played ? self->next_end : self->played;
    end = start + frames;
    self->q[(self->qhead + self->qcount) % XA2_QUEUE_MAX].end = end;
    self->q[(self->qhead + self->qcount) % XA2_QUEUE_MAX].ctx = buf->pContext;
    self->qcount++;
    self->next_end = end;
    g_stats.buffers_submitted++;
    xa2_unlock();

    if (g_verbose > 1)
        xa2_log("SubmitSourceBuffer(%p): %u bytes, %u frames, ctx=%p -> 0",
                (void *)self, buf->AudioBytes, (unsigned)frames, buf->pContext);
    return XA2_HR_OK;
}

static HRESULT __stdcall src_FlushSourceBuffers(xa2_voice *self) {
    if (!xa2_voice_valid(self)) return XA2_HR_INVALID;
    xa2_lock();
    self->qhead = self->qcount = 0;
    self->next_end = self->played;
    self->last_ctx = NULL;
    xa2_unlock();
    if (g_verbose > 1) xa2_log("FlushSourceBuffers(%p) -> 0", (void *)self);
    return XA2_HR_OK;
}

static HRESULT __stdcall src_Discontinuity(xa2_voice *self) {
    if (!xa2_voice_valid(self)) return XA2_HR_INVALID;
    xa2_lock();
    if (!self->qcount) self->next_end = self->played;
    xa2_unlock();
    if (g_verbose > 1) xa2_log("Discontinuity(%p) -> 0", (void *)self);
    return XA2_HR_OK;
}

static HRESULT __stdcall src_ExitLoop(xa2_voice *self, UINT32 opset) {
    (void)opset;
    return xa2_voice_valid(self) ? XA2_HR_OK : XA2_HR_INVALID;
}

static void __stdcall src_GetState(xa2_voice *self, xa2_voice_state *state) {
    UINT64 now;
    if (!state) return;
    if (!xa2_voice_valid(self)) { memset(state, 0, sizeof *state); return; }
    xa2_lock();
    now = xa2_voice_now(self);
    xa2_voice_retire(self, now);
    /* SamplesPlayed counts frames played since the voice started, including the
     * part of the buffer that is playing right now; with nothing queued it must
     * freeze at the cursor (a started XAudio2 voice does not count silence). */
    if (now > self->next_end) now = self->next_end;
    state->pCurrentBufferContext = self->qcount ? self->q[self->qhead].ctx : self->last_ctx;
    state->BuffersQueued = self->qcount;
    state->SamplesPlayed = now;
    xa2_unlock();
}

static HRESULT __stdcall src_SetFrequencyRatio(xa2_voice *self, float ratio, UINT32 opset) {
    LARGE_INTEGER t;
    (void)opset;
    if (!xa2_voice_valid(self) || !(ratio > 0.0f)) return XA2_HR_INVALID;
    xa2_lock();
    /* Re-base the clock so a ratio change does not move the cursor backwards. */
    self->base = xa2_voice_now(self);
    QueryPerformanceCounter(&t);
    self->t0 = (UINT64)t.QuadPart;
    self->ratio = ratio;
    xa2_unlock();
    return XA2_HR_OK;
}
static void __stdcall src_GetFrequencyRatio(xa2_voice *self, float *ratio) {
    if (ratio) *ratio = xa2_voice_valid(self) ? self->ratio : 1.0f;
}
static HRESULT __stdcall src_SetSourceSampleRate(xa2_voice *self, UINT32 rate) {
    LARGE_INTEGER t;
    if (!xa2_voice_valid(self) || !rate) return XA2_HR_INVALID;
    xa2_lock();
    self->base = xa2_voice_now(self);
    QueryPerformanceCounter(&t);
    self->t0 = (UINT64)t.QuadPart;
    self->rate = rate;
    xa2_unlock();
    return XA2_HR_OK;
}

/* -------------------------------------------------------- factory entry */
int xa2_stub_is_xaudio2_clsid(const GUID *rclsid) {
    if (!rclsid) return 0;
    return IsEqualGUID(rclsid, &xa2_clsid_xaudio2) ||
           IsEqualGUID(rclsid, &xa2_clsid_xaudio2_debug);
}

HRESULT xa2_stub_create_instance(const GUID *rclsid, DWORD cls_context, const GUID *riid, void **ppv) {
    if (!ppv) return XA2_HR_INVALID;
    *ppv = NULL;
    if (!xa2_stub_is_xaudio2_clsid(rclsid)) return XA2_HR_NOCLASS;

    xa2_lock();
    g_stats.cocreate_hits++;
    if (!g_device.magic) {
        g_device.magic = XA2_MAGIC;
        g_device.vtbl = g_device_vtbl;
        g_device.refs = 0;
    }
    if (riid && !IsEqualGUID(riid, &xa2_iid_ixaudio2) && !IsEqualGUID(riid, &xa2_iid_iunknown)) {
        xa2_unlock();
        g_stats.cocreate_missed++;
        return XA2_HR_NOIFACE;
    }
    g_device.refs++;
    *ppv = &g_device;
    xa2_unlock();
    if (g_verbose)
        xa2_log("CoCreateInstance(%s, ctx=0x%lX) -> IXAudio2 2.7 (silent, %p)",
                IsEqualGUID(rclsid, &xa2_clsid_xaudio2_debug) ? "XAudio2Debug" : "XAudio2",
                cls_context, (void *)&g_device);
    return XA2_HR_OK;
}

/* ------------------------------------------------ CoCreateInstance shim
 * Runs inside the lifted model: args at g_esp+4, stdcall (5 dwords + ret).
 * Every other CLSID goes to the real ole32, which matters here: the same
 * binary also creates DirectSound8 (0x88aae0) and the WASAPI device
 * enumerator (0x8701f0) through this import. */
static void shim_CoCreateInstance(void) {
    uint32_t *a = (uint32_t *)(uintptr_t)(g_esp + 4);
    const GUID *rclsid = (const GUID *)(uintptr_t)a[0];
    const GUID *riid = (const GUID *)(uintptr_t)a[3];
    void **ppv = (void **)(uintptr_t)a[4];
    HRESULT hr;

    if (xa2_readable(rclsid, sizeof(GUID)) && xa2_stub_is_xaudio2_clsid(rclsid)) {
        hr = xa2_stub_create_instance(rclsid, (DWORD)a[2], riid, ppv);
    } else {
        /* Every other CLSID is the real COM server, and creating one can load a
         * DLL (DirectSound8, Wwise's device enumerator). Doing that while
         * holding the machine lock is the same hazard native_bridge avoids:
         * the loader lock plus a thread that wants the machine is a deadlock.
         * Leave the machine around the call, like native_bridge does. */
        DWORD guest_error = GetLastError();
        g_stats.cocreate_missed++;
        mach_leave();
        SetLastError(guest_error);
        hr = CoCreateInstance((REFCLSID)(uintptr_t)a[0], (LPUNKNOWN)(uintptr_t)a[1],
                              (DWORD)a[2], (REFIID)(uintptr_t)a[3], ppv);
        guest_error = GetLastError();
        mach_enter();
        SetLastError(guest_error);
    }
    g_eax = (uint32_t)hr;
    g_esp += 4 + 5 * 4;   /* ret + the five stdcall arguments */
}

static const native32_shim_t g_shims[] = {
    { "CoCreateInstance", shim_CoCreateInstance },
};

const native32_shim_t *xa2_stub_shims(int *count) {
    if (count) *count = (int)(sizeof g_shims / sizeof g_shims[0]);
    return g_shims;
}
