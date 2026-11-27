/* pool_probe_android.c - diagnostics-only probe at the engine pool free.
 *
 *   adb shell setprop debug.korra.poolprobe 1
 *
 * The Stage 1 forensic crash is an unlink (sub_00829560) called from the
 * coalescing free (sub_00829880) inside the pool free (sub_00828D80) with a
 * NULL block header link while the size word says the previous chunk is free.
 * This probe records the block state at the pool-free entry, where the block
 * and the pool record are still intact, so the layer can be named:
 *
 *   - the block is already on its size bin's free list  -> double free
 *   - the header dword is 0 at free                     -> uninitialised or corrupted
 *   - header valid at free but corrupted by unlink time -> concurrent writer
 *
 * Hook: the pool free's own EnterCriticalSection (g_cur_func == 0x00828D80).
 * At that point the guest ebp is still the function's frame, so arg0 (the pool
 * index) is [ebp+8] and arg1 (the block) is [ebp+0xC]. Nothing is patched in
 * the guest and no code is relifted: the shim reads two stack slots and memory.
 *
 * Lifecycle (2026-11-27, allocation-to-free watch). The entry probe alone
 * cannot say *when* a header changed. The allocator sub_00829000 has the
 * returned block in EDI at its own LeaveCriticalSection, so the same shim
 * snapshots every block at allocation; the free then compares:
 *
 *   alloc snapshot  -> was the link already 0 when the block was handed out?
 *   after the lock  -> did the header change while this thread waited for the
 *                      pool lock (a writer that does not take the lock)?
 *   after the free  -> did the free itself, or a concurrent writer during its
 *                      critical section, leave the header different?
 *
 * Free-list layout (from sub_00829560 / sub_00829880): the chunk is
 * block-8; [chunk]=prev physical chunk link, [chunk+4]=size word,
 * [chunk+8]=fd and [chunk+0xC]=bk on the region's bin list, whose head is
 * region + 0x74 + bin*4 and whose sentinel is the region itself. The bin is
 * derived from (size & ~3) exactly as the unlink does it, and "previous chunk
 * is free" is bit 1 of the size word.
 *
 * The last 256 frees are kept in a ring; a crash dumps the ring. One compact
 * line per free (capped) also lets a non-crashing control run show whether the
 * header state is normal. Diagnostics only: off unless the property is 1, and
 * no behaviour depends on it.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/system_properties.h>
#include <unistd.h>

#include "host_android.h"

#define POOL_PROBE_RING 256u
#define POOL_PROBE_LOG_CAP 40000u
#define POOL_PROBE_BIN_CAP 1024u
#define POOL_PROBE_WALK_CAP 256u
#define POOL_SNAP_BITS 16u
#define POOL_SNAP_SIZE (1u << POOL_SNAP_BITS)
#define POOL_SNAP_MASK (POOL_SNAP_SIZE - 1u)
#define POOL_SNAP_TOMB 1u     /* deleted slot; a block is 16-byte aligned */

#define POOL_FREE_FUNC  0x00828D80u
#define POOL_ALLOC_FUNC 0x00829000u

/* Snapshot taken at allocation: exactly the fields the free will judge. */
typedef struct {
    uint32_t block;     /* 0 = empty */
    uint32_t seq, tid;
    uint32_t prev, size, word;
} pool_snap;

typedef struct {
    uint32_t seq;
    uint32_t tid;
    uint32_t func;
    uint32_t idx;       /* pool index (sub_00828D80 arg0) */
    uint32_t block;     /* sub_00828D80 arg1 */
    uint32_t pool;      /* [0xCA1EF0] + idx*0x48 */
    uint32_t region;    /* [pool+0x38] */
    uint32_t freeb;     /* [pool+0x40] before the free */
    uint32_t h_prev;    /* [block-8]  (chunk[0]) */
    uint32_t h_size;    /* [block-4]  (chunk[1]) */
    uint32_t h_word;    /* [block] */
    uint32_t size;      /* h_size & ~3 */
    int32_t  bin;       /* derived bin, -1 if not derivable */
    int32_t  self;      /* chunk itself is on the bin list */
    int32_t  prevok;    /* the header link is a plausible chunk address */
    /* lifecycle */
    int32_t  has_alloc;
    uint32_t a_seq, a_tid, a_prev, a_size, a_word;
    int32_t  has_locked;
    uint32_t l_prev, l_size;
    int32_t  has_post;
    uint32_t p_prev, p_size;
} pool_rec;

