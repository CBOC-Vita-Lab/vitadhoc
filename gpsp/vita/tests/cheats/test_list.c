/* test_list.c -- vita/cheats_vita.c and vita/cheatdb.c without an emulator
 * (cheats_vita.c is built -DCHEATS_VITA_NO_CORE: what would go to the core
 * is counted instead).
 *
 *   test_list <cheats.db> <fixture.db> <work directory>
 *
 *   1. the .cht file: a hostile one read, changed, written, read back
 *   2. the list's rules: add / remove / rename / enable, limits, suspend
 *   3. the fixture database (built by tools/make_cheatdb.py from fixture/):
 *      lookups by CRC-32, game code, title and file name, the merge with the
 *      player's file, the CRC cache
 *   4. the bundled database: every block read and checksummed, every name
 *      found again through the C side's reading of names, and EVERY CODE
 *      run through cheats_validate -- the numbers the build report quotes
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "cheats_vita.h"
#include "cheatdb.h"

extern unsigned cheats_test_pushed_ops, cheats_test_pushed_patches,
                cheats_test_pushed_gb, cheats_test_pushes;

static int failures, checks;
static char work[400];

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

static void put_file(const char *path, const char *text, size_t n)
{
   FILE *f = fopen(path, "wb");
   if (!f)
   {
      printf("cannot write %s\n", path);
      exit(2);
   }
   fwrite(text, 1, n, f);
   fclose(f);
}

static char *get_file(const char *path)
{
   static char buf[1 << 16];
   FILE *f = fopen(path, "rb");
   size_t n;
   buf[0] = 0;
   if (!f)
      return NULL;
   n = fread(buf, 1, sizeof(buf) - 1, f);
   buf[n] = 0;
   fclose(f);
   return buf;
}

static int find(const char *name)
{
   int i;
   for (i = 0; i < cheats_count(); i++)
      if (!strcmp(cheats_get(i)->name, name))
         return i;
   return -1;
}

/* ---- 1. the .cht file ---------------------------------------------------------- */

/* A byte order mark, CRLF, a wrong count, numbers out of order and with a
 * gap, values without quotes, keys with odd spacing, a comment, RetroArch's
 * own per-cheat keys, a key that is nobody's, a code over two lines, a
 * quote inside a name, an entry without a name, one without a code. */
static const char hostile[] =
   "\xEF\xBB\xBF" "cheats = 3\r\n"
   "\r\n"
   "# a comment\r\n"
   "cheat0_desc = \"Infinite money\"\r\n"
   "cheat0_code = \"82025838 E0FF+82025840 E0FF\"\r\n"
   "cheat0_enable = true\r\n"
   "cheat0_handler = \"0\"\r\n"
   "cheat0_memory_search_size = \"3\"\r\n"
   "\r\n"
   "cheat7_desc=No quotes at all\r\n"
   "cheat7_code   =   33000010 00ab\r\n"
   "cheat7_enable = TRUE\r\n"
   "cheat2_code = \"82000100 0001\r\n"
   "82000102 0002\"\r\n"
   "cheat2_desc = \"Two \"quoted\" lines\"\r\n"
   "cheat2_enable = false\r\n"
   "some_frontend_key = \"kept\"\r\n"
   "cheat3_code = \"3300075C 00??\"\r\n"
   "cheat3_enable = true\r\n"
   "cheat4_desc = \"No code\"\r\n"
   "cheat5_desc = \"Z\xC3\xBCrich \xE2\x82\xAC\"\r\n"
   "cheat5_code = \"12000100 0000ABCD\"\r\n";

