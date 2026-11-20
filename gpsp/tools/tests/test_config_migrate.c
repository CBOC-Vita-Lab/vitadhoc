/* test_config_migrate.c -- a CONFIG.INI written by an older VitAdhoc keeps the player's choices.
 *
 * tools/tests/fixtures/config-1.3.1.ini and config-2.0-rc1.ini are the files those versions' own
 * pcfg_save wrote (built from the v1.3.1 tag's and integ-2.0's config_psp.c, fed a player's non-default
 * choices: tools/tests/fixtures/README says how).  This build loads each one:
 *   1. every choice the file holds is what g_pcfg / g_pdisp hold afterwards (a table of the keys);
 *   2. a key the older version did not have gets this version's default, which for the
 *      improvements is ON (1.3.1 has no smooth_handoff, steady_pacing, core4, nes_wide, ws_blend),
 *      and where 2.0-rc1's file says the player turned one off, it stays off;
 *   3. saving and loading again changes none of it, and the player's lines are still in the file.
 * gcc -DVITA -I vita -I psp -I frontend-common -I libretro/libretro-common/include -I .
 *     tools/tests/test_config_migrate.c psp/config_psp.c psp/ctl_map.c frontend-common/fe_util.c
 *     -Wl,--unresolved-symbols=ignore-all  (run from gpsp/ with the fixture path as argument) */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ctl_map.h"
#include "config_psp.h"
#include "fe_util.h"

void fe_evt(const char *fmt, ...) { (void)fmt; }
void fe_log(const char *fmt, ...) { (void)fmt; }
const char *vid_scale_name(int m) { (void)m; return "t"; }
const char *vid_filter_name(int m) { (void)m; return "t"; }
const char *pspcompat_user_name(void) { return "Player"; }
static int g_direct;                    /* vita.ini's direct connection (Internet) switch */
int ui_host_link_direct(void) { return g_direct; }

static int checks, fails;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL %s:%d: %s ", __FILE__, __LINE__, #c); \
   fprintf(stderr, "" __VA_ARGS__); fputc('\n', stderr); } } while (0)

struct key { const char *name; int *field; };
#define D(c) g_pdisp[c]
static const struct key keys[] = {
   { "scale_gba", &D(FE_CONSOLE_GBA).scale }, { "filter_gba", &D(FE_CONSOLE_GBA).filter },
   { "ambient_gba", &D(FE_CONSOLE_GBA).ambient }, { "quality_gba", &D(FE_CONSOLE_GBA).quality },
   { "color_gba", &D(FE_CONSOLE_GBA).color }, { "blend_gba", &D(FE_CONSOLE_GBA).blend },
   { "wide_gba", &D(FE_CONSOLE_GBA).wide }, { "wide_fill_gba", &D(FE_CONSOLE_GBA).wide_fill },
   { "scale_gb", &D(FE_CONSOLE_GB).scale }, { "filter_gb", &D(FE_CONSOLE_GB).filter },
   { "quality_gb", &D(FE_CONSOLE_GB).quality }, { "gb_palette_gb", &D(FE_CONSOLE_GB).gb_palette },
   { "ff_mult_x10", &g_pcfg.ff_mult_x10 }, { "ff_hold", &g_pcfg.ff_hold },
   { "stick_rewind", &g_pcfg.stick_rewind }, { "rewind", &g_pcfg.rewind },
   { "show_fps", &g_pcfg.show_fps }, { "hints", &g_pcfg.hints }, { "ui_home", &g_pcfg.ui_home },
   { "theme", &g_pcfg.theme }, { "previews", &g_pcfg.previews }, { "full_speed", &g_pcfg.full_speed },
   { "link_delay", &g_pcfg.link_delay },
   { 0, 0 }
};

static int file_value(const char *text, const char *key, char *out, size_t cap)
{
   size_t kl = strlen(key);
   const char *p = text;
   while (p && *p) {
      const char *nl = strchr(p, '\n');
      if (!strncmp(p, key, kl) && (p[kl] == ' ' || p[kl] == '=')) {
         const char *v = strchr(p, '=');
         size_t n;
         if (!v) return 0;
         v++;
         while (*v == ' ') v++;
         n = nl ? (size_t)(nl - v) : strlen(v);
         if (n >= cap) n = cap - 1;
         memcpy(out, v, n); out[n] = 0;
         return 1;
      }
      p = nl ? nl + 1 : NULL;
   }
   return 0;
}

static char *slurp(const char *path)
{
   FILE *f = fopen(path, "rb");
   char *b;
   long n;
   if (!f) return NULL;
   fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
   b = malloc((size_t)n + 1);
   if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
   if (b) b[n] = 0;
   fclose(f);
   return b;
}

