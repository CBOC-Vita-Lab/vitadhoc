/* cover_custom.c -- see cover_custom.h. */
#include "cover_custom.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "artfetch.h"
#include "stb_image.h"
#include "stb_image_write.h"

#define CV_PATH      600
#define CV_STEM      256
#define CV_MAX_FILE  (24u * 1024u * 1024u)    /* the encoded picture        */
#define CV_MAX_PIX   (8u * 1024u * 1024u)     /* decoded: 8 megapixels      */
#define CV_INBOX_MAX 4096

static char g_root[CV_PATH - 100];

static int inbox_current(const char *stem, char *seen, size_t cap);

void cover_init(const char *root)
{
   static const char *const sub[] = { "cache", "cache/covers", "cache/boxart" };
   char p[CV_PATH];
   unsigned i;
   snprintf(g_root, sizeof(g_root), "%s", root ? root : "");
   for (i = 0; g_root[0] && i < sizeof(sub) / sizeof(sub[0]); i++)
      if (snprintf(p, sizeof(p), "%s/%s", g_root, sub[i]) < (int)sizeof(p))
         mkdir(p, 0777);
}

/* ---- paths ---------------------------------------------------------------- */

/* boxart/ holds pictures only (the player's own and downloaded ones).  The
 * records (.own .auto), the kept downloaded cover (.dl) and the app's decoded
 * copies (.v565 .vh565, made by ui_psp.c) live under cache/ (VitAdhoc 1.3:
 * no app files among the player's pictures). */
static int art_path(char *out, size_t cap, const char *stem, const char *ext)
{
   const char *dir = "boxart";
   int n;
   if (!strcmp(ext, "own") || !strcmp(ext, "auto") || !strcmp(ext, "dl"))
      dir = "cache/covers";
   else if (!strcmp(ext, "v565") || !strcmp(ext, "vh565"))
      dir = "cache/boxart";
   n = snprintf(out, cap, "%s/%s/%s.%s", g_root, dir, stem, ext);
   return n > 0 && (size_t)n < cap ? 0 : -1;
}

int cover_thumb_path(const char *stem, char *out, size_t cap)
{
   int n = snprintf(out, cap, "%s/" ART_THUMB_DIR "/%s%s", g_root, stem,
                    ART_THUMB_SUFFIX);
   return n > 0 && (size_t)n < cap ? 0 : -1;
}

static int exists(const char *path)
{
   struct stat st;
   return stat(path, &st) == 0;
}

static void drop(const char *path)
{
   remove(path);
}

/* tmp -> dst, replacing dst.  The Vita's card refuses a rename over an
 * existing file, so the old one steps aside first and comes back if the new
 * one cannot be put in place. */
static int put_file(const char *tmp, const char *dst)
{
   char old[CV_PATH + 8];
   int had = exists(dst);
   if (had)
   {
      snprintf(old, sizeof(old), "%s.old", dst);
      drop(old);
      if (rename(dst, old) != 0)
         return -1;
   }
   if (rename(tmp, dst) != 0)
   {
      if (had)
         rename(old, dst);
      return -1;
   }
   if (had)
      drop(old);
   return 0;
}

/* The decoded caches of the old picture: the next draw makes them again from
 * the new one.  Only the Vita's own cache names; a picture of anyone's
 * (.png .jpg .bmp .565) is never touched here. */
static void purge_caches(const char *stem)
{
   static const char *const ext[] = { "v565", "vh565" };
   char p[CV_PATH];
   unsigned i;
   for (i = 0; i < sizeof(ext) / sizeof(ext[0]); i++)
      if (art_path(p, sizeof(p), stem, ext[i]) == 0)
         drop(p);
}

/* ---- the records ------------------------------------------------------------ */

/* "seen=<file name>|<size>|<mtime>" is the inbox file last looked at. */
static void triple(char *out, size_t cap, const char *name, off_t size,
                   time_t mtime)
{
   snprintf(out, cap, "%s|%lld|%lld", name, (long long)size,
            (long long)mtime);
}

