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

/* port 0..4: pad 1, pad 2, and (with the multitap on) pads 3 to 5. */
void snes_host_set_pad(int port, unsigned mask);
/* Super Multitap in controller port 2 (pads 2..5). Off = a plain pad 2. */
void snes_host_set_multitap(int on);
int snes_host_multitap(void);

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

/* Extra folder for the first copy of an existing save before we overwrite it
 * (the Vita app uses ux0:data/GBAdhoc/backups/snes). The folder must exist. */
void snes_host_set_backup_dir(const char *dir);

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

/* Worker threads (phase 2). Switch on with snes_host_set_option("apu_thread", 1) / ("ppu_thread", 1);
 * frame hashes and audio stay identical to the single-thread build. */
/* Vita: never waits for the PPU worker. Returns the newest finished picture (normally the previous
 * frame) or NULL when none exists yet; *serial changes whenever a new picture is available. The data
 * must be copied before the next call. Without the PPU thread it returns the current frame. */
const uint16_t *snes_host_video_nowait(int *width, int *height, int *pitch_bytes, unsigned *serial);
/* Core number (0..2, -1 = any) and Vita priority of the workers, and how many empty polls an idle
 * worker makes before it blocks on a semaphore. Call before enabling the threads. */
void snes_host_thread_config(int apu_core, int apu_prio, int ppu_core, int ppu_prio, int spin);
void snes_host_sync(void); /* wait until the worker threads have caught up with the emulation */
int snes_host_threads_active(void); /* bit 0 APU thread, bit 1 PPU thread */
typedef struct {
	uint64_t apu_busy_ns;  /* time the APU thread spent working (not waiting) */
	uint64_t apu_cmds;     /* work items it ran */
	uint64_t apu_helped;   /* work items the 65816 side ran itself (port reads) */
	uint64_t apu_waits;    /* times the 65816 side waited for the worker to finish a chunk */
	uint64_t apu_wait_ns;  /* total time spent waiting */
	uint64_t ppu_busy_ns;  /* time the PPU thread spent drawing (not waiting) */
	uint64_t ppu_batches;  /* snapshots it drew */
	uint64_t ppu_waits;    /* times the register side waited for it (full ring, picture asked for) */
	uint64_t ppu_wait_ns;
} snes_thread_stats;
void snes_host_thread_stats(snes_thread_stats *st);

const char *snes_host_rom_title(void);
/* One line about the loaded cartridge: map, size, chips, SRAM. */
const char *snes_host_rom_info(void);

#endif
