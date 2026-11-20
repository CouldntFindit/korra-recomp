/*
 * perf_probe.c - where the first area's 298 ms frame goes.
 *
 * The defect this exists for: build 470ab9e renders the menus at ~59 fps and
 * the first playable area at a few fps (measured on the machine's overlay as
 * 3 fps / 298 ms). A one-second average cannot say *where* inside the frame the
 * time sits, so this probe reports per frame:
 *
 *   1. wall time    - changes of [frameMgr+0x5c], the limiter's own tick
 *                     (1/3-ms units), timed with QPC,
 *   2. CPU time     - GetProcessTimes delta (every guest thread), which
 *                     separates "burning cycles" from "blocked",
 *   3. native time  - QPC around every bridged Windows call, attributed to the
 *                     callee *and to the lifted caller* (g_cur_func at entry),
 *                     i.e. the wall time the frame really spends inside Win32,
 *   4. presents     - d3d9 device Present calls (vtable slot 17), which is the
 *                     number an fps overlay shows; the limiter tick is not the
 *                     same thing and must not be read as if it were,
 *   5. EIP samples  - the guest's main thread is suspended once per pass and its
 *                     EIP recorded, labelled with the bridged call in flight.
 *
 * Plus, once a second: a summary (frames/s, CPU%, presents/s, main-thread EIP
 * histogram, top native callees of that second) and a census of every thread in
 * the process, which is what shows who holds the runtime's machine lock while
 * the frame waits. The per-second callee list keeps working when the frame tick
 * stops advancing, which is exactly the state that has to be explained.
 *
 * How to read a per-frame line:
 *
 *   nat ~= dt                       a Windows call is the frame; the callee list
 *                                   names it and the guest function that made it.
 *   cpu ~= dt, nat small            the guest (or another thread) burns it.
 *   dt large, cpu/nat small, the
 *   census shows another thread
 *   in host+0x...                   the frame waits for the machine lock.
 *
 * Knobs:
 *   KORRA_PERF=1            on (inert otherwise)
 *   KORRA_PERF_FRAMES=N     frames that get a line (default 400; after that
 *                           only frames slower than KORRA_PERF_SLOW_MS)
 *   KORRA_PERF_SLOW_MS=N    slow-frame threshold, default 50
 *   KORRA_PERF_EIP_MS=N     sampler period, default 8 ms; 0 disables sampling
 *   KORRA_PERF_TOPS=N       entries per one-second summary, default 6
 *
 * The probe only reads guest memory and per-thread contexts. It never writes
 * guest memory and never takes the machine lock: a torn 32-bit read of a
 * timestamp is harmless, and holding the lock from a sampler would perturb the
 * thing being measured.
 *
 * It installs itself from main() rather than a .CRT$XIU initializer because it
 * needs a thread, and creating one before main faults (AGENTS.md ledger).
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <powrprof.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "native32.h"

/* The same addresses docs/quirks.md and quirk_probe.c use. */
#define VA_FRAME_MGR   0x00E4D060u   /* app / frame-manager singleton pointer */
#define VA_MODE        0x00E4D210u   /* DAT_00e4d210: 1 = 30 fps, else 60     */
#define VA_APP_FLAGS   0x00E4D01Cu   /* bit 2 = limiter disabled              */

#define HOST_BASE      0x60000000u   /* /FIXED /BASE:0x60000000, see build    */
#define FIRM_MAX       32
#define EIP_MAX        24
#define SLOT_CACHE     64

static int      g_on;
static DWORD    g_main_tid;
static HANDLE   g_main_thread;
static unsigned g_frame_budget = 400;
static unsigned g_slow_ms = 50;
static unsigned g_eip_ms = 8;
static unsigned g_tops = 6;

/* ------------------------------------------------------------ guest memory */
static int readable(uint32_t va, size_t n) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!va || va + n < va) return 0;
    if (!VirtualQuery((const void *)(uintptr_t)va, &mbi, sizeof mbi)) return 0;
    if (mbi.State != MEM_COMMIT) return 0;
    if ((DWORD)mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return 0;
    return (uint32_t)(uintptr_t)mbi.BaseAddress + mbi.RegionSize >= va + n;
}

