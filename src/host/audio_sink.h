/*
 * audio_sink.h - the host output stage for the Wwise mix (M4).
 *
 * The engine's own mixer produces the final PCM: the XAudio2 shim only has to
 * collect what the guest submits and hand it to a device. This header is the
 * contract between that mixer and the platform sink, so the Windows (WASAPI)
 * and Android (AAudio) sides stay interchangeable.
 *
 * Threading contract:
 *   - korra_audio_sink_start() may be called from the guest thread. It starts
 *     one output thread, which owns the device and calls the render function
 *     for every period. The render function is never called before start()
 *     returns.
 *   - The render function is called on the output thread only, always for
 *     KORRA_AUDIO_CHANNELS interleaved float samples at KORRA_AUDIO_RATE.
 *     It must not block on the guest and must not call guest code.
 *   - start() is idempotent; stop() may be called from any thread.
 */
#ifndef KORRA_AUDIO_SINK_H
#define KORRA_AUDIO_SINK_H

#ifdef __cplusplus
extern "C" {
#endif

#define KORRA_AUDIO_RATE     48000
#define KORRA_AUDIO_CHANNELS 2

/* Fill `dst` with `frames` interleaved stereo float frames (48 kHz). */
typedef void (*korra_audio_render_fn)(float *dst, int frames);

typedef struct korra_audio_stats {
    unsigned long long frames;             /* frames handed to the device     */
    unsigned long long silent_frames;      /* frames whose peak was < -80 dB  */
    unsigned long long first_sound_frame;  /* 0 = no audible sample yet       */
    unsigned long long underruns;          /* render calls that ran dry       */
    unsigned blocks;                       /* render periods                  */
    double peak;                           /* linear peak, whole run          */
    double rms;                            /* linear RMS, whole run           */
    int backend;                           /* korra_audio_backend values      */
    int device_rate, device_channels, device_bits;
    int live;
} korra_audio_stats;

enum {
    KORRA_AUDIO_BACKEND_NONE   = 0,
    KORRA_AUDIO_BACKEND_WASAPI = 1,
    KORRA_AUDIO_BACKEND_WAVEOUT = 2,
    KORRA_AUDIO_BACKEND_NULL   = 3,  /* real-time clock, no device           */
    KORRA_AUDIO_BACKEND_AAUDIO = 4   /* Android AAudio (src/host/aaudio_sink.c) */
};

/* KORRA_AUDIO_BACKEND=wasapi|waveout|null|none, KORRA_AUDIO_DUMP=<path.wav>,
 * KORRA_AUDIO_LATENCY_MS=<n>, KORRA_AUDIO_LOG=0. */
int  korra_audio_sink_start(korra_audio_render_fn fn);
void korra_audio_sink_stop(void);
int  korra_audio_sink_live(void);
const char *korra_audio_sink_backend(void);
void korra_audio_sink_get_stats(korra_audio_stats *out);
/* Called by the mixer when a running voice has no buffer to hand over. */
void korra_audio_sink_note_underrun(void);

#ifdef __cplusplus
}
#endif

#endif /* KORRA_AUDIO_SINK_H */
