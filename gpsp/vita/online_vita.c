/* online_vita.c -- one online connection (2.0); see online_vita.h.
 *
 * The state machine of the plan's 2.4: STUN, the room service, the punch
 * race with the relay credentials fetched beside it, the relay fallback,
 * and a short echo measurement.  Single instance, nothing blocks: every
 * call to online_step() looks at the answers that arrived, looks at the
 * NAT layer's status, and sends at most a few requests.  The caller supplies
 * the transport (the NAT layer lives in transport_lan.c, package N) and the
 * HTTP client is online_http.cpp (package H); neither is touched here
 * except through their headers.
 *
 * Why one flat file with one static state: the console has one Wi-Fi radio
 * and one link at a time, and link_vita.c already keeps one static state
 * the same way.  The caller is the emulation thread, once per frame.
 *
 * Share game data reuses this as it is: it passes a LAN_KIND_SHARE room and
 * reads the same stages; nothing here knows about the link kinds. */
#include <stdio.h>
#include <string.h>

#include "online_vita.h"
#include "plat.h"                 /* plat_time_us: the default clock */
#include "fe_evt.h"

/* ---- limits, all from the plan (times in milliseconds) ------------------- */
#define T_RESOLVE_MS       6000   /* one name lookup, then one retry */
#define T_STUN_MS          8000   /* the NAT layer gives up at 5.25 s; this is the net under it */
#define T_PUNCH_SLACK_MS   2000   /* our own net under the transport's punch timeout */
#define T_PUNCH_DEFAULT_MS 4000
#define T_JOINER_MS       45000   /* the joiner gives up this long after its join */
#define T_RELAY_PORT_MS    3000   /* the transport moves to the next port after this */
#define T_RELAY_WAIT_MS   15000   /* for the peer's relayed address */
#define T_RELAY_BIND_MS    5000   /* permission + channel */
#define T_POLL_MIN_MS      1000
#define T_POLL_MAX_MS     15000
#define T_STUN_KEEP_MS    15000
#define T_RETRY_MS         1000   /* before a failed create/join/credential request is sent again */
#define T_CREDS_403_MS     3000   /* before relay credentials are asked again after a 403 (the host has not polled yet) */
#define CREDS_TRIES_403      12   /* the service gives credentials once both members polled: about 30 s of asking */
#define T_POLL_WAIT_MAX_S    30   /* a poll told to wait longer than this (429) ends the attempt instead */
#define T_SLOW_MAX_S        120   /* a 429 that clears within this reads "wait a minute", not "busy today" */
#define RCV_TRIES_MAX          3  /* sends of a create / join repeated only to learn its token (see online_stop) */
#define ROOM_TTL_DEFAULT_S   600
#define NET_POLLS_MAX          3
#define MEASURE_ECHOES         6
#define MEASURE_GAP            4  /* frames between echoes */
#define MEASURE_FRAMES        24
#define GREAT_US           80000
#define GOOD_US           120000
#define RQ_MAX                 8  /* answers waiting for the next step */

typedef struct stun_ent
{
   char     host[64];
   uint16_t port;
   uint32_t ip_be;
   int      have_ip, dead;
} stun_ent;

typedef struct closing
{
   int  pending;          /* a close still has to be sent */
   int  sent;             /* how many times it went out (2 at most) */
   char code[6], token[24], reason[12];
   /* A create / join whose answer never arrived and that online_stop abandoned: the service may have made the
    * room (or taken us in).  The same request, with the same request id, is sent again only to get the token
    * back, and the room is closed with it.  Survives online_stop like the close itself. */
   int  rcv;              /* 0 none, 1 create, 2 join: still to learn the token */
   int  rcv_out;          /* the repeated request is in flight */
   int  rcv_tries;
   uint64_t rcv_at;       /* not before */
   char rid[17], jcode[6];
   online_desc desc;
} closing;

static struct
{
   online_cfg  cfg;
   int         cfg_set;

   /* the connection */
   int         conn;                     /* online_host / online_join happened, online_stop not yet */
   int         stage;
   int         is_host;
   lan_transport *t;
   online_desc desc;
   uint64_t    t_stage;                  /* when the stage began */

   /* room */
   int         have_room;                /* we hold a code and a token on the service */
   char        code[6], token[24];
   uint8_t     key[8];
   int         ttl_s, poll_ms;
   uint64_t    t_code, t_join, next_poll;
   int         room_gone;                /* the service told us the room is over: no close needed */
   int         saw_peer;                 /* a poll answer showed the guest (host) */
   char        posted_pub[24];           /* the pub the service knows */
   int         net_polls;
   int         create_tries, join_tries;
   char        rid[17];                  /* the request id of this attempt: the same on every retry of its create / join */
   int         rid_unknown;              /* a create / join went out and no definite answer came back */
   uint64_t    retry_at;
   int         req_sent;                 /* the stage's own request is out */

   /* STUN */
   stun_ent    st[2];
   int         nst, st_idx;              /* servers; the one being resolved */
   int         st_ok;                    /* the NAT layer holds a mapping (keep-alive on) */
   int         resolve_for;              /* 0 none, 1 a STUN host, 2 the TURN host */
   uint64_t    t_resolve;
   int         resolve_tries;
   int         stun_started;
   int         stun_fb_tried;            /* fell back from default STUN provider once */

   /* the peer */
   int         peer_known, mode_relay, relayed;
   char        peer_nick[16], peer_pub[24], peer_lan[24];
   int         punch_ms, punch_started;
   uint64_t    t_punch0;
   uint32_t    peer_ip_be;  uint16_t peer_port;

   /* relay */
   int         creds_ok, creds_asked, creds_err, creds_tries;
   char        turn_host[64], turn_user[160], turn_cred[160];
   uint16_t    turn_port[3];
   int         turn_ports;
   uint32_t    turn_ip_be;  int turn_ip_ok;
   int         turn_resolve_failed;
   int         alloc_started, alloc_posted, bound;
   uint64_t    t_alloc, t_bind;
   char        relay_peer[24];
   uint32_t    relay_ip_be;  uint16_t relay_port;

   /* measure */
   int         m_frame;
   int         quality, rtt_ms;

   /* result */
   int         fail, fail_http;
   char        host_ver[16];

   /* what the next step sends / what was sent when (for the events) */
   uint64_t    t_req[10];
   int         close_linked_asked;

   /* answers taken from the HTTP client, waiting for the step */
   online_reply rq[RQ_MAX];
   int          rq_n;

   closing     cl;                       /* outlives online_stop */

   /* the room card (UI code pad) */
   int         card_asked, card_have;
   int         card_sent;                /* the lookup of card_code is out */
   char        card_code[9];
   online_reply card;
} O;

/* ---- small helpers -------------------------------------------------------- */

static uint64_t now_ms(void)
{
   uint64_t us = O.cfg.now_us ? O.cfg.now_us() : plat_time_us();
   return us / 1000;
}

static void set_stage(int to)
{
   if (to == O.stage)
      return;
   fe_evt("online_stage from=%d to=%d", O.stage, to);
   O.stage = to;
   O.t_stage = now_ms();
}

/* "a.b.c.d" -> network-order address; strict, four decimal octets. */
static int parse_ip(const char *s, uint32_t *ip_be, const char **end)
{
   uint8_t b[4];
   int i;
   for (i = 0; i < 4; i++)
   {
      unsigned v = 0;
      int digits = 0;
      while (*s >= '0' && *s <= '9')
      {
         v = v * 10 + (unsigned)(*s - '0');
         if (v > 255)
            return 0;
         s++;
         digits++;
      }
      if (!digits)
         return 0;
      b[i] = (uint8_t)v;
      if (i < 3)
      {
         if (*s != '.')
            return 0;
         s++;
      }
   }
   memcpy(ip_be, b, 4);
   if (end)
      *end = s;
   return 1;
}

