/* fe_retro.c -- the frontend side of the Mega Drive / NES / SNES cores.
 * See fe_retro.h.  SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The three cores are unmodified libretro cores (Genesis Plus GX, FCEUmm,
 * Snes9x 2005), linked as isolated objects whose only global symbols are
 * md_retro_*, nes_retro_* and snes_retro_* (vita/retrocores.mk).  This file
 * is the libretro "frontend" they expect:
 *
 *   environment   the handful of calls these cores make, answered the way a
 *                 small frontend does (RGB565 or XRGB8888 pictures, a system
 *                 and save directory, core options at their defaults, the
 *                 joypad bitmask)
 *   video         RGB565 handed on as it is; XRGB8888 converted once per
 *                 frame into a buffer of our own
 *   audio         int16 stereo batches, passed on at the rate the core
 *                 reports (the platform resamples from fe_retro_sample_rate)
 *   input         one joypad on port 0, the mask the platform supplies
 *   battery       the core's save RAM <-> <game>.srm, the layout every other
 *                 libretro frontend uses, so a RetroArch save can be dropped
 *                 beside the game; written only when it changed
 *   states        retro_serialize behind a 16-byte header
 *
 * Compiled without FE_RETRO_CORES (the host test twin) it keeps the API and
 * reports every console unavailable.
 */
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fe_retro.h"
#include "fe_evt.h"
#include "fe_util.h"
#include "libretro.h"

#ifdef FE_RETRO_CORES
#include <malloc.h>
/* Genesis Plus GX's bundled LZMA asks for posix_memalign, which the Vita's
 * newlib does not have. */
int posix_memalign(void **out, size_t align, size_t size)
{
   void *m = memalign(align, size);
   if (!m)
      return ENOMEM;
   *out = m;
   return 0;
}
#endif

#define FR_NCORES 3

enum { FR_MD = 0, FR_NES, FR_SNES };

typedef struct fr_api
{
   const char *name;
   void   (*set_environment)(retro_environment_t);
   void   (*set_video_refresh)(retro_video_refresh_t);
   void   (*set_audio_sample)(retro_audio_sample_t);
   void   (*set_audio_sample_batch)(retro_audio_sample_batch_t);
   void   (*set_input_poll)(retro_input_poll_t);
   void   (*set_input_state)(retro_input_state_t);
   void   (*init)(void);
   void   (*deinit)(void);
   bool   (*load_game)(const struct retro_game_info *);
   void   (*unload_game)(void);
   void   (*run)(void);
   void   (*reset)(void);
   size_t (*serialize_size)(void);
   bool   (*serialize)(void *, size_t);
   bool   (*unserialize)(const void *, size_t);
   void   (*get_system_av_info)(struct retro_system_av_info *);
   void  *(*get_memory_data)(unsigned);
   size_t (*get_memory_size)(unsigned);
   void   (*set_controller_port_device)(unsigned, unsigned);
} fr_api;

#ifdef FE_RETRO_CORES
#define FR_DECLARE(P) \
   extern void P##_retro_set_environment(retro_environment_t); \
   extern void P##_retro_set_video_refresh(retro_video_refresh_t); \
   extern void P##_retro_set_audio_sample(retro_audio_sample_t); \
   extern void P##_retro_set_audio_sample_batch(retro_audio_sample_batch_t); \
   extern void P##_retro_set_input_poll(retro_input_poll_t); \
   extern void P##_retro_set_input_state(retro_input_state_t); \
   extern void P##_retro_init(void); \
   extern void P##_retro_deinit(void); \
   extern bool P##_retro_load_game(const struct retro_game_info *); \
   extern void P##_retro_unload_game(void); \
   extern void P##_retro_run(void); \
   extern void P##_retro_reset(void); \
   extern size_t P##_retro_serialize_size(void); \
   extern bool P##_retro_serialize(void *, size_t); \
   extern bool P##_retro_unserialize(const void *, size_t); \
   extern void P##_retro_get_system_av_info(struct retro_system_av_info *); \
   extern void *P##_retro_get_memory_data(unsigned); \
   extern size_t P##_retro_get_memory_size(unsigned); \
   extern void P##_retro_set_controller_port_device(unsigned, unsigned);
