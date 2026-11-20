/* mmpx_vita.c -- see mmpx_vita.h.
 *
 * The rules below are Morgan McGuire's and Mara Gagiu's MMPX
 * (https://casual-effects.com/research/McGuire2021PixelArt/index.html,
 * reference C port https://github.com/ITotalJustice/mmpx):
 *
 *    Copyright 2020 Morgan McGuire & Mara Gagiu.
 *    Available under the MIT license.
 *
 * What is different here, and why:
 *  - The picture is RGB565, compared as 16-bit values (two colours are equal
 *    in the reference's RGBA8 exactly when their 565 values are: the
 *    expansion is one to one).  The luma the rules order colours by comes
 *    from a 64 K table made the reference's way (R + G + B + 1 of the
 *    expanded 8-bit channels), so every decision is the reference's.
 *  - The reference clamps every read to the picture.  Here the rows of a
 *    piece are first copied into a small buffer with a border of 3 pixels on
 *    every side (the farthest a rule looks), the edge pixel repeated, so the
 *    inner loop has no bounds work at all.
 *  - Most of a game's picture is flat: when the 8 neighbours of a pixel are
 *    all equal to it, the answer is the pixel itself four times, and the 5x5
 *    reads, the luma lookups and every rule are skipped.  Nothing a smart
 *    scaler does to a flat area differs from copying it.
 *  - The output goes through the caller's 565 -> RGBA8 table straight into
 *    the 2x block; the flat case looks it up once.
 */
#include "mmpx_vita.h"
#include "upscale_vita.h"

#include <string.h>

#if (defined(__ARM_NEON) || defined(__ARM_NEON__)) && !defined(MMPX_NO_NEON)
#include <arm_neon.h>
#define MM_NEON 1
#endif

/* The widest and tallest piece done in one go; the border is on top. */
#define MM_PAD 3
#define MM_BAND 8
#define MM_SPAN 320

/* tests: 1 = the plain C loop even where NEON is built in (the NEON path is
 * checked against it, pixel for pixel) */
static int g_mm_c_only;
void mmpx_test_c_only(int on) { g_mm_c_only = on; }

static uint16_t g_mm_luma[65536];
static int g_mm_luma_ready;

static void mm_luma_build(void)
{
   unsigned v;
   for (v = 0; v < 65536; v++)
   {
      const unsigned r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
      const unsigned r8 = (r << 3) | (r >> 2), g8 = (g << 2) | (g >> 4),
                     b8 = (b << 3) | (b >> 2);
      /* (the reference multiplies by 256 - alpha, a constant for an opaque
       * picture: the order of two lumas is the same) */
      g_mm_luma[v] = (uint16_t)(r8 + g8 + b8 + 1);
   }
   __atomic_store_n(&g_mm_luma_ready, 1, __ATOMIC_RELEASE);
}

#define MM_EQ3(B, A0, A1, A2) ((((B) ^ (A0)) | ((B) ^ (A1)) | ((B) ^ (A2))) == 0)
#define MM_EQ2(B, A0, A1) ((((B) ^ (A0)) | ((B) ^ (A1))) == 0)
#define MM_EQ4(B, A0, A1, A2, A3) \
   ((((B) ^ (A0)) | ((B) ^ (A1)) | ((B) ^ (A2)) | ((B) ^ (A3))) == 0)
#define MM_ANY3(B, A0, A1, A2) ((B) == (A0) || (B) == (A1) || (B) == (A2))
#define MM_NONE2(B, A0, A1) ((B) != (A0) && (B) != (A1))
#define MM_NONE4(B, A0, A1, A2, A3) \
   ((B) != (A0) && (B) != (A1) && (B) != (A2) && (B) != (A3))

/* One pixel whose neighbourhood is not flat: the reference's rules.  r[k]
 * is the buffer row y + k - 3 (k = 0..6), positioned so that r[k][x] is the
 * pixel at column x. */
