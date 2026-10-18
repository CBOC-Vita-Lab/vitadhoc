/* gbalink_pair.h -- two GBA cores linked by a cable, stepped together.
 *
 * The stepper owns no machine state; it only decides which core runs next
 * and to what session time, and hands each core its buttons when it starts a
 * frame.  Rule (parent leads):
 *
 *   if child.now + margin < parent.now:  run the child to parent.now - margin
 *   else:                                run the parent to child.now + lookahead
 *
 * A core stops at its own first event at or after the time it was given
 * (never at an extra one: that would move guest timing -- gbalink_core.c),
 * and the core's events are at most ~960 cycles apart (the H-blank edges), so
 * a margin of one scanline keeps the child behind the parent: what the parent
 * does on the cable reaches the child on time, and the child's own actions
 * reach the parent at most `lookahead` cycles late.  A core whose
 * transfer needs the other side's bits first is BLOCKED; the other side is
 * then pumped (it applies the events already due) and run as the rule says.
 *
 * The decision depends only on the two clocks and the configuration, so two
 * consoles stepping the same pair compute the same machines; a missing input
 * makes step() return STALL at a point it will resume from unchanged.
 */
#ifndef GBALINK_PAIR_H
#define GBALINK_PAIR_H

#include <stdint.h>
#include "../gbalink.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GBALINK_IN_RING 256u   /* frames of buttons held per slot */

/* One GBA frame: 228 lines x 1232 cycles. */
#define GBALINK_FRAME_CYCLES 280896u
/* Half a frame.  PSP-3000 (2026-10-01, AW2 link session, no per-frame
 * hash): 409 switches a frame at lookahead 2464 cost the pair 21.3 ms a
 * frame, 7 switches at this value 14.2 ms (solo 5.7).  The battle is hash-
 * identical at 2464 / 10000 / 50000 / 140448 (desktop, PPSSPP and this
 * console); a whole frame changes AW2's lobby timing (a child event late). */
#define GBALINK_DEFAULT_LOOKAHEAD 140448u

typedef struct gbalink_pair_config
{
   /* How far (cycles) the parent may run ahead of the child.  Larger means
    * fewer switches between the cores; it bounds how late a child-initiated
    * cable event can reach the parent.  0 = GBALINK_DEFAULT_LOOKAHEAD. */
   uint64_t lookahead;
   /* How far behind the parent the child is stopped (cycles); default 1232.
    * lookahead must exceed it. */
   uint64_t child_margin;
   unsigned local_slot;          /* the machine that is drawn and heard     */
   int      headless[GBALINK_SLOTS];
   /* Called whenever either machine completes a frame (frame = the count of
    * frames that machine has completed, 1-based). */
   void   (*on_frame)(void *user, unsigned slot, uint32_t frame);
   void    *user;
   /* Optional: asked for `slot`'s buttons for `frame` when the machine is
    * about to start it and gbalink_pair_input has not supplied them -- for
    * scripts whose next buttons depend on the RAM the last frame left.
    * Return 1 with *keys set, or 0 (= not known yet: a stall). */
   int    (*input)(void *user, unsigned slot, uint32_t frame, uint16_t *keys);
   /* Progress guard (docs/GBA-LINK-MIRROR.md 22): a clock, and the longest
    * one step() may take (microseconds; 0 or no clock = no wall-clock
    * bound).  A step that exceeds it ends the pair with E_TIMEOUT. */
   uint64_t (*now_us)(void);
   uint64_t step_us_max;
   /* TEST ONLY (gl_host --stress): nonzero seeds a generator that picks a
    * random run horizon inside the rule's limits and pumps a random machine
    * before every run.  Still deterministic for a given seed. */
   uint32_t stress_seed;
} gbalink_pair_config;

typedef struct gbalink_pair_stats
{
   uint32_t runs[GBALINK_SLOTS];
   uint32_t blocked[GBALINK_SLOTS];
   uint32_t stalls;               /* step() calls that returned STALL       */
   uint32_t switches;             /* times the running core changed         */
   /* Progress guard (docs/GBA-LINK-MIRROR.md section 13).  A run that
    * returns HORIZON with neither clock moved is "idle"; the most runs one
    * step() needed, and the most partner frames inside one step(). */
   uint32_t idle_runs;
   uint32_t step_runs_max;
   uint32_t step_peer_frames_max;
} gbalink_pair_stats;

