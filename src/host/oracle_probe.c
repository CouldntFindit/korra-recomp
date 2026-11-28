/*
 * oracle_probe.c - two Windows-oracle instruments, both off by default.
 *
 * 1. KORRA_STALLPROBE=1 - a ring of every wait/signal the guest makes through
 *    the bridge (WaitForSingleObject, WaitForMultipleObjects, SetEvent,
 *    ResetEvent, ReleaseSemaphore, CreateEvent/Semaphore/Mutex, CreateThread,
 *    ExitThread), with the calling thread, the handle, the timeout, the caller
 *    (g_cur_func) and the result. A record whose result has not arrived yet is
 *    an in-flight call, so when the frame counter stops the ring still holds
 *    every parked wait - which is the one thing the perf census cannot say:
 *    it names the ntdll frames but not the object each thread is parked on.
 *
 *    Once the tick at [frameMgr+0x5C] (and +0x58) has not changed for
 *    KORRA_STALLPROBE_MS (default 3000), the ring is dumped exactly once:
 *    pending waits, the last 20 calls newest-first, and for every handle the
 *    creating call plus who last signalled it and when. A handle that was
 *    never signalled and a handle that was signalled with no waiter waking are
 *    different defects, and this line tells them apart.
 *
 * 2. KORRA_DINPUTPROBE=1 - a census of every call that lands in DINPUT8.dll
 *    (only DirectInput8Create is a named import; the device methods are COM
 *    vtable calls, so they appear here as DINPUT8.dll#<slot>). For
 *    IDirectInputDevice8 the slots are: 7 Acquire, 8 Unacquire, 9
 *    GetDeviceState, 10 GetDeviceData, 11 SetDataFormat, 12
 *    SetEventNotification, 13 SetCooperativeLevel. Every new call site prints
 *    once with its arguments and result; a one-second summary prints the
 *    per-site counts, so "the engine never polls the keyboard" is a count of
 *    zero rather than the absence of a name in a top-6 list. For slots 9/10
 *    the state buffer is sampled on EVERY poll (validated once, so the steady
 *    path is an 8-byte read): `first8` is the first poll, `or8` is the OR of
 *    every byte of every poll, and a one-shot line reports when that OR goes
 *    nonzero. That is what makes "does a held key ever appear in this state"
 *    decidable; before 2026-11-28 the site lookup returned before the read, so
 *    `or8` was the first poll forever and the line could not fire.
 *
 * Neither probe writes guest memory, takes the machine lock or calls guest
 * code. The dump runs on its own host thread and only reads the ring, so a
 * frozen guest cannot stop it (the sink thread proves the process is alive).
 *
 * Hook order: main() installs the perf probe, then this file; this file calls
 * the previous hooks first, exactly like draw_probe.c, so both instruments see
 * every bridged call.
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "native32.h"

#define VA_FRAME_MGR 0x00E4D060u   /* app / frame-manager singleton pointer */

#define RING 1024
#define OBJ_MAX 256
#define KIND_CACHE 96

enum {
    K_NONE = 0, K_WAIT1, K_WAITN, K_SET, K_RESET, K_REL, K_SIGANDWAIT,
    K_CREATEEVENT, K_CREATESEM, K_CREATEMUTEX, K_CREATETHREAD, K_EXITTHREAD
};

typedef struct {
    uint32_t seq;
    uint32_t tid;
    uint32_t kind;
    uint32_t target;
    uint32_t caller;
    uint32_t h;        /* handle (wait/set) or count/n for the others */
    uint32_t to;       /* timeout, or the second argument that matters */
    uint32_t state;    /* 0 = in flight, 1 = returned */
    uint32_t ret;
    uint64_t ms;       /* uptime at entry */
} sp_rec;

typedef struct {
    uint32_t h;        /* 0 = free               */
    uint32_t kind;     /* how it was created     */
    uint32_t creator;  /* g_cur_func at create   */
    uint32_t flags;    /* CreateEvent flags      */
    uint32_t sets;     /* SetEvent/Release count */
    uint32_t last_tid;
    uint64_t last_ms;
} sp_obj;

static sp_rec  g_ring[RING];
static sp_obj  g_obj[OBJ_MAX];
static volatile uint32_t g_seq;
static volatile LONG g_dumped;

static int      g_wait_on;
static int      g_din_on;
static uint32_t g_stall_ms = 3000;
static uint64_t g_t0;

