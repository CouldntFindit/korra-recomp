/*
 * quirk_probe.c - the measurements docs/quirks.md section 8 asks for, taken
 * from inside a running build instead of guessed at.
 *
 * M2.13 wants three answers before any frame-rate patch is written:
 *
 *   1. the live value of DAT_00e4d210 (0xe4d210) while the window is focused
 *      - `1` means 30 fps, anything else 60 fps, per FUN_0074c580,
 *   2. the limiter interval [frameMgr+0x58] (1/3 ms units: 100 = 30 fps,
 *      50 = 60 fps) and the rate the loop actually runs at,
 *   3. whether the simulation step is tied to that interval, i.e. whether
 *      60 fps doubles the logic rate.
 *
 * The probe is a reader only: it samples guest data through the mapped image
 * on its own thread and writes to stderr. It never writes guest memory and
 * never takes the machine lock (a torn 32-bit read of a timestamp is
 * harmless, and holding the lock from a sampler would perturb the timing it
 * exists to measure).
 *
 * Enable with KORRA_QUIRK_PROBE=1. Sample period 4 ms; a line per second:
 *
 *   [probe] focus=1 mode=0 applied=0 interval=50 tick=1234567 frames=60.1/s
 *           mean=16.6ms min=16.0ms max=17.4ms flags=0x00000004 hwnd=0x... =0x...
 *
 * frames/s counts changes of [frameMgr+0x5c], which the limiter rewrites every
 * frame; mean/min/max are the wall-clock gaps between those changes.
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "native32.h"

#define VA_MODE        0x00E4D210u   /* DAT_00e4d210: 1 = 30 fps, else 60 */
#define VA_MODE_APPL   0x00E4D214u   /* applied-mode cache */
#define VA_APP_FLAGS   0x00E4D01Cu   /* bit 2 = limiter disabled */
#define VA_FRAME_MGR   0x00E4D060u   /* app / frame-manager singleton pointer */
#define VA_HWND        0x00E4E52Cu   /* the HWND global D3D + DInput read */

static int probe_readable(const void *p, size_t bytes) {
    MEMORY_BASIC_INFORMATION mbi;
    const char *c = (const char *)p;
    if (!p || (uintptr_t)p < 0x10000) return 0;
    if (!VirtualQuery(c, &mbi, sizeof mbi)) return 0;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return 0;
    return (size_t)((const char *)mbi.BaseAddress + mbi.RegionSize - c) >= bytes;
}

static uint32_t rd32(uint32_t va) {
    if (!probe_readable((const void *)(uintptr_t)va, sizeof(uint32_t))) return 0xFFFFFFFFu;
    return *(const volatile uint32_t *)(uintptr_t)va;
}

