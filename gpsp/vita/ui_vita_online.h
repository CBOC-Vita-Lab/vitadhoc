/* ui_vita_online.h -- the Online pages of the Play together menu (2.0):
 * the host's code, the code pad, the room card, the library's "find the
 * game" step, the sentences of plan section 1.6.
 *
 * Included by psp/ui_psp.c right after ui_vita_play.h, so everything here
 * sees the same statics (g_screen, g_cursor, kp_*, join_decide, ...).
 * Owner: package U.
 *
 *   SCR_ONLINE_CODE   five boxes and a touch pad of the 31 symbols a room
 *                     code uses (ABCDEFGHJKMNPQRSTUVWXYZ23456789: no I, L,
 *                     O, 0, 1).  D-pad: up/down changes the letter in the
 *                     box, left/right moves, Square deletes; on the pad X
 *                     types a key.  Triangle, Start or Join asks the service.
 *   SCR_ONLINE_CARD   what the code is: the game, who hosts, and the answer
 *                     (join, open the game and join, pick a file, or why not)
 *
 * Both pages also run as MODAL pages (a loop of their own, like Settings'
 * share screens): the game list's "Join online" (browser Settings) and
 * ui_online_code_modal().  g_oc.modal says which.
 *
 * NOTHING HERE WAITS: the room service is asked through ui_host_online_*
 * (a request in the link layer, polled every frame), the library is walked
 * eight files a frame (online_lib.h).
 */
#ifndef UI_VITA_ONLINE_H
#define UI_VITA_ONLINE_H

#include "online_vita.h"      /* ON_*, ONLINE_Q_*, ONLINE_APP_VERSION */
#include "online_lib.h"

static unsigned vita_frame_begin(unsigned *pad);   /* ui_psp.c, below */

#define OC_LEN       5
#define OC_NALPHA    31
#define OC_KEY_JOIN  31       /* (kp_draw's grey mask has 32 bits: Join is bit 31) */
#define OC_KEY_DEL   32
#define OC_NKEYS     33       /* 31 symbols, Join, Del                       */
#define OC_BOX_BASE  100      /* cursor / tch_row ids of the five boxes      */
#define OC_ROW_JOIN  0        /* the card page's cursor ids                  */

static const char OC_ALPHA[OC_NALPHA + 1] = "ABCDEFGHJKMNPQRSTUVWXYZ23456789";

/* The pad: 8 columns x 4 rows of the keypad's 42 x 36 cells (the 31 symbols
 * in order, then Del in the last cell) and the Join key across the row
 * below.  Filled once by oc_init_keys (a label is a string). */
static kp_key g_oc_keys[OC_NKEYS];
static char   g_oc_lbl[OC_NALPHA][2];
static kp_pad g_oc_pad = { g_oc_keys, OC_NKEYS, 16, 78 };

enum { OCP_LOOK = 0,   /* waiting for the service's answer                */
       OCP_BAD,        /* a sentence (the page's l1 / l2)                 */
       OCP_JOIN,       /* this game can enter: X joins                    */
       OCP_SEARCH,     /* walking the library                             */
       OCP_OPEN,       /* one file to open, then join                     */
       OCP_ASK };      /* a list of files, the player picks               */

static struct
{
   char c[OC_LEN];                /* the code typed so far, 0 = empty box   */
   int  box;                      /* the box typing goes to                 */
   int  modal;                    /* 0 the Play together page, 1 a modal
                                   * page with a lookup (the game list), 2
                                   * a modal pad that only returns a code   */
   int  leave;                    /* a modal page's way out: 1 back, 2 done */
   char code[OC_LEN + 1];         /* the code asked for                     */
   ui_online_card card;
   int  phase;
   const char *l1, *l2;           /* OCP_BAD: the sentences                 */
   char title[40];                /* the game as the page names it          */
   int  cable;
   online_lib_hit hit[ONLINE_LIB_MAX];
   int  nhit, top;
   int  frames;
   char open_path[APP_PATH_CAP];  /* ui_online_open_path()                  */
   int  pending;                  /* ui_online_take_pending()               */
   int  launch;                   /* a modal page picked a file: the game
                                   * list opens it (ui_browser)             */
   int  snes_two;                 /* ui_snes_two_take(): 1 host, 2 join     */
   char out[OC_LEN + 1];          /* ui_online_code_modal's answer          */
} g_oc;

const char *ui_online_open_path(void)
{
   return g_oc.open_path;
}

int ui_online_take_pending(void)
{
   int p = g_oc.pending;
   g_oc.pending = 0;
   return p;
}

int ui_snes_two_take(void)
{
   int p = g_oc.snes_two;
   g_oc.snes_two = 0;
   return p;
}

/* ---- the sentences of plan 1.6 ---------------------------------------------- */

/* One entry per UI_FAIL_ONLINE_*, in their order.  %s: see the table in the
 * plan (the host's version, the game's title, this build's version). */
