/* upnp_igd.c -- see upnp_igd.h.  Original code for this project. */
#include "upnp_igd.h"
#include "nd_thread.h"            /* nd_now_us */

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef VITA
#include <fcntl.h>
#endif

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0            /* vitasdk: no SIGPIPE to suppress */
#endif

#define IGD_RSP_MAX      (96 * 1024)   /* a description is 3-30 KiB */
#define IGD_SSDP_MS      3000          /* how long to look for a gateway */
#define IGD_SSDP_EVERY_MS 300
#define IGD_HTTP_MS      4000          /* per request */
#define IGD_LEASE_S      7200          /* 2 h; renewed at half-life */
#define IGD_PORT_TRIES   4             /* external port taken: try the next */

/* ---------------------------------------------------------------- strings -- */

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

/* First occurrence of `needle` in hay[0..len), ASCII case-insensitive. */
static const char *find_ci(const char *hay, size_t len, const char *needle)
{
   size_t n = strlen(needle), i, k;
   if (!n || len < n)
      return NULL;
   for (i = 0; i + n <= len; i++)
   {
      for (k = 0; k < n && lower(hay[i + k]) == lower(needle[k]); k++)
         ;
      if (k == n)
         return hay + i;
   }
   return NULL;
}

static const char *find_cs(const char *hay, size_t len, const char *needle)
{
   size_t n = strlen(needle), i;
   if (!n || len < n)
      return NULL;
   for (i = 0; i + n <= len; i++)
      if (hay[i] == needle[0] && !memcmp(hay + i, needle, n))
         return hay + i;
   return NULL;
}