/* "a.b.c.d:port" */
static int parse_addr(const char *s, uint32_t *ip_be, uint16_t *port)
{
   const char *e;
   unsigned p = 0;
   if (!s || !parse_ip(s, ip_be, &e) || *e != ':')
      return 0;
   e++;
   if (*e < '0' || *e > '9')
      return 0;
   while (*e >= '0' && *e <= '9')
   {
      p = p * 10 + (unsigned)(*e - '0');
      if (p > 65535)
         return 0;
      e++;
   }
   if (*e || p == 0)
      return 0;
   *port = (uint16_t)p;
   return 1;
}

static void fmt_addr(char *out, size_t cap, uint32_t ip_be, uint16_t port)
{
   uint8_t b[4];
   memcpy(b, &ip_be, 4);
   snprintf(out, cap, "%u.%u.%u.%u:%u", b[0], b[1], b[2], b[3], (unsigned)port);
}

static void copy_str(char *dst, size_t cap, const char *src)
{
   if (!cap)
      return;
   size_t n = 0;
   if (!src)
      src = "";
   while (src[n] && n < cap - 1)
      n++;
   memcpy(dst, src, n);
   dst[n] = 0;
}

static int hexv(char c)
{
   if (c >= '0' && c <= '9') return c - '0';
   if (c >= 'a' && c <= 'f') return c - 'a' + 10;
   if (c >= 'A' && c <= 'F') return c - 'A' + 10;
   return -1;
}

/* The room's punchKey is 16 hex digits = the 8 bytes that ride in PUNCH.  A
 * service that sent something else still gets a key: its first 8 bytes. */
static void key_from_hex(uint8_t key[8], const char *hex)
{
   int i, ok = (int)strlen(hex) == 16;
   for (i = 0; ok && i < 16; i++)
      if (hexv(hex[i]) < 0)
         ok = 0;
   if (ok)
   {
      for (i = 0; i < 8; i++)
         key[i] = (uint8_t)(hexv(hex[2 * i]) * 16 + hexv(hex[2 * i + 1]));
      return;
   }
   memset(key, 0, 8);
   for (i = 0; i < 8 && hex[i]; i++)
      key[i] = (uint8_t)hex[i];
}

static int clamp_poll(int ms)
{
   return ms < T_POLL_MIN_MS ? T_POLL_MIN_MS : ms > T_POLL_MAX_MS ? T_POLL_MAX_MS : ms;
}

/* The poll table the service uses, for an answer that lacks pollMs. */
static int poll_table(void)
{
   uint64_t age = (now_ms() - O.t_code) / 1000;
   if (O.stage >= ON_PUNCH)
      return 1500;
   return age < 30 ? 2000 : age < 120 ? 5000 : 10000;
}

static int req_ok(int op, int rc)
{
   if (rc == 0 && op >= 0 && op < 10)
      O.t_req[op] = now_ms();
   return rc == 0;
}

/* ---- closing the room: survives online_stop ------------------------------- */

static void close_queue(const char *reason)
{
   if (!O.have_room || O.room_gone)
      return;
   copy_str(O.cl.code, sizeof O.cl.code, O.code);
   copy_str(O.cl.token, sizeof O.cl.token, O.token);
   copy_str(O.cl.reason, sizeof O.cl.reason, reason);
   O.cl.pending = 1;
   O.cl.sent = 0;
}

/* The abandoned create / join, sent again (same request id) to learn the token. */
static void recover_send(void)
{
   int op, rc;
   if (!O.cl.rcv || O.cl.rcv_out || O.cl.pending || now_ms() < O.cl.rcv_at)
      return;
   op = O.cl.rcv == 1 ? ONLINE_OP_CREATE : ONLINE_OP_JOIN;
   if (online_http_busy(op))
      return;
   rc = O.cl.rcv == 1 ? online_http_create(&O.cl.desc, O.cl.rid) : online_http_join(O.cl.jcode, &O.cl.desc, O.cl.rid);
   if (rc == 0)
   {
      O.t_req[op] = now_ms();
      O.cl.rcv_out = 1;
      O.cl.rcv_tries++;
   }
}

static void recover_answer(const online_reply *r)
{
   O.cl.rcv_out = 0;
   if (r->err == ONLINE_E_NONE && r->token[0] && (O.cl.rcv == 1 ? r->code[0] : O.cl.jcode[0]))
   {
      copy_str(O.cl.code, sizeof O.cl.code, O.cl.rcv == 1 ? r->code : O.cl.jcode);
      copy_str(O.cl.token, sizeof O.cl.token, r->token);
      copy_str(O.cl.reason, sizeof O.cl.reason, O.cl.rcv == 1 ? "cancel" : "failed");
      O.cl.pending = 1;
      O.cl.sent = 0;
      O.cl.rcv = 0;
   }
   else if (r->err == ONLINE_E_NET && O.cl.rcv_tries < RCV_TRIES_MAX)
      O.cl.rcv_at = now_ms() + T_RETRY_MS;
   else
      O.cl.rcv = 0;           /* the service refused, or we gave up: the room expires by itself */
}

static void close_service(void)
{
   recover_send();
   if (!O.cl.pending || online_http_busy(ONLINE_OP_CLOSE))
      return;
   if (online_http_close(O.cl.code, O.cl.token, O.cl.reason) == 0)
   {
      O.t_req[ONLINE_OP_CLOSE] = now_ms();
      O.cl.sent++;
      O.cl.pending = 0;      /* back to pending only if the answer is a failure (pump) */
   }
}

/* ---- failure ------------------------------------------------------------- */

static void relay_down(void)
{
   lan_nat_status ns;
   if (!O.t)
      return;
   lan_transport_nat_status(O.t, &ns);
   lan_transport_peer_lock(O.t, 0, 0);
   lan_transport_punch_stop(O.t);
   if (ns.relay != LAN_RELAY_OFF)
      lan_transport_relay_stop(O.t);
}

static void fail(int why, int http)
{
   if (O.stage == ON_FAILED)
      return;
   O.fail = why;
   O.fail_http = http;
   fe_evt("online_fail why=%d http=%d code=%s", why, http, O.code);
   relay_down();
   if (O.have_room && !O.room_gone && !O.cl.pending)
   {
      /* drop first: a close sent now must not be the one that is dropped */
      online_http_drop();
      O.rq_n = 0;
      close_queue(O.is_host ? "cancel" : "failed");
      close_service();
   }
   set_stage(ON_FAILED);
}

static int map_err(int err, int op, int retry_after)
{
   switch (err)
   {
      case ONLINE_E_NOT_FOUND:
         return (op == ONLINE_OP_JOIN || op == ONLINE_OP_LOOKUP) ? ONLINE_FAIL_CODE
                                                                  : ONLINE_FAIL_EXPIRED;
      case ONLINE_E_FULL:    return ONLINE_FAIL_FULL;
      case ONLINE_E_VERSION: return ONLINE_FAIL_VERSION;
      case ONLINE_E_UPDATE:  return ONLINE_FAIL_UPDATE;
      case ONLINE_E_BUSY:    return ONLINE_FAIL_BUSY;
      case ONLINE_E_RATE:    /* slow_down, too_many_rooms, guessing, ...: the wait says which sentence is true */
         return retry_after > 0 && retry_after <= T_SLOW_MAX_S ? ONLINE_FAIL_SLOW : ONLINE_FAIL_BUSY;
      case ONLINE_E_TOKEN:   return ONLINE_FAIL_CLOSED;
      case ONLINE_E_RELAY:
      case ONLINE_E_RELAY_WAIT:
         return ONLINE_FAIL_UNREACHABLE;
      case ONLINE_E_CLOCK:   return ONLINE_FAIL_CLOCK;
      case ONLINE_E_CONSOLE: return ONLINE_FAIL_CONSOLE;
      default:               return ONLINE_FAIL_SERVICE;   /* NET, CERT, SERVER, BAD */
   }
}

