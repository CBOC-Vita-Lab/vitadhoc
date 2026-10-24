#define PSPCOMPAT_KERNEL_CALLS
#include "../../pspcompat.h"
#include "../../vstyle.h"
#include "../../artfetch.h"
#include "../../../psp/video_psp.h"
static SceUID record_directory_open(const char *path);
static int record_directory_read(SceUID dir, SceIoDirent *entry);
unsigned record_artwork_list(const char *const *paths, const char *const *names, int n);
static int fixture_art_progress(int *seen, int *total, int *have);
static int fixture_art_active(void);
static float record_style_text(int face, float x, float y, const char *s, uint32_t rgba);
static void record_style_center(int face, float cx, float y, const char *s, uint32_t rgba);
static void record_ui_text(int x, int y, const char *s, uint16_t color);
static void record_ui_center(int y, const char *s, uint16_t color);
static void record_art_retry(void);
#undef sceIoDopen
#define sceIoDopen record_directory_open
#undef sceIoDread
#define sceIoDread record_directory_read
#define artfetch_list record_artwork_list
#define artfetch_progress fixture_art_progress
#define artfetch_active fixture_art_active
#define artfetch_retry record_art_retry
#define vs_text record_style_text
#define vs_text_c record_style_center
#define vid_text record_ui_text
#define vid_text_center record_ui_center
#include "../../../psp/ui_psp.c"
#undef artfetch_list
#undef artfetch_progress
#undef artfetch_active
#undef artfetch_retry
#undef vs_text
#undef vs_text_c
#undef vid_text
#undef vid_text_center
#undef sceIoDopen
#undef sceIoDread

#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../../upscale_vita.h"

static int handed, handed_n;
static unsigned directory_opens;
static int fail_directory_read_after = -1;
static char handed_paths[16][PSP_FILE_PATH_CAP * 2];
static char handed_last[PSP_FILE_PATH_CAP * 2];
static int art_busy, art_seen, art_total, art_have, art_retries;
static struct {
   char text[256];
   float x, y, w, h;
} text_draws[256];
static int text_count;

/* Only the worker's asynchronous input is substituted. All text still goes
 * through the real fonts and software renderer, with its actual bounds. */
static int fixture_art_progress(int *seen, int *total, int *have)
{
   *seen = art_seen;
   *total = art_total;
   *have = art_have;
   return art_busy;
}

static int fixture_art_active(void)
{
   return 1;
}

static void record_text(float x, float y, float w, float h, const char *s)
{
   if (text_count < (int)(sizeof(text_draws) / sizeof(text_draws[0])))
   {
      snprintf(text_draws[text_count].text, sizeof(text_draws[0].text), "%s", s);
      text_draws[text_count].x = x;
      text_draws[text_count].y = y;
      text_draws[text_count].w = w;
      text_draws[text_count].h = h;
      text_count++;
   }
}

static float record_style_text(int face, float x, float y, const char *s, uint32_t rgba)
{
   record_text(x, y, vs_text_w(face, s), (float)vs_line_h(face), s);
   return vs_text(face, x, y, s, rgba);
}

static void record_style_center(int face, float cx, float y, const char *s, uint32_t rgba)
{
   float w = vs_text_w(face, s);
   record_text(cx - w * 0.5f, y, w, (float)vs_line_h(face), s);
   vs_text_c(face, cx, y, s, rgba);
}

static void record_ui_text(int x, int y, const char *s, uint16_t color)
{
   record_text(x * 2, y * 2, vid_text_w(s) * 2, FE_FONT_H * 2, s);
   vid_text(x, y, s, color);
}

static void record_ui_center(int y, const char *s, uint16_t color)
{
   record_text(VID_SCR_W - vid_text_w(s), y * 2, vid_text_w(s) * 2, FE_FONT_H * 2, s);
   vid_text_center(y, s, color);
}

static void record_art_retry(void)
{
   art_retries++;
   artfetch_retry();
}

static int find_text(const char *s, int exact)
{
   int i;
   for (i = 0; i < text_count; i++)
      if (exact ? !strcmp(text_draws[i].text, s) : strstr(text_draws[i].text, s) != NULL)
         return i;
   return -1;
}

static SceUID record_directory_open(const char *path)
{
   directory_opens++;
   return pc_io_dopen(path);
}

static int record_directory_read(SceUID dir, SceIoDirent *entry)
{
   if (fail_directory_read_after == 0)
      return -1;
   if (fail_directory_read_after > 0)
      fail_directory_read_after--;
   return pc_io_dread(dir, entry);
}

unsigned record_artwork_list(const char *const *paths, const char *const *names, int n)
{
   int i;
   (void)names;
   handed++;
   handed_n = n;
   for (i = 0; i < n && i < 16; i++)
      snprintf(handed_paths[i], sizeof(handed_paths[i]), "%s", paths[i]);
   if (n > 0)
      snprintf(handed_last, sizeof(handed_last), "%s", paths[n - 1]);
   return (unsigned)handed;
}

static void check(int ok, const char *why)
{
   if (!ok) { fprintf(stderr, "FAIL: %s\n", why); exit(1); }
}

static void touch_file(const char *dir, const char *name)
{
   char path[1024];
   FILE *f;
   snprintf(path, sizeof(path), "%s/%s", dir, name);
   f = fopen(path, "wb");
   check(f != NULL, "create synthetic fixture file");
   fputc(0, f);
   fclose(f);
}

