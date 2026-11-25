/* ui_vita_lists.h -- the Collections page: the highlighted game's lists.
 *
 * Included by psp/ui_psp.c under -DVITA before the browser.  Opened from the
 * game list with SELECT + SQUARE (the Home also has a Lists button), over the
 * same backdrop Settings uses:
 *
 *   X         add the game to the collection, or take it out (a tap on a
 *             row is the same);  on "New collection" it asks for a name
 *   SQUARE    rename the collection under the cursor (the keyboard, as Find)
 *   TRIANGLE  delete it, after a question -- the games stay on the card
 *   O         back
 *
 * Every change is written at once (collections.txt, vita/ui_vita_library.h
 * and frontend-common/fe_collect.h: a temp file, the old one aside, the new
 * one in).  A write that fails puts the file's own list back on screen and
 * says so: what is shown is always what is on the card.  RetroArch's
 * playlists are not here: they are read-only and have no page. */
#ifndef UI_VITA_LISTS_H
#define UI_VITA_LISTS_H

#define LS_ROWS 6
#define LS_Y0   70

static struct
{
   int  rom;                    /* the game: a g_roms index                 */
   int  cur, top;               /* the row; the first visible row           */
   int  ask;                    /* the collection a delete was asked of, -1 */
   int  rename;                 /* the collection being renamed, -1         */
   int  changed;                /* something was written                    */
   char name[96];               /* the game's name, for the header          */
   char note[96];
} g_ls;

/* Write collections.txt; on a failure put what is on the card back. */
static int ls_save(void)
{
   char p[PSP_FILE_PATH_CAP];
   int rc = lib_coll_path(p, sizeof(p)) == 0 ? fe_coll_save(&g_coll, p) : -1;
   fe_evt("collections_save n=%u games=%u rc=%d", g_coll.ncoll, g_coll.n, rc);
   if (rc != 0)
   {
      lib_load_collections();
      snprintf(g_ls.note, sizeof(g_ls.note), rc == -2
               ? "The collections file is too big to change"
               : "Could not save the collections");
      if (g_ls.cur > (int)g_coll.ncoll)
         g_ls.cur = (int)g_coll.ncoll;
      g_ls.changed = 1;           /* what is on screen may differ from before */
      return rc;
   }
   g_ls.changed = 1;
   return 0;
}

static void ls_toggle(int k)
{
   const char *rel = g_roms[g_ls.rom].name;
   char nm[FE_COLL_NAME_MAX];
   int rc;
   snprintf(nm, sizeof(nm), "%s", g_coll.name[k]);
   if (fe_coll_has(&g_coll, k, rel))
   {
      fe_coll_remove(&g_coll, k, rel);
      if (ls_save() == 0)
         snprintf(g_ls.note, sizeof(g_ls.note), "Removed from \"%s\"", nm);
      fe_evt("ui_collection op=remove name=\"%s\" rom=%s", nm, rel);
   }
   else
   {
      rc = fe_coll_add(&g_coll, k, rel);
      if (rc != 0)
         snprintf(g_ls.note, sizeof(g_ls.note), "No room for more games");
      else if (ls_save() == 0)
         snprintf(g_ls.note, sizeof(g_ls.note), "Added to \"%s\"", nm);
      fe_evt("ui_collection op=add name=\"%s\" rom=%s rc=%d", nm, rel, rc);
   }
}

