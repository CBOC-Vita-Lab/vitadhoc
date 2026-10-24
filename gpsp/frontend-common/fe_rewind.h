/* fe_rewind.h -- rewind core (PROTOTYPE, build switch REWIND, off by default).
 *
 * Platform- and core-independent: it knows nothing about the GBA.  It is
 * handed three operations on "the machine" (gather a state image, restore
 * one, run one frame with a given input) and one block of memory, and it
 * never allocates.  docs/REWIND.md has the design and the measurements.
 *
 * TIMELINE.  Frames are numbered: "frame f" is both the picture the f-th run
 * produced and the machine state right after that run.  Two positions:
 *   now    the frame on screen; where play continues from.
 *   mach   the frame the machine is actually at.  Equal to `now` in normal
 *          play.  While rewinding, and for a few ticks after, the machine is
 *          somewhere else, re-running old frames.
 *
 * WHAT IS KEPT
 *   ref      the NEWEST snapshot, whole (state_bytes), at frame ref_frame.
 *   ring     a byte ring of BACKWARD deltas.  Record k turns snapshot k+1
 *            into snapshot k (XOR of the two, zero runs run-length coded).
 *            Every delta hangs off its NEWER neighbour, so the oldest record
 *            can be dropped at any time without touching the rest: that is
 *            all "the ring wraps" amounts to, there are no keyframes, and
 *            stepping back one snapshot always costs one delta however long
 *            the history is.
 *   log      the input of every frame, a ring indexed by frame number.
 *   capture  the pictures and sound of TWO segments (a segment = the frames
 *            between two snapshots): in normal play, the one being played
 *            and the one before it.
 *
 * TAKING A SNAPSHOT is two steps, deliberately separate:
 *   gather   the machine serialises itself into the gather buffer.  Must run
 *            on the emulation thread at a frame boundary.
 *   pack     compare gather against ref, write the delta, swap the two
 *            buffers.  Touches only rewind memory, so on the PSP it can run
 *            on the second core while the game continues; the only rule is
 *            that it finishes before the next gather or the next step back
 *            (fe_rewind_pack() is idempotent, so "finish" = call it).
 *
 * THE CONTROL IS A LEVEL, SAMPLED EVERY DISPLAY TICK: fe_rewind_tick(held).
 * There is no "begin" and no "end" to get out of step with, which is what
 * makes tapping it like a DJ harmless.  Each tick does a bounded amount of
 * work -- at most `burst` emulated frames and one restore -- whatever the
 * history of presses was:
 *
 *   held      shows the frame before `now` (already captured, so the first
 *             tick of a press costs nothing) and moves `now` back.  In the
 *             background the machine is restored to the snapshot before the
 *             segment being shown and re-runs it with the logged inputs,
 *             capturing, so that segment is ready when `now` reaches it.  If
 *             it is not ready the picture holds for a tick; nothing waits.
 *   released  if the machine is not at `now` it is brought there, `burst`
 *             frames a tick, the picture holding meanwhile (never more than
 *             about two segments of frames).  It only ever runs FORWARD from
 *             a snapshot at or before `now`, taking the snapshots it passes
 *             again, so when play resumes the ring ends at or before `now`:
 *             no snapshot of the abandoned future survives.
 *   pressed again while that is going on: the captures are still there, the
 *             rewind simply continues from `now`.
 */
#ifndef FE_REWIND_H
#define FE_REWIND_H

#include <stddef.h>
#include <stdint.h>

