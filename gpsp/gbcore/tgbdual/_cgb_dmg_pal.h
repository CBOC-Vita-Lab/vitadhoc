/* Copyright (c) 2026 VitAdhoc contributors.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The Game Boy Color's colours for original Game Boy games (issue #34).
 *
 * A GBC running a DMG cartridge colours it with one of 51 palette
 * combinations (BG, OBJ0, OBJ1).  Its boot ROM picks the combination from
 * the cartridge header: only for Nintendo-licensed games (old licensee 0x01,
 * or 0x33 with new licensee "01"), by the sum of the 16 title bytes, and for
 * a few colliding sums by the title's 4th letter as well.  Anything else
 * gets combination 0.  A player can instead hold one of 12 d-pad(+A/B)
 * combinations at boot.
 *
 * This is DATA only, no boot-ROM code: the "Assigned Palette
 * Configurations" table (tcrf.net, Notes:Game_Boy_Color_Bootstrap_ROM) in
 * the layout SameBoy's clean-room boot ROM uses for it (BootROMs/
 * cgb_boot.asm, MIT/Expat, Copyright (c) 2015-2026 Lior Halphon).  SameBoy's
 * own extra palettes (its "exclusives", 30-31 and combinations 51-54) are
 * left out.
 *
 * The 32 Super Game Boy system palettes (1-A .. 4-H) follow, as the
 * Super Game Boy's own menu lists them (colour values as the libretro
 * Gambatte core, GPL-2, lists them, Copyright (C) 2007 Sindre Aamas).  All
 * the licences are in notices/THIRD-PARTY.md.
 *
 * Defined once, in tgb_shared.c, outside the per-instance partial link, so
 * the second Game Boy of a link session does not carry a copy.  .rodata
 * only; nothing is allocated.  622 bytes in all.
 */
#ifndef TGB_CGB_DMG_PAL_H
#define TGB_CGB_DMG_PAL_H

/* Manual (button) palettes, in the order the GB palette row offers them. */
enum {
  TGB_CGBPAL_UP = 0, TGB_CGBPAL_UP_A, TGB_CGBPAL_UP_B,
  TGB_CGBPAL_LEFT, TGB_CGBPAL_LEFT_A, TGB_CGBPAL_LEFT_B,
  TGB_CGBPAL_DOWN, TGB_CGBPAL_DOWN_A, TGB_CGBPAL_DOWN_B,
  TGB_CGBPAL_RIGHT, TGB_CGBPAL_RIGHT_A, TGB_CGBPAL_RIGHT_B,
  TGB_CGBPAL_MANUAL_COUNT
};
#define TGB_CGBPAL_COMBOS 51

/* hdr: the cartridge header from 0x134 (title) to 0x14B (old licensee) at
 * least, i.e. 0x18 bytes or more. */
unsigned tgbshared_cgbpal_title_sum(const unsigned char *hdr);
#define TGB_SGBPAL_COUNT 32
/* Super Game Boy system palette `n` (0 = 1-A, 1 = 1-B ... 31 = 4-H) as four
 * BGR555 colours, lightest first.  Out-of-range gives 1-A. */
void tgbshared_sgbpal_colors(unsigned n, unsigned short out[4]);
/* The combination the GBC boot ROM picks with no button held (0..50). */
unsigned tgbshared_cgbpal_auto(const unsigned char *hdr);
/* The combination of manual palette `n` (TGB_CGBPAL_*). */
unsigned tgbshared_cgbpal_manual(unsigned n);
/* Combination `combo` as twelve BGR555 colours: BG[4], OBJ0[4], OBJ1[4]
 * (tgb_dmg_palette's layout).  Out-of-range combos give combination 0. */
void tgbshared_cgbpal_colors(unsigned combo, unsigned short out[12]);

#ifdef TGB_SHARED_TABLES
/* The 30 four-colour palettes, BGR555, as one array: a few combinations
 * start a palette one colour early (offsets not a multiple of 4 below). */
