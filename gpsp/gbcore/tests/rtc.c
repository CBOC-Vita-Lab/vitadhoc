/* The MBC3 cartridge clock (Pokemon Gold/Silver/Crystal) on its edges, with
 * a wall clock the test owns.
 *
 * A generated MBC3+TIMER+RAM+BATTERY ROM latches the clock every pass of
 * its loop and copies the five registers, read through the cartridge as a
 * game reads them, into WRAM C000-C004; a command at C010-C012 makes it
 * write one register.  So every check below is of what THE GAME sees, not of
 * the adapter's own bookkeeping.  Battery images are built and inspected in
 * the common layout (bgb.bircd.org/rtcsave.html: five 32-bit live registers,
 * five latched, 64-bit little-endian UNIX time; a 44-byte form has a 32-bit
 * stamp).
 *
 * Covered: a fresh cartridge; the clock following the wall; save and reload
 * with time passing; 48- and 44-byte trailers from another emulator; cart
 * RAM alone and every other size (RAM kept, clock from zero, the next image
 * valid); a stamp from the future, by a second, by hours and the format's
 * "no clock" value (registers stand -- this used to reset the clock); a
 * stamp before 1997 (not clock data); the wall clock going backwards a
 * little and a lot; the 9-bit day counter wrapping at load and while
 * running; a clock halted in the save and halted by the game; a hook with
 * no reading (returns 0) at boot, mid-game and at save; a save state not
 * moving the clock; and the load a link session does (stamp == now) giving
 * exactly the registers.
 */
#include "../gbcore.h"

#include <stdio.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { \
   fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); return 1; \
} } while (0)

#define RAM 0x8000u
#define DAY 86400
#define T0 1790000000ll               /* 2026-09-21, an arbitrary "now" */
#define NO_CLOCK 0x7fffffff7fffffffll

static time_t wall;
static time_t wall_clock(void) { return wall; }

static unsigned char rom[0x8000];

static void make_rom(void)
{
  static const unsigned char logo[48] = {
    0xCE,0xED,0x66,0x66,0xCC,0x0D,0x00,0x0B,0x03,0x73,0x00,0x83,
    0x00,0x0C,0x00,0x0D,0x00,0x08,0x11,0x1F,0x88,0x89,0x00,0x0E,
    0xDC,0xCC,0x6E,0xE6,0xDD,0xDD,0xD9,0x99,0xBB,0xBB,0x67,0x63,
    0x6E,0x0E,0xEC,0xCC,0xDD,0xDC,0x99,0x9F,0xBB,0xB9,0x33,0x3E
  };
  static const unsigned char program[] = {
    0x31, 0xFE, 0xFF,       /* 0150 LD SP,$FFFE                 */
    0x3E, 0x0A,             /* 0153 LD A,$0A                    */
    0xEA, 0x00, 0x00,       /* 0155 LD ($0000),A   RAM/RTC on   */
    0xAF,                   /* 0158 XOR A                 loop: */
    0xEA, 0x00, 0x60,       /* 0159 LD ($6000),A   latch 0      */
    0x3C,                   /* 015C INC A                       */
    0xEA, 0x00, 0x60,       /* 015D LD ($6000),A   latch 1      */
    0x21, 0x00, 0xC0,       /* 0160 LD HL,$C000                 */
    0x06, 0x08,             /* 0163 LD B,$08                    */
    0x78,                   /* 0165 LD A,B                  rd: */
    0xEA, 0x00, 0x40,       /* 0166 LD ($4000),A   select reg   */
    0xFA, 0x00, 0xA0,       /* 0169 LD A,($A000)                */
    0x22,                   /* 016C LD (HL+),A                  */
    0x04,                   /* 016D INC B                       */
    0x78,                   /* 016E LD A,B                      */
    0xFE, 0x0D,             /* 016F CP $0D                      */
    0x20, 0xF2,             /* 0171 JR NZ,rd                    */
    0xFA, 0x10, 0xC0,       /* 0173 LD A,($C010)   command?     */
    0xB7,                   /* 0176 OR A                        */
    0x28, 0x10,             /* 0177 JR Z,next                   */
    0xFA, 0x11, 0xC0,       /* 0179 LD A,($C011)   register     */
    0xEA, 0x00, 0x40,       /* 017C LD ($4000),A                */
    0xFA, 0x12, 0xC0,       /* 017F LD A,($C012)   value        */
    0xEA, 0x00, 0xA0,       /* 0182 LD ($A000),A   write it     */
    0xAF,                   /* 0185 XOR A                       */
    0xEA, 0x10, 0xC0,       /* 0186 LD ($C010),A   done         */
    0x18, 0xCD              /* 0189 JR loop               next: */
  };
  unsigned i;
  unsigned char check = 0;
  memset(rom, 0, sizeof(rom));
  rom[0x100] = 0xC3; rom[0x101] = 0x50; rom[0x102] = 0x01;
  memcpy(rom + 0x104, logo, sizeof(logo));
  memcpy(rom + 0x134, "RTC TEST", 8);
  rom[0x143] = 0x80;
  rom[0x147] = 0x10;                  /* MBC3+TIMER+RAM+BATTERY */
  rom[0x149] = 0x03;                  /* 32 KiB */
  for (i = 0x134; i <= 0x14C; i++) check = (unsigned char)(check - rom[i] - 1);
  rom[0x14D] = check;
  memcpy(rom + 0x150, program, sizeof(program));
}