/* A reply with an error that ends the attempt.  The service no longer holds
 * a room we can close when it said the room or our token is gone. */
static void fail_from_reply(const online_reply *r)
{
   int why = map_err(r->err, r->op, r->retry_after);
   if (r->err == ONLINE_E_NOT_FOUND || r->err == ONLINE_E_TOKEN)
      O.room_gone = 1;
   if (r->err == ONLINE_E_VERSION)
      copy_str(O.host_ver, sizeof O.host_ver, r->host_ver);
   fail(why, r->http);
}

/* ---- setup and the entries ------------------------------------------------ */

static void ensure_cfg(void)
{
   if (!O.cfg_set)
      online_setup(NULL);
}

void online_setup(const online_cfg *cfg)
{
   memset(&O, 0, sizeof O);
   if (cfg)
      O.cfg = *cfg;
   else
      O.cfg.post_lan = 1;
   if (!O.cfg.url || !O.cfg.url[0])
      O.cfg.url = ONLINE_URL_DEFAULT;
   if (!O.cfg.stun || !O.cfg.stun[0])
      O.cfg.stun = ONLINE_STUN_DEFAULT;
   O.cfg_set = 1;
   /* the strings must outlive the caller's: both are literals or the
    * caller's long-lived config, as the header says ("copied" is the
    * struct; the pointers are read again at each online_host / online_join). */
   online_http_config(O.cfg.url, ONLINE_APP_VERSION);
}

static void parse_stun_str(const char *s)
{
   O.nst = 0;
   O.st_idx = 0;
   if (!s || !strcmp(s, "off"))
      return;
   while (*s && O.nst < 2)
   {
      stun_ent *e = &O.st[O.nst];
      size_t n = 0;
      unsigned p = 0;
      memset(e, 0, sizeof *e);
      while (s[n] && s[n] != ':' && s[n] != ',' && n < sizeof(e->host) - 1)
         n++;
      memcpy(e->host, s, n);
      s += n;
      if (*s == ':')
      {
         s++;
         while (*s >= '0' && *s <= '9' && p < 65536)
            p = p * 10 + (unsigned)(*s++ - '0');
      }
      while (*s && *s != ',')
         s++;
      if (*s == ',')
         s++;
      if (!e->host[0] || p == 0 || p > 65535)
         continue;
      e->port = (uint16_t)p;
      if (parse_ip(e->host, &e->ip_be, NULL))
         e->have_ip = 1;
      O.nst++;
   }
}

static void parse_stun_list(void)
{
   parse_stun_str(O.cfg.stun);
}

/* Forget the connection, keep what outlives it: the configuration, a close
 * that is still to be sent, and the room card the UI may be reading. */
static void reset_conn(void)
{
   closing keep = O.cl;
   online_cfg c = O.cfg;
   int set = O.cfg_set, ca = O.card_asked, ch = O.card_have, cs = O.card_sent;
   online_reply card = O.card;
   char cc[sizeof O.card_code];
   memcpy(cc, O.card_code, sizeof cc);
   memset(&O, 0, sizeof O);
   O.cfg = c;
   O.cfg_set = set;
   O.cl = keep;
   O.card_asked = ca;
   O.card_have = ch;
   O.card_sent = cs;
   O.card = card;
   memcpy(O.card_code, cc, sizeof cc);
}

/* The NAT layer refuses punch_arm, stun_start and relay_start until it has
 * been seeded.  Seed it here, once per connection, and not only on the way
 * into STUN: "Always use the relay", stun "off" and a STUN name that does
 * not resolve all skip that stage and still need the punch key armed or
 * the relay allocated. */
static void seed_nat(void)
{
   uint8_t seed[16];
   int i;
   if (O.cfg.entropy)
      O.cfg.entropy(seed);
   else
   {
      /* no real source given (tests only: the console's caller always has one) */
      uint64_t t = plat_time_us();
      for (i = 0; i < 16; i++)
         seed[i] = (uint8_t)((t >> ((i & 7) * 8)) ^ (uint64_t)(i * 37 + 11));
   }
   lan_transport_nat_seed(O.t, seed);
}

static void begin_common(lan_transport *t, const online_desc *d, int host)
{
   reset_conn();
   O.t = t;
   O.is_host = host;
   O.desc = *d;
   /* always the app version (digits and dots: the service refuses anything
    * else, e.g. a build label like "2.0-dev"), whatever the caller put there */
   copy_str(O.desc.ver, sizeof O.desc.ver, ONLINE_APP_VERSION);
   O.desc.pub[0] = 0;
   O.desc.lan[0] = 0;
   O.desc.sym = 0;
   O.conn = 1;
   O.t_code = now_ms();
   O.t_stage = O.t_code;
   O.poll_ms = 2000;
   parse_stun_list();
   seed_nat();
   /* Nobody is this connection's peer until the room says who (stage_measure
    * locks the transport to it): until then no session datagram is taken. */
   lan_transport_peer_hold(O.t);
}

int online_host(lan_transport *t, const online_desc *room)
{
   ensure_cfg();
   if (!t || !room || O.conn)
      return -1;
   begin_common(t, room, 1);
   fe_evt("online_stage from=%d to=%d", ON_IDLE, ON_RESOLVE);
   O.stage = ON_RESOLVE;
   return 0;
}

int online_join(lan_transport *t, const char *code, const online_desc *me)
{
   size_t i, n;
   ensure_cfg();
   if (!t || !me || !code || O.conn)
      return -1;
   n = strlen(code);
   if (n < 1 || n > 5)
      return -1;
   begin_common(t, me, 0);
   for (i = 0; i < n; i++)
      O.code[i] = (code[i] >= 'a' && code[i] <= 'z') ? (char)(code[i] - 32) : code[i];
   O.code[n] = 0;
   fe_evt("online_stage from=%d to=%d", ON_IDLE, ON_RESOLVE);
   O.stage = ON_RESOLVE;
   return 0;
}

int online_active(void)
{
   return O.conn && O.stage != ON_IDLE && O.stage != ON_FAILED;
}

void online_get(online_view *out)
{
   if (!out)
      return;
   memset(out, 0, sizeof *out);
   out->stage = O.stage;
   out->is_host = O.is_host;
   out->relayed = O.relayed;
   out->quality = O.quality;
   out->rtt_ms = O.rtt_ms;
   out->fail = O.fail;
   copy_str(out->code, sizeof out->code, O.code);
   copy_str(out->peer_nick, sizeof out->peer_nick, O.peer_nick);
   copy_str(out->host_ver, sizeof out->host_ver, O.host_ver);
   out->ttl_s = (unsigned)(O.ttl_s ? O.ttl_s : ROOM_TTL_DEFAULT_S);
   if (O.conn && O.is_host && O.have_room)
   {
      /* counts up while the code waits, stops when a friend is there */
      uint64_t end = O.t_join ? O.t_join : now_ms();
      out->waited_s = (unsigned)((end - O.t_code) / 1000);
   }
   out->peer_ip_be = O.peer_ip_be;
   out->peer_port = O.peer_port;
}

/* ---- the card ------------------------------------------------------------- */

/* The lookup of the card that is wanted goes out once no older lookup is
 * in flight.  online_http_drop() cancels everything or nothing, so a lookup
 * the player backed out of cannot be taken back by itself: it is left to
 * finish and its answer is thrown away (pump).  Sending the next one only
 * after that is what keeps a late answer for the LAST code from being shown
 * as the card of THIS one (answers carry their op, not their code). */
