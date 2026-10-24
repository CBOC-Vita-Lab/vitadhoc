/* test_igd.c -- upnp_igd.c against a pretend router (tests/fake_igd.py,
 * started as a child process per scenario) and its parsers against text.
 *
 *   parsers   URLs, XML text search, the description walk, chunked bodies,
 *             private-range detection
 *   plain     search -> describe -> external IP -> map -> remove
 *   ...       one scenario per way a router can differ or refuse
 *   multicast the real SSDP group, if this machine delivers it (else SKIPPED)
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <arpa/inet.h>

#include "nd_thread.h"
#include "upnp_igd.h"

int upnp_igd_test_url(const char *, uint32_t *, uint16_t *, char *, size_t);
int upnp_igd_test_xml(const char *, const char *, char *, size_t);
int upnp_igd_test_desc(const char *, uint32_t, uint16_t, uint32_t *, uint16_t *,
                       char *, size_t, char *, size_t);
size_t upnp_igd_test_dechunk(char *, size_t);
int upnp_igd_test_private(uint32_t);

static int g_fail;
#define CHECK(cond, ...) do { \
   if (!(cond)) { \
      g_fail++; \
      printf("  FAIL %s:%d: %s -- ", __FILE__, __LINE__, #cond); \
      printf(__VA_ARGS__); \
      printf("\n"); \
   } } while (0)

static const char *g_fake = "tests/fake_igd.py";

/* ----------------------------------------------------------------- parsers -- */

