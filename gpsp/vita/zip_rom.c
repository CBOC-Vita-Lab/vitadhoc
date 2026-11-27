/* zip_rom.c -- a game kept in a .zip (see zip_rom.h).
 *
 * Only what a zipped cartridge needs: the End Of Central Directory record
 * (searched for in the file's last 64 KiB + 22 bytes, where the format puts
 * it), the central directory's first entry named .gba / .gb / .gbc, its
 * local header, and its data -- stored (method 0) or deflated (method 8,
 * through stb_image's inflater, psp/stb_impl.c).  No ZIP64, no encryption,
 * no multi-disk archives: those are refused, never guessed at.  The result
 * is checked against the entry's CRC-32 before it is used.
 *
 * A file that begins with the 7z signature goes to sz_rom.c instead (the same
 * entry record, method 7).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "zip_rom.h"
#include "fe_util.h"
#include "sz_rom.h"

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
   static const char *const ext[21] = { ".gba", ".gb", ".gbc", ".sfc", ".smc",
                                        ".nes", ".fds", ".unf", ".unif",
                                        ".ws", ".wsc", ".pc2",
                                        ".md", ".gen", ".smd", ".bin",
                                        ".sms", ".gg", ".sg", ".68k", ".sc" };
   int c;
   for (c = 0; c < 21; c++)
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
          * .ws/.wsc/.pc2: 5 (WonderSwan); .md/.gen/.smd/.bin: 6 (Genesis);
          * .sms: 7; .gg: 8; .sg: 9; .68k: 6 (raw Genesis); .sc: 9 (SC-3000, run
          * as the SG-1000 family) (fe_console_t) */
         return c == 19 ? 6 : c == 20 ? 9 : c > 17 ? 9 : c > 16 ? 8 : c > 15 ? 7 : c > 11 ? 6 : c > 8 ? 5 : c > 4 ? 4 : c > 3 ? 3 : c;
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
   {
      /* the 7z signature: the first six bytes (the file is opened once for a zip) */
      static const unsigned char sig7z[6] = { '7', 'z', 0xBC, 0xAF, 0x27, 0x1C };
      unsigned char sig[6];
      if (fread(sig, 1, 6, f) == 6 && !memcmp(sig, sig7z, 6))
      {
         fclose(f);
         return sz_rom_peek(path, out, name_console);
      }
   }
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
         /* a Genesis ROM is never under 16 KiB: a README.md or a .bin of a few bytes beside the game is no game */
         if (c == 6 && rd32(e + 24) < 0x4000ul)
            c = -1;
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

/* Validate the local file header and seek to the start of its payload. */
static FILE *zip_open_entry(const char *path, const zip_rom_entry *e,
                            unsigned long *data_off_out)
{
   unsigned char hdr[30];
   unsigned long data_off;
   long fsize;
   FILE *f;

   if (!path || !e || e->console < 0)
      return NULL;
   f = fopen(path, "rb");
   if (!f)
      return NULL;
   if (fseek(f, (long)e->local, SEEK_SET) != 0 ||
       fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr) ||
       rd32(hdr) != 0x04034b50ul)
      goto fail;
   data_off = e->local + 30 + rd16(hdr + 26) + rd16(hdr + 28);
   if (fseek(f, 0, SEEK_END) != 0 || (fsize = ftell(f)) < 0 ||
       data_off > (unsigned long)fsize ||
       e->csize > (unsigned long)fsize - data_off ||
       e->usize > ZIP_ROM_MAX || e->csize > 0x7FFFFFFEul ||
       fseek(f, (long)data_off, SEEK_SET) != 0)
      goto fail;
   if (data_off_out)
      *data_off_out = data_off;
   return f;
fail:
   fclose(f);
   return NULL;
}

/* The entry's data, inflated and checked against its CRC-32: malloc'd in *data (the caller frees), usize bytes.
 * 0 = done; -2 = the data does not match its CRC; -1 = could not. */
