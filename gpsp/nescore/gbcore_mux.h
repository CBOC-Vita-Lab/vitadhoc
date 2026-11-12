/* Copyright (c) 2026 CBOC-Vita contributors.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * fe_host.c's Game Boy path, reused for the NES.  Included by fe_host.c only,
 * after gbcore.h: every gbcore_* call it makes becomes a gbmux_* call, which
 * goes to the NES core (nescore.h) when the machine is the NES one and to the
 * real gbcore_* otherwise.  Nothing else sees the macros, and the GB link
 * code (fe_gblink.c, gbcore_dual.c) keeps calling gbcore_* directly.
 *
 * With no NES game running every function is one pointer compare and the
 * real call: the GBA and GB paths behave exactly as before. */
#ifndef GBADHOC_GBCORE_MUX_H
#define GBADHOC_GBCORE_MUX_H

#include "../gbcore/gbcore.h"

#ifdef __cplusplus
extern "C" {
#endif

int gbmux_is_nes(const gbcore_t *core);
const char *gbmux_nes_error(void);
void gbmux_nes_system_dir(const char *dir);
/* The NES core's own numbers (fps, region) for the machine, if it is one. */
double gbmux_fps(const gbcore_t *core);
/* The running NES game's frame rate (60.0988 NTSC, 50.007 PAL), 0 with none. */
double gbmux_nes_fps(void);
/* A cheat list for the NES core: codes joined by '+'. */
int gbmux_nes_cheats(const char *codes);
int gbmux_nes_save_dirty(const gbcore_t *core);

/* The WonderSwan machine (wscore.h), through the same path. */
int gbmux_is_ws(const gbcore_t *core);
/* the console the machine is: FE_CONSOLE_NES / _WSWAN, else -1 (a GB one) */
int gbmux_other_console(const gbcore_t *core);
/* the error of the last NES or WonderSwan create */
const char *gbmux_error(void);
/* Cheat codes for the NES or WonderSwan machine (joined by '+'). */
int gbmux_cheats_text(const char *codes);
/* 1 when the machine's own battery data changed (NES and WonderSwan) */
int gbmux_save_dirty(const gbcore_t *core);

gbcore_t *gbmux_create(const char *rom_path, const void *rom_data,
                       size_t rom_size, fe_console_t console,
                       unsigned audio_rate, const gbcore_callbacks_t *cb);
int gbmux_run_frame(gbcore_t *core, uint16_t buttons);
void gbmux_set_skip_render(gbcore_t *core, int skip);
void gbmux_shutdown(gbcore_t *core);
unsigned gbmux_sample_rate(const gbcore_t *core);
unsigned gbmux_frame_width(const gbcore_t *core);
unsigned gbmux_frame_height(const gbcore_t *core);
int gbmux_rom_title(gbcore_t *core, char *dst, size_t cap);
size_t gbmux_save_ram_size(gbcore_t *core);
size_t gbmux_cart_ram_size(gbcore_t *core);
int gbmux_save_ram_read(gbcore_t *core, void *dst, size_t cap);
int gbmux_save_ram_write(gbcore_t *core, const void *src, size_t size);
size_t gbmux_state_size(gbcore_t *core);
long gbmux_state_save(gbcore_t *core, void *dst, size_t cap);
int gbmux_state_load(gbcore_t *core, const void *src, size_t size);
int gbmux_has_tilt(gbcore_t *core);
unsigned gbmux_sgb_border(gbcore_t *core, const uint16_t **pixels);
size_t gbmux_sgb_border_save(gbcore_t *core, void *buf, size_t cap);
int gbmux_sgb_border_load(gbcore_t *core, const void *buf, size_t n);
void gbmux_set_tilt(gbcore_t *core, uint32_t x_y);
uint32_t gbmux_ra_read(gbcore_t *core, uint32_t addr, uint8_t *out, uint32_t len);
int gbmux_peek(gbcore_t *core, uint16_t addr, void *out, unsigned len);
int gbmux_poke(gbcore_t *core, uint16_t addr, const void *data, unsigned len);
int gbmux_cheats_set(gbcore_t *core, const gbcore_cheat_t *codes, unsigned count);

#ifdef GBADHOC_GBMUX
#define gbcore_create gbmux_create
#define gbcore_run_frame gbmux_run_frame
#define gbcore_set_skip_render gbmux_set_skip_render
#define gbcore_shutdown gbmux_shutdown
#define gbcore_sample_rate gbmux_sample_rate
#define gbcore_frame_width gbmux_frame_width
#define gbcore_frame_height gbmux_frame_height
#define gbcore_rom_title gbmux_rom_title
#define gbcore_save_ram_size gbmux_save_ram_size
#define gbcore_cart_ram_size gbmux_cart_ram_size
#define gbcore_save_ram_read gbmux_save_ram_read
#define gbcore_save_ram_write gbmux_save_ram_write
#define gbcore_state_size gbmux_state_size
#define gbcore_state_save gbmux_state_save
#define gbcore_state_load gbmux_state_load
#define gbcore_has_tilt gbmux_has_tilt
#define gbcore_sgb_border gbmux_sgb_border
#define gbcore_sgb_border_save gbmux_sgb_border_save
#define gbcore_sgb_border_load gbmux_sgb_border_load
#define gbcore_set_tilt gbmux_set_tilt
#define gbcore_ra_read gbmux_ra_read
#define gbcore_peek gbmux_peek
#define gbcore_poke gbmux_poke
#define gbcore_cheats_set gbmux_cheats_set
#endif

#ifdef __cplusplus
}
#endif
#endif
