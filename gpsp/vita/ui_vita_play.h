/* ui_vita_play.h -- PLAY TOGETHER: the Vita's link menus.
 *
 * Included by psp/ui_psp.c under -DVITA where the PSP has its Wireless
 * screens, which these replace (the PSP's are a ten-second blocking scan of
 * ad hoc group names; the Vita has rooms that announce themselves, a typed
 * address, the system's ad hoc dialog, and a second machine on the same
 * console).  What stays the PSP's: the link MODE vocabulary (Link Cable /
 * Wireless Adapter, link_games.h, the per-game override), the HOST chooser
 * for a game that links both ways (screen_host), Mystery Gift, and the
 * LINKING screen.
 *
 *   SCR_WIRELESS   the page itself: Host a game / Join a game / Two players
 *                  on this Vita / Connection / [Mystery Gift] / How to play
 *                  together, under a live line about the network -- or,
 *                  while a room is open or a link is up, the session's
 *                  status in plain words and how to end it
 *   SCR_JOIN       the room list: fills and changes by itself
 *   SCR_ADDR       the host's address, on a numeric keypad, and the last
 *                  four typed
 *   SCR_ADHOC      "the Vita's own connection screen comes next"
 *   SCR_LOCAL      two players on this Vita: how the partner starts; while
 *                  it runs, who the pad drives, the layout, Stop
 *   SCR_CONN       Wi-Fi / Internet / Ad hoc, each with its one line, and
 *                  Link delay
 *   SCR_HELP       how to play together, two short pages
 *
 * NOTHING HERE WAITS.  The network is asked through vita/ui_host_hooks.h:
 * its state twice a second (play_net), the rooms every frame of the Join
 * page, the local link every frame of its pages.
 */
#ifndef UI_VITA_PLAY_H
#define UI_VITA_PLAY_H

/* ---- state -------------------------------------------------------------- */

static ui_net_status g_net;            /* the last answer                    */
static unsigned g_net_frame;           /* g_vframe when it was asked         */
static int  g_net_have;
static char g_play_note[112];          /* the plate, until the next key      */
static int  g_play_pending;            /* frames since a Host / local start
                                        * was returned and nothing answered  */
#define PLAY_PENDING_MAX 600           /* 10 s: then the page says so        */
static ui_join_info g_join_tgt;
static int  g_adhoc_host;              /* SCR_ADHOC: 1 host, 0 join          */
static int  g_help_page;

const ui_join_info *ui_join_target(void)
{
   return &g_join_tgt;
}

/* The network's state, asked at most twice a second (and at once when
 * `now` -- a page just opened, or the player pressed something that
 * depends on it). */
static const ui_net_status *play_net(int now)
{
   if (now || !g_net_have || g_vframe - g_net_frame >= 30u)
   {
      memset(&g_net, 0, sizeof(g_net));
      g_net.signal_pct = -1;
      ui_host_net_status(&g_net);
      g_net.ssid[sizeof(g_net.ssid) - 1] = '\0';
      g_net.ip[sizeof(g_net.ip) - 1] = '\0';
      g_net.host_addr[sizeof(g_net.host_addr) - 1] = '\0';
      g_net.public_addr[sizeof(g_net.public_addr) - 1] = '\0';
      g_net_frame = g_vframe;
      g_net_have = 1;
   }
   return &g_net;
}

static const char *conn_name(int c)
{
   return c == UI_CONN_INTERNET ? "Internet" : c == UI_CONN_ADHOC ? "Ad hoc"
                                                                  : "Wi-Fi";
}

static const char *kind_name(int kind)
{
   return kind == ROOM_ADAPTER ? "Wireless Adapter"
        : kind == ROOM_CABLE_GB ? "GB Link Cable" : "Link Cable";
}

/* Every failure, in the player's words (ui_psp.h). */
void ui_link_fail_text(int why, const char **l1, const char **l2)
{
   static const char *const t[UI_FAIL_COUNT][2] = {
      { "This " UI_DEV " is not connected to Wi-Fi.",
        "Connect in the " UI_DEV "'s Settings > Network, then try again." },
      { "Nobody answered.",
        "Is the other " UI_DEV " hosting, and on the same network?" },
      { "That room is full.",
        "A room takes two players. Ask the host to open a new one." },
      { "The other " UI_DEV " runs a different version of VitAdhoc.",
        "Install the same version on both, then link again." },
      { "The two games are not the same game.",
        "Both players open the same game (and region), then link." },
      { "The host closed the room.",
        "Ask them to choose Host a game again." },
      { "Link lost: the other " UI_DEV " is gone.",
        "Your game carries on. Save as usual." },
      { "The two games fell out of step.",
        "The link ended and nothing was saved. Link again to retry." },
      { "That is not an address.",
        "It looks like 192.168.1.23 or 203.0.113.9:19019." },
      { "The connection screen was closed.",
        "Nothing was changed. Choose Host or Join to try again." },
      { "Wi-Fi and ad hoc cannot be used at the same time.",
        "End the current link first." },
      { "The room could not be opened on this network.",
        "The port may be in use. Try again in a moment." },
      { "Not enough memory to link this game.",
        "Quit to the game list, open the game again and link first." },
      { "The second game would not start.",
        "This game cannot run twice on one " UI_DEV "." },
   };
   if (why < 0 || why >= UI_FAIL_COUNT)
      why = UI_FAIL_NO_ANSWER;
   if (l1) *l1 = t[why][0];
   if (l2) *l2 = t[why][1];
}

/* "Wi-Fi: HomeNet  192.168.1.23" -- the one live line every Play together
 * page starts with.  Returns 1 when the network is usable for `conn`. */
static int play_status_line(char *out, size_t cap, int conn,
                            const ui_net_status *n)
{
   if (conn == UI_CONN_ADHOC)
   {
      snprintf(out, cap, "Ad hoc: the two " UI_DEVS " connect directly, no "
               "router.");
      return 1;
   }
   if (n->wifi == UI_WIFI_CONNECTED)
   {
      char ssid[40];
      vw_clip(ssid, sizeof(ssid), n->ssid[0] ? n->ssid : "connected", 170);
      snprintf(out, cap, "%s: %s   %s", conn_name(conn), ssid, n->ip);
      return 1;
   }
   if (n->wifi == UI_WIFI_CONNECTING)
      snprintf(out, cap, "%s: connecting to Wi-Fi...", conn_name(conn));
   else if (n->radio == UI_RADIO_ADHOC)
      snprintf(out, cap, "%s: the radio is in ad hoc use.", conn_name(conn));
   else
      snprintf(out, cap, "No Wi-Fi. Connect in the " UI_DEV "'s Settings > "
               "Network.");
   return 0;
}

static void play_status_draw(int y, int conn, const ui_net_status *n)
{
   char s[120];
   int ok = play_status_line(s, sizeof(s), conn, n);
   clip_title(s, 410);
   vid_text(22, y, s, ok ? C_VALUE : C_ITEM);
   if (ok && conn != UI_CONN_ADHOC)
      vw_signal(22 + vid_text_w(s) + 10, y + 1, n->signal_pct);
}

/* What this game links with, in one line (the page's sub-title). */
static void play_game_line(char *out, size_t cap)
{
   if (g_pcfg.console != FE_CONSOLE_GBA)
      wl_line_name(out, cap, "Link Cable game. Player 2 needs ",
                   own_game_name(), ", or is sent it.", 436);
   else
      switch (ui_host_mode())
      {
      case UI_HOST_CABLE:
         wl_line_name(out, cap, "Link Cable game: both " UI_DEVS " need ",
                      own_game_name(), " open.", 436);
         break;
      case UI_HOST_ASK:
         snprintf(out, cap, "Links by Link Cable or Wireless Adapter: the "
                  "host chooses.");
         break;
      default:
         snprintf(out, cap, "Wireless Adapter game: the game itself pairs "
                  "the players.");
         break;
      }
}

/* ---- the page ----------------------------------------------------------- */

enum { PL_HOST, PL_JOIN, PL_LOCAL, PL_CONN, PL_MGIFT, PL_HELP, PL_COUNT };

static int play_rows(int ids[PL_COUNT])
{
   int n = 0;
   ids[n++] = PL_HOST;
   ids[n++] = PL_JOIN;
   ids[n++] = PL_LOCAL;
   ids[n++] = PL_CONN;
   if (g_pcfg.console == FE_CONSOLE_GBA && ui_host_mgift_available())
      ids[n++] = PL_MGIFT;
   ids[n++] = PL_HELP;
   return n;
}

static struct { char text[24]; int primed; } g_ad;   /* SCR_ADDR */
static ui_action screen_join(unsigned edges);
static ui_action screen_addr(unsigned edges);
static ui_action screen_local(unsigned edges);
static void join_open(const char *addr);

/* Host, decided: Link Cable or Wireless Adapter.  Ad hoc explains the
 * system's screen first; everything else starts the room now.  The menu
 * stays open on the waiting page (ui_host_hooks.h section 7). */
static ui_action play_host_go(int cable)
{
   g_host_cable = cable;
   snprintf(g_join_group, sizeof(g_join_group), "%s", g_pcfg.group);
   if (g_pcfg.net_conn == UI_CONN_ADHOC && g_screen != SCR_ADHOC)
   {
      screen_to(SCR_ADHOC);
      g_adhoc_host = 1;
      return UI_ACT_NONE;
   }
   screen_to(SCR_WIRELESS);
   g_play_pending = 1;
   g_play_note[0] = '\0';
   fe_evt("play_host conn=%d cable=%d group=%s", g_pcfg.net_conn, cable,
          g_join_group);
   return UI_ACT_NET_HOST;
}

