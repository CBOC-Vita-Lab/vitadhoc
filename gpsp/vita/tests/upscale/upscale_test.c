/* upscale_test.c -- upscale_vita.c against itself, on a computer.
 *
 * A picture that scrolls (every direction, by up to 3 pixels, a still
 * status bar on top, a sprite that moves its own way, pauses) is handed to
 * the upscalers frame after frame, the way video_vita.c does (two textures,
 * the one not on screen written).  Each finished texture must equal the
 * whole picture scaled from nothing (xbrz_c_scale / Scale2x-3x on all of
 * it): whatever was skipped, slid or copied, the result is the same.  Every
 * slide mode (0 none, 1 moved in place, 2 the helpers copy), every factor,
 * both filters, 240 and 282 wide; GBADHOC_UPSCALE_ASYNC=1 runs the helpers
 * the console's way (the picture collected later).
 *
 *    make -C tests/upscale && ./tests/upscale/upscale_test
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "upscale_vita.h"

void xbrz_c_init(void);
void xbrz_c_scale(int factor, const uint32_t *src, uint32_t *dst,
                  int w, int h, int y0, int y1);

unsigned pspcompat_time_us(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (unsigned)(ts.tv_sec * 1000000u + ts.tv_nsec / 1000u);
}

#define WORLD 512
static uint16_t g_world[WORLD * WORLD];

static uint32_t rng = 12345;
static uint32_t rnd(void) { rng = rng * 1103515245u + 12345u; return rng >> 8; }

/* Tiles a scaler has opinions about: diagonals, steps, dots, flat. */
static void world_make(void)
{
   static const uint16_t pal[8] = { 0x0000, 0xFFFF, 0xF800, 0x07E0, 0x001F,
                                    0x7BEF, 0xFFE0, 0x8410 };
   int tx, ty, x, y;
   for (ty = 0; ty < WORLD / 8; ty++)
      for (tx = 0; tx < WORLD / 8; tx++)
      {
         int kind = (int)(rnd() % 5);
         uint16_t a = pal[rnd() % 8], b = pal[rnd() % 8];
         for (y = 0; y < 8; y++)
            for (x = 0; x < 8; x++)
            {
               uint16_t c;
               switch (kind)
               {
               case 0: c = x > y ? a : b; break;
               case 1: c = (x + y) & 4 ? a : b; break;
               case 2: c = (x == 3 && y == 4) ? a : b; break;
               case 3: c = (x * x + y * y) < 30 ? a : b; break;
               default: c = a; break;
               }
               g_world[(ty * 8 + y) * WORLD + tx * 8 + x] = c;
            }
      }
}

static void frame_make(uint16_t *pic, int w, int h, int cx, int cy, int sx,
                       int sy, int frame)
{
   int x, y;
   for (y = 0; y < h; y++)
      for (x = 0; x < w; x++)
      {
         uint16_t c;
         if (y < 12)                       /* a status bar that never moves */
            c = (uint16_t)(((x / 6) & 1) ? 0x2104 : (x > y * 3 ? 0xFFFF : 0x001F));
         else
            c = g_world[((cy + y) & (WORLD - 1)) * WORLD +
                        ((cx + x) & (WORLD - 1))];
         /* a sprite, moving its own way */
         if (x >= sx && x < sx + 16 && y >= sy && y < sy + 16 &&
             ((x - sx) - (y - sy)) * ((x - sx) + (y - sy) - 15) <= 0)
            c = (uint16_t)(0xF81F ^ (frame & 1 ? 0 : 0x0841));
         pic[y * w + x] = c;
      }
}

static uint32_t lut0(uint16_t v)
{
   unsigned hi = (v >> 11) & 31, g6 = (v >> 5) & 63, lo = v & 31;
   unsigned h8 = (hi << 3) | (hi >> 2), g8 = (g6 << 2) | (g6 >> 4);
   unsigned l8 = (lo << 3) | (lo >> 2);
   return 0xFF000000u | (l8 << 16) | (g8 << 8) | h8;
}