static int zip_rom_load(const char *path, const zip_rom_entry *e, unsigned char **data)
{
   unsigned char *in = NULL, *out = NULL;
   int rc = -1, n;
   FILE *f;

   if (!data)
      return -1;
   *data = NULL;
   f = zip_open_entry(path, e, NULL);
   if (!f)
      return -1;
   out = (unsigned char *)malloc(e->usize);
   in = e->method == 8 ? (unsigned char *)malloc(e->csize + 1) : out;
   if (!out || !in)
      goto done;
   if (e->method == 0)
   {
      if (e->csize != e->usize ||
          fread(out, 1, e->usize, f) != e->usize)
         goto done;
   }
   else if (e->method == 8)
   {
      if (fread(in, 1, e->csize, f) != e->csize)
         goto done;
      n = stbi_zlib_decode_noheader_buffer((char *)out, (int)e->usize,
                                           (const char *)in, (int)e->csize);
      if (n != (int)e->usize)
         goto done;
   }
   else
      goto done;
   if (fe_crc32(0, out, e->usize) != (uint32_t)e->crc)
   {
      rc = -2;
      goto done;
   }
   rc = 0;
done:
   if (in && in != out)
      free(in);
   if (rc == 0)
      *data = out;                      /* the caller's now */
   else
      free(out);
   fclose(f);
   return rc;
}

/* ---- Streaming RFC 1951 inflate for zip_rom_head() ---- */

#define ZINF_WINSIZE 32768u
#define ZINF_INBUF   4096u
#define ZINF_MAXBITS 15
#define ZINF_MAXSYMS 288

typedef struct
{
   uint16_t count[ZINF_MAXBITS + 1];
   uint16_t sym[ZINF_MAXSYMS];
} zinf_huff;

typedef struct
{
   FILE         *f;
   unsigned long rem;
   uint8_t      *ibuf;
   size_t        ipos, ilen;
   uint32_t      bb;
   int           nbits;
   uint8_t      *win;
   size_t        total_out;
   uint8_t      *out;
   unsigned      want;
   unsigned      got;
} zinf_state;

static int zinf_fill_byte(zinf_state *s)
{
   if (s->ipos >= s->ilen)
   {
      size_t chunk, rd;
      if (s->rem == 0)
         return -1;
      chunk = s->rem < ZINF_INBUF ? (size_t)s->rem : ZINF_INBUF;
      rd = fread(s->ibuf, 1, chunk, s->f);
      if (rd == 0)
         return -1;
      s->rem -= (unsigned long)rd;
      s->ipos = 0;
      s->ilen = rd;
   }
   return (int)s->ibuf[s->ipos++];
}

static int zinf_bits(zinf_state *s, int n)
{
   int v;
   while (s->nbits < n)
   {
      int b = zinf_fill_byte(s);
      if (b < 0)
         return -1;
      s->bb |= (uint32_t)b << s->nbits;
      s->nbits += 8;
   }
   v = (int)(s->bb & ((1u << n) - 1u));
   s->bb >>= n;
   s->nbits -= n;
   return v;
}

static int zinf_build(zinf_huff *h, const uint8_t *lens, int nsyms)
{
   uint16_t offs[ZINF_MAXBITS + 1];
   int i, len, left = 1;
   for (len = 0; len <= ZINF_MAXBITS; len++)
      h->count[len] = 0;
   for (i = 0; i < nsyms; i++)
   {
      if (lens[i] > ZINF_MAXBITS)
         return -1;
      h->count[lens[i]]++;
   }
   if (h->count[0] == nsyms)
      return 0;
   for (len = 1; len <= ZINF_MAXBITS; len++)
   {
      left <<= 1;
      left -= h->count[len];
      if (left < 0)
         return -1;
   }
   offs[1] = 0;
   for (len = 1; len < ZINF_MAXBITS; len++)
      offs[len + 1] = (uint16_t)(offs[len] + h->count[len]);
   for (i = 0; i < nsyms; i++)
      if (lens[i] != 0)
         h->sym[offs[lens[i]]++] = (uint16_t)i;
   return 0;
}

static int zinf_decode(zinf_state *s, const zinf_huff *h)
{
   int code = 0, first = 0, index = 0, len;
   for (len = 1; len <= ZINF_MAXBITS; len++)
   {
      int bit = zinf_bits(s, 1);
      int count;
      if (bit < 0)
         return -1;
      code |= bit;
      count = h->count[len];
      if (code - count < first)
         return (int)h->sym[index + (code - first)];
      index += count;
      first += count;
      first <<= 1;
      code <<= 1;
   }
   return -1;
}

static void zinf_emit(zinf_state *s, uint8_t b)
{
   s->win[s->total_out & (ZINF_WINSIZE - 1u)] = b;
   s->total_out++;
   if (s->got < s->want)
      s->out[s->got++] = b;
}

