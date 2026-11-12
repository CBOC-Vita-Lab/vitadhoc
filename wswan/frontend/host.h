/*
 * WonderSwan host: the platform independent layer between the Beetle WonderSwan core (wswan/core, Mednafen's
 * WonderSwan module, GPL-2.0-or-later) and a frontend. It is built into the relocatable object wswancore.o (see
 * wswan/frontend/Makefile), which exports exactly the wswan_host_* functions below and nothing else, so it can be
 * linked into a program that has its own globals (gpSP, TGB Dual, FCEUmm).
 *
 * Rules of the API (the same as nes/frontend/host.h)
 *  - Plain C, extern "C" from C++. No file access: ROMs, saves and states are memory buffers owned by the caller.
 *  - ONE instance per process, ONE thread: call everything from the thread that runs the frames.
 *  - Allocation: malloc/free at load / unload only (ROM image padded to a power of two, cartridge SRAM, a state
 *    scratch buffer, the one second audio ring). Frames, pads, saves and states do not allocate.
 *
 * TIMING IN ONE PLACE: the console runs 159 lines x 256 clocks at 3.072 MHz = 75.4716981 frames per second
 * (wswan_host_frame_rate), not 60. A game may change the line count (port 0x16); wswan_host_frame_rate_now() follows it.
 *
 * Copyright CBOC-Vita. This file is MIT; host.cpp contains logic adapted from libretro.c of beetle-wswan-libretro
 * (GPL-2.0-or-later), so a program containing wswancore.o is distributed under the GPL.
 */
#ifndef WSWAN_HOST_H
#define WSWAN_HOST_H
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Keys, in the order of the console's key matrix. X1..X4 = the left cross (up, right, down, left as the unit is
 * held horizontally); Y1..Y4 = the second cross (up, right, down, left in the unit's HORIZONTAL frame; vertical games
 * use these as their d-pad). */
enum {
	WSWAN_X1 = 1 << 0, WSWAN_X2 = 1 << 1, WSWAN_X3 = 1 << 2, WSWAN_X4 = 1 << 3,
	WSWAN_Y1 = 1 << 4, WSWAN_Y2 = 1 << 5, WSWAN_Y3 = 1 << 6, WSWAN_Y4 = 1 << 7,
	WSWAN_START = 1 << 8, WSWAN_A = 1 << 9, WSWAN_B = 1 << 10
};
#define WSWAN_WIDTH 224
#define WSWAN_HEIGHT 144
#define WSWAN_AUDIO_RATE 48000
#define WSWAN_CLOCK_HZ 3072000
#define WSWAN_FRAME_CLOCKS (159 * 256)               /* 40704 */
#define WSWAN_FRAME_RATE (3072000.0 / (159.0 * 256.0))   /* 75.4716981... */

enum { WSWAN_HORIZONTAL = 0, WSWAN_VERTICAL = 1 };
enum { WSWAN_ORIENT_AUTO = 0, WSWAN_ORIENT_HORIZONTAL = 1, WSWAN_ORIENT_VERTICAL = 2 };
enum { WSWAN_SAVE_IEEPROM = 0, WSWAN_SAVE_SRAM = 1, WSWAN_SAVE_EEPROM = 2 };
/* Where the orientation answer came from (wswan_host_orientation_info.source). */
enum { WSWAN_SRC_HEADER = 0, WSWAN_SRC_KEYS = 1, WSWAN_SRC_ICON = 2, WSWAN_SRC_MANUAL = 3, WSWAN_SRC_TABLE = 4 };

/* ---- lifetime -------------------------------------------------------------------------------------------- */

/* Once, before anything else. Returns 0. Defaults: audio 48000 Hz, greyscale palette, English, owner "WONDERSWAN". */
int wswan_host_init(void);
void wswan_host_shutdown(void);

/* Load a cartridge image from memory (.ws, .wsc, .pc2: the core reads the 16 byte footer itself, nothing is stripped).
 * `data` is copied. At least 64 KiB and at most 16 MiB. Any previous game is unloaded first. Returns 0, else non-zero
 * with a short reason in err (1 not initialised, 2 too small / too large, 3 not a WonderSwan image: footer without the
 * far jump, 4 out of memory). Cartridge save data is NOT loaded: call wswan_host_srm_import afterwards. */
int wswan_host_load_mem(const uint8_t *data, size_t n, const char *name, char *err, size_t errlen);
void wswan_host_unload(void);

