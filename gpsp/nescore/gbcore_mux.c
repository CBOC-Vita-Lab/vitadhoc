/* Copyright (c) 2026 CBOC-Vita contributors.
 * SPDX-License-Identifier: GPL-2.0-or-later
 * See gbcore_mux.h. */
#include "gbcore_mux.h"
#include "nescore.h"
#include "../wscore/wscore.h"

#include <string.h>
#include "../frontend-common/fe_evt.h"

static nescore_t *g_nes;                 /* the running NES machine, if any */
static gbcore_callbacks_t g_gbcb;        /* the frontend's callbacks for it */

static wscore_t *g_ws;                   /* the running WonderSwan machine, if any */

#define IS_NES(c) (g_nes && ((const void *)(c) == (const void *)g_nes || !(c)))
#define IS_WS(c) (g_ws && ((const void *)(c) == (const void *)g_ws || !(c)))

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
void gbmux_set_display_rate(double hz) { if (hz > 1.0) g_disp_hz = hz; g_nes_acc = 0; }

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
    frames++;
    rc = nescore_run_frame(g_nes, nes_pad(b));
  }
  else
    rc = nescore_repeat_frame(g_nes);
  if (calls % 600 == 0)
    fe_evt("nes_pace fps=%.3f display=%.3f frames=%u calls=%u", fps, g_disp_hz, frames, calls);
  return rc;
}

int gbmux_run_frame(gbcore_t *c, uint16_t b)
{ if (IS_WS(c)) return wscore_run_frame(g_ws);   /* buttons: wscore_set_buttons */
  if (IS_NES(c)) return nes_pal_frame(c, b);
  return IS_NES(c) ? nescore_run_frame(g_nes, nes_pad(b)) : gbcore_run_frame(c, b); }
void gbmux_set_skip_render(gbcore_t *c, int s)
{ if (IS_WS(c)) wscore_set_skip_render(g_ws, s); else if (IS_NES(c)) nescore_set_skip_render(g_nes, s); else gbcore_set_skip_render(c, s); }
void gbmux_shutdown(gbcore_t *c)
{
  if (IS_WS(c)) { wscore_shutdown(g_ws); g_ws = NULL; }
  else if (IS_NES(c)) { nescore_shutdown(g_nes); g_nes = NULL; }
  else gbcore_shutdown(c);
}
unsigned gbmux_sample_rate(const gbcore_t *c)
{ return gbmux_is_ws(c) ? wscore_sample_rate(g_ws) : gbmux_is_nes(c) ? nescore_sample_rate(g_nes) : gbcore_sample_rate(c); }
unsigned gbmux_frame_width(const gbcore_t *c)
{ return gbmux_is_ws(c) ? wscore_frame_width(g_ws) : gbmux_is_nes(c) ? nescore_frame_width(g_nes) : gbcore_frame_width(c); }
unsigned gbmux_frame_height(const gbcore_t *c)
{ return gbmux_is_ws(c) ? wscore_frame_height(g_ws) : gbmux_is_nes(c) ? nescore_frame_height(g_nes) : gbcore_frame_height(c); }
int gbmux_rom_title(gbcore_t *c, char *d, size_t n)
{ return IS_WS(c) ? wscore_rom_title(g_ws, d, n) : IS_NES(c) ? nescore_rom_title(g_nes, d, n) : gbcore_rom_title(c, d, n); }
size_t gbmux_save_ram_size(gbcore_t *c)
{ return IS_WS(c) ? wscore_save_ram_size(g_ws) : IS_NES(c) ? nescore_save_ram_size(g_nes) : gbcore_save_ram_size(c); }
size_t gbmux_cart_ram_size(gbcore_t *c)
{ return IS_WS(c) ? wscore_save_ram_size(g_ws) : IS_NES(c) ? nescore_save_ram_size(g_nes) : gbcore_cart_ram_size(c); }
int gbmux_save_ram_read(gbcore_t *c, void *d, size_t n)
{ return IS_WS(c) ? wscore_save_ram_read(g_ws, d, n) : IS_NES(c) ? nescore_save_ram_read(g_nes, d, n) : gbcore_save_ram_read(c, d, n); }
int gbmux_save_ram_write(gbcore_t *c, const void *s, size_t n)
{ return IS_WS(c) ? wscore_save_ram_write(g_ws, s, n) : IS_NES(c) ? nescore_save_ram_write(g_nes, s, n) : gbcore_save_ram_write(c, s, n); }
size_t gbmux_state_size(gbcore_t *c)
{ return IS_WS(c) ? wscore_state_size(g_ws) : IS_NES(c) ? nescore_state_size(g_nes) : gbcore_state_size(c); }
long gbmux_state_save(gbcore_t *c, void *d, size_t n)
{ return IS_WS(c) ? wscore_state_save(g_ws, d, n) : IS_NES(c) ? nescore_state_save(g_nes, d, n) : gbcore_state_save(c, d, n); }
int gbmux_state_load(gbcore_t *c, const void *s, size_t n)
{ return IS_WS(c) ? wscore_state_load(g_ws, s, n) : IS_NES(c) ? nescore_state_load(g_nes, s, n) : gbcore_state_load(c, s, n); }
int gbmux_has_tilt(gbcore_t *c) { return IS_NES(c) || IS_WS(c) ? 0 : gbcore_has_tilt(c); }
unsigned gbmux_sgb_border(gbcore_t *c, const uint16_t **p)
{ if (IS_NES(c) || IS_WS(c)) { *p = NULL; return 0; } return gbcore_sgb_border(c, p); }
size_t gbmux_sgb_border_save(gbcore_t *c, void *b, size_t n)
{ return IS_NES(c) || IS_WS(c) ? 0 : gbcore_sgb_border_save(c, b, n); }
int gbmux_sgb_border_load(gbcore_t *c, const void *b, size_t n)
{ return IS_NES(c) || IS_WS(c) ? 0 : gbcore_sgb_border_load(c, b, n); }
void gbmux_set_tilt(gbcore_t *c, uint32_t v) { if (!IS_NES(c) && !IS_WS(c)) gbcore_set_tilt(c, v); }
uint32_t gbmux_ra_read(gbcore_t *c, uint32_t a, uint8_t *o, uint32_t n)
{ return IS_WS(c) ? wscore_ra_read(g_ws, a, o, n) : IS_NES(c) ? nescore_ra_read(g_nes, a, o, n) : gbcore_ra_read(c, a, o, n); }

int gbmux_peek(gbcore_t *c, uint16_t a, void *out, unsigned n)
{
  unsigned i;
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
{ return IS_NES(c) || IS_WS(c) ? -1 : gbcore_poke(c, a, d, n); }
int gbmux_cheats_set(gbcore_t *c, const gbcore_cheat_t *codes, unsigned n)
{ return IS_NES(c) || IS_WS(c) ? 0 : gbcore_cheats_set(c, codes, n); }
