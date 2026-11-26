/* xaudio2_android.c - IXAudio2 2.7 for the Android host: the device, the
 * mixer, and the guest-visible COM objects.
 *
 * This is the Android twin of src/host/xaudio2_stub.c, which is the reference
 * and was measured on Windows (docs/audio.md 10). The engine's own Wwise/CRI
 * mixer is in the lifted C; the host answers the device and mixes what it is
 * handed. Everything below is a port of that file onto this host's model:
 *
 *   - a guest COM object is [vtable VA][tag] in guest memory, the vtable is a
 *     guest array of shim VAs (misc_android.c make_object), so
 *     `mov eax,[ecx]; call [eax+4*n]` resolves through recomp_lookup_import;
 *   - the lifted guest calls those slots as ordinary stdcall: `this` is the
 *     first stack argument, so every thunk reads A32(0) and pops 1 + params;
 *   - pointer fields (WAVEFORMATEX, buffer, matrix, callback, PCM data) are
 *     32-bit guest VAs and are reached with the MEM32/MEM16 accessors and
 *     ka_host(), never as host pointers;
 *   - the mixer never enters guest code. The one guest memory it writes is
 *     CRI's completion counter, a single aligned dword.
 *
 * The semantics that cost a defect each on Windows (docs/AUDIO_ANDROID.md 3)
 * are kept exactly: both CLSIDs, GetDeviceCount() == 1, a 2 ch / 48 kHz
 * mastering voice, SetOutputMatrix is the mix, honest GetState, accept the
 * engine's formats, start the sink lazily on the first real buffer, no guest
 * code from the mix callback.
 *
 * Knobs (android properties, see `adb shell setprop`):
 *   debug.korra.xa2 = 0..3   log level (default 1; 2 logs every voice call,
 *                            3 logs every queue-length change)
 *   debug.korra.audio.dump=<path>  16-bit WAV of what the device got
 *   debug.korra.audio.backend=null  no device; the monotonic clock still
 *                                  retires buffers so CRI keeps feeding
 */
#define _GNU_SOURCE
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/system_properties.h>

#include "host_android.h"
#include "audio_sink.h"

/* The guest heap is committed RW for its whole 1 GiB (memory_android.c), the
 * image for SizeOfImage (pe_android.c) and the stack region up front. Those
 * three ranges are what "readable" means here; VirtualQuery's job on Windows. */
#define XV_HEAP_LO  0x20000000u
#define XV_HEAP_HI  0x60000000u   /* KA_HEAP_BASE + HEAP_SIZE */
#define XV_STACK_LO 0xC0000000u
#define XV_STACK_HI 0xD0000000u

#define XV_MAX_VOICES 32
#define XV_QUEUE_MAX  8

#define XV_KIND_MASTER 1
#define XV_KIND_SOURCE 2
#define XV_KIND_SUBMIX 3

#define XV_HR_OK      0u
#define XV_HR_INVALID 0x80070057u   /* E_INVALIDARG */
#define XV_HR_NOIFACE 0x80004002u   /* E_NOINTERFACE */
#define XV_HR_NOMEM   0x8007000Eu   /* E_OUTOFMEMORY */

/* CRI's IXAudio2VoiceCallback::OnBufferEnd is guest 0x7B2A24 and its whole
 * body is `inc dword ptr [pBufferContext]`. The image is mapped at its own
 * base on Android too, so the VA is the same identity as on Windows. */
#define XV_CB_ONBUFFEREND_VA 0x7B2A24u

typedef struct {
    uint64_t start, end;   /* absolute source frames */
    uint32_t data;         /* guest VA of the PCM slice (owned by the guest) */
    uint32_t ctx;          /* guest VA of pBufferContext */
    uint32_t hash;         /* FNV of the payload at submit: a mismatch at probe
                            * time means the engine rewrote a buffer we still
                            * read (torn read), not that it wrote an integer. */
} xv_slice;

typedef struct {
    uint32_t magic;        /* XV_MAGIC when live */
    uint32_t kind;
    uint32_t obj;          /* the guest COM object VA this state backs */
    uint32_t rate, channels, bits, block_align;
    uint32_t fmt_tag, is_float, channel_mask;
    float ratio, volume;
    int running;
    uint32_t qhead, qcount;
    double pos;            /* next source frame to mix (fractional) */
    uint64_t played;       /* frames retired (clock fallback) */
    uint64_t base, t0ns;   /* fallback clock */
    uint64_t next_end;
    uint32_t last_ctx;
    uint32_t cb;           /* guest IXAudio2VoiceCallback VA or 0 */
    uint32_t cb_kind;      /* 0 none, 1 CRI counter, 2 other */
    uint32_t cb_retired;   /* buffers retired since the callback thread last ran */
    uint32_t overflow_logged, cb_logs;
    uint32_t submits, gs_calls, last_logged_q;
    int had_data, dry, oor_warned;
    uint32_t bad_slices;   /* diagnostics: whole-slice profiles printed */
    uint32_t hw_short;     /* diagnostics: Hw blocks whose count < declared frames */
    uint32_t nan_words;    /* float words that were not PCM and were zeroed */
    uint32_t nan_logged;   /* diagnostics: the first three named */
    uint32_t matrix_ch;
    float matrix[8 * 2];
    xv_slice q[XV_QUEUE_MAX];
} xv_voice;

#define XV_MAGIC 0x58413432u   /* "XA42" */

static xv_voice g_v[XV_MAX_VOICES];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static struct {
    int magic;
    uint32_t refs, initialized, engine_cb;
    int master;                /* index into g_v, -1 = none */
} g_dev;

static int g_verbose = 1;
static int g_inited;

/* Device identity as the guest knows it: two guest heap VAs, built once. */
static uint32_t g_vt_dev, g_vt_master, g_vt_src;
static uint32_t g_obj_dev;

static uint64_t g_t0ns;
static uint64_t g_cocreates;

/* ---------------------------------------------------------------- logging */
static void xv_log(const char *fmt, ...) {
    char buf[768];
    va_list ap;
    struct timespec ts;
    uint64_t ms;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n >= sizeof buf) n = (int)sizeof buf - 1;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    ms = (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
    if (!g_t0ns) g_t0ns = ms;
    ka_log("[xa2] %8.1f %s", (double)(ms - g_t0ns), buf);
}

static void xv_lock(void) { pthread_mutex_lock(&g_lock); }
static void xv_unlock(void) { pthread_mutex_unlock(&g_lock); }

static uint64_t xv_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int xv_readable(uint32_t va, uint32_t bytes) {
    uint32_t end;
    if (!va || va < 0x10000u) return 0;
    end = va + bytes;
    if (end < va) return 0;
    if (va >= XV_HEAP_LO && end <= XV_HEAP_HI) return 1;
    if (va >= KA_IMAGE_BASE && end <= KA_IMAGE_BASE + ka_pe_span()) return 1;
    if (va >= XV_STACK_LO && end <= XV_STACK_HI) return 1;
    return 0;
}

/* ------------------------------------------------ the game's sound table
 * The Hw output device (0x0088B0C0) reads [0xCA243C]/[0xCA2438] once, while
 * it is being constructed. Its prefill takes the first entry whose 64-bit key
 * at +0x10 is zero -- the key is a selector, not an "active" flag -- and then
 * needs that entry's cfg pointer (+0x04) to be a live output device: the
 * base constructor 0x879260 writes the tag 1 at cfg+0x20, and the prefill
 * copies cfg+0x44/+0x68 into its own ring bookkeeping. Sampling the table
 * after creation says whether the entry ever gets its cfg, and the call site
 * that stores it. */
static void xv_table_sample(const char *why) {
    static uint32_t last_cfg, last_key_lo, last_key_hi, calls;
    uint32_t tbl, n, cfg = 0, key_lo = 0, key_hi = 0, state = 0, ring = 0, depth = 0;
    if (g_verbose < 2) return;
    tbl = MEM32(0xCA2438);
    n = MEM32(0xCA243C);
    if (n && tbl && xv_readable(tbl, 0x38)) {
        uint32_t e = tbl;
        key_lo = MEM32(e + 0x10);
        key_hi = MEM32(e + 0x14);
        cfg = MEM32(e + 0x04);
        if (cfg && xv_readable(cfg, 0x6C)) {
            state = MEM32(cfg + 0x20);
            ring = MEM16(cfg + 0x44);
            depth = MEM32(cfg + 0x68);
        }
    }
    calls++;
    if (calls != 1 && cfg == last_cfg && key_lo == last_key_lo && key_hi == last_key_hi)
        return;
    last_cfg = cfg; last_key_lo = key_lo; last_key_hi = key_hi;
    xv_log("[table] %s: count=%u e0 key=%08X%08X cfg=%08X state=%u ring=%u depth=%u (from=0x%08X)",
           why, n, key_hi, key_lo, cfg, state, ring, depth, g_cur_func);
}