/* Copy [b, e) into out without its surrounding blanks.  0 if it fits. */
static int copy_trim(char *out, size_t cap, const char *b, const char *e)
{
   while (b < e && (*b == ' ' || *b == '\t' || *b == '\r' || *b == '\n'))
      b++;
   while (e > b && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'))
      e--;
   if ((size_t)(e - b) >= cap)
      return -1;
   memcpy(out, b, (size_t)(e - b));
   out[e - b] = '\0';
   return 0;
}

/* The text of the first <tag>...</tag> in buf[0..len).  The opening tag may
 * carry attributes; a namespace prefix is not looked for (UPnP's device and
 * SOAP argument elements have none).  0 on success. */
static int xml_text(const char *buf, size_t len, const char *tag,
                    char *out, size_t cap)
{
   char open[64], close[64];
   const char *p = buf, *end = buf + len, *q, *e;
   size_t tl = strlen(tag);

   if (tl + 4 > sizeof(open))
      return -1;
   snprintf(open, sizeof(open), "<%s", tag);
   snprintf(close, sizeof(close), "</%s>", tag);
   while ((p = find_cs(p, (size_t)(end - p), open)) != NULL)
   {
      q = p + tl + 1;
      if (q < end && (*q == '>' || *q == ' ' || *q == '\t' || *q == '\r' || *q == '\n'))
      {
         while (q < end && *q != '>')
            q++;
         if (q >= end)
            return -1;
         q++;
         e = find_cs(q, (size_t)(end - q), close);
         if (!e)
            return -1;
         return copy_trim(out, cap, q, e);
      }
      p = q;                         /* "<tagSomethingElse": keep looking */
   }
   return -1;
}

/* "http://a.b.c.d[:port][/path]" -> address, port (80 by default), path.
 * Only IPv4 literals: a gateway names itself by address. */
static int url_parse(const char *url, uint32_t *ip_be, uint16_t *port,
                     char *path, size_t cap)
{
   char host[32];
   const char *p = url, *h;
   struct in_addr a;
   unsigned v = 80;
   size_t n;

   if (strncmp(p, "http://", 7) != 0 && strncmp(p, "HTTP://", 7) != 0)
      return -1;
   p += 7;
   h = p;
   while (*p && *p != ':' && *p != '/')
      p++;
   n = (size_t)(p - h);
   if (!n || n >= sizeof(host))
      return -1;
   memcpy(host, h, n);
   host[n] = '\0';
   if (inet_pton(AF_INET, host, &a) != 1)
      return -1;
   if (*p == ':')
   {
      int digits = 0;
      p++;
      v = 0;
      while (*p >= '0' && *p <= '9' && digits < 5)
      {
         v = v * 10 + (unsigned)(*p - '0');
         p++;
         digits++;
      }
      if (!digits || !v || v > 65535)
         return -1;
   }
   if (*p && *p != '/')
      return -1;
   if (snprintf(path, cap, "%s", *p ? p : "/") >= (int)cap)
      return -1;
   *ip_be = a.s_addr;
   *port = (uint16_t)v;
   return 0;
}

static int ip_is_private(uint32_t ip_be)
{
   uint32_t ip = ntohl(ip_be);
   return ip == 0 ||
          (ip >> 24) == 10 || (ip >> 24) == 127 ||
          (ip >> 20) == ((172u << 4) | 1) ||            /* 172.16/12 */
          (ip >> 16) == ((192u << 8) | 168) ||
          (ip >> 22) == ((100u << 2) | 1) ||            /* 100.64/10 CGNAT */
          (ip >> 16) == ((169u << 8) | 254);
}

/* ------------------------------------------------------------------- HTTP -- */

enum { H_IDLE = 0, H_SEND, H_RECV, H_DONE, H_FAIL };

typedef struct igd_http
{
   int      fd;
   int      phase;
   char    *req;
   size_t   req_len, req_sent;
   char    *rsp;
   size_t   rsp_len, rsp_cap;
   uint64_t deadline_us;
   /* after H_DONE */
   int      status;
   const char *body;
   size_t   body_len;
} igd_http;

static void http_reset(igd_http *h)
{
   if (h->fd >= 0)
      close(h->fd);
   h->fd = -1;
   free(h->req);
   h->req = NULL;
   h->phase = H_IDLE;
}

static void sock_nonblock(int fd)
{
#ifdef VITA
   int on = 1;
   setsockopt(fd, SOL_SOCKET, SO_NONBLOCK, &on, sizeof(on));
#else
   fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
#endif
}

/* Takes ownership of `req` (malloc'd).  0 if the request is under way. */
static int http_start(igd_http *h, uint32_t ip_be, uint16_t port, char *req,
                      unsigned timeout_ms)
{
   struct sockaddr_in a;

   http_reset(h);
   h->req = req;
   if (!req)
      return -1;
   h->req_len = strlen(req);
   h->req_sent = 0;
   h->rsp_len = 0;
   h->status = 0;
   h->fd = socket(AF_INET, SOCK_STREAM, 0);
   if (h->fd < 0)
   {
      h->phase = H_FAIL;
      return -1;
   }
   sock_nonblock(h->fd);
   memset(&a, 0, sizeof(a));
   a.sin_family = AF_INET;
   a.sin_addr.s_addr = ip_be;
   a.sin_port = htons(port);
   /* Non-blocking: "in progress" is the normal answer.  We do not wait for
    * the connection with select/poll; the send below simply keeps failing
    * with a would-block class of error until the socket is connected. */
   if (connect(h->fd, (struct sockaddr *)&a, sizeof(a)) != 0 &&
       errno != EINPROGRESS && errno != EWOULDBLOCK && errno != EAGAIN &&
       errno != EINTR)
   {
      h->phase = H_FAIL;
      return -1;
   }
   h->deadline_us = nd_now_us() + (uint64_t)timeout_ms * 1000ull;
   h->phase = H_SEND;
   return 0;
}

/* In place: turn a chunked body into a plain one.  Returns the new length. */
static size_t dechunk(char *b, size_t len)
{
   size_t in = 0, out = 0;
   while (in < len)
   {
      size_t n = 0;
      int digits = 0;
      while (in < len)
      {
         char c = b[in];
         unsigned d;
         if (c >= '0' && c <= '9') d = (unsigned)(c - '0');
         else if (c >= 'a' && c <= 'f') d = (unsigned)(c - 'a' + 10);
         else if (c >= 'A' && c <= 'F') d = (unsigned)(c - 'A' + 10);
         else break;
         n = n * 16 + d;
         in++;
         digits++;
      }
      if (!digits)
         break;
      while (in < len && b[in] != '\n')      /* chunk extensions, CR */
         in++;
      in++;
      if (!n || in >= len)
         break;
      if (n > len - in)
         n = len - in;
      memmove(b + out, b + in, n);
      out += n;
      in += n;
      while (in < len && (b[in] == '\r' || b[in] == '\n'))
         in++;
   }
   return out;
}

/* Is the response complete without the peer closing?  Looks at the headers. */
static int http_complete(const igd_http *h)
{
   const char *hdr_end = find_cs(h->rsp, h->rsp_len, "\r\n\r\n");
   const char *cl;
   size_t hdr_len, body;
   if (!hdr_end)
      return 0;
   hdr_len = (size_t)(hdr_end - h->rsp) + 4;
   body = h->rsp_len - hdr_len;
   cl = find_ci(h->rsp, hdr_len, "\r\nContent-Length:");
   if (cl)
      return body >= (size_t)strtoul(cl + 17, NULL, 10);
   if (find_ci(h->rsp, hdr_len, "chunked"))
      return body >= 5 && !memcmp(h->rsp + h->rsp_len - 5, "0\r\n\r\n", 5);
   return 0;                                 /* only a close ends it */
}

static void http_finish(igd_http *h)
{
   char *hdr_end;
   size_t hdr_len;

   close(h->fd);
   h->fd = -1;
   h->rsp[h->rsp_len] = '\0';
   hdr_end = (char *)find_cs(h->rsp, h->rsp_len, "\r\n\r\n");
   if (h->rsp_len < 12 || strncmp(h->rsp, "HTTP/1.", 7) != 0 || !hdr_end)
   {
      h->phase = H_FAIL;
      return;
   }
   h->status = atoi(h->rsp + 9);
   hdr_len = (size_t)(hdr_end - h->rsp) + 4;
   h->body = h->rsp + hdr_len;
   h->body_len = h->rsp_len - hdr_len;
   if (find_ci(h->rsp, hdr_len, "chunked"))
   {
      h->body_len = dechunk(h->rsp + hdr_len, h->body_len);
      h->rsp[hdr_len + h->body_len] = '\0';
   }
   h->phase = H_DONE;
}

/* Advance.  Returns the phase. */
static int http_poll(igd_http *h, uint64_t now)
{
   if (h->phase != H_SEND && h->phase != H_RECV)
      return h->phase;
   if (now > h->deadline_us)
   {
      close(h->fd);
      h->fd = -1;
      h->phase = H_FAIL;
      return h->phase;
   }
   if (h->phase == H_SEND)
   {
      ssize_t n = send(h->fd, h->req + h->req_sent, h->req_len - h->req_sent,
                       MSG_DONTWAIT | MSG_NOSIGNAL);
      if (n > 0)
         h->req_sent += (size_t)n;
      else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
               errno != ENOTCONN && errno != EINPROGRESS && errno != EALREADY &&
               errno != EINTR)
      {
         close(h->fd);                       /* refused, unreachable, reset */
         h->fd = -1;
         h->phase = H_FAIL;
         return h->phase;
      }
      if (h->req_sent < h->req_len)
         return h->phase;
      h->phase = H_RECV;
   }
   for (;;)
   {
      ssize_t n;
      if (h->rsp_len + 2048 > h->rsp_cap)
      {
         size_t cap = h->rsp_cap ? h->rsp_cap * 2 : 8192;
         char *p;
         if (cap > IGD_RSP_MAX)
            cap = IGD_RSP_MAX;
         if (h->rsp_len + 2 > cap || !(p = (char *)realloc(h->rsp, cap)))
         {
            close(h->fd);                    /* absurdly large: not a router */
            h->fd = -1;
            h->phase = H_FAIL;
            return h->phase;
         }
         h->rsp = p;
         h->rsp_cap = cap;
      }
      n = recv(h->fd, h->rsp + h->rsp_len, h->rsp_cap - h->rsp_len - 1,
               MSG_DONTWAIT);
      if (n > 0)
      {
         h->rsp_len += (size_t)n;
         continue;
      }
      if (n == 0 || (h->rsp_len && http_complete(h)))
      {
         http_finish(h);
         return h->phase;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
         return h->phase;                    /* more next frame */
      if (h->rsp_len)
      {
         http_finish(h);                     /* reset after the answer */
         return h->phase;
      }
      close(h->fd);
      h->fd = -1;
      h->phase = H_FAIL;
      return h->phase;
   }
}

/* -------------------------------------------------------------------- IGD -- */

struct upnp_igd
{
   int      state;
   char     err[80];
   unsigned ssdp_ms, http_ms;

   int      ssdp_fd;
   struct sockaddr_in ssdp_to;
   int      ssdp_unicast;          /* test target: no multicast */
   uint64_t ssdp_start_us, ssdp_last_us;
   int      ssdp_round;

   uint32_t local_ip;
   uint16_t int_port, ext_port_req, ext_port_try, ext_port;
   int      map_tries, lease;
   char     desc[32];

   uint32_t ctl_ip;
   uint16_t ctl_port;
   char     ctl_path[256];
   char     service[64];           /* the service type in use */
   char     ext_ip[16];
   uint64_t renew_at_us;
   int      renewing;

   igd_http h;
};

static void igd_fail(upnp_igd *u, const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(u->err, sizeof(u->err), fmt, ap);
   va_end(ap);
   http_reset(&u->h);
   u->state = UPNP_IGD_FAILED;
}

static char *fmt_alloc(const char *fmt, ...)
{
   va_list ap;
   char *s;
   int n;
   va_start(ap, fmt);
   n = vsnprintf(NULL, 0, fmt, ap);
   va_end(ap);
   if (n < 0 || !(s = (char *)malloc((size_t)n + 1)))
      return NULL;
   va_start(ap, fmt);
   vsnprintf(s, (size_t)n + 1, fmt, ap);
   va_end(ap);
   return s;
}

static char *soap_request(const upnp_igd *u, const char *action,
                          const char *args)
{
   char host[32];
   char *body, *req;
   const uint8_t *b = (const uint8_t *)&u->ctl_ip;

   snprintf(host, sizeof(host), "%u.%u.%u.%u:%u", b[0], b[1], b[2], b[3],
            (unsigned)u->ctl_port);
   body = fmt_alloc(
      "<?xml version=\"1.0\"?>\r\n"
      "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
      "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
      "<s:Body><u:%s xmlns:u=\"%s\">%s</u:%s></s:Body></s:Envelope>\r\n",
      action, u->service, args, action);
   if (!body)
      return NULL;
   req = fmt_alloc(
      "POST %s HTTP/1.1\r\n"
      "Host: %s\r\n"
      "User-Agent: GBAdhoc/3.5 UPnP/1.1\r\n"
      "Content-Type: text/xml; charset=\"utf-8\"\r\n"
      "SOAPAction: \"%s#%s\"\r\n"
      "Connection: close\r\n"
      "Content-Length: %u\r\n\r\n%s",
      u->ctl_path, host, u->service, action, (unsigned)strlen(body), body);
   free(body);
   return req;
}

static int start_add_mapping(upnp_igd *u)
{
   const uint8_t *b = (const uint8_t *)&u->local_ip;
   char args[512];
   snprintf(args, sizeof(args),
            "<NewRemoteHost></NewRemoteHost>"
            "<NewExternalPort>%u</NewExternalPort>"
            "<NewProtocol>UDP</NewProtocol>"
            "<NewInternalPort>%u</NewInternalPort>"
            "<NewInternalClient>%u.%u.%u.%u</NewInternalClient>"
            "<NewEnabled>1</NewEnabled>"
            "<NewPortMappingDescription>%s</NewPortMappingDescription>"
            "<NewLeaseDuration>%d</NewLeaseDuration>",
            (unsigned)u->ext_port_try, (unsigned)u->int_port, b[0], b[1], b[2],
            b[3], u->desc, u->lease);
   return http_start(&u->h, u->ctl_ip, u->ctl_port,
                     soap_request(u, "AddPortMapping", args), u->http_ms);
}

/* The description: which service, and where its control URL points. */
static int igd_parse_description(upnp_igd *u, const char *xml, size_t len,
                                 uint32_t base_ip, uint16_t base_port)
{
   static const char *const TYPES[] = {
      "urn:schemas-upnp-org:service:WANIPConnection:2",
      "urn:schemas-upnp-org:service:WANIPConnection:1",
      "urn:schemas-upnp-org:service:WANPPPConnection:1",
   };
   char url[300], base[300], dummy[256];
   unsigned k;

   /* UPnP 1.0 devices may give a base for relative URLs. */
   if (xml_text(xml, len, "URLBase", base, sizeof(base)) == 0)
   {
      uint32_t ip;
      uint16_t port;
      if (url_parse(base, &ip, &port, dummy, sizeof(dummy)) == 0)
      {
         base_ip = ip;
         base_port = port;
      }
   }
   for (k = 0; k < sizeof(TYPES) / sizeof(TYPES[0]); k++)
   {
      const char *t = find_cs(xml, len, TYPES[k]), *b, *e, *p;
      if (!t)
         continue;
      /* the <service> element this type sits in */
      b = xml;
      for (p = xml; (p = find_cs(p, (size_t)(t - p), "<service>")) != NULL; p += 9)
         b = p;
      e = find_cs(t, len - (size_t)(t - xml), "</service>");
      if (!e)
         continue;
      if (xml_text(b, (size_t)(e - b), "controlURL", url, sizeof(url)) != 0 ||
          !url[0])
         continue;
      snprintf(u->service, sizeof(u->service), "%s", TYPES[k]);
      if (!strncmp(url, "http://", 7) || !strncmp(url, "HTTP://", 7))
         return url_parse(url, &u->ctl_ip, &u->ctl_port, u->ctl_path,
                          sizeof(u->ctl_path));
      u->ctl_ip = base_ip;
      u->ctl_port = base_port;
      if (snprintf(u->ctl_path, sizeof(u->ctl_path), "%s%s",
                   url[0] == '/' ? "" : "/", url) >= (int)sizeof(u->ctl_path))
         return -1;
      return 0;
   }
   return -1;
}

static void ssdp_send(upnp_igd *u)
{
   static const char *const ST[] = {
      "urn:schemas-upnp-org:device:InternetGatewayDevice:1",
      "urn:schemas-upnp-org:device:InternetGatewayDevice:2",
      "urn:schemas-upnp-org:service:WANIPConnection:1",
   };
   char msg[256];
   int n = snprintf(msg, sizeof(msg),
                    "M-SEARCH * HTTP/1.1\r\n"
                    "HOST: 239.255.255.250:1900\r\n"
                    "MAN: \"ssdp:discover\"\r\n"
                    "MX: 1\r\n"
                    "ST: %s\r\n\r\n", ST[u->ssdp_round % 3]);
   sendto(u->ssdp_fd, msg, (size_t)n, MSG_DONTWAIT,
          (struct sockaddr *)&u->ssdp_to, sizeof(u->ssdp_to));
   u->ssdp_round++;
}

/* One SSDP answer.  1 if it names a gateway and its description is being
 * fetched. */
static int ssdp_take(upnp_igd *u, const char *msg, size_t len,
                     const struct sockaddr_in *from)
{
   char url[300], path[256], *req;
   const char *loc, *eol;
   uint32_t ip;
   uint16_t port;
   const uint8_t *b;

   if (len < 12 || strncmp(msg, "HTTP/1.1 200", 12) != 0)
      return 0;
   if (!find_cs(msg, len, "InternetGatewayDevice") &&
       !find_cs(msg, len, "WANIPConnection") &&
       !find_cs(msg, len, "WANPPPConnection"))
      return 0;                              /* some other UPnP device */
   loc = find_ci(msg, len, "\r\nLOCATION:");
   if (!loc)
      return 0;
   loc += 11;
   eol = find_cs(loc, len - (size_t)(loc - msg), "\r\n");
   if (!eol || copy_trim(url, sizeof(url), loc, eol) != 0 ||
       url_parse(url, &ip, &port, path, sizeof(path)) != 0)
      return 0;
   /* The description must live on the device that answered: an answer that
    * points somewhere else is not a gateway talking about itself. */
   if (ip != from->sin_addr.s_addr)
      return 0;
   u->ctl_ip = ip;                           /* the base, until parsed */
   u->ctl_port = port;
   b = (const uint8_t *)&ip;
   req = fmt_alloc("GET %s HTTP/1.1\r\n"
                   "Host: %u.%u.%u.%u:%u\r\n"
                   "User-Agent: GBAdhoc/3.5 UPnP/1.1\r\n"
                   "Connection: close\r\n\r\n",
                   path, b[0], b[1], b[2], b[3], (unsigned)port);
   if (http_start(&u->h, ip, port, req, u->http_ms) != 0)
      return 0;
   u->state = UPNP_IGD_DESCRIBING;
   return 1;
}

upnp_igd *upnp_igd_open(uint32_t local_ip_be, uint16_t internal_port,
                        uint16_t external_port, const char *description)
{
   upnp_igd *u = (upnp_igd *)calloc(1, sizeof(*u));
   struct sockaddr_in a;
   size_t i;
   if (!u)
      return NULL;
   u->h.fd = -1;
   u->ssdp_ms = IGD_SSDP_MS;
   u->http_ms = IGD_HTTP_MS;
   u->local_ip = local_ip_be;
   u->int_port = internal_port;
   u->ext_port_req = u->ext_port_try = external_port ? external_port
                                                     : internal_port;
   u->lease = IGD_LEASE_S;
   /* It goes into XML and onto a router's status page: plain text only. */
   snprintf(u->desc, sizeof(u->desc), "%s", description && description[0]
                                               ? description : "GBAdhoc");
   for (i = 0; u->desc[i]; i++)
   {
      char c = u->desc[i];
      if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == ' ' || c == '-' || c == '_' ||
            c == '.'))
         u->desc[i] = '_';
   }
   u->ssdp_fd = socket(AF_INET, SOCK_DGRAM, 0);
   if (u->ssdp_fd < 0)
   {
      free(u);
      return NULL;
   }
   sock_nonblock(u->ssdp_fd);
   memset(&a, 0, sizeof(a));
   a.sin_family = AF_INET;
   a.sin_addr.s_addr = htonl(INADDR_ANY);
   bind(u->ssdp_fd, (struct sockaddr *)&a, sizeof(a));
   memset(&u->ssdp_to, 0, sizeof(u->ssdp_to));
   u->ssdp_to.sin_family = AF_INET;
   u->ssdp_to.sin_addr.s_addr = inet_addr("239.255.255.250");
   u->ssdp_to.sin_port = htons(1900);
   u->state = UPNP_IGD_SEARCHING;
   return u;
}

