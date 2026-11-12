/* test_video_merge.cc -- the colour-effect merge kernels of video_merge.h
 * against a verbatim copy of the per-pixel loops they replaced (video.cc's
 * merge_blend / merge_brightness as of the commit before WP4).
 *
 *   x86:    g++ -O2 -o t tools/tests/test_video_merge.cc && ./t          (SSE2
 *           vector code and the scalar code)
 *   armhf:  arm-linux-gnueabihf-g++ -O2 -static -marm -march=armv7-a \
 *             -mfpu=neon -mfloat-abi=hard -o t tools/tests/test_video_merge.cc
 *           qemu-arm ./t                                          (NEON)
 *           add -DVIDEO_MERGE_NO_VEC to run only the scalar code on ARM / x86.
 *   ./t quick   a few percent of the work, for a smoke test.
 *
 * Implementations compared with the verbatim copy of the original loops: the
 * one the renderer calls (vector where the build has NEON or SSE2), the
 * original loops, the branch-free scalar code, and the vector code.
 * What is compared, for every blend type video.cc instantiates
 * (OBJ_BLEND/true, BLEND_ONLY/true+false, BLEND_BRIGHT/true, BLEND_DARK/true,
 * and the two brightness-only kernels):
 *   1  every value of every colour channel pair (R: 32x32, G: 64x64, B: 32x32)
 *      x every coefficient pair 0..16 x 0..16, the other channels random (three
 *      fills), all pixels flagged for blending and brightening;
 *   2  brightness/darkness: all 65536 colours x BLDY 0..31 (>16 clamps) x
 *      flag set and clear;
 *   3  dense random lines: random palette, random flag bits including the junk
 *      bits the renderer never sets, raw BLDALPHA/BLDY register values, every
 *      start 0..9 and length 0..40 (the NEON tail), guard words around the
 *      destination that must stay untouched.
 * Prints "OK" and exits 0, or the first mismatch and exits 1.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>

typedef uint16_t u16;
typedef uint32_t u32;

#include "../../video_merge.h"

/* ---- the reference: the original loops, verbatim, palette passed in ------- */
template <blendtype bldtype, bool st_objs>
static void ref_merge_blend(const u16 *palette_ram_converted, u32 bldalpha,
                            u32 bldy, u32 start, u32 end, u16 *dst, u32 *src) {
#define MIN(a, b) ((a) < (b) ? (a) : (b))
  u32 brightf = MIN(16, bldy & 0x1F);
  u32 blend_a = MIN(16, (bldalpha >> 0) & 0x1F);
  u32 blend_b = MIN(16, (bldalpha >> 8) & 0x1F);

  bool can_saturate = blend_a + blend_b > 16;

  if (can_saturate) {
    while (start < end) {
      u32 pixpair = src[start];
      bool force_blend = (pixpair & 0x04000800) == 0x04000800;
      bool do_blend    = (pixpair & 0x04000200) == 0x04000200;
      if ((st_objs && force_blend) || (do_blend && bldtype == BLEND_ONLY)) {
        u16 p1 = palette_ram_converted[(pixpair >>  0) & 0x1FF];
        u16 p2 = palette_ram_converted[(pixpair >> 16) & 0x1FF];
        u32 p1e = (p1 | (p1 << 16)) & BLND_MSK;
        u32 p2e = (p2 | (p2 << 16)) & BLND_MSK;
        u32 pfe = (((p1e * blend_a) + (p2e * blend_b)) >> 4);

        if (pfe & (OVFR_MSK | OVFG_MSK | OVFB_MSK)) {
          if (pfe & OVFG_MSK)
            pfe |= SATG_MSK;
          if (pfe & OVFR_MSK)
            pfe |= SATR_MSK;
#if VREGRESS_PERTURB != 2
          if (pfe & OVFB_MSK)
            pfe |= SATB_MSK;
#endif
        }
        pfe &= BLND_MSK;
        dst[start++] = (pfe >> 16) | pfe;
      }
      else if ((bldtype == BLEND_DARK || bldtype == BLEND_BRIGHT) &&
               (pixpair & 0x200) == 0x200) {
        u16 pidx = palette_ram_converted[pixpair & 0x1FF];
        u32 epixel = (pidx | (pidx << 16)) & BLND_MSK;
        u32 pa = bldtype == BLEND_DARK ? 0 : ((BLND_MSK * brightf) >> 4) & BLND_MSK;
        u32 pb = ((epixel * (16 - brightf)) >> 4) & BLND_MSK;
        epixel = (pa + pb) & BLND_MSK;
        dst[start++] = (epixel >> 16) | epixel;
      }
      else {
        dst[start++] = palette_ram_converted[pixpair & 0x1FF];
      }
    }
  } else {
    while (start < end) {
      u32 pixpair = src[start];
      bool do_blend    = (pixpair & 0x04000200) == 0x04000200;
      bool force_blend = (pixpair & 0x04000800) == 0x04000800;
      if ((st_objs && force_blend) || (do_blend && bldtype == BLEND_ONLY)) {
        u16 p1 = palette_ram_converted[(pixpair >>  0) & 0x1FF];
        u16 p2 = palette_ram_converted[(pixpair >> 16) & 0x1FF];
        u32 p1e = (p1 | (p1 << 16)) & BLND_MSK;
        u32 p2e = (p2 | (p2 << 16)) & BLND_MSK;
        u32 pfe = (((p1e * blend_a) + (p2e * blend_b)) >> 4) & BLND_MSK;
        dst[start++] = (pfe >> 16) | pfe;
      }
      else if ((bldtype == BLEND_DARK || bldtype == BLEND_BRIGHT) &&
               (pixpair & 0x200) == 0x200) {
        u16 pidx = palette_ram_converted[pixpair & 0x1FF];
        u32 epixel = (pidx | (pidx << 16)) & BLND_MSK;
        u32 pa = bldtype == BLEND_DARK ? 0 : ((BLND_MSK * brightf) >> 4) & BLND_MSK;
        u32 pb = ((epixel * (16 - brightf)) >> 4) & BLND_MSK;
        epixel = (pa + pb) & BLND_MSK;
        dst[start++] = (epixel >> 16) | epixel;
      }
      else {
        dst[start++] = palette_ram_converted[pixpair & 0x1FF];
      }
    }
  }
}

