/*
 * runtime_glue.c - the few symbols the pcrecomp runtime declares and expects
 * the host to provide.
 *
 * Kept apart from shims.c on purpose: src/host is shared between the M2 bring-up
 * session (main.c, shims.c) and the audio/Steam track (steam_stub.*,
 * xaudio2_stub.*), so each concern owns its own file and nobody has to merge.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include "native32.h"

/* recomp_types.h declares this ("a host may define it; this weak default prints
 * and aborts") but no runtime object defines it -- MSVC has no weak symbols, so
 * every host must. A body that continues past an instruction the lifter could
 * not express is the worst failure a recompile can have: the state is already
 * wrong. Name the address and the instruction, dump the last indirect calls,
 * stop. */
void recomp_unimpl(uint32_t va, const char *what) {
    fprintf(stderr, "\n[unimpl] 0x%08X: %s\n", va, what ? what : "(no description)");
    native32_dump_icalls(8);
    fflush(stderr);
    abort();
}

/* MSVC's thread-name idiom (and the CRT's own _SetThreadName) raises
 * 0x406D1388 and expects a debugger to catch it; with no debugger the raise is
 * fatal and the process dies the moment a named thread starts. Resuming after
 * the raise is exactly what the idiom assumes ("the name was reported, carry
 * on"), so continue execution. */
static LONG CALLBACK korra_veh(EXCEPTION_POINTERS *ep) {
    if (ep->ExceptionRecord->ExceptionCode == 0x406D1388UL) {
        static int logged;
        if (!logged) {
            fprintf(stderr, "[host] thread-name exception 0x406D1388 resumed\n");
            logged = 1;
        }
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

/* Anything else that reaches the top is fatal; say what and where before the
 * process dies, because a bare exit code is not a debugging trail. */
static LONG CALLBACK korra_unhandled(EXCEPTION_POINTERS *ep) {
    uint32_t code = ep->ExceptionRecord->ExceptionCode;
    uint32_t addr = (uint32_t)(uintptr_t)ep->ExceptionRecord->ExceptionAddress;
    fprintf(stderr, "\n[fatal] exception 0x%08X at 0x%08X, guest func 0x%08X\n",
            code, addr, g_cur_func);
    if (code == 0xC0000005u) {
        uint32_t info0 = ep->ExceptionRecord->ExceptionInformation[0];
        uint32_t info1 = (uint32_t)ep->ExceptionRecord->ExceptionInformation[1];
        fprintf(stderr, "[fatal] %s of 0x%08X\n", info0 ? "write" : "read", info1);
    }
    native32_dump_icalls(8);
    fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;
}

void korra_install_fault_handlers(void) {
    AddVectoredExceptionHandler(1, korra_veh);
    SetUnhandledExceptionFilter(korra_unhandled);
}
