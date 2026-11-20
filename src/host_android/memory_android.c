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
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "host_android.h"

static uint8_t *g_win;
static size_t   g_page = 4096;

/* ---- heap ------------------------------------------------------------- */
#define HEAP_SIZE  0x40000000u   /* 1 GiB of address space, lazily backed */
#define BLK_MAGIC  0x4B41524Bu

typedef struct blk {
    uint32_t size;      /* payload bytes, header excluded */
    uint32_t magic;     /* BLK_MAGIC when free */
    struct blk *next;   /* free list only */
} blk;

static blk *g_free_list;
static uint32_t g_heap_hi, g_heap_end;

static uint32_t g_stack_hi;
static uint32_t g_stack_end;

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

uint32_t ka_guest_alloc(uint32_t size, uint32_t align) {
    if (!size) size = 16;
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
            uint32_t va = (uint32_t)((uint8_t *)b - g_win) + (uint32_t)sizeof(blk);
            return (va + align - 1u) & ~(align - 1u);
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
    uint32_t va = (uint32_t)((uint8_t *)b - g_win) + (uint32_t)sizeof(blk);
    return (va + align - 1u) & ~(align - 1u);
}

void ka_guest_free(uint32_t va) {
    if (!va) return;
    blk *b = (blk *)(g_win + va - sizeof(blk));
    if (b->magic == BLK_MAGIC) return;     /* double free */
    b->magic = BLK_MAGIC;
    b->next = g_free_list;
    g_free_list = b;
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
    if (size < 0x10000u) size = 0x10000u;
    size = (size + g_page - 1u) & ~(uint32_t)(g_page - 1u);
    if (g_stack_hi + size > g_stack_end) {
        ka_log("guest stack space exhausted");
        return 0;
    }
    uint32_t lo = g_stack_hi;
    uint32_t esp = g_stack_hi + size - 64u;
    g_stack_hi += size;
    if (limit) *limit = lo;
    memset(ka_host(lo), 0, 64);
    return esp;
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
