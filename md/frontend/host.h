/*
 * Sega host: the platform independent layer between Genesis Plus GX (md/core) and a frontend, for the Mega Drive /
 * Genesis, Master System (and Mark III), Game Gear and SG-1000 (SC-3000 cartridges). It is built into the relocatable
 * object mdcore.o (see md/core/Makefile), which exports exactly the md_host_* functions below and nothing else, so the
 * same discipline as nes/frontend holds even though md.bin is a separate program.
 *
 * Rules of the API
 *  - Plain C, extern "C" from C++. No file access: ROMs, battery RAM and states are memory buffers owned by the caller.
 *  - ONE instance per process (the core keeps its machine in globals), ONE thread: call everything from the thread that
 *    runs the frames. Allocation only in md_host_init (fixed buffers) and md_host_load_mem (the 10 MB cartridge area is
 *    allocated once and reused, plus the audio ring). Frames, pads, saves and states do not allocate.
 *  - Deterministic: the same ROM, options, seed and pad script give the same frames, audio and state on every machine
 *    and C library (no rand(), no wall clock). Two consoles that load the same state and get the same pads stay equal.
 *
 * Copyright CBOC-Vita. MIT licence (the licence of this file; the core in md/core keeps the Genesis Plus GX licence,
 * non-commercial: a program containing mdcore.o must not be sold, and ships its full source when modified).
 */
#ifndef MD_HOST_H
#define MD_HOST_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Pad bits. The value is the core's own pad word, so it can be passed through. Master System / Game Gear / SG-1000
 * pads have two buttons: MD_B is button 1 and MD_C is button 2 there; MD_START is Pause (SMS / SG) or Start (GG).
 * X Y Z MODE only exist on a six button pad and are ignored by a three button pad. Up+Down and Left+Right together are
 * not filtered by the core (a few games rely on that on a real pad): filter in the frontend if you want. */
enum {
	MD_UP = 0x0001, MD_DOWN = 0x0002, MD_LEFT = 0x0004, MD_RIGHT = 0x0008,
	MD_B = 0x0010, MD_C = 0x0020, MD_A = 0x0040, MD_START = 0x0080,
	MD_Z = 0x0100, MD_Y = 0x0200, MD_X = 0x0400, MD_MODE = 0x0800
};
#define MD_BUTTON1 MD_B
#define MD_BUTTON2 MD_C

#define MD_AUDIO_RATE 48000          /* default audio rate; md_host_set_audio_rate changes it (8000..96000) */
#define MD_MAX_WIDTH 352             /* the largest picture the core draws: 320 + 2 * 14 border pixels (only with overscan; 320 otherwise) */
#define MD_MAX_HEIGHT 576            /* an interlaced double resolution PAL frame with borders; 480 without overscan, 224..240 normally */

/* system: what md_host_load_mem was told, and what md_host_system() reports */
enum { MD_SYS_AUTO = 0, MD_SYS_MD, MD_SYS_SMS, MD_SYS_GG, MD_SYS_SG };
enum { MD_REGION_AUTO = 0, MD_REGION_USA, MD_REGION_EUROPE, MD_REGION_JAPAN_NTSC, MD_REGION_JAPAN_PAL };
enum { MD_PAD_AUTO = 0, MD_PAD_3, MD_PAD_6 };
enum { MD_PIXEL_RGB565 = 1 };
/* the six values md_host_system_hw() can name (what the core decided inside one system): "Mega Drive", "Master System II",
 * "Master System", "Mark III", "Game Gear", "Game Gear (SMS mode)", "SG-1000", "SG-1000 II", "SC-3000" ... */

/* ---- worker threads ---------------------------------------------------------------------------------------- */

/* Optional. The VDP line renderer can run on its own thread (md/core/vdp_thread.h: a write log, bit identical pictures, sound and state).
 * The core has no thread code: pass the platform's functions (md/core/thread_ops.h) and MD_THREAD_VDP before md_host_load_mem; the threads
 * start inside the load and end in md_host_unload. ops = NULL or mask 0: everything on the calling thread (the default). The worker is
 * idle whenever a host call returns, so every other API call stays valid from the calling thread only. Returns 0. */
