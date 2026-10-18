/* fe_gbadual.h -- the GBA link mirror's second machine in a frontend
 * (GBA_LINK builds only; docs/GBA-LINK-MIRROR.md).
 *
 * The local machine is the core as the frontend already runs it (instance A,
 * slot 0 here).  This module brings up instance B (the renamed copy of the
 * core) on the same cartridge, starts it from the same save state, and steps
 * the two through gbalink_pair with the cable between them.
 *
 * Milestone 3 (this file): the bench form, no network -- the partner's
 * buttons come from the player (mirror) or from a frame-stamped script.  The
 * network session will feed the same pair.
 */
#ifndef FE_GBADUAL_H
#define FE_GBADUAL_H

#include <stddef.h>
#include <stdint.h>
#include "libretro.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct fe_gbadual_config
{
   const char *rom_path;        /* the cartridge (same as instance A's)     */
   const char *script;          /* partner's button script, or NULL         */
   /* ...or the partner's AUTOPILOT script (fe_autopilot grammar, RAM
    * predicates read the partner's machine); wins over `script`. */
   const char *autopilot;
   int         mirror;          /* partner gets the player's buttons        */
   /* 0 (default): B reads instance A's ROM pool (gbalink.h shared ROM).
    * 1: B loads its own copy of the cartridge (the A/B arm for sharing). */
   int         own_rom;
   uint32_t    lookahead;       /* cycles; 0 = default                       */
   const char *hash_path;       /* per-frame hash of both machines, or NULL */
   /* The frontend's environment callback: instance B asks it the same
    * questions (BIOS directory, core options), so both machines are set up
    * alike; requests that would re-point frontend state are not passed. */
   retro_environment_t env;
   void      (*log)(const char *fmt, ...);
   /* 1: a two-console session (fe_gblink with fe_gbadual_machines) starts
    * the pair; a state load no longer restarts the bench pair. */
   int         session;
   /* 1: the headless partner mixes its sound as before (kill switch for
    * sound.h sound_synth_off; harness/config gba_link_headless_audio = 0). */
   int         partner_audio;   /* -1: silent AND its ring is not read out
                                 * (gba_link_headless_audio = 2, section 23) */
   /* 1: the session's save watch hashes the whole battery image every 30
    * frames (kill switch for the write count; gba_link_sram_crc = 1). */
   int         sram_crc;
   /* Instance B's SMALL-tier translation cache offset mod 8 KiB, PLUS ONE
    * (0 = the build's JIT_PIN_OFFSET, as instance A).  LAYOUT_PIN link
    * builds only (cpu_threaded.c jit_small_pin_at); harness/config
    * gba_link_jit_pin_b.  GBA-LINK-MIRROR.md 17.5. */
   unsigned    b_jit_pin_p1;
   /* 1: the cable fast path in both cores (gbalink_core.c gbalink_fast;
    * harness/config gba_link_fast).  0 = the path every hardware run so far
    * used.  GBA-LINK-MIRROR.md 21. */
   int         cable_fast;      /* 0 off, 1 on, 2 the checking mode,
                                 * 3 on + the lean inline hooks (section 23) */
   /* Progress guard (section 22): the clock, and the longest one pair step
    * may take in milliseconds (0 = 15000; harness gba_link_step_max_ms,
    * negative = no wall-clock bound). */
   uint64_t  (*time_us)(void);
   int         step_max_ms;
} fe_gbadual_config;

/* After instance A has loaded its cartridge and save.  0 = B is up. */
int  fe_gbadual_load(const fe_gbadual_config *cfg);
/* Section 19: instance B's static state belongs to the session.
 * fe_gbadual_boot: once, first thing in the process (B's pristine .data).
 * fe_gbadual_lend(1): lend B's zero-initialised block to instance A's ROM
 * cache (call before A's retro_load_game; withdraw with 0 only after A's
 * ROM cache is gone).  fe_gbadual_load refuses while it is lent. */
void   fe_gbadual_boot(void);
void   fe_gbadual_lend(int on);
int    fe_gbadual_lent(void);
size_t fe_gbadual_lendable(void);
/* B loaded (fe_gbadual_load ran) / give back everything it took. */
int    fe_gbadual_loaded(void);
void   fe_gbadual_unload(void);
/* After instance A loaded a state from `state` (n bytes): B loads the same
 * state and save RAM, and the pair (re)starts with session time 0. */
