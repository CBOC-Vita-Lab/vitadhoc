/* boot_guard.c -- see boot_guard.h. */
#include "boot_guard.h"
#include "fe_evt.h"
#include "fe_util.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

#define BG_PATH 300

static char     g_dir[BG_PATH];       /* <root>/guard, "" = not initialised   */
static fe_ini_doc g_doc;              /* one at a time (main thread)           */

static int      g_active;             /* game.mark is on the card              */
static uint64_t g_t0;                 /* clock at the first tick of the game   */
static char     g_key[16];            /* the running game's id                 */
static int      g_safe;               /* the running game is on safe settings  */
static int      g_app_marked;         /* app.mark is on the card               */
static int      g_notice;             /* a notice is waiting                   */
static char     g_notice_name[96];
static int      g_app_revert;
static int      g_core4_off;          /* a safe game had the fourth core on    */

/* ---- paths and files ---- */

static int path_of(char *out, size_t cap, const char *name)
{
   int n;
   if (!g_dir[0])
      return -1;
   n = snprintf(out, cap, "%s/%s", g_dir, name);
   return n > 0 && (size_t)n < cap ? 0 : -1;
}

static int has_file(const char *path)
{
   struct stat st;
   return stat(path, &st) == 0;
}

static void drop(const char *name)
{
   char p[BG_PATH];
   if (path_of(p, sizeof(p), name) == 0)
      remove(p);
}

static const char *base_of(const char *path)
{
   const char *s = path ? strrchr(path, '/') : NULL;
   const char *c = path ? strrchr(path, ':') : NULL;
   if (c && (!s || c > s))
      s = c;
   return s ? s + 1 : (path ? path : "");
}

/* the game's id: eight hex digits of the CRC-32 of its file name */
static void key_of(const char *rom_path, char *out, size_t cap)
{
   const char *b = base_of(rom_path);
   snprintf(out, cap, "%08x", (unsigned)fe_crc32(0, b, strlen(b)));
}

/* the name the notice uses: the file name without its extension */
static void name_of(const char *rom_path, char *out, size_t cap)
{
   const char *b = base_of(rom_path);
   const char *dot = strrchr(b, '.');
   size_t n = dot && dot != b ? (size_t)(dot - b) : strlen(b);
   if (n >= cap)
      n = cap - 1;
   memcpy(out, b, n);
   out[n] = '\0';
   {
      char *c;
      for (c = out; *c; c++)
         if (*c == '\n' || *c == '\r' || *c == '"')
            *c = ' ';
   }
}

/* write `file` with the given keys (fe_ini_doc_save: temp file, read back,
 * rename).  0 = written. */
static int put(const char *name, const char *k1, const char *v1,
               const char *k2, const char *v2, const char *k3, const char *v3,
               const char *k4, const char *v4)
{
   char p[BG_PATH];
   if (path_of(p, sizeof(p), name) != 0)
      return -1;
   memset(&g_doc, 0, sizeof(g_doc));
   g_doc.ok = 1;
   if ((k1 && fe_ini_doc_set(&g_doc, k1, v1) != 0) ||
       (k2 && fe_ini_doc_set(&g_doc, k2, v2) != 0) ||
       (k3 && fe_ini_doc_set(&g_doc, k3, v3) != 0) ||
       (k4 && fe_ini_doc_set(&g_doc, k4, v4) != 0))
      return -1;
   g_doc.changed = 1;
   return fe_ini_doc_save(&g_doc, p);
}

static int load(const char *name)
{
   char p[BG_PATH];
   if (path_of(p, sizeof(p), name) != 0)
      return 0;
   if (fe_ini_doc_load(&g_doc, p) != 0 || !g_doc.ok || !g_doc.existed)
      return 0;
   return 1;
}

static void getv(const char *k, char *out, size_t cap)
{
   if (!fe_ini_doc_get(&g_doc, k, out, cap))
      out[0] = '\0';
}

static void unum(char *out, size_t cap, unsigned v)
{
   snprintf(out, cap, "%u", v);
}

static void index_note(void)
{
   /* safe.idx: core4=1 while any safe game had the fourth core on.  Written
    * when a safe file is made, cleared by release when no game needs it. */
   put("safe.idx", "core4", g_core4_off ? "1" : "0", NULL, NULL, NULL, NULL, NULL, NULL);
}

/* ---- the streak ---- */

static void streak_reset(void)
{
   drop("state.ini");
}

