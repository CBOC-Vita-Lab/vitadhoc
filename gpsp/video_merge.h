/* video_merge.h -- the scanline renderer's colour-effect merges.
 *
 * merge_blend() and merge_brightness() of video.cc, as plain kernels over
 * (palette, coefficients, source line, destination line), so that the renderer
 * and tools/tests/test_video_merge.cc compile the SAME code.  Four
 * implementations of one function, every one bit-for-bit the original
 * per-pixel loop (the test compares all of them with a verbatim copy of it):
 *
 *   vm_*_orig  the original loops (the same text, parameters instead of
 *              register reads).  What a build without a vector unit runs, and
 *              what the vector kernels use for the last (end - start) % 8
 *              pixels.
 *   vm_*_bf    branch-free scalar: both outcomes are computed and one is
 *              selected with a mask.  Always compiled (tested), used with
 *              -DVIDEO_MERGE_SCALAR_BF only: it costs about twice the
 *              instructions of the original on typical lines (the blend is
 *              computed for every pixel), so it only pays where a
 *              misprediction costs more than ~25 instructions.
 *   vm_*_vec   8 pixels per step on ARMv7 NEON (the Vita's Cortex-A9) or x86
 *              SSE2: the palette lookups stay scalar loads (neither has a
 *              gather), the arithmetic is on 16-bit lanes, one colour channel
 *              per vector, and a group of eight pixels none of which is
 *              blended or brightened is stored straight from the lookup.
 *              ONE algorithm over a dozen vector operations (vq_*), written
 *              twice (NEON, SSE2), so the SSE2 build tests the algorithm and
 *              the NEON build tests the instructions.
 * -DVIDEO_MERGE_NO_VEC selects _orig; -DVIDEO_MERGE_ORIG (video.cc) selects the
 * original functions themselves.
 *
 * WHY THE CHANNEL ARITHMETIC IS EXACT.  The pixel is RGB565 (R 15..11,
 * G 10..5, B 4..0).  The original expands a pixel to 32 bits with BLND_MSK
 * (R at 11, G at 21, B at 0) and multiplies the three channels at once; the
 * channels are kept apart by the gaps: a channel times a coefficient of at
 * most 16 (blend: the sum of two, at most 32) never reaches the next channel's
 * lowest bit, and the >> 4 only moves a channel's fraction into the gap below
 * it, where the final & BLND_MSK drops it.  So every channel is computed on
 * its own: floor((c1*a + c2*b) / 16) (blend), floor(c*(16-y)/16) + floor(max*y/16)
 * (brighten), floor(c*(16-y)/16) (darken); the original's saturation (overflow
 * bit set: every bit of the channel to one) is min(value, max).  Brighten's two
 * terms add up to at most the channel maximum, so the sum never carries.  The
 * test checks it over every channel value and every coefficient, and over
 * whole pixels.  The vector kernels are RGB565 only (not USE_XBGR1555_FORMAT)
 * and little-endian.
 */
#ifndef GPSP_VIDEO_MERGE_H
#define GPSP_VIDEO_MERGE_H

/* -DVIDEO_KERNELS_SCALAR (see video.cc): no vector kernels at all. */
#ifdef VIDEO_KERNELS_SCALAR
#ifndef VIDEO_MERGE_NO_VEC
#define VIDEO_MERGE_NO_VEC
#endif
#endif

#ifdef USE_XBGR1555_FORMAT
  #define OVFG_MSK 0x04000000
  #define OVFR_MSK 0x00008000
  #define OVFB_MSK 0x00000020
  #define SATG_MSK 0x03E00000
  #define SATR_MSK 0x00007C00
  #define SATB_MSK 0x0000001F
#else
  #define OVFG_MSK 0x08000000
  #define OVFR_MSK 0x00010000
  #define OVFB_MSK 0x00000020
  #define SATG_MSK 0x07E00000
  #define SATR_MSK 0x0000F800
  #define SATB_MSK 0x0000001F
#endif
#define BLND_MSK (SATR_MSK | SATG_MSK | SATB_MSK)

#ifndef VREGRESS_PERTURB
#define VREGRESS_PERTURB 0
#endif

typedef enum
{
  OBJ_BLEND,    // No effects, just blend forced-blend pixels (ie. ST objects)
  BLEND_ONLY,   // Just alpha blending (if the pixels are 1st and 2nd target)
  BLEND_BRIGHT, // Perform alpha blending if appropiate, and brighten otherwise
  BLEND_DARK,   // Same but with darken effecg
} blendtype;

/* ---- the original loops --------------------------------------------------- */

