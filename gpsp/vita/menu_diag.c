#include <string.h>
#include "fe_evt.h"
#include "menu_diag.h"

static const char *const names[MD_STAGE_COUNT] = {
   "still", "hero", "thumb", "prefetch", "art_read", "decode", "resample",
   "cache_write", "upload", "draw", "vblank", "present", "swap", "defer", "probe",
   "pre_draw", "metadata", "description", "publication"
};
static struct {
   int on, frame, rom, console, last_rom, last_console, sample, paused, input_frame;
   unsigned frames, slow, input, nav, suppressed;
   unsigned long long t0, frame_t0, poll_t0, window, detail, nav_detail, sample_t0;
   unsigned long long pause_t0;
   unsigned long long total, maximum, poll_max, current[MD_STAGE_COUNT];
   unsigned long long sum[MD_STAGE_COUNT], max[MD_STAGE_COUNT];
   unsigned calls[MD_STAGE_COUNT];
} d;

int md_active(void) { return d.on && !d.paused; }
unsigned long long md_begin(void) { return md_active() ? fe_evt_now_us() : 0; }
void md_end(enum md_stage stage, unsigned long long began)
{
   unsigned long long elapsed;
   if (!md_active() || stage < 0 || stage >= MD_STAGE_COUNT) return;
   elapsed = fe_evt_now_us() - began;
   d.sum[stage] += elapsed;
   d.current[stage] += elapsed;
   if (elapsed > d.max[stage]) d.max[stage] = elapsed;
   d.calls[stage]++;
}
void md_start(void)
{
#ifdef GPSP_NO_TELEMETRY
   return;
#endif
   memset(&d, 0, sizeof(d));
   d.on = 1;
   d.last_rom = d.last_console = -2;
   d.t0 = d.window = d.sample_t0 = fe_evt_now_us();
   fe_evt("menu_begin diag=7 summary_us=5000000 slow_us=50000 nested=1");
}
static void window_emit(unsigned long long now)
{
   unsigned i, lines, us, max_us, io_us, io_max, drops, hi;
   fe_evt_prof(&lines, &us, &max_us);
   fe_evt_prof_io(&io_us, &io_max, &drops, &hi);
   fe_evt("menu_window span_us=%llu frames=%u avg_us=%llu max_us=%llu "
          "slow=%u input=%u nav=%u poll_max_us=%llu suppressed=%u "
          "rom=%d console=%d", now - d.window, d.frames,
          d.frames ? d.total / d.frames : 0, d.maximum, d.slow, d.input,
          d.nav, d.poll_max, d.suppressed, d.rom, d.console);
   for (i = 0; i < MD_STAGE_COUNT; i++)
      if (d.calls[i])
         fe_evt("menu_stage stage=%s calls=%u total_us=%llu max_us=%llu",
                names[i], d.calls[i], d.sum[i], d.max[i]);
   fe_evt("menu_log lines=%u caller_us=%u caller_max_us=%u writer_us=%u "
          "writer_max_us=%u drops=%u ring_hi=%u pending=%d", lines, us,
          max_us, io_us, io_max, drops, hi, fe_evt_pending());
   d.window = now;
   d.frames = d.slow = d.input = d.nav = d.suppressed = 0;
   d.total = d.maximum = d.poll_max = 0;
   memset(d.sum, 0, sizeof(d.sum));
   memset(d.max, 0, sizeof(d.max));
   memset(d.calls, 0, sizeof(d.calls));
}
void md_frame_begin(int rom, int console, int input_active)
{
   unsigned long long now;
   if (!md_active()) return;
   if (d.frame) md_frame_end();
   now = fe_evt_now_us();
   if (d.poll_t0 && now - d.poll_t0 > d.poll_max) d.poll_max = now - d.poll_t0;
   d.poll_t0 = now;
   d.frame_t0 = now;
   d.frame = 1;
   d.input_frame = 0;
   md_input(input_active);
   memset(d.current, 0, sizeof(d.current));
   md_selection(rom, console);
}
void md_selection(int rom, int console)
{
   unsigned long long now;
   if (!d.on) return;
   now = fe_evt_now_us();
   d.rom = rom; d.console = console;
   if (rom != d.last_rom || console != d.last_console)
   {
      /* Navigation bursts are counted; identity is emitted at most once/sec. */
      d.nav++;
      d.last_rom = rom; d.last_console = console;
      if (!d.nav_detail || now - d.nav_detail >= 1000000)
      {
         fe_evt("menu_nav rom=%d console=%d", rom, console);
         d.nav_detail = now;
      }
   }
}
void md_input(int active)
{
   if (md_active() && d.frame && active && !d.input_frame)
   {
      d.input++;
      d.input_frame = 1;
   }
}
void md_frame_end(void)
{
   unsigned long long now, elapsed;
   if (!d.on || !d.frame) return;
   now = fe_evt_now_us(); elapsed = now - d.frame_t0;
   d.frame = 0;
   d.frames++; d.total += elapsed;
   if (elapsed > d.maximum) d.maximum = elapsed;
   if (elapsed >= 50000)
   {
      d.slow++;
      if (!d.detail || now - d.detail >= 1000000)
      {
         fe_evt("menu_slow rom=%d console=%d frame_us=%llu still_us=%llu "
                "hero_us=%llu thumb_us=%llu prefetch_us=%llu read_us=%llu "
                "decode_us=%llu resample_us=%llu cache_write_us=%llu "
                "upload_us=%llu draw_us=%llu vblank_us=%llu present_us=%llu "
                "swap_us=%llu defer_us=%llu probe_us=%llu pre_draw_us=%llu "
                "metadata_us=%llu description_us=%llu publication_us=%llu", d.rom, d.console, elapsed,
                d.current[MD_STILL], d.current[MD_HERO], d.current[MD_THUMB],
                d.current[MD_PREFETCH], d.current[MD_ART_READ],
                d.current[MD_DECODE], d.current[MD_RESAMPLE],
                d.current[MD_CACHE_WRITE], d.current[MD_UPLOAD],
                d.current[MD_DRAW], d.current[MD_VBLANK], d.current[MD_PRESENT],
                d.current[MD_SWAP], d.current[MD_DEFER], d.current[MD_PROBE],
                d.current[MD_PRE_DRAW], d.current[MD_META],
                d.current[MD_DESCRIPTION], d.current[MD_PUBLISH]);
         d.detail = now;
      }
      else d.suppressed++;
   }
   if (now - d.window >= 5000000) window_emit(now);
   if (now - d.sample_t0 >= 5000000) { d.sample = 1; d.sample_t0 = now; }
}
int md_sample_due(void)
{
   int sample = d.on && d.sample;
   d.sample = 0;
   return sample;
}
void md_stop(void)
{
   unsigned long long now;
   if (!d.on) return;
   if (d.frame) md_frame_end();
   now = fe_evt_now_us();
   if (d.frames || d.calls[0]) window_emit(now);
   fe_evt("menu_end elapsed_us=%llu drops=%u", now - d.t0, fe_evt_drops());
   d.on = 0;
}

void md_pause(void)
{
   if (!md_active()) return;
   if (d.frame) md_frame_end();
   if (d.frames) window_emit(fe_evt_now_us());
   d.pause_t0 = fe_evt_now_us();
   d.paused = 1;
   fe_evt("menu_pause");
}
void md_resume(void)
{
   if (!d.on || !d.paused) return;
   fe_evt("menu_resume paused_us=%llu", fe_evt_now_us() - d.pause_t0);
   d.paused = 0;
   d.poll_t0 = 0;
   d.window = d.sample_t0 = fe_evt_now_us();
}