static int ls_screen(unsigned edges)
{
   const char *typed;
   int total = (int)g_coll.ncoll + 1, i, r;
   char tmp[96];
   int y0;

   r = ui_text_take(TXT_COLL_NEW, &typed);
   if (r > 0)
   {
      int k = fe_coll_create(&g_coll, typed);
      if (k == -2)
         snprintf(g_ls.note, sizeof(g_ls.note), "A collection needs a name");
      else if (k == -3)
         snprintf(g_ls.note, sizeof(g_ls.note), "There is one with that name");
      else if (k < 0)
         snprintf(g_ls.note, sizeof(g_ls.note), "No room for another collection");
      else
      {
         fe_evt("ui_collection op=create name=\"%s\"", g_coll.name[k]);
         g_ls.cur = k;
         ls_toggle(k);               /* the game goes in at once */
      }
      total = (int)g_coll.ncoll + 1;
   }
   r = ui_text_take(TXT_COLL_RENAME, &typed);
   if (r > 0 && g_ls.rename >= 0 && g_ls.rename < (int)g_coll.ncoll)
   {
      char old[FE_COLL_NAME_MAX];
      int rc;
      snprintf(old, sizeof(old), "%s", g_coll.name[g_ls.rename]);
      rc = fe_coll_rename(&g_coll, g_ls.rename, typed);
      if (rc == -2)
         snprintf(g_ls.note, sizeof(g_ls.note), "A collection needs a name");
      else if (rc == -3)
         snprintf(g_ls.note, sizeof(g_ls.note), "There is one with that name");
      else if (rc == 0 && strcmp(old, g_coll.name[g_ls.rename]) != 0 &&
               ls_save() == 0)
      {
         snprintf(g_ls.note, sizeof(g_ls.note), "Renamed to \"%s\"",
                  g_coll.name[g_ls.rename]);
         fe_evt("ui_collection op=rename from=\"%s\" to=\"%s\"", old,
                g_coll.name[g_ls.rename]);
      }
   }

   /* the delete question: X answers it, anything else keeps the collection */
   if (g_ls.ask >= 0)
   {
      if (edges & PSP_CTRL_CROSS)
      {
         char nm[FE_COLL_NAME_MAX];
         snprintf(nm, sizeof(nm), "%s", g_coll.name[g_ls.ask]);
         fe_coll_delete(&g_coll, g_ls.ask);
         if (ls_save() == 0)
         {
            snprintf(g_ls.note, sizeof(g_ls.note), "Deleted \"%s\"", nm);
            /* the ones after it moved up: the list behind keeps showing the
             * collection it showed, or all games when that one is gone */
            if (g_view_kind == VK_COLL && g_view_which == g_ls.ask)
            {
               g_view_kind = VK_ALL;
               g_view_which = 0;
               g_browser_favs = 0;
            }
            else if (g_view_kind == VK_COLL && g_view_which > g_ls.ask)
               g_view_which--;
         }
         fe_evt("ui_collection op=delete name=\"%s\"", nm);
         g_ls.ask = -1;
         total = (int)g_coll.ncoll + 1;
         if (g_ls.cur >= total)
            g_ls.cur = total - 1;
      }
      else if (edges)
         g_ls.ask = -1;
      edges = 0;
   }
   else if (edges)
      g_ls.note[0] = '\0';

   if (g_ls.cur >= total)
      g_ls.cur = total - 1;
   tch_rows(&g_ls.cur, &edges);
   if (edges & PSP_CTRL_UP)
      g_ls.cur = (g_ls.cur + total - 1) % total;
   if (edges & PSP_CTRL_DOWN)
      g_ls.cur = (g_ls.cur + 1) % total;
   if (edges & PSP_CTRL_LTRIGGER)
      g_ls.cur = g_ls.cur >= LS_ROWS ? g_ls.cur - LS_ROWS : 0;
   if (edges & PSP_CTRL_RTRIGGER)
      g_ls.cur = g_ls.cur + LS_ROWS < total ? g_ls.cur + LS_ROWS : total - 1;
   if (g_ls.cur < g_ls.top)
      g_ls.top = g_ls.cur;
   if (g_ls.cur >= g_ls.top + LS_ROWS)
      g_ls.top = g_ls.cur - LS_ROWS + 1;

   if (edges & PSP_CTRL_CIRCLE)
      return 1;
   if (edges & PSP_CTRL_CROSS)
   {
      if (g_ls.cur < (int)g_coll.ncoll)
         ls_toggle(g_ls.cur);
      else if (ui_text_open(TXT_COLL_NEW, "Collection name", "",
                            FE_COLL_NAME_MAX - 1) != 0)
         snprintf(g_ls.note, sizeof(g_ls.note), "The keyboard is not available");
   }
   if ((edges & PSP_CTRL_SQUARE) && g_ls.cur < (int)g_coll.ncoll)
   {
      g_ls.rename = g_ls.cur;
      if (ui_text_open(TXT_COLL_RENAME, "Rename collection",
                       g_coll.name[g_ls.cur], FE_COLL_NAME_MAX - 1) != 0)
         snprintf(g_ls.note, sizeof(g_ls.note), "The keyboard is not available");
   }
   if ((edges & PSP_CTRL_TRIANGLE) && g_ls.cur < (int)g_coll.ncoll)
      g_ls.ask = g_ls.cur;

   ov_backdrop(OV_SCRIM_DENSE);
   vw_head(g_ls.name, "COLLECTIONS");
   y0 = LS_Y0 + (g_coll.ncoll ? 14 : 44);
   if (!g_coll.ncoll)
      vw_wrap(22, 62, 436, "Collections are lists of your own, across "
              "every console. Make one to put this game in it.", C_DIM, 2);
   for (i = g_ls.top; i < total && i < g_ls.top + LS_ROWS; i++)
   {
      int y = y0 + (i - g_ls.top) * VW_PITCH, sel = g_ls.cur == i;
      if (i < (int)g_coll.ncoll)
         vw_row(i, y, 440, sel, 1,
                vw_clip(tmp, sizeof(tmp), g_coll.name[i], 280),
                fe_coll_has(&g_coll, i, g_roms[g_ls.rom].name) ? "Added" : NULL);
      else
         vw_row(i, y, 440, sel, 1, "New collection", NULL);
   }
   if (total > LS_ROWS)
   {
      int track = LS_ROWS * VW_PITCH - 4;
      int kh = track * LS_ROWS / total, ky = track * g_ls.top / total;
      vid_rect(452, y0, 2, track, C_DIM, 90);
      vid_rect(452, y0 + ky, 2, kh < 6 ? 6 : kh, C_ACCENT, 255);
   }
   if (g_ls.ask >= 0)
   {
      char q[112], nm[40];
      vw_clip(nm, sizeof(nm), g_coll.name[g_ls.ask], 170);
      snprintf(q, sizeof(q), "Delete \"%s\"?      " VID_GLYPH_X
               " delete     " VID_GLYPH_O " keep", nm);
      tch_modal(PSP_CTRL_CIRCLE);
      ov_plate(q);
   }
   else if (g_ls.note[0])
      vw_plate(g_ls.note);
   else if (g_ls.cur < (int)g_coll.ncoll)
      footer(VID_GLYPH_X " add/remove   " VID_GLYPH_SQ " rename   " VID_GLYPH_TRI
             " delete   " VID_GLYPH_O " back");
   else
      footer(VID_GLYPH_X " new collection   " VID_GLYPH_O " back");
   return 0;
}

