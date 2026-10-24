/* cheats_vita.h -- cheat codes for the running game.
 *
 * THE CONTRACT between three files:
 *   vita/cheats_vita.c   the list, the .cht file, and pushing codes into the
 *                        cores (GBA: cheats.c, GB/GBC: gbcore/tgbdual/cheat.c)
 *   psp/ui_psp.c (VITA)  the in-game "Cheats" page
 *   vita/main_vita.c     load at boot, re-apply after a state load / reset,
 *                        suspend while a network session is up
 *
 * One file per game:  <root>/cheats/<rom file name without extension>.cht
 * in the libretro format, so files from RetroArch and from the libretro
 * database work as they are:
 *
 *     cheats = 2
 *     cheat0_desc = "Infinite money"
 *     cheat0_code = "82025838 E0FF+82025840 E0FF"
 *     cheat0_enable = false
 *     cheat1_desc = ...
 *
 * A code is one or more lines joined by '+' (libretro's separator); spaces
 * inside a line are kept as typed.
 *
 * THE BUNDLED DATABASE.  The app ships a cheat database (built from the
 * libretro database) so a game has its known cheats the first time it is
 * booted, with nothing to copy: cheats_load finds the game in it (by the
 * ROM's CRC-32, then its header code / title, then its file name) and the
 * list is  [the player's .cht entries] + [database entries not already in
 * it], everything from the database OFF until switched on.  Switching one
 * on (or adding / deleting anything) writes the player's .cht, which from
 * then on records what he enabled; database entries he never touched are
 * not copied into it.
 *
 * Kinds accepted (cheats_kind): see the enum.  Whether an encrypted kind is
 * really decoded is the engine's business; cheats_validate says what will
 * happen BEFORE the player saves a code.
 */
#ifndef GBADHOC_CHEATS_VITA_H
#define GBADHOC_CHEATS_VITA_H

#include "fe_console.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CHEATS_MAX        1024   /* entries per game (the bundled database
                                  * has games with several hundred)        */
#define CHEATS_NAME_CAP   64
#define CHEATS_CODE_CAP   4096   /* all lines of one entry, '+'-joined     */

/* The list's storage belongs to cheats_vita.c (allocated per game); the
 * pointers are valid until the next cheats_add/remove/load/unload. */
typedef struct cheat_entry
{
   const char *name;
   const char *code;
   int  enabled;
   int  kind;                    /* CHEAT_KIND_*, from cheats_validate     */
   int  usable;                  /* 0: the engine cannot run this code     */
   int  builtin;                 /* 1: from the bundled database, 0: the
                                  * player's own (typed, or in his .cht)   */
} cheat_entry;

enum
{
   CHEAT_KIND_UNKNOWN = 0,
   CHEAT_KIND_GBA_RAW,           /* AAAAAAAA VV / VVVV / VVVVVVVV          */
   CHEAT_KIND_GBA_CODEBREAKER,   /* AAAAAAAA VVVV                          */
   CHEAT_KIND_GBA_GAMESHARK,     /* GameShark / Action Replay v1/v2, 8+8   */
   CHEAT_KIND_GBA_AR3,           /* Pro Action Replay v3, 8+8              */
   CHEAT_KIND_GB_GAMESHARK,      /* TTVVAAAA                               */
   CHEAT_KIND_GB_GAMEGENIE       /* VVA-AAA or VVA-AAA-CCC                 */
};

enum
{
   CHEATS_OK = 0,
   CHEATS_E_FULL = -1,           /* CHEATS_MAX entries already             */
   CHEATS_E_FORMAT = -2,         /* not a code this console understands    */
   CHEATS_E_UNSUPPORTED = -3,    /* recognised, but the engine cannot run it*/
   CHEATS_E_TOO_LONG = -4,
   CHEATS_E_IO = -5,             /* the .cht could not be written          */
   CHEATS_E_RANGE = -6
};

/* Once at boot: where the bundled database is ("app0:cheats.db" on the
 * Vita).  A missing database is not an error: games then have only the
 * player's own files.  Returns the number of games in it, or 0. */
int  cheats_db_open(const char *path);
/* The database's name for the running game ("Pokemon - Emerald Version
 * (USA, Europe)"), or "" when it is not in the database. */
const char *cheats_db_game(void);

/* A game booted: forget the previous list, read this game's .cht (a missing
 * file is an empty list), merge the bundled database's entries for it,
 * validate every entry, push the enabled ones into the core.  `root` is the data root ("ux0:data/GBAdhoc"), `rom_path` the
 * ROM's full path.  Returns the number of entries. */
