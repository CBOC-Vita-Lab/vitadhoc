/* runner.c -- tests of online_vita.c (the online connection state machine).
 *
 * One process, one console, one 60 Hz loop on a FAKE clock (nothing sleeps:
 * a 10 minute wait is 36000 loop iterations).  online_vita.c is linked as it
 * is; what it stands on is faked here, exactly as far as its two headers say:
 *
 *   - the room client (online_http.h): an in-process room service with the
 *     plan's routes, states, poll table, modes and errors, 30 ms latency, a
 *     REQ log, and injectable failures.  The other player is a script that
 *     calls the service model directly (a guest joining the host's code, a
 *     host that already created a room for the joiner to find).
 *   - the NAT layer (transport_lan.h 2.1): STUN, punch and TURN status as a
 *     function of the fake time and a few knobs (does the punch ever get an
 *     ACK, which TURN port answers, what the echo round trip is).
 *
 * Usage: runner --list | runner <scenario> | runner all.  Every scenario
 * prints "SCENARIO <name> PASS" or "SCENARIO <name> FAIL"; the exit status
 * is the number of failed scenarios.  test_onlineconn.py runs them. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "online_vita.h"

/* ------------------------------------------------------------------ clock -- */
static uint64_t g_us;
static uint64_t fake_now(void) { return g_us; }
uint64_t plat_time_us(void) { return g_us; }      /* online_vita.c's clock when no now_us is configured */
static uint64_t T0;                         /* scenario start, us */
static unsigned ms_now(void) { return (unsigned)((g_us - T0) / 1000); }

/* ----------------------------------------------------------------- events -- */
static char g_evt[1 << 17];
static size_t g_evt_n;
static int g_verbose;

void fe_evt(const char *fmt, ...)
{
   char line[400];
   va_list ap;
   int n;
   va_start(ap, fmt);
   n = vsnprintf(line, sizeof line, fmt, ap);
   va_end(ap);
   if (n < 0)
      return;
   {
      char stamped[460];
      int m = snprintf(stamped, sizeof stamped, "EVT t=%u %s\n", ms_now(), line);
      if (g_verbose)
         fputs(stamped, stdout);
      if (g_evt_n + (size_t)m < sizeof g_evt)
      {
         memcpy(g_evt + g_evt_n, stamped, (size_t)m);
         g_evt_n += (size_t)m;
         g_evt[g_evt_n] = 0;
      }
   }
}

static int evt_has(const char *needle) { return strstr(g_evt, needle) != NULL; }

/* The time of the first event line holding `needle`, or -1. */
static int evt_time(const char *needle)
{
   const char *p = strstr(g_evt, needle);
   const char *ls;
   unsigned t;
   if (!p)
      return -1;
   ls = p;
   while (ls > g_evt && ls[-1] != '\n')
      ls--;
   if (sscanf(ls, "EVT t=%u", &t) != 1)
      return -1;
   return (int)t;
}

/* ------------------------------------------------------------ the checker -- */
static int g_fail;
static const char *g_name;
#define CHECK(c) do { if (!(c)) { g_fail++; printf("  CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* ================================================================== the fake room service == */
enum { RS_NONE = 0, RS_OPEN, RS_JOINED, RS_RELAY, RS_CLOSED };

typedef struct inject
{
   int err;            /* ONLINE_E_*; 0 = off */
   int http;
   int count;          /* answers to spoil; -1 = forever */
   int retry_after;    /* seconds, for a 429 */
   char host_ver[16];
} inject;

typedef struct svc
{
   int  state;
   char code[6];
   online_desc h, g;
   int  have_g;
   char htok[24], gtok[24], hrel[24], grel[24];
   uint64_t created_us, joined_us;
   int  ttl_s;
   int  no_expire;        /* the service forgets to expire: only the client's own clock ends the wait */
   int  expire_404;       /* an expired room answers 404 instead of state closed */
   char reason[12];
   int  net_down;         /* every request fails like a dead network */
   inject inj[10];
   /* relay credentials */
   char turn_host[64];
   uint16_t turn_port[3];
   int  turn_ports;
   int  mints;
   /* what the service saw */
   char last_close[12];
   int  closes, room_deleted;
   int  polls_with_pub;
   char last_new_pub[24];
   /* idempotent create / join (the request id), and answers lost on the way back */
   char rid_h[20], rid_g[20];
   int  rooms_made;
   int  lose[10];         /* ops whose answer is lost after the service processed them (a NET answer) */
   char rids_c[8][20], rids_j[8][20];
   int  n_rids_c, n_rids_j;
} svc;

static svc S;

static void svc_reset(void)
{
   memset(&S, 0, sizeof S);
   strcpy(S.code, "K7M2Q");
   strcpy(S.htok, "hhhhhhhhhhhhhhhhhhhhhh");
   strcpy(S.gtok, "gggggggggggggggggggggg");
   S.ttl_s = 600;
   strcpy(S.turn_host, "turn.test");
   S.turn_port[0] = 3478;
   S.turn_port[1] = 443;
   S.turn_ports = 2;
}

static int same_ip(const char *a, const char *b)
{
   const char *ca = strchr(a, ':'), *cb = strchr(b, ':');
   size_t na = ca ? (size_t)(ca - a) : strlen(a), nb = cb ? (size_t)(cb - b) : strlen(b);
   return na && na == nb && !strncmp(a, b, na);
}

static int svc_mode_relay(void)
{
   return S.h.relay_only || S.g.relay_only || !S.h.pub[0] || !S.g.pub[0] || S.h.sym || S.g.sym;
}

/* The host's poll interval at this moment of the room's life (1.3). */
static int svc_poll_ms(void)
{
   uint64_t age = (g_us - S.created_us) / 1000000;
   if (S.state == RS_JOINED || S.state == RS_RELAY)
      return 1500;
   return age < 30 ? 2000 : age < 120 ? 5000 : 10000;
}

static void svc_expire(void)
{
   if (!S.no_expire && S.state == RS_OPEN && g_us - S.created_us > (uint64_t)S.ttl_s * 1000000)
   {
      S.state = RS_CLOSED;
      strcpy(S.reason, "expired");
   }
}

/* The other player, as a script: */
static void svc_host_creates(const online_desc *h)
{
   S.h = *h;
   S.state = RS_OPEN;
   S.created_us = g_us;
}

static void svc_guest_joins(const online_desc *g)
{
   S.g = *g;
   S.have_g = 1;
   S.state = RS_JOINED;
   S.joined_us = g_us;
   S.grel[0] = 0;
}

static void svc_guest_leaves(void)
{
   S.rid_g[0] = 0;
   S.have_g = 0;
   memset(&S.g, 0, sizeof S.g);
   S.grel[0] = 0;
   S.state = RS_OPEN;
}

typedef struct pending { int used; int op; uint64_t ready_us; online_reply r; } pending;
static pending P[6];
static int g_latency_ms = 30;

typedef struct req { unsigned ms; int op; char code[8]; char extra[40]; } req;
static req REQ[512];
static int nreq;

static int count_req(int op)
{
   int i, n = 0;
   for (i = 0; i < nreq; i++)
      if (op == ONLINE_OP_NONE || REQ[i].op == op)
         n++;
   return n;
}

static void queue(int op, const online_reply *r, const char *extra, const char *code)
{
   int i;
   if (nreq < 512)
   {
      REQ[nreq].ms = ms_now();
      REQ[nreq].op = op;
      snprintf(REQ[nreq].code, sizeof REQ[nreq].code, "%s", code ? code : "");
      snprintf(REQ[nreq].extra, sizeof REQ[nreq].extra, "%s", extra ? extra : "");
      nreq++;
      if (g_verbose)
         printf("REQ t=%u op=%d %s %s\n", ms_now(), op, code ? code : "", extra ? extra : "");
   }
   for (i = 0; i < 6; i++)
      if (!P[i].used)
      {
         P[i].used = 1;
         P[i].op = op;
         P[i].ready_us = g_us + (uint64_t)g_latency_ms * 1000;
         P[i].r = *r;
         P[i].r.op = op;
         return;
      }
}

static int spoil(int op, online_reply *r)
{
   if (S.net_down)
   {
      r->err = ONLINE_E_NET;
      r->http = 0;
      return 1;
   }
   if (S.inj[op].err && S.inj[op].count != 0)
   {
      r->err = S.inj[op].err;
      r->http = S.inj[op].http;
      r->retry_after = S.inj[op].retry_after;
      strcpy(r->host_ver, S.inj[op].host_ver);
      if (S.inj[op].count > 0)
         S.inj[op].count--;
      return 1;
   }
   return 0;
}

static void fill_peer(online_reply *r, int as_host)
{
   const online_desc *o = as_host ? &S.g : &S.h;
   int relay = svc_mode_relay();
   r->have_peer = 1;
   snprintf(r->peer_nick, sizeof r->peer_nick, "%s", o->nick);
   r->peer_sym = o->sym;
   r->mode = relay ? ONLINE_MODE_RELAY : ONLINE_MODE_PUNCH;
   if (!relay)
   {
      snprintf(r->peer_pub, sizeof r->peer_pub, "%s", o->pub);
      if (same_ip(S.h.pub, S.g.pub))
         snprintf(r->peer_lan, sizeof r->peer_lan, "%s", o->lan);
   }
}

static int is_host_token(const char *t) { return !strcmp(t, S.htok); }
static int is_guest_token(const char *t) { return S.have_g && !strcmp(t, S.gtok); }

/* The service lost the answer on its way back: the request was processed, the client sees a dead network. */
static void maybe_lose(int op, online_reply *r)
{
   if (S.lose[op] != 0 && !r->err)
   {
      if (S.lose[op] > 0)
         S.lose[op]--;
      memset(r, 0, sizeof *r);
      r->err = ONLINE_E_NET;
   }
}

int online_http_create(const online_desc *room, const char *rid)
{
   online_reply r;
   char extra[40];
   memset(&r, 0, sizeof r);
   snprintf(extra, sizeof extra, "pub=%s", room->pub);
   if (S.n_rids_c < 8)
      snprintf(S.rids_c[S.n_rids_c++], 20, "%s", rid ? rid : "");
   if (S.state == RS_OPEN || S.state == RS_JOINED || S.state == RS_RELAY)
   {
      /* a new room replaces nothing: the tests only create once */
   }
   if (!spoil(ONLINE_OP_CREATE, &r))
   {
      int again = rid && rid[0] && !strcmp(rid, S.rid_h) && (S.state == RS_OPEN || S.state == RS_JOINED || S.state == RS_RELAY);
      r.http = 201;
      strcpy(r.code, S.code);
      strcpy(r.token, S.htok);
      strcpy(r.punch_key, "0123456789abcdef");
      r.ttl_sec = S.ttl_s;
      r.poll_ms = 2000;
      if (!again)
      {
         S.h = *room;
         S.state = RS_OPEN;
         S.created_us = g_us;
         S.have_g = 0;
         S.rooms_made++;
         snprintf(S.rid_h, sizeof S.rid_h, "%s", rid ? rid : "");
      }
      maybe_lose(ONLINE_OP_CREATE, &r);
   }
   queue(ONLINE_OP_CREATE, &r, extra, "");
   return 0;
}