int  fe_gbadual_state_loaded(const void *state, size_t n);
/* 1 while the pair is running (from the first state load). */
int  fe_gbadual_active(void);
int  fe_gbadual_running_inst(void);   /* DRPROF_HW: 0 A, 1 B, -1 none */
/* One display frame: the local machine's buttons (GBA KEYINPUT order, 1 =
 * pressed) for its next frame; steps until the local machine finished that
 * frame.  0 = ok, -1 = the pair stopped (error logged). */
int  fe_gbadual_frame(uint16_t local_keys);
/* Telemetry lines (heartbeat cadence): `gbadual ...` and `gbadual_health`. */
void fe_gbadual_stats_evt(void (*evt)(const char *fmt, ...));
/* One line for a watchdog thread: which slot is inside run(), runs this
 * step, frames, switches, the partner's guest pc (docs/GBA-LINK-MIRROR.md
 * section 13).  Safe to call from another thread. */
int  fe_gbadual_probe(char *buf, size_t n);
void fe_gbadual_close(void);
/* The pair stopped on an error (gbalink_pair E_*; 0 = none): a bench run
 * ends on it (the session layer ends its own, fe_gblink `gblink_failed`). */
int  fe_gbadual_fault(void);
/* From a watchdog thread: ask both cores to stop at their next scheduler
 * call (gbalink_core.c gbalink_abort).  The pair then fails E_OVERRUN. */
void fe_gbadual_abort(void);

/* The two-console session's machines (fe_gblink.h): the local machine is
 * instance A in slot `local_slot`, the partner instance B in the other.
 * Needs fe_gbadual_load with cfg.session = 1.  NULL when B is not loaded. */
struct fe_gblink_machines;
const struct fe_gblink_machines *fe_gbadual_machines(void);

/* ---- LOCAL LINK: both machines on this console ----------------------------
 *
 * The same pair, with nobody else involved: the player's machine (instance
 * A, slot 0, the parent end of the cable) carries on from where it stands,
 * and the partner's machine (instance B) is DRAWN too and takes its buttons
 * from the frontend -- a second pad, or the one pad switched between the
 * two.  For trading with yourself, trade evolutions, and two players on one
 * console.
 *
 * The partner starts in one of two ways:
 *   clone     a copy of the player's machine: same state, same battery
 *   own save  powered on from `save` (its own battery image, e.g. a second
 *             save file), with no state: it boots the cartridge -- the
 *             player's, or ANOTHER one (`rom_path`: the other version of a
 *             pair of games; instance B then holds its own copy of that
 *             file until the local link stops)
 *
 * Needs fe_gbadual_load with cfg.session = 1 (B loaded and idle), and no
 * session running.  The frontend then calls fe_host_run_frame as always:
 * fe_gbadual_active() is true and the pair is stepped.  A step ends when
 * the PLAYER's machine has finished its frame; the partner's frames end
 * inside steps (its picture and sound arrive through the callbacks then),
 * one per step on average. */
typedef struct fe_gbadual_local
{
   int             clone;
   const uint8_t  *save;        /* own save: B's battery (may be NULL)     */
   size_t          save_n;
   /* own save only: the partner's cartridge, when it is not the player's
    * (NULL or the player's own path = the same cartridge). */
   const char     *rom_path;
   uint32_t        lookahead;   /* cycles; 0 = default                      */
   /* The partner's picture (libretro RGB565; data NULL = a skipped frame)
    * and sound (interleaved L/R), as its core hands them over.  The picture
    * is the core's own buffer, drawn into again as soon as the callback
    * returns: copy it. */
   void          (*video)(const void *data, unsigned w, unsigned h,
                          size_t pitch);
   void          (*audio)(const int16_t *lr, size_t frames);
   /* The partner's buttons for the frame it is about to start (GBA KEYINPUT
    * order, 1 = pressed). */
   uint16_t      (*keys)(void *user);
   void           *user;
} fe_gbadual_local;

enum
{
   FE_GBADUAL_LOCAL_E_BUSY  = -1,  /* B not loaded, or a pair is running    */
   FE_GBADUAL_LOCAL_E_CART  = -2,  /* the partner's cartridge did not load  */
   FE_GBADUAL_LOCAL_E_CLONE = -3   /* the copy could not be made            */
};