#define FR_TABLE(P, NAME) { NAME, \
   P##_retro_set_environment, P##_retro_set_video_refresh, \
   P##_retro_set_audio_sample, P##_retro_set_audio_sample_batch, \
   P##_retro_set_input_poll, P##_retro_set_input_state, P##_retro_init, \
   P##_retro_deinit, P##_retro_load_game, P##_retro_unload_game, \
   P##_retro_run, P##_retro_reset, P##_retro_serialize_size, \
   P##_retro_serialize, P##_retro_unserialize, \
   P##_retro_get_system_av_info, P##_retro_get_memory_data, \
   P##_retro_get_memory_size, P##_retro_set_controller_port_device }

FR_DECLARE(md)
FR_DECLARE(nes)
FR_DECLARE(snes)

static const fr_api g_api[FR_NCORES] = {
   FR_TABLE(md,   "Genesis Plus GX"),
   FR_TABLE(nes,  "FCEUmm"),
   FR_TABLE(snes, "Snes9x 2005"),
};
#define FR_LINKED 1
#else
static const fr_api g_api[FR_NCORES];
#define FR_LINKED 0
#endif

/* ------------------------------------------------------------- console -- */

int fe_console_is_retro(fe_console_t c)
{
   return c == FE_CONSOLE_MD || c == FE_CONSOLE_NES || c == FE_CONSOLE_SNES;
}

static int core_index(fe_console_t c)
{
   return c == FE_CONSOLE_MD ? FR_MD : c == FE_CONSOLE_NES ? FR_NES :
          c == FE_CONSOLE_SNES ? FR_SNES : -1;
}

int fe_retro_available(fe_console_t c)
{
   return FR_LINKED && core_index(c) >= 0;
}

/* --------------------------------------------------------------- state -- */

#define FR_HEADER 16u            /* "RTRO", version, console, core size    */
#define FR_STATE_VERSION 1u

static struct
{
   int active;
   int ci;
   fe_retro_config cfg;
   const fr_api *api;

   enum retro_pixel_format fmt;
   unsigned w, h, rate;
   double fps;

   const uint16_t *last;        /* the picture last shown                 */
   size_t last_pitch;
   uint16_t *conv;              /* XRGB8888 -> RGB565 staging             */
   size_t conv_cap;
   int skip;                    /* this frame's picture is not to be drawn */

   int16_t *abuf;               /* single-sample audio, flushed per frame */
   size_t acount, acap;

   /* battery */
   uint8_t *sram;               /* the core's own buffer                  */
   size_t sram_size;
   uint32_t sram_crc;           /* of what the file holds / last write     */
   int battery_failed;
   unsigned save_fails;

   /* core options: key -> value, defaults from SET_VARIABLES */
   struct { char key[64]; char val[64]; } opt[96];
   int nopt;

   char sysdir[512], savedir[512];
} R;

static uint32_t crc32_of(const uint8_t *p, size_t n)
{
   return (uint32_t)fe_crc32(0, p, n);
}

/* ------------------------------------------------------------- options -- */

static void opt_set(const char *key, const char *val)
{
   int i;
   for (i = 0; i < R.nopt; i++)
      if (!strcmp(R.opt[i].key, key))
      {
         snprintf(R.opt[i].val, sizeof(R.opt[i].val), "%s", val);
         return;
      }
   if (R.nopt >= (int)(sizeof(R.opt) / sizeof(R.opt[0])))
      return;
   snprintf(R.opt[R.nopt].key, sizeof(R.opt[R.nopt].key), "%s", key);
   snprintf(R.opt[R.nopt].val, sizeof(R.opt[R.nopt].val), "%s", val);
   R.nopt++;
}

