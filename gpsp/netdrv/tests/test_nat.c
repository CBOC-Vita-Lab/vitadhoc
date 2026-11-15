/* test_nat.c -- the NAT layer's tests (package N's file).
 *
 *   hash      MD5 (RFC 1321), SHA-1 (RFC 3174), HMAC-SHA1 (RFC 2202) vectors
 *   stun      the codec, the retransmission table on a virtual clock, give-up,
 *             `sym`, the first-answer grace, wrong id / wrong source
 *   turn      the 401 / nonce round trip checked with an independent HMAC,
 *             438, quota and auth failures, refresh / permission / channel
 *             schedules, Data indications, ChannelData framing, release
 *   punch     the PUNCH / PUNCH_ACK datagrams on a real loopback socket: the
 *             codec, the unarmed and wrong-key drops, the reply cap
 *   sim       two transports in the RFC 4787 NAT world of tests/natsim/ (the
 *             proxy is built and started by this test): the cone matrix ->
 *             punched, symmetric -> TIMEOUT -> relay, same LAN, a host with
 *             no candidates, the port fallback, echo and bulk through the
 *             relay, release at destroy.  A netdrv session carries 200
 *             reliable payloads each way over every path.
 *
 *   test_nat           everything
 *   test_nat NAME...   only those (hash stun turn punch sim)
 *   NATSIM=/path/to/natsim   use this simulator binary instead of building one
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>

#include "../netdrv.h"
#include "../netdrv_wire.h"
#include "../nd_thread.h"
#include "../transport_lan.h"
#include "../nat_hash.h"
#include "../nat_stun.h"
#include "../nat_turn.h"

static int g_fail;
#define CHECK(cond, ...) do { \
   if (!(cond)) { \
      g_fail++; \
      printf("  FAIL %s:%d: %s -- ", __FILE__, __LINE__, #cond); \
      printf(__VA_ARGS__); \
      printf("\n"); \
   } } while (0)

/* ============================================================ byte helpers == */

static void p16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void p32(uint8_t *p, uint32_t v)
{
   p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static uint32_t g16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }
static uint32_t g32(const uint8_t *p)
{
   return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint32_t ip4(int a, int b, int c, int d)
{
   uint8_t q[4];
   uint32_t v;
   q[0] = (uint8_t)a; q[1] = (uint8_t)b; q[2] = (uint8_t)c; q[3] = (uint8_t)d;
   memcpy(&v, q, 4);                      /* network order: the bytes as they lie */
   return v;
}

static void hex(char *out, const uint8_t *d, size_t n)
{
   size_t i;
   for (i = 0; i < n; i++)
      sprintf(out + 2 * i, "%02x", d[i]);
   out[2 * n] = 0;
}

/* a STUN message under construction (the tests' own, independent of the
 * modules' builders) */
typedef struct tmsg { uint8_t b[1500]; size_t n; } tmsg;

static void tm_init(tmsg *m, uint16_t type, const uint8_t txid[12])
{
   memset(m, 0, sizeof *m);
   p16(m->b, type);
   p32(m->b + 4, 0x2112A442u);
   memcpy(m->b + 8, txid, 12);
   m->n = 20;
}

static void tm_attr(tmsg *m, uint16_t type, const void *v, size_t len)
{
   p16(m->b + m->n, type);
   p16(m->b + m->n + 2, (uint32_t)len);
   if (len)
      memcpy(m->b + m->n + 4, v, len);
   m->n += 4 + ((len + 3) & ~(size_t)3);
   p16(m->b + 2, (uint32_t)(m->n - 20));
}

static void tm_xaddr(tmsg *m, uint16_t type, uint32_t ip_be, uint16_t port)
{
   uint8_t a[8], ip[4];
   memcpy(ip, &ip_be, 4);
   a[0] = 0; a[1] = 1;
   p16(a + 2, port ^ 0x2112u);
   a[4] = ip[0] ^ 0x21; a[5] = ip[1] ^ 0x12; a[6] = ip[2] ^ 0xA4; a[7] = ip[3] ^ 0x42;
   tm_attr(m, type, a, 8);
}

static void tm_error(tmsg *m, int code, const char *realm, const char *nonce)
{
   uint8_t e[4];
   e[0] = e[1] = 0;
   e[2] = (uint8_t)(code / 100);
   e[3] = (uint8_t)(code % 100);
   tm_attr(m, 0x0009, e, 4);
   if (realm)
      tm_attr(m, 0x0014, realm, strlen(realm));
   if (nonce)
      tm_attr(m, 0x0015, nonce, strlen(nonce));
}

/* attribute lookup in a message */
static const uint8_t *tm_find(const uint8_t *d, size_t n, uint16_t type, size_t *len)
{
   size_t o = 20, body = g16(d + 2);
   if (n < 20 || o + body > n)
      return NULL;
   while (o + 4 <= 20 + body)
   {
      size_t l = g16(d + o + 2);
      if (g16(d + o) == type)
      {
         if (len)
            *len = l;
         return d + o + 4;
      }
      o += 4 + ((l + 3) & ~(size_t)3);
   }
   return NULL;
}

/* ===================================================================== hash == */

static void hash_check(const char *what, const uint8_t *got, size_t n, const char *want)
{
   char h[80];
   hex(h, got, n);
   CHECK(!strcmp(h, want), "%s: %s (want %s)", what, h, want);
}

static void test_hash(void)
{
   uint8_t d[20], key[80], big[1000];
   size_t i;
   static const struct { const char *in, *md5; } M[] = {
      { "", "d41d8cd98f00b204e9800998ecf8427e" },
      { "a", "0cc175b9c0f1b6a831c399e269772661" },
      { "abc", "900150983cd24fb0d6963f7d28e17f72" },
      { "message digest", "f96b697d7cb7938d525a2f31aaf161d0" },
      { "abcdefghijklmnopqrstuvwxyz", "c3fcd3d76192e4007dfb496cca67e13b" },
      { "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789",
        "d174ab98d277d9f5a5611c2c9f419d9f" },
      { "12345678901234567890123456789012345678901234567890123456789012345678901234567890",
        "57edf4a22be3c955ac49da2e2107b67a" } };
   static const struct { const char *in, *sha; } S[] = {
      { "abc", "a9993e364706816aba3e25717850c26c9cd0d89d" },
      { "", "da39a3ee5e6b4b0d3255bfef95601890afd80709" },
      { "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
        "84983e441c3bd26ebaae4aa1f95129e5e54670f1" } };

   printf("[hash: MD5, SHA-1, HMAC-SHA1 vectors]\n");
   for (i = 0; i < sizeof M / sizeof M[0]; i++)
   {
      nat_md5(d, M[i].in, strlen(M[i].in));
      hash_check("md5", d, 16, M[i].md5);
   }
   for (i = 0; i < sizeof S / sizeof S[0]; i++)
   {
      nat_sha1(d, S[i].in, strlen(S[i].in));
      hash_check("sha1", d, 20, S[i].sha);
   }
   /* a message that straddles the padding boundary (55, 56, 63, 64, 65 bytes)
    * agrees between a one-shot call and the known value for 'a' * n */
   memset(big, 'a', sizeof big);
   nat_sha1(d, big, 1000);
   hash_check("sha1 a*1000", d, 20, "291e9a6c66994949b57ba5e650361e98fc36b1ba");
   nat_md5(d, big, 1000);
   hash_check("md5 a*1000", d, 16, "cabe45dcc9ae5b66ba86600cca6b8ba8");

   /* RFC 2202 test cases 1..7 for HMAC-SHA1 */
   memset(key, 0x0b, 20);
   nat_hmac_sha1(d, key, 20, "Hi There", 8);
   hash_check("hmac 1", d, 20, "b617318655057264e28bc0b6fb378c8ef146be00");
   nat_hmac_sha1(d, "Jefe", 4, "what do ya want for nothing?", 28);
   hash_check("hmac 2", d, 20, "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79");
   memset(key, 0xaa, 20);
   memset(big, 0xdd, 50);
   nat_hmac_sha1(d, key, 20, big, 50);
   hash_check("hmac 3", d, 20, "125d7342b9ac11cd91a39af48aa17b4f63f175d3");
   for (i = 0; i < 25; i++)
      key[i] = (uint8_t)(i + 1);
   memset(big, 0xcd, 50);
   nat_hmac_sha1(d, key, 25, big, 50);
   hash_check("hmac 4", d, 20, "4c9007f4026250c6bc8414f9bf50c86c2d7235da");
   memset(key, 0x0c, 20);
   nat_hmac_sha1(d, key, 20, "Test With Truncation", 20);
   hash_check("hmac 5", d, 20, "4c1a03424b55e07fe7f27be1d58bb9324a9a5a04");
   memset(key, 0xaa, 80);
   nat_hmac_sha1(d, key, 80, "Test Using Larger Than Block-Size Key - Hash Key First", 54);
   hash_check("hmac 6", d, 20, "aa4ae5e15272d00e95705637ce8a3b55ed402112");
   nat_hmac_sha1(d, key, 80,
                 "Test Using Larger Than Block-Size Key and Larger Than One Block-Size Data", 73);
   hash_check("hmac 7", d, 20, "e8e99d0f45237d786d6bbaa7965c7808bbff1a91");
}

/* ===================================================================== stun == */

typedef struct sendlog
{
   struct { uint64_t t; uint32_t ip; uint16_t port; uint8_t d[1500]; size_t n; } e[64];
   int n;
   uint64_t now;
} sendlog;

static int log_send(void *ctx, uint32_t ip_be, uint16_t port, const void *buf, size_t len)
{
   sendlog *l = (sendlog *)ctx;
   if (l->n < 64 && len <= 1500)
   {
      l->e[l->n].t = l->now;
      l->e[l->n].ip = ip_be;
      l->e[l->n].port = port;
      memcpy(l->e[l->n].d, buf, len);
      l->e[l->n].n = len;
      l->n++;
   }
   return 0;
}

static void stun_answer(tmsg *m, const uint8_t txid[12], uint32_t ip_be, uint16_t port, int plain)
{
   tm_init(m, 0x0101, txid);
   if (plain)
   {
      uint8_t a[8];
      a[0] = 0; a[1] = 1;
      p16(a + 2, port);
      memcpy(a + 4, &ip_be, 4);
      tm_attr(m, 0x0001, a, 8);
   }
   else
      tm_xaddr(m, 0x0020, ip_be, port);
}

static void test_stun(void)
{
   static const uint8_t tx0[12] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 };
   static const uint8_t tx1[12] = { 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32 };
   uint8_t req[20], txo[12], txs[2][12];
   uint32_t ip, sip[2], rip;
   uint16_t port, sport[2], rport;
   tmsg m;
   nat_stun s;
   sendlog l;
   int sym;
   uint64_t t;

   printf("[stun: codec and state machine]\n");
   CHECK(nat_stun_build_request(req, tx0) == 20, "request is 20 bytes");
   CHECK(req[0] == 0 && req[1] == 1 && req[2] == 0 && req[3] == 0 && g32(req + 4) == 0x2112A442u &&
         !memcmp(req + 8, tx0, 12), "request layout");

   /* success with XOR-MAPPED-ADDRESS */
   stun_answer(&m, tx0, ip4(203, 0, 113, 12), 54321, 0);
   CHECK(nat_stun_parse_response(m.b, m.n, txo, &ip, &port) == 1 && ip == ip4(203, 0, 113, 12) &&
         port == 54321 && !memcmp(txo, tx0, 12), "XOR-MAPPED-ADDRESS");
   /* plain MAPPED-ADDRESS */
   stun_answer(&m, tx0, ip4(198, 51, 100, 7), 4000, 1);
   CHECK(nat_stun_parse_response(m.b, m.n, txo, &ip, &port) == 1 && ip == ip4(198, 51, 100, 7) &&
         port == 4000, "MAPPED-ADDRESS fallback");
   /* both: the XOR one wins; an attribute before it (SOFTWARE, padded) is skipped */
   tm_init(&m, 0x0101, tx0);
   tm_attr(&m, 0x8022, "srv", 3);
   {
      uint8_t a[8];
      a[0] = 0; a[1] = 1; p16(a + 2, 1111);
      memcpy(a + 4, &(uint32_t){ ip4(1, 1, 1, 1) }, 4);
      tm_attr(&m, 0x0001, a, 8);
   }
   tm_xaddr(&m, 0x0020, ip4(9, 9, 9, 9), 2222);
   CHECK(nat_stun_parse_response(m.b, m.n, txo, &ip, &port) == 1 && ip == ip4(9, 9, 9, 9) && port == 2222,
         "XOR preferred, other attributes skipped");
   /* an IPv6 mapping is refused */
   tm_init(&m, 0x0101, tx0);
   {
      uint8_t a[20];
      memset(a, 0, sizeof a);
      a[1] = 2;
      tm_attr(&m, 0x0020, a, 20);
   }
   CHECK(nat_stun_parse_response(m.b, m.n, txo, &ip, &port) == 0, "IPv4 only");
   /* an error answer */
   tm_init(&m, 0x0111, tx0);
   tm_error(&m, 420, NULL, NULL);
   CHECK(nat_stun_parse_response(m.b, m.n, txo, &ip, &port) == 2 && !memcmp(txo, tx0, 12), "error answer");
   /* not STUN */
   stun_answer(&m, tx0, ip4(1, 2, 3, 4), 5, 0);
   {
      uint8_t bad[64];
      memcpy(bad, m.b, m.n);
      bad[4] ^= 1;
      CHECK(nat_stun_parse_response(bad, m.n, txo, &ip, &port) == 0, "wrong cookie");
      memcpy(bad, m.b, m.n);
      bad[0] |= 0x40;
      CHECK(nat_stun_parse_response(bad, m.n, txo, &ip, &port) == 0, "ChannelData range is not STUN");
      CHECK(nat_stun_parse_response(m.b, 19, txo, &ip, &port) == 0, "short");
      CHECK(nat_stun_parse_response(m.b, m.n - 4, txo, &ip, &port) == 0, "truncated attributes");
      memcpy(bad, m.b, m.n);
      p16(bad + 2, 7);
      CHECK(nat_stun_parse_response(bad, m.n, txo, &ip, &port) == 0, "length not a multiple of 4");
   }

   /* ---- retransmission table, one server, nobody answers ---- */
   sip[0] = ip4(198, 51, 100, 1); sport[0] = 3478;
   sip[1] = ip4(198, 51, 100, 2); sport[1] = 3478;
   memcpy(txs[0], tx0, 12);
   memcpy(txs[1], tx1, 12);
   memset(&l, 0, sizeof l);
   l.now = 1000;
   nat_stun_start(&s, log_send, &l, sip, sport, 1, (const uint8_t (*)[12])txs, 1000);
   CHECK(nat_stun_state(&s) == LAN_STUN_RUNNING && l.n == 1, "started, first request out at once");
   for (t = 1000; t <= 8000; t++)
   {
      l.now = t;
      nat_stun_tick(&s, t);
      if (nat_stun_state(&s) == LAN_STUN_FAILED)
         break;
   }
   CHECK(nat_stun_state(&s) == LAN_STUN_FAILED, "gave up");
   CHECK(t == 1000 + 5250, "gave up at 5.25 s (t=%llu)", (unsigned long long)(t - 1000));
   CHECK(l.n == 5 && l.e[0].t == 1000 && l.e[1].t == 1250 && l.e[2].t == 1750 && l.e[3].t == 2750 &&
         l.e[4].t == 4250, "five requests at 0 250 750 1750 3250 ms (got %d: %llu %llu %llu %llu %llu)", l.n,
         (unsigned long long)(l.e[0].t - 1000), (unsigned long long)(l.e[1].t - 1000),
         (unsigned long long)(l.e[2].t - 1000), (unsigned long long)(l.e[3].t - 1000),
         (unsigned long long)(l.e[4].t - 1000));
   CHECK(!memcmp(l.e[0].d, l.e[4].d, 20), "a retransmission is the same request");
   CHECK(!nat_stun_result(&s, &ip, &port, &sym), "no result");

   /* ---- one server answers: wrong id, wrong source, then right ---- */
   memset(&l, 0, sizeof l);
   nat_stun_start(&s, log_send, &l, sip, sport, 1, (const uint8_t (*)[12])txs, 0);
   stun_answer(&m, tx1, ip4(203, 0, 113, 10), 4444, 0);          /* not our transaction */
   CHECK(nat_stun_handle(&s, sip[0], 3478, m.b, m.n) == 0 && nat_stun_state(&s) == LAN_STUN_RUNNING,
         "unknown transaction id is not ours");
   stun_answer(&m, tx0, ip4(203, 0, 113, 10), 4444, 0);
   CHECK(nat_stun_handle(&s, sip[1], 3478, m.b, m.n) == 1 && nat_stun_state(&s) == LAN_STUN_RUNNING,
         "right id from the wrong address: consumed, ignored");
   CHECK(nat_stun_handle(&s, sip[0], 3479, m.b, m.n) == 1 && nat_stun_state(&s) == LAN_STUN_RUNNING,
         "right id from the wrong port: consumed, ignored");
   CHECK(nat_stun_handle(&s, sip[0], 3478, m.b, m.n) == 1 && nat_stun_state(&s) == LAN_STUN_OK,
         "answered");
   CHECK(nat_stun_result(&s, &rip, &rport, &sym) && rip == ip4(203, 0, 113, 10) && rport == 4444 && !sym,
         "result");
   CHECK(nat_stun_handle(&s, sip[0], 3478, m.b, m.n) == 1, "a duplicate answer is consumed");
   /* an error answer ends the wait at once */
   nat_stun_start(&s, log_send, &l, sip, sport, 1, (const uint8_t (*)[12])txs, 0);
   tm_init(&m, 0x0111, tx0);
   tm_error(&m, 500, NULL, NULL);
   CHECK(nat_stun_handle(&s, sip[0], 3478, m.b, m.n) == 1 && nat_stun_state(&s) == LAN_STUN_FAILED,
         "an error answer fails it at once");

   /* ---- two servers: same mapping, different mapping, one silent ---- */
   memset(&l, 0, sizeof l);
   nat_stun_start(&s, log_send, &l, sip, sport, 2, (const uint8_t (*)[12])txs, 0);
   CHECK(l.n == 2 && l.e[0].ip == sip[0] && l.e[1].ip == sip[1], "both asked at once");
   stun_answer(&m, tx0, ip4(203, 0, 113, 10), 4444, 0);
   nat_stun_handle(&s, sip[0], 3478, m.b, m.n);
   CHECK(nat_stun_state(&s) == LAN_STUN_RUNNING, "one of two answered: still waiting");
   stun_answer(&m, tx1, ip4(203, 0, 113, 10), 4444, 0);
   nat_stun_handle(&s, sip[1], 3478, m.b, m.n);
   CHECK(nat_stun_state(&s) == LAN_STUN_OK && nat_stun_result(&s, &rip, &rport, &sym) && !sym,
         "same mapping from both: cone");

   nat_stun_start(&s, log_send, &l, sip, sport, 2, (const uint8_t (*)[12])txs, 0);
   stun_answer(&m, tx1, ip4(203, 0, 113, 10), 5555, 0);          /* the SECOND server answers first */
   nat_stun_handle(&s, sip[1], 3478, m.b, m.n);
   stun_answer(&m, tx0, ip4(203, 0, 113, 10), 4444, 0);
   nat_stun_handle(&s, sip[0], 3478, m.b, m.n);
   CHECK(nat_stun_state(&s) == LAN_STUN_OK && nat_stun_result(&s, &rip, &rport, &sym) && sym &&
         rport == 5555, "two different mappings: symmetric; pub is the FIRST answer (%u)", rport);

   /* one server silent: OK 1 s after the first answer; the late one still
    * counts for `sym` */
   memset(&l, 0, sizeof l);
   nat_stun_start(&s, log_send, &l, sip, sport, 2, (const uint8_t (*)[12])txs, 0);
   stun_answer(&m, tx0, ip4(203, 0, 113, 10), 4444, 0);
   nat_stun_tick(&s, 100);
   nat_stun_handle(&s, sip[0], 3478, m.b, m.n);
   nat_stun_tick(&s, 900);
   CHECK(nat_stun_state(&s) == LAN_STUN_RUNNING, "grace: not yet at 800 ms after the answer");
   nat_stun_tick(&s, 1100);
   CHECK(nat_stun_state(&s) == LAN_STUN_OK, "OK 1 s after the first answer");
   nat_stun_tick(&s, 2000);
   stun_answer(&m, tx1, ip4(203, 0, 113, 10), 7777, 0);
   nat_stun_handle(&s, sip[1], 3478, m.b, m.n);
   CHECK(nat_stun_result(&s, &rip, &rport, &sym) && sym && rport == 4444, "a late second answer sets sym");
   for (t = 2000; t < 9000; t++)
      nat_stun_tick(&s, t);
   CHECK(nat_stun_state(&s) == LAN_STUN_OK, "OK stays OK after the schedule ends");
   /* bad arguments */
   nat_stun_start(&s, log_send, &l, sip, sport, 3, (const uint8_t (*)[12])txs, 0);
   CHECK(nat_stun_state(&s) == LAN_STUN_FAILED, "three servers refused");
}

/* ===================================================================== turn == */

