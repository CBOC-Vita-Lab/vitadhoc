/* save_sync.c -- save_sync.h. */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "save_sync.h"
#include "fe_evt.h"
#include "fe_util.h"

#define SS_PATH 512
#define SS_LINE (SS_PATH * 2 + 128)
#define SS_CHUNK 4096

static char g_root[SS_PATH];
static char g_ra[SS_PATH + 32];
static char g_rec_path[SS_PATH + 24];
static int  g_on = 1;

typedef struct
{
   char     save[SS_PATH];
   char     src[SS_PATH];        /* "" = none */
   long     src_size;
   long     src_mtime;
   uint32_t src_crc;
   uint32_t our_crc;
   uint32_t sync_crc;           /* our_crc when the source last got it */
} ss_rec;

void save_sync_init(const char *app_root)
{
   snprintf(g_root, sizeof(g_root), "%s", app_root ? app_root : "");
   snprintf(g_rec_path, sizeof(g_rec_path), "%s/saves-sync.txt", g_root);
#ifdef __vita__
   snprintf(g_ra, sizeof(g_ra), "ux0:data/retroarch/savefiles");
#else
   snprintf(g_ra, sizeof(g_ra), "%s/retroarch/savefiles", g_root);
#endif
}

void save_sync_set_enabled(int on) { g_on = on ? 1 : 0; }
int  save_sync_enabled(void) { return g_on; }
const char *save_sync_ra_dir(void) { return g_ra; }

/* ---- small file helpers ---- */
static int stat_file(const char *p, long *size, long *mtime)
{
   struct stat st;
   if (stat(p, &st) != 0 || !S_ISREG(st.st_mode))
      return -1;
   if (size)
      *size = (long)st.st_size;
   if (mtime)
      *mtime = (long)st.st_mtime;
   return 0;
}

static int crc_file(const char *p, uint32_t *crc, long *size)
{
   unsigned char b[SS_CHUNK];
   FILE *f = fopen(p, "rb");
   size_t n;
   long total = 0;
   uint32_t c = 0;
   if (!f)
      return -1;
   while ((n = fread(b, 1, sizeof(b), f)) > 0)
   {
      c = fe_crc32(c, b, n);
      total += (long)n;
   }
   if (ferror(f))
   {
      fclose(f);
      return -1;
   }
   fclose(f);
   *crc = c;
   if (size)
      *size = total;
   return 0;
}

/* tmp -> dest, keeping the old dest as dest.old until the new one is in */
static int promote(const char *tmp, const char *dest)
{
   char old[SS_PATH + 8];
   struct stat st;
   int had = stat(dest, &st) == 0;
   if (snprintf(old, sizeof(old), "%s.old", dest) >= (int)sizeof(old))
      return -1;
   if (had)
   {
      remove(old);
      if (rename(dest, old) != 0 || stat(dest, &st) == 0)
         return -1;
   }
   (void)rename(tmp, dest);
   if (stat(dest, &st) != 0)
   {
      if (had)
         (void)rename(old, dest);
      return -1;
   }
   if (had)
      remove(old);
   return 0;
}

static void ensure_dir(const char *dir)
{
   mkdir(dir, 0777);
}

/* Copy `from` into backups/ once (named by its CRC).  0 = kept (or already
 * there), -1 = not. */
static int backup_file(const char *from, const char *why)
{
   char dir[SS_PATH + 16], dest[SS_PATH * 2], tmp[SS_PATH * 2 + 8];
   const char *bn = strrchr(from, '/');
   uint32_t crc;
   long size;
   FILE *in, *out;
   unsigned char b[SS_CHUNK];
   size_t n;
   int ok = 1;
   struct stat st;

   bn = bn ? bn + 1 : from;
   if (crc_file(from, &crc, &size) != 0 || size <= 0)
      return -1;
   snprintf(dir, sizeof(dir), "%s/backups", g_root);
   ensure_dir(dir);
   if (snprintf(dest, sizeof(dest), "%s/%s.%08x.bak", dir, bn,
                (unsigned)crc) >= (int)sizeof(dest))
      return -1;
   if (stat(dest, &st) == 0)
      return 0;
   snprintf(tmp, sizeof(tmp), "%s.tmp", dest);
   in = fopen(from, "rb");
   out = in ? fopen(tmp, "wb") : NULL;
   if (!in || !out)
      ok = 0;
   while (ok && (n = fread(b, 1, sizeof(b), in)) > 0)
      if (fwrite(b, 1, n, out) != n)
         ok = 0;
   if (in)
      fclose(in);
   if (out && fclose(out) != 0)
      ok = 0;
   if (ok)
      (void)rename(tmp, dest);
   ok = ok && stat(dest, &st) == 0;
   if (!ok)
      remove(tmp);
   fe_evt("save_backup %s file=\"%s\" to=\"%s\" size=%ld why=%s",
          ok ? "kept" : "FAILED", from, dest, size, why);
   return ok ? 0 : -1;
}