static const unsigned short cgbpal_colors[30 * 4] = {
  0x7FFF, 0x32BF, 0x00D0, 0x0000,  /*  0 */
  0x639F, 0x4279, 0x15B0, 0x04CB,  /*  1 */
  0x7FFF, 0x6E31, 0x454A, 0x0000,  /*  2 */
  0x7FFF, 0x1BEF, 0x0200, 0x0000,  /*  3 */
  0x7FFF, 0x421F, 0x1CF2, 0x0000,  /*  4 */
  0x7FFF, 0x5294, 0x294A, 0x0000,  /*  5 */
  0x7FFF, 0x03FF, 0x012F, 0x0000,  /*  6 */
  0x7FFF, 0x03EF, 0x01D6, 0x0000,  /*  7 */
  0x7FFF, 0x42B5, 0x3DC8, 0x0000,  /*  8 */
  0x7E74, 0x03FF, 0x0180, 0x0000,  /*  9 */
  0x67FF, 0x77AC, 0x1A13, 0x2D6B,  /* 10 */
  0x7ED6, 0x4BFF, 0x2175, 0x0000,  /* 11 */
  0x53FF, 0x4A5F, 0x7E52, 0x0000,  /* 12 */
  0x4FFF, 0x7ED2, 0x3A4C, 0x1CE0,  /* 13 */
  0x03ED, 0x7FFF, 0x255F, 0x0000,  /* 14 */
  0x036A, 0x021F, 0x03FF, 0x7FFF,  /* 15 */
  0x7FFF, 0x01DF, 0x0112, 0x0000,  /* 16 */
  0x231F, 0x035F, 0x00F2, 0x0009,  /* 17 */
  0x7FFF, 0x03EA, 0x011F, 0x0000,  /* 18 */
  0x299F, 0x001A, 0x000C, 0x0000,  /* 19 */
  0x7FFF, 0x027F, 0x001F, 0x0000,  /* 20 */
  0x7FFF, 0x03E0, 0x0206, 0x0120,  /* 21 */
  0x7FFF, 0x7EEB, 0x001F, 0x7C00,  /* 22 */
  0x7FFF, 0x3FFF, 0x7E00, 0x001F,  /* 23 */
  0x7FFF, 0x03FF, 0x001F, 0x0000,  /* 24 */
  0x03FF, 0x001F, 0x000C, 0x0000,  /* 25 */
  0x7FFF, 0x033F, 0x0193, 0x0000,  /* 26 */
  0x0000, 0x4200, 0x037F, 0x7FFF,  /* 27 */
  0x7FFF, 0x7E8C, 0x7C00, 0x0000,  /* 28 */
  0x7FFF, 0x1BEF, 0x6180, 0x0000,  /* 29 */
};

/* Combinations: first colour of OBJ0, OBJ1 and BG in cgbpal_colors. */
#define P(o0, o1, bg) { (o0) * 4, (o1) * 4, (bg) * 4 }
#define RAW(o0, o1, bg) { (o0), (o1), (bg) }
static const unsigned char cgbpal_combos[TGB_CGBPAL_COMBOS][3] = {
  P( 4,  4, 29), P(18, 18, 18), P(20, 20, 20), P(24, 24, 24),  /*  0- 3 */
  P( 9,  9,  9), P( 0,  0,  0), P(27, 27, 27), P( 5,  5,  5),  /*  4- 7 */
  P(12, 12, 12), P(26, 26, 26), P(16,  8,  8), P( 4, 28, 28),  /*  8-11 */
  P( 4,  2,  2), P( 3,  4,  4), P( 4, 29, 29), P(28,  4, 28),  /* 12-15 */
  P( 2, 17,  2), P(16, 16,  8), P( 4,  4,  7), P( 4,  4, 18),  /* 16-19 */
  P( 4,  4, 20), P(19, 19,  9),                                /* 20-21 */
  RAW(4 * 4 - 1, 4 * 4 - 1, 11 * 4),                           /* 22    */
  P(17, 17,  2), P( 4,  4,  2), P( 4,  4,  3), P(28, 28,  0),  /* 23-26 */
  P( 3,  3,  0), P( 0,  0,  1), P(18, 22, 18), P(20, 22, 20),  /* 27-30 */
  P(24, 22, 24), P(16, 22,  8), P(17,  4, 13),                 /* 31-33 */
  RAW(28 * 4 - 1, 0 * 4, 14 * 4),                              /* 34    */
  RAW(28 * 4 - 1, 4 * 4, 15 * 4),                              /* 35    */
  RAW(19 * 4, 23 * 4 - 1, 9 * 4),                              /* 36    */
  P(16, 28, 10), P( 4, 23, 28), P(17, 22,  2), P( 4,  0,  2),  /* 37-40 */
  P( 4, 28,  3), P(28,  3,  0), P( 3, 28,  4), P(21, 28,  4),  /* 41-44 */
  P( 3, 28,  0), P(25,  3, 28), P( 0, 28,  8), P( 4,  3, 28),  /* 45-48 */
  P(28,  3,  6), P( 4, 28, 29),                                /* 49-50 */
};
#undef P
#undef RAW