/* ---- frames ---------------------------------------------------------------------------------------------- */

void wswan_host_set_pad(unsigned mask);     /* WSWAN_* bits; opposite directions on one cross cancel. Applies to the next frame */
/* Run one frame (until the next vertical blank: 144 visible lines drawn). */
void wswan_host_run_frame(void);
void wswan_host_reset(void);                 /* power cycle of the machine; the cartridge save stays */
void wswan_host_set_render(int on);          /* 0: skip drawing (state and sound identical), default 1 */
double wswan_host_frame_rate(void);          /* 75.4716981 */
double wswan_host_frame_rate_now(void);      /* from the line count the game programmed (port 0x16) */
uint64_t wswan_host_frame_count(void);       /* frames run since load */

/* ---- video ----------------------------------------------------------------------------------------------- */

/* The last drawn frame, 224 x 144 RGB565, pitch in bytes. Valid until the next wswan_host_run_frame. Monochrome
 * games are drawn in the mono palette (below); colour games in their own colours. */
const uint16_t *wswan_host_video(int *width, int *height, int *pitch_bytes);
int wswan_host_video_is_color(void);         /* 1 while the game has switched the video hardware to colour mode */
int wswan_host_game_is_color(void);          /* footer says "needs WonderSwan Color" */
/* Mono palette: 0 greyscale (default), 1 WonderSwan LCD, 2 WonderSwan Color LCD, 3 SwanCrystal LCD. Applies from the
 * next frame; has no effect on colour games. wswan_host_set_mono_colors: dark / light as 0xRRGGBB. */
int wswan_host_set_mono_palette(int id);
void wswan_host_set_mono_colors(uint32_t dark_rgb, uint32_t light_rgb);

/* ---- audio ----------------------------------------------------------------------------------------------- */

/* Four channels (wave, voice, sweep, noise) and HyperVoice are synthesised by the core's band limited synth directly at
 * the rate asked for (8000..96000, default 48000), stereo. Call before the first frame (drops queued audio). */
int wswan_host_set_audio_rate(unsigned hz);
unsigned wswan_host_audio_rate(void);
/* Pull up to max_frames stereo frames (L,R int16 pairs). Returns the number written. About 636 per frame at 48000. */
int wswan_host_audio_take(int16_t *dst, int max_frames);
void wswan_host_set_audio_enabled(int on);

/* ---- orientation ----------------------------------------------------------------------------------------- */

/* The unit is held sideways (horizontal, 224x144) or upright (vertical, the picture is meant to be turned 90 degrees
 * counter-clockwise and the Y cross is the d-pad). Hardware does not signal it: see INTEGRATION.md for the method
 * (footer flag at boot, key group polling, LCD icon bits, optional per game table, manual override).
 * Returns WSWAN_HORIZONTAL / WSWAN_VERTICAL: the effective answer (override applied). */
int wswan_host_orientation(void);
void wswan_host_set_orientation_mode(int mode);   /* WSWAN_ORIENT_AUTO (default) / _HORIZONTAL / _VERTICAL */
int wswan_host_orientation_mode(void);
typedef struct {
	int header_vertical;    /* footer flag (byte 12 bit 0) */
	int icon_bits;          /* port 0x15 bit 1 = vertical icon, bit 2 = horizontal icon: 1 = vertical only, 2 = horizontal only, 0 none/both */
	int keys_last_frame;    /* 1 = only the Y group was scanned last frame, 2 = only X, 3 = both, 0 neither */
	int detected;           /* WSWAN_HORIZONTAL / VERTICAL: the automatic answer */
	int source;             /* WSWAN_SRC_* of the last change */
	unsigned switches;      /* how many times the automatic answer changed since load */
	unsigned frames_since_switch;
} wswan_orientation_info;
void wswan_host_orientation_info(wswan_orientation_info *out);
/* Per game table (keyed by footer bytes that survive translation patches: developer id, cart id, save type, ROM size
 * code; NOT the checksum). mode as above; 0 removes. Entries are matched at load. Max 64 entries. */
int wswan_host_orientation_table_set(unsigned dev_id, unsigned cart_id, int mode);

/* ---- cartridge and console save data (the caller saves it) ------------------------------------------------ */

