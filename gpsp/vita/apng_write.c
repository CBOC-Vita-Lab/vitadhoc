/* SPDX-License-Identifier: MIT OR GPL-2.0-or-later
 * Copyright (c) 2026 VitAdhoc contributors */
/* apng_write.c -- see apng_write.h. */
#include "apng_write.h"

#include <stdlib.h>
#include <string.h>

/* stb_image_write's in-memory PNG (third_party/stb; built by stb_impl_vita.c
 * on the console, by the test's own translation unit elsewhere). */
extern unsigned char *stbi_write_png_to_mem(const unsigned char *pixels,
                                            ptrdiff_t stride_bytes, int x,
                                            int y, int n, int *out_len);

struct apng
{
   apng_sink sink;
   void *ctx;
   unsigned w, h, frames, added;
   uint32_t seq;               /* the next sequence number (fcTL and fdAT)   */
   int failed;
   uint8_t *prev;              /* the picture so far, w * h * 3              */
};

uint32_t apng_crc32(uint32_t crc, const void *data, size_t len)
{
   static uint32_t table[256];
   static int made;
   const uint8_t *p = (const uint8_t *)data;
   size_t i;
   if (!made)
   {
      uint32_t n, k, c;
      for (n = 0; n < 256; n++)
      {
         c = n;
         for (k = 0; k < 8; k++)
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
         table[n] = c;
      }
      made = 1;
   }
   crc = ~crc;
   for (i = 0; i < len; i++)
      crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
   return ~crc;
}

static void be32(uint8_t *o, uint32_t v)
{
   o[0] = (uint8_t)(v >> 24);
   o[1] = (uint8_t)(v >> 16);
   o[2] = (uint8_t)(v >> 8);
   o[3] = (uint8_t)v;
}

static uint32_t rd32(const uint8_t *s)
{
   return (uint32_t)s[0] << 24 | (uint32_t)s[1] << 16 | (uint32_t)s[2] << 8 |
          (uint32_t)s[3];
}

static int put(apng *a, const void *d, size_t n)
{
   if (a->failed)
      return -1;
   if (a->sink(a->ctx, d, n) != 0)
      a->failed = 1;
   return a->failed ? -1 : 0;
}

/* One chunk: length, type, `pre` (4 bytes of sequence number, or none), data,
 * CRC over type + all of it. */
static int chunk(apng *a, const char *type, const uint8_t *pre, size_t npre,
                 const uint8_t *d, size_t n)
{
   uint8_t head[8], tail[4];
   uint32_t crc;
   be32(head, (uint32_t)(npre + n));
   memcpy(head + 4, type, 4);
   crc = apng_crc32(0, head + 4, 4);
   if (npre)
      crc = apng_crc32(crc, pre, npre);
   if (n)
      crc = apng_crc32(crc, d, n);
   be32(tail, crc);
   if (put(a, head, 8) || (npre && put(a, pre, npre)) || (n && put(a, d, n)) ||
       put(a, tail, 4))
      return -1;
   return 0;
}

apng *apng_begin(apng_sink sink, void *ctx, unsigned w, unsigned h,
                 unsigned frames)
{
   static const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
   uint8_t ihdr[13], actl[8];
   apng *a;

   if (!sink || !w || !h || w > APNG_MAX_W || h > APNG_MAX_H || !frames)
      return NULL;
   a = (apng *)calloc(1, sizeof(*a));
   if (!a)
      return NULL;
   a->prev = (uint8_t *)malloc((size_t)w * h * 3);
   if (!a->prev)
   {
      free(a);
      return NULL;
   }
   a->sink = sink;
   a->ctx = ctx;
   a->w = w;
   a->h = h;
   a->frames = frames;
   be32(ihdr, w);
   be32(ihdr + 4, h);
   ihdr[8] = 8;                   /* bits a channel                          */
   ihdr[9] = 2;                   /* RGB                                     */
   ihdr[10] = ihdr[11] = ihdr[12] = 0;
   be32(actl, frames);
   be32(actl + 4, 0);             /* plays: forever                          */
   if (put(a, sig, 8) || chunk(a, "IHDR", NULL, 0, ihdr, 13) ||
       chunk(a, "acTL", NULL, 0, actl, 8))
   {
      free(a->prev);
      free(a);
      return NULL;
   }
   return a;
}

