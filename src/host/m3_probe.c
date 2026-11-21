/*
 * m3_probe.c - Windows ground truth for the Android graphics-init abort.
 *
 * On the phone, the engine's whole graphics init runs and then sub_00847BB0
 * ("framework start") returns 0x34 at its step 1: sub_008478C0 ->
 * sub_00829140(0, 0x01000000, 0x40, 1, 0) -> sub_00767630(0x01000000) ->
 * sub_0073E700 -> the memory manager's allocator (the object at [0xDB8284]+0,
 * vtable slot 3 = 0x73ED40) which refuses the 16 MB block. On Windows the same
 * boot reaches the menu, so the difference between the two runs is the
 * evidence: either a budget field (CA1FD8 and the allocator's own limits) or a
 * host shim feeding the engine a number it would not see on a real OS.
 *
 * This probe prints the manager and allocator at the two import boundaries
 * that bracket the framework start, the same points the Android host dumps
 * in u_GetForegroundWindow ("pre:") and u_ClipCursor ("post:"), so the two
 * logs can be read side by side.
 *
 * Layout found by reading the lifted chain (0x829140, 0x767630, 0x73E700,
 * 0x73ED40, 0x73E9C0):
 *   M   = [0x00DB8284]          memory manager
 *   A0  = [M+0]                 allocator object; its vtable slot 3 allocates
 *   [A0+0x48]                   alignment passed to the allocate method
 *   [A0+0x38]/[A0+0x3c]         first/second-level free-list bitmaps
 *   [A0+0x40]                   free-list heads indexed by (fl, sl)
 *   [A0+0x44]                   used-chunk list head, singly linked by +0x10
 *   [A0+0x54]                   number of used chunks
 *   [A0+0x58]                   first-level class count; a request whose class
 *                               index is >= this is refused without a search
 *                               (0x73EA9D)
 *   chunk: +0x0c prev, +0x10 next, +0x18 block size, +0x1c owner, +0x24 data
 *
 * Knob: KORRA_TRACE_M3=1 (off by default: no hook is installed).
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

#define VA_BUDGET      0x00CA1FD8u  /* nonzero = the requested pool size       */
#define VA_CFG         0x00CA1FCCu  /* 0x38-byte framework config block        */
#define VA_POST_SLOT   0x009B4390u  /* -1 until the pool create publishes one  */
#define VA_9B44E8      0x009B44E8u
#define VA_CA2020      0x00CA2020u
#define VA_CA2990      0x00CA2990u
#define VA_POOL_N      0x00CA1EE8u  /* pool record count                       */
#define VA_POOL_MAX    0x00CA1EECu  /* pool record limit                       */
#define VA_POOL_ARR    0x00CA1EF0u  /* pool records, stride 0x48               */
#define VA_POOL_READY  0x00CA1EF4u
#define VA_9B4388      0x009B4388u
#define VA_9B438C      0x009B438Cu
#define VA_MANAGER     0x00DB8284u  /* memory manager pointer                  */

static native32_pre_hook_t  g_pre_other;
static native32_post_hook_t g_post_other;
static int g_pre_n, g_post_n, g_giant_n;

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

/* One allocator instance: header fields, free lists per class, used list.
 * Same field names as ka_pool_dump() in src/host_android/memory_android.c. */