static float xv_f(uint32_t bits) {
    float f;
    memcpy(&f, &bits, 4);
    return f;
}
static uint32_t xv_bits(float f) {
    uint32_t b;
    memcpy(&b, &f, 4);
    return b;
}

/* ------------------------------------------------------------- init/knobs */
static void xv_init(void) {
    char v[PROP_VALUE_MAX];
    if (g_inited) return;
    g_inited = 1;
    g_dev.master = -1;
    memset(&g_dev, 0, sizeof g_dev);
    g_dev.master = -1;
    if (__system_property_get("debug.korra.xa2", v) > 0) {
        int n = atoi(v);
        if (n >= 0 && n <= 3) g_verbose = n;
    }
    xv_log("IXAudio2 2.7 device ready (log level %d)", g_verbose);
}

/* -------------------------------------------------------------- vtables
 * One guest array per interface, filled with registered shims. Slots are
 * index*4 and follow the 2.7 layout measured on Windows (see the vtable
 * tables at the bottom of src/host/xaudio2_stub.c). */
typedef recomp_func_t xv_fn;

static uint32_t build_vt(const char *prefix, int count, const xv_fn *fns) {
    ka_shim tmp[40];
    char names[40][32];
    uint32_t vt;
    int i;
    if (count > 39) return 0;
    vt = ka_guest_alloc((uint32_t)count * 4, 4);
    if (!vt) return 0;
    for (i = 0; i < count; i++) {
        snprintf(names[i], sizeof names[i], "%s:%d", prefix, i);
        tmp[i].name = names[i];
        tmp[i].fn = fns[i];
    }
    tmp[count].name = NULL;
    tmp[count].fn = NULL;
    ka_shim_register(tmp);
    for (i = 0; i < count; i++) MEM32(vt + (uint32_t)i * 4) = ka_shim_va(names[i]);
    return vt;
}

/* ------------------------------------------------------------- the clock
 * One clock is authoritative at a time, as on Windows:
 *   sink live - `pos` is advanced by the mixer, one step per device frame.
 *   no sink   - the monotonic clock advances `played`, so CRI's feed loop
 *               never stalls waiting for a completion (silent fallback). */
static uint64_t xv_voice_now(const xv_voice *v) {
    double secs, frames;
    if (!v->running) return v->played;
    secs = (double)(xv_now_ns() - v->t0ns) / 1e9;
    if (secs < 0.0) secs = 0.0;
    frames = secs * (double)v->rate * (double)(v->ratio > 0.0f ? v->ratio : 1.0f);
    return v->base + (uint64_t)(frames + 0.5);
}

static int xv_sink_live(void) { return korra_audio_sink_live(); }

static uint64_t xv_voice_cursor(xv_voice *v) {
    if (xv_sink_live()) return (uint64_t)v->pos;
    {
        uint64_t now = xv_voice_now(v);
        v->pos = (double)now;
        return now;
    }
}

/* ------------------------------------------------------- completion dispatch
 * Retirement performs the guest's OnBufferEnd body in C: CRI's worker waits
 * on the counter it passes in pBufferContext, and only OnBufferEnd moves it.
 * Called on whichever thread consumed the buffer (the sink's, usually). */
static void xv_dispatch_buffer_end(xv_voice *v, uint32_t ctx) {
    if (v->cb_kind == 2) {
        /* The Hw output device's callback object: OnBufferEnd is 0x0088B3F0,
         * which credits one more buffer and wakes the engine's mixer. It runs
         * on the callback thread, never here (the sink callback must not take
         * the machine lock). The feed submits these buffers with pContext = 0,
         * so the credit cannot be conditioned on ctx. */
        __atomic_add_fetch(&v->cb_retired, 1u, __ATOMIC_RELAXED);
        return;
    }
    if (!ctx) return;
    if (v->cb_kind == 1) {
        if (xv_readable(ctx, 4)) {
            __atomic_add_fetch((uint32_t *)ka_host(ctx), 1u, __ATOMIC_RELAXED);
        } else if (g_verbose) {
            xv_log("OnBufferEnd: unreadable pBufferContext %08X (dropped)", ctx);
        }
        return;
    }
}

/* --------------------------------------------- the XAudio2 callback thread
 * XAudio2 2.7 invokes IXAudio2VoiceCallback on its own worker thread. The
 * engine's Hw output device registers the object at device+0x28 as its voice
 * callback: slot 0 (OnVoiceProcessingPassStart) is the feed 0x0088B440 that
 * tops the source voice up from the device's 8-slot ring, and slot 4
 * (OnBufferEnd) is 0x0088B3F0, which credits one more buffer and wakes the
 * mixer. Before this thread existed the shim only performed CRI's counter in
 * C, so the Hw voice was never queued (measured: the mixer had produced four
 * buffers; the voice had zero) - the feed is the caller 0x0088B440 was
 * waiting for.
 *
 * The shipped bodies are plain __stdcall with `this` as the first stack
 * argument (slots 0 and 4 both end in `ret 8`), which hle_call_guest
 * reproduces exactly. The thread is the host twin of XAudio2's worker: it
 * never holds the mixer lock while entering guest code, and it reports buffer
 * ends the sink retired rather than calling from the sink callback. */
#define XV_CB_PERIOD_NS 10000000ull

static pthread_t g_cb_thread;
static int g_cb_thread_started, g_cb_stop;

static void xv_call_slot(uint32_t cb, uint32_t slot, uint32_t arg) {
    uint32_t vt, fn, args[2];
    if (!xv_readable(cb, 4)) return;
    vt = MEM32(cb);
    if (!vt || !xv_readable(vt + slot * 4, 4)) return;
    fn = MEM32(vt + slot * 4);
    if (!fn) return;
    args[0] = cb;
    args[1] = arg;
    hle_call_guest(fn, 2, args);
}

static void *xv_cb_thread_main(void *arg) {
    (void)arg;
    if (!ka_thread_attach("xa2-callback")) {
        ka_log("[xa2] callback thread: no guest stack/TIB; the Hw voice stays unfed");
        return NULL;
    }
    while (!g_cb_stop) {
        struct timespec ts = { 0, (long)XV_CB_PERIOD_NS };
        int i;
        xv_lock();
        for (i = 0; i < XV_MAX_VOICES; i++) {
            xv_voice *v = &g_v[i];
            uint32_t cb, n, credits = 0, qc = 0;
            int log;
            if (v->magic != XV_MAGIC || v->cb_kind != 2 || !v->cb) continue;
            cb = v->cb;
            n = __atomic_exchange_n(&v->cb_retired, 0u, __ATOMIC_RELAXED);
            if (n > 8) n = 8;                     /* ring depth; a tick cannot owe more */
            qc = v->qcount;
            if (xv_readable(cb + 0x40, 4)) credits = MEM32(cb + 0x40);
            log = v->cb_logs < 12;
            if (log) v->cb_logs++;
            xv_unlock();
            /* The guest's loop re-reads the credit counter on every iteration
             * (`while (credits - BuffersQueued != 0) submit`), so a credit
             * that falls while it runs never converges: measured as 8.6M
             * dropped submits in 90 s. Hold the machine lock across the
             * decision and the call, so nothing can decrement credits between
             * them, and never ask for more than the ring can hold. */
            mach_enter();
            if (log)
                xv_log("cb tick(%d): credits=%u qcount=%u end=%u",
                       (int)(v - g_v), credits, qc, n);
            while (n--) xv_call_slot(cb, 4, 0);   /* OnBufferEnd */
            xv_lock();
            qc = v->qcount;
            credits = xv_readable(cb + 0x40, 4) ? MEM32(cb + 0x40) : 0;
            xv_unlock();
            if (credits > qc && credits - qc <= XV_QUEUE_MAX)
                xv_call_slot(cb, 0, 0);           /* OnVoiceProcessingPassStart */
            mach_leave();
            xv_lock();
        }
        xv_unlock();
        nanosleep(&ts, NULL);
    }
    ka_thread_detach();
    return NULL;
}

static void xv_cb_ensure_thread(void) {
    if (g_cb_thread_started) return;
    g_cb_thread_started = 1;
    if (pthread_create(&g_cb_thread, NULL, xv_cb_thread_main, NULL) != 0) {
        g_cb_thread_started = 0;
        ka_log("[xa2] callback thread: pthread_create failed");
    }
}

static void xv_slice_complete(xv_voice *v, const xv_slice *s) {
    v->played = s->end;
    v->last_ctx = s->ctx;
    xv_dispatch_buffer_end(v, s->ctx);
}

static void xv_voice_retire(xv_voice *v, uint64_t now) {
    while (v->qcount && now >= v->q[v->qhead].end) {
        xv_slice s = v->q[v->qhead];
        v->qhead = (v->qhead + 1) % XV_QUEUE_MAX;
        v->qcount--;
        xv_slice_complete(v, &s);
    }
    if (v->qcount) v->pos = (double)(now < v->played ? v->played : now);
    else { v->next_end = v->played; v->pos = (double)v->played; }
}

