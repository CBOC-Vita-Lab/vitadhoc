/* nat_stun.c -- see nat_stun.h.  Port of Galaxy's np_stun.cpp to plain C:
 * the Binding request / answer codec and the retransmission state machine
 * for one or two servers.  Static names start with stun_ (the twin builds
 * the three nat files as one translation unit). */
#include <string.h>

#include "nat_stun.h"

#define STUN_COOKIE_0 0x21
#define STUN_COOKIE_1 0x12
#define STUN_COOKIE_2 0xA4
#define STUN_COOKIE_3 0x42
#define STUN_ATTR_MAPPED     0x0001
#define STUN_ATTR_XOR_MAPPED 0x0020
#define STUN_MAX_SENDS       5      /* the wait after the fifth is the last */
#define STUN_FIRST_GRACE_MS  1000

static const uint16_t stun_wait_ms[STUN_MAX_SENDS] = {250, 500, 1000, 1500, 2000};

static void stun_put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static uint16_t stun_get16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

size_t nat_stun_build_request(uint8_t out[20], const uint8_t txid[12])
{
   stun_put16(out, 0x0001);              /* Binding request */
   stun_put16(out + 2, 0);               /* no attributes */
   out[4] = STUN_COOKIE_0; out[5] = STUN_COOKIE_1;
   out[6] = STUN_COOKIE_2; out[7] = STUN_COOKIE_3;
   memcpy(out + 8, txid, 12);
   return 20;
}

int nat_stun_parse_response(const uint8_t *d, size_t n, uint8_t txid[12],
                            uint32_t *ip_be, uint16_t *port)
{
   uint16_t type, alen;
   size_t pos;
   int have_x = 0, have_p = 0;
   uint8_t xip[4], pip[4];
   uint16_t xport = 0, pport = 0;

   if (n < 20 || (d[0] & 0xC0) != 0 || d[4] != STUN_COOKIE_0 || d[5] != STUN_COOKIE_1 ||
       d[6] != STUN_COOKIE_2 || d[7] != STUN_COOKIE_3)
      return 0;
   type = stun_get16(d);
   alen = stun_get16(d + 2);
   if ((alen & 3) || 20u + alen > n)
      return 0;
   if (type == 0x0111)                   /* Binding error: ours if the id matches, no address */
   {
      memcpy(txid, d + 8, 12);
      return 2;
   }
   if (type != 0x0101)
      return 0;
   pos = 20;
   while (pos + 4 <= 20u + alen)
   {
      uint16_t at = stun_get16(d + pos), al = stun_get16(d + pos + 2);
      size_t padded = ((size_t)al + 3) & ~(size_t)3;
      const uint8_t *v = d + pos + 4;
      if (pos + 4 + al > 20u + alen)
         return 0;
      if ((at == STUN_ATTR_XOR_MAPPED || at == STUN_ATTR_MAPPED) && al >= 8 && v[1] == 0x01)
      {
         if (at == STUN_ATTR_XOR_MAPPED)
         {
            xport = (uint16_t)(stun_get16(v + 2) ^ 0x2112);
            xip[0] = v[4] ^ STUN_COOKIE_0; xip[1] = v[5] ^ STUN_COOKIE_1;
            xip[2] = v[6] ^ STUN_COOKIE_2; xip[3] = v[7] ^ STUN_COOKIE_3;
            have_x = 1;
         }
         else
         {
            pport = stun_get16(v + 2);
            memcpy(pip, v + 4, 4);
            have_p = 1;
         }
      }
      pos += 4 + padded;
   }
   if (have_x)       { memcpy(ip_be, xip, 4); *port = xport; }
   else if (have_p)  { memcpy(ip_be, pip, 4); *port = pport; }
   else
      return 0;
   if (!*port)
      return 0;
   memcpy(txid, d + 8, 12);
   return 1;
}

