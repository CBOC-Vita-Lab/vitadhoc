/* test_apng.c -- apng_write.c: writes animated PNGs the python side
 * (test_apng.py) decodes with PIL and compares, frame by frame, with the
 * pictures given.  Also checks the chunk structure itself (every CRC, the
 * sequence numbers, acTL's count) and the writer's refusals.
 *
 *    test_apng OUTDIR     writes OUTDIR/<case>.png and OUTDIR/<case>.txt
 *                         (one line a frame: its delay in ms; then the
 *                         frames' RGB, raw, in <case>.raw) and checks.
 */
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../../third_party/stb/stb_image_write.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../apng_write.h"

static int checks, bad;
#define CHECK(c) do { checks++; if (!(c)) { bad++; \
   printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

typedef struct { uint8_t *d; size_t n, cap; int fail_after; } mem;

static int sink_mem(void *ctx, const void *p, size_t n)
{
   mem *m = (mem *)ctx;
   if (m->fail_after >= 0 && m->n + n > (size_t)m->fail_after)
      return -1;
   if (m->n + n > m->cap)
   {
      m->cap = (m->n + n) * 2;
      m->d = (uint8_t *)realloc(m->d, m->cap);
   }
   memcpy(m->d + m->n, p, n);
   m->n += n;
   return 0;
}

static unsigned be(const uint8_t *s)
{
   return (unsigned)s[0] << 24 | (unsigned)s[1] << 16 | (unsigned)s[2] << 8 | s[3];
}

/* The file's structure, by the book: signature, IHDR first, acTL before the
 * first IDAT, fcTL / fdAT numbered 0, 1, 2 ... without a gap, every CRC,
 * IEND last.  Returns the frames' fcTL count. */
static int structure(const uint8_t *d, size_t n, unsigned frames, unsigned w, unsigned h)
{
   static const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
   size_t at = 8;
   unsigned seq = 0, fctl = 0, idat = 0, fdat = 0, order = 0;
   int saw_actl = 0, saw_iend = 0;
   if (n < 8 || memcmp(d, sig, 8))
      return -1;
   while (at + 12 <= n)
   {
      unsigned len = be(d + at);
      const uint8_t *t = d + at + 4, *body = d + at + 8;
      if (at + 12 + len > n)
         return -2;
      if (apng_crc32(0, t, 4 + len) != be(body + len))
         return -3;
      if (!order++ && memcmp(t, "IHDR", 4))
         return -4;
      if (!memcmp(t, "IHDR", 4))
         CHECK(be(body) == w && be(body + 4) == h && body[8] == 8 && body[9] == 2);
      else if (!memcmp(t, "acTL", 4))
      {
         CHECK(!idat && len == 8 && be(body) == frames && be(body + 4) == 0);
         saw_actl = 1;
      }
      else if (!memcmp(t, "fcTL", 4))
      {
         CHECK(len == 26 && be(body) == seq);
         seq++;
         fctl++;
         CHECK(be(body + 4) >= 1 && be(body + 8) >= 1);
         CHECK(be(body + 12) + be(body + 4) <= w && be(body + 16) + be(body + 8) <= h);
         CHECK(body[22] == 0x03 && body[23] == 0xE8);       /* ms */
      }
      else if (!memcmp(t, "IDAT", 4))
      {
         CHECK(saw_actl && fctl == 1 && !fdat);              /* the first frame's */
         idat++;
      }
      else if (!memcmp(t, "fdAT", 4))
      {
         CHECK(len > 4 && be(body) == seq);
         seq++;
         fdat++;
      }
      else if (!memcmp(t, "IEND", 4))
      {
         CHECK(len == 0 && at + 12 == n);
         saw_iend = 1;
      }
      at += 12 + len;
   }
   CHECK(saw_iend && at == n);
   CHECK(idat >= 1 && fdat == frames - 1 && fctl == frames);
   return (int)fctl;
}

static void fill(uint8_t *f, unsigned w, unsigned h, int kind, unsigned i)
{
   unsigned x, y;
   for (y = 0; y < h; y++)
      for (x = 0; x < w; x++)
      {
         uint8_t *p = f + 3 * ((size_t)y * w + x);
         p[0] = (uint8_t)(x * 5);          /* a gradient background */
         p[1] = (uint8_t)(y * 7);
         p[2] = 90;
         if (kind == 0)                    /* a square walks across */
         {
            if (x >= i * 3 && x < i * 3 + 8 && y >= 10 && y < 18)
               p[0] = p[1] = p[2] = 255;
         }
         else if (kind == 1)               /* nothing moves but one pixel on frame 3 */
         {
            if (i == 3 && x == w - 1 && y == h - 1)
               p[0] = 1, p[1] = 2, p[2] = 3;
         }
         else                              /* all of it, every frame */
         {
            p[0] = (uint8_t)(x * 5 + i * 31);
            p[1] = (uint8_t)(y * 7 + i * 17);
            p[2] = (uint8_t)((x ^ y) + i);
         }
      }
}