/* The room and the mode for the Host row's value. */
static void play_host_value(char *out, size_t cap)
{
   char lbl[ROOM_LABEL_CAP];
   int m = g_pcfg.console == FE_CONSOLE_GBA ? ui_host_mode() : UI_HOST_CABLE;
   snprintf(out, cap, "%s  -  %s", room_label(g_pcfg.group, lbl, sizeof(lbl)),
            m == UI_HOST_ASK ? "Cable or Adapter" : m == UI_HOST_ADAPTER
               ? "Wireless Adapter" : "Link Cable");
}

/* A room is open or a link is up: the status, and how to end it. */
static ui_action play_session(unsigned edges, const char *session_info)
{
   enum { PS_RESUME, PS_END, PS_COUNT };
   const ui_net_status *n = play_net(0);
   int hosting = g_link.state == UI_LINK_HOSTING;
   int pending = g_link.state == UI_LINK_OFF && g_play_pending;
   int conn = g_link.state != UI_LINK_OFF ? g_link.conn : g_pcfg.net_conn;
   char l1[120], l2[120], la[120], lb[120], lc[120], para[260], big[32];
   const char *mode = g_link.cable ? "Link Cable" : "Wireless Adapter";
   int y;

   if (g_cursor >= PS_COUNT)
      g_cursor = 0;
   tch_rows(&g_cursor, &edges);
   if (edges & PSP_CTRL_UP)
      g_cursor = (g_cursor + PS_COUNT - 1) % PS_COUNT;
   if (edges & PSP_CTRL_DOWN)
      g_cursor = (g_cursor + 1) % PS_COUNT;
   if (edges & PSP_CTRL_CIRCLE)
   {
      screen_to(SCR_MENU);
      return UI_ACT_NONE;
   }
   if (edges & PSP_CTRL_CROSS)
   {
      if (g_cursor == PS_RESUME)
         return UI_ACT_RESUME;
      g_play_pending = 0;
      screen_to(SCR_MENU);
      return UI_ACT_NET_DISCONNECT;
   }

   l1[0] = l2[0] = para[0] = big[0] = la[0] = lb[0] = lc[0] = '\0';
   if (pending)
   {
      snprintf(l1, sizeof(l1), "Opening %s...", g_link_room[0] ? g_link_room
               : room_label(g_pcfg.group, big, sizeof(big)));
      big[0] = '\0';
      snprintf(l2, sizeof(l2), conn == UI_CONN_ADHOC
               ? "The " UI_DEV "'s connection screen comes up next."
               : "One moment.");
   }
   else if (hosting)
   {
      snprintf(l1, sizeof(l1), "Hosting %s by %s", g_link.room, mode);
      snprintf(l2, sizeof(l2), "Waiting for player 2. You can keep playing "
               "meanwhile.");
      if (conn == UI_CONN_INTERNET)
      {
         if (n->public_state == UI_PUBLIC_ASKING)
            snprintf(la, sizeof(la), "Asking your router to let the other "
                     "player in...");
         else if (n->public_state == UI_PUBLIC_CLOSED)
         {
            char port[8] = "19019";
            const char *c = strrchr(n->host_addr, ':');
            if (c && c[1])
               snprintf(port, sizeof(port), "%.5s", c + 1);
            snprintf(para, sizeof(para), "Your router did not open the way "
                     "in by itself. In its settings, forward UDP port %s to "
                     "this " UI_DEV " (%s). The other player then types "
                     "your public address%s%s.", port,
                     n->ip[0] ? n->ip : "its address",
                     n->public_addr[0] ? ": " : "", n->public_addr);
         }
         else if (n->public_state == UI_PUBLIC_OPEN && !n->public_addr[0])
         {
            /* The router opened the port and would not say its own
             * address: only the port is known. */
            const char *c = strrchr(n->host_addr, ':');
            snprintf(para, sizeof(para), "Your router opened the way in, "
                     "but did not say its public address. Look it up (a "
                     "phone on this Wi-Fi: search \"what is my IP\"). The "
                     "other player types that address, then %s.",
                     c && c[1] ? c : ":19019");
         }
         else
         {
            snprintf(la, sizeof(la), "Tell the other player this address:");
            snprintf(big, sizeof(big), "%s", n->public_state == UI_PUBLIC_OPEN
                     && n->public_addr[0] ? n->public_addr : n->host_addr);
            snprintf(lc, sizeof(lc), "They choose Play together > Join a "
                     "game and type it.");
         }
      }
      else
      {
         wl_line_name(la, sizeof(la), "On the other " UI_DEV ", open ",
                      own_game_name(), ".", 436);
         if (conn == UI_CONN_ADHOC)
            snprintf(lb, sizeof(lb), "Connection: Ad hoc, then Play together "
                     "> Join a game.");
         else
         {
            char who[24];
            snprintf(lb, sizeof(lb), "Then Play together > Join a game: "
                     "pick %s's room.", vw_clip(who, sizeof(who),
                     g_pcfg.nick[0] ? g_pcfg.nick : "this " UI_DEV, 96));
            if (n->host_addr[0])
               snprintf(lc, sizeof(lc), "Not listed? Connection: Internet, "
                        "type %s", n->host_addr);
         }
      }
   }
   else if (g_link.state == UI_LINK_LINKED)
   {
      if (g_link_peer[0])
         snprintf(l1, sizeof(l1), "Linked with %s", g_link_peer);
      else
         snprintf(l1, sizeof(l1), "Linked with the other " UI_DEV);
      if (g_link.cable && g_link.player)
         snprintf(l2, sizeof(l2), "You are Player %d%s in %s, by Link Cable.",
                  g_link.player, g_link.player == 1 ? " (the host)" : "",
                  g_link.room);
      else if (g_link.cable)
         snprintf(l2, sizeof(l2), "%s, by Link Cable.", g_link.room);
      else
         snprintf(l2, sizeof(l2), "%s, by Wireless Adapter. The game pairs "
                  "the players.", g_link.room);
      snprintf(para, sizeof(para), "Save states and cheats are off while "
               "linked. Disconnect ends the link for both players: each "
               "game saves and carries on alone.");
   }
   else
      snprintf(l1, sizeof(l1), "%s", session_info && session_info[0]
               ? session_info : "A link session is running.");

   ov_backdrop(OV_SCRIM_DENSE);
   vw_head(g_ov_title, "PLAY TOGETHER");
   vw_line(22, 68, 420, l1, C_VALUE);
   if (pending)
      vw_ring(22 + vid_text_w(l1) + 16, 68 + 8, C_TITLE);
   if (l2[0])
      vw_line(22, 86, 436, l2, C_ITEM);
   y = 110;
   if (g_link.state == UI_LINK_LINKED)
   {
      /* The connection, measured: what it is on, how quick, how strong. */
      char q[96];
      int x;
      if (g_link.rtt_ms >= 0)
         snprintf(q, sizeof(q), "%s   %s (%d ms round trip)",
                  conn_name(conn), g_link.rtt_ms < 40 ? "quick"
                  : g_link.rtt_ms < 120 ? "fine" : "slow, expect delay",
                  g_link.rtt_ms);
      else
         snprintf(q, sizeof(q), "%s", conn_name(conn));
      vid_text(22, y, q, C_ITEM);
      x = 22 + vid_text_w(q) + 12;
      if (g_link.signal_pct >= 0)
      {
         vid_text(x, y, "signal", C_DIM);
         vw_signal(x + vid_text_w("signal") + 6, y + 1, g_link.signal_pct);
      }
      y += FE_FONT_H + 6;
   }
   if (la[0])
   {
      vw_line(22, y, 436, la, C_ITEM);
      y += FE_FONT_H + 2;
   }
   if (lb[0])
   {
      vw_line(22, y, 436, lb, C_ITEM);
      y += FE_FONT_H + 2;
   }
   if (para[0])
      y = vw_wrap(22, y, 436, para, C_DIM, 4);
   if (big[0])
   {
      /* The one thing to read out, in the title face. */
      vid_text_hd(22, y + 2, big, C_SEL);
      y += 28;
   }
   if (lc[0])
      vw_line(22, y + 4, 436, lc, C_DIM);
   vw_row(PS_RESUME, 200, 300, g_cursor == PS_RESUME, 1,
          g_link.state == UI_LINK_LINKED ? "Back to the game"
          : pending ? "Back to the game" : "Keep playing", NULL);
   vw_row(PS_END, 200 + VW_PITCH, 300, g_cursor == PS_END, 1,
          hosting ? "Stop hosting" : pending ? "Cancel" : "Disconnect", NULL);
   /* The other way of linking is not on offer while this one is up
    * (main_vita.c decides that): said beside the row that ends it. */
   if (g_cursor == PS_END)
   {
      ui_local_state ls;
      memset(&ls, 0, sizeof(ls));
      ui_host_local_state(&ls);
      if (!ls.active)
         vw_wrap(334, 198, 126, "Two players on this " UI_DEV
                 ": after this has ended.", C_DIM, 3);
   }
   footer(VID_GLYPH_X " select   " VID_GLYPH_O " back");
   return UI_ACT_NONE;
}