template <blendtype bldtype>
static void ref_merge_brightness(const u16 *palette_ram_converted, u32 bldy,
                                 u32 start, u32 end, u16 *srcdst) {
  u32 brightness = MIN(16, bldy & 0x1F);
  while (start < end) {
    u16 spix = srcdst[start];
    u16 pixcol = palette_ram_converted[spix & 0x1FF];

    if ((spix & 0x200) == 0x200) {
      u32 epixel = (pixcol | (pixcol << 16)) & BLND_MSK;
      u32 pa = bldtype == BLEND_DARK ? 0 : ((BLND_MSK * brightness) >> 4) & BLND_MSK;
      u32 pb = ((epixel * (16 - brightness)) >> 4) & BLND_MSK;
      epixel = (pa + pb) & BLND_MSK;
      pixcol = (epixel >> 16) | epixel;
    }

    srcdst[start++] = pixcol;
  }
}

/* ---- harness --------------------------------------------------------------- */
static uint64_t rs = 0x9E3779B97F4A7C15ull;
static u32 rnd(void)
{
  rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17;
  return (u32)(rs >> 16);
}

static unsigned long long n_px = 0;

static void fail(const char *what, ...)
{
  va_list ap; va_start(ap, what);
  vprintf(what, ap); va_end(ap);
  printf("\nFAIL\n");
  exit(1);
}

#define GUARD 16

// Runs the original loops (the verbatim copy above) and every implementation
// of video_merge.h over src[start..end) and compares them, guards included.
// Implementations: 0 = what the renderer calls, 1 = original, 2 = branch-free
// scalar, 3 = vector (when the build has one).
template <blendtype bt, bool st, int which>
static void run_blend_w(const u16 *pal, u32 alpha, u32 bldy, u32 *src,
                        u32 start, u32 end, const u16 *ref, const char *name)
{
  u16 b[GUARD + 256 + GUARD];
  for (unsigned i = 0; i < GUARD + 256 + GUARD; i++) b[i] = 0xA5A5 + i;
  vm_merge_blend_w<bt, st, which>(pal, alpha, bldy, start, end, b + GUARD, src);
  n_px += end - start;
  if (memcmp(ref, b, sizeof(b))) {
    for (unsigned i = 0; i < GUARD + 256 + GUARD; i++)
      if (ref[i] != b[i])
        fail("%s blend impl %d alpha=%04x bldy=%x start=%u end=%u: lane %d ref %04x got %04x"
             " (src %08x)", name, which, alpha, bldy, start, end, (int)i - GUARD,
             ref[i], b[i], i >= GUARD && i - GUARD < 256 ? src[i - GUARD] : 0);
  }
}

