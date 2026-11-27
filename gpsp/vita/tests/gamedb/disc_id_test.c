/* disc_id.h: the id of a Sega CD disc for Two players, from a cue sheet in the spellings real rips come in. Every spelling of the same disc must
 * give the id of its first sector, which the plain .iso of the same data gives too; and a sheet that names no file or a missing one gives none. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "disc_id.h"

static int fails = 0;
#define EXPECT(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint32_t crc32_of(uint32_t c, const uint8_t *p, size_t n)
{
   size_t i; int k;
   c = ~c;
   for (i = 0; i < n; i++) { c ^= p[i]; for (k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1))); }
   return ~c;
}

static void put(const char *name, const void *d, size_t n)
{
   FILE *f = fopen(name, "wb");
   if (!f || fwrite(d, 1, n, f) != n) { printf("cannot write %s\n", name); exit(2); }
   fclose(f);
}

static void id_is(const char *file, const char *want, const char *what)
{
   char id[9];
   int r = disc_game_id(file, id, crc32_of);
   if (want) EXPECT(r == 1 && !strcmp(id, want), "%s: id '%s' (r %d), expected %s", what, id, r, want);
   else EXPECT(r == 0, "%s: gave id '%s', expected none", what, id);
}

int main(void)
{
   static uint8_t cooked[2048 * 4], raw[2352 * 4];
   char want[9];
   size_t i;
   for (i = 0; i < sizeof cooked; i++) cooked[i] = (uint8_t)(i * 7 + (i >> 8));
   for (i = 0; i < 4; i++) {
      uint8_t *s = raw + i * 2352;
      memset(s + 1, 0xFF, 10); s[15] = 1;
      memcpy(s + 16, cooked + i * 2048, 2048);
   }
   snprintf(want, sizeof want, "%08X", (unsigned)crc32_of(0, cooked, 2048));
   put("did.iso", cooked, sizeof cooked);
   put("did_raw.bin", raw, sizeof raw);
   put("did tr.bin", cooked, sizeof cooked);
   id_is("did.iso", want, "a cooked .iso");
   put("did_a.cue", "FILE \"did_raw.bin\" BINARY\n  TRACK 01 MODE1/2352\n    INDEX 01 00:00:00\n", 72);
   id_is("did_a.cue", want, "a raw .bin through a quoted FILE");
   { const char *t = "FILE did_raw.bin BINARY\r\n  TRACK 01 MODE1/2352\r\n"; put("did_b.cue", t, strlen(t)); }
   id_is("did_b.cue", want, "an unquoted name, CRLF");
   { const char *t = "\xEF\xBB\xBF" "FILE\t\"did_raw.bin\"\tBINARY\r\n"; put("did_c.cue", t, strlen(t)); }
   id_is("did_c.cue", want, "a byte order mark and tabs");
   { const char *t = "FILE \"C:\\Games\\Rips\\did_raw.bin\" BINARY\n"; put("did_d.cue", t, strlen(t)); }
   id_is("did_d.cue", want, "a Windows path in the name (the file beside the sheet is used)");
   { const char *t = "FILE \"some/dir/did tr.bin\" BINARY\n"; put("did_e.cue", t, strlen(t)); }
   id_is("did_e.cue", want, "a directory and a space in the name");
   { const char *t = "REM no file here\nTRACK 01 MODE1/2048\n"; put("did_f.cue", t, strlen(t)); }
   id_is("did_f.cue", NULL, "a sheet without a FILE line");
   { const char *t = "FILE \"missing.bin\" BINARY\n"; put("did_g.cue", t, strlen(t)); }
   id_is("did_g.cue", NULL, "a sheet naming a file that is not there");
   { const char *t = "FILE \"did_raw.bin BINARY\n"; put("did_h.cue", t, strlen(t)); }
   id_is("did_h.cue", NULL, "an unterminated quote");
   { const char *t = "FILE \"did_raw.bin\" BINARY\n"; put("dev:did_i.cue", t, strlen(t)); }
   put("dev:did_raw.bin", raw, sizeof raw);
   id_is("dev:did_i.cue", want, "a sheet in a 'device:' path (the file beside it)");
   id_is("nothing.cue", NULL, "a sheet that does not exist");
   id_is("did.txt", NULL, "not a disc extension");
   {  /* a .chd: the id comes from the header's raw SHA-1 field; a file that is no CHD gives none */
      uint8_t h[124]; char id[9], w2[9];
      memset(h, 0, sizeof h); memcpy(h, "MComprHD", 8); h[11] = 124; h[15] = 5;
      for (i = 0; i < 20; i++) h[64 + i] = (uint8_t)(i + 1);
      put("did.chd", h, sizeof h);
      snprintf(w2, sizeof w2, "%08X", (unsigned)crc32_of(0, h + 64, 20));
      EXPECT(disc_game_id("did.chd", id, crc32_of) == 1 && !strcmp(id, w2), "a v5 .chd: id %s expected %s", id, w2);
      h[15] = 4; put("did_v4.chd", h, sizeof h);
      EXPECT(disc_game_id("did_v4.chd", id, crc32_of) == 0, "a v4 .chd is refused");
   }
   {  /* a .chd whose SHA-1 field is all zero: different discs must not share one id */
      uint8_t f1[124 + 600], f2[124 + 600], f3[124 + 700];
      char a[9], b[9], c[9], a2[9];
      memset(f1, 0, sizeof f1); memcpy(f1, "MComprHD", 8); f1[11] = 124; f1[15] = 5;
      f1[47] = 124;                                    /* map offset (big endian, byte 40..47) */
      for (i = 0; i < 600; i++) f1[124 + i] = (uint8_t)(i * 13 + 5);
      memcpy(f2, f1, sizeof f1); f2[124 + 40] ^= 0x5A;                       /* the same size and header, another first hunk map */
      memset(f3, 0, sizeof f3); memcpy(f3, f1, 124); memcpy(f3 + 124, f1 + 124, 600);   /* the same header and map bytes, a longer file */
      put("did_z1.chd", f1, sizeof f1); put("did_z2.chd", f2, sizeof f2); put("did_z3.chd", f3, sizeof f3);
      EXPECT(disc_game_id("did_z1.chd", a, crc32_of) == 1 && disc_game_id("did_z2.chd", b, crc32_of) == 1 && disc_game_id("did_z3.chd", c, crc32_of) == 1,
             "zero SHA-1 .chd files are given an id");
      EXPECT(strcmp(a, b) && strcmp(a, c) && strcmp(b, c), "zero SHA-1: ids differ (%s %s %s)", a, b, c);
      EXPECT(disc_game_id("did_z1.chd", a2, crc32_of) == 1 && !strcmp(a, a2), "zero SHA-1: the id is stable");
      f1[47] = 0; put("did_z4.chd", f1, sizeof f1);
      EXPECT(disc_game_id("did_z4.chd", a, crc32_of) == 0, "zero SHA-1 and no map offset: refused");
      f1[47] = 124; f1[46] = 0x10; put("did_z5.chd", f1, sizeof f1);
      EXPECT(disc_game_id("did_z5.chd", a, crc32_of) == 0, "zero SHA-1 and a map offset past the end: refused");
      remove("did_z1.chd"); remove("did_z2.chd"); remove("did_z3.chd"); remove("did_z4.chd"); remove("did_z5.chd");
   }
   remove("did.iso"); remove("did_raw.bin"); remove("did tr.bin"); remove("did.chd"); remove("did_v4.chd"); remove("dev:did_i.cue"); remove("dev:did_raw.bin");
   remove("did_a.cue"); remove("did_b.cue"); remove("did_c.cue"); remove("did_d.cue"); remove("did_e.cue"); remove("did_f.cue"); remove("did_g.cue"); remove("did_h.cue");
   printf("disc_id_test: %s\n", fails ? "FAILED" : "all passed");
   return fails ? 1 : 0;
}
