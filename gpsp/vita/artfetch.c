/* artfetch.c -- see artfetch.h. */
#include "artfetch.h"

#include <ctype.h>
#include <stdint.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#if !defined(__vita__)
#include <netinet/tcp.h>
#endif
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "artalt.h"
#include "gamedb.h"
#include "plat.h"
#include "stb_image.h"
#include "sz_rom.h"

#define AF_NAME_CAP   256
#define AF_PATH_CAP   600
#define AF_MAX_BYTES  (4u * 1024u * 1024u)   /* a box art PNG is ~0.5 MB */
#define AF_RING       16

/* What is fetched for a cartridge, in this order: its box art (the tile,
 * the cover) and its title screen (the home screen's picture behind the
 * row for a game never played: libretro's Named_Titles). */
enum { AK_BOX = 0, AK_TITLE, AK_KINDS };
static const char *const KIND_DIR[AK_KINDS] = { "boxart", "titles" };
static const char *const KIND_URL[AK_KINDS] = { "Named_Boxarts",
                                                "Named_Titles" };

#define A_LOAD(x)      __atomic_load_n(&(x), __ATOMIC_SEQ_CST)
#define A_STORE(x, v)  __atomic_store_n(&(x), (v), __ATOMIC_SEQ_CST)

typedef struct
{
   int n;
   unsigned generation;
   char (*path)[AF_PATH_CAP];
   char (*name)[AF_NAME_CAP];
} af_list;

typedef struct
{
   unsigned generation;
   char path[AF_PATH_CAP], name[AF_NAME_CAP];
} af_focus;

/* ---- what the UI and the thread share ---------------------------------- */

static af_list *g_new;                /* atomic: the newest list, handed over */
static af_focus *g_focus;
static unsigned g_list_generation;
static int g_paused, g_quit, g_running;
static int g_nav_busy;                /* atomic: menu nav/local load active  */
static int g_titles = 1;              /* title screens too (artfetch_titles) */
static int g_retry;                   /* atomic: artfetch_retry asked        */
static int g_rescan;                  /* atomic: artfetch_rescan asked       */
/* atomic, for Settings: cartridges looked at / in the list / with a box
 * art now; 1 while the thread is working through the list */
static int g_prog_seen, g_prog_total, g_prog_have, g_prog_busy;
static int (*g_online)(void);
static artfetch_https_ops g_https;
static int g_https_ok;
static int g_prefer_mirror;           /* worker: 1 once GitHub mirror proved alive over dead primary */
static int g_primary_fails;           /* worker: consecutive primary network failures */
static int g_mirror_fails;            /* worker: consecutive mirror network failures */
static uint64_t g_online_at;          /* worker: cached online check timestamp */
static int g_online_val = 1;
static char g_root[AF_PATH_CAP];
static char g_host[128];
static int g_port = 80;

void artfetch_set_https(const artfetch_https_ops *ops)
{
   if (ops && ops->start && ops->poll && ops->cancel && ops->release)
   {
      g_https = *ops;
      g_https_ok = 1;
   }
   else
   {
      memset(&g_https, 0, sizeof(g_https));
      g_https_ok = 0;
   }
}

/* Two single-producer rings (the thread writes, the UI reads): file names
 * whose picture was just written, and lines for the log. */
static char g_done[AF_RING][AF_NAME_CAP];
static unsigned g_done_w, g_done_r;
static int g_done_lost;
static char g_logl[AF_RING][200];
static unsigned g_log_w, g_log_r;

static void say(const char *fmt, const char *a, const char *b)
{
   unsigned w = A_LOAD(g_log_w);
   if (w - A_LOAD(g_log_r) >= AF_RING)
      return;                          /* the UI is behind: a line less */
   snprintf(g_logl[w % AF_RING], sizeof(g_logl[0]), fmt, a ? a : "",
            b ? b : "");
   A_STORE(g_log_w, w + 1);
}

static void done(const char *name)
{
   unsigned w = A_LOAD(g_done_w);
   if (w - A_LOAD(g_done_r) >= AF_RING)
   {
      /* Settings may not drain arrivals. Keep working and ask the UI to
       * invalidate its artwork once, rather than blocking the producer. */
      A_STORE(g_done_lost, 1);
      return;
   }
   snprintf(g_done[w % AF_RING], sizeof(g_done[0]), "%s", name);
   A_STORE(g_done_w, w + 1);
}

int artfetch_take(char *name, size_t cap)
{
   unsigned r = A_LOAD(g_done_r);
   if (r == A_LOAD(g_done_w))
      return 0;
   snprintf(name, cap, "%s", g_done[r % AF_RING]);
   A_STORE(g_done_r, r + 1);
   return 1;
}

int artfetch_resync(void)
{
   return __atomic_exchange_n(&g_done_lost, 0, __ATOMIC_SEQ_CST);
}

int artfetch_log(char *line, size_t cap)
{
   unsigned r = A_LOAD(g_log_r);
   if (r == A_LOAD(g_log_w))
      return 0;
   snprintf(line, cap, "%s", g_logl[r % AF_RING]);
   A_STORE(g_log_r, r + 1);
   return 1;
}

void artfetch_pause(int on)
{
   A_STORE(g_paused, on ? 1 : 0);
}

void artfetch_nav_busy(int busy)
{
   A_STORE(g_nav_busy, busy ? 1 : 0);
}

void artfetch_titles(int on)
{
   A_STORE(g_titles, on ? 1 : 0);
}

void menu_files_art_changed(void);

void artfetch_retry(void)
{
   menu_files_art_changed();
   A_STORE(g_retry, 1);
}

void artfetch_rescan(void)
{
   menu_files_art_changed();
   A_STORE(g_rescan, 1);
}

int artfetch_active(void)
{
   return A_LOAD(g_running) && !A_LOAD(g_quit);
}

int artfetch_progress(int *seen, int *total, int *have)
{
   if (seen)
      *seen = A_LOAD(g_prog_seen);
   if (total)
      *total = A_LOAD(g_prog_total);
   if (have)
      *have = A_LOAD(g_prog_have);
   return A_LOAD(g_running) && A_LOAD(g_prog_busy);
}

static void list_free(af_list *l)
{
   if (!l)
      return;
   free(l->path);
   free(l->name);
   free(l);
}

unsigned artfetch_list(const char *const *paths, const char *const *names, int n)
{
   af_list *l;
   int i;
   if (!A_LOAD(g_running) || n < 0 || (n && (!paths || !names)))
      return 0;
   l = (af_list *)calloc(1, sizeof(*l));
   if (!l)
      return 0;
   l->path = n ? calloc((size_t)n, sizeof(*l->path)) : NULL;
   l->name = n ? calloc((size_t)n, sizeof(*l->name)) : NULL;
   if (n && (!l->path || !l->name))
   {
      list_free(l);
      return 0;
   }
   for (i = 0; i < n; i++)
   {
      snprintf(l->path[i], AF_PATH_CAP, "%s", paths[i]);
      snprintf(l->name[i], AF_NAME_CAP, "%s", names[i]);
   }
   l->n = n;
   l->generation = ++g_list_generation;
   if (!l->generation)
      l->generation = ++g_list_generation;
   {
      unsigned generation = l->generation;
      list_free(__atomic_exchange_n(&g_new, l, __ATOMIC_SEQ_CST));
      return generation;
   }
}

void artfetch_focus(unsigned generation, const char *path, const char *name)
{
   af_focus *focus;
   if (!A_LOAD(g_running) || !generation || !path || !name ||
       strlen(path) >= AF_PATH_CAP || strlen(name) >= AF_NAME_CAP)
      return;
   focus = malloc(sizeof(*focus));
   if (!focus)
      return;
   focus->generation = generation;
   strcpy(focus->path, path);
   strcpy(focus->name, name);
   free(__atomic_exchange_n(&g_focus, focus, __ATOMIC_SEQ_CST));
}

/* ---- the thread's own --------------------------------------------------- */

/* v4: invalidates misses recorded while thumbnails.libretro.com returned 404
 * across all systems, now backed by the GitHub libretro-thumbnails HTTPS mirror. */
#define MISSING_HEADER "# VitAdhoc artwork misses v4\n"
#define AF_MISS_BATCH  16
static unsigned char *g_body;

/* Hard stop: exit, game launched, list replaced, retry, or rescan.
 * Used during active HTTP/HTTPS transfers so scrolling focus does not abort
 * an in-flight cover download or tear down the keep-alive socket. */
static int stop_hard(void)
{
   return A_LOAD(g_quit) || A_LOAD(g_paused) || A_LOAD(g_new) ||
          A_LOAD(g_retry) || A_LOAD(g_rescan);
}

static int stop_now(void)
{
   return stop_hard() || A_LOAD(g_focus);
}

static uint64_t worker_now(void)
{
   struct timeval tv;
   gettimeofday(&tv, NULL);
   return (uint64_t)tv.tv_sec * 1000000u + (unsigned)tv.tv_usec;
}

static int online_check(int force)
{
   uint64_t now;
   if (!g_online)
      return 1;
   now = worker_now();
   if (!force && g_online_at && now - g_online_at < 250000u)
      return g_online_val;
   g_online_val = g_online() ? 1 : 0;
   g_online_at = now;
   return g_online_val;
}

static void nav_yield(void)
{
   while (A_LOAD(g_nav_busy) && !stop_now())
      plat_thread_sleep_us(15000);
}

static int cart_crc_stop(void)
{
   nav_yield();
   return stop_now();
}

/* A pause that ends early when the thread is told to stop. */
static void nap_us(uint32_t us)
{
   int paused = A_LOAD(g_paused);
   int titles = A_LOAD(g_titles);
   while (us && !A_LOAD(g_quit))
   {
      uint32_t d = us < 250000u ? us : 250000u;
      plat_thread_sleep_us(d);
      us -= d;
      if (A_LOAD(g_new) || A_LOAD(g_retry) || A_LOAD(g_rescan) ||
          A_LOAD(g_focus) || A_LOAD(g_paused) != paused || A_LOAD(g_titles) != titles)
         break;
   }
}

/* The file name without its extension. */
static void stem_of(const char *name, char *out, size_t cap)
{
   const char *dot = strrchr(name, '.');
   size_t n = dot ? (size_t)(dot - name) : strlen(name);
   if (n >= cap)
      n = cap - 1;
   memcpy(out, name, n);
   out[n] = '\0';
}

typedef struct { char **keys; size_t cap, count; } af_set;
enum { INV_THUMB = AK_KINDS, INV_OWN, INV_BOX_PNG, INV_BOX_JPG, INV_COUNT };
static af_set g_inventory[INV_COUNT];
static int g_inventory_valid;
static int g_dir_made[AK_KINDS];
static int g_thumb_dir_made;

static unsigned stem_hash(const char *s)
{
   unsigned h = 2166136261u;
   for (; *s; s++)
   {
      unsigned c = (unsigned char)*s;
      if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
      h = (h ^ c) * 16777619u;
   }
   return h;
}

static int set_has(const af_set *set, const char *key)
{
   size_t slot;
   if (!set->cap)
      return 0;
   slot = stem_hash(key) & (set->cap - 1);
   while (set->keys[slot])
   {
      if (!strcasecmp(set->keys[slot], key))
         return 1;
      slot = (slot + 1) & (set->cap - 1);
   }
   return 0;
}

static int set_add(af_set *set, const char *key)
{
   size_t slot, i;
   char *copy;
   if (set_has(set, key))
      return 0;
   if (!set->cap || (set->count + 1) * 2 >= set->cap)
   {
      size_t cap = set->cap ? set->cap * 2 : 128;
      char **keys = calloc(cap, sizeof(*keys));
      if (!keys)
         return -1;
      for (i = 0; i < set->cap; i++)
         if (set->keys[i])
         {
            slot = stem_hash(set->keys[i]) & (cap - 1);
            while (keys[slot]) slot = (slot + 1) & (cap - 1);
            keys[slot] = set->keys[i];
         }
      free(set->keys);
      set->keys = keys;
      set->cap = cap;
   }
   copy = strdup(key);
   if (!copy)
      return -1;
   slot = stem_hash(key) & (set->cap - 1);
   while (set->keys[slot]) slot = (slot + 1) & (set->cap - 1);
   set->keys[slot] = copy;
   set->count++;
   return 0;
}

/* Pictures on the card a thumbnail could not be made from (corrupt, too
 * big, a format thumb_write does not read): not read and decoded again on
 * every pass this session.  "Look again" (g_retry) forgets them. */
static af_set g_thumb_failed;

static void set_free(af_set *set)
{
   size_t i;
   for (i = 0; i < set->cap; i++)
      free(set->keys[i]);
   free(set->keys);
   memset(set, 0, sizeof(*set));
}

/* <kind dir>/.missing.txt, in memory as an O(1) hash set, with batched
 * disk flushes so Vita FAT32 is not rewritten on every single miss. */
static af_set g_missing[AK_KINDS];
static int    g_missing_current[AK_KINDS];
static char **g_miss_pend[AK_KINDS];
static int    g_miss_npend[AK_KINDS], g_miss_cappend[AK_KINDS];

static void missing_pend_clear(int k)
{
   int i;
   for (i = 0; i < g_miss_npend[k]; i++)
      free(g_miss_pend[k][i]);
   free(g_miss_pend[k]);
   g_miss_pend[k] = NULL;
   g_miss_npend[k] = g_miss_cappend[k] = 0;
}

static void missing_flush(int k)
{
   char path[AF_PATH_CAP + 32];
   FILE *f;
   int i;
   if (!g_miss_npend[k])
      return;
   if (!g_dir_made[k])
   {
      snprintf(path, sizeof(path), "%s/%s", g_root, KIND_DIR[k]);
      mkdir(path, 0777);
      g_dir_made[k] = 1;
   }
   snprintf(path, sizeof(path), "%s/%s/.missing.txt", g_root, KIND_DIR[k]);
   f = fopen(path, g_missing_current[k] ? "a" : "w");
   if (f)
   {
      if (!g_missing_current[k])
         fputs(MISSING_HEADER, f);
      for (i = 0; i < g_miss_npend[k]; i++)
         fprintf(f, "%s\n", g_miss_pend[k][i]);
      fclose(f);
      g_missing_current[k] = 1;
   }
   missing_pend_clear(k);
}

static void missing_flush_all(void)
{
   int k;
   for (k = 0; k < AK_KINDS; k++)
      missing_flush(k);
}