static int rec_read_seen(const char *stem, char *seen, size_t cap)
{
   static const char *const ext[2] = { "own", "auto" };
   unsigned i;
   seen[0] = '\0';
   for (i = 0; i < 2; i++)
   {
      char p[CV_PATH], line[CV_PATH];
      FILE *f;
      if (art_path(p, sizeof(p), stem, ext[i]) != 0)
         continue;
      f = fopen(p, "rb");
      if (!f)
         continue;
      while (fgets(line, sizeof(line), f))
         if (!strncmp(line, "seen=", 5))
         {
            size_t n = strlen(line);
            while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
               line[--n] = '\0';
            snprintf(seen, cap, "%s", line + 5);
         }
      fclose(f);
      return 1;
   }
   return 0;
}

/* own = 1: boxart/<stem>.own (and no .auto); 0: boxart/<stem>.auto. */
static int rec_write(const char *stem, int own, const char *src,
                     const char *seen)
{
   char p[CV_PATH], tmp[CV_PATH + 8], other[CV_PATH];
   FILE *f;
   int ok;
   if (art_path(p, sizeof(p), stem, own ? "own" : "auto") != 0 ||
       art_path(other, sizeof(other), stem, own ? "auto" : "own") != 0)
      return -1;
   snprintf(tmp, sizeof(tmp), "%s.tmp", p);
   f = fopen(tmp, "wb");
   if (!f)
      return -1;
   ok = fprintf(f, "src=%s\nseen=%s\n", src ? src : "", seen ? seen : "") > 0;
   ok = fclose(f) == 0 && ok;
   if (!ok || put_file(tmp, p) != 0)
   {
      drop(tmp);
      return -1;
   }
   drop(other);
   return 0;
}

int cover_state(const char *stem)
{
   char p[CV_PATH];
   return art_path(p, sizeof(p), stem, "own") == 0 && exists(p);
}

/* ---- decoding ------------------------------------------------------------------ */

static unsigned char *slurp(const char *path, size_t *len)
{
   FILE *f = fopen(path, "rb");
   unsigned char *buf;
   long n;
   if (!f)
      return NULL;
   if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) <= 0 ||
       (size_t)n > CV_MAX_FILE || fseek(f, 0, SEEK_SET) != 0)
   {
      fclose(f);
      return NULL;
   }
   buf = (unsigned char *)malloc((size_t)n);
   if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n)
   {
      free(buf);
      buf = NULL;
   }
   fclose(f);
   if (buf)
      *len = (size_t)n;
   return buf;
}

/* A .shot: "STH1", w, h, stride (256), then rows of PSP-order RGB565. */
static unsigned char *decode_shot(const unsigned char *d, size_t len, int *w,
                                  int *h)
{
   unsigned sw, sh, ss, x, y;
   unsigned char *rgb;
   if (len < 16 || memcmp(d, "STH1", 4) != 0)
      return NULL;
   sw = d[4] | (unsigned)d[5] << 8;
   sh = d[6] | (unsigned)d[7] << 8;
   ss = d[8] | (unsigned)d[9] << 8;
   if (!sw || !sh || sw > 240 || sh > 160 || ss < sw ||
       len < 16 + (size_t)ss * sh * 2)
      return NULL;
   rgb = (unsigned char *)malloc((size_t)sw * sh * 3);
   if (!rgb)
      return NULL;
   for (y = 0; y < sh; y++)
      for (x = 0; x < sw; x++)
      {
         const unsigned char *p = d + 16 + ((size_t)y * ss + x) * 2;
         unsigned v = p[0] | (unsigned)p[1] << 8;
         unsigned r = v & 31, g = (v >> 5) & 63, b = (v >> 11) & 31;
         unsigned char *o = rgb + ((size_t)y * sw + x) * 3;
         o[0] = (unsigned char)((r << 3) | (r >> 2));
         o[1] = (unsigned char)((g << 2) | (g >> 4));
         o[2] = (unsigned char)((b << 3) | (b >> 2));
      }
   *w = (int)sw;
   *h = (int)sh;
   return rgb;
}

