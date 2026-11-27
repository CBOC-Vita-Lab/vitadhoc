/* osd_psp.c — toast queue + status chips (see osd_psp.h). */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "osd_psp.h"
#include "video_psp.h"
#include "font_8x16.h"
#ifdef VITA
#include "vstyle.h"       /* a page's toast is the app's rounded glass */
#endif

#define TOAST_MAX     4
#define TOAST_FRAMES  150   /* ~2.5 s per toast */
#define TOAST_LEN     168  /* "Hosting Room 7 by Link Cable. Waiting for the other PSP."; names in Japanese take 3 bytes a letter */

#define COL_TEXT   0xFFFF   /* white          */
#define COL_PANEL  0x0000   /* black          */
#define COL_ACCENT 0x65BF   /* soft blue      */
#define COL_FF     0xFEA0   /* warm yellow    */

static char     toast_q[TOAST_MAX][TOAST_LEN];
static unsigned toast_head, toast_count;
static int      toast_timer;

static char chip_session_txt[32];
static char chip_ff_txt[24];   /* "<ff> Unlimited Smooth" + NUL */
/* Wide enough for the full diagnostic line, e.g.
 * "24.0 fps (24)  s1700 f1% x48000 r0" (34 chars).  This was 24 and silently
 * truncated the profiler fields mid-string — the chip looked like it simply
 * had no x/r counters.  480px / 8px per glyph = 60 chars fit on screen. */
static char chip_fps_txt[64];

/* ---- achievement indicators -------------------------------------------- */
#define IND_CHAL_MAX    8
#define IND_PROGRESS_FRAMES 300     /* a progress pill the runtime never hid */
static int      ind_on = 1;
static struct { unsigned id; char ini; } ind_chal[IND_CHAL_MAX];
static int      ind_chal_n;
static char     ind_progress_txt[160], ind_tracker_txt[24];
static int      ind_progress_timer;

void osd_ind_enable(int on)
{
   ind_on = on ? 1 : 0;
   if (!ind_on)
      osd_ind_clear();
}

void osd_ind_clear(void)
{
   ind_chal_n = 0;
   ind_progress_txt[0] = ind_tracker_txt[0] = '\0';
   ind_progress_timer = 0;
}

void osd_ind_challenge(unsigned id, int on, const char *title)
{
   int i;
   if (!ind_on)
      return;
   for (i = 0; i < ind_chal_n; i++)
      if (ind_chal[i].id == id)
         break;
   if (on && i == ind_chal_n && ind_chal_n < IND_CHAL_MAX)
   {
      char c = title && title[0] ? title[0] : '!';
      ind_chal[i].id = id;
      ind_chal[i].ini = c >= 'a' && c <= 'z' ? (char)(c - 32) : c;
      ind_chal_n++;
   }
   else if (!on && i < ind_chal_n)
   {
      for (; i + 1 < ind_chal_n; i++)
         ind_chal[i] = ind_chal[i + 1];
      ind_chal_n--;
   }
}

void osd_ind_progress(const char *title, const char *progress)
{
   if (!ind_on || !progress || !progress[0])
   {
      ind_progress_txt[0] = '\0';
      ind_progress_timer = 0;
      return;
   }
   snprintf(ind_progress_txt, sizeof(ind_progress_txt), "%s%s%s",
            title ? title : "", title && title[0] ? "  " : "", progress);
   ind_progress_timer = IND_PROGRESS_FRAMES;
}

void osd_ind_tracker(const char *text)
{
   if (!ind_on || !text || !text[0])
      ind_tracker_txt[0] = '\0';
   else
      snprintf(ind_tracker_txt, sizeof(ind_tracker_txt), "%s", text);
}

void osd_toast(const char *fmt, ...)
{
   va_list ap;
   unsigned slot;

   if (toast_count == TOAST_MAX)   /* full: drop the oldest */
   {
      toast_head = (toast_head + 1) % TOAST_MAX;
      toast_count--;
      toast_timer = TOAST_FRAMES;
   }
   slot = (toast_head + toast_count) % TOAST_MAX;
   va_start(ap, fmt);
   vsnprintf(toast_q[slot], TOAST_LEN, fmt, ap);
   va_end(ap);
   if (toast_count == 0)
      toast_timer = TOAST_FRAMES;
   toast_count++;
}