void upnp_igd_test_target(upnp_igd *u, uint32_t ssdp_ip_be, uint16_t ssdp_port,
                          unsigned timeout_ms)
{
   if (!u)
      return;
   if (ssdp_ip_be)
   {
      u->ssdp_to.sin_addr.s_addr = ssdp_ip_be;
      u->ssdp_to.sin_port = htons(ssdp_port);
      u->ssdp_unicast = 1;
   }
   if (timeout_ms)
      u->ssdp_ms = u->http_ms = timeout_ms;
}

void upnp_igd_close(upnp_igd *u)
{
   if (!u)
      return;
   http_reset(&u->h);
   free(u->h.rsp);
   if (u->ssdp_fd >= 0)
      close(u->ssdp_fd);
   free(u);
}

int upnp_igd_state(const upnp_igd *u) { return u ? u->state : UPNP_IGD_FAILED; }
const char *upnp_igd_external_ip(const upnp_igd *u) { return u ? u->ext_ip : ""; }
uint16_t upnp_igd_external_port(const upnp_igd *u) { return u ? u->ext_port : 0; }
const char *upnp_igd_error(const upnp_igd *u) { return u ? u->err : "out of memory"; }

int upnp_igd_external_is_private(const upnp_igd *u)
{
   struct in_addr a;
   if (!u || !u->ext_ip[0] || inet_pton(AF_INET, u->ext_ip, &a) != 1)
      return 0;
   return ip_is_private(a.s_addr);
}