void ui_online_fail_text(int why, const char *arg1, const char *arg2,
                         const char **l1, const char **l2)
{
   static char b1[120], b2[120];
   const char *a1 = arg1 && arg1[0] ? arg1 : "?";
   const char *a2 = arg2 && arg2[0] ? arg2 : ONLINE_APP_VERSION;
   const char *s1 = "", *s2 = "";
   b1[0] = b2[0] = '\0';
   switch (why)
   {
   case UI_FAIL_ONLINE_CODE:
      s1 = "That code is wrong or has expired.";
      s2 = "Codes last 10 minutes. Ask your friend for a new one.";
      break;
   case UI_FAIL_ONLINE_UPDATE:
      s1 = "This version of VitAdhoc is too old for online play.";
      s2 = "Install the latest version on this " UI_DEV ", then try again.";
      break;
   case UI_FAIL_ONLINE_VER_OLD_HERE:
      snprintf(b1, sizeof(b1), "Your friend has a newer VitAdhoc (%s).", a1);
      s1 = b1;
      s2 = "Update this " UI_DEV " to the same version, then join again.";
      break;
   case UI_FAIL_ONLINE_VER_OLD_THERE:
      snprintf(b1, sizeof(b1), "Your friend has an older VitAdhoc (%s).", a1);
      snprintf(b2, sizeof(b2), "Ask them to update to %s, then host again.", a2);
      s1 = b1;
      s2 = b2;
      break;
   case UI_FAIL_ONLINE_SETUP:
      s1 = "Both run the same VitAdhoc, but the Game Boy Advance setup differs.";
      s2 = "Use the same BIOS file and CPU setting on both, then try again.";
      break;
   case UI_FAIL_ONLINE_CLOCK:
      s1 = "This " UI_DEV "'s date is wrong.";
      s2 = "Set the date and time in Settings, then try again.";
      break;
   case UI_FAIL_ONLINE_GAME:
      s1 = "Your friend is playing a different game.";
      snprintf(b2, sizeof(b2), "Add %s to this " UI_DEV " to join.", a1);
      s2 = b2;
      break;
   case UI_FAIL_ONLINE_GAME_FILE:
      snprintf(b1, sizeof(b1), "Your copy of %s is a different file from "
               "your friend's.", a1);
      s1 = b1;
      s2 = "The link checks it when it starts.";
      break;
   case UI_FAIL_ONLINE_FULL:
      s1 = "Somebody already joined that room.";
      s2 = "Ask your friend to host again.";
      break;
   case UI_FAIL_ONLINE_UNREACHABLE:
      s1 = "Couldn't reach your friend.";
      s2 = "Try again, or both use Same Wi-Fi.";
      break;
   case UI_FAIL_ONLINE_BUSY:
      s1 = "Online play is busy for today.";
      s2 = "Try again tomorrow, or both use Same Wi-Fi.";
      break;
   case UI_FAIL_ONLINE_SLOW:
      s1 = "Too many tries from this network.";
      s2 = "Wait a minute and try again.";
      break;
   case UI_FAIL_ONLINE_SERVICE:
      s1 = "The online service could not be reached.";
      s2 = "Check this " UI_DEV "'s internet connection and try again.";
      break;
   case UI_FAIL_ONLINE_CONSOLE:
      s1 = "Online play is not ready for this console yet.";
      s2 = "The online service must be updated. Use Same Wi-Fi for now.";
      break;
   case UI_FAIL_ONLINE_CLOSED:
      s1 = "Your friend closed the room.";
      s2 = "Ask them to host again.";
      break;
   default:
      s1 = "The online service could not be reached.";
      s2 = "Check this " UI_DEV "'s internet connection and try again.";
      break;
   }
   if (l1) *l1 = s1;
   if (l2) *l2 = s2;
}

/* The toast a joined link raises (the link layer calls it once, when the
 * session forms): who, and how good the line is. */
void ui_online_connected_toast(const char *peer, int quality)
{
   const char *w = quality == ONLINE_Q_GREAT ? "Great"
                 : quality == ONLINE_Q_GOOD ? "Good"
                 : quality == ONLINE_Q_SLOW ? "Slow" : NULL;
   char who[24];
   vw_clip(who, sizeof(who), peer && peer[0] ? peer : "your friend", 120);
   if (w)
      osd_toast("Connected to %s. Connection: %s", who, w);
   else
      osd_toast("Connected to %s.", who);
   if (quality == ONLINE_Q_SLOW)
      osd_toast("Slow connection: the game may feel delayed.");
}

/* ---- the host's page: the code, big ----------------------------------------- */

/* The largest type the style has, centred in a box (the Home's hero face);
 * the light theme has no style layer on the overlay, so the title face. */
static void oc_big(int x, int y, int w, int h, const char *s)
{
   if (g_pcfg.theme == 1)
   {
      vid_text_hd(x + (w - vid_text_hd_w(s)) / 2, y + (h - 20) / 2, s, C_SEL);
      return;
   }
   {
      /* native pixels: twice the logical ones */
      float lh = (float)vs_line_h(VS_HERO);
      vs_text_c(VS_HERO, (float)(x + w / 2) * 2.0f,
                (float)y * 2.0f + ((float)h * 2.0f - lh) / 2.0f, s,
                VS_HEX(0xF5F7FC, 255));
   }
}

/* One row of the five boxes, centred on the screen; `chars` may hold NULs.
 * Returns the left x.  `focus` is the box to outline (-1 none). */
