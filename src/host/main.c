/*
 * main.c - The Legend of Korra, recompiled: Windows host entry point.
 *
 * The lifted game is one mapping of LoK.exe at its original base (0x400000)
 * plus a 37,029-entry dispatch table. native32 supplies everything else:
 *
 *   guest -> Windows   the import bridge calls the real API and measures the
 *                      callee's stack purge (no argc table, so stdcall, cdecl,
 *                      thiscall and COM all come out right)
 *   Windows -> guest   the mapped code is not executable; a callback from
 *                      user32/D3D faults into the VEH, which enters the lifted
 *                      body on the guest stack
 *   threads            one machine lock around the register file, one guest
 *                      stack and TIB per thread
 *
 * Nothing here shims the Win32 API: it is a 32-bit process, so the guest's
 * pointers and structs are already the host's. Only steam_api (see shims.c)
 * needs answering, because there is no Steam client behind this build.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "native32.h"
#include "recomp_trace.h"

extern const recomp_dispatch_entry_t recomp_dispatch_table[];
extern const uint32_t recomp_dispatch_count;

int korra_shim_count(void);
native32_shim_t *korra_shims(void);
void korra_set_guest_module(const char *path);
void korra_install_fault_handlers(void);
void korra_quirk_probe_init(void);
void korra_file_probe_init(void);
void korra_msg_probe_init(void);
void korra_crash_probe_init(void);
void korra_perf_probe_init(void);
void korra_windowed_init(void);
/* No hand-written function overrides yet. This is where a specific guest VA
 * gets a host body (a debug hook, a stubbed subsystem), keyed by address. */
recomp_func_t recomp_lookup_manual(uint32_t va) {
    (void)va;
    return NULL;
}

static const char *readable_time(void) {
    static char buf[32];
    SYSTEMTIME t;
    GetLocalTime(&t);
    snprintf(buf, sizeof buf, "%04d-%02d-%02d %02d:%02d:%02d",
             t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return buf;
}

/* Send both stderr and a file, so a run from Explorer leaves a log behind.
 * One file per run: the game folder is also where a human plays the same build,
 * and a shared korra_host.log meant the last automated run overwrote the
 * evidence from the last manual one. The newest korra_host-*.log is the run to
 * read; scripts\run_host.ps1 picks it up automatically. */
static void open_log(const char *dir) {
    char path[MAX_PATH];
    SYSTEMTIME t;
    GetLocalTime(&t);
    snprintf(path, sizeof path, "%s\\korra_host-%04d%02d%02d-%02d%02d%02d.log", dir,
             t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    FILE *f = fopen(path, "a");
    if (!f) return;
    fprintf(f, "\n=== korra_recomp %s ===\n", readable_time());
    fflush(f);
    printf("korra: log        %s\n", path);
    fflush(stdout);
    /* Duplicate our stderr handle onto the file's: the runtime writes with
     * fprintf(stderr, ...) everywhere, including the crash report. */
    int fd = _dup(2);
    if (fd >= 0) _dup2(_fileno(f), 2);
}

static int file_exists(const char *p) {
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

int main(int argc, char **argv) {
    char exe_path[MAX_PATH], game_dir[MAX_PATH];
    char *dir_arg = NULL;

    /* The runtime's bring-up diagnostics take their own arguments
     * (--calltrace, --firsthit, --argtrace, --watch, --poison, --poke; see
     * recomp_trace.h). They only do anything in a -DRECOMP_TRACE build. */
    for (int i = 1; i < argc; i++) {
        int n = recomp_trace_arg(argc, argv, i);
        if (n) { i += n - 1; continue; }
        if (!dir_arg) dir_arg = argv[i];
    }

    /* Game directory: first bare argument, else KORRA_GAME_DIR, else
     * ../../The Legend of Korra (the layout of the development tree). */
    if (dir_arg) {
        snprintf(game_dir, sizeof game_dir, "%s", dir_arg);
    } else if (getenv("KORRA_GAME_DIR")) {
        snprintf(game_dir, sizeof game_dir, "%s", getenv("KORRA_GAME_DIR"));
    } else {
        snprintf(game_dir, sizeof game_dir, "..\\..\\The Legend of Korra");
    }
    if (!SetCurrentDirectoryA(game_dir)) {
        fprintf(stderr, "korra: cannot enter game directory '%s' (error %lu)\n",
                game_dir, GetLastError());
        fprintf(stderr, "       pass the game folder as the first argument.\n");
        return 2;
    }
    if (!GetCurrentDirectoryA(sizeof game_dir, game_dir)) return 2;

    snprintf(exe_path, sizeof exe_path, "%s\\LoK.exe", game_dir);
    if (!file_exists(exe_path)) {
        fprintf(stderr, "korra: %s not found\n", exe_path);
        return 2;
    }
    open_log(game_dir);

    printf("korra: game dir  %s\n", game_dir);
    printf("korra: module     %s\n", exe_path);
    printf("korra: dispatch   %u functions\n", recomp_dispatch_count);
    fflush(stdout);

    native32_init();
    korra_install_fault_handlers();
    korra_quirk_probe_init();
    korra_file_probe_init();
    korra_windowed_init();
    korra_msg_probe_init();
    korra_crash_probe_init();
    korra_perf_probe_init();   /* KORRA_PERF=1: per-frame where-the-time-goes */
    if (getenv("KORRA_TRACE")) native32_trace_native = 1;

    uint32_t base = 0x400000;
    uint32_t span = native32_map(exe_path, base);
    if (!span) {
        fprintf(stderr, "korra: failed to map %s at 0x%08X\n", exe_path, base);
        return 3;
    }
    printf("korra: mapped     0x%08X-0x%08X\n", base, base + span);

    korra_set_guest_module(exe_path);
    int unresolved = native32_bind(base, korra_shims(), korra_shim_count());
    if (unresolved)
        fprintf(stderr, "korra: %d imports unresolved (the game will likely fault there)\n", unresolved);

    /* PE entry point of LoK.exe (RVA 0x37C3B7): the static CRT's startup,
     * which calls the guest WinMain. It never returns. */
    const uint32_t entry = 0x77C3B7u;
    printf("korra: entering guest at 0x%08X\n", entry);
    fflush(stdout);

    native32_call_guest(entry, 0, NULL);

    /* The guest usually leaves through ExitProcess, so reaching here means the
     * CRT's startup returned: either WinMain finished or something gave up
     * early. Say where, and show the last native calls -- that is almost always
     * enough to name the subsystem that failed. */
    printf("korra: guest returned eax=0x%08X last_func=0x%08X\n", g_eax, g_cur_func);
    {
        uint32_t idx = g_native_ring_idx;
        int n = (int)(idx < NATIVE32_RING ? idx : NATIVE32_RING);
        for (int i = 1; i <= n && i <= 24; i++) {
            native32_call_rec_t *r = &g_native_ring[(idx - i + NATIVE32_RING) % NATIVE32_RING];
            const char *name = native32_name(r->target);
            printf("korra:   <- %s%s%08X from %08X args %08X %08X %08X ret %08X\n",
                   name ? name : "", name ? " " : "", r->target, r->from,
                   r->a0, r->a1, r->a2, r->ret);
        }
    }
    native32_dump_icalls(0);
    return (int)g_eax;
}