static void card_send(void)
{
   if (!O.card_asked || O.card_sent || online_http_busy(ONLINE_OP_LOOKUP))
      return;
   if (online_http_lookup(O.card_code) == 0)
   {
      O.t_req[ONLINE_OP_LOOKUP] = now_ms();
      O.card_sent = 1;
   }
}

int online_card_begin(const char *code)
{
   ensure_cfg();
   if (!code || !code[0] || O.card_asked)
      return -1;
   O.card_have = 0;
   copy_str(O.card_code, sizeof O.card_code, code);
   O.card_asked = 1;
   O.card_sent = 0;
   card_send();          /* now, or from pump() when a slot is free */
   return 0;
}

void online_card_cancel(void)
{
   O.card_asked = 0;
   O.card_sent = 0;
   O.card_have = 0;     /* a late answer is discarded in pump() */
}

/* ---- answers ----------------------------------------------------------------- */

static void pump(void)
{
   online_reply r;
   while (online_http_take(&r))
   {
      fe_evt("online_http op=%d status=%d ms=%u", r.op, r.http,
             (unsigned)(r.op >= 0 && r.op < 10 ? now_ms() - O.t_req[r.op] : 0));
      if (r.op == ONLINE_OP_LOOKUP)
      {
         if (O.card_asked && O.card_sent)
         {
            O.card = r;
            O.card_have = 1;
            O.card_asked = 0;
            O.card_sent = 0;
         }
      }
      else if (r.op == ONLINE_OP_CLOSE)
      {
         /* fire and forget, one retry */
         if (r.err != ONLINE_E_NONE && O.cl.sent < 2 && O.cl.code[0])
            O.cl.pending = 1;
      }
      else if (O.cl.rcv_out && (r.op == ONLINE_OP_CREATE || r.op == ONLINE_OP_JOIN))
         recover_answer(&r);
      else if (O.conn && O.rq_n < RQ_MAX)
         O.rq[O.rq_n++] = r;
   }
   close_service();
   card_send();
}

int online_card_poll(online_reply *card)
{
   pump();
   if (!O.card_have)
   {
      if (card)
         memset(card, 0, sizeof *card);
      return 0;
   }
   if (card)
      *card = O.card;
   O.card_have = 0;
   return 1;
}

/* ---- starting and stopping the transport's machines ---------------------------- */

static void adopt_peer(const online_reply *r)
{
   copy_str(O.peer_nick, sizeof O.peer_nick, r->peer_nick);
   copy_str(O.peer_pub, sizeof O.peer_pub, r->peer_pub);
   copy_str(O.peer_lan, sizeof O.peer_lan, r->peer_lan);
   O.mode_relay = r->mode == ONLINE_MODE_RELAY;
   if (!O.t_join)
      O.t_join = now_ms();
   if (!O.peer_known)
      fe_evt("online_peer nick=\"%s\" mode=%s", O.peer_nick, O.mode_relay ? "relay" : "punch");
   O.peer_known = 1;
   if (r->punch_ms > 0)
      O.punch_ms = r->punch_ms;
}

static int punch_window(void)
{
   if (O.cfg.punch_ms)
      return (int)O.cfg.punch_ms;
   return O.punch_ms > 0 ? O.punch_ms : T_PUNCH_DEFAULT_MS;
}

static void punch_begin(void)
{
   uint32_t ip[2];
   uint16_t port[2];
   int n = 0;
   lan_nat_status ns;

   if (O.punch_started || !O.peer_known)
      return;
   if (parse_addr(O.peer_pub, &ip[n], &port[n]))
      n++;
   if (n < 2 && parse_addr(O.peer_lan, &ip[n], &port[n]))
      n++;
   if (!n)
      return;
   lan_transport_nat_status(O.t, &ns);
   if (ns.punch == LAN_PUNCH_DONE)
   {
      O.punch_started = 1;
      return;
   }
   if (lan_transport_punch_start(O.t, ip, port, n, (unsigned)punch_window()) == 0)
   {
      O.punch_started = 1;
      O.t_punch0 = now_ms();
   }
}

static void enter_relay(void)
{
   if (O.cfg.no_relay)
   {
      fail(ONLINE_FAIL_UNREACHABLE, 0);
      return;
   }
   O.relayed = 1;
   set_stage(ON_RELAY_CREDS);
}

static void enter_punch(void)
{
   O.t_punch0 = now_ms();
   set_stage(ON_PUNCH);
   punch_begin();
}

/* Where a peer who is known goes next. */
static void peer_ready(void)
{
   if (O.mode_relay || O.cfg.force_relay)
      enter_relay();
   else
      enter_punch();
}

/* The host's guest left before the link formed: wait for another. */
static void guest_left(void)
{
   lan_nat_status ns;
   lan_transport_nat_status(O.t, &ns);
   lan_transport_peer_hold(O.t);          /* the next guest is somebody else, and not known yet */
   lan_transport_punch_stop(O.t);
   lan_transport_punch_arm(O.t, O.key);
   if (ns.relay != LAN_RELAY_OFF)
      lan_transport_relay_stop(O.t);
   O.peer_known = O.saw_peer = O.punch_started = O.mode_relay = O.relayed = 0;
   O.alloc_started = O.alloc_posted = O.bound = 0;
   O.creds_asked = O.creds_ok = O.creds_err = O.creds_tries = 0;
   O.turn_ip_ok = O.turn_resolve_failed = 0;
   if (O.resolve_for == 2)
      O.resolve_for = 0;
   O.retry_at = 0;
   O.relay_peer[0] = O.peer_nick[0] = O.peer_pub[0] = O.peer_lan[0] = 0;
   O.t_join = 0;
   O.next_poll = now_ms() + (uint64_t)poll_table();
   set_stage(ON_WAITING);
}

/* ---- per-reply handlers ----------------------------------------------------- */

static void on_create(const online_reply *r)
{
   if (O.stage != ON_CREATE)
      return;
   if (r->err != ONLINE_E_NET)
      O.rid_unknown = 0;                  /* a definite answer: nothing is left to recover */
   if (r->err != ONLINE_E_NONE)
   {
      if (r->err == ONLINE_E_NET && O.create_tries < 2)
      {
         O.create_tries++;
         O.req_sent = 0;
         O.retry_at = now_ms() + T_RETRY_MS;
         return;
      }
      fail_from_reply(r);
      return;
   }
   if (!r->code[0] || !r->token[0])
   {
      fail(ONLINE_FAIL_SERVICE, r->http);
      return;
   }
   copy_str(O.code, sizeof O.code, r->code);
   copy_str(O.token, sizeof O.token, r->token);
   key_from_hex(O.key, r->punch_key);
   O.have_room = 1;
   O.ttl_s = r->ttl_sec > 0 ? r->ttl_sec : ROOM_TTL_DEFAULT_S;
   O.poll_ms = r->poll_ms > 0 ? clamp_poll(r->poll_ms) : 2000;
   O.t_code = now_ms();
   O.next_poll = O.t_code + (uint64_t)O.poll_ms;
   copy_str(O.posted_pub, sizeof O.posted_pub, O.desc.pub);
   lan_transport_punch_arm(O.t, O.key);
   if (O.st_ok)
      lan_transport_stun_keep(O.t, T_STUN_KEEP_MS);
   fe_evt("online_room code=%s ttl=%d", O.code, O.ttl_s);
   set_stage(ON_WAITING);
}

