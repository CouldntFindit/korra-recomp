/*
 * aaudio_sink.c - Android output stage for the Wwise mix (M4, written for M3).
 *
 * This file is the Android twin of wasapi_sink.c: same contract, same pull
 * model, one output callback owned by the device. It is written but NOT built
 * into the Windows host and NOT integrated into the Android target this
 * session - docs/AUDIO_ANDROID.md has the hand-off notes.
 *
 * Why it is a separate file: the Windows build globs src/host (all .c), so this file
 * must be empty on every other platform. On Android the CMake target lists its
 * sources explicitly, which is where M3 adds it (with xaudio2_stub.c).
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
static volatile int g_live, g_started, g_stop;
static int g_out_channels = KORRA_AUDIO_CHANNELS;
static int g_out_rate = KORRA_AUDIO_RATE;
static float *g_scratch;
static int g_scratch_frames;

static korra_audio_stats g_stats;
static double g_rms_sum;
static unsigned long long g_rms_n;
static unsigned long long g_last_log;

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
    if (g_stats.frames - g_last_log >= KORRA_AUDIO_RATE) {
        g_last_log = g_stats.frames;
        alog("%.1f s frames=%llu peak=%.1f dBFS rms=%.1f dBFS quiet=%.1f s",
             (double)g_stats.frames / KORRA_AUDIO_RATE, g_stats.frames, adb(g_stats.peak),
             adb(g_rms_n ? sqrt(g_rms_sum / (double)g_rms_n) : 0.0),
             (double)g_stats.silent_frames / KORRA_AUDIO_RATE);
    }
}

/* AAudio always hands us the stream's real channel count; the mixer produces
 * stereo. Mono/surround devices are rare, but converting here keeps the mixer
 * contract fixed at stereo. */
static void to_device(float *dst, int dst_ch, const float *src, int frames) {
    int i, c;
    if (dst_ch == KORRA_AUDIO_CHANNELS) {
        memcpy(dst, src, (size_t)frames * KORRA_AUDIO_CHANNELS * sizeof(float));
        return;
    }
    for (i = 0; i < frames; i++) {
        float l = src[2 * i], r = src[2 * i + 1];
        for (c = 0; c < dst_ch; c++) {
            if (dst_ch == 1) dst[i] = 0.5f * (l + r);
            else dst[i * dst_ch + c] = (c == 0) ? l : (c == 1) ? r : ((c & 1) ? r : l);
        }
    }
}

static int g_out_format = AAUDIO_FORMAT_PCM_FLOAT;

/* AAudio does not always give what was asked for. Float is what the mixer
 * produces; I16 is the one format a shared stream actually hands back on some
 * devices, so convert here instead of letting the callback write garbage. */
static void to_i16(int16_t *dst, const float *src, int frames, int ch) {
    int i, n = frames * ch;
    for (i = 0; i < n; i++) {
        float v = src[i] * 32767.0f;
        if (v > 32767.0f) v = 32767.0f;
        if (v < -32768.0f) v = -32768.0f;
        dst[i] = (int16_t)v;
    }
}

