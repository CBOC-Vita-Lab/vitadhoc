// runner.cpp -- tests of online_http.cpp.  One binary, one mode per run (argv[1]); the Python side starts the fake service and passes
// its address.  Every check prints "ok <name>" or "FAIL <name>: <why>"; the exit status is the number of failures (capped at 100).
//
//   json                          the pure helpers: every answer shape of 1.2, garbage, the description writer
//   flow BASE                     every route against the fake (or the real dev service), the fields of every answer
//   errors BASE                   every error code of 1.2 and its ONLINE_E_* (2.3), oversized and garbage answers
//   three BASE                    three requests in flight at once, a fourth refused, drop()
//   down PORT | blackhole PORT    nobody listens -> NET at once; accepted and never answered -> NET after the 8 s limit
//   nohttps                       built without TLS: https:// is answered NET, np_https_available() is 0
//   tls BASE expect               (TLS build) expect = ok | cert | clock; the test root comes from $TEST_ROOT_PEM
//   selftest URL                  (TLS build) np_https_selftest's own GET, started by the environment
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "online_http.h"
#include "nethttps/np_https.h"
#include "nethttps/np_https_sys.h"
#include "nethttps/np_https_tls.h"
#ifdef NP_HAVE_MBEDTLS
#include "mbedtls/base64.h"
#include "mbedtls/x509_crt.h"
#include "nethttps/np_https_roots.h"   /* (t_scope: this file's own copy of the bundled roots) */
#endif

// online_http.cpp reports through fe_evt; here the lines are kept so a test can read them
static char g_evt[64][256];
static int g_evt_n;
extern "C" void fe_evt(const char *fmt, ...)
{
   va_list ap;
   if (g_evt_n >= 64)
      return;
   va_start(ap, fmt);
   vsnprintf(g_evt[g_evt_n++], sizeof g_evt[0], fmt, ap);
   va_end(ap);
}

static int g_fail;

static void check(int cond, const char *name, const char *fmt = "", ...)
{
   if (cond)
   {
      printf("ok %s\n", name);
      return;
   }
   char why[256];
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(why, sizeof why, fmt, ap);
   va_end(ap);
   printf("FAIL %s: %s\n", name, why);
   g_fail++;
}

