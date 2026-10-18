/* rewind_vita.c -- hold a button and the game runs backwards.
 * rewind_vita.h says what it does and why it is built this way.
 *
 * THREADS.  Two, at most:
 *
 *   frame thread   everything but rw_commit(): taking a state, the whole
 *                  of rewinding, clearing, the settings
 *   worker thread  rewind_worker() -> rw_commit(): one state's XOR and
 *                  squeeze, and its place in the ring
 *
 * They meet in one slot, g_pend.  The frame thread fills R.pend (a whole
 * state) while the slot is PEND_NONE and sets it PEND_READY; whoever turns
 * READY into PEND_BUSY (a compare-and-swap) commits it and sets NONE.  That
 * is normally the worker.  While the slot is not NONE its taker owns the
 * ring, the index and both state buffers, and the frame thread touches none
 * of them: it skips a capture that finds the slot full, and before it
 * rewinds, clears or frees anything it gets the slot to NONE (rw_wait_idle)
 * -- by taking a READY job itself, so a worker that is busy elsewhere (a
 * slow write to the card) never makes the player wait for a rewind, or by
 * waiting out the worker's commit, which is a millisecond.  Without a
 * worker (rewind_set_async(NULL)) rw_commit() runs inside rewind_capture.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fe_host.h"
#include "fe_evt.h"

#include "plat.h"
#include "app_vita.h"
#include "cheats_vita.h"
#include "osd_psp.h"
#include "video_vita.h"
#include "rewind_vita.h"

/* A run of fewer than this many equal bytes inside a changed stretch is
 * cheaper said as literal zeros than as a new [skip][count] pair. */
#define RW_GAP 3

/* How fast the game runs backwards, in emulated frames per display refresh,
 * by how long the control has been held (in refreshes): 2x for the first
 * second -- slow enough to stop on a moment --, then 4x, 8x, and 16x for
 * crossing minutes. */
static unsigned rw_speed(unsigned held)
{
   return held < 60 ? 2 : held < 180 ? 4 : held < 360 ? 8 : 16;
}

/* One step: the difference between a state and the one before it. */
typedef struct rw_ent
{
   uint32_t off;        /* in the ring                                       */
   uint32_t len;        /* the state's difference                            */
   uint32_t bat_len;    /* the battery's, right after it (0 = unchanged)     */
   uint32_t alloc;      /* ring bytes taken (>= 1, so no two share a place)  */
   uint16_t frames;     /* emulated frames from the state before to this one */
} rw_ent;

#define RW_IDX_CAP 16384u    /* steps: 5 minutes at one every 2 frames = 9000 */

enum { PEND_NONE = 0, PEND_READY, PEND_BUSY };
static volatile int g_pend;

