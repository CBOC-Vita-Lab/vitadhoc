/*
 * UTF-8 helpers: one place that knows where a character starts and ends, so
 * nothing that measures, cuts, wraps or draws a name ever splits a
 * multi-byte character. Header only, plain C (the main app's C files and the
 * players' C++ include it alike); every function is static inline.
 *
 * File names on the Vita's file system, game titles, cheat and achievement
 * names are UTF-8. A byte string that is not valid UTF-8 is read byte by byte
 * (pk_u8_next gives such a byte back as PK_U8_RAW + the byte, so a caller
 * that has its own meaning for a lone byte, the UI's button glyphs 0x7F..0x84,
 * can still see it).
 *
 * Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE).
 */
#ifndef PK_UTF8_H
#define PK_UTF8_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A byte that is not part of a valid sequence comes back as PK_U8_RAW + byte. */
#define PK_U8_RAW 0x110000u

/* The code point that starts at s (s must be NUL-terminated), and its length
 * in bytes in *len (1..4). An overlong form, a surrogate, a value past
 * U+10FFFF, a stray continuation byte or a sequence cut off by the end of the
 * string is not valid: PK_U8_RAW + the first byte, length 1. */
static inline uint32_t pk_u8_next(const char *s, int *len)
{
   const unsigned char *p = (const unsigned char *)s;
   unsigned c = p[0];
   if (c < 0x80)
   {
      *len = 1;
      return c;
   }
   if (c >= 0xC2 && c <= 0xDF && (p[1] & 0xC0) == 0x80)
   {
      *len = 2;
      return ((c & 0x1Fu) << 6) | (p[1] & 0x3Fu);
   }
   if (c >= 0xE0 && c <= 0xEF && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80)
   {
      uint32_t v = ((c & 0x0Fu) << 12) | ((p[1] & 0x3Fu) << 6) | (p[2] & 0x3Fu);
      if (v >= 0x800 && (v < 0xD800 || v > 0xDFFF))
      {
         *len = 3;
         return v;
      }
   }
   if (c >= 0xF0 && c <= 0xF4 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 &&
       (p[3] & 0xC0) == 0x80)
   {
      uint32_t v = ((c & 0x07u) << 18) | ((p[1] & 0x3Fu) << 12) |
                   ((p[2] & 0x3Fu) << 6) | (p[3] & 0x3Fu);
      if (v >= 0x10000 && v <= 0x10FFFF)
      {
         *len = 4;
         return v;
      }
   }
   *len = 1;
   return PK_U8_RAW + c;
}

/* 1 when s (at the lead byte of a sequence pk_u8_next refused) is the front
 * of a character the end of the string cut off: a lead byte and the
 * continuation bytes that follow it, then the terminator. A name cut to fit
 * a buffer ends like that; it is shown as nothing, not as a '?'. */
static inline int pk_u8_cut_tail(const char *s)
{
   const unsigned char *p = (const unsigned char *)s;
   int want, have = 0;
   if (p[0] >= 0xC2 && p[0] <= 0xDF)
      want = 1;
   else if (p[0] >= 0xE0 && p[0] <= 0xEF)
      want = 2;
   else if (p[0] >= 0xF0 && p[0] <= 0xF4)
      want = 3;
   else
      return 0;
   p++;
   while (have < want && (*p & 0xC0) == 0x80)
   {
      p++;
      have++;
   }
   return have < want && *p == '\0';
}

/* A code point as UTF-8 into out (room for 4 bytes); returns the length.
 * Anything not encodable (a surrogate, past U+10FFFF) is written as '?'. */
