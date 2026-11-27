/* boot_guard.c: the crash-loop guard.  gcc -I vita -I frontend-common
 *   vita/tests/guard/test_boot_guard.c vita/boot_guard.c frontend-common/fe_util.c
 * Each "crash" is a start that ends without boot_guard_game_end: the process
 * is simulated by boot_guard_reset_for_tests + boot_guard_init on the same
 * folder. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "boot_guard.h"

void fe_evt(const char *f, ...) { (void)f; }

static char root[128];
static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static int exists(const char *name)
{
   char p[300];
   struct stat st;
   snprintf(p, sizeof(p), "%s/guard/%s", root, name);
   return stat(p, &st) == 0;
}

static void new_run(void)            /* the app starts again */
{
   boot_guard_init(root);
}

static const unsigned ALL = BG_RISK_HANDOFF | BG_RISK_CORE4 | BG_RISK_WIDE |
                            BG_RISK_QUALITY | BG_RISK_STEADY;

int main(void)
{
   char t[96], l1[128], l2[128], full[400];
   snprintf(root, sizeof(root), "/tmp/bg-test-%d", (int)getpid());
   {
      char cmd[400];
      snprintf(cmd, sizeof(cmd), "rm -rf %s; mkdir -p %s", root, root);
      if (system(cmd)) return 2;
   }

   /* one crash: nothing */
   new_run();
   CHECK(exists("app.mark"));
   CHECK(boot_guard_game_begin("roms/Alpha Game.gba", ALL) == 0);
   CHECK(exists("game.mark") && !exists("app.mark"));
   boot_guard_tick(1000000);
   boot_guard_tick(5000000);          /* 4 s: no clear */
   CHECK(exists("game.mark"));
   new_run();                          /* it died */
   CHECK(!boot_guard_take_notice(t, 96, l1, 128, l2, 128, full, 400));
   CHECK(!boot_guard_core4_off());
   CHECK(boot_guard_game_begin("roms/Alpha Game.gba", ALL) == 0);   /* still risky */
   boot_guard_game_end();              /* a clean exit clears it */
   CHECK(!exists("game.mark") && !exists("state.ini"));

   /* a crash, a clean finish, a crash: not two in a row */
   new_run();
   boot_guard_game_begin("roms/Alpha Game.gba", ALL);
   new_run();
   boot_guard_game_begin("roms/Alpha Game.gba", ALL);
   boot_guard_game_end();
   new_run();
   boot_guard_game_begin("roms/Alpha Game.gba", ALL);
   new_run();
   CHECK(!boot_guard_take_notice(t, 96, l1, 128, l2, 128, full, 400));
   boot_guard_game_end();

   /* stable play clears the marker and the streak */
   new_run();
   boot_guard_game_begin("roms/Alpha Game.gba", ALL);
   new_run();                          /* one unfinished */
   boot_guard_game_begin("roms/Alpha Game.gba", ALL);
   boot_guard_tick(100);
   boot_guard_tick(100 + 19u * 1000000u);
   CHECK(exists("game.mark"));
   boot_guard_tick(100 + 20u * 1000000u);
   CHECK(!exists("game.mark") && !exists("state.ini"));
   new_run();
   CHECK(!boot_guard_take_notice(t, 96, l1, 128, l2, 128, full, 400));

   /* two crashes in a row with the same game: safe + one notice */
   new_run();
   boot_guard_game_begin("roms/Beta.gba", ALL);
   new_run();
   boot_guard_game_begin("roms/Beta.gba", ALL);
   new_run();
   CHECK(boot_guard_take_notice(t, 96, l1, 128, l2, 128, full, 400));
   CHECK(!strcmp(full, "VitAdhoc closed unexpectedly twice with Beta; it now starts "
                       "with safe picture settings. Settings > This game to change them."));
   CHECK(!boot_guard_take_notice(t, 96, l1, 128, l2, 128, full, 400));   /* once */
   CHECK(boot_guard_core4_off());
   CHECK(boot_guard_game_begin("roms/Beta.gba", ALL) == 1);
   CHECK(boot_guard_safe());
   CHECK(boot_guard_take_look() == 1 && boot_guard_take_look() == 0);   /* once */
   CHECK(!exists("game.mark"));        /* safe settings are the floor: no marker */
   boot_guard_game_end();
   CHECK(!boot_guard_safe());
   /* another game is unaffected */
   CHECK(boot_guard_game_begin("roms/Gamma.gba", ALL) == 0);
   boot_guard_game_end();
   /* it stays safe in the next run, and the core stays off until released */
   new_run();
   CHECK(boot_guard_core4_off());
   CHECK(boot_guard_game_begin("roms/Beta.gba", ALL) == 1);
   CHECK(boot_guard_take_look() == 0);   /* the player's own look from now on */
   boot_guard_release();
   CHECK(!boot_guard_safe() && !boot_guard_core4_off());
   boot_guard_game_end();
   new_run();
   CHECK(!boot_guard_core4_off());
   CHECK(boot_guard_game_begin("roms/Beta.gba", ALL) == 0);
   boot_guard_game_end();

   /* two crashes of two different games are not a streak */
   new_run();
   boot_guard_game_begin("roms/Delta.gba", ALL);
   new_run();
   boot_guard_game_begin("roms/Eps.gba", ALL);
   new_run();
   CHECK(!boot_guard_take_notice(t, 96, l1, 128, l2, 128, full, 400));
   CHECK(boot_guard_game_begin("roms/Delta.gba", ALL) == 0);
   boot_guard_game_end();

   /* the app itself: two starts that never reach the list */
   new_run();                          /* start 1 (app.mark written) */
   new_run();                          /* crashed: 1 */
   new_run();                          /* crashed: 2 */
   CHECK(boot_guard_take_app_revert());
   CHECK(!boot_guard_take_app_revert());
   CHECK(boot_guard_take_notice(t, 96, l1, 128, l2, 128, full, 400));
   CHECK(strstr(full, "before the game list") != NULL);
   boot_guard_app_ready();
   CHECK(!exists("app.mark"));
   new_run();                          /* a clean start after it */
   boot_guard_app_ready();
   new_run();
   CHECK(!boot_guard_take_app_revert());
   boot_guard_app_ready();

   /* a player program: no report twice, quickly => safe; with a report => clear */
   new_run();
   boot_guard_player_begin("roms/Snes One.sfc", ALL);
   CHECK(exists("player.mark"));
   boot_guard_player_end(1, 30);       /* its report was there */
   CHECK(!exists("player.mark") && !exists("state.ini"));
   boot_guard_player_begin("roms/Snes One.sfc", ALL);
   boot_guard_player_end(0, 5);        /* died: 1 */
   CHECK(!boot_guard_take_notice(t, 96, l1, 128, l2, 128, full, 400));
   boot_guard_player_begin("roms/Snes One.sfc", ALL);
   boot_guard_player_end(0, 7);        /* died: 2 */
   CHECK(boot_guard_take_notice(t, 96, l1, 128, l2, 128, full, 400));
   CHECK(strstr(full, "Snes One") != NULL);
   CHECK(boot_guard_game_begin("roms/Snes One.sfc", ALL) == 1);
   boot_guard_game_end();
   /* closed from LiveArea after a long game: not a crash */
   boot_guard_player_begin("roms/Snes Two.sfc", ALL);
   boot_guard_player_end(0, 4000);
   boot_guard_player_begin("roms/Snes Two.sfc", ALL);
   boot_guard_player_end(0, 4000);
   CHECK(!boot_guard_take_notice(t, 96, l1, 128, l2, 128, full, 400));
   /* no risky feature on: no marker at all */
   boot_guard_player_begin("roms/Snes Three.sfc", 0);
   CHECK(!exists("player.mark"));
   CHECK(boot_guard_game_begin("roms/Plain.gba", 0) == 0 && !exists("game.mark"));
   boot_guard_game_end();

   {
      char cmd[400];
      snprintf(cmd, sizeof(cmd), "rm -rf %s", root);
      if (system(cmd)) return 2;
   }
   printf("boot guard: %d checks, %d failed\n", checks, fails);
   return fails ? 1 : 0;
}