/* Legacy variables: "Label; first|second|third" -- the first is the default. */
static void options_from_variables(const struct retro_variable *v)
{
   for (; v && v->key; v++)
   {
      const char *semi = v->value ? strchr(v->value, ';') : NULL;
      char def[64];
      size_t n = 0;
      if (!semi)
         continue;
      semi++;
      while (*semi == ' ')
         semi++;
      while (semi[n] && semi[n] != '|' && n < sizeof(def) - 1)
      {
         def[n] = semi[n];
         n++;
      }
      def[n] = '\0';
      opt_set(v->key, def);
   }
}

/* Per-core overrides of the defaults: what is right for a handheld screen
 * without a CRT filter, kept next to the table that applies them. */
static void options_tune(int ci)
{
   if (ci == FR_MD)
   {
      /* 16-bit picture, no NTSC filter, no per-channel audio panning */
      opt_set("genesis_plus_gx_blargg_ntsc_filter", "disabled");
      opt_set("genesis_plus_gx_render", "single field");
   }
   else if (ci == FR_NES)
   {
      opt_set("fceumm_ntsc_filter", "disabled");
      opt_set("fceumm_sndquality", "Low");
   }
}

/* --------------------------------------------------------- environment -- */

static void fr_log(enum retro_log_level level, const char *fmt, ...)
{
   char buf[256];
   va_list ap;
   size_t n;
   if (level < RETRO_LOG_WARN)
      return;
   va_start(ap, fmt);
   vsnprintf(buf, sizeof(buf), fmt, ap);
   va_end(ap);
   n = strlen(buf);
   while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
      buf[--n] = '\0';
   fe_log("retro[%s]: %s", R.api ? R.api->name : "?", buf);
}

static bool env_cb(unsigned cmd, void *data)
{
   switch (cmd)
   {
   case RETRO_ENVIRONMENT_SET_ROTATION:
   case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL:
   case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
   case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
   case RETRO_ENVIRONMENT_SET_MEMORY_MAPS:
   case RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS:
   case RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS:
   case RETRO_ENVIRONMENT_SET_SUBSYSTEM_INFO:
   case RETRO_ENVIRONMENT_SET_CONTENT_INFO_OVERRIDE:
   case RETRO_ENVIRONMENT_SET_MINIMUM_AUDIO_LATENCY:
      return true;
   case RETRO_ENVIRONMENT_GET_OVERSCAN:
      if (data) *(bool *)data = false;
      return true;
   case RETRO_ENVIRONMENT_GET_CAN_DUPE:
      if (data) *(bool *)data = true;
      return true;
   case RETRO_ENVIRONMENT_SET_MESSAGE:
      if (data && ((const struct retro_message *)data)->msg)
         fe_log("retro[%s]: %s", R.api ? R.api->name : "?",
                ((const struct retro_message *)data)->msg);
      return true;
   case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
      if (data) *(const char **)data = R.sysdir;
      return true;
   case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
   case RETRO_ENVIRONMENT_GET_CONTENT_DIRECTORY:
      if (data) *(const char **)data = R.savedir;
      return true;
   case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
   {
      enum retro_pixel_format f = data ? *(enum retro_pixel_format *)data
                                       : RETRO_PIXEL_FORMAT_0RGB1555;
      if (f != RETRO_PIXEL_FORMAT_RGB565 && f != RETRO_PIXEL_FORMAT_XRGB8888)
      {
         fe_log("retro: refused pixel format %d", (int)f);
         return false;
      }
      R.fmt = f;
      return true;
   }
   case RETRO_ENVIRONMENT_SET_VARIABLES:
      options_from_variables((const struct retro_variable *)data);
      options_tune(R.ci);
      return true;
   case RETRO_ENVIRONMENT_GET_VARIABLE:
   {
      struct retro_variable *v = (struct retro_variable *)data;
      int i;
      if (!v || !v->key)
         return false;
      for (i = 0; i < R.nopt; i++)
         if (!strcmp(R.opt[i].key, v->key))
         {
            v->value = R.opt[i].val;
            return true;
         }
      v->value = NULL;
      return false;
   }
   case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
      if (data) *(bool *)data = false;
      return true;
   case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
      if (data) ((struct retro_log_callback *)data)->log = fr_log;
      return true;
   case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
      return true;
   case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
      if (data) *(unsigned *)data = 0;      /* legacy SET_VARIABLES only */
      return true;
   case RETRO_ENVIRONMENT_GET_LANGUAGE:
      if (data) *(unsigned *)data = RETRO_LANGUAGE_ENGLISH;
      return true;
   case RETRO_ENVIRONMENT_SET_GEOMETRY:
      if (data)
      {
         const struct retro_game_geometry *g = data;
         R.w = g->base_width;
         R.h = g->base_height;
      }
      return true;
   case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
      if (data)
      {
         const struct retro_system_av_info *a = data;
         R.w = a->geometry.base_width;
         R.h = a->geometry.base_height;
         R.rate = (unsigned)(a->timing.sample_rate + 0.5);
         R.fps = a->timing.fps;
      }
      return true;
   default:
      /* everything else -- the VFS and perf interfaces, rumble, sensors,
       * the message interface -- the cores do without */
      return false;
   }
}

