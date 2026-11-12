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
/* Safe save: a verified backup of the existing file first (once per session), then
 * path.tmp, read back, renamed over the file. Returns 0 on success; on failure the
 * existing file is untouched (codes: see snes_host_save_error). */
int snes_host_sram_save(void);
/* For a save written on another thread: snes_host_sram_take copies the SRAM into
 * `out` (cap bytes) and counts that content as saved; snes_host_sram_write writes
 * such a copy (any thread, one at a time); after a failed write the main thread
 * calls snes_host_sram_forget so the SRAM counts as unsaved again. */
size_t snes_host_sram_take(uint8_t *out, size_t cap);
int snes_host_sram_write(const uint8_t *data, size_t n);
void snes_host_sram_forget(void);

/* Extra folder for the first copy of an existing save before we overwrite it
 * (the Vita app uses ux0:data/GBAdhoc/backups/snes). The folder must exist. */
void snes_host_set_backup_dir(const char *dir);

/* Why the last battery save failed: 0 when it did not (or none was tried), else the
 * return code of the failed snes_host_sram_save / snes_host_sram_write (see host.cpp:
 * 1 temp file, 2 write, 3 no verified backup, 4 rename, 5 read-back, 6 unreadable save)
 * and, in `text` (cap bytes), the real reason. Any thread. The existing file is left
 * untouched on every failure; the caller retries later. */
int snes_host_save_error(char *text, size_t cap);
/* Where the host reports save problems (one line per call); default is stderr. */
void snes_host_set_log(void (*fn)(const char *line));

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
/* Super FX carts: is there one, and its speed in percent of the hardware's (read
 * by the game thread only, so it can be changed between frames without a sync). */
int snes_host_has_superfx(void);
void snes_host_set_superfx_clock(int percent);
/* Microseconds the GSU has run since the last call (needs a clock: `now_us`). */
void snes_host_superfx_timer(uint64_t (*now_us)(void));
uint64_t snes_host_superfx_take_us(void);
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

/* ---- Cheats (all systems share the shape of this API) ------------------------------------
 * Codes: Game Genie XXXX-XXXX, Pro Action Replay AAAAAAVV (8 hex), raw AAAAAA:VV or AAAAAA=VV
 * (address 1-6 hex digits, value 1-2), and AAAAAA=CC?VV (poke VV only while the byte is CC).
 * Several cells in one code: separate them with '+', ',', ';', a blank or a line end. Case and
 * spacing do not matter; spaces around ':' or '=' are not allowed. Address 0 is refused.
 *
 * The codes of a game live in a list (up to SNES_CHEATS_MAX); a code is poked into memory at the end
 * of every frame while it is on, on the emulation thread (RAM writes at a frame boundary, so the APU
 * and PPU workers stay bit-identical to the single thread), a Game Genie style ROM patch is put back
 * when the code is switched off. Reset and state load switch the codes off, do their work and arm them
 * again, so a state's own bytes are never mixed with stale ones. With no code on, the emulation is
 * bit-identical to a build without cheats (checked by hash on the 15 owner games, threads on and off).
 *
 * Persistence: after snes_host_cheats_load(root) every change is written to
 * <root>/cheats/<ROM file name without extension>.cht in RetroArch's format
 *     cheats = 2
 *     cheat0_desc = "Infinite lives"
 *     cheat0_code = "7E0DBE=63"
 *     cheat0_enable = true
 * (a code of several cells joined by '+'; keys we do not know are kept). Writing is the safe writer:
 * temp file, read back, rename, a verified backup of the old file before the first overwrite. If a
 * write fails the change still holds in memory, snes_host_cheat_error() says why and the next change
 * (or snes_host_cheats_save()) retries; a file that exists but cannot be read is never overwritten.
 * Call everything from the thread that runs the frames, between frames. */
#define SNES_CHEATS_MAX 512
#define SNES_CHEAT_NAME_CAP 64
#define SNES_CHEAT_CODE_CAP 512
enum {
	SNES_CHEAT_OK = 0,
	SNES_CHEAT_E_FULL = -1,
	SNES_CHEAT_E_FORMAT = -2,       /* not a code */
	SNES_CHEAT_E_UNSUPPORTED = -3,  /* in the list but not a code this engine can run (cannot be switched on) */
	SNES_CHEAT_E_TOO_LONG = -4,     /* more than 16 cells, or a text over the cap */
	SNES_CHEAT_E_IO = -5,           /* the .cht could not be read or written */
	SNES_CHEAT_E_RANGE = -6,        /* bad index, or address 0 */
	SNES_CHEAT_E_NOROM = -7
};
typedef struct {
	char label[SNES_CHEAT_NAME_CAP];
	char code[SNES_CHEAT_CODE_CAP];
	int enabled;
	int usable;                     /* 0: the text is not a code we can run (kept so the file round-trips) */
} snes_cheat_info;
/* Needs a loaded ROM. Reads the game's .cht (a missing file is an empty list) and arms the entries that
 * are on. Returns the number of entries or a SNES_CHEAT_E_*. Loading a ROM forgets the list. */
int snes_host_cheats_load(const char *root);
/* Checks a code without adding it. Returns the number of cells (> 0) or SNES_CHEAT_E_*. If canon is given
 * it receives the normalised text ("7E0DBE=63", "C2A6-3D6D", upper case, cells joined by '+'). */
int snes_host_cheat_validate(const char *code, char *canon, size_t cap);
/* The same code as plain address=value cells, "7E0DBE=63+..." (a Game Genie or Action Replay cell decoded); for tests. */
int snes_host_cheat_decode(const char *code, char *raw, size_t cap);
/* Work RAM byte at offset 0..0x1FFFF (address $7E0000 + offset), or -1; for tests. */
int snes_host_peek_wram(uint32_t offset);
/* Adds a code (switched on). label may be NULL/"" ("Cheat N"). Returns the new index or SNES_CHEAT_E_*. */
int snes_host_cheat_add(const char *code, const char *label);
int snes_host_cheat_enable(int index, int on);
int snes_host_cheat_remove(int index);
int snes_host_cheat_count(void);
int snes_host_cheat_get(int index, snes_cheat_info *out);
void snes_host_cheat_disable_all(void);
/* Removes every code (memory is restored) and writes the empty list. */
void snes_host_cheat_reset(void);
/* Writes the list now (a retry after a failed write). 0 or SNES_CHEAT_E_IO. */
int snes_host_cheats_save(void);
/* 1 and the reason in text when the last write of the list failed, else 0. */
int snes_host_cheat_error(char *text, size_t cap);

const char *snes_host_rom_title(void);
/* One line about the loaded cartridge: map, size, chips, SRAM. */
const char *snes_host_rom_info(void);

#endif
