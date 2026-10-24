/* ui_vita_touch.h -- the front panel in the menus and the game list.
 *
 * Included by psp/ui_psp.c under -DVITA (it uses that file's includes and
 * nothing else of it).  The pad's handling is NOT touched: touch arrives as
 * the same things the pad produces -- a cursor position and button edges --
 * so every screen keeps one input path.
 *
 * HOW IT WORKS.  Screens are drawn top to bottom every frame.  While it
 * draws, a screen says where its touchable things are:
 *
 *     tch_row(id, x, y, w, h)     a list row; `id` is the cursor value that
 *                                 selects it
 *     tch_btn(button, x, y, w, h) something that acts like a pad button
 *     tch_hints(x, y, text)       a line of footer hints: every "X select",
 *                                 "O back", "START settings" in it becomes
 *                                 a tch_btn for that button
 *
 * and the next frame's input is read against what was registered -- the
 * frame the player was looking at when the finger moved.  Regions drawn
 * later lie on top (a question plate over a list); tch_modal() makes
 * everything registered before it untouchable.
 *
 * WHAT A FINGER DOES
 *   lands on a row          the cursor goes there at once (tch_rows), so
 *                           the selection band shows what will happen
 *   lifts without moving    a TAP: a row -> X on that row, a button ->
 *                           that button's edge
 *   moves first             no tap.  Inside a scrolling list the list
 *                           follows the finger and coasts when it lifts
 *                           (tch_scroll / tch_steps), and a finger put on
 *                           a coasting list only stops it; elsewhere the cursor
 *                           follows the finger from row to row and nothing
 *                           is activated -- sliding off is how to cancel
 *
 * SIZES.  The Vita's panel is 220 dpi and the UI's logical pixel is two
 * panel pixels, so 1 mm = 4.33 logical px and the 9 mm a fingertip wants is
 * 39 px.  Rows are as WIDE as their band (60 mm and more) but only as tall
 * as the list's pitch -- 22 to 34 px, 5 to 8 mm, the PSP layouts'.  Three
 * things make that workable without redrawing every screen: a touch between
 * two rows goes to the nearer one (no dead gaps), the row under the finger
 * lights up BEFORE anything happens, and footer hints are padded to
 * TCH_HINT_H x TCH_HINT_MINW.  New keypads (the address and code pages) are
 * laid out at 9 mm outright.
 */
#ifndef UI_VITA_TOUCH_H
#define UI_VITA_TOUCH_H

#define TCH_MAX        128
#define TCH_SLOP       7      /* px a finger may wander and still tap (3 mm
                               * across both directions)                    */
#define TCH_NEAR       12     /* px outside a row that still count as it    */
#define TCH_HINT_Y     240    /* a footer hint's box starts here (the bar
                               * itself is 252..272)                        */
#define TCH_HINT_MINW  44
#define TCH_HINT_PAD   7

enum { TCH_K_ROW = 1, TCH_K_BTN };

typedef struct
{
   short x, y, w, h;
   int   id;                  /* row: the cursor value                      */
   unsigned btn;              /* button: PSP_CTRL_*                         */
   unsigned char kind;
} tch_rgn;

static struct
{
   tch_rgn prev[TCH_MAX], cur[TCH_MAX];
   int n_prev, n_cur;
   int down, was_down;        /* this frame / the frame before              */
   int dead;                  /* this touch is ignored until it lifts (a
                               * page changed under the finger)             */
   int x, y;                  /* where the finger is (or last was)          */
   int x0, y0;                /* where it landed                            */
   int dx, dy;                /* movement this frame                        */
   int moved;                 /* left the slop since landing                */
   int claimed;               /* a scrolling list took this drag            */
   int land;                  /* index into prev[] of what it landed on, -1 */
   tch_rgn land_r;            /* a copy: prev[] is rewritten every frame    */
   /* this frame's events */
   int tap_row;               /* row id tapped, or TCH_NONE                 */
   unsigned tap_btn;          /* buttons tapped                             */
   int hover_row;             /* row id under the finger, or TCH_NONE       */
   int v256;                  /* drag velocity, px/frame * 256 (smoothed)   */
   int vx256;                 /* horizontal velocity, independent of Y     */
   int axis;                  /* first motion past slop: 1 horizontal, 2 Y  */
   int used;                  /* a touch has been seen in this run          */
} g_tch;

#define TCH_NONE (-0x7fffffff)

static void tch_add(int kind, int id, unsigned btn, int x, int y, int w, int h)
{
   tch_rgn *r;
   if (g_tch.n_cur >= TCH_MAX || w <= 0 || h <= 0)
      return;
   r = &g_tch.cur[g_tch.n_cur++];
   r->x = (short)x; r->y = (short)y; r->w = (short)w; r->h = (short)h;
   r->id = id; r->btn = btn; r->kind = (unsigned char)kind;
}

