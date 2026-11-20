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

const char *ka_fs_resolve(const char *guest_path, char *out, size_t cap) {
    char norm[768];
    size_t i, j = 0;
    const char *p = guest_path;

    if (!p || !*p) { snprintf(out, cap, "%s", g_root); return out; }

    for (i = 0; p[i] && j + 1 < sizeof norm; i++)
        norm[j++] = (p[i] == '\\') ? '/' : p[i];
    norm[j] = 0;

    /* An absolute path: either the one GetModuleFileNameA hands the engine or an
     * absolute path the engine built from it. Never re-root it. */
    if (norm[0] == '/') { snprintf(out, cap, "%s", norm); return out; }

    /* drive letter / UNC prefix */
    p = norm;
    if (((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':')
        p += 2;
    while (*p == '/' || *p == '\\') p++;
    snprintf(norm, sizeof norm, "%s", p);

    /* A path the engine built from its own module name: keep from GameData. */
    {
        char *g = strstr(norm, "GameData/");
        if (g && g != norm) memmove(norm, g, strlen(g) + 1);
    }

    if (!g_cwd[0]) snprintf(out, cap, "%s/%s", g_root, norm);
    else if (g_cwd[0] == '/') snprintf(out, cap, "%s/%s", g_cwd, norm);
    else snprintf(out, cap, "%s/%s/%s", g_root, g_cwd, norm);
    return out;
}

/* ---- handles ---------------------------------------------------------- */
#define MAX_FILES 512
typedef struct { int fd; int used; } kfile;
static kfile g_files[MAX_FILES];

static kfile *kf(uint32_t tok) {
    uint32_t i = tok - 1;
    if (tok == 0 || i >= MAX_FILES || !g_files[i].used) return NULL;
    return &g_files[i];
}

uint32_t ka_file_open(const char *guest_path) {
    char path[1024];
    int fd;
    ka_fs_resolve(guest_path, path, sizeof path);
    fd = open(path, O_RDWR);
    if (fd < 0) fd = open(path, O_RDONLY);
    if (fd < 0) {
        if (ka_trace_files) ka_log("[fs] open %s -> %s", guest_path, strerror(errno));
        return 0;
    }
    if (ka_trace_files) ka_log("[fs] open %s -> %s", guest_path, path);
    for (uint32_t i = 0; i < MAX_FILES; i++) {
        if (!g_files[i].used) {
            g_files[i].used = 1;
            g_files[i].fd = fd;
            return i + 1;
        }
    }
    close(fd);
    return 0;
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

int ka_file_close(uint32_t tok) {
    kfile *f = kf(tok);
    if (!f) return 0;
    close(f->fd);
    f->used = 0;
    return 1;
}

/* ---- guest cwd -------------------------------------------------------- */
void ka_fs_set_cwd(const char *p) {
    if (!p || !*p) { g_cwd[0] = 0; return; }
    snprintf(g_cwd, sizeof g_cwd, "%s", p);
    for (char *c = g_cwd; *c; c++) if (*c == '\\') *c = '/';
    /* Keep an absolute path absolute; only a relative one is rooted later. */
    if (g_cwd[0] != '/') while (g_cwd[0] == '/') memmove(g_cwd, g_cwd + 1, strlen(g_cwd));
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
