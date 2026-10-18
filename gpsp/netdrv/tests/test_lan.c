/* test_lan.c -- transport_lan on real sockets, in real time, on this machine.
 *
 * Every "console" is what a Vita is to netdrv: one transport_lan (its own
 * socket and I/O thread) and one netdrv instance pumped by a thread of its
 * own at 1 ms ticks -- so the SPSC rings really are crossed by two threads
 * and a ThreadSanitizer run means something.  The consoles talk over the
 * loopback network (LAN_F_LOOPBACK: 127.0.0.1 + 127.255.255.255), each on a
 * port of its own, with the transport's fault shim standing in for Wi-Fi
 * and the internet.
 *
 *   helpers    ip[:port] parsing, formatting, fake MACs, the compat hash
 *   discovery  three rooms on one "LAN" (two of them on fallback ports), all
 *              fields, the compat flag, round trip, update, GONE, expiry
 *   portbusy   the default port taken: fallback, and the no-fallback refusal
 *   beacon     a room is announced without being asked (the beacon port)
 *   session    host + two joiners, each a thread; join by scan result and by
 *              typed "ip:port"; reliable data in order both ways and across
 *              the mesh under 5 % loss + 40 ms latency + 30 ms jitter
 *   bcastjoin  a JOIN broadcast to the subnet finds the one host that
 *              speaks the protocol, among two
 *   nat        joiner and host see each other only through an address-
 *              rewriting relay: the declared->observed alias carries the
 *              session and the bulk lane
 *   bulk       1 MiB over the bulk lane with 5 % loss, a NACK bitmap on the
 *              ordered channel; nothing of it reaches netdrv
 *   hostile    discovery datagrams, garbage and foreign bulk aimed at a
 *              running session: none delivered, none counted by netdrv
 *   cycles     create/session/destroy over and over: no descriptor or
 *              thread is left behind
 *   realnet    discovery + join over the machine's real interface and its
 *              broadcast addresses (skipped where there is none)
 *
 *   test_lan            run everything
 *   test_lan NAME...    run only those
 *   test_lan bench [LINK]  ARQ-profile table (not a test; see the report)
 */
#include <dirent.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "netdrv.h"
#include "netdrv_wire.h"
#include "nd_thread.h"
#include "transport_lan.h"

static int g_fail;
#define CHECK(cond, ...) do { \
   if (!(cond)) { \
      g_fail++; \
      printf("  FAIL %s:%d: %s -- ", __FILE__, __LINE__, #cond); \
      printf(__VA_ARGS__); \
      printf("\n"); \
   } } while (0)

/* Each test gets a 16-port block of its own, offset by the pid so several
 * runs on one machine do not meet. */
static uint16_t g_port_base;
static uint16_t test_port(int test)
{
   return (uint16_t)(g_port_base + test * 16);
}

static unsigned g_net_flags = LAN_F_LOOPBACK;

#define PROTO_A "GBAdhoc GB link v2"
#define PROTO_B "GBAdhoc cable v1"

/* ================================================================ console == */

#define MSG_MAX ND_MAX_PAYLOAD

typedef struct console console;
struct console
{
   const char *name;
   lan_transport *tp;
   netdrv *nd;
   pthread_t th;
   atomic_int run;
   int     threaded;

   /* written by the console thread, read by the test thread */
   atomic_int active, local_id, peers, stop_reason;
   atomic_int connects, disconnects;
   atomic_uint rx_ok[ND_MAX_CLIENTS];   /* in-order valid payloads per source */
   atomic_uint rx_bad;                  /* out of order / damaged */
   atomic_uint sent;

   /* the plan, set by the test thread before `go` */
   atomic_int go;
   unsigned to_send;                    /* reliable broadcasts */
   unsigned send_every_ms;
   uint64_t last_send_us;

   /* test-specific hooks, run on the console thread */
   void (*tick)(console *c, uint64_t now);
   int  (*deliver)(console *c, const uint8_t *buf, size_t len, uint8_t src);
   void *user;
};

/* payload: u32 seq, u8 sender id, then a pattern derived from both; the
 * length varies with seq across the whole range the core uses and beyond */
static size_t msg_len(uint32_t seq)
{
   static const uint16_t L[] = { 16, 36, 104, 8, 240, MSG_MAX, 61, 104 };
   size_t n = L[seq % (sizeof(L) / sizeof(L[0]))];
   return n > MSG_MAX ? MSG_MAX : n;
}

static size_t msg_build(uint8_t *b, uint32_t seq, uint8_t from)
{
   size_t n = msg_len(seq), i;
   b[0] = (uint8_t)seq; b[1] = (uint8_t)(seq >> 8);
   b[2] = (uint8_t)(seq >> 16); b[3] = (uint8_t)(seq >> 24);
   b[4] = from;
   for (i = 5; i < n; i++)
      b[i] = (uint8_t)(seq * 31u + from * 7u + i);
   return n;
}

static int msg_check(const uint8_t *b, size_t len, uint32_t want_seq, uint8_t from)
{
   uint8_t ref[MSG_MAX];
   size_t n = msg_build(ref, want_seq, from);
   return len == n && !memcmp(ref, b, n);
}

static void con_deliver(void *user, const void *buf, size_t len, uint8_t src)
{
   console *c = (console *)user;
   if (c->deliver && c->deliver(c, (const uint8_t *)buf, len, src))
      return;
   if (src < ND_MAX_CLIENTS &&
       msg_check((const uint8_t *)buf, len, atomic_load(&c->rx_ok[src]), src))
      atomic_fetch_add(&c->rx_ok[src], 1);
   else
      atomic_fetch_add(&c->rx_bad, 1);
}

static void con_started(void *user, uint8_t id)
{
   console *c = (console *)user;
   atomic_store(&c->local_id, id);
   atomic_store(&c->active, 1);
}

static int con_connected(void *user, uint8_t id)
{
   console *c = (console *)user;
   (void)id;
   atomic_fetch_add(&c->connects, 1);
   return 0;
}

static void con_disconnected(void *user, uint8_t id)
{
   console *c = (console *)user;
   (void)id;
   atomic_fetch_add(&c->disconnects, 1);
}

static void con_stopped(void *user, int reason)
{
   console *c = (console *)user;
   atomic_store(&c->stop_reason, reason);
   atomic_store(&c->active, 0);
}

static void con_step(console *c)
{
   uint64_t now = nd_now_us();
   netdrv_pump(c->nd, now);
   atomic_store(&c->peers, netdrv_peer_count(c->nd));
   if (c->tick)
      c->tick(c, now);
   if (atomic_load(&c->go) && netdrv_active(c->nd) &&
       netdrv_peer_count(c->nd) > 0 &&
       atomic_load(&c->sent) < c->to_send &&
       now - c->last_send_us >= (uint64_t)c->send_every_ms * 1000u)
   {
      uint8_t b[MSG_MAX];
      uint32_t seq = atomic_load(&c->sent);
      size_t n = msg_build(b, seq, (uint8_t)netdrv_local_id(c->nd));
      if (netdrv_send(c->nd, ND_RELIABLE | ND_FLUSH_HINT, b, n,
                      ND_BROADCAST_ID) == 0)
         atomic_fetch_add(&c->sent, 1);
      c->last_send_us = now;
   }
}

static void *con_thread(void *p)
{
   console *c = (console *)p;
   while (atomic_load(&c->run))
   {
      con_step(c);
      nd_sleep_us(1000);
   }
   return NULL;
}

/* Test-specific hooks for the NEXT con_open(): installed before the console
 * thread exists, so nothing is ever changed under it. */
static struct
{
   void (*tick)(console *c, uint64_t now);
   int  (*deliver)(console *c, const uint8_t *buf, size_t len, uint8_t src);
   void *user;
} g_hooks;

static void con_hooks(void (*tick)(console *, uint64_t),
                      int (*deliver)(console *, const uint8_t *, size_t, uint8_t),
                      void *user)
{
   g_hooks.tick = tick;
   g_hooks.deliver = deliver;
   g_hooks.user = user;
}

/* Bring a console up on an existing transport.  cfg may be NULL. */
static int con_open(console *c, const char *name, lan_transport *tp,
                    int is_host, const char *proto, uint32_t nonce,
                    const nd_config *base_cfg)
{
   nd_transport iface;
   nd_callbacks cb;
   nd_config cfg;

   memset(c, 0, sizeof(*c));
   c->name = name;
   c->tp = tp;
   c->send_every_ms = 4;
   c->tick = g_hooks.tick;
   c->deliver = g_hooks.deliver;
   c->user = g_hooks.user;
   memset(&g_hooks, 0, sizeof(g_hooks));
   atomic_store(&c->local_id, -1);
   memset(&iface, 0xA5, sizeof(iface));
   lan_transport_iface(tp, &iface);
   memset(&cb, 0, sizeof(cb));
   cb.user = c;
   cb.deliver = con_deliver;
   cb.session_started = con_started;
   cb.peer_connected = con_connected;
   cb.peer_disconnected = con_disconnected;
   cb.session_stopped = con_stopped;
   if (base_cfg)
      cfg = *base_cfg;
   else
      memset(&cfg, 0, sizeof(cfg));
   cfg.protocol = proto;
   cfg.nick = name;
   cfg.join_nonce = nonce;
   c->nd = netdrv_create(&iface, &cb, &cfg);
   if (!c->nd)
      return -1;
   if (is_host ? netdrv_host(c->nd) : netdrv_join(c->nd))
      return -1;
   atomic_store(&c->run, 1);
   if (pthread_create(&c->th, NULL, con_thread, c) != 0)
      return -1;
   c->threaded = 1;
   return 0;
}

/* Stop the thread, leave, free netdrv.  The transport is the caller's. */
static void con_close(console *c)
{
   if (c->threaded)
   {
      atomic_store(&c->run, 0);
      pthread_join(c->th, NULL);
      c->threaded = 0;
   }
   if (c->nd)
   {
      netdrv_leave(c->nd);
      netdrv_destroy(c->nd);
      c->nd = NULL;
   }
}