/* No chaining: this probe is a leaf, like perf_probe's post hook. A probe that
 * captures "the hook that was installed" and is then itself captured by a
 * later re-install (draw_probe's watchdog re-installs its post hook every
 * 500 ms) turns the chain into a cycle: measured 2026-11-28 as
 * exception 0xC00000FD in sp_post's prologue, 0.2 s into the boot, with
 * KORRA_DRAWPATH=1. The hooks below carry a per-thread depth counter as a
 * second line of defence. */

static __declspec(thread) int t_pend = -1;      /* index of this thread's call */
static __declspec(thread) uint32_t t_pend_kind;

static uint64_t now_ms(void) {
    LARGE_INTEGER f, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    if (!g_t0) g_t0 = (uint64_t)t.QuadPart;
    if (!f.QuadPart) return 0;
    return (uint64_t)((t.QuadPart - (LONGLONG)g_t0) * 1000 / f.QuadPart);
}

/* ---------------------------------------------------------------- names */
static const char *name_of(uint32_t target) {
    const char *nm = native32_name(target);
    if (!nm) return NULL;
    {
        const char *bang = strchr(nm, '!');
        return bang ? bang + 1 : nm;
    }
}

static uint32_t kind_of(uint32_t target) {
    /* Direct-mapped cache: the hook runs on every bridged call (a few thousand
     * per frame), so classification must cost one array read after the first
     * sighting. 0 = never classified, 0xFFFFFFFF = not a wait/signal. */
    static uint32_t tag[8192], kind[8192];
    uint32_t idx = (target >> 2) & 8191u;
    const char *nm;
    uint32_t k = K_NONE;
    if (tag[idx] == target) return kind[idx];
    nm = name_of(target);
    if (nm) {
        if (!strcmp(nm, "WaitForSingleObject"))          k = K_WAIT1;
        else if (!strcmp(nm, "WaitForSingleObjectEx"))   k = K_WAIT1;
        else if (!strcmp(nm, "WaitForMultipleObjects"))  k = K_WAITN;
        else if (!strcmp(nm, "SetEvent"))                k = K_SET;
        else if (!strcmp(nm, "ResetEvent"))              k = K_RESET;
        else if (!strcmp(nm, "PulseEvent"))              k = K_SET;
        else if (!strcmp(nm, "ReleaseSemaphore"))        k = K_REL;
        else if (!strcmp(nm, "SignalObjectAndWait"))     k = K_SIGANDWAIT;
        else if (!strcmp(nm, "CreateEventA"))            k = K_CREATEEVENT;
        else if (!strcmp(nm, "CreateEventW"))            k = K_CREATEEVENT;
        else if (!strcmp(nm, "CreateSemaphoreA"))        k = K_CREATESEM;
        else if (!strcmp(nm, "CreateSemaphoreW"))        k = K_CREATESEM;
        else if (!strcmp(nm, "CreateMutexA"))            k = K_CREATEMUTEX;
        else if (!strcmp(nm, "CreateMutexW"))            k = K_CREATEMUTEX;
        else if (!strcmp(nm, "CreateThread"))            k = K_CREATETHREAD;
        else if (!strcmp(nm, "ExitThread"))              k = K_EXITTHREAD;
    }
    tag[idx] = target;
    kind[idx] = k ? k : 0xFFFFFFFFu;
    return k;
}

static const char *kind_name(uint32_t k) {
    switch (k) {
    case K_WAIT1:        return "WaitForSingleObject";
    case K_WAITN:        return "WaitForMultipleObjects";
    case K_SET:          return "SetEvent";
    case K_RESET:        return "ResetEvent";
    case K_REL:          return "ReleaseSemaphore";
    case K_SIGANDWAIT:   return "SignalObjectAndWait";
    case K_CREATEEVENT:  return "CreateEvent";
    case K_CREATESEM:    return "CreateSemaphore";
    case K_CREATEMUTEX:  return "CreateMutex";
    case K_CREATETHREAD: return "CreateThread";
    case K_EXITTHREAD:   return "ExitThread";
    default:             return "?";
    }
}

static int is_wait(uint32_t k) { return k == K_WAIT1 || k == K_WAITN || k == K_SIGANDWAIT; }
static int is_create(uint32_t k) {
    return k == K_CREATEEVENT || k == K_CREATESEM || k == K_CREATEMUTEX || k == K_CREATETHREAD;
}