int  cheats_load(const char *root, const char *rom_path, fe_console_t console);
/* The game is going away. */
void cheats_unload(void);

int                cheats_count(void);
const cheat_entry *cheats_get(int i);          /* NULL out of range */
int                cheats_enabled_count(void);

/* Switch one entry; takes effect in the core at once and is written to the
 * .cht (the write may be deferred to cheats_flush).  CHEATS_OK or an error. */
int  cheats_set_enabled(int i, int on);
/* Switch everything off (the list keeps its entries). */
void cheats_disable_all(void);

/* What would this text be on this console?  Returns CHEAT_KIND_* (> 0) when
 * it can be added and run, CHEATS_E_FORMAT or CHEATS_E_UNSUPPORTED when not.
 * Accepts lower case, any spacing, '+' or newline between lines. */
int  cheats_validate(const char *code, fe_console_t console);
/* Add an entry (enabled).  `name` may be NULL/"" -> "Cheat N".  Returns the
 * new index (>= 0) or an error. */
int  cheats_add(const char *name, const char *code);
int  cheats_remove(int i);
int  cheats_rename(int i, const char *name);

/* Push the enabled entries into the core again: after a state load, a
 * reset, or anything else that rebuilt the core's cheat table. */
void cheats_apply(void);
/* A network link session is up: every code leaves the core (the two
 * consoles must run identical machines) and nothing can be switched on
 * until cheats_suspend(0).  The list and the file are untouched.  The
 * LOCAL link (both machines on this console) does not suspend: codes apply
 * to the player's own machine only. */
void cheats_suspend(int on);
/* 0 = codes apply; 1 = suspended by a link session; 2 = by Hardcore. */
int  cheats_suspended(void);
/* RetroAchievements' Hardcore mode is on for the running game: no code
 * reaches the core and none can be switched on, until cheats_hardcore(0)
 * or the game's end.  Independent of cheats_suspend (a link that ends does
 * not bring the codes back). */
void cheats_hardcore(int on);

/* Write the .cht now if it changed (the main loop calls this when the menu
 * closes and when the game ends).  CHEATS_OK or CHEATS_E_IO. */
int  cheats_flush(void);

/* A sentence for a toast or the page's footer. */
const char *cheats_error_text(int rc);
const char *cheats_kind_name(int kind);
/* How a code of this console is typed, for the entry page's hint line:
 * "8 digits, space, 8 digits" ... */
const char *cheats_format_hint(fe_console_t console);

/* ---- additions by the cheats workstream (nothing above is changed) -------- */

/* More reasons, from cheats_validate / cheats_add / cheats_set_enabled.
 * cheats_error_text has a sentence for each. */
enum
{
   CHEATS_E_NEEDS_VALUE = -7,    /* "01??C5D0": a database code with a hole
                                  * where the player chooses the value     */
   CHEATS_E_ENCRYPTED = -8,      /* encrypted CodeBreaker lines and no
                                  * master (seed) code for them in the list */
   CHEATS_E_SUSPENDED = -9,      /* nothing is switched on during a network
                                  * session (cheats_suspend)                */
   CHEATS_E_BUILTIN = -10        /* cheats_remove of a database entry: it
                                  * would be back at the next boot          */
};

/* How the running game was found in the database (cheats_db_game). */
enum
{
   CHEATS_MATCH_NONE = 0,
   CHEATS_MATCH_CRC,             /* this very ROM                            */
   CHEATS_MATCH_CODE,            /* the GBA game code in its header          */
   CHEATS_MATCH_NAME,            /* title, region and revision of the file's
                                  * name                                     */
   CHEATS_MATCH_TITLE            /* the title alone (header or file name):
                                  * the codes may be another region's        */
};
int  cheats_db_match(void);

/* A line about entry i for the page's footer: why it cannot be switched on
 * (cheats_error_text of its reason), or what is special about it ("An
 * enable code...", "Acts while the cheat button is held."), or "". */
const char *cheats_note(int i);

/* The button of a GameShark / Action Replay cartridge, for the few codes
 * that act only while it is held (cheats_note says which).  The main loop
 * may tie it to any control the Game Boy Advance does not have. */
void cheats_button(int down);

#ifdef __cplusplus
}
#endif

#endif /* GBADHOC_CHEATS_VITA_H */