#define MD_THREAD_VDP 1u          /* the VDP line renderer on its own thread (Mega Drive) */
#define MD_THREAD_VDP_SMS 2u      /* ... also for Master System / Game Gear / SG-1000 (with MD_THREAD_VDP) */
struct md_thread_ops;
int md_host_set_threads(const struct md_thread_ops *ops, unsigned mask);
/* Debug and census for the VDP worker (the Linux twin's --vdp-check / --vdp-stress / census line). check: compare the worker's copy of the
 * raw VDP state with the emulating thread's at the end of every frame and count the differences; stress: join after every drawn line (a
 * game that reads the status register every line). Both only matter while the worker runs. */
void md_host_vdp_debug(int check, int stress);
unsigned long md_host_vdp_check_errors(void);
/* census of the worker since it started: records queued, status joins, frame joins, polls spent waiting, queue full waits, wake-ups,
 * VRAM patterns / whole VRAM images sent. Returns 0 when the worker is off. */
int md_host_vdp_census(unsigned long out[11]);   /* [9] worker busy us, [10] join wait us: 0 without now_us */

/* ---- lifetime -------------------------------------------------------------------------------------------- */

/* Once, before anything else. Returns 0 on success. Starts with: audio rate 48000, region automatic, FM chip "discrete
 * YM2612", pads automatic (3 or 6 buttons from the ROM header), interlace double field, sprite limit as on hardware. */
int md_host_init(void);
void md_host_shutdown(void);

/* Load a cartridge from memory. `name` is the file name (only its extension is used: md gen bin smd mdx 32x? sms gg sg sc
 * ...; may be NULL), `system` MD_SYS_AUTO or a system forced by the caller. With AUTO the extension decides when it is
 * unambiguous, and the header otherwise (".bin" is common to all: "SEGA" at 0x100 or a SMD header is a Mega Drive ROM,
 * "TMR SEGA" at 0x1FF0 / 0x3FF0 / 0x7FF0 is a Master System / Game Gear ROM, the region nibble tells which).
 * Interleaved (.smd) dumps and ROMs with a 512 byte header are converted by the core, byte swapped dumps too. `data` is
 * copied: the caller may free it on return. Any previous game is unloaded first. Returns 0, else non-zero with a short
 * reason in err (1 not initialised, 2 unreadable / too big (> 10 MB), 3 not a Sega ROM, 4 the core refused it).
 * The console is powered on at the end of the load. Battery RAM is NOT loaded here: call md_host_sram_write afterwards. */
int md_host_load_mem(const uint8_t *data, size_t n, const char *name, int system, char *err, size_t errlen);
void md_host_unload(void);
int md_host_loaded(void);
/* The seed of the only random values the core uses (the position of the CPU in the frame after md_host_reset). Set it
 * BEFORE md_host_load_mem / md_host_reset; the same seed gives the same machine everywhere. Default 1. */
void md_host_set_seed(uint32_t seed);

/* ---- frames ---------------------------------------------------------------------------------------------- */

/* Pads: port 0 / 1 = pad 1 / 2 (the Game Gear has one). Applies to the next frame. */
void md_host_set_pad(int port, unsigned mask);
/* 3 or 6 buttons (MD_PAD_*). AUTO follows the ROM header (the "6" in its I/O support field), as the core does. Effective
 * at once; it changes what the game can read, so it belongs in the emulation settings two consoles must agree on. */
int md_host_set_pad_type(int port, int type);
int md_host_pad_type(int port);        /* the type in force: MD_PAD_3, MD_PAD_6, or 0 when the system has two button pads */

/* Run one frame (from the start of vertical blank to the next). Cheats are applied inside. */
void md_host_run_frame(void);
void md_host_reset(void);              /* the console's reset button: the game stays loaded; uses the seed */
/* Render flag: 0 = the next frames skip drawing (game logic, sound and the state are identical; the video buffer keeps the
 * last drawn picture). For fast forward and rewind. Default 1. */
void md_host_set_render(int on);

/* ---- video ----------------------------------------------------------------------------------------------- */

