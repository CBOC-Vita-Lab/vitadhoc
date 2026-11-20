/* SPDX-License-Identifier: MIT OR GPL-2.0-or-later
 * Written for VitAdhoc by its owner; offered under either licence so the separate players (snes.bin, md.bin) can link it. */
/* transport_lan.c -- UDP/IPv4 netdrv backend with room discovery.
 * See transport_lan.h for the contract; this file's comments are the "why".
 *
 * WHY ONE SOCKET FOR GAME AND DISCOVERY (and one extra port for beacons).
 *
 *   - A host answers discovery probes on the socket it plays on.  One socket
 *     is one port to forward on a router and one hole in a firewall, so a
 *     direct-IP joiner can probe the very port it is about to join and learn
 *     the room, the game and whether the builds match before it tries.
 *   - Several hosts on one machine (the tests; two desktop twins) each own a
 *     different port of the range, which a shared well-known discovery port
 *     could not give them: only one process can receive unicast on a port.
 *   - netdrv never sees a discovery datagram: the I/O thread routes on the
 *     first four bytes ("GBLD" discovery, 'GPN1' netdrv, the session's own
 *     magic for the bulk lane) before anything is queued, and netdrv's own
 *     magic/length/CRC wall stands behind that.
 *   - The price is that a scan has to probe LAN_PORT_TRIES ports instead of
 *     one: eight 18-byte datagrams per broadcast address every 500 ms.
 *   - Beacons are the exception.  A host cannot know which port a listening
 *     scanner sits on, so the once-a-second beacon goes to one fixed port,
 *     disc_port-1 (19018), which only scanners bind.  It is a second chance,
 *     not the mechanism: it covers an access point that forwards a host's
 *     broadcast but drops the scanner's.  A scanner that cannot bind it
 *     simply relies on probe and reply.
 *
 * NAT (transport B, a joiner behind a different network than the host).
 *
 *   Where do addresses come from in netdrv?  The host learns a joiner's
 *   address from the SOURCE of its JOIN datagram (netdrv.c host_on_join),
 *   stores that in the roster and sends everything there.  So host -> joiner
 *   already goes to the address and port the host RECEIVED from, i.e. to the
 *   joiner's NAT mapping, which netdrv's 250 ms PING keeps open.  Nothing to
 *   fix on that side.
 *
 *   The other direction is the problem.  The roster entry for the host
 *   itself carries the host's own local_addr() -- its private address, say
 *   192.168.1.5:19019 -- and a client's netdrv (roster_apply) adopts that as
 *   the address of peer 0: it sends DATA there and drops every frame whose
 *   source is not that (lookup_peer).  The joiner reached the host at
 *   203.0.113.7:19019, so from the first DATA frame on nothing gets through
 *   in either direction.
 *
 *   The fix lives here, in the joining transport.  Netdrv's wire version 2
 *   (GBAdhoc 4.1) admits a joiner in three steps -- JOIN, the host's OFFER,
 *   the joiner's CONFIRM -- before the WELCOME, and the joiner takes the
 *   host's address from the OFFER, which carries no roster: from then on
 *   the host is wherever the OFFER came from (the OBSERVED address).  So
 *   when a WELCOME or ROSTER arrives, this transport rewrites the host's
 *   own roster entry -- the address it DECLARES for itself -- to the
 *   observed one, and the CRC with it; netdrv then finds the same address
 *   in the OFFER, the WELCOME, the roster and every DATA frame.  Version 2
 *   also checks that the joiner's own entry is its own address; through a
 *   NAT the host lists the joiner's MAPPED address instead (the router's
 *   outside socket), so that entry is rewritten to the joiner's local
 *   address as well (remembered from the WELCOME, which names "your_id",
 *   and matched by address in each later ROSTER).  Sending needs nothing:
 *   netdrv sends to the observed address, which is where the host is.
 *   (Version 1 had no OFFER, and this transport instead reported the
 *   host's frames as coming from its declared address.)  Netdrv, its wire
 *   format and the host are untouched; on a LAN the addresses are equal
 *   and nothing is rewritten.
 *
 *   What this does not solve: a third player.  Client-to-client addresses in
 *   the roster are each client's mapping AS SEEN BY THE HOST, which another
 *   client behind a different NAT may not reach.  Internet play is two
 *   players (which is every link-cable mode); more need everybody on one LAN.
 */
#include "transport_lan.h"
#include "netdrv_wire.h"
#include "nd_thread.h"
#include "nat_stun.h"
#include "nat_turn.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#ifdef VITA
#include <psp2/net/netctl.h>
#else
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#endif

#define LAN_BOOK_CAP     16        /* known peer addresses (the mesh is 5) */
#define LAN_RX_SLOTS     256
#define LAN_RX_SCRATCH   1536      /* one datagram, MTU-sized */
#define LAN_DELAYQ_CAP   1024      /* fault shim: datagrams held back */
#define LAN_IDLE_WAIT_MS 50        /* I/O thread poll slice = teardown bound */
#define LAN_BEACON_US    1000000ull
#define LAN_PROBE_US     500000ull
#define LAN_ROOM_TTL_US  3500000ull
#define LAN_REPLY_BURST  30        /* probe answers per second, at most */
#define LAN_KEEP_US      1000000ull /* I/O-thread keepalive, per session peer */
#define LAN_HEARD_TTL_US 60000000ull /* a peer not heard for this long gets no
                                      * more keepalives (and is forgotten) */
#define LAN_NAT_WAIT_MS  20        /* I/O poll slice while STUN / punch / TURN is in flight */
#define LAN_PUNCH_LEN    26
#define LAN_PUNCH_FAST_MS 200      /* PUNCH every 200 ms for the first 2 s ... */
#define LAN_PUNCH_SLOW_MS 500      /* ... then every 500 ms */
#define LAN_PUNCH_MAX_CAND 3
#define LAN_PUNCH_OPENED_MS 4000   /* the race an incoming PUNCH starts on an armed transport */
#define LAN_RELAY_PORT_MS 3000     /* no answer at all from a TURN port: try the next */
#define LAN_RELAY_PORTS_MAX 3
#define LAN_STUN_SRC_MAX  3        /* STUN servers (2) + the TURN server (1) */

/* The pure NAT modules use their own copies of the state numbers (they do
 * not include this header); they must be the same. */
_Static_assert((int)NAT_STUN_IDLE == (int)LAN_STUN_IDLE && (int)NAT_STUN_RUNNING == (int)LAN_STUN_RUNNING &&
               (int)NAT_STUN_OK == (int)LAN_STUN_OK && (int)NAT_STUN_FAILED == (int)LAN_STUN_FAILED,
               "nat_stun.h and transport_lan.h disagree on LAN_STUN_*");
_Static_assert((int)NAT_RELAY_OFF == (int)LAN_RELAY_OFF && (int)NAT_RELAY_ALLOCATING == (int)LAN_RELAY_ALLOCATING &&
               (int)NAT_RELAY_ALLOCATED == (int)LAN_RELAY_ALLOCATED && (int)NAT_RELAY_BINDING == (int)LAN_RELAY_BINDING &&
               (int)NAT_RELAY_READY == (int)LAN_RELAY_READY && (int)NAT_RELAY_FAILED == (int)LAN_RELAY_FAILED &&
               (int)NAT_RELAY_E_NONE == (int)LAN_RELAY_E_NONE && (int)NAT_RELAY_E_TIMEOUT == (int)LAN_RELAY_E_TIMEOUT &&
               (int)NAT_RELAY_E_AUTH == (int)LAN_RELAY_E_AUTH && (int)NAT_RELAY_E_QUOTA == (int)LAN_RELAY_E_QUOTA &&
               (int)NAT_RELAY_E_LOST == (int)LAN_RELAY_E_LOST,
               "nat_turn.h and transport_lan.h disagree on LAN_RELAY_*");

/* ---- discovery wire format (all little-endian, explicit) -----------------
 *
 *   0  4  magic "GBLD"
 *   4  1  version (LAN_DISC_VERSION)
 *   5  1  type: 1 PROBE, 2 ANNOUNCE, 3 GONE
 *   6  2  total length in bytes
 *   8  4  cookie: a PROBE's own; an ANNOUNCE echoes the probe it answers
 *         (0 in a beacon), which gives the scanner a round-trip time
 *   12 4  compatibility hash of the sender
 *   PROBE ends here: +16 u16 CRC                                   = 18 bytes
 *   GONE:     16 u32 room_id, 20 u16 CRC                           = 22 bytes
 *   ANNOUNCE: 16 u32 room_id   20 u16 port
 *             22 u8 kind  23 u8 players  24 u8 max_players  25 u8 flags
 *             26 u8 room number  27 u8 reserved
 *             28 name[16]  44 nick[16]  60 title[20]  80 code[4]
 *             84 u16 CRC                                           = 86 bytes
 *   CRC = netdrv's CRC16-CCITT over everything before it.
 *
 * Versioning: fields are only ever APPENDED.  A receiver accepts any
 * version >= 1 whose length is at least what it knows, reads the fields it
 * knows and takes the CRC from the last two bytes, so an older build still
 * lists a newer build's room (and shows it as incompatible through the
 * hash) instead of not seeing it at all. */
#define DISC_MAGIC0 'G'
#define DISC_MAGIC1 'B'
#define DISC_MAGIC2 'L'
#define DISC_MAGIC3 'D'
/* ECHO / ECHO_REPLY (added for the Vita's link-delay choice, see
 * lan_transport_echo): PROBE-shaped, the cookie is the sender's; any
 * transport answers one, host or joiner.  An older build ignores both
 * (disc_check: unknown type). */
enum { DISC_PROBE = 1, DISC_ANNOUNCE = 2, DISC_GONE = 3, DISC_ECHO = 4,
       DISC_ECHO_REPLY = 5,
       /* KEEP: PROBE-shaped, sent once a second by the I/O thread to every
        * session peer (LAN_F_IO_KEEPALIVE).  Never answered; its arrival
        * only says "this console's process is alive". */
       DISC_KEEP = 6,
       /* PUNCH / PUNCH_ACK (online play, 2.0): see "NAT layer" below.  An
        * older build ignores both (disc_check: unknown type). */
       DISC_PUNCH = 7, DISC_PUNCH_ACK = 8 };
#define DISC_PROBE_LEN    18
#define DISC_GONE_LEN     22
#define DISC_ANNOUNCE_LEN 86
#define DISC_MAX_LEN      256      /* room for later versions' fields */

static const char *g_lan_err = "";
const char *lan_last_error(void) { return g_lan_err; }

/* ------------------------------------------------------------- byte utils -- */

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v)
{
   p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
   p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t get32(const uint8_t *p)
{
   return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
          ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Fixed-width text field: NUL-padded, not necessarily terminated on the
 * wire; always terminated in memory. */
static void put_text(uint8_t *dst, size_t width, const char *src)
{
   size_t n = src ? strnlen(src, width) : 0;
   memset(dst, 0, width);
   memcpy(dst, src, n);
}
static void get_text(char *dst, size_t cap, const uint8_t *src, size_t width)
{
   size_t n = width < cap - 1 ? width : cap - 1, i;
   memcpy(dst, src, n);
   dst[n] = '\0';
   /* Whatever arrives ends up on a screen: keep printable ASCII only. */
   for (i = 0; dst[i]; i++)
      if ((unsigned char)dst[i] < 0x20 || (unsigned char)dst[i] > 0x7E)
         dst[i] = '?';
}

/* ---------------------------------------------------------------- helpers -- */

void lan_mac_from_addr(uint32_t ip_be, uint16_t port, uint8_t mac[6])
{
   uint16_t p = htons(port);
   memcpy(mac, &ip_be, 4);
   memcpy(mac + 4, &p, 2);
}

void lan_addr_from_mac(const uint8_t mac[6], uint32_t *ip_be, uint16_t *port)
{
   uint16_t p;
   memcpy(ip_be, mac, 4);
   memcpy(&p, mac + 4, 2);
   *port = ntohs(p);
}

static void sa_from_mac(const uint8_t mac[6], struct sockaddr_in *a)
{
   memset(a, 0, sizeof(*a));
   a->sin_family = AF_INET;
   memcpy(&a->sin_addr.s_addr, mac, 4);
   memcpy(&a->sin_port, mac + 4, 2);
}

static void mac_from_sa(const struct sockaddr_in *a, uint8_t mac[6])
{
   memcpy(mac, &a->sin_addr.s_addr, 4);
   memcpy(mac + 4, &a->sin_port, 2);
}

static void sa_make(struct sockaddr_in *a, uint32_t ip_be, uint16_t port)
{
   memset(a, 0, sizeof(*a));
   a->sin_family = AF_INET;
   a->sin_addr.s_addr = ip_be;
   a->sin_port = htons(port);
}

static int sa_eq(const struct sockaddr_in *a, const struct sockaddr_in *b)
{
   return a->sin_addr.s_addr == b->sin_addr.s_addr && a->sin_port == b->sin_port;
}

const char *lan_format_addr(uint32_t ip_be, uint16_t port, char *buf, size_t n)
{
   const uint8_t *b = (const uint8_t *)&ip_be;
   if (!buf || !n)
      return "";
   snprintf(buf, n, "%u.%u.%u.%u:%u", b[0], b[1], b[2], b[3], (unsigned)port);
   return buf;
}

int lan_parse_addr(const char *text, uint32_t *ip_be, uint16_t *port,
                   uint16_t default_port)
{
   /* By hand rather than inet_pton + strtol: the input is what a player
    * typed on an on-screen keyboard, and "1.2.3" or "1.2.3.4:" or
    * "1.2.3.4:99999" must be refused, not half-read. */
   uint8_t  quad[4];
   unsigned v;
   int      i, digits;
   const char *p = text;

   if (!p)
      return -1;
   while (*p == ' ' || *p == '\t')
      p++;
   for (i = 0; i < 4; i++)
   {
      v = 0;
      digits = 0;
      while (*p >= '0' && *p <= '9' && digits < 3)
      {
         v = v * 10 + (unsigned)(*p - '0');
         p++;
         digits++;
      }
      if (!digits || v > 255)
         return -1;
      quad[i] = (uint8_t)v;
      if (i < 3)
      {
         if (*p != '.')
            return -1;
         p++;
      }
   }
   v = default_port;
   if (*p == ':')
   {
      p++;
      v = 0;
      digits = 0;
      while (*p >= '0' && *p <= '9' && digits < 5)
      {
         v = v * 10 + (unsigned)(*p - '0');
         p++;
         digits++;
      }
      if (!digits || v == 0 || v > 65535)
         return -1;
   }
   while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
      p++;
   if (*p)
      return -1;
   if (ip_be)
      memcpy(ip_be, quad, 4);
   if (port)
      *port = (uint16_t)v;
   return 0;
}

uint32_t lan_compat_hash(const char *protocol, const char *build)
{
   uint32_t h = 2166136261u;
   const char *parts[2];
   char geom[48];
   size_t i;
   int k;
   parts[0] = protocol;
   parts[1] = build;
   for (k = 0; k < 2; k++)
   {
      for (i = 0; parts[k] && parts[k][i]; i++)
         h = (h ^ (uint8_t)parts[k][i]) * 16777619u;
      h = (h ^ 0xFFu) * 16777619u;         /* field separator */
   }
   /* Two builds whose frame budget or ARQ window differ cannot hold a
    * session even with the same protocol string (a frame longer than the
    * receiver's ND_MAX_FRAME is malformed to it). */
   snprintf(geom, sizeof(geom), "%u/%u/%u", (unsigned)ND_MAX_PAYLOAD,
            (unsigned)ND_WINDOW, (unsigned)LAN_DISC_VERSION);
   for (i = 0; geom[i]; i++)
      h = (h ^ (uint8_t)geom[i]) * 16777619u;
   return h ? h : 1;
}

/* ------------------------------------------------------ platform: sockets -- */

static int lan_would_block(void)
{
   return errno == EAGAIN || errno == EWOULDBLOCK;
}

/* A UDP socket: non-blocking, allowed to send to broadcast addresses.
 * Every recvfrom/sendto below also passes MSG_DONTWAIT, so the emulation
 * thread cannot block even if the option were lost. */
static int lan_sock_open(void)
{
   int on = 1, fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
   int rcv = 256 * 1024;
   if (fd < 0)
      return -1;
#ifdef VITA
   /* vitasdk newlib: no ioctl(FIONBIO); the socket option is the way
    * (fcntl(O_NONBLOCK) is implemented on top of the same option). */
   setsockopt(fd, SOL_SOCKET, SO_NONBLOCK, &on, sizeof(on));
#else
   fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
#endif
   setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));
   /* A bulk transfer arrives in bursts; the I/O thread drains at once, the
    * buffer only has to cover the thread being descheduled. */
   setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof(rcv));
   return fd;
}

