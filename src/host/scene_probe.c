/*
 * scene_probe.c - timeline of the scene/stage-load pipeline and the asset pool.
 *
 * The new-game crash was traced to the player's asset bundle never being
 * registered, and the text defect to no font atlas ever being created. Both
 * live upstream of the draw call and of the player, in the same place: the
 * engine's scene task (SceneTask) that turns a queued stage request into
 * installed assets, and the resource pool those assets are looked up in.
 *
 * Instrumentation used so far (--firsthit, --poison) is sampled per lifted
 * function entry, so a value written and overwritten between two entries is
 * invisible, and a pool record that changes state without an entry nearby is
 * missed entirely. This probe reads guest memory from the host instead, from
 * native32_post_hook - which fires on every bridged import and COM method -
 * and prints a line only when something changed:
 *
 *   [scene] flow=0x11 inst=0x1 list=0x0E1B2C40 gate=0x0 early=0x1
 *           player=0x00000000 step=0x00000710 mode=2 pool=0
 *   [scene] pool 3: "core/core.dat" id=1 st=4 | "core/coreui.dat" id=2 st=1 | ...
 *
 * `flow`  0xE38A54  scene/stage flow state (0..0x16, 0xC -> 0x13 starts install)
 * `inst`  0xDBC02C  installer state machine (0 idle, 1 reset, 2 build, 3 run, 4 done)
 * `list`  0xDBC030  the 8-slot request container
 * `gate`  0xDC09F0  the value whose 0x100/0x101 selects the load branch
 * `early` 0xDC09C8  half of the early-out in 0x53FB30
 * `player`0xCA3E58  the published BehaviorPlayer (0 = the crash's NULL)
 * `step`  0xE38AB0  pending step id
 * `mode`  0xCA3AC0  stage/game mode
 * `pool`  0xE45644  number of records in the resource pool at 0xE45618
 *
 * Each pool record is { +0x08 key[0x20], +0x28 id, +0x4C state }; a find
 * (0x6AAB10) returns record+0x30 only when the state is a "ready" one, so the
 * state column is what tells "installed but never loaded" from "never
 * installed".
 *
 * Knobs:
 *   KORRA_TRACE_SCENE=1     on (off by default: no hook is installed)
 *   KORRA_TRACE_SCENE_MS=N  sampling period for the globals, default 50 ms
 *
 * Everything printed goes to stderr, which the host also mirrors into
 * <game>\korra_host-*.log.
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

#define VA_FLOW        0x00E38A54u  /* scene flow state                    */
#define VA_INST        0x00DBC02Cu  /* installer state machine             */
#define VA_LIST        0x00DBC030u  /* 8-slot request container            */
#define VA_GATE        0x00DC09F0u  /* selects the load branch at flow 9   */
#define VA_EARLY       0x00DC09C8u  /* early-out in 0x53FB30               */
#define VA_PLAYER      0x00CA3E58u  /* published BehaviorPlayer            */
#define VA_STEP        0x00E38AB0u  /* pending step id                     */
#define VA_MODE        0x00CA3AC0u  /* stage/game mode                     */
#define VA_POOL_N      0x00E45644u  /* resource pool count                 */
#define VA_POOL_A      0x00E4563Cu  /* resource pool record array          */

static native32_post_hook_t g_other;
static int      g_enabled;
static DWORD    g_period_ms = 16;    /* pool re-scan period */
static ULONGLONG g_next_sample;
static int      g_globals_ok;        /* one range check for all watch addresses */

static uint32_t g_last[9];
static int      g_have_last;

/* ---------------------------------------------------------------- guest memory
 * The guest image is mapped into this process at its own virtual addresses, so
 * a watch address is readable when it lies inside a committed region. */
static int readable(uint32_t va, size_t n) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!va || va + n < va) return 0;
    if (!VirtualQuery((const void *)(uintptr_t)va, &mbi, sizeof mbi)) return 0;
    if (mbi.State != MEM_COMMIT) return 0;
    if ((DWORD)mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return 0;
    return (uint32_t)(uintptr_t)mbi.BaseAddress + mbi.RegionSize >= va + n;
}

static uint32_t rd32(uint32_t va) {
    return readable(va, 4) ? *(const volatile uint32_t *)(uintptr_t)va : 0xDEADBEEFu;
}