static void mmpx_pixel(const uint16_t *const r[7], int x, uint16_t out[4])
{
   const uint16_t A = r[2][x - 1], B = r[2][x], C = r[2][x + 1];
   const uint16_t D = r[3][x - 1], E = r[3][x], F = r[3][x + 1];
   const uint16_t G = r[4][x - 1], H = r[4][x], I = r[4][x + 1];
   const uint16_t Q = r[3][x - 2], R = r[3][x + 2];
   const uint16_t P = r[1][x], S = r[5][x];
   /* the luma lookups are lazy: only a rule whose cheap tests pass reads one */
#define Bl g_mm_luma[B]
#define Dl g_mm_luma[D]
#define El g_mm_luma[E]
#define Fl g_mm_luma[F]
#define Hl g_mm_luma[H]
   uint16_t J = E, K = E, L = E, M = E;

   /* 1:1 slope rules */
   if ((D == B && D != H && D != F) && (El >= Dl || E == A) &&
       MM_ANY3(E, A, C, G) && ((El < Dl) || A != D || E != P || E != Q))
      J = D;
   if ((B == F && B != D && B != H) && (El >= Bl || E == C) &&
       MM_ANY3(E, A, C, I) && ((El < Bl) || C != B || E != P || E != R))
      K = B;
   if ((H == D && H != F && H != B) && (El >= Hl || E == G) &&
       MM_ANY3(E, A, G, I) && ((El < Hl) || G != H || E != S || E != Q))
      L = H;
   if ((F == H && F != B && F != D) && (El >= Fl || E == I) &&
       MM_ANY3(E, C, G, I) && ((El < Fl) || I != H || E != R || E != S))
      M = F;

   /* Intersection rules */
   if ((E != F && MM_EQ4(E, C, I, D, Q) && MM_EQ2(F, B, H)) &&
       (F != r[3][x + 3]))
      K = M = F;
   if ((E != D && MM_EQ4(E, A, G, F, R) && MM_EQ2(D, B, H)) &&
       (D != r[3][x - 3]))
      J = L = D;
   if ((E != H && MM_EQ4(E, G, I, B, P) && MM_EQ2(H, D, F)) &&
       (H != r[6][x]))
      L = M = H;
   if ((E != B && MM_EQ4(E, A, C, H, S) && MM_EQ2(B, D, F)) &&
       (B != r[0][x]))
      J = K = B;
   if (Bl < El && MM_EQ4(E, G, H, I, S) && MM_NONE4(E, A, D, C, F))
      J = K = B;
   if (Hl < El && MM_EQ4(E, A, B, C, P) && MM_NONE4(E, D, G, I, F))
      L = M = H;
   if (Fl < El && MM_EQ4(E, A, D, G, Q) && MM_NONE4(E, B, C, I, H))
      K = M = F;
   if (Dl < El && MM_EQ4(E, C, F, I, R) && MM_NONE4(E, B, A, G, H))
      J = L = D;

   /* 2:1 slope rules */
   if (H != B)
   {
      if (H != A && H != E && H != C)
      {
         if (MM_EQ3(H, G, F, R) && MM_NONE2(H, D, r[2][x + 2]))
            L = M;
         if (MM_EQ3(H, I, D, Q) && MM_NONE2(H, F, r[2][x - 2]))
            M = L;
      }
      if (B != I && B != G && B != E)
      {
         if (MM_EQ3(B, A, F, R) && MM_NONE2(B, D, r[4][x + 2]))
            J = K;
         if (MM_EQ3(B, C, D, Q) && MM_NONE2(B, F, r[4][x - 2]))
            K = J;
      }
   }
   if (F != D)
   {
      if (D != I && D != E && D != C)
      {
         if (MM_EQ3(D, A, H, S) && MM_NONE2(D, B, r[5][x + 1]))
            J = L;
         if (MM_EQ3(D, G, B, P) && MM_NONE2(D, H, r[1][x + 1]))
            L = J;
      }
      if (F != E && F != A && F != G)
      {
         if (MM_EQ3(F, C, H, S) && MM_NONE2(F, B, r[5][x - 1]))
            K = M;
         if (MM_EQ3(F, I, B, P) && MM_NONE2(F, H, r[1][x - 1]))
            M = K;
      }
   }
#undef Bl
#undef Dl
#undef El
#undef Fl
#undef Hl
   out[0] = J;
   out[1] = K;
   out[2] = L;
   out[3] = M;
}

