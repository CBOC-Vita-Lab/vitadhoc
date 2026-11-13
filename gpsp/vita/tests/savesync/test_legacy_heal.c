/* save_sync.c, 1.4: every legacy save layout (what 1.0, 1.1, 1.2, 1.3 and 1.3.1
 * leave on a card) x {GBA, GB, GBC} x the conditions a card can be in, run
 * through the launch path the app uses (save_sync_game_start, then
 * save_sync_heal_game).  The point: a save the app has, with no record of a
 * RetroArch file, gets a plain RetroArch-readable file where RetroArch looks,
 * without ever overwriting a different or newer file of RetroArch's, without
 * ever touching the app's own save, and without leaving half files behind.
 *
 *   gcc -Wall -Wextra -Werror -I vita -I frontend-common \
 *       vita/tests/savesync/test_legacy_heal.c vita/save_sync.c \
 *       frontend-common/fe_util.c -o /tmp/test_legacy_heal
 *
 * Built with -DOLD_CODE against the 1.3.1 / early-1.4 save_sync.c (which has no
 * heal), the launch() below is the old launch path and the checks fail: that
 * is the RED run. */
#undef NDEBUG
#pragma GCC diagnostic ignored "-Wformat-truncation"   /* test paths are short */
#include <assert.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <utime.h>
#include "save_sync.h"
#include "fe_util.h"

static char evt[16384];
void fe_evt(const char *f, ...)
{
   va_list ap;
   size_t n = strlen(evt);
   if (n > sizeof(evt) - 1024)
      n = evt[0] = 0;
   va_start(ap, f);
   vsnprintf(evt + n, sizeof(evt) - n, f, ap);
   va_end(ap);
   strncat(evt, "\n", sizeof(evt) - strlen(evt) - 1);
}