/* Poll `cond` every millisecond for up to ms milliseconds. */
#define WAIT_FOR(ms, cond) do { \
      uint64_t end_ = nd_now_us() + (uint64_t)(ms) * 1000ull; \
      while (!(cond) && nd_now_us() < end_) \
         nd_sleep_us(1000); \
   } while (0)

static lan_transport *mk_host(uint16_t disc_port, int port_tries)
{
   lan_config cfg;
   memset(&cfg, 0, sizeof(cfg));
   cfg.mode = LAN_MODE_HOST;
   cfg.disc_port = disc_port;
   cfg.port_tries = port_tries;
   cfg.flags = g_net_flags;
   return lan_transport_create(&cfg);
}

static lan_transport *mk_join(uint16_t disc_port, uint32_t ip_be, uint16_t port)
{
   lan_config cfg;
   memset(&cfg, 0, sizeof(cfg));
   cfg.mode = LAN_MODE_JOIN;
   cfg.disc_port = disc_port;
   cfg.target_ip_be = ip_be;
   cfg.target_port = port;
   cfg.flags = g_net_flags;
   return lan_transport_create(&cfg);
}

static void mk_info(lan_room_info *in, const char *name, int number, int kind,
                    const char *nick, const char *title, const char *code,
                    uint32_t compat)
{
   memset(in, 0, sizeof(*in));
   snprintf(in->name, sizeof(in->name), "%s", name);
   in->number = (uint8_t)number;
   in->kind = (uint8_t)kind;
   in->players = 1;
   in->max_players = 2;
   in->flags = LAN_ROOM_F_OPEN;
   snprintf(in->nick, sizeof(in->nick), "%s", nick);
   snprintf(in->title, sizeof(in->title), "%s", title);
   snprintf(in->code, sizeof(in->code), "%s", code);
   in->compat = compat;
}

static void scan_cfg(lan_scan_config *sc, uint16_t disc_port, uint32_t compat)
{
   memset(sc, 0, sizeof(*sc));
   sc->disc_port = disc_port;
   sc->compat = compat;
   sc->flags = g_net_flags;
}

static const lan_room *find_room(const lan_room *r, int n, int number)
{
   int i;
   for (i = 0; i < n; i++)
      if (r[i].info.number == number)
         return &r[i];
   return NULL;
}

/* ================================================================= helpers == */

static void test_helpers(void)
{
   uint32_t ip = 0;
   uint16_t port = 0;
   uint8_t mac[6];
   char buf[32];

   printf("[helpers: ip[:port] parsing, fake MACs, compat hash]\n");
   CHECK(lan_parse_addr("192.168.1.23:19019", &ip, &port, 1) == 0 &&
         ip == inet_addr("192.168.1.23") && port == 19019, "ip:port");
   CHECK(lan_parse_addr("10.0.0.7", &ip, &port, LAN_PORT_DEFAULT) == 0 &&
         ip == inet_addr("10.0.0.7") && port == LAN_PORT_DEFAULT, "default port");
   CHECK(lan_parse_addr("  203.0.113.9:7  \n", &ip, &port, 1) == 0 && port == 7,
         "blanks");
   CHECK(lan_parse_addr("1.2.3", &ip, &port, 1) != 0, "three octets");
   CHECK(lan_parse_addr("1.2.3.4.5", &ip, &port, 1) != 0, "five octets");
   CHECK(lan_parse_addr("1.2.3.256", &ip, &port, 1) != 0, "octet range");
   CHECK(lan_parse_addr("1.2.3.4:", &ip, &port, 1) != 0, "empty port");
   CHECK(lan_parse_addr("1.2.3.4:0", &ip, &port, 1) != 0, "port 0");
   CHECK(lan_parse_addr("1.2.3.4:65536", &ip, &port, 1) != 0, "port range");
   CHECK(lan_parse_addr("1.2.3.4:80x", &ip, &port, 1) != 0, "trailing text");
   CHECK(lan_parse_addr("host.example", &ip, &port, 1) != 0, "a name");
   CHECK(lan_parse_addr("", &ip, &port, 1) != 0, "empty");
   CHECK(lan_parse_addr(NULL, &ip, &port, 1) != 0, "NULL");

   CHECK(!strcmp(lan_format_addr(inet_addr("192.168.1.23"), 19019, buf,
                                 sizeof(buf)), "192.168.1.23:19019"),
         "format: %s", buf);
   lan_mac_from_addr(inet_addr("192.168.1.23"), 0x4A4B, mac);
   CHECK(mac[0] == 192 && mac[1] == 168 && mac[2] == 1 && mac[3] == 23 &&
         mac[4] == 0x4A && mac[5] == 0x4B, "fake MAC layout = transport_udp's");
   lan_addr_from_mac(mac, &ip, &port);
   CHECK(ip == inet_addr("192.168.1.23") && port == 0x4A4B, "MAC round trip");

   CHECK(lan_compat_hash(PROTO_A, "3.5.0") == lan_compat_hash(PROTO_A, "3.5.0"),
         "hash is stable");
   CHECK(lan_compat_hash(PROTO_A, "3.5.0") != lan_compat_hash(PROTO_B, "3.5.0"),
         "hash separates protocols");
   CHECK(lan_compat_hash(PROTO_A, "3.5.0") != lan_compat_hash(PROTO_A, "3.5.1"),
         "hash separates builds");
   CHECK(lan_compat_hash("ab", "c") != lan_compat_hash("a", "bc"),
         "hash separates fields");
   CHECK(lan_compat_hash(NULL, NULL) != 0, "hash is never 0");
}

/* =============================================================== discovery == */

static void test_discovery(void)
{
   uint16_t base = test_port(1);
   uint32_t mine = lan_compat_hash(PROTO_A, "3.5.0");
   uint32_t other = lan_compat_hash(PROTO_A, "9.9.9");
   lan_transport *h[3];
   lan_room_info in[3];
   lan_room rooms[LAN_SCAN_MAX];
   lan_scan_config sc;
   lan_scan *scan;
   const lan_room *r;
   char want[32];
   int i, n = 0;

   printf("[discovery: three rooms on one LAN, two on fallback ports]\n");
   mk_info(&in[0], "GC07AMKE", 7, LAN_KIND_GBA_CABLE, "Alex", "MARIOKART",
           "AMKE", mine);
   mk_info(&in[1], "GB03POKE", 3, LAN_KIND_GB_CABLE, "Nickname of 15!",
           "POKEMON RED", "POKE", mine);
   mk_info(&in[2], "GPSP12", 12, LAN_KIND_GBA_WIRELESS, "other build",
           "A title of nineteen", "BPEE", other);
   in[2].max_players = 5;
   for (i = 0; i < 3; i++)
   {
      h[i] = mk_host(base, 0);
      CHECK(h[i] != NULL, "host %d: %s", i, lan_last_error());
      if (!h[i])
         return;
      CHECK(lan_transport_port(h[i]) == base + i,
            "host %d bound %u, want %u (fallback)", i,
            lan_transport_port(h[i]), base + i);
      CHECK(lan_transport_announce(h[i], &in[i]) == 0, "announce");
   }

   scan_cfg(&sc, base, mine);
   n = lan_discovery_scan(&sc, rooms, LAN_SCAN_MAX, 700);
   CHECK(n == 3, "found %d rooms, want 3", n);
   for (i = 1; i < n; i++)
      CHECK(rooms[i - 1].info.number <= rooms[i].info.number, "sorted by number");

   r = find_room(rooms, n, 7);
   CHECK(r != NULL, "room 7");
   if (r)
   {
      snprintf(want, sizeof(want), "127.0.0.1:%u", base);
      CHECK(!strcmp(r->info.name, "GC07AMKE") && !strcmp(r->info.nick, "Alex") &&
            !strcmp(r->info.title, "MARIOKART") && !strcmp(r->info.code, "AMKE"),
            "room 7 text: '%s' '%s' '%s' '%s'", r->info.name, r->info.nick,
            r->info.title, r->info.code);
      CHECK(r->info.kind == LAN_KIND_GBA_CABLE && r->info.players == 1 &&
            r->info.max_players == 2 && (r->info.flags & LAN_ROOM_F_OPEN),
            "room 7 numbers");
      CHECK(r->port == base && r->announced_port == base &&
            r->ip_be == inet_addr("127.0.0.1") && !strcmp(r->addr, want),
            "room 7 address %s", r->addr);
      CHECK(r->compatible == 1, "same build is compatible");
      CHECK(r->rtt_ms >= 0 && r->rtt_ms < 200, "round trip %d ms", r->rtt_ms);
      CHECK(r->age_ms < 700, "age %u ms", r->age_ms);
      CHECK(r->mac[0] == 127 && r->mac[3] == 1 &&
            ((r->mac[4] << 8) | r->mac[5]) == base, "fake MAC");
   }
   r = find_room(rooms, n, 3);
   CHECK(r && r->port == base + 1 && r->info.kind == LAN_KIND_GB_CABLE &&
         !strcmp(r->info.nick, "Nickname of 15!") &&
         !strcmp(r->info.title, "POKEMON RED"), "room 3 on the fallback port");
   r = find_room(rooms, n, 12);
   CHECK(r && r->port == base + 2 && r->compatible == 0 &&
         r->info.max_players == 5 && r->info.kind == LAN_KIND_GBA_WIRELESS &&
         !strcmp(r->info.title, "A title of nineteen"),
         "room 12: another build is listed, and marked incompatible");

   /* a live scan object: updates, GONE, expiry */
   scan = lan_scan_open(&sc);
   CHECK(scan != NULL, "scan open: %s", lan_last_error());
   if (scan)
   {
      in[0].players = 2;
      in[0].flags = LAN_ROOM_F_BUSY;
      lan_transport_announce(h[0], &in[0]);
      n = 0;
      WAIT_FOR(1500, (lan_scan_poll(scan),
                      (n = lan_scan_rooms(scan, rooms, LAN_SCAN_MAX)) == 3 &&
                      (r = find_room(rooms, n, 7)) && r->info.players == 2));
      r = find_room(rooms, n, 7);
      CHECK(n == 3 && r && r->info.players == 2 &&
            (r->info.flags & LAN_ROOM_F_BUSY) && !(r->info.flags & LAN_ROOM_F_OPEN),
            "player count and flags follow the host (n=%d)", n);

      /* a host that closes says so: the room leaves the list at once
       * (this scanner holds the beacon port in this process) */
      lan_transport_destroy(h[1]);
      h[1] = NULL;
      WAIT_FOR(1000, (lan_scan_poll(scan),
                      lan_scan_rooms(scan, rooms, LAN_SCAN_MAX) == 2));
      n = lan_scan_rooms(scan, rooms, LAN_SCAN_MAX);
      CHECK(n == 2 && !find_room(rooms, n, 3), "closed room is gone (n=%d)", n);

      /* a host that only stops announcing times out (3.5 s) */
      lan_transport_announce(h[2], NULL);
      WAIT_FOR(5000, (lan_scan_poll(scan),
                      lan_scan_rooms(scan, rooms, LAN_SCAN_MAX) == 1));
      n = lan_scan_rooms(scan, rooms, LAN_SCAN_MAX);
      CHECK(n == 1 && find_room(rooms, n, 7), "silent room expires (n=%d)", n);
      lan_scan_close(scan);
   }
   {
      lan_stats st;
      lan_transport_get_stats(h[0], &st);
      CHECK(st.disc_probes_rx > 0 && st.disc_replies_tx > 0 &&
            st.disc_beacons_tx > 0, "host answered probes and beaconed");
      CHECK(st.rx_pkts == 0, "no discovery datagram was queued for netdrv "
            "(rx_pkts=%u)", st.rx_pkts);
      CHECK(st.io_poll_err == 0, "the idle poll never misfired (%u)",
            st.io_poll_err);
      /* an idle host sleeps: one wake per beacon/probe, not a busy loop */
      CHECK(st.io_wakeups < 2000, "I/O thread wakeups while idle: %u",
            st.io_wakeups);
      printf("  host0: probes=%u replies=%u beacons=%u limited=%u\n",
             st.disc_probes_rx, st.disc_replies_tx, st.disc_beacons_tx,
             st.disc_limited);
   }
   for (i = 0; i < 3; i++)
      lan_transport_destroy(h[i]);

   /* an empty LAN is an empty list, not an error */
   n = lan_discovery_scan(&sc, rooms, LAN_SCAN_MAX, 300);
   CHECK(n == 0, "nobody hosting: %d rooms", n);
}

