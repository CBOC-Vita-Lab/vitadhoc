/* test_link_mode.c -- the link-mode pieces the PSP UI shares with this test
 * (docs/GBA-LINK-UI.md): the game table (link_games.h), the per-game
 * override file (fe_linkmode.c) and the room names (psp/room_label.h).
 * Run by the fe_favs host suite (tools/run_host_tests.py). */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "link_games.h"
#include "fe_linkmode.h"
#include "../frontend-common/fe_linkmode.c"
#include "../psp/room_label.h"

static int fails;
#define CHECK(c)                                                         \
   do {                                                                  \
      if (!(c)) {                                                        \
         fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);    \
         fails++;                                                        \
      }                                                                  \
   } while (0)

static void test_table(void)
{
   size_t i, j;
   for (i = 0; i < LINK_GAMES_N; i++)
   {
      const link_game *g = &LINK_GAMES[i];
      CHECK(g->title && strlen(g->title) <= 40);
      CHECK(g->mode == LINK_CABLE || g->mode == LINK_ADAPTER ||
            g->mode == LINK_BOTH);
      for (j = 0; j < 3; j++)
         CHECK(room_is_alnum(g->code[j]));
      if (i)
         CHECK(memcmp(LINK_GAMES[i - 1].code, g->code, 3) < 0); /* sorted, unique */
      CHECK(link_game_find((const char *)g->code) == g);
   }
   CHECK(link_game_mode("BPEE") == LINK_BOTH);
   CHECK(link_game_mode("BPRJ") == LINK_BOTH);     /* any region */
   CHECK(link_game_mode("BPGE") == LINK_BOTH);
   CHECK(link_game_mode("AW2E") == LINK_CABLE);
   CHECK(link_game_mode("AMKE") == LINK_CABLE);
   CHECK(link_game_mode("BMGE") == LINK_ADAPTER);
   CHECK(link_game_mode("ZZZZ") == LINK_CABLE);    /* unlisted -> cable */
   CHECK(link_game_mode("") == LINK_CABLE);
   CHECK(link_game_mode(NULL) == LINK_CABLE);
   CHECK(link_game_title("ZZZZ") == NULL);
   CHECK(!strcmp(link_game_title("AMKE"), "Mario Kart Super Circuit"));
   CHECK(!strcmp(link_game_title("AW2P"), "Advance Wars 2"));
}

static void test_override(void)
{
   static fe_linkmode m;
   char buf[512];
   size_t n;
   const char *txt =
      "# comment\r\n"
      "  Pokemon Unbound.gba = cable \r\n"
      "hacks/Radical Red.gba=adapter\n"
      "a=b=c.gba = cable\n"            /* split at the LAST '=' */
      "bad.gba = wireless\n"           /* unknown value: ignored */
      "= cable\n"                      /* no name */
      "noequals.gba\n"
      "Pokemon Unbound.gba = adapter\n";   /* repeat: last wins */
   CHECK(fe_linkmode_parse(&m, txt, strlen(txt)) == 3);
   CHECK(fe_linkmode_get(&m, "Pokemon Unbound.gba") == FE_LM_ADAPTER);
   CHECK(fe_linkmode_get(&m, "hacks/Radical Red.gba") == FE_LM_ADAPTER);
   CHECK(fe_linkmode_get(&m, "a=b=c.gba") == FE_LM_CABLE);
   CHECK(fe_linkmode_get(&m, "bad.gba") == FE_LM_AUTO);
   CHECK(fe_linkmode_get(&m, "missing.gba") == FE_LM_AUTO);
   CHECK(fe_linkmode_set(&m, "hacks/Radical Red.gba", FE_LM_AUTO) == 0);
   CHECK(fe_linkmode_get(&m, "hacks/Radical Red.gba") == FE_LM_AUTO);
   CHECK(m.n == 2);
   CHECK(fe_linkmode_set(&m, "new.gba", FE_LM_CABLE) == 0);
   n = fe_linkmode_serialise(&m, buf, sizeof(buf));
   CHECK(n == strlen(buf));
   CHECK(strstr(buf, "Pokemon Unbound.gba = adapter\n") != NULL);
   CHECK(strstr(buf, "new.gba = cable\n") != NULL);
   CHECK(strstr(buf, "Radical") == NULL);
   {
      /* round trip */
      static fe_linkmode m2;
      CHECK(fe_linkmode_parse(&m2, buf, n) == 3);
      CHECK(fe_linkmode_get(&m2, "new.gba") == FE_LM_CABLE);
      CHECK(fe_linkmode_get(&m2, "a=b=c.gba") == FE_LM_CABLE);
   }
   {
      /* short buffer: truncated, terminated, full size reported */
      char small[16];
      size_t need = fe_linkmode_serialise(&m, small, sizeof(small));
      CHECK(need == n);
      CHECK(strlen(small) == sizeof(small) - 1);
   }
   {
      /* full table */
      int i;
      char nm[32];
      fe_linkmode_clear(&m);
      for (i = 0; i < FE_LINKMODE_MAX; i++)
      {
         snprintf(nm, sizeof(nm), "g%d.gba", i);
         CHECK(fe_linkmode_set(&m, nm, FE_LM_CABLE) == 0);
      }
      CHECK(fe_linkmode_set(&m, "one-more.gba", FE_LM_CABLE) == -1);
      CHECK(fe_linkmode_set(&m, "g5.gba", FE_LM_ADAPTER) == 0);  /* update */
      CHECK(fe_linkmode_set(&m, "", FE_LM_CABLE) == -1);
   }
   CHECK(fe_linkmode_effective(LINK_BOTH, FE_LM_AUTO) == LINK_BOTH);
   CHECK(fe_linkmode_effective(LINK_BOTH, FE_LM_CABLE) == LINK_CABLE);
   CHECK(fe_linkmode_effective(LINK_CABLE, FE_LM_ADAPTER) == LINK_ADAPTER);
   CHECK(fe_linkmode_effective(LINK_ADAPTER, FE_LM_AUTO) == LINK_ADAPTER);
   CHECK(fe_linkmode_effective(7, FE_LM_AUTO) == LINK_CABLE);
}

