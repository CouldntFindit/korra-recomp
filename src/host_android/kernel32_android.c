/* kernel32_android.c - KERNEL32 answered on Bionic.
 *
 * Korra imports 121 KERNEL32 functions (the whole list is in
 * docs/ANDROID_BRINGUP.md section 4.4 and in the build log). Every one ends in
 * RET() with its exact stdcall argument count: a wrong count leaves the guest
 * esp shifted and the crash lands in an unrelated function.
 *
 * Handles are 32-bit tokens into host tables, never host pointers. Guest memory
 * is reached through MEM32/ka_host, so nothing here depends on the lift.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unwind.h>
#include <unistd.h>

#include "host_android.h"

/* ------------------------------------------------------ host backtrace ---
 * The guest stack cannot name callers: RECOMP_CALL pushes RECOMP_RETADDR
 * (0xDEAD0000) instead of a real return address, so a frame walk finds only
 * that marker. The HOST stack still has the lifted C function return
 * addresses, and addr2line over the unstripped libkorra.so turns those into
 * sub_XXXXXXXX names. bionic has no backtrace() in <execinfo.h>, so this walks
 * with the unwinder. */
typedef struct { void **pc; int n, max; } ka_bt;
static _Unwind_Reason_Code ka_bt_cb(struct _Unwind_Context *ctx, void *arg) {
    ka_bt *b = (ka_bt *)arg;
    uintptr_t ip = (uintptr_t)_Unwind_GetIP(ctx);
    if (ip && b->n < b->max) b->pc[b->n++] = (void *)ip;
    return b->n < b->max ? _URC_NO_REASON : _URC_END_OF_STACK;
}
static int ka_host_backtrace(void **pc, int max) {
    ka_bt b;
    b.pc = pc; b.n = 0; b.max = max;
    _Unwind_Backtrace(ka_bt_cb, &b);
    return b.n;
}

/* ------------------------------------------------------------- constants */
#define INVALID_HANDLE 0xFFFFFFFFu
#define STD_OUT 0xFFFFF001u
#define STD_ERR 0xFFFFF002u
#define STD_IN  0xFFFFF003u

/* --------------------------------------------------------------- objects */
#define MAX_OBJ 256
enum { OBJ_EVENT = 1, OBJ_MUTEX = 2, OBJ_SEM = 3 };
typedef struct {
    int used, kind, manual, signaled, count, maxcount, owned;
} ka_obj;
static ka_obj g_obj[MAX_OBJ];
static pthread_mutex_t g_obj_lock = PTHREAD_MUTEX_INITIALIZER;

static ka_obj *obj_get(uint32_t h) {
    uint32_t i = h - 1;
    if (h == 0 || i >= MAX_OBJ || !g_obj[i].used) return NULL;
    return &g_obj[i];
}
static uint32_t obj_new(int kind, int manual, int initial) {
    pthread_mutex_lock(&g_obj_lock);
    for (uint32_t i = 0; i < MAX_OBJ; i++) {
        if (!g_obj[i].used) {
            g_obj[i].used = 1; g_obj[i].kind = kind; g_obj[i].manual = manual;
            g_obj[i].signaled = initial; g_obj[i].count = initial;
            g_obj[i].maxcount = 0;
            pthread_mutex_unlock(&g_obj_lock);
            return i + 1;
        }
    }
    pthread_mutex_unlock(&g_obj_lock);
    return 0;
}

/* ------------------------------------------------------ pool slot probe ---
 * M3 deadlock, 2026-11-21. The asset-load pool is two managers of slots, each
 * slot a 0x18-byte block in guest heap reached through a posted/done semaphore
 * pair: the pump sub_00741410 takes the pool's own InterlockedCompareExchange
 * spin lock, stores a job count, releases every slot's posted semaphore and
 * then waits on every slot's done semaphore; the worker sub_007414E0 waits on
 * [+8], calls the job function at [+0x10] with the argument at [+0x14], and
 * releases [+0xc]. On Windows every one of those posts carries a live function
 * and the game reaches the menu. On the device one worker woke with [+0x10]
 * already zero, called it, and then spun forever on a wait handle of 0.
 *
 * These records answer, in one run, which handle is which slot's semaphore and
 * what the block held at the moment of the wake. They are diagnostics only:
 * nothing here changes an answer the guest sees. */
typedef struct { uint32_t h, init, max, esp, ebp; } ka_sem_rec;
#define MAX_SEM_REC 24
static ka_sem_rec g_sem_rec[MAX_SEM_REC];
static int g_sem_n;

static uint32_t sem_note(uint32_t tok, uint32_t init, uint32_t max) {
    if (g_sem_n < MAX_SEM_REC) {
        ka_sem_rec *r = &g_sem_rec[g_sem_n++];
        r->h = tok; r->init = init; r->max = max; r->esp = g_esp; r->ebp = g_ebp;
        ka_log("sem: h=%08X init=%u max=%u esp=%08X ebp=%08X", tok, init, max, g_esp, g_ebp);
    }
    return tok;
}

/* The worker block, read at wake. `mgr` is the pool object at [0] and its byte
 * at +8 carries the shutdown bits sub_007414E0 tests. */
static void slot_dump(uint32_t h, uint32_t param) {
    uint32_t mgr, flags = 0;
    if (param < KA_HEAP_BASE || param >= KA_HEAP_END) return;
    mgr = MEM32(param);
    if (mgr >= KA_HEAP_BASE && mgr < KA_HEAP_END) flags = MEM8(mgr + 8);
    ka_log("slot: h=%08X param=%08X cap=%u post=%08X done=%08X fn=%08X arg=%08X"
           " w18=%08X w1c=%08X mgr=%08X mgrflags=%02X",
           h, param, MEM32(param + 4), MEM32(param + 8), MEM32(param + 0xc),
           MEM32(param + 0x10), MEM32(param + 0x14),
           MEM32(param + 0x18), MEM32(param + 0x1c), mgr, flags);
}

/* ------------------------------------------------------ critical sections */
#define MAX_CS 256
static pthread_mutex_t g_cs[MAX_CS];
static int g_cs_used[MAX_CS];

/* --------------------------------------------------------------- modules */
typedef struct { char name[64]; uint32_t token; } ka_module;
static ka_module g_modules[16];
static int g_module_n;

static uint32_t module_token(const char *dll) {
    for (int i = 0; i < g_module_n; i++)
        if (!strcasecmp(g_modules[i].name, dll)) return g_modules[i].token;
    if (g_module_n < 16) {
        snprintf(g_modules[g_module_n].name, sizeof g_modules[0].name, "%s", dll);
        g_modules[g_module_n].token = 0x6000u + (uint32_t)g_module_n;
        return g_modules[g_module_n++].token;
    }
    return 0;
}

static uint32_t proc_address(const char *dll, const char *fn) {
    uint32_t va;
    (void)dll;
    va = ka_shim_va(fn);
    if (va) return va;
    /* Runtime-resolved names are the only imports the PE scan cannot list. Log
     * the miss and return NULL, which is what Windows does for an export that
     * is not there: the CRT and the engine treat these as "probe failed" and
     * take their older code path. Binding an aborting stub instead would turn
     * an optional probe into a crash (the vista+ probe list above the CRT's
     * locale/threadpool/Ex entry points is all optional). */
    ka_log("getprocaddress: no shim for %s!%s -> NULL", dll ? dll : "?", fn ? fn : "?");
    return 0;
}

/* ------------------------------------------------------------- time/misc */
static uint64_t mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
static void guest_sleep_ms(uint32_t ms) {
    extern int ka_spine_depth(void);
    int depth = 0;
    struct timespec ts;
    ts.tv_sec = ms / 1000; ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    /* Release the machine lock so other guest threads run while we sleep. */
    while (ka_spine_depth() > 0) { mach_leave(); depth++; }
    nanosleep(&ts, NULL);
    while (depth-- > 0) mach_enter();
}

