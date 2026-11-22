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
/* The draw callback says how long its present (vid_swap) waited, once per
 * picture, before it returns: that wait is taken out of the cost the pacer
 * and the look governor are given (the display's queue being full is not
 * drawing load).  ho_swap_stats: the waits in total and how many pictures. */
void ho_note_present_wait(uint32_t us);
void ho_swap_stats(uint64_t *sum_us, uint32_t *n);
/* What the drawing thread did since the last call (Quick response's picture-thread check): its average work for a picture (0: none drawn)
 * and the refreshes the picture side missed (pictures dropped, waits at the gate for a free buffer).  Each call starts the next interval. */
void ho_picture_pass(uint32_t *draw_us, uint32_t *missed);
/* the next ho_picture_pass starts from now (a hand-over began, or the passes were not taken for a while) */
void ho_picture_rebase(void);


/* ---- the fourth CPU core (1.4; playerkit's pk_cores, docs/FOURTH-CORE.md) ----
 * Lives here because this is the glue file that already compiles playerkit
 * into the app.  Roles are pk::Role numbers: 1 render thread, 4 upscale
 * helper (index 0..2), 7 hand-over draw thread.  Everything is a no-op
 * answer ("as today") when the core is off, refused, unproven or lost. */
enum { C4_RENDER = 1, C4_UPSCALE = 4, C4_DRAW = 7 };
/* Once, before the first worker thread exists: off = vita.ini core4 = off.
 * Probes (10 ms with the plugin, up to 300 ms without), plans, logs. */
void core4_init(int off);
/* The planned affinity mask of the role, `dflt` when the role is not
 * planned or the plan leaves it floating (mask 0). */
unsigned core4_mask(int role, int index, unsigned dflt);
/* 1 when the plan puts the role on the extra core. */
int  core4_on_core3(int role, int index);
/* A watchdog slot for a thread on the extra core (-1 when it is not there).
 * beat once per unit of work; busy while it owes progress. */
int  core4_wd_add(const char *name, int role, int index);
void core4_beat(int slot);
void core4_busy(int slot, int busy);
/* Tell the glue the thread's id, so a fallback can move it. */
void core4_set_thread(int role, int index, int thid);
/* Once a frame, on the emulating thread: the reference beat and the
 * watchdog.  1 = the core was lost now and every registered thread has been
 * moved back. */
int  core4_poll(uint64_t now_us);
/* Outside evidence (a wait that ran out): same as a trip.  1 = it changed
 * the state (and the threads were moved). */
int  core4_mark_lost(const char *why);
int  core4_lost(void);
/* "Extra CPU core: in use" and its siblings. */
const char *core4_status(void);

#ifdef __cplusplus
}
#endif
#endif