/* RGB, 3 bytes a pixel, from a PNG/JPG/.shot file.  NULL and `why` on
 * failure. */
static unsigned char *decode_file(const char *path, int *w, int *h, char *why,
                                  size_t cap)
{
   size_t len = 0;
   unsigned char *file = slurp(path, &len), *rgb, *rgba;
   int nc = 0, x, y;
   if (!file)
   {
      snprintf(why, cap, "That picture could not be read.");
      return NULL;
   }
   rgb = decode_shot(file, len, w, h);
   if (rgb)
   {
      free(file);
      return rgb;
   }
   if (!stbi_info_from_memory(file, (int)len, w, h, &nc) || *w <= 0 ||
       *h <= 0)
   {
      free(file);
      snprintf(why, cap, "That is not a PNG or JPG picture.");
      return NULL;
   }
   if ((uint64_t)*w * (uint64_t)*h > CV_MAX_PIX)
   {
      free(file);
      snprintf(why, cap, "That picture is too big (8 megapixels at most).");
      return NULL;
   }
   rgba = stbi_load_from_memory(file, (int)len, w, h, &nc, 4);
   free(file);
   if (!rgba)
   {
      snprintf(why, cap, "That picture could not be opened.");
      return NULL;
   }
   rgb = (unsigned char *)malloc((size_t)*w * *h * 3);
   if (!rgb)
   {
      stbi_image_free(rgba);
      snprintf(why, cap, "Not enough memory for that picture.");
      return NULL;
   }
   /* a see-through picture is laid over the menu's dark, not over black */
   for (y = 0; y < *h; y++)
      for (x = 0; x < *w; x++)
      {
         const unsigned char *s = rgba + ((size_t)y * *w + x) * 4;
         unsigned char *o = rgb + ((size_t)y * *w + x) * 3;
         unsigned a = s[3];
         o[0] = (unsigned char)((s[0] * a + 24 * (255 - a)) / 255);
         o[1] = (unsigned char)((s[1] * a + 26 * (255 - a)) / 255);
         o[2] = (unsigned char)((s[2] * a + 32 * (255 - a)) / 255);
      }
   stbi_image_free(rgba);
   return rgb;
}

/* Box-average down to at most COVER_MAX_EDGE on the long side (never up).
 * Returns the picture to use (a new buffer when it was reduced; `rgb` itself,
 * unchanged, when it already fits) and its size. */
static unsigned char *reduce(unsigned char *rgb, int *w, int *h)
{
   int nw, nh, x, y;
   unsigned char *out;
   if (*w <= COVER_MAX_EDGE && *h <= COVER_MAX_EDGE)
      return rgb;
   if (*w >= *h)
   {
      nw = COVER_MAX_EDGE;
      nh = (int)((long long)*h * COVER_MAX_EDGE / *w);
   }
   else
   {
      nh = COVER_MAX_EDGE;
      nw = (int)((long long)*w * COVER_MAX_EDGE / *h);
   }
   if (nw < 1) nw = 1;
   if (nh < 1) nh = 1;
   out = (unsigned char *)malloc((size_t)nw * nh * 3);
   if (!out)
      return rgb;
   for (y = 0; y < nh; y++)
   {
      int y0 = (int)((long long)y * *h / nh), y1 = (int)((long long)(y + 1) * *h / nh);
      if (y1 <= y0) y1 = y0 + 1;
      for (x = 0; x < nw; x++)
      {
         int x0 = (int)((long long)x * *w / nw), x1 = (int)((long long)(x + 1) * *w / nw);
         unsigned r = 0, g = 0, b = 0, n = 0;
         int u, v;
         if (x1 <= x0) x1 = x0 + 1;
         for (v = y0; v < y1; v++)
            for (u = x0; u < x1; u++)
            {
               const unsigned char *p = rgb + ((size_t)v * *w + u) * 3;
               r += p[0]; g += p[1]; b += p[2];
               n++;
            }
         out[((size_t)y * nw + x) * 3]     = (unsigned char)(r / n);
         out[((size_t)y * nw + x) * 3 + 1] = (unsigned char)(g / n);
         out[((size_t)y * nw + x) * 3 + 2] = (unsigned char)(b / n);
      }
   }
   free(rgb);
   *w = nw;
   *h = nh;
   return out;
}

