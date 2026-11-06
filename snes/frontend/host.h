/*
 * SNES host glue: platform independent layer between Snes9x and a frontend.
 * Used by the Linux twin runner and by the Vita app.
 *
 * Copyright CBOC-Vita. MIT licence (see snes/frontend/LICENSE).
 */
#ifndef SNES_HOST_H
#define SNES_HOST_H

#include <stddef.h>
#include <stdint.h>

/* Joypad bits (same order as the hardware auto-read register high/low bytes
 * is not needed; these are just our own flags). */
enum {
	SNES_B = 1 << 0, SNES_Y = 1 << 1, SNES_SELECT = 1 << 2, SNES_START = 1 << 3,
	SNES_UP = 1 << 4, SNES_DOWN = 1 << 5, SNES_LEFT = 1 << 6, SNES_RIGHT = 1 << 7,
	SNES_A = 1 << 8, SNES_X = 1 << 9, SNES_L = 1 << 10, SNES_R = 1 << 11
};

#define SNES_AUDIO_RATE 32040

/* Call once. Returns 0 on success. */
int snes_host_init(void);
void snes_host_shutdown(void);

/* Load a ROM file (.sfc/.smc, 512 byte copier header is removed). On failure
 * returns non-zero and writes a short reason into err. Battery RAM is loaded
 * from srm_path when the file exists. */
int snes_host_load_rom(const char *rom_path, const char *srm_path, char *err, size_t errlen);
void snes_host_unload(void);

void snes_host_set_pad(int port, unsigned mask);

/* Run until the next vertical blank (one frame). */
void snes_host_run_frame(void);
void snes_host_reset(void);

/* Last finished frame: RGB565, pitch in bytes. Valid until the next run_frame. */
const uint16_t *snes_host_video(int *width, int *height, int *pitch_bytes);

/* Pop up to max_frames stereo frames (L,R int16 pairs) of 32040 Hz audio.
 * Returns the number of frames written. */
int snes_host_audio_take(int16_t *dst, int max_frames);
void snes_host_set_audio_enabled(int on);

/* Battery RAM. Size 0 means the game has none. */
size_t snes_host_sram_size(void);
const uint8_t *snes_host_sram_data(void);
/* Returns 1 when the SRAM changed since the last successful snes_host_sram_save. */
int snes_host_sram_dirty(void);
/* Safe save: writes path.tmp, keeps the previous file as path.bak, renames.
 * Returns 0 on success. */
int snes_host_sram_save(void);

/* Save states, same safe write rule. Return 0 on success. */
int snes_host_state_save(const char *path);
int snes_host_state_load(const char *path);

/* Core options by name; returns 0 when the name is known:
 *   block_vram 0/1       block CPU/DMA VRAM access while the screen is drawn (Snes9x default 1)
 *   sprite_tiles N       sprite tiles per line, 34 = hardware, 128 = no flicker
 *   interp 0..4          DSP interpolation: 0 none 1 linear 2 gaussian (hardware) 3 cubic 4 sinc
 *   superfx_clock N      Super FX speed in percent (100 = hardware)
 *   transparency 0/1
 *   echo_buffer 0/1      separate echo buffer (fixes a few games, costs accuracy elsewhere) */
int snes_host_set_option(const char *name, int value);
int snes_host_is_pal(void);
/* When off, the core skips PPU rendering for the coming frames (audio and
 * game logic still run). */
void snes_host_set_render(int on);

const char *snes_host_rom_title(void);
/* One line about the loaded cartridge: map, size, chips, SRAM. */
const char *snes_host_rom_info(void);

#endif
