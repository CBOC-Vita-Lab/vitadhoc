/* ui_vita_home.h -- VitAdhoc's home screen: the game list as a row of
 * tiles over the focused game's own picture.
 *
 * Included by psp/ui_psp.c under -DVITA after ui_vita_browser.h: it draws
 * with the browser's state (g_roms, the view, favourites, the state shelf)
 * and vstyle.h's native-pixel primitives.  It is the browser's third shell
 * (g_ui_shell == UI_SHELL_HOME, the Vita's default); the browser loop keeps
 * every rule of input it has, and home_remap() only turns the pad's
 * directions round for a row that runs sideways:
 *
 *   LEFT / RIGHT    the previous / next game          (the list's UP / DOWN)
 *   DOWN            the focused game's save states    (the shelf's LEFT)
 *   in the states:  LEFT / RIGHT the slot, UP back    (UP / DOWN, CIRCLE)
 *
 * WHAT IS ON SCREEN, top to bottom
 * BOX ART comes by itself: the cartridges of the list are handed to
 * artfetch.h, which fetches what boxart/ lacks from the libretro thumbnail
 * server over Wi-Fi; a picture that arrives replaces the tile's card or
 * scene (hm_art_arrived).
 *
 *   a top bar       Games / Starred (SELECT), the console (TRIANGLE), the
 *                   time and the battery
 *   the row         the focused game large with its name beside it, the
 *                   next ones small; a tile is the game's box art, else the
 *                   last picture of it you saw (scenes/<game>.shot, written
 *                   when a game is left), else a card in the console's colour
 *   behind it all   the focused game's picture, large: hero/ art when there
 *                   is some, else that last picture -- "where you left off"
 *                   -- and, when the row rests on the game a second, the
 *                   last ten seconds you played of it, moving
 *                   (clips/<game>.clip, clip_vita.h), fading back into the
 *                   still at its end and starting again a moment later
 *   the name        large, with what is known about the game as chips --
 *                   and, for a game Wikipedia has an article about, its
 *                   first lines (gamedb.h: bundled, no network needed)
 *   Play, Save states, the star; the buttons' hints
 *   the states      DOWN lifts the page and shows the five slots as cards
 */
#ifndef UI_VITA_HOME_H
#define UI_VITA_HOME_H

#include "ui_vita_style.h"
#include "clip_vita.h"
#include "gamedb.h"
#include "artfetch.h"

#define HM_X        44
#define HM_ROW_Y    86
#define HM_BIG      132
#define HM_SMALL    96
#define HM_GAP      14
#define HM_TEX      160          /* a tile's texture edge                    */
#define HM_TILES    16           /* tiles kept                               */
#define HM_TITLE_Y  300
#define HM_BTN_Y    448
#define HM_BLURB_LIFT 60         /* the name goes up for a game's lines     */
#define HM_BLURB_W  600          /* ...which are this wide, three at most   */
#define HM_LIFT     272          /* how far DOWN lifts the page              */

/* ---- pictures --------------------------------------------------------- */

/* A .shot (state_slots.h: 16-byte header, then rows of PSP-order 565 at a
 * stride of 256) into RGBA.  0 = read. */
static int hm_shot_read(const char *path, uint32_t *rgba, unsigned *w,
                        unsigned *h)
{
   unsigned char hdr[PSP_STATE_THUMB_HEADER_SIZE];
   uint16_t row[PSP_STATE_SHOT_STRIDE];
   unsigned x, y;
   SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
   int ok;
   if (fd < 0)
      return -1;
   ok = sceIoRead(fd, hdr, sizeof(hdr)) == (int)sizeof(hdr) &&
        psp_state_shot_check(hdr, w, h) == 0;
   for (y = 0; ok && y < *h; y++)
   {
      ok = sceIoRead(fd, row, sizeof(row)) == (int)sizeof(row);
      for (x = 0; ok && x < *w; x++)
      {
         unsigned v = row[x];
         unsigned r = v & 31, g = (v >> 5) & 63, b = (v >> 11) & 31;
         rgba[y * *w + x] = 0xFF000000u | (((b << 3) | (b >> 2)) << 16) |
                            (((g << 2) | (g >> 4)) << 8) | ((r << 3) | (r >> 2));
      }
   }
   sceIoClose(fd);
   return ok ? 0 : -1;
}

/* The scene file of a game: scenes/<stem>.shot in the data folder. */
static int hm_scene_path(int rom, char *out, size_t cap)
{
   extern char g_dir_base[];
   const char *base = g_roms[rom].base;
   int n = snprintf(out, cap, "%s/scenes/%.*s.shot", g_dir_base,
                    (int)ui_rom_stem_length(base), base);
   return n > 0 && (size_t)n < cap ? 0 : -1;
}

/* A title screen artfetch.h fetched: titles/<stem>.png, decoded into `rgba`
 * (PSP_STATE_SHOT_MAX_W x _H at most: a larger one is scaled down to fit,
 * nearest, keeping its shape) in the scene's layout.  For a game never
 * played -- no scene yet -- so its tile and the picture behind the row are
 * the game's own title screen rather than a plain card. */
static int hm_title_read(int rom, uint32_t *rgba, unsigned *ow, unsigned *oh)
{
   extern char g_dir_base[];
   const char *base = g_roms[rom].base;
   char path[PSP_FILE_PATH_CAP];
   unsigned char *file, *pix;
   int flen = 0, w = 0, h = 0, nc = 0, tw, th, x, y;
   if (snprintf(path, sizeof(path), "%s/titles/%.*s.png", g_dir_base,
                (int)ui_rom_stem_length(base), base) >= (int)sizeof(path))
      return -1;
   file = art_slurp(path, &flen);
   if (!file)
      return -1;
   if (!stbi_info_from_memory(file, flen, &w, &h, &nc) || w <= 0 || h <= 0 ||
       w > 2048 || h > 2048)
   {
      free(file);
      return -1;
   }
   pix = stbi_load_from_memory(file, flen, &w, &h, &nc, 4);
   free(file);
   if (!pix)
      return -1;
   tw = w;
   th = h;
   if (tw > PSP_STATE_SHOT_MAX_W || th > PSP_STATE_SHOT_MAX_H)
   {
      if ((long)w * PSP_STATE_SHOT_MAX_H > (long)h * PSP_STATE_SHOT_MAX_W)
      {
         tw = PSP_STATE_SHOT_MAX_W;
         th = (int)((long)h * tw / w);
      }
      else
      {
         th = PSP_STATE_SHOT_MAX_H;
         tw = (int)((long)w * th / h);
      }
      if (tw < 1) tw = 1;
      if (th < 1) th = 1;
   }
   for (y = 0; y < th; y++)
      for (x = 0; x < tw; x++)
      {
         const unsigned char *p = pix + 4 * ((size_t)(y * h / th) * w +
                                             (size_t)(x * w / tw));
         rgba[y * tw + x] = 0xFF000000u | ((uint32_t)p[2] << 16) |
                            ((uint32_t)p[1] << 8) | p[0];
      }
   stbi_image_free(pix);
   *ow = (unsigned)tw;
   *oh = (unsigned)th;
   return 0;
}