static void oc_boxes(int y, int bw, int bh, int gap, const char *chars,
                     int focus, int touch, int big)
{
   int total = OC_LEN * bw + (OC_LEN - 1) * gap;
   int x0 = touch ? 20 : (VID_SCR_W - total) / 2, i;
   for (i = 0; i < OC_LEN; i++)
   {
      int x = x0 + i * (bw + gap);
      char s[2];
      vw_box(x, y, bw, bh, i == focus);
      s[0] = chars[i];
      s[1] = '\0';
      if (s[0])
      {
         if (big)
            oc_big(x, y, bw, bh, s);
         else
            vid_text_hd(x + (bw - vid_text_hd_w(s)) / 2, y + (bh - 20) / 2 - 1,
                        s, C_SEL);
      }
      else if (i == focus)
         vw_caret(x + bw / 2, y + 4, bh - 8);
      if (touch)
         tch_row(OC_BOX_BASE + i, x, y, bw, bh);
   }
}

/* The waiting page while hosting Online (play_session): below the header,
 * above the two rows.  `y_top` is where the page may start. */
static void online_host_page(int y_top, const char *game)
{
   const char *code = g_link.online_code && g_link.online_code[0]
                      ? g_link.online_code : NULL;
   int st = g_link.online_stage;
   int relay = st >= ON_RELAY_CREDS && st <= ON_RELAY_WAIT;
   char line[80], who[40];
   int y = y_top;

   vw_clip(who, sizeof(who), game ? game : "", 380);
   vid_text_center(y, who, C_VALUE);
   if (!code)
   {
      /* before the code exists: what the link is doing */
      const char *why = st == ON_RESOLVE ? "Finding the service..."
                      : st == ON_STUN ? "Checking your connection..."
                      : "Asking for a code...";
      vid_text_center(y + 44, "Getting a code...", C_ITEM);
      vid_text_center(y + 64, why, C_DIM);
      vw_ring((VID_SCR_W - vid_text_w("Getting a code...")) / 2 - 14, y + 44 + 8,
              C_TITLE);
      return;
   }
   vid_text_center(y + 20, "Give this code to your friend", C_ITEM);
   {
      char five[OC_LEN];
      int i;
      for (i = 0; i < OC_LEN; i++)
         five[i] = code[i];
      oc_boxes(y + 42, 48, 56, 8, five, -1, 0, 1);
   }
   if (relay)
      vid_text_center(y + 108, "Connecting through the relay.", C_VALUE);
   else if (st >= ON_PUNCH)
      vid_text_center(y + 108, "Your friend is joining...", C_VALUE);
   else if (g_link.online_ttl_s > 0)
   {
      snprintf(line, sizeof(line), "expires in %d:%02d",
               g_link.online_ttl_s / 60, g_link.online_ttl_s % 60);
      vid_text_center(y + 108, line, C_DIM);
   }
   else
      vid_text_center(y + 108, "Waiting for your friend...", C_DIM);
}

/* ---- the code pad ------------------------------------------------------------- */

static void oc_init_keys(void)
{
   int i;
   if (g_oc_keys[0].label)
      return;
   for (i = 0; i < OC_NALPHA; i++)
   {
      kp_key *k = &g_oc_keys[i];
      g_oc_lbl[i][0] = OC_ALPHA[i];
      g_oc_lbl[i][1] = '\0';
      k->label = g_oc_lbl[i];
      k->ch = OC_ALPHA[i];
      k->act = KP_CHAR;
      k->col = (unsigned char)(i % 8);
      k->row = (unsigned char)(i / 8);
      k->span = 1;
   }
   g_oc_keys[OC_KEY_DEL] = (kp_key){ "Del", 0, KP_DEL, 7, 3, 1 };
   g_oc_keys[OC_KEY_JOIN] = (kp_key){ "Join", 0, KP_OK, 0, 4, 8 };
}

static void oc_clear_code(void)
{
   memset(g_oc.c, 0, sizeof(g_oc.c));
   g_oc.box = 0;
}

static int oc_full(void)
{
   int i;
   for (i = 0; i < OC_LEN; i++)
      if (!g_oc.c[i])
         return 0;
   return 1;
}

/* A new visit to the pad: empty boxes ("the last code typed is not
 * remembered"), the first box focused. */
static void online_code_open(void)
{
   oc_init_keys();
   oc_clear_code();
   g_oc.leave = 0;
   screen_to(SCR_ONLINE_CODE);
   g_cursor = OC_BOX_BASE;
   fe_evt("online_pad_open modal=%d", g_oc.modal);
}

static void oc_type(char ch)
{
   g_oc.c[g_oc.box] = ch;
   if (g_oc.box < OC_LEN - 1)
      g_oc.box++;
}

/* Square: the box's symbol, or when it is empty the one before it. */
static void oc_delete(void)
{
   if (g_oc.c[g_oc.box])
      g_oc.c[g_oc.box] = 0;
   else if (g_oc.box > 0)
      g_oc.c[--g_oc.box] = 0;
}

/* Up / down in a box: the next / previous symbol (an empty box starts at
 * the ends). */
static void oc_cycle(int dir)
{
   const char *at = g_oc.c[g_oc.box] ? strchr(OC_ALPHA, g_oc.c[g_oc.box])
                                     : NULL;
   int i = at ? (int)(at - OC_ALPHA) : dir > 0 ? -1 : 0;
   i = (i + dir + OC_NALPHA) % OC_NALPHA;
   g_oc.c[g_oc.box] = OC_ALPHA[i];
}

