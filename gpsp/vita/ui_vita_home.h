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
#include "menu_load.h"
#include "menu_files_private.h"
#include "plat.h"

#define UI_VITA_HOME_ASYNC 1

#define HM_X        44
#define HM_TEX      160          /* a tile's texture edge                    */
#define HM_TILES    16           /* tiles kept                               */
#define HM_TITLE_Y  300
#define HM_BTN_Y    448
#define HM_BLURB_LIFT 60         /* the name goes up for a game's lines     */
#define HM_BLURB_W  600          /* ...which are this wide, three at most   */
#define HM_LIFT     272          /* how far DOWN lifts the page              */

typedef struct
{
   menu_load_meta value;
   unsigned epoch, attempts;
   int status;
   uint64_t retry_us;
} hm_meta;
static hm_meta g_hm_meta[BROWSER_MAX];
static unsigned g_hm_epoch[BROWSER_MAX], g_hm_generation = 1;
/* Artwork has its own key, separate from the save-state epoch: a cover that
 * arrives must not drop a game's save metadata or cancel the batch the loader
 * is working on.  Arrivals only mark keys dirty (all of them for a coalesced
 * arrival, including raw ROMs hidden by Find or Starred); hm_art_apply()
 * advances the marked keys when no batch is outstanding, so the selected game
 * is then reloaded first and its current picture stays until the new one is
 * ready. */
static unsigned g_hm_art_epoch[BROWSER_MAX];
static unsigned char g_hm_art_dirty[BROWSER_MAX];
static int g_hm_art_any, g_hm_art_all;
static unsigned g_hm_load_serial, g_hm_load_dirty;
static int g_hm_load_rom = -1, g_hm_load_open = -1, g_hm_load_active;
/* The raw games of the batch in flight, in job order (hm_result_rom). */
static int g_hm_batch_rom[1 + ML_NEIGHBORS];
static unsigned g_hm_batch_n;
static unsigned g_hm_load_left;
static int g_hm_uploaded, g_hm_still_for = -1;

static unsigned hm_epoch(int rom)
{
   return g_hm_epoch[rom] ? g_hm_epoch[rom] : 1;
}
static unsigned hm_art_epoch(int rom)
{
   return g_hm_art_epoch[rom] ? g_hm_art_epoch[rom] : 1;
}
/* 1 if an arrival marked this game's artwork and it has not been applied. */
static int hm_art_pending(int rom)
{
   return rom >= 0 && rom < BROWSER_MAX &&
          (g_hm_art_all || g_hm_art_dirty[rom]);
}
/* Advance every marked artwork key.  Only between batches (hm_load_pump),
 * never while a request is outstanding, so arrivals cannot starve it. */
static void hm_art_apply(void)
{
   if (!g_hm_art_any)
      return;
   for (int i = 0; i < BROWSER_MAX; i++)
      if (hm_art_pending(i))
      {
         unsigned next = hm_art_epoch(i) + 1;
         g_hm_art_epoch[i] = next ? next : 1;
         g_hm_art_dirty[i] = 0;
      }
   g_hm_art_any = g_hm_art_all = 0;
}
/* When to ask again.  A failure that may pass (I/O, memory) after 1 s, then
 * every 4 s; a missing picture after 30 s; content that can never decode
 * (a corrupt cover) after 10 minutes -- new artwork for the game makes it
 * due at once anyway (its art key changes). */
#define HM_RETRY_INVALID_US 600000000u
static uint64_t hm_retry(int status, unsigned *attempts)
{
   if (status == ML_ERROR)
      return plat_time_us() + ((*attempts)++ ? 4000000u : 1000000u);
   *attempts = 0;
   if (status == ML_INVALID)
      return plat_time_us() + HM_RETRY_INVALID_US;
   return status == ML_ABSENT ? plat_time_us() + 30000000u : 0;
}
static const hm_meta *hm_metadata(int rom)
{
   return rom >= 0 && rom < BROWSER_MAX && g_hm_meta[rom].status &&
          g_hm_meta[rom].epoch == hm_epoch(rom) ? &g_hm_meta[rom] : NULL;
}
static int home_cached_states(int rom)
{
   const hm_meta *m = hm_metadata(rom);
   int any = 0, unknown = !m;
   for (int i = 0; i < PSP_STATE_SLOT_COUNT; i++)
   {
      int state = m ? m->value.state[i] : ML_UNKNOWN;
      g_browser_state_exists[i] = state == ML_PRESENT || state == ML_OLD_ONLY;
      any |= g_browser_state_exists[i];
      unknown |= state == ML_UNKNOWN;
   }
   return any || unknown; /* An unfinished query must not hide the shelf. */
}
/* 1 when the game's save-state metadata is current and it has no state at
 * all: the shelf, opened before that was known, has nothing to offer. */
static int home_states_known_empty(int rom)
{
   const hm_meta *m = hm_metadata(rom);
   if (!m) return 0;
   for (int i = 0; i < PSP_STATE_SLOT_COUNT; i++)
      if (m->value.state[i] != ML_MISSING) return 0;
   return 1;
}
/* 1 when the game has a point to continue from (state_slots.h
 * psp_state_resume_path), as its metadata says. */
static int home_has_resume(int rom)
{
   const hm_meta *m = hm_metadata(rom);
   return m && m->value.resume == ML_PRESENT;
}
static void home_state_invalidate(int rom)
{
   if (rom < 0 || rom >= BROWSER_MAX) return;
   unsigned next = hm_epoch(rom) + 1;
   g_hm_epoch[rom] = next ? next : 1;
   g_hm_load_dirty = 1;
}

/* ---- pictures --------------------------------------------------------- */

/* A .shot (state_slots.h: 16-byte header, then rows of PSP-order 565 at a
 * stride of 256) into RGBA.  0 = read. */