/* ---- setting and taking away ------------------------------------------------------ */

static int set_from(const char *stem, const char *path, const char *src,
                    const char *seen, char *why, size_t cap)
{
   char png[CV_PATH], tmp[CV_PATH + 8], dl[CV_PATH], th[CV_PATH];
   int w = 0, h = 0, had_png, moved = 0;
   unsigned char *rgb;
   char reason[96];

   if (!g_root[0] || !stem || !stem[0] || strlen(stem) >= CV_STEM)
   {
      snprintf(why, cap, "That game has no name to keep a picture under.");
      return -1;
   }
   rgb = decode_file(path, &w, &h, reason, sizeof(reason));
   if (!rgb)
   {
      snprintf(why, cap, "%s", reason);
      return -1;
   }
   rgb = reduce(rgb, &w, &h);
   if (art_path(png, sizeof(png), stem, "png") != 0 ||
       art_path(dl, sizeof(dl), stem, "dl") != 0 ||
       snprintf(tmp, sizeof(tmp), "%s.tmp", png) >= (int)sizeof(tmp))
   {
      free(rgb);
      snprintf(why, cap, "That game's name is too long.");
      return -1;
   }
   if (!stbi_write_png(tmp, w, h, 3, rgb, w * 3))
   {
      drop(tmp);
      free(rgb);
      snprintf(why, cap, "The picture could not be saved. Is the card full?");
      return -1;
   }
   /* keep what was there: a download, or a picture put there by hand */
   had_png = exists(png);
   if (had_png && !cover_state(stem) && !exists(dl))
   {
      if (rename(png, dl) != 0)
      {
         drop(tmp);
         free(rgb);
         snprintf(why, cap, "The old cover could not be put aside.");
         return -1;
      }
      moved = 1;
   }
   if (put_file(tmp, png) != 0)
   {
      if (moved)
         rename(dl, png);
      drop(tmp);
      free(rgb);
      snprintf(why, cap, "The picture could not be saved.");
      return -1;
   }
   /* the note before the thumbnail: the fetcher's own thumbnail pass reads
    * it, so one it makes in between is the whole picture on a square too */
   if (rec_write(stem, 1, src, seen) != 0)
   {
      /* without the note the fetcher would not know it is the player's:
       * put everything back as it was */
      free(rgb);
      drop(png);
      if (moved)
         rename(dl, png);
      purge_caches(stem);
      snprintf(why, cap, "The picture could not be saved.");
      return -1;
   }
   purge_caches(stem);
   if (cover_thumb_path(stem, th, sizeof(th)) == 0)
   {
      drop(th);
      artfetch_thumb_rgb(rgb, w, h, th, 1);
   }
   free(rgb);
   artfetch_rescan();        /* the fetcher's idea of what is on the card */
   return 0;
}

int cover_set_file(const char *stem, const char *path, char *why, size_t cap)
{
   char seen[CV_PATH];
   char w[96];
   int rc;
   if (!why)
   {
      why = w;
      cap = sizeof(w);
   }
   why[0] = '\0';
   /* if an inbox file for this game is there, what the player chooses now is
    * newer than it: note it as seen so the next refresh leaves it alone */
   seen[0] = '\0';
   rec_read_seen(stem, seen, sizeof(seen));
   {
      char cur[CV_PATH];
      if (inbox_current(stem, cur, sizeof(cur)))
         snprintf(seen, sizeof(seen), "%s", cur);
   }
   rc = set_from(stem, path, "menu", seen, why, cap);
   return rc;
}

