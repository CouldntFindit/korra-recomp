/* wait_objects.c - Windows wait-object semantics on the Android host.
 *
 * Extracted from kernel32_android.c (Stage 0, 2026-11-27) so the same state
 * machine can be driven by the selftest (wait_selftest.c) and by the guest
 * shims. The shims keep every probe line and the stdcall ABI; this file owns
 * the table and the decisions.
 *
 * Measured engine use (docs/FINDINGS.md 2026-11-27, Step 0): 24 semaphores
 * created per boot, all init=0/max=1; 120 probed waits split 91 sem / 29
 * event / 0 mutex; 60 releases all n=1; 60 SetEvent all events. The semaphore
 * count therefore matters on the hot path (the pool pump releases a slot's
 * posted semaphore and waits on its done semaphore), while mutex ownership is
 * unexercised and stays as it was.
 *
 * Stage 0 is poll-based by design. A waiter checks and consumes under
 * g_obj_lock, then sleeps through the caller's callback and re-resolves the
 * handle; a closed handle's slot can be recycled by ka_obj_new, so no pointer
 * captured before a sleep is ever used to consume.
 */
#include <pthread.h>
#include <stdint.h>
#include <string.h>

#include "host_android.h"
#include "wait_objects.h"

static ka_obj g_obj[KA_OBJ_MAX];
static pthread_mutex_t g_obj_lock = PTHREAD_MUTEX_INITIALIZER;

/* Caller holds g_obj_lock. */
static ka_obj *obj_at(uint32_t h) {
    uint32_t i = h - 1;
    if (h == 0 || i >= KA_OBJ_MAX || !g_obj[i].used) return NULL;
    return &g_obj[i];
}

/* Caller holds g_obj_lock. */
static uint32_t obj_create_locked(int kind, int manual, int initial) {
    uint32_t i;
    for (i = 0; i < KA_OBJ_MAX; i++) {
        if (!g_obj[i].used) {
            /* Every field, including `owned`: a recycled slot must not keep
             * the previous object's ownership bit (M3, 2026-11-21). */
            g_obj[i].used = 1;
            g_obj[i].kind = kind;
            g_obj[i].manual = manual;
            g_obj[i].signaled = initial;
            g_obj[i].count = initial;
            g_obj[i].maxcount = 0;
            g_obj[i].owned = 0;
            return i + 1;
        }
    }
    return 0;
}

uint32_t ka_obj_new(int kind, int manual, int initial) {
    uint32_t h;
    pthread_mutex_lock(&g_obj_lock);
    h = obj_create_locked(kind, manual, initial);
    pthread_mutex_unlock(&g_obj_lock);
    return h;
}

uint32_t ka_obj_new_sem(int initial, int maxcount) {
    uint32_t h;
    pthread_mutex_lock(&g_obj_lock);
    h = obj_create_locked(KA_OBJ_SEM, 0, initial != 0);
    if (h) {
        /* A semaphore's signal is its count, never a separate bit. */
        g_obj[h - 1].count = initial;
        g_obj[h - 1].maxcount = maxcount;
        g_obj[h - 1].signaled = initial > 0;
    }
    pthread_mutex_unlock(&g_obj_lock);
    return h;
}

int ka_obj_snapshot(uint32_t h, ka_obj *out, int *idx) {
    ka_obj *o;
    int found = 0;
    pthread_mutex_lock(&g_obj_lock);
    o = obj_at(h);
    if (o) {
        *out = *o;
        if (idx) *idx = (int)(o - g_obj);
        found = 1;
    }
    pthread_mutex_unlock(&g_obj_lock);
    return found;
}

int ka_obj_close(uint32_t h) {
    ka_obj *o;
    pthread_mutex_lock(&g_obj_lock);
    o = obj_at(h);
    if (o) {
        o->used = 0;
        o->owned = 0;
    }
    pthread_mutex_unlock(&g_obj_lock);
    return o != NULL;
}

int ka_obj_set(uint32_t h) {
    ka_obj *o;
    pthread_mutex_lock(&g_obj_lock);
    o = obj_at(h);
    /* Stage 0 baseline: any object's signal bit is set, as the old shim did. */
    if (o) o->signaled = 1;
    pthread_mutex_unlock(&g_obj_lock);
    return 1;
}

int ka_obj_reset(uint32_t h) {
    ka_obj *o;
    pthread_mutex_lock(&g_obj_lock);
    o = obj_at(h);
    if (o) o->signaled = 0;
    pthread_mutex_unlock(&g_obj_lock);
    return 1;
}

int ka_obj_release_sem(uint32_t h, uint32_t count, uint32_t *previous) {
    ka_obj *o;
    pthread_mutex_lock(&g_obj_lock);
    o = obj_at(h);
    if (o) {
        /* Stage 0 baseline: no maxcount clamp, lPreviousCount written before
         * the add, always success - the old shim's body. */
        if (previous) *previous = (uint32_t)o->count;
        o->count += (int)count;
        o->signaled = o->count > 0;
    } else if (previous) {
        *previous = 0;
    }
    pthread_mutex_unlock(&g_obj_lock);
    return 1;
}

/* Caller holds g_obj_lock. Returns 1 and consumes one signal when the object
 * is ready for this wait. (Stage 0 baseline: a semaphore's count is only
 * bookkeeping here - the wait consumes the signal bit and leaves the count
 * alone; the count-consume fix is the next commit.) */