template <blendtype bt, bool st>
static void run_blend(const u16 *pal, u32 alpha, u32 bldy, u32 *src,
                      u32 start, u32 end, const char *name)
{
  u16 a[GUARD + 256 + GUARD];
  for (unsigned i = 0; i < GUARD + 256 + GUARD; i++) a[i] = 0xA5A5 + i;
  ref_merge_blend<bt, st>(pal, alpha, bldy, start, end, a + GUARD, src);
  run_blend_w<bt, st, 0>(pal, alpha, bldy, src, start, end, a, name);
  run_blend_w<bt, st, 1>(pal, alpha, bldy, src, start, end, a, name);
  run_blend_w<bt, st, 2>(pal, alpha, bldy, src, start, end, a, name);
#if VM_VEC
  run_blend_w<bt, st, 3>(pal, alpha, bldy, src, start, end, a, name);
#endif
}

template <blendtype bt, int which>
static void run_bright_w(const u16 *pal, u32 bldy, const u16 *in, u32 start,
                         u32 end, const u16 *ref, const char *name)
{
  u16 b[GUARD + 256 + GUARD];
  for (unsigned i = 0; i < GUARD + 256 + GUARD; i++) b[i] = 0xA5A5 + i;
  memcpy(b + GUARD, in, 256 * 2);
  vm_merge_brightness_w<bt, which>(pal, bldy, start, end, b + GUARD);
  n_px += end - start;
  if (memcmp(ref, b, sizeof(b))) {
    for (unsigned i = 0; i < GUARD + 256 + GUARD; i++)
      if (ref[i] != b[i])
        fail("%s brightness impl %d bldy=%x start=%u end=%u: lane %d ref %04x got %04x",
             name, which, bldy, start, end, (int)i - GUARD, ref[i], b[i]);
  }
}

template <blendtype bt>
static void run_bright(const u16 *pal, u32 bldy, const u16 *in, u32 start,
                       u32 end, const char *name)
{
  u16 a[GUARD + 256 + GUARD];
  for (unsigned i = 0; i < GUARD + 256 + GUARD; i++) a[i] = 0xA5A5 + i;
  memcpy(a + GUARD, in, 256 * 2);
  ref_merge_brightness<bt>(pal, bldy, start, end, a + GUARD);
  run_bright_w<bt, 0>(pal, bldy, in, start, end, a, name);
  run_bright_w<bt, 1>(pal, bldy, in, start, end, a, name);
  run_bright_w<bt, 2>(pal, bldy, in, start, end, a, name);
#if VM_VEC
  run_bright_w<bt, 3>(pal, bldy, in, start, end, a, name);
#endif
}

// All five blend instantiations at once.
#define ALL_BLEND(pal, alpha, bldy, src, s, e)                        \
  do {                                                                \
    run_blend<OBJ_BLEND,   true >(pal, alpha, bldy, src, s, e, "OBJ_BLEND/st");   \
    run_blend<BLEND_ONLY,  true >(pal, alpha, bldy, src, s, e, "BLEND_ONLY/st");  \
    run_blend<BLEND_ONLY,  false>(pal, alpha, bldy, src, s, e, "BLEND_ONLY");     \
    run_blend<BLEND_BRIGHT,true >(pal, alpha, bldy, src, s, e, "BLEND_BRIGHT/st");\
    run_blend<BLEND_DARK,  true >(pal, alpha, bldy, src, s, e, "BLEND_DARK/st");  \
  } while (0)

static void test_channels(int fills)
{
  // channel 0=R(5 bits at 11) 1=G(6 at 5) 2=B(5 at 0)
  static const int bits[3] = { 5, 6, 5 }, shift[3] = { 11, 5, 0 };
  for (int fill = 0; fill < fills; fill++)
    for (int ch = 0; ch < 3; ch++) {
      const u32 n = 1u << bits[ch];
      u16 pal[512];
      u32 src[256];
      for (u32 a = 0; a <= 16; a++)
        for (u32 b = 0; b <= 16; b++) {
          // n*n combinations, 128 per palette (two entries each)
          u32 total = n * n;
          for (u32 base = 0; base < total; base += 128) {
            for (u32 j = 0; j < 128; j++) {
              u32 k = base + j;
              u32 c1 = k / n, c2 = k % n;
              u32 o1 = rnd() & 0xFFFF, o2 = rnd() & 0xFFFF;
              if (fill == 1) { o1 = 0xFFFF; o2 = 0x0000; }
              if (fill == 2) { o1 = 0x0000; o2 = 0xFFFF; }
              u32 m = ((1u << bits[ch]) - 1) << shift[ch];
              pal[2 * j]     = (o1 & ~m) | (c1 << shift[ch]);
              pal[2 * j + 1] = (o2 & ~m) | (c2 << shift[ch]);
              // both flags of both layers set: blend and brighten both apply
              src[j] = (2 * j) | 0xA00 | (((2 * j + 1) | 0x400) << 16);
            }
            for (u32 j = 128; j < 256; j++) src[j] = src[j - 128];
            ALL_BLEND(pal, a | (b << 8), (a + b) & 0x1F, src, 0, 128);
            // misaligned start/end of the same
            ALL_BLEND(pal, a | (b << 8), (a * 3 + b) & 0x1F, src, 3, 128 - 5);
          }
        }
    }
  printf("  channel pairs x coefficients: ok\n");
}

