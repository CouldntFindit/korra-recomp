/*
 * save_probe.c - the save-device init chain, call by call, on the Windows host.
 *
 * The device's "Failed to save data." is pre-filesystem: no open, mkdir or
 * enumeration fails, yet the writer never reaches CreateFileA. The save
 * device's path is built from SHGetSpecialFolderPathA(CSIDL_PERSONAL) plus
 * "Korra" (derived statically from the original exe: 0x76CB50 ->
 * "%s/%s/" with "Korra" -> "%sSystemData.dat" / "%sGameData.dat" /
 * "%sSlotData_%d.dat"; the directory check 0x76CC40 appends "*.*"), so the
 * whole init chain is a small set of file calls whose answers must match
 * between the two hosts.
 *
 * This probe logs every call in that chain with its arguments AND its result in
 * ONE fprintf - the two-part-line interleave trap from the ledger - so the
 * Windows lines and the device lines (src/host_android/save_probe_android.c,
 * same format) can be diffed line for line.
 *
 *   KORRA_TRACE_SAVE=1
 *
 * It chains native32_post_hook, so KORRA_TRACE_FILE / KORRA_TRACE_D3D keep
 * working alongside it.
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "native32.h"

static int s_on = -1;
static native32_post_hook_t s_prev;

/* Guest pointers are 32-bit values in the caller's stack slots; only read one
 * whose page is committed and readable, or a bad argument faults the probe
 * instead of reporting it. */
static const char *safe_str(uint32_t va) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!va) return NULL;
    if (!VirtualQuery((LPCVOID)(uintptr_t)va, &mbi, sizeof mbi)) return NULL;
    if (mbi.State != MEM_COMMIT) return NULL;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return NULL;
    return (const char *)(uintptr_t)va;
}

static uint32_t safe_u32(uint32_t va) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!va) return 0;
    if (!VirtualQuery((LPCVOID)(uintptr_t)va, &mbi, sizeof mbi)) return 0;
    if (mbi.State != MEM_COMMIT) return 0;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return 0;
    return *(const uint32_t *)(uintptr_t)va;
}

/* One line, one write. Fields are ordered so a Windows line and a device line
 * read the same way; paths are quoted so a diff is exact. */
static void sv(const char *fmt, ...) {
    char line[768];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    fprintf(stderr, "[save] %s\n", line);
    fflush(stderr);
}

static void save_post_hook(uint32_t target, const uint32_t *args, uint32_t ret) {
    const char *name = native32_name(target);
    if (!name) return;

    /* Callers' GetLastError is what tells "missing directory" apart from
     * "empty directory" on Windows; take it before anything else runs. */
    DWORD err = GetLastError();

    if (strstr(name, "SHGetSpecialFolderPathA")) {
        const char *p = ret ? safe_str(args[1]) : NULL;
        sv("SHGetSpecialFolderPathA hwnd=%u csidl=%u create=%u -> ret=%u path=\"%s\"",
           args[0], args[2], args[3], ret, p ? p : "");
    } else if (strstr(name, "CreateDirectoryA")) {
        const char *p = safe_str(args[0]);
        sv("CreateDirectoryA path=\"%s\" -> ret=%u last=%lu", p ? p : "?", ret, err);
    } else if (strstr(name, "GetFileAttributesA")) {
        const char *p = safe_str(args[0]);
        sv("GetFileAttributesA path=\"%s\" -> 0x%08X last=%lu", p ? p : "?", ret, err);
    } else if (strstr(name, "FindFirstFile")) {   /* A and ExA: the engine uses both */
        /* FindFirstFileExA puts lpFindFileData in args[2], FindFirstFileA in
         * args[1]; reading args[1] for ExA printed first="" on every Windows
         * run and hid the name that the device log showed. */
        int exa = strstr(name, "ExA") != NULL;
        const char *p = safe_str(args[0]);
        const char *hit = (ret != 0xFFFFFFFFu) ? safe_str(args[exa ? 2 : 1] + 44) : "";
        sv("%s pat=\"%s\" -> 0x%08X last=%lu first=\"%s\"",
           exa ? "FindFirstFileExA" : "FindFirstFileA",
           p ? p : "?", ret, err, hit ? hit : "");
    } else if (strstr(name, "FindNextFileA")) {
        const char *hit = ret ? safe_str(args[1] + 44) : "";
        sv("FindNextFileA h=0x%08X -> ret=%u last=%lu name=\"%s\"",
           args[0], ret, err, hit ? hit : "");
    } else if (strstr(name, "FindClose")) {
        sv("FindClose h=0x%08X -> ret=%u", args[0], ret);
    } else if (strstr(name, "CreateFileA")) {
        const char *p = safe_str(args[0]);
        sv("CreateFileA path=\"%s\" access=0x%08X share=0x%08X disp=%u flags=0x%08X -> 0x%08X last=%lu",
           p ? p : "?", args[1], args[2], args[4], args[5], ret, err);
    } else if (strstr(name, "CreateFileW")) {
        sv("CreateFileW (wide) access=0x%08X share=0x%08X disp=%u flags=0x%08X -> 0x%08X last=%lu",
           args[1], args[2], args[4], args[5], ret, err);
    } else if (strstr(name, "ReadFile")) {
        sv("ReadFile h=0x%08X n=%u -> ret=%u got=%u last=%lu",
           args[0], args[2], ret, safe_u32(args[3]), err);
    } else if (strstr(name, "WriteFile")) {
        sv("WriteFile h=0x%08X n=%u -> ret=%u put=%u last=%lu",
           args[0], args[2], ret, safe_u32(args[3]), err);
    } else if (strstr(name, "SetEndOfFile")) {
        sv("SetEndOfFile h=0x%08X -> ret=%u last=%lu", args[0], ret, err);
    } else if (strstr(name, "FlushFileBuffers")) {
        sv("FlushFileBuffers h=0x%08X -> ret=%u last=%lu", args[0], ret, err);
    } else if (strstr(name, "DeleteFileA")) {
        const char *p = safe_str(args[0]);
        sv("DeleteFileA path=\"%s\" -> ret=%u last=%lu", p ? p : "?", ret, err);
    } else if (strstr(name, "MoveFileA")) {
        const char *a = safe_str(args[0]), *b = safe_str(args[1]);
        sv("MoveFileA from=\"%s\" to=\"%s\" -> ret=%u last=%lu",
           a ? a : "?", b ? b : "?", ret, err);
    } else if (strstr(name, "SetCurrentDirectoryA")) {
        const char *p = safe_str(args[0]);
        sv("SetCurrentDirectoryA path=\"%s\" -> ret=%u last=%lu", p ? p : "?", ret, err);
    }
}

static void save_hook(uint32_t target, const uint32_t *args, uint32_t ret) {
    if (s_prev) s_prev(target, args, ret);   /* keep KORRA_TRACE_FILE/D3D */
    save_post_hook(target, args, ret);
}

void korra_save_probe_init(void) {
    if (s_on < 0) s_on = getenv("KORRA_TRACE_SAVE") != NULL;
    if (!s_on) return;
    s_prev = native32_post_hook;
    native32_post_hook = save_hook;
    fprintf(stderr, "[save] probe on (KORRA_TRACE_SAVE=1)\n");
}
