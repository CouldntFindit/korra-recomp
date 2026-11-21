/*
 * wasapi_sink.c - Windows output stage for the Wwise mix (M4).
 *
 * Pull model: one thread owns the device and asks the mixer for exactly the
 * frames the device wants right now. There is no second ring buffer between
 * the mixer and the device, so GetState().BuffersQueued drains at the device's
 * rate instead of a timer's - the M2.9 stub clock is only the fallback when no
 * sink could be opened.
 *
 * Backends, in order of preference:
 *   wasapi  IAudioClient shared mode, event driven. The engine is asked for
 *           float32 stereo 48 kHz with AUTOCONVERTPCM, so the mixer never has
 *           to know the device format. If the driver refuses, the device mix
 *           format is used and converted here.
 *   waveout fallback for machines where the audio service is not reachable.
 *   null    a real-time clock with no device: keeps the mixer and the WAV dump
 *           running on a machine with no sound card at all. Silence in, silence
 *           out - but a non-silent dump proves the engine's mix reached us.
 *
 * KORRA_AUDIO_DUMP=<file.wav> writes the mix as 16-bit PCM 48 kHz stereo. The
 * header is patched once a second, so a run that is killed still leaves a
 * readable file. The log line once a second carries peak/RMS in dBFS, which is
 * what the M4 acceptance checks (RMS > -40 dBFS is "not silence").
 */
#ifdef __ANDROID__
/* The Android twin is aaudio_sink.c; this file is Windows-only. */
#else
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <mmsystem.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "audio_sink.h"

/* Diagnostic: KORRA_AUDIO_CENSUS=1 samples every thread's EIP twice, a few
 * milliseconds apart, once every five seconds. A guest thread that spins shows
 * the same guest address both times; one blocked in a wait shows the same host
 * address; one making progress shows different addresses. This is how an
 * engine-side audio thread that never submits a buffer can be told apart from a
 * mixer that is simply idle. Read-only: nothing is written into the process. */
#ifndef TH32CS_SNAPTHREAD
#include <tlhelp32.h>
#endif

static unsigned long long g_census_next; /* frames at which to sample next */

static korra_audio_render_fn g_render;
static HANDLE g_thread, g_ready, g_wake;
static volatile LONG g_stop, g_live;
static volatile LONG g_started;
static korra_audio_stats g_stats;
static int g_verbose = 1;
static int g_latency_ms = 20;
static int g_want_backend = -1;          /* -1 = auto */
static char g_dump_path[MAX_PATH];
static FILE *g_dump;
static unsigned long long g_dump_frames;
static int g_dump_header_dirty;
static double g_rms_sum;
static unsigned long long g_rms_n;
static double g_rs_pos;
static float g_rs_last[2];
static int g_rs_have;

#ifndef AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
#define AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM 0x80000000
#endif
#ifndef AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY
#define AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY 0x08000000
#endif

/* ------------------------------------------------------------- utilities */
static void sink_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("[sink] ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    fflush(stderr);
    va_end(ap);
}

static double db(double lin) {
    if (lin <= 1e-9) return -180.0;
    return 20.0 * log10(lin);
}

static void stats_block(const float *src, int frames, unsigned long long base) {
    double peak = 0.0, sum = 0.0;
    int i, n = frames * KORRA_AUDIO_CHANNELS;
    for (i = 0; i < n; i++) {
        double a = fabs((double)src[i]);
        if (a > peak) peak = a;
        sum += a * a;
    }
    if (peak > g_stats.peak) g_stats.peak = peak;
    g_rms_sum += sum;
    g_rms_n += (unsigned long long)n;
    g_stats.frames += (unsigned long long)frames;
    g_stats.blocks++;
    if (peak < 3.16e-4) {                 /* -70 dBFS: treat as silence */
        g_stats.silent_frames += (unsigned long long)frames;
    } else if (!g_stats.first_sound_frame) {
        g_stats.first_sound_frame = base;
        sink_log("first audible block at %.3f s (peak %.1f dBFS)",
                 (double)base / KORRA_AUDIO_RATE, db(peak));
    }
}

