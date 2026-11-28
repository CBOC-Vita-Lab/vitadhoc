#include "artfetch.h"
#include "gamedb.h"
#include "plat.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static pthread_t worker;
static int finished, joined;
static void (*background)(void *);
static void *background_arg;
static unsigned art_opens, rom_opens, requests, first_art_opens;
static unsigned art_stats, first_art_stats;
static unsigned long long rom_bytes, first_rom_bytes, began, first_request;
static unsigned delay_us;
static FILE *rom_files[16];

uint64_t plat_time_us(void)
{
   struct timespec t;
   clock_gettime(CLOCK_MONOTONIC, &t);
   return (uint64_t)t.tv_sec * 1000000 + (unsigned)t.tv_nsec / 1000;
}

void plat_thread_sleep_us(uint32_t us)
{
   struct timespec t = { us / 1000000, (long)(us % 1000000) * 1000 };
   nanosleep(&t, NULL);
}

static void *thread_run(void *arg)
{
   (void)arg;
   background(background_arg);
   __atomic_store_n(&finished, 1, __ATOMIC_RELEASE);
   return NULL;
}

int plat_bg_start(void (*fn)(void *), void *arg)
{
   background = fn;
   background_arg = arg;
   return pthread_create(&worker, NULL, thread_run, NULL);
}

int plat_bg_join(uint32_t timeout_us)
{
   uint64_t end = plat_time_us() + timeout_us;
   while (!__atomic_load_n(&finished, __ATOMIC_ACQUIRE) && plat_time_us() < end)
      plat_thread_sleep_us(1000);
   if (!__atomic_load_n(&finished, __ATOMIC_ACQUIRE))
      return 0;
   joined = pthread_join(worker, NULL) == 0;
   return joined;
}

/* Instrument only the production translation units, not the test driver. */
FILE *af_fopen(const char *path, const char *mode)
{
   FILE *f;
   int i;
   if (strstr(path, "/boxart/") || strstr(path, "/titles/"))
   {
      art_opens++;
      if (delay_us)
         plat_thread_sleep_us(delay_us);
   }
   f = fopen(path, mode);
   if (strstr(path, "/roms/") && mode[0] == 'r')
   {
      rom_opens++;
      for (i = 0; f && i < 16; i++)
         if (!rom_files[i]) { rom_files[i] = f; break; }
   }
   return f;
}

int af_stat(const char *path, struct stat *st)
{
   if (strstr(path, "/boxart/") || strstr(path, "/titles/"))
      art_stats++;
   return stat(path, st);
}

size_t af_fread(void *ptr, size_t size, size_t n, FILE *f)
{
   size_t got = fread(ptr, size, n, f);
   int i;
   for (i = 0; i < 16; i++)
      if (rom_files[i] == f) { rom_bytes += got * size; break; }
   return got;
}

int af_fclose(FILE *f)
{
   int i;
   for (i = 0; i < 16; i++)
      if (rom_files[i] == f) rom_files[i] = NULL;
   return fclose(f);
}

static unsigned resolves;
#undef getaddrinfo
int af_getaddrinfo(const char *node, const char *service,
                   const struct addrinfo *hints, struct addrinfo **res)
{
   resolves++;
   return getaddrinfo(node, service, hints, res);
}

int af_connect(int fd, const struct sockaddr *address, socklen_t size)
{
   if (!requests++)
   {
      first_art_opens = art_opens;
      first_art_stats = art_stats;
      first_rom_bytes = rom_bytes;
      first_request = plat_time_us() - began;
   }
   return connect(fd, address, size);
}

static int online(void)
{
   return getenv("AF_OFFLINE") == NULL;
}

/* Async mock HTTPS backend for tests: rewrites https://raw.githubusercontent.com/<path>
 * to http://127.0.0.1:$AF_MOCK_HTTPS_PORT/<path> on a background thread. */
typedef struct mock_req
{
   pthread_t th;
   int started;
   int port;
   char path[1400];
   size_t max_bytes;
   int cancel;
   int done;
   int status;
   unsigned char *body;
   size_t body_len;
} mock_req;

