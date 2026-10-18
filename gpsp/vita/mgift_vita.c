/* mgift_vita.c -- Mystery Gift on the Vita (the PSP's since 3.0): a phone
 * running Mystery Gift Station sends a Wonder Card over Wi-Fi, and the
 * emulated Pokémon FireRed, LeafGreen or Emerald receives it through its own
 * MYSTERY GIFT menu from an emulated distribution cart (psp/mgift_cart.c,
 * unchanged).  Nothing is written into the save by the emulator.
 *
 * What differs from the PSP (psp/main_psp.c mgift_*, psp/mgift_net.c):
 *   - The network.  The PSP creates a connection profile for the phone's open
 *     hotspot and joins it itself.  A Vita application cannot choose a Wi-Fi
 *     network: the player connects the Vita to the phone's hotspot (or any
 *     network the phone is on) in the system's Settings, and this file only
 *     checks that Wi-Fi is up.
 *   - The socket is a BSD socket (newlib on the console, the twin's own), the
 *     same UDP port as the PSP's (MGNET_LISTEN_PORT, 21064; vita.ini
 *     mgift_port for tests), taking the phone's broadcast parcels.
 *   - The radio is claimed for the infrastructure use while listening
 *     (net_vita_claim), so ad hoc cannot start under it, and it is refused
 *     while a link session holds the network -- the PSP's interlock.
 *
 * The screen (ui_psp.c screen_mgift, the Play together page's "Mystery Gift"
 * row) and the shortcut (Controls > Mystery Gift, SELECT+DOWN) are the PSP's.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#ifndef __vita__
#include <fcntl.h>
#endif

#include "mgift_vita.h"
#include "mgift_net.h"
#include "mgift_cart.h"
#include "mgift_payload.h"
#include "net_vita.h"
#include "app_vita.h"
#include "osd_psp.h"
#include "config_psp.h"
#include "fe_evt.h"
#include "fe_host.h"

static struct
{
   int      on;                  /* listening */
   int      claimed;             /* the radio, NET_VITA_INFRA */
   int      sock;
   unsigned port;
   struct sockaddr_in from;      /* where the last good parcel came from */
   int      have_from;
   int      linked;              /* the game connected to the cart */
   unsigned secs;
   char     ip[20];
   char     game[5];             /* the running cartridge's header code */
   char     l1[64], l2[64];      /* the screen's two lines */
   char     chip[24];
} M = { .sock = -1, .port = MGNET_LISTEN_PORT };

/* ---- the screen's hooks (ui_host_hooks.h section 2) ---------------------- */

int ui_host_mgift_available(void)
{
   /* A GBA game is running: the Play together page offers the row (the
    * cart itself says "This game has no wireless" when the game is not one
    * of the three). */
   return M.game[0] != '\0' || M.on;
}

int         mgift_ui_active(void) { return M.on; }
const char *mgift_ui_line1(void)  { return M.l1; }
const char *mgift_ui_line2(void)  { return M.l2; }

/* The PSP's stored-connection list does not exist here (ui_psp.c's
 * mgnet_config_count() is 0 on the Vita); the name lookup is never reached
 * but must link. */
int mgnet_config_name(int index, char *out, unsigned out_sz)
{
   (void)index;
   if (out && out_sz)
      out[0] = '\0';
   return -1;
}

/* ---- the socket ---------------------------------------------------------- */

static int sock_open(unsigned port)
{
   struct sockaddr_in a;
   int fd, on = 1;

   fd = socket(AF_INET, SOCK_DGRAM, 0);
   if (fd < 0)
      return -1;
   setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
   setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));
#ifdef __vita__
   setsockopt(fd, SOL_SOCKET, SO_NONBLOCK, &on, sizeof(on));
#else
   fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
#endif
   memset(&a, 0, sizeof(a));
   a.sin_family = AF_INET;
   a.sin_port = htons((uint16_t)port);
   a.sin_addr.s_addr = htonl(INADDR_ANY);
   if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0)
   {
      close(fd);
      return -1;
   }
   return fd;
}

static void sock_close(void)
{
   if (M.sock >= 0)
      close(M.sock);
   M.sock = -1;
   M.have_from = 0;
}

/* One datagram; 1 with a valid parcel, 0 otherwise (an invalid MGC2 one is
 * answered "invalid" at once, as on the PSP). */
static int card_poll(mgift_parcel *out)
{
   uint8_t buf[MGIFT_PARCEL_MAX + 1];
   struct sockaddr_in from;
   socklen_t flen = sizeof(from);
   int n;

   if (M.sock < 0)
      return 0;
   memset(&from, 0, sizeof(from));
   n = (int)recvfrom(M.sock, buf, sizeof(buf), 0, (struct sockaddr *)&from,
                     &flen);
   if (n < 16)
      return 0;
   if (mgift_parcel_decode(out, buf, (unsigned)n) != 0)
   {
      if (!memcmp(buf, "MGC2", 4))
      {
         uint8_t ack[12];
         uint32_t id = buf[4] | (buf[5] << 8) | (buf[6] << 16) |
                       ((uint32_t)buf[7] << 24);
         mgift_parcel_ack(ack, id, 2);
         sendto(M.sock, ack, sizeof(ack), 0, (struct sockaddr *)&from,
                sizeof(from));
         fe_evt("mg_parcel_invalid bytes=%d", n);
      }
      return 0;
   }
   M.from = from;
   M.have_from = 1;
   return 1;
}

