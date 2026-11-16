/* ui_vita_snes.h -- the game list's Settings > Two players, for a SNES game (2.0).
 *
 * A SNES game is played by snes.bin, another program, so the in-game menu's Play together pages are not there for it.
 * This is the way in from the list: a small page of its own (a modal loop, like the Online code pad) with
 *
 *   Host a game   Same Wi-Fi (a room is announced) or Online (the room code is shown)
 *   Join a game   Same Wi-Fi (a list of the Two players rooms of this very game) or Online (the code pad)
 *
 * NES and WonderSwan games have no such page: they run here, and their Play together menu is the way.
 *
 * Nothing is formed here.  A choice that starts something fills what the main loop needs (g_oc: the file, the role;
 * ui_join_set: the room to join) and ends the page; the list opens the game and main_vita.c's snes_link_page forms the
 * link (link_snes_begin), whose failure sentences and end toasts are the ones the other games use.
 * Nearby is not offered: the system's ad hoc connection does not outlive the change of program.
 *
 * Included by psp/ui_psp.c after ui_vita_online.h (online_join_modal, g_oc). */
#ifndef UI_VITA_SNES_H
#define UI_VITA_SNES_H

enum { S2_MAIN, S2_HOST, S2_JOIN, S2_ROOMS };

static struct
{
   int  page, cur, top;
   char path[APP_PATH_CAP];
   char title[48];
   char id[9];                 /* the game as a room names it; "" = unreadable (every Two players room is then listed) */
   char note[112];             /* the plate, until the next key */
   int  open, err, retry;      /* the room scan: begun, its error, frames to the next try */
   int  nroom;
   ui_room room[UI_ROOMS_MAX];
   unsigned sel_id;
   int  have_sel;
   int  frames;
} g_s2;

/* A room of this game that a join could be tried on: a Two players room, for this game. */
static int s2_room_is_ours(const ui_room *r)
{
   return r->kind == ROOM_PAD && (!g_s2.id[0] || !strncasecmp(r->code, g_s2.id, 4));
}

/* Why a listed room cannot be entered right now; NULL when it can. */
static const char *s2_room_why(const ui_room *r)
{
   if (!r->compatible)
      return "different version";
   if (r->max_players > 0 && r->players >= r->max_players)
      return "full";
   if (!r->open)
      return "already playing";
   return NULL;
}

static void s2_scan_begin(void)
{
   int rc = ui_host_rooms_begin(NULL);
   g_s2.open = rc == 0;
   g_s2.err = rc < 0 ? rc : 0;
   g_s2.retry = 60;
   fe_evt("snes_two_scan rc=%d", rc);
}

static void s2_scan_close(void)
{
   if (g_s2.open)
      ui_host_rooms_end();
   g_s2.open = 0;
}

/* The rooms of this game, joinable ones first, then by room number and id (an order that stays put from frame to frame). */
static void s2_scan_poll(void)
{
   ui_room tmp[UI_ROOMS_MAX];
   int n, j, k, m = 0;

   if (!g_s2.open)
   {
      g_s2.nroom = 0;
      if (--g_s2.retry <= 0)
         s2_scan_begin();
      return;
   }
   memset(tmp, 0, sizeof(tmp));
   n = ui_host_rooms_poll(tmp, UI_ROOMS_MAX);
   if (n < 0)
   {
      g_s2.err = n;
      s2_scan_close();
      g_s2.retry = 60;
      g_s2.nroom = 0;
      return;
   }
   if (n > UI_ROOMS_MAX)
      n = UI_ROOMS_MAX;
   for (j = 0; j < n; j++)
   {
      ui_room x = tmp[j];
      x.title[sizeof(x.title) - 1] = '\0';      /* nothing a hook says overruns */
      x.nick[sizeof(x.nick) - 1] = '\0';
      x.code[4] = '\0';
      x.addr[sizeof(x.addr) - 1] = '\0';
      if (!s2_room_is_ours(&x))
         continue;
      for (k = m; k > 0; k--)
      {
         const ui_room *p = &g_s2.room[k - 1];
         int pok = !s2_room_why(p), xok = !s2_room_why(&x);
         if (pok > xok || (pok == xok && (p->number < x.number || (p->number == x.number && p->id < x.id))))
            break;
         g_s2.room[k] = g_s2.room[k - 1];
      }
      g_s2.room[k] = x;
      m++;
   }
   g_s2.nroom = m;
}