static void put32(unsigned char *p, unsigned v)
{
  p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8);
  p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}

static unsigned get32(const unsigned char *p)
{
  return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) |
         ((unsigned)p[3] << 24);
}

static long long get_stamp(const unsigned char *img)
{
  return (long long)get32(img + RAM + 40) |
         ((long long)get32(img + RAM + 44) << 32);
}

/* Seconds the live registers of a battery image, or of the five bytes the
 * game read, stand for. */
static long long count_of(unsigned s, unsigned m, unsigned h, unsigned dl,
                          unsigned dh)
{
  return (long long)s + m * 60ll + h * 3600ll +
         (long long)(dl | ((dh & 1) << 8)) * DAY;
}

static long long image_count(const unsigned char *img)
{
  const unsigned char *t = img + RAM;
  return count_of(get32(t), get32(t + 4), get32(t + 8), get32(t + 12),
                  get32(t + 16));
}

/* A battery image as another emulator writes it: the clock at `count`
 * seconds with flag bits `flags` (0x40 halted, 0x80 day carry), stamped
 * `stamp`. */
static unsigned char image[RAM + 64], out[RAM + 64];

static size_t make_image(long long count, unsigned flags, long long stamp,
                         unsigned trailer)
{
  unsigned char *t = image + RAM;
  unsigned i;
  for (i = 0; i < RAM; i++) image[i] = (unsigned char)(i * 11 + 1);
  memset(t, 0, 64);
  put32(t, (unsigned)(count % 60));
  put32(t + 4, (unsigned)(count / 60 % 60));
  put32(t + 8, (unsigned)(count / 3600 % 24));
  put32(t + 12, (unsigned)(count / DAY) & 0xff);
  put32(t + 16, (unsigned)((count / DAY) >> 8 & 1) | flags);
  for (i = 0; i < 20; i += 4)         /* latched = live, as mGBA writes */
    put32(t + 20 + i, get32(t + i));
  put32(t + 40, (unsigned)((unsigned long long)stamp & 0xffffffffu));
  put32(t + 44, (unsigned)((unsigned long long)stamp >> 32));
  return RAM + trailer;
}

static gbcore_t *core;

static int boot(void)
{
  gbcore_callbacks_t cb = {0};
  gbcore_set_wallclock(wall_clock);
  core = gbcore_create(NULL, rom, sizeof(rom), FE_CONSOLE_GBC, 32768, &cb);
  CHECK(core != NULL);
  CHECK(gbcore_save_ram_size(core) == RAM + 48);
  CHECK(gbcore_cart_ram_size(core) == RAM);
  return 0;
}

/* What the game reads now: the count in seconds, and the flag bits. */
static unsigned game_flags;
static long long game_count(void)
{
  unsigned char r[5];
  gbcore_run_frame(core, 0);
  gbcore_run_frame(core, 0);
  if (gbcore_peek(core, 0xC000, r, 5) != 0)
    return -1;
  game_flags = r[4] & 0xC0;
  /* Registers in range: a game that trusts them must be able to. */
  if (r[0] > 59 || r[1] > 59 || r[2] > 23 || (r[4] & 0x3E))
    return -2;
  return count_of(r[0], r[1], r[2], r[3], r[4]);
}

