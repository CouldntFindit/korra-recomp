/* spine_android.c - register file, machine lock, dispatch, shims, threads.
 *
 * The lifted guest has one global register file, so exactly one guest thread
 * runs at a time: mach_enter() takes a recursive process-wide lock and loads
 * that thread's saved registers, mach_leave() saves them and releases it. A
 * blocked host call inside a shim (sleep, file read) therefore also blocks the
 * guest, which is what the Windows host does too and what the engine's own
 * single-machine-lock assumption expects.
 *
 * Per-thread state lives in host TLS; the guest-visible TIB is a 4 KiB block
 * inside the guest window, because g_fs_base is a 32-bit VA.
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "host_android.h"

/* ------------------------------------------------------------ register file */
uint32_t  g_eax, g_ecx, g_edx, g_esp, g_ebx, g_esi, g_edi, g_ebp;
double    g_st[8];
int       g_fp_top;
uint16_t  g_fpu_cw = 0x027F;
uint16_t  g_seg_cs, g_seg_ds, g_seg_es, g_seg_fs, g_seg_gs, g_seg_ss;
uint64_t  g_mm[8];
uint32_t  g_fs_base, g_gs_base;
ptrdiff_t g_mem_base;
uint32_t  g_cur_func;
const char *g_cur_import;
uint32_t  g_icall_trace[ICALL_TRACE_SIZE], g_icall_from[ICALL_TRACE_SIZE];
uint32_t  g_icall_trace_idx, g_icall_count;
static uint32_t g_trace_left = 12000;
/* Every so many import dispatches, hand the machine to another guest thread.
 * The engine's spin-waits call InterlockedExchange in a loop that has no
 * instrumented lift back edge, so recomp_yield_hook never fires there. */
static uint32_t g_import_calls;
/* Set from the guest's main thread (ka_spine_note_main); lets the import trace
 * above stay silent for the main thread and log only spawned workers. */
static uint32_t g_main_fs;
/* The guest's main thread as a host pthread, so the watchdog can take a stack
 * sample of it (g_main_pt is set by ka_spine_note_main). */
static pthread_t g_main_pt;
static pid_t g_main_tid;
static int g_wtrace_left = 800;

/* SSE/MMX register file and control word (recomp_simd.h). The lifted code uses
 * them for every movaps/mulps/movd instruction. */
V128      g_xmm[8];
uint32_t  g_mxcsr = 0x1F80u;   /* round-to-nearest, all exceptions masked */

/* An instruction the lifter could not express. Continuing is the worst failure
 * a recompile can have (the state is already wrong), so name it and stop. */
void recomp_unimpl(uint32_t va, const char *what) {
    ka_log("[unimpl] 0x%08X: %s (guest func 0x%08X)", va,
           what ? what : "(no description)", g_cur_func);
    abort();
}

/* --------------------------------------------------------------- the lock */
static pthread_mutex_t g_mach = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;

typedef struct {
    uint32_t eax, ecx, edx, esp, ebx, esi, edi, ebp, fs, cur;
    double   st[8];
    int      fp_top;
    uint16_t fpu_cw;
    uint64_t mm[8];
    int      depth, inited;
    uint32_t tls[64];
    uint32_t last_error;
} tstate;

static __thread tstate g_ts;

static void regs_save(tstate *t) {
    t->eax=g_eax; t->ecx=g_ecx; t->edx=g_edx; t->esp=g_esp;
    t->ebx=g_ebx; t->esi=g_esi; t->edi=g_edi; t->ebp=g_ebp;
    t->fs=g_fs_base; t->cur=g_cur_func;
    memcpy(t->st, g_st, sizeof g_st); t->fp_top=g_fp_top; t->fpu_cw=g_fpu_cw;
    memcpy(t->mm, g_mm, sizeof g_mm);
}
static void regs_load(const tstate *t) {
    g_eax=t->eax; g_ecx=t->ecx; g_edx=t->edx; g_esp=t->esp;
    g_ebx=t->ebx; g_esi=t->esi; g_edi=t->edi; g_ebp=t->ebp;
    g_fs_base=t->fs; g_cur_func=t->cur;
    memcpy(g_st, t->st, sizeof g_st); g_fp_top=t->fp_top; g_fpu_cw=t->fpu_cw;
    memcpy(g_mm, t->mm, sizeof g_mm);
}