/* Where the player goes back to from a page of this file. */
static void oc_back_to_play(void)
{
   screen_to(SCR_WIRELESS);
   g_cursor = PL_JOIN;
}

static void oc_card_begin(void)
{
   memcpy(g_oc.code, g_oc.c, OC_LEN);
   g_oc.code[OC_LEN] = '\0';
   memset(&g_oc.card, 0, sizeof(g_oc.card));
   g_oc.phase = OCP_LOOK;
   g_oc.frames = 0;
   g_oc.nhit = g_oc.top = 0;
   g_oc.l1 = g_oc.l2 = "";
   screen_to(SCR_ONLINE_CARD);
   g_cursor = OC_ROW_JOIN;
   fe_evt("online_card_begin");
}

static ui_action screen_online_code(unsigned edges)
{
   unsigned dir;
   int act = -1, tapped, on_box, full;
   int row;

   oc_init_keys();
   if (!((g_cursor >= 0 && g_cursor < OC_NKEYS) ||
         (g_cursor >= OC_BOX_BASE && g_cursor < OC_BOX_BASE + OC_LEN)))
      g_cursor = OC_BOX_BASE + g_oc.box;
   row = g_cursor < OC_NKEYS ? KP_ROW_BASE + g_cursor : g_cursor;
   tapped = tch_rows(&row, &edges);
   if (tapped || g_tch.hover_row != TCH_NONE)
      g_cursor = row >= KP_ROW_BASE ? row - KP_ROW_BASE : row;
   on_box = g_cursor >= OC_BOX_BASE;
   if (on_box)
      g_oc.box = g_cursor - OC_BOX_BASE;
   /* a finger on a box only focuses it */
   if (tapped && on_box)
      edges &= ~(unsigned)PSP_CTRL_CROSS;

   dir = edges & (PSP_CTRL_UP | PSP_CTRL_DOWN | PSP_CTRL_LEFT | PSP_CTRL_RIGHT);
   dir &= ~(dir - 1);                        /* one direction at a time */
   if (edges)
      g_play_note[0] = '\0';
   if (on_box)
   {
      if (dir == PSP_CTRL_LEFT && g_oc.box > 0)
         g_oc.box--;
      else if (dir == PSP_CTRL_RIGHT && g_oc.box < OC_LEN - 1)
         g_oc.box++;
      else if (dir == PSP_CTRL_UP)
         oc_cycle(+1);
      else if (dir == PSP_CTRL_DOWN)
         oc_cycle(-1);
      if (dir)
         fe_evt("online_pad_key box=%d sym=%c", g_oc.box, g_oc.c[g_oc.box] ? g_oc.c[g_oc.box] : '-');
      g_cursor = OC_BOX_BASE + g_oc.box;
      /* X or a shoulder button: on to the pad */
      if ((edges & PSP_CTRL_CROSS) ||
          (edges & (PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER)))
         g_cursor = 0;
   }
   else
   {
      if (dir)
      {
         int to = kp_step(&g_oc_pad, g_cursor, dir);
         if (to >= 0)
            g_cursor = to;
         else if (to == KP_EDGE_U)
            g_cursor = OC_BOX_BASE + g_oc.box;
      }
      if (edges & (PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER))
         g_cursor = OC_BOX_BASE + g_oc.box;
      else if (edges & PSP_CTRL_CROSS)
      {
         act = g_oc_keys[g_cursor].act;
         if (act == KP_CHAR)
            oc_type(g_oc_keys[g_cursor].ch);
      }
   }
   if ((edges & PSP_CTRL_SQUARE) || act == KP_DEL)
      oc_delete();
   full = oc_full();
   if ((edges & (PSP_CTRL_TRIANGLE | PSP_CTRL_START)) || act == KP_OK)
   {
      if (!full)
         snprintf(g_play_note, sizeof(g_play_note), "Type all five characters, "
                  "like K7M4Q.");
      else if (g_oc.modal == 2)
      {
         memcpy(g_oc.out, g_oc.c, OC_LEN);
         g_oc.out[OC_LEN] = '\0';
         g_oc.leave = 2;
         return UI_ACT_NONE;
      }
      else
      {
         char code[OC_LEN + 1];
         int rc;
         memcpy(code, g_oc.c, OC_LEN);
         code[OC_LEN] = '\0';
         rc = ui_host_online_lookup(code);
         fe_evt("online_lookup rc=%d", rc);
         if (rc < 0)
         {
            const char *a, *b;
            ui_link_fail_text(rc == UI_ROOMS_E_NO_WIFI ? UI_FAIL_NO_WIFI
                              : rc == UI_ROOMS_E_BUSY ? UI_FAIL_RADIO_BUSY
                              : UI_FAIL_ONLINE_SERVICE, &a, &b);
            snprintf(g_play_note, sizeof(g_play_note), "%s", a);
         }
         else
         {
            oc_card_begin();
            return UI_ACT_NONE;
         }
      }
   }
   if (edges & PSP_CTRL_CIRCLE)
   {
      g_play_note[0] = '\0';
      if (g_oc.modal)
         g_oc.leave = 1;
      else
         oc_back_to_play();
      return UI_ACT_NONE;
   }

   ov_backdrop(OV_SCRIM_DENSE);
   vw_head(NULL, "JOIN ONLINE");
   oc_boxes(46, 40, 28, 8, g_oc.c, on_box ? g_oc.box : -1, 1, 0);
   {
      char n[24];
      int k, have = 0;
      for (k = 0; k < OC_LEN; k++)
         have += g_oc.c[k] ? 1 : 0;
      snprintf(n, sizeof(n), "%d of %d", have, OC_LEN);
      ov_text_right(460, 52, n, full ? C_VALUE : C_DIM);
   }
   kp_draw(&g_oc_pad, on_box ? -1 : g_cursor,
           full ? 0u : 1u << OC_KEY_JOIN);
   vw_wrap(364, 84, 100, "Your friend's screen shows five letters and "
           "numbers. There is no I, L, O, 0 or 1.", C_DIM, 8);
   if (g_play_note[0])
      vw_plate(g_play_note);
   else if (on_box)
      footer("DPAD letter, move   " VID_GLYPH_X " pad   " VID_GLYPH_SQ
             " delete   " VID_GLYPH_TRI " join   " VID_GLYPH_O " back");
   else
      footer(VID_GLYPH_X " type   " VID_GLYPH_SQ " delete   " VID_GLYPH_TRI
             " join   " VID_GLYPH_O " back");
   return UI_ACT_NONE;
}

