/* Headless runner for the public Game Boy test suites against the Gambatte core (gbcore/gambatte_host.h): the same
 * command line and output lines as suites.c (which runs TGB Dual), read by run_suites.py --core gambatte.
 *   suites_gambatte ROM [--gbc|--dmg] [--frames N] [--ppm FILE]
 * The ROMs are read from disk and never copied anywhere. */
#include "../gambatte_host.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char ser[1 << 16];
static size_t sern;
static const uint16_t *pix;
static unsigned pw, ph, ppitch;

static void on_video(void *, const uint16_t *p, unsigned w, unsigned h, unsigned pitch) { pix = p; pw = w; ph = h; ppitch = pitch; }
static long g_apeak; static double g_asum; static unsigned long g_acount;
static void on_audio(void *u, const int16_t *p, size_t n)
{
  (void)u;
  for (size_t i = 0; i < n * 2; i++) { long v = p[i] < 0 ? -(long)p[i] : p[i]; if (v > g_apeak) g_apeak = v; g_asum += (double)p[i] * p[i]; }
  g_acount += n * 2;
}
static int on_serial(void *, uint8_t out, int, uint8_t *in)
{
  if (sern < sizeof ser - 1) ser[sern++] = (char)out;
  *in = 0xFF;
  return 1;
}

static uint8_t rd(unsigned a)
{
  uint8_t v = 0;
  if (gambatte_host_peek((uint16_t)a, &v, 1) != 0) return 0;
  return v;
}

static int mem_report(char *text, size_t cap, int *status)
{
  size_t i;
  if (rd(0xA001) != 0xDE || rd(0xA002) != 0xB0 || rd(0xA003) != 0x61) return 0;
  *status = rd(0xA000);
  for (i = 0; i + 1 < cap; i++) {
    unsigned char c = rd((unsigned)(0xA004 + i));
    if (!c) break;
    text[i] = (char)c;
  }
  text[i] = 0;
  return 1;
}

static int is_regs(const uint8_t *r, unsigned v0, unsigned v1, unsigned v2, unsigned v3, unsigned v4, unsigned v5)
{
  return r[0] == v0 && r[1] == v1 && r[2] == v2 && r[3] == v3 && r[4] == v4 && r[5] == v5;
}

int main(int argc, char **argv)
{
  const char *rom_path = NULL, *ppm = NULL;
  int mode = -1;
  unsigned frames = 600, f;
  static uint8_t rom[1 << 22];
  size_t n;
  FILE *fp;
  gambatte_host_cb_t cb;
  char text[512];
  int status;
  uint8_t regs[8];
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--gbc")) mode = 1;
    else if (!strcmp(argv[i], "--dmg")) mode = 0;
    else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = (unsigned)atoi(argv[++i]);
    else if (!strcmp(argv[i], "--ppm") && i + 1 < argc) ppm = argv[++i];
    else rom_path = argv[i];
  }
  if (!rom_path) { fprintf(stderr, "usage: suites_gambatte ROM [--gbc|--dmg] [--frames N] [--ppm F]\n"); return 2; }
  fp = fopen(rom_path, "rb");
  if (!fp) { perror(rom_path); return 2; }
  n = fread(rom, 1, sizeof rom, fp);
  fclose(fp);
  if (mode < 0) mode = n > 0x143 && (rom[0x143] & 0x80);
  memset(&cb, 0, sizeof cb);
  cb.video = on_video; cb.audio_batch = on_audio; cb.serial_transfer = on_serial;
  {
    /* the grey palette, no colour model: what suites.c shows */
    static const uint16_t grey[12] = { 0xFFFF, 0xAD55, 0x52AA, 0x0000, 0xFFFF, 0xAD55, 0x52AA, 0x0000, 0xFFFF, 0xAD55, 0x52AA, 0x0000 };
    gambatte_host_set_dmg_colors(grey);
    gambatte_host_set_color_correction(0);
  }
  if (gambatte_host_create(rom, n, mode, 32768, &cb) != 0) { printf("CREATE FAILED\n"); return 2; }
  for (f = 0; f < frames; f++) {
    gambatte_host_run_frame(0);
    ser[sern] = 0;
    if (strstr(ser, "Passed") || strstr(ser, "Failed")) { f++; break; }
    if (mem_report(text, sizeof text, &status) && status != 0x80 && f > 5) { f++; break; }
    if (f > 5) {
      gambatte_host_regs(regs);
      if (is_regs(regs, 3, 5, 8, 13, 21, 34) || is_regs(regs, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42)) { f++; break; }
    }
  }
  ser[sern] = 0;
  for (size_t i = 0; i < sern; i++) if (ser[i] == '\n' || ser[i] == '\r') ser[i] = ' ';
  printf("SERIAL %s\n", ser);
  if (mem_report(text, sizeof text, &status)) {
    for (char *p = text; *p; p++) if (*p == '\n' || *p == '\r') *p = ' ';
    printf("MEM %d %s\n", status, text);
  }
  gambatte_host_regs(regs);
  printf("REGS %u %u %u %u %u %u\n", regs[0], regs[1], regs[2], regs[3], regs[4], regs[5]);
  {
    /* blargg's console draws ASCII into the background map: read it back */
    unsigned base = (rd(0xFF40) & 8) ? 0x9C00 : 0x9800;
    char scr[20 * 19 + 1], *q = scr;
    for (unsigned y = 0; y < 18; y++) {
      for (unsigned x = 0; x < 20; x++) {
        unsigned char c = rd(base + y * 32 + x);
        *q++ = (c >= 0x20 && c < 0x7F) ? (char)c : ' ';
      }
      *q++ = '|';
    }
    *q = 0;
    printf("SCREEN %s\n", scr);
  }
  printf("FRAMES %u\n", f);
  printf("AUDIO %ld %.1f\n", g_apeak, g_acount ? __builtin_sqrt(g_asum / (double)g_acount) : 0.0);
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
  gambatte_host_shutdown();
  return 0;
}
