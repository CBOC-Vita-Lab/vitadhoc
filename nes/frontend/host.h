/*
 * NES host: the platform independent layer between FCEUmm (nes/core) and a frontend. It is built into the
 * relocatable object nescore.o (see nes/frontend/Makefile), which exports exactly the nes_host_* functions below
 * and nothing else, so it can be linked into a program that has its own globals (gpSP, TGB Dual).
 *
 * Rules of the API
 *  - Plain C, extern "C" from C++. No file access: ROMs, the BIOS, battery RAM and states are memory buffers
 *    owned by the caller (host_files.h is an optional file layer on top, used by the Linux twin).
 *  - ONE instance per process (FCEUmm keeps its machine in globals), ONE thread: call everything from the thread
 *    that runs the frames. No call blocks or allocates after nes_host_load_mem except where said.
 *  - Allocation: the core and this file use malloc/free (the console image, ~200-300 KB for a mid size game;
 *    FDS and big mappers more) plus about 130 KB of static buffers (see INTEGRATION.md for the numbers).
 *
 * Copyright CBOC-Vita. MIT licence (see nes/frontend/LICENSE).
 * Links against FCEUmm (nes/core, GPL-2.0-or-later): a program containing nescore.o is distributed under the GPL.
 */
#ifndef NES_HOST_H
#define NES_HOST_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Joypad bits (the order of the controller's shift register: A first). */
enum {
	NES_A = 1 << 0, NES_B = 1 << 1, NES_SELECT = 1 << 2, NES_START = 1 << 3,
	NES_UP = 1 << 4, NES_DOWN = 1 << 5, NES_LEFT = 1 << 6, NES_RIGHT = 1 << 7
};
#define NES_AUDIO_RATE 48000   /* default audio rate; nes_host_set_audio_rate changes it (8000..96000) */
#define NES_WIDTH 256
#define NES_HEIGHT 240
enum { NES_REGION_AUTO = 0, NES_REGION_NTSC, NES_REGION_PAL, NES_REGION_DENDY };
enum { NES_4P_AUTO = 0, NES_4P_ON, NES_4P_OFF };

/* ---- lifetime -------------------------------------------------------------------------------------------- */

/* Once, before anything else. Returns 0 on success. Starts with: audio rate 48000 (or the rate set before),
 * sprite limit removed, Four Score automatic, region automatic, power-on RAM $FF. */
int nes_host_init(void);
void nes_host_shutdown(void);

/* The Famicom Disk System BIOS (disksys.rom, 8192 bytes), copied here and used by every FDS load afterwards.
 * Never shipped with the player: the user supplies it. NULL / wrong size forgets it. */
void nes_host_set_fds_bios(const uint8_t *bios, size_t n);

/* 1 when the bytes look like a Disk System image (the 16 byte "FDS\x1a" header or the raw "*NINTENDO-HVC*" block). */
int nes_host_is_fds_image(const uint8_t *data, size_t n);

/* Load a cartridge from memory: iNES, NES 2.0 (also with a 512 byte trainer), UNIF, or an FDS disk image (with or
 * without the 16 byte header; needs nes_host_set_fds_bios first). The core reads the header itself; nothing is
 * stripped here. `data` is copied by the core: the caller may free it on return. `name` is the file name (title,
 * region tags such as "(E)"); may be NULL. Any previous game is unloaded first. Returns 0, else non-zero with a
 * short reason in err (1 not initialised, 2 unreadable, 3 not a valid game / NSF, 4 BIOS missing). The console is
 * powered on at the end of the load. Battery RAM is NOT loaded here: call nes_host_sram_load afterwards. */
int nes_host_load_mem(const uint8_t *data, size_t n, const char *name, char *err, size_t errlen);
void nes_host_unload(void);   /* also clears the cheats */

/* ---- frames ---------------------------------------------------------------------------------------------- */

/* Joypads: port 0..3 = pad 1..4; pads 3 and 4 reach the game only through the Four Score / Famicom 4 player
 * adapter. Up+Down and Left+Right together cancel, as on the hardware. Applies to the next frame. */
void nes_host_set_pad(int port, unsigned mask);
/* Light gun. nes_host_zapper_active() is 1 when the loaded game uses a Zapper (the core's game database says so,
 * or the "zapper" option forces it on port 2): then show a touch aim. x,y = the picture pixel under the finger
 * (0..255, 0..239 of the 256x240 frame from nes_host_video, NOT the cropped one); anything outside is "aimed off the
 * screen". trigger 1 = pulled (a game sees the pull for about 5 frames, a new pull needs a release in between).
 * The gun senses the picture the core drew, so it does not work with nes_host_set_render(0); the core draws no
 * cross hair: draw your own. Light is the brightness of the pixel(s) within 6 pixels (a rounded target). */