static pool_rec s_ring[POOL_PROBE_RING];
static pool_snap s_snap[POOL_SNAP_SIZE];
static uint32_t s_seq, s_aseq;
static uint32_t s_logged;
static int      s_on = -1;   /* -1 unknown, 0 off, 1 on */

/* Counters, printed in the crash dump and once every few thousand frees. */
static uint32_t s_frees, s_alloc, s_with_snap, s_no_snap;
static uint32_t s_alloc_bit1, s_alloc_bad, s_size_chg, s_link_zeroed;
static uint32_t s_locked_chg, s_post_chg, s_log_cap;
static uint32_t s_guard_hits, s_snap_full;
static int      s_guard = -1;   /* debug.korra.poolguard: candidate, off */

static __thread pool_rec *t_cur;   /* the free this thread is inside */

int ka_pool_probe_on(void) {
    if (s_on < 0) {
        char v[PROP_VALUE_MAX] = {0};
        __system_property_get("debug.korra.poolprobe", v);
        s_on = (v[0] == '1') ? 1 : 0;
        if (s_on) ka_log("[poolprobe] on (debug.korra.poolprobe=1)");
    }
    return s_on;
}

/* Candidate host guard (owner-gated, off unless debug.korra.poolguard=1): when
 * the free is about to unlink a "previous chunk is free" neighbour whose link
 * is not a plausible chunk address, clear size bit 1 before the guest reads it
 * so the guest skips the unlink. It writes guest memory but patches no code:
 * the header is the allocator's own state and the invariant it relies on
 * (bit 1 set => non-NULL valid link) is already broken at that point. */
static int guard_on(void) {
    if (s_guard < 0) {
        char v[PROP_VALUE_MAX] = {0};
        __system_property_get("debug.korra.poolguard", v);
        s_guard = (v[0] == '1') ? 1 : 0;
        if (s_guard) ka_log("[poolprobe] guard on (debug.korra.poolguard=1)");
    }
    return s_guard;
}

static int readable(uint32_t va, uint32_t n) {
    uint32_t end = va + n;
    if (!va || end < va) return 0;
    if (va >= KA_IMAGE_BASE && end <= KA_IMAGE_BASE + ka_pe_span()) return 1;
    if (va >= KA_HEAP_BASE && end <= KA_HEAP_END) return 1;
    return 0;
}

/* The unlink's bin derivation, copied from sub_00829560:
 *   size = [node+4] & ~3
 *   if size < 0x80: bin = size >> 2
 *   else:           msb = bsr(size); hi = (size >> (msb-5)) ^ 0x20;
 *                   bin = (msb-6)*32 + hi
 * Returns -1 when the size is 0 (no bin). */
static int32_t bin_of(uint32_t size) {
    uint32_t msb = 0;
    if (!size) return -1;
    if (size < 0x80u) return (int32_t)(size >> 2);
    while (msb < 31 && (size >> (msb + 1))) msb++;
    return (int32_t)(((msb - 6) * 32u) + ((size >> (msb - 5)) ^ 0x20u));
}

/* Is `want` on the bin list headed at region+0x74+bin*4? The list is circular
 * with the region as its sentinel (fd at +8). Bounded, range-checked, and the
 * nodes must lie in the allocator's own region: an uncommitted heap page is
 * PROT_NONE, so a bogus pointer there cannot be dereferenced just because it
 * is inside the heap window (the first probe build faulted exactly that way on
 * a header link of 0x20000002). */
static int on_bin(uint32_t region, int32_t bin, uint32_t want) {
    uint32_t head, n;
    int k = 0;
    if (!want || bin < 0 || bin >= (int32_t)POOL_PROBE_BIN_CAP) return 0;
    if (!readable(region + 0x74u + (uint32_t)bin * 4u, 4)) return 0;
    head = MEM32(region + 0x74u + (uint32_t)bin * 4u);
    n = head;
    while (n != region && k < POOL_PROBE_WALK_CAP) {
        if (n == want) return 1;
        if (n < region || (n & 3u) || !readable(n, 0x10)) return 0;
        n = MEM32(n + 8);
        k++;
        if (n == head) break;
    }
    return 0;
}