/* ================================================================ portbusy == */

static void test_portbusy(void)
{
   uint16_t base = test_port(2);
   struct sockaddr_in a;
   lan_transport *h;
   lan_room_info in;
   lan_room rooms[4];
   lan_scan_config sc;
   int squat, n;

   printf("[port busy: fallback to the next port; refusal when told not to]\n");
   squat = socket(AF_INET, SOCK_DGRAM, 0);
   memset(&a, 0, sizeof(a));
   a.sin_family = AF_INET;
   a.sin_addr.s_addr = htonl(INADDR_ANY);
   a.sin_port = htons(base);
   CHECK(bind(squat, (struct sockaddr *)&a, sizeof(a)) == 0, "squatter binds");

   h = mk_host(base, 1);
   CHECK(h == NULL, "port_tries=1 with the port taken must fail");
   CHECK(!strcmp(lan_last_error(), "no free port"), "says why: '%s'",
         lan_last_error());
   if (h)
      lan_transport_destroy(h);

   h = mk_host(base, 0);
   CHECK(h != NULL, "fallback host: %s", lan_last_error());
   if (h)
   {
      char s[32];
      CHECK(lan_transport_port(h) == base + 1, "fell back to %u (want %u)",
            lan_transport_port(h), base + 1);
      lan_transport_addr_str(h, s, sizeof(s));
      printf("  host shows %s\n", s);
      mk_info(&in, "GB01TETR", 1, LAN_KIND_GB_CABLE, "host", "TETRIS", "TETR", 5);
      lan_transport_announce(h, &in);
      scan_cfg(&sc, base, 5);
      n = lan_discovery_scan(&sc, rooms, 4, 600);
      CHECK(n == 1 && rooms[0].port == base + 1,
            "the scan finds it on the fallback port (n=%d)", n);
      lan_transport_destroy(h);
   }
   close(squat);
}

/* ================================================================== beacon == */

static void test_beacon(void)
{
   uint16_t base = test_port(3);
   struct sockaddr_in a, from;
   socklen_t flen = sizeof(from);
   lan_transport *h;
   lan_room_info in;
   uint8_t buf[512];
   struct pollfd p;
   ssize_t n = 0;
   int fd, on = 1;

   printf("[beacon: a room is announced unasked on the beacon port]\n");
   fd = socket(AF_INET, SOCK_DGRAM, 0);
   setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
   memset(&a, 0, sizeof(a));
   a.sin_family = AF_INET;
   a.sin_addr.s_addr = htonl(INADDR_ANY);
   a.sin_port = htons((uint16_t)(base - 1));
   CHECK(bind(fd, (struct sockaddr *)&a, sizeof(a)) == 0, "listener binds");

   h = mk_host(base, 0);
   CHECK(h != NULL, "host");
   if (!h)
   {
      close(fd);
      return;
   }
   mk_info(&in, "GC09BEAC", 9, LAN_KIND_GBA_CABLE, "b", "BEACON", "BEAC", 77);
   lan_transport_announce(h, &in);
   p.fd = fd;
   p.events = POLLIN;
   if (poll(&p, 1, 1500) > 0)
      n = recvfrom(fd, buf, sizeof(buf), 0, (struct sockaddr *)&from, &flen);
   CHECK(n == 86, "beacon length %d, want 86", (int)n);
   if (n == 86)
   {
      CHECK(!memcmp(buf, "GBLD", 4) && buf[4] == LAN_DISC_VERSION && buf[5] == 2,
            "magic, version, type");
      CHECK(buf[8] == 0 && buf[9] == 0 && buf[10] == 0 && buf[11] == 0,
            "a beacon echoes no probe");
      CHECK(ntohs(from.sin_port) == base,
            "sent FROM the game port, so the source is the join address");
      CHECK(nd_crc16(0xFFFF, buf, 84) == (uint16_t)(buf[84] | (buf[85] << 8)),
            "CRC");
      CHECK(!memcmp(buf + 28, "GC09BEAC", 8) && buf[26] == 9, "room fields");
   }
   lan_transport_destroy(h);
   /* the farewell */
   n = 0;
   if (poll(&p, 1, 500) > 0)
   {
      n = recvfrom(fd, buf, sizeof(buf), 0, NULL, NULL);
      while (n == 86 && poll(&p, 1, 200) > 0)   /* a beacon still in flight */
         n = recvfrom(fd, buf, sizeof(buf), 0, NULL, NULL);
   }
   CHECK(n == 22 && buf[5] == 3, "GONE on destroy (len %d)", (int)n);
   close(fd);
}

/* ================================================================= session == */

static void session_report(const char *who, console *c)
{
   nd_stats ns;
   lan_stats ls;
   netdrv_get_stats(c->nd, &ns);
   lan_transport_get_stats(c->tp, &ls);
   printf("  %-5s id=%d tx=%u rx=%u retx=%u dup=%u acked=%u srtt=%ums rto=%ums"
          " | lan tx=%u rx=%u ringdrop=%u foreign=%u wb=%u fault drop/delay="
          "%u/%u\n",
          who, netdrv_local_id(c->nd), ns.tx_frames, ns.rx_frames, ns.retx,
          ns.rx_dup, ns.acked, ns.srtt_us / 1000, ns.rto_us / 1000,
          ls.tx_pkts, ls.rx_pkts, ls.rx_ring_drop, ls.rx_foreign,
          ls.tx_would_block, ls.fault_dropped, ls.fault_delayed);
}

