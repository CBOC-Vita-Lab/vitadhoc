/* disc_id.h -- a Sega CD disc's id for Two players, plain C, header only.
 * The id is 8 upper-case hex digits like the cartridge ones: the CRC-32 of the disc's first sector (2048 bytes of user data) for a .cue
 * and an .iso, so the same disc as a cooked or a raw image gives the same id; for a .chd the CRC-32 of the header's raw SHA-1 field (a
 * decompressing reader is not linked into the app).  Players of one room must hold the same kind of image of a .chd game.
 * Included by link_vita.c and online_lib.c, which both compute the id of a library file. */
#ifndef GBADHOC_DISC_ID_H
#define GBADHOC_DISC_ID_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

static inline int disc_ext_is(const char *path, const char *ext)
{
   size_t n = path ? strlen(path) : 0, e = strlen(ext);
   return n > e && strcasecmp(path + n - e, ext) == 0;
}

static inline int disc_is_image(const char *path)
{
   return disc_ext_is(path, ".cue") || disc_ext_is(path, ".chd") || disc_ext_is(path, ".iso");
}

/* `crc` is the caller's CRC-32 routine (start 0). 1 = id written, 0 = not a readable disc */
static inline int disc_game_id(const char *path, char out[9], uint32_t (*crc)(uint32_t, const uint8_t *, size_t))
{
   char track[640];
   uint8_t buf[2352 + 16];
   const char *data = path;
   FILE *f;
   size_t n;
   uint32_t c;
   if (out)
      out[0] = '\0';
   if (!path || !out || !disc_is_image(path))
      return 0;
   if (disc_ext_is(path, ".cue"))
   {
      /* the first FILE "name" of the sheet: the data track, beside the sheet */
      char line[512];
      const char *sl = strrchr(path, '/');
      size_t dl = sl ? (size_t)(sl - path) + 1 : 0;
      int found = 0;
      f = fopen(path, "rb");
      if (!f)
         return 0;
      while (!found && fgets(line, sizeof(line), f))
      {
         char *q = strstr(line, "FILE");
         char *a, *b;
         if (!q || !(a = strchr(q, '"')) || !(b = strchr(a + 1, '"')))
            continue;
         *b = '\0';
         if (strlen(a + 1) + dl >= sizeof(track) || strchr(a + 1, '/'))
            continue;
         memcpy(track, path, dl);
         strcpy(track + dl, a + 1);
         found = 1;
      }
      fclose(f);
      if (!found)
         return 0;
      data = track;
   }
   f = fopen(data, "rb");
   if (!f)
      return 0;
   if (disc_ext_is(path, ".chd"))
   {
      /* v5 header: "MComprHD", length 124, version 5, ..., raw SHA-1 at byte 64 */
      n = fread(buf, 1, 84, f);
      fclose(f);
      if (n != 84 || memcmp(buf, "MComprHD", 8) || buf[8] || buf[9] || buf[10] || buf[11] != 124 || buf[15] != 5)
         return 0;
      snprintf(out, 9, "%08X", (unsigned)crc(0, buf + 64, 20));
      return 1;
   }
   n = fread(buf, 1, sizeof(buf), f);
   fclose(f);
   if (n < 2048)
      return 0;
   /* a raw image has the 12 byte sync and a 4 byte header before the user data of a Mode 1 sector */
   if (n >= 2352 && buf[0] == 0x00 && buf[1] == 0xFF && buf[11] == 0x00 && buf[15] == 1)
      c = crc(0, buf + 16, 2048);
   else
      c = crc(0, buf, 2048);
   snprintf(out, 9, "%08X", (unsigned)c);
   return 1;
}

#endif
