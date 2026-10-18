/* fe_rewind.c -- rewind core (PROTOTYPE).  The design is at the top of
 * fe_rewind.h; docs/REWIND.md has the measurements.
 *
 * Nothing here allocates, and nothing here runs unless the platform calls
 * it: a build without REWIND does not compile this file at all. */
#include "fe_rewind.h"

#include <string.h>

/* A capture buffer: the pictures and sound of a WINDOW of the timeline,
 * frames start+1 .. start+done, of at most `span` (<= interval, the buffer's
 * capacity).  In normal play the two buffers are the window being played and
 * the one before it, rolling.  While rewinding they are the window being
 * shown and the one before it, which the machine is re-running. */
typedef struct
{
   int       valid;
   uint32_t  start;
   uint32_t  span;       /* frames this window is meant to hold */
   uint32_t  done;       /* captured so far, contiguous from start+1 */
   uint16_t *pix;
   int16_t  *aud;
   uint32_t *aud_n;
} rw_seg;

static struct
{
   fe_rewind_config cfg;
   int       ready;
   int       fault;          /* failed restore: no replay/play until reset */

   uint32_t *ref;          /* the reference image: the newest snapshot */
   uint32_t  state_bytes;  /* the spans end to end */
   int       have_ref;     /* ref holds the state at ref_frame */
   int       pack_pending; /* gather ran for frame gat_frame; pack has not */
   int       ref_moved;    /* ref was stepped back and the machine has not
                            * been restored to it since: the dirty maps do
                            * not describe the difference between the two */
   uint32_t  ref_frame, gat_frame;
   uint64_t  snap_us;      /* clock at the last gather (pacing) */

   uint8_t  *ring, *ring2;
   uint32_t  ring_bytes, ring_split, head, used;

   fe_rewind_rec *rec;     /* index ring, oldest at rec_tail */
   uint32_t  rec_cap, rec_tail, rec_n;

   uint16_t *log;          /* input of frame f at log[f & log_mask] */
   uint64_t *context;      /* optional external input, same frame/lifetime */
   uint32_t  log_mask;

   uint32_t  mach;         /* frame the machine is at */
   uint32_t  now;          /* frame on screen */
   uint32_t  top;          /* `now` when the current rewind began */
   int       gesture;      /* a rewind has moved `now` since normal play */
   int       at_limit;     /* the last tick was held at the start of history */
   int       busy;         /* fe_rewind_hint_busy, for the next frame only */
   uint32_t  stride;       /* fe_rewind_set_stride (>= 1) */
   uint32_t  phase;        /* a frame f keeps its picture iff
                            * (phase - f) % stride == 0 */
   int       tainted;      /* fe_rewind_taint since the last restore */
   int       tick_pics;    /* frames with a picture re-run this tick */
   uint32_t  run_us;       /* what a re-run frame has been costing: */
   uint32_t  target_s;     /* fe_rewind_set_target */
   uint32_t  gap_max;
   uint32_t  gap;          /* frames between snapshots, now */
   uint32_t  snap_bytes;   /* what a snapshot has been taking in the ring */
   uint32_t  run_pic_us;   /* ...one that keeps a picture */
   uint32_t  run_head_us;  /* ...one that does not */
   uint32_t  margin;       /* start preparing the previous window when `now`
                            * is this close to the start of its own */
   rw_seg    seg[2];
   int16_t  *rev;          /* reversed sound of the frame being shown */

   /* Where a rewind may not stop (fe_rewind_set_noland) */
   uint32_t  quiet;        /* N; 0 = anywhere */
   uint32_t  last_change;  /* every frame below this one has an outside
                            * change after it */
   uint32_t  floor;        /* log entries at or below this frame are not
                            * this history's */
   int       snapped;      /* `now` was moved off a no-land frame and the
                            * machine is still on its way there */

   uint32_t  tick_frames, tick_restores, catchup_run;
   uint64_t  tick_t0;      /* clock at the start of this tick */
   fe_rewind_stats st;
} rw;

static uint64_t now_us(void)
{
   return rw.cfg.now_us ? rw.cfg.now_us() : 0;
}

static void stat_add(uint64_t t0, uint64_t *sum, uint32_t *max, uint32_t *wmax)
{
   uint32_t d;
   if (!rw.cfg.now_us)
      return;
   d = (uint32_t)(rw.cfg.now_us() - t0);
   *sum += d;
   if (d > *max)
      *max = d;
   if (wmax && d > *wmax)
      *wmax = d;
}

/* ------------------------------------------------------------ snapshot -- */

void fe_rewind_gather(void)
{
   uint64_t t0;
   if (!rw.ready)
      return;
   fe_rewind_pack();            /* one snapshot in flight at a time */
   t0 = now_us();
   if (rw.cfg.gather() == 0)
   {
      rw.pack_pending = 1;
      rw.gat_frame = rw.mach;
   }
   rw.snap_us = t0;
   stat_add(t0, &rw.st.gather_us, &rw.st.gather_max_us,
            &rw.st.gather_win_max_us);
}

/* cfg.verify: the reference must now equal the machine, word for word. */
static void verify_ref(void)
{
   uint32_t *ref = rw.ref;
   uint32_t s, off = 0, bad = 0;
   for (s = 0; s < rw.cfg.nspans; s++)
   {
      const uint32_t *cur = (const uint32_t *)rw.cfg.spans[s].cur;
      uint32_t words = rw.cfg.spans[s].bytes / 4, i;
      for (i = 0; i < words; i++)
         if (cur[i] != ref[i])
         {
            if (!rw.st.verify_bad_words && !bad)
               rw.st.verify_first_off = off + i * 4;
            bad++;
            ref[i] = cur[i];
         }
      ref += words;
      off += words * 4;
   }
   rw.st.verify_runs++;
   if (bad)
   {
      rw.st.verify_bad_words += bad;
      rw.st.verify_bad_snaps++;
      rw.st.anomalies++;
   }
}

