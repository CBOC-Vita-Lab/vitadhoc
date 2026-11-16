/* transport_lan.h -- UDP/IPv4 backend for netdrv on a Wi-Fi network or the
 * internet, with room discovery.  The PS Vita port's transport for
 *
 *   A. Wi-Fi LAN   both consoles on one router or phone hotspot: a host
 *                  announces a room, a joiner finds it with a scan and joins
 *                  it by address (or broadcasts its JOIN to the subnet);
 *   B. direct IP   the joiner types the host's "ip[:port]" (LAN or internet,
 *                  the host behind a forwarded port): unicast only.
 *
 * It carries exactly what transport_adhoc carries on a PSP -- netdrv frames
 * and the link session's bulk lane -- so everything above it is unchanged.
 * Portable BSD sockets: the same file is the desktop build's and the tests'.
 *
 * ADDRESSES.  netdrv names a peer by 6 bytes.  As in transport_udp they are
 * mac[0..3] = IPv4, mac[4..5] = UDP port, both in network order, so a roster
 * entry decodes straight back to an address.  One difference matters on the
 * internet: the address a host DECLARES for itself (its own idea of its
 * ip:port, which is what netdrv puts in the roster) is not the address a
 * joiner behind a different network REACHES it at.  The joining side of this
 * transport therefore keeps an alias "declared -> observed" learned from the
 * host's WELCOME/ROSTER frames and applies it in both directions; netdrv and
 * its wire format are untouched.  See "NAT" in transport_lan.c.
 *
 * THREADS.  One I/O thread per transport blocks in poll() on the socket and
 * feeds two lock-free SPSC rings (netdrv frames, bulk lane); it also answers
 * discovery probes and sends the room beacon, so a host is discoverable even
 * while the emulation thread is busy loading a game.  The emulation thread
 * calls send_to/broadcast/recv/pending and never blocks: recv and pending
 * are ring operations (pending is two plain loads), a send is one
 * non-blocking sendto().  The I/O thread never touches netdrv.
 *
 * DISCOVERY shares the game socket (see "why one socket" in transport_lan.c):
 * discovery datagrams start with the magic "GBLD", netdrv frames with 'GPN1',
 * bulk datagrams with the session's own magic; the I/O thread routes on the
 * first four bytes and a discovery datagram is never queued for netdrv.
 *
 * IPv4 only.  ND_MAX_FRAME is compile-time: build this file with the same
 * -DND_* flags as netdrv.c.
 */
#ifndef TRANSPORT_LAN_H
#define TRANSPORT_LAN_H

#include <stddef.h>
#include <stdint.h>

#include "netdrv.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The port a host binds: 19019 = 0x4A4B, the PSP build's PDP port and the
 * desktop build's default.  If it is taken the host tries the next ones, so
 * a scan probes the whole range. */
#define LAN_PORT_DEFAULT  19019
#define LAN_PORT_TRIES    8
/* Largest bulk-lane datagram.  1400 leaves room for IPv4+UDP inside a 1500
 * byte Ethernet MTU with ~70 bytes to spare for PPPoE or a tunnel on an
 * internet path; a datagram that has to be fragmented is lost whole when one
 * fragment is. */
#define LAN_BULK_MAX      1400
#define LAN_DISC_VERSION  1
#define LAN_SCAN_MAX      16

/* ------------------------------------------------------------------ rooms -- */

/* The link a room offers.  Numerically psp/room_label.h's ROOM_ADAPTER /
 * ROOM_CABLE_GBA / ROOM_CABLE_GB, so the PSP frontend's room vocabulary and
 * this one's agree. */
enum
{
   LAN_KIND_GBA_WIRELESS = 0,    /* GBA Wireless Adapter (RFU) */
   LAN_KIND_GBA_CABLE    = 1,    /* GBA Link Cable (the mirror) */
   LAN_KIND_GB_CABLE     = 2,    /* Game Boy / Color Link Cable */
   LAN_KIND_SHARE        = 4,    /* Share game data (ROOM_SHARE): no link,
                                    saves and states (vita/share_vita.c) */
   LAN_KIND_PAD          = 5     /* Two players on a NES / WonderSwan game
                                    (ROOM_PAD): both consoles run one
                                    machine, one pad each (fe_padpair.h) */
};