/* ---- the record file ---- */
static int rec_parse(char *line, ss_rec *r)
{
   char *f[7];
   int i;
   char *p = line;
   for (i = 0; i < 7; i++)
   {
      f[i] = p;
      if (i < 6)
      {
         p = strchr(p, '\t');
         if (!p)
            return -1;
         *p++ = 0;
      }
   }
   p = f[6] + strlen(f[6]);
   while (p > f[6] && (p[-1] == '\n' || p[-1] == '\r'))
      *--p = 0;
   if (strlen(f[0]) >= SS_PATH || strlen(f[1]) >= SS_PATH)
      return -1;
   snprintf(r->save, sizeof(r->save), "%s", f[0]);
   snprintf(r->src, sizeof(r->src), "%s", strcmp(f[1], "-") ? f[1] : "");
   r->src_size = atol(f[2]);
   r->src_mtime = atol(f[3]);
   r->src_crc = (uint32_t)strtoul(f[4], NULL, 16);
   r->our_crc = (uint32_t)strtoul(f[5], NULL, 16);
   r->sync_crc = (uint32_t)strtoul(f[6], NULL, 16);
   return 0;
}

static int rec_get(const char *save, ss_rec *r)
{
   FILE *f = fopen(g_rec_path, "rb");
   static char line[SS_LINE];
   int found = 0;
   if (!f)
      return 0;
   while (fgets(line, sizeof(line), f))
   {
      ss_rec t;
      if (rec_parse(line, &t) == 0 && !strcmp(t.save, save))
      {
         *r = t;
         found = 1;
         break;
      }
   }
   fclose(f);
   return found;
}

/* Replace (or add) the record for r->save: copy the file line by line. */
static int rec_put(const ss_rec *r)
{
   static char line[SS_LINE], copy[SS_LINE];
   char tmp[SS_PATH + 32];
   FILE *in = fopen(g_rec_path, "rb"), *out;
   int ok = 1;

   snprintf(tmp, sizeof(tmp), "%s.tmp", g_rec_path);
   out = fopen(tmp, "wb");
   if (!out)
   {
      if (in)
         fclose(in);
      return -1;
   }
   while (in && fgets(line, sizeof(line), in))
   {
      ss_rec t;
      snprintf(copy, sizeof(copy), "%s", line);
      if (rec_parse(copy, &t) == 0 && !strcmp(t.save, r->save))
         continue;
      if (fputs(line, out) < 0)
         ok = 0;
   }
   if (in)
      fclose(in);
   if (fprintf(out, "%s\t%s\t%ld\t%ld\t%08x\t%08x\t%08x\n", r->save,
               r->src[0] ? r->src : "-", r->src_size, r->src_mtime,
               (unsigned)r->src_crc, (unsigned)r->our_crc,
               (unsigned)r->sync_crc) < 0)
      ok = 0;
   if (fclose(out) != 0)
      ok = 0;
   if (ok)
      ok = promote(tmp, g_rec_path) == 0;
   if (!ok)
      remove(tmp);
   return ok ? 0 : -1;
}

/* ---- copying a source over the save, and the save over the source ---- */

/* from -> to (whole file, as it is), through to.import */
static int copy_over(const char *from, const char *to)
{
   char tmp[SS_PATH + 16];
   unsigned char b[SS_CHUNK];
   FILE *in = fopen(from, "rb"), *out;
   size_t n;
   int ok = 1;
   snprintf(tmp, sizeof(tmp), "%s.import", to);
   out = in ? fopen(tmp, "wb") : NULL;
   if (!in || !out)
      ok = 0;
   while (ok && (n = fread(b, 1, sizeof(b), in)) > 0)
      if (fwrite(b, 1, n, out) != n)
         ok = 0;
   if (ok && ferror(in))
      ok = 0;
   if (in)
      fclose(in);
   if (out && fclose(out) != 0)
      ok = 0;
   if (ok)
      ok = promote(tmp, to) == 0;
   if (!ok)
      remove(tmp);
   return ok ? 0 : -1;
}

/* The core length of a battery file of `size` bytes: the standard sizes a
 * trailer may follow (RetroArch/mGBA: 131072 + 16 and so on). */
