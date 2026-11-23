/*
 * aaudio_sink.c - Android output stage for the engine's mix (M4).
 *
 * The Android twin of wasapi_sink.c: same contract (src/host/audio_sink.h),
 * same pull model, one output callback owned by the device. Integrated on
 * 2026-11-23 by the M4-Audio session: the Android XAudio2 shim
 * (src/host_android/xaudio2_android.c) mixes the engine's voices into 48 kHz
 * stereo float and this file hands that to AAudio.
 *
 * What changed against the M3 hand-off version (docs/AUDIO_ANDROID.md):
 *   - the callback reads cached channel/rate/format, never calls into AAudio;
 *   - a device rate other than 48000 is resampled (carried position across
 *     blocks), so the 44.1 kHz refusal is a working path, not a warning;
 *   - the WAV dump is written at the device rate and its header says so, so
 *     wall-clock length and the header agree on any device;
 *   - the dump path comes from `debug.korra.audio.dump` (1 = the app's own
 *     external files dir, handed in as KORRA_AUDIO_DIR) or KORRA_AUDIO_DUMP;
 *   - `debug.korra.audio.backend=null` starts no device at all (the XAudio2
 *     shim then retires buffers on its clock, so CRI still feeds).
 *
 * Still true from the hand-off: no guest code, no allocation and no per-block
 * logging in the callback; buffer 2x256 frames; exclusive first, shared on
 * refusal; clean stop closes the stream (AAudio waits for the callback).
 */
#ifdef __ANDROID__

#include <aaudio/AAudio.h>
#include <android/log.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/system_properties.h>

#include "audio_sink.h"

#define TAG "korra-audio"

static void alog(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    __android_log_vprint(ANDROID_LOG_INFO, TAG, fmt, ap);
    va_end(ap);
}

static korra_audio_render_fn g_render;
static AAudioStream *g_stream;
static volatile int g_live, g_started;
static int g_out_channels = KORRA_AUDIO_CHANNELS;
static int g_out_rate = KORRA_AUDIO_RATE;
static int g_out_format = AAUDIO_FORMAT_PCM_FLOAT;

/* Device-rate mix, then format/channel conversion into AAudio's buffer. */
static float *g_mix;
static int g_mix_cap;

/* 48 kHz source scratch for the resampler, and its carried state. */
static float *g_in;
static int g_in_cap;
static double g_src_pos;          /* global 48k position of the next out frame */
static unsigned long long g_src_index;   /* 48k frames produced so far */
#define RS_H 4
static float g_hist_l[RS_H], g_hist_r[RS_H];

static korra_audio_stats g_stats;
static double g_rms_sum;
static unsigned long long g_rms_n;
static unsigned long long g_last_log;
static int g_last_xrun;

/* Defined at the bottom; the callback and start/stop use them. */
static void dump_open(void);
static void dump_write(const float *p, int frames);
static void dump_close(void);

static double adb(double lin) {
    return lin <= 1e-9 ? -180.0 : 20.0 * log10(lin);
}

static void note_block(const float *p, int frames) {
    double peak = 0.0, sum = 0.0;
    int i, n = frames * KORRA_AUDIO_CHANNELS;
    for (i = 0; i < n; i++) {
        double a = fabs((double)p[i]);
        if (a > peak) peak = a;
        sum += a * a;
    }
    if (peak > g_stats.peak) g_stats.peak = peak;
    g_rms_sum += sum;
    g_rms_n += (unsigned long long)n;
    g_stats.frames += (unsigned long long)frames;
    g_stats.blocks++;
    if (peak < 3.16e-4) g_stats.silent_frames += (unsigned long long)frames;
    else if (!g_stats.first_sound_frame)
        g_stats.first_sound_frame = g_stats.frames;
    if (g_stats.frames - g_last_log >= (unsigned long long)g_out_rate) {
        int xrun = (int)AAudioStream_getXRunCount(g_stream);
        g_last_log = g_stats.frames;
        alog("%.1f s frames=%llu peak=%.1f dBFS rms=%.1f dBFS quiet=%.1f s "
             "underruns=%llu xruns=%d (%d new)",
             (double)g_stats.frames / (double)g_out_rate, g_stats.frames,
             adb(g_stats.peak),
             adb(g_rms_n ? sqrt(g_rms_sum / (double)g_rms_n) : 0.0),
             (double)g_stats.silent_frames / (double)g_out_rate,
             g_stats.underruns, xrun, xrun - g_last_xrun);
        g_last_xrun = xrun;
    }
}

