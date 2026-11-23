/* host_android.h - the Android host's contract (M3).
 *
 * Windows runs the lifted guest on native32 (real Win32 imports, real PE
 * mapping, x86 code entered through the VEH). None of that exists on arm64, so
 * this tree answers the guest's 194 imports itself and maps only the image's
 * *data*. The lifted C is the program; the guest never executes x86 bytes.
 *
 * Model, in one paragraph: every guest address is a uint32 VA inside a 4 GiB
 * PROT_NONE window; g_mem_base = (window host address) - 0, so ADDR(va) is just
 * the window plus the VA. The image's sections are committed at their VAs.
 * Imports are recomp_func_t shims registered under synthetic guest VAs; the PE
 * loader writes those VAs into the guest IAT, and RECOMP_CALL/RECOMP_ICALL
 * resolves them through recomp_lookup_import(). A shim reads its stdcall
 * arguments from g_esp with A32() and returns with RET(), which pops ret + argc.
 * Guest -> host is therefore an ordinary C call, and native -> guest (WndProc,
 * thread start) is hle_call_guest().
 */
#ifndef KORRA_HOST_ANDROID_H
#define KORRA_HOST_ANDROID_H

#include <stdint.h>
#include <stddef.h>

#include "recomp_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- memory */
#define KA_WINDOW_SIZE   0x100000000ull   /* the whole 32-bit guest space */
#define KA_IMAGE_BASE    0x00400000u
#define KA_HEAP_BASE     0x20000000u
#define KA_HEAP_END      0xE0000000u
#define KA_SHIM_BASE     0xF0000000u
#define KA_SHIM_STEP     16u
#define KA_SHIM_MAX      4096

int       ka_mem_init(void);
void     *ka_host(uint32_t va);              /* host pointer, no checks */
int       ka_commit(uint32_t va, uint32_t size, int prot_none); /* 0 ok */
uint32_t  ka_guest_alloc(uint32_t size, uint32_t align);        /* 0 = out */
extern uint32_t ka_alloc_calls, ka_alloc_bytes, ka_alloc_max;
void      ka_guest_free(uint32_t va);
/* Dumps the engine memory manager, its allocator and the fallback allocator
 * (free lists, used list); see memory_android.c. Diagnostics only. */
void      ka_pool_dump(const char *tag);
void      ka_ring_dump(const char *why);
void      ka_obj_check(void);
uint32_t  ka_guest_strdup(const char *s);
uint32_t  ka_guest_size(uint32_t va);
/* A fresh guest stack; returns the initial esp (top, 16-byte aligned). */
uint32_t  ka_stack_new(uint32_t size, uint32_t *limit);
/* Give a stack back when its guest thread ends; a later thread reuses it. */
void      ka_stack_release(uint32_t lo, uint32_t size);
/* LoK.exe's own SizeOfStackReserve: CreateThread(dwStackSize=0) means this. */
#define KA_STACK_DEFAULT (1u << 20)
/* Census for the 1 Hz sampler: heap bump pointer + free list, and the stack
 * region's bump pointer, end, free list and free bytes. */
void      ka_heap_stats(uint32_t *hi, uint32_t *free_blocks, uint32_t *free_bytes);
void      ka_stack_stats(uint32_t *hi, uint32_t *end, uint32_t *free_blocks,
                         uint32_t *free_bytes);

/* ------------------------------------------------------------------- PE */
int  ka_pe_map(const char *exe_path);     /* map sections at their VAs */
uint32_t ka_pe_span(void);
uint32_t ka_pe_entry(void);
int  ka_pe_bind(void);                    /* IAT -> shim VAs; returns unresolved */

/* --------------------------------------------------------------- shims */
typedef struct { const char *name; recomp_func_t fn; } ka_shim;
int      ka_shim_register(const ka_shim *shims);      /* NULL-name terminated */
uint32_t ka_shim_va(const char *name);                /* 0 if unknown */
uint32_t ka_shim_stub(const char *name);              /* register self-naming stub */
const char *ka_shim_name(uint32_t va);
uint32_t ka_shim_count(void);

/* stdcall argument access from inside a shim: g_esp -> [ret][arg0][arg1]...
 * g_esp is a GUEST VA, so the read must go through the guest window (MEM32),
 * never through the host pointer the guest number would be. */
static inline uint32_t ka_a32(int i) {
    return MEM32(g_esp + 4u + 4u * (uint32_t)i);
}
static inline void *ka_host_or_null(uint32_t va) {
    return va ? (void *)((uintptr_t)va + g_mem_base) : (void *)0;
}
#define A32(i)   ka_a32(i)
#define APTR(i)  ka_host_or_null(ka_a32(i))
#define ASTR(i)  ((char *)ka_host_or_null(ka_a32(i)))
#define AWSTR(i) ((uint16_t *)ka_host_or_null(ka_a32(i)))
/* Return from a stdcall shim: eax, then pop ret + argc args. */
#define RET(eax, argc)  do { g_eax = (uint32_t)(eax); g_esp += 4u + 4u * (uint32_t)(argc); return; } while (0)
#define RETV(argc)      RET(0, argc)
#define RETP(p, argc)   RET((uint32_t)(uintptr_t)(p), argc)