static ui_action screen_play(unsigned edges, int session_active,
                             const char *session_info)
{
   int ids[PL_COUNT], n = play_rows(ids), i, id, y0;
   const ui_net_status *net;
   ui_local_state ls;
   char sub[120], hv[48];

   /* A Host that was returned and has not been answered yet. */
   if (g_play_pending)
   {
      /* An error still standing from the attempt BEFORE this one is not
       * this one's answer: the link layer has two frames to clear it. */
      if (g_link.state != UI_LINK_OFF)
         g_play_pending = 0;
      else if (g_link_error[0] && g_play_pending > 2)
      {
         snprintf(g_play_note, sizeof(g_play_note), "%s", g_link_error);
         g_play_pending = 0;
      }
      /* Ad hoc: the system's connection screen is up for as long as the
       * player takes with it (a host waits in it for the other console),
       * and the link layer has its own limit on that.  The radio reads
       * "ad hoc" from the moment it opens: no count while it does. */
      else if (play_net(0)->radio != UI_RADIO_ADHOC &&
               ++g_play_pending > PLAY_PENDING_MAX)
      {
         snprintf(g_play_note, sizeof(g_play_note), "The room did not "
                  "open. Check the connection and try again.");
         g_play_pending = 0;
      }
   }
   memset(&ls, 0, sizeof(ls));
   ui_host_local_state(&ls);
   if (ls.active)
      return screen_local(edges);
   if (session_active || g_link.state != UI_LINK_OFF || g_play_pending)
      return play_session(edges, session_info);

   if (g_cursor >= n)
      g_cursor = n - 1;          /* back from Help: its row is the last */
   if (edges)
      g_play_note[0] = '\0';
   tch_rows(&g_cursor, &edges);
   if (edges & PSP_CTRL_UP)
      g_cursor = (g_cursor + n - 1) % n;
   if (edges & PSP_CTRL_DOWN)
      g_cursor = (g_cursor + 1) % n;
   id = ids[g_cursor];
   if (edges & (PSP_CTRL_LEFT | PSP_CTRL_RIGHT))
   {
      int dir = (edges & PSP_CTRL_LEFT) ? -1 : 1;
      if (id == PL_HOST)
         settings_adjust(SET_ROOM, dir);
      else if (id == PL_CONN)
         settings_adjust(SET_CONN, dir);
   }
   if (edges & PSP_CTRL_CIRCLE)
   {
      screen_to(SCR_MENU);
      return UI_ACT_NONE;
   }
   if (edges & PSP_CTRL_CROSS)
   {
      net = play_net(1);
      switch (id)
      {
      case PL_HOST:
      case PL_JOIN:
         if (g_pcfg.net_conn != UI_CONN_ADHOC &&
             net->wifi != UI_WIFI_CONNECTED)
         {
            const char *a, *b;
            ui_link_fail_text(net->radio == UI_RADIO_ADHOC
                              ? UI_FAIL_RADIO_BUSY : UI_FAIL_NO_WIFI, &a, &b);
            snprintf(g_play_note, sizeof(g_play_note), "%s", a);
            break;
         }
         if (id == PL_HOST)
         {
            if (g_pcfg.console == FE_CONSOLE_GBA &&
                ui_host_mode() == UI_HOST_ASK)
            {
               screen_to(SCR_HOST);
               return UI_ACT_NONE;
            }
            return play_host_go(g_pcfg.console != FE_CONSOLE_GBA ||
                                ui_host_mode() == UI_HOST_CABLE);
         }
         if (g_pcfg.net_conn == UI_CONN_INTERNET)
         {
            screen_to(SCR_ADDR);
            g_ad.primed = 0;
            return screen_addr(0);
         }
         if (g_pcfg.net_conn == UI_CONN_ADHOC)
         {
            screen_to(SCR_ADHOC);
            g_adhoc_host = 0;
            return UI_ACT_NONE;
         }
         join_open(NULL);
         return screen_join(0);
      case PL_LOCAL:
         if (!ls.available)
         {
            snprintf(g_play_note, sizeof(g_play_note), "%s",
                     ls.why_not && ls.why_not[0] ? ls.why_not
                     : "This game cannot run twice on one " UI_DEV ".");
            break;
         }
         screen_to(SCR_LOCAL);
         return screen_local(0);
      case PL_CONN:
         screen_to(SCR_CONN);
         g_cursor = g_pcfg.net_conn;
         return UI_ACT_NONE;
      case PL_MGIFT:
         screen_to(SCR_MGIFT);
         return UI_ACT_NONE;
      default:
         screen_to(SCR_HELP);
         g_help_page = 0;
         return UI_ACT_NONE;
      }
   }

   net = play_net(0);
   play_game_line(sub, sizeof(sub));
   ov_backdrop(OV_SCRIM);
   vw_head(sub, "PLAY TOGETHER");
   play_status_draw(68, g_pcfg.net_conn, net);
   y0 = n > 5 ? 92 : 96;
   for (i = 0; i < n; i++)
   {
      int y = y0 + i * VW_PITCH, sel = g_cursor == i;
      switch (ids[i])
      {
      case PL_HOST:
         play_host_value(hv, sizeof(hv));
         vw_row(i, y, 440, sel, 1, "Host a game", hv);
         break;
      case PL_JOIN:
         vw_row(i, y, 440, sel, 1, "Join a game",
                g_pcfg.net_conn == UI_CONN_INTERNET ? "type the host's address"
                : g_pcfg.net_conn == UI_CONN_ADHOC ? "a " UI_DEV " nearby"
                : "rooms on this Wi-Fi");
         break;
      case PL_LOCAL:
         vw_row(i, y, 440, sel, ls.available, "Two players on this " UI_DEV,
                ls.available ? NULL : "not for this game");
         break;
      case PL_CONN:
         vw_row(i, y, 440, sel, 1, "Connection", conn_name(g_pcfg.net_conn));
         break;
      case PL_MGIFT:
         vw_row(i, y, 440, sel, 1, "Mystery Gift",
                mgift_ui_active() ? "listening" : "from a phone");
         break;
      default:
         vw_row(i, y, 440, sel, 1, "How to play together", NULL);
         break;
      }
   }
   if (g_play_note[0])
      vw_plate(g_play_note);
   else if (id == PL_HOST)
      footer(VID_GLYPH_X " host   DPAD room number   " VID_GLYPH_O " back");
   else if (id == PL_CONN)
      footer(VID_GLYPH_X " choose   DPAD change   " VID_GLYPH_O " back");
   else
      footer(VID_GLYPH_X " select   " VID_GLYPH_O " back");
   return UI_ACT_NONE;
}

/* ---- Join a game: the room list ------------------------------------------ */

#define JOIN_ROW_H    34
#define JOIN_VIEW_TOP 68
#define JOIN_VIEW_BOT (JOIN_VIEW_TOP + 5 * JOIN_ROW_H)   /* five whole rows */
#define JOIN_QUIET    90         /* frames of nothing before "none yet"      */

static struct
{
   ui_room room[UI_ROOMS_MAX];
   int  n;
   int  open;                    /* a scan is open                           */
   int  err;                     /* why not (UI_ROOMS_E_*), 0 = none         */
   int  frames;                  /* since the page opened                    */
   int  retry;                   /* frames until begin is tried again        */
   unsigned sel_id;              /* the room under the cursor                */
   int  have_sel;
   int  scroll;                  /* px                                       */
   tch_scroll_t ts;
   int  free_scroll;             /* the finger moved the list: do not snap
                                  * it back to the cursor                    */
   char addr[24];                /* Internet: the address being looked at    */
   char note[112];
} g_jn;

/* The game a room is playing, as a name: this game's own, the link
 * table's for a GBA code it knows, else what the host announced. */
static const char *join_title(const ui_room *r)
{
   char code[5];
   const char *t;
   room_code_gba(g_game_code, code);
   if (r->kind == ROOM_CABLE_GBA && g_pcfg.console == FE_CONSOLE_GBA &&
       !strncmp(r->code, code, 4))
      return own_game_name();
   t = r->kind == ROOM_CABLE_GB ? NULL : link_game_title(r->code);
   return t ? t : r->title[0] ? r->title : "a game";
}

/* Whether this game can enter a room, and the sentence when it cannot. */
typedef struct { int ok; char why[40]; char plate[112]; } join_verdict;

static void join_decide(const ui_room *r, join_verdict *v)
{
   char rl[24], code[5], t[40];
   int gba = g_pcfg.console == FE_CONSOLE_GBA;
   memset(v, 0, sizeof(*v));
   v->ok = 1;
   snprintf(rl, sizeof(rl), ROOM_LABEL_WORD " %d", r->number);
   vw_clip(t, sizeof(t), join_title(r), 150);
   room_code_gba(g_game_code, code);
#define JOIN_NO(short_, ...) \
   do { v->ok = 0; snprintf(v->why, sizeof(v->why), "%s", short_); \
        snprintf(v->plate, sizeof(v->plate), __VA_ARGS__); } while (0)
   if (!r->compatible)
      JOIN_NO("different version", "%s runs a different version of "
              "VitAdhoc.", r->nick[0] ? r->nick : rl);
   else if (r->kind == ROOM_ADAPTER && !gba)
      JOIN_NO("a Game Boy Advance room", "%s is a Game Boy Advance room.", rl);
   else if (r->kind == ROOM_ADAPTER && !join_can_adapter())
      JOIN_NO("this game links by cable", "%s is a Wireless Adapter room. "
              "This game links by cable.", rl);
   else if (r->kind == ROOM_CABLE_GBA && !gba)
      JOIN_NO("a Game Boy Advance room", "%s is a Game Boy Advance room.", rl);
   else if (r->kind == ROOM_CABLE_GBA && strncmp(r->code, code, 4) != 0)
      JOIN_NO("another game: open it to join", "%s is playing %s. Open that "
              "game to join.", rl, t);
   else if (r->kind == ROOM_CABLE_GBA && !join_can_cable())
      JOIN_NO(g_cable_ready ? "this game links by adapter"
                            : "restart the game to link",
              g_cable_ready ? "%s is a Link Cable room. This game links by "
                              "adapter."
                            : "%s is a Link Cable room. Restart the game to "
                              "link.", rl);
   else if (r->kind == ROOM_CABLE_GB && gba)
      JOIN_NO("a Game Boy room", "%s is a Game Boy room.", rl);
   else if (r->max_players > 0 && r->players >= r->max_players)
      JOIN_NO("full", "%s is full: a room takes %d players.", rl,
              r->max_players);
   else if (!r->open)
      JOIN_NO("already playing", "%s is in the middle of a game.", rl);
#undef JOIN_NO
}