#ifdef MM_NEON
/* The same rules as mmpx_pixel, for eight pixels in a row at once (lane k is
 * the pixel at column x + k).  Every rule is a compare and a select, so
 * the eight run through them together; the order the rules are applied in
 * (a later one overrides an earlier one) is the scalar function's, and the
 * luma is computed from the 565 value the way mm_luma_build does, so each
 * lane's answer is the scalar one. */
#define MV(p) vld1q_u16(p)
#define MEQ(a, b) vceqq_u16((a), (b))
#define MNE(a, b) vmvnq_u16(vceqq_u16((a), (b)))
#define MAND(a, b) vandq_u16((a), (b))
#define MOR(a, b) vorrq_u16((a), (b))
#define MSEL(m, a, b) vbslq_u16((m), (a), (b))
#define MANY3(b, a0, a1, a2) MOR(MOR(MEQ(b, a0), MEQ(b, a1)), MEQ(b, a2))
#define MEQ2(b, a0, a1) MAND(MEQ(b, a0), MEQ(b, a1))
#define MEQ3(b, a0, a1, a2) MAND(MAND(MEQ(b, a0), MEQ(b, a1)), MEQ(b, a2))
#define MEQ4(b, a0, a1, a2, a3) MAND(MEQ3(b, a0, a1, a2), MEQ(b, a3))
#define MNONE2(b, a0, a1) MAND(MNE(b, a0), MNE(b, a1))
#define MNONE4(b, a0, a1, a2, a3) MAND(MNONE2(b, a0, a1), MNONE2(b, a2, a3))

static inline uint16x8_t mm_luma8(uint16x8_t v)
{
   const uint16x8_t r = vshrq_n_u16(v, 11);
   const uint16x8_t g = vandq_u16(vshrq_n_u16(v, 5), vdupq_n_u16(63));
   const uint16x8_t b = vandq_u16(v, vdupq_n_u16(31));
   const uint16x8_t r8 = vorrq_u16(vshlq_n_u16(r, 3), vshrq_n_u16(r, 2));
   const uint16x8_t g8 = vorrq_u16(vshlq_n_u16(g, 2), vshrq_n_u16(g, 4));
   const uint16x8_t b8 = vorrq_u16(vshlq_n_u16(b, 3), vshrq_n_u16(b, 2));
   return vaddq_u16(vaddq_u16(vaddq_u16(r8, g8), b8), vdupq_n_u16(1));
}

