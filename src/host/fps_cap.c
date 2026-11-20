/*
 * fps_cap.c - hold the engine's own frame limiter at a chosen rate.
 *
 * The engine computes its limiter interval itself in FUN_0074c580 from the
 * global DAT_00e4d210 (0xe4d210): `1` = 30 fps, anything else = 60 fps, and it
 * stores floor(seconds * 1000 * 3) into [frameMgr+0x58] - 1/3-ms units, so 100
 * for 30 fps and 50 for 60 (docs/quirks.md section 4). FUN_00754840 then sleeps
 * against that field.
 *
 * The engine has no 40 fps setting, but that field is just a number, so a host
 * can ask for the same thing at another rate without touching either function:
 * interval = round(3000 / fps). This writes that value and re-applies it if the
 * engine recomputes it (which happens whenever DAT_00e4d210 changes, e.g. on a
 * focus change).
 *
 *   KORRA_FPS=30|40|60    initial cap (any 1-240 works; unset = the engine's own)
 *   KORRA_FPS_CYCLE=VK     key that cycles 30 -> 40 -> 60 -> engine default,
 *                          default F9; set KORRA_FPS_CYCLE=0 to disable the key
 *
 * A cap can only lower the rate the engine asks for; it never shortens a frame
 * that already takes 25 ms. That is the point: a port that averages 45 with
 * frames between 17.5 and 25 ms looks far steadier pinned to 40, and the cap is
 * a measurement/comfort knob, not a fix - leave it off when profiling.
 *
 * "Engine default" means: stop enforcing and let DAT_00e4d210 decide (100 for
 * its 30 fps mode, 50 for 60). The 32-bit field write is a single aligned
 * dword, i.e. atomic on x86, so the watchdog does not need the machine lock.
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
#define VA_MODE      0x00E4D210u   /* DAT_00e4d210: 1 = 30 fps, else 60     */

/* The rates the cycle key walks through, in order; 0 = engine default. */
static const unsigned k_rates[] = { 30, 40, 60, 0 };

static int readable(uint32_t va, size_t n) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!va || va + n < va) return 0;
    if (!VirtualQuery((const void *)(uintptr_t)va, &mbi, sizeof mbi)) return 0;
    if (mbi.State != MEM_COMMIT) return 0;
    if ((DWORD)mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return 0;
    return (uint32_t)(uintptr_t)mbi.BaseAddress + mbi.RegionSize >= va + n;
}

static uint32_t interval_for(unsigned fps) {
    if (!fps) return 0;                            /* engine default: unknown here */
    if (fps > 240) fps = 240;
    {
        uint32_t want = (uint32_t)((3000u + fps / 2) / fps);
        return want < 1 ? 1u : want;
    }
}

/* What the engine itself would ask for right now. */
static uint32_t engine_interval(void) {
    uint32_t mode = readable(VA_MODE, 4) ? *(const volatile uint32_t *)(uintptr_t)VA_MODE : 0;
    return mode == 1u ? 100u : 50u;
}

static const char *engine_note(void) { return " (engine default)"; }

static unsigned parse_key(const char *s) {
    if (!s || !*s) return VK_F9;
    if (s[0] >= '0' && s[0] <= '9') return (unsigned)strtoul(s, NULL, 0);
    if (s[0] == 'F' || s[0] == 'f') {
        int n = atoi(s + 1);
        if (n >= 1 && n <= 12) return (unsigned)(VK_F1 + (n - 1));
        if (n >= 13 && n <= 24) return (unsigned)(VK_F13 + (n - 13));
    }
    return VK_F9;
}

static DWORD WINAPI cap_thread(LPVOID param) {
    unsigned start = (unsigned)(uintptr_t)param;
    unsigned cur = start;             /* 0 = engine default */
    unsigned cycle_key = 0;
    int was_down = 0;
    unsigned applied = 0;
    unsigned next_apply = 0;

    {
        const char *k = getenv("KORRA_FPS_CYCLE");
        if (!k || strcmp(k, "0") != 0) cycle_key = parse_key(k);
    }

    for (;;) {
        Sleep(50);

        if (cycle_key) {
            int down = (GetAsyncKeyState((int)cycle_key) & 0x8000) != 0;
            if (down && !was_down) {
                int i, found = -1;
                for (i = 0; i < (int)(sizeof k_rates / sizeof k_rates[0]); i++)
                    if (k_rates[i] == cur) { found = i; break; }
                cur = k_rates[(found < 0 ? 0 : (found + 1) % (int)(sizeof k_rates / sizeof k_rates[0]))];
                next_apply = 0;
                fprintf(stderr, "[fps] cap -> %u fps%s\n", cur, cur ? "" : engine_note());
                fflush(stderr);
            }
            was_down = down;
        }

        /* Re-apply immediately after a change, then as a watchdog. */
        if (next_apply > 0) { next_apply--; continue; }
        next_apply = 3;                      /* ~200 ms */
        {
            uint32_t mgr, want, now;
            if (!readable(VA_FRAME_MGR, 4)) continue;
            mgr = *(const volatile uint32_t *)(uintptr_t)VA_FRAME_MGR;
            if (!mgr || mgr == 0xFFFFFFFFu || !readable(mgr, 0x60)) continue;
            want = cur ? interval_for(cur) : engine_interval();
            now = *(const volatile uint32_t *)(uintptr_t)(mgr + 0x58);
            if (now == want) continue;
            if (!now) continue;              /* not initialised yet */
            *(volatile uint32_t *)(uintptr_t)(mgr + 0x58) = want;
            applied++;
            if (applied <= 4 || applied % 64 == 0)
                fprintf(stderr, "[fps] limiter interval %u -> %u (cap %u fps%s)%s\n",
                        now, want, cur, cur ? "" : engine_note(),
                        applied > 4 ? " (re-applied)" : "");
            fflush(stderr);
        }
    }
    return 0;
}

void korra_fps_cap_init(void) {
    const char *s = getenv("KORRA_FPS");
    unsigned start = 0;
    if (s) {
        long fps = strtol(s, NULL, 0);
        if (fps > 0) start = (unsigned)(fps > 240 ? 240 : fps);
    }
    /* The cycle key is always armed (F9 by default) so the three rates are
     * reachable during a playtest even when no cap was asked for at launch;
     * the engine's own 60 is where it starts. */
    if (!CreateThread(NULL, 0, cap_thread, (LPVOID)(uintptr_t)start, 0, NULL)) {
        fprintf(stderr, "[fps] cap thread failed\n");
        return;
    }
    fprintf(stderr, "[fps] cap %u fps%s; F9 cycles 30/40/60/engine\n",
            start, start ? "" : " (engine default)");
    fflush(stderr);
}
