/*
 * playerkit: .m3u disc lists (a game on several discs), plain C, header only.
 *
 * An .m3u is a text file with one disc image (.cue, .chd or .iso) per line. Lines starting with # are comments (#EXTM3U, #EXTINF), empty lines
 * and lines with another kind of file are ignored, line ends may be LF, CRLF or CR, a byte order mark at the start is skipped, and a path may
 * use \ as well as /. A relative path is read from the folder of the .m3u; "./" and "name/../" are folded away. A line that is not an image
 * the list can use is skipped. Discs that are not there are reported by pk_m3u_load, not dropped silently: the caller says so.
 *
 * Used by the game list (only a list that names discs is a game; the discs it names are not listed on their own), the Sega CD player (the discs of the game) and the game id of
 * Two players. No allocation.
 *
 * Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE).
 */
#ifndef PK_M3U_H
#define PK_M3U_H

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define PK_M3U_PATH 512          /* longest path kept (longer lines are skipped) */
#define PK_M3U_MAX_DISCS 8
#define PK_M3U_MAX_BYTES (64u * 1024u)   /* a larger file is not read */

#ifdef __cplusplus
extern "C" {
#endif

static inline int pk_m3u_ext_is(const char *path, const char *ext)
{
   size_t n = path ? strlen(path) : 0, e = strlen(ext);
   return n > e && strcasecmp(path + n - e, ext) == 0;
}

/* a file name the list may name: a disc image */
static inline int pk_m3u_is_disc(const char *path)
{
   return pk_m3u_ext_is(path, ".cue") || pk_m3u_ext_is(path, ".chd") || pk_m3u_ext_is(path, ".iso");
}

static inline int pk_m3u_is_list(const char *path) { return pk_m3u_ext_is(path, ".m3u"); }

/* Folds "." and ".." segments and doubled slashes out of a path in place; a leading "name:" (ux0:) or "/" is kept as the root. */
static inline void pk_m3u_fold(char *p)
{
   char *root_end = p, *r, *w;
   char *c = strchr(p, ':');
   char *s = strchr(p, '/');
   if (c && (!s || c < s))
      root_end = c + 1;
   if (*root_end == '/')
      root_end++;
   for (r = w = root_end; *r;)
   {
      char *e = strchr(r, '/');
      size_t l = e ? (size_t)(e - r) : strlen(r);
      if (l == 0 || (l == 1 && r[0] == '.'))
         ;                                              /* "//" or "./" */
      else if (l == 2 && r[0] == '.' && r[1] == '.' && w > root_end)
      {
         char *b = w - 1;                               /* drop the segment before it (w is just after its "/") */
         if (b > root_end)
            for (b--; b >= root_end && *b != '/'; b--)
               ;
         if (b < root_end)
            b = root_end - 1;
         w = b + 1;
      }
      else
      {
         if (w != r)
            memmove(w, r, l);
         w += l;
         if (e)
            *w++ = '/';
      }
      if (!e)
         break;
      r = e + 1;
   }
   *w = '\0';
}

/* Reads the lines of `text` (n bytes): up to `max` image paths into out, each resolved against the folder of m3u_path. Returns the count;
 * *skipped (may be NULL) gets the number of non-comment lines that were not used (another kind of file, too long, too many). */
static inline int pk_m3u_parse(const char *text, size_t n, const char *m3u_path, char (*out)[PK_M3U_PATH], int max, int *skipped)
{
   size_t pos = 0, dl = 0;
   int count = 0, skip = 0, first = 1;
   const char *sl, *co;
   if (skipped)
      *skipped = 0;
   if (!text || !m3u_path || !out || max <= 0)
      return 0;
   sl = strrchr(m3u_path, '/');
   co = strrchr(m3u_path, ':');
   if (co && (!sl || co > sl))
      sl = co;
   dl = sl ? (size_t)(sl - m3u_path) + 1 : 0;
   while (pos < n)
   {
      char line[PK_M3U_PATH];
      size_t ll = 0, k;
      int too_long = 0, absolute;
      char *q, *e;
      while (pos < n && text[pos] != '\n' && text[pos] != '\r')
      {
         if (ll + 1 < sizeof(line))
            line[ll++] = text[pos];
         else
            too_long = 1;
         pos++;
      }
      while (pos < n && (text[pos] == '\n' || text[pos] == '\r'))
         pos++;
      line[ll] = '\0';
      q = line;
      if (first && ll >= 3 && (unsigned char)q[0] == 0xEF && (unsigned char)q[1] == 0xBB && (unsigned char)q[2] == 0xBF)
         q += 3;                                        /* a UTF-8 byte order mark */
      first = 0;
      while (*q == ' ' || *q == '\t')
         q++;
      e = q + strlen(q);
      while (e > q && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\0'))
         *--e = '\0';
      if (!*q || *q == '#')
         continue;
      for (k = 0; q[k]; k++)
         if (q[k] == '\\')
            q[k] = '/';
      if (too_long || !pk_m3u_is_disc(q) || count >= max)
      {
         skip++;
         continue;
      }
      absolute = q[0] == '/' || strchr(q, ':') != NULL;
      if (absolute)
      {
         if (strlen(q) >= PK_M3U_PATH)
         {
            skip++;
            continue;
         }
         strcpy(out[count], q);
      }
      else
      {
         if (dl + strlen(q) >= PK_M3U_PATH)
         {
            skip++;
            continue;
         }
         memcpy(out[count], m3u_path, dl);
         strcpy(out[count] + dl, q);
      }
      pk_m3u_fold(out[count]);
      count++;
   }
   if (skipped)
      *skipped = skip;
   return count;
}

/* Reads the file: the discs it names, in order. *missing (may be NULL) = how many of them are not files. -1 = the .m3u could not be read
 * (or is larger than PK_M3U_MAX_BYTES). The discs that are missing stay in the list (the caller names them). */
static inline int pk_m3u_load(const char *m3u_path, char (*out)[PK_M3U_PATH], int max, int *missing, int *skipped)
{
   static char buf[PK_M3U_MAX_BYTES];                  /* one reader at a time: the scan, the player and the id calls are single threaded */
   FILE *f;
   size_t n;
   int count, i;
   if (missing)
      *missing = 0;
   if (skipped)
      *skipped = 0;
   f = m3u_path ? fopen(m3u_path, "rb") : NULL;
   if (!f)
      return -1;
   n = fread(buf, 1, sizeof(buf), f);
   if (n == sizeof(buf) && fgetc(f) != EOF)
   {
      fclose(f);
      return -1;
   }
   fclose(f);
   count = pk_m3u_parse(buf, n, m3u_path, out, max, skipped);
   for (i = 0; i < count && missing; i++)
   {
      struct stat st;
      if (stat(out[i], &st) != 0 || !S_ISREG(st.st_mode))
         (*missing)++;
   }
   return count;
}

/* 1 when the file is an .m3u with at least one line that names a disc image (.cue, .chd, .iso): a Sega CD game's list.  A music playlist, another
 * system's list (.bin, .pbp, .gba) or an unreadable or empty file is not one; discs that are missing still count (the player says so). */
static inline int pk_m3u_names_discs(const char *m3u_path)
{
   static char one[1][PK_M3U_PATH];
   return pk_m3u_load(m3u_path, one, 1, NULL, NULL) >= 1;
}

#ifdef __cplusplus
}
#endif
#endif