/* ---------------------------------------------------------------- objects */
static sp_obj *obj_find(uint32_t h) {
    int i;
    if (!h) return NULL;
    for (i = 0; i < OBJ_MAX; i++) if (g_obj[i].h == h) return &g_obj[i];
    return NULL;
}

static sp_obj *obj_add(uint32_t h, uint32_t kind, uint32_t creator, uint32_t flags) {
    int i, free_i = -1;
    if (!h) return NULL;
    for (i = 0; i < OBJ_MAX; i++) {
        if (g_obj[i].h == h) return &g_obj[i];
        if (!g_obj[i].h && free_i < 0) free_i = i;
    }
    if (free_i < 0) return NULL;
    memset(&g_obj[free_i], 0, sizeof g_obj[free_i]);
    g_obj[free_i].h = h;
    g_obj[free_i].kind = kind;
    g_obj[free_i].creator = creator;
    g_obj[free_i].flags = flags;
    return &g_obj[free_i];
}

/* ---------------------------------------------------------------- dinput */
#define DIN_MAX 32
typedef struct {
    uint32_t target;    /* the DINPUT8.dll address called */
    uint32_t this_ptr;
    uint32_t slot;
    uint32_t calls;
    uint32_t last_ret;
    uint32_t first_ms;     /* ms of the first call */
    uint32_t a1, a2, a3;   /* arguments of the first call */
    uint32_t state_ptr;    /* the engine's state buffer, validated once */
    uint32_t state_cb;     /* cbData of the first call                  */
    uint32_t state8;       /* FIRST poll's bytes 0-3, packed            */
    uint32_t state8b;      /* FIRST poll's bytes 4-7, packed            */
    uint32_t state_or;     /* OR of every state byte of every poll      */
    uint32_t state_n;      /* polls sampled                             */
    uint32_t nz_logged;
    uint32_t logged;
} din_site;
static din_site g_din[DIN_MAX];
static volatile LONG g_din_n;
static volatile LONG g_din_total;

/* Module base + file name for an address, NULL when unmapped. */
static const char *module_of(uint32_t va, uint32_t *base, char *buf, size_t cap) {
    MEMORY_BASIC_INFORMATION mbi;
    char path[MAX_PATH];
    const char *slash;
    if (!VirtualQuery((const void *)(uintptr_t)va, &mbi, sizeof mbi)) return NULL;
    if (!mbi.AllocationBase) return NULL;
    *base = (uint32_t)(uintptr_t)mbi.AllocationBase;
    if (!GetModuleFileNameA((HMODULE)mbi.AllocationBase, path, MAX_PATH)) return NULL;
    slash = strrchr(path, '\\');
    slash = slash ? slash + 1 : path;
    _snprintf(buf, cap, "%s", slash);
    buf[cap - 1] = 0;
    return buf;
}

/* True when n dwords at va are inside one committed, readable region. */
static int readable(uint32_t va, size_t n) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!va || va + n < va) return 0;
    if (!VirtualQuery((const void *)(uintptr_t)va, &mbi, sizeof mbi)) return 0;
    if (mbi.State != MEM_COMMIT) return 0;
    if ((DWORD)mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return 0;
    return (uint32_t)(uintptr_t)mbi.BaseAddress + mbi.RegionSize >= va + n;
}

/* The engine's device vtable slot, when the table around `this` is readable.
 * This is the same structural trick perf_probe/draw_probe use: the entry, not
 * the table, lives in the module. Both the object and the table it points at
 * are range-checked: `this` is not always a COM object (DirectInput8Create's
 * first argument is the guest hmodule). */
static int vtable_slot(uint32_t this_ptr, uint32_t target) {
    uint32_t vt;
    int i;
    if (!readable(this_ptr, 4)) return -1;
    vt = *(const uint32_t *)(uintptr_t)this_ptr;
    if (!readable(vt, 40 * 4)) return -1;
    for (i = 0; i < 40; i++) {
        if (*(const uint32_t *)(uintptr_t)(vt + 4 * i) == target) return i;
    }
    return -1;
}