/* "No flicker" (option no_sprite_limit) is switched off by itself for the few games that hide parts of the picture
 * behind the 8 sprite limit (a built-in CRC table; the loaded game's state is identical either way, only the picture
 * differs). nes_host_sprite_limit_exception() = 1 when the loaded game is in the table, so a menu can say so;
 * nes_host_sprite_exception_add(crc) adds a CRC-32 (PRG+CHR, as nes_host_rom_crc) at run time; 0 = added. */
int nes_host_sprite_limit_exception(void);
int nes_host_sprite_exception_add(uint32_t crc);

void nes_host_set_zapper(int x, int y, int trigger);
int nes_host_zapper_active(void);
/* Arkanoid "Vaus" paddle (NES port 2 or the Famicom expansion port, by the database or the "paddle" option):
 * x = 0..255 screen column of the finger (the core maps it to the paddle's range), button = fire. */
void nes_host_set_paddle(int x, int button);
int nes_host_paddle_active(void);
void nes_host_set_fourscore(int mode);      /* NES_4P_AUTO (the games known to need it), ON, OFF; effective at once */
int nes_host_fourscore_mode(void);
int nes_host_fourscore_active(void);        /* 1 when pads 3/4 are connected */

/* Run one frame (until the next vertical blank). Cheats are applied inside. */
void nes_host_run_frame(void);
void nes_host_reset(void);                  /* the console's reset button (the game stays loaded) */
/* Render flag: 0 = the next frames skip drawing (game logic, sound and the state are identical; the video buffer
 * keeps the last drawn picture). For fast forward and rewind. Default 1. */
void nes_host_set_render(int on);

/* ---- video ----------------------------------------------------------------------------------------------- */

/* The last drawn frame, 256 x 240 RGB565 (emphasis bits and the palette already applied), pitch in bytes. Valid
 * until the next nes_host_run_frame. A television shows rows 8..231 (NTSC); see nes_host_overscan. */
const uint16_t *nes_host_video(int *width, int *height, int *pitch_bytes);
/* Number of rows an NTSC television hides at the top and at the bottom (8 and 8). PAL sets show all 240. */
void nes_host_overscan(int *top, int *bottom);
void nes_host_visible_rows(int *first, int *last);   /* 8 and 231 */
/* For a GPU palette path: the colour table (RGB565; entries 0..63 are the base palette, then 8 emphasis banks of 64
 * at 256 + (emphasis << 6)) and the last frame's raw index bytes (256 x 240) with each row's emphasis bits at
 * emph[y * 256]. Pointers stay valid for the life of the game. */
const uint16_t *nes_host_palette(int *entries);
int nes_host_video_indexed(const uint8_t **indices, const uint8_t **emphasis);

/* ---- audio ----------------------------------------------------------------------------------------------- */

/* The core synthesises directly at the rate asked for, mono, and this layer doubles it to stereo: no resampler is
 * involved. Default 48000. The main app's mixer accepts any input rate from 8000 to 65536 (audio_start), so
 * 32768 or 48000 both work; call before the first frame (changing it drops queued audio). Returns 0, else 1. */
int nes_host_set_audio_rate(unsigned hz);
unsigned nes_host_audio_rate(void);
/* Pull up to max_frames stereo frames (L,R int16 pairs) made by the frames run so far. Returns the number written.
 * About 800 frames per NTSC frame at 48000 Hz. */
int nes_host_audio_take(int16_t *dst, int max_frames);
void nes_host_set_audio_enabled(int on);    /* 0: silence (the frames still run) */

/* ---- battery RAM (the caller saves it) ------------------------------------------------------------------- */

/* The cartridge's battery backed RAM, as RetroArch's FCEUmm exposes it (RETRO_MEMORY_SAVE_RAM): 8 KiB for most
 * boards, the board's own size otherwise; for a Disk System game the whole disk image (what the game wrote to
 * the disk). Size 0 = the game has none. Valid while the game is loaded. */
size_t nes_host_sram_size(void);
uint8_t *nes_host_sram_data(void);
/* Copies a saved image in (min(n, size) bytes; call right after load) and counts it as saved. Returns the bytes used. */
size_t nes_host_sram_load(const uint8_t *data, size_t n);
/* 1 when the RAM differs from the last load / nes_host_sram_mark_saved. Cheap (a compare of the RAM). */
int nes_host_sram_dirty(void);
void nes_host_sram_mark_saved(void);        /* call after the caller wrote the data out */
void nes_host_sram_mark_unsaved(void);      /* a write failed: the RAM counts as changed again */