/* --------------------------------------------------------------- the shims */
static void k_CloseHandle(void) {
    uint32_t h = A32(0);
    ka_obj *o = obj_get(h);
    {
        /* Handle reuse is normal on Windows too, so this is not a bug by
         * itself: it is how the run shows WHICH object a value stood for at the
         * moment the pool handed it on. obj_new reuses a freed slot, so a
         * stale handle silently becomes the next object created. */
        static int n;
        if (n < 40) {
            n++;
            ka_log("close: h=%08X %s esp=%08X ebp=%08X fs=%08X", h,
                   o ? (o->kind == OBJ_SEM ? "sem" : o->kind == OBJ_EVENT ? "event" : "mutex")
                     : "not an object",
                   g_esp, g_ebp, g_fs_base);
        }
    }
    if (o) { o->used = 0; RET(1, 1); }
    /* A thread handle: create/openthread ids are real here even though they are
     * not kernel objects, and closing one must not touch the thread table (the
     * engine opens the same worker again to join it). */
    if (ka_thread_exists(h)) RET(1, 1);
    if (ka_file_close(h)) RET(1, 1);
    RET(0, 1);
}

static void k_GetLastError(void) { RET(ka_last_error_get(), 0); }
static void k_SetLastError(void) { ka_last_error_set(A32(0)); RETV(1); }

static void k_GetCurrentProcess(void) { RET(0xFFFFFFFFu, 0); }
static void k_GetCurrentProcessId(void) { RET((uint32_t)getpid(), 0); }
static void k_GetCurrentThread(void) { RET(0xFFFFFFFEu, 0); }
static void k_GetCurrentThreadId(void) {
    extern uint32_t ka_guest_tid(void);
    /* Inside a guest thread this must be the id CreateThread reported as
     * lpThreadId - the engine indexes its thread records by it (sub_00741A90)
     * and dereferences a NULL when the two disagree. Threads the guest did not
     * create (the main thread, host helpers) get a lazy counter. */
    uint32_t gid = ka_guest_tid();
    static __thread uint32_t id;
    if (gid) { RET(gid, 0); }
    if (!id) { static uint32_t next = 100; id = __atomic_add_fetch(&next, 1, __ATOMIC_RELAXED); }
    RET(id, 0);
}

static void k_GetTickCount(void) { RET((uint32_t)(mono_ns() / 1000000ull), 0); }
static void k_QueryPerformanceFrequency(void) {
    uint32_t out = A32(0);
    if (out) MEM32(out) = 10000000u;
    RET(1, 1);
}
static void k_QueryPerformanceCounter(void) {
    uint32_t out = A32(0);
    uint64_t v = mono_ns() / 100ull;
    if (out) { MEM32(out) = (uint32_t)v; MEM32(out + 4) = (uint32_t)(v >> 32); }
    RET(1, 1);
}
static void k_GetSystemTimeAsFileTime(void) {
    uint32_t out = A32(0);
    uint64_t v = (mono_ns() / 100ull) + 116444736000000000ull; /* not wall clock */
    if (out) { MEM32(out) = (uint32_t)v; MEM32(out + 4) = (uint32_t)(v >> 32); }
    RETV(1);
}
static void k_FileTimeToSystemTime(void) {
    uint32_t ft = A32(0), st = A32(1);
    if (ft && st) memset(ka_host(st), 0, 16);
    RET(1, 2);
}
static void k_SystemTimeToTzSpecificLocalTime(void) {
    uint32_t tz = A32(0), utc = A32(1), loc = A32(2);
    (void)tz;
    if (utc && loc && utc != loc) memcpy(ka_host(loc), ka_host(utc), 16);
    RET(1, 3);
}
static void k_GetTimeZoneInformation(void) {
    uint32_t out = A32(0);
    if (out) memset(ka_host(out), 0, 172);
    RET(0, 1);   /* TIME_ZONE_ID_UNKNOWN */
}

static void k_Sleep(void) { guest_sleep_ms(A32(0)); RETV(1); }
static void k_SleepEx(void) { guest_sleep_ms(A32(0)); RET(0, 2); }
static void k_OutputDebugStringW(void) { RETV(1); }
static void k_IsDebuggerPresent(void) { RET(0, 0); }
static void k_IsProcessorFeaturePresent(void) { RET(1, 1); }
static void k_EncodePointer(void) { RET(A32(0), 1); }
static void k_DecodePointer(void) { RET(A32(0), 1); }
static void k_SetUnhandledExceptionFilter(void) { RET(0, 1); }
static void k_UnhandledExceptionFilter(void) { RET(1, 1); }

/* RtlUnwind walks SEH frames calling termination handlers. The lifted code's
 * frames live in guest memory and the engine only reaches this on a C++
 * exception; there is no host walker for them. Log and continue: a thrown
 * exception in normal play is a defect to investigate, not a path to emulate
 * blind. */
static void k_RtlUnwind(void) {
    ka_log("kernel32: RtlUnwind(target=0x%08X ip=0x%08X) from 0x%08X - not emulated",
           A32(0), A32(1), g_cur_func);
    RET(0, 4);
}

static void k_RaiseException(void) {    uint32_t code = A32(0);
    if (code == 0x406D1388u) RETV(4);   /* MSVC thread-naming idiom */
    ka_log("FATAL: guest RaiseException(0x%08X, flags 0x%X, %u args) from 0x%08X",
           code, A32(1), A32(2), g_cur_func);
    abort();
}

static void k_ExitProcess(void) {
    /* The boot ends with an orderly shutdown before the first Present. The
     * engine's framework start (sub_00847BB0 through sub_007657D0) needs
     * eax==1 and otherwise crosses one of its early exits, so dumping the
     * globals it tests names the branch: 9B4390 == FFFFFFFF means step 1's
     * pool was never created (0x34), CA2990 == 0 means the 0x4B early check
     * (its factory sub_0089F040 never ran), and CA2020 == 0 with both set
     * means step 2 (sub_00847FF0) or step 3 (sub_00847CE0) failed. */
    ka_log("state: 9B4390=%08X 9B4388=%08X 9B438C=%08X 9B4394=%08X CA2990=%08X "
           "CA2020=%08X CA1FD8=%08X CA2004=%08X 9B4888=%08X 9B44E8=%08X",
           MEM32(0x9B4390u), MEM32(0x9B4388u), MEM32(0x9B438Cu), MEM32(0x9B4394u),
           MEM32(0xCA2990u), MEM32(0xCA2020u), MEM32(0xCA1FD8u), MEM32(0xCA2004u),
           MEM32(0x9B4888u), MEM32(0x9B44E8u));
    ka_log("state: DB8AA0=%08X DB8AA4=%08X DB8AC8=%08X DB8AE8=%08X DB8AF4=%08X",
           MEM32(0xDB8AA0u), MEM32(0xDB8AA4u), MEM32(0xDB8AC8u), MEM32(0xDB8AE8u),
           MEM32(0xDB8AF4u));
    ka_log("guest ExitProcess(%u)", A32(0));
    fflush(NULL);
    _exit((int)A32(0));
}
static void k_ExitThread(void) {
    int d;
    extern int ka_spine_depth(void);
    while ((d = ka_spine_depth()) > 0) mach_leave();
    pthread_exit(NULL);
}
static void k_TerminateProcess(void) { k_ExitProcess(); }

/* --------------------------------------------------------------- memory */
static void k_GetProcessHeap(void) { RET(1, 0); }
static void k_HeapCreate(void) { RET(0x2001u + (A32(1) ? 0u : 0u), 3); }
static void k_HeapDestroy(void) { RET(1, 1); }
static void k_HeapAlloc(void) {
    uint32_t flags = A32(1), size = A32(2);
    uint32_t p = ka_guest_alloc(size, 16);
    /* The CRT asks for zeroed blocks in places that assume it. */
    if (p && (flags & 8u)) memset(ka_host(p), 0, size);
    if (!p) ka_log("heap: HeapAlloc(%u, 0x%X) -> NULL", size, flags);
    RET(p, 3);
}
static void k_HeapFree(void) { ka_guest_free(A32(2)); RET(1, 3); }
static void k_HeapSize(void) { RET(ka_guest_size(A32(2)), 3); }
static void k_HeapReAlloc(void) {
    uint32_t old = A32(2), size = A32(3);
    uint32_t osz = ka_guest_size(old);
    uint32_t nv = ka_guest_alloc(size, 16);
    if (nv && old && osz) memcpy(ka_host(nv), ka_host(old), osz < size ? osz : size);
    if (nv) ka_guest_free(old);
    RET(nv, 4);
}
static void k_GlobalAlloc(void) { RET(ka_guest_alloc(A32(1), 16), 2); }
static void k_GlobalFree(void) { ka_guest_free(A32(0)); RET(0, 1); }
static void k_GlobalSize(void) { RET(ka_guest_size(A32(0)), 1); }