/* ---- the room card -------------------------------------------------------------- */

/* The game as a name: the link table's for a GBA code it knows, else what
 * the host announced. */
static void oc_title_of(const ui_online_card *c, char *out, size_t cap)
{
   const char *t = !strcmp(c->console, "gba") ? link_game_title(c->game) : NULL;
   vw_clip(out, cap, t ? t : c->title[0] ? c->title : "a game", 330);
}

static void oc_bad(int why)
{
   if (why < UI_FAIL_NO_WIFI || why >= UI_FAIL_COUNT)
      why = UI_FAIL_ONLINE_SERVICE;
   ui_link_fail_text(why, &g_oc.l1, &g_oc.l2);
   g_oc.phase = OCP_BAD;
   fe_evt("online_card_bad why=%d", why);
}

/* What the room card does with this game on the Play together page: the
 * rules join_decide applies to a listed room of that kind, and for the
 * Wireless Adapter the same 4-character game code. */
static int oc_can_enter(const ui_online_card *c)
{
   ui_room r;
   join_verdict v;
   char own[5];
   if (g_oc.modal)
      return 0;                    /* from the game list: no game is running */
   if (c->kind != ROOM_ADAPTER && c->kind != ROOM_CABLE_GBA &&
       c->kind != ROOM_CABLE_GB && c->kind != ROOM_PAD)
      return 0;
   if (c->kind == ROOM_PAD)
   {
      /* Two players: the same console and the very same file (its whole
       * CRC; join_decide below looks at the code's first 4 digits). */
      char id[9];
      if (strcmp(c->console, g_pcfg.console == FE_CONSOLE_NES ? "nes" : "ws") ||
          !ui_host_pad_game(id) || strncmp(c->game, id, 8) != 0)
         return 0;
   }
   memset(&r, 0, sizeof(r));
   r.kind = c->kind;
   snprintf(r.code, sizeof(r.code), "%.4s", c->game);
   snprintf(r.title, sizeof(r.title), "%s", c->title);
   snprintf(r.nick, sizeof(r.nick), "%s", c->nick);
   r.players = 1;
   r.max_players = 2;
   r.open = 1;
   r.compatible = 1;
   join_decide(&r, &v);
   if (!v.ok)
      return 0;
   room_code_gba(g_game_code, own);
   if (c->kind == ROOM_ADAPTER && strncmp(own, r.code, 4) != 0)
      return 0;
   return 1;
}

/* The target the link layer joins (ui_join_target), from the card. */
static void oc_fill_target(void)
{
   const ui_online_card *c = &g_oc.card;
   char code[5];
   int nn = room_number(g_pcfg.group);
   memset(&g_join_tgt, 0, sizeof(g_join_tgt));
   g_join_tgt.conn = UI_CONN_ONLINE;
   g_join_tgt.listed = 0;
   g_join_tgt.kind = c->kind;
   g_join_tgt.number = nn < 0 ? 7 : nn;
   snprintf(g_join_tgt.code, sizeof(g_join_tgt.code), "%.4s", c->game);
   snprintf(g_join_tgt.title, sizeof(g_join_tgt.title), "%s", c->title);
   snprintf(g_join_tgt.nick, sizeof(g_join_tgt.nick), "%s", c->nick);
   snprintf(g_join_tgt.online_code, sizeof(g_join_tgt.online_code), "%s",
            g_oc.code);
   snprintf(code, sizeof(code), "%.4s", c->game);
   room_make(c->kind, g_join_tgt.number, strlen(code) == 4 ? code : "GAME",
             g_join_group, sizeof(g_join_group));
   g_host_cable = c->kind != ROOM_ADAPTER;
}

static void oc_card_leave(void)
{
   online_lib_end();
   ui_host_online_cancel();
}