static const char golden[] =
   "cheats = 8\n"
   "\n"
   "cheat0_desc = \"Infinite money\"\n"
   "cheat0_code = \"82025838 E0FF+82025840 E0FF\"\n"
   "cheat0_enable = false\n"
   "cheat0_handler = \"0\"\n"
   "cheat0_memory_search_size = \"3\"\n"
   "\n"
   "cheat1_desc = \"Renamed\"\n"
   "cheat1_code = \"82000100 0001+82000102 0002\"\n"
   "cheat1_enable = true\n"
   "\n"
   "cheat2_desc = \"Cheat 3\"\n"
   "cheat2_code = \"3300075C 00??\"\n"
   "cheat2_enable = true\n"
   "\n"
   "cheat3_desc = \"No code\"\n"
   "cheat3_code = \"\"\n"
   "cheat3_enable = false\n"
   "\n"
   "cheat4_desc = \"Z\xC3\xBCrich \xE2\x82\xAC\"\n"
   "cheat4_code = \"12000100 0000ABCD\"\n"
   "cheat4_enable = false\n"
   "\n"
   "cheat5_desc = \"No quotes at all\"\n"
   "cheat5_code = \"33000010 00ab\"\n"
   "cheat5_enable = true\n"
   "\n"
   "cheat6_desc = \"Cheat 7\"\n"
   "cheat6_code = \"02000200:7f\"\n"
   "cheat6_enable = true\n"
   "\n"
   "cheat7_desc = \"Typed 'name'\"\n"
   "cheat7_code = \"82000300 0005+82000302 0006\"\n"
   "cheat7_enable = true\n"
   "\n"
   "some_frontend_key = \"kept\"\n";

