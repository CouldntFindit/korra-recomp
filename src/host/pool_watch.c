/*
 * pool_watch.c - unbiased watch of the engine pool's block argument
 * (debug only, inert unless KORRA_POOLWATCH is set).
 *
 * The 2026-11-27 oracle run used `--watch 0x00828D80 --watchspan -8 16`, which
 * dumps the object under ECX. On entry to the pool free (sub_00828D80) ECX is
 * only sometimes the block: the function is `sub_00828D80(idx, block)` and the
 * caller's ECX is usually free to be anything (the log shows `ecx=00000001`,
 * `ecx=C0000002`). The block is stack argument 1, i.e. MEM32(esp+8) before the
 * prologue, so the earlier sample (268 readable frees) was biased to the
 * callers that happened to leave the block in ECX, and the span dump itself
 * faulted at 86.7 s reading 0xBFFFFFE2.
 *
 * This probe fixes that and adds the allocation half:
 *
 *  - at every entry to the pool free it reads args[1] and records
 *    {[block-8], [block-4], [block]} (validity: 4-aligned, inside mapped
 *    memory, below the chunk). Counts: frees, size bit 1, link 0, bit 1 with an
 *    implausible link, size 0.
 *  - at the allocation's own LeaveCriticalSection inside sub_00829000 the
 *    generated code already holds the returned block in EDI, so a snapshot is
 *    stored for that block. The free compares against it, which separates
 *    "already zero at allocation" from a write during the block's lifetime; the
 *    one legitimate change (the previous neighbour's free rewrites the link and
 *    sets size bit 1) is not reported, an aligned-size change or a link that
 *    went to 0 while bit 1 stayed is.
 *
 * Span reads are guarded per 4 bytes with VirtualQuery: the old probe died on
 * a wild base, not on guest state, and that tombstone must not be repeated.
 * No behaviour depends on any of this; every store still happens.
 *
 * Needs the -DRECOMP_TRACE build (scripts\build_host.ps1 -Trace): the free hook
 * rides recomp_trace_extra, which RECOMP_ENTER calls on every lifted function.
 */
#ifdef RECOMP_TRACE
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "recomp_types.h"
#include "native32.h"

#define POOL_FREE_VA   0x00828D80u
#define POOL_ALLOC_VA  0x00829000u

static uint32_t s_leave_cs;     /* host LeaveCriticalSection, resolved once */

#define PW_SNAP_BITS   16
#define PW_SNAP_SIZE   (1u << PW_SNAP_BITS)
#define PW_SNAP_MASK   (PW_SNAP_SIZE - 1u)
#define PW_LOG_CAP     400u
#define PW_BAD_CAP     200u

typedef struct {
    uint32_t block;     /* 0 = free slot */
    uint32_t seq, tid;
    uint32_t prev, size, word;
} pw_alloc;

static pw_alloc s_snap[PW_SNAP_SIZE];
static uint32_t s_seq;
static uint32_t s_frees, s_bit1, s_bit1_ok, s_bit1_bad, s_zero, s_zero_bit1;
static uint32_t s_bitsize, s_nosnap, s_changed, s_alloc, s_unreadable;
static uint32_t s_size_chg, s_link_zeroed, s_alloc_bad, s_alloc_bit1;
static uint32_t s_logged, s_bad_logged, s_span_logged;
static int s_span;

/* Committed and readable for n bytes starting at va. VirtualQuery does not
 * touch the target, so a wild pointer cannot fault the probe here. */
static int readable(uint32_t va, uint32_t n) {
    uint32_t end = va + n;
    if (!va || end < va) return 0;
    while (va < end) {
        MEMORY_BASIC_INFORMATION mbi;
        uintptr_t next;
        if (VirtualQuery((const void *)(uintptr_t)va, &mbi, sizeof mbi)
                != sizeof mbi) return 0;
        if (mbi.State != MEM_COMMIT) return 0;
        if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return 0;
        next = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        if (next <= (uintptr_t)va) return 0;
        if (next >= (uintptr_t)end) break;
        va = (uint32_t)next;
    }
    return 1;
}

/* A header link is the previous physical chunk: 4-aligned, inside mapped
 * memory, below the chunk. It is never dereferenced as a chunk here (a
 * corrupted link points anywhere); only the address itself is validated. */
static int plausible_prev(uint32_t block, uint32_t p) {
    return p && (p & 3u) == 0 && p < block && p >= 0x10000u &&
           readable(p, 8);
}

