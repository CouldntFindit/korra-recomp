/* fps_cap_android.c - the port's frame cap, on the engine's own limiter.
 *
 * Pattern: src/host/fps_cap.c (Windows: KORRA_FPS + F9). The engine computes its
 * limiter interval in FUN_0074c580 from DAT_00e4d210 (`1` = 30 fps, anything
 * else 60) and stores floor(seconds * 1000 * 3) into [frameMgr+0x58] -
 * 1/3-ms units, 100 for 30 fps and 50 for 60 (docs/quirks.md section 4).
 * FUN_00754840 then sleeps against that field, so writing it asks for any rate
 * without touching either function.
 *
 * Android has no keyboard for the Windows F9 cycle, so the knob is a property:
 *
 *   debug.korra.fps=<1-240>   cap (default 30: the owner's choice for this port)
 *   debug.korra.fps=0         leave the engine's own 30/60 mode alone
 *
 * The watchdog re-applies the value when the engine recomputes it (a focus
 * change, a mode switch), the same way the Windows thread does. It runs on the
 * presenting (render) thread from the Present path, throttled to ~5 Hz, so the
 * 32-bit store needs no machine lock of its own - the guest thread that reads
 * the field is this one.
 *
 * A cap only lowers the rate the engine asks for; it never shortens a frame
 * that already takes 100 ms. "30 fps default" here means the engine stops
 * trying for 60 on a port that cannot hold it, not that 30 is reachable.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/system_properties.h>
#include <unistd.h>

#include "host_android.h"

#define VA_FRAME_MGR 0x00E4D060u
#define VA_MODE      0x00E4D210u
#define VA_GUEST_LO  0x00400000u
#define VA_GUEST_HI  0xD0000000u

static int      g_on;
static unsigned g_fps;          /* 0 = engine default */
static uint32_t g_want;
static unsigned g_applied;
static uint32_t g_last_ms;

static uint32_t interval_for(unsigned fps) {
    uint32_t want;
    if (!fps) return 0;
    if (fps > 240) fps = 240;
    want = (3000u + fps / 2) / fps;
    return want < 1 ? 1u : want;
}

static uint32_t frame_mgr(void) {
    uint32_t mgr;
    if (!g_mem_base) return 0;
    mgr = MEM32(VA_FRAME_MGR);
    if (mgr < VA_GUEST_LO || mgr >= VA_GUEST_HI || (mgr & 3u)) return 0;
    return mgr;
}

/* Called from the Present path: cheap enough to run every frame, and it does
 * real work at most every 200 ms. */
void ka_fps_tick(void) {
    uint32_t mgr, now, ms;
    if (!g_on) return;
    ms = ka_mono_ms();
    if (ms - g_last_ms < 200u) return;
    g_last_ms = ms;

    mgr = frame_mgr();
    if (!mgr) return;
    now = MEM32(mgr + 0x58);
    if (!now) return;                 /* not initialised yet */
    if (g_want == 0) {
        /* Engine default: follow DAT_00e4d210 (1 = 30 fps, else 60). */
        g_want = MEM32(VA_MODE) == 1u ? 100u : 50u;
    }
    if (now == g_want) return;
    MEM32(mgr + 0x58) = g_want;
    g_applied++;
    if (g_applied <= 4 || !(g_applied % 64)) {
        char line[192];
        int n = snprintf(line, sizeof line,
                         "[fps] limiter interval %u -> %u (cap %u fps)%s\n",
                         now, g_want, g_fps, g_fps ? "" : " (engine default)");
        if (n > 0) {
            ssize_t w = write(2, line, (size_t)n);
            (void)w;
        }
    }
}

/* Started from ka_guest_run: reads the property and logs the value, so a run's
 * log always says which cap it was asked for. */
void ka_fps_cap_start(void) {
    char v[PROP_VALUE_MAX] = {0};
    long n = 30;
    __system_property_get("debug.korra.fps", v);
    if (v[0]) n = strtol(v, NULL, 0);
    if (n < 0) n = 0;
    if (n > 240) n = 240;
    g_fps = (unsigned)n;
    g_want = interval_for(g_fps);
    g_on = 1;
    g_last_ms = ka_mono_ms();
    {
        char line[192];
        int len = snprintf(line, sizeof line,
                           "[fps] cap %u fps (interval %u, 1/3-ms units)%s\n",
                           g_fps, g_want, g_fps ? "" : " - engine default");
        if (len > 0) {
            ssize_t w = write(2, line, (size_t)len);
            (void)w;
        }
    }
}

unsigned ka_fps_cap_value(void) { return g_fps; }