static void missing_add(int k, const char *stem, int write)
{
   if (set_has(&g_missing[k], stem))
      return;
   if (set_add(&g_missing[k], stem) != 0)
      return;
   if (write)
   {
      if (g_miss_npend[k] == g_miss_cappend[k])
      {
         int cap = g_miss_cappend[k] ? g_miss_cappend[k] * 2 : AF_MISS_BATCH;
         char **m = (char **)realloc(g_miss_pend[k], (size_t)cap * sizeof(char *));
         if (!m)
         {
            missing_flush(k);
            return;
         }
         g_miss_pend[k] = m;
         g_miss_cappend[k] = cap;
      }
      g_miss_pend[k][g_miss_npend[k]] = strdup(stem);
      if (g_miss_pend[k][g_miss_npend[k]])
         g_miss_npend[k]++;
      if (g_miss_npend[k] >= AF_MISS_BATCH)
         missing_flush(k);
   }
}

static int missing_has(int k, const char *stem)
{
   return set_has(&g_missing[k], stem);
}

static void missing_clear(int k, int unlink_file)
{
   missing_pend_clear(k);
   set_free(&g_missing[k]);
   g_missing_current[k] = 0;
   if (unlink_file)
   {
      char path[AF_PATH_CAP + 32];
      snprintf(path, sizeof(path), "%s/%s/.missing.txt", g_root,
               KIND_DIR[k]);
      remove(path);
   }
}

static void missing_load(int k)
{
   char path[AF_PATH_CAP + 32], line[AF_NAME_CAP + 8];
   FILE *f;
   snprintf(path, sizeof(path), "%s/%s/.missing.txt", g_root, KIND_DIR[k]);
   f = fopen(path, "r");
   if (!f)
      return;
   /* Older files can contain false misses from case-sensitive routing. */
   if (!fgets(line, sizeof(line), f) || strcmp(line, MISSING_HEADER))
   {
      fclose(f);
      return;
   }
   g_missing_current[k] = 1;
   while (fgets(line, sizeof(line), f))
   {
      size_t n = strlen(line);
      while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
         line[--n] = '\0';
      if (n)
         missing_add(k, line, 0);
   }
   fclose(f);
}

static void inventory_clear(void)
{
   unsigned k;
   for (k = 0; k < INV_COUNT; k++)
      set_free(&g_inventory[k]);
   g_inventory_valid = 0;
}

/* menu_files.c's picture-name index notification hooks; do-nothing defaults
 * where artfetch is built alone (its unit tests). */
__attribute__((weak)) void menu_files_art_changed(void) { }
__attribute__((weak)) void menu_files_art_added(const char *path) { (void)path; }

static void inventory_add(int kind, const char *stem)
{
   menu_files_art_changed();
   if (set_add(&g_inventory[kind], stem) != 0)
      g_inventory_valid = 0;
}

static void inventory_add_path(int kind, const char *stem, const char *path)
{
   if (path && path[0])
      menu_files_art_added(path);
   else
      menu_files_art_changed();
   if (set_add(&g_inventory[kind], stem) != 0)
      g_inventory_valid = 0;
}

/* cache/ and cache/boxart/ (the app makes them at boot; this is for a root
 * that did not). */
static void thumb_dir_make(void)
{
   char p[AF_PATH_CAP + 32];
   if (g_thumb_dir_made)
      return;
   snprintf(p, sizeof(p), "%s/cache", g_root);
   mkdir(p, 0777);
   snprintf(p, sizeof(p), "%s/" ART_THUMB_DIR, g_root);
   mkdir(p, 0777);
   g_thumb_dir_made = 1;
}

static void inventory_load(void)
{
   int kind, ok = 1;
   inventory_clear();
   g_thumb_dir_made = 0;
   for (kind = 0; kind < AK_KINDS; kind++)
   {
      char path[AF_PATH_CAP + 32];
      DIR *dir;
      struct dirent *entry;
      unsigned nseen = 0;
      snprintf(path, sizeof(path), "%s/%s", g_root, KIND_DIR[kind]);
      dir = opendir(path);
      if (!dir)
      {
         if (errno != ENOENT) ok = 0;
         continue;
      }
      g_dir_made[kind] = 1;
      while (!A_LOAD(g_quit) && !A_LOAD(g_new))
      {
         const char *name, *ext;
         size_t len, stem_len;
         char stem[AF_NAME_CAP];
         int target = kind;
         if ((++nseen & 63u) == 0 && A_LOAD(g_nav_busy))
            nav_yield();
         errno = 0;
         entry = readdir(dir);
         if (!entry)
         {
            if (errno) ok = 0;
            break;
         }
         name = entry->d_name;
         ext = strrchr(name, '.');
         len = strlen(name);
         if (kind == AK_BOX && len > strlen(ART_THUMB_SUFFIX) &&
             !strcasecmp(name + len - strlen(ART_THUMB_SUFFIX), ART_THUMB_SUFFIX))
         {
            /* made by 1.2 and before: it belongs in cache/boxart/ now */
            char from[AF_PATH_CAP + AF_NAME_CAP + 32], to[AF_PATH_CAP + AF_NAME_CAP + 32];
            thumb_dir_make();
            snprintf(from, sizeof(from), "%s/%s/%s", g_root, KIND_DIR[kind], name);
            snprintf(to, sizeof(to), "%s/" ART_THUMB_DIR "/%s", g_root, name);
            if (access(to, F_OK) != 0)
               rename(from, to);       /* same card: a rename */
            else
               remove(from);           /* the new one is the copy that counts */
            continue;
         }
         if (kind == AK_BOX && ext && !strcasecmp(ext, ".own"))
         {
            stem_len = (size_t)(ext - name);
            if (stem_len && stem_len < sizeof(stem))
            {
               memcpy(stem, name, stem_len);
               stem[stem_len] = '\0';
               if (set_add(&g_inventory[INV_OWN], stem) != 0) ok = 0;
            }
            continue;
         }
         {
            int is_png = ext && !strcasecmp(ext, ".png");
            int is_jpg = ext && !strcasecmp(ext, ".jpg");
            if (!ext || (!is_png && !is_jpg &&
                         strcasecmp(ext, ".jpeg") && strcasecmp(ext, ".bmp") &&
                         strcasecmp(ext, ".v565") && strcasecmp(ext, ".565")))
               continue;
            stem_len = (size_t)(ext - name);
            if (!stem_len || stem_len >= sizeof(stem))
               continue;
            memcpy(stem, name, stem_len);
            stem[stem_len] = '\0';
            if (set_add(&g_inventory[target], stem) != 0) ok = 0;
            if (kind == AK_BOX)
            {
               if (is_png && set_add(&g_inventory[INV_BOX_PNG], stem) != 0) ok = 0;
               if (is_jpg && set_add(&g_inventory[INV_BOX_JPG], stem) != 0) ok = 0;
            }
         }
      }
      if (A_LOAD(g_quit) || A_LOAD(g_new)) ok = 0;
      closedir(dir);
   }
   /* the tile pictures */
   {
      char path[AF_PATH_CAP + 32];
      DIR *dir;
      struct dirent *entry;
      unsigned nseen = 0;
      snprintf(path, sizeof(path), "%s/" ART_THUMB_DIR, g_root);
      dir = opendir(path);
      if (!dir)
      {
         if (errno != ENOENT) ok = 0;
      }
      else
      {
         g_thumb_dir_made = 1;
         while (!A_LOAD(g_quit) && !A_LOAD(g_new))
         {
            const char *name;
            size_t len, sl = strlen(ART_THUMB_SUFFIX), stem_len;
            char stem[AF_NAME_CAP];
            if ((++nseen & 63u) == 0 && A_LOAD(g_nav_busy))
               nav_yield();
            errno = 0;
            entry = readdir(dir);
            if (!entry)
            {
               if (errno) ok = 0;
               break;
            }
            name = entry->d_name;
            len = strlen(name);
            if (len > 4 && !strcasecmp(name + len - 4, ".own"))
            {
               stem_len = len - 4;
               if (stem_len && stem_len < sizeof(stem))
               {
                  memcpy(stem, name, stem_len);
                  stem[stem_len] = '\0';
                  if (set_add(&g_inventory[INV_OWN], stem) != 0) ok = 0;
               }
               continue;
            }
            if (len <= sl || strcasecmp(name + len - sl, ART_THUMB_SUFFIX))
               continue;
            stem_len = len - sl;
            if (stem_len >= sizeof(stem))
               continue;
            memcpy(stem, name, stem_len);
            stem[stem_len] = '\0';
            if (set_add(&g_inventory[INV_THUMB], stem) != 0) ok = 0;
         }
         if (A_LOAD(g_quit) || A_LOAD(g_new)) ok = 0;
         closedir(dir);
      }
   }
   g_inventory_valid = ok;
}

/* Revalidate at write boundaries so user artwork arriving during HTTP wins. */
static int have_art_disk(int k, const char *stem)
{
   static const char *const ext[] = { "png", "jpg", "jpeg", "bmp", "v565",
                                      "565" };
   char path[AF_PATH_CAP + AF_NAME_CAP + 32];
   struct stat st;
   unsigned i;
   for (i = 0; i < sizeof(ext) / sizeof(ext[0]); i++)
   {
      snprintf(path, sizeof(path), "%s/%s/%s.%s", g_root, KIND_DIR[k], stem,
               ext[i]);
      if (stat(path, &st) == 0)
         return 1;
   }
   return 0;
}

static int have_art(int k, const char *stem)
{
   return g_inventory_valid ? set_has(&g_inventory[k], stem)
                            : have_art_disk(k, stem);
}

/* '&*\/:`<>?\|"' -> '_' (libretro-thumbnails' rule), then every byte that
 * is not unreserved as %XX. */
static void url_name(const char *s, char *out, size_t cap)
{
   static const char hex[] = "0123456789ABCDEF";
   size_t n = 0;
   for (; *s && n + 4 < cap; s++)
   {
      unsigned char c = (unsigned char)*s;
      if (strchr("&*/:`<>?\\|\"", c))
         c = '_';
      if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
          (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
          c == '~')
         out[n++] = (char)c;
      else
      {
         out[n++] = '%';
         out[n++] = hex[c >> 4];
         out[n++] = hex[c & 15];
      }
   }
   out[n] = '\0';
}

enum { GET_OK = 0, GET_MISSING = 1, GET_FAILED = -1, GET_REDIRECT = 2, GET_UNAVAIL = 3 };

/* The server's address, looked up once: a pass over a library asks it for
 * thousands of pictures, and each lookup was a DNS query on the console.
 * Forgotten when a connection or a transfer fails (the next request looks
 * it up again) and when the server changes. */
static struct sockaddr_storage g_addr;
static socklen_t g_addr_len;
static char g_addr_key[300];

static void extract_location(const char *hdr, char *loc, size_t cap)
{
   const char *p = hdr;
   loc[0] = '\0';
   while (p && *p)
   {
      const char *eol = strstr(p, "\r\n");
      if (!strncasecmp(p, "location:", 9))
      {
         const char *v = p + 9;
         size_t n;
         while (*v == ' ' || *v == '\t')
            v++;
         n = eol ? (size_t)(eol - v) : strlen(v);
         while (n > 0 && (v[n - 1] == ' ' || v[n - 1] == '\t'))
            n--;
         if (n >= cap)
            n = cap - 1;
         memcpy(loc, v, n);
         loc[n] = '\0';
         return;
      }
      if (!eol)
         break;
      p = eol + 2;
   }
}

static int apply_redirect(const char *loc, char *host, size_t hcap, int *port,
                          char *path, size_t pcap)
{
   char next_path[1400];
   if (!loc || !loc[0])
      return -1;
   if (!strncasecmp(loc, "https://", 8))
      return -1;
   if (!strncasecmp(loc, "http://", 7))
   {
      const char *hp = loc + 7;
      const char *slash = strchr(hp, '/');
      const char *colon = NULL, *p;
      size_t hlen;
      int next_port = 80;
      const char *hend = slash ? slash : hp + strlen(hp);
      for (p = hp; p < hend; p++)
         if (*p == ':')
            colon = p;
      hlen = (size_t)((colon ? colon : hend) - hp);
      if (hlen == 0 || hlen >= hcap)
         return -1;
      if (colon)
      {
         char *ep = NULL;
         long pv = strtol(colon + 1, &ep, 10);
         if (ep != hend || pv <= 0 || pv > 65535)
            return -1;
         next_port = (int)pv;
      }
      memcpy(host, hp, hlen);
      host[hlen] = '\0';
      *port = next_port;
      snprintf(next_path, sizeof(next_path), "%s", slash ? slash : "/");
   }
   else if (loc[0] == '/')
      snprintf(next_path, sizeof(next_path), "%s", loc);
   else
   {
      const char *slash = strrchr(path, '/');
      size_t prefix = slash ? (size_t)(slash - path + 1) : 0;
      size_t llen = strlen(loc);
      if (prefix)
      {
         if (prefix + llen >= sizeof(next_path))
            return -1;
         memcpy(next_path, path, prefix);
         memcpy(next_path + prefix, loc, llen + 1);
      }
      else
      {
         if (1 + llen >= sizeof(next_path))
            return -1;
         next_path[0] = '/';
         memcpy(next_path + 1, loc, llen + 1);
      }
   }
   if (strlen(next_path) >= pcap)
      return -1;
   snprintf(path, pcap, "%s", next_path);
   return 0;
}

/* Persistent HTTP/1.1 Keep-Alive socket (worker thread only). */
static int  g_http_sock = -1;
static char g_http_host[sizeof(g_host)];
static int  g_http_port;

#define AF_DRAIN_CAP   65536u
#define AF_RECV_MAX_US 15000000u

#ifdef MSG_NOSIGNAL
#define AF_SEND_FLAGS MSG_NOSIGNAL
#else
#define AF_SEND_FLAGS 0
#endif

static void http_close(void)
{
   if (g_http_sock >= 0)
   {
      close(g_http_sock);
      g_http_sock = -1;
   }
   g_http_host[0] = '\0';
   g_http_port = 0;
}

/* Check whether lowercased header block `hdr` contains a header `name`
 * (e.g. "connection:") with comma/whitespace-delimited token `tok`. */