static void test_find_view(void)
{
   static const char *const names[] = {
      "First/Pokemon Emerald (USA).gba", "ux0:Other/Pokemon Emerald (Europe).gba",
      "A very long cartridge title extending far beyond the visible sixty four characters NeedleSuffix.gba",
      "Legend of Zelda, The (Europe).gba", "Mega MAN.gba", "MANAGER.gba", "Zero.gba"
   };
   static char bulk[BROWSER_MAX][32];
   unsigned opened = directory_opens;
   int i, cur, n = (int)(sizeof(names) / sizeof(names[0]));
   g_browser_favs = 0;
   g_find[0] = '\0';
   memset(g_roms, 0, sizeof(g_roms));
   for (i = 0; i < n; i++)
   {
      const char *base = strrchr(names[i], '/');
      g_roms[i].name = names[i];
      g_roms[i].base = base ? base + 1 : names[i];
   }
   g_roms[0].is_fav = g_roms[1].is_fav = g_roms[4].is_fav = 1;
   favs_build_view(n);
   cur = browser_find_set("  pOkEmOn \t", n, 1);
   check(!strcmp(g_find, "pOkEmOn") && view_count(n) == 2 &&
         view_rom(cur) == 1 && !strcmp(g_roms[view_rom(cur)].name, names[1]),
         "Find trims and ignores ASCII case while preserving duplicate titles' full-path identity");
   check(scope_count(n) == 7 && scope_rom(6) == 6,
         "a search never narrows the underlying console scope");
   cur = browser_find_set("the legend", n, 0);
   check(view_count(n) == 1 && view_rom(cur) == 3,
         "Find matches the displayed No-Intro article order");
   cur = browser_find_set("needlesuffix", n, 0);
   check(view_count(n) == 1 && view_rom(cur) == 2,
         "Find searches beyond the tile's truncated title");
   browser_find_set("man", n, 4);
   check(view_count(n) == 2 && view_rom(0) == 4 && view_rom(1) == 5,
         "a word inside other words still matches, in scan order");
   g_browser_favs = 1;
   browser_find_build(n);
   check(scope_count(n) == 3 && view_count(n) == 1 && view_rom(0) == 4,
         "Find intersects with Starred instead of searching the whole console");
   browser_find_set("pokemon", n, 1);
   check(view_count(n) == 2, "both same-title starred cartridges remain independent results");
   g_roms[1].is_fav = 0;
   favs_build_view(n);
   check(view_count(n) == 1 && view_rom(0) == 0,
         "unstar rebuilds the filtered result mapping immediately");
   cur = browser_find_set(" \t ", n, 4);
   check(!g_find[0] && view_count(n) == 2 && view_rom(cur) == 4,
         "accepted whitespace clears Find and keeps a still-visible cartridge");
   browser_find_set("no such cartridge", n, 0);
   check(view_count(n) == 0 && scope_count(n) == 2,
         "zero matches do not erase the console or Starred scope");
   g_browser_favs = 0;
   for (i = 0; i < BROWSER_MAX; i++)
   {
      snprintf(bulk[i], sizeof(bulk[i]), "Game %04d.gba", i);
      g_roms[i].name = g_roms[i].base = bulk[i];
      g_roms[i].is_fav = 0;
   }
   favs_build_view(BROWSER_MAX);
   cur = browser_find_set("game", BROWSER_MAX, BROWSER_MAX - 1);
   check(view_count(BROWSER_MAX) == BROWSER_MAX && cur == BROWSER_MAX - 1 &&
         view_rom(cur) == BROWSER_MAX - 1,
         "Find retains every entry and the last selection at maximum library capacity");
   check(directory_opens == opened, "Find and Starred rebuilds perform no directory I/O");
   g_find[0] = '\0';
   g_find_n = g_view_n = 0;
   puts("PASS: Find matching, full-path identity, Starred, clear, zero results and 2048-entry boundary");
}

/* vs_text_fit and vs_text_wrap now measure in one pass with cached
 * kerning: the same cut and the same lines as the measure-again versions
 * they replaced (copied here as the reference), for thousands of strings. */
static void ref_fit(int fi, char *s, float max_w)
{
   size_t n;
   if (!s || vs_text_w(fi, s) <= max_w)
      return;
   n = strlen(s);
   while (n > 0)
   {
      char keep;
      n--;
      while (n > 0 && s[n - 1] == ' ')
         n--;
      keep = s[n];
      s[n] = '\0';
      if (vs_text_w(fi, s) + vs_text_w(fi, "...") <= max_w)
      {
         strcat(s, "...");
         return;
      }
      s[n] = keep;
   }
   s[0] = '\0';
}

static int ref_wrap(int fi, float max_w, int max_lines, const char *s,
                    char out[][160])
{
   char line[160];
   int lines = 0;
   const char *p = s;
   while (p && *p && lines < max_lines)
   {
      size_t take = 0, best = 0, len = strlen(p);
      while (take < len && take < sizeof(line) - 4)
      {
         size_t next = take;
         while (next < len && p[next] != ' ')
            next++;
         if (next >= sizeof(line))
         {
            if (best > 0) break;
            best = next;
            break;
         }
         memcpy(line, p, next);
         line[next] = '\0';
         if (vs_text_w(fi, line) > max_w && best > 0)
            break;
         best = next;
         take = next;
         while (take < len && p[take] == ' ')
            take++;
         if (next >= len)
            break;
      }
      if (best == 0)
         best = len < sizeof(line) - 4 ? len : sizeof(line) - 4;
      if (best > sizeof(line) - 4)
         best = sizeof(line) - 4;
      memcpy(line, p, best);
      line[best] = '\0';
      if (lines == max_lines - 1 && best < len)
      {
         size_t rest = len < sizeof(line) - 4 ? len : sizeof(line) - 4;
         memcpy(line, p, rest);
         line[rest] = '\0';
         ref_fit(fi, line, max_w);
      }
      snprintf(out[lines], 160, "%s", line);
      lines++;
      p += best;
      while (*p == ' ')
         p++;
   }
   return lines;
}

