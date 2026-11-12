/* video_merge.h -- the scanline renderer's colour-effect merges.
 *
 * merge_blend() and merge_brightness() of video.cc, as plain kernels over
 * (palette, coefficients, source line, destination line), so that the renderer
 * and tools/tests/test_video_merge.cc compile the SAME code.
 *
 * Two implementations of one function, each bit-for-bit the original
 * per-pixel loop (the test compares both with a verbatim copy of it):
 *
 *   vm_*_scalar  branch-free: both outcomes are computed and one is selected
 *                with a mask.  Used wherever there is no NEON.
 *   vm_*_neon    8 pixels per step (ARMv7 NEON; the Vita is a Cortex-A9).  The
 *                palette lookups stay scalar loads (NEON has no gather), the
 *                arithmetic is done on 16-bit lanes, one colour channel per
 *                vector; a group of eight pixels none of which is blended or
 *                brightened is stored straight from the palette lookup.  The
 *                last (end - start) % 8 pixels take the scalar kernel.
 * The original loops stay in video.cc under -DVIDEO_MERGE_ORIG (the reference
 * the speed and the pictures are compared with).
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
 * whole pixels.
 */
#ifndef GPSP_VIDEO_MERGE_H
#define GPSP_VIDEO_MERGE_H

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#define VM_HAVE_NEON 1
#else
#define VM_HAVE_NEON 0
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

/* ---- the scalar kernels (branch-free) ------------------------------------ */

// Pixel p1 blended with p2 (coefficients a, b <= 16).  `sat`: a + b > 16, the
// sum can exceed a channel's range and is clamped (otherwise it cannot).
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

// Pixel p brightened or darkened; pa is the white part, computed once.
static inline unsigned vm_bright_px(unsigned p, unsigned pa,
                                    unsigned y16)    // 16 - y
{
  unsigned e = (p | (p << 16)) & BLND_MSK;
  unsigned pb = ((e * y16) >> 4) & BLND_MSK;
  e = (pa + pb) & BLND_MSK;
  return ((e >> 16) | e) & 0xFFFF;
}

