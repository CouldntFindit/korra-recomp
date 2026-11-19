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
#include <android/native_window.h>
#include <time.h>

#include <game-activity/native_app_glue/android_native_app_glue.h>

#include "korra_android.h"

namespace {

constexpr long long kTargetFrameNs = 16666667LL; /* 60 Hz */

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
            }
            break;
        case APP_CMD_TERM_WINDOW:
            korra_logv("window: destroyed");
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
        korra_perf_frame(state.window, frame_end - frame_start, kTargetFrameNs);

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