/* Guest TIB: ExceptionList, StackBase, StackLimit, ..., Self. */
uint32_t ka_thread_attach(const char *why) {
    if (!g_ts.inited) {
        uint32_t limit = 0;
        uint32_t esp = ka_stack_new(4u << 20, &limit);
        uint32_t tib = ka_guest_alloc(0x1000, 16);
        uint32_t *t;
        if (!esp || !tib) {
            ka_log("FATAL: cannot create guest stack/TIB (%s)", why ? why : "?");
            abort();
        }
        t = (uint32_t *)ka_host(tib);
        t[0x00/4] = 0xFFFFFFFFu;     /* ExceptionList: end of chain */
        t[0x04/4] = limit + (4u << 20);
        t[0x08/4] = limit;
        t[0x18/4] = tib;             /* Self */
        g_ts.esp = esp;
        g_ts.fs  = tib;
        g_ts.fpu_cw = 0x027F;
        g_ts.inited = 1;
        g_ts.last_error = 0;
        ka_log("thread: attach fs=%08X tib=%08X why=%s", g_ts.fs, tib, why ? why : "?");
    }
    return g_ts.fs;
}

void ka_thread_detach(void) { /* stacks are freed with the process */ }

static volatile uint32_t g_wd_fs, g_wd_depth;

void mach_enter(void) {
    pthread_mutex_lock(&g_mach);
    if (!g_ts.inited) {
        pthread_mutex_unlock(&g_mach);
        ka_thread_attach("mach_enter");
        pthread_mutex_lock(&g_mach);
    }
    if (g_ts.depth++ == 0) {
        regs_load(&g_ts);
        /* Mirror for the stall watchdog (see below): it cannot read this
         * thread's TLS, and a diagnostic read of a word only the machine holder
         * writes is harmless. */
        g_wd_fs = g_ts.fs;
        g_wd_depth = g_ts.depth;
        /* One line per host thread the first time it gets the machine: with
         * five workers parked in mach_enter and the main thread spinning, this
         * says whether the hand-over never happens (no worker line) or the
         * workers do run and then block somewhere else. */
        {
            static __thread int logged;
            if (!logged) {
                logged = 1;
                ka_log("mach: first acquire fs=%08X", g_ts.fs);
            }
        }
    }
}

void mach_leave(void) {
    if (--g_ts.depth == 0) regs_save(&g_ts);
    g_wd_depth = g_ts.depth;
    pthread_mutex_unlock(&g_mach);
}

/* The guest's main thread identifies itself once, after its first mach_enter,
 * so the import trace can log only the spawned workers (which are the ones a
 * stuck boot needs to name). */
void ka_spine_note_main(void) {
    g_main_fs = g_ts.fs;
    g_main_pt = pthread_self();
    g_main_tid = (pid_t)syscall(SYS_gettid);
}
uint32_t ka_spine_main_fs(void) { return g_main_fs; }

/* ---- stall watchdog -----------------------------------------------------
 * A boot that goes silent is either lifted code that calls no import (so no
 * shim line appears) or a thread parked inside a shim. Nothing in the guest can
 * say which, so a host thread reports the machine holder once a second while
 * the import count does not move: depth 0 means the holder is inside a shim,
 * depth > 0 with a steady func means a lifted loop. Bounded, then silent. */
static void ka_stall_probe_fire(void);

/* Filled by the stack-probe handler (ka_stall_handler) and printed afterwards by
 * the watchdog, so nothing logs from a signal context. */
static void *g_stall_pc[40];
static volatile int g_stall_n;
static volatile uint32_t g_stall_esp, g_stall_func;