static void test_text_fast(void)
{
   static const char *const words[] = {
      "a", "Pokemon", "AVA", "Tyrannosaurus", "of", "the", "Legend", "W.", "--",
      "WAVE", "Yo", "fi", "Te", "\xc3\xa9t\xc3\xa9", "x", "Supercalifragilistic",
      "LT", "Vo", "Ty", "(USA,", "Europe)", "1", "2", "\"Quote\"", "",
   };
   static char s[700], a[700], b[700];
   static char ref[3][160];
   uint32_t seed = 7;
   int iter, mismatches = 0, fi;
   vid_init();
   g_thm = &THM_DARK;
   for (iter = 0; iter < 4000; iter++)
   {
      int nw, i, lines, k;
      float w;
      size_t at = 0;
      seed = seed * 1103515245u + 12345u;
      fi = (int)((seed >> 8) % VS_FACES);
      nw = 1 + (int)((seed >> 12) % (iter % 50 == 0 ? 120 : 30));
      s[0] = '\0';
      for (i = 0; i < nw && at < sizeof(s) - 40; i++)
      {
         const char *wd;
         seed = seed * 1103515245u + 12345u;
         wd = words[(seed >> 9) % (sizeof(words) / sizeof(words[0]))];
         at += (size_t)snprintf(s + at, sizeof(s) - at, "%s%s",
                                i ? ((seed >> 20) % 7 == 0 ? "  " : " ") : "", wd);
      }
      if (iter % 97 == 0)                 /* a word longer than a line */
         for (k = 0; k < 200 && at < sizeof(s) - 1; k++)
            s[at++] = (char)('a' + k % 26), s[at] = '\0';
      w = 40.0f + (float)((seed >> 4) % 900);
      snprintf(a, sizeof(a), "%s", s);
      snprintf(b, sizeof(b), "%s", s);
      vs_text_fit(fi, a, w);
      ref_fit(fi, b, w);
      if (strcmp(a, b))
         mismatches++;
      lines = ref_wrap(fi, w, 3, s, ref);
      text_count = 0;
      if (vs_text_wrap(fi, 0, 0, w, 3, 2, s, 0xFFFFFFFFu) != lines)
         mismatches++;
      else
         for (k = 0; k < lines && k < text_count; k++)
            if (strcmp(text_draws[k].text, ref[k]))
               mismatches++;
      vid_swap();
   }
   vid_term();
   check(mismatches == 0, "one-pass text fit and wrap match the measure-again originals exactly");
   puts("PASS: text fit and wrap, one pass with cached kerning, same output");
}

/* Find by words: any order, case, accents, punctuation and apostrophes
 * aside, the extension left out. */
static void test_find_words(void)
{
   static const char *const names[] = {
      "Legend of Zelda, The - The Minish Cap (USA).gba",
      "Pok\xc3\xa9mon - Emerald Version (USA, Europe).gba",
      "Mario-Kart Super Circuit (Europe).gba",
      "Mario's Picross (Europe).gb",
      "Pokemon - Pinball (USA).gb",
      "Metroid - Zero Mission (USA).gba",
      "\xe3\x83\x9d\xe3\x82\xb1\xe3\x83\xa2\xe3\x83\xb3 (Japan).gba",
   };
   static const struct { const char *q; int n, first; const char *why; } cases[] = {
      { "zelda minish", 1, 0, "words in any position" },
      { "MINISH zelda", 1, 0, "words in any order and case" },
      { "pokemon emerald", 1, 1, "an accented title is found without the accent" },
      { "pok\xc3\xa9mon", 2, 1, "...and typed with it, both Pokemon games" },
      { "mario kart", 1, 2, "punctuation between words is a space" },
      { "marios", 1, 3, "an apostrophe is not a word break" },
      { "mario", 2, 2, "a word finds every title holding it" },
      { "gba", 0, 0, "the extension is not part of the title" },
      { "zero", 1, 5, "a word inside a title" },
      { "  emerald   pokemon ", 1, 1, "extra spaces do not matter" },
      { "\xe3\x82\xb1\xe3\x83\xa2", 1, 6, "Japanese text is matched as typed" },
      { "-", 5, 0, "punctuation alone is looked for as it is" },
      { "zelda metroid", 0, 0, "every word must be in the title" },
   };
   int i, n = (int)(sizeof(names) / sizeof(names[0]));
   g_browser_favs = 0;
   g_find[0] = '\0';
   memset(g_roms, 0, sizeof(g_roms));
   for (i = 0; i < n; i++)
      g_roms[i].name = g_roms[i].base = names[i];
   favs_build_view(n);
   for (i = 0; i < (int)(sizeof(cases) / sizeof(cases[0])); i++)
   {
      browser_find_set(cases[i].q, n, 0);
      if (view_count(n) != cases[i].n ||
          (cases[i].n && view_rom(0) != cases[i].first))
         fprintf(stderr, "Find \"%s\": %d results, first %d\n", cases[i].q,
                 view_count(n), view_count(n) ? view_rom(0) : -1);
      check(view_count(n) == cases[i].n &&
            (!cases[i].n || view_rom(0) == cases[i].first), cases[i].why);
   }
   g_find[0] = '\0';
   g_find_n = g_view_n = 0;
   puts("PASS: Find by words, any order, accents, punctuation and apostrophes");
}