int cover_auto(const char *stem)
{
   char png[CV_PATH], dl[CV_PATH], th[CV_PATH], seen[CV_PATH], cur[CV_PATH];
   int was_own = cover_state(stem), restored = 0;
   if (!g_root[0] || !stem || !stem[0] ||
       art_path(png, sizeof(png), stem, "png") != 0 ||
       art_path(dl, sizeof(dl), stem, "dl") != 0)
      return -1;
   if (!was_own)
      return 0;
   seen[0] = '\0';
   rec_read_seen(stem, seen, sizeof(seen));
   if (inbox_current(stem, cur, sizeof(cur)))
      snprintf(seen, sizeof(seen), "%s", cur);
   /* the note first: if the picture goes and the note stays, the fetcher
    * would think a cover is still the player's */
   if (rec_write(stem, 0, "automatic", seen) != 0)
      return -1;
   drop(png);
   if (exists(dl) && rename(dl, png) == 0)
      restored = 1;
   purge_caches(stem);
   if (cover_thumb_path(stem, th, sizeof(th)) == 0)
      drop(th);
   if (restored)
   {
      /* its thumbnail now, from the picture put back */
      size_t len = 0;
      unsigned char *file = slurp(png, &len);
      if (file)
      {
         int w = 0, h = 0, nc = 0;
         unsigned char *px = stbi_load_from_memory(file, (int)len, &w, &h,
                                                   &nc, 3);
         free(file);
         if (px)
         {
            if (cover_thumb_path(stem, th, sizeof(th)) == 0)
               artfetch_thumb_rgb(px, w, h, th, 0);
            stbi_image_free(px);
         }
      }
      return 1;
   }
   artfetch_rescan();
   return 2;
}

/* ---- the game's own picture ----------------------------------------------------------- */

/* Keeps the newest of the files offered (a tie goes to the later name). */
static int consider(const char *path, time_t *t, char *best, size_t cap,
                    int *found)
{
   struct stat st;
   if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0)
      return 0;
   if (*found && (st.st_mtime < *t ||
                  (st.st_mtime == *t && strcmp(path, best) <= 0)))
      return 0;
   *t = st.st_mtime;
   snprintf(best, cap, "%s", path);
   *found = 1;
   return 1;
}

int cover_game_picture(const char *stem, const char *shot_dir,
                       const char *state1, char *path, size_t cap)
{
   char dir[CV_PATH], p[CV_PATH + CV_STEM], best[CV_PATH];
   time_t t = 0;
   DIR *d;
   struct dirent *e;
   size_t sl;
   int found = 0;
   if (!g_root[0] || !stem || !stem[0])
      return 0;
   sl = strlen(stem);
   /* 1  a screenshot of it: <stem>-YYYYMMDD-HHMMSS.png, the newest */
   if (shot_dir && shot_dir[0])
      snprintf(dir, sizeof(dir), "%s", shot_dir);
   else
      snprintf(dir, sizeof(dir), "%s/screenshots", g_root);
   d = opendir(dir);
   if (d)
   {
      while ((e = readdir(d)) != NULL)
      {
         size_t n = strlen(e->d_name);
         if (n > sl + 5 && !strncasecmp(e->d_name, stem, sl) &&
             e->d_name[sl] == '-' && isdigit((unsigned char)e->d_name[sl + 1]) &&
             !strcasecmp(e->d_name + n - 4, ".png") &&
             snprintf(p, sizeof(p), "%s/%s", dir, e->d_name) < (int)sizeof(p))
            consider(p, &t, best, sizeof(best), &found);
      }
      closedir(d);
   }
   if (!found)
   {
      /* 2  the picture it left behind */
      snprintf(p, sizeof(p), "%s/scenes/%s.shot", g_root, stem);
      consider(p, &t, best, sizeof(best), &found);
   }
   if (!found && state1 && state1[0])
   {
      /* 3  the newest save-state picture: <state>.shot, st0 to st9 */
      size_t n = strlen(state1);
      int slot;
      if (n > 1 && state1[n - 1] == '0')
         for (slot = 0; slot <= 9; slot++)
            if (snprintf(p, sizeof(p), "%.*s%d.shot", (int)(n - 1), state1,
                         slot) < (int)sizeof(p))
               consider(p, &t, best, sizeof(best), &found);
   }
   if (!found)
   {
      /* 4  its title screen */
      snprintf(p, sizeof(p), "%s/titles/%s.png", g_root, stem);
      consider(p, &t, best, sizeof(best), &found);
   }
   if (found)
      snprintf(path, cap, "%s", best);
   return found;
}