/* Round the corners of a square RGBA picture, `r` px, by coverage. */
static void hm_round(uint32_t *px, int edge, int r)
{
   int x, y, c;
   for (c = 0; c < 4; c++)
      for (y = 0; y < r; y++)
         for (x = 0; x < r; x++)
         {
            int sx, sy, hits = 0;
            for (sy = 0; sy < 4; sy++)
               for (sx = 0; sx < 4; sx++)
               {
                  float dx = (float)r - ((float)x + ((float)sx + 0.5f) / 4.0f);
                  float dy = (float)r - ((float)y + ((float)sy + 0.5f) / 4.0f);
                  if (dx * dx + dy * dy <= (float)(r * r))
                     hits++;
               }
            {
               int px_x = (c & 1) ? edge - 1 - x : x;
               int px_y = (c & 2) ? edge - 1 - y : y;
               uint32_t *p = &px[px_y * edge + px_x];
               unsigned a = (*p >> 24) * (unsigned)hits / 16u;
               *p = (*p & 0x00FFFFFFu) | (a << 24);
            }
         }
}

/* Cover-fit a src picture (RGBA, sw x sh) into a square edge x edge:
 * the centre crop, nearest (pixel art stays pixel art) when it is being
 * enlarged, box-averaged when reduced. */
static void hm_square(const uint32_t *src, int sw, int sh, uint32_t *dst,
                      int edge)
{
   int s = sw < sh ? sw : sh, x0 = (sw - s) / 2, y0 = (sh - s) / 2, x, y;
   for (y = 0; y < edge; y++)
      for (x = 0; x < edge; x++)
      {
         if (s >= edge * 2)
         {
            /* reduce: average the source block */
            int ax = x0 + x * s / edge, bx = x0 + (x + 1) * s / edge;
            int ay = y0 + y * s / edge, by = y0 + (y + 1) * s / edge;
            unsigned r = 0, g = 0, b = 0, n = 0;
            int u, v;
            for (v = ay; v < by; v++)
               for (u = ax; u < bx; u++)
               {
                  uint32_t c = src[v * sw + u];
                  r += c & 0xFF; g += (c >> 8) & 0xFF; b += (c >> 16) & 0xFF;
                  n++;
               }
            if (!n) n = 1;
            dst[y * edge + x] = 0xFF000000u | ((b / n) << 16) |
                                ((g / n) << 8) | (r / n);
         }
         else
            dst[y * edge + x] = src[(y0 + y * s / edge) * sw + x0 +
                                    x * s / edge];
      }
}

/* PSP-order 565 (a UI texture) -> RGBA, w x h at `stride` texels. */
static void hm_from565(const uint16_t *src, int stride, int w, int h,
                       uint32_t *dst)
{
   int x, y;
   for (y = 0; y < h; y++)
      for (x = 0; x < w; x++)
      {
         unsigned v = src[y * stride + x];
         unsigned r = v & 31, g = (v >> 5) & 63, b = (v >> 11) & 31;
         dst[y * w + x] = 0xFF000000u | (((b << 3) | (b >> 2)) << 16) |
                          (((g << 2) | (g >> 4)) << 8) | ((r << 3) | (r >> 2));
      }
}

/* ---- tiles -------------------------------------------------------------- */

typedef struct
{
   int rom;            /* g_roms index, -1 = free                          */
   int state;          /* 0 = not looked at, 1 = picture, -1 = none        */
   vs_img *img;
   float x, s;         /* where it is drawn now (eased), native px         */
   int seen;           /* the frame it was last drawn                      */
} hm_tile;

static hm_tile g_hm_tile[HM_TILES];
static unsigned g_hm_frame;
static uint32_t *g_hm_buf;          /* 240 x 160 RGBA scratch              */
static uint32_t *g_hm_sq;           /* HM_TEX^2 RGBA scratch               */

static int hm_scratch(void)
{
   if (!g_hm_buf)
      g_hm_buf = (uint32_t *)malloc(PSP_STATE_SHOT_MAX_W *
                                    PSP_STATE_SHOT_MAX_H * 4);
   if (!g_hm_sq)
      g_hm_sq = (uint32_t *)malloc(HM_TEX * HM_TEX * 4);
   return g_hm_buf && g_hm_sq ? 0 : -1;
}

static hm_tile *hm_tile_find(int rom)
{
   int i, oldest = 0;
   for (i = 0; i < HM_TILES; i++)
      if (g_hm_tile[i].rom == rom)
         return &g_hm_tile[i];
   for (i = 0; i < HM_TILES; i++)
   {
      if (g_hm_tile[i].rom < 0)
      {
         oldest = i;
         break;
      }
      if (g_hm_tile[i].seen < g_hm_tile[oldest].seen)
         oldest = i;
   }
   if (g_hm_tile[oldest].img)
      vs_img_free(g_hm_tile[oldest].img);
   memset(&g_hm_tile[oldest], 0, sizeof(g_hm_tile[oldest]));
   g_hm_tile[oldest].rom = rom;
   g_hm_tile[oldest].x = -1.0f;
   return &g_hm_tile[oldest];
}

/* Make the tile's picture: box art, else the scene.  One per frame. */
static void hm_tile_load(const char *rom_dir, hm_tile *t)
{
   char path[PSP_FILE_PATH_CAP];
   unsigned w = 0, h = 0;
   art_slot a;
   const uint16_t *art;
   t->state = -1;
   if (hm_scratch() != 0)
      return;
   a.aw = ART_PX_W; a.ah = ART_PX_H;
   art = art_get(rom_dir, t->rom, 1, &a);
   if (art && a.aw > 0 && a.ah > 0 && a.aw <= ART_TEX_W && a.ah <= ART_TEX_H)
   {
      uint32_t *big = (uint32_t *)malloc((size_t)a.aw * a.ah * 4);
      if (big)
      {
         hm_from565(art, ART_TEX_W, a.aw, a.ah, big);
         hm_square(big, a.aw, a.ah, g_hm_sq, HM_TEX);
         free(big);
         t->state = 1;
      }
   }
   else if ((hm_scene_path(t->rom, path, sizeof(path)) == 0 &&
             hm_shot_read(path, g_hm_buf, &w, &h) == 0) ||
            hm_title_read(t->rom, g_hm_buf, &w, &h) == 0)
   {
      hm_square(g_hm_buf, (int)w, (int)h, g_hm_sq, HM_TEX);
      t->state = 1;
   }
   if (t->state == 1)
   {
      hm_round(g_hm_sq, HM_TEX, 20);
      t->img = vs_img_new(HM_TEX, HM_TEX);
      if (t->img)
         vs_img_set(t->img, g_hm_sq, HM_TEX);
      else
         t->state = -1;
   }
}