static int has_hdr_token(const char *hdr, const char *name, const char *tok)
{
   size_t nlen = strlen(name), tlen = strlen(tok);
   const char *p = strstr(hdr, "\r\n");
   while (p)
   {
      const char *line = p + 2;
      const char *eol = strstr(line, "\r\n");
      const char *end = eol ? eol : line + strlen(line);
      if ((size_t)(end - line) >= nlen && !memcmp(line, name, nlen))
      {
         const char *q = line + nlen;
         while (q < end)
         {
            const char *s;
            while (q < end && (*q == ' ' || *q == '\t' || *q == ','))
               q++;
            s = q;
            while (q < end && *q != ' ' && *q != '\t' && *q != ',' && *q != ';')
               q++;
            if ((size_t)(q - s) == tlen && !memcmp(s, tok, tlen))
               return 1;
            if (q < end && *q == ';')
               while (q < end && *q != ',')
                  q++;
         }
      }
      p = eol;
   }
   return 0;
}

/* Find "\r\n" inside buf[pos..n). Returns offset of '\r', or (size_t)-1. */
static size_t find_crlf(const unsigned char *buf, size_t pos, size_t n)
{
   while (pos + 1 < n)
   {
      const unsigned char *cr = (const unsigned char *)memchr(buf + pos, '\r', n - pos - 1);
      if (!cr)
         return (size_t)-1;
      pos = (size_t)(cr - buf);
      if (buf[pos + 1] == '\n')
         return pos;
      pos++;
   }
   return (size_t)-1;
}

