/* audio_vita.c -- see audio_vita.h.  Portable C; the port and the thread are
 * plat.h's.
 *
 * One producer (the emulation thread) and one consumer (the audio thread)
 * share a ring of input-rate frames.  Each owns one index; the other only
 * reads it.  The Vita has four cores and ARMv7's memory ordering is weak, so
 * the indices are published with release/acquire atomics (a `dmb`), not
 * plain volatile stores as on the single-core PSP: without the barrier the
 * consumer may see the new write index before the samples it covers.
 */
#include "audio_vita.h"
#include "plat.h"

#include <string.h>

#define RING_FRAMES  16384u               /* 0.5 s at 32768 Hz, power of two */
#define RING_MASK    (RING_FRAMES - 1u)
#define FADE_FRAMES  64                   /* 1.3 ms: a gap fades, never clicks */
#define FADE_STEP    (65536 / FADE_FRAMES)

static int16_t  g_ring[RING_FRAMES * 2];
static uint32_t g_w;                      /* producer's index (frames, free-running) */
static uint32_t g_r;                      /* consumer's index                        */

#define LOAD(v)      __atomic_load_n(&(v), __ATOMIC_ACQUIRE)
#define STORE(v, x)  __atomic_store_n(&(v), (x), __ATOMIC_RELEASE)

static int      g_running;                /* audio thread should keep going   */
static int      g_open;                   /* session still owns its resources */
/* The latest submitted window belongs to the port until a later successful
 * write retires it. Both windows also survive a timed-out thread stop. */
static int16_t  g_out[2][AUDIO_GRAIN * 2] __attribute__((aligned(64)));
static unsigned g_out_next;
static int      g_recovering;             /* retire uncertain failed writes  */
static unsigned g_in_rate;
static uint32_t g_target;                 /* input frames the follow holds:
                                           * written by the producer, read
                                           * by the consumer (atomic)        */
static uint32_t g_target_min, g_target_max, g_target_step;
static unsigned g_under_seen, g_calm;     /* the producer's: gaps seen, frames
                                           * since the last one              */

/* Input frames per output frame, 2.30 fixed point. The supported rate/speed
 * range plus follow fits below 4, and ARMv7 only needs a 32-bit atomic. */
static uint32_t g_step;
static uint64_t g_step_base;              /* before the follow's correction   */
static int      g_follow_ppm;
static uint32_t g_level_ema16;            /* smoothed level, 28.4 fixed point */

/* Requests from the emulation thread to the consumer, which owns g_r. */
static int      g_flush_req, g_trim_req;

/* Consumer state. */
static uint64_t g_phase;                  /* 32.32: fraction between frames   */
static int      g_primed;                 /* playing (0 = waiting for cushion)*/
static int      g_fade = 0;               /* 16.16 gain                       */
static int      g_last_l, g_last_r;
static int      g_tail_l, g_tail_r;       /* last emitted (after fades)       */
static int      g_trim_l, g_trim_r;
static unsigned g_trim_left;             /* bounded dezipper after a skip    */

static unsigned g_underruns, g_starved, g_dropped, g_trims, g_port_errors;

/* ---- consumer ---------------------------------------------------------- */

/* Render `n` output frames.  Linear interpolation between neighbouring input
 * frames: the source is band-limited to 16 kHz and the output is 48 kHz, so
 * the images land above hearing; the PSP frontend does the same. */
