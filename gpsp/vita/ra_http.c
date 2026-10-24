/* ra_http.c -- see ra_http.h. */
#include "ra_http.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#ifdef __vita__
#include <psp2/libssl.h>
#include <psp2/net/http.h>
#include <psp2/sysmodule.h>
#include "ra_roots.h"
#endif

#include "plat.h"
#include "rhash/md5.h"

#define A_LOAD(x)      __atomic_load_n(&(x), __ATOMIC_SEQ_CST)
#define A_STORE(x, v)  __atomic_store_n(&(x), (v), __ATOMIC_SEQ_CST)

/* The one request's life: EMPTY -> ASKED (the game thread filled it in) ->
 * DONE (the net thread answered) -> EMPTY (taken). */
enum { ST_EMPTY = 0, ST_ASKED, ST_DONE };

static struct
{
   int    state;                 /* atomic */
   int    quit;                  /* atomic */
   int    running;
   char  *url, *post, *type;     /* the request (owned while ASKED)  */
   int    hash;                  /* the job is ra_http_hash_file(url) */
   ra_http_res res;              /* the answer (owned while DONE)    */
   char   agent[192];
   int  (*online)(void);
} H = { 0, 0, 0, NULL, NULL, NULL, 0, { 0, NULL, 0, "" }, "VitAdhoc", NULL };

void ra_http_config(const char *user_agent, int (*online)(void))
{
   if (user_agent)
      snprintf(H.agent, sizeof(H.agent), "%s", user_agent);
   H.online = online;
}

int ra_http_split_url(const char *url, char *host, size_t hcap, int *port,
                      const char **path)
{
   const char *h, *e, *slash;
   size_t n;
   if (strncmp(url, "http://", 7))
      return -1;
   h = url + 7;
   slash = strchr(h, '/');
   e = slash ? slash : h + strlen(h);
   *path = slash ? slash : "/";
   *port = 80;
   {
      const char *colon = memchr(h, ':', (size_t)(e - h));
      if (colon)
      {
         *port = atoi(colon + 1);
         e = colon;
      }
   }
   n = (size_t)(e - h);
   if (!n || n >= hcap || *port <= 0 || *port > 65535)
      return -1;
   memcpy(host, h, n);
   host[n] = '\0';
   return 0;
}

/* ---- the answer's buffer --------------------------------------------------- */

static int body_room(size_t *cap, size_t need)
{
   char *b;
   size_t c = *cap ? *cap : 16384;
   if (need + 1 <= *cap)
      return 0;
   if (need + 1 > RA_HTTP_MAX_BODY)
      return -1;
   while (c < need + 1)
      c *= 2;
   if (c > RA_HTTP_MAX_BODY)
      c = RA_HTTP_MAX_BODY;
   b = (char *)realloc(H.res.body, c);
   if (!b)
      return -1;
   H.res.body = b;
   *cap = c;
   return 0;
}

static void fail(int status, const char *why)
{
   free(H.res.body);
   H.res.body = NULL;
   H.res.len = 0;
   H.res.status = status;
   snprintf(H.res.note, sizeof(H.res.note), "%s", why);
}

/* ---- http:// over sockets --------------------------------------------------- */

/* Headers end in the buffer's first `got` bytes: parse the status, the
 * length and the coding.  The body is moved to the front. */
