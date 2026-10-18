/* Production GBA RTC + rewind engine: external clock input, wraps/forks.
 * Build with -DREWIND -DINLINE=inline, section GC, fe_rewind{,_pack}.c.
 * No ROM or wall-clock sleep; all timing is controlled by the test. */
#include <assert.h>
#include <stdlib.h>
#include "../../gba_memory.c"
#include "../../frontend-common/fe_rewind.h"

RFILE *filestream_open(const char *p, unsigned m, unsigned h)
{ (void)p; (void)m; (void)h; return NULL; }
int64_t filestream_seek(RFILE *f, int64_t o, int s)
{ (void)f; (void)o; (void)s; return -1; }
int64_t filestream_read(RFILE *f, void *d, int64_t n)
{ (void)f; (void)d; (void)n; return -1; }
int filestream_close(RFILE *f) { (void)f; return 0; }
u32 cpu_ticks, frame_counter, reg[64], cph_bkr, cph_bkw;
u16 palette_ram[512], oam_ram[512], palette_ram_converted[512], io_registers[512];
u8 vram[1024 * 96], vram_clean[VRAM_DIRTY_MAP_BYTES];
u32 vram_dirty_marks;
u8 ewram[1024 * 256 * 2], iwram[1024 * 32 * 2];
u8 *memory_map_read[8 * 1024];

#define BASE 1791504000ll
static time_t fixed_clock(void) { return BASE; }
static struct { uint32_t frame, hash; int64_t base; } machine, snapshot;
static uint32_t hashes[10000];
static long long seconds[10000];
static unsigned replays;
static int fail_restore;