#ifdef VITA
/* In a menu the toast is a strip on the screen's top edge, 17 rows: the
 * wordmark begins on row 17. */
static int toast_y = 6, toast_slim, toast_glass;
void osd_toast_place(int where)
{
   /* A page's toast (a menu, the game list) sits 24 px above the footer,
    * in rounded glass: at the top edge it was under the header and at the
    * bottom it was cut off by the screen.  Over the game it is the PSP's
    * strip, unchanged. */
   toast_glass = where != OSD_TOAST_GAME;
   toast_slim = where == OSD_TOAST_MENU;
   toast_y = where == OSD_TOAST_MENU ? 0
           : where == OSD_TOAST_LIST ? VID_SCR_H - 20 - (FE_FONT_H + 8) - 4 : 6;
}
#define TOAST_Y    toast_y
#define TOAST_H    (toast_slim ? 17 : FE_FONT_H + 8)
#define TOAST_TY   (toast_slim ? 0 : toast_y + 4)
#define TOAST_LINE (toast_slim ? 16 : toast_y)
#else
#define TOAST_Y    6
#define TOAST_H    (FE_FONT_H + 8)
#define TOAST_TY   10
#define TOAST_LINE 6
#endif

void osd_reset(void)
{
   toast_head = toast_count = 0;
   toast_timer = 0;
   chip_session_txt[0] = '\0';
   chip_ff_txt[0] = '\0';
   chip_fps_txt[0] = '\0';
   osd_ind_clear();
}

void osd_chip_session(const char *text)
{
   if (text && text[0])
      snprintf(chip_session_txt, sizeof(chip_session_txt), "%s", text);
   else
      chip_session_txt[0] = '\0';
}

void osd_chip_ff(const char *text)
{
   if (text && text[0])
      snprintf(chip_ff_txt, sizeof(chip_ff_txt), "%s", text);
   else
      chip_ff_txt[0] = '\0';
}

void osd_chip_fps(const char *text)
{
   if (text && text[0])
      snprintf(chip_fps_txt, sizeof(chip_fps_txt), "%s", text);
   else
      chip_fps_txt[0] = '\0';
}

void osd_take(osd_snap *s)
{
   int i;
   if (ind_progress_txt[0] && --ind_progress_timer <= 0)
      ind_progress_txt[0] = '\0';
   s->any = toast_count || chip_session_txt[0] || chip_ff_txt[0] ||
            chip_fps_txt[0] || ind_chal_n || ind_progress_txt[0] ||
            ind_tracker_txt[0];
   s->toast[0] = '\0';
   memcpy(s->ind_tracker, ind_tracker_txt, sizeof(s->ind_tracker));
   memcpy(s->ind_progress, ind_progress_txt, sizeof(s->ind_progress));
   s->ind_chal_n = (short)ind_chal_n;
   for (i = 0; i < ind_chal_n; i++)
      s->ind_chal[i] = ind_chal[i].ini;
   if (!s->any)
      return;
   if (toast_count)
   {
      memcpy(s->toast, toast_q[toast_head], sizeof(s->toast));
      s->toast[sizeof(s->toast) - 1] = '\0';
      if (--toast_timer <= 0)
      {
         toast_head = (toast_head + 1) % TOAST_MAX;
         toast_count--;
         toast_timer = TOAST_FRAMES;
      }
   }
   memcpy(s->chip_session, chip_session_txt, sizeof(s->chip_session));
   memcpy(s->chip_ff, chip_ff_txt, sizeof(s->chip_ff));
   memcpy(s->chip_fps, chip_fps_txt, sizeof(s->chip_fps));
#ifdef VITA
   s->toast_y = (short)toast_y;
   s->toast_slim = (short)toast_slim;
   s->toast_glass = (short)toast_glass;
#else
   s->toast_y = TOAST_Y;
   s->toast_slim = 0;
   s->toast_glass = 0;
#endif
}

