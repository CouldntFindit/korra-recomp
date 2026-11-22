/* frame_probe_android.c - where a device frame's 100 ms sits (debug.korra.perf=1).
 *
 * The 1 Hz [mem] line says what a run costs (rss, threads, stacks) and its
 * presents column says how fast it goes, but not where a slow frame's time is.
 * The one frame boundary the host owns is IDirect3DDevice9::Present, so this
 * probe times around it and prints three numbers per second:
 *
 *   dt       present-to-present wall time (the frame the player sees)
 *   present  time inside the DXVK call (GPU submission + WSI + frame-latency
 *            sync) - the half a DXVK-A00x / dxvk_android fix owns
 *   sleep    time the presenting thread spent in Sleep, i.e. the engine's own
 *            limiter (FUN_00754840 against [frameMgr+0x58])
 *
 * and guest = dt - present - sleep, the lifted work plus the machine-lock
 * waits, which is the half a src/host_android shim owns. Bridged calls per
 * frame, the engine's limiter fields and process CPU are printed beside them so
 * one device line diffs against src/host/perf_probe.c's Windows line.
 *
 * No thread of its own: it reports from the present path, so it cannot perturb
 * scheduling, and when frames stop it goes silent (the spine watchdog then
 * names the machine holder). One line, one write - the [mem] sampler's rule,
 * because several threads interleave multi-part lines.
 *
 *   setprop debug.korra.perf 1          on
 *   setprop debug.korra.perf.frames 1   one line per frame slower than
 *                                       debug.korra.perf.slow (default 50 ms)
 *
 * The engine addresses are the Windows probe's (docs/quirks.md section 4):
 * 0xE4D060 holds the frame-manager singleton, +0x58 its interval in 1/3-ms
 * units, +0x5c its tick in the same units, and 0xE4D210 the 30/60 mode.
 */
#define _GNU_SOURCE
#include <android/log.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/system_properties.h>
#include <time.h>
#include <unistd.h>

#include "host_android.h"

/* The shell's ADPF session (android/app/src/main/cpp/perf_android.c); same
 * libkorra.so, so this is an ordinary call. Reports the CPU half of the frame
 * (dt - present), which is what the platform sizes the CPU budget against. */
extern void korra_perf_guest_frame(long long work_ns);
/* The shell's log path (.../files/log.txt), for the probe's own file. */
extern const char *korra_log_path(void);

#define VA_FRAME_MGR 0x00E4D060u
#define VA_MODE      0x00E4D210u
/* The guest heap and image, i.e. where the singleton can legally live. A
 * pointer outside this is not the frame manager and is never dereferenced. */
#define VA_GUEST_LO  0x00400000u
#define VA_GUEST_HI  0xD0000000u

static int      g_on;
static int      g_frames_on;
static int      g_fd = -1;           /* the probe's own perf.txt append fd */
static unsigned g_slow_ms = 50;
static unsigned g_slow_left = 200;

static uint64_t g_t0;                /* probe start, for the t= column */
static uint64_t g_last_present;      /* ns of the last Present entry */
static pid_t    g_render_tid;        /* set on the first present */
static uint64_t g_frame_no;

/* the current one-second period */
static unsigned g_n;                 /* presents */
static unsigned g_n_dt;              /* presents whose dt was measured */
static uint64_t g_dt_sum, g_dt_min, g_dt_max;
static uint64_t g_pres_sum, g_pres_max;
static uint64_t g_sleep_sum, g_sleep_max;
static uint64_t g_sleep_frame;      /* this frame's sleeps, cleared at each present */
static unsigned g_sleep_calls, g_sleep_zero, g_sleep_one, g_sleep_many;
static uint32_t g_sleep_arg_max;
static uint64_t g_wait_sum, g_wait_max;      /* WaitForSingleObject poll loops */
static unsigned g_wait_calls;
static uint64_t g_lock_sum, g_lock_max;      /* blocked acquiring the machine */
static unsigned g_lock_calls;
static uint64_t g_cs_sum, g_cs_max;          /* blocked on a guest critical section */
static unsigned g_cs_calls;
static unsigned g_icx_calls;                 /* InterlockedCompareExchange calls */
static unsigned g_long50, g_long100;
static uint64_t g_period_start;
static unsigned g_calls_last;
static uint32_t g_tick_last;
static uint64_t g_cpu_last;
static int      g_have_prev;

static uint64_t mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int prop_is(const char *name, const char *want) {
    char v[PROP_VALUE_MAX] = {0};
    __system_property_get(name, v);
    return strcmp(v, want) == 0;
}

