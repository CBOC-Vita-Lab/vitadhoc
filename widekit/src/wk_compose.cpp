/* widekit - MIT licence.  wk_compose: see ../include/wk_compose.h. */

#include "wk_compose.h"
#include "wk_util.h"

#include <stdlib.h>
#include <string.h>

#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

/* ----- vectors (8 x 16 bit or 16 x 8 bit a step: NEON on the Vita, SSE2 on the host) ----- */

#if defined(__GNUC__) && (defined(__ARM_NEON) || defined(__SSE2__))
#define WK_VEC 1
namespace {
typedef uint16_t vu16 __attribute__((vector_size(16)));
typedef int16_t vs16 __attribute__((vector_size(16)));
typedef uint8_t vu8 __attribute__((vector_size(16)));
typedef uint8_t vu8h __attribute__((vector_size(8)));
template <class V> inline V vld(const void *p) { V v; memcpy(&v, p, sizeof v); return v; }
template <class V> inline void vst(void *p, V v) { memcpy(p, &v, sizeof v); }
inline vu16 widen8(const uint8_t *p) { return __builtin_convertvector(vld<vu8h>(p), vu16); }
inline vu16 splat16(unsigned v) { const uint16_t x = (uint16_t)v; return (vu16){x, x, x, x, x, x, x, x}; }
#if defined(__clang__)
inline vu16 rev16v(vu16 v) { return __builtin_shufflevector(v, v, 7, 6, 5, 4, 3, 2, 1, 0); }
#else
inline vu16 rev16v(vu16 v) { return __builtin_shuffle(v, (vu16){7, 6, 5, 4, 3, 2, 1, 0}); }
#endif
inline vu8 rev8v(vu8 v)
{
    vu16 w = (vu16)v;
    w = (vu16)((w << 8) | (w >> 8));                 /* bytes swapped inside each 16 bit lane */
    return (vu8)rev16v(w);
}
}  // namespace
#else
#define WK_VEC 0
#endif

/* ----- blend kernel ----------------------------------------------------------- */

void wk_blend_row_scalar(uint16_t *dst, const uint16_t *src, const uint8_t *w,
                         const uint8_t *dither, int n)
{
    for (int i = 0; i < n; i++) {
        unsigned s = src[i], d = dst[i];
        unsigned wi = w[i], iw = 32u - wi, t = dither[i];
        unsigned r = (((s >> 11) * wi) + ((d >> 11) * iw) + t) >> 5;
        unsigned g = ((((s >> 5) & 63u) * wi) + (((d >> 5) & 63u) * iw) + t) >> 5;
        unsigned b = (((s & 31u) * wi) + ((d & 31u) * iw) + t) >> 5;
        dst[i] = (uint16_t)((r << 11) | (g << 5) | b);
    }
}

int wk_have_neon(void)
{
#if defined(__ARM_NEON)
    return 1;
#else
    return 0;
#endif
}

void wk_blend_row(uint16_t *dst, const uint16_t *src, const uint8_t *w,
                  const uint8_t *dither, int n)
{
    int i = 0;
#if defined(__ARM_NEON)
    const uint16x8_t k32 = vdupq_n_u16(32);
    const uint16x8_t m63 = vdupq_n_u16(63);
    const uint16x8_t m31 = vdupq_n_u16(31);
    for (; i + 8 <= n; i += 8) {
        uint16x8_t s = vld1q_u16(src + i), d = vld1q_u16(dst + i);
        uint16x8_t wv = vmovl_u8(vld1_u8(w + i));
        uint16x8_t iw = vsubq_u16(k32, wv);
        uint16x8_t t = vmovl_u8(vld1_u8(dither + i));
        uint16x8_t r = vshrq_n_u16(
            vaddq_u16(vaddq_u16(vmulq_u16(vshrq_n_u16(s, 11), wv),
                                vmulq_u16(vshrq_n_u16(d, 11), iw)), t), 5);
        uint16x8_t g = vshrq_n_u16(
            vaddq_u16(vaddq_u16(vmulq_u16(vandq_u16(vshrq_n_u16(s, 5), m63), wv),
                                vmulq_u16(vandq_u16(vshrq_n_u16(d, 5), m63), iw)), t), 5);
        uint16x8_t b = vshrq_n_u16(
            vaddq_u16(vaddq_u16(vmulq_u16(vandq_u16(s, m31), wv),
                                vmulq_u16(vandq_u16(d, m31), iw)), t), 5);
        vst1q_u16(dst + i, vorrq_u16(vorrq_u16(vshlq_n_u16(r, 11), vshlq_n_u16(g, 5)), b));
    }
#elif WK_VEC
    /* the same sums in 16 bit lanes (at most 63 * 32 + 31) */
    const vu16 k32 = splat16(32), m63 = splat16(63), m31 = splat16(31);
    for (; i + 8 <= n; i += 8) {
        const vu16 sv = vld<vu16>(src + i), dv = vld<vu16>(dst + i);
        const vu16 wv = widen8(w + i), iw = k32 - wv, t = widen8(dither + i);
        const vu16 r = ((sv >> 11) * wv + (dv >> 11) * iw + t) >> 5;
        const vu16 g = (((sv >> 5) & m63) * wv + ((dv >> 5) & m63) * iw + t) >> 5;
        const vu16 b = ((sv & m31) * wv + (dv & m31) * iw + t) >> 5;
        vst(dst + i, (vu16)((r << 11) | (g << 5) | b));
    }
#endif
    if (i < n)
        wk_blend_row_scalar(dst + i, src + i, w + i, dither + i, n - i);
}

void wk_soft_row_scalar(uint16_t *dst, int n, int e0, int e1, int e2, const uint16_t *f, const uint8_t *th)
{
    for (int d = 0; d < n; d++) {
        const int fd = f[d], t = th[d];
        int r = ((e0 * fd) >> 8) + (t >> 1);
        int g = ((e1 * fd) >> 8) + (t >> 2);
        int bl = ((e2 * fd) >> 8) + (t >> 1);
        r = r > 255 ? 255 : r;
        g = g > 255 ? 255 : g;
        bl = bl > 255 ? 255 : bl;
        dst[d] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (bl >> 3));
    }
}

void wk_soft_row(uint16_t *dst, int n, int e0, int e1, int e2, const uint16_t *f, const uint8_t *th)
{
    int d = 0;
#if WK_VEC
    if (e0 >= 0 && e0 <= 255 && e1 >= 0 && e1 <= 255 && e2 >= 0 && e2 <= 255) {   /* e * f fits 16 bits (f <= 256) */
        const vu16 E0 = splat16((unsigned)e0), E1 = splat16((unsigned)e1), E2 = splat16((unsigned)e2), k255 = splat16(255);
        for (; d + 8 <= n; d += 8) {
            const vu16 fv = vld<vu16>(f + d), t = widen8(th + d);
            vu16 r = ((E0 * fv) >> 8) + (t >> 1);
            vu16 g = ((E1 * fv) >> 8) + (t >> 2);
            vu16 b = ((E2 * fv) >> 8) + (t >> 1);
            /* the sums are below 32768: a signed compare (what SSE2 has) is the unsigned one */
            const vu16 mr = (vu16)((vs16)r > (vs16)k255), mg = (vu16)((vs16)g > (vs16)k255), mb = (vu16)((vs16)b > (vs16)k255);
            r = (r & ~mr) | (k255 & mr);
            g = (g & ~mg) | (k255 & mg);
            b = (b & ~mb) | (k255 & mb);
            vst(dst + d, (vu16)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)));
        }
    }