static int window_focused(void) {
    DWORD pid = 0;
    HWND fg = GetForegroundWindow();
    if (!fg) return 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

static DWORD WINAPI probe_thread(LPVOID unused) {
    LARGE_INTEGER freq, t, prev_change;
    uint32_t last_tick = 0;
    unsigned frames = 0;
    double sum_ms = 0.0, min_ms = 1e9, max_ms = 0.0;
    int have_prev = 0;
    (void)unused;

    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&prev_change);
    last_tick = 0;

    for (;;) {
        uint32_t mgr, tick = 0, interval = 0, mode, applied, flags, hwnd;
        int focused;

        Sleep(4);
        QueryPerformanceCounter(&t);

        mgr = rd32(VA_FRAME_MGR);
        if (mgr && mgr != 0xFFFFFFFFu && probe_readable((const void *)(uintptr_t)mgr, 0x60)) {
            tick = *(const volatile uint32_t *)(uintptr_t)(mgr + 0x5C);
            interval = *(const volatile uint32_t *)(uintptr_t)(mgr + 0x58);
        }
        if (tick != last_tick) {
            if (have_prev) {
                double ms = (double)(t.QuadPart - prev_change.QuadPart) * 1000.0 / (double)freq.QuadPart;
                if (ms < min_ms) min_ms = ms;
                if (ms > max_ms) max_ms = ms;
                sum_ms += ms;
            }
            frames++;
            prev_change = t;
            last_tick = tick;
            have_prev = 1;
        }

        /* One line per second of wall clock. */
        {
            static LARGE_INTEGER window_start;
            static int started;
            if (!started) { window_start = t; started = 1; }
            if ((double)(t.QuadPart - window_start.QuadPart) / (double)freq.QuadPart >= 1.0) {
                double secs = (double)(t.QuadPart - window_start.QuadPart) / (double)freq.QuadPart;
                mode = rd32(VA_MODE);
                applied = rd32(VA_MODE_APPL);
                flags = rd32(VA_APP_FLAGS);
                hwnd = rd32(VA_HWND);
                focused = window_focused();
                fprintf(stderr,
                        "[probe] focus=%d mode=%u%s applied=%u interval=%u%s "
                        "frames=%.1f/s mean=%.1fms min=%.1fms max=%.1fms flags=0x%08X hwnd=0x%08X\n",
                        focused, mode, mode == 1u ? "(30fps)" : "(60fps)",
                        applied, interval,
                        interval == 100u ? "(30fps)" : interval == 50u ? "(60fps)" : "(?)",
                        frames / secs,
                        frames ? sum_ms / frames : 0.0,
                        min_ms < 1e8 ? min_ms : 0.0,
                        max_ms,
                        flags, hwnd);
                fflush(stderr);
                window_start = t;
                frames = 0;
                sum_ms = 0.0; min_ms = 1e9; max_ms = 0.0;
            }
        }
    }
    return 0;
}

void korra_quirk_probe_init(void) {
    if (!getenv("KORRA_QUIRK_PROBE")) return;
    if (CreateThread(NULL, 0, probe_thread, NULL, 0, NULL))
        fprintf(stderr, "[probe] quirk probe running (4 ms samples, one line per second)\n");
}

/* ------------------------------------------------------------ file tracing
 * KORRA_TRACE_FILE=1 logs every CreateFileA/W with all seven arguments and the
 * result. The native trace prints four, and the three it drops are the ones
 * that decide a failure (dwCreationDisposition, flags/attributes, template).
 *
 * Used on the M2 stall: the title screen stops loading with no file I/O at all,
 * and the last opens in the log are GameData\data001.cpk (NULL) plus the four
 * loose sound files and movie_logo\Nickelodeon.usm - files that demonstrably
 * exist. Either the guest's arguments are wrong or the failure is deliberate
 * and the loader is waiting on something else.
 */
static int g_file_calls;

/* The pre-hook and the post-hook print separate lines, and a second thread's
 * call can land between them - the same trap that made a CreateFileA line look
 * like it returned 0. The post-hook has the argument slots as well, so the
 * whole record is printed there, in one write. */
static int is_create_file(const char *name) {
    return name && (strstr(name, "CreateFileA") || strstr(name, "CreateFileW"));
}

static void file_post_hook(uint32_t target, const uint32_t *args, uint32_t ret) {
    const char *name = native32_name(target);
    char line[512];
    int n;
    if (!is_create_file(name)) return;
    if (g_file_calls++ > 64) return;
    n = _snprintf(line, sizeof line, "[file] %s access=0x%08X share=%u disp=%u flags=0x%08X",
                  name, args[1], args[2], args[4], args[5]);
    if (n > 0 && name[strlen(name) - 1] == 'A') {
        const char *path = (const char *)(uintptr_t)args[0];
        MEMORY_BASIC_INFORMATION mbi;
        if (path && VirtualQuery(path, &mbi, sizeof mbi) && mbi.State == MEM_COMMIT &&
            !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
            _snprintf(line + n, sizeof line - n, " path=\"%s\"", path);
            n = (int)strlen(line);
            /* The interesting case is whether the save/profile path really
             * exists: an open that reports a handle for a directory that is not
             * on disk is a trace artefact, not a success. */
            if (strstr(path, "Korra") || strstr(path, "SystemData")) {
                DWORD attr = GetFileAttributesA(path);
                _snprintf(line + n, sizeof line - n, " attrs=%s",
                          attr == INVALID_FILE_ATTRIBUTES ? "MISSING" : "present");
            }
        }
    }
    fprintf(stderr, "%s -> handle=0x%08X GetLastError=%lu\n", line, ret, GetLastError());
    fflush(stderr);
}

