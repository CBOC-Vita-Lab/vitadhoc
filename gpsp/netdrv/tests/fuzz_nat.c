/* SPDX-License-Identifier: MIT OR GPL-2.0-or-later */
/* fuzz_nat.c -- malformed-input test of the STUN / TURN codecs (nat_stun.c, nat_turn.c): what a server, or anybody
 * who can put a datagram on the wire with a server's address, can hand them.  2.0 security review.
 *
 *   make fuzz-nat                (AddressSanitizer + UBSan, about 400,000 datagrams, a few seconds, no sockets)
 *   ./fuzz_nat [rounds] [seed]
 *
 * Real answers are built here (a Binding success, a 401 with realm and nonce, a signed Allocate / Refresh /
 * CreatePermission / ChannelBind success, error answers, a Data indication, ChannelData), then damaged: bytes
 * flipped, cut short, lengths and attribute lengths changed, attributes repeated; a share of the damaged answers
 * is signed again with the right key (a server that lies).  Every datagram lives in a heap block of exactly its
 * length, so a read one byte past it stops the run.  The pass conditions: no sanitizer report, no hang, every
 * pointer handed back lies inside the datagram, the client's strings stay terminated, its state stays a known one. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nat_hash.h"
#include "nat_stun.h"
#include "nat_turn.h"

static uint32_t g_x = 0x2545F491u;
static uint32_t rnd(void) { g_x ^= g_x << 13; g_x ^= g_x >> 17; g_x ^= g_x << 5; return g_x; }

static int g_fail;
#define CHECK(c, what) do { if (!(c)) { g_fail++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, what); } } while (0)

/* ---- what the client sent last (its transaction id is what an answer needs) ---- */
static uint8_t g_last[700];
static size_t g_last_n;
static int cap_send(void *ctx, uint32_t ip, uint16_t port, const void *buf, size_t len)
{
   (void)ctx; (void)ip; (void)port;
   g_last_n = len < sizeof g_last ? len : sizeof g_last;
   memcpy(g_last, buf, g_last_n);
   return 0;
}