static void http_plain(void)
{
   char host[128], portstr[8], *hdr = NULL;
   const char *path;
   int port, s = -1, status = 0, chunked = 0;
   struct addrinfo hints, *ai = NULL;
   struct timeval tv;
   size_t cap = 0, got = 0, body_at = 0, want = (size_t)-1, plen;

   if (ra_http_split_url(H.url, host, sizeof(host), &port, &path))
   {
      fail(-1, "bad url");
      return;
   }
   snprintf(portstr, sizeof(portstr), "%d", port);
   memset(&hints, 0, sizeof(hints));
   hints.ai_family = AF_INET;
   hints.ai_socktype = SOCK_STREAM;
   if (getaddrinfo(host, portstr, &hints, &ai) != 0 || !ai)
   {
      fail(RA_HTTP_RETRY, "dns");
      goto out;
   }
   s = socket(AF_INET, SOCK_STREAM, 0);
   if (s < 0)
   {
      fail(RA_HTTP_RETRY, "socket");
      goto out;
   }
   tv.tv_sec = 20;
   tv.tv_usec = 0;
   setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
   setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
   if (connect(s, ai->ai_addr, ai->ai_addrlen) != 0)
   {
      fail(RA_HTTP_RETRY, "connect");
      goto out;
   }
   plen = H.post ? strlen(H.post) : 0;
   hdr = (char *)malloc(1024 + strlen(path));
   if (!hdr)
   {
      fail(RA_HTTP_RETRY, "memory");
      goto out;
   }
   if (H.post)
      sprintf(hdr, "POST %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: %s\r\n"
              "Content-Type: %s\r\nContent-Length: %u\r\n"
              "Connection: close\r\n\r\n", path, host, H.agent,
              H.type ? H.type : "application/x-www-form-urlencoded",
              (unsigned)plen);
   else
      sprintf(hdr, "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: %s\r\n"
              "Connection: close\r\n\r\n", path, host, H.agent);
   if (send(s, hdr, strlen(hdr), 0) != (ssize_t)strlen(hdr) ||
       (plen && send(s, H.post, plen, 0) != (ssize_t)plen))
   {
      fail(RA_HTTP_RETRY, "send");
      goto out;
   }
   for (;;)
   {
      ssize_t r;
      if (A_LOAD(H.quit) || body_room(&cap, got + 8192))
      {
         fail(RA_HTTP_RETRY, A_LOAD(H.quit) ? "stopped" : "too big");
         goto out;
      }
      r = recv(s, H.res.body + got, cap - 1 - got, 0);
      if (r < 0)
      {
         fail(RA_HTTP_RETRY, "recv");
         goto out;
      }
      if (r == 0)
         break;
      got += (size_t)r;
      H.res.body[got] = '\0';
      if (!body_at)
      {
         char *end = strstr(H.res.body, "\r\n\r\n"), *p;
         if (!end)
            continue;
         body_at = (size_t)(end + 4 - H.res.body);
         if (sscanf(H.res.body, "HTTP/1.%*d %d", &status) != 1)
         {
            fail(RA_HTTP_RETRY, "not http");
            goto out;
         }
         for (p = H.res.body; p < end; p++)
            *p = (char)tolower((unsigned char)*p);
         *end = '\0';
         p = strstr(H.res.body, "\r\ncontent-length:");
         if (p)
            want = (size_t)strtoul(p + 17, NULL, 10);
         chunked = strstr(H.res.body, "\r\ntransfer-encoding: chunked") != NULL;
         *end = '\r';
      }
      if (body_at && !chunked && want != (size_t)-1 && got - body_at >= want)
         break;
   }
   if (!body_at)
   {
      fail(RA_HTTP_RETRY, "no headers");
      goto out;
   }
   {
      size_t n = got - body_at;
      memmove(H.res.body, H.res.body + body_at, n);
      H.res.body[n] = '\0';
      if (chunked)
      {
         size_t rd = 0, wr = 0;
         for (;;)
         {
            char *e, *crlf;
            unsigned long c;
            if (!isxdigit((unsigned char)H.res.body[rd]))
            {
               fail(RA_HTTP_RETRY, "bad chunk");
               goto out;
            }
            c = strtoul(H.res.body + rd, &e, 16);
            crlf = strstr(H.res.body + rd, "\r\n");
            if (!crlf || e == H.res.body + rd)
            {
               fail(RA_HTTP_RETRY, "bad chunk");
               goto out;
            }
            rd = (size_t)(crlf + 2 - H.res.body);
            if (!c)
               break;
            /* (no rd + c: a size near the top of the range wraps it) */
            if (rd > n || c > n - rd || n - rd - c < 2 ||
                H.res.body[rd + c] != '\r' || H.res.body[rd + c + 1] != '\n')
            {
               fail(RA_HTTP_RETRY, "cut short");
               goto out;
            }
            memmove(H.res.body + wr, H.res.body + rd, c);
            wr += c;
            rd += c + 2;
         }
         n = wr;
         H.res.body[n] = '\0';
      }
      else if (want != (size_t)-1 && n < want)
      {
         fail(RA_HTTP_RETRY, "cut short");
         goto out;
      }
      H.res.len = n;
      H.res.status = status;
      snprintf(H.res.note, sizeof(H.res.note), "http");
   }
out:
   free(hdr);
   if (s >= 0)
      close(s);
   if (ai)
      freeaddrinfo(ai);
}

/* ---- https:// through the Vita's library ------------------------------------ */

#ifdef __vita__
static int g_tls;            /* 0 not up (tried again each request), 1 up */
static int g_tpl = -1;
static int g_certs_rc;

