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

/* fe_host's hook: the battery file at `save_path` was just written. */
void app_battery_written(const char *save_path);

#endif
