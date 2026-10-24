/* artfetch.c -- see artfetch.h. */
#include "artfetch.h"

#include <stdint.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "gamedb.h"
#include "plat.h"
#include "stb_image.h"

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
static int g_titles = 1;              /* title screens too (artfetch_titles) */
static int g_retry;                   /* atomic: artfetch_retry asked        */
/* atomic, for Settings: cartridges looked at / in the list / with a box
 * art now; 1 while the thread is working through the list */
static int g_prog_seen, g_prog_total, g_prog_have, g_prog_busy;
static int (*g_online)(void);
static char g_root[AF_PATH_CAP];
static char g_host[128];
static int g_port = 80;

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

void artfetch_titles(int on)
{
   A_STORE(g_titles, on ? 1 : 0);
}

void artfetch_retry(void)
{
   A_STORE(g_retry, 1);
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

/* <kind dir>/.missing.txt, in memory */
static char **g_missing[AK_KINDS];
static int g_nmissing[AK_KINDS], g_capmissing[AK_KINDS];
static int g_missing_current[AK_KINDS];
#define MISSING_HEADER "# VitAdhoc artwork misses v2\n"
static unsigned char *g_body;

static int stop_now(void)
{
   return A_LOAD(g_quit) || A_LOAD(g_paused) || A_LOAD(g_new) ||
          A_LOAD(g_retry) || A_LOAD(g_focus);
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
      if (A_LOAD(g_new) || A_LOAD(g_retry) || A_LOAD(g_focus) ||
          A_LOAD(g_paused) != paused || A_LOAD(g_titles) != titles)
         break;
   }
}

static void missing_add(int k, const char *stem, int write)
{
   if (g_nmissing[k] == g_capmissing[k])
   {
      int cap = g_capmissing[k] ? g_capmissing[k] * 2 : 64;
      char **m = (char **)realloc(g_missing[k], (size_t)cap * sizeof(char *));
      if (!m)
         return;
      g_missing[k] = m;
      g_capmissing[k] = cap;
   }
   g_missing[k][g_nmissing[k]] = strdup(stem);
   if (!g_missing[k][g_nmissing[k]])
      return;
   g_nmissing[k]++;
   if (write)
   {
      char path[AF_PATH_CAP + 32];
      FILE *f;
      snprintf(path, sizeof(path), "%s/%s/.missing.txt", g_root,
               KIND_DIR[k]);
      f = fopen(path, g_missing_current[k] ? "a" : "w");
      if (f)
      {
         if (!g_missing_current[k])
            fputs(MISSING_HEADER, f);
         fprintf(f, "%s\n", stem);
         fclose(f);
         g_missing_current[k] = 1;
      }
   }
}

static int missing_has(int k, const char *stem)
{
   int i;
   for (i = 0; i < g_nmissing[k]; i++)
      if (!strcmp(g_missing[k][i], stem))
         return 1;
   return 0;
}

static void missing_clear(int k, int unlink_file)
{
   int i;
   for (i = 0; i < g_nmissing[k]; i++)
      free(g_missing[k][i]);
   free(g_missing[k]);
   g_missing[k] = NULL;
   g_nmissing[k] = g_capmissing[k] = 0;
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
enum { INV_THUMB = AK_KINDS, INV_COUNT };
static af_set g_inventory[INV_COUNT];
static int g_inventory_valid;

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

static void inventory_clear(void)
{
   unsigned k;
   for (k = 0; k < INV_COUNT; k++)
   {
      size_t i;
      for (i = 0; i < g_inventory[k].cap; i++)
         free(g_inventory[k].keys[i]);
      free(g_inventory[k].keys);
      memset(&g_inventory[k], 0, sizeof(g_inventory[k]));
   }
   g_inventory_valid = 0;
}

static void inventory_add(int kind, const char *stem)
{
   if (set_add(&g_inventory[kind], stem) != 0)
      g_inventory_valid = 0;
}

static void inventory_load(void)
{
   int kind, ok = 1;
   inventory_clear();
   for (kind = 0; kind < AK_KINDS; kind++)
   {
      char path[AF_PATH_CAP + 32];
      DIR *dir;
      struct dirent *entry;
      snprintf(path, sizeof(path), "%s/%s", g_root, KIND_DIR[kind]);
      dir = opendir(path);
      if (!dir)
      {
         if (errno != ENOENT) ok = 0;
         continue;
      }
      while (!A_LOAD(g_quit) && !A_LOAD(g_new))
      {
         const char *name, *ext;
         size_t len, stem_len;
         char stem[AF_NAME_CAP];
         int target = kind;
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
            target = INV_THUMB;
            stem_len = len - strlen(ART_THUMB_SUFFIX);
         }
         else
         {
            if (!ext || (strcasecmp(ext, ".png") && strcasecmp(ext, ".jpg") &&
                         strcasecmp(ext, ".jpeg") && strcasecmp(ext, ".bmp") &&
                         strcasecmp(ext, ".v565") && strcasecmp(ext, ".565")))
               continue;
            stem_len = (size_t)(ext - name);
         }
         if (!stem_len || stem_len >= sizeof(stem))
            continue;
         memcpy(stem, name, stem_len);
         stem[stem_len] = '\0';
         if (set_add(&g_inventory[target], stem) != 0) ok = 0;
      }
      if (A_LOAD(g_quit) || A_LOAD(g_new)) ok = 0;
      closedir(dir);
   }
   g_inventory_valid = ok;
}

