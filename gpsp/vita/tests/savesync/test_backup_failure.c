/* save_sync.c: a backup is what lets a save be overwritten.  When it cannot
 * be made (backups/ not a directory, storage full, a truncated or wrong copy
 * already there) both files stay byte for byte as they were, nothing is marked
 * as synced, the player is told the real reason, and the work is done at a
 * later start once a whole, verified backup exists.
 *   gcc -Wall -Wextra -Werror -I vita -I frontend-common \
 *       vita/tests/savesync/test_backup_failure.c vita/save_sync.c \
 *       frontend-common/fe_util.c -o /tmp/test_backup_failure */
#undef NDEBUG
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

#define BIG 131088                     /* 128 KB + a 16 byte trailer */

static char evt[4096];
void fe_evt(const char *f, ...)
{
   va_list ap;
   size_t n = strlen(evt);
   if (n > sizeof(evt) - 512)
      n = evt[0] = 0;
   va_start(ap, f);
   vsnprintf(evt + n, sizeof(evt) - n, f, ap);
   va_end(ap);
   strncat(evt, "\n", sizeof(evt) - strlen(evt) - 1);
}

static char root[256], save[300], src[300], bdir[300];

static void put(const char *p, const void *d, size_t n)
{ FILE *f = fopen(p, "wb"); assert(f && fwrite(d, 1, n, f) == n); assert(fclose(f) == 0); }
static size_t get(const char *p, unsigned char *b, size_t cap)
{ FILE *f = fopen(p, "rb"); size_t n; if (!f) return (size_t)-1; n = fread(b, 1, cap, f); fclose(f); return n; }
static void stamp(const char *p, long t)
{ struct utimbuf u; u.actime = u.modtime = t; assert(utime(p, &u) == 0); }
static void sh(const char *fmt, const char *a)
{ char c[1200]; snprintf(c, sizeof c, fmt, a); assert(system(c) == 0); }

static void fresh(void)
{
   snprintf(root, sizeof root, "/tmp/save_sync_bf_%d", (int)getpid());
   sh("rm -rf %s", root);
   { char c[600]; snprintf(c, sizeof c, "mkdir -p %s/roms %s/ra", root, root); assert(system(c) == 0); }
   snprintf(save, sizeof save, "%s/roms/g.sav", root);
   snprintf(src, sizeof src, "%s/ra/g.srm", root);
   snprintf(bdir, sizeof bdir, "%s/backups", root);
   save_sync_init(root);
   save_sync_set_enabled(1);
   evt[0] = 0;
}

/* make backups/ unusable (a file where the directory should be) / usable */
static void break_backups(void)
{ sh("rm -rf %s", bdir); put(bdir, "x", 1); }
static void fix_backups(void)
{ assert(unlink(bdir) == 0); }

static int n_backups(void)
{
   char c[700]; int n = 0; FILE *p;
   snprintf(c, sizeof c, "ls %s 2>/dev/null | wc -l", bdir);
   p = popen(c, "r"); if (p) { if (fscanf(p, "%d", &n) != 1) n = 0; pclose(p); }
   return n;
}

/* Is there a whole backup of exactly these bytes of `orig` (named as
 * save_sync names it)? */
static int has_backup(const char *orig, const unsigned char *d, size_t n)
{
   static unsigned char b[BIG + 64];
   char p[700];
   const char *bn = strrchr(orig, '/') + 1;
   snprintf(p, sizeof p, "%s/%s.%08x.bak", bdir, bn, (unsigned)fe_crc32(0, d, n));
   return get(p, b, sizeof b) == n && !memcmp(b, d, n);
}

static int same(const char *p, const unsigned char *d, size_t n)
{
   static unsigned char b[BIG + 64];
   return get(p, b, sizeof b) == n && !memcmp(b, d, n);
}

static void fill(unsigned char *d, size_t n, unsigned seed)
{ size_t i; for (i = 0; i < n; i++) d[i] = (unsigned char)(i * 31 + seed); }