/* The game writes one clock register (8 seconds .. 12 day high/flags). */
static int game_write(unsigned reg, unsigned value)
{
  unsigned char cmd[3];
  cmd[0] = 1; cmd[1] = (unsigned char)reg; cmd[2] = (unsigned char)value;
  CHECK(gbcore_poke(core, 0xC010, cmd, 3) == 0);
  gbcore_run_frame(core, 0);
  gbcore_run_frame(core, 0);
  CHECK(gbcore_peek(core, 0xC010, cmd, 1) == 0 && cmd[0] == 0);
  return 0;
}

static int ram_kept(void)
{
  unsigned i;
  CHECK(gbcore_save_ram_read(core, out, sizeof(out)) == 0);
  for (i = 0; i < RAM; i++)
    CHECK(out[i] == (unsigned char)(i * 11 + 1));
  return 0;
}

/* Boots at T0 and loads `size` bytes of `image`. */
static int boot_load(size_t size)
{
  wall = T0;
  CHECK(boot() == 0);
  CHECK(gbcore_save_ram_write(core, image, size) == 0);
  return 0;
}

static int test_fresh_and_running(void)
{
  wall = T0;
  CHECK(boot() == 0);
  CHECK(game_count() == 0 && game_flags == 0);
  wall += 100;
  CHECK(game_count() == 100);
  wall += 3 * DAY + 5 * 3600 + 7 * 60 + 9 - 100;
  CHECK(game_count() == 3 * DAY + 5 * 3600 + 7 * 60 + 9);
  /* The image: live registers at this instant, stamped with it. */
  CHECK(gbcore_save_ram_read(core, out, sizeof(out)) == 0);
  CHECK(image_count(out) == 3 * DAY + 5 * 3600 + 7 * 60 + 9);
  CHECK(get_stamp(out) == (long long)wall);
  /* The format's registers are bytes in 32-bit fields, the rest 0. */
  {
    unsigned i;
    for (i = 0; i < 40; i += 4)
      CHECK(get32(out + RAM + i) <= 0xff);
  }
  gbcore_shutdown(core);
  printf("rtc: fresh cartridge at zero, follows the wall, image stamped\n");
  return 0;
}

static int test_round_trip(void)
{
  long long c;
  wall = T0;
  CHECK(boot() == 0);
  CHECK(game_write(10, 13) == 0);     /* the player sets 13:xx */
  CHECK(game_write(11, 2) == 0);      /* day 2 */
  wall += 250;
  c = game_count();
  CHECK(c >= 2 * DAY + 13 * 3600 + 250 && c < 2 * DAY + 13 * 3600 + 260);
  CHECK(gbcore_save_ram_read(core, image, sizeof(image)) == 0);
  gbcore_shutdown(core);
  /* Off for 7 h 3 s, then the same file again -- twice, to show that
   * loading without saving does not compound. */
  wall += 7 * 3600 + 3;
  CHECK(boot() == 0);
  CHECK(gbcore_save_ram_write(core, image, RAM + 48) == 0);
  CHECK(game_count() == c + 7 * 3600 + 3);
  gbcore_shutdown(core);
  wall += 40;
  CHECK(boot() == 0);
  CHECK(gbcore_save_ram_write(core, image, RAM + 48) == 0);
  CHECK(game_count() == c + 7 * 3600 + 43);
  gbcore_shutdown(core);
  printf("rtc: save, power off, reload: the clock advanced by the time off\n");
  return 0;
}

static int test_import(unsigned trailer)
{
  long long c0 = 5 * DAY + 3 * 3600 + 2 * 60 + 10;
  CHECK(boot_load(make_image(c0, 0, T0 - 100, trailer)) == 0);
  CHECK(game_count() == c0 + 100);
  CHECK(ram_kept() == 0);
  CHECK(get_stamp(out) == T0);        /* always written as 48 bytes */
  gbcore_shutdown(core);
  /* Day 300: bit 8 of the day counter lives in the flags register. */
  c0 = 300ll * DAY + 59;
  CHECK(boot_load(make_image(c0, 0, T0 - 1, trailer)) == 0);
  CHECK(game_count() == c0 + 1);
  gbcore_shutdown(core);
  printf("rtc: %u-byte trailer from another emulator: clock resumed\n",
         trailer);
  return 0;
}