/* --------------------------------------------------------------- video -- */

static void video_cb(const void *data, unsigned w, unsigned h, size_t pitch)
{
   if (!R.cfg.video_frame)
      return;
   if (!data || R.skip)
   {
      /* a duplicated frame (the core's own) or one the frontend skips */
      R.cfg.video_frame(NULL, R.w ? R.w : w, R.h ? R.h : h, R.last_pitch);
      return;
   }
   if (R.fmt == RETRO_PIXEL_FORMAT_XRGB8888)
   {
      size_t need = (size_t)w * h * sizeof(uint16_t);
      unsigned y, x;
      if (need > R.conv_cap)
      {
         uint16_t *p = (uint16_t *)realloc(R.conv, need);
         if (!p)
            return;
         R.conv = p;
         R.conv_cap = need;
      }
      for (y = 0; y < h; y++)
      {
         const uint32_t *src = (const uint32_t *)((const uint8_t *)data +
                                                   y * pitch);
         uint16_t *dst = R.conv + (size_t)y * w;
         for (x = 0; x < w; x++)
         {
            uint32_t c = src[x];
            dst[x] = (uint16_t)(((c >> 8) & 0xF800) | ((c >> 5) & 0x07E0) |
                                ((c >> 3) & 0x001F));
         }
      }
      data = R.conv;
      pitch = (size_t)w * sizeof(uint16_t);
   }
   R.w = w;
   R.h = h;
   R.last = (const uint16_t *)data;
   R.last_pitch = pitch;
   R.cfg.video_frame((const uint16_t *)data, w, h, pitch);
}

/* --------------------------------------------------------------- audio -- */

static void audio_cb(int16_t left, int16_t right)
{
   if (R.acount + 1 > R.acap)
   {
      size_t cap = R.acap ? R.acap * 2 : 2048;
      int16_t *p = (int16_t *)realloc(R.abuf, cap * 2 * sizeof(int16_t));
      if (!p)
         return;
      R.abuf = p;
      R.acap = cap;
   }
   R.abuf[R.acount * 2] = left;
   R.abuf[R.acount * 2 + 1] = right;
   R.acount++;
}

static size_t audio_batch_cb(const int16_t *data, size_t frames)
{
   if (R.cfg.audio_frames && data && frames)
      R.cfg.audio_frames(data, frames);
   return frames;
}

/* --------------------------------------------------------------- input -- */

static uint32_t g_pad;

static void input_poll_cb(void)
{
   g_pad = R.cfg.input_bitmask ? R.cfg.input_bitmask() : 0;
}

static int16_t input_state_cb(unsigned port, unsigned device, unsigned index,
                              unsigned id)
{
   (void)index;
   if (port != 0 || (device & RETRO_DEVICE_MASK) != RETRO_DEVICE_JOYPAD)
      return 0;
   if (id == RETRO_DEVICE_ID_JOYPAD_MASK)
      return (int16_t)(g_pad & 0xFFFF);
   return id < 16 ? (int16_t)((g_pad >> id) & 1u) : 0;
}