/* Joinable rooms first, then by room number, then by id: an order that
 * does not shuffle from frame to frame. */
static int join_less(const ui_room *a, const ui_room *b)
{
   join_verdict va, vb;
   join_decide(a, &va);
   join_decide(b, &vb);
   if (va.ok != vb.ok)
      return va.ok > vb.ok;
   if (a->number != b->number)
      return a->number < b->number;
   return a->id < b->id;
}

static void join_begin_scan(void)
{
   int rc = ui_host_rooms_begin(g_jn.addr[0] ? g_jn.addr : NULL);
   g_jn.open = rc == 0;
   g_jn.err = rc < 0 ? rc : 0;
   g_jn.retry = 60;
   fe_evt("join_scan_begin addr=%s rc=%d", g_jn.addr[0] ? g_jn.addr : "-",
          rc);
}

static void join_close(void)
{
   if (g_jn.open)
      ui_host_rooms_end();
   g_jn.open = 0;
}

/* Open the page: NULL = the rooms on this network, else one address. */
static void join_open(const char *addr)
{
   screen_to(SCR_JOIN);
   memset(&g_jn, 0, sizeof(g_jn));
   snprintf(g_jn.addr, sizeof(g_jn.addr), "%s", addr ? addr : "");
   join_begin_scan();
}

static ui_action join_pick(const ui_room *r)
{
   char code[5];
   memset(&g_join_tgt, 0, sizeof(g_join_tgt));
   g_join_tgt.conn = g_pcfg.net_conn;
   g_join_tgt.listed = 1;
   g_join_tgt.id = r->id;
   g_join_tgt.kind = r->kind;
   g_join_tgt.number = r->number;
   snprintf(g_join_tgt.addr, sizeof(g_join_tgt.addr), "%s", r->addr);
   snprintf(g_join_tgt.code, sizeof(g_join_tgt.code), "%.4s", r->code);
   snprintf(g_join_tgt.title, sizeof(g_join_tgt.title), "%s", r->title);
   snprintf(g_join_tgt.nick, sizeof(g_join_tgt.nick), "%s", r->nick);
   /* ui_group(), for code that still thinks in the PSP's group names. */
   snprintf(code, sizeof(code), "%.4s", r->code);
   room_make(r->kind, r->number, strlen(code) == 4 ? code : "GAME",
             g_join_group, sizeof(g_join_group));
   g_host_cable = r->kind != ROOM_ADAPTER;
   join_close();
   fe_evt("play_join id=%u addr=%s kind=%d room=%d", r->id, r->addr, r->kind,
          r->number);
   screen_to(SCR_MENU);
   return UI_ACT_NET_JOIN;
}

static ui_action screen_join(unsigned edges)
{
   const ui_net_status *net = play_net(0);
   int i, n, cur = 0, rows, view = JOIN_VIEW_BOT - JOIN_VIEW_TOP, max;
   int internet = g_jn.addr[0] != 0;

   g_jn.frames++;
   /* The scan: open it when it can be, ask it every frame. */
   if (!g_jn.open)
   {
      g_jn.n = 0;
      if (--g_jn.retry <= 0)
         join_begin_scan();
   }
   if (g_jn.open)
   {
      ui_room tmp[UI_ROOMS_MAX];
      int j, k;
      memset(tmp, 0, sizeof(tmp));
      n = ui_host_rooms_poll(tmp, UI_ROOMS_MAX);
      if (n < 0)
      {
         g_jn.err = n;
         join_close();
         g_jn.retry = 60;
         n = 0;
      }
      if (n > UI_ROOMS_MAX)
         n = UI_ROOMS_MAX;
      for (j = 0; j < n; j++)          /* nothing a hook says overruns */
      {
         tmp[j].title[sizeof(tmp[j].title) - 1] = '\0';
         tmp[j].nick[sizeof(tmp[j].nick) - 1] = '\0';
         tmp[j].code[4] = '\0';
         tmp[j].addr[sizeof(tmp[j].addr) - 1] = '\0';
      }
      for (j = 1; j < n; j++)          /* insertion sort: 16 at most */
      {
         ui_room x = tmp[j];
         for (k = j; k > 0 && join_less(&x, &tmp[k - 1]); k--)
            tmp[k] = tmp[k - 1];
         tmp[k] = x;
      }
      memcpy(g_jn.room, tmp, sizeof(tmp));
      g_jn.n = n;
   }
   n = g_jn.n;
   rows = n;
   /* The cursor stays on its ROOM while the list changes around it. */
   if (g_jn.have_sel)
      for (i = 0; i < n; i++)
         if (g_jn.room[i].id == g_jn.sel_id)
            cur = i;
   if (cur >= rows)
      cur = rows ? rows - 1 : 0;

   if (edges)
      g_jn.note[0] = '\0';
   {
      /* A finger names a ROOM, not a row: the list may have re-sorted
       * between its landing and its lifting, and the room it meant may be
       * gone (then nothing happens). */
      int was = rows ? (int)(g_jn.room[cur].id & 0x7fffffffu) : -1;
      int tid = was, tapped = tch_rows(&tid, &edges), found = tid == was;
      for (i = 0; i < rows && tid != was; i++)
         if ((int)(g_jn.room[i].id & 0x7fffffffu) == tid)
         {
            cur = i;
            found = 1;
            break;
         }
      if (tapped && !found)
         edges &= ~(unsigned)PSP_CTRL_CROSS;
      if (tapped || g_tch.hover_row != TCH_NONE)
         g_jn.free_scroll = 1;
   }
   if (rows > 0 && (edges & (PSP_CTRL_UP | PSP_CTRL_DOWN)))
   {
      cur = (edges & PSP_CTRL_UP) ? (cur + rows - 1) % rows
                                  : (cur + 1) % rows;
      g_jn.free_scroll = 0;
      tch_scroll_stop(&g_jn.ts);
   }
   if (rows > 0)
   {
      g_jn.sel_id = g_jn.room[cur].id;
      g_jn.have_sel = 1;
   }
   if (edges & PSP_CTRL_CIRCLE)
   {
      join_close();
      if (internet)
      {
         screen_to(SCR_ADDR);
         return screen_addr(0);
      }
      screen_to(SCR_WIRELESS);
      g_cursor = PL_JOIN;
      return UI_ACT_NONE;
   }
   if ((edges & PSP_CTRL_CROSS) && rows > 0)
   {
      join_verdict v;
      join_decide(&g_jn.room[cur], &v);
      if (v.ok)
         return join_pick(&g_jn.room[cur]);
      snprintf(g_jn.note, sizeof(g_jn.note), "%s", v.plate);
      fe_evt("join_refused id=%u why=\"%s\"", g_jn.room[cur].id, v.plate);
   }

   /* Scroll: the finger's, or follow the cursor. */
   max = rows * JOIN_ROW_H - view;
   if (max < 0)
      max = 0;
   if (tch_scroll(&g_jn.ts, &g_jn.scroll, max, 0, JOIN_VIEW_TOP, VID_SCR_W,
                  view))
      g_jn.free_scroll = 1;
   else if (!g_jn.free_scroll)
   {
      if (cur * JOIN_ROW_H < g_jn.scroll)
         g_jn.scroll = cur * JOIN_ROW_H;
      if ((cur + 1) * JOIN_ROW_H > g_jn.scroll + view)
         g_jn.scroll = (cur + 1) * JOIN_ROW_H - view;
   }
   if (g_jn.scroll > max) g_jn.scroll = max;
   if (g_jn.scroll < 0) g_jn.scroll = 0;
   if (g_jn.free_scroll && rows > 0)
   {
      /* The cursor stays among the rows the finger left in view. */
      int first = (g_jn.scroll + JOIN_ROW_H - 1) / JOIN_ROW_H;
      int last = (g_jn.scroll + view) / JOIN_ROW_H - 1;
      if (last >= rows) last = rows - 1;
      if (cur < first) cur = first;
      if (cur > last) cur = last;
      g_jn.sel_id = g_jn.room[cur].id;
   }

   ov_backdrop(OV_SCRIM_DENSE);
   vw_head(internet ? g_jn.addr : NULL, "JOIN A GAME");
   {
      /* Top right: what the page is doing.  It never stops looking. */
      char st[32];
      if (n > 0)
         snprintf(st, sizeof(st), "%d FOUND", n);
      else
         snprintf(st, sizeof(st), "%s", g_jn.open ? "SEARCHING" : "WAITING");
      ov_text_right(460, 48, st, C_DIM);
      if (g_jn.open)
         vw_ring(460 - vid_text_w(st) - 14, 48 + 8, C_TITLE);
   }
   if (rows == 0)
   {
      char l1[120], l2[160], l3[120];
      l1[0] = l2[0] = l3[0] = '\0';
      if (!g_jn.open && (g_jn.err == UI_ROOMS_E_NO_WIFI ||
                         net->wifi != UI_WIFI_CONNECTED))
      {
         snprintf(l1, sizeof(l1), "This " UI_DEV " is not connected to "
                  "Wi-Fi.");
         snprintf(l2, sizeof(l2), "Connect in the " UI_DEV "'s Settings > "
                  "Network. This page carries on by itself as soon as it "
                  "is.");
      }
      else if (!g_jn.open && g_jn.err == UI_ROOMS_E_BUSY)
      {
         snprintf(l1, sizeof(l1), "The radio is busy with another link.");
         snprintf(l2, sizeof(l2), "End it first (Play together > "
                  "Disconnect).");
      }
      else if (!g_jn.open)
      {
         snprintf(l1, sizeof(l1), "The search could not start.");
         snprintf(l2, sizeof(l2), "Trying again by itself. If this stays, "
                  "check the " UI_DEV "'s Wi-Fi.");
      }
      else if (g_jn.frames < JOIN_QUIET)
         snprintf(l1, sizeof(l1), internet ? "Looking for a game at that "
                  "address..." : "Looking for games on this Wi-Fi...");
      else if (internet)
      {
         snprintf(l1, sizeof(l1), "No answer from %s yet.", g_jn.addr);
         snprintf(l2, sizeof(l2), "Is the address right, and is the other "
                  "player hosting with Connection set to Internet? Their "
                  "router must let UDP port 19019 through.");
         snprintf(l3, sizeof(l3), "Still trying.");
      }
      else
      {
         char ssid[40];
         snprintf(l1, sizeof(l1), "No games found yet.");
         snprintf(l2, sizeof(l2), "Both consoles on the same Wi-Fi? Is the "
                  "other one hosting?");
         snprintf(l3, sizeof(l3), "This " UI_DEV " is on %s (%s). Still "
                  "looking.", vw_clip(ssid, sizeof(ssid), net->ssid[0]
                  ? net->ssid : "Wi-Fi", 150), net->ip);
      }
      vid_text(22, 76, l1, C_ITEM);
      i = vw_wrap(22, 76 + FE_FONT_H + 4, 436, l2, C_DIM, 3);
      if (l3[0])
         vid_text(22, i + 4, l3, C_DIM);
   }
   else
   {
      vid_clip(0, JOIN_VIEW_TOP - 2, VID_SCR_W, view + 2);
      for (i = 0; i < rows; i++)
      {
         const ui_room *r = &g_jn.room[i];
         join_verdict v;
         char t[100], d[96], pl[12];
         int y = JOIN_VIEW_TOP + 4 + i * JOIN_ROW_H - g_jn.scroll;
         int sel = i == cur;
         if (y + JOIN_ROW_H <= JOIN_VIEW_TOP || y >= JOIN_VIEW_BOT)
            continue;
         join_decide(r, &v);
         if (sel)
         {
            vid_rect(0, y - 3, 460, JOIN_ROW_H - 2, C_ACCENT, 40);
            vid_rect(0, y - 3, 4, JOIN_ROW_H - 2, C_ACCENT, 255);
         }
         vw_clip(t, 64, join_title(r), 236);
         vid_text(18, y, t, v.ok ? (sel ? C_SEL : C_ITEM) : C_DIM);
         vid_text(270, y, kind_name(r->kind), v.ok ? C_ACCENT_DK : C_DIM);
         snprintf(pl, sizeof(pl), "%d/%d", r->players,
                  r->max_players > 0 ? r->max_players : 2);
         ov_text_right(452, y, pl, v.ok ? C_VALUE : C_DIM);
         /* Who, where, how quick -- or why not. */
         if (v.ok && r->rtt_ms >= 0)
            snprintf(d, sizeof(d), "%s  -  " ROOM_LABEL_WORD " %d  -  %d ms",
                     r->nick[0] ? r->nick : "a player", r->number, r->rtt_ms);
         else if (v.ok)
            snprintf(d, sizeof(d), "%s  -  " ROOM_LABEL_WORD " %d",
                     r->nick[0] ? r->nick : "a player", r->number);
         else
            snprintf(d, sizeof(d), "%s  -  %s", r->nick[0] ? r->nick
                     : "a player", v.why);
         /* The reason reads a step brighter than the greyed row: it is the
          * answer to "why can't I join my friend's room". */
         vid_text(18, y + 14, vw_clip(t, sizeof(t), d, 430),
                  v.ok ? (sel ? C_VALUE : C_ACCENT_DK)
                       : (sel ? C_ITEM : C_ACCENT_DK));
         {
            /* Only where the row shows: the viewport's edges cut it. */
            int ty = y - 4, th = JOIN_ROW_H;
            if (ty < JOIN_VIEW_TOP) { th -= JOIN_VIEW_TOP - ty; ty = JOIN_VIEW_TOP; }
            if (ty + th > JOIN_VIEW_BOT) th = JOIN_VIEW_BOT - ty;
            tch_row((int)(r->id & 0x7fffffffu), 0, ty, 460, th);
         }
      }
      vid_clip_off();
      ov_scrollbar(470, JOIN_VIEW_TOP, view, g_jn.scroll, view,
                   rows * JOIN_ROW_H);
   }
   if (g_jn.note[0])
      vw_plate(g_jn.note);
   else if (rows > 0)
      footer(VID_GLYPH_X " join   " VID_GLYPH_O " back");
   else
      footer(VID_GLYPH_O " back");
   return UI_ACT_NONE;
}

