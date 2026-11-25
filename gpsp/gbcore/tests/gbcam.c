/* Game Boy Camera (cartridge 0xFC, tgbdual/gbcam.h): a small program in a
 * synthetic cartridge copies a register set from WRAM to the camera, starts a
 * capture and waits for it; the picture that lands in cartridge RAM is
 * compared with the answer worked out here by hand (a test image, thresholds
 * and gain chosen so every shade is predictable), and the busy time, the
 * mirrored registers, ROM and RAM banking and the state are checked. */
#include "../gbcore.h"
#include "../tgbdual/gbcam.h"
#include "fe_gbcam_fit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void on_video(void *u, const gbcore_video_frame_t *f) { (void)u; (void)f; }
static void on_audio(void *u, const int16_t *s, size_t n) { (void)u; (void)s; (void)n; }

#define FRAME_LINES 154
#define CODE_START  0x150
/* WRAM: C100 = A000 value, C101.. = A001..A035; C0F0 = done flag */
static void make_rom(unsigned char *rom, size_t size)
{
  static const unsigned char logo[48] = {
    0xCE,0xED,0x66,0x66,0xCC,0x0D,0x00,0x0B,0x03,0x73,0x00,0x83,
    0x00,0x0C,0x00,0x0D,0x00,0x08,0x11,0x1F,0x88,0x89,0x00,0x0E,
    0xDC,0xCC,0x6E,0xE6,0xDD,0xDD,0xD9,0x99,0xBB,0xBB,0x67,0x63,
    0x6E,0x0E,0xEC,0xCC,0xDD,0xDC,0x99,0x9F,0xBB,0xB9,0x33,0x3E
  };
  static const unsigned char prog[] = {
    0x31, 0xFE, 0xFF,                   /* LD SP,$FFFE                   */
    0x3E, 0x0A, 0xEA, 0x00, 0x00,       /* RAM enable                    */
    0x3E, 0x10, 0xEA, 0x00, 0x40,       /* camera registers at A000      */
    0x21, 0x01, 0xC1,                   /* LD HL,$C101                   */
    0x11, 0x01, 0xA0,                   /* LD DE,$A001                   */
    0x06, 0x35,                         /* LD B,53                       */
    0x2A, 0x12, 0x13, 0x05, 0x20, 0xFA,/* copy loop                     */
    0xFA, 0x00, 0xC1,                   /* LD A,($C100)                  */
    0xEA, 0x00, 0xA0,                   /* LD ($A000),A: start           */
    0xFA, 0x00, 0xA0,                   /* wait: LD A,($A000)            */
    0xE6, 0x01,                         /* AND 1                         */
    0x20, 0xF9,                         /* JR NZ,wait                    */
    0x3E, 0x00, 0xEA, 0x00, 0x20,       /* ROM bank 0 at 4000            */
    0xFA, 0x00, 0x40, 0xEA, 0xF1, 0xC0, /* -> C0F1                       */
    0x3E, 0x03, 0xEA, 0x00, 0x20,       /* ROM bank 3                    */
    0xFA, 0x00, 0x40, 0xEA, 0xF2, 0xC0, /* -> C0F2                       */
    0x3E, 0x01, 0xEA, 0x00, 0x40,       /* RAM bank 1                    */
    0x3E, 0x5A, 0xEA, 0x00, 0xA0,       /* write 5A                      */
    0x3E, 0x00, 0xEA, 0x00, 0x40,       /* RAM bank 0                    */
    0xFA, 0x00, 0xA0, 0xEA, 0xF3, 0xC0, /* A000 of bank 0 -> C0F3        */
    0x3E, 0x01, 0xEA, 0x00, 0x40,       /* RAM bank 1                    */
    0xFA, 0x00, 0xA0, 0xEA, 0xF4, 0xC0, /* A000 of bank 1 -> C0F4        */
    0x3E, 0x00, 0xEA, 0x00, 0x40,       /* RAM bank 0                    */
    0x3E, 0x01, 0xEA, 0xF0, 0xC0,       /* done flag                     */
    0xFA, 0xF0, 0xC0, 0xB7, 0x20, 0xFA, /* until the test clears it      */
    0xC3, 0x58, 0x01                    /* and go again                  */
  };
  unsigned i, sum = 0;
  unsigned char check = 0;
  memset(rom, 0, size);
  rom[0x100] = 0xC3; rom[0x101] = 0x50; rom[0x102] = 0x01;
  memcpy(rom + 0x104, logo, sizeof(logo));
  memcpy(rom + 0x134, "GBCAMTEST", 9);
  rom[0x147] = 0xFC;                    /* POCKET CAMERA */
  rom[0x148] = 0x01;                    /* 64 KiB: four banks */
  rom[0x4000] = 0xB1; rom[0x8000] = 0xB2; rom[0xC000] = 0xB3;
  rom[0x149] = 0x04;                    /* 128 KiB */
  for (i = 0x134; i <= 0x14C; i++) check = (unsigned char)(check - rom[i] - 1);
  rom[0x14D] = check;
  memcpy(rom + CODE_START, prog, sizeof(prog));
  for (i = 0; i < size; i++) if (i != 0x14E && i != 0x14F) sum += rom[i];
  rom[0x14E] = (unsigned char)(sum >> 8);
  rom[0x14F] = (unsigned char)sum;
}