static void render(int16_t *out, unsigned n)
{
   uint32_t w = LOAD(g_w);
   uint32_t r = g_r;
   uint32_t step = LOAD(g_step);
   unsigned i;

   if (LOAD(g_flush_req))
   {
      STORE(g_flush_req, 0);
      r = w;
      g_phase = 0;
      g_primed = 0;
      g_last_l = g_tail_l;
      g_last_r = g_tail_r;
      g_fade = 65536;
      g_trim_left = 0;
      STORE(g_trim_req, 0);
   }
   if (LOAD(g_trim_req))
   {
      STORE(g_trim_req, 0);
      const uint32_t t = LOAD(g_target);
      if (w - r > t)
      {
         r = w - t;                       /* a skip, once, instead of latency */
         g_trim_l = g_tail_l;
         g_trim_r = g_tail_r;
         g_trim_left = FADE_FRAMES;
         g_trims++;
      }
   }

   for (i = 0; i < n; i++)
   {
      uint32_t avail = w - r;
      /* Start (and restart after a gap) only with a cushion in the ring: a
       * consumer that resumes on the first sample runs dry again at once. */
      if (!g_primed && avail >= LOAD(g_target) / 2)
         g_primed = 1;
      if (!g_primed || avail < 2)
      {
         if (g_primed)
         {
            g_primed = 0;
            g_underruns++;
            /* A trim or fade-in can make the emitted tail differ from the
             * raw input. Begin the gap's fade at what actually played. */
            g_last_l = g_tail_l;
            g_last_r = g_tail_r;
            g_fade = 65536;
         }
         /* the last sample, fading to silence */
         g_fade = g_fade > FADE_STEP ? g_fade - FADE_STEP : 0;
         out[i * 2 + 0] = (int16_t)((g_last_l * g_fade) >> 16);
         out[i * 2 + 1] = (int16_t)((g_last_r * g_fade) >> 16);
         g_tail_l = out[i * 2 + 0];
         g_tail_r = out[i * 2 + 1];
         g_trim_left = 0;
         g_starved++;
         continue;
      }
      {
         uint32_t i0 = r & RING_MASK, i1 = (r + 1) & RING_MASK;
         int l0 = g_ring[i0 * 2 + 0], l1 = g_ring[i1 * 2 + 0];
         int r0 = g_ring[i0 * 2 + 1], r1 = g_ring[i1 * 2 + 1];
         int f = (int)((g_phase >> 16) & 0xFFFF);
         /* 64-bit product: |l1 - l0| and f both reach 65535 */
         int sl = l0 + (int)(((int64_t)(l1 - l0) * f) >> 16);
         int sr = r0 + (int)(((int64_t)(r1 - r0) * f) >> 16);
         g_last_l = sl;
         g_last_r = sr;
         if (g_fade < 65536)
         {
            /* back from a gap: fade in rather than jump */
            g_fade = g_fade + FADE_STEP < 65536 ? g_fade + FADE_STEP : 65536;
            sl = (sl * g_fade) >> 16;
            sr = (sr * g_fade) >> 16;
         }
         if (g_trim_left)
         {
            /* Start at the actual last output, not the raw pre-fade sample.
             * Blend into the new timeline without an extra queued grain. */
            unsigned fresh = FADE_FRAMES - g_trim_left;
            sl = (g_trim_l * (int)g_trim_left + sl * (int)fresh) / FADE_FRAMES;
            sr = (g_trim_r * (int)g_trim_left + sr * (int)fresh) / FADE_FRAMES;
            g_trim_left--;
         }
         out[i * 2 + 0] = (int16_t)sl;
         out[i * 2 + 1] = (int16_t)sr;
         g_tail_l = sl;
         g_tail_r = sr;
      }
      g_phase += (uint64_t)step << 2;      /* preserve the 32.32 phase */
      r += (uint32_t)(g_phase >> 32);
      g_phase &= 0xFFFFFFFFu;
      if ((int32_t)(w - r) < 0)
         r = w;                           /* never pass the producer */
   }
   STORE(g_r, r);
}

/* One grain: the audio thread's whole loop body (plat_audio_thread_start). */
static int audio_step(void)
{
   if (!LOAD(g_running))
      return 1;
   if (!g_recovering)
   {
      render(g_out[g_out_next], AUDIO_GRAIN);
      if (plat_audio_write(g_out[g_out_next]) >= 0)
      {
         g_out_next ^= 1u;
         return 0;
      }
      g_port_errors++;
      g_recovering = 1;
   }
   /* A failed submission is not a lifetime guarantee for either window.
    * Do not render again until checked close has retired the old port.
    * Sleep on every recovery attempt: this thread outranks emulation. */
   if (!plat_audio_close_checked())
   {
      plat_sleep_us(100000);
      return 0;
   }
   plat_sleep_us(100000);
   if (!LOAD(g_running))
      return 1;
   if (plat_audio_open(AUDIO_OUT_RATE, AUDIO_GRAIN) < 0)
      plat_sleep_us(400000);
   else
      g_recovering = 0;
   return 0;
}

/* ---- producer ---------------------------------------------------------- */

static void set_step(int ppm)
{
   uint64_t s = g_step_base + (int64_t)g_step_base * ppm / 1000000;
   g_follow_ppm = ppm;
   STORE(g_step, (uint32_t)s);
}