static void test_cht(void)
{
   char path[600], rom[600];
   const cheat_entry *e;
   int i, rc;
   char *text;

   printf(".cht file\n");
   snprintf(path, sizeof(path), "%s/cheats", work);
   mkdir(work, 0777);
   mkdir(path, 0777);
   snprintf(path, sizeof(path), "%s/cheats/Round Trip (USA).cht", work);
   put_file(path, hostile, sizeof(hostile) - 1);
   snprintf(rom, sizeof(rom), "%s/roms/Round Trip (USA).gba", work);

   CHECK(cheats_load(work, rom, FE_CONSOLE_GBA) == 6, "entries: %d", cheats_count());
   e = cheats_get(0);
   CHECK(e && !strcmp(e->name, "Infinite money") &&
         !strcmp(e->code, "82025838 E0FF+82025840 E0FF") && e->enabled &&
         e->kind == CHEAT_KIND_GBA_CODEBREAKER && e->usable && !e->builtin, "entry 0");
   e = cheats_get(1);
   CHECK(e && !strcmp(e->name, "Two 'quoted' lines") &&
         !strcmp(e->code, "82000100 0001+82000102 0002") && !e->enabled && e->usable,
         "entry 1: \"%s\" \"%s\"", e ? e->name : "", e ? e->code : "");
   e = cheats_get(2);
   CHECK(e && !strcmp(e->name, "Cheat 3") && e->enabled && !e->usable,
         "entry 2: \"%s\" usable %d", e ? e->name : "", e ? e->usable : -1);
   CHECK(!strcmp(cheats_note(2), cheats_error_text(CHEATS_E_NEEDS_VALUE)), "note 2");
   e = cheats_get(3);
   CHECK(e && !strcmp(e->name, "No code") && !e->code[0] && !e->usable, "entry 3");
   e = cheats_get(4);
   CHECK(e && !strcmp(e->name, "Z\xC3\xBCrich \xE2\x82\xAC") &&
         e->kind == CHEAT_KIND_GBA_GAMESHARK && e->usable, "entry 4");
   e = cheats_get(5);
   CHECK(e && !strcmp(e->name, "No quotes at all") &&
         !strcmp(e->code, "33000010 00ab") && e->enabled && e->usable, "entry 5");
   CHECK(cheats_get(6) == NULL && cheats_get(-1) == NULL, "out of range");
   CHECK(cheats_enabled_count() == 3, "enabled: %d", cheats_enabled_count());
   /* what went to the core: entries 0 and 5 (entry 2 is on but cannot run) */
   CHECK(cheats_test_pushed_ops == 3, "pushed ops: %u", cheats_test_pushed_ops);
   CHECK(cheats_db_game()[0] == 0 && cheats_db_match() == CHEATS_MATCH_NONE, "no database");

   /* nothing changed: nothing written */
   remove(path);
   CHECK(cheats_flush() == CHEATS_OK && get_file(path) == NULL, "flush of an unchanged list wrote");
   put_file(path, hostile, sizeof(hostile) - 1);

   /* change it */
   CHECK(cheats_set_enabled(0, 0) == CHEATS_OK, "disable 0");
   CHECK(cheats_set_enabled(1, 1) == CHEATS_OK, "enable 1");
   CHECK(cheats_set_enabled(2, 1) == CHEATS_E_NEEDS_VALUE, "enable a code with a hole");
   CHECK(cheats_set_enabled(3, 1) == CHEATS_E_FORMAT, "enable an entry without a code");
   CHECK(cheats_set_enabled(99, 1) == CHEATS_E_RANGE, "enable out of range");
   CHECK(cheats_rename(1, "Renamed") == CHEATS_OK && cheats_rename(1, "") == CHEATS_E_FORMAT,
         "rename");
   i = cheats_add(NULL, "02000200:7f");
   CHECK(i == 6 && !strcmp(cheats_get(i)->name, "Cheat 7") && cheats_get(i)->enabled &&
         cheats_get(i)->kind == CHEAT_KIND_GBA_RAW, "add without a name: %d", i);
   i = cheats_add("Typed \"name\"", "82000300 0005\n82000302 0006\n");
   CHECK(i == 7 && !strcmp(cheats_get(i)->code, "82000300 0005+82000302 0006"),
         "add with newlines: %d \"%s\"", i, i >= 0 ? cheats_get(i)->code : "");
   /* the same code again is that entry, not a second one */
   CHECK(cheats_add("Again", "82000300 0005 + 82000302 0006") == 7 && cheats_count() == 8,
         "duplicate add");
   CHECK(cheats_add("Bad", "hello") == CHEATS_E_FORMAT, "add text");
   CHECK(cheats_add("Bad", "3300075C 00XX") == CHEATS_E_NEEDS_VALUE, "add a hole");
   CHECK(cheats_add("Bad", "60A2DCEA C78E") == CHEATS_E_ENCRYPTED, "add encrypted");
   CHECK(cheats_add("Bad", "00000000 08000000") == CHEATS_E_UNSUPPORTED, "add slow motion");
   CHECK(cheats_validate("82000300 0005", FE_CONSOLE_GBA) == CHEAT_KIND_GBA_CODEBREAKER &&
         cheats_validate("01FF16D0", FE_CONSOLE_GB) == CHEAT_KIND_GB_GAMESHARK &&
         cheats_validate("FAA-12D", FE_CONSOLE_GBC) == CHEAT_KIND_GB_GAMEGENIE &&
         cheats_validate("01FF16D0", FE_CONSOLE_GBA) == CHEATS_E_FORMAT &&
         cheats_validate("d8bae4d9 4864dce5\na86cdba5 19ba49b3", FE_CONSOLE_GBA) == CHEAT_KIND_GBA_AR3,
         "cheats_validate");
   {
      static char huge[CHEATS_CODE_CAP + 64];
      memset(huge, '8', sizeof(huge) - 1);
      CHECK(cheats_validate(huge, FE_CONSOLE_GBA) == CHEATS_E_TOO_LONG, "too long");
   }
   CHECK(cheats_flush() == CHEATS_OK, "flush");
   text = get_file(path);
   CHECK(text && !strcmp(text, golden), "written file differs:\n%s", text ? text : "(none)");

   /* and it reads back as the same list */
   cheats_unload();
   CHECK(cheats_count() == 0 && cheats_get(0) == NULL, "unload");
   CHECK(cheats_load(work, rom, FE_CONSOLE_GBA) == 8, "reload: %d", cheats_count());
   CHECK(!strcmp(cheats_get(1)->name, "Renamed") && cheats_get(1)->enabled &&
         !cheats_get(0)->enabled && cheats_get(7)->enabled, "reload: states");
   /* writing it again changes nothing */
   CHECK(cheats_set_enabled(0, 1) == CHEATS_OK && cheats_set_enabled(0, 0) == CHEATS_OK &&
         cheats_flush() == CHEATS_OK, "touch and flush");
   text = get_file(path);
   CHECK(text && !strcmp(text, golden), "second write differs");

   /* remove everything: the file stays the player's, with nothing in it */
   while (cheats_count())
      CHECK(cheats_remove(0) == CHEATS_OK, "remove");
   CHECK(cheats_remove(0) == CHEATS_E_RANGE, "remove from an empty list");
   CHECK(cheats_flush() == CHEATS_OK, "flush empty");
   text = get_file(path);
   CHECK(text && !strncmp(text, "cheats = 0\n", 11) && strstr(text, "some_frontend_key"),
         "empty file: %s", text ? text : "(none)");
   cheats_unload();

   /* no file: an empty list, and no file made for it */
   snprintf(rom, sizeof(rom), "%s/roms/Never Seen.gb", work);
   snprintf(path, sizeof(path), "%s/cheats/Never Seen.cht", work);
   remove(path);
   CHECK(cheats_load(work, rom, FE_CONSOLE_GB) == 0, "no file");
   CHECK(cheats_flush() == CHEATS_OK && get_file(path) == NULL, "no file made");
   i = cheats_add("Lives", "01ff16d0");
   CHECK(i == 0 && cheats_get(0)->kind == CHEAT_KIND_GB_GAMESHARK && cheats_test_pushed_gb == 1,
         "GB add: %d", i);
   i = cheats_add("Genie", "993-00F-A2E+01AA17D0");
   CHECK(i == 1 && cheats_get(1)->kind == CHEAT_KIND_GB_GAMEGENIE && cheats_test_pushed_gb == 3,
         "GG add: %d, pushed %u", i, cheats_test_pushed_gb);
   CHECK(cheats_add("x", "82000300 0005") == CHEATS_E_FORMAT, "a GBA code on a Game Boy");
   CHECK(cheats_add("x", "$0D24206E") == CHEATS_E_UNSUPPORTED, "Xploder");
   CHECK(cheats_flush() == CHEATS_OK && get_file(path) != NULL, "file made");
   cheats_unload();
   remove(path);

   /* garbage is not a .cht: an empty list, no crash */
   snprintf(path, sizeof(path), "%s/cheats/Garbage.cht", work);
   {
      static char junk[5000];
      unsigned k, x = 12345;
      for (k = 0; k < sizeof(junk); k++)
      {
         x = x * 1103515245u + 12345u;
         junk[k] = (char)(x >> 16);
      }
      put_file(path, junk, sizeof(junk));
      snprintf(rom, sizeof(rom), "%s/roms/Garbage.gba", work);
      cheats_load(work, rom, FE_CONSOLE_GBA);
      CHECK(cheats_count() <= CHEATS_MAX, "garbage: %d entries", cheats_count());
      cheats_unload();
      put_file(path, "cheat0_code = \"unterminated", 26);
      CHECK(cheats_load(work, rom, FE_CONSOLE_GBA) == 1 && !cheats_get(0)->usable, "unterminated quote");
      cheats_unload();
      put_file(path, "", 0);
      CHECK(cheats_load(work, rom, FE_CONSOLE_GBA) == 0, "empty file");
      cheats_unload();
   }
   remove(path);
   (void)rc;
}