static int zinf_dynamic(zinf_state *s, zinf_huff *lit, zinf_huff *dist)
{
   static const uint8_t order[19] = {
      16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
   };
   uint8_t clens[19], lens[288 + 32];
   zinf_huff ch;
   int nlen, ndist, ncode, i, idx = 0;

   nlen = zinf_bits(s, 5);
   ndist = zinf_bits(s, 5);
   ncode = zinf_bits(s, 4);
   if (nlen < 0 || ndist < 0 || ncode < 0)
      return -1;
   nlen += 257;
   ndist += 1;
   ncode += 4;
   if (nlen > 288 || ndist > 32)
      return -1;
   memset(clens, 0, sizeof(clens));
   for (i = 0; i < ncode; i++)
   {
      int b = zinf_bits(s, 3);
      if (b < 0)
         return -1;
      clens[order[i]] = (uint8_t)b;
   }
   if (zinf_build(&ch, clens, 19) != 0)
      return -1;
   while (idx < nlen + ndist)
   {
      int sym = zinf_decode(s, &ch);
      int rep, val;
      if (sym < 0)
         return -1;
      if (sym < 16)
      {
         lens[idx++] = (uint8_t)sym;
         continue;
      }
      if (sym == 16)
      {
         int ex = zinf_bits(s, 2);
         if (idx == 0 || ex < 0)
            return -1;
         val = lens[idx - 1];
         rep = 3 + ex;
      }
      else if (sym == 17)
      {
         int ex = zinf_bits(s, 3);
         if (ex < 0)
            return -1;
         val = 0;
         rep = 3 + ex;
      }
      else
      {
         int ex = zinf_bits(s, 7);
         if (ex < 0)
            return -1;
         val = 0;
         rep = 11 + ex;
      }
      if (idx + rep > nlen + ndist)
         return -1;
      while (rep-- > 0)
         lens[idx++] = (uint8_t)val;
   }
   if (zinf_build(lit, lens, nlen) != 0 ||
       zinf_build(dist, lens + nlen, ndist) != 0)
      return -1;
   return 0;
}

static int zinf_codes(zinf_state *s, const zinf_huff *lit, const zinf_huff *dist)
{
   static const uint16_t lbase[29] = {
      3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
      35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
   };
   static const uint8_t lextra[29] = {
      0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
      3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
   };
   static const uint16_t dbase[30] = {
      1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
      257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
      8193, 12289, 16385, 24577
   };
   static const uint8_t dextra[30] = {
      0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
      7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
   };

   while (s->got < s->want)
   {
      int sym = zinf_decode(s, lit);
      if (sym < 0)
         return -1;
      if (sym < 256)
      {
         zinf_emit(s, (uint8_t)sym);
      }
      else if (sym == 256)
      {
         return 0;
      }
      else
      {
         int li, ex, mlen, dsym, dex;
         unsigned mdist;
         if (sym > 285)
            return -1;
         li = sym - 257;
         ex = lextra[li] ? zinf_bits(s, lextra[li]) : 0;
         if (ex < 0)
            return -1;
         mlen = (int)lbase[li] + ex;

         dsym = zinf_decode(s, dist);
         if (dsym < 0 || dsym >= 30)
            return -1;
         dex = dextra[dsym] ? zinf_bits(s, dextra[dsym]) : 0;
         if (dex < 0)
            return -1;
         mdist = (unsigned)dbase[dsym] + (unsigned)dex;
         if (mdist == 0 || (size_t)mdist > s->total_out)
            return -1;
         while (mlen-- > 0 && s->got < s->want)
         {
            uint8_t b = s->win[(s->total_out - mdist) & (ZINF_WINSIZE - 1u)];
            zinf_emit(s, b);
         }
      }
   }
   return 0;
}