// Blend `sat`: a + b > 16 (the sum can exceed a channel and is clamped).
template <blendtype bldtype, bool st_objs, bool sat>
static inline void vm_blend_orig(const unsigned short *pal,
  unsigned blend_a, unsigned blend_b, unsigned brightf, unsigned start,
  unsigned end, unsigned short *dst, const unsigned *src)
{
  while (start < end) {
    unsigned pixpair = src[start];
    // If ST-OBJ, force blending mode (has priority over other effects).
    // If regular blending mode, blend if 1st/2nd bits are set respectively.
    // Otherwise, apply other color effects if 1st bit is set.
    bool force_blend = (pixpair & 0x04000800) == 0x04000800;
    bool do_blend    = (pixpair & 0x04000200) == 0x04000200;
    if ((st_objs && force_blend) || (do_blend && bldtype == BLEND_ONLY)) {
      // Top pixel is 1st target, pixel below is 2nd target. Blend!
      unsigned short p1 = pal[(pixpair >>  0) & 0x1FF];
      unsigned short p2 = pal[(pixpair >> 16) & 0x1FF];
      unsigned p1e = (p1 | (p1 << 16)) & BLND_MSK;
      unsigned p2e = (p2 | (p2 << 16)) & BLND_MSK;
      unsigned pfe = (((p1e * blend_a) + (p2e * blend_b)) >> 4);
      if (sat) {
        // If the overflow bit is set, saturate (set) all bits to one.
        if (pfe & (OVFR_MSK | OVFG_MSK | OVFB_MSK)) {
          if (pfe & OVFG_MSK)
            pfe |= SATG_MSK;
          if (pfe & OVFR_MSK)
            pfe |= SATR_MSK;
#if VREGRESS_PERTURB != 2   /* oracle proof: "forgot to saturate blue" */
          if (pfe & OVFB_MSK)
            pfe |= SATB_MSK;
#endif
        }
      }
      pfe &= BLND_MSK;
      dst[start++] = (pfe >> 16) | pfe;
    }
    else if ((bldtype == BLEND_DARK || bldtype == BLEND_BRIGHT) &&
             (pixpair & 0x200) == 0x200) {
      // Top pixel is 1st-target, can still apply bright/dark effect.
      unsigned short pidx = pal[pixpair & 0x1FF];
      unsigned epixel = (pidx | (pidx << 16)) & BLND_MSK;
      unsigned pa = bldtype == BLEND_DARK ? 0 : ((BLND_MSK * brightf) >> 4) & BLND_MSK;
      unsigned pb = ((epixel * (16 - brightf)) >> 4) & BLND_MSK;
      epixel = (pa + pb) & BLND_MSK;
      dst[start++] = (epixel >> 16) | epixel;
    }
    else {
      dst[start++] = pal[pixpair & 0x1FF];   // No effects
    }
  }
}

template <blendtype bldtype>
static inline void vm_bright_orig(const unsigned short *pal,
  unsigned brightness, unsigned start, unsigned end, unsigned short *srcdst)
{
  while (start < end) {
    unsigned short spix = srcdst[start];
    unsigned short pixcol = pal[spix & 0x1FF];

    if ((spix & 0x200) == 0x200) {
      // Pixel is 1st target, can apply color effect.
      unsigned epixel = (pixcol | (pixcol << 16)) & BLND_MSK;
      unsigned pa = bldtype == BLEND_DARK ? 0 : ((BLND_MSK * brightness) >> 4) & BLND_MSK; // B/W
      unsigned pb = ((epixel * (16 - brightness)) >> 4) & BLND_MSK;  // Pixel color
      epixel = (pa + pb) & BLND_MSK;
      pixcol = (epixel >> 16) | epixel;
    }

    srcdst[start++] = pixcol;
  }
}

/* ---- branch-free scalar ----------------------------------------------------- */

template <bool sat>
static inline unsigned vm_blend_px(unsigned p1, unsigned p2,
                                   unsigned a, unsigned b)
{
  unsigned p1e = (p1 | (p1 << 16)) & BLND_MSK;
  unsigned p2e = (p2 | (p2 << 16)) & BLND_MSK;
  unsigned pfe = ((p1e * a) + (p2e * b)) >> 4;
  if (sat) {
    // A channel's overflow bit (0 or 1) times the channel's mask sets all its
    // bits; no branch.
    pfe |= ((pfe & OVFG_MSK) != 0) * SATG_MSK;
    pfe |= ((pfe & OVFR_MSK) != 0) * SATR_MSK;
#if VREGRESS_PERTURB != 2   /* oracle proof: "forgot to saturate blue" */
    pfe |= ((pfe & OVFB_MSK) != 0) * SATB_MSK;
#endif
  }
  pfe &= BLND_MSK;
  return ((pfe >> 16) | pfe) & 0xFFFF;
}

static inline unsigned vm_bright_px(unsigned p, unsigned pa, unsigned y16)
{
  unsigned e = (p | (p << 16)) & BLND_MSK;
  unsigned pb = ((e * y16) >> 4) & BLND_MSK;
  e = (pa + pb) & BLND_MSK;
  return ((e >> 16) | e) & 0xFFFF;
}