/* Copy a printable key out of guest memory, stopping at NUL or 0x20 bytes. */
static void rd_key(uint32_t va, char *out, size_t cap) {
    size_t i = 0;
    if (!cap) return;
    out[0] = 0;
    if (!readable(va, 1)) return;
    for (; i + 1 < cap && i < 0x20; i++) {
        char c = *(const volatile char *)(uintptr_t)(va + i);
        if (!c) break;
        out[i] = (c >= 0x20 && c < 0x7F) ? c : '.';
    }
    out[i] = 0;
}

/* One-shot dump of a record whose key ends with `suffix`: the first dwords of
 * its payload and any printable runs nearby. The player's bundle is the one
 * gate 2 resolves a sub-resource from, so its shape is what tells "parsed" from
 * "parsed empty". */
#define POOL_MAX 64
static char     g_poolsig[POOL_MAX * 40];
static int      g_pool_n_last = -1;

static void dump_bundle(const char *suffix) {
    static int done;
    uint32_t n = rd32(VA_POOL_N);
    uint32_t arr = rd32(VA_POOL_A);
    if (done || n > POOL_MAX) return;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t rec;
        char key[36];
        if (!arr || !readable(arr + i * 4, 4)) return;
        rec = *(const volatile uint32_t *)(uintptr_t)(arr + i * 4);
        if (!rec || !readable(rec + 0x08, 0x48)) continue;
        rd_key(rec + 0x08, key, sizeof key);
        if (!strstr(key, suffix)) continue;
        {
            uint32_t pl = rd32(rec + 0x30);
            uint32_t st = rd32(rec + 0x4C);
            if (st != 8 || !pl || !readable(pl, 0x40)) return;
            done = 1;
            fprintf(stderr, "[scene] bundle \"%s\" rec=0x%08X payload=0x%08X head=",
                    key, rec, pl);
            for (int k = 0; k < 16; k++) fprintf(stderr, " %08X", rd32(pl + (uint32_t)k * 4));
            fprintf(stderr, "\n");
            if (readable(pl, 0x400)) {
                fprintf(stderr, "[scene] bundle text:");
                for (int off = 0, runs = 0; off < 0x400 && runs < 12; off++) {
                    char s[64];
                    int len = 0;
                    while (off + len < 0x400 && len < 60) {
                        char c = *(const volatile char *)(uintptr_t)(pl + (uint32_t)(off + len));
                        if (c < 0x20 || c >= 0x7F) break;
                        s[len++] = c;
                    }
                    if (len >= 6) {
                        s[len] = 0;
                        fprintf(stderr, " \"%s\"", s);
                        runs++;
                        off += len - 1;
                    }
                }
                fprintf(stderr, "\n");
            }
        }
        return;
    }
}

/* Print the pool only when it changes: record count, a record's state or a key.
 * A stage that installs 8 slots and a UI bundle that installs 20 records both
 * show up here as an ordered list, which is what says whether the installer
 * ever ran at all. */

static void dump_pool(void) {
    uint32_t n = rd32(VA_POOL_N);
    uint32_t arr = rd32(VA_POOL_A);
    char sig[sizeof g_poolsig];
    char line[2048];
    size_t o = 0;
    int drawn = 0;

    if (n > POOL_MAX) n = POOL_MAX;
    sig[0] = 0;
    line[0] = 0;
    o += (size_t)_snprintf(line + o, sizeof line - o, "[scene] pool %u:", rd32(VA_POOL_N));
    for (uint32_t i = 0; i < n && o + 120 < sizeof line; i++) {
        uint32_t rec = 0;
        if (!arr || !readable(arr + i * 4, 4)) break;
        rec = *(const volatile uint32_t *)(uintptr_t)(arr + i * 4);
        if (!rec || !readable(rec + 0x08, 0x48)) continue;
        {
            char key[36];
            uint32_t id = rd32(rec + 0x28);
            uint32_t st = rd32(rec + 0x4C);
            uint32_t pl = rd32(rec + 0x30);
            uint32_t rc = rd32(rec + 0x40);
            uint32_t tmr = rd32(rec + 0x38);
            uint32_t typ = rd32(rec + 0x04);
            rd_key(rec + 0x08, key, sizeof key);
            o += (size_t)_snprintf(line + o, sizeof line - o,
                                   " \"%s\" id=%u st=%u pl=0x%08X rc=%u typ=%u tmr=%u |",
                                   key, id, st, pl, rc, typ, tmr);
            if (drawn + 60 < (int)sizeof sig)
                drawn += _snprintf(sig + drawn, sizeof sig - (size_t)drawn,
                                   "%s/%u/%u/%08X;", key, id, st, pl);
        }
    }
    if (n == 0 && o + 8 < sizeof line) o += (size_t)_snprintf(line + o, sizeof line - o, " empty");
    if (strcmp(sig, g_poolsig) == 0) return;
    strncpy(g_poolsig, sig, sizeof g_poolsig - 1);
    g_poolsig[sizeof g_poolsig - 1] = 0;
    g_pool_n_last = (int)n;
    fprintf(stderr, "%s\n", line);
}