static void xv_voice_retire_consumed(xv_voice *v) {
    while (v->qcount && v->pos >= (double)v->q[v->qhead].end) {
        xv_slice s = v->q[v->qhead];
        v->qhead = (v->qhead + 1) % XV_QUEUE_MAX;
        v->qcount--;
        xv_slice_complete(v, &s);
    }
    if (!v->qcount) {
        /* Never rewind. The M4 Windows build clamped `pos` back to `played`
         * here; with a live sink that rewinds the read cursor into a buffer
         * whose OnBufferEnd has already fired, and CRI is free to refill it -
         * measured on the device as a torn float (-56538) at a slice edge.
         * The cursor is the play position: it only goes forward. */
        if (v->pos < (double)v->played) v->pos = (double)v->played;
        v->next_end = (uint64_t)v->pos;
    }
}

/* Called from every device entry point (CommitChanges included) while no sink
 * is live: the engine keeps committing while it waits for completions. */
static void xv_pump_all(void) {
    int i;
    if (xv_sink_live()) return;
    for (i = 0; i < XV_MAX_VOICES; i++) {
        xv_voice *v = &g_v[i];
        if (v->magic == XV_MAGIC && v->qcount) xv_voice_retire(v, xv_voice_now(v));
    }
}

/* --------------------------------------------------------------- the mixer
 * Flat bus: every running source voice sums straight into the stereo output.
 * That is what both consumers do (CRI feeds one voice; the Hw mixer's single
 * float voice is the engine's final mix). The engine's SetOutputMatrix is the
 * downmix; inventing coefficients made the Windows mix 11 dB hot. */
static float xv_sample_ch(const xv_voice *v, const xv_slice *s, uint32_t idx, uint32_t ch) {
    const uint8_t *p;
    if (!s->data || !v->bits || idx >= (uint32_t)(s->end - s->start)) return 0.0f;
    p = (const uint8_t *)ka_host(s->data) +
        ((size_t)idx * v->channels + ch) * (size_t)(v->bits / 8);
    if (v->is_float && v->bits == 32) {
        float f;
        memcpy(&f, p, 4);
        /* NaN/Inf are not PCM in the engine's own IEEE float format, and the
         * slice probe named where they come from: the uncommitted tail of a
         * ring slot the engine is still writing (hash changed since submit).
         * Output silence for exactly those words - every finite value is left
         * untouched, so this invents no level - and count them. Not silent:
         * the probe still logs the slice, and this logs the first three. */
        if (!isfinite(f)) {
            xv_voice *w = (xv_voice *)v;
            w->nan_words++;
            if (w->nan_logged < 3) {
                w->nan_logged++;
                xv_log("voice %d: non-finite word in the mix (%08X at rel %u ch %u), "
                       "zeroed - %u so far", (int)(v - g_v),
                       *((const uint32_t *)p), idx, ch, w->nan_words);
            }
            return 0.0f;
        }
        return f;
    }
    if (v->bits == 16) return (float)(*(const int16_t *)p) * (1.0f / 32768.0f);
    if (v->bits == 24) {
        int x = p[0] | (p[1] << 8) | (p[2] << 16);
        if (x & 0x800000) x -= 0x1000000;
        return (float)x * (1.0f / 8388608.0f);
    }
    if (v->bits == 32) {
        int32_t i;
        memcpy(&i, p, 4);
        return (float)i * (1.0f / 2147483648.0f);
    }
    return 0.0f;
}

static void xv_frame_stereo(const xv_voice *v, const xv_slice *s, uint32_t idx,
                            float *l, float *r) {
    uint32_t c, nch = v->channels ? v->channels : 2;
    float ch[8];
    if (nch > 8) nch = 8;
    for (c = 0; c < nch; c++) ch[c] = xv_sample_ch(v, s, idx, c);
    if (v->matrix_ch == nch && v->matrix_ch > 0) {
        float ml = 0.0f, mr = 0.0f;
        for (c = 0; c < nch; c++) {
            ml += ch[c] * v->matrix[c * 2 + 0];
            mr += ch[c] * v->matrix[c * 2 + 1];
        }
        *l = ml; *r = mr;
        return;
    }
    switch (nch) {
    case 1: *l = *r = ch[0]; break;
    case 3: *l = ch[0] + ch[2] * 0.7071f; *r = ch[1] + ch[2] * 0.7071f; break;
    case 4: *l = ch[0] + ch[2] * 0.7071f; *r = ch[1] + ch[3] * 0.7071f; break;
    case 5: *l = ch[0] + ch[2] * 0.7071f + ch[3] * 0.7071f;
            *r = ch[1] + ch[2] * 0.7071f + ch[4] * 0.7071f; break;
    case 6: *l = ch[0] + ch[2] * 0.7071f + ch[3] * 0.5f + ch[4] * 0.7071f;
            *r = ch[1] + ch[2] * 0.7071f + ch[3] * 0.5f + ch[5] * 0.7071f; break;
    case 7: *l = ch[0] + ch[2] * 0.7071f + ch[4] * 0.7071f + ch[6] * 0.7071f;
            *r = ch[1] + ch[2] * 0.7071f + ch[5] * 0.7071f + ch[6] * 0.7071f; break;
    default:
        if (nch >= 8) {
            *l = ch[0] + ch[2] * 0.7071f + ch[4] * 0.7071f + ch[6] * 0.7071f;
            *r = ch[1] + ch[2] * 0.7071f + ch[5] * 0.7071f + ch[7] * 0.7071f;
        } else { *l = ch[0]; *r = ch[1]; }
        break;
    }
}

static void xv_read_at(xv_voice *v, double p, float *l, float *r) {
    const xv_slice *s = &v->q[v->qhead];
    uint32_t n = (uint32_t)(s->end - s->start);
    double rel;
    uint32_t i0;
    float l0, r0, l1, r1, f;
    if (p < (double)s->start) p = (double)s->start;
    if (!n) { *l = *r = 0.0f; return; }
    rel = p - (double)s->start;
    i0 = (uint32_t)rel;
    if (i0 >= n) i0 = n - 1;
    f = (float)(rel - (double)i0);
    xv_frame_stereo(v, s, i0, &l0, &r0);
    if (i0 + 1 < n) {
        xv_frame_stereo(v, s, i0 + 1, &l1, &r1);
    } else if (v->qcount > 1) {
        const xv_slice *nx = &v->q[(v->qhead + 1) % XV_QUEUE_MAX];
        if (nx->start == s->end && nx->end > nx->start) xv_frame_stereo(v, nx, 0, &l1, &r1);
        else { l1 = l0; r1 = r0; }
    } else { l1 = l0; r1 = r0; }
    *l = l0 + (l1 - l0) * f;
    *r = r0 + (r1 - r0) * f;
}

/* FNV-1a over every second word of a slice. Cheap enough to run per submit
 * (1024 reads for a 8192-byte buffer) and it answers one question the probe
 * cannot otherwise: was the payload the host mixes the same bytes the engine
 * handed over at SubmitSourceBuffer, or did the engine rewrite the buffer while
 * it was still queued? */
static uint32_t xv_slice_hash(uint32_t data, uint32_t frames, uint32_t align) {
    uint32_t h = 2166136261u, i, words;
    if (!data || !align || align < 4) return 0;
    words = frames * (align / 4);
    for (i = 0; i < words; i += 2) {
        uint32_t va = data + i * 4;
        if (!xv_readable(va, 4)) return 0;
        h = (h ^ MEM32(va)) * 16777619u;
    }
    return h;
}

/* ------------------------------------- the Hw block boundary, measured
 * The Hw output device (guest 0x0088B000; its IXAudio2VoiceCallback object is
 * device+0x28, vtable 0x0091ECC8) renders exactly one block per mixer tick:
 *
 *   0x008872D0  zeroes the count at device+0x1E, calls 0x00887200, and then
 *               jumps to vtable slot 7 (0x0088B510) when the count is > 0,
 *               or to slot 8 (0x0088B5F0) when the mix produced nothing;
 *   0x00887200  mixes into the buffer at device+0x10 and writes the block's
 *               frame count to device+0x1E from [mixer+0x424] (configured
 *               0x400 = 1024 by 0x00887320/0x0088663F);
 *   0x0088B510  copies exactly that many frames from device+0x10 into the
 *               ring slot at the fill cursor (device+0x44), clamped, and
 *               advances the fill cursor;
 *   0x0088B440  the feed: submits ring slots through the submit cursor
 *               (device+0x46) with AudioBytes = block_align << 10 always.
 *
 * So the engine does expose, per block, how many frames it filled - the same
 * number the fill used - and this probe reports it next to the declared
 * buffer, so a stale tail is a measurement instead of an inference. */
