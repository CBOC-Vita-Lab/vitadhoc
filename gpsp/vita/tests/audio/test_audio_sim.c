/* Real-time audio simulation: the real audio_vita.c ring, resampler and rate
 * follow driven by a virtual clock -- a producer that pushes one emulated
 * frame of a 440 Hz tone per display refresh (with jitter, stalls, pauses and
 * fast forward) and a consumer that takes one grain every 10.67 ms.  The
 * output is checked for dropouts, clicks, runaway latency and wrapped
 * samples.  No device, no ROM.  Prints the measurements. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../audio_vita.c"

int plat_audio_open(unsigned rate, unsigned grain) { (void)rate; (void)grain; return 0; }
int plat_audio_write(const int16_t *stereo) { (void)stereo; return 0; }
int plat_audio_close_checked(void) { return 1; }
void plat_audio_close(void) {}
void plat_sleep_us(uint32_t us) { (void)us; }
int plat_audio_thread_start(int (*step)(void)) { (void)step; return 0; }
int plat_audio_thread_stop_checked(void) { return 1; }
void plat_audio_thread_stop(void) {}

static unsigned failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #c); failures++; } } while (0)

#define GBA_HZ 59.7275
#define IN_RATE 32768u

typedef struct { double refresh_hz, jitter_ms; } sim_cfg;

typedef struct
{
   unsigned grains, clicks, wraps, silent_after_start;
   int max_step;
   unsigned underruns, starved, dropped, trims, target, target_max;
} sim_out;

/* Producer events: [from_s, to_s) of "pause" (nothing pushed, no audio_frame)
 * or "ff" (same, then a flush request as the app does when it ends). */
typedef struct { double from_s, to_s; int kind; } sim_gap;   /* 1 pause, 2 ff */

static double tone_phase;
static void push_frame(unsigned n, int amp)
{
   int16_t buf[1200 * 2];
   unsigned i;
   for (i = 0; i < n; i++)
   {
      int v = (int)(amp * sin(tone_phase));
      tone_phase += 6.283185307179586 * 440.0 / IN_RATE;
      buf[i * 2] = buf[i * 2 + 1] = (int16_t)v;
   }
   audio_push(buf, n);
}

static double rnd(void) { return (double)rand() / RAND_MAX; }

typedef void (*gap_hook)(int starting, int kind);

static sim_out run(double seconds, const sim_cfg *cfg, const sim_gap *gaps,
                   unsigned ngaps, double stall_at, double stall_ms,
                   gap_hook hook)
{
   sim_out o;
   const double grain_us = 1e6 * AUDIO_GRAIN / AUDIO_OUT_RATE;
   const double frame_us = 1e6 / cfg->refresh_hz;
   double next_grain = 0, next_frame = 0, now = 0;
   double acc = 0;
   int16_t out[AUDIO_GRAIN * 2];
   int prev = 0, have_prev = 0, was_gap = 0, started = 0;
   unsigned g_idx;
   const unsigned amp = 12000;

   memset(&o, 0, sizeof(o));
   tone_phase = 0;
   srand(12345);
   audio_stop();
   if (audio_start(IN_RATE, (unsigned)(cfg->refresh_hz / GBA_HZ * 1e6 + 0.5)))
   { puts("audio_start failed"); exit(2); }
   while (now < seconds * 1e6)
   {
      if (next_frame <= next_grain)
      {
         unsigned n, k;
         int in_gap = 0, kind = 0;
         now = next_frame;
         for (k = 0; k < ngaps; k++)
            if (now >= gaps[k].from_s * 1e6 && now < gaps[k].to_s * 1e6)
               in_gap = 1, kind = gaps[k].kind;
         if (in_gap != was_gap)
         {
            if (hook) hook(in_gap, kind ? kind : 1);
            was_gap = in_gap;
         }
         if (!in_gap)
         {
            acc += (double)IN_RATE / GBA_HZ;
            n = (unsigned)acc;
            acc -= n;
            push_frame(n, (int)amp);
            audio_frame();
            started = 1;
         }
         next_frame += frame_us;
         if (stall_ms > 0 && now >= stall_at * 1e6 && now - frame_us < stall_at * 1e6)
            next_frame += stall_ms * 1000.0;
         else if (cfg->jitter_ms > 0)
            next_frame += (rnd() * 2 - 1) * cfg->jitter_ms * 1000.0 -
                          0 * frame_us;
         /* keep the long-run rate exact: jitter is a delay of this frame only */
         if (cfg->jitter_ms > 0 && !(stall_ms > 0))
            next_frame = (next_frame > now + frame_us * 0.2) ? next_frame : now + frame_us * 0.2;
      }
      else
      {
         unsigned i;
         now = next_grain;
         render(out, AUDIO_GRAIN);
         o.grains++;
         for (i = 0; i < AUDIO_GRAIN; i++)
         {
            int v = out[i * 2];
            if (have_prev)
            {
               int d = abs(v - prev);
               if (d > o.max_step && now > 1.0e6) o.max_step = d;
               if (now > 1.0e6 && d > 2500) o.clicks++;
               if (now > 1.0e6 && d > 20000 && ((v ^ prev) < 0)) o.wraps++;
            }
            prev = v; have_prev = 1;
         }
         (void)g_idx; (void)started;
         next_grain += grain_us;
      }
   }
   o.underruns = g_underruns; o.starved = g_starved; o.dropped = g_dropped;
   o.trims = g_trims; o.target = g_target;
   audio_stop();
   return o;
}

