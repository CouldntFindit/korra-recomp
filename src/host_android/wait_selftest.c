/* wait_selftest.c - the wait-object state machine's own scenarios.
 *
 * debug.korra.waittest=1 runs these before the guest thread exists, from
 * ka_guest_run (main_android.c). Each scenario prints one line
 *
 *   [wait-selftest] <name> PASS|FAIL <detail>
 *
 * and the run ends with `[wait-selftest] N/M PASS`. The scenarios drive the
 * same ka_obj_* API the kernel32 shims use, with a plain nanosleep in place of
 * guest_sleep_ms: these are host pthreads, not guest threads, and
 * guest_sleep_ms unwinds and re-enters the machine lock (illegal here).
 *
 * The baseline is the Windows contract (performance_plan.md section 5.1):
 * auto-reset = one set enables exactly one wait, no accumulation; manual-reset
 * stays signaled until ResetEvent; a semaphore counts and each wait consumes
 * one; ReleaseSemaphore clamps at maxcount with ERROR_TOO_MANY_POSTS (298);
 * ms=0 polls, a finite timeout returns WAIT_TIMEOUT at the deadline, INFINITE
 * blocks until signaled; wait-any returns the lowest index and consumes only
 * that one, wait-all consumes nothing until all are ready.
 *
 * Mutex ownership has no scenario: Step 0 measured zero mutex waits (120
 * probed waits: 91 semaphore, 29 event, 0 mutex) and zero mutex closes, so it
 * is implemented as it was and marked unexercised in the FINDINGS entry.
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/system_properties.h>
#include <time.h>

#include "host_android.h"
#include "wait_objects.h"

static int g_pass, g_total;

static uint64_t mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)(ts.tv_nsec / 1000000L);
}

/* The state machine's sleep for these scenarios: a host pthread must not call
 * guest_sleep_ms. */
