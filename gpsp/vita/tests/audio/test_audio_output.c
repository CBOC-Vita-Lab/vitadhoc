/* The real portable backend, with a deferred native-device lifetime oracle. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../audio_vita.c"

static int (*scheduled_step)(void);
static const int16_t *pending[2];
static int16_t accepted[2][AUDIO_GRAIN * 2];
static const int16_t *submitted[32];
static unsigned writes, closes, opens, stops, inspections, overwritten;
static unsigned misaligned, slept_us;
static int stop_result = 1, fail_write, fail_open, fail_close, fail_thread, device_open;
static int retain_failed_thread;
static unsigned failures;

#define CHECK(condition) do { if (!(condition)) { \
   fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #condition); \
   failures++; \
} } while (0)

static void inspect_pending(void)
{
   unsigned i;
   for (i = 0; i < 2; i++)
      if (pending[i])
      {
         inspections++;
         overwritten += memcmp(pending[i], accepted[i], sizeof(accepted[i])) != 0;
      }
}

int plat_audio_open(unsigned rate, unsigned grain)
{
   opens++;
   CHECK(rate == AUDIO_OUT_RATE && grain == AUDIO_GRAIN);
   if (device_open)
      return -1;
   if (fail_open)
   {
      fail_open--;
      return -1;
   }
   device_open = 1;
   return 0;
}

int plat_audio_write(const int16_t *stereo)
{
   inspect_pending();
   misaligned += ((uintptr_t)stereo & 63u) != 0;
   if (writes < sizeof(submitted) / sizeof(submitted[0]))
      submitted[writes] = stereo;
   writes++;
   if (!device_open)
      return -1;
   if (fail_write)
   {
      /* Even a failing call may have retained its argument. Neither window
       * can be reused until close has retired every possibly owned pointer. */
      fail_write--;
      pending[1] = stereo;
      memcpy(accepted[1], stereo, sizeof(accepted[1]));
      return -1;
   }
   pending[0] = stereo;
   memcpy(accepted[0], stereo, sizeof(accepted[0]));
   pending[1] = NULL;
   return 0;
}

int plat_audio_close_checked(void)
{
   inspect_pending();
   closes++;
   if (fail_close)
   {
      fail_close--;
      return 0;
   }
   pending[0] = pending[1] = NULL;
   device_open = 0;
   return 1;
}
void plat_audio_close(void) { (void)plat_audio_close_checked(); }

void plat_sleep_us(uint32_t us) { slept_us += us; }
int plat_audio_thread_start(int (*step)(void))
{
   if (scheduled_step)
      return -1;
   if (fail_thread)
   {
      if (retain_failed_thread)
         scheduled_step = step; /* dormant UID whose deletion failed */
      return -1;
   }
   scheduled_step = step;
   return 0;
}
int plat_audio_thread_stop_checked(void)
{
   stops++;
   if (!stop_result)
      return 0;
   scheduled_step = NULL;
   return 1;
}
void plat_audio_thread_stop(void) { (void)plat_audio_thread_stop_checked(); }

static void reset_fixture(void)
{
   stop_result = 1;
   fail_close = 0;
   audio_stop();
   scheduled_step = NULL;
   pending[0] = pending[1] = NULL;
   memset(submitted, 0, sizeof(submitted));
   writes = closes = opens = stops = inspections = overwritten = 0;
   misaligned = slept_us = 0;
   fail_write = fail_open = fail_close = fail_thread = device_open = 0;
   retain_failed_thread = 0;
}

static void push_tone(unsigned rate, unsigned frames)
{
   static int16_t samples[12000 * 2];
   unsigned i;
   CHECK(frames <= 12000);
   if (frames > 12000)
      abort();
   for (i = 0; i < frames; i++)
   {
      samples[i * 2] = (int16_t)(20000.0 * sin(6.283185307179586 * 440 * i / rate));
      samples[i * 2 + 1] = (int16_t)(18000.0 * sin(6.283185307179586 * 523 * i / rate));
   }
   audio_push(samples, frames);
}