/* ------------------------------------------------ failed D3D9/D3DX results
 * KORRA_TRACE_D3D=1 logs every d3d9/d3dx call that does not return S_OK. This
 * is the cheapest way to tell "the engine never asked" from "the engine asked
 * and the device said no": a missing menu font or a rejected shader shows up as
 * one of these lines, with the calling guest function right next to it. */
static void d3d_post_hook(uint32_t target, const uint32_t *args, uint32_t ret) {
    const char *name = native32_name(target);
    if (!name) return;
    /* D3DX is rare (four imported functions), so every call is worth a line;
     * d3d9 is called thousands of times a frame, so only its failures are. */
    if (strstr(name, "d3dx9_43.dll!")) {
        fprintf(stderr, "[d3dx] %s (a0=%08X a1=%08X a2=%08X a3=%08X a4=%08X a5=%08X) from 0x%08X -> 0x%08X\n",
                name, args[0], args[1], args[2], args[3], args[4], args[5], g_cur_func, ret);
        fflush(stderr);
        return;
    }
    if (ret == 0) return;
    if (!strstr(name, "d3d9.dll!")) return;
    fprintf(stderr, "[d3d] %s (%08X %08X %08X %08X) from 0x%08X -> 0x%08X\n",
            name, args[0], args[1], args[2], args[3], g_cur_func, ret);
    fflush(stderr);
}

static void combined_post_hook(uint32_t target, const uint32_t *args, uint32_t ret) {
    static int d3d_on = -1;
    if (d3d_on < 0) d3d_on = getenv("KORRA_TRACE_D3D") != NULL;
    if (d3d_on) d3d_post_hook(target, args, ret);
    if (getenv("KORRA_TRACE_FILE")) file_post_hook(target, args, ret);
}

void korra_file_probe_init(void) {
    if (!getenv("KORRA_TRACE_FILE") && !getenv("KORRA_TRACE_D3D")) return;
    native32_post_hook = combined_post_hook;
    fprintf(stderr, "[file] native diagnostics on (files=%d, d3d failures=%d)\n",
            getenv("KORRA_TRACE_FILE") != NULL, getenv("KORRA_TRACE_D3D") != NULL);
}

/* ----------------------------------------------------------- windowed mode
 * KORRA_WINDOWED=1 rewrites the engine's D3DPRESENT_PARAMETERS to windowed just
 * before IDirect3D9::CreateDevice/Reset. An exclusive fullscreen swapchain
 * hijacks the desktop, steals focus from everything else and makes a
 * CopyFromScreen capture unreliable; a window is what a test harness wants, and
 * the engine's own setting lives in the registry, not in the game folder.
 *
 * D3DPRESENT_PARAMETERS: Windowed at +0x20, FullScreen_RefreshRateInHz at
 * +0x30, PresentationInterval at +0x34 (0x38 bytes, the size the engine memsets
 * in FUN_00758e40). */
#define D3DPP_WINDOWED        0x20u
#define D3DPP_REFRESH_HZ      0x30u
#define D3DPP_INTERVAL        0x34u
#define D3DPRESENT_INTERVAL_IMMEDIATE 0x80000000u

static int is_create_device(const char *name) {
    return name && (strstr(name, "d3d9.dll!CreateDevice") || strstr(name, "d3d9.dll!Reset"));
}