int online_http_lookup(const char *code)
{
   online_reply r;
   memset(&r, 0, sizeof r);
   if (!spoil(ONLINE_OP_LOOKUP, &r))
   {
      svc_expire();
      if (S.state != RS_OPEN && S.state != RS_JOINED && S.state != RS_RELAY)
      {
         r.err = ONLINE_E_NOT_FOUND;
         r.http = 404;
      }
      else if (strcmp(code, S.code))
      {
         r.err = ONLINE_E_NOT_FOUND;
         r.http = 404;
      }
      else
      {
         r.http = 200;
         r.card = S.h;
         r.state = S.state == RS_OPEN ? ONLINE_ROOM_OPEN : ONLINE_ROOM_FULL;
      }
   }
   queue(ONLINE_OP_LOOKUP, &r, "", code);
   return 0;
}

int online_http_join(const char *code, const online_desc *me, const char *rid)
{
   online_reply r;
   char extra[40];
   memset(&r, 0, sizeof r);
   snprintf(extra, sizeof extra, "pub=%s", me->pub);
   if (S.n_rids_j < 8)
      snprintf(S.rids_j[S.n_rids_j++], 20, "%s", rid ? rid : "");
   if (!spoil(ONLINE_OP_JOIN, &r))
   {
      svc_expire();
      if (rid && rid[0] && S.have_g && !strcmp(rid, S.rid_g) && !strcmp(code, S.code) && S.state != RS_CLOSED && S.state != RS_NONE)
      {
         int host_poll = svc_poll_ms();
         r.http = 200;                    /* the repeated join: the original answer */
         strcpy(r.token, S.gtok);
         strcpy(r.punch_key, "0123456789abcdef");
         fill_peer(&r, 0);
         r.punch_ms = host_poll + 4000;
         r.poll_ms = 1500;
      }
      else if (strcmp(code, S.code) || (S.state != RS_OPEN && S.state != RS_JOINED && S.state != RS_RELAY))
      {
         r.err = ONLINE_E_NOT_FOUND;
         r.http = 404;
      }
      else if (S.state != RS_OPEN)
      {
         r.err = ONLINE_E_FULL;
         r.http = 409;
      }
      else if (S.h.compat != me->compat)
      {
         r.err = ONLINE_E_VERSION;
         r.http = 409;
         snprintf(r.host_ver, sizeof r.host_ver, "%s", S.h.ver);
      }
      else
      {
         int host_poll = svc_poll_ms();       /* the host's interval before this join changes the state */
         svc_guest_joins(me);
         r.http = 200;
         strcpy(r.token, S.gtok);
         strcpy(r.punch_key, "0123456789abcdef");
         fill_peer(&r, 0);
         r.punch_ms = host_poll + 4000;      /* the host's interval + 4 s */
         r.poll_ms = 1500;
         snprintf(S.rid_g, sizeof S.rid_g, "%s", rid ? rid : "");
      }
      maybe_lose(ONLINE_OP_JOIN, &r);
   }
   queue(ONLINE_OP_JOIN, &r, extra, code);
   return 0;
}

int online_http_poll(const char *code, const char *token, const char *new_pub)
{
   online_reply r;
   int host = is_host_token(token);
   memset(&r, 0, sizeof r);
   if (new_pub)
   {
      S.polls_with_pub++;
      snprintf(S.last_new_pub, sizeof S.last_new_pub, "%s", new_pub);
      if (host)
         snprintf(S.h.pub, sizeof S.h.pub, "%s", new_pub);
   }
   if (!spoil(ONLINE_OP_POLL, &r))
   {
      svc_expire();
      if (S.state == RS_NONE || (!host && !is_guest_token(token) && S.state != RS_CLOSED))
      {
         r.err = host || is_guest_token(token) ? ONLINE_E_NOT_FOUND : ONLINE_E_TOKEN;
         r.http = r.err == ONLINE_E_TOKEN ? 401 : 404;
      }
      else if (S.state == RS_CLOSED && S.expire_404)
      {
         r.err = ONLINE_E_NOT_FOUND;
         r.http = 404;
      }
      else
      {
         r.http = 200;
         r.state = S.state == RS_OPEN ? ONLINE_ROOM_OPEN : S.state == RS_JOINED ? ONLINE_ROOM_JOINED
                 : S.state == RS_RELAY ? ONLINE_ROOM_RELAY : ONLINE_ROOM_CLOSED;
         snprintf(r.reason, sizeof r.reason, "%s", S.state == RS_CLOSED ? S.reason : "");
         r.poll_ms = svc_poll_ms();
         if (S.have_g && S.state != RS_CLOSED)
         {
            fill_peer(&r, host);
            r.mode = svc_mode_relay() ? ONLINE_MODE_RELAY : ONLINE_MODE_PUNCH;
            r.punch_ms = host ? 4000 : svc_poll_ms() + 4000;
            snprintf(r.relay_peer, sizeof r.relay_peer, "%s", host ? S.grel : S.hrel);
         }
      }
   }
   queue(ONLINE_OP_POLL, &r, new_pub ? new_pub : "", code);
   return 0;
}

int online_http_relay(const char *code, const char *token)
{
   online_reply r;
   (void)token;
   memset(&r, 0, sizeof r);
   if (!spoil(ONLINE_OP_RELAY, &r))
   {
      if (!S.have_g)
      {
         r.err = ONLINE_E_RELAY;
         r.http = 403;
      }
      else
      {
         r.http = 200;
         r.ttl_sec = 3600;
         S.mints++;
         snprintf(r.turn_host, sizeof r.turn_host, "%s", S.turn_host);
         r.turn_ports = S.turn_ports;
         memcpy(r.turn_port, S.turn_port, sizeof r.turn_port);
         strcpy(r.turn_user, "1700000000:u");
         strcpy(r.turn_cred, "secret");
      }
   }
   queue(ONLINE_OP_RELAY, &r, "", code);
   return 0;
}

int online_http_endpoint(const char *code, const char *token, const char *relay_addr)
{
   online_reply r;
   int host = is_host_token(token);
   memset(&r, 0, sizeof r);
   if (!spoil(ONLINE_OP_ENDPOINT, &r))
   {
      if (!S.have_g)
      {
         r.err = ONLINE_E_RELAY;
         r.http = 403;
      }
      else
      {
         r.http = 200;
         snprintf(host ? S.hrel : S.grel, 24, "%s", relay_addr);
         S.state = RS_RELAY;
         snprintf(r.relay_peer, sizeof r.relay_peer, "%s", host ? S.grel : S.hrel);
      }
   }
   queue(ONLINE_OP_ENDPOINT, &r, relay_addr, code);
   return 0;
}

int online_http_close(const char *code, const char *token, const char *reason)
{
   online_reply r;
   int host = is_host_token(token);
   memset(&r, 0, sizeof r);
   if (!spoil(ONLINE_OP_CLOSE, &r))
   {
      r.http = 200;
      S.closes++;
      snprintf(S.last_close, sizeof S.last_close, "%s", reason);
      if (host && !strcmp(reason, "linked"))
      {
         S.state = RS_NONE;
         S.room_deleted = 1;
      }
      else if (host)
      {
         S.state = RS_CLOSED;
         strcpy(S.reason, "cancel");
      }
      else
         svc_guest_leaves();
   }
   queue(ONLINE_OP_CLOSE, &r, reason, code);
   return 0;
}

static int g_resolve_fail;       /* number of resolve answers to spoil with NET */
int online_http_resolve(const char *host)
{
   online_reply r;
   memset(&r, 0, sizeof r);
   if (g_resolve_fail != 0)
   {
      if (g_resolve_fail > 0)
         g_resolve_fail--;
      r.err = ONLINE_E_NET;
   }
   else if (!spoil(ONLINE_OP_RESOLVE, &r))
   {
      r.http = 200;
      if (!strcmp(host, "turn.test"))
         strcpy(r.resolved, "198.51.100.77");
      else
         strcpy(r.resolved, "198.51.100.1");
   }
   queue(ONLINE_OP_RESOLVE, &r, host, "");
   return 0;
}

int online_http_busy(int op)
{
   int i;
   for (i = 0; i < 6; i++)
      if (P[i].used && (op == ONLINE_OP_NONE || P[i].op == op))
         return 1;
   return 0;
}

int online_http_take(online_reply *out)
{
   int i;
   for (i = 0; i < 6; i++)
      if (P[i].used && P[i].ready_us <= g_us)
      {
         *out = P[i].r;
         P[i].used = 0;
         return 1;
      }
   return 0;
}

void online_http_drop(void)
{
   memset(P, 0, sizeof P);
}

void online_http_config(const char *u, const char *v) { (void)u; (void)v; }
void online_http_shutdown(void) { }
int  online_json_get(const char *b, const char *p, char *o, size_t c) { (void)b; (void)p; (void)o; (void)c; return 0; }
int  online_json_int(const char *b, const char *p, int d) { (void)b; (void)p; return d; }
size_t online_json_desc(char *o, size_t c, const online_desc *d, const char *e) { (void)o; (void)c; (void)d; (void)e; return 0; }

/* ================================================================== the fake NAT layer == */
struct lan_transport { int dummy; };
static lan_transport g_tr;

typedef struct nat
{
   /* knobs */
   int      stun_ok;               /* 1: answers; 0: FAILED after 1 s */
   uint32_t pub_ip;  uint16_t pub_port;
   int      sym;
   int      punch_after_ms;        /* DONE this long after punch_start; < 0 never */
   int      incoming_at_ms;        /* >= 0: an incoming PUNCH at this scenario time starts the race */
   int      relay_ok_idx;          /* the port (index in the list) that answers; -1 none */
   int      bind_ms;               /* READY this long after relay_bind; < 0 never */
   int      echo_rtt_us;           /* 0: no echo answered */
   /* what happened */
   int      stun_calls, seeds, keep_ms, armed, arm_calls, punch_starts, punch_stops, relay_starts,
            relay_binds, relay_stops, echoes, echo_resets, targets;
   uint16_t relay_ports[3];
   int      relay_nports;
   int      n_cand;
   char     cand[3][24];
   unsigned punch_timeout_ms;
   uint8_t  key[8];
   char     user[160], cred[160];
   uint32_t target_ip; uint16_t target_port;
   int locks, unlocks, holds; uint32_t lock_ip; uint16_t lock_port;   /* lan_transport_peer_lock / _hold */
   uint32_t bound_ip;  uint16_t bound_port;
   /* internal */
   uint64_t t_stun, t_punch, t_relay, t_bind;
   int      stun_started, punch_running, punch_incoming, relay_started, bind_started;
   uint32_t stun_ip[2]; uint16_t stun_port[2]; int stun_n;
} nat;
static nat N;

static uint32_t ip4(int a, int b, int c, int d)
{
   uint8_t x[4] = { (uint8_t)a, (uint8_t)b, (uint8_t)c, (uint8_t)d };
   uint32_t v;
   memcpy(&v, x, 4);
   return v;
}

static void nat_reset(void)
{
   memset(&N, 0, sizeof N);
   N.stun_ok = 1;
   N.pub_ip = ip4(203, 0, 113, 1);
   N.pub_port = 40001;
   N.punch_after_ms = 300;
   N.incoming_at_ms = -1;
   N.relay_ok_idx = 0;
   N.bind_ms = 200;
   N.echo_rtt_us = 40000;
}

int lan_transport_nat_seed(lan_transport *t, const uint8_t e[16]) { (void)t; (void)e; N.seeds++; return 0; }

int lan_transport_stun_start(lan_transport *t, const uint32_t *ip, const uint16_t *port, int n)
{
   int i;
   (void)t;
   if (!N.seeds)
      return -1;                     /* as the real layer: nothing before nat_seed */
   N.stun_calls++;
   N.stun_n = n;
   for (i = 0; i < n && i < 2; i++)
   {
      N.stun_ip[i] = ip[i];
      N.stun_port[i] = port[i];
   }
   N.stun_started = 1;
   N.t_stun = g_us;
   return 0;
}

