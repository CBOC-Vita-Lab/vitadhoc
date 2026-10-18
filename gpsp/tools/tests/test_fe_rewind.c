/* test_fe_rewind.c -- the rewind core against a fake machine, no ROM needed.
 *
 *   gcc -std=gnu99 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
 *       -Ifrontend-common -o /tmp/t_fe_rewind tools/tests/test_fe_rewind.c \
 *       frontend-common/fe_rewind.c && /tmp/t_fe_rewind
 *
 * The machine is a 64 KiB state that every frame mixes with its input: a
 * handful of words usually, thousands on a "scene change".  Its inputs are a
 * pure function of the frame number, so a clean run that never rewinds gives
 * the truth for every frame: the state hash after it, its picture, its
 * sound.
 *
 * Each case then drives fe_rewind_tick() with a seeded random pattern of the
 * rewind control -- alternating every tick, short taps, holds of seconds,
 * holds into the start of the history -- and checks, EVERY tick:
 *
 *   - a frame shown backwards is exactly the one before the frame that was
 *     on screen, with the reference picture and the reference sound reversed;
 *   - a frame run forward is exactly the one after it, and leaves the machine
 *     in the reference state (so play after any pattern of taps is the game
 *     it would have been);
 *   - the ring is consistent (fe_rewind_selfcheck: contiguous records, byte
 *     accounting, no snapshot newer than the machine, none left over from an
 *     abandoned future);
 *   - the work done in the tick is bounded: at most `burst` frames run and
 *     at most one restore;
 *   - letting go always gets back to play within a bounded number of ticks.
 *
 * Cases cover interval 1/2/6/10, burst 1/2/3, a ring so small it wraps and
 * overflows, an input log so short it limits the history, barriers (resets)
 * arriving at random, the three ways the state can be read for a snapshot
 * (in place, in place with a dirty-page map, copied first) and snapshots
 * paced by a wall clock instead of a frame count.
 *
 * THE SAVE BARRIER (cases "save_*").  The machine has a cartridge save that
 * is NOT part of its state, and writes it on fixed frames: a save is five
 * writes spread over eleven frames, every 701 frames.  The driver does what
 * fe_host.c does: a frame that wrote is a barrier, and so is every frame
 * until SAVE_QUIET frames have passed without a write.  Checked every tick:
 * no frame at or before the last write of a save is ever shown again, the
 * save bytes are exactly those of the run that never rewound AT THE FRAME ON
 * SCREEN (so the save is never newer than the game, and never half of one),
 * and no re-run frame ever tries to write it.
 *
 * THE SAVE IS PERMANENT (cases "savekeep_*", the product since stage 11; it
 * replaced the save-as-a-span design described next, whose machinery is
 * gone).  The same machine and the same save writes; nothing is a barrier;
 * frames re-run for a rewind have their save writes refused; the frames of
 * a save are reported as FE_RW_IN_BUSY / FE_RW_IN_CHANGE.  Every tick: the
 * save is byte for byte what the last PLAYED frame left (a rewind never
 * changes it).  Every time play resumes at a frame the save has changed
 * after: none of the SAVE_QUIET frames up to it is a frame of a save.
 * Built with -DMUT_NOSNAP the core is never told the quiet length and the
 * second check must fail.
 *
 * (formerly) THE SAVE AS PART OF THE STATE (cases "savekeep_*", stage
 * 9).  The same machine and the same save writes, but the save is a second
 * span of the image with its own written-page map, nothing is a barrier and
 * re-run frames write it.  A write depends on the byte it replaces, so a
 * re-run that starts from a save that is not exactly the snapshot's shows.
 * Every tick: the save is the clean run's save at the frame the MACHINE is
 * at; whenever play resumes, at the frame on screen; and rewinds do go back
 * over save writes (counted).
 *
 * THE MAP THAT LIES (case "lying_map_is_caught").  The dirty-page map drops
 * one mark in fifty; cfg.verify must report it. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fe_rewind.h"

#define WORDS   16384u
#define PIXELS  24u
#define AUDCAP  8u
#define MAXF    60000u

typedef struct
{
   uint32_t w[WORDS];           /* w[0] = frame, w[1] = rng */
} machine;

static machine  M;              /* the machine fe_rewind drives */
static unsigned n_gather, n_restore, n_run;

/* How the state reaches the snapshot (fe_rewind_span):
 *   0  compared where it lies, in the machine's own memory
 *   1  the same, with a page map of what was written since the last
 *      snapshot (pages it calls clean are not even read)
 *   2  copied to a buffer first (what a second CPU would then pack) */
#define PAGE_SHIFT 8
static int      g_mode;
static uint8_t  g_dirty[(WORDS * 4) >> PAGE_SHIFT];
static machine  g_copy;
static fe_rewind_span g_span;
static uint64_t g_clock;        /* the fake wall clock (pacing cases) */