static void windowed_pre_hook(uint32_t target, uint32_t *args) {
    const char *name = native32_name(target);
    uint32_t params;
    MEMORY_BASIC_INFORMATION mbi;
    if (!is_create_device(name)) return;
    /* CreateDevice: this, adapter, type, hFocus, flags, pParams, ppDevice
     * Reset:        this, pParams, ppDevice */
    params = strstr(name, "Reset") ? args[1] : args[5];
    if (!params || !VirtualQuery((const void *)(uintptr_t)params, &mbi, sizeof mbi) ||
        mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
        return;
    if ((size_t)((const char *)mbi.BaseAddress + mbi.RegionSize - (const char *)(uintptr_t)params) < 0x38)
        return;
    *(uint32_t *)(uintptr_t)(params + D3DPP_WINDOWED) = 1;
    *(uint32_t *)(uintptr_t)(params + D3DPP_REFRESH_HZ) = 0;
    *(uint32_t *)(uintptr_t)(params + D3DPP_INTERVAL) = D3DPRESENT_INTERVAL_IMMEDIATE;
    fprintf(stderr, "[host] %s: forced windowed (%ux%u backbuffer)\n", name,
            *(uint32_t *)(uintptr_t)params, *(uint32_t *)(uintptr_t)(params + 4));
    fflush(stderr);
}

/* The engine creates its window as a popup at desktop size; make it a real
 * window once it exists, and keep it there (the engine rewrites the style when
 * it changes display mode). */
static HWND g_game_window;

static BOOL CALLBACK find_window_proc(HWND h, LPARAM l) {
    DWORD pid = 0;
    RECT r;
    (void)l;
    GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(h)) return TRUE;
    if (!GetWindowRect(h, &r) || (r.right - r.left) < 200 || (r.bottom - r.top) < 150) return TRUE;
    g_game_window = h;
    return FALSE;
}

static DWORD WINAPI windowed_thread(LPVOID unused) {
    (void)unused;
    for (;;) {
        Sleep(1000);
        /* The engine destroys and re-creates its window when the display mode
         * changes, so a handle kept from the first find can go stale. */
        if (g_game_window && !IsWindow(g_game_window)) g_game_window = NULL;
        if (!g_game_window) {
            EnumWindows(find_window_proc, 0);
            if (g_game_window) {
                fprintf(stderr, "[host] windowed: game window 0x%p moved to 1280x720\n",
                        (void *)g_game_window);
                fflush(stderr);
            }
        }
        if (g_game_window) {
            RECT r;
            if (GetWindowRect(g_game_window, &r) &&
                (r.right - r.left != 1280 + 16 || r.bottom - r.top != 720 + 39))
                SetWindowPos(g_game_window, HWND_TOP, 40, 40, 1280 + 16, 720 + 39, SWP_NOACTIVATE);
        }
    }
    return 0;
}

void korra_windowed_init(void) {
    if (!getenv("KORRA_WINDOWED")) return;
    native32_pre_hook = windowed_pre_hook;
    CreateThread(NULL, 0, windowed_thread, NULL, 0, NULL);
    fprintf(stderr, "[host] windowed mode forced for CreateDevice/Reset\n");
}


/* ------------------------------------------------------------ message trace
 * KORRA_TRACE_MSG=1 logs the engine's message dispatcher: the id (ECX) and the
 * two stack arguments, one line per message. The M2 new-game crash needs to
 * know whether message 0x710 - the one handler that creates the game manager at
 * 0xCA3E58 - is ever dispatched, and with what payload.
 *
 * Needs the -DRECOMP_TRACE build (scripts\build_host.ps1 -Trace): the hook rides
 * on recomp_trace_extra, which RECOMP_ENTER calls on every lifted function.
 * In a normal build the pointer is never called, so this is inert there. */
#define KORRA_MSG_DISPATCH 0x0058DAB0u
static unsigned g_msg_count;

static void msg_trace_hook(uint32_t va) {
    if (va != KORRA_MSG_DISPATCH) return;
    if (g_msg_count++ < 4000)
        fprintf(stderr, "[msg] id=0x%08X a0=0x%08X a1=0x%08X\n",
                g_ecx, MEM32(g_esp + 8), MEM32(g_esp + 0xC));
}