static int test_no_trailer_sizes(void)
{
  /* Cart RAM alone (a cartridge dump, RetroArch's .srm beside an .rtc, a
   * flash cart), a 3DS Virtual Console file (16 more bytes), near misses of
   * the trailer sizes, a short file and a padded one. */
  static const size_t sizes[] = { RAM, RAM + 16, RAM + 43, RAM + 45, RAM + 47,
                                  RAM + 49, RAM + 64, 0x2000, 0x2000 + 48,
                                  RAM - 1, 1 };
  unsigned i, k;
  for (k = 0; k < sizeof(sizes) / sizeof(sizes[0]); k++)
  {
    make_image(77 * DAY, 0x80, T0 - 50, 48);
    CHECK(boot_load(sizes[k]) == 0);
    wall += 9;
    CHECK(game_count() == 9 && game_flags == 0);   /* from zero, running */
    CHECK(gbcore_save_ram_read(core, out, sizeof(out)) == 0);
    for (i = 0; i < RAM && i < sizes[k]; i++)
      CHECK(out[i] == (unsigned char)(i * 11 + 1));
    /* The next save is a valid image: reloaded, the clock continues. */
    CHECK(get_stamp(out) == (long long)wall && image_count(out) == 9);
    gbcore_shutdown(core);
    memcpy(image, out, RAM + 48);
    wall += 1000;
    CHECK(boot() == 0);
    CHECK(gbcore_save_ram_write(core, image, RAM + 48) == 0);
    CHECK(game_count() == 1009);
    gbcore_shutdown(core);
  }
  printf("rtc: %u sizes without a usable trailer: RAM kept, clock from zero, "
         "next image valid\n", k);
  return 0;
}

static int test_future_stamp(void)
{
  static const long long ahead[] = { 1, 3600, 14 * 3600, 400ll * DAY,
                                     NO_CLOCK - T0 };
  long long c0 = 9 * DAY + 20 * 3600 + 15;
  unsigned k;
  for (k = 0; k < sizeof(ahead) / sizeof(ahead[0]); k++)
  {
    CHECK(boot_load(make_image(c0, 0, T0 + ahead[k], 48)) == 0);
    CHECK(game_count() == c0 && game_flags == 0);  /* registers stand */
    wall += 30;
    CHECK(game_count() == c0 + 30);                /* and it runs */
    /* Saved again it is an ordinary image stamped now. */
    CHECK(gbcore_save_ram_read(core, out, sizeof(out)) == 0);
    CHECK(get_stamp(out) == (long long)wall && image_count(out) == c0 + 30);
    gbcore_shutdown(core);
  }
  /* The 44-byte form of "no clock": 0x7fffffff. */
  CHECK(boot_load(make_image(c0, 0, 0x7fffffffll, 44)) == 0);
  CHECK(game_count() == c0);
  gbcore_shutdown(core);
  printf("rtc: stamp from the future (1 s .. the format's no-clock value): "
         "registers kept, no time added\n");
  return 0;
}

static int test_ancient_stamp(void)
{
  static const long long stamps[] = { 0, 1, 852076799ll };
  unsigned k;
  for (k = 0; k < 3; k++)
  {
    CHECK(boot_load(make_image(9 * DAY, 0xC0, stamps[k], 48)) == 0);
    CHECK(game_count() == 0 && game_flags == 0);
    gbcore_shutdown(core);
  }
  CHECK(boot_load(make_image(9 * DAY, 0, 852076800ll, 48)) == 0);
  CHECK(game_count() == (9 * DAY + (T0 - 852076800ll)) % (512ll * DAY));
  CHECK(game_flags == 0x80);          /* 29 years off: the day carry */
  gbcore_shutdown(core);
  printf("rtc: stamp before 1997 is not clock data: clock from zero\n");
  return 0;
}