/* Sample the device's state buffer. Called on the site's first poll and on
 * every poll after it: the buffer is validated once (at first sighting, and
 * again only if the engine moves it), so the steady path is an 8-byte read and
 * eight ORs - no VirtualQuery, no stdio, no allocation - which keeps this
 * affordable inside a post hook that runs thousands of times a frame.
 *
 * `state_or` is the OR of every byte of every poll, the only form that can
 * answer "does a held key ever appear here": one sample taken before the key
 * went down says nothing. `state8`/`state8b` stay the FIRST poll, labelled. */
static void din_state_sample(din_site *s, const uint32_t *args) {
    const unsigned char *p;
    uint32_t k;
    if (s->slot != 9 && s->slot != 10) return;
    if (args[2] != s->state_ptr) {          /* first poll, or the buffer moved */
        if (!readable(args[2], 8)) return;
        s->state_ptr = args[2];
        s->state_cb = args[1];
        s->state8 = s->state8b = 0;
        s->state_n = 0;
    }
    s->state_n++;
    p = (const unsigned char *)(uintptr_t)s->state_ptr;
    for (k = 0; k < 8; k++) s->state_or |= p[k];
    if (s->state_n == 1) {
        s->state8  = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                     ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
        s->state8b = (uint32_t)p[4] | ((uint32_t)p[5] << 8) |
                     ((uint32_t)p[6] << 16) | ((uint32_t)p[7] << 24);
    }
}

static void din_record(uint32_t target, const uint32_t *args, uint32_t ret) {
    /* Same direct-mapped filter as kind_of: the post hook sees every bridged
     * call, and only a handful of targets live in DINPUT8.dll. Nothing here
     * prints: the hook runs under the machine lock on the guest's thread, so
     * stdio stays on the sampler thread (it prints the first sighting). */
    static uint32_t tag[8192], is_din[8192];
    uint32_t base = 0;
    uint32_t idx = (target >> 2) & 8191u;
    char mod[64];
    const char *m;
    din_site *s;
    int i, k, slot = -1;
    if (tag[idx] != target) {
        m = module_of(target, &base, mod, sizeof mod);
        tag[idx] = target;
        is_din[idx] = (m && !_stricmp(m, "DINPUT8.dll")) ? 1 : 0;
    }
    if (!is_din[idx]) return;
    InterlockedIncrement(&g_din_total);
    for (i = 0; i < g_din_n; i++) {
        if (g_din[i].target == target && g_din[i].this_ptr == args[0]) {
            g_din[i].calls++;
            g_din[i].last_ret = ret;
            /* Every poll, not just the first: until 2026-11-28 this returned
             * here, so state_or stayed the first sample forever and the
             * "NONZERO" line could never fire after t~1.5 s. */
            din_state_sample(&g_din[i], args);
            return;
        }
    }
    if (g_din_n >= DIN_MAX) return;
    slot = vtable_slot(args[0], target);
    s = &g_din[g_din_n];
    s->target = target;
    s->this_ptr = args[0];
    s->slot = (uint32_t)slot;
    s->calls = 1;
    s->last_ret = ret;
    s->first_ms = (uint32_t)now_ms();
    s->a1 = args[1]; s->a2 = args[2]; s->a3 = args[3];
    s->state_ptr = 0;
    s->state8 = s->state8b = s->state_or = 0;
    s->state_n = 0;
    s->state_cb = 0;
    s->nz_logged = 0;
    s->logged = 0;
    din_state_sample(s, args);
    g_din_n++;
}

/* ---------------------------------------------------------------- hooks */
/* A probe that installs itself later (draw_probe's watchdog re-installs its
 * post hook every 500 ms) can end up above this one while this one's saved
 * "previous" already points at it, which makes the chain a cycle. A re-entrant
 * call means exactly that, and running the probe once is enough. This is the
 * only definition of the guard (the file had a duplicate tentative one). */
static __declspec(thread) int t_depth;