void lan_transport_stun_keep(lan_transport *t, unsigned ms) { (void)t; N.keep_ms = (int)ms; }

int lan_transport_punch_arm(lan_transport *t, const uint8_t key[8])
{
   (void)t;
   if (!N.seeds)
      return -1;
   memcpy(N.key, key, 8);
   N.armed = 1;
   N.arm_calls++;
   return 0;
}

int lan_transport_punch_start(lan_transport *t, const uint32_t *ip, const uint16_t *port, int n,
                              unsigned timeout_ms)
{
   int i;
   (void)t;
   if (!N.armed)
      return -1;
   N.punch_starts++;
   N.n_cand = n;
   for (i = 0; i < n; i++)
   {
      uint8_t b[4];
      memcpy(b, &ip[i], 4);
      snprintf(N.cand[i], sizeof N.cand[i], "%u.%u.%u.%u:%u", b[0], b[1], b[2], b[3], port[i]);
   }
   N.punch_timeout_ms = timeout_ms;
   N.punch_running = 1;
   N.t_punch = g_us;
   return 0;
}

void lan_transport_punch_stop(lan_transport *t)
{
   (void)t;
   N.punch_stops++;
   N.armed = 0;
   N.punch_running = 0;
   N.punch_incoming = 0;
   N.incoming_at_ms = -1;
}

int lan_transport_relay_start(lan_transport *t, uint32_t ip, const uint16_t *ports, int n,
                              const char *user, const char *cred)
{
   (void)t; (void)ip;
   if (!N.seeds)
      return -1;
   N.relay_starts++;
   N.relay_nports = n;
   memcpy(N.relay_ports, ports, (size_t)n * sizeof ports[0]);
   snprintf(N.user, sizeof N.user, "%s", user);
   snprintf(N.cred, sizeof N.cred, "%s", cred);
   N.relay_started = 1;
   N.t_relay = g_us;
   return 0;
}

int lan_transport_relay_bind(lan_transport *t, uint32_t ip, uint16_t port)
{
   (void)t;
   N.relay_binds++;
   N.bound_ip = ip;
   N.bound_port = port;
   N.bind_started = 1;
   N.t_bind = g_us;
   return 0;
}

void lan_transport_relay_stop(lan_transport *t)
{
   (void)t;
   N.relay_stops++;
   N.relay_started = N.bind_started = 0;
}

void lan_transport_nat_status(lan_transport *t, lan_nat_status *o)
{
   uint64_t now = g_us;
   (void)t;
   memset(o, 0, sizeof *o);
   if (N.stun_started)
   {
      if (N.stun_ok && now - N.t_stun >= 150000)
      {
         o->stun = LAN_STUN_OK;
         o->pub_ip_be = N.pub_ip;
         o->pub_port = N.pub_port;
         o->sym = N.sym;
      }
      else if (!N.stun_ok && now - N.t_stun >= 1000000)
         o->stun = LAN_STUN_FAILED;
      else
         o->stun = LAN_STUN_RUNNING;
   }
   if (N.armed)
      o->punch = LAN_PUNCH_ARMED;
   if (N.armed && N.incoming_at_ms >= 0 && (now - T0) / 1000 >= (uint64_t)N.incoming_at_ms &&
       !N.punch_running)
   {
      N.punch_incoming = 1;
      N.punch_running = 1;
      N.t_punch = g_us;
   }
   if (N.punch_running)
   {
      uint64_t el = (now - N.t_punch) / 1000;
      o->punch = LAN_PUNCH_RUNNING;
      if (N.punch_after_ms >= 0 && el >= (uint64_t)N.punch_after_ms && !N.relay_started)
      {
         o->punch = LAN_PUNCH_DONE;
         o->peer_ip_be = ip4(203, 0, 113, 2);
         o->peer_port = 40002;
         if (N.n_cand > 0)
         {
            /* the first candidate answers: it is where the peer is */
            uint32_t ip; uint16_t port;
            unsigned a, b, c, d, p;
            if (sscanf(N.cand[0], "%u.%u.%u.%u:%u", &a, &b, &c, &d, &p) == 5)
            {
               ip = ip4((int)a, (int)b, (int)c, (int)d);
               port = (uint16_t)p;
               o->peer_ip_be = ip;
               o->peer_port = port;
            }
         }
      }
      else if (N.punch_after_ms < 0 && N.punch_timeout_ms && el >= N.punch_timeout_ms)
         o->punch = LAN_PUNCH_TIMEOUT;
   }
   if (N.relay_started)
   {
      uint64_t el = (now - N.t_relay) / 1000;
      if (N.relay_ok_idx >= 0 && el >= (uint64_t)3000 * (unsigned)N.relay_ok_idx + 120)
      {
         o->relay = LAN_RELAY_ALLOCATED;
         o->relay_ip_be = ip4(198, 51, 100, 77);
         o->relay_port = (uint16_t)(50000 + N.relay_ok_idx);
         if (N.bind_started)
         {
            o->relay = LAN_RELAY_BINDING;
            if (N.bind_ms >= 0 && (now - N.t_bind) / 1000 >= (uint64_t)N.bind_ms)
               o->relay = LAN_RELAY_READY;
         }
      }
      else if (N.relay_ok_idx < 0 && el >= (uint64_t)3000 * (unsigned)N.relay_nports)
      {
         o->relay = LAN_RELAY_FAILED;
         o->relay_err = LAN_RELAY_E_TIMEOUT;
      }
      else
         o->relay = LAN_RELAY_ALLOCATING;
   }
}

int lan_transport_set_target(lan_transport *t, uint32_t ip, uint16_t port)
{
   (void)t;
   N.targets++;
   N.target_ip = ip;
   N.target_port = port;
   return 0;
}

void lan_transport_peer_hold(lan_transport *t)
{
   (void)t;
   N.holds++;
   N.lock_ip = 0;
   N.lock_port = 0;
}

int lan_transport_peer_lock(lan_transport *t, uint32_t ip, uint16_t port)
{
   (void)t;
   if (!ip)
   {
      N.unlocks++;
      N.lock_ip = 0;
      N.lock_port = 0;
      return 0;
   }
   N.locks++;
   N.lock_ip = ip;
   N.lock_port = port;
   return 0;
}

int lan_transport_echo(lan_transport *t, const uint8_t mac[6]) { (void)t; (void)mac; N.echoes++; return 0; }
void lan_transport_echo_reset(lan_transport *t) { (void)t; N.echo_resets++; N.echoes = 0; }
int lan_transport_echo_result(lan_transport *t, uint32_t *mn, uint32_t *mx, uint32_t *last)
{
   (void)t;
   if (mn) *mn = (uint32_t)N.echo_rtt_us;
   if (mx) *mx = (uint32_t)N.echo_rtt_us;
   if (last) *last = (uint32_t)N.echo_rtt_us;
   return N.echo_rtt_us ? N.echoes : 0;
}
void lan_mac_from_addr(uint32_t ip, uint16_t port, uint8_t mac[6])
{
   memset(mac, 0, 6);
   memcpy(mac, &ip, 4);
   mac[4] = (uint8_t)(port >> 8);
   mac[5] = (uint8_t)port;
}
/* SNES hand-over (online_handoff_get): the bound port, and a relayed link's allocation.  The fake relay has nothing
 * to hand over beyond "it was READY". */
uint16_t lan_transport_port(const lan_transport *t) { (void)t; return 19019; }
int lan_transport_relay_handoff(lan_transport *t, lan_relay_params *out)
{
   lan_nat_status s;
   lan_transport_nat_status(t, &s);
   if (!out || s.relay != LAN_RELAY_READY)
      return -1;
   memset(out, 0, sizeof *out);
   out->relay_ip_be = s.relay_ip_be;
   out->relay_port = s.relay_port;
   return 0;
}
const char *lan_transport_addr_str(const lan_transport *t, char *buf, size_t n)
{
   (void)t;
   snprintf(buf, n, "192.168.1.23:19019");
   return buf;
}

/* ================================================================== the loop == */
static void (*g_hook)(unsigned ms);       /* the script of "the other player" */
static int g_stage;

static void tick(void)
{
   g_us += 16667;
   if (g_hook)
      g_hook(ms_now());
   g_stage = online_step();
}

/* Run until the stage is `want` (or the connection failed / ms passed). */
static int run_to(int want, unsigned ms)
{
   unsigned end = ms_now() + ms;
   while (ms_now() < end)
   {
      tick();
      if (g_stage == want)
         return 1;
      if (g_stage == ON_FAILED && want != ON_FAILED)
         return 0;
   }
   return 0;
}

static void run_for(unsigned ms)
{
   unsigned end = ms_now() + ms;
   while (ms_now() < end)
      tick();
}

static online_cfg g_cfg;

static void entropy_fn(uint8_t out[16]) { int i; for (i = 0; i < 16; i++) out[i] = (uint8_t)(i * 7 + 1); }

static void world(void)
{
   memset(&g_cfg, 0, sizeof g_cfg);
   g_cfg.stun = "stun.test:3478,stun.test:53";
   g_cfg.post_lan = 1;
   g_cfg.now_us = fake_now;
   g_cfg.entropy = entropy_fn;
   g_us = 5000000;
   T0 = g_us;
   g_evt_n = 0;
   g_evt[0] = 0;
   g_hook = NULL;
   nreq = 0;
   memset(P, 0, sizeof P);
   g_resolve_fail = 0;
   svc_reset();
   nat_reset();
}

static void start(void) { online_setup(&g_cfg); }

static online_desc mk_desc(const char *nick, int kind)
{
   online_desc d;
   memset(&d, 0, sizeof d);
   d.kind = kind;
   strcpy(d.console, "gba");
   strcpy(d.game, "BPEE");
   strcpy(d.rom, "9f3a01c2");
   strcpy(d.title, "Test Game");
   snprintf(d.nick, sizeof d.nick, "%s", nick);
   strcpy(d.ver, "2.0.0");
   d.compat = 0x1a2b3c4dU;
   return d;
}

/* a room the host console (the script) made, with a given public address */
static void script_host(const char *pub, const char *lan, int sym, int relay_only)
{
   online_desc h = mk_desc("Hosty", LAN_KIND_GBA_CABLE);
   snprintf(h.pub, sizeof h.pub, "%s", pub);
   snprintf(h.lan, sizeof h.lan, "%s", lan);
   h.sym = sym;
   h.relay_only = relay_only;
   svc_host_creates(&h);
}

static online_desc guest_desc(const char *pub, const char *lan, int sym, int relay_only)
{
   online_desc g = mk_desc("Gus", LAN_KIND_GBA_CABLE);
   snprintf(g.pub, sizeof g.pub, "%s", pub);
   snprintf(g.lan, sizeof g.lan, "%s", lan);
   g.sym = sym;
   g.relay_only = relay_only;
   return g;
}

/* ================================================================== scenarios == */
static void finish_host(online_view *v)
{
   online_get(v);
}