/* A game with no picture: a card in its console's colour, a cartridge and
 * its name. */
static void hm_card(float x, float y, float s, int rom, uint32_t a)
{
   char name[64];
   int con = g_pcfg.console;
   vs_rrect_grad_v(x, y, s, s, s * 0.125f, VS_ALPHA(hm_deep(con, 0), a),
                   VS_ALPHA(hm_deep(con, 1), a));
   vs_icon(VS_I_CART, x + s * 0.5f, y + s * 0.37f, s * 0.36f,
           VS_HEX(0xFFFFFF, (a * 200) / 255));
   rom_display_name(&g_roms[rom], name, sizeof(name), 1);
   vs_text_fit(VS_CAPTION, name, s - (s >= 110 ? 20 : 12));
   vs_text_c(VS_CAPTION, x + s * 0.5f, y + s * 0.65f, name,
             VS_HEX(0xFFFFFF, (a * 230) / 255));
}

/* ---- the focused game's picture ---------------------------------------- */

static struct
{
   int rom;            /* whose picture is loaded, -1 none                  */
   int state;          /* 1 = scene in img, 2 = hero art (g_hero), -1 none  */
   vs_img *img;
   int w, h;
   float a;            /* fade, 0..255                                      */
   /* the one before, fading out */
   vs_img *old;
   int old_w, old_h;
   float old_a;
} g_hm_pic = { -1, 0, NULL, 0, 0, 0.0f, NULL, 0, 0, 0.0f };

static void hm_pic_load(int rom)
{
   char path[PSP_FILE_PATH_CAP];
   unsigned w = 0, h = 0;
   /* the picture on screen now becomes the one fading out */
   if (g_hm_pic.old)
      vs_img_free(g_hm_pic.old);
   g_hm_pic.old = g_hm_pic.state == 1 ? g_hm_pic.img : NULL;
   g_hm_pic.old_w = g_hm_pic.w;
   g_hm_pic.old_h = g_hm_pic.h;
   g_hm_pic.old_a = g_hm_pic.state == 1 ? g_hm_pic.a : 0.0f;
   if (g_hm_pic.state != 1 && g_hm_pic.img)
      vs_img_free(g_hm_pic.img);
   g_hm_pic.img = NULL;
   g_hm_pic.rom = rom;
   g_hm_pic.state = -1;
   g_hm_pic.a = 0.0f;
   if (hero_get(rom, 1) && g_hero_precomposed)
   {
      g_hm_pic.state = 2;         /* drawn from the browser's own hero */
      return;
   }
   if (hm_scratch() == 0 &&
       ((hm_scene_path(rom, path, sizeof(path)) == 0 &&
         hm_shot_read(path, g_hm_buf, &w, &h) == 0) ||
        hm_title_read(rom, g_hm_buf, &w, &h) == 0))
   {
      g_hm_pic.img = vs_img_new((int)w, (int)h);
      if (g_hm_pic.img)
      {
         vs_img_set(g_hm_pic.img, g_hm_buf, (int)w);
         g_hm_pic.w = (int)w;
         g_hm_pic.h = (int)h;
         g_hm_pic.state = 1;
      }
   }
}

/* ---- the preview: the last ten seconds played -------------------------- */

#define HM_CLIP_WAIT   48       /* frames resting on a game after it is
                                 * selected (IDLE_SELECTED) before its clip
                                 * starts: about a second in all            */
#define HM_CLIP_REST   2500000u /* us of the still between two plays        */
#define HM_CLIP_READ   (128u * 1024u)   /* bytes of the file read a frame   */
#define HM_CLIP_TEX    3        /* textures in turn: the GPU may still be
                                 * drawing the one before last              */

enum { HC_OFF = 0, HC_PLAY, HC_OUT, HC_REST };

static struct
{
   int rom;                 /* whose clip is open, -1 none                  */
   int none_for;            /* a game known to have none (this visit)       */
   clip_player *p;
   vs_img *tex[HM_CLIP_TEX];
   int shown;               /* the texture on screen, -1 none yet           */
   int phase;
   float a;                 /* fade, 0..255                                 */
   const uint16_t *next;    /* decoded, waiting for its time                */
   unsigned next_ticks;
   unsigned acc_us;         /* time since the frame on screen came          */
   unsigned last_us;
   unsigned rest_us;
   int ended;
} g_hm_clip = { -1, -1, NULL, { NULL, NULL, NULL }, -1, HC_OFF, 0.0f, NULL,
                0, 0, 0, 0, 0 };

static int hm_clip_path(int rom, char *out, size_t cap)
{
   extern char g_dir_base[];
   const char *base = g_roms[rom].base;
   int n = snprintf(out, cap, "%s/clips/%.*s.clip", g_dir_base,
                    (int)ui_rom_stem_length(base), base);
   return n > 0 && (size_t)n < cap ? 0 : -1;
}

static void hm_clip_drop(void)
{
   int i;
   clip_close(g_hm_clip.p);
   for (i = 0; i < HM_CLIP_TEX; i++)
      if (g_hm_clip.tex[i])
         vs_img_free(g_hm_clip.tex[i]);
   memset(&g_hm_clip.tex, 0, sizeof(g_hm_clip.tex));
   g_hm_clip.p = NULL;
   g_hm_clip.rom = -1;
   g_hm_clip.shown = -1;
   g_hm_clip.phase = HC_OFF;
   g_hm_clip.a = 0.0f;
   g_hm_clip.next = NULL;
   g_hm_clip.ended = 0;
}

static void hm_clip_open(int rom)
{
   char path[PSP_FILE_PATH_CAP];
   int i;
   hm_clip_drop();
   if (hm_clip_path(rom, path, sizeof(path)) != 0 ||
       !(g_hm_clip.p = clip_open(path)))
   {
      g_hm_clip.none_for = rom;
      return;
   }
   for (i = 0; i < HM_CLIP_TEX; i++)
      if (!(g_hm_clip.tex[i] = vs_img_new_565((int)clip_w(g_hm_clip.p),
                                              (int)clip_h(g_hm_clip.p))))
      {
         hm_clip_drop();
         g_hm_clip.none_for = rom;
         return;
      }
   g_hm_clip.rom = rom;
   g_hm_clip.phase = HC_PLAY;
   g_hm_clip.acc_us = 0;
   g_hm_clip.last_us = sceKernelGetSystemTimeLow();
   fe_evt("home_clip open rom=%d frames=%u w=%u h=%u", rom,
          clip_frames(g_hm_clip.p), clip_w(g_hm_clip.p), clip_h(g_hm_clip.p));
}

