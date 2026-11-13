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
static inline int vq_any(vq m)
{
  uint16x4_t h = vorr_u16(vget_low_u16(m), vget_high_u16(m));
  uint32x2_t w = vreinterpret_u32_u16(h);
  return (vget_lane_u32(w, 0) | vget_lane_u32(w, 1)) != 0;
}
static inline vq vq_load(const unsigned short *p) { return vld1q_u16(p); }
static inline void vq_store(unsigned short *p, vq v) { vst1q_u16(p, v); }
// The low and the high halves of eight 32-bit words.
static inline void vq_lohi(const unsigned *s, vq *lo, vq *hi)
{
  uint16x8x2_t lh = vld2q_u16((const unsigned short *)s);
  *lo = lh.val[0]; *hi = lh.val[1];
}
#define VQ_LANE(v, n, x) (v) = vsetq_lane_u16((x), (v), (n))
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
static inline int vq_any(vq m) { return _mm_movemask_epi8(m) != 0; }
static inline vq vq_load(const unsigned short *p) { return _mm_loadu_si128((const __m128i *)p); }
static inline void vq_store(unsigned short *p, vq v) { _mm_storeu_si128((__m128i *)p, v); }
static inline void vq_lohi(const unsigned *s, vq *lo, vq *hi)
{
  __m128i a = _mm_loadu_si128((const __m128i *)s);
  __m128i b = _mm_loadu_si128((const __m128i *)(s + 4));
  // sign-extended 16-bit halves survive the signed pack unchanged
  *lo = _mm_packs_epi32(_mm_srai_epi32(_mm_slli_epi32(a, 16), 16),
                        _mm_srai_epi32(_mm_slli_epi32(b, 16), 16));
  *hi = _mm_packs_epi32(_mm_srai_epi32(a, 16), _mm_srai_epi32(b, 16));
}
#define VQ_LANE(v, n, x) (v) = _mm_insert_epi16((v), (x), (n))
#endif
#endif
#ifndef VM_VEC
#define VM_VEC 0
#endif

#if VM_VEC

// Eight palette entries, indexed by the 9-bit fields of s[0..7] at bit `sh`.
template <int sh>
static inline vq vm_gather32(const unsigned short *pal, const unsigned *s)
{
  vq v = vq_set1(0);
  VQ_LANE(v, 0, pal[(s[0] >> sh) & 0x1FF]);
  VQ_LANE(v, 1, pal[(s[1] >> sh) & 0x1FF]);
  VQ_LANE(v, 2, pal[(s[2] >> sh) & 0x1FF]);
  VQ_LANE(v, 3, pal[(s[3] >> sh) & 0x1FF]);
  VQ_LANE(v, 4, pal[(s[4] >> sh) & 0x1FF]);
  VQ_LANE(v, 5, pal[(s[5] >> sh) & 0x1FF]);
  VQ_LANE(v, 6, pal[(s[6] >> sh) & 0x1FF]);
  VQ_LANE(v, 7, pal[(s[7] >> sh) & 0x1FF]);
  return v;
}

static inline vq vm_gather16(const unsigned short *pal, const unsigned short *s)
{
  vq v = vq_set1(0);
  VQ_LANE(v, 0, pal[s[0] & 0x1FF]);
  VQ_LANE(v, 1, pal[s[1] & 0x1FF]);
  VQ_LANE(v, 2, pal[s[2] & 0x1FF]);
  VQ_LANE(v, 3, pal[s[3] & 0x1FF]);
  VQ_LANE(v, 4, pal[s[4] & 0x1FF]);
  VQ_LANE(v, 5, pal[s[5] & 0x1FF]);
  VQ_LANE(v, 6, pal[s[6] & 0x1FF]);
  VQ_LANE(v, 7, pal[s[7] & 0x1FF]);
  return v;
}

// floor((x*ca + y*cb) / 16), clamped to `mx`.  Blue's clamp is the one the
// perturbation proof removes (the original then keeps the low 5 bits).
template <bool blue>
static inline vq vm_chan_blend(vq x, vq y, unsigned short ca, unsigned short cb,
                               unsigned short mx)
{
  vq t = vq_shr<4>(vq_mla(vq_mul(x, ca), y, cb));
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

template <blendtype bldtype, bool st_objs, bool sat>
static inline void vm_blend_vec(const unsigned short *pal,
  unsigned a, unsigned b, unsigned y, unsigned start, unsigned end,
  unsigned short *dst, const unsigned *src)
{
  const unsigned y16 = 16 - y;
  const vq k200 = vq_set1(0x200), k400 = vq_set1(0x400), k800 = vq_set1(0x800);
  for (; start + 8 <= end; start += 8) {
    const unsigned *s = &src[start];
    vq lo, hi;
    vq_lohi(s, &lo, &hi);                  // the low and high halves of the pairs
    vq p1 = vm_gather32<0>(pal, s);
    vq r = p1;

    if (bldtype == BLEND_DARK || bldtype == BLEND_BRIGHT) {
      vq m_bri = vq_tst(lo, k200);
      if (vq_any(m_bri))
        r = vq_bsl(m_bri, vm_bright8<bldtype>(p1, y16, y), r);
    }
    if (st_objs || bldtype == BLEND_ONLY) {
      vq hi2nd = vq_tst(hi, k400);                         // 0x04000000
      vq m_blend = vq_set1(0);
      if (st_objs)
        m_blend = vq_or(m_blend, vq_and(vq_tst(lo, k800), hi2nd));
      if (bldtype == BLEND_ONLY)
        m_blend = vq_or(m_blend, vq_and(vq_tst(lo, k200), hi2nd));
      if (vq_any(m_blend)) {
        vq p2 = vm_gather32<16>(pal, s);
        vq v = vm_pack(
          vm_chan_blend<false>(VM_R(p1), VM_R(p2), a, b, 31),
          vm_chan_blend<false>(VM_G(p1), VM_G(p2), a, b, 63),
          vm_chan_blend<true>(VM_B(p1), VM_B(p2), a, b, 31));
        r = vq_bsl(m_blend, v, r);
      }
    }
    vq_store(&dst[start], r);
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
    vq s = vq_load(&srcdst[start]);
    vq p = vm_gather16(pal, &srcdst[start]);
    vq m = vq_tst(s, k200);
    if (vq_any(m))
      p = vq_bsl(m, vm_bright8<bldtype>(p, y16, y), p);
    vq_store(&srcdst[start], p);
  }
  vm_bright_orig<bldtype>(pal, y, start, end, srcdst);
}
#endif /* VM_VEC */

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

#endif /* GPSP_VIDEO_MERGE_H */