/* ------------------------------------------------------------- WAV dump */
static void wav_header(FILE *f, unsigned long long frames) {
    DWORD data = (DWORD)(frames * KORRA_AUDIO_CHANNELS * 2);
    DWORD riff = data + 36;
    BYTE h[44];
    memcpy(h, "RIFF", 4);
    h[4] = (BYTE)riff; h[5] = (BYTE)(riff >> 8); h[6] = (BYTE)(riff >> 16); h[7] = (BYTE)(riff >> 24);
    memcpy(h + 8, "WAVEfmt ", 8);
    h[16] = 16; h[17] = 0; h[18] = 0; h[19] = 0;
    h[20] = 1; h[21] = 0;                 /* PCM */
    h[22] = KORRA_AUDIO_CHANNELS; h[23] = 0;
    h[24] = (BYTE)KORRA_AUDIO_RATE; h[25] = (BYTE)(KORRA_AUDIO_RATE >> 8);
    h[26] = (BYTE)(KORRA_AUDIO_RATE >> 16); h[27] = (BYTE)(KORRA_AUDIO_RATE >> 24);
    {
        DWORD byte_rate = KORRA_AUDIO_RATE * KORRA_AUDIO_CHANNELS * 2;
        h[28] = (BYTE)byte_rate; h[29] = (BYTE)(byte_rate >> 8);
        h[30] = (BYTE)(byte_rate >> 16); h[31] = (BYTE)(byte_rate >> 24);
    }
    h[32] = (BYTE)(KORRA_AUDIO_CHANNELS * 2); h[33] = 0;
    h[34] = 16; h[35] = 0;
    memcpy(h + 36, "data", 4);
    h[40] = (BYTE)data; h[41] = (BYTE)(data >> 8);
    h[42] = (BYTE)(data >> 16); h[43] = (BYTE)(data >> 24);
    fseek(f, 0, SEEK_SET);
    fwrite(h, 1, sizeof h, f);
    fseek(f, 0, SEEK_END);
}

static void dump_open(void) {
    const char *p = getenv("KORRA_AUDIO_DUMP");
    if (!p || !*p) return;
    strncpy(g_dump_path, p, sizeof g_dump_path - 1);
    g_dump = fopen(g_dump_path, "wb");
    if (!g_dump) {
        sink_log("KORRA_AUDIO_DUMP: cannot create %s", g_dump_path);
        return;
    }
    wav_header(g_dump, 0);
    g_dump_frames = 0;
    g_dump_header_dirty = 0;
    sink_log("dump -> %s (16-bit PCM, %d Hz, %d ch)", g_dump_path,
             KORRA_AUDIO_RATE, KORRA_AUDIO_CHANNELS);
}

static void dump_write(const float *src, int frames) {
    short *pcm;
    int i, n;
    if (!g_dump) return;
    n = frames * KORRA_AUDIO_CHANNELS;
    pcm = (short *)malloc((size_t)n * sizeof(short));
    if (!pcm) return;
    for (i = 0; i < n; i++) {
        double v = src[i];
        if (v > 1.0) v = 1.0;
        if (v < -1.0) v = -1.0;
        pcm[i] = (short)(v * 32767.0);
    }
    fwrite(pcm, sizeof(short), (size_t)n, g_dump);
    free(pcm);
    g_dump_frames += (unsigned long long)frames;
    g_dump_header_dirty = 1;
}

static void dump_patch(void) {
    if (!g_dump || !g_dump_header_dirty) return;
    wav_header(g_dump, g_dump_frames);
    fflush(g_dump);
    g_dump_header_dirty = 0;
}

static void dump_close(void) {
    if (!g_dump) return;
    wav_header(g_dump, g_dump_frames);
    fclose(g_dump);
    g_dump = NULL;
    sink_log("dump closed: %s (%.3f s)", g_dump_path,
             (double)g_dump_frames / KORRA_AUDIO_RATE);
}

