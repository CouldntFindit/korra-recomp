/* First-run install state, shared with MainActivity.java.
 *
 * Java owns the copy (it is the only side that can read a SAF tree URI). This
 * file owns the hand-off: a path plus a ready flag that the native frame loop
 * polls. Keeping it here means the native thread never blocks on the UI thread.
 */
#include "korra_android.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_gamedata[512];
static int g_ready;

void korra_install_set_gamedata(const char* path) {
    pthread_mutex_lock(&g_lock);
    if (path) {
        strncpy(g_gamedata, path, sizeof g_gamedata - 1);
        g_gamedata[sizeof g_gamedata - 1] = 0;
        g_ready = 1;
    }
    pthread_mutex_unlock(&g_lock);
}

int korra_install_ready(void) {
    int ready;
    pthread_mutex_lock(&g_lock);
    ready = g_ready;
    pthread_mutex_unlock(&g_lock);
    return ready;
}

const char* korra_install_gamedata(void) {
    return g_gamedata;
}

/* adb-driven install: if the tree is already at <files>/game, use it without a
 * picker. This is the loop the dev builds are tested with
 * (scripts\android_push_game.ps1) and it cannot fake a real install: the same
 * three paths are required. */
int korra_install_probe(const char* external_files) {
    char path[640];
    FILE* f;
    if (!external_files || !*external_files) return 0;
    snprintf(path, sizeof path, "%s/game/LoK.exe", external_files);
    f = fopen(path, "rb");
    if (!f) {
        korra_logv("install: probe %s failed (%s)", path, strerror(errno));
        return 0;
    }
    fclose(f);
    snprintf(g_gamedata, sizeof g_gamedata, "%s/game/GameData", external_files);
    g_ready = 1;
    return 1;
}

/* ---------------------------------------------------------------------------
 * JNI entry points. The names follow the Java package and class
 * (org.korra.recomp.MainActivity); they are looked up by name in libkorra.so,
 * which is why nothing registers them explicitly.
 * ------------------------------------------------------------------------ */

#include <jni.h>

static void copy_string(JNIEnv* env, jstring value, char* out, size_t out_size) {
    const char* utf = value ? (*env)->GetStringUTFChars(env, value, NULL) : NULL;
    if (utf) {
        strncpy(out, utf, out_size - 1);
        out[out_size - 1] = 0;
        (*env)->ReleaseStringUTFChars(env, value, utf);
    }
}

JNIEXPORT void JNICALL
Java_org_korra_recomp_MainActivity_nativeGameDataReady(JNIEnv* env, jobject thiz, jstring path) {
    (void)thiz;
    char buffer[512] = {0};
    copy_string(env, path, buffer, sizeof buffer);
    korra_install_set_gamedata(buffer);
    korra_logv("install: GameData ready at %s", buffer);
}

JNIEXPORT void JNICALL
Java_org_korra_recomp_MainActivity_nativeInstallFailed(JNIEnv* env, jobject thiz, jstring reason) {
    (void)thiz;
    char buffer[512] = {0};
    copy_string(env, reason, buffer, sizeof buffer);
    korra_logv("install: failed: %s", buffer);
}
