/* widekit tests - MIT licence.  NEON blend == scalar blend (run under qemu-arm). */
#include "wk_compose.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t rng = 12345;
static uint32_t rnd() { rng = rng * 1664525u + 1013904223u; return rng >> 8; }

int main()
{
    printf("NEON compiled in: %d\n", wk_have_neon());
    long bad = 0, total = 0;
    uint16_t src[300], a[300], b[300];
    uint8_t w[300], d[300];
    for (int iter = 0; iter < 4000; iter++) {
        int n = (int)(rnd() % 200);
        int off = (int)(rnd() % 8);
        for (int i = 0; i < 300; i++) {
            src[i] = (uint16_t)rnd();
            a[i] = b[i] = (uint16_t)rnd();
            w[i] = (uint8_t)(rnd() % 33);
            d[i] = (uint8_t)(rnd() % 32);
        }
        if (iter % 7 == 0) memset(w, 32, sizeof w);
        if (iter % 11 == 0) memset(w, 0, sizeof w);
        wk_blend_row_scalar(a + off, src + off, w + off, d + off, n);
        wk_blend_row(b + off, src + off, w + off, d + off, n);
        total += n;
        if (memcmp(a, b, sizeof a)) bad++;
    }
    printf("blend: %ld pixels, %ld mismatching rows\n", total, bad);
    return bad ? 1 : 0;
}