void fe_rewind_pack(void)
{
   uint64_t t0;

   if (!rw.ready || !rw.pack_pending)
      return;
   rw.pack_pending = 0;
   t0 = now_us();

   if (!rw.have_ref)
   {
      /* First snapshot after a reset: it IS the reference; no delta. */
      uint8_t *d = (uint8_t *)rw.ref;
      uint32_t s;
      for (s = 0; s < rw.cfg.nspans; s++)
      {
         memcpy(d, rw.cfg.spans[s].cur, rw.cfg.spans[s].bytes);
         d += rw.cfg.spans[s].bytes;
      }
      rw.have_ref = 1;
   }
   else
   {
      fe_rewind_packjob j;
      j.spans      = rw.cfg.spans;
      j.nspans     = rw.cfg.nspans;
      j.ref        = rw.ref;
      j.ring       = rw.ring;
      j.ring_bytes = rw.ring_bytes;
      j.ring2      = rw.ring2;
      j.ring_split = rw.ring_split;
      j.rec        = rw.rec;
      j.rec_cap    = rw.rec_cap;
      j.base_frame = rw.ref_frame;
      j.ignore_dirty = (uint32_t)rw.ref_moved;
      j.head       = rw.head;
      j.used       = rw.used;
      j.rec_tail   = rw.rec_tail;
      j.rec_n      = rw.rec_n;
      fe_rewind_pack_run(&j);
      rw.head     = j.head;
      rw.used     = j.used;
      rw.rec_tail = j.rec_tail;
      rw.rec_n    = j.rec_n;
      rw.st.evicted += j.evicted;
      rw.st.words_read += j.words_read;
      rw.st.words_total += rw.state_bytes / 4;
      if (j.overflow)
         rw.st.overflow_resets++;
      else
      {
         rw.st.snapshots++;
         rw.st.packed_bytes += j.len;
         /* (first pass only: a snapshot left behind on the way back is the
          * same content again) */
         if (rw.target_s && rw.mach == rw.now)
         {
            /* bytes a frame may cost for the ring to hold the target */
            uint32_t per_frame = rw.cfg.ring_bytes / (rw.target_s * 60u);
            uint32_t g;
            rw.snap_bytes = rw.snap_bytes ? (rw.snap_bytes * 7u + j.len) / 8u
                                          : j.len;
            g = per_frame ? (rw.snap_bytes + per_frame - 1u) / per_frame
                          : rw.gap_max;
            if (g < rw.cfg.interval) g = rw.cfg.interval;
            if (g > rw.gap_max)      g = rw.gap_max;
            rw.gap = g;
         }
         if (j.len > rw.st.packed_max)
            rw.st.packed_max = j.len;
      }
   }
   rw.ref_frame = rw.gat_frame;
   rw.ref_moved = 0;            /* reference == machine again, everywhere */
   if (rw.cfg.verify)
      verify_ref();
   if (rw.cfg.snapshot_done)
      rw.cfg.snapshot_done();

   /* The input log is a ring too: history it will not cover until the next
    * snapshot (up to max_span more frames get logged before then) goes. */
   while (rw.rec_n && rw.ref_frame - rw.rec[rw.rec_tail].base >=
                      rw.cfg.max_frames - rw.cfg.max_span - rw.cfg.interval)
   {
      rw.used -= rw.rec[rw.rec_tail].len;
      rw.rec_tail = (rw.rec_tail + 1) % rw.rec_cap;
      rw.rec_n--;
      rw.st.evicted++;
   }

   stat_add(t0, &rw.st.pack_us, &rw.st.pack_max_us, &rw.st.pack_win_max_us);
}

/* n bytes from ring offset `off`, not wrapping (two pieces of memory). */
static void ring_load(uint32_t off, uint8_t *dst, uint32_t n)
{
   if (off < rw.ring_split)
   {
      uint32_t k = rw.ring_split - off;
      if (k > n)
         k = n;
      memcpy(dst, rw.ring + off, k);
      dst += k;
      off += k;
      n -= k;
   }
   if (n)
      memcpy(dst, rw.ring2 + (off - rw.ring_split), n);
}

static void ring_get(uint32_t *off, void *dst, uint32_t n)
{
   uint32_t first = rw.ring_bytes - *off;
   if (first > n)
      first = n;
   ring_load(*off, (uint8_t *)dst, first);
   if (n > first)
      ring_load(0, (uint8_t *)dst + first, n - first);
   *off += n;
   if (*off >= rw.ring_bytes)
      *off -= rw.ring_bytes;
}

/* ref := the snapshot before it.  0 = there is none. */
static int step_back(void)
{
   fe_rewind_rec *e;
   uint32_t off, left, i = 0;
   uint64_t t0;

   fe_rewind_pack();
   if (!rw.rec_n)
      return 0;
   t0 = now_us();
   e = &rw.rec[(rw.rec_tail + rw.rec_n - 1) % rw.rec_cap];
   off = e->off;
   left = e->len;
   while (left)
   {
      uint16_t hdr[2];
      uint32_t k;
      ring_get(&off, hdr, 4);
      left -= 4;
      i += hdr[0];
      if (hdr[1] && rw.cfg.ref_touched)
         rw.cfg.ref_touched(i * 4, (uint32_t)hdr[1] * 4);
      for (k = 0; k < hdr[1]; )
      {
         uint32_t x[64], m = hdr[1] - k, j;
         if (m > 64)
            m = 64;
         ring_get(&off, x, m * 4);
         for (j = 0; j < m; j++)
            rw.ref[i + j] ^= x[j];
         i += m;
         k += m;
      }
      left -= (uint32_t)hdr[1] * 4;
   }
   rw.head = e->off;
   rw.used -= e->len;
   rw.ref_frame = e->base;
   rw.ref_moved = 1;
   rw.rec_n--;
   rw.st.steps_back++;
   stat_add(t0, &rw.st.unpack_us, &rw.st.unpack_max_us, NULL);
   return 1;
}

