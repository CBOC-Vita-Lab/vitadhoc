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
    /* the other row kernels (wk_compose.h): the vector paths against their plain definitions */
    long kbad = 0, kn = 0;
    for (int iter = 0; iter < 4000; iter++) {
        enum { N = 140 };
        const int n = (int)(rnd() % 110), off = (int)(rnd() % 9);
        uint16_t p[N], q[N], r1[N], r2[N], f[N];
        uint8_t m[N], q1[N], q2[N], th[N];
        for (int i = 0; i < N; i++) {
            p[i] = (uint16_t)rnd(); q[i] = (uint16_t)rnd();
            m[i] = (uint8_t)((rnd() & 3) ? rnd() : 0);
            f[i] = (uint16_t)(rnd() % 257); th[i] = (uint8_t)(rnd() % 16);
            q1[i] = q2[i] = (uint8_t)rnd();
        }
        const int e0 = (int)(rnd() % 256), e1 = (int)(rnd() % 256), e2 = (int)(rnd() % 256), wt = (int)(rnd() % 33);
        memcpy(r1, p, sizeof r1); memcpy(r2, p, sizeof r2);
        wk_soft_row(r1 + off, n, e0, e1, e2, f + off, th + off);
        wk_soft_row_scalar(r2 + off, n, e0, e1, e2, f + off, th + off);
        kbad += memcmp(r1, r2, sizeof r1) != 0;
        memcpy(r1, p, sizeof r1); memcpy(r2, p, sizeof r2);
        wk_mix565_row(r1 + off, q + off, wt, n);
        wk_mix565_row_scalar(r2 + off, q + off, wt, n);
        kbad += memcmp(r1, r2, sizeof r1) != 0;
        memcpy(r1, p, sizeof r1); memcpy(r2, p, sizeof r2);
        wk_copy_masked(r1 + off, q + off, m + off, n);
        for (int i = 0; i < n; i++) if (m[off + i]) r2[off + i] = q[off + i];
        kbad += memcmp(r1, r2, sizeof r1) != 0;
        wk_rev16(r1 + off, q + off, n);
        for (int i = 0; i < n; i++) r2[off + i] = q[off + n - 1 - i];
        kbad += memcmp(r1, r2, sizeof r1) != 0;
        wk_rev16_inplace(r1 + off, n);
        for (int i = 0; i < n; i++) r2[off + i] = q[off + i];
        kbad += memcmp(r1, r2, sizeof r1) != 0;
        wk_fill_masked8(q1 + off, m + off, 0x4B, n);
        for (int i = 0; i < n; i++) if (m[off + i]) q2[off + i] = 0x4B;
        kbad += memcmp(q1, q2, sizeof q1) != 0;
        wk_or_nz8(q1 + off, m + off, n);
        for (int i = 0; i < n; i++) q2[off + i] |= (uint8_t)(m[off + i] != 0);
        kbad += memcmp(q1, q2, sizeof q1) != 0;
        wk_nz8(q1 + off, m + off, n);
        for (int i = 0; i < n; i++) q2[off + i] = (uint8_t)(m[off + i] != 0);
        kbad += memcmp(q1, q2, sizeof q1) != 0;
        wk_rev8(q1 + off, m + off, n);
        for (int i = 0; i < n; i++) q2[off + i] = m[off + n - 1 - i];
        kbad += memcmp(q1, q2, sizeof q1) != 0;
        wk_rev8_inplace(q1 + off, n);
        for (int i = 0; i < n; i++) q2[off + i] = m[off + i];
        kbad += memcmp(q1, q2, sizeof q1) != 0;
        kn += 10;
    }
    printf("row kernels: %ld runs, %ld mismatching\n", kn, kbad);
    return (bad || kbad) ? 1 : 0;
}