/* ---- 1. both conflict directions, backups/ unusable ---- */
static void both_directions(void)
{
   static unsigned char o1[8192], s1[8192], o2[8192];
   char note[200];
   int dir;
   for (dir = 0; dir < 2; dir++)       /* 0: RetroArch newer, 1: ours newer */
   {
      fresh();
      memset(o1, 0x11, sizeof o1);
      put(save, o1, sizeof o1); put(src, o1, sizeof o1);
      save_sync_link(save, src);
      save_sync_game_start(save, note, sizeof note);   /* backs up the import */
      memset(o1, 0x22, sizeof o1); memset(s1, 0x33, sizeof s1);
      put(save, o1, sizeof o1); put(src, s1, sizeof s1);
      stamp(save, 1700000000L); stamp(src, dir ? 1699999000L : 1700001000L);
      break_backups();
      save_sync_game_start(save, note, sizeof note);
      assert(same(save, o1, sizeof o1) && same(src, s1, sizeof s1));
      assert(strstr(note, "Not a directory"));                 /* the real errno */
      assert(!strstr(note, "is in backups") && !strstr(note, "Newer save from"));
      assert(strstr(evt, "FAILED"));
      /* a later start with the same trouble changes nothing either */
      save_sync_game_start(save, note, sizeof note);
      assert(same(save, o1, sizeof o1) && same(src, s1, sizeof s1));
      /* the game saves meanwhile: the source still is not touched */
      if (dir) { memset(o2, 0x44, sizeof o2); put(save, o2, sizeof o2); app_battery_written(save);
                 assert(same(src, s1, sizeof s1)); put(save, o1, sizeof o1); stamp(save, 1700000000L); }
      /* storage is back: now the work is done, after a whole backup */
      fix_backups();
      save_sync_game_start(save, note, sizeof note);
      if (dir)
      {
         assert(same(src, o1, sizeof o1) && same(save, o1, sizeof o1));
         assert(has_backup(src, s1, sizeof s1));
         assert(strstr(note, "is in backups"));
      }
      else
      {
         assert(same(save, s1, sizeof s1));
         assert(has_backup(save, o1, sizeof o1));
         assert(strstr(note, "Newer save from RetroArch"));
      }
   }
}

/* ---- 2. first write over an imported save, with a trailer ---- */
static void first_write_over_import(void)
{
   static unsigned char imp[BIG], gm[BIG];
   char note[200];
   fresh();
   fill(imp, BIG, 7);
   memcpy(imp + 131072, "MGBA-CLOCK-BYTES", 16);        /* mGBA's trailer */
   put(src, imp, BIG); put(save, imp, BIG);
   stamp(save, 1700000000L); stamp(src, 1700000000L);
   save_sync_link(save, src);
   break_backups();                                      /* even the first copy fails */
   save_sync_game_start(save, note, sizeof note);
   assert(strstr(note, "Not a directory"));
   /* the game saves over its battery file */
   memcpy(gm, imp, BIG); memset(gm, 0x5A, 300);
   put(save, gm, BIG);
   app_battery_written(save);
   assert(same(src, imp, BIG));                          /* RetroArch's file untouched */
   save_sync_game_start(save, note, sizeof note);        /* still broken: still untouched */
   assert(same(src, imp, BIG) && same(save, gm, BIG));
   fix_backups();
   save_sync_game_start(save, note, sizeof note);
   assert(has_backup(src, imp, BIG));                    /* the imported bytes, trailer and all */
   assert(same(src, gm, BIG));                           /* now written, trailer kept */
   assert(same(save, gm, BIG));
}

/* ---- 3. storage full: a short write ---- */
static void full_storage(void)
{
   static unsigned char o[BIG], s[BIG];
   char note[200];
   struct rlimit lim, old;
   fresh();
   fill(o, BIG, 1); fill(s, BIG, 2);
   put(save, o, BIG); put(src, o, BIG);
   save_sync_link(save, src);
   put(save, o, BIG); put(src, s, BIG);
   stamp(save, 1700000000L); stamp(src, 1700001000L);    /* RetroArch newer */
   signal(SIGXFSZ, SIG_IGN);
   assert(getrlimit(RLIMIT_FSIZE, &old) == 0);
   lim = old; lim.rlim_cur = 40000;
   assert(setrlimit(RLIMIT_FSIZE, &lim) == 0);
   save_sync_game_start(save, note, sizeof note);
   assert(setrlimit(RLIMIT_FSIZE, &old) == 0);
   assert(same(save, o, BIG) && same(src, s, BIG));
   assert(strstr(note, "File too large"));
   assert(!has_backup(save, o, BIG));                    /* no partial file counts */
   { char c[700]; snprintf(c, sizeof c, "ls %s | grep -c tmp >/dev/null", bdir); assert(system(c) != 0); }
   save_sync_game_start(save, note, sizeof note);        /* space is back */
   assert(same(save, s, BIG) && has_backup(save, o, BIG));
   /* the same on the way out: ours newer, the source's backup cannot be written */
   fill(o, BIG, 9); put(save, o, BIG); stamp(save, 1700009000L);
   fill(s, BIG, 8); put(src, s, BIG); stamp(src, 1700002000L);
   assert(setrlimit(RLIMIT_FSIZE, &lim) == 0);
   save_sync_game_start(save, note, sizeof note);
   assert(setrlimit(RLIMIT_FSIZE, &old) == 0);
   assert(same(save, o, BIG) && same(src, s, BIG));
   assert(strstr(note, "File too large") && !strstr(note, "is in backups"));
   save_sync_game_start(save, note, sizeof note);
   assert(same(src, o, BIG) && has_backup(src, s, BIG));
   /* backup exists but the copy itself runs out of room: also nothing lost */
   fill(s, BIG, 12); put(src, s, BIG); stamp(src, 1700020000L);
   assert(setrlimit(RLIMIT_FSIZE, &lim) == 0);
   save_sync_game_start(save, note, sizeof note);
   assert(setrlimit(RLIMIT_FSIZE, &old) == 0);
   assert(same(save, o, BIG) && same(src, s, BIG) && strstr(note, "File too large"));
   save_sync_game_start(save, note, sizeof note);
   assert(same(save, s, BIG));
}

