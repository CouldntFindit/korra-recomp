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
 * The scaffold only wires the calls: reporting real work durations belongs to
 * the M3 session once the guest loop exists.
 */
#include "korra_android.h"

#include <dlfcn.h>
#include <stdint.h>

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

void korra_perf_init(void) {
    if (initialised) {
        return;
    }
    initialised = 1;

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
        int32_t thread = 0;
        if (manager) {
            /* 16.666 ms: the engine's own limiter targets 30 or 60 fps (see
             * docs/quirks.md); the hint session starts at 60 and the M3 session
             * can switch it from DAT_00e4d210. */
            session = create_session(manager, &thread, 0, 16666667LL);
        }
    }
    korra_logv("perf: adpf=%s setFrameRate=%s session=%s",
               get_manager && create_session ? "yes" : "no",
               set_frame_rate ? "yes" : "no",
               session ? "yes" : "no");
}

void korra_perf_frame(void* native_window, long long work_ns, long long target_ns) {
    if (native_window && set_frame_rate) {
        set_frame_rate(native_window, target_ns > 0 ? (float)(1e9 / (double)target_ns) : 60.0f,
                       0 /* ADAPTIVE */);
    }
    if (session && report_actual && work_ns > 0) {
        report_actual(session, (int64_t)work_ns);
    }
}