/* Scale2x/3x on the whole picture, as upscale_vita.c's epx_rows. */
static void epx_ref(const uint16_t *src, int w, int h, int f, uint32_t *dst)
{
   const size_t dp = (size_t)f * w;
   int x, y;
   for (y = 0; y < h; y++)
      for (x = 0; x < w; x++)
      {
         const uint16_t *rm = src + w * y;
         const uint16_t *ru = y > 0 ? rm - w : rm, *rd = y < h - 1 ? rm + w : rm;
         const int xl = x > 0 ? x - 1 : x, xr = x < w - 1 ? x + 1 : x;
         const uint16_t B = ru[x], D = rm[xl], E = rm[x], F = rm[xr], H = rd[x];
         uint32_t *o = dst + dp * f * y + f * x;
         if (f == 2)
         {
            uint16_t e0 = E, e1 = E, e2 = E, e3 = E;
            if (B != H && D != F)
            {
               if (D == B) e0 = D;
               if (B == F) e1 = F;
               if (D == H) e2 = D;
               if (H == F) e3 = F;
            }
            o[0] = lut0(e0); o[1] = lut0(e1); o[dp] = lut0(e2); o[dp + 1] = lut0(e3);
         }
         else
         {
            const uint16_t A = ru[xl], C = ru[xr], G = rd[xl], I = rd[xr];
            uint16_t e[9];
            int i;
            for (i = 0; i < 9; i++) e[i] = E;
            if (B != H && D != F)
            {
               if (D == B) e[0] = D;
               if ((D == B && E != C) || (B == F && E != A)) e[1] = B;
               if (B == F) e[2] = F;
               if ((D == B && E != G) || (D == H && E != A)) e[3] = D;
               if ((B == F && E != I) || (H == F && E != C)) e[5] = F;
               if (D == H) e[6] = D;
               if ((D == H && E != I) || (H == F && E != G)) e[7] = H;
               if (H == F) e[8] = F;
            }
            o[0] = lut0(e[0]); o[1] = lut0(e[1]); o[2] = lut0(e[2]);
            o[dp] = lut0(e[3]); o[dp + 1] = lut0(e[4]); o[dp + 2] = lut0(e[5]);
            o[2 * dp] = lut0(e[6]); o[2 * dp + 1] = lut0(e[7]); o[2 * dp + 2] = lut0(e[8]);
         }
      }
}