void upnp_igd_remove(upnp_igd *u)
{
   char args[160];
   if (!u || u->state != UPNP_IGD_MAPPED)
      return;
   snprintf(args, sizeof(args),
            "<NewRemoteHost></NewRemoteHost>"
            "<NewExternalPort>%u</NewExternalPort>"
            "<NewProtocol>UDP</NewProtocol>", (unsigned)u->ext_port);
   u->renewing = 0;
   if (http_start(&u->h, u->ctl_ip, u->ctl_port,
                  soap_request(u, "DeletePortMapping", args), u->http_ms) != 0)
   {
      igd_fail(u, "could not reach the router to close the port");
      return;
   }
   u->state = UPNP_IGD_REMOVING;
}

int upnp_igd_poll(upnp_igd *u)
{
   uint64_t now;
   char val[64];

   if (!u)
      return UPNP_IGD_FAILED;
   now = nd_now_us();

   switch (u->state)
   {
   case UPNP_IGD_SEARCHING:
   {
      char buf[1536];
      int budget;
      if (!u->ssdp_start_us)
         u->ssdp_start_us = now;
      if (!u->ssdp_last_us ||
          now - u->ssdp_last_us >= IGD_SSDP_EVERY_MS * 1000ull)
      {
         ssdp_send(u);
         u->ssdp_last_us = now;
      }
      for (budget = 0; budget < 16; budget++)
      {
         struct sockaddr_in from;
         socklen_t flen = sizeof(from);
         ssize_t n = recvfrom(u->ssdp_fd, buf, sizeof(buf) - 1, MSG_DONTWAIT,
                              (struct sockaddr *)&from, &flen);
         if (n <= 0)
            break;
         buf[n] = '\0';
         if (ssdp_take(u, buf, (size_t)n, &from))
            return u->state;
      }
      if (now - u->ssdp_start_us > (uint64_t)u->ssdp_ms * 1000ull)
         igd_fail(u, "no UPnP router answered");
      break;
   }

   case UPNP_IGD_DESCRIBING:
      if (http_poll(&u->h, now) == H_FAIL)
         igd_fail(u, "the router's description could not be read");
      else if (u->h.phase == H_DONE)
      {
         if (u->h.status != 200 ||
             igd_parse_description(u, u->h.body, u->h.body_len, u->ctl_ip,
                                   u->ctl_port) != 0)
            igd_fail(u, "the router offers no port mapping service");
         else if (http_start(&u->h, u->ctl_ip, u->ctl_port,
                             soap_request(u, "GetExternalIPAddress", ""),
                             u->http_ms) != 0)
            igd_fail(u, "could not reach the router");
         else
            u->state = UPNP_IGD_ASKING_IP;
      }
      break;

   case UPNP_IGD_ASKING_IP:
      if (http_poll(&u->h, now) == H_SEND || u->h.phase == H_RECV)
         break;
      /* The address is for the screen; a router that will not tell it can
       * still map the port, so a failure here is not the end. */
      if (u->h.phase == H_DONE && u->h.status == 200 &&
          xml_text(u->h.body, u->h.body_len, "NewExternalIPAddress", val,
                   sizeof(val)) == 0)
      {
         struct in_addr a;
         if (inet_pton(AF_INET, val, &a) == 1 && strlen(val) < sizeof(u->ext_ip))
            snprintf(u->ext_ip, sizeof(u->ext_ip), "%s", val);
      }
      if (start_add_mapping(u) != 0)
         igd_fail(u, "could not reach the router");
      else
         u->state = UPNP_IGD_MAPPING;
      break;

   case UPNP_IGD_MAPPING:
      if (http_poll(&u->h, now) == H_FAIL)
         igd_fail(u, "the router did not answer the mapping request");
      else if (u->h.phase == H_DONE)
      {
         int code = 0;
         if (u->h.status == 200)
         {
            u->ext_port = u->ext_port_try;
            u->renew_at_us = now + (uint64_t)(u->lease ? u->lease / 2 : 0) *
                                   1000000ull;
            http_reset(&u->h);
            u->state = UPNP_IGD_MAPPED;
            break;
         }
         if (xml_text(u->h.body, u->h.body_len, "errorCode", val,
                      sizeof(val)) == 0)
            code = atoi(val);
         if (code == 725 && u->lease)
         {
            u->lease = 0;                    /* only permanent leases */
            if (start_add_mapping(u) != 0)
               igd_fail(u, "could not reach the router");
         }
         else if (code == 718 && ++u->map_tries < IGD_PORT_TRIES &&
                  u->ext_port_try < 65535)
         {
            u->ext_port_try++;               /* taken by another device */
            if (start_add_mapping(u) != 0)
               igd_fail(u, "could not reach the router");
         }
         else if (code == 718)
            igd_fail(u, "ports %u-%u are already forwarded elsewhere",
                     (unsigned)u->ext_port_req, (unsigned)u->ext_port_try);
         else if (code == 606)
            igd_fail(u, "the router does not allow this console to open ports");
         else
            igd_fail(u, "the router refused the port (error %d)",
                     code ? code : u->h.status);
      }
      break;

   case UPNP_IGD_MAPPED:
      /* A leased mapping is renewed at half its life; a failed renewal is
       * tried again later and never ends the session. */
      if (u->renewing)
      {
         if (http_poll(&u->h, now) != H_SEND && u->h.phase != H_RECV)
         {
            http_reset(&u->h);
            u->renewing = 0;
            u->renew_at_us = now + (uint64_t)(u->lease / 2) * 1000000ull;
         }
      }
      else if (u->lease && now >= u->renew_at_us)
      {
         u->ext_port_try = u->ext_port;
         if (start_add_mapping(u) == 0)
            u->renewing = 1;
         else
            u->renew_at_us = now + 60ull * 1000000ull;
      }
      break;

   case UPNP_IGD_REMOVING:
      if (http_poll(&u->h, now) != H_SEND && u->h.phase != H_RECV)
      {
         /* Gone, or it never knew it (714 NoSuchEntry): closed either way.
          * Only silence is a failure. */
         if (u->h.phase == H_DONE)
         {
            http_reset(&u->h);
            u->ext_port = 0;
            u->state = UPNP_IGD_REMOVED;
         }
         else
            igd_fail(u, "the router did not confirm closing the port");
      }
      break;

   default:
      break;
   }
   return u->state;
}

#ifdef UPNP_IGD_TEST
/* The parsers, for netdrv/tests/test_igd.c. */
int upnp_igd_test_url(const char *url, uint32_t *ip, uint16_t *port, char *path,
                      size_t cap)
{
   return url_parse(url, ip, port, path, cap);
}
int upnp_igd_test_xml(const char *buf, const char *tag, char *out, size_t cap)
{
   return xml_text(buf, strlen(buf), tag, out, cap);
}
int upnp_igd_test_desc(const char *xml, uint32_t base_ip, uint16_t base_port,
                       uint32_t *ip, uint16_t *port, char *path, size_t pcap,
                       char *service, size_t scap)
{
   upnp_igd u;
   memset(&u, 0, sizeof(u));
   if (igd_parse_description(&u, xml, strlen(xml), base_ip, base_port) != 0)
      return -1;
   *ip = u.ctl_ip;
   *port = u.ctl_port;
   snprintf(path, pcap, "%s", u.ctl_path);
   snprintf(service, scap, "%s", u.service);
   return 0;
}
size_t upnp_igd_test_dechunk(char *b, size_t len) { return dechunk(b, len); }
int upnp_igd_test_private(uint32_t ip_be) { return ip_is_private(ip_be); }
#endif