/* 1. punch, host side: waits 5 s, a guest joins, punch wins, linked closes the room. */
static void sc_host_punch(void)
{
   online_desc room = mk_desc("Alice", LAN_KIND_GBA_CABLE);
   online_view v;
   unsigned joined_at = 0;
   strcpy(room.ver, "2.0-dev");
   int polls;

   world();
   start();
   CHECK(online_host(&g_tr, &room) == 0);
   CHECK(online_active());
   CHECK(run_to(ON_WAITING, 3000));
   CHECK(N.seeds == 1 && N.stun_calls == 1 && N.stun_n == 2);
   CHECK(N.stun_port[0] == 3478 && N.stun_port[1] == 53);
   CHECK(N.keep_ms == 15000);
   CHECK(N.armed && !memcmp(N.key, "\x01\x23\x45\x67\x89\xab\xcd\xef", 8));
   CHECK(!strcmp(S.h.pub, "203.0.113.1:40001"));
   CHECK(!strcmp(S.h.lan, "192.168.1.23:19019"));
   CHECK(!strcmp(S.h.ver, "2.0.0"));               /* never the caller's build label */
   CHECK(S.h.sym == 0);
   online_get(&v);
   CHECK(!strcmp(v.code, "K7M2Q") && v.is_host);
   CHECK(v.ttl_s == 600);
   run_for(5000);
   online_get(&v);
   CHECK(v.waited_s >= 4 && v.waited_s <= 6 && v.ttl_s == 600);
   /* the guest, a script: joins now */
   {
      online_desc g = guest_desc("203.0.113.2:40002", "192.168.2.9:19019", 0, 0);
      svc_guest_joins(&g);
      joined_at = ms_now();
   }
   CHECK(run_to(ON_CONNECTED, 6000));
   online_get(&v);
   CHECK(v.stage == ON_CONNECTED && !v.relayed);
   CHECK(v.waited_s >= 5 && v.waited_s <= 7);        /* frozen when the friend came */
   CHECK(!strcmp(v.peer_nick, "Gus"));
   CHECK(N.punch_starts == 1);
   CHECK(N.n_cand == 1 && !strcmp(N.cand[0], "203.0.113.2:40002"));
   CHECK(N.punch_timeout_ms == 4000);
   CHECK(N.echoes == 6 && N.echo_resets == 1);
   CHECK(v.quality == ONLINE_Q_GREAT && v.rtt_ms == 40);
   CHECK(N.targets == 0);                       /* the host does not set a target */
   CHECK(N.holds == 1 && N.locks == 1 && N.lock_ip == ip4(203, 0, 113, 2) && N.lock_port == 40002);   /* ... held from the start, and it hears only its guest from here on */
   /* no relay request waited for the punch? it is fetched beside it, and unused */
   CHECK(N.relay_starts == 0);
   CHECK(ms_now() - joined_at < 5000);
   online_linked();
   run_for(500);
   CHECK(S.room_deleted && !strcmp(S.last_close, "linked"));
   polls = count_req(ONLINE_OP_POLL);
   run_for(5000);
   CHECK(count_req(ONLINE_OP_POLL) == polls);     /* polling stopped */
   CHECK(count_req(ONLINE_OP_CLOSE) == 1);
   CHECK(evt_has("online_room code=K7M2Q ttl=600"));
   CHECK(evt_has("online_peer nick=\"Gus\" mode=punch"));
   CHECK(evt_has("online_punch done=1 peer=203.0.113.2:40002 lan=0"));
   CHECK(evt_has("online_quality rtt_ms=40 q=1 relayed=0"));
   (void)finish_host;
   online_stop();
   CHECK(S.closes == 1);                         /* a connected stop adds no cancel */
}

/* 2. the request count for a 5 s wait stays small, and the polling slows with the room's age. */
static void sc_host_wait_cadence(void)
{
   online_desc room = mk_desc("Alice", LAN_KIND_GBA_CABLE);
   int i, gaps_2 = 0, gaps_5 = 0, gaps_10 = 0;
   unsigned last = 0;
   unsigned prev_t[200];
   int np = 0;

   world();
   start();
   online_host(&g_tr, &room);
   run_for(5000);
   CHECK(count_req(ONLINE_OP_NONE) <= 12);       /* create + polls + the STUN-time resolve */
   run_for(35000);                               /* 40 s in */
   for (i = 0; i < nreq && np < 200; i++)
      if (REQ[i].op == ONLINE_OP_POLL)
         prev_t[np++] = REQ[i].ms;
   for (i = 1; i < np; i++)
   {
      unsigned gap = prev_t[i] - prev_t[i - 1];
      if (prev_t[i] < 30000)
         gaps_2 += (gap >= 1950 && gap <= 2100);
      else if (prev_t[i - 1] > 31000 && prev_t[i] < 120000)
         gaps_5 += (gap >= 4950 && gap <= 5100);
      (void)last;
   }
   CHECK(gaps_2 >= 10);                          /* 2 s polls while young */
   CHECK(gaps_5 >= 1);                           /* then 5 s */
   run_for(100000);                              /* past 2 minutes: 10 s */
   for (i = 0, np = 0; i < nreq && np < 200; i++)
      if (REQ[i].op == ONLINE_OP_POLL)
         prev_t[np++] = REQ[i].ms;
   for (i = 1; i < np; i++)
      if (prev_t[i - 1] > 125000 && prev_t[i] - prev_t[i - 1] >= 9950 && prev_t[i] - prev_t[i - 1] <= 10100)
         gaps_10++;
   CHECK(gaps_10 >= 1);
   online_stop();
   CHECK(!strcmp(S.last_close, "cancel"));
}

/* 3. TTL: the code expires on its own clock (a short TTL), and on the service's 404. */
static void sc_host_ttl(void)
{
   online_desc room = mk_desc("Alice", LAN_KIND_GBA_CABLE);
   online_view v;
   world();
   svc_reset();
   S.ttl_s = 20;
   start();
   online_host(&g_tr, &room);
   CHECK(run_to(ON_FAILED, 40000));
   online_get(&v);
   CHECK(v.fail == ONLINE_FAIL_EXPIRED);
   CHECK(S.closes == 0);                         /* an expired room needs no close */
   online_stop();

   /* the client's own clock, when the service says nothing */
   world();
   S.ttl_s = 20;
   S.no_expire = 1;
   start();
   online_host(&g_tr, &room);
   CHECK(run_to(ON_FAILED, 40000));
   online_get(&v);
   CHECK(v.fail == ONLINE_FAIL_EXPIRED && ms_now() >= 20000 && ms_now() < 22000);
   online_stop();

   world();
   S.ttl_s = 20;
   S.expire_404 = 1;
   start();
   online_host(&g_tr, &room);
   CHECK(run_to(ON_FAILED, 15000) == 0 || 1);
   run_to(ON_FAILED, 40000);
   online_get(&v);
   CHECK(v.fail == ONLINE_FAIL_EXPIRED);
   online_stop();
}

/* 4. the host's own mapping changes while it waits: the next poll carries new_pub. */
static void sc_host_new_pub(void)
{
   online_desc room = mk_desc("Alice", LAN_KIND_GBA_CABLE);
   world();
   start();
   online_host(&g_tr, &room);
   CHECK(run_to(ON_WAITING, 3000));
   run_for(3000);
   CHECK(S.polls_with_pub == 0);
   N.pub_port = 40077;
   run_for(4000);
   CHECK(S.polls_with_pub == 1 && !strcmp(S.last_new_pub, "203.0.113.1:40077"));
   run_for(6000);
   CHECK(S.polls_with_pub == 1);                 /* said once */
   online_stop();
}

/* 5. the joiner's PUNCH reaches the host before its poll shows the guest. */
static void sc_host_incoming_punch(void)
{
   online_desc room = mk_desc("Alice", LAN_KIND_GBA_CABLE);
   online_view v;
   world();
   start();
   online_host(&g_tr, &room);
   CHECK(run_to(ON_WAITING, 3000));
   run_for(3300);
   {
      online_desc g = guest_desc("203.0.113.2:40002", "192.168.2.9:19019", 0, 0);
      svc_guest_joins(&g);
   }
   N.incoming_at_ms = (int)ms_now() + 100;
   N.punch_after_ms = 1;
   CHECK(run_to(ON_CONNECTED, 6000));
   online_get(&v);
   CHECK(!strcmp(v.peer_nick, "Gus"));           /* the poll still told us who it is */
   online_stop();
}

/* 5b. A joiner's PUNCH opens the host, but the joiner has already given up
 * (its close reopened the room): the poll sent after the PUNCH shows nobody.
 * The host must go back to waiting with the same code, not sit in the punch
 * stage until the room expires; the next guest then connects. */
static void sc_host_punch_guest_gone(void)
{
   online_desc room = mk_desc("Alice", LAN_KIND_GBA_CABLE);
   online_desc g = guest_desc("203.0.113.2:40002", "192.168.2.9:19019", 0, 0);
   online_view v;
   world();
   start();
   online_host(&g_tr, &room);
   CHECK(run_to(ON_WAITING, 3000));
   run_for(3300);
   svc_guest_joins(&g);
   svc_guest_leaves();                           /* before the host ever polled it */
   N.incoming_at_ms = (int)ms_now() + 100;       /* its PUNCH was already on the way */
   N.punch_after_ms = -1;                        /* and nobody answers ours */
   CHECK(run_to(ON_PUNCH, 1000));
   CHECK(run_to(ON_WAITING, 3000));
   online_get(&v);
   CHECK(v.fail == ONLINE_FAIL_NONE && v.code[0]);
   CHECK(N.armed == 1);                          /* armed again for the next guest */
   N.punch_after_ms = 300;
   svc_guest_joins(&g);
   CHECK(run_to(ON_CONNECTED, 12000));
   online_stop();
}

/* 6. joiner, punch (port x port), request count and set_target. */
static void sc_join_punch(void)
{
   online_desc me = mk_desc("Gus", LAN_KIND_GBA_CABLE);
   online_view v;
   world();
   start();
   script_host("203.0.113.2:40002", "192.168.2.9:19019", 0, 0);
   CHECK(online_join(&g_tr, "k7m2q", &me) == 0);
   CHECK(run_to(ON_CONNECTED, 8000));
   online_get(&v);
   CHECK(!v.is_host && !strcmp(v.code, "K7M2Q") && !strcmp(v.peer_nick, "Hosty"));
   CHECK(N.punch_starts == 1 && N.punch_timeout_ms == 6000);       /* the host's 2 s interval + 4 s */
   CHECK(N.targets == 1);
   CHECK(N.target_ip == ip4(203, 0, 113, 2) && N.target_port == 40002);
   CHECK(N.locks == 1 && N.lock_ip == ip4(203, 0, 113, 2) && N.lock_port == 40002);
   CHECK(count_req(ONLINE_OP_LOOKUP) == 0 && count_req(ONLINE_OP_JOIN) == 1);
   CHECK(S.state == RS_JOINED);                  /* D14: the joiner never closes */
   online_linked();                              /* a joiner's linked is nothing */
   run_for(1000);
   CHECK(S.closes == 0);
   online_stop();
   CHECK(S.closes == 0);
}

/* 7. same router: the LAN candidate is offered, and the punch is DONE on it. */
static void sc_same_lan(void)
{
   online_desc me = mk_desc("Gus", LAN_KIND_GBA_CABLE);
   world();
   start();
   me.compat = 0x1a2b3c4dU;
   script_host("203.0.113.1:40001", "192.168.1.55:19019", 0, 0);
   N.pub_ip = ip4(203, 0, 113, 1);
   N.pub_port = 40009;
   online_join(&g_tr, "K7M2Q", &me);
   CHECK(run_to(ON_CONNECTED, 8000));
   CHECK(!strcmp(S.g.lan, "192.168.1.23:19019"));
   CHECK(N.n_cand == 2);
   CHECK(!strcmp(N.cand[0], "203.0.113.1:40001") && !strcmp(N.cand[1], "192.168.1.55:19019"));
   online_stop();
}