#endif
    if (d < n)
        wk_soft_row_scalar(dst + d, n - d, e0, e1, e2, f + d, th + d);
}

/* dst[i] = src[i] where op[i] != 0 */
void wk_copy_masked(uint16_t *dst, const uint16_t *src, const uint8_t *op, int n)
{
    int i = 0;
#if WK_VEC
    for (; i + 8 <= n; i += 8) {
        const vu16 keep = (vu16)(widen8(op + i) != splat16(0));
        const vu16 sv = vld<vu16>(src + i), dv = vld<vu16>(dst + i);
        vst(dst + i, (vu16)((sv & keep) | (dv & ~keep)));
    }
#endif
    for (; i < n; i++)
        if (op[i])
            dst[i] = src[i];
}

void wk_fill_masked8(uint8_t *dst, const uint8_t *op, uint8_t v, int n)
{
    if (n <= 0)
        return;
    if (!op) {
        memset(dst, v, (size_t)n);
        return;
    }
    int i = 0;
#if WK_VEC
    const vu8 vv = (vu8){v, v, v, v, v, v, v, v, v, v, v, v, v, v, v, v}, z = (vu8){0};
    for (; i + 16 <= n; i += 16) {
        const vu8 keep = (vu8)(vld<vu8>(op + i) != z);
        const vu8 dv = vld<vu8>(dst + i);
        vst(dst + i, (vu8)((vv & keep) | (dv & ~keep)));
    }
#endif
    for (; i < n; i++)
        if (op[i])
            dst[i] = v;
}

