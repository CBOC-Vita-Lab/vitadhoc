/* gamedb.h -- what the game list knows about a cartridge it has never seen:
 * its No-Intro name (to ask for its box art) and two or three lines about
 * the game (the home screen shows them under its name).
 *
 * THE FILE.  assets/gamedb.txt ("app0:gamedb.txt" on the Vita), written by
 * tools/make_gamedb.py from the No-Intro catalogues (libretro-database),
 * Wikidata and English Wikipedia.  Text, sorted, one record a line, TAB
 * between key and value:
 *
 *    C<crc32, 8 hex digits>   No-Intro name of the cartridge with that CRC
 *    S<GBA game code>         No-Intro name of a GBA cartridge by header
 *    Z<GBA game code>         ...and that cartridge's size in bytes
 *    N<sys><loose title>      No-Intro name by a loose title (gamedb_name_loose)
 *
 * System letters (<sys>), the same in make_gamedb.py SYSTEMS, artfetch.c and
 * the cheat database's name keys (make_cheatdb.py):
 *    A GBA  B GB  C GBC  D SNES  E NES  F Famicom Disk System
 *    G WonderSwan  H WonderSwan Color
 *    I Sega Genesis / Mega Drive  J Sega Master System (and Mark III)
 *    K Sega Game Gear  L Sega SG-1000                  (2.0, md.bin)
 * gamedb_sega.h has each Sega console's extensions, thumbnail folder and the
 * folder names to scan.
 *    T<title key>             the Wikipedia article about that title
 *    W<article>               its first sentences (plain ASCII)
 *
 * It is read whole into memory (about 2.6 MiB in 2.0) and searched in place; after
 * gamedb_open nothing writes it, so any thread may look things up.
 * Wikipedia's text is CC BY-SA 4.0: the home screen and the README say
 * where it comes from.
 */
#ifndef GBADHOC_GAMEDB_H
#define GBADHOC_GAMEDB_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Read the file.  Returns the number of records (0: none there). */
int  gamedb_open(const char *path);
/* Remember the file; the first lookup (on whichever thread) reads it, and
 * lookups made meanwhile wait for that read.  0 = remembered. */
int  gamedb_open_later(const char *path);
/* Records read, or -1 while the file has not been read yet. */
int  gamedb_records(void);
void gamedb_close(void);

/* The value of `key`: 1 and a copy in out (cut to cap), or 0. */
int  gamedb_get(const char *key, char *out, size_t cap);

/* 1 when `name` is a No-Intro name the file knows (a cartridge's full
 * name, regions and all: "Pokemon - Emerald Version (USA, Europe)"). */
int  gamedb_known_name(const char *name);

/* The title a No-Intro name or file name is about: what comes before its
 * first " (", with a trailing ", The" / ", A" / ", An" put back in front
 * ("Legend of Zelda, The - The Minish Cap (USA)" -> "The Legend of Zelda -
 * The Minish Cap"). */
void gamedb_base_title(const char *name, char *out, size_t cap);

/* The key a title is filed under (make_gamedb.py title_key, the same rules):
 * accents off, lower case, '&' as "and", every run of anything that is not
 * a letter or a digit one space, a "the" at either end left off. */
void gamedb_title_key(const char *title, char *out, size_t cap);

/* The lines about the game called `name` (a No-Intro name or a file name
 * without its extension) on any console (the first that has one): 1 and the
 * text in out, or 0. */
int  gamedb_blurb(const char *name, char *out, size_t cap);
/* The description of a game of console `sys` (the letters of gamedb_name_loose;
 * M = Sega CD): only an article about that game on that console (or on the one
 * it plays as well: B/C, G/H, F->E, J->K), else 0 and nothing is shown.  Two
 * consoles' games of one title ("Strider") each get their own article. */
int  gamedb_blurb_sys(char sys, const char *name, char *out, size_t cap);

/* A GBA cartridge's header code (four characters at 0xAC) and a whole
 * cartridge's CRC32 -> its No-Intro name.  1 = found. */
int  gamedb_name_by_code(const char code[4], char *out, size_t cap);
int  gamedb_name_by_crc(uint32_t crc, char *out, size_t cap);
/* A No-Intro name for a file named loosely ("Act Raiser", "7th Saga",
 * "Mega Man X 2", "Pocket Dogs (U)"): its title key without spaces, looked
 * up among console `sys`'s games ('A' GBA, 'B' GB, 'C' GBC, 'D' SNES, 'E' NES and Famicom Disk
 * System, 'G' WonderSwan, 'H' WonderSwan Color, 'I' Genesis, 'J' Master System,
 * 'K' Game Gear, 'L' SG-1000; N records).  1 when found. */