/* ---- the host's address (Connection: Internet) ---------------------------- */

static const kp_key ADDR_KEYS[] = {
   { "1", '1', KP_CHAR, 0, 0, 1 }, { "2", '2', KP_CHAR, 1, 0, 1 },
   { "3", '3', KP_CHAR, 2, 0, 1 }, { "Del", 0, KP_DEL, 3, 0, 1 },
   { "4", '4', KP_CHAR, 0, 1, 1 }, { "5", '5', KP_CHAR, 1, 1, 1 },
   { "6", '6', KP_CHAR, 2, 1, 1 }, { ".", '.', KP_CHAR, 3, 1, 1 },
   { "7", '7', KP_CHAR, 0, 2, 1 }, { "8", '8', KP_CHAR, 1, 2, 1 },
   { "9", '9', KP_CHAR, 2, 2, 1 }, { ":", ':', KP_CHAR, 3, 2, 1 },
   { "0", '0', KP_CHAR, 0, 3, 2 }, { "Join", 0, KP_OK, 2, 3, 2 },
};
#define ADDR_NKEYS ((int)(sizeof(ADDR_KEYS) / sizeof(ADDR_KEYS[0])))
#define ADDR_KEY_OK (ADDR_NKEYS - 1)
#define ADDR_RECENT_BASE 100     /* cursor values of the remembered ones    */

/* 0 = "a.b.c.d" or "a.b.c.d:port" with every number in range. */
static int addr_valid(const char *s)
{
   int part = 0, digits = 0, v = 0;
   for (;; s++)
   {
      if (*s >= '0' && *s <= '9')
      {
         v = v * 10 + (*s - '0');
         if (++digits > 5 || v > 65535)
            return -1;
         continue;
      }
      if (!digits)
         return -1;
      if (part < 4 && v > 255)
         return -1;
      if (*s == '.' && part < 3)
         part++;
      else if (*s == ':' && part == 3)
         part = 4;
      else if (*s == '\0')
         return (part == 3 || (part == 4 && v >= 1)) ? 0 : -1;
      else
         return -1;
      digits = 0;
      v = 0;
   }
}

static int addr_recent_n(void)
{
   int n = 0;
   while (n < PCFG_NET_ADDRS && g_pcfg.net_addr[n][0])
      n++;
   return n;
}

static ui_action addr_go(const char *a)
{
   char addr[24];
   snprintf(addr, sizeof(addr), "%s", a);
   pcfg_net_addr_push(addr);
   g_settings_dirty = 1;
   join_open(addr);
   return screen_join(0);
}