static void pool_obj(const char *tag, const char *name, uint32_t o) {
    uint32_t heads, nclasses, c;
    unsigned k;

    if (!o || !readable(o, 0x60)) {
        fprintf(stderr, "[m3] %s %s=0x%08X (not a pool)\n", tag, name, o);
        return;
    }
    heads = rd32(o + 0x40);
    nclasses = rd32(o + 0x58);
    fprintf(stderr,
            "[m3] %s %s=0x%08X vt=0x%08X +8=0x%08X +C=0x%08X +10=0x%08X "
            "+14=0x%08X +18=0x%08X +1C=0x%08X +20=0x%08X +38=0x%08X "
            "+3C=0x%08X +40=0x%08X +44=0x%08X +48=0x%08X +54=%u +58=%u "
            "+5C=0x%08X\n",
            tag, name, o, rd32(o), rd32(o + 8), rd32(o + 0xC), rd32(o + 0x10),
            rd32(o + 0x14), rd32(o + 0x18), rd32(o + 0x1C), rd32(o + 0x20),
            rd32(o + 0x38), rd32(o + 0x3C), heads, rd32(o + 0x44),
            rd32(o + 0x48), rd32(o + 0x54), nclasses, rd32(o + 0x5C));

    {
        unsigned nodes = 0;
        unsigned long sum = 0;
        uint32_t big = 0, bigc = 0;
        if (heads && nclasses <= 64 && readable(heads, (size_t)(nclasses ? nclasses : 32) * 16)) {
            for (uint32_t i = 0; i < nclasses * 4; i++) {
                uint32_t n = rd32(heads + i * 4);
                unsigned k2 = 0;
                uint32_t csum = 0, cbig = 0;
                while (n && k2 < 512 && readable(n, 0x28)) {
                    uint32_t sz = rd32(n + 0x18);
                    csum += sz;
                    if (sz > cbig) cbig = sz;
                    k2++;
                    n = rd32(n + 0x10);
                }
                if (k2) {
                    nodes += k2;
                    sum += csum;
                    if (cbig > big) { big = cbig; bigc = i; }
                    fprintf(stderr, "[m3] %s %s free[%u] (fl=%u sl=%u) n=%u largest=%u\n",
                            tag, name, i, i >> 2, i & 3u, k2, cbig);
                }
            }
        }
        fprintf(stderr, "[m3] %s %s free total: nodes=%u sum=%lu largest=%u@%u\n",
                tag, name, nodes, sum, big, bigc);
    }

    c = rd32(o + 0x44);
    for (k = 0; c && k < 64 && readable(c, 0x28); k++) {
        fprintf(stderr, "[m3] %s %s used[%u] %08X sz=%u own=%08X\n",
                tag, name, k, c, rd32(c + 0x18), rd32(c + 0x1C));
        c = rd32(c + 0x10);
    }
    fprintf(stderr, "[m3] %s %s used end k=%u\n", tag, name, k);
}

/* The image globals the framework start reads and writes. */
static void dump_globals(const char *tag) {
    uint32_t m = rd32(VA_MANAGER);
    uint32_t a0 = m ? rd32(m) : 0;
    uint32_t avt = a0 ? rd32(a0) : 0;

    fprintf(stderr,
            "[m3] %s budget(CA1FD8)=0x%08X slot(9B4390)=0x%08X 9B44E8=0x%08X "
            "CA2020=0x%08X CA2990=0x%08X pools=%d/%d arr=0x%08X ready=%d "
            "9B4388=%d 9B438C=%d\n",
            tag, rd32(VA_BUDGET), rd32(VA_POST_SLOT), rd32(VA_9B44E8),
            rd32(VA_CA2020), rd32(VA_CA2990), (int)rd32(VA_POOL_N),
            (int)rd32(VA_POOL_MAX), rd32(VA_POOL_ARR), (int)rd32(VA_POOL_READY),
            (int)rd32(VA_9B4388), (int)rd32(VA_9B438C));

    /* The config block 0xCA1FCC is 0x38 bytes: framework start (0x847C24)
     * copies it from its argument 0xDB8AE8, whose defaults come from 0x8479C0
     * and whose +0xC 0x7657D0 overwrites with 0x300000. */
    fprintf(stderr,
            "[m3] %s cfg=", tag);
    for (int i = 0; i < 0x38; i += 4)
        fprintf(stderr, "%s%x:%08X", i ? " " : "", i,
                rd32(VA_CFG + i));
    fprintf(stderr, "\n");

    fprintf(stderr,
            "[m3] %s mgr=0x%08X vt=0x%08X +8=0x%08X +C=0x%08X +10=0x%08X "
            "+14=0x%08X +18=0x%08X +1C=0x%08X +20=0x%08X +44=0x%08X "
            "+48=0x%08X +4C=0x%08X\n",
            tag, m, m ? rd32(m) : 0,
            m ? rd32(m + 0x08) : 0, m ? rd32(m + 0x0C) : 0,
            m ? rd32(m + 0x10) : 0, m ? rd32(m + 0x14) : 0,
            m ? rd32(m + 0x18) : 0, m ? rd32(m + 0x1C) : 0,
            m ? rd32(m + 0x20) : 0,
            m ? rd32(m + 0x44) : 0, m ? rd32(m + 0x48) : 0,
            m ? rd32(m + 0x4C) : 0);

    /* 0x00767630 builds its fallback list from *([M+0x14]): the cell holds the
     * allocator that serves what A0's classes cannot. A0 refuses a 16 MB
     * request on Windows too (its class index is past +0x58), so the success
     * here comes from this object; on the device it is the prime suspect. */
    if (m) {
        uint32_t cell = rd32(m + 0x14);
        uint32_t fb = cell ? rd32(cell) : 0;
        fprintf(stderr, "[m3] %s fallback cell=0x%08X -> 0x%08X\n", tag, cell, fb);
        pool_obj(tag, "a0", a0);
        pool_obj(tag, "fb", fb);
    }

    fflush(stderr);
}

