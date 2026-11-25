/* Copyright (c) 2026 CBOC-Vita contributors.
 * SPDX-License-Identifier: GPL-2.0-or-later
 * See gbcore_mux.h. */
#include "gbcore_mux.h"
#include "nescore.h"
#include "../wscore/wscore.h"
#include "../gbcore/gambatte_host.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../frontend-common/fe_evt.h"

/* The two-player link's calls (gbmux_pad_*) are the only ones that reach the
 * nescore / wscore functions below, and a host test of fe_host.c that links
 * no NES or WonderSwan machine (tools/tests/fe_host_absent.c) has none of
 * them: weak, so such a link still resolves, and never called without a
 * machine (gbmux_pad_ok is checked first). */
#pragma weak nescore_run_frame2
#pragma weak nescore_session_pin
#pragma weak nescore_save_ram_crc
#pragma weak wscore_pad_now
#pragma weak wscore_session_due
#pragma weak wscore_session_frame
#pragma weak wscore_session_present
#pragma weak wscore_session_clock
#pragma weak wscore_save_ram_crc
#pragma weak nescore_rom_crc
#pragma weak wscore_rom_crc

static nescore_t *g_nes;                 /* the running NES machine, if any */
static gbcore_callbacks_t g_gbcb;        /* the frontend's callbacks for it */

static wscore_t *g_ws;                   /* the running WonderSwan machine, if any */

/* The Accurate Game Boy core (Gambatte).  Its machine is one at a time; the pointer the frontend holds for it is
 * this token. */
static char g_gam_token;
static int g_gam;                        /* a Gambatte machine runs */
static int g_gam_choice;                 /* Settings: 1 = the next Game Boy game runs on Gambatte */
static int g_gam_fallback;               /* the last game asked for Accurate and the cartridge needs Standard */
static int g_state_other;                /* the last state load met the other Game Boy core's image */
static unsigned g_gb_palette;            /* gbcore_set_palette's last id (for the Gambatte machine) */

#define IS_NES(c) (g_nes && ((const void *)(c) == (const void *)g_nes || !(c)))
#define IS_WS(c) (g_ws && ((const void *)(c) == (const void *)g_ws || !(c)))
#define IS_GAM(c) (g_gam && ((const void *)(c) == (const void *)&g_gam_token || !(c)))
#define IS_ANY(c) (IS_NES(c) || IS_WS(c) || IS_GAM(c))

int gbmux_is_ws(const gbcore_t *core) { return g_ws && (const void *)core == (const void *)g_ws; }
int gbmux_other_console(const gbcore_t *core)
{ return gbmux_is_nes(core) ? FE_CONSOLE_NES : gbmux_is_ws(core) ? FE_CONSOLE_WSWAN : -1; }
static int g_last_ws_create;
const char *gbmux_error(void) { return g_last_ws_create ? wscore_error() : nescore_error(); }
int gbmux_cheats_text(const char *codes)
{
  if (g_nes) return nescore_cheats_set(g_nes, codes);
  if (g_ws) return wscore_cheats_set(g_ws, codes);
  return 0;
}
int gbmux_save_dirty(const gbcore_t *core)
{
  if (gbmux_is_nes(core)) return nescore_save_ram_dirty(g_nes);
  if (gbmux_is_ws(core)) return wscore_save_ram_dirty(g_ws);
  return 0;
}

int gbmux_is_nes(const gbcore_t *core) { return g_nes && (const void *)core == (const void *)g_nes; }
const char *gbmux_nes_error(void) { return nescore_error(); }
void gbmux_nes_system_dir(const char *dir) { nescore_set_system_dir(dir); }
double gbmux_fps(const gbcore_t *core)
{ return gbmux_is_nes(core) ? nescore_fps(g_nes) : gbmux_is_ws(core) ? wscore_fps(g_ws) : 59.7275; }
double gbmux_nes_fps(void) { return g_nes ? nescore_fps(g_nes) : 0.0; }
int gbmux_nes_cheats(const char *codes) { return g_nes ? nescore_cheats_set(g_nes, codes) : 0; }
int gbmux_nes_save_dirty(const gbcore_t *core) { return gbmux_is_nes(core) ? nescore_save_ram_dirty(g_nes) : 0; }

