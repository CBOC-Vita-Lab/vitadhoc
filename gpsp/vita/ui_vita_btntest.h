/* ui_vita_btntest.h -- Settings > Controls > Button test.
 *
 * Included by psp/ui_psp.c under -DVITA.  One page that shows every Vita
 * input as it is now -- the twelve buttons, both sticks, the front touch
 * screen and the four corners of the rear panel -- and what each one does for
 * the current console, so a player can check a worn button or a mapping
 * without starting a game.  Inputs come from ui_host_btn_read (the hardware,
 * not the game's view of it); what they do comes from the control map
 * (g_pcfg.controls), so a change in Button mapping shows here at once.
 *
 * LEAVING.  Every button is being tested, so none of them can leave: hold
 * Start + Select for half a second (the chord that opens the menu in a game).
 * The footer says so and a bar fills while the two are held.  Nothing on this
 * page is touchable: a finger on the screen is only shown.
 */
#ifndef UI_VITA_BTNTEST_H
#define UI_VITA_BTNTEST_H

#define BT_HOLD_FRAMES 30          /* Start + Select held this long: leave   */
#define BT_Y0          58
#define BT_PITCH       15
#define BT_COL2        248

typedef struct { unsigned bit; const char *name; } bt_input;

/* The part of a game's control map that is the console's: the shoulder
 * buttons are for the GBA only; Mystery Gift too. */
static int bt_action_applies(int a, int console)
{
   if ((a == CTL_GAME_L || a == CTL_GAME_R || a == CTL_SC_MGIFT) &&
       console != FE_CONSOLE_GBA)
      return 0;
   return ctl_available(a);
}

/* What the buttons do on a WonderSwan held sideways (its game buttons are the
 * Vita's own, not the control map's; a vertical game turns them). */
static const char *bt_ws_name(unsigned bit)
{
   switch (bit)
   {
   case CTL_UP:       return "X1";
   case CTL_RIGHT:    return "X2";
   case CTL_DOWN:     return "X3";
   case CTL_LEFT:     return "X4";
   case CTL_CIRCLE:
   case CTL_TRIANGLE: return "A";
   case CTL_CROSS:
   case CTL_SQUARE:   return "B";
   case CTL_START:    return "Start";
   default:           return NULL;
   }
}

/* Words for what the input `bit` does: every action bound to it, a chord's
 * member marked.  "" when it does nothing. */
static void bt_action_text(unsigned bit, int console, char *out, size_t n)
{
   int a;
   size_t o = 0;
   out[0] = '\0';
   if (console == FE_CONSOLE_WSWAN && bt_ws_name(bit))
      o = (size_t)snprintf(out, n, "%s", bt_ws_name(bit));
   for (a = 0; a < CTL_ACTIONS; a++)
   {
      unsigned b = g_pcfg.controls.bind[a];
      const char *lbl;
      if (!(b & bit) || !bt_action_applies(a, console))
         continue;
      if (console == FE_CONSOLE_WSWAN && (a < CTL_GAME_COUNT))
         continue;            /* the WonderSwan's own layout, above */
      lbl = ctl_info(a)->label;
      if (o && o + 2 < n)
      {
         out[o++] = ',';
         out[o++] = ' ';
         out[o] = '\0';
      }
      if (o < n)
         o += (size_t)snprintf(out + o, n - o, "%s%s", lbl,
                               b != bit ? " (chord)" : "");
   }
   if (o >= n)
      out[n - 1] = '\0';
}

static void bt_row(int x, int y, int w, const char *name, int down,
                   const char *text)
{
   char t[64];
   if (down)
      vid_rect(x - 4, y - 1, w, BT_PITCH - 1, C_ACCENT_DK, 150);
   vid_text(x, y, name, down ? C_SEL : C_ITEM);
   if (text && text[0])
      ov_text_right(x + w - 10, y, vw_clip(t, sizeof(t), text, w - 14 - vid_text_w(name) - 8),
                    down ? C_SEL : C_ACCENT_DK);
}