static aaudio_data_callback_result_t data_cb(AAudioStream *stream, void *user,
                                             void *audio, int32_t frames) {
    int ch = (int)AAudioStream_getChannelCount(stream);
    (void)user;
    if (!g_render) {
        memset(audio, 0, (size_t)frames * ch * 4);
        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }
    if (ch == KORRA_AUDIO_CHANNELS && g_out_format == AAUDIO_FORMAT_PCM_FLOAT) {
        g_render((float *)audio, (int)frames);
        note_block((const float *)audio, (int)frames);
        dump_write((const float *)audio, (int)frames);
    } else {
        if (frames > g_scratch_frames) {
            float *p = (float *)realloc(g_scratch, (size_t)frames * KORRA_AUDIO_CHANNELS * sizeof(float));
            if (!p) {
                memset(audio, 0, (size_t)frames * ch * (g_out_format == AAUDIO_FORMAT_PCM_FLOAT ? 4 : 2));
                return AAUDIO_CALLBACK_RESULT_CONTINUE;
            }
            g_scratch = p;
            g_scratch_frames = (int)frames;
        }
        g_render(g_scratch, (int)frames);
        note_block(g_scratch, (int)frames);
        dump_write(g_scratch, (int)frames);
        if (g_out_format == AAUDIO_FORMAT_PCM_FLOAT) to_device((float *)audio, ch, g_scratch, (int)frames);
        else to_i16((int16_t *)audio, g_scratch, (int)frames, ch);
    }
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

static void error_cb(AAudioStream *stream, void *user, aaudio_result_t error) {
    (void)stream; (void)user;
    alog("stream error %d (%s) - marking the sink dead; the mixer falls back to its clock",
         (int)error, AAudio_convertResultToText(error));
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

int korra_audio_sink_start(korra_audio_render_fn fn) {
    aaudio_result_t r;
    int tries;
    if (__atomic_exchange_n(&g_started, 1, __ATOMIC_SEQ_CST)) return g_live;
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
    AAudioStream_setBufferSizeInFrames(g_stream, 2 * 256);
    alog("opened: %d ch, %d Hz, buffer %d frames (capacity %d), format %d",
         g_out_channels, g_out_rate,
         (int)AAudioStream_getBufferSizeInFrames(g_stream),
         (int)AAudioStream_getBufferCapacityInFrames(g_stream),
         g_out_format);
    if (g_out_format != AAUDIO_FORMAT_PCM_FLOAT)
        alog("WARNING: device format %d is not float; converting I16 in the callback "
             "(the mixer itself stays float)", g_out_format);
    if (g_out_rate != KORRA_AUDIO_RATE)
        alog("WARNING: device rate %d != %d; the mixer is 48 kHz, M3 must add a "
             "resampler or request AAUDIO_SAMPLE_RATE_UNSPECIFIED", g_out_rate, KORRA_AUDIO_RATE);

    r = AAudioStream_requestStart(g_stream);
    if (r != AAUDIO_OK) {
        alog("requestStart failed: %d (%s)", (int)r, AAudio_convertResultToText(r));
        AAudioStream_close(g_stream);
        g_stream = NULL;
        dump_close();
        return 0;
    }
    g_live = 1;
    dump_open();
    alog("sink live (%s)", g_live ? "aaudio" : "none");
    return g_live;
}

void korra_audio_sink_stop(void) {
    if (!g_stream) return;
    g_stop = 1;
    AAudioStream_requestStop(g_stream);
    AAudioStream_close(g_stream);
    g_stream = NULL;
    g_live = 0;
    dump_close();
    free(g_scratch);
    g_scratch = NULL;
    g_scratch_frames = 0;
    alog("stopped: %.3f s, peak %.1f dBFS, rms %.1f dBFS",
         (double)g_stats.frames / KORRA_AUDIO_RATE, adb(g_stats.peak),
         adb(g_rms_n ? sqrt(g_rms_sum / (double)g_rms_n) : 0.0));
}

int korra_audio_sink_live(void) { return g_live; }

const char *korra_audio_sink_backend(void) { return g_live ? "aaudio" : "none"; }

void korra_audio_sink_get_stats(korra_audio_stats *out) {
    if (!out) return;
    *out = g_stats;
    out->backend = g_live ? KORRA_AUDIO_BACKEND_AAUDIO : KORRA_AUDIO_BACKEND_NONE;
    out->device_rate = g_out_rate;
    out->device_channels = g_out_channels;
    out->device_bits = 32;
    out->live = g_live;
    out->rms = g_rms_n ? sqrt(g_rms_sum / (double)g_rms_n) : 0.0;
}

void korra_audio_sink_note_underrun(void) { g_stats.underruns++; }

/* --------------------------------------------------------------- WAV dump
 * The Windows sink writes the same file for acceptance test 1; M3 needs it on
 * the device because "I heard something" is not evidence. 16-bit PCM, 48 kHz,
 * stereo, header patched once a second so `adb pull` after a kill still reads.
 * KORRA_AUDIO_DUMP=/data/local/tmp/korra_audio.wav (or any writable path). */
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
    put32(h + 24, KORRA_AUDIO_RATE); put32(h + 28, KORRA_AUDIO_RATE * KORRA_AUDIO_CHANNELS * 2);
    put16(h + 32, KORRA_AUDIO_CHANNELS * 2); put16(h + 34, 16);
    memcpy(h + 36, "data", 4); put32(h + 40, data);
    if (fseek(g_dump, 0, SEEK_SET) == 0) { fwrite(h, 1, 44, g_dump); fflush(g_dump); }
}
static void dump_open(void) {
    const char *path = getenv("KORRA_AUDIO_DUMP");
    if (!path || !*path) return;
    g_dump = fopen(path, "wb");
    if (!g_dump) { alog("dump: cannot open %s", path); return; }
    dump_patch();
    alog("dump: %s (48 kHz, 16-bit, stereo)", path);
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
    if (g_dump_frames - g_dump_patched >= (unsigned long long)KORRA_AUDIO_RATE) {
        g_dump_patched = g_dump_frames;
        dump_patch();
    }
}
static void dump_close(void) {
    if (!g_dump) return;
    dump_patch();
    fclose(g_dump);
    g_dump = NULL;
    alog("dump closed: %.1f s of audio", (double)g_dump_frames / KORRA_AUDIO_RATE);
}

#endif /* __ANDROID__ */