static void nes_video(void *u, const nescore_video_frame_t *f)
{
  gbcore_video_frame_t g;
  (void)u;
  g.pixels = f->pixels; g.width = f->width; g.height = f->height;
  g.pitch_bytes = f->pitch_bytes;
  if (g_gbcb.video) g_gbcb.video(g_gbcb.userdata, &g);
}

static void nes_audio(void *u, const int16_t *s, size_t n)
{
  (void)u;
  if (g_gbcb.audio_batch) g_gbcb.audio_batch(g_gbcb.userdata, s, n);
}

static void ws_video(void *u, const wscore_video_frame_t *f)
{
  gbcore_video_frame_t g;
  (void)u;
  g.pixels = f->pixels; g.width = f->width; g.height = f->height;
  g.pitch_bytes = f->pitch_bytes;
  if (g_gbcb.video) g_gbcb.video(g_gbcb.userdata, &g);
}

/* ---- the Accurate Game Boy core (gbcore/gambatte_host.h) ---- */
static void gam_video(void *u, const uint16_t *p, unsigned w, unsigned h, unsigned pitch)
{
  gbcore_video_frame_t g;
  (void)u;
  g.pixels = p; g.width = w; g.height = h; g.pitch_bytes = pitch;
  if (g_gbcb.video) g_gbcb.video(g_gbcb.userdata, &g);
}

static int gam_serial(void *u, uint8_t out, int internal, uint8_t *in)
{
  (void)u;
  return g_gbcb.serial_transfer ? g_gbcb.serial_transfer(g_gbcb.userdata, out, internal, in) : 0;
}

/* the cartridge file, as gbcore_create reads one */
static uint8_t *gam_read_rom(const char *path, size_t *n)
{
  FILE *f = fopen(path, "rb");
  uint8_t *buf = NULL;
  long len;
  *n = 0;
  if (!f)
    return NULL;
  if (fseek(f, 0, SEEK_END) == 0 && (len = ftell(f)) >= 0x150 && len <= 8 * 1024 * 1024 && fseek(f, 0, SEEK_SET) == 0 &&
      (buf = (uint8_t *)malloc((size_t)len)) != NULL)
  {
    if (fread(buf, 1, (size_t)len, f) == (size_t)len)
      *n = (size_t)len;
    else
    {
      free(buf);
      buf = NULL;
    }
  }
  fclose(f);
  return buf;
}

/* The DMG palette as the Gambatte machine draws it: the same twelve colours the other core shows for the palette
 * (gbcore_palette_preview_for), the game's own header deciding GBC auto.  Super Game Boy colours are the other core's
 * (a game that is run for them goes there: gbmux_create). */
static void gam_palette_apply(void)
{
  uint16_t pv[12];
  unsigned id = g_gb_palette < GBCORE_PALETTE_COUNT ? g_gb_palette : GBCORE_PALETTE_AUTO;
  if (id == GBCORE_PALETTE_AUTO)
    id = GBCORE_PALETTE_GREY;
  (void)gbcore_palette_preview_for(id, gambatte_host_header(), pv);
  gambatte_host_set_dmg_colors(pv);
}

void gbmux_set_gb_accurate(int on) { g_gam_choice = on != 0; }
int gbmux_gb_accurate_chosen(void) { return g_gam_choice; }
int gbmux_is_gambatte(const gbcore_t *core) { return g_gam && (const void *)core == (const void *)&g_gam_token; }
int gbmux_gambatte_running(void) { return g_gam; }
int gbmux_gb_fallback_take(void) { int r = g_gam_fallback; g_gam_fallback = 0; return r; }
int gbmux_state_other_take(void) { int r = g_state_other; g_state_other = 0; return r; }

void gbmux_set_palette(unsigned palette)
{
  g_gb_palette = palette;
  gbcore_set_palette(palette);
  if (g_gam)
    gam_palette_apply();
}