/* The machine := the snapshot in ref. */
static int restore_ref(void)
{
   uint64_t t0 = now_us();
   fe_rewind_pack();
   if (rw.cfg.restore(rw.ref, rw.state_bytes) != 0)
   {
      /* A loader may have rejected or partly consumed the image. Pretending
       * it is the reference can resume a save sequence with unrelated RAM. */
      rw.fault = 1;
      rw.st.anomalies++;
      return -1;
   }
   rw.tainted = 0;
   rw.ref_moved = 0;
   rw.mach = rw.ref_frame;
   rw.st.restores++;
   rw.tick_restores++;
   stat_add(t0, &rw.st.restore_us, &rw.st.restore_max_us, NULL);
   return 0;
}

static int snapshot_due(void)
{
   uint32_t n;
   if (!rw.have_ref)
      return 1;
   n = rw.mach - rw.ref_frame;
   if (n < rw.cfg.interval)
      return 0;
   if (n >= rw.cfg.max_span)
      return 1;
   if (rw.target_s && n < rw.gap && rw.mach == rw.now)
      return 0;                 /* large snapshots: further apart */
   if (rw.busy)
   {
      rw.st.deferred++;
      return 0;
   }
   if (!rw.cfg.pace_us || !rw.cfg.now_us)
      return 1;
   return rw.cfg.now_us() - rw.snap_us >= rw.cfg.pace_us;
}

static void snapshot_if_due(void)
{
   if (snapshot_due())
   {
      fe_rewind_gather();
      fe_rewind_pack();   /* a platform with a second core posts this instead */
   }
}

/* The newest snapshot frame below `end`, or `end` if there is none. */
static uint32_t snapshot_below(uint32_t end)
{
   uint32_t k;
   if (rw.ref_frame < end)
      return rw.ref_frame;
   for (k = rw.rec_n; k--; )
   {
      uint32_t b = rw.rec[(rw.rec_tail + k) % rw.rec_cap].base;
      if (b < end)
         return b;
   }
   return end;
}

static uint32_t oldest_snapshot(void)
{
   return rw.rec_n ? rw.rec[rw.rec_tail].base : rw.ref_frame;
}

/* ---------------------------------------------------------------- setup -- */

static uint32_t pow2_up(uint32_t v)
{
   uint32_t p = 1;
   while (p < v)
      p <<= 1;
   return p;
}

static size_t align8(size_t v)
{
   return (v + 7u) & ~(size_t)7u;
}

static uint32_t rec_cap_of(const fe_rewind_config *c)
{
   return pow2_up(c->max_frames) / c->interval + 2;
}

static uint32_t state_bytes_of(const fe_rewind_config *c)
{
   uint32_t s, n = 0;
   for (s = 0; s < c->nspans; s++)
   {
      if ((c->spans[s].bytes & 3) || !c->spans[s].cur ||
          (c->spans[s].dirty && c->spans[s].page_shift < 2))
         return 0;
      n += c->spans[s].bytes;
   }
   return n;
}

/* Pictures start on a 64-byte boundary and are padded to one: a platform
 * may have a second CPU render straight into them through its own cache. */
static size_t align64(size_t v)
{
   return (v + 63u) & ~(size_t)63u;
}

static size_t seg_bytes(const fe_rewind_config *c)
{
   return align64((size_t)c->interval * c->frame_pixels * 2) + 64 +
          align8((size_t)c->interval * c->audio_cap * 4) +
          align8((size_t)c->interval * 4);
}

size_t fe_rewind_mem_bytes(const fe_rewind_config *c)
{
   if (!c || !c->interval || !c->spans || !state_bytes_of(c))
      return 0;
   return align8(state_bytes_of(c)) +
          (c->ring_mem ? 0 : align8(c->ring_bytes)) +
          align8((size_t)rec_cap_of(c) * sizeof(fe_rewind_rec)) +
          align8((size_t)pow2_up(c->max_frames) * 2) + 2 * seg_bytes(c) +
          (c->frame_context ? (size_t)pow2_up(c->max_frames) * 8 : 0) +
          align8((size_t)c->audio_cap * 4);
}

