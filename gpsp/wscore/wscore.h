/* Copyright (c) 2026 CBOC-Vita contributors.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * WonderSwan / WonderSwan Color inside the main app, the NES way (nescore.h):
 * Beetle WonderSwan (wswan/core, GPL-2.0-or-later) behind wswan/frontend/
 * host.h (wswan_host_*, one relocatable object, wswancore.o), plus this
 * adapter (gpsp/vita/wswancore.mk).  fe_host.c reaches it through
 * gbcore_mux.h, so a WonderSwan game gets the Game Boy path's states,
 * rewind, fast forward, Continue and battery saves.
 *
 * ONE CALL PER DISPLAY REFRESH.  The console runs 75.47 frames a second; the
 * app calls wscore_run_frame once per refresh (59.94 Hz, or the timer pacer's
 * rate: wscore_set_display_rate), and the call runs the 1 or 2 emulated
 * frames that refresh covers (wswan/frontend/blend.h's exact schedule,
 * 1.259 on average), feeds all their sound, and presents one picture: the
 * temporal blend of the frames the refresh covers (default on), else the
 * newest frame.
 *
 * ORIENTATION.  wswan_host_orientation() is read every refresh (the game's
 * LCD icons, its key scan, the cartridge footer; Rotation: Auto/Horizontal/
 * Vertical overrides it).  Horizontal: the 224x144 picture.  Vertical,
 * "Upright" (default): the picture turned 90 degrees counter-clockwise,
 * 144x224, controls turned with it.  Vertical, "Turn the Vita": the frame
 * unrotated (224x144, full size), controls 1:1 with the turned Vita.  The
 * picture and the control map always change together, in the same call.
 */
#ifndef GBADHOC_WSCORE_H
#define GBADHOC_WSCORE_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct wscore wscore_t;

typedef struct wscore_video_frame {
  const uint16_t *pixels;     /* RGB565, valid until the next run_frame */
  unsigned width, height, pitch_bytes;
} wscore_video_frame_t;

typedef struct wscore_callbacks {
  void *userdata;
  void (*video)(void *userdata, const wscore_video_frame_t *frame);
  void (*audio_batch)(void *userdata, const int16_t *interleaved, size_t frames);
} wscore_callbacks_t;

/* Vita buttons (wswan/frontend/layout.h WSL_* bits) */
enum {
  WSCORE_UP = 1u << 0, WSCORE_DOWN = 1u << 1, WSCORE_LEFT = 1u << 2,
  WSCORE_RIGHT = 1u << 3, WSCORE_CROSS = 1u << 4, WSCORE_CIRCLE = 1u << 5,
  WSCORE_SQUARE = 1u << 6, WSCORE_TRIANGLE = 1u << 7, WSCORE_L = 1u << 8,
  WSCORE_R = 1u << 9, WSCORE_START = 1u << 10, WSCORE_SELECT = 1u << 11
};
enum { WSCORE_ROT_AUTO = 0, WSCORE_ROT_HORIZONTAL = 1, WSCORE_ROT_VERTICAL = 2 };
enum { WSCORE_UPRIGHT = 0, WSCORE_TURNED = 1 };
/* what the player sees now */
enum { WSCORE_SHOW_HORIZONTAL = 0, WSCORE_SHOW_UPRIGHT = 1, WSCORE_SHOW_TURNED = 2 };

wscore_t *wscore_create(const char *rom_path, unsigned audio_rate,
                        const wscore_callbacks_t *callbacks);
const char *wscore_error(void);
int wscore_run_frame(wscore_t *core);
void wscore_set_skip_render(wscore_t *core, int skip);
void wscore_shutdown(wscore_t *core);

/* Vita buttons for the next call (WSCORE_* bits): mapped to the console's
 * keys by the layout shown at that call. */
void wscore_set_buttons(unsigned vita_bits);
/* The rate run_frame is called at, num/den Hz (default 60000/1001). */
void wscore_set_display_rate(uint64_t num, uint64_t den);
void wscore_set_rotation(int mode);          /* WSCORE_ROT_* */
void wscore_set_vertical_play(int how);      /* WSCORE_UPRIGHT / _TURNED */
void wscore_set_blend(int on);               /* default 1 */
void wscore_set_mono_palette(int id);        /* 0 greyscale, 1 classic LCD tint */
/* Local wall-clock seconds for a cartridge clock (the Vita's libc has no
 * time zone); set before the game starts. */
void wscore_set_time_source(time_t (*fn)(void));
int wscore_showing(void);                    /* WSCORE_SHOW_* */
/* Counts every change of WSCORE_SHOW_* since the game started. */
unsigned wscore_layout_changes(void);

unsigned wscore_sample_rate(const wscore_t *core);
unsigned wscore_frame_width(const wscore_t *core);
unsigned wscore_frame_height(const wscore_t *core);
double wscore_fps(const wscore_t *core);     /* 75.4717 */
int wscore_rom_title(wscore_t *core, char *dst, size_t cap);
uint32_t wscore_rom_crc(void);
int wscore_is_color(void);

/* The battery file is beetle-wswan's .srm (plus the console EEPROM tail). */
size_t wscore_save_ram_size(wscore_t *core);
int wscore_save_ram_read(wscore_t *core, void *dst, size_t cap);
int wscore_save_ram_write(wscore_t *core, const void *src, size_t size);
int wscore_save_ram_dirty(wscore_t *core);

size_t wscore_state_size(wscore_t *core);
long wscore_state_save(wscore_t *core, void *dst, size_t cap);
int wscore_state_load(wscore_t *core, const void *src, size_t size);

/* RetroAchievements console 53 */
uint32_t wscore_ra_read(wscore_t *core, uint32_t addr, uint8_t *out, uint32_t len);
int wscore_peek(wscore_t *core, unsigned address);
/* Raw RAM pokes "AAAAA:VV" joined by + , or space; replaces the list. */
int wscore_cheats_set(wscore_t *core, const char *codes);
void wscore_reset(wscore_t *core);

#ifdef __cplusplus
}
#endif
#endif