static void *mock_https_thread(void *arg)
{
   mock_req *r = (mock_req *)arg;
   struct sockaddr_in sa;
   char req[1600];
   unsigned char *buf = NULL;
   size_t cap = r->max_bytes + 4096u, got = 0;
   int fd, nreq;

   fd = socket(AF_INET, SOCK_STREAM, 0);
   if (fd < 0) goto out;
   memset(&sa, 0, sizeof(sa));
   sa.sin_family = AF_INET;
   sa.sin_port = htons((uint16_t)r->port);
   sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
   if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) { close(fd); goto out; }
   nreq = snprintf(req, sizeof(req),
                   "GET %s HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n",
                   r->path);
   if (nreq <= 0 || nreq >= (int)sizeof(req) ||
       send(fd, req, (size_t)nreq, 0) != (ssize_t)nreq)
   {
      close(fd);
      goto out;
   }
   buf = (unsigned char *)malloc(cap + 1);
   if (!buf) { close(fd); goto out; }
   while (!__atomic_load_n(&r->cancel, __ATOMIC_ACQUIRE) && got < cap)
   {
      ssize_t n = recv(fd, buf + got, cap - got, 0);
      if (n <= 0) break;
      got += (size_t)n;
   }
   close(fd);
   buf[got] = '\0';
   if (!__atomic_load_n(&r->cancel, __ATOMIC_ACQUIRE))
   {
      char *hdr_end = strstr((char *)buf, "\r\n\r\n");
      int minor = 0, st = 0;
      if (hdr_end && sscanf((char *)buf, "HTTP/1.%d %d", &minor, &st) == 2)
      {
         size_t off = (size_t)(hdr_end + 4 - (char *)buf);
         size_t blen = got - off;
         r->status = st;
         if (st == 200 && blen > 0 && blen <= r->max_bytes)
         {
            memmove(buf, buf + off, blen);
            r->body = buf;
            r->body_len = blen;
            buf = NULL;
         }
      }
   }
   free(buf);
out:
   __atomic_store_n(&r->done, 1, __ATOMIC_RELEASE);
   return NULL;
}

static int g_mock_https_port;

static uintptr_t mock_https_start(const char *url, size_t max_bytes)
{
   static const char prefix[] = "https://raw.githubusercontent.com";
   mock_req *r;
   if (!g_mock_https_port || !url || strncmp(url, prefix, sizeof(prefix) - 1) != 0)
      return 0;
   r = (mock_req *)calloc(1, sizeof(*r));
   if (!r) return 0;
   r->port = g_mock_https_port;
   r->max_bytes = max_bytes;
   snprintf(r->path, sizeof(r->path), "%s", url + sizeof(prefix) - 1);
   if (pthread_create(&r->th, NULL, mock_https_thread, r) != 0)
   {
      free(r);
      return 0;
   }
   r->started = 1;
   return (uintptr_t)r;
}

static int mock_https_poll(uintptr_t h, int *http_status,
                           const unsigned char **out_body, size_t *out_len)
{
   mock_req *r = (mock_req *)h;
   if (!r) return -1;
   if (!__atomic_load_n(&r->done, __ATOMIC_ACQUIRE)) return 0;
   if (http_status) *http_status = r->status;
   if (out_body) *out_body = r->body;
   if (out_len) *out_len = r->body_len;
   return 1;
}

static void mock_https_cancel(uintptr_t h)
{
   mock_req *r = (mock_req *)h;
   if (r) __atomic_store_n(&r->cancel, 1, __ATOMIC_RELEASE);
}

static void mock_https_release(uintptr_t h)
{
   mock_req *r = (mock_req *)h;
   if (!r) return;
   __atomic_store_n(&r->cancel, 1, __ATOMIC_RELEASE);
   if (r->started) pthread_join(r->th, NULL);
   free(r->body);
   free(r);
}

static const artfetch_https_ops g_mock_https_ops = {
   mock_https_start,
   mock_https_poll,
   mock_https_cancel,
   mock_https_release
};

