/* Probe: what does the real CPU do for the pcmpistri shapes we care about?
 * Strings are copied WITH their terminator, then the rest is filled with 'z'. */
#include <stdio.h>
#include <string.h>
#include <nmmintrin.h>

static void probe(const char *tag, const char *a, const char *b, int imm) {
    unsigned char A[16], B[16];
    size_t la = strlen(a), lb = strlen(b);
    memset(A, 'z', 16); memset(B, 'z', 16);
    if (la > 15) la = 16; else { memcpy(A, a, la); A[la] = 0; }
    if (lb > 15) lb = 16; else { memcpy(B, b, lb); B[lb] = 0; }
    __m128i va = _mm_loadu_si128((const __m128i *)A);
    __m128i vb = _mm_loadu_si128((const __m128i *)B);
    int idx = -1, cf = -1, zf = -1;
    switch (imm) {
    case 0x08: idx = _mm_cmpistri(va, vb, 0x08); cf = _mm_cmpistrc(va, vb, 0x08); zf = _mm_cmpistrz(va, vb, 0x08); break;
    case 0x18: idx = _mm_cmpistri(va, vb, 0x18); cf = _mm_cmpistrc(va, vb, 0x18); zf = _mm_cmpistrz(va, vb, 0x18); break;
    case 0x0C: idx = _mm_cmpistri(va, vb, 0x0C); cf = _mm_cmpistrc(va, vb, 0x0C); zf = _mm_cmpistrz(va, vb, 0x0C); break;
    case 0x00: idx = _mm_cmpistri(va, vb, 0x00); cf = _mm_cmpistrc(va, vb, 0x00); zf = _mm_cmpistrz(va, vb, 0x00); break;
    default: return;
    }
    printf("%-18s imm=%02X a=%-6s b=%-6s -> index=%2d cf=%d zf=%d\n", tag, imm, a, b, idx, cf, zf);
}

int main(void) {
    const int imms[] = {0x08, 0x18, 0x0C, 0x00};
    for (int i = 0; i < 4; i++) {
        int imm = imms[i];
        probe("equal", "abc", "abc", imm);
        probe("differ at 2", "abc", "abd", imm);
        probe("a empty", "", "abc", imm);
        probe("b empty", "abc", "", imm);
        probe("both empty", "", "", imm);
        probe("a prefix of b", "ab", "abcd", imm);
        probe("b prefix of a", "abcd", "ab", imm);
        probe("substring", "bc", "abcd", imm);
        probe("no null", "abcdefghijklmnop", "abcdefghijklmnop", imm);
        printf("\n");
    }
    return 0;
}