/* Runs the page until the player leaves it.  `rom`: a g_roms index.
 * Returns 1 when anything was written. */
static int ui_lists_run(const char *rom_dir, int rom, int n_scan)
{
   (void)rom_dir;
   (void)n_scan;
   memset(&g_ls, 0, sizeof(g_ls));
   g_ls.rom = rom;
   g_ls.ask = g_ls.rename = -1;
   rom_display_name(&g_roms[rom], g_ls.name, sizeof(g_ls.name), 1);
   browse_backdrop_take();
   g_ov_browse = 1;
   fe_evt("ui_screen name=collections from=browser rom=%s n=%u", g_roms[rom].name,
          g_coll.ncoll);
   g_prev_pad = 0xFFFFFFFFu;
   while (g_running && !g_sh_quit)
   {
      SceCtrlData pd;
      unsigned edges, pad;
      sceCtrlPeekBufferPositive(&pd, 1);
      pad = ui_demo_running() ? demo_pad() : pd.Buttons;
      edges = vita_frame_begin(&pad);
      browser_demo(++g_bframe, &edges);
      vid_overlay_begin(1);
      if (ls_screen(edges))
      {
         vid_overlay_end();
         break;
      }
      vid_overlay_end();
      osd_draw();
      demo_dump_flush();
      sceDisplayWaitVblankStart();
      vid_swap();
   }
   g_ov_browse = 0;
   vid_ambient_drop();
   g_prev_pad = 0xFFFFFFFFu;
   return g_ls.changed;
}

#endif /* UI_VITA_LISTS_H */