int main(int argc, char **argv)
{
   FILE *list;
   char line[1800], name[256], db[1024];
   char **names = NULL, **paths = NULL;
   int n = 0, seen = 0, total = 0, have = 0, done = 0, busy = 0, i;
   int hold, command_sent = 0, expected, retry_seen = 0, pause_quiet = 0;
   int resync, resync_cleared;
   unsigned relist_base = 0;
   unsigned long long deadline, first_cover = 0, stopping, stop_us, relist_at = 0;
   if (argc != 7) return 2;
   hold = !strncmp(argv[5], "hold", 4);
   delay_us = getenv("AF_IO_DELAY_US") ? (unsigned)atoi(getenv("AF_IO_DELAY_US")) : 0;
   list = fopen(argv[3], "r");
   if (!list) return 3;
   while (fgets(line, sizeof(line), list))
   {
      char *tab = strchr(line, '\t');
      if (!tab) return 4;
      *tab++ = '\0';
      tab[strcspn(tab, "\r\n")] = '\0';
      names = realloc(names, (size_t)(n + 1) * sizeof(*names));
      paths = realloc(paths, (size_t)(n + 1) * sizeof(*paths));
      if (!names || !paths) return 5;
      names[n] = strdup(line);
      paths[n++] = strdup(tab);
   }
   fclose(list);
   expected = n;
   snprintf(db, sizeof(db), "%s/gamedb.txt", argv[1]);
   gamedb_open(db);
   began = plat_time_us();
   g_mock_https_port = getenv("AF_MOCK_HTTPS_PORT") ? atoi(getenv("AF_MOCK_HTTPS_PORT")) : 0;
   artfetch_set_https(g_mock_https_port > 0 ? &g_mock_https_ops : NULL);
   if (artfetch_start(argv[1], argv[2], online)) return 6;
   artfetch_titles(atoi(argv[4]));
   if (!strcmp(argv[5], "retry")) artfetch_retry();
   if (!strcmp(argv[5], "pause")) artfetch_pause(1);
   artfetch_list((const char *const *)paths, (const char *const *)names, n);
   if (!strcmp(argv[5], "pause"))
   {
      plat_thread_sleep_us(100000);
      artfetch_pause(0);
   }
   deadline = began + (unsigned)atoi(argv[6]) * 1000u;
   while (plat_time_us() < deadline)
   {
      while (!hold && artfetch_take(name, sizeof(name)))
      {
         done++;
         if (!first_cover) first_cover = plat_time_us() - began;
      }
      while (artfetch_log(line, sizeof(line)))
      {
         if (strstr(line, "art_fetch retry")) retry_seen = 1;
         fprintf(stderr, "%s\n", line);
      }
      busy = artfetch_progress(&seen, &total, &have);
      if (!command_sent && have >= 17 && !strcmp(argv[5], "hold_retry"))
      {
         artfetch_retry();
         command_sent = 1;
      }
      if (!command_sent && have >= 17 && !strcmp(argv[5], "hold_replace"))
      {
         artfetch_list((const char *const *)&paths[n - 1],
                       (const char *const *)&names[n - 1], 1);
         expected = 1;
         command_sent = 1;
      }
      if (!command_sent && seen >= 1 && !strcmp(argv[5], "hold_pause"))
      {
         unsigned accesses;
         unsigned long long paused_until = plat_time_us() + 1000000;
         artfetch_pause(1);
         while (artfetch_progress(NULL, NULL, NULL) && plat_time_us() < paused_until)
            plat_thread_sleep_us(1000);
         accesses = art_opens + art_stats;
         plat_thread_sleep_us(100000);
         pause_quiet = accesses == art_opens + art_stats;
         artfetch_pause(0);
         command_sent = 1;
      }
      /* "relist": the same list handed over again once the first pass is
       * done (as Home does after a console change); counts what the
       * second pass opens. */
      if (!command_sent && !strcmp(argv[5], "relist") && done >= 1)
      {
         relist_base = art_opens;
         artfetch_list((const char *const *)paths, (const char *const *)names, n);
         relist_at = plat_time_us();
         command_sent = 1;
         continue;
      }
      if (!strcmp(argv[5], "relist"))
      {
         if (command_sent && plat_time_us() - relist_at > 600000) break;
         plat_thread_sleep_us(1000);
         continue;
      }
      if ((total == expected && seen == total && !busy &&
           (strcmp(argv[5], "hold_retry") || retry_seen)) ||
          (!strcmp(argv[5], "first") && done)) break;
      plat_thread_sleep_us(1000);
   }
   stopping = plat_time_us();
   artfetch_stop();
   stop_us = plat_time_us() - stopping;
#ifdef AF_LEGACY_WORKER
   resync = 0;
   resync_cleared = 1;
#else
   resync = artfetch_resync();
   resync_cleared = artfetch_resync() == 0;
#endif
   printf("{\"total\":%d,\"seen\":%d,\"have\":%d,\"done\":%d,"
          "\"requests\":%u,\"art_opens\":%u,\"rom_opens\":%u,"
          "\"rom_bytes\":%llu,\"first_art_opens\":%u,\"first_rom_bytes\":%llu,"
          "\"art_stats\":%u,\"first_art_stats\":%u,"
          "\"command_sent\":%d,\"retry_seen\":%d,\"pause_quiet\":%d,\"stop_us\":%llu,"
          "\"resync\":%d,\"resync_cleared\":%d,\"relist_opens\":%u,\"resolves\":%u,"
          "\"first_request_us\":%llu,\"first_cover_us\":%llu,\"elapsed_us\":%llu,\"joined\":%d}\n",
          total, seen, have, done, requests, art_opens, rom_opens,
          rom_bytes, first_art_opens, first_rom_bytes, art_stats, first_art_stats,
          command_sent, retry_seen, pause_quiet, stop_us, resync, resync_cleared,
          command_sent ? art_opens - relist_base : 0u, resolves,
          first_request, first_cover,
          (unsigned long long)(plat_time_us() - began), joined);
   for (i = 0; i < n; i++) { free(names[i]); free(paths[i]); }
   free(names); free(paths);
   gamedb_close();
   return joined ? 0 : 7;
}