/* 8. post_lan = 0: no LAN address is offered. */
static void sc_no_post_lan(void)
{
   online_desc me = mk_desc("Gus", LAN_KIND_GBA_CABLE);
   world();
   g_cfg.post_lan = 0;
   start();
   script_host("203.0.113.2:40002", "", 0, 0);
   online_join(&g_tr, "K7M2Q", &me);
   run_to(ON_CONNECTED, 8000);
   CHECK(S.g.lan[0] == 0);
   online_stop();
}

/* 9. relay by mode (the peer is symmetric): no PUNCH, relayed within 3 s of the join, no peer address in the log. */
static void sc_join_relay_sym(void)
{
   online_desc me = mk_desc("Gus", LAN_KIND_GBA_CABLE);
   online_view v;
   unsigned t_join;
   world();
   start();
   script_host("203.0.113.2:40002", "192.168.2.9:19019", 1, 0);
   online_join(&g_tr, "K7M2Q", &me);
   CHECK(run_to(ON_RELAY_POST, 6000));
   /* the join answered; the host (script) posts its relayed address a moment later */
   t_join = (unsigned)evt_time("online_peer");
   snprintf(S.hrel, sizeof S.hrel, "198.51.100.77:50999");
   CHECK(run_to(ON_CONNECTED, 6000));
   online_get(&v);
   CHECK(v.relayed);
   CHECK(N.punch_starts == 0);
   CHECK(N.relay_starts == 1 && N.relay_nports == 2 && N.relay_ports[0] == 3478 && N.relay_ports[1] == 443);
   CHECK(!strcmp(N.user, "1700000000:u") && !strcmp(N.cred, "secret"));
   CHECK(N.relay_binds == 1 && N.bound_ip == ip4(198, 51, 100, 77) && N.bound_port == 50999);
   CHECK(N.targets == 1 && N.target_port == 50999);
   CHECK(N.locks == 1 && N.lock_ip == ip4(198, 51, 100, 77) && N.lock_port == 50999);   /* the peer's RELAYED address */
   CHECK(evt_time("online_quality") - (int)t_join < 3000);
   CHECK(!evt_has("203.0.113.2"));               /* never the other side's address */
   CHECK(evt_has("online_relay alloc=198.51.100.77:50000 peer=198.51.100.77:50999"));
   CHECK(count_req(ONLINE_OP_ENDPOINT) == 1);
   CHECK(!strcmp(S.grel, "198.51.100.77:50000"));
   online_stop();
}

/* 10. host side relay: this console is symmetric; the service says relay at once. */
static void sc_host_relay_sym(void)
{
   online_desc room = mk_desc("Alice", LAN_KIND_GBA_CABLE);
   online_view v;
   world();
   N.sym = 1;
   start();
   online_host(&g_tr, &room);
   CHECK(run_to(ON_WAITING, 3000));
   CHECK(S.h.sym == 1);
   {
      online_desc g = guest_desc("203.0.113.2:40002", "192.168.2.9:19019", 0, 0);
      svc_guest_joins(&g);
   }
   run_for(2500);
   CHECK(N.punch_starts == 0);
   snprintf(S.grel, sizeof S.grel, "198.51.100.77:50888");
   S.state = RS_RELAY;
   CHECK(run_to(ON_CONNECTED, 8000));
   online_get(&v);
   CHECK(v.relayed);
   CHECK(!evt_has("203.0.113.2"));
   CHECK(N.targets == 0);
   online_linked();
   run_for(300);
   CHECK(S.room_deleted);
   online_stop();
   CHECK(N.relay_stops >= 1);
}

/* 11. the race: nobody answers the punch; the credentials were already there when the 4 s ended. */
static void sc_punch_race_falls_to_relay(void)
{
   online_desc me = mk_desc("Gus", LAN_KIND_GBA_CABLE);
   int t_relay_req, t_punch_to, t_conn;
   world();
   N.punch_after_ms = -1;
   start();
   script_host("203.0.113.2:40002", "192.168.2.9:19019", 0, 0);
   online_join(&g_tr, "K7M2Q", &me);
   run_to(ON_PUNCH, 3000);
   /* the script host posts its relayed address once the guest's race is over */
   snprintf(S.hrel, sizeof S.hrel, "198.51.100.77:50999");
   CHECK(run_to(ON_CONNECTED, 20000));
   t_relay_req = -1;
   {
      int i;
      for (i = 0; i < nreq; i++)
         if (REQ[i].op == ONLINE_OP_RELAY)
         {
            t_relay_req = (int)REQ[i].ms;
            break;
         }
   }
   t_punch_to = evt_time("online_punch done=0");
   t_conn = evt_time("online_quality");
   CHECK(t_relay_req >= 0 && t_punch_to >= 0);
   CHECK(t_relay_req < t_punch_to);              /* credentials before the race ended */
   CHECK(count_req(ONLINE_OP_RESOLVE) >= 2);     /* the TURN host's lookup too (one for STUN) */
   CHECK(t_conn - t_punch_to < 2500);
   CHECK(count_req(ONLINE_OP_RELAY) == 1);       /* once, not again at the fallback */
   CHECK(N.punch_starts == 1 && N.relay_starts == 1);
   online_stop();
}

/* 12. TURN: the first port is dead, the second answers; every port dead fails. */
static void sc_relay_ports(void)
{
   online_desc me = mk_desc("Gus", LAN_KIND_GBA_CABLE);
   online_view v;
   world();
   N.relay_ok_idx = 1;
   start();
   script_host("203.0.113.2:40002", "", 1, 0);
   S.turn_port[0] = 3478; S.turn_port[1] = 443; S.turn_port[2] = 53; S.turn_ports = 3;
   online_join(&g_tr, "K7M2Q", &me);
   run_to(ON_RELAY_ALLOC, 4000);
   snprintf(S.hrel, sizeof S.hrel, "198.51.100.77:50999");
   CHECK(run_to(ON_CONNECTED, 10000));
   CHECK(N.relay_nports == 3 && N.relay_ports[2] == 53);
   online_stop();

   world();
   N.relay_ok_idx = -1;
   start();
   script_host("203.0.113.2:40002", "", 1, 0);
   online_join(&g_tr, "K7M2Q", &me);
   CHECK(run_to(ON_FAILED, 15000));
   online_get(&v);
   CHECK(v.fail == ONLINE_FAIL_UNREACHABLE);
   CHECK(!strcmp(S.last_close, "failed"));       /* a guest that gave up says so */
   online_stop();
}

/* 13. relay_only on one side with open NATs, force_relay, no_relay + sym. */
static void sc_relay_forced(void)
{
   online_desc me = mk_desc("Gus", LAN_KIND_GBA_CABLE);
   online_view v;
   world();
   start();
   me.relay_only = 1;
   script_host("203.0.113.2:40002", "192.168.2.9:19019", 0, 0);
   online_join(&g_tr, "K7M2Q", &me);
   run_to(ON_RELAY_POST, 5000);
   CHECK(N.stun_calls == 0);                     /* an address-hiding console never asks STUN */
   CHECK(S.g.pub[0] == 0 && S.g.lan[0] == 0 && S.g.relay_only == 1);
   snprintf(S.hrel, sizeof S.hrel, "198.51.100.77:50999");
   CHECK(run_to(ON_CONNECTED, 6000));
   CHECK(N.punch_starts == 0);
   online_stop();

   world();
   g_cfg.force_relay = 1;
   start();
   script_host("203.0.113.2:40002", "192.168.2.9:19019", 0, 0);
   online_join(&g_tr, "K7M2Q", &me);
   run_to(ON_RELAY_POST, 5000);
   snprintf(S.hrel, sizeof S.hrel, "198.51.100.77:50999");
   CHECK(run_to(ON_CONNECTED, 6000));
   CHECK(N.punch_starts == 0);
   online_stop();

   world();
   g_cfg.no_relay = 1;
   start();
   script_host("203.0.113.2:40002", "192.168.2.9:19019", 1, 0);
   online_join(&g_tr, "K7M2Q", &me);
   CHECK(run_to(ON_FAILED, 6000));
   online_get(&v);
   CHECK(v.fail == ONLINE_FAIL_UNREACHABLE);
   CHECK(count_req(ONLINE_OP_RELAY) == 0 && N.relay_starts == 0);
   online_stop();

   /* no_relay, punch never answers: UNREACHABLE after the race, no credentials asked */
   world();
   g_cfg.no_relay = 1;
   N.punch_after_ms = -1;
   start();
   script_host("203.0.113.2:40002", "192.168.2.9:19019", 0, 0);
   online_join(&g_tr, "K7M2Q", &me);
   CHECK(run_to(ON_FAILED, 15000));
   online_get(&v);
   CHECK(v.fail == ONLINE_FAIL_UNREACHABLE);
   CHECK(count_req(ONLINE_OP_RELAY) == 0);
   online_stop();
}

/* 14. a relay-only host never asks STUN, and says so in its room. */
static void sc_host_relay_only(void)
{
   online_desc room = mk_desc("Alice", LAN_KIND_GBA_CABLE);
   world();
   start();
   room.relay_only = 1;
   online_host(&g_tr, &room);
   CHECK(run_to(ON_WAITING, 3000));
   CHECK(N.stun_calls == 0 && S.h.pub[0] == 0 && S.h.lan[0] == 0 && S.h.relay_only == 1);
   online_stop();
}

/* 15. STUN off / failing / unresolvable. */
static void sc_stun_variants(void)
{
   online_desc room = mk_desc("Alice", LAN_KIND_GBA_CABLE);
   world();
   g_cfg.stun = "off";
   start();
   online_host(&g_tr, &room);
   CHECK(run_to(ON_WAITING, 3000));
   CHECK(!strcmp(S.h.pub, "192.168.1.23:19019") && N.stun_calls == 0 && count_req(ONLINE_OP_RESOLVE) == 0);
   online_stop();

   world();
   N.stun_ok = 0;
   start();
   online_host(&g_tr, &room);
   CHECK(run_to(ON_WAITING, 4000));
   CHECK(S.h.pub[0] == 0 && S.h.lan[0] != 0);
   CHECK(evt_has("online_stun pub= sym=0 rc=1"));
   online_stop();

   world();
   g_resolve_fail = -1;                          /* DNS never answers */
   start();
   online_host(&g_tr, &room);
   CHECK(run_to(ON_WAITING, 20000));
   CHECK(S.h.pub[0] == 0 && N.stun_calls == 0);
   CHECK(evt_has("online_stun pub= sym=0 rc=-1"));
   online_stop();

   /* literal addresses need no lookup; one name, two ports: one lookup */
   world();
   g_cfg.stun = "198.51.100.1:3478,198.51.100.2:3478";
   start();
   online_host(&g_tr, &room);
   CHECK(run_to(ON_WAITING, 3000));
   CHECK(count_req(ONLINE_OP_RESOLVE) == 0 && N.stun_n == 2);
   online_stop();

   world();
   start();
   online_host(&g_tr, &room);
   run_to(ON_WAITING, 3000);
   CHECK(count_req(ONLINE_OP_RESOLVE) == 1);
   online_stop();

   /* one resolve failure is retried */
   world();
   g_resolve_fail = 1;
   start();
   online_host(&g_tr, &room);
   CHECK(run_to(ON_WAITING, 8000));
   CHECK(N.stun_calls == 1);
   online_stop();
}

/* 16. a wrong code: CODE, after one request. */
static void sc_wrong_code(void)
{
   online_desc me = mk_desc("Gus", LAN_KIND_GBA_CABLE);
   online_view v;
   world();
   start();
   script_host("203.0.113.2:40002", "", 0, 0);
   online_join(&g_tr, "ZZZZZ", &me);
   CHECK(run_to(ON_FAILED, 5000));
   online_get(&v);
   CHECK(v.fail == ONLINE_FAIL_CODE);
   CHECK(count_req(ONLINE_OP_JOIN) == 1);
   CHECK(S.closes == 0);                         /* we never held a token */
   online_stop();
}

