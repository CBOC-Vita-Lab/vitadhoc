/* share_vita.c -- "Share game data" (GBAdhoc 4.0, docs/SAVE-SHARE.md) on the
 * Vita: the radio half of psp/share_psp.h, on Wi-Fi.
 *
 * The PSP's share room is an ad hoc group with the transport's bulk lane and
 * nothing else on it.  Here it is a LAN room (transport_lan) of its own kind,
 * LAN_KIND_SHARE, named exactly as the PSP names the group (room_label.h:
 * "GS07DATA" for Room 7), so the screens (psp/ui_share.inc) and the protocol
 * (frontend-common/fe_share.c) are the PSP's unchanged:
 *
 *   share_net_up(1, group)  the sharer opens a room and announces it; its
 *                           bulk lane takes a HELLO from anybody (the
 *                           protocol has its own challenge and the server is
 *                           read-only)
 *   share_net_up(0, group)  the guest looks for that room on the network
 *                           (lan_discovery_scan) and keeps its address: the
 *                           client's "broadcast" HELLO goes there
 *
 * A console's MAC in the protocol is its address on the lane (transport_lan's
 * fake MAC: the IPv4 address and port), which is how the sharer answers.
 * Over the internet that address would be the one behind the router, so
 * sharing is Wi-Fi only: both Vitas on the same network or hotspot.
 *
 * Ad hoc (no router) is not offered for sharing yet: on the Vita an ad hoc
 * group comes up through the system's own dialog drawn over the frames, and
 * the share screens are blocking loops of their own. */
#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>

#include "share_psp.h"
#include "transport_lan.h"
#include "net_vita.h"
#include "room_label.h"
#include "config_psp.h"
#include "fe_evt.h"
#include "plat.h"
#include "app_vita.h"

/* The bulk lane's ring: as the PSP's (share_psp.c). */
#define SHARE_BULK_SLOTS 32
/* How long a guest listens for the room before saying it is not there. */
#define SHARE_SCAN_MS 4000

static lan_transport *g_lan;
static int g_claimed;
static int g_host;
static uint8_t g_target[6];
static int g_have_target;