/* ---- 4. a truncated or wrong backup is not trusted ---- */
static void truncated_backup(void)
{
   static unsigned char o[BIG], s[BIG];
   char note[200], p[700];
   fresh();
   fill(o, BIG, 3); fill(s, BIG, 4);
   put(save, o, BIG); put(src, o, BIG);
   save_sync_link(save, src);
   save_sync_game_start(save, note, sizeof note);
   sh("rm -rf %s", bdir);
   assert(mkdir(bdir, 0777) == 0);
   put(save, o, BIG); put(src, s, BIG);
   stamp(save, 1700000000L); stamp(src, 1700001000L);
   /* the file a backup of `o` would be, cut short; and one with the right
    * size but wrong bytes */
   snprintf(p, sizeof p, "%s/g.sav.%08x.bak", bdir, (unsigned)fe_crc32(0, o, BIG));
   put(p, o, BIG / 2);
   save_sync_game_start(save, note, sizeof note);
   assert(same(save, s, BIG));                           /* it went ahead ... */
   assert(same(p, o, BIG));                              /* ... after a whole copy replaced it */
   fresh();
   fill(o, BIG, 5); fill(s, BIG, 6);
   put(save, o, BIG); put(src, o, BIG);
   save_sync_link(save, src);
   save_sync_game_start(save, note, sizeof note);
   sh("rm -rf %s", bdir);
   assert(mkdir(bdir, 0777) == 0);
   put(save, o, BIG); put(src, s, BIG);
   stamp(save, 1700000000L); stamp(src, 1700001000L);
   snprintf(p, sizeof p, "%s/g.sav.%08x.bak", bdir, (unsigned)fe_crc32(0, o, BIG));
   { static unsigned char w[BIG]; memcpy(w, o, BIG); w[BIG - 1] ^= 1; put(p, w, BIG); }
   save_sync_game_start(save, note, sizeof note);
   assert(same(p, o, BIG) && same(save, s, BIG));
   /* a stale .tmp from a crashed run is simply rewritten */
   fresh();
   fill(o, BIG, 5); fill(s, BIG, 6);
   put(save, o, BIG); put(src, o, BIG);
   save_sync_link(save, src);
   save_sync_game_start(save, note, sizeof note);
   put(save, o, BIG); put(src, s, BIG);
   stamp(save, 1700000000L); stamp(src, 1700001000L);
   snprintf(p, sizeof p, "%s/g.sav.%08x.bak.tmp", bdir, (unsigned)fe_crc32(0, o, BIG));
   put(p, "junk", 4);
   save_sync_game_start(save, note, sizeof note);
   assert(same(save, s, BIG) && has_backup(save, o, BIG));
}

