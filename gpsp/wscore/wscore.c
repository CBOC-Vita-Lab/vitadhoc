/* Copyright (c) 2026 CBOC-Vita contributors.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * wscore.h over wswan/frontend/host.h (Beetle WonderSwan), blend.h (the
 * 75.47 Hz -> refresh temporal blend) and layout.h (rotation, pad map).
 * Plain C; memory is taken at create and given back at shutdown. */
#include "wscore.h"
#include "../../wswan/frontend/host.h"
#include "../../wswan/frontend/blend.h"
#include "../../wswan/frontend/layout.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AUDIO_CHUNK 2048
#define PIX (WSWAN_WIDTH * WSWAN_HEIGHT)

struct wscore
{
  wscore_callbacks_t cb;
  int16_t audio[AUDIO_CHUNK * 2];
  uint16_t out[PIX];          /* the picture of this refresh, as drawn */
  uint16_t rot[PIX];          /* ...turned upright (144 x 224) */
};

static struct wscore *g_core;
static int g_inited;
static char g_err[96];
static wsb g_blend;
static int g_blend_ok;
static uint64_t g_disp_num = 60000, g_disp_den = 1001;
static int g_rotation = WSCORE_ROT_AUTO;
static int g_vplay = WSCORE_UPRIGHT;
static int g_blend_on = 1;
static int g_palette;
static int g_skip;
static unsigned g_buttons;
static int g_show = -1;
static unsigned g_changes;

const char *wscore_error(void) { return g_err; }

static uint8_t *read_all(const char *path, size_t *n, size_t max)
{
  FILE *f = fopen(path, "rb");
  uint8_t *b;
  long len;
  *n = 0;
  if (!f) return NULL;
  if (fseek(f, 0, SEEK_END) != 0 || (len = ftell(f)) <= 0 || (size_t)len > max ||
      fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
  b = (uint8_t *)malloc((size_t)len);
  if (b && fread(b, 1, (size_t)len, f) != (size_t)len) { free(b); b = NULL; }
  fclose(f);
  if (b) *n = (size_t)len;
  return b;
}

static void blend_setup(void)
{
  if (g_blend_ok) wsb_free(&g_blend);
  g_blend_ok = wsb_init(&g_blend, WSWAN_WIDTH, WSWAN_HEIGHT, WSWAN_CLOCK_HZ,
                        WSWAN_FRAME_CLOCKS, g_disp_num, g_disp_den) == 0;
}

void wscore_set_display_rate(uint64_t num, uint64_t den)
{
  if (!num || !den) return;
  g_disp_num = num;
  g_disp_den = den;
  if (g_core) blend_setup();
}

static int show_now(void)
{
  if (!g_core || wswan_host_orientation() != WSWAN_VERTICAL)
    return WSCORE_SHOW_HORIZONTAL;
  return g_vplay == WSCORE_TURNED ? WSCORE_SHOW_TURNED : WSCORE_SHOW_UPRIGHT;
}

static void show_update(void)
{
  int s = show_now();
  if (s != g_show)
  {
    if (g_show >= 0) g_changes++;
    g_show = s;
  }
}

void wscore_set_rotation(int mode)
{
  g_rotation = mode == WSCORE_ROT_HORIZONTAL || mode == WSCORE_ROT_VERTICAL
               ? mode : WSCORE_ROT_AUTO;
  if (g_core)
  {
    wswan_host_set_orientation_mode(g_rotation == WSCORE_ROT_HORIZONTAL
                                    ? WSWAN_ORIENT_HORIZONTAL
                                    : g_rotation == WSCORE_ROT_VERTICAL
                                    ? WSWAN_ORIENT_VERTICAL : WSWAN_ORIENT_AUTO);
    show_update();
  }
}

void wscore_set_vertical_play(int how)
{
  g_vplay = how == WSCORE_TURNED ? WSCORE_TURNED : WSCORE_UPRIGHT;
  if (g_core) show_update();
}

void wscore_set_blend(int on) { g_blend_on = on != 0; }

void wscore_set_mono_palette(int id)
{
  g_palette = id ? 1 : 0;
  if (g_inited) wswan_host_set_mono_palette(g_palette);
}

int wscore_showing(void) { return g_show < 0 ? WSCORE_SHOW_HORIZONTAL : g_show; }
unsigned wscore_layout_changes(void) { return g_changes; }
void wscore_set_buttons(unsigned vita_bits) { g_buttons = vita_bits; }

wscore_t *wscore_create(const char *rom_path, unsigned audio_rate,
                        const wscore_callbacks_t *callbacks)
{
  size_t n = 0;
  uint8_t *rom;
  const char *name;
  g_err[0] = 0;
  if (g_core) { snprintf(g_err, sizeof(g_err), "a game is already running"); return NULL; }
  if (!g_inited)
  {
    if (wswan_host_init() != 0) { snprintf(g_err, sizeof(g_err), "WonderSwan core failed to start"); return NULL; }
    g_inited = 1;
  }
  rom = read_all(rom_path, &n, 16u << 20);
  if (!rom) { snprintf(g_err, sizeof(g_err), "cannot read the game"); return NULL; }
  name = strrchr(rom_path, '/');
  name = name ? name + 1 : rom_path;
  wswan_host_set_audio_rate(audio_rate ? audio_rate : WSWAN_AUDIO_RATE);
  wswan_host_set_mono_palette(g_palette);
  if (wswan_host_load_mem(rom, n, name, g_err, sizeof(g_err)) != 0)
  {
    free(rom);
    if (!g_err[0]) snprintf(g_err, sizeof(g_err), "not a WonderSwan game");
    return NULL;
  }
  free(rom);
  g_core = (struct wscore *)calloc(1, sizeof(*g_core));
  if (!g_core)
  {
    wswan_host_unload();
    snprintf(g_err, sizeof(g_err), "out of memory");
    return NULL;
  }
  if (callbacks) g_core->cb = *callbacks;
  blend_setup();
  g_show = -1;
  g_changes = 0;
  g_skip = 0;
  wscore_set_rotation(g_rotation);
  show_update();
  return g_core;
}

static void take_audio(struct wscore *c)
{
  int got;
  while ((got = wswan_host_audio_take(c->audio, AUDIO_CHUNK)) > 0)
    if (c->cb.audio_batch)
      c->cb.audio_batch(c->cb.userdata, c->audio, (size_t)got);
}

int wscore_run_frame(wscore_t *core)
{
  struct wscore *c = core;
  const uint16_t *px;
  int w, h, pitch, n, i, blend;
  wscore_video_frame_t vf;
  if (!c || c != g_core) return -1;
  blend = g_blend_on && g_blend_ok;
  n = g_blend_ok ? wsb_frames_needed(&g_blend) : 1;
  /* the layout of this refresh: its pad map is the picture's */
  show_update();
  wswan_host_set_pad(wsl_map_pad(g_buttons & 0xFFFu,
                                 g_show != WSCORE_SHOW_HORIZONTAL,
                                 g_show == WSCORE_SHOW_TURNED ? WSL_TURNED : WSL_UPRIGHT));
  for (i = 0; i < n; i++)
  {
    /* a frame the picture does not use is not drawn (sound, state equal) */
    wswan_host_set_render(!g_skip && (blend || i == n - 1));
    wswan_host_run_frame();
    take_audio(c);
    if (g_blend_ok)
    {
      px = wswan_host_video(&w, &h, &pitch);
      if (px && pitch == WSWAN_WIDTH * 2) wsb_submit(&g_blend, px);
      else g_blend.emu_done++;
    }
  }
  wswan_host_set_render(1);
  show_update();
  if (g_skip)
  {
    if (g_blend_ok) g_blend.t += g_blend.td;     /* the refresh passes */
    if (c->cb.video)
    {
      vf.pixels = c->out; vf.width = wscore_frame_width(c);
      vf.height = wscore_frame_height(c); vf.pitch_bytes = vf.width * 2;
      c->cb.video(c->cb.userdata, &vf);         /* ignored while skipping */
    }
    return 0;
  }
  if (blend)
    wsb_present(&g_blend, c->out);
  else
  {
    px = wswan_host_video(&w, &h, &pitch);
    if (g_blend_ok) g_blend.t += g_blend.td;
    if (px)
      for (i = 0; i < WSWAN_HEIGHT; i++)
        memcpy(c->out + i * WSWAN_WIDTH, (const uint8_t *)px + (size_t)i * (size_t)pitch,
               WSWAN_WIDTH * 2);
  }
  if (c->cb.video)
  {
    if (g_show == WSCORE_SHOW_UPRIGHT)
    {
      wsl_rotate_ccw(c->rot, c->out);
      vf.pixels = c->rot; vf.width = WSWAN_HEIGHT; vf.height = WSWAN_WIDTH;
    }
    else
    {
      vf.pixels = c->out; vf.width = WSWAN_WIDTH; vf.height = WSWAN_HEIGHT;
    }
    vf.pitch_bytes = vf.width * 2;
    c->cb.video(c->cb.userdata, &vf);
  }
  return 0;
}

void wscore_set_skip_render(wscore_t *core, int skip) { (void)core; g_skip = skip != 0; }

void wscore_shutdown(wscore_t *core)
{
  if (!core || core != g_core) return;
  wswan_host_unload();
  wswan_host_set_render(1);
  if (g_blend_ok) wsb_free(&g_blend);
  g_blend_ok = 0;
  free(g_core);
  g_core = NULL;
  g_show = -1;
}

unsigned wscore_sample_rate(const wscore_t *core) { (void)core; return wswan_host_audio_rate(); }
unsigned wscore_frame_width(const wscore_t *core)
{ (void)core; return wscore_showing() == WSCORE_SHOW_UPRIGHT ? WSWAN_HEIGHT : WSWAN_WIDTH; }
unsigned wscore_frame_height(const wscore_t *core)
{ (void)core; return wscore_showing() == WSCORE_SHOW_UPRIGHT ? WSWAN_WIDTH : WSWAN_HEIGHT; }
double wscore_fps(const wscore_t *core) { (void)core; return wswan_host_frame_rate(); }
uint32_t wscore_rom_crc(void) { return wswan_host_rom_crc(); }
int wscore_is_color(void) { return wswan_host_game_is_color(); }

int wscore_rom_title(wscore_t *core, char *dst, size_t cap)
{
  uint8_t f[16];
  (void)core;
  if (!dst || !cap) return -1;
  /* the cartridge has no title: its developer and cart ids name it */
  if (wswan_host_footer(f))
    snprintf(dst, cap, "WS-%02X%02X", f[6], f[8]);
  else
    dst[0] = 0;
  return 0;
}

size_t wscore_save_ram_size(wscore_t *core) { return core == g_core && core ? wswan_host_srm_size() : 0; }

int wscore_save_ram_read(wscore_t *core, void *dst, size_t cap)
{
  size_t n = wscore_save_ram_size(core);
  if (!n || cap < n) return -1;
  return wswan_host_srm_export((uint8_t *)dst, cap) == n ? 0 : -1;
}

int wscore_save_ram_write(wscore_t *core, const void *src, size_t size)
{
  if (!core || core != g_core) return -1;
  wswan_host_srm_import((const uint8_t *)src, size);
  return 0;
}

int wscore_save_ram_dirty(wscore_t *core) { return core && core == g_core ? wswan_host_srm_dirty() : 0; }

size_t wscore_state_size(wscore_t *core) { return core && core == g_core ? wswan_host_state_size() : 0; }

long wscore_state_save(wscore_t *core, void *dst, size_t cap)
{
  size_t r;
  if (!core || core != g_core) return -1;
  r = wswan_host_state_save_mem((uint8_t *)dst, cap);
  return r ? (long)r : -1;
}

int wscore_state_load(wscore_t *core, const void *src, size_t size)
{
  if (!core || core != g_core) return -1;
  if (wswan_host_state_load_mem((const uint8_t *)src, size) != 0) return -1;
  if (g_blend_ok) wsb_reset(&g_blend);    /* the phase starts again */
  show_update();
  return 0;
}

uint32_t wscore_ra_read(wscore_t *core, uint32_t addr, uint8_t *out, uint32_t len)
{
  (void)core;
  return (uint32_t)wswan_host_ra_read(addr, out, len);
}

int wscore_peek(wscore_t *core, unsigned address) { (void)core; return wswan_host_peek(address); }

int wscore_cheats_set(wscore_t *core, const char *codes)
{
  (void)core;
  wswan_host_cheat_reset();
  return codes && codes[0] ? wswan_host_cheat_add(codes) : 0;
}

void wscore_reset(wscore_t *core)
{
  (void)core;
  wswan_host_reset();
  if (g_blend_ok) wsb_reset(&g_blend);
}
