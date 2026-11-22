/* fs_android.c - the POSIX file layer behind CreateFileA/ReadFile/FindFirstFileA.
 *
 * The guest speaks Windows paths relative to the directory it "cd"s into. The
 * install lives at <app files>/game, with LoK.exe next to GameData/. Resolution:
 * normalise separators, drop a drive/UNC prefix, and treat the path as relative
 * to the current guest directory (SetCurrentDirectoryA) which itself is relative
 * to the game root. An absolute path that contains "GameData/" starts there, so
 * a path the engine built from its own module name still resolves.
 *
 * Handles are small integers, never host pointers: guest code stores them in
 * 32-bit fields.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "host_android.h"

static char g_root[512];
static char g_cwd[512];     /* relative to root; "" = root */

void ka_fs_set_root(const char *root) {
    snprintf(g_root, sizeof g_root, "%s", root);
    g_cwd[0] = 0;
}
const char *ka_fs_root(void) { return g_root; }

/* What GetModuleFileNameA/W report to the guest: the virtual drive, never the
 * host root (see the mapping in ka_fs_resolve). */
const char *ka_fs_guest_exe(void) { return "C:\\LoK.exe"; }

const char *ka_fs_resolve(const char *guest_path, char *out, size_t cap) {
    char norm[768];
    size_t i, j = 0;
    const char *p = guest_path;

    if (!p || !*p) { snprintf(out, cap, "%s", g_root); return out; }

    for (i = 0; p[i] && j + 1 < sizeof norm; i++)
        norm[j++] = (p[i] == '\\') ? '/' : p[i];
    norm[j] = 0;

    /* The guest sees a virtual C: drive: "C:\LoK.exe", "C:\GameData\...". The
     * drive letter is not cosmetic. The engine decides whether a path is an
     * absolute Windows path by looking for "X:", so a POSIX root
     * (/data/user/0/...) looked relative and the engine prepended its own base
     * directory to it - "<root>/<root>/GameData\" - and the rofs mount failed.
     * Everything below maps a guest path onto the host root. */
    {
        size_t w = 0;
        for (i = 0; norm[i]; i++) {
            if (norm[i] == '/' && w && norm[w - 1] == '/') continue;
            norm[w++] = norm[i];
        }
        norm[w] = 0;
    }

    /* Defensive: if a host path leaked through (GetModuleFileNameA used to hand
     * the engine one), drop leading copies of the host root. */
    {
        size_t rl = strlen(g_root);
        while (rl && strncmp(norm, g_root, rl) == 0 && (norm[rl] == 0 || norm[rl] == '/')) {
            const char *rest = norm + rl;
            while (*rest == '/') rest++;
            memmove(norm, rest, strlen(rest) + 1);
        }
    }

    /* "C:" (or any single-letter drive): absolute inside the guest root. */
    p = norm;
    if (((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':') {
        p += 2;
        while (*p == '/') p++;
        snprintf(out, cap, "%s/%s", g_root, p);
        return out;
    }

    /* A leading slash is an absolute guest path as well. */
    if (norm[0] == '/') {
        p = norm;
        while (*p == '/') p++;
        snprintf(out, cap, "%s/%s", g_root, p);
        return out;
    }

    if (!g_cwd[0]) snprintf(out, cap, "%s/%s", g_root, norm);
    else snprintf(out, cap, "%s/%s/%s", g_root, g_cwd, norm);
    return out;
}

/* ---- handles ---------------------------------------------------------- */
#define MAX_FILES 512
typedef struct { int fd; int used; } kfile;
static kfile g_files[MAX_FILES];

/* File tokens live in their own high range. They used to be small ints
 * 1..MAX_FILES, the same space as kernel-object handles: the engine's two open
 * CPKs took tokens 1 and 2, so its CloseHandle(1)/CloseHandle(2) destroyed
 * mutex 1 and the file pool's wake event instead of the files. That pool could
 * then never shut down (its final SetEvent had nothing to wake) and the boot
 * ended in a SetEvent retry loop with no frame. Objects stay 1..256, find
 * handles sit at 0x02000000, thread tokens at 0x1000 and up. */
#define KA_FILE_TOKEN_BASE 0x01000000u

static kfile *kf(uint32_t tok) {
    uint32_t i;
    if (tok < KA_FILE_TOKEN_BASE) return NULL;
    i = tok - KA_FILE_TOKEN_BASE;
    if (i >= MAX_FILES || !g_files[i].used) return NULL;
    return &g_files[i];
}

uint32_t ka_file_open_ex(const char *guest_path, int write, int create, int truncate) {
    char path[1024];
    int flags = (write || create || truncate) ? O_RDWR : O_RDONLY;
    int fd;
    ka_fs_resolve(guest_path, path, sizeof path);
    if (create) flags |= O_CREAT;
    if (truncate) flags |= O_TRUNC;
    fd = open(path, flags, 0644);
    /* A read-only data file opened with a write disposition (some code paths
     * pass GENERIC_WRITE for a file it only reads) still has to open. */
    if (fd < 0 && !create && !truncate) fd = open(path, O_RDONLY);
    if (fd < 0) {
        /* A failed open is always worth a line: the save path fails silently
         * otherwise ("Failed to save data." with no clue which file). */
        ka_log("[fs] open FAILED %s (w=%d c=%d t=%d): %s", guest_path,
               write, create, truncate, strerror(errno));
        return 0;
    }
    if (ka_trace_files) ka_log("[fs] open %s -> %s", guest_path, path);
    for (uint32_t i = 0; i < MAX_FILES; i++) {
        if (!g_files[i].used) {
            g_files[i].used = 1;
            g_files[i].fd = fd;
            return KA_FILE_TOKEN_BASE + i;
        }
    }
    close(fd);
    return 0;
}

uint32_t ka_file_open(const char *guest_path) {
    return ka_file_open_ex(guest_path, 0, 0, 0);
}

uint32_t ka_file_seek(uint32_t tok, int64_t delta, int method, int *ok) {
    kfile *f = kf(tok);
    off_t r;
    if (!f) { if (ok) *ok = 0; return 0xFFFFFFFFu; }
    r = lseek(f->fd, (off_t)delta, method == 0 ? SEEK_SET : method == 1 ? SEEK_CUR : SEEK_END);
    if (ok) *ok = r >= 0;
    return (uint32_t)r;
}

int ka_file_read(uint32_t tok, void *buf, uint32_t n, uint32_t *got) {
    kfile *f = kf(tok);
    ssize_t r;
    if (!f) { if (got) *got = 0; return 0; }
    r = read(f->fd, buf, n);
    if (r < 0) r = 0;
    if (got) *got = (uint32_t)r;
    return 1;
}

int ka_file_write(uint32_t tok, const void *buf, uint32_t n, uint32_t *put) {
    kfile *f = kf(tok);
    ssize_t r;
    if (!f) { if (put) *put = 0; return 0; }
    r = write(f->fd, buf, n);
    if (r < 0) r = 0;
    if (put) *put = (uint32_t)r;
    return 1;
}

uint64_t ka_file_size(uint32_t tok) {
    kfile *f = kf(tok);
    struct stat st;
    if (!f || fstat(f->fd, &st) != 0) return 0;
    return (uint64_t)st.st_size;
}

/* SetEndOfFile: truncate at the current position, which is what Windows means.
 * The save writer uses it after its last block. */
int ka_file_truncate(uint32_t tok) {
    kfile *f = kf(tok);
    off_t at;
    if (!f) return 0;
    at = lseek(f->fd, 0, SEEK_CUR);
    return at >= 0 && ftruncate(f->fd, at) == 0;
}

int ka_file_sync(uint32_t tok) {
    kfile *f = kf(tok);
    return f ? fsync(f->fd) == 0 : 0;
}

int ka_file_close(uint32_t tok) {
    kfile *f = kf(tok);
    if (!f) return 0;
    close(f->fd);
    f->used = 0;
    return 1;
}

/* ---- guest cwd -------------------------------------------------------- */
/* The cwd is guest-relative: "C:\GameData\" and "\GameData\" both become
 * "GameData", "" means the root. Keeping it relative is what makes a relative
 * guest path join correctly no matter how the engine spelled its base. */
void ka_fs_set_cwd(const char *p) {
    char norm[768];
    size_t i, j = 0;
    const char *q;

    if (!p || !*p) { g_cwd[0] = 0; return; }
    for (i = 0; p[i] && j + 1 < sizeof norm; i++)
        norm[j++] = (p[i] == '\\') ? '/' : p[i];
    norm[j] = 0;

    q = norm;
    if (((q[0] >= 'A' && q[0] <= 'Z') || (q[0] >= 'a' && q[0] <= 'z')) && q[1] == ':')
        q += 2;
    while (*q == '/') q++;
    {
        size_t rl = strlen(g_root);
        while (rl && strncmp(q, g_root, rl) == 0 && (q[rl] == 0 || q[rl] == '/')) {
            q += rl;
            while (*q == '/') q++;
        }
    }
    snprintf(g_cwd, sizeof g_cwd, "%s", q);
}
const char *ka_fs_cwd(void) { return g_cwd; }
uint32_t ka_file_read_at(uint32_t tok, void *buf, uint32_t n, uint64_t off, uint32_t *got);
uint32_t ka_file_read_at(uint32_t tok, void *buf, uint32_t n, uint64_t off, uint32_t *got) {
    kfile *f = kf(tok);
    ssize_t r;
    if (!f) { if (got) *got = 0; return 0; }
    r = pread(f->fd, buf, n, (off_t)off);
    if (r < 0) r = 0;
    if (got) *got = (uint32_t)r;
    return 1;
}