template <blendtype bldtype, bool st_objs, bool sat>
static inline void vm_blend_bf(const unsigned short *pal,
  unsigned a, unsigned b, unsigned y, unsigned start, unsigned end,
  unsigned short *dst, const unsigned *src)
{
  const unsigned y16 = 16 - y;
  const unsigned pa = bldtype == BLEND_DARK ? 0
                                            : ((BLND_MSK * y) >> 4) & BLND_MSK;
  for (; start < end; start++) {
    unsigned pp = src[start];
    unsigned p1 = pal[pp & 0x1FF];
    unsigned r = p1;
    // All the selections are masks of 0 or ~0.  The blend wins over the
    // brightness (the original tests it first).
    if (bldtype == BLEND_DARK || bldtype == BLEND_BRIGHT) {
      unsigned m = 0u - (unsigned)((pp & 0x200) == 0x200);
      r = (r & ~m) | (vm_bright_px(p1, pa, y16) & m);
    }
    if (st_objs || bldtype == BLEND_ONLY) {
      unsigned p2 = pal[(pp >> 16) & 0x1FF];
      unsigned m = 0;
      if (st_objs)
        m |= 0u - (unsigned)((pp & 0x04000800) == 0x04000800);
      if (bldtype == BLEND_ONLY)
        m |= 0u - (unsigned)((pp & 0x04000200) == 0x04000200);
      r = (r & ~m) | (vm_blend_px<sat>(p1, p2, a, b) & m);
    }
    dst[start] = (unsigned short)r;
  }
}

template <blendtype bldtype>
static inline void vm_bright_bf(const unsigned short *pal, unsigned y,
  unsigned start, unsigned end, unsigned short *srcdst)
{
  const unsigned y16 = 16 - y;
  const unsigned pa = bldtype == BLEND_DARK ? 0
                                            : ((BLND_MSK * y) >> 4) & BLND_MSK;
  for (; start < end; start++) {
    unsigned s = srcdst[start];
    unsigned p = pal[s & 0x1FF];
    unsigned m = 0u - ((s >> 9) & 1);
    srcdst[start] = (unsigned short)((p & ~m) | (vm_bright_px(p, pa, y16) & m));
  }
}

/* ---- the vector unit: 8 x u16 lanes --------------------------------------- */

#if !defined(USE_XBGR1555_FORMAT) && !defined(VIDEO_MERGE_NO_VEC)
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#define VM_VEC 1
#include <arm_neon.h>
typedef uint16x8_t vq;
static inline vq vq_set1(unsigned short x) { return vdupq_n_u16(x); }
static inline vq vq_and(vq a, vq b) { return vandq_u16(a, b); }
static inline vq vq_or(vq a, vq b)  { return vorrq_u16(a, b); }
static inline vq vq_add(vq a, vq b) { return vaddq_u16(a, b); }
static inline vq vq_min(vq a, vq b) { return vminq_u16(a, b); }   // values < 0x8000
template <int n> static inline vq vq_shr(vq a) { return vshrq_n_u16(a, n); }
template <int n> static inline vq vq_shl(vq a) { return vshlq_n_u16(a, n); }
static inline vq vq_mul(vq a, unsigned short c) { return vmulq_n_u16(a, c); }
static inline vq vq_mla(vq acc, vq a, unsigned short c) { return vmlaq_n_u16(acc, a, c); }
static inline vq vq_bsl(vq m, vq a, vq b) { return vbslq_u16(m, a, b); }   // m ? a : b
static inline vq vq_tst(vq x, vq bit) { return vtstq_u16(x, bit); }       // (x & bit) != 0, one-bit `bit`
static inline vq vq_load(const unsigned short *p) { return vld1q_u16(p); }
static inline void vq_store(unsigned short *p, vq v) { vst1q_u16(p, v); }
// Pack four 32-bit pairs of u16 lanes (little-endian) into one 8x u16 vector.
static inline vq vq_from_u32x4(unsigned a, unsigned b, unsigned c, unsigned d)
{
  unsigned long long lo = (unsigned long long)a | ((unsigned long long)b << 32);
  unsigned long long hi = (unsigned long long)c | ((unsigned long long)d << 32);
  return vcombine_u16(vcreate_u16(lo), vcreate_u16(hi));
}
#elif defined(__SSE2__)
#define VM_VEC 1
#include <emmintrin.h>
typedef __m128i vq;
static inline vq vq_set1(unsigned short x) { return _mm_set1_epi16((short)x); }
static inline vq vq_and(vq a, vq b) { return _mm_and_si128(a, b); }
static inline vq vq_or(vq a, vq b)  { return _mm_or_si128(a, b); }
static inline vq vq_add(vq a, vq b) { return _mm_add_epi16(a, b); }
static inline vq vq_min(vq a, vq b) { return _mm_min_epi16(a, b); }   // values < 0x8000
template <int n> static inline vq vq_shr(vq a) { return _mm_srli_epi16(a, n); }
template <int n> static inline vq vq_shl(vq a) { return _mm_slli_epi16(a, n); }
static inline vq vq_mul(vq a, unsigned short c) { return _mm_mullo_epi16(a, _mm_set1_epi16((short)c)); }
static inline vq vq_mla(vq acc, vq a, unsigned short c) { return _mm_add_epi16(acc, vq_mul(a, c)); }
static inline vq vq_bsl(vq m, vq a, vq b) { return _mm_or_si128(_mm_and_si128(m, a), _mm_andnot_si128(m, b)); }
static inline vq vq_tst(vq x, vq bit) { return _mm_cmpeq_epi16(_mm_and_si128(x, bit), bit); }
static inline vq vq_load(const unsigned short *p) { return _mm_loadu_si128((const __m128i *)p); }
static inline void vq_store(unsigned short *p, vq v) { _mm_storeu_si128((__m128i *)p, v); }
static inline vq vq_from_u32x4(unsigned a, unsigned b, unsigned c, unsigned d)
{
  return _mm_set_epi32((int)d, (int)c, (int)b, (int)a);
}
#endif
#endif
#ifndef VM_VEC
#define VM_VEC 0
#endif