static void test_session(void)
{
   enum { MSGS = 300 };
   uint16_t base = test_port(4);
   uint32_t compat = lan_compat_hash(PROTO_A, "test");
   lan_transport *th, *t1, *t2;
   console H, C1, C2;
   lan_room_info in;
   lan_room rooms[4];
   lan_scan_config sc;
   nd_transport probe;
   uint32_t ip;
   uint16_t port;
   char typed[32];
   int n, i;

   printf("[session: host + 2 joiners, a thread each; 5%% loss, 40 ms latency,"
          " 30 ms jitter]\n");
   th = mk_host(base, 0);
   CHECK(th != NULL, "host transport: %s", lan_last_error());
   if (!th)
      return;
   memset(&probe, 0xA5, sizeof(probe));
   lan_transport_iface(th, &probe);
   CHECK(probe.pending != NULL && probe.send_to && probe.broadcast &&
         probe.recv && probe.local_addr, "full vtable incl. pending");
   CHECK(probe.pending(probe.ctx) == 0, "pending is 0 on an idle transport");
   {
      uint8_t mac[6];
      probe.local_addr(probe.ctx, mac);
      CHECK(mac[0] == 127 && mac[3] == 1 && ((mac[4] << 8) | mac[5]) == base,
            "local_addr is ip:port");
   }
   mk_info(&in, "GB05LINK", 5, LAN_KIND_GB_CABLE, "host", "LINK TEST", "LINK",
           compat);
   lan_transport_announce(th, &in);
   CHECK(con_open(&H, "host", th, 1, PROTO_A, 0x1001, NULL) == 0, "host console");

   /* joiner 1: picks the room from a scan */
   scan_cfg(&sc, base, compat);
   n = lan_discovery_scan(&sc, rooms, 4, 600);
   CHECK(n == 1 && rooms[0].compatible, "scan finds the room (n=%d)", n);
   if (n != 1)
   {
      con_close(&H);
      lan_transport_destroy(th);
      return;
   }
   t1 = mk_join(base, rooms[0].ip_be, rooms[0].port);
   /* joiner 2: the player typed what the host's screen shows */
   lan_transport_addr_str(th, typed, sizeof(typed));
   CHECK(lan_parse_addr(typed, &ip, &port, LAN_PORT_DEFAULT) == 0, "typed %s",
         typed);
   t2 = mk_join(base, ip, port);
   CHECK(t1 && t2, "join transports: %s", lan_last_error());
   if (!t1 || !t2)
      return;

   lan_transport_set_fault(th, 5, 0, 40, 30, 0x1111);
   lan_transport_set_fault(t1, 5, 0, 40, 30, 0x2222);
   lan_transport_set_fault(t2, 5, 0, 40, 30, 0x3333);

   CHECK(con_open(&C1, "join1", t1, 0, PROTO_A, 0x2002, NULL) == 0, "console 1");
   CHECK(con_open(&C2, "join2", t2, 0, PROTO_A, 0x3003, NULL) == 0, "console 2");

   WAIT_FOR(10000, atomic_load(&H.peers) == 2 && atomic_load(&C1.peers) == 2 &&
                   atomic_load(&C2.peers) == 2 && atomic_load(&C1.active) &&
                   atomic_load(&C2.active));
   CHECK(atomic_load(&H.peers) == 2 && atomic_load(&C1.peers) == 2 &&
         atomic_load(&C2.peers) == 2, "mesh of three (%d/%d/%d peers)",
         atomic_load(&H.peers), atomic_load(&C1.peers), atomic_load(&C2.peers));
   CHECK(atomic_load(&H.local_id) == 0 && atomic_load(&C1.local_id) > 0 &&
         atomic_load(&C2.local_id) > 0 &&
         atomic_load(&C1.local_id) != atomic_load(&C2.local_id), "ids");

   H.to_send = C1.to_send = C2.to_send = MSGS;
   atomic_store(&H.go, 1);
   atomic_store(&C1.go, 1);
   atomic_store(&C2.go, 1);
#define ALL_IN(c, a, b) (atomic_load(&(c).rx_ok[a]) == MSGS && \
                         atomic_load(&(c).rx_ok[b]) == MSGS)
   {
      int i1 = atomic_load(&C1.local_id), i2 = atomic_load(&C2.local_id);
      if (i1 < 0 || i2 < 0)
         i1 = 1, i2 = 2;
      WAIT_FOR(40000, ALL_IN(H, i1, i2) && ALL_IN(C1, 0, i2) && ALL_IN(C2, 0, i1));
      CHECK(ALL_IN(H, i1, i2), "host got %u+%u of %d+%d",
            atomic_load(&H.rx_ok[i1]), atomic_load(&H.rx_ok[i2]), MSGS, MSGS);
      CHECK(ALL_IN(C1, 0, i2), "join1 got %u (host) + %u (join2)",
            atomic_load(&C1.rx_ok[0]), atomic_load(&C1.rx_ok[i2]));
      CHECK(ALL_IN(C2, 0, i1), "join2 got %u (host) + %u (join1)",
            atomic_load(&C2.rx_ok[0]), atomic_load(&C2.rx_ok[i1]));
   }
#undef ALL_IN
   CHECK(atomic_load(&H.rx_bad) == 0 && atomic_load(&C1.rx_bad) == 0 &&
         atomic_load(&C2.rx_bad) == 0, "nothing damaged or out of order");

   /* stop the pump threads before reading netdrv from this one */
   atomic_store(&H.run, 0); atomic_store(&C1.run, 0); atomic_store(&C2.run, 0);
   pthread_join(H.th, NULL); pthread_join(C1.th, NULL); pthread_join(C2.th, NULL);
   H.threaded = C1.threaded = C2.threaded = 0;
   session_report("host", &H);
   session_report("join1", &C1);
   session_report("join2", &C2);
   for (i = 0; i < 3; i++)
   {
      console *c = i == 0 ? &H : i == 1 ? &C1 : &C2;
      nd_stats ns;
      lan_stats ls;
      netdrv_get_stats(c->nd, &ns);
      lan_transport_get_stats(c->tp, &ls);
      CHECK(ns.tx_overflow == 0 && ns.rx_drop_crc == 0 && ns.rx_beyond_win == 0,
            "%s: overflow=%u crc=%u beyond=%u", c->name, ns.tx_overflow,
            ns.rx_drop_crc, ns.rx_beyond_win);
      CHECK(ns.retx > 0 && ls.fault_dropped > 0 && ls.fault_delayed > 0,
            "%s: the shim really dropped and delayed", c->name);
      CHECK(ls.rx_ring_drop == 0, "%s: RX ring never overflowed", c->name);
      CHECK(ls.nat_aliases == 0, "%s: no alias on a LAN", c->name);
      CHECK(ls.io_poll_err == 0 && ls.rx_err == 0 && ls.tx_fail == 0,
            "%s: poll/recv/send errors %u/%u/%u", c->name, ls.io_poll_err,
            ls.rx_err, ls.tx_fail);
   }
   con_close(&C1);
   con_close(&C2);
   con_close(&H);
   lan_transport_destroy(t1);
   lan_transport_destroy(t2);
   lan_transport_destroy(th);
}

/* =============================================================== bcastjoin == */

static void test_bcastjoin(void)
{
   uint16_t base = test_port(5);
   lan_transport *ta, *tb, *tj;
   console A, B, J;

   printf("[broadcast JOIN: two hosts, one speaks our protocol]\n");
   ta = mk_host(base, 0);                 /* base     : another protocol */
   tb = mk_host(base, 0);                 /* base + 1 : ours */
   tj = mk_join(base, 0, 0);              /* no target: broadcast */
   CHECK(ta && tb && tj, "transports: %s", lan_last_error());
   if (!ta || !tb || !tj)
      return;
   CHECK(con_open(&A, "hostA", ta, 1, PROTO_B, 0xA, NULL) == 0, "A");
   CHECK(con_open(&B, "hostB", tb, 1, PROTO_A, 0xB, NULL) == 0, "B");
   CHECK(con_open(&J, "join", tj, 0, PROTO_A, 0xC, NULL) == 0, "J");

   WAIT_FOR(6000, atomic_load(&J.active) && atomic_load(&B.peers) == 1);
   CHECK(atomic_load(&J.active) && atomic_load(&J.local_id) == 1,
         "joined by broadcast");
   CHECK(atomic_load(&B.peers) == 1 && atomic_load(&A.peers) == 0,
         "in the room that speaks the protocol (A=%d B=%d)",
         atomic_load(&A.peers), atomic_load(&B.peers));
   CHECK(atomic_load(&B.connects) == 1,
         "admitted once: every broadcast path gives the same source "
         "(connects=%d)", atomic_load(&B.connects));

   J.to_send = B.to_send = 50;
   atomic_store(&J.go, 1);
   atomic_store(&B.go, 1);
   WAIT_FOR(8000, atomic_load(&J.rx_ok[0]) == 50 && atomic_load(&B.rx_ok[1]) == 50);
   CHECK(atomic_load(&J.rx_ok[0]) == 50 && atomic_load(&B.rx_ok[1]) == 50,
         "data both ways (%u/%u)", atomic_load(&J.rx_ok[0]),
         atomic_load(&B.rx_ok[1]));
   con_close(&J); con_close(&A); con_close(&B);
   lan_transport_destroy(tj);
   lan_transport_destroy(ta);
   lan_transport_destroy(tb);
}

/* ===================================================================== nat == */

/* A "router": the joiner talks to socket X, the host sees everything come
 * from socket Y, and neither ever sees the other's real address.  Payloads
 * pass untouched, as through a NAT with a forwarded port. */
typedef struct relay
{
   int x, y;
   uint16_t xport, yport;
   struct sockaddr_in host, joiner;
   atomic_int have_joiner;
   atomic_int run;
   atomic_uint fwd;
   pthread_t th;
} relay;

static void *relay_thread(void *arg)
{
   relay *r = (relay *)arg;
   uint8_t buf[2048];
   while (atomic_load(&r->run))
   {
      struct pollfd p[2];
      int k;
      p[0].fd = r->x; p[0].events = POLLIN; p[0].revents = 0;
      p[1].fd = r->y; p[1].events = POLLIN; p[1].revents = 0;
      if (poll(p, 2, 20) <= 0)
         continue;
      for (k = 0; k < 2; k++)
      {
         struct sockaddr_in from;
         socklen_t flen = sizeof(from);
         ssize_t n;
         if (!(p[k].revents & POLLIN))
            continue;
         n = recvfrom(p[k].fd, buf, sizeof(buf), MSG_DONTWAIT,
                      (struct sockaddr *)&from, &flen);
         if (n <= 0)
            continue;
         if (k == 0)
         {
            r->joiner = from;            /* the "NAT mapping" */
            atomic_store(&r->have_joiner, 1);
            sendto(r->y, buf, (size_t)n, 0, (struct sockaddr *)&r->host,
                   sizeof(r->host));
         }
         else if (atomic_load(&r->have_joiner))
            sendto(r->x, buf, (size_t)n, 0, (struct sockaddr *)&r->joiner,
                   sizeof(r->joiner));
         atomic_fetch_add(&r->fwd, 1);
      }
   }
   return NULL;
}