static void mmpx_pixel8(const uint16_t *const r[7], int x, uint16x8_t out[4])
{
   const uint16x8_t A = MV(r[2] + x - 1), B = MV(r[2] + x), C = MV(r[2] + x + 1);
   const uint16x8_t D = MV(r[3] + x - 1), E = MV(r[3] + x), F = MV(r[3] + x + 1);
   const uint16x8_t G = MV(r[4] + x - 1), H = MV(r[4] + x), I = MV(r[4] + x + 1);
   const uint16x8_t Q = MV(r[3] + x - 2), R = MV(r[3] + x + 2);
   const uint16x8_t P = MV(r[1] + x), S = MV(r[5] + x);
   const uint16x8_t Bl = mm_luma8(B), Dl = mm_luma8(D), El = mm_luma8(E),
                    Fl = mm_luma8(F), Hl = mm_luma8(H);
   uint16x8_t J = E, K = E, L = E, M = E, c;

   /* 1:1 slope rules */
   c = MAND(MAND(MAND(MEQ(D, B), MNE(D, H)), MNE(D, F)),
            MAND(MAND(MOR(vcgeq_u16(El, Dl), MEQ(E, A)), MANY3(E, A, C, G)),
                 MOR(MOR(MOR(vcltq_u16(El, Dl), MNE(A, D)), MNE(E, P)), MNE(E, Q))));
   J = MSEL(c, D, J);
   c = MAND(MAND(MAND(MEQ(B, F), MNE(B, D)), MNE(B, H)),
            MAND(MAND(MOR(vcgeq_u16(El, Bl), MEQ(E, C)), MANY3(E, A, C, I)),
                 MOR(MOR(MOR(vcltq_u16(El, Bl), MNE(C, B)), MNE(E, P)), MNE(E, R))));
   K = MSEL(c, B, K);
   c = MAND(MAND(MAND(MEQ(H, D), MNE(H, F)), MNE(H, B)),
            MAND(MAND(MOR(vcgeq_u16(El, Hl), MEQ(E, G)), MANY3(E, A, G, I)),
                 MOR(MOR(MOR(vcltq_u16(El, Hl), MNE(G, H)), MNE(E, S)), MNE(E, Q))));
   L = MSEL(c, H, L);
   c = MAND(MAND(MAND(MEQ(F, H), MNE(F, B)), MNE(F, D)),
            MAND(MAND(MOR(vcgeq_u16(El, Fl), MEQ(E, I)), MANY3(E, C, G, I)),
                 MOR(MOR(MOR(vcltq_u16(El, Fl), MNE(I, H)), MNE(E, R)), MNE(E, S))));
   M = MSEL(c, F, M);

   /* Intersection rules */
   c = MAND(MAND(MNE(E, F), MEQ4(E, C, I, D, Q)), MAND(MEQ2(F, B, H), MNE(F, MV(r[3] + x + 3))));
   K = MSEL(c, F, K); M = MSEL(c, F, M);
   c = MAND(MAND(MNE(E, D), MEQ4(E, A, G, F, R)), MAND(MEQ2(D, B, H), MNE(D, MV(r[3] + x - 3))));
   J = MSEL(c, D, J); L = MSEL(c, D, L);
   c = MAND(MAND(MNE(E, H), MEQ4(E, G, I, B, P)), MAND(MEQ2(H, D, F), MNE(H, MV(r[6] + x))));
   L = MSEL(c, H, L); M = MSEL(c, H, M);
   c = MAND(MAND(MNE(E, B), MEQ4(E, A, C, H, S)), MAND(MEQ2(B, D, F), MNE(B, MV(r[0] + x))));
   J = MSEL(c, B, J); K = MSEL(c, B, K);
   c = MAND(MAND(vcltq_u16(Bl, El), MEQ4(E, G, H, I, S)), MNONE4(E, A, D, C, F));
   J = MSEL(c, B, J); K = MSEL(c, B, K);
   c = MAND(MAND(vcltq_u16(Hl, El), MEQ4(E, A, B, C, P)), MNONE4(E, D, G, I, F));
   L = MSEL(c, H, L); M = MSEL(c, H, M);
   c = MAND(MAND(vcltq_u16(Fl, El), MEQ4(E, A, D, G, Q)), MNONE4(E, B, C, I, H));
   K = MSEL(c, F, K); M = MSEL(c, F, M);
   c = MAND(MAND(vcltq_u16(Dl, El), MEQ4(E, C, F, I, R)), MNONE4(E, B, A, G, H));
   J = MSEL(c, D, J); L = MSEL(c, D, L);

   /* 2:1 slope rules (each one reads the values the earlier ones left) */
   {
      const uint16x8_t hb = MNE(H, B), fd = MNE(F, D);
      uint16x8_t g1, c1;
      g1 = MAND(hb, MAND(MAND(MNE(H, A), MNE(H, E)), MNE(H, C)));
      c1 = MAND(g1, MAND(MEQ3(H, G, F, R), MNONE2(H, D, MV(r[2] + x + 2))));
      L = MSEL(c1, M, L);
      c1 = MAND(g1, MAND(MEQ3(H, I, D, Q), MNONE2(H, F, MV(r[2] + x - 2))));
      M = MSEL(c1, L, M);
      g1 = MAND(hb, MAND(MAND(MNE(B, I), MNE(B, G)), MNE(B, E)));
      c1 = MAND(g1, MAND(MEQ3(B, A, F, R), MNONE2(B, D, MV(r[4] + x + 2))));
      J = MSEL(c1, K, J);
      c1 = MAND(g1, MAND(MEQ3(B, C, D, Q), MNONE2(B, F, MV(r[4] + x - 2))));
      K = MSEL(c1, J, K);
      g1 = MAND(fd, MAND(MAND(MNE(D, I), MNE(D, E)), MNE(D, C)));
      c1 = MAND(g1, MAND(MEQ3(D, A, H, S), MNONE2(D, B, MV(r[5] + x + 1))));
      J = MSEL(c1, L, J);
      c1 = MAND(g1, MAND(MEQ3(D, G, B, P), MNONE2(D, H, MV(r[1] + x + 1))));
      L = MSEL(c1, J, L);
      g1 = MAND(fd, MAND(MAND(MNE(F, E), MNE(F, A)), MNE(F, G)));
      c1 = MAND(g1, MAND(MEQ3(F, C, H, S), MNONE2(F, B, MV(r[5] + x - 1))));
      K = MSEL(c1, M, K);
      c1 = MAND(g1, MAND(MEQ3(F, I, B, P), MNONE2(F, H, MV(r[1] + x - 1))));
      M = MSEL(c1, K, M);
   }
   out[0] = J;
   out[1] = K;
   out[2] = L;
   out[3] = M;
}
#endif

