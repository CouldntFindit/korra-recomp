/* GameActivity native entry point: android_main().
 *
 * GameActivity's glue starts this on its own thread once the Activity exists
 * and calls app->onAppCmd for lifecycle/window events. The loop below is the
 * M3 scaffold's whole runtime: window bookkeeping, input capture (counted, then
 * dropped - the touch HUD maps it in the M3 session), a 16 ms tick, and the
 * hand-off to korra_guest_start() once the GameData tree is in place.
 *
 * Nothing from the recompilation is linked yet. The stubs at the bottom log
 * clearly and render nothing, so "the shell runs" and "the game runs" cannot be
 * confused in log.txt.
 */
#include <android/api-level.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <stdio.h>
#include <android/native_window.h>
#include <time.h>

#include <game-activity/native_app_glue/android_native_app_glue.h>

#include "korra_android.h"

/* These live in src/host_android/ (C linkage) and are declared at file scope:
 * a declaration inside the anonymous namespace below would give it internal
 * linkage and the link would fail with "undefined symbol ::name". */
#if KORRA_WITH_DXVK
extern "C" void dxvkAndroidSetWindow(ANativeWindow * window);
#elif KORRA_LINK_GUEST
extern "C" void ka_window_set(void *window);
#endif
#if KORRA_LINK_GUEST
extern "C" void ka_input_touch(int action, int id, float x, float y, int w, int h);
#endif

namespace {

/* The frame deadline comes from korra_perf_target_ns() (debug.korra.fps,
 * default 30 fps), so the shell and the engine's limiter ask for one rate. */

struct AppState {
    android_app* app = nullptr;
    ANativeWindow* window = nullptr;
    unsigned long long frames = 0;
    unsigned long long touch_events = 0;
    unsigned long long key_events = 0;
    long long last_report_ns = 0;
    bool guest_started = false;
};

long long now_ns() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

void handle_cmd(android_app* app, int32_t cmd) {
    auto* state = static_cast<AppState*>(app->userData);
    if (!state) {
        return;
    }
    switch (cmd) {
        case APP_CMD_INIT_WINDOW:
            state->window = app->window;
            if (state->window) {
                korra_logv("window: created %dx%d format=%d",
                           ANativeWindow_getWidth(state->window),
                           ANativeWindow_getHeight(state->window),
                           ANativeWindow_getFormat(state->window));
#if KORRA_WITH_DXVK
                /* DXVK's WSI binds its VkSurface to this window. */
                dxvkAndroidSetWindow(state->window);
#elif KORRA_LINK_GUEST
                ka_window_set(state->window);
#endif
            }
            break;
        case APP_CMD_TERM_WINDOW:
            korra_logv("window: destroyed");
#if KORRA_WITH_DXVK
            dxvkAndroidSetWindow(nullptr);
#endif
            state->window = nullptr;
            break;
        case APP_CMD_WINDOW_RESIZED:
            if (state->window) {
                korra_logv("window: resized %dx%d",
                           ANativeWindow_getWidth(state->window),
                           ANativeWindow_getHeight(state->window));
            }
            break;
        case APP_CMD_GAINED_FOCUS:
            korra_logv("lifecycle: focus gained");
            break;
        case APP_CMD_LOST_FOCUS:
            korra_logv("lifecycle: focus lost");
            break;
        case APP_CMD_PAUSE:
            korra_logv("lifecycle: pause");
            break;
        case APP_CMD_RESUME:
            korra_logv("lifecycle: resume");
            break;
        case APP_CMD_LOW_MEMORY:
            korra_logv("lifecycle: low memory");
            break;
        case APP_CMD_DESTROY:
            korra_logv("lifecycle: destroy");
            break;
        default:
            break;
    }
}

} // namespace

