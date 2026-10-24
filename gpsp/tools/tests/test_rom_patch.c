/* rom_patch.c: IPS, BPS and UPS patches built here by the formats' own
 * rules (IPS: records and runs; beat's BPS and UPS: varints, actions,
 * hunks, CRC32 footers), applied and checked byte for byte -- including
 * the refusals: a damaged patch, a patch for another version of the game,
 * a truncated one.  gcc -Ifrontend-common tools/tests/test_rom_patch.c
 * vita/rom_patch.c frontend-common/fe_util.c */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../vita/rom_patch.h"
#include "fe_util.h"

static uint8_t buf[1 << 16];
static size_t n;

static void put(const void *p, size_t k) { memcpy(buf + n, p, k); n += k; }
static void put8(unsigned v) { buf[n++] = (uint8_t)v; }
static void put32le(uint32_t v) { put8(v); put8(v >> 8); put8(v >> 16); put8(v >> 24); }
static void varint(uint64_t data)
{
   for (;;)
   {
      uint8_t x = data & 0x7F;
      data >>= 7;
      if (!data) { put8(0x80 | x); break; }
      put8(x);
      data--;
   }
}

static int apply(const uint8_t *src, size_t slen, uint8_t *dst, size_t cap,
                 size_t *dlen, const char **why)
{
   size_t want = rom_patch_size(buf, n, slen, why);
   int rc = rom_patch_apply(buf, n, src, slen, dst, cap, dlen, why);
   if (rc == 0)
      assert(want == *dlen);
   return rc;
}