/* AAudio always hands us the stream's real channel count; the mixer produces
 * stereo. Mono/surround devices are rare; converting here keeps the mixer
 * contract fixed at stereo. */
static void to_device(float *dst, int dst_ch, const float *src, int frames) {
    int i, c;
    if (dst_ch == KORRA_AUDIO_CHANNELS) {
        memcpy(dst, src, (size_t)frames * KORRA_AUDIO_CHANNELS * sizeof(float));
        return;
    }
    for (i = 0; i < frames; i++) {
        float l = src[2 * i], r = src[2 * i + 1];
        if (dst_ch == 1) { dst[i] = 0.5f * (l + r); continue; }
        for (c = 0; c < dst_ch; c++)
            dst[i * dst_ch + c] = (c == 0) ? l : (c == 1) ? r : ((c & 1) ? r : l);
    }
}

static void to_i16(int16_t *dst, const float *src, int frames, int ch) {
    int i, n = frames * ch;
    for (i = 0; i < n; i++) {
        float v = src[i] * 32767.0f;
        if (v > 32767.0f) v = 32767.0f;
        if (v < -32768.0f) v = -32768.0f;
        dst[i] = (int16_t)v;
    }
}

/* Resample the 48 kHz mix to the device rate, carrying the fractional position
 * and the last RS_H source frames across blocks. Linear interpolation: the
 * mixer already band-limits nothing (it is a game mix), and a 44.1 kHz device
 * is a fallback, not the target. */
static void resample_block(int n) {
    double step = (double)KORRA_AUDIO_RATE / (double)g_out_rate;
    int need = (int)((double)n * step) + RS_H + 8;
    int nnew, i, k;
    long long base;

    if (need > g_in_cap) need = g_in_cap;
    nnew = need - RS_H;
    if (nnew < 1) nnew = 1;

    /* Prefix: the last RS_H frames of everything produced so far. */
    for (k = 0; k < RS_H; k++) {
        g_in[k * 2 + 0] = g_hist_l[k];
        g_in[k * 2 + 1] = g_hist_r[k];
    }
    g_render(&g_in[RS_H * 2], nnew);

    base = (long long)g_src_index - RS_H;
    for (i = 0; i < n; i++) {
        double p = g_src_pos - (double)base;
        int i0 = (int)floor(p);
        double fr;
        if (i0 < 0) i0 = 0;
        if (i0 > need - 2) i0 = need - 2;
        fr = p - (double)i0;
        if (fr < 0.0) fr = 0.0;
        if (fr > 1.0) fr = 1.0;
        g_mix[i * 2 + 0] = (float)(g_in[i0 * 2 + 0] * (1.0 - fr) + g_in[(i0 + 1) * 2 + 0] * fr);
        g_mix[i * 2 + 1] = (float)(g_in[i0 * 2 + 1] * (1.0 - fr) + g_in[(i0 + 1) * 2 + 1] * fr);
        g_src_pos += step;
    }

    /* History = the last RS_H produced frames (old history first, in case this
     * block produced fewer than RS_H new frames). */
    if (nnew >= RS_H) {
        for (k = 0; k < RS_H; k++) {
            int j = RS_H + nnew - RS_H + k;
            g_hist_l[k] = g_in[j * 2 + 0];
            g_hist_r[k] = g_in[j * 2 + 1];
        }
    } else {
        float cl[RS_H], cr[RS_H];
        for (k = 0; k < RS_H; k++) { cl[k] = g_hist_l[k]; cr[k] = g_hist_r[k]; }
        for (i = 0; i < nnew; i++) {
            for (k = 0; k + 1 < RS_H; k++) { cl[k] = cl[k + 1]; cr[k] = cr[k + 1]; }
            cl[RS_H - 1] = g_in[(RS_H + i) * 2 + 0];
            cr[RS_H - 1] = g_in[(RS_H + i) * 2 + 1];
        }
        for (k = 0; k < RS_H; k++) { g_hist_l[k] = cl[k]; g_hist_r[k] = cr[k]; }
    }
    g_src_index += (unsigned long long)nnew;
}