/* The card is here: decide what the page offers. */
static void oc_found(void)
{
   const ui_online_card *c = &g_oc.card;
   oc_title_of(c, g_oc.title, sizeof(g_oc.title));
   snprintf(g_ol_last_ver, sizeof(g_ol_last_ver), "%s", c->ver);
   snprintf(g_ol_last_title, sizeof(g_ol_last_title), "%.23s", g_oc.title);
   g_oc.cable = c->kind == ROOM_CABLE_GBA || c->kind == ROOM_CABLE_GB ||
                c->kind == ROOM_PAD;
   /* the build first: only a room this build can enter has its game looked
    * at; then a full room */
   if (!c->compatible)
   {
      oc_bad(c->fail >= UI_FAIL_ONLINE_VER_OLD_HERE &&
             c->fail <= UI_FAIL_ONLINE_SETUP ? c->fail : UI_FAIL_VERSION);
      return;
   }
   if (c->full)
   {
      oc_bad(UI_FAIL_ONLINE_FULL);
      return;
   }
   if (oc_can_enter(c))
   {
      g_oc.phase = OCP_JOIN;
      return;
   }
   if (online_lib_begin(c->console, c->game, c->rom) != 0)
   {
      oc_bad(UI_FAIL_ONLINE_GAME);
      return;
   }
   g_oc.phase = OCP_SEARCH;
   fe_evt("online_lib_begin console=%s game=%s", c->console, c->game);
}

static void oc_searched(void)
{
   int pick;
   g_oc.nhit = online_lib_hits(g_oc.hit, ONLINE_LIB_MAX);
   pick = online_lib_decide(g_oc.hit, g_oc.nhit, g_oc.cable);
   fe_evt("online_lib_done hits=%d pick=%d cable=%d skipped=%d", g_oc.nhit,
          pick, g_oc.cable, online_lib_skipped());
   if (pick == ONLINE_PICK_OPEN)
      g_oc.phase = OCP_OPEN;
   else if (pick == ONLINE_PICK_ASK)
   {
      g_oc.phase = OCP_ASK;
      g_cursor = 0;
      g_oc.top = 0;
   }
   else
      oc_bad(UI_FAIL_ONLINE_GAME);
}

/* The file the player chose (or the one opened without asking): the game
 * list opens it, or the main loop does (UI_ACT_ONLINE_OPEN_JOIN). */
static ui_action oc_open_hit(int i)
{
   oc_fill_target();
   snprintf(g_oc.open_path, sizeof(g_oc.open_path), "%s", g_oc.hit[i].path);
   fe_evt("online_open_join rom_match=%d patched=%d", g_oc.hit[i].rom_match,
          g_oc.hit[i].patched);
   oc_card_leave();
   if (g_oc.modal)
   {
      g_oc.leave = 2;
      return UI_ACT_NONE;
   }
   g_oc.pending = 1;
   screen_to(SCR_MENU);
   return UI_ACT_ONLINE_OPEN_JOIN;
}

/* The console of the file a modal page chose (the game list opens it): by
 * its extension, and for a .zip by the card's console. */
static fe_console_t oc_console_of(const char *path)
{
   const char *dot = strrchr(path, '.');
   if (dot && !strcasecmp(dot, ".gba"))
      return FE_CONSOLE_GBA;
   if (dot && !strcasecmp(dot, ".gbc"))
      return FE_CONSOLE_GBC;
   if (dot && !strcasecmp(dot, ".gb"))
      return FE_CONSOLE_GB;
   if (dot && !strcasecmp(dot, ".nes"))
      return FE_CONSOLE_NES;
   if (dot && (!strcasecmp(dot, ".sfc") || !strcasecmp(dot, ".smc")))
      return FE_CONSOLE_SNES;
   if (dot && (!strcasecmp(dot, ".ws") || !strcasecmp(dot, ".wsc")))
      return FE_CONSOLE_WSWAN;
   if (dot && (!strcasecmp(dot, ".md") || !strcasecmp(dot, ".gen") || !strcasecmp(dot, ".smd") || !strcasecmp(dot, ".bin") || !strcasecmp(dot, ".68k")))
      return FE_CONSOLE_MD;
   if (dot && !strcasecmp(dot, ".sms"))
      return FE_CONSOLE_SMS;
   if (dot && !strcasecmp(dot, ".gg"))
      return FE_CONSOLE_GG;
   if (dot && (!strcasecmp(dot, ".sg") || !strcasecmp(dot, ".sc")))
      return FE_CONSOLE_SG;
   if (dot && (!strcasecmp(dot, ".cue") || !strcasecmp(dot, ".chd") || !strcasecmp(dot, ".iso")))
      return FE_CONSOLE_SCD;
   return !strcmp(g_oc.card.console, "gbc") ? FE_CONSOLE_GBC
        : !strcmp(g_oc.card.console, "gb") ? FE_CONSOLE_GB
        : !strcmp(g_oc.card.console, "nes") ? FE_CONSOLE_NES
        : !strcmp(g_oc.card.console, "snes") ? FE_CONSOLE_SNES
        : !strcmp(g_oc.card.console, "md") ? FE_CONSOLE_MD
        : !strcmp(g_oc.card.console, "sms") ? FE_CONSOLE_SMS
        : !strcmp(g_oc.card.console, "gg") ? FE_CONSOLE_GG
        : !strcmp(g_oc.card.console, "sg") ? FE_CONSOLE_SG
        : !strcmp(g_oc.card.console, "scd") ? FE_CONSOLE_SCD
        : !strcmp(g_oc.card.console, "ws") ? FE_CONSOLE_WSWAN : FE_CONSOLE_GBA;
}