template <blendtype bldtype, bool st_objs, bool sat>
static inline void vm_blend_scalar(const unsigned short *pal,
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
static inline void vm_bright_scalar(const unsigned short *pal, unsigned y,
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

/* ---- NEON (RGB565 only: it splits the channels at their 565 positions) ----- */

#if VM_HAVE_NEON && !defined(USE_XBGR1555_FORMAT) && !defined(VIDEO_MERGE_NO_NEON)
#define VM_NEON 1

// True if any lane of m is set.
static inline int vm_any(uint16x8_t m)
{
  uint16x4_t h = vorr_u16(vget_low_u16(m), vget_high_u16(m));
  uint32x2_t w = vreinterpret_u32_u16(h);
  return (vget_lane_u32(w, 0) | vget_lane_u32(w, 1)) != 0;
}

// Eight palette entries, indexed by the 9-bit fields of s[0..7] at bit `sh`.
template <int sh>
static inline uint16x8_t vm_gather32(const unsigned short *pal,
                                     const unsigned *s)
{
  uint16x8_t v = vdupq_n_u16(0);
  v = vsetq_lane_u16(pal[(s[0] >> sh) & 0x1FF], v, 0);
  v = vsetq_lane_u16(pal[(s[1] >> sh) & 0x1FF], v, 1);
  v = vsetq_lane_u16(pal[(s[2] >> sh) & 0x1FF], v, 2);
  v = vsetq_lane_u16(pal[(s[3] >> sh) & 0x1FF], v, 3);
  v = vsetq_lane_u16(pal[(s[4] >> sh) & 0x1FF], v, 4);
  v = vsetq_lane_u16(pal[(s[5] >> sh) & 0x1FF], v, 5);
  v = vsetq_lane_u16(pal[(s[6] >> sh) & 0x1FF], v, 6);
  v = vsetq_lane_u16(pal[(s[7] >> sh) & 0x1FF], v, 7);
  return v;
}

static inline uint16x8_t vm_gather16(const unsigned short *pal,
                                     const unsigned short *s)
{
  uint16x8_t v = vdupq_n_u16(0);
  v = vsetq_lane_u16(pal[s[0] & 0x1FF], v, 0);
  v = vsetq_lane_u16(pal[s[1] & 0x1FF], v, 1);
  v = vsetq_lane_u16(pal[s[2] & 0x1FF], v, 2);
  v = vsetq_lane_u16(pal[s[3] & 0x1FF], v, 3);
  v = vsetq_lane_u16(pal[s[4] & 0x1FF], v, 4);
  v = vsetq_lane_u16(pal[s[5] & 0x1FF], v, 5);
  v = vsetq_lane_u16(pal[s[6] & 0x1FF], v, 6);
  v = vsetq_lane_u16(pal[s[7] & 0x1FF], v, 7);
  return v;
}

// floor((x*ca + y*cb) / 16), clamped to `mx`.  Blue's clamp is the one the
// perturbation proof removes (the original then keeps the low 5 bits).
static inline uint16x8_t vm_chan_blend(uint16x8_t x, uint16x8_t y,
                                       unsigned short ca, unsigned short cb,
                                       unsigned short mx, bool blue)
{
  uint16x8_t t = vmulq_n_u16(x, ca);
  t = vmlaq_n_u16(t, y, cb);
  t = vshrq_n_u16(t, 4);
#if VREGRESS_PERTURB == 2
  if (blue) return vandq_u16(t, vdupq_n_u16(mx));
#endif
  (void)blue;
  return vminq_u16(t, vdupq_n_u16(mx));
}

// floor(c*(16-y)/16) + pa
static inline uint16x8_t vm_chan_bright(uint16x8_t x, unsigned short y16,
                                        unsigned short pa)
{
  uint16x8_t t = vshrq_n_u16(vmulq_n_u16(x, y16), 4);
  return vaddq_u16(t, vdupq_n_u16(pa));
}

static inline uint16x8_t vm_pack(uint16x8_t r, uint16x8_t g, uint16x8_t b)
{
  return vorrq_u16(vorrq_u16(vshlq_n_u16(r, 11), vshlq_n_u16(g, 5)), b);
}

#define VM_R(p) vshrq_n_u16((p), 11)
#define VM_G(p) vandq_u16(vshrq_n_u16((p), 5), vdupq_n_u16(63))
#define VM_B(p) vandq_u16((p), vdupq_n_u16(31))

// Eight pixels brightened/darkened: the result for every lane.
template <blendtype bldtype>
static inline uint16x8_t vm_bright8(uint16x8_t p, unsigned y16, unsigned y)
{
  const unsigned short par = bldtype == BLEND_DARK ? 0 : (31 * y) >> 4;
  const unsigned short pag = bldtype == BLEND_DARK ? 0 : (63 * y) >> 4;
  return vm_pack(vm_chan_bright(VM_R(p), y16, par),
                 vm_chan_bright(VM_G(p), y16, pag),
                 vm_chan_bright(VM_B(p), y16, par));
}

template <blendtype bldtype, bool st_objs, bool sat>
static inline void vm_blend_neon(const unsigned short *pal,
  unsigned a, unsigned b, unsigned y, unsigned start, unsigned end,
  unsigned short *dst, const unsigned *src)
{
  const unsigned y16 = 16 - y;
  const uint16x8_t k200 = vdupq_n_u16(0x200), k400 = vdupq_n_u16(0x400),
                   k800 = vdupq_n_u16(0x800);
  for (; start + 8 <= end; start += 8) {
    const unsigned *s = &src[start];
    // Low and high halves of the eight pixel pairs (little-endian).
    uint16x8x2_t lh = vld2q_u16((const unsigned short *)s);
    uint16x8_t p1 = vm_gather32<0>(pal, s);
    uint16x8_t r = p1;

    if (bldtype == BLEND_DARK || bldtype == BLEND_BRIGHT) {
      uint16x8_t m_bri = vtstq_u16(lh.val[0], k200);
      if (vm_any(m_bri))
        r = vbslq_u16(m_bri, vm_bright8<bldtype>(p1, y16, y), r);
    }
    if (st_objs || bldtype == BLEND_ONLY) {
      uint16x8_t hi2nd = vtstq_u16(lh.val[1], k400);        // 0x04000000
      uint16x8_t m_blend = vdupq_n_u16(0);
      if (st_objs)
        m_blend = vorrq_u16(m_blend, vandq_u16(vtstq_u16(lh.val[0], k800), hi2nd));
      if (bldtype == BLEND_ONLY)
        m_blend = vorrq_u16(m_blend, vandq_u16(vtstq_u16(lh.val[0], k200), hi2nd));
      if (vm_any(m_blend)) {
        uint16x8_t p2 = vm_gather32<16>(pal, s);
        uint16x8_t v = vm_pack(
          vm_chan_blend(VM_R(p1), VM_R(p2), a, b, 31, false),
          vm_chan_blend(VM_G(p1), VM_G(p2), a, b, 63, false),
          vm_chan_blend(VM_B(p1), VM_B(p2), a, b, 31, true));
        r = vbslq_u16(m_blend, v, r);
      }
    }
    vst1q_u16(&dst[start], r);
  }
  vm_blend_scalar<bldtype, st_objs, sat>(pal, a, b, y, start, end, dst, src);
}

template <blendtype bldtype>
static inline void vm_bright_neon(const unsigned short *pal, unsigned y,
  unsigned start, unsigned end, unsigned short *srcdst)
{
  const unsigned y16 = 16 - y;
  const uint16x8_t k200 = vdupq_n_u16(0x200);
  for (; start + 8 <= end; start += 8) {
    uint16x8_t s = vld1q_u16(&srcdst[start]);
    uint16x8_t p = vm_gather16(pal, &srcdst[start]);
    uint16x8_t m = vtstq_u16(s, k200);
    if (vm_any(m))
      p = vbslq_u16(m, vm_bright8<bldtype>(p, y16, y), p);
    vst1q_u16(&srcdst[start], p);
  }
  vm_bright_scalar<bldtype>(pal, y, start, end, srcdst);
}
#else
#define VM_NEON 0
#endif

/* ---- entry points --------------------------------------------------------- */

template <blendtype bldtype, bool st_objs>
static inline void vm_merge_blend(const unsigned short *pal,
  unsigned bldalpha, unsigned bldy, unsigned start, unsigned end,
  unsigned short *dst, const unsigned *src)
{
  unsigned y = (bldy & 0x1F) > 16 ? 16 : (bldy & 0x1F);
  unsigned a = ((bldalpha >> 0) & 0x1F) > 16 ? 16 : ((bldalpha >> 0) & 0x1F);
  unsigned b = ((bldalpha >> 8) & 0x1F) > 16 ? 16 : ((bldalpha >> 8) & 0x1F);
  bool can_saturate = a + b > 16;
#if VM_NEON
  if (can_saturate)
    vm_blend_neon<bldtype, st_objs, true>(pal, a, b, y, start, end, dst, src);
  else
    vm_blend_neon<bldtype, st_objs, false>(pal, a, b, y, start, end, dst, src);
#else
  if (can_saturate)
    vm_blend_scalar<bldtype, st_objs, true>(pal, a, b, y, start, end, dst, src);
  else
    vm_blend_scalar<bldtype, st_objs, false>(pal, a, b, y, start, end, dst, src);
#endif
}

template <blendtype bldtype>
static inline void vm_merge_brightness(const unsigned short *pal,
  unsigned bldy, unsigned start, unsigned end, unsigned short *srcdst)
{
  unsigned y = (bldy & 0x1F) > 16 ? 16 : (bldy & 0x1F);
#if VM_NEON
  vm_bright_neon<bldtype>(pal, y, start, end, srcdst);
#else
  vm_bright_scalar<bldtype>(pal, y, start, end, srcdst);
#endif
}

#endif /* GPSP_VIDEO_MERGE_H */