static void *watchdog_thread(void *arg) {
    static int n;
    int stalled = 0;
    uint32_t last = 0;
    uint32_t last_bsrz = 0;
    int bsrz_logged = 0;
    (void)arg;
    for (;;) {
        struct timespec ts;
        ts.tv_sec = 1;
        ts.tv_nsec = 0;
        nanosleep(&ts, NULL);
        /* Bounded proof that the guarded zero-source bit scans are reached,
         * and how often: first 20 changes only. */
        {
            uint32_t z = recomp_bsf_zero_hits + recomp_bsr_zero_hits;
            if (z != last_bsrz && bsrz_logged < 20) {
                last_bsrz = z;
                bsrz_logged++;
                ka_log("bsrz: total=%u bsf=%u bsr=%u", z,
                       recomp_bsf_zero_hits, recomp_bsr_zero_hits);
            }
        }
        if (last == g_import_calls) {
            stalled++;
            if (n < 40) {
                n++;
                ka_log("wd: stalled fs=%08X depth=%u func=%08X imports=%u back=%u esp=%08X",
                       g_wd_fs, g_wd_depth, g_cur_func, g_import_calls, g_backedges, g_esp);
            }
            /* Two silent seconds with no back edges means the machine holder
             * is parked in host code; its own stack is the only thing that can
             * name where. Interrupt that thread so it samples itself. */
            if (stalled == 2 || stalled == 6 || stalled == 12)
                ka_stall_probe_fire();
        } else {
            n = 0;
            stalled = 0;
        }
        last = g_import_calls;
    }
    return NULL;
}

/* ---- stack sample of a stalled guest thread -------------------------
 * A guest stall inside host code has no guest stack worth walking (the
 * recompiled call pushes only RECOMP_RETADDR), so the machine holder is
 * interrupted with a signal and unwinds its own host stack, which does name
 * the shim or driver call it is parked in. Offsets are relative to the module
 * base: llvm-addr2line -f -C -e libkorra.so on them names the frames.
 *
 * The signal is SIGURG, not SIGUSR1: the app's threads inherit ART's mask,
 * which blocks SIGUSR1 (ART suspends threads for a GC with it) as well as
 * SIGQUIT, SIGPIPE and SIGRTMIN. A SIGUSR1 probe was accepted by
 * pthread_kill, stayed pending forever (procfs SigPnd bit 9) and captured
 * nothing - including when the watchdog signalled itself. Nothing in the
 * process claims SIGURG, so it is delivered. */
#include <signal.h>
#include <unwind.h>
#include <dlfcn.h>

typedef struct { void **pc; int n, max; } ka_bt;
static _Unwind_Reason_Code ka_bt_cb(struct _Unwind_Context *ctx, void *arg) {
    ka_bt *b = (ka_bt *)arg;
    uintptr_t ip = (uintptr_t)_Unwind_GetIP(ctx);
    if (ip && b->n < b->max) b->pc[b->n++] = (void *)ip;
    return b->n < b->max ? _URC_NO_REASON : _URC_END_OF_STACK;
}
static int ka_host_bt(void **pc, int max) {
    ka_bt b;
    b.pc = pc; b.n = 0; b.max = max;
    _Unwind_Backtrace(ka_bt_cb, &b);
    return b.n;
}
static void ka_stall_handler(int sig, siginfo_t *si, void *uc) {
    (void)sig; (void)si; (void)uc;
    /* No logging here: ka_log takes a lock and a FILE, neither is safe from a
     * signal context. Capture the frames and let the watchdog print them. */
    g_stall_n = ka_host_bt(g_stall_pc, 40);
    g_stall_esp = g_esp;
    g_stall_func = g_cur_func;
}
/* Does the signal machinery work at all? Kill ourselves and see whether the
 * handler captured a stack; if this is zero the delivery is the problem, not
 * the target thread. */