/* Rows [ya, yb) (at most MM_BAND), columns [xa, xb) (at most MM_SPAN). */
static void mmpx_band(const uint16_t *src, int w, int h, const uint32_t *lut,
                      uint32_t *at, size_t dp, int y0, int ya, int yb, int x0,
                      int xa, int xb)
{
   uint16_t buf[(MM_BAND + 2 * MM_PAD) * (MM_SPAN + 2 * MM_PAD)];
   const int bw = xb - xa + 2 * MM_PAD, nrows = yb - ya + 2 * MM_PAD;
   const uint16_t *rows[MM_BAND + 2 * MM_PAD];     /* each: the buffer row's start */
   int i, x, y;

   /* the rows, the border repeated: ya - 3 .. yb + 2, clamped to the picture */
   for (i = 0; i < nrows; i++)
   {
      int sy = ya - MM_PAD + i;
      uint16_t *d = buf + (size_t)bw * i;
      const uint16_t *s;
      int lo = xa - MM_PAD, hi = xb + MM_PAD, k;
      sy = sy < 0 ? 0 : sy > h - 1 ? h - 1 : sy;
      s = src + (size_t)w * (size_t)sy;
      rows[i] = d;
      for (k = lo; k < 0; k++)
         d[k - lo] = s[0];
      /* the part inside the picture, in one copy */
      {
         const int in0 = lo < 0 ? 0 : lo, in1 = hi > w ? w : hi;
         if (in1 > in0)
            memcpy(d + (in0 - lo), s + in0, (size_t)(in1 - in0) * 2u);
      }
      for (k = w; k < hi; k++)
         d[k - lo] = s[w - 1];
   }
   for (y = ya; y < yb; y++)
   {
      const uint16_t *const *r = rows + (y - ya);   /* r[0..6]: y-3 .. y+3 */
      uint32_t *o = at + dp * 2u * (size_t)(y - y0) + 2u * (size_t)(xa - x0);
      /* x is the column in the buffer: the picture's xa is at MM_PAD */
      x = MM_PAD;
#ifdef MM_NEON
      /* Eight pixels at a time: the flat test (the 8 neighbours all equal
       * the pixel) for the eight at once.  Eight flat pixels in a row are
       * one colour (each equals the next), so that case is one lookup and
       * four vector stores a row; otherwise all eight go through the rules
       * together (mmpx_pixel8; flat pixels come out as themselves) -- the
       * same answer for every pixel as the scalar loop below. */
      for (; !g_mm_c_only && x + 8 <= MM_PAD + (xb - xa); x += 8, o += 16)
      {
         const uint16x8_t e = vld1q_u16(r[3] + x);
         uint16x8_t f = vceqq_u16(e, vld1q_u16(r[2] + x - 1));
         int i;
         f = vandq_u16(f, vceqq_u16(e, vld1q_u16(r[2] + x)));
         f = vandq_u16(f, vceqq_u16(e, vld1q_u16(r[2] + x + 1)));
         f = vandq_u16(f, vceqq_u16(e, vld1q_u16(r[3] + x - 1)));
         f = vandq_u16(f, vceqq_u16(e, vld1q_u16(r[3] + x + 1)));
         f = vandq_u16(f, vceqq_u16(e, vld1q_u16(r[4] + x - 1)));
         f = vandq_u16(f, vceqq_u16(e, vld1q_u16(r[4] + x)));
         f = vandq_u16(f, vceqq_u16(e, vld1q_u16(r[4] + x + 1)));
         if ((vgetq_lane_u64(vreinterpretq_u64_u16(f), 0) &
              vgetq_lane_u64(vreinterpretq_u64_u16(f), 1)) == ~(uint64_t)0)
         {
            const uint32x4_t c = vdupq_n_u32(lut[r[3][x]]);
            vst1q_u32(o, c);
            vst1q_u32(o + 4, c);
            vst1q_u32(o + 8, c);
            vst1q_u32(o + 12, c);
            vst1q_u32(o + dp, c);
            vst1q_u32(o + dp + 4, c);
            vst1q_u32(o + dp + 8, c);
            vst1q_u32(o + dp + 12, c);
            continue;
         }
         {
            uint16x8_t v[4];
            uint16_t jj[4][8];
            mmpx_pixel8(r, x, v);
            vst1q_u16(jj[0], v[0]);
            vst1q_u16(jj[1], v[1]);
            vst1q_u16(jj[2], v[2]);
            vst1q_u16(jj[3], v[3]);
            for (i = 0; i < 8; i++)
            {
               uint32_t *q = o + 2 * i;
               q[0] = lut[jj[0][i]]; q[1] = lut[jj[1][i]];
               q[dp] = lut[jj[2][i]]; q[dp + 1] = lut[jj[3][i]];
            }
         }
      }
#endif
      for (; x < MM_PAD + (xb - xa); x++, o += 2)
      {
         const uint16_t E = r[3][x];
         /* flat: all 8 neighbours equal the pixel */
         if ((((r[2][x - 1] ^ E) | (r[2][x] ^ E) | (r[2][x + 1] ^ E) |
               (r[3][x - 1] ^ E) | (r[3][x + 1] ^ E) | (r[4][x - 1] ^ E) |
               (r[4][x] ^ E) | (r[4][x + 1] ^ E)) == 0))
         {
            const uint32_t c = lut[E];
            o[0] = c;
            o[1] = c;
            o[dp] = c;
            o[dp + 1] = c;
         }
         else
         {
            uint16_t q[4];
            mmpx_pixel(r, x, q);
            o[0] = lut[q[0]];
            o[1] = lut[q[1]];
            o[dp] = lut[q[2]];
            o[dp + 1] = lut[q[3]];
         }
      }
   }
}