static void test_parsers(void)
{
   uint32_t ip = 0;
   uint16_t port = 0;
   char path[128], svc[80], out[64], chunked[128];
   size_t n;

   printf("[parsers]\n");
   CHECK(upnp_igd_test_url("http://192.168.1.1:5000/rootDesc.xml", &ip, &port,
                           path, sizeof(path)) == 0 &&
         ip == inet_addr("192.168.1.1") && port == 5000 &&
         !strcmp(path, "/rootDesc.xml"), "url with port");
   CHECK(upnp_igd_test_url("http://10.0.0.138/", &ip, &port, path,
                           sizeof(path)) == 0 && port == 80 && !strcmp(path, "/"),
         "default port");
   CHECK(upnp_igd_test_url("http://10.0.0.138", &ip, &port, path,
                           sizeof(path)) == 0 && !strcmp(path, "/"), "no path");
   CHECK(upnp_igd_test_url("https://10.0.0.1/x", &ip, &port, path,
                           sizeof(path)) != 0, "https refused");
   CHECK(upnp_igd_test_url("http://router.lan/x", &ip, &port, path,
                           sizeof(path)) != 0, "a name refused");
   CHECK(upnp_igd_test_url("http://10.0.0.1:0/x", &ip, &port, path,
                           sizeof(path)) != 0, "port 0 refused");
   CHECK(upnp_igd_test_url("http://10.0.0.1:99999/x", &ip, &port, path,
                           sizeof(path)) != 0, "port range");
   CHECK(upnp_igd_test_url("http://10.0.0.1:80x/", &ip, &port, path,
                           sizeof(path)) != 0, "junk after the port");

   CHECK(upnp_igd_test_xml("<a><NewExternalIPAddress>1.2.3.4</NewExternalIPAddress></a>",
                           "NewExternalIPAddress", out, sizeof(out)) == 0 &&
         !strcmp(out, "1.2.3.4"), "plain element");
   CHECK(upnp_igd_test_xml("<x>\n <errorCode >\n  718 </errorCode></x>",
                           "errorCode", out, sizeof(out)) == 0 &&
         !strcmp(out, "718"), "blanks and a spaced tag: '%s'", out);
   CHECK(upnp_igd_test_xml("<controlURLish>/no</controlURLish><controlURL a=\"b\">/yes</controlURL>",
                           "controlURL", out, sizeof(out)) == 0 &&
         !strcmp(out, "/yes"), "a longer tag name is not a match: '%s'", out);
   CHECK(upnp_igd_test_xml("<controlURL>/unterminated", "controlURL", out,
                           sizeof(out)) != 0, "unterminated element");
   CHECK(upnp_igd_test_xml("<t>0123456789</t>", "t", out, 5) != 0,
         "a value that does not fit is refused, not truncated");

   CHECK(upnp_igd_test_desc(
            "<root><device><serviceList><service>"
            "<serviceType>urn:schemas-upnp-org:service:Layer3Forwarding:1</serviceType>"
            "<controlURL>/l3f</controlURL></service></serviceList>"
            "<service><controlURL>/before</controlURL>"
            "<serviceType>urn:schemas-upnp-org:service:WANIPConnection:1</serviceType>"
            "</service><service>"
            "<serviceType>urn:schemas-upnp-org:service:WANPPPConnection:1</serviceType>"
            "<controlURL>/ppp</controlURL></service></device></root>",
            inet_addr("192.168.0.1"), 49152, &ip, &port, path, sizeof(path),
            svc, sizeof(svc)) == 0 &&
         !strcmp(path, "/before") && port == 49152 &&
         ip == inet_addr("192.168.0.1") &&
         !strcmp(svc, "urn:schemas-upnp-org:service:WANIPConnection:1"),
         "the control URL of the right <service>, whatever the order: %s", path);
   CHECK(upnp_igd_test_desc(
            "<root><URLBase>http://192.168.0.254:8080/</URLBase><service>"
            "<serviceType>urn:schemas-upnp-org:service:WANIPConnection:2</serviceType>"
            "<controlURL>upnp/control/WANIPConn1</controlURL></service></root>",
            inet_addr("192.168.0.1"), 49152, &ip, &port, path, sizeof(path),
            svc, sizeof(svc)) == 0 &&
         ip == inet_addr("192.168.0.254") && port == 8080 &&
         !strcmp(path, "/upnp/control/WANIPConn1"),
         "URLBase + relative control URL: %s", path);
   CHECK(upnp_igd_test_desc(
            "<root><service><serviceType>urn:schemas-upnp-org:service:"
            "WANCommonInterfaceConfig:1</serviceType><controlURL>/c</controlURL>"
            "</service></root>", 1, 1, &ip, &port, path, sizeof(path), svc,
            sizeof(svc)) != 0, "no WAN connection service");

   strcpy(chunked, "5\r\nhello\r\n3;ext=1\r\n, w\r\n4\r\norld\r\n0\r\n\r\n");
   n = upnp_igd_test_dechunk(chunked, strlen(chunked));
   CHECK(n == 12 && !memcmp(chunked, "hello, world", 12), "dechunk (%u)",
         (unsigned)n);

   CHECK(upnp_igd_test_private(inet_addr("10.1.2.3")) &&
         upnp_igd_test_private(inet_addr("172.16.0.1")) &&
         upnp_igd_test_private(inet_addr("172.31.255.1")) &&
         upnp_igd_test_private(inet_addr("192.168.1.1")) &&
         upnp_igd_test_private(inet_addr("100.64.0.1")) &&
         upnp_igd_test_private(inet_addr("100.127.255.1")) &&
         upnp_igd_test_private(inet_addr("169.254.1.1")), "private ranges");
   CHECK(!upnp_igd_test_private(inet_addr("203.0.113.7")) &&
         !upnp_igd_test_private(inet_addr("172.32.0.1")) &&
         !upnp_igd_test_private(inet_addr("100.128.0.1")) &&
         !upnp_igd_test_private(inet_addr("11.0.0.1")), "public addresses");
}

/* ------------------------------------------------------------- the router -- */

typedef struct router
{
   pid_t pid;
   int   ssdp_port, http_port;
   char  log[128];
} router;

