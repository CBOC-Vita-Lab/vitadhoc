/* ui_vita_cheats.h -- the in-game CHEATS page and its "add a code" page.
 *
 * Included by psp/ui_psp.c under -DVITA.  The engine is vita/cheats_vita.h
 * (the list, the .cht file, the bundled database, pushing codes into the
 * cores); this is only its face.  A game found in the database arrives with
 * several hundred entries, all off, so the page is built for a long list:
 *
 *   - what is ON comes first (the order is taken when the page opens and
 *     kept while it is open: a row that is switched does not jump away from
 *     under the cursor);
 *   - L / R page, a held d-pad accelerates, a flick coasts, the scrollbar
 *     can be dragged, and SELECT filters by a word typed on the system
 *     keyboard;
 *   - the highlighted row shows its code and what kind of code it is.
 *
 *   X         on / off            (an unsupported code says so instead)
 *   TRIANGLE  add a code          (SCR_CHEAT_ADD: a hex keypad, checked as
 *                                  it is typed)
 *   SQUARE    delete, after a question -- the player's own entries only;
 *             the database's cannot be deleted, only left off
 *   SELECT    find / clear the filter
 *   START     search memory      (SCR_CHEAT_SEARCH, below)
 *
 * While a NETWORK link is up the engine is suspended (both consoles must
 * run identical machines): the page says so, greys everything and changes
 * nothing.  The local link does not suspend.
 */
#ifndef UI_VITA_CHEATS_H
#define UI_VITA_CHEATS_H

/* cheats_note(i) and cheats_db_match() are vita/cheats_vita.h's (psp/ui_psp.c
 * includes it before this file). */
#define CH_MATCH_TITLE CHEATS_MATCH_TITLE

#include <stdarg.h>
#include "../../playerkit/src/pk_csearch.h"

/* Five rows of 29 px (6.7 mm of the Vita's glass) where six of 24 would
 * fit: this is a list people work with a finger -- one tap a switch --
 * and 5.5 mm rows put two switches under one fingertip. */
#define CH_ROW_H     29
#define CH_VIEW_TOP  70
#define CH_ROWS      5
#define CH_VIEW_H    (CH_ROWS * CH_ROW_H)
/* A row's text sits this far below the row's top (centred, as 5 was in 24). */
#define CH_TEXT_DY   ((CH_ROW_H - FE_FONT_H) / 2 + 1)
#define CH_NAME_X    54
#define CH_NAME_W    316
/* The highlighted name's slide: the game list's dwell and speed. */
#define CH_MARQ_DWELL 80
#define CH_MARQ_SPEED 2

static struct
{
   short order[CHEATS_MAX];      /* display row -> engine index             */
   int  n;                       /* rows after the filter                   */
   int  built_for;               /* cheats_count() the order was built for  */
   int  cur;                     /* display row under the cursor            */
   int  scroll;                  /* px                                      */
   tch_scroll_t ts;
   int  free_scroll;
   int  held;                    /* frames up/down has been held            */
   char find[32];
   char note[112];
   int  ask_del;                 /* engine index awaiting the answer, -1    */
   int  marq_t;                  /* the selected name's scroll clock        */
   int  marq_row;
} g_ch;

static int ch_match(const char *name, const char *find)
{
   size_t n = strlen(find), i;
   if (!n)
      return 1;
   for (; *name; name++)
   {
      for (i = 0; i < n && name[i]; i++)
      {
         char a = name[i], b = find[i];
         if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
         if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
         if (a != b)
            break;
      }
      if (i == n)
         return 1;
   }
   return 0;
}

/* The display order: what is on, then the rest, each in the engine's own
 * order, less whatever the filter hides. */
static void ch_build(void)
{
   int total = cheats_count(), pass, i, keep = -1;
   if (g_ch.cur >= 0 && g_ch.cur < g_ch.n)
      keep = g_ch.order[g_ch.cur];
   if (total > CHEATS_MAX)
      total = CHEATS_MAX;
   g_ch.n = 0;
   for (pass = 0; pass < 2; pass++)
      for (i = 0; i < total; i++)
      {
         const cheat_entry *e = cheats_get(i);
         if (!e || (e->enabled != 0) != (pass == 0) ||
             !ch_match(e->name ? e->name : "", g_ch.find))
            continue;
         g_ch.order[g_ch.n++] = (short)i;
      }
   g_ch.built_for = total;
   g_ch.cur = 0;
   for (i = 0; i < g_ch.n; i++)       /* stay on the entry, if it is shown */
      if (g_ch.order[i] == keep)
         g_ch.cur = i;
}

static void ch_open(void)
{
   screen_to(SCR_CHEATS);
   memset(&g_ch, 0, sizeof(g_ch));
   g_ch.ask_del = -1;
   g_ch.marq_row = -1;
   ch_build();
   if (g_ch.n && cheats_db_match() == CH_MATCH_TITLE)
      snprintf(g_ch.note, sizeof(g_ch.note), tr("cheats.found_by_the_game_s_title")
);
}

/* "3 on" / "214 available" / "none": the menu row's value and the page's
 * header. */
static const char *ch_summary(char *out, size_t cap)
{
   int on = cheats_enabled_count(), n = cheats_count();
   if (on > 0)
      snprintf(out, cap, tr("cheats.on"), on);
   else if (n > 0)
      snprintf(out, cap, tr("cheats.available"), n);
   else
      snprintf(out, cap, "none");
   return out;
}

/* A code as one line: '+' between its lines becomes a visible gap. */
static void ch_code_line(char *out, size_t cap, const char *code)
{
   size_t n = 0;
   for (; code && *code && n + 6 < cap; code++)
      if (*code == '+' || *code == '\n')
      {
         memcpy(out + n, "  +  ", 5);
         n += 5;
      }
      else
         out[n++] = *code;
   out[n] = '\0';
}

static void ca_open(void);
static ui_action screen_cheat_add(unsigned edges);
static void cs_open(void);
static ui_action screen_cheat_search(unsigned edges, unsigned pad);