/* Parse hex chunk size from buf[pos..eol). Returns 0 on success, -1 on error. */
static int parse_chunk_hex(const unsigned char *buf, size_t pos, size_t eol,
                           size_t *out)
{
   size_t val = 0;
   int digits = 0;
   while (pos < eol && (buf[pos] == ' ' || buf[pos] == '\t'))
      pos++;
   while (pos < eol)
   {
      unsigned char c = buf[pos];
      unsigned d;
      if (c >= '0' && c <= '9') d = (unsigned)(c - '0');
      else if (c >= 'a' && c <= 'f') d = (unsigned)(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') d = (unsigned)(c - 'A' + 10);
      else break;
      if (val > (AF_MAX_BYTES >> 4))
         return -1;
      val = (val << 4) | d;
      digits = 1;
      pos++;
   }
   if (!digits)
      return -1;
   while (pos < eol && (buf[pos] == ' ' || buf[pos] == '\t'))
      pos++;
   if (pos < eol && buf[pos] != ';')
      return -1;
   *out = val;
   return 0;
}

/* 1 = complete chunked body ends at *end_out, 0 = need more data, -1 = bad. */
static int chunked_complete(const unsigned char *buf, size_t n, size_t *end_out)
{
   size_t pos = 0;
   for (;;)
   {
      size_t eol = find_crlf(buf, pos, n);
      size_t csize = 0;
      if (eol == (size_t)-1)
         return 0;
      if (parse_chunk_hex(buf, pos, eol, &csize) != 0)
         return -1;
      pos = eol + 2;
      if (csize == 0)
      {
         for (;;)
         {
            if (pos + 2 > n)
               return 0;
            if (buf[pos] == '\r' && buf[pos + 1] == '\n')
            {
               if (end_out)
                  *end_out = pos + 2;
               return 1;
            }
            eol = find_crlf(buf, pos, n);
            if (eol == (size_t)-1)
               return 0;
            pos = eol + 2;
         }
      }
      if (csize > n - pos || n - pos - csize < 2)
         return 0;
      if (buf[pos + csize] != '\r' || buf[pos + csize + 1] != '\n')
         return -1;
      pos += csize + 2;
   }
}

/* Decode a complete chunked body in place. Returns 0 on success, -1 on error. */
static int chunked_decode(unsigned char *buf, size_t n, size_t *out_len)
{
   size_t rd = 0, wr = 0;
   for (;;)
   {
      size_t eol = find_crlf(buf, rd, n);
      size_t csize = 0;
      if (eol == (size_t)-1 || parse_chunk_hex(buf, rd, eol, &csize) != 0)
         return -1;
      rd = eol + 2;
      if (csize == 0)
      {
         *out_len = wr;
         return 0;
      }
      if (csize > n - rd || n - rd - csize < 2)
         return -1;
      if (buf[rd + csize] != '\r' || buf[rd + csize + 1] != '\n')
         return -1;
      memmove(buf + wr, buf + rd, csize);
      wr += csize;
      rd += csize + 2;
   }
}

static int http_open(const char *host, int portnum)
{
   char port[8], key[sizeof(g_addr_key)];
   struct addrinfo hints, *ai = NULL;
   struct sockaddr_storage sa;
   socklen_t sa_len = 0;
   struct timeval tv;
   int s = -1;
   int primary = !strcmp(host, g_host) && portnum == g_port;

   snprintf(port, sizeof(port), "%d", portnum);
   snprintf(key, sizeof(key), "%s:%s", host, port);
   if (primary && g_addr_len && !strcmp(key, g_addr_key))
   {
      memcpy(&sa, &g_addr, g_addr_len);
      sa_len = g_addr_len;
   }
   else
   {
      memset(&hints, 0, sizeof(hints));
      hints.ai_family = AF_INET;
      hints.ai_socktype = SOCK_STREAM;
      if (primary)
         g_addr_len = 0;
      if (getaddrinfo(host, port, &hints, &ai) != 0 || !ai)
      {
         if (ai)
            freeaddrinfo(ai);
         return -1;
      }
      if (ai->ai_addrlen <= sizeof(sa))
      {
         memcpy(&sa, ai->ai_addr, ai->ai_addrlen);
         sa_len = (socklen_t)ai->ai_addrlen;
         if (primary)
         {
            memcpy(&g_addr, &sa, sa_len);
            g_addr_len = sa_len;
            snprintf(g_addr_key, sizeof(g_addr_key), "%s", key);
         }
      }
      freeaddrinfo(ai);
      if (!sa_len)
         return -1;
   }
   s = socket(AF_INET, SOCK_STREAM, 0);
   if (s < 0)
      return -1;
   /* 1 s recv slices let stop_now() abort promptly while keeping a 15 s
    * overall transfer timeout below. */
   tv.tv_sec = 1;
   tv.tv_usec = 0;
   setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
   tv.tv_sec = 15;
   tv.tv_usec = 0;
   setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#ifdef SO_NOSIGPIPE
   {
      int one = 1;
      setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
   }
#endif
#ifdef TCP_NODELAY
   {
      int one = 1;
      setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
   }
#endif
   if (connect(s, (const struct sockaddr *)&sa, sa_len) != 0)
   {
      if (primary)
         g_addr_len = 0;                  /* look it up again next time */
      close(s);
      return -1;
   }
   g_http_sock = s;
   snprintf(g_http_host, sizeof(g_http_host), "%s", host);
   g_http_port = portnum;
   return s;
}

static int http_get_once(const char *host, int portnum, const char *path,
                         size_t *len, char *loc, size_t loc_cap)
{
   char req[2048];
   int req_len, attempt;
   int primary = !strcmp(host, g_host) && portnum == g_port;

   loc[0] = '\0';
   req_len = snprintf(req, sizeof(req),
                      "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: VitAdhoc\r\n"
                      "Accept: image/png\r\nConnection: keep-alive\r\n\r\n",
                      path, host);
   if (req_len < 0 || req_len >= (int)sizeof(req))
      return GET_FAILED;

   for (attempt = 0; attempt < 2; attempt++)
   {
      size_t got = 0, body_at = 0, want = (size_t)-1, chunk_end = 0;
      int status = 0, chunked = 0, keep = 0, reused = 0, rc = GET_FAILED;
      int http_minor = 0;
      uint64_t deadline;

      if (stop_hard())
      {
         http_close();
         return GET_FAILED;
      }
      if (g_http_sock >= 0 &&
          (strcmp(g_http_host, host) != 0 || g_http_port != portnum))
         http_close();
      reused = g_http_sock >= 0;
      if (!reused && http_open(host, portnum) < 0)
         return GET_FAILED;
      if (send(g_http_sock, req, (size_t)req_len, AF_SEND_FLAGS) != (ssize_t)req_len)
      {
         http_close();
         if (reused)
            continue;
         return GET_FAILED;
      }
      deadline = worker_now() + AF_RECV_MAX_US;
      for (;;)
      {
         ssize_t r;
         if (stop_hard())
         {
            http_close();
            return GET_FAILED;
         }
         if (got >= AF_MAX_BYTES)
         {
            http_close();
            return GET_FAILED;            /* not a box art picture */
         }
         r = recv(g_http_sock, g_body + got, AF_MAX_BYTES - got, 0);
         if (r < 0)
         {
            int e = errno;
            if ((e == EAGAIN || e == EWOULDBLOCK || e == ETIMEDOUT || e == EINTR) &&
                !stop_hard() && worker_now() < deadline)
               continue;
            http_close();
            if (reused && got == 0)
               goto retry_fresh;
            if (primary && !reused)
               g_addr_len = 0;
            return GET_FAILED;
         }
         if (r == 0)
         {
            http_close();
            if (reused && got == 0)
               goto retry_fresh;          /* server closed idle keep-alive socket */
            keep = 0;
            break;
         }
         got += (size_t)r;
         if (!body_at)
         {
            char *hdr_end;
            g_body[got] = '\0';
            hdr_end = strstr((char *)g_body, "\r\n\r\n");
            if (hdr_end)
            {
               char *p;
               *hdr_end = '\0';
               body_at = (size_t)(hdr_end + 4 - (char *)g_body);
               if (sscanf((char *)g_body, "HTTP/1.%d %d", &http_minor, &status) != 2)
               {
                  http_close();
                  return GET_FAILED;
               }
               if (status == 301 || status == 302 || status == 303 ||
                   status == 307 || status == 308)
                  extract_location((char *)g_body, loc, loc_cap);
               for (p = (char *)g_body; *p; p++)
                  if (*p >= 'A' && *p <= 'Z')
                     *p = (char)(*p - 'A' + 'a');
               p = strstr((char *)g_body, "\r\ncontent-length:");
               if (p)
               {
                  char *ep = NULL;
                  unsigned long long cl;
                  p += 17;
                  while (*p == ' ' || *p == '\t') p++;
                  if (*p >= '0' && *p <= '9')
                  {
                     cl = strtoull(p, &ep, 10);
                     if (cl <= AF_MAX_BYTES)
                        want = (size_t)cl;
                     else
                        want = AF_MAX_BYTES + 1;
                  }
               }
               chunked = has_hdr_token((char *)g_body, "transfer-encoding:", "chunked");
               keep = (http_minor >= 1) &&
                      !has_hdr_token((char *)g_body, "connection:", "close");
               if (chunked && want != (size_t)-1)
               {
                  /* RFC 9112 s6.3: Transfer-Encoding overrides Content-Length;
                   * do not reuse the connection on ambiguous framing. */
                  want = (size_t)-1;
                  keep = 0;
               }
               if (status == 200 && want != (size_t)-1 &&
                   (want > AF_MAX_BYTES || body_at + want > AF_MAX_BYTES))
               {
                  http_close();
                  return GET_FAILED;
               }
               if (status == 204 || status == 304)
                  break;                  /* no body by RFC 9110 */
               if (status != 200)
               {
                  /* Drain small framed error/redirect bodies so the TCP
                   * connection stays alive across consecutive 404 misses. */
                  if (!keep)
                     break;
                  if (want != (size_t)-1)
                  {
                     if (want > AF_DRAIN_CAP || body_at + want > AF_MAX_BYTES)
                     {
                        keep = 0;
                        break;
                     }
                     if (got - body_at >= want)
                     {
                        if (got - body_at > want)
                           keep = 0;
                        break;
                     }
                  }
                  else if (chunked)
                  {
                     int ck = chunked_complete(g_body + body_at, got - body_at, &chunk_end);
                     if (ck < 0) { keep = 0; break; }
                     if (ck > 0)
                     {
                        if (chunk_end != got - body_at)
                           keep = 0;
                        break;
                     }
                     if (got - body_at >= AF_DRAIN_CAP)
                     {
                        keep = 0;
                        break;
                     }
                  }
                  else
                  {
                     keep = 0;
                     break;
                  }
                  continue;
               }
               if (want == (size_t)-1 && !chunked)
                  keep = 0;               /* close-delimited body */
            }
            else if (got >= 16384u)
            {
               http_close();
               return GET_FAILED;         /* absurdly large response headers */
            }
         }
         if (body_at)
         {
            size_t body_n = got - body_at;
            if (status != 200)
            {
               if (want != (size_t)-1)
               {
                  if (body_n >= want)
                  {
                     if (body_n > want) keep = 0;
                     break;
                  }
               }
               else if (chunked)
               {
                  int ck = chunked_complete(g_body + body_at, body_n, &chunk_end);
                  if (ck < 0) { keep = 0; break; }
                  if (ck > 0)
                  {
                     if (chunk_end != body_n) keep = 0;
                     break;
                  }
                  if (body_n >= AF_DRAIN_CAP) { keep = 0; break; }
               }
            }
            else
            {
               if (want != (size_t)-1 && body_n >= want)
               {
                  if (body_n > want) keep = 0;
                  break;
               }
               if (chunked)
               {
                  int ck = chunked_complete(g_body + body_at, body_n, &chunk_end);
                  if (ck < 0)
                  {
                     http_close();
                     return GET_FAILED;
                  }
                  if (ck > 0)
                  {
                     if (chunk_end != body_n) keep = 0;
                     break;
                  }
               }
            }
         }
      }
      if (!keep)
         http_close();
      if (status == 301 || status == 302 || status == 303 ||
          status == 307 || status == 308)
         rc = GET_REDIRECT;
      else if (status == 404 || status == 410)
         rc = GET_MISSING;
      else if (status == 200 && body_at)
      {
         size_t n = got - body_at;
         memmove(g_body, g_body + body_at, n);
         if (chunked)
         {
            size_t dec_n = 0;
            if (chunked_decode(g_body, n, &dec_n) != 0)
            {
               http_close();
               return GET_FAILED;
            }
            n = dec_n;
         }
         else if (want != (size_t)-1)
         {
            if (n < want)
            {
               http_close();
               return GET_FAILED;         /* cut short */
            }
            n = want;
         }
         g_body[n] = '\0';
         *len = n;
         rc = GET_OK;
      }
      else
         http_close();
      return rc;
   retry_fresh:
      ;
   }
   return GET_FAILED;
}

/* GET http://g_host:g_port/<path>, following up to 3 redirects.  GET_OK with
 * the body in g_body (*len bytes), GET_MISSING for 404/410, GET_FAILED
 * otherwise (no network, a timeout, anything else). */
static int http_get(const char *path, size_t *len)
{
   char cur_host[sizeof(g_host)], cur_path[1400], loc[1400];
   int cur_port = g_port, hop;
   snprintf(cur_host, sizeof(cur_host), "%s", g_host);
   snprintf(cur_path, sizeof(cur_path), "%s", path);
   for (hop = 0; hop < 4; hop++)
   {
      int rc = http_get_once(cur_host, cur_port, cur_path, len, loc, sizeof(loc));
      if (rc != GET_REDIRECT)
         return rc;
      if (hop == 3 ||
          apply_redirect(loc, cur_host, sizeof(cur_host), &cur_port,
                         cur_path, sizeof(cur_path)) != 0)
         return GET_FAILED;
   }
   return GET_FAILED;
}

/* The thumbnail file for a cover: cache/boxart/<stem>.t2.565, the picture fitted into
 * ART_THUMB_W x ART_THUMB_H (never enlarged), box-averaged, as RGB565 behind
 * the same 16-byte header the app's .v565 files use, so the home screen reads
 * it with its ordinary loader.  Written whole or not at all.  0 = written,
 * -2 = not a picture stb can read, -3 = a picture too big to shrink here,
 * -1 = anything that may pass (stopping, memory, the card). */
#define THUMB_MAGIC 0x52353635u     /* '565R' little-endian, as ui_psp.c */
static int thumb_rgb(const unsigned char *pix, int w, int h, const char *dst,
                     int square);

static int thumb_write(const unsigned char *png, size_t len, const char *dst,
                       int square)
{
   unsigned char *pix;
   int w = 0, h = 0, nc = 0, rc;

   if (!png || len == 0 || stop_hard())
      return -1;
   thumb_dir_make();
   if (len > AF_MAX_BYTES ||
       !stbi_info_from_memory(png, (int)len, &w, &h, &nc) ||
       w <= 0 || h <= 0)
      return -2;
   if ((uint64_t)w * h > 1600u * 1600u)
      return -3;                       /* a picture, too big to shrink here */
   pix = stbi_load_from_memory(png, (int)len, &w, &h, &nc, 3);
   if (!pix)
      return strcmp(stbi_failure_reason() ? stbi_failure_reason() : "",
                    "outofmem") ? -2 : -1;
   if (w <= 0 || h <= 0)
   {
      stbi_image_free(pix);
      return -2;
   }
   rc = thumb_rgb(pix, w, h, dst, square);
   stbi_image_free(pix);
   return rc;
}

/* Write a complete in-memory buffer to `path`. On Vita, bypass newlib stdio
 * completely so the entire PNG or thumbnail lands in one sceIoWrite syscall. */
static int af_write_file(const char *path, const void *buf, size_t len)
{
#ifdef __vita__
   int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
   const unsigned char *p = (const unsigned char *)buf;
   size_t left = len;
   int ok = 1;
   if (fd < 0)
      return -1;
   while (left > 0)
   {
      ssize_t w = write(fd, p, left);
      if (w <= 0)
      {
         if (w < 0 && errno == EINTR)
            continue;
         ok = 0;
         break;
      }
      p += (size_t)w;
      left -= (size_t)w;
   }
   if (close(fd) != 0)
      ok = 0;
   return ok ? 0 : -1;
#else
   FILE *f = fopen(path, "wb");
   int ok;
   if (!f)
      return -1;
   ok = fwrite(buf, 1, len, f) == len;
   ok = fclose(f) == 0 && ok;
   return ok ? 0 : -1;
#endif
}

/* The same from decoded RGB.  `square`: the player's own cover (cover_custom.h)
 * is shown whole -- fitted, enlarged if small, on a square of its average
 * colour -- where a downloaded one is fitted and left at its shape (the tile
 * crops the middle square of that). */
static int thumb_rgb(const unsigned char *pix, int w, int h, const char *dst,
                     int square)
{
   char tmp[AF_PATH_CAP + AF_NAME_CAP + 40];
   unsigned char *file_buf, *hdr, *out;
   int x0s[ART_THUMB_W], x1s[ART_THUMB_W];
   int tw, th, cw, ch, ox, oy, x, y, rc = -1;
   size_t row_stride, total_bytes;
   double s;

   if (!pix || w <= 0 || h <= 0)
      return -2;
   if (square)
   {
      s = (double)ART_THUMB_W / w;
      if ((double)ART_THUMB_W / h < s)
         s = (double)ART_THUMB_W / h;
      if (s > 4.0)
         s = 4.0;
      cw = ch = ART_THUMB_W;
   }
   else
   {
      s = (double)ART_THUMB_W / w;
      if ((double)ART_THUMB_H / h < s)
         s = (double)ART_THUMB_H / h;
      if (s > 1.0)
         s = 1.0;
      cw = ch = 0;
   }
   tw = (int)(w * s + 0.5);
   th = (int)(h * s + 0.5);
   if (tw < 1) tw = 1;
   if (th < 1) th = 1;
   if (tw > ART_THUMB_W) tw = ART_THUMB_W;
   if (th > (square ? ART_THUMB_W : ART_THUMB_H)) th = square ? ART_THUMB_W : ART_THUMB_H;
   if (!square)
   {
      cw = tw;
      ch = th;
   }
   ox = (cw - tw) / 2;
   oy = (ch - th) / 2;
   total_bytes = 16u + (size_t)cw * (size_t)ch * 2u;
   file_buf = (unsigned char *)malloc(total_bytes);
   if (!file_buf)
      return -1;
   hdr = file_buf;
   out = file_buf + 16;
   if (square && (tw < cw || th < ch))
   {
      /* the bars: the picture's average colour, darkened */
      unsigned long long sr = 0, sg = 0, sb = 0, cnt = 0;
      unsigned v;
      int yy, xx;
      for (yy = 0; yy < h; yy += 1 + h / 64)
         for (xx = 0; xx < w; xx += 1 + w / 64)
         {
            const unsigned char *p = pix + ((size_t)yy * w + xx) * 3;
            sr += p[0]; sg += p[1]; sb += p[2];
            cnt++;
         }
      if (!cnt) cnt = 1;
      v = (unsigned)((sr / cnt) * 4 / 10) >> 3 |
          ((unsigned)((sg / cnt) * 4 / 10) >> 2) << 5 |
          ((unsigned)((sb / cnt) * 4 / 10) >> 3) << 11;
      for (x = 0; x < cw * ch; x++)
      {
         out[2 * x]     = (unsigned char)(v & 0xFF);
         out[2 * x + 1] = (unsigned char)(v >> 8);
      }
   }
   /* Precompute horizontal spans once and use exact 64-bit reciprocal division
    * (Cortex-A9 has no hardware integer divide; box-count n changes at most
    * twice per output row). */
   for (x = 0; x < tw; x++)
   {
      int xa = x * w / tw, xb = (x + 1) * w / tw;
      if (xb <= xa) xb = xa + 1;
      if (xb > w) xb = w;
      x0s[x] = xa;
      x1s[x] = xb;
   }
   row_stride = (size_t)w * 3u;
   for (y = 0; y < th; y++)
   {
      int y0 = y * h / th, y1 = (y + 1) * h / th, yy;
      unsigned prev_n = 0;
      uint64_t inv = 0;
      unsigned char *dst_row;
      if (y1 <= y0) y1 = y0 + 1;
      if (y1 > h) y1 = h;
      dst_row = out + 2u * ((size_t)(y + oy) * (size_t)cw + (size_t)ox);
      for (x = 0; x < tw; x++)
      {
         int x0 = x0s[x], x1 = x1s[x];
         unsigned n = (unsigned)(y1 - y0) * (unsigned)(x1 - x0);
         unsigned r = 0, g = 0, b = 0, v;
         size_t span_bytes = (size_t)(x1 - x0) * 3u;
         for (yy = y0; yy < y1; yy++)
         {
            const unsigned char *p = pix + (size_t)yy * row_stride + (size_t)x0 * 3u;
            const unsigned char *pe = p + span_bytes;
            for (; p < pe; p += 3)
            {
               r += p[0]; g += p[1]; b += p[2];
            }
         }
         if (n <= 4096u)
         {
            if (n != prev_n)
            {
               prev_n = n;
               inv = ((1ull << 32) + n - 1u) / n;
            }
            r = (unsigned)(((uint64_t)r * inv) >> 32);
            g = (unsigned)(((uint64_t)g * inv) >> 32);
            b = (unsigned)(((uint64_t)b * inv) >> 32);
         }
         else
         {
            r /= n; g /= n; b /= n;
         }
         v = (r >> 3) | ((g >> 2) << 5) | ((b >> 3) << 11);
         dst_row[2 * x]     = (unsigned char)(v & 0xFF);
         dst_row[2 * x + 1] = (unsigned char)(v >> 8);
      }
   }

   memset(hdr, 0, 16);
   hdr[0] = THUMB_MAGIC & 0xFF;
   hdr[1] = (THUMB_MAGIC >> 8) & 0xFF;
   hdr[2] = (THUMB_MAGIC >> 16) & 0xFF;
   hdr[3] = (THUMB_MAGIC >> 24) & 0xFF;
   hdr[4] = (unsigned char)(cw & 0xFF);
   hdr[5] = (unsigned char)(cw >> 8);
   hdr[6] = (unsigned char)(ch & 0xFF);
   hdr[7] = (unsigned char)(ch >> 8);
   hdr[8] = (unsigned char)(cw & 0xFF);       /* fstride = cw */
   hdr[9] = (unsigned char)(cw >> 8);

   /* (a name of its own: the UI thread and the worker may both make one) */
   {
      static unsigned seq;
      snprintf(tmp, sizeof(tmp), "%s.tmp%u", dst,
               __atomic_add_fetch(&seq, 1u, __ATOMIC_RELAXED));
   }
   if (af_write_file(tmp, file_buf, total_bytes) == 0 && rename(tmp, dst) == 0)
      rc = 0;
   else
      remove(tmp);
   free(file_buf);
   return rc;
}

int artfetch_thumb_rgb(const unsigned char *rgb, int w, int h,
                       const char *dst, int square)
{
   return thumb_rgb(rgb, w, h, dst, square);
}

/* 1 when the player has set a cover of their own for this stem
 * (boxart/<stem>.own or cache/boxart/<stem>.own, written by cover_custom.c):
 * its thumbnail is the whole picture on a square. */
static int own_cover(const char *stem)
{
   char path[AF_PATH_CAP + AF_NAME_CAP + 32];
   struct stat st;
   if (g_inventory_valid)
      return set_has(&g_inventory[INV_OWN], stem);
   snprintf(path, sizeof(path), "%s/boxart/%s.own", g_root, stem);
   if (stat(path, &st) == 0)
      return 1;
   snprintf(path, sizeof(path), "%s/" ART_THUMB_DIR "/%s.own", g_root, stem);
   return stat(path, &st) == 0;
}

/* The picture to <kind dir>/<stem>.png, whole or not at all. */
static int save(int k, const char *stem, size_t len)
{
   char tmp[AF_PATH_CAP + AF_NAME_CAP + 32], dst[sizeof(tmp)];
   int wr;
   if (stop_hard())
      return -1;
#ifdef __vita__
   /* Vita runs one app at a time, cover_custom triggers artfetch_rescan()
    * (stop_hard()), and rename() refuses to overwrite an existing dst:
    * skipping 6 linear FAT32 stat() scans per cover avoids ~1 s of card I/O. */
   if (!g_inventory_valid && have_art_disk(k, stem))
   {
      inventory_add(k, stem);
      return 0;
   }
#else
   if (have_art_disk(k, stem))
   {
      inventory_add(k, stem);
      return 0;
   }
#endif
   if (!g_dir_made[k])
   {
      snprintf(dst, sizeof(dst), "%s/%s", g_root, KIND_DIR[k]);
      mkdir(dst, 0777);                /* titles/ is made on the first one */
      g_dir_made[k] = 1;
   }
   snprintf(dst, sizeof(dst), "%s/%s/%s.png", g_root, KIND_DIR[k], stem);
   snprintf(tmp, sizeof(tmp), "%s/%s/%s.png.tmp", g_root, KIND_DIR[k], stem);
   wr = af_write_file(tmp, g_body, len);
   if (wr != 0 && errno == ENOENT)
   {
      char dir[AF_PATH_CAP + 32];
      snprintf(dir, sizeof(dir), "%s/%s", g_root, KIND_DIR[k]);
      mkdir(dir, 0777);
      wr = af_write_file(tmp, g_body, len);
   }
   if (wr != 0 || stop_hard() || rename(tmp, dst) != 0)
   {
      remove(tmp);
      return -1;
   }
   if (k == AK_BOX)
   {
      /* the home screen's tile picture, made now while the bytes are here */
      char th[AF_PATH_CAP + AF_NAME_CAP + 40];
      int rc;
      snprintf(th, sizeof(th), "%s/" ART_THUMB_DIR "/%s%s", g_root, stem,
               ART_THUMB_SUFFIX);
      rc = thumb_write((const unsigned char *)g_body, len, th, 0);
      if (rc == 0)
         inventory_add_path(INV_THUMB, stem, th);
      else if (rc == -2)
      {
         /* It had a PNG's first bytes and is not a picture: a transfer cut
          * short, or a broken file.  Not kept (art on the card is never
          * fetched again): a failure, tried again after the cool-down. */
         remove(dst);
         return -1;
      }
      if (set_add(&g_inventory[INV_BOX_PNG], stem) != 0)
         g_inventory_valid = 0;
   }
   inventory_add_path(k, stem, dst);
   return 0;
}

/* One cartridge with a picture but no thumbnail: make it from the picture
 * that is already on the card (the thread's background pass, below). */
static int warm_one(const char *name)
{
   static const char *const ext[2] = { "png", "jpg" };
   char stem[AF_NAME_CAP], src[AF_PATH_CAP + AF_NAME_CAP + 40];
   char th[AF_PATH_CAP + AF_NAME_CAP + 40];
   struct stat st;
   unsigned char *buf;
   FILE *f;
   size_t n;
   int written = 0;
   unsigned e = 2;

   stem_of(name, stem, sizeof(stem));
   if (g_inventory_valid)
   {
      int has_png, has_jpg;
      if (set_has(&g_inventory[INV_THUMB], stem))
         return 0;
      has_png = set_has(&g_inventory[INV_BOX_PNG], stem);
      has_jpg = !has_png && set_has(&g_inventory[INV_BOX_JPG], stem);
      if (!has_png && !has_jpg)
         return 0;
      if (set_has(&g_thumb_failed, stem))
         return 0;
      nav_yield();
      if (stop_now())
         return 0;
      snprintf(th, sizeof(th), "%s/" ART_THUMB_DIR "/%s%s", g_root, stem,
               ART_THUMB_SUFFIX);
      e = has_png ? 0u : 1u;
      snprintf(src, sizeof(src), "%s/boxart/%s.%s", g_root, stem, ext[e]);
      if (stat(src, &st) != 0)
         return 0;
   }
   else
   {
      if (set_has(&g_thumb_failed, stem))
         return 0;
      nav_yield();
      if (stop_now())
         return 0;
      snprintf(th, sizeof(th), "%s/" ART_THUMB_DIR "/%s%s", g_root, stem,
               ART_THUMB_SUFFIX);
      if (stat(th, &st) == 0)
         return 0;
      for (e = 0; e < 2; e++)
      {
         snprintf(src, sizeof(src), "%s/boxart/%s.%s", g_root, stem, ext[e]);
         if (stat(src, &st) == 0)
            break;
      }
      if (e == 2)
         return 0;
   }
   if (st.st_size <= 0 || st.st_size > (off_t)AF_MAX_BYTES)
   {
      set_add(&g_thumb_failed, stem);
      return 0;
   }
   f = fopen(src, "rb");
   if (!f)
      return 0;
   buf = malloc((size_t)st.st_size);
   n = buf ? fread(buf, 1, (size_t)st.st_size, f) : 0;
   fclose(f);
   if (buf && n == (size_t)st.st_size)
   {
      int rc = thumb_write(buf, n, th, own_cover(stem));
      written = rc == 0;
      if (rc == -2 || rc == -3)
         set_add(&g_thumb_failed, stem);   /* its content, not the moment */
   }
   free(buf);
   if (written)
      inventory_add_path(INV_THUMB, stem, th);
   return written;
}

/* 0 GBA, 1 GB, 2 GBC, 3 SNES, 4 NES, 5 Famicom Disk System, 6 WonderSwan,
 * 7 WonderSwan Color, 8 Genesis / Mega Drive, 9 Master System, 10 Game Gear,
 * 11 SG-1000, 12 Sega CD (gamedb_sega.h has the same folder names; the game
 * database's system letters are A..M in this order) */
static const char *const SYS[13] = {
   "Nintendo%20-%20Game%20Boy%20Advance", "Nintendo%20-%20Game%20Boy",
   "Nintendo%20-%20Game%20Boy%20Color",
   "Nintendo%20-%20Super%20Nintendo%20Entertainment%20System",
   "Nintendo%20-%20Nintendo%20Entertainment%20System",
   "Nintendo%20-%20Family%20Computer%20Disk%20System",
   "Bandai%20-%20WonderSwan", "Bandai%20-%20WonderSwan%20Color",
   "Sega%20-%20Mega%20Drive%20-%20Genesis",
   "Sega%20-%20Master%20System%20-%20Mark%20III",
   "Sega%20-%20Game%20Gear", "Sega%20-%20SG-1000",
   "Sega%20-%20Mega-CD%20-%20Sega%20CD" };

/* Corresponding GitHub libretro-thumbnails repository names (HTTPS mirror). */
static const char *const SYS_GH[13] = {
   "Nintendo_-_Game_Boy_Advance", "Nintendo_-_Game_Boy",
   "Nintendo_-_Game_Boy_Color",
   "Nintendo_-_Super_Nintendo_Entertainment_System",
   "Nintendo_-_Nintendo_Entertainment_System",
   "Nintendo_-_Family_Computer_Disk_System",
   "Bandai_-_WonderSwan", "Bandai_-_WonderSwan_Color",
   "Sega_-_Mega_Drive_-_Genesis",
   "Sega_-_Master_System_-_Mark_III",
   "Sega_-_Game_Gear", "Sega_-_SG-1000",
   "Sega_-_Mega-CD_-_Sega_CD" };

typedef struct
{
   char stem[AF_NAME_CAP], cand[5 + AF_ALT_MAX][AF_NAME_CAP];
   int ncand, sys[2], nsys, ready, crc_allowed, crc_tried, crc_unavailable;
   int no_crc;                  /* UNIF / disc: the checksum names nothing */
   int is_archive;              /* .zip / .7z: inspect inner entry, never raw-hash container */
   int alts_done;               /* the fallback names (artalt.h) are in cand[] */
   int hit_cand_1, hit_sys_1;   /* 1-based winning (cand, sys) from AK_BOX for AK_TITLE reuse */
} af_candidates;

/* The cartridge's checksum as the console's catalogue has it: SNES without
 * its copier header, NES / Famicom Disk System without theirs. */
static int cart_crc(const af_candidates *a, const char *path, uint32_t *crc)
{
   if (a->is_archive)
      return -1;
   return a->sys[0] == 3 ? gamedb_snes_crc_cancel(path, crc, cart_crc_stop)
        : a->sys[0] == 4 || a->sys[0] == 5 ? gamedb_nes_crc_cancel(path, crc, cart_crc_stop)
        : a->sys[0] == 8 ? gamedb_md_crc_cancel(path, crc, cart_crc_stop)
        : a->sys[0] >= 9 ? gamedb_sms_crc_cancel(path, crc, cart_crc_stop)
        : gamedb_file_crc_cancel(path, crc, cart_crc_stop);
}

/* Weak fallback when artfetch.c is compiled standalone without sz_rom.c. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((weak))
int sz_rom_peek(const char *path, zip_rom_entry *out,
                int (*name_console)(const char *, size_t))
{
   (void)path;
   (void)out;
   (void)name_console;
   return -1;
}
#endif

static uint16_t af_u16le(const unsigned char *p)
{
   return (uint16_t)(p[0] | ((unsigned)p[1] << 8));
}

static uint32_t af_u32le(const unsigned char *p)
{
   return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
          ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int af_ext_sys(const char *ext, int sys[2], int *nsys, int *no_crc)
{
   *no_crc = 0;
   if (!ext)
      return 0;
   if (!strcasecmp(ext, ".gba"))
      { sys[0] = 0; *nsys = 1; return 1; }
   if (!strcasecmp(ext, ".sfc") || !strcasecmp(ext, ".smc") ||
       !strcasecmp(ext, ".swc") || !strcasecmp(ext, ".fig"))
      { sys[0] = 3; *nsys = 1; return 1; }
   if (!strcasecmp(ext, ".fds"))
      { sys[0] = 5; *nsys = 1; return 1; }
   if (!strcasecmp(ext, ".nes") || !strcasecmp(ext, ".unf") ||
       !strcasecmp(ext, ".unif"))
   {
      sys[0] = 4; *nsys = 1;
      *no_crc = strcasecmp(ext, ".nes") != 0;
      return 1;
   }
   if (!strcasecmp(ext, ".ws") || !strcasecmp(ext, ".pc2"))
      { sys[0] = 6; sys[1] = 7; *nsys = 2; return 1; }
   if (!strcasecmp(ext, ".wsc"))
      { sys[0] = 7; sys[1] = 6; *nsys = 2; return 1; }
   if (!strcasecmp(ext, ".md") || !strcasecmp(ext, ".gen") ||
       !strcasecmp(ext, ".smd") || !strcasecmp(ext, ".bin") ||
       !strcasecmp(ext, ".68k"))
      { sys[0] = 8; *nsys = 1; return 1; }
   if (!strcasecmp(ext, ".sms") || !strcasecmp(ext, ".mark3"))
      { sys[0] = 9; sys[1] = 10; *nsys = 2; return 1; }
   if (!strcasecmp(ext, ".gg"))
      { sys[0] = 10; *nsys = 1; return 1; }
   if (!strcasecmp(ext, ".sg") || !strcasecmp(ext, ".sc") ||
       !strcasecmp(ext, ".sg1000"))
      { sys[0] = 11; *nsys = 1; return 1; }
   if (!strcasecmp(ext, ".cue") || !strcasecmp(ext, ".chd") ||
       !strcasecmp(ext, ".iso") || !strcasecmp(ext, ".m3u"))
      { sys[0] = 12; *nsys = 1; *no_crc = 1; return 1; }
   if (!strcasecmp(ext, ".gbc"))
      { sys[0] = 2; sys[1] = 1; *nsys = 2; return 1; }
   if (!strcasecmp(ext, ".gb") || !strcasecmp(ext, ".sgb"))
      { sys[0] = 1; sys[1] = 2; *nsys = 2; return 1; }
   return 0;
}

static int af_is_disc_track_bin(const char *leaf, const char *ext)
{
   const char *tr;
   if (!ext || strcasecmp(ext, ".bin"))
      return 0;
   tr = strstr(leaf, "(Track ");
   if (tr && tr[7] >= '0' && tr[7] <= '9')
   {
      const char *d = tr + 7;
      while (*d >= '0' && *d <= '9')
         d++;
      if (d[0] == ')' && !strcasecmp(d + 1, ".bin"))
         return 1;
   }
   return 0;
}

static int af_sz_console(const char *name, size_t len)
{
   char buf[AF_NAME_CAP];
   const char *leaf, *ext;
   int sys[2], nsys = 0, no_crc = 0;
   size_t i;
   if (!name || len == 0 || len >= sizeof(buf))
      return -1;
   memcpy(buf, name, len);
   buf[len] = '\0';
   leaf = buf;
   for (i = 0; i < len; i++)
      if (buf[i] == '/' || buf[i] == '\\')
         leaf = buf + i + 1;
   if (!leaf[0] || leaf[0] == '.')
      return -1;
   ext = strrchr(leaf, '.');
   if (af_is_disc_track_bin(leaf, ext))
      return -1;
   if (!af_ext_sys(ext, sys, &nsys, &no_crc))
      return -1;
   /* Avoid returning 6 (which sz_rom_peek treats as Genesis < 16 KiB size check)
    * unless it actually is Genesis/MD, where a small file can't be a Genesis ROM anyway. */
   return sys[0] == 8 ? 6 : sys[0] == 6 ? 16 : sys[0];
}