static int xv_hw_fields(const xv_voice *v, uint32_t *count, uint32_t *fill,
                        uint32_t *cur, uint32_t *credits, uint32_t *mix,
                        uint32_t *ba) {
    uint32_t dev;
    if (v->cb_kind != 2 || !v->cb) return 0;
    dev = v->cb - 0x28;
    if (!xv_readable(dev, 0x70)) return 0;
    *count = MEM32(dev + 0x1E) & 0xFFFFu;
    *fill = MEM32(dev + 0x44) & 0xFFFFu;
    *cur = MEM32(dev + 0x46) & 0xFFFFu;
    *credits = MEM32(dev + 0x68);
    *mix = MEM32(dev + 0x10);
    *ba = MEM32(dev + 0x6C) & 0xFFFFu;
    return 1;
}

/* Class profile of one range of a slot, channel 0. Used for the tail the
 * engine's count says it did not fill in this block. */
static void xv_profile_range(const char *tag, uint32_t data, uint32_t align,
                             uint32_t from, uint32_t to) {
    uint32_t zero_n = 0, nan_n = 0, huge_n = 0, fin_n = 0, k;
    if (!data || !align || from >= to) return;
    for (k = from; k < to; k++) {
        float x;
        uint32_t va = data + k * align;
        if (!xv_readable(va, 4)) break;
        memcpy(&x, ka_host(va), 4);
        if (x == 0.0f) zero_n++;
        else if (!isfinite(x)) nan_n++;
        else if (fabsf(x) > 2.0f) huge_n++;
        else fin_n++;
    }
    xv_log("  %s [%u..%u): zero %u, non-finite %u, |x|>2 %u, finite %u",
           tag, from, to, zero_n, nan_n, huge_n, fin_n);
}

/* The engine's declared fill next to the host's declared buffer. Logged once
 * per submit at verbose >= 2; a short block is logged once per voice with the
 * committed/stale split, which is the whole question this probe exists for. */
static void xv_log_hw_block(xv_voice *v, uint32_t frames, uint32_t data) {
    uint32_t count, fill, cur, credits, mix, ba;
    if (!xv_hw_fields(v, &count, &fill, &cur, &credits, &mix, &ba)) return;
    if (g_verbose >= 2)
        xv_log("hw block(%d): frames=%u count=%u fill=%u cur=%u credits=%u "
               "mix=%08X ba=%u slot=%08X",
               (int)(v - g_v), frames, count, fill, cur, credits, mix, ba, data);
    if (count && count < frames && v->hw_short < 4) {
        v->hw_short++;
        xv_log("hw boundary(%d): slot %08X declares %u frames, engine count %u "
               "- committed [0..%u) from the mix, uncommitted tail [%u..%u)",
               (int)(v - g_v), data, frames, count, count, count, frames);
        xv_profile_range("tail", data, ba, count, frames);
        xv_profile_range("head", data, ba, 0, count < 256 ? count : 256);
    }
}

static void xv_mix_voice(xv_voice *v, float *out, int frames) {
    double step = (double)v->rate * (double)(v->ratio > 0.0f ? v->ratio : 1.0f) /
                  (double)KORRA_AUDIO_RATE;
    int i;
    if (!(step > 0.0)) step = 1.0;
    for (i = 0; i < frames; i++) {
        float l, r;
        if (v->qcount && v->pos >= (double)v->q[v->qhead].end) xv_voice_retire_consumed(v);
        if (!v->qcount) {
            if (v->had_data && !v->dry) {
                v->dry = 1;
                korra_audio_sink_note_underrun();
            }
            return;
        }
        xv_read_at(v, v->pos, &l, &r);
        out[2 * i] += l * v->volume;
        out[2 * i + 1] += r * v->volume;
        /* First three per voice out of range: a sample that leaves [-1,1] far
         * enough can only be a read of the wrong memory, not the engine's own
         * level. NaN/Inf are included - fabsf(NaN) > 2 is false, which is why
         * the old probe missed the sink's nan/186 dBFS reading. The slice
         * bounds and the raw word name the source. */
        if (v->oor_warned < 3 &&
            (!isfinite(l) || !isfinite(r) || fabsf(l) > 2.0f || fabsf(r) > 2.0f)) {
            const xv_slice *s = &v->q[v->qhead];
            uint64_t i0 = (uint64_t)v->pos;
            uint32_t raw = 0;
            uint32_t rel = 0;
            uint32_t now_hash = 0;
            v->oor_warned++;
            if (s->data && i0 >= s->start) {
                rel = (uint32_t)(i0 - s->start);
                uint32_t va = s->data + rel * v->block_align;
                if (xv_readable(va, 4)) raw = MEM32(va);
            }
            if (s->data)
                now_hash = xv_slice_hash(s->data, (uint32_t)(s->end - s->start),
                                         v->block_align);
            xv_log("voice %d bad sample l=%g r=%g (pos %.1f, slice [%llu,%llu), "
                   "i0=%llu, rel=%u, raw=%08X, ch %u, bits %u, float %u, matrix %u, "
                   "q=%u, rate %u, ratio %.3f, data %08X, hash %08X (at submit %08X%s))",
                   (int)(v - g_v), (double)l, (double)r, v->pos,
                   (unsigned long long)s->start, (unsigned long long)s->end,
                   (unsigned long long)i0, rel, raw, v->channels, v->bits, v->is_float,
                   v->matrix_ch, v->qcount, v->rate, (double)v->ratio, s->data,
                   now_hash, s->hash,
                   now_hash && s->hash && now_hash != s->hash
                       ? " - REWRITTEN since submit" : "");
            /* The words around the sample, so the burst's shape can be read off
             * one line: contiguous (a header/tail) or periodic (a stride bug). */
            if (s->data) {
                char buf[640];
                int n = 0, w;
                uint64_t len = s->end - s->start;
                uint64_t base = (rel > 8) ? rel - 8 : 0;
                for (w = 0; w < 32 && n < (int)sizeof buf - 12; w++) {
                    uint64_t r2 = base + (uint64_t)w;
                    uint32_t va, val;
                    if (r2 >= len) break;
                    va = s->data + (uint32_t)(r2 * v->block_align);
                    if (!xv_readable(va, 4)) break;
                    val = MEM32(va);
                    n += snprintf(buf + n, sizeof buf - (size_t)n, "%s%08X",
                                  w ? " " : "", val);
                }
                xv_log("  ring words rel %llu.. (%llu frames in slice, data%%4096=0x%X): %s",
                       (unsigned long long)base, (unsigned long long)len,
                       (unsigned)(s->data & 0xFFF), buf);
            }
            /* Every channel of the offending frame. The slice profile below
             * only scans channel 0, but l/r are the engine's whole matrix mix,
             * so a huge value in channel 1..5 shows up in l/r while channel 0
             * looks clean - this line says which channel carried it. */
            if (s->data && v->is_float && v->bits == 32 && v->channels > 1) {
                char fbuf[256];
                int fn = 0;
                uint32_t cw, nch = v->channels > 8 ? 8 : v->channels;
                uint32_t fva = s->data + rel * v->block_align;
                for (cw = 0; cw < nch && fn < (int)sizeof fbuf - 16; cw++) {
                    uint32_t va2 = fva + cw * 4, val = 0;
                    if (xv_readable(va2, 4)) val = MEM32(va2);
                    fn += snprintf(fbuf + fn, sizeof fbuf - (size_t)fn, "%s%08X",
                                   cw ? " " : "", val);
                }
                xv_log("  frame words (rel %u, %u ch): %s", rel, nch, fbuf);
            }
            /* Whole-slice profile: the first bad sample does not say whether the
             * bad words are a tail the engine never filled, a contiguous block,
             * or scattered. Count the classes and the span of each. */
            if (s->data && v->bad_slices < 12) {
                uint32_t zero_n = 0, nan_n = 0, huge_n = 0, fin_n = 0;
                uint32_t zero_a = 0xFFFFFFFFu, zero_b = 0, nan_a = 0xFFFFFFFFu, nan_b = 0;
                uint64_t len = s->end - s->start, k;
                v->bad_slices++;
                for (k = 0; k < len; k++) {
                    float x;
                    uint32_t va = s->data + (uint32_t)(k * v->block_align);
                    if (!xv_readable(va, 4)) break;
                    memcpy(&x, ka_host(va), 4);
                    if (x == 0.0f) {
                        zero_n++;
                        if (k < zero_a) zero_a = (uint32_t)k;
                        zero_b = (uint32_t)k;
                    } else if (!isfinite(x)) {
                        nan_n++;
                        if (k < nan_a) nan_a = (uint32_t)k;
                        nan_b = (uint32_t)k;
                    } else if (fabsf(x) > 2.0f) huge_n++;
                    else fin_n++;
                }
                xv_log("  slice profile: %llu frames - zero %u [%d..%u], non-finite %u [%d..%u], "
                       "|x|>2 %u, finite %u, q=%u pos=%.0f",
                       (unsigned long long)len, zero_n,
                       zero_n ? (int)zero_a : -1, zero_n ? zero_b : 0,
                       nan_n, nan_n ? (int)nan_a : -1, nan_n ? nan_b : 0,
                       huge_n, fin_n, v->qcount, v->pos);
                /* The class above is channel 0 only. A multi-channel voice can
                 * carry the out-of-range word in another channel (l/r are the
                 * matrix mix), so name the channel and the span: a tail is one
                 * contiguous span at the end, a decoder poison is a block. */
                if (v->is_float && v->bits == 32 && v->channels > 2) {
                    char cbuf[512];
                    int cn = 0;
                    uint32_t cw, nch = v->channels > 8 ? 8 : v->channels;
                    for (cw = 0; cw < nch && cn < (int)sizeof cbuf - 40; cw++) {
                        uint32_t hn = 0, ha = 0xFFFFFFFFu, hb = 0, nn = 0;
                        uint64_t kk;
                        float mx = 0.0f;
                        for (kk = 0; kk < len; kk++) {
                            float x;
                            uint32_t va2 = s->data + (uint32_t)kk * v->block_align + cw * 4;
                            if (!xv_readable(va2, 4)) break;
                            memcpy(&x, ka_host(va2), 4);
                            if (!isfinite(x)) nn++;
                            else {
                                if (fabsf(x) > mx) mx = fabsf(x);
                                if (fabsf(x) > 2.0f) {
                                    hn++;
                                    if (kk < ha) ha = (uint32_t)kk;
                                    hb = (uint32_t)kk;
                                }
                            }
                        }
                        cn += snprintf(cbuf + cn, sizeof cbuf - (size_t)cn,
                                       "%sc%u >2 %u", cw ? " " : "", cw, hn);
                        if (hn) cn += snprintf(cbuf + cn, sizeof cbuf - (size_t)cn,
                                               " [%u..%u]", ha, hb);
                        cn += snprintf(cbuf + cn, sizeof cbuf - (size_t)cn,
                                       " nan %u max %g", nn, (double)mx);
                    }
                    xv_log("  channels: %s", cbuf);
                }
            }
        }
        v->pos += step;
    }
    if (v->qcount) v->dry = 0;
}

