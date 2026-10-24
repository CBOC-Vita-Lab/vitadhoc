/* ra_vita.h -- RetroAchievements: sign in, earn achievements as you play.
 *
 * The rules are the site's and its runtime's (third_party/rcheevos, the
 * library every RetroAchievements emulator uses); this file is the app's
 * side of it:
 *
 *   THE ACCOUNT.  Signed in once from Settings > RETROACHIEVEMENTS >
 *   Account: the name and the password through the system keyboard, sent
 *   once, never stored.  What is kept is the name and the server's token,
 *   in <data>/retroachievements.txt, and the next boot signs in with those.
 *
 *   A GAME.  When a game starts its file is hashed (MD5, the whole file --
 *   for a patched game the patched copy, which is a different game to the
 *   site) and its achievements are fetched.  Each emulated frame they are
 *   tested against the machine's memory (ra_frame); an unlock is a toast
 *   and is sent to the server, again later if the server could not be
 *   reached.
 *
 *   HARDCORE.  The setting is read when a game starts.  While it is in
 *   force for the running game (ra_hardcore_active) the app refuses what
 *   the site's hardcore rules forbid: loading a state (a slot, the shelf,
 *   Continue), rewinding, and cheats.  Fast forward stays.  When the game
 *   turns out to have no achievements, nothing is refused.
 *
 *   NOT IN A LINK.  Two machines that must stay identical are no place for
 *   a runtime that only one of them runs: during a link session (either
 *   kind) achievements are not processed, and their progress starts over
 *   when it ends, as after a loaded state.
 *
 * Everything here runs on the game thread.  The server's answers come back
 * through ra_pump(), which the game loop and the menus' refresh call. */
#ifndef RA_VITA_H
#define RA_VITA_H

#include <stddef.h>
#include <stdint.h>

#include "../frontend-common/fe_console.h"

/* At boot: `data` is the app's folder (the account file lives there);
 * `host` overrides the server ("http://127.0.0.1:8080" for the twin's
 * tests; NULL or "" = retroachievements.org); `online` says whether the
 * network may be used now (NULL = always). */
void ra_boot(const char *data, const char *host, const char *version,
             int (*online)(void));
/* At exit. */
void ra_shutdown(void);

/* Deliver what the server answered; run the runtime's idle work when no
 * frame was emulated since the last call.  Every refresh, game or menu. */
void ra_pump(void);

/* ---- the account ------------------------------------------------------- */
enum { RA_ACCT_OUT = 0, RA_ACCT_BUSY, RA_ACCT_IN };
/* RA_ACCT_*; `name` gets the display name (or the name being signed in). */
int  ra_account(char *name, size_t cap);
/* 0 = asked (the answer is a toast); -1 = refused (busy, empty). */
int  ra_login(const char *user, const char *password);
void ra_logout(void);
/* The Hardcore setting (for the next game started). */
int  ra_hardcore_pref(void);
void ra_hardcore_set(int on);

/* ---- a game ------------------------------------------------------------ */
/* `boot_path` is the file the core runs (a patched copy when patched). */
void ra_game_begin(const char *boot_path, fe_console_t console);
void ra_game_end(void);
/* After each emulated frame.  `linked`: a link session is up. */
void ra_frame(int linked);
/* A state was loaded, the game rewound or reset: progress starts over. */
void ra_state_loaded(void);
void ra_game_reset(void);
/* Hardcore is in force for the running game. */
int  ra_hardcore_active(void);
/* Hardcore is set, and whether the game is one RetroAchievements knows (and
 * so whether it applies) is not known yet: the file's hash or the server's
 * answer is still to come. */
int  ra_hardcore_pending(void);

/* ---- the list (Settings > Achievements) --------------------------------- */
typedef struct ra_ach
{
   char     title[64];
   char     desc[160];
   char     progress[24];      /* "12/50", or "" */
   unsigned points;
   int      unlocked;
} ra_ach;

/* 1 = a game with achievements is running: counts and points filled in. */
int  ra_summary(unsigned *unlocked, unsigned *total, unsigned *points_have,
                unsigned *points_total, int *hardcore);
/* The game's title on the site ("" when none). */
const char *ra_game_title(void);
/* Rebuild the list (opening the page); then ra_list_get(i) for each. */
int  ra_list_build(void);
const ra_ach *ra_list_get(int i);
void ra_list_free(void);

#endif