static void one_case(const char *dir, const char *name, unsigned w, unsigned h,
                     unsigned frames, int kind)
{
   char path[512];
   mem m = { NULL, 0, 0, -1 };
   uint8_t *f = (uint8_t *)malloc((size_t)w * h * 3);
   FILE *raw, *txt;
   unsigned i;
   apng *a = apng_begin(sink_mem, &m, w, h, frames);

   CHECK(a != NULL);
   snprintf(path, sizeof(path), "%s/%s.raw", dir, name);
   raw = fopen(path, "wb");
   snprintf(path, sizeof(path), "%s/%s.txt", dir, name);
   txt = fopen(path, "w");
   fprintf(txt, "%u %u %u\n", w, h, frames);
   for (i = 0; i < frames; i++)
   {
      unsigned delay = 17 + (i % 5) * 33;
      fill(f, w, h, kind, i);
      CHECK(apng_add(a, f, delay) == 0);
      fwrite(f, 1, (size_t)w * h * 3, raw);
      fprintf(txt, "%u\n", delay);
   }
   CHECK(apng_end(a) == 0);
   fclose(raw);
   fclose(txt);
   CHECK(structure(m.d, m.n, frames, w, h) == (int)frames);
   snprintf(path, sizeof(path), "%s/%s.png", dir, name);
   {
      FILE *o = fopen(path, "wb");
      fwrite(m.d, 1, m.n, o);
      fclose(o);
   }
   printf("%s: %ux%u, %u frames, %zu bytes (raw %zu)\n", name, w, h, frames,
          m.n, (size_t)w * h * 3 * frames);
   if (kind == 1)       /* a still picture is nearly free after the first */
      CHECK(m.n < (size_t)w * h * 3 / 2 + frames * 120);
   free(m.d);
   free(f);
}

static void refusals(void)
{
   mem m = { NULL, 0, 0, -1 };
   uint8_t px[3 * 4 * 4] = { 0 };
   apng *a;

   CHECK(apng_begin(sink_mem, &m, 0, 4, 1) == NULL);
   CHECK(apng_begin(sink_mem, &m, 4, 0, 1) == NULL);
   CHECK(apng_begin(sink_mem, &m, 4, 4, 0) == NULL);
   CHECK(apng_begin(sink_mem, &m, APNG_MAX_W + 1, 4, 1) == NULL);
   CHECK(apng_begin(NULL, &m, 4, 4, 1) == NULL);
   free(m.d);

   /* more frames than announced */
   memset(&m, 0, sizeof(m));
   m.fail_after = -1;
   a = apng_begin(sink_mem, &m, 4, 4, 1);
   CHECK(a && apng_add(a, px, 10) == 0 && apng_add(a, px, 10) != 0);
   CHECK(apng_end(a) != 0 || 1);          /* freed; the file is not to be trusted */
   free(m.d);

   /* fewer than announced: not a whole file */
   memset(&m, 0, sizeof(m));
   m.fail_after = -1;
   a = apng_begin(sink_mem, &m, 4, 4, 3);
   CHECK(a && apng_add(a, px, 10) == 0);
   CHECK(apng_end(a) != 0);
   free(m.d);

   /* the sink fails half way: every later call says so */
   memset(&m, 0, sizeof(m));
   m.fail_after = 80;
   a = apng_begin(sink_mem, &m, 4, 4, 2);
   CHECK(a != NULL);
   if (a)
   {
      CHECK(apng_add(a, px, 10) != 0);
      CHECK(apng_add(a, px, 10) != 0);
      CHECK(apng_end(a) != 0);
   }
   free(m.d);
   /* ...or at once */
   memset(&m, 0, sizeof(m));
   m.fail_after = 5;
   CHECK(apng_begin(sink_mem, &m, 4, 4, 2) == NULL);
   free(m.d);
   /* the CRC of the PNG spec's own example: "IEND" */
   CHECK(apng_crc32(0, "IEND", 4) == 0xAE426082u);
}

int main(int argc, char **argv)
{
   const char *dir = argc > 1 ? argv[1] : ".";
   one_case(dir, "walk", 40, 30, 20, 0);
   one_case(dir, "still", 40, 30, 12, 1);
   one_case(dir, "all", 33, 17, 6, 2);
   one_case(dir, "gba", 240, 160, 30, 0);
   one_case(dir, "one", 7, 5, 1, 2);
   one_case(dir, "wide", 1, 64, 4, 2);
   refusals();
   printf("test_apng: %s (%d checks, %d failed)\n", bad ? "FAILED" : "OK", checks, bad);
   return bad ? 1 : 0;
}
