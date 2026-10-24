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
 *    T<title key>             the Wikipedia article about that title
 *    W<article>               its first sentences (plain ASCII)
 *
 * It is read whole into memory (under 1 MiB) and searched in place; after
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
 * without its extension): 1 and the text in out, or 0. */
int  gamedb_blurb(const char *name, char *out, size_t cap);

/* A GBA cartridge's header code (four characters at 0xAC) and a whole
 * cartridge's CRC32 -> its No-Intro name.  1 = found. */
int  gamedb_name_by_code(const char code[4], char *out, size_t cap);
int  gamedb_name_by_crc(uint32_t crc, char *out, size_t cap);

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