#if VM_VEC

static inline vq vm_pack_u16x8(unsigned short x0, unsigned short x1,
  unsigned short x2, unsigned short x3, unsigned short x4, unsigned short x5,
  unsigned short x6, unsigned short x7)
{
  return vq_from_u32x4((unsigned)x0 | ((unsigned)x1 << 16),
                       (unsigned)x2 | ((unsigned)x3 << 16),
                       (unsigned)x4 | ((unsigned)x5 << 16),
                       (unsigned)x6 | ((unsigned)x7 << 16));
}

// floor((x*ca + y*cb) / 16), clamped to `mx` only when `sat` (a + b > 16).
// When !sat, x*ca + y*cb <= mx*(ca + cb) <= 16*mx, so floor(...) <= mx always.
// Blue's clamp is the one the perturbation proof removes (keeping low 5 bits).
template <bool blue, bool sat>
static inline vq vm_chan_blend(vq x, vq y, unsigned short ca, unsigned short cb,
                               unsigned short mx)
{
  vq t = vq_shr<4>(vq_mla(vq_mul(x, ca), y, cb));
  if (!sat) return t;
#if VREGRESS_PERTURB == 2
  if (blue) return vq_and(t, vq_set1(mx));
#endif
  return vq_min(t, vq_set1(mx));
}

// floor(c*(16-y)/16) + pa
static inline vq vm_chan_bright(vq x, unsigned short y16, unsigned short pa)
{
  return vq_add(vq_shr<4>(vq_mul(x, y16)), vq_set1(pa));
}

static inline vq vm_pack(vq r, vq g, vq b)
{
  return vq_or(vq_or(vq_shl<11>(r), vq_shl<5>(g)), b);
}

#define VM_R(p) vq_shr<11>(p)
#define VM_G(p) vq_and(vq_shr<5>(p), vq_set1(63))
#define VM_B(p) vq_and((p), vq_set1(31))

// Eight pixels brightened/darkened: the result for every lane.
template <blendtype bldtype>
static inline vq vm_bright8(vq p, unsigned y16, unsigned y)
{
  const unsigned short par = bldtype == BLEND_DARK ? 0 : (31 * y) >> 4;
  const unsigned short pag = bldtype == BLEND_DARK ? 0 : (63 * y) >> 4;
  return vm_pack(vm_chan_bright(VM_R(p), y16, par),
                 vm_chan_bright(VM_G(p), y16, pag),
                 vm_chan_bright(VM_B(p), y16, par));
}

// Branchless ARM-GPR per-pixel blend predicate placed at bit 9 (BLEND_ONLY)
// or bit 11 (st_objs without BLEND_ONLY):
//   bit 9  (0x200)      = top 1st-target
//   bit 11 (0x800)      = top ST-OBJ
//   bit 26 (0x04000000) = bottom 2nd-target
template <blendtype bldtype, bool st_objs>
static inline unsigned vm_blend_bit(unsigned w)
{
  if (st_objs && bldtype == BLEND_ONLY)
    return (w | (w >> 2)) & (w >> 17);   // bit 9 = (bit 9 | bit 11) & bit 26
  if (st_objs)
    return w & (w >> 15);                // bit 11 = bit 11 & bit 26
  return w & (w >> 17);                  // bit 9 = bit 9 & bit 26
}