LARGE_WRAP(ui_action, screen_cheats, (unsigned edges, unsigned pad), (edges, pad))
static ui_action screen_cheats_page(unsigned edges, unsigned pad)
{
   const cheat_entry *sel = NULL;
   const char *typed;
   int suspended = cheats_suspended();
   int max, i, r, step;

   if (cheats_count() != g_ch.built_for)
      ch_build();                 /* the list changed under the page */
   r = ui_text_take(TXT_CHEAT_FIND, &typed);
   if (r > 0)
   {
      snprintf(g_ch.find, sizeof(g_ch.find), "%s", typed);
      g_ch.cur = -1;
      ch_build();
      g_ch.scroll = 0;
      g_ch.free_scroll = 0;
   }

   /* The delete question: X answers it, anything else keeps the entry. */
   if (g_ch.ask_del >= 0)
   {
      if (edges & PSP_CTRL_CROSS)
      {
         int rc = cheats_remove(g_ch.ask_del);
         if (rc == CHEATS_OK)
            snprintf(g_ch.note, sizeof(g_ch.note), tr("cheats.deleted"));
         else
            snprintf(g_ch.note, sizeof(g_ch.note), "%s",
                     cheats_error_text(rc));
         g_ch.ask_del = -1;
         ch_build();
      }
      else if (edges)
         g_ch.ask_del = -1;
      edges = 0;
   }
   if (edges)
      g_ch.note[0] = '\0';

   if (tch_rows(&g_ch.cur, &edges) || g_tch.hover_row != TCH_NONE)
      g_ch.free_scroll = 1;
   /* A held d-pad accelerates: one row a step for the first second, then
    * three, then a page's worth. */
   if (pad & (PSP_CTRL_UP | PSP_CTRL_DOWN))
      g_ch.held++;
   else
      g_ch.held = 0;
   step = g_ch.held < 60 ? 1 : g_ch.held < 150 ? 3 : CH_ROWS;
   if (g_ch.n > 0)
   {
      int moved = 1;
      if (edges & PSP_CTRL_UP)
         g_ch.cur = step == 1 ? (g_ch.cur + g_ch.n - 1) % g_ch.n
                  : g_ch.cur - step < 0 ? 0 : g_ch.cur - step;
      else if (edges & PSP_CTRL_DOWN)
         g_ch.cur = step == 1 ? (g_ch.cur + 1) % g_ch.n
                  : g_ch.cur + step >= g_ch.n ? g_ch.n - 1 : g_ch.cur + step;
      else if (edges & PSP_CTRL_LTRIGGER)
         g_ch.cur = g_ch.cur - CH_ROWS < 0 ? 0 : g_ch.cur - CH_ROWS;
      else if (edges & PSP_CTRL_RTRIGGER)
         g_ch.cur = g_ch.cur + CH_ROWS >= g_ch.n ? g_ch.n - 1
                                                 : g_ch.cur + CH_ROWS;
      else
         moved = 0;
      if (moved)
      {
         g_ch.free_scroll = 0;
         tch_scroll_stop(&g_ch.ts);
      }
      if (g_ch.cur >= g_ch.n) g_ch.cur = g_ch.n - 1;
      if (g_ch.cur < 0) g_ch.cur = 0;
      sel = cheats_get(g_ch.order[g_ch.cur]);
   }

   if (edges & PSP_CTRL_CIRCLE)
   {
      screen_to(SCR_MENU);
      g_cursor = M_CHEATS;
      return UI_ACT_NONE;
   }
   if (suspended && (edges & PSP_CTRL_START))
      snprintf(g_ch.note, sizeof(g_ch.note), "%s", suspended == 2
               ? tr("cheats.cheats_are_off_in_hardcore_mode")
               : tr("cheats.cheats_are_off_while_linked_to"));
   else if (edges & PSP_CTRL_START)
   {
      cs_open();
      return screen_cheat_search(0, 0);
   }
   else if (suspended && (edges & (PSP_CTRL_CROSS | PSP_CTRL_TRIANGLE |
                              PSP_CTRL_SQUARE)))
      snprintf(g_ch.note, sizeof(g_ch.note), tr("cheats.cheats_are_off_while_linked_to")
);
   else if ((edges & PSP_CTRL_CROSS) && sel)
   {
      if (!sel->usable)
      {
         /* The engine's own reason when it has one. */
         const char *why = cheats_note(g_ch.order[g_ch.cur]);
         if (why && why[0])
            snprintf(g_ch.note, sizeof(g_ch.note), "%s", why);
         else
            snprintf(g_ch.note, sizeof(g_ch.note), tr("cheats.this_code_cannot_be_run_by")
, cheats_kind_name(sel->kind));
      }
      else
      {
         int rc = cheats_set_enabled(g_ch.order[g_ch.cur], !sel->enabled);
         if (rc != CHEATS_OK)
            snprintf(g_ch.note, sizeof(g_ch.note), "%s",
                     cheats_error_text(rc));
         fe_evt("cheat_toggle i=%d on=%d rc=%d", g_ch.order[g_ch.cur],
                !sel->enabled, rc);
      }
   }
   else if (edges & PSP_CTRL_TRIANGLE)
   {
      if (cheats_count() >= CHEATS_MAX)
         snprintf(g_ch.note, sizeof(g_ch.note), "%s",
                  cheats_error_text(CHEATS_E_FULL));
      else
      {
         ca_open();
         return screen_cheat_add(0);
      }
   }
   else if ((edges & PSP_CTRL_SQUARE) && sel)
   {
      if (sel->builtin)
         snprintf(g_ch.note, sizeof(g_ch.note), tr("cheats.built_in_cheats_cannot_be_deleted")
);
      else
         g_ch.ask_del = g_ch.order[g_ch.cur];
   }
   else if (edges & PSP_CTRL_SELECT)
   {
      if (g_ch.find[0])
      {
         g_ch.find[0] = '\0';
         ch_build();
      }
      else if (ui_text_open(TXT_CHEAT_FIND, tr("cheats.find_a_cheat"), "", 24) != 0)
         snprintf(g_ch.note, sizeof(g_ch.note), tr("cheats.the_keyboard_is_not_available")
);
   }

   /* Scroll: the scrollbar under a finger, a drag, or follow the cursor. */
   max = g_ch.n * CH_ROW_H - CH_VIEW_H;
   if (max < 0)
      max = 0;
   if (g_tch.down && !g_tch.dead && g_tch.x0 >= 456 &&
       g_tch.y0 >= CH_VIEW_TOP && g_tch.y0 < CH_VIEW_TOP + CH_VIEW_H && max)
   {
      g_tch.claimed = 1;
      g_ch.scroll = (g_tch.y - CH_VIEW_TOP) * (max + CH_VIEW_H) / CH_VIEW_H -
                    CH_VIEW_H / 2;
      g_ch.free_scroll = 1;
   }
   else if (tch_scroll(&g_ch.ts, &g_ch.scroll, max, 0, CH_VIEW_TOP, 456,
                       CH_VIEW_H))
      g_ch.free_scroll = 1;
   else if (!g_ch.free_scroll)
   {
      if (g_ch.cur * CH_ROW_H < g_ch.scroll)
         g_ch.scroll = g_ch.cur * CH_ROW_H;
      if ((g_ch.cur + 1) * CH_ROW_H > g_ch.scroll + CH_VIEW_H)
         g_ch.scroll = (g_ch.cur + 1) * CH_ROW_H - CH_VIEW_H;
   }
   if (g_ch.scroll > max) g_ch.scroll = max;
   if (g_ch.scroll < 0) g_ch.scroll = 0;
   if (g_ch.free_scroll && g_ch.n > 0)
   {
      /* The cursor rides the edge of what the finger scrolled into view:
       * X never acts on a row that cannot be seen. */
      int first = (g_ch.scroll + CH_ROW_H - 1) / CH_ROW_H;
      int last = (g_ch.scroll + CH_VIEW_H) / CH_ROW_H - 1;
      if (last >= g_ch.n) last = g_ch.n - 1;
      if (g_ch.cur < first) g_ch.cur = first;
      if (g_ch.cur > last) g_ch.cur = last;
      sel = cheats_get(g_ch.order[g_ch.cur]);
   }

   ov_backdrop(OV_SCRIM_DENSE);
   {
      const char *dbn = cheats_db_game();
      char sub[100], sum[64], cnt[32];
      int on = cheats_enabled_count(), total = cheats_count();
      snprintf(sub, sizeof(sub), "%s", dbn && dbn[0] ? dbn : g_ov_title);
      clip_title(sub, 300);
      vw_head(suspended ? NULL : sub, tr("cheats.cheats"));
      if (suspended)
      {
         /* Where the game's name was, and it stays: the plate under the
          * list is cleared by the next key. */
         vid_text(22, 48, suspended == 2
                  ? tr("cheats.cheats_are_off_in_hardcore_mode")
                  : tr("cheats.cheats_are_off_while_linked_to"),
                  C_VALUE);
         ov_text_right(460, 48, "OFF", C_DIM);
      }
      else if (g_ch.find[0])
         snprintf(sum, sizeof(sum), tr("cheats.of"), g_ch.find, g_ch.n,
                  total);
      else if (total)
         snprintf(sum, sizeof(sum), tr("cheats.on_in_all"), on, total);
      else
         snprintf(sum, sizeof(sum), "%s", ch_summary(cnt, sizeof(cnt)));
      ov_text_right(460, 48, sum, on && !suspended ? C_VALUE : C_DIM);
   }

   if (g_ch.n == 0)
   {
      if (g_ch.find[0])
      {
         char t[80];
         snprintf(t, sizeof(t), tr("cheats.no_cheat_s_name_contains"),
                  g_ch.find);
         vid_text(22, 84, t, C_ITEM);
         vid_text(22, 84 + FE_FONT_H + 4, tr("cheats.select_shows_the_whole_list_again")
, C_DIM);
      }
      else
      {
         vid_text(22, 84, tr("cheats.no_cheats_for_this_game_yet"), C_ITEM);
         vw_wrap(22, 84 + FE_FONT_H + 4, 436, "The built-in list has none "
                 "for it. Add a code of your own with " VID_GLYPH_TRI
                 ", or copy a .cht file into the cheats folder beside "
                 "roms.", C_DIM, 3);
      }
   }
   else
   {
      vid_clip(0, CH_VIEW_TOP - 1, VID_SCR_W, CH_VIEW_H + 2);
      for (i = g_ch.scroll / CH_ROW_H; i < g_ch.n; i++)
      {
         const cheat_entry *e = cheats_get(g_ch.order[i]);
         int y = CH_VIEW_TOP + CH_TEXT_DY + i * CH_ROW_H - g_ch.scroll;
         int cur = i == g_ch.cur, live;
         char nm[CHEATS_NAME_CAP + 4];
         uint16_t col;
         if (y >= CH_VIEW_TOP + CH_VIEW_H)
            break;
         if (!e)
            continue;
         live = e->usable && !suspended;
         if (cur)
            ov_band(y, 456, CH_ROW_H);
         vw_switch(18, y + 3, e->enabled && e->usable, live);
         col = !live ? C_DIM : cur ? C_SEL : e->enabled ? C_VALUE : C_ITEM;
         snprintf(nm, sizeof(nm), "%s", e->name && e->name[0] ? e->name
                                                              : tr("cheats.no_name"));
         if (cur && vid_text_w(nm) > CH_NAME_W)
         {
            /* The highlighted name slides to show its end, as the game
             * list's does (same dwell and speed). */
            int over = vid_text_w(nm) - CH_NAME_W, t, off;
            if (g_ch.marq_row != i)
            {
               g_ch.marq_row = i;
               g_ch.marq_t = 0;
            }
            t = g_ch.marq_t++ % (2 * CH_MARQ_DWELL + over * CH_MARQ_SPEED);
            off = t < CH_MARQ_DWELL ? 0
                : t < CH_MARQ_DWELL + over * CH_MARQ_SPEED
                  ? (t - CH_MARQ_DWELL) / CH_MARQ_SPEED : over;
            vid_clip(CH_NAME_X, y - 2 < CH_VIEW_TOP ? CH_VIEW_TOP : y - 2,
                     CH_NAME_W, CH_ROW_H);
            vid_text(CH_NAME_X - off, y, nm, col);
            vid_clip(0, CH_VIEW_TOP - 1, VID_SCR_W, CH_VIEW_H + 2);
         }
         else
         {
            clip_title(nm, CH_NAME_W);
            vid_text(CH_NAME_X, y, nm, col);
         }
         if (!e->usable)
            ov_text_right(448, y, "unsupported", C_DIM);
         else if (!e->builtin)
            ov_text_right(448, y, "yours", cur ? C_ACCENT_DK : C_DIM);
         {
            int ty = y - CH_TEXT_DY + 1, th = CH_ROW_H;
            if (ty < CH_VIEW_TOP) { th -= CH_VIEW_TOP - ty; ty = CH_VIEW_TOP; }
            if (ty + th > CH_VIEW_TOP + CH_VIEW_H)
               th = CH_VIEW_TOP + CH_VIEW_H - ty;
            tch_row(i, 0, ty, 456, th);
         }
      }
      vid_clip_off();
      ov_scrollbar(470, CH_VIEW_TOP, CH_VIEW_H, g_ch.scroll, CH_VIEW_H,
                   g_ch.n * CH_ROW_H);
      /* The highlighted cheat's code and kind. */
      vid_rect(20, CH_VIEW_TOP + CH_VIEW_H + 3, 440, 1, C_ACCENT_DK, 120);
      if (sel)
      {
         /* Its code and kind -- or, when the engine has something to say
          * about it (an enable code, a code it cannot run), that: beside
          * the code when both fit, instead of the kind. */
         char code[160], nb[120];
         const char *kn = cheats_kind_name(sel->kind);
         const char *note = cheats_note(g_ch.order[g_ch.cur]);
         const int y = CH_VIEW_TOP + CH_VIEW_H + 8;
         ch_code_line(code, sizeof(code), sel->code);
         if (note && note[0] && !sel->usable)
            vw_line(22, y, 436, note, C_ITEM);
         else if (note && note[0])
         {
            clip_title(code, 150);
            vid_text(22, y, code, C_ITEM);
            ov_text_right(458, y, vw_clip(nb, sizeof(nb), note,
                          436 - vid_text_w(code) - 16), C_ACCENT_DK);
         }
         else
         {
            clip_title(code, 436 - vid_text_w(kn) - 16);
            vid_text(22, y, code, C_ITEM);
            ov_text_right(458, y, kn, C_DIM);
         }
      }
   }
   if (g_ch.ask_del >= 0)
   {
      const cheat_entry *e = cheats_get(g_ch.ask_del);
      char q[112], nm[40];
      vw_clip(nm, sizeof(nm), e && e->name ? e->name : tr("cheats.this_code"), 170);
      snprintf(q, sizeof(q), tr("cheats.delete_x_delete_o_keep")
, nm);
      tch_modal(PSP_CTRL_CIRCLE);
      ov_plate(q);
   }
   else if (g_ch.note[0])
      vw_plate(g_ch.note);
   else if (suspended)
      footer(tr("hint.l_r_page_o_back"));   /* START search is refused */
   else if (sel && !sel->builtin)
      footer(tr("hint.x_on_off_tri_add_sq")
);
   else if (sel)
      footer(tr("hint.x_on_off_tri_add_select")
);
   else
      footer(tr("hint.tri_add_a_code_select_find")
);
   return UI_ACT_NONE;
}