/* ------------------------------------------------------------- battery -- */

static void path_with(char *out, size_t cap, const char *base,
                      const char *suffix)
{
   snprintf(out, cap, "%s%s", base, suffix);
}

static int file_exists(const char *p)
{
   FILE *f = fopen(p, "rb");
   if (!f)
      return 0;
   fclose(f);
   return 1;
}

/* An interrupted replacement leaves the old image as .bak: put it back. */
static void recover_bak(const char *path)
{
   char bak[600];
   path_with(bak, sizeof(bak), path, ".bak");
   if (!file_exists(path) && file_exists(bak))
   {
      if (rename(bak, path) == 0)
         fe_evt("retro_sram_recovered file=%s", path);
   }
}

static int battery_load(void)
{
   FILE *f;
   size_t n;
   R.sram = (uint8_t *)R.api->get_memory_data(RETRO_MEMORY_SAVE_RAM);
   R.sram_size = R.api->get_memory_size(RETRO_MEMORY_SAVE_RAM);
   R.battery_failed = 0;
   if (!R.sram || !R.sram_size || !R.cfg.save_path)
   {
      R.sram = NULL;
      R.sram_size = 0;
      return 0;
   }
   recover_bak(R.cfg.save_path);
   f = fopen(R.cfg.save_path, "rb");
   if (!f)
   {
      if (errno != ENOENT)
      {
         R.battery_failed = 1;
         fe_log("retro_sram_load FAILED; preserving %s", R.cfg.save_path);
         return -1;
      }
      R.sram_crc = crc32_of(R.sram, R.sram_size);
      fe_evt("retro_sram_load size=0 core=%u", (unsigned)R.sram_size);
      return 0;
   }
   n = fread(R.sram, 1, R.sram_size, f);
   if (ferror(f))
   {
      fclose(f);
      R.battery_failed = 1;
      fe_log("retro_sram_load FAILED; preserving %s", R.cfg.save_path);
      return -1;
   }
   fclose(f);
   R.sram_crc = crc32_of(R.sram, R.sram_size);
   fe_evt("retro_sram_load size=%u file=%u crc=%08x", (unsigned)R.sram_size,
          (unsigned)n, R.sram_crc);
   return 0;
}

int fe_retro_sram_flush(int force)
{
   char tmp[600], bak[600];
   uint32_t crc;
   FILE *f;
   int ok, had;
   if (!R.active || !R.sram || !R.sram_size || !R.cfg.save_path)
      return 0;
   if (R.battery_failed)
      return -1;
   /* the core may have reallocated it (a state load): ask again */
   R.sram = (uint8_t *)R.api->get_memory_data(RETRO_MEMORY_SAVE_RAM);
   if (!R.sram)
      return 0;
   crc = crc32_of(R.sram, R.sram_size);
   if (!force && crc == R.sram_crc)
      return 0;
   path_with(tmp, sizeof(tmp), R.cfg.save_path, ".tmp");
   path_with(bak, sizeof(bak), R.cfg.save_path, ".bak");
   f = fopen(tmp, "wb");
   if (!f)
      goto failed;
   ok = fwrite(R.sram, 1, R.sram_size, f) == R.sram_size;
   if (fclose(f) != 0)
      ok = 0;
   if (!ok)
   {
      remove(tmp);
      goto failed;
   }
   had = file_exists(R.cfg.save_path);
   if (had)
   {
      remove(bak);
      if (rename(R.cfg.save_path, bak) != 0)
      {
         remove(tmp);
         goto failed;
      }
   }
   if (rename(tmp, R.cfg.save_path) != 0)
   {
      if (had)
         (void)rename(bak, R.cfg.save_path);
      remove(tmp);
      goto failed;
   }
   if (had)
      remove(bak);
   R.sram_crc = crc;
   fe_evt("retro_sram_write size=%u crc=%08x", (unsigned)R.sram_size, crc);
   return 0;
failed:
   R.save_fails++;
   fe_evt("retro_sram_write FAILED file=%s", R.cfg.save_path);
   return -1;
}