int main(void)
{
   static uint8_t src[4096], dst[8192];
   const char *why = NULL;
   size_t dlen = 0;
   unsigned i;
   for (i = 0; i < sizeof(src); i++)
      src[i] = (uint8_t)(i * 13 + 7);

   /* ---- IPS: a record, a run, a record past the end (the file grows) -- */
   n = 0;
   put("PATCH", 5);
   put8(0x00); put8(0x00); put8(0x10); put8(0); put8(3); put("abc", 3);
   put8(0x00); put8(0x01); put8(0x00); put8(0); put8(0); put8(0); put8(40); put8(0xEE);
   put8(0x00); put8(0x10); put8(0x10); put8(0); put8(2); put("zz", 2);
   put("EOF", 3);
   assert(rom_patch_kind(buf, n) == ROM_PATCH_IPS);
   assert(apply(src, sizeof(src), dst, sizeof(dst), &dlen, &why) == 0);
   assert(dlen == 0x1012);
   assert(!memcmp(dst, src, 0x10) && !memcmp(dst + 0x10, "abc", 3));
   assert(!memcmp(dst + 0x13, src + 0x13, 0x100 - 0x13));
   for (i = 0; i < 40; i++) assert(dst[0x100 + i] == 0xEE);
   assert(!memcmp(dst + 0x128, src + 0x128, sizeof(src) - 0x128));
   assert(dst[0x1000] == 0 && !memcmp(dst + 0x1010, "zz", 2));
   /* ...cut to a size after EOF (Lunar IPS's truncation) */
   n = 0;
   put("PATCH", 5);
   put8(0x00); put8(0x00); put8(0x10); put8(0); put8(3); put("abc", 3);
   put("EOF", 3);
   put8(0x00); put8(0x08); put8(0x00);
   assert(apply(src, sizeof(src), dst, sizeof(dst), &dlen, &why) == 0);
   assert(dlen == 0x800 && !memcmp(dst + 0x10, "abc", 3));
   /* damaged: cut short inside a record */
   n = 0;
   put("PATCH", 5);
   put8(0x00); put8(0x00); put8(0x10); put8(0); put8(9); put("abc", 3);
   assert(apply(src, sizeof(src), dst, sizeof(dst), &dlen, &why) == -1);
   assert(!strcmp(why, "damaged"));
   /* too large for the buffer */
   n = 0;
   put("PATCH", 5);
   put8(0x7F); put8(0xFF); put8(0x00); put8(0); put8(1); put("x", 1);
   put("EOF", 3);
   assert(apply(src, sizeof(src), dst, sizeof(dst), &dlen, &why) == -1);
   assert(!strcmp(why, "too large"));
   /* not a patch */
   n = 0;
   put("HELLO WORLD!!", 13);
   assert(rom_patch_kind(buf, n) == ROM_PATCH_NONE);
   assert(apply(src, sizeof(src), dst, sizeof(dst), &dlen, &why) == -1);
   assert(!strcmp(why, "not a patch"));

   /* ---- BPS: every action ------------------------------------------------ */
   {
      static const uint8_t s8[] = "ABCDEFGH";
      static const uint8_t t8[] = "ABXYEFGHGHGH";
      size_t ss = 8, ts = 12;
      n = 0;
      put("BPS1", 4);
      varint(ss); varint(ts); varint(5); put("meta!", 5);
      varint(((2 - 1) << 2) | 0);                   /* SourceRead 2: AB    */
      varint(((2 - 1) << 2) | 1); put("XY", 2);      /* TargetRead 2: XY    */
      varint(((4 - 1) << 2) | 2); varint(4 << 1);    /* SourceCopy 4 @+4    */
      varint(((4 - 1) << 2) | 3); varint(6 << 1);    /* TargetCopy 4 @6:    */
                                                     /* GHGH, overlapping   */
      put32le(fe_crc32(0, s8, ss));
      put32le(fe_crc32(0, t8, ts));
      put32le(fe_crc32(0, buf, n));
      assert(rom_patch_kind(buf, n) == ROM_PATCH_BPS);
      assert(apply(s8, ss, dst, sizeof(dst), &dlen, &why) == 0);
      assert(dlen == ts && !memcmp(dst, t8, ts));
      /* another version of the game */
      assert(apply((const uint8_t *)"ABCDEFGX", 8, dst, sizeof(dst), &dlen, &why) == -1);
      assert(!strcmp(why, "made for another version of the game"));
      /* a byte of the patch changed */
      buf[12] ^= 1;
      assert(apply(s8, ss, dst, sizeof(dst), &dlen, &why) == -1);
      assert(!strcmp(why, "damaged"));
   }
   /* a larger one: the 4 KiB source with its middle replaced and its end
    * repeated -- against a result computed here */
   {
      static uint8_t want[6000];
      memcpy(want, src, 4096);
      memset(want + 1000, 0x5A, 300);
      for (i = 0; i < 1904; i++)          /* TargetCopy: byte by byte */
         want[4096 + i] = want[3000 + i];
      n = 0;
      put("BPS1", 4);
      varint(4096); varint(6000); varint(0);
      varint(((1000 - 1) << 2) | 0);
      varint(((300 - 1) << 2) | 1); { static uint8_t z[300]; memset(z, 0x5A, 300); put(z, 300); }
      varint(((2796 - 1) << 2) | 2); varint(1300 << 1);
      varint(((1904 - 1) << 2) | 3); varint(3000 << 1);
      put32le(fe_crc32(0, src, 4096));
      put32le(fe_crc32(0, want, 6000));
      put32le(fe_crc32(0, buf, n));
      assert(apply(src, 4096, dst, sizeof(dst), &dlen, &why) == 0);
      assert(dlen == 6000 && !memcmp(dst, want, 6000));
      /* a result over the buffer is refused before anything is written */
      assert(rom_patch_apply(buf, n, src, 4096, dst, 5999, &dlen, &why) == -1 &&
             !strcmp(why, "too large"));
   }

   /* ---- UPS: XOR hunks, a larger result, and backwards ------------------- */
   {
      static const uint8_t a[] = "HELLOWORLD";
      static const uint8_t b[] = "HELLOWXRLDZZ";
      static uint8_t back[64];
      n = 0;
      put("UPS1", 4);
      varint(10); varint(12);
      varint(6); put8('O' ^ 'X'); put8(0);           /* @6: O -> X           */
      varint(2); put8('Z'); put8('Z'); put8(0);       /* @10: 6, the X, its 0, 2: ZZ */
      put32le(fe_crc32(0, a, 10));
      put32le(fe_crc32(0, b, 12));
      put32le(fe_crc32(0, buf, n));
      assert(rom_patch_kind(buf, n) == ROM_PATCH_UPS);
      assert(apply(a, 10, dst, sizeof(dst), &dlen, &why) == 0);
      assert(dlen == 12 && !memcmp(dst, b, 12));
      assert(apply(b, 12, back, sizeof(back), &dlen, &why) == 0);
      assert(dlen == 10 && !memcmp(back, a, 10));
      assert(apply((const uint8_t *)"HELLOWORLX", 10, dst, sizeof(dst), &dlen,
                   &why) == -1 &&
             !strcmp(why, "made for another version of the game"));
      buf[n - 5] ^= 0x40;              /* the result's CRC in the footer */
      assert(apply(a, 10, dst, sizeof(dst), &dlen, &why) == -1 &&
             !strcmp(why, "damaged"));
   }

   /* ---- fuzz: nothing read or written out of bounds ---------------------- */
   {
      unsigned seed = 12345, k;
      for (k = 0; k < 20000; k++)
      {
         size_t len = 8 + (seed >> 16) % 200, j;
         static const char *const magic[3] = { "PATCH", "BPS1", "UPS1" };
         const char *m = magic[k % 3];
         n = 0;
         put(m, strlen(m));
         for (j = strlen(m); j < len; j++)
         {
            seed = seed * 1103515245u + 12345u;
            put8(seed >> 16);
         }
         if (k % 3)
         {
            /* a valid CRC now and then, so the bodies get walked */
            n -= 4;
            put32le(fe_crc32(0, buf, n));
         }
         (void)rom_patch_size(buf, n, 64, &why);
         (void)rom_patch_apply(buf, n, src, 64, dst, sizeof(dst), &dlen, &why);
      }
   }
   puts("rom patches: ok");
   return 0;
}