/* --------------------------------------------------------- format convert */
static int fmt_is_float(const WAVEFORMATEX *f) {
    if (f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return 1;
    if (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const WAVEFORMATEXTENSIBLE *x = (const WAVEFORMATEXTENSIBLE *)f;
        return x->SubFormat.Data1 == 3;
    }
    return 0;
}

/* Interleaved float stereo 48 kHz -> the device's format. Device rates other
 * than 48 kHz get a carried linear resampler; channel counts other than two
 * get a simple up/down mix. */
static void convert_block(const float *src, int frames, BYTE *dst, const WAVEFORMATEX *f) {
    int ch = f->nChannels ? f->nChannels : 2;
    int bits = f->wBitsPerSample ? f->wBitsPerSample : 32;
    int isf = fmt_is_float(f);
    int step_i = f->nSamplesPerSec ? (int)(f->nSamplesPerSec) : KORRA_AUDIO_RATE;
    double step = (double)KORRA_AUDIO_RATE / (double)step_i;
    int i;

    if (step_i == KORRA_AUDIO_RATE) {
        for (i = 0; i < frames; i++) {
            float l = src[2 * i], r = src[2 * i + 1];
            int c;
            for (c = 0; c < ch; c++) {
                float v;
                if (ch == 1) v = 0.5f * (l + r);
                else if (c == 0) v = l;
                else if (c == 1) v = r;
                else v = 0.0f;
                if (v > 1.0f) v = 1.0f;
                if (v < -1.0f) v = -1.0f;
                if (isf && bits == 32) ((float *)dst)[i * ch + c] = v;
                else if (bits == 16) ((short *)dst)[i * ch + c] = (short)(v * 32767.0f);
                else if (bits == 24) {
                    long s = (long)(v * 8388607.0f);
                    BYTE *p = dst + (size_t)(i * ch + c) * 3;
                    p[0] = (BYTE)s; p[1] = (BYTE)(s >> 8); p[2] = (BYTE)(s >> 16);
                } else if (bits == 32) ((int *)dst)[i * ch + c] = (int)(v * 2147483647.0f);
                else ((short *)dst)[i * ch + c] = (short)(v * 32767.0f);
            }
        }
        return;
    }

    /* Resample: g_rs_pos is a position in the incoming stream, carried across
     * calls, so no click at a block boundary (p = -1 means "the last frame of
     * the previous block"). */
    {
        double stepf = step;
        for (i = 0; i < frames; i++) {
            double p = g_rs_pos;
            int i0 = (int)floor(p), i1;
            double fr = p - (double)i0;
            float l0, r0, l1, r1, l, r;
            int c;
            i1 = i0 + 1;
            if (i0 < 0) {
                if (g_rs_have) { l0 = g_rs_last[0]; r0 = g_rs_last[1]; }
                else { l0 = src[0]; r0 = src[1]; }
            } else if (i0 >= frames) { l0 = src[(frames - 1) * 2]; r0 = src[(frames - 1) * 2 + 1]; }
            else { l0 = src[2 * i0]; r0 = src[2 * i0 + 1]; }
            if (i1 < 0) {
                if (g_rs_have) { l1 = g_rs_last[0]; r1 = g_rs_last[1]; }
                else { l1 = src[0]; r1 = src[1]; }
            } else if (i1 >= frames) { l1 = src[(frames - 1) * 2]; r1 = src[(frames - 1) * 2 + 1]; }
            else { l1 = src[2 * i1]; r1 = src[2 * i1 + 1]; }
            l = (float)(l0 + (l1 - l0) * fr);
            r = (float)(r0 + (r1 - r0) * fr);
            for (c = 0; c < ch; c++) {
                float v = (ch == 1) ? 0.5f * (l + r) : (c == 0 ? l : (c == 1 ? r : 0.0f));
                if (v > 1.0f) v = 1.0f;
                if (v < -1.0f) v = -1.0f;
                if (isf && bits == 32) ((float *)dst)[i * ch + c] = v;
                else if (bits == 16) ((short *)dst)[i * ch + c] = (short)(v * 32767.0f);
                else if (bits == 24) {
                    long s = (long)(v * 8388607.0f);
                    BYTE *q = dst + (size_t)(i * ch + c) * 3;
                    q[0] = (BYTE)s; q[1] = (BYTE)(s >> 8); q[2] = (BYTE)(s >> 16);
                } else if (bits == 32) ((int *)dst)[i * ch + c] = (int)(v * 2147483647.0f);
                else ((short *)dst)[i * ch + c] = (short)(v * 32767.0f);
            }
            g_rs_pos += stepf;
        }
    }
    g_rs_pos -= (double)frames;
    if (g_rs_pos < -1.0) g_rs_pos = -1.0;
    g_rs_last[0] = src[(frames - 1) * 2];
    g_rs_last[1] = src[(frames - 1) * 2 + 1];
    g_rs_have = 1;
}

/* ------------------------------------------------------------ render tick */
static float *g_scratch;
static int g_scratch_cap;
static unsigned long long g_last_log_frame;

static float *scratch(int frames) {
    if (frames > g_scratch_cap) {
        float *p = (float *)realloc(g_scratch, (size_t)frames * KORRA_AUDIO_CHANNELS * sizeof(float));
        if (!p) return NULL;
        g_scratch = p;
        g_scratch_cap = frames;
    }
    return g_scratch;
}

static void render_block(float *dst, int frames, const WAVEFORMATEX *fmt, BYTE *dev) {
    if (frames <= 0) return;
    if (!dst) dst = scratch(frames);
    if (!dst) { ZeroMemory(dev, (size_t)frames * (fmt ? fmt->nBlockAlign : 4)); return; }
    if (g_render) g_render(dst, frames);
    else ZeroMemory(dst, (size_t)frames * KORRA_AUDIO_CHANNELS * sizeof(float));
    stats_block(dst, frames, g_stats.frames);
    dump_write(dst, frames);
    if (dev) convert_block(dst, frames, dev, fmt);
}

/* Thread census: which threads exist and whether they are spinning in guest
 * code or parked in a host wait. Twice-sampled so "blocked" and "spinning"
 * cannot be confused with "slow". */
static uint32_t thread_eip(HANDLE h) {
    CONTEXT c;
    DWORD eip = 0;
    memset(&c, 0, sizeof c);
    c.ContextFlags = CONTEXT_CONTROL;
    if (!GetThreadContext(h, &c)) return 0;
    eip = (DWORD)c.Eip;
    return eip;
}

static const char *eip_class(uint32_t eip, char *buf, size_t n) {
    if (eip >= 0x00400000u && eip < 0x00F42000u) {
        _snprintf(buf, n, "guest");
        return buf;
    }
    buf[0] = 0;
    {
        HMODULE m = NULL;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)(uintptr_t)eip, &m) && m) {
            char path[MAX_PATH];
            const char *base;
            if (GetModuleFileNameA(m, path, sizeof path)) {
                base = strrchr(path, '\\');
                _snprintf(buf, n, "%s", base ? base + 1 : path);
                return buf;
            }
        }
    }
    _snprintf(buf, n, "?");
    return buf;
}