static void test_find_layout(void)
{
   int query, count, progress, i;
   unsigned opened = directory_opens;
   vid_init();
   g_thm = &THM_DARK;
   g_browser_favs = 0;
   memset(g_find, 'W', sizeof(g_find) - 1);
   g_find[sizeof(g_find) - 1] = '\0';
   g_find_n = 7;
   art_busy = 1; art_seen = 900; art_total = 925; art_have = 700;
   text_count = 0;
   tch_reset();
   hm_topbar(0, 925);
   query = find_text("\"", 0);
   count = find_text("7 of 925", 1);
   progress = find_text("Checking covers: 900 of 925", 1);
   check(query >= 0 && count >= 0 && progress >= 0 &&
         text_draws[query].x + text_draws[query].w + 12 <= text_draws[count].x &&
         text_draws[count].x + text_draws[count].w <= 414 &&
         text_draws[progress].x >= 500,
         "a maximum-length query cannot hide result counts or overlap background-cover progress");
   g_find_n = 0;
   text_count = 0;
   home_empty("unread-search-root", 925);
   check(find_text("No games match", 1) >= 0,
         "zero matches visibly show their distinct no-results heading");
   check(find_text("Copy your", 0) < 0 && find_text("No Game Boy", 0) < 0 &&
         directory_opens == opened,
         "zero matches never scan folders or claim missing ROMs");
   check(find_text("Try fewer words", 0) >= 0 && find_text("All games", 0) < 0,
         "a search with no match anywhere suggests a shorter search");
   {
      /* Starred finds nothing, All games would: say so, offer it. */
      static const char *const names[] = { "Alpha (USA).gba", "Beta Quest (USA).gba",
                                           "Gamma Quest (Europe).gba" };
      int i, label;
      memset(g_roms, 0, sizeof(g_roms));
      for (i = 0; i < 3; i++)
         g_roms[i].name = g_roms[i].base = names[i];
      g_roms[0].is_fav = 1;
      g_browser_favs = 1;
      g_find[0] = '\0';
      favs_build_view(3);
      browser_find_set("quest", 3, 0);
      check(view_count(3) == 0 && browser_find_count_all(3) == 2,
            "two of the console's games match although no starred one does");
      text_count = 0;
      home_empty("unread-search-root", 3);
      label = find_text("2 games match in All games", 1);
      check(label >= 0 && find_text("All games", 1) >= 0 &&
            text_draws[label].x >= 0 && text_draws[label].x + text_draws[label].w <= VS_W,
            "the empty Starred search names the matches in All games and offers them");
      g_browser_favs = 0;
      g_view_n = 0;
   }
   for (i = 0; i < text_count; i++)
      if (strchr(text_draws[i].text, '"'))
         check(text_draws[i].x >= 0 && text_draws[i].x + text_draws[i].w <= VS_W,
               "every maximum-length query caption fits inside the screen");
   g_find[0] = '\0';
   art_busy = art_seen = art_total = art_have = 0;
   vid_swap();
   vid_term();
   puts("PASS: long Find captions, visible result counts, cover progress and no-results layout");
}

static void test_cover_progress(void)
{
   static const struct {
      int busy, seen, total, have;
      const char *home, *settings;
   } cases[] = {
      {1, 12, 40, 31, "Checking covers: 12 of 40", "Checking 12 of 40 - 31 have covers"},
      {0, 40, 40, 31, NULL, "31 of 40 games have covers"},
      {0, 40, 40, 40, NULL, "40 of 40 games have covers"},
      {1, 70, 40, 80, "Checking covers: 40 of 40", "Checking 40 of 40 - 40 have covers"},
      {1, -3, 40, -5, "Checking covers: 0 of 40", "Checking 0 of 40 - 0 have covers"},
      {1, 1, -1, 1, NULL, "Not checking"},
      {0, 0, 0, 0, NULL, "Not checking"},
      {0, 12, 40, 31, NULL, "31 of 40 games have covers"},
      {1, 1799, 1800, 1800, "Checking covers: 1799 of 1800",
                            "Checking 1799 of 1800 - 1800 have covers"},
   };
   int i, found, label;
   vid_init();
   g_thm = &THM_DARK;
   for (i = 0; i < (int)(sizeof(cases) / sizeof(cases[0])); i++)
   {
      art_busy = cases[i].busy; art_seen = cases[i].seen;
      art_total = cases[i].total; art_have = cases[i].have;
      text_count = 0;
      tch_reset();
      set_touch_reset();
      hm_topbar(0, cases[i].total);
      found = find_text("Checking covers:", 0);
      check(cases[i].home ? found >= 0 &&
            !strcmp(text_draws[found].text, cases[i].home) : found < 0,
            "Home shows examined-game progress only while busy, with safe counters");
      if (found >= 0)
         check(text_draws[found].x >= 200 && text_draws[found].x + text_draws[found].w <= 917 &&
               text_draws[found].y >= 60 && text_draws[found].y + text_draws[found].h <= 100,
               "cover status stays below navigation and above the cover row, including 1800 games");
      text_count = 0;
      g_ov_browse = 1;
      g_cursor = SET_COVERS;
      screen_settings(0);
      found = find_text(cases[i].settings, 1);
      check(found >= 0, "Settings distinguishes checked games from games with covers, without false completion");
      label = find_text("Covers", 1);
      check(label >= 0 && text_draws[label].x + text_draws[label].w + 16 <= text_draws[found].x &&
            text_draws[found].x + text_draws[found].w <= 881,
            "Settings cover counts fit without overlapping the row label");
      check(find_text("retry missing", 0) >= 0,
            "the focused Covers row visibly offers the existing retry action");
      found = find_text(set_help(SET_COVERS), 1);
      check(found >= 0 && text_draws[found].x + text_draws[found].w <= VS_W - HM_X,
            "the Covers helper line fits the Vita screen instead of clipping its retry text");
      check(find_text("complete", 0) < 0 && find_text("downloaded", 0) < 0 &&
            find_text("connect to Wi-Fi", 0) < 0,
            "idle and partial coverage never imply success or diagnose offline from busy=0");
      vid_swap();
   }
   g_library_refresh = 0;
   g_cursor = SET_COVERS;
   check(screen_settings(PSP_CTRL_CROSS) == UI_ACT_NONE && art_retries == 1 &&
         !g_library_refresh,
         "retry remains a nonblocking artwork action, not a library rescan or game launch");
   g_ov_browse = 0;
   g_cursor = 0;
   art_busy = art_seen = art_total = art_have = 0;
   vid_term();
   puts("PASS: background cover status, honest partial results, torn counters, layout and retry");
}