static void test_turn(void)
{
   static const char USER[] = "1999999999:alice", PASS[] = "s3cret";
   static const uint8_t seed[16] = { 9, 8, 7, 6, 5, 4, 3, 2, 1, 0, 1, 2, 3, 4, 5, 6 };
   const uint32_t srv = ip4(198, 51, 100, 9);
   const uint32_t rel = ip4(198, 51, 100, 9), peer = ip4(198, 51, 100, 9);
   nat_turn c;
   sendlog l;
   tmsg m;
   uint8_t key[16], mac[20], txid[12];
   char kin[200];
   const uint8_t *v, *inner;
   size_t vl, il;
   uint32_t rip;
   uint16_t rport;
   int i, k;
   uint64_t t = 100000;

   printf("[turn: allocation, credentials, schedules, framing]\n");
   memset(&l, 0, sizeof l);
   l.now = t;
   nat_turn_start(&c, log_send, &l, srv, 3478, USER, PASS, seed, t);
   CHECK(nat_turn_state(&c) == LAN_RELAY_ALLOCATING && l.n == 1, "allocating, request out");
   CHECK(g16(l.e[0].d) == 0x0003 && g32(l.e[0].d + 4) == 0x2112A442u, "an Allocate request");
   v = tm_find(l.e[0].d, l.e[0].n, 0x0019, &vl);
   CHECK(v && vl == 4 && v[0] == 17, "REQUESTED-TRANSPORT UDP");
   v = tm_find(l.e[0].d, l.e[0].n, 0x000D, &vl);
   CHECK(v && vl == 4 && g32(v) == 600, "LIFETIME 600");
   CHECK(!tm_find(l.e[0].d, l.e[0].n, 0x0008, NULL), "the first request carries no integrity");
   CHECK(l.e[0].ip == srv && l.e[0].port == 3478, "sent to the server");

   /* 401 with realm + nonce: the same request again, signed */
   memcpy(txid, l.e[0].d + 8, 12);
   tm_init(&m, 0x0113, txid);
   tm_error(&m, 401, "turn.test", "nonce-one");
   l.n = 0;
   CHECK(nat_turn_handle(&c, m.b, m.n, &inner, &il) == 1, "401 consumed");
   CHECK(l.n == 1 && g16(l.e[0].d) == 0x0003, "Allocate sent again");
   CHECK(memcmp(l.e[0].d + 8, txid, 12) != 0, "with a new transaction id");
   v = tm_find(l.e[0].d, l.e[0].n, 0x0006, &vl);
   CHECK(v && vl == strlen(USER) && !memcmp(v, USER, vl), "USERNAME");
   v = tm_find(l.e[0].d, l.e[0].n, 0x0014, &vl);
   CHECK(v && vl == 9 && !memcmp(v, "turn.test", 9), "REALM");
   v = tm_find(l.e[0].d, l.e[0].n, 0x0015, &vl);
   CHECK(v && vl == 9 && !memcmp(v, "nonce-one", 9), "NONCE");
   /* MESSAGE-INTEGRITY checked with the long-term key built here: the
    * HMAC covers everything before the attribute, with the header's length
    * already counting it (RFC 5389 15.4) */
   snprintf(kin, sizeof kin, "%s:%s:%s", USER, "turn.test", PASS);
   nat_md5(key, kin, strlen(kin));
   v = tm_find(l.e[0].d, l.e[0].n, 0x0008, &vl);
   CHECK(v && vl == 20, "MESSAGE-INTEGRITY present");
   if (v)
   {
      size_t at = (size_t)(v - l.e[0].d) - 4;
      CHECK(at + 24 == l.e[0].n && g16(l.e[0].d + 2) == l.e[0].n - 20, "integrity is last and counted");
      nat_hmac_sha1(mac, key, 16, l.e[0].d, at);
      CHECK(!memcmp(mac, v, 20), "MESSAGE-INTEGRITY verifies with MD5(user:realm:pass)");
   }

   /* 438 stale nonce: new nonce, signed again */
   memcpy(txid, l.e[0].d + 8, 12);
   tm_init(&m, 0x0113, txid);
   tm_error(&m, 438, "turn.test", "nonce-two");
   l.n = 0;
   CHECK(nat_turn_handle(&c, m.b, m.n, &inner, &il) == 1 && l.n == 1, "438: retried");
   v = tm_find(l.e[0].d, l.e[0].n, 0x0015, &vl);
   CHECK(v && vl == 9 && !memcmp(v, "nonce-two", 9), "with the new nonce");
   v = tm_find(l.e[0].d, l.e[0].n, 0x0008, &vl);
   if (v)
   {
      nat_hmac_sha1(mac, key, 16, l.e[0].d, (size_t)(v - l.e[0].d) - 4);
      CHECK(!memcmp(mac, v, 20), "still verifies (the key does not depend on the nonce)");
   }

   /* unknown transaction: not ours */
   tm_init(&m, 0x0103, (const uint8_t *)"abcdefghijkl");
   CHECK(nat_turn_handle(&c, m.b, m.n, &inner, &il) == 0, "unknown transaction id");

   /* success: relayed address and lifetime */
   memcpy(txid, l.e[0].d + 8, 12);
   tm_init(&m, 0x0103, txid);
   tm_xaddr(&m, 0x0016, rel, 50000);
   tm_xaddr(&m, 0x0020, ip4(203, 0, 113, 10), 4444);
   p32((uint8_t[4]){ 0 }, 0);
   {
      uint8_t lt[4];
      p32(lt, 600);
      tm_attr(&m, 0x000D, lt, 4);
   }
   t += 40;
   l.now = t;
   nat_turn_tick(&c, t);
   CHECK(nat_turn_handle(&c, m.b, m.n, &inner, &il) == 1, "Allocate success");
   CHECK(nat_turn_state(&c) == LAN_RELAY_ALLOCATED, "ALLOCATED");
   CHECK(nat_turn_relayed(&c, &rip, &rport) && rip == rel && rport == 50000, "relayed address");
   {
      uint64_t t_alloc = t;
      uint64_t t_bind, t_end;

      CHECK(nat_turn_bind(&c, peer, 50001, t) == 0 && nat_turn_state(&c) == LAN_RELAY_BINDING, "BINDING");
      CHECK(nat_turn_bind(&c, peer, 50002, t) < 0, "a second peer is refused");
      l.n = 0;
      nat_turn_tick(&c, t);
      CHECK(l.n == 2, "CreatePermission and ChannelBind sent (%d)", l.n);
      {
         int perm = -1, bind = -1;
         for (i = 0; i < l.n; i++)
         {
            if (g16(l.e[i].d) == 0x0008) perm = i;
            if (g16(l.e[i].d) == 0x0009) bind = i;
         }
         CHECK(perm >= 0 && bind >= 0, "one of each");
         if (perm >= 0 && bind >= 0)
         {
            v = tm_find(l.e[bind].d, l.e[bind].n, 0x000C, &vl);
            CHECK(v && vl == 4 && g16(v) == 0x4000, "channel 0x4000");
            v = tm_find(l.e[perm].d, l.e[perm].n, 0x0012, &vl);
            CHECK(v && vl == 8 && (uint16_t)(g16(v + 2) ^ 0x2112) == 50001 &&
                  (uint8_t)(v[4] ^ 0x21) == 198 && (uint8_t)(v[5] ^ 0x12) == 51, "XOR-PEER-ADDRESS");
            CHECK(tm_find(l.e[perm].d, l.e[perm].n, 0x0008, NULL), "signed");
            t += 20;
            l.now = t;
            nat_turn_tick(&c, t);
            tm_init(&m, 0x0108, l.e[perm].d + 8);
            nat_turn_handle(&c, m.b, m.n, &inner, &il);
            CHECK(nat_turn_state(&c) == LAN_RELAY_BINDING, "permission alone is not READY");
            tm_init(&m, 0x0109, l.e[bind].d + 8);
            nat_turn_handle(&c, m.b, m.n, &inner, &il);
            CHECK(nat_turn_state(&c) == LAN_RELAY_READY, "READY");
         }
      }
      t_bind = t;

      /* Data indication from the bound peer, and from somebody else */
      memcpy(txid, "dataindic001", 12);
      tm_init(&m, 0x0017, txid);
      tm_xaddr(&m, 0x0012, peer, 50001);
      tm_attr(&m, 0x0013, "hello", 5);
      CHECK(nat_turn_handle(&c, m.b, m.n, &inner, &il) == 2 && il == 5 && !memcmp(inner, "hello", 5) &&
            inner >= m.b && inner < m.b + m.n, "Data indication from the peer: payload points into the datagram");
      tm_init(&m, 0x0017, txid);
      tm_xaddr(&m, 0x0012, peer, 50999);
      tm_attr(&m, 0x0013, "hello", 5);
      CHECK(nat_turn_handle(&c, m.b, m.n, &inner, &il) == 1, "Data indication from another address: consumed, dropped");

      /* schedules: refresh at half the lifetime after ALLOCATE (300 s),
       * permission 180 s after its success, channel 300 s after.  (The
       * module has no clock of its own: an answer is handled at the time of
       * the last tick, as on the I/O thread, which ticks every 20 ms.) */
      l.n = 0;
      l.now = t_bind + 179900;
      nat_turn_tick(&c, t_bind + 179900);
      CHECK(l.n == 0, "no permission refresh at 179.9 s (%d)", l.n);
      l.now = t_bind + 180000;
      nat_turn_tick(&c, t_bind + 180000);
      CHECK(l.n == 1 && g16(l.e[0].d) == 0x0008, "CreatePermission again at 180 s (%d)", l.n);
      tm_init(&m, 0x0108, l.e[0].d + 8);
      nat_turn_handle(&c, m.b, m.n, &inner, &il);
      l.n = 0;
      l.now = t_alloc + 299000;
      nat_turn_tick(&c, t_alloc + 299000);
      CHECK(l.n == 0, "nothing at 299 s (%d)", l.n);
      l.now = t_alloc + 300000;
      nat_turn_tick(&c, t_alloc + 300000);
      CHECK(l.n == 1 && g16(l.e[0].d) == 0x0004, "Refresh at 300 s (%d)", l.n);
      v = tm_find(l.e[0].d, l.e[0].n, 0x000D, &vl);
      CHECK(v && g32(v) == 600, "asking for 600 s again");
      tm_init(&m, 0x0104, l.e[0].d + 8);
      {
         uint8_t lt[4];
         p32(lt, 600);
         tm_attr(&m, 0x000D, lt, 4);
      }
      nat_turn_handle(&c, m.b, m.n, &inner, &il);
      l.n = 0;
      l.now = t_bind + 299900;
      nat_turn_tick(&c, t_bind + 299900);
      CHECK(l.n == 0, "no ChannelBind at 299.9 s (%d)", l.n);
      l.now = t_bind + 300000;
      nat_turn_tick(&c, t_bind + 300000);
      {
         int bind = 0;
         for (i = 0; i < l.n; i++)
            if (g16(l.e[i].d) == 0x0009)
               bind++;
         CHECK(bind == 1, "ChannelBind again at 300 s (%d)", bind);
      }
      CHECK(nat_turn_state(&c) == LAN_RELAY_READY, "still READY while they are renewed");
      t_end = t_bind + 300000;

      /* release: Refresh with lifetime 0, signed, once */
      l.n = 0;
      nat_turn_release(&c, t_end);
      {
         int rf = 0;
         for (i = 0; i < l.n; i++)
            if (g16(l.e[i].d) == 0x0004)
            {
               v = tm_find(l.e[i].d, l.e[i].n, 0x000D, &vl);
               CHECK(v && g32(v) == 0, "lifetime 0");
               CHECK(tm_find(l.e[i].d, l.e[i].n, 0x0008, NULL), "signed");
               rf++;
            }
         CHECK(rf == 1 && l.n == 1, "one release datagram (%d)", l.n);
      }
      CHECK(nat_turn_state(&c) == LAN_RELAY_OFF, "OFF after release");
   }

   /* the request retransmission table: 300, 600, 1200, 1600, 1600 ms, then failed */
   memset(&l, 0, sizeof l);
   t = 5000;
   nat_turn_start(&c, log_send, &l, srv, 3478, USER, PASS, seed, t);
   for (k = 0; k < 7000; k++)
   {
      l.now = t + (uint64_t)k;
      nat_turn_tick(&c, t + (uint64_t)k);
      if (nat_turn_state(&c) == LAN_RELAY_FAILED)
         break;
   }
   CHECK(nat_turn_state(&c) == LAN_RELAY_FAILED && nat_turn_error(&c) == LAN_RELAY_E_TIMEOUT,
         "FAILED / TIMEOUT");
   CHECK(k == 5300, "after 5.3 s (%d ms)", k);
   CHECK(l.n == 5 && l.e[1].t - t == 300 && l.e[2].t - t == 900 && l.e[3].t - t == 2100 &&
         l.e[4].t - t == 3700, "sends at 0 300 900 2100 3700 ms (got %d)", l.n);

   /* failures: 401 after we signed -> AUTH; 486 -> QUOTA; 508 -> QUOTA */
   for (k = 0; k < 3; k++)
   {
      memset(&l, 0, sizeof l);
      nat_turn_start(&c, log_send, &l, srv, 3478, USER, PASS, seed, 0);
      memcpy(txid, l.e[0].d + 8, 12);
      tm_init(&m, 0x0113, txid);
      if (k == 0)
      {
         tm_error(&m, 401, "r", "n1");
         nat_turn_handle(&c, m.b, m.n, &inner, &il);
         memcpy(txid, l.e[l.n - 1].d + 8, 12);
         tm_init(&m, 0x0113, txid);
         tm_error(&m, 401, "r", "n1");                  /* same nonce again: the credentials are wrong */
         nat_turn_handle(&c, m.b, m.n, &inner, &il);
         CHECK(nat_turn_state(&c) == LAN_RELAY_FAILED && nat_turn_error(&c) == LAN_RELAY_E_AUTH,
               "refused sign-in: AUTH");
      }
      else
      {
         tm_error(&m, k == 1 ? 486 : 508, NULL, NULL);
         nat_turn_handle(&c, m.b, m.n, &inner, &il);
         CHECK(nat_turn_state(&c) == LAN_RELAY_FAILED && nat_turn_error(&c) == LAN_RELAY_E_QUOTA,
               "%d: QUOTA", k == 1 ? 486 : 508);
      }
   }
   /* bind before ALLOCATED is refused; a server of port 0 fails at once */
   nat_turn_start(&c, log_send, &l, srv, 3478, USER, PASS, seed, 0);
   CHECK(nat_turn_bind(&c, peer, 1, 0) < 0, "bind before ALLOCATED");
   nat_turn_start(&c, log_send, &l, srv, 0, USER, PASS, seed, 0);
   CHECK(nat_turn_state(&c) == LAN_RELAY_FAILED, "no server port");

   /* ChannelData framing both ways */
   {
      uint8_t out[64];
      const uint8_t *pl;
      size_t pn;
      CHECK(nat_turn_channel_wrap(out, sizeof out, 0x4000, "abcde", 5) == 9 && out[0] == 0x40 && out[1] == 0 &&
            out[2] == 0 && out[3] == 5 && !memcmp(out + 4, "abcde", 5), "wrap: 40 00 | len | payload, no padding");
      CHECK(nat_turn_channel_wrap(out, 8, 0x4000, "abcde", 5) == 0, "wrap: no room");
      CHECK(nat_turn_channel_unwrap(out, 9, &pl, &pn) == 0x4000 && pn == 5 && !memcmp(pl, "abcde", 5), "unwrap");
      CHECK(nat_turn_channel_unwrap(out, 8, &pl, &pn) == 0, "unwrap: truncated");
      out[0] = 0x00;
      CHECK(nat_turn_channel_unwrap(out, 9, &pl, &pn) == 0, "unwrap: a STUN message is not ChannelData");
      CHECK(nat_turn_channel_wrap(out, sizeof out, 0x4000, "", 0) == 4, "empty payload");
   }
}