static uint32_t rd32(uint32_t va) {
    return readable(va, 4) ? *(const volatile uint32_t *)(uintptr_t)va : 0xFFFFFFFFu;
}

/* ------------------------------------------------------------------ labels */
static const char *module_base_name(uint32_t va, uint32_t *module_base, char *name, size_t cap) {
    MEMORY_BASIC_INFORMATION mbi;
    char path[MAX_PATH];
    const char *slash;
    if (!VirtualQuery((const void *)(uintptr_t)va, &mbi, sizeof mbi)) return NULL;
    if (!mbi.AllocationBase) return NULL;
    *module_base = (uint32_t)(uintptr_t)mbi.AllocationBase;
    if (!GetModuleFileNameA((HMODULE)mbi.AllocationBase, path, MAX_PATH)) return NULL;
    slash = strrchr(path, '\\');
    slash = slash ? slash + 1 : path;
    _snprintf(name, cap, "%s", slash);
    name[cap - 1] = 0;
    return name;
}

/* An address as "module+0xRVA", with the guest image named "host". */
static const char *addr_label(uint32_t va, char *buf, size_t cap) {
    uint32_t base = 0;
    char mod[64];
    if (!va) { _snprintf(buf, cap, "?"); return buf; }
    if (!module_base_name(va, &base, mod, sizeof mod)) {
        _snprintf(buf, cap, "0x%08X", (unsigned)va);
    } else if (base == HOST_BASE) {
        _snprintf(buf, cap, "host+0x%X", (unsigned)(va - base));
    } else {
        _snprintf(buf, cap, "%s+0x%X", mod, (unsigned)(va - base));
    }
    buf[cap - 1] = 0;
    return buf;
}

/* The vtable slot of `target` in `this`'s vtable, or -1. COM methods have no
 * name in the runtime's table (only bridged imports do), so the slot is what
 * turns "an unnamed call that took 280 ms" into "d3d9.dll#17" (Present).
 * Cached per target: the slot never changes for a given method address. */
static struct { uint32_t target; int slot; } g_slot_cache[SLOT_CACHE];
static int g_slot_n;

static int vtable_slot(uint32_t this_ptr, uint32_t target) {
    uint32_t vt;
    int i;
    for (i = 0; i < g_slot_n; i++) if (g_slot_cache[i].target == target) return g_slot_cache[i].slot;
    i = -1;
    if (readable(this_ptr, 4)) {
        vt = *(const volatile uint32_t *)(uintptr_t)this_ptr;
        if (readable(vt, 4 * 128)) {
            for (int k = 0; k < 128; k++) {
                if (*(const volatile uint32_t *)(uintptr_t)(vt + 4 * k) == target) { i = k; break; }
            }
        }
    }
    if (g_slot_n < SLOT_CACHE) {
        g_slot_cache[g_slot_n].target = target;
        g_slot_cache[g_slot_n].slot = i;
        g_slot_n++;
    }
    return i;
}

/* ------------------------------------------------------- native call timing
 * pre_hook/post_hook run on the calling thread, with the machine held. Only the
 * guest's main thread matters: it is the one that runs the frame. */
typedef struct {
    uint32_t     target;
    uint32_t     caller;     /* g_cur_func at entry: the lifted caller */
    uint32_t     this_ptr;
    const char  *name;
    ULONGLONG    ticks;
    unsigned     calls;
} callee_t;

static callee_t  g_frame_callee[FIRM_MAX];      /* keyed by (target, caller) */
static int       g_frame_callee_n;

/* Aggregate by callee alone (the question "which Windows call ate the frame"
 * does not care who made it), plus the three longest single calls of the frame
 * with their caller. Without this, a frame with thousands of distinct
 * (callee, caller) pairs fills the table with dust and hides the real cost. */
typedef struct { uint32_t target; const char *name; ULONGLONG ticks; unsigned calls; } agg_t;
static agg_t     g_frame_agg[64];
static int       g_frame_agg_n;
static agg_t     g_period_agg[64];
static int       g_period_agg_n;
/* Other guest threads: the question "who holds the device / the engine's locks
 * while this frame's Present blocks" is answered by their native calls, not by
 * the main thread's. */