static ui_action screen_addr(unsigned edges)
{
   static const kp_pad pad = { ADDR_KEYS, ADDR_NKEYS, 20, 102 };
   int nrec = addr_recent_n(), len, ok, i, act = -1;

   if (!g_ad.primed)
   {
      /* The first visit starts on the last address used, ready to join or
       * to be edited: most evenings it is the same friend. */
      snprintf(g_ad.text, sizeof(g_ad.text), "%s", g_pcfg.net_addr[0]);
      g_ad.primed = 1;
      g_cursor = g_ad.text[0] ? ADDR_KEY_OK : 0;
   }
   if (!(g_cursor >= 0 && g_cursor < ADDR_NKEYS) &&
       !(g_cursor >= ADDR_RECENT_BASE && g_cursor < ADDR_RECENT_BASE + nrec))
      g_cursor = 0;

   {
      int row = g_cursor < ADDR_NKEYS ? KP_ROW_BASE + g_cursor : g_cursor;
      if (tch_rows(&row, &edges) || g_tch.hover_row != TCH_NONE)
         g_cursor = row >= KP_ROW_BASE ? row - KP_ROW_BASE : row;
   }
   if (edges & (PSP_CTRL_UP | PSP_CTRL_DOWN | PSP_CTRL_LEFT | PSP_CTRL_RIGHT))
   {
      unsigned e = edges & (PSP_CTRL_UP | PSP_CTRL_DOWN | PSP_CTRL_LEFT |
                            PSP_CTRL_RIGHT);
      e &= ~(e - 1);                         /* one direction at a time */
      if (g_cursor < ADDR_NKEYS)
      {
         int to = kp_step(&pad, g_cursor, e);
         if (to >= 0)
            g_cursor = to;
         else if (to == KP_EDGE_R && nrec)
            g_cursor = ADDR_RECENT_BASE;
      }
      else
      {
         int r = g_cursor - ADDR_RECENT_BASE;
         if (e == PSP_CTRL_UP && r > 0)
            g_cursor--;
         else if (e == PSP_CTRL_DOWN && r < nrec - 1)
            g_cursor++;
         else if (e == PSP_CTRL_LEFT)
            g_cursor = 3;                    /* the keypad's right column */
      }
   }
   len = (int)strlen(g_ad.text);
   if (edges & PSP_CTRL_CROSS)
   {
      if (g_cursor >= ADDR_RECENT_BASE)
         return addr_go(g_pcfg.net_addr[g_cursor - ADDR_RECENT_BASE]);
      act = ADDR_KEYS[g_cursor].act;
      if (act == KP_CHAR && len < (int)sizeof(g_ad.text) - 3)
      {
         g_ad.text[len++] = ADDR_KEYS[g_cursor].ch;
         g_ad.text[len] = '\0';
      }
   }
   if ((edges & PSP_CTRL_SQUARE) || act == KP_DEL)
   {
      if (len)
         g_ad.text[--len] = '\0';
   }
   ok = addr_valid(g_ad.text) == 0;
   if ((edges & (PSP_CTRL_TRIANGLE | PSP_CTRL_START)) || act == KP_OK)
   {
      if (ok)
         return addr_go(g_ad.text);
      snprintf(g_play_note, sizeof(g_play_note), "Not a whole address yet. "
               "Example: 203.0.113.9:19019");
   }
   else if (edges)
      g_play_note[0] = '\0';
   if (edges & PSP_CTRL_CIRCLE)
   {
      g_play_note[0] = '\0';
      screen_to(SCR_WIRELESS);
      g_cursor = PL_JOIN;
      return UI_ACT_NONE;
   }

   ov_backdrop(OV_SCRIM_DENSE);
   vw_head("Type the address the host's screen shows.", "JOIN A GAME");
   vw_box(20, 66, 440, 28, g_cursor < ADDR_NKEYS);
   vid_text_hd(28, 69, g_ad.text, C_SEL);
   vw_caret(28 + vid_text_hd_w(g_ad.text) + 1, 70, 20);
   /* In the box's right end: is it an address yet. */
   ov_text_right(452, 72, ok ? "looks right" : len ? "not a whole address yet"
                 : "like 203.0.113.9:19019", ok ? C_VALUE : C_DIM);
   kp_draw(&pad, g_cursor < ADDR_NKEYS ? g_cursor : -1,
           ok ? 0u : 1u << ADDR_KEY_OK);
   {
      int x = 208, y = 102;
      if (nrec)
      {
         int lx = x + vid_text_w("RECENT") + FE_FONT_W;
         vid_text(x, y, "RECENT", C_ACCENT);
         vid_rect(lx, y + FE_FONT_H / 2, 456 - lx, 1, C_ACCENT_DK, 120);
         for (i = 0; i < nrec; i++)
         {
            int ry = y + 24 + i * 30, sel = g_cursor == ADDR_RECENT_BASE + i;
            if (sel)
            {
               int by = vid_band_y(ry, 24);
               vid_rect(x - 6, by, 462 - x, 24, C_ACCENT, 40);
               vid_rect(x - 6, by, 3, 24, C_ACCENT, 255);
            }
            vid_text(x + 4, ry, g_pcfg.net_addr[i], sel ? C_SEL : C_ITEM);
            tch_row(ADDR_RECENT_BASE + i, x - 6, vid_band_y(ry, 24) - 3,
                    462 - x, 30);
         }
      }
      else
      {
         vw_wrap(x, y + 2, 250, "The host chooses Play together > Host a "
                 "game with Connection set to Internet. Their screen then "
                 "shows the address to type here.", C_DIM, 6);
      }
   }
   if (g_play_note[0])
      vw_plate(g_play_note);
   else
      footer(VID_GLYPH_X " type   " VID_GLYPH_SQ " delete   " VID_GLYPH_TRI
             " join   " VID_GLYPH_O " back");
   return UI_ACT_NONE;
}

/* ---- ad hoc: what comes next ---------------------------------------------- */

static ui_action screen_adhoc(unsigned edges)
{
   enum { AH_GO, AH_BACK, AH_COUNT };
   if (g_cursor >= AH_COUNT)
      g_cursor = 0;
   tch_rows(&g_cursor, &edges);
   if (edges & (PSP_CTRL_UP | PSP_CTRL_DOWN))
      g_cursor ^= 1;
   if ((edges & PSP_CTRL_CIRCLE) ||
       ((edges & PSP_CTRL_CROSS) && g_cursor == AH_BACK))
   {
      screen_to(SCR_WIRELESS);
      g_cursor = g_adhoc_host ? PL_HOST : PL_JOIN;
      return UI_ACT_NONE;
   }
   if (edges & PSP_CTRL_CROSS)
   {
      if (g_adhoc_host)
         return play_host_go(g_host_cable);
      memset(&g_join_tgt, 0, sizeof(g_join_tgt));
      g_join_tgt.conn = UI_CONN_ADHOC;
      g_join_tgt.kind = g_pcfg.console != FE_CONSOLE_GBA ? ROOM_CABLE_GB
                      : ui_host_mode() == UI_HOST_ADAPTER ? ROOM_ADAPTER
                                                          : ROOM_CABLE_GBA;
      g_join_tgt.number = room_number(g_pcfg.group);
      snprintf(g_join_group, sizeof(g_join_group), "%s", g_pcfg.group);
      fe_evt("play_join adhoc kind=%d", g_join_tgt.kind);
      screen_to(SCR_MENU);
      return UI_ACT_NET_JOIN;
   }
   ov_backdrop(OV_SCRIM_DENSE);
   vw_head(g_ov_title, "AD HOC");
   vid_text(22, 68, "Next: the " UI_DEV "'s own connection screen.", C_VALUE);
   vw_wrap(22, 90, 436, g_adhoc_host
           ? "It links the two " UI_DEVS " directly, with no router. Follow "
             "it to create the connection; the other " UI_DEV " then joins "
             "from the same screen. When it is done you are back in the game."
           : "It links the two " UI_DEVS " directly, with no router. Follow "
             "it to join the connection the other " UI_DEV " created. When "
             "it is done you are back in the game.", C_ITEM, 4);
   vid_text(22, 150, "Wi-Fi is set aside while linked and returns "
            "afterwards.", C_DIM);
   vw_row(AH_GO, 182, 300, g_cursor == AH_GO, 1, "Continue", NULL);
   vw_row(AH_BACK, 182 + VW_PITCH, 300, g_cursor == AH_BACK, 1, "Back", NULL);
   footer(VID_GLYPH_X " select   " VID_GLYPH_O " back");
   return UI_ACT_NONE;
}

/* ---- two players on this Vita --------------------------------------------- */

static const char *layout_name(int l)
{
   return l == VID_PAIR_INSET ? "large + small" : "side by side";
}

/* The partner's game: a list of the other cartridges of this console, in
 * place of the page's rows while it is open.  Row 0 is "this game". */
#define LP_ROWS 5
static struct { int open, n, cur, top; } g_lpick;

static void local_pick_open(const char *current)
{
   int i;
   g_lpick.open = 1;
   g_lpick.n = ui_host_local_roms();
   g_lpick.cur = g_lpick.top = 0;
   for (i = 0; current && i < g_lpick.n; i++)
      if (!strcmp(ui_host_local_rom_name(i), current))
         g_lpick.cur = i + 1;
   fe_evt("play_local_pick open n=%d", g_lpick.n);
}

static void local_pick(unsigned edges, const ui_local_state *ls)
{
   int total = g_lpick.n + 1, i;

   tch_rows(&g_lpick.cur, &edges);
   if (edges & PSP_CTRL_UP)
      g_lpick.cur = (g_lpick.cur + total - 1) % total;
   if (edges & PSP_CTRL_DOWN)
      g_lpick.cur = (g_lpick.cur + 1) % total;
   if (edges & PSP_CTRL_LTRIGGER)
      g_lpick.cur = g_lpick.cur >= LP_ROWS ? g_lpick.cur - LP_ROWS : 0;
   if (edges & PSP_CTRL_RTRIGGER)
      g_lpick.cur = g_lpick.cur + LP_ROWS < total ? g_lpick.cur + LP_ROWS
                                                 : total - 1;
   if (g_lpick.cur < g_lpick.top)
      g_lpick.top = g_lpick.cur;
   if (g_lpick.cur >= g_lpick.top + LP_ROWS)
      g_lpick.top = g_lpick.cur - LP_ROWS + 1;
   if (edges & PSP_CTRL_CROSS)
   {
      int rc = ui_host_local_rom_pick(g_lpick.cur - 1);
      fe_evt("play_local_pick row=%d rc=%d", g_lpick.cur, rc);
      if (rc != 0)
         snprintf(g_play_note, sizeof(g_play_note),
                  "That game could not be chosen.");
      g_lpick.open = 0;
      tch_reset();
      return;
   }
   if (edges & PSP_CTRL_CIRCLE)
   {
      g_lpick.open = 0;
      tch_reset();
      return;
   }

   ov_backdrop(OV_SCRIM_DENSE);
   vw_head(g_ov_title, "PARTNER'S GAME");
   vw_wrap(22, 68, 436, g_lpick.n
           ? "The second machine can hold another game: the other version, "
             "with the save you have for it."
           : "No other game for this console is in your game list.",
           C_ITEM, 2);
   for (i = g_lpick.top; i < total && i < g_lpick.top + LP_ROWS; i++)
   {
      int y = 108 + (i - g_lpick.top) * VW_PITCH, sel = g_lpick.cur == i;
      const char *nm = i ? ui_host_local_rom_name(i - 1) : "This game";
      int chosen = i ? (ls->partner_game && !strcmp(ls->partner_game, nm))
                     : !ls->partner_game;
      vw_row(i, y, 440, sel, 1, nm,
             chosen ? "chosen" : i ? NULL : "a copy, or a second save");
   }
   if (total > LP_ROWS)
   {
      /* where the window is in the list */
      int track = LP_ROWS * VW_PITCH - 4;
      int kh = track * LP_ROWS / total, ky = track * g_lpick.top / total;
      vid_rect(452, 108, 2, track, C_DIM, 90);
      vid_rect(452, 108 + ky, 2, kh < 6 ? 6 : kh, C_ACCENT, 255);
   }
   footer(total > LP_ROWS
          ? VID_GLYPH_X " choose   L/R page   " VID_GLYPH_O " back"
          : VID_GLYPH_X " choose   " VID_GLYPH_O " back");
}

