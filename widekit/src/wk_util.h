/* widekit - MIT licence.  Internal helpers (not part of the public API). */
#ifndef WK_UTIL_H
#define WK_UTIL_H

#include <stdint.h>
#include <stddef.h>

static inline int32_t wk_floordiv(int32_t a, int32_t b)
{
    int32_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0)))
        q--;
    return q;
}

static inline int32_t wk_floormod(int32_t a, int32_t b)
{
    int32_t r = a % b;
    if (r != 0 && ((r < 0) != (b < 0)))
        r += b;
    return r;
}

/* Signed shortest difference of two positions on a ring of `wrap` pixels:
 * result in (-wrap/2, wrap/2]. */
static inline int32_t wk_wrapdiff(int32_t d, int32_t wrap)
{
    d = wk_floormod(d, wrap);
    if (d > wrap / 2)
        d -= wrap;
    return d;
}

static inline int32_t wk_abs(int32_t a) { return a < 0 ? -a : a; }
static inline int32_t wk_min(int32_t a, int32_t b) { return a < b ? a : b; }
static inline int32_t wk_max(int32_t a, int32_t b) { return a > b ? a : b; }
static inline int32_t wk_clamp(int32_t v, int32_t lo, int32_t hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline uint32_t wk_hash2(uint32_t a, uint32_t b)
{
    a ^= b * 0x9E3779B1u;
    a = (a << 13) | (a >> 19);
    a *= 0x85EBCA6Bu;
    a ^= a >> 15;
    a *= 0xC2B2AE35u;
    return a ^ (a >> 16);
}

/* CRC-32 (IEEE), table built on first use (idempotent, benign race). */
uint32_t wk_crc32(uint32_t crc, const void *data, size_t len);

#endif