static void stun_send_one(nat_stun *s, int i)
{
   uint8_t pkt[20];
   nat_stun_build_request(pkt, s->txid[i]);
   if (s->send)
      s->send(s->ctx, s->ip_be[i], s->port[i], pkt, sizeof pkt);
   s->next_ms[i] = s->clock_ms + stun_wait_ms[s->sends[i] < STUN_MAX_SENDS ? s->sends[i] : STUN_MAX_SENDS - 1];
   s->sends[i]++;
}

void nat_stun_start(nat_stun *s, nat_send_fn send, void *ctx, const uint32_t *ip_be,
                    const uint16_t *port, int n, const uint8_t txids[2][12], uint64_t now_ms)
{
   int i;
   memset(s, 0, sizeof *s);
   s->send = send;
   s->ctx = ctx;
   s->first = -1;
   s->clock_ms = now_ms;
   if (n < 1 || n > 2 || !ip_be || !port || !txids)
   {
      s->state = NAT_STUN_FAILED;
      return;
   }
   s->n = n;
   for (i = 0; i < n; i++)
   {
      s->ip_be[i] = ip_be[i];
      s->port[i] = port[i];
      memcpy(s->txid[i], txids[i], 12);
   }
   s->state = NAT_STUN_RUNNING;
   for (i = 0; i < n; i++)
      stun_send_one(s, i);
}

/* OK / FAILED from what the servers have said so far. */
static void stun_settle(nat_stun *s)
{
   int i, pending = 0;
   if (s->state != NAT_STUN_RUNNING)
      return;
   for (i = 0; i < s->n; i++)
      if (!s->got[i] && !s->gave_up[i])
         pending = 1;
   if (!pending)
      s->state = s->first >= 0 ? NAT_STUN_OK : NAT_STUN_FAILED;
   else if (s->first >= 0 && s->clock_ms - s->first_ms >= STUN_FIRST_GRACE_MS)
      s->state = NAT_STUN_OK;
}

void nat_stun_tick(nat_stun *s, uint64_t now_ms)
{
   int i;
   if (s->state != NAT_STUN_RUNNING && s->state != NAT_STUN_OK)
      return;
   s->clock_ms = now_ms;
   for (i = 0; i < s->n; i++)
   {
      if (s->got[i] || s->gave_up[i] || now_ms < s->next_ms[i])
         continue;
      if (s->sends[i] >= STUN_MAX_SENDS)
         s->gave_up[i] = 1;              /* the last wait is over */
      else
         stun_send_one(s, i);
   }
   stun_settle(s);
}

int nat_stun_handle(nat_stun *s, uint32_t from_ip_be, uint16_t from_port,
                    const uint8_t *d, size_t n)
{
   uint8_t tx[12];
   uint32_t ip = 0;
   uint16_t port = 0;
   int i, r;

   if (s->state != NAT_STUN_RUNNING && s->state != NAT_STUN_OK)
      return 0;
   r = nat_stun_parse_response(d, n, tx, &ip, &port);
   if (!r)
      return 0;
   for (i = 0; i < s->n; i++)
   {
      if (memcmp(s->txid[i], tx, 12) != 0)
         continue;
      if (from_ip_be != s->ip_be[i] || from_port != s->port[i])
         return 1;                       /* right transaction, wrong source: ignored */
      if (r == 2)
         s->gave_up[i] = 1;              /* an error answer: do not wait for more */
      else if (!s->got[i])
      {
         s->got[i] = 1;
         s->map_ip_be[i] = ip;
         s->map_port[i] = port;
         if (s->first < 0)
         {
            s->first = i;
            s->first_ms = s->clock_ms;
         }
         else if (s->map_ip_be[i] != s->map_ip_be[s->first] ||
                  s->map_port[i] != s->map_port[s->first])
            s->sym = 1;
      }
      stun_settle(s);
      return 1;
   }
   return 0;
}

int nat_stun_state(const nat_stun *s)
{
   return s->state;
}

int nat_stun_result(const nat_stun *s, uint32_t *ip_be, uint16_t *port, int *sym)
{
   if (s->first < 0)
      return 0;
   if (ip_be)
      *ip_be = s->map_ip_be[s->first];
   if (port)
      *port = s->map_port[s->first];
   if (sym)
      *sym = s->sym;
   return 1;
}