static int lan_sock_bind(int fd, uint16_t port, int reuse)
{
   struct sockaddr_in a;
   if (reuse)
   {
      int on = 1;
      setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
   }
   /* INADDR_ANY, not our address: a socket bound to one unicast address is
    * not handed datagrams sent to the broadcast address. */
   sa_make(&a, htonl(INADDR_ANY), port);
   return bind(fd, (struct sockaddr *)&a, sizeof(a));
}

static uint16_t lan_sock_port(int fd)
{
   struct sockaddr_in a;
   socklen_t alen = sizeof(a);
   memset(&a, 0, sizeof(a));
   if (getsockname(fd, (struct sockaddr *)&a, &alen) != 0)
      return 0;
   return ntohs(a.sin_port);
}

/* Wait until the socket is readable or timeout_ms passed.  >0 readable,
 * 0 timeout, <0 poll itself failed. */
static int lan_wait_readable(int fd, int timeout_ms)
{
   struct pollfd p;
   int rc;
   p.fd = fd;
   p.events = POLLIN;
   p.revents = 0;
   rc = poll(&p, 1, timeout_ms);
   if (rc < 0 && errno == EINTR)
      return 0;
   return rc;
}

int lan_local_ipv4(uint32_t *ip_be, uint32_t *netmask_be)
{
#ifdef VITA
   SceNetCtlInfo info;
   struct in_addr a;
   memset(&info, 0, sizeof(info));
   if (sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_IP_ADDRESS, &info) < 0 ||
       inet_pton(AF_INET, info.ip_address, &a) != 1)
      return -1;
   if (ip_be)
      *ip_be = a.s_addr;
   if (netmask_be)
   {
      *netmask_be = 0;
      memset(&info, 0, sizeof(info));
      if (sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_NETMASK, &info) >= 0 &&
          inet_pton(AF_INET, info.netmask, &a) == 1)
         *netmask_be = a.s_addr;
   }
   return 0;
#else
   struct ifaddrs *list = NULL, *i;
   int found = -1;
   if (getifaddrs(&list) != 0)
      return -1;
   for (i = list; i; i = i->ifa_next)
   {
      if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET ||
          !(i->ifa_flags & IFF_UP) || (i->ifa_flags & IFF_LOOPBACK))
         continue;
      if (ip_be)
         *ip_be = ((struct sockaddr_in *)i->ifa_addr)->sin_addr.s_addr;
      if (netmask_be)
         *netmask_be = i->ifa_netmask
            ? ((struct sockaddr_in *)i->ifa_netmask)->sin_addr.s_addr : 0;
      found = 0;
      break;
   }
   freeifaddrs(list);
   return found;
#endif
}

/* Our address and the broadcast addresses worth trying, in `out` (ports are
 * filled in per send).  Returns how many.
 *
 * Every address in the list must make our datagrams arrive with the SAME
 * source address.  netdrv's host identifies a joiner by the source of its
 * JOIN: one joiner heard over two paths with two sources (the interface and
 * the loopback, say) would be admitted twice, and in a two-player room the
 * second admission is a refusal that can overtake the first one's WELCOME.
 * That is why LAN_F_LOOPBACK replaces the interface instead of adding to it. */
static int lan_bcast_list(uint32_t *ip_be, uint32_t *mask_be, unsigned flags,
                          struct sockaddr_in out[3])
{
   int n = 0;
   if (flags & LAN_F_LOOPBACK)
   {
      *ip_be = htonl(INADDR_LOOPBACK);
      *mask_be = htonl(0xFF000000u);
      sa_make(&out[n++], htonl(0x7FFFFFFFu), 0);      /* 127.255.255.255 */
      sa_make(&out[n++], htonl(INADDR_LOOPBACK), 0);  /* where lo has no
                                                         broadcast */
      return n;
   }
   if (!*ip_be)
   {
      uint32_t ip = 0, mask = 0;
      if (lan_local_ipv4(&ip, &mask) == 0)
      {
         *ip_be = ip;
         if (!*mask_be)
            *mask_be = mask;
      }
      else
      {
         /* No network interface: only this machine can be reached. */
         *ip_be = htonl(INADDR_LOOPBACK);
         *mask_be = htonl(0xFF000000u);
      }
   }
   /* The subnet's directed broadcast: the one that still works when the
    * access point filters the limited broadcast. */
   if (*mask_be && *mask_be != 0xFFFFFFFFu)
      sa_make(&out[n++], (*ip_be & *mask_be) | ~*mask_be, 0);
   if (*ip_be != htonl(INADDR_LOOPBACK))
      sa_make(&out[n++], htonl(INADDR_BROADCAST), 0);
   return n;
}

/* -------------------------------------------------------- discovery codec -- */

/* netdrv's CRC16-CCITT (poly 0x1021, init 0xFFFF), bit-serial and private.
 * NOT nd_crc16(): that one builds its table lazily on first use and is
 * documented single-threaded ("by the netdrv pump contract"), while this
 * code runs on the I/O thread.  ThreadSanitizer caught exactly that race in
 * the first version of this file; on a four-core console a reader could
 * take a half-built table.  Discovery datagrams are 86 bytes and rare, so
 * the slow loop costs nothing and shares nothing. */
static uint16_t disc_crc16(const uint8_t *p, size_t len)
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

static size_t disc_finish(uint8_t *b, size_t len)
{
   put16(b + 6, (uint16_t)(len + 2));
   put16(b + len, disc_crc16(b, len));
   return len + 2;
}

static void disc_head(uint8_t *b, uint8_t type, uint32_t cookie, uint32_t compat)
{
   b[0] = DISC_MAGIC0; b[1] = DISC_MAGIC1; b[2] = DISC_MAGIC2; b[3] = DISC_MAGIC3;
   b[4] = LAN_DISC_VERSION;
   b[5] = type;
   put16(b + 6, 0);
   put32(b + 8, cookie);
   put32(b + 12, compat);
}

static size_t disc_build_probe(uint8_t *b, uint32_t cookie, uint32_t compat)
{
   disc_head(b, DISC_PROBE, cookie, compat);
   return disc_finish(b, 16);
}

static size_t disc_build_echo(uint8_t *b, uint8_t type, uint32_t cookie)
{
   disc_head(b, type, cookie, 0);
   return disc_finish(b, 16);
}

/* PUNCH / PUNCH_ACK: the 8 bytes of the room's punch key after four zero
 * bytes; length 26 (see "NAT layer"). */
static size_t disc_build_punch(uint8_t *b, uint8_t type, uint32_t cookie, const uint8_t key[8])
{
   disc_head(b, type, cookie, 0);
   memcpy(b + 16, key, 8);
   return disc_finish(b, 24);
}

static size_t disc_build_gone(uint8_t *b, uint32_t room_id, uint32_t compat)
{
   disc_head(b, DISC_GONE, 0, compat);
   put32(b + 16, room_id);
   return disc_finish(b, 20);
}

static size_t disc_build_announce(uint8_t *b, uint32_t cookie,
                                  const lan_room_info *in, uint32_t room_id,
                                  uint16_t port)
{
   disc_head(b, DISC_ANNOUNCE, cookie, in->compat);
   put32(b + 16, room_id);
   put16(b + 20, port);
   b[22] = in->kind;
   b[23] = in->players;
   b[24] = in->max_players;
   b[25] = in->flags;
   b[26] = in->number;
   b[27] = 0;
   put_text(b + 28, 16, in->name);
   put_text(b + 44, 16, in->nick);
   put_text(b + 60, 20, in->title);
   put_text(b + 80, 4, in->code);
   return disc_finish(b, 84);
}

static int disc_is(const uint8_t *b, size_t n)
{
   return n >= 4 && b[0] == DISC_MAGIC0 && b[1] == DISC_MAGIC1 &&
          b[2] == DISC_MAGIC2 && b[3] == DISC_MAGIC3;
}

/* Validate a discovery datagram.  Returns its type, or 0 if it is not one
 * we can read (bad length, CRC, version 0, unknown type, too short for its
 * type). */
static int disc_check(const uint8_t *b, size_t n)
{
   size_t need;
   if (!disc_is(b, n) || n < DISC_PROBE_LEN || n > DISC_MAX_LEN)
      return 0;
   if (b[4] < 1 || get16(b + 6) != n)
      return 0;
   if (disc_crc16(b, n - 2) != get16(b + n - 2))
      return 0;
   switch (b[5])
   {
      case DISC_PROBE:
      case DISC_ECHO:
      case DISC_ECHO_REPLY:
      case DISC_KEEP: need = DISC_PROBE_LEN; break;
      case DISC_GONE:     need = DISC_GONE_LEN; break;
      case DISC_ANNOUNCE: need = DISC_ANNOUNCE_LEN; break;
      case DISC_PUNCH:
      case DISC_PUNCH_ACK: need = LAN_PUNCH_LEN; break;
      default:            return 0;
   }
   return n >= need ? b[5] : 0;
}

static void disc_parse_announce(const uint8_t *b, lan_room_info *out,
                                uint32_t *room_id, uint16_t *port)
{
   memset(out, 0, sizeof(*out));
   out->compat = get32(b + 12);
   *room_id = get32(b + 16);
   *port = get16(b + 20);
   out->kind = b[22];
   out->players = b[23];
   out->max_players = b[24];
   out->flags = b[25];
   out->number = b[26];
   get_text(out->name, sizeof(out->name), b + 28, 16);
   get_text(out->nick, sizeof(out->nick), b + 44, 16);
   get_text(out->title, sizeof(out->title), b + 60, 20);
   get_text(out->code, sizeof(out->code), b + 80, 4);
}

/* ---------------------------------------------------------------- transport -- */

/* One session peer as the I/O thread sees it (under mx). */
typedef struct lan_heard
{
   struct sockaddr_in a;
   uint64_t rx_us;                 /* nd_now_us() of its last datagram */
   uint64_t keep_us;               /* ...and of our last keepalive to it */
   int      used;
} lan_heard;

typedef struct lan_delayed
{
   uint64_t due_us;
   struct sockaddr_in to;
   uint16_t len;
   uint8_t  data[LAN_BULK_MAX];
} lan_delayed;

struct lan_transport
{
   int      fd;
   int      mode;
   unsigned flags;
   uint16_t port;                  /* bound, host order */
   uint16_t disc_port;
   uint32_t ip_be, mask_be;
   struct sockaddr_in bcast[3];
   int      bcast_n;
   struct sockaddr_in target;      /* JOIN: the host we were pointed at */
   int      have_target;

   /* ---- emulation thread only ---- */
   struct { struct sockaddr_in a; uint32_t used_at; } book[LAN_BOOK_CAP];
   int      book_n;
   uint32_t book_clock;
   int      nat_host;               /* the host's entry was rewritten     */
   uint8_t  self_seen[6];           /* our address as the host lists it   */
   int      have_self_seen;
   int      loss_pct, dup_pct, latency_ms, jitter_ms;
   uint32_t prng;

   /* ---- I/O thread -> emulation thread ---- */
   nd_ring  rx;                    /* netdrv frames */
   nd_ring  bulk;                  /* bulk lane */
   int      bulk_any;              /* bulk from any address (emulation thread) */
   atomic_int bulk_on;
   atomic_uint_fast32_t bulk_magic;

   /* ---- I/O thread ---- */
   nd_thread th;
   atomic_int run;
   uint8_t  scratch[LAN_RX_SCRATCH];
   uint32_t reply_tokens;
   uint64_t reply_refill_us;

   /* ---- shared under mx (rare) ---- */
   nd_mutex mx;
   lan_heard heard[LAN_BOOK_CAP];
   uint32_t keep_cookie;
   atomic_int io_keep;
   uint32_t cen_tx_prev;           /* census baselines (caller's thread) */
   uint32_t cen_in_prev, cen_keepin_prev, cen_keepout_prev;
   int      announcing;
   lan_room_info info;
   uint32_t room_id;
   uint64_t next_beacon_us;
   atomic_int fault_delay;         /* a delay is configured: tick at 1 ms */
   lan_delayed *dq;
   int      dq_n;
   /* echo (lan_transport_echo): the last eight requests' send times by
    * cookie & 7, and what came back */
   uint32_t echo_cookie;
   uint64_t echo_sent_us[8];
   uint32_t echo_n, echo_min_us, echo_max_us, echo_last_us;

