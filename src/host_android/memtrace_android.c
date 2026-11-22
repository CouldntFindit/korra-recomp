/* memtrace_android.c - the 1 Hz stability sampler (debug.korra.memtrace=1).
 *
 * A death has to be read against a trajectory, not a single exit record: the
 * post-cutscene SIGABRT arrived with rss=1.2GB and no guest [crash] line, and
 * the pulled logcat ended minutes before the event. This thread writes one
 * line a second - RSS/HWM straight from /proc/self/status, the guest arena's
 * bump and free-list totals, the guest thread/stack census and the D3D9
 * present count - so a hard death still leaves the last few samples in log.txt
 * and logcat.
 *
 * One line, one write: the same rule the [crash] handler follows, because the
 * pump thread and the guest threads interleave multi-part lines.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/system_properties.h>
#include <time.h>
#include <unistd.h>

#include "host_android.h"

static int64_t g_t0_ns;
static volatile int g_started;

static int64_t mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/* VmRSS / VmHWM in kB from /proc/self/status. Returns 0 when the file cannot
 * be read; the line then says rss=0 instead of lying with a stale value. */
static int read_status_kb(uint64_t *rss, uint64_t *hwm) {
    char buf[4096];
    int fd = open("/proc/self/status", O_RDONLY | O_CLOEXEC);
    ssize_t n;
    *rss = *hwm = 0;
    if (fd < 0) return 0;
    n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = 0;
    {
        const char *p = strstr(buf, "VmRSS:");
        if (p) *rss = strtoull(p + 6, NULL, 10);
        p = strstr(buf, "VmHWM:");
        if (p) *hwm = strtoull(p + 6, NULL, 10);
    }
    return 1;
}

/* Guest scene state, the same four words the Windows oracle's scene probe
 * prints (src/host/scene_probe.c: flow 0xE38A54, inst 0xDBC02C, step 0xE38AB0,
 * mode 0xCA3AC0), so a device line and a Windows line diff directly. */
static uint32_t guest_u32(uint32_t va) { return g_mem_base ? MEM32(va) : 0; }

static void sample(void) {
    uint64_t rss = 0, hwm = 0;
    uint32_t heap_hi = 0, free_blocks = 0, free_bytes = 0;
    uint32_t stack_hi = 0, stack_end = 0, stack_free_b = 0, stack_free_bytes = 0;
    uint32_t thr_live = 0, thr_created = 0, thr_peak = 0;
    char line[640];
    int n;

    read_status_kb(&rss, &hwm);
    ka_heap_stats(&heap_hi, &free_blocks, &free_bytes);
    ka_stack_stats(&stack_hi, &stack_end, &stack_free_b, &stack_free_bytes);
    ka_thread_stats(&thr_live, &thr_created, &thr_peak);

    n = snprintf(line, sizeof line,
                 "[mem] t=%llus frames=%u presents=%d rss=%llukB hwm=%llukB "
                 "heap=%08X alloc=%u/%uK max=%u free=%u/%uK "
                 "stack=%08X/%08X sfree=%u/%uK thr=%u/%u peak=%u "
                 "flow=%08X inst=%08X step=%08X mode=%08X\n",
                 (unsigned long long)((mono_ns() - g_t0_ns) / 1000000000LL),
                 ka_shell_frames(), ka_d3d9_frame_presented(),
                 (unsigned long long)rss, (unsigned long long)hwm,
                 heap_hi, ka_alloc_calls, ka_alloc_bytes / 1024u, ka_alloc_max,
                 free_blocks, free_bytes / 1024u,
                 stack_hi, stack_end, stack_free_b, stack_free_bytes / 1024u,
                 thr_live, thr_created, thr_peak,
                 guest_u32(0x00E38A54u), guest_u32(0x00DBC02Cu),
                 guest_u32(0x00E38AB0u), guest_u32(0x00CA3AC0u));
    if (n > 0) {
        ssize_t w = write(2, line, (size_t)n);
        (void)w;
    }
}

static void *sampler(void *arg) {
    (void)arg;
    g_t0_ns = mono_ns();
    for (;;) {
        struct timespec ts = { 1, 0 };
        sample();
        nanosleep(&ts, NULL);
    }
    return NULL;
}

/* Started from ka_guest_run; a no-op unless the prop is set, so the default
 * build pays nothing and a run opts in with `setprop debug.korra.memtrace 1`. */
void ka_memtrace_start(void) {
    char prop[PROP_VALUE_MAX] = {0};
    pthread_t th;
    if (g_started) return;
    __system_property_get("debug.korra.memtrace", prop);
    if (strcmp(prop, "1") != 0) return;
    g_started = 1;
    if (pthread_create(&th, NULL, sampler, NULL) == 0) pthread_detach(th);
}
