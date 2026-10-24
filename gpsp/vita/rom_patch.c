/* rom_patch.c -- rom_patch.h. */
#include <string.h>

#include "rom_patch.h"
#include "fe_util.h"        /* fe_crc32: the CRC-32 of zip, PNG, BPS, UPS */

int rom_patch_kind(const uint8_t *p, size_t n)
{
   if (n >= 8 && !memcmp(p, "PATCH", 5))
      return ROM_PATCH_IPS;
   if (n >= 4 + 3 + 12 && !memcmp(p, "BPS1", 4))
      return ROM_PATCH_BPS;
   if (n >= 4 + 2 + 12 && !memcmp(p, "UPS1", 4))
      return ROM_PATCH_UPS;
   return ROM_PATCH_NONE;
}

static uint32_t rd32le(const uint8_t *p)
{
   return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
          ((uint32_t)p[3] << 24);
}

/* beat's variable-length number (BPS and UPS).  0 = read; -1 = past `end`
 * or more than 64 bits. */
static int varint(const uint8_t **pp, const uint8_t *end, uint64_t *out)
{
   uint64_t data = 0, shift = 1;
   const uint8_t *p = *pp;
   int i;
   for (i = 0; i < 10; i++)
   {
      uint8_t x;
      if (p >= end)
         return -1;
      x = *p++;
      data += (uint64_t)(x & 0x7F) * shift;
      if (x & 0x80)
      {
         *pp = p;
         *out = data;
         return 0;
      }
      shift <<= 7;
      data += shift;
   }
   return -1;
}

/* ---- IPS ----------------------------------------------------------------- */

static size_t ips_walk(const uint8_t *p, size_t n, size_t slen, uint8_t *dst,
                       size_t dcap, const char **why)
{
   size_t i = 5, top = slen;
   while (i + 3 <= n)
   {
      uint32_t off = ((uint32_t)p[i] << 16) | ((uint32_t)p[i + 1] << 8) | p[i + 2];
      uint32_t len;
      if (off == 0x454F46)              /* "EOF" */
      {
         i += 3;
         if (i + 3 <= n)                /* the cut-to size, if there is one */
            top = ((size_t)p[i] << 16) | ((size_t)p[i + 1] << 8) | p[i + 2];
         return top ? top : (size_t)-1;
      }
      if (i + 5 > n)
         break;
      len = ((uint32_t)p[i + 3] << 8) | p[i + 4];
      i += 5;
      if (len)
      {
         if (i + len > n)
            break;
         if (dst)
         {
            if (off + len > dcap)
               break;
            memcpy(dst + off, p + i, len);
         }
         i += len;
      }
      else
      {
         uint8_t v;
         if (i + 3 > n)
            break;
         len = ((uint32_t)p[i] << 8) | p[i + 1];
         v = p[i + 2];
         i += 3;
         if (dst)
         {
            if (off + len > dcap)
               break;
            memset(dst + off, v, len);
         }
      }
      if (off + len > top)
         top = off + len;
   }
   if (why)
      *why = "damaged";
   return 0;
}

/* ---- BPS / UPS headers ---------------------------------------------------- */

typedef struct
{
   uint64_t in, out;
   const uint8_t *body, *end;       /* the actions / hunks */
   uint32_t in_crc, out_crc;
} beat_hdr;

static int beat_header(const uint8_t *p, size_t n, int bps, beat_hdr *h,
                       const char **why)
{
   const uint8_t *q = p + 4, *end = p + n - 12;
   uint64_t meta = 0;
   if (fe_crc32(0, p, n - 4) != rd32le(p + n - 4))
   {
      *why = "damaged";
      return -1;
   }
   if (varint(&q, end, &h->in) || varint(&q, end, &h->out) ||
       (bps && varint(&q, end, &meta)) || (uint64_t)(end - q) < meta)
   {
      *why = "damaged";
      return -1;
   }
   h->body = q + meta;
   h->end = end;
   h->in_crc = rd32le(p + n - 12);
   h->out_crc = rd32le(p + n - 8);
   return 0;
}

size_t rom_patch_size(const uint8_t *p, size_t n, size_t slen, const char **why)
{
   const char *w = "not a patch";
   beat_hdr h;
   size_t r = 0;
   switch (rom_patch_kind(p, n))
   {
   case ROM_PATCH_IPS:
      r = ips_walk(p, n, slen, NULL, 0, &w);
      if (r == (size_t)-1)
         r = 0, w = "damaged";
      break;
   case ROM_PATCH_BPS:
      if (beat_header(p, n, 1, &h, &w) == 0)
         r = (size_t)h.out;
      break;
   case ROM_PATCH_UPS:
      if (beat_header(p, n, 0, &h, &w) == 0)
         r = (size_t)(slen == h.out && h.in != h.out ? h.in : h.out);
      break;
   }
   if (!r && why)
      *why = w;
   return r;
}