   /* ---- NAT layer (2.0).  The I/O thread runs the state machines; the API
    * calls from other threads and the I/O thread both hold `nx` while they
    * touch anything below it.  Lock order: nx, then mx (never the other way).
    * Nothing under nx blocks: the senders are non-blocking sendto()s. ---- */
   nd_mutex nx;
   atomic_int nat_on;               /* any machine started: tick them */
   atomic_int nat_srcs;             /* STUN / TURN server addresses in use (0 = the receive loop skips them) */
   uint32_t sim_ip_be;              /* the NAT simulator (tests; never on a console) */
   uint16_t sim_port;
   uint8_t  sim_id;
   uint32_t xs[4];                  /* xorshift128, seeded by lan_transport_nat_seed */
   int      seeded;
   /* STUN */
   nat_stun stun, skeep;            /* the lookup; the mapping keep-alive (server 0 only) */
   int      stun_on;                /* the I/O thread listens for STUN answers: a lookup runs, or a keep-alive does */
   int      stun_used;              /* a lookup was started: the status reports its result (stays after stun_on is cleared) */
   int      pub_valid;
   uint32_t pub_ip_be;
   uint16_t pub_port;
   unsigned keep_ms;
   uint64_t keep_next_ms;
   /* punch */
   int      punch_armed, punch_state, peer_set, peer_lan;
   uint8_t  punch_key[8];
   uint32_t cand_ip[LAN_PUNCH_MAX_CAND];
   uint16_t cand_port[LAN_PUNCH_MAX_CAND];
   int      ncand;
   uint64_t punch_t0_ms, punch_end_ms, punch_next_ms;
   uint32_t punch_cookie, punch_sent;      /* this round's; rounds sent */
   uint32_t peer_ip_be;
   uint16_t peer_port;
   /* relay */
   nat_turn turn;
   int      turn_on, turn_heard, turn_idx, turn_nports, bind_pending;
   uint16_t turn_ports[LAN_RELAY_PORTS_MAX];
   uint32_t turn_ip_be;
   char     turn_user[160], turn_pass[160];
   uint64_t turn_port_t0_ms;
   uint32_t bind_ip_be;
   uint16_t bind_port;
   /* what lan_raw_send reads without a lock (one 64-bit word each, so a
    * reader never sees half of an address): bit 49 relay engaged for this
    * peer, bit 48 READY, bits 47..16 ip (as sin_addr.s_addr), 15..0 port */
   atomic_uint_fast64_t relay_peer_w, relay_srv_w;

   struct
   {
      nd_ctr stun_tx, stun_rx, punch_tx, punch_rx, ack_rx, relay_tx, relay_rx, nat_foreign;
      nd_ctr tx_pkts, tx_would_block, tx_fail, tx_us, tx_max_us, bcast_pkts;
      nd_ctr rx_pkts, rx_ring_drop, rx_oversize, rx_foreign, rx_err;
      nd_ctr bulk_tx, bulk_txfail, bulk_rx, bulk_drop, bulk_foreign;
      nd_ctr disc_probes_rx, disc_replies_tx, disc_beacons_tx, disc_limited;
      nd_ctr fault_dropped, fault_delayed, fault_dup;
      nd_ctr io_wakeups, io_poll_err, nat_aliases;
      nd_ctr peer_in, keep_in, keep_out, gap_max_us;
   } st;
};

static void lan_io_send(lan_transport *t, const struct sockaddr_in *to,
                        const void *buf, size_t len);

static uint32_t lan_rand(lan_transport *t)
{
   uint32_t x = t->prng ? t->prng : 0x9E3779B9u;
   x ^= x << 13; x ^= x >> 17; x ^= x << 5;
   t->prng = x;
   return x;
}

/* The relay's word for an address (see relay_peer_w). */
#define RELAY_W_ENGAGED (1ull << 49)
#define RELAY_W_READY   (1ull << 48)
static uint64_t relay_word(const struct sockaddr_in *a)
{
   return ((uint64_t)a->sin_addr.s_addr << 16) | ntohs(a->sin_port);
}

/* A datagram onto the wire: the simulator's wrapper when a test asked for
 * it (1.8: FE | id | dst ip[4] | dst port[2] | payload to the simulator),
 * else straight to `to`.  Any thread; no lock.  0 = sent or dropped
 * (loss is the layers' above to repair), -1 = hard error. */
static int lan_wire_send(lan_transport *t, const struct sockaddr_in *to,
                         const void *buf, size_t len)
{
   ssize_t n;
#ifndef VITA
   if (t->sim_port)
   {
      uint8_t w[8 + LAN_RX_SCRATCH];
      struct sockaddr_in sim;
      if (len > LAN_RX_SCRATCH)
         return -1;
      w[0] = 0xFE;
      w[1] = t->sim_id;
      memcpy(w + 2, &to->sin_addr.s_addr, 4);
      memcpy(w + 6, &to->sin_port, 2);
      memcpy(w + 8, buf, len);
      sa_make(&sim, t->sim_ip_be, t->sim_port);
      n = sendto(t->fd, w, len + 8, MSG_DONTWAIT, (const struct sockaddr *)&sim, sizeof(sim));
   }
   else
#endif
      n = sendto(t->fd, buf, len, MSG_DONTWAIT, (const struct sockaddr *)to, sizeof(*to));
   if (n >= 0)
   {
      nd_ctr_add(&t->st.tx_pkts, 1);
      return 0;
   }
   if (lan_would_block())
   {
      nd_ctr_add(&t->st.tx_would_block, 1);
      return 0;
   }
   nd_ctr_add(&t->st.tx_fail, 1);
   return -1;
}

/* One datagram out, now.  Any thread.  With a relay engaged for the
 * destination (the peer's relayed address) the datagram is wrapped as
 * ChannelData and goes to the TURN server instead; until the channel is
 * bound it is dropped (netdrv and the session resend).  The emulation
 * thread's whole cost when no relay is in use: this one atomic load. */
static int lan_raw_send(lan_transport *t, const struct sockaddr_in *to,
                        const void *buf, size_t len)
{
   uint64_t pw = atomic_load_explicit(&t->relay_peer_w, memory_order_acquire);
   if (pw && (pw & 0xFFFFFFFFFFFFull) == relay_word(to))
   {
      uint8_t w[4 + LAN_RX_SCRATCH];
      uint64_t sw;
      struct sockaddr_in srv;
      size_t n;
      if (!(pw & RELAY_W_READY))
         return 0;
      sw = atomic_load_explicit(&t->relay_srv_w, memory_order_acquire);
      n = nat_turn_channel_wrap(w, sizeof w, NAT_TURN_CHANNEL, buf, len);
      if (!sw || !n)
         return -1;
      sa_make(&srv, htonl(0), 0);
      srv.sin_addr.s_addr = (uint32_t)((sw >> 16) & 0xFFFFFFFFu);
      srv.sin_port = htons((uint16_t)(sw & 0xFFFF));
      nd_ctr_add(&t->st.relay_tx, 1);
      return lan_wire_send(t, &srv, w, n);
   }
   return lan_wire_send(t, to, buf, len);
}

/* The emulation thread's send: through the fault shim when one is set,
 * else straight to the socket (timed: the one cost of this transport that
 * the emulation thread pays and only hardware can price). */
static int lan_send(lan_transport *t, const struct sockaddr_in *to,
                    const void *buf, size_t len)
{
   int copies = 1, rc = 0;

   if (!(t->loss_pct | t->dup_pct | t->latency_ms | t->jitter_ms))
   {
      uint64_t t0 = nd_now_us();
      uint32_t d;
      rc = lan_raw_send(t, to, buf, len);
      d = (uint32_t)(nd_now_us() - t0);
      nd_ctr_add(&t->st.tx_us, d);
      nd_ctr_max(&t->st.tx_max_us, d);
      return rc;
   }

   if (t->loss_pct && (int)(lan_rand(t) % 100) < t->loss_pct)
   {
      nd_ctr_add(&t->st.fault_dropped, 1);
      return 0;
   }
   if (t->dup_pct && (int)(lan_rand(t) % 100) < t->dup_pct)
   {
      copies = 2;
      nd_ctr_add(&t->st.fault_dup, 1);
   }
   while (copies--)
   {
      uint32_t delay_ms = (uint32_t)t->latency_ms +
         (t->jitter_ms ? lan_rand(t) % (uint32_t)(t->jitter_ms + 1) : 0);
      int queued = 0;
      if (delay_ms && len <= LAN_BULK_MAX)
      {
         nd_mutex_lock(&t->mx);
         if (t->dq && t->dq_n < LAN_DELAYQ_CAP)
         {
            lan_delayed *d = &t->dq[t->dq_n++];
            d->due_us = nd_now_us() + (uint64_t)delay_ms * 1000ull;
            d->to = *to;
            d->len = (uint16_t)len;
            memcpy(d->data, buf, len);
            queued = 1;
         }
         nd_mutex_unlock(&t->mx);
      }
      if (queued)
         nd_ctr_add(&t->st.fault_delayed, 1);
      else
         rc = lan_raw_send(t, to, buf, len);     /* no delay, or queue full */
   }
   return rc;
}

/* I/O thread (and destroy, with force): release delayed datagrams. */
static void lan_dq_flush(lan_transport *t, uint64_t now, int force)
{
   int i = 0;
   nd_mutex_lock(&t->mx);
   while (i < t->dq_n)
   {
      if (force || t->dq[i].due_us <= now)
      {
         lan_raw_send(t, &t->dq[i].to, t->dq[i].data, t->dq[i].len);
         t->dq[i] = t->dq[--t->dq_n];           /* order is by due time */
      }
      else
         i++;
   }
   nd_mutex_unlock(&t->mx);
}

/* ---- address book (emulation thread) -------------------------------------
 * Who netdrv's broadcast() fans out to: every address a netdrv frame came
 * from or was sent to.  Least-recently-used entry is replaced when full. */
static int book_find(const lan_transport *t, const struct sockaddr_in *a)
{
   int i;
   for (i = 0; i < t->book_n; i++)
      if (sa_eq(&t->book[i].a, a))
         return i;
   return -1;
}

static void book_touch(lan_transport *t, const struct sockaddr_in *a)
{
   int i = book_find(t, a), lru = 0;
   if (i < 0)
   {
      if (t->book_n < LAN_BOOK_CAP)
         i = t->book_n++;
      else
      {
         for (i = 1; i < t->book_n; i++)
            if (t->book[i].used_at < t->book[lru].used_at)
               lru = i;
         i = lru;
      }
      t->book[i].a = *a;
   }
   t->book[i].used_at = ++t->book_clock;
}

/* ---- who is alive: the I/O thread's table of session peers --------------
 * A peer is an address we received a netdrv frame, a bulk datagram or a
 * keepalive from (discovery does not count: a scanner is not a session),
 * or the host a joiner was pointed at.  Under mx; any thread. */
static void heard_touch(lan_transport *t, const struct sockaddr_in *a,
                        uint64_t now)
{
   int i, free_i = -1, old_i = 0;
   nd_mutex_lock(&t->mx);
   for (i = 0; i < LAN_BOOK_CAP; i++)
   {
      if (!t->heard[i].used)
      {
         if (free_i < 0)
            free_i = i;
         continue;
      }
      if (sa_eq(&t->heard[i].a, a))
      {
         uint64_t gap = now - t->heard[i].rx_us;
         t->heard[i].rx_us = now;
         nd_mutex_unlock(&t->mx);
         nd_ctr_max(&t->st.gap_max_us, gap > 0xFFFFFFFFull ? 0xFFFFFFFFu
                                                           : (uint32_t)gap);
         return;
      }
      if (t->heard[i].rx_us < t->heard[old_i].rx_us)
         old_i = i;
   }
   i = free_i >= 0 ? free_i : old_i;
   t->heard[i].a = *a;
   t->heard[i].rx_us = now;
   t->heard[i].keep_us = now;       /* the first keepalive is a second away */
   t->heard[i].used = 1;
   nd_mutex_unlock(&t->mx);
}

/* nd_transport.heard_age_us: microseconds since this peer was last heard. */
static uint64_t lan_heard_age(void *ctx, const uint8_t mac[6])
{
   lan_transport *t = (lan_transport *)ctx;
   struct sockaddr_in a;
   uint64_t age = UINT64_MAX, now = nd_now_us();
   int i;
   sa_from_mac(mac, &a);
   nd_mutex_lock(&t->mx);
   for (i = 0; i < LAN_BOOK_CAP; i++)
      if (t->heard[i].used && sa_eq(&t->heard[i].a, &a))
         age = now - t->heard[i].rx_us;
   nd_mutex_unlock(&t->mx);
   return age;
}

/* I/O thread: the keepalives that are due. */
static void lan_io_keepalives(lan_transport *t, uint64_t now)
{
   uint8_t pkt[DISC_PROBE_LEN];
   struct sockaddr_in to[LAN_BOOK_CAP];
   int i, n = 0;
   size_t len;

   nd_mutex_lock(&t->mx);
   for (i = 0; i < LAN_BOOK_CAP; i++)
   {
      lan_heard *h = &t->heard[i];
      if (!h->used)
         continue;
      if (now - h->rx_us >= LAN_HEARD_TTL_US)
      {
         h->used = 0;               /* gone for a minute: forget it */
         continue;
      }
      if (now - h->keep_us >= LAN_KEEP_US)
      {
         h->keep_us = now;
         to[n++] = h->a;
      }
   }
   len = disc_build_echo(pkt, DISC_KEEP, ++t->keep_cookie);
   nd_mutex_unlock(&t->mx);
   for (i = 0; i < n; i++)
   {
      lan_io_send(t, &to[i], pkt, len);
      nd_ctr_add(&t->st.keep_out, 1);
   }
}

/* ---- NAT translation (emulation thread; see the top of the file) --------- */

static int frame_crc_ok(const uint8_t *f, int n)
{
   uint8_t hdr[ND_HDR_SIZE];
   uint16_t crc;
   memcpy(hdr, f, ND_HDR_SIZE);
   hdr[14] = hdr[15] = 0;
   crc = nd_crc16(0xFFFF, hdr, ND_HDR_SIZE);
   crc = nd_crc16(crc, f + ND_HDR_SIZE, (size_t)(n - ND_HDR_SIZE));
   return crc == (uint16_t)(f[14] | (f[15] << 8));
}

