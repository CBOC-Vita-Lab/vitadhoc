/* Copyright (c) 2026 CBOC-Vita contributors.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * nescore.h over nes/frontend/host.h (FCEUmm).  Plain C, no allocation but the
 * ROM file read at create. */
#include "nescore.h"
#include "../../nes/frontend/host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "../frontend-common/fe_evt.h"

#define AUDIO_CHUNK 2048

struct nescore
{
  nescore_callbacks_t cb;
  unsigned width, height;
  int top;
  int16_t audio[AUDIO_CHUNK * 2];
};

static struct nescore g_core;
static int g_live;
static int g_inited;
static char g_err[96];
static char g_sysdir[512];
static int g_wide = NESCORE_WIDE_AUTO;
static int g_wide_shown;
static nescore_video_frame_t g_last_vf;   /* the last picture handed over */
static unsigned g_wide_dark;     /* frames in a row with no real side picture */
#define WIDE_MARGIN 84           /* 240 lines at square pixels on 16:9 */

void nescore_set_wide(int mode)
{
  g_wide = mode == NESCORE_WIDE_ON || mode == NESCORE_WIDE_OFF ? mode : NESCORE_WIDE_AUTO;
  if (g_live)
  {
    nes_host_wide_enable(g_wide == NESCORE_WIDE_OFF ? 0 : 1);
    nes_host_wide_margin(g_wide == NESCORE_WIDE_OFF ? 0 : WIDE_MARGIN);
  }
}
int nescore_wide_shown(void) { return g_wide_shown; }

static int wide_map_path(char *p, size_t cap)
{
  uint32_t key = nes_host_wide_map_key();
  if (!key || !g_sysdir[0]) return -1;
  return snprintf(p, cap, "%s/wide/%08x.map", g_sysdir, (unsigned)key) < (int)cap ? 0 : -1;
}

static void wide_map_save(void)
{
  char p[600], tmp[610];
  size_t n;
  uint8_t *b;
  FILE *f;
  int ok;
  if (g_wide == NESCORE_WIDE_OFF || !nes_host_wide_map_dirty() || wide_map_path(p, sizeof(p)) != 0)
  {
    fe_evt("nes_wide_map saved=0 dirty=%d key=%08x", nes_host_wide_map_dirty(),
           (unsigned)nes_host_wide_map_key());
    return;
  }
  n = nes_host_wide_map_save(NULL, 0);
  if (!n || !(b = (uint8_t *)malloc(n))) return;
  n = nes_host_wide_map_save(b, n);
  {
    char d[600];
    mkdir(g_sysdir, 0777);          /* (already there is the normal case) */
    snprintf(d, sizeof(d), "%s/wide", g_sysdir);
    mkdir(d, 0777);
  }
  /* written whole beside it, then put in its place: a cut leaves the old map */
  snprintf(tmp, sizeof(tmp), "%s.tmp", p);
  f = fopen(tmp, "wb");
  ok = f && fwrite(b, 1, n, f) == n;
  if (f && fclose(f) != 0) ok = 0;
  if (ok && rename(tmp, p) != 0) ok = 0;
  if (!ok) remove(tmp);
  fe_evt("nes_wide_map saved=%d bytes=%u", ok, (unsigned)n);
  free(b);
}

void nescore_set_system_dir(const char *dir)
{
  snprintf(g_sysdir, sizeof(g_sysdir), "%s", dir ? dir : "");
}

const char *nescore_error(void) { return g_err; }

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

/* disksys.rom: the system folder first, then beside the game. */
static void find_bios(const char *rom_path)
{
  char p[640];
  size_t n = 0;
  uint8_t *b = NULL;
  const char *slash = strrchr(rom_path, '/');
  if (g_sysdir[0])
  {
    snprintf(p, sizeof(p), "%s/disksys.rom", g_sysdir);
    b = read_all(p, &n, 1 << 20);
  }
  if (!b && slash)
  {
    snprintf(p, sizeof(p), "%.*s/disksys.rom", (int)(slash - rom_path), rom_path);
    b = read_all(p, &n, 1 << 20);
  }
  nes_host_set_fds_bios(b, n);
  free(b);
}

