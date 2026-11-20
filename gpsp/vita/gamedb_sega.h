/* gamedb_sega.h -- the four Sega consoles of md.bin (2.0) as data: the
 * game database's system letter, the libretro-thumbnails folder, the file
 * extensions the library lists, and the folder names the library scans.
 * Header only; nothing here is compiled unless a file includes it.
 *
 * The library lists a console's games from these folders:
 *    ux0:ROMS/<alias>   ux0:roms/<alias>   (the card does not tell them apart:
 *                                          the scanner keeps one of the two)
 *    ux0:data/RetroFlow/ROMS/<retroflow>   (RetroFlow's own folder name, which
 *                                          is also the thumbnail folder's)
 * Each alias is a folder directly under ux0:ROMS (or ux0:roms); several may
 * exist at once.  Everything is matched without regard to case.
 *
 * Extensions (matched without regard to case; a ".zip" holding one of them
 * is read by zip_rom.c as for the other consoles):
 *    Genesis  .md .gen .smd .bin .68k   (.bin only when gamedb_md_accept says so)
 *    SMS      .sms                 Game Gear  .gg      SG-1000  .sg .sc
 * A ".sms" in the Game Gear catalogue (25 games are Game Gear titles that
 * run in Master System mode) is looked up under Master System first, then
 * Game Gear.  A ".bin" that is not a Genesis file (gamedb_md_rom_kind NONE or
 * 32X) is not listed.
 *
 * thumb is the folder name on thumbnails.libretro.com exactly (spaces become
 * %20 in the URL, artfetch.c SYS); the box art of a game is
 * <thumb>/Named_Boxarts/<No-Intro name>.png with  & * / : ` < > ? \ | "
 * in the name written as '_'.  Checked against the libretro-thumbnails
 * repositories Sega_-_Mega_Drive_-_Genesis, Sega_-_Master_System_-_Mark_III,
 * Sega_-_Game_Gear and Sega_-_SG-1000 (their Named_Boxarts folders).
 */
#ifndef GBADHOC_GAMEDB_SEGA_H
#define GBADHOC_GAMEDB_SEGA_H

#define GAMEDB_SEGA_CONSOLES 4

typedef struct
{
   char letter;                 /* gamedb system letter (N records) and cheat key letter */
   int  thumb_index;            /* artfetch.c SYS[] index */
   const char *name;            /* display name */
   const char *thumb;           /* thumbnail folder / No-Intro catalogue name */
   const char *exts;            /* '|' separated, with the dots */
   const char *aliases;         /* '|' separated folder names under ux0:ROMS and ux0:roms */
   const char *retroflow;       /* folder under ux0:data/RetroFlow/ROMS */
} gamedb_sega_console;

static const gamedb_sega_console gamedb_sega_consoles[GAMEDB_SEGA_CONSOLES] = {
   { 'I', 8, "Genesis / Mega Drive", "Sega - Mega Drive - Genesis",
     ".md|.gen|.smd|.bin|.68k",
     "MD|GEN|SMD|Genesis|MegaDrive|Mega Drive|MegaDrive-Genesis|Sega Genesis|Sega Mega Drive|"
     "Sega - Mega Drive - Genesis",
     "Sega - Mega Drive - Genesis" },
   { 'J', 9, "Master System", "Sega - Master System - Mark III",
     ".sms",
     "SMS|MS|MasterSystem|Master System|Sega Master System|Mark III|"
     "Sega - Master System - Mark III",
     "Sega - Master System - Mark III" },
   { 'K', 10, "Game Gear", "Sega - Game Gear",
     ".gg",
     "GG|GameGear|Game Gear|Sega Game Gear|Sega - Game Gear",
     "Sega - Game Gear" },
   { 'L', 11, "SG-1000", "Sega - SG-1000",
     ".sg|.sc",
     "SG|SG1000|SG-1000|SG 1000|Sega SG-1000|Sega - SG-1000",
     "Sega - SG-1000" },
};

#endif