/* A header link is the previous physical chunk, so it must be an aligned
 * address below the chunk and inside the heap. Never dereference it here: a
 * corrupted link points at unmapped pages, and reading it kills the probe (and
 * the tombstone) before it can say what the link was. */
static int plausible_prev(uint32_t block, uint32_t p) {
    return p && (p & 3u) == 0 && p >= KA_HEAP_BASE && p < KA_HEAP_END &&
           p + 8u <= block;
}

/* A header change between two observations is suspicious only if it cannot be
 * the previous neighbour's free: that legitimately rewrites this chunk's link
 * and sets size bit 1. An aligned-size change, an implausible link with bit 1
 * set, or a link that went to 0 while bit 1 says "previous is free" is not. */
static int suspicious_change(uint32_t block, uint32_t oprev, uint32_t osize,
                             uint32_t prev, uint32_t size) {
    if ((osize & ~3u) != (size & ~3u)) return 1;
    if ((size & 2u) && !plausible_prev(block, prev)) return 1;
    if (oprev && !prev && (size & 2u)) return 1;
    return 0;
}

/* ---- allocation snapshots ------------------------------------------------ */

/* Open addressing with tombstones: deleting an entry in the middle of a probe
 * cluster must not hide the entries inserted behind it (the first version
 * cleared the slot and the free then stopped at it, which is why most frees
 * reported alloc=no). A block address is 16-byte aligned, so 1 is a safe
 * tombstone. */
static pool_snap *snap_find(uint32_t block, int for_insert) {
    uint32_t h = (block >> 4) & POOL_SNAP_MASK;
    pool_snap *tomb = NULL;
    for (uint32_t i = 0; i < 128; i++) {
        pool_snap *e = &s_snap[(h + i) & POOL_SNAP_MASK];
        if (e->block == block) return e;
        if (e->block == POOL_SNAP_TOMB) { if (!tomb) tomb = e; continue; }
        if (e->block == 0) {
            if (!for_insert) return NULL;
            e = tomb ? tomb : e;
            e->block = block;
            return e;
        }
    }
    if (for_insert && tomb) { tomb->block = block; return tomb; }
    return NULL;
}

void ka_pool_probe_alloc(uint32_t block) {
    pool_snap *e;
    if (!ka_pool_probe_on()) return;
    if (!block || !readable(block - 8, 12)) return;
    e = snap_find(block, 1);
    if (!e) { s_snap_full++; return; }   /* table full: this block loses it */
    e->seq = ++s_aseq;
    e->tid = ka_guest_tid();
    e->prev = MEM32(block - 8);
    e->size = MEM32(block - 4);
    e->word = MEM32(block);
    s_alloc++;
}

/* ---- free ----------------------------------------------------------------- */

static void log_rec(const pool_rec *r, const char *why) {
    uint32_t l_prev = r->has_locked ? r->l_prev : 0xDEADDEADu;
    uint32_t l_size = r->has_locked ? r->l_size : 0xDEADDEADu;
    uint32_t p_prev = r->has_post ? r->p_prev : 0xDEADDEADu;
    uint32_t p_size = r->has_post ? r->p_size : 0xDEADDEADu;
    ka_log("[poolprobe] %s #%u blk=%08X pre=%08X/%08X w=%08X size=%u "
           "alloc=%s %08X/%08X post=%08X/%08X postfree=%08X/%08X bin=%d "
           "self=%d prev=%d tid=%u",
           why, r->seq, r->block, r->h_prev, r->h_size, r->h_word, r->size,
           r->has_alloc ? "yes" : "no", r->a_prev, r->a_size,
           l_prev, l_size, p_prev, p_size,
           r->bin, r->self, r->prevok, r->tid);
}