static void span_line(const char *tag, uint32_t base, int row) {
    char buf[160];
    int len = snprintf(buf, sizeof buf, "[poolwatch]   %s[%08X+%02X]:",
                       tag, base, row);
    for (int k = 0; k < 0x20; k += 4) {
        uint32_t a = base + (uint32_t)row + (uint32_t)k;
        if (len < (int)sizeof buf - 12 && readable(a, 4))
            len += snprintf(buf + len, sizeof buf - (size_t)len, " %08X",
                            MEM32(a));
        else if (len < (int)sizeof buf - 8)
            len += snprintf(buf + len, sizeof buf - (size_t)len, " ????????");
    }
    if (len < (int)sizeof buf - 1) buf[len++] = '\n';
    fwrite(buf, 1, (size_t)len, stderr);
}

static void dump_span(uint32_t block) {
    uint32_t base = block & ~0x1Fu;
    int row = (int)(block - base);
    span_line("", base, -0x20);
    /* the row holding the header, then the payload's first row */
    for (int r = 0; r < 0x40; r += 0x20) {
        uint32_t a = base + (uint32_t)row + (uint32_t)r;
        if (!readable(a, 0x20)) continue;
        span_line("", a, 0);
    }
}

static void snap_put(uint32_t block, uint32_t prev, uint32_t size,
                     uint32_t word) {
    uint32_t h = (block >> 4) & PW_SNAP_MASK;
    for (uint32_t i = 0; i < 64; i++) {
        pw_alloc *e = &s_snap[(h + i) & PW_SNAP_MASK];
        if (e->block == block || e->block == 0) {
            e->block = block;
            e->seq = ++s_seq;
            e->tid = GetCurrentThreadId();
            e->prev = prev;
            e->size = size;
            e->word = word;
            return;
        }
    }
    s_nosnap++;   /* table full: the oldest block loses its snapshot */
}

static const pw_alloc *snap_get(uint32_t block) {
    uint32_t h = (block >> 4) & PW_SNAP_MASK;
    for (uint32_t i = 0; i < 64; i++) {
        const pw_alloc *e = &s_snap[(h + i) & PW_SNAP_MASK];
        if (e->block == block) return e;
        if (e->block == 0) return NULL;
    }
    return NULL;
}

static void snap_del(uint32_t block) {
    uint32_t h = (block >> 4) & PW_SNAP_MASK;
    for (uint32_t i = 0; i < 64; i++) {
        pw_alloc *e = &s_snap[(h + i) & PW_SNAP_MASK];
        if (e->block == block) { memset(e, 0, sizeof *e); return; }
        if (e->block == 0) return;
    }
}

/* The allocation's own LeaveCriticalSection: sub_00829000 has already put the
 * returned block in EDI and the generated code syncs registers before the
 * import, so g_edi is the block. No import sits between "chunk found" and the
 * return, so this is the only host-visible allocation point. On Windows
 * LeaveCriticalSection is a native import (not a shim), so the bridge passes
 * the resolved kernel32 address; match that, not the IAT slot. */
static void alloc_hook(uint32_t fn, uint32_t *args) {
    uint32_t block;
    (void)args;
    if (fn != s_leave_cs || g_cur_func != POOL_ALLOC_VA) return;
    block = g_edi;
    s_alloc++;
    if (!block || !readable(block - 8, 12)) return;
    snap_put(block, MEM32(block - 8), MEM32(block - 4), MEM32(block));
}

static void summary(void) {
    fprintf(stderr, "[poolwatch] summary frees=%u bit1=%u bit1_ok=%u "
                    "bit1_bad=%u zero_link=%u zero_bit1=%u size0=%u "
                    "alloc=%u nosnap=%u changed=%u size_chg=%u link_zeroed=%u "
                    "alloc_bad=%u alloc_bit1=%u unreadable=%u\n",
            s_frees, s_bit1, s_bit1_ok, s_bit1_bad, s_zero, s_zero_bit1,
            s_bitsize, s_alloc, s_nosnap, s_changed, s_size_chg,
            s_link_zeroed, s_alloc_bad, s_alloc_bit1, s_unreadable);
    fflush(stderr);
}

