/*
 * VitAdhoc: the VDP worker thread (write log). Copyright CBOC-Vita; the core keeps the Genesis Plus GX licence (md/core/LICENSE.txt).
 *
 * With the worker on, the emulating thread does everything it did before except drawing: every register, VRAM, CRAM, VSRAM write still
 * happens on its own copy of the VDP (the CPUs see the same machine), and each "draw this line" is appended, with the VDP state the drawing
 * needs, to a single producer / single consumer queue. The worker owns a second copy of that state and runs the UNCHANGED line renderer
 * (vdp_render.c, compiled a second time as vdp_render_w.c) on it, in the same order, so the picture is the one the single thread made.
 *
 * What comes back: the sprite collision / overflow bits of the status register and the collision position. A read of the VDP status
 * (or the HV counter) waits until the worker has finished every queued line, then ORs its bits in (vdpw_join_status): a game that polls
 * the status inside the picture degrades towards one thread, never below. Joins: status / HV read, the end of every frame (so a host
 * call between frames sees an idle worker and a finished picture), reset, state save / load and option changes all happen between frames.
 *
 * The calls the core made into the renderer (render_line, blank_line, remap_line, parse_satb, window_clip, color_update_*, render_init,
 * render_reset) go through the wrappers at the end of this file; with the worker off each is exactly the old direct call.
 */
#ifndef VDP_THREAD_H
#define VDP_THREAD_H

#include "thread_ops.h"

extern int vdpw_on;                                   /* 1 while the worker runs */
int vdpw_start(const md_thread_ops *ops);             /* before system_init; 0 on success */
void vdpw_stop(void);                                 /* joins, ends the thread */
void vdpw_join(void);                                 /* every queued line is drawn */
void vdpw_join_status(void);                          /* vdpw_join + the renderer's status bits and spr_col into the emulating thread's copy */
void vdpw_frame_end(void);                            /* end of a frame: join, status merged, a check in debug mode */
void vdpw_status_changed(void);                       /* the emulating thread wrote status bits itself (reset, state load) */
void vdpw_vram_all_changed(void);                     /* VRAM was rewritten in bulk without the per-pattern marks */
void vdpw_mark_vram(unsigned name);                   /* one 32 byte VRAM pattern changed (MARK_BG_DIRTY) */
void vdpw_mark_sat(unsigned off);                     /* one 32 byte chunk of internal SAT changed */
void vdpw_mark_vsram(unsigned off);                   /* one 32 byte chunk of VSRAM changed */
void vdpw_small_all_changed(void);                    /* SAT / VSRAM rewritten in bulk (reset, state load) */
unsigned vdpw_spr_col(void);
void vdpw_q_call(void (*fn)(void *), void *arg);   /* fn(arg) on the worker after everything queued, its copy of the VDP level with the emulating thread's */
void vdpw_wide_cache(int on, int check);           /* the worker renderer's strip cache (on by default with the worker); check: draw anyway and compare */
void vdpw_wide_cache_stat(unsigned long out[3]);   /* strips copied, drawn, copies that differed (check mode) */

/* census (the emulating thread's view; counters since start) */
typedef struct {
	unsigned long records, words, status_joins, joins, join_wait_polls, full_waits, wakes;
	unsigned long vram_blocks, vram_alls;
	unsigned long vdp_us, join_wait_us;     /* the worker's busy time, the emulating thread's time waiting in joins (0 without ops->now_us) */
} vdpw_census_t;
void vdpw_census(vdpw_census_t *c);

/* debug: compare the worker's copy of the raw VDP state with the emulating thread's at every join (md_twin --vdp-check). Returns the number of
 * differences found so far. */
void vdpw_set_check(int on);
unsigned long vdpw_check_errors(void);
void vdpw_set_stress(int on);       /* debug: a join after every drawn line */

/* VitAdhoc: a frame the host does not want drawn (fast forward, half-rate pacing, the real frame behind a run-ahead picture) still has to
 * leave the machine as a drawn frame would: the sprite collision and overflow bits of the status register come out of the sprite renderer.
 * While vdp_status_only is set (system_frame_* sets it for such a frame) every render_line() call carries VDP_LINE_STATUS_ONLY and the
 * renderer does the sprite pass and the sprite table parse only: no background, no pixels to the frame buffer, no wide view strip. */
#define VDP_LINE_STATUS_ONLY 0x10000
extern uint8 vdp_status_only;

int vdpw_carry_size(void);        /* bytes the drawing renderer's carried state takes (sprite lists, pending flags) */
void vdpw_carry_save(void *buf);
void vdpw_carry_load(void *buf);

/* wrapper entry points (the queue side) */
void vdpw_q_render_line(int line);
void vdpw_q_remap_line(int line);
void vdpw_q_blank_line(int line, int offset, int width);
void vdpw_q_parse_satb(int line);
void vdpw_q_window_clip(unsigned data, unsigned sw);
void vdpw_q_color(int m5, int index, unsigned data);
void vdpw_q_render_init(void);
void vdpw_q_render_reset(void);

#ifndef VDPW_NO_WRAP
/* The names below are macros so that the core's own sources (vdp_ctrl.c, system.c) stay as they were. The parentheses around the real
 * function in the else branches stop the macro expanding again. */
static inline void vdpw_do_render_line(int l) { if (vdp_status_only && l >= 0) l |= VDP_LINE_STATUS_ONLY; if (vdpw_on) vdpw_q_render_line(l); else (render_line)(l); }
static inline void vdpw_do_remap_line(int l) { if (vdpw_on) vdpw_q_remap_line(l); else (remap_line)(l); }
static inline void vdpw_do_blank_line(int l, int o, int w) { if (vdpw_on) vdpw_q_blank_line(l, o, w); else (blank_line)(l, o, w); }
static inline void vdpw_do_parse_satb(int l) { if (vdpw_on) vdpw_q_parse_satb(l); else (parse_satb)(l); }
static inline void vdpw_do_window_clip(unsigned d, unsigned s) { if (vdpw_on) vdpw_q_window_clip(d, s); else (window_clip)(d, s); }
static inline void vdpw_do_color_m4(int i, unsigned d) { if (vdpw_on) vdpw_q_color(0, i, d); else (color_update_m4)(i, d); }
static inline void vdpw_do_color_m5(int i, unsigned d) { if (vdpw_on) vdpw_q_color(1, i, d); else (color_update_m5)(i, d); }
static inline void vdpw_do_render_init(void) { if (vdpw_on) vdpw_q_render_init(); else (render_init)(); }
static inline void vdpw_do_render_reset(void) { if (vdpw_on) vdpw_q_render_reset(); else (render_reset)(); }
#define render_line(l) vdpw_do_render_line(l)
#define remap_line(l) vdpw_do_remap_line(l)
#define blank_line(l, o, w) vdpw_do_blank_line(l, o, w)
#define parse_satb(l) vdpw_do_parse_satb(l)
#define window_clip(d, s) vdpw_do_window_clip(d, s)
#define color_update_m4(i, d) vdpw_do_color_m4(i, d)
#define color_update_m5(i, d) vdpw_do_color_m5(i, d)
#define render_init() vdpw_do_render_init()
#define render_reset() vdpw_do_render_reset()
#define spr_col vdpw_spr_col()
#endif

#endif