static void thread_census(void) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te;
    DWORD me = GetCurrentThreadId();
    int printed = 0;
    if (snap == INVALID_HANDLE_VALUE) return;
    te.dwSize = sizeof te;
    sink_log("census:");
    if (Thread32First(snap, &te)) {
        do {
            HANDLE h;
            uint32_t a, b;
            char cls[64];
            if (te.th32OwnerProcessID != GetCurrentProcessId()) continue;
            if (te.th32ThreadID == me) continue;
            h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                           FALSE, te.th32ThreadID);
            if (!h) continue;
            if (SuspendThread(h) == (DWORD)-1) { CloseHandle(h); continue; }
            a = thread_eip(h);
            Sleep(3);
            b = thread_eip(h);
            ResumeThread(h);
            CloseHandle(h);
            eip_class(b, cls, sizeof cls);
            sink_log("  tid %5lu  eip %08X/%08X %-14s %s",
                     (unsigned long)te.th32ThreadID, a, b, cls,
                     a == b ? (b >= 0x00400000u && b < 0x00F42000u ? "SAME-GUEST" : "same") : "moving");
            printed++;
        } while (printed < 40 && Thread32Next(snap, &te));
    }
    CloseHandle(snap);
}

static void tick_log(void) {
    unsigned long long f = g_stats.frames;
    if (f - g_last_log_frame < KORRA_AUDIO_RATE) return;
    g_last_log_frame = f;
    dump_patch();
    if (g_census_next && f >= g_census_next) {
        g_census_next = f + 5ull * KORRA_AUDIO_RATE;
        thread_census();
    }
    if (!g_verbose) return;
    sink_log("%.1f s  frames=%llu  peak=%.1f dBFS  rms=%.1f dBFS  quiet=%.1f s  underruns=%llu",
             (double)f / KORRA_AUDIO_RATE, f, db(g_stats.peak),
             db(sqrt(g_rms_sum / (double)(g_rms_n ? g_rms_n : 1))),
             (double)g_stats.silent_frames / KORRA_AUDIO_RATE,
             g_stats.underruns);
}