/* Native -> guest. Pushes nargs stdcall args, returns the guest's eax.
 * Takes the machine lock; nests. */
uint32_t hle_call_guest(uint32_t va, int nargs, const uint32_t *args);
void     mach_enter(void);
void     mach_leave(void);
/* Releases the machine lock briefly; installed as the runtime's
 * recomp_yield_hook so a lifted spin-wait lets the thread it waits for run. */
void     ka_machine_yield(void);
/* One-second stall reporter: names the machine holder's depth and function
 * while no import is dispatched, which tells a silent lifted loop from a
 * thread parked inside a shim. */
void     ka_spine_watchdog_start(void);
/* Records the caller as the guest main thread for the per-thread trace. */
void     ka_spine_note_main(void);
uint32_t ka_spine_main_fs(void);

/* --------------------------------------------------------------- files */
/* POSIX file layer over a fixed game root. Guest paths may be relative,
 * backslash-separated, drive-qualified, or absolute under the root. */
void ka_fs_set_root(const char *root);
const char *ka_fs_root(void);
const char *ka_fs_resolve(const char *guest_path, char *out, size_t cap);
uint32_t ka_file_open(const char *guest_path);       /* token, 0 on failure */
/* create/truncate honour dwCreationDisposition; write is GENERIC_WRITE. The
 * engine's save path needs all three: without O_CREAT every CreateFileA of a
 * new save answered FILE_NOT_FOUND and the New Game route stopped at
 * "Failed to save data." (2026-11-22). */
uint32_t ka_file_open_ex(const char *guest_path, int write, int create, int truncate);
uint32_t ka_file_seek(uint32_t tok, int64_t delta, int method, int *ok);
int      ka_file_read(uint32_t tok, void *buf, uint32_t n, uint32_t *got);
int      ka_file_write(uint32_t tok, const void *buf, uint32_t n, uint32_t *put);
uint64_t ka_file_size(uint32_t tok);
int      ka_file_truncate(uint32_t tok);   /* SetEndOfFile at the current offset */
int      ka_file_sync(uint32_t tok);       /* FlushFileBuffers */
int      ka_file_close(uint32_t tok);

/* ------------------------------------------------------------ logging */
void ka_log(const char *fmt, ...);
extern int ka_trace_imports;    /* KORRA_TRACE_IMPORTS=1: one line per shim */
extern int ka_trace_files;
void ka_trace_init(void);
void ka_crash_init(void);       /* guest registers on SIGSEGV/SIGBUS/SIGILL/SIGFPE */

/* ------------------------------------------------- save-path probe (debug)
 * debug.korra.trace_save=1: one line per call in the save device's init chain
 * with the guest path, the host path it resolved to, and the answer. Same
 * format as src/host/save_probe.c (KORRA_TRACE_SAVE=1) so the two logs diff. */
int  ka_save_on(void);
void ka_save_str(const char *call, const char *guest, const char *arg,
                 uint32_t res, int last);
void ka_save_file(const char *call, const char *guest, const char *host,
                  uint32_t access, uint32_t share, uint32_t disp,
                  uint32_t res, int last);
void ka_save_find(const char *call, const char *pat, const char *host,
                  uint32_t res, int last, const char *first);
void ka_save_io(const char *call, uint32_t h, uint32_t n, uint32_t res,
                uint32_t count, int last);
void ka_save_plain(const char *call, const char *arg, uint32_t res, int last);

/* ------------------------------------------------------------ services */
void     ka_set_guest_module(const char *exe_path);
uint32_t ka_last_error_get(void);
void     ka_last_error_set(uint32_t e);
uint32_t ka_tls_alloc(void);
void     ka_tls_free(uint32_t idx);
uint32_t ka_tls_get(uint32_t idx);
void     ka_tls_set(uint32_t idx, uint32_t v);
uint32_t ka_thread_attach(const char *why);   /* returns TIB VA for this thread */
void     ka_thread_detach(void);
uint32_t ka_thread_create(uint32_t start_va, uint32_t param, int suspended);
uint32_t ka_thread_resume(uint32_t token);   /* previous suspend count, 0xFFFFFFFF if unknown */
int      ka_thread_exists(uint32_t token);   /* 1 if the id names a live host thread */
int      ka_thread_close(uint32_t token);    /* CloseHandle on a thread id */
uint32_t ka_thread_wait(uint32_t token, uint32_t timeout_ms);
/* The CreateThread token of the calling guest thread, or 0 outside one.
 * GetCurrentThreadId must return it (see k_GetCurrentThreadId). */