#define LAN_ROOM_F_OPEN   0x01   /* accepting a JOIN right now */
#define LAN_ROOM_F_BUSY   0x02   /* a session is running in it */

/* What a host says about its room.  Strings are NUL-terminated here and
 * NUL-padded on the wire; longer text is truncated. */
typedef struct lan_room_info
{
   char     name[16];        /* e.g. the PSP group id "GC07AMKE", or free text */
   uint8_t  number;          /* room number 0..99 (Settings > Room) */
   uint8_t  kind;            /* LAN_KIND_* */
   uint8_t  players;         /* in the room now, the host included */
   uint8_t  max_players;
   uint8_t  flags;           /* LAN_ROOM_F_* */
   char     nick[16];        /* the host's nickname */
   char     title[20];       /* game title (GBA header title / GB title) */
   char     code[5];         /* 4-character game code, or the first four
                                alphanumerics of a GB title */
   uint32_t compat;          /* build/protocol compatibility hash: two
                                consoles can link only if these are equal.
                                See lan_compat_hash(). */
} lan_room_info;

/* One room found by a scan. */
typedef struct lan_room
{
   lan_room_info info;
   uint32_t room_id;         /* random per hosting instance */
   uint32_t ip_be;           /* where to join it: the address its datagrams
                                came FROM (network order) ... */
   uint16_t port;            /* ... and port (host order) */
   uint16_t announced_port;  /* the port the host believes it is on */
   uint8_t  mac[6];          /* the same address as a netdrv fake MAC */
   uint8_t  compatible;      /* info.compat == the scanner's */
   uint32_t age_ms;          /* since it was last heard */
   int32_t  rtt_ms;          /* probe round trip, -1 = only its beacon was
                                heard.  UDP has no signal strength; this is
                                the list's quality column. */
   char     addr[24];        /* "192.168.1.23:19019" for the UI */
} lan_room;

/* FNV-1a over the protocol string, a build tag and this build's netdrv frame
 * geometry (ND_MAX_PAYLOAD, ND_WINDOW): everything that makes two builds
 * unable to hold a session.  NULLs are skipped. */
uint32_t lan_compat_hash(const char *protocol, const char *build);

/* ---------------------------------------------------------------- transport -- */

enum { LAN_MODE_HOST = 0, LAN_MODE_JOIN = 1 };

/* Use the loopback network INSTEAD of the real interface (address 127.0.0.1,
 * broadcasts to 127.255.255.255 and 127.0.0.1): several instances on one
 * machine with no dependence on its network (the tests, two desktop twins).
 * Never set on a console. */
#define LAN_F_LOOPBACK   0x01
/* Host: do not answer probes or beacon (a direct-IP-only room). */
#define LAN_F_NO_DISCOVERY 0x02
/* The I/O thread sends a small keepalive datagram to every session peer it
 * has heard from (once a second), and records when anything last arrived
 * from each, so netdrv can tell a peer whose main thread is busy (a state
 * save, card I/O) from one that is gone: nd_transport.heard_age_us.  A
 * build that does not know the datagram ignores it. */
#define LAN_F_IO_KEEPALIVE 0x04

typedef struct lan_config
{
   int      mode;            /* LAN_MODE_* */
   /* HOST: the port to bind; 0 = LAN_PORT_DEFAULT.  If it is taken, the next
    * port_tries-1 ports are tried (0 = LAN_PORT_TRIES; 1 = this port or
    * fail, for a host whose router forwards exactly one port).
    * JOIN: the local port; 0 = any (recommended). */
   uint16_t port;
   int      port_tries;
   /* Discovery's well-known base port, when it is not the default: probes
    * go to disc_port .. disc_port+LAN_PORT_TRIES-1, beacons to disc_port-1.
    * 0 = LAN_PORT_DEFAULT.  A host must bind inside that range to be found
    * (it always is when `port` is left 0). */
   uint16_t disc_port;
   /* This console's IPv4 address and netmask, network order.  0 = find out
    * (Vita: sceNetCtlInetGetInfo; desktop: the first non-loopback interface,
    * else 127.0.0.1).  The address is what a host declares in the roster;
    * the mask gives the subnet broadcast address. */
   uint32_t local_ip_be;
   uint32_t netmask_be;
   /* JOIN: the host to talk to (a room picked from a scan, or "ip[:port]"
    * typed by the player).  target_ip_be 0 = nobody in particular: netdrv's
    * JOIN is broadcast to the subnet and to 255.255.255.255 on every port
    * of the range until a host answers.  target_port 0 = LAN_PORT_DEFAULT. */
   uint32_t target_ip_be;
   uint16_t target_port;
   unsigned flags;           /* LAN_F_* */
   unsigned rx_slots;        /* netdrv RX ring depth; 0 = 256 */
   /* New in 2.0 (online play); all zero = off. */
   uint32_t sim_ip_be;       /* test builds: the NAT simulator (see 1.8) */
   uint16_t sim_port;
   uint8_t  sim_id;
   uint16_t sim_lan_port;    /* the port this transport declares inside the simulated LAN */
} lan_config;