/* --------------------------------------------------------------- WASAPI
 * These are declared with DEFINE_GUID in the SDK, which would need uuid.lib;
 * the sink links ole32 only, so the four identities it uses are spelled out. */
static const GUID g_cls_mmdev =
    { 0xbcde0395, 0xe52f, 0x467c, { 0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e } };
static const GUID g_iid_mmdev =
    { 0xa95664d2, 0x9614, 0x4f35, { 0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36, 0x17, 0xe6 } };
static const GUID g_iid_audioclient =
    { 0x1cb9ad4c, 0xdbfa, 0x4c32, { 0xb1, 0x78, 0xc2, 0xf5, 0x68, 0xa7, 0x03, 0xb2 } };
static const GUID g_iid_renderclient =
    { 0xf294acfc, 0x3146, 0x4483, { 0xa7, 0xbf, 0xad, 0xdc, 0xa7, 0xc2, 0x60, 0xe2 } };

static int wasapi_run(void) {
    IMMDeviceEnumerator *en = NULL;
    IMMDevice *dev = NULL;
    IAudioClient *cl = NULL;
    IAudioRenderClient *rc = NULL;
    WAVEFORMATEX req;
    WAVEFORMATEX *mix = NULL, *use = NULL;
    HANDLE ev = NULL;
    UINT32 buf_frames = 0;
    REFERENCE_TIME dur = (REFERENCE_TIME)g_latency_ms * 10000;
    HRESULT hr;
    int ok = 0;
    DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) return 0;

    hr = CoCreateInstance(&g_cls_mmdev, NULL, CLSCTX_ALL, &g_iid_mmdev, (void **)&en);
    if (FAILED(hr)) { sink_log("WASAPI: no device enumerator (0x%08lX)", (unsigned long)hr); goto out; }
    hr = en->lpVtbl->GetDefaultAudioEndpoint(en, eRender, eConsole, &dev);
    if (FAILED(hr)) { sink_log("WASAPI: no render endpoint (0x%08lX)", (unsigned long)hr); goto out; }
    hr = dev->lpVtbl->Activate(dev, &g_iid_audioclient, CLSCTX_ALL, NULL, (void **)&cl);
    if (FAILED(hr)) { sink_log("WASAPI: Activate(IAudioClient) failed (0x%08lX)", (unsigned long)hr); goto out; }

    ZeroMemory(&req, sizeof req);
    req.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    req.nChannels = KORRA_AUDIO_CHANNELS;
    req.nSamplesPerSec = KORRA_AUDIO_RATE;
    req.wBitsPerSample = 32;
    req.nBlockAlign = (WORD)(req.nChannels * 4);
    req.nAvgBytesPerSec = req.nSamplesPerSec * req.nBlockAlign;

    ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (!ev) goto out;

    /* First choice: our format, converted by the audio engine. */
    hr = cl->lpVtbl->Initialize(cl, AUDCLNT_SHAREMODE_SHARED,
                                flags | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                dur, 0, &req, NULL);
    if (SUCCEEDED(hr)) {
        use = &req;
    } else {
        hr = cl->lpVtbl->GetMixFormat(cl, &mix);
        if (FAILED(hr) || !mix) { sink_log("WASAPI: GetMixFormat failed (0x%08lX)", (unsigned long)hr); goto out; }
        use = mix;
        hr = cl->lpVtbl->Initialize(cl, AUDCLNT_SHAREMODE_SHARED, flags, dur, 0, mix, NULL);
        if (FAILED(hr)) {
            /* Some drivers want the buffer size they advertise. */
            REFERENCE_TIME def = 0, minp = 0;
            if (SUCCEEDED(cl->lpVtbl->GetDevicePeriod(cl, &def, &minp)))
                hr = cl->lpVtbl->Initialize(cl, AUDCLNT_SHAREMODE_SHARED, flags, def, 0, mix, NULL);
        }
        if (FAILED(hr)) { sink_log("WASAPI: Initialize failed (0x%08lX)", (unsigned long)hr); goto out; }
    }
    hr = cl->lpVtbl->SetEventHandle(cl, ev);
    if (FAILED(hr)) { sink_log("WASAPI: SetEventHandle failed (0x%08lX)", (unsigned long)hr); goto out; }
    hr = cl->lpVtbl->GetService(cl, &g_iid_renderclient, (void **)&rc);
    if (FAILED(hr)) { sink_log("WASAPI: GetService(IAudioRenderClient) failed (0x%08lX)", (unsigned long)hr); goto out; }
    hr = cl->lpVtbl->GetBufferSize(cl, &buf_frames);
    if (FAILED(hr) || !buf_frames) { sink_log("WASAPI: GetBufferSize failed"); goto out; }

    g_stats.backend = KORRA_AUDIO_BACKEND_WASAPI;
    g_stats.device_rate = (int)use->nSamplesPerSec;
    g_stats.device_channels = use->nChannels;
    g_stats.device_bits = use->wBitsPerSample;
    sink_log("WASAPI shared: %d ch, %d Hz, %d bits, buffer %u frames (%.1f ms), %s",
             use->nChannels, use->nSamplesPerSec, use->wBitsPerSample, buf_frames,
             1000.0 * buf_frames / use->nSamplesPerSec,
             use == &req ? "engine-converted 48k float" : "device mix format");

    hr = cl->lpVtbl->Start(cl);
    if (FAILED(hr)) { sink_log("WASAPI: Start failed (0x%08lX)", (unsigned long)hr); goto out; }
    InterlockedExchange(&g_live, 1);
    SetEvent(g_ready);
    ok = 1;

    while (!g_stop) {
        DWORD w = WaitForSingleObject(ev, 500);
        UINT32 pad = 0, avail = 0;
        BYTE *p = NULL;
        if (g_stop) break;
        if (w != WAIT_OBJECT_0) continue;
        if (FAILED(cl->lpVtbl->GetCurrentPadding(cl, &pad))) break;
        avail = buf_frames - pad;
        if (!avail) continue;
        if (FAILED(rc->lpVtbl->GetBuffer(rc, avail, &p)) || !p) break;
        render_block(NULL, (int)avail, use, p);
        rc->lpVtbl->ReleaseBuffer(rc, avail, 0);
        tick_log();
    }
    cl->lpVtbl->Stop(cl);