static uint32_t ring_hash(void)
{
   uint32_t hash = 2166136261u;
   unsigned i;
   for (i = 0; i < RING_FRAMES * 2; i++)
      hash = (hash ^ (uint16_t)g_ring[i]) * 16777619u;
   return hash;
}

static void test_deferred_windows(void)
{
   unsigned i;
   reset_fixture();
   CHECK(audio_start(32768, 1000000) == 0);
   push_tone(32768, 12000);
   for (i = 0; i < 8; i++)
      CHECK(scheduled_step() == 0);
   CHECK(inspections == 7);
   CHECK(overwritten == 0);
   CHECK(misaligned == 0);
   CHECK(submitted[0] != submitted[1]);
   for (i = 2; i < 8; i++)
      CHECK(submitted[i] == submitted[i - 2]);
   audio_stop();
   CHECK(overwritten == 0);
   CHECK(pending[0] == NULL && scheduled_step == NULL);
}

static void test_failed_write_reopen(void)
{
   reset_fixture();
   CHECK(audio_start(32768, 1000000) == 0);
   push_tone(32768, 12000);
   CHECK(scheduled_step() == 0);
   fail_write = 1;
   fail_open = 1;
   CHECK(scheduled_step() == 0);
   CHECK(overwritten == 0);
   CHECK(closes == 1 && opens == 2);
   CHECK(slept_us >= 500000);
   CHECK(scheduled_step() == 0);
   CHECK(device_open == 1);
   CHECK(scheduled_step() == 0);
   CHECK(scheduled_step() == 0);
   CHECK(overwritten == 0 && misaligned == 0);
   CHECK(g_port_errors == 1);
   CHECK(submitted[1] == submitted[2]); /* failed submits do not rotate */
   CHECK(submitted[2] != submitted[3]);
   audio_stop();
   CHECK(overwritten == 0);
}

static void test_failed_close_keeps_windows(void)
{
   uint32_t r, w, hash;
   uint64_t phase;
   unsigned old_writes, old_opens;
   reset_fixture();
   CHECK(audio_start(32768, 1000000) == 0);
   push_tone(32768, 12000);
   CHECK(scheduled_step() == 0);
   fail_write = 1;
   fail_close = 2;
   CHECK(scheduled_step() == 0);
   CHECK(pending[0] != NULL && pending[1] != NULL);
   r = g_r; w = g_w; phase = g_phase; hash = ring_hash();
   old_writes = writes; old_opens = opens;
   CHECK(scheduled_step() == 0);
   CHECK(g_r == r && g_w == w && g_phase == phase && ring_hash() == hash);
   CHECK(writes == old_writes && opens == old_opens);
   CHECK(overwritten == 0);
   CHECK(scheduled_step() == 0); /* definitive retirement and reopen */
   CHECK(g_r == r && writes == old_writes && opens == old_opens + 1);
   CHECK(scheduled_step() == 0);
   CHECK(submitted[1] == submitted[2]);
   CHECK(overwritten == 0);
   audio_stop();
}

static void test_stopped_close_failure_blocks_restart(void)
{
   uint32_t r, w, hash;
   unsigned old_opens;
   reset_fixture();
   CHECK(audio_start(32768, 1000000) == 0);
   push_tone(32768, 12000);
   CHECK(scheduled_step() == 0);
   r = g_r; w = g_w; hash = ring_hash(); old_opens = opens;
   fail_close = 2;
   audio_stop();
   CHECK(g_open == 1 && g_running == 0 && pending[0] != NULL);
   CHECK(scheduled_step == NULL);
   CHECK(audio_start(48000, 1000000) == -1);
   CHECK(g_r == r && g_w == w && ring_hash() == hash);
   CHECK(g_open == 1 && g_running == 0 && opens == old_opens);
   CHECK(overwritten == 0);
   CHECK(audio_start(48000, 1000000) == 0);
   CHECK(opens == old_opens + 1 && g_r == 0 && g_w == 0);
   audio_stop();
}