typedef struct lan_transport lan_transport;

/* Open the socket and start the I/O thread.  The network must already be up
 * (Vita: vita/net_vita.c; this file never initialises SceNet).  NULL on
 * error; lan_last_error() says why. */
lan_transport *lan_transport_create(const lan_config *cfg);
/* Stop the thread, announce the room gone, close the socket, free
 * everything.  The netdrv using it must be destroyed first. */
void lan_transport_destroy(lan_transport *t);
/* Why the last lan_transport_create()/lan_scan_open() on this thread's
 * process failed: a short static string, never NULL. */
const char *lan_last_error(void);

/* Fill the nd_transport vtable (zeroed first: netdrv.h's rule). */
void lan_transport_iface(lan_transport *t, nd_transport *out);

/* The bound port (host order), our address (network order), and both as
 * "192.168.1.23:19019" -- what a host shows for a direct-IP joiner. */
uint16_t    lan_transport_port(const lan_transport *t);
uint32_t    lan_transport_ip(const lan_transport *t);
const char *lan_transport_addr_str(const lan_transport *t, char *buf, size_t n);

/* HOST: announce this room (answer probes, beacon once a second), or change
 * what is announced (player count, flags).  NULL = stop announcing.  The
 * info is copied.  0 on success. */
int lan_transport_announce(lan_transport *t, const lan_room_info *info);

/* ---- echo: the round trip to a session peer, before any session traffic ----
 * netdrv's srtt needs a reliable payload of ours acknowledged; a host about
 * to start a link session has sent none, and it is the host that picks the
 * session's input delay.  lan_transport_echo() sends one 18-byte request to
 * the peer's address (through the fault shim, so an injected latency is
 * measured too); the other console's I/O THREAD answers it at once -- any
 * transport does, host or joiner -- so the result is the network's round
 * trip, not the other frame loop's.  Send a handful a frame apart and read
 * the result: how many came back and the smallest / largest / latest round
 * trip in microseconds.  Emulation thread; never blocks. */
int  lan_transport_echo(lan_transport *t, const uint8_t mac[6]);
int  lan_transport_echo_result(lan_transport *t, uint32_t *min_us,
                               uint32_t *max_us, uint32_t *last_us);
void lan_transport_echo_reset(lan_transport *t);

/* ---- bulk lane: transport_adhoc's, same semantics ------------------------
 * Unreliable datagrams of up to LAN_BULK_MAX bytes beside netdrv's frames,
 * on the same socket: a received datagram that begins with the 4-byte
 * `magic` goes to its own ring (`slots` deep, allocated on the first open,
 * freed at destroy) instead of netdrv's.  Emulation thread only.  Datagrams
 * from an address that is not a session peer are discarded at recv. */
int  lan_transport_bulk_open(lan_transport *t, unsigned slots,
                             const char magic[4]);
void lan_transport_bulk_close(lan_transport *t);
/* 0 = sent or lost (the session layer resends); <0 = not open / too long. */
int  lan_transport_bulk_send(lan_transport *t, const uint8_t mac[6],
                             const void *buf, size_t len);
/* One datagram into buf: its length, 0 if none. */
int  lan_transport_bulk_recv(lan_transport *t, void *buf, size_t cap);
/* 1 = the bulk lane takes datagrams from ANY address, not only from the
 * netdrv session's peers: for a user of the lane that runs no netdrv
 * session and has a handshake of its own (Share game data: fe_share.h's
 * HELLO / OFFER / CONFIRM and its read-only server).  Emulation thread. */