out:
    if (ok) {
        InterlockedExchange(&g_live, 0);
    }
    if (rc) rc->lpVtbl->Release(rc);
    if (cl) cl->lpVtbl->Release(cl);
    if (mix) CoTaskMemFree(mix);
    if (dev) dev->lpVtbl->Release(dev);
    if (en) en->lpVtbl->Release(en);
    if (ev) CloseHandle(ev);
    CoUninitialize();
    return ok;
}

/* -------------------------------------------------------------- waveOut */
static int waveout_run(void) {
    enum { NBUF = 4, FRAMES = 480 };      /* 10 ms at 48 kHz */
    HWAVEOUT h = NULL;
    WAVEHDR hdr[NBUF];
    HANDLE ev;
    WAVEFORMATEX fmt;
    BYTE *mem;
    int i, ok = 0;
    DWORD flags = WAVE_FORMAT_PCM;
    MMRESULT mr;

    (void)flags;
    ZeroMemory(hdr, sizeof hdr);
    ZeroMemory(&fmt, sizeof fmt);
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = KORRA_AUDIO_CHANNELS;
    fmt.nSamplesPerSec = KORRA_AUDIO_RATE;
    fmt.wBitsPerSample = 16;
    fmt.nBlockAlign = (WORD)(KORRA_AUDIO_CHANNELS * 2);
    fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;

    ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (!ev) return 0;
    mr = waveOutOpen(&h, WAVE_MAPPER, &fmt, (DWORD_PTR)ev, 0, CALLBACK_EVENT);
    if (mr != MMSYSERR_NOERROR) { sink_log("waveOut: open failed (%u)", (unsigned)mr); CloseHandle(ev); return 0; }

    mem = (BYTE *)calloc(NBUF, FRAMES * fmt.nBlockAlign);
    if (!mem) { waveOutClose(h); CloseHandle(ev); return 0; }
    g_stats.backend = KORRA_AUDIO_BACKEND_WAVEOUT;
    g_stats.device_rate = KORRA_AUDIO_RATE;
    g_stats.device_channels = KORRA_AUDIO_CHANNELS;
    g_stats.device_bits = 16;
    sink_log("waveOut: 2 ch, 48000 Hz, 16 bits, %d x %d frames", NBUF, FRAMES);

    for (i = 0; i < NBUF; i++) {
        hdr[i].lpData = (LPSTR)(mem + (size_t)i * FRAMES * fmt.nBlockAlign);
        hdr[i].dwBufferLength = FRAMES * fmt.nBlockAlign;
        hdr[i].dwUser = (DWORD_PTR)i;
        waveOutPrepareHeader(h, &hdr[i], sizeof hdr[i]);
        waveOutWrite(h, &hdr[i], sizeof hdr[i]);
    }
    InterlockedExchange(&g_live, 1);
    SetEvent(g_ready);
    ok = 1;

    while (!g_stop) {
        DWORD w = WaitForSingleObject(ev, 200);
        int i2;
        if (g_stop) break;
        if (w != WAIT_OBJECT_0) continue;
        for (i2 = 0; i2 < NBUF; i2++) {
            if (!(hdr[i2].dwFlags & WHDR_DONE)) continue;
            hdr[i2].dwFlags &= ~(DWORD)WHDR_DONE;
            render_block(NULL, FRAMES, &fmt, (BYTE *)hdr[i2].lpData);
            waveOutWrite(h, &hdr[i2], sizeof hdr[i2]);
            tick_log();
        }
    }
    waveOutReset(h);
    for (i = 0; i < NBUF; i++) waveOutUnprepareHeader(h, &hdr[i], sizeof hdr[i]);
    waveOutClose(h);
    free(mem);
    CloseHandle(ev);
    InterlockedExchange(&g_live, 0);
    return ok;
}