static inline int pk_u8_put(uint32_t cp, char *out)
{
   if (cp < 0x80)
   {
      out[0] = (char)cp;
      return 1;
   }
   if (cp < 0x800)
   {
      out[0] = (char)(0xC0 | (cp >> 6));
      out[1] = (char)(0x80 | (cp & 0x3F));
      return 2;
   }
   if (cp >= 0xD800 && cp <= 0xDFFF)
   {
      out[0] = '?';
      return 1;
   }
   if (cp < 0x10000)
   {
      out[0] = (char)(0xE0 | (cp >> 12));
      out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
      out[2] = (char)(0x80 | (cp & 0x3F));
      return 3;
   }
   if (cp <= 0x10FFFF)
   {
      out[0] = (char)(0xF0 | (cp >> 18));
      out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
      out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
      out[3] = (char)(0x80 | (cp & 0x3F));
      return 4;
   }
   out[0] = '?';
   return 1;
}

/* The length in bytes of the character that STARTS at s (0 at the end). */
static inline size_t pk_u8_len(const char *s)
{
   int n;
   if (!*s)
      return 0;
   (void)pk_u8_next(s, &n);
   return (size_t)n;
}

/* Where the character before p begins (p > start): the start of the valid
 * sequence that ends at p, else p - 1 (a lone byte). */
static inline const char *pk_u8_prev(const char *start, const char *p)
{
   const char *q = p - 1;
   int back = 0;
   while (q > start && back < 3 && ((unsigned char)*q & 0xC0) == 0x80)
   {
      q--;
      back++;
   }
   if (q + pk_u8_len(q) == p)
      return q;
   return p - 1;
}

/* The largest k <= n such that s[0..k) ends on a character boundary: use it
 * to cut a copy at a byte count without leaving half a character. s must be
 * at least n bytes long (or NUL-terminated before that). */
static inline size_t pk_u8_floor(const char *s, size_t n)
{
   size_t k = 0;
   while (s[k])
   {
      size_t l = pk_u8_len(s + k);
      if (k + l > n)
         break;
      k += l;
   }
   return k < n ? k : n;
}

/* Cut s (NUL-terminated) to at most max bytes WITHOUT its terminator, on a
 * character boundary; returns the new length. */
static inline size_t pk_u8_clip(char *s, size_t max)
{
   size_t k = pk_u8_floor(s, max);
   s[k] = '\0';
   return k;
}

/* snprintf's other half: the text of src copied into dst (cap bytes with
 * the terminator), cut on a character boundary. */
static inline void pk_u8_copy(char *dst, size_t cap, const char *src)
{
   size_t n = 0, k;
   if (!dst || !cap)
      return;
   if (!src)
      src = "";
   while (src[n])
      n++;
   k = pk_u8_floor(src, n < cap - 1 ? n : cap - 1);
   {
      size_t i;
      for (i = 0; i < k; i++)
         dst[i] = src[i];
   }
   dst[k] = '\0';
}

/* 1 for a character that is written without spaces between words (kana,
 * kanji, Hangul, fullwidth forms): a line may break either side of it. */
static inline int pk_u8_wide(uint32_t cp)
{
   return (cp >= 0x2E80 && cp <= 0xD7AF) || (cp >= 0xF900 && cp <= 0xFAFF) ||
          (cp >= 0xFF00 && cp <= 0xFFEF) || (cp >= 0x20000 && cp <= 0x3FFFF);
}

/* 1 for a character that may not start a line (closing marks, the
 * prolonged sound mark, small kana, iteration marks, the closing quotes and
 * the ellipsis of Chinese text): it stays with the one before. */
static inline int pk_u8_no_line_start(uint32_t cp)
{
   switch (cp)
   {
   case 0x3001: case 0x3002: case 0xFF0C: case 0xFF0E: case 0x300D: case 0x300F:
   case 0xFF09: case 0x3011: case 0x3015: case 0xFF01: case 0xFF1F: case 0x30FC:
   case 0x3099: case 0x309A: case 0x30FB: case 0xFF1A: case 0xFF1B:
   case 0x3009: case 0x300B: case 0x3017: case 0x3019: case 0x301B: case 0xFF3D:
   case 0xFF5D: case 0xFF05: case 0xFF61: case 0xFF63: case 0xFF64: case 0xFF65:
   case 0x309B: case 0x309C: case 0x309D: case 0x309E: case 0x30FD: case 0x30FE:
   case 0x301C: case 0x30A0: case 0x2019: case 0x201D: case 0x2025: case 0x2026:
   case 0x3041: case 0x3043: case 0x3045: case 0x3047: case 0x3049: case 0x3063:
   case 0x3083: case 0x3085: case 0x3087: case 0x308E: case 0x3095: case 0x3096:
   case 0x30A1: case 0x30A3: case 0x30A5: case 0x30A7: case 0x30A9: case 0x30C3:
   case 0x30E3: case 0x30E5: case 0x30E7: case 0x30EE: case 0x30F5: case 0x30F6:
      return 1;
   }
   return cp >= 0x31F0 && cp <= 0x31FF;      /* the small katakana extensions */
}