typedef struct
{
   char inner_stem[AF_NAME_CAP];
   char inner_ext[16];
   uint32_t crc;
   int has_crc;
} af_arc_info;

static int af_peek_zip(FILE *f, af_arc_info *out)
{
   long fsz, tail_off, pos;
   size_t tail_len, i, cd_read;
   unsigned char *buf, *cd;
   uint32_t cd_size = 0, cd_off = 0, best_sz = 0;
   uint16_t entries = 0, e;
   int eocd = -1, found = 0;

   if (fseek(f, 0, SEEK_END) != 0)
      return -1;
   fsz = ftell(f);
   if (fsz < 22)
      return -1;
   tail_len = (size_t)(fsz > 65557L ? 65557L : fsz);
   tail_off = fsz - (long)tail_len;
   buf = malloc(tail_len);
   if (!buf)
      return -1;
   if (fseek(f, tail_off, SEEK_SET) != 0 || fread(buf, 1, tail_len, f) != tail_len)
   {
      free(buf);
      return -1;
   }
   for (i = tail_len - 22 + 1; i > 0; i--)
   {
      size_t k = i - 1;
      if (buf[k] == 0x50 && buf[k + 1] == 0x4B && buf[k + 2] == 0x05 && buf[k + 3] == 0x06)
      {
         uint16_t clen = af_u16le(buf + k + 20);
         if ((size_t)(k + 22 + clen) == tail_len)
         {
            eocd = (int)k;
            entries = af_u16le(buf + k + 10);
            cd_size = af_u32le(buf + k + 12);
            cd_off = af_u32le(buf + k + 16);
            break;
         }
      }
   }
   free(buf);
   if (eocd < 0 || cd_size == 0 || cd_size > (8u << 20) ||
       (long)cd_off + (long)cd_size > fsz)
      return -1;
   cd = malloc(cd_size);
   if (!cd)
      return -1;
   if (fseek(f, (long)cd_off, SEEK_SET) != 0 || fread(cd, 1, cd_size, f) != cd_size)
   {
      free(cd);
      return -1;
   }
   pos = 0;
   cd_read = 0;
   for (e = 0; e < entries && cd_read + 46 <= cd_size; e++)
   {
      const unsigned char *hdr = cd + cd_read;
      uint16_t nlen, xlen, clen;
      uint32_t crc, usz;
      char ename[AF_NAME_CAP];
      const char *leaf, *ext;
      int sys[2], nsys, no_crc;
      size_t copy_n, j;

      if (hdr[0] != 0x50 || hdr[1] != 0x4B || hdr[2] != 0x01 || hdr[3] != 0x02)
         break;
      crc = af_u32le(hdr + 16);
      usz = af_u32le(hdr + 24);
      nlen = af_u16le(hdr + 28);
      xlen = af_u16le(hdr + 30);
      clen = af_u16le(hdr + 32);
      if (cd_read + 46 + nlen + xlen + clen > cd_size)
         break;
      copy_n = nlen < sizeof(ename) - 1 ? nlen : sizeof(ename) - 1;
      memcpy(ename, hdr + 46, copy_n);
      ename[copy_n] = '\0';
      {
         int is_dir = (nlen > 0 && hdr[46 + nlen - 1] == '/');
         cd_read += 46 + nlen + xlen + clen;

         if (nlen == 0 || is_dir ||
             strncmp(ename, "__MACOSX/", 9) == 0)
            continue;
      }
      leaf = ename;
      for (j = 0; ename[j]; j++)
         if (ename[j] == '/' || ename[j] == '\\')
            leaf = ename + j + 1;
      if (!leaf[0] || leaf[0] == '.')
         continue;
      ext = strrchr(leaf, '.');
      if (af_is_disc_track_bin(leaf, ext) ||
          !af_ext_sys(ext, sys, &nsys, &no_crc))
         continue;
      (void)pos;
      if (!found || usz > best_sz)
      {
         stem_of(leaf, out->inner_stem, sizeof(out->inner_stem));
         snprintf(out->inner_ext, sizeof(out->inner_ext), "%s", ext);
         out->crc = crc;
         out->has_crc = (usz > 0 || crc != 0);
         best_sz = usz;
         found = 1;
      }
   }
   free(cd);
   return found ? 0 : -1;
}