static int router_start(router *r, const char *const *extra)
{
   const char *argv[24];
   char line[128];
   int fds[2], n = 0, i;
   FILE *f;

   snprintf(r->log, sizeof(r->log), "/tmp/fake_igd.%d.log", (int)getpid());
   unlink(r->log);
   argv[n++] = "python3";
   argv[n++] = g_fake;
   argv[n++] = "--log";
   argv[n++] = r->log;
   for (i = 0; extra && extra[i] && n < 22; i++)
      argv[n++] = extra[i];
   argv[n] = NULL;
   if (pipe(fds) != 0)
      return -1;
   r->pid = fork();
   if (r->pid == 0)
   {
      dup2(fds[1], 1);
      close(fds[0]);
      close(fds[1]);
      execvp("python3", (char *const *)argv);
      _exit(127);
   }
   close(fds[1]);
   f = fdopen(fds[0], "r");
   r->ssdp_port = r->http_port = 0;
   if (!f || !fgets(line, sizeof(line), f) ||
       sscanf(line, "PORTS %d %d", &r->ssdp_port, &r->http_port) != 2)
   {
      if (f)
         fclose(f);
      return -1;
   }
   fclose(f);
   return 0;
}

static void router_stop(router *r)
{
   if (r->pid > 0)
   {
      kill(r->pid, SIGKILL);
      waitpid(r->pid, NULL, 0);
   }
   unlink(r->log);
}

/* Does the router's call log contain this text? */
static int logged(const router *r, const char *text)
{
   char line[512];
   FILE *f = fopen(r->log, "r");
   int hit = 0;
   if (!f)
      return 0;
   while (fgets(line, sizeof(line), f))
      if (strstr(line, text))
         hit++;
   fclose(f);
   return hit;
}

/* Poll like a main loop until the state leaves the busy ones; returns the
 * state and how many polls and how long the slowest poll took. */
static int run(upnp_igd *u, int until_a, int until_b, unsigned *polls,
               uint32_t *worst_us)
{
   uint64_t end = nd_now_us() + 15000000ull;
   int st = upnp_igd_state(u);
   *polls = 0;
   *worst_us = 0;
   while (nd_now_us() < end)
   {
      uint64_t t0 = nd_now_us();
      uint32_t d;
      st = upnp_igd_poll(u);
      d = (uint32_t)(nd_now_us() - t0);
      if (d > *worst_us)
         *worst_us = d;
      (*polls)++;
      if (st == until_a || st == until_b || st == UPNP_IGD_FAILED)
         break;
      nd_sleep_us(1000);
   }
   return st;
}

typedef struct scenario
{
   const char *name;
   const char *args[8];
   int         want;             /* UPNP_IGD_MAPPED or UPNP_IGD_FAILED */
   int         ext_port;         /* expected mapped port (0 = requested) */
   const char *ext_ip;           /* expected, NULL = 203.0.113.7 */
   const char *err;              /* substring of the error when FAILED */
   const char *log;              /* a line the router must have logged */
} scenario;

