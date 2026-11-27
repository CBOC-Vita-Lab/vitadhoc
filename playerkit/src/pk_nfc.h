/*
 * A small canonical composition for UTF-8 names: a base letter followed by a
 * combining accent becomes the one precomposed character the fonts draw
 * ("e" + U+0301 -> U+00E9), and a kana followed by the combining voiced or
 * semi-voiced sound mark becomes the voiced kana (U+304B + U+3099 -> U+304C).
 * File names that went through a Mac (decomposed form) are full of both; the
 * fonts have no mark positioning, so without this the accent or the two dots
 * of a kana would sit beside the letter instead of on it.
 *
 * In place, never longer. Anything it does not know is left as it is. Not a
 * full Unicode normaliser. Header only, plain C.
 *
 * Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE).
 */
#ifndef PK_NFC_H
#define PK_NFC_H

#include "pk_utf8.h"

#ifdef __cplusplus
extern "C" {
#endif

/* base, combining mark, composed */
static const struct pk_nfc_pair { uint16_t base, mark, out; } pk_nfc_table[] = {
   /* grave 0300 */
   {'A',0x300,0xC0},{'E',0x300,0xC8},{'I',0x300,0xCC},{'O',0x300,0xD2},{'U',0x300,0xD9},
   {'a',0x300,0xE0},{'e',0x300,0xE8},{'i',0x300,0xEC},{'o',0x300,0xF2},{'u',0x300,0xF9},
   /* acute 0301 */
   {'A',0x301,0xC1},{'E',0x301,0xC9},{'I',0x301,0xCD},{'O',0x301,0xD3},{'U',0x301,0xDA},{'Y',0x301,0xDD},
   {'a',0x301,0xE1},{'e',0x301,0xE9},{'i',0x301,0xED},{'o',0x301,0xF3},{'u',0x301,0xFA},{'y',0x301,0xFD},
   {'C',0x301,0x106},{'c',0x301,0x107},{'N',0x301,0x143},{'n',0x301,0x144},{'S',0x301,0x15A},{'s',0x301,0x15B},
   {'Z',0x301,0x179},{'z',0x301,0x17A},
   /* circumflex 0302 */
   {'A',0x302,0xC2},{'E',0x302,0xCA},{'I',0x302,0xCE},{'O',0x302,0xD4},{'U',0x302,0xDB},
   {'a',0x302,0xE2},{'e',0x302,0xEA},{'i',0x302,0xEE},{'o',0x302,0xF4},{'u',0x302,0xFB},
   /* tilde 0303 */
   {'A',0x303,0xC3},{'N',0x303,0xD1},{'O',0x303,0xD5},{'a',0x303,0xE3},{'n',0x303,0xF1},{'o',0x303,0xF5},
   /* macron 0304 */
   {'A',0x304,0x100},{'a',0x304,0x101},{'E',0x304,0x112},{'e',0x304,0x113},{'I',0x304,0x12A},{'i',0x304,0x12B},
   {'O',0x304,0x14C},{'o',0x304,0x14D},{'U',0x304,0x16A},{'u',0x304,0x16B},
   /* diaeresis 0308 */
   {'A',0x308,0xC4},{'E',0x308,0xCB},{'I',0x308,0xCF},{'O',0x308,0xD6},{'U',0x308,0xDC},
   {'a',0x308,0xE4},{'e',0x308,0xEB},{'i',0x308,0xEF},{'o',0x308,0xF6},{'u',0x308,0xFC},{'y',0x308,0xFF},
   /* ring 030A */
   {'A',0x30A,0xC5},{'a',0x30A,0xE5},{'U',0x30A,0x16E},{'u',0x30A,0x16F},
   /* caron 030C */
   {'C',0x30C,0x10C},{'c',0x30C,0x10D},{'D',0x30C,0x10E},{'d',0x30C,0x10F},{'E',0x30C,0x11A},{'e',0x30C,0x11B},
   {'N',0x30C,0x147},{'n',0x30C,0x148},{'R',0x30C,0x158},{'r',0x30C,0x159},{'S',0x30C,0x160},{'s',0x30C,0x161},
   {'T',0x30C,0x164},{'t',0x30C,0x165},{'Z',0x30C,0x17D},{'z',0x30C,0x17E},
   /* cedilla 0327 */
   {'C',0x327,0xC7},{'c',0x327,0xE7},{'S',0x327,0x15E},{'s',0x327,0x15F},
   {0,0,0}
};

/* The voiced (U+3099) or semi-voiced (U+309A) form of a kana, 0 when it has none. */
static inline uint32_t pk_nfc_kana(uint32_t base, uint32_t mark)
{
   uint32_t h = base;
   int kata = 0;
   if (base >= 0x30A1 && base <= 0x30FD)
   {
      h = base - 0x60;
      kata = 1;
   }
   else if (!(base >= 0x3041 && base <= 0x309D))
      return 0;
   if (mark == 0x3099)
   {
      uint32_t o = 0;
      if (h >= 0x304B && h <= 0x3062 && ((h - 0x304B) & 1) == 0)
         o = h + 1;
      else if (h >= 0x3064 && h <= 0x3068 && ((h - 0x3064) & 1) == 0)
         o = h + 1;
      else if (h == 0x306F || h == 0x3072 || h == 0x3075 || h == 0x3078 || h == 0x307B)
         o = h + 1;
      else if (h == 0x3046)
         o = 0x3094;                                  /* ゔ / ヴ */
      else if (h == 0x309D)
         o = 0x309E;                                  /* ゝ -> ゞ */
      if (!o)
         return 0;
      return kata ? o + 0x60 : o;
   }
   if (mark == 0x309A)
   {
      if (h == 0x306F || h == 0x3072 || h == 0x3075 || h == 0x3078 || h == 0x307B)
         return kata ? h + 2 + 0x60 : h + 2;
   }
   return 0;
}

static inline void pk_nfc_lite(char *s)
{
   char *r = s, *w = s;
   uint32_t prev = 0;                 /* the code point last written, when it is one a mark may follow */
   int prev_len = 0;                  /* its length in bytes (it is at w - prev_len) */
   while (*r)
   {
      int n;
      uint32_t cp = pk_u8_next(r, &n);
      if (prev && ((cp >= 0x300 && cp <= 0x36F) || cp == 0x3099 || cp == 0x309A))
      {
         uint32_t out = 0;
         if (cp >= 0x3099)
            out = pk_nfc_kana(prev, cp);
         else
         {
            const struct pk_nfc_pair *t;
            for (t = pk_nfc_table; t->base; t++)
               if (t->base == prev && t->mark == cp)
               {
                  out = t->out;
                  break;
               }
         }
         if (out)
         {
            char tmp[4];
            int m = pk_u8_put(out, tmp), i;
            w -= prev_len;
            for (i = 0; i < m; i++)
               *w++ = tmp[i];
            prev = out;
            prev_len = m;
            r += n;
            continue;
         }
      }
      {
         int i;
         for (i = 0; i < n; i++)
            *w++ = r[i];
      }
      prev = cp < PK_U8_RAW ? cp : 0;
      prev_len = n;
      r += n;
   }
   *w = '\0';
}

#ifdef __cplusplus
}
#endif

#endif