static void frame_crc_set(uint8_t *f, int n)
{
   uint16_t crc;
   f[14] = f[15] = 0;
   crc = nd_crc16(0xFFFF, f, (size_t)n);
   f[14] = (uint8_t)crc;
   f[15] = (uint8_t)(crc >> 8);
}

/* A WELCOME or ROSTER from the host: its own entry becomes the address the
 * datagram came from, and ours (as the host sees us) our own address.  Only
 * the joining side does this, with a direct target only for frames from
 * that target, and only for a frame that is intact (a damaged one is left
 * for netdrv's wall to drop, not given a new CRC). */
static void nat_rewrite(lan_transport *t, const struct sockaddr_in *from,
                        uint8_t *f, int n)
{
   uint8_t observed[6], local[6], *pl, *e;
   int i, count, welcome, changed = 0;

   if (t->mode != LAN_MODE_JOIN || n < ND_HDR_SIZE + 3 + ND_ROSTER_ENTRY_SIZE)
      return;
   if (f[0] != 0x31 || f[1] != 0x4E || f[2] != 0x50 || f[3] != 0x47 ||
       f[4] != ND_WIRE_VER)
      return;                            /* 'GPN1' little-endian, version */
   if (f[5] != ND_T_WELCOME && f[5] != ND_T_ROSTER)
      return;
   if (t->have_target && !sa_eq(from, &t->target))
      return;
   pl = f + ND_HDR_SIZE;
   count = pl[2];
   if (count < 1 || count > ND_MAX_CLIENTS ||
       n != ND_HDR_SIZE + 3 + count * ND_ROSTER_ENTRY_SIZE ||
       (int)(f[12] | (f[13] << 8)) != n - ND_HDR_SIZE || !frame_crc_ok(f, n))
      return;
   welcome = f[5] == ND_T_WELCOME;
   mac_from_sa(from, observed);
   lan_mac_from_addr(t->ip_be, t->port, local);
   for (i = 0, e = pl + 3; i < count; i++, e += ND_ROSTER_ENTRY_SIZE)
   {
      if (e[0] == f[6])                  /* the sender's own entry */
      {
         if (memcmp(e + 1, observed, 6))
         {
            memcpy(e + 1, observed, 6);
            changed = 1;
            if (!t->nat_host)
            {
               t->nat_host = 1;
               nd_ctr_add(&t->st.nat_aliases, 1);
            }
         }
      }
      else if (welcome && e[0] == pl[0])  /* "your_id": us */
      {
         memcpy(t->self_seen, e + 1, 6);
         t->have_self_seen = 1;
         if (memcmp(e + 1, local, 6))
         {
            memcpy(e + 1, local, 6);
            changed = 1;
         }
      }
      else if (!welcome && t->have_self_seen &&
               !memcmp(e + 1, t->self_seen, 6) && memcmp(e + 1, local, 6))
      {
         memcpy(e + 1, local, 6);
         changed = 1;
      }
   }
   if (changed)
      frame_crc_set(f, n);
}

/* ---- nd_transport vtable (emulation thread) ------------------------------ */

static void lan_resolve(const lan_transport *t, const uint8_t mac[6],
                        struct sockaddr_in *to)
{
   (void)t;
   sa_from_mac(mac, to);
}

static int lan_send_to(void *ctx, const uint8_t mac[6],
                       const void *buf, size_t len)
{
   lan_transport *t = (lan_transport *)ctx;
   struct sockaddr_in to;
   lan_resolve(t, mac, &to);
   book_touch(t, &to);
   return lan_send(t, &to, buf, len);
}

static int lan_broadcast(void *ctx, const void *buf, size_t len)
{
   lan_transport *t = (lan_transport *)ctx;
   int i, p;

   for (i = 0; i < t->book_n; i++)
   {
      lan_send(t, &t->book[i].a, buf, len);
      nd_ctr_add(&t->st.bcast_pkts, 1);
   }
   /* A joiner that was not pointed at a host has nobody in its book until
    * one answers: its JOIN goes to the whole subnet, on every port a host
    * may have fallen back to.  (netdrv only broadcasts JOIN while joining;
    * once a host answered, the book carries everything.) */
   if (t->mode == LAN_MODE_JOIN && !t->have_target && !t->book_n)
   {
      for (i = 0; i < t->bcast_n; i++)
         for (p = 0; p < LAN_PORT_TRIES; p++)
         {
            struct sockaddr_in to = t->bcast[i];
            to.sin_port = htons((uint16_t)(t->disc_port + p));
            lan_send(t, &to, buf, len);
            nd_ctr_add(&t->st.bcast_pkts, 1);
         }
   }
   return 0;
}

static int lan_recv(void *ctx, uint8_t src_mac[6], void *buf, size_t cap)
{
   lan_transport *t = (lan_transport *)ctx;
   const nd_ring_slot *s = nd_ring_peek(&t->rx);
   struct sockaddr_in from;
   int len;

   if (!s)
      return 0;
   len = s->len;
   if ((size_t)len > cap)
      len = (int)cap;                    /* cap >= ND_MAX_FRAME per contract */
   memcpy(buf, s->data, (size_t)len);
   sa_from_mac(s->mac, &from);
   nd_ring_drop(&t->rx);

   book_touch(t, &from);
   nat_rewrite(t, &from, (uint8_t *)buf, len);
   mac_from_sa(&from, src_mac);
   return len;
}

static int lan_pending(void *ctx)
{
   return nd_ring_nonempty(&((lan_transport *)ctx)->rx);
}

static void lan_local_addr(void *ctx, uint8_t mac[6])
{
   lan_transport *t = (lan_transport *)ctx;
   lan_mac_from_addr(t->ip_be, t->port, mac);
}

void lan_transport_iface(lan_transport *t, nd_transport *out)
{
   memset(out, 0, sizeof(*out));         /* optional hooks must read NULL */
   out->ctx        = t;
   out->send_to    = lan_send_to;
   out->broadcast  = lan_broadcast;
   out->recv       = lan_recv;
   out->local_addr = lan_local_addr;
   out->pending    = lan_pending;
   out->heard_age_us = lan_heard_age;
}

/* ---- bulk lane ------------------------------------------------------------ */

int lan_transport_bulk_open(lan_transport *t, unsigned slots,
                            const char magic[4])
{
   uint32_t m;
   if (!t || !slots)
      return -1;
   if (!t->bulk.mem && nd_ring_init(&t->bulk, slots, LAN_BULK_MAX) != 0)
      return -1;
   memcpy(&m, magic, 4);
   atomic_store_explicit(&t->bulk_magic, m, memory_order_relaxed);
   nd_ring_flush(&t->bulk);
   /* The I/O thread reads bulk_on with acquire: it sees the ring and the
    * magic that were written before this store. */
   atomic_store_explicit(&t->bulk_on, 1, memory_order_release);
   return 0;
}

void lan_transport_bulk_close(lan_transport *t)
{
   /* The ring stays (the I/O thread may be mid-copy); destroy frees it. */
   if (t)
      atomic_store_explicit(&t->bulk_on, 0, memory_order_release);
}

int lan_transport_bulk_send(lan_transport *t, const uint8_t mac[6],
                            const void *buf, size_t len)
{
   struct sockaddr_in to;
   if (!t || len > LAN_BULK_MAX || len < 4)
      return -1;
   lan_resolve(t, mac, &to);
   if (lan_send(t, &to, buf, len) != 0)
   {
      nd_ctr_add(&t->st.bulk_txfail, 1);
      return 0;                          /* lost: the sender resends */
   }
   nd_ctr_add(&t->st.bulk_tx, 1);
   return 0;
}

static int bulk_recv_impl(lan_transport *t, uint8_t src_mac[6], void *buf, size_t cap)
{
   if (!t)
      return 0;
   for (;;)
   {
      const nd_ring_slot *s = nd_ring_peek(&t->bulk);
      struct sockaddr_in from;
      int len;
      if (!s)
         return 0;
      sa_from_mac(s->mac, &from);
      if (!t->bulk_any && book_find(t, &from) < 0)
      {
         /* On the internet anybody can send to this port.  The bulk lane
          * has no handshake of its own, so it takes datagrams only from an
          * address the netdrv session is talking to. */
         nd_ctr_add(&t->st.bulk_foreign, 1);
         nd_ring_drop(&t->bulk);
         continue;
      }
      len = s->len;
      if ((size_t)len > cap)
         len = (int)cap;
      memcpy(buf, s->data, (size_t)len);
      if (src_mac)
         memcpy(src_mac, s->mac, 6);
      nd_ring_drop(&t->bulk);
      return len;
   }
}

int lan_transport_bulk_recv(lan_transport *t, void *buf, size_t cap)
{
   return bulk_recv_impl(t, NULL, buf, cap);
}

/* ... plus where it came from (a relayed peer's source is its relayed
 * address, the one netdrv names it by). */
int lan_transport_bulk_recv_from(lan_transport *t, uint8_t src_mac[6], void *buf, size_t cap)
{
   if (src_mac)
      memset(src_mac, 0, 6);
   return bulk_recv_impl(t, src_mac, buf, cap);
}

void lan_transport_bulk_any(lan_transport *t, int on)
{
   if (t)
      t->bulk_any = on != 0;
}

unsigned lan_transport_rx_depth(const lan_transport *t)
{
   return t ? nd_ring_depth(&t->rx) : 0u;
}

unsigned lan_transport_bulk_depth(const lan_transport *t)
{
   return t && t->bulk.mem ? nd_ring_depth(&t->bulk) : 0u;
}

/* ---- I/O thread ------------------------------------------------------------ */

/* A datagram the I/O thread itself originates (an echo's answer), held back
 * by the fault shim's latency when one is set, so a measured round trip
 * sees the same link the session will.  No loss, no jitter: the shim's
 * generator belongs to the emulation thread. */
static void lan_io_send(lan_transport *t, const struct sockaddr_in *to,
                        const void *buf, size_t len)
{
   int latency = t->latency_ms, queued = 0;
   if (latency > 0 && len <= LAN_BULK_MAX)
   {
      nd_mutex_lock(&t->mx);
      if (t->dq && t->dq_n < LAN_DELAYQ_CAP)
      {
         lan_delayed *d = &t->dq[t->dq_n++];
         d->due_us = nd_now_us() + (uint64_t)latency * 1000ull;
         d->to = *to;
         d->len = (uint16_t)len;
         memcpy(d->data, buf, len);
         queued = 1;
      }
      nd_mutex_unlock(&t->mx);
   }
   if (!queued)
      lan_raw_send(t, to, buf, len);
}

/* Answer a probe / send the beacon.  cookie 0 = beacon. */
static void lan_io_announce(lan_transport *t, const struct sockaddr_in *to,
                            uint32_t cookie)
{
   uint8_t pkt[DISC_ANNOUNCE_LEN];
   size_t n = 0;
   nd_mutex_lock(&t->mx);
   if (t->announcing)
      n = disc_build_announce(pkt, cookie, &t->info, t->room_id, t->port);
   nd_mutex_unlock(&t->mx);
   if (!n)
      return;
   if (to)
   {
      lan_raw_send(t, to, pkt, n);
      nd_ctr_add(&t->st.disc_replies_tx, 1);
   }
   else
   {
      int i;
      for (i = 0; i < t->bcast_n; i++)
      {
         struct sockaddr_in b = t->bcast[i];
         b.sin_port = htons((uint16_t)(t->disc_port - 1));
         lan_raw_send(t, &b, pkt, n);
      }
      nd_ctr_add(&t->st.disc_beacons_tx, 1);
   }
}

static void lan_io_route(lan_transport *t, const struct sockaddr_in *from,
                         const uint8_t *b, int n, uint64_t now);

/* ---- NAT layer: STUN, punch, TURN (online play, 2.0) -----------------------
 *
 * Everything here runs on the transport's own socket, in the I/O thread.
 * The pure modules (nat_stun.c, nat_turn.c) hold the message codecs and the
 * retransmission schedules; this part gives them a socket, a clock, a
 * source address check and the punch race.  Commands from other threads
 * (lan_transport_stun_start and friends, at the end of this file) and the
 * I/O thread's ticks both hold `nx`; the senders are non-blocking.
 *
 * Who is a STUN / TURN message?  Routing is by SOURCE first (1.7): a
 * datagram from an address we asked STUN or TURN of is read as one of
 * theirs, anything else goes to lan_io_route as before.  'GPN1' frames
 * start with 0x31 (top bits 00, STUN-like) and "GBLD" with 0x47 (the
 * ChannelData range), but neither ever comes from a STUN / TURN server, so
 * there is nothing to mistake.  The emulation thread's cost when no relay
 * is in use is one atomic load in lan_raw_send; the receive loop's is one
 * in the I/O thread.
 *
 * The relay's data path.  Out: lan_raw_send wraps a datagram for the peer's
 * relayed address as ChannelData and sends it to the TURN server.  In:
 * ChannelData (or a Data indication) from the server is unwrapped and
 * handed to lan_io_route with the peer's relayed address as its source, so
 * to netdrv, the bulk lane, the echo and the keepalive the peer simply IS
 * that address. */

static uint32_t nat_rand(lan_transport *t)
{
   uint32_t x = t->xs[0] ^ (t->xs[0] << 11);
   t->xs[0] = t->xs[1]; t->xs[1] = t->xs[2]; t->xs[2] = t->xs[3];
   t->xs[3] = t->xs[3] ^ (t->xs[3] >> 19) ^ x ^ (x >> 8);
   return t->xs[3];
}

static void nat_rand_bytes(lan_transport *t, uint8_t *out, size_t n)
{
   size_t i;
   for (i = 0; i < n; i += 4)
   {
      uint32_t r = nat_rand(t);
      size_t k;
      for (k = 0; k < 4 && i + k < n; k++)
         out[i + k] = (uint8_t)(r >> (8 * k));
   }
}

static int nat_cb_stun_send(void *ctx, uint32_t ip_be, uint16_t port, const void *buf, size_t len)
{
   lan_transport *t = (lan_transport *)ctx;
   struct sockaddr_in to;
   sa_make(&to, ip_be, port);
   nd_ctr_add(&t->st.stun_tx, 1);
   return lan_wire_send(t, &to, buf, len);
}

static int nat_cb_turn_send(void *ctx, uint32_t ip_be, uint16_t port, const void *buf, size_t len)
{
   lan_transport *t = (lan_transport *)ctx;
   struct sockaddr_in to;
   sa_make(&to, ip_be, port);
   return lan_wire_send(t, &to, buf, len);
}

