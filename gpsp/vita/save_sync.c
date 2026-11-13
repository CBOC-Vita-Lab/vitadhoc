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
static char g_racfg[SS_PATH + 48];  /* RetroArch's retroarch.cfg */
static int  g_on = 1;

static char g_ctx_rom[SS_PATH], g_ctx_save[SS_PATH];
static int  g_ctx_kind = -1;
static char g_late_note[160];

typedef struct
{
   char     save[SS_PATH];
   char     src[SS_PATH];        /* "" = none */
   long     src_size;
   long     src_mtime;
   uint32_t src_crc;
   uint32_t our_crc;
   uint32_t sync_crc;           /* our_crc when the source last got it */
   int      src_ours;           /* 1 = the source's bytes were written by us
                                 * (so they are a copy of an earlier save of
                                 * ours); 0 = they came from RetroArch and must
                                 * be backed up before they are overwritten */
   int      flags;              /* SSF_*: what the 1.4 export has settled */
} ss_rec;

/* record flags (the 9th field; 1.3 ignores what follows its 8th) */
#define SSF_HEALED   1           /* the plain copy for other emulators is dealt
                                  * with: made, linked, or kept apart on purpose */
#define SSF_CONFLICT 2           /* ...kept apart: the other file differs */

void save_sync_init(const char *app_root)
{
   snprintf(g_root, sizeof(g_root), "%s", app_root ? app_root : "");
   snprintf(g_rec_path, sizeof(g_rec_path), "%s/saves-sync.txt", g_root);
#ifdef __vita__
   snprintf(g_ra, sizeof(g_ra), "ux0:data/retroarch/savefiles");
   snprintf(g_racfg, sizeof(g_racfg), "ux0:data/retroarch/retroarch.cfg");
#else
   snprintf(g_ra, sizeof(g_ra), "%s/retroarch/savefiles", g_root);
   snprintf(g_racfg, sizeof(g_racfg), "%s/retroarch/retroarch.cfg", g_root);
#endif
   g_ctx_kind = -1;
   g_ctx_rom[0] = g_ctx_save[0] = g_late_note[0] = 0;
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

/* Why the last backup_file() failed, as text for the player and the log. */
static char g_berr[96];

static void err_errno(void)
{
   snprintf(g_berr, sizeof(g_berr), "%s", errno ? strerror(errno) : "write incomplete");
}

static void err_text(const char *t)
{
   snprintf(g_berr, sizeof(g_berr), "%s", t);
}

/* Is `p` a whole file of `size` bytes with this CRC? */
static int file_is(const char *p, uint32_t crc, long size)
{
   uint32_t c;
   long s;
   return crc_file(p, &c, &s) == 0 && c == crc && s == size;
}

static int backup_do(const char *from, const uint32_t *expect, long *size_out)
{
   char dir[SS_PATH + 16], dest[SS_PATH * 2], tmp[SS_PATH * 2 + 8];
   const char *bn = strrchr(from, '/');
   uint32_t crc, c = 0;
   long size, copied = 0;
   FILE *in, *out;
   unsigned char b[SS_CHUNK];
   size_t n;
   int ok = 1;

   bn = bn ? bn + 1 : from;
   errno = 0;
   if (crc_file(from, &crc, &size) != 0)
   {
      err_errno();
      return -1;
   }
   *size_out = size;
   if (expect && crc != *expect)
   {
      err_text("the file changed while it was being synced");
      return -1;
   }
   if (size <= 0)
      return 0;                        /* nothing in it to lose */
   snprintf(dir, sizeof(dir), "%s/backups", g_root);
   ensure_dir(dir);
   if (snprintf(dest, sizeof(dest), "%s/%s.%08x.bak", dir, bn,
                (unsigned)crc) >= (int)sizeof(dest) ||
       snprintf(tmp, sizeof(tmp), "%s.tmp", dest) >= (int)sizeof(tmp))
   {
      err_text("backup path too long");
      return -1;
   }
   if (file_is(dest, crc, size))
      return 0;                        /* already there, and whole */
   errno = 0;
   in = fopen(from, "rb");
   if (!in)
   {
      err_errno();
      return -1;
   }
   out = fopen(tmp, "wb");
   if (!out)
   {
      err_errno();
      fclose(in);
      return -1;
   }
   while ((n = fread(b, 1, sizeof(b), in)) > 0)
   {
      if (fwrite(b, 1, n, out) != n)
      {
         err_errno();
         ok = 0;
         break;
      }
      c = fe_crc32(c, b, n);
      copied += (long)n;
   }
   if (ok && ferror(in))
   {
      err_errno();
      ok = 0;
   }
   fclose(in);
   if (fclose(out) != 0 && ok)
   {
      err_errno();
      ok = 0;
   }
   if (ok && (copied != size || c != crc))
   {
      err_text("the file changed while it was being copied");
      ok = 0;
   }
   if (ok && !file_is(tmp, crc, size))
   {
      err_text("the backup did not read back whole");
      ok = 0;
   }
   if (ok && promote(tmp, dest) != 0)
   {
      err_errno();
      ok = 0;
   }
   if (ok && !file_is(dest, crc, size))
   {
      err_text("the backup did not read back whole");
      ok = 0;
   }
   if (!ok)
      remove(tmp);
   return ok ? 0 : -1;
}

/* Copy `from` into backups/ once (named by its CRC) and check the copy by
 * reading it back (size and CRC).  0 = a whole copy is there, -1 = not (the
 * reason is in g_berr).  A backup is what lets a save be overwritten: every
 * caller leaves both files alone when this fails.  `expect`, if given, is the
 * CRC the caller has decided on; a file that no longer has it is refused. */
static int backup_file(const char *from, const char *why, const uint32_t *expect)
{
   long size = 0;
   int rc = backup_do(from, expect, &size);
   if (rc != 0)
      fe_evt("save_backup FAILED file=\"%s\" size=%ld why=%s err=\"%s\"",
             from, size, why, g_berr);
   else
      fe_evt("save_backup kept file=\"%s\" size=%ld why=%s", from, size, why);
   return rc;
}

/* "<what> - nothing was overwritten (<errno text>)" into the player's note. */
static void note_fail(char *note, size_t cap, const char *what)
{
   if (note && cap && !note[0])
      snprintf(note, cap, "%s - nothing was overwritten (%s)", what, g_berr);
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
   r->src_ours = 0;                    /* absent in older records */
   r->flags = 0;
   p = strchr(f[6], '\t');
   if (p)
   {
      char *q;
      *p++ = 0;
      r->src_ours = atoi(p) ? 1 : 0;
      q = strchr(p, '\t');
      if (q)
         r->flags = atoi(q + 1);
   }
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
   if (fprintf(out, "%s\t%s\t%ld\t%ld\t%08x\t%08x\t%08x\t%d\t%d\n", r->save,
               r->src[0] ? r->src : "-", r->src_size, r->src_mtime,
               (unsigned)r->src_crc, (unsigned)r->our_crc,
               (unsigned)r->sync_crc, r->src_ours ? 1 : 0, r->flags) < 0)
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

/* from -> to (whole file, as it is), through to.import.  `crc` is what
 * `from` held when the caller chose it; a copy that is not that is dropped
 * and `to` stays as it was. */
static int copy_over(const char *from, const char *to, uint32_t crc)
{
   char tmp[SS_PATH + 16];
   unsigned char b[SS_CHUNK];
   FILE *in = fopen(from, "rb"), *out;
   size_t n;
   int ok = 1;
   snprintf(tmp, sizeof(tmp), "%s.import", to);
   errno = in ? 0 : errno;
   out = in ? fopen(tmp, "wb") : NULL;
   if (!in || !out)
      ok = 0;
   while (ok && (n = fread(b, 1, sizeof(b), in)) > 0)
      if (fwrite(b, 1, n, out) != n)
         ok = 0;
   if (ok && ferror(in))
      ok = 0;
   if (!ok)
      err_errno();
   if (in)
      fclose(in);
   if (out && fclose(out) != 0 && ok)
   {
      err_errno();
      ok = 0;
   }
   if (ok)
   {
      uint32_t c;
      ok = crc_file(tmp, &c, NULL) == 0 && c == crc;
      if (!ok)
         err_text("the copy did not read back whole");
   }
   if (ok && promote(tmp, to) != 0)
   {
      err_errno();
      ok = 0;
   }
   if (!ok)
      remove(tmp);
   return ok ? 0 : -1;
}

/* The core length of a battery file of `size` bytes: the standard sizes a
 * trailer may follow (RetroArch/mGBA: 131072 + 16 and so on). */
static long core_len(long size)
{
   static const long base[] = { 131072, 65536, 32768, 8192, 2048, 512 };
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
   /* RetroArch's own bytes are kept before the first time we write over
    * them.  Without a whole, verified copy the source is left as it is and
    * the write is tried again at the next start. */
   if (!r->src_ours &&
       backup_file(r->src, "RetroArch save about to be written over", &cur_crc) != 0)
   {
      fe_evt("save_sync_out DEFERRED: no backup of src=\"%s\"", r->src);
      return -2;
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
   r->src_ours = 1;
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
   int have, deferred = 0;

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
         int ours_known = have && r.our_crc != 0;
         int ours_changed = ours_known && ours_crc != r.our_crc;
         if (src_changed && s_size > 0 && s_crc != ours_crc)
         {
            /* Equal times: if only the source changed it is the newer one;
             * if both may have changed there is nothing to pick by. */
            int tie = s_mtime == ours_mtime;
            int both = ours_changed || !ours_known;
            int take = s_mtime > ours_mtime || (tie && !both);
            fe_evt("save_sync_conflict save=\"%s\" src=\"%s\" ours_changed=%d "
                   "src_mtime=%ld ours_mtime=%ld take=%s", save_path, r.src,
                   ours_changed, s_mtime, ours_mtime,
                   tie && both ? "neither" : take ? "source" : "ours");
            if (tie && both)
            {
               /* Keep both, overwrite nothing, and ask again at the next
                * start: the record is left as it was. */
               int a = backup_file(save_path, "both saves changed at the same time",
                                   &ours_crc);
               int c = backup_file(r.src, "both saves changed at the same time",
                                   &s_crc);
               deferred = 1;
               if (a == 0 && c == 0)
               {
                  if (note)
                     snprintf(note, cap, "Both saves changed at the same time - "
                              "kept both, copies are in backups, nothing overwritten");
               }
               else
                  note_fail(note, cap, "Both saves changed at the same time and "
                            "could not be backed up");
            }
            else if (take)
            {
               if (backup_file(save_path, "replaced by a newer RetroArch save",
                               &ours_crc) != 0)
               {
                  deferred = 1;
                  note_fail(note, cap, "A newer RetroArch save was not used: "
                            "yours could not be backed up");
               }
               else if (copy_over(r.src, save_path, s_crc) == 0)
               {
                  r.src_size = s_size;
                  r.src_mtime = s_mtime;
                  r.src_crc = s_crc;
                  r.src_ours = 0;
                  r.our_crc = s_crc;
                  r.sync_crc = s_crc;
                  ours_crc = s_crc;
                  if (note)
                     snprintf(note, cap,
                              "Newer save from RetroArch used - kept in sync");
               }
               else
               {
                  deferred = 1;
                  fe_evt("save_sync copy from source FAILED src=\"%s\"", r.src);
                  if (note)
                     snprintf(note, cap, "RetroArch's newer save could not be "
                              "copied - nothing was overwritten (%s)", g_berr);
               }
            }
            else
            {
               if (backup_file(r.src, "RetroArch save older than this one",
                               &s_crc) != 0)
               {
                  deferred = 1;      /* RetroArch's file is left alone */
                  note_fail(note, cap, "RetroArch's older save could not be "
                            "backed up");
               }
               else
               {
                  r.src_size = s_size;     /* the next save goes over it */
                  r.src_mtime = s_mtime;
                  r.src_crc = s_crc;
                  r.src_ours = 0;
                  r.sync_crc = 0;          /* bring RetroArch's file up to date */
                  if (note)
                     snprintf(note, cap,
                              "Your save here is newer - RetroArch's old one is in backups");
               }
            }
         }
         else if (src_changed)
         {
            /* the source holds what we hold: just note it */
            r.src_size = s_size;
            r.src_mtime = s_mtime;
            r.src_crc = s_crc;
            r.src_ours = 0;
         }
      }
   }

   if (!deferred && (!have || r.our_crc != ours_crc))
   {
      /* A file the app did not write: one copy before it is saved over.  If
       * the copy cannot be made, the record keeps "not known" so the next
       * start tries again. */
      if (backup_file(save_path, have && r.our_crc ? "changed outside the app"
                                                  : "first time the app uses this save",
                      &ours_crc) == 0)
         r.our_crc = ours_crc;
      else
         note_fail(note, cap, "Your save could not be backed up");
   }
   (void)rec_put(&r);
   /* RetroArch's file is behind this one (a save made with sync off, or a
    * write that never got back): bring it up to date now. */
   if (!deferred && g_on && r.src[0] && r.sync_crc != ours_crc)
   {
      long len = 0;
      if (stat_file(save_path, &len, NULL) == 0)
      {
         int rc = sync_out(&r, len);
         if (rc == 0)
            (void)rec_put(&r);
         else if (rc == -2)
            note_fail(note, cap, "RetroArch's save could not be backed up");
      }
   }
}

/* ======================================================================
 * 1.4: a plain copy of every save for RetroArch and other emulators
 *
 * 1.0 - 1.2 never wrote one (their battery file is Game.sav / Game.gb.sav /
 * Game.gbc.sav beside the game, a name RetroArch does not read), and 1.3 only
 * wrote back to a RetroArch file the save had been TAKEN from.  A save with no
 * record of one (every save made in 1.0 - 1.2, a new game, a copy taken from
 * import/ or from beside the game) was never written anywhere RetroArch looks.
 * save_sync_heal_game() does that once per game, at its start (and after the
 * first save of a new game), by these rules:
 *
 *  - the file is the app's save byte for byte: it already is the layout the
 *    cores load (GBA: the 128 KB gpSP image; Game Boy: cartridge RAM and, for a
 *    clock cartridge, the 48 byte trailer of mGBA, SameBoy and VBA-M; SNES: raw
 *    SRAM) -- nothing is converted, so nothing can be converted wrongly;
 *  - an existing RetroArch file of that game (its savefiles folder, a core's
 *    folder in it, or beside the game) is never replaced unless it is older
 *    than ours and a verified copy of it is in backups/; when it is as new or
 *    newer and differs, both are kept and the record says so;
 *  - where a new file goes follows RetroArch's own configuration.
 * ====================================================================== */

typedef struct
{
   int  have;
   int  in_content;                    /* savefiles_in_content_dir */
   int  sort_core;                     /* sort_savefiles_enable */
   int  sort_content;                  /* sort_savefiles_by_content_enable */
   char dir[SS_PATH];                  /* savefile_directory, "" = none/default */
} ra_cfg;

static void cfg_val(const char *line, const char *key, char *out, size_t cap)
{
   size_t kl = strlen(key);
   const char *p = line;
   while (*p == ' ' || *p == '\t')
      p++;
   if (strncmp(p, key, kl))
      return;
   p += kl;
   while (*p == ' ' || *p == '\t')
      p++;
   if (*p != '=')
      return;
   p++;
   while (*p == ' ' || *p == '\t' || *p == '"')
      p++;
   snprintf(out, cap, "%s", p);
   {
      size_t n = strlen(out);
      while (n && (out[n - 1] == '\n' || out[n - 1] == '\r' ||
                   out[n - 1] == '"' || out[n - 1] == ' '))
         out[--n] = 0;
   }
}

static void ra_cfg_read(ra_cfg *c)
{
   FILE *f = fopen(g_racfg, "rb");
   char line[SS_PATH + 64], v[SS_PATH];
   memset(c, 0, sizeof(*c));
   if (!f)
      return;
   c->have = 1;
   while (fgets(line, sizeof(line), f))
   {
      v[0] = 0;
      cfg_val(line, "savefiles_in_content_dir", v, sizeof(v));
      if (v[0]) c->in_content = !strcmp(v, "true");
      v[0] = 0;
      cfg_val(line, "sort_savefiles_enable", v, sizeof(v));
      if (v[0]) c->sort_core = !strcmp(v, "true");
      v[0] = 0;
      cfg_val(line, "sort_savefiles_by_content_enable", v, sizeof(v));
      if (v[0]) c->sort_content = !strcmp(v, "true");
      v[0] = 0;
      cfg_val(line, "savefile_directory", v, sizeof(v));
      if (v[0] && strcmp(v, "default") && v[0] != ':' && v[0] != '~' &&
          (strchr(v, ':') || v[0] == '/'))
         snprintf(c->dir, sizeof(c->dir), "%s", v);
   }
   fclose(f);
}

static int is_dir(const char *p)
{
   struct stat st;
   return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static int exists(const char *p)
{
   struct stat st;
   return stat(p, &st) == 0;
}

/* dst = src when it fits SS_PATH, else -1 (a record path is never cut) */
static int set_path(char *dst, const char *src)
{
   size_t n = strlen(src);
   if (n >= SS_PATH)
      return -1;
   memcpy(dst, src, n + 1);
   return 0;
}

static const char *kind_name(int k)
{
   static const char *const n[] = { "gba", "gb", "gbc", "snes", "nes", "wswan" };
   return k >= 0 && k < 6 ? n[k] : "?";
}

/* "<dir of the game>" into dir (with its trailing slash), and the game's name
 * without its extension into stem. */
static int rom_parts(const char *rom, char *dir, size_t dcap, char *stem, size_t scap)
{
   const char *sl = strrchr(rom, '/'), *c = strrchr(rom, ':'), *bn, *dot;
   size_t dl;
   if (c && (!sl || c > sl))
      sl = c;
   bn = sl ? sl + 1 : rom;
   dl = (size_t)(bn - rom);
   if (dl >= dcap || !*bn)
      return -1;
   memcpy(dir, rom, dl);
   dir[dl] = 0;
   snprintf(stem, scap, "%s", bn);
   dot = strrchr(stem, '.');
   if (dot && dot != stem)
      stem[dot - stem] = 0;
   return stem[0] ? 0 : -1;
}

/* RetroArch's per-core folders, for the console, in the order to look. */
static const char *const k_sub_gb[] = { "", "/Gambatte", "/mGBA", "/SameBoy", "/TGB Dual",
   "/Gearboy", "/VBA-M", "/VBA Next", "/gpSP", NULL };
static const char *const k_sub_gba[] = { "", "/gpSP", "/mGBA", "/VBA-M", "/VBA Next", NULL };
static const char *const k_sub_snes[] = { "", "/Snes9x", "/Snes9x 2010", "/Snes9x 2005",
   "/bsnes", "/bsnes-mercury Balanced", NULL };
static const char *const k_sub_other[] = { "", "/FCEUmm", "/Nestopia", "/Mesen",
   "/QuickNES", "/Beetle WonderSwan", "/Beetle Cygne", NULL };

static const char *const *subs_for(int kind)
{
   return kind == 0 ? k_sub_gba : kind == 1 || kind == 2 ? k_sub_gb :
          kind == 3 ? k_sub_snes : k_sub_other;
}

/* Is the data of the two files the same save?  Same bytes as far as both go;
 * what is left over is only a clock trailer (at most 64 bytes) or erased
 * (0xFF) -- a 32 KB copy of a 128 KB image, a trailer RetroArch wrote with
 * other clock bytes. */
static int same_save(const char *a, const char *b)
{
   unsigned char x[SS_CHUNK], y[SS_CHUNK];
   long sa = 0, sb = 0, i, cmp;
   FILE *fa, *fb;
   int same = 1;
   if (stat_file(a, &sa, NULL) != 0 || stat_file(b, &sb, NULL) != 0)
      return 0;
   cmp = core_len(sa) < core_len(sb) ? core_len(sa) : core_len(sb);
   fa = fopen(a, "rb");
   fb = fopen(b, "rb");
   if (!fa || !fb)
      same = 0;
   for (i = 0; same && i < cmp; )
   {
      size_t want = (size_t)(cmp - i > (long)sizeof(x) ? (long)sizeof(x) : cmp - i);
      if (fread(x, 1, want, fa) != want || fread(y, 1, want, fb) != want ||
          memcmp(x, y, want))
         same = 0;
      i += (long)want;
   }
   if (fa)
      fclose(fa);
   if (fb)
      fclose(fb);
   if (!same)
      return 0;
   if (sa - cmp > 64 && has_data_between(a, cmp, sa))
      return 0;
   if (sb - cmp > 64 && has_data_between(b, cmp, sb))
      return 0;
   return 1;
}

/* An existing RetroArch file of this game, wherever RetroArch could have put
 * it: its configured folder (and the core folders in it), the default folder,
 * beside the game.  `skip` (our own save) is never one. */
static int find_foreign(const char *skip, const char *rom_dir, const char *stem,
                        int kind, const ra_cfg *cfg, char *out, size_t cap)
{
   const char *bases[2];
   const char *const *sub = subs_for(kind);
   char p[SS_PATH * 2];
   int b, i;
   bases[0] = cfg->dir[0] ? cfg->dir : g_ra;
   bases[1] = cfg->dir[0] && strcmp(cfg->dir, g_ra) ? g_ra : NULL;
   for (b = 0; b < 2; b++)
   {
      if (!bases[b])
         continue;
      for (i = 0; sub[i]; i++)
      {
         if (snprintf(p, sizeof(p), "%s%s/%s.srm", bases[b], sub[i], stem) < (int)sizeof(p) &&
             strcmp(p, skip) && exists(p) && strlen(p) < cap)
         {
            snprintf(out, cap, "%s", p);
            return 1;
         }
      }
   }
   if (snprintf(p, sizeof(p), "%s%s.srm", rom_dir, stem) < (int)sizeof(p) &&
       strcmp(p, skip) && exists(p) && strlen(p) < cap)
   {
      snprintf(out, cap, "%s", p);
      return 1;
   }
   return 0;
}

/* Where a NEW plain copy goes.  Returns a short name of the rule used. */
static const char *pick_target(const char *save_path, const char *rom_dir,
                               const char *stem, int kind, const ra_cfg *cfg,
                               char *out, size_t cap)
{
   char base[SS_PATH + 48], ra_root[SS_PATH + 48];
   const char *rule;
   const char *const *sub = subs_for(kind);
   int i;
   out[0] = 0;
   snprintf(ra_root, sizeof(ra_root), "%s", g_ra);
   {
      char *sl = strrchr(ra_root, '/');
      if (sl)
         *sl = 0;                      /* RetroArch's own folder */
   }
   if (cfg->have && cfg->in_content && !cfg->dir[0])
   {
      snprintf(out, cap, "%s%s.srm", rom_dir, stem);
      rule = "retroarch-config:beside-game";
   }
   else if (cfg->dir[0] || is_dir(g_ra) || is_dir(ra_root))
   {
      snprintf(base, sizeof(base), "%s", cfg->dir[0] ? cfg->dir : g_ra);
      if (cfg->sort_core)
      {
         for (i = 1; sub[i]; i++)
         {
            char d[SS_PATH * 2];
            if (snprintf(d, sizeof(d), "%s%s", base, sub[i]) < (int)sizeof(d) && is_dir(d))
            {
               snprintf(out, cap, "%s/%s.srm", d, stem);
               return "retroarch-core-folder";
            }
         }
      }
      snprintf(out, cap, "%s/%s.srm", base, stem);
      rule = cfg->dir[0] ? "retroarch-config:savefile_directory" : "retroarch-savefiles";
   }
   else
   {
      /* No RetroArch on the card: the plain .sav beside the game.  A GBA
       * game's save already is that (Game.sav); a Game Boy game's is named
       * Game.gb.sav, so it gets Game.sav -- unless a GBA game beside it owns
       * that name.  SNES, NES and WonderSwan saves already are Game.srm. */
      if (kind == 1 || kind == 2)
      {
         char a[SS_PATH * 2], z[SS_PATH * 2];
         if (snprintf(a, sizeof(a), "%s%s.gba", rom_dir, stem) < (int)sizeof(a) &&
             !exists(a) &&
             snprintf(z, sizeof(z), "%s%s.zip", rom_dir, stem) < (int)sizeof(z) &&
             !exists(z))
         {
            snprintf(out, cap, "%s%s.sav", rom_dir, stem);
            rule = "no-retroarch:beside-game-sav";
         }
         else
         {
            snprintf(out, cap, "%s%s.srm", rom_dir, stem);
            rule = "no-retroarch:beside-game-srm";
         }
      }
      else
      {
         snprintf(out, cap, "%s", save_path);   /* already the plain file */
         rule = "no-retroarch:save-is-plain";
      }
   }
   return rule;
}

static void ensure_parent(const char *path)
{
   char d[SS_PATH * 2];
   char *sl;
   snprintf(d, sizeof(d), "%s", path);
   sl = strrchr(d, '/');
   if (sl)
   {
      *sl = 0;
      ensure_dir(d);
   }
}

/* One census line per launch: never silent. */
static void census(const char *save, int kind, long size, uint32_t crc,
                   const char *found, const char *exported, const char *backup,
                   const char *skipped, const char *rule)
{
   /* reasons first: the log line is cut at 512 bytes and paths are long */
   fe_evt("save_census kind=%s bytes=%ld crc=%08x skipped=\"%s\" rule=%s backup=%s "
          "save=\"%s\" found=\"%s\" exported=\"%s\"", kind_name(kind), size,
          (unsigned)crc, skipped ? skipped : "-", rule ? rule : "-",
          backup ? backup : "none", save, found ? found : "-",
          exported ? exported : "-");
}

static void put_flags(ss_rec *r, int flags)
{
   r->flags |= flags;
   (void)rec_put(r);
}

/* Remove what an export cut short left behind: <target>.import, and put a
 * RetroArch file that was being replaced back (<target>.old). */
static void export_recover(const char *t)
{
   char a[SS_PATH * 2 + 16];
   if (snprintf(a, sizeof(a), "%s.import", t) < (int)sizeof(a) && exists(a))
   {
      remove(a);
      fe_evt("save_export removed an unfinished copy \"%s\"", a);
   }
   if (snprintf(a, sizeof(a), "%s.old", t) < (int)sizeof(a) && exists(a))
   {
      if (!exists(t) && rename(a, t) == 0)
         fe_evt("save_export put back \"%s\" (a replacement was cut short)", t);
   }
}

int save_sync_heal_game(const char *save_path, const char *rom_path, int kind,
                        char *note, size_t cap)
{
   ss_rec r;
   ra_cfg cfg;
   char rom_dir[SS_PATH], stem[SS_PATH], found[SS_PATH], target[SS_PATH * 2 + 16];
   long ours_len = 0, ours_mtime = 0;
   uint32_t ours_crc = 0;
   const char *rule;
   int have;

   if (note && cap)
      note[0] = 0;
   if (!save_path || !rom_path || !g_root[0])
      return 0;
   if (stat_file(save_path, &ours_len, &ours_mtime) != 0 || ours_len <= 0)
   {
      census(save_path, kind, 0, 0, NULL, NULL, NULL, "no save yet", NULL);
      return 0;
   }
   have = rec_get(save_path, &r);
   if (!have)
   {
      memset(&r, 0, sizeof(r));
      snprintf(r.save, sizeof(r.save), "%s", save_path);
   }
   if (r.flags & SSF_HEALED)
   {
      /* settled: one stat tells whether the linked file is still there */
      if (r.src[0] && !exists(r.src))
         census(save_path, kind, ours_len, 0, r.src, NULL, NULL,
                "RetroArch file was removed; not made again", NULL);
      else
         census(save_path, kind, ours_len, r.our_crc, r.src[0] ? r.src : "-",
                r.src[0] ? r.src : NULL, NULL,
                r.flags & SSF_CONFLICT ? "kept apart: the files differ" : "already done", NULL);
      return 0;
   }
   if (!g_on)
   {
      census(save_path, kind, ours_len, 0, NULL, NULL, NULL,
             "Sync RetroArch saves is off", NULL);
      return 0;
   }
   if (rom_parts(rom_path, rom_dir, sizeof(rom_dir), stem, sizeof(stem)) != 0 ||
       crc_file(save_path, &ours_crc, &ours_len) != 0)
   {
      census(save_path, kind, ours_len, 0, NULL, NULL, NULL, "cannot read the save or the game name", NULL);
      return -1;
   }
   ra_cfg_read(&cfg);

   /* A record that names a source: 1.3's own business, as long as it is there. */
   if (r.src[0])
   {
      if (exists(r.src))
      {
         r.flags |= SSF_HEALED;
         (void)rec_put(&r);
         census(save_path, kind, ours_len, ours_crc, r.src, r.src, NULL, "linked already", NULL);
         return 0;
      }
      census(save_path, kind, ours_len, ours_crc, r.src, NULL, NULL,
             "the linked RetroArch file is gone; looking again", NULL);
      r.src[0] = 0;
   }

   /* an export or a replacement that was cut short last time is put right
    * BEFORE the RetroArch file is looked for: a file waiting as <name>.old is
    * RetroArch's own and goes back where it was, never written over */
   rule = pick_target(save_path, rom_dir, stem, kind, &cfg, target, sizeof(target));
   if (strcmp(target, save_path))
      export_recover(target);
   if (find_foreign(save_path, rom_dir, stem, kind, &cfg, found, sizeof(found)))
   {
      long f_size = 0, f_mtime = 0;
      uint32_t f_crc = 0;
      export_recover(found);
      if (stat_file(found, &f_size, &f_mtime) != 0 ||
          crc_file(found, &f_crc, &f_size) != 0)
      {
         census(save_path, kind, ours_len, ours_crc, found, NULL, NULL, "cannot read it", NULL);
         return -1;
      }
      if (same_save(save_path, found))
      {
         snprintf(r.src, sizeof(r.src), "%s", found);
         r.src_size = f_size; r.src_mtime = f_mtime; r.src_crc = f_crc;
         r.src_ours = 0; r.our_crc = ours_crc; r.sync_crc = ours_crc;
         r.flags |= SSF_HEALED;
         (void)rec_put(&r);
         census(save_path, kind, ours_len, ours_crc, found, found, NULL,
                "RetroArch's file is the same save: linked, nothing written", NULL);
         return 2;
      }
      if (f_mtime >= ours_mtime)
      {
         int a = backup_file(save_path, "RetroArch has a different save of this game", &ours_crc);
         int c = backup_file(found, "RetroArch has a different save of this game", &f_crc);
         if (a != 0 || c != 0)
         {
            note_fail(note, cap, "Your save and RetroArch's differ and could not be backed up");
            census(save_path, kind, ours_len, ours_crc, found, NULL, "FAILED",
                   "no verified backup: retry at the next start", NULL);
            return -1;
         }
         r.our_crc = ours_crc;
         put_flags(&r, SSF_HEALED | SSF_CONFLICT);
         if (note)
            snprintf(note, cap, "Your save here and RetroArch's save (%s) differ - "
                     "both kept, nothing overwritten", found);
         census(save_path, kind, ours_len, ours_crc, found, NULL, "both",
                f_mtime > ours_mtime ? "RetroArch's is newer and differs" :
                                       "same time and different", NULL);
         return 3;
      }
      /* RetroArch's is older and differs: after a verified backup of it, ours
       * goes over it, in its own size and layout (sync_out). */
      snprintf(r.src, sizeof(r.src), "%s", found);
      r.src_size = f_size; r.src_mtime = f_mtime; r.src_crc = f_crc;
      r.src_ours = 0; r.our_crc = ours_crc; r.sync_crc = 0;
      {
         int rc = sync_out(&r, ours_len);
         if (rc == 0)
         {
            r.flags |= SSF_HEALED;
            (void)rec_put(&r);
            if (note)
               snprintf(note, cap, "Save is now also at %s, RetroArch can use it "
                        "(its older one is in backups)", found);
            census(save_path, kind, ours_len, ours_crc, found, found, "kept", "-", NULL);
            return 1;
         }
         if (rc == 1)
         {
            int a = backup_file(save_path, "RetroArch has a different save of this game", &ours_crc);
            int c = backup_file(found, "RetroArch has a different save of this game", &f_crc);
            r.src[0] = 0;
            if (a == 0 && c == 0)
            {
               r.our_crc = ours_crc;
               put_flags(&r, SSF_HEALED | SSF_CONFLICT);
               if (note)
                  snprintf(note, cap, "Your save here and RetroArch's save (%s) differ "
                           "- both kept, nothing overwritten", found);
            }
            census(save_path, kind, ours_len, ours_crc, found, NULL,
                   a == 0 && c == 0 ? "both" : "FAILED", "the files do not fit each other", NULL);
            return 3;
         }
         if (rc == -2)
            note_fail(note, cap, "RetroArch's save could not be backed up");
         census(save_path, kind, ours_len, ours_crc, found, NULL, rc == -2 ? "FAILED" : "none",
                rc == -2 ? "no verified backup: retry at the next start"
                         : "write failed: retry at the next start", NULL);
         return -1;
      }
   }

   if (!strcmp(target, save_path))
   {
      r.our_crc = ours_crc;
      put_flags(&r, SSF_HEALED);
      census(save_path, kind, ours_len, ours_crc, save_path, save_path, NULL, NULL, rule);
      return 0;
   }
   ensure_parent(target);
   if (copy_over(save_path, target, ours_crc) != 0)
   {
      note_fail(note, cap, "A copy of your save for RetroArch could not be written");
      fe_evt("save_export FAILED target=\"%s\" rule=%s err=\"%s\"", target, rule, g_berr);
      census(save_path, kind, ours_len, ours_crc, NULL, NULL, NULL,
             "write failed: nothing changed, retry at the next start", NULL);
      return -1;
   }
   if (set_path(r.src, target) != 0) r.src[0] = 0;
   r.src_size = ours_len; r.src_crc = ours_crc;
   if (stat_file(target, NULL, &r.src_mtime) != 0)
      r.src_mtime = 0;
   r.src_ours = 1;
   r.our_crc = ours_crc;
   r.sync_crc = ours_crc;
   r.flags |= SSF_HEALED;
   (void)rec_put(&r);
   if (note)
      snprintf(note, cap, "Save is now also at %s, RetroArch can use it", target);
   fe_evt("save_export ok target=\"%s\" rule=%s", target, rule);
   census(save_path, kind, ours_len, ours_crc, NULL, target, "-", NULL, rule);
   return 1;
}

/* Files of an older version that are still beside the game although the game's
 * own folder (states/...) has the same file: the migration leaves both
 * alone ("kept"), and the old one would stay in the game folder for good.  A
 * verified copy goes to <data>/leftovers/ and the old file is removed; the
 * file in the game's folder is never touched, and an old file with no twin
 * stays where it is (it is what the game reads).  `old1` / `new1` are the
 * slot-1 state paths, beside the game and in the game's folder.  Returns the
 * number of files moved. */
int save_sync_tidy_beside_rom(const char *old1, const char *new1)
{
   static const char *const tail[] = { "", ".thumb", ".shot", ".p2", ".old" };
   static const char *const rtail[] = { "", ".pt", ".old" };
   char ba[SS_PATH], bb[SS_PATH], a[SS_PATH + 24], b[SS_PATH + 24], dir[SS_PATH + 16];
   size_t la, lb;
   int moved = 0, slot, t;

   if (!old1 || !new1 || !g_root[0])
      return 0;
   la = strlen(old1);
   lb = strlen(new1);
   if (la < 4 || lb < 4 || la >= sizeof(ba) || lb >= sizeof(bb) ||
       strcmp(old1 + la - 3, "st0") || strcmp(new1 + lb - 3, "st0") ||
       !strcmp(old1, new1))
      return 0;
   memcpy(ba, old1, la - 3); ba[la - 3] = 0;
   memcpy(bb, new1, lb - 3); bb[lb - 3] = 0;
   snprintf(dir, sizeof(dir), "%s/leftovers", g_root);
   for (slot = 0; slot < 11; slot++)
   {
      int nt = slot < 10 ? 5 : 3;
      for (t = 0; t < nt; t++)
      {
         const char *tl = slot < 10 ? tail[t] : rtail[t];
         char nm[40];
         uint32_t crc;
         long size;
         if (slot < 10)
            snprintf(nm, sizeof(nm), "st%d%s", slot, tl);
         else
            snprintf(nm, sizeof(nm), "rsm%s", tl);
         if (snprintf(a, sizeof(a), "%s%s", ba, nm) >= (int)sizeof(a) ||
             snprintf(b, sizeof(b), "%s%s", bb, nm) >= (int)sizeof(b) ||
             !exists(a) || !exists(b))
            continue;
         if (crc_file(a, &crc, &size) == 0)
         {
            char to[SS_PATH * 2 + 32];
            const char *bn = strrchr(a, '/');
            bn = bn ? bn + 1 : a;
            ensure_dir(dir);
            if (snprintf(to, sizeof(to), "%s/%s.%08x", dir, bn, (unsigned)crc) <
                   (int)sizeof(to) &&
                (file_is(to, crc, size) || copy_over(a, to, crc) == 0) &&
                file_is(to, crc, size) && remove(a) == 0)
            {
               moved++;
               fe_evt("save_tidy moved \"%s\" to leftovers (the game's folder has its own)", a);
            }
            else
               fe_evt("save_tidy kept \"%s\" beside the game (could not be copied away)", a);
         }
      }
   }
   return moved;
}

void save_sync_set_game(const char *save_path, const char *rom_path, int kind)
{
   g_late_note[0] = 0;
   if (!save_path || !rom_path)
   {
      g_ctx_kind = -1;
      return;
   }
   snprintf(g_ctx_save, sizeof(g_ctx_save), "%s", save_path);
   snprintf(g_ctx_rom, sizeof(g_ctx_rom), "%s", rom_path);
   g_ctx_kind = kind;
}

int save_sync_take_note(char *note, size_t cap)
{
   if (!note || !cap || !g_late_note[0])
      return 0;
   snprintf(note, cap, "%s", g_late_note);
   g_late_note[0] = 0;
   return 1;
}

/* "Export save for other emulators": the save as a plain file where the
 * player chose, whatever the setting says, after a verified backup of any file
 * it replaces. */
int save_sync_export_now(const char *save_path, const char *rom_path, int kind,
                         int dest, char *where, size_t wcap, char *note, size_t cap)
{
   ra_cfg cfg;
   char rom_dir[SS_PATH], stem[SS_PATH], target[SS_PATH * 2 + 16];
   long ours_len = 0;
   uint32_t ours_crc = 0, t_crc;
   long t_size;
   ss_rec r;
   int have;

   if (note && cap)
      note[0] = 0;
   if (where && wcap)
      where[0] = 0;
   if (!save_path || !rom_path || !g_root[0] ||
       rom_parts(rom_path, rom_dir, sizeof(rom_dir), stem, sizeof(stem)) != 0 ||
       crc_file(save_path, &ours_crc, &ours_len) != 0 || ours_len <= 0)
   {
      if (note && cap)
         snprintf(note, cap, "This game has no save yet");
      return -1;
   }
   ra_cfg_read(&cfg);
   if (dest == SS_DEST_ROM_SAV)
   {
      char a[SS_PATH * 2];
      if (kind == 0 || kind >= 3)
         snprintf(target, sizeof(target), "%s", save_path);   /* it is that file already */
      else if (snprintf(a, sizeof(a), "%s%s.gba", rom_dir, stem) < (int)sizeof(a) && exists(a))
      {
         if (note && cap)
            snprintf(note, cap, "%s.sav belongs to the GBA game of the same name", stem);
         return -1;
      }
      else
         snprintf(target, sizeof(target), "%s%s.sav", rom_dir, stem);
   }
   else
   {
      char found[SS_PATH];
      if (find_foreign(save_path, rom_dir, stem, kind, &cfg, found, sizeof(found)))
         snprintf(target, sizeof(target), "%s", found);
      else
         (void)pick_target(save_path, rom_dir, stem, kind, &cfg, target, sizeof(target));
   }
   if (where && wcap)
      snprintf(where, wcap, "%s", target);
   if (!strcmp(target, save_path))
   {
      if (note && cap)
         snprintf(note, cap, "The save is already a plain file: %s", target);
      return 0;
   }
   export_recover(target);
   if (crc_file(target, &t_crc, &t_size) == 0)
   {
      if (t_crc == ours_crc && t_size == ours_len)
      {
         if (note && cap)
            snprintf(note, cap, "Already there: %s", target);
         return 0;
      }
      if (backup_file(target, "replaced by Export save", &t_crc) != 0)
      {
         if (note && cap)
            snprintf(note, cap, "Not exported: the file there could not be backed up (%s)", g_berr);
         return -1;
      }
   }
   ensure_parent(target);
   if (copy_over(save_path, target, ours_crc) != 0)
   {
      if (note && cap)
         snprintf(note, cap, "Not exported (%s) - nothing was changed", g_berr);
      return -1;
   }
   if (dest == SS_DEST_RETROARCH)
   {
      have = rec_get(save_path, &r);
      if (!have)
      {
         memset(&r, 0, sizeof(r));
         snprintf(r.save, sizeof(r.save), "%s", save_path);
      }
      if (set_path(r.src, target) != 0) r.src[0] = 0;
      r.src_size = ours_len; r.src_crc = ours_crc;
      if (stat_file(target, NULL, &r.src_mtime) != 0)
         r.src_mtime = 0;
      r.src_ours = 1; r.our_crc = ours_crc; r.sync_crc = ours_crc;
      r.flags |= SSF_HEALED;
      r.flags &= ~SSF_CONFLICT;
      (void)rec_put(&r);
   }
   fe_evt("save_export ok target=\"%s\" rule=manual bytes=%ld crc=%08x", target, ours_len,
          (unsigned)ours_crc);
   if (note && cap)
      snprintf(note, cap, "Saved a plain copy: %s", target);
   return 0;
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
   /* a save with no RetroArch file yet (a new game, or one that could not be
    * dealt with at the start): its plain copy, now that there is something */
   if (g_on && g_ctx_kind >= 0 && !r.src[0] && !(r.flags & SSF_HEALED) &&
       !strcmp(g_ctx_save, save_path))
   {
      char n[160];
      if (save_sync_heal_game(save_path, g_ctx_rom, g_ctx_kind, n, sizeof(n)) == 1 &&
          n[0])
         snprintf(g_late_note, sizeof(g_late_note), "%s", n);
   }
}
