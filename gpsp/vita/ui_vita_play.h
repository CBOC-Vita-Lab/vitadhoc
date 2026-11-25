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
 *   SCR_CONN       Same Wi-Fi / Online / Nearby (and Direct address when
 *                  vita.ini link_direct = 1), each with its one line, then
 *                  Link delay and Always use the relay
 *   SCR_ONLINE_CODE, SCR_ONLINE_CARD   Online: the code pad and the room
 *                  card, in ui_vita_online.h
 *   SCR_HELP       how to play together, two short pages
 *
 * NOTHING HERE WAITS.  The network is asked through vita/ui_host_hooks.h:
 * its state twice a second (play_net), the rooms every frame of the Join
 * page, the local link every frame of its pages.
 */
#ifndef UI_VITA_PLAY_H
#define UI_VITA_PLAY_H

#include "online_vita.h"      /* ON_*, ONLINE_Q_*, ONLINE_APP_VERSION */

/* ---- state -------------------------------------------------------------- */

static ui_net_status g_net;            /* the last answer                    */
static unsigned g_net_frame;           /* g_vframe when it was asked         */
static int  g_net_have;
static char g_play_note[112];          /* the plate, until the next key      */
static int  g_play_pending;            /* frames since a Host / local start
                                        * was returned and nothing answered  */
#define PLAY_PENDING_MAX 600           /* 10 s: then the page says so        */
static ui_join_info g_join_tgt;
/* The last room card's version and game title: the %s of the Online
 * sentences (ui_online_fail_text), whoever asks for them. */
static char g_ol_last_ver[16], g_ol_last_title[24];
static int  g_adhoc_host;              /* SCR_ADHOC: 1 host, 0 join          */
static int  g_help_page;

const ui_join_info *ui_join_target(void)
{
   return &g_join_tgt;
}

/* A page that picks the room itself (the SNES Two players page) hands it over here. */
void ui_join_set(const ui_join_info *t)
{
   if (t)
      g_join_tgt = *t;
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
   return c == UI_CONN_INTERNET ? tr("play.direct_address")
        : c == UI_CONN_ADHOC ? tr("play.nearby")
        : c == UI_CONN_ONLINE ? tr("play.online") : tr("play.same_wi_fi");
}

/* The Connection page's rows, in the order they are shown: Same Wi-Fi,
 * Online, Nearby, and the typed address only when vita.ini link_direct = 1
 * (the old way, kept for a player who forwards a port by hand). */
static int conn_rows(int ids[UI_CONN_COUNT])
{
   int n = 0;
   ids[n++] = UI_CONN_WIFI;
   ids[n++] = UI_CONN_ONLINE;
   ids[n++] = UI_CONN_ADHOC;
   if (ui_host_link_direct())
      ids[n++] = UI_CONN_INTERNET;
   return n;
}

/* The row of `conn` on that page (a value that is not shown: the first). */
static int conn_row_of(int conn)
{
   int ids[UI_CONN_COUNT], n = conn_rows(ids), i;
   for (i = 0; i < n; i++)
      if (ids[i] == conn)
         return i;
   return 0;
}

/* The next connection in the page's order, `dir` steps on (Settings). */
static int conn_step(int conn, int dir)
{
   int ids[UI_CONN_COUNT], n = conn_rows(ids);
   return ids[(conn_row_of(conn) + n + dir) % n];
}

/* A NES or WonderSwan game links one way only: Two players (both Vitas run
 * the same game, one pad each). */
static int pad_console(void)
{
   return g_pcfg.console == FE_CONSOLE_NES ||
          g_pcfg.console == FE_CONSOLE_WSWAN;
}

static const char *kind_name(int kind)
{
   return kind == ROOM_ADAPTER ? tr("play.wireless_adapter")
        : kind == ROOM_PAD ? tr("play.two_players")
        : kind == ROOM_CABLE_GB ? tr("play.gb_link_cable") : tr("play.link_cable");
}

/* Every failure, in the player's words (ui_psp.h). */
void ui_link_fail_text(int why, const char **l1, const char **l2)
{
   const char *const t[UI_FAIL_COUNT][2] = {
      { tr("play.this_vita_is_not_connected_to"),
        tr("play.connect_in_the_vita_s_settings") },
      { tr("play.nobody_answered"),
        tr("play.is_the_other_vita_hosting_and") },
      { tr("play.that_room_is_full"),
        tr("play.a_room_takes_two_players_ask") },
      { tr("play.the_other_vita_runs_a_different"),
        tr("play.install_the_same_version_on_both") },
      { tr("play.the_other_player_s_game_did"),
        tr("play.try_again_if_it_keeps_happening") },
      { tr("play.the_host_closed_the_room"),
        tr("play.ask_them_to_choose_host_a") },
      { tr("play.link_lost_the_other_vita_is"),
        tr("play.your_game_carries_on_save_as") },
      { tr("play.the_two_games_fell_out_of"),
        tr("play.the_link_ended_and_nothing_was") },
      { tr("play.that_is_not_an_address"),
        tr("play.it_looks_like_192_168_1") },
      { tr("play.the_connection_screen_was_closed"),
        tr("play.nothing_was_changed_choose_host_or") },
      { tr("play.wi_fi_and_ad_hoc_cannot"),
        tr("play.end_the_current_link_first") },
      { tr("play.the_room_could_not_be_opened"),
        tr("play.the_port_may_be_in_use") },
      { tr("play.not_enough_memory_to_link_this"),
        tr("play.quit_to_the_game_list_open") },
      { tr("play.the_second_game_would_not_start"),
        tr("play.this_game_cannot_run_twice_on") },
   };
   if (why >= UI_FAIL_ONLINE_CODE && why < UI_FAIL_COUNT)
   {
      ui_online_fail_text(why, why == UI_FAIL_ONLINE_GAME ||
                          why == UI_FAIL_ONLINE_GAME_FILE ? g_ol_last_title
                          : g_ol_last_ver, ONLINE_APP_VERSION, l1, l2);
      return;
   }
   if (why < 0 || why >= UI_FAIL_COUNT)
      why = UI_FAIL_NO_ANSWER;
   if (l1) *l1 = tr(t[why][0]);
   if (l2) *l2 = tr(t[why][1]);
}