void korra_msg_probe_init(void) {
    extern void (*recomp_trace_extra)(uint32_t va);
    if (!getenv("KORRA_TRACE_MSG")) return;
    recomp_trace_extra = msg_trace_hook;
    fprintf(stderr, "[msg] dispatcher trace on (0x%08X)\n", KORRA_MSG_DISPATCH);
}

/* --------------------------------------------------- new-game crash timeline
 * KORRA_TRACE_CRASH=1 prints one ordered timeline of the values that decide the
 * new-game crash, instead of the seven separate runs it took to collect them by
 * hand. Two halves, because the two questions are different shapes:
 *
 *  - --poison reports a change between two function entries, so it silently
 *    misses an A -> B -> A round trip inside one frame. Here a set of dwords is
 *    sampled at every entry and any difference is printed with the function that
 *    is being entered, so the round trip is visible.
 *  - the named sites print the registers that matter at the point they are
 *    live: ECX at the step dispatcher is the step id, and the globals around it
 *    are the stage mode, the UI visibility word and the published player.
 *
 * The crash is the HUD widget reading 0xCA3E58 when it is null, and 0xCA3E58 is
 * written only by the stage publish in 0x0058DAB0, so the timeline answers
 * whether the publish ran, with which stage mode, and whether the player it
 * looked for was in the scene. Nothing here changes target state.
 *
 * Needs -DRECOMP_TRACE (scripts\build_host.ps1 -Trace). */
#define CRASH_WATCH_MAX 10
#define CRASH_LOG_MAX   80

static struct { uint32_t addr, last; unsigned count; } g_cw[CRASH_WATCH_MAX];
static unsigned g_cw_n;

/* 0xCA3E58 and friends only exist once the guest image is mapped, and
 * RECOMP_ENTER fires before that (the CRT's first entries are at low VAs), so
 * nothing is sampled until a real image address is entered. */
static int g_crash_live;

static unsigned g_site_calls[28];

static int crash_site_budget(int slot, unsigned cap) {
    if (g_site_calls[slot]++ >= cap) return 0;
    return 1;
}

static void crash_watch(uint32_t addr) {
    if (g_cw_n < CRASH_WATCH_MAX) {
        g_cw[g_cw_n].addr = addr;
        g_cw[g_cw_n].last = 0;
        g_cw[g_cw_n].count = 0;
        g_cw_n++;
    }
}

/* The step name is a C string in a fixed buffer; the sequence machine compares
 * it against literal stage names, so printing it turns a bare id into a name. */
static void print_step_name(void) {
    char buf[40];
    unsigned i;
    for (i = 0; i < sizeof buf - 1; i++) {
        char c = (char)MEM8(0xE38A8C + i);
        buf[i] = c;
        if (!c) break;
    }
    buf[sizeof buf - 1] = 0;
    fprintf(stderr, " name=\"%s\"", buf);
}

static void crash_site(const char *what, uint32_t va, int with_step, int with_name) {
    fprintf(stderr, "[crash] %-13s va=0x%08X", what, va);
    if (with_step) fprintf(stderr, " step=0x%08X", g_ecx);
    fprintf(stderr, " player=0x%08X ui=0x%08X mode=%u phase=%u",
            MEM32(0xCA3E58), MEM32(0xE381F8), MEM32(0xCA3AC0), MEM32(0xE562E4));
    if (with_name) print_step_name();
    fprintf(stderr, "\n");
}

