/* Game Boy audio levels on an original synthetic cartridge: peak, DC offset
 * and clicks of the real core's output for channels set up the way games set
 * them.  Prints the measurements; exits nonzero on a failed expectation. */
#include "../gbcore.h"
#include "../tgbdual/gb.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL %s:%d: %s\n",__func__,__LINE__,#x); fails++; } } while (0)
static int fails;
static byte rom[32768];
static int16_t cap[32768 * 2 * 4];
static size_t capn;
static void video(void *u, const gbcore_video_frame_t *f) { (void)u; (void)f; }
static void audio(void *u, const int16_t *p, size_t n)
{
  (void)u;
  if (capn + n <= sizeof(cap) / 4) { memcpy(cap + capn * 2, p, n * 4); capn += n; }
}
static gbcore_t *boot(void)
{
  gbcore_callbacks_t cb = {0}; byte sum = 0;
  memset(rom, 0, sizeof rom); rom[0x100] = 0xc3; rom[0x101] = 0x50; rom[0x102] = 1;
  memcpy(rom + 0x134, "GBAUDIO", 7);
  rom[0x146] = 3; rom[0x14b] = 0x33;
  for (unsigned i = 0x134; i <= 0x14c; i++) sum = (byte)(sum - rom[i] - 1);
  rom[0x14d] = sum; rom[0x150] = 0xc3; rom[0x151] = 0x50; rom[0x152] = 1;
  cb.video = video; cb.audio_batch = audio;
  gbcore_set_palette(GBCORE_PALETTE_AUTO);
  gbcore_t *c = gbcore_create(NULL, rom, sizeof rom, FE_CONSOLE_GB, 32768, &cb);
  if (!c) { fprintf(stderr, "no core\n"); exit(2); }
  return c;
}
typedef struct { int peak, maxstep; double dc; unsigned long clicks; } stat_t;
static stat_t measure(size_t from, size_t to)
{
  stat_t s = {0, 0, 0, 0}; double sum = 0; int prev = 0;
  for (size_t i = from; i < to; i++)
  {
    int v = cap[i * 2], a = abs(v), d = abs(v - prev);
    if (a > s.peak) s.peak = a;
    if (d > s.maxstep) s.maxstep = d;
    if (d > 6000) s.clicks++;
    sum += v; prev = v;
  }
  s.dc = sum / (double)(to - from);
  return s;
}
static void frames(gbcore_t *c, int n) { while (n--) gbcore_run_frame(c, 0); }
static void on(int duty, int freq)
{
  cpu_write(0xff26, 0x80); cpu_write(0xff24, 0x77); cpu_write(0xff25, 0xff);
  cpu_write(0xff11, (byte)(duty << 6)); cpu_write(0xff12, 0xf0);
  cpu_write(0xff13, (byte)(freq & 255)); cpu_write(0xff14, (byte)(0x80 | (freq >> 8)));
}
int main(void)
{
  static const char *dn[4] = {"12.5%", "25%", "50%", "75%"};
  double tone_dc[4] = {0};
  gbcore_t *c = boot();
  for (int duty = 0; duty < 4; duty++)
  {
    stat_t s;
    cpu_write(0xff26, 0); capn = 0;
    on(duty, 1750);                              /* a 440 Hz note */
    frames(c, 30);
    s = measure(2000, capn);
    printf("square %-5s 440 Hz        peak %5d dc %8.1f\n", dn[duty], s.peak, s.dc);
    tone_dc[duty] = s.dc;
  }
  {
    stat_t s;
    cpu_write(0xff26, 0); capn = 0;
    frames(c, 10);
    on(2, 2047);                                 /* parked at 0x7FF: ultrasonic */
    frames(c, 20);
    s = measure(10 * 548 + 100, capn);
    printf("square 50%%  parked 0x7FF  peak %5d dc %8.1f\n", s.peak, s.dc);
    CHECK(s.peak < 300);                          /* silent, not a push */
  }
  {
    stat_t s;
    cpu_write(0xff26, 0); capn = 0;
    frames(c, 10);
    on(0, 2047);
    frames(c, 20);
    s = measure(10 * 548 + 100, capn);
    printf("square 12.5%% parked 0x7FF peak %5d dc %8.1f\n", s.peak, s.dc);
    CHECK(fabs(s.dc - tone_dc[0]) < 300);         /* the tone's own average */
    CHECK(s.peak <= fabs(tone_dc[0]) + 300);
  }
  {
    stat_t s;
    cpu_write(0xff26, 0); capn = 0;
    frames(c, 5);
    on(2, 1750);
    cpu_write(0xff17, 0xf0); cpu_write(0xff19, 0x80 | (1750 >> 8)); cpu_write(0xff18, 1750 & 255);
    cpu_write(0xff16, 0x80);
    frames(c, 20);
    s = measure(5 * 548 + 100, capn);
    printf("squares 1+2 together       peak %5d dc %8.1f maxstep %d\n", s.peak, s.dc, s.maxstep);
  }
  gbcore_shutdown(c);
  printf("gb audio levels: %s\n", fails ? "FAIL" : "PASS");
  return fails != 0;
}
