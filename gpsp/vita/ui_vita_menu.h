/* ui_vita_menu.h -- the in-game menu as a control centre.
 *
 * Included by psp/ui_psp.c under -DVITA just before screen_menu, which
 * keeps its input rules (the cursor, X, O, the questions before leaving)
 * and hands the drawing to cc_draw().  The game stays on screen, frozen and
 * dimmed; a sheet rises from the bottom edge with the seven things the menu
 * does as cards in a row.  LEFT / RIGHT (or UP / DOWN) move along them, a
 * tap on one does it.  Above the sheet, one line says what the focused card
 * will do -- and, for the ones with a state, what that state is (how many
 * slots are used, whether a link is up, which cheats are on).
 */
#ifndef UI_VITA_MENU_H
#define UI_VITA_MENU_H

#include "ui_vita_style.h"

#define CC_CARD_W   120
#define CC_CARD_H   124
#define CC_GAP      8
#define CC_SHEET_H  262

static const struct { int icon; const char *label; } CC_CARD[M_COUNT] = {
   { VS_I_PLAY,   "Resume" },
   { VS_I_LAYERS, "Save states" },
   { VS_I_PEOPLE, "Play together" },
   { VS_I_SPARK,  "Cheats" },
   { VS_I_GEAR,   "Settings" },
   { VS_I_GRID,   "Game list" },
   { VS_I_POWER,  "Exit" },
};

/* What the focused card does, in a line. */
static void cc_says(int i, int session_active, char *out, size_t cap)
{
   ui_local_state ls;
   char st[48];
   switch (i)
   {
   case M_RESUME:
      snprintf(out, cap, "Back to the game");
      break;
   case M_SAVESTATES:
      if (session_active)
         snprintf(out, cap, "Save states are off while linked");
      else
         snprintf(out, cap, "Save or load the game: %d of %d slots used",
                  g_ov_used, PSP_STATE_SLOT_COUNT);
      break;
   case M_WIRELESS:
      memset(&ls, 0, sizeof(ls));
      ui_host_local_state(&ls);
      if (ls.active)
         snprintf(out, cap, "Two players on this " UI_DEV);
      else if (g_link.state == UI_LINK_HOSTING)
         snprintf(out, cap, "Hosting %s, waiting for the other player",
                  g_link.room);
      else if (g_link.state == UI_LINK_LINKED && g_link.player)
         snprintf(out, cap, "Linked: you are Player %d", g_link.player);
      else if (g_link.state == UI_LINK_LINKED || session_active)
         snprintf(out, cap, "Linked to another console");
      else
         snprintf(out, cap, "Link up over Wi-Fi, ad hoc or on this " UI_DEV);
      break;
   case M_CHEATS:
      if (cheats_suspended())
         snprintf(out, cap, "Cheats are off while linked");
      else
         snprintf(out, cap, "Cheats: %s", ch_summary(st, sizeof(st)));
      break;
   case M_SETTINGS:
      snprintf(out, cap, "Picture, sound, controls and more");
      break;
   case M_GAMELIST:
      snprintf(out, cap, "Leave this game for the game list");
      break;
   default:
      snprintf(out, cap, "Close VitAdhoc");
      break;
   }
}