/* The IDAT data of the PNG stb made: one chunk or several, joined. */
static uint8_t *idat_of(const uint8_t *png, size_t len, size_t *out_len)
{
   size_t at = 8, have = 0;
   uint8_t *d = (uint8_t *)malloc(len);
   if (!d)
      return NULL;
   while (at + 12 <= len)
   {
      uint32_t n = rd32(png + at);
      if (n > len || at + 12 + n > len)
         break;
      if (!memcmp(png + at + 4, "IDAT", 4))
      {
         memcpy(d + have, png + at + 8, n);
         have += n;
      }
      at += 12 + (size_t)n;
   }
   if (!have)
   {
      free(d);
      return NULL;
   }
   *out_len = have;
   return d;
}

int apng_add(apng *a, const uint8_t *rgb, unsigned delay_ms)
{
   unsigned x0 = 0, y0 = 0, x1 = a->w, y1 = a->h, rw, rh, x, y;
   size_t stride = (size_t)a->w * 3, dlen = 0;
   uint8_t fctl[26], seq[4], *idat;
   unsigned char *png;
   int png_len = 0;
   int rc = -1;

   if (a->failed || a->added >= a->frames)
      return -1;
   if (!delay_ms)
      delay_ms = 1;
   if (delay_ms > 65535)
      delay_ms = 65535;
   if (a->added)
   {
      /* the rectangle that changed; nothing: one pixel (of the same colour) */
      x0 = a->w;
      y0 = a->h;
      x1 = y1 = 0;
      for (y = 0; y < a->h; y++)
      {
         const uint8_t *c = rgb + (size_t)y * stride;
         const uint8_t *p = a->prev + (size_t)y * stride;
         if (!memcmp(c, p, stride))
            continue;
         for (x = 0; x < a->w && !memcmp(c + 3 * x, p + 3 * x, 3); x++)
            ;
         if (x < x0)
            x0 = x;
         for (x = a->w; x-- > 0 && !memcmp(c + 3 * x, p + 3 * x, 3);)
            ;
         if (x + 1 > x1)
            x1 = x + 1;
         if (y < y0)
            y0 = y;
         y1 = y + 1;
      }
      if (x1 <= x0 || y1 <= y0)
      {
         x0 = y0 = 0;
         x1 = y1 = 1;
      }
   }
   rw = x1 - x0;
   rh = y1 - y0;
   png = stbi_write_png_to_mem(rgb + (size_t)y0 * stride + (size_t)x0 * 3,
                               (ptrdiff_t)stride, (int)rw, (int)rh, 3,
                               &png_len);
   if (!png)
      return -1;
   idat = idat_of(png, (size_t)png_len, &dlen);
   free(png);
   if (!idat)
      return -1;

   be32(fctl, a->seq++);
   be32(fctl + 4, rw);
   be32(fctl + 8, rh);
   be32(fctl + 12, x0);
   be32(fctl + 16, y0);
   fctl[20] = (uint8_t)(delay_ms >> 8);       /* delay numerator             */
   fctl[21] = (uint8_t)delay_ms;
   fctl[22] = 0x03;                           /* ...over 1000: milliseconds  */
   fctl[23] = 0xE8;
   fctl[24] = 0;                              /* dispose: none               */
   fctl[25] = 0;                              /* blend: source               */
   if (chunk(a, "fcTL", NULL, 0, fctl, 26) != 0)
      goto done;
   if (!a->added)
   {
      if (chunk(a, "IDAT", NULL, 0, idat, dlen) != 0)
         goto done;
   }
   else
   {
      be32(seq, a->seq++);
      if (chunk(a, "fdAT", seq, 4, idat, dlen) != 0)
         goto done;
   }
   /* the picture so far */
   for (y = y0; y < y1; y++)
      memcpy(a->prev + (size_t)y * stride + (size_t)x0 * 3,
             rgb + (size_t)y * stride + (size_t)x0 * 3, (size_t)rw * 3);
   a->added++;
   rc = 0;
done:
   free(idat);
   return rc;
}

int apng_end(apng *a)
{
   int ok;
   if (!a)
      return -1;
   ok = !a->failed && a->added == a->frames && chunk(a, "IEND", NULL, 0, NULL, 0) == 0;
   free(a->prev);
   free(a);
   return ok ? 0 : -1;
}
