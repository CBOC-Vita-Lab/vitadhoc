/* cheat_e2e.c -- cheats, end to end: the real cores (the same out-of-tree
 * build as the Vita app and its host twins, ../../corebuild.mk), fe_host,
 * cheats_vita.c, cheatdb.c and a database built by the real tool, running
 * the two cartridges of rom/ with codes switched on and off.
 *
 *   cheat_e2e <work directory> <directory of the ROMs> <fixture.db>
 *
 * Built twice: x86-64 (gpSP's x86 dynarec) and ARMv7 hard-float (the ARM
 * dynarec the Vita uses), the latter run under qemu-arm.  What is checked is
 * what the GAME does -- the values it computes and the picture it draws --
 * not what the cheat engine believes it did.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "fe_host.h"
#include "fe_evt.h"
#include "fe_gbadual.h"
#include "gbalink.h"
#include "cheats_vita.h"
#include "cheat_engine.h"

/* the cores' own globals (instance A; gbab_* is instance B) */
extern int dynarec_enable;
extern uint32_t gamepak_buffer_cap;
extern uint8_t gbab_ewram[];
void gamepak_share_export(gbalink_rom_share *s);
void retro_reset(void);
void retro_cheat_reset(void);
void retro_cheat_set(unsigned index, bool enabled, const char *code);

static int failures, checks;
static char root[400];
static const char *romdir;

#define CHECK(cond, ...)                                                     \
   do {                                                                      \
      checks++;                                                              \
      if (!(cond))                                                           \
      {                                                                      \
         failures++;                                                         \
         printf("   FAIL %s:%d: ", __FILE__, __LINE__);                      \
         printf(__VA_ARGS__);                                                \
         printf("\n");                                                       \
      }                                                                      \
   } while (0)

/* ---- the platform: nothing but memory ---------------------------------------- */

static uint16_t frame[240 * 160];
static unsigned frame_w, frame_h, frames_seen;
static uint32_t pad;                 /* RETRO_DEVICE_ID_JOYPAD_* bits */
static uint64_t now_us;

static void sink_video(const uint16_t *rgb565, unsigned w, unsigned h, size_t pitch)
{
   unsigned y;
   if (!rgb565 || w > 240 || h > 160)
      return;
   for (y = 0; y < h; y++)
      memcpy(&frame[y * w], (const uint8_t *)rgb565 + y * pitch, w * 2);
   frame_w = w;
   frame_h = h;
   frames_seen++;
}
static void sink_audio(const int16_t *s, size_t n) { (void)s; (void)n; }
static uint32_t sink_input(void) { return pad; }
static uint64_t sink_time(void) { return now_us; }

#define PAD_SELECT (1u << 2)
#define PAD_A      (1u << 8)

static void run(int n)
{
   while (n-- > 0)
   {
      now_us += 16743;
      fe_host_run_frame();
   }
}

static uint32_t m(uint32_t address, unsigned width)
{
   uint8_t b[4] = { 0, 0, 0, 0 };
   if (fe_host_mem_read(address, b, width) != 0)
      return 0xBADBAD00u;
   return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}

static int by_name(const char *name)
{
   int i;
   for (i = 0; i < cheats_count(); i++)
      if (!strcmp(cheats_get(i)->name, name))
         return i;
   printf("   (no entry \"%s\")\n", name);
   return -1;
}

static int on(const char *name)  { return cheats_set_enabled(by_name(name), 1); }
static int off(const char *name) { return cheats_set_enabled(by_name(name), 0); }

static void copy_rom(const char *name, const char *as)
{
   char from[600], to[600], buf[4096];
   FILE *a, *b;
   size_t n;
   snprintf(from, sizeof(from), "%s/%s", romdir, name);
   snprintf(to, sizeof(to), "%s/roms/%s", root, as);
   a = fopen(from, "rb");
   b = fopen(to, "wb");
   if (!a || !b)
   {
      printf("cannot copy %s to %s\n", from, to);
      exit(2);
   }
   while ((n = fread(buf, 1, sizeof(buf), a)) > 0)
      fwrite(buf, 1, n, b);
   fclose(a);
   fclose(b);
}

static char rom_path[600], save_path[600], state_path[600], cht_path[600];