int gbmux_palette_preview(unsigned palette, uint16_t rgb565[12])
{
  if (!g_gam)
    return gbcore_palette_preview(palette, rgb565);
  if (palette >= GBCORE_PALETTE_COUNT)
    palette = GBCORE_PALETTE_AUTO;
  return gbcore_palette_preview_for(palette == GBCORE_PALETTE_AUTO ? GBCORE_PALETTE_GREY : palette,
                                    gambatte_host_header(), rgb565);
}

void gbmux_set_color_correction(int on)
{
  gbcore_set_color_correction(on);
  gambatte_host_set_color_correction(on);
  if (g_gam)
    gam_palette_apply();
}

void gbmux_set_wallclock(time_t (*wallclock)(void))
{
  gbcore_set_wallclock(wallclock);
  gambatte_host_set_wallclock(wallclock);
}

gbcore_t *gbmux_create(const char *rom_path, const void *rom_data, size_t rom_size,
                       fe_console_t console, unsigned audio_rate,
                       const gbcore_callbacks_t *cb)
{
  if (console == FE_CONSOLE_NES)
  {
    nescore_callbacks_t nc;
    if (!rom_path || rom_data || g_nes) return NULL;
    memset(&nc, 0, sizeof(nc));
    nc.video = nes_video;
    nc.audio_batch = nes_audio;
    if (cb) g_gbcb = *cb; else memset(&g_gbcb, 0, sizeof(g_gbcb));
    g_last_ws_create = 0;
    g_nes = nescore_create(rom_path, NULL, &nc);
    return (gbcore_t *)g_nes;
  }
  if (console == FE_CONSOLE_WSWAN)
  {
    wscore_callbacks_t wc;
    if (!rom_path || rom_data || g_ws) return NULL;
    memset(&wc, 0, sizeof(wc));
    wc.video = ws_video;
    wc.audio_batch = nes_audio;
    if (cb) g_gbcb = *cb; else memset(&g_gbcb, 0, sizeof(g_gbcb));
    g_last_ws_create = 1;
    g_ws = wscore_create(rom_path, audio_rate, &wc);
    return (gbcore_t *)g_ws;
  }
  g_gam_fallback = 0;
  if ((console == FE_CONSOLE_GB || console == FE_CONSOLE_GBC) && g_gam_choice && !g_gam)
  {
    const uint8_t *rom = (const uint8_t *)rom_data;
    uint8_t *own = NULL;
    size_t n = rom_size;
    if (!rom && rom_path)
      rom = own = gam_read_rom(rom_path, &n);
    /* a Super Game Boy game under the Super Game Boy palette is run for its colours and border: the other core's */
    if (rom && n >= 0x150 && !(g_gb_palette == GBCORE_PALETTE_AUTO && console == FE_CONSOLE_GB && rom[0x146] == 3))
    {
      gambatte_host_cb_t gc;
      memset(&gc, 0, sizeof(gc));
      gc.video = gam_video;
      gc.audio_batch = nes_audio;
      gc.serial_transfer = cb && cb->serial_transfer ? gam_serial : NULL;
      if (cb) g_gbcb = *cb; else memset(&g_gbcb, 0, sizeof(g_gbcb));
      if (gambatte_host_create(rom, n, console == FE_CONSOLE_GBC, audio_rate, &gc) == 0)
      {
        free(own);
        g_gam = 1;
        gam_palette_apply();
        return (gbcore_t *)&g_gam_token;
      }
    }
    free(own);
    g_gam_fallback = 1;
  }
  return gbcore_create(rom_path, rom_data, rom_size, console, audio_rate, cb);
}

/* gbcore button bits -> the NES pad's */
static unsigned nes_pad(uint16_t b)
{
  unsigned n = 0;
  if (b & GBCORE_BUTTON_A) n |= NESCORE_BUTTON_A;
  if (b & GBCORE_BUTTON_B) n |= NESCORE_BUTTON_B;
  if (b & GBCORE_BUTTON_SELECT) n |= NESCORE_BUTTON_SELECT;
  if (b & GBCORE_BUTTON_START) n |= NESCORE_BUTTON_START;
  if (b & GBCORE_BUTTON_UP) n |= NESCORE_BUTTON_UP;
  if (b & GBCORE_BUTTON_DOWN) n |= NESCORE_BUTTON_DOWN;
  if (b & GBCORE_BUTTON_LEFT) n |= NESCORE_BUTTON_LEFT;
  if (b & GBCORE_BUTTON_RIGHT) n |= NESCORE_BUTTON_RIGHT;
  return n;
}

