/* artfetch.h -- box art for the game list, fetched by itself.
 *
 * A cartridge with no picture in boxart/ gets one from the libretro
 * thumbnail server (thumbnails.libretro.com, the one RetroArch uses), over
 * the console's Wi-Fi, on a thread of its own while the game list is up:
 *
 *    http://thumbnails.libretro.com/<console>/Named_Boxarts/<name>.png
 *
 * where <console> is "Nintendo - Game Boy Advance" (or "- Game Boy",
 * "- Game Boy Color") and <name> the cartridge's No-Intro name with
 * &*\/:`<>?\|" written as _.  The name is the file's own when the file is
 * named the No-Intro way; else the one gamedb.h knows for the GBA header's
 * game code, else for the file's CRC32.  The picture is written to
 * boxart/<file name without extension>.png -- whole or not at all (a .tmp
 * renamed) -- and the list draws it as it would one put there by hand.
 *
 * What it does not find (the server answered "not found" for every name
 * there is) goes into boxart/.missing.txt and is not asked for again;
 * delete a line, or the file, to try again.  Nothing is fetched while a
 * game runs (artfetch_pause), without Wi-Fi, or while the radio is in ad hoc
 * mode (the online check main_vita.c installs).
 *
 * Portable C over BSD sockets: the twin runs the same file against a local
 * server (vita.ini art_url = http://127.0.0.1:PORT; with no art_url the twin
 * fetches nothing).
 */
#ifndef GBADHOC_ARTFETCH_H
#define GBADHOC_ARTFETCH_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Start the thread.  `root` is the data folder (boxart/ is in it),
 * `base_url` "http://host[:port]" (NULL: the libretro server), `online`
 * says whether the network may be used now (NULL: always).  0 = started. */
int  artfetch_start(const char *root, const char *base_url,
                    int (*online)(void));
/* At exit: the thread is asked to stop and given up to a second. */
void artfetch_stop(void);
/* 1 when the worker is running and has not been asked to stop. */
int  artfetch_active(void);

/* The game list's cartridges (full paths and file names), handed over
 * whole; the thread looks at each in turn and skips those with a picture.
 * Any number of calls: the newest list is the one worked through. */
/* Zero entries replaces old work too. Zero return means not accepted. */
#define ART_FETCH_FOCUS_API 1
unsigned artfetch_list(const char *const *paths, const char *const *names,
                       int n);
/* Latest settled selection, copied without I/O. Generations bind it to its list. */
void artfetch_focus(unsigned generation, const char *path, const char *name);

/* 1 while a game runs: nothing is fetched (a download in the works is
 * abandoned). */
void artfetch_pause(int on);

/* Also each cartridge's title screen (libretro's Named_Titles) into
 * titles/<file name without extension>.png -- the home screen's picture for
 * a game never played (vita.ini art_titles = 0: box art only).  Its own
 * titles/.missing.txt. */
void artfetch_titles(int on);
/* Settings > Covers: forget what the server did not have (both .missing.txt
 * files) and go through the list again. */
void artfetch_retry(void);
/* For Settings: cartridges looked at so far, in the list, and with a box art
 * now; returns 1 while the thread is working through the list (0 when it is
 * done, paused, offline or not started). */
int  artfetch_progress(int *seen, int *total, int *have);

/* A picture was written since the last call: 1 and its file name (as given
 * to artfetch_list), for the list to load it again.  The UI thread only. */
int  artfetch_take(char *name, size_t cap);
/* After draining arrivals: 1 if some were coalesced while the UI was away.
 * Invalidate all artwork caches once, including rows outside the current
 * view. Acknowledges the flag;
 * the next call returns 0 unless another overflow occurs. UI thread only. */
int  artfetch_resync(void);

/* Each cover also gets boxart/<stem>.t2.565: the picture fitted into this box
 * (never enlarged) as RGB565, which is all a home-screen tile reads. */
#define ART_THUMB_W 160
#define ART_THUMB_H 200
/* v1 .t565 files reversed red and blue; regenerate without touching originals. */
#define ART_THUMB_SUFFIX ".t2.565"

/* A line for the log, when the thread has one (the UI thread writes it with
 * fe_evt; the thread itself never logs). */
int  artfetch_log(char *line, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* GBADHOC_ARTFETCH_H */