static int af_peek_archive(const char *path, const char *ext, af_arc_info *out)
{
   static const unsigned char SZ_SIG[6] = { '7', 'z', 0xBC, 0xAF, 0x27, 0x1C };
   unsigned char sig[6];
   FILE *f;
   size_t n;
   int is_sz;

   memset(out, 0, sizeof(*out));
   if (!path || !path[0])
      return -1;
   f = fopen(path, "rb");
   if (!f)
      return -1;
   n = fread(sig, 1, sizeof(sig), f);
   is_sz = (n == 6 && !memcmp(sig, SZ_SIG, 6)) || (ext && !strcasecmp(ext, ".7z"));
   if (n == 6 && !memcmp(sig, SZ_SIG, 6))
      is_sz = 1;
   else if (n >= 2 && sig[0] == 'P' && sig[1] == 'K')
      is_sz = 0;
   if (!is_sz)
   {
      int r = af_peek_zip(f, out);
      fclose(f);
      return r;
   }
   fclose(f);
   {
      zip_rom_entry z;
      const char *leaf, *zext;
      size_t j;
      memset(&z, 0, sizeof(z));
      if (sz_rom_peek(path, &z, af_sz_console) != 0)
         return -1;
      leaf = z.name;
      for (j = 0; z.name[j]; j++)
         if (z.name[j] == '/' || z.name[j] == '\\')
            leaf = z.name + j + 1;
      zext = strrchr(leaf, '.');
      if (!leaf[0] || !zext)
         return -1;
      stem_of(leaf, out->inner_stem, sizeof(out->inner_stem));
      snprintf(out->inner_ext, sizeof(out->inner_ext), "%s", zext);
      out->crc = z.crc;
      out->has_crc = (z.crc != 0);
      return 0;
   }
}

static void cand_add(af_candidates *a, const char *s)
{
   int i;
   if (!s || !s[0] || a->ncand >= 5 + AF_ALT_MAX)
      return;
   for (i = 0; i < a->ncand; i++)
      if (!strcmp(a->cand[i], s))
         return;
   snprintf(a->cand[a->ncand++], AF_NAME_CAP, "%s", s);
}

static int ci_contains(const char *hay, const char *needle)
{
   size_t n = strlen(needle), i;
   if (!n)
      return 0;
   for (; *hay; hay++)
   {
      for (i = 0; i < n; i++)
      {
         if (!hay[i] || tolower((unsigned char)hay[i]) != tolower((unsigned char)needle[i]))
            break;
      }
      if (i == n)
         return 1;
   }
   return 0;
}

static int af_is_hack_stem(const char *stem, const char *hdr_name)
{
   static const char *const MARKS[] = {
      "[h", "[t+", "[t-", "[b", "[f", "[p", "[o", "hack", "unbound", "kaizo",
      "redux", "randomiz", "remix", "deluxe", "gaiden", "radical red",
      "rocket edition", "ash gray", "crystal clear", "dark violet", "glazed",
      "flora sky", "light platinum", "blaze black", "volt white", "renegade",
      "inclement", "translation", "english", "patched", "improved",
      "rebalance", "restoration", "(eng", "(v1.", "(v2.", "(v3.", " v1.",
      " v2.", " v3.", NULL
   };
   int i;
   char w1[32], w2[32];
   size_t k;

   for (i = 0; MARKS[i]; i++)
      if (ci_contains(stem, MARKS[i]))
         return 1;

   /* Same franchise prefix (e.g. "Pokemon Unbound" vs "Pokemon - FireRed Version")
    * while gamedb_name_loose missed indicates a different title / hack sharing the base header. */
   if (hdr_name && hdr_name[0])
   {
      for (k = 0; k + 1 < sizeof(w1) && isalpha((unsigned char)stem[k]); k++)
         w1[k] = (char)tolower((unsigned char)stem[k]);
      w1[k] = '\0';
      for (k = 0; k + 1 < sizeof(w2) && isalpha((unsigned char)hdr_name[k]); k++)
         w2[k] = (char)tolower((unsigned char)hdr_name[k]);
      w2[k] = '\0';
      if (strlen(w1) >= 4 && !strcmp(w1, w2))
         return 1;
   }
   return 0;
}

/* Strict marker list for skipping slow ROM CRC32: only unambiguous hack or
 * fan-translation filenames, never retail tags like (V1.1), Gaiden, Deluxe,
 * Remix, Renegade, or bare "hack" (which matches retail words like "Shack"). */
static int af_is_explicit_hack_stem(const char *stem)
{
   static const char *const MARKS[] = {
      "[h", "[t+", "[t-", "kaizo ", "randomiz", "unbound", "radical red",
      "rocket edition", "ash gray", "crystal clear", "dark violet", "glazed",
      "flora sky", "light platinum", "blaze black", "volt white",
      "renegade platinum", "inclement", "translation", "patched", "rebalance",
      "restoration", "(eng", NULL
   };
   int i;
   for (i = 0; MARKS[i]; i++)
      if (ci_contains(stem, MARKS[i]))
         return 1;
   return 0;
}

/* Every name asked for has missed: the same game under the name of another
 * release of it (artalt.h).  Appended once; returns how many. */
static int add_alts(af_candidates *a)
{
   const char *have[5 + AF_ALT_MAX];
   int b, i, added = 0;
   if (a->alts_done)
      return 0;
   a->alts_done = 1;
   for (i = 0; i < a->ncand; i++)
      have[i] = a->cand[i];
   for (b = 0; b < a->ncand && added < AF_ALT_MAX; b++)
   {
      af_alts alts;
      alt_names(a->cand[b], have, a->ncand, &alts);
      for (i = 0; i < alts.n && added < AF_ALT_MAX && a->ncand < 5 + AF_ALT_MAX; i++)
      {
         memmove(a->cand[a->ncand], alts.name[i], strlen(alts.name[i]) + 1);
         have[a->ncand] = a->cand[a->ncand];
         a->ncand++;
         added++;
      }
   }
   if (added)
      { char n[16]; snprintf(n, sizeof(n), "%d", added); say("art_alt name=\"%s\" tries=%s", a->stem, n); }
   return added;
}

static void candidates_init(af_candidates *a, const char *path, const char *name)
{
   static const char TAG[13] = { 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H',
                                 'I', 'J', 'K', 'L', 'M' };
   const char *ext = strrchr(name, '.');
   char buf[AF_NAME_CAP];
   int k;

   stem_of(name, a->stem, sizeof(a->stem));
   if (ext && (!strcasecmp(ext, ".zip") || !strcasecmp(ext, ".7z")))
   {
      af_arc_info arc;
      a->is_archive = 1;
      a->crc_tried = 1;
      a->crc_allowed = 0;
      if (af_peek_archive(path, ext, &arc) != 0 ||
          !af_ext_sys(arc.inner_ext, a->sys, &a->nsys, &a->no_crc))
      {
         /* Never fall through to Game Boy or permanently blacklist a .zip/.7z
          * whose inner ROM could not be inspected right now. */
         a->nsys = 0;
         a->crc_unavailable = 1;
         a->ready = 1;
         return;
      }
      if (gamedb_known_name(a->stem))
         cand_add(a, a->stem);
      if (arc.inner_stem[0] && gamedb_known_name(arc.inner_stem))
         cand_add(a, arc.inner_stem);
      if (!a->no_crc && arc.has_crc &&
          gamedb_name_by_crc(arc.crc, buf, AF_NAME_CAP))
         cand_add(a, buf);
      if (a->ncand < 2 && arc.inner_stem[0])
      {
         for (k = 0; k < a->nsys && a->ncand < 3; k++)
            if (gamedb_name_loose(TAG[a->sys[k]], arc.inner_stem, buf, AF_NAME_CAP))
               cand_add(a, buf);
      }
      if (a->ncand < 2)
      {
         for (k = 0; k < a->nsys && a->ncand < 3; k++)
            if (gamedb_name_loose(TAG[a->sys[k]], a->stem, buf, AF_NAME_CAP))
               cand_add(a, buf);
      }
      cand_add(a, arc.inner_stem);
      cand_add(a, a->stem);
      a->ready = 1;
      return;
   }

   if (!af_ext_sys(ext, a->sys, &a->nsys, &a->no_crc))
   {
      a->sys[0] = 1;
      a->sys[1] = 2;
      a->nsys = 2;
   }
   if (gamedb_known_name(a->stem))
      cand_add(a, a->stem);
   /* Loose No-Intro match before the GBA header so differently named hacks or
    * shortened filenames resolve properly first. */
   if (a->ncand < 2)
   {
      for (k = 0; k < a->nsys && a->ncand < 3; k++)
         if (gamedb_name_loose(TAG[a->sys[k]], a->stem, buf, AF_NAME_CAP))
            cand_add(a, buf);
   }
   if (a->sys[0] == 0 && !a->ncand &&
       gamedb_name_by_header(path, buf, AF_NAME_CAP))
   {
      if (!af_is_hack_stem(a->stem, buf))
         cand_add(a, buf);
   }
   /* For small non-hack ROMs (<= 4 MiB, i.e. 8/16-bit consoles) whose filename
    * is not in gamedb, compute CRC32 up front so renamed carts ask for their
    * exact catalogue name first. Larger ROMs (8-32 MiB GBA) defer CRC32 until
    * after fast filename/header checks, and obvious hacks skip CRC32. */
   {
      int hack = af_is_explicit_hack_stem(a->stem);
      if (!a->ncand && !a->no_crc && !hack && !stop_now())
      {
         struct stat st;
         if (path && path[0] && stat(path, &st) == 0 &&
             st.st_size > 0 && st.st_size <= (off_t)(4 * 1024 * 1024))
         {
            uint32_t crc;
            if (cart_crc(a, path, &crc) == 0)
            {
               a->crc_tried = 1;
               if (gamedb_name_by_crc(crc, buf, AF_NAME_CAP))
                  cand_add(a, buf);
            }
         }
      }
      a->crc_allowed = !gamedb_known_name(a->stem) && !a->no_crc && !hack;
   }
   cand_add(a, a->stem);
   a->ready = 1;
}

/* Single overlapped HTTPS prefetch slot: while artfetch writes the current PNG
 * and decodes/shrinks its .t2.565 thumbnail, np_https is already fetching the
 * next library entry on the kept-alive TLS socket. */
typedef struct
{
   uintptr_t h;
   char url[AF_NAME_CAP * 3 + 192];
} af_prefetch;
static af_prefetch g_pf;

static void prefetch_cancel(void)
{
   if (g_pf.h && g_https_ok)
   {
      g_https.cancel(g_pf.h);
      g_https.release(g_pf.h);
   }
   g_pf.h = 0;
   g_pf.url[0] = '\0';
}

static void mirror_url(int sys, int kind, const char *cand, char *out, size_t cap)
{
   char enc[AF_NAME_CAP * 3 + 8];
   url_name(cand, enc, sizeof(enc));
   snprintf(out, cap,
            "https://raw.githubusercontent.com/libretro-thumbnails/%s/master/%s/%s.png",
            SYS_GH[sys], KIND_URL[kind], enc);
}

static int https_wait_handle(uintptr_t h, size_t *len)
{
   int status = 0;
   const unsigned char *body = NULL;
   size_t blen = 0;
   int rc = GET_FAILED;
   if (!h || !g_https_ok)
      return GET_FAILED;
   for (;;)
   {
      int st = g_https.poll(h, &status, &body, &blen);
      if (st < 0)
         break;
      if (st == 1)
      {
         if (status == 200 && body && blen > 0 && blen <= AF_MAX_BYTES)
         {
            memcpy(g_body, body, blen);
            g_body[blen] = '\0';
            *len = blen;
            rc = GET_OK;
         }
         else if (status == 404 || status == 410)
            rc = GET_MISSING;
         else
            rc = GET_FAILED;
         break;
      }
      if (stop_hard() || !online_check(0))
      {
         g_https.cancel(h);
         break;
      }
      plat_thread_sleep_us(2000);
   }
   g_https.release(h);
   return rc;
}

static int fetch_mirror_get(int sys, int kind, const char *cand, size_t *len)
{
   char murl[AF_NAME_CAP * 3 + 192];
   uintptr_t h;
   if (!g_https_ok || sys < 0 || sys >= 13)
      return GET_UNAVAIL;
   mirror_url(sys, kind, cand, murl, sizeof(murl));
   *len = 0;
   if (g_pf.h)
   {
      if (!strcmp(g_pf.url, murl))
      {
         h = g_pf.h;
         g_pf.h = 0;
         g_pf.url[0] = '\0';
         return https_wait_handle(h, len);
      }
      /* Cancel mismatched background prefetch so focused covers never sit behind
       * an unwanted request on np_https's serial worker or hold 2x RAM. */
      prefetch_cancel();
   }
   h = g_https.start(murl, AF_MAX_BYTES);
   if (!h)
      return GET_UNAVAIL;
   return https_wait_handle(h, len);
}

static int fetch_art_get(int sys, int kind, const char *cand, size_t *len)
{
   char url[AF_NAME_CAP * 3 + 128], enc[AF_NAME_CAP * 3 + 8];
   int r;
   if (g_https_ok && g_prefer_mirror)
   {
      int mr = fetch_mirror_get(sys, kind, cand, len);
      if (mr == GET_OK)
      {
         g_mirror_fails = 0;
         if (*len >= 8 && memcmp(g_body, "\x89PNG\r\n\x1a\n", 8) == 0)
            return GET_OK;
         return GET_FAILED;
      }
      if (mr == GET_MISSING)
      {
         g_mirror_fails = 0;
         return GET_MISSING;
      }
      if (mr == GET_UNAVAIL)
      {
         g_prefer_mirror = 0;
         g_mirror_fails = 0;
      }
      else
      {
         if (++g_mirror_fails >= 3)
         {
            g_prefer_mirror = 0;
            g_mirror_fails = 0;
         }
         return GET_FAILED;
      }
   }
   url_name(cand, enc, sizeof(enc));
   snprintf(url, sizeof(url), "/%s/%s/%s.png", SYS[sys], KIND_URL[kind], enc);
   r = http_get(url, len);
   if (r == GET_OK)
   {
      g_primary_fails = 0;
      if (*len >= 8 && memcmp(g_body, "\x89PNG\r\n\x1a\n", 8) == 0)
         return GET_OK;
      r = GET_FAILED;
   }
   else if (r == GET_MISSING)
      g_primary_fails = 0;
   else
      g_primary_fails++;
   if (!g_https_ok || stop_hard() || !online_check(0))
      return r;
   /* Primary failed or returned 404 (e.g. thumbnails.libretro.com outage):
    * check the GitHub raw HTTPS mirror before accepting a miss or failure. */
   {
      int mr = fetch_mirror_get(sys, kind, cand, len);
      if (mr == GET_OK)
      {
         g_mirror_fails = 0;
         if (*len >= 8 && memcmp(g_body, "\x89PNG\r\n\x1a\n", 8) == 0)
         {
            if (!g_prefer_mirror)
            {
               g_prefer_mirror = 1;
               http_close();
               say("art_fetch mirror active host=%s%s", "raw.githubusercontent.com", "");
            }
            return GET_OK;
         }
         return GET_FAILED;
      }
      if (mr == GET_MISSING)
      {
         g_mirror_fails = 0;
         if (r == GET_FAILED && g_primary_fails >= 2 && !g_prefer_mirror)
         {
            g_prefer_mirror = 1;
            http_close();
            say("art_fetch mirror active host=%s%s", "raw.githubusercontent.com", "");
         }
         return r == GET_MISSING ? GET_MISSING : GET_FAILED;
      }
      if (mr == GET_UNAVAIL)
         return r;   /* local TLS/clock unavailable: respect live primary verdict */
      /* Mirror itself hit a network/TLS error: never turn a primary 404 into a
       * permanent miss in .missing.txt. */
      return GET_FAILED;
   }
}