/* ---- save states ----------------------------------------------------------------------------------------- */

/* The core's own format ("FCS"). The size is fixed for a loaded game: nes_host_state_size() is the number of bytes
 * every save returns (the real state, zero padded; measured at load with 1 KiB of headroom), so a rewind ring can
 * use equal slots. 0 when no game is loaded. */
size_t nes_host_state_size(void);
size_t nes_host_state_save_mem(uint8_t *buf, size_t cap);   /* needs cap >= state_size; returns the size, 0 on failure */
int nes_host_state_load_mem(const uint8_t *buf, size_t n);  /* 0 on success; 1 no game, 3 not a state / refused */

/* ---- memory for achievements and tools ------------------------------------------------------------------- */

/* RetroAchievements console 7 (NES): the address space rcheevos expects is the CPU's own, with two windows:
 * $0000-$07FF system RAM and $6000-$7FFF cartridge RAM. Copies n bytes starting at addr into out; returns the number
 * of bytes that were inside a window (a request is cut at the end of its window; 0 = unmapped). Reads have no side
 * effects. nes_host_ram() is the 2 KiB system RAM itself for fast polling (valid while a game is loaded). */
int nes_host_ra_read(unsigned addr, uint8_t *out, unsigned n);
const uint8_t *nes_host_ram(size_t *size);
int nes_host_peek(unsigned addr);   /* one byte through the CPU's read map ($6000-$7FFF, RAM), -1 when no game */

/* ---- cheats ---------------------------------------------------------------------------------------------- */

/* A code string; several codes may be joined by + , ; . _ or a space. Accepted forms: Game Genie (6 or 8 letters,
 * which patch ROM reads), raw "AAAA:VV" and "AAAA?CC:VV" (compare value) RAM pokes, and Pro Action Replay
 * "AAAAVV" style codes the core decodes. RAM pokes are written every frame inside nes_host_run_frame. Returns how
 * many codes were accepted (0 = none valid). Cheats are dropped at unload. */
int nes_host_cheat_add(const char *codes);
void nes_host_cheat_reset(void);
int nes_host_cheat_count(void);

/* ---- console state and options --------------------------------------------------------------------------- */

/* Core options by name; returns 0 when the name is known:
 *   region 0..3          auto (from the header / database), NTSC, PAL, Dendy
 *   no_sprite_limit 0/1  draw all sprites of a line (no flicker; the hardware's 8 per line limit and the overflow
 *                        flag the game reads are unchanged)
 *   fourscore 0..2       NES_4P_*
 *   zapper 0..2          0 = the games the database lists, 1 = force a Zapper on port 2, 2 = never (next load)
 *   paddle 0..2          the same for the Arkanoid paddle
 *   ram_init 0..2        power-on RAM: 0 = $FF (default), 1 = $00, 2 = pseudo random (takes effect at the next load)
 *   sound_quality 0..2   low (default, cheapest), high, very high
 *   lowpass 0/1          */
int nes_host_set_option(const char *name, int value);
int nes_host_is_pal(void);          /* 1 when the console runs at 50 Hz (PAL or Dendy) */
int nes_host_region(void);          /* NES_REGION_NTSC / PAL / DENDY in force */
int nes_host_region_detected(void); /* what the header / database said */
double nes_host_fps(void);          /* 60.0988 or 50.007 */
const char *nes_host_rom_title(void);
/* One line about the loaded cartridge: format, mapper, sizes, battery, region, CRC. */
const char *nes_host_rom_info(void);
uint32_t nes_host_rom_crc(void);   /* the core's CRC-32 of PRG + CHR (what its database uses), 0 for disks */

/* ---- Famicom Disk System --------------------------------------------------------------------------------- */

int nes_host_is_fds(void);
/* side: 0-based side number, sides: how many, inserted: 1 when the disk is in the drive. */
void nes_host_fds_info(int *side, int *sides, int *inserted);
void nes_host_fds_toggle_disk(void);   /* eject / insert */
void nes_host_fds_next_side(void);     /* select the next side (the disk must be ejected first) */

/* ---- messages -------------------------------------------------------------------------------------------- */

/* The newest message the core wanted to show (disk inserted, BIOS missing...); copied out and cleared. */
int nes_host_take_message(char *out, size_t cap);
/* Optional sink for the core's text output; NULL for none. */
void nes_host_set_log(void (*fn)(const char *line));

#ifdef __cplusplus
}
#endif
#endif