static void test_sleep(uint32_t ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static void report(const char *name, int ok, const char *fmt, ...) {
    char detail[220];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(detail, sizeof detail, fmt, ap);
    va_end(ap);
    ka_log("[wait-selftest] %s %s %s", name, ok ? "PASS" : "FAIL", detail);
    g_total++;
    if (ok) g_pass++;
}

/* ---- two-waiter plumbing ---------------------------------------------- */
typedef struct {
    uint32_t h, ms;
    uint64_t t0, t1;
    ka_wait_result r;
    int started, done;
} waiter_arg;

static void *waiter_main(void *p) {
    waiter_arg *w = (waiter_arg *)p;
    __atomic_store_n(&w->started, 1, __ATOMIC_RELEASE);
    w->t0 = mono_ms();
    w->r = ka_obj_wait(w->h, w->ms, test_sleep);
    w->t1 = mono_ms();
    __atomic_store_n(&w->done, 1, __ATOMIC_RELEASE);
    return NULL;
}

static int start_waiter(waiter_arg *w, pthread_t *t, uint32_t h, uint32_t ms) {
    memset(w, 0, sizeof *w);
    w->h = h;
    w->ms = ms;
    return pthread_create(t, NULL, waiter_main, w) == 0 ? 0 : -1;
}

static int wait_started(waiter_arg *w) {
    int i;
    for (i = 0; i < 300; i++) {
        if (__atomic_load_n(&w->started, __ATOMIC_ACQUIRE)) return 0;
        test_sleep(1);
    }
    return -1;
}

/* Bounded join (bionic has no pthread_timedjoin_np): wait for the waiter's
 * done flag, then join. A semantics failure must be reported, never hang the
 * app; a waiter that never returns is left unjoined and its object unfreed. */
static int join_waiter(waiter_arg *w, pthread_t t, int ms) {
    int i;
    for (i = 0; i < ms; i++) {
        if (__atomic_load_n(&w->done, __ATOMIC_ACQUIRE)) {
            pthread_join(t, NULL);
            return 1;
        }
        test_sleep(1);
    }
    return 0;
}

static uint64_t waited_ms(const waiter_arg *w) {
    return w->t1 > w->t0 ? w->t1 - w->t0 : 0;
}

/* ---- auto-reset events ------------------------------------------------- */
static void sc_a1(void) {
    uint32_t ev = ka_obj_new(KA_OBJ_EVENT, 0, 0);
    waiter_arg w1, w2;
    pthread_t t1, t2;
    uint32_t woke = 0, timed = 0;
    uint64_t to_ms = 0;
    int ok = ev != 0;
    int j1 = 0, j2 = 0;
    if (ok) ok = start_waiter(&w1, &t1, ev, 300) == 0
             && start_waiter(&w2, &t2, ev, 300) == 0;
    if (ok) {
        ok = wait_started(&w1) == 0 && wait_started(&w2) == 0;
        test_sleep(30);
        ka_obj_set(ev);
        j1 = join_waiter(&w1, t1, 2000);
        j2 = join_waiter(&w2, t2, 2000);
        if (!j1 || !j2) ok = 0;
        woke = (uint32_t)(w1.r.result == KA_WAIT_OBJECT_0) + (uint32_t)(w2.r.result == KA_WAIT_OBJECT_0);
        timed = (uint32_t)(w1.r.result == KA_WAIT_TIMEOUT) + (uint32_t)(w2.r.result == KA_WAIT_TIMEOUT);
        to_ms = (w1.r.result == KA_WAIT_TIMEOUT) ? waited_ms(&w1) : waited_ms(&w2);
        ok = ok && woke == 1 && timed == 1 && to_ms >= 250;
    }
    report("A1", ok, "one set, two waiters: %u woke, %u timed out at %llu ms",
           woke, timed, (unsigned long long)to_ms);
    if (j1 && j2) ka_obj_close(ev);
}

static void sc_a2(void) {
    uint32_t ev = ka_obj_new(KA_OBJ_EVENT, 0, 0);
    waiter_arg w1, w2;
    pthread_t t1, t2;
    uint64_t lo = 0, hi = 0;
    int ok = ev != 0;
    int j1 = 0, j2 = 0;
    if (ok) ok = start_waiter(&w1, &t1, ev, 700) == 0
             && start_waiter(&w2, &t2, ev, 700) == 0;
    if (ok) {
        ok = wait_started(&w1) == 0 && wait_started(&w2) == 0;
        test_sleep(30);
        ka_obj_set(ev);
        test_sleep(70);
        ka_obj_set(ev);
        j1 = join_waiter(&w1, t1, 2500);
        j2 = join_waiter(&w2, t2, 2500);
        if (!j1 || !j2) ok = 0;
        lo = waited_ms(&w1) < waited_ms(&w2) ? waited_ms(&w1) : waited_ms(&w2);
        hi = waited_ms(&w1) > waited_ms(&w2) ? waited_ms(&w1) : waited_ms(&w2);
        ok = ok && w1.r.result == KA_WAIT_OBJECT_0 && w2.r.result == KA_WAIT_OBJECT_0
                && lo < 80 && hi >= 90;
    }
    report("A2", ok, "second set wakes the second: %llu ms then %llu ms",
           (unsigned long long)lo, (unsigned long long)hi);
    if (j1 && j2) ka_obj_close(ev);
}

static void sc_a3(void) {
    uint32_t ev = ka_obj_new(KA_OBJ_EVENT, 0, 0);
    ka_wait_result r1 = { KA_WAIT_FAILED, 0, 0 }, r2 = r1;
    int ok = ev != 0;
    if (ok) {
        ka_obj_set(ev);
        r1 = ka_obj_wait(ev, 0, test_sleep);
        r2 = ka_obj_wait(ev, 0, test_sleep);
        ok = r1.result == KA_WAIT_OBJECT_0 && r2.result == KA_WAIT_TIMEOUT;
        ka_obj_close(ev);
    }
    report("A3", ok, "set with no waiter: next wait %u, second wait %u",
           r1.result, r2.result);
}

static void sc_a4(void) {
    uint32_t ev = ka_obj_new(KA_OBJ_EVENT, 0, 0);
    ka_wait_result r1 = { KA_WAIT_FAILED, 0, 0 };
    ka_wait_result r2 = r1, r3 = r1;
    uint64_t t0 = 0, el = 0;
    int ok = ev != 0;
    if (ok) {
        ka_obj_set(ev);
        ka_obj_set(ev);   /* while signaled: no second credit */
        r1 = ka_obj_wait(ev, 0, test_sleep);
        t0 = mono_ms();
        r2 = ka_obj_wait(ev, 60, test_sleep);
        el = mono_ms() - t0;
        r3 = ka_obj_wait(ev, 0, test_sleep);
        ok = r1.result == KA_WAIT_OBJECT_0 && r2.result == KA_WAIT_TIMEOUT
                && el >= 60 && r3.result == KA_WAIT_TIMEOUT;
        ka_obj_close(ev);
    }
    report("A4", ok, "double set adds no credit: %u then %u after %llu ms then %u",
           r1.result, r2.result, (unsigned long long)el, r3.result);
}

/* ---- manual-reset events ----------------------------------------------- */
static void sc_m1(void) {
    uint32_t ev = ka_obj_new(KA_OBJ_EVENT, 1, 0);
    waiter_arg w1, w2;
    pthread_t t1, t2;
    int ok = ev != 0;
    int j1 = 0, j2 = 0;
    uint64_t el1 = 0, el2 = 0;
    if (ok) ok = start_waiter(&w1, &t1, ev, 400) == 0
             && start_waiter(&w2, &t2, ev, 400) == 0;
    if (ok) {
        ok = wait_started(&w1) == 0 && wait_started(&w2) == 0;
        test_sleep(30);
        ka_obj_set(ev);
        j1 = join_waiter(&w1, t1, 2000);
        j2 = join_waiter(&w2, t2, 2000);
        if (!j1 || !j2) ok = 0;
        el1 = waited_ms(&w1);
        el2 = waited_ms(&w2);
        ok = ok && w1.r.result == KA_WAIT_OBJECT_0 && w2.r.result == KA_WAIT_OBJECT_0
                && el1 < 250 && el2 < 250;
    }
    report("M1", ok, "one set wakes both: %llu ms / %llu ms",
           (unsigned long long)el1, (unsigned long long)el2);
    if (j1 && j2) ka_obj_close(ev);
}

static void sc_m2(void) {
    uint32_t ev = ka_obj_new(KA_OBJ_EVENT, 1, 0);
    ka_wait_result r1 = { KA_WAIT_FAILED, 0, 0 }, r2 = r1;
    uint64_t t0 = 0, el = 0;
    int ok = ev != 0;
    if (ok) {
        ka_obj_set(ev);
        r1 = ka_obj_wait(ev, 0, test_sleep);
        ka_obj_reset(ev);
        t0 = mono_ms();
        r2 = ka_obj_wait(ev, 40, test_sleep);
        el = mono_ms() - t0;
        ok = r1.result == KA_WAIT_OBJECT_0 && r2.result == KA_WAIT_TIMEOUT && el >= 40;
        ka_obj_close(ev);
    }
    report("M2", ok, "reset then wait: %u after %llu ms (set first: %u)",
           r2.result, (unsigned long long)el, r1.result);
}

static void sc_m3(void) {
    uint32_t ev = ka_obj_new(KA_OBJ_EVENT, 1, 0);
    ka_wait_result r1 = { KA_WAIT_FAILED, 0, 0 }, r2 = r1;
    int ok = ev != 0;
    if (ok) {
        ka_obj_set(ev);
        r1 = ka_obj_wait(ev, 0, test_sleep);
        r2 = ka_obj_wait(ev, 0, test_sleep);
        ok = r1.result == KA_WAIT_OBJECT_0 && r2.result == KA_WAIT_OBJECT_0;
        ka_obj_close(ev);
    }
    report("M3", ok, "set with no waiter stays signaled: %u, %u", r1.result, r2.result);
}

/* ---- semaphores -------------------------------------------------------- */
static void sc_s1(void) {
    uint32_t sem = ka_obj_new_sem(0, 2);
    waiter_arg w1, w2;
    pthread_t t1, t2;
    uint64_t lo = 0, hi = 0;
    int ok = sem != 0, ok1 = 0, ok2 = 0;
    int j1 = 0, j2 = 0;
    uint32_t prev = 0;
    if (ok) ok = start_waiter(&w1, &t1, sem, 400) == 0
             && start_waiter(&w2, &t2, sem, 400) == 0;
    if (ok) {
        ok = wait_started(&w1) == 0 && wait_started(&w2) == 0;
        test_sleep(30);
        ok1 = ka_obj_release_sem(sem, 1, &prev);
        test_sleep(70);
        ok2 = ka_obj_release_sem(sem, 1, &prev);
        j1 = join_waiter(&w1, t1, 2000);
        j2 = join_waiter(&w2, t2, 2000);
        if (!j1 || !j2) ok = 0;
        lo = waited_ms(&w1) < waited_ms(&w2) ? waited_ms(&w1) : waited_ms(&w2);
        hi = waited_ms(&w1) > waited_ms(&w2) ? waited_ms(&w1) : waited_ms(&w2);
        ok = ok && ok1 && ok2 && w1.r.result == KA_WAIT_OBJECT_0
                && w2.r.result == KA_WAIT_OBJECT_0 && lo < 80 && hi >= 90;
    }
    report("S1", ok, "two releases of 1 wake both: %llu ms then %llu ms",
           (unsigned long long)lo, (unsigned long long)hi);
    if (j1 && j2) ka_obj_close(sem);
}

static void sc_s2(void) {
    uint32_t sem = ka_obj_new_sem(0, 1);
    ka_wait_result r = { KA_WAIT_FAILED, 0, 0 };
    uint64_t t0 = 0, el = 0;
    int ok = sem != 0;
    if (ok) {
        t0 = mono_ms();
        r = ka_obj_wait(sem, 30, test_sleep);
        el = mono_ms() - t0;
        ok = r.result == KA_WAIT_TIMEOUT && el >= 30 && el < 80;
        ka_obj_close(sem);
    }
    report("S2", ok, "third wait times out: %u after %llu ms",
           r.result, (unsigned long long)el);
}

static void sc_s3(void) {
    uint32_t sem = ka_obj_new_sem(0, 1);
    ka_obj snap = {0};
    int idx;
    uint32_t prev = 0;
    int ok = sem != 0, r1 = 0, r2 = 0;
    uint32_t err = 0, cnt = 0;
    if (ok) {
        r1 = ka_obj_release_sem(sem, 1, &prev);
        r2 = ka_obj_release_sem(sem, 1, &prev);
        err = ka_last_error_get();
        ka_obj_snapshot(sem, &snap, &idx);
        cnt = (uint32_t)snap.count;
        ok = r1 == 1 && r2 == 0 && err == KA_ERROR_TOO_MANY_POSTS && cnt == 1;
        ka_obj_close(sem);
    }
    report("S3", ok, "release over max: ret %d, last error %u, count %u (first ret %d)",
           r2, err, cnt, r1);
}

static void sc_s4(void) {
    uint32_t sem = ka_obj_new_sem(1, 3);
    ka_obj snap = {0};
    int idx;
    uint32_t prev = 0;
    int ok = sem != 0, r = 0;
    uint32_t cnt = 0;
    if (ok) {
        r = ka_obj_release_sem(sem, 2, &prev);
        ka_obj_snapshot(sem, &snap, &idx);
        cnt = (uint32_t)snap.count;
        ok = r == 1 && prev == 1 && cnt == 3;
        ka_obj_close(sem);
    }
    report("S4", ok, "lPreviousCount %u, count after +2 %u", prev, cnt);
}

static void sc_s5(void) {
    uint32_t sem = ka_obj_new_sem(2, 2);
    ka_obj snap = {0};
    int idx;
    ka_wait_result r1 = { KA_WAIT_FAILED, 0, 0 }, r2 = r1, r3 = r1;
    uint32_t c1 = 0, c2 = 0;
    int ok = sem != 0;
    if (ok) {
        r1 = ka_obj_wait(sem, 0, test_sleep);
        ka_obj_snapshot(sem, &snap, &idx);
        c1 = (uint32_t)snap.count;
        r2 = ka_obj_wait(sem, 0, test_sleep);
        ka_obj_snapshot(sem, &snap, &idx);
        c2 = (uint32_t)snap.count;
        r3 = ka_obj_wait(sem, 0, test_sleep);
        ok = r1.result == KA_WAIT_OBJECT_0 && r2.result == KA_WAIT_OBJECT_0
                && c1 == 1 && c2 == 0 && r3.result == KA_WAIT_TIMEOUT;
        ka_obj_close(sem);
    }
    report("S5", ok, "each wait consumes one: counts %u then %u, third wait %u",
           c1, c2, r3.result);
}

/* ---- timeouts ---------------------------------------------------------- */
static void sc_t1(void) {
    uint32_t ev = ka_obj_new(KA_OBJ_EVENT, 0, 0);
    ka_wait_result r = { KA_WAIT_FAILED, 0, 0 };
    uint64_t t0 = 0, el = 0;
    int ok = ev != 0;
    if (ok) {
        t0 = mono_ms();
        r = ka_obj_wait(ev, 0, test_sleep);
        el = mono_ms() - t0;
        ok = r.result == KA_WAIT_TIMEOUT && el < 20;
        ka_obj_close(ev);
    }
    report("T1", ok, "ms=0 unsignaled: %u in %llu ms", r.result, (unsigned long long)el);
}

static void sc_t2(void) {
    uint32_t ev = ka_obj_new(KA_OBJ_EVENT, 0, 0);
    ka_wait_result r = { KA_WAIT_FAILED, 0, 0 };
    uint64_t t0 = 0, el = 0;
    int ok = ev != 0;
    if (ok) {
        t0 = mono_ms();
        r = ka_obj_wait(ev, 60, test_sleep);
        el = mono_ms() - t0;
        ok = r.result == KA_WAIT_TIMEOUT && el >= 60 && el < 110;
        ka_obj_close(ev);
    }
    report("T2", ok, "ms=60 unsignaled: %u at %llu ms", r.result, (unsigned long long)el);
}

static void sc_t3(void) {
    uint32_t ev = ka_obj_new(KA_OBJ_EVENT, 0, 0);
    waiter_arg w = {0};
    pthread_t t;
    int ok = ev != 0, joined = 0;
    uint64_t el = 0;
    if (ok) ok = start_waiter(&w, &t, ev, KA_WAIT_INFINITE) == 0;
    if (ok) {
        ok = wait_started(&w) == 0;
        test_sleep(40);
        ka_obj_set(ev);
        joined = join_waiter(&w, t, 2000);
        if (!joined) ok = 0;
        el = waited_ms(&w);
        ok = ok && w.r.result == KA_WAIT_OBJECT_0 && el >= 30 && el < 1500;
    }
    report("T3", ok, "INFINITE blocks until set: %u at %llu ms",
           w.r.result, (unsigned long long)el);
    if (joined) ka_obj_close(ev);
}

/* ---- multiple objects -------------------------------------------------- */
static void sc_x1(void) {
    uint32_t hs[2];
    ka_obj s1 = {0}, s2 = {0};
    int idx;
    ka_wait_result r = { KA_WAIT_FAILED, 0, 0 };
    int ok;
    hs[0] = ka_obj_new(KA_OBJ_EVENT, 0, 0);
    hs[1] = ka_obj_new(KA_OBJ_EVENT, 0, 0);
    ok = hs[0] != 0 && hs[1] != 0;
    if (ok) {
        ka_obj_set(hs[0]);
        ka_obj_set(hs[1]);
        r = ka_obj_wait_multi(hs, 2, 0, 0, test_sleep);
        ka_obj_snapshot(hs[0], &s1, &idx);
        ka_obj_snapshot(hs[1], &s2, &idx);
        ok = r.result == 0 && s1.signaled == 0 && s2.signaled == 1;
    }
    report("X1", ok, "wait-any lowest index: ret %u, first signaled %d, second %d",
           r.result, s1.signaled, s2.signaled);
    ka_obj_close(hs[0]);
    ka_obj_close(hs[1]);
}

static void sc_x2(void) {
    uint32_t hs[2];
    ka_obj s1 = {0}, s2 = {0};
    int idx;
    ka_wait_result r1 = { KA_WAIT_FAILED, 0, 0 }, r2 = r1;
    int ok, keep = 0;
    hs[0] = ka_obj_new(KA_OBJ_EVENT, 0, 0);
    hs[1] = ka_obj_new(KA_OBJ_EVENT, 0, 0);
    ok = hs[0] != 0 && hs[1] != 0;
    if (ok) {
        ka_obj_set(hs[0]);
        r1 = ka_obj_wait_multi(hs, 2, 1, 40, test_sleep);
        ka_obj_snapshot(hs[0], &s1, &idx);
        keep = s1.signaled;
        ka_obj_set(hs[1]);
        r2 = ka_obj_wait_multi(hs, 2, 1, 300, test_sleep);
        ka_obj_snapshot(hs[0], &s1, &idx);
        ka_obj_snapshot(hs[1], &s2, &idx);
        ok = r1.result == KA_WAIT_TIMEOUT && keep == 1
                && r2.result == KA_WAIT_OBJECT_0 && s1.signaled == 0 && s2.signaled == 0;
    }
    report("X2", ok, "wait-all: partial %u kept %d, complete %u consumed %d/%d",
           r1.result, keep, r2.result, s1.signaled, s2.signaled);
    ka_obj_close(hs[0]);
    ka_obj_close(hs[1]);
}

static void sc_x3(void) {
    uint32_t hs[1];
    ka_obj m = {0};
    int idx;
    ka_wait_result r = { KA_WAIT_FAILED, 0, 0 };
    int ok;
    hs[0] = ka_obj_new(KA_OBJ_EVENT, 1, 0);
    ok = hs[0] != 0;
    if (ok) {
        ka_obj_set(hs[0]);
        r = ka_obj_wait_multi(hs, 1, 0, 0, test_sleep);
        ka_obj_snapshot(hs[0], &m, &idx);
        ok = r.result == 0 && m.signaled == 1;
    }
    report("X3", ok, "wait-any on manual: ret %u, still signaled %d", r.result, m.signaled);
    ka_obj_close(hs[0]);
}

int ka_wait_selftest(void) {
    char v[PROP_VALUE_MAX];
    if (__system_property_get("debug.korra.waittest", v) <= 0 || v[0] != '1') return 0;
    g_pass = g_total = 0;
    ka_log("[wait-selftest] start (18 scenarios, debug.korra.waittest=1)");
    sc_a1(); sc_a2(); sc_a3(); sc_a4();
    sc_m1(); sc_m2(); sc_m3();
    sc_s1(); sc_s2(); sc_s3(); sc_s4(); sc_s5();
    sc_t1(); sc_t2(); sc_t3();
    sc_x1(); sc_x2(); sc_x3();
    ka_log("[wait-selftest] %d/%d PASS", g_pass, g_total);
    return g_pass == g_total ? 0 : 1;
}