#include "fe_rewind_pack.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct fe_rewind_config
{
   /* The state image is these spans laid end to end (fe_rewind_pack.h): a
    * span is either the machine's own memory, compared where it lies, or a
    * buffer that `gather` refreshes.  The array must outlive the rewind. */
   const fe_rewind_span *spans;
   uint32_t nspans;
   uint32_t ring_bytes;     /* byte budget of the delta ring */
   /* Optional: the ring's own memory.  NULL = it is carved out of the block
    * given to fe_rewind_init like everything else.  Otherwise the first
    * ring_split bytes of the ring are at ring_mem and the remaining
    * ring_bytes - ring_split at ring_mem2 (ring_split == ring_bytes, or 0
    * with ring_mem2 == NULL: one piece).  For a platform whose spare block
    * holds only part of the ring the player asked for. */
   void    *ring_mem;
   void    *ring_mem2;
   uint32_t ring_split;
   /* Test instrument: after every pack, compare the whole machine (every
    * span, every word, maps ignored) with the reference it should now
    * equal.  A difference means a dirty map called a written page clean:
    * counted in stats.verify_bad_words and as an anomaly, and the reference
    * is repaired so one hole is reported once.  Costs a full compare per
    * snapshot. */
   int      verify;
   uint32_t interval;       /* frames between snapshots (>= 1); also the
                             * frames one capture buffer holds */
   /* Wall-clock pacing: 0 = a snapshot every `interval` frames.  Otherwise
    * a snapshot is due after `interval` frames AND pace_us microseconds
    * since the last one (needs now_us), so fast-forward does not take them
    * faster than normal play does; max_span frames force one regardless
    * (0 = 8 * interval). */
   uint32_t pace_us;
   uint32_t max_span;
   uint32_t max_frames;     /* input-log length: the longest rewind, frames */
   uint32_t frame_pixels;   /* pixels of one captured picture (w * h) */
   uint32_t audio_cap;      /* stereo sample frames kept per emulated frame */
   uint32_t burst;          /* emulated frames per rewind/catch-up tick, max
                             * (0 = 2).  1 = never more than normal play. */
   /* With a clock: a tick stops re-running frames once it has taken this
    * long (0 = no limit but `burst`).  It always runs one.  So a light game
    * re-runs several frames a tick and can be gone back over faster than it
    * was played, and a heavy one never makes a tick longer than a frame. */
   uint32_t tick_budget_us;

   /* Make every span's `cur` (and `dirty` map) describe the machine as it
    * is now.  Frame boundary, emulation thread.  0 = ok. */
   int  (*gather)(void);
   /* Optional: the snapshot gather prepared has been taken (the dirty maps
    * were used; start collecting for the next one). */
   void (*snapshot_done)(void);
   /* Load the machine from an image (the spans end to end).  0 = ok. */
   int  (*restore)(const void *image, uint32_t bytes);
   /* Run exactly one frame with `input`, presenting nothing.  pix != NULL:
    * store the picture (frame_pixels RGB565) and up to audio_cap stereo
    * frames, their count in *aud_n (a count above audio_cap = clipped).
    * A frame whose picture could not be produced (a renderer that was busy,
    * a frame fast-forward did not draw) ORs FE_RW_NOPIC into *aud_n. */
   void (*run)(uint32_t input, uint16_t *pix, int16_t *aud, uint32_t *aud_n);
   /* Optional external input (e.g. a sampled cartridge clock). Capture the
    * value used by the normally-run frame; restore it before every replay.
    * Both callbacks or neither. Its ring has exactly the input log's life,
    * so forks, wraps and barriers cannot resurrect an abandoned value.
    * Costs eight bytes per max_frames entry only when enabled. */
   uint64_t (*frame_context)(void);
   void (*replay_context)(uint64_t value);
   /* Optional: the reference image was changed in [off, off + bytes) by a
    * step back, without the machine following yet.  For a restore that
    * copies only what can differ. */
   void (*ref_touched)(uint32_t off, uint32_t bytes);
   /* Optional clock for the cost counters. */
   uint64_t (*now_us)(void);
} fe_rewind_config;

/* One captured frame, as handed to the platform while rewinding.  The audio
 * is already sample-reversed.  Valid until the next fe_rewind call. */
#define FE_RW_NOPIC 0x80000000u /* in an audio count: the frame has no picture */

typedef struct fe_rewind_shown
{
   uint32_t        frame;       /* timeline frame number of this picture */
   const uint16_t *pix;         /* NULL: this frame has none, keep the last */
   uint32_t        pix_frame;   /* the frame `pix` is the picture of: `frame`,
                                 * or one passed on the way to it in this
                                 * tick when `frame` itself keeps none */
   const int16_t  *aud;
   uint32_t        aud_n;       /* stereo frames */
} fe_rewind_shown;