void ka_pool_probe_free(uint32_t idx, uint32_t block) {
    pool_rec r;
    pool_snap *a;
    uint32_t region, pool;
    if (!ka_pool_probe_on()) return;
    t_cur = NULL;
    if (!block || !readable(block - 8, 12)) return;

    memset(&r, 0, sizeof r);
    r.seq = ++s_seq;
    s_frees++;
    r.tid = ka_guest_tid();
    r.func = g_cur_func;
    r.idx = idx;
    r.block = block;

    if (readable(0xCA1EF0u, 4)) {
        pool = MEM32(0xCA1EF0u) + idx * 0x48u;
        r.pool = pool;
        if (readable(pool, 0x44)) {
            region = MEM32(pool + 0x38u);
            r.region = region;
            r.freeb = MEM32(pool + 0x40u);
        }
    }
    r.h_prev = MEM32(block - 8);
    r.h_size = MEM32(block - 4);
    r.h_word = MEM32(block);
    r.size = r.h_size & ~3u;
    r.bin = bin_of(r.size);
    r.prevok = plausible_prev(block, r.h_prev) ? 1 : 0;
    if (r.region && readable(r.region, 0x74))
        r.self = on_bin(r.region, r.bin, block - 8u) ? 1 : 0;

    /* The fault state: the size word says the previous chunk is free but the
     * link is not a chunk. The guest would call the unlink with it and die in
     * sub_00829560. Clearing bit 1 here makes the guest skip that unlink. */
    if ((r.h_size & 2u) && !r.prevok && guard_on()) {
        MEM32(block - 4) = r.h_size & ~2u;
        s_guard_hits++;
        ka_log("[poolprobe] GUARD blk=%08X hdr=%08X/%08X -> size=%08X",
               block, r.h_prev, r.h_size, r.h_size & ~2u);
    }

    /* What did the allocator hand out? A legitimate change since then is only
     * the previous neighbour's free (it writes this chunk's link and sets size
     * bit 1). An aligned size change, or a link that went to 0 while the
     * previous-free bit stayed, is not legitimate. */
    a = snap_find(block, 0);
    if (a) {
        r.has_alloc = 1;
        r.a_seq = a->seq;
        r.a_tid = a->tid;
        r.a_prev = a->prev;
        r.a_size = a->size;
        r.a_word = a->word;
        s_with_snap++;
        if (a->size & 2u) {
            s_alloc_bit1++;
            if (!plausible_prev(block, a->prev)) {
                s_alloc_bad++;
                ka_log("[poolprobe] ALLOCBAD blk=%08X alloc=%08X/%08X "
                       "free=%08X/%08X tid=%u alloc_tid=%u",
                       block, a->prev, a->size, r.h_prev, r.h_size,
                       r.tid, a->tid);
            }
        }
        if ((a->size & ~3u) != r.size) {
            s_size_chg++;
            ka_log("[poolprobe] SIZECHG blk=%08X alloc=%08X/%08X "
                   "free=%08X/%08X tid=%u alloc_tid=%u",
                   block, a->prev, a->size, r.h_prev, r.h_size,
                   r.tid, a->tid);
        } else if (a->prev && !r.h_prev && (r.h_size & 2u)) {
            s_link_zeroed++;
            ka_log("[poolprobe] LINKZEROED blk=%08X alloc=%08X/%08X "
                   "free=%08X/%08X tid=%u alloc_tid=%u",
                   block, a->prev, a->size, r.h_prev, r.h_size,
                   r.tid, a->tid);
        }
        memset(a, 0, sizeof *a);
        a->block = POOL_SNAP_TOMB;
    } else {
        s_no_snap++;
    }

    s_ring[r.seq % POOL_PROBE_RING] = r;
    t_cur = &s_ring[r.seq % POOL_PROBE_RING];

    /* One compact line per free for the controls (capped); an already-listed
     * chunk or an inconsistent header is always logged. */
    if (s_logged < POOL_PROBE_LOG_CAP || r.self ||
        ((r.size & 2u) && !r.prevok)) {
        s_logged++;
        log_rec(&r, "free");
    }
    if ((s_frees % 1024u) == 0u) {
        ka_log("[poolprobe] stats frees=%u alloc=%u with_snap=%u no_snap=%u "
               "alloc_bit1=%u alloc_bad=%u size_chg=%u link_zeroed=%u "
               "locked_chg=%u post_chg=%u guard=%u snap_full=%u",
               s_frees, s_alloc, s_with_snap, s_no_snap, s_alloc_bit1,
               s_alloc_bad, s_size_chg, s_link_zeroed, s_locked_chg,
               s_post_chg, s_guard_hits, s_snap_full);
    }
}