int fe_rewind_init(const fe_rewind_config *c, void *mem, size_t mem_bytes)
{
   uint8_t *p = (uint8_t *)mem;
   size_t need = fe_rewind_mem_bytes(c);
   uint32_t span;
   int s;

   memset(&rw, 0, sizeof(rw));
   if (!need || !mem || mem_bytes < need || !c->gather || !c->restore ||
       !c->run || c->ring_bytes < 64 || !c->frame_pixels ||
       (!!c->frame_context != !!c->replay_context))
      return -1;
   /* Without pacing a snapshot falls every `interval` frames; a platform
    * that uses fe_rewind_hint_busy sets max_span itself. */
   span = c->max_span ? c->max_span : (c->pace_us ? 8 * c->interval
                                                  : c->interval);
   if (span < c->interval)
      span = c->interval;
   if (pow2_up(c->max_frames) < 4 * (span + c->interval))
      return -1;
   rw.cfg = *c;
   rw.cfg.max_frames = pow2_up(c->max_frames);
   rw.cfg.max_span = span;
   if (!rw.cfg.burst)
      rw.cfg.burst = 2;
   /* Re-running a window takes interval/burst ticks plus the restore's. */
   rw.stride = 1;
   rw.margin = (c->interval + rw.cfg.burst - 1) / rw.cfg.burst + 1;
   if (c->tick_budget_us && c->now_us)
      rw.margin = c->interval + 1;      /* a tick may manage only one frame */
   rw.state_bytes = state_bytes_of(c);

   rw.ref = (uint32_t *)p;  p += align8(rw.state_bytes);
   if (c->ring_mem)
   {
      rw.ring = (uint8_t *)c->ring_mem;
      rw.ring2 = (uint8_t *)c->ring_mem2;
      rw.ring_split = c->ring_mem2 ? c->ring_split : c->ring_bytes;
      if (rw.ring_split > c->ring_bytes)
         return -1;
   }
   else
   {
      rw.ring = p;
      rw.ring2 = NULL;
      rw.ring_split = c->ring_bytes;
      p += align8(c->ring_bytes);
   }
   rw.ring_bytes = c->ring_bytes;
   rw.rec_cap = rec_cap_of(c);
   rw.rec = (fe_rewind_rec *)p;
   p += align8((size_t)rw.rec_cap * sizeof(fe_rewind_rec));
   rw.log = (uint16_t *)p;  p += align8((size_t)rw.cfg.max_frames * 2);
   rw.log_mask = rw.cfg.max_frames - 1;
   if (c->frame_context)
   {
      rw.context = (uint64_t *)p;
      p += (size_t)rw.cfg.max_frames * 8;
   }
   for (s = 0; s < 2; s++)
   {
      p = (uint8_t *)mem + align64((size_t)(p - (uint8_t *)mem));
      rw.seg[s].pix = (uint16_t *)p;
      p += align64((size_t)c->interval * c->frame_pixels * 2);
      rw.seg[s].aud = (int16_t *)p;
      p += align8((size_t)c->interval * c->audio_cap * 4);
      rw.seg[s].aud_n = (uint32_t *)p;
      p += align8((size_t)c->interval * 4);
   }
   rw.rev = (int16_t *)p;
   rw.ready = 1;
   return 0;
}

static uint32_t oldest_snapshot(void);

void fe_rewind_reset(uint32_t frame)
{
   if (!rw.ready)
      return;
   rw.fault = 0;
   rw.have_ref = 0;
   rw.pack_pending = 0;
   rw.ref_moved = 0;
   rw.head = rw.used = 0;
   rw.rec_tail = rw.rec_n = 0;
   rw.mach = rw.now = rw.top = frame;
   rw.gesture = 0;
   rw.at_limit = 0;
   rw.tainted = 0;
   rw.seg[0].valid = rw.seg[1].valid = 0;
   rw.floor = rw.last_change = frame;
   rw.snapped = 0;
}

uint32_t fe_rewind_trim(uint32_t frame)
{
   if (!rw.ready || !rw.have_ref)
      return 0xFFFFFFFFu;              /* no history: nothing older than it */
   if (rw.mach != rw.now)
      return oldest_snapshot();
   fe_rewind_pack();
   while (rw.rec_n && rw.rec[rw.rec_tail].base < frame)
   {
      rw.used -= rw.rec[rw.rec_tail].len;
      rw.rec_tail = (rw.rec_tail + 1) % rw.rec_cap;
      rw.rec_n--;
      rw.st.evicted++;
   }
   return oldest_snapshot();
}

void fe_rewind_set_noland(uint32_t quiet)
{
   rw.quiet = quiet;
}

/* ------------------------------------------------- where not to stop -- */

/* May play not continue from frame t (the machine as frame t left it)?
 * Something outside the snapshots (the cartridge save) was being accessed
 * in one of the `quiet` frames up to and including t, AND it was changed
 * after t: the machine of frame t is then in the middle of a conversation
 * with a thing that is no longer what it was.  Without a change after t
 * the outside is exactly what it was at t, wherever t lies. */
static int noland(uint32_t t)
{
   uint32_t lo, f;
   if (!rw.quiet || t >= rw.last_change)
      return 0;
   lo = t > rw.quiet ? t - rw.quiet : 0;
   if (lo < rw.floor)
      lo = rw.floor;
   if (rw.top > rw.log_mask && lo < rw.top - rw.log_mask)
      lo = rw.top - rw.log_mask;
   for (f = t; f > lo; f--)
      if (rw.log[f & rw.log_mask] & FE_RW_IN_BUSY)
         return 1;
   return 0;
}

/* The control is up: if `now` is a frame play may not continue from, move
 * it to the nearest one it may (the earlier one of two equally near).  One
 * always exists: `top`, where the rewind began. */
static void snap_now(void)
{
   uint32_t t = rw.now, lo = oldest_snapshot() + 1, d, best = t;
   if (!noland(t))
      return;
   for (d = 1; ; d++)
   {
      int any = 0;
      if (t >= lo + d)
      {
         any = 1;
         if (!noland(t - d)) { best = t - d; break; }
      }
      if (t + d <= rw.top && t + d >= t)
      {
         any = 1;
         if (!noland(t + d)) { best = t + d; break; }
      }
      if (!any)
         break;
   }
   if (best == t)
   {
      rw.st.anomalies++;               /* (top can always be landed on) */
      best = rw.top;
   }
   if (best > t)
      rw.st.snaps_fwd++;
   else
      rw.st.snaps_back++;
   if ((best > t ? best - t : t - best) > rw.st.snap_max)
      rw.st.snap_max = best > t ? best - t : t - best;
   rw.now = best;
   rw.snapped = 1;
   /* The pictures kept are of the frames around the old `now`. */
   rw.seg[0].valid = rw.seg[1].valid = 0;
}