typedef struct af_prefetch_ctx
{
   const af_list *list;
   void *items_v;
   const char *cur_stem;
   int next_idx;
   int kind;
} af_prefetch_ctx;

static void prefetch_arm_ctx(const af_prefetch_ctx *pf);

/* Candidates and the lazy CRC are shared by box art and title screen.
 * 2 defers an unavailable local identity without a permanent miss or backoff. */
static int fetch_one(int kind, const char *path, const char *name,
                     af_candidates *a, const af_prefetch_ctx *pf)
{
   int c, k, failed = 0;
   int skip_c = -1, skip_k = -1;
   if (!a->ready)
      candidates_init(a, path, name);
   /* Reuse the winning (candidate, system) from box art first for title screens. */
   if (a->hit_cand_1 > 0 && a->hit_sys_1 > 0 &&
       a->hit_cand_1 - 1 < a->ncand && a->hit_sys_1 - 1 < a->nsys)
   {
      int hc = a->hit_cand_1 - 1, hk = a->hit_sys_1 - 1;
      size_t len = 0;
      int r;
      if (stop_now() || !online_check(0))
         return -1;
      r = fetch_art_get(a->sys[hk], kind, a->cand[hc], &len);
      if (r == GET_OK)
      {
         if (len >= 8 && memcmp(g_body, "\x89PNG\r\n\x1a\n", 8) == 0)
         {
            if (pf) prefetch_arm_ctx(pf);
            if (save(kind, a->stem, len) != 0)
               return -1;
            say(kind == AK_BOX ? "art_fetch ok name=\"%s\" as=\"%s\""
                               : "art_fetch title name=\"%s\" as=\"%s\"",
                a->stem, a->cand[hc]);
            return 1;
         }
         failed = 1;
      }
      else if (r == GET_FAILED)
         failed = 1;
      else
      {
         skip_c = hc;
         skip_k = hk;
      }
   }
   for (c = 0; ; c++)
   {
      if (c >= a->ncand)
      {
         uint32_t crc;
         if (failed)
            break;
         if (a->crc_unavailable)
            return 2;
         if (!a->crc_allowed || a->crc_tried)
         {
            if (add_alts(a))
            {
               c--;
               continue;
            }
            break;
         }
         a->crc_tried = 1;
         if (stop_now())
         {
            a->crc_tried = 0;          /* not tried: asked to stop first */
            return -1;
         }
         if (cart_crc(a, path, &crc) != 0)
         {
            if (stop_now())
            {
               a->crc_tried = 0;       /* cut short: it is tried again */
               return -1;
            }
            a->crc_unavailable = 1;
            return 2;
         }
         if (stop_now())
         {
            a->crc_tried = 0;
            return -1;
         }
         if (!gamedb_name_by_crc(crc, a->cand[a->ncand], AF_NAME_CAP) ||
             !strcmp(a->cand[a->ncand], a->stem))
         {
            if (add_alts(a))
            {
               c--;
               continue;
            }
            break;
         }
         a->ncand++;
      }
      if (c && !strcmp(a->cand[c], a->cand[c - 1]))
         continue;
      for (k = 0; k < a->nsys; k++)
      {
         size_t len = 0;
         int r;
         if (c == skip_c && k == skip_k)
            continue;
         if (stop_now() || !online_check(0))
            return -1;
         r = fetch_art_get(a->sys[k], kind, a->cand[c], &len);
         if (r == GET_OK)
         {
            if (len < 8 || memcmp(g_body, "\x89PNG\r\n\x1a\n", 8) != 0)
            {
               failed = 1;            /* a portal's page, not a picture */
               continue;
            }
            if (kind == AK_BOX)
            {
               a->hit_cand_1 = c + 1;
               a->hit_sys_1 = k + 1;
            }
            if (pf) prefetch_arm_ctx(pf);
            if (save(kind, a->stem, len) != 0)
               return -1;
            say(kind == AK_BOX ? "art_fetch ok name=\"%s\" as=\"%s\""
                               : "art_fetch title name=\"%s\" as=\"%s\"",
                a->stem, a->cand[c]);
            return 1;
         }
         if (r == GET_FAILED)
            failed = 1;
      }
   }
   if (failed)
   {
      say("art_fetch failed name=\"%s\"%s", a->stem, kind ? " kind=title" : "");
      return -1;
   }
   say("art_fetch none name=\"%s\"%s", a->stem, kind ? " kind=title" : "");
   return 0;
}

typedef struct {
   af_candidates *candidates;
   unsigned checked;
   unsigned stem_hash;
   int next_same;
} af_item;
enum { AF_HAVE_BOX = 1u << AK_KINDS };
typedef struct { char *path, *name; uint64_t at; } af_failure;
#define AF_FAILURES 128
#define AF_COOLDOWN 30000000u
static af_failure g_failures[AF_FAILURES];

static void failures_clear(void)
{
   unsigned k;
   for (k = 0; k < AF_FAILURES; k++)
   {
      free(g_failures[k].path); free(g_failures[k].name);
      memset(&g_failures[k], 0, sizeof(g_failures[k]));
   }
}

static uint64_t failure_time(const char *path, const char *name)
{
   unsigned i;
   for (i = 0; i < AF_FAILURES; i++)
      if (g_failures[i].path && !strcmp(g_failures[i].path, path) &&
          !strcmp(g_failures[i].name, name))
         return g_failures[i].at;
   return 0;
}

static void failure_record(const char *path, const char *name)
{
   unsigned i, victim = 0;
   char *p, *n;
   for (i = 0; i < AF_FAILURES; i++)
   {
      if (g_failures[i].path && !strcmp(g_failures[i].path, path) &&
          !strcmp(g_failures[i].name, name))
      {
         g_failures[i].at = worker_now();
         return;
      }
      if (!g_failures[i].path) { victim = i; break; }
      if (g_failures[i].at < g_failures[victim].at) victim = i;
   }
   p = strdup(path); n = strdup(name);
   if (!p || !n) { free(p); free(n); return; }
   free(g_failures[victim].path); free(g_failures[victim].name);
   g_failures[victim].path = p; g_failures[victim].name = n;
   g_failures[victim].at = worker_now();
}

static void items_free(af_item *items, int n)
{
   int i;
   for (i = 0; items && i < n; i++) free(items[i].candidates);
   free(items);
}

static void progress_publish(int box_done, int title_done)
{
   int seen = A_LOAD(g_titles) ? (box_done + title_done) / 2 : box_done;
   A_STORE(g_prog_seen, seen);
}

static void item_mark(af_item *item, unsigned bits, int *box_done, int *title_done)
{
   unsigned added = bits & ~item->checked;
   item->checked |= bits;
   if (added & (1u << AK_BOX)) (*box_done)++;
   if (added & (1u << AK_TITLE)) (*title_done)++;
   progress_publish(*box_done, *title_done);
}

static int *stem_index_build(const af_list *list, af_item *items, unsigned *mask_out)
{
   unsigned cap = 16;
   int *buckets, j;
   *mask_out = 0;
   if (!list || !list->n)
      return NULL;
   while (cap < (unsigned)list->n * 2u && cap < (1u << 20))
      cap <<= 1;
   buckets = (int *)malloc((size_t)cap * sizeof(*buckets));
   if (!buckets)
      return NULL;
   for (j = 0; j < (int)cap; j++)
      buckets[j] = -1;
   *mask_out = cap - 1u;
   for (j = 0; j < list->n; j++)
   {
      char stem[AF_NAME_CAP];
      unsigned h, slot;
      stem_of(list->name[j], stem, sizeof(stem));
      h = stem_hash(stem);
      slot = h & (cap - 1u);
      items[j].stem_hash = h;
      items[j].next_same = buckets[slot];
      buckets[slot] = j;
   }
   return buckets;
}

static void box_available(const af_list *list, af_item *items,
                          const int *buckets, unsigned bucket_mask,
                          int idx, const char *stem)
{
   int j, have = A_LOAD(g_prog_have);
   unsigned h;
   if (items[idx].checked & AF_HAVE_BOX)
      return;
   /* Shared, case-folded stems gain one cover for every matching cartridge. */
   if (buckets)
   {
      h = items[idx].stem_hash;
      for (j = buckets[h & bucket_mask]; j >= 0; j = items[j].next_same)
         if (!(items[j].checked & AF_HAVE_BOX) && items[j].stem_hash == h)
         {
            char other[AF_NAME_CAP];
            stem_of(list->name[j], other, sizeof(other));
            if (!strcasecmp(other, stem))
            {
               items[j].checked |= AF_HAVE_BOX;
               have++;
            }
         }
   }
   else
   {
      for (j = 0; j < list->n; j++)
         if (!(items[j].checked & AF_HAVE_BOX))
         {
            char other[AF_NAME_CAP];
            stem_of(list->name[j], other, sizeof(other));
            if (!strcasecmp(other, stem))
            {
               items[j].checked |= AF_HAVE_BOX;
               have++;
            }
         }
   }
   A_STORE(g_prog_have, have);
}

static void prefetch_arm_ctx(const af_prefetch_ctx *pf)
{
   static const char TAG[13] = { 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H',
                                 'I', 'J', 'K', 'L', 'M' };
   af_item *items;
   const af_list *l;
   int j, kind;
   unsigned bit;
   if (!pf || !pf->list || !pf->items_v || !g_https_ok || !g_prefer_mirror ||
       g_pf.h || stop_now() || !online_check(0))
      return;
   l = pf->list;
   items = (af_item *)pf->items_v;
   kind = pf->kind;
   bit = 1u << kind;
   for (j = pf->next_idx; j < l->n; j++)
   {
      char stem[AF_NAME_CAP], murl[AF_NAME_CAP * 3 + 192];
      uint64_t failed;
      af_candidates *a;
      int hc, hk;
      if (items[j].checked & bit)
         continue;
      stem_of(l->name[j], stem, sizeof(stem));
      if (pf->cur_stem && !strcasecmp(stem, pf->cur_stem))
         continue;
      if (have_art(kind, stem) || missing_has(kind, stem))
         continue;
      failed = failure_time(l->path[j], l->name[j]);
      if (failed && worker_now() < failed + AF_COOLDOWN)
         continue;
      /* Never stall save() with archive peeks or CRC32 reads: only prefetch items
       * already resolved or resolvable purely in RAM via gamedb. */
      if (!items[j].candidates || !items[j].candidates->ready)
      {
         const char *ext = strrchr(l->name[j], '.');
         int sys[4], nsys = 0, no_crc = 0, k, known = 0;
         char tmp[AF_NAME_CAP];
         if (ext && (!strcasecmp(ext, ".zip") || !strcasecmp(ext, ".7z")))
            continue;
         if (!af_ext_sys(ext, sys, &nsys, &no_crc))
            { sys[0] = 1; sys[1] = 2; nsys = 2; }
         if (gamedb_known_name(stem))
            known = 1;
         else
         {
            for (k = 0; k < nsys; k++)
               if (gamedb_name_loose(TAG[sys[k]], stem, tmp, sizeof(tmp)))
                  { known = 1; break; }
         }
         if (!known)
            continue;
      }
      if (!items[j].candidates)
         items[j].candidates = calloc(1, sizeof(*items[j].candidates));
      a = items[j].candidates;
      if (!a)
         return;
      if (!a->ready)
         candidates_init(a, l->path[j], l->name[j]);
      if (a->ncand <= 0 || a->nsys <= 0)
         continue;
      hc = (kind == AK_TITLE && a->hit_cand_1 > 0 && a->hit_cand_1 - 1 < a->ncand)
               ? a->hit_cand_1 - 1 : 0;
      hk = (kind == AK_TITLE && a->hit_sys_1 > 0 && a->hit_sys_1 - 1 < a->nsys)
               ? a->hit_sys_1 - 1 : 0;
      if (a->sys[hk] < 0 || a->sys[hk] >= 13)
         continue;
      if (stop_now() || !online_check(0))
         return;
      mirror_url(a->sys[hk], kind, a->cand[hc], murl, sizeof(murl));
      g_pf.h = g_https.start(murl, AF_MAX_BYTES);
      if (g_pf.h)
         snprintf(g_pf.url, sizeof(g_pf.url), "%s", murl);
      return;
   }
}

