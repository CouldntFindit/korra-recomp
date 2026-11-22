/* memory_android.c - the guest address space and its allocators.
 *
 * One 4 GiB PROT_NONE reservation represents the whole 32-bit guest space.
 * g_mem_base is the host address of guest VA 0, so ADDR(va) lands inside it and
 * any wild guest pointer faults on a page the host never mapped (that is the
 * "huge address" class in AGENTS.md section 5).
 *
 * Regions:
 *   0x00400000  the image's sections (mapped by pe_android.c)
 *   0x20000000  1 GiB guest heap, committed RW up front (pages stay lazy)
 *   0xC0000000  guest thread stacks, bump-allocated
 *   0xF0000000  synthetic import (shim) VAs
 *
 * Page size is read at runtime: this device uses 16 KiB pages and mprotect
 * rejects a 4 KiB-aligned range there.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <ucontext.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/system_properties.h>
#include <unistd.h>

#include "host_android.h"

static uint8_t *g_win;
static size_t   g_page = 4096;

/* ---- heap ------------------------------------------------------------- */
#define HEAP_SIZE  0x40000000u   /* 1 GiB of address space, lazily backed */
#define BLK_MAGIC  0x4B41524Bu
/* LoK.exe's own SizeOfStackReserve (PE optional header: reserve 0x100000,
 * commit 0x1000). CreateThread(dwStackSize=0) means this value on Windows, so
 * this is the guest's contract, not a budget guess: 4 MiB per thread was four
 * times what the engine reserves and it is what capped the process at 64
 * concurrent guest threads. The constant lives in host_android.h. */

typedef struct blk {
    uint32_t size;      /* payload bytes, header excluded */
    uint32_t magic;     /* BLK_MAGIC when free */
    struct blk *next;   /* free list only */
} blk;

static blk *g_free_list;
static uint32_t g_heap_hi, g_heap_end;
/* Free-list census maintained at the two places the list changes, so the 1 Hz
 * sampler never walks a list the guest is mutating (the guest holds the machine
 * lock; the sampler must not take it). */
static uint32_t g_free_blocks, g_free_bytes;

static uint32_t g_stack_hi;
static uint32_t g_stack_end;

/* Stacks handed back by threads that ended. The node lives in the block it
 * describes (the block is dead memory once its thread is gone), so a release
 * costs nothing and a level load that spawns a worker per stream reuses what
 * the previous one returned instead of spending the region. */
typedef struct stkfree {
    struct stkfree *next;
    uint32_t lo, size;
} stkfree;
static stkfree *g_stack_free;
static uint32_t g_stack_free_b, g_stack_free_bytes;
static pthread_mutex_t g_stack_lock = PTHREAD_MUTEX_INITIALIZER;

