/* local_vita.c save_write(): the second game's save file is never lost.
 * A clone that replaces a save the player already has first needs a whole
 * copy of it that reads back right (size + CRC); without one the save is left
 * byte for byte as it was and the player is told.  A full card (RLIMIT_FSIZE
 * short writes) or an unwritable name never leaves the only copy truncated.
 *   gcc -Wall -Wextra -Werror -no-pie -DVITA -I vita -I psp -I frontend-common \
 *       -I libretro/libretro-common/include -I . \
 *       vita/tests/localsave/test_local_save_fail.c frontend-common/fe_util.c \
 *       -Wl,--unresolved-symbols=ignore-all -o /tmp/test_local_save_fail
 * (the rest of the app is not linked: nothing the test calls reaches it) */
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
#include "../../local_vita.c"

static char evt[8192], toast[256];
void fe_evt(const char *f, ...)
{
   va_list ap;
   size_t n = strlen(evt);
   if (n > sizeof(evt) - 512)
      n = evt[0] = 0;
   va_start(ap, f);
   vsnprintf(evt + n, sizeof(evt) - n, f, ap);
   va_end(ap);
}
void osd_toast(const char *s, ...) { snprintf(toast, sizeof(toast), "%s", s); }

#define SZ 16384
static char dir[128], sav[200], bak[220], tmpn[220];

static void put(const char *p, int v, size_t n)
{
   FILE *f = fopen(p, "wb");
   size_t i;
   assert(f);
   for (i = 0; i < n; i++)
      fputc(v, f);
   assert(fclose(f) == 0);
}
static int is(const char *p, int v, size_t n)
{
   FILE *f = fopen(p, "rb");
   size_t i;
   int ok = 1;
   if (!f)
      return 0;
   for (i = 0; i < n; i++)
      if (fgetc(f) != v)
      {
         ok = 0;
         break;
      }
   if (ok && fgetc(f) != EOF)
      ok = 0;
   fclose(f);
   return ok;
}
static int exists(const char *p) { return access(p, F_OK) == 0; }

static void fsize_limit(long n)
{
   struct rlimit r;
   getrlimit(RLIMIT_FSIZE, &r);
   r.rlim_cur = n < 0 ? r.rlim_max : (rlim_t)n;
   assert(setrlimit(RLIMIT_FSIZE, &r) == 0);
}

/* one write of a clone's image (all bytes `v`) through the real code */
static void run(int clone, int v)
{
   memset(L.save_snap, v, SZ);
   L.save_n = SZ;
   L.clone = clone;
   L.wr.done = 0;
   toast[0] = 0;
   evt[0] = 0;
   save_write();
   save_report();
}

static void fresh(void)
{
   char cmd[400];
   snprintf(cmd, sizeof(cmd), "rm -rf %s/* ; rmdir %s/*.d 2>/dev/null", dir, dir);
   (void)system(cmd);
   fsize_limit(-1);
   L.save_bak_done = 0;
   L.save_exists = 0;
   L.save_count = 0;
   snprintf(L.save_path, sizeof(L.save_path), "%s", sav);
}