/* ---- 2. limits, suspend -------------------------------------------------------- */

static void test_rules(void)
{
   char rom[600], code[32];
   int i, rc = 0;

   printf("the list's rules\n");
   snprintf(rom, sizeof(rom), "%s/roms/Rules.gba", work);
   cheats_load(work, rom, FE_CONSOLE_GBA);

   /* full */
   for (i = 0; i < CHEATS_MAX + 5 && rc >= 0; i++)
   {
      snprintf(code, sizeof(code), "8200%04X 0001", i * 2);
      rc = cheats_add(NULL, code);
   }
   CHECK(cheats_count() == CHEATS_MAX && rc == CHEATS_E_FULL, "full: %d entries, rc %d",
         cheats_count(), rc);
   /* everything is on: 1024 operations in the core */
   CHECK(cheats_enabled_count() == CHEATS_MAX && cheats_test_pushed_ops == CHEATS_MAX,
         "all on: %d, pushed %u", cheats_enabled_count(), cheats_test_pushed_ops);

   cheats_suspend(1);
   CHECK(cheats_suspended() && cheats_test_pushed_ops == 0, "suspend: pushed %u",
         cheats_test_pushed_ops);
   CHECK(cheats_enabled_count() == CHEATS_MAX, "suspend keeps the list's switches");
   CHECK(cheats_set_enabled(3, 0) == CHEATS_OK, "switching off while suspended");
   CHECK(cheats_set_enabled(3, 1) == CHEATS_E_SUSPENDED, "switching on while suspended");
   cheats_apply();
   CHECK(cheats_test_pushed_ops == 0, "apply while suspended pushed %u", cheats_test_pushed_ops);
   cheats_suspend(0);
   CHECK(!cheats_suspended() && cheats_test_pushed_ops == CHEATS_MAX - 1, "resume: pushed %u",
         cheats_test_pushed_ops);
   cheats_disable_all();
   CHECK(cheats_enabled_count() == 0 && cheats_test_pushed_ops == 0, "disable all");
   cheats_unload();                 /* not flushed: nothing of it on disk */
   {
      char path[600];
      snprintf(path, sizeof(path), "%s/cheats/Rules.cht", work);
      CHECK(get_file(path) == NULL, "unload wrote a file");
   }
}