/* ---- a message under construction ---- */
typedef struct { uint8_t b[900]; size_t n; } msg;
static void p16(uint8_t *p, unsigned v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void p32(uint8_t *p, uint32_t v) { p16(p, v >> 16); p16(p + 2, v & 0xFFFF); }
static void m_init(msg *m, unsigned type, const uint8_t txid[12])
{
   memset(m, 0, sizeof *m);
   p16(m->b, type); p32(m->b + 4, 0x2112A442u);
   memcpy(m->b + 8, txid, 12);
   m->n = 20;
}
static void m_attr(msg *m, unsigned type, const void *v, size_t len)
{
   size_t pad = (len + 3) & ~(size_t)3;
   if (m->n + 4 + pad > sizeof m->b - 32)
      return;
   p16(m->b + m->n, type); p16(m->b + m->n + 2, (unsigned)len);
   if (len) memcpy(m->b + m->n + 4, v, len);
   m->n += 4 + pad;
   p16(m->b + 2, (unsigned)(m->n - 20));
}
static void m_addr(msg *m, unsigned type, const uint8_t ip[4], unsigned port)
{
   uint8_t t[8] = { 0, 1, 0, 0, 0, 0, 0, 0 };
   p16(t + 2, port ^ 0x2112u);
   t[4] = ip[0] ^ 0x21; t[5] = ip[1] ^ 0x12; t[6] = ip[2] ^ 0xA4; t[7] = ip[3] ^ 0x42;
   m_attr(m, type, t, 8);
}
static void m_err(msg *m, int code)
{
   uint8_t t[4] = { 0, 0, (uint8_t)(code / 100), (uint8_t)(code % 100) };
   m_attr(m, 0x0009, t, 4);
}
static void m_sign(msg *m, const uint8_t key[16])
{
   uint8_t mac[20];
   if (m->n + 24 > sizeof m->b)
      return;
   p16(m->b + 2, (unsigned)(m->n - 20 + 24));
   nat_hmac_sha1(mac, key, 16, m->b, m->n);
   p16(m->b + m->n, 0x0008); p16(m->b + m->n + 2, 20);
   memcpy(m->b + m->n + 4, mac, 20);
   m->n += 24;
}

static const char USER[] = "1700000000:fuzz", PASS[] = "secret", REALM[] = "fuzz.example", NONCE[] = "0123456789abcdef";
static const uint8_t PEER_IP[4] = { 198, 51, 100, 7 };
#define PEER_PORT 40000

static void key_of(uint8_t key[16], const char *realm)
{
   char buf[400];
   int n = snprintf(buf, sizeof buf, "%s:%s:%s", USER, realm, PASS);
   nat_md5(key, buf, (size_t)n);
}

/* Damage m in place (n may shrink or grow within cap). */
static void damage(msg *m)
{
   int k, flips = 1 + (int)(rnd() % 4);
   for (k = 0; k < flips; k++)
   {
      switch (rnd() % 9)
      {
         case 0: case 1: case 2:               /* any byte */
            m->b[rnd() % m->n] = (uint8_t)rnd();
            break;
         case 3:                               /* the header's length */
            p16(m->b + 2, rnd() % 5 ? (unsigned)(rnd() % 1200) : 0xFFFC);
            break;
         case 4:                               /* cut */
            if (m->n > 1) m->n = 1 + rnd() % m->n;
            break;
         case 5:                               /* an attribute's length field (walk to one) */
         {
            size_t o = 20;
            int hops = (int)(rnd() % 6);
            while (hops-- > 0 && o + 4 <= m->n)
               o += 4 + ((((size_t)m->b[o + 2] << 8 | m->b[o + 3]) + 3) & ~(size_t)3);
            if (o + 4 <= m->n)
               p16(m->b + o + 2, rnd() % 3 ? (unsigned)(rnd() % 700) : 0xFFFF);
            break;
         }
         case 6:                               /* an attribute's type */
            if (m->n >= 24) p16(m->b + 20, (unsigned)(rnd() % 0x30));
            break;
         case 7:                               /* grow with noise */
         {
            size_t add = 4 * (1 + rnd() % 40), i;
            if (m->n + add < sizeof m->b - 32)
            {
               for (i = 0; i < add; i++) m->b[m->n + i] = (uint8_t)rnd();
               m->n += add;
               if (rnd() & 1) p16(m->b + 2, (unsigned)(m->n - 20));
            }
            break;
         }
         default:                              /* the type's class / method bits */
            m->b[rnd() & 1] ^= (uint8_t)(1u << (rnd() % 8));
            break;
      }
   }
}

/* Hand the datagram over from a heap block of exactly its size. */
static int feed_turn(nat_turn *c, const msg *m)
{
   uint8_t *d = (uint8_t *)malloc(m->n ? m->n : 1);
   const uint8_t *inner = NULL;
   size_t il = 0;
   int r;
   memcpy(d, m->b, m->n);
   r = nat_turn_handle(c, d, m->n, &inner, &il);
   if (r == 2)
   {
      size_t i;
      volatile unsigned sum = 0;
      CHECK(inner >= d && inner + il <= d + m->n, "a Data indication's payload lies inside the datagram");
      for (i = 0; i < il; i++) sum += inner[i];     /* ASan reads every byte of it */
   }
   {
      const uint8_t *p = NULL;
      size_t l = 0;
      unsigned ch = nat_turn_channel_unwrap(d, m->n, &p, &l);
      if (ch)
      {
         size_t i;
         volatile unsigned sum = 0;
         CHECK(p >= d && p + l <= d + m->n, "a ChannelData payload lies inside the datagram");
         for (i = 0; i < l; i++) sum += p[i];
      }
   }
   free(d);
   CHECK(memchr(c->realm, 0, sizeof c->realm) && memchr(c->nonce, 0, sizeof c->nonce), "realm / nonce terminated");
   CHECK(c->state >= NAT_RELAY_OFF && c->state <= NAT_RELAY_FAILED, "a known state");
   return r;
}

static void feed_stun(nat_stun *s, const msg *m, uint32_t from_ip, uint16_t from_port)
{
   uint8_t *d = (uint8_t *)malloc(m->n ? m->n : 1), tx[12];
   uint32_t ip = 0;
   uint16_t port = 0;
   memcpy(d, m->b, m->n);
   nat_stun_parse_response(d, m->n, tx, &ip, &port);
   nat_stun_handle(s, from_ip, from_port, d, m->n);
   free(d);
   CHECK(s->state >= NAT_STUN_IDLE && s->state <= NAT_STUN_FAILED, "a known STUN state");
}

/* A fresh client brought to `stage`: 0 = the first Allocate is out (no nonce), 1 = it has a nonce and the signed
 * Allocate is out, 2 = allocated and a peer is being bound (CreatePermission + ChannelBind out). */
static void client_at(nat_turn *c, int stage, uint64_t *now, uint8_t key[16])
{
   uint8_t seed[16];
   msg m;
   int i;
   const uint8_t *in = NULL;
   size_t il = 0;
   uint32_t srv = 0;
   const uint8_t sip[4] = { 203, 0, 113, 9 };
   for (i = 0; i < 16; i++) seed[i] = (uint8_t)rnd();
   memcpy(&srv, sip, 4);
   nat_turn_start(c, cap_send, NULL, srv, 3478, USER, PASS, seed, *now);
   key_of(key, REALM);
   if (stage < 1)
      return;
   m_init(&m, 0x0113, g_last + 8);                 /* Allocate error 401 */
   m_err(&m, 401);
   m_attr(&m, 0x0014, REALM, strlen(REALM));
   m_attr(&m, 0x0015, NONCE, strlen(NONCE));
   nat_turn_handle(c, m.b, m.n, &in, &il);
   if (stage < 2)
      return;
   m_init(&m, 0x0103, g_last + 8);                 /* Allocate success */
   m_addr(&m, 0x0016, sip, 50000);
   { uint8_t lt[4]; p32(lt, 600); m_attr(&m, 0x000D, lt, 4); }
   m_sign(&m, key);
   nat_turn_handle(c, m.b, m.n, &in, &il);
   {
      uint32_t pip = 0;
      memcpy(&pip, PEER_IP, 4);
      nat_turn_bind(c, pip, PEER_PORT, *now);
   }
   *now += 10;
   nat_turn_tick(c, *now);
}

int main(int argc, char **argv)
{
   long rounds = argc > 1 ? atol(argv[1]) : 400000, it;
   unsigned long handled = 0, inner = 0, failed = 0, ready = 0;
   uint64_t now = 1000;
   if (argc > 2)
      g_x = (uint32_t)strtoul(argv[2], NULL, 0) | 1u;

   for (it = 0; it < rounds; it++)
   {
      msg m;
      uint8_t key[16], badkey[16];
      const uint8_t sip[4] = { 203, 0, 113, 9 };
      int stage = (int)(rnd() % 3), kind = (int)(rnd() % 10), r, resign;

      if (it % 4 == 3)
      {
         /* ---- STUN ---- */
         nat_stun s;
         uint8_t tx[2][12];
         uint32_t ip[2];
         uint16_t port[2] = { 3478, 53 };
         int i;
         for (i = 0; i < 24; i++) tx[i / 12][i % 12] = (uint8_t)rnd();
         memcpy(&ip[0], sip, 4);
         ip[1] = ip[0] ^ 0x01000000u;
         nat_stun_start(&s, cap_send, NULL, ip, port, 1 + (int)(rnd() & 1), (const uint8_t (*)[12])tx, now);
         m_init(&m, kind == 0 ? 0x0111 : 0x0101, tx[rnd() & 1]);
         if (kind & 1) m_addr(&m, 0x0020, sip, 40001);
         if (kind & 2) { uint8_t t[8] = { 0, 1, 0x9C, 0x41, 9, 9, 9, 9 }; m_attr(&m, 0x0001, t, 8); }
         if (kind & 4) { uint8_t t[20] = { 0, 2 }; m_attr(&m, 0x0020, t, 20); }      /* IPv6 */
         if (rnd() % 5)
            damage(&m);
         feed_stun(&s, &m, rnd() % 4 ? ip[0] : ip[1], rnd() % 4 ? 3478 : 53);
         now += 300;
         nat_stun_tick(&s, now);
         continue;
      }

      /* ---- TURN ---- */
      {
         nat_turn c;
         uint32_t answered;
         client_at(&c, stage, &now, key);
         memcpy(badkey, key, 16);
         badkey[rnd() % 16] ^= 0x40;
         answered = (uint32_t)g_last[0] << 8 | g_last[1];         /* the request type that is out */
         switch (kind)
         {
            case 0:                                   /* 401 with (long) realm and nonce */
            {
               char big[260];
               int long_realm = rnd() % 4 == 0, long_nonce = rnd() % 4 == 0;
               size_t i;
               m_init(&m, answered | 0x0110, g_last + 8);
               m_err(&m, rnd() % 3 ? 401 : 438);
               for (i = 0; i < sizeof big; i++) big[i] = (char)('a' + rnd() % 26);
               m_attr(&m, 0x0014, long_realm ? big : REALM, long_realm ? 1 + rnd() % 250 : strlen(REALM));
               m_attr(&m, 0x0015, long_nonce ? big : NONCE, long_nonce ? 1 + rnd() % 250 : strlen(NONCE));
               break;
            }
            case 1: case 2: case 3:                   /* a success for what is out */
               m_init(&m, answered | 0x0100, g_last + 8);
               if (rnd() % 4) m_addr(&m, 0x0016, sip, 50000 + rnd() % 100);
               if (rnd() % 3) { uint8_t lt[4]; p32(lt, rnd() % 4 ? 600 : rnd()); m_attr(&m, 0x000D, lt, 4); }
               break;
            case 4:                                   /* an error answer of its own */
            {
               static const int codes[] = { 400, 403, 437, 441, 481, 486, 508, 500, 300, 699 };
               m_init(&m, answered | 0x0110, g_last + 8);
               m_err(&m, codes[rnd() % 10]);
               break;
            }
            case 5: case 6:                           /* a Data indication */
            {
               uint8_t data[300];
               size_t dl = rnd() % sizeof data, i;
               uint8_t tx[12];
               for (i = 0; i < 12; i++) tx[i] = (uint8_t)rnd();
               for (i = 0; i < dl; i++) data[i] = (uint8_t)rnd();
               m_init(&m, 0x0017, tx);
               m_addr(&m, 0x0012, PEER_IP, rnd() % 5 ? PEER_PORT : PEER_PORT + 1);
               m_attr(&m, 0x0013, data, dl);
               break;
            }
            case 7:                                   /* ChannelData */
            {
               size_t dl = rnd() % 600, i;
               memset(&m, 0, sizeof m);
               p16(m.b, rnd() % 4 ? 0x4000 : 0x4000 + rnd() % 0x4000);
               p16(m.b + 2, rnd() % 4 ? (unsigned)dl : (unsigned)(rnd() % 0x10000));
               for (i = 0; i < dl; i++) m.b[4 + i] = (uint8_t)rnd();
               m.n = 4 + dl;
               break;
            }
            case 8:                                   /* an answer to a transaction nobody made */
            {
               uint8_t tx[12];
               size_t i;
               for (i = 0; i < 12; i++) tx[i] = (uint8_t)rnd();
               m_init(&m, 0x0103, tx);
               m_addr(&m, 0x0016, sip, 50000);
               break;
            }
            default:                                  /* noise */
            {
               size_t i;
               m.n = 1 + rnd() % 700;
               for (i = 0; i < m.n; i++) m.b[i] = (uint8_t)rnd();
               if (rnd() & 1) { m.b[0] &= 0x3F; p32(m.b + 4, 0x2112A442u); if (m.n >= 20 && (rnd() & 1)) memcpy(m.b + 8, g_last + 8, 12); }
               break;
            }
         }
         resign = kind >= 1 && kind <= 3;
         if (resign && rnd() % 6 == 0)
            m_sign(&m, rnd() % 8 ? key : badkey);    /* signed and intact (or with a wrong key) */
         else
         {
            if (rnd() % 6)
               damage(&m);
            /* a server that lies: the damaged message, its length repaired, signed with the right key */
            if (resign && rnd() % 2 && m.n >= 20 && m.n < 480)
            {
               m.n = 20 + ((m.n - 20) & ~(size_t)3);
               p16(m.b + 2, (unsigned)(m.n - 20));
               m_sign(&m, key);
            }
         }
         r = feed_turn(&c, &m);
         handled += r != 0;
         inner += r == 2;
         now += 400;
         nat_turn_tick(&c, now);
         nat_turn_tick(&c, now + 700000);             /* refresh / expiry paths with whatever lifetime it took */
         failed += c.state == NAT_RELAY_FAILED;
         ready += c.state == NAT_RELAY_READY || c.state == NAT_RELAY_BINDING || c.state == NAT_RELAY_ALLOCATED;
         CHECK(c.lifetime_s <= 86400, "the allocation lifetime is clamped");
         nat_turn_release(&c, now);
      }
   }
   printf("fuzz_nat: %ld datagrams, %lu consumed by the TURN client, %lu delivered as peer data, %lu ended a client, %lu left it allocated; %d failures\n",
          rounds, handled, inner, failed, ready, g_fail);
   return g_fail ? 1 : 0;
}