static void show(const char *name, const sim_out *o)
{
   printf("%-34s underruns=%u starved=%u dropped=%u trims=%u clicks=%u wraps=%u maxstep=%d target=%u (%.0f ms)\n",
          name, o->underruns, o->starved, o->dropped, o->trims, o->clicks,
          o->wraps, o->max_step, o->target, 1000.0 * o->target / IN_RATE);
}

static unsigned g_flushes;
static void hook_ff_flush(int starting, int kind)
{
   (void)kind;
   (void)starting;
   audio_flush();
   g_flushes++;
}

/* A sine of `hz` at the core's rate through the real resampler: the level
 * of everything that is not the sine, relative to it (dB), over a stretch
 * long enough for the rate follow to settle.  Plain DFT at the fitted
 * frequency; no FFT needed. */
static double resample_thdn_db(double hz, int amp, int *peak_out)
{
   int16_t in[600 * 2], out[AUDIO_GRAIN * 2];
   static double d[48000];
   double ph = 0, re = 0, im = 0, tot = 0, sig;
   unsigned n = 0, got = 0, i;
   int peak = 0;
   audio_stop();
   if (audio_start(IN_RATE, 1000000)) { puts("audio_start failed"); exit(2); }
   /* no rate follow: the step stays at the exact 32768 -> 48000 ratio, so
    * the sine comes out at exactly `hz` */
   while (got < 48000)
   {
      while (g_w - g_r < 4000)
      {
         for (i = 0; i < 548; i++)
         {
            in[i * 2] = in[i * 2 + 1] = (int16_t)lrint(amp * sin(ph));
            ph += 6.283185307179586 * hz / IN_RATE;
         }
         audio_push(in, 548);
      }
      render(out, AUDIO_GRAIN);
      n += AUDIO_GRAIN;
      if (n > 24000)
         for (i = 0; i < AUDIO_GRAIN && got < 48000; i++)
         {
            d[got++] = out[i * 2];
            if (abs(out[i * 2]) > peak) peak = abs(out[i * 2]);
         }
   }
   for (i = 0; i < 48000; i++)
   {
      re += d[i] * cos(6.283185307179586 * hz * i / AUDIO_OUT_RATE);
      im += d[i] * sin(6.283185307179586 * hz * i / AUDIO_OUT_RATE);
   }
   re *= 2.0 / 48000; im *= 2.0 / 48000;
   for (i = 0; i < 48000; i++)
   {
      double e = d[i] - (re * cos(6.283185307179586 * hz * i / AUDIO_OUT_RATE) +
                         im * sin(6.283185307179586 * hz * i / AUDIO_OUT_RATE));
      tot += e * e;
   }
   sig = (re * re + im * im) / 2.0;
   *peak_out = peak;
   audio_stop();
   return 10.0 * log10(tot / 48000 / (sig > 0 ? sig : 1));
}