static void test_stop_timeout_restart(void)
{
   uint32_t before_r, before_w, before_hash, before_step;
   uint64_t before_phase;
   unsigned before_target, before_opens, before_closes;
   int (*old_step)(void);
   reset_fixture();
   CHECK(audio_start(32768, 1000000) == 0);
   push_tone(32768, 12000);
   CHECK(scheduled_step() == 0);
   before_r = g_r; before_w = g_w; before_hash = ring_hash();
   before_step = g_step; before_phase = g_phase; before_target = g_target;
   before_opens = opens; before_closes = closes; old_step = scheduled_step;
   stop_result = 0;
   audio_stop();
   CHECK(g_open == 1 && g_running == 0);
   CHECK(scheduled_step == old_step && closes == before_closes);
   CHECK(audio_start(48000, 1500000) == -1);
   CHECK(g_open == 1 && g_running == 0);
   CHECK(g_r == before_r && g_w == before_w && ring_hash() == before_hash);
   CHECK(g_step == before_step && g_phase == before_phase);
   CHECK(g_target == before_target && g_in_rate == 32768);
   CHECK(opens == before_opens && closes == before_closes);
   CHECK(scheduled_step == old_step && pending[0] != NULL);
   inspect_pending();
   CHECK(overwritten == 0);
   stop_result = 1;
   CHECK(audio_start(48000, 1000000) == 0);
   CHECK(g_running == 1 && g_open == 1 && g_in_rate == 48000);
   CHECK(closes == before_closes + 1 && opens == before_opens + 1);
   CHECK(g_w == 0 && g_r == 0 && g_phase == 0);
   audio_stop();
}

static void test_invalid_settings_preserve_session(void)
{
   static const unsigned invalid[][2] = {
      {7999, 1000000}, {65537, 1000000}, {32768, 499999},
      {32768, 2000001}, {~0u, ~0u}
   };
   unsigned i;
   for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++)
   {
      uint32_t r, w, hash, step;
      uint64_t phase;
      unsigned old_opens, old_closes, old_stops, target, rate, under;
      int (*old_step)(void);
      reset_fixture();
      CHECK(audio_start(32768, 1000000) == 0);
      push_tone(32768, 12000);
      CHECK(scheduled_step() == 0);
      r = g_r; w = g_w; hash = ring_hash(); step = g_step; phase = g_phase;
      old_opens = opens; old_closes = closes; old_stops = stops;
      target = g_target; rate = g_in_rate; under = g_underruns;
      old_step = scheduled_step;
      CHECK(audio_start(invalid[i][0], invalid[i][1]) == -1);
      CHECK(g_running == 1 && g_open == 1 && scheduled_step == old_step);
      CHECK(g_r == r && g_w == w && ring_hash() == hash && g_step == step);
      CHECK(g_phase == phase && g_target == target && g_in_rate == rate);
      CHECK(g_underruns == under && opens == old_opens && closes == old_closes);
      CHECK(stops == old_stops);
   }
   reset_fixture();
   CHECK(audio_start(0, 0) == 0 && g_in_rate == 32768);
   audio_stop();
   fail_thread = 1;
   CHECK(audio_start(32768, 1000000) == -1);
   CHECK(g_running == 0 && g_open == 0 && scheduled_step == NULL);
}

