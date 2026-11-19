/* log.txt + logcat, one line at a time.
 *
 * Layout: Android/data/<pkg>/files/log.txt (the app's external files dir,
 * no permission needed on API 29+). The file is truncated per launch, like the
 * reference ports: a crash report is more useful than an unbounded log, and the
 * previous run's file can be pulled before restarting.
 *
 * Everything written to stdout/stderr is funnelled through a pipe into one
 * pump thread, so printf() in host code and third-party libraries also lands in
 * the file and in logcat. korra_logv() writes directly (same format).
 */
#include "korra_android.h"

#include <android/log.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define LOG_TAG "Korra"
#define LINE_MAX_BYTES 1024

static int g_pipe[2] = {-1, -1};
static FILE* g_pipe_in;
static pthread_t g_pump;
static int g_file_fd = -1;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_path[512];

static void emit_line(const char* line, size_t len) {
    __android_log_write(ANDROID_LOG_INFO, LOG_TAG, line);
    pthread_mutex_lock(&g_lock);
    if (g_file_fd >= 0) {
        ssize_t ignored = write(g_file_fd, line, len);
        (void)ignored;
        ignored = write(g_file_fd, "\n", 1);
        (void)ignored;
    }
    pthread_mutex_unlock(&g_lock);
}

static void trim_newline(char* line) {
    size_t n = strlen(line);
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
        line[--n] = 0;
    }
}

static void* pump_thread(void* arg) {
    (void)arg;
    char line[LINE_MAX_BYTES];
    // stderr is unbuffered and stdout line-buffered (set in korra_log_init), so
    // a partial line blocks here until its newline arrives rather than being
    // split across records.
    while (fgets(line, sizeof line, g_pipe_in)) {
        trim_newline(line);
        emit_line(line, strlen(line));
    }
    return NULL;
}

void korra_log_init(const char* files_dir) {
    if (g_path[0]) {
        return; /* already initialised */
    }
    snprintf(g_path, sizeof g_path, "%s/log.txt", files_dir && *files_dir ? files_dir : ".");
    g_file_fd = open(g_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);

    if (pipe(g_pipe) == 0) {
        g_pipe_in = fdopen(g_pipe[0], "r");
        dup2(g_pipe[1], STDOUT_FILENO);
        dup2(g_pipe[1], STDERR_FILENO);
        setvbuf(stdout, NULL, _IOLBF, 0);
        setvbuf(stderr, NULL, _IONBF, 0);
        if (g_pipe_in && pthread_create(&g_pump, NULL, pump_thread, NULL) == 0) {
            pthread_detach(g_pump);
        }
    }
    korra_logv("log: %s", g_path);
}

void korra_log_shutdown(void) {
    korra_logv("log: shutdown");
    pthread_mutex_lock(&g_lock);
    if (g_file_fd >= 0) {
        close(g_file_fd);
        g_file_fd = -1;
    }
    pthread_mutex_unlock(&g_lock);
}

const char* korra_log_path(void) {
    return g_path;
}

void korra_logv(const char* fmt, ...) {
    char line[LINE_MAX_BYTES];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    emit_line(line, strlen(line));
}