/* Everything a picture pipeline needs about the frame just run. `w` x `h` is the picture in the buffer (what md_host_video
 * returns); the native fields describe the machine's own picture before the optional processing here: native_w x native_h is
 * 256x192 / 256x224 / 256x240 (Master System, H32 Mega Drive), 320x224 / 320x240 (H40), 160x144 (Game Gear window). An
 * interlaced double resolution Mega Drive frame has h = 2 * native_h with the "interlace" option on (md_host_set_option),
 * and h = native_h with it off. `serial` increases whenever any of the fields changes (a game that switches H32 / H40 or
 * 224 / 240 lines mid-game), so a pipeline re-reads this only when it differs from the last value. */
typedef struct md_video_info {
	int w, h;                  /* picture in the buffer (after the SMS border crop) */
	int native_w, native_h;    /* the machine's visible area: 256 / 320 / 160 by 144 / 192 / 224 / 240 */
	int system;                /* MD_SYS_* of the loaded game */
	int h40;                   /* 1: 320 pixel wide mode */
	int interlace;             /* 0 none, 1 interlaced (single field output), 2 interlaced, double resolution output (h = 2 * native_h) */
	int pal;                   /* 1: 50 Hz timings (313 lines) */
	int left_mask;             /* Master System: pixels at the left edge the game masks with the backdrop colour (8 or 0); still in the picture unless sms_border is on */
	int crop_left, crop_right; /* pixels removed at the left / right by the sms_border option (already out of w) */
	int gg_window;             /* 1: the 160x144 Game Gear LCD window (native_w x native_h is that window), 0: the full 256x192 (option gg_extra) */
	int gg_band;               /* 1: the Game Gear wide view's picture: 256x144, the LCD window in its middle (option gg_band; w = 256, native_w = 256) */
	double dar;                /* display aspect ratio (width / height) of the picture, from the pixel clock; 0 = square pixels */
	double par;                /* pixel aspect ratio (a pixel's width / height) behind dar */
	double fps;                /* 59.9227 / 49.7014 (the console's frame rate) */
	unsigned serial;
} md_video_info;

/* The last drawn frame as RGB565 (the only pixel format, MD_PIXEL_RGB565), pitch in bytes; valid until the next
 * md_host_run_frame. The picture starts at the top left of the buffer. */
const uint16_t *md_host_video(int *width, int *height, int *pitch_bytes);
void md_host_video_info(md_video_info *vi);
int md_host_pixel_format(void);
/* Draw straight into a buffer the caller owns (a texture, a wide-view canvas): at least MD_MAX_HEIGHT rows of pitch_px
 * pixels (pitch_px >= MD_MAX_WIDTH). NULL goes back to the internal buffer. Effective from the next frame. */
void md_host_set_framebuffer(uint16_t *buf, int pitch_px);

/* ---- wide view --------------------------------------------------------------------------------------------- */

/* Mega Drive: the VDP worker (or the core's own renderer) can draw a strip of picture beyond each side of the screen, into the frame buffer next
 * to the picture: `margin` pixels (0..MD_WIDE_MAX, 0 = off) at columns -margin .. -1 and w .. w + margin - 1 of every drawn line. The machine, the
 * picture inside the screen, the sound and the saved state are identical with the strips on and off (the strip pass saves and restores the
 * sprite status; tests: md/twin/selftest.sh). The frame buffer must be a canvas: give md_host_set_framebuffer(canvas + MD_WIDE_MAX,
 * MD_WIDE_PITCH) (rows of MD_WIDE_PITCH pixels, MD_MAX_HEIGHT of them); with the internal buffer the margin stays 0.
 * Lines the strip cannot be drawn for (window plane on the line, interlace, per 2-cell vertical scroll with different values, display off)
 * have WL_STRIP clear in md_wide_frame; the strip pixels of such a line are not written (stale). Returns the margin in force. */