static void tch_row(int id, int x, int y, int w, int h)
{
   tch_add(TCH_K_ROW, id, 0, x, y, w, h);
}

static void tch_btn(unsigned btn, int x, int y, int w, int h)
{
   tch_add(TCH_K_BTN, 0, btn, x, y, w, h);
}

/* Everything registered so far this frame stops answering: a dialog is
 * about to be drawn over it.  `outside` is the button a tap beside the
 * dialog stands for (PSP_CTRL_CIRCLE where backing out is safe), or 0 to
 * swallow it. */
static void tch_modal(unsigned outside)
{
   g_tch.n_cur = 0;
   tch_add(TCH_K_BTN, 0, outside, 0, 0, VID_SCR_W, VID_SCR_H);
}

/* The pad button a hint stands for, from how it starts: one of the baked
 * button glyphs, or a button's name followed by a blank or a colon.  0 =
 * the hint is information ("DPAD move", "blank = disabled"). */
static unsigned tch_hint_button(const char *s, int len)
{
   static const struct { const char *word; unsigned btn; } w[] = {
      { "START", PSP_CTRL_START }, { "SELECT", PSP_CTRL_SELECT },
      { "L/R", PSP_CTRL_RTRIGGER }, { "Left", PSP_CTRL_LEFT },
   };
   unsigned i;
   if (len <= 0)
      return 0;
   switch ((unsigned char)s[0])
   {
   case 0x7F: return PSP_CTRL_TRIANGLE;
   case 0x80: return PSP_CTRL_CIRCLE;
   case 0x81: return PSP_CTRL_CROSS;
   case 0x82: return PSP_CTRL_SQUARE;
   default:   break;
   }
   for (i = 0; i < sizeof(w) / sizeof(w[0]); i++)
   {
      int n = (int)strlen(w[i].word);
      if (len > n && !strncmp(s, w[i].word, (size_t)n) &&
          (s[n] == ' ' || s[n] == ':'))
         return w[i].btn;
   }
   return 0;
}

/* A line of hints drawn with vid_text at x: split at runs of two or more
 * blanks, one button box per hint that names a button, each box_y..box_y +
 * box_h tall (the footer's: tch_hints). */
static void tch_hints_box(int x, const char *text, int box_y, int box_h)
{
   char pre[200];
   int i = 0, n = text ? (int)strlen(text) : 0;
   while (i < n)
   {
      int s, e;
      unsigned b;
      while (i < n && text[i] == ' ')
         i++;
      s = i;
      while (i < n && !(text[i] == ' ' && i + 1 < n && text[i + 1] == ' '))
         i++;
      e = i;
      while (e > s && text[e - 1] == ' ')
         e--;
      b = tch_hint_button(text + s, e - s);
      if (b && s < (int)sizeof(pre) && e - s < (int)sizeof(pre))
      {
         int x0, w;
         memcpy(pre, text, (size_t)s);
         pre[s] = '\0';
         x0 = x + vid_text_w(pre);
         memcpy(pre, text + s, (size_t)(e - s));
         pre[e - s] = '\0';
         w = vid_text_w(pre);
         if (w < TCH_HINT_MINW - 2 * TCH_HINT_PAD)
         {
            x0 -= (TCH_HINT_MINW - 2 * TCH_HINT_PAD - w) / 2;
            w = TCH_HINT_MINW - 2 * TCH_HINT_PAD;
         }
         tch_btn(b, x0 - TCH_HINT_PAD, box_y, w + 2 * TCH_HINT_PAD, box_h);
      }
   }
}

/* The footer's hints (the bar is 252..272; the boxes reach up to 240). */
static void tch_hints(int x, int y, const char *text)
{
   (void)y;
   tch_hints_box(x, text, TCH_HINT_Y, VID_SCR_H - TCH_HINT_Y);
}

/* The same for a centred line (vid_text_center). */
static void tch_hints_center(int y, const char *text)
{
   tch_hints((VID_SCR_W - vid_text_w(text)) / 2, y, text);
}

/* What lies under px,py in the frame the player is looking at: the topmost
 * region containing it, else the nearest row within TCH_NEAR above or below
 * (rows only: a button's box is already padded).  -1 = nothing. */