static const uint8_t BCAST[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

unsigned share_now_ms(void)
{
   return (unsigned)(plat_time_us() / 1000u);
}

static uint32_t share_compat(void)
{
   /* The protocol refuses a different generation itself (REFUSE, "update
    * both"); the room's hash only keeps the list honest. */
   return lan_compat_hash("GBAdhoc share", "2");
}

static void share_release(void)
{
   if (g_lan)
   {
      lan_transport_bulk_close(g_lan);
      lan_transport_destroy(g_lan);
      g_lan = NULL;
   }
   if (g_claimed)
      net_vita_release(NET_VITA_INFRA);
   g_claimed = 0;
   g_have_target = 0;
}

static int share_find_room(const char *group, const net_vita_wifi *w,
                           uint16_t disc_port, unsigned flags, lan_room *out)
{
   static lan_room rooms[LAN_SCAN_MAX];
   lan_scan_config sc;
   int n, i;

   memset(&sc, 0, sizeof(sc));
   sc.disc_port = disc_port;
   sc.local_ip_be = w->ip_be;
   sc.netmask_be = w->netmask_be;
   sc.compat = share_compat();
   sc.flags = flags;
   n = lan_discovery_scan(&sc, rooms, LAN_SCAN_MAX, SHARE_SCAN_MS);
   for (i = 0; i < n; i++)
   {
      fe_evt("share_scan_room name=%s kind=%d addr=%s compatible=%d",
             rooms[i].info.name, rooms[i].info.kind, rooms[i].addr,
             rooms[i].compatible);
      if (rooms[i].info.kind == LAN_KIND_SHARE &&
          !strcmp(rooms[i].info.name, group))
      {
         *out = rooms[i];
         return 1;
      }
   }
   return 0;
}

int share_net_up(int host, const char *group)
{
   net_vita_wifi w;
   lan_config c;
   lan_room room;
   uint16_t disc_port = 0;
   unsigned flags;
   int rc = ADHOC_OK;

   if (g_lan)
      return ADHOC_OK;
   if (!link_net_info(&disc_port) || !net_vita_wifi_status(&w))
   {
      fe_evt("share_net_up host=%d group=%s rc=%d why=no_wifi", host, group,
             ADHOC_ERR_WLAN_OFF);
      return ADHOC_ERR_WLAN_OFF;
   }
   if (net_vita_claim(NET_VITA_INFRA) != 0)
   {
      fe_evt("share_net_up host=%d group=%s rc=%d why=radio_busy", host,
             group, ADHOC_ERR_NP_START);
      return ADHOC_ERR_NP_START;
   }
   g_claimed = 1;
   g_host = host;
   /* 127.x is the twin's "Wi-Fi": broadcasts stay on the loopback. */
   flags = (ntohl(w.ip_be) >> 24) == 127 ? LAN_F_LOOPBACK : 0;

   memset(&c, 0, sizeof(c));
   c.mode = host ? LAN_MODE_HOST : LAN_MODE_JOIN;
   c.disc_port = disc_port;
   c.local_ip_be = w.ip_be;
   c.netmask_be = w.netmask_be;
   c.flags = flags;
   if (!host)
   {
      if (!share_find_room(group, &w, disc_port, flags, &room))
      {
         share_release();
         fe_evt("share_net_up host=0 group=%s rc=%d why=not_found", group,
                ADHOC_ERR_NO_GROUP);
         return ADHOC_ERR_NO_GROUP;
      }
      c.target_ip_be = room.ip_be;
      c.target_port = room.port;
      memcpy(g_target, room.mac, 6);
      g_have_target = 1;
   }
   g_lan = lan_transport_create(&c);
   if (!g_lan)
   {
      fe_evt("share_net_up host=%d group=%s why=\"%s\"", host, group,
             lan_last_error());
      share_release();
      return ADHOC_ERR_NP_START;
   }
   if (lan_transport_bulk_open(g_lan, SHARE_BULK_SLOTS, FE_SHARE_MAGIC) != 0)
   {
      share_release();
      return ADHOC_ERR_NP_START;      /* "could not start": memory */
   }
   lan_transport_bulk_any(g_lan, 1);
   if (host)
   {
      lan_room_info ri;
      int num = room_number(group);
      memset(&ri, 0, sizeof(ri));
      snprintf(ri.name, sizeof(ri.name), "%s", group);
      ri.number = (uint8_t)(num < 0 ? 0 : num);
      ri.kind = LAN_KIND_SHARE;
      ri.players = 1;
      ri.max_players = 2;
      ri.flags = LAN_ROOM_F_OPEN;
      snprintf(ri.nick, sizeof(ri.nick), "%.15s", g_pcfg.nick);
      snprintf(ri.title, sizeof(ri.title), "Game data");
      memcpy(ri.code, ROOM_SHARE_CODE, 4);
      ri.compat = share_compat();
      lan_transport_announce(g_lan, &ri);
   }
   {
      char addr[32];
      lan_transport_addr_str(g_lan, addr, sizeof(addr));
      fe_evt("share_net_up host=%d group=%s rc=%d addr=%s loopback=%d", host,
             group, rc, addr, (flags & LAN_F_LOOPBACK) != 0);
   }
   return rc;
}

void share_net_down(void)
{
   lan_stats st;
   if (!g_lan)
      return;
   lan_transport_get_stats(g_lan, &st);
   fe_evt("share_net_down bulk_tx=%u bulk_txfail=%u bulk_rx=%u bulk_drop=%u "
          "bulk_foreign=%u", (unsigned)st.bulk_tx, (unsigned)st.bulk_txfail,
          (unsigned)st.bulk_rx, (unsigned)st.bulk_drop,
          (unsigned)st.bulk_foreign);
   if (g_host)
      lan_transport_announce(g_lan, NULL);
   share_release();
}

int share_net_is_up(void)
{
   return g_lan != NULL;
}

int share_net_connected(void)
{
   net_vita_wifi w;
   return g_lan != NULL && net_vita_wifi_status(&w);
}

void share_net_send(void *user, const uint8_t mac[6], const void *buf,
                    size_t len)
{
   (void)user;
   if (!g_lan)
      return;
   /* The client's HELLO is addressed to everybody: here that is the room
    * the scan found. */
   if (!memcmp(mac, BCAST, 6))
   {
      if (!g_have_target)
         return;
      mac = g_target;
   }
   lan_transport_bulk_send(g_lan, mac, buf, len);
}

int share_net_recv(void *buf, size_t cap)
{
   return g_lan ? lan_transport_bulk_recv(g_lan, buf, cap) : 0;
}

void share_net_local_mac(uint8_t mac[6])
{
   if (!g_lan)
   {
      memset(mac, 0, 6);
      return;
   }
   lan_mac_from_addr(lan_transport_ip(g_lan), lan_transport_port(g_lan), mac);
}