void wk_or_nz8(uint8_t *dst, const uint8_t *src, int n)
{
    int i = 0;
#if WK_VEC
    const vu8 z = (vu8){0}, one = (vu8){1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
    for (; i + 16 <= n; i += 16)
        vst(dst + i, (vu8)(vld<vu8>(dst + i) | ((vu8)(vld<vu8>(src + i) != z) & one)));
#endif
    for (; i < n; i++)
        dst[i] |= (uint8_t)(src[i] != 0);
}

void wk_nz8(uint8_t *dst, const uint8_t *src, int n)
{
    int i = 0;
#if WK_VEC
    const vu8 z = (vu8){0}, one = (vu8){1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
    for (; i + 16 <= n; i += 16)
        vst(dst + i, (vu8)((vu8)(vld<vu8>(src + i) != z) & one));
#endif
    for (; i < n; i++)
        dst[i] = (uint8_t)(src[i] != 0);
}

void wk_rev16(uint16_t *dst, const uint16_t *src, int n)
{
    int i = 0;
#if WK_VEC
    for (; i + 8 <= n; i += 8)
        vst(dst + i, rev16v(vld<vu16>(src + n - 8 - i)));
#endif
    for (; i < n; i++)
        dst[i] = src[n - 1 - i];
}

void wk_rev8(uint8_t *dst, const uint8_t *src, int n)
{
    int i = 0;
#if WK_VEC
    for (; i + 16 <= n; i += 16)
        vst(dst + i, rev8v(vld<vu8>(src + n - 16 - i)));
#endif
    for (; i < n; i++)
        dst[i] = src[n - 1 - i];
}

void wk_rev16_inplace(uint16_t *p, int n)
{
    int a = 0, b = n;
#if WK_VEC
    for (; b - a >= 16; a += 8, b -= 8) {
        const vu16 lo = vld<vu16>(p + a), hi = vld<vu16>(p + b - 8);
        vst(p + a, rev16v(hi));
        vst(p + b - 8, rev16v(lo));
    }
#endif
    for (b--; a < b; a++, b--) {
        const uint16_t t = p[a];
        p[a] = p[b];
        p[b] = t;
    }
}

void wk_rev8_inplace(uint8_t *p, int n)
{
    int a = 0, b = n;
#if WK_VEC
    for (; b - a >= 32; a += 16, b -= 16) {
        const vu8 lo = vld<vu8>(p + a), hi = vld<vu8>(p + b - 16);
        vst(p + a, rev8v(hi));
        vst(p + b - 16, rev8v(lo));
    }
#endif
    for (b--; a < b; a++, b--) {
        const uint8_t t = p[a];
        p[a] = p[b];
        p[b] = t;
    }
}

void wk_mix565_row_scalar(uint16_t *dst, const uint16_t *amb, int w, int n)
{
    for (int i = 0; i < n; i++) {
        const unsigned a = dst[i], b = amb[i];
        const int ar = (int)(a >> 11), ag = (int)((a >> 5) & 63u), ab = (int)(a & 31u);
        const int br = (int)(b >> 11), bg = (int)((b >> 5) & 63u), bb = (int)(b & 31u);
        dst[i] = (uint16_t)(((ar + (br - ar) * w / 32) << 11) | ((ag + (bg - ag) * w / 32) << 5) | (ab + (bb - ab) * w / 32));
    }
}

void wk_mix565_row(uint16_t *dst, const uint16_t *amb, int w, int n)
{
    int i = 0;
#if WK_VEC
    if (w >= 0 && w <= 32) {
        const vs16 wv = (vs16)splat16((unsigned)w), k31 = (vs16)splat16(31);
        const vu16 m63 = splat16(63), m31 = splat16(31);
        for (; i + 8 <= n; i += 8) {
            const vu16 a = vld<vu16>(dst + i), b = vld<vu16>(amb + i);
            const vs16 ar = (vs16)(a >> 11), ag = (vs16)((a >> 5) & m63), ab = (vs16)(a & m31);
            const vs16 br = (vs16)(b >> 11), bg = (vs16)((b >> 5) & m63), bb = (vs16)(b & m31);
            /* t / 32 towards zero: (t + (t < 0 ? 31 : 0)) >> 5 (arithmetic) */
            vs16 tr = (br - ar) * wv, tg = (bg - ag) * wv, tb = (bb - ab) * wv;
            tr = (tr + ((tr >> 15) & k31)) >> 5;
            tg = (tg + ((tg >> 15) & k31)) >> 5;
            tb = (tb + ((tb >> 15) & k31)) >> 5;
            vst(dst + i, (vu16)(((vu16)(ar + tr) << 11) | ((vu16)(ag + tg) << 5) | (vu16)(ab + tb)));
        }
    }
#endif
    if (i < n)
        wk_mix565_row_scalar(dst + i, amb + i, w, n - i);
}

/* ----- state -------------------------------------------------------------------- */

namespace {
const uint8_t kBayer4[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
const uint8_t kBayer8[8][8] = {
    {0, 32, 8, 40, 2, 34, 10, 42},   {48, 16, 56, 24, 50, 18, 58, 26},
    {12, 44, 4, 36, 14, 46, 6, 38},  {60, 28, 52, 20, 62, 30, 54, 22},
    {3, 35, 11, 43, 1, 33, 9, 41},   {51, 19, 59, 27, 49, 17, 57, 25},
    {15, 47, 7, 39, 13, 45, 5, 37},  {63, 31, 55, 23, 61, 29, 53, 21}};
}  // namespace

typedef struct per_t {
    int32_t lo, hi;        /* world pixel columns of the last prediction     */
    uint16_t proto[16];    /* predicted pixel per (world x mod P)            */
    uint8_t P, valid, ptail, stb;
} per_t;

/* A whole soft-edge row (distance 0 .. M - 1) for one edge colour, side and row phase: a pure function of those, so a row the
 * previous lines (or frames) already built is reused. Direct mapped. */
struct softrow_t {
    int16_t e[3];
    uint8_t ph, ok;
    uint16_t px[WK_MAX_MARGIN];
};
#define SOFTROWS 512      /* a sky gradient has a colour per line and side; 128 thrashed (a pure cache: no output depends on its size) */

/* what one caller of wk_compose_lines works in (rows being built, the soft row cache) */
struct wk_compose_scratch {
    int M;
    uint16_t *row, *srow;
    uint8_t *prow, *wrow;
    uint8_t *mat;      /* per pixel of the row being built: 1 = row[] holds the final value so far */
    uint16_t *seq;     /* inner strip + real run + tail */
    uint8_t *seqo;
    softrow_t softc[SOFTROWS];
};

struct wk_compose {
    wk_compose_cfg cfg;
    int W, H, M;
    uint8_t *ext;      /* [layer][side][H] */
    uint8_t *stable;
    uint8_t *lvl_prev; /* [side][H] */
    uint8_t *gh_ext, *gh_t;
    uint8_t *prev_stale; /* [side][H] 1: the line's row of the last frame was not drawn (side_skip): prev holds an older one */
    uint16_t *prev;    /* [side][H][M] last final margin rows */
    int16_t *edge;     /* [side][H][3] blurred edge colour */
    uint8_t smooth[65];
    uint8_t ramp[65][65];                /* [F][k]: weight k px from the outer end of a ramp of F px */
    uint16_t softf[WK_MAX_MARGIN + 8];   /* soft edge darkening factor per distance (256 = none) */
    /* per phase (y & 3, y & 7) and side (s) and distance d: the Bayer thresholds the rows use */
    uint8_t th4[4][2][WK_MAX_MARGIN];    /* kBayer4 value (0..15)                              */
    uint8_t dth4[4][2][WK_MAX_MARGIN];   /* 2 * kBayer4: the blend dither                      */
    uint8_t th8[8][2][WK_MAX_MARGIN];    /* kBayer8 value (stipple threshold)                  */
    uint8_t zero[WK_MAX_MARGIN];         /* all-zero dither (stipple)                          */
    int lut5[32], lut6[64];              /* 5 and 6 bit channel to 0..255 (edge colours)       */
    uint32_t *recip;                     /* precomputed ceil(2^32 / wsum) for edge_colours */
    int recip_n;
    wk_compose_scratch *own;             /* the scratch of wk_compose_frame and of the per frame part */
    int primed;
    /* periodic continuation */
    per_t *per;          /* [layer][side][H] */
    uint8_t *per_el;     /* [H] eligibility scratch */
    uint16_t per_ban[WK_MAX_LAYERS];
    uint8_t per_strikes[WK_MAX_LAYERS];
    uint32_t last_epoch;
    int have_epoch;
    int bans;
};

#define EXT(c, l, s, y) ((c)->ext[(((l) * 2 + (s)) * (c)->H) + (y)])
#define PER(c, l, s, y) ((c)->per[(((l) * 2 + (s)) * (c)->H) + (y)])
#define STB(c, l, s, y) ((c)->stable[(((l) * 2 + (s)) * (c)->H) + (y)])

void wk_compose_cfg_default(wk_compose_cfg *c)
{
    c->fade_px = 20;
    c->fade_mode = WK_FADE_ALPHA;
    c->appear_frames = 8;
    c->grow_px = 2;
    c->ghost_frames = 3;
    c->ghost_min_px = 12;
    c->smear_px = 0;
    c->smear_alpha = 14;
    c->soft_blur = 16;
    c->soft_sample = 4;
    c->soft_darken = 30;
    c->bg_default = 0;
    c->period = 1;
    c->period_ban_frames = 300;
    c->soft_always = 0;
    c->reveal_prov = 0;
    c->fg_fade_px = 0;
}

wk_compose *wk_compose_create(const wk_compose_cfg *cfg, int view_w, int view_h, int margin)
{
    if (view_w < 8 || view_h < 1 || view_h > WK_MAX_LINES || margin < 0 || margin > WK_MAX_MARGIN)
        return NULL;
    wk_compose *c = (wk_compose *)calloc(1, sizeof *c);
    if (!c)
        return NULL;
    if (cfg)
        c->cfg = *cfg;
    else
        wk_compose_cfg_default(&c->cfg);
    c->W = view_w;
    c->H = view_h;
    c->M = margin;
    size_t HL = (size_t)view_h;
    c->ext = (uint8_t *)calloc(WK_MAX_LAYERS * 2 * HL, 1);
    c->stable = (uint8_t *)calloc(WK_MAX_LAYERS * 2 * HL, 1);
    c->lvl_prev = (uint8_t *)calloc(2 * HL, 1);
    c->gh_ext = (uint8_t *)calloc(2 * HL, 1);
    c->gh_t = (uint8_t *)calloc(2 * HL, 1);
    c->prev_stale = (uint8_t *)calloc(2 * HL, 1);
    c->prev = (uint16_t *)calloc(2 * HL * (size_t)(margin ? margin : 1), 2);
    c->edge = (int16_t *)calloc(2 * HL * 3, 2);
    c->per = (per_t *)calloc(WK_MAX_LAYERS * 2 * HL, sizeof(per_t));
    c->per_el = (uint8_t *)calloc(HL, 1);
    c->own = wk_compose_scratch_create(c);
    if (!c->per || !c->per_el || !c->own) {
        wk_compose_destroy(c);
        return NULL;
    }
    if (!c->ext || !c->stable || !c->lvl_prev || !c->gh_ext || !c->gh_t || !c->prev ||
        !c->edge || !c->prev_stale) {
        wk_compose_destroy(c);
        return NULL;
    }
    {
        const int dk = wk_clamp(c->cfg.soft_darken, 0, 100);
        for (int d = 0; d < margin; d++) {
            int t = d * 256 / margin;
            int ease = (t * t) >> 8;
            c->softf[d] = (uint16_t)(256 - (dk * 256 / 100) * ease / 256);
        }
    }
    for (int ph = 0; ph < 4; ph++)
        for (int sd = 0; sd < 2; sd++)
            for (int d = 0; d < margin; d++) {
                const int xs = sd == 0 ? margin - 1 - d : margin + view_w + d;
                c->th4[ph][sd][d] = kBayer4[ph][xs & 3];
                c->dth4[ph][sd][d] = (uint8_t)(kBayer4[ph][xs & 3] * 2);
            }
    for (int ph = 0; ph < 8; ph++)
        for (int sd = 0; sd < 2; sd++)
            for (int d = 0; d < margin; d++) {
                const int xs = sd == 0 ? margin - 1 - d : margin + view_w + d;
                c->th8[ph][sd][d] = kBayer8[ph][xs & 7];
            }
    for (int v = 0; v < 32; v++) c->lut5[v] = v * 255 / 31;
    for (int v = 0; v < 64; v++) c->lut6[v] = v * 255 / 63;
    for (int i = 0; i <= 64; i++) {
        double t = i / 64.0;
        c->smooth[i] = (uint8_t)(32.0 * (3 * t * t - 2 * t * t * t) + 0.5);
    }
    for (int F = 1; F <= 64; F++)
        for (int k = 0; k <= F; k++)
            c->ramp[F][k] = c->smooth[k * 64 / F];
    if (c->cfg.soft_blur > 0) {
        const int max_w = c->cfg.soft_blur >= 63 ? 4096 : (c->cfg.soft_blur + 1) * (c->cfg.soft_blur + 1);
        c->recip = (uint32_t *)malloc((size_t)(max_w + 1) * sizeof(uint32_t));
        if (!c->recip) {
            wk_compose_destroy(c);
            return NULL;
        }
        c->recip_n = max_w;
        c->recip[0] = 0;
        c->recip[1] = 0;
        for (int d = 2; d <= max_w; d++)
            c->recip[d] = (uint32_t)(((1ULL << 32) + (uint64_t)d - 1u) / (uint64_t)d);
    }
    return c;
}

void wk_compose_destroy(wk_compose *c)
{
    if (!c)
        return;
    free(c->ext); free(c->stable); free(c->lvl_prev); free(c->gh_ext); free(c->gh_t);
    free(c->per); free(c->per_el);
    free(c->prev); free(c->edge); free(c->prev_stale);
    free(c->recip);
    wk_compose_scratch_destroy(c->own);
    free(c);
}

wk_compose_scratch *wk_compose_scratch_create(const wk_compose *c)
{
    wk_compose_scratch *t = (wk_compose_scratch *)calloc(1, sizeof *t);
    if (!t)
        return NULL;
    const size_t m = (size_t)(c->M ? c->M : 1);
    t->M = c->M;
    t->row = (uint16_t *)calloc(m + 16, 2);
    t->srow = (uint16_t *)calloc(m + 16, 2);
    t->prow = (uint8_t *)calloc(m + 16, 1);
    t->wrow = (uint8_t *)calloc(m + 16, 1);
    t->mat = (uint8_t *)calloc(m + 16, 1);
    t->seq = (uint16_t *)calloc((size_t)WK_MAX_INNER + m + 40, 2);
    t->seqo = (uint8_t *)calloc((size_t)WK_MAX_INNER + m + 40, 1);
    if (!t->row || !t->srow || !t->prow || !t->wrow || !t->mat || !t->seq || !t->seqo) {
        wk_compose_scratch_destroy(t);
        return NULL;
    }
    return t;
}

void wk_compose_scratch_destroy(wk_compose_scratch *t)
{
    if (!t)
        return;
    free(t->row); free(t->srow); free(t->prow); free(t->wrow); free(t->mat); free(t->seq); free(t->seqo);
    free(t);
}

void wk_compose_reset(wk_compose *c)
{
    size_t HL = (size_t)c->H;
    memset(c->ext, 0, WK_MAX_LAYERS * 2 * HL);
    memset(c->stable, 0, WK_MAX_LAYERS * 2 * HL);
    memset(c->lvl_prev, 0, 2 * HL);
    memset(c->gh_ext, 0, 2 * HL);
    memset(c->gh_t, 0, 2 * HL);
    memset(c->per, 0, WK_MAX_LAYERS * 2 * HL * sizeof(per_t));
    memset(c->per_ban, 0, sizeof c->per_ban);
    memset(c->per_strikes, 0, sizeof c->per_strikes);
    c->have_epoch = 0;
    c->primed = 0;
}

int wk_compose_period_bans(const wk_compose *c) { return c ? c->bans : 0; }

/* ----- soft edge ---------------------------------------------------------------- */

static void edge_colours(wk_compose *c, const wk_compose_in *in)
{
    static const uint32_t kRecipNS[9] = { 0u, 65536u, 32768u, 21846u, 16384u, 13108u, 10923u, 9363u, 8192u };
    const int H = c->H, W = c->W, ns = wk_clamp(c->cfg.soft_sample, 1, 8);
    const uint32_t ns_rec = kRecipNS[ns];
    int16_t raw[2][WK_MAX_LINES][3];
    for (int y = 0; y < H; y++) {
        const uint16_t *line = in->centre + (size_t)y * (size_t)in->centre_stride;
        for (int s = 0; s < 2; s++) {
            int r = 0, g = 0, b = 0;
            const uint16_t *ep = in->edge_px[s] ? in->edge_px[s] + (size_t)y * WK_EDGE_N : NULL;
            for (int i = 0; i < ns; i++) {
                unsigned p = ep ? ep[i] : s == 0 ? line[i] : line[W - 1 - i];
                r += c->lut5[(p >> 11) & 31u];
                g += c->lut6[(p >> 5) & 63u];
                b += c->lut5[p & 31u];
            }
            raw[s][y][0] = (int16_t)(((uint32_t)r * ns_rec) >> 16);
            raw[s][y][1] = (int16_t)(((uint32_t)g * ns_rec) >> 16);
            raw[s][y][2] = (int16_t)(((uint32_t)b * ns_rec) >> 16);
        }
    }
    /* Triangular blur over +-R lines inside the run of lines with the same hud flag.
     * O(H): with S = prefix sums of v, T = prefix sums of j*v the weighted window sum
     *   sum_j (R+1-|j-y|) v[j] over lo..hi  =  (R+1)*S(lo..hi)
     *        - ( y*S(lo..y) - T(lo..y) ) - ( T(y+1..hi) - y*S(y+1..hi) ). */
    const int R = c->cfg.soft_blur;
    if (R <= 0) {   /* no blur (the caller's colours are already smooth): the edge colour is the raw colour */
        for (int s = 0; s < 2; s++)
            for (int y = 0; y < H; y++)
                for (int ch = 0; ch < 3; ch++)
                    c->edge[((size_t)s * (size_t)H + (size_t)y) * 3 + (size_t)ch] = raw[s][y][ch];
        return;
    }
    int32_t S[4][WK_MAX_LINES + 1], T[4][WK_MAX_LINES + 1];   /* ch 3 = count */
    int seg_lo[WK_MAX_LINES], seg_hi[WK_MAX_LINES];
    for (int y = 0; y < H; y++) {
        seg_lo[y] = (y > 0 && in->ana->line[y - 1].hud == in->ana->line[y].hud) ? seg_lo[y - 1] : y;
    }
    for (int y = H - 1; y >= 0; y--) {
        seg_hi[y] = (y < H - 1 && in->ana->line[y + 1].hud == in->ana->line[y].hud) ? seg_hi[y + 1] : y;
    }
    const uint32_t *const recip = c->recip;
    const int recip_n = c->recip_n;
    for (int s = 0; s < 2; s++) {
        for (int ch = 0; ch < 4; ch++) {
            S[ch][0] = 0;
            T[ch][0] = 0;
            for (int y = 0; y < H; y++) {
                int32_t v = ch < 3 ? raw[s][y][ch] : 1;
                S[ch][y + 1] = S[ch][y] + v;
                T[ch][y + 1] = T[ch][y] + (int32_t)y * v;
            }
        }
        for (int y = 0; y < H; y++) {
            const int lo = wk_max(seg_lo[y], y - R), hi = wk_min(seg_hi[y], y + R);
            int32_t acc[4];
            for (int ch = 0; ch < 4; ch++) {
                const int32_t s_lo_y = S[ch][y + 1] - S[ch][lo], t_lo_y = T[ch][y + 1] - T[ch][lo];
                const int32_t s_y_hi = S[ch][hi + 1] - S[ch][y + 1], t_y_hi = T[ch][hi + 1] - T[ch][y + 1];
                acc[ch] = (R + 1) * (s_lo_y + s_y_hi) - (y * s_lo_y - t_lo_y) - (t_y_hi - y * s_y_hi);
            }
            const int wsum = acc[3] ? acc[3] : 1;
            int16_t *dst_e = &c->edge[((size_t)s * (size_t)H + (size_t)y) * 3];
            if (wsum >= 2 && wsum <= recip_n) {
                const uint32_t rec = recip[wsum];
                for (int ch = 0; ch < 3; ch++)
                    dst_e[ch] = (int16_t)(uint32_t)(((uint64_t)(uint32_t)acc[ch] * rec) >> 32);
            } else {
                for (int ch = 0; ch < 3; ch++)
                    dst_e[ch] = (int16_t)(acc[ch] / wsum);
            }
        }
    }
}

/* The base of a margin pixel: the soft edge (a pure function of the edge colour, the distance and the
 * dither) or the flat backdrop colour.  row[] holds the final value of a pixel once mat[] is set; until
 * then it is the base, which is computed only where something reads it (a blend) or nothing covers it. */
struct base_t {
    int soft;               /* 1: soft edge, 0: flat colour */
    int s, y;
    const int16_t *e;       /* edge colour of this line and side (soft only) */
    uint16_t bgc;
};

/* row[a + i] := base for i < n unless already final (mat); a pixel a blend covers fully (wgt[i] == 32) needs no
 * base (the blend result does not read it), it only becomes final.  The soft edge is evaluated in one loop per span
 * with the line's edge colour in locals. */
static void base_fill(const wk_compose *c, wk_compose_scratch *t, const base_t &b, uint16_t *row, uint8_t *mat, int a, int n, const uint8_t *wgt)
{
    if (!b.soft) {
        for (int i = 0; i < n; i++) {
            const int d = a + i;
            if (!mat[d] && !(wgt && wgt[i] == 32))
                row[d] = b.bgc;
            mat[d] = 1;
        }
        return;
    }
    const int e0 = b.e[0], e1 = b.e[1], e2 = b.e[2];
    if (n >= 8) {
        /* whole row from the cache (computed for every distance on a miss, 8 pixels a step) */
        const uint8_t ph = (uint8_t)((b.y & 3) | (b.s << 2));
        softrow_t *sr = &t->softc[(((unsigned)e0 * 0x9E3779B1u) ^ ((unsigned)e1 * 0x85EBCA6Bu) ^ ((unsigned)e2 * 0xC2B2AE35u) ^ ((unsigned)ph * 0x27D4EB2Fu)) >> 23];
        if (!sr->ok || sr->ph != ph || sr->e[0] != e0 || sr->e[1] != e1 || sr->e[2] != e2) {
            wk_soft_row(sr->px, c->M, e0, e1, e2, c->softf, c->th4[b.y & 3][b.s]);
            sr->e[0] = (int16_t)e0; sr->e[1] = (int16_t)e1; sr->e[2] = (int16_t)e2; sr->ph = ph; sr->ok = 1;
        }
        const uint16_t *px = sr->px;
        if (!wgt) {
            /* the usual call (what nothing covered): runs of untouched pixels are copied whole */
            int i = 0;
            while (i < n) {
                if (mat[a + i]) { i++; continue; }
                const uint8_t *e = (const uint8_t *)memchr(mat + a + i, 1, (size_t)(n - i));
                const int j = e ? (int)(e - (mat + a)) : n;
                memcpy(row + a + i, px + a + i, (size_t)(j - i) * sizeof(uint16_t));
                memset(mat + a + i, 1, (size_t)(j - i));
                i = j;
            }
            return;
        }
        for (int i = 0; i < n; i++) {
            const int d = a + i;
            if (mat[d])
                continue;
            mat[d] = 1;
            if (wgt && wgt[i] == 32)
                continue;
            row[d] = px[d];
        }
        return;
    }
    const uint16_t *sf = c->softf;
    const uint8_t *th4 = c->th4[b.y & 3][b.s];
    for (int i = 0; i < n; i++) {
        const int d = a + i;
        if (mat[d])
            continue;
        mat[d] = 1;
        if (wgt && wgt[i] == 32)
            continue;
        const int f = sf[d], th = th4[d];
        int r = ((e0 * f) >> 8) + (th >> 1);
        int g = ((e1 * f) >> 8) + (th >> 2);
        int bl = ((e2 * f) >> 8) + (th >> 1);
        r = r > 255 ? 255 : r;
        g = g > 255 ? 255 : g;
        bl = bl > 255 ? 255 : bl;
        row[d] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (bl >> 3));
    }
}

/* ----- periodic continuation ------------------------------------------------------ */

/* seq = inner strip (far .. near the edge) followed by the real margin run */
static void build_seq(const wk_compose *c, wk_compose_scratch *t, const wk_compose_in *in, int l, int s, int y, int ext)
{
    const int IN = in->inner_n, M = c->M;
    uint16_t *const seq = t->seq;
    uint8_t *const seqo = t->seqo;
    const uint16_t *inn = in->inner[l][s] + (size_t)y * (size_t)IN;
    const uint8_t *ino = in->inner_opaque[l][s] ? in->inner_opaque[l][s] + (size_t)y * (size_t)IN : NULL;
    for (int k = 0; k < IN; k++) {
        seq[k] = inn[IN - 1 - k];
        seqo[k] = ino ? (uint8_t)(ino[IN - 1 - k] != 0) : 1;
    }
    const uint16_t *cand = in->cand[l][s] ? in->cand[l][s] + (size_t)y * (size_t)M : NULL;
    const uint8_t *op = in->opaque[l][s] ? in->opaque[l][s] + (size_t)y * (size_t)M : NULL;
    for (int d = 0; d < ext; d++) {
        seq[IN + d] = cand ? cand[d] : 0;
        seqo[IN + d] = (cand && (!op || op[d])) ? 1 : 0;
    }
}

/* smallest period (8 or 16) the last three periods of the run (inner strip, then the
 * real margin pixels, outward) repeat exactly in, all opaque; 0 = none.  Read in place
 * with early exit: almost every line fails at its first comparison. */
static int detect_period(const wk_compose *c, const wk_compose_in *in, int l, int s, int y, int ext)
{
    const int IN = in->inner_n, M = c->M;
    const uint16_t *inn = in->inner[l][s] + (size_t)y * (size_t)IN;
    const uint8_t *ino = in->inner_opaque[l][s] ? in->inner_opaque[l][s] + (size_t)y * (size_t)IN : NULL;
    const uint16_t *cand = in->cand[l][s] + (size_t)y * (size_t)M;
    const uint8_t *op = in->opaque[l][s] ? in->opaque[l][s] + (size_t)y * (size_t)M : NULL;
    const int len = IN + ext;
#define WK_PX(t) ((t) < IN ? inn[IN - 1 - (t)] : cand[(t) - IN])
#define WK_OP(t) ((t) < IN ? (ino ? ino[IN - 1 - (t)] != 0 : 1) : (op ? op[(t) - IN] != 0 : 1))
    for (int P = 8; P <= 16; P += 8) {
        if (len < 3 * P)
            continue;
        int ok = 1;
        for (int t = len - 1; t >= len - 2 * P && ok; t--)
            if (!WK_OP(t) || !WK_OP(t - P) || WK_PX(t) != WK_PX(t - P))
                ok = 0;
        if (ok)
            return P;
    }
#undef WK_PX
#undef WK_OP
    return 0;
}

static int32_t per_wx(const wk_compose *c, const wk_out *ana, int l, int s, int y, int d)
{
    return s == 1 ? ana->world_x[l][y] + c->W + d : ana->world_x[l][y] - 1 - d;
}

/* hindsight + eligibility + hysteresis of the tail for one level layer */
static void period_layer(wk_compose *c, const wk_compose_in *in, int l)
{
    const wk_out *ana = in->ana;
    const int H = c->H, M = c->M, IN = in->inner_n;
    const wk_compose_cfg &cfg = c->cfg;
    int bad = 0;
    for (int s = 0; s < 2 && !bad; s++) {
        const uint16_t *cb = in->cand[l][s];
        const uint8_t *ob = in->opaque[l][s];
        if (!cb)
            continue;
        for (int y = 0; y < H && !bad; y++) {
            per_t &pl = PER(c, l, s, y);
            if (!pl.valid)
                continue;
            if (!ana->coherent[l][y]) {
                pl.valid = 0;
                continue;
            }
            const int real = wk_min(ana->line[y].real[s][l], M);
            const int Pp = pl.P;
            const int ppow2 = wk_is_pow2(Pp);
            const uint32_t pmask = (uint32_t)(Pp - 1);
            int32_t wx = per_wx(c, ana, l, s, y, 0);
            const int32_t wx_step = s == 1 ? 1 : -1;
            for (int d = 0; d < real; d++, wx += wx_step) {
                if (wx < pl.lo || wx > pl.hi)
                    continue;
                const size_t i = (size_t)y * (size_t)M + (size_t)d;
                const int pidx = ppow2 ? (int)((uint32_t)wx & pmask) : wk_floormod(wx, Pp);
                if ((ob && !ob[i]) || cb[i] != pl.proto[pidx]) {
                    bad = 1;
                    break;
                }
            }
        }
    }
    if (bad) {
        /* every repeated miss doubles-ish the ban: this engine is not periodic */
        c->per_ban[l] = (uint16_t)wk_clamp(cfg.period_ban_frames * (1 + c->per_strikes[l]), 0, 65535);
        if (c->per_strikes[l] < 10)
            c->per_strikes[l]++;
        c->bans++;
        for (int i = 0; i < 2 * H; i++)
            memset(&c->per[(size_t)l * 2 * (size_t)H + (size_t)i], 0, sizeof(per_t));
        return;
    }
    const int banned = c->per_ban[l] > 0;
    if (banned)
        c->per_ban[l]--;
    for (int s = 0; s < 2; s++) {
        /* eligibility, bottom up: terrain that stands on the floor of the view */
        for (int y = H - 1; y >= 0; y--) {
            const wk_line_out &lo = ana->line[y];
            c->per_el[y] = 0;
            per_t &pl = PER(c, l, s, y);
            const int ext = EXT(c, l, s, y);
            int P = 0;
            if (!banned && !lo.hud && ((lo.level_mask >> l) & 1) && ana->coherent[l][y] &&
                ext < M && in->inner[l][s] && in->cand[l][s] && IN >= 24) {
                P = detect_period(c, in, l, s, y, ext);
            }
            const int below = (y == H - 1) || ana->line[y + 1].hud || c->per_el[y + 1];
            if (P && below) {
                c->per_el[y] = 1;
                pl.P = (uint8_t)P;
            }
            const int want = c->per_el[y] ? M - ext : 0;
            if (!want) {
                pl.ptail = 0;
                pl.stb = 0;
                pl.valid = 0;
                continue;
            }
            if (pl.ptail > want) {
                pl.ptail = (uint8_t)want;                 /* the real run grew */
            } else if (want > pl.ptail) {
                if (pl.stb < 255)
                    pl.stb++;
                if (pl.stb >= cfg.appear_frames)
                    pl.ptail = (uint8_t)wk_min(want, pl.ptail + cfg.grow_px);
            }
            if (pl.ptail == 0) {
                pl.valid = 0;
                continue;
            }
            /* remember what is predicted, keyed by world column */
            build_seq(c, c->own, in, l, s, y, ext);
            const int Pp = pl.P;
            const int ppow2 = wk_is_pow2(Pp);
            const uint32_t pmask = (uint32_t)(Pp - 1);
            int32_t wx = per_wx(c, ana, l, s, y, ext);
            const int32_t wx_step = s == 1 ? 1 : -1;
            for (int d = ext; d < ext + pl.ptail; d++, wx += wx_step) {
                c->own->seq[IN + d] = c->own->seq[IN + d - Pp];
                c->own->seqo[IN + d] = 1;
                const int pidx = ppow2 ? (int)((uint32_t)wx & pmask) : wk_floormod(wx, Pp);
                pl.proto[pidx] = c->own->seq[IN + d];
            }
            if (s == 1) {
                pl.lo = per_wx(c, ana, l, s, y, ext);
                pl.hi = per_wx(c, ana, l, s, y, ext + pl.ptail - 1);
            } else {
                pl.lo = per_wx(c, ana, l, s, y, ext + pl.ptail - 1);
                pl.hi = per_wx(c, ana, l, s, y, ext);
            }
            pl.valid = 1;
        }
    }
}

/* ----- the frame ------------------------------------------------------------------ */

void wk_compose_begin(wk_compose *c, const wk_compose_in *in)
{
    const wk_out *ana = in->ana;
    const int H = c->H, M = c->M;
    const wk_compose_cfg &cfg = c->cfg;
    const int nl = in->norder;
    {   /* the soft edge is only needed on lines nothing continues on */
        int need = 0;
        for (int y = 0; y < H && !need; y++)
            need = ana->line[y].hud || ana->line[y].backdrop_mask == 0;
        if (need)
            edge_colours(c, in);
    }

    if (!c->have_epoch || ana->epoch != c->last_epoch || ana->scene_event) {
        c->have_epoch = 1;
        c->last_epoch = ana->epoch;
        memset(c->per, 0, WK_MAX_LAYERS * 2 * (size_t)H * sizeof(per_t));
        memset(c->per_ban, 0, sizeof c->per_ban);
        memset(c->per_strikes, 0, sizeof c->per_strikes);
    }
    /* pass A: hysteresis of every layer's extent */
    if (M > 0)
        for (int y = 0; y < H; y++) {
            const wk_line_out &lo = ana->line[y];
            for (int s = 0; s < 2; s++)
                for (int oi = 0; oi < nl; oi++) {
                    const int l = in->order[oi];
                    const int is_back = (lo.backdrop_mask >> l) & 1;
                    const int is_level = (lo.level_mask >> l) & 1;
                    uint8_t &ext = EXT(c, l, s, y);
                    uint8_t &stb = STB(c, l, s, y);
                    if (lo.hud || !in->cand[l][s] || (!is_back && !is_level)) {
                        ext = 0;
                        stb = 0;
                        continue;
                    }
                    const int target = wk_min(lo.real[s][l], M);
                    if (target < ext) {
                        ext = (uint8_t)target;
                        stb = 0;
                    } else if (target > ext) {
                        if (stb < 255) stb++;
                        if (stb >= cfg.appear_frames)
                            ext = (uint8_t)wk_min(target, ext + cfg.grow_px);
                    } else {
                        stb = 0;
                    }
                }
        }
    /* pass B: periodic continuation of level layers */
    if (M > 0 && cfg.period && in->inner_n >= 24 && in->inner_n <= WK_MAX_INNER)
        for (int oi = 0; oi < nl; oi++)
            period_layer(c, in, in->order[oi]);
    else
        memset(c->per, 0, WK_MAX_LAYERS * 2 * (size_t)H * sizeof(per_t));
}

void wk_compose_end(wk_compose *c) { c->primed = 1; }

void wk_compose_frame(wk_compose *c, const wk_compose_in *in)
{
    wk_compose_begin(c, in);
    wk_compose_lines(c, in, 0, c->H, NULL);
    wk_compose_end(c);
}

void wk_compose_lines(wk_compose *c, const wk_compose_in *in, int y0, int y1, wk_compose_scratch *t)
{
    const wk_out *ana = in->ana;
    const int H = c->H, W = c->W, M = c->M;
    const wk_compose_cfg &cfg = c->cfg;
    const int nl = in->norder;
    if (!t)
        t = c->own;
    if (y0 < 0) y0 = 0;
    if (y1 > H) y1 = H;

    for (int y = y0; y < y1; y++) {
        uint16_t *orow = in->out + (size_t)y * (size_t)in->out_stride;
        uint8_t *oprov = in->prov ? in->prov + (size_t)y * (size_t)in->out_stride : NULL;
        const uint16_t *cptr = in->centre + (size_t)y * (size_t)in->centre_stride;
        if (orow + M != cptr)
            memcpy(orow + M, cptr, (size_t)W * 2);
        if (oprov)
            memset(oprov + M, WK_PROV_CENTRE, (size_t)W);
        const wk_line_out &lo = ana->line[y];
        const uint16_t bgc = in->bg ? in->bg[y] : cfg.bg_default;
        const int stipple = cfg.fade_mode == WK_FADE_STIPPLE;

        for (int s = 0; s < 2; s++) {
            if (M <= 0)
                break;
            if (in->side_skip[s]) {
                /* not shown this frame: only what the next frames' hysteresis reads (the level extent, the ghost timers) */
                int lvl = 0;
                for (int oi = 0; oi < nl; oi++) {
                    const int l = in->order[oi];
                    const int is_back = (lo.backdrop_mask >> l) & 1;
                    const int is_level = (lo.level_mask >> l) & 1;
                    if (lo.hud || !in->cand[l][s] || (!is_back && !is_level))
                        continue;
                    const int e = EXT(c, l, s, y);
                    if (is_level && e > lvl)
                        lvl = e;
                }
                uint8_t &lp = c->lvl_prev[s * H + y];
                uint8_t &ge = c->gh_ext[s * H + y];
                uint8_t &gt = c->gh_t[s * H + y];
                if (cfg.ghost_frames > 0 && c->primed && !ana->scene_event && lp - lvl >= cfg.ghost_min_px) {
                    ge = lp;
                    gt = (uint8_t)cfg.ghost_frames;
                }
                lp = (uint8_t)lvl;
                if (gt > 0 && !ana->scene_event && !lo.hud)
                    gt--;
                else
                    gt = 0;
                c->prev_stale[s * H + y] = 1;
                continue;
            }
            uint16_t *row = t->row;
            uint8_t *prow = t->prow;
            uint8_t *mat = t->mat;
            base_t bs;
            bs.soft = lo.hud || lo.backdrop_mask == 0 || cfg.soft_always;
            bs.s = s;
            bs.y = y;
            bs.e = bs.soft ? &c->edge[((size_t)s * (size_t)H + (size_t)y) * 3] : NULL;
            bs.bgc = bgc;
            const uint8_t *dith4 = c->dth4[y & 3][s];      /* blend dither per distance */
            memset(mat, 0, (size_t)M);
            if (oprov)
                memset(prow, bs.soft ? WK_PROV_SOFT : WK_PROV_BG, (size_t)M);
            int lvl_now = 0;
            for (int oi = 0; oi < nl; oi++) {
                const int l = in->order[oi];
                const uint16_t *cand = in->cand[l][s];
                const int is_back = (lo.backdrop_mask >> l) & 1;
                const int is_level = (lo.level_mask >> l) & 1;
                uint8_t &ext = EXT(c, l, s, y);
                if (lo.hud || !cand || (!is_back && !is_level))
                    continue;
                const int ptail = (is_level && cfg.period) ? PER(c, l, s, y).ptail : 0;
                if (is_level && ext > lvl_now)
                    lvl_now = ext;
                const int tailreq = (cfg.fg_fade_px > 0 && is_level && !ptail && in->tail_px[l][s]) ? wk_min(cfg.fg_fade_px, M) : 0;
                const int use_tail = tailreq > 0 && (int)ext < tailreq;      /* a short real run: the fade replaces its own ramp */
                if (ext + ptail == 0 && !(use_tail && in->tail_op[l][s] && in->tail_op[l][s][y]))
                    continue;
                /* the run to draw: the real part, then the periodic tail */
                const uint16_t *cdat = cand + (size_t)y * (size_t)M;
                const uint8_t *op = in->opaque[l][s] ? in->opaque[l][s] + (size_t)y * (size_t)M : NULL;
                if (ptail > 0) {
                    const int IN = in->inner_n, Pp = PER(c, l, s, y).P;
                    build_seq(c, t, in, l, s, y, ext);
                    for (int d = ext; d < ext + ptail; d++) {
                        t->seq[IN + d] = t->seq[IN + d - Pp];
                        t->seqo[IN + d] = 1;
                    }
                    cdat = t->seq + IN;
                    op = t->seqo + IN;
                }
                /* weights: full weight up to n-F (a plain opaque copy), the ramp over the last F */
                const int n = ext + ptail;
                const int F = (n >= M || use_tail) ? 0 : wk_min(wk_min(cfg.fade_px, 64), n);
                const int smear = (is_level && n < M && cfg.smear_px > 0) ? wk_min(cfg.smear_px, M - n) : 0;
                const int a0 = smear ? wk_clamp(cfg.smear_alpha, 0, 32) : 0;
                uint8_t *w = t->wrow;
                const int nf = n - F;
                if (op) {
                    wk_copy_masked(row, cdat, op, nf);
                    wk_or_nz8(mat, op, nf);
                } else {
                    memcpy(row, cdat, (size_t)nf * 2);
                    memset(mat, 1, (size_t)nf);
                }
                const uint8_t *dith = stipple ? c->zero : dith4;
                for (int d = nf; d < n; d++) {
                    int wv = a0 ? a0 + (c->ramp[F][n - d] * (32 - a0)) / 32 : c->ramp[F][n - d];
                    if (stipple)
                        wv = (wv * 2 > c->th8[y & 7][s][d]) ? 32 : 0;
                    w[d] = (uint8_t)(op && !op[d] ? 0 : wv);
                }
                if (F > 0) {
                    base_fill(c, t, bs, row, mat, nf, F, w + nf);
                    wk_blend_row(row + nf, cdat + nf, w + nf, dith + nf, F);
                }
                if (oprov) {
                    int lim = M;               /* where a higher level layer stops being known */
                    if (cfg.reveal_prov)
                        for (int oj = oi + 1; oj < nl; oj++) {
                            const int lj = in->order[oj];
                            if (lo.hud || !in->cand[lj][s] || !((lo.level_mask >> lj) & 1))
                                continue;
                            const int ej = EXT(c, lj, s, y);       /* the upper layer is fully there only up to its ramp */
                            lim = wk_min(lim, ej >= M ? M : ej - wk_min(wk_min(cfg.fade_px, 64), ej));
                        }
                    /* the full weight part (d < nf): the same value over each of up to three stretches, wherever the layer is opaque */
                    {
                        const uint8_t hi = (uint8_t)(l << 4);
                        const int b1 = wk_clamp(wk_min(lim, (int)ext), 0, nf);      /* [0, b1): the layer's own real pixels */
                        const int b2 = wk_clamp(lim, b1, nf);                       /* [b1, b2): predicted (d >= ext), [b2, nf): revealed */
                        wk_fill_masked8(prow, op, (uint8_t)(hi | (is_back ? WK_PROV_BACKDROP : WK_PROV_LEVEL)), b1);
                        wk_fill_masked8(prow + b1, op ? op + b1 : NULL, (uint8_t)(hi | WK_PROV_PERIOD), b2 - b1);
                        wk_fill_masked8(prow + b2, op ? op + b2 : NULL, (uint8_t)(hi | WK_PROV_REVEAL), nf - b2);
                    }
                    for (int d = nf; d < n; d++) {
                        const int full = w[d] == 32;
                        const int any = w[d] != 0;
                        if (!any)
                            continue;
                        const int pred = d >= ext;
                        prow[d] = (uint8_t)((l << 4) | (d >= lim ? WK_PROV_REVEAL
                                                       : pred ? (full ? WK_PROV_PERIOD : WK_PROV_PFADE)
                                                              : full ? (is_back ? WK_PROV_BACKDROP : WK_PROV_LEVEL)
                                                                     : WK_PROV_FADE));
                    }
                }
                if (use_tail && n < M) {
                    /* the foreground is not known beyond d = n: its last known colour continues and fades out */
                    int have = 0;
                    uint16_t tc = 0;
                    if (n > 0 && (!op || op[n - 1])) { tc = cdat[n - 1]; have = 1; }
                    else if (n == 0 && in->tail_op[l][s] && in->tail_op[l][s][y]) { tc = in->tail_px[l][s][y]; have = 1; }
                    const int ft = wk_min(tailreq - n, M - n);
                    if (have && ft > 0) {
                        uint16_t *sp = t->srow;
                        for (int d = 0; d < ft; d++) {
                            w[d] = c->ramp[wk_min(ft, 64)][wk_min(ft, 64) - (d < 64 ? d : 63)];
                            sp[d] = tc;
                        }
                        base_fill(c, t, bs, row, mat, n, ft, w);
                        wk_blend_row(row + n, sp, w, c->zero + n, ft);
                        if (oprov)
                            memset(prow + n, (uint8_t)((l << 4) | WK_PROV_SMEAR), (size_t)ft);
                    }
                }
                if (smear) {
                    /* flat tail: average of the last real opaque pixels */
                    int cnt = 0, r = 0, g = 0, b = 0;
                    for (int d = wk_max(0, n - 4); d < n; d++) {
                        if (op && !op[d])
                            continue;
                        unsigned p = cdat[d];
                        r += (int)(p >> 11); g += (int)((p >> 5) & 63u); b += (int)(p & 31u);
                        cnt++;
                    }
                    if (cnt >= 2) {
                        uint16_t sc = (uint16_t)(((r / cnt) << 11) | ((g / cnt) << 5) | (b / cnt));
                        uint16_t *sp = t->srow;
                        for (int d = 0; d < smear; d++) {
                            int tt = 32 - d * 32 / smear;          /* 32 .. 1 */
                            w[d] = (uint8_t)(a0 * tt / 32);
                            sp[d] = sc;
                        }
                        base_fill(c, t, bs, row, mat, n, smear, w);
                        wk_blend_row(row + n, sp, w, dith4 + n, smear);
                        if (oprov)
                            for (int d = 0; d < smear; d++)
                                if (w[d] && prow[n + d] != (uint8_t)((l << 4) | WK_PROV_LEVEL))
                                    prow[n + d] = (uint8_t)((l << 4) | WK_PROV_SMEAR);
                    }
                }
            }
            /* ghost of a large shrink */
            uint8_t &lp = c->lvl_prev[s * H + y];
            uint8_t &ge = c->gh_ext[s * H + y];
            uint8_t &gt = c->gh_t[s * H + y];
            if (cfg.ghost_frames > 0 && c->primed && !ana->scene_event && lp - lvl_now >= cfg.ghost_min_px) {
                ge = lp;
                gt = (uint8_t)cfg.ghost_frames;
            }
            lp = (uint8_t)lvl_now;
            if (gt > 0 && !ana->scene_event && !lo.hud) {
                const int lo_d = lvl_now, end_d = wk_min((int)ge, M);
                const int gn = end_d - lo_d;
                if (gn > 0) {
                    const uint16_t *pv = c->prev + ((size_t)s * (size_t)H + (size_t)y) * (size_t)M;
                    const uint8_t wk8 = (uint8_t)(32 * gt / (cfg.ghost_frames + 1));
                    const int stale = c->prev_stale[s * H + y];   /* nothing to fade from: the stretch is marked, not blended */
                    memset(t->wrow, wk8, (size_t)gn);
                    base_fill(c, t, bs, row, mat, lo_d, gn, t->wrow);
                    if (!stale)
                        wk_blend_row(row + lo_d, pv + lo_d, t->wrow, dith4 + lo_d, gn);
                    if (oprov)
                        memset(prow + lo_d, WK_PROV_GHOST, (size_t)gn);
                }
                gt--;
            } else {
                gt = 0;
            }
            base_fill(c, t, bs, row, mat, 0, M, NULL);      /* what nothing covered shows the base */
            memcpy(c->prev + ((size_t)s * (size_t)H + (size_t)y) * (size_t)M, row, (size_t)M * sizeof(uint16_t));
            c->prev_stale[s * H + y] = 0;
            /* write out (left side runs outwards from the centre) */
            if (s == 0 && !in->left_by_distance) {
                wk_rev16(orow, row, M);
                if (oprov)
                    wk_rev8(oprov, prow, M);
            } else {
                uint16_t *const dst = s == 0 ? orow : orow + M + W;
                memcpy(dst, row, (size_t)M * 2);
                if (oprov)
                    memcpy(s == 0 ? oprov : oprov + M + W, prow, (size_t)M);
            }
        }
    }
}