static void test_scenario(const scenario *s)
{
   enum { PORT = 19019 };
   router r;
   upnp_igd *u;
   unsigned polls;
   uint32_t worst;
   int st;
   char want_add[160];

   printf("[%s]\n", s->name);
   if (router_start(&r, s->args) != 0)
   {
      CHECK(0, "the fake router did not start (python3 %s)", g_fake);
      router_stop(&r);
      return;
   }
   u = upnp_igd_open(inet_addr("192.168.1.23"), PORT, PORT, "GBAdhoc <Room 7>");
   CHECK(u != NULL, "open");
   upnp_igd_test_target(u, inet_addr("127.0.0.1"), (uint16_t)r.ssdp_port, 1200);
   CHECK(upnp_igd_state(u) == UPNP_IGD_SEARCHING, "starts searching");
   CHECK(!upnp_igd_external_ip(u)[0] && !upnp_igd_external_port(u),
         "nothing known yet");

   st = run(u, UPNP_IGD_MAPPED, UPNP_IGD_MAPPED, &polls, &worst);
   CHECK(st == s->want, "ended in state %d, want %d (error '%s')", st, s->want,
         upnp_igd_error(u));
   /* "never blocks": the slowest single poll, in a loop that sleeps 1 ms
    * between polls.  Generous, for a loaded machine. */
   CHECK(worst < 50000, "slowest poll %u us", worst);
   printf("  state=%d polls=%u slowest=%uus ip='%s' port=%u err='%s'\n", st,
          polls, worst, upnp_igd_external_ip(u), upnp_igd_external_port(u),
          upnp_igd_error(u));

   if (s->want == UPNP_IGD_MAPPED && st == UPNP_IGD_MAPPED)
   {
      int port = s->ext_port ? s->ext_port : PORT;
      CHECK(upnp_igd_external_port(u) == port, "mapped port %u, want %d",
            upnp_igd_external_port(u), port);
      CHECK(!strcmp(upnp_igd_external_ip(u), s->ext_ip ? s->ext_ip : "203.0.113.7"),
            "external ip '%s'", upnp_igd_external_ip(u));
      snprintf(want_add, sizeof(want_add),
               "AddPortMapping proto=UDP ext=%d int=%d client=192.168.1.23", port,
               PORT);
      CHECK(logged(&r, want_add) >= 1, "the router mapped: %s", want_add);
      CHECK(logged(&r, "desc=GBAdhoc _Room 7_ enabled=1 remote=[]") >= 1,
            "description sanitised, enabled, any remote host");
      CHECK(!upnp_igd_error(u)[0], "no error text");
      /* a "public" address inside a carrier's NAT range must raise the flag */
      CHECK(upnp_igd_external_is_private(u) ==
            !strncmp(upnp_igd_external_ip(u), "100.72.", 7),
            "private-address warning for '%s'", upnp_igd_external_ip(u));
      if (s->log)
         CHECK(logged(&r, s->log) >= 1, "router log has '%s'", s->log);
      CHECK(logged(&r, "BAD") == 0, "no malformed request");

      /* a MAPPED client costs nothing to poll, and stays MAPPED */
      CHECK(upnp_igd_poll(u) == UPNP_IGD_MAPPED, "stays mapped");

      upnp_igd_remove(u);
      CHECK(upnp_igd_state(u) == UPNP_IGD_REMOVING, "removing");
      st = run(u, UPNP_IGD_REMOVED, UPNP_IGD_REMOVED, &polls, &worst);
      CHECK(st == UPNP_IGD_REMOVED, "removed (state %d, '%s')", st,
            upnp_igd_error(u));
      snprintf(want_add, sizeof(want_add), "DeletePortMapping ext=%d -> ok", port);
      CHECK(logged(&r, want_add) == 1, "the router unmapped: %s", want_add);
      CHECK(upnp_igd_external_port(u) == 0, "no port after removal");
   }
   else if (s->want == UPNP_IGD_FAILED)
   {
      CHECK(s->err && strstr(upnp_igd_error(u), s->err), "error '%s' has '%s'",
            upnp_igd_error(u), s->err ? s->err : "");
      CHECK(upnp_igd_external_port(u) == 0, "no port");
      if (s->log)
         CHECK(logged(&r, s->log) >= 1, "router log has '%s'", s->log);
      upnp_igd_remove(u);                 /* a no-op when not mapped */
      CHECK(upnp_igd_state(u) == UPNP_IGD_FAILED, "remove is a no-op");
   }
   upnp_igd_close(u);
   router_stop(&r);
}

/* The real thing: M-SEARCH to 239.255.255.250:1900. */
static void test_multicast(void)
{
   static const char *const args[] = { "--multicast", NULL };
   router r;
   upnp_igd *u;
   unsigned polls;
   uint32_t worst;
   int st;

   printf("[multicast: the real SSDP group on this machine]\n");
   if (router_start(&r, args) != 0)
   {
      printf("  SKIPPED: the fake router could not bind UDP 1900\n");
      router_stop(&r);
      return;
   }
   u = upnp_igd_open(inet_addr("192.168.1.23"), 19019, 19019, NULL);
   upnp_igd_test_target(u, 0, 0, 1500);   /* keep the multicast target */
   st = run(u, UPNP_IGD_MAPPED, UPNP_IGD_MAPPED, &polls, &worst);
   if (st == UPNP_IGD_FAILED && logged(&r, "M-SEARCH") == 0)
      printf("  SKIPPED: multicast is not delivered locally here ('%s')\n",
             upnp_igd_error(u));
   else
   {
      CHECK(st == UPNP_IGD_MAPPED, "mapped over multicast (state %d '%s')", st,
            upnp_igd_error(u));
      CHECK(logged(&r, "desc=GBAdhoc ") >= 1, "default description");
      printf("  state=%d ip='%s' port=%u\n", st, upnp_igd_external_ip(u),
             upnp_igd_external_port(u));
   }
   upnp_igd_close(u);
   router_stop(&r);
}

