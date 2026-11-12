/* Copyright (c) 2026 CBOC-Vita contributors.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Frontend-neutral NES / Famicom execution API, modelled on gbcore.h.
 * Implementation: FCEUmm (nes/core, GPL-2.0-or-later) through nes/frontend/
 * host.{h,c} (nes_host_*), a relocatable object whose only globals are
 * nes_host_*, plus this adapter (nescore.c; gpsp/vita/nescore.mk).  A single global machine: one nescore_t at a time.
 *
 * The frontend (fe_host.c) reaches it through gbcore_mux.h, which routes the
 * gbcore_* calls the Game Boy path makes to these when the console is the
 * NES, so a NES game gets the same state, rewind, fast-forward, Continue and
 * battery-save machinery as a Game Boy game.
 */
#ifndef GBADHOC_NESCORE_H
#define GBADHOC_NESCORE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nescore nescore_t;

enum {
  NESCORE_BUTTON_A = 1u << 0, NESCORE_BUTTON_B = 1u << 1,
  NESCORE_BUTTON_SELECT = 1u << 2, NESCORE_BUTTON_START = 1u << 3,
  NESCORE_BUTTON_UP = 1u << 4, NESCORE_BUTTON_DOWN = 1u << 5,
  NESCORE_BUTTON_LEFT = 1u << 6, NESCORE_BUTTON_RIGHT = 1u << 7
};

typedef struct nescore_video_frame {
  const uint16_t *pixels;     /* RGB565, valid until the next run_frame */
  unsigned width, height, pitch_bytes;
} nescore_video_frame_t;

typedef struct nescore_callbacks {
  void *userdata;
  void (*video)(void *userdata, const nescore_video_frame_t *frame);
  void (*audio_batch)(void *userdata, const int16_t *interleaved, size_t frames);
} nescore_callbacks_t;

/* Folder that may hold disksys.rom (never shipped), set before create. */
void nescore_set_system_dir(const char *dir);
/* Loads the cartridge; the battery RAM is read from srm_path when it exists
 * (NULL: none).  NULL on failure, the reason in nescore_error(). */
nescore_t *nescore_create(const char *rom_path, const char *srm_path,
                          const nescore_callbacks_t *callbacks);
const char *nescore_error(void);
int nescore_run_frame(nescore_t *core, unsigned buttons);
void nescore_set_skip_render(nescore_t *core, int skip);
void nescore_shutdown(nescore_t *core);

unsigned nescore_sample_rate(const nescore_t *core);
unsigned nescore_frame_width(const nescore_t *core);
unsigned nescore_frame_height(const nescore_t *core);
double nescore_fps(const nescore_t *core);
int nescore_rom_title(nescore_t *core, char *destination, size_t capacity);
uint32_t nescore_rom_crc(const nescore_t *core);

size_t nescore_save_ram_size(nescore_t *core);
int nescore_save_ram_read(nescore_t *core, void *destination, size_t capacity);
/* 0 when the bytes already are the RAM (the load did it); else -1 for now. */
int nescore_save_ram_write(nescore_t *core, const void *source, size_t size);

size_t nescore_state_size(nescore_t *core);
long nescore_state_save(nescore_t *core, void *destination, size_t capacity);
int nescore_state_load(nescore_t *core, const void *source, size_t size);

/* RetroAchievements console 7: the CPU's address space ($0000-$07FF,
 * $6000-$7FFF); returns the bytes copied. */
uint32_t nescore_ra_read(nescore_t *core, uint32_t addr, uint8_t *out, uint32_t len);
/* Game Genie / raw "AAAA:VV" codes joined by + , or space; replaces the list
 * (NULL or "" clears it).  Returns how many were accepted. */
int nescore_cheats_set(nescore_t *core, const char *codes);
/* 1 when the battery RAM changed since it was loaded or last written. */
int nescore_save_ram_dirty(nescore_t *core);
void nescore_set_fourscore(nescore_t *core, int mode);

/* CPU-visible RAM ($0000-$1FFF, $6000-$7FFF) for RetroAchievements; -1 if none. */
int nescore_peek(nescore_t *core, unsigned address);
void nescore_reset(nescore_t *core);

#ifdef __cplusplus
}
#endif
#endif
