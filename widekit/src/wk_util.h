/* widekit - MIT licence.  Internal helpers (not part of the public API). */
#ifndef WK_UTIL_H
#define WK_UTIL_H

#include <stdint.h>
#include <stddef.h>

static inline int wk_is_pow2(int32_t b)
{
    return b > 0 && (b & (b - 1)) == 0;
}

static inline int wk_ctz32(uint32_t v)
{
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_ctz(v);
#else
    int s = 0;
    while ((v & 1u) == 0u) {
        v >>= 1;
        s++;
    }
    return s;
#endif
}

static inline int32_t wk_floordiv(int32_t a, int32_t b)
{
    if (wk_is_pow2(b))
        return a >> wk_ctz32((uint32_t)b);
    int32_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0)))
        q--;
    return q;
}

static inline int32_t wk_floormod(int32_t a, int32_t b)
{
    if (wk_is_pow2(b))
        return (int32_t)((uint32_t)a & (uint32_t)(b - 1));
    int32_t r = a % b;
    if (r != 0 && ((r < 0) != (b < 0)))
        r += b;
    return r;
}

/* Signed shortest difference of two positions on a ring of `wrap` pixels:
 * result in (-wrap/2, wrap/2]. */
static inline int32_t wk_wrapdiff(int32_t d, int32_t wrap)
{
    if (wk_is_pow2(wrap)) {
        d = (int32_t)((uint32_t)d & (uint32_t)(wrap - 1));
        if (d > (wrap >> 1))
            d -= wrap;
        return d;
    }
    if (wrap > 0) {
        const int32_t half = wrap >> 1;
        const int32_t lo = half - wrap + 1;
        if (d >= lo && d <= half)
            return d;
        if (d > half) {
            const int32_t t = d - wrap;
            if (t <= half)
                return t;
        } else {
            const int32_t t = d + wrap;
            if (t >= lo)
                return t;
        }
    }
    d = wk_floormod(d, wrap);
    if (d > wrap / 2)
        d -= wrap;
    return d;
}

/* Hoisted positive divisor / ring wrap for hot loops (zero __aeabi_idivmod on ARMv7). */
typedef struct WkDiv {
    int32_t b;
    uint32_t mask;
    int shift;
    int pow2;
} WkDiv;

static inline WkDiv wk_div_make(int32_t b)
{
    WkDiv d;
    d.b = b;
    d.pow2 = wk_is_pow2(b);
    d.mask = (uint32_t)(b - 1);
    d.shift = d.pow2 ? wk_ctz32((uint32_t)b) : 0;
    return d;
}

static inline int32_t wk_div_floor(int32_t a, const WkDiv *d)
{
    return d->pow2 ? (a >> d->shift) : wk_floordiv(a, d->b);
}

static inline int32_t wk_div_mod(int32_t a, const WkDiv *d)
{
    if (d->pow2)
        return (int32_t)((uint32_t)a & d->mask);
    if ((uint32_t)a < (uint32_t)d->b)
        return a;
    return wk_floormod(a, d->b);
}

typedef struct WkWrap {
    int32_t wrap;
    int32_t half;
    int32_t lo;
    uint32_t mask;
    int pow2;
} WkWrap;

static inline WkWrap wk_wrap_make(int32_t wrap)
{
    WkWrap w;
    w.wrap = wrap;
    w.half = wrap >> 1;
    w.lo = w.half - wrap + 1;
    w.pow2 = wk_is_pow2(wrap);
    w.mask = (uint32_t)(wrap - 1);
    return w;
}

static inline int32_t wk_wrap_diff(int32_t d, const WkWrap *w)
{
    if (w->pow2) {
        d = (int32_t)((uint32_t)d & w->mask);
        if (d > w->half)
            d -= w->wrap;
        return d;
    }
    if (d >= w->lo && d <= w->half)
        return d;
    if (d > w->half) {
        const int32_t t = d - w->wrap;
        if (t <= w->half)
            return t;
    } else {
        const int32_t t = d + w->wrap;
        if (t >= w->lo)
            return t;
    }
    return wk_wrapdiff(d, w->wrap);
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