void lan_transport_bulk_any(lan_transport *t, int on);
unsigned lan_transport_rx_depth(const lan_transport *t);
unsigned lan_transport_bulk_depth(const lan_transport *t);

typedef struct lan_stats
{
   uint32_t tx_pkts;         /* sendto successes (netdrv + bulk) */
   uint32_t tx_would_block;  /* socket buffer full: dropped, ARQ recovers */
   uint32_t tx_fail;         /* other sendto errors */
   uint32_t tx_us, tx_max_us;/* time the emulation thread spent in sendto */
   uint32_t bcast_pkts;      /* datagrams sent for netdrv broadcast() */
   uint32_t rx_pkts;         /* netdrv frames queued for the pump */
   uint32_t rx_ring_drop;    /* netdrv ring full (ARQ recovers) */
   uint32_t rx_oversize;     /* 'GPN1' datagram > ND_MAX_FRAME */
   uint32_t rx_foreign;      /* not ours at all (no known magic) */
   uint32_t rx_err;          /* unexpected recvfrom errors */
   uint32_t bulk_tx, bulk_txfail, bulk_rx, bulk_drop, bulk_foreign;
   uint32_t disc_probes_rx;  /* discovery probes heard */
   uint32_t disc_replies_tx; /* announces sent in answer */
   uint32_t disc_beacons_tx; /* periodic announces sent */
   uint32_t disc_limited;    /* probes not answered (rate limit) */
   uint32_t fault_dropped, fault_delayed, fault_dup;   /* debug shim */
   uint32_t io_wakeups;      /* I/O thread loop iterations */
   uint32_t io_poll_err;     /* poll() failed: fell back to a 1 ms sleep */
   uint32_t peers;           /* addresses in the book now */
   uint32_t nat_aliases;     /* hosts found behind a NAT: their roster entry
                              * rewritten to the observed address */
} lan_stats;

void lan_transport_get_stats(const lan_transport *t, lan_stats *out);

/* ---- census: what the wire did, for a once-a-second log line -------------
 * Totals since the last call with reset = 1 (or since creation), counted by
 * the I/O thread so a stalled main thread does not hide anything.
 * `pkts_in` / `pkts_out` are datagrams from / to session peers (netdrv
 * frames, bulk lane and keepalives; discovery excluded); `worst_gap_us` is
 * the longest silence between two such datagrams from a peer inside the
 * window.  `path` is "lan" or "direct": whether the peer's address is in
 * our own subnet (internet play by room code, with punching and a relay, is
 * not built: docs/ONLINE.md is not in this release). */
typedef struct lan_census
{
   uint32_t pkts_in, pkts_out;
   uint32_t keep_in, keep_out;
   uint32_t worst_gap_us;
   const char *path;
} lan_census;
void lan_transport_census(lan_transport *t, lan_census *out, int reset);
/* Switch the I/O-thread keepalive on or off after creation (LAN_F_IO_KEEPALIVE
 * at create does the same). */
void lan_transport_set_io_keepalive(lan_transport *t, int on);

/* ---- debug fault-injection shim (tests, harness) -------------------------
 * udp_transport_set_fault()'s contract: loss/dup in percent, latency/jitter
 * in milliseconds, applied to every netdrv and bulk datagram this transport
 * SENDS (each is delayed latency + U[0,jitter] ms, so jitter also reorders).
 * All zero = transparent, and then no datagram ever takes this path.  With
 * a delay configured the datagrams wait in a mutex-guarded queue that the
 * I/O thread drains on 1 ms ticks.  Emulation thread only. */
void lan_transport_set_fault(lan_transport *t, int loss_pct, int dup_pct,
                             int latency_ms, int jitter_ms, uint32_t seed);

/* ----------------------------------------------------------------- discovery -- */

typedef struct lan_scan_config
{
   uint16_t disc_port;       /* 0 = LAN_PORT_DEFAULT (see lan_config) */
   uint32_t local_ip_be;     /* as lan_config; 0 = find out */
   uint32_t netmask_be;
   uint32_t compat;          /* this console's hash (lan_room.compatible) */
   unsigned flags;           /* LAN_F_LOOPBACK */
   /* Optional: also probe this one address directly (a "ip[:port]" the
    * player typed: shows the room behind it, checks compatibility and
    * measures the round trip before joining).  0 = none. */
   uint32_t target_ip_be;
   uint16_t target_port;
} lan_scan_config;