static int n_checks, n_fail;
static const char *g_label = "";
#define CK(c) do { n_checks++; if (!(c)) { n_fail++; \
   printf("FAIL [%s] line %d: %s\n", g_label, __LINE__, #c); } } while (0)

static char root[256], rom[700], save[700], ra[700], ra_dir[700], rom_dir[700], bdir[700];
static int g_kind;

static void sh(const char *fmt, const char *a)
{ char c[1200]; snprintf(c, sizeof c, fmt, a); if (system(c) != 0) { puts("sh failed"); exit(2); } }
static void put(const char *p, const void *d, size_t n)
{ FILE *f = fopen(p, "wb"); assert(f && fwrite(d, 1, n, f) == n); assert(fclose(f) == 0); }
static long sizeof_file(const char *p)
{ struct stat st; return stat(p, &st) == 0 ? (long)st.st_size : -1; }
static int has(const char *p) { return sizeof_file(p) >= 0; }
static int same(const char *p, const unsigned char *d, size_t n)
{
   static unsigned char b[140000];
   FILE *f = fopen(p, "rb");
   size_t got;
   if (!f) return 0;
   got = fread(b, 1, sizeof b, f);
   fclose(f);
   return got == n && !memcmp(b, d, n);
}
static void stamp(const char *p, long t)
{ struct utimbuf u; u.actime = u.modtime = t; assert(utime(p, &u) == 0); }
static int n_files(const char *dir)
{
   char c[700]; int n = 0; FILE *p;
   snprintf(c, sizeof c, "ls %s 2>/dev/null | wc -l", dir);
   p = popen(c, "r"); if (p) { if (fscanf(p, "%d", &n) != 1) n = 0; pclose(p); }
   return n;
}
static int has_backup(const char *orig, const unsigned char *d, size_t n)
{
   char p[700];
   const char *bn = strrchr(orig, '/') + 1;
   snprintf(p, sizeof p, "%s/%s.%08x.bak", bdir, bn, (unsigned)fe_crc32(0, d, n));
   return same(p, d, n);
}

/* ---- what a console's save looks like (synthetic bytes) ---- */
typedef struct { const char *name; int kind; const char *rom_ext, *save_ext; size_t len, ram; } kind_t;
static const kind_t kinds[] = {
   { "GBA",  SS_KIND_GBA, ".gba", ".sav",     131072, 131072 },
   { "GB",   SS_KIND_GB,  ".gb",  ".gb.sav",  8192 + 0, 8192 },
   { "GBC",  SS_KIND_GBC, ".gbc", ".gbc.sav", 32768 + 48, 32768 },   /* a clock cartridge */
};

static void fill(unsigned char *d, size_t n, unsigned seed)
{ size_t i; for (i = 0; i < n; i++) d[i] = (unsigned char)(i * 31 + seed); }

static unsigned char ours[140000], theirs[140000], tmpb[140000];
static size_t ours_n;

/* ours: the app's own save of the kind; GBA is a 128 KB image with the game's
 * data at the start and the rest erased (0xFF), as the core writes it; a GBC
 * clock cartridge ends in the 48 byte trailer with the "UTC" mark. */
static void make_ours(const kind_t *k, unsigned seed)
{
   ours_n = k->len;
   if (k->kind == SS_KIND_GBA)
   {
      memset(ours, 0xFF, ours_n);
      fill(ours, 2000, seed);
   }
   else
   {
      fill(ours, k->ram, seed);
      if (k->len > k->ram)
      {
         memset(ours + k->ram, 0, 48);
         ours[k->ram + 37] = 'U'; ours[k->ram + 38] = 'T'; ours[k->ram + 39] = 'C';
         ours[k->ram + 40] = 0x80; ours[k->ram + 41] = 0x11;
      }
   }
}

static void fresh(const kind_t *k, int ra_on)
{
   snprintf(root, sizeof root, "/tmp/save_sync_lh_%d", (int)getpid());
   sh("rm -rf %s", root);
   { char c[700]; snprintf(c, sizeof c, "mkdir -p %s/roms/Nintendo", root); sh("%s", c); }
   snprintf(rom_dir, sizeof rom_dir, "%s/roms/Nintendo/", root);
   snprintf(rom, sizeof rom, "%sSuper Example (USA) [!].%s", rom_dir, k->rom_ext + 1);
   snprintf(save, sizeof save, "%sSuper Example (USA) [!]%s", rom_dir, k->save_ext);
   snprintf(ra_dir, sizeof ra_dir, "%s/retroarch/savefiles", root);
   snprintf(ra, sizeof ra, "%s/Super Example (USA) [!].srm", ra_dir);
   snprintf(bdir, sizeof bdir, "%s/backups", root);
   put(rom, "ROMROMROM", 9);
   if (ra_on)
   { char c[700]; snprintf(c, sizeof c, "mkdir -p '%s'", ra_dir); sh("%s", c); }
   save_sync_init(root);
   save_sync_set_enabled(1);
   evt[0] = 0;
   g_kind = k->kind;
}

/* the old files 1.0 - 1.2 left beside the game (their names are not this
 * test's business: the app's sidecar code moves them) */
static void legacy_sidecars(void)
{
   char p[600];
   const char *x[] = { ".st0", ".st0.thumb", ".rsm", ".rsm.pt" };
   int i;
   for (i = 0; i < 4; i++)
   {
      snprintf(p, sizeof p, "%sSuper Example (USA) [!]%s", rom_dir, x[i]);
      put(p, "state", 5);
   }
}

static char note1[200], note2[200];
static int heal_rc;
/* the launch path of game_boot (main_vita.c), in order */
static void launch(void)
{
   note1[0] = note2[0] = 0;
   save_sync_game_start(save, note1, sizeof note1);
#ifndef OLD_CODE
   save_sync_set_game(save, rom, g_kind);
   heal_rc = save_sync_heal_game(save, rom, g_kind, note2, sizeof note2);
#else
   heal_rc = 0;
#endif
}

static const char *plain_target(const kind_t *k, int ra_on)
{
   if (ra_on) return ra;
   if (k->kind == SS_KIND_GBA) return save;
   {
      static char t[400];
      snprintf(t, sizeof t, "%sSuper Example (USA) [!].sav", rom_dir);
      return t;
   }
}

static int junk_left(void)
{
   char c[900]; int n = 0; FILE *p;
   snprintf(c, sizeof c, "find %s -name '*.import' -o -name '*.tmp' -o -name '*.old' | wc -l", root);
   p = popen(c, "r"); if (p) { if (fscanf(p, "%d", &n) != 1) n = 0; pclose(p); }
   return n;
}

static int census_lines(void)
{
   int n = 0; const char *p = evt;
   while ((p = strstr(p, "save_census")) != NULL) { n++; p++; }
   return n;
}

/* ---------------------------------------------------------------- cases -- */

/* A. 1.0 / 1.1 / 1.2: Game.sav / Game.gb.sav / Game.gbc.sav beside the game,
 * the old state files around it, no record, and RetroArch on the card with no
 * file of its own for this game (the reported card). */
static void case_v10_no_record(const kind_t *k, int ra_on)
{
   char l[100];
   snprintf(l, sizeof l, "%s v1.0-1.2 no record, RetroArch %s", k->name, ra_on ? "present" : "absent");
   g_label = l;
   fresh(k, ra_on);
   make_ours(k, 1);
   put(save, ours, ours_n);
   legacy_sidecars();
   launch();
   CK(same(save, ours, ours_n));                                /* our save untouched */
   {
      const char *t = plain_target(k, ra_on);
      CK(has(t));
      CK(same(t, ours, ours_n));                                /* a plain copy, as the core wrote it */
      CK(!strcmp(t, save) ? heal_rc == 0 : heal_rc == 1);
      if (strcmp(t, save)) CK(strstr(note2, "Save is now also at") && strstr(note2, "RetroArch can use it"));
   }
   CK(junk_left() == 0);
   CK(census_lines() >= 1 && strstr(evt, "save_census") && strstr(evt, "crc="));
   /* the second start: nothing again, no sentence, one census line more */
   { int c0 = census_lines();
     launch();
     CK(!note2[0]);
     CK(heal_rc == 0);
     CK(census_lines() == c0 + 1);
     CK(same(save, ours, ours_n)); }
   /* a later save of the game follows into the copy (and only then) */
   if (strcmp(plain_target(k, ra_on), save))
   {
      ours[10] ^= 0x5A;
      put(save, ours, ours_n);
      app_battery_written(save);
      CK(same(plain_target(k, ra_on), ours, ours_n));
   }
}

/* B. 1.3 / 1.3.1: the record exists (made at the first start) but names no
 * RetroArch file -- every save that did not come from RetroArch. */
static void case_v13_record_no_source(const kind_t *k)
{
   char l[100];
   snprintf(l, sizeof l, "%s v1.3 record without source", k->name);
   g_label = l;
   fresh(k, 1);
   make_ours(k, 2);
   put(save, ours, ours_n);
   save_sync_game_start(save, note1, sizeof note1);        /* what 1.3 did */
   save_sync_game_start(save, note1, sizeof note1);
   CK(!has(ra));                                           /* ...and RetroArch had nothing */
   launch();
   CK(has(ra) && same(ra, ours, ours_n));
   CK(same(save, ours, ours_n));
   CK(heal_rc == 1);
}

/* C. 1.2 copied RetroArch's Game.srm to Game.sav and the player played on:
 * RetroArch's file is older and differs.  A verified backup, then ours over it. */
static void case_v12_played_after_import(const kind_t *k)
{
   char l[100];
   snprintf(l, sizeof l, "%s v1.2 imported then played (RetroArch older)", k->name);
   g_label = l;
   fresh(k, 1);
   make_ours(k, 3);
   memcpy(theirs, ours, ours_n); theirs[5] ^= 0xFF; theirs[6] ^= 0xFF;
   put(ra, theirs, ours_n); stamp(ra, 1700000000L);
   put(save, ours, ours_n); stamp(save, 1700009000L);
   launch();
   CK(same(save, ours, ours_n));
   CK(same(ra, ours, ours_n));
   CK(has_backup(ra, theirs, ours_n));                      /* RetroArch's own bytes, whole */
   CK(heal_rc == 1);
   CK(junk_left() == 0);
}

/* D. 1.2 copied it and nothing was played: the same save -- linked, nothing
 * written, RetroArch's file untouched (same bytes, same time). */
static void case_v12_import_identical(const kind_t *k)
{
   char l[100];
   struct stat a, b;
   snprintf(l, sizeof l, "%s v1.2 imported, unchanged", k->name);
   g_label = l;
   fresh(k, 1);
   make_ours(k, 4);
   put(ra, ours, ours_n); stamp(ra, 1700000000L);
   put(save, ours, ours_n); stamp(save, 1700000100L);
   assert(stat(ra, &a) == 0);
   launch();
   assert(stat(ra, &b) == 0);
   CK(a.st_mtime == b.st_mtime && a.st_ino == b.st_ino);   /* not even rewritten */
   CK(same(ra, ours, ours_n) && same(save, ours, ours_n));
   CK(heal_rc == 2);
   CK(!note2[0]);
   /* and the link works: the game saves, RetroArch's file follows */
   ours[7] ^= 0x33; put(save, ours, ours_n);
   app_battery_written(save);
   CK(same(ra, ours, ours_n));
}

/* E. RetroArch's file is newer and differs, or has the same time and differs:
 * both stay as they are, both are backed up, and the player is told. */
static void case_foreign_conflict(const kind_t *k, int equal_time)
{
   char l[100];
   snprintf(l, sizeof l, "%s RetroArch file %s and different", k->name, equal_time ? "same time" : "newer");
   g_label = l;
   fresh(k, 1);
   make_ours(k, 5);
   memcpy(theirs, ours, ours_n); theirs[3] ^= 0x77; theirs[ours_n / 2] ^= 0x0F;
   if (k->kind == SS_KIND_GBA) theirs[20000] = 0x12;
   put(ra, theirs, ours_n); stamp(ra, equal_time ? 1700000000L : 1700005000L);
   put(save, ours, ours_n); stamp(save, 1700000000L);
   launch();
   CK(same(ra, theirs, ours_n));                            /* not clobbered */
   CK(same(save, ours, ours_n));
   CK(heal_rc == 3);
   CK(strstr(note2, "differ") && strstr(note2, "both kept") && strstr(note2, ra));
   CK(has_backup(ra, theirs, ours_n) && has_backup(save, ours, ours_n));
   /* kept apart at every later start, no sentence again, and a game save
    * does not reach RetroArch's file either */
   launch();
   CK(!note2[0] && same(ra, theirs, ours_n));
   ours[9] ^= 1; put(save, ours, ours_n);
   app_battery_written(save);
   CK(same(ra, theirs, ours_n));
}

/* F. 1.3 with a linked record, in step: nothing happens, no sentence. */
static void case_v13_linked(const kind_t *k)
{
   char l[100];
   snprintf(l, sizeof l, "%s v1.3 linked and in step", k->name);
   g_label = l;
   fresh(k, 1);
   make_ours(k, 6);
   put(ra, ours, ours_n); put(save, ours, ours_n);
   stamp(ra, 1700000000L); stamp(save, 1700000000L);
   save_sync_link(save, ra);
   save_sync_game_start(save, note1, sizeof note1);
   launch();
   CK(!note2[0] && heal_rc == 0);
   CK(same(ra, ours, ours_n) && same(save, ours, ours_n));
   CK(n_files(ra_dir) == 1);
}

/* G. the export cannot be written (a file where RetroArch's folder should
 * be): our save is untouched, the reason is given, nothing is half done, and the
 * next start (the folder is back) makes the copy. */
static void case_failed_write(const kind_t *k)
{
   char l[100];
   char r[400];
   snprintf(l, sizeof l, "%s write fails (not a directory)", k->name);
   g_label = l;
   fresh(k, 0);
   make_ours(k, 7);
   put(save, ours, ours_n);
   snprintf(r, sizeof r, "%s/retroarch", root);
   { char c[700]; snprintf(c, sizeof c, "mkdir -p %s", r); sh("%s", c); }
   put(ra_dir, "x", 1);                                     /* a FILE called savefiles */
   launch();
   CK(heal_rc == -1);
   CK(same(save, ours, ours_n));
   CK(strstr(note2, "could not be written"));
   CK(strstr(evt, "save_export FAILED"));
   CK(junk_left() == 0);
   CK(sizeof_file(ra_dir) == 1);                            /* untouched */
   assert(unlink(ra_dir) == 0);
   { char c[700]; snprintf(c, sizeof c, "mkdir -p '%s'", ra_dir); sh("%s", c); }
   launch();
   CK(heal_rc == 1 && same(ra, ours, ours_n));
}

/* H. power cut: (1) a half-written .import from an export that was cut short;
 * (2) a write that runs out of room half way; (3) a replacement of an older
 * RetroArch file cut between its two renames (the file is called .old and the
 * target is missing). */
static void case_power_cut(const kind_t *k)
{
   char l[100], p[500];
   struct rlimit old, lim;
   snprintf(l, sizeof l, "%s power cut during export", k->name);
   g_label = l;
   /* (1) */
   fresh(k, 1);
   make_ours(k, 8);
   put(save, ours, ours_n);
   snprintf(p, sizeof p, "%s.import", ra);
   put(p, ours, 777);
   launch();
   CK(same(ra, ours, ours_n) && !has(p) && junk_left() == 0);
   CK(same(save, ours, ours_n));
   /* (2) the card fills up half way */
   fresh(k, 1);
   make_ours(k, 9);
   put(save, ours, ours_n);
   signal(SIGXFSZ, SIG_IGN);
   assert(getrlimit(RLIMIT_FSIZE, &old) == 0);
   lim = old; lim.rlim_cur = 1000;
   assert(setrlimit(RLIMIT_FSIZE, &lim) == 0);
   launch();
   assert(setrlimit(RLIMIT_FSIZE, &old) == 0);
   CK(heal_rc == -1);
   CK(!has(ra) && junk_left() == 0);                        /* no half file anywhere */
   CK(same(save, ours, ours_n));
   launch();
   CK(heal_rc == 1 && same(ra, ours, ours_n));
   /* (3) */
   fresh(k, 1);
   make_ours(k, 10);
   memcpy(theirs, ours, ours_n); theirs[1] ^= 0xFF;
   snprintf(p, sizeof p, "%s.old", ra);
   put(p, theirs, ours_n); stamp(p, 1700000000L);
   put(save, ours, ours_n); stamp(save, 1700009000L);
   launch();
   CK(same(save, ours, ours_n));
   CK(same(ra, ours, ours_n) && has_backup(ra, theirs, ours_n));   /* theirs was put back, kept, replaced */
   CK(!has(p));
}

/* I. the setting: off means nothing is written, and says why; on again, it is. */
static void case_setting(const kind_t *k)
{
   char l[100];
   snprintf(l, sizeof l, "%s Sync RetroArch saves off, then on", k->name);
   g_label = l;
   fresh(k, 1);
   make_ours(k, 11);
   put(save, ours, ours_n);
   save_sync_set_enabled(0);
   launch();
   CK(!has(ra) && heal_rc == 0 && strstr(evt, "Sync RetroArch saves is off"));
   save_sync_set_enabled(1);
   launch();
   CK(has(ra) && same(ra, ours, ours_n));
}

/* J. RetroArch's own configuration decides where a new file goes. */
static void put_cfg(const char *text)
{
   char cfg[400];
   snprintf(cfg, sizeof cfg, "%s/retroarch/retroarch.cfg", root);
   put(cfg, text, strlen(text));
}

static void case_config(const kind_t *k)
{
   char l[100], other[400], p[500], c[900];
   snprintf(l, sizeof l, "%s retroarch.cfg decides the place", k->name);
   g_label = l;
   /* saves beside the game */
   fresh(k, 1);
   make_ours(k, 12);
   put(save, ours, ours_n);
   put_cfg("savefiles_in_content_dir = \"true\"\n");
   launch();
   snprintf(p, sizeof p, "%sSuper Example (USA) [!].srm", rom_dir);
   CK(has(p) && same(p, ours, ours_n) && !has(ra));
   /* a folder of its own */
   fresh(k, 1);
   make_ours(k, 13);
   put(save, ours, ours_n);
   snprintf(other, sizeof other, "%s/my saves", root);
   snprintf(c, sizeof c, "mkdir -p '%s'", other); sh("%s", c);
   snprintf(c, sizeof c, "savefile_directory = \"%s\"\n", other);
   put_cfg(c);
   launch();
   snprintf(p, sizeof p, "%s/Super Example (USA) [!].srm", other);
   CK(has(p) && same(p, ours, ours_n) && !has(ra));
   /* sorted into core folders, one of which exists */
   fresh(k, 1);
   make_ours(k, 14);
   put(save, ours, ours_n);
   snprintf(other, sizeof other, "%s/%s", ra_dir, k->kind == SS_KIND_GBA ? "mGBA" : "Gambatte");
   snprintf(c, sizeof c, "mkdir -p '%s'", other); sh("%s", c);
   put_cfg("sort_savefiles_enable = \"true\"\n");
   launch();
   snprintf(p, sizeof p, "%s/Super Example (USA) [!].srm", other);
   CK(has(p) && same(p, ours, ours_n) && !has(ra));
}

/* K. trailer variants: RetroArch's cores write the same save with a clock
 * trailer of 44 or 48 bytes, none (Gambatte keeps the clock in a .rtc), or at
 * the size of the save type (a 32 KB SRAM copy of the 128 KB image).  Each is
 * the same save (linked, nothing written); a save of ours then reaches it in
 * ITS size and trailer; and a newer file of RetroArch's is taken in whole. */
static void case_trailers(const kind_t *k)
{
   static const long variants[] = { 0, 44, 48 };
   char l[100];
   size_t v;
   if (k->kind == SS_KIND_GBA)
   {
      snprintf(l, sizeof l, "GBA 32 KB copy of the 128 KB image");
      g_label = l;
      fresh(k, 1);
      make_ours(k, 15);
      memset(ours + 32768, 0xFF, ours_n - 32768);
      put(save, ours, ours_n); stamp(save, 1700000100L);
      put(ra, ours, 32768); stamp(ra, 1700000000L);
      launch();
      CK(heal_rc == 2);
      memset(ours, 0x41, 300); put(save, ours, ours_n);
      app_battery_written(save);
      CK(same(ra, ours, 32768) && sizeof_file(ra) == 32768);
      /* a newer file of RetroArch's is taken whole, ours is backed up */
      memset(theirs, 0x77, 32768); put(ra, theirs, 32768); stamp(ra, 4000000000L);
      launch();
      CK(same(save, theirs, 32768));
      return;
   }
   for (v = 0; v < sizeof variants / sizeof variants[0]; v++)
   {
      size_t ram = k->ram, tn = (size_t)variants[v];
      snprintf(l, sizeof l, "%s RetroArch copy with a %ld byte trailer", k->name, variants[v]);
      g_label = l;
      fresh(k, 1);
      make_ours(k, 16);
      if (k->len == k->ram) tn = 0;                         /* no clock cartridge: no trailer at all */
      memcpy(theirs, ours, ram);
      memset(theirs + ram, 0x55, tn);                       /* their own clock bytes */
      put(save, ours, ours_n); stamp(save, 1700000100L);
      put(ra, theirs, ram + tn); stamp(ra, 1700000000L);
      launch();
      CK(heal_rc == 2);
      CK(same(ra, theirs, ram + tn));                       /* not rewritten */
      ours[11] ^= 0x44; put(save, ours, ours_n);
      app_battery_written(save);
      CK(sizeof_file(ra) == (long)(ram + tn));              /* it keeps its size */
      memcpy(tmpb, ours, ram);
      CK(same(ra, tmpb, ram) || (tn && sizeof_file(ra) == (long)(ram + tn)));
      { FILE *f = fopen(ra, "rb"); size_t got = f ? fread(tmpb, 1, sizeof tmpb, f) : 0; if (f) fclose(f);
        CK(got == ram + tn && !memcmp(tmpb, ours, ram)); }
   }
}

/* L. a game that is new: no save at its start, the first save makes the copy,
 * and the sentence waits to be taken. */
static void case_new_game(const kind_t *k)
{
   char l[100], n[200];
   snprintf(l, sizeof l, "%s new game, first save", k->name);
   g_label = l;
   fresh(k, 1);
   launch();
   CK(!has(save) && !has(ra) && heal_rc == 0);
   make_ours(k, 17);
   put(save, ours, ours_n);
   app_battery_written(save);
   CK(same(ra, ours, ours_n));
   CK(save_sync_take_note(n, sizeof n) && strstr(n, "RetroArch can use it"));
   CK(!save_sync_take_note(n, sizeof n));
}

/* M. the menu action: a plain file for other emulators, on request. */
static void case_manual(const kind_t *k)
{
   char l[100], where[400], n[200];
   snprintf(l, sizeof l, "%s Export save for other emulators", k->name);
   g_label = l;
   fresh(k, 1);
   make_ours(k, 18);
   put(save, ours, ours_n);
   save_sync_set_enabled(0);                                /* works whatever the setting says */
   CK(save_sync_export_now(save, rom, k->kind, SS_DEST_RETROARCH, where, sizeof where, n, sizeof n) == 0);
   CK(!strcmp(where, ra) && same(ra, ours, ours_n));
   memset(theirs, 0x21, ours_n); put(ra, theirs, ours_n);   /* something else is there now */
   CK(save_sync_export_now(save, rom, k->kind, SS_DEST_RETROARCH, where, sizeof where, n, sizeof n) == 0);
   CK(same(ra, ours, ours_n) && has_backup(ra, theirs, ours_n));
   if (k->kind != SS_KIND_GBA)
   {
      char p[500];
      CK(save_sync_export_now(save, rom, k->kind, SS_DEST_ROM_SAV, where, sizeof where, n, sizeof n) == 0);
      snprintf(p, sizeof p, "%sSuper Example (USA) [!].sav", rom_dir);
      CK(!strcmp(where, p) && same(p, ours, ours_n));
      /* a GBA game of the same name owns Game.sav */
      snprintf(p, sizeof p, "%sSuper Example (USA) [!].gba", rom_dir);
      if (k->kind == SS_KIND_GB)
      {
         char q[500];
         put(p, "gba", 3);
         snprintf(q, sizeof q, "%sSuper Example (USA) [!].sav", rom_dir);
         (void)unlink(q);
         CK(save_sync_export_now(save, rom, k->kind, SS_DEST_ROM_SAV, where, sizeof where, n, sizeof n) == -1);
         CK(!has(q));
      }
   }
   else
   {
      CK(save_sync_export_now(save, rom, k->kind, SS_DEST_ROM_SAV, where, sizeof where, n, sizeof n) == 0);
      CK(!strcmp(where, save));                              /* Game.sav is the plain file already */
   }
}

/* N. what an older version left beside the game while the game's folder has
 * its own: out of the ROM folder, verified; a file with no twin stays. */
static void case_tidy(const kind_t *k)
{
   char l[100], old1[700], new1[700], a[760], b[760], c[760];
   const char *st = k->kind == SS_KIND_GBA ? "st0" : k->kind == SS_KIND_GB ? "gb.st0" : "gbc.st0";
   snprintf(l, sizeof l, "%s odd files beside the game", k->name);
   g_label = l;
   fresh(k, 1);
   snprintf(old1, sizeof old1, "%sSuper Example (USA) [!].%s", rom_dir, st);
   snprintf(new1, sizeof new1, "%s/states/x/ab/Super Example (USA) [!].%s", root, st);
   { char d[800]; snprintf(d, sizeof d, "mkdir -p '%s/states/x/ab'", root); sh("%s", d); }
   put(old1, "OLD-slot1", 9); put(new1, "NEW-slot1", 9);
   snprintf(a, sizeof a, "%sSuper Example (USA) [!].%.*srsm", rom_dir, (int)(strlen(st) - 3), st);
   put(a, "OLD-resume", 10);                                 /* no twin: stays */
   snprintf(b, sizeof b, "%sSuper Example (USA) [!].%.*sst1.thumb", rom_dir, (int)(strlen(st) - 3), st);
   snprintf(c, sizeof c, "%s/states/x/ab/Super Example (USA) [!].%.*sst1.thumb", root, (int)(strlen(st) - 3), st);
   put(b, "OLD-thumb", 9); put(c, "NEW-thumb", 9);
   CK(save_sync_tidy_beside_rom(old1, new1) == 2);
   CK(!has(old1) && !has(b) && has(a));                      /* twins gone, the lone file stays */
   CK(same(new1, (const unsigned char *)"NEW-slot1", 9) && same(c, (const unsigned char *)"NEW-thumb", 9));
   CK(n_files((snprintf(l, sizeof l, "%s/leftovers", root), l)) == 2);
   CK(save_sync_tidy_beside_rom(old1, new1) == 0);           /* once */
}

int main(void)
{
   size_t i;
   for (i = 0; i < sizeof kinds / sizeof kinds[0]; i++)
   {
      const kind_t *k = &kinds[i];
      case_v10_no_record(k, 1);
      case_v10_no_record(k, 0);
      case_v13_record_no_source(k);
      case_v12_played_after_import(k);
      case_v12_import_identical(k);
      case_foreign_conflict(k, 0);
      case_foreign_conflict(k, 1);
      case_v13_linked(k);
      case_failed_write(k);
      case_power_cut(k);
      case_setting(k);
      case_config(k);
      case_trailers(k);
      case_new_game(k);
      case_manual(k);
      case_tidy(k);
   }
   sh("rm -rf %s", root);
   printf("legacy heal: %d checks, %d failed\n", n_checks, n_fail);
   if (n_fail)
      return 1;
   puts("legacy heal: ok");
   return 0;
}
