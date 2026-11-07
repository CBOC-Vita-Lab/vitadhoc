/* cover_custom.h -- a cover picture of the player's own.
 *
 * The home screen, the shelf and the game info show boxart/<stem>.png (the
 * one artfetch.h downloads, or one put there by hand).  A player can set a
 * picture of their own for a game -- a hack, a custom game, a favourite
 * screenshot -- and it takes the place of the downloaded one everywhere,
 * because it IS the file those places read:
 *
 *    boxart/<stem>.png      the picture, scaled down to at most
 *                           COVER_MAX_EDGE px on its long side, shape kept
 *    boxart/<stem>.own      "this one is the player's": artfetch never asks
 *                           for the game, never writes over it, and makes its
 *                           tile thumbnail the whole picture on a square
 *    boxart/<stem>.dl       the downloaded cover it replaced (or one put there
 *                           by hand), kept to come back to
 *    boxart/<stem>.auto     the player chose "Automatic": own pictures off
 *                           for this game (and what the inbox held then)
 *    boxart/<stem>.t2.565   the tile thumbnail, made again (artfetch.h)
 *    boxart/<stem>.v565 .vh565   decoded caches of the old picture, removed
 *
 * covers/ is the inbox: a PNG or JPG named like the game ("Pokemon
 * Unbound.png", also "pokemon unbound.JPG" or "Pokemon Unbound.gba.png") is
 * picked up by cover_inbox_scan() / cover_inbox_step() when the game list
 * opens or is refreshed, and wins over a download.  The file there is never
 * changed or deleted by this code: what was last looked at is written down
 * in the game's .own / .auto, so "Automatic" stays automatic until a new
 * or changed file is put in covers/.
 *
 * Nothing here knows a console: a game is its stem (file name without the
 * extension).  Plain C over stdio/POSIX, UI thread only; the twin runs the
 * same file.
 */
#ifndef GBADHOC_COVER_CUSTOM_H
#define GBADHOC_COVER_CUSTOM_H

#include <stddef.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define COVER_MAX_EDGE 512
#define COVER_INBOX_DIR "covers"

/* `root`: the data folder (boxart/ and covers/ are in it).  Copied. */
void cover_init(const char *root);

/* 1: the game has a cover of the player's own set; 0: automatic. */
int  cover_state(const char *stem);

/* Set the cover from a PNG or JPG file, or from a game's .shot (the
 * emulator's own screenshot format).  0 = done; otherwise -1 and `why`
 * (short, plain English, ASCII) says what is wrong. */
int  cover_set_file(const char *stem, const char *path, char *why, size_t cap);

/* Back to the downloaded cover: the one kept in <stem>.dl is put back; with
 * none, the fetcher is asked to look again (it downloads one when it can).
 * Returns 1 when a kept cover came back, 2 when none is kept (a download
 * may follow), 0 when there was nothing of the player's, -1 on an error. */
int  cover_auto(const char *stem);

/* The game's own latest picture, for "Use a screenshot of the game":
 *   a screenshot named after the game in `shot_dir` (NULL: screenshots/),
 *   else its scene (scenes/<stem>.shot), else the newest save-state picture
 *   (`state1`: the path of slot 1's state, may be NULL), else its title
 *   screen (titles/<stem>.png).
 * Returns 1 and the path, or 0 when there is none. */
int  cover_game_picture(const char *stem, const char *shot_dir,
                        const char *state1, char *path, size_t cap);

/* ---- the inbox ----------------------------------------------------------- */

/* Look at covers/ against these games (their stems) and note the ones whose
 * file is new or changed.  Returns how many are waiting. */
int  cover_inbox_scan(const char *const *stems, int n);
/* The same against the folder as the last scan saw it (reload 0: no look at
 * the memory card at all, for the console switch), or a fresh look (1). */
int  cover_inbox_match(const char *const *stems, int n, int reload);
int  cover_inbox_files(void);   /* pictures in covers/ at the last look */
/* Take over one waiting file (a decode and a write, so one per call).
 * 1 = a cover was set for `stem` (copied to stem_out), 0 = nothing waiting,
 * -1 = one file could not be used (named in stem_out; not tried again until
 * it changes). */
int  cover_inbox_step(char *stem_out, size_t cap);
/* Waiting now. */
int  cover_inbox_pending(void);

/* ---- finding pictures ---------------------------------------------------- */

typedef struct
{
   char   path[600];
   char   label[48];       /* the folder it is in, short */
   time_t mtime;
} cover_pic;

/* PNG and JPG files in these folders and the folders below them (three
 * levels), the newest first.  At most `max` are returned in a malloc'ed
 * array (free() it); *count is how many. */
cover_pic *cover_pictures(const char *const *dirs, int ndirs, int max,
                          int *count);

/* The tile thumbnail path of a stem (boxart/<stem> + ART_THUMB_SUFFIX).
 * The ONE place this file builds it. */
int  cover_thumb_path(const char *stem, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* GBADHOC_COVER_CUSTOM_H */
