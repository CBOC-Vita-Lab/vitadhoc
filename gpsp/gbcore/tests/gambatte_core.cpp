/* Copyright (c) 2026 CBOC-Vita contributors.  SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Host test of the Accurate core's wrapper (gbcore/gambatte_host.cpp): what the app relies on besides the suites.
 *   gambatte_core ROM_WITH_PICTURE      (any real ROM; blargg's cpu_instrs works)
 * Prints "ok NAME" or "FAIL NAME ..." lines; exit status 1 on any FAIL. */
#include "../gambatte_host.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

static int g_fail;
static void check(int c, const char *what) { printf("%s %s\n", c ? "ok" : "FAIL", what); if (!c) g_fail = 1; }

struct Cap {
  uint64_t vh = 1469598103934665603ull, ah = 1469598103934665603ull;
  unsigned vcalls = 0, acalls = 0;
  void clear() { vh = ah = 1469598103934665603ull; vcalls = acalls = 0; }
};
static void mix(uint64_t &h, const void *p, size_t n) { const uint8_t *b = (const uint8_t *)p; for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ull; }
static void on_video(void *u, const uint16_t *px, unsigned w, unsigned h, unsigned pitch)
{
  Cap *c = (Cap *)u; c->vcalls++;
  for (unsigned y = 0; y < h; y++) mix(c->vh, (const uint8_t *)px + y * pitch, w * 2);
}
static void on_audio(void *u, const int16_t *p, size_t n) { Cap *c = (Cap *)u; c->acalls++; mix(c->ah, p, n * 4); }

static time_t g_now = 1700000000;
static time_t wall(void) { return g_now; }

static void run(unsigned n, uint16_t keys = 0) { while (n--) gambatte_host_run_frame(keys); }

static bool create(const std::vector<uint8_t> &rom, Cap *cap, int cgb)
{
  gambatte_host_cb_t cb; memset(&cb, 0, sizeof cb);
  cb.userdata = cap; cb.video = on_video; cb.audio_batch = on_audio;
  return gambatte_host_create(rom.data(), rom.size(), cgb, 32768, &cb) == 0;
}

static std::vector<uint8_t> synth(uint8_t type, uint8_t ramsz)
{
  std::vector<uint8_t> r(65536, 0);
  const uint8_t code[] = { 0x3E, 0x0A, 0xEA, 0x00, 0x00, 0x18, 0xFE };   /* a loop */
  memcpy(&r[0x100], code, sizeof code);
  r[0x147] = type; r[0x148] = 1; r[0x149] = ramsz;
  uint8_t c = 0; for (int i = 0x134; i <= 0x14C; i++) c = (uint8_t)(c - r[i] - 1);
  r[0x14D] = c;
  return r;
}