/* Once a frame of the home screen: open, read, decode, time, fade. */
static void hm_clip_tick(int rom, int idle)
{
   unsigned now, dt, us;
   if (!g_pcfg.previews)
   {
      if (g_hm_clip.p)
         hm_clip_drop();
      return;
   }
   if (g_hm_clip.rom != rom)
   {
      /* another game: the one before stops at once (its picture is
       * already fading out with the still) */
      if (g_hm_clip.p)
         hm_clip_drop();
      if (idle >= IDLE_SELECTED + HM_CLIP_WAIT && g_hm_clip.none_for != rom)
         hm_clip_open(rom);
      if (!g_hm_clip.p)
         return;
   }
   now = sceKernelGetSystemTimeLow();
   dt = now - g_hm_clip.last_us;
   g_hm_clip.last_us = now;
   if (dt > 100000)
      dt = 100000;              /* a stall is not a reason to skip ahead */
   if (clip_load(g_hm_clip.p, HM_CLIP_READ) < 0)
   {
      fe_evt("home_clip unreadable rom=%d", rom);
      hm_clip_drop();
      g_hm_clip.none_for = rom;
      return;
   }
   us = clip_us_per_tick(g_hm_clip.p);

   switch (g_hm_clip.phase)
   {
   case HC_PLAY:
      if (!g_hm_clip.next && !g_hm_clip.ended)
         g_hm_clip.next = clip_next(g_hm_clip.p, &g_hm_clip.next_ticks,
                                    &g_hm_clip.ended);
      g_hm_clip.acc_us += dt;
      if (g_hm_clip.next &&
          (g_hm_clip.shown < 0 ||
           g_hm_clip.acc_us >= g_hm_clip.next_ticks * us))
      {
         int t = (g_hm_clip.shown + 1) % HM_CLIP_TEX;
         vs_img_set_565(g_hm_clip.tex[t], g_hm_clip.next,
                        (int)clip_w(g_hm_clip.p));
         if (g_hm_clip.shown < 0 || g_hm_clip.acc_us > 4 * 2 * us)
            g_hm_clip.acc_us = 0;     /* the first frame, or a late one */
         else
            g_hm_clip.acc_us -= g_hm_clip.next_ticks * us;
         g_hm_clip.shown = t;
         g_hm_clip.next = NULL;
      }
      else if (!g_hm_clip.next && g_hm_clip.acc_us > 4 * 2 * us)
         g_hm_clip.acc_us = 4 * 2 * us;  /* still being read in */
      if (g_hm_clip.shown >= 0)
         hm_ease(&g_hm_clip.a, 255.0f, 0.12f);
      if (g_hm_clip.ended && !g_hm_clip.next)
      {
         if (g_hm_clip.shown < 0)
         {
            /* broken from the first frame */
            hm_clip_drop();
            g_hm_clip.none_for = rom;
            return;
         }
         g_hm_clip.phase = HC_OUT;
      }
      break;
   case HC_OUT:
      /* the last frame, which is the still behind it, gives way to it */
      if (!hm_ease(&g_hm_clip.a, 0.0f, 0.12f))
      {
         g_hm_clip.phase = HC_REST;
         g_hm_clip.rest_us = 0;
      }
      break;
   case HC_REST:
      g_hm_clip.rest_us += dt;
      if (g_hm_clip.rest_us >= HM_CLIP_REST)
      {
         clip_restart(g_hm_clip.p);
         g_hm_clip.ended = 0;
         g_hm_clip.next = NULL;
         g_hm_clip.shown = -1;
         g_hm_clip.acc_us = 0;
         g_hm_clip.phase = HC_PLAY;
      }
      break;
   default:
      break;
   }
}

/* A scene behind the page: four times its size (the 3x picture's own
 * crispness, a little larger), against the right edge, then veiled where
 * the text sits. */
static void hm_scene_draw(vs_img *im, int w, int h, float a, float dy)
{
   float k = w <= 160 ? 4.0f : 4.0f, dw = (float)w * k, dh = (float)h * k;
   float x = VS_W - dw + 120.0f, y = (VS_H - dh) * 0.5f - 10.0f + dy * 0.35f;
   if (!im || a <= 0.5f)
      return;
   vs_img_draw(im, x, y, dw, dh, VS_HEX(0xFFFFFF, (int)a), 0);
}

static void hm_backdrop(int rom, float dy)
{
   int con = g_pcfg.console;
   vs_rect(0, 0, VS_W, VS_H, HC_BG);
   /* the console's glow, always: under a picture it is barely there, with
    * none it is the page */
   vs_shadow(560, 60 + dy * 0.3f, 520, 420, 210, 180, hm_tint(con, 46));
   if (!(g_hm_pic.rom == rom && g_hm_pic.state > 0))
   {
      vs_shadow(600, 110 + dy * 0.3f, 300, 300, 150, 150, hm_tint(con, 40));
      vs_icon(VS_I_CART, 760, 250 + dy * 0.3f, 300, VS_HEX(0xFFFFFF, 14));
   }
   if (g_hm_pic.old && g_hm_pic.old_a > 0.5f)
      hm_scene_draw(g_hm_pic.old, g_hm_pic.old_w, g_hm_pic.old_h,
                    g_hm_pic.old_a, dy);
   if (g_hm_pic.rom == rom && g_hm_pic.state == 1)
      hm_scene_draw(g_hm_pic.img, g_hm_pic.w, g_hm_pic.h, g_hm_pic.a, dy);
   else if (g_hm_pic.rom == rom && g_hm_pic.state == 2 && g_hero)
   {
      int bh = (int)((long)480 * g_hero_h / (g_hero_w ? g_hero_w : 1));
      vid_image(0, (272 - bh) / 2, 480, bh, g_hero, g_hero_tex, g_hero_tex,
                g_hero_w, g_hero_h, (int)g_hm_pic.a);
   }
   /* the preview, over the still it ends on */
   if (g_hm_clip.rom == rom && g_hm_clip.shown >= 0)
      hm_scene_draw(g_hm_clip.tex[g_hm_clip.shown], (int)clip_w(g_hm_clip.p),
                    (int)clip_h(g_hm_clip.p), g_hm_clip.a, dy);
   /* veils: dark under the words on the left, under the row at the top and
    * under the buttons at the bottom -- lifted a little on the right while
    * a preview plays, so that it comes forward */
   {
      float k = g_hm_clip.rom == rom && g_hm_clip.shown >= 0
                ? g_hm_clip.a / 255.0f : 0.0f;
      vs_grad_h(0, 0, VS_W, VS_H, VS_HEX(0x060914, 250),
                VS_HEX(0x060914, (int)(40.0f - 34.0f * k)));
      vs_grad_v(0, 0, VS_W, 250, VS_HEX(0x060914, (int)(236.0f - 40.0f * k)),
                VS_HEX(0x060914, 0));
   }
   vs_grad_v(0, 290, VS_W, VS_H - 290, VS_HEX(0x060914, 0),
             VS_HEX(0x060914, 246));
}