static int ka_stall_selftest(void) {
    g_stall_n = 0;
    pthread_kill(pthread_self(), SIGURG);
    {
        struct timespec ts = { 0, 20 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    return g_stall_n;
}

/* The signal mask of a thread, from procfs: a pending bit or a blocked bit
 * explains a probe that reports rc=0 and captures nothing. */
static void ka_stall_mask_log(const char *what, pid_t tid) {
    char path[64], buf[512];
    FILE *f;
    snprintf(path, sizeof path, "/proc/self/task/%d/status", (int)tid);
    f = fopen(path, "r");
    if (!f) { ka_log("stall: %s tid=%d status unreadable", what, (int)tid); return; }
    while (fgets(buf, sizeof buf, f)) {
        if (!strncmp(buf, "SigBlk:", 7) || !strncmp(buf, "SigPnd:", 7) ||
            !strncmp(buf, "ShdPnd:", 7))
            ka_log("stall: %s tid=%d %s", what, (int)tid, buf);
    }
    fclose(f);
}

static void ka_stall_probe_fire(void) {
    int r;
    if (!g_main_pt) { ka_log("stall: probe has no main thread"); return; }
    g_stall_n = 0;
    r = pthread_kill(g_main_pt, SIGURG);
    {
        struct timespec ts = { 0, 20 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    ka_log("stall: probe rc=%d frames=%d esp=%08X func=%08X",
           r, g_stall_n, g_stall_esp, g_stall_func);
    if (g_stall_n == 0 && g_main_tid > 0)
        ka_stall_mask_log("main", g_main_tid);
    if (g_stall_n > 0) {
        Dl_info di;
        uintptr_t base = 0;
        int i;
        if (dladdr((void *)&ka_stall_handler, &di) && di.dli_fbase)
            base = (uintptr_t)di.dli_fbase;
        ka_log("stall: base=%08lX", (unsigned long)base);
        for (i = 0; i < g_stall_n; i++)
            ka_log("stall: off=0x%lX", (unsigned long)((uintptr_t)g_stall_pc[i] - base));
    }
}

void ka_spine_watchdog_start(void) {
    pthread_t t;
    pthread_attr_t attr;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = ka_stall_handler;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SIGURG, &sa, NULL);
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &attr, watchdog_thread, NULL) == 0) {
        /* Run the probe once against ourselves before relying on it. */
        ka_log("wd: started");
        ka_log("wd: selftest frames=%d", ka_stall_selftest());
    }
    pthread_attr_destroy(&attr);
}

/* Hand the machine to another guest thread at a lifted loop's back edge.
 *
 * The engine's own spin-waits are plain guest loops that call
 * InterlockedExchange; the thread holding the lock they wait for is parked in
 * mach_enter() and would never run, because a guest thread otherwise keeps
 * g_mach from hle_call_guest() to the end of its entry function. The runtime's
 * RECOMP_BACKEDGE calls recomp_yield_hook every 0x10000 backward branches and
 * saves/restores the register file around it, so leaving here is safe. */
void ka_machine_yield(void) {
    extern int ka_spine_depth(void);
    int depth = 0;
    static uint32_t n;
    if (n < 3 || (n % 1000u) == 0)
        ka_log("yield: n=%u depth=%u from=%08X", n, g_ts.depth, g_cur_func);
    n++;
    /* Unwind the WHOLE recursive depth: the guest runs at depth 2 (guest_thread
     * enters, then hle_call_guest enters), and mach_leave() only unlocks at
     * depth 0 - so a single leave/sleep/enter slept while still holding the
     * machine and the waiting workers never ran. guest_sleep_ms unwinds the
     * same way; this must too. */
    while (ka_spine_depth() > 0) { mach_leave(); depth++; }
    {
        struct timespec ts;
        ts.tv_sec = 0;
        ts.tv_nsec = 100000L;   /* 100 us: long enough for a waiter to run */
        nanosleep(&ts, NULL);
    }
    while (depth-- > 0) mach_enter();
}

uint32_t ka_last_error_get(void) { return g_ts.last_error; }
void     ka_last_error_set(uint32_t e) { g_ts.last_error = e; }
uint32_t ka_tls_alloc(void) {
    for (int i = 0; i < 64; i++)
        if (!g_ts.tls[i]) { g_ts.tls[i] = 1; return (uint32_t)i; }
    return 0xFFFFFFFFu;
}
void ka_tls_free(uint32_t idx) { if (idx < 64) g_ts.tls[idx] = 0; }
uint32_t ka_tls_get(uint32_t idx) { return idx < 64 ? g_ts.tls[idx] : 0; }
void ka_tls_set(uint32_t idx, uint32_t v) { if (idx < 64) g_ts.tls[idx] = v ? v : 1; }

/* --------------------------------------------------------------- dispatch */
recomp_func_t recomp_lookup(uint32_t va) {
    uint32_t lo = 0, hi = recomp_dispatch_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2, m = recomp_dispatch_table[mid].address;
        if (m == va) return recomp_dispatch_table[mid].func;
        if (m < va) lo = mid + 1; else hi = mid;
    }
    return NULL;
}
recomp_func_t recomp_lookup_manual(uint32_t va) { (void)va; return NULL; }