static void tls_up(void)
{
   static SceHttpsData data[RA_ROOTS_N];
   static const SceHttpsData *list[RA_ROOTS_N];
   int rc, i;
   if (g_tls == 1)
      return;
   if (sceSysmoduleIsLoaded(SCE_SYSMODULE_HTTPS) != SCE_SYSMODULE_LOADED &&
       sceSysmoduleLoadModule(SCE_SYSMODULE_HTTPS) < 0)
      return;
   rc = sceSslInit(300 * 1024);
   if (rc < 0 && (unsigned)rc != (unsigned)SCE_SSL_ERROR_ALREADY_INITED)
      return;
   rc = sceHttpInit(256 * 1024);
   if (rc < 0 && (unsigned)rc != (unsigned)SCE_HTTP_ERROR_ALREADY_INITED)
      return;
   for (i = 0; i < RA_ROOTS_N; i++)
   {
      data[i].ptr = (char *)ra_roots[i];
      data[i].size = ra_root_sizes[i];
      list[i] = &data[i];
   }
   /* Beside the console's own store; if the library will not take them the
    * console's store is what is left. */
   if (g_tpl < 0)
      g_certs_rc = sceHttpsLoadCert(RA_ROOTS_N, list, NULL, NULL);
   if (g_tpl < 0)
      g_tpl = sceHttpCreateTemplate(H.agent, SCE_HTTP_VERSION_1_1, SCE_TRUE);
   if (g_tpl < 0)
      return;
   sceHttpSetResolveTimeOut(g_tpl, 10 * 1000000);
   sceHttpSetConnectTimeOut(g_tpl, 15 * 1000000);
   sceHttpSetSendTimeOut(g_tpl, 20 * 1000000);
   sceHttpSetRecvTimeOut(g_tpl, 20 * 1000000);
   g_tls = 1;
}

static void http_tls(void)
{
   int conn = -1, req = -1, rc, status = 0;
   unsigned long long want = (unsigned long long)-1;
   size_t cap = 0, got = 0, plen = H.post ? strlen(H.post) : 0;
   char why[96];

   tls_up();
   if (g_tls != 1)
   {
      fail(RA_HTTP_RETRY, "tls library");
      return;
   }
   conn = sceHttpCreateConnectionWithURL(g_tpl, H.url, SCE_FALSE);
   if (conn < 0)
   {
      snprintf(why, sizeof(why), "connection 0x%08x", (unsigned)conn);
      fail(RA_HTTP_RETRY, why);
      return;
   }
   req = sceHttpCreateRequestWithURL(conn, H.post ? SCE_HTTP_METHOD_POST
                                                  : SCE_HTTP_METHOD_GET,
                                     H.url, (unsigned long long)plen);
   if (req < 0)
   {
      snprintf(why, sizeof(why), "request 0x%08x", (unsigned)req);
      fail(RA_HTTP_RETRY, why);
      goto out;
   }
   if (H.post)
      sceHttpAddRequestHeader(req, "Content-Type",
                              H.type ? H.type
                                     : "application/x-www-form-urlencoded",
                              SCE_HTTP_HEADER_OVERWRITE);
   rc = sceHttpSendRequest(req, H.post, (unsigned)plen);
   if (rc < 0)
   {
      int err = 0;
      unsigned detail = 0;
      sceHttpsGetSslError(req, &err, &detail);
      snprintf(why, sizeof(why), "send 0x%08x ssl 0x%08x/0x%x certs 0x%08x",
               (unsigned)rc, (unsigned)err, detail, (unsigned)g_certs_rc);
      fail(RA_HTTP_RETRY, why);
      goto out;
   }
   if (sceHttpGetStatusCode(req, &status) < 0)
   {
      fail(RA_HTTP_RETRY, "status");
      goto out;
   }
   if (sceHttpGetResponseContentLength(req, &want) < 0)
      want = (unsigned long long)-1;   /* chunked: the library's to join */
   for (;;)
   {
      if (A_LOAD(H.quit) || body_room(&cap, got + 8192))
      {
         fail(RA_HTTP_RETRY, A_LOAD(H.quit) ? "stopped" : "too big");
         goto out;
      }
      rc = sceHttpReadData(req, H.res.body + got, (unsigned)(cap - 1 - got));
      if (rc < 0)
      {
         snprintf(why, sizeof(why), "read 0x%08x", (unsigned)rc);
         fail(RA_HTTP_RETRY, why);
         goto out;
      }
      if (rc == 0)
         break;
      got += (size_t)rc;
   }
   if (want != (unsigned long long)-1 && got < want)
   {
      fail(RA_HTTP_RETRY, "cut short");
      goto out;
   }
   if (body_room(&cap, got))
   {
      fail(RA_HTTP_RETRY, "memory");
      goto out;
   }
   H.res.body[got] = '\0';
   H.res.len = got;
   H.res.status = status;
   snprintf(H.res.note, sizeof(H.res.note), "https certs 0x%08x",
            (unsigned)g_certs_rc);
out:
   if (req >= 0)
      sceHttpDeleteRequest(req);
   if (conn >= 0)
      sceHttpDeleteConnection(conn);
}
#endif