/* -------------------------------------------------------------- windows -- */

/* The buffer that holds the picture of frame `f`, or NULL. */
static rw_seg *seg_holding(uint32_t f)
{
   int s;
   for (s = 0; s < 2; s++)
      if (rw.seg[s].valid && f > rw.seg[s].start &&
          f - rw.seg[s].start <= rw.seg[s].done)
         return &rw.seg[s];
   return NULL;
}

static rw_seg *seg_other(rw_seg *s)
{
   return s == &rw.seg[0] ? &rw.seg[1] : &rw.seg[0];
}

/* x is the window that ends exactly where s starts. */
static int seg_is_pred_of(const rw_seg *x, const rw_seg *s)
{
   return x->valid && x->start + x->span == s->start;
}

/* The buffer frame `f` (the next one the machine runs, going forward) is
 * captured into.  One is made when none covers f: the buffer given up is
 * the one that does not hold the frame on screen. */
static rw_seg *seg_for(uint32_t f)
{
   rw_seg *w, *o;
   int s;
   for (s = 0; s < 2; s++)
      if (rw.seg[s].valid && f > rw.seg[s].start &&
          f - rw.seg[s].start <= rw.seg[s].span)
         return &rw.seg[s];
   o = seg_holding(rw.now);
   w = o ? seg_other(o) : (rw.seg[0].valid ? &rw.seg[1] : &rw.seg[0]);
   o = seg_other(w);
   w->valid = 1;
   w->start = f - 1;
   w->span  = rw.cfg.interval;
   w->done  = 0;
   /* Up to the next window, if there is one ahead: then this one is exactly
    * its predecessor. */
   if (o->valid && o->start > w->start && o->start - w->start < w->span)
      w->span = o->start - w->start;
   return w;
}

/* Run the machine one frame forward with the logged input.  Its picture is
 * captured into `s` when it is exactly the next one `s` lacks; any other
 * frame (one `s` already has, or one on the way to it) is run silently. */
/* Does frame f keep a picture when it is re-run for the window? */
static int wants_picture(uint32_t f)
{
   return rw.stride <= 1 || f % rw.stride == rw.phase % rw.stride;
}

/* masked: apply the picture stride (the windows a held rewind prepares). */
static void run_logged(rw_seg *s, int masked)
{
   uint32_t f = rw.mach + 1;
   uint64_t t0 = now_us();
   int pictured = 0;

   if (rw.context)
      rw.cfg.replay_context(rw.context[f & rw.log_mask]);

   if (s && s->valid && rw.mach == s->start + s->done && s->done < s->span)
   {
      uint32_t i = s->done, n = 0;
      pictured = 1;
      if (masked && !wants_picture(f))
      {
         pictured = 0;
         /* in the window, but nobody will look at it */
         rw.cfg.run(rw.log[f & rw.log_mask] & FE_RW_IN_KEYS, NULL, NULL, NULL);
         n = FE_RW_NOPIC;
      }
      else
      {
         rw.cfg.run(rw.log[f & rw.log_mask] & FE_RW_IN_KEYS,
                    s->pix + (size_t)i * rw.cfg.frame_pixels,
                    s->aud + (size_t)i * rw.cfg.audio_cap * 2, &n);
         rw.tick_pics++;
      }
      {
         uint32_t nopic = n & FE_RW_NOPIC;
         n &= ~FE_RW_NOPIC;
         if (n > rw.cfg.audio_cap)
         {
            n = rw.cfg.audio_cap;
            rw.st.audio_clipped++;
         }
         s->aud_n[i] = n | nopic;
      }
      s->done++;
   }
   else
      rw.cfg.run(rw.log[f & rw.log_mask] & FE_RW_IN_KEYS, NULL, NULL, NULL);
   rw.mach = f;
   rw.tick_frames++;
   rw.st.replayed++;
   if (rw.cfg.now_us)
   {
      uint32_t d = (uint32_t)(rw.cfg.now_us() - t0);
      rw.st.replay_us += d;
      uint32_t *e = pictured ? &rw.run_pic_us : &rw.run_head_us;
      rw.run_us = rw.run_us ? (rw.run_us * 3 + d) / 4 : d;
      *e = *e ? (*e * 3 + d) / 4 : d;
   }
}

void fe_rewind_set_stride(uint32_t stride)
{
   if (!stride)
      stride = 1;
   if (stride != rw.stride)
   {
      rw.stride = stride;
      rw.phase = rw.now;      /* the frames a step of `stride` lands on */
   }
}

void fe_rewind_set_target(uint32_t seconds, uint32_t gap_max)
{
   rw.target_s = seconds;
   rw.gap_max = gap_max > rw.cfg.interval ? gap_max : rw.cfg.interval;
   if (rw.gap_max > rw.cfg.max_span)
      rw.gap_max = rw.cfg.max_span;
   rw.gap = rw.cfg.interval;
   rw.snap_bytes = 0;
}

uint32_t fe_rewind_gap(void)
{
   return rw.target_s ? rw.gap : rw.cfg.interval;
}

void fe_rewind_set_budget(uint32_t us)
{
   rw.cfg.tick_budget_us = us;
}

