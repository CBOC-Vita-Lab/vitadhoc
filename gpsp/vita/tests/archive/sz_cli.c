/* sz_cli: the .7z / .zip reader on its own, for test_archive.py.
 *   sz_cli peek <file>              -> "rc console name usize csize"
 *   sz_cli extract <file> <out>     -> "rc"
 *   sz_cli head <file> <n>          -> "n" and the bytes as hex
 * (the zip reader's inflater and CRC are stubbed: only .7z and stored zips are exercised here) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "zip_rom.h"

uint32_t fe_crc32(uint32_t crc, const void *data, size_t len)
{
   static uint32_t t[256];
   const unsigned char *p = (const unsigned char *)data;
   size_t i;
   if (!t[1])
   {
      uint32_t k, c, j;
      for (k = 0; k < 256; k++)
      {
         for (c = k, j = 0; j < 8; j++)
            c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
         t[k] = c;
      }
   }
   crc = ~crc;
   for (i = 0; i < len; i++)
      crc = t[(crc ^ p[i]) & 255] ^ (crc >> 8);
   return ~crc;
}

int stbi_zlib_decode_noheader_buffer(char *o, int ol, const char *i, int il)
{
   (void)o; (void)ol; (void)i; (void)il;
   return -1;
}

int main(int argc, char **argv)
{
   zip_rom_entry e;
   int rc;
   if (argc < 3)
      return 2;
   rc = zip_rom_peek(argv[2], &e);
   if (!strcmp(argv[1], "peek"))
   {
      printf("%d %d %s %lu %lu\n", rc, e.console, e.name[0] ? e.name : "-", e.usize, e.csize);
      return 0;
   }
   if (rc != 0)
   {
      printf("%d\n", rc);
      return 0;
   }
   if (!strcmp(argv[1], "extract") && argc > 3)
   {
      printf("%d\n", zip_rom_extract(argv[2], &e, argv[3]));
      return 0;
   }
   if (!strcmp(argv[1], "head") && argc > 3)
   {
      unsigned char buf[4096];
      unsigned n = (unsigned)atoi(argv[3]), i;
      int got = zip_rom_head(argv[2], &e, buf, n > sizeof(buf) ? sizeof(buf) : n);
      printf("%d ", got);
      for (i = 0; got > 0 && i < (unsigned)got; i++)
         printf("%02x", buf[i]);
      printf("\n");
      return 0;
   }
   return 2;
}