static int boot(const char *file, fe_console_t console, int pair)
{
   fe_host_config cfg;
   const char *dot = strrchr(file, '.');
   snprintf(rom_path, sizeof(rom_path), "%s/roms/%s", root, file);
   snprintf(save_path, sizeof(save_path), "%s/roms/%s.sav", root, file);
   snprintf(state_path, sizeof(state_path), "%s/roms/%s.st0", root, file);
   snprintf(cht_path, sizeof(cht_path), "%s/cheats/%.*s.cht", root,
            (int)(dot - file), file);
   remove(save_path);
   remove(cht_path);

   memset(&cfg, 0, sizeof(cfg));
   cfg.console = console;
   cfg.rom_path = rom_path;
   cfg.system_dir = root;
   cfg.save_path = save_path;
   cfg.video_frame = sink_video;
   cfg.audio_frames = sink_audio;
   cfg.input_bitmask = sink_input;
   cfg.time_us = sink_time;
   if (console == FE_CONSOLE_GBA)
   {
      gamepak_buffer_cap = 1;
      if (pair)
      {
         /* the link mirror's bench pair, as main_vita.c's link_bench */
         cfg.gba_dual = 1;
         cfg.gba_dual_mirror = 1;
         cfg.gba_link_nolend = 1;
      }
   }
   pad = 0;
   frames_seen = 0;
   memset(frame, 0, sizeof(frame));
   return fe_host_boot(&cfg);
}

static void leave(void)
{
   cheats_unload();
   fe_host_shutdown();
   fe_host_session_reset();
}

/* ---- the Game Boy Advance ------------------------------------------------------- */

#define LIVES   0x02000000
#define GOLD    0x02000004
#define GOLD2   0x02000006
#define TRIGGER 0x02000008
#define BIG     0x0200000C
#define RES0    0x02000020      /* rom_const(): ROM code            5 */
#define RES1    0x02000024      /* ram_func():  RAM code            7 */
#define RES2    0x02000028      /* rom_lit():   ROM literal pool    0x11223344 */
#define FRAMES  0x0200002C

/* colour of block n of the game's picture (RGB565) */
static unsigned block(int n) { return frame[60 * 240 + n * 48 + 4]; }