/* ---- the top bar -------------------------------------------------------- */

static float hm_topbar(float dy, int n_scan)
{
   static const char *const CON[FE_CONSOLE_COUNT] = { "GBA", "GB", "GBC" };
   float x = HM_X, y = 18 + dy, cx;
   int c;
   (void)n_scan;

   /* Games | Starred: SELECT */
   vs_text(VS_SUB, x, y, "Games", g_browser_favs ? HC_TEXT3 : HC_TEXT);
   UI_TCH_BTN(g_browser_favs ? PSP_CTRL_SELECT : 0, (int)(x / 2) - 4,
              (int)(y / 2), (int)(vs_text_w(VS_SUB, "Games") / 2) + 8, 22);
   x += vs_text_w(VS_SUB, "Games") + 28;
   vs_text(VS_SUB, x, y, "Starred", g_browser_favs ? HC_TEXT : HC_TEXT3);
   UI_TCH_BTN(g_browser_favs ? 0 : PSP_CTRL_SELECT, (int)(x / 2) - 4,
              (int)(y / 2), (int)(vs_text_w(VS_SUB, "Starred") / 2) + 8, 22);
   x += vs_text_w(VS_SUB, "Starred") + 34;

   /* the consoles: TRIANGLE steps through them */
   cx = x;
   for (c = 0; c < FE_CONSOLE_COUNT; c++)
   {
      float w = vs_text_w(VS_LABEL, CON[c]) + 26;
      if (c == g_pcfg.console)
      {
         vs_rrect(x, y + 2, w, 34, 17, HC_GLASS_HI);
         vs_text(VS_LABEL, x + 13, y + 7, CON[c], HC_TEXT);
      }
      else
         vs_text(VS_LABEL, x + 13, y + 7, CON[c], HC_TEXT3);
      x += w + 4;
   }
   vs_icon(VS_I_BTN_TRI, x + 12, y + 19, 20, HC_TEXT3);
   UI_TCH_BTN(PSP_CTRL_TRIANGLE, (int)(cx / 2) - 2, (int)(y / 2),
              (int)((x + 26 - cx) / 2) + 4, 22);

   /* the right: settings, the time, the battery */
   x = hm_status(VS_W - HM_X, y);
   vs_icon(VS_I_GEAR, x - 30, y + 19, 26, HC_TEXT2);
   VS_TCH_BTN(PSP_CTRL_START, x - 50, y, 44, 44);
   return y;
}

/* ---- the row ------------------------------------------------------------ */

static float g_hm_label_a;          /* the focused name's fade               */
static int g_hm_label_for = -1;

static void hm_row(const char *rom_dir, int cur, int n, float dy, int idle)
{
   int k, first = cur > 0 ? -1 : 0, loaded = 0;
   float tx = HM_X;
   char name[64];
   int rom_cur = view_rom(cur);

   if (g_hm_label_for != rom_cur)
   {
      g_hm_label_for = rom_cur;
      g_hm_label_a = 0.0f;
   }
   hm_ease(&g_hm_label_a, 255.0f, 0.25f);

   /* the targets: the focused tile at HM_X, its name after it, then the
    * rest; one before it, leaving to the left */
   for (k = first; k < 10 && (n <= 10 ? k < n : 1); k++)
   {
      int vi = cur + k, rom;
      float s = k == 0 ? HM_BIG : HM_SMALL, target;
      hm_tile *t;
      if (n > 10)
         vi = ((vi % n) + n) % n;
      else if (vi < 0 || vi >= n)
         continue;
      rom = view_rom(vi);
      if (k < 0)
         target = HM_X - HM_GAP - HM_SMALL - 40;
      else
         target = tx;
      t = hm_tile_find(rom);
      t->seen = (int)g_hm_frame;
      if (t->x < -0.5f)
      {
         t->x = target;
         t->s = s;
      }
      hm_ease(&t->x, target, 0.28f);
      hm_ease(&t->s, s, 0.28f);
      if (!t->state && !loaded && idle >= 3 && (k == 0 || idle >= 8))
      {
         hm_tile_load(rom_dir, t);
         loaded = 1;
      }
      {
         float y = HM_ROW_Y + dy + (HM_BIG - t->s) * 0.5f;
         float x = t->x;
         int a = k < 0 ? 110 : 255;
         if (x > VS_W)
            break;
         if (k == 0)
         {
            vs_shadow(x - 6, y - 6, t->s + 12, t->s + 12, 22, 16,
                      VS_HEX(0xFFFFFF, 54));
         }
         if (t->state == 1 && t->img)
         {
            vs_shadow(x, y + 6, t->s, t->s, t->s * 0.125f, 10,
                      VS_HEX(0x000000, 90));
            vs_img_draw(t->img, x, y, t->s, t->s, VS_HEX(0xFFFFFF, a), 1);
         }
         else
            hm_card(x, y, t->s, rom, (uint32_t)a);
         if (g_roms[rom].is_fav)
         {
            vs_circle(x + t->s - 16, y + 16, 13, VS_HEX(0x060914, 170));
            vs_icon(VS_I_STAR, x + t->s - 16, y + 16, 16,
                    VS_ALPHA(hm_tint(g_pcfg.console, 255), 255));
         }
         if (k == 0)
            vs_rrect_line(x - 6, y - 6, t->s + 12, t->s + 12, 22, 3,
                          HC_TEXT);
         /* the tile: a tap selects it, the focused one plays */
         if (k >= 0)
            UI_TCH_ROW(vi, (int)(x / 2), (int)(y / 2), (int)(t->s / 2) + 2,
                       (int)(t->s / 2));
      }
      if (k == 0)
      {
         float lw;
         rom_display_name(&g_roms[rom], name, sizeof(name), 1);
         vs_text_fit(VS_SUB, name, 300);
         lw = vs_text_w(VS_SUB, name);
         {
            const char *con = skin_of(g_pcfg.console)->full;
            float cw = vs_text_w(VS_BODY, con);
            int la = (int)g_hm_label_a;
            float ly = HM_ROW_Y + dy + HM_BIG * 0.5f;
            vs_text(VS_SUB, t->x + t->s + 24, ly - 40, name,
                    VS_HEX(0xF5F7FC, la));
            vs_text(VS_BODY, t->x + t->s + 24, ly + 4, con,
                    VS_HEX(0xA7B0C6, la));
            if (cw > lw)
               lw = cw;
         }
         tx = t->x + t->s + 24 + lw + 36;
      }
      else if (k > 0)
         tx += HM_SMALL + HM_GAP;
   }
}

/* ---- the focused game --------------------------------------------------- */

static int g_hm_states_for = -1;      /* the game the counts below are for */
static int g_hm_states_n;             /* how many slots hold a state       */

/* The focused game's lines from gamedb.h, looked up once per game: by its
 * file name, else (GBA) by the No-Intro name of its header's code. */
static int g_hm_blurb_for = -1;
static char g_hm_blurb[400];

static const char *hm_blurb(const char *rom_dir, int rom)
{
   if (g_hm_blurb_for != rom)
   {
      char stem[PSP_FILE_PATH_CAP], name[256];
      size_t n = ui_rom_stem_length(g_roms[rom].base);
      g_hm_blurb_for = rom;
      g_hm_blurb[0] = '\0';
      if (n >= sizeof(stem))
         n = sizeof(stem) - 1;
      memcpy(stem, g_roms[rom].base, n);
      stem[n] = '\0';
      if (!gamedb_blurb(stem, g_hm_blurb, sizeof(g_hm_blurb)))
      {
         char path[PSP_FILE_PATH_CAP];
         if (psp_rom_path_join(path, sizeof(path), rom_dir,
                               g_roms[rom].name) == 0 &&
             gamedb_name_by_header(path, name, sizeof(name)))
            gamedb_blurb(name, g_hm_blurb, sizeof(g_hm_blurb));
      }
   }
   return g_hm_blurb;
}

static void hm_focus(const char *rom_dir, int rom, float dy)
{
   char title[80], buf[48];
   const char *blurb = hm_blurb(rom_dir, rom);
   float x, y = HM_TITLE_Y + dy - (blurb[0] ? HM_BLURB_LIFT : 0);
   int la = (int)g_hm_label_a;

   if (g_hm_states_for != rom)
   {
      int s;
      g_hm_states_for = rom;
      g_hm_states_n = 0;
      if (browser_rom_has_state(rom_dir, rom))
         for (s = 0; s < PSP_STATE_SLOT_COUNT; s++)
            g_hm_states_n += g_browser_state_exists[s] ? 1 : 0;
   }

   rom_display_name(&g_roms[rom], title, sizeof(title), 1);
   vs_text_fit(VS_HERO, title, VS_W - 2 * HM_X);
   vs_text(VS_HERO, HM_X, y, title, VS_HEX(0xF5F7FC, la));
   UI_TCH_BTN(PSP_CTRL_CROSS, HM_X / 2, (int)(y / 2), 300, 34);

   /* what is known about it */
   x = HM_X;
   y += 76;
   meta_line(&g_roms[rom], buf, sizeof(buf));
   x = hm_chip(x, y, buf, HC_GLASS, HC_TEXT2);
   x = hm_chip(x, y, rom_has_sav(rom) ? "Battery save" : "No save yet",
               HC_GLASS, HC_TEXT2);
   if (g_hm_states_n)
   {
      snprintf(buf, sizeof(buf), g_hm_states_n == 1 ? "%d save state"
                                                    : "%d save states",
               g_hm_states_n);
      x = hm_chip(x, y, buf, HC_GLASS, HC_TEXT2);
   }
   if (g_roms[rom].is_fav)
      x = hm_chip(x, y, "Starred", VS_ALPHA(hm_tint(g_pcfg.console, 255), 70),
                  HC_TEXT);

   /* what the game is, in a few lines -- Wikipedia's (CC BY-SA), said so
    * in a chip of its own, readable over any picture */
   if (blurb[0])
   {
      x = hm_chip(x, y, "From Wikipedia", HC_GLASS, HC_TEXT2);
      vs_text_wrap(VS_CAPTION, HM_X, y + 42, HM_BLURB_W, 3, 2, blurb,
                   VS_HEX(0xC9CFE0, la));
   }

   /* the buttons */
   y = HM_BTN_Y + dy;
   x = hm_button(HM_X, y, VS_I_PLAY, "Play", 1, PSP_CTRL_CROSS);
   if (g_hm_states_n)
      x = hm_button(x, y, VS_I_LAYERS, "Save states", 0, PSP_CTRL_DOWN);
   x = hm_button(x, y, g_roms[rom].is_fav ? VS_I_STAR : VS_I_STAR_O, NULL, 0,
                 PSP_CTRL_SQUARE);
   (void)x;
}

/* ---- the states, as cards ---------------------------------------------- */

static struct
{
   int rom;
   int status[PSP_STATE_SLOT_COUNT];   /* 0 not read, 1 picture, 2 none    */
   vs_img *img[PSP_STATE_SLOT_COUNT];
   int w[PSP_STATE_SLOT_COUNT], h[PSP_STATE_SLOT_COUNT];
   char when[PSP_STATE_SLOT_COUNT][24];
} g_hm_cards = { -1 };

static void hm_cards_drop(void)
{
   int i;
   for (i = 0; i < PSP_STATE_SLOT_COUNT; i++)
   {
      if (g_hm_cards.img[i])
         vs_img_free(g_hm_cards.img[i]);
      g_hm_cards.img[i] = NULL;
      g_hm_cards.status[i] = 0;
      g_hm_cards.when[i][0] = '\0';
   }
   g_hm_cards.rom = -1;
}

static void hm_cards_load_one(const char *rom_dir, int rom)
{
   char path[PSP_FILE_PATH_CAP], shot[PSP_STATE_SHOT_PATH_CAP];
   SceIoStat st;
   unsigned w = 0, h = 0;
   int i;
   if (g_hm_cards.rom != rom)
   {
      hm_cards_drop();
      g_hm_cards.rom = rom;
   }
   for (i = 0; i < PSP_STATE_SLOT_COUNT; i++)
      if (!g_hm_cards.status[i])
         break;
   if (i == PSP_STATE_SLOT_COUNT)
      return;
   g_hm_cards.status[i] = 2;
   if (browser_rom_state_path(rom_dir, rom, (unsigned)i + 1, path,
                              sizeof(path)) != 0 ||
       sceIoGetstat(path, &st) < 0)
      return;
   ov_stamp(g_hm_cards.when[i], sizeof(g_hm_cards.when[i]), &st.sce_st_mtime);
   if (hm_scratch() == 0 &&
       psp_state_shot_path(shot, sizeof(shot), path) == 0 &&
       hm_shot_read(shot, g_hm_buf, &w, &h) == 0)
   {
      g_hm_cards.img[i] = vs_img_new((int)w, (int)h);
      if (g_hm_cards.img[i])
      {
         vs_img_set(g_hm_cards.img[i], g_hm_buf, (int)w);
         g_hm_cards.w[i] = (int)w;
         g_hm_cards.h[i] = (int)h;
         g_hm_cards.status[i] = 1;
      }
   }
}