/* The sink's one output thread calls this for exactly the frames the device
 * wants. No guest code runs here; the only guest memory touched is CRI's
 * completion counter (one aligned dword). */
void ka_xa2_render(float *dst, int frames) {
    int i;
    if (!dst || frames <= 0) return;
    memset(dst, 0, (size_t)frames * KORRA_AUDIO_CHANNELS * sizeof(float));
    xv_lock();
    for (i = 0; i < XV_MAX_VOICES; i++) {
        xv_voice *v = &g_v[i];
        if (v->magic == XV_MAGIC && v->kind == XV_KIND_SOURCE && v->running)
            xv_mix_voice(v, dst, frames);
    }
    if (g_dev.master >= 0 && g_v[g_dev.master].magic == XV_MAGIC &&
        g_v[g_dev.master].volume != 1.0f) {
        float g = g_v[g_dev.master].volume;
        int n = frames * KORRA_AUDIO_CHANNELS, k;
        for (k = 0; k < n; k++) dst[k] *= g;
    }
    xv_unlock();
}

/* Open the device on the first real buffer, not on device creation: the game
 * creates and destroys a whole XAudio2 device per movie, and the Wwise device
 * exists from boot with nothing to play. */
static void xv_ensure_sink(void) {
    static int tried;
    if (xv_sink_live()) return;
    if (__atomic_exchange_n(&tried, 1, __ATOMIC_SEQ_CST)) return;
    korra_audio_sink_start(ka_xa2_render);
}

/* ---------------------------------------------------------------- helpers */
static xv_voice *xv_slot(void) {
    int i;
    for (i = 0; i < XV_MAX_VOICES; i++)
        if (g_v[i].magic != XV_MAGIC) return &g_v[i];
    return NULL;
}

/* A voice object is [vtable][tag]; tag is 1 + the state index. Validate both
 * the object and the tag: a stale object after DestroyVoice must not resolve. */
static xv_voice *xv_from_object(uint32_t obj) {
    uint32_t tag;
    if (!xv_readable(obj, 8)) return NULL;
    tag = MEM32(obj + 4);
    if (!tag || tag > XV_MAX_VOICES) return NULL;
    if (g_v[tag - 1].magic != XV_MAGIC) return NULL;
    if (g_v[tag - 1].obj != obj) return NULL;
    return &g_v[tag - 1];
}

static uint32_t xv_new_voice(uint32_t kind) {
    xv_voice *v;
    uint32_t obj, vt;
    v = xv_slot();
    if (!v) return 0;
    obj = ka_guest_alloc(8, 4);
    if (!obj) return 0;
    memset(v, 0, sizeof *v);
    v->magic = XV_MAGIC;
    v->kind = kind;
    v->obj = obj;
    v->volume = 1.0f;
    v->ratio = 1.0f;
    v->rate = 48000;
    v->cb_kind = 0;
    vt = (kind == XV_KIND_SOURCE) ? g_vt_src : g_vt_master;
    MEM32(obj) = vt;
    MEM32(obj + 4) = (uint32_t)(v - g_v) + 1;
    return obj;
}

static void xv_kill_voice(xv_voice *v) {
    uint32_t obj;
    if (!v || v->magic != XV_MAGIC) return;
    obj = v->obj;
    if (g_dev.master == (int)(v - g_v)) g_dev.master = -1;
    v->magic = 0;
    if (obj) ka_guest_free(obj);
}

/* ------------------------------------------------------- IXAudio2 device
 * dev_call(slot) performs one IXAudio2 method; the thunk below it only names
 * the slot and pops `this` + that method's parameters. */