static unsigned prop_uint(const char *name, unsigned dflt) {
    char v[PROP_VALUE_MAX] = {0};
    long n;
    __system_property_get(name, v);
    if (!v[0]) return dflt;
    n = strtol(v, NULL, 0);
    return n > 0 ? (unsigned)n : dflt;
}

/* utime + stime of the whole process, in clock ticks. Field 14/15 of
 * /proc/self/stat, counted from the state field after the parenthesised comm
 * (which may itself contain spaces and parentheses, so the last ')' is the
 * separator, not the first). */
static uint64_t proc_cpu_ticks(void) {
    char buf[1024];
    char *p, *save = NULL, *t;
    unsigned long long u = 0, s = 0;
    int field = 3;
    ssize_t n;
    int fd = open("/proc/self/stat", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = 0;
    p = strrchr(buf, ')');
    if (!p) return 0;
    for (t = strtok_r(p + 1, " ", &save); t; t = strtok_r(NULL, " ", &save), field++) {
        if (field == 14) u = strtoull(t, NULL, 10);
        else if (field == 15) { s = strtoull(t, NULL, 10); break; }
    }
    return (uint64_t)(u + s);
}

/* The frame manager, only when the pointer is plausible: inside the guest
 * window and 4-aligned. The interval itself is the second check (0 means the
 * engine has not initialised it yet, anything over 3000 is not 1/3-ms units). */
static uint32_t frame_mgr(void) {
    uint32_t mgr;
    if (!g_mem_base) return 0;
    mgr = MEM32(VA_FRAME_MGR);
    if (mgr < VA_GUEST_LO || mgr >= VA_GUEST_HI || (mgr & 3u)) return 0;
    return mgr;
}

static void emit_line(char *line, int n) {
    if (n > 0) {
        ssize_t w;
        /* Three channels on purpose. fd 2 is the shell's pump (file + logcat);
         * a run on 2026-11-22 lost every line after t=266 s while the game kept
         * rendering, so the probe also keeps its own append fd and writes to
         * logcat directly. Whichever survives, the numbers are there. */
        w = write(2, line, (size_t)n);
        (void)w;
        if (g_fd >= 0) {
            w = write(g_fd, line, (size_t)n);
            (void)w;
        }
        __android_log_write(ANDROID_LOG_INFO, "KorraPerf", line);
    }
}

/* Opened once: <files dir>/perf.txt, derived from the shell's log path. */
static void probe_open(void) {
    const char *p = korra_log_path();
    char path[512];
    if (!p || !*p) return;
    {
        const char *slash = strrchr(p, '/');
        size_t n = slash ? (size_t)(slash - p) : 0;
        if (!n || n + 16 >= sizeof path) return;
        memcpy(path, p, n);
        path[n] = 0;
        strncat(path, "/perf.txt", sizeof path - strlen(path) - 1);
    }
    g_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND | O_CLOEXEC, 0644);
}