static void on_join(const online_reply *r)
{
   if (O.stage != ON_JOIN)
      return;
   if (r->err != ONLINE_E_NET)
      O.rid_unknown = 0;
   if (r->err != ONLINE_E_NONE)
   {
      if (r->err == ONLINE_E_NET && O.join_tries < 1)
      {
         O.join_tries++;
         O.req_sent = 0;
         O.retry_at = now_ms() + T_RETRY_MS;
         return;
      }
      fail_from_reply(r);
      return;
   }
   if (!r->token[0])
   {
      fail(ONLINE_FAIL_SERVICE, r->http);
      return;
   }
   copy_str(O.token, sizeof O.token, r->token);
   key_from_hex(O.key, r->punch_key);
   O.have_room = 1;
   O.t_join = now_ms();
   O.poll_ms = r->poll_ms > 0 ? clamp_poll(r->poll_ms) : 1500;
   O.next_poll = O.t_join + (uint64_t)O.poll_ms;
   lan_transport_punch_arm(O.t, O.key);
   adopt_peer(r);
   peer_ready();
}

static void on_relay_reply(const online_reply *r)
{
   O.creds_asked = 0;
   if (O.stage < ON_PUNCH || O.stage > ON_RELAY_CREDS)
      return;
   if (r->err != ONLINE_E_NONE)
   {
      /* during the punch this is not fatal: the punch may still win.  At the
       * fallback it is looked at again (retry once on a network failure). */
      O.creds_err = r->err;
      O.creds_ok = 0;
      if (O.stage == ON_RELAY_CREDS)
      {
         if (r->err == ONLINE_E_NET && O.creds_tries < 2)
         {
            O.retry_at = now_ms() + T_RETRY_MS;
            return;
         }
         if (r->err == ONLINE_E_RELAY_WAIT && O.creds_tries < CREDS_TRIES_403)
         {
            O.retry_at = now_ms() + T_CREDS_403_MS;
            return;
         }
         if (r->err == ONLINE_E_CLOCK || r->err == ONLINE_E_UPDATE || r->err == ONLINE_E_BUSY ||
             r->err == ONLINE_E_TOKEN || r->err == ONLINE_E_CONSOLE)
            fail_from_reply(r);
         else
            fail(ONLINE_FAIL_UNREACHABLE, r->http);
      }
      return;
   }
   if (!r->turn_host[0] || r->turn_ports < 0)
   {
      O.creds_err = ONLINE_E_BAD;
      if (O.stage == ON_RELAY_CREDS)
         fail(ONLINE_FAIL_UNREACHABLE, r->http);
      return;
   }
   copy_str(O.turn_host, sizeof O.turn_host, r->turn_host);
   copy_str(O.turn_user, sizeof O.turn_user, r->turn_user);
   copy_str(O.turn_cred, sizeof O.turn_cred, r->turn_cred);
   O.turn_ports = r->turn_ports > 3 ? 3 : r->turn_ports;
   memcpy(O.turn_port, r->turn_port, sizeof O.turn_port);
   if (O.turn_ports == 0)
   {
      O.turn_port[0] = 3478;
      O.turn_ports = 1;
   }
   O.creds_ok = 1;
   O.creds_err = 0;
   O.turn_ip_ok = parse_ip(O.turn_host, &O.turn_ip_be, NULL);
}

static void on_resolve(const online_reply *r)
{
   if (O.resolve_for == 1)
   {
      stun_ent *e = &O.st[O.st_idx];
      O.resolve_for = 0;
      if (r->err == ONLINE_E_CLOCK)
      {
         fail(ONLINE_FAIL_CLOCK, r->http);
         return;
      }
      if (r->err == ONLINE_E_NONE && parse_ip(r->resolved, &e->ip_be, NULL))
      {
         e->have_ip = 1;
         O.st_idx++;
         O.resolve_tries = 0;
      }
      else if (O.resolve_tries < 2)
         ;                                   /* the stage resends */
      else
      {
         e->dead = 1;
         O.st_idx++;
         O.resolve_tries = 0;
      }
   }
   else if (O.resolve_for == 2)
   {
      O.resolve_for = 0;
      if (r->err == ONLINE_E_NONE && parse_ip(r->resolved, &O.turn_ip_be, NULL))
         O.turn_ip_ok = 1;
      else if (O.resolve_tries >= 2)
         O.turn_resolve_failed = 1;
   }
}

static void on_endpoint(const online_reply *r)
{
   if (O.stage != ON_RELAY_POST)
      return;
   if (r->err != ONLINE_E_NONE)
   {
      if (r->err == ONLINE_E_NET && O.net_polls < NET_POLLS_MAX - 1)
      {
         O.net_polls++;
         O.alloc_posted = 0;
         O.retry_at = now_ms() + T_RETRY_MS;
         return;
      }
      if (r->err == ONLINE_E_RELAY || r->err == ONLINE_E_NOT_FOUND)
         fail(ONLINE_FAIL_UNREACHABLE, r->http);
      else
         fail_from_reply(r);
      return;
   }
   O.net_polls = 0;
   if (r->relay_peer[0])
      copy_str(O.relay_peer, sizeof O.relay_peer, r->relay_peer);
   O.t_stage = now_ms();
   set_stage(ON_RELAY_WAIT);
}

static void on_poll(const online_reply *r)
{
   if (O.stage < ON_WAITING || O.stage > ON_RELAY_WAIT || !O.have_room)
      return;
   if (O.stage == ON_LOOKUP || O.stage == ON_JOIN)
      return;
   if (r->err != ONLINE_E_NONE)
   {
      if (r->err == ONLINE_E_NET)
      {
         if (++O.net_polls >= NET_POLLS_MAX)
            fail(ONLINE_FAIL_SERVICE, r->http);
         return;
      }
      if (r->err == ONLINE_E_RATE && r->retry_after <= T_POLL_WAIT_MAX_S)
      {
         O.next_poll = now_ms() + (uint64_t)(r->retry_after > 0 ? r->retry_after : 1) * 1000;
         return;
      }
      fail_from_reply(r);
      return;
   }
   O.net_polls = 0;
   if (r->state == ONLINE_ROOM_CLOSED)
   {
      O.room_gone = 1;
      fail(!strcmp(r->reason, "expired") ? ONLINE_FAIL_EXPIRED : ONLINE_FAIL_CLOSED, r->http);
      return;
   }
   if (r->poll_ms > 0)
      O.poll_ms = clamp_poll(r->poll_ms);
   else
      O.poll_ms = clamp_poll(poll_table());
   if (r->relay_peer[0])
      copy_str(O.relay_peer, sizeof O.relay_peer, r->relay_peer);

   if (O.is_host)
   {
      if (r->have_peer)
      {
         int first = !O.peer_known;
         O.saw_peer = 1;
         adopt_peer(r);
         if (O.stage == ON_WAITING)
            peer_ready();
         else if (first && O.stage == ON_PUNCH)
         {
            /* the joiner's PUNCH opened us before this poll */
            if (O.mode_relay)
               enter_relay();
            else
               punch_begin();
         }
      }
      else if (O.saw_peer && r->state == ONLINE_ROOM_OPEN)
      {
         guest_left();
         return;
      }
      else if (O.stage == ON_PUNCH && !O.peer_known && r->state == ONLINE_ROOM_OPEN &&
               O.t_req[ONLINE_OP_POLL] > O.t_punch0)
      {
         /* a joiner's PUNCH opened us (ON_WAITING -> ON_PUNCH), but a poll
          * sent AFTER that still shows nobody: the joiner gave up before we
          * ever saw it.  Back to waiting, or the code page would sit in the
          * punch stage until the room expires. */
         guest_left();
         return;
      }
   }
   /* a member posted its relayed address: the room is on the relay now */
   if ((r->state == ONLINE_ROOM_RELAY || r->mode == ONLINE_MODE_RELAY) && r->have_peer &&
       O.stage == ON_PUNCH)
      enter_relay();
}

