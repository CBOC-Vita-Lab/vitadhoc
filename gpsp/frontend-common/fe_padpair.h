/* SPDX-License-Identifier: MIT OR GPL-2.0-or-later
 * Written for VitAdhoc by its owner; offered under either licence so the separate players (snes.bin, md.bin) can link it. */
/* fe_padpair.h -- a two-player session on ONE machine, as a fe_gblink
 * machines vtable (docs: ONLINE-2.0 plan, section 4).
 *
 * NES, SNES and WonderSwan games have no serial link to cable two consoles
 * together the way the GB/GBA mirror does; their two players share one
 * machine.  Both consoles therefore run the SAME machine: the host's buttons
 * are pad 1, the guest's are pad 2, and the machine advances one emulated
 * frame only when BOTH players' buttons for that frame are known.  The
 * protocol, the input delay, stall absorption, NACKs, the hashes and the
 * agreed end are fe_gblink's, unchanged (FE_GBLINK_PROTO stays 6):
 *
 *   slot 0 = the machine (its start state and battery come from the host);
 *   slot 1 = nothing: no state, no battery, no hash of its own.
 *
 * What the caller (the frontend) must do:
 *   - HOST: fe_gblink_local.state / .save = the running game's state and
 *     battery image (a state is required: the machine has no "reset" here,
 *     so both consoles start from the host's state); .rom_path = the image
 *     the machine runs.
 *   - GUEST: fe_gblink_local.state = NULL and .save = NULL.  The guest's
 *     own game stays in the machine until start(); this layer then sets its
 *     state and battery image aside, runs the host's, and puts the guest's
 *     own back at stop().  Nothing the guest's console computes during the
 *     session is ever committed (save_size(slot 1) == 0, so fe_gblink calls
 *     commit_save on the host only).
 *   - BOTH: the same cartridge image (fe_gblink's HELLO identity; a
 *     different one is refused with FE_GBLINK_E_ROM_MISSING because there is
 *     no partner_rom), the same core build and emulation options (the
 *     caller's compat word), no turbo / rewind / fast forward / cheats / state
 *     loads while linked.
 *   - Pads are opaque 16-bit masks: whatever the caller passes to
 *     fe_gblink_step() as `local_buttons` arrives, by emulated frame number,
 *     as run_frame's pad1 (host) or pad2 (guest) on both consoles.
 *
 * After fe_gblink_destroy():
 *   - the guest's machine is back on its own game (fe_padpair_restored());
 *   - the host's machine is back on its pre-session state too, UNLESS the
 *     session reached the agreed end frame (then it stays there, exactly the
 *     frame fe_gblink_final_state describes).  The frontend decides what to
 *     keep from fe_gblink_state(): DONE means the host commits its save (the
 *     session already did, through commit_save) and the machine may
 *     continue.
 *   - set_clock(user, 0) was called: the machine reads its own clock again.
 */
#ifndef FE_PADPAIR_H
#define FE_PADPAIR_H

#include <stddef.h>
#include <stdint.h>

#include "fe_gblink.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct fe_pad_machine
{
   void *user;                  /* also names the pair (see _machines) */
   uint8_t console;             /* FE_CONSOLE_*: HELLO's console; peer must match */
   /* bounds HELLO enforces: the cartridge image's size, the battery image's */
   uint32_t rom_min, rom_max, save_max;
   /* One emulated frame with both players' buttons.  0 = a frame completed;
    * anything else ends the session (FE_GBLINK_E_START).  Drawing and sound
    * are the machine's own business. */
   int    (*run_frame)(void *user, uint16_t pad1, uint16_t pad2);
   /* The machine's whole deterministic state (fixed size for a game).  The
    * same bytes loaded on two consoles must run the same frames. */
   size_t (*state_size)(void *user);
   int    (*state_save)(void *user, void *buf, size_t n);        /* 0 = ok */
   int    (*state_load)(void *user, const void *buf, size_t n);  /* 0 = ok */
   /* The battery image (0 bytes = no battery).  save_write only changes the
    * machine's memory; writing storage stays the frontend's job. */
   size_t (*save_size)(void *user);
   int    (*save_read)(void *user, void *buf, size_t n);         /* 0 = ok */
   int    (*save_write)(void *user, const void *buf, size_t n);  /* 0 = ok */
   /* The session's clock (CONFIG's seed, the host's wall clock) for anything
    * that would read a console's own; called with 0 when the session is over
    * (the machine reads its own clock again).  May be NULL. */
   void   (*set_clock)(void *user, int64_t seed);
   /* A cheap fingerprint of the battery RAM: fe_gblink defers the agreed end
    * while it keeps changing, so a save is never cut in half.  May be NULL
    * (0: the end is never deferred). */
   uint32_t (*cart_crc)(void *user);
} fe_pad_machine;

/* The machines vtable to put in fe_gblink_local.machines.  `m` is copied.
 * One pair at a time per machine: the pair is named by m->user (a second
 * call with the same user reuses it, once its session is over); up to
 * FE_PADPAIR_MAX sessions at once (two consoles in one test process).
 * NULL if `m` is incomplete, the pair is in use, or all are running.  Valid
 * until the next call for the same user. */
#define FE_PADPAIR_MAX 4
const fe_gblink_machines *fe_padpair_machines(const fe_pad_machine *m);

/* After the session: 1 if this console's own game was put back (or there
 * was nothing to put back), 0 if a state_load / save_write failed -- the
 * frontend should then reload its own game. */
int fe_padpair_restored(const fe_gblink_machines *v);

#ifdef __cplusplus
}
#endif
#endif
