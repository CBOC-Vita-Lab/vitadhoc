/* vita_linkcheck.c -- not a program anyone runs: a reference to every entry
 * point of the Vita network files, so that linking it against vitasdk's
 * stub libraries proves each SCE function they call exists under that name
 * (`make -C gpsp/netdrv vita-check`; vita-elf-create then resolves every
 * import to a NID).  The calls are behind a condition the compiler cannot
 * fold, so nothing is discarded as dead.
 */
#include <stdio.h>
#include <string.h>

#include "netdrv.h"
#include "transport_lan.h"
#include "transport_vadhoc.h"
#include "upnp_igd.h"
#include "../vita/net_vita.h"

static void deliver(void *u, const void *b, size_t n, uint8_t s)
{
   (void)u; (void)b; (void)n; (void)s;
}

int main(int argc, char **argv)
{
   nd_transport tp;
   nd_callbacks cb;
   nd_config nc;
   netdrv *nd;
   uint8_t mac[6] = { 0 }, buf[LAN_BULK_MAX];
   char text[64];
   uint32_t ip = 0, a = 0, b = 0;
   uint16_t port = 0;
   volatile int go = argc > 1000;      /* never true; never provably false */
   (void)argv;

   if (!go)
      return 0;

   /* vita/net_vita.c */
   {
      net_vita_wifi w;
      net_vita_init();
      net_vita_wifi_status(&w);
      net_vita_host_string(LAN_PORT_DEFAULT, text, sizeof(text));
      net_vita_parse_addr(text, &ip, &port);
      net_vita_claim(NET_VITA_INFRA);
      net_vita_arq_profile(NET_VITA_LINK_LAN, &a, &b);
      printf("%u %s %u %d\n", (unsigned)net_vita_last_sce_error(), w.ssid,
             net_vita_rx_depth(), net_vita_use());
   }

   /* netdrv/transport_lan.c */
   {
      lan_config cfg;
      lan_scan_config sc;
      lan_room_info info;
      lan_room rooms[LAN_SCAN_MAX];
      lan_stats st;
      lan_transport *t;
      lan_scan *s;

      memset(&cfg, 0, sizeof(cfg));
      memset(&sc, 0, sizeof(sc));
      memset(&info, 0, sizeof(info));
      lan_local_ipv4(&a, &b);
      info.compat = lan_compat_hash("gpSP v1.0", "vita");
      s = lan_scan_open(&sc);
      lan_scan_poll(s);
      lan_scan_rooms(s, rooms, LAN_SCAN_MAX);
      lan_scan_close(s);
      lan_discovery_scan(&sc, rooms, LAN_SCAN_MAX, 100);
      t = lan_transport_create(&cfg);
      lan_transport_announce(t, &info);
      lan_transport_set_fault(t, 0, 0, 0, 0, 0);
      lan_transport_bulk_open(t, 16, "GBLK");
      lan_transport_bulk_send(t, mac, buf, 16);
      lan_transport_bulk_recv(t, buf, sizeof(buf));
      lan_transport_bulk_close(t);
      lan_transport_get_stats(t, &st);
      lan_transport_addr_str(t, text, sizeof(text));
      lan_format_addr(lan_transport_ip(t), lan_transport_port(t), text,
                      sizeof(text));
      lan_mac_from_addr(ip, port, mac);
      lan_addr_from_mac(mac, &ip, &port);
      printf("%s %u %u %s\n", text, lan_transport_rx_depth(t),
             lan_transport_bulk_depth(t), lan_last_error());
      net_vita_link_lan(t);
      net_vita_link_iface(&tp);
      net_vita_bulk_enable(1);
      net_vita_bulk_send(mac, buf, 16);
      net_vita_bulk_recv(buf, sizeof(buf));
      memset(&cb, 0, sizeof(cb));
      memset(&nc, 0, sizeof(nc));
      cb.deliver = deliver;
      nc.protocol = "gpSP v1.0";
      nd = netdrv_create(&tp, &cb, &nc);
      netdrv_host(nd);
      netdrv_pump(nd, 1);
      netdrv_destroy(nd);
      net_vita_link_none();
      lan_transport_destroy(t);
      net_vita_release(NET_VITA_INFRA);
   }

   /* netdrv/upnp_igd.c */
   {
      upnp_igd *u = upnp_igd_open(a, LAN_PORT_DEFAULT, LAN_PORT_DEFAULT,
                                  "GBAdhoc");
      upnp_igd_poll(u);
      printf("%d %s %s\n", upnp_igd_state(u), upnp_igd_external_ip(u),
             upnp_igd_error(u));
      upnp_igd_remove(u);
      upnp_igd_close(u);
   }

   /* netdrv/transport_vadhoc.c */
   {
      vadhoc_stats st;
      vadhoc_peer peers[4];

      vadhoc_net_init(NULL);
      vadhoc_dialog_set_group("GC07AMKE");
      vadhoc_dialog_start(VADHOC_DIALOG_CREATE, 0);
      vadhoc_dialog_update(buf, buf, buf, 960, 544, 960, 0);
      vadhoc_dialog_poll();
      vadhoc_dialog_abort();
      vadhoc_dialog_end();
      vadhoc_set_tx_thread(1);
      vadhoc_set_broadcast_mode(0);
      vadhoc_set_fault(0, 0, 0, 0);
      vadhoc_transport_open();
      net_vita_link_adhoc();
      vadhoc_transport_iface(&tp);
      vadhoc_bulk_open(16, "GBLK");
      vadhoc_bulk_send(mac, buf, 16);
      vadhoc_bulk_recv(buf, sizeof(buf));
      vadhoc_bulk_close();
      vadhoc_peers(peers, 4);
      vadhoc_local_mac(mac);
      vadhoc_get_stats(&st);
      printf("%d %u %u %08x %s\n", vadhoc_transport_connected(),
             vadhoc_rx_depth(), vadhoc_bulk_depth(),
             (unsigned)vadhoc_last_sce_error(), vadhoc_stage());
      vadhoc_transport_close();
      vadhoc_net_term();
   }
   net_vita_term();
   return 0;
}
