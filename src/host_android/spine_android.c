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
static int g_trace_left = 2000;   /* import-trace budget, see recomp_lookup_import */

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
    }
    return g_ts.fs;
}

void ka_thread_detach(void) { /* stacks are freed with the process */ }

void mach_enter(void) {
    pthread_mutex_lock(&g_mach);
    if (!g_ts.inited) {
        pthread_mutex_unlock(&g_mach);
        ka_thread_attach("mach_enter");
        pthread_mutex_lock(&g_mach);
    }
    if (g_ts.depth++ == 0) regs_load(&g_ts);
}

void mach_leave(void) {
    if (--g_ts.depth == 0) regs_save(&g_ts);
    pthread_mutex_unlock(&g_mach);
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

recomp_func_t recomp_lookup_import(uint32_t va) {    if (va < KA_SHIM_BASE || va >= KA_SHIM_BASE + KA_SHIM_STEP * (uint32_t)g_shim_n)
        return NULL;
    uint32_t i = (va - KA_SHIM_BASE) / KA_SHIM_STEP;
    if (i < (uint32_t)g_shim_n && g_shim[i].va == va) {
        g_last_import_va = va;
        /* Bounded: a boot makes tens of thousands of import calls, and an
         * unbounded trace buries the interesting lines. The tail of the trace
         * is what failures need, so keep the FIRST N (startup is the fragile
         * part) and say when it stops. */
        if (ka_trace_imports && g_trace_left > 0) {
            if (--g_trace_left == 0) ka_log("[import] %s (trace limit; set KORRA_TRACE_IMPORTS=0 to silence)", g_shim[i].name);
            else ka_log("[import] %s", g_shim[i].name);
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
    pthread_t th;
} ka_thread;
static ka_thread g_threads[MAX_THREADS];
static pthread_mutex_t g_threads_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t g_next_token = 0x100;

typedef struct { uint32_t start_va, param, token; } thread_start_t;

static void *thread_trampoline(void *p) {
    thread_start_t *t = (thread_start_t *)p;
    uint32_t args[1];
    uint32_t ret;
    ka_thread_attach("CreateThread");
    args[0] = t->param;
    ret = hle_call_guest(t->start_va, 1, args);   /* DWORD WINAPI fn(LPVOID) */
    pthread_mutex_lock(&g_threads_lock);
    for (int i = 0; i < MAX_THREADS; i++)
        if (g_threads[i].token == t->token) {
            g_threads[i].done = 1;
            g_threads[i].exit_code = ret;
        }
    pthread_mutex_unlock(&g_threads_lock);
    free(t);
    return NULL;
}

uint32_t ka_thread_create(uint32_t start_va, uint32_t param) {
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
            pthread_attr_init(&attr);
            pthread_attr_setstacksize(&attr, 1u << 20);
            pthread_create(&g_threads[i].th, &attr, thread_trampoline, ts);
            pthread_attr_destroy(&attr);
            uint32_t tok = g_threads[i].token;
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

uint32_t ka_thread_wait(uint32_t token, uint32_t timeout_ms) {
    ka_thread *t = ka_thread_find(token);
    if (!t) return 0xFFFFFFFFu;   /* WAIT_FAILED */
    if (timeout_ms == 0xFFFFFFFFu) {
        /* The guest thread needs the machine lock to run; release it while we
         * wait, otherwise a join inside a shim deadlocks. */
        int depth = g_ts.depth;
        for (int i = 0; i < depth; i++) mach_leave();
        pthread_join(t->th, NULL);
        for (int i = 0; i < depth; i++) mach_enter();
        return 0;
    }
    /* Timed wait: poll; the engine only uses this for short waits. */
    struct timespec ts = { 0, 2 * 1000 * 1000 };
    uint32_t waited = 0;
    while (!t->done && waited < timeout_ms) {
        mach_leave();
        nanosleep(&ts, NULL);
        mach_enter();
        waited += 2;
    }
    return t->done ? 0 : 258u;   /* WAIT_OBJECT_0 / WAIT_TIMEOUT */
}

int ka_spine_depth(void) { return g_ts.depth; }

uint32_t ka_thread_exit_code(uint32_t token) {
    ka_thread *t = ka_thread_find(token);
    return t ? t->exit_code : 0;
}
