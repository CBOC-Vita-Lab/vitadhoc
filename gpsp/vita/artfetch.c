/* artfetch.c -- see artfetch.h. */
#include "artfetch.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "gamedb.h"
#include "plat.h"

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
   char (*path)[AF_PATH_CAP];
   char (*name)[AF_NAME_CAP];
} af_list;

/* ---- what the UI and the thread share ---------------------------------- */

static af_list *g_new;                /* atomic: the newest list, handed over */
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
   while (w - A_LOAD(g_done_r) >= AF_RING && !A_LOAD(g_quit))
      plat_thread_sleep_us(100000);    /* (only if the UI stopped reading) */
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

void artfetch_list(const char *const *paths, const char *const *names, int n)
{
   af_list *l;
   int i;
   if (!A_LOAD(g_running) || n <= 0)
      return;
   l = (af_list *)calloc(1, sizeof(*l));
   if (!l)
      return;
   l->path = calloc((size_t)n, sizeof(*l->path));
   l->name = calloc((size_t)n, sizeof(*l->name));
   if (!l->path || !l->name)
   {
      list_free(l);
      return;
   }
   for (i = 0; i < n; i++)
   {
      snprintf(l->path[i], AF_PATH_CAP, "%s", paths[i]);
      snprintf(l->name[i], AF_NAME_CAP, "%s", names[i]);
   }
   l->n = n;
   list_free(__atomic_exchange_n(&g_new, l, __ATOMIC_SEQ_CST));
}

/* ---- the thread's own --------------------------------------------------- */

/* <kind dir>/.missing.txt, in memory */
static char **g_missing[AK_KINDS];
static int g_nmissing[AK_KINDS], g_capmissing[AK_KINDS];
static unsigned char *g_body;

static int stop_now(void)
{
   return A_LOAD(g_quit) || A_LOAD(g_paused);
}