/* How many server addresses the receive loop has to check. (nx held) */
static void nat_refresh_srcs(lan_transport *t)
{
   int n = (t->stun_on ? t->stun.n : 0) + (t->turn_on ? 1 : 0);
   atomic_store_explicit(&t->nat_srcs, n, memory_order_release);
}

/* What lan_raw_send reads.  Engaged from the moment a peer is given to
 * relay_bind (datagrams for it are dropped, not sent in the clear to the
 * server's address), READY when the channel is bound.  (nx held) */
static void nat_publish_relay(lan_transport *t)
{
   uint64_t pw = 0, sw = 0;
   if (t->turn_on)
   {
      int st = nat_turn_state(&t->turn);
      struct sockaddr_in a;
      sa_make(&a, t->turn_ip_be, t->turn_ports[t->turn_idx]);
      sw = relay_word(&a);
      if (t->bind_pending && st != LAN_RELAY_FAILED && st != LAN_RELAY_OFF)
      {
         sa_make(&a, t->bind_ip_be, t->bind_port);
         pw = RELAY_W_ENGAGED | relay_word(&a) | (st == LAN_RELAY_READY ? RELAY_W_READY : 0);
      }
   }
   atomic_store_explicit(&t->relay_srv_w, sw, memory_order_release);
   atomic_store_explicit(&t->relay_peer_w, pw, memory_order_release);
}

static int nat_in_subnet(const lan_transport *t, uint32_t ip_be)
{
   return t->mask_be && t->ip_be && (ip_be & t->mask_be) == (t->ip_be & t->mask_be);
}

/* The lookup has answered: remember our public address.  (nx held) */
static void nat_stun_sync(lan_transport *t, uint64_t now_ms)
{
   if (t->stun_on && !t->pub_valid && nat_stun_state(&t->stun) == NAT_STUN_OK &&
       nat_stun_result(&t->stun, &t->pub_ip_be, &t->pub_port, NULL))
   {
      t->pub_valid = 1;
      if (t->keep_ms)
         t->keep_next_ms = now_ms + t->keep_ms;
   }
}

static void nat_punch_send_to(lan_transport *t, uint32_t ip_be, uint16_t port)
{
   uint8_t pkt[LAN_PUNCH_LEN];
   struct sockaddr_in to;
   size_t n = disc_build_punch(pkt, DISC_PUNCH, t->punch_cookie, t->punch_key);
   sa_make(&to, ip_be, port);
   lan_wire_send(t, &to, pkt, n);
   nd_ctr_add(&t->st.punch_tx, 1);
}

/* One round of PUNCH to every candidate; one cookie per round, so an ACK
 * is valid for any of the last eight rounds.  (nx held) */
static void nat_punch_round(lan_transport *t, uint64_t now_ms)
{
   int i;
   t->punch_cookie++;
   t->punch_sent++;
   for (i = 0; i < t->ncand; i++)
      nat_punch_send_to(t, t->cand_ip[i], t->cand_port[i]);
   t->punch_next_ms = now_ms + (now_ms - t->punch_t0_ms < 2000 ? LAN_PUNCH_FAST_MS : LAN_PUNCH_SLOW_MS);
}

/* A PUNCH or PUNCH_ACK that passed disc_check.  I/O thread. */
static void lan_io_punch_rx(lan_transport *t, const struct sockaddr_in *from,
                            const uint8_t *b, int type, uint64_t now_us)
{
   uint32_t cookie = get32(b + 8);
   uint32_t ip = from->sin_addr.s_addr;
   uint16_t port = ntohs(from->sin_port);

   nd_mutex_lock(&t->nx);
   if (!t->punch_armed || memcmp(b + 16, t->punch_key, 8) != 0)
   {
      nd_mutex_unlock(&t->nx);
      nd_ctr_add(&t->st.nat_foreign, 1);         /* unarmed, or somebody else's room */
      return;
   }
   if (type == DISC_PUNCH)
   {
      nd_ctr_add(&t->st.punch_rx, 1);
      /* Answer under the same cap as probes: an 26-byte PUNCH draws a
       * 26-byte answer to whatever address it claims. */
      if (now_us - t->reply_refill_us >= 1000000ull)
      {
         t->reply_tokens = LAN_REPLY_BURST;
         t->reply_refill_us = now_us;
      }
      if (t->reply_tokens)
      {
         uint8_t ack[LAN_PUNCH_LEN];
         size_t n = disc_build_punch(ack, DISC_PUNCH_ACK, cookie, t->punch_key);
         t->reply_tokens--;
         lan_wire_send(t, from, ack, n);
      }
      else
         nd_ctr_add(&t->st.disc_limited, 1);
      /* Armed and waiting, no race running yet (a host whose poll has not
       * shown the guest): the joiner's first PUNCH starts the race here,
       * with its sender as the only candidate (1.8).  The datagram carries
       * the room's key, which only the two members were given.  Without
       * this the host would only answer, and learn of the guest at its next
       * poll, up to ten seconds later. */
      if (t->punch_state == LAN_PUNCH_ARMED)
      {
         uint64_t now_ms = now_us / 1000u;
         t->ncand = 1;
         t->cand_ip[0] = ip;
         t->cand_port[0] = port;
         t->peer_set = 0;
         t->peer_lan = 0;
         t->punch_sent = 0;
         t->punch_cookie = nat_rand(t);
         t->punch_t0_ms = now_ms;
         t->punch_end_ms = now_ms + LAN_PUNCH_OPENED_MS;
         t->punch_state = LAN_PUNCH_RUNNING;
         nat_punch_round(t, now_ms);
      }
      /* A peer we did not know: it is a candidate now (a NAT that changed
       * the port is followed). */
      else if (t->punch_state == LAN_PUNCH_RUNNING)
      {
         int i, known = 0;
         for (i = 0; i < t->ncand; i++)
            if (t->cand_ip[i] == ip && t->cand_port[i] == port)
               known = 1;
         if (!known && t->ncand < LAN_PUNCH_MAX_CAND)
         {
            t->cand_ip[t->ncand] = ip;
            t->cand_port[t->ncand] = port;
            t->ncand++;
            nat_punch_send_to(t, ip, port);
         }
      }
   }
   else
   {
      uint32_t age = t->punch_cookie - cookie;
      nd_ctr_add(&t->st.ack_rx, 1);
      if (t->punch_state == LAN_PUNCH_RUNNING && age < 8 && age < t->punch_sent)
      {
         /* the first ACK wins; a LAN candidate normally answers first */
         t->punch_state = LAN_PUNCH_DONE;
         t->peer_set = 1;
         t->peer_ip_be = ip;
         t->peer_port = port;
         t->peer_lan = nat_in_subnet(t, ip);
      }
   }
   nd_mutex_unlock(&t->nx);
}

static int nat_is_cookie(const uint8_t *b, size_t n)
{
   return n >= 20 && (b[0] & 0xC0) == 0 && b[4] == 0x21 && b[5] == 0x12 && b[6] == 0xA4 && b[7] == 0x42;
}

/* The receive loop's first step (1.7).  1 = the datagram came from a
 * STUN / TURN server address we use and has been dealt with.  I/O thread. */
static int lan_io_nat_rx(lan_transport *t, const struct sockaddr_in *from,
                         const uint8_t *b, int n, uint64_t now_us)
{
   uint32_t ip = from->sin_addr.s_addr;
   uint16_t port = ntohs(from->sin_port);
   int is_stun = 0, is_turn = 0, i, deliver = 0, consumed = 0;
   const uint8_t *payload = NULL;
   size_t plen = 0;
   struct sockaddr_in peer;

   nd_mutex_lock(&t->nx);
   if (t->stun_on)
      for (i = 0; i < t->stun.n; i++)
         if (t->stun.ip_be[i] == ip && t->stun.port[i] == port)
            is_stun = 1;
   if (t->turn_on && t->turn.server_ip_be == ip && t->turn.server_port == port)
      is_turn = 1;
   if (!is_stun && !is_turn)
   {
      nd_mutex_unlock(&t->nx);
      return 0;
   }
   if (is_turn)
      t->turn_heard = 1;                       /* it is there: the next port is not needed */
   if (is_turn && n >= 4 && (b[0] & 0xC0) == 0x40)
   {
      /* ChannelData: only our one channel, only once a peer is bound */
      int st = nat_turn_state(&t->turn);
      const uint8_t *p = NULL;
      size_t l = 0;
      uint16_t ch = nat_turn_channel_unwrap(b, (size_t)n, &p, &l);
      if (ch == NAT_TURN_CHANNEL && t->bind_pending && (st == LAN_RELAY_BINDING || st == LAN_RELAY_READY))
      {
         deliver = 1;
         payload = p;
         plen = l;
         consumed = 1;
      }
   }
   else if (nat_is_cookie(b, (size_t)n))
   {
      if (is_stun)
      {
         if (nat_stun_handle(&t->stun, ip, port, b, (size_t)n) ||
             nat_stun_handle(&t->skeep, ip, port, b, (size_t)n))
         {
            nd_ctr_add(&t->st.stun_rx, 1);
            nat_stun_sync(t, now_us / 1000u);
            consumed = 1;
         }
      }
      if (!consumed && is_turn)
      {
         const uint8_t *p = NULL;
         size_t l = 0;
         int r = nat_turn_handle(&t->turn, b, (size_t)n, &p, &l);
         if (r == 2)
         {
            deliver = 1;
            payload = p;
            plen = l;
         }
         consumed = r != 0;
      }
   }
   if (!consumed)
      nd_ctr_add(&t->st.nat_foreign, 1);         /* not what a server of ours sends */
   if (deliver)
      sa_make(&peer, t->bind_ip_be, t->bind_port);
   nat_publish_relay(t);
   nd_mutex_unlock(&t->nx);

   if (deliver)
   {
      nd_ctr_add(&t->st.relay_rx, 1);
      lan_io_route(t, &peer, payload, (int)plen, now_us);
   }
   return 1;
}

/* Start (or move on to) the TURN server's port number `idx`.  (nx held) */
static void nat_relay_begin(lan_transport *t, int idx, uint64_t now_ms)
{
   uint8_t seed[16];
   t->turn_idx = idx;
   nat_rand_bytes(t, seed, sizeof seed);
   nat_turn_start(&t->turn, nat_cb_turn_send, t, t->turn_ip_be, t->turn_ports[idx],
                  t->turn_user, t->turn_pass, seed, now_ms);
   t->turn_port_t0_ms = now_ms;
   t->turn_heard = 0;
}

/* The I/O thread's tick of every machine.  Returns 1 while something is in
 * flight (the poll slice then drops to LAN_NAT_WAIT_MS). */
static int lan_io_nat_tick(lan_transport *t, uint64_t now_us)
{
   uint64_t now = now_us / 1000u;
   int busy = 0, i;

   if (!atomic_load_explicit(&t->nat_on, memory_order_relaxed))
      return 0;
   nd_mutex_lock(&t->nx);

   if (t->stun_on)
   {
      nat_stun_tick(&t->stun, now);
      nat_stun_sync(t, now);
      for (i = 0; i < t->stun.n; i++)
         if (!t->stun.got[i] && !t->stun.gave_up[i])
            busy = 1;
      if (t->keep_ms && t->pub_valid && now >= t->keep_next_ms && nat_stun_state(&t->skeep) == NAT_STUN_IDLE)
      {
         uint8_t tx[2][12];
         nat_rand_bytes(t, &tx[0][0], 24);
         nat_stun_start(&t->skeep, nat_cb_stun_send, t, t->stun.ip_be, t->stun.port, 1, (const uint8_t (*)[12])tx, now);
         t->keep_next_ms = now + t->keep_ms;
      }
      if (nat_stun_state(&t->skeep) != NAT_STUN_IDLE)
      {
         uint32_t ip;
         uint16_t port;
         nat_stun_tick(&t->skeep, now);
         if (nat_stun_state(&t->skeep) == NAT_STUN_OK || nat_stun_state(&t->skeep) == NAT_STUN_FAILED)
         {
            /* a changed answer is the new public address (the next poll's
             * description carries it) */
            if (nat_stun_result(&t->skeep, &ip, &port, NULL) && (ip != t->pub_ip_be || port != t->pub_port))
            {
               t->pub_ip_be = ip;
               t->pub_port = port;
            }
            memset(&t->skeep, 0, sizeof(t->skeep));
         }
         else
            busy = 1;
      }
      /* A finished lookup with no keep-alive: stop checking every datagram against the servers (each check takes the mutex). */
      if (!busy && (nat_stun_state(&t->stun) == NAT_STUN_OK || nat_stun_state(&t->stun) == NAT_STUN_FAILED) &&
          !(t->keep_ms && t->pub_valid) && nat_stun_state(&t->skeep) == NAT_STUN_IDLE)
      {
         t->stun_on = 0;
         nat_refresh_srcs(t);
      }
   }

   if (t->punch_state == LAN_PUNCH_RUNNING)
   {
      if (now >= t->punch_end_ms)
         t->punch_state = LAN_PUNCH_TIMEOUT;   /* still armed: it keeps answering */
      else
      {
         if (now >= t->punch_next_ms)
            nat_punch_round(t, now);
         busy = 1;
      }
   }

   if (t->turn_on)
   {
      int st;
      nat_turn_tick(&t->turn, now);
      st = nat_turn_state(&t->turn);
      /* 3 s without ANY answer from this port: try the next one (UDP 3478,
       * then 443: some networks only let the second through) */
      if (!t->turn_heard && t->turn_idx + 1 < t->turn_nports &&
          ((st == LAN_RELAY_ALLOCATING && now - t->turn_port_t0_ms >= LAN_RELAY_PORT_MS) ||
           (st == LAN_RELAY_FAILED && nat_turn_error(&t->turn) == LAN_RELAY_E_TIMEOUT)))
      {
         nat_relay_begin(t, t->turn_idx + 1, now);
         nat_refresh_srcs(t);
         st = nat_turn_state(&t->turn);
      }
      if (st == LAN_RELAY_ALLOCATED && t->bind_pending)
      {
         nat_turn_bind(&t->turn, t->bind_ip_be, t->bind_port, now);
         st = nat_turn_state(&t->turn);
      }
      if (st == LAN_RELAY_ALLOCATING || st == LAN_RELAY_BINDING)
         busy = 1;
      for (i = 0; i < NAT_TURN_TXNS; i++)
         if (t->turn.txn[i].active)
            busy = 1;
   }
   nat_publish_relay(t);
   nd_mutex_unlock(&t->nx);
   return busy;
}

