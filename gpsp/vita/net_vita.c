/* net_vita.c -- see net_vita.h.
 *
 * TWO HALVES.  The stack's life and the Wi-Fi's state are Sony calls:
 * compiled and linked against vitasdk only, never run here.  Everything
 * else -- the one-radio rule, which transport the session is on, the bulk
 * lane, the ARQ profiles -- is plain C, and the Linux twin of the app
 * (vita/hostcore/net_host.c) compiles this same file with -DNET_VITA_HOST
 * and supplies the first half itself: real sockets on the loopback and a
 * Wi-Fi status read from a file. */
#include "net_vita.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <arpa/inet.h>

#ifndef NET_VITA_HOST
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/sysmodule.h>
#endif

#include "transport_vadhoc.h"

/* vitaQuake's size.  The pool holds the stack's socket buffers and control
 * blocks; our use is one or two UDP sockets with 256 KiB receive buffers. */
#define NET_VITA_POOL (1024 * 1024)
#define NETCTL_NOT_TERMINATED 0x80412102u   /* sceNetCtlInit: already up */

static struct
{
   int      up;
   int      module_ours, net_ours, netctl_ours;
   void    *pool;
   int      use;                 /* NET_VITA_NONE / INFRA / ADHOC */
   int      use_count;           /* nested claims of the same use */
   int      link;                /* 0 none, 1 lan, 2 adhoc */
   lan_transport *lan;
   uint32_t last_sce;
} N;

uint32_t net_vita_last_sce_error(void) { return N.last_sce; }

#ifndef NET_VITA_HOST
int net_vita_init(void)
{
   SceNetInitParam p;
   int rc;

   if (N.up)
      return 0;
   N.module_ours = N.net_ours = N.netctl_ours = 0;
   if (sceSysmoduleIsLoaded(SCE_SYSMODULE_NET) != SCE_SYSMODULE_LOADED)
   {
      rc = sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
      if (rc < 0)
      {
         N.last_sce = (uint32_t)rc;
         return rc;
      }
      N.module_ours = 1;
   }
   N.pool = malloc(NET_VITA_POOL);
   if (!N.pool)
   {
      net_vita_term();
      return -1;
   }
   p.memory = N.pool;
   p.size = NET_VITA_POOL;
   p.flags = 0;
   rc = sceNetInit(&p);
   if (rc >= 0)
      N.net_ours = 1;
   else
   {
      /* EBUSY: somebody (newlib's socket(), another library) was first.
       * Their pool serves; ours goes back. */
      free(N.pool);
      N.pool = NULL;
      if ((unsigned)rc != (unsigned)SCE_NET_ERROR_EBUSY)
      {
         N.last_sce = (uint32_t)rc;
         net_vita_term();
         return rc;
      }
   }
   rc = sceNetCtlInit();
   if (rc >= 0)
      N.netctl_ours = 1;
   else if ((unsigned)rc != NETCTL_NOT_TERMINATED)
   {
      N.last_sce = (uint32_t)rc;
      net_vita_term();
      return rc;
   }
   N.up = 1;
   return 0;
}

void net_vita_term(void)
{
   if (N.netctl_ours)
      sceNetCtlTerm();
   if (N.net_ours)
      sceNetTerm();
   free(N.pool);
   N.pool = NULL;
   if (N.module_ours)
      sceSysmoduleUnloadModule(SCE_SYSMODULE_NET);
   N.module_ours = N.net_ours = N.netctl_ours = 0;
   N.up = 0;
}

/* ---- Wi-Fi ----------------------------------------------------------------- */