void osd_draw_snap(const osd_snap *s)
{
   if (!s || !s->any)
      return;

   vid_overlay_begin(0);

#ifdef VITA
   if (s->toast[0] && s->toast_glass)
   {
      /* Vita pixels (960x544).  The footer is 20 logical rows tall. */
      char msg[sizeof(s->toast)];
      float tw, pw, py;
      memcpy(msg, s->toast, sizeof(msg));
      vs_text_fit(VS_LABEL, msg, VS_W - 120);
      tw = vs_text_w(VS_LABEL, msg);
      pw = tw + 48;
      py = (float)(VS_H - 2 * 20 - 24 - 48);
      vs_rrect((VS_W - pw) / 2, py, pw, 48, 24, VS_HEX(0x1A2340, 240));
      vs_text_c(VS_LABEL, VS_W / 2.0f, py + 12, msg, VS_HEX(0xFFFFFF, 255));
   }
   else
#endif
   if (s->toast[0])
   {
      const char *msg = s->toast;
      const int slim = s->toast_slim, ty = s->toast_y;
      const int th = slim ? 17 : FE_FONT_H + 8;
      const int tty = slim ? 0 : ty + 4;
      const int line = slim ? 16 : ty;
      int tw = vid_text_w(msg);
      int px = (VID_SCR_W - tw) / 2 - 8;
      int pw = tw + 16;
      vid_rect(px, ty, pw, th, COL_PANEL, 200);
      vid_rect(px, line, pw, 2, COL_ACCENT, 255);
      vid_text_center(tty, msg, COL_TEXT);
   }

   if (s->chip_fps[0])
   {
      /* Top-left, mirroring the FF chip's top-right — the two are designed
       * to be on screen together (FPS is most interesting DURING FF). */
      int tw = vid_text_w(s->chip_fps);
      vid_rect(10, 6, tw + 12, FE_FONT_H + 4, COL_PANEL, 180);
      vid_text(16, 8, s->chip_fps, COL_TEXT);
   }

   if (s->chip_ff[0])
   {
      int tw = vid_text_w(s->chip_ff);
      int px = VID_SCR_W - tw - 14;
      vid_rect(px - 4, 6, tw + 12, FE_FONT_H + 4, COL_PANEL, 180);
      vid_text(px + 2, 8, s->chip_ff, COL_FF);
   }

   if (s->chip_session[0])
   {
      int tw = vid_text_w(s->chip_session);
      int px = VID_SCR_W - tw - 14;
      int py = VID_SCR_H - FE_FONT_H - 8;
      vid_rect(px - 4, py - 2, tw + 12, FE_FONT_H + 4, COL_PANEL, 180);
      vid_text(px + 2, py, s->chip_session, COL_ACCENT);
   }

   if (!s->toast_glass)
   {
      /* Indicators: small, in the corners, over the game's edge only.  The
       * challenge chips sit above the session chip's row at the bottom
       * right, the tracker at the bottom left, the progress pill a line
       * under the toasts. */
      int i;
      if (s->ind_progress[0])
      {
         int tw = vid_text_w(s->ind_progress);
         int px = (VID_SCR_W - tw - 28) / 2, py = 28;
         vid_rect(px, py, tw + 28, FE_FONT_H + 6, COL_PANEL, 200);
         vid_rect(px, py, 18, FE_FONT_H + 6, COL_FF, 255);
         vid_text(px + 5, py + 3, "*", COL_PANEL);
         vid_text(px + 22, py + 3, s->ind_progress, COL_TEXT);
      }
      if (s->ind_tracker[0])
      {
         int tw = vid_text_w(s->ind_tracker);
         int py = VID_SCR_H - FE_FONT_H - 10;
         vid_rect(8, py - 2, tw + 12, FE_FONT_H + 4, COL_PANEL, 190);
         vid_rect(8, py - 2, 2, FE_FONT_H + 4, COL_ACCENT, 255);
         vid_text(14, py, s->ind_tracker, COL_TEXT);
      }
      for (i = 0; i < s->ind_chal_n; i++)
      {
         char g[2];
         int px = VID_SCR_W - 8 - (i + 1) * 18, py = VID_SCR_H - 52;
         g[0] = s->ind_chal[i];
         g[1] = '\0';
         vid_rect(px, py, 16, 16, COL_PANEL, 200);
         vid_rect(px, py, 16, 2, COL_FF, 255);
         vid_text(px + (16 - vid_text_w(g)) / 2, py + 1, g, COL_FF);
      }
   }

   vid_overlay_end();
}

/* One pass, on the thread that owns everything (the menus, the PSP). */
void osd_draw(void)
{
   osd_snap s;
   osd_take(&s);
   osd_draw_snap(&s);
}