template <blendtype bldtype, bool st_objs, bool sat>
static inline void vm_blend_vec(const unsigned short *pal,
  unsigned a, unsigned b, unsigned y, unsigned start, unsigned end,
  unsigned short *dst, const unsigned *src)
{
  const unsigned y16 = 16 - y;
  const unsigned bmask = (bldtype == BLEND_ONLY) ? 0x200u : 0x800u;
  const unsigned bwant = (st_objs && bldtype == BLEND_ONLY) ? 0xA00u
                       : st_objs ? 0x800u : 0x200u;
  for (; start + 8 <= end; start += 8) {
    const unsigned *s = &src[start];
    unsigned short *d = &dst[start];
    const unsigned w0 = s[0], w1 = s[1], w2 = s[2], w3 = s[3];
    const unsigned w4 = s[4], w5 = s[5], w6 = s[6], w7 = s[7];
    const unsigned wor = w0 | w1 | w2 | w3 | w4 | w5 | w6 | w7;
    const bool need_bri = (bldtype == BLEND_DARK || bldtype == BLEND_BRIGHT) &&
                          ((wor & 0x200u) != 0);
    const bool maybe_blend = (st_objs || bldtype == BLEND_ONLY) &&
                             ((wor & 0x04000000u) != 0) && ((wor & bwant) != 0);

    if (!need_bri && !maybe_blend) {
      d[0] = pal[w0 & 0x1FF]; d[1] = pal[w1 & 0x1FF];
      d[2] = pal[w2 & 0x1FF]; d[3] = pal[w3 & 0x1FF];
      d[4] = pal[w4 & 0x1FF]; d[5] = pal[w5 & 0x1FF];
      d[6] = pal[w6 & 0x1FF]; d[7] = pal[w7 & 0x1FF];
      continue;
    }

    unsigned t0 = 0, t1 = 0, t2 = 0, t3 = 0, t4 = 0, t5 = 0, t6 = 0, t7 = 0;
    bool need_blend = false, all_blend = false;
    if (maybe_blend) {
      t0 = vm_blend_bit<bldtype, st_objs>(w0);
      t1 = vm_blend_bit<bldtype, st_objs>(w1);
      t2 = vm_blend_bit<bldtype, st_objs>(w2);
      t3 = vm_blend_bit<bldtype, st_objs>(w3);
      t4 = vm_blend_bit<bldtype, st_objs>(w4);
      t5 = vm_blend_bit<bldtype, st_objs>(w5);
      t6 = vm_blend_bit<bldtype, st_objs>(w6);
      t7 = vm_blend_bit<bldtype, st_objs>(w7);
      need_blend = ((t0 | t1 | t2 | t3 | t4 | t5 | t6 | t7) & bmask) != 0;
      if (!need_bri && !need_blend) {
        d[0] = pal[w0 & 0x1FF]; d[1] = pal[w1 & 0x1FF];
        d[2] = pal[w2 & 0x1FF]; d[3] = pal[w3 & 0x1FF];
        d[4] = pal[w4 & 0x1FF]; d[5] = pal[w5 & 0x1FF];
        d[6] = pal[w6 & 0x1FF]; d[7] = pal[w7 & 0x1FF];
        continue;
      }
      all_blend = ((t0 & t1 & t2 & t3 & t4 & t5 & t6 & t7) & bmask) != 0;
    }

    vq p1 = vm_pack_u16x8(pal[w0 & 0x1FF], pal[w1 & 0x1FF],
                          pal[w2 & 0x1FF], pal[w3 & 0x1FF],
                          pal[w4 & 0x1FF], pal[w5 & 0x1FF],
                          pal[w6 & 0x1FF], pal[w7 & 0x1FF]);
    if (all_blend) {
      vq p2 = vm_pack_u16x8(pal[(w0 >> 16) & 0x1FF], pal[(w1 >> 16) & 0x1FF],
                            pal[(w2 >> 16) & 0x1FF], pal[(w3 >> 16) & 0x1FF],
                            pal[(w4 >> 16) & 0x1FF], pal[(w5 >> 16) & 0x1FF],
                            pal[(w6 >> 16) & 0x1FF], pal[(w7 >> 16) & 0x1FF]);
      vq v = vm_pack(
        vm_chan_blend<false, sat>(VM_R(p1), VM_R(p2), a, b, 31),
        vm_chan_blend<false, sat>(VM_G(p1), VM_G(p2), a, b, 63),
        vm_chan_blend<true,  sat>(VM_B(p1), VM_B(p2), a, b, 31));
      vq_store(d, v);
      continue;
    }

    vq r = p1;
    if (need_bri) {
      vq b8 = vm_bright8<bldtype>(p1, y16, y);
      if ((w0 & w1 & w2 & w3 & w4 & w5 & w6 & w7) & 0x200u) {
        r = b8;
      } else {
        vq mv = vq_from_u32x4((w0 & 0xFFFFu) | (w1 << 16),
                              (w2 & 0xFFFFu) | (w3 << 16),
                              (w4 & 0xFFFFu) | (w5 << 16),
                              (w6 & 0xFFFFu) | (w7 << 16));
        r = vq_bsl(vq_tst(mv, vq_set1(0x200)), b8, r);
      }
    }
    if (need_blend) {
      vq p2 = vm_pack_u16x8(pal[(w0 >> 16) & 0x1FF], pal[(w1 >> 16) & 0x1FF],
                            pal[(w2 >> 16) & 0x1FF], pal[(w3 >> 16) & 0x1FF],
                            pal[(w4 >> 16) & 0x1FF], pal[(w5 >> 16) & 0x1FF],
                            pal[(w6 >> 16) & 0x1FF], pal[(w7 >> 16) & 0x1FF]);
      vq v = vm_pack(
        vm_chan_blend<false, sat>(VM_R(p1), VM_R(p2), a, b, 31),
        vm_chan_blend<false, sat>(VM_G(p1), VM_G(p2), a, b, 63),
        vm_chan_blend<true,  sat>(VM_B(p1), VM_B(p2), a, b, 31));
      vq mv = vq_from_u32x4((t0 & 0xFFFFu) | (t1 << 16),
                            (t2 & 0xFFFFu) | (t3 << 16),
                            (t4 & 0xFFFFu) | (t5 << 16),
                            (t6 & 0xFFFFu) | (t7 << 16));
      r = vq_bsl(vq_tst(mv, vq_set1((unsigned short)bmask)), v, r);
    }
    vq_store(d, r);
  }
  vm_blend_orig<bldtype, st_objs, sat>(pal, a, b, y, start, end, dst, src);
}