static int named(uint32_t target, const char *fn) {
    const char *n = native32_name(target);
    if (!n) return 0;
    for (; *n; n++) if (!_strnicmp(n, fn, strlen(fn))) return 1;
    return 0;
}

/* The budget global 0xCA1FD8 is 0 at the pre anchor and 0x300000 at the post
 * anchor on Windows, and only two absolute reads of it exist in .text, so the
 * writer uses a base register.  Poll it at every import dispatch instead:
 * g_cur_func is the function whose call site we are at, and the previous
 * (function, import) pair says how far the write is from the last boundary. */
extern uint32_t g_cur_func;
static uint32_t g_budget_last;
static uint32_t g_prev_fn;
static char g_prev_imp[48];
static int g_budget_n;

static void budget_poll(const char *at) {
    uint32_t v = *(volatile uint32_t *)(uintptr_t)VA_BUDGET;
    if (v == g_budget_last) return;
    g_budget_last = v;
    if (g_budget_n++ >= 24) return;
    fprintf(stderr,
            "[m3] budget 0x%08X -> 0x%08X at %s 0x%08X (prev %s from 0x%08X)\n",
            g_budget_last, v, at, g_cur_func,
            g_prev_imp[0] ? g_prev_imp : "?", g_prev_fn);
    fflush(stderr);
}

static void probe_pre(uint32_t target, uint32_t *args) {
    const char *nm = native32_name(target);
    budget_poll("import");
    snprintf(g_prev_imp, sizeof g_prev_imp, "%s", nm ? nm : "?");
    g_prev_fn = g_cur_func;
    if (named(target, "GetForegroundWindow") && g_pre_n < 8) {
        g_pre_n++;
        dump_globals("pre:");
    }
    if (g_pre_other) g_pre_other(target, args);
}

static void probe_post(uint32_t target, const uint32_t *args, uint32_t ret) {
    const char *n = native32_name(target);
    budget_poll("post-import");
    snprintf(g_prev_imp, sizeof g_prev_imp, "%s", n ? n : "?");
    g_prev_fn = g_cur_func;

    if (named(target, "ClipCursor") && g_post_n < 8) {
        g_post_n++;
        dump_globals("post:");
    }

    /* Real-OS giant allocations, the same question the Android host answers
     * with its alloc: lines: if the engine asks Windows for ~326 MB of giants
     * too, that is normal engine behaviour and not a shim artifact. */
    if (n && g_giant_n < 60) {
        if (strstr(n, "VirtualAlloc") && args && args[1] >= 0x400000u) {
            g_giant_n++;
            fprintf(stderr,
                    "[m3] giant: %s(0x%08X, %u, 0x%X, 0x%X) = 0x%08X\n",
                    n, args[0], args[1], args[2], args[3], ret);
            fflush(stderr);
        } else if (strstr(n, "HeapAlloc") && args && args[2] >= 0x400000u) {
            g_giant_n++;
            fprintf(stderr, "[m3] giant: %s(0x%08X) = 0x%08X\n", n, args[2], ret);
            fflush(stderr);
        }
    }

    if (g_post_other) g_post_other(target, args, ret);
}

static void __cdecl m3_probe_ctor(void) {
    if (!getenv("KORRA_TRACE_M3")) return;
    if (native32_pre_hook && native32_pre_hook != probe_pre)
        g_pre_other = native32_pre_hook;
    if (native32_post_hook && native32_post_hook != probe_post)
        g_post_other = native32_post_hook;
    native32_pre_hook = probe_pre;
    native32_post_hook = probe_post;
    fprintf(stderr, "[m3] Windows parity probe on\n");
    fflush(stderr);
}

#pragma section(".CRT$XIU", read)
__declspec(allocate(".CRT$XIU")) static void (__cdecl *g_m3_ctor)(void) = m3_probe_ctor;
