/* ui_vita_widgets.h -- the few pieces the Vita's own pages share.
 *
 * Included by psp/ui_psp.c under -DVITA after the overlay's chrome (it uses
 * ov_band, ov_outline, ov_text_right, clip_title and the C_* palette), and
 * before the pages that use it: ui_vita_play.h, ui_vita_cheats.h and the
 * Settings rows the Vita adds.
 *
 *   ui_text_*   one text field through the system keyboard (names)
 *   vw_row      a list row in the overlay's language, touchable
 *   vw_switch   an on/off pill;  vw_signal  four bars
 *   kp_*        an on-screen keypad (the address page, the cheat-code page):
 *               d-pad + X, and every key a 9 mm touch target
 */
#ifndef UI_VITA_WIDGETS_H
#define UI_VITA_WIDGETS_H

/* ---- text entry ---------------------------------------------------------
 * Names only (the player's, a cheat's, the cheat list's filter): everything
 * else that is typed has a keypad of its own, because a code or an address
 * needs sixteen keys, not a hundred.
 *
 * ui_text_open() asks the system for its keyboard and returns at once.
 * While it is up ui_typing() is 1: ui_frame keeps drawing the page (the
 * dialog composites itself over the presented frame), and feeds it no pad
 * and no touch -- both belong to the keyboard.  The frame it closes,
 * ui_text_take(field, &text) answers 1 = accepted (text points at the
 * result, valid for that frame) or -1 = cancelled, once. */
enum { TXT_NONE = 0, TXT_NICK, TXT_CHEAT_NAME, TXT_CHEAT_FIND,
       TXT_BROWSER_FIND, TXT_RA_USER, TXT_RA_PASS };

static struct
{
   int  field;                /* the field the keyboard is up for          */
   int  done_field;           /* the field that finished THIS frame        */
   int  done_ok;
   int  secret;               /* the open field is a password          */
   /* UTF-8: a password comes back as typed, three bytes a character at
    * most; the other fields are printable ASCII */
   char text[PC_IME_MAX_CHARS * 3 + 4];
} g_txt;

int ui_typing(void)
{
   return g_txt.field != TXT_NONE;
}

static int ui_text_open(int field, const char *title, const char *initial,
                        int max_chars)
{
   if (g_txt.field != TXT_NONE ||
       pspcompat_ime_open(title, initial, max_chars) != 0)
   {
      fe_evt("ui_text_open field=%d REFUSED", field);
      return -1;
   }
   g_txt.field = field;
   vid_set_common_dialog(1);
   fe_evt("ui_text_open field=%d max=%d", field, max_chars);
   return 0;
}

/* A password: dots on screen, nothing typed kept by the keyboard's side.
 * The taker wipes g_txt.text once it has used it (ui_text_wipe). */
static int ui_text_open_secret(int field, const char *title, int max_chars)
{
   if (g_txt.field != TXT_NONE ||
       pspcompat_ime_open_secret(title, max_chars) != 0)
   {
      fe_evt("ui_text_open field=%d REFUSED", field);
      return -1;
   }
   g_txt.field = field;
   g_txt.secret = 1;
   vid_set_common_dialog(1);
   fe_evt("ui_text_open field=%d max=%d secret=1", field, max_chars);
   return 0;
}

static void ui_text_wipe(void)
{
   volatile char *v = g_txt.text;
   size_t i;
   for (i = 0; i < sizeof(g_txt.text); i++)
      v[i] = 0;
}

/* Once per UI frame.  Returns 1 while the keyboard owns the input. */
static int ui_text_pump(void)
{
   int st;
   g_txt.done_field = TXT_NONE;
   if (g_txt.field == TXT_NONE)
      return 0;
   st = pspcompat_ime_poll(g_txt.text, sizeof(g_txt.text));
   if (st == PC_IME_RUNNING)
      return 1;
   vid_set_common_dialog(0);
   g_txt.done_field = g_txt.field;
   g_txt.done_ok = st == PC_IME_DONE;
   g_txt.field = TXT_NONE;
   /* (a password's length is not the log's business) */
   fe_evt("ui_text_done field=%d ok=%d len=%d", g_txt.done_field,
          g_txt.done_ok, g_txt.secret ? -1
                         : g_txt.done_ok ? (int)strlen(g_txt.text) : 0);
   g_txt.secret = 0;
   /* The frame the keyboard closes still belongs to it: the finger or the
    * button that dismissed it must not reach the page. */
   return 1;
}

