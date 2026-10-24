/* ui_vita_ra.h -- Settings > Achievements: the running game's
 * RetroAchievements, in the runtime's grouping (ra_vita.h).
 *
 * Included by psp/ui_psp.c under -DVITA after ui_vita_widgets.h.  Two lines
 * a row -- the title with its points, then what it asks for -- and the
 * page's head says how far along the game is.  The d-pad (L/R a page), a
 * drag or a flick scrolls it; O goes back to Settings.  The list is taken
 * when the page opens and again when something is earned while it is up. */
#ifndef UI_VITA_RA_H
#define UI_VITA_RA_H

#define RA_TOP    66            /* the list's viewport */
#define RA_BOT    238
#define RA_PITCH  42            /* two lines and a gap */

static struct
{
   int cur, scroll;
   tch_scroll_t ts;
   int free_scroll;
} g_rap;

static int ra_page_count(void)
{
   int n = 0;
   while (ra_list_get(n))
      n++;
   return n;
}

static void ra_page_open(void)
{
   ra_list_build();
   memset(&g_rap, 0, sizeof(g_rap));
   screen_to(SCR_RA);
   fe_evt("ra_page open n=%d", ra_page_count());
}

static ui_action screen_ra(unsigned edges)
{
   unsigned have = 0, total = 0, ph = 0, pt = 0;
   int hc = 0, n = ra_page_count(), i, max;
   char sub[96], sum[48];

   if (edges & PSP_CTRL_CIRCLE)
   {
      ra_list_free();
      screen_to(SCR_SETTINGS);
      g_cursor = SET_RA_LIST;
      return UI_ACT_NONE;
   }
   if (tch_rows(&g_rap.cur, &edges))
      g_rap.free_scroll = 1;
   if (edges & PSP_CTRL_UP)   { g_rap.cur--; g_rap.free_scroll = 0; }
   if (edges & PSP_CTRL_DOWN) { g_rap.cur++; g_rap.free_scroll = 0; }
   if (edges & PSP_CTRL_LTRIGGER) { g_rap.cur -= 4; g_rap.free_scroll = 0; }
   if (edges & PSP_CTRL_RTRIGGER) { g_rap.cur += 4; g_rap.free_scroll = 0; }
   if (g_rap.cur >= n) g_rap.cur = n - 1;
   if (g_rap.cur < 0)  g_rap.cur = 0;
   max = n * RA_PITCH - (RA_BOT - RA_TOP);
   if (max < 0)
      max = 0;
   if (tch_scroll(&g_rap.ts, &g_rap.scroll, max, 0, RA_TOP, VID_SCR_W,
                  RA_BOT - RA_TOP))
      g_rap.free_scroll = 1;
   else if (!g_rap.free_scroll)
   {
      int y = g_rap.cur * RA_PITCH;
      if (y < g_rap.scroll)
         g_rap.scroll = y;
      if (y + RA_PITCH > g_rap.scroll + (RA_BOT - RA_TOP))
         g_rap.scroll = y + RA_PITCH - (RA_BOT - RA_TOP);
      if (g_rap.scroll > max)
         g_rap.scroll = max;
   }

   ov_backdrop(OV_SCRIM_DENSE);
   ra_summary(&have, &total, &ph, &pt, &hc);
   snprintf(sub, sizeof(sub), "%s%s", ra_game_title(),
            hc ? " - Hardcore" : "");
   vw_head(sub, "ACHIEVEMENTS");
   snprintf(sum, sizeof(sum), "%u / %u   %u / %u pts", have, total, ph, pt);
   ov_text_right(460, 48, sum, C_VALUE);

   vid_clip(0, RA_TOP - 2, VID_SCR_W, RA_BOT - RA_TOP + 4);
   for (i = 0; i < n; i++)
   {
      const ra_ach *a = ra_list_get(i);
      int y = RA_TOP + i * RA_PITCH - g_rap.scroll + 4;
      char pts[16], desc[200];
      if (y + RA_PITCH <= RA_TOP - 2 || y >= RA_BOT + 2)
         continue;
      if (i == g_rap.cur)
         ov_band(y + FE_FONT_H / 2 + 1, 460, RA_PITCH - 4);
      snprintf(pts, sizeof(pts), "%u", a->points);
      vw_line(22, y, 380, a->title,
              a->unlocked ? (i == g_rap.cur ? C_SEL : C_ITEM) : C_DIM);
      ov_text_right(452, y, pts,
                    a->unlocked ? C_VALUE : C_DIM);
      if (a->progress[0] && !a->unlocked)
         snprintf(desc, sizeof(desc), "%s  (%s)", a->desc, a->progress);
      else
         snprintf(desc, sizeof(desc), "%s", a->desc);
      vw_line(22, y + FE_FONT_H + 2, 430, desc, C_DIM);
      if (a->unlocked)
         vid_rect(12, y + 4, 4, FE_FONT_H - 6, C_CON, 255);
      tch_row(i, 0, y - 2, 460, RA_PITCH);
   }
   vid_clip_off();
   if (!n)
      vid_text(22, RA_TOP + 10, "No achievements to show.", C_DIM);
   footer("UP/DOWN scroll   L/R page   " VID_GLYPH_O " back");
   return UI_ACT_NONE;
}

#endif /* UI_VITA_RA_H */