/* The NES at a rate the display does not have (PAL and Dendy games, 50.007
 * frames a second): one core call per refresh runs the frames the refresh
 * covers, 0 or 1 (5 of 6 on a 59.94 Hz display), an exact accumulator of
 * frame time against refresh time.  An NTSC game (60.0988) runs one per call
 * as before.  The picture of a call that runs none is the last one again. */
static double g_disp_hz = 60000.0 / 1001.0, g_nes_acc;
static int g_pal_smooth, g_pal_borrowed;
void gbmux_set_display_rate(double hz) { if (hz > 1.0) g_disp_hz = hz; g_nes_acc = 0; g_pal_borrowed = 0; }
void gbmux_set_pal_smooth(int on) { g_pal_smooth = on != 0; g_pal_borrowed = 0; }

static int nes_pal_frame(gbcore_t *c, uint16_t b)
{
  double fps = nescore_fps(g_nes);
  (void)c;
  if (fps > 55.0 || fps < 1.0)
    return nescore_run_frame(g_nes, nes_pad(b));
  static unsigned calls, frames;
  int rc;
  g_nes_acc += fps / g_disp_hz;
  calls++;
  if (g_nes_acc >= 1.0)
  {
    g_nes_acc -= 1.0;
    g_pal_borrowed = 0;
    frames++;
    rc = nescore_run_frame(g_nes, nes_pad(b));
  }
  else if (g_pal_smooth && !g_pal_borrowed)
  {
    /* Smooth 50 Hz: the frame this refresh lacks runs now, and shows mixed with the one before; the next refresh shows it alone */
    g_nes_acc -= 1.0;
    g_pal_borrowed = 1;
    frames++;
    nescore_mix_next(g_nes);
    rc = nescore_run_frame(g_nes, nes_pad(b));
  }
  else
  {
    g_pal_borrowed = 0;
    rc = nescore_repeat_frame(g_nes);
  }
  if (calls % 600 == 0)
    fe_evt("nes_pace fps=%.3f display=%.3f frames=%u calls=%u", fps, g_disp_hz, frames, calls);
  return rc;
}

int gbmux_run_frame(gbcore_t *c, uint16_t b)
{ if (IS_GAM(c)) return gambatte_host_run_frame(b);
  if (IS_WS(c)) return wscore_run_frame(g_ws);   /* buttons: wscore_set_buttons */
  if (IS_NES(c)) return nes_pal_frame(c, b);
  return IS_NES(c) ? nescore_run_frame(g_nes, nes_pad(b)) : gbcore_run_frame(c, b); }
void gbmux_set_skip_render(gbcore_t *c, int s)
{ if (IS_GAM(c)) gambatte_host_set_skip_render(s); else if (IS_WS(c)) wscore_set_skip_render(g_ws, s); else if (IS_NES(c)) nescore_set_skip_render(g_nes, s); else gbcore_set_skip_render(c, s); }
void gbmux_shutdown(gbcore_t *c)
{
  if (IS_GAM(c)) { gambatte_host_shutdown(); g_gam = 0; }
  else if (IS_WS(c)) { wscore_shutdown(g_ws); g_ws = NULL; }
  else if (IS_NES(c)) { nescore_shutdown(g_nes); g_nes = NULL; }
  else gbcore_shutdown(c);
}
unsigned gbmux_sample_rate(const gbcore_t *c)
{ return gbmux_is_gambatte(c) ? gambatte_host_sample_rate() : gbmux_is_ws(c) ? wscore_sample_rate(g_ws) : gbmux_is_nes(c) ? nescore_sample_rate(g_nes) : gbcore_sample_rate(c); }
unsigned gbmux_frame_width(const gbcore_t *c)
{ return gbmux_is_gambatte(c) ? gambatte_host_frame_width() : gbmux_is_ws(c) ? wscore_frame_width(g_ws) : gbmux_is_nes(c) ? nescore_frame_width(g_nes) : gbcore_frame_width(c); }
unsigned gbmux_frame_height(const gbcore_t *c)
{ return gbmux_is_gambatte(c) ? gambatte_host_frame_height() : gbmux_is_ws(c) ? wscore_frame_height(g_ws) : gbmux_is_nes(c) ? nescore_frame_height(g_nes) : gbcore_frame_height(c); }
int gbmux_rom_title(gbcore_t *c, char *d, size_t n)
{ return IS_GAM(c) ? gambatte_host_rom_title(d, n) : IS_WS(c) ? wscore_rom_title(g_ws, d, n) : IS_NES(c) ? nescore_rom_title(g_nes, d, n) : gbcore_rom_title(c, d, n); }
size_t gbmux_save_ram_size(gbcore_t *c)
{ return IS_GAM(c) ? gambatte_host_save_ram_size() : IS_WS(c) ? wscore_save_ram_size(g_ws) : IS_NES(c) ? nescore_save_ram_size(g_nes) : gbcore_save_ram_size(c); }
size_t gbmux_cart_ram_size(gbcore_t *c)
{ return IS_GAM(c) ? gambatte_host_cart_ram_size() : IS_WS(c) ? wscore_save_ram_size(g_ws) : IS_NES(c) ? nescore_save_ram_size(g_nes) : gbcore_cart_ram_size(c); }
int gbmux_save_ram_read(gbcore_t *c, void *d, size_t n)
{ return IS_GAM(c) ? gambatte_host_save_ram_read(d, n) : IS_WS(c) ? wscore_save_ram_read(g_ws, d, n) : IS_NES(c) ? nescore_save_ram_read(g_nes, d, n) : gbcore_save_ram_read(c, d, n); }
int gbmux_save_ram_write(gbcore_t *c, const void *s, size_t n)
{ return IS_GAM(c) ? gambatte_host_save_ram_write(s, n) : IS_WS(c) ? wscore_save_ram_write(g_ws, s, n) : IS_NES(c) ? nescore_save_ram_write(g_nes, s, n) : gbcore_save_ram_write(c, s, n); }
size_t gbmux_state_size(gbcore_t *c)
{ return IS_GAM(c) ? gambatte_host_state_size() : IS_WS(c) ? wscore_state_size(g_ws) : IS_NES(c) ? nescore_state_size(g_nes) : gbcore_state_size(c); }
long gbmux_state_save(gbcore_t *c, void *d, size_t n)
{ return IS_GAM(c) ? gambatte_host_state_save(d, n) : IS_WS(c) ? wscore_state_save(g_ws, d, n) : IS_NES(c) ? nescore_state_save(g_nes, d, n) : gbcore_state_save(c, d, n); }
/* A state is one core's: the image says which ("GBGM" Gambatte, "GBAT" TGB Dual).  The other core's is refused, and the
 * frontend can say why (gbmux_state_other_take). */
int gbmux_state_load(gbcore_t *c, const void *s, size_t n)
{
  if (IS_GAM(c))
  {
    if (gambatte_host_state_kind(s, n) == 1) { g_state_other = 1; return -1; }
    return gambatte_host_state_load(s, n);
  }
  if (!IS_WS(c) && !IS_NES(c))
  {
    if (gambatte_host_state_kind(s, n) == 2) { g_state_other = 1; return -1; }
    return gbcore_state_load(c, s, n);
  }
  return IS_WS(c) ? wscore_state_load(g_ws, s, n) : nescore_state_load(g_nes, s, n);
}
size_t gbmux_ahead_size(gbcore_t *c)
{ /* (a PAL or Dendy NES game runs 0 or 1 frames a refresh, and a refresh that runs none shows the picture before: no run-ahead for it) */
  if (IS_NES(c)) { const double fps = nescore_fps(g_nes); return fps > 55.0 || fps < 1.0 ? nescore_ahead_size(g_nes) : 0; }
  return IS_GAM(c) ? gambatte_host_ahead_size() : IS_WS(c) ? wscore_ahead_size(g_ws) : gbcore_ahead_size(c); }
long gbmux_ahead_save(gbcore_t *c, void *d, size_t n)
{ return IS_GAM(c) ? gambatte_host_ahead_save(d, n) : IS_WS(c) ? wscore_ahead_save(g_ws, d, n) : IS_NES(c) ? nescore_ahead_save(g_nes, d, n) : gbcore_ahead_save(c, d, n); }
int gbmux_ahead_load(gbcore_t *c, const void *s, size_t n)
{ return IS_GAM(c) ? gambatte_host_ahead_load(s, n) : IS_WS(c) ? wscore_ahead_load(g_ws, s, n) : IS_NES(c) ? nescore_ahead_load(g_nes, s, n) : gbcore_ahead_load(c, s, n); }
void gbmux_set_quiet(gbcore_t *c, int q)
{ if (IS_GAM(c)) gambatte_host_set_quiet(q); else if (IS_WS(c)) wscore_set_quiet(g_ws, q); else if (IS_NES(c)) nescore_set_quiet(g_nes, q); else gbcore_set_quiet(c, q); }
/* the sensors, the Super Game Boy border and the camera are the other core's: Gambatte runs no cartridge that has a
 * sensor, and has no border */
int gbmux_has_tilt(gbcore_t *c) { return IS_ANY(c) ? 0 : gbcore_has_tilt(c); }
unsigned gbmux_sgb_border(gbcore_t *c, const uint16_t **p)
{ if (IS_ANY(c)) { *p = NULL; return 0; } return gbcore_sgb_border(c, p); }
size_t gbmux_sgb_border_save(gbcore_t *c, void *b, size_t n)
{ return IS_ANY(c) ? 0 : gbcore_sgb_border_save(c, b, n); }
int gbmux_sgb_border_load(gbcore_t *c, const void *b, size_t n)
{ return IS_ANY(c) ? 0 : gbcore_sgb_border_load(c, b, n); }
void gbmux_set_tilt(gbcore_t *c, uint32_t v) { if (!IS_ANY(c)) gbcore_set_tilt(c, v); }
int gbmux_has_camera(gbcore_t *c) { return IS_ANY(c) ? 0 : gbcore_has_camera(c); }
void gbmux_set_camera_image(gbcore_t *c, const uint8_t *p) { if (!IS_ANY(c)) gbcore_set_camera_image(c, p); }
unsigned gbmux_camera_requests(gbcore_t *c) { return IS_ANY(c) ? 0 : gbcore_camera_requests(c); }
uint32_t gbmux_ra_read(gbcore_t *c, uint32_t a, uint8_t *o, uint32_t n)
{ return IS_GAM(c) ? gambatte_host_ra_read(a, o, n) : IS_WS(c) ? wscore_ra_read(g_ws, a, o, n) : IS_NES(c) ? nescore_ra_read(g_nes, a, o, n) : gbcore_ra_read(c, a, o, n); }

int gbmux_peek(gbcore_t *c, uint16_t a, void *out, unsigned n)
{
  unsigned i;
  if (IS_GAM(c)) return gambatte_host_peek(a, out, n);
  if (!IS_NES(c) && !IS_WS(c)) return gbcore_peek(c, a, out, n);
  for (i = 0; i < n; i++)
  {
    int v = IS_WS(c) ? wscore_peek(g_ws, (unsigned)a + i) : nescore_peek(g_nes, (unsigned)a + i);
    if (v < 0) return -1;
    ((uint8_t *)out)[i] = (uint8_t)v;
  }
  return 0;
}

/* The NES keeps its own cheat engine (gbmux_nes_cheats); fe_cheats' RAM
 * writes are not used for it. */
int gbmux_poke(gbcore_t *c, uint16_t a, const void *d, unsigned n)
{ return IS_GAM(c) ? gambatte_host_poke(a, d, n) : IS_NES(c) || IS_WS(c) ? -1 : gbcore_poke(c, a, d, n); }
int gbmux_cheats_set(gbcore_t *c, const gbcore_cheat_t *codes, unsigned n)
{
  if (IS_GAM(c))
    return gambatte_host_cheats_set((const gambatte_host_cheat_t *)(const void *)codes, n);
  return IS_NES(c) || IS_WS(c) ? 0 : gbcore_cheats_set(c, codes, n);
}

/* ---- the two-player link (frontend-common/fe_padpair.h) ----------------------
 * Two consoles run ONE machine, the host's pad on port 1 and the guest's on
 * port 2 (NES), or both onto the one pad (WonderSwan).  fe_host.c owns the
 * session; these are the machine-specific parts of it. */

int gbmux_pad_ok(const gbcore_t *c)
{
  return (gbmux_is_nes(c) && &nescore_run_frame2) ||
         (gbmux_is_ws(c) && &wscore_session_frame);
}

/* What this console's player puts on the wire for the frame it samples: the
 * NES's is the GB bit layout fe_host already has (turned into the console's
 * pad when it runs), the WonderSwan's is the console's own key bits. */
uint16_t gbmux_pad_local(gbcore_t *c, uint16_t gb_buttons)
{
  return gbmux_is_ws(c) ? (uint16_t)wscore_pad_now() : gb_buttons;
}

/* ONE emulated frame with both players' words.  0 = it ran. */
int gbmux_pad_run(gbcore_t *c, uint16_t pad1, uint16_t pad2)
{
  if (gbmux_is_ws(c))
    return wscore_session_frame(g_ws, (unsigned)(pad1 | pad2));
  if (gbmux_is_nes(c))
    return nescore_run_frame2(g_nes, nes_pad(pad1), nes_pad(pad2));
  return -1;
}

/* How many emulated frames the display refresh now starting covers.  The
 * WonderSwan's 75.47 Hz is 1 or 2 (0 when the frames a late partner held up
 * are all done); a PAL / Dendy NES (50 Hz) skips 1 refresh in 6 as it does
 * on its own; an NTSC NES is 1. */
int gbmux_pad_due(gbcore_t *c)
{
  if (gbmux_is_ws(c))
    return wscore_session_due(g_ws);
  if (gbmux_is_nes(c))
  {
    double fps = nescore_fps(g_nes);
    if (fps > 55.0 || fps < 1.0)
      return 1;
    g_nes_acc += fps / g_disp_hz;
    if (g_nes_acc >= 1.0)
    {
      g_nes_acc -= 1.0;
      return 1;
    }
    return 0;
  }
  return 0;
}

/* The refresh's picture, after its frames (`ran` of them) have run. */
void gbmux_pad_present(gbcore_t *c, int ran)
{
  if (gbmux_is_ws(c))
    (void)wscore_session_present(g_ws);
  else if (gbmux_is_nes(c) && !ran)
    (void)nescore_repeat_frame(g_nes);        /* the last picture again */
}

/* The options a session must run alike on both consoles, and their undoing. */
void gbmux_pad_pin(gbcore_t *c, int on)
{
  if (gbmux_is_nes(c))
  {
    nescore_session_pin(g_nes, on);
    g_nes_acc = 0;
  }
}

void gbmux_pad_clock(gbcore_t *c, int64_t seed)
{
  if (gbmux_is_ws(c))
    wscore_session_clock(seed);
}

uint32_t gbmux_pad_save_crc(gbcore_t *c)
{
  return gbmux_is_ws(c) ? wscore_save_ram_crc(g_ws)
       : gbmux_is_nes(c) ? nescore_save_ram_crc(g_nes) : 0;
}

/* The running game's identity for a room: 8 hex digits, the machine's CRC-32
 * of the cartridge (the NES core's of PRG + CHR, the WonderSwan's of the
 * whole file; "00000000" when the core has none, a disk). */
int gbmux_pad_game_id(const gbcore_t *c, char out[9])
{
  uint32_t crc = gbmux_is_nes(c) ? nescore_rom_crc(g_nes)
               : gbmux_is_ws(c) ? wscore_rom_crc() : 0;
  snprintf(out, 9, "%08X", (unsigned)crc);
  return gbmux_is_nes(c) || gbmux_is_ws(c) ? 0 : -1;
}
