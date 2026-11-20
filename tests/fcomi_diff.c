/* FCOMI/FUCOMI differential harness: the instruction's flags vs the runtime's
 * model, plus the LAHF idiom the eight Korra sites use.
 *
 * Why this exists: the lifter publishes FCOMI/FCOMIP/FUCOMI/FUCOMIP as an
 * EFLAGS image through recomp_fcomi_eflags() (pcrecomp Korra-0006). On x86 the
 * instruction itself is available, so a harness can compare the model against
 * the CPU on the same inputs; on arm64 the same table must come out, which is
 * what proves the model on silicon.
 *
 * The eight sites in LoK.exe are MSVC's "is this float zero":
 *     fucompi st(1) / fstp st(0) / lahf / test ah, 0x44 / jnp
 * so the harness compares the AH byte LAHF produces, and the ZF/PF that the
 * following `test ah, 0x44` derives from it -- the values the branch reads.
 *
 * Usage:
 *   fcomi_diff                print the case/hash table
 *   fcomi_diff --verbose      one line per case (first differences only)
 *
 * Build/run x86 (oracle): tests\run_fcomi_diff.ps1
 * Build/run arm64 (silicon): scripts\android_probe.ps1 -Name fcomi_diff \
 *     -Source tests\fcomi_diff.c -Include ..\pcrecomp\runtime\recomp32
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "recomp_types.h"

#if defined(_MSC_VER) && defined(_M_IX86)
#define HAVE_HW 1
#else
#define HAVE_HW 0
#endif

/* Hardware FUCOMIP st(0), st(1): flags as the CPU leaves them. The x87 stack
 * is left empty, as the site's `fstp st(0)` does. */
static uint32_t hw_flags(const float *pa, const float *pb) {
#if HAVE_HW
    uint32_t r;
    __asm {
        push eax
        push ebx
        mov  ebx, pb
        fld  dword ptr [ebx]
        mov  ebx, pa
        fld  dword ptr [ebx]
        fucomip st, st(1)
        fstp st(0)
        pushfd
        pop  eax
        mov  r, eax
        pop  ebx
        pop  eax
    }
    return r;
#else
    (void)pa; (void)pb;
    return 0xFFFFFFFFu;
#endif
}

/* LAHF's AH: SF ZF 0 AF 0 PF 1 CF. */
static uint32_t lahf_ah(uint32_t eflags) {
    uint32_t e = recomp_eflags(FK_EFLAGS, eflags, 0, eflags & 1u, 0);
    return ((e & 0xD5u) | 2u);
}

/* `test ah, imm8`: ZF and PF as the branch after it sees them. */
static uint32_t test_ah(uint32_t ah, uint32_t imm) {
    uint32_t r = (ah & 0xFFFFu) & (imm & 0xFFu);
    return (r == 0 ? 1u : 0u) | (recomp_parity8((uint8_t)r) ? 2u : 0u);
}

static uint32_t g_lcg = 0x9E3779B9u;
static uint32_t rnd(void) {
    g_lcg = g_lcg * 1664525u + 1013904223u;
    return g_lcg;
}

#define FIXED_CASES 24
static const uint32_t kFixed[FIXED_CASES] = {
    0x00000000u, /* +0                     */
    0x80000000u, /* -0                     */
    0x3F800000u, /* 1.0                    */
    0xBF800000u, /* -1.0                   */
    0x40000000u, /* 2.0                    */
    0x3F000000u, /* 0.5                    */
    0x40490FDBu, /* pi ~3.14159            */
    0x40490FDAu, /* pi's neighbour         */
    0x7F800000u, /* +inf                   */
    0xFF800000u, /* -inf                   */
    0x7FC00000u, /* quiet NaN              */
    0x7F800001u, /* signalling NaN         */
    0xFFC00001u, /* negative quiet NaN     */
    0x00000001u, /* smallest denormal      */
    0x80000001u, /* -smallest denormal     */
    0x00800000u, /* smallest normal        */
    0x7F7FFFFFu, /* FLT_MAX                */
    0xFF7FFFFFu, /* -FLT_MAX               */
    0x3F800001u, /* 1.0 + 1 ulp            */
    0x3F7FFFFFu, /* 1.0 - 1 ulp            */
    0x4B000000u, /* 2^23                   */
    0x4B800000u, /* 2^24                   */
    0xC2C80000u, /* -100.0                 */
    0x42480000u, /* 50.0                   */
};