/* ---- 5. equal timestamps ---- */
static void equal_times(void)
{
   static unsigned char o[8192], s[8192], o0[8192];
   char note[200];
   int known;
   for (known = 0; known < 2; known++)
   {
      fresh();
      memset(o0, 0x10, sizeof o0);
      put(save, o0, sizeof o0); put(src, o0, sizeof o0);
      save_sync_link(save, src);
      if (known) save_sync_game_start(save, note, sizeof note);
      memset(o, 0x21, sizeof o); memset(s, 0x32, sizeof s);
      put(save, o, sizeof o); put(src, s, sizeof s);
      stamp(save, 1700000000L); stamp(src, 1700000000L);
      save_sync_game_start(save, note, sizeof note);
      assert(same(save, o, sizeof o) && same(src, s, sizeof s));   /* nothing picked */
      assert(strstr(note, "Both saves changed"));
      assert(has_backup(save, o, sizeof o) && has_backup(src, s, sizeof s));
      save_sync_game_start(save, note, sizeof note);               /* and not at the next start */
      assert(same(save, o, sizeof o) && same(src, s, sizeof s));
      assert(strstr(note, "Both saves changed"));
      app_battery_written(save);                                   /* nor by a save */
      assert(same(src, s, sizeof s));
      /* the same, backups unusable: still nothing, and the reason is given */
      fresh();
      put(save, o0, sizeof o0); put(src, o0, sizeof o0);
      save_sync_link(save, src);
      save_sync_game_start(save, note, sizeof note);
      put(save, o, sizeof o); put(src, s, sizeof s);
      stamp(save, 1700000000L); stamp(src, 1700000000L);
      break_backups();
      save_sync_game_start(save, note, sizeof note);
      assert(same(save, o, sizeof o) && same(src, s, sizeof s));
      assert(strstr(note, "Not a directory"));
   }
   /* only the source changed, same second: it is the newer one, with a backup */
   fresh();
   put(save, o0, sizeof o0); put(src, o0, sizeof o0);
   save_sync_link(save, src);
   save_sync_game_start(save, note, sizeof note);
   memset(s, 0x32, sizeof s); put(src, s, sizeof s);
   stamp(save, 1700000000L); stamp(src, 1700000000L);
   save_sync_game_start(save, note, sizeof note);
   assert(same(save, s, sizeof s) && has_backup(save, o0, sizeof o0));
}

/* ---- 6. trailers (mGBA clock bytes) through the failure ---- */
static void trailers(void)
{
   static unsigned char o[BIG], s[BIG], w[BIG];
   char note[200];
   fresh();
   fill(o, 131072, 1);                   /* the core writes no trailer */
   fill(s, 131072, 2); memcpy(s + 131072, "MGBA-RTC-CLOCK!!", 16);
   put(save, o, 131072); put(src, s, BIG);
   save_sync_link(save, src);
   put(src, s, BIG);
   save_sync_game_start(save, note, sizeof note);           /* first backups made */
   memset(o, 0x61, 500); put(save, o, 131072);                 /* ours changes, newer */
   stamp(save, 1700009000L); stamp(src, 1700000000L);
   put(src, s, BIG); stamp(src, 1700000000L);
   {  /* RetroArch changes its clock bytes and data */
      memcpy(w, s, BIG); memset(w, 0x77, 50); memcpy(w + 131072, "MGBA-RTC-CLOCK#2", 16);
      put(src, w, BIG); stamp(src, 1700000000L);
   }
   break_backups();
   save_sync_game_start(save, note, sizeof note);
   assert(same(src, w, BIG) && same(save, o, 131072));         /* trailer and all, untouched */
   fix_backups();
   save_sync_game_start(save, note, sizeof note);
   assert(has_backup(src, w, BIG));                         /* trailer inside the backup */
   assert(get(src, s, sizeof s) == BIG);                    /* the source keeps its size */
   assert(!memcmp(s, o, 131072) && !memcmp(s + 131072, "MGBA-RTC-CLOCK#2", 16));
}

/* ---- 7. external edits between passes ---- */
static void external_edits(void)
{
   static unsigned char o[8192], s[8192], e[8192];
   char note[200];
   fresh();
   memset(o, 0x10, sizeof o);
   put(save, o, sizeof o); put(src, o, sizeof o);
   save_sync_link(save, src);
   save_sync_game_start(save, note, sizeof note);
   /* RetroArch's file is edited by hand (same size) after the sync */
   memset(s, 0x20, sizeof s); put(src, s, sizeof s);
   memset(o, 0x11, sizeof o); put(save, o, sizeof o);
   app_battery_written(save);
   assert(same(src, s, sizeof s));
   /* our save is edited outside the app, and backups/ is unusable: the record
    * must not say "known", so the copy is made once storage is back */
   fresh();
   memset(o, 0x10, sizeof o);
   put(save, o, sizeof o);
   save_sync_game_start(save, note, sizeof note);           /* no source: first backup */
   assert(has_backup(save, o, sizeof o));
   memset(e, 0x99, sizeof e); put(save, e, sizeof e);       /* edited by hand */
   break_backups();
   save_sync_game_start(save, note, sizeof note);
   assert(strstr(note, "Not a directory") && same(save, e, sizeof e));
   fix_backups();
   save_sync_game_start(save, note, sizeof note);
   assert(has_backup(save, e, sizeof e) && !note[0]);
   { int n = n_backups(); save_sync_game_start(save, note, sizeof note); assert(n_backups() == n); }
}

int main(void)
{
   both_directions();
   first_write_over_import();
   full_storage();
   truncated_backup();
   equal_times();
   trailers();
   external_edits();
   sh("rm -rf %s", root);
   puts("save sync backup failure: ok");
   return 0;
}