/* ---- the inbox ---------------------------------------------------------------------------- */

typedef struct
{
   char   name[CV_STEM];
   char   key_a[CV_STEM];      /* the name without .png/.jpg, lower case      */
   char   key_b[CV_STEM];      /* ...and without a ROM extension, or ""       */
   off_t  size;
   time_t mtime;
   int    pref;                /* png before jpg before jpeg                  */
} inbox_file;

static inbox_file *g_inbox;
static int g_inbox_n;

typedef struct
{
   char stem[CV_STEM];
   int  file;                  /* index into g_inbox                          */
} inbox_job;
static inbox_job *g_jobs;
static int g_jobs_n, g_jobs_next;

static void lower(char *s)
{
   for (; *s; s++)
      *s = (char)tolower((unsigned char)*s);
}

static int image_ext(const char *name, size_t *stem_len, int *pref)
{
   static const char *const ext[3] = { ".png", ".jpg", ".jpeg" };
   size_t n = strlen(name), e;
   int i;
   for (i = 0; i < 3; i++)
   {
      e = strlen(ext[i]);
      if (n > e && !strcasecmp(name + n - e, ext[i]))
      {
         *stem_len = n - e;
         *pref = i;
         return 1;
      }
   }
   return 0;
}

static void rom_ext_off(char *key)
{
   static const char *const rom[] = { ".gba", ".gbc", ".gb", ".sgb", ".sfc",
                                      ".smc", ".swc", ".fig", ".nes", ".zip",
                                      ".7z" };
   size_t n = strlen(key), e;
   unsigned i;
   for (i = 0; i < sizeof(rom) / sizeof(rom[0]); i++)
   {
      e = strlen(rom[i]);
      if (n > e && !strcmp(key + n - e, rom[i]))
      {
         key[n - e] = '\0';
         return;
      }
   }
}

static void inbox_free(void)
{
   free(g_inbox);
   g_inbox = NULL;
   g_inbox_n = 0;
   free(g_jobs);
   g_jobs = NULL;
   g_jobs_n = g_jobs_next = 0;
}