/* Title sums.  The first CGBPAL_UNIQUE are matched by the sum alone; the
 * rest also need the title's 4th letter to be cgbpal_letter[i - UNIQUE]. */
#define CGBPAL_UNIQUE 65
#define CGBPAL_SUMS 94
static const unsigned char cgbpal_sums[CGBPAL_SUMS] = {
  0x00, 0x88, 0x16, 0x36, 0xD1, 0xDB, 0xF2, 0x3C, 0x8C, 0x92, 0x3D, 0x5C,
  0x58, 0xC9, 0x3E, 0x70, 0x1D, 0x59, 0x69, 0x19, 0x35, 0xA8, 0x14, 0xAA,
  0x75, 0x95, 0x99, 0x34, 0x6F, 0x15, 0xFF, 0x97, 0x4B, 0x90, 0x17, 0x10,
  0x39, 0xF7, 0xF6, 0xA2, 0x49, 0x4E, 0x43, 0x68, 0xE0, 0x8B, 0xF0, 0xCE,
  0x0C, 0x29, 0xE8, 0xB7, 0x86, 0x9A, 0x52, 0x01, 0x9D, 0x71, 0x9C, 0xBD,
  0x5D, 0x6D, 0x67, 0x3F, 0x6B,
  /* 4th letter decides */
  0xB3, 0x46, 0x28, 0xA5, 0xC6, 0xD3, 0x27, 0x61, 0x18, 0x66, 0x6A, 0xBF,
  0x0D, 0xF4, 0xB3, 0x46, 0x28, 0xA5, 0xC6, 0xD3, 0x27, 0x61, 0x18, 0x66,
  0x6A, 0xBF, 0x0D, 0xF4, 0xB3,
};
static const char cgbpal_letter[CGBPAL_SUMS - CGBPAL_UNIQUE] = {
  'B','E','F','A','A','R','B','E','K','E','K',' ','R','-','U','R','A','R',
  ' ','I','N','A','I','L','I','C','E',' ','R'
};
/* The combination for each sum. */
static const unsigned char cgbpal_pick[CGBPAL_SUMS] = {
   0,  4,  5, 35, 34,  3, 31, 15, 10,  5, 19, 36,  7, 37, 30, 44, 21, 32,
  31, 20,  5, 33, 13, 14,  5, 29,  5, 18,  9,  3,  2, 26, 25, 25, 41, 42,
  26, 45, 42, 45, 36, 38, 26, 42, 30, 41, 34, 34,  5, 42,  6,  5, 33, 25,
  42, 42, 40,  2, 16, 25, 42, 42,  5,  0, 39,
  /* 4th letter decides */
  36, 22, 25,  6, 32, 12, 36, 11, 39, 18, 39, 24, 31, 50, 17, 46,  6, 27,
   0, 47, 41, 41,  0,  0, 19, 34, 23, 18, 29,
};