/* What the main loop is told when the page has chosen: the file, and the role. */
static int s2_choose(int role)
{
   snprintf(g_oc.open_path, sizeof(g_oc.open_path), "%s", g_s2.path);
   g_oc.launch = 1;
   g_oc.snes_two = role;
   g_oc.pending = role == 2;
   fe_evt("snes_two_go role=%s conn=%d", role == 1 ? "host" : "guest", role == 1 ? g_pcfg.net_conn : ui_join_target()->conn);
   return 1;
}

static void s2_say(int why)
{
   const char *a, *b;
   ui_link_fail_text(why, &a, &b);
   snprintf(g_s2.note, sizeof(g_s2.note), "%s", a);
}

/* A row page: `n` rows with their values; returns the row X chose, -1 for O, -2 for nothing yet. */
static int s2_rows(unsigned *edges, int n)
{
   (void)tch_rows(&g_s2.cur, edges);
   if (g_s2.cur >= n)
      g_s2.cur = n - 1;
   if (*edges & PSP_CTRL_UP)
      g_s2.cur = (g_s2.cur + n - 1) % n;
   if (*edges & PSP_CTRL_DOWN)
      g_s2.cur = (g_s2.cur + 1) % n;
   if (*edges & PSP_CTRL_CIRCLE)
      return -1;
   if (*edges & PSP_CTRL_CROSS)
      return g_s2.cur;
   return -2;
}