static void test_brightness(bool quick)
{
  u16 pal[512], in[256];
  for (u32 base = 0; base < 65536; base += quick ? 512 * 17 : 512) {
    for (u32 j = 0; j < 512; j++) pal[j] = base + j;
    for (u32 f = 0; f < 2; f++) {
      for (u32 j = 0; j < 256; j++)
        in[j] = (u16)((2 * j + (rnd() & 1)) | (f ? 0x200 : 0) | ((rnd() & 3) << 14));
      for (u32 y = 0; y < 32; y++) {
        run_bright<BLEND_BRIGHT>(pal, y, in, 0, 256, "bright");
        run_bright<BLEND_DARK>(pal, y, in, 0, 256, "dark");
      }
    }
    // every index with the flag set and clear, mixed in the same line
    for (u32 j = 0; j < 256; j++) in[j] = (u16)((2 * j) | ((j & 1) ? 0x200 : 0));
    run_bright<BLEND_BRIGHT>(pal, 9, in, 1, 255, "bright mixed");
    run_bright<BLEND_DARK>(pal, 5, in, 2, 254, "dark mixed");
  }
  printf("  brightness, all colours x BLDY 0..31: ok\n");
}

static void test_random(unsigned iters)
{
  u16 pal[512];
  u32 src[256];
  u16 in[256];
  for (unsigned it = 0; it < iters; it++) {
    for (u32 j = 0; j < 512; j++) pal[j] = rnd();
    // a flag mix: each pixel draws its own, with junk bits
    u32 mode = rnd() % 4;
    for (u32 j = 0; j < 256; j++) {
      u32 lo = rnd() & 0xFFFF, hi = rnd() & 0xFFFF;
      if (mode == 1) { lo |= 0xA00; hi |= 0x400; }                  // all blend
      if (mode == 2) { lo &= ~0xE00u; hi &= ~0x0400u; }             // none
      if (mode == 3) { lo = (lo & ~0xE00u) | ((rnd() & 1) ? 0x200 : 0); }
      src[j] = lo | (hi << 16);
      in[j] = (u16)(lo & 0xFFFF);
    }
    u32 alpha = rnd() & 0xFFFF, bldy = rnd() & 0xFFFF;
    if (it & 1) { alpha = (rnd() % 17) | ((rnd() % 17) << 8); bldy = rnd() % 17; }
    u32 start = it % 10;
    for (u32 len = 0; len <= 40; len++) {
      ALL_BLEND(pal, alpha, bldy, src, start, start + len);
      run_bright<BLEND_BRIGHT>(pal, bldy, in, start, start + len, "bright");
      run_bright<BLEND_DARK>(pal, bldy, in, start, start + len, "dark");
    }
    // a full line
    ALL_BLEND(pal, alpha, bldy, src, 0, 240 > 256 ? 256 : 240);
    run_bright<BLEND_BRIGHT>(pal, bldy, in, 0, 240, "bright line");
  }
  printf("  random lines, starts 0..9 x lengths 0..40, guards: ok\n");
}

int main(int argc, char **argv)
{
  bool quick = argc > 1 && !strcmp(argv[1], "quick");
  printf("test_video_merge: vector unit: %s%s\n",
#if VM_VEC
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
         "NEON",
#else
         "SSE2",
#endif
#else
         "none (scalar only)",
#endif
         quick ? " (quick)" : "");
  test_channels(quick ? 1 : 3);
  test_brightness(quick);
  test_random(quick ? 300 : 4000);
  printf("OK (%llu pixels merged per implementation)\n", n_px);
  return 0;
}