static void report(uint64_t now) {
    char line[768];
    int n;
    double secs = (double)(now - g_period_start) / 1e9;
    uint32_t mgr = frame_mgr();
    uint32_t interval = mgr ? MEM32(mgr + 0x58) : 0;
    uint32_t tick = mgr ? MEM32(mgr + 0x5c) : 0;
    uint32_t mode = MEM32(VA_MODE);
    uint32_t thr_live = 0, thr_created = 0, thr_peak = 0;
    double dt_avg = g_n_dt ? (double)g_dt_sum / (double)g_n_dt / 1e6 : 0.0;
    double pres_avg = g_n ? (double)g_pres_sum / (double)g_n / 1e6 : 0.0;
    double sleep_avg = g_n ? (double)g_sleep_sum / (double)g_n / 1e6 : 0.0;
    double wait_avg = g_n ? (double)g_wait_sum / (double)g_n / 1e6 : 0.0;
    double lock_avg = g_n ? (double)g_lock_sum / (double)g_n / 1e6 : 0.0;
    double cs_avg = g_n ? (double)g_cs_sum / (double)g_n / 1e6 : 0.0;
    double guest_avg = dt_avg - pres_avg - sleep_avg - lock_avg - cs_avg;
    double clk_rate = (g_have_prev && secs > 0.0)
                    ? (double)(uint32_t)(tick - g_tick_last) / 3.0 / secs : 0.0;
    unsigned calls_per_frame = g_n ? (ka_import_calls() - g_calls_last + g_n / 2) / g_n : 0;
    double cpu_pct = 0.0;
    uint64_t cpu = proc_cpu_ticks();
    long hz = sysconf(_SC_CLK_TCK);
    if (hz <= 0) hz = 100;

    if (g_have_prev && secs > 0.0)
        cpu_pct = (double)(cpu - g_cpu_last) / (double)hz / secs * 100.0;

    ka_thread_stats(&thr_live, &thr_created, &thr_peak);
    n = snprintf(line, sizeof line,
                 "[perf] t=%llus n=%u dt=%.1f/%.1f/%.1fms long50=%u long100=%u "
                 "present=%.1f/%.1fms sleep=%.1f/%.1fms wait=%.1f/%.1fms "
                 "lock=%.1f/%.1fms cs=%.1f/%.1fms guest=%.1fms "
                 "calls=%u/f clk=%.0f/s int=%u mode=%u cpu=%.0f%% thr=%u "
                 "slp=%u(0:%u,1:%u,mx%u) wt=%u csn=%u icx=%u\n",
                 (unsigned long long)((now - g_t0) / 1000000000ull),
                 g_n,
                 dt_avg,
                 g_n_dt ? (double)g_dt_min / 1e6 : 0.0,
                 g_n_dt ? (double)g_dt_max / 1e6 : 0.0,
                 g_long50, g_long100,
                 pres_avg, (double)g_pres_max / 1e6,
                 sleep_avg, (double)g_sleep_max / 1e6,
                 wait_avg, (double)g_wait_max / 1e6,
                 lock_avg, (double)g_lock_max / 1e6,
                 cs_avg, (double)g_cs_max / 1e6,
                 guest_avg, calls_per_frame, clk_rate,
                 interval, mode, cpu_pct, thr_live,
                 g_sleep_calls, g_sleep_zero, g_sleep_one, g_sleep_arg_max,
                 g_wait_calls, g_cs_calls, g_icx_calls);
    emit_line(line, n);

    g_n = g_n_dt = 0;
    g_dt_sum = g_dt_min = g_dt_max = 0;
    g_pres_sum = g_pres_max = 0;
    g_sleep_sum = g_sleep_max = 0;
    g_sleep_calls = g_sleep_zero = g_sleep_one = g_sleep_many = 0;
    g_sleep_arg_max = 0;
    g_wait_sum = g_wait_max = 0;
    g_wait_calls = 0;
    g_lock_sum = g_lock_max = 0;
    g_lock_calls = 0;
    g_cs_sum = g_cs_max = 0;
    g_cs_calls = 0;
    g_icx_calls = 0;
    g_long50 = g_long100 = 0;
    g_calls_last = ka_import_calls();
    g_tick_last = tick;
    g_cpu_last = cpu;
    g_have_prev = 1;
    g_period_start = now;
}

/* True while the probe is on; the call sites use it to skip the clock read. */
int ka_fp_on(void) { return g_on; }

/* Start of the timed region around IDirect3DDevice9::Present. Always on: the
 * platform hint and the frame cap ride on this same boundary, so these two
 * clock reads are part of the port and not of the probe. */
uint64_t ka_fp_begin(void) { return mono_ns(); }

/* End of IDirect3DDevice9::Present. Runs on the presenting (render) thread.
 * Always: hand the frame's CPU half to the platform's hint session and run the
 * frame cap's watchdog. With debug.korra.perf=1, also accumulate the per-second
 * decomposition and print it. */
void ka_fp_end_present(uint64_t t0) {
    uint64_t t1, dt, pres;
    if (!t0) return;
    t1 = mono_ns();
    pres = t1 - t0;
    dt = g_last_present ? t0 - g_last_present : 0;
    g_last_present = t0;

    if (dt) korra_perf_guest_frame((long long)(dt > pres ? dt - pres : 0));
    ka_fps_tick();

    if (!g_on) return;

    if (!g_render_tid) {
        g_render_tid = gettid();
        g_period_start = t0;
        g_calls_last = ka_import_calls();
        {
            uint32_t mgr = frame_mgr();
            g_tick_last = mgr ? MEM32(mgr + 0x5c) : 0;
        }
        g_cpu_last = proc_cpu_ticks();
        g_have_prev = 1;
        {
            char line[256];
            emit_line(line, snprintf(line, sizeof line,
                "[perf] frame probe on (render tid %d, slow>=%ums frames=%s)\n",
                (int)g_render_tid, g_slow_ms, g_frames_on ? "on" : "off"));
        }
    }
    g_frame_no++;
    g_n++;
    g_pres_sum += pres;
    if (pres > g_pres_max) g_pres_max = pres;

    if (dt) {
        g_n_dt++;
        g_dt_sum += dt;
        if (!g_dt_min || dt < g_dt_min) g_dt_min = dt;
        if (dt > g_dt_max) g_dt_max = dt;
        if (dt > 50000000ull) g_long50++;
        if (dt > 100000000ull) g_long100++;
        if (g_frames_on && g_slow_left && dt >= (uint64_t)g_slow_ms * 1000000ull) {
            char line[256];
            g_slow_left--;
            emit_line(line, snprintf(line, sizeof line,
                "[perf] slow f=%llu dt=%.1fms present=%.1fms sleep=%.1fms calls=%u\n",
                (unsigned long long)g_frame_no, (double)dt / 1e6,
                (double)pres / 1e6, (double)g_sleep_frame / 1e6,
                ka_import_calls()));
        }
    }
    g_sleep_frame = 0;

    if (t1 - g_period_start >= 1000000000ull) report(t1);
}

