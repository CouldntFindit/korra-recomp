/* Frame-pacing hints, resolved at runtime.
 *
 * Two mechanisms, both optional and both present in the reference ports:
 *
 *  1. ADPF (Android Dynamic Performance Framework): a hint session that tells
 *     the platform the frame deadline and how long this frame actually took,
 *     which is what pulls the CPU governor out of its slow state. Available
 *     from API 33 (libandroid.so), so it is dlsym'd rather than linked.
 *  2. ANativeWindow_setFrameRate: API 30, tells the compositor the desired
 *     refresh rate (a 60 Hz game on a 120 Hz panel otherwise runs the panel at
 *     120 and fights the limiter).
 *
 * The deadline is the port's capped rate (debug.korra.fps, default 30, the same
 * property src/host_android/fps_cap_android.c applies to the engine), not a
 * hard-coded 60: hinting 60 while the port runs at 30 lets the panel and the
 * governor chase a frame the port never asked for.
 *
 * The work duration is reported by the *guest's* frame, not by the shell's
 * 16 ms poll: the shell loop's own body is microseconds, and reporting that
 * would tell the platform the app finishes instantly. src/host_android's frame
 * probe calls korra_perf_guest_frame() once per Present with the real frame
 * time.
 */
#include "korra_android.h"

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/system_properties.h>
#include <unistd.h>

typedef struct APerformanceHintManager APerformanceHintManager;
typedef struct APerformanceHintSession APerformanceHintSession;

typedef APerformanceHintManager* (*get_manager_fn)(void);
typedef APerformanceHintSession* (*create_session_fn)(APerformanceHintManager*, const int32_t*,
                                                      size_t, int64_t);
typedef int (*report_actual_fn)(APerformanceHintSession*, int64_t);
typedef int (*set_threads_fn)(APerformanceHintSession*, const int32_t*, size_t);
typedef int32_t (*set_frame_rate_fn)(void* window, float fps, int8_t compatibility);

static get_manager_fn get_manager;
static create_session_fn create_session;
static report_actual_fn report_actual;
static set_threads_fn set_threads;
static set_frame_rate_fn set_frame_rate;
static APerformanceHintSession* session;
static int initialised;
static long long target_ns = 33333333LL;   /* 30 fps: the port's default */
static float hinted_fps = 30.0f;
static int report_failed;

/* The rate the port asks for and the hints follow. Property first, so a run can
 * ask for the engine's 60 (debug.korra.fps=60) or an uncapped probe (0). */
long long korra_perf_target_ns(void) {
    if (!initialised) {
        char v[PROP_VALUE_MAX] = {0};
        long fps = 30;
        __system_property_get("debug.korra.fps", v);
        if (v[0]) fps = strtol(v, NULL, 0);
        if (fps <= 0 || fps > 240) fps = 30;
        target_ns = 1000000000LL / fps;
        hinted_fps = (float)fps;
    }
    return target_ns;
}

void korra_perf_init(void) {
    if (initialised) {
        return;
    }
    initialised = 1;
    korra_perf_target_ns();

    void* lib = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
    if (lib) {
        get_manager = (get_manager_fn)dlsym(lib, "APerformanceHint_getManager");
        create_session = (create_session_fn)dlsym(lib, "APerformanceHint_createSession");
        report_actual = (report_actual_fn)dlsym(lib, "APerformanceHint_reportActualWorkDuration");
        set_threads = (set_threads_fn)dlsym(lib, "APerformanceHint_setThreads");
        set_frame_rate = (set_frame_rate_fn)dlsym(lib, "ANativeWindow_setFrameRate");
    }

    if (get_manager && create_session) {
        APerformanceHintManager* manager = get_manager();
        /* A session needs at least one real thread id; the empty list this
         * used to pass made createSession answer NULL ("session=no" in every
         * log so far). The shell thread is the one this call runs on. */
        int32_t thread = (int32_t)gettid();
        if (manager) {
            session = create_session(manager, &thread, 1, target_ns);
        }
    }
    korra_logv("perf: adpf=%s setFrameRate=%s session=%s target=%.1fms (%.0ffps)",
               get_manager && create_session ? "yes" : "no",
               set_frame_rate ? "yes" : "no",
               session ? "yes" : "no",
               (double)target_ns / 1e6, (double)hinted_fps);
}

void korra_perf_frame(void* native_window, long long work_ns, long long target) {
    (void)work_ns;
    if (native_window && set_frame_rate) {
        set_frame_rate(native_window, hinted_fps, 0 /* ADAPTIVE */);
    }
    (void)target;
}

/* Called once per guest Present (src/host_android/frame_probe_android.c) with
 * the frame's wall time: the only number worth reporting, because it is the one
 * the player sees. */
void korra_perf_guest_frame(long long work_ns) {
    if (!session || !report_actual || work_ns <= 0) return;
    if (report_actual(session, (int64_t)work_ns) < 0 && !report_failed++) {
        korra_logv("perf: reportActualWorkDuration rejected (session targets another thread)");
    }
}