/* 1 for a character that may not END a line (opening brackets and quotes):
 * it stays with the one after. */
static inline int pk_u8_no_line_end(uint32_t cp)
{
   switch (cp)
   {
   case 0x3008: case 0x300A: case 0x300C: case 0x300E: case 0x3010: case 0x3014:
   case 0x3016: case 0x3018: case 0x301A: case 0xFF08: case 0xFF3B: case 0xFF5B:
   case 0xFF62: case 0x2018: case 0x201C: case 0xFFE1: case 0xFFE5: case 0xFF04:
      return 1;
   }
   return 0;
}

/* Where a line may break WITHOUT a space: between two characters, `before`
 * then `after` (code points; a space is the caller's own business).  Words
 * of Latin letters are never broken; a character written without spaces
 * (pk_u8_wide) may be broken from either neighbour, except that a line
 * never starts with a closing mark or ends with an opening one, and the
 * ASCII punctuation that closes a phrase (, . ! ? : ; ) ] } %) stays with
 * the CJK character before it. */
static inline int pk_u8_can_break(uint32_t before, uint32_t after)
{
   if (!pk_u8_wide(before) && !pk_u8_wide(after))
      return 0;
   if (pk_u8_no_line_start(after) || pk_u8_no_line_end(before))
      return 0;
   if (after < 0x80 && after && strchr(",.!?:;)]}%'\"", (int)after))
      return 0;
   if (before < 0x80 && before && strchr("([{$", (int)before))
      return 0;
   return 1;
}

/* How many characters s holds. */
static inline size_t pk_u8_count(const char *s)
{
   size_t n = 0;
   while (*s)
   {
      s += pk_u8_len(s);
      n++;
   }
   return n;
}

/* The small letter of a capital in ASCII, Latin-1 Supplement and Latin
 * Extended-A ("È" -> "è", "Œ" -> "œ", "Ÿ" -> "ÿ"); any other code point
 * comes back as it is.  (Sentence case of titles: "PARAMÈTRES" -> "Paramètres".) */
static inline uint32_t pk_u8_lower(uint32_t cp)
{
   if (cp >= 'A' && cp <= 'Z')
      return cp + 32;
   if (cp >= 0xC0 && cp <= 0xDE && cp != 0xD7)       /* À..Þ, not × */
      return cp + 32;
   if (cp == 0x130)                                  /* İ */
      return 'i';
   if (cp == 0x178)                                  /* Ÿ */
      return 0xFF;
   if ((cp >= 0x100 && cp <= 0x137) || (cp >= 0x14A && cp <= 0x177))
      return (cp & 1) ? cp : cp + 1;                 /* capitals on even code points */
   if ((cp >= 0x139 && cp <= 0x148) || (cp >= 0x179 && cp <= 0x17E))
      return (cp & 1) ? cp + 1 : cp;                 /* capitals on odd code points */
   return cp;
}

/* 1 for a letter of ASCII, Latin-1 Supplement or Latin Extended-A/B. */
static inline int pk_u8_is_latin_letter(uint32_t cp)
{
   return (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') ||
          (cp >= 0xC0 && cp <= 0x24F && cp != 0xD7 && cp != 0xF7);
}

#ifdef __cplusplus
}
#endif

#endif