/* One frame of the page. 0 = stay, 1 = a game is chosen (leave and start), -1 = leave (back to Settings). */
static int s2_frame(unsigned edges)
{
   const ui_net_status *net;
   int pick, i;
   char sub[64];

   if (edges)
      g_s2.note[0] = '\0';
   g_s2.frames++;
   snprintf(sub, sizeof(sub), "%s", g_s2.title);
   if (g_s2.page == S2_MAIN || g_s2.page == S2_HOST || g_s2.page == S2_JOIN)
   {
      static const char *const l_main[2] = { "Host a game", "Join a game" };
      static const char *const l_conn[2] = { "Same Wi-Fi", "Online" };
      const char *const *lab = g_s2.page == S2_MAIN ? l_main : l_conn;
      const char *v[2];

      pick = s2_rows(&edges, 2);
      if (pick == -1)
      {
         if (g_s2.page == S2_MAIN)
            return -1;
         g_s2.cur = g_s2.page == S2_HOST ? 0 : 1;      /* back to the row it came from */
         g_s2.page = S2_MAIN;
         return 0;
      }
      if (pick >= 0)
      {
         if (g_s2.page == S2_MAIN)
         {
            g_s2.page = pick == 0 ? S2_HOST : S2_JOIN;
            g_s2.cur = 0;
         }
         else
         {
            const int online = pick == 1;
            net = play_net(1);
            if (net->wifi != UI_WIFI_CONNECTED)
               s2_say(net->radio == UI_RADIO_ADHOC ? UI_FAIL_RADIO_BUSY : UI_FAIL_NO_WIFI);
            else if (online && !ui_host_online_available())
               s2_say(UI_FAIL_RADIO_BUSY);
            else if (g_s2.page == S2_HOST)
            {
               /* the link layer hosts on the Connection setting: this is that choice, made here */
               const int c = online ? UI_CONN_ONLINE : UI_CONN_WIFI;
               if (g_pcfg.net_conn != c)
               {
                  g_pcfg.net_conn = c;
                  g_settings_dirty = 1;
                  fe_evt("net_conn value=%d", g_pcfg.net_conn);
               }
               return s2_choose(1);
            }
            else if (online)
            {
               /* the code pad, the card and the library, as Settings > Join online: the game it finds is the one
                * that starts (the same file when it is this one) */
               if (online_join_modal())
               {
                  g_oc.snes_two = 0;      /* main_vita.c: pending alone says "join" */
                  return 1;
               }
               g_prev_pad = 0xFFFFFFFFu;
            }
            else
            {
               g_s2.page = S2_ROOMS;
               g_s2.cur = g_s2.top = 0;
               g_s2.have_sel = 0;
               g_s2.nroom = 0;
               g_s2.frames = 0;
               s2_scan_begin();
            }
         }
      }
      v[0] = v[1] = NULL;
      if (g_s2.page == S2_MAIN)
      {
         v[0] = "you start the room";
         v[1] = "a friend's room";
      }
      else
      {
         v[0] = g_s2.page == S2_HOST ? "a room on this Wi-Fi" : "rooms on this Wi-Fi";
         v[1] = g_s2.page == S2_HOST ? "a code to share" : "type your friend's code";
      }
      if (g_s2.page == S2_ROOMS)
         goto rooms;                 /* chosen just now: draw it this frame */
      ov_backdrop(OV_SCRIM_DENSE);
      vw_head(sub, g_s2.page == S2_MAIN ? "TWO PLAYERS" : g_s2.page == S2_HOST ? "HOST A GAME" : "JOIN A GAME");
      play_status_draw(50, g_s2.page == S2_MAIN ? g_pcfg.net_conn : g_s2.cur == 1 ? UI_CONN_ONLINE : UI_CONN_WIFI,
                       play_net(0));
      for (i = 0; i < 2; i++)
         vw_row(i, 96 + i * VW_PITCH, 440, g_s2.cur == i, 1, lab[i], v[i]);
      if (g_s2.note[0])
         vw_plate(g_s2.note);
      else
         footer(VID_GLYPH_X " select   " VID_GLYPH_O " back");
      return 0;
   }

   /* ---- S2_ROOMS: the Two players rooms of this game on this Wi-Fi ---- */
rooms:
   {
      enum { VIEW = 5, ROW_H = 34 };
      int n, cur = 0;

      s2_scan_poll();
      n = g_s2.nroom;
      if (g_s2.have_sel)
         for (i = 0; i < n; i++)
            if (g_s2.room[i].id == g_s2.sel_id)
               cur = i;
      if (cur >= n)
         cur = n ? n - 1 : 0;
      (void)tch_rows(&cur, &edges);
      if (n > 0 && (edges & PSP_CTRL_UP))
         cur = (cur + n - 1) % n;
      if (n > 0 && (edges & PSP_CTRL_DOWN))
         cur = (cur + 1) % n;
      if (n > 0)
      {
         g_s2.sel_id = g_s2.room[cur].id;
         g_s2.have_sel = 1;
      }
      if (cur < g_s2.top)
         g_s2.top = cur;
      if (cur >= g_s2.top + VIEW)
         g_s2.top = cur - VIEW + 1;
      if (edges & PSP_CTRL_CIRCLE)
      {
         s2_scan_close();
         g_s2.page = S2_JOIN;
         g_s2.cur = 0;
         return 0;
      }
      if ((edges & PSP_CTRL_CROSS) && n > 0)
      {
         const ui_room *r = &g_s2.room[cur];
         const char *why = s2_room_why(r);
         if (why)
         {
            if (!r->compatible)
               snprintf(g_s2.note, sizeof(g_s2.note), "%s runs a different version of VitAdhoc.",
                        r->nick[0] ? r->nick : "That player");
            else
               snprintf(g_s2.note, sizeof(g_s2.note), "%s is %s.", r->nick[0] ? r->nick : "That room", why);
            fe_evt("snes_two_refused id=%u why=\"%s\"", r->id, why);
         }
         else
         {
            ui_join_info t;
            memset(&t, 0, sizeof(t));
            t.conn = UI_CONN_WIFI;               /* whatever Connection says: this page is Same Wi-Fi */
            t.listed = 1;
            t.id = r->id;
            t.kind = r->kind;
            t.number = r->number;
            snprintf(t.addr, sizeof(t.addr), "%s", r->addr);
            snprintf(t.code, sizeof(t.code), "%.4s", r->code);
            snprintf(t.title, sizeof(t.title), "%s", r->title);
            snprintf(t.nick, sizeof(t.nick), "%s", r->nick);
            ui_join_set(&t);
            s2_scan_close();
            fe_evt("snes_two_join id=%u addr=%s room=%d", r->id, r->addr, r->number);
            return s2_choose(2);
         }
      }

      net = play_net(0);
      ov_backdrop(OV_SCRIM_DENSE);
      vw_head(sub, "JOIN A GAME");
      {
         char st[32];
         if (n > 0)
            snprintf(st, sizeof(st), "%d FOUND", n);
         else
            snprintf(st, sizeof(st), "%s", g_s2.open ? "SEARCHING" : "WAITING");
         ov_text_right(460, 48, st, C_DIM);
         if (g_s2.open)
            vw_ring(460 - vid_text_w(st) - 14, 48 + 8, C_TITLE);
      }
      if (n == 0)
      {
         char l1[120], l2[160];
         l1[0] = l2[0] = '\0';
         if (!g_s2.open && (g_s2.err == UI_ROOMS_E_NO_WIFI || net->wifi != UI_WIFI_CONNECTED))
         {
            snprintf(l1, sizeof(l1), "This " UI_DEV " is not connected to Wi-Fi.");
            snprintf(l2, sizeof(l2), "Connect in the " UI_DEV "'s Settings > Network. This page carries on by itself "
                     "as soon as it is.");
         }
         else if (!g_s2.open && g_s2.err == UI_ROOMS_E_BUSY)
         {
            snprintf(l1, sizeof(l1), "The radio is busy with another link.");
            snprintf(l2, sizeof(l2), "End it first (Play together > Disconnect).");
         }
         else if (!g_s2.open)
         {
            snprintf(l1, sizeof(l1), "The search could not start.");
            snprintf(l2, sizeof(l2), "Trying again by itself. If this stays, check the " UI_DEV "'s Wi-Fi.");
         }
         else if (g_s2.frames < 90)
            snprintf(l1, sizeof(l1), "Looking for games on this Wi-Fi...");
         else
         {
            snprintf(l1, sizeof(l1), "No Two players room for this game yet.");
            snprintf(l2, sizeof(l2), "The other " UI_DEV " hosts the same game (Settings > Two players), on the same "
                     "Wi-Fi. Still looking.");
         }
         vid_text(22, 76, l1, C_ITEM);
         if (l2[0])
            (void)vw_wrap(22, 76 + FE_FONT_H + 4, 436, l2, C_DIM, 3);
      }
      else
      {
         for (i = g_s2.top; i < n && i < g_s2.top + VIEW; i++)
         {
            const ui_room *r = &g_s2.room[i];
            const char *why = s2_room_why(r);
            int y = 76 + (i - g_s2.top) * ROW_H, sel = i == cur;
            char t[80], d[96], pl[12];
            if (sel)
            {
               vid_rect(0, y - 3, 460, ROW_H - 2, C_ACCENT, 40);
               vid_rect(0, y - 3, 4, ROW_H - 2, C_ACCENT, 255);
            }
            vid_text(18, y, vw_clip(t, sizeof(t), r->nick[0] ? r->nick : "a player", 236),
                     !why ? (sel ? C_SEL : C_ITEM) : C_DIM);
            snprintf(pl, sizeof(pl), "%d/%d", r->players, r->max_players > 0 ? r->max_players : 2);
            ov_text_right(452, y, pl, !why ? C_VALUE : C_DIM);
            if (!why && r->rtt_ms >= 0)
               snprintf(d, sizeof(d), ROOM_LABEL_WORD " %d  -  %d ms", r->number, r->rtt_ms);
            else if (!why)
               snprintf(d, sizeof(d), ROOM_LABEL_WORD " %d", r->number);
            else
               snprintf(d, sizeof(d), ROOM_LABEL_WORD " %d  -  %s", r->number, why);
            vid_text(18, y + 14, vw_clip(t, sizeof(t), d, 430), !why ? (sel ? C_VALUE : C_ACCENT_DK) : C_ACCENT_DK);
            tch_row(i, 0, y - 4, 460, ROW_H);
         }
         if (n > VIEW)
            ov_scrollbar(470, 72, VIEW * ROW_H, g_s2.top * ROW_H, VIEW * ROW_H, n * ROW_H);
      }
      if (g_s2.note[0])
         vw_plate(g_s2.note);
      else if (n > 0)
         footer(VID_GLYPH_X " join   " VID_GLYPH_O " back");
      else
         footer(VID_GLYPH_O " back");
   }
   return 0;
}

