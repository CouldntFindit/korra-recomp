/* wait_objects.h - the Android host's kernel wait objects as a state machine.
 *
 * The shims in kernel32_android.c stay the guest-facing ABI (stdcall argument
 * pops, probes, thread/file fallbacks); everything that is a *decision* about
 * an object's state lives here. Stage 0 keeps the existing 1 ms poll: a waiter
 * re-resolves its handle and checks the object under g_obj_lock once per
 * poll, and the poll is supplied by the caller as a sleep callback. There is
 * no wake list, no condvar and no broadcast - the recorded 77 s frame was a
 * broadcast waking every waiter of an auto-reset event.
 *
 * Why the sleep is a parameter: the shim's sleep is guest_sleep_ms(), which
 * unwinds the full machine-lock recursion before sleeping and re-enters after
 * it. That is only legal on a guest thread. The selftest runs on host pthreads
 * and passes plain nanosleep, which must never be replaced by guest_sleep_ms.
 *
 * Locking: every state change (create, close, set, reset, release, and the
 * consume inside a successful wait) happens under g_obj_lock; the lock is
 * never held across a sleep and never held while taking the machine lock.
 * ka_obj_check() stays a lock-free read-only integrity probe (it runs from
 * every import dispatch and must not serialise the machine).
 */
#ifndef KORRA_WAIT_OBJECTS_H
#define KORRA_WAIT_OBJECTS_H

#include <stdint.h>

/* Handle tokens are slot + 1 (1..256); thread ids start above this range. */
#define KA_OBJ_MAX 256

enum { KA_OBJ_EVENT = 1, KA_OBJ_MUTEX = 2, KA_OBJ_SEM = 3 };

/* Field order is load-bearing: the "wait: raw" probe prints these seven ints
 * and ka_obj_check() validates `kind` in place. Do not reorder. */
typedef struct {
    int used, kind, manual, signaled, count, maxcount, owned;
} ka_obj;

/* WAIT_* return values the guest sees (winbase.h). */
#define KA_WAIT_OBJECT_0 0u
#define KA_WAIT_TIMEOUT  258u
#define KA_WAIT_FAILED   0xFFFFFFFFu
#define KA_WAIT_INFINITE 0xFFFFFFFFu

/* ERROR_TOO_MANY_POSTS / ERROR_INVALID_HANDLE / ERROR_INVALID_PARAMETER. */
#define KA_ERROR_TOO_MANY_POSTS      298u
#define KA_ERROR_INVALID_HANDLE      6u
#define KA_ERROR_INVALID_PARAMETER   87u

typedef void (*ka_wait_sleep_fn)(uint32_t ms);
/* Called once per 1 ms sleep, with the count so far (the shim's "a main
 * thread has been waiting 2000 ms" probe). May be NULL. */
typedef void (*ka_wait_poll_fn)(uint32_t polls, void *ctx);

typedef struct {
    uint32_t result;   /* KA_WAIT_OBJECT_0, KA_WAIT_TIMEOUT, KA_WAIT_FAILED */
    uint32_t polls;    /* 1 ms sleeps performed */
    int consumed;      /* a signal was consumed by this wait */
} ka_wait_result;

uint32_t ka_obj_new(int kind, int manual, int initial);
uint32_t ka_obj_new_sem(int initial, int maxcount);
/* Locked copy of a handle's record: 1 found (out written), 0 not an object. */
int ka_obj_snapshot(uint32_t h, ka_obj *out, int *idx);
int ka_obj_close(uint32_t h);          /* 1 closed, 0 unknown handle */
int ka_obj_set(uint32_t h);            /* SetEvent: 1 if an event was set */
int ka_obj_reset(uint32_t h);          /* ResetEvent: 1 if an event was reset */
/* ReleaseSemaphore. Stage 0 baseline: always succeeds (the extraction keeps
 * the old shim's body); the maxcount clamp lands with the semantics fix. */
int ka_obj_release_sem(uint32_t h, uint32_t count, uint32_t *previous);
ka_wait_result ka_obj_wait(uint32_t h, uint32_t ms, ka_wait_sleep_fn sleep);
ka_wait_result ka_obj_wait_ex(uint32_t h, uint32_t ms, ka_wait_sleep_fn sleep,
                              ka_wait_poll_fn poll, void *poll_ctx);
/* wait_all: return only when every object is signaled, then consume one from
 * each. wait_any: return WAIT_OBJECT_0 + the lowest signaled index and
 * consume only that one. */
ka_wait_result ka_obj_wait_multi(const uint32_t *handles, uint32_t count,
                                 int wait_all, uint32_t ms,
                                 ka_wait_sleep_fn sleep);

/* Integrity probe kept for the import dispatch (spine_android.c). */
void ka_obj_check(void);

#endif