/* ---- add a code ----------------------------------------------------------
 * A code is hexadecimal digits in groups, one group per line of the code:
 * sixteen digit keys, a blank, a new line and the dash a Game Boy Game
 * Genie code carries.  cheats_validate reads what is there after every
 * key, so the page says what the engine will make of it BEFORE it is
 * saved. */

#define CA_CODE_CAP 400

/* Six columns: the sixteen digits in the order a hex pad has them, the
 * three editing keys down the right, and the bottom row's blank and Save.
 * The NAME is not a key: it is the field on the right (tap it, or RIGHT
 * from the last column, or SELECT) -- the system keyboard types it. */
static const kp_key CA_KEYS[] = {
   { "1", '1', KP_CHAR, 0, 0, 1 }, { "2", '2', KP_CHAR, 1, 0, 1 },
   { "3", '3', KP_CHAR, 2, 0, 1 }, { "A", 'A', KP_CHAR, 3, 0, 1 },
   { "B", 'B', KP_CHAR, 4, 0, 1 }, { TR_KEY("cheats.del"), 0, KP_DEL, 5, 0, 1 },
   { "4", '4', KP_CHAR, 0, 1, 1 }, { "5", '5', KP_CHAR, 1, 1, 1 },
   { "6", '6', KP_CHAR, 2, 1, 1 }, { "C", 'C', KP_CHAR, 3, 1, 1 },
   { "D", 'D', KP_CHAR, 4, 1, 1 }, { TR_KEY("cheats.line"), '\n', KP_CHAR, 5, 1, 1 },
   { "7", '7', KP_CHAR, 0, 2, 1 }, { "8", '8', KP_CHAR, 1, 2, 1 },
   { "9", '9', KP_CHAR, 2, 2, 1 }, { "E", 'E', KP_CHAR, 3, 2, 1 },
   { "F", 'F', KP_CHAR, 4, 2, 1 }, { "-", '-', KP_CHAR, 5, 2, 1 },
   { "0", '0', KP_CHAR, 0, 3, 1 }, { TR_KEY("cheats.space"), ' ', KP_CHAR, 1, 3, 2 },
   { TR_KEY("cheats.save"), 0, KP_OK, 3, 3, 3 },
};
#define CA_NKEYS  ((int)(sizeof(CA_KEYS) / sizeof(CA_KEYS[0])))
/* The keys as this console has them: the dash key types the colon of a NES
 * or WonderSwan RAM code ("AAAA:VV", "AAAAA:VV") there (ca_open).  Letters
 * (a NES Game Genie code) and anything else come from the system keyboard:
 * L opens it with the code typed so far. */