static void drain(void)
{
   int i;
   for (i = 0; i < O.rq_n; i++)
   {
      const online_reply *r = &O.rq[i];
      if (O.stage == ON_FAILED || !O.conn)
         break;
      /* a CLOCK answer to anything ends the attempt wherever it comes from */
      switch (r->op)
      {
         case ONLINE_OP_CREATE:   on_create(r); break;
         case ONLINE_OP_JOIN:     on_join(r); break;
         case ONLINE_OP_POLL:     on_poll(r); break;
         case ONLINE_OP_RELAY:    on_relay_reply(r); break;
         case ONLINE_OP_ENDPOINT: on_endpoint(r); break;
         case ONLINE_OP_RESOLVE:  on_resolve(r); break;
         default: break;
      }
   }
   O.rq_n = 0;
}

/* ---- the stages ------------------------------------------------------------------ */

static void own_addr(char *out, size_t cap)
{
   if (!lan_transport_addr_str(O.t, out, cap))
      out[0] = 0;
}

/* 64 random bits as 16 hex digits: names this attempt's create / join to the service. */
static void make_rid(void)
{
   static const char hx[] = "0123456789abcdef";
   uint8_t seed[16];
   int i;
   if (O.cfg.entropy)
      O.cfg.entropy(seed);
   else
   {
      uint64_t t = plat_time_us();
      for (i = 0; i < 16; i++)
         seed[i] = (uint8_t)((t >> ((i & 7) * 8)) ^ (uint64_t)(i * 53 + 7));
   }
   /* the NAT layer was seeded from the same call: fold the bytes so the id is not the transaction ids' own bytes */
   for (i = 0; i < 8; i++)
      seed[i] = (uint8_t)(seed[i] ^ (seed[i + 8] * 31u) ^ (uint8_t)(i * 17 + 3));
   for (i = 0; i < 8; i++)
   {
      O.rid[2 * i] = hx[seed[i] >> 4];
      O.rid[2 * i + 1] = hx[seed[i] & 15];
   }
   O.rid[16] = 0;
}

static void go_create_or_join(void)
{
   make_rid();
   O.rid_unknown = 0;
   O.req_sent = 0;
   O.retry_at = 0;
   set_stage(O.is_host ? ON_CREATE : ON_JOIN);
}

/* ON_RESOLVE: the STUN servers' names. */
static void stage_resolve(void)
{
   uint64_t now = now_ms();

   if (O.desc.relay_only)
   {
      /* nothing about this console's address leaves it */
      O.desc.pub[0] = O.desc.lan[0] = 0;
      O.desc.sym = 0;
      go_create_or_join();
      return;
   }
   if (O.nst == 0)
   {
      /* "off": the transport's own address is the public one (tests, a console with a public address) */
      own_addr(O.desc.pub, sizeof O.desc.pub);
      if (O.cfg.post_lan)
         own_addr(O.desc.lan, sizeof O.desc.lan);
      fe_evt("online_stun pub=%s sym=0 rc=0", O.desc.pub);
      go_create_or_join();
      return;
   }
   /* skip entries that share a host with an earlier one: one lookup serves both */
   while (O.st_idx < O.nst)
   {
      stun_ent *e = &O.st[O.st_idx];
      int k;
      if (!e->have_ip)
         for (k = 0; k < O.st_idx; k++)
            if (!strcmp(O.st[k].host, e->host) && O.st[k].have_ip)
            {
               e->ip_be = O.st[k].ip_be;
               e->have_ip = 1;
            }
      if (e->have_ip || e->dead)
      {
         O.st_idx++;
         O.resolve_tries = 0;
         continue;
      }
      break;
   }
   if (O.st_idx >= O.nst)
   {
      set_stage(ON_STUN);
      return;
   }
   if (O.resolve_for == 1)
   {
      if (now - O.t_resolve > T_RESOLVE_MS)
      {
         O.resolve_for = 0;
         if (O.resolve_tries >= 2)
         {
            O.st[O.st_idx].dead = 1;
            O.st_idx++;
            O.resolve_tries = 0;
         }
      }
      return;
   }
   if (online_http_busy(ONLINE_OP_RESOLVE))
      return;
   if (online_http_resolve(O.st[O.st_idx].host) == 0)
   {
      O.resolve_for = 1;
      O.resolve_tries++;
      O.t_resolve = now;
      O.t_req[ONLINE_OP_RESOLVE] = now;
   }
}

/* ON_STUN */
static int stun_try_fallback(void)
{
   if (O.stun_fb_tried || !O.cfg.stun || strcmp(O.cfg.stun, ONLINE_STUN_DEFAULT) != 0)
      return 0;
   O.stun_fb_tried = 1;
   O.resolve_for = 0;
   O.resolve_tries = 0;
   O.stun_started = 0;
   parse_stun_str("stun.l.google.com:19302,stun1.l.google.com:19302");
   fe_evt("online_stun_fallback host=stun.l.google.com");
   set_stage(ON_RESOLVE);
   return 1;
}

static void stage_stun(void)
{
   uint32_t ip[2];
   uint16_t port[2];
   int i, n = 0;
   lan_nat_status ns;

   if (!O.stun_started)
   {
      for (i = 0; i < O.nst && n < 2; i++)
         if (O.st[i].have_ip && !O.st[i].dead)
         {
            ip[n] = O.st[i].ip_be;
            port[n] = O.st[i].port;
            n++;
         }
      O.stun_started = 1;
      if (n == 0)
      {
         if (stun_try_fallback())
            return;
         fe_evt("online_stun pub=%s sym=0 rc=-1", "");
         own_addr(O.desc.lan, sizeof O.desc.lan);
         if (!O.cfg.post_lan)
            O.desc.lan[0] = 0;
         go_create_or_join();
         return;
      }
      if (lan_transport_stun_start(O.t, ip, port, n) != 0)
      {
         if (stun_try_fallback())
            return;
         fe_evt("online_stun pub=%s sym=0 rc=-2", "");
         if (O.cfg.post_lan)
            own_addr(O.desc.lan, sizeof O.desc.lan);
         go_create_or_join();
         return;
      }
      O.t_resolve = now_ms();            /* the STUN deadline's start */
      return;
   }
   lan_transport_nat_status(O.t, &ns);
   if (ns.stun == LAN_STUN_OK)
   {
      fmt_addr(O.desc.pub, sizeof O.desc.pub, ns.pub_ip_be, ns.pub_port);
      O.desc.sym = ns.sym ? 1 : 0;
      O.st_ok = 1;
      fe_evt("online_stun pub=%s sym=%d rc=0", O.desc.pub, O.desc.sym);
   }
   else if (ns.stun == LAN_STUN_FAILED || now_ms() - O.t_resolve > T_STUN_MS)
   {
      if (stun_try_fallback())
         return;
      O.desc.pub[0] = 0;
      fe_evt("online_stun pub=%s sym=0 rc=%d", "", 1);
   }
   else
      return;
   if (O.cfg.post_lan)
      own_addr(O.desc.lan, sizeof O.desc.lan);
   go_create_or_join();
}

/* The poll, at the cadence the service asked for.  One in flight at a time. */
static void poll_maybe(void)
{
   uint64_t now = now_ms();
   const char *np = NULL;
   char buf[24];

   if (!O.have_room || online_http_busy(ONLINE_OP_POLL) || now < O.next_poll)
      return;
   if (O.is_host && O.stage == ON_WAITING && O.st_ok)
   {
      lan_nat_status ns;
      lan_transport_nat_status(O.t, &ns);
      if (ns.stun == LAN_STUN_OK)
      {
         fmt_addr(buf, sizeof buf, ns.pub_ip_be, ns.pub_port);
         if (strcmp(buf, O.posted_pub))
            np = buf;
      }
   }
   if (req_ok(ONLINE_OP_POLL, online_http_poll(O.code, O.token, np)))
   {
      if (np)
         copy_str(O.posted_pub, sizeof O.posted_pub, np);
      O.next_poll = now + (uint64_t)O.poll_ms;
   }
}