int ka_mem_init(void) {
    long ps = sysconf(_SC_PAGESIZE);
    if (ps > 0) g_page = (size_t)ps;

    void *p = mmap(NULL, (size_t)KA_WINDOW_SIZE, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) {
        fprintf(stderr, "korra: 4 GiB guest window reservation failed: %s\n", strerror(errno));
        return -1;
    }
    g_win = (uint8_t *)p;
    g_mem_base = (ptrdiff_t)g_win;

    /* Commit the heap RW: address space only, pages back on first touch. */
    if (mprotect(g_win + KA_HEAP_BASE, HEAP_SIZE, PROT_READ | PROT_WRITE) != 0) {
        fprintf(stderr, "korra: heap commit failed: %s\n", strerror(errno));
        return -1;
    }
    g_heap_hi  = KA_HEAP_BASE;
    g_heap_end = KA_HEAP_BASE + HEAP_SIZE;

    g_stack_hi  = 0xC0000000u;
    g_stack_end = 0xD0000000u;
    if (mprotect(g_win + g_stack_hi, g_stack_end - g_stack_hi,
                 PROT_READ | PROT_WRITE) != 0) {
        fprintf(stderr, "korra: stack region commit failed: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

void *ka_host(uint32_t va) { return g_win + va; }

int ka_commit(uint32_t va, uint32_t size, int prot_none) {
    uintptr_t lo = (uintptr_t)va & ~(uintptr_t)(g_page - 1);
    uintptr_t hi = ((uintptr_t)va + size + g_page - 1) & ~(uintptr_t)(g_page - 1);
    int prot = prot_none ? PROT_NONE : (PROT_READ | PROT_WRITE);
    return mprotect(g_win + lo, hi - lo, prot) == 0 ? 0 : -1;
}

/* Diagnostics for the graphics-init abort: did a 16 MB request ever reach
 * this arena, and how much has the engine taken in total? */
uint32_t ka_alloc_calls, ka_alloc_bytes, ka_alloc_max;
static int g_big_logs;

uint32_t ka_guest_alloc(uint32_t size, uint32_t align) {
    if (!size) size = 16;
    ka_alloc_calls++;
    ka_alloc_bytes += size;
    if (size > ka_alloc_max) ka_alloc_max = size;
    if (!align) align = 16;
    if (align < 16) align = 16;
    size = (size + 15u) & ~15u;

    /* First fit. Blocks are freed into a singly linked list; adjacent blocks
     * are not coalesced (guest churn is small compared with 1 GiB). */
    blk **pp = &g_free_list;
    while (*pp) {
        blk *b = *pp;
        if (b->size >= size) {
            *pp = b->next;
            b->magic = 0;
            g_free_blocks--;
            g_free_bytes -= b->size;
            uint32_t va = (uint32_t)((uint8_t *)b - g_win) + (uint32_t)sizeof(blk);
            va = (va + align - 1u) & ~(align - 1u);
            if (size >= (4u << 20) && g_big_logs < 60) {
                g_big_logs++;
                ka_log("alloc: size=%u -> %08X (freed block)", size, va);
            }
            return va;
        }
        pp = &b->next;
    }
    uint32_t need = size + (uint32_t)sizeof(blk) + align;
    if (g_heap_hi + need > g_heap_end) {
        ka_log("guest heap exhausted (want %u bytes at 0x%08X)", size, g_heap_hi);
        return 0;
    }
    blk *b = (blk *)(g_win + g_heap_hi);
    g_heap_hi += need;
    b->size = size;
    b->magic = 0;
    uint32_t va2 = (uint32_t)((uint8_t *)b - g_win) + (uint32_t)sizeof(blk);
    va2 = (va2 + align - 1u) & ~(align - 1u);
    if (size >= (4u << 20) && g_big_logs < 60) {
        g_big_logs++;
        ka_log("alloc: size=%u -> %08X (bump, hi=%08X)", size, va2, g_heap_hi);
    }
    return va2;
}

void ka_guest_free(uint32_t va) {
    if (!va) return;
    blk *b = (blk *)(g_win + va - sizeof(blk));
    if (b->magic == BLK_MAGIC) return;     /* double free */
    b->magic = BLK_MAGIC;
    b->next = g_free_list;
    g_free_list = b;
    g_free_blocks++;
    g_free_bytes += b->size;
}

uint32_t ka_guest_size(uint32_t va) {
    if (!va) return 0;
    blk *b = (blk *)(g_win + va - sizeof(blk));
    return b->size;
}

uint32_t ka_guest_strdup(const char *s) {
    if (!s) return 0;
    size_t n = strlen(s) + 1;
    uint32_t va = ka_guest_alloc((uint32_t)n, 1);
    if (va) memcpy(ka_host(va), s, n);
    return va;
}

uint32_t ka_stack_new(uint32_t size, uint32_t *limit) {
    uint32_t lo;
    if (size < 0x10000u) size = 0x10000u;
    size = (size + g_page - 1u) & ~(uint32_t)(g_page - 1u);

    pthread_mutex_lock(&g_stack_lock);
    {
        stkfree **pp = &g_stack_free;
        while (*pp) {
            stkfree *b = *pp;
            if (b->size >= size) {
                *pp = b->next;
                g_stack_free_b--;
                g_stack_free_bytes -= b->size;
                lo = b->lo;
                pthread_mutex_unlock(&g_stack_lock);
                memset(ka_host(lo), 0, 64);
                if (limit) *limit = lo;
                return lo + size - 64u;
            }
            pp = &b->next;
        }
    }
    if (g_stack_hi + size > g_stack_end) {
        ka_log("guest stack space exhausted (hi=%08X end=%08X want=%u "
               "free=%u/%uK)", g_stack_hi, g_stack_end, size,
               g_stack_free_b, g_stack_free_bytes / 1024u);
        pthread_mutex_unlock(&g_stack_lock);
        return 0;
    }
    lo = g_stack_hi;
    g_stack_hi += size;
    pthread_mutex_unlock(&g_stack_lock);
    if (limit) *limit = lo;
    memset(ka_host(lo), 0, 64);
    return lo + size - 64u;
}

void ka_stack_release(uint32_t lo, uint32_t size) {
    stkfree *b;
    if (!lo || size < 0x10000u || lo < 0xC0000000u || lo >= g_stack_end) return;
    if (lo + size > g_stack_end) return;
    b = (stkfree *)ka_host(lo);
    pthread_mutex_lock(&g_stack_lock);
    b->lo = lo;
    b->size = size;
    b->next = g_stack_free;
    g_stack_free = b;
    g_stack_free_b++;
    g_stack_free_bytes += size;
    pthread_mutex_unlock(&g_stack_lock);
}

void ka_stack_stats(uint32_t *hi, uint32_t *end, uint32_t *free_blocks,
                    uint32_t *free_bytes) {
    if (hi) *hi = g_stack_hi;
    if (end) *end = g_stack_end;
    if (free_blocks) *free_blocks = g_stack_free_b;
    if (free_bytes) *free_bytes = g_stack_free_bytes;
}

void ka_heap_stats(uint32_t *hi, uint32_t *free_blocks, uint32_t *free_bytes) {
    if (hi) *hi = g_heap_hi;
    if (free_blocks) *free_blocks = g_free_blocks;
    if (free_bytes) *free_bytes = g_free_bytes;
}

/* ---- engine pool inspection -------------------------------------------- */
/* The graphics-init abort is the engine's own memory manager refusing a 16 MB
 * pool, without any host allocation call. Its allocator (vtable 0x0096C930,
 * core 0x0073E9C0) is a segregated-fit pool: class count at +0x58, class heads
 * array at +0x40 indexed (fl*4 + sl), per-class bitmaps at +0x3C/+0x38, used
 * list head at +0x44, used count at +0x54, alignment at +0x48, lock flag
 * +0x20. Chunks: neighbour tags -0x8/-0xC, self tag +0x18 = payload size,
 * owner +0x1C, and the same-class chain at +0x10.
 * 0x00767630 passes A0 = [0xDB8284] and a one-entry fallback list built from
 * *(0xDB8284+0x14); A0's class index for 16 MB is out of range on Windows too,
 * so Windows is served by that fallback. Dump both, before and after the
 * request, and the delta names the refusal branch. */
static int pool_ok(uint32_t va) {
    if (va & 3u) return 0;
    if (va >= KA_HEAP_BASE && va < KA_HEAP_END) return 1;
    if (va >= KA_IMAGE_BASE && va < 0x01000000u) return 1;
    return 0;
}
static uint32_t pool_u32(uint32_t va) { return pool_ok(va) ? MEM32(va) : 0; }

static void pool_dump_obj(const char *tag, const char *name, uint32_t o) {
    uint32_t vt, fl, heads, i, nodes = 0, sum = 0, big = 0, bigc = 0;
    if (!pool_ok(o)) {
        ka_log("%s %s=%08X <not a pool>", tag, name, o);
        return;
    }
    vt = MEM32(o);
    fl = pool_u32(o + 0x58);
    heads = pool_u32(o + 0x40);
    ka_log("%s %s=%08X vt=%08X +8=%08X +C=%08X +10=%08X +14=%08X +18=%08X "
           "+1C=%08X +20=%08X +38=%08X +3C=%08X +40=%08X +44=%08X +48=%08X "
           "+54=%u +58=%u +5C=%08X",
           tag, name, o, vt, pool_u32(o + 8), pool_u32(o + 0xC),
           pool_u32(o + 0x10), pool_u32(o + 0x14), pool_u32(o + 0x18),
           pool_u32(o + 0x1C), pool_u32(o + 0x20), pool_u32(o + 0x38),
           pool_u32(o + 0x3C), heads, pool_u32(o + 0x44),
           pool_u32(o + 0x48), pool_u32(o + 0x54), fl, pool_u32(o + 0x5C));

    if (pool_ok(heads) && fl <= 64) {
        uint32_t ncls = fl * 4u;
        for (i = 0; i < ncls; i++) {
            uint32_t n = pool_u32(heads + i * 4u), k = 0, csum = 0, cbig = 0;
            while (pool_ok(n) && k < 512) {
                uint32_t sz = pool_u32(n + 0x18);
                csum += sz;
                if (sz > cbig) cbig = sz;
                k++;
                n = pool_u32(n + 0x10);
            }
            if (k) {
                nodes += k;
                sum += csum;
                if (cbig > big) { big = cbig; bigc = i; }
                ka_log("%s %s free[%u] (fl=%u sl=%u) n=%u largest=%u",
                       tag, name, i, i >> 2, i & 3u, k, cbig);
            }
        }
    }
    ka_log("%s %s free total: nodes=%u sum=%u largest=%u@%u",
           tag, name, nodes, sum, big, bigc);

    {
        uint32_t c = pool_u32(o + 0x44), k = 0;
        while (pool_ok(c) && k < 64) {
            ka_log("%s %s used[%u] %08X sz=%u own=%08X", tag, name, k, c,
                   pool_u32(c + 0x18), pool_u32(c + 0x1C));
            c = pool_u32(c + 0x10);
            k++;
        }
        ka_log("%s %s used end k=%u", tag, name, k);
    }
}

void ka_pool_dump(const char *tag) {
    uint32_t m = pool_u32(0xDB8284);
    uint32_t cell, fb;
    if (!pool_ok(m)) {
        ka_log("%s mgr=%08X <not mapped>", tag, m);
        return;
    }
    cell = pool_u32(m + 0x14);
    fb = pool_u32(cell);
    ka_log("%s mgr=%08X vt=%08X +8=%08X +C=%08X +10=%08X +14=%08X +18=%08X "
           "+1C=%08X +20=%08X +44=%08X +48=%08X +4C=%08X",
           tag, m, pool_u32(m), pool_u32(m + 8), pool_u32(m + 0xC),
           pool_u32(m + 0x10), cell, pool_u32(m + 0x18), pool_u32(m + 0x1C),
           pool_u32(m + 0x20), pool_u32(m + 0x44), pool_u32(m + 0x48),
           pool_u32(m + 0x4C));
    /* sub_00847BB0 rep-movsd's the caller's struct (0xDB8AE8) over 0xCA1FCC,
     * and cfg+0xC (CA1FD8) is the pool size the next step asks for: 0x300000
     * from the caller, or 0x01000000 from sub_008479C0 when arg1 was NULL.
     * The record array sub_00829140 fills says whether the request got as far
     * as a pool record. Together they separate "the copy did not land" from
     * "the allocator refused". */
    {
        char line[400];
        int n = snprintf(line, sizeof line, "%s cfg=", tag);
        uint32_t i;
        for (i = 0; i < 14 && n > 0 && n < (int)sizeof line - 16; i++)
            n += snprintf(line + n, sizeof line - (size_t)n, "%x:%08X ",
                          i * 4, pool_u32(0xCA1FCC + i * 4));
        ka_log("%s", line);
        ka_log("%s pools ready=%02X n=%u cap=%u arr=%08X", tag,
               pool_u32(0xCA1EF4) & 0xFFu, pool_u32(0xCA1EEC),
               pool_u32(0xCA1EE8), pool_u32(0xCA1EF0));
        {
            uint32_t arr = pool_u32(0xCA1EF0);
            for (i = 0; arr && pool_ok(arr) && i < 5; i++) {
                uint32_t r = arr + i * 0x48u;
                ka_log("%s rec[%u] %08X chunk=%08X end=%08X mode=%08X "
                       "gran=%08X size=%08X", tag, i, r, pool_u32(r),
                       pool_u32(r + 8), pool_u32(r + 0xC), pool_u32(r + 0x18),
                       pool_u32(r + 0x1C), pool_u32(r + 0x44));
            }
        }
        /* The caller's struct 0x7657D0 prepares (0xDB8AE8) versus a window
         * around 0xCA1FCC, the destination of the rep movsd at 0x847C24.
         * Both ends in one line pair: if the source is filled and the window
         * holds no copy of it, the string move never wrote; if the pattern
         * sits at a shifted address, the destination is off. */
        {
            char src[300], win[300];
            int ns = snprintf(src, sizeof src, "%s src=", tag);
            int nw = snprintf(win, sizeof win, "%s win=", tag);
            uint32_t a;
            for (i = 0; i < 14 && ns > 0 && ns < (int)sizeof src - 12; i++)
                ns += snprintf(src + ns, sizeof src - (size_t)ns, "%x:%08X ",
                               i * 4, pool_u32(0xDB8AE8 + i * 4));
            ka_log("%s", src);
            /* 0xCA1F40..0xCA2040: covers a copy shifted by up to 0x8C bytes
             * in either direction from 0xCA1FCC. */
            for (a = 0xCA1F40; a < 0xCA2060 && nw > 0; a += 0x30) {
                nw = snprintf(win, sizeof win, "%s win %08X:", tag, a);
                for (i = 0; i < 12 && nw > 0 && nw < (int)sizeof win - 12; i++)
                    nw += snprintf(win + nw, sizeof win - (size_t)nw, " %08X",
                                   pool_u32(a + i * 4));
                ka_log("%s", win);
            }
            ka_log("%s tail CA2004=%08X 9B4394=%08X 9B4388=%08X 9B438C=%08X",
                   tag, pool_u32(0xCA2004), pool_u32(0x9B4394),
                   pool_u32(0x9B4388), pool_u32(0x9B438C));
        }
    }
    if (!pool_ok(cell)) {
        ka_log("%s fallback cell=%08X <none> (no fallback allocator)",
               tag, cell);
        pool_dump_obj(tag, "a0", pool_u32(m));
        return;
    }
    ka_log("%s fallback cell=%08X -> %08X", tag, cell, fb);
    pool_dump_obj(tag, "a0", pool_u32(m));
    pool_dump_obj(tag, "fb", fb);
}

/* ---- logging ---------------------------------------------------------- */
/* Bring-up default on: one line per shim call is the only way to see what a
 * silent exit did last. Turn off with KORRA_TRACE_IMPORTS=0 on a desktop run. */
int ka_trace_imports = 1;
int ka_trace_files;

void ka_trace_init(void) {
    const char *t = getenv("KORRA_TRACE_IMPORTS");
    if (t && *t) ka_trace_imports = (*t != '0');
}

void ka_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    fflush(stderr);
}

/* ---- crash breadcrumb -------------------------------------------------- */
/* A tombstone shows host frames only. The guest register file lives in host
 * globals, so a handler can still say which guest function was running and what
 * its registers were - the difference between "memset faulted" and "guest ebp
 * was zero because a shim over-popped". Logs through the raw fd, so it works
 * even if libc's stdio state is the thing that is broken. */
static uintptr_t g_crash_base;  /* libkorra.so load base, for offline resolution */

static void ka_crash_handler(int sig, siginfo_t *si, void *uc) {
    char buf[400];
    int n;
    /* The host PC is the one thing the guest register dump cannot give: it
     * names the instruction that faulted, and the module offset resolves it
     * with llvm-addr2line. ucontext is valid in a SA_SIGINFO handler. */
    uintptr_t pc = 0, lr = 0, sp = 0, fp = 0;
#if defined(__aarch64__)
    if (uc) {
        const ucontext_t *u = (const ucontext_t *)uc;
        pc = (uintptr_t)u->uc_mcontext.pc;
        sp = (uintptr_t)u->uc_mcontext.sp;
        fp = (uintptr_t)u->uc_mcontext.regs[29];
        lr = (uintptr_t)u->uc_mcontext.regs[30];
    }
#endif
    n = snprintf(buf, sizeof buf,
                 "\n[crash] signal %d code=%d addr=%p guest_func=0x%08X esp=0x%08X "
                 "ebp=0x%08X eax=0x%08X ecx=0x%08X edx=0x%08X ebx=0x%08X "
                 "esi=0x%08X edi=0x%08X\n",
                 sig, si ? si->si_code : 0, si ? si->si_addr : (void *)0,
                 g_cur_func, g_esp, g_ebp, g_eax, g_ecx, g_edx, g_ebx, g_esi, g_edi);
    if (n > 0) {
        ssize_t w = write(2, buf, (size_t)n);
        (void)w;
    }
    n = snprintf(buf, sizeof buf,
                 "[crash] pc=0x%lX off=0x%lX lr=0x%lX sp=0x%lX fp=0x%lX\n",
                 (unsigned long)pc,
                 (unsigned long)(g_crash_base && pc >= g_crash_base ? pc - g_crash_base : 0),
                 (unsigned long)lr, (unsigned long)sp, (unsigned long)fp);
    if (n > 0) {
        ssize_t w = write(2, buf, (size_t)n);
        (void)w;
    }
    /* `adb shell setprop debug.korra.watch E4D064`: dump that guest address (and
     * the two pointers in front of it) at fault time. Half of a crash is the
     * state of the object the faulting code was reading; the register dump only
     * gives its address. The address must be a mapped guest VA - a second fault
     * inside a handler kills the tombstone. */
    {
        char pv[PROP_VALUE_MAX] = {0};
        __system_property_get("debug.korra.watch", pv);
        uint32_t va = (uint32_t)strtoul(pv, NULL, 16);
        if (va) {
            uint32_t *m = (uint32_t *)((char *)g_mem_base + va);
            for (int i = 0; i < 4; i++) {
                uint32_t b = va + (uint32_t)i * 0x10u;
                uint32_t *p = (uint32_t *)((char *)g_mem_base + b);
                n = snprintf(buf, sizeof buf,
                             "[crash] watch %08X: %08X %08X %08X %08X\n", b,
                             p[0], p[1], p[2], p[3]);
                if (n > 0) { ssize_t w = write(2, buf, (size_t)n); (void)w; }
            }
            uint32_t head = m[0];
            if (head > 0x400000u && head < 0x80000000u) {
                uint32_t *p = (uint32_t *)((char *)g_mem_base + head);
                n = snprintf(buf, sizeof buf,
                             "[crash] watch [%08X]=%08X: %08X %08X %08X %08X %08X %08X %08X %08X\n",
                             va, head, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
                if (n > 0) { ssize_t w = write(2, buf, (size_t)n); (void)w; }
            }
            uint32_t sub = m[0x30 / 4];
            if (sub > 0x400000u && sub < 0x80000000u) {
                uint32_t *p = (uint32_t *)((char *)g_mem_base + sub);
                n = snprintf(buf, sizeof buf,
                             "[crash] watch [%08X+30]=%08X: %08X %08X %08X %08X %08X %08X %08X %08X\n",
                             va, sub, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
                if (n > 0) { ssize_t w = write(2, buf, (size_t)n); (void)w; }
            }
        }
    }
    /* Walk the device-change callback list (head [0xDC0F3C], entries
     * {fn, arg, ?, next, key}) so a fault can be read against the exact
     * object that was dispatched. Cheap: a few entries. */
    {
        uint32_t e = *(uint32_t *)((char *)g_mem_base + 0xDC0F3C);
        for (int i = 0; i < 300 && e > 0x400000u && e < 0x80000000u; i++) {
            uint32_t *p = (uint32_t *)((char *)g_mem_base + e);
            n = snprintf(buf, sizeof buf,
                         "[crash] list %08X: fn=%08X arg=%08X next=%08X key=%08X\n",
                         e, p[0], p[1], p[3], p[4]);
            if (n > 0) { ssize_t w = write(2, buf, (size_t)n); (void)w; }
            if (p[0] == 0x0074F8F0u && p[1] > 0x400000u && p[1] < 0x80000000u) {
                uint32_t *o = (uint32_t *)((char *)g_mem_base + p[1]);
                n = snprintf(buf, sizeof buf,
                             "[crash] list arg %08X: %08X %08X %08X %08X %08X %08X %08X %08X\n",
                             p[1], o[0], o[1], o[2], o[3], o[4], o[5], o[6], o[7]);
                if (n > 0) { ssize_t w = write(2, buf, (size_t)n); (void)w; }
            }
            e = p[3];
        }
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

void ka_crash_init(void) {
    static char altstack[SIGSTKSZ];
    stack_t ss;
    struct sigaction sa;
    Dl_info di;
    if (dladdr((void *)&ka_crash_init, &di) && di.dli_fbase)
        g_crash_base = (uintptr_t)di.dli_fbase;
    ss.ss_sp = altstack;
    ss.ss_size = sizeof altstack;
    ss.ss_flags = 0;
    sigaltstack(&ss, NULL);
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = ka_crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
}