static void check_choices(const char *tag, const char *fixture_text)
{
   const struct key *k;
   char v[64];
   for (k = keys; k->name; k++) {
      if (!file_value(fixture_text, k->name, v, sizeof(v))) continue;
      CHECK(*k->field == atoi(v), "%s: %s is %d, the file says %s", tag, k->name, *k->field, v);
   }
   if (file_value(fixture_text, "nick", v, sizeof(v)))
      CHECK(!strcmp(g_pcfg.nick, v), "%s: nick '%s' vs '%s'", tag, g_pcfg.nick, v);
   if (file_value(fixture_text, "group", v, sizeof(v)))
      CHECK(!strcmp(g_pcfg.group, v), "%s: group", tag);
   if (file_value(fixture_text, "last_rom", v, sizeof(v)))
      CHECK(!strcmp(g_pcfg.last_rom, v), "%s: last_rom '%s' vs '%s'", tag, g_pcfg.last_rom, v);
}

int main(int argc, char **argv)
{
   const char *names[2] = { "1.3.1", "2.0-rc1" };
   int i;
   if (argc < 3) { fprintf(stderr, "usage: %s config-1.3.1.ini config-2.0-rc1.ini\n", argv[0]); return 2; }
   for (i = 0; i < 2; i++) {
      char work[64], *text, *after;
      snprintf(work, sizeof(work), "/tmp/cfg-migrate-%d-%d.ini", (int)getpid(), i);
      text = slurp(argv[1 + i]);
      CHECK(text != NULL, "fixture %s", argv[1 + i]);
      if (!text) continue;
      { FILE *f = fopen(work, "wb"); fwrite(text, 1, strlen(text), f); fclose(f); }
      pcfg_load(work);
      check_choices(names[i], text);
      /* net_conn: 2.0 moved "Internet" (1, the joiner types an address) behind a vita.ini switch and made
       * Online (3) the way over the internet: an old 1 becomes 3 unless the direct switch is on, which
       * keeps it; the other values are untouched */
      if (i == 0) {
         CHECK(g_pcfg.net_conn == 3, "1.3.1: net_conn 1 (Internet) becomes Online (3) (%d)", g_pcfg.net_conn);
         g_direct = 1; pcfg_load(work);
         CHECK(g_pcfg.net_conn == 1, "1.3.1: ...and stays 1 with the direct switch on (%d)", g_pcfg.net_conn);
         g_direct = 0; pcfg_load(work);
      } else
         CHECK(g_pcfg.net_conn == 3, "2.0-rc1: net_conn 3 (Online) stays (%d)", g_pcfg.net_conn);
      /* the improvements 1.3.1 never had: this version's defaults, ON */
      if (i == 0) {
         CHECK(g_pcfg.smooth_handoff == 1, "1.3.1: Smooth hand-over defaults on");
         CHECK(g_pcfg.steady_pacing == 1, "1.3.1: Steady pacing defaults on");
         CHECK(g_pcfg.core4_off == 0, "1.3.1: Fourth core defaults to Auto");
         CHECK(g_pcfg.nes_wide == 0, "1.3.1: NES Wide view defaults to Auto");
         CHECK(g_pcfg.ws_blend == 1, "1.3.1: WonderSwan smoothing defaults on");
      } else {
         /* what a 2.0-rc1 player turned off stays off */
         CHECK(g_pcfg.smooth_handoff == 0, "2.0-rc1: Smooth hand-over stays off");
         CHECK(g_pcfg.steady_pacing == 0, "2.0-rc1: Steady pacing stays off");
         CHECK(g_pcfg.core4_off == 1, "2.0-rc1: Fourth core stays off");
         CHECK(g_pcfg.nes_wide == 2, "2.0-rc1: NES Wide view stays off");
         CHECK(g_pcfg.ws_blend == 0, "2.0-rc1: WonderSwan smoothing stays off");
      }
      /* save, load again: nothing moves, and the player's lines are still there */
      pcfg_save();
      memset(&g_pcfg, 0, sizeof(g_pcfg));
      pcfg_load(work);
      after = slurp(work);
      CHECK(after != NULL, "saved file");
      if (after) {
         check_choices(names[i], text);
         { const struct key *k; char a[64], b[64];
           for (k = keys; k->name; k++)
              if (file_value(text, k->name, a, sizeof(a)))
                 CHECK(file_value(after, k->name, b, sizeof(b)) && !strcmp(a, b), "%s: saved file keeps %s = %s (now %s)", names[i], k->name, a, b); }
         free(after);
      }
      free(text);
      unlink(work);
   }
   printf("config migrate: %d checks, %d failed\n", checks, fails);
   return fails ? 1 : 0;
}
