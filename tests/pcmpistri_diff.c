/* pcmpistri differential harness: real SSE4.2 vs the portable body.
 *
 * recomp_pcmpistri() in pcrecomp's runtime has two implementations behind one
 * signature (runtime/recomp32/recomp_types.h):
 *
 *   MSVC + x86  -> _mm_cmpistri/_mm_cmpistrz/_mm_cmpistrc, i.e. the CPU itself
 *   everything else (arm64) -> the portable C loop
 *
 * The Windows host therefore never exercises the arm64 path. This harness
 * closes that gap without relifting and without touching the pinned pcrecomp
 * tree: build the same source twice - Win32 (intrinsics) and x64 (the guard
 * requires _M_IX86, so x64 falls through to the portable body) - and compare
 * every (index, ZF, CF) triple. Both builds run on the same CPU, and the code
 * is integer-only, so a difference is a difference between the two
 * implementations rather than between two machines.
 *
 * Usage:
 *   pcmpistri_diff <results.bin>          run all cases, 8 bytes each
 *   pcmpistri_diff --dump <case-index>    print one case's operands
 *
 * Build/run: tests\run_pcmpistri_diff.ps1
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recomp_types.h"   /* -I <pcrecomp>/runtime/recomp32 */

/* imm8 values that recomp_pcmpistri()'s x86 switch covers. Any other value
 * falls through to the portable body on x86 too, so there is nothing to
 * compare against. Korra's own six sites use 0x0C, 0x0D and 0x40 only
 * (tools/scan_pcmpistri.py); the rest are future-proofing. */
static const unsigned kImm[] = {
    0x00, 0x04, 0x08, 0x0C, 0x0D, 0x10, 0x14, 0x18,
    0x1C, 0x20, 0x30, 0x38, 0x40, 0x44, 0x48, 0x4C
};
#define IMM_COUNT ((unsigned)(sizeof kImm / sizeof kImm[0]))

#define CASES 470000u

static uint32_t g_lcg = 0x12345678u;

static uint32_t rnd(void) {
    g_lcg = g_lcg * 1664525u + 1013904223u;
    return g_lcg >> 8;
}

/* Fill a/b for case n. Must be called in increasing n: it is a single PRNG
 * stream, which keeps --dump able to replay any case cheaply. */
static void gen_case(unsigned long n, uint8_t a[16], uint8_t b[16]) {
    unsigned i;
    unsigned cat = (n < 200000u) ? 0 : (n < 220000u ? 1 : (n < 420000u ? 2 : 3));
    if (cat == 0) {
        /* CRT-shaped: printable ASCII, NUL-terminated at a random length. */
        unsigned la = rnd() % 16, lb = rnd() % 16;
        for (i = 0; i < 16; i++) {
            a[i] = (uint8_t)('a' + (rnd() % 4));
            b[i] = (uint8_t)('a' + (rnd() % 4));
        }
        a[la] = 0;
        b[lb] = 0;
    } else if (cat == 1) {
        /* Adversarial shapes: no NUL at all, all-zero, one byte repeated. */
        memset(a, (int)(0x41 + (n % 26)), 16);
        memset(b, (int)(0x41 + ((n / 26) % 26)), 16);
        if (n % 3 == 0) memset(a, 0, 16);
        if (n % 5 == 0) memset(b, 0x7F, 16);
    } else if (cat == 2) {
        /* Random bytes with random NUL placement. */
        for (i = 0; i < 16; i++) {
            a[i] = (uint8_t)rnd();
            b[i] = (uint8_t)rnd();
        }
        if (rnd() % 3) a[rnd() % 16] = 0;
        if (rnd() % 3) b[rnd() % 16] = 0;
    } else {
        /* A short needle copied out of b, so first-position hits are common -
         * the shapes that reach the equal-ordered/negative paths. */
        unsigned len = 1 + (unsigned)(rnd() % 4), pos = (unsigned)(rnd() % 13);
        for (i = 0; i < 16; i++) b[i] = (uint8_t)('a' + (rnd() % 3));
        for (i = 0; i < 16; i++) a[i] = 0;
        for (i = 0; i < len; i++) a[i] = b[pos + i];
        b[15 - (rnd() % 2)] = 0;
    }
}

static int dump(unsigned long want) {
    unsigned long n;
    uint8_t a[16], b[16];
    unsigned k;
    g_lcg = 0x12345678u;
    for (n = 0; n <= want; n++) gen_case(n, a, b);
    printf("case=%lu\na=", want);
    for (k = 0; k < 16; k++) printf("%02X", a[k]);
    printf("\nb=");
    for (k = 0; k < 16; k++) printf("%02X", b[k]);
    printf("\nimm_values=");
    for (k = 0; k < IMM_COUNT; k++) printf("%s%02X", k ? "," : "", kImm[k]);
    printf("\n");
    return 0;
}

int main(int argc, char **argv) {
    unsigned long per_imm_cases[IMM_COUNT];
    uint32_t per_imm_hash[IMM_COUNT];
    uint8_t a[16], b[16];
    FILE *out;
    unsigned long n, tuples = 0;
    unsigned k;

    if (argc > 1 && strcmp(argv[1], "--dump") == 0)
        return dump(argc > 2 ? strtoul(argv[2], NULL, 10) : 0);

    if (argc < 2) {
        fprintf(stderr, "usage: %s <results.bin> | --dump <case>\n", argv[0]);
        return 2;
    }
    out = fopen(argv[1], "wb");
    if (!out) { perror(argv[1]); return 2; }

    for (k = 0; k < IMM_COUNT; k++) {
        per_imm_cases[k] = 0;
        per_imm_hash[k] = 2166136261u;
    }

    for (n = 0; n < CASES; n++) {
        gen_case(n, a, b);
        for (k = 0; k < IMM_COUNT; k++) {
            uint32_t flags = 0;
            uint32_t idx = recomp_pcmpistri(a, b, kImm[k], &flags);
            uint8_t rec[8];
            rec[0] = (uint8_t)idx;
            rec[1] = (uint8_t)(idx >> 8);
            rec[2] = (uint8_t)(flags & 3u);
            rec[3] = (uint8_t)kImm[k];
            rec[4] = (uint8_t)n;
            rec[5] = (uint8_t)(n >> 8);
            rec[6] = (uint8_t)(n >> 16);
            rec[7] = 0;
            if (fwrite(rec, 1, 8, out) != 8) { perror("write"); fclose(out); return 2; }
            per_imm_hash[k] ^= (uint32_t)(idx | ((flags & 3u) << 8));
            per_imm_hash[k] *= 16777619u;
            per_imm_cases[k]++;
            tuples++;
        }
    }
    fclose(out);

    printf("cases=%lu tuples=%lu\n", (unsigned long)CASES, tuples);
    for (k = 0; k < IMM_COUNT; k++)
        printf("imm=%02X hash=%08X n=%lu\n", kImm[k], per_imm_hash[k], per_imm_cases[k]);
    return 0;
}