static int fail(const char *what)
{
  fprintf(stderr, "FAIL gbcam: %s\n", what);
  return 1;
}

static void default_regs(unsigned char r[0x36], unsigned expo)
{
  int i;
  memset(r, 0, 0x36);
  r[0] = 0x03;                       /* start, plain 1-D filter */
  r[1] = 0x04;                       /* gain 20 dB, no edge     */
  r[2] = (unsigned char)(expo >> 8);
  r[3] = (unsigned char)expo;
  for (i = 0; i < 16; i++)           /* every cell: 0x40 / 0x80 / 0xC0 */
  {
    r[6 + i * 3] = 0x40; r[7 + i * 3] = 0x80; r[8 + i * 3] = 0xC0;
  }
}

/* The shade (0 = white .. 3 = black) of a picture pixel in the cartridge's
 * RAM image. */
static int pixel(const unsigned char *img, int x, int y)
{
  const unsigned char *t = img + ((y >> 3) * 16 + (x >> 3)) * 16 + (y & 7) * 2;
  int bit = 7 - (x & 7);
  return ((t[0] >> bit) & 1) | (((t[1] >> bit) & 1) << 1);
}

static gbcore_t *boot(unsigned char *rom, size_t size)
{
  gbcore_callbacks_t cb;
  memset(&cb, 0, sizeof(cb));
  cb.video = on_video;
  cb.audio_batch = on_audio;
  make_rom(rom, size);
  return gbcore_create(NULL, rom, size, FE_CONSOLE_GB, 32768, &cb);
}

/* Runs frames until the program's done flag is set; returns the frame count
 * (or -1). */
static int run_capture(gbcore_t *c, const unsigned char regs[0x36], int max)
{
  unsigned char zero = 0, flag = 0;
  int f;
  gbcore_poke(c, 0xC0F0, &zero, 1);
  gbcore_poke(c, 0xC100, regs, 0x36);
  for (f = 1; f <= max; f++)
  {
    if (gbcore_run_frame(c, 0) != 0)
      return -1;
    gbcore_peek(c, 0xC0F0, &flag, 1);
    if (flag)
      return f;
  }
  return -1;
}