int audio_start(unsigned in_rate, unsigned speed_x1e6)
{
   in_rate = in_rate ? in_rate : 32768u;
   speed_x1e6 = speed_x1e6 ? speed_x1e6 : 1000000u;
   if (in_rate < 8000u || in_rate > 65536u ||
       speed_x1e6 < 500000u || speed_x1e6 > 2000000u)
      return -1;
   /* A timed-out consumer still owns this ring and the output windows.
    * Never reset even one shared flag until checked stop has retired it. */
   if (g_open)
   {
      audio_stop();
      if (g_open)
         return -1;
   }
   g_in_rate = in_rate;
   g_target_min  = g_in_rate * AUDIO_TARGET_MS / 1000u;
   g_target_max  = g_in_rate * AUDIO_TARGET_MAX_MS / 1000u;
   g_target_step = g_in_rate * AUDIO_TARGET_STEP_MS / 1000u;
   g_target  = g_target_min;
   g_under_seen = 0;
   g_calm = 0;
   /* in/out as 2.30, times how fast the emulation is paced */
   g_step_base = (((uint64_t)g_in_rate << 30) / AUDIO_OUT_RATE) *
                 speed_x1e6 / 1000000u;
   g_level_ema16 = g_target << 4;
   set_step(0);
   STORE(g_w, 0);
   STORE(g_r, 0);
   g_phase = 0;
   g_primed = 0;
   g_fade = 0;
   g_last_l = g_last_r = 0;
   g_tail_l = g_tail_r = 0;
   g_trim_l = g_trim_r = 0;
   g_trim_left = 0;
   g_out_next = 0;
   g_recovering = 0;
   g_flush_req = g_trim_req = 0;
   g_underruns = g_starved = g_dropped = g_trims = g_port_errors = 0;

   if (plat_audio_open(AUDIO_OUT_RATE, AUDIO_GRAIN) < 0)
      return -1;
   g_open = 1;
   STORE(g_running, 1);
   if (plat_audio_thread_start(audio_step) != 0)
   {
      /* Failed native start can still leave a thread UID to retire. */
      audio_stop();
      return -1;
   }
   return 0;
}

void audio_stop(void)
{
   if (!g_open)
      return;
   STORE(g_running, 0);
   if (!plat_audio_thread_stop_checked())
      return;
   if (plat_audio_close_checked())
      g_open = 0;
}

void audio_push(const int16_t *lr, size_t frames)
{
   uint32_t w, r;
   size_t i;

   if (!LOAD(g_running))
      return;          /* no consumer: a ring nobody drains would only fill */
   w = g_w;
   r = LOAD(g_r);
   for (i = 0; i < frames; i++)
   {
      uint32_t idx;
      if (w - r >= RING_FRAMES - 1)
      {
         g_dropped += (unsigned)(frames - i);
         break;
      }
      idx = w & RING_MASK;
      g_ring[idx * 2 + 0] = lr[i * 2 + 0];
      g_ring[idx * 2 + 1] = lr[i * 2 + 1];
      w++;
   }
   STORE(g_w, w);
}

void audio_frame(void)
{
   uint32_t level;
   int32_t err;
   int ppm;

   if (!LOAD(g_running))
      return;
   level = g_w - LOAD(g_r);
   if (level > RING_FRAMES)
      level = 0;                          /* a flush in flight */

   /* A gap: the cushion was too thin for this game on this console.  Grow
    * it (a few more ms of delay, which no one hears; a break in the music
    * everyone does), and let it shrink back once things have been calm for
    * half a minute. */
   {
      const unsigned under = LOAD(g_underruns);
      if (under != g_under_seen)
      {
         g_under_seen = under;
         g_calm = 0;
         if (g_target + g_target_step <= g_target_max)
            STORE(g_target, g_target + g_target_step);
      }
      else if (++g_calm >= 1800u && g_target > g_target_min)
      {
         uint32_t less = g_in_rate * 5u / 1000u;
         g_calm = 0;
         STORE(g_target, g_target - less > g_target_min ? g_target - less
                                                        : g_target_min);
      }
   }

   /* A backlog the follow could only drain at 1 % -- what a burst of
    * catch-up frames leaves -- would sit there as latency for seconds. */
   if (level > g_target * 3u)
      STORE(g_trim_req, 1);

   /* The raw level is a sawtooth (a whole frame of sound arrives at once);
    * smooth it over about eight frames before steering by it. */
   g_level_ema16 += (int32_t)((level << 4) - g_level_ema16) / 8;
   err = (int32_t)(g_level_ema16 >> 4) - (int32_t)g_target;
   /* Proportional: full correction when the fill is off by the target itself
    * (empty, or twice the cushion). */
   ppm = (int)((int64_t)err * AUDIO_FOLLOW_PPM / (int32_t)g_target);
   if (ppm > AUDIO_FOLLOW_PPM)  ppm = AUDIO_FOLLOW_PPM;
   if (ppm < -AUDIO_FOLLOW_PPM) ppm = -AUDIO_FOLLOW_PPM;
   set_step(ppm);
}

void audio_flush(void)
{
   g_level_ema16 = g_target << 4;
   STORE(g_flush_req, 1);
}

void audio_get_stats(audio_stats *out)
{
   uint32_t level = g_w - LOAD(g_r);
   memset(out, 0, sizeof(*out));
   out->in_rate     = g_in_rate;
   out->level       = level > RING_FRAMES ? 0 : level;
   out->level_avg   = g_level_ema16 >> 4;
   out->target      = g_target;
   out->follow_ppm  = g_follow_ppm;
   out->underruns   = g_underruns;
   out->starved     = g_starved;
   out->dropped     = g_dropped;
   out->trims       = g_trims;
   out->port_errors = g_port_errors;
}
