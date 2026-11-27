/* save_sync.c: the cases a whole-app run cannot make -- RetroArch changing
 * its file while the game runs, a source smaller than the save, backups.
 *   gcc -I vita -I frontend-common vita/tests/savesync/test_save_sync.c \
 *       vita/save_sync.c frontend-common/fe_util.c -o /tmp/test_save_sync */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <sys/stat.h>
#include <utime.h>
#include <time.h>
#include "save_sync.h"

void fe_evt(const char *f, ...) { (void)f; }

static char root[256], save[300], src[300];

static void put(const char *p, const void *d, size_t n)
{ FILE *f = fopen(p, "wb"); assert(f && fwrite(d, 1, n, f) == n); fclose(f); }
static size_t get(const char *p, unsigned char *b, size_t cap)
{ FILE *f = fopen(p, "rb"); size_t n; if (!f) return (size_t)-1; n = fread(b, 1, cap, f); fclose(f); return n; }
static void age(const char *p, long secs)
{ struct utimbuf t; t.actime = t.modtime = time(NULL) + secs; utime(p, &t); }
static int backups(void)
{ char c[400]; int n = 0; FILE *p; snprintf(c, sizeof c, "ls %s/backups 2>/dev/null | wc -l", root);
  p = popen(c, "r"); if (p) { if (fscanf(p, "%d", &n) != 1) n = 0; pclose(p); } return n; }

int main(void)
{
   static unsigned char a[140000], b[140000], c[140000];
   char note[160];
   size_t i;
   snprintf(root, sizeof root, "/tmp/save_sync_test_%d", (int)getpid());
   { char cmd[1100]; snprintf(cmd, sizeof cmd, "rm -rf %s; mkdir -p %s/roms %s/ra", root, root, root); assert(system(cmd) == 0); }
   snprintf(save, sizeof save, "%s/roms/g.sav", root);
   snprintf(src, sizeof src, "%s/ra/g.srm", root);
   save_sync_init(root);
   save_sync_set_enabled(1);

   /* 131088 bytes with a trailer: imported, written back unchanged in size */
   for (i = 0; i < 131088; i++) a[i] = (unsigned char)(i * 13 + 5);
   put(src, a, 131088); put(save, a, 131088); age(save, -100); age(src, -100);
   save_sync_link(save, src);
   save_sync_game_start(save, note, sizeof note);
   assert(backups() == 1);                       /* the one-time copy */
   save_sync_game_start(save, note, sizeof note);
   assert(backups() == 1);                       /* not again */
   /* the game saves: first 100 bytes change */
   memset(a, 0x42, 100); put(save, a, 131088);
   app_battery_written(save);
   assert(get(src, b, sizeof b) == 131088 && !memcmp(a, b, 131088));

   /* RetroArch changes its file while the game runs: not overwritten */
   memset(b, 0x77, 131088); put(src, b, 131088);
   memset(a, 0x43, 100); put(save, a, 131088);
   app_battery_written(save);
   assert(get(src, c, sizeof c) == 131088 && !memcmp(b, c, 131088));

   /* next start: RetroArch's is newer -> used, ours kept in backups */
   age(save, -50); age(src, 0);
   save_sync_game_start(save, note, sizeof note);
   assert(strstr(note, "RetroArch"));
   assert(get(save, c, sizeof c) == 131088 && !memcmp(b, c, 131088));
   assert(backups() >= 2);

   /* RetroArch changed it, but ours is newer: ours kept, theirs backed up,
    * and its file brought up to date */
   memset(b, 0x11, 131088); put(src, b, 131088); age(src, -500);
   memset(a, 0x99, 200); put(save, a, 131088); age(save, 0);
   { int before = backups();
     save_sync_game_start(save, note, sizeof note);
     assert(backups() == before + 2);            /* theirs, and ours (new) */ }
   assert(get(save, c, sizeof c) == 131088 && !memcmp(a, c, 131088));
   assert(get(src, c, sizeof c) == 131088 && !memcmp(a, c, 131088));

   /* a 32 KB source and a 128 KB save with data past 32 KB: not written */
   snprintf(src, sizeof src, "%s/ra/h.srm", root);
   snprintf(save, sizeof save, "%s/roms/h.sav", root);
   memset(a, 0xFF, 131072); memset(a, 0x21, 300);
   put(src, a, 32768); put(save, a, 32768); age(save, -9); age(src, -9);
   save_sync_link(save, src);
   save_sync_game_start(save, note, sizeof note);
   memset(a, 0xFF, 131072); memset(a, 0x22, 300); put(save, a, 131072);
   app_battery_written(save);
   assert(get(src, c, sizeof c) == 32768 && !memcmp(a, c, 32768));   /* 32 KB follows */
   a[40000] = 0x01; put(save, a, 131072);
   app_battery_written(save);
   a[40000] = 0xFF;
   assert(get(src, c, sizeof c) == 32768 && c[0] == 0x22);           /* source kept at 32 KB */
   memset(a, 0x23, 300);
   /* sync off: nothing is written to the source */
   save_sync_set_enabled(0);
   a[40000] = 0xFF; put(save, a, 131072);
   app_battery_written(save);
   assert(get(src, c, sizeof c) == 32768 && c[0] == 0x22);
   save_sync_set_enabled(1);

   { char cmd[1100]; snprintf(cmd, sizeof cmd, "rm -rf %s", root); (void)system(cmd); }
   puts("save sync: ok");
   return 0;
}