static int hm_shot_read_inner(const char *path, uint32_t *rgba, unsigned *w,
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

static int hm_shot_read(const char *path, uint32_t *rgba, unsigned *w, unsigned *h)
{
   unsigned long long t = md_begin();
   int rc = hm_shot_read_inner(path, rgba, w, h);
   md_end(MD_ART_READ, t);
   return rc;
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
   unsigned long long t;
   int flen = 0, w = 0, h = 0, nc = 0, tw, th, x, y;
   if (snprintf(path, sizeof(path), "%s/titles/%.*s.png", g_dir_base,
                (int)ui_rom_stem_length(base), base) >= (int)sizeof(path))
      return -1;
   file = art_slurp(path, &flen);
   if (!file)
      return -1;
   t = md_begin();
   if (!stbi_info_from_memory(file, flen, &w, &h, &nc) || w <= 0 || h <= 0 ||
       w > 2048 || h > 2048)
   {
      free(file);
      md_end(MD_DECODE, t);
      return -1;
   }
   pix = stbi_load_from_memory(file, flen, &w, &h, &nc, 4);
   free(file);
   md_end(MD_DECODE, t);
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
   t = md_begin();
   for (y = 0; y < th; y++)
      for (x = 0; x < tw; x++)
      {
         const unsigned char *p = pix + 4 * ((size_t)(y * h / th) * w +
                                             (size_t)(x * w / tw));
         rgba[y * tw + x] = 0xFF000000u | ((uint32_t)p[2] << 16) |
                            ((uint32_t)p[1] << 8) | p[0];
      }
   md_end(MD_RESAMPLE, t);
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
   unsigned epoch, attempts;
   uint64_t retry_us;
   unsigned art_epoch; /* hm_art_epoch when the picture was made           */
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

/* The tile from its thumbnail (art_thumb_load: a small copy made when the
 * cover arrived).  One read and a centre crop, no decode.  1 = t has its
 * picture; 0 = no thumbnail, and hm_tile_load makes the tile the slow way. */
static int hm_thumb_tile(hm_tile *t)
{
   uint16_t *th;
   uint32_t *big;
   int tw = ART_THUMB_W, tht = ART_THUMB_H;
   if (hm_scratch() != 0)
      return 0;
   th = (uint16_t *)malloc((size_t)ART_THUMB_W * ART_THUMB_H * 2);
   if (!th)
      return 0;
   if (art_thumb_load(t->rom, th, ART_THUMB_W, &tw, &tht) != 0)
   {
      free(th);
      return 0;
   }
   big = (uint32_t *)malloc((size_t)tw * tht * 4);
   if (!big)
   {
      free(th);
      return 0;
   }
   hm_from565(th, ART_THUMB_W, tw, tht, big);
   free(th);
   hm_square(big, tw, tht, g_hm_sq, HM_TEX);
   free(big);
   hm_round(g_hm_sq, HM_TEX, 20);
   t->img = vs_img_new(HM_TEX, HM_TEX);
   if (!t->img)
      return 0;
   vs_img_set(t->img, g_hm_sq, HM_TEX);
   t->state = 1;
   return 1;
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
   int old_rom;
   int old_state;
   unsigned epoch, attempts;
   uint64_t retry_us;
   unsigned art_epoch;
} g_hm_pic = { -1, 0, NULL, 0, 0, 0.0f, NULL, 0, 0, 0.0f, -1 };

static void hm_pic_load_decode(int rom)
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
   g_hm_pic.old_rom = g_hm_pic.rom;
   if (g_hm_pic.state != 1 && g_hm_pic.img)
      vs_img_free(g_hm_pic.img);
   g_hm_pic.img = NULL;
   g_hm_pic.rom = rom;
   g_hm_pic.state = -1;
   g_hm_pic.a = 0.0f;
   unsigned long long hero_t = md_begin();
   const uint16_t *hero = hero_get(rom, 1);
   md_end(MD_HERO, hero_t);
   if (hero && g_hero_precomposed)
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

static void hm_pic_load(int rom)
{
   unsigned started = sceKernelGetSystemTimeLow();
   unsigned long long t = md_begin();
   if (md_active()) fe_evt("home_still_begin rom=%d", rom);
   hm_pic_load_decode(rom);
   md_end(MD_STILL, t);
   fe_evt("home_still rom=%d load_us=%u state=%d", rom,
          (unsigned)(sceKernelGetSystemTimeLow() - started), g_hm_pic.state);
}

/* ---- the preview: the last ten seconds played -------------------------- */

#include "preview_vita.h"

#define HM_CLIP_WAIT   48       /* frames resting on a game after it is
                                 * selected (IDLE_SELECTED) before its clip
                                 * starts: about a second in all            */
#define HM_CLIP_REST   2500000u /* us of the still between two plays        */
#define HM_CLIP_TEX    3        /* textures in turn: the GPU may still be
                                 * drawing the one before last              */

enum { HC_OFF = 0, HC_PLAY, HC_OUT, HC_REST };

static struct
{
   int rom;                 /* whose clip is open, -1 none                  */
   int none_for;            /* a game known to have none (this visit)       */
   vs_img *tex[HM_CLIP_TEX];
   unsigned w, h;
   int shown;               /* the texture on screen, -1 none yet           */
   int phase;
   float a;                 /* fade, 0..255                                 */
   clip_preview_frame next;
   int have_next;
   unsigned acc_us;         /* time since the frame on screen came          */
   unsigned last_us;
   unsigned rest_us;
   unsigned us_per_tick;
   uint32_t token;
   int ended;
} g_hm_clip = { .rom = -1, .none_for = -1, .shown = -1 };

static uint32_t g_hm_clip_serial;
static int g_hm_clip_selection = -1;
static int g_hm_clip_disabled;

static int hm_clip_path(int rom, char *out, size_t cap)
{
   extern char g_dir_base[];
   const char *base = g_roms[rom].base;
   int n = snprintf(out, cap, "%s/clips/%.*s.clip", g_dir_base,
                    (int)ui_rom_stem_length(base), base);
   return n > 0 && (size_t)n < cap ? 0 : -1;
}

static void hm_clip_cancel(void)
{
   if (g_hm_clip.have_next)
      clip_preview_release(g_hm_clip.next.slot);
   g_hm_clip.have_next = 0;
   g_hm_clip.token = ++g_hm_clip_serial;
   clip_preview_cancel(g_hm_clip.token);
   g_hm_clip.rom = -1;
   g_hm_clip.shown = -1;
   g_hm_clip.phase = HC_OFF;
   g_hm_clip.a = 0.0f;
   g_hm_clip.ended = 0;
   g_hm_clip.acc_us = 0;
   g_hm_clip.rest_us = 0;
   g_hm_clip.us_per_tick = 0;
}

static void hm_clip_drop(void)
{
   int i;
   hm_clip_cancel();
   for (i = 0; i < HM_CLIP_TEX; i++)
   {
      if (g_hm_clip.tex[i])
         vs_img_free(g_hm_clip.tex[i]);
      g_hm_clip.tex[i] = NULL;
   }
   g_hm_clip.w = g_hm_clip.h = 0;
   g_hm_clip.none_for = -1;
   g_hm_clip_selection = -1;
}

/* Selection invalidation happens before hero/scene loading can block. */
static void hm_selection_tick(int rom)
{
   if (g_hm_clip_selection == rom)
      return;
   g_hm_clip_selection = rom;
   hm_clip_cancel();
   g_hm_clip.none_for = -1;
   if (g_hm_pic.old)
      vs_img_free(g_hm_pic.old);
   g_hm_pic.old = NULL;
   g_hm_pic.old_a = 0.0f;
   g_hm_pic.old_rom = -1;
   if (g_pcfg.previews)
      (void)clip_preview_start();
}

static void hm_clip_open(int rom)
{
   char path[PSP_FILE_PATH_CAP];
   hm_clip_cancel();
   if (hm_clip_path(rom, path, sizeof(path)) != 0 ||
       clip_preview_request(g_hm_clip.token, path) != 0)
   {
      g_hm_clip.none_for = rom;
      return;
   }
   g_hm_clip.rom = rom;
   g_hm_clip.phase = HC_PLAY;
   g_hm_clip.acc_us = 0;
   g_hm_clip.last_us = sceKernelGetSystemTimeLow();
   fe_evt("home_clip_request rom=%d token=%u path=%s", rom, g_hm_clip.token, path);
}

static int hm_clip_textures(unsigned w, unsigned h)
{
   int i;
   if (g_hm_clip.w == w && g_hm_clip.h == h && g_hm_clip.tex[0])
      return 0;
   for (i = 0; i < HM_CLIP_TEX; i++)
   {
      if (g_hm_clip.tex[i])
         vs_img_free(g_hm_clip.tex[i]);
      g_hm_clip.tex[i] = NULL;
   }
   g_hm_clip.w = w;
   g_hm_clip.h = h;
   for (i = 0; i < HM_CLIP_TEX; i++)
      if (!(g_hm_clip.tex[i] = vs_img_new_565((int)w, (int)h)))
      {
         int j;
         for (j = 0; j < i; j++)
         {
            vs_img_free(g_hm_clip.tex[j]);
            g_hm_clip.tex[j] = NULL;
         }
         return -1;
      }
   return 0;
}

/* Main thread only times, fades and publishes one immutable CPU frame. */
static void hm_clip_tick(int rom, int idle)
{
   unsigned now, dt, us;
   if (!g_pcfg.previews)
   {
      if (!g_hm_clip_disabled)
         hm_clip_cancel();
      g_hm_clip_disabled = 1;
      return;
   }
   g_hm_clip_disabled = 0;
   if (g_hm_clip.rom != rom)
   {
      /* another game: the one before stops at once (its picture is
       * already fading out with the still) */
      if (g_hm_clip.rom >= 0)
         hm_clip_cancel();
      if (idle >= IDLE_SELECTED + HM_CLIP_WAIT && g_hm_clip.none_for != rom)
         hm_clip_open(rom);
      if (g_hm_clip.rom < 0)
         return;
   }
   if (idle < IDLE_SELECTED)
   {
      g_hm_clip.last_us = sceKernelGetSystemTimeLow();
      return;                   /* retain the frame, not background disk reads */
   }
   now = sceKernelGetSystemTimeLow();
   dt = now - g_hm_clip.last_us;
   g_hm_clip.last_us = now;
   if (dt > 100000)
      dt = 100000;              /* a stall is not a reason to skip ahead */
   us = g_hm_clip.us_per_tick ? g_hm_clip.us_per_tick : 16743u;

   switch (g_hm_clip.phase)
   {
   case HC_PLAY:
      if (!g_hm_clip.have_next && !g_hm_clip.ended)
         g_hm_clip.have_next = clip_preview_take(g_hm_clip.token, &g_hm_clip.next);
      if (g_hm_clip.have_next && g_hm_clip.next.ended)
      {
         int bad = g_hm_clip.next.bad;
         clip_preview_release(g_hm_clip.next.slot);
         g_hm_clip.have_next = 0;
         g_hm_clip.ended = 1;
         if (bad)
         {
            hm_clip_cancel();
            g_hm_clip.none_for = rom;
            return;
         }
      }
      g_hm_clip.acc_us += dt;
      if (g_hm_clip.have_next &&
          (g_hm_clip.shown < 0 ||
           g_hm_clip.acc_us >= g_hm_clip.next.ticks * us))
      {
         int t = -1;
         if (g_hm_clip.next.token != g_hm_clip.token ||
             hm_clip_textures(g_hm_clip.next.w, g_hm_clip.next.h) != 0)
         {
            hm_clip_cancel();
            g_hm_clip.none_for = rom;
            return;
         }
         if (g_hm_uploaded) break;
         for (int step = 1; step <= HM_CLIP_TEX; step++)
         {
            int at = (g_hm_clip.shown + step) % HM_CLIP_TEX;
            if (at != g_hm_clip.shown && vs_img_writable(g_hm_clip.tex[at]))
            { t = at; break; }
         }
         if (t < 0) break; /* Keep the CPU pin and old frame; never wait. */
         vs_img_set_565(g_hm_clip.tex[t], g_hm_clip.next.pixels,
                        (int)g_hm_clip.next.w);
         g_hm_uploaded = 1;
         g_hm_clip.us_per_tick = g_hm_clip.next.us_per_tick;
         if (g_hm_clip.shown < 0)
            fe_evt("home_clip open rom=%d token=%u frames=%u w=%u h=%u", rom,
                   g_hm_clip.token, g_hm_clip.next.frames, g_hm_clip.w, g_hm_clip.h);
         if (g_hm_clip.shown < 0 || g_hm_clip.acc_us > 4 * 2 * us)
            g_hm_clip.acc_us = 0;     /* the first frame, or a late one */
         else
            g_hm_clip.acc_us -= g_hm_clip.next.ticks * us;
         g_hm_clip.shown = t;
         clip_preview_release(g_hm_clip.next.slot);
         g_hm_clip.have_next = 0;
      }
      else if (!g_hm_clip.have_next && g_hm_clip.acc_us > 4 * 2 * us)
         g_hm_clip.acc_us = 4 * 2 * us;  /* still being read in */
      if (g_hm_clip.shown >= 0)
         hm_ease(&g_hm_clip.a, 255.0f, 0.12f);
      if (g_hm_clip.ended && !g_hm_clip.have_next)
      {
         if (g_hm_clip.shown < 0)
         {
            /* broken from the first frame */
            hm_clip_cancel();
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
         if (clip_preview_restart(g_hm_clip.token) != 0)
         {
            hm_clip_cancel();
            g_hm_clip.none_for = rom;
            return;
         }
         g_hm_clip.ended = 0;
         g_hm_clip.have_next = 0;
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
   if (g_hm_pic.old && g_hm_pic.old_rom == rom && g_hm_pic.old_a > 0.5f)
   {
      if (g_hm_pic.old_state == 3)
         vs_img_draw(g_hm_pic.old, 0, 0, VS_W, VS_H,
                      VS_HEX(0xFFFFFF, (int)g_hm_pic.old_a), 1);
      else
         hm_scene_draw(g_hm_pic.old, g_hm_pic.old_w, g_hm_pic.old_h,
                       g_hm_pic.old_a, dy);
   }
   if (g_hm_pic.rom == rom && g_hm_pic.state == 1)
      hm_scene_draw(g_hm_pic.img, g_hm_pic.w, g_hm_pic.h, g_hm_pic.a, dy);
   else if (g_hm_pic.rom == rom && g_hm_pic.state == 3)
   {
      int bh = VS_W * g_hm_pic.h / (g_hm_pic.w ? g_hm_pic.w : 1);
      vs_img_draw(g_hm_pic.img, 0, (VS_H - bh) * 0.5f, VS_W, bh,
                   VS_HEX(0xFFFFFF, (int)g_hm_pic.a), 1);
   }
   else if (g_hm_pic.rom == rom && g_hm_pic.state == 2 && g_hero &&
            g_hero_idx == rom && g_hero_state > 0)
   {
      int bh = (int)((long)480 * g_hero_h / (g_hero_w ? g_hero_w : 1));
      vid_image(0, (272 - bh) / 2, 480, bh, g_hero, g_hero_tex, g_hero_tex,
                g_hero_w, g_hero_h, (int)g_hm_pic.a);
   }
   /* the preview, over the still it ends on */
   if (g_hm_clip.rom == rom && g_hm_clip.shown >= 0)
      hm_scene_draw(g_hm_clip.tex[g_hm_clip.shown], (int)g_hm_clip.w,
                    (int)g_hm_clip.h, g_hm_clip.a, dy);
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

   /* Games | Starred | Recent: SELECT steps through them (Recent once a
    * game has been played), a tap goes straight to one. */
   {
      static const char *const TAB[3] = { "Games", "Starred", "Recent" };
      int t, on = browser_tab();
      for (t = 0; t < 3; t++)
      {
         vs_text(VS_SUB, x, y, TAB[t], t == on ? HC_TEXT : HC_TEXT3);
         UI_TCH_BTN(t == on ? 0 : BROWSER_TAB_BUTTON(t), (int)(x / 2) - 4,
                    (int)(y / 2), (int)(vs_text_w(VS_SUB, TAB[t]) / 2) + 8, 22);
         x += vs_text_w(VS_SUB, TAB[t]) + (t < 2 ? 24 : 30);
      }
   }

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
   vs_icon(VS_I_SEARCH, x - 80, y + 19, 26, HC_TEXT2);
   VS_TCH_BTN(BROWSER_FIND_BUTTON, x - 102, y, 44, 44);
   if (g_find[0])
   {
      /* The search in force, as a chip under the tabs: what was looked
       * for, how many of the console's games it shows, and its X.  A tap
       * on the words edits the search, on the X clears it. */
      char find[96], count[32];
      float count_x, qx = HM_X + 20;
      snprintf(count, sizeof(count), "%d of %d", view_count(n_scan), scope_count(n_scan));
      count_x = 414 - vs_text_w(VS_CAPTION, count);
      vs_rrect(HM_X - 12, y + 38, 478 - HM_X, 24, 12, HC_GLASS);
      vs_icon(VS_I_SEARCH, HM_X + 6, y + 50, 16, HC_TEXT2);
      snprintf(find, sizeof(find), "\"%s\"", g_find);
      vs_text_fit(VS_CAPTION, find, count_x - qx - 14);
      vs_text(VS_CAPTION, qx, y + 39, find, HC_TEXT);
      vs_text(VS_CAPTION, count_x, y + 39, count, HC_TEXT2);
      VS_TCH_BTN(BROWSER_FIND_BUTTON, HM_X, y + 46, 380, 20);
      vs_icon(VS_I_CLOSE, 446, y + 50, 16, HC_TEXT);
      VS_TCH_BTN(PSP_CTRL_CIRCLE, 426, y + 46, 40, 20);
   }
   {
      int seen = 0, total = 0, have = 0;
      if (ui_cover_progress(&seen, &total, &have))
      {
         char status[64];
         snprintf(status, sizeof(status), "Checking covers: %d of %d", seen, total);
         vs_text(VS_CAPTION, VS_W - HM_X - vs_text_w(VS_CAPTION, status),
                 y + 44, status, HC_TEXT2);
      }
   }
   return y;
}

/* ---- the row ------------------------------------------------------------ */

static float g_hm_label_a;          /* the focused name's fade               */
static int g_hm_label_for = -1;

static void hm_row(const char *rom_dir, int cur, int n, float dy, int idle)
{
   int k, first = cur > 0 ? -1 : 0;
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
   (void)rom_dir; (void)idle;
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
   unsigned long long began = md_begin();
   const hm_meta *m = hm_metadata(rom);
   if (m)
   {
      g_hm_blurb_for = rom;
      memcpy(g_hm_blurb, m->value.blurb, sizeof(g_hm_blurb));
   }
   (void)rom_dir;
   md_end(MD_DESCRIPTION, began);
   return g_hm_blurb_for == rom ? g_hm_blurb : "";
}

static void hm_focus(const char *rom_dir, int rom, float dy, int idle)
{
   char title[80], buf[48];
   const char *blurb = idle >= IDLE_SELECTED ? hm_blurb(rom_dir, rom)
      : g_hm_blurb_for == rom ? g_hm_blurb : "";
   float x, y = HM_TITLE_Y + dy - (blurb[0] ? HM_BLURB_LIFT : 0);
   int la = (int)g_hm_label_a;

   const hm_meta *m = hm_metadata(rom);
   unsigned long long meta_t = md_begin();
   if (m)
   {
      int s;
      g_hm_states_for = rom;
      g_hm_states_n = 0;
      (void)home_cached_states(rom);
      for (s = 0; s < PSP_STATE_SLOT_COUNT; s++)
         g_hm_states_n += g_browser_state_exists[s] ? 1 : 0;
   }
   else { g_hm_states_for = -1; g_hm_states_n = 0; }
   md_end(MD_META, meta_t);

   rom_display_name(&g_roms[rom], title, sizeof(title), 1);
   vs_text_fit(VS_HERO, title, VS_W - 2 * HM_X);
   vs_text(VS_HERO, HM_X, y, title, VS_HEX(0xF5F7FC, la));
   UI_TCH_BTN(PSP_CTRL_CROSS, HM_X / 2, (int)(y / 2), 300, 34);

   /* what is known about it */
   x = HM_X;
   y += 76;
   meta_line(&g_roms[rom], buf, sizeof(buf));
   x = hm_chip(x, y, buf, HC_GLASS, HC_TEXT2);
   if (m && m->value.battery != ML_UNKNOWN)
      x = hm_chip(x, y, m->value.battery == ML_PRESENT ? "Battery save" : "No save yet",
                  HC_GLASS, HC_TEXT2);
   if (g_hm_states_for == rom && g_hm_states_n)
   {
      snprintf(buf, sizeof(buf), g_hm_states_n == 1 ? "%d save state"
                                                    : "%d save states",
               g_hm_states_n);
      x = hm_chip(x, y, buf, HC_GLASS, HC_TEXT2);
   }
   if (g_roms[rom].is_fav)
      x = hm_chip(x, y, "Starred", VS_ALPHA(hm_tint(g_pcfg.console, 255), 70),
                  HC_TEXT);
   if (g_roms[rom].play_last)
   {
      /* play_log.h: when, and for how long */
      char when[32], dur[32];
      play_log_when(g_roms[rom].play_last, (uint32_t)plat_wallclock(),
                    when, sizeof(when));
      if (g_roms[rom].play_secs >= 60)
      {
         play_log_duration(g_roms[rom].play_secs, dur, sizeof(dur));
         snprintf(buf, sizeof(buf), "Played %s, %s", when, dur);
      }
      else
         snprintf(buf, sizeof(buf), "Played %s", when);
      x = hm_chip(x, y, buf, HC_GLASS, HC_TEXT2);
   }
   if (m && m->value.patch)
      x = hm_chip(x, y, m->value.patch == 1 ? "Patched (IPS)" :
                        m->value.patch == 2 ? "Patched (BPS)" : "Patched (UPS)",
                  HC_GLASS, HC_TEXT2);

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
   if (home_has_resume(rom))
   {
      /* Where the game was left: CROSS goes back to it; Start over (UP,
       * or a tap) plays from the battery save as before. */
      x = hm_button(HM_X, y, VS_I_PLAY, "Continue", 1, PSP_CTRL_CROSS);
      x = hm_button(x, y, VS_I_REFRESH, "Start over", 0, BROWSER_FRESH_BUTTON);
   }
   else
      x = hm_button(HM_X, y, VS_I_PLAY, "Play", 1, PSP_CTRL_CROSS);
   if (g_hm_states_for == rom && g_hm_states_n)
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
   unsigned epoch[PSP_STATE_SLOT_COUNT], attempts[PSP_STATE_SLOT_COUNT];
   uint64_t retry_us[PSP_STATE_SLOT_COUNT];
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
      g_hm_cards.epoch[i] = g_hm_cards.attempts[i] = 0;
      g_hm_cards.retry_us[i] = 0;
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
                   : (!hm_metadata(rom) || hm_metadata(rom)->value.state[i] == ML_UNKNOWN
                      ? "Checking..." : "Empty"), HC_TEXT3);
      if (g_browser_state_open && !g_shelf_del)
         UI_TCH_ROW(i, (int)(x / 2), (int)(y / 2), (int)(cw / 2),
                    (int)((ch + 60) / 2));
      x += cw + 22;
   }
   (void)rom_dir;
}

/* ---- box art that arrives ---------------------------------------------- */

/* The raw console inventory is independent of Games/Starred and Find. */
static int g_hm_af_n = -1, g_hm_af_first = -1, g_hm_af_con = -1;
static unsigned g_hm_af_revision;
static unsigned g_hm_af_generation;
static int g_hm_af_focus = -1;
static unsigned g_hm_af_retry_polls;

static void hm_art_hand_over(const char *rom_dir, int n)
{
   char (*paths)[PSP_FILE_PATH_CAP * 2];
   const char **pp, **np;
   int i;
   if (!artfetch_active())
   {
      g_hm_af_generation = 0;
      return;
   }
   if (g_hm_af_retry_polls)
   {
      g_hm_af_retry_polls--;
      return;
   }
   if (n < 0 || (g_hm_af_generation && n == g_hm_af_n &&
                g_pcfg.console == g_hm_af_con &&
                g_rom_view_revision == g_hm_af_revision))
      return;
   g_hm_af_generation = 0;
   g_hm_af_focus = -1;
   if (!n)
   {
      g_hm_af_generation = artfetch_list(NULL, NULL, 0);
      if (!g_hm_af_generation)
      {
         g_hm_af_retry_polls = 60;
         return;
      }
      g_hm_af_n = 0;
      g_hm_af_first = -1;
      g_hm_af_con = g_pcfg.console;
      g_hm_af_revision = g_rom_view_revision;
      g_hm_af_focus = -1;
      return;
   }
   paths = malloc((size_t)n * sizeof(*paths));
   pp = (const char **)malloc((size_t)n * sizeof(char *));
   np = (const char **)malloc((size_t)n * sizeof(char *));
   if (paths && pp && np)
   {
      for (i = 0; i < n; i++)
      {
         int r = i;
         if (psp_rom_path_join(paths[i], sizeof(paths[i]), rom_dir,
                               g_roms[r].name) != 0)
            paths[i][0] = '\0';          /* (it cannot be opened: skipped) */
         pp[i] = paths[i];
         np[i] = g_roms[r].base;
      }
      g_hm_af_generation = artfetch_list(pp, np, n);
      if (g_hm_af_generation)
      {
         g_hm_af_n = n;
         g_hm_af_first = 0;
         g_hm_af_con = g_pcfg.console;
         g_hm_af_revision = g_rom_view_revision;
         fe_evt("art_queue console=%d n=%d", g_pcfg.console, n);
      }
   }
   if (!g_hm_af_generation)
      g_hm_af_retry_polls = 60;
   free(paths);
   free(pp);
   free(np);
}

static void hm_art_focus(const char *rom_dir, int rom, int idle)
{
   char path[PSP_FILE_PATH_CAP * 2];
   if (idle < IDLE_SELECTED || !g_hm_af_generation ||
       rom < 0 || rom >= g_hm_af_n || rom == g_hm_af_focus)
      return;
   if (psp_rom_path_join(path, sizeof(path), rom_dir, g_roms[rom].name) != 0)
      return;
   artfetch_focus(g_hm_af_generation, path, g_roms[rom].base);
   g_hm_af_focus = rom;
}

static int hm_art_matches(int rom, const char *base, int count)
{
   return rom >= 0 && rom < count && g_roms[rom].base &&
          !strcmp(g_roms[rom].base, base);
}

/* A picture came for `base` (NULL: all cached games after coalesced arrivals).
 * Art made without it -- "none", tile, cover-as-hero -- is made again. */
static void hm_art_arrived(const char *base, int n)
{
   int i, count = n;
   /* Only artwork keys are marked: save metadata and shelf shots keep their
    * epoch, and the loader's current batch is not cancelled (hm_art_apply).
    * Visible textures stay until their replacement is adopted. */
   if (!base)
   {
      /* Coalesced: every raw key, whatever count the caller had (a hidden
       * Starred or Find row can own a cached "none" tile). */
      g_hm_art_all = g_hm_art_any = 1;
      for (i = 0; i < ART_SLOTS; i++)
         g_art[i].state = 0;
      g_hero_state = 0;
      return;
   }
   /* Cache ownership is raw-ROM based, including games hidden by Find. */
   for (i = 0; i < n && i < BROWSER_MAX; i++)
      if (hm_art_matches(i, base, count))
         g_hm_art_dirty[i] = 1, g_hm_art_any = 1;
   for (i = 0; i < ART_SLOTS; i++)
      if (g_art[i].state && hm_art_matches(g_art[i].rom_idx, base, count))
         g_art[i].state = 0;
   if (hm_art_matches(g_hero_idx, base, count))
      g_hero_state = 0;
}

static void hm_art_poll(const char *rom_dir, int n, int idle)
{
   char name[256], line[200];
   hm_art_hand_over(rom_dir, n);
   /* Keep visible textures until navigation settles. The worker's bounded
    * completion queue coalesces overflow and never waits for the UI. */
   if (idle >= IDLE_SELECTED)
   {
      while (artfetch_take(name, sizeof(name)))
         hm_art_arrived(name, n);
      if (artfetch_resync())
      {
         hm_art_arrived(NULL, n);
         fe_evt("art_resync console=%d n=%d", g_pcfg.console, n);
      }
   }
   while (artfetch_log(line, sizeof(line)))
      fe_evt("%s", line);
}

/* Paths are copied into requests. Neither the inventory pool nor a UI/GPU
 * object is ever referenced by the local I/O thread. */
static int hm_job(const char *rom_dir, int rom, menu_load_job *j)
{
   extern char g_dir_base[];
   size_t stem = ui_rom_stem_length(g_roms[rom].base);
   memset(j, 0, sizeof(*j));
   if (stem >= sizeof(j->stem) || strlen(g_dir_base) >= sizeof(j->root) ||
       psp_rom_path_join(j->rom, sizeof(j->rom), rom_dir, g_roms[rom].name) ||
       browser_rom_state_path(rom_dir, rom, 1, j->state1, sizeof(j->state1)))
      return 0;
   memcpy(j->stem, g_roms[rom].base, stem);
   strcpy(j->root, g_dir_base);
   j->console = g_pcfg.console;
   j->generation = g_hm_generation;
   j->epoch = hm_epoch(rom);
   j->art_epoch = hm_art_epoch(rom);
   return 1;
}
static int hm_result_rom(const char *rom_dir, int n, const menu_load_result *r)
{
   char path[ML_PATH];
   if (r->job.generation != g_hm_generation ||
       r->job.focus != g_hm_load_serial || r->job.console != g_pcfg.console)
      return -1;
   /* Only the batch's own games (at most 1 + ML_NEIGHBORS), not every
    * game in the library: the path joined per candidate was a whole-list
    * snprintf + strcmp scan for each result, on the UI thread. */
   for (unsigned k = 0; k < g_hm_batch_n; k++)
   {
      int i = g_hm_batch_rom[k];
      if (i >= 0 && i < n && i < BROWSER_MAX &&
          r->job.epoch == hm_epoch(i) &&
          ((r->kind != ML_TILE && r->kind != ML_STILL) ||
           r->job.art_epoch == hm_art_epoch(i)) &&
          psp_rom_path_join(path, sizeof(path), rom_dir, g_roms[i].name) == 0 &&
          !strcmp(path, r->job.rom))
         return i;
   }
   return -1;
}
static void hm_adopt_meta(int rom, const menu_load_result *r)
{
   hm_meta *m = &g_hm_meta[rom];
   m->value = r->meta;
   m->status = r->status;
   m->epoch = r->job.epoch;
   m->retry_us = hm_retry(r->status, &m->attempts);
   if (r->status == ML_OK)
   {
      int missing = m->value.battery == ML_MISSING;
      for (int i = 0; i < PSP_STATE_SLOT_COUNT; i++)
         missing |= m->value.state[i] == ML_MISSING;
      if (missing) m->retry_us = plat_time_us() + 30000000u;
   }
   if (m->value.battery != ML_UNKNOWN)
      g_roms[rom].has_sav = m->value.battery == ML_PRESENT;
}
static void hm_adopt_image(int rom, menu_load_result *r)
{
   vs_img *im = NULL;
   int status = r->status;
   if (status == ML_OK)
   {
      im = vs_img_new((int)r->w, (int)r->h);
      if (im) { vs_img_set(im, r->pixels, (int)r->w); g_hm_uploaded = 1; }
      else status = ML_ERROR;
   }
   if (r->kind == ML_TILE)
   {
      hm_tile *t = hm_tile_find(rom);
      if (im)
      {
         if (t->img) vs_img_free(t->img);
         t->img = im; t->state = 1;
      }
      else if (!t->img) t->state = -1;
      t->epoch = r->job.epoch;
      t->art_epoch = r->job.art_epoch;
      t->retry_us = hm_retry(status, &t->attempts);
   }
   else if (r->kind == ML_STILL)
   {
      g_hm_still_for = rom;
      g_hm_pic.epoch = r->job.epoch;
      g_hm_pic.art_epoch = r->job.art_epoch;
      g_hm_pic.retry_us = hm_retry(status, &g_hm_pic.attempts);
      if (im)
      {
         if (g_hm_pic.old) vs_img_free(g_hm_pic.old);
         g_hm_pic.old = g_hm_pic.img;
         g_hm_pic.old_rom = g_hm_pic.rom;
         g_hm_pic.old_state = g_hm_pic.state;
         g_hm_pic.old_w = g_hm_pic.w; g_hm_pic.old_h = g_hm_pic.h;
         g_hm_pic.old_a = g_hm_pic.a;
         g_hm_pic.img = im; g_hm_pic.rom = rom;
         g_hm_pic.w = (int)r->w; g_hm_pic.h = (int)r->h;
         g_hm_pic.state = r->source == ML_SOURCE_HERO ? 3 : 1;
         g_hm_pic.a = 0;
      }
   }
   else
   {
      unsigned i = 0;
      while (i < PSP_STATE_SLOT_COUNT && r->kind != ((unsigned)ML_STATE0 << i)) i++;
      if (i >= PSP_STATE_SLOT_COUNT || g_hm_cards.rom != rom)
      { if (im) vs_img_free(im); return; }
      if (im)
      {
         if (g_hm_cards.img[i]) vs_img_free(g_hm_cards.img[i]);
         g_hm_cards.img[i] = im;
         g_hm_cards.w[i] = (int)r->w; g_hm_cards.h[i] = (int)r->h;
         g_hm_cards.status[i] = 1;
      }
      else if (!g_hm_cards.img[i]) g_hm_cards.status[i] = 2;
      g_hm_cards.epoch[i] = r->job.epoch;
      g_hm_cards.retry_us[i] = hm_retry(status, &g_hm_cards.attempts[i]);
   }
}
static int hm_image_due(const hm_tile *t, int rom, uint64_t now)
{
   return t->epoch != hm_epoch(rom) || t->art_epoch != hm_art_epoch(rom) ||
          (t->retry_us && now >= t->retry_us);
}
static int hm_still_due(int rom, uint64_t now)
{
   return g_hm_still_for != rom || g_hm_pic.epoch != hm_epoch(rom) ||
          g_hm_pic.art_epoch != hm_art_epoch(rom) ||
          (g_hm_pic.retry_us && now >= g_hm_pic.retry_us);
}
/* Only a result of the batch now outstanding counts toward finishing it
 * (the worker's take already drops older revisions; this does not rely on
 * that). */
static int hm_result_current(const menu_load_result *r)
{
   return r->job.generation == g_hm_generation &&
          r->job.focus == g_hm_load_serial;
}
static unsigned hm_task_count(unsigned tasks)
{
   unsigned n = 0;
   while (tasks) { tasks &= tasks - 1; n++; }
   return n;
}
static void hm_load_pump(const char *rom_dir, int cur, int n, int n_scan, int idle)
{
   menu_load_result r;
   menu_load_job jobs[1 + ML_NEIGHBORS];
   int rom = view_rom(cur);
   unsigned count = 1, total = 0;
   uint64_t now = plat_time_us();
   g_hm_uploaded = 0;
   if (idle < IDLE_SELECTED || g_hm_load_rom != rom || g_hm_load_dirty ||
       g_hm_load_open != g_browser_state_open)
   {
      if (g_hm_load_active) menu_load_pause();
      g_hm_load_active = 0; g_hm_load_left = 0;
      g_hm_load_rom = rom; g_hm_load_open = g_browser_state_open;
      g_hm_load_dirty = 0;
      if (idle < IDLE_SELECTED) return;
   }
   menu_load_resume();
   for (unsigned i = 0; i < 4 && menu_load_take(0, &r); i++)
   {
      unsigned long long began = md_begin();
      int at = hm_result_rom(rom_dir, n_scan, &r);
      if (at >= 0) hm_adopt_meta(at, &r);
      if (hm_result_current(&r) && g_hm_load_left) g_hm_load_left--;
      menu_load_release(r.slot);
      md_end(MD_PUBLISH, began);
   }
   if (menu_load_take(1, &r))
   {
      unsigned long long began = md_begin();
      int at = hm_result_rom(rom_dir, n_scan, &r);
      if (at >= 0) hm_adopt_image(at, &r);
      if (hm_result_current(&r) && g_hm_load_left) g_hm_load_left--;
      menu_load_release(r.slot);
      md_end(MD_PUBLISH, began);
   }
   if (!g_hm_load_left) g_hm_load_active = 0;
   if (g_hm_load_active) return;
   /* The batch boundary: artwork that arrived meanwhile becomes due now. */
   hm_art_apply();
   if (!hm_job(rom_dir, rom, &jobs[0])) return;
   g_hm_batch_rom[0] = rom;
   const hm_meta *m = hm_metadata(rom);
   if (!m || (m->retry_us && now >= m->retry_us)) jobs[0].tasks |= ML_META;
   hm_tile *t = hm_tile_find(rom);
   if (hm_image_due(t, rom, now)) jobs[0].tasks |= ML_TILE;
   if (hm_still_due(rom, now)) jobs[0].tasks |= ML_STILL;
   if (g_browser_state_open)
   {
      if (g_hm_cards.rom != rom) { hm_cards_drop(); g_hm_cards.rom = rom; }
      for (unsigned i = 0; m && i < PSP_STATE_SLOT_COUNT; i++)
      {
         strcpy(g_hm_cards.when[i], m->value.when[i]);
         if ((m->value.state[i] == ML_PRESENT || m->value.state[i] == ML_OLD_ONLY) &&
             (g_hm_cards.epoch[i] != hm_epoch(rom) ||
              (g_hm_cards.retry_us[i] && now >= g_hm_cards.retry_us[i])))
            jobs[0].tasks |= ML_STATE0 << i;
      }
   }
   /* Only the nearby row is optional, distance-ordered and bounded: the
    * one tile hm_row draws to the left and those to the right that are on
    * screen (it shows up to five), wrapping round a list of more than ten
    * exactly as hm_row does. */
   if (idle >= IDLE_HERO && !g_browser_state_open)
   {
      for (int d = 1; d <= ML_NEIGHBORS && count < 1 + ML_NEIGHBORS; d++)
         for (int side = 0; side < 2 && count < 1 + ML_NEIGHBORS; side++)
         {
            int vi = cur + (side ? -d : d), at;
            if (side && d > 1) continue;
            if (n > 10) vi = ((vi % n) + n) % n;
            else if (vi < 0 || vi >= n) continue;
            at = view_rom(vi);
            if (at == rom) continue;
            t = hm_tile_find(at);
            if (t->x >= VS_W || !hm_image_due(t, at, now)) continue;
            if (hm_job(rom_dir, at, &jobs[count]))
            {
               g_hm_batch_rom[count] = at;
               jobs[count++].tasks = ML_TILE;
            }
         }
   }
   for (unsigned i = 0; i < count; i++) total += hm_task_count(jobs[i].tasks);
   if (!total) return;
   if (++g_hm_load_serial == 0) ++g_hm_load_serial;
   for (unsigned i = 0; i < count; i++) jobs[i].focus = g_hm_load_serial;
   if (menu_load_submit(jobs, count))
   { g_hm_load_active = 1; g_hm_load_left = total; g_hm_batch_n = count; }
   else
      g_hm_batch_n = 0;
}

/* Settings and cheats cover Home.  Entering parks the loader without
 * waiting for it (a read in progress is abandoned on its own thread, its
 * result discarded) and forgets the outstanding batch, so nothing is read or
 * published for a page that is not shown.  Leaving changes nothing at once:
 * the next settled Home frame resumes the worker with a fresh batch.  After
 * Settings every artwork key is marked (covers can be looked for again
 * there); saves are not touched by either page. */
static void home_modal_enter(void)
{
   menu_load_pause();
   g_hm_load_active = 0;
   g_hm_load_left = 0;
   g_hm_load_dirty = 1;
}
static void home_modal_leave(int artwork_may_change)
{
   if (artwork_may_change)
      g_hm_art_all = g_hm_art_any = 1;
   g_hm_load_active = 0;
   g_hm_load_left = 0;
   g_hm_load_dirty = 1;
}

/* ---- the whole page ---------------------------------------------------- */

static float g_hm_lift;               /* 0 = the row, 1 = the states        */

static void shell_home(const char *rom_dir, int cur, int n, int n_scan,
                       int idle)
{
   int rom = view_rom(cur);
   float dy;

   hm_selection_tick(rom);
   hm_art_poll(rom_dir, n_scan, idle);
   hm_art_focus(rom_dir, rom, idle);
   hm_load_pump(rom_dir, cur, n, n_scan, idle);
   g_thm = &THM_DARK;
   g_hm_frame++;
   hm_ease(&g_hm_lift, g_browser_state_open ? 1.0f : 0.0f, 0.22f);
   dy = -HM_LIFT * g_hm_lift;
   if (g_browser_state_open && g_hm_lift > 0.99f)
      dy = -HM_LIFT;

   /* the picture: loaded when the row has settled on a game */
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

   hm_clip_tick(rom, idle);
   hm_backdrop(rom, dy);
   hm_topbar(dy, n_scan);
   hm_row(rom_dir, cur, n, dy, idle);
   hm_focus(rom_dir, rom, dy, idle);
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
      static const int ic[3] = { VS_I_BTN_O, VS_I_BTN_TRI, -1 };
      const char *w[3] = { g_find[0] ? "Clear" : "Find", "Console", "START  Settings" };
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
   hm_selection_tick(-1);
   g_thm = &THM_DARK;
   vs_rect(0, 0, VS_W, VS_H, HC_BG);
   vs_shadow(330, 120, 300, 220, 110, 160, hm_tint(con, 60));
   hm_topbar(0, n_scan);
   vs_circle(VS_W * 0.5f, y + 20, 56, HC_GLASS);
   if (g_find[0])
   {
      char query[96];
      vs_icon(VS_I_SEARCH, VS_W * 0.5f, y + 36, 56, HC_TEXT3);
      vs_text_c(VS_SUB, VS_W * 0.5f, y + 94, "No games match", HC_TEXT);
      int elsewhere = g_browser_favs || g_browser_recent
                    ? browser_find_count_all(n_scan) : 0;
      snprintf(query, sizeof(query), "\"%s\"", g_find);
      vs_text_fit(VS_BODY, query, 760);
      vs_text_c(VS_BODY, VS_W * 0.5f, y + 136, query, HC_TEXT2);
      if (elsewhere > 0)
      {
         /* Nothing starred matches, but games do: one press shows them
          * (SELECT keeps the search). */
         float w1 = vs_text_w(VS_LABEL, "Clear") + 76;
         float w2 = vs_text_w(VS_LABEL, "All games") + 76, x;
         snprintf(msg, sizeof(msg), elsewhere == 1
                  ? "1 game matches in All games"
                  : "%d games match in All games", elsewhere);
         vs_text_c(VS_CAPTION, VS_W * 0.5f, y + 174, msg, HC_TEXT2);
         x = VS_W * 0.5f - (w1 + 14 + w2) * 0.5f;
         x = hm_button(x, y + 216, VS_I_BACK, "Clear", 0, PSP_CTRL_CIRCLE);
         hm_button(x, y + 216, VS_I_GRID, "All games", 1,
                   g_browser_recent ? BROWSER_TAB_BUTTON(0) : PSP_CTRL_SELECT);
      }
      else
      {
         vs_text_c(VS_CAPTION, VS_W * 0.5f, y + 174,
                   "Try fewer words, or part of a word.", HC_TEXT3);
         hm_button(VS_W * 0.5f - 90, y + 216, VS_I_BACK, "Clear", 1,
                   PSP_CTRL_CIRCLE);
      }
      return;
   }
   else if (g_browser_recent)
   {
      vs_icon(VS_I_REWIND, VS_W * 0.5f, y + 20, 56, hm_tint(con, 255));
      snprintf(msg, sizeof(msg), "No %s games played yet", sk->id);
      vs_text_c(VS_TITLE, VS_W * 0.5f, y + 96, msg, HC_TEXT);
      vs_text_c(VS_BODY, VS_W * 0.5f, y + 156,
                "The games you play show up here, the latest first.", HC_TEXT2);
      hm_button(VS_W * 0.5f - 110, y + 216, VS_I_GRID, "All games", 1,
                BROWSER_TAB_BUTTON(0));
      return;
   }
   else if (g_browser_favs)
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
      if (e & PSP_CTRL_UP)    out |= BROWSER_FRESH_BUTTON;  /* Start over */
   }
   return out;
}

static void home_free(void)
{
   int i;
   menu_load_pause();
   if (++g_hm_generation == 0) ++g_hm_generation;
   memset(g_hm_meta, 0, sizeof(g_hm_meta));
   memset(g_hm_epoch, 0, sizeof(g_hm_epoch));
   memset(g_hm_art_epoch, 0, sizeof(g_hm_art_epoch));
   memset(g_hm_art_dirty, 0, sizeof(g_hm_art_dirty));
   g_hm_art_any = g_hm_art_all = 0;
   g_hm_load_rom = g_hm_load_open = g_hm_still_for = -1;
   g_hm_load_active = 0; g_hm_load_left = g_hm_load_dirty = 0;
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
   g_hm_blurb_for = -1;
   g_hm_blurb[0] = '\0';
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
   (void)menu_load_start(menu_files_load);
   menu_load_resume();
   g_hm_clip.none_for = -1;
   g_hm_lift = 0.0f;
}

#endif /* UI_VITA_HOME_H */