void fe_rewind_taint(void)
{
   rw.tainted = 1;
}

/* --------------------------------------------------------- normal play -- */

int fe_rewind_play_slot(uint16_t **pix, int16_t **aud)
{
   rw_seg *w;
   uint32_t i;

   if (!rw.ready || rw.fault || !rw.have_ref || rw.mach != rw.now || rw.tainted)
      return 0;
   snapshot_if_due();           /* a boundary a rewind stopped exactly on */
   w = seg_for(rw.mach + 1);
   i = rw.mach - w->start;      /* slot of frame mach + 1 */
   if (i >= w->span || i > w->done)
      return 0;
   *pix = w->pix + (size_t)i * rw.cfg.frame_pixels;
   *aud = w->aud + (size_t)i * rw.cfg.audio_cap * 2;
   return 1;
}

void fe_rewind_frame(uint32_t input, uint32_t aud_n)
{
   rw_seg *w, *x;
   uint32_t i;

   if (!rw.ready)
      return;
   if (!rw.have_ref)
   {
      /* The frame just run started from a state nobody kept: the history
       * begins after it. */
      rw.mach++;
      rw.now = rw.top = rw.mach;
      rw.log[rw.mach & rw.log_mask] = (uint16_t)(input & ~FE_RW_IN_CHANGE);
      if (rw.context)
         rw.context[rw.mach & rw.log_mask] = rw.cfg.frame_context();
      rw.floor = rw.mach - 1;
      rw.last_change = rw.mach;
      fe_rewind_gather();
      fe_rewind_pack();
      return;
   }
   if (rw.mach != rw.now)
   {
      rw.st.anomalies++;        /* a frame was run without FE_RW_PLAY */
      fe_rewind_reset(rw.mach + 1);
      return;
   }
   w = seg_for(rw.mach + 1);
   i = rw.mach - w->start;
   rw.mach++;
   rw.now = rw.top = rw.mach;
   rw.gesture = 0;
   rw.log[rw.mach & rw.log_mask] = (uint16_t)(input & ~FE_RW_IN_CHANGE);
   if (rw.context)
      rw.context[rw.mach & rw.log_mask] = rw.cfg.frame_context();
   /* Play has gone on from here: whatever was changed in a future that was
    * abandoned stays changed (min), and so does what this frame changed. */
   if (rw.last_change > rw.mach - 1)
      rw.last_change = rw.mach - 1;
   if (input & FE_RW_IN_CHANGE)
      rw.last_change = rw.mach;
   if (i < w->span && i <= w->done)
   {
      uint32_t nopic = aud_n & FE_RW_NOPIC;
      aud_n &= ~FE_RW_NOPIC;
      if (aud_n > rw.cfg.audio_cap)
      {
         aud_n = rw.cfg.audio_cap;
         rw.st.audio_clipped++;
      }
      w->aud_n[i] = aud_n | nopic;
      w->done = i + 1;          /* and nothing after it: that future is gone */
   }
   /* A buffer of a LATER window is pictures of the abandoned future. */
   x = seg_other(w);
   if (x->valid && x->start > w->start)
      x->valid = 0;
   snapshot_if_due();
}

void fe_rewind_hint_busy(int busy)
{
   rw.busy = busy;
}

uint32_t fe_rewind_logged_input(uint32_t frame)
{
   return rw.ready ? rw.log[frame & rw.log_mask] & FE_RW_IN_KEYS : 0;
}

/* -------------------------------------------------------------- rewind -- */

/* Held: show the frame before `now`. */
static int show_prev(fe_rewind_shown *out)
{
   rw_seg *s, *from;
   uint32_t f, i, n, k;
   const int16_t *a;

   if (rw.now <= oldest_snapshot() + 1)
   {
      rw.st.ticks_limit++;
      rw.at_limit = 1;
      return FE_RW_HOLD;               /* the start of the history */
   }
   f = rw.now - 1;
   s = seg_holding(rw.now);
   if (!s)
   {
      rw.st.anomalies++;
      return FE_RW_HOLD;
   }
   if (f > s->start)
      from = s;
   else
   {
      /* f is the last frame of the window before. */
      from = seg_other(s);
      if (!seg_is_pred_of(from, s) || from->done != from->span)
      {
         rw.st.ticks_stall++;
         return FE_RW_HOLD;            /* still being re-run: next tick */
      }
   }
   i = f - from->start - 1;
   rw.now = f;
   rw.gesture = 1;

   a = from->aud + (size_t)i * rw.cfg.audio_cap * 2;
   n = from->aud_n[i] & ~FE_RW_NOPIC;
   for (k = 0; k < n; k++)
   {
      rw.rev[k * 2]     = a[(n - 1 - k) * 2];
      rw.rev[k * 2 + 1] = a[(n - 1 - k) * 2 + 1];
   }
   if (out)
   {
      out->frame = f;
      out->pix_frame = f;
      out->pix   = (from->aud_n[i] & FE_RW_NOPIC) ? NULL
                   : from->pix + (size_t)i * rw.cfg.frame_pixels;
      out->aud   = rw.rev;
      out->aud_n = n;
   }
   rw.st.ticks_show++;
   return FE_RW_SHOW;
}

/* May this tick re-run another frame?  Only if one more, at what they have
 * been costing, still ends inside the budget. */
static int tick_room(int pictured)
{
   uint32_t c = pictured ? rw.run_pic_us : rw.run_head_us;
   if (!rw.cfg.tick_budget_us || !rw.cfg.now_us)
      return 1;
   if (!c)
      c = rw.run_us;
   return rw.cfg.now_us() - rw.tick_t0 + c < rw.cfg.tick_budget_us;
}