/* "Wi-Fi: HomeNet  192.168.1.23" -- the one live line every Play together
 * page starts with.  Returns 1 when the network is usable for `conn`. */
static int play_status_line(char *out, size_t cap, int conn,
                            const ui_net_status *n)
{
   if (conn == UI_CONN_ADHOC)
   {
      snprintf(out, cap, tr("play.ad_hoc_the_two_vitas_connect")
);
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
      snprintf(out, cap, tr("play.connecting_to_wi_fi"), conn_name(conn));
   else if (n->radio == UI_RADIO_ADHOC)
      snprintf(out, cap, tr("play.the_radio_is_in_ad_hoc"), conn_name(conn));
   else
      snprintf(out, cap, tr("play.no_wi_fi_connect_in_the")
);
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
   if (pad_console())
      wl_line_name(out, cap, tr("play.two_players_both_vitas_need"),
                   own_game_name(), 436);
   else if (g_pcfg.console != FE_CONSOLE_GBA)
      wl_line_name(out, cap, tr("play.link_cable_game_player_2_needs"),
                   own_game_name(), 436);
   else
      switch (ui_host_mode())
      {
      case UI_HOST_CABLE:
         /* Each side may run its own game (Ruby with Sapphire): the partner's cartridge crosses once
          * (fe_gblink), so nobody needs this very file. */
         wl_line_name(out, cap, tr("play.link_cable_game_each_vita_plays"),
                      own_game_name(), 436);
         break;
      case UI_HOST_ASK:
         snprintf(out, cap, tr("play.links_by_link_cable_or_wireless")
);
         break;
      default:
         snprintf(out, cap, tr("play.wireless_adapter_game_the_game_itself")
);
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
   if (!pad_console())
      ids[n++] = PL_LOCAL;           /* a NES / WonderSwan game runs once */
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
/* ui_vita_online.h */
static ui_action screen_online_code(unsigned edges);
static void online_code_open(void);
static void online_host_page(int y_top, const char *game);

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
   if (g_pcfg.net_conn == UI_CONN_ONLINE)
   {
      /* a code, not a room number: the number means nothing over the
       * internet */
      snprintf(out, cap, tr("play.a_code_for_your_friend"));
      return;
   }
   snprintf(out, cap, "%s  -  %s", room_label(g_pcfg.group, lbl, sizeof(lbl)),
            pad_console() ? tr("play.two_players") : m == UI_HOST_ASK ? tr("play.cable_or_adapter") : m == UI_HOST_ADAPTER
               ? tr("play.wireless_adapter") : tr("play.link_cable"));
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
   const char *mode = pad_console() ? tr("play.two_players")
                    : g_link.cable ? tr("play.link_cable") : tr("play.wireless_adapter");
   int y, online = 0;

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
      snprintf(l1, sizeof(l1), tr("play.opening"), g_link_room[0] ? g_link_room
               : room_label(g_pcfg.group, big, sizeof(big)));
      big[0] = '\0';
      snprintf(l2, sizeof(l2), conn == UI_CONN_ADHOC
               ? tr("play.the_vita_s_connection_screen_comes")
               : tr("play.one_moment"));
   }
   else if (hosting && conn == UI_CONN_ONLINE)
   {
      /* Online: the code, big, and nothing else the player must read. */
      online = 1;
   }
   else if (hosting)
   {
      snprintf(l1, sizeof(l1), tr("play.hosting_by"), g_link.room, mode);
      snprintf(l2, sizeof(l2), tr("play.waiting_for_player_2_you_can")
);
      if (conn == UI_CONN_INTERNET)
      {
         if (n->public_state == UI_PUBLIC_ASKING)
            snprintf(la, sizeof(la), tr("play.asking_your_router_to_let_the")
);
         else if (n->public_state == UI_PUBLIC_CLOSED)
         {
            char port[8] = "19019";
            const char *c = strrchr(n->host_addr, ':');
            if (c && c[1])
               snprintf(port, sizeof(port), "%.5s", c + 1);
            snprintf(para, sizeof(para), tr("play.your_router_did_not_open_the")


, port,
                     n->ip[0] ? n->ip : tr("play.its_address"),
                     n->public_addr[0] ? ": " : "", n->public_addr);
         }
         else if (n->public_state == UI_PUBLIC_OPEN && !n->public_addr[0])
         {
            /* The router opened the port and would not say its own
             * address: only the port is known. */
            const char *c = strrchr(n->host_addr, ':');
            snprintf(para, sizeof(para), tr("play.your_router_opened_the_way_in")


,
                     c && c[1] ? c : ":19019");
         }
         else
         {
            snprintf(la, sizeof(la), tr("play.tell_the_other_player_this_address"));
            snprintf(big, sizeof(big), "%s", n->public_state == UI_PUBLIC_OPEN
                     && n->public_addr[0] ? n->public_addr : n->host_addr);
            snprintf(lc, sizeof(lc), tr("play.they_choose_play_together_join_a")
);
         }
      }
      else
      {
         wl_line_name(la, sizeof(la), tr("play.on_the_other_vita_open"),
                      own_game_name(), 436);
         if (conn == UI_CONN_ADHOC)
            snprintf(lb, sizeof(lb), tr("play.connection_ad_hoc_then_play_together")
);
         else
         {
            char who[24];
            snprintf(lb, sizeof(lb), tr("play.then_play_together_join_a_game")
, vw_clip(who, sizeof(who),
                     g_pcfg.nick[0] ? g_pcfg.nick : tr("play.this_vita"), 96));
            if (n->host_addr[0])
               snprintf(lc, sizeof(lc), tr("play.not_listed_connection_internet_type")
, n->host_addr);
         }
      }
   }
   else if (g_link.state == UI_LINK_LINKED)
   {
      if (g_link_peer[0])
         snprintf(l1, sizeof(l1), tr("play.linked_with"), g_link_peer);
      else
         snprintf(l1, sizeof(l1), tr("play.linked_with_the_other"));
      /* Online has no room to name: a code that did its work */
      if (g_link.cable && g_link.player && conn == UI_CONN_ONLINE)
         snprintf(l2, sizeof(l2), tr("play.you_are_player"),
                  g_link.player, g_link.player == 1 ? tr("play.the_host") : "",
                  pad_console() ? tr("play.two_players") : tr("play.by_link_cable"));
      else if (g_link.cable && g_link.player)
         snprintf(l2, sizeof(l2), tr("play.you_are_player_in"),
                  g_link.player, g_link.player == 1 ? tr("play.the_host") : "",
                  g_link.room,
                  pad_console() ? tr("play.two_players") : tr("play.by_link_cable"));
      else if (g_link.cable)
         snprintf(l2, sizeof(l2), "%s, %s.", conn == UI_CONN_ONLINE
                  ? tr("play.online") : g_link.room,
                  pad_console() ? tr("play.two_players") : tr("play.by_link_cable"));
      else
         snprintf(l2, sizeof(l2), tr("play.by_wireless_adapter_the_game_pairs")
, conn == UI_CONN_ONLINE ? tr("play.online")
                  : g_link.room);
      if (pad_console())
         snprintf(para, sizeof(para), tr("play.save_states_cheats_rewind_and_fast")


);
      else
         snprintf(para, sizeof(para), tr("play.save_states_and_cheats_are_off")

);
   }
   else
      snprintf(l1, sizeof(l1), "%s", session_info && session_info[0]
               ? session_info : tr("play.a_link_session_is_running"));

   ov_backdrop(OV_SCRIM_DENSE);
   vw_head(g_ov_title, tr("play.play_together"));
   if (online)
      online_host_page(60, own_game_name());
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
      if (conn == UI_CONN_ONLINE)
      {
         /* Great / Good / Slow, the words of the connect toast, and the
          * relay when the link goes through it */
         const char *w = g_link.quality == ONLINE_Q_GREAT ? tr("play.great")
                       : g_link.quality == ONLINE_Q_GOOD ? tr("play.good")
                       : g_link.quality == ONLINE_Q_SLOW ? tr("play.slow") : "";
         char rtt[40];
         rtt[0] = '\0';
         if (g_link.rtt_ms >= 0)
            snprintf(rtt, sizeof(rtt), tr("play.ms_round_trip"), g_link.rtt_ms);
         snprintf(q, sizeof(q), "%s%s   %s%s", conn_name(conn),
                  g_link.relay ? tr("play.through_the_relay") : "", w, w[0] ? rtt : "");
      }
      else if (g_link.rtt_ms >= 0)
         snprintf(q, sizeof(q), tr("play.ms_round_trip_2"),
                  conn_name(conn), g_link.rtt_ms < 40 ? "quick"
                  : g_link.rtt_ms < 120 ? "fine" : tr("play.slow_expect_delay"),
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
          g_link.state == UI_LINK_LINKED ? tr("play.back_to_the_game")
          : pending ? tr("play.back_to_the_game") : tr("play.keep_playing"), NULL);
   vw_row(PS_END, 200 + VW_PITCH, 300, g_cursor == PS_END, 1,
          hosting ? tr("play.stop_hosting") : pending ? tr("play.cancel") : tr("play.disconnect"), NULL);
   /* The other way of linking is not on offer while this one is up
    * (main_vita.c decides that): said beside the row that ends it. */
   if (g_cursor == PS_END)
   {
      ui_local_state ls;
      memset(&ls, 0, sizeof(ls));
      ui_host_local_state(&ls);
      if (!ls.active)
         vw_wrap(334, 198, 126, tr("play.two_players_on_this_vita_after")
, C_DIM, 3);
   }
   footer(tr("hint.x_select_o_back"));
   return UI_ACT_NONE;
}

LARGE_WRAP(ui_action, screen_play, (unsigned edges, int session_active, const char *session_info),
           (edges, session_active, session_info))
static ui_action screen_play_page(unsigned edges, int session_active,
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
         snprintf(g_play_note, sizeof(g_play_note), tr("play.the_room_did_not_open_check")
);
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
      if (id == PL_HOST && g_pcfg.net_conn != UI_CONN_ONLINE)
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
            if (g_pcfg.net_conn == UI_CONN_ONLINE && !ui_host_online_available())
            {
               const char *a, *b;
               ui_link_fail_text(UI_FAIL_RADIO_BUSY, &a, &b);
               snprintf(g_play_note, sizeof(g_play_note), "%s", a);
               break;
            }
            if (g_pcfg.console == FE_CONSOLE_GBA &&
                ui_host_mode() == UI_HOST_ASK)
            {
               screen_to(SCR_HOST);
               return UI_ACT_NONE;
            }
            return play_host_go(g_pcfg.console != FE_CONSOLE_GBA ||
                                ui_host_mode() == UI_HOST_CABLE);
         }
         if (g_pcfg.net_conn == UI_CONN_ONLINE)
         {
            if (!ui_host_online_available())
            {
               const char *a, *b;
               ui_link_fail_text(UI_FAIL_RADIO_BUSY, &a, &b);
               snprintf(g_play_note, sizeof(g_play_note), "%s", a);
               break;
            }
            online_code_open();
            return screen_online_code(0);
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
                     : tr("play.this_game_cannot_run_twice_on"));
            break;
         }
         screen_to(SCR_LOCAL);
         return screen_local(0);
      case PL_CONN:
         screen_to(SCR_CONN);
         g_cursor = conn_row_of(g_pcfg.net_conn);
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
   vw_head(sub, tr("play.play_together"));
   play_status_draw(68, g_pcfg.net_conn, net);
   y0 = n > 5 ? 92 : 96;
   for (i = 0; i < n; i++)
   {
      int y = y0 + i * VW_PITCH, sel = g_cursor == i;
      switch (ids[i])
      {
      case PL_HOST:
         play_host_value(hv, sizeof(hv));
         vw_row(i, y, 440, sel, 1, tr("play.host_a_game"), hv);
         break;
      case PL_JOIN:
         vw_row(i, y, 440, sel, 1, tr("play.join_a_game"),
                g_pcfg.net_conn == UI_CONN_ONLINE ? tr("play.type_your_friend_s_code")
                : g_pcfg.net_conn == UI_CONN_INTERNET ? tr("play.type_the_host_s_address")
                : g_pcfg.net_conn == UI_CONN_ADHOC ? tr("play.a_vita_nearby")
                : tr("play.rooms_on_this_wi_fi"));
         break;
      case PL_LOCAL:
         vw_row(i, y, 440, sel, ls.available, tr("play.two_players_on_this"),
                ls.available ? NULL : tr("play.not_for_this_game"));
         break;
      case PL_CONN:
         vw_row(i, y, 440, sel, 1, tr("play.connection"), conn_name(g_pcfg.net_conn));
         break;
      case PL_MGIFT:
         vw_row(i, y, 440, sel, 1, tr("play.mystery_gift"),
                mgift_ui_active() ? "listening" : tr("play.from_a_phone"));
         break;
      default:
         vw_row(i, y, 440, sel, 1, tr("play.how_to_play_together"), NULL);
         break;
      }
   }
   if (g_play_note[0])
      vw_plate(g_play_note);
   else if (id == PL_HOST && g_pcfg.net_conn == UI_CONN_ONLINE)
      footer(tr("hint.x_host_o_back"));
   else if (id == PL_HOST)
      footer(tr("hint.x_host_dpad_room_number_o"));
   else if (id == PL_CONN)
      footer(tr("hint.x_choose_dpad_change_o_back"));
   else
      footer(tr("hint.x_select_o_back"));
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
   t = r->kind == ROOM_CABLE_GB || r->kind == ROOM_PAD
         ? NULL : link_game_title(r->code);
   return t ? t : r->title[0] ? r->title : tr("play.a_game");
}

