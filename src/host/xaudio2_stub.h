/*
 * xaudio2_stub.h - silent stand-in for the XAudio2 2.7 COM server.
 *
 * The game never imports XAudio2: CRI NcVoice creates it through
 *   CoCreateInstance(CLSID_XAudio2, NULL, CLSCTX_INPROC_SERVER, IID_IXAudio2, ...)
 * at LoK.exe VA 0x7B22A8 (the only CoCreateInstance site that names XAudio2;
 * see docs/xaudio2_callsites.md for the map and the evidence). So the host
 * shims CoCreateInstance: our CLSIDs get this in-process object, everything
 * else is forwarded to the real ole32.
 *
 * The object implements exactly the XAudio2 2.7 ABI the guest calls - 13
 * IXAudio2 methods plus IUnknown, the 19 IXAudio2Voice methods, and the 10
 * IXAudio2SourceVoice additions - with a virtual playback clock so the engine
 * sees buffers being consumed instead of a voice that never drains.
 *
 * It makes no sound. M4 replaces the clock and adds a real sink behind the
 * same vtable.
 */
#ifndef KORRA_XAUDIO2_STUB_H
#define KORRA_XAUDIO2_STUB_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "native32.h"

/* {5a508685-a254-4fba-9b82-9a24b00306af} - CLSID_XAudio2 (2.6/2.7 API) */
extern const GUID xa2_clsid_xaudio2;
/* {db05ea35-0329-4d4b-a53a-6deadd3d3852} - CLSID_XAudio2Debug (same API) */
extern const GUID xa2_clsid_xaudio2_debug;
/* {8bcf1f58-9fe7-4583-8ac6-e2adc465c8bb} - IID_IXAudio2 (2.7) */
extern const GUID xa2_iid_ixaudio2;

/* 1 = one line per create/destroy (default), 2 = every call, 0 = quiet. */
void xa2_stub_set_verbose(int level);

/* Live counts, for a host status line or the crash report. */
typedef struct {
    unsigned voices_created, voices_destroyed, voices_live;
    unsigned buffers_submitted;
    unsigned long long frames_played;   /* sum over every voice */
    unsigned cocreate_hits, cocreate_missed;
} xa2_stub_stats;
void xa2_stub_get_stats(xa2_stub_stats *out);

/* CoCreateInstance(CLSID_XAudio2 | CLSID_XAudio2Debug, IID_IXAudio2 | IID_IUnknown).
 * Returns S_OK and a reference-counted object, or CLASS_E_CLASSNOTAVAILABLE. */
HRESULT xa2_stub_create_instance(const GUID *rclsid, DWORD cls_context,
                                 const GUID *riid, void **ppv);

/* True when rclsid is one of the two XAudio2 CLSIDs (NULL-safe). */
int xa2_stub_is_xaudio2_clsid(const GUID *rclsid);

/* Import shims this module contributes to native32_bind: CoCreateInstance. */
const native32_shim_t *xa2_stub_shims(int *count);

#endif /* KORRA_XAUDIO2_STUB_H */