static int consume_ready(ka_obj *o) {
    if (!o->signaled) return 0;
    /* Mutex ownership is unexercised (Step 0: 0 mutex waits) and stays the
     * behaviour it had: a wait consumes the signal, ReleaseMutex does not
     * restore it. Do not build ownership/recursion here without a call site. */
    if (!o->manual) o->signaled = 0;
    return 1;
}

ka_wait_result ka_obj_wait_ex(uint32_t h, uint32_t ms, ka_wait_sleep_fn sleep,
                              ka_wait_poll_fn poll, void *poll_ctx) {
    ka_wait_result r;
    uint32_t waited = 0;
    r.result = KA_WAIT_TIMEOUT;
    r.polls = 0;
    r.consumed = 0;
    if (!sleep) { r.result = KA_WAIT_FAILED; return r; }
    for (;;) {
        ka_obj *o;
        int found, ready = 0;
        pthread_mutex_lock(&g_obj_lock);
        o = obj_at(h);
        found = o != NULL;
        if (found) ready = consume_ready(o);
        pthread_mutex_unlock(&g_obj_lock);
        if (!found) {
            /* Closed or recycled while we polled. The shim answers 258 for a
             * handle it can no longer resolve, as it did before. */
            r.result = KA_WAIT_FAILED;
            r.polls = waited;
            return r;
        }
        if (ready) {
            r.result = KA_WAIT_OBJECT_0;
            r.consumed = 1;
            r.polls = waited;
            return r;
        }
        /* ms == 0 is a state poll: the check above already ran, so answer
         * WAIT_TIMEOUT without sleeping. */
        if (ms != KA_WAIT_INFINITE && waited >= ms) {
            r.result = KA_WAIT_TIMEOUT;
            r.polls = waited;
            return r;
        }
        sleep(1);
        waited++;
        if (poll) poll(waited, poll_ctx);
    }
}

ka_wait_result ka_obj_wait(uint32_t h, uint32_t ms, ka_wait_sleep_fn sleep) {
    return ka_obj_wait_ex(h, ms, sleep, NULL, NULL);
}

ka_wait_result ka_obj_wait_multi(const uint32_t *handles, uint32_t count,
                                 int wait_all, uint32_t ms,
                                 ka_wait_sleep_fn sleep) {
    ka_wait_result r;
    uint32_t waited = 0;
    r.result = KA_WAIT_TIMEOUT;
    r.polls = 0;
    r.consumed = 0;
    if (!sleep) { r.result = KA_WAIT_FAILED; return r; }
    if (count == 0) {
        ka_last_error_set(KA_ERROR_INVALID_PARAMETER);
        r.result = KA_WAIT_FAILED;
        return r;
    }
    for (;;) {
        uint32_t i;
        int ready = 0;
        pthread_mutex_lock(&g_obj_lock);
        for (i = 0; i < count; i++) {
            ka_obj *o = obj_at(handles[i]);
            if (o && o->signaled) {
                if (!wait_all) {
                    /* Consume only the object that is returned. */
                    consume_ready(o);
                    pthread_mutex_unlock(&g_obj_lock);
                    r.result = KA_WAIT_OBJECT_0 + i;
                    r.consumed = 1;
                    r.polls = waited;
                    return r;
                }
                /* Stage 0 baseline: consumed while counting, so a wait-all
                 * that times out has already eaten the signals it saw. */
                consume_ready(o);
                ready++;
            }
        }
        if (wait_all && ready == (int)count) {
            pthread_mutex_unlock(&g_obj_lock);
            r.result = KA_WAIT_OBJECT_0;
            r.consumed = 1;
            r.polls = waited;
            return r;
        }
        pthread_mutex_unlock(&g_obj_lock);
        if (ms != KA_WAIT_INFINITE && waited >= ms) {
            r.result = KA_WAIT_TIMEOUT;
            r.polls = waited;
            return r;
        }
        sleep(1);
        waited++;
    }
}

/* Integrity check (M3, 2026-11-21). g_obj[9]'s fields at +4..+16 are found
 * overwritten with host-pointer halves during the render burst, which turns a
 * worker's blocking wait into a 730k/s spin and freezes the frame loop. The
 * wait that first notices it belongs to a different thread than the writer, so
 * the ring of recent imports has to be captured as soon as the table goes bad:
 * this runs from every import dispatch, covering the 256 entries in 32
 * imports. Diagnostics only; a read-only probe, so it takes no lock. */
void ka_obj_check(void) {
    static unsigned rot;
    static int bad;
    unsigned k;
    if (bad) return;
    for (k = 0; k < 8; k++) {
        unsigned i = (rot + k) & (KA_OBJ_MAX - 1);
        ka_obj *e = &g_obj[i];
        if (e->used && (e->kind < KA_OBJ_EVENT || e->kind > KA_OBJ_SEM)) {
            bad = 1;
            ka_log("obj: entry %u corrupt: %d %d %d %d %d %d %d (fs=%08X f=0x%08X)", i,
                   e->used, e->kind, e->manual, e->signaled, e->count,
                   e->maxcount, e->owned, g_fs_base, g_cur_func);
            ka_ring_dump("imports just before the object table went bad");
            return;
        }
    }
    rot = (rot + 8) & (KA_OBJ_MAX - 1);
}
