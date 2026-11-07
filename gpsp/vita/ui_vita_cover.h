/* ui_vita_cover.h -- Cover picture: a picture of the player's own for a game.
 *
 * Included by psp/ui_psp.c under -DVITA just after ui_vita_home.h (it uses
 * the browser's game table and the Home's artwork keys).  The files and the
 * rules are cover_custom.h; this is the page and the glue:
 *
 *   Home, the Cover button / Settings > LIBRARY > Cover picture
 *       Choose a picture...             the console's photos and screenshots
 *                                       (ux0:picture and below), VitAdhoc's
 *                                       screenshot folder, and covers/
 *       Use a screenshot of the game    its newest screenshot, scene, state
 *                                       picture or title screen
 *       Automatic (download)            the downloaded cover again
 *   a picture is previewed first and set on X; a file named like the game in
 *   covers/ is taken at the next refresh (ui_cover_inbox_scan / _pump).
 *
 * Nothing here names a console: a game is its file name without extension.
 */
#ifndef UI_VITA_COVER_H
#define UI_VITA_COVER_H

#include <strings.h>
#include "cover_custom.h"

extern char g_dir_base[];

enum { CV_MENU, CV_PICK, CV_SURE };
#define CV_PICS_MAX   1500
#define CV_LIST_TOP   56
#define CV_LIST_ROWS  7
#define CV_LIST_W     270
#define CV_PREV_X     300
#define CV_PREV_Y     52
#define CV_PREV_W     162
#define CV_PREV_H     176

static struct
{
   int         page, cur, scroll, prev_page;
   int         rom, n_scan;
   char        stem[256], name[100], sure_path[600], sure_from[24];
   char        note[120], shot_path[600];
   int         have_shot, changed;
   cover_pic  *pics;
   int         npics;
   /* the preview picture */
   uint16_t   *tex;
   int         tw, th, tex_ok, tex_wait;
   char        tex_path[600];
} g_cv;

/* ---- the preview ------------------------------------------------------------ */

static void cv_tex_free(void)
{
   if (g_cv.tex)
   {
      UI_TEX_GONE(g_cv.tex, ART_TEX_W * ART_TEX_H * 2);
      free(g_cv.tex);
      g_cv.tex = NULL;
   }
   g_cv.tex_ok = 0;
   g_cv.tex_path[0] = '\0';
}

/* `path`: a PNG/JPG or a .shot; NULL: the game's cover as it is now. */
static void cv_preview(const char *path)
{
   int dw = ART_PX_W, dh = ART_PX_H, ok = 0;
   if (!g_cv.tex)
      g_cv.tex = (uint16_t *)memalign(16, ART_TEX_W * ART_TEX_H * 2);
   g_cv.tex_ok = 0;
   g_cv.tex_wait = 0;
   snprintf(g_cv.tex_path, sizeof(g_cv.tex_path), "%s", path ? path : "");
   if (!g_cv.tex)
      return;
   memset(g_cv.tex, 0, ART_TEX_W * ART_TEX_H * 2);
   if (!path)
      ok = art_load_any(g_roms[g_cv.rom].base, "boxart", g_cv.tex, ART_TEX_W,
                        &dw, &dh) == 0;
   else if (strlen(path) > 5 && !strcasecmp(path + strlen(path) - 5, ".shot"))
   {
      int len = 0;
      unsigned char *f = art_slurp(path, &len);
      unsigned w = 0, h = 0, y;
      if (f && len >= 16 && psp_state_shot_check(f, &w, &h) == 0 &&
          (size_t)len >= 16 + (size_t)h * PSP_STATE_SHOT_STRIDE * 2)
      {
         for (y = 0; y < h; y++)
            memcpy(g_cv.tex + (size_t)y * ART_TEX_W,
                   f + 16 + (size_t)y * PSP_STATE_SHOT_STRIDE * 2, w * 2);
         dw = (int)w;
         dh = (int)h;
         ok = 1;
      }
      free(f);
   }
   else
      ok = art_load_stb(path, g_cv.tex, ART_TEX_W, &dw, &dh) == 0;
   if (ok)
   {
      sceKernelDcacheWritebackRange(g_cv.tex, ART_TEX_W * ART_TEX_H * 2);
      g_cv.tw = dw;
      g_cv.th = dh;
      g_cv.tex_ok = 1;
   }
}