static void gba_single(int dynarec)
{
   int rc, n, i;
   char code[64];
   uint32_t a, v;

   printf("GBA, one machine, %s\n", dynarec ? "dynarec" : "interpreter");
   fe_host_option_set("gpsp_drc", dynarec ? "enabled" : "disabled");
   CHECK(boot("cheatrom.gba", FE_CONSOLE_GBA, 0) == 0, "boot");
   CHECK(!dynarec_enable == !dynarec, "the core runs on the %s",
         dynarec_enable ? "dynarec" : "interpreter");
   n = cheats_load(root, rom_path, FE_CONSOLE_GBA);
   CHECK(n == 11 && cheats_db_match() == CHEATS_MATCH_CODE &&
         !strcmp(cheats_db_game(), "Cheat Test (World)"),
         "database: %d entries, match %d, \"%s\"", n, cheats_db_match(), cheats_db_game());

   /* the game alone */
   run(150);
   CHECK(m(FRAMES, 4) > 100, "the game runs: %u frames", (unsigned)m(FRAMES, 4));
   CHECK(m(LIVES, 2) == 0 && m(GOLD, 2) == 0 && m(RES0, 4) == 5 && m(RES1, 4) == 7 &&
         m(RES2, 4) == 0x11223344, "baseline: lives %u gold %u res %u %u %08X",
         (unsigned)m(LIVES, 2), (unsigned)m(GOLD, 2), (unsigned)m(RES0, 4),
         (unsigned)m(RES1, 4), (unsigned)m(RES2, 4));
   CHECK(block(0) == 0x0000 && block(2) == 0xF81F, "baseline picture: %04X %04X",
         block(0), block(2));

   /* a RAM write: what the game shows changes */
   CHECK(on("Infinite Lives") == CHEATS_OK, "enable");
   run(4);
   CHECK(m(LIVES, 2) == 0x63, "lives held: %u", (unsigned)m(LIVES, 2));
   CHECK(block(0) == 0x001F, "the lives block turned blue: %04X", block(0));
   run(60);
   CHECK(m(LIVES, 2) == 0x63 || m(LIVES, 2) == 0x62, "lives still held: %u", (unsigned)m(LIVES, 2));
   /* off: the game counts down again */
   CHECK(off("Infinite Lives") == CHEATS_OK, "disable");
   run(40);
   CHECK(m(LIVES, 2) < 0x62 && m(LIVES, 2) > 0x50, "lives count down again: %u",
         (unsigned)m(LIVES, 2));

   /* a conditional code */
   CHECK(on("Gold When A Is Held") == CHEATS_OK, "enable conditional");
   run(10);
   CHECK(m(GOLD, 2) == 0, "condition false: gold %u", (unsigned)m(GOLD, 2));
   pad = PAD_A;
   run(4);
   CHECK(m(TRIGGER, 2) == 0x1234 && m(GOLD, 2) == 0x0999, "condition true: trigger %04X gold %04X",
         (unsigned)m(TRIGGER, 2), (unsigned)m(GOLD, 2));
   CHECK(block(1) == 0x8400 || block(1) != 0x0000, "the gold block changed: %04X", block(1));
   pad = 0;
   off("Gold When A Is Held");

   /* a button code (CodeBreaker D): Select */
   CHECK(on("Gold 2 With Select") == CHEATS_OK, "enable button code");
   run(4);
   CHECK(m(GOLD2, 2) == 0, "select not held: %u", (unsigned)m(GOLD2, 2));
   pad = PAD_SELECT;
   run(4);
   CHECK(m(GOLD2, 2) == 0x0777, "select held: %04X", (unsigned)m(GOLD2, 2));
   pad = 0;
   off("Gold 2 With Select");

   /* ROM patches: code the dynarec has translated 170 times over */
   CHECK(gba_cheats_rom_pages() == 0, "no page patched yet");
   CHECK(on("Constant Is 9 (ROM)") == CHEATS_OK, "enable ROM patch");
   CHECK(gba_cheats_rom_pages() == 1, "one ROM page copied: %u", gba_cheats_rom_pages());
   run(3);
   CHECK(m(RES0, 4) == 9, "patched ROM code runs: %u", (unsigned)m(RES0, 4));
   CHECK(block(2) == 0x0200 || block(2) != 0xF81F, "its block changed: %04X", block(2));
   CHECK(on("Literal Low Half (ROM)") == CHEATS_OK, "enable literal patch");
   CHECK(gba_cheats_rom_pages() == 1, "the same page: %u", gba_cheats_rom_pages());
   run(3);
   CHECK(m(RES2, 4) == 0x11229999 && m(RES0, 4) == 9, "patched literal pool: %08X",
         (unsigned)m(RES2, 4));
   CHECK(off("Constant Is 9 (ROM)") == CHEATS_OK, "disable ROM patch");
   run(3);
   CHECK(m(RES0, 4) == 5 && m(RES2, 4) == 0x11229999, "one patch off, one on: %u %08X",
         (unsigned)m(RES0, 4), (unsigned)m(RES2, 4));
   off("Literal Low Half (ROM)");
   run(3);
   CHECK(m(RES2, 4) == 0x11223344 && gba_cheats_rom_pages() == 0, "ROM as it was: %08X, %u pages",
         (unsigned)m(RES2, 4), gba_cheats_rom_pages());
   i = cheats_add("typed", "08002008:55667788");
   CHECK(i >= 0 && cheats_get(i)->kind == CHEAT_KIND_GBA_RAW, "raw ROM line: %d", i);
   run(3);
   CHECK(m(RES2, 4) == 0x55667788, "raw ROM patch: %08X", (unsigned)m(RES2, 4));
   CHECK(cheats_remove(i) == CHEATS_OK, "remove it");
   run(3);
   CHECK(m(RES2, 4) == 0x11223344, "removed: %08X", (unsigned)m(RES2, 4));

   /* a RAM write into code the dynarec translated from RAM */
   CHECK(on("Patch The RAM Routine") == CHEATS_OK, "enable RAM code patch");
   run(3);
   CHECK(m(RES1, 4) == 8, "patched RAM code runs: %u", (unsigned)m(RES1, 4));
   off("Patch The RAM Routine");
   run(3);
   CHECK(m(RES1, 4) == 8, "a RAM write is not undone by switching it off: %u", (unsigned)m(RES1, 4));
   i = cheats_add("back", "83004000 2007");
   run(3);
   CHECK(m(RES1, 4) == 7, "written back: %u", (unsigned)m(RES1, 4));
   cheats_remove(i);

   /* encrypted codes, made here with the device's own cipher */
   a = 0x02200004; v = 0x00000ABC;                 /* PARv3: 16 bits to GOLD */
   cheat_gsa_encrypt(&a, &v, cheat_par3_default_seeds);
   snprintf(code, sizeof(code), "%08X %08X", (unsigned)a, (unsigned)v);
   i = cheats_add("ar3", code);
   CHECK(i >= 0 && cheats_get(i)->kind == CHEAT_KIND_GBA_AR3, "encrypted Action Replay v3: %d", i);
   run(3);
   CHECK(m(GOLD, 2) == 0x0ABC, "its effect: %04X", (unsigned)m(GOLD, 2));
   cheats_remove(i);
   a = 0x12000006; v = 0x00000ABD;                 /* GSA: 16 bits to GOLD2 */
   cheat_gsa_encrypt(&a, &v, cheat_gsa1_default_seeds);
   snprintf(code, sizeof(code), "%08X%08X", (unsigned)a, (unsigned)v);
   i = cheats_add("gsa", code);
   CHECK(i >= 0 && cheats_get(i)->kind == CHEAT_KIND_GBA_GAMESHARK, "encrypted GameShark: %d", i);
   run(3);
   CHECK(m(GOLD2, 2) == 0x0ABD, "its effect: %04X", (unsigned)m(GOLD2, 2));
   cheats_remove(i);
   CHECK(on("Big Is 12345678") == CHEATS_OK, "raw 32-bit");
   run(3);
   CHECK(m(BIG, 4) == 0x12345678, "raw 32-bit write: %08X", (unsigned)m(BIG, 4));
   off("Big Is 12345678");

   /* the cheat device's button */
   on("Press The Button For Gold");
   run(3);
   CHECK(m(GOLD, 1) != 0xAB, "button not held");
   cheats_button(1);
   run(3);
   CHECK(m(GOLD, 1) == 0xAB, "button held: %02X", (unsigned)m(GOLD, 1));
   cheats_button(0);
   off("Press The Button For Gold");

   /* suspend (a network session): everything leaves the core */
   on("Infinite Lives");
   on("Constant Is 9 (ROM)");
   run(3);
   CHECK(m(LIVES, 2) == 0x63 && m(RES0, 4) == 9, "before suspend");
   cheats_suspend(1);
   CHECK(gba_cheats_rom_pages() == 0, "suspend lifts the ROM patch");
   run(40);
   CHECK(m(LIVES, 2) < 0x62 && m(RES0, 4) == 5, "suspended: lives %u res %u",
         (unsigned)m(LIVES, 2), (unsigned)m(RES0, 4));
   CHECK(on("Gold 2 With Select") == CHEATS_E_SUSPENDED, "nothing switches on while suspended");
   CHECK(cheats_enabled_count() == 2, "the list keeps its switches: %d", cheats_enabled_count());
   cheats_suspend(0);
   run(3);
   CHECK(m(LIVES, 2) == 0x63 && m(RES0, 4) == 9, "resumed: lives %u res %u",
         (unsigned)m(LIVES, 2), (unsigned)m(RES0, 4));

   /* a state load, and a reset */
   CHECK(fe_host_state_save(state_path) == 0, "state save");
   cheats_disable_all();
   run(40);
   CHECK(m(LIVES, 2) < 0x62 && m(RES0, 4) == 5, "all off: lives %u res %u",
         (unsigned)m(LIVES, 2), (unsigned)m(RES0, 4));
   on("Infinite Lives");
   on("Constant Is 9 (ROM)");
   CHECK(fe_host_state_load(state_path) == 0, "state load");
   cheats_apply();
   run(3);
   CHECK(m(LIVES, 2) == 0x63 && m(RES0, 4) == 9, "after a state load: lives %u res %u",
         (unsigned)m(LIVES, 2), (unsigned)m(RES0, 4));
   retro_reset();
   cheats_apply();
   run(90);
   CHECK(m(FRAMES, 4) < 95 && m(FRAMES, 4) > 20, "the game restarted: frame %u", (unsigned)m(FRAMES, 4));
   CHECK(m(LIVES, 2) == 0x63 && m(RES0, 4) == 9, "after a reset: lives %u res %u",
         (unsigned)m(LIVES, 2), (unsigned)m(RES0, 4));

   /* instance B is about to borrow the ROM map: it must see the cartridge */
   {
      gbalink_rom_share share;
      const uint8_t *page;
      gamepak_share_export(&share);
      page = share.map_read[0x08000000 >> 15];
      CHECK(gba_cheats_rom_pages() == 0 && page[0x2000] == 0x05,
            "the exported map is the cartridge's: %u pages, byte %02X",
            gba_cheats_rom_pages(), page[0x2000]);
      run(2);
      CHECK(gba_cheats_rom_pages() == 1 && m(RES0, 4) == 9,
            "the patch is back a frame later: %u pages, res %u", gba_cheats_rom_pages(),
            (unsigned)m(RES0, 4));
   }

   /* the file, and the next boot */
   CHECK(cheats_flush() == CHEATS_OK, "flush");
   cheats_unload();
   run(40);
   CHECK(m(LIVES, 2) < 0x62 && m(RES0, 4) == 5, "unloaded: lives %u res %u",
         (unsigned)m(LIVES, 2), (unsigned)m(RES0, 4));
   n = cheats_load(root, rom_path, FE_CONSOLE_GBA);
   /* his file has the eight database entries he switched at some point,
    * two of them on; the three he never touched are the database's */
   {
      int own = 0;
      for (i = 0; i < n; i++)
         own += !cheats_get(i)->builtin;
      CHECK(n == 11 && own == 8 && cheats_enabled_count() == 2 &&
            cheats_get(by_name("Infinite Lives"))->enabled &&
            !cheats_get(by_name("Infinite Lives"))->builtin &&
            cheats_get(by_name("Constant Is 9 (ROM)"))->enabled &&
            cheats_get(by_name("Enable Code (Must Be On)"))->builtin,
            "loaded again: %d entries, %d his, %d on", n, own, cheats_enabled_count());
   }
   run(3);
   CHECK(m(LIVES, 2) == 0x63 && m(RES0, 4) == 9, "his cheats are on again: lives %u res %u",
         (unsigned)m(LIVES, 2), (unsigned)m(RES0, 4));
   rc = remove(cht_path);
   CHECK(rc == 0, "his .cht existed");

   /* the libretro entry points other frontends use reach the same engine */
   cheats_unload();
   retro_cheat_set(3, true, "D8BAE4D9 4864DCE5+A86CDBA5 19BA49B3");   /* a master code */
   retro_cheat_set(0, true, "82000000+0042");
   retro_cheat_set(1, true, "60001000 00002009");
   run(3);
   CHECK(m(LIVES, 2) == 0x42 && m(RES0, 4) == 9, "retro_cheat_set: lives %u res %u",
         (unsigned)m(LIVES, 2), (unsigned)m(RES0, 4));
   retro_cheat_set(1, false, "");
   run(3);
   CHECK(m(LIVES, 2) == 0x42 && m(RES0, 4) == 5, "retro_cheat_set off: lives %u res %u",
         (unsigned)m(LIVES, 2), (unsigned)m(RES0, 4));
   retro_cheat_reset();
   run(20);
   CHECK(m(LIVES, 2) < 0x42, "retro_cheat_reset: lives %u", (unsigned)m(LIVES, 2));
   leave();
}