static int bps_apply(const uint8_t *p, size_t n, const uint8_t *src,
                     size_t slen, uint8_t *dst, size_t dcap, size_t *dlen,
                     const char **why)
{
   beat_hdr h;
   const uint8_t *q;
   size_t out = 0;
   int64_t src_rel = 0, dst_rel = 0;
   if (beat_header(p, n, 1, &h, why))
      return -1;
   if (h.in != slen || fe_crc32(0, src, slen) != h.in_crc)
   {
      *why = "made for another version of the game";
      return -1;
   }
   if (h.out > dcap)
   {
      *why = "too large";
      return -1;
   }
   q = h.body;
   while (q < h.end)
   {
      uint64_t cmd, len, off;
      if (varint(&q, h.end, &cmd))
         goto bad;
      len = (cmd >> 2) + 1;
      if (len > h.out - out)
         goto bad;
      switch (cmd & 3)
      {
      case 0:                           /* SourceRead */
         if (out + len > slen)
            goto bad;
         memcpy(dst + out, src + out, (size_t)len);
         out += (size_t)len;
         break;
      case 1:                           /* TargetRead */
         if ((uint64_t)(h.end - q) < len)
            goto bad;
         memcpy(dst + out, q, (size_t)len);
         q += len;
         out += (size_t)len;
         break;
      case 2:                           /* SourceCopy */
         if (varint(&q, h.end, &off))
            goto bad;
         src_rel += (off & 1 ? -1 : 1) * (int64_t)(off >> 1);
         if (src_rel < 0 || (uint64_t)src_rel + len > slen)
            goto bad;
         memcpy(dst + out, src + src_rel, (size_t)len);
         src_rel += (int64_t)len;
         out += (size_t)len;
         break;
      default:                          /* TargetCopy: may overlap, bytewise */
         if (varint(&q, h.end, &off))
            goto bad;
         dst_rel += (off & 1 ? -1 : 1) * (int64_t)(off >> 1);
         if (dst_rel < 0 || (uint64_t)dst_rel >= out)
            goto bad;
         while (len--)
         {
            if ((uint64_t)dst_rel >= out)
               goto bad;
            dst[out++] = dst[dst_rel++];
         }
         break;
      }
   }
   if (out != h.out || fe_crc32(0, dst, out) != h.out_crc)
      goto bad;
   *dlen = out;
   return 0;
bad:
   *why = "damaged";
   return -1;
}

static int ups_apply(const uint8_t *p, size_t n, const uint8_t *src,
                     size_t slen, uint8_t *dst, size_t dcap, size_t *dlen,
                     const char **why)
{
   beat_hdr h;
   const uint8_t *q;
   uint64_t from, to, off = 0;
   uint32_t want_in, want_out;
   uint32_t crc;
   if (beat_header(p, n, 0, &h, why))
      return -1;
   crc = fe_crc32(0, src, slen);
   if (slen == h.in && crc == h.in_crc)
      from = h.in, to = h.out, want_in = h.in_crc, want_out = h.out_crc;
   else if (slen == h.out && crc == h.out_crc)
      from = h.out, to = h.in, want_in = h.out_crc, want_out = h.in_crc;
   else
   {
      *why = "made for another version of the game";
      return -1;
   }
   (void)want_in;
   if (to > dcap)
   {
      *why = "too large";
      return -1;
   }
   memset(dst, 0, (size_t)to);
   memcpy(dst, src, (size_t)(from < to ? from : to));
   q = h.body;
   while (q < h.end)
   {
      uint64_t skip;
      if (varint(&q, h.end, &skip))
         goto bad;
      off += skip;
      for (;;)
      {
         uint8_t x;
         if (q >= h.end)
            goto bad;
         x = *q++;
         if (!x)
         {
            off++;
            break;
         }
         if (off < to)
            dst[off] ^= x;
         off++;
      }
   }
   if (fe_crc32(0, dst, (size_t)to) != want_out)
      goto bad;
   *dlen = (size_t)to;
   return 0;
bad:
   *why = "damaged";
   return -1;
}

int rom_patch_apply(const uint8_t *p, size_t n, const uint8_t *src,
                    size_t slen, uint8_t *dst, size_t dcap, size_t *dlen,
                    const char **why)
{
   const char *w = "not a patch";
   int kind = rom_patch_kind(p, n), rc = -1;
   if (kind == ROM_PATCH_IPS)
   {
      size_t top = ips_walk(p, n, slen, NULL, 0, &w);
      if (top && top != (size_t)-1)
      {
         if (top > dcap)
            w = "too large";
         else
         {
            size_t keep = slen < top ? slen : top;
            memcpy(dst, src, keep);
            if (top > keep)
               memset(dst + keep, 0, top - keep);
            if (ips_walk(p, n, slen, dst, top, &w))
            {
               *dlen = top;
               rc = 0;
            }
         }
      }
      else if (top == (size_t)-1)
         w = "damaged";
   }
   else if (kind == ROM_PATCH_BPS)
      rc = bps_apply(p, n, src, slen, dst, dcap, dlen, &w);
   else if (kind == ROM_PATCH_UPS)
      rc = ups_apply(p, n, src, slen, dst, dcap, dlen, &w);
   if (rc && why)
      *why = w;
   return rc;
}