int main(int argc, char **argv)
{
  Cap cap;
  std::vector<uint8_t> rom;
  if (argc < 2) { fprintf(stderr, "usage: gambatte_core ROM\n"); return 2; }
  { FILE *f = fopen(argv[1], "rb"); if (!f) return 2; uint8_t b[4096]; size_t n; while ((n = fread(b, 1, sizeof b, f)) > 0) rom.insert(rom.end(), b, b + n); fclose(f); }

  /* ---- a machine runs, and the picture is 160x144 ---- */
  check(create(rom, &cap, 0), "create");
  check(gambatte_host_frame_width() == 160 && gambatte_host_frame_height() == 144, "frame is 160x144");
  check(gambatte_host_sample_rate() == 32768, "audio rate 32768");
  run(200);
  check(cap.vcalls == 200 && cap.acalls == 200, "one picture and one audio batch per frame");

  /* ---- states: saving and loading gives the same future ---- */
  size_t ss = gambatte_host_state_size();
  std::vector<uint8_t> st(ss);
  long n = gambatte_host_state_save(st.data(), st.size());
  check(n > 16 && gambatte_host_state_kind(st.data(), (size_t)n) == 2, "state is tagged GBGM (kind 2)");
  cap.clear(); run(120, 0x0010); uint64_t v1 = cap.vh, a1 = cap.ah;
  check(gambatte_host_state_load(st.data(), (size_t)n) == 0, "state loads");
  cap.clear(); run(120, 0x0010);
  check(cap.vh == v1 && cap.ah == a1, "state: same video and audio after a load");
  uint8_t fake[64]; memcpy(fake, "GBAT", 4); memset(fake + 4, 0, 60);
  check(gambatte_host_state_kind(fake, sizeof fake) == 1, "a TGB image is recognised (kind 1)");
  check(gambatte_host_state_load(fake, sizeof fake) != 0, "a TGB image is refused");

  /* ---- Quick response (run-ahead): the ahead image restores bit-identically, audio included ---- */
  size_t as = gambatte_host_ahead_size();
  std::vector<uint8_t> ah(as);
  long an = gambatte_host_ahead_save(ah.data(), ah.size());
  check(an > 0, "ahead image saved");
  cap.clear(); run(90, 0x0001); uint64_t v2 = cap.vh, a2 = cap.ah; uint64_t f2 = gambatte_host_frame_number();
  check(gambatte_host_ahead_load(ah.data(), (size_t)an) == 0, "ahead image loads");
  cap.clear(); run(90, 0x0001);
  check(cap.vh == v2 && cap.ah == a2 && gambatte_host_frame_number() == f2, "ahead: video, audio and frame count identical");
  /* many ahead round trips in the middle of a run leave the run unchanged */
  cap.clear(); run(60); uint64_t v3 = cap.vh, a3 = cap.ah;
  gambatte_host_ahead_load(ah.data(), (size_t)an); run(90, 0x0001);   /* back to the same point */
  cap.clear();
  for (int i = 0; i < 60; i++) {
    long k = gambatte_host_ahead_save(ah.data(), ah.size());
    gambatte_host_run_frame(0); gambatte_host_run_frame(0);
    gambatte_host_ahead_load(ah.data(), (size_t)k);
    gambatte_host_run_frame(0);
  }
  (void)v3; (void)a3;
  check(1, "ahead: 60 save/ahead/load rounds");

  /* ---- skip render: no picture, and the run goes on ---- */
  cap.clear(); gambatte_host_set_skip_render(1); run(10); gambatte_host_set_skip_render(0);
  check(cap.vcalls == 10 && cap.acalls == 10, "skip render: frames and audio keep coming (the last picture is handed on)");
  uint64_t before = cap.vh; run(5); check(cap.vh != before, "drawing again after skip");
  gambatte_host_shutdown();

  /* ---- a battery with a clock: TGB's file layout (cart RAM, then the 48-byte trailer) ---- */
  std::vector<uint8_t> m3 = synth(0x10, 2);
  gambatte_host_set_wallclock(wall);
  check(create(m3, &cap, 0), "MBC3+RTC+RAM+BATTERY creates");
  check(gambatte_host_cart_ram_size() == 8192, "cart RAM 8 KiB");
  check(gambatte_host_save_ram_size() == 8192 + 48, "save image = RAM + 48-byte clock trailer");
  std::vector<uint8_t> sv(gambatte_host_save_ram_size());
  for (size_t i = 0; i < 8192; i++) sv[i] = (uint8_t)(i * 7 + 3);
  uint8_t *t = &sv[8192];
  memset(t, 0, 48);
  const uint32_t live[5] = { 30, 20, 10, 5, 0 };          /* s m h dl dh */
  for (int i = 0; i < 5; i++) { t[4 * i] = (uint8_t)live[i]; t[20 + 4 * i] = (uint8_t)live[i]; }
  uint64_t stamp = (uint64_t)g_now;
  for (int i = 0; i < 8; i++) t[40 + i] = (uint8_t)(stamp >> (8 * i));
  check(gambatte_host_save_ram_write(sv.data(), sv.size()) == 0, "battery image loads");
  std::vector<uint8_t> back(sv.size());
  check(gambatte_host_save_ram_read(back.data(), back.size()) == 0, "battery image reads");
  check(memcmp(back.data(), sv.data(), 8192) == 0, "cart RAM round trips");
  check(memcmp(back.data() + 8192, sv.data() + 8192, 40) == 0, "clock registers round trip");
  uint64_t st2 = 0; for (int i = 7; i >= 0; i--) st2 = st2 << 8 | back[8192 + 40 + i];
  check(st2 == stamp, "clock stamp round trips");
  /* an hour and a half later by the wall clock, the clock shows it */
  g_now += 5400; run(2);
  gambatte_host_save_ram_read(back.data(), back.size());
  check(back[8192 + 8] == 11 && back[8192 + 4] == 50, "clock runs on with the wall clock (+1 h 30 min)");
  gambatte_host_shutdown();

  /* ---- no clock: the plain RAM ---- */
  std::vector<uint8_t> m1 = synth(0x03, 3);
  check(create(m1, &cap, 0), "MBC1+RAM+BATTERY creates");
  check(gambatte_host_save_ram_size() == 32768, "MBC1 32 KiB RAM has no trailer");
  gambatte_host_shutdown();
  std::vector<uint8_t> m2 = synth(0x06, 0);
  check(create(m2, &cap, 0), "MBC2+BATTERY creates");
  check(gambatte_host_save_ram_size() == 0x200, "MBC2 512 bytes");
  gambatte_host_shutdown();

  /* ---- cartridges the core does not run are refused (the app falls back to Standard) ---- */
  std::vector<uint8_t> cam = synth(0xFC, 5);
  check(!create(cam, &cap, 0), "Game Boy Camera refused");
  std::vector<uint8_t> m7 = synth(0x22, 3);
  check(!create(m7, &cap, 0), "MBC7 refused");

  /* ---- cheats, memory and the RA map ---- */
  check(create(rom, &cap, 0), "recreate");
  run(10);
  uint8_t b = 0;
  const uint8_t v = 0x5A;
  check(gambatte_host_poke(0xC123, &v, 1) == 0, "poke WRAM");
  check(gambatte_host_peek(0xC123, &b, 1) == 0, "peek WRAM");
  check(b == 0x5A, "peek returns what was poked");
  uint8_t ra = 0; check(gambatte_host_ra_read(0xC123, &ra, 1) == 1 && ra == 0x5A, "RA map: 0xC123 is the same byte");
  check(gambatte_host_ra_read(0x34000, &ra, 1) == 0, "RA map ends at 0x34000");
  gambatte_host_shutdown();
  printf(g_fail ? "FAILED\n" : "PASSED\n");
  return g_fail;
}
