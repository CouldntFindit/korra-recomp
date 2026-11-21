/*
 * watch_probe.c - task 2 oracle instrumentation (temporary, debug only).
 *
 * The Android run dies in sub_007492B0 because [0xE4D064 + 0x30] is NULL and
 * the renderer reads through it without a test. The Windows host is the
 * reference the device run is compared against, so the same two probes are
 * available here, driven by the environment instead of system properties:
 *
 *   KORRA_ICALL=<hex>     every indirect transfer to that guest VA logs its
 *                         caller and the first three stack arguments (8 hits).
 *   KORRA_WATCHLOG=<hex>  eight dwords at that VA are sampled on every indirect
 *                         transfer; each change is logged with the lifted
 *                         function that was running when it appeared. This is
 *                         how a writer reachable only through a base register
 *                         gets named: `from=` is the store site's function.
 *
 * Both run before the lookup, so a hook can never hide a miss.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "recomp_types.h"

static int      s_read;
static uint32_t s_icall_va, s_icall_hits;
static uint32_t s_watch_va, s_watch_hits, s_watch_last[8];

static uint32_t hex_env(const char *name) {
    const char *v = getenv(name);
    if (!v || !*v) return 0;
    return (uint32_t)strtoul(v, NULL, 16);
}

void korra_watch_probe_tick(void) {
    if (!s_read) {
        s_read = 1;
        s_icall_va = hex_env("KORRA_ICALL");
        s_watch_va = hex_env("KORRA_WATCHLOG");
        if (s_icall_va)
            fprintf(stderr, "[icall] watching %08X\n", s_icall_va);
        if (s_watch_va) {
            const uint32_t *w = (const uint32_t *)ADDR(s_watch_va);
            int i;
            for (i = 0; i < 8; i++) s_watch_last[i] = w[i];
            fprintf(stderr, "[wlog] watching %08X start=%08X %08X %08X %08X\n",
                    s_watch_va, w[0], w[1], w[2], w[3]);
        }
        fflush(stderr);
    }

    if (s_watch_va) {
        const uint32_t *w = (const uint32_t *)ADDR(s_watch_va);
        int i;
        for (i = 0; i < 8; i++) {
            if (w[i] == s_watch_last[i]) continue;
            if (++s_watch_hits <= 400)
                fprintf(stderr, "[wlog] +%02X = %08X (was %08X) from=%08X\n",
                        i * 4, w[i], s_watch_last[i], g_cur_func);
            s_watch_last[i] = w[i];
        }
    }
}

void korra_watch_probe_lookup(uint32_t va) {
    korra_watch_probe_tick();
    if (!s_icall_va || va != s_icall_va) return;
    if (++s_icall_hits > 8) return;
    fprintf(stderr, "[icall] target=%08X from=%08X esp=%08X ebp=%08X"
                    " args=%08X %08X %08X\n",
            va, g_cur_func, g_esp, g_ebp,
            (uint32_t)MEM32(g_esp), (uint32_t)MEM32(g_esp + 4),
            (uint32_t)MEM32(g_esp + 8));
    fflush(stderr);
}