/* A pause that ends early when the thread is told to stop. */
static void nap_us(uint32_t us)
{
   while (us && !A_LOAD(g_quit))
   {
      uint32_t d = us < 250000u ? us : 250000u;
      plat_thread_sleep_us(d);
      us -= d;
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
      f = fopen(path, "a");
      if (f)
      {
         fprintf(f, "%s\n", stem);
         fclose(f);
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

/* <kind dir>/ has a picture for it already, of any kind the list reads. */
static int have_art(int k, const char *stem)
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

   snprintf(port, sizeof(port), "%d", g_port);
   memset(&hints, 0, sizeof(hints));
   hints.ai_family = AF_INET;
   hints.ai_socktype = SOCK_STREAM;
   if (getaddrinfo(g_host, port, &hints, &ai) != 0 || !ai)
      return GET_FAILED;
   s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
   if (s < 0)
      goto out;
   tv.tv_sec = 15;
   tv.tv_usec = 0;
   setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
   setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
   if (connect(s, ai->ai_addr, ai->ai_addrlen) != 0)
      goto out;
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
      if (r <= 0)
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
   freeaddrinfo(ai);
   return rc;
}

/* The picture to <kind dir>/<stem>.png, whole or not at all. */
static int save(int k, const char *stem, size_t len)
{
   char tmp[AF_PATH_CAP + AF_NAME_CAP + 32], dst[sizeof(tmp)];
   FILE *f;
   int ok;
   snprintf(dst, sizeof(dst), "%s/%s", g_root, KIND_DIR[k]);
   mkdir(dst, 0777);                   /* titles/ is made on the first one */
   snprintf(dst, sizeof(dst), "%s/%s/%s.png", g_root, KIND_DIR[k], stem);
   snprintf(tmp, sizeof(tmp), "%s/%s/%s.png.tmp", g_root, KIND_DIR[k], stem);
   f = fopen(tmp, "wb");
   if (!f)
      return -1;
   ok = fwrite(g_body, 1, len, f) == len;
   ok = fclose(f) == 0 && ok;
   if (!ok || rename(tmp, dst) != 0)
   {
      remove(tmp);
      return -1;
   }
   return 0;
}

static const char *const SYS[3] = {
   "Nintendo%20-%20Game%20Boy%20Advance", "Nintendo%20-%20Game%20Boy",
   "Nintendo%20-%20Game%20Boy%20Color" };

/* One cartridge.  1 = a picture was written, 0 = none there (missing),
 * -1 = it could not be asked (offline, paused, a failure: try later). */
static int fetch_one(int kind, const char *path, const char *name)
{
   char stem[AF_NAME_CAP], cand[3][AF_NAME_CAP];
   const char *ext = strrchr(name, '.');
   int ncand = 0, sys[2], nsys, c, k, failed = 0;
   uint32_t crc;

   stem_of(name, stem, sizeof(stem));
   if (ext && !strcmp(ext, ".gba"))
   {
      sys[0] = 0;
      nsys = 1;
   }
   else if (ext && !strcmp(ext, ".gbc"))
   {
      sys[0] = 2; sys[1] = 1;
      nsys = 2;
   }
   else
   {
      sys[0] = 1; sys[1] = 2;
      nsys = 2;
   }
   /* the names it may be filed under, the likeliest first */
   if (gamedb_known_name(stem))
      snprintf(cand[ncand++], AF_NAME_CAP, "%s", stem);
   if (sys[0] == 0 && gamedb_name_by_header(path, cand[ncand], AF_NAME_CAP))
      ncand++;
   if (!ncand)
   {
      if (gamedb_file_crc(path, &crc, &g_quit) == 0 &&
          gamedb_name_by_crc(crc, cand[ncand], AF_NAME_CAP))
         ncand++;
      if (stop_now())
         return -1;
   }
   if (!ncand || strcmp(cand[0], stem) != 0)
      snprintf(cand[ncand++], AF_NAME_CAP, "%s", stem);

   for (c = 0; c < ncand; c++)
   {
      if (c && !strcmp(cand[c], cand[c - 1]))
         continue;
      for (k = 0; k < nsys; k++)
      {
         char url[AF_NAME_CAP * 3 + 128], enc[AF_NAME_CAP * 3 + 8];
         size_t len = 0;
         int r;
         if (stop_now() || (g_online && !g_online()))
            return -1;
         url_name(cand[c], enc, sizeof(enc));
         snprintf(url, sizeof(url), "/%s/%s/%s.png", SYS[sys[k]],
                  KIND_URL[kind], enc);
         r = http_get(url, &len);
         if (r == GET_OK)
         {
            if (len < 8 || memcmp(g_body, "\x89PNG\r\n\x1a\n", 8) != 0)
            {
               failed = 1;            /* a portal's page, not a picture */
               continue;
            }
            if (save(kind, stem, len) != 0)
               return -1;
            say(kind == AK_BOX ? "art_fetch ok name=\"%s\" as=\"%s\""
                               : "art_fetch title name=\"%s\" as=\"%s\"",
                stem, cand[c]);
            return 1;
         }
         if (r == GET_FAILED)
            failed = 1;
      }
   }
   if (failed)
   {
      say("art_fetch failed name=\"%s\"%s", stem, kind ? " kind=title" : "");
      return -1;
   }
   say("art_fetch none name=\"%s\"%s", stem, kind ? " kind=title" : "");
   return 0;
}

/* For Settings' count: the cartridges of the list with a box art now. */
static int count_have(const af_list *l)
{
   int i, n = 0;
   char stem[AF_NAME_CAP];
   for (i = 0; l && i < l->n; i++)
   {
      stem_of(l->name[i], stem, sizeof(stem));
      n += have_art(AK_BOX, stem);
   }
   return n;
}

static void thread_main(void *arg)
{
   af_list *l = NULL;
   int i = 0, fails = 0, k;
   (void)arg;
   for (k = 0; k < AK_KINDS; k++)
      missing_load(k);
   while (!A_LOAD(g_quit))
   {
      af_list *nl = __atomic_exchange_n(&g_new, NULL, __ATOMIC_SEQ_CST);
      if (__atomic_exchange_n(&g_retry, 0, __ATOMIC_SEQ_CST))
      {
         /* Settings > Covers: forget what was not found, look again */
         for (k = 0; k < AK_KINDS; k++)
            missing_clear(k, 1);
         i = 0;
         say("art_fetch retry%s%s", "", "");
      }
      if (nl)
      {
         list_free(l);
         l = nl;
         i = 0;
         A_STORE(g_prog_total, l->n);
         A_STORE(g_prog_have, count_have(l));
      }
      if (l)
         A_STORE(g_prog_seen, i < l->n ? i : l->n);
      A_STORE(g_prog_busy, l && i < l->n && !A_LOAD(g_paused) &&
                           (!g_online || g_online()));
      if (!l || i >= l->n || A_LOAD(g_paused) || (g_online && !g_online()))
      {
         nap_us(250000);
         continue;
      }
      {
         char stem[AF_NAME_CAP];
         int r = 0, got = 0, failed = 0;
         stem_of(l->name[i], stem, sizeof(stem));
         for (k = 0; k < AK_KINDS && !failed; k++)
         {
            if (k == AK_TITLE && !A_LOAD(g_titles))
               continue;
            if (have_art(k, stem) || missing_has(k, stem))
               continue;
            r = fetch_one(k, l->path[i], l->name[i]);
            if (r == 1)
            {
               got = 1;
               if (k == AK_BOX)
                  A_STORE(g_prog_have, A_LOAD(g_prog_have) + 1);
            }
            else if (r == 0)
               missing_add(k, stem, 1);
            else
               failed = 1;
         }
         if (got)
         {
            done(l->name[i]);
            fails = 0;
         }
         r = failed ? -1 : 0;
         if (r >= 0)
            i++;
         else if (!stop_now())
         {
            /* the network failed: the next one, and this one again on the
             * next list; after a few, a pause */
            i++;
            if (++fails >= 3)
            {
               nap_us(30000000);
               fails = 0;
            }
         }
      }
   }
   list_free(l);
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
   A_STORE(g_running, 0);
}