/* Image quality's own help line, and Video filter under Original saying
 * what is drawn rather than Auto's stored filter. */
static void test_quality_text(void)
{
   static const char *const helps[PCFG_QUALITY_MODES] = {
      "Follows the Video filter below", "evenly sized at any screen size",
      "twice the game's size", "three times its size"
   };
   int q, found;
   vid_init();
   g_thm = &THM_DARK;
   g_ov_browse = 1;
   g_pcfg.filter = VID_FILTER_XBRZ;
   for (q = 0; q < PCFG_QUALITY_MODES; q++)
   {
      g_pcfg.quality = q;
      g_cursor = SET_QUALITY;
      text_count = 0;
      screen_settings(0);
      found = find_text(helps[q], 0);
      check(found >= 0 && text_draws[found].x + text_draws[found].w <= VS_W - HM_X,
            "each Image quality explains itself in a help line that fits the screen");
      check(find_text(pcfg_quality_name(q), 1) >= 0, "the row shows the quality's name");
      check(find_text(q == PCFG_QUALITY_ORIGINAL ? "sharp bilinear" : "xBRZ smooth", 1) >= 0,
            "Video filter shows what is drawn: exact pixels under Original, the filter otherwise");
      vid_swap();
      g_cursor = SET_FILTER;
      text_count = 0;
      screen_settings(0);
      found = find_text("Set by Image quality", 0);
      check(q == PCFG_QUALITY_ORIGINAL
               ? found >= 0 && text_draws[found].x + text_draws[found].w <= VS_W - HM_X
               : found < 0 && find_text("Redraws curves", 0) >= 0,
            "the Video filter help says when Original sets the filter, and fits");
      vid_swap();
   }
   /* A Video filter press under Original steps on from what the row shows
    * (sharp bilinear), not from Auto's hidden stored filter. */
   g_pcfg.quality = PCFG_QUALITY_ORIGINAL;
   g_pcfg.filter = VID_FILTER_XBRZ;
   settings_adjust(SET_FILTER, 1);
   check(g_pcfg.quality == PCFG_QUALITY_AUTO &&
         g_pcfg.filter == (VID_FILTER_SHARP + 1) % VID_FILTER_MODES,
         "under Original a Video filter press steps on from the sharp bilinear it shows, into Auto");
   /* The in-game Video preset under Auto: what it picked stays when the
    * look is re-applied later (the Colours, Blend or Wide rows). */
   g_pcfg.quality = PCFG_QUALITY_AUTO;
   vid_set_quality(PCFG_QUALITY_AUTO);
   vid_set_mode(VID_SCALE_FIT, VID_FILTER_SHARP);
   check(strcmp(vid_cycle_preset(), "stretch") == 0 && vid_filter() == VID_FILTER_NEAREST,
         "the Auto preset steps from fit sharp to stretch, nearest");
   ui_host_look_changed();
   check(vid_filter() == VID_FILTER_NEAREST && vid_scale_mode() == VID_SCALE_STRETCH,
         "re-applying the look keeps the preset's filter instead of the one from before it");
   g_pcfg.quality = PCFG_QUALITY_ORIGINAL;
   g_pcfg.filter = VID_FILTER_NEAREST;
   g_ov_browse = 0;
   g_cursor = 0;
   vid_term();
   puts("PASS: Image quality help lines and the Video filter shown under Original");
}