/* ---------------------------------------------------------- shim registry */
static struct { const char *name; recomp_func_t fn; uint32_t va; } g_shim[KA_SHIM_MAX];
static int g_shim_n;
static uint32_t g_last_import_va;

static uint32_t shim_add(const char *name, recomp_func_t fn) {
    if (g_shim_n >= KA_SHIM_MAX) { ka_log("FATAL: too many shims"); abort(); }
    /* Names arrive in caller stack buffers (vtable builders, ordinal names) and
     * the registry outlives them, so take a copy. */
    g_shim[g_shim_n].name = strdup(name ? name : "");
    g_shim[g_shim_n].fn   = fn;
    g_shim[g_shim_n].va   = KA_SHIM_BASE + KA_SHIM_STEP * (uint32_t)g_shim_n;
    return g_shim[g_shim_n++].va;
}

int ka_shim_register(const ka_shim *shims) {
    int added = 0;
    for (; shims->name; shims++) { shim_add(shims->name, shims->fn); added++; }
    return added;
}

uint32_t ka_shim_va(const char *name) {
    for (int i = 0; i < g_shim_n; i++)
        if (!strcmp(g_shim[i].name, name)) return g_shim[i].va;
    return 0;
}

const char *ka_shim_name(uint32_t va) {
    for (int i = 0; i < g_shim_n; i++) if (g_shim[i].va == va) return g_shim[i].name;
    return NULL;
}

uint32_t ka_shim_count(void) { return (uint32_t)g_shim_n; }

static void shim_unimplemented(void) {
    const char *nm = ka_shim_name(g_last_import_va);
    ka_log("FATAL: unimplemented import %s (0x%08X) called from guest 0x%08X",
           nm ? nm : "?", g_last_import_va, g_cur_func);
    abort();
}

uint32_t ka_shim_stub(const char *name) {
    uint32_t va = ka_shim_va(name);
    if (va) return va;
    return shim_add(name, shim_unimplemented);
}

/* Bounded import trace. A boot makes tens of thousands of import calls, and an
 * unbounded trace buries the interesting lines; the tail of the trace is what
 * failures need, so keep the first N cold calls and say when it stops. The
 * allocator and lock traffic inside CRT init is tens of thousands of lines on
 * its own and tells nothing, so those names never spend budget. */
static int trace_is_hot(const char *name) {
    static const char *const hot[] = {
        "EnterCriticalSection", "LeaveCriticalSection", "DecodePointer",
        "EncodePointer", "HeapSize", "HeapAlloc", "HeapFree", "HeapReAlloc",
        "GetLastError", "SetLastError", "GetTickCount", "Sleep",
        "QueryPerformanceCounter", "InterlockedIncrement",
        "InterlockedDecrement", "TlsGetValue", "TlsSetValue",
        "InterlockedCompareExchange", "InterlockedExchange", "GetCurrentThreadId",
        "InterlockedExchangeAdd",
    };
    for (unsigned k = 0; k < sizeof hot / sizeof hot[0]; k++)
        if (!strcmp(name, hot[k])) return 1;
    return 0;
}