static ui_action screen_local(unsigned edges)
{
   enum { LS_COPY, LS_OWN, LS_GAME, LS_LAYOUT, LS_COUNT };
   enum { LA_SWAP, LA_LAYOUT, LA_STOP, LA_RESUME, LA_COUNT };
   ui_local_state ls;
   ui_action act = UI_ACT_NONE;
   int ids[LA_COUNT], n = 0, i, id, other;
   char own[80];

   memset(&ls, 0, sizeof(ls));
   ui_host_local_state(&ls);
   other = ls.partner_game && ls.partner_game[0];
   if (ls.active)
      g_lpick.open = 0;
   if (g_lpick.open)
   {
      local_pick(edges, &ls);
      return UI_ACT_NONE;
   }
   if (g_play_pending)
   {
      if (ls.active)
         g_play_pending = 0;
      else if (g_link_error[0] && g_play_pending > 2)
      {
         snprintf(g_play_note, sizeof(g_play_note), "%s", g_link_error);
         g_play_pending = 0;
      }
      else if (++g_play_pending > PLAY_PENDING_MAX)
      {
         const char *a, *b;
         ui_link_fail_text(UI_FAIL_LOCAL, &a, &b);
         snprintf(g_play_note, sizeof(g_play_note), "%s", a);
         g_play_pending = 0;
      }
   }
   if (ls.active)
   {
      ids[n++] = LA_SWAP;
      ids[n++] = LA_LAYOUT;
      ids[n++] = LA_STOP;
      ids[n++] = LA_RESUME;
   }
   else
   {
      ids[n++] = LS_COPY;
      ids[n++] = LS_OWN;
      ids[n++] = LS_GAME;
      ids[n++] = LS_LAYOUT;
   }
   if (g_cursor >= n)
      g_cursor = 0;
   if (edges)
      g_play_note[0] = '\0';
   tch_rows(&g_cursor, &edges);
   if (edges & PSP_CTRL_UP)
      g_cursor = (g_cursor + n - 1) % n;
   if (edges & PSP_CTRL_DOWN)
      g_cursor = (g_cursor + 1) % n;
   /* a copy of ANOTHER game is not a thing: the cursor rests on the start
    * that exists */
   if (!ls.active && other && ids[g_cursor] == LS_COPY)
      g_cursor = (edges & PSP_CTRL_UP) ? n - 1 : 1;
   id = ids[g_cursor];
   if ((edges & (PSP_CTRL_LEFT | PSP_CTRL_RIGHT | PSP_CTRL_CROSS)) &&
       id == (ls.active ? (int)LA_LAYOUT : (int)LS_LAYOUT))
   {
      g_pcfg.local_layout = g_pcfg.local_layout == VID_PAIR_INSET
                               ? VID_PAIR_SIDE : VID_PAIR_INSET;
      g_settings_dirty = 1;
      edges &= ~PSP_CTRL_CROSS;
   }
   if (edges & PSP_CTRL_CIRCLE)
   {
      if (ls.active)
      {
         screen_to(SCR_MENU);
         return UI_ACT_NONE;
      }
      screen_to(SCR_WIRELESS);
      g_cursor = PL_LOCAL;
      return UI_ACT_NONE;
   }
   if (edges & PSP_CTRL_CROSS)
   {
      if (ls.active)
      {
         if (id == LA_SWAP)
            act = UI_ACT_NET_LOCAL_SWAP;
         else if (id == LA_STOP)
            act = UI_ACT_NET_LOCAL_STOP;
         else if (id == LA_RESUME)
            return UI_ACT_RESUME;
      }
      else if (id == LS_GAME)
      {
         if (!g_play_pending)
         {
            local_pick_open(ls.partner_game);
            tch_reset();
            local_pick(0, &ls);
            return UI_ACT_NONE;
         }
      }
      else if (!ls.available)
         snprintf(g_play_note, sizeof(g_play_note), "%s",
                  ls.why_not && ls.why_not[0] ? ls.why_not
                  : "This game cannot run twice on one " UI_DEV ".");
      else if (!g_play_pending)
      {
         g_pcfg.local_partner = id == LS_OWN;
         g_settings_dirty = 1;
         g_play_pending = 1;
         fe_evt("play_local partner=%d layout=%d", g_pcfg.local_partner,
                g_pcfg.local_layout);
         act = UI_ACT_NET_LOCAL;
      }
   }

   ov_backdrop(OV_SCRIM_DENSE);
   vw_head(g_ov_title, "TWO PLAYERS");
   if (ls.active)
   {
      char line[120];
      if (other)
         snprintf(line, sizeof(line), "Linked by cable with %s",
                  ls.partner_game);
      else
         snprintf(line, sizeof(line), "Two games are running on this " UI_DEV
                  ", linked by cable.");
      vw_line(22, 68, 436, line, C_VALUE);
      vid_text(22, 86, ls.have_pad2
               ? "Your buttons: your game. Second controller: the "
                 "partner's."
               : ls.controlled ? "The buttons drive the partner's game."
                               : "The buttons drive your game.", C_ITEM);
      for (i = 0; i < n; i++)
      {
         int y = 114 + i * VW_PITCH, sel = g_cursor == i;
         switch (ids[i])
         {
         case LA_SWAP:
            /* two pads: nobody's buttons move, the view does */
            vw_row(i, y, 440, sel, 1, ls.have_pad2
                   ? "Swap sound and large picture" : "Swap controlled game",
                   ls.controlled ? "now: the partner's" : "now: yours");
            break;
         case LA_LAYOUT:
            vw_row(i, y, 440, sel, 1, "Layout",
                   layout_name(g_pcfg.local_layout));
            break;
         case LA_STOP:
            vw_row(i, y, 440, sel, 1, "Stop", "your game carries on alone");
            break;
         default:
            vw_row(i, y, 440, sel, 1, "Back to the game", NULL);
            break;
         }
      }
      /* (Host a game and Join a game are not on offer while this runs:
       * the page says so where the way out is) */
      vw_line(22, 226, 436, ids[g_cursor] == LA_STOP
              ? "Stop it first to host or join a game over the network."
              : ls.have_pad2
              ? "In the game: right stick sideways or a tap swaps the sound."
              : "In the game: right stick sideways or a tap on a picture "
                "swaps.", C_DIM);
   }
   else
   {
      vw_wrap(22, 66, 436, "Both games run here, joined by a link cable: "
              "trade with yourself, or pass the " UI_DEV " to a friend.",
              C_ITEM, 2);
      snprintf(own, sizeof(own), other ? "Start: the partner plays its game"
                                       : "Start: the partner uses its own "
                                         "save");
      vw_row(0, 106, 440, g_cursor == 0, ls.available && !other,
             "Start: the partner copies my save",
             other ? "same game only" : NULL);
      vw_row(1, 106 + VW_PITCH, 440, g_cursor == 1, ls.available, own,
             ls.partner_save ? "save found" : "starts a new game");
      vw_row(2, 106 + 2 * VW_PITCH, 440, g_cursor == 2, 1, "Partner's game",
             other ? ls.partner_game : "this one");
      vw_row(3, 106 + 3 * VW_PITCH, 440, g_cursor == 3, 1, "Layout",
             layout_name(g_pcfg.local_layout));
      vw_wrap(22, 212, 436,
              g_play_pending ? "Starting the second game..."
              : !ls.available ? (ls.why_not && ls.why_not[0] ? ls.why_not
                 : "This game cannot run twice on one " UI_DEV ".")
              : id == LS_COPY
              ? "The second game begins as a copy of yours, exactly where "
                "you are now. If it saves, that is the second save file."
              : id == LS_OWN
              ? (other ? "The second machine boots that game with its own "
                         "save, the one you play it with."
                       : "The second game starts from its own save file, "
                         "like a second cartridge. It is kept beside yours.")
              : id == LS_GAME
              ? "The same game twice, or the other version of it in the "
                "second machine."
              : "Side by side: two equal pictures. Large + small: the game "
                "you drive is the large one.", C_DIM, 2);
   }
   if (g_play_note[0])
      vw_plate(g_play_note);
   else if (ls.active)
      footer(VID_GLYPH_X " select   " VID_GLYPH_O " back");
   else
      footer(id == LS_GAME ? VID_GLYPH_X " choose   " VID_GLYPH_O " back"
             : id == LS_LAYOUT ? VID_GLYPH_X " change   " VID_GLYPH_O " back"
             : VID_GLYPH_X " start   " VID_GLYPH_O " back");
   return act;
}

/* ---- Connection ------------------------------------------------------------ */

static const char *delay_name(int d, char *buf, size_t cap)
{
   if (d <= 0)
      return "Auto";
   snprintf(buf, cap, d == 1 ? "%d frame" : "%d frames", d);
   return buf;
}