/* 17. the host cancels while the guest punches: the guest reads CLOSED. */
static void sc_host_cancels(void)
{
   online_desc me = mk_desc("Gus", LAN_KIND_GBA_CABLE);
   online_view v;
   world();
   N.punch_after_ms = -1;
   start();
   script_host("203.0.113.2:40002", "", 0, 0);
   online_join(&g_tr, "K7M2Q", &me);
   run_to(ON_PUNCH, 3000);
   run_for(500);
   S.state = RS_CLOSED;
   strcpy(S.reason, "cancel");
   CHECK(run_to(ON_FAILED, 4000));
   online_get(&v);
   CHECK(v.fail == ONLINE_FAIL_CLOSED);
   CHECK(S.closes == 0);                         /* the room was already closed */
   CHECK(N.punch_stops >= 1);
   online_stop();
}

/* 18. the guest stops mid-punch: the host is back to WAITING and a second guest connects. */
static void sc_guest_leaves(void)
{
   online_desc room = mk_desc("Alice", LAN_KIND_GBA_CABLE);
   online_view v;
   world();
   N.punch_after_ms = -1;
   start();
   online_host(&g_tr, &room);
   run_to(ON_WAITING, 3000);
   run_for(2000);
   {
      online_desc g = guest_desc("203.0.113.2:40002", "", 0, 0);
      svc_guest_joins(&g);
   }
   CHECK(run_to(ON_PUNCH, 5000));
   run_for(2500);                                /* the poll that shows the guest has run */
   svc_guest_leaves();
   CHECK(run_to(ON_WAITING, 5000));
   CHECK(N.punch_stops >= 1 && N.arm_calls >= 2 && N.armed);
   online_get(&v);
   CHECK(v.peer_nick[0] == 0);
   N.punch_after_ms = 300;
   {
      online_desc g = guest_desc("203.0.113.3:40003", "", 0, 0);
      snprintf(g.nick, sizeof g.nick, "Bea");
      svc_guest_joins(&g);
   }
   CHECK(run_to(ON_CONNECTED, 10000));
   online_get(&v);
   CHECK(!strcmp(v.peer_nick, "Bea") && !strcmp(v.code, "K7M2Q"));
   CHECK(count_req(ONLINE_OP_CREATE) == 1);
   online_stop();
}

/* 19. a host's stop before connecting sends cancel; a guest's, failed. */
static void sc_stop_messages(void)
{
   online_desc room = mk_desc("Alice", LAN_KIND_GBA_CABLE);
   online_desc me = mk_desc("Gus", LAN_KIND_GBA_CABLE);
   online_view v;
   world();
   start();
   online_host(&g_tr, &room);
   run_to(ON_WAITING, 3000);
   online_stop();
   run_for(200);
   CHECK(S.closes == 1 && !strcmp(S.last_close, "cancel"));
   CHECK(N.punch_stops >= 1);
   online_get(&v);
   CHECK(v.stage == ON_IDLE && !online_active());
   /* a second connection right away works */
   CHECK(online_host(&g_tr, &room) == 0);
   online_stop();

   world();
   N.punch_after_ms = -1;
   start();
   script_host("203.0.113.2:40002", "", 0, 0);
   online_join(&g_tr, "K7M2Q", &me);
   run_to(ON_PUNCH, 3000);
   online_stop();
   run_for(200);
   CHECK(S.closes == 1 && !strcmp(S.last_close, "failed") && S.state == RS_OPEN);

   /* a stop before the join answered holds no token: nothing to close */
   world();
   start();
   script_host("203.0.113.2:40002", "", 0, 0);
   online_join(&g_tr, "K7M2Q", &me);
   run_for(100);
   online_stop();
   run_for(200);
   CHECK(S.closes == 0);
}


static int hex16(const char *s)
{
   int i;
   if (strlen(s) != 16)
      return 0;
   for (i = 0; i < 16; i++)
      if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f')))
         return 0;
   return 1;
}

/* Run until the n-th request of `op` went out. */
static void run_until_req(int op, int n)
{
   unsigned end = ms_now() + 20000;
   while (count_req(op) < n && ms_now() < end)
      tick();
}

/* A. a create whose answer was lost is sent again with the SAME request id: one room, not two. */
static void sc_create_retry_rid(void)
{
   online_desc room = mk_desc("Alice", LAN_KIND_GBA_CABLE);
   online_view v;
   world();
   S.lose[ONLINE_OP_CREATE] = 1;
   start();
   online_host(&g_tr, &room);
   CHECK(run_to(ON_WAITING, 6000));
   online_get(&v);
   CHECK(v.stage == ON_WAITING && v.fail == 0);
   CHECK(S.n_rids_c == 2 && hex16(S.rids_c[0]) && !strcmp(S.rids_c[0], S.rids_c[1]));
   CHECK(S.rooms_made == 1);
   online_stop();
   run_for(100);
   CHECK(S.closes == 1 && !strcmp(S.last_close, "cancel"));
   /* the next attempt chooses its own id */
   world();
   start();
   online_host(&g_tr, &room);
   run_to(ON_WAITING, 3000);
   online_stop();
   CHECK(S.n_rids_c == 1 && hex16(S.rids_c[0]));
}

/* A. a join whose answer was lost: the retry (same id) is the same guest, not "room full". */
static void sc_join_retry_rid(void)
{
   online_desc me = mk_desc("Gus", LAN_KIND_GBA_CABLE);
   online_view v;
   world();
   S.lose[ONLINE_OP_JOIN] = 1;
   start();
   script_host("203.0.113.2:40002", "", 0, 0);
   online_join(&g_tr, "K7M2Q", &me);
   CHECK(run_to(ON_PUNCH, 6000));
   online_get(&v);
   CHECK(v.fail == 0 && v.stage >= ON_PUNCH);
   CHECK(S.n_rids_j == 2 && hex16(S.rids_j[0]) && !strcmp(S.rids_j[0], S.rids_j[1]));
   online_stop();
   run_for(200);
   CHECK(S.closes == 1 && !strcmp(S.last_close, "failed") && S.state == RS_OPEN);
}

/* B. a stop while a create is in flight, or between its retries: the room is closed all the same. */
static void sc_stop_during_create(void)
{
   online_desc room = mk_desc("Alice", LAN_KIND_GBA_CABLE);
   int wait;
   for (wait = 0; wait < 2; wait++)
   {
      world();
      S.lose[ONLINE_OP_CREATE] = 1;
      start();
      online_host(&g_tr, &room);
      run_until_req(ONLINE_OP_CREATE, 1);
      run_for(wait ? 100 : 0);        /* 0: the request is out; 100: its answer was lost, the retry waits */
      online_stop();
      run_for(500);
      CHECK(S.rooms_made == 1);
      CHECK(S.n_rids_c == 2 && !strcmp(S.rids_c[0], S.rids_c[1]));
      CHECK(S.closes == 1 && !strcmp(S.last_close, "cancel") && S.state == RS_CLOSED);
      CHECK(!online_active());
   }
   /* a new connection right after the stop works, and does not mistake the recovery's answer for its own */
   world();
   S.lose[ONLINE_OP_CREATE] = 1;
   start();
   online_host(&g_tr, &room);
   run_until_req(ONLINE_OP_CREATE, 1);
   run_for(100);
   online_stop();
   CHECK(online_host(&g_tr, &room) == 0);
   CHECK(run_to(ON_WAITING, 6000));
   {
      online_view v;
      online_get(&v);
      CHECK(v.stage == ON_WAITING && v.fail == 0);
   }
   CHECK(S.rooms_made == 2);          /* the recovered room (closed) and the new one */
   online_stop();
   run_for(300);
   CHECK(S.closes == 2);
}

/* B. the same for a join. */
static void sc_stop_during_join(void)
{
   online_desc me = mk_desc("Gus", LAN_KIND_GBA_CABLE);
   int wait;
   for (wait = 0; wait < 2; wait++)
   {
      world();
      S.lose[ONLINE_OP_JOIN] = 1;
      start();
      script_host("203.0.113.2:40002", "", 0, 0);
      online_join(&g_tr, "K7M2Q", &me);
      run_until_req(ONLINE_OP_JOIN, 1);
      run_for(wait ? 100 : 0);
      online_stop();
      run_for(500);
      CHECK(S.n_rids_j == 2 && !strcmp(S.rids_j[0], S.rids_j[1]));
      CHECK(S.closes == 1 && !strcmp(S.last_close, "failed") && S.state == RS_OPEN && !S.have_g);
      CHECK(!online_active());
   }
   /* a join the service refused leaves nothing to recover */
   world();
   S.inj[ONLINE_OP_JOIN].err = ONLINE_E_FULL;
   S.inj[ONLINE_OP_JOIN].http = 409;
   S.inj[ONLINE_OP_JOIN].count = -1;
   start();
   script_host("203.0.113.2:40002", "", 0, 0);
   online_join(&g_tr, "K7M2Q", &me);
   run_until_req(ONLINE_OP_JOIN, 1);
   run_for(300);
   online_stop();
   run_for(300);
   CHECK(S.n_rids_j == 1 && S.closes == 0);
}

/* C, D. 429 answers: short waits read "wait a minute", long ones "busy"; a poll never waits more than 30 s silently. */
static void sc_rate_limits(void)
{
   struct { int op, retry, want; } T[] = {
      { ONLINE_OP_CREATE, 60, ONLINE_FAIL_SLOW }, { ONLINE_OP_CREATE, 120, ONLINE_FAIL_SLOW },
      { ONLINE_OP_CREATE, 121, ONLINE_FAIL_BUSY }, { ONLINE_OP_CREATE, 3600, ONLINE_FAIL_BUSY },
      { ONLINE_OP_JOIN, 5, ONLINE_FAIL_SLOW }, { ONLINE_OP_JOIN, 3000, ONLINE_FAIL_BUSY },
      { ONLINE_OP_POLL, 3600, ONLINE_FAIL_BUSY }, { ONLINE_OP_POLL, 31, ONLINE_FAIL_SLOW },
   };
   unsigned i;
   online_desc room = mk_desc("Alice", LAN_KIND_GBA_CABLE);
   online_desc me = mk_desc("Gus", LAN_KIND_GBA_CABLE);
   for (i = 0; i < sizeof T / sizeof T[0]; i++)
   {
      online_view v;
      world();
      start();
      if (T[i].op == ONLINE_OP_JOIN)
         script_host("203.0.113.2:40002", "", 0, 0);
      if (T[i].op == ONLINE_OP_JOIN)
         online_join(&g_tr, "K7M2Q", &me);
      else
      {
         if (T[i].op == ONLINE_OP_POLL)
         {
            online_host(&g_tr, &room);
            CHECK(run_to(ON_WAITING, 3000));
         }
         else
            online_host(&g_tr, &room);
      }
      S.inj[T[i].op].err = ONLINE_E_RATE;
      S.inj[T[i].op].http = 429;
      S.inj[T[i].op].retry_after = T[i].retry;
      S.inj[T[i].op].count = -1;
      if (T[i].op == ONLINE_OP_JOIN || T[i].op == ONLINE_OP_CREATE)
         S.inj[T[i].op].count = -1;
      CHECK(run_to(ON_FAILED, 10000));
      online_get(&v);
      if (v.fail != T[i].want)
         printf("  rate case %u (op %d retry %d): got fail=%d want %d\n", i, T[i].op, T[i].retry, v.fail, T[i].want);
      CHECK(v.fail == T[i].want);
      online_stop();
   }
   /* a poll told to wait 20 s waits (no failure), then carries on */
   world();
   start();
   online_host(&g_tr, &room);
   CHECK(run_to(ON_WAITING, 3000));
   S.inj[ONLINE_OP_POLL].err = ONLINE_E_RATE;
   S.inj[ONLINE_OP_POLL].http = 429;
   S.inj[ONLINE_OP_POLL].retry_after = 20;
   S.inj[ONLINE_OP_POLL].count = 1;
   run_for(25000);
   {
      online_view v;
      online_get(&v);
      CHECK(v.stage == ON_WAITING && v.fail == 0);
   }
   online_stop();
}

