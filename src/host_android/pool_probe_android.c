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
 * Free-list layout (from sub_00829560 / sub_00829880): the chunk is
 * block-8; [chunk]=prev physical chunk link, [chunk+4]=size word,
 * [chunk+8]=fd and [chunk+0xC]=bk on the region's bin list, whose head is
 * region + 0x74 + bin*4 and whose sentinel is the region itself. The bin is
 * derived from (size & ~3) exactly as the unlink does it.
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
} pool_rec;

static pool_rec s_ring[POOL_PROBE_RING];
static uint32_t s_seq;
static uint32_t s_logged;
static int      s_on = -1;   /* -1 unknown, 0 off, 1 on */

int ka_pool_probe_on(void) {
    if (s_on < 0) {
        char v[PROP_VALUE_MAX] = {0};
        __system_property_get("debug.korra.poolprobe", v);
        s_on = (v[0] == '1') ? 1 : 0;
        if (s_on) ka_log("[poolprobe] on (debug.korra.poolprobe=1)");
    }
    return s_on;
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

void ka_pool_probe_free(uint32_t idx, uint32_t block) {
    pool_rec r;
    uint32_t region, pool;
    if (!ka_pool_probe_on()) return;
    if (!block || !readable(block - 8, 12)) return;

    memset(&r, 0, sizeof r);
    r.seq = ++s_seq;
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

    s_ring[r.seq % POOL_PROBE_RING] = r;

    /* One compact line per free for the controls (capped); an already-listed
     * chunk or an inconsistent header is always logged. */
    if (s_logged < POOL_PROBE_LOG_CAP || r.self ||
        ((r.size & 2u) && !r.prevok)) {
        s_logged++;
        ka_log("[poolprobe] free idx=%u block=%08X hdr=%08X/%08X/%08X size=%u "
               "pool=%08X region=%08X freeb=%u bin=%d self=%d prev=%d tid=%u%s",
               idx, block, r.h_prev, r.h_size, r.h_word, r.size, r.pool,
               r.region, r.freeb, r.bin, r.self, r.prevok, r.tid,
               (r.self ? " ALREADY" :
                (((r.size & 2u) && !r.prevok) ? " SUSPECT" : "")));
    }
}

/* Raw-fd dump so it survives a broken stdio state in the crash handler. */
void ka_pool_probe_dump(const char *why) {
    uint32_t i, n;
    if (!ka_pool_probe_on() || !s_seq) return;
    n = s_seq < POOL_PROBE_RING ? s_seq : POOL_PROBE_RING;
    for (i = 0; i < n; i++) {
        uint32_t seq = s_seq - n + 1 + i;
        const pool_rec *r = &s_ring[seq % POOL_PROBE_RING];
        char buf[320];
        int len;
        if (r->seq != seq) continue;
        len = snprintf(buf, sizeof buf,
                       "[poolprobe] %s #%u block=%08X hdr=%08X/%08X/%08X "
                       "size=%u pool=%08X region=%08X freeb=%u bin=%d "
                       "self=%d prev=%d tid=%u\n",
                       why, r->seq, r->block, r->h_prev, r->h_size, r->h_word,
                       r->size, r->pool, r->region, r->freeb, r->bin,
                       r->self, r->prevok, r->tid);
        if (len > 0) { ssize_t w = write(2, buf, (size_t)len); (void)w; }
    }
}