typedef struct gbalink_pair
{
   gbalink_inst_ops m[GBALINK_SLOTS];
   gbalink_pair_config cfg;
   uint32_t frames[GBALINK_SLOTS];
   uint16_t in_keys[GBALINK_SLOTS][GBALINK_IN_RING];
   uint32_t in_tag[GBALINK_SLOTS][GBALINK_IN_RING];   /* frame + 1; 0 = none */
   int      last_ran;
   int      error;                /* GBALINK_PAIR_E_*                       */
   /* For a watchdog thread (heartbeat / stall record): the slot inside
    * run() now (-1 = none), and runs made in the current step(). */
   volatile int      running;
   volatile uint32_t step_runs;
   uint32_t stress_state;
   gbalink_pair_stats st;
} gbalink_pair;

enum { GBALINK_PAIR_FRAME = 1, GBALINK_PAIR_STALL = 2, GBALINK_PAIR_ERROR = 3 };
enum
{
   GBALINK_PAIR_OK = 0,
   GBALINK_PAIR_E_RUN,            /* a core refused to run                   */
   GBALINK_PAIR_E_DEADLOCK,       /* blocked with no way forward (a bug)     */
   GBALINK_PAIR_E_BUS,            /* the cable queue overflowed              */
   GBALINK_PAIR_E_STUCK,          /* runs that move neither clock (a bug)    */
   GBALINK_PAIR_E_RUNAWAY,        /* one step() far past any real frame      */
   GBALINK_PAIR_E_OVERRUN,        /* a core's run made no progress / aborted */
   GBALINK_PAIR_E_TIMEOUT         /* one step() over cfg.step_us_max         */
};

/* Progress guard limits.  Measured (AW2 battle, desktop/PPSSPP/PSP-3000): a
 * step makes <= ~600 runs at lookahead 2464 and ~10 at half a frame, and the
 * partner completes at most 2 frames inside one; idle runs are 0.  The
 * limits are orders of magnitude above that, so they never fire on a
 * working pair; a pair that loops forever stops with an error instead of
 * hanging the console.  Deterministic: both consoles stop at the same run. */
#define GBALINK_PAIR_IDLE_MAX      64u
#define GBALINK_PAIR_STEP_RUNS_MAX 200000u
#define GBALINK_PAIR_STEP_PEER_MAX 16u

/* Both cores must stand at a frame boundary with their state loaded.  Resets
 * the bus and puts both into link mode (slot 0 = s0). */
void gbalink_pair_init(gbalink_pair *p, const gbalink_inst_ops *s0,
                       const gbalink_inst_ops *s1,
                       const gbalink_pair_config *cfg);
void gbalink_pair_end(gbalink_pair *p);

/* Buttons for `slot`'s frame `frame` (0-based: the frame it starts after
 * completing `frame` frames). */
void gbalink_pair_input(gbalink_pair *p, unsigned slot, uint32_t frame,
                        uint16_t keys);
int  gbalink_pair_has_input(const gbalink_pair *p, unsigned slot,
                            uint32_t frame);

/* Advance until the local machine completes a frame (FRAME) or a core needs
 * buttons that have not arrived (STALL). */
int gbalink_pair_step(gbalink_pair *p);

/* Between steps the local machine stands at a frame boundary and the other
 * is a few scanlines short of its own.  This runs the OTHER machine to the
 * end of the frame it is in, so that both stand at a boundary -- the only
 * point at which a core can be saved, loaded or handed back to a frontend
 * whole.  For pairs whose machines started together (frame boundaries at the
 * same session time, as the local link's do): the other machine then passes
 * the local one's clock by at most the few cycles its last instructions
 * overshoot, and nothing the local machine has done is late for it.
 * Returns 1 when both machines wait for frame_begin, 0 when the other could
 * not get there now (it needs a reply the local machine has not reached;
 * step once more and ask again), -1 on an error. */
int gbalink_pair_sync(gbalink_pair *p);

#ifdef __cplusplus
}
#endif
#endif