static kp_key g_ca_keys[sizeof(CA_KEYS) / sizeof(CA_KEYS[0])];
#define CA_KEY_DASH 17
#define CA_KEY_OK   (CA_NKEYS - 1)
#define CA_NAME_ROW 100          /* the cursor on the NAME field */

static struct
{
   char code[CA_CODE_CAP];       /* as typed: digits, blanks, '\n'          */
   int  len;
   char name[CHEATS_NAME_CAP];
   int  verdict;                 /* cheats_validate of what is there        */
   int  ask_discard;
   char note[112];
} g_ca;

static void ca_validate(void)
{
   char t[CA_CODE_CAP];
   int n = g_ca.len;
   while (n && (g_ca.code[n - 1] == ' ' || g_ca.code[n - 1] == '\n'))
      n--;
   memcpy(t, g_ca.code, (size_t)n);
   t[n] = '\0';
   g_ca.verdict = n ? cheats_validate(t, (fe_console_t)g_pcfg.console)
                    : CHEATS_E_FORMAT;
}

static void ca_open(void)
{
   screen_to(SCR_CHEAT_ADD);
   memset(&g_ca, 0, sizeof(g_ca));
   memcpy(g_ca_keys, CA_KEYS, sizeof(g_ca_keys));
   if (g_pcfg.console == FE_CONSOLE_NES || g_pcfg.console == FE_CONSOLE_WSWAN)
   {
      g_ca_keys[CA_KEY_DASH].label = ":";
      g_ca_keys[CA_KEY_DASH].ch = ':';
   }
   ca_validate();
}

/* One character per CA_CELL px, so a code's columns line up as they do on
 * the sheet it is copied from. */
#define CA_CELL 10
static void ca_mono(int x, int y, const char *s, int n, uint16_t col)
{
   char c[2] = { 0, 0 };
   int i;
   for (i = 0; i < n; i++)
   {
      c[0] = s[i];
      if (c[0] != ' ')
         vid_text(x + i * CA_CELL + (CA_CELL - vid_text_w(c)) / 2, y, c, col);
   }
}