int main(int argc, char **argv)
{
   static const scenario S[] = {
      { "plain", { NULL }, UPNP_IGD_MAPPED, 0, NULL, NULL, "lease=7200" },
      { "another device answers SSDP first", { "--other-device", NULL },
        UPNP_IGD_MAPPED, 0, NULL, NULL, NULL },
      { "an SSDP answer pointing elsewhere is ignored", { "--wrong-location", NULL },
        UPNP_IGD_MAPPED, 0, NULL, NULL, NULL },
      { "chunked description, URLBase, relative control URL",
        { "--chunked", "--urlbase", "--relative", NULL },
        UPNP_IGD_MAPPED, 0, NULL, NULL, NULL },
      { "absolute control URL, responses ended by close, slow router",
        { "--absolute", "--no-length", "--slow", NULL },
        UPNP_IGD_MAPPED, 0, NULL, NULL, NULL },
      { "WANPPPConnection (DSL)", { "--ppp", NULL },
        UPNP_IGD_MAPPED, 0, NULL, NULL, NULL },
      { "external port taken: the next ones are tried",
        { "--taken", "19019,19020", NULL },
        UPNP_IGD_MAPPED, 19021, NULL, NULL, "ext=19019 int=19019" },
      { "router with permanent leases only", { "--permanent-only", NULL },
        UPNP_IGD_MAPPED, 0, NULL, NULL, "lease=0 desc" },
      { "router that will not tell its address", { "--no-extip", NULL },
        UPNP_IGD_MAPPED, 0, "", NULL, NULL },
      { "carrier-grade NAT", { "--extip", "100.72.1.9", NULL },
        UPNP_IGD_MAPPED, 0, "100.72.1.9", NULL, NULL },
      { "no router answers", { "--silent", NULL },
        UPNP_IGD_FAILED, 0, NULL, "no UPnP router answered", "M-SEARCH" },
      { "description missing", { "--desc-404", NULL },
        UPNP_IGD_FAILED, 0, NULL, "no port mapping service", NULL },
      { "gateway without a WAN connection service", { "--no-service", NULL },
        UPNP_IGD_FAILED, 0, NULL, "no port mapping service", NULL },
      { "router refuses (not authorised)", { "--deny", NULL },
        UPNP_IGD_FAILED, 0, NULL, "does not allow", "-> 606" },
      { "every port taken", { "--taken", "19019,19020,19021,19022,19023", NULL },
        UPNP_IGD_FAILED, 0, NULL, "already forwarded", "-> 718" },
   };
   unsigned i;

   setvbuf(stdout, NULL, _IOLBF, 0);
   if (argc > 1)
      g_fake = argv[1];
   test_parsers();
   for (i = 0; i < sizeof(S) / sizeof(S[0]); i++)
      test_scenario(&S[i]);
   {
      /* the carrier-grade NAT warning */
      upnp_igd *u = upnp_igd_open(inet_addr("192.168.1.23"), 1, 1, NULL);
      CHECK(upnp_igd_external_is_private(u) == 0, "unknown address: no warning");
      upnp_igd_close(u);
   }
   test_multicast();
   if (g_fail)
   {
      printf("FAILED: %d check(s)\n", g_fail);
      return 1;
   }
   printf("ALL IGD TESTS PASSED (against tests/fake_igd.py, not a real router)\n");
   return 0;
}