static void test_failed_start_retains_cleanup_ownership(void)
{
   uint32_t r, w, step, hash;
   reset_fixture();
   fail_thread = retain_failed_thread = 1;
   stop_result = 0;
   CHECK(audio_start(32768, 1000000) == -1);
   CHECK(g_running == 0 && g_open == 1);
   CHECK(stops == 1 && closes == 0 && opens == 1 && device_open == 1);
   CHECK(scheduled_step != NULL);
   r = g_r; w = g_w; step = g_step; hash = ring_hash();
   CHECK(audio_start(48000, 1500000) == -1);
   CHECK(g_running == 0 && g_open == 1 && g_in_rate == 32768);
   CHECK(g_r == r && g_w == w && g_step == step && ring_hash() == hash);
   CHECK(stops == 2 && closes == 0 && opens == 1);
   stop_result = 1;
   fail_thread = retain_failed_thread = 0;
   CHECK(audio_start(48000, 1000000) == 0);
   CHECK(stops == 3 && closes == 1 && opens == 2);
   CHECK(g_running == 1 && g_open == 1 && g_in_rate == 48000);
   audio_stop();
}

static void test_rate_speed_follow_range(void)
{
   static const unsigned rates[] = {8000, 32768, 48000, 65536};
   static const unsigned speeds[] = {500000, 1000000, 2000000};
   static const int ppms[] = {-AUDIO_FOLLOW_PPM, 0, AUDIO_FOLLOW_PPM};
   unsigned i, j, k;
   for (i = 0; i < sizeof(rates) / sizeof(rates[0]); i++)
      for (j = 0; j < sizeof(speeds) / sizeof(speeds[0]); j++)
         for (k = 0; k < sizeof(ppms) / sizeof(ppms[0]); k++)
         {
            int16_t output[AUDIO_GRAIN * 2];
            double expected = AUDIO_GRAIN * (double)rates[i] / AUDIO_OUT_RATE *
               speeds[j] / 1000000.0 * (1.0 + ppms[k] / 1000000.0);
            reset_fixture();
            CHECK(audio_start(rates[i], speeds[j]) == 0);
            push_tone(rates[i], 12000);
            set_step(ppms[k]);
            render(output, AUDIO_GRAIN);
            CHECK(fabs((double)g_r - expected) < 1.01);
            CHECK(g_phase <= UINT32_MAX);
            CHECK(g_underruns == 0 && g_starved == 0);
            audio_stop();
         }
}

static void test_trim_continuity_flush(void)
{
   int16_t output[128 * 2], before[2], fresh[2048 * 2];
   unsigned i, channel;
   int maximum = 0;
   reset_fixture();
   CHECK(audio_start(32768, 1000000) == 0);
   push_tone(32768, 8192);
   g_fade = 65536;
   render(before, 1);
   audio_frame();
   render(output, 128);
   for (channel = 0; channel < 2; channel++)
   {
      CHECK(abs(output[channel] - before[channel]) <= 1024);
      for (i = 0; i < 128; i++)
      {
         int previous = i ? output[(i - 1) * 2 + channel] : before[channel];
         int delta = abs(output[i * 2 + channel] - previous);
         if (delta > maximum) maximum = delta;
      }
   }
   CHECK(maximum <= 2500);
   CHECK(g_trims == 1);
   printf("trim: maximum stereo adjacent step=%d (limit 2500)\n", maximum);

   /* Start another transition, flush it partway through, and refill. The
    * old transition must not survive and mix stale audio into fresh input. */
   push_tone(32768, 8192);
   audio_frame();
   render(output, 16);
   audio_flush();
   render(output, 128);
   CHECK(g_r == g_w && g_phase == 0);
   CHECK(output[127 * 2] == 0 && output[127 * 2 + 1] == 0);
   for (i = 0; i < 2048; i++)
   {
      fresh[i * 2] = 2000;
      fresh[i * 2 + 1] = -3000;
   }
   audio_push(fresh, 2048);
   render(output, 128);
   CHECK(output[127 * 2] == 2000 && output[127 * 2 + 1] == -3000);
   CHECK(g_trims == 2);
   audio_stop();
}