/* The cartridge save: outside the state, written by the machine itself. */
#define SAVE_BYTES 64u
#define SAVE_QUIET 8u           /* > the longest gap inside one save (4) */
static int      g_save_mode;    /* the machine M writes g_save */
static uint8_t  g_save[SAVE_BYTES];
static uint32_t g_save_gen;     /* writes made (backup_write_gen) */
static int      g_save_ro;      /* a re-run: writes are refused and counted */
static unsigned g_save_ro_hits;
static int      g_save_span;    /* ...and it is the second span of the image */
static uint8_t  g_save_dirty[SAVE_BYTES >> 3];   /* 8-byte pages */
static int      g_lie;          /* the dirty map drops one mark in fifty */
static unsigned g_lies;

static int is_save_frame(uint32_t f)
{
   uint32_t k = f % 701u;
   return f >= 701u && (k == 0 || k == 1 || k == 4 || k == 9 || k == 10);
}

static void save_write(uint8_t *save, uint32_t f)
{
   save[f % SAVE_BYTES] = (uint8_t)(save[f % SAVE_BYTES] * 5u + f);
   save[(f * 7u) % SAVE_BYTES] ^= (uint8_t)(f >> 3);
   if (save == g_save)
   {
      g_save_dirty[(f % SAVE_BYTES) >> 3] = 1;
      g_save_dirty[((f * 7u) % SAVE_BYTES) >> 3] = 1;
   }
}

static uint32_t input_of(uint32_t frame)
{
   uint32_t h = (frame / 7u) * 2654435761u;
   h ^= h >> 13;
   return h & 0x3FF;
}

static uint32_t hash_words(const uint32_t *p, uint32_t n)
{
   uint32_t h = 2166136261u, i;
   for (i = 0; i < n; i++)
      h = (h ^ p[i]) * 16777619u;
   return h;
}

static void machine_reset(machine *m)
{
   uint32_t i;
   for (i = 0; i < WORDS; i++)
      m->w[i] = i * 2246822519u;
   m->w[0] = 0;
   m->w[1] = 12345;
}

static uint32_t rnd(machine *m)
{
   m->w[1] = m->w[1] * 1664525u + 1013904223u;
   return m->w[1] >> 8;
}

/* One frame.  pix/aud as fe_rewind's run callback wants them. */
static void machine_frame(machine *m, uint32_t input, uint16_t *pix,
                          int16_t *aud, uint32_t *aud_n)
{
   uint32_t i, n, f = ++m->w[0];
   n = (input & 0x200) && (f % 97u) == 0 ? 6000u : 4u + (input & 7u);
   for (i = 0; i < n; i++)
   {
      uint32_t at = 2 + rnd(m) % (WORDS - 2);
      m->w[at] = m->w[at] * 31u + input + f;
      if (m == &M)
      {
         if (g_lie && (at % 50u) == 7u)
            g_lies++;                      /* written, not marked */
         else
            g_dirty[(at * 4) >> PAGE_SHIFT] = 1;
      }
   }
   if (m == &M && g_save_mode && is_save_frame(f))
   {
      if (g_save_ro)
         g_save_ro_hits++;
      else
      {
         save_write(g_save, f);
         g_save_gen++;
      }
   }
   if (m == &M)
      g_dirty[0] = 1;                      /* the frame counter and the rng */
   if (pix)
   {
      uint32_t h = hash_words(m->w, 64);
      for (i = 0; i < PIXELS; i++)
         pix[i] = (uint16_t)(h >> (i & 15)) ^ (uint16_t)(f * (i + 1));
      n = 3 + (f & 3);                     /* a varying sample count */
      for (i = 0; i < n; i++)
      {
         aud[i * 2]     = (int16_t)(f * 7 + i);
         aud[i * 2 + 1] = (int16_t)(h + i * 3);
      }
      *aud_n = n;
   }
}

static int cb_gather(void)
{
   n_gather++;
   if (g_mode == 2)
      memcpy(&g_copy, &M, sizeof(M));
   return 0;
}

static void cb_done(void)
{
   memset(g_dirty, 0, sizeof(g_dirty));
   memset(g_save_dirty, 0, sizeof(g_save_dirty));
}

static uint64_t cb_clock(void)
{
   return g_clock;
}

static int cb_restore(const void *buf, uint32_t bytes)
{
   n_restore++;
   memcpy(&M, buf, bytes);
   return 0;
}

static int g_taint, g_held;
static uint32_t g_tainted;
static void cb_run(uint32_t input, uint16_t *pix, int16_t *aud, uint32_t *aud_n)
{
   n_run++;
   g_save_ro = 1;               /* fe_host.c: gba_rewind_backup_ro */
   machine_frame(&M, input, pix, aud, aud_n);
   g_save_ro = 0;
   if (g_taint && g_held && !pix)
   {
      /* What a platform does that skips its sound synthesis in the frames a
       * held rewind re-runs without a picture: the machine is left
       * different in a part no picture depends on, and says so. */
      M.w[WORDS - 1] ^= 0x5a5a5a5au;
      g_dirty[((WORDS - 1) * 4) >> PAGE_SHIFT] = 1;
      g_tainted++;
#ifndef TEST_NO_TAINT
      fe_rewind_taint();
#endif
   }
}