/* One more unfinished start of `key`.  Two in a row: safe settings. */
static void unfinished(const char *what, const char *key, const char *name,
                       unsigned risky)
{
   char last[16] = "", cnt[16];
   int count = 0;
   if (load("state.ini"))
   {
      getv("last", last, sizeof(last));
      count = (int)fe_ini_doc_get_int(&g_doc, "count", 0);
   }
   count = (last[0] && strcmp(last, key) == 0) ? count + 1 : 1;
   if (count < BG_STREAK_LIMIT)
   {
      unum(cnt, sizeof(cnt), (unsigned)count);
      put("state.ini", "last", key, "count", cnt, "name", name, NULL, NULL);
      fe_evt("boot_guard unfinished what=%s key=%s count=%d action=none",
             what, key, count);
      return;
   }
   streak_reset();
   if (!strcmp(key, "app"))
   {
      g_app_revert = 1;
      g_notice = 1;
      g_notice_name[0] = '\0';
      fe_evt("boot_guard unfinished what=%s key=app count=%d action=revert_global",
             what, count);
      return;
   }
   {
      char rk[16], c4[4];
      unum(rk, sizeof(rk), risky);
      c4[0] = (risky & BG_RISK_CORE4) ? '1' : '0';
      c4[1] = '\0';
      {
         char fn[40];
         snprintf(fn, sizeof(fn), "safe-%s.ini", key);
         put(fn, "name", name, "risky", rk, "core4", c4, "look", "0");
      }
   }
   if (risky & BG_RISK_CORE4)
   {
      g_core4_off = 1;
      index_note();
   }
   snprintf(g_notice_name, sizeof(g_notice_name), "%s", name);
   g_notice = 1;
   fe_evt("boot_guard unfinished what=%s key=%s count=%d action=safe game=\"%s\" risky=%u",
          what, key, count, name, risky);
}

static void settle_game_mark(void)
{
   char key[16], name[96], r[16];
   if (!load("game.mark"))
      return;
   getv("key", key, sizeof(key));
   getv("name", name, sizeof(name));
   getv("risky", r, sizeof(r));
   drop("game.mark");
   if (key[0])
      unfinished("game", key, name, (unsigned)fe_ini_doc_get_int(&g_doc, "risky", 0));
}

/* every safe file: the fourth core stays off while one has it on */
static int any_safe_core4(void)
{
   /* the list is short; find them through state we keep: one index file */
   if (!load("safe.idx"))
      return 0;
   return fe_ini_doc_get_int(&g_doc, "core4", 0) != 0;
}

/* ---- the API ---- */

void boot_guard_reset_for_tests(void)
{
   g_dir[0] = 0;
   g_active = g_safe = g_app_marked = g_notice = g_app_revert = g_core4_off = 0;
   g_t0 = 0;
   g_key[0] = 0;
   g_notice_name[0] = 0;
}

void boot_guard_init(const char *root)
{
   char app_key[] = "app";
   int n;
   boot_guard_reset_for_tests();
   if (!root || !root[0])
      return;
   n = snprintf(g_dir, sizeof(g_dir), "%s/guard", root);
   if (n <= 0 || (size_t)n >= sizeof(g_dir))
   {
      g_dir[0] = 0;
      return;
   }
   mkdir(g_dir, 0777);
   g_core4_off = any_safe_core4();
   settle_game_mark();
   {
      char p[BG_PATH];
      if (path_of(p, sizeof(p), "app.mark") == 0 && has_file(p))
      {
         drop("app.mark");
         unfinished("app", app_key, "", 0);
      }
   }
   if (put("app.mark", "t", "1", NULL, NULL, NULL, NULL, NULL, NULL) == 0)
      g_app_marked = 1;
   fe_evt("boot_guard init core4_off=%d notice=%d app_revert=%d", g_core4_off,
          g_notice, g_app_revert);
}

void boot_guard_app_ready(void)
{
   if (g_app_marked)
   {
      drop("app.mark");
      g_app_marked = 0;
      /* the app got to the list: a streak of app starts is over */
      {
         char last[16] = "";
         if (load("state.ini"))
            getv("last", last, sizeof(last));
         if (!strcmp(last, "app"))
            streak_reset();
      }
   }
}

int boot_guard_game_begin(const char *rom_path, unsigned risky)
{
   char key[16], name[96], rk[16], fn[40], t[24];
   boot_guard_app_ready();
   g_active = 0;
   g_safe = 0;
   g_t0 = 0;
   if (!g_dir[0] || !rom_path)
      return 0;
   key_of(rom_path, key, sizeof(key));
   name_of(rom_path, name, sizeof(name));
   snprintf(g_key, sizeof(g_key), "%s", key);
   snprintf(fn, sizeof(fn), "safe-%s.ini", key);
   {
      char p[BG_PATH];
      if (path_of(p, sizeof(p), fn) == 0 && has_file(p))
      {
         g_safe = 1;
         fe_evt("boot_guard game_begin game=\"%s\" safe=1 marker=0", name);
         return 1;
      }
   }
   if (!risky)
   {
      fe_evt("boot_guard game_begin game=\"%s\" safe=0 marker=0 risky=0", name);
      return 0;
   }
   unum(rk, sizeof(rk), risky);
   unum(t, sizeof(t), (unsigned)time(NULL));
   if (put("game.mark", "key", key, "name", name, "risky", rk, "t", t) == 0)
      g_active = 1;
   fe_evt("boot_guard game_begin game=\"%s\" safe=0 marker=%d risky=%u", name,
          g_active, risky);
   return 0;
}