int net_vita_wifi_status(net_vita_wifi *out)
{
   SceNetCtlInfo info;
   struct in_addr a;
   int st = -1;

   memset(out, 0, sizeof(*out));
   out->rssi_pct = -1;
   out->state = -1;
   if (!N.up || sceNetCtlInetGetState(&st) < 0)
      return 0;
   out->state = st;
   if (st != SCE_NETCTL_STATE_CONNECTED)
      return 0;

   memset(&info, 0, sizeof(info));
   if (sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_IP_ADDRESS, &info) < 0 ||
       inet_pton(AF_INET, info.ip_address, &a) != 1 || !a.s_addr)
      return 0;
   out->ip_be = a.s_addr;
   snprintf(out->ip, sizeof(out->ip), "%s", info.ip_address);

   memset(&info, 0, sizeof(info));
   if (sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_NETMASK, &info) >= 0 &&
       inet_pton(AF_INET, info.netmask, &a) == 1)
      out->netmask_be = a.s_addr;
   if (!out->netmask_be)
      out->netmask_be = htonl(0xFFFFFF00u);    /* a home network's /24 */
   out->bcast_be = (out->ip_be & out->netmask_be) | ~out->netmask_be;

   memset(&info, 0, sizeof(info));
   if (sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_SSID, &info) >= 0)
      snprintf(out->ssid, sizeof(out->ssid), "%s", info.ssid);
   memset(&info, 0, sizeof(info));
   if (sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_RSSI_PERCENTAGE, &info) >= 0 &&
       info.rssi_percentage <= 100)
      out->rssi_pct = (int)info.rssi_percentage;
   out->connected = 1;
   return 1;
}
#endif /* !NET_VITA_HOST */

const char *net_vita_host_string(uint16_t port, char *buf, size_t n)
{
   net_vita_wifi w;
   if (!buf || !n)
      return "";
   buf[0] = '\0';
   if (net_vita_wifi_status(&w))
      lan_format_addr(w.ip_be, port, buf, n);
   return buf;
}

int net_vita_parse_addr(const char *text, uint32_t *ip_be, uint16_t *port)
{
   return lan_parse_addr(text, ip_be, port, LAN_PORT_DEFAULT);
}

/* ---- one radio, one use ---------------------------------------------------- */

int net_vita_claim(int use)
{
   if (use != NET_VITA_INFRA && use != NET_VITA_ADHOC)
      return -1;
   if (N.use != NET_VITA_NONE && N.use != use)
      return -1;
   N.use = use;
   N.use_count++;
   return 0;
}

void net_vita_release(int use)
{
   if (N.use != use || !N.use_count)
      return;
   if (--N.use_count == 0)
      N.use = NET_VITA_NONE;
}

int net_vita_use(void) { return N.use; }

/* ---- the session's link ---------------------------------------------------- */

void net_vita_link_lan(lan_transport *t)
{
   N.lan = t;
   N.link = t ? 1 : 0;
}

void net_vita_link_adhoc(void)
{
   N.lan = NULL;
   N.link = 2;
}

void net_vita_link_none(void)
{
   N.lan = NULL;
   N.link = 0;
}

int net_vita_link_iface(nd_transport *out)
{
   memset(out, 0, sizeof(*out));
   if (N.link == 1)
      lan_transport_iface(N.lan, out);
   else if (N.link == 2)
      vadhoc_transport_iface(out);
   else
      return -1;
   return 0;
}

int net_vita_bulk_enable(int on)
{
   if (N.link == 1)
   {
      if (!on)
      {
         lan_transport_bulk_close(N.lan);
         return 0;
      }
      return lan_transport_bulk_open(N.lan, NET_VITA_BULK_SLOTS,
                                     "GBLK" /* FE_GBLINK_BULK_MAGIC */);
   }
   if (N.link == 2)
   {
      if (!on)
      {
         vadhoc_bulk_close();
         return 0;
      }
      return vadhoc_bulk_open(NET_VITA_BULK_SLOTS, "GBLK");
   }
   return on ? -1 : 0;
}

int net_vita_bulk_send(const uint8_t mac[6], const void *buf, size_t len)
{
   if (N.link == 1)
      return lan_transport_bulk_send(N.lan, mac, buf, len);
   if (N.link == 2)
      return vadhoc_bulk_send(mac, buf, len);
   return -1;
}

int net_vita_bulk_recv(void *buf, size_t cap)
{
   if (N.link == 1)
      return lan_transport_bulk_recv(N.lan, buf, cap);
   if (N.link == 2)
      return vadhoc_bulk_recv(buf, cap);
   return 0;
}

unsigned net_vita_rx_depth(void)
{
   if (N.link == 1)
      return lan_transport_rx_depth(N.lan);
   if (N.link == 2)
      return vadhoc_rx_depth();
   return 0;
}