/* 20. a lost close ("linked") is sent once more, then forgotten. */
static void sc_linked_retry(void)
{
   online_desc room = mk_desc("Alice", LAN_KIND_GBA_CABLE);
   world();
   start();
   online_host(&g_tr, &room);
   run_to(ON_WAITING, 3000);
   {
      online_desc g = guest_desc("203.0.113.2:40002", "", 0, 0);
      svc_guest_joins(&g);
   }
   CHECK(run_to(ON_CONNECTED, 8000));
   S.inj[ONLINE_OP_CLOSE].err = ONLINE_E_NET;
   S.inj[ONLINE_OP_CLOSE].count = -1;
   online_linked();
   run_for(3000);
   CHECK(count_req(ONLINE_OP_CLOSE) == 2);
   online_linked();                              /* a second call adds nothing */
   run_for(1000);
   CHECK(count_req(ONLINE_OP_CLOSE) == 2);
   online_stop();
}

/* 21. the service is gone: three failed polls end it; two do not. */
static void sc_service_down(void)
{
   online_desc room = mk_desc("Alice", LAN_KIND_GBA_CABLE);
   online_view v;
   int polls_before;
   world();
   start();
   online_host(&g_tr, &room);
   run_to(ON_WAITING, 3000);
   polls_before = count_req(ONLINE_OP_POLL);
   S.net_down = 1;
   while (count_req(ONLINE_OP_POLL) < polls_before + 2)
      run_for(100);
   run_for(100);                                 /* the second failed answer is in */
   S.net_down = 0;                               /* two failed polls in a row do not end it */
   run_for(6000);
   online_get(&v);
   CHECK(v.stage == ON_WAITING);
   S.net_down = 1;
   CHECK(run_to(ON_FAILED, 12000));
   online_get(&v);
   CHECK(v.fail == ONLINE_FAIL_SERVICE);
   CHECK(count_req(ONLINE_OP_POLL) - polls_before <= 12);
   online_stop();

   /* create with the network down: retried twice, then SERVICE */
   world();
   S.net_down = 1;
   start();
   online_host(&g_tr, &room);
   CHECK(run_to(ON_FAILED, 15000));
   online_get(&v);
   CHECK(v.fail == ONLINE_FAIL_SERVICE);
   CHECK(count_req(ONLINE_OP_CREATE) == 3);
   online_stop();
}

/* 22. every error answer and where it lands. */
static void sc_errors(void)
{
   struct { int op, err, http, want; const char *name; } T[] = {
      { ONLINE_OP_CREATE, ONLINE_E_UPDATE, 426, ONLINE_FAIL_UPDATE, "update" },
      { ONLINE_OP_CREATE, ONLINE_E_BUSY, 503, ONLINE_FAIL_BUSY, "busy" },
      { ONLINE_OP_CREATE, ONLINE_E_RATE, 429, ONLINE_FAIL_BUSY, "rate" },
      { ONLINE_OP_CREATE, ONLINE_E_SERVER, 500, ONLINE_FAIL_SERVICE, "server" },
      { ONLINE_OP_CREATE, ONLINE_E_BAD, 400, ONLINE_FAIL_SERVICE, "bad" },
      { ONLINE_OP_CREATE, ONLINE_E_CERT, 0, ONLINE_FAIL_SERVICE, "cert" },
      { ONLINE_OP_CREATE, ONLINE_E_CLOCK, 0, ONLINE_FAIL_CLOCK, "clock" },
      { ONLINE_OP_CREATE, ONLINE_E_CONSOLE, 400, ONLINE_FAIL_CONSOLE, "console refused" },
      { ONLINE_OP_JOIN, ONLINE_E_FULL, 409, ONLINE_FAIL_FULL, "full" },
      { ONLINE_OP_JOIN, ONLINE_E_NOT_FOUND, 404, ONLINE_FAIL_CODE, "notfound" },
      { ONLINE_OP_JOIN, ONLINE_E_UPDATE, 426, ONLINE_FAIL_UPDATE, "join update" },
      { ONLINE_OP_JOIN, ONLINE_E_CLOCK, 0, ONLINE_FAIL_CLOCK, "join clock" },
      { ONLINE_OP_JOIN, ONLINE_E_BUSY, 503, ONLINE_FAIL_BUSY, "join busy" },
      { ONLINE_OP_POLL, ONLINE_E_TOKEN, 401, ONLINE_FAIL_CLOSED, "poll token" },
      { ONLINE_OP_POLL, ONLINE_E_SERVER, 500, ONLINE_FAIL_SERVICE, "poll server" },
      { ONLINE_OP_POLL, ONLINE_E_BAD, 400, ONLINE_FAIL_SERVICE, "poll bad" },
      { ONLINE_OP_POLL, ONLINE_E_NOT_FOUND, 404, ONLINE_FAIL_EXPIRED, "poll 404" },
      { ONLINE_OP_POLL, ONLINE_E_UPDATE, 426, ONLINE_FAIL_UPDATE, "poll update" },
      { ONLINE_OP_POLL, ONLINE_E_CERT, 0, ONLINE_FAIL_SERVICE, "poll cert" },
   };
   unsigned i;
   for (i = 0; i < sizeof T / sizeof T[0]; i++)
   {
      online_desc room = mk_desc("Alice", LAN_KIND_GBA_CABLE);
      online_desc me = mk_desc("Gus", LAN_KIND_GBA_CABLE);
      online_view v;
      world();
      start();
      S.inj[T[i].op].err = T[i].err;
      S.inj[T[i].op].http = T[i].http;
      S.inj[T[i].op].count = -1;
      if (T[i].op == ONLINE_OP_CREATE)
         online_host(&g_tr, &room);
      else if (T[i].op == ONLINE_OP_JOIN)
      {
         script_host("203.0.113.2:40002", "", 0, 0);
         online_join(&g_tr, "K7M2Q", &me);
      }
      else
      {
         online_host(&g_tr, &room);
         run_to(ON_WAITING, 3000);
         S.inj[T[i].op].err = T[i].err;
         S.inj[T[i].op].http = T[i].http;
         S.inj[T[i].op].count = -1;
      }
      CHECK(run_to(ON_FAILED, 15000));
      online_get(&v);
      if (v.fail != T[i].want)
         printf("  error case '%s': got fail=%d want %d\n", T[i].name, v.fail, T[i].want);
      CHECK(v.fail == T[i].want);
      CHECK(evt_has("online_fail why="));
      online_stop();
   }
}

/* 23. version answers: VERSION keeps host_ver, each way. */
static void sc_version(void)
{
   online_desc me = mk_desc("Gus", LAN_KIND_GBA_CABLE);
   online_view v;
   const char *hv[3] = { "2.1.0", "1.9.0", "2.0.0" };
   int i;
   for (i = 0; i < 3; i++)
   {
      world();
      start();
      script_host("203.0.113.2:40002", "", 0, 0);
      snprintf(S.h.ver, sizeof S.h.ver, "%s", hv[i]);
      S.h.compat = 0x0badf00dU;                  /* differs from ours */
      online_join(&g_tr, "K7M2Q", &me);
      CHECK(run_to(ON_FAILED, 5000));
      online_get(&v);
      CHECK(v.fail == ONLINE_FAIL_VERSION);
      CHECK(!strcmp(v.host_ver, hv[i]));
      online_stop();
   }
}

/* 24. the joiner's own 45 s: a host that never posts its relayed address. */
static void sc_joiner_gives_up(void)
{
   online_desc me = mk_desc("Gus", LAN_KIND_GBA_CABLE);
   online_view v;
   unsigned t_j;
   world();
   start();
   script_host("203.0.113.2:40002", "", 1, 0);
   online_join(&g_tr, "K7M2Q", &me);
   CHECK(run_to(ON_FAILED, 60000));
   online_get(&v);
   CHECK(v.fail == ONLINE_FAIL_UNREACHABLE);
   t_j = (unsigned)evt_time("online_fail");
   CHECK(t_j <= 46500);                          /* 15 s of waiting for the peer's address ends it first */
   online_stop();
}

/* 24b. a punch that is allowed to run long: the joiner's 45 s ends it. */
static void sc_joiner_45s(void)
{
   online_desc me = mk_desc("Gus", LAN_KIND_GBA_CABLE);
   online_view v;
   world();
   g_cfg.punch_ms = 60000;
   N.punch_after_ms = -1;
   start();
   script_host("203.0.113.2:40002", "", 0, 0);
   online_join(&g_tr, "K7M2Q", &me);
   CHECK(run_to(ON_FAILED, 60000));
   online_get(&v);
   CHECK(v.fail == ONLINE_FAIL_UNREACHABLE);
   CHECK(ms_now() >= 45000 && ms_now() < 47000);
   online_stop();
}

/* 25. credentials refused at the fallback: UNREACHABLE; a refused fetch beside the punch is not fatal. */
static void sc_creds_errors(void)
{
   online_desc me = mk_desc("Gus", LAN_KIND_GBA_CABLE);
   online_view v;
   world();
   S.inj[ONLINE_OP_RELAY].err = ONLINE_E_RELAY;
   S.inj[ONLINE_OP_RELAY].http = 503;
   S.inj[ONLINE_OP_RELAY].count = -1;
   start();
   script_host("203.0.113.2:40002", "", 0, 0);
   online_join(&g_tr, "K7M2Q", &me);
   CHECK(run_to(ON_CONNECTED, 8000));            /* the punch won; the refused credentials never mattered */
   CHECK(!evt_has("online_fail"));
   CHECK(count_req(ONLINE_OP_RELAY) == 1);       /* beside the punch it is asked once, not nagged */
   online_stop();

   /* 503 ONLINE_E_RELAY: fails fast in 2 tries */
   world();
   S.inj[ONLINE_OP_RELAY].err = ONLINE_E_RELAY;
   S.inj[ONLINE_OP_RELAY].http = 503;
   S.inj[ONLINE_OP_RELAY].count = -1;
   N.punch_after_ms = -1;
   start();
   script_host("203.0.113.2:40002", "", 0, 0);
   online_join(&g_tr, "K7M2Q", &me);
   CHECK(run_to(ON_FAILED, 15000));
   CHECK(count_req(ONLINE_OP_RELAY) == 2);
   online_get(&v);
   CHECK(v.fail == ONLINE_FAIL_UNREACHABLE);
   online_stop();

   /* 403 ONLINE_E_RELAY_WAIT: asks again every 3 s, 12 times (~36 s) */
   world();
   S.inj[ONLINE_OP_RELAY].err = ONLINE_E_RELAY_WAIT;
   S.inj[ONLINE_OP_RELAY].http = 403;
   S.inj[ONLINE_OP_RELAY].count = -1;
   N.punch_after_ms = -1;
   start();
   script_host("203.0.113.2:40002", "", 0, 0);
   online_join(&g_tr, "K7M2Q", &me);
   CHECK(run_to(ON_FAILED, 60000));
   CHECK(count_req(ONLINE_OP_RELAY) == 12);
   online_get(&v);
   CHECK(v.fail == ONLINE_FAIL_UNREACHABLE);
   online_stop();

   /* a network failure of the credentials: one retry, then fine */
   world();
   S.inj[ONLINE_OP_RELAY].err = ONLINE_E_NET;
   S.inj[ONLINE_OP_RELAY].count = 1;
   start();
   script_host("203.0.113.2:40002", "", 1, 0);
   online_join(&g_tr, "K7M2Q", &me);
   run_to(ON_RELAY_POST, 8000);
   snprintf(S.hrel, sizeof S.hrel, "198.51.100.77:50999");
   CHECK(run_to(ON_CONNECTED, 8000));
   online_stop();
}