static void card_reply(uint32_t id, unsigned result)
{
   uint8_t ack[12];
   if (M.sock < 0 || !M.have_from)
      return;
   mgift_parcel_ack(ack, id, result);
   sendto(M.sock, ack, sizeof(ack), 0, (struct sockaddr *)&M.from,
          sizeof(M.from));
}

/* ---- start / stop / frame ------------------------------------------------ */

void mgift_vita_set_port(unsigned port)
{
   if (port > 0 && port < 65536)
      M.port = port;
}

void mgift_vita_game(const char *code4)
{
   if (code4 && code4[0])
      snprintf(M.game, sizeof(M.game), "%.4s", code4);
   else
      M.game[0] = '\0';
}

static void chip(int on)
{
   if (on)
      snprintf(M.chip, sizeof(M.chip), "Mystery Gift");
   osd_chip_session(on && g_pcfg.osd_wireless ? M.chip : NULL);
}

void mgift_vita_stop(int quiet)
{
   if (!M.on)
      return;
   /* The cart first: it says goodbye through the netpacket seam. */
   mgift_cart_stop();
   sock_close();
   if (M.claimed)
      net_vita_release(NET_VITA_INFRA);
   M.claimed = 0;
   fe_evt("mg_stop linked=%d", M.linked);
   M.on = 0;
   M.linked = 0;
   chip(0);
   snprintf(M.l1, sizeof(M.l1), "Not listening");
   M.l2[0] = '\0';
   if (!quiet)
      osd_toast("Mystery Gift off");
}

static void refuse(const char *l1, const char *l2)
{
   snprintf(M.l1, sizeof(M.l1), "%s", l1);
   snprintf(M.l2, sizeof(M.l2), "%s", l2 ? l2 : "");
   osd_toast("%s", l1);
   fe_evt("mg_start_refused why=\"%s\"", l1);
}

void mgift_vita_start(void)
{
   net_vita_wifi w;
   struct in_addr a;

   if (M.on)
   {
      osd_toast("Mystery Gift is already listening");
      return;
   }
   /* The PSP's interlock, direction 1: a link holds the network. */
   if (link_net_busy())
   {
      refuse("Leave the link session first", NULL);
      return;
   }
   if (!net_vita_wifi_status(&w))
   {
      refuse("Connect to Wi-Fi first",
             "Join the phone's hotspot in the Vita's Settings");
      return;
   }
   if (net_vita_claim(NET_VITA_INFRA) != 0)
   {
      refuse("Leave ad hoc first", NULL);
      return;
   }
   M.claimed = 1;
   M.sock = sock_open(M.port);
   if (M.sock < 0)
   {
      net_vita_release(NET_VITA_INFRA);
      M.claimed = 0;
      refuse("Could not open the socket", NULL);
      return;
   }
   if (mgift_cart_start() != 0)
   {
      sock_close();
      net_vita_release(NET_VITA_INFRA);
      M.claimed = 0;
      refuse("This game has no wireless",
             "Mystery Gift needs FireRed, LeafGreen or Emerald");
      return;
   }
   a.s_addr = w.ip_be;
   snprintf(M.ip, sizeof(M.ip), "%s", inet_ntoa(a));
   M.on = 1;
   M.linked = 0;
   M.secs = 0;
   snprintf(M.l1, sizeof(M.l1), "Waiting for a gift");
   snprintf(M.l2, sizeof(M.l2), "%s  game %s", M.ip,
            M.game[0] ? M.game : "?");
   chip(1);
   osd_toast("Mystery Gift listening");
   fe_evt("mg_start ip=%s game=%s port=%u", M.ip, M.game, M.port);
}

void mgift_vita_frame(void)
{
   static mgift_parcel parcel;
   mgift_cart_stats s;

   if (!M.on)
      return;
   /* Acknowledge only after the cart took the whole parcel; an identical
    * retry keeps a session going, a different gift gets "busy". */
   if (card_poll(&parcel))
   {
      int rc = mgift_cart_set_parcel(&parcel);
      card_reply(parcel.id, rc == 0 ? 0 : 1);
      fe_evt("mg_parcel id=%08x frlg=%u emerald=%u rc=%d", (unsigned)parcel.id,
             parcel.frlg_size, parcel.emerald_size, rc);
      if (rc == 0)
      {
         snprintf(M.l1, sizeof(M.l1), "Gift ready");
         snprintf(M.l2, sizeof(M.l2), "Open MYSTERY GIFT in the game");
         osd_toast("Gift received from the phone");
      }
   }
   mgift_cart_frame();
   mgift_cart_get_stats(&s);
   if (!M.linked && s.linked)
   {
      M.linked = 1;
      snprintf(M.l1, sizeof(M.l1), "Game connected");
      osd_toast("The game found the gift");
      fe_evt("mg_game_linked");
   }
   if (M.linked || mgift_cart_has_card())
      snprintf(M.l2, sizeof(M.l2), "%s", mgift_cart_status());
   else
   {
      /* Say how long it has been: a line that never changes looks like a
       * hang, and the usual cause is the phone on another network. */
      unsigned secs = fe_host_frame_count() / 60u;
      if (secs != M.secs)
      {
         M.secs = secs;
         snprintf(M.l2, sizeof(M.l2), "%s  game %s  no gift yet", M.ip,
                  M.game[0] ? M.game : "?");
      }
   }
}

int mgift_vita_active(void)
{
   return M.on;
}