LARGE_WRAP(ui_action, screen_cheat_add, (unsigned edges), (edges))
static ui_action screen_cheat_add_page(unsigned edges)
{
   static const kp_pad pad = { g_ca_keys, CA_NKEYS, 20, 68 };
   int gb = g_pcfg.console != FE_CONSOLE_GBA;
   int act = -1, r;
   const char *typed;
   char ch = 0;

   r = ui_text_take(TXT_CHEAT_NAME, &typed);
   if (r > 0)
      snprintf(g_ca.name, sizeof(g_ca.name), "%s", typed);
   r = ui_text_take(TXT_CHEAT_CODE, &typed);
   if (r > 0)
   {
      /* the code from the system keyboard: capitals, '+' or ',' are lines */
      int i, n = 0;
      for (i = 0; typed[i] && n < CA_CODE_CAP - 1; i++)
      {
         char c = typed[i];
         if (c >= 'a' && c <= 'z')
            c = (char)(c - 32);
         if (c == '+' || c == ',' || c == ';')
            c = '\n';
         if ((c == ' ' || c == '\n') && (!n || g_ca.code[n - 1] == ' ' ||
                                          g_ca.code[n - 1] == '\n'))
            continue;
         g_ca.code[n++] = c;
      }
      g_ca.code[n] = '\0';
      g_ca.len = n;
      ca_validate();
      fe_evt("cheat_code_typed len=%d verdict=%d", n, g_ca.verdict);
   }
   if ((edges & PSP_CTRL_LTRIGGER) && !g_ca.ask_discard)
   {
      char now[CA_CODE_CAP];
      int i;
      for (i = 0; i < g_ca.len; i++)
         now[i] = g_ca.code[i] == '\n' ? '+' : g_ca.code[i];
      now[g_ca.len] = '\0';
      if (ui_text_open(TXT_CHEAT_CODE, tr("cheats.cheat_code"), now, CA_CODE_CAP - 1) != 0)
         snprintf(g_ca.note, sizeof(g_ca.note), tr("cheats.no_keyboard_use_the_keys"));
   }

   if (g_ca.ask_discard)
   {
      if (edges & PSP_CTRL_CROSS)
      {
         ch_open();
         return screen_cheats(0, 0);
      }
      if (edges)
         g_ca.ask_discard = 0;
      edges = 0;
   }
   if ((g_cursor < 0 || g_cursor >= CA_NKEYS) && g_cursor != CA_NAME_ROW)
      g_cursor = 0;
   {
      int row = g_cursor == CA_NAME_ROW ? CA_NAME_ROW
                                        : KP_ROW_BASE + g_cursor;
      if (tch_rows(&row, &edges) || g_tch.hover_row != TCH_NONE)
      {
         if (row >= KP_ROW_BASE && row < KP_ROW_BASE + CA_NKEYS)
            g_cursor = row - KP_ROW_BASE;
         else if (row == CA_NAME_ROW)
            g_cursor = CA_NAME_ROW;
      }
   }
   if (edges)
      g_ca.note[0] = '\0';
   if (edges & (PSP_CTRL_UP | PSP_CTRL_DOWN | PSP_CTRL_LEFT | PSP_CTRL_RIGHT))
   {
      unsigned e = edges & (PSP_CTRL_UP | PSP_CTRL_DOWN | PSP_CTRL_LEFT |
                            PSP_CTRL_RIGHT);
      int to;
      e &= ~(e - 1);
      if (g_cursor == CA_NAME_ROW)
      {
         if (e == PSP_CTRL_LEFT || e == PSP_CTRL_DOWN)
            g_cursor = 5;                     /* Del: the nearest key */
      }
      else
      {
         to = kp_step(&pad, g_cursor, e);
         if (to >= 0)
            g_cursor = to;
         else if (to == KP_EDGE_R)
            g_cursor = CA_NAME_ROW;
      }
   }
   if ((edges & PSP_CTRL_CROSS) && g_cursor != CA_NAME_ROW)
   {
      act = g_ca_keys[g_cursor].act;
      ch = g_ca_keys[g_cursor].ch;
      if (act == KP_CHAR && ch == '-' && !gb)
      {
         snprintf(g_ca.note, sizeof(g_ca.note), tr("cheats.the_dash_belongs_to_game_boy")
);
         act = -1;
      }
   }
   if (act == KP_CHAR)
   {
      /* No blank or new line at the very start, and never two in a row. */
      char last = g_ca.len ? g_ca.code[g_ca.len - 1] : '\n';
      if ((ch == ' ' || ch == '\n') && (last == ' ' || last == '\n'))
      {
         if (ch == '\n' && last == ' ' && g_ca.len)
            g_ca.code[g_ca.len - 1] = '\n';
      }
      else if (g_ca.len < CA_CODE_CAP - 1)
      {
         g_ca.code[g_ca.len++] = ch;
         g_ca.code[g_ca.len] = '\0';
      }
      else
         snprintf(g_ca.note, sizeof(g_ca.note), "%s",
                  cheats_error_text(CHEATS_E_TOO_LONG));
      ca_validate();
   }
   if ((edges & PSP_CTRL_SQUARE) || act == KP_DEL)
   {
      if (g_ca.len)
         g_ca.code[--g_ca.len] = '\0';
      ca_validate();
   }
   if ((edges & PSP_CTRL_SELECT) ||
       ((edges & PSP_CTRL_CROSS) && g_cursor == CA_NAME_ROW))
   {
      if (ui_text_open(TXT_CHEAT_NAME, tr("cheats.cheat_name"), g_ca.name, 40) != 0)
         snprintf(g_ca.note, sizeof(g_ca.note), tr("cheats.no_keyboard_the_cheat_gets_a")
);
   }
   if ((edges & (PSP_CTRL_TRIANGLE | PSP_CTRL_START)) || act == KP_OK)
   {
      if (g_ca.verdict > 0)
      {
         char code[CA_CODE_CAP];
         int n = g_ca.len, i, rc;
         while (n && (g_ca.code[n - 1] == ' ' || g_ca.code[n - 1] == '\n'))
            n--;
         for (i = 0; i < n; i++)       /* the .cht joins lines with '+' */
            code[i] = g_ca.code[i] == '\n' ? '+' : g_ca.code[i];
         code[n] = '\0';
         rc = cheats_add(g_ca.name[0] ? g_ca.name : NULL, code);
         fe_evt("cheat_add rc=%d len=%d", rc, n);
         if (rc >= 0)
         {
            ch_open();
            for (i = 0; i < g_ch.n; i++)
               if (g_ch.order[i] == rc)
                  g_ch.cur = i;
            snprintf(g_ch.note, sizeof(g_ch.note), tr("cheats.added_and_switched_on"));
            return screen_cheats(0, 0);
         }
         snprintf(g_ca.note, sizeof(g_ca.note), "%s", cheats_error_text(rc));
      }
      else
         snprintf(g_ca.note, sizeof(g_ca.note), "%s", !g_ca.len
                  ? tr("cheats.type_the_code_first")
                  : cheats_error_text(g_ca.verdict));
   }
   if (edges & PSP_CTRL_CIRCLE)
   {
      if (g_ca.len)
         g_ca.ask_discard = 1;
      else
      {
         ch_open();
         return screen_cheats(0, 0);
      }
   }

   ov_backdrop(OV_SCRIM_DENSE);
   {
      char sub[120];
      snprintf(sub, sizeof(sub), tr("cheats.codes"), pcfg_console_tag(g_pcfg.console),
               cheats_format_hint((fe_console_t)g_pcfg.console));
      clip_title(sub, 436);
      vw_head(sub, tr("cheats.add_a_cheat"));
   }
   kp_draw(&pad, g_cursor == CA_NAME_ROW ? -1 : g_cursor,
           (g_ca.verdict > 0 ? 0u : 1u << CA_KEY_OK) |
           (gb ? 0u : 1u << CA_KEY_DASH));
   {
      /* Right: the name, then the code as lines, the newest at the bottom
       * (six fit; older lines scroll off the top). */
      const int x = 284, bx = 280, bw = 180, by = 104, bh = 100;
      const char *line[64];
      int len[64], nl = 0, i, first;
      char nm[CHEATS_NAME_CAP];
      const char *p = g_ca.code;
      if (g_cursor == CA_NAME_ROW)
      {
         vid_rect(bx - 2, 66, bw + 2, 36, C_ACCENT, 40);
         vid_rect(bx - 2, 66, 3, 36, C_ACCENT, 255);
      }
      vid_text(x, 68, "NAME", C_ACCENT);
      if (g_ca.name[0])
         vid_text(x, 84, vw_clip(nm, sizeof(nm), g_ca.name, 172), C_VALUE);
      else
         vid_text(x, 84, vw_clip(nm, sizeof(nm), tr("cheats.a_number_unless_named"), 172), C_DIM);
      tch_row(CA_NAME_ROW, bx - 2, 64, bw + 2, 40);
      vw_box(bx, by, bw, bh, g_cursor != CA_NAME_ROW);
      line[0] = p;
      len[0] = 0;
      for (; *p; p++)
         if (*p == '\n' && nl < 63)
         {
            nl++;
            line[nl] = p + 1;
            len[nl] = 0;
         }
         else
            len[nl]++;
      first = nl - 5 > 0 ? nl - 5 : 0;
      for (i = first; i <= nl; i++)
      {
         int ly = by + 4 + (i - first) * 15;
         /* A line too long for the box shows its tail: what is typed. */
         int skip = len[i] > 17 ? len[i] - 17 : 0;
         ca_mono(bx + 5, ly, line[i] + skip, len[i] - skip, C_SEL);
         if (i == nl)
            vw_caret(bx + 5 + (len[i] - skip) * CA_CELL + 1, ly + 1, 14);
      }
      if (!g_ca.len)
         vid_text(bx + 12, by + 4, tr("cheats.the_code"), C_DIM);
      /* What the engine makes of it. */
      if (g_ca.verdict > 0)
      {
         char v[64];
         snprintf(v, sizeof(v), nl ? tr("cheats.line_many") : tr("cheats.line_one"),
                  cheats_kind_name(g_ca.verdict), nl + 1);
         vid_text(x, by + bh + 6, vw_clip(nm, sizeof(nm), v, 176), C_VALUE);
         vid_text(x, by + bh + 6 + FE_FONT_H, tr("cheats.ready_to_save"), C_DIM);
      }
      else if (g_ca.len)
      {
         vid_text(x, by + bh + 6, g_ca.verdict == CHEATS_E_UNSUPPORTED
                  ? tr("cheats.recognised_not_supported") : tr("cheats.not_a_whole_code_yet"),
                  C_ITEM);
      }
   }
   if (g_ca.ask_discard)
   {
      tch_modal(PSP_CTRL_CIRCLE);
      ov_plate(tr("cheats.throw_this_code_away_x_discard")
);
   }
   else if (g_ca.note[0])
      vw_plate(g_ca.note);
   else
      footer(g_cursor == CA_NAME_ROW
             ? tr("hint.x_name_it_tri_save_o")

             : tr("hint.x_type_sq_delete_l_keyboard")
);
   return UI_ACT_NONE;
}