static ui_action screen_btntest(unsigned edges)
{
   static const bt_input btn[] = {
      { CTL_UP, "UP" }, { CTL_DOWN, "DOWN" }, { CTL_LEFT, "LEFT" },
      { CTL_RIGHT, "RIGHT" }, { CTL_CROSS, VID_GLYPH_X },
      { CTL_CIRCLE, VID_GLYPH_O }, { CTL_SQUARE, VID_GLYPH_SQ },
      { CTL_TRIANGLE, VID_GLYPH_TRI }, { CTL_L, "L" }, { CTL_R, "R" },
      { CTL_SELECT, "SELECT" }, { CTL_START, "START" }
   };
   static const bt_input rear[] = {
      { CTL_REAR_TL, "REAR TOP LEFT" }, { CTL_REAR_TR, "REAR TOP RIGHT" },
      { CTL_REAR_BL, "REAR BOTTOM LEFT" }, { CTL_REAR_BR, "REAR BOTTOM RIGHT" }
   };
   ui_btn_state s;
   const int console = pcfg_display_console();
   char txt[96], num[48];
   int i, y;

   (void)edges;                 /* no button leaves this page by itself */
   memset(&s, 0, sizeof(s));
   ui_host_btn_read(&s);

   if ((s.buttons & (CTL_START | CTL_SELECT)) == (CTL_START | CTL_SELECT))
      g_bt_hold++;
   else
      g_bt_hold = 0;
   if (g_bt_hold >= BT_HOLD_FRAMES)
   {
      g_bt_hold = 0;
      screen_to(SCR_SETTINGS);
      g_cursor = SET_BTNTEST;
      g_prev_pad = 0xFFFFFFFFu;  /* the two buttons that left are spent */
      return UI_ACT_NONE;
   }

   ov_backdrop(OV_SCRIM_DENSE);
   vw_head(NULL, "BUTTON TEST");

   vid_text(14, BT_Y0 - 16, "BUTTONS", C_ACCENT);
   for (i = 0; i < (int)(sizeof(btn) / sizeof(btn[0])); i++)
   {
      bt_action_text(btn[i].bit, console, txt, sizeof(txt));
      bt_row(18, BT_Y0 + 2 + i * BT_PITCH, 218, btn[i].name,
             (s.buttons & btn[i].bit) != 0, txt);
   }

   y = BT_Y0 - 16;
   vid_text(BT_COL2, y, "STICKS AND TOUCH", C_ACCENT);
   y = BT_Y0 + 2;
   {
      /* the left stick is the d-pad's second hand; the right stick's words are its gestures */
      int moved = abs(s.lx - 128) > 40 || abs(s.ly - 128) > 40;
      snprintf(num, sizeof(num), "%d, %d", s.lx, s.ly);
      bt_row(BT_COL2, y, 218, "LEFT STICK", moved, num);
      y += BT_PITCH;
      moved = abs(s.rx - 128) > 40 || abs(s.ry - 128) > 40;
      snprintf(num, sizeof(num), "%d, %d", s.rx, s.ry);
      bt_row(BT_COL2, y, 218, "RIGHT STICK", moved, num);
      y += BT_PITCH;
      vid_text(BT_COL2 + 8, y, "Left stick: moves like the d-pad", C_DIM);
      y += BT_PITCH;
      vid_text(BT_COL2 + 8, y, "Right: left Rewind, right Fast-forward", C_DIM);
      y += BT_PITCH;
      if (s.front_n > 0)
         snprintf(num, sizeof(num), "%d, %d", s.front_x, s.front_y);
      else
         num[0] = '\0';
      bt_row(BT_COL2, y, 218, "FRONT TOUCH", s.front_n > 0,
             s.front_n > 0 ? num : (s.front_use ? s.front_use : ""));
      y += BT_PITCH;
      if (s.front_n > 0 && s.front_use)
         vid_text(BT_COL2 + 8, y, s.front_use, C_DIM);
      y += BT_PITCH;
   }
   for (i = 0; i < 4; i++)
   {
      bt_action_text(rear[i].bit, console, txt, sizeof(txt));
      if (!txt[0])
         snprintf(txt, sizeof(txt), "not set");
      bt_row(BT_COL2, y + i * BT_PITCH, 218, rear[i].name,
             (s.rear & rear[i].bit) != 0, txt);
   }

   /* the leaving bar */
   if (g_bt_hold > 0)
   {
      int w = g_bt_hold * 200 / BT_HOLD_FRAMES;
      vid_rect(140, 236, 200, 3, C_CARD, 255);
      vid_rect(140, 236, w, 3, C_ACCENT, 255);
   }
   footer("Hold Start + Select to leave");
   return UI_ACT_NONE;
}

#endif /* UI_VITA_BTNTEST_H */