/* The link mirror's two machines: codes are the player's machine's only. */
static void gba_pair(void)
{
   uint32_t b_lives, b_frames;

   printf("GBA, the link pair\n");
   CHECK(boot("cheatrom.gba", FE_CONSOLE_GBA, 1) == 0, "boot");
   cheats_load(root, rom_path, FE_CONSOLE_GBA);
   run(30);
   CHECK(fe_host_state_save(state_path) == 0 && fe_host_state_load(state_path) == 0,
         "a state load starts the partner's machine");
   run(60);
   CHECK(fe_gbadual_active(), "the pair runs");
   memcpy(&b_frames, &gbab_ewram[0x2C], 4);
   CHECK(b_frames > 60, "the partner's game runs: frame %u", (unsigned)b_frames);

   CHECK(on("Infinite Lives") == CHEATS_OK && on("Constant Is 9 (ROM)") == CHEATS_OK &&
         on("Patch The RAM Routine") == CHEATS_OK, "enable three codes");
   run(60);
   CHECK(fe_gbadual_active(), "the pair still runs");
   CHECK(m(LIVES, 2) == 0x63 && m(RES0, 4) == 9 && m(RES1, 4) == 8,
         "the player's machine: lives %u res %u %u", (unsigned)m(LIVES, 2),
         (unsigned)m(RES0, 4), (unsigned)m(RES1, 4));
   b_lives = gbab_ewram[0] | gbab_ewram[1] << 8;
   CHECK(b_lives == 0 && gbab_ewram[0x20] == 5 && gbab_ewram[0x24] == 7,
         "the partner's machine is untouched: lives %u res %u %u", (unsigned)b_lives,
         gbab_ewram[0x20], gbab_ewram[0x24]);
   memcpy(&b_frames, &gbab_ewram[0x2C], 4);
   CHECK(b_frames > 120 && m(FRAMES, 4) == b_frames, "both at frame %u / %u",
         (unsigned)m(FRAMES, 4), (unsigned)b_frames);
   leave();
}