/* ---- a file's MD5 ------------------------------------------------------------ */

static void hash_file(void)
{
   static unsigned char buf[64 * 1024];
   md5_state_t md5;
   md5_byte_t d[16];
   FILE *f = fopen(H.url, "rb");
   size_t n;
   int i, bad;
   if (!f)
   {
      fail(-1, "unreadable");
      return;
   }
   md5_init(&md5);
   while (!A_LOAD(H.quit) && (n = fread(buf, 1, sizeof(buf), f)) > 0)
      md5_append(&md5, buf, (int)n);
   bad = ferror(f) || A_LOAD(H.quit);
   fclose(f);
   if (bad)
   {
      fail(-1, "read error");
      return;
   }
   md5_finish(&md5, d);
   for (i = 0; i < 16; i++)
      snprintf(H.res.note + i * 2, 3, "%02x", d[i]);
   H.res.status = 0;
}

/* ---- the thread ------------------------------------------------------------- */

static void serve(void)
{
   H.res.body = NULL;
   H.res.len = 0;
   H.res.note[0] = '\0';
   if (H.hash)
      hash_file();
   else if (H.online && !H.online())
      fail(RA_HTTP_RETRY, "offline");
   else if (!strncmp(H.url, "http://", 7))
      http_plain();
   else if (!strncmp(H.url, "https://", 8))
   {
#ifdef __vita__
      http_tls();
#else
      fail(-1, "no tls on this build");
#endif
   }
   else
      fail(-1, "bad url");
}

static void thread_main(void *arg)
{
   (void)arg;
   while (!A_LOAD(H.quit))
   {
      if (A_LOAD(H.state) != ST_ASKED)
      {
         plat_thread_sleep_us(10000);
         continue;
      }
      serve();
      if (H.post)
      {
         volatile char *v = H.post;  /* it may have been a password */
         while (*v)
            *v++ = 0;
      }
      A_STORE(H.state, ST_DONE);
   }
}

static int submit(const char *url, const char *post, const char *type,
                  int hash);

int ra_http_submit(const char *url, const char *post, const char *type)
{
   return submit(url, post, type, 0);
}

int ra_http_hash_file(const char *path)
{
   return submit(path, NULL, NULL, 1);
}

static int submit(const char *url, const char *post, const char *type,
                  int hash)
{
   if (A_LOAD(H.state) != ST_EMPTY || !url)
      return -1;
   if (!H.running)
   {
      A_STORE(H.quit, 0);
      if (plat_net_start(thread_main, NULL) != 0)
         return -1;
      H.running = 1;
   }
   free(H.url);
   if (H.post)
   {
      volatile char *v = H.post;   /* the last one may have been a password */
      while (*v)
         *v++ = 0;
   }
   free(H.post);
   free(H.type);
   H.url = strdup(url);
   H.post = post ? strdup(post) : NULL;
   H.type = type ? strdup(type) : NULL;
   if (!H.url || (post && !H.post) || (type && !H.type))
      return -1;
   H.hash = hash;
   A_STORE(H.state, ST_ASKED);
   return 0;
}

int ra_http_take(ra_http_res *res)
{
   if (A_LOAD(H.state) != ST_DONE)
      return 0;
   *res = H.res;
   H.res.body = NULL;
   H.res.len = 0;
   A_STORE(H.state, ST_EMPTY);
   return 1;
}

int ra_http_idle(void)
{
   return A_LOAD(H.state) == ST_EMPTY;
}

void ra_http_stop(uint32_t timeout_us)
{
   if (!H.running)
      return;
   A_STORE(H.quit, 1);
   if (plat_net_join(timeout_us))
   {
      H.running = 0;
      /* An answer nobody took. */
      if (A_LOAD(H.state) == ST_DONE)
      {
         free(H.res.body);
         H.res.body = NULL;
      }
      A_STORE(H.state, ST_EMPTY);
   }
}