static void cc_draw(int session_active, int fade_k)
{
   float t = (float)fade_k / (float)OV_OPEN_FRAMES, e, y0, x;
   char line[96];
   int i;

   e = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);   /* ease out */
   /* the game stays readable above the sheet: a lighter scrim than the
    * pages' */
   ov_backdrop(110 * fade_k / OV_OPEN_FRAMES);

   /* the game's name and the console, top left; the time, top right */
   {
      char title[80];
      int a = (int)(255.0f * e);
      char *par;
      snprintf(title, sizeof(title), "%s", g_ov_title);
      par = strstr(title, " (");          /* the region tag: not here */
      if (par && par != title)
         *par = '\0';
      vs_text_fit(VS_SUB, title, 640);
      vs_grad_v(0, 0, VS_W, 120, VS_HEX(0x060914, (int)(200 * e)),
                VS_HEX(0x060914, 0));
      vs_text(VS_SUB, 44, 24, title, VS_HEX(0xF5F7FC, a));
      x = vs_text(VS_CAPTION, 46, 64, skin_of(g_pcfg.console)->full,
                  VS_HEX(0xA7B0C6, a));
      if (g_link.state || session_active)
         hm_chip(x + 14, 58, g_link.state == UI_LINK_HOSTING ? "Hosting"
                                                             : "Linked",
                 VS_ALPHA(hm_tint(g_pcfg.console, 255), 90), HC_TEXT);
      hm_status(VS_W - 44, 24);
   }

   /* the sheet */
   y0 = VS_H - CC_SHEET_H * e;
   vs_shadow(20, y0, VS_W - 40, CC_SHEET_H + 40, 30, 24,
             VS_HEX(0x000000, 120));
   vs_rrect(20, y0, VS_W - 40, CC_SHEET_H + 40, 30, HC_SHEET);
   vs_rect(40, y0 + 1, VS_W - 80, 1, VS_HEX(0xFFFFFF, 26));

   cc_says(g_cursor, session_active, line, sizeof(line));
   vs_text(VS_LABEL, 52, y0 + 26, CC_CARD[g_cursor].label, HC_TEXT);
   vs_text(VS_BODY, 52 + vs_text_w(VS_LABEL, CC_CARD[g_cursor].label) + 16,
           y0 + 26, line, HC_TEXT2);

   x = (VS_W - (M_COUNT * CC_CARD_W + (M_COUNT - 1) * CC_GAP)) * 0.5f;
   for (i = 0; i < M_COUNT; i++)
   {
      int on = i == g_cursor;
      int enabled = !(i == M_SAVESTATES && session_active);
      float y = y0 + 78 - (on ? 6.0f : 0.0f);
      uint32_t ink = on ? HC_INK : enabled ? HC_TEXT : HC_TEXT3;
      if (on)
      {
         vs_shadow(x, y, CC_CARD_W, CC_CARD_H, 20, 16,
                   VS_HEX(0xFFFFFF, 50));
         vs_rrect(x, y, CC_CARD_W, CC_CARD_H, 20, HC_TEXT);
      }
      else
         vs_rrect(x, y, CC_CARD_W, CC_CARD_H, 20, HC_GLASS);
      {
         uint32_t lc = on ? HC_INK : enabled ? HC_TEXT2 : HC_TEXT3;
         const char *lb = CC_CARD[i].label, *sp = strchr(lb, ' ');
         if (vs_text_w(VS_CAPTION, lb) > CC_CARD_W - 12 && sp)
         {
            /* two lines, split at the space */
            char a[24];
            size_t n = (size_t)(sp - lb) < sizeof(a) - 1 ? (size_t)(sp - lb)
                                                         : sizeof(a) - 1;
            memcpy(a, lb, n);
            a[n] = '\0';
            vs_icon(CC_CARD[i].icon, x + CC_CARD_W * 0.5f, y + 36, 36, ink);
            vs_text_c(VS_CAPTION, x + CC_CARD_W * 0.5f, y + 64, a, lc);
            vs_text_c(VS_CAPTION, x + CC_CARD_W * 0.5f, y + 88, sp + 1, lc);
         }
         else
         {
            vs_icon(CC_CARD[i].icon, x + CC_CARD_W * 0.5f, y + 42, 38, ink);
            vs_text_c(VS_CAPTION, x + CC_CARD_W * 0.5f, y + 78, lb, lc);
         }
      }
      /* a dot where something is going on */
      if ((i == M_WIRELESS && (g_link.state || session_active)) ||
          (i == M_SAVESTATES && g_ov_used && enabled))
         vs_circle(x + CC_CARD_W - 18, y + 18, 5,
                   on ? HC_INK : hm_tint(g_pcfg.console, 255));
      VS_TCH_ROW(i, x, y, CC_CARD_W, CC_CARD_H);
      x += CC_CARD_W + CC_GAP;
   }

   if (!g_menu_ask && !g_menu_note[0])
   {
      static const int ic[2] = { VS_I_BTN_X, VS_I_BTN_O };
      static const char *const w[2] = { "Select", "Resume" };
      static const unsigned b[2] = { PSP_CTRL_CROSS, PSP_CTRL_CIRCLE };
      hm_hints_b(VS_W - 52, y0 + 222, ic, w, b, 2);
   }
}

/* The question before leaving the game, or the menu's one note: a plate
 * over the sheet's top edge.  `ask` is M_GAMELIST / M_EXIT, or 0. */
static void cc_plate(int ask, int session_active, const char *note)
{
   float y = VS_H - CC_SHEET_H - 76;
   vs_shadow(44, y, VS_W - 88, 60, 30, 18, VS_HEX(0x000000, 140));
   vs_rrect(44, y, VS_W - 88, 60, 30, VS_HEX(0x1C2646, 250));
   if (ask)
   {
      static const int ic[2] = { VS_I_BTN_X, VS_I_BTN_O };
      const char *w[2];
      static const unsigned b[2] = { PSP_CTRL_CROSS, PSP_CTRL_CIRCLE };
      w[0] = ask == M_EXIT ? "Exit" : "Quit";
      w[1] = "Stay";
      tch_modal(PSP_CTRL_CIRCLE);        /* a tap anywhere else stays */
      vs_icon(VS_I_POWER, 82, y + 30, 26, HC_TEXT2);
      vs_text(VS_LABEL, 108, y + 17,
              session_active ? "This ends the link for both players."
                             : "Progress the game has not saved is lost.",
              HC_TEXT);
      hm_hints_b(VS_W - 72, y + 18, ic, w, b, 2);
   }
   else
      vs_text(VS_LABEL, 72, y + 17, note, HC_TEXT);
}

#endif /* UI_VITA_MENU_H */
