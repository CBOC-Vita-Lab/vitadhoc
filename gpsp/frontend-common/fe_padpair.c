/* SPDX-License-Identifier: MIT OR GPL-2.0-or-later
 * Written for VitAdhoc by its owner; offered under either licence so the separate players (snes.bin, md.bin) can link it. */
/* fe_padpair.c -- see fe_padpair.h.
 *
 * One machine, both players: fe_gblink's lockstep (inputs by frame number,
 * an input delay, stalls that cost time and never correctness) is exactly
 * what two consoles need to run the same machine, so this is only the
 * adapter between that vtable and a machine with two pads. */
#include "fe_padpair.h"
#include "fe_util.h"

#include <stdlib.h>
#include <string.h>

typedef struct pp
{
   fe_pad_machine m;
   fe_gblink_machines v;
   int used;
   int running;
   int broken;                   /* run_frame failed: no more frames */
   int local_slot;
   int ended;                    /* the session read slot 0 at its end frame */
   int restore_ok;
   uint64_t frames;              /* frames the machine has completed */
   void (*hook)(void *hu, int slot, uint64_t frame);
   void *hook_user;
   /* this console's own game, set aside while the session runs */
   uint8_t *own_state;
   size_t own_state_n;
   uint8_t *own_save;
   size_t own_save_n;
   /* the sync hash's buffer (allocated once, not per hash) */
   uint8_t *hbuf;
   size_t hbuf_n;
} pp_t;

static pp_t POOL[FE_PADPAIR_MAX];

static void release_buffers(pp_t *p)
{
   free(p->own_state);
   free(p->own_save);
   free(p->hbuf);
   p->own_state = p->own_save = p->hbuf = NULL;
   p->own_state_n = p->own_save_n = p->hbuf_n = 0;
}

/* Put this console's own game back.  Order: the state first, then the
 * battery image, so the battery is exactly the player's whatever the state
 * carries. */
static void restore_own(pp_t *p)
{
   p->restore_ok = 1;
   if (p->own_state &&
       p->m.state_load(p->m.user, p->own_state, p->own_state_n) != 0)
      p->restore_ok = 0;
   if (p->own_save && p->own_save_n &&
       p->m.save_write(p->m.user, p->own_save, p->own_save_n) != 0)
      p->restore_ok = 0;
}

static int pp_start(void *user, int local_slot, unsigned batch,
                    int64_t clock_seed,
                    const uint8_t *const state[2], const size_t state_n[2],
                    const uint8_t *const save[2], const size_t save_n[2],
                    void (*frame_hook)(void *hu, int slot, uint64_t frame),
                    void *hook_user)
{
   pp_t *p = (pp_t *)user;
   size_t want_state, want_save;
   (void)batch;                   /* a vtable pair's quantum: unused here */
   if (p->running)
      return -1;
   /* Reject a bad payload before the machine is touched.  Slot 1 has
    * nothing; slot 0's state and battery must be exactly this machine's
    * sizes (same cartridge, same core build). */
   want_state = p->m.state_size(p->m.user);
   want_save = p->m.save_size(p->m.user);
   if (!want_state || !state[0] || state_n[0] != want_state ||
       state_n[1] != 0 || save_n[1] != 0 ||
       save_n[0] != want_save || (save_n[0] && !save[0]) ||
       want_save > p->m.save_max)
      return -1;
   p->local_slot = local_slot ? 1 : 0;
   p->ended = 0;
   p->frames = 0;
   p->restore_ok = 1;
   /* Set the own game aside (the guest's is never committed; the host's
    * comes back too unless the session reaches its end). */
   p->own_state = (uint8_t *)malloc(want_state);
   p->hbuf = (uint8_t *)malloc(want_state);
   p->own_save = want_save ? (uint8_t *)malloc(want_save) : NULL;
   p->own_state_n = want_state;
   p->own_save_n = want_save;
   p->hbuf_n = want_state;
   if (!p->own_state || !p->hbuf || (want_save && !p->own_save) ||
       p->m.state_save(p->m.user, p->own_state, want_state) != 0 ||
       (want_save && p->m.save_read(p->m.user, p->own_save, want_save) != 0))
   {
      release_buffers(p);
      return -1;
   }
   /* Both consoles start from the host's state and battery. */
   if (p->m.state_load(p->m.user, state[0], want_state) != 0 ||
       (want_save && p->m.save_write(p->m.user, save[0], want_save) != 0))
   {
      restore_own(p);
      release_buffers(p);
      return -1;
   }
   if (p->m.set_clock)
      p->m.set_clock(p->m.user, clock_seed);
   p->hook = frame_hook;
   p->hook_user = hook_user;
   p->running = 1;
   return 0;
}

