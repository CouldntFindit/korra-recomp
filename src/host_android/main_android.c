/* main_android.c - the Android host entry: from "the shell has a surface and
 * the data is installed" to "the lifted CRT entry is running".
 *
 * Order matters and mirrors the Windows ladder (AGENTS.md section 4): memory
 * window, shims, image map, IAT bind, then the entry. Every step logs, and the
 * guest is only started once the file layer has LoK.exe and GameData visible.
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "host_android.h"
#include "audio_sink.h"

/* The shell's entry points (android/app/src/main/cpp/korra_android.h). Declared
 * here instead of including the shell header so the host tree stays buildable
 * without the Gradle project. */
extern void korra_logv(const char *fmt, ...);

static char g_exe_path[768];
static char g_root[640];
static void *g_window;
static int g_started;
static volatile uint32_t g_shell_frames;

void ka_set_guest_module(const char *exe_path) {
    snprintf(g_exe_path, sizeof g_exe_path, "%s", exe_path);
}
const char *ka_fs_exe_path(void) { return g_exe_path; }

void ka_window_set(void *w) { g_window = w; }
void *ka_window_get(void) { return g_window; }

static void register_stubs(const char *const *names) {
    for (; *names; names++) ka_shim_stub(*names);
}

void ka_shims_register_all(void) {
    ka_shim_register(ka_kernel32_shims);
    ka_shim_register(ka_user32_shims);
    ka_shim_register(ka_gdi32_shims);
    ka_shim_register(ka_misc_shims);
    ka_shim_register(ka_steam_shims);
    ka_shim_register(ka_input_shims);
    ka_shim_register(ka_d3dx_shims);
    /* Kernel32's unfortunate extras that are still imported but harmless. */
    static const char *const stubs[] = {
        "GetConsoleCP", "SetEnvironmentVariableA",
        NULL
    };
    register_stubs(stubs);
}

/* ---- the guest thread ------------------------------------------------- */
static void *guest_thread(void *arg) {
    (void)arg;
    ka_log("guest: entry 0x%08X, dispatch %u functions", ka_pe_entry(),
           (unsigned)recomp_dispatch_count);
    /* Lifted spin-waits must hand the machine to the thread they wait for. */
    recomp_yield_hook = ka_machine_yield;
    ka_log("guest: yield hook %s", recomp_yield_hook ? "installed" : "missing");
    ka_spine_watchdog_start();
    mach_enter();
    ka_spine_note_main();
    hle_call_guest(ka_pe_entry(), 0, NULL);
    mach_leave();
    ka_log("guest: CRT entry returned (exit %u) - the game loop is over", g_eax);
    return NULL;
}

int ka_guest_run(const char *game_root) {
    char exe[768];
    int unresolved;

    snprintf(g_root, sizeof g_root, "%s", game_root);
    ka_fs_set_root(g_root);
    snprintf(exe, sizeof exe, "%s/LoK.exe", g_root);
    ka_set_guest_module(exe);
    ka_log("host: game root %s", g_root);
    ka_trace_init();
    ka_crash_init();

    if (ka_mem_init() != 0) return -1;
    ka_shims_register_all();
    ka_d3d9_init();

    if (ka_pe_map(exe) != 0) return -1;
    unresolved = ka_pe_bind();
    ka_log("host: %d unresolved imports", unresolved);
    /* After the image map: the sampler reads guest globals, and those are not
     * addressable until the window and the .data section exist. */
    ka_memtrace_start();
    ka_frame_probe_start();
    ka_fps_cap_start();
    /* debug.korra.waittest=1: the wait-object scenarios run here, on this
     * host pthread, before the guest thread exists (wait_selftest.c). */
    ka_wait_selftest();

    {
        pthread_t th;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 4u << 20);
        pthread_create(&th, &attr, guest_thread, NULL);
        pthread_attr_destroy(&attr);
    }
    return 0;
}

/* ---- the shell's three entry points (korra_android.h) ------------------ */
int korra_guest_start(const char *game_data_dir, void *native_window) {
    char root[640];
    char marker[768];

    if (g_started) return 0;
    if (!game_data_dir || !*game_data_dir) {
        korra_logv("guest: no GameData path yet");
        return -1;
    }
    /* The install root is the parent of GameData: LoK.exe lives beside it. */
    snprintf(root, sizeof root, "%s", game_data_dir);
    if (strlen(root) > 9 && !strcasecmp(root + strlen(root) - 9, "/GameData"))
        root[strlen(root) - 9] = 0;

    snprintf(marker, sizeof marker, "%s/LoK.exe", root);
    if (access(marker, R_OK) != 0) {
        korra_logv("guest: %s is missing - the installer must copy LoK.exe beside GameData",
                   marker);
        return -1;
    }

    ka_window_set(native_window);
    korra_logv("guest: starting host, root %s", root);
    if (ka_guest_run(root) != 0) {
        korra_logv("guest: host start failed");
        return -1;
    }
    g_started = 1;
    return 0;
}

void korra_guest_frame(void *native_window) {
    if (native_window != g_window) ka_window_set(native_window);
    ka_d3d9_window_changed();
    g_shell_frames++;
}

uint32_t ka_shell_frames(void) { return g_shell_frames; }

void korra_guest_stop(void) {
    /* Clean shutdown order: the sink first (it closes the device and waits for
     * its callback), then the mixer's voices, then graphics. */
    korra_audio_sink_stop();
    ka_xaudio2_shutdown();
    ka_d3d9_shutdown();
}
