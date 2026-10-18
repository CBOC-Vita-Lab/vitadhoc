/* fe_cheats.h -- the per-game cheat list: a cheat file's text, its entries,
 * which of them are switched on, and handing those to the core
 * (docs/CHEATS.md).
 *
 * Built only with CHEATS=1 (-DGPSP_CHEATS=1), the PSP default.
 *
 * Split like fe_favs: everything here is pure text and tables (host-tested,
 * tools/test_fe_cheats.c); reading and writing files is the platform's job
 * (psp/ui_psp.c uses sceIo, because after a ROM has loaded there is no heap
 * left for stdio -- heap-exhausted-by-design).  Storage is static for the
 * same reason: nothing in this module allocates.
 *
 * A cheat file is HOSTILE INPUT.  This module only ever produces
 *   - names (printable ASCII, NUL-terminated, inside its own buffer), and
 *   - code text, which the core's strict parser (cheats.c) or the Game Boy
 *     checker below must accept before anything runs.
 * Nothing in a file can switch a cheat on: a list always loads all-off, and
 * only the player's own choices (the .on file the menu writes) turn entries
 * on. */
#ifndef FE_CHEATS_H
#define FE_CHEATS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FE_CHEATS_MAX        256          /* entries listed from one file    */
#define FE_CHEATS_FILE_MAX   (32 * 1024)  /* bytes of a file that are read   */
#define FE_CHEATS_STATE_MAX  (12 * 1024)  /* the .on file, read or written   */
#define FE_CHEATS_GB_POKES   256          /* Game Boy writes active at once  */
#define FE_CHEATS_NAME_SHOWN 48           /* name bytes kept in the .on file */

enum { FE_CHEATS_SYS_GBA = 0, FE_CHEATS_SYS_GB = 1 };

/* An entry's `error`: 0, a core cheat_error (1..9, cheats.h), or one of
 * these.  fe_cheats_error_name() has the words for all of them. */
enum
{
   FE_CHEAT_OK = 0,
   FE_CHEAT_ERR_GB_GENIE = 20,   /* Game Genie: a ROM patch                */
   FE_CHEAT_ERR_GB_TYPE,         /* GameShark, but not a 01 (RAM) code     */
   FE_CHEAT_ERR_GB_REGION,       /* outside work RAM (ROM, save RAM, I/O)  */
   FE_CHEAT_ERR_GB_SYNTAX,       /* not a Game Boy code at all             */
   FE_CHEAT_ERR_GB_TOO_MANY      /* more writes on than FE_CHEATS_GB_POKES */
};

typedef struct
{
   const char *name;     /* never NULL */
   const char *code;     /* never NULL; the file's own text for this cheat */
   uint32_t hash;        /* identity: name + code, formatting-independent  */
   unsigned char enabled;
   unsigned char error;  /* why it cannot be switched on, or 0             */
} fe_cheat;

enum
{
   FE_CHEATS_NONE = 0,   /* no file for this game                          */
   FE_CHEATS_LOADED,     /* a file with at least one cheat                 */
   FE_CHEATS_EMPTY       /* a file, but nothing in it reads as a cheat     */
};

/* The text buffer a file is read into: FE_CHEATS_FILE_MAX + 1 bytes. */
char *fe_cheats_buffer(void);

/* Drop the list.  If it was attached, the core's cheats go with it. */
void fe_cheats_reset(void);

/* Parse the first `len` bytes of fe_cheats_buffer() IN PLACE as a cheat file
 * for `sys` (FE_CHEATS_SYS_*).  Either format, told apart by content:
 *   - the plain list: a name line, then its code lines (docs/CHEATS.md);
 *   - libretro .cht: cheatN_desc / cheatN_code keys.
 * `truncated`: the file was longer than the buffer (the last, cut line is
 * dropped).  Every entry is checked (error set) and left OFF.  Returns the
 * status (FE_CHEATS_*). */
int fe_cheats_parse(size_t len, int sys, int truncated);

/* Convenience for tests and stdio frontends: copy `text` in and parse it. */
int fe_cheats_load_text(const char *text, size_t len, int sys);

/* The player's choices.  `text` is a .on file: one "HHHHHHHH name" line per
 * cheat that is on (the hash decides; the name is for people).  Unknown
 * hashes are ignored -- the cheat left the file.  Entries with an error
 * stay off.  Then, if attached, the list is applied to the core. */
void fe_cheats_state_parse(const char *text, size_t len);
/* The .on text for the current list; returns its length (< cap), or cap if
 * it did not fit (nothing usable written). */
size_t fe_cheats_state_serialise(char *out, size_t cap);

/* 1: this list belongs to the RUNNING game, so every change is handed to
 * the core at once.  0 (the ROM browser): the list is only edited. */
void fe_cheats_attach(int on);

int fe_cheats_status(void);
int fe_cheats_system(void);
int fe_cheats_truncated(void);     /* file longer than FE_CHEATS_FILE_MAX */
unsigned fe_cheats_found(void);    /* cheats in the file, including the ones
                                    * past FE_CHEATS_MAX that are not listed */
unsigned fe_cheats_count(void);
const fe_cheat *fe_cheats_get(unsigned i);
unsigned fe_cheats_enabled_count(void);
unsigned fe_cheats_usable_count(void);   /* entries with no error */

/* Switch entry i on or off.  Returns 0 when it is now in the state asked
 * for, else the reason it is not (and it is off). */
int fe_cheats_set(unsigned i, int on);
void fe_cheats_all_off(void);

/* A link / wireless session is up: nothing is applied while locked, on
 * either system; the choices are kept and run again when it clears. */
void fe_cheats_lock(int on);
int fe_cheats_locked(void);
/* Cheats that are running right now (0 while locked or detached). */
unsigned fe_cheats_running(void);

/* Game Boy: once per emulated frame, before the core runs it.  Each active
 * GameShark write goes through `poke` (0 = accepted).  Does nothing unless
 * the attached list is a Game Boy one with entries on and no lock. */
void fe_cheats_gb_frame(int (*poke)(void *ctx, uint16_t addr, uint8_t value),
                        void *ctx);
unsigned fe_cheats_gb_writes(void);    /* pokes accepted so far */

/* Words for an entry's error, short enough for a list row. */
const char *fe_cheats_error_name(int err);

/* What the core reports (cheats.c), for logs. */
unsigned fe_cheats_ram_flushes(void);
unsigned fe_cheats_core_writes(void);

#ifdef __cplusplus
}
#endif

#endif