/* Nanosleep actually spent, measured around it in kernel32's guest_sleep_ms and
 * counted only for the presenting thread (the limiter is the frame's own
 * sleep; a worker's timed wait is not). The requested milliseconds are kept as
 * a histogram because they name the caller: ~31 ms is the limiter's
 * (interval-elapsed)/3, 1 ms is the engine's WaitForSingleObject poll loop. */
void ka_fp_end_sleep(uint64_t t0, uint32_t ms) {
    uint64_t t1;
    if (!g_on || !t0 || !g_render_tid || gettid() != g_render_tid) return;
    t1 = mono_ns();
    {
        uint64_t d = t1 - t0;
        g_sleep_sum += d;
        g_sleep_frame += d;
        if (d > g_sleep_max) g_sleep_max = d;
        g_sleep_calls++;
        if (ms == 0) g_sleep_zero++;
        else if (ms == 1) g_sleep_one++;
        else g_sleep_many++;
        if (ms > g_sleep_arg_max) g_sleep_arg_max = ms;
    }
}

/* The engine's own waits are 1 ms polls (kernel32's k_WaitForSingleObject
 * loops guest_sleep_ms(1)), so the time a frame spends waiting for its workers
 * hides inside the sleep bucket. Measuring the whole loop separates them: on
 * Windows the same handshake was 34 WaitForSingleObject calls worth 16-21 ms of
 * a 25 ms frame (src/host/perf_probe.c), and this is the device's number. */
void ka_fp_wait_done(uint64_t t0) {
    uint64_t d;
    if (!g_on || !g_render_tid || gettid() != g_render_tid) return;
    d = mono_ns() - t0;
    g_wait_sum += d;
    g_wait_calls++;
    if (d > g_wait_max) g_wait_max = d;
}

/* Blocked in mach_enter(): another guest thread held the machine. This is the
 * single-machine-lock cost measured directly instead of inferred. */
void ka_fp_lock_waited(uint64_t ns) {
    if (!g_on || !g_render_tid || gettid() != g_render_tid) return;
    g_lock_sum += ns;
    g_lock_calls++;
    if (ns > g_lock_max) g_lock_max = ns;
}

/* Blocked in EnterCriticalSection on a guest lock another thread owned. */
void ka_fp_cs_waited(uint64_t ns) {
    if (!g_on || !g_render_tid || gettid() != g_render_tid) return;
    g_cs_sum += ns;
    g_cs_calls++;
    if (ns > g_cs_max) g_cs_max = ns;
}

/* The engine's own spinlock primitive (docs/quirks.md and the Windows probe:
 * 400-670 calls a frame). Count only: its cost lands in the guest bucket. */
void ka_fp_icx(void) {
    if (!g_on || !g_render_tid || gettid() != g_render_tid) return;
    g_icx_calls++;
}

/* For the mach_enter/EnterCriticalSection hooks: is this cheap enough to time? */
int ka_fp_lock_timing(void) { return g_on; }
uint64_t ka_fp_now(void) { return mono_ns(); }

/* Started from ka_guest_run after the image is mapped (the sampler reads guest
 * globals, which do not exist before that). A no-op unless opted in. */
void ka_frame_probe_start(void) {
    if (g_on) return;
    if (!prop_is("debug.korra.perf", "1")) return;
    g_frames_on = prop_is("debug.korra.perf.frames", "1");
    g_slow_ms = prop_uint("debug.korra.perf.slow", 50);
    g_t0 = mono_ns();
    probe_open();
    g_on = 1;
}
