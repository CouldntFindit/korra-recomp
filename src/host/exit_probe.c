/*
 * exit_probe.c - name whoever ends the process.
 *
 * A guest that calls ExitProcess looks exactly like a silent crash: the window
 * disappears, the log stops mid-line, and there is no [fatal] report because no
 * exception was raised. The M2 bring-up hit that after the intro movies, at
 * different points on different runs, so this shim answers the only question
 * that matters first - which lifted function asked to quit, and with what code -
 * before anything else is blamed.
 *
 * Always installed. It prints one line and forwards to the real ExitProcess, so
 * behaviour is unchanged apart from the diagnostic.
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdio.h>
#include "native32.h"
#include "recomp_trace.h"

static void shim_ExitProcess(void) {
    uint32_t *a = (uint32_t *)(uintptr_t)(g_esp + 4);
    uint32_t code = a[0];
    const char *import = g_cur_import;
    fprintf(stderr, "\n[exit] ExitProcess(%u) called from guest function 0x%08X (import %s)\n",
            code, g_cur_func, import ? import : "(none)");
    native32_dump_icalls(16);
    fflush(stderr);
    ExitProcess(code);
}

static void shim_TerminateProcess(void) {
    uint32_t *a = (uint32_t *)(uintptr_t)(g_esp + 4);
    fprintf(stderr, "\n[exit] TerminateProcess(handle 0x%08X, code %u) from guest 0x%08X\n",
            a[0], a[1], g_cur_func);
    native32_dump_icalls(16);
    fflush(stderr);
    TerminateProcess((HANDLE)(uintptr_t)a[0], a[1]);
}

/* The guest's static CRT installs its own unhandled-exception filter during
 * startup, which *replaces* the host's: every later crash then dies inside the
 * CRT's filter without the [fatal] report that names the faulting guest
 * function (the M2 cutscene crash was found only through the Windows event
 * log). Ignore the request and keep ours. */
static void shim_SetUnhandledExceptionFilter(void) {
    uint32_t *a = (uint32_t *)(uintptr_t)(g_esp + 4);
    (void)a;
    fprintf(stderr, "[host] guest SetUnhandledExceptionFilter ignored (host keeps the fault report)\n");
    fflush(stderr);
    g_eax = 0;
    g_esp += 4 + 4;
}

static const native32_shim_t g_exit_shims[] = {
    { "ExitProcess", shim_ExitProcess },
    { "TerminateProcess", shim_TerminateProcess },
    { "SetUnhandledExceptionFilter", shim_SetUnhandledExceptionFilter },
};

const native32_shim_t *exit_probe_shims(int *count) {
    if (count) *count = (int)(sizeof g_exit_shims / sizeof g_exit_shims[0]);
    return g_exit_shims;
}