/* ---- the Game Boy ------------------------------------------------------------------ */

static void gb(fe_console_t console, const char *file)
{
   int n, i, color = console == FE_CONSOLE_GBC;
   unsigned white;
   uint8_t x;

   printf("%s\n", color ? "Game Boy Color" : "Game Boy");
   CHECK(boot(file, console, 0) == 0, "boot");
   n = cheats_load(root, rom_path, console);
   CHECK(n == 6 && cheats_db_match() == CHEATS_MATCH_CRC, "database: %d entries, match %d",
         n, cheats_db_match());
   run(150);
   CHECK(m(0xC004, 1) != 0xBADBAD00u && frames_seen > 100, "the game runs");
   CHECK(m(0xC000, 1) == 0 && m(0xC001, 1) == 0x11 && m(0xC002, 1) == 0x22 &&
         m(0xC003, 1) == 0x44 && m(0xC006, 1) == 0x77,
         "baseline: %02X %02X %02X %02X %02X", (unsigned)m(0xC000, 1), (unsigned)m(0xC001, 1),
         (unsigned)m(0xC002, 1), (unsigned)m(0xC003, 1), (unsigned)m(0xC006, 1));
   CHECK((m(0xC007, 1) == 0x11) == color, "the cartridge sees a %s", color ? "Game Boy Color" : "Game Boy");
   white = frame[0];

   /* GameShark: a RAM write every frame */
   CHECK(on("Infinite Lives") == CHEATS_OK, "enable GameShark");
   run(4);
   CHECK(m(0xC000, 1) >= 0xFE, "lives held: %02X", (unsigned)m(0xC000, 1));
   if (!color)
      CHECK(frame[0] != white, "the picture changed: %04X -> %04X", white, frame[0]);
   CHECK(on("High RAM Is 42") == CHEATS_OK, "enable high RAM code");
   run(3);
   CHECK(m(0xC006, 1) == 0x42, "high RAM: %02X", (unsigned)m(0xC006, 1));
   off("High RAM Is 42");
   off("Infinite Lives");
   run(40);
   CHECK(m(0xC000, 1) < 0xFD && m(0xC000, 1) > 0xF0, "lives count down again: %02X",
         (unsigned)m(0xC000, 1));

   /* Game Genie: what the game reads from its ROM */
   CHECK(on("Operand Is 99") == CHEATS_OK, "enable Game Genie");
   run(3);
   CHECK(m(0xC001, 1) == 0x99, "patched operand: %02X", (unsigned)m(0xC001, 1));
   CHECK(on("Bank 2 Byte Is 99") == CHEATS_OK, "enable banked Game Genie");
   run(3);
   CHECK(m(0xC002, 1) == 0x99 && m(0xC003, 1) == 0x44,
         "only the bank whose byte matches: %02X %02X", (unsigned)m(0xC002, 1), (unsigned)m(0xC003, 1));
   off("Operand Is 99");
   run(3);
   CHECK(m(0xC001, 1) == 0x11 && m(0xC002, 1) == 0x99, "one off, one on: %02X %02X",
         (unsigned)m(0xC001, 1), (unsigned)m(0xC002, 1));
   off("Bank 2 Byte Is 99");
   i = cheats_add("every bank", "990-00B");
   CHECK(i >= 0 && cheats_get(i)->kind == CHEAT_KIND_GB_GAMEGENIE, "six-digit Game Genie: %d", i);
   run(3);
   CHECK(m(0xC002, 1) == 0x99 && m(0xC003, 1) == 0x99, "every bank: %02X %02X",
         (unsigned)m(0xC002, 1), (unsigned)m(0xC003, 1));
   cheats_remove(i);
   run(3);
   CHECK(m(0xC002, 1) == 0x22 && m(0xC003, 1) == 0x44, "the ROM as it was: %02X %02X",
         (unsigned)m(0xC002, 1), (unsigned)m(0xC003, 1));

   if (color)
   {
      /* a banked work RAM code: bank 1, while the game sits in bank 2 */
      CHECK(on("Work RAM Bank 1 Is AA") == CHEATS_OK, "enable 91 code");
      run(3);
      CHECK(m(0xC005, 1) == 0xAA && m(0xC008, 1) == 0x5A, "bank 1 written, bank 2 not: %02X %02X",
            (unsigned)m(0xC005, 1), (unsigned)m(0xC008, 1));
      off("Work RAM Bank 1 Is AA");
      /* an 01 code writes whichever bank is mapped when the frame begins
       * (which is why 9n codes exist): one of the two, not both */
      i = cheats_add("mapped", "01BB00D0");
      run(3);
      CHECK((m(0xC005, 1) == 0xBB) != (m(0xC008, 1) == 0xBB),
            "an 01 code writes the mapped bank only: %02X %02X",
            (unsigned)m(0xC005, 1), (unsigned)m(0xC008, 1));
      cheats_remove(i);
   }

   /* suspend, state load, the file */
   on("Infinite Lives");
   on("Operand Is 99");
   run(3);
   cheats_suspend(1);
   run(40);
   CHECK(m(0xC000, 1) < 0xFD && m(0xC001, 1) == 0x11, "suspended: %02X %02X",
         (unsigned)m(0xC000, 1), (unsigned)m(0xC001, 1));
   cheats_suspend(0);
   run(3);
   CHECK(m(0xC000, 1) >= 0xFE && m(0xC001, 1) == 0x99, "resumed: %02X %02X",
         (unsigned)m(0xC000, 1), (unsigned)m(0xC001, 1));
   CHECK(fe_host_state_save(state_path) == 0, "state save");
   run(5);
   CHECK(fe_host_state_load(state_path) == 0, "state load");
   cheats_apply();
   run(3);
   CHECK(m(0xC000, 1) >= 0xFE && m(0xC001, 1) == 0x99, "after a state load: %02X %02X",
         (unsigned)m(0xC000, 1), (unsigned)m(0xC001, 1));
   CHECK(cheats_flush() == CHEATS_OK, "flush");
   cheats_unload();
   run(40);
   CHECK(m(0xC000, 1) < 0xFD && m(0xC001, 1) == 0x11, "unloaded: %02X %02X",
         (unsigned)m(0xC000, 1), (unsigned)m(0xC001, 1));
   n = cheats_load(root, rom_path, console);
   run(3);
   CHECK(n == 6 && cheats_enabled_count() == 2 && m(0xC000, 1) >= 0xFE && m(0xC001, 1) == 0x99,
         "loaded again: %d entries, %d on, %02X %02X", n, cheats_enabled_count(),
         (unsigned)m(0xC000, 1), (unsigned)m(0xC001, 1));
   remove(cht_path);
   (void)x;
   leave();
}

