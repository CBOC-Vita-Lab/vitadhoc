/* clip_export.h -- "Save clip": the last ten seconds of the game as an
 * animated PNG.
 *
 * clip_vita.h's recorder already keeps the last twenty seconds of the game
 * (for the home screen's previews), squeezed, in a ring of memory.  Saving a
 * clip takes nothing more than that:
 *
 *    1. clip_snapshot copies the squeezed frames of the last ten seconds
 *       (the frame thread's whole cost: a memcpy of 0.5-2 MiB, typically);
 *    2. a thread of its own (plat_job_start, low priority) decodes them one
 *       by one, turns each into RGB, and writes
 *       clips/<game>-<date>-<time>.png with apng_write.h -- the game's own
 *       pixels at its own size, each frame as long as it was on screen;
 *    3. the UI thread asks clip_export_take() once a frame (or a refresh,
 *       in a menu) and says "Clip saved" -- or why not.
 *
 * Memory: the recorder's ring is the same one the previews use (nothing
 * new is kept while playing); the export holds the snapshot (<= 6 MiB, the
 * ring's size, typically 1-2) and two pictures; it declines to start when
 * the heap could not spare that and 8 MiB more.  One export at a time.
 *
 * Portable C: the same file in the twin, where app_tests.py exports a clip
 * and reads it back with a PNG decoder.  */
#ifndef GBADHOC_CLIP_EXPORT_H
#define GBADHOC_CLIP_EXPORT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { CLIPX_STARTED = 0,   /* the thread is on it                           */
       CLIPX_NONE = 1,      /* nothing (or too little) recorded to save       */
       CLIPX_BUSY = 2,      /* the last export is still running               */
       CLIPX_FAIL = -1 };   /* no memory, or no thread                        */

/* Starts saving the recorder's last ten seconds to `path`.  `us_per_tick`:
 * the core's frame time (as for clip_save).  `sync_ok`: when no thread can
 * be had, do it here and now (only while the game is not running: the menu). */
int  clip_export_start(const char *path, unsigned us_per_tick, int sync_ok);

/* 1 once, when an export has ended (UI thread): *ok, the file's path, the
 * frames written and the file's size. */
int  clip_export_take(int *ok, char *path, size_t cap, unsigned *frames,
                      unsigned *bytes);
int  clip_export_busy(void);

/* At exit: stops the export (its half file is removed) and waits up to
 * timeout_us for the thread. */
void clip_export_stop(uint32_t timeout_us);

#ifdef __cplusplus
}
#endif

#endif /* GBADHOC_CLIP_EXPORT_H */