static int pp_advance(void *user,
                      int (*input)(void *iu, int slot, uint64_t frame,
                                   uint16_t *buttons),
                      void *input_user)
{
   pp_t *p = (pp_t *)user;
   uint16_t k[2];
   int local, other;
   if (!p->running || p->broken)
      return -1;
   local = p->local_slot;
   other = local ^ 1;
   /* The local player's buttons first: asking enqueues (and sends) them
    * for the frame `delay` ahead even while the partner's are late, as the
    * other machines do. */
   if (!input(input_user, local, p->frames, &k[local]) ||
       !input(input_user, other, p->frames, &k[other]))
      return 0;                    /* resumable: nothing has changed */
   if (p->m.run_frame(p->m.user, k[0], k[1]) != 0)
   {
      p->broken = 1;               /* the session ends; stop() restores */
      return -1;
   }
   p->frames++;
   /* Slot 0 first: the session takes the final hash on slot 0's hook and a
    * guest captures on its own (slot 1's) hook, after it. */
   p->hook(p->hook_user, 0, p->frames);
   p->hook(p->hook_user, 1, p->frames);
   return 1;
}

/* CRC-32 of the machine's state, with the frame count above it: the same
 * computation on both consoles at equal points.  Not computed every frame:
 * fe_gblink asks once per hash interval. */
static uint64_t pp_sync_hash(void *user)
{
   pp_t *p = (pp_t *)user;
   uint32_t crc;
   if (!p->running || p->m.state_save(p->m.user, p->hbuf, p->hbuf_n) != 0)
      return 0;
   crc = fe_crc32(0, p->hbuf, p->hbuf_n);
   return ((uint64_t)(uint32_t)p->frames << 32) | crc;
}

static uint32_t pp_cart_crc(void *user, int slot)
{
   pp_t *p = (pp_t *)user;
   return slot == 0 && p->m.cart_crc ? p->m.cart_crc(p->m.user) : 0;
}

static size_t pp_save_size(void *user, int slot)
{
   pp_t *p = (pp_t *)user;
   return slot == 0 ? p->m.save_size(p->m.user) : 0;
}

static int pp_save_read(void *user, int slot, void *buf, size_t n)
{
   pp_t *p = (pp_t *)user;
   if (slot != 0)
      return n == 0 ? 0 : -1;
   return p->m.save_read(p->m.user, buf, n);
}

static size_t pp_state_size(void *user, int slot)
{
   pp_t *p = (pp_t *)user;
   return slot == 0 ? p->m.state_size(p->m.user) : 0;
}

static int pp_state_save(void *user, int slot, void *buf, size_t n)
{
   pp_t *p = (pp_t *)user;
   if (slot != 0)
      return n == 0 ? 0 : -1;
   /* The session reads slot 0 only when the agreed end frame is reached. */
   if (p->running)
      p->ended = 1;
   return p->m.state_save(p->m.user, buf, n);
}

static void pp_stop(void *user)
{
   pp_t *p = (pp_t *)user;
   if (!p->running)
      return;
   p->running = 0;
   /* A guest never keeps the host's game; a host keeps the machine only if
    * the session reached its end frame, else it is back where it was. */
   if (p->local_slot == 1 || !p->ended)
      restore_own(p);
   if (p->m.set_clock)
      p->m.set_clock(p->m.user, 0);
   release_buffers(p);
}

const fe_gblink_machines *fe_padpair_machines(const fe_pad_machine *m)
{
   pp_t *p = NULL;
   int i;
   if (!m || !m->run_frame || !m->state_size || !m->state_save ||
       !m->state_load || !m->save_size || !m->save_read || !m->save_write ||
       m->rom_max < m->rom_min)
      return NULL;
   for (i = 0; i < FE_PADPAIR_MAX; i++)
      if (POOL[i].used && POOL[i].m.user == m->user)
      {
         if (POOL[i].running)
            return NULL;
         p = &POOL[i];
         break;
      }
   for (i = 0; !p && i < FE_PADPAIR_MAX; i++)
      if (!POOL[i].used)
         p = &POOL[i];
   /* None free: take over one whose session is over (a frontend that
    * names its machines by a pointer that changes must not run out). */
   for (i = 0; !p && i < FE_PADPAIR_MAX; i++)
      if (!POOL[i].running)
         p = &POOL[i];
   if (!p)
      return NULL;
   release_buffers(p);
   memset(p, 0, sizeof(*p));
   p->used = 1;
   p->restore_ok = 1;
   p->m = *m;
   p->v.user = p;
   p->v.console = m->console;
   p->v.header_off = 0;
   /* HELLO carries 0x1C header bytes read from the image's start */
   p->v.rom_min = m->rom_min > 0x1Cu ? m->rom_min : 0x1Cu;
   p->v.rom_max = m->rom_max;
   p->v.save_max = m->save_max;
   p->v.start = pp_start;
   p->v.advance = pp_advance;
   p->v.sync_hash = pp_sync_hash;
   p->v.cart_crc = pp_cart_crc;
   p->v.save_size = pp_save_size;
   p->v.save_read = pp_save_read;
   p->v.state_size = pp_state_size;
   p->v.state_save = pp_state_save;
   p->v.stop = pp_stop;
   p->v.partner_rom = NULL;      /* one cartridge: a different one is refused */
   return &p->v;
}

int fe_padpair_restored(const fe_gblink_machines *v)
{
   const pp_t *p = v ? (const pp_t *)v->user : NULL;
   return p ? p->restore_ok : 1;
}