/* -2 is interrupted/allocation failure: never a permanent decision. */
static int item_kind(const af_list *list, af_item *items,
                     const int *buckets, unsigned bucket_mask,
                     int idx, int kind, int foreground,
                     int *box_done, int *title_done)
{
   af_item *item = &items[idx];
   char stem[AF_NAME_CAP];
   unsigned bit = 1u << kind;
   int r;
   if ((item->checked & bit) && !foreground)
      return 0;
   if (stop_now())
      return -2;
   stem_of(list->name[idx], stem, sizeof(stem));
   if (have_art(kind, stem))
   {
      if (kind == AK_BOX)
      {
         box_available(list, items, buckets, bucket_mask, idx, stem);
         if (warm_one(list->name[idx])) done(list->name[idx]);
      }
      if (stop_now()) return -2;
      r = 0;
   }
   else if (missing_has(kind, stem))
      r = 0;
   else
   {
      uint64_t failed = failure_time(list->path[idx], list->name[idx]);
      if (failed && worker_now() < failed + AF_COOLDOWN)
      {
         item_mark(item, bit, box_done, title_done);
         return 2;
      }
      if (!item->candidates)
         item->candidates = calloc(1, sizeof(*item->candidates));
      if (!item->candidates)
         return -2;
      {
         af_prefetch_ctx pf;
         pf.list = list;
         pf.items_v = items;
         pf.cur_stem = stem;
         pf.next_idx = idx + 1;
         pf.kind = kind;
         r = fetch_one(kind, list->path[idx], list->name[idx], item->candidates,
                       foreground ? NULL : &pf);
      }
      if (r != 1 && stop_now())
      {
         /* The names worked out for it so far (the header's, the file's)
          * are kept for when it comes round again; only an interrupted
          * checksum is redone (fetch_one leaves it untried). */
         return -2;
      }
      if (r == 0)
         missing_add(kind, stem, 1);
      if (r == 1)
      {
         done(list->name[idx]);
         if (kind == AK_BOX)
         {
            box_available(list, items, buckets, bucket_mask, idx, stem);
            /* A promoted PNG can outlive an interrupted thumbnail write. */
            if (stop_now()) return -2;
         }
      }
   }
   item_mark(item, bit, box_done, title_done);
   return r;
}

static void thread_main(void *arg)
{
   af_list *l = NULL;
   af_list *waiting = NULL;
   af_item *items = NULL;
   int *buckets = NULL;
   unsigned bucket_mask = 0;
   af_focus *focus = NULL;
   int box_i = 0, title_i = 0, fails = 0, k, warm_i = 0;
   int box_done = 0, title_done = 0;
   int focus_served = 0, focus_warmed = 0, inventory_dirty = 0;
   int titles_seen = -1;
   uint64_t backoff_until = 0;
   (void)arg;
   for (k = 0; k < AK_KINDS; k++)
      missing_load(k);
   while (!A_LOAD(g_quit))
   {
      int warmed = 0, focus_idx = -1, online_now, titles_now, work_left;
      af_list *nl = __atomic_exchange_n(&g_new, NULL, __ATOMIC_SEQ_CST);
      af_focus *nf;
      if (nl) { list_free(waiting); waiting = nl; }
      nl = waiting;
      if (nl)
      {
         af_item *next = nl->n ? calloc((size_t)nl->n, sizeof(*next)) : NULL;
         if (nl->n && !next)
         {
            plat_thread_sleep_us(250000);
            continue;
         }
         missing_flush_all();
         prefetch_cancel();
         waiting = NULL;
         free(buckets); buckets = NULL; bucket_mask = 0;
         items_free(items, l ? l->n : 0);
         list_free(l);
         l = nl; items = next;
         buckets = stem_index_build(l, items, &bucket_mask);
         box_i = title_i = warm_i = box_done = title_done = fails = 0;
         titles_seen = -1;
         inventory_dirty = 1;
         backoff_until = 0;
         A_STORE(g_prog_total, l->n);
         A_STORE(g_prog_seen, 0);
         A_STORE(g_prog_have, 0);
         if (focus && focus->generation < l->generation)
         { free(focus); focus = NULL; }
         focus_served = 0;
         focus_warmed = 0;
      }
      if (__atomic_exchange_n(&g_retry, 0, __ATOMIC_SEQ_CST))
      {
         prefetch_cancel();
         g_prefer_mirror = 0;
         g_primary_fails = 0;
         g_mirror_fails = 0;
         g_online_at = 0;
         failures_clear();
         set_free(&g_thumb_failed);
         for (k = 0; k < AK_KINDS; k++) missing_clear(k, 1);
         for (k = 0; l && k < l->n; k++)
         {
            free(items[k].candidates);
            items[k].candidates = NULL;
            items[k].checked = 0;
         }
         box_i = title_i = warm_i = box_done = title_done = fails = focus_served = 0;
         focus_warmed = 0;
         backoff_until = 0;
         inventory_dirty = 1;
         A_STORE(g_prog_seen, 0);
         say("art_fetch retry%s%s", "", "");
      }
      if (__atomic_exchange_n(&g_rescan, 0, __ATOMIC_SEQ_CST) && l)
      {
         /* A picture was put in or taken out of boxart/ by hand or by
          * cover_custom.c: look at the folders again, forget no misses. */
         prefetch_cancel();
         missing_flush_all();
         for (k = 0; k < l->n; k++)
            items[k].checked &= ~(1u << AK_BOX);
         box_i = warm_i = box_done = 0;
         progress_publish(box_done, title_done);
         inventory_dirty = 1;
         backoff_until = 0;
         say("art_fetch rescan%s%s", "", "");
      }
      nf = __atomic_exchange_n(&g_focus, NULL, __ATOMIC_SEQ_CST);
      if (nf)
      {
         if (focus && focus->generation == nf->generation &&
             !strcmp(focus->path, nf->path) && !strcmp(focus->name, nf->name))
            free(nf);
         else
         {
            free(focus); focus = nf; focus_served = focus_warmed = 0;
         }
      }
      titles_now = A_LOAD(g_titles);
      if (l && titles_now != titles_seen)
      {
         titles_seen = titles_now;
         if (titles_now) title_i = 0;
         progress_publish(box_done, title_done);
      }
      if (!l || A_LOAD(g_paused))
      {
         prefetch_cancel();
         http_close();
         missing_flush_all();
         A_STORE(g_prog_busy, 0);
         g_online_at = 0;
         nap_us(250000);
         continue;
      }
      if (inventory_dirty)
      {
         int have = 0;
         inventory_load();
         if (A_LOAD(g_quit) || A_LOAD(g_new)) continue;
         for (k = 0; k < l->n; k++)
         {
            char stem[AF_NAME_CAP];
            stem_of(l->name[k], stem, sizeof(stem));
            items[k].checked &= ~AF_HAVE_BOX;
            if (have_art(AK_BOX, stem))
            {
               items[k].checked |= AF_HAVE_BOX;
               have++;
            }
         }
         A_STORE(g_prog_have, have);
         inventory_dirty = 0;
         if (stop_now()) continue;
      }
      if (focus && focus->generation == l->generation)
      {
         for (k = 0; k < l->n; k++)
            if (!strcmp(focus->path, l->path[k]) && !strcmp(focus->name, l->name[k]))
            { focus_idx = k; break; }
         if (focus_idx < 0) { free(focus); focus = NULL; }
      }
      else if (focus && focus->generation < l->generation)
      { free(focus); focus = NULL; }
      /* Fast-skip already-cached and already-known-missing entries in O(1) RAM
       * without calling g_online() or looping once per item on large libraries. */
      if (g_inventory_valid)
      {
         int skipped = 0;
         while (box_i < l->n)
         {
            char stem[AF_NAME_CAP];
            if (items[box_i].checked & (1u << AK_BOX)) { box_i++; continue; }
            stem_of(l->name[box_i], stem, sizeof(stem));
            if (missing_has(AK_BOX, stem) ||
                (set_has(&g_inventory[AK_BOX], stem) &&
                 (set_has(&g_inventory[INV_THUMB], stem) ||
                  (!set_has(&g_inventory[INV_BOX_PNG], stem) &&
                   !set_has(&g_inventory[INV_BOX_JPG], stem)) ||
                  set_has(&g_thumb_failed, stem))))
            {
               items[box_i].checked |= (1u << AK_BOX);
               box_done++;
               box_i++;
               if (++skipped >= 128 && stop_now()) break;
               continue;
            }
            break;
         }
         while (titles_now && title_i < l->n)
         {
            char stem[AF_NAME_CAP];
            if (items[title_i].checked & (1u << AK_TITLE)) { title_i++; continue; }
            stem_of(l->name[title_i], stem, sizeof(stem));
            if (set_has(&g_inventory[AK_TITLE], stem) || missing_has(AK_TITLE, stem))
            {
               items[title_i].checked |= (1u << AK_TITLE);
               title_done++;
               title_i++;
               if (++skipped >= 128 && stop_now()) break;
               continue;
            }
            break;
         }
         if (skipped)
            progress_publish(box_done, title_done);
         if (stop_now()) continue;
      }
      while (box_i < l->n && (items[box_i].checked & (1u << AK_BOX))) box_i++;
      while (titles_now && title_i < l->n &&
             (items[title_i].checked & (1u << AK_TITLE))) title_i++;
      online_now = online_check(0);
      work_left = (box_i < l->n) || (titles_now && title_i < l->n) ||
                  (warm_i < l->n) || (focus_idx >= 0 && !focus_served);
      A_STORE(g_prog_busy, online_now && work_left);
      if (focus_idx >= 0 && !focus_served)
      {
         uint64_t failed = failure_time(focus->path, focus->name);
         if (failed && worker_now() < failed + AF_COOLDOWN)
            focus_served = 1;
         else
         {
            if (!focus_warmed)
            {
               if (warm_one(l->name[focus_idx])) done(l->name[focus_idx]);
               if (stop_now()) continue;
               focus_warmed = 1;
            }
            if (online_now)
            {
               int r = item_kind(l, items, buckets, bucket_mask, focus_idx,
                                 AK_BOX, 1, &box_done, &title_done);
               if (r == -2)
               {
                  if (stop_hard()) { prefetch_cancel(); http_close(); }
                  if (!stop_now()) nap_us(250000);
                  continue;
               }
               if (r < 0) failure_record(focus->path, focus->name);
               focus_served = 1;
               continue;
            }
         }
      }
      if (warm_i < l->n)
      {
         if (g_inventory_valid)
         {
            int wskip = 0;
            while (warm_i < l->n)
            {
               char stem[AF_NAME_CAP];
               stem_of(l->name[warm_i], stem, sizeof(stem));
               if (set_has(&g_inventory[INV_THUMB], stem) ||
                   (!set_has(&g_inventory[INV_BOX_PNG], stem) &&
                    !set_has(&g_inventory[INV_BOX_JPG], stem)) ||
                   set_has(&g_thumb_failed, stem))
               {
                  warm_i++;
                  if (++wskip >= 128 && stop_now()) break;
                  continue;
               }
               break;
            }
            if (stop_now()) continue;
         }
         if (warm_i < l->n)
         {
            if (warm_one(l->name[warm_i])) done(l->name[warm_i]);
            if (stop_now()) continue;
            warm_i++;
            warmed = 1;
         }
      }
      /* Pass 1: all box art (AK_BOX) across the library before Pass 2
       * (AK_TITLE), so visible covers arrive twice as fast. */
      {
         int pass_kind = -1, idx = -1;
         if (box_i < l->n)
         {
            pass_kind = AK_BOX;
            idx = box_i;
         }
         else if (titles_now && title_i < l->n)
         {
            pass_kind = AK_TITLE;
            idx = title_i;
         }
         if (idx < 0 || !online_now)
         {
            if (!warmed)
            {
               prefetch_cancel();
               http_close();
               missing_flush_all();
               g_online_at = 0;
               nap_us(250000);
            }
            continue;
         }
         {
            uint64_t now = worker_now();
            int r;
            if (now < backoff_until)
            {
               prefetch_cancel();
               http_close();
               missing_flush_all();
               g_online_at = 0;
               nap_us((uint32_t)(backoff_until - now));
               continue;
            }
            r = item_kind(l, items, buckets, bucket_mask, idx, pass_kind,
                          0, &box_done, &title_done);
            if (r == -2)
            {
               if (stop_hard()) { prefetch_cancel(); http_close(); }
               if (!stop_now()) nap_us(250000);
               continue;
            }
            if (r < 0)
            {
               item_mark(&items[idx], 1u << pass_kind, &box_done, &title_done);
               if (++fails >= 3)
               {
                  backoff_until = worker_now() + AF_COOLDOWN;
                  fails = 0;
               }
            }
            else
               fails = 0;
            if (pass_kind == AK_BOX && (items[idx].checked & (1u << AK_BOX)))
               box_i++;
            else if (pass_kind == AK_TITLE && (items[idx].checked & (1u << AK_TITLE)))
               title_i++;
         }
      }
   }
   prefetch_cancel();
   http_close();
   missing_flush_all();
   free(buckets);
   items_free(items, l ? l->n : 0);
   list_free(l);
   list_free(waiting);
   free(focus);
   inventory_clear();
   set_free(&g_thumb_failed);
   failures_clear();
}

int artfetch_start(const char *root, const char *base_url, int (*online)(void))
{
   const char *u = base_url && base_url[0] ? base_url
                 : "http://thumbnails.libretro.com";
   const char *h;
   char *colon;
   if (A_LOAD(g_running))
      return 0;
   if (strncmp(u, "http://", 7) != 0)
      return -1;
   h = u + 7;
   snprintf(g_host, sizeof(g_host), "%s", h);
   g_host[strcspn(g_host, "/")] = '\0';
   colon = strchr(g_host, ':');
   g_port = 80;
   if (colon)
   {
      *colon = '\0';
      g_port = atoi(colon + 1);
   }
   snprintf(g_root, sizeof(g_root), "%s", root);
   g_online = online;
   g_http_sock = -1;
   g_http_host[0] = '\0';
   g_http_port = 0;
   g_prefer_mirror = 0;
   g_primary_fails = 0;
   g_mirror_fails = 0;
   g_online_at = 0;
   g_online_val = 1;
   g_pf.h = 0;
   g_pf.url[0] = '\0';
   g_dir_made[0] = g_dir_made[1] = 0;
   g_thumb_dir_made = 0;
   A_STORE(g_nav_busy, 0);
   g_body = (unsigned char *)malloc(AF_MAX_BYTES + 1);
   if (!g_body)
      return -1;
   A_STORE(g_quit, 0);
   if (plat_bg_start(thread_main, NULL) != 0)
   {
      free(g_body);
      g_body = NULL;
      return -1;
   }
   A_STORE(g_running, 1);
   return 0;
}

void artfetch_stop(void)
{
   if (!A_LOAD(g_running))
      return;
   A_STORE(g_quit, 1);
   if (plat_bg_join(1000000))
   {
      /* gone: its memory can go too (else it is left to the process) */
      int k;
      free(g_body);
      g_body = NULL;
      for (k = 0; k < AK_KINDS; k++)
         missing_clear(k, 0);
   }
   list_free(__atomic_exchange_n(&g_new, NULL, __ATOMIC_SEQ_CST));
   free(__atomic_exchange_n(&g_focus, NULL, __ATOMIC_SEQ_CST));
   A_STORE(g_running, 0);
}
