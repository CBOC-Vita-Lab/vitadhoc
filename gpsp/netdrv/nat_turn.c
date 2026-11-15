/* nat_turn.c -- see nat_turn.h.  Port of Galaxy's np_turn.cpp to plain C,
 * reduced to one peer and no heap.  Static names start with turn_ (the twin
 * builds the three nat files as one translation unit). */
#include <stdio.h>
#include <string.h>

#include "nat_hash.h"
#include "nat_turn.h"

#define TURN_LIFETIME_S    600
#define TURN_PERM_MS       180000u      /* CreatePermission again every 3 minutes */
#define TURN_BIND_MS       300000u      /* ChannelBind again every 5 minutes */
#define TURN_MAX_SENDS     5
static const uint16_t turn_rto_ms[TURN_MAX_SENDS] = {300, 600, 1200, 1600, 1600};

/* message types: method | class (request 0, indication 0x10, success 0x100, error 0x110) */
#define TT_ALLOCATE   0x0003
#define TT_REFRESH    0x0004
#define TT_PERMISSION 0x0008
#define TT_CHANNELBND 0x0009
#define TT_DATA_IND   0x0017
/* attributes */
#define TA_USERNAME   0x0006
#define TA_INTEGRITY  0x0008
#define TA_ERRORCODE  0x0009
#define TA_CHANNEL    0x000C
#define TA_LIFETIME   0x000D
#define TA_PEER       0x0012
#define TA_DATA       0x0013
#define TA_REALM      0x0014
#define TA_NONCE      0x0015
#define TA_RELAYED    0x0016
#define TA_TRANSPORT  0x0019

enum { TK_ALLOC = 1, TK_REFRESH, TK_PERM, TK_BIND };

static void turn_put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void turn_put32(uint8_t *p, uint32_t v)
{
   p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static uint32_t turn_get16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }
static uint32_t turn_get32(const uint8_t *p)
{
   return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static void turn_copy_str(char *dst, size_t cap, const char *src, size_t n)
{
   size_t i = 0;
   for (; src && i < n && src[i] && i + 1 < cap; i++)
      dst[i] = src[i];
   dst[i] = 0;
}

/* xorshift128 over the caller's 16 bytes; never all zero. */
static uint32_t turn_rand(nat_turn *c)
{
   uint32_t t = c->xs[0] ^ (c->xs[0] << 11);
   c->xs[0] = c->xs[1]; c->xs[1] = c->xs[2]; c->xs[2] = c->xs[3];
   c->xs[3] = c->xs[3] ^ (c->xs[3] >> 19) ^ t ^ (t >> 8);
   return c->xs[3];
}

static void turn_txid(nat_turn *c, uint8_t id[12])
{
   int i;
   for (i = 0; i < 12; i += 4)
      turn_put32(id + i, turn_rand(c));
}

/* ---- a message under construction ---- */
typedef struct turn_msg
{
   uint8_t *b;
   size_t   cap, n;
} turn_msg;

static void turn_msg_init(turn_msg *m, uint8_t *buf, size_t cap, uint16_t type, const uint8_t txid[12])
{
   m->b = buf;
   m->cap = cap;
   m->n = 20;
   turn_put16(buf, type);
   turn_put16(buf + 2, 0);
   turn_put32(buf + 4, 0x2112A442u);
   memcpy(buf + 8, txid, 12);
}

static void turn_attr(turn_msg *m, uint16_t type, const void *v, size_t len)
{
   size_t padded = (len + 3) & ~(size_t)3;
   if (m->n + 4 + padded > m->cap)
      return;
   turn_put16(m->b + m->n, type);
   turn_put16(m->b + m->n + 2, (uint32_t)len);
   if (len)
      memcpy(m->b + m->n + 4, v, len);
   memset(m->b + m->n + 4 + len, 0, padded - len);
   m->n += 4 + padded;
   turn_put16(m->b + 2, (uint32_t)(m->n - 20));
}

static void turn_attr_u32(turn_msg *m, uint16_t type, uint32_t v)
{
   uint8_t t[4];
   turn_put32(t, v);
   turn_attr(m, type, t, 4);
}

static void turn_attr_str(turn_msg *m, uint16_t type, const char *s)
{
   turn_attr(m, type, s, strlen(s));
}

static void turn_attr_xor_addr(turn_msg *m, uint16_t type, uint32_t ip_be, uint16_t port)
{
   uint8_t t[8], ip[4];
   memcpy(ip, &ip_be, 4);
   t[0] = 0;
   t[1] = 1;
   turn_put16(t + 2, port ^ 0x2112u);
   t[4] = ip[0] ^ 0x21; t[5] = ip[1] ^ 0x12; t[6] = ip[2] ^ 0xA4; t[7] = ip[3] ^ 0x42;
   turn_attr(m, type, t, 8);
}

static int turn_dec_xor_addr(const uint8_t *v, size_t len, uint32_t *ip_be, uint16_t *port)
{
   uint8_t ip[4];
   if (len < 8 || v[1] != 1)
      return 0;
   *port = (uint16_t)(turn_get16(v + 2) ^ 0x2112u);
   ip[0] = v[4] ^ 0x21; ip[1] = v[5] ^ 0x12; ip[2] = v[6] ^ 0xA4; ip[3] = v[7] ^ 0x42;
   memcpy(ip_be, ip, 4);
   return 1;
}

/* MESSAGE-INTEGRITY: HMAC-SHA1 over the message so far with the header's
 * length already counting the attribute (RFC 5389 15.4). */
static int turn_add_integrity(uint8_t *msg, size_t *len, size_t cap, const uint8_t key[16])
{
   uint8_t mac[20];
   if (*len + 24 > cap)
      return 0;
   turn_put16(msg + 2, (uint32_t)(*len - 20 + 24));
   nat_hmac_sha1(mac, key, 16, msg, *len);
   turn_put16(msg + *len, TA_INTEGRITY);
   turn_put16(msg + *len + 2, 20);
   memcpy(msg + *len + 4, mac, 20);
   *len += 24;
   return 1;
}

static void turn_long_term_key(nat_turn *c)
{
   char buf[480];
   int n = snprintf(buf, sizeof buf, "%s:%s:%s", c->user, c->realm, c->pass);
   nat_md5(c->key, buf, n > 0 && (size_t)n < sizeof buf ? (size_t)n : sizeof buf - 1);
}

/* ---- requests ---- */

static nat_turn_txn *turn_free_txn(nat_turn *c)
{
   int i;
   for (i = 0; i < NAT_TURN_TXNS; i++)
      if (!c->txn[i].active)
         return &c->txn[i];
   return NULL;
}

static nat_turn_txn *turn_find_txn(nat_turn *c, const uint8_t txid[12])
{
   int i;
   for (i = 0; i < NAT_TURN_TXNS; i++)
      if (c->txn[i].active && !memcmp(c->txn[i].txid, txid, 12))
         return &c->txn[i];
   return NULL;
}

/* Build (or rebuild with a fresh id and the current nonce) a request. */
static void turn_build(nat_turn *c, nat_turn_txn *t)
{
   uint16_t type = TT_ALLOCATE;
   turn_msg m;
   turn_txid(c, t->txid);
   switch (t->kind)
   {
      case TK_REFRESH: type = TT_REFRESH; break;
      case TK_PERM:    type = TT_PERMISSION; break;
      case TK_BIND:    type = TT_CHANNELBND; break;
      default:         break;
   }
   turn_msg_init(&m, t->msg, sizeof t->msg, type, t->txid);
   if (t->kind == TK_ALLOC)
   {
      turn_attr_u32(&m, TA_TRANSPORT, 17u << 24);       /* UDP */
      turn_attr_u32(&m, TA_LIFETIME, TURN_LIFETIME_S);
   }
   else if (t->kind == TK_REFRESH)
      turn_attr_u32(&m, TA_LIFETIME, TURN_LIFETIME_S);
   else
   {
      if (t->kind == TK_BIND)
      {
         uint8_t ch[4];
         turn_put16(ch, NAT_TURN_CHANNEL);
         ch[2] = ch[3] = 0;
         turn_attr(&m, TA_CHANNEL, ch, 4);
      }
      turn_attr_xor_addr(&m, TA_PEER, c->peer_ip_be, c->peer_port);
   }
   t->len = m.n;
   t->authed = 0;
   if (c->have_nonce)
   {
      turn_attr_str(&m, TA_USERNAME, c->user);
      turn_attr_str(&m, TA_REALM, c->realm);
      turn_attr_str(&m, TA_NONCE, c->nonce);
      t->len = m.n;
      if (turn_add_integrity(t->msg, &t->len, sizeof t->msg, c->key))
         t->authed = 1;
   }
}

static void turn_transmit(nat_turn *c, nat_turn_txn *t, uint64_t now)
{
   if (c->send)
      c->send(c->ctx, c->server_ip_be, c->server_port, t->msg, t->len);
   t->next_ms = now + turn_rto_ms[t->sends < TURN_MAX_SENDS ? t->sends : TURN_MAX_SENDS - 1];
   t->sends++;
}

static void turn_begin(nat_turn *c, int kind, uint64_t now)
{
   nat_turn_txn *t = turn_free_txn(c);
   if (!t)
      return;
   memset(t, 0, sizeof *t);
   t->active = 1;
   t->kind = kind;
   turn_build(c, t);
   turn_transmit(c, t, now);
}

static void turn_fail(nat_turn *c, int err)
{
   c->state = NAT_RELAY_FAILED;
   c->err = err;
   memset(c->txn, 0, sizeof c->txn);
}

static void turn_start_alloc(nat_turn *c, uint64_t now)
{
   int i;
   for (i = 0; i < NAT_TURN_TXNS; i++)
      c->txn[i].active = 0;
   c->state = NAT_RELAY_ALLOCATING;
   turn_begin(c, TK_ALLOC, now);
}

void nat_turn_start(nat_turn *c, nat_send_fn send, void *ctx, uint32_t server_ip_be, uint16_t server_port,
                    const char *user, const char *pass, const uint8_t seed[16], uint64_t now_ms)
{
   int i;
   memset(c, 0, sizeof *c);
   c->send = send;
   c->ctx = ctx;
   c->server_ip_be = server_ip_be;
   c->server_port = server_port;
   c->t0_ms = c->clock_ms = now_ms;
   turn_copy_str(c->user, sizeof c->user, user, (size_t)-1);
   turn_copy_str(c->pass, sizeof c->pass, pass, (size_t)-1);
   for (i = 0; i < 4; i++)
      c->xs[i] = seed ? turn_get32(seed + 4 * i) : 0;
   if (!(c->xs[0] | c->xs[1] | c->xs[2] | c->xs[3]))
      c->xs[0] = 0x9E3779B9u;
   if (!server_ip_be || !server_port)
   {
      turn_fail(c, NAT_RELAY_E_TIMEOUT);
      return;
   }
   turn_start_alloc(c, now_ms);
}

int nat_turn_bind(nat_turn *c, uint32_t peer_ip_be, uint16_t peer_port, uint64_t now_ms)
{
   c->clock_ms = now_ms;
   if (c->state != NAT_RELAY_ALLOCATED && c->state != NAT_RELAY_BINDING && c->state != NAT_RELAY_READY)
      return -1;
   if (!peer_ip_be || !peer_port)
      return -1;
   if (c->have_peer)
      return c->peer_ip_be == peer_ip_be && c->peer_port == peer_port ? 0 : -1;
   c->have_peer = 1;
   c->peer_ip_be = peer_ip_be;
   c->peer_port = peer_port;
   c->perm_ok = c->bind_ok = c->perm_busy = c->bind_busy = 0;
   c->retry_at = 0;
   c->state = NAT_RELAY_BINDING;
   return 0;
}

void nat_turn_release(nat_turn *c, uint64_t now_ms)
{
   c->clock_ms = now_ms;
   if (c->have_nonce && c->send &&
       (c->state == NAT_RELAY_ALLOCATED || c->state == NAT_RELAY_BINDING || c->state == NAT_RELAY_READY))
   {
      uint8_t buf[480], txid[12];
      turn_msg m;
      size_t len;
      turn_txid(c, txid);
      turn_msg_init(&m, buf, sizeof buf, TT_REFRESH, txid);
      turn_attr_u32(&m, TA_LIFETIME, 0);
      turn_attr_str(&m, TA_USERNAME, c->user);
      turn_attr_str(&m, TA_REALM, c->realm);
      turn_attr_str(&m, TA_NONCE, c->nonce);
      len = m.n;
      if (turn_add_integrity(buf, &len, sizeof buf, c->key))
         c->send(c->ctx, c->server_ip_be, c->server_port, buf, len);
   }
   memset(c->txn, 0, sizeof c->txn);
   c->state = NAT_RELAY_OFF;
   c->relayed_ip_be = 0;
   c->relayed_port = 0;
}

static int turn_live(const nat_turn *c)
{
   return c->state == NAT_RELAY_ALLOCATING || c->state == NAT_RELAY_ALLOCATED ||
          c->state == NAT_RELAY_BINDING || c->state == NAT_RELAY_READY;
}

static void turn_timeout(nat_turn *c, nat_turn_txn *t, uint64_t now)
{
   int kind = t->kind;
   t->active = 0;
   switch (kind)
   {
      case TK_ALLOC:
         turn_fail(c, NAT_RELAY_E_TIMEOUT);
         break;
      case TK_REFRESH:
         if (++c->refresh_fails >= 2)
            turn_fail(c, NAT_RELAY_E_LOST);
         else
            c->refresh_at = now + 1000;
         break;
      case TK_PERM:
         c->perm_busy = 0;
         c->retry_at = now + 2000;
         break;
      case TK_BIND:
         c->bind_busy = 0;
         c->retry_at = now + 2000;
         break;
   }
}

void nat_turn_tick(nat_turn *c, uint64_t now)
{
   int i;
   c->clock_ms = now;
   if (!turn_live(c))
      return;
   if (c->alloc_retry_at && now >= c->alloc_retry_at)
   {
      c->alloc_retry_at = 0;
      turn_start_alloc(c, now);
   }
   for (i = 0; i < NAT_TURN_TXNS; i++)
   {
      nat_turn_txn *t = &c->txn[i];
      if (!t->active || now < t->next_ms)
         continue;
      if (t->sends >= TURN_MAX_SENDS)
      {
         turn_timeout(c, t, now);
         if (!turn_live(c))
            return;
         continue;
      }
      turn_transmit(c, t, now);          /* same id, same bytes */
   }
   if (c->state == NAT_RELAY_ALLOCATING)
      return;
   if (c->expires_at && now >= c->expires_at)
   {
      turn_fail(c, NAT_RELAY_E_LOST);    /* the allocation ran out and nobody could refresh it */
      return;
   }
   if (c->refresh_at && now >= c->refresh_at)
   {
      int busy = 0;
      for (i = 0; i < NAT_TURN_TXNS; i++)
         if (c->txn[i].active && (c->txn[i].kind == TK_REFRESH || c->txn[i].kind == TK_ALLOC))
            busy = 1;
      if (!busy)
      {
         turn_begin(c, TK_REFRESH, now);
         c->refresh_at = now + 600000;   /* the answer sets the next time */
      }
   }
   if (c->have_peer && now >= c->retry_at)
   {
      if (!c->perm_busy && (!c->perm_ok || now >= c->perm_at))
      {
         c->perm_busy = 1;
         turn_begin(c, TK_PERM, now);
      }
      if (!c->bind_busy && (!c->bind_ok || now >= c->bind_at))
      {
         c->bind_busy = 1;
         turn_begin(c, TK_BIND, now);
      }
   }
}

/* ---- answers ---- */

typedef struct turn_parsed
{
   uint16_t type;
   uint8_t  txid[12];
   int      has_realm, has_nonce, has_lifetime, has_relayed, has_peer, has_data;
   char     realm[128], nonce[192];
   int      err_code;
   uint32_t lifetime;
   uint32_t relayed_ip, peer_ip;
   uint16_t relayed_port, peer_port;
   const uint8_t *data;
   size_t   data_len;
} turn_parsed;

static int turn_parse(const uint8_t *d, size_t n, turn_parsed *p)
{
   size_t body, o;
   if (n < 20 || (d[0] & 0xC0) != 0 || turn_get32(d + 4) != 0x2112A442u)
      return 0;
   body = turn_get16(d + 2);
   if (body + 20 > n || (body & 3))
      return 0;
   memset(p, 0, sizeof *p);
   p->type = (uint16_t)turn_get16(d);
   memcpy(p->txid, d + 8, 12);
   o = 20;
   while (o + 4 <= 20 + body)
   {
      uint16_t t = (uint16_t)turn_get16(d + o);
      size_t l = turn_get16(d + o + 2);
      const uint8_t *v = d + o + 4;
      if (o + 4 + l > 20 + body)
         return 0;
      switch (t)
      {
         case TA_REALM: p->has_realm = 1; turn_copy_str(p->realm, sizeof p->realm, (const char *)v, l); break;
         case TA_NONCE: p->has_nonce = 1; turn_copy_str(p->nonce, sizeof p->nonce, (const char *)v, l); break;
         case TA_ERRORCODE: if (l >= 4) p->err_code = (int)(v[2] & 7) * 100 + (int)v[3]; break;
         case TA_LIFETIME: if (l >= 4) { p->has_lifetime = 1; p->lifetime = turn_get32(v); } break;
         case TA_RELAYED: p->has_relayed = turn_dec_xor_addr(v, l, &p->relayed_ip, &p->relayed_port); break;
         case TA_PEER: p->has_peer = turn_dec_xor_addr(v, l, &p->peer_ip, &p->peer_port); break;
         case TA_DATA: p->has_data = 1; p->data = v; p->data_len = l; break;
         default: break;
      }
      o += 4 + ((l + 3) & ~(size_t)3);
   }
   return 1;
}

/* the request `t` answered, replaced by one with a new nonce */
static void turn_retry_with_nonce(nat_turn *c, nat_turn_txn *t, const turn_parsed *p, uint64_t now)
{
   int tries = t->auth_tries + 1, kind = t->kind;
   nat_turn_txn *n;
   turn_copy_str(c->realm, sizeof c->realm, p->realm, (size_t)-1);
   turn_copy_str(c->nonce, sizeof c->nonce, p->nonce, (size_t)-1);
   c->have_nonce = 1;
   turn_long_term_key(c);
   t->active = 0;
   n = turn_free_txn(c);
   if (!n)
      return;
   memset(n, 0, sizeof *n);
   n->active = 1;
   n->kind = kind;
   n->auth_tries = tries;
   turn_build(c, n);
   turn_transmit(c, n, now);
}

static void turn_on_success(nat_turn *c, nat_turn_txn *t, const turn_parsed *p, uint64_t now)
{
   int kind = t->kind;
   t->active = 0;
   switch (kind)
   {
      case TK_ALLOC:
         if (!p->has_relayed)
         {
            turn_fail(c, NAT_RELAY_E_AUTH);
            return;
         }
         c->relayed_ip_be = p->relayed_ip;
         c->relayed_port = p->relayed_port;
         c->lifetime_s = p->has_lifetime ? p->lifetime : TURN_LIFETIME_S;
         if (c->lifetime_s < 10)
            c->lifetime_s = 10;           /* a broken answer must not make us refresh in a tight loop */
         if (c->lifetime_s > 86400)
            c->lifetime_s = 86400;
         c->expires_at = now + (uint64_t)c->lifetime_s * 1000u;
         c->refresh_at = now + (uint64_t)c->lifetime_s * 500u;      /* half of it */
         c->refresh_fails = 0;
         c->state = NAT_RELAY_ALLOCATED;
         break;
      case TK_REFRESH:
         if (p->has_lifetime)
            c->lifetime_s = p->lifetime < 10 ? 10 : (p->lifetime > 86400 ? 86400 : p->lifetime);
         c->expires_at = now + (uint64_t)c->lifetime_s * 1000u;
         c->refresh_at = now + (uint64_t)c->lifetime_s * 500u;
         c->refresh_fails = 0;
         break;
      case TK_PERM:
         c->perm_busy = 0;
         c->perm_ok = 1;
         c->perm_at = now + TURN_PERM_MS;
         break;
      case TK_BIND:
         c->bind_busy = 0;
         c->bind_ok = 1;
         c->bind_at = now + TURN_BIND_MS;
         break;
   }
   if (c->state == NAT_RELAY_BINDING && c->perm_ok && c->bind_ok)
      c->state = NAT_RELAY_READY;
}

static void turn_on_error(nat_turn *c, nat_turn_txn *t, const turn_parsed *p, uint64_t now)
{
   int code = p->err_code, kind = t->kind;

   /* 401 with a nonce (the first Allocate, or a stale nonce from a server
    * that answers 401 instead of 438) and 438: take the new realm / nonce
    * and send the request again. */
   if ((code == 401 || code == 438) && p->has_nonce && p->has_realm)
   {
      int changed = !c->have_nonce || strcmp(c->nonce, p->nonce) || strcmp(c->realm, p->realm);
      if (!t->authed || (changed && t->auth_tries < 3))
      {
         turn_retry_with_nonce(c, t, p, now);
         return;
      }
   }
   t->active = 0;
   switch (code)
   {
      case 401: case 441: case 403:
         if (kind == TK_ALLOC)
         {
            turn_fail(c, NAT_RELAY_E_AUTH);
            return;
         }
         if (code == 403 && (kind == TK_PERM || kind == TK_BIND))
         {
            c->perm_busy = c->bind_busy = 0;
            c->retry_at = now + 5000;
            return;
         }
         /* the credentials ran out after the allocation worked: it lives on
          * until its own lifetime ends; remember why, retry slowly */
         c->err = NAT_RELAY_E_AUTH;
         if (kind == TK_REFRESH)
            c->refresh_at = now + 30000;
         else
         {
            c->perm_busy = c->bind_busy = 0;
            c->retry_at = now + 30000;
         }
         return;
      case 437:                          /* allocation mismatch: an old one of this socket is still there */
         if (kind == TK_ALLOC && c->have_nonce && c->realloc_tries < 2)
         {
            uint8_t buf[480], txid[12];
            turn_msg m;
            size_t len;
            turn_txid(c, txid);
            turn_msg_init(&m, buf, sizeof buf, TT_REFRESH, txid);
            turn_attr_u32(&m, TA_LIFETIME, 0);
            turn_attr_str(&m, TA_USERNAME, c->user);
            turn_attr_str(&m, TA_REALM, c->realm);
            turn_attr_str(&m, TA_NONCE, c->nonce);
            len = m.n;
            if (turn_add_integrity(buf, &len, sizeof buf, c->key) && c->send)
               c->send(c->ctx, c->server_ip_be, c->server_port, buf, len);
            c->realloc_tries++;
            c->alloc_retry_at = now + 500;
            return;
         }
         turn_fail(c, kind == TK_ALLOC ? NAT_RELAY_E_AUTH : NAT_RELAY_E_LOST);
         return;
      case 481:
         turn_fail(c, NAT_RELAY_E_LOST);
         return;
      case 486: case 508:
         turn_fail(c, NAT_RELAY_E_QUOTA);
         return;
      default:
         break;
   }
   if (kind == TK_ALLOC)
      turn_fail(c, NAT_RELAY_E_AUTH);     /* refused for a reason of its own */
   else if (kind == TK_PERM || kind == TK_BIND)
   {
      c->perm_busy = c->bind_busy = 0;
      c->retry_at = now + 3000;
   }
}

int nat_turn_handle(nat_turn *c, const uint8_t *d, size_t n, const uint8_t **inner, size_t *inner_len)
{
   turn_parsed p;
   nat_turn_txn *t;
   if (!turn_live(c) || !turn_parse(d, n, &p))
      return 0;
   if (p.type == TT_DATA_IND)
   {
      if (!p.has_data || !p.has_peer || !c->have_peer || p.peer_ip != c->peer_ip_be ||
          p.peer_port != c->peer_port)
         return 1;                        /* a Data indication from nobody we talk to: dropped */
      *inner = p.data;
      *inner_len = p.data_len;
      return 2;
   }
   t = turn_find_txn(c, p.txid);
   if (!t)
      return 0;
   switch (p.type & 0x0110)
   {
      case 0x0100: turn_on_success(c, t, &p, c->clock_ms); return 1;
      case 0x0110: turn_on_error(c, t, &p, c->clock_ms); return 1;
      default:     return 0;
   }
}

size_t nat_turn_channel_wrap(uint8_t *out, size_t cap, uint16_t channel, const void *payload, size_t len)
{
   if (len > 0xFFFF || cap < len + 4)
      return 0;
   turn_put16(out, channel);
   turn_put16(out + 2, (uint32_t)len);
   if (len)
      memcpy(out + 4, payload, len);
   return len + 4;
}

uint16_t nat_turn_channel_unwrap(const uint8_t *d, size_t n, const uint8_t **payload, size_t *len)
{
   size_t l;
   if (n < 4 || (d[0] & 0xC0) != 0x40)
      return 0;
   l = turn_get16(d + 2);
   if (4 + l > n)
      return 0;                           /* truncated */
   *payload = d + 4;
   *len = l;
   return (uint16_t)turn_get16(d);
}

int nat_turn_state(const nat_turn *c)
{
   return c->state;
}

int nat_turn_error(const nat_turn *c)
{
   return c->err;
}

int nat_turn_relayed(const nat_turn *c, uint32_t *ip_be, uint16_t *port)
{
   if (c->state != NAT_RELAY_ALLOCATED && c->state != NAT_RELAY_BINDING && c->state != NAT_RELAY_READY)
      return 0;
   if (ip_be)
      *ip_be = c->relayed_ip_be;
   if (port)
      *port = c->relayed_port;
   return 1;
}