/* ---- 3. the fixture database ------------------------------------------------- */

static void copy_file(const char *from, const char *to)
{
   FILE *a = fopen(from, "rb"), *b = fopen(to, "wb");
   char buf[4096];
   size_t n;
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

static void test_fixture(const char *db)
{
   char rom[600], path[600], cache[600];
   const cheat_entry *e;
   int i, n, lives;
   char *text;

   printf("the fixture database\n");
   CHECK(cheats_db_open("/nonexistent/cheats.db") == 0, "a missing database");
   snprintf(path, sizeof(path), "%s/cheats/Garbage.cht", work);
   put_file(path, "GBADCHT1 but not really", 23);
   CHECK(cheats_db_open(path) == 0, "a file that is no database");
   remove(path);
   CHECK(cheats_db_open(db) == 2, "open: %d games", cheatdb_games());

   snprintf(path, sizeof(path), "%s/roms", work);
   mkdir(path, 0777);
   snprintf(cache, sizeof(cache), "%s/cheats/index.cache", work);
   remove(cache);

   /* GBA: by the game code in the header; no hashing, so no cache yet */
   snprintf(rom, sizeof(rom), "%s/roms/renamed.gba", work);
   copy_file("rom/cheatrom.gba", rom);
   snprintf(path, sizeof(path), "%s/cheats/renamed.cht", work);
   remove(path);
   n = cheats_load(work, rom, FE_CONSOLE_GBA);
   CHECK(n == 11, "GBA fixture: %d entries", n);
   CHECK(!strcmp(cheats_db_game(), "Cheat Test (World)") &&
         cheats_db_match() == CHEATS_MATCH_CODE, "found as \"%s\", match %d",
         cheats_db_game(), cheats_db_match());
   CHECK(get_file(cache) == NULL, "a unique game code must not hash the ROM");
   for (i = 0; i < n; i++)
   {
      e = cheats_get(i);
      CHECK(e->builtin && !e->enabled, "entry %d builtin %d enabled %d", i, e->builtin, e->enabled);
   }
   CHECK(cheats_enabled_count() == 0 && cheats_test_pushed_ops == 0, "everything off");
   /* the merge of the two files: the plain one first, duplicates once,
    * equal names told apart */
   CHECK(!strcmp(cheats_get(0)->name, "Enable Code (Must Be On)") &&
         !strcmp(cheats_get(1)->name, "Infinite Lives") &&
         find("Infinite Lives (2)") >= 0 && find("Nothing Here") < 0 &&
         find("Not A Code") < 0 && find("Constant Is 9 (ROM)") >= 0, "merge");
   i = find("Lives Modifier");
   CHECK(i >= 0 && !cheats_get(i)->usable &&
         cheats_set_enabled(i, 1) == CHEATS_E_NEEDS_VALUE, "placeholder entry");
   i = find("Enable Code (Must Be On)");
   CHECK(i >= 0 && cheats_get(i)->usable && strstr(cheats_note(i), "enable code"), "enable code note");
   i = find("Press The Button For Gold");
   CHECK(i >= 0 && cheats_get(i)->usable && strstr(cheats_note(i), "button"), "button note");
   i = find("Constant Is 9 (ROM)");
   CHECK(i >= 0 && cheats_get(i)->kind == CHEAT_KIND_GBA_GAMESHARK, "ROM patch kind");

   /* nothing touched: no file */
   CHECK(cheats_flush() == CHEATS_OK && get_file(path) == NULL, "untouched list wrote a file");
   /* switch one on: his file has that one and no other */
   lives = find("Infinite Lives");
   CHECK(cheats_set_enabled(lives, 1) == CHEATS_OK && cheats_test_pushed_ops == 1, "enable");
   CHECK(cheats_remove(find("Constant Is 9 (ROM)")) == CHEATS_E_BUILTIN &&
         cheats_count() == 11 && cheats_enabled_count() == 1,
         "a database entry cannot be deleted");
   CHECK(cheats_flush() == CHEATS_OK, "flush");
   text = get_file(path);
   CHECK(text && !strcmp(text, "cheats = 1\n\ncheat0_desc = \"Infinite Lives\"\n"
                               "cheat0_code = \"82000000+0063\"\ncheat0_enable = true\n"),
         "his file:\n%s", text ? text : "(none)");
   /* next boot: his entry first and on, the database's others after it */
   cheats_unload();
   n = cheats_load(work, rom, FE_CONSOLE_GBA);
   CHECK(n == 11 && !cheats_get(0)->builtin && cheats_get(0)->enabled &&
         !strcmp(cheats_get(0)->name, "Infinite Lives") && cheats_get(1)->builtin &&
         cheats_test_pushed_ops == 1, "second boot: %d entries", n);
   /* his own code, added: after his entries, before the database's */
   i = cheats_add("Mine", "33000010 00AB");
   CHECK(i == 1 && !cheats_get(1)->builtin && cheats_get(2)->builtin, "added at %d", i);
   /* delete his copy of a database entry: it is the database's again */
   CHECK(cheats_remove(0) == CHEATS_OK && cheats_flush() == CHEATS_OK, "remove his");
   cheats_unload();
   n = cheats_load(work, rom, FE_CONSOLE_GBA);
   CHECK(n == 12 && !strcmp(cheats_get(0)->name, "Mine") && find("Infinite Lives") > 0 &&
         cheats_get(find("Infinite Lives"))->builtin, "third boot: %d entries", n);
   cheats_unload();
   remove(path);

   /* by name: a file that is not there (no header, no CRC) */
   snprintf(rom, sizeof(rom), "%s/roms/Cheat Test (World).gba", work);
   n = cheats_load(work, rom, FE_CONSOLE_GBA);
   CHECK(n == 11 && cheats_db_match() == CHEATS_MATCH_NAME, "by full name: %d, match %d",
         n, cheats_db_match());
   snprintf(rom, sizeof(rom), "%s/roms/cheat test (Europe) [!].gba", work);
   n = cheats_load(work, rom, FE_CONSOLE_GBA);
   CHECK(n == 11 && cheats_db_match() == CHEATS_MATCH_TITLE, "by title: %d, match %d",
         n, cheats_db_match());
   snprintf(rom, sizeof(rom), "%s/roms/Another Game (USA).gba", work);
   n = cheats_load(work, rom, FE_CONSOLE_GBA);
   CHECK(n == 0 && cheats_db_match() == CHEATS_MATCH_NONE && !cheats_db_game()[0], "unknown game");
   /* the same title on the other system is another game */
   snprintf(rom, sizeof(rom), "%s/roms/Cheat Test (World).gb", work);
   CHECK(cheats_load(work, rom, FE_CONSOLE_GB) == 0, "GBA title asked for as a Game Boy game");
   cheats_unload();

   /* Game Boy: by CRC-32, hashed once and then read from the cache */
   snprintf(rom, sizeof(rom), "%s/roms/whatever.gb", work);
   copy_file("rom/gbcheat.gb", rom);
   n = cheats_load(work, rom, FE_CONSOLE_GB);
   CHECK(n == 6 && cheats_db_match() == CHEATS_MATCH_CRC &&
         !strcmp(cheats_db_game(), "GB Cheat (World)"), "GB by CRC: %d entries, match %d \"%s\"",
         n, cheats_db_match(), cheats_db_game());
   CHECK(find("Unendlich Leben") < 0 && find("Operand Is 99") >= 0 &&
         cheats_get(find("Operand Is 99"))->kind == CHEAT_KIND_GB_GAMEGENIE &&
         find("Work RAM Bank 1 Is AA") >= 0, "GB merge");
   text = get_file(cache);
   CHECK(text && strstr(text, " 65536 whatever.gb\n"), "cache: %s", text ? text : "(none)");
   cheats_unload();
   /* prove the cache is what is used: lie in it */
   put_file(cache, "DEADBEEF 65536 whatever.gb\n", 27);
   n = cheats_load(work, rom, FE_CONSOLE_GB);
   CHECK(cheats_db_match() != CHEATS_MATCH_CRC, "the cached CRC was not used (match %d)",
         cheats_db_match());
   /* ... and that then the header's title finds it */
   CHECK(n == 6 && cheats_db_match() == CHEATS_MATCH_TITLE, "GB by header title: %d, match %d",
         n, cheats_db_match());
   cheats_unload();
   remove(cache);
   cheatdb_close();
}

/* ---- 4. the bundled database --------------------------------------------------- */

static void test_real(const char *db)
{
   int games, g, i, same = 0, unique;
   unsigned total = 0;
   /* per system: games, entries, usable, value, encrypted, unsupported, other */
   unsigned stat[3][7];
   char rom[700];
   static const char *sysname[3] = { "GBA", "GB", "GBC" };

   printf("the bundled database\n");
   memset(stat, 0, sizeof(stat));
   games = cheats_db_open(db);
   CHECK(games > 1500, "games: %d", games);
   CHECK(cheatdb_cheats() > 20000, "cheats: %d", cheatdb_cheats());

   /* landmarks */
   g = cheatdb_find_serial("BPEE", &unique);
   CHECK(g >= 0 && unique, "BPEE: %d unique %d", g, unique);
   CHECK(g == cheatdb_find_name('A', "ux0:data/GBAdhoc/roms/Pokemon - Emerald Version (USA, Europe).gba", &i) && i == 1,
         "Emerald by name");
   CHECK(g == cheatdb_find_title('A', "POKEMON EMER"), "Emerald by header title");
   CHECK(cheatdb_find_serial("ZZZZ", &unique) < 0 && cheatdb_find_crc(0x12345678) < 0 &&
         cheatdb_find_title('G', "ZZQQXXKK") < 0 && cheatdb_find_title('A', "ZOO") < 0, "not found");
   g = cheatdb_find_title('G', "POKEMON RED");
   {
      unsigned n = 0;
      char *b = cheatdb_load(g, &n, NULL);
      CHECK(b && strstr(b, "Red") && n > 100, "POKEMON RED: \"%s\", %u cheats", b ? b : "", n);
      free(b);
   }
   {
      char full[256], base[256];
      cheatdb_name_keys('A', "c:\\roms\\Legend of Zelda, The - The Minish Cap (USA) (Rev 1) [!].gba",
                        full, base, sizeof(full));
      CHECK(!strcmp(full, "Aflegendofzeldathetheminishcap(usa)r1") &&
            !strcmp(base, "Ablegendofzeldathetheminishcap"), "keys: %s %s", full, base);
      cheatdb_name_keys('G', "Pokemon - Saphir-Edition (G) (V1.1) [!].gb", full, base, sizeof(full));
      CHECK(!strcmp(full, "Gfpokemonsaphiredition(germany)r1"), "GoodTools name: %s", full);
      cheatdb_name_keys('G', "Mr. Do! (Europe, USA) (En,Fr,De).gbc", full, base, sizeof(full));
      CHECK(!strcmp(full, "Gfmrdo(europe,usa)"), "regions sorted: %s", full);
   }

   /* every game: its block, its codes through cheats_validate, and its own
    * name found again by the C side's reading of names */
   for (g = 0; g < games; g++)
   {
      unsigned n = 0;
      size_t size = 0;
      char *block = cheatdb_load(g, &n, &size);
      const char *p;
      int sys, exact = 0, found, cnt, by_title;
      fe_console_t console;
      char first_code[64];

      CHECK(block != NULL, "game %d does not load", g);
      if (!block)
         continue;
      total += n;
      /* which system: ask the index both ways */
      by_title = cheatdb_find_name('A', block, &exact);
      sys = (by_title == g && exact) ? 0 : 1;
      p = block + strlen(block) + 1;
      snprintf(first_code, sizeof(first_code), "%.60s", p + strlen(p) + 1);
      console = sys ? FE_CONSOLE_GB : FE_CONSOLE_GBA;
      snprintf(rom, sizeof(rom), "/roms/%s.%s", block, sys ? "gb" : "gba");
      cnt = cheats_load(work, rom, console);
      found = !strcmp(cheats_db_game(), block) && cheats_db_match() == CHEATS_MATCH_NAME;
      same += found;
      if (!found && failures < 20)
         CHECK(0, "\"%s\" is found as \"%s\" (match %d)", block, cheats_db_game(), cheats_db_match());
      if (found)
      {
         CHECK((unsigned)cnt == n, "\"%s\": %d entries, block says %u", block, cnt, n);
         stat[sys][0]++;
         for (i = 0; i < cnt; i++)
         {
            const cheat_entry *e = cheats_get(i);
            int v = cheats_validate(e->code, console);
            stat[sys][1]++;
            CHECK((v > 0) == (e->usable != 0) && (v <= 0 || v == e->kind),
                  "\"%s\" entry %d: validate %d, list says usable %d kind %d", block, i, v,
                  e->usable, e->kind);
            if (v > 0)
               stat[sys][2]++;
            else if (v == CHEATS_E_NEEDS_VALUE)
               stat[sys][3]++;
            else if (v == CHEATS_E_ENCRYPTED)
               stat[sys][4]++;
            else if (v == CHEATS_E_UNSUPPORTED)
               stat[sys][5]++;
            else
               stat[sys][6]++;
         }
      }
      cheats_unload();
      free(block);
   }
   CHECK(same == games, "%d of %d games found by their own name", same, games);
   CHECK((int)total == cheatdb_cheats(), "cheats: %u in the blocks, %d in the header", total,
         cheatdb_cheats());
   printf("   %d games, %u cheats; through cheats_validate:\n", games, total);
   printf("   %-7s %6s %8s %8s %8s %8s %8s %8s\n", "", "games", "entries", "usable",
          "value?", "encrypt.", "type", "other");
   for (i = 0; i < 2; i++)
      printf("   %-7s %6u %8u %8u %8u %8u %8u %8u\n", i ? "GB+GBC" : sysname[0], stat[i][0],
             stat[i][1], stat[i][2], stat[i][3], stat[i][4], stat[i][5], stat[i][6]);
   CHECK(stat[0][6] == 0 && stat[1][6] == 0, "entries that are not codes shipped");
   cheatdb_close();
}

int main(int argc, char **argv)
{
   if (argc < 4)
   {
      printf("usage: test_list cheats.db fixture.db workdir\n");
      return 2;
   }
   snprintf(work, sizeof(work), "%s", argv[3]);
   {
      /* a clean work directory's cheats folder is made by the tests */
      char path[600];
      mkdir(work, 0777);
      snprintf(path, sizeof(path), "%s/cheats/index.cache", work);
      remove(path);
   }
   test_cht();
   test_rules();
   test_fixture(argv[2]);
   test_real(argv[1]);
   printf("test_list: %d checks, %d failed\n", checks, failures);
   return failures ? 1 : 0;
}
