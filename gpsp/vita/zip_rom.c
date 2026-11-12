/* zip_rom.c -- a game kept in a .zip (see zip_rom.h).
 *
 * Only what a zipped cartridge needs: the End Of Central Directory record
 * (searched for in the file's last 64 KiB + 22 bytes, where the format puts
 * it), the central directory's first entry named .gba / .gb / .gbc, its
 * local header, and its data -- stored (method 0) or deflated (method 8,
 * through stb_image's inflater, psp/stb_impl.c).  No ZIP64, no encryption,
 * no multi-disk archives: those are refused, never guessed at.  The result
 * is checked against the entry's CRC-32 before it is used.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zip_rom.h"
#include "fe_util.h"

/* psp/stb_impl.c builds stb_image with its zlib decoder (PNG needs it). */
int stbi_zlib_decode_noheader_buffer(char *obuffer, int olen,
                                     const char *ibuffer, int ilen);

#define ZIP_TAIL_MAX (65535 + 22)

static unsigned rd16(const unsigned char *p)
{
   return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

static unsigned long rd32(const unsigned char *p)
{
   return (unsigned long)p[0] | ((unsigned long)p[1] << 8) |
          ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}

static int name_console(const char *name, size_t len)
{
   static const char *const ext[12] = { ".gba", ".gb", ".gbc", ".sfc", ".smc",
                                        ".nes", ".fds", ".unf", ".unif",
                                        ".ws", ".wsc", ".pc2" };
   int c;
   for (c = 0; c < 12; c++)
   {
      size_t e = strlen(ext[c]), i;
      if (len <= e)
         continue;
      for (i = 0; i < e; i++)
      {
         char a = name[len - e + i];
         if (a >= 'A' && a <= 'Z')
            a = (char)(a + 'a' - 'A');
         if (a != ext[c][i])
            break;
      }
      if (i == e)
         /* .sfc/.smc: 3 (SNES); .nes/.fds/.unf/.unif: 4 (NES);
          * .ws/.wsc/.pc2: 5 (WonderSwan) */
         return c > 8 ? 5 : c > 4 ? 4 : c > 3 ? 3 : c;
   }
   return -1;
}

int zip_rom_peek(const char *path, zip_rom_entry *out)
{
   unsigned char *tail = NULL, *cd = NULL;
   unsigned long cd_size, cd_off, pos;
   long fsize, tail_len, i;
   unsigned entries, k;
   int rc = -1;
   FILE *f;

   if (!path || !out)
      return -1;
   memset(out, 0, sizeof(*out));
   out->console = -1;
   f = fopen(path, "rb");
   if (!f)
      return -1;
   if (fseek(f, 0, SEEK_END) != 0 || (fsize = ftell(f)) < 22)
      goto done;
   tail_len = fsize < ZIP_TAIL_MAX ? fsize : ZIP_TAIL_MAX;
   tail = (unsigned char *)malloc((size_t)tail_len);
   if (!tail || fseek(f, fsize - tail_len, SEEK_SET) != 0 ||
       fread(tail, 1, (size_t)tail_len, f) != (size_t)tail_len)
      goto done;
   for (i = tail_len - 22; i >= 0; i--)
      if (rd32(tail + i) == 0x06054b50ul &&
          (long)(i + 22 + rd16(tail + i + 20)) == tail_len)
         break;
   if (i < 0)
      goto done;
   if (rd16(tail + i + 4) != 0 || rd16(tail + i + 6) != 0)
      goto done;                        /* a multi-disk archive */
   entries = rd16(tail + i + 10);
   cd_size = rd32(tail + i + 12);
   cd_off  = rd32(tail + i + 16);
   if (entries == 0xFFFF || cd_size == 0xFFFFFFFFul ||
       cd_off == 0xFFFFFFFFul || cd_off > (unsigned long)fsize ||
       cd_size > (unsigned long)fsize - cd_off || cd_size > (1ul << 20))
      goto done;                        /* ZIP64, or not a sane directory */
   cd = (unsigned char *)malloc(cd_size ? cd_size : 1);
   if (!cd || fseek(f, (long)cd_off, SEEK_SET) != 0 ||
       fread(cd, 1, cd_size, f) != cd_size)
      goto done;
   for (pos = 0, k = 0; k < entries && pos + 46 <= cd_size; k++)
   {
      const unsigned char *e = cd + pos;
      unsigned nlen, xlen, clen;
      if (rd32(e) != 0x02014b50ul)
         break;
      nlen = rd16(e + 28);
      xlen = rd16(e + 30);
      clen = rd16(e + 32);
      if (pos + 46 + nlen > cd_size)
         break;
      if (nlen && nlen < sizeof(out->name) &&
          !memchr((const char *)e + 46, 0, nlen))   /* no NUL in a name */
      {
         const char *nm = (const char *)e + 46;
         const char *leaf = nm;
         unsigned j;
         int c = name_console(nm, nlen);
         for (j = 0; j < nlen; j++)
            if (nm[j] == '/' || nm[j] == '\\')
               leaf = nm + j + 1;
         if (c >= 0 && leaf < nm + nlen && leaf[0] != '.')
         {
            out->console  = c;
            out->method   = (int)rd16(e + 10);
            out->flags    = rd16(e + 8);
            out->crc      = rd32(e + 16);
            out->csize    = rd32(e + 20);
            out->usize    = rd32(e + 24);
            out->local    = rd32(e + 42);
            memcpy(out->name, nm, nlen);
            out->name[nlen] = '\0';
            rc = (out->flags & 1) || (out->method != 0 && out->method != 8) ||
                 out->usize == 0 || out->usize > ZIP_ROM_MAX ||
                 out->csize == 0xFFFFFFFFul ||
                 out->local >= (unsigned long)fsize ||
                 out->csize > (unsigned long)fsize - out->local
                 ? -2 : 0;
            break;
         }
      }
      pos += 46 + nlen + xlen + clen;
   }
done:
   free(cd);
   free(tail);
   fclose(f);
   return rc;
}

int zip_rom_extract(const char *path, const zip_rom_entry *e,
                    const char *out_path)
{
   unsigned char hdr[30], *in = NULL, *out = NULL;
   char tmp[600];
   unsigned long data_off;
   int rc = -1, n;
   FILE *f, *o;

   if (!path || !e || !out_path || e->console < 0 ||
       snprintf(tmp, sizeof(tmp), "%s.part", out_path) >= (int)sizeof(tmp))
      return -1;
   f = fopen(path, "rb");
   if (!f)
      return -1;
   if (fseek(f, (long)e->local, SEEK_SET) != 0 ||
       fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr) ||
       rd32(hdr) != 0x04034b50ul)
      goto done;
   data_off = e->local + 30 + rd16(hdr + 26) + rd16(hdr + 28);
   {
      /* the data must lie inside the file: no allocation for what isn't */
      long fsize;
      if (fseek(f, 0, SEEK_END) != 0 || (fsize = ftell(f)) < 0 ||
          data_off > (unsigned long)fsize ||
          e->csize > (unsigned long)fsize - data_off ||
          e->usize > ZIP_ROM_MAX || e->csize > 0x7FFFFFFEul)
         goto done;
   }
   out = (unsigned char *)malloc(e->usize);
   in = e->method == 8 ? (unsigned char *)malloc(e->csize + 1) : out;
   if (!out || !in || fseek(f, (long)data_off, SEEK_SET) != 0)
      goto done;
   if (e->method == 0)
   {
      if (e->csize != e->usize ||
          fread(out, 1, e->usize, f) != e->usize)
         goto done;
   }
   else
   {
      if (fread(in, 1, e->csize, f) != e->csize)
         goto done;
      n = stbi_zlib_decode_noheader_buffer((char *)out, (int)e->usize,
                                           (const char *)in, (int)e->csize);
      if (n != (int)e->usize)
         goto done;
   }
   if (fe_crc32(0, out, e->usize) != (uint32_t)e->crc)
   {
      rc = -2;
      goto done;
   }
   o = fopen(tmp, "wb");
   if (!o)
      goto done;
   n = fwrite(out, 1, e->usize, o) == e->usize;
   if (fclose(o) != 0)
      n = 0;
   remove(out_path);
   if (!n || rename(tmp, out_path) != 0)
   {
      remove(tmp);
      goto done;
   }
   rc = 0;
done:
   if (in && in != out)
      free(in);
   free(out);
   fclose(f);
   return rc;
}
