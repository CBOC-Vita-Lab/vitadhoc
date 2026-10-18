/* ui_vita_browser.h -- the game list on the Vita: the finger, and a first
 * run that says where the games go.
 *
 * Included by psp/ui_psp.c under -DVITA just before ui_browser (it uses the
 * browser's own state: the shells' geometry, the state shelf, rom_scan).
 *
 * THE FINGER IN THE GAME LIST
 *   a row            selects that game (the list slides it under the band)
 *   the selected row, or the box art / the big title
 *                    starts it
 *   drag, flick      scrolls the list, a row at a time, and coasts
 *   SELECT's pill, the console card, every footer hint
 *                    that button
 *   the state shelf  a slot loads it; beside the shelf closes it
 * A tap SELECTS first and starts on the second tap -- unlike the menus,
 * where a tap is select-and-go -- because starting a game takes seconds
 * and the row under a finger at the end of a flick is not always the one
 * meant.
 */
#ifndef UI_VITA_BROWSER_H
#define UI_VITA_BROWSER_H

static tch_scroll_t g_br_ts;

/* The list's drag area and row pitch for the current shell. */
static void vita_browser_list(int *x, int *y, int *w, int *h, int *row_h)
{
   if (g_ui_shell)
   {
      *x = 0; *y = MQ_MID - 2 * MQ_ROW_H - 8; *w = 290;
      *h = 5 * MQ_ROW_H + 8; *row_h = MQ_ROW_H;
   }
   else
   {
      *x = 0; *y = SHELF_TOP - 6; *w = 300; *h = 246 - (SHELF_TOP - 6);
      *row_h = SHELF_ROW_H;
   }
}

/* One frame of touch in the browser, after the pad's edges are known.
 * `busy`: a transition owns the list (console flare, favourites swap, the
 * crossfade from a game) -- nothing may move it. */
static void vita_browser_touch(int *cur, int n, unsigned *edges, int *idle,
                               int busy)
{
   int id, k, x, y, w, h, row_h;
   if (busy)
   {
      int dummy;
      (void)tch_tapped_row(&dummy);
      tch_scroll_stop(&g_br_ts);
      return;
   }
   if (g_browser_state_open)
   {
      /* The shelf's slots: a tap is X on that slot. */
      int s = g_browser_state_slot - 1;
      (void)tch_rows(&s, edges);
      if (s >= 0 && s < PSP_STATE_SLOT_COUNT)
         g_browser_state_slot = s + 1;
      tch_scroll_stop(&g_br_ts);
      return;
   }
   if (n <= 0)
      return;
   if (*edges & (PSP_CTRL_UP | PSP_CTRL_DOWN | PSP_CTRL_LTRIGGER |
                 PSP_CTRL_RTRIGGER))
      tch_scroll_stop(&g_br_ts);
   if (tch_tapped_row(&id) && id >= 0 && id < n)
   {
      if (id == *cur)
         *edges |= PSP_CTRL_CROSS;
      else
      {
         *cur = id;
         *idle = 0;
      }
   }
   if (g_ui_home)
      return;                   /* the home row has no list to drag */
   vita_browser_list(&x, &y, &w, &h, &row_h);
   k = tch_steps(&g_br_ts, row_h, x, y, w, h);
   if (k)
   {
      *cur = ((*cur + k) % n + n) % n;
      *idle = 0;
   }
}

/* ---- the empty console / the first run ----------------------------------- */

static int g_empty_for = -1;          /* console the counts below are for   */
static int g_empty_others;            /* games on the OTHER two consoles    */

/* How many games the other consoles have: two more scans, once per visit
 * to an empty console (the list being scanned is empty, so nothing is
 * lost), then this console's scan again to leave rom_scan's state as the
 * browser expects it. */
static int vita_browser_others(const char *rom_dir)
{
   int c, k;
   if (g_empty_for == g_pcfg.console)
      return g_empty_others;
   g_empty_others = 0;
   for (c = 0; c < FE_CONSOLE_COUNT; c++)
      if (c != g_pcfg.console && (k = rom_scan(rom_dir, (fe_console_t)c)) > 0)
         g_empty_others += k;
   (void)rom_scan(rom_dir, (fe_console_t)g_pcfg.console);
   g_empty_for = g_pcfg.console;
   return g_empty_others;
}

/* The extra folders (ui_host_extra_roots) in one short line, for a page that
 * says where games go: the games of those are listed too, so "no games" is
 * said of them as well.  "" when there are none. */
static const char *vita_extra_note(char *out, size_t cap)
{
   int n = ui_host_extra_roots();
   out[0] = '\0';
   if (n == 1)
      snprintf(out, cap, "Also looked in %s", ui_host_extra_root(0));
   else if (n > 1)
      snprintf(out, cap, "Also looked in %s and %d more", ui_host_extra_root(0),
               n - 1);
   return out;
}

/* The page for a console with no games: what is missing, exactly where it
 * goes, and the one thing to press afterwards. */
static void vita_browser_empty(const char *rom_dir, const ui_skin *s,
                               uint16_t sel, uint16_t item, uint16_t dim)
{
   char msg[96], dir[PSP_FILE_PATH_CAP];
   int others = vita_browser_others(rom_dir);
   browser_cart(240, 92, s);
   snprintf(msg, sizeof(msg), "No %s games yet", s->full);
   vid_text_hd((480 - vid_text_hd_w(msg)) / 2, 118, msg, sel);
   snprintf(msg, sizeof(msg), "Copy your .%s files into this folder on the "
            "memory card:", s->ext);
   vid_text_center(150, msg, item);
   snprintf(dir, sizeof(dir), "%s", rom_dir);
   clip_title(dir, 440);
   vid_text_center(170, dir, sel);
   if (others > 0)
   {
      snprintf(msg, sizeof(msg), VID_GLYPH_TRI " shows your %d other game%s",
               others, others == 1 ? "" : "s");
      vid_text_center(200, msg, dim);
      UI_TCH_BTN(PSP_CTRL_TRIANGLE, (480 - vid_text_w(msg)) / 2 - 10, 192,
                 vid_text_w(msg) + 20, 32);
   }
   else
      vid_text_center(200, ".gba, .gb and .gbc games all go there. Then "
                      "press " VID_GLYPH_X ".", dim);
   if (vita_extra_note(dir, sizeof(dir))[0])
   {
      clip_title(dir, 440);
      vid_text_center(224, dir, dim);
   }
}

#endif /* UI_VITA_BROWSER_H */