/* The preview in a box, the picture's own shape kept (art_fit), centred. */
static void cv_preview_draw(int x, int y, int w, int h)
{
   int lw, lh;
   if (!g_cv.tex_ok)
   {
      vid_rect(x, y, w, h, C_CARD, 120);
      {
         const char *m = g_cv.tex_wait ? "..." : "No preview";
         vid_text(x + (w - vid_text_w(m)) / 2, y + h / 2 - 8, m, C_DIM);
      }
      return;
   }
   art_fit(g_cv.tw, g_cv.th, w, h, &lw, &lh);
   vid_rect(x + (w - lw) / 2 + 3, y + (h - lh) / 2 + 4, lw, lh, C_SHADOW, 90);
   vid_image(x + (w - lw) / 2, y + (h - lh) / 2, lw, lh, g_cv.tex,
             ART_TEX_W, ART_TEX_H, g_cv.tw, g_cv.th, 255);
}

/* ---- where the pictures are ------------------------------------------------------- */

static void cv_dirs(char dirs[3][PSP_FILE_PATH_CAP], const char *p[3], int *n)
{
   const char *shots = ui_host_shot_dir();
   *n = 0;
   snprintf(dirs[0], PSP_FILE_PATH_CAP, "%s", ui_host_pictures_dir());
   snprintf(dirs[1], PSP_FILE_PATH_CAP, "%s",
            shots && shots[0] ? shots : "");
   if (!dirs[1][0])
      snprintf(dirs[1], PSP_FILE_PATH_CAP, "%s/screenshots", g_dir_base);
   snprintf(dirs[2], PSP_FILE_PATH_CAP, "%s/%s", g_dir_base,
            COVER_INBOX_DIR);
   for (int i = 0; i < 3; i++)
   {
      int dup = 0;
      for (int j = 0; j < i; j++)
         if (!strcmp(dirs[i], dirs[j]))
            dup = 1;
      if (!dup)
         p[(*n)++] = dirs[i];
   }
}

static void cv_pics_load(void)
{
   char dirs[3][PSP_FILE_PATH_CAP];
   const char *p[3];
   int n;
   free(g_cv.pics);
   g_cv.pics = NULL;
   g_cv.npics = 0;
   cv_dirs(dirs, p, &n);
   g_cv.pics = cover_pictures(p, n, CV_PICS_MAX, &g_cv.npics);
}

/* "name" of a file, without folder and extension, for a row. */
static void cv_file_name(const char *path, char *out, size_t cap)
{
   const char *s = strrchr(path, '/'), *dot;
   size_t n;
   s = s ? s + 1 : path;
   dot = strrchr(s, '.');
   n = dot && dot > s ? (size_t)(dot - s) : strlen(s);
   if (n >= cap)
      n = cap - 1;
   memcpy(out, s, n);
   out[n] = '\0';
}

/* The game's own latest picture, if it has one. */
static void cv_find_shot(const char *rom_dir)
{
   char state1[PSP_FILE_PATH_CAP];
   const char *shots = ui_host_shot_dir();
   state1[0] = '\0';
   if (browser_rom_state_path(rom_dir, g_cv.rom, 1, state1,
                              sizeof(state1)) != 0)
      state1[0] = '\0';
   g_cv.have_shot = cover_game_picture(g_cv.stem, shots, state1,
                                       g_cv.shot_path,
                                       sizeof(g_cv.shot_path));
}

/* ---- artwork keys ---------------------------------------------------------------------- */

/* The game's cover changed on the card: every picture made from the old one
 * is made again (the Home's tile, the shelf, the hero). */
static void cv_changed(void)
{
   g_cv.changed = 1;
   hm_art_arrived(g_roms[g_cv.rom].base, g_cv.n_scan);
   g_art_fade_idx = -1;
}

static void cv_apply(const char *path, const char *from)
{
   char why[120];
   int rc = cover_set_file(g_cv.stem, path, why, sizeof(why));
   fe_evt("cover_set stem=%s rc=%d from=%s", g_cv.stem, rc, from);
   if (rc == 0)
   {
      snprintf(g_cv.note, sizeof(g_cv.note), "Cover set. It shows at once.");
      cv_changed();
      cv_preview(NULL);
   }
   else
      snprintf(g_cv.note, sizeof(g_cv.note), "%s", why);
   g_cv.page = CV_MENU;
   g_cv.cur = 0;
}

