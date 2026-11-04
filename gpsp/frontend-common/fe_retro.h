/* fe_retro.h -- Mega Drive, NES and SNES through unmodified libretro cores.
 *
 * Genesis Plus GX, FCEUmm and Snes9x 2005 are linked as isolated objects
 * (vita/retrocores.mk): every symbol local except md_retro_*, nes_retro_* and
 * snes_retro_*.  This file is the frontend side: the libretro environment,
 * the video / audio / input sinks, the battery file and the save states.
 * fe_host.c calls it for FE_CONSOLE_MD / NES / SNES the way it calls gbcore
 * for the Game Boy; nothing here knows the PSP or the Vita.
 *
 * One retro core runs at a time (fe_retro_boot refuses a second).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef FE_RETRO_H
#define FE_RETRO_H

#include <stddef.h>
#include <stdint.h>
#include "fe_console.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 1 for the consoles this frontend runs through a libretro core. */
int fe_console_is_retro(fe_console_t console);

/* 1 when the core for `console` is linked into this build. */
int fe_retro_available(fe_console_t console);

typedef struct fe_retro_config
{
   fe_console_t console;
   const char *rom_path;       /* the game, read by the core itself          */
   const char *system_dir;     /* where a BIOS would be (none is needed)     */
   const char *save_path;      /* battery RAM: the core's own .srm layout    */
   void (*video_frame)(const uint16_t *rgb565, unsigned w, unsigned h,
                       size_t pitch_bytes);   /* NULL data = repeat the last */
   void (*audio_frames)(const int16_t *stereo_lr, size_t frames);
   uint32_t (*input_bitmask)(void);           /* RETRO_DEVICE_ID_JOYPAD_*    */
   uint64_t (*time_us)(void);
   void (*boot_status)(const char *stage);
} fe_retro_config;

/* Boots the core and loads the game and its battery file.  0 on success. */
int  fe_retro_boot(const fe_retro_config *cfg);
int  fe_retro_active(void);
void fe_retro_shutdown(void);

/* One emulated frame.  `skip` = emulate fully but hand the sink "no new
 * picture" (fast-forward's intermediate frames). */
void fe_retro_run_frame(int skip);
void fe_retro_reset(void);

unsigned fe_retro_sample_rate(void);
/* Frames a second, in thousandths (60098 for the NES), 0 before boot. */
unsigned fe_retro_fps_x1000(void);
unsigned fe_retro_width(void);
unsigned fe_retro_height(void);
const uint16_t *fe_retro_last_frame(size_t *pitch_bytes);

/* Save states.  The image carries a small header (magic, console, core
 * state size) so a state of another console or core build is refused before
 * the core sees it.  _save returns bytes written or -1; _load 0 or -1. */
size_t fe_retro_state_size(void);
long   fe_retro_state_save(void *buf, size_t cap);
int    fe_retro_state_load(const void *buf, size_t n);
/* Largest state any of the cores writes plus the header: the size of the
 * staging buffer a caller needs. */
#define FE_RETRO_STATE_MAX (1536u * 1024u)

/* Battery RAM.  flush(force): write the file when its contents changed since
 * the last write (or always with force).  Atomic: .tmp, then renamed over.
 * service(frame) checks every few hundred frames.  0 on success. */
int  fe_retro_sram_flush(int force);
void fe_retro_sram_service(unsigned frame);
/* 1 once the battery file could not be read or written: the game then runs
 * without saving rather than overwriting what it could not read. */
int  fe_retro_battery_failed(void);
unsigned fe_retro_save_failures(void);

/* Memory view for RetroAchievements / cheats-style readers: the core's
 * system RAM.  Bytes past the end read 0.  Returns bytes written. */
uint32_t fe_retro_ram_read(uint32_t addr, uint8_t *out, uint32_t len);

#ifdef __cplusplus
}
#endif
#endif
