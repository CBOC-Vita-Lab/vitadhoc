/* render_thread.h -- the GBA's picture drawn on another core.
 *
 * gpSP draws the GBA's picture line by line while it emulates: at the end
 * of each visible line the core calls its renderer (video.cc
 * update_scanline).  Measured on a console in Emerald, that is 2.2 ms of
 * the emulator thread's 4.8 ms a frame at 444 MHz.
 *
 * HOW.  The machinery is the PSP version's Media Engine renderer (video.h,
 * "ME RENDERER CAPTURE"; docs/ME-MIDFRAME.md):
 *   - the core runs with me_capture_mode = 1: at each visible line it keeps
 *     that line's LCD registers (and the affine reference, and a log of the
 *     palette and OAM writes made while the lines are drawn) and draws
 *     nothing;
 *   - at line 160 (gpsp_visible_done_hook, before the vertical blank's
 *     work) this module copies VRAM, OAM and the palette to the render
 *     thread's own renderer -- gpsp/video.cc built a second time, every
 *     global renamed gbar_* (vita/corebuild.mk, CORE_R; its memory is
 *     vita/render_data.c) -- and wakes the thread;
 *   - the thread replays the 160 lines through that renderer
 *     (me_replay_lines) while the emulator goes on with the vertical blank;
 *   - at the end of the frame the picture is collected (rt_frame_end),
 *     waiting for it if it is not done: the picture shown is the frame's
 *     own, as before.
 *
 * WHEN.  Only for a GBA game, alone (no link, not two machines on one
 * Vita: instance B draws its own).  rt_frame_begin decides it again before
 * every frame.  With the wide view, its margins are still drawn on the
 * emulator's thread as the lines go by (they come from the live game) and
 * only the 240 pixels in the middle move to the thread; rt_frame_end puts
 * the lines together (video.h wide_compose).  Not with the wide view's
 * self-check (wide & 8), which compares its strips with the core's own
 * picture.
 *
 * CHECK.  Mode 2 (vita.ini render_thread = 2): the core draws as before and
 * the thread draws the same frame from the capture; the two are compared
 * every frame and the log's vita_render line counts the frames and pixels
 * that differ.  The picture shown is the core's.
 *
 * Exactness: graphics memory written while the lines are drawn is replayed
 * line by line from a log the core keeps (video_me_log.h): the palette,
 * OAM and VRAM (me_log_vram_always: from the first frame on; the ARM code
 * generator marks the VRAM pages it writes, arm/arm_stub.S).  Mode 2
 * measures what is left.
 */
#ifndef GBADHOC_RENDER_THREAD_H
#define GBADHOC_RENDER_THREAD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { RT_OFF = 0, RT_ON = 1, RT_CHECK = 2 };

/* Before running the core (once per pass of the main loop is enough):
 * `mode` RT_* from now on, RT_OFF when frames must be drawn the old way (a
 * link, a GB game).  Waits for a picture still in the works. */
void rt_frame_begin(int mode);

/* In the core's picture callback, every frame: `pix` is what the core
 * handed over.  Returns the picture to show: the thread's, once it is done
 * (it stays whole until the frame after next), or `pix`. */
const uint16_t *rt_frame_end(const uint16_t *pix);

/* Statistics since the last call (the log's vita_render line). */
typedef struct
{
   unsigned frames;            /* frames the thread drew                  */
   unsigned render_avg_us, render_max_us;  /* the thread's time a frame   */
   unsigned copy_avg_us;       /* the emulator's time at line 160         */
   unsigned wait_avg_us, wait_max_us;      /* the frame end waiting       */
   unsigned checked, bad_frames, bad_px;   /* mode 2                      */
   unsigned log_entries, log_overflows;    /* mid-frame palette/OAM log   */
   unsigned vram_pages;        /* VRAM pages copied a frame, on average   */
} rt_stats_t;
void rt_stats(rt_stats_t *out);

/* Stop the thread (exit). */
void rt_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif
