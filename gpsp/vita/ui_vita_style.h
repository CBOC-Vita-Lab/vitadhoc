/* ui_vita_style.h -- VitAdhoc's look, shared by the home screen
 * (ui_vita_home.h), the in-game control centre (ui_vita_menu.h) and the
 * restyled page chrome in psp/ui_psp.c.
 *
 * Included by psp/ui_psp.c under -DVITA right after ui_vita_touch.h.  It
 * draws with vstyle.h (native pixels); touch regions are registered in the
 * UI's logical pixels, half the native ones.
 *
 * THE PALETTE.  A deep navy page (never pure black: the game's own blacks
 * have to stand out of it), white type in three strengths, translucent
 * "glass" for anything raised, and one colour per console -- the GBA's
 * indigo, the DMG's pea green, the GBC's teal -- spent on glows, on the card
 * of a game with no picture and on a starred game's star.  The focused
 * thing wears a white ring; a primary button is solid white with dark ink.
 */
#ifndef UI_VITA_STYLE_H
#define UI_VITA_STYLE_H

#include "vstyle.h"

#define HC_BG       VS_HEX(0x060914, 255)
#define HC_TEXT     VS_HEX(0xF5F7FC, 255)
#define HC_TEXT2    VS_HEX(0xA7B0C6, 255)
#define HC_TEXT3    VS_HEX(0x6E7891, 255)
#define HC_GLASS    VS_HEX(0xFFFFFF, 30)
#define HC_GLASS_HI VS_HEX(0xFFFFFF, 52)
#define HC_INK      VS_HEX(0x0A0F1F, 255)
#define HC_SHEET    VS_HEX(0x0D1428, 236)
#define HC_RED      VS_HEX(0xFF6B6B, 255)

/* Touch regions are logical (480 x 272): native / 2. */
#define VS_TCH_BTN(b, x, y, w, h) \
   UI_TCH_BTN((b), (int)((x) / 2), (int)((y) / 2), (int)((w) / 2) + 1, \
              (int)((h) / 2) + 1)
#define VS_TCH_ROW(id, x, y, w, h) \
   UI_TCH_ROW((id), (int)((x) / 2), (int)((y) / 2), (int)((w) / 2) + 1, \
              (int)((h) / 2) + 1)

/* One colour per console (GBA, GB, GBC). */
static uint32_t hm_tint(int console, int a)
{
   static const uint32_t rgb[3] = { 0x7B6CFF, 0x8CC63F, 0x19C3B4 };
   if (console < 0 || console > 2)
      console = 0;
   return VS_HEX(rgb[console], a);
}

/* The console's card: a top and a bottom colour. */
static uint32_t hm_deep(int console, int bottom)
{
   static const uint32_t top[3] = { 0x5B4BC4, 0x3D7A35, 0x1C9E97 };
   static const uint32_t bot[3] = { 0x231B5E, 0x12301A, 0x0A3F45 };
   if (console < 0 || console > 2)
      console = 0;
   return VS_HEX(bottom ? bot[console] : top[console], 255);
}

static int hm_ease(float *v, float to, float k)
{
   float d = to - *v;
   if (d > -0.4f && d < 0.4f)
   {
      *v = to;
      return 0;
   }
   *v += d * k;
   return 1;
}

/* The battery: an outline, its charge, a bolt while charging. */
static void hm_battery(float x, float y)
{
   int pct = pspcompat_battery_percent();
   uint32_t c = HC_TEXT;
   vs_rrect_line(x, y, 36, 19, 5, 2, VS_ALPHA(c, 220));
   vs_rrect(x + 37, y + 6, 3, 7, 1, VS_ALPHA(c, 220));
   if (pct >= 0)
   {
      float fw = 28.0f * (float)(pct > 100 ? 100 : pct) / 100.0f;
      vs_rrect(x + 4, y + 4, fw < 3 ? 3 : fw, 11, 2,
               pct <= 15 ? HC_RED : VS_ALPHA(c, 235));
   }
   if (pspcompat_battery_charging())
      vs_icon(VS_I_FAST, x - 16, y + 9.5f, 18, VS_ALPHA(c, 200));
}