#define MD_WIDE_MAX 96
#define MD_WIDE_PITCH 512            /* MD_WIDE_MAX + 320 + MD_WIDE_MAX */
int md_host_set_wide_margin(int margin);
/* Which planes the strips contain (bit 0 plane A, bit 1 plane B; default 3). A plane left out is drawn as nothing in the strips: the strip
 * pixel is then transparent wherever no sprite and no other plane drew (md_wide_frame.opq says which pixels are opaque). Sprites are always in. */
void md_host_set_wide_planes(int mask);

/* What the line renderer saw for each line of the frame just run (the Mega Drive in mode 5 and, for the Master System / Game Gear, in mode 4
 * with xs[0] / ys[0] = the scroll registers as the line used them, col_mask 31, ntab = the name table base). Same layout as the core's
 * wide_line_t (md/core/vdp_render.h). */
enum { MDW_ON = 1, MDW_STRIP = 2, MDW_WINDOW = 4, MDW_VSCROLL = 8, MDW_IM2 = 16, MDW_H40 = 32, MDW_M4 = 64 };
typedef struct md_wide_line {
	uint8_t flags, col_mask, shift, pad;
	uint16_t xs[2], ys[2];
	uint16_t ntab, ntbb, ntwb, row_mask;
} md_wide_line;
typedef struct md_wide_frame {
	int h;                       /* lines in the picture */
	int margin;                  /* the strip width in force (Mega Drive) */
	const md_wide_line *line;    /* h entries, valid until the next md_host_run_frame */
	const uint32_t *opq;         /* [256][2][3]: per line and side (0 left, 1 right) 96 bits, bit d = the strip pixel d pixels from the screen's edge is opaque */
	const uint8_t *vram;         /* 64 KB (the emulating thread's copy: complete when the frame has finished) */
	const uint8_t *vsram;        /* 128 bytes */
	const uint8_t *cram;         /* 128 bytes */
	int reg[24];                 /* the VDP registers 0..23 */
} md_wide_frame;
void md_host_wide_frame(md_wide_frame *f);

/* ---- audio ----------------------------------------------------------------------------------------------- */

/* The core synthesises (FM, PSG) at its native rates and resamples with a band limited buffer straight to the rate asked
 * for: no second resampler. Default 48000. Call before the first frame (changing it re-initialises the sound and drops
 * queued audio). Returns 0, else 1. About 800 frames per NTSC frame at 48000 Hz (960 at 50 Hz). */
int md_host_set_audio_rate(unsigned hz);
unsigned md_host_audio_rate(void);
/* Pull up to max_frames stereo frames (L,R int16 pairs) made by the frames run so far. Returns the number written. */
int md_host_audio_take(int16_t *dst, int max_frames);
void md_host_set_audio_enabled(int on);    /* 0: silence (the frames still run) */

/* ---- battery RAM (the caller saves it) ------------------------------------------------------------------- */

/* The .srm of RetroArch's Genesis Plus GX core: the raw save RAM bytes (up to 64 KB), as the core holds them (cartridge RAM
 * on the odd bytes for most Mega Drive games), cut after the last byte that is not 0xFF; 0 = nothing was written yet. EEPROM
 * and flash cartridges use the same buffer. Master System / Game Gear: the cartridge RAM (up to 32 KB). */
size_t md_host_sram_size(void);            /* what a save would write now (0 = nothing / no battery) */
size_t md_host_sram_capacity(void);        /* 0x10000 when the cartridge has save RAM, else 0 */
size_t md_host_sram_read(uint8_t *out, size_t cap);    /* copies md_host_sram_size() bytes; returns the number */
/* Copies a saved image in (min(n, capacity) bytes; call right after load) and counts it as saved. Returns the bytes used. */
size_t md_host_sram_write(const uint8_t *data, size_t n);
int md_host_sram_dirty(void);              /* 1 when the RAM differs from the last load / mark_saved (a CRC compare, ~0.1 ms) */
void md_host_sram_mark_saved(void);
void md_host_sram_mark_unsaved(void);

/* ---- save states ----------------------------------------------------------------------------------------- */