typedef struct fe_rewind_stats
{
   uint32_t snapshots;          /* deltas packed */
   uint64_t packed_bytes;       /* total bytes of those deltas */
   uint32_t packed_max;         /* largest single delta */
   uint32_t evicted;            /* deltas dropped to make room */
   uint32_t overflow_resets;    /* one delta larger than the whole ring */
   uint32_t deferred;           /* frames a due snapshot was put off (busy) */
   uint64_t words_read;         /* words pack compared (of words_total) */
   uint64_t words_total;
   uint64_t gather_us, pack_us; /* totals */
   uint32_t gather_max_us, pack_max_us;
   uint32_t gather_win_max_us, pack_win_max_us;   /* since the last
                                 * fe_rewind_window_reset() */
   uint32_t steps_back;         /* deltas applied backwards */
   uint64_t unpack_us, restore_us;
   uint32_t unpack_max_us, restore_max_us;
   uint32_t restores;
   uint32_t replayed;           /* frames re-run (captured + silent) */
   uint32_t resnaps;            /* snapshots taken again on the way to a
                                 * window from a snapshot far below it */
   uint64_t replay_us;
   uint32_t audio_clipped;      /* frames whose audio exceeded audio_cap */

   /* the control */
   uint32_t engages;            /* presses that started from normal play */
   uint32_t ticks_show;         /* ticks that showed an earlier frame */
   uint32_t ticks_stall;        /* held, frame not ready yet: picture held */
   uint32_t ticks_limit;        /* held at the start of the history */
   uint32_t ticks_catchup;      /* released, machine not yet at `now` */
   uint32_t tick_frames_max;    /* most frames run in one tick (<= burst) */
   uint32_t tick_restores_max;  /* most restores in one tick (<= 1) */
   uint32_t tick_max_us;        /* longest tick */
   uint32_t engage_max_us;      /* longest first tick of a press */
   uint64_t engage_us;
   uint32_t catchup_run_max;    /* longest run of catch-up ticks */
   uint32_t anomalies;          /* an invariant did not hold (must be 0) */
   uint32_t snaps_back, snaps_fwd; /* releases on a no-land frame, moved to
                                 * an earlier / a later one */
   uint32_t snap_max;           /* furthest such move, frames */
   /* cfg.verify: snapshots checked; words the maps hid (must be 0); the
    * image offset (bytes) of the first one ever seen. */
   uint32_t verify_runs;
   uint32_t verify_bad_words;
   uint32_t verify_bad_snaps;
   uint32_t verify_first_off;
} fe_rewind_stats;

/* fe_rewind_tick results */
#define FE_RW_ERROR (-1) /* restore failed: machine untrusted; reset required */
#define FE_RW_PLAY  2   /* not rewinding: run a normal frame now */
#define FE_RW_SHOW  1   /* *out is the frame to present */
#define FE_RW_HOLD  0   /* present nothing new this tick */

/* Bytes of memory fe_rewind_init needs for this configuration. */
size_t fe_rewind_mem_bytes(const fe_rewind_config *cfg);

/* mem: one block of at least fe_rewind_mem_bytes(), 8-byte aligned, owned by
 * the caller for as long as rewind is in use.  0 = ok. */
int  fe_rewind_init(const fe_rewind_config *cfg, void *mem, size_t mem_bytes);

/* Forget all history and restart the timeline at `frame` (the machine's
 * current frame).  This is the barrier: nothing before it can be reached
 * again.  Used at boot, after a state load, at a cartridge save and when
 * rewind becomes unavailable.  Only in normal play (after a tick said
 * FE_RW_PLAY). */
void fe_rewind_reset(uint32_t frame);

/* One display tick.  held = the rewind control is down.  See the top of the
 * file; the caller runs a normal frame if and only if this returns
 * FE_RW_PLAY. */
int  fe_rewind_tick(int held, fe_rewind_shown *out);
/* The same, going back up to `step` frames in this one tick (the picture
 * handed out is the last one reached that has a picture; fewer frames when
 * the ones before are not ready or the history ends).  *stepped, when
 * given, is how many.  step == 0 with the control held: go back nothing
 * this tick (FE_RW_HOLD), but keep preparing -- for a platform that shows a
 * new picture every second or third tick, evenly, when the game cannot be
 * re-run as fast as it is shown. */
int  fe_rewind_tick_step(int held, uint32_t step, fe_rewind_shown *out,
                         uint32_t *stepped);

/* Normal play, around the frame the caller runs after FE_RW_PLAY:
 *   fe_rewind_play_slot   before it: where to put its picture and sound (as
 *                         `run` would).  0 = nothing to capture this frame.
 *   fe_rewind_frame       after it: its input and its audio frame count. */
int  fe_rewind_play_slot(uint16_t **pix, int16_t **aud);
void fe_rewind_frame(uint32_t input, uint32_t aud_n);

/* WHERE A REWIND MAY NOT STOP.  The machine talks to things that are not in
 * a snapshot and that a rewind does not take back (the cartridge save).  The
 * platform says so per frame, in the input word given to fe_rewind_frame:
 *   FE_RW_IN_BUSY    the frame touched such a thing, or left it in the
 *                    middle of something;
 *   FE_RW_IN_CHANGE  the frame changed it.
 * A rewind passes over such frames freely, but play never continues from a
 * frame t that has a BUSY frame among the `quiet` frames up to and including
 * t while a CHANGE lies after t (in this timeline or one that was
 * abandoned): when the control is let go there, `now` moves to the nearest
 * frame where that is not so (the earlier of two equally near), and the
 * control is ignored until the machine has got there.  The frame the rewind
 * began at is always such a frame.  quiet == 0 (the default): anywhere.
 * The low 14 bits of the input word are the input. */
