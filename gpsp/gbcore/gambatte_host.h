/* Copyright (c) 2026 CBOC-Vita contributors.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Gambatte (libgambatte, gbcore/gambatte/; Sindre Aamas and the libretro fork's contributors, GPL-2.0) as the
 * app's "Accurate" Game Boy / Game Boy Color core.  A plain C face for one running machine; the whole of libgambatte
 * is partially linked into one object whose only globals are gambatte_host_* (vita/gambattecore.mk), so none of its
 * C++ names meets the GBA core's or TGB Dual's.  gbcore_mux.c routes fe_host.c's gbcore_* calls here when the
 * running machine is a Gambatte one.
 *
 * Same contracts as gbcore.h: 16-bit RGB565 pixels (libretro order), 160x144, one stereo batch per emulated frame at
 * a rate near the one asked for (the machine makes 2 MiHz; an exact whole-number decimation with a triangle filter,
 * gambatte_host_sample_rate says which), the battery image in TGB Dual's file layout (cartridge RAM sized as SameBoy
 * sized it, then for MBC3 clock carts the 48-byte VBA-M/BGB trailer), save states tagged "GBGM" (TGB's are "GBAT":
 * neither core loads the other's). */
#ifndef GBADHOC_GAMBATTE_HOST_H
#define GBADHOC_GAMBATTE_HOST_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gambatte_host_cb {
  void *userdata;
  void (*video)(void *userdata, const uint16_t *pixels, unsigned width, unsigned height, unsigned pitch_bytes);
  void (*audio_batch)(void *userdata, const int16_t *interleaved, size_t frames);
  /* the serial port, as gbcore_callbacks_t.serial_transfer (internal clock only; no cable partner) */
  int (*serial_transfer)(void *userdata, uint8_t outgoing, int internal_clock, uint8_t *received);
} gambatte_host_cb_t;

/* One cheat, the same seven fields as gbcore_cheat_t (gbcore.h) */
typedef struct gambatte_host_cheat {
  uint8_t rom, type, value, compare, has_compare, pad;
  uint16_t address;
} gambatte_host_cheat_t;

/* 0 = the cartridge is running; -1 = refused (a cartridge type Gambatte does not run: MBC6, MBC7, the Game Boy
 * Camera, TAMA5, MMM01; a bad image; a machine is already running).  cgb_console: 1 = Game Boy Color hardware
 * (colour games in colour), 0 = Game Boy hardware.  rate: the output rate asked for (32768 on the Game Boy path). */
int gambatte_host_create(const uint8_t *rom, size_t size, int cgb_console, unsigned rate, const gambatte_host_cb_t *cb);
void gambatte_host_shutdown(void);
int gambatte_host_running(void);
/* the cartridge header from 0x134 (28 bytes), NULL with no machine */
const uint8_t *gambatte_host_header(void);
int gambatte_host_run_frame(uint16_t gbcore_buttons);
void gambatte_host_set_skip_render(int skip);
void gambatte_host_set_quiet(int quiet);
unsigned gambatte_host_sample_rate(void);
unsigned gambatte_host_frame_width(void);
unsigned gambatte_host_frame_height(void);
uint64_t gambatte_host_frame_number(void);
int gambatte_host_rom_title(char *dst, size_t cap);

/* the battery image (gbcore.h gbcore_save_ram_*) */
size_t gambatte_host_save_ram_size(void);
size_t gambatte_host_cart_ram_size(void);
int gambatte_host_save_ram_read(void *dst, size_t cap);
int gambatte_host_save_ram_write(const void *src, size_t size);

/* save states and the Quick response image (the state plus the frame counter, no audio reset) */
size_t gambatte_host_state_size(void);
long gambatte_host_state_save(void *dst, size_t cap);
int gambatte_host_state_load(const void *src, size_t size);
size_t gambatte_host_ahead_size(void);
long gambatte_host_ahead_save(void *dst, size_t cap);
int gambatte_host_ahead_load(const void *src, size_t size);
/* 1: the image is a TGB Dual state ("GBAT"); 2: it is a Gambatte one ("GBGM"); 0: neither */
int gambatte_host_state_kind(const void *src, size_t size);

/* The Game Boy's four-shade palettes (BG, OBJ0, OBJ1 x 4, RGB565 as drawn); applies to a Game Boy running as a Game
 * Boy, never to Color games.  Kept across machines. */
void gambatte_host_set_dmg_colors(const uint16_t rgb565[12]);
/* Game Boy Color palette entries: 1 = through the Game Boy Color LCD model, 0 = plain 5-to-8-bit expansion */
void gambatte_host_set_color_correction(int on);
void gambatte_host_set_wallclock(time_t (*wallclock)(void));

int gambatte_host_cheats_set(const gambatte_host_cheat_t *codes, unsigned count);
uint32_t gambatte_host_ra_read(uint32_t addr, uint8_t *out, uint32_t len);
int gambatte_host_peek(uint16_t addr, void *out, unsigned len);
int gambatte_host_poke(uint16_t addr, const void *data, unsigned len);
/* B C D E H L A F, for the test harness */
void gambatte_host_regs(uint8_t out[8]);

#ifdef __cplusplus
}
#endif
#endif