typedef struct lan_scan lan_scan;

/* A scan is a UI-thread object with no thread of its own: open it when the
 * Join screen opens, call lan_scan_poll() once per frame (it sends a probe
 * round every 500 ms and drains what came back; never blocks), read the
 * list whenever it is drawn, close it when the screen closes.  A room not
 * heard for 3.5 s leaves the list. */
lan_scan *lan_scan_open(const lan_scan_config *cfg);
void      lan_scan_poll(lan_scan *s);
/* Up to `max` rooms into out[], ordered by room number then address.
 * Returns how many. */
int       lan_scan_rooms(lan_scan *s, lan_room *out, int max);
void      lan_scan_close(lan_scan *s);

/* The whole thing in one blocking call (tests, a simple frontend): scan for
 * wait_ms, return the rooms found.  <0 on socket error. */
int lan_discovery_scan(const lan_scan_config *cfg, lan_room *out, int max,
                       unsigned wait_ms);

/* ------------------------------------------------- online play: NAT layer (2.0) -- */
/* Owner: package N.  W0 gives the declarations and stubs that say "not
 * available"; N fills them in.  See ONLINE-2.0-PLAN.md section 2.1. */

enum { LAN_STUN_IDLE = 0, LAN_STUN_RUNNING, LAN_STUN_OK, LAN_STUN_FAILED };
enum { LAN_PUNCH_OFF = 0, LAN_PUNCH_ARMED, LAN_PUNCH_RUNNING, LAN_PUNCH_DONE, LAN_PUNCH_TIMEOUT };
enum { LAN_RELAY_OFF = 0, LAN_RELAY_ALLOCATING, LAN_RELAY_ALLOCATED, LAN_RELAY_BINDING,
       LAN_RELAY_READY, LAN_RELAY_FAILED };
enum { LAN_RELAY_E_NONE = 0, LAN_RELAY_E_TIMEOUT, LAN_RELAY_E_AUTH, LAN_RELAY_E_QUOTA, LAN_RELAY_E_LOST };

typedef struct lan_nat_status
{
   int      stun;                       /* LAN_STUN_* */
   uint32_t pub_ip_be;  uint16_t pub_port;   /* OK: our address as the internet sees it */
   int      sym;                        /* two servers answered differently */
   int      open;                       /* the mapped address is our own: no NAT */
   int      punch;                      /* LAN_PUNCH_* */
   uint32_t peer_ip_be; uint16_t peer_port;  /* DONE: where the peer answered from */
   int      peer_is_lan;                /* ...and it is in our own subnet */
   int      relay;                      /* LAN_RELAY_* */
   int      relay_err;                  /* LAN_RELAY_E_* */
   uint32_t relay_ip_be; uint16_t relay_port; /* ALLOCATED and later: our relayed address */
   uint32_t stun_tx, stun_rx, punch_tx, punch_rx, ack_rx, relay_tx, relay_rx, nat_foreign;
} lan_nat_status;

/* 16 bytes of real randomness for transaction ids and cookies; call before anything below. 0 = ok. */
int  lan_transport_nat_seed(lan_transport *t, const uint8_t entropy[16]);
/* Ask n (1..2) STUN servers for our public address.  Restarting is allowed.  0 = started. */
int  lan_transport_stun_start(lan_transport *t, const uint32_t *ip_be, const uint16_t *port, int n);
/* After LAN_STUN_OK: ask server 0 again every `every_ms` (0 = stop) so the mapping stays. */
void lan_transport_stun_keep(lan_transport *t, unsigned every_ms);
/* Accept and answer PUNCH datagrams that carry this key (the 8 characters' bytes). */
int  lan_transport_punch_arm(lan_transport *t, const uint8_t key[8]);
/* Send PUNCH to n (0..3) candidates until DONE or timeout_ms.  Needs punch_arm.  0 = started. */
int  lan_transport_punch_start(lan_transport *t, const uint32_t *ip_be, const uint16_t *port, int n,
                               unsigned timeout_ms);
void lan_transport_punch_stop(lan_transport *t);          /* also disarms */
/* Allocate on a TURN server with short-lived credentials (copied).  0 = started. */
int  lan_transport_relay_start(lan_transport *t, uint32_t server_ip_be, const uint16_t *ports, int n_ports,
                               const char *username, const char *credential);   /* n_ports 1..3, tried in order */