void fe_retro_sram_service(unsigned frame)
{
   if (R.active && R.sram && (frame % 300u) == 0)
      (void)fe_retro_sram_flush(0);
}

int fe_retro_battery_failed(void) { return R.battery_failed; }
unsigned fe_retro_save_failures(void) { return R.save_fails; }

/* ---------------------------------------------------------------- boot -- */

static void dir_of(char *out, size_t cap, const char *path)
{
   const char *slash = strrchr(path, '/');
   size_t n = slash ? (size_t)(slash - path) : 0;
   if (n >= cap)
      n = cap - 1;
   memcpy(out, path, n);
   out[n] = '\0';
}

int fe_retro_boot(const fe_retro_config *cfg)
{
   struct retro_game_info game;
   struct retro_system_av_info av;
   struct retro_system_info si;
   FILE *f;
   long len;
   uint8_t *rom = NULL;
   int ci;

   if (!cfg || !cfg->rom_path)
      return -1;
   ci = core_index(cfg->console);
   if (ci < 0 || !FR_LINKED || R.active)
      return -1;

   memset(&R, 0, sizeof(R));
   R.ci = ci;
   R.cfg = *cfg;
   R.api = &g_api[ci];
   R.fmt = RETRO_PIXEL_FORMAT_RGB565;
   snprintf(R.sysdir, sizeof(R.sysdir), "%s",
            cfg->system_dir ? cfg->system_dir : ".");
   dir_of(R.savedir, sizeof(R.savedir), cfg->save_path ? cfg->save_path
                                                       : cfg->rom_path);
   if (cfg->boot_status)
      cfg->boot_status("Preparing emulator");

   /* the whole game in memory: the cores accept it whether they ask for the
    * path or not, and a Memory Stick read happens once, here */
   f = fopen(cfg->rom_path, "rb");
   if (!f)
   {
      fe_log("retro: cannot open %s", cfg->rom_path);
      return -1;
   }
   fseek(f, 0, SEEK_END);
   len = ftell(f);
   fseek(f, 0, SEEK_SET);
   if (len <= 0 || len > 40L * 1024 * 1024 ||
       !(rom = (uint8_t *)malloc((size_t)len)) ||
       fread(rom, 1, (size_t)len, f) != (size_t)len)
   {
      fe_log("retro: cannot read %s (%ld bytes)", cfg->rom_path, len);
      fclose(f);
      free(rom);
      return -1;
   }
   fclose(f);

   R.api->set_environment(env_cb);
   R.api->set_video_refresh(video_cb);
   R.api->set_audio_sample(audio_cb);
   R.api->set_audio_sample_batch(audio_batch_cb);
   R.api->set_input_poll(input_poll_cb);
   R.api->set_input_state(input_state_cb);
   R.api->init();
   if (cfg->boot_status)
      cfg->boot_status("Loading game");
   memset(&game, 0, sizeof(game));
   game.path = cfg->rom_path;
   game.data = rom;
   game.size = (size_t)len;
   if (!R.api->load_game(&game))
   {
      fe_log("retro[%s]: load_game FAILED for %s", R.api->name, cfg->rom_path);
      R.api->deinit();
      free(rom);
      memset(&R, 0, sizeof(R));
      return -1;
   }
   free(rom);
   R.active = 1;

   R.api->get_system_av_info(&av);
   R.w = av.geometry.base_width;
   R.h = av.geometry.base_height;
   R.rate = (unsigned)(av.timing.sample_rate + 0.5);
   R.fps = av.timing.fps > 1.0 ? av.timing.fps : 60.0;
   if (R.rate < 8000)
      R.rate = 32000;
   (void)si;
   R.api->set_controller_port_device(0, RETRO_DEVICE_JOYPAD);
   fe_evt("retro_boot core=%s console=%d w=%u h=%u rate=%u fps=%.4f "
          "fmt=%d size=%ld", R.api->name, (int)cfg->console, R.w, R.h, R.rate,
          R.fps, (int)R.fmt, len);
   if (cfg->boot_status)
      cfg->boot_status("Loading save");
   if (battery_load() != 0)
   {
      /* keep running without saving: the file is left untouched */
      fe_log("retro: running without a battery save");
   }
   return 0;
}