static int zip_inflate_head(FILE *f, unsigned long csize, unsigned char *buf,
                            unsigned want)
{
   zinf_state s;
   zinf_huff lit, dist;
   int rc = -1, bfinal = 0;

   memset(&s, 0, sizeof(s));
   s.f = f;
   s.rem = csize;
   s.out = buf;
   s.want = want;
   s.ibuf = (uint8_t *)malloc(ZINF_INBUF);
   s.win = (uint8_t *)malloc(ZINF_WINSIZE);
   if (!s.ibuf || !s.win)
      goto out;

   while (s.got < s.want)
   {
      int btype;
      bfinal = zinf_bits(&s, 1);
      btype = zinf_bits(&s, 2);
      if (bfinal < 0 || btype < 0 || btype == 3)
         goto out;
      if (btype == 0)
      {
         int b0, b1, b2, b3;
         unsigned len, nlen;
         if (s.nbits & 7)
         {
            int drop = s.nbits & 7;
            s.bb >>= drop;
            s.nbits -= drop;
         }
         b0 = zinf_bits(&s, 8);
         b1 = zinf_bits(&s, 8);
         b2 = zinf_bits(&s, 8);
         b3 = zinf_bits(&s, 8);
         if (b0 < 0 || b1 < 0 || b2 < 0 || b3 < 0)
            goto out;
         len = (unsigned)b0 | ((unsigned)b1 << 8);
         nlen = (unsigned)b2 | ((unsigned)b3 << 8);
         if (nlen != (len ^ 0xFFFFu))
            goto out;
         while (len-- > 0 && s.got < s.want)
         {
            int c = zinf_bits(&s, 8);
            if (c < 0)
               goto out;
            zinf_emit(&s, (uint8_t)c);
         }
      }
      else
      {
         if (btype == 1)
         {
            uint8_t fl[288], dl[32];
            int i;
            for (i = 0; i <= 143; i++) fl[i] = 8;
            for (; i <= 255; i++)      fl[i] = 9;
            for (; i <= 279; i++)      fl[i] = 7;
            for (; i <= 287; i++)      fl[i] = 8;
            for (i = 0; i < 32; i++)   dl[i] = 5;
            if (zinf_build(&lit, fl, 288) != 0 || zinf_build(&dist, dl, 32) != 0)
               goto out;
         }
         else if (zinf_dynamic(&s, &lit, &dist) != 0)
            goto out;
         if (zinf_codes(&s, &lit, &dist) != 0)
            goto out;
      }
      if (bfinal)
         break;
   }
   if (s.got == s.want)
      rc = (int)s.got;
out:
   free(s.win);
   free(s.ibuf);
   return rc;
}

int zip_rom_head(const char *path, const zip_rom_entry *e, unsigned char *buf, unsigned n)
{
   unsigned want;
   int rc = -1;
   FILE *f;
   if (e && e->method == 7)
      return sz_rom_head(path, e, buf, n);
   if (!buf || !e)
      return -1;
   want = e->usize < n ? (unsigned)e->usize : n;
   f = zip_open_entry(path, e, NULL);
   if (!f)
      return -1;
   if (want == 0)
      rc = 0;
   else if (e->method == 0)
   {
      if (e->csize == e->usize && fread(buf, 1, want, f) == want)
         rc = (int)want;
   }
   else if (e->method == 8)
      rc = zip_inflate_head(f, e->csize, buf, want);
   fclose(f);
   return rc;
}

/* Two-step .old atomic replace on Vita FAT32 after tmp is fully written and closed. */
static int zip_promote(const char *tmp, const char *out_path)
{
   char old[608];
   struct stat st;
   int had = stat(out_path, &st) == 0;
   if (snprintf(old, sizeof(old), "%s.old", out_path) >= (int)sizeof(old))
      return -1;
   if (had)
   {
      remove(old);
      if (rename(out_path, old) != 0 || stat(out_path, &st) == 0)
         return -1;
   }
   (void)rename(tmp, out_path);
   if (stat(out_path, &st) != 0)
   {
      if (had)
         (void)rename(old, out_path);
      return -1;
   }
   if (had)
      remove(old);
   return 0;
}

int zip_rom_extract(const char *path, const zip_rom_entry *e,
                    const char *out_path)
{
   unsigned char *out = NULL;
   char tmp[600];
   int rc, ok;
   FILE *o;

   if (e && e->method == 7)
      return sz_rom_extract(path, e, out_path);
   if (!out_path || snprintf(tmp, sizeof(tmp), "%s.part", out_path) >= (int)sizeof(tmp))
      return -1;
   rc = zip_rom_load(path, e, &out);
   if (rc != 0)
      return rc;
   rc = -1;
   o = fopen(tmp, "wb");
   if (!o)
      goto done;
   ok = fwrite(out, 1, e->usize, o) == e->usize;
   if (fclose(o) != 0)
      ok = 0;
   if (ok && zip_promote(tmp, out_path) == 0)
      rc = 0;
   else
      remove(tmp);
done:
   free(out);
   return rc;
}