/* ---- search memory ---------------------------------------------------------
 * RetroArch's and mGBA's cheat search, the same words on every console
 * (playerkit/src/pk_csearch.h is the engine, shared with the two players):
 *
 *   Size of the number  1 / 2 / 4 bytes
 *   New search          remember the memory now
 *   then, to narrow the list:  Equal to / Not equal to / Greater than / Less
 *   than a number you type, or Changed / Unchanged / Increased / Decreased
 *   since the last step.  With 50 results or fewer they are listed with
 *   their value; X on one makes it a cheat that keeps that value.
 *
 * The game is paused while this page is up, so a step costs no frame; the
 * search is kept when the menu closes (play, lose a life, come back and
 * choose Decreased) and freed when the game ends or a new search starts.
 * It searches the work RAM (what a typed code can reach), through the
 * RetroAchievements memory map (fe_host_ra_read). */

static struct
{
   pk_cs cs;
   int  ready;                   /* the machine was set up for this game    */
   int  size;                    /* pk_cs_size_choice: 1, 2, 4 bytes, then 2 and 4 at any address */
   int  cur, scroll;
   tch_scroll_t ts;
   int  free_scroll;
   int  op;                      /* the step waiting for its number, -1     */
   int  nres;                    /* results listed (<= PK_CS_LIST_MAX)      */
   uint32_t addr[PK_CS_LIST_MAX], val[PK_CS_LIST_MAX];
   char note[112];
} g_cs;

#define CS_ROW_H    26
#define CS_VIEW_TOP 66
#define CS_ROWS     6
#define CS_VIEW_H   (CS_ROWS * CS_ROW_H)
enum { CSR_SIZE, CSR_NEW, CSR_RESULTS, CSR_OP, CSR_HIT };

static uint32_t cs_read(void *ud, uint32_t addr, uint8_t *buf, uint32_t n)
{
   (void)ud;
   return fe_host_ra_read(addr, buf, n);
}

/* The work RAM of the console running, as a typed code reaches it.  0 = this
 * console has nothing to search here. */
static int cs_setup(void)
{
   static const pk_cs_region gba[] = { { 0, 0x48000 } };      /* IWRAM, EWRAM  */
   static const pk_cs_region gb[] = { { 0xC000, 0x2000 }, { 0xFF80, 0x7F } };
   static const pk_cs_region nes[] = { { 0, 0x800 } };
   static const pk_cs_region ws[] = { { 0, 0x10000 } };
   pk_cs_free(&g_cs.cs);
   switch (g_pcfg.console)
   {
   case FE_CONSOLE_GBA:
      pk_cs_init(&g_cs.cs, cs_read, NULL, PK_CS_GBA, PK_CS_LE, gba, 1);
      break;
   case FE_CONSOLE_GB:
   case FE_CONSOLE_GBC:
      pk_cs_init(&g_cs.cs, cs_read, NULL, PK_CS_GB, PK_CS_LE, gb, 2);
      break;
   case FE_CONSOLE_NES:
      pk_cs_init(&g_cs.cs, cs_read, NULL, PK_CS_NES, PK_CS_LE, nes, 1);
      break;
   case FE_CONSOLE_WSWAN:
      pk_cs_init(&g_cs.cs, cs_read, NULL, PK_CS_WS, PK_CS_LE, ws, 1);
      break;
   default:
      return 0;
   }
   g_cs.ready = 1;
   return 1;
}

/* The game ended: the snapshot and the list go (main_vita.c). */
void ui_cheat_search_free(void)
{
   pk_cs_free(&g_cs.cs);
   memset(&g_cs, 0, sizeof(g_cs));
   g_cs.op = -1;
}

static int cs_width(void) { return pk_cs_size_width(g_cs.size); }

static void cs_note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void cs_note(const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(g_cs.note, sizeof(g_cs.note), fmt, ap);
   va_end(ap);
}