/* THE SCOUT (gpsp/wide.h): while instance B is idle and on the player's
 * cartridge, the wide view borrows it to look a second ahead.  Call once a
 * frame with the time that can be spared (microseconds; now_us NULL = a
 * fixed three frames of B, for a host twin with simulated time); it does
 * nothing unless the wide view asks.  1 = B was used this frame. */
int  fe_gbadual_scout_tick(unsigned budget_us, uint64_t (*now_us)(void),
                           uint32_t pad);
void fe_gbadual_scout_stats(unsigned *runs, unsigned *frames,
                            unsigned *clone_us_max, unsigned *ticks,
                            unsigned *run_us);
void fe_gbadual_scout_log(int on);      /* a log line for every look ahead */
/* ...the copy's two halves (the longest of each since the last call), and
 * its frames per one of ours lately, x16. */
void fe_gbadual_scout_stats2(unsigned *ser_us_max, unsigned *unser_us_max,
                             unsigned *rate_x16);

/* 0 = the pair is running; else FE_GBADUAL_LOCAL_E_* and the player's
 * machine is as it was. */
int  fe_gbadual_local_start(const fe_gbadual_local *cfg);
int  fe_gbadual_local_active(void);
/* Nonzero when a local pair stopped by itself (a GBALINK_PAIR_E_* of the
 * stepper, or -1): the frontend should call fe_gbadual_local_stop. */
int  fe_gbadual_local_error(void);
/* Pull the cable: the player's machine carries on alone from its frame
 * boundary (fe_gbadual_frame always returns at one).  Instance B goes back
 * on the player's cartridge if it held another. */
void fe_gbadual_local_stop(void);
/* The partner's battery image (128 KiB) while the local link is up, for the
 * frontend to write to its own file; NULL otherwise.  `writes` = the
 * partner's count of backup-chip writes (changes when the game saves). */
const uint8_t *fe_gbadual_local_partner_save(uint32_t *writes);
/* Whose sound is mixed: the machine that is not heard synthesises nothing
 * (its audio callback still gets its frames, of stale samples: drop them).
 * Takes effect when each machine begins its next frame. */
enum { FE_GBADUAL_HEAR_PLAYER = 0, FE_GBADUAL_HEAR_PARTNER = 1,
       FE_GBADUAL_HEAR_BOTH = 2 };
void fe_gbadual_local_hear(int who);
/* The player's machine was replaced under the pair (it loaded a save state
 * of its own): plug the cable in again.  reclone = the partner becomes a
 * copy of it again (not with another cartridge); else the partner carries
 * on from the end of the frame it is in.  0 = the pair runs. */
int  fe_gbadual_local_restart(int reclone);

/* THE PAIR IN A SAVE STATE.  Both machines, both ends of the cable and the
 * wire between them, taken with both machines at a frame boundary: loading
 * it continues exactly what was saved, a transfer in flight included.  Not
 * the battery images (as a save state of one machine).  _max: a buffer that
 * is always enough, 0 when no local pair runs.  _save: bytes written, or -1
 * (nothing was changed; with the partner mid-transfer in the wrong cycle it
 * can ask to be tried again a frame later -- practically never).  _load: 0,
 * or -1 = not a state of this pair (the cable was plugged in afresh on the
 * machines as they are). */
size_t fe_gbadual_local_state_max(void);
long   fe_gbadual_local_state_save(void *buf, size_t cap);
int    fe_gbadual_local_state_load(const void *buf, size_t n);

/* For tests and telemetry. */
typedef struct fe_gbadual_local_info
{
   uint32_t frames[2], runs[2], switches, lookahead;
   uint32_t mp_xfers[2], n_xfers[2], ev_late[2], late_max[2];
   uint32_t bus_overflow, bus_depth_max;
   int      error;
} fe_gbadual_local_info;
int  fe_gbadual_local_stats(fe_gbadual_local_info *out);
/* Guest RAM of machine `slot` (0 the player's, 1 the partner's). */
int  fe_gbadual_local_peek(int slot, uint32_t addr, void *out, unsigned len);
/* One line per machine per frame with its state hashes (the format of
 * tools/gbamirror's gl_host --hash); NULL closes. */
void fe_gbadual_local_hash_file(const char *path);

/* retropad bitmask -> GBA KEYINPUT order */
uint16_t fe_gbadual_keys_from_retro(uint32_t pad);

#ifdef __cplusplus
}
#endif
#endif