static int tch_find(int px, int py)
{
   int i, best = -1, best_d = TCH_NEAR + 1;
   for (i = g_tch.n_prev - 1; i >= 0; i--)
   {
      const tch_rgn *r = &g_tch.prev[i];
      if (px >= r->x && px < r->x + r->w && py >= r->y && py < r->y + r->h)
         return i;
   }
   for (i = g_tch.n_prev - 1; i >= 0; i--)
   {
      const tch_rgn *r = &g_tch.prev[i];
      int d;
      if (r->kind != TCH_K_ROW || px < r->x || px >= r->x + r->w)
         continue;
      d = py < r->y ? r->y - py : py - (r->y + r->h - 1);
      if (d < best_d)
      {
         best_d = d;
         best = i;
      }
   }
   return best;
}

/* Once per UI frame, before the screen handles input. */
static void tch_poll(void)
{
   pc_touch t;
   int was = g_tch.down;

   memcpy(g_tch.prev, g_tch.cur, sizeof(tch_rgn) * (size_t)g_tch.n_cur);
   g_tch.n_prev = g_tch.n_cur;
   g_tch.n_cur = 0;
   g_tch.tap_row = TCH_NONE;
   g_tch.hover_row = TCH_NONE;
   g_tch.tap_btn = 0;
   g_tch.dx = g_tch.dy = 0;

   memset(&t, 0, sizeof(t));
   if (g_pcfg.touch)
      pspcompat_touch(&t);
   g_tch.was_down = was;
   g_tch.down = t.down;
   if (t.down)
      g_tch.used = 1;

   if (t.down && !was)
   {
      g_tch.x0 = g_tch.x = t.x;
      g_tch.y0 = g_tch.y = t.y;
      g_tch.moved = 0;
      g_tch.claimed = 0;
      g_tch.dead = 0;
      g_tch.v256 = 0;
      g_tch.vx256 = 0;
      g_tch.axis = 0;
      g_tch.land = tch_find(t.x, t.y);
      if (g_tch.land >= 0)
         g_tch.land_r = g_tch.prev[g_tch.land];
   }
   else if (t.down)
   {
      g_tch.dx = t.x - g_tch.x;
      g_tch.dy = t.y - g_tch.y;
      g_tch.x = t.x;
      g_tch.y = t.y;
      if (abs(t.x - g_tch.x0) > TCH_SLOP || abs(t.y - g_tch.y0) > TCH_SLOP)
      {
         if (!g_tch.moved)
            g_tch.axis = abs(t.x - g_tch.x0) >= abs(t.y - g_tch.y0) ? 1 : 2;
         g_tch.moved = 1;
      }
      g_tch.v256 = (g_tch.v256 + g_tch.dy * 256) / 2;
      g_tch.vx256 = (g_tch.vx256 + g_tch.dx * 256) / 2;
   }
   if (g_tch.dead)
   {
      if (!t.down)
         g_tch.dead = 0;
      return;
   }
   if (t.down)
   {
      /* The row under the finger.  Until it moves that is the row it
       * landed on (which may have scrolled a pixel since); after, whatever
       * is under it now -- unless a list is scrolling under it. */
      if (!g_tch.moved)
      {
         if (g_tch.land >= 0 && g_tch.land_r.kind == TCH_K_ROW)
            g_tch.hover_row = g_tch.land_r.id;
      }
      else if (!g_tch.claimed)
      {
         int i = tch_find(t.x, t.y);
         if (i >= 0 && g_tch.prev[i].kind == TCH_K_ROW)
            g_tch.hover_row = g_tch.prev[i].id;
      }
   }
   else if (was && !g_tch.moved && g_tch.land >= 0)
   {
      if (g_tch.land_r.kind == TCH_K_ROW)
         g_tch.tap_row = g_tch.land_r.id;
      else
         g_tch.tap_btn = g_tch.land_r.btn;
   }
}

/* The page changed (by the pad, or by the tap that just fired): forget the
 * frame that was on screen, and ignore a finger that is still down. */
static void tch_reset(void)
{
   g_tch.n_cur = g_tch.n_prev = 0;
   g_tch.tap_row = g_tch.hover_row = TCH_NONE;
   g_tch.tap_btn = 0;
   g_tch.land = -1;
   if (g_tch.down)
      g_tch.dead = 1;
}

/* A screen's rows: the cursor follows the finger; a tap is X on the row.
 * Returns 1 on the frame of a tap. */
static int tch_rows(int *cursor, unsigned *edges)
{
   if (g_tch.hover_row != TCH_NONE)
      *cursor = g_tch.hover_row;
   if (g_tch.tap_row != TCH_NONE)
   {
      *cursor = g_tch.tap_row;
      g_tch.tap_row = TCH_NONE;
      if (edges)
         *edges |= PSP_CTRL_CROSS;
      return 1;
   }
   return 0;
}