static long core_len(long size)
{
   static const long base[] = { 131072, 65536, 32768, 8192, 512 };
   unsigned i;
   for (i = 0; i < sizeof(base) / sizeof(base[0]); i++)
      if (size >= base[i] && size - base[i] <= 64)
         return base[i];
   return size;
}

/* Does the save hold data (not 0xFF) in [from, to)? */
static int has_data_between(const char *save, long from, long to)
{
   unsigned char b[SS_CHUNK];
   FILE *f;
   long pos = from;
   if (to <= from)
      return 0;
   f = fopen(save, "rb");
   if (!f)
      return 1;                        /* cannot tell: assume yes */
   if (fseek(f, from, SEEK_SET) != 0)
   {
      fclose(f);
      return 1;
   }
   while (pos < to)
   {
      size_t want = (size_t)(to - pos > (long)sizeof(b) ? (long)sizeof(b) : to - pos);
      size_t n = fread(b, 1, want, f), i;
      if (!n)
         break;
      for (i = 0; i < n; i++)
         if (b[i] != 0xFF)
         {
            fclose(f);
            return 1;
         }
      pos += (long)n;
   }
   fclose(f);
   return 0;
}

/* Write the save back to its source in the source's size: the save's bytes
 * as far as both reach, the source's own bytes for what the save lacks.
 * Updates r on success. */
static int sync_out(ss_rec *r, long ours_len)
{
   char tmp[SS_PATH + 16];
   unsigned char b[SS_CHUNK];
   FILE *in, *old, *out;
   long cur_size, cur_mtime, i, limit, written = 0;
   uint32_t cur_crc, crc = 0;
   long clen;
   int ok = 1;

   if (stat_file(r->src, &cur_size, &cur_mtime) != 0)
      return -1;                       /* the source is gone: leave it so */
   if (crc_file(r->src, &cur_crc, &cur_size) != 0)
      return -1;
   if (cur_size != r->src_size || cur_crc != r->src_crc)
   {
      fe_evt("save_sync_out skipped: source changed since the last sync "
             "src=\"%s\"", r->src);
      return 1;
   }
   clen = core_len(cur_size);
   if (ours_len >= 131072 && clen < 131072 &&
       has_data_between(r->save, clen, 131072))
   {
      fe_evt("save_sync_out skipped: the save is larger than the source "
             "(%ld) src=\"%s\"", cur_size, r->src);
      return 1;
   }
   limit = ours_len < cur_size ? ours_len : cur_size;
   snprintf(tmp, sizeof(tmp), "%s.import", r->src);
   in = fopen(r->save, "rb");
   old = fopen(r->src, "rb");
   out = (in && old) ? fopen(tmp, "wb") : NULL;
   if (!out)
      ok = 0;
   for (i = 0; ok && i < cur_size; )
   {
      long want = cur_size - i > (long)sizeof(b) ? (long)sizeof(b) : cur_size - i;
      size_t n;
      if (i < limit)
      {
         if (want > limit - i)
            want = limit - i;
         n = fread(b, 1, (size_t)want, in);
      }
      else
      {
         if (fseek(old, i, SEEK_SET) != 0)
            ok = 0;
         n = ok ? fread(b, 1, (size_t)want, old) : 0;
      }
      if (n != (size_t)want || fwrite(b, 1, n, out) != n)
         ok = 0;
      else
      {
         crc = fe_crc32(crc, b, n);
         i += (long)n;
         written += (long)n;
      }
   }
   if (in)
      fclose(in);
   if (old)
      fclose(old);
   if (out && fclose(out) != 0)
      ok = 0;
   if (ok)
      ok = promote(tmp, r->src) == 0;
   if (!ok)
   {
      remove(tmp);
      fe_evt("save_sync_out FAILED src=\"%s\"", r->src);
      return -1;
   }
   r->src_size = written;
   r->src_crc = crc;
   if (stat_file(r->src, NULL, &r->src_mtime) != 0)
      r->src_mtime = 0;
   r->sync_crc = r->our_crc;
   fe_evt("save_sync_out ok src=\"%s\" size=%ld crc=%08x", r->src, written,
          (unsigned)crc);
   return 0;
}

/* ---- the hooks ---- */