static void test_resampler_fidelity(void)
{
   static const double hz[] = { 1000, 3000, 6000, 9000 };
   /* dB.  Linear interpolation measures -57 / -38 / -25 / -16 (a Catmull-Rom
    * cubic -78 / -52 / -33 / -21, but overshoots square edges by 14%) */
   static const double need[] = { -50, -33, -22, -14 };
   unsigned i;
   for (i = 0; i < 4; i++)
   {
      int peak;
      double db = resample_thdn_db(hz[i], 12000, &peak);
      printf("resampler %5.0f Hz sine: everything else %.1f dB (peak %d)\n", hz[i], db, peak);
      CHECK(db < need[i]);
   }
}

/* A hard-edged full-scale square at the input rate: whatever the interpolator
 * overshoots must clip at the rails, not wrap into the other sign. */
static void test_full_scale_edges(void)
{
   int16_t in[512 * 2], out[AUDIO_GRAIN * 2];
   unsigned i, blocks, wraps = 0;
   int prev = 0, maxabs = 0;
   audio_stop();
   CHECK(audio_start(IN_RATE, 1000000) == 0);
   for (blocks = 0; blocks < 40; blocks++)
   {
      for (i = 0; i < 512; i++)
         in[i * 2] = in[i * 2 + 1] = ((i / 7) & 1) ? 32767 : -32768;
      audio_push(in, 512);
      audio_frame();
      while (g_w - g_r > 1500)
      {
         render(out, AUDIO_GRAIN);
         if (blocks > 8)
            for (i = 0; i < AUDIO_GRAIN; i++)
            {
               int v = out[i * 2];
               if (abs(v) > maxabs) maxabs = abs(v);
               if (abs(v - prev) > 60000) wraps++;
               prev = v;
            }
      }
   }
   printf("full-scale edges: peak %d, wrapped steps %u\n", maxabs, wraps);
   CHECK(wraps == 0);
   audio_stop();
}

int main(void)
{
   test_resampler_fidelity();
   test_full_scale_edges();
   sim_cfg c60 = { 60.0, 0 }, c5994 = { 59.94, 0 }, cj = { 60.0, 4.0 };
   sim_out o;

   o = run(30, &c60, NULL, 0, 0, 0, NULL);
   show("steady 60.00 Hz, 30 s", &o);
   CHECK(o.underruns == 0 && o.clicks == 0 && o.wraps == 0);
   o = run(30, &c5994, NULL, 0, 0, 0, NULL);
   show("steady 59.94 Hz, 30 s", &o);
   CHECK(o.underruns == 0 && o.clicks == 0 && o.wraps == 0);
   o = run(30, &cj, NULL, 0, 0, 0, NULL);
   show("jitter +-4 ms, 30 s", &o);
   CHECK(o.underruns == 0 && o.clicks == 0);
   o = run(20, &c60, NULL, 0, 10.0, 45.0, NULL);
   show("one 45 ms stall", &o);
   CHECK(o.wraps == 0);
   o = run(20, &c60, NULL, 0, 10.0, 90.0, NULL);
   show("one 90 ms stall", &o);
   CHECK(o.wraps == 0);

   {
      /* the game menu opened five times for three seconds, and fast forward
       * pressed five times: nothing is pushed meanwhile. */
      sim_gap g[5];
      unsigned k;
      for (k = 0; k < 5; k++) { g[k].from_s = 5 + 6 * k; g[k].to_s = 8 + 6 * k; g[k].kind = 1; }
      o = run(40, &c60, g, 5, 0, 0, NULL);
      show("5 pauses of 3 s (no flush)", &o);
      printf("   -> latency target after: %.0f ms (start %u ms)\n", 1000.0 * o.target / IN_RATE, AUDIO_TARGET_MS);
      CHECK(o.target <= IN_RATE * AUDIO_TARGET_MS / 1000u);
      CHECK(o.clicks == 0 && o.wraps == 0);
      g_flushes = 0;
      o = run(40, &c60, g, 5, 0, 0, hook_ff_flush);
      show("5 pauses of 3 s (flush each side)", &o);
      printf("   -> latency target after: %.0f ms, %u flushes\n", 1000.0 * o.target / IN_RATE, g_flushes);
      CHECK(o.target <= IN_RATE * AUDIO_TARGET_MS / 1000u);
      CHECK(o.clicks == 0 && o.wraps == 0);
   }
   printf("audio sim: %s (%u failures)\n", failures ? "FAIL" : "PASS", failures);
   return failures != 0;
}