void mmpx_rect(const uint16_t *src, int w, int h, const uint32_t *lut,
               uint32_t *at, size_t dp, int y0, int y1, int x0, int x1)
{
   int ya, xa;
   if (!src || !lut || !at || w < 1 || h < 1)
      return;
   /* (built by mmpx_set_* before any job runs; two helpers that both get here
    * first write the same values) */
   if (!__atomic_load_n(&g_mm_luma_ready, __ATOMIC_ACQUIRE))
      mm_luma_build();
   for (ya = y0; ya < y1; ya += MM_BAND)
   {
      const int yb = ya + MM_BAND < y1 ? ya + MM_BAND : y1;
      for (xa = x0; xa < x1; xa += MM_SPAN)
      {
         const int xb = xa + MM_SPAN < x1 ? xa + MM_SPAN : x1;
         mmpx_band(src, w, h, lut, at, dp, y0, ya, yb, x0, xa, xb);
      }
   }
}

/* ---- who asks for it ------------------------------------------------------ */

/* Set on the emulator's thread, read on the drawing thread: upscale_submit
 * reads them once a picture (mmpx_wanted) and the slot remembers the answer,
 * so nothing here touches a slot, and a change shows from the next picture
 * on, whole. */
static int g_mm_auto, g_mm_choice;

void mmpx_set_auto(int on)
{
   on = on ? 1 : 0;
   if (on)
      mm_luma_build();
   __atomic_store_n(&g_mm_auto, on, __ATOMIC_SEQ_CST);
}

void mmpx_set_choice(int on)
{
   on = on ? 1 : 0;
   if (on)
      mm_luma_build();
   __atomic_store_n(&g_mm_choice, on, __ATOMIC_SEQ_CST);
}

int mmpx_wanted(int kind)
{
   /* Auto's step is a 2x picture in the xBRZ slot; the hand choice replaces
    * the Scale2x of the Scale filter (its 3x stays Scale3x) */
   return (kind == UPSCALE_XBRZ && __atomic_load_n(&g_mm_auto, __ATOMIC_SEQ_CST)) ||
          (kind == UPSCALE_SCALE && __atomic_load_n(&g_mm_choice, __ATOMIC_SEQ_CST));
}
