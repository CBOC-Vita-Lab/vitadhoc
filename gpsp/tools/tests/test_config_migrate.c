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
 * gcc -DVITA -I vita -I psp -I frontend-common -I ../playerkit/src
 *     -I libretro/libretro-common/include -I .
 *     tools/tests/test_config_migrate.c psp/config_psp.c psp/ctl_map.c frontend-common/fe_util.c
 *     -Wl,--unresolved-symbols=ignore-all  (run from gpsp/ with the fixture path as argument) */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ctl_map.h"
#include "config_psp.h"
#include "video_psp.h"
#include "fe_util.h"

void fe_evt(const char *fmt, ...) { (void)fmt; }
void fe_log(const char *fmt, ...) { (void)fmt; }
const char *vid_scale_name(int m) { (void)m; return "t"; }
const char *vid_filter_name(int m) { (void)m; return "t"; }
const char *pspcompat_user_name(void) { return "Player"; }
int pspcompat_system_lang(void) { return 1; }
const char *plat_app_dir(void) { return "."; }
void i18n_dir(const char *d) { (void)d; }
void i18n_use(int l, int sys) { (void)l; (void)sys; }
int i18n_current(void) { return 1; }
int i18n_index(const char *code) { (void)code; return 0; }
const char *const i18n_code[10] = { "auto", "en", "es", "fr", "de", "it", "pt-BR", "ja", "zh-Hans", "xx-long" };
void vglyph_region(int sc) { (void)sc; }
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
   { "color_gba", &D(FE_CONSOLE_GBA).color },
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
      unlink("/tmp/display.ini");
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
      unlink("/tmp/display.ini");
   }
   /* 2.0: the looks became rows (docs/LOOKS.md).  Each old form migrates, once, and the new keys win. */
   {
      const char *w = "/tmp/cfg-looks-migrate.ini";
      FILE *f;
      unlink("/tmp/display.ini");
      /* a 1.x file: the grid and scanline FILTERS (with vita.ini's one strength), the three-way ambient, blend on/off */
      f = fopen(w, "wb");
      fputs("filter_gba = 3\nfilter_gb = 6\nfilter_gbc = 2\nambient_gba = 0\nambient_gb = 1\nambient_gbc = 2\n"
            "blend_gba = 1\nblend_gb = 0\ncolor_gbc = 0\nconsole = 0\nfilter_nes = 3\n", f);
      fclose(f);
      g_pcfg_legacy_grid = 8;                      /* vita.ini grid_strength = 8 */
      pcfg_load(w);
      CHECK(D(FE_CONSOLE_GBA).filter == VID_FILTER_SHARP && D(FE_CONSOLE_GBA).grid == 8 && D(FE_CONSOLE_GBA).scan == 0,
            "filter LCD grid -> sharp + LCD grid at the old strength (%d/%d/%d)", D(FE_CONSOLE_GBA).filter, D(FE_CONSOLE_GBA).grid, D(FE_CONSOLE_GBA).scan);
      CHECK(D(FE_CONSOLE_GB).filter == VID_FILTER_SHARP && D(FE_CONSOLE_GB).scan == 8 && D(FE_CONSOLE_GB).grid == 0,
            "filter scanlines -> sharp + Scanlines at the old strength");
      CHECK(D(FE_CONSOLE_GBC).filter == VID_FILTER_SHARP && D(FE_CONSOLE_GBC).scan == 0 && D(FE_CONSOLE_GBC).grid == 0,
            "a plain filter brings no look");
      CHECK(D(FE_CONSOLE_NES).grid == 8 && D(FE_CONSOLE_NES).filter == VID_FILTER_SHARP, "the NES profile migrates too");
      CHECK(D(FE_CONSOLE_GBA).glow == 0 && D(FE_CONSOLE_GB).glow == PCFG_GLOW_DEF && D(FE_CONSOLE_GBC).glow == PCFG_GLOW_DEF,
            "ambient off -> glow 0, art / art-else-game -> the default strength (%d %d %d)",
            D(FE_CONSOLE_GBA).glow, D(FE_CONSOLE_GB).glow, D(FE_CONSOLE_GBC).glow);
      CHECK(D(FE_CONSOLE_GBA).ambient == PCFG_AMB_ART_GAME && D(FE_CONSOLE_GB).ambient == PCFG_AMB_ART && D(FE_CONSOLE_GBC).ambient == PCFG_AMB_ART_GAME,
            "the old mode stays the glow's source, never 'off'");
      CHECK(D(FE_CONSOLE_GBA).ghost == 5 && D(FE_CONSOLE_GB).ghost == 0, "blend on -> LCD ghosting 5 (half and half), off -> 0");
      CHECK(D(FE_CONSOLE_GBC).color == 0, "a stored GBC colour 0 stays vivid");
      CHECK(D(FE_CONSOLE_GBA).color == 1, "an absent GBA colour is the new default, the GBA screen");
      CHECK(D(FE_CONSOLE_WSWAN).color == 0 && D(FE_CONSOLE_NES).color == 0, "WonderSwan and NES colours start vivid");
      CHECK(D(FE_CONSOLE_NES).ghost == 0 && D(FE_CONSOLE_GBA).scan == 0, "no ghosting on a television");
      /* saving writes the new keys and a 1.x-readable mirror; loading them again changes nothing; the new keys beat the old */
      pcfg_save();
      CHECK(fe_ini_get_int(w, "grid_gba", -1) == 8 && fe_ini_get_int(w, "scan_gb", -1) == 8 &&
            fe_ini_get_int(w, "glow_gba", -1) == 0 && fe_ini_get_int(w, "glow_gb", -1) == PCFG_GLOW_DEF &&
            fe_ini_get_int(w, "ghost_gba", -1) == 5 && fe_ini_get_int(w, "blend_gba", -1) == 1 &&
            fe_ini_get_int(w, "ambient_gba", -1) == 0 && fe_ini_get_int(w, "filter_gba", -1) == VID_FILTER_SHARP,
            "the new keys are written, with ambient / blend as a 1.x build reads them");
      g_pcfg_legacy_grid = 3;                      /* a later vita.ini must not matter any more */
      pcfg_load(w);
      CHECK(D(FE_CONSOLE_GBA).grid == 8 && D(FE_CONSOLE_GBA).glow == 0 && D(FE_CONSOLE_GBA).ghost == 5 && D(FE_CONSOLE_GB).scan == 8,
            "a second load keeps what was saved");
      /* hand-written new keys win over the old ones and are clamped */
      f = fopen(w, "wb");
      fputs("filter_gba = 3\ngrid_gba = 2\nscan_gba = 99\nglow_gba = 10\nambient_gba = 0\nghost_gba = 7\nblend_gba = 0\n"
            "color_gba = 2\nghost_nes = 9\nscan_gb = -4\n", f);
      fclose(f);
      pcfg_load(w);
      CHECK(D(FE_CONSOLE_GBA).grid == 2 && D(FE_CONSOLE_GBA).scan == PCFG_SCAN_DEF, "grid_ key wins over the filter; scan 99 clamps to off (%d/%d)", D(FE_CONSOLE_GBA).grid, D(FE_CONSOLE_GBA).scan);
      CHECK(D(FE_CONSOLE_GBA).glow == 10 && D(FE_CONSOLE_GBA).ghost == 7 && D(FE_CONSOLE_GBA).color == 2, "glow / ghost / colour keys win");
      CHECK(D(FE_CONSOLE_NES).ghost == 0 && D(FE_CONSOLE_GB).scan == PCFG_SCAN_DEF, "ghosting is not a television's; a negative scan is off");
      CHECK(pcfg_ghost_modes(FE_CONSOLE_GBA) && pcfg_ghost_modes(FE_CONSOLE_GB) && pcfg_ghost_modes(FE_CONSOLE_GBC) &&
            pcfg_ghost_modes(FE_CONSOLE_WSWAN) && !pcfg_ghost_modes(FE_CONSOLE_NES) && !pcfg_ghost_modes(FE_CONSOLE_SNES),
            "LCD ghosting is for the handhelds");
      CHECK(pcfg_color_modes(FE_CONSOLE_GBA) == 3 && pcfg_color_modes(FE_CONSOLE_GBC) == 2 && pcfg_color_modes(FE_CONSOLE_WSWAN) == 2 &&
            pcfg_color_modes(FE_CONSOLE_NES) == 2 && pcfg_color_modes(FE_CONSOLE_GB) == 0, "LCD colours: GBA 3, GBC / WS / NES 2, GB palette row");
      unlink(w);
   }
   printf("config migrate: %d checks, %d failed\n", checks, fails);
   return fails ? 1 : 0;
}