static void crash_probe_hook(uint32_t va) {
    unsigned i;

    if (!g_crash_live) {
        if (va < 0x00400000u) return;
        g_crash_live = 1;
        /* First live sample defines the baseline: a zero-initialised `last`
         * would report a bogus transition for every address. */
        for (i = 0; i < g_cw_n; i++) g_cw[i].last = MEM32(g_cw[i].addr);
        fprintf(stderr, "[crash] baseline player=0x%08X ui=0x%08X mode=%u phase=%u step=0x%08X\n",
                MEM32(0xCA3E58), MEM32(0xE381F8), MEM32(0xCA3AC0),
                MEM32(0xE562E4), MEM32(0xE38A84));
        return;
    }

    for (i = 0; i < g_cw_n; i++) {
        uint32_t v = MEM32(g_cw[i].addr);
        if (v == g_cw[i].last) continue;
        if (g_cw[i].count++ < CRASH_LOG_MAX) {
            fprintf(stderr, "[crash] 0x%08X: 0x%08X -> 0x%08X (entry 0x%08X)\n",
                    g_cw[i].addr, g_cw[i].last, v, va);
            if (g_cw[i].addr == 0xE38A84 || g_cw[i].addr == 0xE38AB0) print_step_name();
            fprintf(stderr, "\n");
        }
        g_cw[i].last = v;
    }

    switch (va) {
    case 0x0053BE60u:  /* app/sequence state machine */
        if (crash_site_budget(0, 120)) crash_site("seq", va, 0, 1);
        break;
    case 0x0058DAB0u:  /* stage publish: writes 0xCA3E58 */
        if (crash_site_budget(1, 120)) crash_site("publish", va, 1, 1);
        break;
    case 0x0058EE10u:  /* stage UI setup, returns early when player is null */
        if (crash_site_budget(2, 40)) crash_site("stage-ui", va, 0, 0);
        break;
    case 0x0058E730u:  /* stage release: clears 0xCA3E58 */
        if (crash_site_budget(3, 20)) crash_site("release", va, 0, 0);
        break;
    case 0x0058DC70u:  /* per-step UI visibility flags */
        if (crash_site_budget(4, 120)) crash_site("ui-flags", va, 1, 0);
        break;
    case 0x00541FB0u:  /* queues a step id */
        if (crash_site_budget(5, 80)) crash_site("queue-step", va, 1, 0);
        break;
    case 0x00541870u:  /* movie/cutscene machine */
        if (crash_site_budget(6, 10)) crash_site("movie", va, 0, 0);
        break;
    case 0x00602D80u:  /* UIPlHpBendingIcon ctor */
        if (crash_site_budget(7, 20)) crash_site("hud-ctor", va, 0, 0);
        break;
    case 0x00603C00u:  /* UIPlHpBendingIcon update: reads 0xCA3E58 */
        if (crash_site_budget(8, 20)) crash_site("hud-update", va, 0, 0);
        break;
    case 0x00503BD0u:  /* player spawn request */
        if (crash_site_budget(9, 20)) crash_site("player-spawn", va, 0, 0);
        break;
    case 0x005201D0u:  /* the "Player" scene-object lookup itself */
        if (crash_site_budget(10, 40))
            fprintf(stderr, "[crash] scene-lookup va=0x005201D0 mode=%u\n", MEM32(0xCA3AC0));
        break;
    case 0x0040F6F0u:  /* publish tail: only reached when the object was non-null */
        if (crash_site_budget(11, 40))
            fprintf(stderr, "[crash] publish-tail va=0x0040F6F0 player=0x%08X\n", MEM32(0xCA3E58));
        break;
    /* The lookup is create-if-missing: 0x520230 is its body, 0x73E700 the pool
     * factory it calls, 0x51F460 the new object's field init, 0x51F4B0 fills it
     * from the resource and returns 0 to reject, and 0x521C80 links it into the
     * cache. Printing them in order says which step gives up. */
    case 0x00520230u: {
        uint32_t req = MEM32(g_esp + 4);
        if (crash_site_budget(12, 40))
            fprintf(stderr, "[crash] lookup-body req=0x%08X name=0x%08X id=0x%08X\n",
                    req, req ? MEM32(req) : 0, req ? MEM32(req + 4) : 0);
        break;
    }
    case 0x0073E700u:
        if (crash_site_budget(13, 40))
            fprintf(stderr, "[crash] factory va=0x0073E700 pool=0x%08X\n", g_ecx);
        break;
    case 0x0051F460u:
        if (crash_site_budget(14, 40))
            fprintf(stderr, "[crash] obj-init va=0x0051F460 obj=0x%08X\n", g_ecx);
        break;
    case 0x0051F4B0u:
        if (crash_site_budget(15, 40))
            fprintf(stderr, "[crash] obj-fill va=0x0051F4B0 obj=0x%08X\n", g_ecx);
        break;
    case 0x00521C80u:
        if (crash_site_budget(16, 40))
            fprintf(stderr, "[crash] cache-link va=0x00521C80\n");
        break;
    case 0x0051F8B0u:
        if (crash_site_budget(17, 40))
            fprintf(stderr, "[crash] obj-reject va=0x0051F8B0 obj=0x%08X\n", g_ecx);
        break;
    /* The activation that rejects the player: 0x492D90 is BehaviorPlayer vtable
     * slot 8, and its first two gates are 0x4FB4C0 then 0x4E85A0. Whichever of
     * those is the last one entered names the failing subsystem. */
    case 0x00492D90u:
        if (crash_site_budget(18, 30))
            fprintf(stderr, "[crash] activate va=0x00492D90 obj=0x%08X\n", g_ecx);
        break;
    case 0x004FB4C0u:
        if (crash_site_budget(19, 30))
            fprintf(stderr, "[crash] gate1 va=0x004FB4C0 obj=0x%08X\n", g_ecx);
        break;
    case 0x004E85A0u:
        if (crash_site_budget(20, 30))
            fprintf(stderr, "[crash] gate2 va=0x004E85A0 obj=0x%08X\n", g_ecx);
        break;
    /* gate2 is Find loaded asset "pl\pl00f0.dat" -> 0. Print every path it is
     * asked for, so the log shows whether the asset was ever requested (and so
     * whether a load failed) or only looked up at activation time. */
    case 0x006AAB10u: {
        /* first stack argument is at esp+4: the call pushed the return address */
        uint32_t p = MEM32(g_esp + 4);
        if (crash_site_budget(21, 120) && p >= 0x00400000u && p < 0x01000000u) {
            char buf[80];
            unsigned k;
            for (k = 0; k < sizeof buf - 1; k++) {
                char c = (char)MEM8(p + k);
                buf[k] = c;
                if (!c) break;
            }
            buf[sizeof buf - 1] = 0;
            fprintf(stderr, "[crash] asset-lookup \"%s\"\n", buf);
        }
        break;
    }
    default:
        break;
    }
}