/* A create / join of an earlier connection that is still being recovered holds the slot: its answer must not be
 * taken for this attempt's. */
static void stage_create(void)
{
   if (O.req_sent || O.cl.rcv || online_http_busy(ONLINE_OP_CREATE) || now_ms() < O.retry_at)
      return;
   if (req_ok(ONLINE_OP_CREATE, online_http_create(&O.desc, O.rid)))
   {
      O.req_sent = 1;
      O.rid_unknown = 1;
   }
}

static void stage_join(void)
{
   if (O.req_sent || O.cl.rcv || online_http_busy(ONLINE_OP_JOIN) || now_ms() < O.retry_at)
      return;
   if (req_ok(ONLINE_OP_JOIN, online_http_join(O.code, &O.desc, O.rid)))
   {
      O.req_sent = 1;
      O.rid_unknown = 1;
   }
}

/* Relay credentials, and the TURN host's address, whenever they are wanted. */
static void relay_prefetch(void)
{
   if (O.cfg.no_relay)
      return;
   if (!O.creds_ok && !O.creds_asked && !online_http_busy(ONLINE_OP_RELAY) &&
       now_ms() >= O.retry_at)
   {
      /* beside the punch one try is enough (a failure is not fatal there);
       * at the fallback (stage RELAY_CREDS) there is one more */
      if (O.creds_tries >= (O.stage == ON_PUNCH ? 1 : O.creds_err == ONLINE_E_RELAY_WAIT ? CREDS_TRIES_403 : 2))
         return;
      if (req_ok(ONLINE_OP_RELAY, online_http_relay(O.code, O.token)))
      {
         O.creds_asked = 1;
         O.creds_tries++;
      }
   }
   if (O.creds_ok && !O.turn_ip_ok && !O.turn_resolve_failed)
   {
      uint64_t now = now_ms();
      if (O.resolve_for == 2)
      {
         if (now - O.t_resolve > T_RESOLVE_MS)
         {
            O.resolve_for = 0;
            if (O.resolve_tries >= 2)
               O.turn_resolve_failed = 1;
         }
      }
      else if (!online_http_busy(ONLINE_OP_RESOLVE) &&
               online_http_resolve(O.turn_host) == 0)
      {
         O.resolve_for = 2;
         O.resolve_tries++;
         O.t_resolve = now;
         O.t_req[ONLINE_OP_RESOLVE] = now;
      }
   }
}

static void stage_punch(void)
{
   uint64_t now = now_ms();
   lan_nat_status ns;

   if (O.cfg.force_relay)
   {
      enter_relay();
      return;
   }
   lan_transport_nat_status(O.t, &ns);
   if (!O.cfg.no_relay)
      relay_prefetch();
   poll_maybe();
   if (O.stage != ON_PUNCH)
      return;
   if (ns.punch == LAN_PUNCH_DONE)
   {
      /* wait a moment for the poll that names the peer, when the punch won first */
      if (O.peer_known || now - O.t_punch0 > 2000)
      {
         O.peer_ip_be = ns.peer_ip_be;
         O.peer_port = ns.peer_port;
         {
            char a[24];
            fmt_addr(a, sizeof a, ns.peer_ip_be, ns.peer_port);
            fe_evt("online_punch done=1 peer=%s lan=%d ms=%u", a, ns.peer_is_lan,
                   (unsigned)(now - O.t_punch0));
         }
         O.relayed = 0;
         set_stage(ON_MEASURE);
         O.m_frame = 0;
      }
      return;
   }
   if (ns.punch == LAN_PUNCH_TIMEOUT ||
       (O.punch_started && now - O.t_punch0 > (uint64_t)(punch_window() + T_PUNCH_SLACK_MS)) ||
       (!O.punch_started && O.peer_known && !O.mode_relay && now - O.t_punch0 >
        (uint64_t)(punch_window() + T_PUNCH_SLACK_MS)))
   {
      fe_evt("online_punch done=0 peer=%s lan=0 ms=%u", "", (unsigned)(now - O.t_punch0));
      enter_relay();
   }
}

static int ports_known(void) { return O.turn_ports > 0; }

static void stage_relay(void)
{
   uint64_t now = now_ms();
   lan_nat_status ns;

   relay_prefetch();
   poll_maybe();
   if (O.stage < ON_RELAY_CREDS || O.stage > ON_RELAY_WAIT)
      return;
   for (;;)
   {
      int before = O.stage;
      switch (O.stage)
      {
         case ON_RELAY_CREDS:
            if (O.creds_ok)
               set_stage(ON_RELAY_RESOLVE);
            else if (!O.creds_asked && O.creds_tries >= (O.creds_err == ONLINE_E_RELAY_WAIT ? CREDS_TRIES_403 : 2) &&
                     !O.cfg.no_relay)
               fail(ONLINE_FAIL_UNREACHABLE, 0);
            break;
         case ON_RELAY_RESOLVE:
            if (O.turn_ip_ok)
               set_stage(ON_RELAY_ALLOC);
            else if (O.turn_resolve_failed)
               fail(ONLINE_FAIL_UNREACHABLE, 0);
            break;
         case ON_RELAY_ALLOC:
            if (!O.alloc_started)
            {
               if (!ports_known() ||
                   lan_transport_relay_start(O.t, O.turn_ip_be, O.turn_port, O.turn_ports,
                                             O.turn_user, O.turn_cred) != 0)
               {
                  fail(ONLINE_FAIL_UNREACHABLE, 0);
                  break;
               }
               O.alloc_started = 1;
               O.t_alloc = now;
               break;
            }
            lan_transport_nat_status(O.t, &ns);
            if (ns.relay == LAN_RELAY_ALLOCATED || ns.relay == LAN_RELAY_BINDING ||
                ns.relay == LAN_RELAY_READY)
            {
               O.relay_ip_be = ns.relay_ip_be;
               O.relay_port = ns.relay_port;
               O.alloc_posted = 0;
               O.req_sent = 0;
               set_stage(ON_RELAY_POST);
            }
            else if (ns.relay == LAN_RELAY_FAILED ||
                     now - O.t_alloc > (uint64_t)(T_RELAY_PORT_MS * O.turn_ports + 1000))
               fail(ONLINE_FAIL_UNREACHABLE, 0);
            break;
         case ON_RELAY_POST:
            if (!O.alloc_posted && !online_http_busy(ONLINE_OP_ENDPOINT) && now >= O.retry_at)
            {
               char a[24];
               fmt_addr(a, sizeof a, O.relay_ip_be, O.relay_port);
               if (req_ok(ONLINE_OP_ENDPOINT, online_http_endpoint(O.code, O.token, a)))
                  O.alloc_posted = 1;
            }
            break;
         case ON_RELAY_WAIT:
            if (O.relay_peer[0])
            {
               uint32_t ip;
               uint16_t port;
               if (!O.bound)
               {
                  if (!parse_addr(O.relay_peer, &ip, &port) ||
                      lan_transport_relay_bind(O.t, ip, port) != 0)
                  {
                     fail(ONLINE_FAIL_UNREACHABLE, 0);
                     break;
                  }
                  O.bound = 1;
                  O.t_bind = now;
                  O.peer_ip_be = ip;
                  O.peer_port = port;
                  {
                     char a[24];
                     fmt_addr(a, sizeof a, O.relay_ip_be, O.relay_port);
                     fe_evt("online_relay alloc=%s peer=%s", a, O.relay_peer);
                  }
                  break;
               }
               lan_transport_nat_status(O.t, &ns);
               if (ns.relay == LAN_RELAY_READY)
               {
                  set_stage(ON_MEASURE);
                  O.m_frame = 0;
               }
               else if (ns.relay == LAN_RELAY_FAILED || now - O.t_bind > T_RELAY_BIND_MS)
                  fail(ONLINE_FAIL_UNREACHABLE, 0);
            }
            else if (now - O.t_stage > T_RELAY_WAIT_MS)
               fail(ONLINE_FAIL_UNREACHABLE, 0);
            break;
         default:
            break;
      }
      if (O.stage == before || O.stage < ON_RELAY_CREDS || O.stage > ON_RELAY_WAIT)
         break;
   }
}