/* ===================================================================== punch == */

static uint16_t crc16(const uint8_t *p, size_t len)
{
   uint16_t c = 0xFFFF;
   size_t i;
   int b;
   for (i = 0; i < len; i++)
   {
      c ^= (uint16_t)(p[i] << 8);
      for (b = 0; b < 8; b++)
         c = (uint16_t)((c & 0x8000) ? (c << 1) ^ 0x1021 : (c << 1));
   }
   return c;
}

/* A PUNCH or PUNCH_ACK as the plan lays it out (1.8), built here from the
 * table, not by the transport's builder. */
static size_t punch_pkt(uint8_t *b, int type, uint32_t cookie, const uint8_t key[8])
{
   uint16_t c;
   memset(b, 0, 26);
   memcpy(b, "GBLD", 4);
   b[4] = 1;
   b[5] = (uint8_t)type;
   b[6] = 26; b[7] = 0;
   b[8] = (uint8_t)cookie; b[9] = (uint8_t)(cookie >> 8); b[10] = (uint8_t)(cookie >> 16); b[11] = (uint8_t)(cookie >> 24);
   memcpy(b + 16, key, 8);
   c = crc16(b, 24);
   b[24] = (uint8_t)c; b[25] = (uint8_t)(c >> 8);
   return 26;
}

static lan_transport *mk_lo(uint16_t port, int join)
{
   lan_config cfg;
   memset(&cfg, 0, sizeof cfg);
   cfg.mode = join ? LAN_MODE_JOIN : LAN_MODE_HOST;
   cfg.port = port;
   cfg.port_tries = 1;
   cfg.flags = LAN_F_LOOPBACK | LAN_F_NO_DISCOVERY;
   return lan_transport_create(&cfg);
}

static int udp_open(uint16_t *port)
{
   struct sockaddr_in a;
   socklen_t al = sizeof a;
   int fd = socket(AF_INET, SOCK_DGRAM, 0);
   memset(&a, 0, sizeof a);
   a.sin_family = AF_INET;
   a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
   bind(fd, (struct sockaddr *)&a, sizeof a);
   getsockname(fd, (struct sockaddr *)&a, &al);
   *port = ntohs(a.sin_port);
   return fd;
}

static int udp_recv(int fd, uint8_t *b, size_t cap, int ms)
{
   struct pollfd p;
   p.fd = fd; p.events = POLLIN; p.revents = 0;
   if (poll(&p, 1, ms) <= 0)
      return -1;
   return (int)recv(fd, b, cap, 0);
}

static void udp_send(int fd, uint16_t to, const void *b, size_t n)
{
   struct sockaddr_in a;
   memset(&a, 0, sizeof a);
   a.sin_family = AF_INET;
   a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
   a.sin_port = htons(to);
   sendto(fd, b, n, 0, (struct sockaddr *)&a, sizeof a);
}

