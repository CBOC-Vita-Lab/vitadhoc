/* Headless runner for the public Game Boy test suites (blargg, mooneye, the
 * acid2 pictures).  One ROM per run; prints what each suite reports.
 *
 *   suites ROM [--gbc|--dmg] [--frames N] [--ppm FILE]
 *
 * Lines on stdout (the driver, tests/run_suites.py, reads them):
 *   SERIAL <text>            bytes the ROM sent out of the serial port
 *   MEM <status> <text>      blargg's cartridge-RAM report (A000 status,
 *                            A004 text) when its signature DE B0 61 is there
 *   REGS B C D E H L         registers when the run ended
 *   FRAMES n                 frames run (stops early once a verdict is in)
 * The ROMs are read from disk and never copied anywhere. */
#include "../gbcore.h"
#include "../tgbdual/gb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char ser[1 << 16];
static size_t sern;
static const uint16_t *pix;
static unsigned pw, ph, ppitch;

static void on_video(void *u, const gbcore_video_frame_t *f)
{
  (void)u;
  pix = f->pixels; pw = f->width; ph = f->height; ppitch = f->pitch_bytes;
}
static void on_audio(void *u, const int16_t *p, size_t n) { (void)u; (void)p; (void)n; }
static int on_serial(void *u, uint8_t out, int internal, uint8_t *in)
{
  (void)u; (void)internal;
  if (sern < sizeof ser - 1) ser[sern++] = (char)out;
  *in = 0xFF;
  return 1;
}

static int mem_report(char *text, size_t cap, int *status)
{
  size_t i;
  if (cpu_read(0xA001) != 0xDE || cpu_read(0xA002) != 0xB0 || cpu_read(0xA003) != 0x61)
    return 0;
  *status = cpu_read(0xA000);
  for (i = 0; i + 1 < cap; i++) {
    unsigned char c = cpu_read((word)(0xA004 + i));
    if (!c) break;
    text[i] = (char)c;
  }
  text[i] = 0;
  return 1;
}

static int fib(void)
{
  struct cpu_regs *r = cpu_get_c_regs();
  return r->BC.b.h == 3 && r->BC.b.l == 5 && r->DE.b.h == 8 && r->DE.b.l == 13 &&
         r->HL.b.h == 21 && r->HL.b.l == 34;
}
static int fail42(void)
{
  struct cpu_regs *r = cpu_get_c_regs();
  return r->BC.b.h == 0x42 && r->BC.b.l == 0x42 && r->DE.b.h == 0x42 &&
         r->DE.b.l == 0x42 && r->HL.b.h == 0x42 && r->HL.b.l == 0x42;
}

int main(int argc, char **argv)
{
  const char *rom_path = NULL, *ppm = NULL;
  int mode = -1; /* -1 auto from header */
  unsigned frames = 600, f;
  static uint8_t rom[1 << 22];
  size_t n;
  FILE *fp;
  gbcore_callbacks_t cb;
  gbcore_t *core;
  char text[512];
  int status;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--gbc")) mode = 1;
    else if (!strcmp(argv[i], "--dmg")) mode = 0;
    else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = (unsigned)atoi(argv[++i]);
    else if (!strcmp(argv[i], "--ppm") && i + 1 < argc) ppm = argv[++i];
    else rom_path = argv[i];
  }
  if (!rom_path) { fprintf(stderr, "usage: suites ROM [--gbc|--dmg] [--frames N] [--ppm F]\n"); return 2; }
  fp = fopen(rom_path, "rb");
  if (!fp) { perror(rom_path); return 2; }
  n = fread(rom, 1, sizeof rom, fp);
  fclose(fp);
  if (mode < 0) mode = n > 0x143 && (rom[0x143] & 0x80);
  memset(&cb, 0, sizeof cb);
  cb.video = on_video; cb.audio_batch = on_audio; cb.serial_transfer = on_serial;
  gbcore_set_palette(GBCORE_PALETTE_GREY);
  gbcore_set_color_correction(0);
  core = gbcore_create(NULL, rom, n, mode ? FE_CONSOLE_GBC : FE_CONSOLE_GB, 32768, &cb);
  if (!core) { printf("CREATE FAILED\n"); return 2; }
  for (f = 0; f < frames; f++) {
    gbcore_run_frame(core, 0);
    ser[sern] = 0;
    if (strstr(ser, "Passed") || strstr(ser, "Failed")) { f++; break; }
    if (mem_report(text, sizeof text, &status) && status != 0x80 && f > 5) { f++; break; }
    if (f > 5 && (fib() || fail42())) { f++; break; }
  }
  ser[sern] = 0;
  for (size_t i = 0; i < sern; i++) if (ser[i] == '\n' || ser[i] == '\r') ser[i] = ' ';
  printf("SERIAL %s\n", ser);
  if (mem_report(text, sizeof text, &status)) {
    for (char *p = text; *p; p++) if (*p == '\n' || *p == '\r') *p = ' ';
    printf("MEM %d %s\n", status, text);
  }
  {
    struct cpu_regs *r = cpu_get_c_regs();
    printf("REGS %u %u %u %u %u %u\n", r->BC.b.h, r->BC.b.l, r->DE.b.h, r->DE.b.l, r->HL.b.h, r->HL.b.l);
  }
  {
    /* blargg's console draws ASCII into the background map: read it back */
    word base = (cpu_read(0xFF40) & 8) ? 0x9C00 : 0x9800;
    char scr[20 * 19 + 1], *q = scr;
    for (unsigned y = 0; y < 18; y++) {
      for (unsigned x = 0; x < 20; x++) {
        unsigned char c = cpu_read((word)(base + y * 32 + x));
        *q++ = (c >= 0x20 && c < 0x7F) ? (char)c : ' ';
      }
      *q++ = '|';
    }
    *q = 0;
    printf("SCREEN %s\n", scr);
  }
  printf("FRAMES %u\n", f);
  if (ppm && pix) {
    FILE *o = fopen(ppm, "wb");
    if (o) {
      fprintf(o, "P6\n%u %u\n255\n", pw, ph);
      for (unsigned y = 0; y < ph; y++)
        for (unsigned x = 0; x < pw; x++) {
          uint16_t v = ((const uint16_t *)((const uint8_t *)pix + y * ppitch))[x];
          unsigned r5 = v >> 11, g6 = (v >> 5) & 63, b5 = v & 31;
          fputc((int)(r5 << 3 | r5 >> 2), o); fputc((int)(g6 << 2 | g6 >> 4), o); fputc((int)(b5 << 3 | b5 >> 2), o);
        }
      fclose(o);
    }
  }
  gbcore_shutdown(core);
  return 0;
}