/* ---- the truth: a run that never rewinds -------------------------------- */
static uint32_t ref_state[MAXF + 1];
static uint16_t ref_pix[MAXF + 1][PIXELS];
static int16_t  ref_aud[MAXF + 1][AUDCAP * 2];
static uint8_t  ref_aud_n[MAXF + 1];
static uint32_t ref_save[MAXF + 1];     /* hash of the save after frame f */

static uint32_t save_hash(const uint8_t *s)
{
   uint32_t h = 2166136261u, i;
   for (i = 0; i < SAVE_BYTES; i++)
      h = (h ^ s[i]) * 16777619u;
   return h;
}

static void build_reference(void)
{
   static machine r;
   static uint8_t r_save[SAVE_BYTES];
   uint32_t f;
   machine_reset(&r);
   ref_state[0] = hash_words(r.w, WORDS);
   ref_save[0] = save_hash(r_save);
   for (f = 1; f <= MAXF; f++)
   {
      uint32_t n = 0;
      machine_frame(&r, input_of(f), ref_pix[f], ref_aud[f], &n);
      ref_aud_n[f] = (uint8_t)n;
      ref_state[f] = hash_words(r.w, WORDS);
      if (is_save_frame(f))
         save_write(r_save, f);
      ref_save[f] = save_hash(r_save);
   }
}

/* ---- one case ------------------------------------------------------------ */
static uint32_t prng_s;
static uint32_t prng(void)
{
   prng_s ^= prng_s << 13;
   prng_s ^= prng_s >> 17;
   prng_s ^= prng_s << 5;
   return prng_s;
}

static int fails;
#define CHECK(c, ...) do { if (!(c)) { if (fails - before < 4) { \
   printf("  FAIL %s tick %u: ", name, tick); printf(__VA_ARGS__); printf("\n"); } \
   fails++; } } while (0)

typedef struct
{
   const char *name;
   uint32_t seed, interval, burst, ring, max_frames, ticks;
   int barriers;                /* resets at random */
   int pattern;                 /* 0 random mix, 1 alternate every tick,
                                 * 2 hold to the limit then tap */
   int mode;                    /* g_mode; +10 = the map lies and verify
                                 * must say so; +20 = verify on, must be
                                 * silent */
   uint32_t pace_us;            /* wall-clock pacing; the fake clock then
                                 * advances 0.2-20 ms a tick, so snapshots
                                 * fall 1 to 8 intervals apart */
} test_case;