static void test_quality(const char *conf)
{
   int q;
   char legacy[1200];
   FILE *file;
   for (q = 1; q <= 3; q++)
   {
      vid_rect_t rect;
      check(fe_ini_set_int(conf, "scale_gba", VID_SCALE_FIT) == 0, "write fitted screen size");
      check(fe_ini_set_int(conf, "filter_gba", VID_FILTER_XBRZ) == 0, "write enhancement filter");
      check(fe_ini_set_int(conf, "quality_gba", q) == 0, "write independent processing quality");
      pcfg_load(conf);
      pcfg_display_select(FE_CONSOLE_GBA);
      vid_set_mode(g_pcfg.scale, g_pcfg.filter);
      ui_host_look_changed();
      check(upscale_factor(0, UPSCALE_XBRZ, 240, 160, 816) == (q == 1 ? 0 : q),
            "processing quality chooses native, 2x or 3x independently from fitted width");
      check(vid_filter() == (q == 1 ? VID_FILTER_SHARP : VID_FILTER_XBRZ),
            "Original bypasses enhancement (even-sized sharp pixels at fit) while 2x and 3x retain it");
      check(upscale_factor(1, UPSCALE_SCALE, 160, 144, 604) == (q == 1 ? 0 : q),
            "GB/GBC and the second player use the selected intermediate factor too");
      vid_picture_rect(240, 160, &rect);
      check(rect.x == 72 && rect.y == 0 && rect.w == 816 && rect.h == 544,
            "every processing quality fills the Vita height at the GBA aspect ratio");
      vid_picture_rect(282, 160, &rect);
      check(rect.x == 1 && rect.y == 0 && rect.w == 958 && rect.h == 544,
            "wide content keeps the same full-height framing at every quality");
      check(pcfg_display_save() == 0, "save independent quality through the normal profile writer");
      check(fe_ini_get_int(conf, "quality_gba", -1) == q,
            "quality persists under its own per-console key");
   }
   settings_adjust(SET_QUALITY, -1);
   check(g_pcfg.quality == 2 && g_pcfg.scale == VID_SCALE_FIT,
         "the quality control changes processing without changing screen fit");
   check(upscale_factor(0, UPSCALE_XBRZ, 240, 160, 816) == 2,
         "changing quality updates the live processing factor immediately");
   settings_adjust(SET_QUALITY, -1);
   check(vid_filter() == VID_FILTER_SHARP &&
         upscale_factor(0, UPSCALE_XBRZ, 240, 160, 816) == 0,
         "returning to Original immediately disables enhancement");
   /* The in-game Video preset (TRIANGLE) under an explicit quality steps the
    * size alone: it never switches smoothing on over Original. */
   check(strcmp(vid_cycle_preset(), "stretch") == 0 &&
         vid_scale_mode() == VID_SCALE_STRETCH && vid_filter() == VID_FILTER_SHARP,
         "the video preset under Original changes size only, never smoothing");
   check(strcmp(vid_cycle_preset(), "3x") == 0 && vid_filter() == VID_FILTER_SHARP &&
         strcmp(vid_cycle_preset(), "fit") == 0 && vid_scale_mode() == VID_SCALE_FIT,
         "the Original preset cycle is 3x, fit, stretch and keeps its processing");
   settings_adjust(SET_QUALITY, -1);
   check(g_pcfg.quality == PCFG_QUALITY_AUTO &&
         vid_filter() == VID_FILTER_XBRZ &&
         upscale_factor(0, UPSCALE_XBRZ, 240, 160, 816) == 4,
         "returning to Auto restores the requested legacy filter and factor");
   settings_adjust(SET_QUALITY, 1);
   settings_adjust(SET_QUALITY, 1);
   settings_adjust(SET_FILTER, 1);
   check(g_pcfg.quality == PCFG_QUALITY_2X && vid_filter() == VID_FILTER_SCALE &&
         upscale_factor(0, UPSCALE_SCALE, 240, 160, 816) == 2,
         "changing the enhancement algorithm keeps the independent 2x factor");
   check(strcmp(vid_cycle_preset(), "stretch") == 0 &&
         vid_scale_mode() == VID_SCALE_STRETCH && vid_filter() == VID_FILTER_SCALE &&
         upscale_factor(0, UPSCALE_SCALE, 240, 160, 960) == 2,
         "the video preset under Enhanced 2x changes size and keeps the selected enhancement");
   check(strcmp(vid_cycle_preset(), "3x") == 0 && vid_filter() == VID_FILTER_SCALE &&
         strcmp(vid_cycle_preset(), "fit") == 0 && vid_scale_mode() == VID_SCALE_FIT &&
         vid_filter() == VID_FILTER_SCALE,
         "the Enhanced preset cycle never drops to unprocessed nearest pixels");
   pcfg_display_select(FE_CONSOLE_GB);
   g_pcfg.quality = PCFG_QUALITY_ORIGINAL;
   pcfg_display_commit();
   check(pcfg_display_save() == 0 && fe_ini_get_int(conf, "quality_gba", -1) == 3 &&
         fe_ini_get_int(conf, "quality_gb", -1) == 1,
         "saving GB quality does not overwrite the GBA profile");
   check(fe_ini_set_int(conf, "quality_gba", 999) == 0, "write invalid quality fixture");
   pcfg_load(conf);
   check(g_pdisp[FE_CONSOLE_GBA].quality == PCFG_QUALITY_AUTO,
         "invalid quality falls back to the compatible auto path");
   snprintf(legacy, sizeof(legacy), "%s.legacy", conf);
   file = fopen(legacy, "wb");
   check(file != NULL, "create isolated legacy display config");
   fputs("scale = 4\nfilter = 4\n", file);
   fclose(file);
   pcfg_load(legacy);
   vid_set_mode(g_pcfg.scale, g_pcfg.filter);
   ui_host_look_changed();
   check(g_pcfg.scale == VID_SCALE_INT3 && g_pcfg.quality == PCFG_QUALITY_AUTO &&
         upscale_factor(0, UPSCALE_XBRZ, 240, 160, 816) == 4,
         "existing scale/filter keys retain integer zoom and automatic enhancement");
   file = fopen(legacy, "wb");
   check(file != NULL, "create fresh-config fixture");
   fclose(file);
   pcfg_load(legacy);
   check(g_pcfg.scale == VID_SCALE_FIT && g_pcfg.quality == PCFG_QUALITY_ORIGINAL,
         "a fresh installation defaults to full-height Original without migrating explicit preferences");
   g_pcfg.scale = VID_SCALE_INT3;
   check(pcfg_display_save() == 0, "save only the fresh GBA profile through the preset path");
   pcfg_load(legacy);
   check(g_pdisp[FE_CONSOLE_GB].quality == PCFG_QUALITY_ORIGINAL &&
         g_pdisp[FE_CONSOLE_GBC].quality == PCFG_QUALITY_ORIGINAL,
         "a sparse first save must not reclassify other fresh profiles as legacy Auto");
   check(g_pdisp[FE_CONSOLE_GB].scale == VID_SCALE_FIT &&
         g_pdisp[FE_CONSOLE_GBC].scale == VID_SCALE_FIT &&
         g_pdisp[FE_CONSOLE_GB].filter == VID_FILTER_NEAREST &&
         g_pdisp[FE_CONSOLE_GBC].filter == VID_FILTER_NEAREST,
         "legacy mirrors from the sparse GBA save do not change other consoles' size or filter");
   check(fe_ini_set_int(legacy, "quality_gb", PCFG_QUALITY_3X) == 0 &&
         fe_ini_set_int(legacy, "scale_gb", VID_SCALE_INT2) == 0 &&
         fe_ini_set_int(legacy, "filter_gb", VID_FILTER_SCALE) == 0,
         "simulate another writer updating the inactive GB profile");
   check(pcfg_display_save() == 0 &&
         fe_ini_get_int(legacy, "quality_gb", -1) == PCFG_QUALITY_3X &&
         fe_ini_get_int(legacy, "scale_gb", -1) == VID_SCALE_INT2 &&
         fe_ini_get_int(legacy, "filter_gb", -1) == VID_FILTER_SCALE,
         "sparse-profile seeding never overwrites existing inactive profile keys");
}