static int ui_text_take(int field, const char **text)
{
   if (g_txt.done_field != field)
      return 0;
   g_txt.done_field = TXT_NONE;
   if (text)
      *text = g_txt.text;
   return g_txt.done_ok ? 1 : -1;
}

/* ---- rows --------------------------------------------------------------- */

#define VW_PITCH   26         /* the Vita pages' row pitch: 6 mm           */
#define VW_BAND_H  24

/* `s` clipped to `px` with the UI's "..." (clip_title), into buf. */
static const char *vw_clip(char *buf, size_t cap, const char *s, int px)
{
   snprintf(buf, cap, "%s", s ? s : "");
   clip_title(buf, px);
   return buf;
}

/* One line that must fit `w` px whatever a hook or a file name put in it. */
static void vw_line(int x, int y, int w, const char *s, uint16_t col)
{
   char b[200];
   vid_text(x, y, vw_clip(b, sizeof(b), s, w), col);
}

/* ov_plate for text this file did not write (a hook's reason, a name). */
static void vw_plate(const char *msg)
{
   char b[200];
   ov_plate(vw_clip(b, sizeof(b), msg, 464));
}

/* The overlay header without the room: the Play together pages say where
 * the room matters, and their titles need the width. */
static void vw_head(const char *sub, const char *title)
{
   char b[200];
   g_ov_room_txt = "";
   ov_head(sub ? vw_clip(b, sizeof(b), sub, 436) : NULL, title);
   g_ov_room_txt = NULL;
}

/* One row: label at 18, value right-aligned at the band's end, the cursor
 * row on the left-bleed band.  `id` is the cursor value; a row is touchable
 * across its whole band and from the middle of the gap above it to the
 * middle of the gap below. */
static void vw_row_h(int id, int y, int w, int pitch, int sel, int enabled,
                     const char *label, const char *value)
{
   char v[80];
   int vx_min = 18 + vid_text_w(label) + 14;
   if (sel)
      ov_band(y, w, VW_BAND_H);
   vid_text(18, y, label, enabled ? (sel ? C_SEL : C_ITEM) : C_DIM);
   if (value && value[0])
      ov_text_right(w - 8, y, vw_clip(v, sizeof(v), value, w - 8 - vx_min),
                    enabled ? (sel ? C_VALUE : C_ACCENT_DK) : C_DIM);
   tch_row(id, 0, vid_band_y(y, VW_BAND_H) - (pitch - VW_BAND_H) / 2, w,
           pitch);
}

static void vw_row(int id, int y, int w, int sel, int enabled,
                   const char *label, const char *value)
{
   vw_row_h(id, y, w, VW_PITCH, sel, enabled, label, value);
}

/* An on/off pill, 26 x 12, its top at `y` (a row's text y + 3 centres it on
 * the cap height). */
static void vw_switch(int x, int y, int on, int enabled)
{
   uint16_t c = enabled ? C_ACCENT : C_DIM;
   if (on)
   {
      vid_rect(x, y, 26, 12, enabled ? C_CON : C_DIM, 255);
      vid_rect(x + 15, y + 2, 9, 8, C_BG_BOT, 255);
   }
   else
   {
      ov_outline(x, y, 26, 12, c, enabled ? 150 : 110);
      vid_rect(x + 2, y + 2, 9, 8, c, enabled ? 150 : 110);
   }
}

/* Signal strength as four bars rising to the right, 18 x 12, bottom-aligned
 * on y + 12.  pct < 0 = unknown: nothing is drawn. */