#define FE_RW_IN_BUSY    0x8000u
#define FE_RW_IN_CHANGE  0x4000u
#define FE_RW_IN_KEYS    0x3FFFu
void fe_rewind_set_noland(uint32_t quiet);
/* Drop the history before `frame` (snapshots older than it), keeping the
 * rest.  Only in normal play.  Returns the frame of the oldest snapshot
 * left (the newest one is never dropped, so this can be below `frame`);
 * 0xFFFFFFFF when there is no history. */
uint32_t fe_rewind_trim(uint32_t frame);

/* PICTURE STRIDE while rewinding (1 = every frame, the default).  With
 * stride s only every s-th frame a rewind re-runs keeps a picture -- the
 * ones a rewind stepping s frames at a time lands on -- and the others are
 * run without one (the run callback gets pix == NULL), which on a platform
 * whose pictures are expensive is most of what a re-run frame costs beyond
 * the emulation.  A tick never runs more than ONE frame that keeps a
 * picture.  Takes effect for the frames re-run from now on. */
void fe_rewind_set_stride(uint32_t stride);
/* The tick budget (fe_rewind_config.tick_budget_us), for a platform that
 * gives some ticks more than one display period to work in. */
void fe_rewind_set_budget(uint32_t us);
/* A TARGET for how many seconds the ring should hold (0 = none, the
 * default).  A snapshot's size depends on what the game is doing -- a
 * busy scene can be five times a quiet one -- so with snapshots at a fixed
 * distance the same ring holds 30 s in one place and 6 s in another.  With
 * a target, snapshots are taken further apart while they are large (never
 * closer than `interval`, never further than `gap_max` frames), which is
 * what a scene that rewrites the same memory every frame needs: its
 * snapshots do not grow with their distance.  The price is paid when such
 * a stretch is rewound: the way from each snapshot to its window is re-run
 * as well. */
void fe_rewind_set_target(uint32_t seconds, uint32_t gap_max);
/* Frames between snapshots at the moment (>= interval). */
uint32_t fe_rewind_gap(void);
/* The frames re-run since the last restore are not exact copies of the
 * first pass (the platform skipped something that does not matter to the
 * picture, e.g. mixing sound): the machine must be restored before play
 * continues from it, and no snapshot is taken from it.  Call from the run
 * callback. */
void fe_rewind_taint(void);

/* Normal play: the frame about to be reported with fe_rewind_frame left the
 * platform no time to spare (busy != 0).  A snapshot that is due is then
 * put off, frame by frame, until one has room or max_span frames have gone
 * by since the last (the same bound wall-clock pacing uses).  It stands
 * until the next hint: the checks made before the NEXT frame runs (the
 * tick's, the slot's) must not take the snapshot this frame had no room
 * for. */
void fe_rewind_hint_busy(int busy);

/* The two halves of a snapshot (taken by fe_rewind_frame when due; exposed
 * so a platform can run the second elsewhere).  pack is a no-op when nothing
 * is pending. */
void fe_rewind_gather(void);
void fe_rewind_pack(void);

int      fe_rewind_active(void);        /* rewinding or catching up */
/* 1 = the machine itself stands on a frame play may not continue from (the
 * rule of "where a rewind may not stop", asked of `mach` instead of `now`). */
int      fe_rewind_mach_noland(void);
/* The last tick was held at the start of the history (FE_RW_HOLD because
 * there is nothing earlier to show). */
int      fe_rewind_at_limit(void);
uint32_t fe_rewind_frame_now(void);     /* `now` */
uint32_t fe_rewind_frame_mach(void);    /* `mach`: where the machine really is */
uint32_t fe_rewind_frame_oldest(void);  /* earliest frame that can be shown */
uint32_t fe_rewind_frame_top(void);     /* `now` when this rewind began */
/* The logged input of `frame`; valid for the last max_frames frames, and
 * still valid for frames a rewind has just gone back over. */
uint32_t fe_rewind_logged_input(uint32_t frame);
uint32_t fe_rewind_ring_used(void);
uint32_t fe_rewind_ring_records(void);
const fe_rewind_stats *fe_rewind_get_stats(void);
/* Start a new measurement window: zeroes the *_win_max_us fields only. */
void     fe_rewind_window_reset(void);
/* Structural self-check of the ring and the positions (tests call it every
 * tick).  0 = consistent, else which rule failed. */
int      fe_rewind_selfcheck(void);

#ifdef __cplusplus
}
#endif

#endif /* FE_REWIND_H */