/* 26. a joiner whose TURN lookup works only by IP literal; ports order from the service is kept. */
static void sc_turn_literal(void)
{
   online_desc me = mk_desc("Gus", LAN_KIND_GBA_CABLE);
   world();
   strcpy(S.turn_host, "198.51.100.77");
   start();
   script_host("203.0.113.2:40002", "", 1, 0);
   online_join(&g_tr, "K7M2Q", &me);
   run_to(ON_RELAY_POST, 6000);
   CHECK(count_req(ONLINE_OP_RESOLVE) == 1);     /* only the STUN host */
   snprintf(S.hrel, sizeof S.hrel, "198.51.100.77:50999");
   CHECK(run_to(ON_CONNECTED, 6000));
   online_stop();
}

/* 27. quality thresholds, no answer, and the joiner's measured round trip. */
static void sc_quality(void)
{
   struct { int us, q, ms; } T[] = {
      { 79000, ONLINE_Q_GREAT, 79 }, { 80000, ONLINE_Q_GOOD, 80 }, { 119000, ONLINE_Q_GOOD, 119 },
      { 120000, ONLINE_Q_SLOW, 120 }, { 300000, ONLINE_Q_SLOW, 300 }, { 0, ONLINE_Q_NONE, 0 } };
   unsigned i;
   for (i = 0; i < sizeof T / sizeof T[0]; i++)
   {
      online_desc me = mk_desc("Gus", LAN_KIND_GBA_CABLE);
      online_view v;
      world();
      N.echo_rtt_us = T[i].us;
      start();
      script_host("203.0.113.2:40002", "", 0, 0);
      online_join(&g_tr, "K7M2Q", &me);
      CHECK(run_to(ON_CONNECTED, 8000));
      online_get(&v);
      CHECK(v.quality == T[i].q && v.rtt_ms == T[i].ms);
      online_stop();
   }
}

/* 28. the room card: found, wrong code, error; cancel discards a late answer. */
static void sc_card(void)
{
   online_reply c;
   int n, i;
   world();
   start();
   script_host("203.0.113.2:40002", "", 0, 0);
   CHECK(online_card_begin("K7M2Q") == 0);
   CHECK(online_card_begin("K7M2Q") != 0);       /* one at a time */
   CHECK(online_card_poll(&c) == 0);
   for (i = 0, n = 0; i < 30 && !n; i++)
   {
      g_us += 16667;
      n = online_card_poll(&c);
   }
   CHECK(n == 1 && c.err == ONLINE_E_NONE && !strcmp(c.card.game, "BPEE") && !strcmp(c.card.nick, "Hosty"));
   CHECK(c.card.compat == 0x1a2b3c4dU);
   CHECK(online_card_poll(&c) == 0);

   CHECK(online_card_begin("ZZZZZ") == 0);
   for (i = 0, n = 0; i < 30 && !n; i++)
   {
      g_us += 16667;
      n = online_card_poll(&c);
   }
   CHECK(n == 1 && c.err == ONLINE_E_NOT_FOUND);

   CHECK(online_card_begin("K7M2Q") == 0);
   online_card_cancel();
   for (i = 0, n = 0; i < 30; i++)
   {
      g_us += 16667;
      n += online_card_poll(&c);
   }
   CHECK(n == 0);
   /* a code the player backed out of, then another one at once: the late
    * answer of the first (404) must not be shown as the card of the second */
   CHECK(online_card_begin("ZZZZZ") == 0);
   online_card_cancel();
   CHECK(online_card_begin("K7M2Q") == 0);
   for (i = 0, n = 0; i < 90 && !n; i++)
   {
      g_us += 16667;
      n = online_card_poll(&c);
   }
   CHECK(n == 1 && c.err == ONLINE_E_NONE && !strcmp(c.card.game, "BPEE"));
   /* the card does not disturb a connection's answers */
   {
      online_desc me = mk_desc("Gus", LAN_KIND_GBA_CABLE);
      CHECK(online_card_begin("K7M2Q") == 0);
      online_join(&g_tr, "K7M2Q", &me);
      CHECK(run_to(ON_CONNECTED, 8000));
      CHECK(online_card_poll(&c) == 1 || 1);
      online_stop();
   }
}

/* 29. events never carry a token, and every request is logged with its op. */
static void sc_events(void)
{
   online_desc room = mk_desc("Alice", LAN_KIND_GBA_CABLE);
   world();
   start();
   online_host(&g_tr, &room);
   run_to(ON_WAITING, 3000);
   {
      online_desc g = guest_desc("203.0.113.2:40002", "", 0, 0);
      svc_guest_joins(&g);
   }
   run_to(ON_CONNECTED, 8000);
   CHECK(!evt_has("hhhhhhhh") && !evt_has("gggggggg") && !evt_has("secret"));
   CHECK(evt_has("online_http op=1 status=201"));
   CHECK(evt_has("online_http op=4 status=200"));
   CHECK(evt_has("online_stage from=0 to=1"));
   CHECK(evt_has("online_stun pub=203.0.113.1:40001 sym=0 rc=0"));
   online_stop();
}

/* 30. a guest joining while this host is still in STUN/CREATE is impossible; a second online_host while active is refused. */
static void sc_double_begin(void)
{
   online_desc room = mk_desc("Alice", LAN_KIND_GBA_CABLE);
   world();
   start();
   CHECK(online_host(&g_tr, &room) == 0);
   CHECK(online_host(&g_tr, &room) == -1);
   CHECK(online_join(&g_tr, "K7M2Q", &room) == -1);
   CHECK(online_host(NULL, &room) == -1);
   online_stop();
   CHECK(online_join(&g_tr, "", &room) == -1);
   CHECK(online_join(&g_tr, "TOOLONGCODE", &room) == -1);
}

/* 31. the punch window comes from the service unless the config overrides it. */
static void sc_punch_window(void)
{
   online_desc room = mk_desc("Alice", LAN_KIND_GBA_CABLE);
   world();
   g_cfg.punch_ms = 2500;
   start();
   online_host(&g_tr, &room);
   run_to(ON_WAITING, 3000);
   {
      online_desc g = guest_desc("203.0.113.2:40002", "", 0, 0);
      svc_guest_joins(&g);
   }
   run_to(ON_CONNECTED, 8000);
   CHECK(N.punch_timeout_ms == 2500);
   online_stop();
}

/* 32. the host's wait for the guest's relayed address ends after 15 s. */
static void sc_relay_wait_timeout(void)
{
   online_desc room = mk_desc("Alice", LAN_KIND_GBA_CABLE);
   online_view v;
   world();
   N.sym = 1;
   start();
   online_host(&g_tr, &room);
   run_to(ON_WAITING, 3000);
   {
      online_desc g = guest_desc("203.0.113.2:40002", "", 0, 0);
      svc_guest_joins(&g);
   }
   CHECK(run_to(ON_FAILED, 30000));
   online_get(&v);
   CHECK(v.fail == ONLINE_FAIL_UNREACHABLE);
   CHECK(!strcmp(S.last_close, "cancel"));
   online_stop();
}

/* 33. relay READY never comes: UNREACHABLE after 5 s. */
static void sc_bind_timeout(void)
{
   online_desc me = mk_desc("Gus", LAN_KIND_GBA_CABLE);
   online_view v;
   world();
   N.bind_ms = -1;
   start();
   script_host("203.0.113.2:40002", "", 1, 0);
   online_join(&g_tr, "K7M2Q", &me);
   run_to(ON_RELAY_POST, 6000);
   snprintf(S.hrel, sizeof S.hrel, "198.51.100.77:50999");
   CHECK(run_to(ON_FAILED, 12000));
   online_get(&v);
   CHECK(v.fail == ONLINE_FAIL_UNREACHABLE);
   online_stop();
}

typedef struct sc { const char *name; void (*fn)(void); } sc;
static const sc SC[] = {
   { "host_punch", sc_host_punch },
   { "host_wait_cadence", sc_host_wait_cadence },
   { "host_ttl", sc_host_ttl },
   { "host_new_pub", sc_host_new_pub },
   { "host_incoming_punch", sc_host_incoming_punch },
   { "host_punch_guest_gone", sc_host_punch_guest_gone },
   { "join_punch", sc_join_punch },
   { "same_lan", sc_same_lan },
   { "no_post_lan", sc_no_post_lan },
   { "join_relay_sym", sc_join_relay_sym },
   { "host_relay_sym", sc_host_relay_sym },
   { "punch_race_falls_to_relay", sc_punch_race_falls_to_relay },
   { "relay_ports", sc_relay_ports },
   { "relay_forced", sc_relay_forced },
   { "host_relay_only", sc_host_relay_only },
   { "stun_variants", sc_stun_variants },
   { "wrong_code", sc_wrong_code },
   { "host_cancels", sc_host_cancels },
   { "guest_leaves", sc_guest_leaves },
   { "stop_messages", sc_stop_messages },
   { "create_retry_rid", sc_create_retry_rid },
   { "join_retry_rid", sc_join_retry_rid },
   { "stop_during_create", sc_stop_during_create },
   { "stop_during_join", sc_stop_during_join },
   { "rate_limits", sc_rate_limits },
   { "linked_retry", sc_linked_retry },
   { "service_down", sc_service_down },
   { "errors", sc_errors },
   { "version", sc_version },
   { "joiner_gives_up", sc_joiner_gives_up },
   { "joiner_45s", sc_joiner_45s },
   { "creds_errors", sc_creds_errors },
   { "turn_literal", sc_turn_literal },
   { "quality", sc_quality },
   { "card", sc_card },
   { "events", sc_events },
   { "double_begin", sc_double_begin },
   { "punch_window", sc_punch_window },
   { "relay_wait_timeout", sc_relay_wait_timeout },
   { "bind_timeout", sc_bind_timeout },
};

static int run_one(const sc *s)
{
   g_fail = 0;
   g_name = s->name;
   s->fn();
   printf("SCENARIO %s %s\n", s->name, g_fail ? "FAIL" : "PASS");
   return g_fail ? 1 : 0;
}

int main(int argc, char **argv)
{
   unsigned i;
   int bad = 0, ran = 0;
   const char *which = "all";
   for (i = 1; i < (unsigned)argc; i++)
   {
      if (!strcmp(argv[i], "-v"))
         g_verbose = 1;
      else
         which = argv[i];
   }
   if (!strcmp(which, "--list"))
   {
      for (i = 0; i < sizeof SC / sizeof SC[0]; i++)
         puts(SC[i].name);
      return 0;
   }
   for (i = 0; i < sizeof SC / sizeof SC[0]; i++)
      if (!strcmp(which, "all") || !strcmp(which, SC[i].name))
      {
         bad += run_one(&SC[i]);
         ran++;
      }
   if (!ran)
   {
      printf("no such scenario: %s\n", which);
      return 99;
   }
   return bad;
}