static void k_VirtualAlloc(void) {
    uint32_t addr = A32(0), size = A32(1), type = A32(2);
    if (addr) {
        uint32_t va = addr;
        if (type & 0x2000u) {                 /* MEM_RESERVE at a hint */
            if (ka_commit(va, size ? size : 0x1000, 1) != 0) RET(0, 4);
        }
        if (ka_commit(va, size ? size : 0x1000, 0) != 0) RET(0, 4);
        RET(va, 4);
    }
    {
        uint32_t va = ka_guest_alloc(size ? size : 0x1000, 0x10000);
        if (!va) RET(0, 4);
        memset(ka_host(va), 0, size ? size : 0x1000);
        RET(va, 4);
    }
}
static void k_VirtualQuery(void) {
    uint32_t addr = A32(0), info = A32(1);
    if (info) {
        memset(ka_host(info), 0, 28);
        MEM32(info + 0) = addr & 0xFFFF0000u;   /* BaseAddress */
        MEM32(info + 12) = 0x10000;             /* RegionSize */
        MEM32(info + 16) = 0x1000;              /* MEM_COMMIT */
        MEM32(info + 20) = 0x04;                /* PAGE_READWRITE */
    }
    RET(28, 3);
}

/* --------------------------------------------------- critical sections */
static void k_InitializeCriticalSection(void) {
    uint32_t p = A32(0);
    for (int i = 0; i < MAX_CS; i++) {
        if (!g_cs_used[i]) {
            pthread_mutexattr_t a;
            pthread_mutexattr_init(&a);
            pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
            pthread_mutex_init(&g_cs[i], &a);
            pthread_mutexattr_destroy(&a);
            g_cs_used[i] = 1;
            if (p) MEM32(p) = (uint32_t)i + 1;
            RETV(1);
        }
    }
    ka_log("FATAL: out of critical sections");
    abort();
}
static pthread_mutex_t *cs_of(uint32_t p) {
    uint32_t i = p ? MEM32(p) : 0;
    if (!i || i > MAX_CS || !g_cs_used[i - 1]) return NULL;
    return &g_cs[i - 1];
}
static void k_EnterCriticalSection(void) {
    pthread_mutex_t *m = cs_of(A32(0));
    if (m) pthread_mutex_lock(m);
    RETV(1);
}
static void k_LeaveCriticalSection(void) {
    pthread_mutex_t *m = cs_of(A32(0));
    if (m) pthread_mutex_unlock(m);
    RETV(1);
}
static void k_DeleteCriticalSection(void) { RETV(1); }
/* Shared body: k_InitCSAndSpin and InitializeCriticalSectionEx differ only in
 * the flags argument the guest also pushes, so each wrapper does its own RET. */
static int cs_init_at(uint32_t p) {
    for (int i = 0; i < MAX_CS; i++) {
        if (!g_cs_used[i]) {
            pthread_mutexattr_t a;
            pthread_mutexattr_init(&a);
            pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
            pthread_mutex_init(&g_cs[i], &a);
            pthread_mutexattr_destroy(&a);
            g_cs_used[i] = 1;
            if (p) MEM32(p) = (uint32_t)i + 1;
            return 1;
        }
    }
    return 0;
}
static void k_InitCSAndSpin(void) { RET(cs_init_at(A32(0)), 2); }
static void k_SetCSSpin(void) { RET(0, 2); }