static void lan_io_route(lan_transport *t, const struct sockaddr_in *from,
                         const uint8_t *b, int n, uint64_t now)
{
   uint8_t mac[6];

   if (disc_is(b, (size_t)n))
   {
      /* Discovery stops here: it is never queued for netdrv. */
      int type = disc_check(b, (size_t)n);
      if (type == DISC_ECHO)
      {
         /* Answered here, on the I/O thread, so the round trip the asker
          * measures is the network's and not this console's frame loop.
          * Under the probes' answer cap: 18 bytes for 18, but still to an
          * address the datagram only claims. */
         if (now - t->reply_refill_us >= 1000000ull)
         {
            t->reply_tokens = LAN_REPLY_BURST;
            t->reply_refill_us = now;
         }
         if (t->reply_tokens)
         {
            uint8_t pkt[DISC_PROBE_LEN];
            size_t len = disc_build_echo(pkt, DISC_ECHO_REPLY, get32(b + 8));
            t->reply_tokens--;
            lan_io_send(t, from, pkt, len);
         }
         return;
      }
      if (type == DISC_PUNCH || type == DISC_PUNCH_ACK)
      {
         lan_io_punch_rx(t, from, b, type, now);
         return;
      }
      if (type == DISC_KEEP)
      {
         heard_touch(t, from, now);
         nd_ctr_add(&t->st.keep_in, 1);
         nd_ctr_add(&t->st.peer_in, 1);
         return;
      }
      if (type == DISC_ECHO_REPLY)
      {
         uint32_t cookie = get32(b + 8);
         nd_mutex_lock(&t->mx);
         if (t->echo_sent_us[cookie & 7] && cookie - 1 < t->echo_cookie &&
             t->echo_cookie - cookie < 8)
         {
            uint64_t d = now - t->echo_sent_us[cookie & 7];
            uint32_t us = d > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)d;
            t->echo_sent_us[cookie & 7] = 0;        /* one answer each */
            if (!t->echo_n || us < t->echo_min_us)
               t->echo_min_us = us;
            if (!t->echo_n || us > t->echo_max_us)
               t->echo_max_us = us;
            t->echo_last_us = us;
            t->echo_n++;
         }
         nd_mutex_unlock(&t->mx);
         return;
      }
      if (type == DISC_PROBE &&
          t->mode == LAN_MODE_HOST && !(t->flags & LAN_F_NO_DISCOVERY))
      {
         nd_ctr_add(&t->st.disc_probes_rx, 1);
         /* An 18-byte probe draws an 86-byte answer to whatever source
          * address it claims: cap the answers so a forwarded port cannot be
          * used to aim traffic at somebody else. */
         if (now - t->reply_refill_us >= 1000000ull)
         {
            t->reply_tokens = LAN_REPLY_BURST;
            t->reply_refill_us = now;
         }
         if (t->reply_tokens)
         {
            t->reply_tokens--;
            lan_io_announce(t, from, get32(b + 8));
         }
         else
            nd_ctr_add(&t->st.disc_limited, 1);
      }
      return;
   }

   mac_from_sa(from, mac);

   if (n >= 4 && atomic_load_explicit(&t->bulk_on, memory_order_acquire))
   {
      uint32_t m;
      memcpy(&m, b, 4);
      if (m == (uint32_t)atomic_load_explicit(&t->bulk_magic,
                                              memory_order_relaxed))
      {
         heard_touch(t, from, now);
         nd_ctr_add(&t->st.peer_in, 1);
         if (n <= LAN_BULK_MAX && nd_ring_push(&t->bulk, mac, b, (size_t)n))
            nd_ctr_add(&t->st.bulk_rx, 1);
         else
            nd_ctr_add(&t->st.bulk_drop, 1);   /* full: the sender resends */
         return;
      }
   }

   if (n < 4 || get32(b) != ND_WIRE_MAGIC)
   {
      nd_ctr_add(&t->st.rx_foreign, 1);
      return;
   }
   if (n > ND_MAX_FRAME)
   {
      nd_ctr_add(&t->st.rx_oversize, 1);
      return;
   }
   heard_touch(t, from, now);
   nd_ctr_add(&t->st.peer_in, 1);
   if (nd_ring_push(&t->rx, mac, b, (size_t)n))
      nd_ctr_add(&t->st.rx_pkts, 1);
   else
      nd_ctr_add(&t->st.rx_ring_drop, 1);      /* full: ARQ retransmits */
}

static void lan_io_thread(void *arg)
{
   lan_transport *t = (lan_transport *)arg;

   while (atomic_load_explicit(&t->run, memory_order_acquire))
   {
      uint64_t now = nd_now_us();
      int budget, wait_ms = LAN_IDLE_WAIT_MS, beacon = 0;

      nd_ctr_add(&t->st.io_wakeups, 1);

      /* 1. everything the socket has */
      for (budget = 0; budget < 256; budget++)
      {
         struct sockaddr_in from;
         socklen_t flen = sizeof(from);
         ssize_t n = recvfrom(t->fd, t->scratch, sizeof(t->scratch),
                              MSG_DONTWAIT, (struct sockaddr *)&from, &flen);
         if (n < 0)
         {
            /* ECONNREFUSED: an ICMP port-unreachable for something we sent
             * earlier, reported on the next call.  Not an error of ours. */
            if (!lan_would_block() && errno != ECONNREFUSED && errno != EINTR)
               nd_ctr_add(&t->st.rx_err, 1);
            break;
         }
         if (n == 0 || from.sin_family != AF_INET)
            continue;
#ifndef VITA
         if (t->sim_port)
         {
            /* the simulator's wrapper (1.8): everything comes from it as
             * src ip[4] | src port[2] | payload */
            if (from.sin_addr.s_addr != t->sim_ip_be || from.sin_port != htons(t->sim_port) || n < 6)
               continue;
            sa_make(&from, 0, 0);
            memcpy(&from.sin_addr.s_addr, t->scratch, 4);
            memcpy(&from.sin_port, t->scratch + 4, 2);
            n -= 6;
            memmove(t->scratch, t->scratch + 6, (size_t)n);
         }
#endif
         /* STUN / TURN servers' datagrams first, by source (1.7) */
         if (atomic_load_explicit(&t->nat_srcs, memory_order_acquire) &&
             lan_io_nat_rx(t, &from, t->scratch, (int)n, now))
            continue;
         lan_io_route(t, &from, t->scratch, (int)n, now);
      }

      /* 2. the fault shim's delayed datagrams */
      if (atomic_load_explicit(&t->fault_delay, memory_order_relaxed))
      {
         lan_dq_flush(t, now, 0);
         wait_ms = 1;
      }

      /* 2b. keepalives to session peers: sent HERE, not by the pump, so a
       * main thread that is busy for seconds does not fall silent */
      if (atomic_load_explicit(&t->io_keep, memory_order_relaxed))
         lan_io_keepalives(t, now);

      /* 2c. STUN / punch / TURN timers (a short poll slice while one runs) */
      if (lan_io_nat_tick(t, nd_now_us()) && wait_ms > LAN_NAT_WAIT_MS)
         wait_ms = LAN_NAT_WAIT_MS;

      /* 3. the room beacon */
      nd_mutex_lock(&t->mx);
      if (t->announcing)
      {
         if (now >= t->next_beacon_us)
         {
            beacon = 1;
            t->next_beacon_us = now + LAN_BEACON_US;
         }
         else if ((t->next_beacon_us - now) / 1000u + 1 < (uint64_t)wait_ms)
            wait_ms = (int)((t->next_beacon_us - now) / 1000u) + 1;
      }
      nd_mutex_unlock(&t->mx);
      if (beacon)
         lan_io_announce(t, NULL, 0);

      /* 4. sleep until there is something to read */
      if (budget < 256)
      {
         uint64_t w0 = nd_now_us();
         int rc = lan_wait_readable(t->fd, wait_ms);
         /* HARDWARE: on the Vita poll() is vitasdk newlib's, built on
          * sceNetEpoll*, and has never been run by us.  If it fails, or
          * "times out" long before the timeout it was given, degrade to a
          * 1 ms polling loop instead of spinning a core or going deaf: the
          * transport keeps working at up to 1 ms of added latency, and
          * io_poll_err in the stats says so. */
         if (rc < 0 || (rc == 0 && wait_ms >= 4 &&
                        nd_now_us() - w0 < (uint64_t)wait_ms * 500u))
         {
            nd_ctr_add(&t->st.io_poll_err, 1);
            nd_sleep_us(1000);
         }
      }
   }
}

/* ---- create / destroy ------------------------------------------------------ */

lan_transport *lan_transport_create(const lan_config *cfg)
{
   lan_transport *t;
   uint16_t base, bind_port;
   int tries, i, bound = 0;

   g_lan_err = "";
   if (!cfg || (cfg->mode != LAN_MODE_HOST && cfg->mode != LAN_MODE_JOIN))
   {
      g_lan_err = "invalid config";
      return NULL;
   }
   t = (lan_transport *)calloc(1, sizeof(*t));
   if (!t)
   {
      g_lan_err = "out of memory";
      return NULL;
   }
   t->fd = -1;
   t->mode = cfg->mode;
   t->flags = cfg->flags;
   t->disc_port = cfg->disc_port ? cfg->disc_port : LAN_PORT_DEFAULT;
   t->ip_be = cfg->local_ip_be;
   t->mask_be = cfg->netmask_be;
   t->bcast_n = lan_bcast_list(&t->ip_be, &t->mask_be, t->flags, t->bcast);
   t->prng = 0x243F6A88u;
   atomic_init(&t->run, 0);
   atomic_init(&t->bulk_on, 0);
   atomic_init(&t->bulk_magic, 0);
   atomic_init(&t->fault_delay, 0);
   atomic_init(&t->io_keep, (cfg->flags & LAN_F_IO_KEEPALIVE) ? 1 : 0);
   atomic_init(&t->nat_on, 0);
   atomic_init(&t->nat_srcs, 0);
   atomic_init(&t->relay_peer_w, 0);
   atomic_init(&t->relay_srv_w, 0);
#ifndef VITA
   t->sim_ip_be = cfg->sim_ip_be;
   t->sim_port = cfg->sim_port;
   t->sim_id = cfg->sim_id;
#endif

   if (nd_mutex_init(&t->mx) != 0)
   {
      g_lan_err = "mutex";
      free(t);
      return NULL;
   }
   if (nd_mutex_init(&t->nx) != 0)
   {
      g_lan_err = "mutex";
      nd_mutex_destroy(&t->mx);
      free(t);
      return NULL;
   }
   if (nd_ring_init(&t->rx, cfg->rx_slots ? cfg->rx_slots : LAN_RX_SLOTS,
                    ND_MAX_FRAME) != 0)
   {
      g_lan_err = "out of memory";
      goto fail;
   }

   t->fd = lan_sock_open();
   if (t->fd < 0)
   {
      g_lan_err = "socket (is the network up?)";
      goto fail;
   }
   if (t->sim_port)
   {
      base = 0;                          /* the simulator world has no real ports: any */
      tries = 1;
   }
   else if (cfg->mode == LAN_MODE_HOST)
   {
      base = cfg->port ? cfg->port : t->disc_port;
      tries = cfg->port_tries > 0 ? cfg->port_tries : LAN_PORT_TRIES;
   }
   else
   {
      base = cfg->port;                  /* 0 = any */
      tries = 1;
   }
   for (i = 0; i < tries && !bound; i++)
   {
      bind_port = (uint16_t)(base ? base + i : 0);
      if (lan_sock_bind(t->fd, bind_port, 0) == 0)
         bound = 1;
   }
   if (!bound)
   {
      g_lan_err = "no free port";
      goto fail;
   }
   t->port = lan_sock_port(t->fd);
   if (!t->port)
      t->port = bind_port;               /* getsockname unavailable */
   if (t->sim_port && cfg->sim_lan_port)
      t->port = cfg->sim_lan_port;       /* what this console declares inside the simulated LAN */

   if (cfg->mode == LAN_MODE_JOIN && cfg->target_ip_be)
   {
      sa_make(&t->target, cfg->target_ip_be,
              cfg->target_port ? cfg->target_port : LAN_PORT_DEFAULT);
      t->have_target = 1;
      book_touch(t, &t->target);         /* JOIN goes there, as --join does */
      heard_touch(t, &t->target, nd_now_us());
   }

   /* A room id that two hosts on one LAN will not share. */
   t->room_id = (uint32_t)nd_now_us() * 2654435761u ^
                ((uint32_t)t->port << 16) ^ t->ip_be ^ (uint32_t)(uintptr_t)t;
   if (!t->room_id)
      t->room_id = 1;

   atomic_store_explicit(&t->run, 1, memory_order_release);
   if (nd_thread_start(&t->th, "gbadhoc_lan_io", lan_io_thread, t) != 0)
   {
      g_lan_err = "thread";
      goto fail;
   }
   return t;

fail:
   if (t->fd >= 0)
      close(t->fd);
   nd_ring_free(&t->rx);
   nd_mutex_destroy(&t->nx);
   nd_mutex_destroy(&t->mx);
   free(t);
   return NULL;
}

void lan_transport_destroy(lan_transport *t)
{
   if (!t)
      return;
   atomic_store_explicit(&t->run, 0, memory_order_release);
   nd_thread_join(&t->th);               /* <= one poll slice */

   /* Whatever the shim still holds leaves now: the last thing netdrv does
    * is BYE, and a peer that never hears it waits out an 8 s death timer. */
   lan_dq_flush(t, 0, 1);
   lan_transport_relay_stop(t);          /* release the allocation while the socket is still open */
   if (t->announcing)
   {
      uint8_t pkt[DISC_GONE_LEN];
      size_t n = disc_build_gone(pkt, t->room_id, t->info.compat);
      int i;
      for (i = 0; i < t->bcast_n; i++)
      {
         struct sockaddr_in b = t->bcast[i];
         b.sin_port = htons((uint16_t)(t->disc_port - 1));
         lan_raw_send(t, &b, pkt, n);
      }
   }
   close(t->fd);
   nd_ring_free(&t->rx);
   nd_ring_free(&t->bulk);
   free(t->dq);
   nd_mutex_destroy(&t->nx);
   nd_mutex_destroy(&t->mx);
   free(t);
}

uint16_t lan_transport_port(const lan_transport *t) { return t ? t->port : 0; }
uint32_t lan_transport_ip(const lan_transport *t)   { return t ? t->ip_be : 0; }

const char *lan_transport_addr_str(const lan_transport *t, char *buf, size_t n)
{
   if (!t)
   {
      if (buf && n)
         buf[0] = '\0';
      return buf ? buf : "";
   }
   return lan_format_addr(t->ip_be, t->port, buf, n);
}