/* "Pokemon Emerald (Europe).gba" -> "Pokemon Emerald"; the folder it is in. */
static void oc_hit_names(const char *path, char *name, size_t ncap, char *dir,
                         size_t dcap)
{
   const char *sl = strrchr(path, '/'), *b = sl ? sl + 1 : path, *dot;
   char stem[96], parent[96];
   size_t n;
   snprintf(stem, sizeof(stem), "%s", b);
   dot = strrchr(stem, '.');
   if (dot)
      stem[dot - stem] = '\0';
   short_game_name(stem, name, ncap);
   parent[0] = '\0';
   if (sl)
   {
      const char *e = sl, *s = sl;
      while (s > path && s[-1] != '/')
         s--;
      n = (size_t)(e - s);
      if (n >= sizeof(parent))
         n = sizeof(parent) - 1;
      memcpy(parent, s, n);
      parent[n] = '\0';
   }
   snprintf(dir, dcap, "%s", parent);
}

#define OC_LIST_ROWS 4

static ui_action screen_online_card(unsigned edges)
{
   ui_action act = UI_ACT_NONE;
   char l[120];
   int y;

   g_oc.frames++;
   /* the service's answer */
   if (g_oc.phase == OCP_LOOK)
   {
      ui_online_card c;
      memset(&c, 0, sizeof(c));
      ui_host_online_card(&c);
      c.console[sizeof(c.console) - 1] = '\0';
      c.game[sizeof(c.game) - 1] = '\0';
      c.rom[sizeof(c.rom) - 1] = '\0';
      c.title[sizeof(c.title) - 1] = '\0';
      c.nick[sizeof(c.nick) - 1] = '\0';
      c.ver[sizeof(c.ver) - 1] = '\0';
      if (c.state == 1)
      {
         g_oc.card = c;
         oc_found();
      }
      else if (c.state == 2)
      {
         g_oc.card = c;
         snprintf(g_ol_last_ver, sizeof(g_ol_last_ver), "%s", c.ver);
         oc_bad(c.fail);
      }
   }
   if (g_oc.phase == OCP_SEARCH)
   {
      if (online_lib_step(8))
         oc_searched();
   }

   /* input */
   if (g_oc.phase == OCP_ASK)
   {
      int n = g_oc.nhit, cur = g_cursor;
      if (cur < 0 || cur >= n)
         cur = 0;
      tch_rows(&cur, &edges);
      if (edges & PSP_CTRL_UP)
         cur = (cur + n - 1) % n;
      if (edges & PSP_CTRL_DOWN)
         cur = (cur + 1) % n;
      g_cursor = cur;
      if (cur < g_oc.top)
         g_oc.top = cur;
      if (cur >= g_oc.top + OC_LIST_ROWS)
         g_oc.top = cur - OC_LIST_ROWS + 1;
      if (edges & PSP_CTRL_CROSS)
         return oc_open_hit(cur);
   }
   else if (g_oc.phase == OCP_JOIN || g_oc.phase == OCP_OPEN)
   {
      int row = OC_ROW_JOIN;
      tch_rows(&row, &edges);
      g_cursor = OC_ROW_JOIN;
      if (edges & PSP_CTRL_CROSS)
      {
         if (g_oc.phase == OCP_OPEN)
            return oc_open_hit(0);
         oc_fill_target();
         fe_evt("online_join code_given=1 kind=%d", g_join_tgt.kind);
         oc_card_leave();
         screen_to(SCR_MENU);
         return UI_ACT_NET_JOIN;
      }
   }
   if (edges & PSP_CTRL_CIRCLE)
   {
      /* back to the pad, the code kept */
      oc_card_leave();
      screen_to(SCR_ONLINE_CODE);
      g_cursor = OC_BOX_BASE + (g_oc.box < OC_LEN ? g_oc.box : OC_LEN - 1);
      return UI_ACT_NONE;
   }

   /* the page */
   ov_backdrop(OV_SCRIM_DENSE);
   vw_head(NULL, "JOIN ONLINE");
   ov_text_right(460, 48, g_oc.code, C_DIM);
   if (g_oc.phase == OCP_LOOK || g_oc.phase == OCP_BAD)
   {
      if (g_oc.phase == OCP_LOOK)
      {
         vid_text(22, 70, "Looking for the room...", C_ITEM);
         vw_ring(22 + vid_text_w("Looking for the room...") + 16, 70 + 8,
                 C_TITLE);
      }
      else
      {
         vw_line(22, 70, 436, g_oc.l1, C_VALUE);
         vw_wrap(22, 90, 436, g_oc.l2, C_ITEM, 3);
         if (g_oc.card.state == 1 && g_oc.nhit == 0 && online_lib_skipped() > 0)
            vw_wrap(22, 134, 436, "A compressed .zip cannot be checked "
                    "here. Unzip the game into your games folder.", C_DIM, 2);
      }
      footer(VID_GLYPH_O " back");
      return act;
   }
   /* a room that can be entered: its game, kind and host */
   vw_line(22, 54, 436, g_oc.title, C_SEL);
   snprintf(l, sizeof(l), "%s  -  hosted by %s", kind_name(g_oc.card.kind),
            g_oc.card.nick[0] ? g_oc.card.nick : "a player");
   vw_line(22, 82, 436, l, C_VALUE);
   y = 112;
   if (g_oc.phase == OCP_JOIN)
   {
      vw_line(22, y, 436, "Your game can enter this room.", C_ITEM);
      vw_row(OC_ROW_JOIN, y + 28, 440, 1, 1, "Join", NULL);
      footer(VID_GLYPH_X " join   " VID_GLYPH_O " back");
   }
   else if (g_oc.phase == OCP_SEARCH)
   {
      vid_text(22, y, "Looking in your games...", C_ITEM);
      vw_ring(22 + vid_text_w("Looking in your games...") + 16, y + 8, C_TITLE);
      footer(VID_GLYPH_O " back");
   }
   else if (g_oc.phase == OCP_OPEN)
   {
      char t[100];
      vw_line(22, y, 436, "You have this game. It starts, then joins.", C_ITEM);
      snprintf(t, sizeof(t), "Open %s and join", g_oc.title);
      vw_row(OC_ROW_JOIN, y + 28, 440, 1, 1, vw_clip(l, sizeof(l), t, 400),
             NULL);
      footer(VID_GLYPH_X " open and join   " VID_GLYPH_O " back");
   }
   else   /* OCP_ASK */
   {
      const char *a, *b;
      char nm[64], dir[64], val[80];
      int i;
      if (g_oc.cable)
      {
         ui_online_fail_text(UI_FAIL_ONLINE_GAME_FILE, g_oc.title, NULL, &a, &b);
         vw_line(22, y - 6, 436, a, C_VALUE);
         vw_line(22, y + 12, 436, b, C_ITEM);
      }
      else
      {
         snprintf(l, sizeof(l), "You have more than one copy of %s.", g_oc.title);
         vw_line(22, y - 6, 436, l, C_VALUE);
         vw_line(22, y + 12, 436, "Pick the one to open.", C_ITEM);
      }
      for (i = g_oc.top; i < g_oc.nhit && i < g_oc.top + OC_LIST_ROWS; i++)
      {
         int ry = y + 38 + (i - g_oc.top) * VW_PITCH;
         oc_hit_names(g_oc.hit[i].path, nm, sizeof(nm), dir, sizeof(dir));
         if (g_oc.hit[i].patched)
            snprintf(val, sizeof(val), "patched  -  %s", dir);
         else
            snprintf(val, sizeof(val), "%s", dir);
         vw_row(i, ry, 440, g_cursor == i, 1, nm, val);
      }
      if (g_oc.nhit > OC_LIST_ROWS)
         ov_scrollbar(470, y + 38, OC_LIST_ROWS * VW_PITCH, g_oc.top *
                      VW_PITCH, OC_LIST_ROWS * VW_PITCH, g_oc.nhit * VW_PITCH);
      footer(VID_GLYPH_X " open and join   " VID_GLYPH_O " back");
   }
   return act;
}