static aaudio_data_callback_result_t data_cb(AAudioStream *stream, void *user,
                                             void *audio, int32_t frames) {
    int ch = g_out_channels;
    (void)stream;
    (void)user;
    if (!g_render || frames <= 0 || frames > g_mix_cap) {
        memset(audio, 0, (size_t)(frames > 0 ? frames : 0) * (size_t)ch *
                            (g_out_format == AAUDIO_FORMAT_PCM_FLOAT ? 4u : 2u));
        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }
    if (g_out_rate == KORRA_AUDIO_RATE) {
        g_render(g_mix, (int)frames);
    } else {
        resample_block((int)frames);
    }
    note_block(g_mix, (int)frames);
    dump_write(g_mix, (int)frames);
    if (g_out_format == AAUDIO_FORMAT_PCM_FLOAT) to_device((float *)audio, ch, g_mix, (int)frames);
    else to_i16((int16_t *)audio, g_mix, (int)frames, ch);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

static void error_cb(AAudioStream *stream, void *user, aaudio_result_t error) {
    (void)stream; (void)user;
    alog("stream error %d (%s) - the sink is dead; the XAudio2 shim falls back "
         "to its clock and CRI keeps feeding", (int)error, AAudio_convertResultToText(error));
    g_live = 0;
}

static aaudio_result_t open_stream(AAudioStream **out, int exclusive) {
    AAudioStreamBuilder *b = NULL;
    aaudio_result_t r;
    r = AAudio_createStreamBuilder(&b);
    if (r != AAUDIO_OK) return r;
    AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setSharingMode(b, exclusive ? AAUDIO_SHARING_MODE_EXCLUSIVE
                                                    : AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_FLOAT);
    AAudioStreamBuilder_setSampleRate(b, KORRA_AUDIO_RATE);
    AAudioStreamBuilder_setChannelCount(b, KORRA_AUDIO_CHANNELS);
    /* 2 x 256 frames: the low-latency figure from docs/audio.md 9.3. */
    AAudioStreamBuilder_setBufferCapacityInFrames(b, 2 * 256);
    AAudioStreamBuilder_setFramesPerDataCallback(b, 0);   /* device default */
    AAudioStreamBuilder_setDataCallback(b, data_cb, NULL);
    AAudioStreamBuilder_setErrorCallback(b, error_cb, NULL);
    r = AAudioStreamBuilder_openStream(b, out);
    AAudioStreamBuilder_delete(b);
    return r;
}

static const char *prop(const char *name, char *buf) {
    buf[0] = 0;
    if (__system_property_get(name, buf) <= 0) buf[0] = 0;
    return buf;
}

static int prop_int(const char *name, int def) {
    char v[PROP_VALUE_MAX];
    if (__system_property_get(name, v) <= 0 || !v[0]) return def;
    return atoi(v);
}

int korra_audio_sink_start(korra_audio_render_fn fn) {
    aaudio_result_t r;
    int tries;
    char v[PROP_VALUE_MAX];

    if (__atomic_exchange_n(&g_started, 1, __ATOMIC_SEQ_CST)) return g_live;

    if (!strcmp(prop("debug.korra.audio.backend", v), "null")) {
        alog("backend null (debug.korra.audio.backend): the shim clock drives retirement");
        return 0;
    }

    g_render = fn;

    /* Exclusive first, then shared: some devices refuse exclusive while a call
     * or a notification is active, and that refusal is normal, not an error. */
    for (tries = 0; tries < 2; tries++) {
        r = open_stream(&g_stream, tries == 0);
        if (r == AAUDIO_OK) break;
        alog("%s open failed: %d (%s)", tries == 0 ? "exclusive" : "shared",
             (int)r, AAudio_convertResultToText(r));
        g_stream = NULL;
    }
    if (!g_stream) return 0;

    g_out_channels = (int)AAudioStream_getChannelCount(g_stream);
    g_out_rate = (int)AAudioStream_getSampleRate(g_stream);
    g_out_format = (int)AAudioStream_getFormat(g_stream);
    /* 2x256 frames is the hand-off's starting point; docs/audio.md 9.3 says
     * raise it if the log shows underruns. It is a property so the choice can
     * be measured on the device without a rebuild. */
    {
        int want = prop_int("debug.korra.audio.buffer", 2 * 256);
        int cap = (int)AAudioStream_getBufferCapacityInFrames(g_stream);
        if (want < 64) want = 64;
        if (want > cap) want = cap;
        AAudioStream_setBufferSizeInFrames(g_stream, want);
    }
    alog("opened: %d ch, %d Hz, buffer %d frames (%.1f ms, capacity %d), "
         "burst %d, format %d",
         g_out_channels, g_out_rate,
         (int)AAudioStream_getBufferSizeInFrames(g_stream),
         1000.0 * (double)AAudioStream_getBufferSizeInFrames(g_stream) /
             (double)(g_out_rate ? g_out_rate : KORRA_AUDIO_RATE),
         (int)AAudioStream_getBufferCapacityInFrames(g_stream),
         (int)AAudioStream_getFramesPerBurst(g_stream),
         g_out_format);
    if (g_out_format != AAUDIO_FORMAT_PCM_FLOAT)
        alog("WARNING: device format %d is not float; converting I16 in the "
             "callback (the mixer itself stays float)", g_out_format);
    if (g_out_rate != KORRA_AUDIO_RATE)
        alog("device rate %d != %d: resampling the 48 kHz mix (carried position)",
             g_out_rate, KORRA_AUDIO_RATE);

    /* Everything the callback touches is allocated here, before the stream
     * starts: no malloc, no AAudio call and no log per block. */
    g_mix_cap = (int)AAudioStream_getBufferCapacityInFrames(g_stream) + 64;
    g_mix = (float *)calloc((size_t)g_mix_cap * KORRA_AUDIO_CHANNELS, sizeof(float));
    if (g_out_rate != KORRA_AUDIO_RATE) {
        double step = (double)KORRA_AUDIO_RATE / (double)g_out_rate;
        g_in_cap = (int)((double)g_mix_cap * step) + RS_H + 16;
        g_in = (float *)calloc((size_t)g_in_cap * KORRA_AUDIO_CHANNELS, sizeof(float));
        g_src_pos = 0.0;
        g_src_index = 0;
        memset(g_hist_l, 0, sizeof g_hist_l);
        memset(g_hist_r, 0, sizeof g_hist_r);
    }
    if (!g_mix || (g_out_rate != KORRA_AUDIO_RATE && !g_in)) {
        alog("cannot allocate the mix buffers (%d frames)", g_mix_cap);
        AAudioStream_close(g_stream);
        g_stream = NULL;
        free(g_mix); g_mix = NULL;
        free(g_in); g_in = NULL;
        return 0;
    }

    dump_open();                      /* before the first callback can run */
    r = AAudioStream_requestStart(g_stream);
    if (r != AAUDIO_OK) {
        alog("requestStart failed: %d (%s)", (int)r, AAudio_convertResultToText(r));
        AAudioStream_close(g_stream);
        g_stream = NULL;
        return 0;
    }
    g_live = 1;
    alog("sink live (aaudio %d ch %d Hz)", g_out_channels, g_out_rate);
    return g_live;
}

void korra_audio_sink_stop(void) {
    if (!g_stream) return;
    AAudioStream_requestStop(g_stream);
    AAudioStream_close(g_stream);     /* waits for a running callback to finish */
    g_stream = NULL;
    g_live = 0;
    dump_close();
    free(g_mix);
    g_mix = NULL;
    g_mix_cap = 0;
    free(g_in);
    g_in = NULL;
    g_in_cap = 0;
    alog("stopped: %.3f s, peak %.1f dBFS, rms %.1f dBFS, underruns %llu, xruns %d",
         (double)g_stats.frames / (double)(g_out_rate ? g_out_rate : KORRA_AUDIO_RATE),
         adb(g_stats.peak),
         adb(g_rms_n ? sqrt(g_rms_sum / (double)g_rms_n) : 0.0),
         g_stats.underruns, g_last_xrun);
}

int korra_audio_sink_live(void) { return g_live; }

const char *korra_audio_sink_backend(void) { return g_live ? "aaudio" : "none"; }

void korra_audio_sink_get_stats(korra_audio_stats *out) {
    if (!out) return;
    *out = g_stats;
    out->backend = g_live ? KORRA_AUDIO_BACKEND_AAUDIO : KORRA_AUDIO_BACKEND_NONE;
    out->device_rate = g_out_rate;
    out->device_channels = g_out_channels;
    out->device_bits = (g_out_format == AAUDIO_FORMAT_PCM_FLOAT) ? 32 : 16;
    out->live = g_live;
    out->rms = g_rms_n ? sqrt(g_rms_sum / (double)g_rms_n) : 0.0;
}

void korra_audio_sink_note_underrun(void) { g_stats.underruns++; }

/* --------------------------------------------------------------- WAV dump
 * The Windows sink writes the same artifact for acceptance test 1, and it is
 * the only evidence that survives `adb pull` after a crash. 16-bit PCM,
 * stereo, at the device rate (48 kHz on the target, so the Windows numbers
 * compare directly); the header is patched once a second, so a killed run
 * still yields a readable file.
 *
 * debug.korra.audio.dump=1            -> KORRA_AUDIO_DIR/korra_audio.wav
 * debug.korra.audio.dump=/some/path   -> that path
 * KORRA_AUDIO_DUMP=/some/path         -> that path (same variable as Windows)
 */
static FILE *g_dump;
static unsigned long long g_dump_frames;
static unsigned long long g_dump_patched;

static void put32(unsigned char *p, unsigned long v) {
    p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}
static void put16(unsigned char *p, unsigned v) {
    p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8);
}
static void dump_patch(void) {
    unsigned char h[44];
    unsigned long data = (unsigned long)(g_dump_frames * KORRA_AUDIO_CHANNELS * 2);
    unsigned long riff = data + 36;
    if (!g_dump) return;
    memcpy(h, "RIFF", 4); put32(h + 4, riff); memcpy(h + 8, "WAVEfmt ", 8);
    put32(h + 16, 16); put16(h + 20, 1); put16(h + 22, KORRA_AUDIO_CHANNELS);
    put32(h + 24, (unsigned long)g_out_rate);
    put32(h + 28, (unsigned long)g_out_rate * KORRA_AUDIO_CHANNELS * 2);
    put16(h + 32, KORRA_AUDIO_CHANNELS * 2); put16(h + 34, 16);
    memcpy(h + 36, "data", 4); put32(h + 40, data);
    if (fseek(g_dump, 0, SEEK_SET) == 0) {
        fwrite(h, 1, 44, g_dump);
        fflush(g_dump);
        /* Back to the end: a patch that leaves the stream at offset 44 makes
         * every following block overwrite the file's start, so a long run kept
         * only its last second (measured on the first device run). */
        fseek(g_dump, 0, SEEK_END);
    }
}
static void dump_open(void) {
    char v[PROP_VALUE_MAX];
    char path[512];
    const char *p = NULL;

    if (__system_property_get("debug.korra.audio.dump", v) > 0 && v[0]) {
        if (!strcmp(v, "1")) {
            const char *dir = getenv("KORRA_AUDIO_DIR");
            if (dir && *dir) {
                snprintf(path, sizeof path, "%s/korra_audio.wav", dir);
                p = path;
            }
        } else {
            p = v;
        }
    }
    if (!p) p = getenv("KORRA_AUDIO_DUMP");
    if (!p || !*p) return;
    g_dump = fopen(p, "wb");
    if (!g_dump) { alog("dump: cannot open %s", p); return; }
    dump_patch();
    alog("dump: %s (%d Hz, 16-bit, stereo)", p, g_out_rate);
}
static void dump_write(const float *p, int frames) {
    short buf[512 * KORRA_AUDIO_CHANNELS];
    int done = 0;
    if (!g_dump) return;
    while (done < frames) {
        int n = frames - done, i;
        if (n > 512) n = 512;
        for (i = 0; i < n * KORRA_AUDIO_CHANNELS; i++) {
            float v = p[(size_t)done * KORRA_AUDIO_CHANNELS + i] * 32767.0f;
            if (v > 32767.0f) v = 32767.0f;
            if (v < -32768.0f) v = -32768.0f;
            buf[i] = (short)v;
        }
        fwrite(buf, sizeof(short), (size_t)n * KORRA_AUDIO_CHANNELS, g_dump);
        done += n;
    }
    g_dump_frames += (unsigned long long)frames;
    if (g_dump_frames - g_dump_patched >= (unsigned long long)g_out_rate) {
        g_dump_patched = g_dump_frames;
        dump_patch();
    }
}
static void dump_close(void) {
    if (!g_dump) return;
    dump_patch();
    fclose(g_dump);
    g_dump = NULL;
    alog("dump closed: %.1f s of audio at %d Hz",
         (double)g_dump_frames / (double)(g_out_rate ? g_out_rate : KORRA_AUDIO_RATE),
         g_out_rate);
}

#endif /* __ANDROID__ */