void boot_guard_tick(uint64_t now_us)
{
   if (!g_active)
      return;
   if (!g_t0)
   {
      g_t0 = now_us ? now_us : 1;
      return;
   }
   if (now_us - g_t0 >= (uint64_t)BG_STABLE_SECONDS * 1000000u)
   {
      drop("game.mark");
      g_active = 0;
      streak_reset();
      fe_evt("boot_guard stable seconds=%d", BG_STABLE_SECONDS);
   }
}

void boot_guard_game_end(void)
{
   if (g_active)
   {
      drop("game.mark");
      g_active = 0;
      streak_reset();
      fe_evt("boot_guard clean_end");
   }
   g_safe = 0;
   g_key[0] = 0;
}

int boot_guard_is_safe_game(const char *rom_path)
{
   char key[16], fn[40], p[BG_PATH];
   if (!g_dir[0] || !rom_path)
      return 0;
   key_of(rom_path, key, sizeof(key));
   snprintf(fn, sizeof(fn), "safe-%s.ini", key);
   return path_of(p, sizeof(p), fn) == 0 && has_file(p);
}

int boot_guard_safe(void)
{
   return g_safe;
}

int boot_guard_core4_off(void)
{
   return g_core4_off;
}

void boot_guard_release(void)
{
   char fn[40];
   if (!g_safe || !g_key[0])
      return;
   snprintf(fn, sizeof(fn), "safe-%s.ini", g_key);
   if (load(fn) && fe_ini_doc_get_int(&g_doc, "core4", 0))
   {
      g_core4_off = 0;
      index_note();
   }
   drop(fn);
   g_safe = 0;
   fe_evt("boot_guard release key=%s", g_key);
}

int boot_guard_take_look(void)
{
   char fn[40], name[96], rk[16], c4[4];
   if (!g_safe || !g_key[0])
      return 0;
   snprintf(fn, sizeof(fn), "safe-%s.ini", g_key);
   if (!load(fn) || fe_ini_doc_get_int(&g_doc, "look", 0))
      return 0;
   getv("name", name, sizeof(name));
   getv("risky", rk, sizeof(rk));
   getv("core4", c4, sizeof(c4));
   put(fn, "name", name, "risky", rk, "core4", c4, "look", "1");
   fe_evt("boot_guard look_applied key=%s", g_key);
   return 1;
}

void boot_guard_player_begin(const char *rom_path, unsigned risky)
{
   char key[16], name[96], rk[16], t[24];
   if (!g_dir[0] || !rom_path)
      return;
   boot_guard_app_ready();
   key_of(rom_path, key, sizeof(key));
   name_of(rom_path, name, sizeof(name));
   drop("player.mark");
   if (!risky)
      return;
   unum(rk, sizeof(rk), risky);
   unum(t, sizeof(t), (unsigned)time(NULL));
   put("player.mark", "key", key, "name", name, "risky", rk, "t", t);
   fe_evt("boot_guard player_begin game=\"%s\" risky=%u", name, risky);
}

void boot_guard_player_end(int had_report, long elapsed_s)
{
   char key[16], name[96];
   if (!load("player.mark"))
      return;
   getv("key", key, sizeof(key));
   getv("name", name, sizeof(name));
   {
      unsigned risky = (unsigned)fe_ini_doc_get_int(&g_doc, "risky", 0);
      drop("player.mark");
      if (had_report)
      {
         streak_reset();
         fe_evt("boot_guard player_end report=1");
         return;
      }
      if (elapsed_s < 0 || elapsed_s > BG_PLAYER_WINDOW_S)
      {
         fe_evt("boot_guard player_end report=0 elapsed=%ld counted=0", elapsed_s);
         return;
      }
      unfinished("player", key, name, risky);
   }
}

int boot_guard_take_notice(char *title, size_t tc, char *l1, size_t c1,
                           char *l2, size_t c2, char *full, size_t cf)
{
   if (!g_notice)
      return 0;
   g_notice = 0;
   if (g_notice_name[0])
   {
      snprintf(title, tc, "VitAdhoc closed unexpectedly twice");
      snprintf(l1, c1, "with %s; it now starts with safe picture settings.",
               g_notice_name);
      snprintf(l2, c2, "Settings > This game to change them.");
      if (full)
         snprintf(full, cf, "VitAdhoc closed unexpectedly twice with %s; it now "
                  "starts with safe picture settings. Settings > This game to "
                  "change them.", g_notice_name);
   }
   else
   {
      snprintf(title, tc, "VitAdhoc closed unexpectedly twice");
      snprintf(l1, c1, "before the game list; it now starts with safe picture settings.");
      snprintf(l2, c2, "Settings > Display and Gameplay to change them.");
      if (full)
         snprintf(full, cf, "VitAdhoc closed unexpectedly twice before the game "
                  "list; it now starts with safe picture settings. Settings > "
                  "Display and Gameplay to change them.");
   }
   fe_evt("boot_guard notice text=\"%s\"", l1);
   return 1;
}

int boot_guard_take_app_revert(void)
{
   const int r = g_app_revert;
   g_app_revert = 0;
   return r;
}