int  gamedb_name_loose(char sys, const char *name, char *out, size_t cap);
/* The CRC32 of a SNES file's ROM: a 512-byte copier header (a file 512
 * bytes over a multiple of 1 KiB) is left out, as the No-Intro CRCs are of
 * the cartridge alone. */
int  gamedb_snes_crc_cancel(const char *path, uint32_t *crc, int (*abort)(void));
/* ...and of a NES file's ROM: the 16-byte iNES header ("NES\x1a") is left
 * out, and with flag 6 bit 2 set the 512-byte trainer after it; a Famicom
 * Disk System file loses its 16-byte "FDS\x1a" header.  A UNIF file, or a
 * file with no header, is taken whole.  (The No-Intro CRCs are of the
 * cartridge without them.) */
int  gamedb_nes_crc_cancel(const char *path, uint32_t *crc, int (*abort)(void));

/* Sega.  The Genesis / Mega Drive catalogue's CRCs are of the plain ROM
 * (big-endian, no copier header).  A file in the SMD format (Super Magic
 * Drive: a 512-byte header, then 16 KiB blocks whose odd bytes come first and
 * even bytes second) is hashed as the plain ROM: header dropped, each block
 * de-interleaved (Genesis Plus GX loadrom.c, deinterleave_block).  An SMD file
 * is: size = 512 + n x 16384 (n >= 1), header bytes 8 and 9 = AA BB, and no
 * "SEGA" at 0x100 (a ".smd" file needs no AA BB).  Everything else (.md, .gen,
 * .bin) is hashed whole. */
int gamedb_md_crc_cancel(const char *path, uint32_t *crc, int (*abort)(void));
/* The Master System, Game Gear and SG-1000 catalogues' CRCs are of the whole
 * cartridge image: a file of 512 bytes over a multiple of 16 KiB has a copier
 * header, which is left out (the core drops one from any file of an odd
 * number of 512-byte blocks that has no "SEGA" at 0x100; for real images the
 * two rules agree).  Anything else is hashed whole. */
int gamedb_sms_crc_cancel(const char *path, uint32_t *crc, int (*abort)(void));
/* What a file is, read from its bytes (not its name): */
enum { GAMEDB_MD_NONE = 0,   /* not a Genesis ROM (no "SEGA" at 0x100, no SMD header) */
       GAMEDB_MD_PLAIN = 1,  /* "SEGA" at 0x100 */
       GAMEDB_MD_SMD = 2,    /* SMD, as above */
       GAMEDB_MD_32X = 3,    /* "SEGA 32X" at 0x100: a 32X ROM, which md.bin cannot run */
       GAMEDB_MD_CD = 4 };   /* "SEGADISCSYSTEM" at 0: the data track of a Sega CD disc (a cooked .bin or .iso): a Sega CD game, not a Genesis one */
int gamedb_md_rom_kind(const char *path);
/* 1 when the library should list the file as a Genesis game: .md, .gen and .smd
 * always; .bin only when gamedb_md_rom_kind says PLAIN or SMD (32X, Mega-CD,
 * Master System and Pico files use .bin too).  0 for any other extension. */
int gamedb_md_accept(const char *path);
/* The same test for a file that is not on the card (an entry of a .zip): its first bytes and its size.
 * 1 for a plain Genesis ROM ("SEGA" at 0x100, not 32X) or an SMD (AA BB, size 512 + n x 16384). */
int gamedb_md_head_ok(const unsigned char *head, unsigned n, unsigned long size);

/* The GBA game code of the cartridge file at `path` (bytes 0xAC-0xAF, four
 * capital letters or digits): 1 and code[0..3], or 0. */
int  gamedb_read_code(const char *path, char code[4]);

/* The No-Intro name of the GBA cartridge file at `path` by its header's
 * code -- when the file is at least half the size of that cartridge
 * (homebrew borrows codes: the port's own test cartridges say AGDE, which
 * is Lufia).  1 = found. */
int  gamedb_name_by_header(const char *path, char *out, size_t cap);

/* CRC32 (the zip one) of a whole file, in pieces; 0 = done.  `abort` (may
 * be NULL) is looked at between pieces: nonzero stops it (-1). */
int  gamedb_file_crc(const char *path, uint32_t *crc,
                     volatile const int *abort);
/* Worker cancellation predicate, tested before each bounded file chunk. */
int  gamedb_file_crc_cancel(const char *path, uint32_t *crc,
                            int (*abort)(void));

#ifdef __cplusplus
}
#endif

#endif /* GBADHOC_GAMEDB_H */