template <blendtype bldtype>
static inline void vm_bright_vec(const unsigned short *pal, unsigned y,
  unsigned start, unsigned end, unsigned short *srcdst)
{
  const unsigned y16 = 16 - y;
  const vq k200 = vq_set1(0x200);
  for (; start + 8 <= end; start += 8) {
    unsigned short *s = &srcdst[start];
    const unsigned h0 = s[0], h1 = s[1], h2 = s[2], h3 = s[3];
    const unsigned h4 = s[4], h5 = s[5], h6 = s[6], h7 = s[7];
    const unsigned hor = h0 | h1 | h2 | h3 | h4 | h5 | h6 | h7;
    if (!(hor & 0x200u)) {
      s[0] = pal[h0 & 0x1FF]; s[1] = pal[h1 & 0x1FF];
      s[2] = pal[h2 & 0x1FF]; s[3] = pal[h3 & 0x1FF];
      s[4] = pal[h4 & 0x1FF]; s[5] = pal[h5 & 0x1FF];
      s[6] = pal[h6 & 0x1FF]; s[7] = pal[h7 & 0x1FF];
      continue;
    }
    vq p = vm_pack_u16x8(pal[h0 & 0x1FF], pal[h1 & 0x1FF],
                         pal[h2 & 0x1FF], pal[h3 & 0x1FF],
                         pal[h4 & 0x1FF], pal[h5 & 0x1FF],
                         pal[h6 & 0x1FF], pal[h7 & 0x1FF]);
    vq b8 = vm_bright8<bldtype>(p, y16, y);
    if ((h0 & h1 & h2 & h3 & h4 & h5 & h6 & h7) & 0x200u) {
      vq_store(s, b8);
    } else {
      vq mv = vq_from_u32x4(h0 | (h1 << 16), h2 | (h3 << 16),
                            h4 | (h5 << 16), h6 | (h7 << 16));
      vq_store(s, vq_bsl(vq_tst(mv, k200), b8, p));
    }
  }
  vm_bright_orig<bldtype>(pal, y, start, end, srcdst);
}