static agg_t     g_frame_oth[32];
static int       g_frame_oth_n;
static agg_t     g_period_oth[64];
static int       g_period_oth_n;
static ULONGLONG g_frame_oth_ticks;
static callee_t  g_worst[3];                    /* longest single calls, frame */

static ULONGLONG g_frame_native_ticks;
static unsigned  g_frame_native_calls;
static unsigned  g_frame_presents;
static unsigned  g_period_native_calls;
static unsigned  g_period_presents;

/* Per-thread, so every guest thread is timed without them racing on one
 * timestamp. g_in_native is the main thread's, for the EIP labels. */
static __declspec(thread) ULONGLONG t_native_t0;
static __declspec(thread) LONG      t_in_native;
static __declspec(thread) uint32_t  t_native_target;
static volatile LONG g_in_native;
static ULONGLONG     g_native_t0;
static uint32_t      g_native_target;

static native32_pre_hook_t  g_prev_pre;
static native32_post_hook_t g_prev_post;

/* Elapsed wall clock in the log, so a hitch can be lined up with anything else
 * that happened at that moment (a screenshot, a load, another process). */
static LARGE_INTEGER g_qpc_freq;
static ULONGLONG     g_t0_qpc;
static double elapsed_s(void) {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)((ULONGLONG)t.QuadPart - g_t0_qpc) / (double)g_qpc_freq.QuadPart;
}

static void callee_add(callee_t *arr, int *n, uint32_t target, uint32_t caller,
                       uint32_t this_ptr, const char *name, ULONGLONG ticks) {
    int i;
    for (i = 0; i < *n; i++) {
        if (arr[i].target == target && arr[i].caller == caller) {
            arr[i].ticks += ticks;
            arr[i].calls++;
            return;
        }
    }
    if (*n < FIRM_MAX) {
        arr[*n].target = target;
        arr[*n].caller = caller;
        arr[*n].this_ptr = this_ptr;
        arr[*n].name = name;
        arr[*n].ticks = ticks;
        arr[*n].calls = 1;
        (*n)++;
    }
}

static void callee_label(const callee_t *c, char *buf, size_t cap) {
    const char *nm = c->name;
    if (!nm) {
        char mod[64];
        uint32_t base = 0;
        const char *m = module_base_name(c->target, &base, mod, sizeof mod);
        int slot = vtable_slot(c->this_ptr, c->target);
        if (m && slot >= 0) _snprintf(buf, cap, "%s#%d", m, slot);
        else if (m) _snprintf(buf, cap, "%s+0x%X", m, (unsigned)(c->target - base));
        else _snprintf(buf, cap, "0x%08X", (unsigned)c->target);
    } else {
        const char *bang = strchr(nm, '!');
        _snprintf(buf, cap, "%s", bang ? bang + 1 : nm);
    }
    buf[cap - 1] = 0;
}

static void agg_add(agg_t *arr, int *n, int cap, uint32_t target, const char *name, ULONGLONG ticks) {
    int i;
    for (i = 0; i < *n; i++) {
        if (arr[i].target == target) { arr[i].ticks += ticks; arr[i].calls++; return; }
    }
    if (*n < cap) {
        arr[*n].target = target;
        arr[*n].name = name;
        arr[*n].ticks = ticks;
        arr[*n].calls = 1;
        (*n)++;
    }
}

static void worst_add(const callee_t *c) {
    int i, worst = -1;
    for (i = 0; i < 3; i++) if (g_worst[i].target == c->target && g_worst[i].caller == c->caller)
        { if (c->ticks > g_worst[i].ticks) g_worst[i] = *c; return; }
    for (i = 0; i < 3; i++) if (worst < 0 || g_worst[i].ticks < g_worst[worst].ticks) worst = i;
    if (g_worst[worst].ticks < c->ticks) g_worst[worst] = *c;
}