/* A container: 16 byte header ("MDST", version, system, payload length), the core's state zero padded to a constant size,
 * and a CRC-32 trailer over everything before it. md_host_state_size() is the number of bytes every save returns for the loaded
 * game (the Mega Drive state is about 145 KB), so a rewind ring can use equal slots. 0 when no game is loaded.
 * A state is loaded only when its length is state_size, its header matches and its CRC is right. Video, game state, RAM
 * and the sound chips' registers are restored exactly (the next frames' picture is identical); the resampler and
 * filter memory of the audio output are not (a few samples after a load differ by less than one LSB of a sample
 * click: both consoles load the same state, so lockstep holds; PROVENANCE.md has the details). */
size_t md_host_state_size(void);
size_t md_host_state_save_mem(uint8_t *buf, size_t cap);    /* needs cap >= state_size; returns the size, 0 on failure */
size_t md_host_state_save_quick(uint8_t *buf, size_t cap);  /* as save_mem without the CRC (rewind keeps its states in memory only) */
int md_host_state_load_quick(const uint8_t *buf, size_t n);  /* as load_mem without the CRC: only for what save_quick wrote */
int md_host_state_load_mem(const uint8_t *buf, size_t n);   /* 0 ok, 1 no game, 3 refused (length, header or CRC; machine untouched) */

/* ---- memory for achievements and tools ------------------------------------------------------------------- */

/* RetroAchievements (rc_consoles.h: MEGA_DRIVE, MASTER_SYSTEM, GAME_GEAR, SG1000): the address space rcheevos expects.
 *   Mega Drive:   $000000-$00FFFF system RAM = the 68000's $FF0000-$FFFFFF (true byte addresses, the core's byte swapped
 *                 RAM is un-swapped here), $010000-$01FFFF cartridge RAM (the save RAM buffer as held, no swap)
 *   Master System / Game Gear: $0000-$1FFF system RAM (Z80 $C000-$DFFF), $2000-$9FFF cartridge RAM (the save RAM buffer)
 *   SG-1000:      $0000-$03FF RAM ($C000), $0400-$1FFF extended RAM ($C400), $2000-$3FFF ($2000), $4000-$5FFF ($8000):
 *                 only what the cartridge really has is readable
 * Copies n bytes from addr into out; returns the number of bytes that were inside a window (a request is cut at the end of
 * its window; 0 = unmapped). Reads have no side effects. */
int md_host_ra_read(unsigned addr, uint8_t *out, unsigned n);
int md_host_ra_console(void);   /* the rc_consoles.h id of the loaded game: 1 MEGA_DRIVE, 11 MASTER_SYSTEM, 15 GAME_GEAR, 33 SG1000; 0 = none */
uint8_t md_host_peek(unsigned addr);   /* one byte of the CPU's own address space without side effects (68000 or Z80), ROM and RAM only */

/* ---- cheats ---------------------------------------------------------------------------------------------- */

/* The cheat database (gpsp/vita/tests/cheats/SEGA-CODES.md) stores decoded parts; these are applied as that file says:
 *   Mega Drive "AAAAAA:VVVV" (hex): address $FF0000-$FFFFFF = work RAM, written every frame: a byte store at exactly that
 *     address when the value's high byte is 0, otherwise a word store at address & ~1; address below $400000 = a ROM word patch
 *     (the word at address & ~1 becomes the value, undone by md_host_cheat_reset); anything else is refused.
 *   Master System / Game Gear / SG-1000 "AAAA:VV[:CC]": $C000-$FFFF = RAM at address & $1FFF, written every frame; below $C000 = a
 *     ROM patch of the byte the Z80 sees there now, applied again after every bank switch, and with :CC only while that byte is CC
 *     (a compare of 00 is no compare, as in GPGX).
 * At most 150 patches (GPGX's MAX_CHEATS). Game Genie / Action Replay text is decoded by the database tool, never here. */
/* One patch; compare -1 = none. Returns 0 when added, 1 refused (no game, bad address or value, table full). */
int md_host_cheat_add_patch(unsigned address, unsigned value, int compare);
/* Parts as text joined by + , ; or white space, e.g. "FFE4A2:0020+FFC843:0000" or "C123:05:FF". Returns how many were accepted. */
int md_host_cheat_add(const char *codes);
void md_host_cheat_reset(void);            /* undoes the ROM patches and forgets all */
int md_host_cheat_count(void);