static void sample(int force) {
    uint32_t v[9];
    int changed = 0;

    /* All watch addresses live in the guest's .data, inside the mapped image.
     * One range check at startup keeps the per-call path to nine loads. */
    if (!g_globals_ok) {
        if (!readable(VA_FLOW - 0x1000, 0x2000)) return;
        if (!readable(VA_GATE - 0x1000, 0x2000)) return;
        if (!readable(VA_PLAYER - 0x1000, 0x2000)) return;
        if (!readable(VA_POOL_N - 0x1000, 0x2000)) return;
        g_globals_ok = 1;
    }

    v[0] = *(const volatile uint32_t *)(uintptr_t)VA_FLOW;
    v[1] = *(const volatile uint32_t *)(uintptr_t)VA_INST;
    v[2] = *(const volatile uint32_t *)(uintptr_t)VA_LIST;
    v[3] = *(const volatile uint32_t *)(uintptr_t)VA_GATE;
    v[4] = *(const volatile uint32_t *)(uintptr_t)VA_EARLY;
    v[5] = *(const volatile uint32_t *)(uintptr_t)VA_PLAYER;
    v[6] = *(const volatile uint32_t *)(uintptr_t)VA_STEP;
    v[7] = *(const volatile uint32_t *)(uintptr_t)VA_MODE;
    v[8] = *(const volatile uint32_t *)(uintptr_t)VA_POOL_N;

    if (!g_have_last) changed = 1;
    else for (int i = 0; i < 9; i++) if (v[i] != g_last[i]) { changed = 1; break; }

    if (changed) {
        memcpy(g_last, v, sizeof v);
        g_have_last = 1;
        fprintf(stderr,
                "[scene] flow=0x%X inst=0x%X list=0x%08X gate=0x%X early=0x%X "
                "player=0x%08X step=0x%X mode=%u pool=%u\n",
                v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8]);
    }

    /* The pool is rescanned on every change and otherwise on the period, so a
     * record changing state without a nearby global change is still caught. */
    if (changed || force || GetTickCount64() >= g_next_sample) {
        g_next_sample = GetTickCount64() + g_period_ms;
        dump_pool();
        dump_bundle("pl00f0.dat");
    }
}

static void probe_post(uint32_t target, const uint32_t *args, uint32_t ret) {
    sample(0);
    if (g_other) g_other(target, args, ret);
}

static void __cdecl scene_probe_ctor(void) {
    const char *period = getenv("KORRA_TRACE_SCENE_MS");
    if (!getenv("KORRA_TRACE_SCENE")) return;
    if (period) {
        long ms = strtol(period, NULL, 0);
        if (ms > 0 && ms < 10000) g_period_ms = (DWORD)ms;
    }
    if (native32_post_hook && native32_post_hook != probe_post)
        g_other = native32_post_hook;
    native32_post_hook = probe_post;
    g_enabled = 1;
    fprintf(stderr, "[scene] scene/stage timeline on (period %lu ms)\n",
            (unsigned long)g_period_ms);
    sample(1);
}

/* A CRT initializer, so this file needs no edit to main.c (which belongs to the
 * bring-up session) and no new build-script entry (the build globs src\host). */
#pragma section(".CRT$XIU", read)
__declspec(allocate(".CRT$XIU")) static void (__cdecl *g_scene_ctor)(void) = scene_probe_ctor;

/* Keep the double-checked variable reachable from a debugger. */
int korra_scene_probe_active(void) { return g_enabled; }