recomp_func_t recomp_lookup_import(uint32_t va) {
    if (va < KA_SHIM_BASE || va >= KA_SHIM_BASE + KA_SHIM_STEP * (uint32_t)g_shim_n)
        return NULL;
    uint32_t i = (va - KA_SHIM_BASE) / KA_SHIM_STEP;
    if (i < (uint32_t)g_shim_n && g_shim[i].va == va) {
        g_last_import_va = va;
        /* A synthetic COM slot ("D3D9:5", "DInput:3", ...) may carry `this` in
         * ECX or as the first stack argument; the bridge decides per call. */
        ka_vtable_note_import(g_shim[i].name);
        if (ka_trace_imports && g_trace_left > 0 && !trace_is_hot(g_shim[i].name)) {
            /* esp/ebp on the line: a shim that pops the wrong count shifts esp,
             * and the guest's next `pop ebp` turns into garbage. Seeing the
             * stack pair walk tells that apart from a real guest bug. */
            if (--g_trace_left == 0)
                ka_log("[import] %s esp=0x%08X ebp=0x%08X (trace limit; set KORRA_TRACE_IMPORTS=0 to silence)",
                       g_shim[i].name, g_esp, g_ebp);
            else
                ka_log("[import] %s esp=0x%08X ebp=0x%08X", g_shim[i].name, g_esp, g_ebp);
        }
        if ((++g_import_calls & 0xFFFu) == 0)
            ka_machine_yield();
        /* Worker-only trace: the main thread's spin floods the shared budget,
         * so a stuck worker is invisible in the normal import log. This names
         * the sequence each spawned thread runs after its entry point. */
        if (g_wtrace_left > 0 && g_main_fs && g_fs_base != g_main_fs) {
            --g_wtrace_left;
            ka_log("[w%08X] %s esp=0x%08X", g_fs_base, g_shim[i].name, g_esp);
        }
        return g_shim[i].fn;
    }
    return NULL;
}

/* --------------------------------------------------------- native -> guest */
uint32_t hle_call_guest(uint32_t va, int nargs, const uint32_t *args) {
    recomp_func_t f = recomp_lookup(va);
    if (!f) f = recomp_lookup_import(va);
    if (!f) {
        ka_log("fatal: no lifted function at 0x%08X (called from 0x%08X)", va, g_cur_func);
        abort();
    }
    mach_enter();
    tstate saved = g_ts;
    regs_save(&saved);
    for (int i = nargs - 1; i >= 0; i--) PUSH32(g_esp, args ? args[i] : 0);
    PUSH32(g_esp, RECOMP_RETADDR);
    f();
    uint32_t eax = g_eax;
    regs_load(&saved);
    mach_leave();
    return eax;
}

/* ------------------------------------------------------------- LdrGetProc */
uint32_t ka_proc_address(const char *dll, const char *name);

/* ---------------------------------------------------------------- threads */
#define MAX_THREADS 64
typedef struct {
    uint32_t token, used, done, exit_code, start_va, param;
    uint32_t suspended, resumed;   /* CREATE_SUSPENDED gate, see ka_thread_resume */
    pthread_t th;
} ka_thread;
static ka_thread g_threads[MAX_THREADS];
static pthread_mutex_t g_threads_lock = PTHREAD_MUTEX_INITIALIZER;
/* CREATE_SUSPENDED parks the thread at its entry point until ResumeThread. */
static pthread_cond_t g_resume_cond = PTHREAD_COND_INITIALIZER;
static uint32_t g_next_token = 0x100;

typedef struct { uint32_t start_va, param, token; uint32_t *gate; } thread_start_t;

static void *thread_trampoline(void *p) {
    thread_start_t *t = (thread_start_t *)p;
    uint32_t args[1];
    uint32_t ret;
    ka_thread_attach("CreateThread");
    /* CREATE_SUSPENDED: the engine's spawner passes the start routine, then
     * fills the parameter block (its function pointer, its event) only after
     * CreateThread returns. A thread that starts early reads those slots as
     * zero and calls NULL in a loop. Windows holds it at the entry point. */
    ka_log("thread: enter va=%08X param=%08X tok=%u waiting=%d",
           t->start_va, t->param, t->token, (t->gate && !*t->gate) ? 1 : 0);
    pthread_mutex_lock(&g_threads_lock);
    while (t->gate && !*t->gate)
        pthread_cond_wait(&g_resume_cond, &g_threads_lock);
    pthread_mutex_unlock(&g_threads_lock);
    args[0] = t->param;
    ret = hle_call_guest(t->start_va, 1, args);   /* DWORD WINAPI fn(LPVOID) */
    pthread_mutex_lock(&g_threads_lock);
    for (int i = 0; i < MAX_THREADS; i++)
        if (g_threads[i].token == t->token) {
            g_threads[i].done = 1;
            g_threads[i].exit_code = ret;
        }
    pthread_mutex_unlock(&g_threads_lock);
    ka_log("thread: exit tok=%u code=0x%08X", t->token, ret);
    free(t);
    return NULL;
}

