/* net_vita.h -- what the Vita main loop needs around the two link
 * transports: the network stack's life, the Wi-Fi's state for the menus,
 * address strings, the "one radio, one use" rule, and one place that knows
 * which transport the running session is on.
 *
 * The transports themselves are netdrv/transport_lan.h (Wi-Fi LAN and
 * direct IP) and netdrv/transport_vadhoc.h (ad hoc).  The call sequences
 * for hosting, scanning, joining and tearing down on each are written out
 * at the bottom of this header.
 */
#ifndef NET_VITA_H
#define NET_VITA_H

#include <stddef.h>
#include <stdint.h>

#include "netdrv.h"
#include "transport_lan.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- the stack -------------------------------------------------------------
 * Once at boot: SCE_SYSMODULE_NET, sceNetInit with a pool of our own (1 MiB),
 * sceNetCtlInit.  Explicit and early, for two reasons: vitasdk's newlib
 * would otherwise do it silently inside the first socket() with a 140 KiB
 * pool, and transport_vadhoc's own init then finds the stack up and leaves
 * it alone.  Idempotent; 0 on success, else the SCE error.  The stack stays
 * up for the life of the program: it costs the pool and nothing else. */
int  net_vita_init(void);
void net_vita_term(void);                 /* at exit, after every release */
uint32_t net_vita_last_sce_error(void);

/* ---- Wi-Fi (infrastructure) state ------------------------------------------ */

typedef struct net_vita_wifi
{
   int      connected;       /* associated to an access point, with an address */
   int      state;           /* raw SceNetCtlState, -1 if the query failed */
   uint32_t ip_be;           /* this console, network order (0 if none) */
   uint32_t netmask_be;
   uint32_t bcast_be;        /* subnet broadcast: (ip & mask) | ~mask */
   char     ip[16];          /* "192.168.1.23" */
   char     ssid[33];
   int      rssi_pct;        /* 0..100 access-point signal, -1 unknown */
} net_vita_wifi;

/* Five small syscalls: call it when a menu opens and about once a second
 * while it shows, not per frame.  Always fills `out` (zeros when down);
 * returns out->connected. */
int net_vita_wifi_status(net_vita_wifi *out);

/* "192.168.1.23:19019": what a host shows so the other player can type it.
 * "" when there is no address. */
const char *net_vita_host_string(uint16_t port, char *buf, size_t n);

/* "a.b.c.d" or "a.b.c.d:port" from the on-screen keyboard -> address
 * (network order) and port (LAN_PORT_DEFAULT when omitted).  0 on success. */
int net_vita_parse_addr(const char *text, uint32_t *ip_be, uint16_t *port);

/* ---- one radio, one use ----------------------------------------------------
 * The console has one WLAN interface.  While it is associated to an access
 * point the UDP transport works; when the system's NetCheck dialog connects
 * it in PSP ad hoc mode the association is gone and UDP sockets go deaf (and
 * the other way round).  So exactly one of these may be claimed:
 *
 *   NET_VITA_INFRA  a lan_scan is open or a lan_transport exists
 *   NET_VITA_ADHOC  from vadhoc_dialog_start() until vadhoc_transport_close()
 *
 * claim() refuses (-1) while the other is held; the menus turn that into
 * "End the Wi-Fi session first" / "Leave ad hoc first".  After releasing
 * ADHOC the console re-associates by itself: wait for
 * net_vita_wifi_status().connected before offering LAN again.
 * (HARDWARE: that it does, and how long it takes.) */
enum { NET_VITA_NONE = 0, NET_VITA_INFRA = 1, NET_VITA_ADHOC = 2 };
int  net_vita_claim(int use);
void net_vita_release(int use);
int  net_vita_use(void);

/* ---- the session's link, whichever transport --------------------------------
 * Tell this module which transport the session runs on; it then provides the
 * vtable for fe_np_start*() and the three bulk-lane hooks with exactly
 * fe_host_config's signatures (gb_bulk_enable / gb_bulk_send / gb_bulk_recv),
 * so main_vita.c wires them once and never looks at the transport again. */
void net_vita_link_lan(lan_transport *t);   /* NULL = none */
void net_vita_link_adhoc(void);             /* transport_vadhoc is open */
void net_vita_link_none(void);
/* 0 on success, -1 with no link. */
int  net_vita_link_iface(nd_transport *out);

#define NET_VITA_BULK_SLOTS 128             /* ring depth for either transport */
int  net_vita_bulk_enable(int on);
int  net_vita_bulk_send(const uint8_t mac[6], const void *buf, size_t len);
int  net_vita_bulk_recv(void *buf, size_t cap);
/* Datagrams waiting for the pump (telemetry: EVT gblink_win rxq_max). */
unsigned net_vita_rx_depth(void);

/* ---- ARQ timing per kind of link --------------------------------------------
 * netdrv's timers are per session (nd_config); frontend-common exposes two
 * of them through fe_np_set_arq_timers(rto_first_max_us, rto_min_us).  Call
 * it before EVERY fe_np_start*(): the setter's values persist from one
 * session to the next.  These are the values to pass; the measurements
 * behind them are in net_vita.c and reproduce with
 * `make -C gpsp/netdrv bench-lan`. */
enum
{
   NET_VITA_LINK_LAN = 0,     /* same access point / hotspot */
   NET_VITA_LINK_INTERNET,    /* direct IP across the internet */
   NET_VITA_LINK_ADHOC        /* console to console */
};
void net_vita_arq_profile(int link, uint32_t *rto_first_max_us,
                          uint32_t *rto_min_us);

