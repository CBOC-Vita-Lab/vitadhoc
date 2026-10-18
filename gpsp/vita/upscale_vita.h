/* upscale_vita.h -- pixel-art upscalers that run on the CPU, beside the game.
 *
 * The Vita's GPU cannot run the smart scalers as shaders at full screen
 * (xBR measures ~8 fps there), so they are done here from the SOURCE picture
 * -- 240x160 -- and the result is a texture the renderer draws like any
 * other:
 *
 *   UPSCALE_XBRZ    xBRZ 2x-4x: diagonals and curves are rebuilt, flat
 *                   areas and straight edges stay exactly as drawn
 *   UPSCALE_SCALE   Scale2x / Scale3x (EPX): only stair-steps are rounded,
 *                   no new colours; a fraction of xBRZ's cost
 *
 * THE GAME NEVER WAITS FOR IT.  The first build scaled inside the frame, and
 * on a real console xBRZ took 9 to 19 ms of a 16.7 ms frame: the game ran at
 * 33 to 50 frames a second, the sound starved.  So:
 *
 *   - a picture is SUBMITTED and the caller carries on; helper threads on
 *     the cores the emulator does not use scale it into the texture that is
 *     not on screen; the renderer COLLECTS it a frame later and shows it.
 *     If a picture is still being scaled when the next one comes, that next
 *     one is dropped: only the smoothing loses frames, never the game;
 *   - only what changed is scaled again: the picture is cut into stripes
 *     of eight source rows, and in each the columns that differ from the
 *     last picture (and their neighbours).  A menu, a dialogue, a player
 *     standing still cost next to nothing;
 *   - a picture that is the last one SCROLLED is recognised.  The scaled
 *     picture is kept as a RING (in memory and in both textures): a scroll
 *     turns the ring, nothing is moved or written again, and only what came
 *     into view, or moved some other way, is scaled.  The renderer draws the
 *     texture turned back (upscale_ring).  Walking through a map is no
 *     longer the case where every pixel is new -- nor the one where 2.4 MB
 *     are moved and 2.4 MB written to the GPU each frame (the first builds
 *     did that: on a console a walk with xBRZ at "fit" ran at 56 fps with
 *     the sound breaking up, the memory bus being the limit);
 *   - when whole pictures keep taking longer than a frame, the factor steps
 *     down (3x -> 2x) by itself, and back when there is room again.
 *
 * On a host twin a submitted picture is finished before upscale_submit
 * returns (the same code, the helpers joined): what is on screen never
 * depends on a thread's timing, so the tests compare exact pictures.
 * GBADHOC_UPSCALE_ASYNC=1 runs the console's way there too (the race tests).
 *
 * Output is RGBA8 (bytes R,G,B,A, opaque): vback.h's VB_FMT_RGBA8.
 */
#ifndef GBADHOC_UPSCALE_VITA_H
#define GBADHOC_UPSCALE_VITA_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { UPSCALE_NONE = 0, UPSCALE_SCALE, UPSCALE_XBRZ };

/* Two pictures can be in the works at once: the game's (or the pair's
 * left), and the pair's right. */
#define UPSCALE_SLOTS 2

/* The factor to scale slot `slot`'s w x h source by when it will be shown
 * dw wide: the whole factor at or just above that size (so the result is
 * drawn 1:1 or a little smaller, never stretched), within what the filter
 * offers (SCALE: 2-3, XBRZ: 2-4) and a budget of output pixels.  The
 * textures are made for this factor; a picture may be scaled one less
 * (upscale_job_factor) and then fills only the top left of them.
 * 0 = do not upscale. */
int upscale_factor(int slot, int kind, unsigned w, unsigned h, int dw);

/* The factor the picture last submitted to `slot` is scaled by: the one
 * passed to upscale_submit, or less while the screen is changing whole
 * (a fade: 4x becomes 2x, upscale_fade_drop) or one less while the slot's
 * pictures have been taking longer than a frame.  Read it right after upscale_submit took the
 * picture, with upscale_ring. */
int upscale_job_factor(int slot);

/* vita.ini fade_drop: how many factors a fade's 4x pictures go down (2, the
 * default: 2x; 1: 3x; 0: none). */
void upscale_fade_drop(int n);