static int test_backwards(void)
{
  long long c0 = 2 * DAY + 100;
  CHECK(boot_load(make_image(c0, 0, T0, 48)) == 0);
  wall += 500;
  CHECK(game_count() == c0 + 500);
  /* A small step back: the count follows the wall, never below zero. */
  wall -= 700;
  CHECK(game_count() == c0 - 200);
  wall += 200;
  CHECK(game_count() == c0);
  /* Further back than the count is long: it holds at zero, then runs. */
  wall -= 10ll * DAY;
  CHECK(game_count() == 0 && game_flags == 0);
  wall += 60;
  CHECK(game_count() == 60);
  gbcore_shutdown(core);
  /* Between sessions: saved, the console's clock set back a year, loaded.
   * The stamp is now in the future: the clock picks up where it was. */
  CHECK(boot_load(make_image(c0, 0, T0, 48)) == 0);
  wall += 10;
  CHECK(gbcore_save_ram_read(core, image, sizeof(image)) == 0);
  gbcore_shutdown(core);
  wall -= 365ll * DAY;
  CHECK(boot() == 0);
  CHECK(gbcore_save_ram_write(core, image, RAM + 48) == 0);
  CHECK(game_count() == c0 + 10);
  wall += 5;
  CHECK(game_count() == c0 + 15);
  gbcore_shutdown(core);
  printf("rtc: wall clock set back: follows a small step, holds at zero "
         "past its start, resumes after a reload\n");
  return 0;
}

static int test_day_wrap(void)
{
  long long c0 = 511ll * DAY + 23 * 3600 + 59 * 60 + 50;
  /* While running. */
  CHECK(boot_load(make_image(c0, 0, T0, 48)) == 0);
  CHECK(game_count() == c0 && game_flags == 0);
  wall += 15;
  CHECK(game_count() == 5 && game_flags == 0x80);
  /* The carry stays until the game clears it, and survives a save. */
  wall += DAY;
  CHECK(game_count() == DAY + 5 && game_flags == 0x80);
  CHECK(gbcore_save_ram_read(core, image, sizeof(image)) == 0);
  CHECK(get32(image + RAM + 16) == 0x80);
  gbcore_shutdown(core);
  CHECK(boot() == 0);
  CHECK(gbcore_save_ram_write(core, image, RAM + 48) == 0);
  CHECK(game_count() == DAY + 5 && game_flags == 0x80);
  CHECK(game_write(12, 0) == 0);      /* the game acknowledges it */
  CHECK(game_count() == DAY + 5 && game_flags == 0);
  gbcore_shutdown(core);
  /* At load: off for 600 days, and for three whole spans. */
  CHECK(boot_load(make_image(10 * DAY, 0, T0 - 600ll * DAY, 48)) == 0);
  CHECK(game_count() == 98 * DAY && game_flags == 0x80);
  gbcore_shutdown(core);
  CHECK(boot_load(make_image(10 * DAY + 7, 0, T0 - 3 * 512ll * DAY, 48)) == 0);
  CHECK(game_count() == 10 * DAY + 7 && game_flags == 0x80);
  gbcore_shutdown(core);
  /* 511 days off does not wrap. */
  CHECK(boot_load(make_image(0, 0, T0 - 511ll * DAY, 48)) == 0);
  CHECK(game_count() == 511ll * DAY && game_flags == 0);
  gbcore_shutdown(core);
  printf("rtc: day counter wraps at 512 with the carry bit, running and at "
         "load\n");
  return 0;
}