/* =============================================================================
 * THE SEQUENCES (main_vita.c).  `kind` is LAN_KIND_*; `proto` the netdrv
 * protocol string of the mode ("gpSP v1.0", "GBAdhoc GB link v2",
 * "GBAdhoc cable v1"); compat = lan_compat_hash(proto, BUILD_ID).
 *
 * Boot, once:
 *   net_vita_init();
 *   sceCommonDialogSetConfigParam(...)      (the renderer's start-up; needed
 *                                            for the ad hoc dialog)
 *
 * A. Wi-Fi LAN ------------------------------------------------------------
 *  host:
 *   net_vita_wifi_status(&w)  -> w.connected, else "Connect to Wi-Fi first"
 *   net_vita_claim(NET_VITA_INFRA)
 *   cfg = { .mode = LAN_MODE_HOST, .local_ip_be = w.ip_be,
 *           .netmask_be = w.netmask_be };       (port 0 = 19019, fallback)
 *   t = lan_transport_create(&cfg);
 *   lan_transport_announce(t, &room);           (name/number/kind/nick/
 *                                                title/code/players/compat)
 *   net_vita_link_lan(t); net_vita_link_iface(&tp);
 *   net_vita_arq_profile(NET_VITA_LINK_LAN, &first, &floor);
 *   fe_np_set_arq_timers(first, floor);
 *   fe_np_start(&npc) / fe_np_start_gb(&npc)    (npc.transport = &tp,
 *                                                npc.is_host = 1)
 *   show lan_transport_addr_str(t)              ("192.168.1.23:19019")
 *   on a peer joining/leaving: room.players = ...; lan_transport_announce()
 *   then exactly as the PSP: gblink_begin() -> fe_host_gblink_start(), with
 *   cfg.gb_bulk_enable/send/recv = net_vita_bulk_enable/send/recv,
 *   cfg.gb_bulk_max = 1037.
 *  scan (the Join screen):
 *   net_vita_claim(NET_VITA_INFRA)
 *   sc = { .local_ip_be = w.ip_be, .netmask_be = w.netmask_be,
 *          .compat = compat };
 *   scan = lan_scan_open(&sc);
 *   every frame:  lan_scan_poll(scan); n = lan_scan_rooms(scan, rooms, 16);
 *   draw rooms[i].info.{number,kind,title,nick,players}, rooms[i].compatible
 *   on close: lan_scan_close(scan); (net_vita_release if not joining)
 *  join a listed room:
 *   lan_scan_close(scan);
 *   cfg = { .mode = LAN_MODE_JOIN, .local_ip_be, .netmask_be,
 *           .target_ip_be = room.ip_be, .target_port = room.port };
 *   t = lan_transport_create(&cfg); net_vita_link_lan(t); ...iface, arq...
 *   fe_np_set_gb_protocol(...); fe_np_start*(&npc) with npc.is_host = 0;
 *   then gblink_begin(0) as on the PSP.
 *  teardown (either side):
 *   fe_host_gblink_close() as on the PSP; fe_np_stop();
 *   net_vita_bulk_enable(0); net_vita_link_none();
 *   lan_transport_destroy(t); net_vita_release(NET_VITA_INFRA);
 *
 * B. Direct IP / internet -------------------------------------------------
 *  host: as A, plus .port_tries = 1 if the player forwarded exactly port
 *   19019 on the router (optionally upnp_igd.h does it for him and gives the
 *   public address to show); use NET_VITA_LINK_INTERNET's profile if the
 *   partner is not on the LAN (the host cannot know: offer it as the
 *   "Internet" choice of the Host menu).
 *  join:
 *   net_vita_parse_addr(typed, &ip, &port) == 0, else "Not an address"
 *   optional: a lan_scan with .target_ip_be/.target_port = ip/port shows the
 *    room behind the address (game, compatible, round trip) before joining
 *   cfg = { .mode = LAN_MODE_JOIN, .target_ip_be = ip, .target_port = port,
 *           .local_ip_be, .netmask_be };
 *   ... as A; profile NET_VITA_LINK_INTERNET.
 *
 * C. Ad hoc ----------------------------------------------------------------
 *   net_vita_claim(NET_VITA_ADHOC)             (refused while LAN is in use)
 *   vadhoc_net_init(NULL);
 *   vadhoc_dialog_set_group(group);            (optional, see the header)
 *   vadhoc_dialog_start(host ? VADHOC_DIALOG_CREATE : VADHOC_DIALOG_JOIN,
 *                       host ? 0 : 60000000);
 *   every frame while vadhoc_dialog_poll() == VADHOC_DLG_RUNNING:
 *      draw the frame; vadhoc_dialog_update(back buffer...); flip
 *   st = vadhoc_dialog_poll(); vadhoc_dialog_end();
 *   st != VADHOC_DLG_OK: vadhoc_net_term(); net_vita_release(ADHOC); toast
 *   vadhoc_transport_open();
 *   net_vita_link_adhoc(); net_vita_link_iface(&tp);
 *   net_vita_arq_profile(NET_VITA_LINK_ADHOC, ...); fe_np_set_arq_timers();
 *   fe_np_start*(&npc)  (host: is_host = 1; joiner: is_host = 0, its JOIN
 *                        is a PDP broadcast: no scan list on ad hoc)
 *   gblink_begin() as on the PSP; bulk hooks as in A.
 *   once a second: vadhoc_transport_connected() == 0 -> "link lost" path
 *  teardown:
 *   fe_host_gblink_close(); fe_np_stop(); net_vita_bulk_enable(0);
 *   net_vita_link_none(); vadhoc_transport_close(); vadhoc_net_term();
 *   net_vita_release(NET_VITA_ADHOC);
 * ========================================================================== */

#ifdef __cplusplus
}
#endif

#endif /* NET_VITA_H */