/* A clean early quit is the one failure mode the fault report cannot explain:
 * ExitProcess takes the process down with code 0 and no stack. The entry ring
 * still holds the last calls, so print it on the way out. */
static void crash_exit_hook(uint32_t target, uint32_t *args) {
    const char *name = native32_name(target);
    if (!name || !strstr(name, "ExitProcess")) return;
    recomp_dump_trace("ExitProcess");
    fprintf(stderr, "[exit] guest stack (esp=0x%08X):", g_esp);
    for (int k = 0; k < 0x40; k += 4) fprintf(stderr, " %08X", MEM32(g_esp + k));
    fprintf(stderr, "\n");
}

void korra_crash_probe_init(void) {
    extern void (*recomp_trace_extra)(uint32_t va);
    if (!getenv("KORRA_TRACE_CRASH")) return;
    crash_watch(0xCA3E58);   /* published BehaviorPlayer */
    crash_watch(0xE381F8);   /* UI visibility word */
    crash_watch(0xCA3AC0);   /* stage/game mode */
    crash_watch(0xE562E4);   /* app phase */
    crash_watch(0xE38A84);   /* current step id */
    crash_watch(0xE38AB0);   /* pending step id */
    recomp_trace_extra = crash_probe_hook;
    native32_pre_hook = crash_exit_hook;
    fprintf(stderr, "[crash] new-game crash timeline on\n");
}