/* ---- ARQ profiles ----------------------------------------------------------
 *
 * Measured, not guessed: `make -C gpsp/netdrv bench-lan` sends one small
 * reliable payload per 16.7 ms frame each way (the link session's traffic)
 * through transport_lan's fault shim and reports how late payloads are
 * DELIVERED.  ">50" below is the share delivered more than 50 ms later than
 * the link's own latency -- for a lockstep game, the share that can stall a
 * frame.  Floor / first-retransmission cap in ms, ceiling 2.5 s throughout:
 *
 *   link (loss, one-way latency, jitter)   30/30        30/40       100/none    200/none
 *   Wi-Fi LAN       1 %,  2 ms,  4 ms      0.1 %        0.0 %        5 %         11 %
 *   busy Wi-Fi      3 %,  5 ms, 25 ms      0.8 % (66)   1.9 % (38)  14 %         30 %
 *   internet        1 %, 30 ms, 10 ms      0.0 % (200)  0.0 % (200)  5 %         16 %
 *   far internet    2 %, 80 ms, 30 ms      0.5 % (301)  1.3 % (301) 28 %         24 %
 *   ad hoc, guessed 2 %, 10 ms, 15 ms      0.0 % (82)   1.1 % (33)  11 %         15 %
 *   ad hoc, no loss 0 %,  8 ms, 12 ms      0.0 % (42)   0.0 % (1)    0.3 %        0.8 %
 *   (in brackets: retransmissions per 100 payloads)
 *
 * What it says:
 *  - A first-retransmission cap BELOW the ceiling ("pinned", netdrv.c
 *    rto_recovery_pinned) is what keeps the tail short.  netdrv acknowledges
 *    cumulatively, so with an honest RTO one lost datagram holds back every
 *    payload behind it for a whole RTO; on an 80 ms path that was 22 % of
 *    all payloads late by more than 50 ms.
 *  - The price is duplicates: two to three copies of every frame once the
 *    round trip exceeds the cap.  At the session's traffic that is 10-50
 *    kB/s -- nothing for Wi-Fi or broadband, and the Vita's CPU does not
 *    care.  It was the PSP that could not afford it: there a send costs up
 *    to a millisecond of the emulation thread (ADR-0017/0021), hence its
 *    100-200 ms floors.  Those floors are the wrong lesson for this
 *    console: the two right-hand columns.
 *  - The link session's own remedies are worth more than any timer and are
 *    cheap here: cfg.gblink_input_copies = 4 (the last four frames' buttons
 *    in one unreliable datagram per frame) and cfg.gblink_nack = 1.  The
 *    PSP ships with copies off because the extra datagram cost it 1 ms a
 *    frame.
 */

void net_vita_arq_profile(int link, uint32_t *rto_first_max_us,
                          uint32_t *rto_min_us)
{
   uint32_t first, floor_us;
   switch (link)
   {
      case NET_VITA_LINK_ADHOC:
         /* HARDWARE: nobody has measured the Vita's ad hoc radio.  If it
          * loses nothing (802.11 retransmits below us, as the PSP's did)
          * every profile ties; a 40 ms cap then costs 1 duplicate per 100
          * payloads where 30 ms costs 42, and it still recovers a real loss
          * inside three frames.  Read `net_stats srtt_us` and the
          * transport's tx_us on the first two-console session: an srtt well
          * above 40 ms or a tx_us in the hundreds means raise this (and
          * turn vadhoc_set_tx_thread on). */
         first = 40000;
         floor_us = 30000;
         break;
      case NET_VITA_LINK_INTERNET:
         /* The same numbers as the LAN by measurement.  Kept as a kind of
          * its own because it is the one most likely to be tuned apart: a
          * path that cannot take the duplicates wants 50000/50000 (half the
          * copies, 1-3 % of payloads late instead of 0-0.5 %). */
         first = 30000;
         floor_us = 30000;
         break;
      case NET_VITA_LINK_LAN:
      default:
         /* The desktop twin's profile (sdl/Makefile, ADR-0010): what this
          * code base has been validated against since Gate 3. */
         first = 30000;
         floor_us = 30000;
         break;
   }
   if (rto_first_max_us)
      *rto_first_max_us = first;
   if (rto_min_us)
      *rto_min_us = floor_us;
}