extern "C" void android_main(android_app* app) {
    AppState state;
    state.app = app;
    app->userData = &state;
    app->onAppCmd = handle_cmd;

    /* GameActivity fills these before the thread starts; they are the same
     * paths the Java side sees, so the Java -> JNI hand-off only carries the
     * install result. */
    const char* external = app->activity ? app->activity->externalDataPath : nullptr;
    const char* internal = app->activity ? app->activity->internalDataPath : nullptr;
    korra_log_init(external);
    korra_logv("shell: android_main started (API %d, abi %s)",
               android_get_device_api_level(),
#if defined(__aarch64__)
               "arm64"
#else
               "non-arm64"
#endif
    );
    korra_logv("shell: external files %s", external ? external : "(null)");
    /* Internal first: on Android 16+ only the app may write into its own
     * external files dir (adb-pushed files there are EACCES), so a dev install
     * lives in internal storage and is copied in with `run-as`. The SAF flow
     * remains the user path. */
    if (internal && korra_install_probe(internal)) {
        korra_logv("install: probed %s/game/LoK.exe - ready (internal)", internal);
    } else if (external && korra_install_probe(external)) {
        korra_logv("install: probed %s/game/LoK.exe - ready (external)", external);
    }
    /* DXVK's state cache path comes from DXVK_STATE_CACHE_PATH; without it the
     * variable is empty and dxvk-native's create_directories("") throws
     * (libc++abi: filesystem_error) at device creation. Point it at a directory
     * this process owns. HOME is unset on Android, so there is no default. */
    if (internal) {
        char cache[512];
        snprintf(cache, sizeof cache, "%s/dxvk-cache", internal);
        mkdir(cache, 0755);
        setenv("DXVK_STATE_CACHE_PATH", cache, 1);
        korra_logv("dxvk: state cache %s", cache);
    }
    korra_perf_init();
    state.last_report_ns = now_ns();

    while (!app->destroyRequested) {
        int events = 0;
        android_poll_source* source = nullptr;
        int timeout_ms = 16;
        while (ALooper_pollOnce(timeout_ms, nullptr, &events,
                                reinterpret_cast<void**>(&source)) >= 0) {
            if (source) {
                source->process(app, source);
            }
            if (app->destroyRequested) {
                break;
            }
            timeout_ms = 0; /* drain, then tick */
        }
        if (app->destroyRequested) {
            break;
        }

        if (android_input_buffer* input = android_app_swap_input_buffers(app)) {
            state.touch_events += input->motionEventsCount;
            state.key_events += input->keyEventsCount;
#if KORRA_LINK_GUEST
            /* Touch becomes the virtual pad at the import boundary
             * (input_android.c); the engine's own XInput remap and edge
             * detection then work unchanged. */
            if (state.window) {
                const int w = ANativeWindow_getWidth(state.window);
                const int h = ANativeWindow_getHeight(state.window);
                for (uint32_t i = 0; i < input->motionEventsCount; i++) {
                    const GameActivityMotionEvent& ev = input->motionEvents[i];
                    const int32_t masked = ev.action & AMOTION_EVENT_ACTION_MASK;
                    const int32_t idx =
                        (ev.action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >>
                        AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;
                    int act;
                    if (masked == AMOTION_EVENT_ACTION_DOWN ||
                        masked == AMOTION_EVENT_ACTION_POINTER_DOWN) act = 0;
                    else if (masked == AMOTION_EVENT_ACTION_MOVE) act = 1;
                    else if (masked == AMOTION_EVENT_ACTION_UP ||
                             masked == AMOTION_EVENT_ACTION_POINTER_UP ||
                             masked == AMOTION_EVENT_ACTION_CANCEL) act = 2;
                    else act = 1;
                    for (uint32_t p = 0; p < ev.pointerCount; p++) {
                        const GameActivityPointerAxes& pt = ev.pointers[p];
                        int pointer_id = (int)pt.id;
                        if (masked != AMOTION_EVENT_ACTION_MOVE && (int32_t)p != idx)
                            continue;
                        ka_input_touch(act, pointer_id, pt.axisValues[AMOTION_EVENT_AXIS_X],
                                       pt.axisValues[AMOTION_EVENT_AXIS_Y], w, h);
                    }
                }
            }
#endif
            if (input->motionEventsCount) {
                android_app_clear_motion_events(input);
            }
            if (input->keyEventsCount) {
                android_app_clear_key_events(input);
            }
        }

        if (!state.window) {
            continue;
        }

        const long long frame_start = now_ns();
        if (!state.guest_started && korra_install_ready()) {
            state.guest_started =
                korra_guest_start(korra_install_gamedata(), state.window) == 0;
        }
        if (state.guest_started) {
            korra_guest_frame(state.window);
        }
        const long long frame_end = now_ns();
        korra_perf_frame(state.window, frame_end - frame_start, korra_perf_target_ns());

        state.frames++;
        if (frame_end - state.last_report_ns >= 1000000000LL) {
            korra_logv("shell: %llu frames, %llu touch events, %llu key events, guest=%s",
                       state.frames, state.touch_events, state.key_events,
                       state.guest_started ? "stub" : "not linked");
            state.last_report_ns = frame_end;
        }
    }

    korra_guest_stop();
    korra_logv("shell: android_main exiting");
    korra_log_shutdown();
}

/* ---------------------------------------------------------------------------
 * Placeholder guest. The M3 session replaces these three functions with the
 * real entry points (src/host_android/ + src/recomp/gen/, see
 * docs/ANDROID_BRINGUP.md section 4) and flips -DKORRA_LINK_GUEST=ON.
 * ------------------------------------------------------------------------ */

#if !KORRA_LINK_GUEST

int korra_guest_start(const char* game_data_dir, void* native_window) {
    korra_logv("guest: STUB - nothing linked. GameData=%s window=%p",
               game_data_dir ? game_data_dir : "(none)", native_window);
    korra_logv("guest: this build cannot render; see docs/ANDROID_BRINGUP.md");
    return 0;
}

void korra_guest_frame(void* native_window) {
    (void)native_window;
}

void korra_guest_stop(void) {
}

#endif /* !KORRA_LINK_GUEST */

