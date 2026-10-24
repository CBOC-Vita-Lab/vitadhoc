/* The MBC7 accelerometer (Kirby Tilt 'n' Tumble): the sample the frontend
 * publishes (gbcore_set_tilt, fe_sensor.h fe_sensor_mbc7) reaches a game
 * that latches it the way Pan Docs describes -- 0x55 to Ax0x erases, 0xAA
 * to Ax1x latches -- as the full 16-bit values, 0x81D0 level.  (The
 * vendored core returned a live 12-bit 0x7FF.) */
#include "../gbcore.h"
#include "fe_sensor.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void on_video(void *u, const gbcore_video_frame_t *f) { (void)u; (void)f; }
static void on_audio(void *u, const int16_t *s, size_t n) { (void)u; (void)s; (void)n; }

static void make_rom(unsigned char rom[0x8000], int latch)
{
  static const unsigned char logo[48] = {
    0xCE,0xED,0x66,0x66,0xCC,0x0D,0x00,0x0B,0x03,0x73,0x00,0x83,
    0x00,0x0C,0x00,0x0D,0x00,0x08,0x11,0x1F,0x88,0x89,0x00,0x0E,
    0xDC,0xCC,0x6E,0xE6,0xDD,0xDD,0xD9,0x99,0xBB,0xBB,0x67,0x63,
    0x6E,0x0E,0xEC,0xCC,0xDD,0xDC,0x99,0x9F,0xBB,0xB9,0x33,0x3E
  };
  static const unsigned char prog[] = {
    0x31, 0xFE, 0xFF,             /* 0150 LD SP,$FFFE                   */
    0x3E, 0x0A, 0xEA, 0x00, 0x00, /*      LD A,$0A ; LD ($0000),A       */
    0x3E, 0x40, 0xEA, 0x00, 0x40, /*      LD A,$40 ; LD ($4000),A       */
    /* loop (015D): */
    0x3E, 0x55, 0xEA, 0x00, 0xA0, /*      LD A,$55 ; LD ($A000),A erase */
    0x3E, 0xAA, 0xEA, 0x10, 0xA0, /*      LD A,$AA ; LD ($A010),A latch */
    0xFA, 0x20, 0xA0, 0xEA, 0x00, 0xC0, /* X low  -> $C000              */
    0xFA, 0x30, 0xA0, 0xEA, 0x01, 0xC0, /* X high -> $C001              */
    0xFA, 0x40, 0xA0, 0xEA, 0x02, 0xC0, /* Y low  -> $C002              */
    0xFA, 0x50, 0xA0, 0xEA, 0x03, 0xC0, /* Y high -> $C003              */
    0x18, 0xDC                    /*      JR loop (015D)                */
  };
  unsigned i, sum = 0;
  unsigned char check = 0;
  memset(rom, 0, 0x8000);
  rom[0x100] = 0xC3; rom[0x101] = 0x50; rom[0x102] = 0x01;
  memcpy(rom + 0x104, logo, sizeof(logo));
  memcpy(rom + 0x134, "MBC7 TILT", 9);
  rom[0x147] = 0x22;                       /* MBC7 + sensor + EEPROM */
  for (i = 0x134; i <= 0x14C; i++) check = (unsigned char)(check - rom[i] - 1);
  rom[0x14D] = check;
  memcpy(rom + 0x150, prog, sizeof(prog));
  if (!latch)
  {
    /* Never erased: an 0xAA alone latches nothing (Pan Docs). */
    rom[0x15D + 1] = 0x00;
  }
  for (i = 0; i < 0x8000; i++) if (i != 0x14E && i != 0x14F) sum += rom[i];
  rom[0x14E] = (unsigned char)(sum >> 8);
  rom[0x14F] = (unsigned char)sum;
}

static int fail(const char *what)
{
  fprintf(stderr, "FAIL mbc7: %s\n", what);
  return 1;
}

static int sample(gbcore_t *c, unsigned *x, unsigned *y)
{
  unsigned char b[4];
  int i;
  for (i = 0; i < 3; i++)
    if (gbcore_run_frame(c, 0) != 0)
      return -1;
  if (gbcore_peek(c, 0xC000, b, 4) != 0)
    return -1;
  *x = b[0] | (b[1] << 8);
  *y = b[2] | (b[3] << 8);
  return 0;
}

int main(void)
{
  static unsigned char rom[0x8000];
  gbcore_callbacks_t cb;
  gbcore_t *c;
  unsigned x, y;

  memset(&cb, 0, sizeof(cb));
  cb.video = on_video;
  cb.audio_batch = on_audio;
  make_rom(rom, 1);
  c = gbcore_create(NULL, rom, 0x8000, FE_CONSOLE_GB, 32768, &cb);
  if (!c) return fail("boot");
  if (!gbcore_has_tilt(c)) return fail("an MBC7 cartridge has the sensor");
  if (sample(c, &x, &y) || x != 0x81D0 || y != 0x81D0)
    return fail("nothing published reads level, 0x81D0");
  gbcore_set_tilt(c, fe_sensor_mbc7(0.5f, -0.25f));
  if (sample(c, &x, &y))
    return fail("run");
  printf("mbc7 right 0.5 g, top 0.25 g down: X %04X Y %04X\n", x, y);
  if (x != 0x81D0 - 56 || y != 0x81D0 + 28)
    return fail("the published sample, 0x70 per g");
  gbcore_set_tilt(c, fe_sensor_mbc7(-1.0f, 1.0f));
  if (sample(c, &x, &y) || x != 0x81D0 + 112 || y != 0x81D0 - 112)
    return fail("left side and bottom down");
  gbcore_shutdown(c);

  make_rom(rom, 0);
  c = gbcore_create(NULL, rom, 0x8000, FE_CONSOLE_GB, 32768, &cb);
  if (!c) return fail("boot 2");
  gbcore_set_tilt(c, fe_sensor_mbc7(0.5f, 0.5f));
  if (sample(c, &x, &y) || x != 0x8000 || y != 0x8000)
    return fail("never erased: nothing latched, 0x8000 (Pan Docs)");
  gbcore_shutdown(c);

  /* An ordinary cartridge has no sensor. */
  memset(rom + 0x147, 0, 1);
  {
    unsigned i, sum = 0;
    unsigned char check = 0;
    for (i = 0x134; i <= 0x14C; i++) check = (unsigned char)(check - rom[i] - 1);
    rom[0x14D] = check;
    for (i = 0; i < 0x8000; i++) if (i != 0x14E && i != 0x14F) sum += rom[i];
    rom[0x14E] = (unsigned char)(sum >> 8);
    rom[0x14F] = (unsigned char)sum;
  }
  c = gbcore_create(NULL, rom, 0x8000, FE_CONSOLE_GB, 32768, &cb);
  if (!c || gbcore_has_tilt(c)) return fail("ROM only: no sensor");
  gbcore_shutdown(c);
  puts("mbc7: ok");
  return 0;
}