// Blend with precomputed 1024-entry brightness palette (pal_bri[0..511] = raw,
// pal_bri[512..1023] = brightened/darkened), for BLEND_BRIGHT / BLEND_DARK.
// Non-blending pixels (whether 1st-target or not) are a single pal_bri[w & 0x3FF]
// lookup; ST-OBJ blending pixels blend raw pal_bri[w & 0x1FF] and pal_bri[(w>>16)&0x1FF].
template <bool st_objs, bool sat>
static inline void vm_blend_palbri_vec(const unsigned short *pal_bri,
  unsigned a, unsigned b, unsigned start, unsigned end,
  unsigned short *dst, const unsigned *src)
{
  const vq k800 = vq_set1(0x800);
  for (; start + 8 <= end; start += 8) {
    const unsigned *s = &src[start];
    unsigned short *d = &dst[start];
    const unsigned w0 = s[0], w1 = s[1], w2 = s[2], w3 = s[3];
    const unsigned w4 = s[4], w5 = s[5], w6 = s[6], w7 = s[7];
    if (st_objs) {
      const unsigned wor = w0 | w1 | w2 | w3 | w4 | w5 | w6 | w7;
      if ((wor & 0x04000800u) == 0x04000800u) {
        const unsigned t0 = w0 & (w0 >> 15), t1 = w1 & (w1 >> 15);
        const unsigned t2 = w2 & (w2 >> 15), t3 = w3 & (w3 >> 15);
        const unsigned t4 = w4 & (w4 >> 15), t5 = w5 & (w5 >> 15);
        const unsigned t6 = w6 & (w6 >> 15), t7 = w7 & (w7 >> 15);
        if ((t0 | t1 | t2 | t3 | t4 | t5 | t6 | t7) & 0x800u) {
          vq p1 = vm_pack_u16x8(pal_bri[w0 & 0x1FF], pal_bri[w1 & 0x1FF],
                                pal_bri[w2 & 0x1FF], pal_bri[w3 & 0x1FF],
                                pal_bri[w4 & 0x1FF], pal_bri[w5 & 0x1FF],
                                pal_bri[w6 & 0x1FF], pal_bri[w7 & 0x1FF]);
          vq p2 = vm_pack_u16x8(pal_bri[(w0 >> 16) & 0x1FF], pal_bri[(w1 >> 16) & 0x1FF],
                                pal_bri[(w2 >> 16) & 0x1FF], pal_bri[(w3 >> 16) & 0x1FF],
                                pal_bri[(w4 >> 16) & 0x1FF], pal_bri[(w5 >> 16) & 0x1FF],
                                pal_bri[(w6 >> 16) & 0x1FF], pal_bri[(w7 >> 16) & 0x1FF]);
          vq v = vm_pack(
            vm_chan_blend<false, sat>(VM_R(p1), VM_R(p2), a, b, 31),
            vm_chan_blend<false, sat>(VM_G(p1), VM_G(p2), a, b, 63),
            vm_chan_blend<true,  sat>(VM_B(p1), VM_B(p2), a, b, 31));
          if ((t0 & t1 & t2 & t3 & t4 & t5 & t6 & t7) & 0x800u) {
            vq_store(d, v);
          } else {
            vq r = vm_pack_u16x8(pal_bri[w0 & 0x3FF], pal_bri[w1 & 0x3FF],
                                 pal_bri[w2 & 0x3FF], pal_bri[w3 & 0x3FF],
                                 pal_bri[w4 & 0x3FF], pal_bri[w5 & 0x3FF],
                                 pal_bri[w6 & 0x3FF], pal_bri[w7 & 0x3FF]);
            vq mv = vq_from_u32x4((t0 & 0xFFFFu) | (t1 << 16),
                                  (t2 & 0xFFFFu) | (t3 << 16),
                                  (t4 & 0xFFFFu) | (t5 << 16),
                                  (t6 & 0xFFFFu) | (t7 << 16));
            vq_store(d, vq_bsl(vq_tst(mv, k800), v, r));
          }
          continue;
        }
      }
    }
    d[0] = pal_bri[w0 & 0x3FF]; d[1] = pal_bri[w1 & 0x3FF];
    d[2] = pal_bri[w2 & 0x3FF]; d[3] = pal_bri[w3 & 0x3FF];
    d[4] = pal_bri[w4 & 0x3FF]; d[5] = pal_bri[w5 & 0x3FF];
    d[6] = pal_bri[w6 & 0x3FF]; d[7] = pal_bri[w7 & 0x3FF];
  }
  for (; start < end; start++) {
    unsigned pp = src[start];
    if (st_objs && (pp & 0x04000800u) == 0x04000800u)
      dst[start] = (unsigned short)vm_blend_px<sat>(
        pal_bri[pp & 0x1FF], pal_bri[(pp >> 16) & 0x1FF], a, b);
    else
      dst[start] = pal_bri[pp & 0x3FF];
  }
}
#endif /* VM_VEC */

/* ---- 2 KiB L1-resident brightness palette helpers ------------------------- */

template <blendtype bldtype>
static inline void vm_build_pal_bri(unsigned short *out1024,
  const unsigned short *pal512, unsigned y)
{
  const unsigned y16 = 16 - y;
#if VM_VEC
  for (unsigned i = 0; i < 512; i += 8) {
    vq p = vq_load(pal512 + i);
    vq_store(out1024 + i, p);
    vq_store(out1024 + 512 + i, vm_bright8<bldtype>(p, y16, y));
  }
#else
  const unsigned pa = bldtype == BLEND_DARK ? 0
                                            : ((BLND_MSK * y) >> 4) & BLND_MSK;
  for (unsigned i = 0; i < 512; i++) {
    unsigned short p = pal512[i];
    out1024[i] = p;
    out1024[512 + i] = (unsigned short)vm_bright_px(p, pa, y16);
  }
#endif
}

static inline void vm_merge_brightness_palbri(const unsigned short *pal_bri,
  unsigned start, unsigned end, unsigned short *srcdst)
{
  for (; start + 4 <= end; start += 4) {
    unsigned s0 = srcdst[start + 0];
    unsigned s1 = srcdst[start + 1];
    unsigned s2 = srcdst[start + 2];
    unsigned s3 = srcdst[start + 3];
    srcdst[start + 0] = pal_bri[s0 & 0x3FF];
    srcdst[start + 1] = pal_bri[s1 & 0x3FF];
    srcdst[start + 2] = pal_bri[s2 & 0x3FF];
    srcdst[start + 3] = pal_bri[s3 & 0x3FF];
  }
  for (; start < end; start++)
    srcdst[start] = pal_bri[srcdst[start] & 0x3FF];
}

