/* net_host.c -- vita/net_vita.h on Linux: the network of the app's twin.
 *
 * The portable half of net_vita.c (the one-radio rule, the session's link,
 * the bulk lane, the ARQ profiles) is that very file, compiled here with
 * -DNET_VITA_HOST.  This file is the other half -- what is sceNet and
 * sceNetCtl on the console:
 *
 *   the stack    nothing to start: sockets are the kernel's
 *   the Wi-Fi    a STAND-IN the tests control.  <root>/nethost.ini says what
 *                the console's Wi-Fi "is"; the file is read again whenever
 *                it changes, so a test takes the network away in the middle
 *                of a session by rewriting it.  With no file the twin is
 *                NOT connected -- which keeps app_tests.py's runs, on
 *                simulated time, away from real sockets.
 *
 *       wifi = 1               0 off, 1 connected, 2 connecting
 *       ssid = TwinNet
 *       ip = 127.0.0.1         the loopback: two twins on one machine.  The
 *       mask = 255.0.0.0       transports are told LAN_F_LOOPBACK by
 *                              link_vita.c when the address is 127.x
 *       rssi = 80
 *
 *   ad hoc       the fake radio cell of netdrv/tests/vadhoc_fake.c:
 *
 *       adhoc_station = 1      this twin (1..15)
 *       adhoc_peers = 2        the other stations, comma separated
 *       adhoc_base = 23000     their UDP ports are base + station
 *       adhoc_dialog_frames = 30   how long the "system dialog" stays up
 *       adhoc_dialog_result = 0    0 connected, 1 the player closed it,
 *                                  anything else: that error code
 *
 *                With no adhoc_station the cell does not exist: the dialog
 *                ends in an error, as a console with its radio off would.
 *
 *                While the radio is claimed for ad hoc the Wi-Fi reads as
 *                off, as on the console (one radio).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include <arpa/inet.h>

#define NET_VITA_HOST
#include "../net_vita.c"

#include "../plat.h"
#include "fe_evt.h"
#include "fe_util.h"
#include "tests/vadhoc_fake.h"

static struct
{
   int      have;                /* the file was read at least once */
   char     path[600];
   struct timespec mtime;
   long     size;
   uint64_t checked_ms;
   int      wifi;                /* 0 off, 1 connected, 2 connecting */
   char     ssid[33], ip[16], mask[16];
   int      rssi;
   int      station, base, dlg_frames, dlg_result;
   int      peers[8], n_peers;
} H;

static uint64_t wall_ms(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void host_apply_fake(void)
{
   vadhoc_fake_opts o;
   memset(&o, 0, sizeof(o));
   o.broadcast_works = 1;
   o.dialog_frames = H.dlg_frames > 0 ? H.dlg_frames : 30;
   /* no station number: no cell (0x80412103: "not connected") */
   o.dialog_result = H.station > 0 ? H.dlg_result : (int)0x80412103u;
   vadhoc_fake_setup(H.station, (uint16_t)H.base, H.peers, H.n_peers);
   vadhoc_fake_options(&o);
}

/* Read <root>/nethost.ini if it is new or has changed.  `force`: now, not at
 * most five times a second. */
static void host_refresh(int force)
{
   struct stat st;
   uint64_t now = wall_ms();
   char v[64];
   const char *p;

   if (!H.path[0])
      snprintf(H.path, sizeof(H.path), "%s/nethost.ini", plat_data_root());
   if (H.have && !force && now - H.checked_ms < 200)
      return;
   H.checked_ms = now;
   if (stat(H.path, &st) != 0)
   {
      if (H.have && H.size >= 0)
         fe_evt("nethost gone");
      if (!H.have || H.size >= 0)
      {
         H.station = 0;
         host_apply_fake();
      }
      H.have = 1;
      H.size = -1;
      H.wifi = 0;
      return;
   }
   if (H.have && H.size == (long)st.st_size &&
       H.mtime.tv_sec == st.st_mtim.tv_sec &&
       H.mtime.tv_nsec == st.st_mtim.tv_nsec)
      return;
   H.have = 1;
   H.size = (long)st.st_size;
   H.mtime = st.st_mtim;

   H.wifi = (int)fe_ini_get_int(H.path, "wifi", 0);
   if (!fe_ini_get(H.path, "ssid", H.ssid, sizeof(H.ssid)))
      snprintf(H.ssid, sizeof(H.ssid), "TwinNet");
   if (!fe_ini_get(H.path, "ip", H.ip, sizeof(H.ip)))
      snprintf(H.ip, sizeof(H.ip), "127.0.0.1");
   if (!fe_ini_get(H.path, "mask", H.mask, sizeof(H.mask)))
      snprintf(H.mask, sizeof(H.mask), "255.0.0.0");
   H.rssi = (int)fe_ini_get_int(H.path, "rssi", 80);
   H.station = (int)fe_ini_get_int(H.path, "adhoc_station", 0);
   H.base = (int)fe_ini_get_int(H.path, "adhoc_base", 23000);
   H.dlg_frames = (int)fe_ini_get_int(H.path, "adhoc_dialog_frames", 30);
   H.dlg_result = (int)fe_ini_get_int(H.path, "adhoc_dialog_result", 0);
   H.n_peers = 0;
   if (fe_ini_get(H.path, "adhoc_peers", v, sizeof(v)))
      for (p = v; *p && H.n_peers < 8; )
      {
         char *end;
         long n = strtol(p, &end, 10);
         if (end == p)
            break;
         H.peers[H.n_peers++] = (int)n;
         p = *end == ',' ? end + 1 : end;
      }
   host_apply_fake();
   fe_evt("nethost wifi=%d ssid=%s ip=%s adhoc_station=%d peers=%d "
          "dialog=%d/%d", H.wifi, H.ssid, H.ip, H.station, H.n_peers,
          H.dlg_frames, H.dlg_result);
}

int net_vita_init(void)
{
   N.up = 1;
   host_refresh(1);
   return 0;
}

void net_vita_term(void)
{
   N.up = 0;
}

int net_vita_wifi_status(net_vita_wifi *out)
{
   struct in_addr a;

   memset(out, 0, sizeof(*out));
   out->rssi_pct = -1;
   out->state = -1;
   if (!N.up)
      return 0;
   host_refresh(0);
   /* SceNetCtlState: 0 disconnected, 1 connecting, 3 connected.  One radio:
    * in ad hoc use the access point is gone. */
   if (N.use == NET_VITA_ADHOC || H.wifi == 0)
   {
      out->state = 0;
      return 0;
   }
   if (H.wifi != 1)
   {
      out->state = 1;
      return 0;
   }
   out->state = 3;
   if (inet_pton(AF_INET, H.ip, &a) != 1 || !a.s_addr)
      return 0;
   out->ip_be = a.s_addr;
   snprintf(out->ip, sizeof(out->ip), "%s", H.ip);
   if (inet_pton(AF_INET, H.mask, &a) == 1)
      out->netmask_be = a.s_addr;
   if (!out->netmask_be)
      out->netmask_be = htonl(0xFFFFFF00u);
   out->bcast_be = (out->ip_be & out->netmask_be) | ~out->netmask_be;
   snprintf(out->ssid, sizeof(out->ssid), "%s", H.ssid);
   out->rssi_pct = H.rssi;
   out->connected = 1;
   return 1;
}