/* Revalidate at write boundaries so user artwork arriving during HTTP wins. */
static int have_art_disk(int k, const char *stem)
{
   static const char *const ext[] = { "png", "jpg", "jpeg", "bmp", "v565",
                                      "565" };
   char path[AF_PATH_CAP + AF_NAME_CAP + 32];
   unsigned i;
   for (i = 0; i < sizeof(ext) / sizeof(ext[0]); i++)
   {
      FILE *f;
      snprintf(path, sizeof(path), "%s/%s/%s.%s", g_root, KIND_DIR[k], stem,
               ext[i]);
      f = fopen(path, "rb");
      if (f)
      {
         fclose(f);
         return 1;
      }
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

enum { GET_OK = 0, GET_MISSING = 1, GET_FAILED = -1 };

/* The server's address, looked up once: a pass over a library asks it for
 * thousands of pictures, and each lookup was a DNS query on the console.
 * Forgotten when a connection or a transfer fails (the next request looks
 * it up again) and when the server changes. */
static struct sockaddr_storage g_addr;
static socklen_t g_addr_len;
static char g_addr_key[300];

/* GET http://g_host:g_port/<path>.  GET_OK with the body in g_body
 * (*len bytes), GET_MISSING for 404/410, GET_FAILED otherwise (no network,
 * a timeout, anything else). */
static int http_get(const char *path, size_t *len)
{
   char req[1400], port[8], *hdr_end;
   struct addrinfo hints, *ai = NULL;
   struct timeval tv;
   size_t got = 0, body_at = 0, want = (size_t)-1;
   int s = -1, status = 0, chunked = 0, rc = GET_FAILED;

   char key[sizeof(g_addr_key)];
   snprintf(port, sizeof(port), "%d", g_port);
   snprintf(key, sizeof(key), "%s:%s", g_host, port);
   if (!g_addr_len || strcmp(key, g_addr_key))
   {
      memset(&hints, 0, sizeof(hints));
      hints.ai_family = AF_INET;
      hints.ai_socktype = SOCK_STREAM;
      g_addr_len = 0;
      if (getaddrinfo(g_host, port, &hints, &ai) != 0 || !ai)
      {
         if (ai)
            freeaddrinfo(ai);
         return GET_FAILED;
      }
      if (ai->ai_addrlen <= sizeof(g_addr))
      {
         memcpy(&g_addr, ai->ai_addr, ai->ai_addrlen);
         g_addr_len = (socklen_t)ai->ai_addrlen;
         snprintf(g_addr_key, sizeof(g_addr_key), "%s", key);
      }
      freeaddrinfo(ai);
      ai = NULL;
      if (!g_addr_len)
         return GET_FAILED;
   }
   s = socket(AF_INET, SOCK_STREAM, 0);
   if (s < 0)
      goto out;
   tv.tv_sec = 15;
   tv.tv_usec = 0;
   setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
   setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
   if (connect(s, (const struct sockaddr *)&g_addr, g_addr_len) != 0)
   {
      g_addr_len = 0;                  /* look it up again next time */
      goto out;
   }
   snprintf(req, sizeof(req),
            "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: VitAdhoc\r\n"
            "Accept: image/png\r\nConnection: close\r\n\r\n", path, g_host);
   if (send(s, req, strlen(req), 0) != (ssize_t)strlen(req))
      goto out;
   for (;;)
   {
      ssize_t r;
      if (stop_now())
         goto out;
      if (got >= AF_MAX_BYTES)
         goto out;                     /* not a box art picture */
      r = recv(s, g_body + got, AF_MAX_BYTES - got, 0);
      if (r < 0)
      {
         g_addr_len = 0;
         goto out;                     /* an error is not the end of it */
      }
      if (r == 0)
         break;
      got += (size_t)r;
      if (!body_at)
      {
         g_body[got < AF_MAX_BYTES ? got : AF_MAX_BYTES - 1] = '\0';
         hdr_end = strstr((char *)g_body, "\r\n\r\n");
         if (hdr_end)
         {
            char *p;
            *hdr_end = '\0';
            body_at = (size_t)(hdr_end + 4 - (char *)g_body);
            if (sscanf((char *)g_body, "HTTP/1.%*d %d", &status) != 1)
               goto out;
            for (p = (char *)g_body; *p; p++)
               if (*p >= 'A' && *p <= 'Z')
                  *p = (char)(*p - 'A' + 'a');
            p = strstr((char *)g_body, "\r\ncontent-length:");
            if (p)
               want = (size_t)strtoul(p + 17, NULL, 10);
            chunked = strstr((char *)g_body, "\r\ntransfer-encoding: chunked")
                      != NULL;
            if (status != 200)
               break;                  /* the body says nothing we need */
         }
      }
      if (body_at && want != (size_t)-1 && got - body_at >= want)
         break;
   }
   if (status == 404 || status == 410)
      rc = GET_MISSING;
   else if (status == 200 && body_at)
   {
      size_t n = got - body_at;
      memmove(g_body, g_body + body_at, n);
      g_body[n] = '\0';               /* (the chunk parser reads strings) */
      if (chunked)
      {
         /* <hex size>\r\n<data>\r\n ... 0\r\n\r\n, put together in place */
         size_t rd = 0, wr = 0;
         for (;;)
         {
            char *e;
            unsigned long c = strtoul((char *)g_body + rd, &e, 16);
            char *crlf = strstr((char *)g_body + rd, "\r\n");
            if (!crlf || e == (char *)g_body + rd)
               goto out;
            rd = (size_t)(crlf + 2 - (char *)g_body);
            if (!c)
               break;
            if (rd + c > n)
               goto out;
            memmove(g_body + wr, g_body + rd, c);
            wr += c;
            rd += c + 2;
         }
         n = wr;
      }
      else if (want != (size_t)-1 && n < want)
         goto out;                     /* cut short */
      *len = n;
      rc = GET_OK;
   }
out:
   if (s >= 0)
      close(s);
   if (ai)
      freeaddrinfo(ai);
   return rc;
}

/* The thumbnail file for a cover: boxart/<stem>.t2.565, the picture fitted into
 * ART_THUMB_W x ART_THUMB_H (never enlarged), box-averaged, as RGB565 behind
 * the same 16-byte header the app's .v565 files use, so the home screen reads
 * it with its ordinary loader.  Written whole or not at all.  0 = written,
 * -2 = not a picture stb can read, -3 = a picture too big to shrink here,
 * -1 = anything that may pass (stopping, memory, the card). */
#define THUMB_MAGIC 0x52353635u     /* '565R' little-endian, as ui_psp.c */
static int thumb_write(const unsigned char *png, size_t len, const char *dst)
{
   char tmp[AF_PATH_CAP + AF_NAME_CAP + 40];
   unsigned char hdr[16], *pix, *out;
   int w = 0, h = 0, nc = 0, tw, th, x, y, rc = -1;
   double s;
   FILE *f;

   if (!png || len == 0 || stop_now())
      return -1;
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
   s = (double)ART_THUMB_W / w;
   if ((double)ART_THUMB_H / h < s)
      s = (double)ART_THUMB_H / h;
   if (s > 1.0)
      s = 1.0;
   tw = (int)(w * s + 0.5);
   th = (int)(h * s + 0.5);
   if (tw < 1) tw = 1;
   if (th < 1) th = 1;
   if (tw > ART_THUMB_W) tw = ART_THUMB_W;
   if (th > ART_THUMB_H) th = ART_THUMB_H;
   out = malloc((size_t)tw * th * 2);
   if (!out)
   {
      stbi_image_free(pix);
      return -1;
   }
   for (y = 0; y < th; y++)
   {
      int y0 = y * h / th, y1 = (y + 1) * h / th, yy, xx;
      if (y1 <= y0) y1 = y0 + 1;
      if (y1 > h) y1 = h;
      for (x = 0; x < tw; x++)
      {
         int x0 = x * w / tw, x1 = (x + 1) * w / tw, n = 0;
         unsigned r = 0, g = 0, b = 0, v;
         if (x1 <= x0) x1 = x0 + 1;
         if (x1 > w) x1 = w;
         for (yy = y0; yy < y1; yy++)
            for (xx = x0; xx < x1; xx++)
            {
               const unsigned char *p = pix + ((size_t)yy * w + xx) * 3;
               r += p[0]; g += p[1]; b += p[2];
               n++;
            }
         r /= (unsigned)n; g /= (unsigned)n; b /= (unsigned)n;
         v = (r >> 3) | ((g >> 2) << 5) | ((b >> 3) << 11);
         out[2 * ((size_t)y * tw + x)]     = (unsigned char)(v & 0xFF);
         out[2 * ((size_t)y * tw + x) + 1] = (unsigned char)(v >> 8);
      }
   }
   stbi_image_free(pix);

   memset(hdr, 0, sizeof(hdr));
   hdr[0] = THUMB_MAGIC & 0xFF;
   hdr[1] = (THUMB_MAGIC >> 8) & 0xFF;
   hdr[2] = (THUMB_MAGIC >> 16) & 0xFF;
   hdr[3] = (THUMB_MAGIC >> 24) & 0xFF;
   hdr[4] = (unsigned char)(tw & 0xFF);
   hdr[5] = (unsigned char)(tw >> 8);
   hdr[6] = (unsigned char)(th & 0xFF);
   hdr[7] = (unsigned char)(th >> 8);
   hdr[8] = (unsigned char)(tw & 0xFF);       /* fstride = tw */
   hdr[9] = (unsigned char)(tw >> 8);

   snprintf(tmp, sizeof(tmp), "%s.tmp", dst);
   f = fopen(tmp, "wb");
   if (f)
   {
      int ok = fwrite(hdr, 1, sizeof(hdr), f) == sizeof(hdr) &&
               fwrite(out, 1, (size_t)tw * th * 2, f) == (size_t)tw * th * 2;
      ok = fclose(f) == 0 && ok;
      if (ok && rename(tmp, dst) == 0)
         rc = 0;
      else
         remove(tmp);
   }
   free(out);
   return rc;
}

/* The picture to <kind dir>/<stem>.png, whole or not at all. */
static int save(int k, const char *stem, size_t len)
{
   char tmp[AF_PATH_CAP + AF_NAME_CAP + 32], dst[sizeof(tmp)];
   FILE *f;
   int ok;
   if (have_art_disk(k, stem))
   {
      inventory_add(k, stem);
      return 0;
   }
   snprintf(dst, sizeof(dst), "%s/%s", g_root, KIND_DIR[k]);
   mkdir(dst, 0777);                   /* titles/ is made on the first one */
   snprintf(dst, sizeof(dst), "%s/%s/%s.png", g_root, KIND_DIR[k], stem);
   snprintf(tmp, sizeof(tmp), "%s/%s/%s.png.tmp", g_root, KIND_DIR[k], stem);
   f = fopen(tmp, "wb");
   if (!f)
      return -1;
   ok = fwrite(g_body, 1, len, f) == len;
   ok = fclose(f) == 0 && ok;
   if (ok && have_art_disk(k, stem))
   {
      remove(tmp);
      inventory_add(k, stem);
      return 0;
   }
   if (!ok || rename(tmp, dst) != 0)
   {
      remove(tmp);
      return -1;
   }
   if (k == AK_BOX)
   {
      /* the home screen's tile picture, made now while the bytes are here */
      char th[AF_PATH_CAP + AF_NAME_CAP + 40];
      int rc;
      snprintf(th, sizeof(th), "%s/boxart/%s%s", g_root, stem, ART_THUMB_SUFFIX);
      rc = thumb_write((const unsigned char *)g_body, len, th);
      if (rc == 0)
         inventory_add(INV_THUMB, stem);
      else if (rc == -2)
      {
         /* It had a PNG's first bytes and is not a picture: a transfer cut
          * short, or a broken file.  Not kept (art on the card is never
          * fetched again): a failure, tried again after the cool-down. */
         remove(dst);
         return -1;
      }
   }
   inventory_add(k, stem);
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
   unsigned e;

   stem_of(name, stem, sizeof(stem));
   if (g_inventory_valid &&
       (set_has(&g_inventory[INV_THUMB], stem) ||
        !set_has(&g_inventory[AK_BOX], stem)))
      return 0;
   if (set_has(&g_thumb_failed, stem))
      return 0;
   snprintf(th, sizeof(th), "%s/boxart/%s%s", g_root, stem, ART_THUMB_SUFFIX);
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
      int rc = thumb_write(buf, n, th);
      written = rc == 0;
      if (rc == -2 || rc == -3)
         set_add(&g_thumb_failed, stem);   /* its content, not the moment */
   }
   free(buf);
   if (written)
      inventory_add(INV_THUMB, stem);
   return written;
}

static const char *const SYS[3] = {
   "Nintendo%20-%20Game%20Boy%20Advance", "Nintendo%20-%20Game%20Boy",
   "Nintendo%20-%20Game%20Boy%20Color" };

typedef struct
{
   char stem[AF_NAME_CAP], cand[3][AF_NAME_CAP];
   int ncand, sys[2], nsys, ready, crc_allowed, crc_tried, crc_unavailable;
} af_candidates;

static void candidates_init(af_candidates *a, const char *path, const char *name)
{
   const char *ext = strrchr(name, '.');
   stem_of(name, a->stem, sizeof(a->stem));
   if (ext && !strcasecmp(ext, ".gba"))
   {
      a->sys[0] = 0;
      a->nsys = 1;
   }
   else if (ext && !strcasecmp(ext, ".gbc"))
   {
      a->sys[0] = 2; a->sys[1] = 1;
      a->nsys = 2;
   }
   else
   {
      a->sys[0] = 1; a->sys[1] = 2;
      a->nsys = 2;
   }
   if (gamedb_known_name(a->stem))
      memmove(a->cand[a->ncand++], a->stem, strlen(a->stem) + 1);
   if (a->sys[0] == 0 &&
       gamedb_name_by_header(path, a->cand[a->ncand], AF_NAME_CAP))
      a->ncand++;
   a->crc_allowed = a->ncand == 0;
   if (!a->ncand || strcmp(a->cand[0], a->stem))
      memmove(a->cand[a->ncand++], a->stem, strlen(a->stem) + 1);
   a->ready = 1;
}

/* Candidates and the lazy CRC are shared by box art and title screen.
 * 2 defers an unavailable local identity without a permanent miss or backoff. */
static int fetch_one(int kind, const char *path, const char *name, af_candidates *a)
{
   int c, k, failed = 0;
   if (!a->ready)
      candidates_init(a, path, name);
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
            break;
         a->crc_tried = 1;
         if (stop_now())
         {
            a->crc_tried = 0;          /* not tried: asked to stop first */
            return -1;
         }
         if (gamedb_file_crc_cancel(path, &crc, stop_now) != 0)
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
            break;
         a->ncand++;
      }
      if (c && !strcmp(a->cand[c], a->cand[c - 1]))
         continue;
      for (k = 0; k < a->nsys; k++)
      {
         char url[AF_NAME_CAP * 3 + 128], enc[AF_NAME_CAP * 3 + 8];
         size_t len = 0;
         int r;
         if (stop_now() || (g_online && !g_online()))
            return -1;
         url_name(a->cand[c], enc, sizeof(enc));
         snprintf(url, sizeof(url), "/%s/%s/%s.png", SYS[a->sys[k]],
                  KIND_URL[kind], enc);
         r = http_get(url, &len);
         if (r == GET_OK)
         {
            if (len < 8 || memcmp(g_body, "\x89PNG\r\n\x1a\n", 8) != 0)
            {
               failed = 1;            /* a portal's page, not a picture */
               continue;
            }
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

typedef struct { af_candidates *candidates; unsigned checked; int counted; } af_item;
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

static uint64_t worker_now(void)
{
   struct timeval tv;
   gettimeofday(&tv, NULL);
   return (uint64_t)tv.tv_sec * 1000000u + (unsigned)tv.tv_usec;
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

static void item_count(af_item *item, int *seen)
{
   unsigned wanted = A_LOAD(g_titles) ? 3u : 1u;
   if (!item->counted && (item->checked & wanted) == wanted)
   {
      item->counted = 1;
      A_STORE(g_prog_seen, ++*seen);
   }
}

static void box_available(af_list *list, af_item *items, int idx, const char *stem)
{
   int j, have = A_LOAD(g_prog_have);
   if (items[idx].checked & AF_HAVE_BOX)
      return;
   /* Shared, case-folded stems gain one cover for every matching cartridge. */
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
   A_STORE(g_prog_have, have);
}

/* -2 is interrupted/allocation failure: never a permanent decision. */
static int item_kind(af_list *list, af_item *items, int idx, int kind,
                     int foreground, int *seen)
{
   af_item *item = &items[idx];
   char stem[AF_NAME_CAP];
   unsigned bit = 1u << kind;
   int r;
   if ((item->checked & bit) && !foreground)
   {
      item_count(item, seen);
      return 0;
   }
   if (stop_now())
      return -2;
   stem_of(list->name[idx], stem, sizeof(stem));
   if (have_art(kind, stem))
   {
      if (kind == AK_BOX)
      {
         box_available(list, items, idx, stem);
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
         item->checked |= bit;
         item_count(item, seen);
         return 2;
      }
      if (!item->candidates)
         item->candidates = calloc(1, sizeof(*item->candidates));
      if (!item->candidates)
         return -2;
      r = fetch_one(kind, list->path[idx], list->name[idx], item->candidates);
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
            box_available(list, items, idx, stem);
            /* A promoted PNG can outlive an interrupted thumbnail write. */
            if (stop_now()) return -2;
         }
      }
   }
   item->checked |= bit;
   item_count(item, seen);
   return r;
}

static void thread_main(void *arg)
{
   af_list *l = NULL;
   af_list *waiting = NULL;
   af_item *items = NULL;
   af_focus *focus = NULL;
   int i = 0, fails = 0, k, warm_i = 0, seen = 0;
   int focus_served = 0, focus_warmed = 0, inventory_dirty = 0;
   int titles_seen = -1;
   uint64_t backoff_until = 0;
   (void)arg;
   for (k = 0; k < AK_KINDS; k++)
      missing_load(k);
   while (!A_LOAD(g_quit))
   {
      int warmed = 0, focus_idx = -1, online_now, titles_now;
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
         waiting = NULL;
         items_free(items, l ? l->n : 0);
         list_free(l);
         l = nl; items = next;
         i = warm_i = seen = fails = 0;
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
         failures_clear();
         set_free(&g_thumb_failed);
         for (k = 0; k < AK_KINDS; k++) missing_clear(k, 1);
         for (k = 0; l && k < l->n; k++)
         {
            free(items[k].candidates);
            memset(&items[k], 0, sizeof(items[k]));
         }
         i = warm_i = seen = fails = focus_served = 0;
         focus_warmed = 0;
         backoff_until = 0;
         inventory_dirty = 1;
         A_STORE(g_prog_seen, 0);
         say("art_fetch retry%s%s", "", "");
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
         /* Keep checked work, but recount against the newly enabled kinds. */
         titles_seen = titles_now;
         i = seen = 0;
         for (k = 0; k < l->n; k++)
         {
            items[k].counted = 0;
            item_count(&items[k], &seen);
         }
         A_STORE(g_prog_seen, seen);
      }
      if (!l || A_LOAD(g_paused))
      {
         A_STORE(g_prog_busy, 0);
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
      online_now = !g_online || g_online();
      A_STORE(g_prog_busy, online_now &&
         (i < l->n || warm_i < l->n || (focus_idx >= 0 && !focus_served)));
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
               int r = item_kind(l, items, focus_idx, AK_BOX, 1, &seen);
               if (r == -2) { nap_us(250000); continue; }
               if (r < 0) failure_record(focus->path, focus->name);
               focus_served = 1;
               continue;
            }
         }
      }
      if (warm_i < l->n)
      {
         if (warm_one(l->name[warm_i])) done(l->name[warm_i]);
         if (stop_now()) continue;
         warm_i++;
         warmed = 1;
      }
      while (i < l->n && items[i].counted) i++;
      if (i >= l->n || !online_now)
      {
         if (!warmed) nap_us(250000);
         continue;
      }
      {
         int failed = 0, interrupted = 0;
         uint64_t now = worker_now();
         if (now < backoff_until)
         {
            nap_us((uint32_t)(backoff_until - now));
            continue;
         }
         for (k = 0; k < AK_KINDS; k++)
         {
            int r;
            if (k == AK_TITLE && !A_LOAD(g_titles)) continue;
            r = item_kind(l, items, i, k, 0, &seen);
            if (r == -2) { interrupted = 1; break; }
            if (r < 0) { failed = 1; break; }
         }
         if (interrupted) { nap_us(250000); continue; }
         if (failed)
         {
            items[i].checked |= A_LOAD(g_titles) ? 3u : 1u;
            item_count(&items[i], &seen);
            if (++fails >= 3)
            {
               backoff_until = worker_now() + AF_COOLDOWN;
               fails = 0;
            }
         }
         else fails = 0;
         if (items[i].counted) i++;
      }
   }
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