static void free_hook(uint32_t va) {
    uint32_t idx, block, prev, size, word, sizev;
    const pw_alloc *a;
    int bit1, prevok;

    if (va != POOL_FREE_VA) return;
    idx = MEM32(g_esp + 4);         /* args[0]: pool index */
    block = MEM32(g_esp + 8);       /* args[1]: the block, not ECX */
    s_frees++;
    if (!block || !readable(block - 8, 12)) { s_unreadable++; return; }

    prev = MEM32(block - 8);
    size = MEM32(block - 4);
    word = MEM32(block);
    sizev = size & ~3u;
    bit1 = (size & 2u) != 0;
    prevok = plausible_prev(block, prev);
    if (bit1) { s_bit1++; if (prevok) s_bit1_ok++; else s_bit1_bad++; }
    if (!prev) { s_zero++; if (bit1) s_zero_bit1++; }
    if (!sizev) s_bitsize++;

    a = snap_get(block);
    if (!a) s_nosnap++;
    else {
        if (a->size & 2u) {
            s_alloc_bit1++;
            if (!plausible_prev(block, a->prev) && ++s_alloc_bad <= PW_BAD_CAP)
                fprintf(stderr, "[poolwatch] ALLOCBAD idx=%u blk=%08X "
                                "alloc hdr=%08X/%08X free hdr=%08X/%08X "
                                "tid=%lu\n",
                        idx, block, a->prev, a->size, prev, size,
                        GetCurrentThreadId());
        }
        /* A change here is normal in one shape: the previous physical chunk
         * was freed during this block's lifetime, so the neighbour wrote this
         * chunk's link and set size bit 1. What is not normal is the aligned
         * size changing (a write past some chunk's payload) or the link going
         * to 0 while the previous-free bit stays set. */
        if (a->prev != prev || a->size != size) {
            s_changed++;
            if ((a->size & ~3u) != sizev) {
                if (++s_size_chg <= PW_BAD_CAP)
                    fprintf(stderr, "[poolwatch] SIZECHG idx=%u blk=%08X "
                                    "alloc hdr=%08X/%08X free hdr=%08X/%08X "
                                    "tid=%lu\n",
                            idx, block, a->prev, a->size, prev, size,
                            GetCurrentThreadId());
            }
            if (a->prev && !prev && bit1) {
                if (++s_link_zeroed <= PW_BAD_CAP)
                    fprintf(stderr, "[poolwatch] LINKZEROED idx=%u blk=%08X "
                                    "alloc hdr=%08X/%08X free hdr=%08X/%08X "
                                    "tid=%lu\n",
                            idx, block, a->prev, a->size, prev, size,
                            GetCurrentThreadId());
            }
        }
        snap_del(block);
    }

    if (s_logged < PW_LOG_CAP) {
        s_logged++;
        fprintf(stderr, "[poolwatch] free idx=%u blk=%08X hdr=%08X/%08X/%08X "
                        "size=%u bit1=%d prev=%d tid=%lu snap=%s\n",
                idx, block, prev, size, word, sizev, bit1, prevok,
                GetCurrentThreadId(), a ? "yes" : "no");
        if (s_span && s_span_logged < 100) { s_span_logged++; dump_span(block); }
        fflush(stderr);
    }
    if (!prevok && (bit1 || (size & 1u))) {
        if (++s_bad_logged <= PW_BAD_CAP) {
            fprintf(stderr, "[poolwatch] SUSPECT idx=%u blk=%08X "
                            "hdr=%08X/%08X/%08X size=%u bit1=%d tid=%lu\n",
                    idx, block, prev, size, word, sizev, bit1,
                    GetCurrentThreadId());
            dump_span(block);
            fflush(stderr);
        }
    }
    if ((s_frees % 500u) == 0u) summary();
}

void korra_pool_watch_init(void) {
    extern void (*recomp_trace_extra)(uint32_t va);
    if (!getenv("KORRA_POOLWATCH")) return;
    s_span = getenv("KORRA_POOLWATCH_SPAN") != NULL;
    s_leave_cs = (uint32_t)(uintptr_t)GetProcAddress(
        GetModuleHandleA("kernel32.dll"), "LeaveCriticalSection");
    /* Both hooks are exclusive slots; another probe may already own one (it is
     * not run together with this one in practice). */
    fprintf(stderr, "[poolwatch] on (free=%08X alloc=%08X span=%d)\n",
            POOL_FREE_VA, POOL_ALLOC_VA, s_span);
    recomp_trace_extra = free_hook;
    native32_pre_hook = alloc_hook;
    atexit(summary);
}
#else  /* not -DRECOMP_TRACE: hooks do not exist, the probe is inert */
void korra_pool_watch_init(void) { }
#endif /* RECOMP_TRACE */
