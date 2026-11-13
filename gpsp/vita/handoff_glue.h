/* Smooth hand-over (1.4): the game's picture is handed to a drawing thread
 * through playerkit's mailbox (playerkit/src/pk_handoff.h, MIT; recipe in
 * playerkit/INTEGRATION.md section 7).  The emulating thread runs the core,
 * the sound and the pad and only copies the finished picture; the drawing
 * thread draws it (filters, xBRZ, the wide view's compose), the toasts over
 * it, and presents.  Settings > Gameplay > Smooth hand-over (on by default),
 * off in every link and in the bench pair.
 *
 * Who touches the display: while a picture is in flight only the drawing
 * thread.  Anything else that draws (the menus, the pause screen, rewind's
 * standing picture, a modal) waits for the drawing thread to go idle first
 * -- video_vita.c calls vid_enter_hook at its drawing entry points, which
 * does that wait (ho_guard) -- so no caller has to remember it.
 *
 * The twin (hostcore) runs the same mailbox; by default the "drawing thread"
 * is the emulating thread itself, right where the old code presented
 * (deterministic: every screenshot is the one the old path made).
 * GBADHOC_HANDOFF_THREAD=1 runs a real second thread, GBADHOC_HANDOFF_SLOW_US
 * makes it slow (the bit-identical proof, app_tests.py t_handoff). */
#ifndef GBADHOC_HANDOFF_GLUE_H
#define GBADHOC_HANDOFF_GLUE_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* What the drawing side does with one picture (main_vita.c): draw it, the
 * OSD snapshot over it, present.  `extra` is the host's own bytes that went
 * with it (the OSD snapshot). */
typedef void (*ho_draw_fn)(const uint16_t *pix, unsigned w, unsigned h,
                           size_t pitch, uint32_t seq, int user,
                           const void *extra);

/* Start for one game: frame_us = one emulated frame; extra_bytes = the
 * host's bytes per picture.  draw_core / draw_prio: vita.ini (-1 = default).
 * 1 = running. */
int  ho_start(uint32_t frame_us, size_t extra_bytes, ho_draw_fn draw,
              int draw_core, int draw_prio);
void ho_stop(void);
int  ho_running(void);          /* started (the setting may still be off) */
int  ho_active(void);           /* started and enabled: pictures go through it */
int  ho_threaded(void);         /* a real drawing thread (always on a console) */
void ho_set_enabled(int on);    /* called on a change only */
const char *ho_where(void);     /* "core 1 prio 0x60" / "inline (twin)" */

/* ---- the emulating thread ---- */
/* A buffer for this picture, copied in (rows of w*2 bytes); it stays the
 * producer's until ho_commit / ho_abandon.  0 = not staged (draw inline). */
int  ho_stage(const uint16_t *pix, unsigned w, unsigned h, size_t pitch,
              uint32_t seq, int user);
int  ho_staged(void);
void *ho_staged_extra(void);    /* the host's bytes of the staged picture */
void ho_commit(uint32_t emu_us);
void ho_abandon(void);
/* the twin's inline drawing thread: draw what is queued, now */
void ho_consume_inline(void);
void ho_note_emulated(uint32_t n);
void ho_set_pace(int half);
void ho_set_force_drop(int on);
int  ho_poll_cost(uint32_t *cost_us);
int  ho_flush(void);
int  ho_discard(void);
void ho_time_baseline(void);
/* once a second: the "handoff: ..." line (1 = written) */
int  ho_census_line(char *buf, size_t n);
/* guard: called before anything else draws (vid_enter_hook) */
void ho_guard(void);
unsigned ho_guard_waits(void);  /* how often the guard had to wait */
uint32_t ho_shown_total(void);
/* the drawing side's work in total (us) and pictures drawn: the look
 * governor's per-second draw cost */
void ho_draw_stats(uint64_t *sum_us, uint32_t *n);

#ifdef __cplusplus
}
#endif
#endif