/* READY -> BUSY, by exactly one thread.  1 = the job is the caller's. */
static int pend_take(void)
{
   int want = PEND_READY;
   return __atomic_compare_exchange_n(&g_pend, &want, PEND_BUSY, 0,
                                      __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

static struct
{
   /* the settings */
   int seconds, budget_mb;
   int game;                  /* between rewind_game_begin and _end          */
   int on;                    /* the buffers exist                           */
   void (*wake)(void);

   /* the ring and its index (the worker's while g_pend == PEND_READY) */
   uint8_t *ring;
   size_t   ring_cap, wpos, used;
   rw_ent  *idx;
   unsigned head, count;      /* oldest step; steps held                     */
   unsigned frames;           /* emulated frames the steps span              */

   /* the newest state, whole; the one being handed over; the encoder's
    * scratch */
   uint8_t *cur, *pend, *enc;
   size_t   state_len, state_cap, enc_cap;
   int      have_cur;         /* (the committer's)                           */
   int      taken;            /* a state has been taken since the history
                               * was last forgotten (the frame thread's)     */
   unsigned pend_frames;
   int      pend_bat;

   /* the battery (GBA): the cartridge's own memory, its image as of `cur`,
    * and the one being handed over */
   uint8_t *bat_live, *bat_cur, *bat_pend;
   size_t   bat_len;
   uint32_t bat_gen;          /* the game's write count when last copied     */
   int      bat_dirty;        /* bat_cur was stepped back: put it in the cart */

   /* capture */
   unsigned since;            /* emulated frames since the last state        */
   unsigned tl;               /* the game's own time: frames played, less
                               * the frames gone back over (for the log)     */
   unsigned interval;         /* (atomic: the committer adapts it)           */
   unsigned iv_min;
   unsigned ema_bytes, commits;
   int      locked;

   /* rewinding */
   int      active, first, at_oldest, told_oldest;
   int      prev_want;
   unsigned held, credit, back_frames;
   unsigned tl_cur;           /* tl of the newest state, while rewinding      */
   const char *notice;

   rewind_stats st;
} R = { .seconds = REWIND_SECONDS_DEFAULT, .budget_mb = REWIND_MB_DEFAULT };

/* ---- the squeeze ------------------------------------------------------------- */

static size_t put_var(uint8_t *o, size_t v)
{
   size_t n = 0;
   while (v >= 0x80)
   {
      o[n++] = (uint8_t)(v | 0x80);
      v >>= 7;
   }
   o[n++] = (uint8_t)v;
   return n;
}

static int get_var(const uint8_t *in, size_t len, size_t *p, size_t *v)
{
   size_t r = 0;
   unsigned shift = 0;
   for (;;)
   {
      uint8_t c;
      if (*p >= len || shift > 28)
         return -1;
      c = in[(*p)++];
      r |= (size_t)(c & 0x7F) << shift;
      if (!(c & 0x80))
         break;
      shift += 7;
   }
   *v = r;
   return 0;
}

long rw_encode(const uint8_t *a, const uint8_t *b, size_t n, uint8_t *out,
               size_t cap)
{
   size_t i = 0, o = 0;

   while (i < n)
   {
      size_t s = i, l, k;

      /* the equal stretch, a machine word at a time: this is nearly all of
       * a state, and the reason a step costs what a pass over it costs */
      while (i + sizeof(size_t) <= n)
      {
         size_t x, y;
         memcpy(&x, a + i, sizeof(x));
         memcpy(&y, b + i, sizeof(y));
         if (x != y)
            break;
         i += sizeof(size_t);
      }
      while (i < n && a[i] == b[i])
         i++;
      if (i == n)
         break;               /* equal to the end: nothing more to say */

      /* the changed stretch, up to the next RW_GAP equal bytes */
      l = i;
      for (;;)
      {
         size_t z = 0;
         while (i < n && a[i] != b[i])
            i++;
         while (i + z < n && z < RW_GAP && a[i + z] == b[i + z])
            z++;
         if (z >= RW_GAP || i + z >= n)
            break;
         i += z;
      }
      if (o + 10 + (i - l) > cap)
         return -1;
      o += put_var(out + o, l - s);
      o += put_var(out + o, i - l);
      for (k = l; k < i; k++)
         out[o++] = (uint8_t)(a[k] ^ b[k]);
   }
   return (long)o;
}

int rw_apply(uint8_t *buf, size_t n, const uint8_t *in, size_t len)
{
   size_t i = 0, p = 0, k;

   while (p < len)
   {
      size_t skip, lit;
      if (get_var(in, len, &p, &skip) != 0 || get_var(in, len, &p, &lit) != 0)
         return -1;
      if (skip > n - i || lit > n - i - skip || lit > len - p)
         return -1;
      i += skip;
      for (k = 0; k < lit; k++)
         buf[i + k] ^= in[p + k];
      i += lit;
      p += lit;
   }
   return 0;
}

/* ---- the ring ------------------------------------------------------------------ */

static rw_ent *ent_at(unsigned i)       /* i steps after the oldest */
{
   return &R.idx[(R.head + i) % RW_IDX_CAP];
}

static void ring_reset(void)
{
   R.head = R.count = 0;
   R.wpos = R.used = 0;
   R.frames = 0;
}

static void ring_drop_oldest(void)
{
   rw_ent *e = ent_at(0);
   R.frames -= e->frames;
   R.used -= e->alloc;
   R.head = (R.head + 1) % RW_IDX_CAP;
   R.count--;
   R.st.dropped++;
}

/* A place for `len` bytes (1 <= len <= ring_cap), in one piece: the oldest
 * steps make way.  The steps lie in the ring in the order they were made,
 * wrapping at most once, so the free space is what is between the write
 * position and the oldest step -- one stretch, or two when the oldest is
 * behind the write position (the end of the ring, and its beginning). */
static size_t ring_place(size_t len)
{
   for (;;)
   {
      size_t tail;
      if (!R.count)
      {
         R.wpos = 0;
         break;
      }
      tail = ent_at(0)->off;
      if (tail >= R.wpos)
      {
         if (R.wpos + len <= tail)
            break;
      }
      else
      {
         if (R.wpos + len <= R.ring_cap)
            break;
         if (len <= tail)
         {
            R.wpos = 0;
            break;
         }
      }
      ring_drop_oldest();
   }
   return R.wpos;
}

/* The interval follows the budget: what one step costs now, against how
 * many of them the length asked for needs.  Looked at every 32 steps and
 * moved a little, so a change of scene does not swing it. */
static void interval_adapt(size_t step_bytes)
{
   uint64_t cap, pred;
   unsigned iv = __atomic_load_n(&R.interval, __ATOMIC_RELAXED);
   unsigned target = (unsigned)R.seconds * 60u;

   R.ema_bytes = R.ema_bytes ? (R.ema_bytes * 7u + (unsigned)step_bytes) / 8u
                             : (unsigned)step_bytes;
   if (++R.commits % 32u)
      return;
   cap = R.ring_cap - R.ring_cap / 16;       /* the wrap wastes a little */
   pred = cap * iv / (R.ema_bytes ? R.ema_bytes : 1);   /* frames held */
   if (pred < target)
   {
      /* Longer steps are bigger, but less than in proportion: assume not
       * at all, move at most half again, and look once more. */
      uint64_t want = ((uint64_t)target * R.ema_bytes + cap - 1) / cap;
      if (want > iv + (iv + 1) / 2)
         want = iv + (iv + 1) / 2;
      if (want <= iv)
         want = iv + 1;
      iv = want > REWIND_IV_MAX ? REWIND_IV_MAX : (unsigned)want;
   }
   else if (iv > R.iv_min)
   {
      /* Shorter steps are no bigger: if one frame less would still hold
       * the length with an eighth to spare, take it. */
      pred = cap * (iv - 1) / (R.ema_bytes ? R.ema_bytes : 1);
      if (pred > target + target / 8)
         iv--;
   }
   __atomic_store_n(&R.interval, iv, __ATOMIC_RELAXED);
}

/* The state in R.pend becomes the newest; the one that was is kept as its
 * difference.  The worker's when there is one. */
static void rw_commit(void)
{
   uint64_t t0 = plat_time_us();
   uint8_t *t;

   if (R.have_cur)
   {
      long n = rw_encode(R.cur, R.pend, R.state_len, R.enc, R.enc_cap);
      long bn = 0;
      size_t alloc;

      if (n >= 0 && R.pend_bat)
         bn = rw_encode(R.bat_cur, R.bat_pend, R.bat_len, R.enc + n,
                        R.enc_cap - (size_t)n);
      alloc = n >= 0 && bn >= 0 ? (size_t)(n + bn) : 0;
      if (!alloc)
         alloc = 1;
      if (n < 0 || bn < 0 || alloc > R.ring_cap || R.pend_frames > 0xFFFFu)
         ring_reset();        /* a step the ring cannot hold: start over here */
      else
      {
         rw_ent *e;
         size_t off = ring_place(alloc);
         if (R.count == RW_IDX_CAP)
            ring_drop_oldest();
         memcpy(R.ring + off, R.enc, (size_t)(n + bn));
         e = ent_at(R.count++);
         e->off = (uint32_t)off;
         e->len = (uint32_t)n;
         e->bat_len = (uint32_t)bn;
         e->alloc = (uint32_t)alloc;
         e->frames = (uint16_t)R.pend_frames;
         R.wpos = off + alloc;
         R.used += alloc;
         R.frames += R.pend_frames;
         R.st.raw_bytes += R.state_len;
         R.st.packed_bytes += alloc;
         if (bn)
            R.st.battery_steps++;
         interval_adapt(alloc);
      }
   }
   t = R.cur; R.cur = R.pend; R.pend = t;
   if (R.pend_bat)
   {
      t = R.bat_cur; R.bat_cur = R.bat_pend; R.bat_pend = t;
   }
   R.have_cur = 1;
   {
      unsigned us = (unsigned)(plat_time_us() - t0);
      R.st.pack_us += us;
      if (us > R.st.pack_max_us)
         R.st.pack_max_us = us;
   }
}

void rewind_worker(void)
{
   if (pend_take())
   {
      rw_commit();
      __atomic_store_n(&g_pend, PEND_NONE, __ATOMIC_SEQ_CST);
   }
}

/* The frame thread wants the ring.  A job nobody has started is done here
 * and now; one the worker is in the middle of is waited for (not on the
 * display's clock: plat_thread_yield). */
static void rw_wait_idle(void)
{
   rewind_worker();
   while (__atomic_load_n(&g_pend, __ATOMIC_SEQ_CST) != PEND_NONE)
      plat_thread_yield();
}

void rewind_set_async(void (*wake)(void))
{
   if (!wake && R.wake)
      rw_wait_idle();         /* (the worker has stopped: nothing is BUSY) */
   R.wake = wake;
}

/* ---- the session ---------------------------------------------------------------- */

static void rw_end(const char *why);

/* The history is about to go (cleared, rebuilt, the game ending): a rewind
 * in progress ends where it is. */
static void rw_stop(const char *why)
{
   if (R.active)
      rw_end(why);
}

static void rw_free(void)
{
   free(R.ring);  free(R.idx);
   free(R.cur);   free(R.pend);  free(R.enc);
   free(R.bat_cur); free(R.bat_pend);
   R.ring = R.cur = R.pend = R.enc = R.bat_cur = R.bat_pend = NULL;
   R.idx = NULL;
   R.bat_live = NULL;
   R.on = 0;
}

static void rw_forget(void)
{
   ring_reset();
   R.have_cur = R.taken = 0;
   R.since = 0;
   R.bat_dirty = 0;
   R.ema_bytes = 0;
   R.commits = 0;
   __atomic_store_n(&R.interval, R.iv_min, __ATOMIC_RELAXED);
}

static void rw_alloc(void)
{
   size_t state = fe_host_state_mem_size(), bat = 0, fixed, budget, need, ring;
   void *live = fe_host_battery_mem(&bat);
   unsigned steps;

   rw_free();
   if (!R.game || R.seconds <= 0 || !state || R.locked)
      return;                 /* (locked: built when the link is over, with
                               * the battery image a session hides) */
   R.state_len = state;
   R.state_cap = (state + 15) & ~(size_t)15;
   R.bat_len = live ? bat : 0;
   R.enc_cap = RW_BOUND(R.state_cap) + RW_BOUND(R.bat_len);
   R.iv_min = state <= 256u * 1024u ? REWIND_IV_MIN_SMALL : REWIND_IV_MIN;

   fixed = 2 * R.state_cap + R.enc_cap + 2 * R.bat_len +
           RW_IDX_CAP * sizeof(rw_ent);
   budget = (size_t)R.budget_mb << 20;
   {
      /* Never the heap's last megabytes: a link may need a second
       * cartridge after this (rewind_vita.h). */
      size_t have = plat_heap_free();
      size_t keep = (size_t)REWIND_HEAP_RESERVE_MB << 20;
      if (have != (size_t)-1)
      {
         size_t spare = have > keep ? have - keep : 0;
         if (budget > spare)
            budget = spare;
         if (budget < ((size_t)REWIND_MB_MIN << 20))
         {
            fe_evt("rewind off reason=heap free=%u budget_mb=%d",
                   (unsigned)have, R.budget_mb);
            return;
         }
      }
   }
   /* A short history of a small machine does not need the whole budget:
    * an eighth of a state per step is several times what a step takes. */
   steps = (unsigned)R.seconds * 60u / R.iv_min + 64u;
   need = (size_t)steps * (state / 8 > 2048 ? state / 8 : 2048);
   ring = budget > fixed ? budget - fixed : 0;
   if (ring > need)
      ring = need;

   R.cur = (uint8_t *)calloc(1, R.state_cap);
   R.pend = (uint8_t *)calloc(1, R.state_cap);
   R.enc = (uint8_t *)malloc(R.enc_cap);
   R.idx = (rw_ent *)malloc(RW_IDX_CAP * sizeof(rw_ent));
   if (R.bat_len)
   {
      R.bat_cur = (uint8_t *)malloc(R.bat_len);
      R.bat_pend = (uint8_t *)malloc(R.bat_len);
   }
   /* The heap may have less than the budget to give (a second cartridge is
    * loaded): half, and half again, down to what is still worth having. */
   while (ring >= (512u << 10) && !(R.ring = (uint8_t *)malloc(ring)))
      ring /= 2;
   if (!R.cur || !R.pend || !R.enc || !R.idx || !R.ring ||
       (R.bat_len && (!R.bat_cur || !R.bat_pend)))
   {
      fe_evt("rewind off reason=memory budget_mb=%d state=%u", R.budget_mb,
             (unsigned)state);
      rw_free();
      return;
   }
   R.ring_cap = ring;
   R.bat_live = (uint8_t *)live;
   R.on = 1;
   rw_forget();
   R.st.on = 1;
   R.st.seconds = (unsigned)R.seconds;
   R.st.ring_bytes = (unsigned)ring;
   R.st.total_bytes = (unsigned)(ring + fixed);
   R.st.state_bytes = (unsigned)state;
   fe_evt("rewind on seconds=%d budget_mb=%d ring=%u total=%u state=%u "
          "battery=%u iv_min=%u async=%d", R.seconds, R.budget_mb,
          (unsigned)ring, (unsigned)(ring + fixed), (unsigned)state,
          (unsigned)R.bat_len, R.iv_min, R.wake != NULL);
}

void rewind_configure(int seconds, int budget_mb)
{
   if (seconds < 0)
      seconds = 0;
   if (seconds > REWIND_SECONDS_MAX)
      seconds = REWIND_SECONDS_MAX;
   if (budget_mb < REWIND_MB_MIN)
      budget_mb = REWIND_MB_MIN;
   if (budget_mb > REWIND_MB_MAX)
      budget_mb = REWIND_MB_MAX;
   if (seconds == R.seconds && budget_mb == R.budget_mb)
      return;
   R.seconds = seconds;
   R.budget_mb = budget_mb;
   if (R.game)
   {
      rw_stop("settings");
      rw_wait_idle();
      rw_alloc();
   }
}

static void stats_evt(const char *why)
{
   rewind_stats s;
   rewind_get_stats(&s);
   fe_evt("rewind_stats why=%s interval=%u entries=%u frames=%u used=%u "
          "ring=%u captures=%u skipped=%u late=%u dropped=%u raw=%llu "
          "packed=%llu "
          "battery_steps=%u capture_us=%llu/%u pack_us=%llu/%u rewinds=%u "
          "steps=%u landings=%u landing_us=%llu/%u", why, s.interval,
          s.entries, s.frames, s.used_bytes, s.ring_bytes, s.captures,
          s.skipped, s.late, s.dropped, (unsigned long long)s.raw_bytes,
          (unsigned long long)s.packed_bytes, s.battery_steps,
          (unsigned long long)s.capture_us, s.capture_max_us,
          (unsigned long long)s.pack_us, s.pack_max_us, s.rewinds,
          s.steps_back, s.landings, (unsigned long long)s.landing_us,
          s.landing_max_us);
}

void rewind_game_begin(void)
{
   memset(&R.st, 0, sizeof(R.st));
   R.game = 1;
   R.tl = 0;
   R.active = R.locked = R.prev_want = 0;
   R.notice = NULL;
   __atomic_store_n(&g_pend, PEND_NONE, __ATOMIC_SEQ_CST);
   rw_alloc();
}

void rewind_game_end(void)
{
   if (!R.game)
      return;
   rw_stop("game_end");
   rw_wait_idle();
   if (R.on)
      stats_evt("game_end");
   R.game = 0;
   rw_free();
}

void rewind_clear(void)
{
   if (!R.on)
      return;
   rw_stop("cleared");
   rw_wait_idle();
   rw_forget();
}

void rewind_get_stats(rewind_stats *out)
{
   if (R.on)
      rw_wait_idle();         /* the committer's numbers, when it is done */
   *out = R.st;
   out->on = R.on;
   out->interval = __atomic_load_n(&R.interval, __ATOMIC_RELAXED);
   out->entries = R.count;
   out->frames = R.frames;
   out->used_bytes = (unsigned)R.used;
}

/* ---- capture ---------------------------------------------------------------------- */

/* The machine as it is now, into the hand-over slot.  0 = it is there. */
static int rw_take(void)
{
   long n = fe_host_state_mem_save(R.pend, R.state_cap);

   if (n <= 0 || (size_t)n != R.state_len)
      return -1;              /* a session has the machines, or the state
                               * changed size: neither is rewound across */
   R.pend_bat = 0;
   if (R.bat_live)
   {
      uint32_t gen = fe_host_battery_gen();
      if (!R.taken || gen != R.bat_gen)
      {
         /* The game wrote its save since the last state (or this is the
          * first): the cartridge's memory goes with this one. */
         memcpy(R.bat_pend, R.bat_live, R.bat_len);
         R.bat_gen = gen;
         R.pend_bat = 1;
      }
   }
   R.pend_frames = R.since;
   R.since = 0;
   R.taken = 1;
   return 0;
}

void rewind_capture(void)
{
   uint64_t t0;
   unsigned us;

   if (!R.on || R.active || R.locked)
      return;
   R.since++;
   R.tl++;
   if (R.taken && R.since < __atomic_load_n(&R.interval, __ATOMIC_RELAXED))
      return;
   t0 = plat_time_us();
   if (__atomic_load_n(&g_pend, __ATOMIC_SEQ_CST) != PEND_NONE)
   {
      /* The last state is still waiting, a whole interval later.  If the
       * worker has not even begun it (it is held up: a slow write to the
       * card), it is done here -- history is not lost to the card; if the
       * worker is in the middle of it, this state waits a frame. */
      if (!pend_take())
      {
         R.st.skipped++;
         return;
      }
      rw_commit();
      __atomic_store_n(&g_pend, PEND_NONE, __ATOMIC_SEQ_CST);
      R.st.late++;
   }
   if (rw_take() != 0)
   {
      if (R.taken)
         rw_forget();         /* (the slot is empty: nobody else is in it) */
      return;
   }
   R.st.captures++;
   if (R.wake)
   {
      __atomic_store_n(&g_pend, PEND_READY, __ATOMIC_SEQ_CST);
      R.wake();
   }
   else
      rw_commit();
   us = (unsigned)(plat_time_us() - t0);
   R.st.capture_us += us;
   if (us > R.st.capture_max_us)
      R.st.capture_max_us = us;
}

/* ---- going back --------------------------------------------------------------------- */

/* The newest state becomes the one before it.  0 = there is none before. */
static int rw_step_back(void)
{
   rw_ent *e;

   if (!R.count)
      return 0;
   e = ent_at(R.count - 1);
   if (rw_apply(R.cur, R.state_len, R.ring + e->off, e->len) != 0 ||
       (e->bat_len && rw_apply(R.bat_cur, R.bat_len,
                               R.ring + e->off + e->len, e->bat_len) != 0))
   {
      /* Cannot happen short of a stray write into the ring: the newest
       * state is no longer one, so nothing here may be used again. */
      fe_evt("rewind FAILED reason=damaged_step");
      rw_forget();
      return -1;
   }
   if (e->bat_len)
      R.bat_dirty = 1;
   R.back_frames += e->frames;
   R.tl_cur -= e->frames;
   R.frames -= e->frames;
   R.used -= e->alloc;
   R.wpos = e->off;           /* its place is free again */
   R.count--;
   R.st.steps_back++;
   return 1;
}

/* Put the newest state into the machine and run its one frame: the picture
 * of this moment (rewind_vita.h, "what is on screen"). */
static int rw_land(void)
{
   uint64_t t0 = plat_time_us();
   unsigned us;

   if (fe_host_state_mem_load(R.cur, R.state_len) != 0)
      return -1;
   if (R.bat_live && (R.bat_dirty || fe_host_battery_gen() != R.bat_gen))
   {
      /* The cartridge as it was at this state: the game wrote to it after
       * (or a step just went back across such a write). */
      memcpy(R.bat_live, R.bat_cur, R.bat_len);
      fe_host_battery_changed();
      R.bat_gen = fe_host_battery_gen();
      R.bat_dirty = 0;
   }
   cheats_apply();            /* the state brought its own RAM back */
   g_app_keys = 0;
   fe_host_run_frame();       /* -> the video sink draws it; the audio sink
                               * is silent while rewind_active() */
   plat_frame_ran();
   plat_power_tick();
   R.since = 1;
   R.tl = R.tl_cur + 1;
   R.st.landings++;
   us = (unsigned)(plat_time_us() - t0);
   R.st.landing_us += us;
   if (us > R.st.landing_max_us)
      R.st.landing_max_us = us;
   return 0;
}

static void rw_chip(void)
{
   /* The chip holds 23 characters (osd_psp.c): "<< REWIND 12 s",
    * "<< REWIND 4:59", "<< 4:59 (oldest)". */
   char t[16], chip[48];
   unsigned s = (R.back_frames + 30u) / 60u;
   if (s < 60)
      snprintf(t, sizeof(t), "%u s", s);
   else
      snprintf(t, sizeof(t), "%u:%02u", (s / 60) % 1000u, s % 60);
   snprintf(chip, sizeof(chip), R.at_oldest ? "<< %s (oldest)"
                                            : "<< REWIND %s", t);
   osd_chip_ff(chip);
}

static void rw_end(const char *why)
{
   /* tl = the game's own frame number of the picture on screen (what a test
    * compares with a run that never went back); frame = fe_host's count,
    * which also counted every step's one frame. */
   fe_evt("rewind end why=%s back_frames=%u oldest=%d left=%u tl=%u frame=%u",
          why, R.back_frames, R.at_oldest, R.frames, R.tl,
          fe_host_frame_count());
   R.active = 0;
   osd_chip_ff(NULL);
   app_audio_flush();         /* nothing was queued; nor may anything stale be */
   app_pace_reset();
}

int rewind_frame(int want, int locked)
{
   int pressed = want && !R.prev_want;
   int landed = 0, rc = 1;

   R.prev_want = want;
   if (!R.game)
      return 0;
   if (locked != R.locked)
   {
      R.locked = locked;
      if (locked)
      {
         if (R.active)
            rw_end("locked");
         rewind_clear();      /* a link's machines are not this timeline */
      }
      else if (!R.on)
         rw_alloc();          /* the settings changed while it was locked */
   }
   if (!R.active)
   {
      if (!pressed)
         return 0;
      if (locked)
      {
         R.notice = "Rewind is off while games are linked";
         fe_evt("rewind refused reason=locked");
         return 0;
      }
      if (!R.on)
      {
         R.notice = R.seconds > 0 ? "Rewind: not enough memory for it"
                                  : "Rewind is switched off (Settings)";
         fe_evt("rewind refused reason=%s", R.seconds > 0 ? "memory" : "off");
         return 0;
      }
      rw_wait_idle();
      if (!R.have_cur)
      {
         /* Nothing yet (the game has only just started, or a state was
          * just loaded): now is the oldest moment there is. */
         R.since = 0;
         if (rw_take() != 0)
            return 0;
         rw_commit();
      }
      R.tl_cur = R.tl - R.since;
      R.active = R.first = 1;
      R.at_oldest = R.told_oldest = 0;
      R.held = R.credit = 0;
      R.back_frames = R.since;
      R.st.rewinds++;
      fe_evt("rewind begin tl=%u frame=%u entries=%u frames=%u interval=%u",
             R.tl, fe_host_frame_count(), R.count, R.frames, R.interval);
   }
   else if (!want)
   {
      rw_end("released");
      return 0;
   }

   /* ---- one refresh of going back ---- */
   if (R.first)
   {
      /* The newest state itself: up to an interval ago. */
      R.first = 0;
      landed = 1;
   }
   else
   {
      R.credit += rw_speed(R.held);
      while (rc > 0 && R.count && R.credit >= ent_at(R.count - 1)->frames)
      {
         R.credit -= ent_at(R.count - 1)->frames;
         rc = rw_step_back();
         if (rc > 0)
            landed = 1;
      }
      if (!R.count)
      {
         R.credit = 0;
         if (!R.at_oldest)
            R.at_oldest = 1;
      }
   }
   R.held++;
   if (rc < 0 || (landed && rw_land() != 0))
   {
      fe_evt("rewind FAILED reason=%s", rc < 0 ? "step" : "state_load");
      rw_forget();
      rw_end("failed");
      R.notice = "Rewind failed";
      return 0;
   }
   if (!landed)
   {
      /* Between two steps, or at the oldest point: the picture stands. */
      unsigned w, h;
      size_t pitch;
      const uint16_t *pix = app_shown_frame(&w, &h, &pitch);
      if (pix)
      {
         vid_draw_frame(pix, w, h, pitch);
         g_app_drew = 1;
      }
   }
   if (R.at_oldest && !R.told_oldest)
   {
      R.told_oldest = 1;
      R.notice = "This is as far back as rewind goes";
   }
   rw_chip();
   return 1;
}

int rewind_active(void)
{
   return R.active;
}

const char *rewind_notice(void)
{
   const char *n = R.notice;
   R.notice = NULL;
   return n;
}