int main(void)
{
  static unsigned char rom[0x10000];
  static unsigned char src[128 * 112], img[0xE00], regs[0x36], again[0xE00];
  gbcore_t *c;
  int x, y, frames, bad;

  c = boot(rom, sizeof(rom));
  if (!c) return fail("boot");
  if (!gbcore_has_camera(c)) return fail("a 0xFC cartridge is a camera");

  /* a known picture: columns step through 0..255 across the width */
  for (y = 0; y < 112; y++)
    for (x = 0; x < 128; x++)
      src[y * 128 + x] = (unsigned char)(x * 2);
  gbcore_set_camera_image(c, src);

  /* 1. Plain picture: 20 dB at exposure 0x1000 passes the source through, so
   * the thresholds 0x40 / 0x80 / 0xC0 give four vertical bands. */
  default_regs(regs, 0x1000);
  frames = run_capture(c, regs, 40);
  printf("capture at exposure 0x1000 took %d frames\n", frames);
  if (frames < 0) return fail("the capture never finished");
  /* 32446 + 512 + 16 * 0x1000 = 98494 cycles = 864 lines = 5.61 frames,
   * and the program notices within a frame or two more */
  if (frames < 6 || frames > 8) return fail("capture time for exposure 0x1000");
  if (gbcore_peek(c, 0xA100, img, sizeof(img)) != 0)
    return fail("read the picture");
  bad = 0;
  for (y = 0; y < 112; y++)
    for (x = 0; x < 128; x++)
    {
      int v = x * 2;
      int level = v < 0x40 ? 0 : v < 0x80 ? 1 : v < 0xC0 ? 2 : 3;
      if (pixel(img, x, y) != 3 - level)
        bad++;
    }
  if (bad) { fprintf(stderr, "%d pixels differ\n", bad); return fail("plain picture bands"); }
  puts("plain picture: four bands, exact");

  /* 2. Exposure halves the level: exposure 0x0800 gives v = x, so only
   * columns >= 0x40 pass the first threshold. */
  default_regs(regs, 0x0800);
  frames = run_capture(c, regs, 40);
  if (frames < 0) return fail("capture 2");
  /* 32446 + 512 + 32768 = 65726 cycles = 577 lines = 3.7 frames */
  if (frames < 4 || frames > 6) return fail("capture time for exposure 0x0800");
  gbcore_peek(c, 0xA100, img, sizeof(img));
  bad = 0;
  for (x = 0; x < 128; x++)
  {
    int level = x < 0x40 ? 0 : x < 0x80 ? 1 : x < 0xC0 ? 2 : 3;
    if (pixel(img, x, 50) != 3 - level) bad++;
  }
  if (bad) return fail("half exposure");
  puts("exposure 0x0800: half the level, exact");

  /* 3. The N bit (A001 bit 7) drops the 512-cycle read lead; the busy
   * time follows the formula. */
  {
    unsigned char r[0x36];
    default_regs(r, 0x0000);
    if (gbcam_capture_cycles(r) != 32446u + 512u) return fail("cycles, N clear");
    r[1] |= 0x80;
    if (gbcam_capture_cycles(r) != 32446u) return fail("cycles, N set");
    r[3] = 0x30;
    if (gbcam_capture_cycles(r) != 32446u + 16u * 0x30) return fail("cycles, exposure");
  }

  /* 4. Invert (A004 bit 3): the bands swap. */
  default_regs(regs, 0x1000);
  regs[4] |= 0x08;
  if (run_capture(c, regs, 40) < 0) return fail("capture 4");
  gbcore_peek(c, 0xA100, img, sizeof(img));
  bad = 0;
  for (x = 0; x < 128; x++)
  {
    int v = 255 - x * 2;
    int level = v < 0x40 ? 0 : v < 0x80 ? 1 : v < 0xC0 ? 2 : 3;
    if (pixel(img, x, 3) != 3 - level) bad++;
  }
  if (bad) return fail("inverted picture");
  puts("invert: bands swapped, exact");

  /* 5. The dither matrix: a flat mid-grey picture with thresholds that
   * differ by cell draws the matrix itself. */
  memset(src, 0x70, sizeof(src));
  gbcore_set_camera_image(c, src);
  default_regs(regs, 0x1000);
  for (x = 0; x < 16; x++)
  {                                  /* cell x: level 1 for even, 2 for odd */
    regs[6 + x * 3] = 0x10; regs[7 + x * 3] = (unsigned char)((x & 1) ? 0x60 : 0xA0);
    regs[8 + x * 3] = 0xF0;
  }
  if (run_capture(c, regs, 40) < 0) return fail("capture 5");
  gbcore_peek(c, 0xA100, img, sizeof(img));
  bad = 0;
  for (y = 0; y < 16; y++)
    for (x = 0; x < 16; x++)
    {
      int cell = (y & 3) * 4 + (x & 3);
      int want = (cell & 1) ? 3 - 2 : 3 - 1;   /* 0x70 vs 0x60: level 2; vs 0xA0: level 1 */
      if (pixel(img, x, y) != want) bad++;
    }
  if (bad) return fail("dither matrix");
  puts("dither matrix: per-cell thresholds, exact");

  /* 6. Edge extraction (N = 1, VH = 3): a flat picture has no edges, so
   * every pixel is the plain level; a step shows an overshoot. */
  memset(src, 0x70, sizeof(src));
  for (y = 0; y < 112; y++)
    for (x = 64; x < 128; x++)
      src[y * 128 + x] = 0x90;
  gbcore_set_camera_image(c, src);
  default_regs(regs, 0x1000);
  regs[1] = 0xE4;                    /* 2-D edge mode, 20 dB */
  regs[4] = 0x20;                    /* ratio 1.0 */
  for (x = 0; x < 16; x++)
  {
    regs[6 + x * 3] = 0x40; regs[7 + x * 3] = 0x80; regs[8 + x * 3] = 0xA0;
  }
  if (run_capture(c, regs, 40) < 0) return fail("capture 6");
  gbcore_peek(c, 0xA100, img, sizeof(img));
  /* flat dark side (0x70 -> 1 step below 0x80): level 1, shade 2; the dark
   * side of the edge is pushed lower by the neighbour (0x70 - 0x20 = 0x50:
   * still level 1); the bright side: 0x90 + 0x20 = 0xB0, level 2 -> shade 1
   * only against the 0xA0 threshold: 0xB0 >= 0xA0 so level 3, shade 0 */
  if (pixel(img, 20, 50) != 2) return fail("edge mode: flat side unchanged");
  if (pixel(img, 63, 50) != 2) return fail("edge mode: dark side of the step");
  if (pixel(img, 64, 50) != 0) return fail("edge mode: bright side overshoots");
  if (pixel(img, 100, 50) != 1) return fail("edge mode: far bright side");
  puts("edge mode: flat areas unchanged, the step overshoots");

  /* 7. Banking, as the program saw it: ROM bank 0 maps at 4000 (a camera
   * allows it), bank 3 is the fourth; RAM banks hold their own bytes. */
  {
    unsigned char r[5];
    gbcore_peek(c, 0xC0F1, r, 4);
    gbcore_peek(c, 0xC0F4, r + 4, 1);
    printf("banks: rom0 %02X rom3 %02X ram0 %02X ram1 %02X\n", r[0], r[1], r[2], r[3]);
    if (r[0] != 0x00 || r[1] != 0xB3) return fail("ROM banking (0 allowed, 3 = fourth)");
    if (r[2] != 0xFF || r[3] != 0x5A) return fail("RAM banking (bank 0 untouched, bank 1 kept the write)");
  }

  /* 8. A state taken mid-capture restores the busy flag and the registers;
   * the picture then lands as it would have. */
  gbcore_shutdown(c);
  c = boot(rom, sizeof(rom));
  gbcore_set_camera_image(c, src);
  default_regs(regs, 0x1000);
  {
    unsigned char zero = 0, flag = 0;
    long n;
    size_t sz;
    unsigned char *st;
    int f;
    gbcore_poke(c, 0xC0F0, &zero, 1);
    gbcore_poke(c, 0xC100, regs, 0x36);
    for (f = 0; f < 3; f++) gbcore_run_frame(c, 0);
    gbcore_peek(c, 0xC0F0, &flag, 1);
    if (flag) return fail("still busy after 3 frames");
    sz = gbcore_state_size(c);
    st = (unsigned char *)malloc(sz);
    n = gbcore_state_save(c, st, sz);
    if (n < 0) return fail("state save");
    for (f = 0; f < 10; f++) gbcore_run_frame(c, 0);
    gbcore_peek(c, 0xA100, img, sizeof(img));
    if (gbcore_state_load(c, st, sz) != 0) return fail("state load");
    gbcore_peek(c, 0xC0F0, &flag, 1);
    if (flag) return fail("the state is from before the capture ended");
    for (f = 0; f < 10; f++) gbcore_run_frame(c, 0);
    gbcore_peek(c, 0xC0F0, &flag, 1);
    if (!flag) return fail("the restored capture finishes");
    gbcore_peek(c, 0xA100, again, sizeof(again));
    if (memcmp(img, again, sizeof(img))) return fail("same picture after a state load");
    free(st);
  }
  puts("state: a capture in progress is restored");

  gbcore_shutdown(c);

  /* 9. The frontend's fit: an 8:7 centre of a 320x240 picture, shrunk to
   * 128x112 by averaging, optionally mirrored. */
  {
    static uint8_t big[320 * 240], fit[128 * 112], mir[128 * 112];
    int bx, by, sum;
    for (by = 0; by < 240; by++)
      for (bx = 0; bx < 320; bx++)
        big[by * 320 + bx] = (uint8_t)(bx < 160 ? 40 : 200);   /* a step in the middle */
    fe_gbcam_fit(big, 320, 240, 320, 0, fit);
    fe_gbcam_fit(big, 320, 240, 320, 1, mir);
    /* the centre 274x240 (x 23..296) is cut in two at x = 160: output
     * column 64 starts at source 23 + 64 * 274 / 128 = 160 */
    if (fit[0] != 40 || fit[63] != 40 || fit[64] != 200 || fit[127] != 200)
      return fail("fit: the step lands at column 64");
    if (mir[0] != 200 || mir[127] != 40 || mir[63] != 200 || mir[64] != 40)
      return fail("fit: mirrored");
    for (sum = 0, by = 0; by < 128 * 112; by++) sum += fit[by];
    if (sum != 64 * 112 * 40 + 64 * 112 * 200) return fail("fit: every row alike");
    /* a picture already 128x112 passes through */
    for (by = 0; by < 128 * 112; by++) big[by] = (uint8_t)(by * 7);
    fe_gbcam_fit(big, 128, 112, 128, 0, fit);
    if (memcmp(fit, big, sizeof(fit))) return fail("fit: 128x112 unchanged");
    /* a tall picture (240x320) is trimmed top and bottom */
    for (by = 0; by < 320; by++)
      for (bx = 0; bx < 240; bx++)
        big[by * 240 + bx] = (uint8_t)(by < 160 ? 10 : 250);
    fe_gbcam_fit(big, 240, 320, 240, 0, fit);
    if (fit[0] != 10 || fit[127 + 128 * 111] != 250) return fail("fit: tall picture");
    puts("fit: centre crop, average, mirror, tall and exact sizes");
  }

  /* 10. A cartridge that is not a camera has none. */
  make_rom(rom, sizeof(rom));
  rom[0x147] = 0x00;
  {
    unsigned i, sum = 0;
    unsigned char check = 0;
    for (i = 0x134; i <= 0x14C; i++) check = (unsigned char)(check - rom[i] - 1);
    rom[0x14D] = check;
    for (i = 0; i < sizeof(rom); i++) if (i != 0x14E && i != 0x14F) sum += rom[i];
    rom[0x14E] = (unsigned char)(sum >> 8);
    rom[0x14F] = (unsigned char)sum;
  }
  {
    gbcore_callbacks_t cb;
    memset(&cb, 0, sizeof(cb));
    cb.video = on_video;
    cb.audio_batch = on_audio;
    c = gbcore_create(NULL, rom, sizeof(rom), FE_CONSOLE_GB, 32768, &cb);
    if (!c || gbcore_has_camera(c)) return fail("ROM only: no camera");
    gbcore_shutdown(c);
  }
  puts("gbcam: ok");
  return 0;
}