int fe_retro_active(void) { return R.active; }

void fe_retro_shutdown(void)
{
   if (!R.active)
      return;
   (void)fe_retro_sram_flush(0);
   R.api->unload_game();
   R.api->deinit();
   free(R.conv);
   free(R.abuf);
   memset(&R, 0, sizeof(R));
}

/* ----------------------------------------------------------------- run -- */

void fe_retro_run_frame(int skip)
{
   if (!R.active)
      return;
   R.skip = skip;
   R.acount = 0;
   R.api->run();
   if (R.acount && R.cfg.audio_frames)
      R.cfg.audio_frames(R.abuf, R.acount);
   R.acount = 0;
   R.skip = 0;
}

void fe_retro_reset(void)
{
   if (R.active)
      R.api->reset();
}

unsigned fe_retro_sample_rate(void) { return R.active ? R.rate : 0; }
unsigned fe_retro_fps_x1000(void)
{
   return R.active ? (unsigned)(R.fps * 1000.0 + 0.5) : 0;
}
unsigned fe_retro_width(void) { return R.w; }
unsigned fe_retro_height(void) { return R.h; }

const uint16_t *fe_retro_last_frame(size_t *pitch)
{
   if (pitch)
      *pitch = R.last_pitch;
   return R.last;
}

/* -------------------------------------------------------------- states -- */

static void put32(uint8_t *p, uint32_t v)
{
   p[0] = (uint8_t)v;
   p[1] = (uint8_t)(v >> 8);
   p[2] = (uint8_t)(v >> 16);
   p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p)
{
   return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
          (uint32_t)p[3] << 24;
}

size_t fe_retro_state_size(void)
{
   size_t n;
   if (!R.active)
      return 0;
   n = R.api->serialize_size();
   return n ? n + FR_HEADER : 0;
}

long fe_retro_state_save(void *buf, size_t cap)
{
   uint8_t *p = (uint8_t *)buf;
   size_t n = fe_retro_state_size();
   if (!n || !buf || cap < n)
      return -1;
   if (!R.api->serialize(p + FR_HEADER, n - FR_HEADER))
      return -1;
   memcpy(p, "RTRO", 4);
   put32(p + 4, FR_STATE_VERSION);
   put32(p + 8, (uint32_t)R.cfg.console);
   put32(p + 12, (uint32_t)(n - FR_HEADER));
   return (long)n;
}

int fe_retro_state_load(const void *buf, size_t n)
{
   const uint8_t *p = (const uint8_t *)buf;
   if (!R.active || !buf || n <= FR_HEADER || memcmp(p, "RTRO", 4) != 0 ||
       get32(p + 4) != FR_STATE_VERSION ||
       get32(p + 8) != (uint32_t)R.cfg.console ||
       get32(p + 12) != n - FR_HEADER ||
       n - FR_HEADER != R.api->serialize_size())
      return -1;
   return R.api->unserialize(p + FR_HEADER, n - FR_HEADER) ? 0 : -1;
}

uint32_t fe_retro_ram_read(uint32_t addr, uint8_t *out, uint32_t len)
{
   const uint8_t *ram;
   size_t size;
   uint32_t i;
   if (!R.active || !out)
      return 0;
   ram = (const uint8_t *)R.api->get_memory_data(RETRO_MEMORY_SYSTEM_RAM);
   size = R.api->get_memory_size(RETRO_MEMORY_SYSTEM_RAM);
   for (i = 0; i < len; i++)
      out[i] = (ram && addr + i < size) ? ram[addr + i] : 0;
   return len;
}