static void cs_open(void)
{
   screen_to(SCR_CHEAT_SEARCH);
   g_cs.cur = g_cs.scroll = g_cs.free_scroll = 0;
   g_cs.op = -1;
   g_cs.note[0] = '\0';
   if (fe_host_gb_local_active())
   {
      /* one console, two players: the memory of the player's game cannot be
       * read while the pair runs (the read answers nothing) */
      pk_cs_free(&g_cs.cs);
      g_cs.ready = 0;
      cs_note(tr("cheats.search_memory_is_not_available_in"));
   }
   else if (!g_cs.ready && !cs_setup())
      cs_note(tr("cheats.there_is_no_memory_to_search"));
}

/* The rows now: size, new search, then (a search is on) results, the steps,
 * and the listed results. */
static int cs_rows(int *kind, int *arg)
{
   int n = 0, i;
   kind[n] = CSR_SIZE; arg[n++] = 0;
   kind[n] = CSR_NEW; arg[n++] = 0;
   if (pk_cs_active(&g_cs.cs))
   {
      kind[n] = CSR_RESULTS; arg[n++] = 0;
      if (g_cs.cs.count > 0)
         for (i = 0; i < PK_CS_OPS; i++)
         {
            kind[n] = CSR_OP; arg[n++] = i;
         }
      for (i = 0; i < g_cs.nres; i++)
      {
         kind[n] = CSR_HIT; arg[n++] = i;
      }
   }
   return n;
}

static void cs_step(int op, uint32_t v)
{
   int32_t n = pk_cs_step(&g_cs.cs, op, v);
   g_cs.nres = 0;
   if (n >= 0 && n <= PK_CS_LIST_MAX)
      g_cs.nres = pk_cs_list(&g_cs.cs, PK_CS_LIST_MAX, g_cs.addr, g_cs.val);
   fe_evt("cheat_search results=%d op=%s size=%d", (int)n, pk_cs_op_name(op),
          g_cs.cs.width);
   if (n < 0)
      cs_note(tr("cheats.there_is_no_search_to_narrow"));
   else if (n == 0)
      cs_note(tr("cheats.no_results_start_a_new_search"));
   else
      cs_note("%d %s", (int)n, n == 1 ? "result" : "results");
}

static void cs_add(int i)
{
   char code[160], at[24], name[CHEATS_NAME_CAP];
   int rc;
   if (i < 0 || i >= g_cs.nres)
      return;
   if (!pk_cs_code(&g_cs.cs, g_cs.addr[i], g_cs.val[i], code, sizeof(code)))
   {
      cs_note(tr("cheats.this_place_cannot_be_made_into"));
      return;
   }
   pk_cs_addr_text(&g_cs.cs, g_cs.addr[i], at, sizeof(at));
   snprintf(name, sizeof(name), tr("cheats.found_at"), at);
   rc = cheats_count() >= CHEATS_MAX ? CHEATS_E_FULL : cheats_add(name, code);
   fe_evt("cheat_search add rc=%d code=\"%s\"", rc, code);
   if (rc >= 0)
      cs_note(tr("cheats.cheat_added_and_on"), name);
   else
      cs_note("%s", cheats_error_text(rc));
}