/* ----------------------------------------------------------------- null */
static int null_run(void) {
    LARGE_INTEGER freq, prev;
    const int frames = 480;
    double due = 0.0;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&prev);
    g_stats.backend = KORRA_AUDIO_BACKEND_NULL;
    g_stats.device_rate = KORRA_AUDIO_RATE;
    g_stats.device_channels = KORRA_AUDIO_CHANNELS;
    g_stats.device_bits = 16;
    sink_log("null sink: real-time clock only (no device)");
    InterlockedExchange(&g_live, 1);
    SetEvent(g_ready);
    while (!g_stop) {
        LARGE_INTEGER now;
        double elapsed;
        QueryPerformanceCounter(&now);
        elapsed = (double)(now.QuadPart - prev.QuadPart) / (double)freq.QuadPart;
        due += (double)frames / KORRA_AUDIO_RATE;
        if (elapsed + 0.001 < due) {
            Sleep(1);
            continue;
        }
        render_block(NULL, frames, NULL, NULL);
        tick_log();
        due = elapsed;
    }
    InterlockedExchange(&g_live, 0);
    return 1;
}

/* ---------------------------------------------------------------- thread */
static DWORD WINAPI sink_thread(LPVOID param) {
    int ok = 0;
    (void)param;
    switch (g_want_backend) {
    case KORRA_AUDIO_BACKEND_WAVEOUT: ok = waveout_run(); break;
    case KORRA_AUDIO_BACKEND_NULL: ok = null_run(); break;
    case KORRA_AUDIO_BACKEND_WASAPI:
    default: ok = wasapi_run(); break;
    }
    if (!ok && g_want_backend != KORRA_AUDIO_BACKEND_WAVEOUT && g_want_backend != KORRA_AUDIO_BACKEND_NULL) {
        sink_log("falling back to waveOut");
        ok = waveout_run();
    }
    if (!ok) {
        sink_log("falling back to the null sink");
        ok = null_run();
    }
    if (!ok) SetEvent(g_ready);
    return ok ? 0 : 1;
}

