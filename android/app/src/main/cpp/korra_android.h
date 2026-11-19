/* Shared declarations for the Android shell (M3 scaffold).
 *
 * The scaffold deliberately links nothing from the recompilation yet: it proves
 * the shell, the log file and the first-run install flow compile and run, so
 * the post-gate session only has to flip -DKORRA_LINK_GUEST=ON and add
 * src/host_android/. See docs/ANDROID_BRINGUP.md.
 */
#ifndef KORRA_ANDROID_H
#define KORRA_ANDROID_H

#ifdef __cplusplus
extern "C" {
#endif

/* ---- log_android.c -------------------------------------------------------
 * log.txt lives in the app's external files directory
 * (Android/data/<pkg>/files/log.txt) and every line also goes to logcat under
 * the tag "Korra". stdout/stderr are redirected through the same pump. */
void korra_log_init(const char* files_dir);
void korra_log_shutdown(void);
const char* korra_log_path(void);
void korra_logv(const char* fmt, ...);

/* ---- install_android.c ---------------------------------------------------
 * First-run state shared with the Java side. Java copies the user's GameData
 * tree into external storage and then calls nativeGameDataReady(); the native
 * frame loop polls korra_install_ready(). */
void korra_install_set_gamedata(const char* path);
int korra_install_ready(void);
const char* korra_install_gamedata(void);

/* ---- perf_android.c ------------------------------------------------------
 * Optional frame-pacing hints, resolved with dlsym so minSdk 29 builds run on
 * devices whose libandroid.so predates ADPF (API 33) or
 * ANativeWindow_setFrameRate (API 30). */
void korra_perf_init(void);
void korra_perf_frame(void* native_window, long long work_ns, long long target_ns);

/* ---- jni_bridge.cpp ------------------------------------------------------
 * The placeholder the real guest entry replaces after the M2 gate. */
int korra_guest_start(const char* game_data_dir, void* native_window);
void korra_guest_frame(void* native_window);
void korra_guest_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* KORRA_ANDROID_H */