/* ---- console state and options --------------------------------------------------------------------------- */

int md_host_system(void);                  /* MD_SYS_MD / SMS / GG / SG of the loaded game, 0 none */
const char *md_host_system_hw(void);       /* the exact hardware the core chose, e.g. "Master System II" */
/* Region: the header's (Mega Drive: 0x1F0; Master System / Game Gear: the "TMR SEGA" region nibble) unless forced. PAL regions
 * run the true 50 Hz timings (313 lines, 49.70 Hz), NTSC 59.92 Hz. A change takes effect at once for timings and the I/O
 * region bits, and is part of the emulation settings two consoles must agree on. */
int md_host_set_region(int region);        /* MD_REGION_*; returns 0 */
int md_host_region(void);                  /* MD_REGION_USA / EUROPE / JAPAN_NTSC / JAPAN_PAL in force */
int md_host_region_detected(void);         /* what the header said */
int md_host_is_pal(void);
double md_host_fps(void);
const char *md_host_rom_title(void);       /* the international name from the header, "" for 8 bit ROMs */
const char *md_host_rom_info(void);        /* one line: system, size, product, checksum, region, save RAM, mapper */
uint32_t md_host_rom_crc(void);            /* CRC-32 of the ROM as dumped without copier header, de-interleaved (the No-Intro CRC) */

/* Core options by name; returns 0 when the name is known (a value out of range is clamped):
 *   fm 0..4            0 YM2612 discrete (default), 1 ASIC YM3438, 2 enhanced, 3 Nuked YM2612, 4 Nuked YM3438 (the Nuked ones are
 *                      several times slower and are never a default)
 *   fm_hq 0/1          high quality FM resampling (default 1)    psg_hq 0/1 (default 1)
 *   ym2413 0..2        Master System FM unit: 0 off, 1 on, 2 automatic (default; on for games that use it)
 *   opll_nuked 0/1     Nuked OPLL for the FM unit (default 0)
 *   fm_preamp 0..400   (default 100)    psg_preamp 0..400 (default 150)
 *   lowpass 0..100     0 = off (default); otherwise the low pass range in percent (60 is GPGX's own)
 *   mono 0/1
 *   interlace 0/1      Mega Drive interlace mode 2: 1 = draw both fields into a double height picture (default), 0 = one field
 *   sprite_limit 0/1   1 = the hardware's sprite limits (default), 0 = draw all sprites of a line (no flicker; the overflow
 *                      flag the game reads is unchanged)
 *   sms_border 0..2    Master System: 0 = full 256 pixel picture (default), 1 = hide the 8 pixel masked column at the left (the
 *                      game masks it when it scrolls), 2 = hide 8 pixels at both sides
 *   gg_extra 0/1       Game Gear: 1 = the full 256x192 picture instead of the 160x144 window (next load)
 *   gg_band 0/1        Game Gear wide view: 1 = the picture is the 256 columns of the VDP's rows that the LCD shows (256x144, the LCD window in the
 *                      middle columns 48..207); the machine runs exactly as without it (unlike gg_extra, which also changes what the core
 *                      does with sprites outside the window); effective at once
 *   overscan 0..3      bit 0 = top / bottom borders, bit 1 = side borders (next load; default 0)
 *   force_dtack 0/1, addr_error 0/1 (default 1)
 *   lcd 0..100         Game Gear LCD ghosting in percent (default 0)
 *   vdp_mode 0..2      0 = follow the region, 1 = force 60 Hz, 2 = force 50 Hz
 * Options that change what a game can see (region, vdp_mode, pad type, ym2413, force_dtack, addr_error, sprite_limit) belong in the
 * compat word of a two player session; the sound and picture options do not change the machine. */
int md_host_set_option(const char *name, int value);
int md_host_get_option(const char *name, int *value);

/* ---- messages -------------------------------------------------------------------------------------------- */

/* Optional sink for the core's text output; NULL for none. */
void md_host_set_log(void (*fn)(const char *line));

#ifdef __cplusplus
}
#endif
#endif
