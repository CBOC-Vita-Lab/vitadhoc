/* fe_collect.h -- the player's own collections, and RetroArch playlists
 * shown as read-only ones.
 *
 * COLLECTIONS.  Named lists of games, the same idea as the Starred list
 * (fe_favs.h) with a name on each: ONE file for every console, a game is
 * its path relative to roms/ (a game from an extra folder: its whole path),
 * the key favourites.txt uses.  The browser lists only the games of the
 * console it is on, so a collection holds games of every console and each
 * console's view shows its own.  The file, roms/collections.txt, is text:
 *
 *    #collection Road trip
 *    Pokemon - Emerald Version.gba
 *    sub/Shantae.gbc
 *    #collection Two at once
 *    ...
 *
 * A name line starts with "#collection " and every other non-blank line is
 * a path.  Saved the way favourites.txt is: a file of its own first, the
 * old one aside, the new one in, the old one gone, so a cut anywhere leaves
 * a whole file; a list that was not read whole (a read error, or more than
 * the tables hold) is never written back over the file.
 *
 * PLAYLISTS.  RetroArch's .lpl files (JSON: "items" with "path", "label",
 * "crc32", ...) are read once at start-up, never written, and kept as
 * hashes only (12 bytes an item), then matched against the console's scan:
 * by path, else by file name, else by checksum (the item's CRC32 asked of
 * the bundled No-Intro table, compared with the game's file name).
 *
 * STATIC, NOT HEAP, like fe_favs: the heap is gone once a game has loaded.
 * Pure C (stdio for the file), no platform calls: the host tests run it
 * as it is. */
#ifndef FE_COLLECT_H
#define FE_COLLECT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FE_COLL_MAX       24            /* collections                      */
#define FE_COLL_NAME_MAX  40            /* bytes with the NUL               */
#define FE_COLL_ENTRIES   4096          /* games over all collections       */
#define FE_COLL_POOL      (192 * 1024)  /* bytes of NUL-separated paths     */
#define FE_COLL_PATH_MAX  512           /* a longer path line is dropped    */

typedef struct {
   unsigned ncoll;
   unsigned n;                          /* entries over all collections     */
   unsigned used;                       /* bytes of pool in use             */
   int      lossy;                       /* something did not fit: no save   */
   int      unread;                      /* a read error: no save            */
   char     name[FE_COLL_MAX][FE_COLL_NAME_MAX];
   unsigned short count[FE_COLL_MAX];   /* entries of each                  */
   unsigned hash[FE_COLL_ENTRIES];      /* FNV-1a of the path, as fe_favs does       */
   unsigned off[FE_COLL_ENTRIES];       /* path = pool + off[i]             */
   unsigned char owner[FE_COLL_ENTRIES];/* the collection it belongs to     */
   char     pool[FE_COLL_POOL];
   /* the text being read: a line at a time */
   char     line[FE_COLL_PATH_MAX + 16];
   unsigned line_n;
   int      line_over;
   int      cur;                        /* the collection being read, -1    */
} fe_coll;

void fe_coll_clear(fe_coll *c);

/* A name made fit for the list: control characters become blanks, the ends
 * are trimmed, it is cut to FE_COLL_NAME_MAX - 1 bytes at a character
 * boundary.  Returns the length (0 = nothing left: not a name). */
size_t fe_coll_clean_name(const char *in, char *out, size_t cap);

/* The file's text, in pieces of any size (a path may span two). */
void fe_coll_feed(fe_coll *c, const char *text, size_t len);
void fe_coll_feed_end(fe_coll *c);

/* Index of the collection called `name` (case does not matter), or -1. */
int fe_coll_find(const fe_coll *c, const char *name);
/* The new collection's index; -1 full, -2 not a name, -3 the name is taken
 * (the old one's index is then fe_coll_find). */
int fe_coll_create(fe_coll *c, const char *name);
/* 0; -2 not a name, -3 another collection has it. */
int fe_coll_rename(fe_coll *c, int idx, const char *name);
/* 0, or -1 when idx is not a collection.  The ones after it move up. */
int fe_coll_delete(fe_coll *c, int idx);

int fe_coll_has(const fe_coll *c, int idx, const char *rel);
int fe_coll_has_h(const fe_coll *c, int idx, const char *rel, unsigned h);
/* 0 on success (adding a member again changes nothing); -1 when a table is
 * full or the path is empty or too long. */
int fe_coll_add(fe_coll *c, int idx, const char *rel);
/* 0, or -1 when it was not a member. */
int fe_coll_remove(fe_coll *c, int idx, const char *rel);