static const unsigned char cgbpal_manual[TGB_CGBPAL_MANUAL_COUNT] = {
  /* Up, Up+A, Up+B, Left, Left+A, Left+B */
   5, 43, 28, 48, 40,  7,
  /* Down, Down+A, Down+B, Right, Right+A, Right+B */
   8,  3, 49,  1,  0,  6,
};

/* The 32 Super Game Boy palettes, RGB888 (shown as BGR555 by tgb_shared.c). */
static const unsigned long cgbpal_sgb[TGB_SGBPAL_COUNT][4] = {
  { 0xF8E8C8, 0xD89048, 0xA82820, 0x301850 },  /* 1-A */
  { 0xD8D8C0, 0xC8B070, 0xB05010, 0x000000 },  /* 1-B */
  { 0xF8C0F8, 0xE89850, 0x983860, 0x383898 },  /* 1-C */
  { 0xF8F8A8, 0xC08048, 0xF80000, 0x501800 },  /* 1-D */
  { 0xF8D8B0, 0x78C078, 0x688840, 0x583820 },  /* 1-E */
  { 0xD8E8F8, 0xE08850, 0xA80000, 0x004010 },  /* 1-F */
  { 0x000050, 0x00A0E8, 0x787800, 0xF8F858 },  /* 1-G */
  { 0xF8E8E0, 0xF8B888, 0x804000, 0x301800 },  /* 1-H */
  { 0xF0C8A0, 0xC08848, 0x287800, 0x000000 },  /* 2-A */
  { 0xF8F8F8, 0xF8E850, 0xF83000, 0x500058 },  /* 2-B */
  { 0xF8C0F8, 0xE88888, 0x7830E8, 0x282898 },  /* 2-C */
  { 0xF8F8A0, 0x00F800, 0xF83000, 0x000050 },  /* 2-D */
  { 0xF8C880, 0x90B0E0, 0x281060, 0x100810 },  /* 2-E */
  { 0xD0F8F8, 0xF89050, 0xA00000, 0x180000 },  /* 2-F */
  { 0x68B838, 0xE05040, 0xE0B880, 0x001800 },  /* 2-G */
  { 0xF8F8F8, 0xB8B8B8, 0x707070, 0x000000 },  /* 2-H */
  { 0xF8D098, 0x70C0C0, 0xF86028, 0x304860 },  /* 3-A */
  { 0xD8D8C0, 0xE08020, 0x005000, 0x001010 },  /* 3-B */
  { 0xE0A8C8, 0xF8F878, 0x00B8F8, 0x202058 },  /* 3-C */
  { 0xF0F8B8, 0xE0A878, 0x08C800, 0x000000 },  /* 3-D */
  { 0xF8F8C0, 0xE0B068, 0xB07820, 0x504870 },  /* 3-E */
  { 0x7878C8, 0xF868F8, 0xF8D000, 0x404040 },  /* 3-F */
  { 0x60D850, 0xF8F8F8, 0xC83038, 0x380000 },  /* 3-G */
  { 0xE0F8A0, 0x78C838, 0x488818, 0x081800 },  /* 3-H */
  { 0xF0A868, 0x78A8F8, 0xD000D0, 0x000078 },  /* 4-A */
  { 0xF0E8F0, 0xE8A060, 0x407838, 0x180808 },  /* 4-B */
  { 0xF8E0E0, 0xD8A0D0, 0x98A0E0, 0x080000 },  /* 4-C */
  { 0xF8F8B8, 0x90C8C8, 0x486878, 0x082048 },  /* 4-D */
  { 0xF8D8A8, 0xE0A878, 0x785888, 0x002030 },  /* 4-E */
  { 0xB8D0D0, 0xD880D8, 0x8000A0, 0x380000 },  /* 4-F */
  { 0xB0E018, 0xB82058, 0x281000, 0x008060 },  /* 4-G */
  { 0xF8F8C8, 0xB8C058, 0x808840, 0x405028 },  /* 4-H */
};
#endif /* TGB_SHARED_TABLES */

#endif