static void cv_automatic(void)
{
   int rc = cover_auto(g_cv.stem);
   fe_evt("cover_auto stem=%s rc=%d", g_cv.stem, rc);
   if (rc == 1)
      snprintf(g_cv.note, sizeof(g_cv.note), "The downloaded cover is back.");
   else if (rc == 2)
      snprintf(g_cv.note, sizeof(g_cv.note),
               "Automatic. A cover is downloaded when there is Wi-Fi.");
   else if (rc == 0)
      snprintf(g_cv.note, sizeof(g_cv.note), "It is automatic already.");
   else
      snprintf(g_cv.note, sizeof(g_cv.note), "That did not work.");
   if (rc > 0)
   {
      cv_changed();
      cv_preview(NULL);
   }
}

/* ---- the page ---------------------------------------------------------------------------- */

/* One frame.  Returns 1 to leave. */
static int cv_screen(unsigned edges, const char *rom_dir)
{
   char nm[160];
   int i, ret = 0;
   (void)rom_dir;

   if (edges)
      g_cv.note[0] = '\0';
   (void)tch_rows(&g_cv.cur, &edges);

   if (g_cv.page == CV_MENU)
   {
      const int rows = 3;
      if (edges & PSP_CTRL_UP)
         g_cv.cur = (g_cv.cur + rows - 1) % rows;
      if (edges & PSP_CTRL_DOWN)
         g_cv.cur = (g_cv.cur + 1) % rows;
      if (g_cv.cur >= rows)
         g_cv.cur = rows - 1;
      if (edges & PSP_CTRL_CIRCLE)
         return 1;
      if (edges & PSP_CTRL_CROSS)
      {
         if (g_cv.cur == 0)
         {
            cv_pics_load();
            g_cv.page = CV_PICK;
            g_cv.cur = 0;
            g_cv.scroll = 0;
            g_cv.tex_path[0] = '\0';
            g_cv.tex_ok = 0;
            g_cv.tex_wait = 1;
            g_cv.prev_page = 0;
            fe_evt("cover_pick pictures=%d", g_cv.npics);
            edges = 0;
         }
         else if (g_cv.cur == 1)
         {
            if (g_cv.have_shot)
            {
               snprintf(g_cv.sure_path, sizeof(g_cv.sure_path), "%s",
                        g_cv.shot_path);
               snprintf(g_cv.sure_from, sizeof(g_cv.sure_from), "game");
               cv_preview(g_cv.sure_path);
               g_cv.page = CV_SURE;
               g_cv.prev_page = CV_MENU;
            }
            else
               snprintf(g_cv.note, sizeof(g_cv.note),
                        "No screenshot of this game yet.");
         }
         else
            cv_automatic();
      }
   }
   else if (g_cv.page == CV_PICK)
   {
      int n = g_cv.npics;
      if (n > 0)
      {
         if (edges & PSP_CTRL_UP)
            g_cv.cur = (g_cv.cur + n - 1) % n;
         if (edges & PSP_CTRL_DOWN)
            g_cv.cur = (g_cv.cur + 1) % n;
         if (edges & PSP_CTRL_LTRIGGER)
            g_cv.cur = g_cv.cur - CV_LIST_ROWS < 0 ? 0 : g_cv.cur - CV_LIST_ROWS;
         if (edges & PSP_CTRL_RTRIGGER)
            g_cv.cur = g_cv.cur + CV_LIST_ROWS >= n ? n - 1
                                                    : g_cv.cur + CV_LIST_ROWS;
         if (g_cv.cur >= n)
            g_cv.cur = n - 1;
         if (g_cv.cur < g_cv.scroll)
            g_cv.scroll = g_cv.cur;
         if (g_cv.cur >= g_cv.scroll + CV_LIST_ROWS)
            g_cv.scroll = g_cv.cur - CV_LIST_ROWS + 1;
      }
      if (edges & PSP_CTRL_CIRCLE)
      {
         g_cv.page = CV_MENU;
         g_cv.cur = 0;
         cv_preview(NULL);
         return 0;
      }
      if ((edges & PSP_CTRL_CROSS) && n > 0)
      {
         snprintf(g_cv.sure_path, sizeof(g_cv.sure_path), "%s",
                  g_cv.pics[g_cv.cur].path);
         snprintf(g_cv.sure_from, sizeof(g_cv.sure_from), "picture");
         if (strcmp(g_cv.tex_path, g_cv.sure_path) || !g_cv.tex_ok)
            cv_preview(g_cv.sure_path);
         g_cv.page = CV_SURE;
         g_cv.prev_page = CV_PICK;
         edges = 0;
      }
      /* the preview follows the cursor once it rests (a decode a step would
       * stutter the list) */
      if (n > 0 && g_cv.page == CV_PICK)
      {
         if (strcmp(g_cv.tex_path, g_cv.pics[g_cv.cur].path))
         {
            if (++g_cv.tex_wait > 10)
               cv_preview(g_cv.pics[g_cv.cur].path);
         }
         else
            g_cv.tex_wait = 0;
      }
   }
   else /* CV_SURE */
   {
      if (edges & PSP_CTRL_CIRCLE)
      {
         g_cv.page = g_cv.prev_page;
         if (g_cv.page == CV_MENU)
         {
            g_cv.cur = 1;
            cv_preview(NULL);
         }
         return 0;
      }
      if (edges & PSP_CTRL_CROSS)
         cv_apply(g_cv.sure_path, g_cv.sure_from);
   }

   /* ---- drawing ---- */
   ov_backdrop(OV_SCRIM_DENSE);
   snprintf(nm, sizeof(nm), "%s", g_cv.name);
   clip_title(nm, 300);
   vw_head(nm, "COVER PICTURE");

   if (g_cv.page == CV_MENU)
   {
      static const char *const label[3] = { "Choose a picture...",
         "Use a screenshot of the game", "Automatic (download)" };
      const int own = cover_state(g_cv.stem);
      vid_text(22, 48, own ? "Now: your own picture" : "Now: automatic",
               own ? C_VALUE : C_DIM);
      for (i = 0; i < 3; i++)
      {
         int y = 80 + i * 30;
         const char *v = i == 2 && !own ? "in use" : "";
         vw_row_h(i, y, 270, 30, g_cv.cur == i,
                  !(i == 1 && !g_cv.have_shot), label[i], v);
      }
      vw_wrap(22, 178, 270, "A cover you pick always wins over downloaded "
              "art. A PNG or JPG in covers/ named like the game is used too.",
              C_DIM, 4);
      cv_preview_draw(CV_PREV_X, CV_PREV_Y, CV_PREV_W, CV_PREV_H);
      /* the plate sits where the footer goes: one or the other */
      if (g_cv.note[0])
         vw_plate(g_cv.note);
      else
         footer(VID_GLYPH_X " select   DPAD move   " VID_GLYPH_O " back");
   }
   else if (g_cv.page == CV_PICK)
   {
      if (g_cv.npics == 0)
      {
         vid_text(22, 56, "No pictures found.", C_ITEM);
         vw_wrap(22, 56 + FE_FONT_H + 6, 436, "Take a screenshot, or copy a "
                 "PNG or JPG into ux0:data/GBAdhoc/covers/ from a computer.",
                 C_DIM, 4);
      }
      else
      {
         char cnt[40];
         vid_clip(0, CV_LIST_TOP - 2, CV_PREV_X - 6, CV_LIST_ROWS * VW_PITCH + 4);
         for (i = g_cv.scroll; i < g_cv.npics && i < g_cv.scroll + CV_LIST_ROWS;
              i++)
         {
            char base[96], row[100];
            cv_file_name(g_cv.pics[i].path, base, sizeof(base));
            snprintf(row, sizeof(row), "%s", base);
            clip_title(row, CV_LIST_W - 20);
            vw_row_h(i, CV_LIST_TOP + (i - g_cv.scroll) * VW_PITCH, CV_LIST_W,
                     VW_PITCH, g_cv.cur == i, 1, row, NULL);
         }
         vid_clip_off();
         cv_preview_draw(CV_PREV_X, CV_PREV_Y, CV_PREV_W, CV_PREV_H - 28);
         snprintf(cnt, sizeof(cnt), "%d of %d", g_cv.cur + 1, g_cv.npics);
         vid_text(CV_PREV_X, CV_PREV_Y + CV_PREV_H - 22,
                  g_cv.pics[g_cv.cur].label, C_DIM);
         vid_text(CV_PREV_X, CV_PREV_Y + CV_PREV_H - 6, cnt, C_DIM);
      }
      footer(VID_GLYPH_X " look   DPAD move   L R page   " VID_GLYPH_O " back");
   }
   else
   {
      char q[140];
      cv_preview_draw(60, 50, 360, 150);
      snprintf(q, sizeof(q), "Use this as the cover for %s?", nm);
      clip_title(q, 440);
      vid_text_center(208, q, C_ITEM);
      footer(VID_GLYPH_X " use it   " VID_GLYPH_O " back");
   }
   return ret;
}