static int inbox_load(void)
{
   char dir[CV_PATH];
   DIR *d;
   struct dirent *e;
   int cap = 0;
   inbox_free();
   snprintf(dir, sizeof(dir), "%s/%s", g_root, COVER_INBOX_DIR);
   d = opendir(dir);
   if (!d)
      return 0;
   while ((e = readdir(d)) != NULL && g_inbox_n < CV_INBOX_MAX)
   {
      size_t sl;
      int pref;
      char p[CV_PATH + CV_STEM];
      struct stat st;
      inbox_file *f;
      if (e->d_name[0] == '.' || strlen(e->d_name) >= CV_STEM ||
          !image_ext(e->d_name, &sl, &pref))
         continue;
      snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
      if (stat(p, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0)
         continue;
      if (g_inbox_n == cap)
      {
         inbox_file *grown;
         cap = cap ? cap * 2 : 64;
         grown = (inbox_file *)realloc(g_inbox, (size_t)cap * sizeof(*grown));
         if (!grown)
            break;
         g_inbox = grown;
      }
      f = &g_inbox[g_inbox_n++];
      memset(f, 0, sizeof(*f));
      snprintf(f->name, sizeof(f->name), "%s", e->d_name);
      memcpy(f->key_a, e->d_name, sl);
      f->key_a[sl] = '\0';
      lower(f->key_a);
      snprintf(f->key_b, sizeof(f->key_b), "%s", f->key_a);
      rom_ext_off(f->key_b);
      if (!strcmp(f->key_b, f->key_a))
         f->key_b[0] = '\0';
      f->size = st.st_size;
      f->mtime = st.st_mtime;
      f->pref = pref;
   }
   closedir(d);
   return g_inbox_n;
}

/* The file for a game: an exact name beats one with the ROM extension in it;
 * then PNG, JPG, JPEG; then the lower name, for a stable answer. */
static int inbox_find(const char *stem)
{
   char key[CV_STEM];
   int i, best = -1, best_score = 1 << 30;
   snprintf(key, sizeof(key), "%s", stem);
   lower(key);
   for (i = 0; i < g_inbox_n; i++)
   {
      int score;
      if (!strcmp(g_inbox[i].key_a, key))
         score = g_inbox[i].pref;
      else if (g_inbox[i].key_b[0] && !strcmp(g_inbox[i].key_b, key))
         score = 10 + g_inbox[i].pref;
      else
         continue;
      if (score < best_score ||
          (score == best_score && strcmp(g_inbox[i].name, g_inbox[best].name) < 0))
      {
         best = i;
         best_score = score;
      }
   }
   return best;
}

/* The inbox file this game matches right now, as a record line.  A fresh
 * look at the folder (the scan's list may be old); what the scan holds is
 * left as it is. */
static int inbox_current(const char *stem, char *seen, size_t cap)
{
   inbox_file *keep = g_inbox;
   int keep_n = g_inbox_n, kjn = g_jobs_n, kjx = g_jobs_next, idx, ok = 0;
   inbox_job *kj = g_jobs;
   g_inbox = NULL; g_inbox_n = 0; g_jobs = NULL; g_jobs_n = g_jobs_next = 0;
   inbox_load();
   idx = inbox_find(stem);
   if (idx >= 0)
   {
      triple(seen, cap, g_inbox[idx].name, g_inbox[idx].size,
             g_inbox[idx].mtime);
      ok = 1;
   }
   free(g_inbox);
   free(g_jobs);
   g_inbox = keep; g_inbox_n = keep_n;
   g_jobs = kj; g_jobs_n = kjn; g_jobs_next = kjx;
   return ok;
}

int cover_inbox_scan(const char *const *stems, int n)
{
   return cover_inbox_match(stems, n, 1);
}

int cover_inbox_match(const char *const *stems, int n, int reload)
{
   int i, cap = 0;
   if (!g_root[0])
      return 0;
   if (reload)
      inbox_load();
   if (!g_inbox_n)
      return 0;
   for (i = 0; i < n; i++)
   {
      char want[CV_PATH], seen[CV_PATH];
      int f;
      if (!stems[i] || !stems[i][0] || strlen(stems[i]) >= CV_STEM)
         continue;
      f = inbox_find(stems[i]);
      if (f < 0)
         continue;
      triple(want, sizeof(want), g_inbox[f].name, g_inbox[f].size,
             g_inbox[f].mtime);
      rec_read_seen(stems[i], seen, sizeof(seen));
      if (!strcmp(want, seen))
         continue;                      /* looked at already */
      if (g_jobs_n == cap)
      {
         inbox_job *grown;
         cap = cap ? cap * 2 : 16;
         grown = (inbox_job *)realloc(g_jobs, (size_t)cap * sizeof(*grown));
         if (!grown)
            break;
         g_jobs = grown;
      }
      snprintf(g_jobs[g_jobs_n].stem, CV_STEM, "%s", stems[i]);
      g_jobs[g_jobs_n].file = f;
      g_jobs_n++;
   }
   g_jobs_next = 0;
   return g_jobs_n;
}

int cover_inbox_files(void)
{
   return g_inbox_n;
}

int cover_inbox_pending(void)
{
   return g_jobs_n - g_jobs_next;
}

int cover_inbox_step(char *stem_out, size_t cap)
{
   char path[CV_PATH + CV_STEM], seen[CV_PATH], why[96];
   const inbox_file *f;
   const inbox_job *j;
   int rc;
   if (g_jobs_next >= g_jobs_n)
      return 0;
   j = &g_jobs[g_jobs_next++];
   f = &g_inbox[j->file];
   if (stem_out)
      snprintf(stem_out, cap, "%s", j->stem);
   snprintf(path, sizeof(path), "%s/%s/%s", g_root, COVER_INBOX_DIR, f->name);
   triple(seen, sizeof(seen), f->name, f->size, f->mtime);
   rc = set_from(j->stem, path, f->name, seen, why, sizeof(why));
   if (rc != 0)
   {
      /* not tried again until the file changes; the cover stays as it was */
      int own = cover_state(j->stem);
      rec_write(j->stem, own, own ? "menu" : "automatic", seen);
      return -1;
   }
   return 1;
}

/* ---- pictures to choose from ---------------------------------------------------------------- */

typedef struct
{
   cover_pic *v;
   int n, cap, max;
} pic_list;

static void pic_walk(pic_list *l, const char *dir, const char *label,
                     int depth)
{
   DIR *d = opendir(dir);
   struct dirent *e;
   if (!d)
      return;
   while ((e = readdir(d)) != NULL && l->n < l->max)
   {
      char p[CV_PATH + CV_STEM];
      struct stat st;
      size_t sl;
      int pref;
      if (e->d_name[0] == '.')
         continue;
      if (snprintf(p, sizeof(p), "%s/%s", dir, e->d_name) >= (int)sizeof(p))
         continue;
      if (stat(p, &st) != 0)
         continue;
      if (S_ISDIR(st.st_mode))
      {
         if (depth < 3)
            pic_walk(l, p, label, depth + 1);
      }
      else if (S_ISREG(st.st_mode) && st.st_size > 0 &&
               image_ext(e->d_name, &sl, &pref))
      {
         cover_pic *c;
         if (strlen(p) >= sizeof(c->path))
            continue;
         if (l->n == l->cap)
         {
            cover_pic *grown;
            l->cap = l->cap ? l->cap * 2 : 64;
            grown = (cover_pic *)realloc(l->v, (size_t)l->cap * sizeof(*grown));
            if (!grown)
               break;
            l->v = grown;
         }
         c = &l->v[l->n++];
         memcpy(c->path, p, strlen(p) + 1);
         snprintf(c->label, sizeof(c->label), "%s", label);
         c->mtime = st.st_mtime;
      }
   }
   closedir(d);
}

static int pic_cmp(const void *a, const void *b)
{
   const cover_pic *x = (const cover_pic *)a, *y = (const cover_pic *)b;
   if (x->mtime != y->mtime)
      return x->mtime < y->mtime ? 1 : -1;
   return strcmp(x->path, y->path);
}

cover_pic *cover_pictures(const char *const *dirs, int ndirs, int max,
                          int *count)
{
   pic_list l;
   int i;
   memset(&l, 0, sizeof(l));
   l.max = max;
   for (i = 0; i < ndirs; i++)
   {
      /* the label: the last part of the folder's name */
      const char *label = strrchr(dirs[i], '/');
      label = label && label[1] ? label + 1 : dirs[i];
      if (strchr(label, ':'))
         label = strchr(label, ':') + 1;
      pic_walk(&l, dirs[i], label, 0);
   }
   if (l.n > 1)
      qsort(l.v, (size_t)l.n, sizeof(*l.v), pic_cmp);
   *count = l.n;
   if (!l.n)
   {
      free(l.v);
      return NULL;
   }
   return l.v;
}