static int relay_open(relay *r, uint16_t host_port)
{
   struct sockaddr_in a;
   socklen_t alen = sizeof(a);
   memset(r, 0, sizeof(*r));
   r->x = socket(AF_INET, SOCK_DGRAM, 0);
   r->y = socket(AF_INET, SOCK_DGRAM, 0);
   memset(&a, 0, sizeof(a));
   a.sin_family = AF_INET;
   a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
   if (bind(r->x, (struct sockaddr *)&a, sizeof(a)) ||
       bind(r->y, (struct sockaddr *)&a, sizeof(a)))
      return -1;
   getsockname(r->x, (struct sockaddr *)&a, &alen);
   r->xport = ntohs(a.sin_port);
   getsockname(r->y, (struct sockaddr *)&a, &alen);
   r->yport = ntohs(a.sin_port);
   memset(&r->host, 0, sizeof(r->host));
   r->host.sin_family = AF_INET;
   r->host.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
   r->host.sin_port = htons(host_port);
   atomic_store(&r->run, 1);
   return pthread_create(&r->th, NULL, relay_thread, r);
}

static void relay_close(relay *r)
{
   atomic_store(&r->run, 0);
   pthread_join(r->th, NULL);
   close(r->x);
   close(r->y);
}

/* the bulk lane, exercised from the console threads */
typedef struct bulk_ping
{
   int send;                    /* this side sends */
   unsigned sent, got, bad;
   atomic_uint got_pub;
} bulk_ping;

static void bulk_ping_tick(console *c, uint64_t now)
{
   bulk_ping *b = (bulk_ping *)c->user;
   uint8_t buf[LAN_BULK_MAX];
   int n;
   (void)now;
   while ((n = lan_transport_bulk_recv(c->tp, buf, sizeof(buf))) > 0)
   {
      if (n == 900 && !memcmp(buf, "GBLK", 4) && buf[4] == (uint8_t)b->got &&
          buf[899] == (uint8_t)(b->got * 3))
         b->got++;
      else
         b->bad++;
      atomic_store(&b->got_pub, b->got);
   }
   if (b->send && b->sent < 40 && netdrv_active(c->nd) &&
       netdrv_peer_count(c->nd) == 1)
   {
      uint8_t mac[6];
      uint16_t peer = netdrv_local_id(c->nd) == 0 ? 1 : 0;
      if (netdrv_peer_mac(c->nd, peer, mac) == 0)
      {
         memset(buf, 0x5A, 900);
         memcpy(buf, "GBLK", 4);
         buf[4] = (uint8_t)b->sent;
         buf[899] = (uint8_t)(b->sent * 3);
         if (lan_transport_bulk_send(c->tp, mac, buf, 900) == 0)
            b->sent++;
      }
   }
}

static void test_nat(void)
{
   uint16_t base = test_port(6);
   lan_transport *th, *tj;
   console H, J;
   relay r;
   bulk_ping bh, bj;
   lan_stats sh, sj;
   uint8_t mac[6];

   printf("[nat: joiner and host only meet through an address-rewriting "
          "relay]\n");
   th = mk_host(base, 0);
   CHECK(th != NULL, "host: %s", lan_last_error());
   if (!th)
      return;
   CHECK(relay_open(&r, lan_transport_port(th)) == 0, "relay");
   /* the joiner types the ROUTER's address, never the host's own */
   tj = mk_join(base, inet_addr("127.0.0.1"), r.xport);
   CHECK(tj != NULL, "join: %s", lan_last_error());
   if (!tj)
      return;
   memset(&bh, 0, sizeof(bh));
   memset(&bj, 0, sizeof(bj));
   bh.send = bj.send = 1;
   CHECK(lan_transport_bulk_open(th, 64, "GBLK") == 0 &&
         lan_transport_bulk_open(tj, 64, "GBLK") == 0, "bulk open");
   /* the bulk lane, addressed by the MAC netdrv reports for the partner */
   con_hooks(bulk_ping_tick, NULL, &bh);
   CHECK(con_open(&H, "host", th, 1, PROTO_A, 0x11, NULL) == 0, "H");
   con_hooks(bulk_ping_tick, NULL, &bj);
   CHECK(con_open(&J, "join", tj, 0, PROTO_A, 0x22, NULL) == 0, "J");

   WAIT_FOR(6000, atomic_load(&J.active) && atomic_load(&H.peers) == 1 &&
                  atomic_load(&J.peers) == 1);
   CHECK(atomic_load(&J.active) && atomic_load(&H.peers) == 1,
         "session up through the relay");

   H.to_send = J.to_send = 200;
   atomic_store(&H.go, 1);
   atomic_store(&J.go, 1);
   WAIT_FOR(15000, atomic_load(&J.rx_ok[0]) == 200 &&
                   atomic_load(&H.rx_ok[1]) == 200);
   CHECK(atomic_load(&J.rx_ok[0]) == 200,
         "host -> joiner: %u of 200 (the host answers the address it "
         "received from)", atomic_load(&J.rx_ok[0]));
   CHECK(atomic_load(&H.rx_ok[1]) == 200,
         "joiner -> host: %u of 200 (the joiner sends to where the host IS, "
         "not where the roster says)", atomic_load(&H.rx_ok[1]));
   CHECK(atomic_load(&H.rx_bad) == 0 && atomic_load(&J.rx_bad) == 0, "intact");

   WAIT_FOR(5000, atomic_load(&bh.got_pub) == 40 && atomic_load(&bj.got_pub) == 40);

   atomic_store(&H.run, 0); atomic_store(&J.run, 0);
   pthread_join(H.th, NULL); pthread_join(J.th, NULL);
   H.threaded = J.threaded = 0;
   CHECK(bh.got == 40 && bj.got == 40 && !bh.bad && !bj.bad,
         "bulk lane through the relay: host got %u, joiner got %u", bh.got,
         bj.got);
   lan_transport_get_stats(th, &sh);
   lan_transport_get_stats(tj, &sj);
   CHECK(sj.nat_aliases == 1 && sh.nat_aliases == 0,
         "one host behind a NAT, found by the joiner (%u/%u)", sj.nat_aliases,
         sh.nat_aliases);
   /* what netdrv believes (wire version 2): the host is where its OFFER
    * came from -- the relay's address the joiner typed -- in the roster as
    * well, its declared entry rewritten by the transport */
   CHECK(netdrv_peer_mac(J.nd, 0, mac) == 0 &&
         ((mac[4] << 8) | mac[5]) == r.xport,
         "the joiner's netdrv names the host by the address it reached, "
         "port %u (got %u)", r.xport, (mac[4] << 8) | mac[5]);
   /* and the host knows the joiner only as the relay's outside socket */
   CHECK(netdrv_peer_mac(H.nd, 1, mac) == 0 &&
         ((mac[4] << 8) | mac[5]) == r.yport,
         "the host's roster holds the joiner's MAPPED port %u (got %u)",
         r.yport, (mac[4] << 8) | mac[5]);
   {
      nd_stats ns;
      netdrv_get_stats(J.nd, &ns);
      CHECK(ns.rx_drop_unknown_peer == 0,
            "no frame was dropped as coming from a stranger (%u)",
            ns.rx_drop_unknown_peer);
   }
   printf("  relay forwarded %u datagrams; joiner aliases=%u\n",
          atomic_load(&r.fwd), sj.nat_aliases);
   con_close(&J);
   con_close(&H);
   lan_transport_destroy(tj);
   lan_transport_destroy(th);
   relay_close(&r);
}

/* ==================================================================== bulk == */

#define BLOB_BYTES  (1024u * 1024u)
#define CHUNK       1024u
#define CHUNKS      (BLOB_BYTES / CHUNK)
#define BULK_HDR    8u                   /* "GBLK" + u32 index */

typedef struct bulk_xfer
{
   int      sender;
   uint8_t *blob;                        /* sender: source; receiver: dest */
   uint8_t  have[CHUNKS / 8];            /* receiver: got it; sender: acked */
   uint64_t sent_at[CHUNKS];             /* sender */
   unsigned cursor, n_have, tx, resent, rx, dup, bad, reports;
   uint64_t last_report_us;
   atomic_uint done;                     /* receiver: all chunks in */
   atomic_uint acked_all;                /* sender: receiver said so */
} bulk_xfer;

static uint8_t blob_byte(uint32_t i)
{
   uint32_t x = i * 2654435761u;
   return (uint8_t)(x ^ (x >> 13) ^ (x >> 21));
}

static void bulk_tick(console *c, uint64_t now)
{
   bulk_xfer *x = (bulk_xfer *)c->user;
   uint8_t buf[LAN_BULK_MAX];
   int n;

   if (!netdrv_active(c->nd) || netdrv_peer_count(c->nd) != 1)
      return;

   if (x->sender)
   {
      uint8_t mac[6];
      unsigned budget = 3, scanned = 0;  /* 3 datagrams per ms */
      if (netdrv_peer_mac(c->nd, netdrv_local_id(c->nd) == 0 ? 1 : 0, mac))
         return;
      while (budget && scanned < CHUNKS && x->n_have < CHUNKS)
      {
         unsigned i = x->cursor;
         x->cursor = (x->cursor + 1) % CHUNKS;
         scanned++;
         if (x->have[i / 8] & (1u << (i % 8)))
            continue;
         /* not acknowledged: (re)send, but give the last copy and the
          * report that would acknowledge it time to cross the link */
         if (x->sent_at[i] && now - x->sent_at[i] < 400000u)
            continue;
         memcpy(buf, "GBLK", 4);
         buf[4] = (uint8_t)i; buf[5] = (uint8_t)(i >> 8);
         buf[6] = (uint8_t)(i >> 16); buf[7] = (uint8_t)(i >> 24);
         memcpy(buf + BULK_HDR, x->blob + i * CHUNK, CHUNK);
         lan_transport_bulk_send(c->tp, mac, buf, BULK_HDR + CHUNK);
         if (x->sent_at[i])
            x->resent++;
         x->sent_at[i] = now;
         x->tx++;
         budget--;
      }
      return;
   }

   while ((n = lan_transport_bulk_recv(c->tp, buf, sizeof(buf))) > 0)
   {
      uint32_t i;
      if (n != (int)(BULK_HDR + CHUNK) || memcmp(buf, "GBLK", 4))
      {
         x->bad++;
         continue;
      }
      i = (uint32_t)buf[4] | ((uint32_t)buf[5] << 8) |
          ((uint32_t)buf[6] << 16) | ((uint32_t)buf[7] << 24);
      if (i >= CHUNKS)
      {
         x->bad++;
         continue;
      }
      x->rx++;
      if (x->have[i / 8] & (1u << (i % 8)))
      {
         x->dup++;
         continue;
      }
      memcpy(x->blob + i * CHUNK, buf + BULK_HDR, CHUNK);
      x->have[i / 8] |= (uint8_t)(1u << (i % 8));
      x->n_have++;
   }
   /* the session layer's NACK, in miniature: every 100 ms the whole bitmap
    * goes back over the ORDERED channel */
   if (now - x->last_report_us >= 100000u &&
       netdrv_send_capacity(c->nd, 0) > 8)
   {
      uint8_t rep[4 + CHUNKS / 8];
      memcpy(rep, "BMAP", 4);
      memcpy(rep + 4, x->have, CHUNKS / 8);
      if (netdrv_send(c->nd, ND_RELIABLE | ND_FLUSH_HINT, rep, sizeof(rep),
                      0) == 0)
         x->reports++;
      x->last_report_us = now;
   }
   if (x->n_have == CHUNKS)
      atomic_store(&x->done, 1);
}