static uint32_t dev_call(int slot) {
    uint32_t self = A32(0);
    uint32_t hr = XV_HR_OK;

    xv_init();
    if (!xv_readable(self, 8) || MEM32(self) != g_vt_dev) return XV_HR_INVALID;

    switch (slot) {
    case 0: {   /* QueryInterface(this, riid, ppv) */
        uint32_t riid = A32(1), out = A32(2);
        uint32_t ok = 0;
        if (riid && xv_readable(riid, 16)) {
            /* IXAudio2 {8bcf1f58-9fe7-4583-8ac6-e2adc465c8bb} */
            static const uint8_t iid_ixaudio2[16] = {
                0x58, 0x1f, 0xcf, 0x8b, 0xe7, 0x9f, 0x83, 0x45,
                0x8a, 0xc6, 0xe2, 0xad, 0xc4, 0x65, 0xc8, 0xbb
            };
            static const uint8_t iid_iunknown[16] = {
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46
            };
            const uint8_t *b = (const uint8_t *)ka_host(riid);
            ok = !memcmp(b, iid_ixaudio2, 16) || !memcmp(b, iid_iunknown, 16);
        }
        if (out) MEM32(out) = ok ? self : 0;
        if (ok) { xv_lock(); g_dev.refs++; xv_unlock(); }
        else hr = XV_HR_NOIFACE;
        break;
    }
    case 1:     /* AddRef */
        xv_lock(); hr = ++g_dev.refs; xv_unlock();
        break;
    case 2:     /* Release */
        xv_lock();
        hr = g_dev.refs ? --g_dev.refs : 0;
        xv_unlock();
        if (g_verbose > 1) xv_log("Release(device) -> %u", hr);
        break;
    case 3: {   /* GetDeviceCount(this, out) - CRI refuses 0 */
        uint32_t out = A32(1);
        if (!out) return XV_HR_INVALID;
        MEM32(out) = 1;
        if (g_verbose > 1) xv_log("GetDeviceCount -> 1");
        break;
    }
    case 4: {   /* GetDeviceDetails(this, index, details) */
        uint32_t index = A32(1), d = A32(2);
        if (!d || index != 0) return XV_HR_INVALID;
        if (!xv_readable(d, 0x42C)) return XV_HR_INVALID;
        memset(ka_host(d), 0, 0x42C);
        /* DeviceID / DisplayName are WCHAR[256] each; left empty is honest. */
        MEM32(d + 0x400) = 0;                        /* Role: GlobalDefaultDevice */
        MEM16(d + 0x404) = 0xFFFE;                   /* WAVE_FORMAT_EXTENSIBLE */
        MEM16(d + 0x406) = 2;                        /* nChannels - what the Hw
                                                      * mixer reads to pick 3 vs 0x3f */
        MEM32(d + 0x408) = 48000;
        MEM32(d + 0x40C) = 48000 * 8;
        MEM16(d + 0x410) = 8;
        MEM16(d + 0x412) = 32;
        MEM16(d + 0x414) = 22;                       /* cbSize */
        MEM16(d + 0x416) = 32;                       /* wValidBitsPerSample */
        MEM32(d + 0x418) = 3;                        /* FL | FR */
        MEM32(d + 0x41C) = 3;                        /* SubFormat Data1 = float */
        MEM32(d + 0x420) = 0x00100000u;              /* Data2/Data3 */
        MEM32(d + 0x424) = 0xAA000080u;              /* Data4[0..3] */
        MEM32(d + 0x428) = 0x719B3800u;              /* Data4[4..7] */
        if (g_verbose > 1) xv_log("GetDeviceDetails(0) -> 2 ch 48 kHz float");
        break;
    }
    case 5:     /* Initialize(this, flags, processor) */
        g_dev.initialized = 1;
        g_dev.magic = XV_MAGIC;
        if (g_verbose) xv_log("Initialize(flags=0x%X, processor=0x%X)",
                              A32(1), A32(2));
        break;
    case 6:     /* RegisterForCallbacks - stored, never called (2.7 contract) */
        g_dev.engine_cb = A32(1);
        if (g_verbose) xv_log("RegisterForCallbacks(%08X)", g_dev.engine_cb);
        break;
    case 7:     /* UnregisterForCallbacks */
        if (g_dev.engine_cb == A32(1)) g_dev.engine_cb = 0;
        if (g_verbose) xv_log("UnregisterForCallbacks(%08X)", A32(1));
        break;
    case 8: {   /* CreateSourceVoice(this, pp, fmt, flags, maxratio, cb, sends, chain) */
        uint32_t out = A32(1), fmt = A32(2), cb = A32(5);
        uint32_t ch, bits, tag, mask = 0, obj;
        float maxratio = xv_f(A32(4));
        xv_voice *v;
        if (!out) return XV_HR_INVALID;
        MEM32(out) = 0;
        if (!fmt || !xv_readable(fmt, 18)) {
            xv_log("CreateSourceVoice: unreadable WAVEFORMATEX %08X", fmt);
            return XV_HR_INVALID;
        }
        ch = MEM16(fmt + 2);
        tag = MEM16(fmt);
        bits = MEM16(fmt + 14);
        xv_lock();
        obj = xv_new_voice(XV_KIND_SOURCE);
        if (!obj) { xv_unlock(); return XV_HR_NOMEM; }
        v = xv_from_object(obj);
        v->rate = MEM32(fmt + 4) ? MEM32(fmt + 4) : 48000;
        v->channels = ch ? ch : 2;
        v->fmt_tag = tag;
        v->bits = bits ? bits : 16;
        v->is_float = (tag == 3);            /* WAVE_FORMAT_IEEE_FLOAT */
        if (tag == 0xFFFE && MEM16(fmt + 16) >= 22) {
            v->is_float = (MEM32(fmt + 24) == 3);   /* SubFormat Data1 */
            mask = MEM32(fmt + 20);                 /* dwChannelMask */
        }
        v->channel_mask = mask;
        v->block_align = MEM16(fmt + 12) ? MEM16(fmt + 12)
                                         : v->channels * (v->bits / 8);
        v->cb = cb;
        v->cb_kind = 0;
        if (cb && xv_readable(cb, 4)) {
            uint32_t vt = MEM32(cb);
            if (vt && xv_readable(vt, 5 * 4)) {
                v->cb_kind = (MEM32(vt + 4 * 4) == XV_CB_ONBUFFEREND_VA) ? 1 : 2;
            }
        }
        MEM32(out) = obj;
        xv_unlock();
        if (v->cb_kind == 2) xv_cb_ensure_thread();
        if (g_verbose)
            xv_log("CreateSourceVoice(%u ch, %u Hz, %u bits %s, tag 0x%04X, mask 0x%X, "
                   "maxratio %.3f) -> #%d (cb=%08X %s)",
                   MEM16(fmt + 2), MEM32(fmt + 4), MEM16(fmt + 14),
                   v->is_float ? "float" : "pcm", tag, mask, maxratio,
                   (int)(v - g_v), cb,
                   v->cb_kind == 1 ? "CRI counter" : v->cb_kind == 2 ? "other" : "none");
        /* The Hw output device copies its prefill parameters out of the game's
         * sound table at creation (0x0088B2CD reads [0xCA243C], walks entries
         * and takes the first zero key). Read it here -- during the device's
         * own Init -- and again from CommitChanges, where the cfg store that
         * follows Init becomes visible. */
        if (mask == 3) xv_table_sample("voice-create");
        break;
    }
    case 9: {   /* CreateSubmixVoice(this, pp, in_ch, rate, flags, stage, sends, chain) */
        uint32_t out = A32(1), ch = A32(2), rate = A32(3), obj;
        xv_voice *v;
        if (!out) return XV_HR_INVALID;
        xv_lock();
        obj = xv_new_voice(XV_KIND_SUBMIX);
        if (!obj) { xv_unlock(); return XV_HR_NOMEM; }
        v = xv_from_object(obj);
        v->channels = ch ? ch : 2;
        v->rate = rate ? rate : 48000;
        MEM32(out) = obj;
        xv_unlock();
        if (g_verbose) xv_log("CreateSubmixVoice(%u ch, %u Hz)", ch, rate);
        break;
    }
    case 10: {  /* CreateMasteringVoice(this, pp, in_ch, rate, flags, devid, chain) */
        uint32_t out = A32(1), in_ch = A32(2), in_rate = A32(3), obj;
        xv_voice *v;
        if (!out) return XV_HR_INVALID;
        MEM32(out) = 0;
        xv_lock();
        if (g_dev.master >= 0 && g_v[g_dev.master].magic == XV_MAGIC) {
            MEM32(out) = g_v[g_dev.master].obj;
            xv_unlock();
            return XV_HR_OK;
        }
        obj = xv_new_voice(XV_KIND_MASTER);
        if (!obj) { xv_unlock(); xv_log("CreateMasteringVoice: no free voice"); return XV_HR_NOMEM; }
        v = xv_from_object(obj);
        /* Report 2 ch / 48 kHz even when asked for 0: the engine then downmixes
         * 5.1 itself and the sink stays stereo (docs/AUDIO_ANDROID.md 3.3). */
        v->channels = in_ch ? in_ch : 2;
        if (v->channels != 2) v->channels = 2;
        v->rate = in_rate ? in_rate : 48000;
        v->block_align = v->channels * 4;
        g_dev.master = (int)(v - g_v);
        MEM32(out) = obj;
        xv_unlock();
        if (g_verbose) xv_log("CreateMasteringVoice(in=%u ch, %u Hz) -> 2 ch 48 kHz",
                              in_ch, in_rate);
        break;
    }
    case 11:    /* StartEngine */
        if (g_verbose > 1) xv_log("StartEngine()");
        break;
    case 12:    /* StopEngine */
        if (g_verbose) xv_log("StopEngine()");
        break;
    case 13: {  /* CommitChanges(this, opset) - the engine's completion wait */
        uint32_t opset = A32(1);
        xv_lock();
        xv_pump_all();
        xv_unlock();
        xv_table_sample("commit");
        if (g_verbose > 1) xv_log("CommitChanges(0x%X) from 0x%08X", opset, g_cur_func);
        break;
    }
    case 14: {  /* GetPerformanceData(this, data): 0x60 bytes in 2.7 */
        uint32_t d = A32(1);
        if (d && xv_readable(d, 0x60)) memset(ka_host(d), 0, 0x60);
        break;
    }
    case 15:    /* SetDebugConfiguration */
        break;
    default:
        hr = XV_HR_INVALID;
        break;
    }
    return hr;
}

#define DEV_THUNK(name, slot, argc) \
    static void name(void) { uint32_t hr = dev_call(slot); RET(hr, argc); }

DEV_THUNK(x_dev_0,  0, 3)  DEV_THUNK(x_dev_1,  1, 1)  DEV_THUNK(x_dev_2,  2, 1)
DEV_THUNK(x_dev_3,  3, 2)  DEV_THUNK(x_dev_4,  4, 3)  DEV_THUNK(x_dev_5,  5, 3)
DEV_THUNK(x_dev_6,  6, 2)  DEV_THUNK(x_dev_7,  7, 2)  DEV_THUNK(x_dev_8,  8, 8)
DEV_THUNK(x_dev_9,  9, 8)  DEV_THUNK(x_dev_10, 10, 7) DEV_THUNK(x_dev_11, 11, 1)
DEV_THUNK(x_dev_12, 12, 1) DEV_THUNK(x_dev_13, 13, 2) DEV_THUNK(x_dev_14, 14, 2)
DEV_THUNK(x_dev_15, 15, 3)