static void perf_pre(uint32_t target, uint32_t *args) {
    if (g_prev_pre) g_prev_pre(target, args);
    if (!g_on) return;
    if (t_in_native++ == 0) {
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        t_native_t0 = (ULONGLONG)t.QuadPart;
        t_native_target = target;
        if (GetCurrentThreadId() == g_main_tid) {
            g_in_native++;
            g_native_t0 = t_native_t0;
            g_native_target = target;
        }
    }
}

static void perf_post(uint32_t target, const uint32_t *args, uint32_t ret) {
    if (g_on && t_in_native > 0 && --t_in_native == 0) {
        LARGE_INTEGER t;
        ULONGLONG dt;
        uint32_t caller = g_cur_func;
        uint32_t this_ptr = args ? args[0] : 0;
        const char *name = native32_name(target);
        QueryPerformanceCounter(&t);
        dt = (ULONGLONG)t.QuadPart - t_native_t0;
        if (GetCurrentThreadId() == g_main_tid) {
            g_in_native--;
            g_frame_native_ticks += dt;
            g_frame_native_calls++;
            g_period_native_calls++;
            callee_add(g_frame_callee, &g_frame_callee_n, target, caller, this_ptr, name, dt);
            agg_add(g_frame_agg, &g_frame_agg_n, 64, target, name, dt);
            agg_add(g_period_agg, &g_period_agg_n, 64, target, name, dt);
            {
                callee_t c;
                c.target = target; c.caller = caller; c.this_ptr = this_ptr;
                c.name = name; c.ticks = dt; c.calls = 1;
                worst_add(&c);
            }
            /* d3d9 device Present: the fps an overlay reports. */
            if (!name && vtable_slot(this_ptr, target) == 17) {
                g_frame_presents++;
                g_period_presents++;
                if (dt * 1000.0 / (double)g_qpc_freq.QuadPart >= 50.0)
                    fprintf(stderr,
                            "[perf] present-slow %.1fms src=%08X dst=%08X hDestWnd=%08X dirty=%08X -> 0x%08X\n",
                            dt * 1000.0 / (double)g_qpc_freq.QuadPart,
                            args ? args[1] : 0, args ? args[2] : 0,
                            args ? args[3] : 0, args ? args[4] : 0, ret);
            }
        } else {
            /* Another guest thread: what holds the engine/device locks while
             * this frame waits. */
            g_frame_oth_ticks += dt;
            agg_add(g_frame_oth, &g_frame_oth_n, 32, target, name, dt);
            agg_add(g_period_oth, &g_period_oth_n, 64, target, name, dt);
        }
    }
    if (g_prev_post) g_prev_post(target, args, ret);
}

/* --------------------------------------------------------------- sampler */
typedef struct { uint32_t va; const char *native; unsigned hits; } eip_slot_t;

static eip_slot_t g_eip[EIP_MAX];
static int        g_eip_n;
static unsigned   g_eip_total;

static void eip_hit(uint32_t va) {
    int i;
    const char *native = NULL;
    if (g_in_native > 0) native = native32_name(g_native_target);
    for (i = 0; i < g_eip_n; i++) {
        if (g_eip[i].va == va && g_eip[i].native == native) { g_eip[i].hits++; g_eip_total++; return; }
    }
    if (g_eip_n < EIP_MAX) {
        g_eip[g_eip_n].va = va;
        g_eip[g_eip_n].native = native;
        g_eip[g_eip_n].hits = 1;
        g_eip_n++;
        g_eip_total++;
    }
}

/* Suspend/read/resume one thread. Never called on the calling thread: a thread
 * that suspends itself never resumes (that bug cost a whole run's data). */
static uint32_t thread_eip(HANDLE thread, DWORD tid) {
    CONTEXT ctx;
    DWORD eip = 0;
    if (!thread || tid == GetCurrentThreadId()) return 0;
    if (SuspendThread(thread) == (DWORD)-1) return 0;
    memset(&ctx, 0, sizeof ctx);
    ctx.ContextFlags = CONTEXT_CONTROL;
    if (GetThreadContext(thread, &ctx)) eip = (DWORD)ctx.Eip;
    ResumeThread(thread);
    return (uint32_t)eip;
}