static int bulk_deliver(console *c, const uint8_t *buf, size_t len, uint8_t src)
{
   bulk_xfer *x = (bulk_xfer *)c->user;
   unsigned i, n = 0;
   (void)src;
   if (!x || !x->sender || len != 4 + CHUNKS / 8 || memcmp(buf, "BMAP", 4))
      return 0;
   memcpy(x->have, buf + 4, CHUNKS / 8);
   for (i = 0; i < CHUNKS; i++)
      if (x->have[i / 8] & (1u << (i % 8)))
         n++;
   x->n_have = n;
   if (n == CHUNKS)
      atomic_store(&x->acked_all, 1);
   return 1;
}

static void test_bulk(void)
{
   uint16_t base = test_port(7);
   lan_transport *th, *tj;
   console H, J;
   bulk_xfer *xs, *xr;
   uint64_t t0, t1;
   uint32_t i;
   nd_stats nh, nj;
   lan_stats sh, sj;

   printf("[bulk lane: 1 MiB, 5%% loss both ways, 20 ms latency, 10 ms jitter]\n");
   CHECK(sizeof(((bulk_xfer *)0)->have) + 4 <= ND_MAX_PAYLOAD,
         "the bitmap report fits one netdrv payload");
   xs = (bulk_xfer *)calloc(1, sizeof(*xs));
   xr = (bulk_xfer *)calloc(1, sizeof(*xr));
   xs->blob = (uint8_t *)malloc(BLOB_BYTES);
   xr->blob = (uint8_t *)calloc(1, BLOB_BYTES);
   xs->sender = 1;
   for (i = 0; i < BLOB_BYTES; i++)
      xs->blob[i] = blob_byte(i);

   th = mk_host(base, 0);
   tj = mk_join(base, inet_addr("127.0.0.1"), base);
   CHECK(th && tj, "transports: %s", lan_last_error());
   if (!th || !tj)
      return;
   CHECK(lan_transport_bulk_send(th, (const uint8_t *)"\x7f\0\0\1\0\1", "GBLK",
                                 LAN_BULK_MAX + 1) < 0,
         "an over-long bulk datagram is refused");
   CHECK(lan_transport_bulk_open(th, 128, "GBLK") == 0 &&
         lan_transport_bulk_open(tj, 128, "GBLK") == 0, "bulk open");
   lan_transport_set_fault(th, 5, 0, 20, 10, 0xB001);
   lan_transport_set_fault(tj, 5, 0, 20, 10, 0xB002);
   /* the host sends its "cartridge" to the joiner as soon as it is there */
   t0 = nd_now_us();
   con_hooks(bulk_tick, bulk_deliver, xs);
   CHECK(con_open(&H, "host", th, 1, PROTO_A, 0x71, NULL) == 0, "H");
   con_hooks(bulk_tick, bulk_deliver, xr);
   CHECK(con_open(&J, "join", tj, 0, PROTO_A, 0x72, NULL) == 0, "J");
   WAIT_FOR(40000, atomic_load(&xr->done) && atomic_load(&xs->acked_all));
   t1 = nd_now_us();

   atomic_store(&H.run, 0); atomic_store(&J.run, 0);
   pthread_join(H.th, NULL); pthread_join(J.th, NULL);
   H.threaded = J.threaded = 0;

   CHECK(atomic_load(&xr->done), "all %u chunks arrived (%u)", CHUNKS, xr->n_have);
   CHECK(atomic_load(&xs->acked_all), "and the sender knows it");
   CHECK(!memcmp(xs->blob, xr->blob, BLOB_BYTES), "the megabyte is identical");
   CHECK(xr->bad == 0, "no malformed bulk datagram (%u)", xr->bad);
   netdrv_get_stats(H.nd, &nh);
   netdrv_get_stats(J.nd, &nj);
   lan_transport_get_stats(th, &sh);
   lan_transport_get_stats(tj, &sj);
   CHECK(sj.bulk_rx >= CHUNKS && sj.bulk_rx == xr->rx + sj.bulk_foreign,
         "every bulk datagram went to the bulk ring (%u in, %u read)",
         sj.bulk_rx, xr->rx);
   CHECK(nj.rx_drop_malformed == 0 && nh.rx_drop_malformed == 0 &&
         nj.rx_drop_crc == 0 && nh.rx_drop_crc == 0,
         "netdrv saw none of them (malformed %u/%u)", nh.rx_drop_malformed,
         nj.rx_drop_malformed);
   CHECK(sj.rx_pkts == nj.rx_frames && sh.rx_pkts == nh.rx_frames,
         "netdrv ring carried netdrv frames only (%u==%u, %u==%u)", sj.rx_pkts,
         nj.rx_frames, sh.rx_pkts, nh.rx_frames);
   CHECK(xs->resent > 0 && sh.fault_dropped > 0, "loss really happened");
   CHECK(sj.bulk_drop == 0, "bulk ring never overflowed (%u)", sj.bulk_drop);
   printf("  %u chunks in %.2f s (%.0f KiB/s): sent %u, resent %u (%.1f%%), "
          "dup %u, reports %u, shim dropped %u\n", CHUNKS,
          (double)(t1 - t0) / 1e6,
          BLOB_BYTES / 1024.0 / ((double)(t1 - t0) / 1e6), xs->tx, xs->resent,
          100.0 * xs->resent / (xs->tx ? xs->tx : 1), xr->dup, xr->reports,
          sh.fault_dropped);

   /* closed lane: its datagrams are no longer taken (and are not netdrv's) */
   lan_transport_bulk_close(tj);
   {
      uint8_t mac[6], pkt[64];
      lan_stats before, after;
      nd_sleep_us(150000);               /* the shim's last delayed frames */
      lan_transport_get_stats(tj, &before);
      memset(pkt, 0, sizeof(pkt));
      memcpy(pkt, "GBLK", 4);
      netdrv_peer_mac(H.nd, 1, mac);
      lan_transport_set_fault(th, 0, 0, 0, 0, 0);
      lan_transport_bulk_send(th, mac, pkt, sizeof(pkt));
      nd_sleep_us(100000);
      lan_transport_get_stats(tj, &after);
      CHECK(after.bulk_rx == before.bulk_rx &&
            after.rx_foreign == before.rx_foreign + 1 &&
            after.rx_pkts == before.rx_pkts,
            "after bulk_close a bulk datagram is foreign, not queued");
   }
   con_close(&J);
   con_close(&H);
   lan_transport_destroy(tj);
   lan_transport_destroy(th);
   free(xs->blob); free(xr->blob); free(xs); free(xr);
}

/* ================================================================= hostile == */