/* ------------------------------------------------------------------- API */
const char *korra_audio_sink_backend(void) {
    switch (g_stats.backend) {
    case KORRA_AUDIO_BACKEND_WASAPI: return "wasapi";
    case KORRA_AUDIO_BACKEND_WAVEOUT: return "waveout";
    case KORRA_AUDIO_BACKEND_NULL: return "null";
    default: return "none";
    }
}

int korra_audio_sink_live(void) { return (int)g_live; }

void korra_audio_sink_note_underrun(void) {
    g_stats.underruns++;
}

void korra_audio_sink_get_stats(korra_audio_stats *out) {
    if (!out) return;
    *out = g_stats;
    out->live = (int)g_live;
    out->rms = g_rms_n ? sqrt(g_rms_sum / (double)g_rms_n) : 0.0;
}

static void sink_atexit(void) {
    korra_audio_sink_stop();
}

int korra_audio_sink_start(korra_audio_render_fn fn) {
    const char *e;
    if (InterlockedCompareExchange(&g_started, 1, 0) != 0) return (int)g_live;
    g_render = fn;
    g_ready = CreateEventA(NULL, TRUE, FALSE, NULL);
    g_wake = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!g_ready || !g_wake) return 0;

    if (getenv("KORRA_AUDIO_LOG")) {
        int v = atoi(getenv("KORRA_AUDIO_LOG"));
        if (v >= 0 && v <= 1) g_verbose = v;
    }
    if (getenv("KORRA_AUDIO_LATENCY_MS")) {
        int v = atoi(getenv("KORRA_AUDIO_LATENCY_MS"));
        if (v >= 5 && v <= 200) g_latency_ms = v;
    }
    e = getenv("KORRA_AUDIO_BACKEND");
    if (e && *e) {
        if (!strcmp(e, "wasapi")) g_want_backend = KORRA_AUDIO_BACKEND_WASAPI;
        else if (!strcmp(e, "waveout")) g_want_backend = KORRA_AUDIO_BACKEND_WAVEOUT;
        else if (!strcmp(e, "null")) g_want_backend = KORRA_AUDIO_BACKEND_NULL;
        else if (!strcmp(e, "none")) { g_want_backend = KORRA_AUDIO_BACKEND_NONE; return 0; }
    }
    e = getenv("KORRA_AUDIO_CENSUS");
    if (e && *e && atoi(e) > 0) g_census_next = 20ull * KORRA_AUDIO_RATE;
    dump_open();
    atexit(sink_atexit);
    g_thread = CreateThread(NULL, 256 * 1024, sink_thread, NULL, 0, NULL);
    if (!g_thread) { sink_log("CreateThread failed"); return 0; }
    WaitForSingleObject(g_ready, 5000);
    return (int)g_live;
}

void korra_audio_sink_stop(void) {
    if (!g_thread) return;
    InterlockedExchange(&g_stop, 1);
    if (g_wake) SetEvent(g_wake);
    WaitForSingleObject(g_thread, 3000);
    CloseHandle(g_thread);
    g_thread = NULL;
    if (g_ready) { CloseHandle(g_ready); g_ready = NULL; }
    if (g_wake) { CloseHandle(g_wake); g_wake = NULL; }
    dump_close();
    {
        korra_audio_stats s = g_stats;
        sink_log("stopped: %s, %.3f s, peak %.1f dBFS, rms %.1f dBFS, quiet %.1f s, %u blocks, %llu underruns",
                 korra_audio_sink_backend(), (double)s.frames / KORRA_AUDIO_RATE,
                 db(s.peak), db(g_rms_n ? sqrt(g_rms_sum / (double)g_rms_n) : 0.0),
                 (double)s.silent_frames / KORRA_AUDIO_RATE, s.blocks, s.underruns);
    }
}

#endif /* !__ANDROID__ */