static void print_eip_tops(int n) {
    char used[EIP_MAX];
    int printed = 0;
    memset(used, 0, sizeof used);
    while (printed < n) {
        int best = -1, i;
        for (i = 0; i < g_eip_n; i++) {
            if (used[i]) continue;
            if (best < 0 || g_eip[i].hits > g_eip[best].hits) best = i;
        }
        if (best < 0) break;
        {
            char buf[80];
            if (g_eip[best].native) {
                const char *bang = strchr(g_eip[best].native, '!');
                fprintf(stderr, "%snative:%s x%u", printed ? " | " : "",
                        bang ? bang + 1 : g_eip[best].native, g_eip[best].hits);
            } else {
                fprintf(stderr, "%s%s x%u", printed ? " | " : "",
                        addr_label(g_eip[best].va, buf, sizeof buf), g_eip[best].hits);
            }
        }
        used[best] = 1;
        printed++;
    }
    fprintf(stderr, "\n");
}

/* Every thread in the process, once: which one runs lifted code while the main
 * thread waits is the machine-lock question, answered directly. */
static void thread_census(void) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te;
    char buf[80];
    int printed = 0;
    DWORD me = GetCurrentThreadId();
    if (snap == INVALID_HANDLE_VALUE) return;
    te.dwSize = sizeof te;
    fprintf(stderr, "[perf] census:");
    if (Thread32First(snap, &te)) {
        do {
            HANDLE h;
            uint32_t eip;
            if (te.th32OwnerProcessID != GetCurrentProcessId()) continue;
            if (te.th32ThreadID == me) continue;
            h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, te.th32ThreadID);
            if (!h) continue;
            eip = thread_eip(h, te.th32ThreadID);
            CloseHandle(h);
            fprintf(stderr, " %s%d=%s", (te.th32ThreadID == g_main_tid) ? "*" : "",
                    (int)te.th32ThreadID, addr_label(eip, buf, sizeof buf));
            printed++;
        } while (printed < 24 && Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    fprintf(stderr, "\n");
}

static double qpc_ms(ULONGLONG ticks, LARGE_INTEGER freq) {
    return (double)ticks * 1000.0 / (double)freq.QuadPart;
}

/* Core 0's current clock. The first-area fps alternates between ~40 and ~58 in
 * a regular cycle with the same call count per frame, so throughput rather than
 * work varies; this is the cheap way to tell a CPU power state from a guest
 * periodic task. (PROCESSOR_POWER_INFORMATION is not declared by every SDK's
 * powrprof.h, so the layout is spelled out here.) */
typedef struct {
    ULONG Number;
    ULONG MaxMhz;
    ULONG CurrentMhz;
    ULONG MhzLimit;
    ULONG MaxIdleState;
    ULONG CurrentIdleState;
} korra_ppi;

static unsigned cpu_mhz(void) {
    korra_ppi info[64];
    if (CallNtPowerInformation((POWER_INFORMATION_LEVEL)11 /* ProcessorInformation */,
                               NULL, 0, info, sizeof info) != 0)
        return 0;
    return info[0].CurrentMhz;
}

