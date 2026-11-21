/*
 * wprot_probe.c - write-watch probe (debug only, inert unless KORRA_WPROT is set).
 *
 * Task 2 needs the *store instruction* that writes [0xE4D064 + 0x30]
 * (0xE4D094), not the function that happens to be running when the next
 * indirect transfer samples it: the Windows watch log names 0x748CB0 because
 * the store happens inside a callee of that window. Nothing in the generated
 * tree stores to the literal address (the field is only reachable through the
 * context pointer), so a value/from scan cannot name it.
 *
 *   KORRA_WPROT=<hex guest VA>
 *
 * marks the 4 KB page holding that dword PAGE_READONLY (reads still work, only
 * writes fault), and a vectored handler turns each write fault into a one-shot
 * observation:
 *
 *   1. fault with write intent inside the page: remember g_cur_func, let the
 *      page be writable for exactly one instruction (EFlags.TF), continue;
 *   2. single-step: clear TF, and if the watched dword actually changed, log
 *      the new value with the function from step 1 - that is the writer;
 *   3. re-arm the page read-only.
 *
 * This observes; it never changes guest behaviour: the store is allowed to
 * execute every time. One watched address per run (same rule as KORRA_WATCHLOG).
 *
 * The guest image is mapped after CRT init, so init only installs the handler;
 * korra_wprot_tick() (called from the lookup path, see watch_probe.c) arms the
 * page on the first transfer once the range is committed.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include "recomp_types.h"

static uintptr_t s_page;
static uint32_t  s_addr;
static volatile LONG s_init, s_hits, s_other, s_armed;

/* Per-thread: the fault that is waiting for its single-step. */
static __declspec(thread) LONG     t_stepping;
static __declspec(thread) uint32_t t_func, t_old;

static void wprot_arm(void) {
    DWORD old;
    VirtualProtect((void *)s_page, 0x1000, PAGE_READONLY, &old);
}

void korra_wprot_tick(void) {
    MEMORY_BASIC_INFORMATION mbi;
    DWORD old;
    if (!s_addr || s_armed) return;
    if (VirtualQuery((void *)s_page, &mbi, sizeof mbi) != sizeof mbi) return;
    if (mbi.State != MEM_COMMIT) return;
    if (!VirtualProtect((void *)s_page, 0x1000, PAGE_READONLY, &old)) return;
    s_armed = 1;
    fprintf(stderr, "[wprot] armed page %08X for %08X\n",
            (unsigned)s_page, (unsigned)s_addr);
    fflush(stderr);
}

static LONG CALLBACK wprot_veh(EXCEPTION_POINTERS *ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;

    if (code == EXCEPTION_SINGLE_STEP && t_stepping) {
        uint32_t now = *(volatile uint32_t *)s_addr;
        t_stepping = 0;
        ep->ContextRecord->EFlags &= ~0x100u;
        if (now != t_old && InterlockedIncrement(&s_hits) <= 200) {
            fprintf(stderr, "[wprot] %08X = %08X (was %08X) writer=%08X\n",
                    s_addr, now, t_old, t_func);
            fflush(stderr);
        }
        wprot_arm();
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    if (code != 0xC0000005u) return EXCEPTION_CONTINUE_SEARCH;
    if (!ep->ExceptionRecord->ExceptionInformation[0]) return EXCEPTION_CONTINUE_SEARCH;

    {
        uintptr_t a = (uintptr_t)ep->ExceptionRecord->ExceptionInformation[1];
        if (a < s_page || a >= s_page + 0x1000) return EXCEPTION_CONTINUE_SEARCH;
        if (a == s_addr) {
            t_func = g_cur_func;
            t_old = *(volatile uint32_t *)s_addr;
        } else if (InterlockedIncrement(&s_other) == 1) {
            fprintf(stderr, "[wprot] page %08X also written at +%03X (first seen from=%08X)\n",
                    (unsigned)s_page, (unsigned)(a - s_page), g_cur_func);
            fflush(stderr);
        }
        /* Let the store execute, then re-arm from the single-step. */
        {
            DWORD old;
            VirtualProtect((void *)s_page, 0x1000, PAGE_READWRITE, &old);
        }
        t_stepping = a == s_addr ? 1 : 2;
        ep->ContextRecord->EFlags |= 0x100u;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
}

static void wprot_init(void) {
    const char *v = getenv("KORRA_WPROT");
    uint32_t a;
    if (!v || !*v) return;
    a = (uint32_t)strtoul(v, NULL, 16);
    if (!a) return;
    s_addr = a;
    s_page = (uintptr_t)a & ~(uintptr_t)0xFFF;
    if (!InterlockedExchange(&s_init, 1)) {
        AddVectoredExceptionHandler(1, wprot_veh);
        fprintf(stderr, "[wprot] watching %08X (page %08X), arming on first transfer\n",
                s_addr, (unsigned)s_page);
        fflush(stderr);
    }
}

/* CRT initializer: installs the handler only; no threads. */
#pragma section(".CRT$XIU", read)
__declspec(allocate(".CRT$XIU")) static void (__cdecl *s_wprot_init)(void) = wprot_init;