static void sp_pre(uint32_t target, uint32_t *args) {
    uint32_t k;
    if (t_depth) return;          /* re-entry = a hook cycle; run once */
    t_depth++;
    if (!g_wait_on) { t_depth--; return; }
    k = kind_of(target);
    if (k == K_NONE) { t_depth--; return; }
    {
        uint32_t i = (uint32_t)InterlockedIncrement((volatile LONG *)&g_seq) - 1;
        sp_rec *r = &g_ring[i % RING];
        r->seq = i;
        r->tid = GetCurrentThreadId();
        r->kind = k;
        r->target = target;
        r->caller = g_cur_func;
        r->h = args[0];
        r->to = 0;
        if (k == K_WAIT1)      r->to = args[1];
        else if (k == K_WAITN) { r->h = args[1]; r->to = (args[0] & 0xFFu) | ((args[3] & 0xFu) << 8); }
        else if (k == K_REL)   r->to = args[1];
        else if (k == K_CREATEEVENT || k == K_CREATESEM || k == K_CREATEMUTEX)
            r->to = args[1];    /* flags / initial count+max */
        else if (k == K_CREATETHREAD) r->to = args[2];   /* start address */
        r->state = 0;
        r->ret = 0;
        r->ms = now_ms();
        t_pend = (int)(i % RING);
        t_pend_kind = k;
    }
    t_depth--;
}

static void sp_post(uint32_t target, const uint32_t *args, uint32_t ret) {
    if (t_depth) return;          /* see sp_pre */
    t_depth++;
    if (g_din_on) din_record(target, args, ret);
    if (!g_wait_on) { t_depth--; return; }
    if (t_pend >= 0) {
        sp_rec *r = &g_ring[t_pend];
        r->state = 1;
        r->ret = ret;
        if (is_create((uint32_t)t_pend_kind))
            obj_add(ret, r->kind, r->caller, r->to);
        else if (r->kind == K_SET || r->kind == K_RESET || r->kind == K_REL) {
            sp_obj *o = obj_find(r->h);
            if (o) { o->sets++; o->last_tid = r->tid; o->last_ms = now_ms(); }
        }
        t_pend = -1;
    }
    t_depth--;
}

/* ---------------------------------------------------------------- dump */
static uint32_t rd32(uint32_t va) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!va || va + 4 < va) return 0;
    if (!VirtualQuery((const void *)(uintptr_t)va, &mbi, sizeof mbi)) return 0;
    if (mbi.State != MEM_COMMIT) return 0;
    if ((DWORD)mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return 0;
    if ((uint32_t)(uintptr_t)mbi.BaseAddress + mbi.RegionSize < va + 4) return 0;
    return *(const volatile uint32_t *)(uintptr_t)va;
}

static void dump_once(const char *why) {
    uint32_t i, last, n = g_seq;
    unsigned pending = 0;
    fprintf(stderr, "[stall] %s\n", why);
    last = (n < RING) ? 0 : n - RING;
    for (i = last; i < n; i++) {
        sp_rec *r = &g_ring[i % RING];
        sp_obj *o;
        if (r->seq != i) continue;   /* stale slot */
        if (r->state) continue;
        o = obj_find(r->h);
        pending++;
        fprintf(stderr, "[stall] pending tid=%u %s h=%08X to=%08X caller=%08X"
                        " since=%llums creator=%08X%s\n",
                r->tid, kind_name(r->kind), r->h, r->to, r->caller,
                (unsigned long long)(now_ms() - r->ms),
                o ? o->creator : 0, o ? "" : " (object unknown)");
        if (o) {
            if (o->sets)
                fprintf(stderr, "[stall]   obj %08X: %u signal(s), last by tid=%u %llums ago"
                                " (created by %08X, kind %s flags=%08X)\n",
                        o->h, o->sets, o->last_tid,
                        (unsigned long long)(now_ms() - o->last_ms), o->creator,
                        kind_name(o->kind), o->flags);
            else
                fprintf(stderr, "[stall]   obj %08X: NEVER SIGNALLED (created by %08X, kind %s flags=%08X)\n",
                        o->h, o->creator, kind_name(o->kind), o->flags);
        }
    }
    fprintf(stderr, "[stall] %u wait(s) in flight; last 20 calls, newest first:\n", pending);
    for (i = n; i > last && i > n - 20; i--) {
        sp_rec *r = &g_ring[(i - 1) % RING];
        if (r->seq != i - 1) continue;
        fprintf(stderr, "[stall]   +%llums tid=%u %s h=%08X to=%08X caller=%08X -> %s%08X\n",
                (unsigned long long)r->ms, r->tid, kind_name(r->kind), r->h, r->to,
                r->caller, r->state ? "" : "PENDING ", r->ret);
    }
    fflush(stderr);
}