/* Runs the page until the player leaves it.  `rom`: a g_roms index. */
static void ui_cover_run(const char *rom_dir, int rom, int n_scan)
{
   const int stem_n = (int)ui_rom_stem_length(g_roms[rom].base);
   if (stem_n <= 0 || stem_n >= (int)sizeof(g_cv.stem))
      return;
   cover_init(g_dir_base);
   free(g_cv.pics);
   memset(&g_cv, 0, sizeof(g_cv));
   g_cv.rom = rom;
   g_cv.n_scan = n_scan;
   memcpy(g_cv.stem, g_roms[rom].base, (size_t)stem_n);
   rom_display_name(&g_roms[rom], g_cv.name, sizeof(g_cv.name), 1);
   cv_find_shot(rom_dir);
   browse_backdrop_take();
   g_ov_browse = 1;
   cv_preview(NULL);
   fe_evt("ui_screen name=cover from=browser rom=%s state=%d shot=%d",
          g_roms[rom].name, cover_state(g_cv.stem), g_cv.have_shot);
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
      if (cv_screen(edges, rom_dir))
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
   cv_tex_free();
   free(g_cv.pics);
   g_cv.pics = NULL;
   g_ov_browse = 0;
   vid_ambient_drop();
   g_prev_pad = 0xFFFFFFFFu;
}