nescore_t *nescore_create(const char *rom_path, const char *srm_path,
                          const nescore_callbacks_t *callbacks)
{
  size_t n = 0;
  uint8_t *rom;
  const char *name;
  int top = 0, bottom = 0;
  g_err[0] = 0;
  if (g_live) { snprintf(g_err, sizeof(g_err), "a game is already running"); return NULL; }
  if (!g_inited)
  {
    if (nes_host_init() != 0) { snprintf(g_err, sizeof(g_err), "NES core failed to start"); return NULL; }
    g_inited = 1;
  }
  rom = read_all(rom_path, &n, 16u << 20);
  if (!rom) { snprintf(g_err, sizeof(g_err), "cannot read the game"); return NULL; }
  find_bios(rom_path);
  name = strrchr(rom_path, '/');
  name = name ? name + 1 : rom_path;
  nes_host_set_audio_rate(48000);
  if (nes_host_load_mem(rom, n, name, g_err, sizeof(g_err)) != 0)
  {
    free(rom);
    if (!g_err[0]) snprintf(g_err, sizeof(g_err), "not a NES game");
    return NULL;
  }
  free(rom);
  memset(&g_core, 0, sizeof(g_core));
  if (callbacks) g_core.cb = *callbacks;
  nes_host_overscan(&top, &bottom);
  g_core.top = top;
  g_core.width = NES_WIDTH;
  g_core.height = (unsigned)(NES_HEIGHT - top - bottom);
  if (srm_path)
  {
    size_t sn = 0;
    uint8_t *s = read_all(srm_path, &sn, 4u << 20);
    if (s) { nes_host_sram_load(s, sn); free(s); }
  }
  g_live = 1;
  g_wide_shown = 0;
  g_wide_dark = 0;
  memset(&g_last_vf, 0, sizeof(g_last_vf));
  nescore_set_wide(g_wide);
  if (g_wide != NESCORE_WIDE_OFF)
  {
    char p[600];
    size_t mn = 0;
    uint8_t *m = wide_map_path(p, sizeof(p)) == 0 ? read_all(p, &mn, 16u << 20) : NULL;
    if (m) { nes_host_wide_map_load(m, mn); free(m); }
  }
  return &g_core;
}

int nescore_run_frame(nescore_t *core, unsigned buttons)
{
  nescore_video_frame_t vf;
  const uint16_t *px;
  int w, h, pitch, got;
  if (core != &g_core || !g_live) return -1;
  nes_host_set_pad(0, buttons & 0xFFu);
  nes_host_run_frame();
  px = nes_host_video(&w, &h, &pitch);
  if (g_wide != NESCORE_WIDE_OFF)
  {
    const uint16_t *wp = NULL;
    int ww = 0, wpitch = 0;
    nes_wide_stats st;
    if (nes_host_wide_frame(&wp, &ww, &wpitch, &st) && wp)
    {
      /* Auto: the sides only while there is something real to show in
       * them (a second without any: the original picture again) */
      g_wide_dark = st.real_l + st.real_r > 0 ? 0 : g_wide_dark + 1;
      if (g_wide == NESCORE_WIDE_ON || g_wide_dark < 60)
      {
        px = wp; w = ww; pitch = wpitch;
      }
    }
  }
  g_wide_shown = w > NES_WIDTH;
  if (core->cb.video && px)
  {
    vf.pixels = (const uint16_t *)((const uint8_t *)px + (size_t)core->top * (size_t)pitch);
    vf.width = (unsigned)w;
    vf.height = core->height;
    vf.pitch_bytes = (unsigned)pitch;
    g_last_vf = vf;
    core->cb.video(core->cb.userdata, &vf);
  }
  while ((got = nes_host_audio_take(core->audio, AUDIO_CHUNK)) > 0)
    if (core->cb.audio_batch)
      core->cb.audio_batch(core->cb.userdata, core->audio, (size_t)got);
  return 0;
}