static void test_hostile(void)
{
   uint16_t base = test_port(8);
   lan_transport *th, *tj;
   console H, J;
   struct sockaddr_in to;
   uint8_t pkt[1600];
   lan_room_info in;
   nd_stats before, after;
   lan_stats sb, sa;
   uint32_t rng = 0xC0FFEE;
   int fd, i;

   printf("[hostile: discovery, garbage and foreign bulk aimed at a session]\n");
   th = mk_host(base, 0);
   tj = mk_join(base, inet_addr("127.0.0.1"), base);
   CHECK(th && tj, "transports");
   if (!th || !tj)
      return;
   mk_info(&in, "GB02HOST", 2, LAN_KIND_GB_CABLE, "h", "HOSTILE", "HOST", 9);
   lan_transport_announce(th, &in);
   lan_transport_bulk_open(th, 64, "GBLK");
   CHECK(con_open(&H, "host", th, 1, PROTO_A, 0x81, NULL) == 0, "H");
   CHECK(con_open(&J, "join", tj, 0, PROTO_A, 0x82, NULL) == 0, "J");
   WAIT_FOR(5000, atomic_load(&J.active) && atomic_load(&H.peers) == 1);
   CHECK(atomic_load(&J.active), "session up");

   /* quiesce, snapshot */
   atomic_store(&H.run, 0);
   pthread_join(H.th, NULL);
   H.threaded = 0;
   netdrv_get_stats(H.nd, &before);
   lan_transport_get_stats(th, &sb);

   fd = socket(AF_INET, SOCK_DGRAM, 0);
   memset(&to, 0, sizeof(to));
   to.sin_family = AF_INET;
   to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
   to.sin_port = htons(base);
   for (i = 0; i < 600; i++)
   {
      size_t n;
      int kind = i % 6;
      rng = rng * 1103515245u + 12345u;
      n = 1 + (rng >> 8) % 1500;
      memset(pkt, (int)(rng >> 16), sizeof(pkt));
      switch (kind)
      {
         case 0:                         /* random bytes */
            break;
         case 1:                         /* discovery magic, garbage after */
            memcpy(pkt, "GBLD", 4);
            break;
         case 2:                         /* a well-formed probe */
            memcpy(pkt, "GBLD", 4);
            pkt[4] = 1; pkt[5] = 1; pkt[6] = 18; pkt[7] = 0;
            pkt[16] = (uint8_t)nd_crc16(0xFFFF, pkt, 16);
            pkt[17] = (uint8_t)(nd_crc16(0xFFFF, pkt, 16) >> 8);
            n = 18;
            break;
         case 3:                         /* bulk magic from a stranger */
            memcpy(pkt, "GBLK", 4);
            if (n > LAN_BULK_MAX) n = LAN_BULK_MAX;
            if (n < 4) n = 4;
            break;
         case 4:                         /* netdrv magic, oversize */
            pkt[0] = 0x31; pkt[1] = 0x4E; pkt[2] = 0x50; pkt[3] = 0x47;
            n = ND_MAX_FRAME + 1 + (rng >> 8) % 64;
            break;
         default:                        /* an ANNOUNCE aimed at a host */
            memcpy(pkt, "GBLD", 4);
            pkt[4] = 1; pkt[5] = 2; pkt[6] = 86; pkt[7] = 0;
            n = 86;
            break;
      }
      sendto(fd, pkt, n, 0, (struct sockaddr *)&to, sizeof(to));
      if ((i % 50) == 49)
         nd_sleep_us(20000);             /* let the I/O thread drain */
   }
   nd_sleep_us(300000);
   close(fd);

   /* pump what the transport queued (on this thread: the console's is
    * stopped) and look at what netdrv saw */
   for (i = 0; i < 50; i++)
   {
      uint8_t b[LAN_BULK_MAX];
      con_step(&H);
      while (lan_transport_bulk_recv(th, b, sizeof(b)) > 0)
         CHECK(0, "a stranger's bulk datagram was delivered");
      nd_sleep_us(1000);
   }
   netdrv_get_stats(H.nd, &after);
   lan_transport_get_stats(th, &sa);
   CHECK(after.rx_drop_malformed == before.rx_drop_malformed &&
         after.rx_drop_crc == before.rx_drop_crc,
         "netdrv was handed none of it (malformed +%u, crc +%u)",
         after.rx_drop_malformed - before.rx_drop_malformed,
         after.rx_drop_crc - before.rx_drop_crc);
   CHECK(after.delivered_rel == before.delivered_rel &&
         after.delivered_unrel == before.delivered_unrel, "nothing delivered");
   CHECK(sa.rx_foreign - sb.rx_foreign >= 100, "random bytes counted foreign "
         "(+%u)", sa.rx_foreign - sb.rx_foreign);
   CHECK(sa.rx_oversize - sb.rx_oversize == 100, "oversize 'GPN1' dropped "
         "(+%u)", sa.rx_oversize - sb.rx_oversize);
   CHECK(sa.disc_probes_rx - sb.disc_probes_rx == 100, "probes seen (+%u)",
         sa.disc_probes_rx - sb.disc_probes_rx);
   CHECK(sa.disc_replies_tx - sb.disc_replies_tx <= 30 * 2 + 30 &&
         sa.disc_limited > sb.disc_limited,
         "probe answers are rate limited (%u answered, %u not)",
         sa.disc_replies_tx - sb.disc_replies_tx,
         sa.disc_limited - sb.disc_limited);
   CHECK(sa.bulk_foreign - sb.bulk_foreign > 0, "stranger's bulk discarded "
         "(+%u)", sa.bulk_foreign - sb.bulk_foreign);
   CHECK(atomic_load(&H.peers) == 1 && netdrv_active(H.nd),
         "the session is still up");
   printf("  foreign +%u oversize +%u probes +%u answered %u limited %u "
          "bulk_foreign +%u\n", sa.rx_foreign - sb.rx_foreign,
          sa.rx_oversize - sb.rx_oversize, sa.disc_probes_rx - sb.disc_probes_rx,
          sa.disc_replies_tx - sb.disc_replies_tx,
          sa.disc_limited - sb.disc_limited, sa.bulk_foreign - sb.bulk_foreign);
   con_close(&J);
   con_close(&H);
   lan_transport_destroy(tj);
   lan_transport_destroy(th);
}

/* ================================================================== cycles == */

static int count_dir(const char *path)
{
   DIR *d = opendir(path);
   struct dirent *e;
   int n = 0;
   if (!d)
      return -1;
   while ((e = readdir(d)))
      if (e->d_name[0] != '.')
         n++;
   closedir(d);
   return n;
}

static void test_cycles(void)
{
   enum { CYCLES = 40 };
   uint16_t base = test_port(9);
   int fds0, thr0, fds1, thr1, i, ok = 0;
   lan_room rooms[4];
   lan_scan_config sc;

   printf("[cycles: %d x (host + scan + join + session + bulk + destroy)]\n",
          CYCLES);
   fds0 = count_dir("/proc/self/fd");
   thr0 = count_dir("/proc/self/task");
   for (i = 0; i < CYCLES; i++)
   {
      lan_transport *th = mk_host(base, 0), *tj;
      lan_room_info in;
      console H, J;
      int n;
      if (!th)
      {
         CHECK(0, "cycle %d host: %s", i, lan_last_error());
         break;
      }
      CHECK(lan_transport_port(th) == base,
            "cycle %d: the port is free again (got %u)", i,
            lan_transport_port(th));
      mk_info(&in, "GB04CYCL", 4, LAN_KIND_GB_CABLE, "c", "CYCLE", "CYCL", 3);
      lan_transport_announce(th, &in);
      scan_cfg(&sc, base, 3);
      n = (i % 4) == 0 ? lan_discovery_scan(&sc, rooms, 4, 60) : 1;
      if ((i % 4) == 0)
         CHECK(n == 1, "cycle %d scan n=%d", i, n);
      tj = mk_join(base, inet_addr("127.0.0.1"), base);
      if (!tj)
      {
         CHECK(0, "cycle %d join: %s", i, lan_last_error());
         lan_transport_destroy(th);
         break;
      }
      if (i & 1)
      {
         lan_transport_set_fault(th, 2, 2, 3, 3, (uint32_t)(i + 1));
         lan_transport_bulk_open(th, 16, "GBLK");
         lan_transport_bulk_open(tj, 16, "GBLK");
      }
      con_open(&H, "host", th, 1, PROTO_A, 0x900 + (uint32_t)i, NULL);
      con_open(&J, "join", tj, 0, PROTO_A, 0xA00 + (uint32_t)i, NULL);
      H.to_send = J.to_send = 5;
      atomic_store(&H.go, 1);
      atomic_store(&J.go, 1);
      WAIT_FOR(3000, atomic_load(&J.rx_ok[0]) == 5 && atomic_load(&H.rx_ok[1]) == 5);
      if (atomic_load(&J.rx_ok[0]) == 5 && atomic_load(&H.rx_ok[1]) == 5)
         ok++;
      /* alternate who goes first: either order must be clean */
      if (i & 2)
      {
         con_close(&H); con_close(&J);
      }
      else
      {
         con_close(&J); con_close(&H);
      }
      lan_transport_destroy(tj);
      lan_transport_destroy(th);
   }
   fds1 = count_dir("/proc/self/fd");
   thr1 = count_dir("/proc/self/task");
   CHECK(ok == CYCLES, "%d of %d cycles carried data both ways", ok, CYCLES);
   CHECK(fds1 == fds0, "descriptors: %d before, %d after", fds0, fds1);
   CHECK(thr1 == thr0, "threads: %d before, %d after", thr0, thr1);
   printf("  fds %d -> %d, threads %d -> %d\n", fds0, fds1, thr0, thr1);
}

/* ================================================================= realnet == */

static void test_realnet(void)
{
   uint16_t base = test_port(10);
   uint32_t ip = 0, mask = 0, compat = 42;
   unsigned saved = g_net_flags;
   lan_transport *th, *tj;
   lan_room_info in;
   lan_room rooms[4];
   lan_scan_config sc;
   console H, J;
   char a[32], m[32];
   int n;

   printf("[realnet: the machine's own interface and broadcast addresses]\n");
   if (lan_local_ipv4(&ip, &mask) != 0)
   {
      printf("  SKIPPED: no non-loopback IPv4 interface\n");
      return;
   }
   inet_ntop(AF_INET, &ip, a, sizeof(a));
   inet_ntop(AF_INET, &mask, m, sizeof(m));
   printf("  interface %s mask %s\n", a, m);
   g_net_flags = 0;
   th = mk_host(base, 0);
   CHECK(th != NULL, "host: %s", lan_last_error());
   if (!th)
   {
      g_net_flags = saved;
      return;
   }
   CHECK(lan_transport_ip(th) == ip, "host declares the interface address");
   mk_info(&in, "GC08REAL", 8, LAN_KIND_GBA_CABLE, "real", "REALNET", "REAL",
           compat);
   lan_transport_announce(th, &in);
   scan_cfg(&sc, base, compat);
   n = lan_discovery_scan(&sc, rooms, 4, 700);
   if (n == 0)
   {
      /* broadcasts do not loop back on this machine's interface; that is
       * the machine, not the transport */
      printf("  SKIPPED: broadcast is not delivered locally here\n");
      lan_transport_destroy(th);
      g_net_flags = saved;
      return;
   }
   CHECK(n == 1 && rooms[0].ip_be == ip && rooms[0].port == base,
         "found over the real broadcast at %s (n=%d)", n ? rooms[0].addr : "-",
         n);
   printf("  room at %s rtt=%d ms\n", rooms[0].addr, rooms[0].rtt_ms);

   /* a broadcast JOIN: subnet broadcast and 255.255.255.255 both reach the
    * host, from one source address, so it admits the joiner once */
   tj = mk_join(base, 0, 0);
   CHECK(tj != NULL, "join: %s", lan_last_error());
   if (tj)
   {
      CHECK(con_open(&H, "host", th, 1, PROTO_A, 0x51, NULL) == 0, "H");
      CHECK(con_open(&J, "join", tj, 0, PROTO_A, 0x52, NULL) == 0, "J");
      WAIT_FOR(6000, atomic_load(&J.active) && atomic_load(&H.peers) == 1);
      H.to_send = J.to_send = 50;
      atomic_store(&H.go, 1);
      atomic_store(&J.go, 1);
      WAIT_FOR(8000, atomic_load(&J.rx_ok[0]) == 50 && atomic_load(&H.rx_ok[1]) == 50);
      CHECK(atomic_load(&J.rx_ok[0]) == 50 && atomic_load(&H.rx_ok[1]) == 50,
            "session by broadcast JOIN (%u/%u)", atomic_load(&J.rx_ok[0]),
            atomic_load(&H.rx_ok[1]));
      CHECK(atomic_load(&H.connects) == 1, "admitted once (connects=%d)",
            atomic_load(&H.connects));
      con_close(&J);
      con_close(&H);
      lan_transport_destroy(tj);
   }
   lan_transport_destroy(th);
   g_net_flags = saved;
}