/* Held: get the window before the one `now` is in ready, a bounded piece at
 * a time. */
static void held_work(int eager)
{
   rw_seg *s = seg_holding(rw.now), *x;
   uint32_t k, target;

   if (!s)
      return;
   x = seg_other(s);
   if (!seg_is_pred_of(x, s))
   {
      uint32_t end = s->start, from;
      /* Not started.  Leave the machine where it is until `now` is close to
       * the start of its window: a short tap then never sends it further
       * back than it has to go anyway.  That is for taps only.  A rewind
       * that has gone back a few frames, or a platform that says the
       * control is going to bite (a held tick with step 0), starts at once:
       * the window before takes as many ticks to re-run as this one takes
       * to show when the game is heavy, and every tick it starts late is a
       * tick the picture stands still at the boundary. */
      if (rw.now - s->start > rw.margin && !eager && rw.top - rw.now < 3)
         return;
      from = snapshot_below(end);
      if (from >= end)
         return;                       /* nothing older: the limit */
      /* From the newest snapshot below, when that is within a buffer's
       * length (then nothing has to be run silently first). */
      if (end - from > rw.cfg.interval)
         from = end - rw.cfg.interval;
      x->valid = 1;
      x->start = from;
      x->span  = end - from;
      x->done  = 0;
   }
   if (x->done >= x->span)
      return;                          /* ready */

   /* The machine captures x's next frame when it stands at `target`; from
    * anywhere before that it simply runs forward (it is the same timeline).
    * Only from beyond it does it have to go back to a snapshot. */
   target = x->start + x->done;
   if (rw.mach > target)
   {
      while (rw.ref_frame > x->start)
         if (!step_back())
            break;
      if (rw.ref_frame > x->start)
      {
         rw.st.anomalies++;
         x->valid = 0;
         return;
      }
      if (restore_ref() != 0)
         return;
   }
   /* (a tick that restored runs a frame as well only if both fit) */
   for (k = 0; k < rw.cfg.burst && x->done < x->span &&
               ((!k && !rw.tick_restores) ||
                tick_room(rw.mach >= x->start && wants_picture(rw.mach + 1)));
        k++)
   {
      /* One frame with a picture a tick: two would have the platform's
       * renderer wait for the first before it can take the second. */
      if (rw.tick_pics && rw.mach >= x->start && wants_picture(rw.mach + 1) &&
          (rw.stride > 1 || rw.cfg.tick_budget_us))
         break;
      /* On the way to the window from a snapshot far below it (history
       * recorded with snapshots far apart: fast-forward, frames that had no
       * time for one): leave snapshots behind every `interval` frames.  The
       * windows before this one then start from one of those instead of
       * making the whole way again. */
      if (rw.mach < x->start && rw.mach - rw.ref_frame >= rw.cfg.interval &&
          !rw.tainted)
      {
         fe_rewind_gather();
         fe_rewind_pack();
         rw.st.resnaps++;
      }
      run_logged(x, 1);
   }
}

/* Released with the machine somewhere else: bring it to `now`. */
static void release_work(void)
{
   uint32_t k;

   if (rw.mach > rw.now || rw.tainted)
   {
      /* It can only run forward: back to the snapshot at or before `now`.
       * The snapshots stepped over here are the abandoned future.  (A
       * machine that is behind `now` but tainted goes back too: play must
       * continue from frames run in full.) */
      while (rw.ref_frame > rw.now)
         if (!step_back())
            break;
      if (rw.ref_frame > rw.now)
      {
         rw.st.anomalies++;
         rw.now = rw.ref_frame;
      }
      if (restore_ref() != 0)
         return;
   }
   for (k = 0; k < rw.cfg.burst && rw.mach < rw.now && (!k || tick_room(1)); k++)
   {
      snapshot_if_due();               /* the ones it passes, again */
      run_logged(seg_for(rw.mach + 1), 0);
   }
   snapshot_if_due();
}

int fe_rewind_tick(int held, fe_rewind_shown *out)
{
   return fe_rewind_tick_step(held, 1, out, NULL);
}