static void run_case(const test_case *tc)
{
   const char *name = tc->name;
   fe_rewind_config c;
   const fe_rewind_stats *st;
   void *mem, *ring_a = NULL, *ring_b = NULL;
   size_t need;
   uint32_t tick = 0, now = 0, left = 0, shown = 0, played = 0, held_ticks = 0;
   uint32_t released_run = 0, transitions = 0, barriers = 0, limit_holds = 0;
   uint32_t quiet = 0, save_floor = 0, saves = 0, stopped_at_save = 0;
   uint32_t stride_shows = 0, stride_exact = 0;
   uint32_t keep_lc = 0, keep_hash = 0, snaps_seen = 0;
   uint32_t mach_unvetted = 0;
   int held = 1, before = fails;   /* the first transition is a release */

   memset(&c, 0, sizeof(c));
   g_mode = tc->mode % 10;
   g_lie = tc->mode / 10 == 1;
   g_lies = 0;
   c.verify = tc->mode / 10 >= 1;
   g_save_mode = tc->barriers == 2 || tc->barriers == 3;
   g_save_span = tc->barriers == 3;
   memset(g_save_dirty, 1, sizeof(g_save_dirty));
   memset(g_save, 0, sizeof(g_save));
   g_save_gen = 0;
   g_save_ro = 0;
   g_save_ro_hits = 0;
   g_span.bytes = sizeof(machine);
   g_span.cur = g_mode == 2 ? (const void *)&g_copy : (const void *)&M;
   g_span.dirty = g_mode == 1 ? g_dirty : NULL;
   g_span.page_shift = PAGE_SHIFT;
   memset(g_dirty, 1, sizeof(g_dirty));
   g_clock = 1000;
   c.spans        = &g_span;
   c.nspans       = 1;
   if (0)
   {
   }
   c.pace_us      = tc->pace_us;
   c.snapshot_done = cb_done;
   c.now_us       = tc->pace_us ? cb_clock : NULL;
   c.ring_bytes   = tc->ring;
   c.interval     = tc->interval;
   c.max_frames   = tc->max_frames;
   c.frame_pixels = PIXELS;
   c.audio_cap    = AUDCAP;
   c.burst        = tc->burst;
   c.gather       = cb_gather;
   c.restore      = cb_restore;
   c.run          = cb_run;
   if (tc->seed % 2 == 0)
   {
      /* every other case: the ring in two pieces of memory, split at an
       * odd place (fe_rewind_config.ring_split) */
      c.ring_split = (tc->ring / 3) | 1u;
      ring_a = malloc(c.ring_split);
      ring_b = malloc(tc->ring - c.ring_split);
      c.ring_mem = ring_a;
      c.ring_mem2 = ring_b;
   }
   need = fe_rewind_mem_bytes(&c);
   mem = malloc(need);
   keep_lc = 0;
   keep_hash = save_hash(g_save);
   snaps_seen = 0;
   if (!mem || fe_rewind_init(&c, mem, need) != 0)
   {
      free(ring_a);
      free(ring_b);
      printf("  FAIL %s: init\n", name);
      fails++;
      free(mem);
      return;
   }
   /* (two cases with a target for the history's length: snapshots further
    * apart while they are large) */
#ifndef MUT_NOSNAP
   fe_rewind_set_noland(g_save_span ? SAVE_QUIET : 0);
#endif
   if (tc->seed == 38 || tc->seed == 40)
      fe_rewind_set_target(tc->seed == 38 ? 3 : 1000, 24);
   machine_reset(&M);
   fe_rewind_reset(0);
   prng_s = tc->seed * 2654435761u + 1;
   n_gather = n_restore = n_run = 0;

   for (tick = 1; tick <= tc->ticks && now + 2 < MAXF; tick++)
   {
      fe_rewind_shown s;
      unsigned run0 = n_run, res0 = n_restore;
      int r;

      /* (the lying map: the case is over once verify has caught it -- what
       * a hidden write does to the history afterwards is the bug itself) */
      if (g_lie && fe_rewind_get_stats()->verify_bad_words)
         break;
      /* the control */
      if (!left)
      {
         uint32_t k = prng() % 100;
         held = !held;
         transitions++;
         if (tc->pattern == 1)   /* play a while, then toggle every tick,
                                  * in bursts that lean back or forward */
            left = transitions == 1 ? 900
                 : (transitions / 400) % 3 == 1 ? (held ? 1 : 2)
                 : (transitions / 400) % 3 == 2 ? (held ? 2 : 1) : 1;
         else if (tc->pattern == 2)
            left = held ? (transitions == 2 ? 4000 : 1 + prng() % 3)
                        : (transitions == 1 ? 600 : 1 + prng() % 2);
         else if (tc->pattern == 3 && k < 35)
            left = 1;
         else if (k < 35)
            left = 1;                                   /* tap storm */
         else if (k < 70)
            left = 1 + prng() % 10;
         else if (k < 97)
            left = 1 + prng() % (held ? 120 : 400);     /* seconds */
         else
            left = held ? 3000 : 1 + prng() % 400;      /* into the limit */
      }
      left--;

      if (tc->pace_us)
         g_clock += 200 + prng() % 20000;
      if (tc->pattern == 3)
      {
         /* going back up to four frames a tick */
         uint32_t n = 0, want = 1 + prng() % 4;
         r = fe_rewind_tick_step(held, want, &s, &n);
         CHECK(r != FE_RW_SHOW || (n >= 1 && n <= want &&
                                   s.frame + n == now),
               "stepped %u to %u from %u", (unsigned)n, (unsigned)s.frame,
               (unsigned)now);
         if (r == FE_RW_SHOW)
            now = s.frame + 1;         /* the checks below expect one step */
      }
      else if (tc->pattern == 4)
      {
         /* A picture every second or third frame, and not on every tick:
          * the even cadence of a game too heavy to be re-run as fast as it
          * is shown.  The picture handed out must be a reference picture
          * of a frame in the stretch just gone over, and nearly always the
          * one of the frame landed on. */
         uint32_t n = 0, st = 2 + tc->seed % 2;
         uint32_t step = prng() % 3 == 0 ? 0 : st;
         fe_rewind_set_stride(st);
         g_taint = 1;
         g_held = held;
         r = fe_rewind_tick_step(held, step, &s, &n);
         g_held = 0;
         CHECK(r != FE_RW_SHOW || (step && n >= 1 && n <= st &&
                                   s.frame + n == now),
               "stepped %u to %u from %u", (unsigned)n, (unsigned)s.frame,
               (unsigned)now);
         if (r == FE_RW_SHOW)
         {
            stride_shows++;
            if (s.pix)
            {
               uint32_t k, hit = 0;
               for (k = 0; k < n; k++)
                  if (!memcmp(s.pix, ref_pix[s.frame + k], sizeof(ref_pix[0])))
                     hit = 1;
               CHECK(hit, "picture at frame %u is not one of the %u gone over",
                     (unsigned)s.frame, (unsigned)n);
               if (!memcmp(s.pix, ref_pix[s.frame], sizeof(ref_pix[0])))
                  stride_exact++;
            }
            now = s.frame + 1;         /* the checks below expect one step */
         }
      }
      else
      r = fe_rewind_tick(held, &s);
      {
         /* a release on a frame play may not continue from moved `now` */
         const fe_rewind_stats *q = fe_rewind_get_stats();
         if (q->snaps_back + q->snaps_fwd != snaps_seen)
         {
            snaps_seen = q->snaps_back + q->snaps_fwd;
            CHECK(g_save_span && !held, "the screen was moved off a save "
                  "outside a release");
            now = fe_rewind_frame_now();
         }
      }
#ifndef MUT_NOSNAP
      if (g_save_span)
      {
         /* THE MACHINE'S OWN FRAME (fe_rewind_mach_noland).  Whoever ends
          * the history in the middle of a rewind without landing it
          * (fe_host.c: a sleep that ran out of time) asks whether play may
          * go on from where the MACHINE stands.  After every tick: it says
          * "no" only for a frame inside a save that a later save has
          * overtaken, it does say "no" for every such frame the machine is
          * left on, and it never says "no" when the tick said PLAY. */
         uint32_t m = fe_rewind_frame_mach(), g;
         int said = fe_rewind_mach_noland(), busy = 0;
         for (g = m; g + SAVE_QUIET > m && g > 0; g--)
            busy |= is_save_frame(g);
         CHECK(!said || (busy && m < keep_lc && m != fe_rewind_frame_top()),
               "machine at %u called unfit to go on from (save busy near: "
               "%d, changed after: %u)", (unsigned)m, busy, (unsigned)keep_lc);
         if (m != fe_rewind_frame_top() && m > fe_rewind_frame_oldest() + 1 &&
             is_save_frame(m) && m < keep_lc && fe_rewind_active())
         {
            CHECK(said, "machine at %u, inside a save overtaken at %u, "
                  "called fit to go on from", (unsigned)m, (unsigned)keep_lc);
            mach_unvetted++;
         }
         CHECK(r != FE_RW_PLAY || !said, "PLAY with the machine at %u, "
               "inside an overtaken save", (unsigned)m);
      }
#endif
      if (r == FE_RW_PLAY)
      {
         uint16_t *pix = NULL;
         int16_t *aud = NULL;
         uint32_t n = 0, in = input_of(now + 1);
         int slot;
         /* (with no history at all the control does nothing: PLAY while held) */
         released_run = 0;
         CHECK(fe_rewind_frame_now() == now, "play at %u, screen had %u",
               (unsigned)fe_rewind_frame_now(), (unsigned)now);
         CHECK(M.w[0] == now, "machine at %u, play resumes at %u",
               (unsigned)M.w[0], (unsigned)now);
         if (g_save_span)
         {
            /* The save is permanent: a frame that writes it is recorded
             * like any other, and reported as busy / changed. */
            uint32_t gen0 = g_save_gen, fl = 0, g;
            if (now < keep_lc)
               for (g = now; g + SAVE_QUIET > now && g > 0; g--)
                  CHECK(!is_save_frame(g), "play resumes at %u, inside the "
                        "save around frame %u (changed after: %u)",
                        (unsigned)now, (unsigned)g, (unsigned)keep_lc);
            slot = fe_rewind_play_slot(&pix, &aud);
            machine_frame(&M, in, slot ? pix : NULL, slot ? aud : NULL, &n);
            if (g_save_gen != gen0 && is_save_frame(now + 1) &&
                (now + 1) % 701u == 0)
               saves++;
            if (is_save_frame(now + 1))
               fl |= FE_RW_IN_BUSY;
            if (keep_lc > now)
               keep_lc = now;
            if (g_save_gen != gen0)
            {
               fl |= FE_RW_IN_CHANGE;
               keep_lc = now + 1;
            }
            keep_hash = save_hash(g_save);
            fe_rewind_frame(in | fl, slot ? n : 0);
            now++;
         }
         else if (g_save_mode)
         {
            /* What fe_host.c does around a normal frame (rw_after_frame):
             * a frame that wrote the save is not recorded and ends the
             * history, and so does every frame until SAVE_QUIET have passed
             * without a write. */
            uint16_t scratch_pix[PIXELS];
            int16_t scratch_aud[AUDCAP * 2];
            uint32_t gen0 = g_save_gen;
            slot = fe_rewind_play_slot(&pix, &aud);
            machine_frame(&M, in, slot ? pix : scratch_pix,
                          slot ? aud : scratch_aud, &n);
            if (g_save_gen != gen0)
            {
               if (!quiet)
                  saves++;
               quiet = SAVE_QUIET;
               now++;
               fe_rewind_reset(now);
               save_floor = now;
            }
            else if (quiet)
            {
               quiet--;
               now++;
               fe_rewind_reset(now);
               save_floor = now;
            }
            else
            {
               fe_rewind_frame(in, slot ? n : 0);
               now++;
            }
         }
         else if (tc->barriers && prng() % 400 == 0)
         {
            /* a frame that is not recorded: a barrier */
            machine_frame(&M, in, NULL, NULL, NULL);
            now++;
            fe_rewind_reset(now);
            barriers++;
         }
         else
         {
            uint16_t scratch_pix[PIXELS];
            int16_t scratch_aud[AUDCAP * 2];
            slot = fe_rewind_play_slot(&pix, &aud);
            machine_frame(&M, in, slot ? pix : scratch_pix,
                          slot ? aud : scratch_aud, &n);
            /* paced cases: a third of the frames say "no time to spare" */
            if (tc->pace_us)
               fe_rewind_hint_busy(prng() % 3 == 0);
            fe_rewind_frame(in, slot ? n : 0);
            now++;
         }
         played++;
         CHECK(hash_words(M.w, WORDS) == ref_state[now],
               "state after frame %u is not the reference", (unsigned)now);
      }
      else if (r == FE_RW_SHOW)
      {
         uint32_t i, n = ref_aud_n[s.frame];
         int same = 1;
         CHECK(held, "showed a frame with the control up");
         CHECK(s.frame + 1 == now, "showed %u after %u", (unsigned)s.frame,
               (unsigned)now);
         CHECK(!g_save_mode || s.frame > save_floor,
               "showed frame %u, at or before the save that ended at %u",
               (unsigned)s.frame, (unsigned)save_floor);
         CHECK(tc->pattern == 4 ||
               memcmp(s.pix, ref_pix[s.frame], sizeof(ref_pix[0])) == 0,
               "picture of frame %u differs", (unsigned)s.frame);
         if (tc->pattern == 4)
            n = 0;                     /* (frames without a picture keep no
                                        * sound either) */
         CHECK(tc->pattern == 4 || s.aud_n == n,
               "frame %u: %u samples, reference %u",
               (unsigned)s.frame, (unsigned)s.aud_n, (unsigned)n);
         for (i = 0; i < n && i < s.aud_n; i++)
            if (s.aud[i * 2] != ref_aud[s.frame][(n - 1 - i) * 2] ||
                s.aud[i * 2 + 1] != ref_aud[s.frame][(n - 1 - i) * 2 + 1])
               same = 0;
         CHECK(same, "sound of frame %u is not the reference reversed",
               (unsigned)s.frame);
         now = s.frame;
         shown++;
         released_run = 0;
      }
      else
      {
         CHECK(fe_rewind_frame_now() == now, "hold moved the screen");
         if (held)
         {
            if (fe_rewind_frame_now() <= fe_rewind_frame_oldest())
               limit_holds++;
            /* stopped with the save right behind: the "save point" stop */
            if (g_save_mode && save_floor && fe_rewind_at_limit() &&
                fe_rewind_frame_oldest() <= save_floor + 2)
               stopped_at_save++;
         }
         else
         {
            /* released: play must come back, and soon */
            released_run++;
            CHECK(released_run <= 2 * (tc->pace_us ? 9 : 1) * tc->interval + 8 +
                                  (g_save_span ? 3 * SAVE_QUIET + 16 : 0),
                  "still not playing %u ticks after release",
                  (unsigned)released_run);
         }
      }
      if (held)
         held_ticks++;

      CHECK(n_run - run0 <= (r == FE_RW_PLAY ? 0u : tc->burst),
            "%u frames run in one tick (burst %u)", n_run - run0,
            (unsigned)tc->burst);
      CHECK(n_restore - res0 <= 1, "%u restores in one tick", n_restore - res0);
      /* (selfcheck 9 = "an anomaly was counted", which the lying map is) */
      CHECK(fe_rewind_selfcheck() == (g_lie && fe_rewind_get_stats()->anomalies ? 9 : 0),
            "selfcheck %d", fe_rewind_selfcheck());
      CHECK(fe_rewind_frame_now() == now, "now %u, expected %u",
            (unsigned)fe_rewind_frame_now(), (unsigned)now);
      if (g_save_span)
      {
         /* the save is what the last played frame left, whatever the
          * rewind is doing */
         CHECK(save_hash(g_save) == keep_hash,
               "the save changed without a played frame (machine at %u, "
               "screen %u)", (unsigned)M.w[0], (unsigned)now);
         if (r == FE_RW_SHOW && ref_save[s.frame] != ref_save[s.frame + 1])
            stopped_at_save++;         /* (here: went back over a write) */
      }
      else if (g_save_mode)
      {
         CHECK(save_hash(g_save) == ref_save[now],
               "the save is not the clean run's save at frame %u",
               (unsigned)now);
         CHECK(g_save_ro_hits == 0, "a re-run frame wrote the save (%u)",
               g_save_ro_hits);
         CHECK(now >= save_floor, "the screen is before the save barrier");
      }
      if (fails - before > 8)
         break;
   }

   st = fe_rewind_get_stats();
   if (g_lie)
   {
      /* The point of this case: a map that hides writes IS noticed. */
      CHECK(g_lies > 0 && st->verify_bad_words > 0 && st->anomalies > 0,
            "the map lied %u times and verify saw %u words", g_lies,
            (unsigned)st->verify_bad_words);
      printf("%-22s       the map hid %u writes; verify reported %u words "
             "in %u of %u snapshots\n", "", g_lies,
             (unsigned)st->verify_bad_words, (unsigned)st->verify_bad_snaps,
             (unsigned)st->verify_runs);
   }
   else
   {
      CHECK(st->anomalies == 0, "%u anomalies", (unsigned)st->anomalies);
      CHECK(st->verify_bad_words == 0, "verify: %u words hidden by the map",
            (unsigned)st->verify_bad_words);
      CHECK(!c.verify || st->verify_runs > 0, "verify never ran");
   }
   if (g_save_span)
   {
      CHECK(saves >= 1u, "only %u saves happened", (unsigned)saves);
      CHECK(stopped_at_save > 0, "no rewind ever went back over a save write");
      CHECK(g_save_ro_hits > 0, "no re-run frame ever met a save write");
      printf("%-22s       %u saves; rewinds went back over %u save writes; "
             "%u writes refused in re-runs; releases inside a save moved "
             "back %u / forward %u (furthest %u frames)\n",
             "", (unsigned)saves, (unsigned)stopped_at_save, g_save_ro_hits,
             (unsigned)st->snaps_back, (unsigned)st->snaps_fwd,
             (unsigned)st->snap_max);
#ifndef MUT_NOSNAP
      if (tc->seed == 43 || tc->seed == 44)
      {
         CHECK(st->snaps_back + st->snaps_fwd > 0,
               "no release ever fell inside a save");
         CHECK(mach_unvetted > 0, "the machine never stood inside an "
               "overtaken save between two ticks");
      }
      printf("%-22s       ticks that left the machine inside an overtaken "
             "save: %u\n", "", (unsigned)mach_unvetted);
#endif
   }
   else if (g_save_mode)
   {
      CHECK(saves >= (tc->pattern ? 1u : 3u), "only %u saves happened",
            (unsigned)saves);
      CHECK(stopped_at_save > 0, "no rewind ever ran into a save");
      printf("%-22s       %u saves; held against a save point for %u ticks; "
             "writes refused in re-runs: %u\n", "", (unsigned)saves,
             (unsigned)stopped_at_save, g_save_ro_hits);
   }
   CHECK(st->tick_frames_max <= tc->burst, "tick_frames_max %u",
         (unsigned)st->tick_frames_max);
   CHECK(st->tick_restores_max <= 1, "tick_restores_max %u",
         (unsigned)st->tick_restores_max);
   CHECK(shown > 0 && played > 0, "nothing exercised");
   if (tc->pattern == 4)
   {
      CHECK(stride_exact * 10 >= stride_shows * 6,
            "only %u of %u pictures were the frame landed on",
            (unsigned)stride_exact, (unsigned)stride_shows);
      printf("%-22s       stride: %u pictures, %u exactly the frame landed "
             "on, %u frames run inexactly\n", "", (unsigned)stride_shows,
             (unsigned)stride_exact, (unsigned)g_tainted);
      CHECK(g_tainted > 100, "the inexact re-runs were not exercised");
      g_taint = 0;
      g_tainted = 0;
      fe_rewind_set_stride(1);
   }
   printf("%-22s %s  ticks=%u presses=%u shown=%u played=%u frame=%u "
          "stall=%u limit=%u catchup=%u(max run %u) restores=%u "
          "evicted=%u overflow=%u barriers=%u\n",
          name, fails == before ? "ok  " : "FAIL", tick - 1, transitions / 2,
          shown, played, now, (unsigned)st->ticks_stall,
          (unsigned)st->ticks_limit, (unsigned)st->ticks_catchup,
          (unsigned)st->catchup_run_max, (unsigned)st->restores,
          (unsigned)st->evicted, (unsigned)st->overflow_resets, barriers);
   (void)limit_holds;
   (void)mach_unvetted;
   (void)held_ticks;
   free(mem);
   free(ring_a);
   free(ring_b);
}

