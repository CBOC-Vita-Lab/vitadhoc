/* disc_id.h -- a Sega CD disc's id for Two players, plain C, header only.
 * The id is 8 upper-case hex digits like the cartridge ones: the CRC-32 of the disc's first sector (2048 bytes of user data) for a .cue
 * and an .iso, so the same disc as a cooked or a raw image gives the same id; for a .chd the CRC-32 of the header's raw SHA-1 field (a
 * decompressing reader is not linked into the app; a header whose SHA-1 is all zero: the CRC-32 of the file size, header and first hunk map bytes).  Players of one room must hold the same kind of image of a .chd game.
 * Included by link_vita.c and online_lib.c, which both compute the id of a library file. */
#ifndef GBADHOC_DISC_ID_H
#define GBADHOC_DISC_ID_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "../../playerkit/src/pk_m3u.h"

static inline int disc_ext_is(const char *path, const char *ext)
{
   size_t n = path ? strlen(path) : 0, e = strlen(ext);
   return n > e && strcasecmp(path + n - e, ext) == 0;
}

static inline int disc_is_image(const char *path)
{
   return disc_ext_is(path, ".cue") || disc_ext_is(path, ".chd") || disc_ext_is(path, ".iso") || disc_ext_is(path, ".m3u");
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
   if (disc_ext_is(path, ".m3u"))
   {
      /* a game on several discs is its first disc that is there: the same id whichever disc of the list is in the drive, and the same as the
       * first disc's own .cue gives */
      static char list[PK_M3U_MAX_DISCS][PK_M3U_PATH];
      int nd = pk_m3u_load(path, list, PK_M3U_MAX_DISCS, NULL, NULL), i;
      for (i = 0; i < nd; i++)
      {
         struct stat st;
         if (stat(list[i], &st) == 0 && S_ISREG(st.st_mode))
            return disc_game_id(list[i], out, crc);
      }
      return 0;
   }
   if (disc_ext_is(path, ".cue"))
   {
      /* the first FILE of the sheet: the data track, beside the sheet. The name may be in quotes or not, may carry a directory (the core takes the
       * file beside the sheet, whatever directory the sheet names: Windows rips do that), and the sheet may be in a folder given as "ux0:Game.cue". */
      char line[512];
      const char *sl = strrchr(path, '/'), *co = strrchr(path, ':');
      size_t dl;
      int found = 0, first = 1;
      if (co && (!sl || co > sl))
         sl = co;
      dl = sl ? (size_t)(sl - path) + 1 : 0;
      f = fopen(path, "rb");
      if (!f)
         return 0;
      while (!found && fgets(line, sizeof(line), f))
      {
         char *q = line, *e, *base, *k;
         if (first && (uint8_t)q[0] == 0xEF && (uint8_t)q[1] == 0xBB && (uint8_t)q[2] == 0xBF)
            q += 3;                                     /* a UTF-8 byte order mark */
         first = 0;
         while (*q == ' ' || *q == '\t')
            q++;
         if (strncmp(q, "FILE", 4) || (q[4] != ' ' && q[4] != '\t'))
            continue;
         q += 4;
         while (*q == ' ' || *q == '\t')
            q++;
         if (*q == '"')
         {
            q++;
            if (!(e = strchr(q, '"')))
               continue;
         }
         else
         {
            e = q;
            while (*e && *e != ' ' && *e != '\t' && *e != '\r' && *e != '\n')
               e++;
         }
         *e = '\0';
         base = q;
         for (k = q; *k; k++)
            if (*k == '/' || *k == '\\')
               base = k + 1;
         if (!*base || strlen(base) + dl >= sizeof(track))
            continue;
         memcpy(track, path, dl);
         strcpy(track + dl, base);
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
      n = fread(buf, 1, 124, f);
      if (n != 124 || memcmp(buf, "MComprHD", 8) || buf[8] || buf[9] || buf[10] || buf[11] != 124 || buf[15] != 5)
      {
         fclose(f);
         return 0;
      }
      {
         int zero = 1, i;
         for (i = 64; i < 84; i++)
            if (buf[i])
               zero = 0;
         if (!zero)
         {
            fclose(f);
            snprintf(out, 9, "%08X", (unsigned)crc(0, buf + 64, 20));
            return 1;
         }
      }
      /* a CHD written without the data's SHA-1 (all zero: some tools do) must not give every such disc the same id: the id is then the CRC-32 of the
       * file's size, its 124 byte header (the logical size, the hunk size and the offsets differ from disc to disc) and the first 512 bytes of its
       * hunk map (compressed hunk sizes and CRCs of the first hunks: the disc's content). A file with no map there is refused. */
      {
         uint8_t sz[8], map[512];
         uint64_t size = 0, mo = 0;
         long end;
         int i;
         if (fseek(f, 0, SEEK_END) || (end = ftell(f)) < 0)
         {
            fclose(f);
            return 0;
         }
         size = (uint64_t)end;
         for (i = 0; i < 8; i++)
            mo = (mo << 8) | buf[40 + i];               /* the map offset, big endian */
         if (mo < 124 || mo >= size || fseek(f, (long)mo, SEEK_SET))
         {
            fclose(f);
            return 0;
         }
         n = fread(map, 1, sizeof(map), f);
         fclose(f);
         if (n < 16)
            return 0;
         for (i = 0; i < 8; i++)
            sz[i] = (uint8_t)(size >> (8 * i));
         c = crc(0, sz, 8);
         c = crc(c, buf, 124);
         c = crc(c, map, n);
         snprintf(out, 9, "%08X", (unsigned)c);
         return 1;
      }
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