/* From the game list (the Home's Cover button). */
static void ui_cover_enter(const char *rom_dir, int rom, int n_scan)
{
   md_pause();
   if (g_ui_home)
      home_modal_enter();
   ui_cover_run(rom_dir, rom, n_scan);
   if (g_ui_home)
      home_modal_leave(1);
   md_resume();
}

/* Settings > Cover picture's value for a game: 1 automatic, 2 its own. */
static int ui_cover_row(int rom)
{
   char stem[256];
   size_t sl = ui_rom_stem_length(g_roms[rom].base);
   if (!sl || sl >= sizeof(stem))
      return 0;
   memcpy(stem, g_roms[rom].base, sl);
   stem[sl] = '\0';
   cover_init(g_dir_base);
   return cover_state(stem) ? 2 : 1;
}

/* ---- covers/: the inbox, at the list's opening and at a refresh ------------------------------ */

static void ui_cover_inbox_scan(int n_scan)
{
   char (*stems)[256];
   const char **p;
   int i, n = 0, waiting;
   cover_init(g_dir_base);
   {
      /* covers/ is there for the player to copy pictures into */
      char dir[PSP_FILE_PATH_CAP];
      if (snprintf(dir, sizeof(dir), "%s/%s", g_dir_base,
                   COVER_INBOX_DIR) < (int)sizeof(dir))
         sceIoMkdir(dir, 0777);
   }
   if (n_scan <= 0)
      return;
   stems = (char (*)[256])malloc((size_t)n_scan * sizeof(*stems));
   p = (const char **)malloc((size_t)n_scan * sizeof(*p));
   if (stems && p)
   {
      for (i = 0; i < n_scan; i++)
      {
         size_t sl = ui_rom_stem_length(g_roms[i].base);
         if (!sl || sl >= sizeof(stems[0]))
            continue;
         memcpy(stems[n], g_roms[i].base, sl);
         stems[n][sl] = '\0';
         p[n] = stems[n];
         n++;
      }
      waiting = cover_inbox_scan(p, n);
      if (waiting)
         fe_evt("cover_inbox waiting=%d games=%d", waiting, n);
   }
   free(stems);
   free(p);
}

/* One waiting file a frame, while the list rests. */
static void ui_cover_pump(int n_scan, int idle)
{
   char stem[256];
   int rc, i;
   if (idle < 3 || !cover_inbox_pending())
      return;
   rc = cover_inbox_step(stem, sizeof(stem));
   if (rc == 0)
      return;
   fe_evt("cover_inbox stem=%s rc=%d left=%d", stem, rc, cover_inbox_pending());
   for (i = 0; i < n_scan && i < BROWSER_MAX; i++)
   {
      size_t sl = ui_rom_stem_length(g_roms[i].base);
      if (sl == strlen(stem) && !strncmp(g_roms[i].base, stem, sl))
         hm_art_arrived(g_roms[i].base, n_scan);
   }
}

#endif /* UI_VITA_COVER_H */