static void test_punch(void)
{
   static const uint8_t KEY[8] = { 'k', 'e', 'y', '1', '2', '3', '4', '5' };
   static const uint8_t OTHER[8] = { 'k', 'e', 'y', '1', '2', '3', '4', '6' };
   static const uint8_t ENT[16] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
   uint16_t tport = (uint16_t)(30000 + (getpid() % 1000) * 8), cport;
   lan_transport *t;
   lan_nat_status st;
   uint8_t pkt[64], got[256];
   int fd, n, i;

   printf("[punch: PUNCH / PUNCH_ACK on a real socket]\n");
   t = mk_lo(tport, 0);
   CHECK(t != NULL, "transport: %s", lan_last_error());
   if (!t)
      return;
   fd = udp_open(&cport);
   CHECK(lan_transport_punch_arm(t, KEY) < 0, "arming needs the seed first");
   CHECK(lan_transport_stun_start(t, (uint32_t[]){ ip4(127, 0, 0, 1) }, (uint16_t[]){ 9 }, 1) < 0,
         "STUN needs the seed first");
   CHECK(lan_transport_nat_seed(t, ENT) == 0, "seeded");
   CHECK(lan_transport_punch_start(t, NULL, NULL, 0, 1000) < 0, "start needs arm");

   /* unarmed: no answer, counted */
   n = (int)punch_pkt(pkt, 7, 0x11223344, KEY);
   udp_send(fd, tport, pkt, (size_t)n);
   CHECK(udp_recv(fd, got, sizeof got, 300) < 0, "unarmed: ignored");
   lan_transport_nat_status(t, &st);
   CHECK(st.punch == LAN_PUNCH_OFF && st.nat_foreign == 1 && st.punch_rx == 0, "counted as foreign (%u)", st.nat_foreign);

   CHECK(lan_transport_punch_arm(t, KEY) == 0, "armed");
   lan_transport_nat_status(t, &st);
   CHECK(st.punch == LAN_PUNCH_ARMED, "ARMED");
   /* wrong key */
   n = (int)punch_pkt(pkt, 7, 0x11223344, OTHER);
   udp_send(fd, tport, pkt, (size_t)n);
   CHECK(udp_recv(fd, got, sizeof got, 300) < 0, "wrong key: ignored");
   /* damaged CRC */
   n = (int)punch_pkt(pkt, 7, 0x11223344, KEY);
   pkt[25] ^= 0x40;
   udp_send(fd, tport, pkt, (size_t)n);
   CHECK(udp_recv(fd, got, sizeof got, 300) < 0, "bad CRC: ignored");
   /* wrong length field */
   n = (int)punch_pkt(pkt, 7, 0x11223344, KEY);
   udp_send(fd, tport, pkt, 25);
   CHECK(udp_recv(fd, got, sizeof got, 300) < 0, "short: ignored");
   lan_transport_nat_status(t, &st);
   CHECK(st.nat_foreign == 2 && st.punch_rx == 0, "two drops counted (%u)", st.nat_foreign);

   /* the right key: PUNCH_ACK with our cookie, same key, valid CRC */
   n = (int)punch_pkt(pkt, 7, 0xCAFEBABE, KEY);
   udp_send(fd, tport, pkt, (size_t)n);
   n = udp_recv(fd, got, sizeof got, 1000);
   CHECK(n == 26, "PUNCH_ACK is 26 bytes (%d)", n);
   if (n == 26)
   {
      CHECK(!memcmp(got, "GBLD", 4) && got[4] == 1 && got[5] == 8 && got[6] == 26 && got[7] == 0,
            "header: GBLD, version 1, type 8, length 26");
      CHECK(got[8] == 0xBE && got[9] == 0xBA && got[10] == 0xFE && got[11] == 0xCA, "the cookie it answers");
      CHECK(!memcmp(got + 16, KEY, 8), "the key");
      CHECK((uint16_t)(got[24] | got[25] << 8) == crc16(got, 24), "CRC16-CCITT");
   }
   /* armed but not running: answered, and the sender is NOT adopted */
   lan_transport_nat_status(t, &st);
   CHECK(st.punch == LAN_PUNCH_ARMED && st.punch_rx == 1, "still ARMED, one PUNCH counted");

   /* the answer cap: 30 a second, the rest dropped */
   {
      int acks = 0;
      for (i = 0; i < 80; i++)
      {
         punch_pkt(pkt, 7, 1000u + (uint32_t)i, KEY);
         udp_send(fd, tport, pkt, 26);
      }
      while (udp_recv(fd, got, sizeof got, 300) > 0)
         acks++;
      CHECK(acks <= 31 && acks >= 20, "answers capped near 30 a second (%d)", acks);
   }

   /* running with no candidates: the first PUNCH from a stranger makes it
    * a candidate and PUNCHes are sent back to it; its ACK completes the race */
   sleep(1);                                  /* a fresh token bucket */
   while (udp_recv(fd, got, sizeof got, 50) > 0)
      ;
   CHECK(lan_transport_punch_start(t, NULL, NULL, 0, 3000) == 0, "started with no candidates");
   lan_transport_nat_status(t, &st);
   CHECK(st.punch == LAN_PUNCH_RUNNING, "RUNNING");
   n = udp_recv(fd, got, sizeof got, 150);
   CHECK(n < 0, "nobody to send to yet");
   punch_pkt(pkt, 7, 0x01020304, KEY);
   udp_send(fd, tport, pkt, 26);
   {
      uint32_t cookie = 0;
      int saw_ack = 0, saw_punch = 0;
      for (i = 0; i < 4 && !(saw_ack && saw_punch); i++)
      {
         n = udp_recv(fd, got, sizeof got, 500);
         if (n == 26 && got[5] == 8)
            saw_ack = 1;
         if (n == 26 && got[5] == 7)
         {
            saw_punch = 1;
            cookie = (uint32_t)got[8] | (uint32_t)got[9] << 8 | (uint32_t)got[10] << 16 | (uint32_t)got[11] << 24;
         }
      }
      CHECK(saw_ack && saw_punch, "the stranger gets an ACK and a PUNCH of ours (ack %d punch %d)", saw_ack, saw_punch);
      /* an ACK for a cookie we never sent does not finish it */
      punch_pkt(pkt, 8, cookie + 100, KEY);
      udp_send(fd, tport, pkt, 26);
      usleep(150000);
      lan_transport_nat_status(t, &st);
      CHECK(st.punch == LAN_PUNCH_RUNNING, "an ACK with a foreign cookie is not accepted");
      punch_pkt(pkt, 8, cookie, KEY);
      udp_send(fd, tport, pkt, 26);
      usleep(150000);
      lan_transport_nat_status(t, &st);
      CHECK(st.punch == LAN_PUNCH_DONE && st.peer_ip_be == ip4(127, 0, 0, 1) && st.peer_port == cport,
            "DONE, peer = the ACK's source (state %d port %u/%u)", st.punch, st.peer_port, cport);
      CHECK(st.ack_rx == 2 && st.punch_rx >= 2, "counters");
   }
   /* DONE: PUNCH still answered while armed; disarmed: not */
   while (udp_recv(fd, got, sizeof got, 50) > 0)
      ;
   punch_pkt(pkt, 7, 0x77, KEY);
   udp_send(fd, tport, pkt, 26);
   CHECK(udp_recv(fd, got, sizeof got, 500) == 26, "DONE still answers PUNCH");
   lan_transport_punch_stop(t);
   lan_transport_nat_status(t, &st);
   CHECK(st.punch == LAN_PUNCH_OFF, "stop: OFF");
   while (udp_recv(fd, got, sizeof got, 50) > 0)
      ;
   punch_pkt(pkt, 7, 0x78, KEY);
   udp_send(fd, tport, pkt, 26);
   CHECK(udp_recv(fd, got, sizeof got, 300) < 0, "stop also disarms");

   /* the race ends at timeout_ms; the transport stays armed (answers) */
   CHECK(lan_transport_punch_arm(t, KEY) == 0, "re-armed");
   CHECK(lan_transport_punch_start(t, (uint32_t[]){ ip4(127, 0, 0, 1) }, (uint16_t[]){ 9 }, 1, 400) == 0, "started");
   usleep(700000);
   lan_transport_nat_status(t, &st);
   CHECK(st.punch == LAN_PUNCH_TIMEOUT, "TIMEOUT after timeout_ms (state %d)", st.punch);
   CHECK(st.punch_tx >= 3, "rounds were sent (%u)", st.punch_tx);

   close(fd);
   lan_transport_destroy(t);
}

/* ======================================================= the NAT simulator == */

typedef struct proxy
{
   pid_t pid;
   int in_fd, out_fd;
   uint16_t port;
   char buf[8192];
   size_t nbuf;
} proxy;

static char g_sim_path[512];

static int build_sim(void)
{
   const char *env = getenv("NATSIM");
   char cmd[1024], dir[256];
   const char *src = __FILE__;
   char *slash;

   if (g_sim_path[0])
      return 0;
   if (env && *env)
   {
      snprintf(g_sim_path, sizeof g_sim_path, "%s", env);
      return 0;
   }
   snprintf(dir, sizeof dir, "%s", src);
   slash = strrchr(dir, '/');
   if (slash)
      *slash = 0;
   else
      snprintf(dir, sizeof dir, ".");
   snprintf(g_sim_path, sizeof g_sim_path, "/tmp/natsim-test-%d", (int)getpid());
   snprintf(cmd, sizeof cmd, "%s/natsim/build.sh %s", dir, g_sim_path);
   printf("  building the simulator: %s\n", cmd);
   if (system(cmd) != 0)
   {
      printf("  FAIL: the simulator did not build (g++ missing?)\n");
      g_sim_path[0] = 0;
      g_fail++;
      return -1;
   }
   return 0;
}

/* what the simulator printed since the last look, appended to p->buf */
static int proxy_pull(proxy *p)
{
   char tmp[512];
   ssize_t n = read(p->out_fd, tmp, sizeof tmp);
   if (n <= 0)
      return 0;
   if (p->nbuf + (size_t)n >= sizeof p->buf)
      p->nbuf = 0;                          /* never more than a page of chatter: start over */
   memcpy(p->buf + p->nbuf, tmp, (size_t)n);
   p->nbuf += (size_t)n;
   p->buf[p->nbuf] = 0;
   return (int)n;
}

static int proxy_start(proxy *p, uint16_t port, const char *args)
{
   int in[2], out[2];
   char cmd[512];
   int waited;
   memset(p, 0, sizeof *p);
   p->port = port;
   if (build_sim() != 0)
      return -1;
   if (pipe(in) || pipe(out))
      return -1;
   p->pid = fork();
   if (p->pid == 0)
   {
      char *argv[64];
      int argc = 0;
      char *tok, *save = NULL, line[512], portstr[16];
      snprintf(portstr, sizeof portstr, "%u", port);
      snprintf(line, sizeof line, "%s", args);
      argv[argc++] = g_sim_path;
      argv[argc++] = (char *)"--port";
      argv[argc++] = portstr;
      for (tok = strtok_r(line, " ", &save); tok && argc < 60; tok = strtok_r(NULL, " ", &save))
         argv[argc++] = tok;
      argv[argc] = NULL;
      dup2(in[0], 0);
      dup2(out[1], 1);
      close(in[1]); close(out[0]);
      execv(g_sim_path, argv);
      _exit(127);
   }
   (void)cmd;
   close(in[0]);
   close(out[1]);
   p->in_fd = in[1];
   p->out_fd = out[0];
   fcntl(p->out_fd, F_SETFL, fcntl(p->out_fd, F_GETFL, 0) | O_NONBLOCK);
   for (waited = 0; waited < 5000; waited += 20)
   {
      if (proxy_pull(p) > 0)
      {
         if (strstr(p->buf, "ready on"))
         {
            p->nbuf = 0;
            return 0;
         }
      }
      usleep(20000);
   }
   printf("  FAIL: the simulator did not say ready\n");
   g_fail++;
   return -1;
}

/* "stats" -> the text it prints (both lines), or "" */
static const char *proxy_stats(proxy *p)
{
   int waited;
   p->nbuf = 0;
   p->buf[0] = 0;
   if (write(p->in_fd, "stats\n", 6) < 0)
      return "";
   for (waited = 0; waited < 3000; waited += 10)
   {
      if (proxy_pull(p) > 0)
      {
         if (strstr(p->buf, "live "))
            return p->buf;
      }
      usleep(10000);
   }
   return p->buf;
}

static int stats_field(const char *text, const char *name)
{
   const char *s = strstr(text, name);
   if (!s)
      return -1;
   return atoi(s + strlen(name));
}

static void proxy_stop(proxy *p)
{
   if (p->pid > 0)
   {
      kill(p->pid, SIGTERM);
      waitpid(p->pid, NULL, 0);
      close(p->in_fd);
      close(p->out_fd);
      p->pid = 0;
   }
}

/* ---- two consoles: a transport + a netdrv session, as test_lan.c does ---- */

#define MSG_MAX ND_MAX_PAYLOAD

typedef struct console console;
struct console
{
   lan_transport *tp;
   netdrv *nd;
   pthread_t th;
   atomic_int run, active, peers, go;
   atomic_uint rx_ok[ND_MAX_CLIENTS], rx_bad, sent;
   unsigned to_send;
   uint64_t last_send_us;
   int threaded;
};

