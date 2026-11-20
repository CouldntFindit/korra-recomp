/* Site-shaped check for the six PCMPISTRI sites in LoK.exe.
 *
 * The differential harness (pcmpistri_diff.c) proves the portable body against
 * the instruction over 7.52 M generated tuples. This file pins the *shapes the
 * game actually feeds those six sites* so the fix is tied to Korra, not only to
 * a random generator:
 *
 *   0x77A34F / 0x77A38B  imm8 0x0C  ubyte equal-ordered  (CRT strstr path)
 *       a = 16-byte needle block (xmm0), b = 16-byte haystack window at [eax]
 *       Shapes: asset paths and class names, NUL-terminated, match at 0/mid/15,
 *       no match, empty needle.
 *   0x77CF3D / 0x77CF53 / 0x77CF88  imm8 0x0D  uword equal-ordered (wide CRT)
 *       a = 8 UTF-16 units of the needle (xmm0), b = 8-unit haystack window.
 *       The site scales the returned element index by two (`lea edx,[edx+ecx*2]`),
 *       so this check compares the element index, not a byte offset.
 *   0x77C33B  imm8 0x40  ubyte equal-any, most-significant index (strrchr path)
 *       a = one byte in xmm0 (the searched character), b = byte window.
 *
 * Both builds (Win32 intrinsics, x64 portable) print the same table; the runner
 * diffs the text. Any difference is a portable-body defect on a real input.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "recomp_types.h"

typedef struct {
    const char *name;
    unsigned imm;
    uint8_t a[16];
    uint8_t b[16];
} site_case;

static void put_bytes(uint8_t dst[16], const char *s) {
    size_t n = strlen(s);
    memset(dst, 0, 16);
    if (n > 15) n = 15;
    memcpy(dst, s, n);
    dst[n] = 0;
}

/* UTF-16LE units, NUL-terminated, padded to 16 bytes. */
static void put_wide(uint8_t dst[16], const wchar_t *s) {
    unsigned i;
    memset(dst, 0, 16);
    for (i = 0; i < 7 && s[i]; i++) {
        dst[2 * i] = (uint8_t)(s[i] & 0xFF);
        dst[2 * i + 1] = (uint8_t)((s[i] >> 8) & 0xFF);
    }
    dst[2 * i] = 0;
    dst[2 * i + 1] = 0;
}

int main(void) {
    static site_case cases[24];
    static const char *kPaths[] = {
        "pl\\pl00f0.dat", "pl0000_battleParameter.bin", "Player",
        "BehaviorPlayer", "core\\coreui.dat", "GameData"
    };
    unsigned n = 0, i;

    /* 0x0C: needle in a, haystack window in b. */
    for (i = 0; i < 6; i++) {
        cases[n].name = kPaths[i];
        cases[n].imm = 0x0Cu;
        put_bytes(cases[n].a, kPaths[i]);
        put_bytes(cases[n].b, i & 1 ? "xxPlayerxx" : "pl\\pl00f0.dat.bak");
        n++;
    }
    cases[n].name = "empty needle";
    cases[n].imm = 0x0C; put_bytes(cases[n].a, ""); put_bytes(cases[n].b, "GameData"); n++;
    cases[n].name = "match at 15";
    cases[n].imm = 0x0C; put_bytes(cases[n].a, "Z"); put_bytes(cases[n].b, "0123456789ABCDEZ"); n++;
    cases[n].name = "no match";
    cases[n].imm = 0x0C; put_bytes(cases[n].a, "needle"); put_bytes(cases[n].b, "haystack"); n++;
    cases[n].name = "needle spans window";
    cases[n].imm = 0x0C; put_bytes(cases[n].a, "_battleParameter"); put_bytes(cases[n].b, "_battleParameter"); n++;

    /* 0x0D: same names as UTF-16, as the wide CRT sees them. */
    for (i = 0; i < 6; i++) {
        wchar_t w[8];
        unsigned k;
        for (k = 0; k < 7 && kPaths[i][k]; k++) w[k] = (wchar_t)(unsigned char)kPaths[i][k];
        w[k] = 0;
        cases[n].name = kPaths[i];
        cases[n].imm = 0x0Du;
        put_wide(cases[n].a, w);
        put_wide(cases[n].b, i & 1 ? L"xxPlayer" : L"pl\\pl00f0");
        n++;
    }
    cases[n].name = "wide empty needle";
    cases[n].imm = 0x0D; put_wide(cases[n].a, L""); put_wide(cases[n].b, L"GameData"); n++;
    cases[n].name = "wide no null";
    cases[n].imm = 0x0D; put_wide(cases[n].a, L"Player"); put_wide(cases[n].b, L"01234567"); n++;

    /* 0x40: single searched byte in a, msb index over the haystack. */
    cases[n].name = "strrchr backslash";
    cases[n].imm = 0x40u; put_bytes(cases[n].a, "\\"); put_bytes(cases[n].b, "pl\\pl00f0.dat"); n++;
    cases[n].name = "strrchr dot";
    cases[n].imm = 0x40u; put_bytes(cases[n].a, "."); put_bytes(cases[n].b, "pl00f0.dat"); n++;
    cases[n].name = "strrchr absent";
    cases[n].imm = 0x40u; put_bytes(cases[n].a, "z"); put_bytes(cases[n].b, "GameData"); n++;

    for (i = 0; i < n; i++) {
        uint32_t flags = 0;
        uint32_t idx = recomp_pcmpistri(cases[i].a, cases[i].b, cases[i].imm, &flags);
        printf("%-28s imm=%02X idx=%2u ZF=%u CF=%u SF=%u OF=%u\n",
               cases[i].name, cases[i].imm, idx,
               flags & 1u, (flags >> 1) & 1u, (flags >> 2) & 1u, (flags >> 3) & 1u);
    }
    return 0;
}