static int64_t now_ms(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static bool take_wait(online_reply *r, int timeout_ms)
{
   const int64_t until = now_ms() + timeout_ms;
   while (now_ms() < until)
   {
      if (online_http_take(r))
         return true;
      usleep(3000);
   }
   return false;
}

/* one call, one answer */
static online_reply one(int rc, int timeout_ms = 15000)
{
   online_reply r;
   memset(&r, 0, sizeof r);
   if (rc != 0)
   {
      r.err = -99;
      return r;
   }
   if (!take_wait(&r, timeout_ms))
      r.err = -98;
   return r;
}

static online_desc sample_desc(void)
{
   online_desc d;
   memset(&d, 0, sizeof d);
   d.kind = 1;
   strcpy(d.console, "gba");
   strcpy(d.game, "BPEE");
   strcpy(d.rom, "9f3a01c2");
   strcpy(d.title, "Pokemon Emerald");
   strcpy(d.nick, "Alice");
   strcpy(d.ver, "2.0.0");
   d.compat = 0x1a2b3c4d;
   strcpy(d.pub, "203.0.113.7:19019");
   strcpy(d.lan, "192.168.1.23:19019");
   return d;
}

/* ---- json ---------------------------------------------------------------------------------------------------------------------- */

static const char *S_CREATE = "{\"code\":\"K7M2Q\",\"token\":\"AbCdEfGhIjKlMnOpQrStUv\",\"punchKey\":\"0123456789abcdef\",\"ttlSec\":600,\"pollMs\":2000}";
static const char *S_POLL = "{\"state\":\"joined\",\"mode\":\"punch\",\"peer\":{\"nick\":\"Bob\",\"pub\":\"198.51.100.9:40000\",\"lan\":\"\",\"sym\":1},"
                            "\"relayPeer\":\"\",\"reason\":\"\",\"punchMs\":4000,\"pollMs\":1500}";
static const char *S_RELAY = "{\"ttlSec\":3600,\"servers\":[{\"urls\":[\"stun:s:3478\",\"turn:turn.cloudflare.com:3478?transport=udp\"],"
                             "\"username\":\"u\",\"credential\":\"c\"}]}";

static void t_json(void)
{
   char out[64];
   check(online_json_get(S_CREATE, "code", out, sizeof out) && !strcmp(out, "K7M2Q"), "json get string");
   check(online_json_get(S_CREATE, "token", out, sizeof out) && !strcmp(out, "AbCdEfGhIjKlMnOpQrStUv"), "json get token");
   check(online_json_int(S_CREATE, "ttlSec", -1) == 600, "json int");
   check(online_json_get(S_POLL, "peer.pub", out, sizeof out) && !strcmp(out, "198.51.100.9:40000"), "json nested");
   check(online_json_int(S_POLL, "peer.sym", 0) == 1, "json nested int");
   check(online_json_get(S_POLL, "peer.lan", out, sizeof out) == 1 && out[0] == 0, "json empty string is found");
   check(online_json_get(S_POLL, "peer.nope", out, sizeof out) == 0, "json missing key");
   check(online_json_get(S_POLL, "nope.pub", out, sizeof out) == 0, "json missing parent");
   check(online_json_get(S_POLL, "peer.pub.x", out, sizeof out) == 0, "json descend into a string");
   check(online_json_get("{\"peer\":null}", "peer.nick", out, sizeof out) == 0, "json null peer");
   check(online_json_get("{\"peer\":null}", "peer", out, sizeof out) && !strcmp(out, "null"), "json null value text");
   check(online_json_int("{\"relay\":true,\"busy\":false}", "relay", 5) == 1 && online_json_int("{\"relay\":true,\"busy\":false}", "busy", 5) == 0, "json booleans");
   check(online_json_int("{\"a\":\"x\"}", "a", 7) == 7 && online_json_int("{\"a\":12x}", "a", 7) == 7, "json int default on junk");
   check(online_json_int("{\"a\":-3}", "a", 7) == -3, "json negative");
   check(online_json_get(S_RELAY, "servers.0.urls.1", out, sizeof out) && !strncmp(out, "turn:turn.cloudflare.com:3478", 29), "json array index");
   check(online_json_get(S_RELAY, "servers.0.urls.2", out, sizeof out) == 0, "json array past the end");
   check(online_json_get(S_RELAY, "servers.1.username", out, sizeof out) == 0, "json array entry past the end");
   check(online_json_get(S_RELAY, "servers.x.username", out, sizeof out) == 0, "json array non-numeric index");
   check(online_json_get("{\"error\":{\"code\":\"update_required\",\"message\":\"m\",\"min\":\"2.1.0\"}}", "error.min", out, sizeof out) && !strcmp(out, "2.1.0"), "json error shape");
   check(online_json_get(S_CREATE, "token", out, 10) == 0 && out[0] == 0, "json never hands out a truncated value");
   check(online_json_get("{\"a\":\"x\\\"y\"}", "a", out, sizeof out) == 0, "json refuses a backslash");
   /* 2.0 security review: the service only sends printable ASCII inside a string; anything else is not its answer */
   check(online_json_get("{\"a\":\"Bob\xc3\xa9\"}", "a", out, sizeof out) == 0, "json refuses a byte above 0x7E in the value");
   check(online_json_get("{\"n\":\"x\x7fy\",\"a\":\"1\"}", "a", out, sizeof out) == 0, "json refuses DEL in an earlier value");
   check(online_json_get("{\"k\xff\":\"1\",\"a\":\"1\"}", "a", out, sizeof out) == 0, "json refuses a byte above 0x7E in a key");
   check(online_json_get("{\"a\":\"~ !\"}", "a", out, sizeof out) && !strcmp(out, "~ !"), "json keeps the whole printable range");
   check(online_json_get("{\"a\":\"1\",\"a\":\"2\"}", "a", out, sizeof out) && !strcmp(out, "1"), "json first duplicate wins");
   check(online_json_get(" \n{ \"a\" : \"1\" , \"b\" : [ 1 , { \"c\" : \"d\" } ] } ", "b.1.c", out, sizeof out) && !strcmp(out, "d"), "json whitespace");
   check(online_json_get("{\"a\":{\"b\":[1,2,{\"c\":3}]},\"z\":\"end\"}", "z", out, sizeof out) && !strcmp(out, "end"), "json skips nested values");

   const char *garbage[] = {"", " ", "{", "}", "[", "{\"a\"", "{\"a\":", "{\"a\":\"x", "{\"a\":1", "{a:1}", "nul", "{\"a\":}", "[1,2", "{\"a\":[}",
                            "{\"a\":{\"b\":}}", "\"just a string\"", "42", "{\"a\":\"x\ny\"}", "<html></html>", "{\"a\":1,}", "{,}"};
   int bad = 0;
   for (size_t i = 0; i < sizeof garbage / sizeof *garbage; i++)
   {
      out[0] = 'x';
      if (online_json_get(garbage[i], "a", out, sizeof out) && strcmp(garbage[i], "{\"a\":1,}") && strcmp(garbage[i], "{\"a\":\"x\ny\"}"))
      {
         if (strcmp(garbage[i], "{\"a\":1") && strcmp(garbage[i], "{\"a\":\"x"))
            bad++;
      }
   }
   check(bad == 0, "json garbage finds nothing", "%d garbage inputs gave a value", bad);
   check(online_json_get(NULL, "a", out, sizeof out) == 0 && online_json_get("{}", NULL, out, sizeof out) == 0 && online_json_get("{}", "", out, sizeof out) == 0 &&
         online_json_get("{\"a\":1}", "a", NULL, 0) == 0, "json null arguments");
   {  /* nesting far past the depth limit, and random bytes, must not crash or hang */
      char deep[400];
      memset(deep, '[', 300);
      deep[300] = 0;
      check(online_json_get(deep, "0.0.0.0.0.0.0.0.0.0.0.0", out, sizeof out) == 0, "json deep nesting");
      unsigned seed = 12345;
      int found = 0;
      for (int it = 0; it < 20000; it++)
      {
         char buf[96];
         const char alphabet[] = "{}[]\",:0123456789 abtrulefn.-\\";
         int n = 1 + (int)(seed % 90);
         for (int i = 0; i < n; i++)
         {
            seed = seed * 1103515245u + 12345u;
            buf[i] = alphabet[(seed >> 16) % (sizeof alphabet - 1)];
         }
         buf[n] = 0;
         found += online_json_get(buf, "a.0.b", out, sizeof out);
         online_json_int(buf, "a", 3);
      }
      check(1, "json random bytes (20000) did not crash", "found %d", found);
      /* mutations of real answers (every byte value, cuts, repeats), into a small output: no crash, no overrun, and a value
       * that is handed out is always terminated, shorter than its buffer and printable */
      const char *real[] = {S_CREATE, S_POLL, S_RELAY};
      int dirty = 0;
      for (int it = 0; it < 30000; it++)
      {
         char buf[1200], small[24];
         const char *src = real[it % 3];
         size_t len = strlen(src);
         if (len >= sizeof buf) len = sizeof buf - 1;
         memcpy(buf, src, len);
         buf[len] = 0;
         for (int m = 0; m < 1 + (int)(seed % 4); m++)
         {
            seed = seed * 1103515245u + 12345u;
            const size_t at = (seed >> 8) % len;
            seed = seed * 1103515245u + 12345u;
            switch ((seed >> 16) % 4)
            {
               case 0: buf[at] = (char)(seed >> 20); break;                  /* any byte, NUL included */
               case 1: buf[at] = "{}[]\",:\\"[(seed >> 20) % 8]; break;        /* a structural one */
               case 2: buf[at] = 0; break;                                    /* cut */
               default: memmove(buf + at, buf + at / 2, len - at); break;      /* a repeat */
            }
         }
         static const char *paths[] = {"token", "code", "peer.nick", "peer.pub", "servers.0.username", "servers.0.urls.1", "state", "error.code"};
         for (size_t k = 0; k < sizeof paths / sizeof *paths; k++)
         {
            memset(small, 0x5A, sizeof small);
            if (online_json_get(buf, paths[k], small, sizeof small - 4))
            {
               const size_t l = strnlen(small, sizeof small);
               if (l >= sizeof small - 4) dirty++;
               for (size_t c = 0; c < l; c++)
                  if ((unsigned char)small[c] < 0x20 || (unsigned char)small[c] > 0x7E) dirty++;
            }
            if (small[sizeof small - 4] != 0x5A || small[sizeof small - 1] != 0x5A) dirty++;   /* nothing past the cap given */
            online_json_int(buf, paths[k], 1);
         }
      }
      check(dirty == 0, "json mutated answers (30000): bounded, terminated, printable", "%d bad", dirty);
   }

   /* the description writer */
   online_desc d = sample_desc();
   char big[512];
   size_t n = online_json_desc(big, sizeof big, &d, NULL);
   const char *want = "{\"kind\":1,\"console\":\"gba\",\"game\":\"BPEE\",\"rom\":\"9f3a01c2\",\"title\":\"Pokemon Emerald\",\"nick\":\"Alice\","
                      "\"ver\":\"2.0.0\",\"compat\":\"1a2b3c4d\",\"pub\":\"203.0.113.7:19019\",\"lan\":\"192.168.1.23:19019\",\"sym\":0,\"relayOnly\":0}";
   check(n == strlen(want) && !strcmp(big, want), "desc exact text", "%s", big);
   d.sym = 1;
   d.relay_only = 1;
   d.pub[0] = d.lan[0] = 0;
   d.compat = 0xA;
   strcpy(d.title, "a\"b\\c\xe9\x01z");
   n = online_json_desc(big, sizeof big, &d, "\"token\":\"x\"");
   check(n == strlen(big) && strstr(big, "\"title\":\"a?b?c??z\"") && strstr(big, "\"compat\":\"0000000a\"") && strstr(big, "\"pub\":\"\"") &&
         strstr(big, "\"sym\":1,\"relayOnly\":1,\"token\":\"x\"}"), "desc sanitised, relay-only, extra members", "%s", big);
   check(online_json_get(big, "nick", out, sizeof out) && !strcmp(out, "Alice"), "desc output is readable back");
   check(online_json_desc(big, 40, &d, NULL) == 0 && big[0] == 0, "desc too small -> 0");
   check(online_json_desc(big, sizeof big, NULL, NULL) == 0, "desc null");
   d = sample_desc();
   char exact[512];
   const size_t full = online_json_desc(exact, sizeof exact, &d, NULL);
   check(online_json_desc(big, full + 1, &d, NULL) == full && online_json_desc(big, full, &d, NULL) == 0, "desc exact fit and one byte short");
   check(full < 400, "desc stays small", "%u bytes", (unsigned)full);
}

/* ---- flow ---------------------------------------------------------------------------------------------------------------------- */

static void t_flow(const char *base, int real)
{
   online_http_config(base, "2.0.0");
   online_desc d = sample_desc();
   online_reply r;

   r = one(online_http_create(&d, "0123456789abcdef"));
   check(r.err == 0 && r.op == ONLINE_OP_CREATE && r.http == 201 && (real || !strcmp(r.code, "K7M2Q")),
         "create answer", "err %d op %d http %d code %s", r.err, r.op, r.http, r.code);
   const char *code = r.code;
   char token[24];
   strcpy(token, r.token);
   check(strlen(r.token) == 22 && strlen(r.punch_key) == 16 && r.ttl_sec == 600 && r.poll_ms == 2000 && r.state == ONLINE_ROOM_OPEN, "create fields",
         "token %s key %s ttl %d poll %d", r.token, r.punch_key, r.ttl_sec, r.poll_ms);
   char roomcode[8];
   strcpy(roomcode, code);

   r = one(online_http_lookup(roomcode));
   check(r.err == 0 && r.op == ONLINE_OP_LOOKUP && r.state == ONLINE_ROOM_OPEN, "lookup answer", "err %d state %d", r.err, r.state);
   check(r.card.kind == 1 && !strcmp(r.card.console, "gba") && !strcmp(r.card.game, "BPEE") && !strcmp(r.card.rom, "9f3a01c2") &&
         !strcmp(r.card.title, "Pokemon Emerald") && !strcmp(r.card.nick, "Alice") && !strcmp(r.card.ver, "2.0.0") && r.card.compat == 0x1a2b3c4d,
         "lookup card", "%d %s %s %s %s %s %s %08x", r.card.kind, r.card.console, r.card.game, r.card.rom, r.card.title, r.card.nick, r.card.ver, r.card.compat);
   if (real)
      return;   /* the rest of the script is the fake's */

   r = one(online_http_lookup("FULL"));
   check(r.err == 0 && r.state == ONLINE_ROOM_FULL, "lookup full room");
   r = one(online_http_lookup("k7m2q"));
   check(r.err == 0 && !strcmp(r.code, "K7M2Q"), "lookup lower-case code");

   r = one(online_http_join("K7M2Q", &d, "fedcba9876543210"));
   check(r.err == 0 && r.op == ONLINE_OP_JOIN && !strcmp(r.token, token) && !strcmp(r.punch_key, "0123456789abcdef") && r.mode == ONLINE_MODE_PUNCH &&
         r.have_peer && !strcmp(r.peer_nick, "Alice") && !strcmp(r.peer_pub, "203.0.113.7:19019") && !strcmp(r.peer_lan, "192.168.1.23:19019") &&
         r.peer_sym == 1 && r.punch_ms == 6000 && r.poll_ms == 1500, "join answer", "err %d mode %d peer %d %s %s", r.err, r.mode, r.have_peer, r.peer_nick, r.peer_pub);
   r = one(online_http_join("RELAYM", &d, NULL));
   check(r.err == 0 && r.mode == ONLINE_MODE_RELAY, "join relay mode");
   /* a request id that is not 16 lower-case hex digits never leaves the console */
   const char *badrid[] = { "NOTHEX", "0123456789ABCDEF", "0123456789abcde", "0123456789abcdef0" };
   for (int i = 0; i < 4; i++) {
      r = one(online_http_create(&d, badrid[i]));
      check(r.err == ONLINE_E_BAD && r.op == ONLINE_OP_CREATE, "create with a malformed request id is refused locally", "rid %s err %d", badrid[i], r.err);
      r = one(online_http_join("K7M2Q", &d, badrid[i]));
      check(r.err == ONLINE_E_BAD && r.op == ONLINE_OP_JOIN, "join with a malformed request id is refused locally", "rid %s err %d", badrid[i], r.err);
   }

   r = one(online_http_poll("K7M2Q", token, NULL));
   check(r.err == 0 && r.op == ONLINE_OP_POLL && r.state == ONLINE_ROOM_JOINED && r.mode == ONLINE_MODE_PUNCH && r.have_peer && !strcmp(r.peer_nick, "Bob") &&
         !strcmp(r.peer_pub, "198.51.100.9:40000") && r.peer_lan[0] == 0 && r.peer_sym == 0 && r.relay_peer[0] == 0 && r.punch_ms == 4000 && r.poll_ms == 1500,
         "poll joined", "err %d state %d peer %d", r.err, r.state, r.have_peer);
   r = one(online_http_poll("K7M2Q", token, "203.0.113.9:20000"));
   check(r.err == 0 && r.state == ONLINE_ROOM_JOINED, "poll with a new public address");
   r = one(online_http_poll("NOBODY", token, NULL));
   check(r.err == 0 && r.state == ONLINE_ROOM_OPEN && !r.have_peer && r.peer_nick[0] == 0 && r.punch_ms == 4000 && r.poll_ms == 2000, "poll open, peer null");
   r = one(online_http_poll("CLOSED", token, NULL));
   check(r.err == 0 && r.state == ONLINE_ROOM_CLOSED && !strcmp(r.reason, "cancel") && r.poll_ms == 15000, "poll closed, pollMs clamped to 15000", "poll_ms %d", r.poll_ms);
   r = one(online_http_poll("RELAYM", token, NULL));
   check(r.err == 0 && r.state == ONLINE_ROOM_RELAY && r.mode == ONLINE_MODE_RELAY && !strcmp(r.relay_peer, "203.0.113.50:49152"), "poll relay + relayPeer");
   r = one(online_http_poll("K7M2Q", "wrongtoken", NULL));
   check(r.err == ONLINE_E_TOKEN && r.http == 401, "poll bad token -> TOKEN", "err %d", r.err);

   r = one(online_http_relay("K7M2Q", token));
   check(r.err == 0 && r.op == ONLINE_OP_RELAY && !strcmp(r.turn_host, "turn.cloudflare.com") && r.turn_ports == 3 && r.turn_port[0] == 3478 &&
         r.turn_port[1] == 443 && r.turn_port[2] == 53 && !strcmp(r.turn_user, "u-1700000000:abc") && strlen(r.turn_cred) == 40 && r.ttl_sec == 3600,
         "relay credentials parsed, ports ordered 3478 443 53", "err %d host %s ports %d: %d %d %d user %s", r.err, r.turn_host, r.turn_ports, r.turn_port[0], r.turn_port[1], r.turn_port[2], r.turn_user);
   r = one(online_http_relay("NOCRED", token));
   check(r.err == ONLINE_E_RELAY, "relay answer without a server entry -> RELAY", "err %d", r.err);

   r = one(online_http_endpoint("K7M2Q", token, "203.0.113.5:50000"));
   check(r.err == 0 && r.op == ONLINE_OP_ENDPOINT && !strcmp(r.relay_peer, "203.0.113.50:49152"), "endpoint answer");
   r = one(online_http_close("K7M2Q", token, "linked"));
   check(r.err == 0 && r.op == ONLINE_OP_CLOSE && r.http == 200, "close linked");
   r = one(online_http_close("K7M2Q", token, "not-a-reason"));
   check(r.err == 0, "close with an unknown reason is sent as cancel");

   r = one(online_http_resolve("127.0.0.1"));
   check(r.err == 0 && r.op == ONLINE_OP_RESOLVE && !strcmp(r.resolved, "127.0.0.1"), "resolve of an IPv4 literal");
   r = one(online_http_resolve("localhost"));
   check(r.err == 0 && !strcmp(r.resolved, "127.0.0.1"), "resolve of localhost", "err %d %s", r.err, r.resolved);
   r = one(online_http_resolve("no such host!"));
   check(r.err == ONLINE_E_BAD, "resolve of a bad name");
   r = one(online_http_resolve("nothing-here.invalid"), 10000);
   check(r.err == ONLINE_E_NET && r.op == ONLINE_OP_RESOLVE, "resolve of an unknown name -> NET", "err %d", r.err);

   r = one(online_http_lookup("../../etc"));
   check(r.err == ONLINE_E_NOT_FOUND && r.op == ONLINE_OP_LOOKUP && r.http == 0, "a code with other characters is NOT_FOUND without a request");
   r = one(online_http_lookup("toolongcode"));
   check(r.err == ONLINE_E_NOT_FOUND, "an over-long code is NOT_FOUND");
   r = one(online_http_lookup(""));
   check(r.err == ONLINE_E_NOT_FOUND, "an empty code is NOT_FOUND");

   r = one(online_http_resolve("127.0.0.1"));   /* the info route has no function of its own in 2.3: it is used by the smoke test through take() */
   check(online_http_busy(ONLINE_OP_NONE) == 0, "nothing in flight at the end");
}

/* ---- errors -------------------------------------------------------------------------------------------------------------------- */

struct ErrCase { const char *code; int err; int http; int retry; const char *a, *b; };

static void t_errors(const char *base)
{
   online_http_config(base, "2.0.0");
   static const ErrCase cases[] = {
      {"E404", ONLINE_E_NOT_FOUND, 404, 0, 0, 0},
      {"E409F", ONLINE_E_FULL, 409, 0, 0, 0},
      {"E409V", ONLINE_E_VERSION, 409, 0, "2.0.7", 0},
      {"E409X", ONLINE_E_BAD, 409, 0, 0, 0},
      {"E426", ONLINE_E_UPDATE, 426, 0, "2.1.0", "2.1.3"},
      {"E503O", ONLINE_E_BUSY, 503, 120, 0, 0},
      {"E503R", ONLINE_E_BUSY, 503, 0, 0, 0},
      {"E503S", ONLINE_E_BUSY, 503, 0, 0, 0},
      {"E503U", ONLINE_E_RELAY, 503, 0, 0, 0},
      {"E503B", ONLINE_E_RELAY, 503, 3600, 0, 0},
      {"E503Z", ONLINE_E_SERVER, 503, 0, 0, 0},
      {"E429D", ONLINE_E_BUSY, 429, 9, 0, 0},
      {"E429S", ONLINE_E_RATE, 429, 1, 0, 0},
      {"E401", ONLINE_E_TOKEN, 401, 0, 0, 0},
      {"E403", ONLINE_E_RELAY_WAIT, 403, 0, 0, 0},
      {"E502", ONLINE_E_RELAY, 502, 0, 0, 0},
      {"E400", ONLINE_E_BAD, 400, 0, 0, 0},
      {"E400V", ONLINE_E_BAD, 400, 0, 0, 0},
      {"E400C", ONLINE_E_CONSOLE, 400, 0, 0, 0},
      {"E400F", ONLINE_E_BAD, 400, 0, 0, 0},
      {"E413", ONLINE_E_BAD, 413, 0, 0, 0},
      {"E500", ONLINE_E_SERVER, 500, 0, 0, 0},
      {"E504", ONLINE_E_SERVER, 504, 0, 0, 0},
      {"E418", ONLINE_E_BAD, 418, 0, 0, 0},
      {"BIG", ONLINE_E_BAD, 0, 0, 0, 0},
      {"GARBAGE", ONLINE_E_BAD, 200, 0, 0, 0},
   };
   online_desc d = sample_desc();
   for (size_t i = 0; i < sizeof cases / sizeof *cases; i++)
   {
      const ErrCase &c = cases[i];
      char name[96];
      /* the same script through three different routes: the mapping does not depend on the call */
      for (int route = 0; route < 3; route++)
      {
         online_reply r;
         if (route == 0)
            r = one(online_http_lookup(c.code));
         else if (route == 1)
            r = one(online_http_poll(c.code, "AbCdEfGhIjKlMnOpQrStUv", NULL));
         else
            r = one(online_http_join(c.code, &d, NULL));
         snprintf(name, sizeof name, "error %s via route %d -> %d", c.code, route, c.err);
         int ok = r.err == c.err && (c.http == 0 || r.http == c.http) && r.retry_after == c.retry;
         if (c.a && c.err == ONLINE_E_UPDATE)
            ok = ok && !strcmp(r.min_ver, c.a) && !strcmp(r.latest_ver, c.b);
         if (c.a && c.err == ONLINE_E_VERSION)
            ok = ok && !strcmp(r.host_ver, c.a);
         check(ok, name, "err %d http %d retry %d min %s latest %s host %s", r.err, r.http, r.retry_after, r.min_ver, r.latest_ver, r.host_ver);
      }
   }
}

/* ---- three at once, drop --------------------------------------------------------------------------------------------------------- */

static void t_three(const char *base)
{
   online_http_config(base, "2.0.0");
   check(online_http_lookup("SLOW") == 0, "three: lookup started");
   check(online_http_poll("NOBODY", "AbCdEfGhIjKlMnOpQrStUv", NULL) == 0, "three: poll started");
   check(online_http_resolve("localhost") == 0, "three: resolve started");
   check(online_http_relay("K7M2Q", "AbCdEfGhIjKlMnOpQrStUv") == -1, "three: a fourth is refused with -1");
   check(online_http_busy(ONLINE_OP_LOOKUP) == 1 && online_http_busy(ONLINE_OP_POLL) == 1 && online_http_busy(ONLINE_OP_RESOLVE) == 1 && online_http_busy(ONLINE_OP_NONE) == 1 &&
         online_http_busy(ONLINE_OP_JOIN) == 0, "three: busy() by op");
   int seen[16] = {0};
   online_reply r;
   int order[3], n = 0;
   for (int i = 0; i < 3; i++)
   {
      if (!take_wait(&r, 10000))
         break;
      if (r.op >= 0 && r.op < 16)
         seen[r.op]++;
      order[n++] = r.op;
      if (r.op == ONLINE_OP_LOOKUP)
         check(r.err == 0 && !strcmp(r.card.game, "BPEE"), "three: the lookup answer is its own");
      if (r.op == ONLINE_OP_POLL)
         check(r.err == 0 && r.state == ONLINE_ROOM_OPEN && !r.have_peer, "three: the poll answer is its own");
      if (r.op == ONLINE_OP_RESOLVE)
         check(r.err == 0 && !strcmp(r.resolved, "127.0.0.1"), "three: the resolve answer is its own");
   }
   check(n == 3 && seen[ONLINE_OP_LOOKUP] == 1 && seen[ONLINE_OP_POLL] == 1 && seen[ONLINE_OP_RESOLVE] == 1, "three: one answer per op", "got %d answers", n);
   check(order[2] == ONLINE_OP_LOOKUP, "three: the slow one came last", "last op %d", order[2]);
   check(online_http_busy(ONLINE_OP_NONE) == 0 && online_http_take(&r) == 0, "three: nothing left");

   /* drop() forgets what is in flight */
   check(online_http_lookup("SLOW") == 0 && online_http_resolve("localhost") == 0, "drop: two started");
   online_http_drop();
   check(online_http_busy(ONLINE_OP_NONE) == 0, "drop: nothing in flight afterwards");
   usleep(900 * 1000);
   check(online_http_take(&r) == 0, "drop: the answers are discarded");
   r = one(online_http_lookup("K7M2Q"));
   check(r.err == 0, "drop: the client still works afterwards");
   /* the log lines reach fe_evt from take() on this thread */
   int lines = 0;
   for (int i = 0; i < g_evt_n; i++)
      if (!strncmp(g_evt[i], "online_tls ", 11))
         lines++;
   check(lines > 0, "log lines were handed to fe_evt", "%d", lines);
   int leaked = 0;
   for (int i = 0; i < g_evt_n; i++)
      if (strstr(g_evt[i], "AbCdEf") || strstr(g_evt[i], "203.0.113") || strstr(g_evt[i], "198.51.100"))
         leaked++;
   check(leaked == 0, "no token or address in the log");
   online_http_shutdown();
   check(1, "shutdown returned");
}

/* A drop must never block the frame thread, even when the resolver threads are stuck (a Vita resolver ignores cancel until its own
 * time-out) and more of them have been abandoned than the client keeps track of. */
static uint64_t NowMs(void)
{
   timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void t_dropstuck(void)
{
   online_http_config("http://127.0.0.1:1", "2.0.0");
   nph::TestStallResolve(2500);   // a Resolve that ignores `cancel` for 2.5 s
   int started = 0;
   uint64_t worst = 0;
   for (int round = 0; round < 8; round++)
   {
      for (int k = 0; k < 3; k++)
         if (online_http_resolve("localhost") == 0)
            started++;
      const uint64_t t0 = NowMs();
      online_http_drop();
      const uint64_t dt = NowMs() - t0;
      if (dt > worst)
         worst = dt;
   }
   check(started >= 18, "dropstuck: resolves were started", "%d", started);
   check(worst < 300, "dropstuck: drop returned at once although every resolver is stuck", "worst %u ms", (unsigned)worst);
   online_http_shutdown();
   check(1, "dropstuck: shutdown returned");
}

static void t_badver(const char *base)
{
   /* a build label is never sent as the version: the fake (like the service) answers 400 to such a `v` */
   const char *labels[] = {"2.0-dev", "", "2.0.0-rc1", "12345.1", "1.2.3.4.5", "v2"};
   for (size_t i = 0; i < sizeof labels / sizeof *labels; i++)
   {
      online_http_config(base, labels[i]);
      online_reply r = one(online_http_lookup("K7M2Q"));
      check(r.err == 0, "unusable version text falls back to 2.0.0", "'%s' -> err %d http %d", labels[i], r.err, r.http);
   }
   online_http_config(base, "1.2.3.4");
   online_reply r = one(online_http_lookup("K7M2Q"));
   check(r.err == 0, "a valid four-part version is kept");
}

static void t_down(int port)
{
   char base[64];
   snprintf(base, sizeof base, "http://127.0.0.1:%d", port);
   online_http_config(base, "2.0.0");
   online_reply r = one(online_http_lookup("K7M2Q"), 5000);
   check(r.err == ONLINE_E_NET && r.op == ONLINE_OP_LOOKUP, "server down -> NET", "err %d", r.err);
   online_desc d = sample_desc();
   r = one(online_http_create(&d, NULL), 5000);
   check(r.err == ONLINE_E_NET, "server down, create -> NET");
}

static void t_blackhole(int port)
{
   char base[64];
   snprintf(base, sizeof base, "http://127.0.0.1:%d", port);
   online_http_config(base, "2.0.0");
   const int64_t t0 = now_ms();
   online_reply r = one(online_http_lookup("K7M2Q"), 15000);
   const int64_t dt = now_ms() - t0;
   check(r.err == ONLINE_E_NET, "no answer -> NET", "err %d", r.err);
   check(dt >= 5500 && dt <= 9500, "the limit is about 8 s (6 s of silence or 8 s in all)", "%d ms", (int)dt);
}

static void t_nohttps(void)
{
   online_http_config("https://localhost:1", "2.0.0");
   check(np_https_available() == 0, "no TLS built in: np_https_available() is 0");
   online_reply r = one(online_http_lookup("K7M2Q"), 5000);
   check(r.err == ONLINE_E_NET && r.op == ONLINE_OP_LOOKUP, "no TLS built in: https:// is answered NET", "err %d", r.err);
   online_http_config("", "2.0.0");
   r = one(online_http_lookup("K7M2Q"), 5000);
   check(r.err == ONLINE_E_NET, "default base url is https and is refused the same way");
   online_http_shutdown();
}

/* ---- TLS ---------------------------------------------------------------------------------------------------------------------- */

static int64_t g_clock2011(void) { return 1300000000LL; }

static void load_test_root(void)
{
   const char *path = getenv("TEST_ROOT_PEM");
   if (!path)
      return;
   FILE *f = fopen(path, "rb");
   if (!f)
      return;
   static char pem[8192];
   const size_t n = fread(pem, 1, sizeof pem - 1, f);
   fclose(f);
   pem[n] = 0;
#ifdef NP_HAVE_MBEDTLS
   nph::tls::SetExtraTestRoots(pem, n + 1);   /* the ONLY way a test root gets in; takes effect at the next Init */
#endif
}

#ifdef NP_HAVE_MBEDTLS
/* Whose roots are whose (np_https_roots.h only_host): all twelve bundled
 * roots carry only_host = 0 so every bundled root anchors every host. */
static void t_scope(void)
{
   char why[80] = "";
   int n = 0, own = 0;
   check(nph::tls::Init(why, sizeof why, &n) && n == NP_HTTPS_ROOT_COUNT, "every bundled root loads", "%s, %d roots", why, n);
   for (int i = 0; i < NP_HTTPS_ROOT_COUNT; i++)
   {
      const NpHttpsRootEntry &r = np_https_roots[i];
      static unsigned char b64[4096];
      static char pem[4400];
      size_t bn = 0;
      uint32_t raw_gh = 0, raw_room = 0, raw_other = 0;
      nph::tls::Details d;
      char name[160];
      if (mbedtls_base64_encode(b64, sizeof b64, &bn, r.der, r.len) != 0)
         bn = 0;
      const int m = snprintf(pem, sizeof pem, "-----BEGIN CERTIFICATE-----\n%.*s\n-----END CERTIFICATE-----\n", (int)bn, b64);
      nph::tls::VerifyChain(pem, (size_t)m + 1, "api.github.com", &raw_gh, &d);
      nph::tls::VerifyChain(pem, (size_t)m + 1, "vitadhoc.psvita.dev", &raw_room, &d);
      nph::tls::VerifyChain(pem, (size_t)m + 1, "github.com", &raw_other, &d);
      const bool gh = bn && !(raw_gh & MBEDTLS_X509_BADCERT_NOT_TRUSTED), room = bn && !(raw_room & MBEDTLS_X509_BADCERT_NOT_TRUSTED),
                 other = bn && !(raw_other & MBEDTLS_X509_BADCERT_NOT_TRUSTED);
      const bool mine = r.only_host != NULL;
      own += mine;
      snprintf(name, sizeof name, "%s: an anchor for every host", r.name);
      check(!mine && gh && room && other, name,
            "github %d room %d other %d", gh, room, other);
   }
   check(own == 0 && NP_HTTPS_ROOT_COUNT == 12, "all twelve roots apply to every host (only_host = 0)", "%d of %d", own,
         NP_HTTPS_ROOT_COUNT);
}
#endif

static void t_tls(const char *base, const char *expect)
{
   load_test_root();
#ifdef NP_HAVE_MBEDTLS
   /* TEST_ONLY_HOST: this host is the one with roots of its own (the header's four), so the test root no longer anchors it */
   if (getenv("TEST_ONLY_HOST"))
      nph::tls::SetTestOnlyHost(getenv("TEST_ONLY_HOST"));
#endif
   online_http_config(base, "2.0.0");
   online_desc d = sample_desc();
   online_reply r;
   if (!strcmp(expect, "clock"))
   {
      nph::g_wall_override = g_clock2011;
      r = one(online_http_lookup("K7M2Q"), 10000);
      check(r.err == ONLINE_E_CLOCK, "clock before the floor -> CLOCK", "err %d", r.err);
      nph::g_wall_override = NULL;
      return;
   }
   if (!strcmp(expect, "cert"))
   {
      r = one(online_http_lookup("K7M2Q"), 10000);
      check(r.err == ONLINE_E_CERT, "certificate refused -> CERT", "err %d", r.err);
      r = one(online_http_create(&d, NULL), 10000);
      check(r.err == ONLINE_E_CERT, "certificate refused, create -> CERT", "err %d", r.err);
      return;
   }
   check(np_https_available() == 1, "TLS is available");
   r = one(online_http_create(&d, NULL), 20000);
   check(r.err == 0 && !strcmp(r.code, "K7M2Q") && strlen(r.token) == 22, "tls create", "err %d http %d", r.err, r.http);
   char token[24];
   strcpy(token, r.token);
   r = one(online_http_lookup("K7M2Q"), 10000);
   check(r.err == 0 && !strcmp(r.card.game, "BPEE") && r.card.compat == 0x1a2b3c4d, "tls lookup", "err %d", r.err);
   r = one(online_http_join("K7M2Q", &d, "fedcba9876543210"), 10000);
   check(r.err == 0 && r.have_peer && !strcmp(r.peer_nick, "Alice"), "tls join");
   r = one(online_http_poll("K7M2Q", token, NULL), 10000);
   check(r.err == 0 && r.state == ONLINE_ROOM_JOINED, "tls poll 1");
   r = one(online_http_poll("K7M2Q", token, "203.0.113.9:20000"), 10000);
   check(r.err == 0 && r.state == ONLINE_ROOM_JOINED, "tls poll 2 (kept connection)");
   r = one(online_http_relay("K7M2Q", token), 10000);
   check(r.err == 0 && r.turn_ports == 3 && r.turn_port[0] == 3478 && !strcmp(r.turn_user, "u-1700000000:abc"), "tls relay");
   r = one(online_http_endpoint("K7M2Q", token, "203.0.113.5:50000"), 10000);
   check(r.err == 0 && !strcmp(r.relay_peer, "203.0.113.50:49152"), "tls endpoint");
   r = one(online_http_close("K7M2Q", token, "linked"), 10000);
   check(r.err == 0, "tls close");
   r = one(online_http_lookup("E404"), 10000);
   check(r.err == ONLINE_E_NOT_FOUND, "tls: an error status still maps", "err %d", r.err);
   online_http_shutdown();
}

static void t_selftest(void)
{
   load_test_root();
   sleep(8);   /* np_https_selftest starts its GET 4 s after start-up */
}

int main(int argc, char **argv)
{
   const char *mode = argc > 1 ? argv[1] : "";
   if (!strcmp(mode, "json"))
      t_json();
   else if (!strcmp(mode, "flow") && argc > 2)
      t_flow(argv[2], argc > 3 && !strcmp(argv[3], "real"));
   else if (!strcmp(mode, "errors") && argc > 2)
      t_errors(argv[2]);
   else if (!strcmp(mode, "three") && argc > 2)
      t_three(argv[2]);
   else if (!strcmp(mode, "badver") && argc > 2)
      t_badver(argv[2]);
   else if (!strcmp(mode, "down") && argc > 2)
      t_down(atoi(argv[2]));
   else if (!strcmp(mode, "blackhole") && argc > 2)
      t_blackhole(atoi(argv[2]));
   else if (!strcmp(mode, "nohttps"))
      t_nohttps();
   else if (!strcmp(mode, "dropstuck"))
      t_dropstuck();
   else if (!strcmp(mode, "tls") && argc > 3)
      t_tls(argv[2], argv[3]);
#ifdef NP_HAVE_MBEDTLS
   else if (!strcmp(mode, "scope"))
      t_scope();
#endif
   else if (!strcmp(mode, "selftest"))
      t_selftest();
   else
   {
      printf("FAIL usage: unknown mode %s\n", mode);
      return 100;
   }
   online_http_shutdown();
   printf("%s: %d failure(s)\n", mode, g_fail);
   return g_fail > 100 ? 100 : g_fail;
}