static size_t msg_len(uint32_t seq)
{
   static const uint16_t L[] = { 16, 36, 104, 8, 240, MSG_MAX, 61, 104 };
   size_t n = L[seq % (sizeof L / sizeof L[0])];
   return n > MSG_MAX ? MSG_MAX : n;
}

static size_t msg_build(uint8_t *b, uint32_t seq, uint8_t from)
{
   size_t n = msg_len(seq), i;
   b[0] = (uint8_t)seq; b[1] = (uint8_t)(seq >> 8); b[2] = (uint8_t)(seq >> 16); b[3] = (uint8_t)(seq >> 24);
   b[4] = from;
   for (i = 5; i < n; i++)
      b[i] = (uint8_t)(seq * 31u + from * 7u + i);
   return n;
}

static void con_deliver(void *user, const void *buf, size_t len, uint8_t src)
{
   console *c = (console *)user;
   uint8_t ref[MSG_MAX];
   if (src < ND_MAX_CLIENTS)
   {
      size_t n = msg_build(ref, atomic_load(&c->rx_ok[src]), src);
      if (len == n && !memcmp(ref, buf, n))
      {
         atomic_fetch_add(&c->rx_ok[src], 1);
         return;
      }
   }
   atomic_fetch_add(&c->rx_bad, 1);
}

static void con_started(void *user, uint8_t id)
{
   (void)id;
   atomic_store(&((console *)user)->active, 1);
}

static void con_stopped(void *user, int reason)
{
   (void)reason;
   atomic_store(&((console *)user)->active, 0);
}

static void *con_thread(void *p)
{
   console *c = (console *)p;
   while (atomic_load(&c->run))
   {
      uint64_t now = nd_now_us();
      netdrv_pump(c->nd, now);
      atomic_store(&c->peers, netdrv_peer_count(c->nd));
      if (atomic_load(&c->go) && netdrv_active(c->nd) && netdrv_peer_count(c->nd) > 0 &&
          atomic_load(&c->sent) < c->to_send && now - c->last_send_us >= 4000u)
      {
         uint8_t b[MSG_MAX];
         uint32_t seq = atomic_load(&c->sent);
         size_t n = msg_build(b, seq, (uint8_t)netdrv_local_id(c->nd));
         if (netdrv_send(c->nd, ND_RELIABLE | ND_FLUSH_HINT, b, n, ND_BROADCAST_ID) == 0)
            atomic_fetch_add(&c->sent, 1);
         c->last_send_us = now;
      }
      nd_sleep_us(1000);
   }
   return NULL;
}

static int con_open(console *c, const char *name, lan_transport *tp, int host)
{
   nd_transport iface;
   nd_callbacks cb;
   nd_config cfg;
   memset(c, 0, sizeof *c);
   c->tp = tp;
   atomic_store(&c->run, 0);
   memset(&iface, 0xA5, sizeof iface);
   lan_transport_iface(tp, &iface);
   memset(&cb, 0, sizeof cb);
   cb.user = c;
   cb.deliver = con_deliver;
   cb.session_started = con_started;
   cb.session_stopped = con_stopped;
   memset(&cfg, 0, sizeof cfg);
   cfg.protocol = "GBAdhoc GB link v2";
   cfg.nick = name;
   cfg.join_nonce = host ? 0x11 : 0x22;
   c->nd = netdrv_create(&iface, &cb, &cfg);
   if (!c->nd || (host ? netdrv_host(c->nd) : netdrv_join(c->nd)))
      return -1;
   atomic_store(&c->run, 1);
   if (pthread_create(&c->th, NULL, con_thread, c) != 0)
      return -1;
   c->threaded = 1;
   return 0;
}

static void con_stop_thread(console *c)
{
   if (c->threaded)
   {
      atomic_store(&c->run, 0);
      pthread_join(c->th, NULL);
      c->threaded = 0;
   }
}

static void con_close(console *c)
{
   con_stop_thread(c);
   if (c->nd)
   {
      netdrv_leave(c->nd);
      netdrv_destroy(c->nd);
      c->nd = NULL;
   }
}

#define WAIT_FOR(ms, cond) do { \
      uint64_t end_ = nd_now_us() + (uint64_t)(ms) * 1000ull; \
      while (!(cond) && nd_now_us() < end_) \
         nd_sleep_us(1000); \
   } while (0)

/* ---- one pair in the simulated world ---- */

typedef struct pair
{
   proxy px;
   lan_transport *t[2];                 /* 0 = host, 1 = joiner */
   uint32_t priv_ip[2];
   int same_lan;
} pair;

static const uint8_t PKEY[8] = { 'r', 'o', 'o', 'm', 'k', 'e', 'y', '1' };

static uint32_t sim_priv(int k, int open, int same_lan)
{
   if (same_lan)
      return ip4(192, 168, 1, 10 + k);
   return open ? ip4(203, 0, 113, 50 + k) : ip4(192, 168, k + 1, 10);
}

static lan_transport *sim_transport(uint16_t port, int k, uint32_t priv, int join)
{
   lan_config cfg;
   uint8_t ent[16];
   lan_transport *t;
   int i;
   memset(&cfg, 0, sizeof cfg);
   cfg.mode = join ? LAN_MODE_JOIN : LAN_MODE_HOST;
   cfg.flags = LAN_F_IO_KEEPALIVE | LAN_F_NO_DISCOVERY;
   cfg.local_ip_be = priv;
   cfg.netmask_be = ip4(255, 255, 255, 0);
   cfg.sim_ip_be = ip4(127, 0, 0, 1);
   cfg.sim_port = port;
   cfg.sim_id = (uint8_t)k;
   cfg.sim_lan_port = LAN_PORT_DEFAULT;
   t = lan_transport_create(&cfg);
   if (!t)
      return NULL;
   for (i = 0; i < 16; i++)
      ent[i] = (uint8_t)(rand() ^ (k * 77) ^ (i * 13));
   lan_transport_nat_seed(t, ent);
   return t;
}

static int pair_open(pair *p, uint16_t port, const char *nat0, const char *nat1, int same_lan, int turn)
{
   char args[256];
   int open0 = !strcmp(nat0, "open"), open1 = !strcmp(nat1, "open");
   memset(p, 0, sizeof *p);
   p->same_lan = same_lan;
   snprintf(args, sizeof args, "--nat0 %s --nat1 %s%s%s", nat0, nat1, same_lan ? " --same-lan" : "",
            turn ? " --turn" : "");
   if (proxy_start(&p->px, port, args) != 0)
      return -1;
   p->priv_ip[0] = sim_priv(0, open0, same_lan);
   p->priv_ip[1] = sim_priv(1, open1, same_lan);
   p->t[0] = sim_transport(port, 0, p->priv_ip[0], 0);
   p->t[1] = sim_transport(port, 1, p->priv_ip[1], 1);
   if (!p->t[0] || !p->t[1])
   {
      printf("  FAIL: transports: %s\n", lan_last_error());
      g_fail++;
      return -1;
   }
   return 0;
}

static void pair_close(pair *p)
{
   if (p->t[1]) lan_transport_destroy(p->t[1]);
   if (p->t[0]) lan_transport_destroy(p->t[0]);
   p->t[0] = p->t[1] = NULL;
   proxy_stop(&p->px);
}

/* STUN on both sides; the answer is each side's public address. */
static int pair_stun(pair *p, int want_sym0, int want_sym1)
{
   static const uint32_t SIP[2] = { 0, 0 };
   uint32_t ip[2];
   uint16_t port[2] = { 3478, 3478 };
   lan_nat_status st[2];
   int k;
   (void)SIP;
   ip[0] = ip4(198, 51, 100, 1);
   ip[1] = ip4(198, 51, 100, 2);
   for (k = 0; k < 2; k++)
      CHECK(lan_transport_stun_start(p->t[k], ip, port, 2) == 0, "stun_start %d", k);
   WAIT_FOR(8000, ({ lan_nat_status a, b; lan_transport_nat_status(p->t[0], &a); lan_transport_nat_status(p->t[1], &b);
                     a.stun != LAN_STUN_RUNNING && b.stun != LAN_STUN_RUNNING; }));
   /* the second server's answer may still be in flight at OK */
   usleep(300000);
   for (k = 0; k < 2; k++)
      lan_transport_nat_status(p->t[k], &st[k]);
   for (k = 0; k < 2; k++)
   {
      CHECK(st[k].stun == LAN_STUN_OK, "side %d STUN OK (%d)", k, st[k].stun);
      CHECK(st[k].pub_port != 0 && st[k].stun_rx >= 1, "public address known");
   }
   CHECK(st[0].sym == want_sym0 && st[1].sym == want_sym1, "sym detection %d/%d (want %d/%d)", st[0].sym,
         st[1].sym, want_sym0, want_sym1);
   return st[0].stun == LAN_STUN_OK && st[1].stun == LAN_STUN_OK ? 0 : -1;
}

/* Both sides punch at once; the candidates are the other's public address
 * (and its LAN address when `lan`). */
static void pair_punch_start(pair *p, int with_lan, int host_blind, unsigned timeout_ms)
{
   lan_nat_status st[2];
   uint32_t cip[2][2];
   uint16_t cport[2][2];
   int n[2], k;
   for (k = 0; k < 2; k++)
      lan_transport_nat_status(p->t[k], &st[k]);
   for (k = 0; k < 2; k++)
   {
      int o = 1 - k;
      n[k] = 0;
      cip[k][n[k]] = st[o].pub_ip_be;
      cport[k][n[k]++] = st[o].pub_port;
      if (with_lan)
      {
         cip[k][n[k]] = p->priv_ip[o];
         cport[k][n[k]++] = LAN_PORT_DEFAULT;
      }
   }
   for (k = 0; k < 2; k++)
      CHECK(lan_transport_punch_arm(p->t[k], PKEY) == 0, "arm %d", k);
   CHECK(lan_transport_punch_start(p->t[0], cip[0], cport[0], host_blind ? 0 : n[0], timeout_ms) == 0, "host punch");
   CHECK(lan_transport_punch_start(p->t[1], cip[1], cport[1], n[1], timeout_ms) == 0, "joiner punch");
}

static int pair_wait_punch(pair *p, int ms, int want_state)
{
   lan_nat_status a, b;
   uint64_t end = nd_now_us() + (uint64_t)ms * 1000ull;
   do
   {
      lan_transport_nat_status(p->t[0], &a);
      lan_transport_nat_status(p->t[1], &b);
      if (a.punch == want_state && b.punch == want_state)
         return 0;
      nd_sleep_us(5000);
   } while (nd_now_us() < end);
   return -1;
}

/* A netdrv session over the paths the transports now have: 200 reliable
 * payloads each way, in order and intact. */