int lan_transport_announce(lan_transport *t, const lan_room_info *info)
{
   if (!t || t->mode != LAN_MODE_HOST)
      return -1;
   nd_mutex_lock(&t->mx);
   if (info)
   {
      t->info = *info;
      if (!t->announcing)
         t->next_beacon_us = 0;          /* first beacon at once */
      t->announcing = 1;
   }
   else
      t->announcing = 0;
   nd_mutex_unlock(&t->mx);
   return 0;
}

/* ---- echo: the round trip to one address, without netdrv -------------------
 * netdrv's own srtt needs a reliable payload of OURS acknowledged, and a
 * host that is about to start a session has sent none.  This asks the other
 * console's I/O thread directly. */
int lan_transport_echo(lan_transport *t, const uint8_t mac[6])
{
   uint8_t pkt[DISC_PROBE_LEN];
   struct sockaddr_in to;
   uint32_t cookie;
   size_t n;
   if (!t || !mac)
      return -1;
   lan_resolve(t, mac, &to);
   nd_mutex_lock(&t->mx);
   cookie = ++t->echo_cookie;
   if (!cookie)
      cookie = ++t->echo_cookie;
   t->echo_sent_us[cookie & 7] = nd_now_us();
   nd_mutex_unlock(&t->mx);
   n = disc_build_echo(pkt, DISC_ECHO, cookie);
   return lan_send(t, &to, pkt, n);
}

int lan_transport_echo_result(lan_transport *t, uint32_t *min_us,
                              uint32_t *max_us, uint32_t *last_us)
{
   int n;
   if (!t)
      return 0;
   nd_mutex_lock(&t->mx);
   n = (int)t->echo_n;
   if (min_us)  *min_us = t->echo_min_us;
   if (max_us)  *max_us = t->echo_max_us;
   if (last_us) *last_us = t->echo_last_us;
   nd_mutex_unlock(&t->mx);
   return n;
}

void lan_transport_echo_reset(lan_transport *t)
{
   if (!t)
      return;
   nd_mutex_lock(&t->mx);
   t->echo_n = t->echo_min_us = t->echo_max_us = t->echo_last_us = 0;
   memset(t->echo_sent_us, 0, sizeof(t->echo_sent_us));
   nd_mutex_unlock(&t->mx);
}

void lan_transport_set_io_keepalive(lan_transport *t, int on)
{
   if (t)
      atomic_store_explicit(&t->io_keep, on ? 1 : 0, memory_order_relaxed);
}

void lan_transport_census(lan_transport *t, lan_census *out, int reset)
{
   uint32_t tx, in, ki, ko;
   uint32_t mask;
   int i;
   if (!out)
      return;
   memset(out, 0, sizeof(*out));
   out->path = "lan";
   if (!t)
      return;
   tx = nd_ctr_get(&t->st.tx_pkts);
   in = nd_ctr_get(&t->st.peer_in);
   ki = nd_ctr_get(&t->st.keep_in);
   ko = nd_ctr_get(&t->st.keep_out);
   out->pkts_out = tx - t->cen_tx_prev;
   out->pkts_in = in - t->cen_in_prev;
   out->keep_in = ki - t->cen_keepin_prev;
   out->keep_out = ko - t->cen_keepout_prev;
   out->worst_gap_us = nd_ctr_get(&t->st.gap_max_us);
   /* "direct": the peer is outside our own subnet (a typed internet address,
    * or a forwarded port) */
   mask = t->mask_be;
   nd_mutex_lock(&t->mx);
   for (i = 0; i < LAN_BOOK_CAP; i++)
      if (t->heard[i].used && mask && t->ip_be &&
          (t->heard[i].a.sin_addr.s_addr & mask) != (t->ip_be & mask) &&
          (ntohl(t->heard[i].a.sin_addr.s_addr) >> 24) != 127)
         out->path = "direct";
   nd_mutex_unlock(&t->mx);
   /* online play: a relay in use, or a hole punched to a peer outside our
    * subnet, is more specific than "direct" */
   nd_mutex_lock(&t->nx);
   if (t->turn_on && nat_turn_state(&t->turn) == LAN_RELAY_READY)
      out->path = "relay";
   else if (t->peer_set && !t->peer_lan)
      out->path = "punch";
   nd_mutex_unlock(&t->nx);
   if (reset)
   {
      t->cen_tx_prev = tx;
      t->cen_in_prev = in;
      t->cen_keepin_prev = ki;
      t->cen_keepout_prev = ko;
      nd_ctr_set(&t->st.gap_max_us, 0);
   }
}

void lan_transport_set_fault(lan_transport *t, int loss_pct, int dup_pct,
                             int latency_ms, int jitter_ms, uint32_t seed)
{
   int delay;
   if (!t)
      return;
   t->loss_pct   = loss_pct   > 0 ? loss_pct   : 0;
   t->dup_pct    = dup_pct    > 0 ? dup_pct    : 0;
   t->latency_ms = latency_ms > 0 ? latency_ms : 0;
   t->jitter_ms  = jitter_ms  > 0 ? jitter_ms  : 0;
   if (seed)
      t->prng = seed;
   delay = (t->latency_ms | t->jitter_ms) != 0;
   if (delay)
   {
      nd_mutex_lock(&t->mx);
      if (!t->dq)
         t->dq = (lan_delayed *)malloc(sizeof(lan_delayed) * LAN_DELAYQ_CAP);
      nd_mutex_unlock(&t->mx);
   }
   /* Left on once set: datagrams already queued must still leave after the
    * shim is switched back off. */
   if (delay)
      atomic_store_explicit(&t->fault_delay, 1, memory_order_relaxed);
}

void lan_transport_get_stats(const lan_transport *t, lan_stats *out)
{
   memset(out, 0, sizeof(*out));
   if (!t)
      return;
#define G(f) out->f = nd_ctr_get(&t->st.f)
   G(tx_pkts); G(tx_would_block); G(tx_fail); G(tx_us); G(tx_max_us);
   G(bcast_pkts); G(rx_pkts); G(rx_ring_drop); G(rx_oversize); G(rx_foreign);
   G(rx_err); G(bulk_tx); G(bulk_txfail); G(bulk_rx); G(bulk_drop);
   G(bulk_foreign); G(disc_probes_rx); G(disc_replies_tx); G(disc_beacons_tx);
   G(disc_limited); G(fault_dropped); G(fault_delayed); G(fault_dup);
   G(io_wakeups); G(io_poll_err); G(nat_aliases);
#undef G
   out->peers = (uint32_t)t->book_n;
}

/* ------------------------------------------------------------------- scan -- */

typedef struct scan_entry
{
   int      used;
   lan_room room;
   struct sockaddr_in from;
   uint64_t last_us;
} scan_entry;

struct lan_scan
{
   int      fd;                    /* probes out, replies in (any port) */
   int      fd_beacon;             /* disc_port-1, or -1 */
   uint16_t disc_port;
   uint32_t compat;
   uint32_t ip_be, mask_be;
   struct sockaddr_in bcast[3];
   int      bcast_n;
   struct sockaddr_in target;
   int      have_target;
   uint32_t cookie;                /* this round's */
   uint64_t sent_us[8];            /* by cookie & 7 */
   uint64_t last_probe_us;
   scan_entry e[LAN_SCAN_MAX];
};

lan_scan *lan_scan_open(const lan_scan_config *cfg)
{
   lan_scan *s;
   lan_scan_config def;

   g_lan_err = "";
   if (!cfg)
   {
      memset(&def, 0, sizeof(def));
      cfg = &def;
   }
   s = (lan_scan *)calloc(1, sizeof(*s));
   if (!s)
   {
      g_lan_err = "out of memory";
      return NULL;
   }
   s->disc_port = cfg->disc_port ? cfg->disc_port : LAN_PORT_DEFAULT;
   s->compat = cfg->compat;
   s->ip_be = cfg->local_ip_be;
   s->mask_be = cfg->netmask_be;
   s->bcast_n = lan_bcast_list(&s->ip_be, &s->mask_be, cfg->flags, s->bcast);
   if (cfg->target_ip_be)
   {
      sa_make(&s->target, cfg->target_ip_be,
              cfg->target_port ? cfg->target_port : LAN_PORT_DEFAULT);
      s->have_target = 1;
   }
   s->fd = lan_sock_open();
   if (s->fd < 0 || lan_sock_bind(s->fd, 0, 0) != 0)
   {
      g_lan_err = "socket (is the network up?)";
      if (s->fd >= 0)
         close(s->fd);
      free(s);
      return NULL;
   }
   /* The beacon listener is optional: another scanner on this machine may
    * hold the port, and probe/reply finds every room without it. */
   s->fd_beacon = lan_sock_open();
   if (s->fd_beacon >= 0 &&
       lan_sock_bind(s->fd_beacon, (uint16_t)(s->disc_port - 1), 1) != 0)
   {
      close(s->fd_beacon);
      s->fd_beacon = -1;
   }
   s->cookie = (uint32_t)nd_now_us() | 1u;
   return s;
}

void lan_scan_close(lan_scan *s)
{
   if (!s)
      return;
   close(s->fd);
   if (s->fd_beacon >= 0)
      close(s->fd_beacon);
   free(s);
}

static void scan_probe(lan_scan *s, uint64_t now)
{
   uint8_t pkt[DISC_PROBE_LEN];
   size_t n;
   int i, p;

   s->cookie++;
   if (!s->cookie)
      s->cookie = 1;                     /* 0 means "beacon" in an announce */
   s->sent_us[s->cookie & 7] = now;
   s->last_probe_us = now;
   n = disc_build_probe(pkt, s->cookie, s->compat);
   for (i = 0; i < s->bcast_n; i++)
      for (p = 0; p < LAN_PORT_TRIES; p++)
      {
         struct sockaddr_in to = s->bcast[i];
         to.sin_port = htons((uint16_t)(s->disc_port + p));
         sendto(s->fd, pkt, n, MSG_DONTWAIT, (struct sockaddr *)&to, sizeof(to));
      }
   if (s->have_target)
      sendto(s->fd, pkt, n, MSG_DONTWAIT, (struct sockaddr *)&s->target,
             sizeof(s->target));
}

static void scan_take(lan_scan *s, const struct sockaddr_in *from,
                      const uint8_t *b, int n, uint64_t now)
{
   lan_room_info info;
   uint32_t room_id, cookie;
   uint16_t port;
   scan_entry *e = NULL;
   int i, type = disc_check(b, (size_t)n);

   if (type == DISC_GONE)
   {
      room_id = get32(b + 16);
      for (i = 0; i < LAN_SCAN_MAX; i++)
         if (s->e[i].used && s->e[i].room.room_id == room_id &&
             s->e[i].from.sin_addr.s_addr == from->sin_addr.s_addr)
            s->e[i].used = 0;
      return;
   }
   if (type != DISC_ANNOUNCE)
      return;
   disc_parse_announce(b, &info, &room_id, &port);
   cookie = get32(b + 8);

   for (i = 0; i < LAN_SCAN_MAX; i++)
      if (s->e[i].used && s->e[i].room.room_id == room_id)
         e = &s->e[i];
   if (e && !sa_eq(&e->from, from))
   {
      /* The same room reached over a second path (a machine with several
       * addresses, or the loopback and the interface).  Keep the address
       * we have while it still answers; switch only if it went quiet. */
      if (now - e->last_us < 2 * LAN_BEACON_US)
         return;
      e->room.rtt_ms = -1;
   }
   if (!e)
   {
      for (i = 0; i < LAN_SCAN_MAX && !e; i++)
         if (!s->e[i].used)
            e = &s->e[i];
      if (!e)
         return;                         /* list full */
      memset(e, 0, sizeof(*e));
      e->used = 1;
      e->room.rtt_ms = -1;
   }
   e->from = *from;
   e->last_us = now;
   e->room.info = info;
   e->room.room_id = room_id;
   e->room.announced_port = port;
   e->room.ip_be = from->sin_addr.s_addr;
   e->room.port = ntohs(from->sin_port);
   e->room.compatible = info.compat == s->compat;
   mac_from_sa(from, e->room.mac);
   lan_format_addr(e->room.ip_be, e->room.port, e->room.addr,
                   sizeof(e->room.addr));
   if (cookie && s->cookie - cookie < 8 && s->sent_us[cookie & 7] &&
       now >= s->sent_us[cookie & 7])
      e->room.rtt_ms = (int32_t)((now - s->sent_us[cookie & 7]) / 1000u);
}

void lan_scan_poll(lan_scan *s)
{
   uint64_t now;
   int k, budget;
   if (!s)
      return;
   now = nd_now_us();
   if (!s->last_probe_us || now - s->last_probe_us >= LAN_PROBE_US)
      scan_probe(s, now);
   for (k = 0; k < 2; k++)
   {
      int fd = k ? s->fd_beacon : s->fd;
      if (fd < 0)
         continue;
      for (budget = 0; budget < 64; budget++)
      {
         uint8_t buf[DISC_MAX_LEN + 8];
         struct sockaddr_in from;
         socklen_t flen = sizeof(from);
         ssize_t n = recvfrom(fd, buf, sizeof(buf), MSG_DONTWAIT,
                              (struct sockaddr *)&from, &flen);
         if (n <= 0)
            break;
         scan_take(s, &from, buf, (int)n, now);
      }
   }
}

static int room_cmp(const void *a, const void *b)
{
   const lan_room *x = (const lan_room *)a, *y = (const lan_room *)b;
   if (x->info.number != y->info.number)
      return (int)x->info.number - (int)y->info.number;
   if (x->ip_be != y->ip_be)
      return ntohl(x->ip_be) < ntohl(y->ip_be) ? -1 : 1;
   return (int)x->port - (int)y->port;
}

int lan_scan_rooms(lan_scan *s, lan_room *out, int max)
{
   uint64_t now;
   int i, n = 0;
   if (!s)
      return 0;
   now = nd_now_us();
   for (i = 0; i < LAN_SCAN_MAX; i++)
   {
      if (!s->e[i].used)
         continue;
      if (now - s->e[i].last_us > LAN_ROOM_TTL_US)
      {
         s->e[i].used = 0;               /* not heard for 3.5 s: gone */
         continue;
      }
      if (out && n < max)
      {
         out[n] = s->e[i].room;
         out[n].age_ms = (uint32_t)((now - s->e[i].last_us) / 1000u);
         n++;
      }
   }
   if (out && n > 1)
      qsort(out, (size_t)n, sizeof(out[0]), room_cmp);
   return n;
}

