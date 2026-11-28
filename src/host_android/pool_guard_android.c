/* pool_guard_android.c - host-side containment at the engine pool free.
 *
 *   adb shell setprop debug.korra.poolguard 1
 *   adb shell setprop debug.korra.poolinject <free-seq>   (diagnostic only)
 *
 * The Stage 1 crash is an unlink (sub_00829560) called from the coalescing
 * free (sub_00829880) inside the pool free (sub_00828D80) with a NULL block
 * header link while the size word says the previous chunk is free. 303,104
 * device frees and 12,500 Windows frees had the header correct at the free
 * entry, so the writer is unnamed; the 1 ms wait poll masks the race.
 *
 * This file is containment, not a fix. When the free is about to use a
 * "previous chunk is free" neighbour whose link is not a plausible chunk
 * address it clears size bit 1, so the guest skips the unlink. It writes one
 * bit of the allocator's own header and patches no code: the invariant the
 * unlink needs (bit 1 set => non-NULL valid link) is already broken at that
 * point.
 *
 * Hook: the pool free's own EnterCriticalSection (g_cur_func == 0x00828D80)
 * in kernel32_android.c, before the pool lock is taken. The guest ebp is still
 * the free's frame there, so arg0 (pool index) is [ebp+8] and arg1 (the block)
 * is [ebp+0xC]. Off by default: with both properties unset the entry costs a
 * cached-flag return. No ring, no allocation, no stdio, no snapshots.
 *
 * Cost control: at most 200 GUARD lines, then one summary line per 1000 hits,
 * so a pathological run cannot fill the device storage.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdlib.h>
#include <sys/system_properties.h>

#include "host_android.h"

#define POOL_GUARD_LOG_CAP 200u
#define POOL_GUARD_SUMMARY_EVERY 1000u

static int      s_state;      /* 1 once the properties were read */
static int      s_guard;      /* debug.korra.poolguard, default 0 */
static int      s_inject;     /* debug.korra.poolinject=<seq>, -1 off */
static uint32_t s_seq;        /* every pool free, 1-based */
static uint32_t s_hits;

/* The free's previous-chunk link is a chunk address: 4-aligned, inside the
 * heap, and below the chunk it is linked from. Never dereferenced here. */
static int plausible_prev(uint32_t block, uint32_t p) {
    return p && (p & 3u) == 0 && p >= KA_HEAP_BASE && p < KA_HEAP_END &&
           p + 8u <= block;
}

static void read_props(void) {
    char v[PROP_VALUE_MAX] = {0};
    __system_property_get("debug.korra.poolguard", v);
    s_guard = (v[0] == '1') ? 1 : 0;
    v[0] = 0;
    __system_property_get("debug.korra.poolinject", v);
    s_inject = v[0] ? atoi(v) : -1;
    s_state = 1;
    /* Once per launch: the A/B and the campaign both assert this line. */
    ka_log("[poolguard] state guard=%d inject=%d", s_guard, s_inject);
}

void ka_pool_guard_free(uint32_t pool_index, uint32_t block) {
    uint32_t prev, size;
    (void)pool_index;
    if (!s_state) read_props();
    /* Default-off path: one cached test, then out. The sequence counter is
     * only needed when one of the two properties is live. */
    if (!s_guard && s_inject <= 0) return;

    s_seq++;
    if (s_inject > 0 && (int)s_seq == s_inject) {
        if (block >= KA_HEAP_BASE + 8u && block < KA_HEAP_END) {
            MEM32(block - 8) = 0;
            MEM32(block - 4) |= 2u;
            ka_log("[poolguard] INJECT blk=%08X seq=%u (hdr now 0/%08X)",
                   block, s_seq, MEM32(block - 4));
        }
    }

    if (block < KA_HEAP_BASE + 8u || block >= KA_HEAP_END) return;
    prev = MEM32(block - 8);
    size = MEM32(block - 4);
    if ((size & 2u) && !plausible_prev(block, prev) && s_guard) {
        MEM32(block - 4) = size & ~2u;
        s_hits++;
        if (s_hits <= POOL_GUARD_LOG_CAP)
            ka_log("[poolguard] GUARD blk=%08X hdr=%08X/%08X -> size=%08X",
                   block, prev, size, size & ~2u);
        else if ((s_hits % POOL_GUARD_SUMMARY_EVERY) == 0u)
            ka_log("[poolguard] GUARD hits=%u (log capped at %u)",
                   s_hits, POOL_GUARD_LOG_CAP);
    }
}
