/* Differential test: recomp_pcmpistri() against the host CPU's own PCMPISTRI.
 *
 *   cl /nologo /O2 /I..\..\pcrecomp\runtime\recomp32 tests\pcmpistri_selftest.c
 *   pcmpistri_selftest.exe
 *
 * The recompiled runtime has to model this instruction for MSVC's CRT string
 * fast paths; the host is x86 with SSE4.2, so the real instruction is the
 * oracle. Compares the index (ECX) and the two flags the generated code
 * consumes (CF, ZF) over random byte strings, for the imm8 values LoK.exe uses.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <nmmintrin.h>
#include "recomp_types.h"

static unsigned long g_seed = 12345;
static unsigned rnd(void) {
    g_seed = g_seed * 1103515245u + 12345u;
    return (unsigned)((g_seed >> 16) & 0x7FFF);
}

/* A string of random letters with a null somewhere (or none at all). */
static void fill(uint8_t *s) {
    int n = (int)(rnd() % 17);          /* 0..16 characters */
    for (int i = 0; i < 16; i++) s[i] = (uint8_t)('a' + rnd() % 4);
    if (n < 16) s[n] = 0;
}

int main(void) {
    const unsigned imms[] = {0x0C, 0x0D, 0x08, 0x18, 0x38, 0x00, 0x04, 0x10, 0x40, 0x0C};
    const unsigned n_imms = sizeof imms / sizeof imms[0];
    int fails = 0, cases = 0;

    for (int t = 0; t < 20000; t++) {
        uint8_t a[16], b[16];
        fill(a);
        fill(b);
        /* Bias towards the equal-each shapes the CRT actually uses. */
        if (t & 1) memcpy(a, b, 16);

        __m128i va = _mm_loadu_si128((const __m128i *)a);
        __m128i vb = _mm_loadu_si128((const __m128i *)b);

        for (unsigned k = 0; k < n_imms; k++) {
            unsigned imm = imms[k];
            uint32_t flags = 0;
            uint32_t mine;
            int cpu = -1, cpu_cf = 0, cpu_zf = 0;

            /* The intrinsics take imm8 as a literal, so dispatch explicitly. */
            switch (imm) {
            case 0x0C: mine = recomp_pcmpistri(a, b, 0x0C, &flags); cpu = _mm_cmpistri(va, vb, 0x0C); cpu_cf = _mm_cmpistrc(va, vb, 0x0C); cpu_zf = _mm_cmpistrz(va, vb, 0x0C); break;
            case 0x0D: mine = recomp_pcmpistri(a, b, 0x0D, &flags); cpu = _mm_cmpistri(va, vb, 0x0D); cpu_cf = _mm_cmpistrc(va, vb, 0x0D); cpu_zf = _mm_cmpistrz(va, vb, 0x0D); break;
            case 0x08: mine = recomp_pcmpistri(a, b, 0x08, &flags); cpu = _mm_cmpistri(va, vb, 0x08); cpu_cf = _mm_cmpistrc(va, vb, 0x08); cpu_zf = _mm_cmpistrz(va, vb, 0x08); break;
            case 0x18: mine = recomp_pcmpistri(a, b, 0x18, &flags); cpu = _mm_cmpistri(va, vb, 0x18); cpu_cf = _mm_cmpistrc(va, vb, 0x18); cpu_zf = _mm_cmpistrz(va, vb, 0x18); break;
            case 0x38: mine = recomp_pcmpistri(a, b, 0x38, &flags); cpu = _mm_cmpistri(va, vb, 0x38); cpu_cf = _mm_cmpistrc(va, vb, 0x38); cpu_zf = _mm_cmpistrz(va, vb, 0x38); break;
            case 0x00: mine = recomp_pcmpistri(a, b, 0x00, &flags); cpu = _mm_cmpistri(va, vb, 0x00); cpu_cf = _mm_cmpistrc(va, vb, 0x00); cpu_zf = _mm_cmpistrz(va, vb, 0x00); break;
            case 0x04: mine = recomp_pcmpistri(a, b, 0x04, &flags); cpu = _mm_cmpistri(va, vb, 0x04); cpu_cf = _mm_cmpistrc(va, vb, 0x04); cpu_zf = _mm_cmpistrz(va, vb, 0x04); break;
            case 0x10: mine = recomp_pcmpistri(a, b, 0x10, &flags); cpu = _mm_cmpistri(va, vb, 0x10); cpu_cf = _mm_cmpistrc(va, vb, 0x10); cpu_zf = _mm_cmpistrz(va, vb, 0x10); break;
            case 0x40: mine = recomp_pcmpistri(a, b, 0x40, &flags); cpu = _mm_cmpistri(va, vb, 0x40); cpu_cf = _mm_cmpistrc(va, vb, 0x40); cpu_zf = _mm_cmpistrz(va, vb, 0x40); break;
            default: continue;
            }

            cases++;
            if ((int)mine != cpu || (int)((flags >> 1) & 1) != cpu_cf || (int)(flags & 1) != cpu_zf) {
                if (fails < 10) {
                    printf("MISMATCH imm=%02X: index mine=%u cpu=%d | CF mine=%u cpu=%d | ZF mine=%u cpu=%d\n",
                           imm, mine, cpu, (unsigned)((flags >> 1) & 1), cpu_cf,
                           (unsigned)(flags & 1), cpu_zf);
                    printf("  a = "); for (int i = 0; i < 16; i++) printf("%02X ", a[i]); printf("\n");
                    printf("  b = "); for (int i = 0; i < 16; i++) printf("%02X ", b[i]); printf("\n");
                }
                fails++;
            }
        }
    }
    printf("%d cases, %d mismatches\n", cases, fails);
    return fails ? 1 : 0;
}