static int test_halted(void)
{
  long long c0 = 4 * DAY + 3600;
  /* Halted in the save: the time off is not added. */
  CHECK(boot_load(make_image(c0, 0x40, T0 - 5000, 48)) == 0);
  CHECK(game_count() == c0 && game_flags == 0x40);
  wall += 300;
  CHECK(game_count() == c0 && game_flags == 0x40);
  /* Saved halted and reloaded later: still there, still halted. */
  CHECK(gbcore_save_ram_read(core, image, sizeof(image)) == 0);
  CHECK(image_count(image) == c0 && (get32(image + RAM + 16) & 0x40));
  gbcore_shutdown(core);
  wall += 9999;
  CHECK(boot() == 0);
  CHECK(gbcore_save_ram_write(core, image, RAM + 48) == 0);
  CHECK(game_count() == c0 && game_flags == 0x40);
  /* The game sets the time while halted (as Crystal does), then starts
   * the clock: it runs from what was set. */
  CHECK(game_write(8, 30) == 0);
  CHECK(game_write(9, 15) == 0);
  CHECK(game_write(10, 8) == 0);
  CHECK(game_write(11, 1) == 0);
  wall += 500;
  CHECK(game_count() == DAY + 8 * 3600 + 15 * 60 + 30);
  CHECK(game_write(12, 0) == 0);
  wall += 20;
  CHECK(game_count() == DAY + 8 * 3600 + 15 * 60 + 50 && game_flags == 0);
  /* Halting a running clock freezes it where it is. */
  CHECK(game_write(12, 0x40) == 0);
  wall += 1000;
  CHECK(game_count() == DAY + 8 * 3600 + 15 * 60 + 50 && game_flags == 0x40);
  gbcore_shutdown(core);
  /* A halted clock with a stamp from the future, and from before 1997. */
  CHECK(boot_load(make_image(c0, 0x40, T0 + 5000, 48)) == 0);
  CHECK(game_count() == c0 && game_flags == 0x40);
  gbcore_shutdown(core);
  printf("rtc: halted clock holds across time, saves and reloads; set and "
         "restarted by the game\n");
  return 0;
}

static int test_no_reading(void)
{
  long long c0 = 6 * DAY + 42;
  /* The hook fails mid-game: the clock holds, then carries on. */
  CHECK(boot_load(make_image(c0, 0, T0 - 10, 48)) == 0);
  CHECK(game_count() == c0 + 10);
  {
    time_t real = wall;
    wall = 0;
    CHECK(game_count() == c0 + 10 && game_flags == 0);
    CHECK(gbcore_save_ram_read(core, out, sizeof(out)) == 0);
    CHECK(get_stamp(out) == (long long)real && image_count(out) == c0 + 10);
    wall = real + 25;
    CHECK(game_count() == c0 + 35 && game_flags == 0);
  }
  gbcore_shutdown(core);
  printf("rtc: wall clock with no reading: the cartridge clock holds, no "
         "carry\n");
  return 0;
}

static int test_state_load(void)
{
  static unsigned char state[1 << 20];
  long long c0 = 3 * DAY + 600;
  long n;
  CHECK(boot_load(make_image(c0, 0, T0, 48)) == 0);
  CHECK(game_count() == c0);
  n = gbcore_state_save(core, state, sizeof(state));
  CHECK(n > 0);
  /* Later: the player has reset the time, the clock is halted and the day
   * counter has carried.  The old state must change none of it. */
  wall += 2 * 3600;
  CHECK(game_write(11, 200) == 0);
  CHECK(game_write(12, 0xC1) == 0);
  CHECK(game_count() == (256 + 200) * (long long)DAY + 7800);
  CHECK(game_flags == 0xC0);
  CHECK(gbcore_state_load(core, state, (size_t)n) == 0);
  CHECK(game_count() == (256 + 200) * (long long)DAY + 7800);
  CHECK(game_flags == 0xC0);
  CHECK(game_write(12, 0x01) == 0);
  wall += 11;
  CHECK(game_count() == (256 + 200) * (long long)DAY + 7811 && game_flags == 0);
  /* A state made while running, loaded into a running clock much later. */
  n = gbcore_state_save(core, state, sizeof(state));
  CHECK(n > 0);
  wall += 5 * DAY;
  CHECK(gbcore_state_load(core, state, (size_t)n) == 0);
  CHECK(game_count() == (256 + 200 + 5) * (long long)DAY + 7811);
  gbcore_shutdown(core);
  printf("rtc: loading a save state leaves the clock, halt and carry alone\n");
  return 0;
}

/* A save with no clock data.  The CORE starts the clock from zero and sets
 * nothing (test_no_trailer_sizes): that is the load a link session and every
 * other caller gets.  The FRONTEND completes a file of exactly the
 * cartridge's RAM with the trailer built here -- clock at zero, running,
 * day carry set, stamped "written without a clock" (fe_host.c
 * gb_rtc_no_trailer) -- so the game is told its clock cannot be trusted and
 * asks the player to set it.  This is that trailer as the game sees it, and
 * what follows from it. */