template <bool st_objs, bool sat>
static inline void vm_blend_palbri_scalar(const unsigned short *pal_bri,
  unsigned a, unsigned b, unsigned start, unsigned end,
  unsigned short *dst, const unsigned *src)
{
  for (; start < end; start++) {
    unsigned pp = src[start];
    if (st_objs && (pp & 0x04000800u) == 0x04000800u)
      dst[start] = (unsigned short)vm_blend_px<sat>(
        pal_bri[pp & 0x1FF], pal_bri[(pp >> 16) & 0x1FF], a, b);
    else
      dst[start] = pal_bri[pp & 0x3FF];
  }
}

/* ---- entry points --------------------------------------------------------- */

// The coefficients as the original reads them: a register field, at most 16.
static inline unsigned vm_c16(unsigned v) { return v > 16 ? 16 : v; }

// which: 0 = dispatch (vector, else the original loops), 1 = original,
// 2 = branch-free scalar, 3 = vector (only when VM_VEC).
template <blendtype bldtype, bool st_objs, int which>
static inline void vm_merge_blend_w(const unsigned short *pal,
  unsigned bldalpha, unsigned bldy, unsigned start, unsigned end,
  unsigned short *dst, const unsigned *src)
{
  const unsigned y = vm_c16(bldy & 0x1F);
  const unsigned a = vm_c16((bldalpha >> 0) & 0x1F);
  const unsigned b = vm_c16((bldalpha >> 8) & 0x1F);
  const bool can_saturate = a + b > 16;
  int w = which;
#if VM_VEC
  if (w == 0) w = 3;
#elif defined(VIDEO_MERGE_SCALAR_BF)
  if (w == 0) w = 2;
#else
  if (w == 0) w = 1;
#endif
#if VM_VEC
  if (w == 3) {
    if (can_saturate)
      vm_blend_vec<bldtype, st_objs, true>(pal, a, b, y, start, end, dst, src);
    else
      vm_blend_vec<bldtype, st_objs, false>(pal, a, b, y, start, end, dst, src);
    return;
  }
#endif
  if (w == 2) {
    if (can_saturate)
      vm_blend_bf<bldtype, st_objs, true>(pal, a, b, y, start, end, dst, src);
    else
      vm_blend_bf<bldtype, st_objs, false>(pal, a, b, y, start, end, dst, src);
  } else {
    if (can_saturate)
      vm_blend_orig<bldtype, st_objs, true>(pal, a, b, y, start, end, dst, src);
    else
      vm_blend_orig<bldtype, st_objs, false>(pal, a, b, y, start, end, dst, src);
  }
}

template <blendtype bldtype, int which>
static inline void vm_merge_brightness_w(const unsigned short *pal,
  unsigned bldy, unsigned start, unsigned end, unsigned short *srcdst)
{
  const unsigned y = vm_c16(bldy & 0x1F);
  int w = which;
#if VM_VEC
  if (w == 0) w = 3;
#elif defined(VIDEO_MERGE_SCALAR_BF)
  if (w == 0) w = 2;
#else
  if (w == 0) w = 1;
#endif
#if VM_VEC
  if (w == 3) { vm_bright_vec<bldtype>(pal, y, start, end, srcdst); return; }
#endif
  if (w == 2) vm_bright_bf<bldtype>(pal, y, start, end, srcdst);
  else        vm_bright_orig<bldtype>(pal, y, start, end, srcdst);
}

template <blendtype bldtype, bool st_objs>
static inline void vm_merge_blend(const unsigned short *pal,
  unsigned bldalpha, unsigned bldy, unsigned start, unsigned end,
  unsigned short *dst, const unsigned *src)
{
  vm_merge_blend_w<bldtype, st_objs, 0>(pal, bldalpha, bldy, start, end, dst, src);
}

template <blendtype bldtype>
static inline void vm_merge_brightness(const unsigned short *pal,
  unsigned bldy, unsigned start, unsigned end, unsigned short *srcdst)
{
  vm_merge_brightness_w<bldtype, 0>(pal, bldy, start, end, srcdst);
}

template <bool st_objs>
static inline void vm_merge_blend_palbri(const unsigned short *pal_bri,
  unsigned bldalpha, unsigned start, unsigned end,
  unsigned short *dst, const unsigned *src)
{
  const unsigned a = vm_c16((bldalpha >> 0) & 0x1F);
  const unsigned b = vm_c16((bldalpha >> 8) & 0x1F);
  const bool can_saturate = a + b > 16;
#if VM_VEC
  if (can_saturate)
    vm_blend_palbri_vec<st_objs, true>(pal_bri, a, b, start, end, dst, src);
  else
    vm_blend_palbri_vec<st_objs, false>(pal_bri, a, b, start, end, dst, src);
#else
  if (can_saturate)
    vm_blend_palbri_scalar<st_objs, true>(pal_bri, a, b, start, end, dst, src);
  else
    vm_blend_palbri_scalar<st_objs, false>(pal_bri, a, b, start, end, dst, src);
#endif
}

#endif /* GPSP_VIDEO_MERGE_H */