/* The page as a blocking loop.  1 = a game was chosen to host or join (g_oc and the join target say how: the game
 * list starts it), 0 = back to Settings (or the program is quitting).  `path` is the highlighted SNES game. */
static int snes_two_modal(const char *path, const char *title)
{
   int rc = 0;

   memset(&g_s2, 0, sizeof(g_s2));
   snprintf(g_s2.path, sizeof(g_s2.path), "%s", path);
   snprintf(g_s2.title, sizeof(g_s2.title), "%s", title);
   if (!ui_host_snes_game_id(path, g_s2.id))
      g_s2.id[0] = '\0';
   fe_evt("snes_two_open game=%s", g_s2.id[0] ? g_s2.id : "-");
   g_prev_pad = 0xFFFFFFFFu;
   while (g_running && rc == 0)
   {
      SceCtrlData pd;
      unsigned edges, pad;
      sceCtrlPeekBufferPositive(&pd, 1);
      pad = pd.Buttons;
      edges = vita_frame_begin(&pad);
      vid_overlay_begin(1);
      rc = s2_frame(edges);
      vid_overlay_end();
      osd_draw();
      demo_dump_flush();
      sceDisplayWaitVblankStart();
      vid_swap();
   }
   s2_scan_close();
   g_prev_pad = 0xFFFFFFFFu;
   return rc > 0;
}

#endif /* UI_VITA_SNES_H */