static size_t make_no_clock_image(void)
{
  unsigned char *t = image + RAM;
  unsigned i;
  for (i = 0; i < RAM; i++) image[i] = (unsigned char)(i * 11 + 1);
  memset(t, 0, 64);
  t[16] = 0x80;
  t[36] = 0x80;
  memset(t + 40, 0xFF, 8);
  t[43] = 0x7F;
  t[47] = 0x7F;
  return RAM + 48;
}

static int test_no_clock_hint(void)
{
  uint64_t h1, h2;
  /* The game reads: zero, running, carry set. */
  CHECK(boot_load(make_no_clock_image()) == 0);
  h1 = gbcore_sync_hash(core);
  CHECK(game_count() == 0 && game_flags == 0x80);
  wall += 50;
  CHECK(game_count() == 50 && game_flags == 0x80);
  CHECK(ram_kept() == 0);
  /* The image written back is an ordinary one, stamped now, flag and all:
   * until the game acts, a reload shows the same thing (the player who
   * quits before setting the time is asked again). */
  CHECK(get_stamp(out) == (long long)wall && image_count(out) == 50);
  CHECK(get32(out + RAM + 16) == 0x80);
  memcpy(image, out, RAM + 48);
  gbcore_shutdown(core);
  wall += 1000;
  CHECK(boot() == 0);
  CHECK(gbcore_save_ram_write(core, image, RAM + 48) == 0);
  CHECK(game_count() == 1050 && game_flags == 0x80);
  /* The game clears the carry (Pokemon G/S/C do when the player saves
   * after setting the time).  From then on the flag is gone for good. */
  CHECK(game_write(12, 0) == 0);
  CHECK(game_count() == 1050 && game_flags == 0);
  CHECK(gbcore_save_ram_read(core, image, sizeof(image)) == 0);
  CHECK(get32(image + RAM + 16) == 0);
  gbcore_shutdown(core);
  wall += 86400;
  CHECK(boot() == 0);
  CHECK(gbcore_save_ram_write(core, image, RAM + 48) == 0);
  CHECK(game_count() == 1050 + 86400 && game_flags == 0);
  gbcore_shutdown(core);
  /* Two consoles loading the same such image agree (the sync hash covers
   * the carry), and whenever they load it: the stamp adds nothing. */
  CHECK(boot_load(make_no_clock_image()) == 0);
  h1 = gbcore_sync_hash(core);
  CHECK(game_count() == 0 && game_flags == 0x80);
  gbcore_shutdown(core);
  CHECK(boot_load(make_no_clock_image()) == 0);
  h2 = gbcore_sync_hash(core);
  gbcore_shutdown(core);
  CHECK(h1 == h2);
  /* Cart RAM alone, straight into the core: no flag.  The hint is the
   * frontend's, for a file; a session's images never get it. */
  make_no_clock_image();
  CHECK(boot_load(RAM) == 0);
  CHECK(game_count() == 0 && game_flags == 0);
  gbcore_shutdown(core);
  /* No save at all (a new game): no flag. */
  wall = T0;
  CHECK(boot() == 0);
  CHECK(game_count() == 0 && game_flags == 0);
  gbcore_shutdown(core);
  printf("rtc: the frontend's no-clock trailer: game sees carry until it "
         "clears it, then never again; bare RAM and no save set nothing\n");
  return 0;
}

/* A link session loads both cartridges with the stamp equal to the session
 * clock (fe_gblink.c rtc_rebase), on both consoles, and compares
 * gbcore_sync_hash, which covers the clock.  That load must give exactly the
 * registers, whatever else is in the trailer's unused bytes, so consoles
 * running different releases still agree. */
static int session_hash(long long c0, unsigned flags, int mark, uint64_t *h)
{
  make_image(c0, flags, T0, 48);
  if (mark)
  {
    /* The unused upper bytes of a latched register, as the frontend marks
     * the files it writes. */
    image[RAM + 37] = 'U'; image[RAM + 38] = 'T'; image[RAM + 39] = 'C';
  }
  CHECK(boot_load(RAM + 48) == 0);
  *h = gbcore_sync_hash(core);
  CHECK(game_count() == c0 && game_flags == flags);
  gbcore_shutdown(core);
  return 0;
}