int lan_discovery_scan(const lan_scan_config *cfg, lan_room *out, int max,
                       unsigned wait_ms)
{
   lan_scan *s = lan_scan_open(cfg);
   uint64_t end;
   int n;
   if (!s)
      return -1;
   end = nd_now_us() + (uint64_t)wait_ms * 1000ull;
   do
   {
      lan_scan_poll(s);
      nd_sleep_us(5000);
   } while (nd_now_us() < end);
   lan_scan_poll(s);
   n = lan_scan_rooms(s, out, max);
   lan_scan_close(s);
   return n;
}

/* ------------------------------------------------- online play (2.0): the API -- */
/* Called from the emulation thread (any thread, in fact); each takes nx,
 * and the I/O thread's ticks do the rest.  The first datagram of a STUN
 * lookup, a punch round and a TURN allocation is sent from here, at once:
 * the I/O thread may be asleep in a 50 ms poll slice. */

int lan_transport_nat_seed(lan_transport *t, const uint8_t entropy[16])
{
   int i;
   if (!t || !entropy)
      return -1;
   nd_mutex_lock(&t->nx);
   for (i = 0; i < 4; i++)
      t->xs[i] = get32(entropy + 4 * i);
   if (!(t->xs[0] | t->xs[1] | t->xs[2] | t->xs[3]))
      t->xs[0] = 0x9E3779B9u;
   t->seeded = 1;
   for (i = 0; i < 8; i++)
      (void)nat_rand(t);                 /* a seed of mostly zeros needs a few rounds */
   nd_mutex_unlock(&t->nx);
   return 0;
}

int lan_transport_stun_start(lan_transport *t, const uint32_t *ip_be, const uint16_t *port, int n)
{
   uint8_t tx[2][12];
   uint64_t now = nd_now_us() / 1000u;
   int rc;
   if (!t || !ip_be || !port || n < 1 || n > 2)
      return -1;
   nd_mutex_lock(&t->nx);
   if (!t->seeded)
   {
      nd_mutex_unlock(&t->nx);
      return -1;
   }
   nat_rand_bytes(t, &tx[0][0], 24);
   memset(&t->skeep, 0, sizeof(t->skeep));
   t->pub_valid = 0;
   t->stun_on = 1;
   t->stun_used = 1;
   atomic_store_explicit(&t->nat_on, 1, memory_order_relaxed);
   nat_stun_start(&t->stun, nat_cb_stun_send, t, ip_be, port, n, (const uint8_t (*)[12])tx, now);
   nat_refresh_srcs(t);
   rc = nat_stun_state(&t->stun) == NAT_STUN_FAILED ? -1 : 0;
   nd_mutex_unlock(&t->nx);
   return rc;
}

void lan_transport_stun_keep(lan_transport *t, unsigned every_ms)
{
   if (!t)
      return;
   nd_mutex_lock(&t->nx);
   t->keep_ms = every_ms;
   t->keep_next_ms = nd_now_us() / 1000u + every_ms;
   if (every_ms && t->stun_used && !t->stun_on)
   {
      t->stun_on = 1;                      /* the keep-alive's answers come from the servers again */
      nat_refresh_srcs(t);
   }
   else if (!every_ms && t->stun_on && nat_stun_state(&t->skeep) == NAT_STUN_IDLE &&
            (nat_stun_state(&t->stun) == NAT_STUN_OK || nat_stun_state(&t->stun) == NAT_STUN_FAILED))
   {
      t->stun_on = 0;                      /* nothing left to listen for; the tick would do it, this is at once */
      nat_refresh_srcs(t);
   }
   atomic_store_explicit(&t->nat_on, 1, memory_order_relaxed);
   nd_mutex_unlock(&t->nx);
}

int lan_transport_punch_arm(lan_transport *t, const uint8_t key[8])
{
   if (!t || !key)
      return -1;
   nd_mutex_lock(&t->nx);
   if (!t->seeded)
   {
      nd_mutex_unlock(&t->nx);
      return -1;
   }
   memcpy(t->punch_key, key, 8);
   t->punch_armed = 1;
   t->peer_set = 0;                       /* a new guest: the last one's address is not this room's */
   t->peer_lan = 0;
   if (t->punch_state == LAN_PUNCH_OFF)
      t->punch_state = LAN_PUNCH_ARMED;
   atomic_store_explicit(&t->nat_on, 1, memory_order_relaxed);
   nd_mutex_unlock(&t->nx);
   return 0;
}

int lan_transport_punch_start(lan_transport *t, const uint32_t *ip_be, const uint16_t *port, int n,
                              unsigned timeout_ms)
{
   uint64_t now = nd_now_us() / 1000u;
   int i;
   if (!t || n < 0 || n > LAN_PUNCH_MAX_CAND || (n && (!ip_be || !port)))
      return -1;
   nd_mutex_lock(&t->nx);
   if (!t->punch_armed)
   {
      nd_mutex_unlock(&t->nx);
      return -1;
   }
   t->ncand = n;
   for (i = 0; i < n; i++)
   {
      t->cand_ip[i] = ip_be[i];
      t->cand_port[i] = port[i];
   }
   t->peer_set = 0;
   t->peer_lan = 0;
   t->punch_sent = 0;
   t->punch_cookie = nat_rand(t);
   t->punch_t0_ms = now;
   t->punch_end_ms = now + (timeout_ms ? timeout_ms : 4000u);
   t->punch_state = LAN_PUNCH_RUNNING;
   atomic_store_explicit(&t->nat_on, 1, memory_order_relaxed);
   nat_punch_round(t, now);
   nd_mutex_unlock(&t->nx);
   return 0;
}

void lan_transport_punch_stop(lan_transport *t)
{
   if (!t)
      return;
   nd_mutex_lock(&t->nx);
   t->punch_armed = 0;
   t->punch_state = LAN_PUNCH_OFF;        /* the peer, if one was found, stays known */
   nd_mutex_unlock(&t->nx);
}

int lan_transport_relay_start(lan_transport *t, uint32_t server_ip_be, const uint16_t *ports, int n_ports,
                              const char *username, const char *credential)
{
   uint64_t now = nd_now_us() / 1000u;
   int i;
   if (!t || !server_ip_be || !ports || n_ports < 1 || n_ports > LAN_RELAY_PORTS_MAX ||
       !username || !credential)
      return -1;
   nd_mutex_lock(&t->nx);
   if (!t->seeded)
   {
      nd_mutex_unlock(&t->nx);
      return -1;
   }
   if (t->turn_on)
      nat_turn_release(&t->turn, now);   /* starting again: let the old one go */
   t->turn_ip_be = server_ip_be;
   t->turn_nports = n_ports;
   for (i = 0; i < n_ports; i++)
      t->turn_ports[i] = ports[i];
   snprintf(t->turn_user, sizeof t->turn_user, "%s", username);
   snprintf(t->turn_pass, sizeof t->turn_pass, "%s", credential);
   t->bind_pending = 0;
   t->turn_on = 1;
   atomic_store_explicit(&t->nat_on, 1, memory_order_relaxed);
   nat_relay_begin(t, 0, now);
   nat_refresh_srcs(t);
   nat_publish_relay(t);
   nd_mutex_unlock(&t->nx);
   return 0;
}

int lan_transport_relay_bind(lan_transport *t, uint32_t peer_ip_be, uint16_t peer_port)
{
   int rc = -1;
   if (!t || !peer_ip_be || !peer_port)
      return -1;
   nd_mutex_lock(&t->nx);
   if (t->turn_on && nat_turn_state(&t->turn) != LAN_RELAY_FAILED &&
       (!t->bind_pending || (t->bind_ip_be == peer_ip_be && t->bind_port == peer_port)))
   {
      /* the tick binds as soon as the allocation exists */
      t->bind_pending = 1;
      t->bind_ip_be = peer_ip_be;
      t->bind_port = peer_port;
      nat_publish_relay(t);
      rc = 0;
   }
   nd_mutex_unlock(&t->nx);
   return rc;
}

void lan_transport_relay_stop(lan_transport *t)
{
   if (!t)
      return;
   nd_mutex_lock(&t->nx);
   if (t->turn_on)
   {
      nat_turn_release(&t->turn, nd_now_us() / 1000u);   /* Refresh with lifetime 0, once */
      t->turn_on = 0;
      t->bind_pending = 0;
      nat_refresh_srcs(t);
      nat_publish_relay(t);
   }
   nd_mutex_unlock(&t->nx);
}

int lan_transport_relay_handoff(lan_transport *t, lan_relay_params *out)
{
   int rc = -1;
   uint32_t ip;
   uint16_t port;
   if (!t || !out)
      return -1;
   nd_mutex_lock(&t->nx);
   if (t->turn_on && t->bind_pending && nat_turn_state(&t->turn) == LAN_RELAY_READY &&
       nat_turn_relayed(&t->turn, &ip, &port))
   {
      memset(out, 0, sizeof *out);
      out->server_ip_be = t->turn_ip_be;
      out->server_port = t->turn_ports[t->turn_idx];
      snprintf(out->user, sizeof out->user, "%s", t->turn_user);
      snprintf(out->pass, sizeof out->pass, "%s", t->turn_pass);
      out->relay_ip_be = ip;
      out->relay_port = port;
      out->peer_ip_be = t->bind_ip_be;
      out->peer_port = t->bind_port;
      /* let go without telling the server: the allocation stays for the next program */
      memset(&t->turn, 0, sizeof t->turn);
      t->turn_on = 0;
      t->bind_pending = 0;
      nat_refresh_srcs(t);
      nat_publish_relay(t);
      rc = 0;
   }
   nd_mutex_unlock(&t->nx);
   return rc;
}

int lan_transport_relay_adopt(lan_transport *t, const lan_relay_params *p)
{
   uint64_t now = nd_now_us() / 1000u;
   uint8_t seed[16];
   if (!t || !p || !p->server_ip_be || !p->server_port || !p->relay_ip_be || !p->relay_port || !p->peer_ip_be ||
       !p->peer_port)
      return -1;
   nd_mutex_lock(&t->nx);
   if (!t->seeded)
   {
      nd_mutex_unlock(&t->nx);
      return -1;
   }
   if (t->turn_on)
      nat_turn_release(&t->turn, now);
   t->turn_ip_be = p->server_ip_be;
   t->turn_nports = 1;
   t->turn_ports[0] = p->server_port;
   t->turn_idx = 0;
   snprintf(t->turn_user, sizeof t->turn_user, "%s", p->user);
   snprintf(t->turn_pass, sizeof t->turn_pass, "%s", p->pass);
   t->bind_pending = 1;                  /* the tick binds once the Refresh has been answered */
   t->bind_ip_be = p->peer_ip_be;
   t->bind_port = p->peer_port;
   t->turn_on = 1;
   atomic_store_explicit(&t->nat_on, 1, memory_order_relaxed);
   nat_rand_bytes(t, seed, sizeof seed);
   nat_turn_adopt(&t->turn, nat_cb_turn_send, t, p->server_ip_be, p->server_port, t->turn_user, t->turn_pass,
                  p->relay_ip_be, p->relay_port, seed, now);
   t->turn_port_t0_ms = now;
   t->turn_heard = 0;
   nat_refresh_srcs(t);
   nat_publish_relay(t);
   nd_mutex_unlock(&t->nx);
   return 0;
}

int lan_transport_nat_listening(lan_transport *t)
{
   return t ? atomic_load_explicit(&t->nat_srcs, memory_order_acquire) : 0;
}

void lan_transport_nat_status(lan_transport *t, lan_nat_status *out)
{
   int sym = 0;
   if (!out)
      return;
   memset(out, 0, sizeof *out);
   if (!t)
      return;
   nd_mutex_lock(&t->nx);
   if (t->stun_used)
   {
      out->stun = nat_stun_state(&t->stun);
      (void)nat_stun_result(&t->stun, NULL, NULL, &sym);
      out->sym = sym;
      if (t->pub_valid)
      {
         out->pub_ip_be = t->pub_ip_be;
         out->pub_port = t->pub_port;
         out->open = t->pub_ip_be == t->ip_be;
      }
   }
   out->punch = t->punch_state;
   if (t->peer_set)
   {
      out->peer_ip_be = t->peer_ip_be;
      out->peer_port = t->peer_port;
      out->peer_is_lan = t->peer_lan;
   }
   if (t->turn_on)
   {
      out->relay = nat_turn_state(&t->turn);
      out->relay_err = nat_turn_error(&t->turn);
      (void)nat_turn_relayed(&t->turn, &out->relay_ip_be, &out->relay_port);
   }
   nd_mutex_unlock(&t->nx);
   out->stun_tx = nd_ctr_get(&t->st.stun_tx);
   out->stun_rx = nd_ctr_get(&t->st.stun_rx);
   out->punch_tx = nd_ctr_get(&t->st.punch_tx);
   out->punch_rx = nd_ctr_get(&t->st.punch_rx);
   out->ack_rx = nd_ctr_get(&t->st.ack_rx);
   out->relay_tx = nd_ctr_get(&t->st.relay_tx);
   out->relay_rx = nd_ctr_get(&t->st.relay_rx);
   out->nat_foreign = nd_ctr_get(&t->st.nat_foreign);
}

int lan_transport_set_target(lan_transport *t, uint32_t ip_be, uint16_t port)
{
   int i;
   if (!t || t->mode != LAN_MODE_JOIN || !ip_be)
      return -1;
   /* A session that already talks to somebody is not ours to move: any
    * session datagram heard (netdrv, bulk, keepalive; not discovery) or any
    * address in the book besides the target we were given before.  (That
    * earlier target is the one thing allowed to be replaced.) */
   if (nd_ctr_get(&t->st.peer_in) > 0 || t->book_n > (t->have_target ? 1 : 0) ||
       (t->have_target && t->book_n == 1 && !sa_eq(&t->book[0].a, &t->target)))
      return -1;
   if (t->have_target)
   {
      nd_mutex_lock(&t->mx);
      for (i = 0; i < LAN_BOOK_CAP; i++)
         if (t->heard[i].used && sa_eq(&t->heard[i].a, &t->target))
            t->heard[i].used = 0;
      nd_mutex_unlock(&t->mx);
      t->book_n = 0;
   }
   sa_make(&t->target, ip_be, port ? port : LAN_PORT_DEFAULT);
   t->have_target = 1;
   book_touch(t, &t->target);             /* JOIN goes there, as create's target does */
   heard_touch(t, &t->target, nd_now_us());
   return 0;
}