static void stage_measure(void)
{
   uint32_t mn = 0;
   int n;
   uint8_t mac[6];

   if (O.m_frame == 0)
   {
      lan_transport_echo_reset(O.t);
      /* the transport refuses when it is not a joiner or already has a
       * session peer: netdrv's JOIN would then go nowhere, so say so now */
      if (!O.is_host && lan_transport_set_target(O.t, O.peer_ip_be, O.peer_port) != 0)
      {
         fe_evt("online_target rc=-1");
         fail(ONLINE_FAIL_UNREACHABLE, 0);
         return;
      }
      /* The peer is known: from here on the transport takes session datagrams
       * (netdrv, the bulk lane, keepalives, echo answers) from it alone, and
       * from the other addresses the room gave for it.  The port is open to
       * the Internet; nobody else has any business on it. */
      lan_transport_peer_lock(O.t, O.peer_ip_be, O.peer_port);
   }
   if (O.m_frame % MEASURE_GAP == 0 && O.m_frame / MEASURE_GAP < MEASURE_ECHOES)
   {
      lan_mac_from_addr(O.peer_ip_be, O.peer_port, mac);
      lan_transport_echo(O.t, mac);
   }
   O.m_frame++;
   if (O.m_frame < MEASURE_FRAMES)
      return;
   n = lan_transport_echo_result(O.t, &mn, NULL, NULL);
   if (n > 0)
   {
      O.rtt_ms = (int)((mn + 500) / 1000);
      O.quality = mn < GREAT_US ? ONLINE_Q_GREAT : mn < GOOD_US ? ONLINE_Q_GOOD : ONLINE_Q_SLOW;
   }
   else
   {
      O.rtt_ms = 0;
      O.quality = ONLINE_Q_NONE;
   }
   fe_evt("online_quality rtt_ms=%d q=%d relayed=%d", O.rtt_ms, O.quality, O.relayed);
   online_http_drop();            /* nothing more is asked of the service but the close */
   O.rq_n = 0;
   set_stage(ON_CONNECTED);
}

/* ---- the frame ----------------------------------------------------------------------- */

int online_step(void)
{
   uint64_t now;

   pump();
   if (!O.conn)
      return O.stage;
   drain();
   if (O.stage == ON_IDLE || O.stage == ON_FAILED || O.stage == ON_CONNECTED)
      return O.stage;
   now = now_ms();

   /* the joiner's own clock */
   if (!O.is_host && O.t_join && O.stage >= ON_PUNCH && O.stage <= ON_RELAY_WAIT &&
       now - O.t_join > T_JOINER_MS)
   {
      fail(ONLINE_FAIL_UNREACHABLE, 0);
      return O.stage;
   }
   /* a host's code lives 10 minutes (the service's clock rules; this is the same one) */
   if (O.is_host && O.stage == ON_WAITING && O.have_room &&
       now - O.t_code >= (uint64_t)O.ttl_s * 1000)
   {
      O.room_gone = 1;
      fail(ONLINE_FAIL_EXPIRED, 0);
      return O.stage;
   }

   switch (O.stage)
   {
      case ON_RESOLVE:      stage_resolve(); break;
      case ON_STUN:         stage_stun(); break;
      case ON_CREATE:       stage_create(); break;
      case ON_JOIN:         stage_join(); break;
      case ON_WAITING:
      {
         lan_nat_status ns;
         poll_maybe();
         if (O.stage != ON_WAITING)
            break;
         /* the joiner's PUNCH reached us before our poll showed it */
         lan_transport_nat_status(O.t, &ns);
         if (ns.punch == LAN_PUNCH_RUNNING || ns.punch == LAN_PUNCH_DONE)
         {
            /* ask who it is, now (the service allows one poll a second) */
            O.t_punch0 = now;
            if (O.t_req[ONLINE_OP_POLL] + 1000 < now)
               O.next_poll = now;
            else
               O.next_poll = O.t_req[ONLINE_OP_POLL] + 1000;
            set_stage(ON_PUNCH);
         }
         break;
      }
      case ON_PUNCH:        stage_punch(); break;
      case ON_RELAY_CREDS:
      case ON_RELAY_RESOLVE:
      case ON_RELAY_ALLOC:
      case ON_RELAY_POST:
      case ON_RELAY_WAIT:   stage_relay(); break;
      case ON_MEASURE:      stage_measure(); break;
      default: break;
   }
   return O.stage;
}

/* ---- the ends ------------------------------------------------------------------------- */

int online_handoff_get(online_handoff *out)
{
   int i;
   if (!out)
      return -1;
   memset(out, 0, sizeof *out);
   if (!O.conn || !O.t || O.stage != ON_CONNECTED)
      return -1;
   out->relayed = O.relayed;
   for (i = 0; i < 8; i++)
      snprintf(out->key + 2 * i, sizeof out->key - 2 * i, "%02x", O.key[i]);
   out->peer_ip_be = O.peer_ip_be;
   out->peer_port = O.peer_port;
   copy_str(out->peer_lan, sizeof out->peer_lan, O.peer_lan);
   out->local_port = lan_transport_port(O.t);
   copy_str(out->peer_nick, sizeof out->peer_nick, O.peer_nick);
   if (O.relayed && lan_transport_relay_handoff(O.t, &out->relay) != 0)
      return -1;
   return 0;
}

void online_linked(void)
{
   if (!O.conn || !O.is_host || !O.have_room || O.close_linked_asked)
      return;
   O.close_linked_asked = 1;
   close_queue("linked");
   O.room_gone = 1;              /* the room dies with the link: no cancel after this */
   close_service();
}

void online_stop(void)
{
   if (!O.conn)
      return;
   {
      int need_close = O.have_room && !O.room_gone && !O.cl.pending && O.stage != ON_CONNECTED &&
                       O.stage != ON_FAILED;
      /* online_http_drop() CANCELS what is in flight (the client releases a
       * running request), it does not let it finish: a close that fail() or
       * online_linked() sent a moment ago and that has no answer yet would
       * be lost with the rest, and the room would stay up until it expires.
       * So a close that is still out is queued again after the drop (the
       * service answers a second close of the same room with 200 or 404). */
      int close_out = online_http_busy(ONLINE_OP_CLOSE) && O.cl.code[0];
      /* a create / join that was sent and has no definite answer may have reached the service: its room (or our
       * place in it) would stay until it expires.  Unless an earlier recovery is still running. */
      int need_rcv = O.rid_unknown && !O.have_room && !O.room_gone && !O.cl.rcv && O.rid[0];
      relay_down();
      online_http_drop();
      O.rq_n = 0;
      if (close_out)
      {
         O.cl.pending = 1;
         O.cl.sent = 0;
      }
      if (need_close)
         close_queue(O.is_host ? "cancel" : "failed");
      if (need_rcv)
      {
         O.cl.rcv = O.is_host ? 1 : 2;
         O.cl.rcv_out = 0;
         O.cl.rcv_tries = 0;
         O.cl.rcv_at = 0;
         copy_str(O.cl.rid, sizeof O.cl.rid, O.rid);
         copy_str(O.cl.jcode, sizeof O.cl.jcode, O.is_host ? "" : O.code);
         O.cl.desc = O.desc;
      }
      /* a close queued by online_linked or fail() before this is kept as it is */
      close_service();
   }
   reset_conn();
}