/* Hand over one picture.  `src` is 16-bit 565 at `pitch_bytes` (copied: the
 * caller may reuse it at once); `bgr` says red is in the LOW bits.
 * `out[2]` are the slot's two textures' own memory (vb_tex_lock) at
 * `out_pitch` bytes a row, each (f*w) x (f*h) RGBA8 at least; the picture is
 * written into out[target] at upscale_job_factor's scale.  The same two pointers must be passed until
 * upscale_forget(slot).
 * Returns 1 = taken; 0 = the slot is still busy with an earlier picture
 * (this one is dropped); -1 = the arguments make no sense or memory ran
 * out. */
int upscale_submit(int slot, int kind, int f, const uint16_t *src,
                   unsigned w, unsigned h, size_t pitch_bytes, int bgr,
                   void *const out[2], size_t out_pitch, int target);

/* Where the picture last submitted to `slot` begins in its texture: the
 * textures hold the scaled picture as a ring, its row y / column x (source
 * pixels) at row (*oy + y) mod h / column (*ox + x) mod w, times the factor
 * -- so that a picture that scrolled is a turn of the ring, not a new
 * texture.  Read it right after upscale_submit took the picture; draw the
 * texture turned back (up to four pieces). */
void upscale_ring(int slot, int *ox, int *oy);

/* 1 once, when the picture last submitted to `slot` is complete in its
 * target (from then on the other texture is the one to submit into). */
int upscale_collect(int slot);

/* 1 while a picture is being scaled for `slot`. */
int upscale_busy(int slot);

/* Wait for the slot's picture in the works, then forget everything about
 * the slot's textures and its last picture: before the textures are
 * destroyed or the picture's size or filter changes. */
void upscale_forget(int slot);

/* The caller has up to `max_us` microseconds with nothing to do (the game's
 * frame is finished and the display's refresh is not due): stripes of the
 * pictures in the works are done here too, one at a time, until they run
 * out or the time does.  A console's emulator thread is idle two thirds of
 * every frame; this is what makes xBRZ fit.  Returns the stripes done. */
int upscale_help(unsigned max_us);

/* Since the last call: pictures scaled, their average and worst time in
 * microseconds (submission to completion), pictures dropped because the
 * slot was busy, the share of the pictures' pixels that had to be scaled
 * (percent), the factor in use, and the helper threads. */
typedef struct upscale_stats_t
{
   unsigned jobs, avg_us, max_us, dropped, scaled_pct, factor, threads;
   unsigned async;             /* 1: the caller does not wait */
   unsigned helped_pct;        /* stripes the caller did in its idle time */
   unsigned slid_pct;          /* pictures that were the last one, scrolled */
   unsigned prep_us;           /* the caller's own time a picture (compare,
                                * find the scroll, slide the buffers)      */
   /* ...split: copy, compare, find the slide, move, mark, expand + masks */
#define UPSCALE_PREP_PARTS 6
   unsigned prep_part[UPSCALE_PREP_PARTS];
   /* The helpers' (and the caller's help) time a picture, all threads
    * added: the scaling, writing the texture. */
   unsigned job_part[2];
   /* Pictures that took longer than a frame: how many, how many of them
    * were slid, and the average share of their pixels scaled (percent). */
   unsigned late, late_slid, late_pct;
   unsigned late_f[3];         /* the late ones at 2x, 3x, 4x             */
   unsigned stepped;           /* times a slot stepped down for time      */
} upscale_stats_t;
void upscale_stats(upscale_stats_t *out);

/* How xBRZ compares colours (third_party/xbrz/xbrz.cpp): 0 its large
 * table, 1 a compact one, 2 arithmetic.  Waits for pictures in the works. */
void upscale_xbrz_dist(int mode);

/* One line of measurements for the log (vita.ini up_bench = 1; a console's
 * numbers are the only ones that count): every filter and factor on
 * `src`, one thread, nothing skipped.  Takes a fraction of a second. */
void upscale_bench(const uint16_t *src, unsigned w, unsigned h,
                   size_t pitch_bytes, char *line, size_t cap);

/* How a picture that is the last one scrolled is followed: 0 not at all
 * (scaled where it changed, like any other), 1 the first builds' way (the
 * scaled picture moved in place by the caller with newlib's memmove, both
 * textures written whole again), 2 the ring turns (upscale_ring; the
 * default).  Waits for pictures in the works. */
void upscale_slide_mode(int mode);

/* The helper threads (a console's thread ids; 0 on a twin): the log's
 * thread line. */
int upscale_thread_ids(int *ids, int max);

/* At exit: the helper threads go away. */
void upscale_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* GBADHOC_UPSCALE_VITA_H */