/* 1 = a Link Cable room playing another Game Boy Advance game than this
 * one (the two will trade across versions). */
static int join_other_game(const ui_room *r)
{
   char code[5];
   room_code_gba(g_game_code, code);
   return r->kind == ROOM_CABLE_GBA && g_pcfg.console == FE_CONSOLE_GBA &&
          strncmp(r->code, code, 4) != 0;
}

/* A Two players room plays one game file: its code is the first 4 hex
 * digits of the file's CRC (the session's handshake checks the whole file). */
static int join_same_pad_game(const ui_room *r)
{
   char id[9];
   if (!ui_host_pad_game(id))
      return 0;
   return !strncmp(r->code, id, 4);
}

/* Whether this game can enter a room, and the sentence when it cannot. */
typedef struct { int ok; char why[40]; char plate[112]; } join_verdict;

static void join_decide(const ui_room *r, join_verdict *v)
{
   char rl[24], t[40];
   int gba = g_pcfg.console == FE_CONSOLE_GBA;
   memset(v, 0, sizeof(*v));
   v->ok = 1;
   snprintf(rl, sizeof(rl), ROOM_LABEL_WORD " %d", r->number);
   vw_clip(t, sizeof(t), join_title(r), 150);
#define JOIN_NO(short_, ...) \
   do { v->ok = 0; snprintf(v->why, sizeof(v->why), "%s", short_); \
        snprintf(v->plate, sizeof(v->plate), __VA_ARGS__); } while (0)
   if (!r->compatible)
      JOIN_NO(tr("play.different_version"), tr("play.runs_a_different_version_of_vitadhoc")
, r->nick[0] ? r->nick : rl);
   else if (r->kind == ROOM_ADAPTER && !gba)
      JOIN_NO(tr("play.a_game_boy_advance_room"), tr("play.is_a_game_boy_advance_room"), rl);
   else if (r->kind == ROOM_ADAPTER && !join_can_adapter())
      JOIN_NO(tr("play.this_game_links_by_cable"), tr("play.is_a_wireless_adapter_room_this")
, rl);
   else if (r->kind == ROOM_CABLE_GBA && !gba)
      JOIN_NO(tr("play.a_game_boy_advance_room"), tr("play.is_a_game_boy_advance_room"), rl);
   /* ANOTHER game is fine (Ruby with Sapphire, FireRed with Emerald): a
    * link cable joins any two Game Boy Advance games, the games themselves
    * decide what they do with it.  The session fetches the other game
    * (fe_gblink partner_rom); the list names both. */
   else if (r->kind == ROOM_CABLE_GBA && !join_can_cable())
      JOIN_NO(g_cable_ready ? tr("play.this_game_links_by_adapter")
                            : tr("play.restart_the_game_to_link"),
              g_cable_ready ? tr("play.is_a_link_cable_room_this")

                            : tr("play.is_a_link_cable_room_restart")
, rl);
   else if (r->kind == ROOM_CABLE_GB && gba)
      JOIN_NO(tr("play.a_game_boy_room"), tr("play.is_a_game_boy_room"), rl);
   else if (r->kind == ROOM_PAD && !pad_console())
      JOIN_NO(tr("play.a_nes_wonderswan_room"), tr("play.is_a_two_players_room_for")
, rl);
   else if (pad_console() && r->kind != ROOM_PAD)
      JOIN_NO(tr("play.not_a_two_players_room"), tr("play.is_not_a_two_players_room"), rl);
   else if (r->kind == ROOM_PAD && !join_same_pad_game(r))
      JOIN_NO(tr("play.a_different_game"), tr("play.is_playing_open_the_same_game")
, rl, t);
   else if (r->max_players > 0 && r->players >= r->max_players)
      JOIN_NO("full", tr("play.is_full_a_room_takes_players"), rl,
              r->max_players);
   else if (!r->open)
      JOIN_NO(tr("play.already_playing"), tr("play.is_in_the_middle_of_a"), rl);
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
   vw_head(internet ? g_jn.addr : NULL, tr("play.join_a_game_2"));
   {
      /* Top right: what the page is doing.  It never stops looking. */
      char st[32];
      if (n > 0)
         snprintf(st, sizeof(st), tr("play.found"), n);
      else
         snprintf(st, sizeof(st), "%s", g_jn.open ? tr("play.searching") : tr("play.waiting"));
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
         snprintf(l1, sizeof(l1), tr("play.this_vita_is_not_connected_to")
);
         snprintf(l2, sizeof(l2), tr("play.connect_in_the_vita_s_settings_2")

);
      }
      else if (!g_jn.open && g_jn.err == UI_ROOMS_E_BUSY)
      {
         snprintf(l1, sizeof(l1), tr("play.the_radio_is_busy_with_another"));
         snprintf(l2, sizeof(l2), tr("play.end_it_first_play_together_disconnect")
);
      }
      else if (!g_jn.open)
      {
         snprintf(l1, sizeof(l1), tr("play.the_search_could_not_start"));
         snprintf(l2, sizeof(l2), tr("play.trying_again_by_itself_if_this")
);
      }
      else if (g_jn.frames < JOIN_QUIET)
         snprintf(l1, sizeof(l1), internet ? tr("play.looking_for_a_game_at_that")
 : tr("play.looking_for_games_on_this_wi"));
      else if (internet)
      {
         snprintf(l1, sizeof(l1), tr("play.no_answer_from_yet"), g_jn.addr);
         snprintf(l2, sizeof(l2), tr("play.is_the_address_right_and_is")

);
         snprintf(l3, sizeof(l3), tr("play.still_trying"));
      }
      else
      {
         char ssid[40];
         snprintf(l1, sizeof(l1), tr("play.no_games_found_yet"));
         snprintf(l2, sizeof(l2), tr("play.both_consoles_on_the_same_wi")
);
         snprintf(l3, sizeof(l3), tr("play.this_vita_is_on_still_looking")
, vw_clip(ssid, sizeof(ssid), net->ssid[0]
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
         if (join_other_game(r))
         {
            /* "FireRed <-> Emerald": their game, then yours */
            char a[40], b[40];
            vw_clip(a, sizeof(a), join_title(r), 96);
            vw_clip(b, sizeof(b), own_game_name(), 96);
            snprintf(t, sizeof(t), "%s <-> %s", a, b);
         }
         else
            vw_clip(t, 64, join_title(r), 236);
         vid_text(18, y, t, v.ok ? (sel ? C_SEL : C_ITEM) : C_DIM);
         vid_text(270, y, kind_name(r->kind), v.ok ? C_ACCENT_DK : C_DIM);
         snprintf(pl, sizeof(pl), "%d/%d", r->players,
                  r->max_players > 0 ? r->max_players : 2);
         ov_text_right(452, y, pl, v.ok ? C_VALUE : C_DIM);
         /* Who, where, how quick -- or why not. */
         if (v.ok && r->rtt_ms >= 0)
            snprintf(d, sizeof(d), tr("play.nick_room_ms"),
                     r->nick[0] ? r->nick : tr("play.a_player"), r->number, r->rtt_ms);
         else if (v.ok)
            snprintf(d, sizeof(d), tr("play.nick_room"),
                     r->nick[0] ? r->nick : tr("play.a_player"), r->number);
         else
            snprintf(d, sizeof(d), "%s  -  %s", r->nick[0] ? r->nick
                     : tr("play.a_player"), v.why);
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
      footer(tr("hint.x_join_o_back"));
   else
      footer(tr("hint.o_back"));
   return UI_ACT_NONE;
}

/* ---- the host's address (Connection: Internet) ---------------------------- */

static const kp_key ADDR_KEYS[] = {
   { "1", '1', KP_CHAR, 0, 0, 1 }, { "2", '2', KP_CHAR, 1, 0, 1 },
   { "3", '3', KP_CHAR, 2, 0, 1 }, { TR_KEY("play.del"), 0, KP_DEL, 3, 0, 1 },
   { "4", '4', KP_CHAR, 0, 1, 1 }, { "5", '5', KP_CHAR, 1, 1, 1 },
   { "6", '6', KP_CHAR, 2, 1, 1 }, { ".", '.', KP_CHAR, 3, 1, 1 },
   { "7", '7', KP_CHAR, 0, 2, 1 }, { "8", '8', KP_CHAR, 1, 2, 1 },
   { "9", '9', KP_CHAR, 2, 2, 1 }, { ":", ':', KP_CHAR, 3, 2, 1 },
   { "0", '0', KP_CHAR, 0, 3, 2 }, { TR_KEY("play.join"), 0, KP_OK, 2, 3, 2 },
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
      snprintf(g_play_note, sizeof(g_play_note), tr("play.not_a_whole_address_yet_example")
);
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
   vw_head(tr("play.type_the_address_the_host_s"), tr("play.join_a_game_2"));
   vw_box(20, 66, 440, 28, g_cursor < ADDR_NKEYS);
   vid_text_hd(28, 69, g_ad.text, C_SEL);
   vw_caret(28 + vid_text_hd_w(g_ad.text) + 1, 70, 20);
   /* In the box's right end: is it an address yet. */
   ov_text_right(452, 72, ok ? tr("play.looks_right") : len ? tr("play.not_a_whole_address_yet")
                 : tr("play.like_203_0_113_9_19019"), ok ? C_VALUE : C_DIM);
   kp_draw(&pad, g_cursor < ADDR_NKEYS ? g_cursor : -1,
           ok ? 0u : 1u << ADDR_KEY_OK);
   {
      int x = 208, y = 102;
      if (nrec)
      {
         int lx = x + vid_text_w(tr("play.recent")) + FE_FONT_W;
         vid_text(x, y, tr("play.recent"), C_ACCENT);
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
         vw_wrap(x, y + 2, 250, tr("play.the_host_chooses_play_together_host")

, C_DIM, 6);
      }
   }
   if (g_play_note[0])
      vw_plate(g_play_note);
   else
      footer(tr("hint.x_type_sq_delete_tri_join")
);
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
      g_join_tgt.kind = pad_console() ? ROOM_PAD
                      : g_pcfg.console != FE_CONSOLE_GBA ? ROOM_CABLE_GB
                      : ui_host_mode() == UI_HOST_ADAPTER ? ROOM_ADAPTER
                                                          : ROOM_CABLE_GBA;
      g_join_tgt.number = room_number(g_pcfg.group);
      snprintf(g_join_group, sizeof(g_join_group), "%s", g_pcfg.group);
      fe_evt("play_join adhoc kind=%d", g_join_tgt.kind);
      screen_to(SCR_MENU);
      return UI_ACT_NET_JOIN;
   }
   ov_backdrop(OV_SCRIM_DENSE);
   vw_head(g_ov_title, tr("play.ad_hoc"));
   vid_text(22, 68, tr("play.next_the_vita_s_own_connection"), C_VALUE);
   vw_wrap(22, 90, 436, g_adhoc_host
           ? tr("play.it_links_the_two_vitas_directly")


           : tr("play.it_links_the_two_vitas_directly_2")

, C_ITEM, 4);
   vid_text(22, 150, tr("play.wi_fi_is_set_aside_while")
, C_DIM);
   vw_row(AH_GO, 182, 300, g_cursor == AH_GO, 1, tr("play.continue"), NULL);
   vw_row(AH_BACK, 182 + VW_PITCH, 300, g_cursor == AH_BACK, 1, tr("play.back"), NULL);
   footer(tr("hint.x_select_o_back"));
   return UI_ACT_NONE;
}

/* ---- two players on this Vita --------------------------------------------- */

static const char *layout_name(int l)
{
   return l == VID_PAIR_INSET ? tr("play.large_small") : tr("play.side_by_side");
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
                  tr("play.that_game_could_not_be_chosen"));
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
   vw_head(g_ov_title, tr("play.partner_s_game"));
   vw_wrap(22, 68, 436, g_lpick.n
           ? tr("play.the_second_machine_can_hold_another")

           : tr("play.no_other_game_for_this_console"),
           C_ITEM, 2);
   for (i = g_lpick.top; i < total && i < g_lpick.top + LP_ROWS; i++)
   {
      int y = 108 + (i - g_lpick.top) * VW_PITCH, sel = g_lpick.cur == i;
      const char *nm = i ? ui_host_local_rom_name(i - 1) : tr("play.this_game");
      int chosen = i ? (ls->partner_game && !strcmp(ls->partner_game, nm))
                     : !ls->partner_game;
      vw_row(i, y, 440, sel, 1, nm,
             chosen ? "chosen" : i ? NULL : tr("play.a_copy_or_a_second_save"));
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
          ? tr("hint.x_choose_l_r_page_o")
          : tr("hint.x_choose_o_back"));
}

static ui_action screen_local(unsigned edges)
{
   enum { LS_COPY, LS_OWN, LS_GAME, LS_LAYOUT, LS_TOUCH, LS_COUNT };
   enum { LA_SWAP, LA_LAYOUT, LA_STOP, LA_RESUME, LA_TOUCH, LA_COUNT };
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
      ids[n++] = LA_TOUCH;
   }
   else
   {
      ids[n++] = LS_COPY;
      ids[n++] = LS_OWN;
      ids[n++] = LS_GAME;
      ids[n++] = LS_LAYOUT;
      ids[n++] = LS_TOUCH;
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
   if ((edges & (PSP_CTRL_LEFT | PSP_CTRL_RIGHT | PSP_CTRL_CROSS)) &&
       id == (ls.active ? (int)LA_TOUCH : (int)LS_TOUCH))
   {
      g_pcfg.local_touch2 = !g_pcfg.local_touch2;
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
                  : tr("play.this_game_cannot_run_twice_on"));
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
   vw_head(g_ov_title, tr("play.two_players_2"));
   if (ls.active)
   {
      char line[120];
      if (other)
         snprintf(line, sizeof(line), tr("play.linked_by_cable_with"),
                  ls.partner_game);
      else
         snprintf(line, sizeof(line), tr("play.two_games_are_running_on_this")
);
      vw_line(22, 68, 436, line, C_VALUE);
      vid_text(22, 86, ls.have_pad2
               ? tr("play.your_buttons_your_game_second_controller")

               : ls.controlled ? tr("play.the_buttons_drive_the_partner_s")
                               : tr("play.the_buttons_drive_your_game"), C_ITEM);
      for (i = 0; i < n; i++)
      {
         int y = 108 + i * VW_PITCH, sel = g_cursor == i;
         switch (ids[i])
         {
         case LA_SWAP:
            /* two pads: nobody's buttons move, the view does */
            vw_row(i, y, 440, sel, 1, ls.have_pad2
                   ? tr("play.swap_sound_and_large_picture") : tr("play.swap_controlled_game"),
                   ls.controlled ? "now: the partner's" : "now: yours");
            break;
         case LA_LAYOUT:
            vw_row(i, y, 440, sel, 1, tr("play.layout"),
                   ls.have_pad2 || g_pcfg.local_touch2
                   ? tr("play.side_by_side_two_players")
                   : layout_name(g_pcfg.local_layout));
            break;
         case LA_TOUCH:
            vw_row(i, y, 440, sel, 1, tr("play.player_2_on_the_touch_screen"),
                   g_pcfg.local_touch2 ? (ls.have_pad2 ? tr("play.on_pad_in_use")
                                                       : "on") : "off");
            break;
         case LA_STOP:
            vw_row(i, y, 440, sel, 1, tr("play.stop"), tr("play.your_game_carries_on_alone"));
            break;
         default:
            vw_row(i, y, 440, sel, 1, tr("play.back_to_the_game"), NULL);
            break;
         }
      }
      /* (Host a game and Join a game are not on offer while this runs:
       * the page says so where the way out is) */
      vw_line(22, 240, 436, ids[g_cursor] == LA_STOP
              ? tr("play.stop_it_first_to_host_or")
              : ids[g_cursor] == LA_TOUCH
              ? tr("play.player_2_plays_on_the_front")

              : ls.have_pad2
              ? tr("play.in_the_game_right_stick_sideways")
              : tr("play.in_the_game_right_stick_sideways_2")
, C_DIM);
   }
   else
   {
      vw_wrap(22, 62, 436, tr("play.both_games_run_here_joined_by"),
              C_ITEM, 1);
      snprintf(own, sizeof(own), other ? tr("play.start_the_partner_plays_its_game")
                                       : tr("play.start_the_partner_uses_its_own")
);
      vw_row(0, 88, 440, g_cursor == 0, ls.available && !other,
             tr("play.start_the_partner_copies_my_save"),
             other ? tr("play.same_game_only") : NULL);
      vw_row(1, 88 + VW_PITCH, 440, g_cursor == 1, ls.available, own,
             ls.partner_save ? tr("play.save_found") : tr("play.starts_a_new_game"));
      vw_row(2, 88 + 2 * VW_PITCH, 440, g_cursor == 2, 1, tr("play.partner_s_game_2"),
             other ? ls.partner_game : tr("play.this_one"));
      vw_row(3, 88 + 3 * VW_PITCH, 440, g_cursor == 3, 1, tr("play.layout"),
             ls.have_pad2 || g_pcfg.local_touch2
             ? tr("play.side_by_side_two_players")
             : layout_name(g_pcfg.local_layout));
      vw_row(4, 88 + 4 * VW_PITCH, 440, g_cursor == 4, 1,
             tr("play.player_2_on_the_touch_screen"),
             g_pcfg.local_touch2 ? "on" : "off");
      vw_wrap(22, 214, 436,
              g_play_pending ? tr("play.starting_the_second_game")
              : !ls.available ? (ls.why_not && ls.why_not[0] ? ls.why_not
                 : tr("play.this_game_cannot_run_twice_on"))
              : id == LS_COPY
              ? tr("play.the_second_game_begins_as_a")

              : id == LS_OWN
              ? (other ? tr("play.the_second_machine_boots_that_game")

                       : tr("play.the_second_game_starts_from_its")
)
              : id == LS_GAME
              ? tr("play.the_same_game_twice_or_the")

              : id == LS_TOUCH
              ? tr("play.no_second_controller_player_2_plays")

              : tr("play.side_by_side_two_equal_pictures")
, C_DIM, 2);
   }
   if (g_play_note[0])
      vw_plate(g_play_note);
   else if (ls.active)
      footer(tr("hint.x_select_o_back"));
   else
      footer(id == LS_GAME ? tr("hint.x_choose_o_back")
             : id == LS_LAYOUT || id == LS_TOUCH
             ? tr("hint.x_change_o_back")
             : tr("hint.x_start_o_back"));
   return act;
}

/* ---- Connection ------------------------------------------------------------ */

static const char *delay_name(int d, char *buf, size_t cap)
{
   if (d <= 0)
      return tr("play.auto");
   snprintf(buf, cap, d == 1 ? tr("play.frame") : tr("play.frames"), d);
   return buf;
}

static ui_action screen_conn(unsigned edges)
{
   /* The connections as rows (conn_rows), then Link delay and Always use the
    * relay. */
   const char *const what[UI_CONN_COUNT] = {
      [UI_CONN_WIFI] = tr("play.same_wi_fi_or_hotspot_rooms"),
      [UI_CONN_INTERNET] = tr("play.advanced_the_joiner_types_the_address"),
      [UI_CONN_ADHOC] = tr("play.no_router_the_two_vitas_connect"),
      [UI_CONN_ONLINE] = tr("play.far_apart_host_shows_a_code"),
   };
   const ui_net_status *net = play_net(0);
   int ids[UI_CONN_COUNT], nc = conn_rows(ids), count = nc + 2;
   int cn_delay = nc, cn_relay = nc + 1;
   int pitch = nc > 3 ? 28 : 36, y0 = 70, i, y;
   char db[16];
   if (g_cursor >= count)
      g_cursor = 0;
   tch_rows(&g_cursor, &edges);
   if (edges & PSP_CTRL_UP)
      g_cursor = (g_cursor + count - 1) % count;
   if (edges & PSP_CTRL_DOWN)
      g_cursor = (g_cursor + 1) % count;
   if (g_cursor == cn_delay)
   {
      if (edges & PSP_CTRL_LEFT)
         settings_adjust(SET_DELAY, -1);
      if (edges & (PSP_CTRL_RIGHT | PSP_CTRL_CROSS))
         settings_adjust(SET_DELAY, +1);
   }
   else if (g_cursor == cn_relay)
   {
      if (edges & (PSP_CTRL_LEFT | PSP_CTRL_RIGHT | PSP_CTRL_CROSS))
      {
         g_pcfg.online_relay = !g_pcfg.online_relay;
         g_settings_dirty = 1;
         fe_evt("online_relay value=%d", g_pcfg.online_relay);
      }
   }
   else if (edges & PSP_CTRL_CROSS)
   {
      if (g_pcfg.net_conn != ids[g_cursor])
      {
         g_pcfg.net_conn = ids[g_cursor];
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
   vw_head(NULL, tr("play.connection_2"));
   play_status_draw(50, g_pcfg.net_conn, net);
   for (i = 0; i < nc; i++)
   {
      int cy = y0 + i * pitch, sel = g_cursor == i, on = g_pcfg.net_conn == ids[i];
      if (sel)
      {
         vid_rect(0, cy - 4, 460, pitch - 2, C_ACCENT, 40);
         vid_rect(0, cy - 4, 4, pitch - 2, C_ACCENT, 255);
      }
      /* The chosen one wears the console colour's dot. */
      ov_outline(18, cy + 2, 10, 10, on ? C_CON : C_DIM, on ? 255 : 160);
      if (on)
         vid_rect(21, cy + 5, 4, 4, C_CON, 255);
      vid_text(36, cy, conn_name(ids[i]), sel ? C_SEL : C_ITEM);
      vw_line(36, cy + 14, 420, what[ids[i]], sel ? C_VALUE : C_DIM);
      tch_row(i, 0, cy - 5, 460, pitch);
   }
   y = y0 + nc * pitch + 2;
   vw_row(cn_delay, y, 440, g_cursor == cn_delay, 1, tr("play.link_delay"),
          delay_name(g_pcfg.link_delay, db, sizeof(db)));
   vw_row(cn_relay, y + VW_PITCH, 440, g_cursor == cn_relay, 1,
          tr("play.always_use_the_relay"), g_pcfg.online_relay ? tr("play.on") : tr("play.off"));
   /* one line of help, for the row the cursor is on */
   y += 2 * VW_PITCH;
   if (g_cursor == cn_delay)
      vw_line(22, y, 436, g_pcfg.link_delay
              ? tr("play.fewer_frames_react_faster_more_ride")
              : tr("play.auto_picks_it_from_the_connection"),
              C_DIM);
   else if (g_cursor == cn_relay)
      vw_line(22, y, 436, tr("play.hides_your_internet_address_from_the")
, C_DIM);
   footer(g_cursor >= cn_delay
          ? tr("hint.dpad_change_o_back")
          : tr("hint.x_choose_o_back"));
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
   const char *const steps[][2] = {
      { tr("play.phone"), tr("play.mystery_gift_station_pick_a_gift") },
      { "Vita",  tr("play.settings_network_join_the_wi_fi") },
      { tr("play.here"),  tr("play.start_listening_or_select_down_in") },
      { tr("play.game"),  tr("play.mystery_gift_on_the_title_screen") },
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
   vw_head(tr("play.firered_leafgreen_and_emerald_from_a"), tr("play.mystery_gift_2"));
   ov_status_right(running ? tr("play.listening") : "OFF");
   play_status_draw(68, UI_CONN_WIFI, play_net(0));
   vw_line(22, 84, 436, l1 && l1[0] ? l1 : tr("play.not_listening"), C_VALUE);
   if (l2 && l2[0])
      vw_line(22, 84 + FE_FONT_H + 2, 436, l2, C_ITEM);
   y = 122;
   vw_row(MGV_START, y, 440, g_cursor == MGV_START, 1,
          running ? tr("play.stop_listening") : tr("play.start_listening"), NULL);
   vw_row(MGV_BACK, y + VW_PITCH, 440, g_cursor == MGV_BACK, 1, tr("play.back"), NULL);
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
      footer(tr("hint.x_back_o_back"));
   else if (running)
      footer(tr("hint.x_stop_o_back_keeps_listening"));
   else
      footer(tr("hint.x_start_o_back"));
   return UI_ACT_NONE;
}

/* ---- How to play together -------------------------------------------------- */

static ui_action screen_help(unsigned edges)
{
   /* Page 1: the steps.  Page 2: each connection, its name leading. */
   const char *const p1[][2] = {
      { "1", tr("play.both_players_open_their_game_the") },
      { "2", tr("play.one_chooses_play_together_host_a") },
      { "3", tr("play.the_other_chooses_play_together_join")
 },
      { "4", tr("play.when_it_says_linked_use_the")
 },
      { "", tr("play.to_stop_either_player_chooses_play")
 },
   };
   const char *const p2[][2] = {
      { tr("play.same_wi_fi"), tr("play.same_network_or_a_phone_s")
 },
      { tr("play.online"), tr("play.for_a_friend_far_away_the")
 },
      { tr("play.nearby"), tr("play.no_router_anywhere_the_two_vitas")

 },
      { tr("play.two_players_on_this"), tr("play.both_games_run_on_this_console")
 },
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
   vw_head(g_help_page ? tr("play.which_connection") : tr("play.the_four_steps"),
           tr("play.how_to_play"));
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
   footer(g_help_page ? tr("hint.x_the_steps_o_back")
                      : tr("hint.x_which_connection_o_back")
);
   return UI_ACT_NONE;
}

#endif /* UI_VITA_PLAY_H */