/* ------------------------------------------------- IXAudio2Voice (0..18) */
static uint32_t voice_call(int slot) {
    uint32_t self = A32(0);
    xv_voice *v;
    uint32_t hr = XV_HR_OK;

    if (!(v = xv_from_object(self))) return XV_HR_INVALID;

    switch (slot) {
    case 0: {   /* GetVoiceDetails(this, out) */
        uint32_t out = A32(1);
        if (!out || !xv_readable(out, 12)) return XV_HR_INVALID;
        MEM32(out) = 0;                 /* CreationFlags */
        MEM32(out + 4) = v->channels;
        MEM32(out + 8) = v->rate;
        break;
    }
    case 1: case 2: case 3: case 4: case 6: case 8: case 10:
        break;                          /* effects/sends: accepted, not modelled */
    case 5: {   /* GetEffectState(this, index, enabled) */
        uint32_t out = A32(2);
        if (out) MEM32(out) = 0;
        break;
    }
    case 7: {   /* GetEffectParameters(this, index, params, size) */
        uint32_t p = A32(2), size = A32(3);
        if (p && size && xv_readable(p, size)) memset(ka_host(p), 0, size);
        break;
    }
    case 9: {   /* GetFilterParameters(this, out) */
        uint32_t p = A32(1);
        if (p && xv_readable(p, 12)) memset(ka_host(p), 0, 12);
        break;
    }
    case 11: {  /* GetOutputFilterParameters(this, dest, params) */
        uint32_t p = A32(2);
        if (p && xv_readable(p, 12)) memset(ka_host(p), 0, 12);
        break;
    }
    case 12: {  /* SetVolume(this, volume, opset) */
        float vol = xv_f(A32(1));
        xv_lock(); v->volume = vol; xv_unlock();
        if (g_verbose) xv_log("SetVolume(%d) = %.3f", (int)(v - g_v), vol);
        break;
    }
    case 13: {  /* GetVolume(this, out) */
        uint32_t out = A32(1);
        if (out) MEM32(out) = xv_bits(v->volume);
        break;
    }
    case 14: {  /* SetChannelVolumes(this, ch, vols, opset) */
        break;
    }
    case 15: {  /* GetChannelVolumes(this, ch, vols) */
        uint32_t ch = A32(1), p = A32(2), i;
        if (p && ch && xv_readable(p, ch * 4))
            for (i = 0; i < ch; i++) MEM32(p + i * 4) = xv_bits(1.0f);
        break;
    }
    case 16: {  /* SetOutputMatrix(this, dest, src, dst, levels, opset) */
        uint32_t src = A32(2), dst = A32(3), levels = A32(4);
        uint32_t i;
        if (!levels || dst != KORRA_AUDIO_CHANNELS || src < 1 || src > 8) {
            xv_lock(); v->matrix_ch = 0; xv_unlock();
            break;
        }
        if (!xv_readable(levels, src * dst * 4)) {
            if (g_verbose) xv_log("SetOutputMatrix: unreadable levels %08X", levels);
            return XV_HR_INVALID;
        }
        xv_lock();
        v->matrix_ch = src;
        for (i = 0; i < src * dst && i < 16; i++)
            v->matrix[i] = xv_f(MEM32(levels + i * 4));
        xv_unlock();
        if (g_verbose) {
            char m[160];
            int n = 0, k;
            for (k = 0; k < (int)(src * dst) && k < 16 && n < (int)sizeof m - 16; k++)
                n += snprintf(m + n, sizeof m - (size_t)n, " %.3f", (double)v->matrix[k]);
            xv_log("SetOutputMatrix(%d): %u x %u ->%s", (int)(v - g_v), src, dst, m);
        }
        break;
    }
    case 17: {  /* GetOutputMatrix(this, dest, src, dst, levels) */
        uint32_t src = A32(2), dst = A32(3), p = A32(4), i, n = src * dst;
        for (i = 0; p && i < n; i++)
            MEM32(p + i * 4) = (v->matrix_ch == src && i < v->matrix_ch * 2)
                                   ? xv_bits(v->matrix[i]) : 0;
        break;
    }
    case 18:    /* DestroyVoice */
        xv_lock();
        if (g_verbose) xv_log("DestroyVoice(%d)", (int)(v - g_v));
        xv_kill_voice(v);
        xv_unlock();
        break;
    default:
        hr = XV_HR_INVALID;
        break;
    }
    return hr;
}

#define VOICE_THUNK(name, slot, argc) \
    static void name(void) { uint32_t hr = voice_call(slot); RET(hr, argc); }

VOICE_THUNK(x_vo_0,  0, 2)  VOICE_THUNK(x_vo_1,  1, 2)  VOICE_THUNK(x_vo_2,  2, 2)
VOICE_THUNK(x_vo_3,  3, 3)  VOICE_THUNK(x_vo_4,  4, 3)  VOICE_THUNK(x_vo_5,  5, 3)
VOICE_THUNK(x_vo_6,  6, 5)  VOICE_THUNK(x_vo_7,  7, 4)  VOICE_THUNK(x_vo_8,  8, 3)
VOICE_THUNK(x_vo_9,  9, 2)  VOICE_THUNK(x_vo_10, 10, 4) VOICE_THUNK(x_vo_11, 11, 3)
VOICE_THUNK(x_vo_12, 12, 3) VOICE_THUNK(x_vo_13, 13, 2) VOICE_THUNK(x_vo_14, 14, 4)
VOICE_THUNK(x_vo_15, 15, 3) VOICE_THUNK(x_vo_16, 16, 6) VOICE_THUNK(x_vo_17, 17, 5)
VOICE_THUNK(x_vo_18, 18, 1)

