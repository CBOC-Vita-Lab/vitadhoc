/* cpuclock.c -- see cpuclock.h. */
#include "cpuclock.h"
#include "plat.h"
#include "fe_evt.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define FAST_MHZ 500
#define STD_MHZ  444
#define SECOND_MEASURE_US 8000000u

static char     g_root[256];
static int      g_fast = 1;
static int      g_granted;          /* the system accepted the ask (500)       */
static int      g_plugin;           /* PSVshellPlus (or its folder) found      */
static int      g_wrote_now;        /* this run wrote a profile (needs restart) */
static int      g_nowrite;          /* the folder refused a write              */
static unsigned g_measured;
static uint64_t g_t0;
static int      g_second_done = 1;      /* 0 once booted */

static void put_le32(unsigned char *p, unsigned v)
{
   p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8);
   p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}

void cpuclk_profile_bytes(unsigned char out[CPUCLK_PROFILE_SIZE])
{
   memcpy(out, "PSVS0300", 8);
   put_le32(out + 8, 0x0bu);        /* CPU, GPU and crossbar locked */
   put_le32(out + 12, FAST_MHZ);
   put_le32(out + 16, 222);
   put_le32(out + 20, 222);
   put_le32(out + 24, 166);
}

/* ---- small file helpers ---- */

static int join(char *out, size_t cap, const char *dir, const char *name)
{
   int n = snprintf(out, cap, "%s/%s", dir, name);
   return n > 0 && (size_t)n < cap ? 0 : -1;
}

/* size read (max cap), -1 = absent/unreadable */
static int slurp(const char *path, unsigned char *buf, size_t cap)
{
   FILE *f = fopen(path, "rb");
   size_t n;
   if (!f)
      return -1;
   n = fread(buf, 1, cap, f);
   fclose(f);
   return (int)n;
}

static int exists(const char *path)
{
   struct stat st;
   return stat(path, &st) == 0;
}

/* Temp file + read back + rename.  0 = done and verified. */
static int safe_write(const char *path, const unsigned char *data, size_t n)
{
   char tmp[300];
   unsigned char back[256];
   FILE *f;
   if (n > sizeof(back) || snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp))
      return -1;
   f = fopen(tmp, "wb");
   if (!f)
      return -2;
   if (fwrite(data, 1, n, f) != n)
   {
      fclose(f); remove(tmp);
      return -1;
   }
   if (fclose(f) != 0 || slurp(tmp, back, sizeof(back)) != (int)n ||
       memcmp(back, data, n) != 0)
   {
      remove(tmp);
      return -1;
   }
   if (exists(path))
      remove(path);           /* the console's rename will not replace */
   if (rename(tmp, path) != 0)
   {
      remove(tmp);
      return -1;
   }
   if (slurp(path, back, sizeof(back)) != (int)n || memcmp(back, data, n) != 0)
      return -1;
   return 0;
}

static void mkdirs(const char *dir)
{
   char p[300];
   size_t i;
   if (snprintf(p, sizeof(p), "%s", dir) >= (int)sizeof(p))
      return;
   for (i = 1; p[i]; i++)
      if (p[i] == '/')
      {
         p[i] = '\0';
         mkdir(p, 0777);
         p[i] = '/';
      }
   mkdir(p, 0777);
}

/* ---- the plugin ---- */

static int plugin_present(void)
{
   const char *tai = plat_tai_dir();
   const char *prof = plat_psvshell_dir();
   DIR *d;
   int found = 0;
   if (tai && tai[0] && (d = opendir(tai)) != NULL)
   {
      struct dirent *e;
      while ((e = readdir(d)) != NULL)
      {
         size_t n = strlen(e->d_name);
         if (strncasecmp(e->d_name, "PSVshellPlus", 12) == 0 && n > 6 &&
             strcasecmp(e->d_name + n - 6, ".skprx") == 0)
            found = 1;
      }
      closedir(d);
   }
   if (!found && prof && prof[0] && (d = opendir(prof)) != NULL)
   {
      closedir(d);
      found = 1;
   }
   return found;
}

/* ---- the profile ---- */

static int paths(char *prof, size_t pc, char *bak, size_t bc, char *state,
                 size_t sc, char *stamp, size_t tc)
{
   const char *dir = plat_psvshell_dir();
   if (!dir || !dir[0] || !g_root[0])
      return -1;
   if (join(prof, pc, dir, plat_title_id()) != 0 ||
       join(bak, bc, g_root, "psvshell_profile_backup.bin") != 0 ||
       join(state, sc, g_root, "psvshell_profile.state") != 0 ||
       join(stamp, tc, g_root, "psvshell_nowrite.stamp") != 0)
      return -1;
   return 0;
}

static void write_state(const char *state, const char *what)
{
   FILE *f = fopen(state, "wb");
   if (f)
   {
      fputs(what, f);
      fclose(f);
   }
}

static int read_state(const char *state)   /* 0 none, 1 created, 2 replaced */
{
   char b[16] = "";
   FILE *f = fopen(state, "rb");
   if (!f)
      return 0;
   if (!fgets(b, sizeof(b), f))
      b[0] = '\0';
   fclose(f);
   return strncmp(b, "replaced", 8) == 0 ? 2 : strncmp(b, "created", 7) == 0 ? 1 : 0;
}

static void note_nowrite(const char *stamp, int err)
{
   FILE *f;
   g_nowrite = 1;
   fe_evt("cpu_profile not_writable errno=%d (not tried again)", err);
   f = fopen(stamp, "wb");
   if (f)
   {
      fprintf(f, "errno=%d\n", err);
      fclose(f);
   }
}