int main(int argc, char **argv)
{
   char path[600];

   if (argc < 4)
   {
      printf("usage: cheat_e2e workdir romdir fixture.db\n");
      return 2;
   }
   snprintf(root, sizeof(root), "%s", argv[1]);
   romdir = argv[2];
   mkdir(root, 0777);
   snprintf(path, sizeof(path), "%s/roms", root);
   mkdir(path, 0777);
   snprintf(path, sizeof(path), "%s/cheats", root);
   mkdir(path, 0777);
   snprintf(path, sizeof(path), "%s/cheats/index.cache", root);
   remove(path);
   snprintf(path, sizeof(path), "%s/e2e.log", root);
   fe_evt_init(path, 0);
   copy_rom("cheatrom.gba", "cheatrom.gba");
   copy_rom("gbcheat.gb", "gbcheat.gb");
   copy_rom("gbcheat.gb", "gbcheat.gbc");

   CHECK(cheats_db_open(argv[3]) == 2, "the fixture database");

   gba_single(1);
   gba_single(0);
   fe_host_option_set("gpsp_drc", "enabled");
   gba_pair();
   gb(FE_CONSOLE_GB, "gbcheat.gb");
   gb(FE_CONSOLE_GBC, "gbcheat.gbc");

   printf("cheat_e2e: %d checks, %d failed\n", checks, failures);
   fe_evt_close();
   return failures ? 1 : 0;
}