/* Called again once the pool's critical section is held: anything different
 * from the pre-lock read was written by another thread while we waited. */
void ka_pool_probe_locked(void) {
    pool_rec *r = t_cur;
    uint32_t prev, size;
    if (!r || r->has_locked || !readable(r->block - 8, 12)) return;
    prev = MEM32(r->block - 8);
    size = MEM32(r->block - 4);
    r->has_locked = 1;
    r->l_prev = prev;
    r->l_size = size;
    if (prev != r->h_prev || size != r->h_size) {
        s_locked_chg++;
        if (suspicious_change(r->block, r->h_prev, r->h_size, prev, size) &&
            s_log_cap++ < 200)
            log_rec(r, "POSTLOCK");
    }
}

/* Called at the free's own LeaveCriticalSection: the free has completed (or
 * returned early), so this is the header the next allocator will see. */
void ka_pool_probe_postfree(void) {
    pool_rec *r = t_cur;
    uint32_t prev, size;
    if (!r) return;
    t_cur = NULL;
    if (r->has_post || !readable(r->block - 8, 12)) return;
    prev = MEM32(r->block - 8);
    size = MEM32(r->block - 4);
    r->has_post = 1;
    r->p_prev = prev;
    r->p_size = size;
    if (prev != r->h_prev || size != r->h_size) {
        s_post_chg++;
        if (suspicious_change(r->block, r->h_prev, r->h_size, prev, size) &&
            s_log_cap++ < 200)
            log_rec(r, "POSTFREE");
    }
}

/* Raw-fd dump so it survives a broken stdio state in the crash handler. */
void ka_pool_probe_dump(const char *why) {
    uint32_t i, n;
    if (!ka_pool_probe_on() || !s_seq) return;
    n = s_seq < POOL_PROBE_RING ? s_seq : POOL_PROBE_RING;
    {
        char head[256];
        int len = snprintf(head, sizeof head,
                           "[poolprobe] counters frees=%u alloc=%u with_snap=%u "
                           "no_snap=%u alloc_bit1=%u alloc_bad=%u size_chg=%u "
                           "link_zeroed=%u locked_chg=%u post_chg=%u guard=%u "
                           "snap_full=%u\n",
                           s_frees, s_alloc, s_with_snap, s_no_snap,
                           s_alloc_bit1, s_alloc_bad, s_size_chg,
                           s_link_zeroed, s_locked_chg, s_post_chg, s_guard_hits,
                           s_snap_full);
        if (len > 0) { ssize_t w = write(2, head, (size_t)len); (void)w; }
    }
    for (i = 0; i < n; i++) {
        uint32_t seq = s_seq - n + 1 + i;
        const pool_rec *r = &s_ring[seq % POOL_PROBE_RING];
        char buf[400];
        int len;
        if (r->seq != seq) continue;
        len = snprintf(buf, sizeof buf,
                       "[poolprobe] %s #%u blk=%08X pre=%08X/%08X w=%08X "
                       "size=%u alloc=%s %08X/%08X post=%08X/%08X "
                       "postfree=%08X/%08X bin=%d self=%d prev=%d tid=%u\n",
                       why, r->seq, r->block, r->h_prev, r->h_size,
                       r->h_word, r->size,
                       r->has_alloc ? "yes" : "no", r->a_prev, r->a_size,
                       r->has_locked ? r->l_prev : 0xDEADDEADu,
                       r->has_locked ? r->l_size : 0xDEADDEADu,
                       r->has_post ? r->p_prev : 0xDEADDEADu,
                       r->has_post ? r->p_size : 0xDEADDEADu,
                       r->bin, r->self, r->prevok, r->tid);
        if (len > 0) { ssize_t w = write(2, buf, (size_t)len); (void)w; }
    }
}