static int run(int kind, int f, int w, int mode, int frames, int async)
{
   const int h = 160;
   const size_t big = (size_t)f * w * f * h;
   const size_t pitch = (size_t)f * w * 4 + 64;  /* a texture's own pitch */
   uint16_t *pic = malloc((size_t)w * h * 2);
   uint32_t *s32 = malloc((size_t)w * h * 4), *ref = malloc(big * 4);
   unsigned char *tex[2] = { calloc(pitch * f * h, 1), calloc(pitch * f * h, 1) };
   void *out[2] = { tex[0], tex[1] };
   int front = -1, pending = 0, fr, bad = 0, slid = 0;
   int pox = 0, poy = 0, fox = 0, foy = 0;   /* the ring's turn: pending, front */
   int pf = f, ff = f, small = 0;            /* factors: pending, front */
   int fade_small = 0, after_big = 0;
   int cx = 100, cy = 100, vx = 0, vy = 0, sx = 60, sy = 60;
   upscale_stats_t st;

   upscale_forget(0);
   upscale_slide_mode(mode);
   upscale_stats(&st);
   for (fr = 0; fr < frames; fr++)
   {
      int i, y, target;
      /* a new heading now and then, pauses included */
      if (fr % 17 == 0)
      {
         vx = (int)(rnd() % 7) - 3;
         vy = (int)(rnd() % 7) - 3;
         if (rnd() % 4 == 0)
            vx = vy = 0;
      }
      cx += vx;
      cy += vy;
      sx = 40 + (int)(rnd() % 3) + (fr % 50);
      sy = 50 + (fr % 30);
      frame_make(pic, w, h, cx, cy, sx, sy, fr);
      /* frames 60-89: a fade (every pixel a little darker each frame, so
       * the whole picture is new and nothing slid) */
      if (fr >= 60 && fr < 90)
         for (i = 0; i < w * h; i++)
         {
            const int d = fr - 59;
            const int r = (pic[i] >> 11) & 31, g = (pic[i] >> 5) & 63,
                      b = pic[i] & 31;
            pic[i] = (uint16_t)(((r * (32 - d) / 32) << 11) |
                                ((g * (32 - d) / 32) << 5) | (b * (32 - d) / 32));
         }

      target = front == 0 ? 1 : 0;
      if (upscale_collect(0))
         front = pending;
      target = front == 0 ? 1 : 0;
      if (upscale_submit(0, kind, f, pic, (unsigned)w, (unsigned)h,
                         (size_t)w * 2, 0, out, pitch, target) != 1)
      {
         fprintf(stderr, "submit refused\n");
         return 1;
      }
      pending = target;
      pf = upscale_job_factor(0);
      upscale_ring(0, &pox, &poy);
      if (pf > f || pf < 2 || (pf < f - 1 && f != 4))
      {
         fprintf(stderr, "  frame %d: factor %d for %d\n", fr, pf, f);
         bad++;
      }
      small += pf < f;
      fade_small += fr >= 60 && fr < 90 && pf < f;
      after_big |= fr >= 120 && pf == f;
      if (async)
         while (!upscale_collect(0))
            upscale_help(1000);
      else if (!upscale_collect(0))
      {
         fprintf(stderr, "a twin's picture was not finished\n");
         return 1;
      }
      front = pending;
      fox = pox;
      foy = poy;
      ff = pf;

      for (i = 0; i < w * h; i++)
         s32[i] = lut0(pic[i]);
      if (kind == UPSCALE_XBRZ)
         xbrz_c_scale(ff, s32, ref, w, h, 0, h);
      else
         epx_ref(pic, w, h, ff, ref);
      for (y = 0; y < ff * h; y++)
      {
         /* the texture holds the picture as a ring turned by (fox, foy), at
          * its own factor in the top left of the texture */
         const unsigned char *row = tex[front] + pitch * ((y + foy * ff) % (ff * h));
         const uint32_t *rr = ref + (size_t)ff * w * y;
         const size_t a = (size_t)(w - fox) * ff;   /* columns before the seam */
         if (memcmp(row + (size_t)fox * ff * 4, rr, a * 4) ||
             memcmp(row, rr + a, (size_t)fox * ff * 4))
         {
            if (bad < 3)
               fprintf(stderr, "  frame %d row %d differs (kind %d f %d w %d mode %d)\n",
                       fr, y, kind, f, w, mode);
            bad++;
            break;
         }
      }
      if (fr % 50 == 49)
      {
         upscale_stats(&st);
         slid += (int)st.slid_pct;
      }
   }
   upscale_stats(&st);
   slid += (int)st.slid_pct;
   printf("%-5s f=%d w=%d mode=%d async=%d: %d frames, %d wrong, slid %s, "
          "%d at a smaller factor\n",
          kind == UPSCALE_XBRZ ? "xbrz" : "scale", f, w, mode, async, frames,
          bad, slid ? "yes" : "no", small);
   free(pic); free(s32); free(ref); free(tex[0]); free(tex[1]);
   /* the fade: every one of its 4x pictures at 2x, and 4x again some time
    * after it (with the slide found; without, every step of the walk is a
    * whole new picture too); no other factor ever changes */
   /* (helpers: a slow machine also steps down for time; the factor is
    * then checked for its range only) */
   if (!async && frames >= 130 && kind == UPSCALE_XBRZ && f == 4 &&
       (fade_small != 30 || (mode && !after_big)))
   {
      fprintf(stderr, "  the fade: %d of its 30 pictures smaller, 4x again "
              "after it: %s\n", fade_small, after_big ? "yes" : "no");
      return 1;
   }
   if (!async && frames >= 130 && !(kind == UPSCALE_XBRZ && f == 4) && small)
      return 1;
   return bad != 0 || (mode && !slid && frames >= 100);
}

int main(int argc, char **argv)
{
   int fails = 0, mode, f, wi, async = getenv("GBADHOC_UPSCALE_ASYNC") != NULL;
   const int widths[2] = { 240, 282 };
   int frames = argc > 1 ? atoi(argv[1]) : 200;
   world_make();
   xbrz_c_init();
   for (mode = 0; mode <= 2; mode++)
      for (wi = 0; wi < 2; wi++)
      {
         for (f = 2; f <= 4; f++)
            fails += run(UPSCALE_XBRZ, f, widths[wi], mode, frames, async);
         for (f = 2; f <= 3; f++)
            fails += run(UPSCALE_SCALE, f, widths[wi], mode, frames, async);
      }
   upscale_shutdown();
   printf("%s: %d failed\n", fails ? "FAIL" : "ok", fails);
   return fails != 0;
}