static int test_session_load(void)
{
  static const unsigned flags[] = { 0, 0x40, 0x80, 0xC0 };
  long long c0 = 123 * DAY + 4567;
  uint64_t warm, plain, marked;
  unsigned k;
  for (k = 0; k < 4; k++)
  {
    /* This binary has no gbcore_power_on (no instance script), so a boot
     * inherits a little of the last game: give each compared boot the same
     * predecessor. */
    CHECK(session_hash(c0, flags[k], 0, &warm) == 0);
    CHECK(session_hash(c0, flags[k], 0, &plain) == 0);
    CHECK(session_hash(c0, flags[k], 1, &marked) == 0);
    CHECK(plain == marked);
  }
  printf("rtc: a link session's load (stamp == now) gives the registers; "
         "unused trailer bytes do not reach the sync hash\n");
  return 0;
}

/* Before any reading at all: a hook that cannot read at boot.  (The last
 * reading lives for the life of the instance, so this one runs first.) */
static int test_no_reading_at_boot(void)
{
  wall = 0;
  CHECK(boot() == 0);
  CHECK(game_count() == 0 && game_flags == 0);
  CHECK(gbcore_save_ram_read(core, out, sizeof(out)) == 0);
  CHECK(get_stamp(out) == NO_CLOCK && image_count(out) == 0);
  /* An image loaded with no reading keeps its registers ... */
  make_image(8 * DAY + 5, 0, T0 - 100, 48);
  CHECK(gbcore_save_ram_write(core, image, RAM + 48) == 0);
  CHECK(game_count() == 8 * DAY + 5 && game_flags == 0);
  /* ... and the first reading starts the clock from them: no jump of 56
   * years, no carry. */
  wall = T0;
  CHECK(game_count() == 8 * DAY + 5 && game_flags == 0);
  wall += 77;
  CHECK(game_count() == 8 * DAY + 82 && game_flags == 0);
  gbcore_shutdown(core);
  printf("rtc: no reading at boot: image says so, first reading starts the "
         "clock where it stood\n");
  return 0;
}

static int test_import_48(void) { return test_import(48); }
static int test_import_44(void) { return test_import(44); }

/* With a name, runs that one test (to see which of them an older core
 * fails); without, all of them. */
int main(int argc, char **argv)
{
  static const struct { const char *name; int (*run)(void); } tests[] = {
    { "no_reading_at_boot", test_no_reading_at_boot },
    { "fresh_and_running", test_fresh_and_running },
    { "round_trip", test_round_trip },
    { "import_48", test_import_48 },
    { "import_44", test_import_44 },
    { "no_trailer_sizes", test_no_trailer_sizes },
    { "future_stamp", test_future_stamp },
    { "ancient_stamp", test_ancient_stamp },
    { "backwards", test_backwards },
    { "day_wrap", test_day_wrap },
    { "halted", test_halted },
    { "no_reading", test_no_reading },
    { "state_load", test_state_load },
    { "session_load", test_session_load },
    { "no_clock_hint", test_no_clock_hint },
  };
  unsigned i, ran = 0;
  make_rom();
  if (argc > 1 && strcmp(argv[1], "--session-hashes") == 0)
  {
    /* For comparing two builds of the core: the sync hash after the load a
     * link session does, for each flag combination. */
    static const unsigned flags[] = { 0, 0x40, 0x80, 0xC0 };
    uint64_t h;
    for (i = 0; i < 8; i++)
    {
      if (session_hash(123 * DAY + 4567, flags[i / 2], 0, &h))
        return 1;
      if (i & 1)
        printf("flags=%02x hash=%016llx\n", flags[i / 2],
               (unsigned long long)h);
    }
    return 0;
  }
  for (i = 0; i < sizeof(tests) / sizeof(tests[0]); i++)
    if (argc < 2 || strcmp(argv[1], tests[i].name) == 0)
    {
      if (tests[i].run())
        return 1;
      ran++;
    }
  if (!ran)
  {
    fprintf(stderr, "no such test: %s\n", argv[1]);
    return 2;
  }
  return 0;
}