/* The whole file's text through `w`, which gets pieces and returns 0 for
 * success (any other value stops the write and is returned). */
int fe_coll_write(const fe_coll *c, int (*w)(void *ctx, const char *s, size_t n),
                  void *ctx);

/* The file.  Load: a missing file is an empty list (0); a cut save leaves
 * path.old and no path, and that is the list; a read error (-1) leaves what
 * was read and forbids saving.  Save: -2 when the list may not be written
 * (lossy or unread), -1 on any failure with the file as it was. */
int fe_coll_load(fe_coll *c, const char *path);
int fe_coll_save(const fe_coll *c, const char *path);

/* ---- RetroArch playlists ---------------------------------------------- */

#define FE_LPL_MAX        16            /* playlists                        */
#define FE_LPL_ITEMS      8192          /* items over all of them           */
#define FE_LPL_PER_LIST   4096          /* items kept of one                */
#define FE_LPL_BYTES_MAX  (8u << 20)    /* read at most this much of a file */
#define FE_LPL_ROMS_MAX   2048          /* games matched at once            */

typedef struct {
   unsigned hp;        /* hash of the normalised path                       */
   unsigned hb;        /* ...of its file name (after "/" or "#", lower case)*/
   unsigned crc;       /* the item's CRC32, 0 = none                        */
} fe_lpl_item;

typedef struct {
   char     name[FE_COLL_NAME_MAX];     /* the file's name without ".lpl"   */
   unsigned first, n;                   /* its items                        */
   unsigned dropped;                    /* items that did not fit           */
   int      cut;                        /* read only up to FE_LPL_BYTES_MAX */
} fe_lpl_list;

typedef struct {
   unsigned    nlist, nitem;
   fe_lpl_list list[FE_LPL_MAX];
   fe_lpl_item item[FE_LPL_ITEMS];
   /* the parser's state while one is read */
   int      open, depth, in_items, in_str, esc, uni_n, have_str, key, saw_items;
   int      bad;
   unsigned uni, bytes;
   char     str[1024];
   unsigned str_n;
   char     path[1024], crc[40];
   unsigned path_n;
   int      want;       /* which item field the open string is the value of  */
} fe_lpl_set;

void fe_lpl_clear(fe_lpl_set *s);
/* Starts a playlist called `name` (the file name; ".lpl" is cut).  -1 when
 * the table is full. */
int fe_lpl_begin(fe_lpl_set *s, const char *name);
/* The file's bytes, in pieces of any size. */
void fe_lpl_feed(fe_lpl_set *s, const char *buf, size_t len);
/* The playlist is over.  The items it has (>= 0), or -1 when it was not a
 * RetroArch playlist (not JSON, cut short, no "items"): it is then dropped
 * whole, as if it had not been there. */
int fe_lpl_end(fe_lpl_set *s);
/* The file at `path` as playlist `name`: begin, feed, end.  Returns what
 * fe_lpl_end returns, or -1 for a file that cannot be read or is a
 * compressed (rzip) playlist, -2 for a table that is full. */
int fe_lpl_load(fe_lpl_set *s, const char *path, const char *name);

/* A path in the one spelling the hashes use: lower case, "/" only, runs of
 * "/" as one, "ux0:/x" as "ux0:x", no leading "./". */
void fe_lpl_norm(const char *in, char *out, size_t cap);

/* What the matcher needs of a game: hashes of its full path, of its file
 * name, and of its file name without the extension. */
typedef struct { unsigned hp, hb, hs; } fe_lpl_rom;
void fe_lpl_rom_key(const char *full_path, fe_lpl_rom *out);

/* The games of roms[0..n) that playlist `list` names, as ascending indices
 * into roms, into out (cap entries); returns how many, whatever cap (out may
 * be NULL to count).  Each item is looked for by full path; failing that by
 * file name; failing that, if it has a CRC32 and crc_name is given, by the
 * hash crc_name returns for it (the hash of the lower-case name, without
 * the extension, the bundled table gives that checksum; 0 = unknown).  An
 * item that matched by an earlier step is not looked for by a later one. */
unsigned fe_lpl_match(const fe_lpl_set *s, int list, const fe_lpl_rom *roms,
                      int n, unsigned short *out, unsigned cap,
                      unsigned (*crc_name)(unsigned crc, void *ctx),
                      void *ctx);

/* FNV-1a of the lower-cased bytes: the hash of a name for crc_name. */
unsigned fe_lpl_hash_name(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* FE_COLLECT_H */