#define RANDOM_CASES 200000u
#define CASES (FIXED_CASES * FIXED_CASES + RANDOM_CASES)

/* Deterministic pair stream: fixed cross product first, then LCG bits, re-seeded
 * per case so the stream does not depend on evaluation order. A pure function
 * of n lets both machines generate identical inputs without a table. */
static void gen_case(unsigned long n, uint32_t *pa, uint32_t *pb) {
    if (n < (unsigned long)(FIXED_CASES * FIXED_CASES)) {
        *pa = kFixed[n / FIXED_CASES];
        *pb = kFixed[n % FIXED_CASES];
        return;
    }
    /* Re-seed per case so the stream does not depend on evaluation order. */
    g_lcg = 0x9E3779B9u ^ (uint32_t)n;
    rnd();
    *pa = rnd();
    *pb = rnd();
}

int main(int argc, char **argv) {
    int verbose = argc > 1 && strcmp(argv[1], "--verbose") == 0;
    uint32_t hw_hash = 2166136261u, model_hash = 2166136261u;
    uint32_t ah_hw_hash = 2166136261u, ah_model_hash = 2166136261u;
    uint32_t mismatches = 0;
    unsigned long n;

    for (n = 0; n < CASES; n++) {
        uint32_t pa, pb;
        float a, b;
        int32_t cmp;
        uint32_t model, hw = 0, ah_m, ah_hw = 0;

        gen_case(n, &pa, &pb);
        memcpy(&a, &pa, 4);
        memcpy(&b, &pb, 4);

        cmp = FPU_CMP(a, b);            /* the lifter's _fpu_cmp */
        model = recomp_fcomi_eflags(cmp);
        ah_m = lahf_ah(model);

        model_hash ^= (model & 0x8D5u);
        model_hash *= 16777619u;
        ah_model_hash ^= (ah_m & 0xD5u) | (test_ah(ah_m, 0x44) << 8);
        ah_model_hash *= 16777619u;

#if HAVE_HW
        hw = hw_flags(&a, &b);
        ah_hw = lahf_ah(hw);
        hw_hash ^= (hw & 0x8D5u);
        hw_hash *= 16777619u;
        ah_hw_hash ^= (ah_hw & 0xD5u) | (test_ah(ah_hw, 0x44) << 8);
        ah_hw_hash *= 16777619u;

        if (((hw & 0x8D5u) != (model & 0x8D5u)) ||
            ((ah_hw & 0xD5u) != (ah_m & 0xD5u)) ||
            (test_ah(ah_hw, 0x44) != test_ah(ah_m, 0x44))) {
            if (mismatches < 10 || verbose)
                printf("MISMATCH case=%lu a=%08X b=%08X cmp=%d hw=%03X model=%03X ah_hw=%02X ah_model=%02X\n",
                       n, pa, pb, cmp, hw & 0x8D5u, model & 0x8D5u,
                       ah_hw & 0xD5u, ah_m & 0xD5u);
            mismatches++;
        }
#endif
    }

    printf("cases=%lu\n", (unsigned long)CASES);
    printf("model_eflags_hash=%08X\n", model_hash);
    printf("model_ah_hash=%08X\n", ah_model_hash);
#if HAVE_HW
    printf("hw_eflags_hash=%08X\n", hw_hash);
    printf("hw_ah_hash=%08X\n", ah_hw_hash);
    printf("hw_vs_model_eflags=%s\n", hw_hash == model_hash ? "EQUAL" : "DIFFER");
    printf("hw_vs_model_ah=%s\n", ah_hw_hash == ah_model_hash ? "EQUAL" : "DIFFER");
    printf("mismatches=%u\n", mismatches);
    if (mismatches) return 1;
    printf("RESULT: PASS - the FCOMI* model reproduces the CPU on all %lu cases\n",
           (unsigned long)CASES);
#else
    printf("hw=n/a (portable build; compare these hashes against the x86 run)\n");
    printf("RESULT: PORTABLE-TABLE (imm=%s)\n", "compare hashes externally");
#endif
    return 0;
}
