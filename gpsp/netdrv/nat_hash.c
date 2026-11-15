/* nat_hash.c -- MD5 (RFC 1321), SHA-1 (RFC 3174) and HMAC-SHA1 (RFC 2104),
 * written from the RFCs.  Both hashes are broken as security primitives and
 * are here only because STUN / TURN (RFC 5389 / 8656) mandate them for the
 * long-term credential and the message integrity check.  Pure C99, no
 * globals, no allocation.  Every static name starts with hash_ so the file
 * can share a translation unit with nat_stun.c and nat_turn.c (the twin does).
 *
 * Public domain, like the reference code this was ported from. */
#include <string.h>

#include "nat_hash.h"

#define HASH_ROL(v, s) (((v) << (s)) | ((v) >> (32 - (s))))

static const uint32_t hash_md5_k[64] = {
   0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
   0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
   0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
   0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
   0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
   0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
   0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
   0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
static const uint8_t hash_md5_s[64] = {
   7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
   5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
   4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
   6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};

static void hash_md5_block(uint32_t st[4], const uint8_t *p)
{
   uint32_t m[16], a = st[0], b = st[1], c = st[2], d = st[3];
   int i;
   for (i = 0; i < 16; i++)
      m[i] = (uint32_t)p[4 * i] | (uint32_t)p[4 * i + 1] << 8 |
             (uint32_t)p[4 * i + 2] << 16 | (uint32_t)p[4 * i + 3] << 24;
   for (i = 0; i < 64; i++)
   {
      uint32_t f, t;
      int g;
      if (i < 16)      { f = (b & c) | (~b & d); g = i; }
      else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) & 15; }
      else if (i < 48) { f = b ^ c ^ d;          g = (3 * i + 5) & 15; }
      else             { f = c ^ (b | ~d);       g = (7 * i) & 15; }
      t = d;
      d = c;
      c = b;
      b = b + HASH_ROL(a + f + hash_md5_k[i] + m[g], hash_md5_s[i]);
      a = t;
   }
   st[0] += a; st[1] += b; st[2] += c; st[3] += d;
}

static void hash_sha1_block(uint32_t st[5], const uint8_t *p)
{
   uint32_t w[80], a = st[0], b = st[1], c = st[2], d = st[3], e = st[4];
   int i;
   for (i = 0; i < 16; i++)
      w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 |
             (uint32_t)p[4 * i + 2] << 8 | (uint32_t)p[4 * i + 3];
   for (i = 16; i < 80; i++)
      w[i] = HASH_ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
   for (i = 0; i < 80; i++)
   {
      uint32_t f, k, t;
      if (i < 20)      { f = (b & c) | (~b & d);          k = 0x5A827999u; }
      else if (i < 40) { f = b ^ c ^ d;                   k = 0x6ED9EBA1u; }
      else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
      else             { f = b ^ c ^ d;                   k = 0xCA62C1D6u; }
      t = HASH_ROL(a, 5) + f + e + k + w[i];
      e = d; d = c; c = HASH_ROL(b, 30); b = a; a = t;
   }
   st[0] += a; st[1] += b; st[2] += c; st[3] += d; st[4] += e;
}

/* Merkle-Damgard: whole blocks, then the tail (message rest, 0x80, zeros,
 * the 64-bit bit length -- little endian for MD5, big endian for SHA-1). */
static void hash_pad(uint32_t *st, const uint8_t *data, size_t len, int big_endian,
                     void (*block)(uint32_t *, const uint8_t *))
{
   uint8_t tail[128];
   size_t i = 0, rest, total, o;
   uint64_t bits = (uint64_t)len * 8u;
   int k;
   for (; i + 64 <= len; i += 64)
      block(st, data + i);
   rest = len - i;
   if (rest)
      memcpy(tail, data + i, rest);
   tail[rest] = 0x80;
   total = rest + 1 + 8 <= 64 ? 64 : 128;
   memset(tail + rest + 1, 0, total - rest - 1);
   for (k = 0; k < 8; k++)
      tail[total - 8 + k] = (uint8_t)(bits >> (big_endian ? 56 - 8 * k : 8 * k));
   for (o = 0; o < total; o += 64)
      block(st, tail + o);
}

void nat_md5(uint8_t out[16], const void *data, size_t len)
{
   uint32_t st[4] = {0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u};
   int i, k;
   hash_pad(st, (const uint8_t *)(data ? data : out), data ? len : 0, 0,
            hash_md5_block);
   for (i = 0; i < 4; i++)
      for (k = 0; k < 4; k++)
         out[4 * i + k] = (uint8_t)(st[i] >> (8 * k));
}

void nat_sha1(uint8_t out[20], const void *data, size_t len)
{
   uint32_t st[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
   int i, k;
   hash_pad(st, (const uint8_t *)(data ? data : out), data ? len : 0, 1,
            hash_sha1_block);
   for (i = 0; i < 5; i++)
      for (k = 0; k < 4; k++)
         out[4 * i + k] = (uint8_t)(st[i] >> (24 - 8 * k));
}

/* One datagram is all this ever signs: the message is copied next to the
 * pad (up to 1500 bytes; a longer one is cut, which cannot happen to a
 * STUN message built here and fails verification at the far end rather
 * than overrunning). */
void nat_hmac_sha1(uint8_t out[20], const void *key, size_t klen, const void *msg, size_t mlen)
{
   uint8_t k[64], buf[64 + 1500], inner[20], outer[64 + 20];
   int i;
   memset(k, 0, sizeof k);
   if (klen > 64)
      nat_sha1(k, key, klen);
   else if (klen)
      memcpy(k, key, klen);
   if (mlen > 1500)
      mlen = 1500;
   for (i = 0; i < 64; i++)
      buf[i] = k[i] ^ 0x36;
   if (mlen)
      memcpy(buf + 64, msg, mlen);
   nat_sha1(inner, buf, 64 + mlen);
   for (i = 0; i < 64; i++)
      outer[i] = k[i] ^ 0x5c;
   memcpy(outer + 64, inner, 20);
   nat_sha1(out, outer, sizeof outer);
}