static DWORD WINAPI perf_thread(LPVOID unused) {
    LARGE_INTEGER freq, now;
    ULONGLONG qpc_prev_frame, qpc_period, cpu_prev, cpu_period_base;
    FILETIME ct, et, kt, ut;
    uint32_t last_tick = 0, last_interval = 0, last_mode = 0, last_flags = 0;
    int have_tick = 0, have_cpu = 0;
    unsigned frames = 0, frame_no = 0, idle_seconds = 0;
    (void)unused;

    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    g_qpc_freq = freq;
    g_t0_qpc = (ULONGLONG)now.QuadPart;
    qpc_prev_frame = qpc_period = (ULONGLONG)now.QuadPart;
    cpu_prev = cpu_period_base = 0;
    if (GetProcessTimes(GetCurrentProcess(), &ct, &et, &kt, &ut))
        cpu_prev = cpu_period_base =
            ((ULONGLONG)ut.dwHighDateTime << 32 | ut.dwLowDateTime) +
            ((ULONGLONG)kt.dwHighDateTime << 32 | kt.dwLowDateTime);
    have_cpu = 1;

    for (;;) {
        uint32_t mgr, tick = 0, interval = 0, mode, flags;

        Sleep(g_eip_ms ? g_eip_ms : 1);
        QueryPerformanceCounter(&now);
        if (g_eip_ms && g_main_thread) eip_hit(thread_eip(g_main_thread, g_main_tid));

        mgr = rd32(VA_FRAME_MGR);
        if (mgr && mgr != 0xFFFFFFFFu && readable(mgr, 0x60)) {
            tick = *(const volatile uint32_t *)(uintptr_t)(mgr + 0x5C);
            interval = *(const volatile uint32_t *)(uintptr_t)(mgr + 0x58);
        }
        if (!have_tick) { have_tick = 1; last_tick = tick; qpc_prev_frame = (ULONGLONG)now.QuadPart; }

        if (tick != last_tick) {
            double dt_ms = qpc_ms((ULONGLONG)now.QuadPart - qpc_prev_frame, freq);
            double nat_ms = qpc_ms(g_frame_native_ticks, freq);
            double cpu_ms = 0.0;
            unsigned calls = g_frame_native_calls;
            int i;

            if (have_cpu && GetProcessTimes(GetCurrentProcess(), &ct, &et, &kt, &ut)) {
                ULONGLONG cpu = ((ULONGLONG)ut.dwHighDateTime << 32 | ut.dwLowDateTime) +
                                ((ULONGLONG)kt.dwHighDateTime << 32 | kt.dwLowDateTime);
                cpu_ms = (double)(cpu - cpu_prev) / 10000.0;  /* 100 ns units */
                cpu_prev = cpu;
            }

            mode = rd32(VA_MODE);
            flags = rd32(VA_APP_FLAGS);
            frame_no++;
            frames++;
            idle_seconds = 0;

            if (frame_no <= g_frame_budget || dt_ms >= (double)g_slow_ms) {
                int k;
                fprintf(stderr, "[perf] t=%.1fs f=%u dt=%.1fms cpu=%.1fms nat=%.1fms calls=%u present=%u int=%u mode=%u flags=0x%X",
                        elapsed_s(), frame_no, dt_ms, cpu_ms, nat_ms, calls, g_frame_presents,
                        interval, mode, flags);
                for (k = 0; k < 4; k++) {
                    int best = -1, j;
                    for (j = 0; j < g_frame_agg_n; j++) {
                        if (g_frame_agg[j].ticks == 0) continue;
                        if (best < 0 || g_frame_agg[j].ticks > g_frame_agg[best].ticks) best = j;
                    }
                    if (best < 0) break;
                    {
                        char lab[96];
                        callee_t c;
                        c.target = g_frame_agg[best].target; c.caller = 0; c.this_ptr = 0;
                        c.name = g_frame_agg[best].name; c.ticks = g_frame_agg[best].ticks;
                        callee_label(&c, lab, sizeof lab);
                        fprintf(stderr, " %s=%.1fms/x%u", lab,
                                qpc_ms(g_frame_agg[best].ticks, freq), g_frame_agg[best].calls);
                    }
                    g_frame_agg[best].ticks = 0;
                }
                /* The single longest call of the frame, with its guest caller:
                 * a 300 ms hitch is one call, not an average. */
                for (k = 0; k < 3; k++) {
                    char lab[96];
                    int best = -1, j;
                    for (j = 0; j < 3; j++) {
                        if (g_worst[j].ticks == 0) continue;
                        if (best < 0 || g_worst[j].ticks > g_worst[best].ticks) best = j;
                    }
                    if (best < 0) break;
                    callee_label(&g_worst[best], lab, sizeof lab);
                    fprintf(stderr, " worst:%s@0x%X=%.1fms", lab,
                            (unsigned)g_worst[best].caller, qpc_ms(g_worst[best].ticks, freq));
                    g_worst[best].ticks = 0;
                }
                /* What the other guest threads spent inside Win32 during this
                 * frame: if a worker held the device through a long D3D call,
                 * it is here. */
                if (g_frame_oth_n) {
                    int k;
                    double total = qpc_ms(g_frame_oth_ticks, freq);
                    fprintf(stderr, " oth=%.0fms", total);
                    for (k = 0; k < 3; k++) {
                        int best = -1, j;
                        for (j = 0; j < g_frame_oth_n; j++) {
                            if (g_frame_oth[j].ticks == 0) continue;
                            if (best < 0 || g_frame_oth[j].ticks > g_frame_oth[best].ticks) best = j;
                        }
                        if (best < 0) break;
                        {
                            char lab[96];
                            callee_t c;
                            c.target = g_frame_oth[best].target; c.caller = 0; c.this_ptr = 0;
                            c.name = g_frame_oth[best].name; c.ticks = g_frame_oth[best].ticks;
                            callee_label(&c, lab, sizeof lab);
                            fprintf(stderr, " %s=%.1fms/x%u", lab,
                                    qpc_ms(g_frame_oth[best].ticks, freq), g_frame_oth[best].calls);
                        }
                        g_frame_oth[best].ticks = 0;
                    }
                }
                fprintf(stderr, "\n");
                fflush(stderr);
            }

            g_frame_native_ticks = 0;
            g_frame_native_calls = 0;
            g_frame_presents = 0;
            g_frame_callee_n = 0;
            g_frame_agg_n = 0;
            g_frame_oth_n = 0;
            g_frame_oth_ticks = 0;
            for (i = 0; i < 3; i++) g_worst[i].ticks = 0;
            qpc_prev_frame = (ULONGLONG)now.QuadPart;
            last_tick = tick;
        } else if (qpc_ms((ULONGLONG)now.QuadPart - qpc_prev_frame, freq) >= 1000.0) {
            idle_seconds++;
            qpc_prev_frame = (ULONGLONG)now.QuadPart;
        }

        /* Config changes get their own line: the whole 30/60 switch lives in
         * DAT_00e4d210 plus the applied interval. */
        mode = rd32(VA_MODE);
        flags = rd32(VA_APP_FLAGS);
        if (interval != last_interval || mode != last_mode || flags != last_flags) {
            fprintf(stderr, "[perf] cfg int=%u mode=%u flags=0x%X\n", interval, mode, flags);
            fflush(stderr);
            last_interval = interval;
            last_mode = mode;
            last_flags = flags;
        }

        /* One summary line per second of wall clock. */
        if (qpc_ms((ULONGLONG)now.QuadPart - qpc_period, freq) >= 1000.0) {
            double secs = qpc_ms((ULONGLONG)now.QuadPart - qpc_period, freq) / 1000.0;
            double cpu_ms = 0.0;
            if (have_cpu && GetProcessTimes(GetCurrentProcess(), &ct, &et, &kt, &ut)) {
                ULONGLONG cpu = ((ULONGLONG)ut.dwHighDateTime << 32 | ut.dwLowDateTime) +
                                ((ULONGLONG)kt.dwHighDateTime << 32 | kt.dwLowDateTime);
                cpu_ms = (double)(cpu - cpu_period_base) / 10000.0;
                cpu_period_base = cpu;
            }
            fprintf(stderr, "[perf] t=%.0fs 1s tick=%u/s present=%u/s cpu=%.0f%% mhz=%u natcalls=%u eip: ",
                    elapsed_s(), (unsigned)(frames / secs + 0.5), g_period_presents,
                    cpu_ms * 100.0 / (secs * 1000.0), cpu_mhz(), g_period_native_calls);
            print_eip_tops((int)g_tops);
            if (idle_seconds) fprintf(stderr, "[perf] STALL tick unchanged for %us, native calls still %u/s\n",
                                      idle_seconds, g_period_native_calls);
            if (g_period_agg_n) {
                int k;
                fprintf(stderr, "[perf] top-nat(%us):", (unsigned)secs);
                for (k = 0; k < (int)g_tops; k++) {
                    int best = -1, j;
                    for (j = 0; j < g_period_agg_n; j++) {
                        if (g_period_agg[j].ticks == 0) continue;
                        if (best < 0 || g_period_agg[j].ticks > g_period_agg[best].ticks) best = j;
                    }
                    if (best < 0) break;
                    {
                        char lab[96];
                        callee_t c;
                        c.target = g_period_agg[best].target; c.caller = 0; c.this_ptr = 0;
                        c.name = g_period_agg[best].name; c.ticks = g_period_agg[best].ticks;
                        callee_label(&c, lab, sizeof lab);
                        fprintf(stderr, "%s%s=%.0fms/x%u", k ? " | " : "", lab,
                                qpc_ms(g_period_agg[best].ticks, freq), g_period_agg[best].calls);
                    }
                    g_period_agg[best].ticks = 0;
                }
                fprintf(stderr, "\n");
            }
            if (g_period_oth_n) {
                int k;
                fprintf(stderr, "[perf] oth-nat(%us):", (unsigned)secs);
                for (k = 0; k < 6; k++) {
                    int best = -1, j;
                    for (j = 0; j < g_period_oth_n; j++) {
                        if (g_period_oth[j].ticks == 0) continue;
                        if (best < 0 || g_period_oth[j].ticks > g_period_oth[best].ticks) best = j;
                    }
                    if (best < 0) break;
                    {
                        char lab[96];
                        callee_t c;
                        c.target = g_period_oth[best].target; c.caller = 0; c.this_ptr = 0;
                        c.name = g_period_oth[best].name; c.ticks = g_period_oth[best].ticks;
                        callee_label(&c, lab, sizeof lab);
                        fprintf(stderr, "%s%s=%.1fms/x%u", k ? " | " : "", lab,
                                qpc_ms(g_period_oth[best].ticks, freq), g_period_oth[best].calls);
                    }
                    g_period_oth[best].ticks = 0;
                }
                fprintf(stderr, "\n");
            }
            thread_census();
            fflush(stderr);
            frames = 0;
            g_eip_n = 0;
            g_eip_total = 0;
            g_period_agg_n = 0;
            g_period_oth_n = 0;
            g_period_native_calls = 0;
            g_period_presents = 0;
            qpc_period = (ULONGLONG)now.QuadPart;
        }
    }
    return 0;
}