int main(void)
{
   char root[] = "/tmp/vitadhoc-library-XXXXXX", roms[1024], other[1024];
   char from[1024], to[1024], conf[1024];
   int n;
   check(mkdtemp(root) != NULL, "create isolated library fixture");
   pspcompat_set_root(root);
   pspcompat_make_dirs();
   snprintf(roms, sizeof(roms), "%s/roms", root);
   snprintf(conf, sizeof(conf), "%s/config.ini", root);
   pcfg_load(conf);
   g_pcfg.console = FE_CONSOLE_GBA;
   test_cover_progress();
   test_text_fast();
   test_find_view();
   test_find_words();
   test_find_layout();
   touch_file(roms, "Zulu.gba");
   touch_file(roms, "Middle.GBA");
   touch_file(roms, "Alpha.gba");
   touch_file(roms, "Color.GBC");
   touch_file(roms, "Mono.gb");
   touch_file(roms, ".hidden.gba");
   touch_file(roms, "._AppleDouble.GBA");
   touch_file(roms, "Notes.txt");
   n = rom_scan(roms, FE_CONSOLE_GBA);
   check(n == 3, "case-insensitive GBA filtering without hidden files");
   check(!strcmp(g_roms[0].base, "Alpha.gba") &&
         !strcmp(g_roms[2].base, "Zulu.gba"), "case-insensitive alphabetical order");
   favs_mark(n);
   hm_art_hand_over(roms, n);
   check(handed == 1 && strstr(handed_paths[1], "/Middle.GBA"),
         "first artwork handover contains the complete real library paths");
   hm_art_hand_over(roms, n);
   check(handed == 1, "unchanged frames do not requeue the same list");

   g_roms[1].is_fav = 1;
   favs_build_view(n);
   hm_art_hand_over(roms, n);
   check(handed == 1, "tagging a favourite in the full view does not restart artwork");
   g_browser_favs = 1;
   hm_art_hand_over(roms, n);
   check(handed == 1 && handed_n == 3, "entering favourites keeps the complete background artwork queue");
   g_roms[2].is_fav = 1;
   favs_build_view(n);
   hm_art_hand_over(roms, n);
   check(handed == 1 && handed_n == 3, "changing favourites never restarts background artwork coverage");
   g_browser_favs = 0;
   hm_art_hand_over(roms, n);
   check(handed == 1 && handed_n == 3, "leaving favourites retains the full artwork list");
   g_roms[2].is_fav = 0;
   favs_build_view(n);
   hm_art_hand_over(roms, n);
   check(handed == 1, "untagging a favourite in the full view does not restart artwork");

   g_art[0].rom_idx = 2;
   g_art[0].state = -1;
   g_hm_tile[0].rom = 2;
   g_hm_tile[0].state = -1;
   g_hero_idx = 2;
   g_hero_state = -1;
   g_hm_pic.rom = 2;
   /* The hidden game's tile ("none") and still are current before the
    * arrival: neither is due. */
   g_hm_tile[0].epoch = hm_epoch(2);
   g_hm_tile[0].art_epoch = hm_art_epoch(2);
   g_hm_tile[0].retry_us = 0;
   g_hm_still_for = 2;
   g_hm_pic.epoch = hm_epoch(2);
   g_hm_pic.art_epoch = hm_art_epoch(2);
   g_hm_pic.retry_us = 0;
   {
      unsigned hidden_art = hm_art_epoch(2), hidden_state = hm_epoch(2);
      uint64_t now = plat_time_us();
      check(!hm_image_due(hm_tile_find(2), 2, now) && !hm_still_due(2, now),
            "the hidden game's cached empty tile and still start current");
      g_browser_favs = 1;
      check(view_count(n) == 1, "the cached empty non-favourite is outside the Starred view");
      hm_art_arrived(NULL, view_count(n));
      g_browser_favs = 0;
      /* Asynchronous contract: arrival marks the raw key dirty and keeps
       * what is shown; it never touches the save key. */
      check(hm_art_pending(2) && g_art[0].state == 0 && g_hero_state == 0 &&
            hm_tile_find(2)->state == -1 && g_hm_pic.rom == 2 &&
            hm_epoch(2) == hidden_state,
            "a coalesced arrival marks hidden artwork dirty, keeps the shown picture and the save key");
      hm_art_apply();
      now = plat_time_us();
      check(!hm_art_pending(2) && hm_art_epoch(2) != hidden_art &&
            hm_image_due(hm_tile_find(2), 2, now) && hm_still_due(2, now),
            "returning from Starred reloads cached empty artwork outside the overflow's current view");
   }
   home_free();

   snprintf(from, sizeof(from), "%s/Middle.GBA", roms);
   snprintf(to, sizeof(to), "%s/New Middle.GBA", roms);
   check(rename(from, to) == 0, "rename a non-first file without changing count");
   n = rom_scan(roms, FE_CONSOLE_GBA);
   favs_mark(n);
   hm_art_hand_over(roms, n);
   check(handed == 2 && strstr(handed_paths[1], "/New Middle.GBA"),
         "same-size refresh must update queued artwork after a middle-file rename");

   snprintf(other, sizeof(other), "%s/other", root);
   check(mkdir(other, 0777) == 0, "create replacement library root");
   touch_file(other, "Alpha.gba");
   touch_file(other, "New Middle.GBA");
   touch_file(other, "Zulu.gba");
   n = rom_scan(other, FE_CONSOLE_GBA);
   favs_mark(n);
   hm_art_hand_over(other, n);
   check(handed == 3 && strstr(handed_paths[0], "/other/Alpha.gba"),
         "same names in a different root must refresh artwork paths");
   check(rom_scan(roms, FE_CONSOLE_GBC) == 1 &&
         !strcmp(g_roms[0].base, "Color.GBC"), "GBC filter stays independent");
   check(rom_scan(roms, FE_CONSOLE_GB) == 1 &&
         !strcmp(g_roms[0].base, "Mono.gb"), "GB filter stays independent");
   {
      int count = 0, visible = 0, cur = 0, idle = 0;
      unsigned opened;
      g_sw.to = FE_CONSOLE_GBA;
      browser_switch_apply(roms, &count, &visible, &cur, &idle);
      opened = directory_opens;
      g_sw.to = FE_CONSOLE_GB;
      browser_switch_apply(roms, &count, &visible, &cur, &idle);
      check(count == 1 && !strcmp(g_roms[0].base, "Mono.gb"),
            "cached GB tab restores its own names, not pointers into the GBA pool");
      g_sw.to = FE_CONSOLE_GBA;
      browser_switch_apply(roms, &count, &visible, &cur, &idle);
      check(count == 3 && !strcmp(g_roms[1].base, "New Middle.GBA"),
            "cached GBA tab restores the renamed full library");
      check(directory_opens == opened, "revisiting scanned console tabs does not reopen directories");
      g_roms[0].has_sav = 1;
      g_sw.to = FE_CONSOLE_GB;
      browser_switch_apply(roms, &count, &visible, &cur, &idle);
      g_sw.to = FE_CONSOLE_GBA;
      browser_switch_apply(roms, &count, &visible, &cur, &idle);
      check(g_roms[0].has_sav == -1, "restoring a cached list does not reuse stale save status");
      touch_file(roms, "Fresh.gba");
      opened = directory_opens;
      check(rom_scan_cached(roms, FE_CONSOLE_GBA) == 3 && directory_opens == opened,
            "cached tabs remain stable until an explicit refresh");
      rom_cache_clear();
      check(rom_scan_cached(roms, FE_CONSOLE_GBA) == 4 && directory_opens > opened,
            "manual invalidation finds games added while the browser was open");
      opened = directory_opens;
      check(rom_scan_cached(other, FE_CONSOLE_GBA) == 3 && directory_opens > opened &&
            strstr(g_rom_root, "/other"), "changing the primary root never reuses another root's list");
   }
   {
      char transient[1024], offline[1024];
      int count;
      snprintf(transient, sizeof(transient), "%s/transient", root);
      snprintf(offline, sizeof(offline), "%s/offline", root);
      check(mkdir(transient, 0777) == 0, "create temporarily unavailable library");
      touch_file(transient, "Recovered.gba");
      check(rename(transient, offline) == 0, "take the library offline before its first scan");
      count = rom_scan_cached(transient, FE_CONSOLE_GBA);
      check(count == 0, "an unavailable folder has no readable entries yet");
      check(rename(offline, transient) == 0, "make the same folder readable again");
      check(rom_scan_cached(transient, FE_CONSOLE_GBA) == 1 &&
            !strcmp(g_roms[0].base, "Recovered.gba"),
            "a failed directory scan must recover on revisit without manual invalidation");
      rom_cache_clear();
      fail_directory_read_after = 1;
      (void)rom_scan_cached(transient, FE_CONSOLE_GBA);
      check(g_rom_scan_io_errors > 0, "a partial directory-read failure is recorded");
      fail_directory_read_after = -1;
      check(rom_scan_cached(transient, FE_CONSOLE_GBA) == 1 &&
            !strcmp(g_roms[0].base, "Recovered.gba"),
            "a partial directory-read failure is not retained as a complete cached list");
   }
   {
      char bulk[1024], name[64];
      int i;
      snprintf(bulk, sizeof(bulk), "%s/bulk", root);
      check(mkdir(bulk, 0777) == 0, "create large isolated library");
      for (i = 1799; i >= 0; i--)
      {
         snprintf(name, sizeof(name), "Game %04d.%s", i, i % 2 ? "GBA" : "gba");
         touch_file(bulk, name);
      }
      n = rom_scan(bulk, FE_CONSOLE_GBA);
      check(n == 1800, "large library is complete, not truncated");
      for (i = 0; i < n; i++)
      {
         snprintf(name, sizeof(name), "Game %04d.%s", i, i % 2 ? "GBA" : "gba");
         check(!strcmp(g_roms[i].base, name), "large library is fully sorted");
      }
      check(rom_scan_cached(bulk, FE_CONSOLE_GB) == 0, "cache an empty console independently");
      {
         unsigned opened = directory_opens;
         check(rom_scan_cached(bulk, FE_CONSOLE_GBA) == 1800 && directory_opens == opened,
               "the entire large library is restored without another directory scan");
         for (i = 0; i < n; i++)
         {
            snprintf(name, sizeof(name), "Game %04d.%s", i, i % 2 ? "GBA" : "gba");
            check(!strcmp(g_roms[i].base, name), "every cached large-library name survives rebasing");
         }
      }
      favs_mark(n);
      hm_art_hand_over(bulk, n);
      check(handed_n == 1800 && strstr(handed_paths[0], "/bulk/Game 0000.gba") &&
            strstr(handed_last, "/bulk/Game 1799.GBA"),
            "large artwork handover includes the first and last full path");
      puts("PASS: 1800-game library scan, full sort and artwork handover");
   }
   test_quality_text();
   test_quality(conf);
   puts("PASS: real scanner, sort, filtering, unchanged frames and same-size refresh");
   return 0;
}