static void test_retrim_uses_actual_output(void)
{
   int16_t input[8192 * 2], output[128 * 2], before[2];
   unsigned i, channel;
   reset_fixture();
   CHECK(audio_start(32768, 1000000) == 0);
   for (i = 0; i < 8192; i++)
      input[i * 2] = input[i * 2 + 1] = i < 1024 ? 30000 : -30000;
   audio_push(input, 8192);
   g_fade = 65536;
   render(before, 1);
   audio_frame();
   render(output, 16);
   for (channel = 0; channel < 2; channel++)
   {
      CHECK(output[channel] == before[channel]);
      for (i = 1; i < 16; i++)
         CHECK(abs(output[i * 2 + channel] - output[(i - 1) * 2 + channel]) <= 1024);
      before[channel] = output[15 * 2 + channel];
   }
   for (i = 0; i < 8192 * 2; i++)
      input[i] = 30000;
   audio_push(input, 8192);
   audio_frame();
   render(output, 128);
   for (channel = 0; channel < 2; channel++)
   {
      CHECK(output[channel] == before[channel]);
      for (i = 1; i < 128; i++)
         CHECK(abs(output[i * 2 + channel] - output[(i - 1) * 2 + channel]) <= 1024);
      CHECK(output[127 * 2 + channel] == 30000);
   }
   CHECK(g_trims == 2);
   audio_stop();
}

static void test_flush_and_underrun_use_emitted_tail(void)
{
   int16_t input[8192 * 2], output[128 * 2], before[2];
   unsigned i, channel, scenario;
   for (scenario = 0; scenario < 2; scenario++)
   {
      reset_fixture();
      CHECK(audio_start(32768, 1000000) == 0);
      for (i = 0; i < 8192; i++)
         input[i * 2] = input[i * 2 + 1] = i < 1024 ? 30000 : -30000;
      audio_push(input, 8192);
      g_fade = 65536;
      render(before, 1);
      /* A short legitimate cushion also forces an underrun before the
       * transition finishes, without inventing or rewinding ring indices. */
      if (scenario == 1)
         STORE(g_target, 16);
      audio_frame();
      render(output, 16);
      before[0] = output[15 * 2];
      before[1] = output[15 * 2 + 1];
      if (scenario == 0)
         audio_flush();
      render(output, 128);
      for (channel = 0; channel < 2; channel++)
      {
         CHECK(abs(output[channel] - before[channel]) <= 1024);
         for (i = 1; i < 128; i++)
            CHECK(abs(output[i * 2 + channel] - output[(i - 1) * 2 + channel]) <= 1024);
         CHECK(output[127 * 2 + channel] == 0);
      }
      if (scenario == 1)
         CHECK(g_underruns == 1);
      audio_stop();
   }

   /* An underrun during fade-in must begin its own fade from the emitted
    * amplitude, without treating that amplitude as still needing the old
    * fade-in gain a second time. */
   reset_fixture();
   CHECK(audio_start(32768, 1000000) == 0);
   STORE(g_target, 4);
   for (i = 0; i < 4; i++) input[i] = 30000;
   audio_push(input, 2);
   render(output, 128);
   for (channel = 0; channel < 2; channel++)
   {
      CHECK(output[2 * 2 + channel] == output[1 * 2 + channel] * 63 / 64);
      for (i = 1; i < 128; i++)
         CHECK(abs(output[i * 2 + channel] - output[(i - 1) * 2 + channel]) <= 1024);
      CHECK(output[127 * 2 + channel] == 0);
   }
   CHECK(g_underruns == 1);
   audio_stop();
}

int main(void)
{
   test_deferred_windows();
   test_failed_write_reopen();
   test_failed_close_keeps_windows();
   test_stopped_close_failure_blocks_restart();
   test_stop_timeout_restart();
   test_invalid_settings_preserve_session();
   test_failed_start_retains_cleanup_ownership();
   test_rate_speed_follow_range();
   test_trim_continuity_flush();
   test_retrim_uses_actual_output();
   test_flush_and_underrun_use_emitted_tail();
   reset_fixture();
   printf("audio output: %s (%u failures)\n", failures ? "FAIL" : "PASS", failures);
   return failures != 0;
}