static void hm_cards(const char *rom_dir, int rom, float dy, float lift)
{
   int i;
   float y = VS_H + 42 + dy, x = HM_X, cw = 160, ch = 107;
   if (lift < 0.02f)
      return;
   if (g_browser_state_open)
      hm_cards_load_one(rom_dir, rom);
   vs_text(VS_SUB, HM_X, y - 54, "Save states", HC_TEXT);
   for (i = 0; i < PSP_STATE_SLOT_COUNT; i++)
   {
      int on = g_browser_state_open && g_browser_state_slot == i + 1;
      int have = g_browser_state_exists[i];
      char slot[16];
      if (on)
      {
         vs_shadow(x - 6, y - 6, cw + 12, ch + 12, 18, 14,
                   VS_HEX(0xFFFFFF, 50));
      }
      if (g_hm_cards.rom == rom && g_hm_cards.status[i] == 1 &&
          g_hm_cards.img[i])
      {
         vs_rrect(x, y, cw, ch, 12, VS_HEX(0x000000, 255));
         vs_img_draw(g_hm_cards.img[i], x + 2, y + 2, cw - 4, ch - 4,
                     VS_HEX(0xFFFFFF, 255), 1);
      }
      else
      {
         vs_rrect(x, y, cw, ch, 12, VS_HEX(0x0D1428, 190));
         vs_rrect_line(x, y, cw, ch, 12, 2, VS_HEX(0xFFFFFF, 26));
         vs_icon(have ? VS_I_LAYERS : VS_I_PLUS, x + cw * 0.5f,
                 y + ch * 0.5f, 30, have ? HC_TEXT2 : HC_TEXT3);
      }
      if (on)
         vs_rrect_line(x - 6, y - 6, cw + 12, ch + 12, 18, 3, HC_TEXT);
      snprintf(slot, sizeof(slot), "Slot %d", i + 1);
      vs_text(VS_LABEL, x, y + ch + 12, slot, on ? HC_TEXT : HC_TEXT2);
      vs_text(VS_CAPTION, x, y + ch + 42,
              have ? (g_hm_cards.rom == rom && g_hm_cards.when[i][0]
                         ? g_hm_cards.when[i] : "Saved")
                   : "Empty", HC_TEXT3);
      if (g_browser_state_open && !g_shelf_del)
         UI_TCH_ROW(i, (int)(x / 2), (int)(y / 2), (int)(cw / 2),
                    (int)((ch + 60) / 2));
      x += cw + 22;
   }
}

/* ---- box art that arrives ---------------------------------------------- */

/* The list's cartridges to artfetch.h, again whenever the list changes. */
static int g_hm_af_n = -1, g_hm_af_first = -1, g_hm_af_con = -1;

static void hm_art_hand_over(const char *rom_dir, int n)
{
   char (*paths)[PSP_FILE_PATH_CAP * 2];
   const char **pp, **np;
   int i;
   if (n <= 0 || (n == g_hm_af_n && view_rom(0) == g_hm_af_first &&
                  g_pcfg.console == g_hm_af_con))
      return;
   g_hm_af_n = n;
   g_hm_af_first = view_rom(0);
   g_hm_af_con = g_pcfg.console;
   paths = malloc((size_t)n * sizeof(*paths));
   pp = (const char **)malloc((size_t)n * sizeof(char *));
   np = (const char **)malloc((size_t)n * sizeof(char *));
   if (paths && pp && np)
   {
      for (i = 0; i < n; i++)
      {
         int r = view_rom(i);
         if (psp_rom_path_join(paths[i], sizeof(paths[i]), rom_dir,
                               g_roms[r].name) != 0)
            paths[i][0] = '\0';          /* (it cannot be opened: skipped) */
         pp[i] = paths[i];
         np[i] = g_roms[r].base;
      }
      artfetch_list(pp, np, n);
   }
   free(paths);
   free(pp);
   free(np);
}

/* A picture came for the cartridge called `base`: whatever was made without
 * it -- the art cache's "none", the tile, a cover-as-hero -- is made again. */
static void hm_art_arrived(const char *base, int n)
{
   int vi, i;
   for (vi = 0; vi < n; vi++)
   {
      const int r = view_rom(vi);
      if (strcmp(g_roms[r].base, base) != 0)
         continue;
      for (i = 0; i < ART_SLOTS; i++)
         if (g_art[i].state && g_art[i].rom_idx == r)
            g_art[i].state = 0;
      for (i = 0; i < HM_TILES; i++)
         if (g_hm_tile[i].rom == r)
         {
            if (g_hm_tile[i].img)
               vs_img_free(g_hm_tile[i].img);
            g_hm_tile[i].img = NULL;
            g_hm_tile[i].state = 0;
         }
      if (g_hero_idx == r)
         g_hero_state = 0;
      if (g_hm_pic.rom == r)
         g_hm_pic.rom = -1;          /* (its picture may be the cover) */
   }
}

static void hm_art_poll(const char *rom_dir, int n)
{
   char name[256], line[200];
   hm_art_hand_over(rom_dir, n);
   while (artfetch_take(name, sizeof(name)))
      hm_art_arrived(name, n);
   while (artfetch_log(line, sizeof(line)))
      fe_evt("%s", line);
}

/* ---- the whole page ---------------------------------------------------- */

static float g_hm_lift;               /* 0 = the row, 1 = the states        */

static void shell_home(const char *rom_dir, int cur, int n, int n_scan,
                       int idle)
{
   int rom = view_rom(cur);
   float dy;

   g_thm = &THM_DARK;
   g_hm_frame++;
   hm_ease(&g_hm_lift, g_browser_state_open ? 1.0f : 0.0f, 0.22f);
   dy = -HM_LIFT * g_hm_lift;
   if (g_browser_state_open && g_hm_lift > 0.99f)
      dy = -HM_LIFT;

   /* the picture: loaded when the row has settled on a game */
   if (g_hm_pic.rom != rom && idle >= IDLE_SELECTED)
      hm_pic_load(rom);
   if (g_hm_pic.rom == rom && g_hm_pic.state > 0)
      hm_ease(&g_hm_pic.a, 255.0f, 0.18f);
   if (g_hm_pic.old)
   {
      hm_ease(&g_hm_pic.old_a, 0.0f, 0.2f);
      if (g_hm_pic.old_a <= 0.5f)
      {
         vs_img_free(g_hm_pic.old);
         g_hm_pic.old = NULL;
      }
   }

   hm_art_poll(rom_dir, n);
   hm_clip_tick(rom, idle);
   hm_backdrop(rom, dy);
   hm_topbar(dy, n_scan);
   hm_row(rom_dir, cur, n, dy, idle);
   hm_focus(rom_dir, rom, dy);
   hm_cards(rom_dir, rom, dy, g_hm_lift);

   /* the pad's hints */
   if (g_shelf_del)
   {
      char q[40];
      static const int ic[2] = { VS_I_BTN_X, VS_I_BTN_O };
      static const char *const w[2] = { "Delete", "Keep" };
      snprintf(q, sizeof(q), "Delete the state in slot %d?", g_shelf_del);
      vs_rrect(HM_X - 14, VS_H - 74, VS_W - 2 * HM_X + 28, 56, 28,
               VS_HEX(0x1A2340, 245));
      vs_text(VS_LABEL, HM_X + 8, VS_H - 61, q, HC_TEXT);
      hm_hints(VS_W - HM_X, VS_H - 58, ic, w, 2);
      tch_modal(0);
      UI_TCH_BTN(PSP_CTRL_CROSS, 300, 238, 70, 30);
      UI_TCH_BTN(PSP_CTRL_CIRCLE, 380, 238, 80, 30);
   }
   else if (g_browser_state_open)
   {
      static const int ic_all[3] = { VS_I_BTN_X, VS_I_BTN_SQ, VS_I_BTN_O };
      static const char *const w_all[3] = { "Load", "Delete", "Back" };
      static const int ic_back[1] = { VS_I_BTN_O };
      static const char *const w_back[1] = { "Back" };
      int have = g_browser_state_exists[g_browser_state_slot - 1];
      const int *ic = have ? ic_all : ic_back;
      const char *const *w = have ? w_all : w_back;
      hm_hints(VS_W - HM_X, VS_H - 44, ic, w, have ? 3 : 1);
      if (g_shelf_note[0])
         vs_text(VS_CAPTION, HM_X, VS_H - 44, g_shelf_note, HC_TEXT2);
   }
   else
   {
      static const int ic[3] = { VS_I_BTN_SQ, VS_I_BTN_TRI, -1 };
      static const char *const w[3] = { "Star", "Console", "START  Settings" };
      hm_hints(VS_W - HM_X, HM_BTN_Y + dy + 16, ic, w, 3);
   }
   (void)n_scan;
}