/* Make the profile ours.  boot: only when none exists.  menu: the player's
 * own is saved first. */
static void profile_apply(int from_menu)
{
   char prof[300], bak[300], state[300], stamp[300];
   unsigned char ours[CPUCLK_PROFILE_SIZE], cur[64];
   int n, rc;
   if (!g_plugin || paths(prof, sizeof(prof), bak, sizeof(bak), state,
                          sizeof(state), stamp, sizeof(stamp)) != 0)
      return;
   if (exists(stamp))
   {
      g_nowrite = 1;
      return;
   }
   cpuclk_profile_bytes(ours);
   n = slurp(prof, cur, sizeof(cur));
   if (n == CPUCLK_PROFILE_SIZE && memcmp(cur, ours, n) == 0)
   {
      fe_evt("cpu_profile present same");
      return;
   }
   if (n >= 0 && !from_menu)
   {
      fe_evt("cpu_profile kept_players bytes=%d", n);
      return;
   }
   if (n >= 0)
   {
      /* the player's own: a copy first; never overwrite an earlier backup
       * of theirs (that one is the original) */
      if (read_state(state) != 2)
      {
         if (safe_write(bak, cur, (size_t)n) != 0)
         {
            fe_evt("cpu_profile backup_failed, nothing written");
            return;
         }
      }
   }
   else
      mkdirs(plat_psvshell_dir());
   rc = safe_write(prof, ours, CPUCLK_PROFILE_SIZE);
   if (rc == -2)
   {
      note_nowrite(stamp, errno);
      return;
   }
   if (rc != 0)
   {
      fe_evt("cpu_profile write_failed rc=%d", rc);
      return;
   }
   if (n < 0 || read_state(state) == 0)
      write_state(state, n >= 0 ? "replaced" : "created");
   g_wrote_now = 1;
   fe_evt("cpu_profile written path=%s replaced=%d", prof, n >= 0);
}

/* Fast -> standard: take away only what we wrote. */
static void profile_remove(void)
{
   char prof[300], bak[300], state[300], stamp[300];
   unsigned char ours[CPUCLK_PROFILE_SIZE], cur[64], old[64];
   int st, n;
   if (!g_plugin || paths(prof, sizeof(prof), bak, sizeof(bak), state,
                          sizeof(state), stamp, sizeof(stamp)) != 0)
      return;
   st = read_state(state);
   if (!st)
      return;
   cpuclk_profile_bytes(ours);
   n = slurp(prof, cur, sizeof(cur));
   if (n >= 0 && !(n == CPUCLK_PROFILE_SIZE && memcmp(cur, ours, n) == 0))
   {
      /* the player changed it since: theirs now, leave it */
      fe_evt("cpu_profile left_alone changed_since");
      remove(state);
      return;
   }
   if (st == 2)
   {
      int b = slurp(bak, old, sizeof(old));
      if (b >= 0 && safe_write(prof, old, (size_t)b) == 0)
      {
         remove(bak);
         remove(state);
         fe_evt("cpu_profile restored bytes=%d", b);
      }
      else
         fe_evt("cpu_profile restore_failed (backup kept)");
      return;
   }
   if (n >= 0)
      remove(prof);
   remove(state);
   fe_evt("cpu_profile removed");
}

/* ---- the clock ---- */

const char *cpuclk_note(void);
static void measure(const char *when)
{
   g_measured = plat_arm_mhz_measured();
   fe_evt("clocks: the ARM core measures %u MHz (the system reports %u) %s",
          g_measured, plat_arm_mhz(), when);
   fe_evt("clocks: note=\"%s\"", cpuclk_note());
}

static void request(int fast)
{
   int want = fast ? FAST_MHZ : STD_MHZ;
   int rc = plat_cpu_set_mhz(want);
   fe_evt("clocks: asked for %d MHz, result %d", want, rc);
   g_granted = (rc >= 0) && fast;
   if (rc < 0 && fast)
   {
      int rc2 = plat_cpu_set_mhz(STD_MHZ);
      fe_evt("clocks: asked for %d MHz, result %d", STD_MHZ, rc2);
   }
}

void cpuclk_boot(const char *root, int fast)
{
   snprintf(g_root, sizeof(g_root), "%s", root ? root : "");
   g_fast = fast ? 1 : 0;
   g_t0 = plat_time_us();
   g_second_done = 0;
   g_plugin = plugin_present();
   fe_evt("clocks: setting=%d MHz plugin=%d", g_fast ? FAST_MHZ : STD_MHZ,
          g_plugin);
   request(g_fast);
   if (g_fast)
      profile_apply(0);
   measure("at start");
}

int cpuclk_fast(void) { return g_fast; }

void cpuclk_set(int fast)
{
   g_fast = fast ? 1 : 0;
   request(g_fast);
   if (g_fast)
      profile_apply(1);
   else
      profile_remove();
   measure("after the change");
}

void cpuclk_tick(void)
{
   if (g_second_done || plat_time_us() - g_t0 < SECOND_MEASURE_US)
      return;
   g_second_done = 1;
   measure("8 s in");
}

const char *cpuclk_note(void)
{
   static char s[128];
   /* One short line (the help line is about 56 characters wide) */
   if (g_fast && !g_granted && !g_plugin)
      return "Needs an overclock plugin such as PSVshell";
   if (g_fast && g_nowrite)
      return "Cannot write the PSVshell profile";
   if (g_measured)
      snprintf(s, sizeof(s), "Measured: %u MHz%s", g_measured,
               g_fast && g_wrote_now && g_measured < 480
                  ? ". Restart the app to apply" : "");
   else
      snprintf(s, sizeof(s), "Measured: not known");
   return s;
}