/* The time and the battery, ending at xr; returns where they begin. */
static float hm_status(float xr, float y)
{
   char clock[8];
   int hh = 0, mm = 0;
   float x = xr - 40;
   pspcompat_clock(&hh, &mm);
   snprintf(clock, sizeof(clock), "%d:%02d", hh, mm);
   hm_battery(x, y + 10);
   x -= 16 + vs_text_w(VS_LABEL, clock);
   vs_text(VS_LABEL, x, y + 6, clock, HC_TEXT);
   return x;
}

/* A chip: a little glass pill with a word in it.  Returns the next x. */
static float hm_chip(float x, float y, const char *s, uint32_t fill,
                     uint32_t ink)
{
   float w = vs_text_w(VS_CAPTION, s) + 26;
   vs_rrect(x, y, w, 34, 17, fill);
   vs_text(VS_CAPTION, x + 13, y + 5, s, ink);
   return x + w + 8;
}

/* A pill button: icon and label, or a circle with only an icon; `primary`
 * is solid white.  A tap is `btn`.  Returns the next x. */
static float hm_button(float x, float y, int icon, const char *label,
                       int primary, unsigned btn)
{
   float w = label ? vs_text_w(VS_LABEL, label) + (icon >= 0 ? 76 : 48) : 56;
   uint32_t ink = primary ? HC_INK : HC_TEXT;
   if (primary)
   {
      vs_shadow(x, y, w, 56, 28, 14, VS_HEX(0xFFFFFF, 46));
      vs_rrect(x, y, w, 56, 28, HC_TEXT);
   }
   else
      vs_rrect(x, y, w, 56, 28, HC_GLASS_HI);
   if (label)
   {
      float lx = x + (icon >= 0 ? 52 : 24);
      if (icon >= 0)
         vs_icon(icon, x + 30, y + 28, 26, ink);
      vs_text(VS_LABEL, lx, y + 15, label, ink);
   }
   else
      vs_icon(icon, x + 28, y + 28, 28, ink);
   VS_TCH_BTN(btn, x, y, w, 56);
   return x + w + 14;
}

/* A line of the pad's hints, right-aligned at xr: an icon (VS_I_BTN_*, or
 * -1 for a word alone) and a word each.  Each one is also a button for the
 * finger when `btns` is given. */
static void hm_hints_b(float xr, float y, const int *icons,
                       const char *const *words, const unsigned *btns, int n)
{
   int i;
   float x = xr;
   for (i = n - 1; i >= 0; i--)
   {
      float w = vs_text_w(VS_CAPTION, words[i]), x1 = x;
      x -= w;
      vs_text(VS_CAPTION, x, y, words[i], HC_TEXT3);
      if (icons[i] >= 0)
      {
         vs_icon(icons[i], x - 14, y + 12, 20, HC_TEXT3);
         x -= 28;
      }
      if (btns && btns[i])
         VS_TCH_BTN(btns[i], x - 8, y - 12, x1 - x + 16, 48);
      x -= 22;
   }
}

static void hm_hints(float xr, float y, const int *icons,
                     const char *const *words, int n)
{
   hm_hints_b(xr, y, icons, words, NULL, n);
}

/* "SAVE STATES" -> "Save states": the old pages' titles, in the new
 * sentence case. */
static void hm_sentence(char *out, size_t cap, const char *s)
{
   size_t i;
   int first = 1;
   for (i = 0; s[i] && i + 1 < cap; i++)
   {
      char c = s[i];
      if (c >= 'A' && c <= 'Z' && !first)
         c = (char)(c - 'A' + 'a');
      if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))
         first = 0;
      out[i] = c;
   }
   out[i] = '\0';
}

#endif /* UI_VITA_STYLE_H */