/* ------------------------------------------- IXAudio2SourceVoice (19..28) */
static uint32_t src_call(int slot) {
    uint32_t self = A32(0);
    xv_voice *v;
    uint32_t hr = XV_HR_OK;

    if (!(v = xv_from_object(self))) return XV_HR_INVALID;
    if (v->kind != XV_KIND_SOURCE) return XV_HR_INVALID;

    switch (slot) {
    case 19: {  /* Start(this, flags, opset) */
        uint32_t flags = A32(1);
        xv_lock();
        if (!v->running) {
            v->base = v->played;
            v->t0ns = xv_now_ns();
            v->running = 1;
            if (!v->qcount) v->pos = (double)v->played;
        }
        xv_unlock();
        if (g_verbose > 1)
            xv_log("Start(%d, flags=0x%X) from 0x%08X", (int)(v - g_v), flags, g_cur_func);
        break;
    }
    case 20: {  /* Stop(this, flags, opset) */
        xv_lock();
        if (v->running) {
            v->played = xv_sink_live() ? (uint64_t)v->pos : xv_voice_now(v);
            v->running = 0;
            v->dry = 0;
        }
        xv_unlock();
        if (g_verbose > 1) xv_log("Stop(%d)", (int)(v - g_v));
        break;
    }
    case 21: {  /* SubmitSourceBuffer(this, buf, wma) */
        uint32_t buf = A32(1);
        uint32_t flags, bytes, data, ctx, play_begin, play_len, loop_count, total;
        uint32_t slot_data = 0;
        uint64_t frames, start, end, cursor;
        xv_slice *slot_p;
        if (!buf || !xv_readable(buf, 36)) return XV_HR_INVALID;
        flags = MEM32(buf + 0);
        bytes = MEM32(buf + 4);
        data = MEM32(buf + 8);
        play_begin = MEM32(buf + 12);
        play_len = MEM32(buf + 16);
        loop_count = MEM32(buf + 28);
        ctx = MEM32(buf + 32);

        total = v->block_align ? bytes / v->block_align : 0;
        if (play_begin > total) play_begin = total;
        if (play_len == 0) play_len = total - play_begin;
        if (play_len > total - play_begin) play_len = total - play_begin;
        frames = play_len ? play_len : 1;   /* a zero-length buffer still retires */

        if (loop_count > 1 && g_verbose)
            xv_log("SubmitSourceBuffer(%d): LoopCount %u not implemented (plays once)",
                   (int)(v - g_v), loop_count);

        xv_lock();
        if (v->qcount == XV_QUEUE_MAX) {
            v->qhead = (v->qhead + 1) % XV_QUEUE_MAX;
            v->qcount--;
            if (g_verbose && !v->overflow_logged) {
                uint32_t credits = (v->cb && xv_readable(v->cb + 0x40, 4))
                                       ? MEM32(v->cb + 0x40) : 0;
                v->overflow_logged = 1;
                xv_log("SubmitSourceBuffer(%d): queue full (credits=%u qcount=%u "
                       "pos=%.0f) - dropping oldest", (int)(v - g_v), credits,
                       v->qcount, v->pos);
            }
        }
        cursor = xv_voice_cursor(v);
        start = v->next_end > cursor ? v->next_end : cursor;
        end = start + frames;
        slot_p = &v->q[(v->qhead + v->qcount) % XV_QUEUE_MAX];
        slot_p->start = start;
        slot_p->end = end;
        slot_p->data = data + (uint32_t)((size_t)play_begin * v->block_align);
        slot_p->ctx = ctx;
        slot_p->hash = xv_slice_hash(slot_p->data, (uint32_t)frames, v->block_align);
        v->qcount++;
        v->next_end = end;
        v->had_data = 1;
        v->dry = 0;
        v->submits++;
        slot_data = slot_p->data;
        xv_unlock();

        xv_log_hw_block(v, (uint32_t)frames, slot_data);

        xv_ensure_sink();

        if (g_verbose && v->submits == 1) {
            xv_log("SubmitSourceBuffer(%d): first buffer %u bytes, %u frames "
                   "(%u Hz, %u ch, %s), ctx=%08X",
                   (int)(v - g_v), bytes, (unsigned)frames, v->rate, v->channels,
                   v->is_float ? "float" : "pcm", ctx);
        } else if (g_verbose > 2) {
            xv_log("SubmitSourceBuffer(%d): %u frames, ctx=%08X",
                   (int)(v - g_v), (unsigned)frames, ctx);
        }
        break;
    }
    case 22:    /* FlushSourceBuffers(this) */
        xv_lock();
        if (!xv_sink_live()) xv_voice_retire(v, xv_voice_now(v));
        v->qhead = v->qcount = 0;
        v->next_end = v->played;
        v->pos = (double)v->played;
        v->last_ctx = 0;
        v->dry = 0;
        xv_unlock();
        if (g_verbose > 1) xv_log("FlushSourceBuffers(%d)", (int)(v - g_v));
        break;
    case 23:    /* Discontinuity(this) */
        xv_lock();
        if (!v->qcount) {
            v->next_end = xv_voice_cursor(v);
            v->pos = (double)v->next_end;
        }
        xv_unlock();
        break;
    case 24:    /* ExitLoop(this, opset) */
        break;
    case 25: {  /* GetState(this, state) - the 2.7 shape: one argument */
        uint32_t out = A32(1);
        uint64_t now;
        if (!out || !xv_readable(out, 16)) return XV_HR_OK;
        xv_lock();
        if (xv_sink_live()) {
            now = (uint64_t)v->pos;
        } else {
            now = xv_voice_now(v);
            xv_voice_retire(v, now);
        }
        if (now > v->next_end) now = v->next_end;
        MEM32(out + 0) = v->qcount ? v->q[v->qhead].ctx : v->last_ctx;
        MEM32(out + 4) = v->qcount;
        MEM32(out + 8) = (uint32_t)now;
        MEM32(out + 12) = (uint32_t)(now >> 32);
        /* Who polls a voice and how often is the one part of the picture the
         * submit log cannot show: a voice never fed looks the same whether the
         * engine polls it 60 times a second or not at all. */
        if (g_verbose >= 2) {
            v->gs_calls++;
            if (v->gs_calls <= 12 || v->gs_calls % 600 == 0)
                xv_log("GetState(%d): poll #%u queued=%u played=%llu from 0x%08X",
                       (int)(v - g_v), v->gs_calls, v->qcount,
                       (unsigned long long)now, g_cur_func);
        }
        if (g_verbose > 2 && v->qcount != v->last_logged_q) {
            v->last_logged_q = v->qcount;
            xv_log("GetState(%d): queued=%u played=%llu", (int)(v - g_v),
                   v->qcount, (unsigned long long)now);
        }
        xv_unlock();
        break;
    }
    case 26: {  /* SetFrequencyRatio(this, ratio, opset) */
        float ratio = xv_f(A32(1));
        if (!(ratio > 0.0f)) return XV_HR_INVALID;
        xv_lock();
        if (!xv_sink_live()) { v->base = xv_voice_now(v); v->t0ns = xv_now_ns(); }
        v->ratio = ratio;
        xv_unlock();
        break;
    }
    case 27: {  /* GetFrequencyRatio(this, out) */
        uint32_t out = A32(1);
        if (out) MEM32(out) = xv_bits(v->ratio);
        break;
    }
    case 28: {  /* SetSourceSampleRate(this, rate) */
        uint32_t rate = A32(1);
        if (!rate) return XV_HR_INVALID;
        xv_lock();
        if (!xv_sink_live()) { v->base = xv_voice_now(v); v->t0ns = xv_now_ns(); }
        v->rate = rate;
        xv_unlock();
        break;
    }
    default:
        hr = XV_HR_INVALID;
        break;
    }
    return hr;
}

#define SRC_THUNK(name, slot, argc) \
    static void name(void) { uint32_t hr = src_call(slot); RET(hr, argc); }

SRC_THUNK(x_src_19, 19, 3) SRC_THUNK(x_src_20, 20, 3) SRC_THUNK(x_src_21, 21, 3)
SRC_THUNK(x_src_22, 22, 1) SRC_THUNK(x_src_23, 23, 1) SRC_THUNK(x_src_24, 24, 2)
SRC_THUNK(x_src_25, 25, 2) SRC_THUNK(x_src_26, 26, 3) SRC_THUNK(x_src_27, 27, 2)
SRC_THUNK(x_src_28, 28, 2)

/* The three vtables, built on the first CoCreateInstance. */
static const xv_fn g_dev_fns[16] = {
    x_dev_0, x_dev_1, x_dev_2, x_dev_3, x_dev_4, x_dev_5, x_dev_6, x_dev_7,
    x_dev_8, x_dev_9, x_dev_10, x_dev_11, x_dev_12, x_dev_13, x_dev_14, x_dev_15
};
static const xv_fn g_voice_fns[19] = {
    x_vo_0, x_vo_1, x_vo_2, x_vo_3, x_vo_4, x_vo_5, x_vo_6, x_vo_7, x_vo_8,
    x_vo_9, x_vo_10, x_vo_11, x_vo_12, x_vo_13, x_vo_14, x_vo_15, x_vo_16,
    x_vo_17, x_vo_18
};
static const xv_fn g_src_fns[29] = {
    x_vo_0, x_vo_1, x_vo_2, x_vo_3, x_vo_4, x_vo_5, x_vo_6, x_vo_7, x_vo_8,
    x_vo_9, x_vo_10, x_vo_11, x_vo_12, x_vo_13, x_vo_14, x_vo_15, x_vo_16,
    x_vo_17, x_vo_18,
    x_src_19, x_src_20, x_src_21, x_src_22, x_src_23, x_src_24, x_src_25,
    x_src_26, x_src_27, x_src_28
};

/* ------------------------------------------------------- factory entry
 * Called from misc_android.c's CoCreateInstance shim. riid/out are guest VAs.
 * Returns the HRESULT the guest sees. */
uint32_t ka_xaudio2_create_instance(uint32_t riid, uint32_t out) {
    static const uint8_t iid_ixaudio2[16] = {
        0x58, 0x1f, 0xcf, 0x8b, 0xe7, 0x9f, 0x83, 0x45,
        0x8a, 0xc6, 0xe2, 0xad, 0xc4, 0x65, 0xc8, 0xbb
    };
    static const uint8_t iid_iunknown[16] = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46
    };
    xv_init();
    xv_lock();
    if (!g_vt_dev) {
        g_vt_master = build_vt("XA2voice", 19, g_voice_fns);
        g_vt_src    = build_vt("XA2src", 29, g_src_fns);
        g_vt_dev    = build_vt("XA2dev", 16, g_dev_fns);
        g_dev.magic = XV_MAGIC;
        g_dev.master = -1;
        g_dev.refs = 0;
        g_obj_dev = ka_guest_alloc(8, 4);
        if (!g_vt_dev || !g_vt_master || !g_vt_src || !g_obj_dev) {
            xv_unlock();
            xv_log("CoCreateInstance: cannot build the guest device");
            return XV_HR_NOMEM;
        }
        MEM32(g_obj_dev) = g_vt_dev;
        MEM32(g_obj_dev + 4) = 0;   /* tag 0: the device, not a voice */
    }
    if (riid && xv_readable(riid, 16)) {
        const uint8_t *b = (const uint8_t *)ka_host(riid);
        if (memcmp(b, iid_ixaudio2, 16) && memcmp(b, iid_iunknown, 16)) {
            xv_unlock();
            if (out) MEM32(out) = 0;
            xv_log("CoCreateInstance: E_NOINTERFACE for this IID");
            return XV_HR_NOIFACE;
        }
    }
    g_dev.refs++;
    g_cocreates++;
    xv_unlock();
    if (out) MEM32(out) = g_obj_dev;
    if (g_verbose)
        xv_log("CoCreateInstance -> IXAudio2 2.7 (%08X), refs=%u",
               g_obj_dev, g_cocreates);
    return XV_HR_OK;
}

void ka_xaudio2_shutdown(void) {
    int i;
    g_cb_stop = 1;
    xv_lock();
    for (i = 0; i < XV_MAX_VOICES; i++)
        if (g_v[i].magic == XV_MAGIC) xv_kill_voice(&g_v[i]);
    xv_unlock();
}

/* ------------------------------------------------------------------ tests
 * Built with -DKORRA_AUDIO_SELFTEST to exercise the mixer arithmetic on the
 * host (no device, no guest). Not part of the APK. */
#ifdef KORRA_AUDIO_SELFTEST
int main(void) { return 0; }
#endif
