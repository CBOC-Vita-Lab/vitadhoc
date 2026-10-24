/* gbalink.h -- GBA link cable between two in-process GBA cores ("mirror").
 *
 * Built only with GBA_LINK=1 on the ROOT make (it changes the core).
 * docs/GBA-LINK-MIRROR.md is the design note; this header is the contract
 * between three parts:
 *
 *   gbalink_core.c   compiled INTO the core, so every core instance has its
 *                    own copy: the cable's port logic for that GBA (SIOCNT,
 *                    RCNT, the SIO data registers), the session clock, and
 *                    the run/yield machinery that lets two cores take turns.
 *                    Instance B is the same object with its globals renamed
 *                    (gbab_*), exactly as the GB link does for TGB Dual.
 *   gbalink_bus.c    ONE copy, outside the cores: the wire between the two
 *                    ports -- two time-stamped event queues.
 *   gbalink_pair.c   ONE copy: the stepper that decides which core runs
 *                    next, how far, and feeds each its buttons per frame.
 *
 * Time.  Each core counts "session cycles" (16.78 MHz GBA clock cycles since
 * the session began); both start at 0 together, so the two clocks are the
 * same physical time.  Every cable event carries the time it happened on the
 * sender's clock; the receiver applies it when its own clock reaches that
 * time (or at once, if it is already past it -- "late"; counted).
 *
 * Determinism.  Which core runs, and to what time, is a pure function of the
 * two cores' clocks and the configuration; a stalled input pauses the
 * stepper but never changes its decisions.  So both consoles of a session
 * compute exactly the same two machines.
 */
#ifndef GBALINK_H
#define GBALINK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GBALINK_SLOTS 2

/* Cable events (sender -> the other port). */
enum
{
   GBALINK_EV_WIRES = 1, /* the sender's mode / output pins changed        */
   GBALINK_EV_MP_START,  /* multi-player: the parent started a transfer    */
   GBALINK_EV_MP_DATA,   /* multi-player: the child's word (reply)         */
   GBALINK_EV_N_START,   /* normal mode: an internal-clock transfer began  */
   GBALINK_EV_N_DATA     /* normal mode: the other side's reply            */
};

typedef struct gbalink_event
{
   uint64_t t;           /* sender's session time                          */
   uint64_t t_done;      /* *_START: when the transfer completes           */
   uint32_t v0, v1;      /* payload (see gbalink_core.c)                   */
   uint8_t  kind;
   uint8_t  a, b, c;
} gbalink_event;

/* ---- bus (single copy) -------------------------------------------------- */
void gbalink_bus_reset(void);
/* Queue an event for the OTHER slot.  Returns 0, or -1 if the queue is full
 * (the session must then stop: a dropped event is a different machine). */
int gbalink_bus_post(unsigned from_slot, const gbalink_event *ev);
/* Oldest event queued for `to_slot` that is not a reply, or NULL. */
const gbalink_event *gbalink_bus_peek(unsigned to_slot);
void gbalink_bus_pop(unsigned to_slot);
/* Take (remove) the oldest reply of `kind` queued for `to_slot`; 1 if one
 * was found.  Replies carry no guest-visible effect of their own, so they are
 * taken out of order. */
int gbalink_bus_take_reply(unsigned to_slot, unsigned kind, gbalink_event *out);
/* Diagnostics */
typedef struct gbalink_bus_stats
{
   uint32_t posted[GBALINK_SLOTS];   /* by sender */
   uint32_t overflow;
   uint32_t depth_max;
} gbalink_bus_stats;
void gbalink_bus_get_stats(gbalink_bus_stats *out);
/* The wire's own state -- the events in flight, both directions -- for a
 * save state of a PAIR of machines (the local link; gbalink_pair_sync).
 * gbalink_bus_state_save writes at most `cap` bytes and returns how many, 0
 * if it does not fit; gbalink_bus_state_load returns 0 when `in` was one. */
unsigned gbalink_bus_state_size(void);
unsigned gbalink_bus_state_save(void *out, unsigned cap);
int      gbalink_bus_state_load(const void *in, unsigned n);

/* ---- one core instance -------------------------------------------------- */
enum
{
   GBALINK_RUN_FRAME   = 1,   /* the machine completed a frame              */
   GBALINK_RUN_HORIZON = 2,   /* reached the time it was allowed to run to  */
   GBALINK_RUN_BLOCKED = 3,   /* needs the other port's reply first         */
   GBALINK_RUN_ERROR   = 4,
   /* Progress guard (section 22): the core made more scheduler calls in one
    * run than any run can need, or was told to stop (gbalink_abort).  The
    * machine stands at a postponed update_gba call; the pair must end. */
   GBALINK_RUN_OVERRUN = 5
};