int fe_rewind_tick_step(int held, uint32_t step, fe_rewind_shown *out,
                        uint32_t *stepped)
{
   uint64_t t0;
   int r, engage = 0;

   if (stepped)
      *stepped = 0;

   if (rw.fault)
      return FE_RW_ERROR;

   if (!rw.ready || !rw.have_ref)
      return FE_RW_PLAY;               /* no history: the control does nothing */
   if (rw.snapped)
      held = 0;                        /* first arrive where it was sent */
   if (!held && rw.gesture)
      snap_now();
   if (!held && rw.mach == rw.now && !rw.tainted)
   {
      rw.snapped = 0;
      rw.at_limit = 0;
      rw.catchup_run = 0;
      snapshot_if_due();               /* stopped exactly on a boundary */
      return FE_RW_PLAY;
   }

   t0 = now_us();
   rw.tick_t0 = t0;
   rw.tick_pics = 0;
   rw.tick_frames = rw.tick_restores = 0;
   rw.at_limit = 0;
   fe_rewind_pack();
   if (held)
   {
      if (!rw.gesture && rw.mach == rw.now && !rw.tainted)
      {
         rw.top = rw.now;
         rw.st.engages++;
         engage = 1;
      }
      rw.catchup_run = 0;
      if (!step)
      {
         /* a tick between two pictures: nothing new, but the work goes on */
         held_work(1);
         r = FE_RW_HOLD;
         goto counted;
      }
      r = show_prev(out);
      if (r == FE_RW_SHOW)
      {
         /* Further back in the same tick: each step that succeeds replaces
          * the picture; the first that cannot is simply where it stops. */
         uint32_t n = 1, st0 = rw.st.ticks_stall, lm0 = rw.st.ticks_limit;
         fe_rewind_shown more;
         while (n < step && show_prev(&more) == FE_RW_SHOW)
         {
            /* the frame is where the timeline now is; the picture is the
             * last one there was */
            if (out)
            {
               out->frame = more.frame;
               out->aud = more.aud;
               out->aud_n = more.aud_n;
               if (more.pix)
               {
                  out->pix = more.pix;
                  out->pix_frame = more.frame;
               }
            }
            n++;
         }
         /* (a further step that was not possible is not a held picture) */
         rw.st.ticks_stall = st0;
         rw.st.ticks_limit = lm0;
         if (out && out->pix)
         {
            /* A picture kept from a frame passed on the way is only good
             * while its buffer is not the one about to be re-used: that is
             * the buffer `now` is NOT in. */
            rw_seg *cur = seg_holding(rw.now);
            size_t span = (size_t)rw.cfg.interval * rw.cfg.frame_pixels;
            if (!cur || out->pix < cur->pix || out->pix >= cur->pix + span)
               out->pix = NULL;
         }
         /* The frames a step lands on are the ones worth a picture: follow
          * where the steps really fall (a first press at any frame, a step
          * cut short). */
         if (rw.stride > 1 && !wants_picture(rw.now))
            rw.phase = rw.now;
         /* (a stop met on a later step is not this tick's result) */
         rw.at_limit = 0;
         if (stepped)
            *stepped = n;
      }
      held_work(0);
   }
   else
   {
      release_work();
      rw.st.ticks_catchup++;
      if (++rw.catchup_run > rw.st.catchup_run_max)
         rw.st.catchup_run_max = rw.catchup_run;
      r = FE_RW_HOLD;
   }

counted:
   if (rw.tick_frames > rw.st.tick_frames_max)
      rw.st.tick_frames_max = rw.tick_frames;
   if (rw.tick_restores > rw.st.tick_restores_max)
      rw.st.tick_restores_max = rw.tick_restores;
   if (rw.cfg.now_us)
   {
      uint32_t d = (uint32_t)(rw.cfg.now_us() - t0);
      if (d > rw.st.tick_max_us)
         rw.st.tick_max_us = d;
      if (engage)
      {
         rw.st.engage_us += d;
         if (d > rw.st.engage_max_us)
            rw.st.engage_max_us = d;
      }
   }
   return rw.fault ? FE_RW_ERROR : r;
}

/* ---------------------------------------------------------------- facts -- */

/* Does the MACHINE (not `now`) stand on a frame play may not continue from?
 * For whoever has to end the history without the control having landed
 * (fe_host.c, "ending the history in the middle of a rewind").  The frame
 * the rewind began at is the present: always fine. */
int fe_rewind_mach_noland(void)
{
   return rw.fault ||
          (rw.ready && rw.have_ref && rw.mach != rw.top && noland(rw.mach));
}

int fe_rewind_active(void)
{
   return rw.ready && rw.have_ref &&
          (rw.gesture || rw.mach != rw.now || rw.tainted);
}

int fe_rewind_at_limit(void)
{
   return rw.ready && rw.at_limit;
}

uint32_t fe_rewind_frame_now(void)
{
   return rw.now;
}

uint32_t fe_rewind_frame_mach(void)
{
   return rw.mach;
}

uint32_t fe_rewind_frame_top(void)
{
   return rw.top;
}

uint32_t fe_rewind_frame_oldest(void)
{
   uint32_t f;
   if (!rw.ready || !rw.have_ref)
      return rw.now;
   f = oldest_snapshot() + 1;
   return f > rw.now ? rw.now : f;
}

uint32_t fe_rewind_ring_used(void)
{
   return rw.used;
}

uint32_t fe_rewind_ring_records(void)
{
   return rw.rec_n;
}

const fe_rewind_stats *fe_rewind_get_stats(void)
{
   return &rw.st;
}

void fe_rewind_window_reset(void)
{
   rw.st.gather_win_max_us = rw.st.pack_win_max_us = 0;
}

int fe_rewind_selfcheck(void)
{
   uint32_t k, sum = 0, prev = 0, at;

   if (!rw.ready || !rw.have_ref)
      return 0;
   if (rw.pack_pending)
      return 0;                        /* mid-snapshot: nothing to assert */
   at = rw.rec_n ? rw.rec[rw.rec_tail].off : rw.head;
   for (k = 0; k < rw.rec_n; k++)
   {
      const fe_rewind_rec *e = &rw.rec[(rw.rec_tail + k) % rw.rec_cap];
      if (k && e->base <= prev)
         return 1;                     /* snapshots out of order */
      if (e->off != at)
         return 2;                     /* records not contiguous */
      at = (at + e->len) % rw.ring_bytes;
      prev = e->base;
      sum += e->len;
   }
   if (sum != rw.used)
      return 3;                        /* byte accounting */
   if (at != rw.head && !(rw.rec_n == 0))
      return 4;                        /* head is not the end of the newest */
   if (rw.rec_n && prev >= rw.ref_frame)
      return 5;                        /* a delta at or after the reference */
   if (rw.ref_frame > rw.mach)
      return 6;                        /* the reference is ahead of the machine */
   if (rw.mach == rw.now && !rw.gesture &&
       rw.mach - rw.ref_frame > rw.cfg.max_span)
      return 7;                        /* a due snapshot was not taken */
   if (rw.st.anomalies)
      return 9;
   return 0;
}
