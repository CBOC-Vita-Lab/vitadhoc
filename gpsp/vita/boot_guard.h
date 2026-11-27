/* boot_guard.h -- the crash-loop guard.
 *
 * A game that dies the moment it starts, again and again, with a feature
 * that is new or heavy on (Smooth hand-over, the fourth core, Wide view, an
 * Image quality above Original, Steady pacing), leaves the player with an
 * app that closes on every try and no way to reach the Settings row that
 * would fix it.  This module notices and puts that game on safe picture
 * settings by itself.
 *
 *   marker     <root>/guard/game.mark is written when a game starts with a
 *              risky feature on, and removed after BG_STABLE_SECONDS of play
 *              or at a clean unload.  app.mark does the same for the app
 *              itself, from start-up until the game list first shows;
 *              player.mark for a player program (snes.bin, md.bin) from the
 *              launch until its report (launch/session.txt) is read.
 *   streak     a marker found at the next start is an unfinished start.
 *              Two in a row for the same game (BG_STREAK_LIMIT) put the game
 *              on SAFE settings: guard/safe-<id>.ini.  A clean finish of any
 *              game ends the streak.
 *   safe       boot_guard_safe() is true while that game runs: the caller
 *              keeps Smooth hand-over, Steady pacing and Wide view off, the
 *              Image quality at Original (the look is written as the game's
 *              own, games/<ROM>.ini, so Settings > This game shows it and
 *              can change it) and, at the next start, the fourth core off.
 *              It ends when the player changes one of those rows
 *              (boot_guard_release) or the game is deleted from the guard.
 *   notice     the one-time notice is returned by boot_guard_take_notice().
 *
 * Files are written with fe_ini_doc_save (temp file, read back, rename).  No
 * heap, no threads: the app's main thread only.  Portable C; the tests
 * (tests/guard/test_boot_guard.c, app_tests.py t_boot_guard) run it as is. */
#ifndef BOOT_GUARD_H
#define BOOT_GUARD_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* risky features, a mask */
#define BG_RISK_HANDOFF 1u
#define BG_RISK_CORE4   2u
#define BG_RISK_WIDE    4u
#define BG_RISK_QUALITY 8u
#define BG_RISK_STEADY  16u

#define BG_STABLE_SECONDS 20     /* play that counts as a start that worked   */
#define BG_STREAK_LIMIT   2      /* unfinished starts in a row                */
#define BG_PLAYER_WINDOW_S 300   /* a player that left no report counts only
                                  * when the app is back within this long
                                  * (closed from LiveArea after a long game
                                  * is not a crash)                          */

/* Once, after the log is open: root = the app's data folder.  Settles the
 * markers an earlier run left, then marks this start of the app. */
void boot_guard_init(const char *root);
/* The game list showed (or a game started): the app start worked. */
void boot_guard_app_ready(void);

/* A game is about to run in this process.  `risky` = the BG_RISK_ bits that
 * are on.  Returns 1 when the game is on safe settings. */
int  boot_guard_game_begin(const char *rom_path, unsigned risky);
/* Once a frame (or any often-called place) with the clock in microseconds:
 * after BG_STABLE_SECONDS of play it clears the marker. */
void boot_guard_tick(uint64_t now_us);
/* The game was left cleanly. */
void boot_guard_game_end(void);
/* This game is on safe settings now. */
int  boot_guard_safe(void);
/* The safe picture look (Wide view off, Image quality Original) is written
 * as the game's own look ONCE per safe entry, so Settings > This game can
 * change it afterwards.  1 = write it now (then call nothing: it is noted). */
int  boot_guard_take_look(void);
/* A game (for a player program) is on safe settings. */
int  boot_guard_is_safe_game(const char *rom_path);
/* The fourth core must stay off for this start of the app (the probe runs
 * once, before any game): true while any game is on safe settings that
 * had it on. */
int  boot_guard_core4_off(void);
/* The player changed a guarded row for the running game (Smooth hand-over,
 * Fourth core, Steady pacing): it is theirs again. */
void boot_guard_release(void);

/* A player program (snes.bin, md.bin) is about to be started for rom_path. */
void boot_guard_player_begin(const char *rom_path, unsigned risky);
/* Back in the app after it: had_report = its session report for this launch
 * was found (any exit reason), elapsed_s = wall seconds since the launch.
 * Settles the marker.  Safe to call when no player was started. */
void boot_guard_player_end(int had_report, long elapsed_s);

/* The one-time notice, once: fills three lines (title, line 1, line 2) and
 * returns 1; later calls return 0.  `full` (may be NULL) gets the whole
 * sentence the log carries. */
int  boot_guard_take_notice(char *title, size_t tc, char *l1, size_t c1,
                            char *l2, size_t c2, char *full, size_t cf);
/* The app itself closed twice before the game list: the caller reverts the
 * global risky settings (pcfg_revert_risky) once.  1 = do it now. */
int  boot_guard_take_app_revert(void);

/* tests: forget everything held in memory (not the files) */
void boot_guard_reset_for_tests(void);

#ifdef __cplusplus
}
#endif
#endif