int main(void)
{
   signal(SIGXFSZ, SIG_IGN);
   snprintf(dir, sizeof(dir), "/tmp/local-save-fail-%d", (int)getpid());
   assert(mkdir(dir, 0777) == 0);
   snprintf(sav, sizeof(sav), "%s/Game.p2.sav", dir);
   snprintf(bak, sizeof(bak), "%s.bak", sav);
   snprintf(tmpn, sizeof(tmpn), "%s.tmp", sav);

   /* 1. a clone over the player's save: kept whole first, then replaced */
   fresh();
   put(sav, 0xA1, SZ);
   run(1, 0xB2);
   assert(is(sav, 0xB2, SZ) && is(bak, 0xA1, SZ) && !toast[0]);
   assert(L.save_bak_done && strstr(evt, "local_save_backup") &&
          strstr(evt, "rc=0"));
   assert(!exists(tmpn));
   puts("ok  clone: old save kept (.bak, verified), new one in");

   /* 2. no .bak taken without a clone; no clone, no backup */
   fresh();
   put(sav, 0xA1, SZ);
   run(0, 0xB2);
   assert(is(sav, 0xB2, SZ) && !exists(bak) && !toast[0]);
   puts("ok  not a clone: no backup needed");

   /* 3. the backup cannot be made (its name is a folder): save untouched */
   fresh();
   put(sav, 0xA1, SZ);
   assert(mkdir(bak, 0777) == 0);
   run(1, 0xB2);
   assert(is(sav, 0xA1, SZ));
   assert(strstr(toast, "backed up") && !L.save_bak_done);
   assert(strstr(evt, "rc=-1") && !exists(tmpn));
   assert(L.wr.ok == 0 && L.wr.bak < 0);
   /* the obstacle goes: the next write backs up, then replaces */
   rmdir(bak);
   run(1, 0xB3);
   assert(is(sav, 0xB3, SZ) && is(bak, 0xA1, SZ) && L.save_bak_done);
   puts("ok  backup name blocked: save byte-identical, player told, next write works");

   /* 4. the backup's temporary name is blocked */
   fresh();
   put(sav, 0xA1, SZ);
   {
      char t[240];
      snprintf(t, sizeof(t), "%s.tmp", bak);
      assert(mkdir(t, 0777) == 0);
      run(1, 0xB2);
      assert(is(sav, 0xA1, SZ) && !exists(bak) && strstr(toast, "backed up"));
      rmdir(t);
   }
   puts("ok  backup .tmp blocked: save byte-identical");

   /* 5. full card while the backup is written (RLIMIT_FSIZE short write) */
   fresh();
   put(sav, 0xA1, SZ);
   fsize_limit(SZ / 2);
   run(1, 0xB2);
   fsize_limit(-1);
   assert(is(sav, 0xA1, SZ) && !exists(bak) && !exists(tmpn));
   {
      char t[240];
      snprintf(t, sizeof(t), "%s.tmp", bak);
      assert(!exists(t));
   }
   assert(strstr(toast, "backed up") && L.wr.ok == 0);
   puts("ok  card full during backup: save byte-identical, no partial .bak");

   /* 6. full card while the new save is written, backup fine: the old file
    * stays (it is NOT reopened "wb" in place) */
   fresh();
   put(sav, 0xA1, SZ);
   L.save_bak_done = 1;           /* backup already made earlier */
   fsize_limit(SZ / 2);
   run(0, 0xB2);
   fsize_limit(-1);
   assert(is(sav, 0xA1, SZ) && !exists(tmpn) && L.wr.ok == 0);
   assert(strstr(toast, "old one is kept"));
   puts("ok  card full during the write: old save intact, player told");

   /* 7. the temp name is taken by a folder ("no room for a second copy"):
    * with a save there, it is left alone, not truncated in place */
   fresh();
   put(sav, 0xA1, SZ);
   assert(mkdir(tmpn, 0777) == 0);
   run(0, 0xB2);
   assert(is(sav, 0xA1, SZ) && L.wr.ok == 0 && strstr(toast, "old one is kept"));
   rmdir(tmpn);
   puts("ok  no second copy possible: existing save never opened for write");

   /* 8. ... but with no save at all there is nothing to lose: in place */
   fresh();
   assert(mkdir(tmpn, 0777) == 0);
   run(0, 0xB2);
   assert(is(sav, 0xB2, SZ) && L.wr.ok == 1 && !toast[0]);
   rmdir(tmpn);
   /* and a failure there leaves no partial file behind */
   fresh();
   assert(mkdir(tmpn, 0777) == 0);
   fsize_limit(SZ / 2);
   run(0, 0xB2);
   fsize_limit(-1);
   assert(!exists(sav) && L.wr.ok == 0 && strstr(toast, "could not be written"));
   rmdir(tmpn);
   puts("ok  first save: written in place when nothing is there; no partial file on failure");

   /* 9. a .bak from an earlier session with other bytes is not overwritten */
   fresh();
   put(sav, 0xA1, SZ);
   put(bak, 0x77, SZ);
   run(1, 0xB2);
   assert(is(sav, 0xB2, SZ) && is(bak, 0x77, SZ) && !toast[0]);
   {
      char cmd[300];
      int c = 0;
      FILE *f;
      snprintf(cmd, sizeof(cmd), "ls %s | grep -c '\\.bak$'", dir);
      f = popen(cmd, "r");
      assert(f && fscanf(f, "%d", &c) == 1);
      pclose(f);
      assert(c == 2);
   }
   puts("ok  an older .bak is kept; this one gets a name of its own");

   /* 10. the same save backed up twice is not copied twice */
   fresh();
   put(sav, 0xA1, SZ);
   run(1, 0xB2);
   L.save_bak_done = 0;
   put(sav, 0xA1, SZ);
   run(1, 0xB4);
   assert(is(sav, 0xB4, SZ) && is(bak, 0xA1, SZ));
   puts("ok  identical .bak already there: reused");

   fresh();
   {
      char cmd[300];
      snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
      (void)system(cmd);
   }
   puts("PASS local_vita save_write failure tests");
   return 0;
}