/* A tapped row's id without the cursor-follows-finger part (the game list:
 * a tap SELECTS, a second tap -- or the box art -- starts the game). */
static int tch_tapped_row(int *id)
{
   if (g_tch.tap_row == TCH_NONE)
      return 0;
   *id = g_tch.tap_row;
   g_tch.tap_row = TCH_NONE;
   return 1;
}

/* The button edges tapped this frame (consumed). */
static unsigned tch_buttons(void)
{
   unsigned b = g_tch.tap_btn;
   g_tch.tap_btn = 0;
   return b;
}

/* ---- scrolling --------------------------------------------------------- */

typedef struct
{
   int v256;                  /* coasting speed, px/frame * 256             */
   int acc;                   /* tch_steps: px not yet turned into a row    */
   int on;                    /* the offset is the finger's this frame      */
} tch_scroll_t;

static int tch_in(int vx, int vy, int vw, int vh)
{
   return g_tch.x0 >= vx && g_tch.x0 < vx + vw && g_tch.y0 >= vy &&
          g_tch.y0 < vy + vh;
}

/* A list with a pixel offset (`*off`, 0..max): a drag that began inside its
 * viewport moves it, and it coasts when the finger lifts.  Returns 1 while
 * touch owns the offset (the caller's "follow the cursor" must stand
 * back). */
static int tch_scroll(tch_scroll_t *s, int *off, int max, int vx, int vy,
                      int vw, int vh)
{
   if (max < 0)
      max = 0;
   if (g_tch.down && !g_tch.was_down && s->on && abs(s->v256) >= 96 &&
       tch_in(vx, vy, vw, vh))
   {
      /* A finger put on a coasting list stops it, and that is all it
       * does: lifting again is not a tap on whatever row stopped there. */
      g_tch.moved = 1;
      g_tch.claimed = 1;
      s->v256 = 0;
   }
   else if (g_tch.down && !g_tch.dead && g_tch.moved && tch_in(vx, vy, vw, vh))
   {
      g_tch.claimed = 1;
      *off -= g_tch.dy;
      s->v256 = g_tch.v256;
      s->on = 1;
   }
   else if (s->on && !g_tch.down && abs(s->v256) >= 96)
   {
      *off -= s->v256 / 256;
      s->v256 = s->v256 * 15 / 16;
   }
   else if (!g_tch.down)
   {
      s->v256 = 0;
      s->on = 0;
   }
   if (*off < 0) { *off = 0; s->v256 = 0; }
   if (*off > max) { *off = max; s->v256 = 0; }
   return s->on;
}

/* A list that moves a whole row at a time (the game list's carousel): the
 * same drag and coast, returned as rows to step -- positive = towards the
 * end of the list (the finger went up). */
static int tch_steps_axis(tch_scroll_t *s, int row_h, int vx, int vy, int vw,
                          int vh, int horizontal)
{
   int n = 0;
   if (g_tch.down && !g_tch.was_down && s->on && abs(s->v256) >= 256 &&
       tch_in(vx, vy, vw, vh))
   {
      g_tch.moved = 1;             /* stops the coast; not a tap */
      g_tch.claimed = 1;
      s->v256 = 0;
      if (horizontal)
      {
         s->acc = 0;
         g_tch.axis = 1;
      }
   }
   else if (g_tch.down && !g_tch.dead && g_tch.moved &&
            (!horizontal || g_tch.axis == 1) && tch_in(vx, vy, vw, vh))
   {
      g_tch.claimed = 1;
      s->acc -= horizontal ? g_tch.dx : g_tch.dy;
      s->v256 = horizontal ? g_tch.vx256 : g_tch.v256;
      s->on = 1;
   }
   else if (s->on && !g_tch.down && abs(s->v256) >= 256)
   {
      s->acc -= s->v256 / 256;
      s->v256 = s->v256 * 12 / 16;     /* a short coast: three or four rows
                                        * past the finger, not the library */
   }
   else if (!g_tch.down)
   {
      s->v256 = 0;
      s->acc = 0;
      s->on = 0;
   }
   while (s->acc >= row_h) { s->acc -= row_h; n++; }
   while (s->acc <= -row_h) { s->acc += row_h; n--; }
   return n;
}

static int tch_steps(tch_scroll_t *s, int row_h, int vx, int vy, int vw,
                     int vh)
{
   return tch_steps_axis(s, row_h, vx, vy, vw, vh, 0);
}

/* Stop a coast (the pad took over). */
static void tch_scroll_stop(tch_scroll_t *s)
{
   s->v256 = 0;
   s->acc = 0;
   s->on = 0;
}

#endif /* UI_VITA_TOUCH_H */