/* A console with no games, or no starred ones: what is missing and the
 * one thing to do about it, in the home screen's own look. */
static void home_empty(const char *rom_dir, int n_scan)
{
   const ui_skin *sk = skin_of(g_pcfg.console);
   char msg[96], dir[PSP_FILE_PATH_CAP];
   float y = 150;
   int con = g_pcfg.console;
   g_thm = &THM_DARK;
   vs_rect(0, 0, VS_W, VS_H, HC_BG);
   vs_shadow(330, 120, 300, 220, 110, 160, hm_tint(con, 60));
   hm_topbar(0, n_scan);
   vs_circle(VS_W * 0.5f, y + 20, 56, HC_GLASS);
   if (g_browser_favs)
   {
      vs_icon(VS_I_STAR_O, VS_W * 0.5f, y + 20, 56, hm_tint(con, 255));
      snprintf(msg, sizeof(msg), "No starred %s games yet", sk->id);
      vs_text_c(VS_TITLE, VS_W * 0.5f, y + 96, msg, HC_TEXT);
      vs_text_c(VS_BODY, VS_W * 0.5f, y + 156,
                "Press square on a game to star it.", HC_TEXT2);
      hm_button(VS_W * 0.5f - 110, y + 216, VS_I_GRID, "All games", 1,
                PSP_CTRL_SELECT);
      return;
   }
   {
      int others = vita_browser_others(rom_dir);
      vs_icon(VS_I_CART, VS_W * 0.5f, y + 20, 60, hm_tint(con, 255));
      snprintf(msg, sizeof(msg), "No %s games yet", sk->full);
      vs_text_c(VS_TITLE, VS_W * 0.5f, y + 96, msg, HC_TEXT);
      snprintf(msg, sizeof(msg), "Copy your .%s files into this folder on "
               "the memory card:", sk->ext);
      vs_text_c(VS_BODY, VS_W * 0.5f, y + 154, msg, HC_TEXT2);
      snprintf(dir, sizeof(dir), "%s", rom_dir);
      vs_text_fit(VS_LABEL, dir, 760);
      {
         float w = vs_text_w(VS_LABEL, dir) + 40;
         vs_rrect(VS_W * 0.5f - w * 0.5f, y + 192, w, 44, 22, HC_GLASS);
         vs_text_c(VS_LABEL, VS_W * 0.5f, y + 202, dir, HC_TEXT);
      }
      if (others > 0)
      {
         snprintf(msg, sizeof(msg), others == 1 ? "Show my other game"
                                                : "Show my %d other games",
                  others);
         {
            float w = vs_text_w(VS_LABEL, msg) + 76;
            hm_button(VS_W * 0.5f - w * 0.5f, y + 262, VS_I_BTN_TRI, msg, 1,
                      PSP_CTRL_TRIANGLE);
         }
      }
      else
      {
         float w = vs_text_w(VS_LABEL, "Look again") + 76;
         hm_button(VS_W * 0.5f - w * 0.5f, y + 262, VS_I_REFRESH,
                   "Look again", 1, PSP_CTRL_CROSS);
      }
      if (vita_extra_note(dir, sizeof(dir))[0])
      {
         vs_text_fit(VS_CAPTION, dir, 760);
         vs_text_c(VS_CAPTION, VS_W * 0.5f, y + 330, dir, HC_TEXT2);
      }
   }
}

/* The browser's directions, for a row that runs sideways (see the top). */
static unsigned home_remap(unsigned e)
{
   unsigned out = e & ~(unsigned)(PSP_CTRL_UP | PSP_CTRL_DOWN | PSP_CTRL_LEFT |
                                  PSP_CTRL_RIGHT);
   if (g_browser_state_open)
   {
      if (e & PSP_CTRL_LEFT)  out |= PSP_CTRL_UP;
      if (e & PSP_CTRL_RIGHT) out |= PSP_CTRL_DOWN;
      if (e & PSP_CTRL_UP)    out |= PSP_CTRL_CIRCLE;
   }
   else
   {
      if (e & PSP_CTRL_LEFT)  out |= PSP_CTRL_UP;
      if (e & PSP_CTRL_RIGHT) out |= PSP_CTRL_DOWN;
      if (e & PSP_CTRL_DOWN)  out |= PSP_CTRL_LEFT;
   }
   return out;
}

static void home_free(void)
{
   int i;
   for (i = 0; i < HM_TILES; i++)
   {
      if (g_hm_tile[i].img)
         vs_img_free(g_hm_tile[i].img);
      memset(&g_hm_tile[i], 0, sizeof(g_hm_tile[i]));
      g_hm_tile[i].rom = -1;
   }
   if (g_hm_pic.img)
      vs_img_free(g_hm_pic.img);
   if (g_hm_pic.old)
      vs_img_free(g_hm_pic.old);
   memset(&g_hm_pic, 0, sizeof(g_hm_pic));
   g_hm_pic.rom = -1;
   hm_cards_drop();
   hm_clip_drop();
   g_hm_states_for = -1;
   g_hm_label_for = -1;
   free(g_hm_buf);
   g_hm_buf = NULL;
   free(g_hm_sq);
   g_hm_sq = NULL;
}

/* The browser opens: nothing is cached from the last visit (the list may
 * have changed, a game was played: its scene is new). */
static void home_reset(void)
{
   home_free();
   g_hm_clip.none_for = -1;
   g_hm_lift = 0.0f;
}

#endif /* UI_VITA_HOME_H */