int nescore_repeat_frame(nescore_t *core)
{
  nescore_video_frame_t vf;
  const uint16_t *px;
  int w, h, pitch;
  if (core != &g_core || !g_live) return -1;
  if (g_last_vf.pixels && core->cb.video)
  {
    core->cb.video(core->cb.userdata, &g_last_vf);   /* the same picture again */
    return 0;
  }
  px = nes_host_video(&w, &h, &pitch);
  if (core->cb.video && px)
  {
    vf.pixels = (const uint16_t *)((const uint8_t *)px + (size_t)core->top * (size_t)pitch);
    vf.width = core->width;
    vf.height = core->height;
    vf.pitch_bytes = (unsigned)pitch;
    core->cb.video(core->cb.userdata, &vf);
  }
  return 0;
}

void nescore_set_skip_render(nescore_t *core, int skip)
{
  (void)core;
  nes_host_set_render(!skip);
}

void nescore_shutdown(nescore_t *core)
{
  if (core != &g_core || !g_live) return;
  wide_map_save();
  nes_host_unload();
  nes_host_set_render(1);
  g_live = 0;
}

unsigned nescore_sample_rate(const nescore_t *core) { (void)core; return nes_host_audio_rate(); }
unsigned nescore_frame_width(const nescore_t *core) { return core ? core->width : 0; }
unsigned nescore_frame_height(const nescore_t *core) { return core ? core->height : 0; }
double nescore_fps(const nescore_t *core) { (void)core; return nes_host_fps(); }

int nescore_rom_title(nescore_t *core, char *dst, size_t cap)
{
  const char *t = nes_host_rom_title();
  (void)core;
  if (!dst || !cap) return -1;
  snprintf(dst, cap, "%s", t ? t : "");
  return 0;
}
uint32_t nescore_rom_crc(const nescore_t *core) { (void)core; return nes_host_rom_crc(); }

size_t nescore_save_ram_size(nescore_t *core) { return core == &g_core ? nes_host_sram_size() : 0; }

int nescore_save_ram_read(nescore_t *core, void *dst, size_t cap)
{
  size_t n = nescore_save_ram_size(core);
  if (!n || cap < n) return -1;
  memcpy(dst, nes_host_sram_data(), n);
  return 0;
}

int nescore_save_ram_write(nescore_t *core, const void *src, size_t size)
{
  if (core != &g_core || !nes_host_sram_size()) return -1;
  nes_host_sram_load((const uint8_t *)src, size);
  return 0;
}

int nescore_save_ram_dirty(nescore_t *core)
{
  return core == &g_core ? nes_host_sram_dirty() : 0;
}

size_t nescore_state_size(nescore_t *core) { return core == &g_core ? nes_host_state_size() : 0; }

long nescore_state_save(nescore_t *core, void *dst, size_t cap)
{
  size_t r;
  if (core != &g_core) return -1;
  r = nes_host_state_save_mem((uint8_t *)dst, cap);
  return r ? (long)r : -1;
}

int nescore_state_load(nescore_t *core, const void *src, size_t size)
{
  if (core != &g_core) return -1;
  return nes_host_state_load_mem((const uint8_t *)src, size) == 0 ? 0 : -1;
}

int nescore_peek(nescore_t *core, unsigned address)
{
  (void)core;
  return nes_host_peek(address);
}

uint32_t nescore_ra_read(nescore_t *core, uint32_t addr, uint8_t *out, uint32_t len)
{
  (void)core;
  return (uint32_t)nes_host_ra_read(addr, out, len);
}

int nescore_cheats_set(nescore_t *core, const char *codes)
{
  (void)core;
  nes_host_cheat_reset();
  return codes && codes[0] ? nes_host_cheat_add(codes) : 0;
}

void nescore_reset(nescore_t *core) { (void)core; nes_host_reset(); }
void nescore_set_fourscore(nescore_t *core, int mode) { (void)core; nes_host_set_fourscore(mode); }