static void pair_session(pair *p, const char *what, uint32_t host_addr_ip, uint16_t host_addr_port)
{
   console H, J;
   lan_census cen;
   CHECK(lan_transport_set_target(p->t[1], host_addr_ip, host_addr_port) == 0, "%s: set_target", what);
   CHECK(con_open(&H, "host", p->t[0], 1) == 0, "H");
   CHECK(con_open(&J, "join", p->t[1], 0) == 0, "J");
   WAIT_FOR(10000, atomic_load(&J.active) && atomic_load(&H.peers) == 1 && atomic_load(&J.peers) == 1);
   CHECK(atomic_load(&J.active) && atomic_load(&H.peers) == 1, "%s: session up", what);
   H.to_send = J.to_send = 200;
   atomic_store(&H.go, 1);
   atomic_store(&J.go, 1);
   WAIT_FOR(20000, atomic_load(&J.rx_ok[0]) == 200 && atomic_load(&H.rx_ok[1]) == 200);
   CHECK(atomic_load(&J.rx_ok[0]) == 200 && atomic_load(&H.rx_ok[1]) == 200,
         "%s: 200 payloads each way (host->joiner %u, joiner->host %u)", what, atomic_load(&J.rx_ok[0]),
         atomic_load(&H.rx_ok[1]));
   CHECK(atomic_load(&H.rx_bad) == 0 && atomic_load(&J.rx_bad) == 0, "%s: intact", what);
   lan_transport_census(p->t[1], &cen, 0);
   printf("  %s: census path \"%s\"\n", what, cen.path);
   con_stop_thread(&H);
   con_stop_thread(&J);
   con_close(&J);
   con_close(&H);
}

static const char *census_path(lan_transport *t)
{
   static char s[16];
   lan_census c;
   lan_transport_census(t, &c, 0);
   snprintf(s, sizeof s, "%s", c.path);
   return s;
}

static void test_punch_matrix(uint16_t base)
{
   static const char *NAT[3] = { "full", "addr", "port" };
   int a, b, idx = 0;
   printf("[sim: cone x cone punches through, and a session runs over it]\n");
   for (a = 0; a < 3; a++)
      for (b = 0; b < 3; b++)
      {
         pair p;
         char what[64];
         lan_nat_status s0, s1;
         snprintf(what, sizeof what, "%s x %s", NAT[a], NAT[b]);
         if (pair_open(&p, (uint16_t)(base + idx++), NAT[a], NAT[b], 0, 0) != 0)
         {
            pair_close(&p);
            continue;
         }
         if (pair_stun(&p, 0, 0) == 0)
         {
            pair_punch_start(&p, 0, 0, 4000);
            CHECK(pair_wait_punch(&p, 5000, LAN_PUNCH_DONE) == 0, "%s: DONE on both sides", what);
            lan_transport_nat_status(p.t[0], &s0);
            lan_transport_nat_status(p.t[1], &s1);
            CHECK(s0.peer_ip_be == s1.pub_ip_be && s0.peer_port == s1.pub_port && !s0.peer_is_lan,
                  "%s: the host's peer is the joiner's public address", what);
            CHECK(s1.peer_ip_be == s0.pub_ip_be && s1.peer_port == s0.pub_port && !s1.peer_is_lan,
                  "%s: the joiner's peer is the host's public address", what);
            CHECK(s0.ack_rx >= 1 && s1.ack_rx >= 1 && s0.punch_tx >= 1, "%s: counters", what);
            pair_session(&p, what, s1.peer_ip_be, s1.peer_port);
            CHECK(!strcmp(census_path(p.t[1]), "punch"), "%s: census path is \"punch\" (%s)", what,
                  census_path(p.t[1]));
         }
         pair_close(&p);
      }
}

/* A fresh pair with `turn`: both relay, bind to each other, run a session. */
static void b64(char *out, const uint8_t *d, size_t n)
{
   static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
   size_t i, o = 0;
   for (i = 0; i < n; i += 3)
   {
      uint32_t v = (uint32_t)d[i] << 16 | (i + 1 < n ? (uint32_t)d[i + 1] << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);
      out[o++] = T[v >> 18 & 63];
      out[o++] = T[v >> 12 & 63];
      out[o++] = i + 1 < n ? T[v >> 6 & 63] : '=';
      out[o++] = i + 2 < n ? T[v & 63] : '=';
   }
   out[o] = 0;
}

/* The TURN REST credential the simulator expects (secret "turn-test-secret"). */
static void rest_cred(int k, char *user, size_t un, char *pass)
{
   uint8_t mac[20];
   snprintf(user, un, "%lu:player%d", (unsigned long)time(NULL) + 3600ul, k);
   nat_hmac_sha1(mac, "turn-test-secret", 16, user, strlen(user));
   b64(pass, mac, 20);
}

static int pair_relay(pair *p, const uint16_t *ports, int n_ports, int ms_allocated)
{
   uint32_t srv = ip4(198, 51, 100, 9);
   char user[64], pass[64];
   lan_nat_status s[2];
   int k, ok = 1;
   for (k = 0; k < 2; k++)
   {
      rest_cred(k, user, sizeof user, pass);
      CHECK(lan_transport_relay_start(p->t[k], srv, ports, n_ports, user, pass) == 0, "relay_start %d", k);
   }
   WAIT_FOR(ms_allocated, ({ lan_nat_status a, b; lan_transport_nat_status(p->t[0], &a); lan_transport_nat_status(p->t[1], &b);
                             a.relay == LAN_RELAY_ALLOCATED && b.relay == LAN_RELAY_ALLOCATED; }));
   for (k = 0; k < 2; k++)
      lan_transport_nat_status(p->t[k], &s[k]);
   for (k = 0; k < 2; k++)
   {
      CHECK(s[k].relay == LAN_RELAY_ALLOCATED, "side %d ALLOCATED (%d, error %d)", k, s[k].relay, s[k].relay_err);
      if (s[k].relay != LAN_RELAY_ALLOCATED)
         ok = 0;
   }
   if (!ok)
      return -1;
   CHECK(s[0].relay_ip_be == srv && s[1].relay_ip_be == srv && s[0].relay_port != s[1].relay_port,
         "two relayed addresses on the server");
   /* the service would hand each side the other's relayed address */
   CHECK(lan_transport_relay_bind(p->t[0], s[1].relay_ip_be, s[1].relay_port) == 0, "bind host");
   CHECK(lan_transport_relay_bind(p->t[1], s[0].relay_ip_be, s[0].relay_port) == 0, "bind joiner");
   WAIT_FOR(5000, ({ lan_nat_status a, b; lan_transport_nat_status(p->t[0], &a); lan_transport_nat_status(p->t[1], &b);
                     a.relay == LAN_RELAY_READY && b.relay == LAN_RELAY_READY; }));
   for (k = 0; k < 2; k++)
   {
      lan_transport_nat_status(p->t[k], &s[k]);
      CHECK(s[k].relay == LAN_RELAY_READY, "side %d READY (%d)", k, s[k].relay);
      if (s[k].relay != LAN_RELAY_READY)
         ok = 0;
   }
   return ok ? 0 : -1;
}

static void test_relay_pair(uint16_t base, const char *nat0, const char *nat1)
{
   pair p;
   lan_nat_status s[2];
   char what[64];
   const uint16_t ports[2] = { 3478, 53 };
   snprintf(what, sizeof what, "%s x %s", nat0, nat1);
   if (pair_open(&p, base, nat0, nat1, 0, 1) != 0)
   {
      pair_close(&p);
      return;
   }
   if (pair_stun(&p, !strcmp(nat0, "sym"), !strcmp(nat1, "sym")) == 0)
   {
      pair_punch_start(&p, 0, 0, 2500);
      CHECK(pair_wait_punch(&p, 4000, LAN_PUNCH_TIMEOUT) == 0, "%s: the punch times out", what);
      lan_transport_nat_status(p.t[0], &s[0]);
      lan_transport_nat_status(p.t[1], &s[1]);
      CHECK(s[0].punch == LAN_PUNCH_TIMEOUT && s[1].punch == LAN_PUNCH_TIMEOUT, "%s: TIMEOUT on both", what);
      if (pair_relay(&p, ports, 2, 6000) == 0)
      {
         lan_nat_status r0, r1;
         uint8_t mac[6], from[6], buf[LAN_BULK_MAX];
         uint32_t mn, mx, last;
         int i, n = 0;
         lan_transport_nat_status(p.t[0], &r0);
         lan_transport_nat_status(p.t[1], &r1);
         pair_session(&p, what, r0.relay_ip_be, r0.relay_port);
         CHECK(!strcmp(census_path(p.t[1]), "relay"), "%s: census path is \"relay\" (%s)", what, census_path(p.t[1]));
         lan_transport_nat_status(p.t[1], &s[1]);
         CHECK(s[1].relay_tx > 100 && s[1].relay_rx > 100, "%s: the data went through the relay (tx %u rx %u)", what,
               s[1].relay_tx, s[1].relay_rx);
         /* echo: the other console's I/O thread answers through the relay */
         lan_mac_from_addr(r0.relay_ip_be, r0.relay_port, mac);
         lan_transport_echo_reset(p.t[1]);
         for (i = 0; i < 5; i++)
         {
            lan_transport_echo(p.t[1], mac);
            usleep(20000);
         }
         usleep(300000);
         n = lan_transport_echo_result(p.t[1], &mn, &mx, &last);
         CHECK(n == 5, "%s: echo round trips through the relay (%d of 5)", what, n);
         /* bulk lane: recv_from names the relayed source */
         lan_transport_bulk_any(p.t[0], 1);
         CHECK(lan_transport_bulk_open(p.t[0], 16, "TEST") == 0 && lan_transport_bulk_open(p.t[1], 16, "TEST") == 0, "bulk");
         memset(buf, 0x33, 100);
         memcpy(buf, "TEST", 4);
         lan_mac_from_addr(r1.relay_ip_be, r1.relay_port, mac);
         CHECK(lan_transport_bulk_send(p.t[1], (uint8_t[6]){ 0 }, buf, 100) == 0, "send to nowhere is harmless");
         lan_mac_from_addr(r0.relay_ip_be, r0.relay_port, mac);
         CHECK(lan_transport_bulk_send(p.t[1], mac, buf, 100) == 0, "bulk send");
         usleep(400000);
         memset(from, 0, 6);
         n = lan_transport_bulk_recv_from(p.t[0], from, buf, sizeof buf);
         lan_mac_from_addr(r1.relay_ip_be, r1.relay_port, mac);
         CHECK(n == 100 && !memcmp(from, mac, 6), "%s: bulk_recv_from returns the relayed source (%d bytes)", what, n);
      }
      /* destroy releases the allocations */
      {
         lan_transport_destroy(p.t[1]);
         lan_transport_destroy(p.t[0]);
         p.t[0] = p.t[1] = NULL;
         usleep(300000);
         {
            const char *st = proxy_stats(&p.px);
            CHECK(stats_field(st, "live ") == 0, "%s: destroy released both allocations (%s)", what, st);
            CHECK(stats_field(st, "releases ") >= 2, "two releases counted");
         }
      }
   }
   pair_close(&p);
}