/* After LAN_RELAY_ALLOCATED: permission + channel for the peer's relayed address; READY follows. */
int  lan_transport_relay_bind(lan_transport *t, uint32_t peer_ip_be, uint16_t peer_port);
void lan_transport_relay_stop(lan_transport *t);          /* releases the allocation, best effort */

/* An allocation that outlives a program (2.0, SNES: the app forms the link, snes.bin plays it).  The server keeps
 * an allocation by the client's 5-tuple, so the next program, bound to the SAME local port, takes it over instead
 * of allocating again (a second Allocate would be refused, 437). */
typedef struct lan_relay_params
{
   uint32_t server_ip_be;  uint16_t server_port;    /* the TURN port that answered */
   char     user[160], pass[160];                   /* the short-lived credentials */
   uint32_t relay_ip_be;   uint16_t relay_port;     /* our relayed address */
   uint32_t peer_ip_be;    uint16_t peer_port;      /* the peer's relayed address (the ChannelBind target) */
} lan_relay_params;
/* In the program that made the relay, once READY: copy out what the next program needs and let go WITHOUT releasing
 * (no Refresh lifetime 0): relay_stop and destroy then do nothing to the server.  0 = filled, -1 = not READY. */
int  lan_transport_relay_handoff(lan_transport *t, lan_relay_params *out);
/* In the next program (same local port): Refresh the existing allocation (401 then signed) and, when it answers,
 * ALLOCATED follows, then the permission and the channel are sent again for the same peer (a ChannelBind may be
 * repeated: it refreshes the binding) and READY follows.  FAILED with LAN_RELAY_E_LOST when the server has no
 * such allocation.  No Allocate is sent.  0 = started. */
int  lan_transport_relay_adopt(lan_transport *t, const lan_relay_params *p);
/* Any thread, never blocks (a copy under the transport's mutex). */
void lan_transport_nat_status(lan_transport *t, lan_nat_status *out);
/* Diagnostic: how many STUN / TURN server addresses the I/O thread checks every datagram against (each check takes the NAT mutex).
 * 0 once STUN has finished and no keep-alive runs and no relay is up: the datagrams of a running link then skip it. */
int lan_transport_nat_listening(lan_transport *t);
/* JOIN mode, before the netdrv session starts: the host is at this address (what create's
 * target_ip_be/target_port do).  0 = ok, -1 = not a joiner / a session already has peers. */
int  lan_transport_set_target(lan_transport *t, uint32_t ip_be, uint16_t port);
/* lan_transport_bulk_recv plus where the datagram came from. */
int  lan_transport_bulk_recv_from(lan_transport *t, uint8_t src_mac[6], void *buf, size_t cap);

/* lan_census.path gains the values "punch" (a punch peer is set, not in our
 * subnet) and "relay".  lan_transport_destroy() calls relay_stop itself.
 * All state machines tick on the I/O thread (its poll slice drops to 20 ms
 * while STUN, punch or a TURN request is in flight); commands and status
 * cross under the transport's mutex. */

/* ------------------------------------------------------------------- helpers -- */

/* "a.b.c.d" or "a.b.c.d:port" -> address (network order) and port (host
 * order; default_port when the text has none).  0 on success, -1 if the
 * text is not a dotted quad with an optional port 1..65535.  Surrounding
 * blanks are ignored. */
int lan_parse_addr(const char *text, uint32_t *ip_be, uint16_t *port,
                   uint16_t default_port);
/* The reverse, "a.b.c.d:port" into buf; returns buf. */
const char *lan_format_addr(uint32_t ip_be, uint16_t port, char *buf, size_t n);
/* netdrv fake MAC <-> address. */
void lan_mac_from_addr(uint32_t ip_be, uint16_t port, uint8_t mac[6]);
void lan_addr_from_mac(const uint8_t mac[6], uint32_t *ip_be, uint16_t *port);
/* This machine's IPv4 address and netmask (network order).  0 on success.
 * Vita: sceNetCtlInetGetInfo (needs sceNetCtlInit and an access point);
 * elsewhere the first interface that is up and not loopback. */
int lan_local_ipv4(uint32_t *ip_be, uint32_t *netmask_be);

#ifdef __cplusplus
}
#endif

#endif /* TRANSPORT_LAN_H */