/* ---------------------------------------------------------- interlocked */
static void k_InterlockedIncrement(void) {
    uint32_t p = A32(0);
    if (p) RET(__atomic_add_fetch((uint32_t *)ka_host(p), 1u, __ATOMIC_SEQ_CST), 1);
    RET(0, 1);
}
static void k_InterlockedDecrement(void) {
    uint32_t p = A32(0);
    if (p) RET(__atomic_sub_fetch((uint32_t *)ka_host(p), 1u, __ATOMIC_SEQ_CST), 1);
    RET(0, 1);
}
static void k_InterlockedExchange(void) {
    uint32_t p = A32(0), v = A32(1);
    if (p) {
        uint32_t old = __atomic_exchange_n((uint32_t *)ka_host(p), v, __ATOMIC_SEQ_CST);
        /* Spin diagnosis: the engine's TestAndSet loops on this. Log the first
         * few hundred and then sparsely, so a stuck spin names its address. */
        static int n;
        /* g_backedges proves whether the spin loop's back edge is instrumented:
         * it is the runtime's own global, incremented by RECOMP_BACKEDGE, and
         * every 0x10000 of them calls recomp_yield_hook. */
        if (n < 200 || (n % 100000) == 0)
            ka_log("[spin] InterlockedExchange(%08X, %u) old=%u n=%d from=0x%08X back=%u",
                   p, v, old, n, g_cur_func, g_backedges);
        n++;
        /* One guest backtrace from inside the sustained spin: 0x008D116E is
         * only the InterlockedExchange forwarder, so g_cur_func names the
         * thunk. Walking ebp names the real spinner and its callers. */
        {
            static int bt;
            if (p == 0x2139780Cu && old == 1 && ++bt == 50) {
                uint32_t fp;
                ka_log("spin: backtrace for %08X (fs=%08X ebp=%08X esp=%08X):", p, g_fs_base, g_ebp, g_esp);
                ka_log("spin:   lock words: %08X %08X %08X %08X %08X",
                       MEM32(p + 4), MEM32(p + 8), MEM32(p + 0xC), MEM32(p + 0x10), MEM32(p + 0x14));
                /* Guest return addresses are RECOMP_RETADDR, so walk the HOST
                 * stack instead and resolve it with addr2line: that names the
                 * lifted C function that is spinning. */
                {
                    void *btv[24];
                    Dl_info di;
                    uintptr_t base = 0;
                    int bn, i;
                    bn = ka_host_backtrace(btv, 24);
                    if (dladdr((void *)&ka_machine_yield, &di)) base = (uintptr_t)di.dli_fbase;
                    ka_log("spin: host frames=%d base=%08lX", bn, (unsigned long)base);
                    for (i = 0; i < bn; i++)
                        ka_log("spin:   off=0x%lX", (unsigned long)((uintptr_t)btv[i] - base));
                }
                for (fp = g_ebp; fp >= 0x1000u; ) {
                    uint32_t next = MEM32(fp), ret = MEM32(fp + 4);
                    ka_log("spin:   ebp=%08X ret=%08X", fp, ret);
                    if (next <= fp) break;
                    fp = next;
                }
            }
        }
        RET(old, 2);
    }
    RET(0, 2);
}
static void k_InterlockedExchangeAdd(void) {
    uint32_t p = A32(0), v = A32(1);
    if (p) RET(__atomic_fetch_add((uint32_t *)ka_host(p), v, __ATOMIC_SEQ_CST), 2);
    RET(0, 2);
}
static void k_InterlockedCompareExchange(void) {
    uint32_t p = A32(0), x = A32(1), c = A32(2);
    if (p) {
        /* The API returns the PREVIOUS value, not whether it swapped: the CRT's
         * once-init patterns branch on it, so a boolean here silently sends them
         * down the "already initialised" path. __atomic_* leaves the comparison
         * value in `c` on failure and overwrites it with the old value on
         * success, which is exactly the documented result both ways. */
        uint32_t prev = c;
        __atomic_compare_exchange_n((uint32_t *)ka_host(p), &prev, x, 0,
                                    __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
        /* Spin diagnosis: a guest spinlock built on CAS loops here. Log the
         * first calls and then sparsely so a stuck spin names its address. */
        static int n;
        if (n < 200 || (n % 100000) == 0)
            ka_log("[cmp] InterlockedCompareExchange(%08X, %u, %u) old=%u n=%d from=0x%08X",
                   p, x, c, prev, n, g_cur_func);
        n++;
        RET(prev, 3);
    }
    RET(0, 3);
}

/* ------------------------------------------------------------------ TLS */
static void k_TlsAlloc(void) { RET(ka_tls_alloc(), 0); }
static void k_TlsFree(void) { ka_tls_free(A32(0)); RET(1, 1); }
static void k_TlsGetValue(void) { RET(ka_tls_get(A32(0)), 1); }
static void k_TlsSetValue(void) { ka_tls_set(A32(0), A32(1)); RET(1, 2); }

/* Fiber-local storage. The engine gets here through GetProcAddress, not the
 * import table (it probes for Fibers). This recompilation runs the game on one
 * guest thread per host thread and does not convert threads to fibers, so a
 * Fls slot is just another index in the same per-thread pool; the pool
 * allocator keeps the two index spaces disjoint. */
static void k_FlsAlloc(void) { RET(ka_tls_alloc(), 1); }
static void k_FlsFree(void) { ka_tls_free(A32(0)); RET(1, 1); }
static void k_FlsGetValue(void) { RET(ka_tls_get(A32(0)), 1); }
static void k_FlsSetValue(void) { ka_tls_set(A32(0), A32(1)); RET(1, 2); }

/* --------------------------------------------------------------- files */
static void k_CreateFileA(void) {
    uint32_t name = A32(0);
    uint32_t disp = A32(4);
    uint32_t h;
    if (!name) RET(INVALID_HANDLE, 7);
    h = ka_file_open((const char *)ka_host(name));
    if (!h) {
        ka_last_error_set(2);               /* ERROR_FILE_NOT_FOUND */
        RET(INVALID_HANDLE, 7);
    }
    (void)disp;
    RET(h, 7);
}
static void wstr_to_ascii(const uint16_t *w, char *out, size_t cap) {
    size_t i = 0;
    while (w && w[i] && i + 1 < cap) { out[i] = (char)(w[i] & 0x7F); i++; }
    out[i] = 0;
}
static void k_CreateFileW(void) {
    char name[512];
    wstr_to_ascii(AWSTR(0), name, sizeof name);
    {
        uint32_t h = ka_file_open(name);
        if (!h) { ka_last_error_set(2); RET(INVALID_HANDLE, 7); }
        RET(h, 7);
    }
}
static void k_ReadFile(void) {
    uint32_t h = A32(0), buf = A32(1), n = A32(2), got = A32(3);
    uint32_t g = 0;
    if (h == STD_IN) RET(0, 5);
    if (!ka_file_read(h, ka_host(buf), n, &g)) { ka_last_error_set(6); RET(0, 5); }
    if (got) MEM32(got) = g;
    RET(1, 5);
}
static void k_WriteFile(void) {
    uint32_t h = A32(0), buf = A32(1), n = A32(2), put = A32(3);
    uint32_t w = 0;
    if (h == STD_OUT || h == STD_ERR) {
        FILE *f = h == STD_OUT ? stdout : stderr;
        if (buf && n) fwrite(ka_host(buf), 1, n, f);
        fflush(f);
        if (put) MEM32(put) = n;
        RET(1, 5);
    }
    if (!ka_file_write(h, ka_host(buf), n, &w)) { ka_last_error_set(6); RET(0, 5); }
    if (put) MEM32(put) = w;
    RET(1, 5);
}
static void k_GetFileSize(void) {
    uint32_t h = A32(0), hi = A32(1);
    uint64_t s = ka_file_size(h);
    if (hi) MEM32(hi) = (uint32_t)(s >> 32);
    RET((uint32_t)s, 2);
}
static void k_GetFileType(void) {
    uint32_t h = A32(0);
    if (h == STD_OUT || h == STD_ERR || h == STD_IN) RET(2, 1);
    RET(1, 1);
}
static void k_FlushFileBuffers(void) { RET(1, 1); }
static void k_SetEndOfFile(void) { RET(1, 1); }
static void k_DeleteFileA(void) {
    char p[1024];
    ka_fs_resolve(ASTR(0), p, sizeof p);
    RET(unlink(p) == 0 ? 1 : 0, 1);
}
static void k_MoveFileA(void) {
    char a[1024], b[1024];
    ka_fs_resolve(ASTR(0), a, sizeof a);
    ka_fs_resolve(ASTR(1), b, sizeof b);
    RET(rename(a, b) == 0 ? 1 : 0, 2);
}
static void k_CreateDirectoryA(void) {
    char p[1024];
    ka_fs_resolve(ASTR(0), p, sizeof p);
    RET(mkdir(p, 0755) == 0 || errno == EEXIST, 2);
}
static void k_SetCurrentDirectoryA(void) {
    extern void ka_fs_set_cwd(const char *);
    ka_log("fs: SetCurrentDirectoryA(%s)", ASTR(0));
    ka_fs_set_cwd(ASTR(0));
    RET(1, 1);
}
static void k_GetFileAttributesA(void) {
    char p[1024];
    struct stat st;
    ka_fs_resolve(ASTR(0), p, sizeof p);
    if (stat(p, &st) != 0) RET(INVALID_HANDLE, 1);
    RET(S_ISDIR(st.st_mode) ? 0x10u : 0x20u, 1);
}
static void k_SetFilePointer(void) {
    uint32_t h = A32(0), lo = A32(1), hip = A32(2), method = A32(3);
    int64_t delta = (int64_t)lo;
    int ok = 0;
    uint32_t r;
    if (hip) delta |= ((int64_t)(int32_t)MEM32(hip)) << 32;
    r = ka_file_seek(h, delta, (int)method, &ok);
    if (hip) MEM32(hip) = ok ? 0 : 0xFFFFFFFFu;
    RET(r, 4);
}
static void k_SetFilePointerEx(void) {
    uint32_t h = A32(0), lo = A32(1), hi = A32(2), out = A32(3), method = A32(4);
    int64_t delta = (int64_t)(uint64_t)((uint64_t)lo | ((uint64_t)hi << 32));
    int ok = 0;
    uint32_t r = ka_file_seek(h, delta, (int)method, &ok);
    if (out) { MEM32(out) = r; MEM32(out + 4) = 0; }
    RET(ok ? 1 : 0, 5);
}
static void k_GetStdHandle(void) {
    uint32_t which = A32(0);
    if (which == 0xFFFFFFF5u) RET(STD_OUT, 1);
    if (which == 0xFFFFFFF4u) RET(STD_ERR, 1);
    if (which == 0xFFFFFFF6u) RET(STD_IN, 1);
    RET(0, 1);
}
static void k_SetStdHandle(void) { RET(1, 2); }

/* WIN32_FIND_DATAA is 320 bytes; only attributes/size/name are filled. */
/* ------------------------------------------------------- find first/next */
/* The engine's rofs mounts GameData by enumerating it (Hw::rofs::fst builds a
 * file-size table); FindNextFileA returning FALSE made that table one entry
 * long and the engine aborted with
 *   "HWライブラリエラー: [Hw::rofs::fst] ファイルサイズテーブルの作成に失敗しました".
 * So the enumeration has to be a real one: wildcard match, real attributes and
 * a directory stream that survives between FindFirstFileA and FindNextFileA. */
#define FIND_MAX 32
/* Find handles are their own namespace as well: small ints 1..31 used to share
 * the object-handle and file-token space (see KA_FILE_TOKEN_BASE above). */
#define FIND_TOKEN_BASE 0x02000000u
typedef struct {
    DIR *d;
    char mask[260];
    char dir[900];
} ka_find;
static ka_find g_finds[FIND_MAX];
static pthread_mutex_t g_find_lock = PTHREAD_MUTEX_INITIALIZER;

static int wild_match(const char *mask, const char *name) {
    const char *m = mask, *n = name, *star = NULL, *retry = NULL;
    while (*n) {
        if (*m == '*' && !star) { star = ++m; retry = n; }
        else if (*m == '?' || *m == *n) { if (*m) m++, n++; else break; }
        else if (star) { m = star; n = ++retry; }
        else return 0;
    }
    while (*m == '*') m++;
    return *m == 0;
}

static void fill_find_data(uint32_t out, const char *dir, const char *name) {
    struct stat st;
    char full[1100];
    int is_dir = 0;
    memset(ka_host(out), 0, 320);
    snprintf(full, sizeof full, "%s/%s", dir, name);
    if (stat(full, &st) == 0) is_dir = S_ISDIR(st.st_mode);
    MEM32(out + 0) = is_dir ? 0x10u : 0x80u;
    if (stat(full, &st) == 0) {
        MEM32(out + 28) = (uint32_t)((uint64_t)st.st_size >> 32);
        MEM32(out + 32) = (uint32_t)((uint64_t)st.st_size & 0xFFFFFFFFu);
    }
    strncpy((char *)ka_host(out + 44), name, 259);
}

/* Returns a 1-based index into g_finds, or 0. */
static uint32_t find_next_matching(uint32_t slot, uint32_t out) {
    ka_find *f = &g_finds[slot - 1];
    struct dirent *e;
    if (!f->d) return 0;
    while ((e = readdir(f->d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (!wild_match(f->mask, e->d_name)) continue;
        if (out) fill_find_data(out, f->dir, e->d_name);
        return slot;
    }
    return 0;
}

static uint32_t find_first_impl(const char *pat, uint32_t out) {
    char path[1024];
    char *slash;
    uint32_t slot;
    if (!pat || !*pat) return INVALID_HANDLE;
    ka_fs_resolve(pat, path, sizeof path);
    slash = strrchr(path, '/');
    if (slash) {
        snprintf(g_finds[FIND_MAX - 1].mask, sizeof g_finds[0].mask, "%s", slash + 1);
        *slash = 0;
    } else {
        snprintf(g_finds[FIND_MAX - 1].mask, sizeof g_finds[0].mask, "%s", path);
        path[0] = '.';
        path[1] = 0;
    }
    snprintf(g_finds[FIND_MAX - 1].dir, sizeof g_finds[0].dir, "%s", path);
    g_finds[FIND_MAX - 1].d = opendir(path);
    if (!g_finds[FIND_MAX - 1].d) {
        ka_log("fs: FindFirstFileA(%s) -> opendir(%s) failed: %s", pat, path, strerror(errno));
        return INVALID_HANDLE;
    }
    ka_log("fs: FindFirstFileA(%s) -> %s mask=%s", pat, path, g_finds[FIND_MAX - 1].mask);
    pthread_mutex_lock(&g_find_lock);
    for (slot = 1; slot <= FIND_MAX - 1; slot++) {
        if (!g_finds[slot - 1].d) {
            g_finds[slot - 1] = g_finds[FIND_MAX - 1];
            g_finds[FIND_MAX - 1].d = NULL;
            break;
        }
    }
    pthread_mutex_unlock(&g_find_lock);
    if (slot > FIND_MAX - 1) {
        closedir(g_finds[FIND_MAX - 1].d);
        g_finds[FIND_MAX - 1].d = NULL;
        return INVALID_HANDLE;
    }
    if (!find_next_matching(slot, out)) {
        closedir(g_finds[slot - 1].d);
        g_finds[slot - 1].d = NULL;
        return INVALID_HANDLE;
    }
    return FIND_TOKEN_BASE + slot;
}

static void k_FindFirstFileA(void) { RET(find_first_impl(ASTR(0), A32(1)), 2); }
/* (name, infoLevel, data, searchOp, filter, flags) */
static void k_FindFirstFileExA(void) { RET(find_first_impl(ASTR(0), A32(2)), 6); }

static void k_FindNextFileA(void) {
    uint32_t h = A32(0), out = A32(1), slot;
    if (h < FIND_TOKEN_BASE) RET(0, 2);
    slot = h - FIND_TOKEN_BASE;
    if (slot == 0 || slot > FIND_MAX - 1) RET(0, 2);
    if (!find_next_matching(slot, out)) {
        closedir(g_finds[slot - 1].d);
        g_finds[slot - 1].d = NULL;
        ka_last_error_set(18);         /* ERROR_NO_MORE_FILES */
        RET(0, 2);
    }
    RET(1, 2);
}
static void k_FindClose(void) {
    uint32_t h = A32(0), slot;
    if (h < FIND_TOKEN_BASE) RET(0, 1);
    slot = h - FIND_TOKEN_BASE;
    if (slot >= 1 && slot <= FIND_MAX - 1 && g_finds[slot - 1].d) {
        closedir(g_finds[slot - 1].d);
        g_finds[slot - 1].d = NULL;
    }
    RET(1, 1);
}

/* ------------------------------------------------------------ modules */
static void k_GetModuleFileNameA(void) {
    uint32_t buf = A32(1), n = A32(2);
    extern const char *ka_fs_guest_exe(void);
    const char *p = ka_fs_guest_exe();
    size_t len = strlen(p);
    if (!buf || !n) RET(0, 3);
    if (len >= n) len = n - 1;
    memcpy(ka_host(buf), p, len);
    ((char *)ka_host(buf))[len] = 0;
    RET((uint32_t)len, 3);
}
static void k_GetModuleFileNameW(void) {
    uint32_t buf = A32(1), n = A32(2);
    extern const char *ka_fs_guest_exe(void);
    const char *p = ka_fs_guest_exe();
    uint32_t i;
    if (!buf || !n) RET(0, 3);
    for (i = 0; p[i] && i + 1 < n; i++) ((uint16_t *)ka_host(buf))[i] = (uint16_t)(uint8_t)p[i];
    ((uint16_t *)ka_host(buf))[i] = 0;
    RET(i, 3);
}
static void k_GetModuleHandleA(void) {
    uint32_t n = A32(0);
    if (!n) RET(KA_IMAGE_BASE, 1);
    RET(module_token((const char *)ka_host(n)), 1);
}
static void k_GetModuleHandleW(void) {
    char name[128];
    wstr_to_ascii(AWSTR(0), name, sizeof name);
    if (!name[0]) RET(KA_IMAGE_BASE, 1);
    RET(module_token(name), 1);
}
static void k_GetModuleHandleExW(void) {
    uint32_t namep = A32(1), outp = A32(2);
    char name[128];
    wstr_to_ascii(AWSTR(1), name, sizeof name);
    if (outp) MEM32(outp) = name[0] ? module_token(name) : KA_IMAGE_BASE;
    RET(1, 3);
}
static void k_LoadLibraryW(void) {
    char name[128];
    wstr_to_ascii(AWSTR(0), name, sizeof name);
    RET(module_token(name), 1);
}
static void k_LoadLibraryExW(void) {
    char name[128];
    wstr_to_ascii(AWSTR(0), name, sizeof name);
    RET(module_token(name), 3);
}
static void k_FreeLibrary(void) { RET(1, 1); }
static void k_GetProcAddress(void) {
    uint32_t mod = A32(0), namep = A32(1);
    const char *fn = (const char *)ka_host(namep);
    char dll[32] = "?";
    for (int i = 0; i < g_module_n; i++)
        if (g_modules[i].token == mod) snprintf(dll, sizeof dll, "%s", g_modules[i].name);
    RET(proc_address(dll, fn), 2);
}

/* ------------------------------------------------------------ threads */
static void k_CreateThread(void) {
    uint32_t va = A32(2), param = A32(3);
    uint32_t flags = A32(4), id_out = A32(5);
    /* CREATE_SUSPENDED (0x4) is load-bearing: the engine's spawner fills the
     * worker's parameter block after this returns. lpThreadId shares the
     * handle's number, which is what the guest compares and prints. */
    uint32_t tok = ka_thread_create(va, param, (flags & 0x4u) != 0);
    if (!tok) { ka_last_error_set(8); RET(INVALID_HANDLE, 6); }
    if (id_out) MEM32(id_out) = tok;
    RET(tok, 6);
}
uint32_t ka_thread_wait(uint32_t token, uint32_t timeout_ms);
static void k_WaitForSingleObject(void) {
    uint32_t h = A32(0), ms = A32(1);
    ka_obj *o = obj_get(h);
    /* Worker waits: name the handle and its state. The five CRI data-line
     * threads end in this call and never move again, so either nothing ever
     * signals the object or it is not an object at all. */
    {
        static int n;
        if (n < 120) {
            n++;
            int ismain = (g_fs_base && g_fs_base == ka_spine_main_fs());
            if (o)
                ka_log("wait: h=%08X kind=%u manual=%u signaled=%u ms=%u main=%d fs=%08X",
                       h, o->kind, o->manual, o->signaled, ms, ismain, g_fs_base);
            else
                ka_log("wait: h=%08X notobj ms=%u main=%d fs=%08X",
                       h, ms, ismain, g_fs_base);
        }
    }
    if (o) {
        uint32_t waited = 0;
        int ismain = (g_fs_base && g_fs_base == ka_spine_main_fs());
        while (!o->signaled && (ms == 0xFFFFFFFFu || waited < ms)) {
            guest_sleep_ms(1);
            waited++;
            if (ms == 0) break;
            /* A main-thread wait is not logged above (it is the common case),
             * but a boot that goes silent ends in exactly one of two places: a
             * lifted loop that calls no import, or this call. 2 s is far longer
             * than any legitimate wait in the load path. */
            if (ismain && waited == 2000) {
                static int nm;
                if (nm < 10) {
                    nm++;
                    ka_log("wait: MAIN h=%08X kind=%u manual=%u still waiting after %u ms fs=%08X",
                           h, o->kind, o->manual, waited, g_fs_base);
                }
            }
        }
        if (!o->signaled) RET(258u, 2);   /* WAIT_TIMEOUT */
        if (!o->manual) o->signaled = 0;
        if (waited) {
            static int n2;
            if (n2 < 40) {
                n2++;
                ka_log("wait: h=%08X woke after %u polls fs=%08X", h, waited, g_fs_base);
            }
        }
        /* What the slot held when its worker was let through. This cannot ride
         * on the 40-wake budget above: the file pool's early wakes spend it long
         * before the asset pool's critical one. Dump when the block names a
         * semaphore this host created, and always when its job function is
         * zero - that is the device bug itself. */
        {
            uint32_t sp = ka_thread_self_param();
            if (sp >= KA_HEAP_BASE && sp < KA_HEAP_END) {
                uint32_t post = MEM32(sp + 8), fn = MEM32(sp + 0x10);
                int named = (fn == 0);
                for (int i = 0; !named && i < g_sem_n; i++)
                    if (g_sem_rec[i].h == post) named = 1;
                if (named) {
                    static int n3;
                    if (n3 < 400) { n3++; slot_dump(h, sp); }
                }
            }
        }
        RET(0, 2);
    }
    /* A handle of 0 is not an object at all: obj_get rejects it and no thread
     * token can equal it, so the call below would return WAIT_OBJECT_0 in zero
     * time and the caller loops as fast as it can. That is the terminal state
     * of the M3 device run (worker token 265, after calling a NULL job
     * function). The wait can never succeed here, so a 1 ms sleep cannot hide
     * a real wake - it only keeps a 22 s device log small enough to read. */
    if (h == 0) {
        static int n0;
        if (n0 < 20) { n0++; ka_log("wait: h=0 is not a handle, throttling fs=%08X", g_fs_base); }
        guest_sleep_ms(1);
        RET(0, 2);
    }
    RET(ka_thread_wait(h, ms), 2);
}
static void k_WaitForMultipleObjectsEx(void) {
    uint32_t count = A32(0), arr = A32(1), waitAll = A32(2), ms = A32(3);
    uint32_t waited = 0;
    for (;;) {
        int ready = 0;
        for (uint32_t i = 0; i < count; i++) {
            ka_obj *o = obj_get(MEM32(arr + i * 4));
            if (o && o->signaled) {
                if (!o->manual) o->signaled = 0;
                if (!waitAll) RET(i, 5);
                ready++;
            }
        }
        if (waitAll && ready == count) RET(0, 5);
        if (ms != 0xFFFFFFFFu && waited >= ms) RET(258, 5);
        guest_sleep_ms(1);
        waited++;
    }
}
static void k_ResumeThread(void) {
    uint32_t prev = ka_thread_resume(A32(0));
    if (prev == 0xFFFFFFFFu) { ka_last_error_set(6); RET(0xFFFFFFFFu, 1); }
    RET(prev, 1);
}
static void k_ExitCodeThread(void) {
    uint32_t h = A32(0), out = A32(1);
    extern uint32_t ka_thread_exit_code(uint32_t);
    if (out) MEM32(out) = ka_thread_exit_code(h);
    RET(1, 2);
}
/* OpenThread must return a handle that names the thread, because the engine's
 * pool teardown (sub_00741320) releases each pump's semaphore and then waits on
 * this handle before closing the semaphores. A stub value made that join a
 * no-op: the destructor closed the semaphores and cleared the pool's stop bit
 * while five pumps were still parked, so each woke into [pool+8]==0, called the
 * zeroed job pointer (ICALL 0x00000000 from 0x007414E0, 3.6M repeats) and spun.
 * Returning the thread's own id lets WaitForSingleObject reach ka_thread_wait,
 * which unwinds the machine lock and joins for real. */
static void k_OpenThread(void) {
    uint32_t id = A32(2);
    static int n;
    if (n < 40) {
        n++;
        ka_log("openthread: tid=%u access=%08X -> %s esp=%08X ebp=%08X fs=%08X",
               id, A32(1), ka_thread_exists(id) ? "thread" : "invalid",
               g_esp, g_ebp, g_fs_base);
    }
    if (!ka_thread_exists(id)) { ka_last_error_set(87); RET(0, 3); }  /* ERROR_INVALID_PARAMETER */
    RET(id, 3);
}
static void k_SetThreadPriority(void) { RET(1, 2); }
static void k_GetThreadPriority(void) { RET(0, 1); }
static void k_SetThreadAffinityMask(void) { RET(1, 2); }
static void k_GetProcessAffinityMask(void) {
    if (A32(1)) MEM32(A32(1)) = 0xFF;
    if (A32(2)) MEM32(A32(2)) = 0xFF;
    RET(1, 3);
}

/* -------------------------------------------------------------- events */
static void k_CreateEventA(void) {
    RET(obj_new(OBJ_EVENT, A32(1) != 0, A32(2) != 0), 4);
}
static void k_CreateEventW(void) {
    RET(obj_new(OBJ_EVENT, A32(1) != 0, A32(2) != 0), 4);
}
static void k_SetEvent(void) {
    ka_obj *o = obj_get(A32(0));
    {
        static int n;
        if (n < 60) {
            n++;
            ka_log("setevent: h=%08X %s fs=%08X", A32(0),
                   o ? (o->kind == OBJ_EVENT ? "event" : "other") : "not an object", g_fs_base);
        }
    }
    if (o) o->signaled = 1;
    RET(1, 1);
}
static void k_ResetEvent(void) {
    ka_obj *o = obj_get(A32(0));
    if (o) o->signaled = 0;
    RET(1, 1);
}
static void k_CreateMutexA(void) { RET(obj_new(OBJ_MUTEX, 0, A32(1) ? 0 : 1), 3); }
static void k_ReleaseMutex(void) { RET(1, 1); }
static void k_CreateSemaphoreA(void) {
    uint32_t tok = obj_new(OBJ_SEM, 0, 0);
    ka_obj *o = obj_get(tok);
    if (o) { o->signaled = A32(1) != 0; o->count = (int)A32(1); o->maxcount = (int)A32(2); }
    sem_note(tok, A32(1), A32(2));
    RET(tok, 4);
}

/* Vista+ forms the CRT resolves with GetProcAddress. Same objects, extra
 * flags/dacl arguments the engine never varies; answering them keeps the CRT
 * on the path this host already implements instead of its old fallback. */
static void k_InitializeCriticalSectionEx(void) { RET(cs_init_at(A32(0)), 3); }
static void k_CreateSemaphoreExW(void) {
    uint32_t tok = obj_new(OBJ_SEM, 0, 0);
    ka_obj *o = obj_get(tok);
    if (o) { o->signaled = A32(1) != 0; o->count = (int)A32(1); o->maxcount = (int)A32(2); }
    RET(tok, 6);
}
static void k_SetThreadStackGuarantee(void) { RET(1, 1); }
static void k_FlushProcessWriteBuffers(void) { RETV(0); }
static void k_GetCurrentProcessorNumber(void) { RET(0, 0); }
static void k_ReleaseSemaphore(void) {
    ka_obj *o = obj_get(A32(0));
    {
        static int n;
        if (n < 60) {
            n++;
            ka_log("semrel: h=%08X n=%u esp=%08X ebp=%08X fs=%08X",
                   A32(0), A32(1), g_esp, g_ebp, g_fs_base);
        }
    }
    if (A32(2)) MEM32(A32(2)) = o ? (uint32_t)o->count : 0;
    if (o) { o->count += (int)A32(1); o->signaled = o->count > 0; }
    RET(1, 3);
}

/* ------------------------------------------------------------ charsets */
static void k_GetACP(void) { RET(1252, 0); }
static void k_GetOEMCP(void) { RET(437, 0); }
static void k_GetConsoleCP(void) { RET(1252, 0); }
static void k_GetConsoleMode(void) { RET(0, 2); }
static void k_IsValidCodePage(void) { RET(1, 1); }
static void k_GetCPInfo(void) {
    uint32_t out = A32(1);
    if (out) {
        /* CPINFO: MaxCharSize is a BYTE at +0, DefaultChar a BYTE at +4.
         * Reporting 2 for a single-byte code page made the CRT's DBCS path
         * treat every >= 0x80 byte as a lead byte. */
        memset(ka_host(out), 0, 20);
        MEM8(out + 0) = 1;           /* MaxCharSize */
        MEM8(out + 4) = '?';         /* DefaultChar */
    }
    RET(1, 2);
}
/* Both conversions count exactly what the caller asked for. With an explicit
 * length Windows converts that many units INCLUDING embedded NULs, and a
 * negative length converts up to and including the terminator; a size query
 * therefore returns at least 1 for an empty string. Stopping at the NUL made
 * the CRT's _setenvp fail: it asks for one unit (the empty environment's
 * double-NUL block), the query answered 0, WideCharToMultiByte then failed on
 * a zero-byte buffer, the env pointer became NULL, and __setenvp returned -1 -
 * which _amsg_exit reports as R6009 "not enough space for environment"
 * (sub_00782D50 -> sub_00782B48 -> sub_0077E425 in the binary). */
static void k_MultiByteToWideChar(void) {
    const char *src = ASTR(2);
    int cb = (int)A32(3);
    uint32_t dst = A32(4);
    int cch = (int)A32(5);
    int n, i;
    if (!src) RET(0, 6);
    if (cb < 0) { for (n = 0; src[n]; n++) {} n++; }
    else n = cb;
    if (!dst || cch == 0) RET((uint32_t)n, 6);      /* size query */
    if (cch < n) RET(0, 6);
    for (i = 0; i < n; i++) ((uint16_t *)ka_host(dst))[i] = (uint16_t)(uint8_t)src[i];
    RET((uint32_t)n, 6);
}
/* CP1252 is Latin-1 over 0xA0-0xFF; the 0x80-0x9F block has its own glyphs and
 * is left as the byte value rather than mangled (nothing in this game's UI
 * uses it, and '?' would lose information on a path we can inspect). */
static char cp1252_from_w(uint16_t c) {
    if (c < 0x80) return (char)c;
    if (c >= 0xA0 && c <= 0xFF) return (char)c;
    if (c >= 0x80 && c <= 0x9F) return (char)c;
    return '?';
}
static void k_WideCharToMultiByte(void) {
    const uint16_t *src = AWSTR(2);
    int cch = (int)A32(3);
    uint32_t dst = A32(4);
    int cb = (int)A32(5);
    int n, i;
    if (!src) RET(0, 8);
    if (cch < 0) { for (n = 0; src[n]; n++) {} n++; }
    else n = cch;
    if (!dst || cb == 0) RET((uint32_t)n, 8);       /* size query */
    if (cb < n) RET(0, 8);
    for (i = 0; i < n; i++) ((char *)ka_host(dst))[i] = cp1252_from_w(src[i]);
    RET((uint32_t)n, 8);
}
/* (LCID, flags, src, cchSrc, dst, cchDest): src is index 2, not 3. */
static void k_LCMapStringW(void) {
    uint32_t src = A32(2), dst = A32(4);
    int cch = (int)A32(3);
    int cch_dst = (int)A32(5);
    int i;
    if (!src) RET(0, 6);
    if (cch < 0) { for (cch = 0; AWSTR(2)[cch]; cch++) {} }
    if (dst && cch_dst != 0 && cch > cch_dst) RET(0, 6);
    for (i = 0; i < cch; i++) {
        uint16_t c = AWSTR(2)[i];
        if (A32(1) & 0x100u && c >= 'a' && c <= 'z') c = (uint16_t)(c - 32);  /* LCMAP_UPPERCASE */
        if (A32(1) & 0x200u && c >= 'A' && c <= 'Z') c = (uint16_t)(c + 32);  /* LCMAP_LOWERCASE */
        if (dst) ((uint16_t *)ka_host(dst))[i] = c;
    }
    RET((uint32_t)i, 6);
}
/* (LCID, flags, s1, cch1, s2, cch2): cch1 is index 3 and s2 index 4. */
static void k_CompareStringW(void) {
    const uint16_t *a = AWSTR(2), *b = AWSTR(4);
    int n1 = (int)A32(3), n2 = (int)A32(5);
    int i;
    if (!a || !b) RET(0, 6);
    if (n1 < 0) { for (n1 = 0; a[n1]; n1++) {} }
    if (n2 < 0) { for (n2 = 0; b[n2]; n2++) {} }
    for (i = 0; i < n1 && i < n2; i++) {
        if (a[i] != b[i]) RET(a[i] < b[i] ? 1u : 3u, 6);
    }
    RET(n1 == n2 ? 2u : (n1 < n2 ? 1u : 3u), 6);
}
/* GetStringTypeW(DWORD type, LPCWSTR src, int cch, LPWORD out): four
 * arguments. (It used to be declared with five here, which popped the caller's
 * local into eax and read a pointer as a character count - the crash was a
 * memset with a 2 GB length. tools/check_shim_arity.py now compares every
 * shared symbol against pcrecomp's win32hle.)
 *
 * The CRT calls this with CT_CTYPE1 to implement iswalpha/iswdigit and friends.
 * Levels 2 and 3 are the Far East case/width tables; refusing them is honest
 * and makes the CRT take its fallback. cch == -1 means NUL-terminated. */
static void k_GetStringTypeW(void) {
    uint32_t type = A32(0), src = A32(1), dst = A32(3);
    int cch = (int)A32(2);
    const uint16_t *s;
    uint16_t *out;
    if (!dst || type != 1) { RET(0, 4); }
    s = (const uint16_t *)ka_host_or_null(src);
    out = (uint16_t *)ka_host_or_null(dst);
    if (!s || !out) { RET(0, 4); }
    if (cch < 0) { cch = 0; while (s[cch]) cch++; }
    for (int i = 0; i < cch; i++) {
        uint16_t c = s[i], t;
        if (c >= 'A' && c <= 'Z')        t = 0x0001 | 0x0100; /* upper, alpha */
        else if (c >= 'a' && c <= 'z')   t = 0x0002 | 0x0100; /* lower, alpha */
        else if (c >= '0' && c <= '9')   t = 0x0004 | 0x0080; /* digit, xdigit */
        else if (c == ' ')               t = 0x0008 | 0x0040; /* space, blank */
        else if (c >= 0x09 && c <= 0x0D) t = 0x0008;          /* space */
        else if (c < 0x20 || c == 0x7F)  t = 0x0020;          /* control */
        else if (c < 0x7F)               t = 0x0010;          /* punct */
        else                             t = 0x0100;          /* alpha elsewhere */
        out[i] = t;
    }
    RET(1, 4);
}

static void k_GetCommandLineA(void) {
    static uint32_t cached;
    if (!cached) cached = ka_guest_strdup("LoK.exe");
    RET(cached, 0);
}
static void k_GetEnvironmentStringsW(void) {
    static uint32_t cached;
    if (!cached) {
        cached = ka_guest_alloc(4, 2);
        ((uint16_t *)ka_host(cached))[0] = 0;
        ((uint16_t *)ka_host(cached))[1] = 0;
        ka_log("env: GetEnvironmentStringsW -> 0x%08X", cached);
    }
    RET(cached, 0);
}
static void k_FreeEnvironmentStringsW(void) { RET(1, 1); }
static void k_SetEnvironmentVariableA(void) { RET(1, 2); }
static void k_GetStartupInfoW(void) {
    if (A32(0)) memset(ka_host(A32(0)), 0, 68);
    RETV(1);
}
static void k_GetSystemInfo(void) {
    uint32_t out = A32(0);
    if (out) {
        memset(ka_host(out), 0, 36);
        MEM32(out + 4) = 4096;        /* dwPageSize */
        MEM32(out + 8) = 0x10000;     /* lpMinimumApplicationAddress */
        MEM32(out + 12) = 0x7FFEFFFF; /* lpMaximumApplicationAddress */
        MEM32(out + 16) = 0xFF;       /* dwActiveProcessorMask */
        MEM32(out + 20) = 8;          /* dwNumberOfProcessors */
        MEM32(out + 24) = 586;        /* dwProcessorType */
        MEM32(out + 28) = 0x10000;    /* dwAllocationGranularity */
        MEM16(out + 32) = 6;          /* wProcessorLevel */
        MEM16(out + 34) = 0x3A09;     /* wProcessorRevision */
    }
    RETV(1);
}
static void k_GetConsoleCP2(void) { RET(1252, 0); }
static void k_ReadConsoleW(void) { RET(0, 5); }
static void k_WriteConsoleW(void) { RET(1, 5); }

/* ------------------------------------------------------------ the table */
#define S(name, fn, n) { name, fn },
const ka_shim ka_kernel32_shims[] = {
    { "CloseHandle", k_CloseHandle },
    { "GetLastError", k_GetLastError },
    { "SetLastError", k_SetLastError },
    { "GetCurrentProcess", k_GetCurrentProcess },
    { "GetCurrentProcessId", k_GetCurrentProcessId },
    { "GetCurrentThread", k_GetCurrentThread },
    { "GetCurrentThreadId", k_GetCurrentThreadId },
    { "GetTickCount", k_GetTickCount },
    { "QueryPerformanceFrequency", k_QueryPerformanceFrequency },
    { "QueryPerformanceCounter", k_QueryPerformanceCounter },
    { "GetSystemTimeAsFileTime", k_GetSystemTimeAsFileTime },
    { "FileTimeToSystemTime", k_FileTimeToSystemTime },
    { "SystemTimeToTzSpecificLocalTime", k_SystemTimeToTzSpecificLocalTime },
    { "GetTimeZoneInformation", k_GetTimeZoneInformation },
    { "Sleep", k_Sleep },
    { "SleepEx", k_SleepEx },
    { "OutputDebugStringW", k_OutputDebugStringW },
    { "IsDebuggerPresent", k_IsDebuggerPresent },
    { "IsProcessorFeaturePresent", k_IsProcessorFeaturePresent },
    { "EncodePointer", k_EncodePointer },
    { "DecodePointer", k_DecodePointer },
    { "SetUnhandledExceptionFilter", k_SetUnhandledExceptionFilter },
    { "UnhandledExceptionFilter", k_UnhandledExceptionFilter },
    { "RaiseException", k_RaiseException },
    { "RtlUnwind", k_RtlUnwind },
    { "ExitProcess", k_ExitProcess },
    { "ExitThread", k_ExitThread },
    { "TerminateProcess", k_TerminateProcess },
    { "GetProcessHeap", k_GetProcessHeap },
    { "HeapCreate", k_HeapCreate },
    { "HeapDestroy", k_HeapDestroy },
    { "HeapAlloc", k_HeapAlloc },
    { "HeapFree", k_HeapFree },
    { "HeapSize", k_HeapSize },
    { "HeapReAlloc", k_HeapReAlloc },
    { "GlobalAlloc", k_GlobalAlloc },
    { "GlobalFree", k_GlobalFree },
    { "GlobalSize", k_GlobalSize },
    { "VirtualAlloc", k_VirtualAlloc },
    { "VirtualQuery", k_VirtualQuery },
    { "InitializeCriticalSection", k_InitializeCriticalSection },
    { "InitializeCriticalSectionAndSpinCount", k_InitCSAndSpin },
    { "SetCriticalSectionSpinCount", k_SetCSSpin },
    { "EnterCriticalSection", k_EnterCriticalSection },
    { "LeaveCriticalSection", k_LeaveCriticalSection },
    { "DeleteCriticalSection", k_DeleteCriticalSection },
    { "InterlockedIncrement", k_InterlockedIncrement },
    { "InterlockedDecrement", k_InterlockedDecrement },
    { "InterlockedExchange", k_InterlockedExchange },
    { "InterlockedExchangeAdd", k_InterlockedExchangeAdd },
    { "InterlockedCompareExchange", k_InterlockedCompareExchange },
    { "TlsAlloc", k_TlsAlloc },
    { "TlsFree", k_TlsFree },
    { "TlsGetValue", k_TlsGetValue },
    { "TlsSetValue", k_TlsSetValue },
    { "FlsAlloc", k_FlsAlloc },
    { "FlsFree", k_FlsFree },
    { "FlsGetValue", k_FlsGetValue },
    { "FlsSetValue", k_FlsSetValue },
    { "CreateFileA", k_CreateFileA },
    { "CreateFileW", k_CreateFileW },
    { "ReadFile", k_ReadFile },
    { "WriteFile", k_WriteFile },
    { "GetFileSize", k_GetFileSize },
    { "GetFileType", k_GetFileType },
    { "FlushFileBuffers", k_FlushFileBuffers },
    { "SetEndOfFile", k_SetEndOfFile },
    { "DeleteFileA", k_DeleteFileA },
    { "MoveFileA", k_MoveFileA },
    { "CreateDirectoryA", k_CreateDirectoryA },
    { "SetCurrentDirectoryA", k_SetCurrentDirectoryA },
    { "GetFileAttributesA", k_GetFileAttributesA },
    { "SetFilePointer", k_SetFilePointer },
    { "SetFilePointerEx", k_SetFilePointerEx },
    { "GetStdHandle", k_GetStdHandle },
    { "SetStdHandle", k_SetStdHandle },
    { "FindFirstFileA", k_FindFirstFileA },
    { "FindFirstFileExA", k_FindFirstFileExA },
    { "FindNextFileA", k_FindNextFileA },
    { "FindClose", k_FindClose },
    { "GetModuleFileNameA", k_GetModuleFileNameA },
    { "GetModuleFileNameW", k_GetModuleFileNameW },
    { "GetModuleHandleA", k_GetModuleHandleA },
    { "GetModuleHandleW", k_GetModuleHandleW },
    { "GetModuleHandleExW", k_GetModuleHandleExW },
    { "LoadLibraryW", k_LoadLibraryW },
    { "LoadLibraryExW", k_LoadLibraryExW },
    { "FreeLibrary", k_FreeLibrary },
    { "GetProcAddress", k_GetProcAddress },
    { "CreateThread", k_CreateThread },
    { "WaitForSingleObject", k_WaitForSingleObject },
    { "WaitForMultipleObjectsEx", k_WaitForMultipleObjectsEx },
    { "ResumeThread", k_ResumeThread },
    { "GetExitCodeThread", k_ExitCodeThread },
    { "OpenThread", k_OpenThread },
    { "SetThreadPriority", k_SetThreadPriority },
    { "GetThreadPriority", k_GetThreadPriority },
    { "SetThreadAffinityMask", k_SetThreadAffinityMask },
    { "GetProcessAffinityMask", k_GetProcessAffinityMask },
    { "CreateEventA", k_CreateEventA },
    { "CreateEventW", k_CreateEventW },
    { "SetEvent", k_SetEvent },
    { "ResetEvent", k_ResetEvent },
    { "CreateMutexA", k_CreateMutexA },
    { "ReleaseMutex", k_ReleaseMutex },
    { "CreateSemaphoreA", k_CreateSemaphoreA },
    { "CreateSemaphoreExW", k_CreateSemaphoreExW },
    { "InitializeCriticalSectionEx", k_InitializeCriticalSectionEx },
    { "SetThreadStackGuarantee", k_SetThreadStackGuarantee },
    { "FlushProcessWriteBuffers", k_FlushProcessWriteBuffers },
    { "GetCurrentProcessorNumber", k_GetCurrentProcessorNumber },
    { "ReleaseSemaphore", k_ReleaseSemaphore },
    { "GetACP", k_GetACP },
    { "GetOEMCP", k_GetOEMCP },
    { "GetConsoleCP", k_GetConsoleCP },
    { "GetConsoleMode", k_GetConsoleMode },
    { "IsValidCodePage", k_IsValidCodePage },
    { "GetCPInfo", k_GetCPInfo },
    { "MultiByteToWideChar", k_MultiByteToWideChar },
    { "WideCharToMultiByte", k_WideCharToMultiByte },
    { "LCMapStringW", k_LCMapStringW },
    { "CompareStringW", k_CompareStringW },
    { "GetStringTypeW", k_GetStringTypeW },
    { "GetCommandLineA", k_GetCommandLineA },
    { "GetEnvironmentStringsW", k_GetEnvironmentStringsW },
    { "FreeEnvironmentStringsW", k_FreeEnvironmentStringsW },
    { "SetEnvironmentVariableA", k_SetEnvironmentVariableA },
    { "GetStartupInfoW", k_GetStartupInfoW },
    { "GetSystemInfo", k_GetSystemInfo },
    { "ReadConsoleW", k_ReadConsoleW },
    { "WriteConsoleW", k_WriteConsoleW },
    { NULL, NULL }
};

