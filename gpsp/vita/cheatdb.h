/* cheatdb.h -- the bundled cheat database (assets/cheats.db, "app0:cheats.db"
 * on the Vita), read in place: a 64-byte header, three sorted indexes that
 * are binary-searched ON DISK, and one block per game that is read whole
 * when its game boots.  Nothing else of the file is ever in memory.
 *
 * The format is described at the top of tools/make_cheatdb.py, which writes
 * it from the libretro database's .cht files and the No-Intro lists.
 * cheats_vita.c is the only user in the app; tests and tools use it too.
 */
#ifndef GBADHOC_CHEATDB_H
#define GBADHOC_CHEATDB_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Remember where the database is and check its header.  Returns the number
 * of games (0: no database, or not one). */
int  cheatdb_open(const char *path);
void cheatdb_close(void);
int  cheatdb_games(void);
int  cheatdb_cheats(void);              /* in all */

/* Lookups return a game index (>= 0) or -1. */
int  cheatdb_find_crc(uint32_t crc);
/* The GBA game code (ROM 0xAC, four characters).  *unique is set to 1 when
 * every release with this code has this list (no need to hash the ROM). */
int  cheatdb_find_serial(const char code[4], int *unique);
/* A ROM's file name (any directory and extension are ignored), system 'A'
 * (GBA) or 'G' (GB / GBC).  *exact: 1 = title, region and revision matched,
 * 0 = the title alone did. */
int  cheatdb_find_name(char system, const char *file_name, int *exact);
/* A cartridge header's title ("POKEMON EMER", "ZELDA"): the one game whose
 * title begins with it, when there is exactly one. */
int  cheatdb_find_title(char system, const char *header_title);

/* One game's block, malloc'ed: the game's name, NUL, then *cheats times
 * name NUL code NUL.  NULL when it cannot be read or fails its checksum. */
char *cheatdb_load(int game, unsigned *cheats, size_t *size);

/* The key strings the name index is built from (also used by the tests):
 * writes "<s>f<title>(<regions>)[r<rev>]" to full and "<s>b<title>" to
 * base, each of capacity cap. */
void cheatdb_name_keys(char system, const char *file_name, char *full,
                       char *base, size_t cap);

uint32_t cheatdb_crc32(uint32_t crc, const void *data, size_t size);

#ifdef __cplusplus
}
#endif
#endif