static void vw_signal(int x, int y, int pct)
{
   int i, lit;
   if (pct < 0)
      return;
   lit = pct >= 75 ? 4 : pct >= 50 ? 3 : pct >= 25 ? 2 : pct > 0 ? 1 : 0;
   for (i = 0; i < 4; i++)
   {
      int h = 3 + i * 3;
      vid_rect(x + i * 5, y + 12 - h, 3, h, i < lit ? C_VALUE : C_DIM,
               i < lit ? 255 : 110);
   }
}

/* The busy ring anywhere (ov_ring hangs it left of the console badge). */
static void vw_ring(int cx, int cy, uint16_t col)
{
   static const signed char ring[8][2] = {
      {0,-7}, {5,-5}, {7,0}, {5,5}, {0,7}, {-5,5}, {-7,0}, {-5,-5}
   };
   unsigned phase = ((unsigned)sceKernelGetSystemTimeLow() / 100000u) % 8u;
   int i;
   for (i = 0; i < 8; i++)
      vid_rect(cx + ring[i][0] - 1, cy + ring[i][1] - 1, 3, 3, col,
               50 + 25 * (int)(((unsigned)i + 8u - phase) % 8u));
}

/* A paragraph: `text` wrapped to `w` px, at most `max_lines` lines of
 * FE_FONT_H + 2.  The first line starts `lead` px in (a term was drawn
 * there), the others `hang` px in (0 = under the term, lead = beside it).
 * Returns the y after the last line drawn. */
static int vw_wrap_lead(int x, int y, int w, const char *text, uint16_t col,
                        int max_lines, int lead, int hang)
{
   char line[128];
   const char *p = text;
   int lines = 0;
   while (p && *p && lines < max_lines)
   {
      int n = 0, last_sp = -1, i, ind = lines ? hang : lead;
      while (*p == ' ')
         p++;
      for (i = 0; p[i] && p[i] != '\n' && i < (int)sizeof(line) - 1; i++)
      {
         line[i] = p[i];
         line[i + 1] = '\0';
         if (vid_text_w(line) > w - ind)
            break;
         if (p[i] == ' ')
            last_sp = i;
         n = i + 1;
      }
      if (p[n] && p[n] != '\n' && last_sp > 0)
         n = last_sp;                    /* break at the last blank */
      memcpy(line, p, (size_t)n);
      line[n] = '\0';
      vid_text(x + ind, y, line, col);
      y += FE_FONT_H + 2;
      lines++;
      p += n;
      if (*p == '\n')
         p++;
   }
   return y;
}

static int vw_wrap(int x, int y, int w, const char *text, uint16_t col,
                   int max_lines)
{
   return vw_wrap_lead(x, y, w, text, col, max_lines, 0, 0);
}

/* ---- the keypad ---------------------------------------------------------
 * Keys on a grid of KP_CW x KP_CH cells (42 x 36 logical = 9.7 x 8.3 mm, a
 * key itself 2 px smaller each way), each with a label and what it does.
 * The page owns the text; the keypad only reports. */
#define KP_CW 42
#define KP_CH 36

enum { KP_CHAR = 0,       /* types .ch                                     */
       KP_DEL,            /* backspace                                     */
       KP_OK,             /* the page's confirm                            */
       KP_AUX };          /* page-specific; .ch tells them apart           */

typedef struct
{
   const char *label;
   char ch;
   unsigned char act;
   unsigned char col, row, span;   /* span = cells wide                    */
} kp_key;

typedef struct
{
   const kp_key *keys;
   int n;
   int x, y;                  /* the grid's top-left                       */
} kp_pad;

#define KP_ROW_BASE 1000      /* tch_row ids of the keys: KP_ROW_BASE + i  */

/* Leaving the grid by an edge. */
enum { KP_EDGE_L = -1, KP_EDGE_R = -2, KP_EDGE_U = -3, KP_EDGE_D = -4 };