LARGE_WRAP(ui_action, screen_cheat_search, (unsigned edges, unsigned pad), (edges, pad))
static ui_action screen_cheat_search_page(unsigned edges, unsigned pad)
{
   int kind[2 + 1 + PK_CS_OPS + PK_CS_LIST_MAX], arg[2 + 1 + PK_CS_OPS + PK_CS_LIST_MAX];
   int n, max, i, r;
   const char *typed;
   (void)pad;

   r = ui_text_take(TXT_SEARCH_VALUE, &typed);
   if (r > 0 && g_cs.op >= 0 && pk_cs_active(&g_cs.cs))
   {
      uint32_t v = 0;
      if (pk_cs_parse(typed, g_cs.cs.width, &v))
         cs_step(g_cs.op, v);
      else
         cs_note(tr("cheats.type_a_whole_number_from_0"),
                 (unsigned)pk_cs_max_value(g_cs.cs.width));
   }
   if (r != 0)
      g_cs.op = -1;
   n = cs_rows(kind, arg);
   if (g_cs.cur >= n) g_cs.cur = n - 1;
   if (g_cs.cur < 0) g_cs.cur = 0;
   if (edges)
      g_cs.note[0] = '\0';
   if (tch_rows(&g_cs.cur, &edges) || g_tch.hover_row != TCH_NONE)
      g_cs.free_scroll = 1;
   if (edges & PSP_CTRL_UP)
      g_cs.cur = (g_cs.cur + n - 1) % n;
   else if (edges & PSP_CTRL_DOWN)
      g_cs.cur = (g_cs.cur + 1) % n;
   else if (edges & PSP_CTRL_LTRIGGER)
      g_cs.cur = g_cs.cur - CS_ROWS < 0 ? 0 : g_cs.cur - CS_ROWS;
   else if (edges & PSP_CTRL_RTRIGGER)
      g_cs.cur = g_cs.cur + CS_ROWS >= n ? n - 1 : g_cs.cur + CS_ROWS;
   if (edges & (PSP_CTRL_UP | PSP_CTRL_DOWN | PSP_CTRL_LTRIGGER |
                PSP_CTRL_RTRIGGER))
   {
      g_cs.free_scroll = 0;
      tch_scroll_stop(&g_cs.ts);
   }
   if (g_cs.cur >= n) g_cs.cur = n - 1;
   if (g_cs.cur < 0) g_cs.cur = 0;

   if (edges & PSP_CTRL_CIRCLE)
   {
      ch_open();
      return screen_cheats(0, 0);
   }
   if (!g_cs.ready && (edges & PSP_CTRL_CROSS))
      cs_note(fe_host_gb_local_active()
              ? tr("cheats.search_memory_is_not_available_in")
              : tr("cheats.there_is_no_memory_to_search"));
   if (g_cs.ready && (edges & (PSP_CTRL_CROSS | PSP_CTRL_LEFT |
                               PSP_CTRL_RIGHT)))
   {
      const int k = kind[g_cs.cur], a = arg[g_cs.cur];
      const int susp = cheats_suspended();
      if (susp && (edges & PSP_CTRL_CROSS) && k != CSR_SIZE)
         cs_note("%s", susp == 2
                 ? tr("cheats.cheats_are_off_in_hardcore_mode")
                 : tr("cheats.cheats_are_off_while_linked_to"));
      else if (k == CSR_SIZE)
         g_cs.size = (g_cs.size + ((edges & PSP_CTRL_LEFT)
                      ? pk_cs_size_count(&g_cs.cs) - 1 : 1)) %
                     pk_cs_size_count(&g_cs.cs);
      else if (!(edges & PSP_CTRL_CROSS))
         ;
      else if (k == CSR_NEW)
      {
         int32_t c = pk_cs_start_ex(&g_cs.cs, cs_width(),
                                    pk_cs_size_any(g_cs.size) &&
                                    pk_cs_can_any(&g_cs.cs));
         g_cs.nres = 0;
         fe_evt("cheat_search new size=%d results=%d any=%d", cs_width(),
                (int)c, g_cs.cs.any);
         if (c < 0)
            cs_note(tr("cheats.the_search_could_not_start_not"));
         else
            cs_note(tr("cheats.search_started_places_play_on_then"),
                    (int)c);
         g_cs.cur = 1;
      }
      else if (k == CSR_OP)
      {
         if (!pk_cs_op_needs_value(a))
            cs_step(a, 0);
         else
         {
            char title[48];
            snprintf(title, sizeof(title), tr("cheats.a_number"), pk_cs_op_name(a));
            g_cs.op = a;
            if (ui_text_open(TXT_SEARCH_VALUE, title, "", 11) != 0)
            {
               g_cs.op = -1;
               cs_note(tr("cheats.the_keyboard_is_not_available"));
            }
         }
      }
      else if (k == CSR_HIT)
         cs_add(a);
   }

   max = n * CS_ROW_H - CS_VIEW_H;
   if (max < 0)
      max = 0;
   if (tch_scroll(&g_cs.ts, &g_cs.scroll, max, 0, CS_VIEW_TOP, 456,
                  CS_VIEW_H))
      g_cs.free_scroll = 1;
   else if (!g_cs.free_scroll)
   {
      if (g_cs.cur * CS_ROW_H < g_cs.scroll)
         g_cs.scroll = g_cs.cur * CS_ROW_H;
      if ((g_cs.cur + 1) * CS_ROW_H > g_cs.scroll + CS_VIEW_H)
         g_cs.scroll = (g_cs.cur + 1) * CS_ROW_H - CS_VIEW_H;
   }
   if (g_cs.scroll > max) g_cs.scroll = max;
   if (g_cs.scroll < 0) g_cs.scroll = 0;
   if (g_cs.free_scroll)
   {
      int first = (g_cs.scroll + CS_ROW_H - 1) / CS_ROW_H;
      int last = (g_cs.scroll + CS_VIEW_H) / CS_ROW_H - 1;
      if (last >= n) last = n - 1;
      if (g_cs.cur < first) g_cs.cur = first;
      if (g_cs.cur > last) g_cs.cur = last;
   }

   ov_backdrop(OV_SCRIM_DENSE);
   {
      const char *dbn = cheats_db_game();
      char sub[100];
      snprintf(sub, sizeof(sub), "%s", dbn && dbn[0] ? dbn : g_ov_title);
      clip_title(sub, 300);
      vw_head(sub, tr("cheats.search_memory"));
      if (pk_cs_active(&g_cs.cs))
      {
         char c[40];
         snprintf(c, sizeof(c), "%u %s", (unsigned)g_cs.cs.count,
                  g_cs.cs.count == 1 ? "result" : "results");
         ov_text_right(460, 48, c, C_VALUE);
      }
   }
   vid_clip(0, CS_VIEW_TOP - 1, VID_SCR_W, CS_VIEW_H + 2);
   for (i = g_cs.scroll / CS_ROW_H; i < n; i++)
   {
      int y = CS_VIEW_TOP + i * CS_ROW_H - g_cs.scroll;
      char label[40], value[48];
      int sel = i == g_cs.cur;
      if (y >= CS_VIEW_TOP + CS_VIEW_H)
         break;
      value[0] = '\0';
      switch (kind[i])
      {
      case CSR_SIZE:
         snprintf(label, sizeof(label), tr("cheats.size_of_the_number"));
         snprintf(value, sizeof(value), "%s", pk_cs_size_choice(g_cs.size));
         break;
      case CSR_NEW:
         snprintf(label, sizeof(label), tr("cheats.new_search"));
         snprintf(value, sizeof(value), "%s", pk_cs_active(&g_cs.cs)
                  ? tr("cheats.start_again") : tr("cheats.look_at_the_game_s_memory"));
         break;
      case CSR_RESULTS:
         snprintf(label, sizeof(label), tr("cheats.results"));
         snprintf(value, sizeof(value), "%u", (unsigned)g_cs.cs.count);
         break;
      case CSR_OP:
         snprintf(label, sizeof(label), "%s", pk_cs_op_name(arg[i]));
         snprintf(value, sizeof(value), "%s", pk_cs_op_needs_value(arg[i])
                  ? tr("cheats.a_number_you_type") : tr("cheats.since_the_last_step"));
         break;
      default:
         pk_cs_addr_text(&g_cs.cs, g_cs.addr[arg[i]], label, sizeof(label));
         snprintf(value, sizeof(value), "%u (0x%X)",
                  (unsigned)g_cs.val[arg[i]], (unsigned)g_cs.val[arg[i]]);
         break;
      }
      vw_row(i, y + (CS_ROW_H - VW_BAND_H) / 2, 456, sel,
             g_cs.ready && kind[i] != CSR_RESULTS, label, value);
   }
   vid_clip_off();
   ov_scrollbar(470, CS_VIEW_TOP, CS_VIEW_H, g_cs.scroll, CS_VIEW_H,
                n * CS_ROW_H);
   /* what the highlighted row does */
   {
      const char *h = "";
      switch (kind[g_cs.cur])
      {
      case CSR_SIZE: h = pk_cs_size_count(&g_cs.cs) > 3
                         ? tr("cheats.most_counters_are_1_byte_money")
                         : tr("cheats.most_counters_are_1_byte_money_2"); break;
      case CSR_NEW: h = tr("cheats.remember_the_memory_play_on_then"); break;
      case CSR_RESULTS:
         h = g_cs.cs.count == 0 ? tr("cheats.nothing_matched_start_a_new_search")
           : g_cs.cs.count <= PK_CS_LIST_MAX
             ? tr("cheats.choose_a_result_to_make_it")
             : tr("cheats.narrow_it_down_to_50_or"); break;
      case CSR_OP: h = pk_cs_op_needs_value(arg[g_cs.cur])
                       ? tr("cheats.keeps_the_places_that_match_the")
                       : tr("cheats.compared_with_the_memory_at_the"); break;
      default: h = tr("cheats.makes_a_cheat_that_keeps_this"); break;
      }
      /* the note's plate sits where this line is: one or the other */
      if (!g_cs.note[0])
      {
         vid_rect(20, CS_VIEW_TOP + CS_VIEW_H + 3, 440, 1, C_ACCENT_DK, 120);
         vw_line(22, CS_VIEW_TOP + CS_VIEW_H + 8, 436, h, C_DIM);
      }
   }
   if (g_cs.note[0])
      vw_plate(g_cs.note);
   else
      footer(tr("hint.x_choose_l_r_page_o"));
   return UI_ACT_NONE;
}

#endif /* UI_VITA_CHEATS_H */