static DWORD WINAPI sp_thread(LPVOID param) {
    uint32_t mgr, t58 = 0, t5c = 0, seen58 = 0xFFFFFFFFu, seen5c = 0xFFFFFFFFu;
    uint64_t since = 0, next_din = 0;
    (void)param;
    for (;;) {
        Sleep(400);
        if (g_wait_on) {
            mgr = rd32(VA_FRAME_MGR);
            if (mgr) {
                t58 = rd32(mgr + 0x58);
                t5c = rd32(mgr + 0x5C);
                if (t58 != seen58 || t5c != seen5c) {
                    seen58 = t58; seen5c = t5c;
                    since = now_ms();
                } else if (!g_dumped && since && now_ms() - since >= g_stall_ms) {
                    char why[160];
                    _snprintf(why, sizeof why,
                              "freeze: frame tick 0x%08X/0x%08X unchanged for %llums at t=%.1fs",
                              t58, t5c, (unsigned long long)(now_ms() - since), now_ms() / 1000.0);
                    why[sizeof why - 1] = 0;
                    InterlockedExchange(&g_dumped, 1);
                    dump_once(why);
                }
            }
        }
        if (g_din_on && now_ms() >= next_din) {
            int i, n = g_din_n;
            next_din = now_ms() + 1000;
            for (i = 0; i < n; i++) {
                if (g_din[i].logged) continue;
                g_din[i].logged = 1;
                fprintf(stderr, "[dinput] t=%.1fs DINPUT8.dll#%d this=%08X ret=%08X"
                                " a1=%08X a2=%08X a3=%08X\n",
                        g_din[i].first_ms / 1000.0, (int)g_din[i].slot,
                        g_din[i].this_ptr, g_din[i].last_ret,
                        g_din[i].a1, g_din[i].a2, g_din[i].a3);
                if ((g_din[i].slot == 9 || g_din[i].slot == 10) && g_din[i].state_cb)
                    fprintf(stderr, "[dinput]   state cb=%u first8=%02X %02X %02X %02X"
                                    " %02X %02X %02X %02X polls=%u or8=%02X\n",
                            g_din[i].state_cb,
                            g_din[i].state8 & 0xFF, (g_din[i].state8 >> 8) & 0xFF,
                            (g_din[i].state8 >> 16) & 0xFF, (g_din[i].state8 >> 24) & 0xFF,
                            g_din[i].state8b & 0xFF, (g_din[i].state8b >> 8) & 0xFF,
                            (g_din[i].state8b >> 16) & 0xFF, (g_din[i].state8b >> 24) & 0xFF,
                            g_din[i].state_n, g_din[i].state_or & 0xFF);
                if ((g_din[i].slot == 9 || g_din[i].slot == 10) &&
                    g_din[i].state_or && !g_din[i].nz_logged) {
                    g_din[i].nz_logged = 1;
                    fprintf(stderr, "[dinput]   state became NONZERO at t=%.1fs"
                                    " (or8=0x%02X) on this=%08X\n",
                            now_ms() / 1000.0, g_din[i].state_or & 0xFF,
                            g_din[i].this_ptr);
                }
            }
            fprintf(stderr, "[dinput] t=%.0fs calls=%u:", now_ms() / 1000.0, (unsigned)g_din_total);
            for (i = 0; i < n; i++)
                fprintf(stderr, " site%d=%u(this=%08X slot=%d)",
                        i, g_din[i].calls, g_din[i].this_ptr, (int)g_din[i].slot);
            fprintf(stderr, "\n");
            fflush(stderr);
        }
    }
}

void oracle_probe_init(void) {
    const char *s;
    if (getenv("KORRA_STALLPROBE")) {
        g_wait_on = 1;
        s = getenv("KORRA_STALLPROBE_MS");
        if (s) g_stall_ms = (uint32_t)strtoul(s, NULL, 0);
    }
    if (getenv("KORRA_DINPUTPROBE")) g_din_on = 1;
    if (!g_wait_on && !g_din_on) return;
    now_ms();   /* set the epoch */
    if (g_wait_on) native32_pre_hook = sp_pre;
    native32_post_hook = sp_post;
    fprintf(stderr, "[probe] oracle: stall-ring=%d dinput-census=%d stall-ms=%u\n",
            g_wait_on, g_din_on, g_stall_ms);
    if (!CreateThread(NULL, 0, sp_thread, NULL, 0, NULL))
        fprintf(stderr, "[probe] oracle: sampler thread failed\n");
}