/* ---- as modal pages (the game list's Join online, the share screens) ------------- */

/* One frame of a modal Online page's loop. */
static void oc_modal_frame(void)
{
   SceCtrlData pd;
   unsigned edges, pad;
   sceCtrlPeekBufferPositive(&pd, 1);
   pad = pd.Buttons;
   edges = vita_frame_begin(&pad);
   vid_overlay_begin(1);
   if (g_screen == SCR_ONLINE_CARD)
      screen_online_card(edges);
   else
      screen_online_code(edges);
   vid_overlay_end();
   osd_draw();
   demo_dump_flush();
   sceDisplayWaitVblankStart();
   vid_swap();
}

/* The code pad as a blocking page: 1 and the five characters in out when the
 * player confirmed, 0 when they backed out (or the program is quitting). */
int ui_online_code_modal(char out[6])
{
   int ok;
   g_oc.modal = 2;
   online_code_open();
   g_prev_pad = 0xFFFFFFFFu;
   while (g_running && !g_oc.leave)
      oc_modal_frame();
   ok = g_oc.leave == 2;
   if (ok && out)
      memcpy(out, g_oc.out, OC_LEN + 1);
   g_oc.modal = 0;
   g_oc.leave = 0;
   g_prev_pad = 0xFFFFFFFFu;
   return ok;
}

/* The game list's Join online (browser Settings): the pad, the card, and the
 * library's answer, as pages of a loop of their own.  1 = the player chose a
 * file to open and join: ui_online_open_path() has it, g_oc.pending is set
 * (the main loop's ui_online_take_pending), g_oc.launch tells ui_browser. */
static int online_join_modal(void)
{
   int ok;
   g_oc.modal = 1;
   g_oc.launch = 0;
   online_code_open();
   g_prev_pad = 0xFFFFFFFFu;
   while (g_running && !g_oc.leave)
      oc_modal_frame();
   ok = g_oc.leave == 2;
   g_oc.modal = 0;
   g_oc.leave = 0;
   g_prev_pad = 0xFFFFFFFFu;
   if (ok)
   {
      g_oc.pending = 1;
      g_oc.launch = 1;
   }
   return ok;
}

#endif /* UI_VITA_ONLINE_H */