/* Three regions: internal console EEPROM (1 KiB, owner data, always present), cartridge SRAM (8..512 KiB, or 0), cartridge
 * EEPROM (128 B..2 KiB, or 0). A cartridge has at most one of the last two. Valid while the game is loaded. */
size_t wswan_host_save_size(int region);
uint8_t *wswan_host_save_data(int region);
int wswan_host_save_dirty(int region);       /* 1 when it differs from the last load / mark_saved (a compare) */
void wswan_host_save_mark_saved(int region);
void wswan_host_save_mark_unsaved(int region);
/* The single combined file, beetle-wswan's .srm: the cartridge SRAM or EEPROM bytes exactly as RetroArch writes them,
 * then (extension, ignored by RetroArch) "WSiE", u32 LE 1024, the 1024 internal EEPROM bytes. */
size_t wswan_host_srm_size(void);            /* 0 = nothing to save at all (cannot happen: the internal EEPROM counts) */
size_t wswan_host_srm_export(uint8_t *dst, size_t cap);   /* copies and counts everything as saved */
size_t wswan_host_srm_import(const uint8_t *src, size_t n);   /* short files fill the front, a missing tail keeps defaults; counts as saved. Returns bytes used */
int wswan_host_srm_dirty(void);              /* any region dirty */
void wswan_host_srm_mark_unsaved(void);      /* a write failed */
/* Owner data written into the internal EEPROM at load (so games do not ask for a name / birthday every boot).
 * Defaults: "WONDERSWAN", 1999-03-04... see host.cpp. Call before load; wswan_host_owner_apply() rewrites it live (dirty). */
void wswan_host_set_owner(const char *name, int year, int month, int day, int sex /*1 male 2 female*/, int blood /*1 A 2 B 3 O 4 AB*/);
void wswan_host_owner_apply(void);

/* ---- clock ----------------------------------------------------------------------------------------------- */

/* The cartridge RTC (Dicing Knight, Dokodemo Hamster 3...) is seeded at load from this source, then counts emulated time.
 * NULL = the C library's time(). The value is split with localtime(): pass local wall clock seconds if the C library has
 * no time zone. wswan_host_rtc_sync() re-seeds the running RTC from the source (call after resuming from sleep). */
void wswan_host_set_time_source(time_t (*fn)(void));
void wswan_host_rtc_sync(void);

/* ---- save states ----------------------------------------------------------------------------------------- */

/* Fixed for a loaded game (measured at load plus 1 KiB headroom, zero padded) so a rewind ring can use equal slots. */
size_t wswan_host_state_size(void);
size_t wswan_host_state_save_mem(uint8_t *buf, size_t cap);    /* cap >= state_size; returns the size, 0 on failure */
int wswan_host_state_load_mem(const uint8_t *buf, size_t n);   /* 0 ok, 1 no game, 3 refused */

/* ---- memory for achievements and tools ------------------------------------------------------------------- */

/* RetroAchievements console 53 (WonderSwan, also used for Color): address space as rcheevos defines it: 0x00000-0x0FFFF
 * the 64 KiB system RAM, 0x10000.. the cartridge save RAM (SRAM, else the cartridge EEPROM, contiguous, as beetle-wswan
 * exposes it; up to 0x8FFFF). Returns the bytes that were inside a region (cut at its end; 0 = unmapped). No side effects. */
int wswan_host_ra_read(unsigned addr, uint8_t *out, unsigned n);
const uint8_t *wswan_host_ram(size_t *size);
int wswan_host_peek(unsigned addr);           /* one byte of the RA map, -1 when unmapped */

/* ---- cheats ---------------------------------------------------------------------------------------------- */

/* Raw RAM pokes "AAAAA:VV" (hex address in the RA map above, hex byte), several joined by + , ; . _ or space, written
 * every frame. Returns how many were accepted. Dropped at unload. */
int wswan_host_cheat_add(const char *codes);
void wswan_host_cheat_reset(void);
int wswan_host_cheat_count(void);

/* ---- information ----------------------------------------------------------------------------------------- */

const char *wswan_host_rom_info(void);        /* one line: size, developer, cart id, colour, orientation flag, save type, checksum */
uint32_t wswan_host_rom_crc(void);            /* CRC-32 of the whole file as loaded */
int wswan_host_footer(uint8_t out[16]);       /* the 16 footer bytes; 0 when no game */
void wswan_host_set_log(void (*fn)(const char *line));

#ifdef __cplusplus
}
#endif
#endif