/* =================================================================== bench == */

/* Not a test: how long a reliable payload takes to be DELIVERED under each
 * candidate ARQ profile on each kind of link.  The traffic is the link
 * session's -- one small payload per 16.7 ms frame each way -- and the
 * number that matters is the tail: a payload later than the input delay
 * stalls the game. */
typedef struct bench_side
{
   uint64_t sent_us[4096];
   uint32_t lat_us[4096];
   unsigned n_sent, n_rx;
   uint64_t last;
} bench_side;

static bench_side g_bench[2];

static void bench_tick(console *c, uint64_t now)
{
   bench_side *b = (bench_side *)c->user;
   if (!netdrv_active(c->nd) || netdrv_peer_count(c->nd) != 1)
      return;
   if (b->n_sent < 600 && now - b->last >= 16667)
   {
      uint8_t p[12];
      unsigned i = b->n_sent;
      memset(p, 0, sizeof(p));
      p[0] = (uint8_t)i; p[1] = (uint8_t)(i >> 8);
      b->sent_us[i] = nd_now_us();
      if (netdrv_send(c->nd, ND_RELIABLE | ND_FLUSH_HINT, p, sizeof(p),
                      netdrv_local_id(c->nd) == 0 ? 1 : 0) == 0)
         b->n_sent++;
      b->last = now;
   }
}

static int bench_deliver(console *c, const uint8_t *buf, size_t len, uint8_t src)
{
   /* the OTHER side's send times: both live in this process */
   bench_side *peer = &g_bench[c->user == &g_bench[0] ? 1 : 0];
   unsigned i = (unsigned)buf[0] | ((unsigned)buf[1] << 8);
   (void)len; (void)src;
   if (i < 4096 && peer->sent_us[i])
   {
      bench_side *me = (bench_side *)c->user;
      peer->lat_us[i] = (uint32_t)(nd_now_us() - peer->sent_us[i]);
      me->n_rx++;
   }
   return 1;
}

static int cmp_u32(const void *a, const void *b)
{
   uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
   return x < y ? -1 : x > y;
}

static void bench_one(int slot, const char *link, int loss, int lat, int jit,
                      const char *prof, uint32_t floor_us, uint32_t ceil_us,
                      uint32_t first_us)
{
   uint16_t base = test_port(11 + slot);
   lan_transport *th = mk_host(base, 0);
   lan_transport *tj = mk_join(base, inet_addr("127.0.0.1"), base);
   console H, J;
   nd_config cfg;
   nd_stats nh, nj;
   uint32_t all[1200];
   unsigned n = 0, i, late50 = 0, late100 = 0;

   if (!th || !tj)
   {
      printf("bench: transports: %s\n", lan_last_error());
      return;
   }
   memset(&cfg, 0, sizeof(cfg));
   cfg.retx_us = floor_us;
   cfg.retx_max_us = ceil_us;
   cfg.rto_first_max_us = first_us;
   memset(g_bench, 0, sizeof(g_bench));
   lan_transport_set_fault(th, loss, 0, lat, jit, 0xBEEF01);
   lan_transport_set_fault(tj, loss, 0, lat, jit, 0xBEEF02);
   con_hooks(bench_tick, bench_deliver, &g_bench[0]);
   con_open(&H, "host", th, 1, PROTO_A, 0x61, &cfg);
   con_hooks(bench_tick, bench_deliver, &g_bench[1]);
   con_open(&J, "join", tj, 0, PROTO_A, 0x62, &cfg);
   WAIT_FOR(40000, g_bench[0].n_rx == 600 && g_bench[1].n_rx == 600);
   atomic_store(&H.run, 0); atomic_store(&J.run, 0);
   pthread_join(H.th, NULL); pthread_join(J.th, NULL);
   H.threaded = J.threaded = 0;
   netdrv_get_stats(H.nd, &nh);
   netdrv_get_stats(J.nd, &nj);
   for (i = 0; i < 600; i++)
   {
      if (g_bench[0].lat_us[i]) all[n++] = g_bench[0].lat_us[i];
      if (g_bench[1].lat_us[i]) all[n++] = g_bench[1].lat_us[i];
   }
   qsort(all, n, sizeof(all[0]), cmp_u32);
   for (i = 0; i < n; i++)
   {
      /* over the one-way latency the link itself imposes */
      uint32_t extra = all[i] > (uint32_t)lat * 1000u
                          ? all[i] - (uint32_t)lat * 1000u : 0;
      if (extra > 50000) late50++;
      if (extra > 100000) late100++;
   }
   if (n)
      printf("%-9s %-22s %4u  %4u  %4u  %4u  %4u  %5.1f%%  %5.1f%%  %4u%%  %4u\n",
             link, prof, all[n / 2] / 1000, all[n * 9 / 10] / 1000,
             all[n * 99 / 100] / 1000, all[n - 1] / 1000, n,
             100.0 * late50 / n, 100.0 * late100 / n,
             (nh.retx + nj.retx) * 100u / ((nh.acked + nj.acked) ? (nh.acked + nj.acked) : 1),
             (nh.srtt_us + nj.srtt_us) / 2000u);
   con_close(&J);
   con_close(&H);
   lan_transport_destroy(tj);
   lan_transport_destroy(th);
}

static void bench(const char *only)
{
   static const struct { const char *name; int loss, lat, jit; } LINK[] = {
      { "lan",      1,  2,  4 },      /* Wi-Fi through an access point */
      { "lan-bad",  3,  5, 25 },      /* a busy 2.4 GHz channel */
      { "inet",     1, 30, 10 },      /* same country */
      { "inet-far", 2, 80, 30 },      /* across an ocean, lossy */
      { "adhoc",    2, 10, 15 },      /* console to console (a guess) */
      { "adhoc0",   0,  8, 12 },      /* ... if 802.11 repairs every loss */
   };
   static const struct { const char *name; uint32_t fl, ce, fi; } PROF[] = {
      { "pin30   30/240/30",      30000,  240000,  30000 },
      { "lan     30/2500/40",     30000, 2500000,  40000 },
      { "inet    50/2500/120",    50000, 2500000, 120000 },
      { "30-60   30/2500/60",     30000, 2500000,  60000 },
      { "pin60   60/2500/60",     60000, 2500000,  60000 },
      { "adaptive 30/2500/-",     30000, 2500000, 2500000 },
      { "adhoc   100/2500/-",    100000, 2500000, 2500000 },
      { "psp     200/2500/-",    200000, 2500000, 2500000 },
   };
   unsigned l, p;
   int slot = 0;
   printf("delivery latency of one 12-byte reliable payload per frame each "
          "way, 600 per side\n(profile = RTO floor / ceiling / first-retx cap, "
          "ms; '>50' = share delivered more than\n 50 ms later than the link's "
          "own latency)\n\n");
   printf("%-9s %-22s %4s  %4s  %4s  %4s  %4s  %6s  %6s  %5s  %4s\n", "link",
          "profile", "p50", "p90", "p99", "max", "n", ">50ms", ">100ms",
          "retx", "srtt");
   for (l = 0; l < sizeof(LINK) / sizeof(LINK[0]); l++)
   {
      if (only && strcmp(only, LINK[l].name))
         continue;
      for (p = 0; p < sizeof(PROF) / sizeof(PROF[0]); p++)
         bench_one(slot++ % 40, LINK[l].name, LINK[l].loss, LINK[l].lat,
                   LINK[l].jit, PROF[p].name, PROF[p].fl, PROF[p].ce,
                   PROF[p].fi);
      printf("\n");
   }
}

/* ==================================================================== main == */

static const struct { const char *name; void (*fn)(void); } TESTS[] = {
   { "helpers",   test_helpers },
   { "discovery", test_discovery },
   { "portbusy",  test_portbusy },
   { "beacon",    test_beacon },
   { "session",   test_session },
   { "bcastjoin", test_bcastjoin },
   { "nat",       test_nat },
   { "bulk",      test_bulk },
   { "hostile",   test_hostile },
   { "cycles",    test_cycles },
   { "realnet",   test_realnet },
};

int main(int argc, char **argv)
{
   unsigned i;
   int a, ran = 0;

   setvbuf(stdout, NULL, _IOLBF, 0);
   /* netdrv builds its CRC table on first use and is single-threaded by
    * contract; this harness runs several netdrv instances, each on a thread
    * of its own, so build the table before any of them exists. */
   (void)nd_crc16(0xFFFF, "", 0);
   g_port_base = (uint16_t)(21000 + (getpid() % 40) * 1000);
   if (argc > 1 && !strcmp(argv[1], "bench"))
   {
      bench(argc > 2 ? argv[2] : NULL);
      return 0;
   }
   for (i = 0; i < sizeof(TESTS) / sizeof(TESTS[0]); i++)
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
      printf("FAILED: %d check(s)\n", g_fail);
      return 1;
   }
   printf("ALL LAN TESTS PASSED\n");
   return 0;
}