static ui_action screen_conn(unsigned edges)
{
   enum { CN_DELAY = UI_CONN_COUNT, CN_COUNT };
   static const char *const what[UI_CONN_COUNT] = {
      "Same Wi-Fi or hotspot. Rooms are found by themselves.",
      "Far apart: the joiner types the address the host is shown.",
      "No router: the two " UI_DEVS " connect directly, side by side.",
   };
   const ui_net_status *net = play_net(0);
   char db[16];
   int i;
   if (g_cursor >= CN_COUNT)
      g_cursor = 0;
   tch_rows(&g_cursor, &edges);
   if (edges & PSP_CTRL_UP)
      g_cursor = (g_cursor + CN_COUNT - 1) % CN_COUNT;
   if (edges & PSP_CTRL_DOWN)
      g_cursor = (g_cursor + 1) % CN_COUNT;
   if (g_cursor == CN_DELAY)
   {
      if (edges & PSP_CTRL_LEFT)
         settings_adjust(SET_DELAY, -1);
      if (edges & (PSP_CTRL_RIGHT | PSP_CTRL_CROSS))
         settings_adjust(SET_DELAY, +1);
   }
   else if (edges & PSP_CTRL_CROSS)
   {
      if (g_pcfg.net_conn != g_cursor)
      {
         g_pcfg.net_conn = g_cursor;
         g_settings_dirty = 1;
         fe_evt("net_conn value=%d", g_pcfg.net_conn);
      }
      play_net(1);
   }
   if (edges & PSP_CTRL_CIRCLE)
   {
      screen_to(SCR_WIRELESS);
      g_cursor = PL_CONN;
      return UI_ACT_NONE;
   }

   ov_backdrop(OV_SCRIM_DENSE);
   vw_head(NULL, "CONNECTION");
   play_status_draw(52, g_pcfg.net_conn, net);
   for (i = 0; i < UI_CONN_COUNT; i++)
   {
      int y = 78 + i * 38, sel = g_cursor == i, on = g_pcfg.net_conn == i;
      if (sel)
      {
         vid_rect(0, y - 4, 460, 36, C_ACCENT, 40);
         vid_rect(0, y - 4, 4, 36, C_ACCENT, 255);
      }
      /* The chosen one wears the console colour's dot. */
      ov_outline(18, y + 2, 10, 10, on ? C_CON : C_DIM, on ? 255 : 160);
      if (on)
         vid_rect(21, y + 5, 4, 4, C_CON, 255);
      vid_text(36, y, conn_name(i), sel ? C_SEL : C_ITEM);
      vw_line(36, y + 15, 420, what[i], sel ? C_VALUE : C_DIM);
      tch_row(i, 0, y - 5, 460, 38);
   }
   {
      int y = 78 + 3 * 38 + 4;
      vw_row(CN_DELAY, y, 440, g_cursor == CN_DELAY, 1, "Link delay",
             delay_name(g_pcfg.link_delay, db, sizeof(db)));
      vid_text(22, y + 19, g_pcfg.link_delay
               ? "Fewer frames react faster; more ride out a slow "
                 "connection."
               : "Auto picks it from the connection. The host's choice "
                 "counts.", C_DIM);
   }
   footer(g_cursor == CN_DELAY
          ? "DPAD change   " VID_GLYPH_O " back"
          : VID_GLYPH_X " choose   " VID_GLYPH_O " back");
   return UI_ACT_NONE;
}

/* ---- Mystery Gift ------------------------------------------------------------
 * The PSP's page (ui_psp.c screen_mgift) picks a stored connection; a Vita
 * application cannot choose a network, so this one says which Wi-Fi the
 * console is on, what the station said, and the three steps -- on the
 * phone, on the Vita, in the game.  vita/mgift_vita.c does the work. */
enum { MGV_START, MGV_BACK, MGV_COUNT };

static ui_action screen_mgift(unsigned edges)
{
   static const char *const steps[][2] = {
      { "Phone", "Mystery Gift Station: pick a gift." },
      { UI_DEV,  "Settings > Network: join the Wi-Fi Mystery Gift." },
      { "Here",  "Start listening, or Select + Down in the game." },
      { "Game",  "MYSTERY GIFT on the title screen." },
   };
   const char *l1 = mgift_ui_line1();
   const char *l2 = mgift_ui_line2();
   int running = mgift_ui_active();
   int i, y;

   (void)tch_rows(&g_cursor, &edges);
   if (g_cursor >= MGV_COUNT)
      g_cursor = MGV_COUNT - 1;
   if (edges & PSP_CTRL_UP)
      g_cursor = (g_cursor + MGV_COUNT - 1) % MGV_COUNT;
   if (edges & PSP_CTRL_DOWN)
      g_cursor = (g_cursor + 1) % MGV_COUNT;
   /* O backs out but does not stop the listener: a gift can be arriving
    * while the player watches the game (the PSP's rule). */
   if ((edges & PSP_CTRL_CROSS) && g_cursor == MGV_START)
      return running ? UI_ACT_NET_MGIFT_STOP : UI_ACT_NET_MGIFT;
   if (edges & (PSP_CTRL_CIRCLE | PSP_CTRL_CROSS))
   {
      screen_to(SCR_WIRELESS);
      g_cursor = PL_MGIFT;      /* the row this page came from (a GBA game) */
      return UI_ACT_NONE;
   }

   ov_backdrop(OV_SCRIM);
   vw_head("FireRed, LeafGreen and Emerald, from a phone", "MYSTERY GIFT");
   ov_status_right(running ? "LISTENING" : "OFF");
   play_status_draw(68, UI_CONN_WIFI, play_net(0));
   vw_line(22, 84, 436, l1 && l1[0] ? l1 : "Not listening", C_VALUE);
   if (l2 && l2[0])
      vw_line(22, 84 + FE_FONT_H + 2, 436, l2, C_ITEM);
   y = 122;
   vw_row(MGV_START, y, 440, g_cursor == MGV_START, 1,
          running ? "Stop listening" : "Start listening", NULL);
   vw_row(MGV_BACK, y + VW_PITCH, 440, g_cursor == MGV_BACK, 1, "Back", NULL);
   if (!running)
   {
      y += 2 * VW_PITCH;
      for (i = 0; i < (int)(sizeof(steps) / sizeof(steps[0])); i++)
      {
         int lead = 52;
         vid_text(22, y, steps[i][0], C_ACCENT_DK);
         y = vw_wrap_lead(22, y, 436, steps[i][1], C_DIM, 1, lead, lead);
      }
   }
   if (g_cursor == MGV_BACK)
      footer(VID_GLYPH_X " back   " VID_GLYPH_O " back");
   else if (running)
      footer(VID_GLYPH_X " stop   " VID_GLYPH_O " back (keeps listening)");
   else
      footer(VID_GLYPH_X " start   " VID_GLYPH_O " back");
   return UI_ACT_NONE;
}

/* ---- How to play together -------------------------------------------------- */

static ui_action screen_help(unsigned edges)
{
   /* Page 1: the steps.  Page 2: each connection, its name leading. */
   static const char *const p1[][2] = {
      { "1", "Both players open the same game." },
      { "2", "One chooses Play together > Host a game." },
      { "3", "The other chooses Play together > Join a game and picks the "
             "room in the list." },
      { "4", "When it says Linked, use the game's own link feature: the "
             "trade centre, a link battle, the cable club." },
      { "", "To stop, either player chooses Play together > Disconnect. "
            "Both games save and carry on alone." },
   };
   static const char *const p2[][2] = {
      { "Wi-Fi", "Same network or a phone's hotspot. Rooms show up in the "
                 "list by themselves. The easy one." },
      { "Internet", "For a friend far away. The host's screen shows an "
                    "address; the joiner types it." },
      { "Ad hoc", "No router anywhere? The two " UI_DEVS " connect "
                  "directly, through the " UI_DEV "'s own connection "
                  "screen." },
      { "Two players on this " UI_DEV, "Both games run on this console: "
                  "trade with yourself, no second " UI_DEV " needed." },
   };
   const char *const (*p)[2] = g_help_page ? p2 : p1;
   int n = g_help_page ? (int)(sizeof(p2) / sizeof(p2[0]))
                       : (int)(sizeof(p1) / sizeof(p1[0]));
   int i, y = 62;
   if (edges & (PSP_CTRL_CROSS | PSP_CTRL_LEFT | PSP_CTRL_RIGHT))
      g_help_page ^= 1;
   if (edges & PSP_CTRL_CIRCLE)
   {
      screen_to(SCR_WIRELESS);
      g_cursor = PL_COUNT;               /* clamps to the last row: this one */
      return UI_ACT_NONE;
   }
   ov_backdrop(OV_SCRIM_DENSE);
   vw_head(g_help_page ? "Which connection?" : "The four steps",
           "HOW TO PLAY");
   ov_text_right(460, 48, g_help_page ? "2 / 2" : "1 / 2", C_DIM);
   for (i = 0; i < n; i++)
   {
      int lead = p[i][0][0] ? vid_text_w(p[i][0]) + (g_help_page ? 10 : 12)
                            : 0;
      y += 8;
      if (lead)
         vid_text(22, y, p[i][0], g_help_page ? C_SEL : C_ACCENT_DK);
      y = vw_wrap_lead(22, y, 436, p[i][1], lead ? C_ITEM : C_DIM, 3, lead,
                       g_help_page ? 0 : lead);
   }
   /* Anywhere on the page turns it. */
   tch_btn(PSP_CTRL_CROSS, 0, 44, VID_SCR_W, TCH_HINT_Y - 44);
   footer(g_help_page ? VID_GLYPH_X " the steps   " VID_GLYPH_O " back"
                      : VID_GLYPH_X " which connection?   " VID_GLYPH_O
                        " back");
   return UI_ACT_NONE;
}

#endif /* UI_VITA_PLAY_H */