static void test_rooms(void)
{
   char b[ROOM_LABEL_CAP], code[5], g[16];
   int num;
   CHECK(room_parse("GPSP07", &num, code) == ROOM_ADAPTER && num == 7 &&
         code[0] == 0);
   CHECK(room_parse("GC07AMKE", &num, code) == ROOM_CABLE_GBA && num == 7 &&
         !strcmp(code, "AMKE"));
   CHECK(room_parse("GB12POKE", &num, code) == ROOM_CABLE_GB && num == 12 &&
         !strcmp(code, "POKE"));
   CHECK(room_parse("GC07AMK", &num, code) == ROOM_OTHER && num == -1);
   CHECK(room_parse("GC07AMKE1", &num, code) == ROOM_OTHER);
   CHECK(room_parse("GC07AM-E", &num, code) == ROOM_OTHER);
   CHECK(room_parse("GPSP7", &num, code) == ROOM_OTHER);
   CHECK(room_parse("GBLNK7", &num, code) == ROOM_OTHER);
   CHECK(room_parse(NULL, &num, code) == ROOM_OTHER);
   /* "Share game data" rooms (docs/SAVE-SHARE.md): their own kind, never a
    * cable or adapter room, and a round trip through room_make. */
   {
      char g[9], lb[ROOM_LABEL_CAP];
      CHECK(room_parse("GS07DATA", &num, code) == ROOM_SHARE && num == 7 &&
            code[0] == 0);
      CHECK(room_parse("GS07DATB", &num, code) == ROOM_OTHER);
      CHECK(room_parse("GS07DAT", &num, code) == ROOM_OTHER);
      CHECK(room_parse("GS7DATA", &num, code) == ROOM_OTHER);
      CHECK(!strcmp(room_make(ROOM_SHARE, 42, NULL, g, sizeof(g)),
                    "GS42DATA"));
      CHECK(room_parse(g, &num, code) == ROOM_SHARE && num == 42);
      CHECK(!strcmp(room_label(g, lb, sizeof(lb)), "Room 42"));
      CHECK(room_number(g) == 42);
   }
   CHECK(!strcmp(room_label("GPSP07", b, sizeof(b)), "Room 7"));
   CHECK(!strcmp(room_label("GC07AMKE", b, sizeof(b)), "Room 7"));
   CHECK(!strcmp(room_label("GB99GAME", b, sizeof(b)), "Room 99"));
   CHECK(!strcmp(room_label("GPSP00", b, sizeof(b)), "Room 0"));
   CHECK(!strcmp(room_label("GBLNK7", b, sizeof(b)), "GBLNK7"));
   CHECK(!strcmp(room_make(ROOM_ADAPTER, 7, NULL, g, sizeof(g)), "GPSP07"));
   CHECK(!strcmp(room_make(ROOM_CABLE_GBA, 7, "AMKE", g, sizeof(g)),
                 "GC07AMKE"));
   CHECK(!strcmp(room_make(ROOM_CABLE_GB, 3, "POKE", g, sizeof(g)),
                 "GB03POKE"));
   CHECK(!strcmp(room_make(ROOM_CABLE_GB, 103, "", g, sizeof(g)), "GB03GAME"));
   room_code_gba("AW2E", code);
   CHECK(!strcmp(code, "AW2E"));
   room_code_gba("A\x01 E", code);
   CHECK(!strcmp(code, "A00E"));
   room_code_gb("POKEMON_SLV", 11, code);
   CHECK(!strcmp(code, "POKE"));
   room_code_gb("A B-C", 5, code);
   CHECK(!strcmp(code, "GAME"));                     /* only 3 alnum */
   room_code_gb("ZEL\0DA", 6, code);
   CHECK(!strcmp(code, "GAME"));                     /* stops at the NUL */
   room_code_gb("T.E.T.R.I.S", 11, code);
   CHECK(!strcmp(code, "TETR"));
   CHECK(room_number("GC42AW2E") == 42);
   CHECK(room_number("GBLNK7") == -1);
   /* every name room_make produces parses back, and fits adhocctl's 8 */
   {
      int k, n;
      for (k = ROOM_ADAPTER; k <= ROOM_CABLE_GB; k++)
         for (n = 0; n < 100; n++)
         {
            int back;
            room_make(k, n, "AB12", g, sizeof(g));
            CHECK(strlen(g) <= 8);
            CHECK(room_parse(g, &back, code) == k && back == n);
         }
   }
}

int main(void)
{
   test_table();
   test_override();
   test_rooms();
   if (fails)
   {
      fprintf(stderr, "test_link_mode: %d FAILED\n", fails);
      return 1;
   }
   printf("test_link_mode: PASS (%u games)\n", (unsigned)LINK_GAMES_N);
   return 0;
}
