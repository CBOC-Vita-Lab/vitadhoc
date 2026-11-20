/* ui_vita_md.h -- the four Sega consoles in the game list (2.0), beside ui_vita_snes.h.
 *
 * Genesis / Mega Drive, Master System, Game Gear and SG-1000 are played by md.bin, one program for the four, exactly as the
 * SNES is played by snes.bin: nothing of the core is linked into this program.  What the app knows about them is here:
 * the console id of the launch file, the folder their states live in, the save-sync kind, the game database's system
 * letter (gamedb_sega.h has the rest: extensions, thumbnail folders, folder names).
 *
 * TWO PLAYERS.  A Sega game's Two players is formed by the app the way a SNES game's is (link_snes.h: the same rooms, code,
 * punch and relay; the launch file's link.* keys; playerkit/FORMATS.md) and then md.bin plays it.  The path is written in
 * main_vita.c (sega_launch / player_launch_write take the same link_snes_link as the SNES one) but is SWITCHED OFF until the
 * link module of the player (playerkit, MIT) lands: UI_SEGA_TWO_PLAYERS is 0 and the Settings > Two players row stays hidden for
 * a Sega game.  To switch it on:
 *   1. build the player with the link module (md/vita/netplay.*) and set UI_SEGA_TWO_PLAYERS to 1 here;
 *   2. psp/ui_psp.c set_row_shown(SET_SNES_TWO): `pcfg_display_console() == FE_CONSOLE_SNES` becomes ui_console_is_player();
 *   3. main_vita.c: the `two` value of the Sega branch (sega_two) reads g_join_pending / snes_menu like the SNES branch;
 *   4. link_snes_game_id() (link_vita.c) learns the four Sega file kinds for the room's game id (ui_host_snes_game_id).
 * Plain C, header only; included by psp/ui_psp.c and by main_vita.c. */
#ifndef UI_VITA_MD_H
#define UI_VITA_MD_H

#include "fe_console.h"

#define UI_SEGA_TWO_PLAYERS 1

/* 1 for the four consoles md.bin plays. */
static inline int ui_console_is_sega(int console)
{
   return console >= FE_CONSOLE_MD && console <= FE_CONSOLE_SG;
}

/* 1 for a console a separate program plays (snes.bin or md.bin): rows that only an in-process core has are not for it. */
static inline int ui_console_is_player(int console)
{
   return console == FE_CONSOLE_SNES || ui_console_is_sega(console);
}

/* 1 when a game of this console has Two players: md.bin's link (playerkit pk_link). Not the Game Gear: it has one controller. */
static inline int ui_sega_two_ok(int console)
{
   return UI_SEGA_TWO_PLAYERS && ui_console_is_sega(console) && console != FE_CONSOLE_GG;
}

/* The launch file's console id (launch/<id>.ini, `console=<id>`), or NULL when md.bin does not play it. */
static inline const char *ui_sega_id(int console)
{
   return console == FE_CONSOLE_MD ? "md" : console == FE_CONSOLE_SMS ? "sms"
        : console == FE_CONSOLE_GG ? "gg" : console == FE_CONSOLE_SG ? "sg" : NULL;
}

/* The game database's system letter (gamedb.h): I Genesis, J Master System, K Game Gear, L SG-1000. */
static inline char ui_sega_letter(int console)
{
   return (char)('I' + (console - FE_CONSOLE_MD));
}

#endif