static void test_blind_host(uint16_t base)
{
   pair p;
   lan_nat_status s0, s1;
   printf("[sim: a host with no candidates is opened by the joiner's PUNCH]\n");
   if (pair_open(&p, base, "full", "full", 0, 0) != 0)
   {
      pair_close(&p);
      return;
   }
   if (pair_stun(&p, 0, 0) == 0)
   {
      pair_punch_start(&p, 0, 1, 4000);
      CHECK(pair_wait_punch(&p, 5000, LAN_PUNCH_DONE) == 0, "DONE on both sides");
      lan_transport_nat_status(p.t[0], &s0);
      lan_transport_nat_status(p.t[1], &s1);
      CHECK(s0.peer_ip_be == s1.pub_ip_be && s0.peer_port == s1.pub_port, "the host learned the joiner's address from its PUNCH");
      pair_session(&p, "blind host", s1.peer_ip_be, s1.peer_port);
   }
   pair_close(&p);
}

static void test_same_lan(uint16_t base)
{
   pair p;
   lan_nat_status s0, s1;
   printf("[sim: same LAN, both candidates]\n");
   if (pair_open(&p, base, "port", "port", 1, 0) != 0)
   {
      pair_close(&p);
      return;
   }
   if (pair_stun(&p, 0, 0) == 0)
   {
      pair_punch_start(&p, 1, 0, 4000);
      CHECK(pair_wait_punch(&p, 5000, LAN_PUNCH_DONE) == 0, "DONE on both sides");
      lan_transport_nat_status(p.t[0], &s0);
      lan_transport_nat_status(p.t[1], &s1);
      CHECK(s0.peer_is_lan && s1.peer_is_lan, "peer_is_lan on both (%d %d)", s0.peer_is_lan, s1.peer_is_lan);
      CHECK(s1.peer_ip_be == p.priv_ip[0] && s1.peer_port == LAN_PORT_DEFAULT, "the joiner reaches the host's LAN address");
      pair_session(&p, "same LAN", s1.peer_ip_be, s1.peer_port);
      CHECK(!strcmp(census_path(p.t[1]), "lan"), "census path is \"lan\" (%s)", census_path(p.t[1]));
   }
   pair_close(&p);
}

static void test_open_nat(uint16_t base)
{
   pair p;
   lan_nat_status s0;
   printf("[sim: open (no NAT) on one side]\n");
   if (pair_open(&p, base, "open", "port", 0, 0) != 0)
   {
      pair_close(&p);
      return;
   }
   if (pair_stun(&p, 0, 0) == 0)
   {
      lan_transport_nat_status(p.t[0], &s0);
      CHECK(s0.open, "the host's mapped address is its own: open");
      pair_punch_start(&p, 0, 0, 4000);
      CHECK(pair_wait_punch(&p, 5000, LAN_PUNCH_DONE) == 0, "DONE");
   }
   pair_close(&p);
}

static void test_relay_ports(uint16_t base)
{
   pair p;
   const uint16_t ports[2] = { 443, 3478 };       /* 443 is not served by the simulator: silence */
   uint64_t t0;
   lan_nat_status s;
   printf("[sim: the first TURN port is silent; 3 s later the next one answers]\n");
   if (pair_open(&p, base, "sym", "sym", 0, 1) != 0)
   {
      pair_close(&p);
      return;
   }
   t0 = nd_now_us();
   if (pair_relay(&p, ports, 2, 9000) == 0)
   {
      uint64_t dt = (nd_now_us() - t0) / 1000u;
      CHECK(dt >= 3000 && dt < 8000, "moved to the second port after about 3 s (%llu ms)", (unsigned long long)dt);
      lan_transport_nat_status(p.t[0], &s);
      pair_session(&p, "sym x sym via 443,3478", s.relay_ip_be, s.relay_port);
   }
   pair_close(&p);
}

static void test_relay_fail(uint16_t base)
{
   pair p;
   const uint16_t ports[1] = { 4444 };
   lan_nat_status s;
   char user[64], pass[64];
   printf("[sim: no TURN answer at all -> FAILED / TIMEOUT; bad credentials -> AUTH]\n");
   if (pair_open(&p, base, "port", "port", 0, 1) != 0)
   {
      pair_close(&p);
      return;
   }
   rest_cred(0, user, sizeof user, pass);
   CHECK(lan_transport_relay_start(p.t[0], ip4(198, 51, 100, 9), ports, 1, user, pass) == 0, "start");
   WAIT_FOR(9000, ({ lan_nat_status a; lan_transport_nat_status(p.t[0], &a); a.relay == LAN_RELAY_FAILED; }));
   lan_transport_nat_status(p.t[0], &s);
   CHECK(s.relay == LAN_RELAY_FAILED && s.relay_err == LAN_RELAY_E_TIMEOUT, "FAILED / TIMEOUT (%d / %d)", s.relay, s.relay_err);
   {
      const uint16_t good[1] = { 3478 };
      CHECK(lan_transport_relay_start(p.t[1], ip4(198, 51, 100, 9), good, 1, user, "wrong-password") == 0, "start with a wrong password");
      WAIT_FOR(6000, ({ lan_nat_status a; lan_transport_nat_status(p.t[1], &a); a.relay == LAN_RELAY_FAILED; }));
      lan_transport_nat_status(p.t[1], &s);
      CHECK(s.relay == LAN_RELAY_FAILED && s.relay_err == LAN_RELAY_E_AUTH, "FAILED / AUTH (%d / %d)", s.relay, s.relay_err);
   }
   pair_close(&p);
}

static void test_rearm(uint16_t base)
{
   pair p;
   const uint16_t ports[1] = { 3478 };
   lan_nat_status s[2];
   uint32_t srv = ip4(198, 51, 100, 9);
   char user[64], pass[64];
   printf("[sim: the host's transport serves a second guest: relay_stop + relay_start, punch_stop + punch_arm]\n");
   if (pair_open(&p, base, "full", "full", 0, 1) != 0)
   {
      pair_close(&p);
      return;
   }
   if (pair_stun(&p, 0, 0) == 0)
   {
      int round;
      for (round = 0; round < 2; round++)
      {
         pair_punch_start(&p, 0, 0, 4000);
         CHECK(pair_wait_punch(&p, 5000, LAN_PUNCH_DONE) == 0, "round %d: punch DONE", round);
         lan_transport_punch_stop(p.t[0]);
         lan_transport_punch_stop(p.t[1]);
         lan_transport_nat_status(p.t[0], &s[0]);
         CHECK(s[0].punch == LAN_PUNCH_OFF, "stopped");
      }
      CHECK(lan_transport_punch_arm(p.t[0], PKEY) == 0, "arm again");
      lan_transport_nat_status(p.t[0], &s[0]);
      CHECK(s[0].punch == LAN_PUNCH_ARMED && s[0].peer_port == 0, "ARMED, the old peer forgotten");
      lan_transport_punch_stop(p.t[0]);
   }
   if (pair_relay(&p, ports, 1, 6000) == 0)
   {
      lan_transport_relay_stop(p.t[0]);
      lan_transport_nat_status(p.t[0], &s[0]);
      CHECK(s[0].relay == LAN_RELAY_OFF, "relay OFF after stop");
      rest_cred(5, user, sizeof user, pass);
      CHECK(lan_transport_relay_start(p.t[0], srv, ports, 1, user, pass) == 0, "relay_start again on the same transport");
      WAIT_FOR(8000, ({ lan_nat_status a; lan_transport_nat_status(p.t[0], &a); a.relay == LAN_RELAY_ALLOCATED; }));
      lan_transport_nat_status(p.t[0], &s[0]);
      lan_transport_nat_status(p.t[1], &s[1]);
      CHECK(s[0].relay == LAN_RELAY_ALLOCATED, "ALLOCATED again (%d, error %d)", s[0].relay, s[0].relay_err);
      CHECK(lan_transport_relay_bind(p.t[0], s[1].relay_ip_be, s[1].relay_port) == 0, "bind again");
      WAIT_FOR(5000, ({ lan_nat_status a; lan_transport_nat_status(p.t[0], &a); a.relay == LAN_RELAY_READY; }));
      lan_transport_nat_status(p.t[0], &s[0]);
      CHECK(s[0].relay == LAN_RELAY_READY, "READY again (%d)", s[0].relay);
   }
   pair_close(&p);
}

static void test_sim(void)
{
   uint16_t base = (uint16_t)(36000 + (getpid() % 40) * 100);
   test_punch_matrix(base);                              /* 9 ports */
   test_blind_host((uint16_t)(base + 10));
   test_same_lan((uint16_t)(base + 11));
   test_open_nat((uint16_t)(base + 12));
   printf("[sim: port-restricted x symmetric -> punch times out -> relay]\n");
   test_relay_pair((uint16_t)(base + 13), "port", "sym");
   printf("[sim: symmetric x symmetric -> punch times out -> relay]\n");
   test_relay_pair((uint16_t)(base + 14), "sym", "sym");
   test_relay_ports((uint16_t)(base + 15));
   test_relay_fail((uint16_t)(base + 16));
   test_rearm((uint16_t)(base + 17));
   if (g_sim_path[0] && !getenv("NATSIM"))
      unlink(g_sim_path);
}

/* ====================================================================== main == */

static const struct { const char *name; void (*fn)(void); } TESTS[] = {
   { "hash", test_hash },
   { "stun", test_stun },
   { "turn", test_turn },
   { "punch", test_punch },
   { "sim", test_sim },
};

int main(int argc, char **argv)
{
   unsigned i;
   int a, ran = 0;
   setvbuf(stdout, NULL, _IOLBF, 0);
   signal(SIGPIPE, SIG_IGN);
   (void)nd_crc16(0xFFFF, "", 0);          /* netdrv's table is built on first use: before any thread */
   srand((unsigned)time(NULL) ^ (unsigned)getpid());
   for (i = 0; i < sizeof TESTS / sizeof TESTS[0]; i++)
   {
      int want = argc < 2;
      for (a = 1; a < argc; a++)
         if (!strcmp(argv[a], TESTS[i].name))
            want = 1;
      if (want)
      {
         TESTS[i].fn();
         ran++;
      }
   }
   if (!ran)
   {
      printf("no such test\n");
      return 2;
   }
   if (g_fail)
   {
      printf("nat: %d FAILED\n", g_fail);
      return 1;
   }
   printf("ALL NAT TESTS PASSED\n");
   return 0;
}