void korra_perf_probe_init(void) {
    const char *v;
    if (!getenv("KORRA_PERF")) return;
    if ((v = getenv("KORRA_PERF_FRAMES")) != NULL) {
        long n = strtol(v, NULL, 0);
        if (n > 0) g_frame_budget = (unsigned)n;
    }
    if ((v = getenv("KORRA_PERF_SLOW_MS")) != NULL) {
        long n = strtol(v, NULL, 0);
        if (n > 0) g_slow_ms = (unsigned)n;
    }
    if ((v = getenv("KORRA_PERF_EIP_MS")) != NULL) {
        long n = strtol(v, NULL, 0);
        if (n >= 0 && n < 1000) g_eip_ms = (unsigned)n;
    }
    if ((v = getenv("KORRA_PERF_TOPS")) != NULL) {
        long n = strtol(v, NULL, 0);
        if (n > 0 && n <= EIP_MAX) g_tops = (unsigned)n;
    }

    g_main_tid = GetCurrentThreadId();
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                    &g_main_thread, THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, 0);
    g_on = 1;

    if (native32_pre_hook && native32_pre_hook != perf_pre)  g_prev_pre  = native32_pre_hook;
    if (native32_post_hook && native32_post_hook != perf_post) g_prev_post = native32_post_hook;
    native32_pre_hook = perf_pre;
    native32_post_hook = perf_post;

    if (!CreateThread(NULL, 0, perf_thread, NULL, 0, NULL))
        fprintf(stderr, "[perf] sampler thread failed; native timing still on\n");
    fprintf(stderr, "[perf] perf probe on (frames<=%u or dt>=%ums, eip every %ums)\n",
            g_frame_budget, g_slow_ms, g_eip_ms);
    fflush(stderr);
}