uint32_t ka_guest_tid(void);
uint32_t ka_thread_exit_code(uint32_t token);
uint32_t ka_thread_self_param(void);
/* live / created / peak-live guest threads, for the 1 Hz sampler. */
void     ka_thread_stats(uint32_t *live, uint32_t *created, uint32_t *peak);
int      ka_spine_depth(void);
uint32_t ka_mono_ms(void);
uint32_t ka_hat_packet(void);
void     ka_input_touch(int action, int id, float x, float y, int w, int h);

/* Guest COM objects used by the shim tables. */
uint32_t ka_steam_user_object(void);
uint32_t ka_steam_stats_object(void);
uint32_t ka_dinput_object(void);
uint32_t ka_guest_empty_string(void);

/* Window / surface hand-off from the shell (jni_bridge.cpp). */
void ka_window_set(void *anativewindow);
void *ka_window_get(void);

/* Pad state read by XInputGetState (input_android.c owns it). */
typedef struct {
    uint32_t buttons;   /* XINPUT_GAMEPAD_* bits */
    uint8_t  lt, rt;
    int16_t  lx, ly, rx, ry;
} ka_pad_state;
void ka_pad_read(ka_pad_state *out);   /* zero when no pad */

/* D3D9 bridge (d3d9_android.cpp). */
int  ka_d3d9_init(void);               /* register interface thunks */
void ka_d3d9_window_changed(void);
int  ka_d3d9_frame_presented(void);
void ka_d3d9_shutdown(void);
/* Per-call `this` convention probe, called from recomp_lookup_import with the
 * shim name (see d3d9_android.cpp). */
void ka_vtable_note_import(const char *name);

/* The guest's entry: run the lifted CRT entry on a host thread. Returns when
 * it returns (normally only through ExitProcess). */
int ka_guest_run(const char *game_root);

/* 1 Hz stability sampler, started by ka_guest_run when debug.korra.memtrace=1
 * (memtrace_android.c). */
void ka_memtrace_start(void);

/* Per-second frame-time decomposition, started by ka_guest_run when
 * debug.korra.perf=1 (frame_probe_android.c). ka_fp_begin returns 0 when off,
 * so a call site can pass the value straight back and pay only a branch. */
void     ka_frame_probe_start(void);
int      ka_fp_on(void);
uint64_t ka_fp_begin(void);
void     ka_fp_end_present(uint64_t t0);
void     ka_fp_end_sleep(uint64_t t0, uint32_t ms);
void     ka_fp_wait_done(uint64_t t0);
void     ka_fp_lock_waited(uint64_t ns);
void     ka_fp_cs_waited(uint64_t ns);
void     ka_fp_icx(void);
int      ka_fp_lock_timing(void);
uint64_t ka_fp_now(void);

/* The port's frame cap on the engine's own limiter (fps_cap_android.c):
 * debug.korra.fps, default 30, 0 = the engine's 30/60 mode. */
void     ka_fps_cap_start(void);
void     ka_fps_tick(void);
unsigned ka_fps_cap_value(void);

/* Bridged import dispatches so far, for the probe's calls/frame column. */
uint32_t ka_import_calls(void);
/* Shell loop iterations (jni_bridge -> main_android.c korra_guest_frame). */
uint32_t ka_shell_frames(void);

/* XAudio2 / steam / misc module tables registered at boot. */
void ka_shims_register_all(void);

/* --------------------------------------------------- module shim tables */
extern const ka_shim ka_kernel32_shims[];
extern const ka_shim ka_kernel32_file_shims[];
extern const ka_shim ka_user32_shims[];
extern const ka_shim ka_gdi32_shims[];
extern const ka_shim ka_misc_shims[];      /* winmm, shell32, oleaut32, ole32 */
extern const ka_shim ka_steam_shims[];
/* XAudio2 is not a module shim table: the engine creates the device through
 * ole32!CoCreateInstance, so misc_android.c's shim hands the CLSID to
 * src/host_android/xaudio2_android.c, which builds the guest COM object and
 * registers one shim per vtable slot ("XA2dev:N", "XA2voice:N", "XA2src:N"). */
uint32_t ka_xaudio2_create_instance(uint32_t riid, uint32_t out);
void     ka_xaudio2_shutdown(void);
/* The mix entry the AAudio sink calls (one output thread, stereo float 48k). */
void     ka_xa2_render(float *dst, int frames);
extern const ka_shim ka_input_shims[];     /* xinput, dinput8 */
extern const ka_shim ka_d3dx_shims[];

#ifdef __cplusplus
}
#endif

#endif