uint32_t ka_thread_create(uint32_t start_va, uint32_t param, int suspended) {
    pthread_mutex_lock(&g_threads_lock);
    for (int i = 0; i < MAX_THREADS; i++) {
        if (!g_threads[i].used) {
            thread_start_t *ts = (thread_start_t *)calloc(1, sizeof *ts);
            pthread_attr_t attr;
            g_threads[i].used = 1;
            g_threads[i].token = g_next_token++;
            g_threads[i].start_va = start_va;
            g_threads[i].param = param;
            ts->start_va = start_va; ts->param = param; ts->token = g_threads[i].token;
            g_threads[i].suspended = suspended ? 1u : 0u;
            g_threads[i].resumed = suspended ? 0u : 1u;
            ts->gate = &g_threads[i].resumed;
            pthread_attr_init(&attr);
            pthread_attr_setstacksize(&attr, 1u << 20);
            pthread_create(&g_threads[i].th, &attr, thread_trampoline, ts);
            pthread_attr_destroy(&attr);
            uint32_t tok = g_threads[i].token;
            ka_log("thread: create va=%08X param=%08X tok=%u suspended=%d",
                   start_va, param, tok, suspended);
            pthread_mutex_unlock(&g_threads_lock);
            return tok;
        }
    }
    pthread_mutex_unlock(&g_threads_lock);
    return 0;
}

ka_thread *ka_thread_find(uint32_t token) {
    for (int i = 0; i < MAX_THREADS; i++)
        if (g_threads[i].used && g_threads[i].token == token) return &g_threads[i];
    return NULL;
}

/* ResumeThread returns the previous suspend count; 0xFFFFFFFF means "not a
 * thread of ours", which the shim turns into WAIT_FAILED/ERROR_INVALID_HANDLE. */
uint32_t ka_thread_resume(uint32_t token) {
    ka_thread *t = ka_thread_find(token);
    if (!t) return 0xFFFFFFFFu;
    pthread_mutex_lock(&g_threads_lock);
    uint32_t prev = t->suspended;
    t->suspended = 0;
    t->resumed = 1;
    pthread_cond_broadcast(&g_resume_cond);
    pthread_mutex_unlock(&g_threads_lock);
    ka_log("thread: resume tok=%u prev=%u", token, prev);
    return prev;
}

uint32_t ka_thread_wait(uint32_t token, uint32_t timeout_ms) {
    ka_thread *t = ka_thread_find(token);
    if (!t) return 0xFFFFFFFFu;   /* WAIT_FAILED */
    /* The thread we wait for needs the machine lock, and the lock nests: the
     * guest runs at depth 2 (guest_thread enters, then hle_call_guest). The
     * timed path below used to release ONE level, which keeps the mutex held -
     * a main thread waiting for a worker therefore blocked it forever, and the
     * watchdog saw exactly that (main stalled at depth=1). Unwind the whole
     * recursion, the way guest_sleep_ms does. */
    int depth = g_ts.depth;
    for (int i = 0; i < depth; i++) mach_leave();
    if (timeout_ms == 0xFFFFFFFFu) {
        pthread_join(t->th, NULL);
        for (int i = 0; i < depth; i++) mach_enter();
        return 0;
    }
    {
        /* Timed wait: poll the done flag. */
        struct timespec ts = { 0, 2 * 1000 * 1000 };
        uint32_t waited = 0;
        while (!t->done && waited < timeout_ms) {
            nanosleep(&ts, NULL);
            waited += 2;
        }
        for (int i = 0; i < depth; i++) mach_enter();
        return t->done ? 0 : 258u;   /* WAIT_OBJECT_0 / WAIT_TIMEOUT */
    }
}

int ka_spine_depth(void) { return g_ts.depth; }

uint32_t ka_thread_exit_code(uint32_t token) {
    ka_thread *t = ka_thread_find(token);
    return t ? t->exit_code : 0;
}