static uint64_t get_context(void) { return (uint64_t)gpsp_rewind_clock_time(NULL); }
static void set_context(uint64_t x)
{
  long long t = (long long)x;
  (void)gpsp_rewind_clock_time(&t);
}
static int gather(void)
{
  /* The real snapshot writer samples the clock before serializing the base. */
  (void)rtc_current_time();
  machine.frame = frame_counter;
  machine.base = rtc_base_time;
  snapshot = machine;
  return 0;
}
static int restore(const void *p, uint32_t n)
{
  assert(n == sizeof(machine));
  if (fail_restore) return -1;
  memcpy(&machine, p, n);
  frame_counter = machine.frame;
  rtc_base_time = machine.base;
  return 0;
}
static void frame(uint32_t input, uint16_t *pix, int16_t *aud, uint32_t *an)
{
  long long t = rtc_current_time();
  unsigned i;
  for (i = 0; i < 100; i++)
    assert(rtc_current_time() == t); /* polling cannot advance time */
  machine.hash = machine.hash * 1664525u + input + (uint32_t)t;
  frame_counter++;
  machine.frame = frame_counter;
  /* Snapshot gathers happen after core frame_counter increments. */
  assert(rtc_current_time() == t);
  if (pix) pix[0] = (uint16_t)machine.hash;
  if (aud) { aud[0] = (int16_t)machine.hash; aud[1] = (int16_t)t; }
  if (an) *an = 1;
}
static void replay(uint32_t input, uint16_t *pix, int16_t *aud, uint32_t *an)
{
  uint32_t f = frame_counter + 1;
  assert(gpsp_rewind_clock_time(NULL) == seconds[f]);
  frame(input, pix, aud, an);
  assert(machine.hash == hashes[f]);
  replays++;
}
static void normal(unsigned wall_tick)
{
  uint16_t *pix = NULL;
  int16_t *aud = NULL;
  uint32_t an = 0, f = frame_counter + 1;
  fe_rewind_play_slot(&pix, &aud);
  gpsp_rtc_wall = BASE + wall_tick / 480;
  gpsp_rewind_clock_sync(); /* first-pass input latch, after possible gather */
  seconds[f] = gpsp_rewind_clock_time(NULL);
  frame(f & 0x3ff, pix, aud, &an);
  hashes[f] = machine.hash;
  fe_rewind_frame(f & 0x3ff, an);
  assert(fe_rewind_selfcheck() == 0);
}
static void basic_clock(void)
{
  unsigned f;
  long long tag, floor;
  frame_counter = 0; gpsp_wallclock = fixed_clock;
  rtc_init_base_time(); gpsp_rtc_frames_only = 0; gba_rewind_clock_frames = 1;
  for (f = 1; f <= 4800; f++)
  {
    gpsp_rtc_wall = BASE + f / 480;
    gpsp_rewind_clock_sync();
    frame_counter = f;
    assert(rtc_current_time() == gpsp_rtc_wall);
    if (!(f % 6)) { /* gathering a state also polls the RTC */
      assert(rtc_current_time() == gpsp_rtc_wall);
    }
  }
  assert(rtc_current_time() == BASE + 10);
  floor = rtc_last_shown;
  tag = BASE + 1;
  (void)gpsp_rewind_clock_time(&tag);
  assert(rtc_current_time() == tag && rtc_last_shown == floor);
  { long long peek; gpsp_rtc_peek(&peek, NULL, NULL); assert(peek == tag); }
  gpsp_rtc_wall = BASE + 9; gpsp_rewind_clock_sync();
  assert(rtc_current_time() == floor); /* backward console-clock adjustment */
  gpsp_rtc_wall = BASE + 1000; gpsp_rewind_clock_sync();
  assert(rtc_current_time() == BASE + 1000); /* sleep/pause */
  frame_counter = 0; rtc_base_time = BASE - 5000; /* explicit load/reset */
  gpsp_rewind_clock_sync();
  assert(rtc_current_time() == BASE + 1000);
  rtc_init_base_time(); gpsp_rtc_wall = BASE; gpsp_rewind_clock_sync();
  assert(rtc_current_time() == BASE); /* ROM switch: old floor gone */

  gpsp_rtc_wall = 0; rtc_base_time = BASE; frame_counter = 600;
  gpsp_rewind_clock_sync();
  assert(gpsp_rewind_clock_time(NULL) == 0);
  assert(rtc_current_time() == BASE + (s64)(600 * GBA_FRAME_SECONDS));
  gpsp_rtc_wall = BASE + 999; /* newly available clock is ignored for this tag */
  assert(rtc_current_time() == BASE + (s64)(600 * GBA_FRAME_SECONDS));
  gpsp_rtc_wall = BASE + 999; gpsp_rtc_frames_only = 1;
  gpsp_rewind_clock_sync();
  assert(rtc_current_time() == BASE + (s64)(600 * GBA_FRAME_SECONDS));
  gpsp_rtc_frames_only = 0;
  puts("RTC: 8x/4800-frame poll+gather, replay, clock rollback, pause, load, ROM switch, link clock PASS");
}
int main(void)
{
  fe_rewind_config c;
  fe_rewind_span span;
  fe_rewind_shown shown;
  void *mem;
  unsigned i, tick = 0, start, loops;
  basic_clock();
  memset(&c, 0, sizeof(c)); memset(&span, 0, sizeof(span));
  span.cur = &snapshot; span.bytes = sizeof(snapshot);
  c.spans = &span; c.nspans = 1; c.interval = 6; c.max_frames = 2048;
  c.ring_bytes = 128 * 1024; c.frame_pixels = 1; c.audio_cap = 1; c.burst = 3;
  c.gather = gather; c.restore = restore; c.run = replay;
  c.frame_context = get_context; c.replay_context = set_context;
  mem = malloc(fe_rewind_mem_bytes(&c)); assert(mem);
  assert(fe_rewind_init(&c, mem, fe_rewind_mem_bytes(&c)) == 0);
  frame_counter = 0; memset(&machine, 0, sizeof(machine));
  rtc_init_base_time(); gpsp_rtc_wall = BASE; gpsp_rewind_clock_sync();
  fe_rewind_reset(0);
  for (i = 0; i < 5000; i++) {
    assert(fe_rewind_tick(0, &shown) == FE_RW_PLAY); normal(++tick);
  }
  for (loops = 0; loops < 4; loops++)
  {
    start = fe_rewind_frame_now();
    for (i = 0; i < 1400 && fe_rewind_frame_now() > start - 700; i++)
    {
      (void)fe_rewind_tick(1, &shown);
      assert(fe_rewind_selfcheck() == 0);
    }
    assert(start - fe_rewind_frame_now() >= 700); /* >64 snapshot anchors */
    for (i = 0; i < 100; i++)
      if (fe_rewind_tick(0, &shown) == FE_RW_PLAY) break;
    assert(i < 100 && machine.hash == hashes[frame_counter]);
    tick += 4800; /* new timeline at a different real second */
    for (i = 0; i < 750; i++)
    { normal(++tick); assert(fe_rewind_tick(0, &shown) == FE_RW_PLAY); }
  }
  /* A history reset must not reuse old clock context with identical indices. */
  frame_counter = 0; memset(&machine, 0, sizeof(machine)); rtc_init_base_time();
  fe_rewind_reset(0);
  for (i = 0; i < 500; i++) normal(++tick);
  for (i = 0; i < 100; i++) (void)fe_rewind_tick(1, &shown);
  assert(replays > 2800);
  assert(fe_rewind_get_stats()->anomalies == 0);
  printf("RTC+rewind: input-ring wrap, 4 branches, >64 snapshots, reset; %u replay frames PASS\n", replays);
  /* A rejected restore must never run another frame or report PLAY until
   * the frontend has explicitly recovered/reset the machine. */
  frame_counter = 0; memset(&machine, 0, sizeof(machine));
  fe_rewind_reset(0);
  for (i = 0; i < 500; i++) normal(++tick);
  fail_restore = 1;
  for (i = 0; i < 100; i++)
    if (fe_rewind_tick(1, &shown) == FE_RW_ERROR) break;
  assert(i < 100);
  start = replays;
  for (i = 0; i < 20; i++)
    assert(fe_rewind_tick(i & 1, &shown) == FE_RW_ERROR);
  assert(replays == start && fe_rewind_mach_noland());
  fail_restore = 0;
  frame_counter = 0; memset(&machine, 0, sizeof(machine));
  fe_rewind_reset(0);
  assert(fe_rewind_tick(0, &shown) == FE_RW_PLAY);
  puts("rewind: failed restore stops immediately, sticky until explicit reset PASS");
  free(mem);
  return 0;
}
