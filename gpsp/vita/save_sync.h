/* save_sync.h -- keeps a player's battery save safe and, for a save that
 * came from RetroArch, in step with RetroArch's own file.
 *
 * <data>/saves-sync.txt has one line per game save the app has met:
 *
 *   <save path> TAB <source path or "-"> TAB <source size> TAB
 *   <source mtime> TAB <source crc> TAB <our crc>
 *
 * "source" is the RetroArch file (Game.srm in RetroArch's savefiles folder,
 * or beside the game) the save was taken from; its size, time and CRC are
 * those of the bytes last written to it or read from it.  "our crc" is the
 * CRC of the game's save as the app last wrote it, or as it last found and
 * backed it up.
 *
 *  - At a game's start (save_sync_game_start): a save the app has not written
 *    itself (an import, a file put there by hand, one from before this
 *    record) gets ONE copy in <data>/backups/ before anything is saved over
 *    it.  If the source changed since it was last synced, the newer of the
 *    two is used and the other is kept in backups/.
 *  - After every complete write of the battery file (app_battery_written,
 *    called by fe_host): the same bytes go back to the source, in its size
 *    and with its own trailer -- but only if the source still is what was
 *    last synced.  Never over a file RetroArch changed.
 *
 * Without memory beyond small buffers: files are streamed. */
#ifndef SAVE_SYNC_H
#define SAVE_SYNC_H

#include <stddef.h>

void save_sync_init(const char *app_root);
void save_sync_set_enabled(int on);
int  save_sync_enabled(void);

/* RetroArch's savefiles folder (the Vita's; a folder in the data root on the
 * Linux twin, for the tests). */
const char *save_sync_ra_dir(void);

/* The game's save was just taken from `src` (a RetroArch Game.srm): remember
 * the pair.  Call right after the copy. */
void save_sync_link(const char *save_path, const char *src_path);

/* A game is starting with `save_path` as its battery file (after the import).
 * Fills `note` (may be empty) with a short sentence for the player. */
void save_sync_game_start(const char *save_path, char *note, size_t cap);

/* ---- 1.4: a plain copy for RetroArch and other emulators ----------------
 *
 * Every save the app has gets a plain file where RetroArch looks, once per
 * game (record flag), whether or not it ever came from RetroArch (1.0 - 1.2
 * never wrote one; 1.3 only for a save it had taken from RetroArch).  Rules
 * and the file layouts: docs/SAVES.md.  `kind`: */
enum { SS_KIND_GBA = 0, SS_KIND_GB = 1, SS_KIND_GBC = 2, SS_KIND_SNES = 3,
       SS_KIND_NES = 4, SS_KIND_WSWAN = 5 };

/* Call at a game's start, after save_sync_game_start.  `note` (may be empty)
 * is the one-time sentence for the player ("Save is now also at <place>,
 * RetroArch can use it").  Returns 1 = a copy was written, 2 = RetroArch's
 * file is the same save and is now linked, 3 = both kept apart (they differ),
 * 0 = nothing to do / already done / sync off, -1 = failed or deferred (nothing
 * was overwritten; tried again at the next start).  Always logs one
 * `save_census` line. */
int save_sync_heal_game(const char *save_path, const char *rom_path, int kind,
                        char *note, size_t cap);

/* The running game, so that the first save of a NEW game (which has none at
 * its start) also gets its copy: app_battery_written does it, and the sentence
 * waits for save_sync_take_note.  Call with NULLs when the game ends. */
void save_sync_set_game(const char *save_path, const char *rom_path, int kind);
int  save_sync_take_note(char *note, size_t cap);

/* What an older version left beside the game that the game's own folder
 * (states/...) already has a copy of: moved to <data>/leftovers/ after a
 * verified copy.  Returns how many.  (psp/sidecar.h leaves such pairs alone.) */
int save_sync_tidy_beside_rom(const char *old_slot1, const char *new_slot1);

/* The menu action "Export save for other emulators". */
enum { SS_DEST_RETROARCH = 0, SS_DEST_ROM_SAV = 1 };
int save_sync_export_now(const char *save_path, const char *rom_path, int kind,
                         int dest, char *where, size_t wcap, char *note, size_t cap);

/* fe_host's hook: the battery file at `save_path` was just written. */
void app_battery_written(const char *save_path);

#endif