void save_sync_link(const char *save_path, const char *src_path)
{
   ss_rec r;
   memset(&r, 0, sizeof(r));
   snprintf(r.save, sizeof(r.save), "%s", save_path);
   snprintf(r.src, sizeof(r.src), "%s", src_path);
   if (crc_file(src_path, &r.src_crc, &r.src_size) != 0 ||
       stat_file(src_path, NULL, &r.src_mtime) != 0)
      return;
   r.our_crc = 0;                      /* "not known": backed up at the start */
   if (crc_file(save_path, &r.sync_crc, NULL) != 0)
      r.sync_crc = 0;
   if (rec_put(&r) == 0)
      fe_evt("save_sync_link save=\"%s\" src=\"%s\" size=%ld crc=%08x",
             save_path, src_path, r.src_size, (unsigned)r.src_crc);
}

void save_sync_game_start(const char *save_path, char *note, size_t cap)
{
   ss_rec r;
   long ours_size = 0, ours_mtime = 0;
   uint32_t ours_crc;
   int have;

   if (note && cap)
      note[0] = 0;
   if (!save_path || !g_root[0] ||
       stat_file(save_path, &ours_size, &ours_mtime) != 0 || ours_size <= 0 ||
       crc_file(save_path, &ours_crc, NULL) != 0)
      return;
   have = rec_get(save_path, &r);
   if (!have)
   {
      memset(&r, 0, sizeof(r));
      snprintf(r.save, sizeof(r.save), "%s", save_path);
   }

   if (g_on && r.src[0])
   {
      long s_size, s_mtime;
      uint32_t s_crc;
      if (stat_file(r.src, &s_size, &s_mtime) == 0 &&
          crc_file(r.src, &s_crc, &s_size) == 0)
      {
         int src_changed = s_size != r.src_size || s_crc != r.src_crc;
         int ours_changed = have && ours_crc != r.our_crc && r.our_crc != 0;
         if (src_changed && s_size > 0 && s_crc != ours_crc)
         {
            int take = s_mtime > ours_mtime;
            fe_evt("save_sync_conflict save=\"%s\" src=\"%s\" ours_changed=%d "
                   "src_mtime=%ld ours_mtime=%ld take=%s", save_path, r.src,
                   ours_changed, s_mtime, ours_mtime, take ? "source" : "ours");
            if (take)
            {
               (void)backup_file(save_path, "replaced by a newer RetroArch save");
               if (copy_over(r.src, save_path) == 0)
               {
                  r.src_size = s_size;
                  r.src_mtime = s_mtime;
                  r.src_crc = s_crc;
                  r.our_crc = s_crc;
                  r.sync_crc = s_crc;
                  ours_crc = s_crc;
                  if (note)
                     snprintf(note, cap,
                              "Newer save from RetroArch used - kept in sync");
               }
               else
                  fe_evt("save_sync copy from source FAILED src=\"%s\"", r.src);
            }
            else
            {
               (void)backup_file(r.src, "RetroArch save older than this one");
               r.src_size = s_size;     /* the next save goes over it */
               r.src_mtime = s_mtime;
               r.src_crc = s_crc;
               r.sync_crc = 0;          /* bring RetroArch's file up to date */
               if (note)
                  snprintf(note, cap,
                           "Your save here is newer - RetroArch's old one is in backups");
            }
         }
         else if (src_changed)
         {
            /* the source holds what we hold: just note it */
            r.src_size = s_size;
            r.src_mtime = s_mtime;
            r.src_crc = s_crc;
         }
      }
   }

   if (!have || r.our_crc != ours_crc)
   {
      /* A file the app did not write: one copy before it is saved over. */
      (void)backup_file(save_path, have && r.our_crc ? "changed outside the app"
                                                    : "first time the app uses this save");
      r.our_crc = ours_crc;
   }
   (void)rec_put(&r);
   /* RetroArch's file is behind this one (a save made with sync off, or a
    * write that never got back): bring it up to date now. */
   if (g_on && r.src[0] && r.sync_crc != ours_crc)
   {
      long len = 0;
      if (stat_file(save_path, &len, NULL) == 0 && sync_out(&r, len) == 0)
         (void)rec_put(&r);
   }
}

void app_battery_written(const char *save_path)
{
   ss_rec r;
   uint32_t crc;
   long len = 0;
   int have;

   if (!save_path || !g_root[0] || crc_file(save_path, &crc, &len) != 0)
      return;
   have = rec_get(save_path, &r);
   if (!have)
   {
      memset(&r, 0, sizeof(r));
      snprintf(r.save, sizeof(r.save), "%s", save_path);
   }
   if (have && r.our_crc == crc)
      return;                          /* nothing new on disk */
   r.our_crc = crc;
   if (g_on && r.src[0])
      (void)sync_out(&r, len);
   (void)rec_put(&r);
}