int main(void)
{
   static const test_case cases[] = {
      /* name                  seed iv burst ring        log   ticks  barr pat */
      { "mix_6_2",               1,  6, 2,  256 * 1024, 4096, 40000, 0, 0, 0, 0 },
      { "mix_6_2_seed2",         2,  6, 2,  256 * 1024, 4096, 40000, 0, 0, 0, 0 },
      { "mix_6_1",               3,  6, 1,  256 * 1024, 4096, 40000, 0, 0, 0, 0 },
      { "mix_6_3",               4,  6, 3,  256 * 1024, 4096, 30000, 0, 0, 0, 0 },
      { "mix_1_2",               5,  1, 2,  256 * 1024, 4096, 30000, 0, 0, 0, 0 },
      { "mix_2_1",               6,  2, 1,  256 * 1024, 4096, 30000, 0, 0, 0, 0 },
      { "mix_10_2",              7, 10, 2,  256 * 1024, 4096, 40000, 0, 0, 0, 0 },
      { "small_ring_wraps",      8,  6, 2,    8 * 1024, 4096, 40000, 0, 0, 0, 0 },
      { "tiny_ring_overflows",   9,  6, 2,    2 * 1024, 4096, 30000, 0, 0, 0, 0 },
      { "short_input_log",      10,  6, 2,  256 * 1024,   64, 30000, 0, 0, 0, 0 },
      { "barriers",             11,  6, 2,  256 * 1024, 4096, 40000, 1, 0, 0, 0 },
      { "barriers_small_ring",  12, 10, 1,   16 * 1024, 4096, 40000, 1, 0, 0, 0 },
      { "alternate_every_tick", 13,  6, 2,  256 * 1024, 4096, 20000, 0, 1, 0, 0 },
      { "alternate_burst1",     14,  6, 1,  256 * 1024, 4096, 20000, 0, 1, 0, 0 },
      { "alternate_interval1",  15,  1, 1,  256 * 1024, 4096, 20000, 0, 1, 0, 0 },
      { "tap_at_the_limit",     16,  6, 2,  256 * 1024, 4096, 12000, 0, 2, 0, 0 },
      { "tap_at_limit_barrier", 17,  6, 2,  256 * 1024, 4096, 12000, 1, 2, 0, 0 },
      /* the same storms through the other two ways of reading the state */
      { "dirtymap_mix",         18,  6, 2,  256 * 1024, 4096, 40000, 0, 0, 1, 0 },
      { "dirtymap_barriers",    19,  6, 1,   16 * 1024, 4096, 40000, 1, 0, 1, 0 },
      { "dirtymap_alternate",   20,  6, 2,  256 * 1024, 4096, 20000, 0, 1, 1, 0 },
      { "copy_mix",             21,  6, 2,  256 * 1024, 4096, 30000, 0, 0, 2, 0 },
      /* snapshots paced by a wall clock: 1 to 8 intervals apart */
      { "paced_mix",            22,  6, 2,  256 * 1024, 4096, 40000, 0, 0, 0, 30000 },
      { "paced_mix_burst1",     23,  6, 1,  256 * 1024, 4096, 40000, 0, 0, 0, 60000 },
      { "paced_dirty_barriers", 24,  6, 2,   64 * 1024, 4096, 40000, 1, 0, 1, 45000 },
      { "paced_alternate",      25,  6, 2,  256 * 1024, 4096, 20000, 0, 1, 0, 30000 },
      { "paced_tap_at_limit",   26,  6, 2,  256 * 1024, 4096, 12000, 0, 2, 2, 90000 },
      { "paced_small_ring",     27, 10, 2,   16 * 1024, 4096, 40000, 0, 0, 1, 50000 },
      /* the save barrier (barr = 2: the machine writes a save every 701
       * frames, five writes over eleven frames) */
      { "save_mix",             28,  6, 2,  256 * 1024, 4096, 60000, 2, 0, 0, 0 },
      { "save_mix_burst1",      29,  6, 1,  256 * 1024, 4096, 60000, 2, 0, 0, 0 },
      { "save_alternate",       30,  6, 1,  256 * 1024, 4096, 30000, 2, 1, 1, 0 },
      { "save_paced_dirty",     31,  6, 2,   64 * 1024, 4096, 60000, 2, 0, 1, 45000 },
      { "save_interval1",       32,  1, 2,  256 * 1024, 4096, 40000, 2, 0, 0, 0 },
      /* the dirty map, checked on every snapshot (verify), and a map that
       * drops marks, which verify must catch */
      { "dirtymap_verified",    33,  6, 2,  256 * 1024, 4096, 40000, 1, 0, 21, 0 },
      { "paced_dirty_verified", 34,  6, 2,   64 * 1024, 4096, 40000, 0, 0, 21, 45000 },
      { "lying_map_is_caught",  35,  6, 2,  256 * 1024, 4096, 20000, 0, 0, 11, 0 },
      /* up to four frames back a tick (pattern 3) */
      { "step4_mix",            36,  6, 2,  256 * 1024, 4096, 40000, 0, 3, 0, 0 },
      { "step4_burst4",         37,  6, 4,  256 * 1024, 4096, 40000, 0, 3, 1, 0 },
      { "step4_save_paced",     38,  6, 3,   64 * 1024, 4096, 40000, 2, 3, 1, 45000 },
      /* a picture every 2nd / 3rd frame, some ticks without one (pattern 4) */
      { "stride2_mix",          40,  6, 2,  256 * 1024, 4096, 40000, 0, 4, 0, 0 },
      { "stride3_burst3",       41,  6, 3,  256 * 1024, 4096, 40000, 0, 4, 1, 0 },
      { "stride2_save_paced",   42,  6, 2,   64 * 1024, 4096, 40000, 2, 4, 1, 45000 },
      /* the save as a span of the image (barr = 3): no barrier, re-runs
       * write it, rewinds cross the writes */
      { "savekeep_mix",         43,  6, 2,  256 * 1024, 4096, 60000, 3, 0, 0, 0 },
      { "savekeep_burst1",      44,  6, 1,  256 * 1024, 4096, 60000, 3, 0, 0, 0 },
      { "savekeep_alternate",   45,  6, 1,  256 * 1024, 4096, 30000, 3, 1, 1, 0 },
      { "savekeep_paced_dirty", 46,  6, 2,   64 * 1024, 4096, 60000, 3, 0, 1, 45000 },
      { "savekeep_interval1",   47,  1, 2,  256 * 1024, 4096, 40000, 3, 0, 0, 0 },
      { "savekeep_small_ring",  48,  6, 2,    8 * 1024, 4096, 40000, 3, 0, 1, 0 },
      { "savekeep_verified",    49,  6, 2,  256 * 1024, 4096, 40000, 3, 0, 21, 0 },
      { "savekeep_step4",       50,  6, 3,   64 * 1024, 4096, 40000, 3, 3, 1, 45000 },
      { "savekeep_stride2",     51,  6, 2,   64 * 1024, 4096, 40000, 3, 4, 1, 45000 },
   };
   unsigned i;

   build_reference();
   for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
      run_case(&cases[i]);
   printf("test_fe_rewind: %s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
   return fails ? 1 : 0;
}