/* The key reached from `cur` by one d-pad step, or KP_EDGE_*.  Left/right
 * stay in the row; up/down go to the key above/below the one's middle. */
static int kp_step(const kp_pad *p, int cur, unsigned edge)
{
   const kp_key *k = &p->keys[cur];
   int i, best = -1, best_d = 1 << 20;
   int mid2 = 2 * k->col + k->span;          /* twice the middle column    */
   for (i = 0; i < p->n; i++)
   {
      const kp_key *o = &p->keys[i];
      int d = -1;
      if (i == cur)
         continue;
      if (edge == PSP_CTRL_LEFT && o->row == k->row && o->col < k->col)
         d = k->col - o->col;
      else if (edge == PSP_CTRL_RIGHT && o->row == k->row && o->col > k->col)
         d = o->col - k->col;
      else if (edge == PSP_CTRL_UP && o->row < k->row)
         d = (k->row - o->row) * 64 +
             (2 * o->col <= mid2 && mid2 <= 2 * (o->col + o->span) ? 0
              : abs(2 * o->col + o->span - mid2));
      else if (edge == PSP_CTRL_DOWN && o->row > k->row)
         d = (o->row - k->row) * 64 +
             (2 * o->col <= mid2 && mid2 <= 2 * (o->col + o->span) ? 0
              : abs(2 * o->col + o->span - mid2));
      if (d >= 0 && d < best_d)
      {
         best_d = d;
         best = i;
      }
   }
   if (best >= 0)
      return best;
   return edge == PSP_CTRL_LEFT ? KP_EDGE_L : edge == PSP_CTRL_RIGHT ? KP_EDGE_R
        : edge == PSP_CTRL_UP ? KP_EDGE_U : KP_EDGE_D;
}

/* Draw the keys; `cur` is the key under the cursor (-1: the focus is
 * elsewhere on the page).  `off` is a bit mask of keys drawn greyed (bit i =
 * key i), still touchable so the page can say why. */
static void kp_draw(const kp_pad *p, int cur, unsigned off)
{
   int i;
   for (i = 0; i < p->n; i++)
   {
      const kp_key *k = &p->keys[i];
      int x = p->x + k->col * KP_CW, y = p->y + k->row * KP_CH;
      int w = k->span * KP_CW - 2, h = KP_CH - 2;
      int grey = (off >> i) & 1, sel = i == cur;
      int strong = k->act == KP_OK && !grey;
      int tw = vid_text_w(k->label);
      vid_rect(x, y, w, h, strong ? C_CON : C_CARD,
               strong ? (g_pcfg.theme == 1 ? 60 : 90) : 235);
      if (sel)
      {
         vid_rect(x, y, w, h, C_ACCENT, 40);
         ov_outline(x, y, w, h, C_ACCENT, 255);
         ov_outline(x + 1, y + 1, w - 2, h - 2, C_ACCENT, 255);
      }
      else
         ov_outline(x, y, w, h, C_ACCENT_DK, 70);
      vid_text(x + (w - tw) / 2, vid_text_y_in(y, h), k->label,
               grey ? C_DIM : sel ? C_SEL : k->act == KP_CHAR ? C_VALUE
                                                              : C_ITEM);
      tch_row(KP_ROW_BASE + i, x - 1, y - 1, w + 2, h + 2);
   }
}

/* A text box: C_CARD plate with an outline, `h` tall.  The caller draws the
 * text inside. */
static void vw_box(int x, int y, int w, int h, int focus)
{
   vid_rect(x, y, w, h, C_CARD, 235);
   ov_outline(x, y, w, h, focus ? C_ACCENT : C_ACCENT_DK, focus ? 255 : 120);
}

/* The typing caret, blinking at 2 Hz. */
static void vw_caret(int x, int y, int h)
{
   if (((unsigned)sceKernelGetSystemTimeLow() / 250000u) & 1u)
      return;
   vid_rect(x, y, 1, h, C_ACCENT, 255);
}

#endif /* UI_VITA_WIDGETS_H */