/* Per-instance counters (telemetry, tests). */
typedef struct gbalink_inst_stats
{
   uint32_t yields_horizon, yields_blocked, frames;
   uint32_t ev_in, ev_late;          /* events applied / applied after due */
   uint32_t late_max;                /* worst lateness, cycles             */
   uint32_t mp_xfers, n_xfers;       /* completed transfers on this port   */
   /* gbalink_fast = 2 (the checking mode): disagreements between the fast
    * path's answer and the full walk's, and the first one.  fm_where: 1 the
    * cached first-event time is not the queue's, 2 the full path did
    * something where the fast path would have skipped, 3 next_event's two
    * answers differ. */
   uint32_t fast_checks, fast_mismatch, fm_where;
   uint64_t fm_cached, fm_true, fm_now;
   uint32_t run_calls_max;           /* most scheduler calls in one run    */
} gbalink_inst_stats;

typedef struct gbalink_inst_ops
{
   const char *name;                 /* "A" (the core as linked) or "B"     */
   /* Enter link mode as `slot` (0 = parent end of the cable).  The machine
    * must stand at a frame boundary (after a state load or a whole frame).
    * headless: never draw (the partner machine). */
   void     (*begin)(unsigned slot, int headless);
   void     (*end)(void);
   uint64_t (*now)(void);
   /* Run until the session clock reaches `horizon`, a frame completes, or
    * the cable needs the other port first. */
   int      (*run)(uint64_t horizon);
   /* Apply the events already due (no emulation). */
   void     (*pump)(void);
   /* 1 when the machine has completed a frame and waits for frame_begin. */
   int      (*need_begin)(void);
   /* Buttons for the frame about to start (GBA KEYINPUT bit order, 1 =
    * pressed). */
   void     (*frame_begin)(uint16_t keys);
   /* headless: 0 = drawn and heard, 1 = neither (the partner machine of a
    * two-console session).  The local link shows both machines and uses
    * GBALINK_NO_SOUND (drawn, but no samples are mixed: the machine the
    * player is not listening to) and GBALINK_NO_VIDEO instead; what the game
    * can see of the sound hardware is the same either way (sound.h
    * sound_synth_off). */
   void     (*set_headless)(int headless);
   /* Guest-visible state hashes: [0] regs r0-r15+CPSR, [1] IWRAM data,
    * [2] EWRAM, [3] I/O, [4] palette, [5] OAM, [6] VRAM, [7] all combined. */
   void     (*hash)(uint32_t out[8]);
   void     (*get_stats)(gbalink_inst_stats *out);
   /* Read guest EWRAM (0x02xxxxxx) or IWRAM data (0x03xxxxxx), for input
    * scripts with RAM predicates.  0 = ok, -1 = outside those regions. */
   int      (*peek)(uint32_t addr, void *out, unsigned len);
   /* 32-bit CRC of the battery RAM (the autopilot's waitsram). */
   uint32_t (*sram_crc)(void);
   /* This port's own state -- the transfer in flight, the wires as last
    * heard, the session clock -- which a core save state does not hold.  For
    * a save state of a pair: valid only while the machine stands at a frame
    * boundary (need_begin).  port_load is called after begin() and after the
    * core's own state was loaded; 0 = ok. */
   unsigned (*port_size)(void);
   void     (*port_save)(void *out);
   int      (*port_load)(const void *in, unsigned n);
} gbalink_inst_ops;

/* set_headless / begin, for a machine that is shown: mix no sound; draw
 * nothing (a frame the frontend's fast forward turned away -- the video
 * callback gets NULL, as with the core's own frameskip). */
#define GBALINK_NO_SOUND 2
#define GBALINK_NO_VIDEO 4

/* ---- shared ROM (gba_memory.c, per instance) ---------------------------- *
 * Two machines on the same cartridge read ONE ROM buffer: the instance that
 * loaded it (the owner) exports this, the other (the borrower) sets
 * gamepak_share_in to it before its retro_init and then loads the game as
 * usual -- it maps the owner's pages instead of reading the file, faults its
 * missing pages through `page`, and the owner's page eviction calls the
 * borrower's gamepak_share_unmap (through gamepak_evict_hook).  The borrower
 * keeps a private copy of page 0, because the cartridge's GPIO port (RTC,
 * rumble) is reflected into ROM bytes 0xC4-0xC9 of whatever page 0 the read
 * map points at (update_gpio_romregs): each machine has its own clock. */
typedef struct gbalink_rom_share
{
   uint8_t *const *map_read;        /* the owner's memory_map_read           */
   uint8_t *(*page)(uint32_t phys); /* owner: page `phys`, loading if need be */
   uint32_t size, file_blocks;
   int      mirror_1m, mini_materialized, must_swap;
   /* the owner's backup detection: it scanned the ROM blocks in file order
    * at load, which the borrower cannot see once the owner has paged */
   uint32_t backup_type_reset, flash_device_id, flash_bank_cnt;
} gbalink_rom_share;

/* Defined once per core instance (instance B's is renamed gbab_...). */
void gbalink_inst_get_ops(gbalink_inst_ops *ops);
void gbab_gbalink_inst_get_ops(gbalink_inst_ops *ops);

#ifdef __cplusplus
}
#endif
#endif
