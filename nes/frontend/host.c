/*
 * NES host for FCEUmm. See host.h.
 *
 * Copyright CBOC-Vita. MIT licence (see nes/frontend/LICENSE).
 * Links against FCEUmm (nes/core, GPL-2.0-or-later); this file only calls its public
 * functions and supplies the callbacks it expects (the part of the libretro driver that
 * is not libretro: palette, messages, input, region). Plain C: no C++ runtime is needed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "fceu-types.h"
#include "fceu.h"
#include "driver.h"
#include "git.h"
#include "cart.h"
#include "fds.h"
#include "state.h"
#include "ines.h"
#include "unif.h"
#include "video.h"
#include "fceu-memory.h"
#include "cheat.h"
extern uint8_t *XBuf;
extern uint8_t *XDBuf;
extern CartInfo iNESCart;
extern CartInfo UNIFCart;
extern int option_ramstate;
extern FCEUGI *GameInfo;
#include "fourscore_db.h"
#include "host.h"

#define MAX_ROM_BYTES (24u * 1024 * 1024)
#define STATE_SCRATCH (1536u * 1024)
#define STATE_HEADROOM 1024u

static int g_inited, g_loaded, g_unif;
static char g_title[128];
static char g_info[240];
static uint8_t *g_sram_saved;               /* copy of what the caller has (or the load put in) */
static size_t g_sram_saved_n;
static int g_audio_on = 1, g_render = 1;
static char g_msg[200];
static void (*g_log)(const char *);
static uint8_t g_bios[8192];
static int g_have_bios;

static uint32_t g_joy;                      /* player n in byte n, as the core's gamepad reader wants it */
static uint32_t g_dummy;
static unsigned g_pad[4];
static int g_4p_mode = NES_4P_AUTO;
static int g_db_fs, g_db_fc, g_fc_set;      /* the loaded game is in the Four Score / Famicom adapter list */

static int g_opt_region = NES_REGION_AUTO;
static unsigned g_detected;                 /* (dendy << 1) | PAL as the header / database said */
static int g_no_sprite_limit = 1;

/* Games that use the hardware's 8 sprites per line limit on purpose (extra sprites hide parts of the picture), so
 * "no flicker" is switched off for them. Key = CRC-32 of PRG+CHR (what nes_host_rom_crc returns). Only CRCs that
 * are in FCEUmm's own database (core/ines-correct.h) are listed here; the other games of the same kind (see
 * nes/README.md) can be added with nes_host_sprite_exception_add when their CRC is known. */
static const uint32_t k_sprite_keep[] = {
	0x3fe272fb, 0xeaf7ed72,   /* The Legend of Zelda (both PRG revisions): sprites hide at the top/bottom of dungeon screens */
};
static uint32_t g_sprite_keep_extra[16];
static unsigned g_sprite_keep_n;
static int g_sprite_exception;

static int sprite_limit_exception(void)
{
	unsigned i;
	uint32_t crc = g_loaded && GameInfo && GameInfo->type == GIT_CART ? iNESCart.CRC32 : 0;
	if (!crc) return 0;
	for (i = 0; i < sizeof k_sprite_keep / sizeof k_sprite_keep[0]; i++) if (k_sprite_keep[i] == crc) return 1;
	for (i = 0; i < g_sprite_keep_n; i++) if (g_sprite_keep_extra[i] == crc) return 1;
	return 0;
}

static void apply_sprite_limit(void)
{
	g_sprite_exception = sprite_limit_exception();
	FCEUI_DisableSpriteLimitation(g_no_sprite_limit && !g_sprite_exception);
}

/* light gun / paddle (Zapper on a controller port, Arkanoid Vaus on port 2 or the Famicom expansion port):
 * the core reads { x, y, trigger, sensor } as four uint32 */
static uint32_t g_gun[4];
static int g_opt_zapper = 0, g_opt_paddle = 0;  /* 0 = by the game database, 1 = force on (port 2), 2 = off */
static int g_gun_port = -1, g_paddle_kind;      /* Zapper port (0/1) or -1; paddle: 0 none, 1 NES port 2, 2 Famicom expansion */
extern void FCEU_ZapperSetTolerance(int t);
static int g_ncheats;
static size_t g_state_size;

static uint16_t g_pal[1024];                /* RGB565 for the core's palette indices (0..767 used) */
static uint16_t g_fb[NES_WIDTH * NES_HEIGHT];
static const uint8_t *g_gfx;                /* the core's index buffer of the last drawn frame */

static unsigned g_rate = NES_AUDIO_RATE;
static int16_t *g_aring;                    /* stereo frames, a plain FIFO */
static size_t g_ar_cap, g_ar_r, g_ar_n;     /* capacity, read position and fill, in int16 units */
static int16_t *g_atmp;
static size_t g_atmp_cap;

/* ---- variables FCEUmm's core expects its driver to define -------------------------------- */
unsigned overclock_enabled = 0;
unsigned overclocked = 0;
unsigned skip_7bit_overclocking = 1;
unsigned totalscanlines = 0;
unsigned normal_scanlines = 240;
unsigned extrascanlines = 0;
unsigned vblankscanlines = 0;
unsigned dendy = 0;
unsigned swapDuty = 0;

const char *GetKeyboard(void) { return ""; }

void FCEUD_SetPalette(uint16_t index, uint8_t r, uint8_t g, uint8_t b)
{
	if (index < 1024) g_pal[index] = (uint16_t) (((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

void FCEUD_PrintError(const char *s)
{
	snprintf(g_msg, sizeof g_msg, "%s", s);
	if (g_log) g_log(s);
}

void FCEUD_Message(const char *s) { if (g_log) g_log(s); }

void FCEUD_DispMessage(enum retro_log_level level, unsigned duration, const char *str)
{
	(void) level; (void) duration;
	snprintf(g_msg, sizeof g_msg, "%s", str);
	if (g_log) g_log(str);
}

/* ---- helpers ------------------------------------------------------------------------------ */

static void set_title(const char *name)
{
	const char *s = name ? name : "";
	const char *b = s, *p;
	for (p = s; *p; p++) if (*p == '/' || *p == ':' || *p == '\\') b = p + 1;
	snprintf(g_title, sizeof g_title, "%s", b);
	char *d = strrchr(g_title, '.');
	if (d && d != g_title) *d = 0;
}

static int ring_alloc(void)
{
	size_t cap = (size_t) g_rate * 2;   /* one second of stereo frames */
	int16_t *r = (int16_t *) calloc(cap, sizeof(int16_t));
	if (!r) return 1;
	free(g_aring);
	g_aring = r;
	g_ar_cap = cap;
	g_ar_r = g_ar_n = 0;
	return 0;
}

/* the oldest audio is dropped when the frontend does not drain (never blocks) */
static void ring_put(const int16_t *src, size_t n)
{
	size_t i, w;
	if (!g_aring) return;
	if (n > g_ar_cap) { src += n - g_ar_cap; n = g_ar_cap; }
	while (g_ar_n + n > g_ar_cap) { g_ar_r = (g_ar_r + 2) % g_ar_cap; g_ar_n -= 2; }
	w = (g_ar_r + g_ar_n) % g_ar_cap;
	for (i = 0; i < n; i++) {
		g_aring[w] = src[i];
		if (++w == g_ar_cap) w = 0;
	}
	g_ar_n += n;
}

/* The save RAM as FCEUmm's libretro core exposes it (RETRO_MEMORY_SAVE_RAM): the battery-backed PRG RAM of an
 * iNES board, else of a UNIF board, else (disk images) the whole disk. */
static uint8_t *sram_ptr(size_t *len)
{
	*len = 0;
	if (!g_loaded) return NULL;
	if (iNESCart.battery && iNESCart.SaveGame[0] && iNESCart.SaveGameLen[0]) {
		*len = (size_t) iNESCart.SaveGameLen[0];
		return iNESCart.SaveGame[0];
	}
	if (UNIFCart.battery && UNIFCart.SaveGame[0] && UNIFCart.SaveGameLen[0]) {
		*len = (size_t) UNIFCart.SaveGameLen[0];
		return UNIFCart.SaveGame[0];
	}
	if (GameInfo && GameInfo->type == GIT_FDS) {
		*len = (size_t) FDSROM_size();
		return FDSROM_ptr();
	}
	return NULL;
}

static void apply_region(void)
{
	unsigned d = 0, pal = 0;
	switch (g_opt_region) {
	case NES_REGION_AUTO: d = (g_detected >> 1) & 1; pal = g_detected & 1; break;
	case NES_REGION_NTSC: break;
	case NES_REGION_PAL: pal = 1; break;
	case NES_REGION_DENDY: d = 1; break;
	}
	dendy = d;
	FCEUI_SetVidSystem((int) pal);   /* also redoes the palette and the sound timing */
}

static void apply_fourscore(void)
{
	int fs = 0, fc = 0;
	switch (g_4p_mode) {
	case NES_4P_AUTO: fs = g_db_fs; fc = g_db_fc; break;
	case NES_4P_ON: if (g_db_fc) fc = 1; else fs = 1; break;
	default: break;
	}
	FCEUI_DisableFourScore(fs ? 0 : 1);
	if (fc) {
		FCEUI_SetInputFC(SIFC_4PLAYER, &g_joy, 0);
		g_fc_set = 1;
	} else if (g_fc_set) {
		FCEUI_SetInputFC(GameInfo && GameInfo->inputfc > 0 && GameInfo->inputfc != SIFC_4PLAYER ? GameInfo->inputfc : SIFC_NONE,
		    &g_dummy, 0);
		g_fc_set = 0;
	}
}

int nes_host_is_fds_image(const uint8_t *p, size_t n)
{
	if (n >= 4 && !memcmp(p, "FDS\x1a", 4)) return 1;
	return n >= 15 && !memcmp(p + 1, "*NINTENDO-HVC*", 14);
}

/* ---- lifetime ----------------------------------------------------------------------------- */

int nes_host_init(void)
{
	if (g_inited) return 0;
	option_ramstate = 0;
	if (!FCEUI_Initialize()) return 1;
	FSettings.SquareVolume[0] = FSettings.SquareVolume[1] = 256;
	FSettings.TriangleVolume = FSettings.NoiseVolume = FSettings.PCMVolume = 256;
	FCEUI_SetSoundVolume(150);
	FCEU_ZapperSetTolerance(6);   /* a touch is not a pixel: the same default as the libretro core's option */
	FCEUI_SetSoundQuality(0);
	FCEUI_Sound((int) g_rate);
	if (ring_alloc()) return 1;
	g_inited = 1;
	return 0;
}

void nes_host_shutdown(void)
{
	if (!g_inited) return;
	nes_host_unload();
	FCEUI_Sound(0);
	FCEUI_Kill();
	free(g_aring); g_aring = NULL; g_ar_cap = 0;
	free(g_atmp); g_atmp = NULL; g_atmp_cap = 0;
	free(g_sram_saved); g_sram_saved = NULL; g_sram_saved_n = 0;
	g_inited = 0;
}

void nes_host_set_fds_bios(const uint8_t *bios, size_t n)
{
	g_have_bios = bios && n == sizeof g_bios;
	if (g_have_bios) memcpy(g_bios, bios, sizeof g_bios);
}

static void fail(char *err, size_t errlen, const char *s)
{
	if (err && errlen) snprintf(err, errlen, "%s", s);
}

int nes_host_load_mem(const uint8_t *data, size_t n, const char *name, char *err, size_t errlen)
{
	if (!g_inited) { fail(err, errlen, "not initialised"); return 1; }
	nes_host_unload();
	if (!data || n < 16 || n > MAX_ROM_BYTES) { fail(err, errlen, "Cannot read the ROM file"); return 2; }
	if (n >= 5 && !memcmp(data, "NESM\x1a", 5)) { fail(err, errlen, "NSF music files are not supported"); return 3; }
	g_unif = n >= 4 && !memcmp(data, "UNIF", 4);
	if (nes_host_is_fds_image(data, n)) {
		if (!g_have_bios) { fail(err, errlen, "Disk System game: put disksys.rom (the FDS BIOS) next to the game"); return 4; }
		FCEU_FDSSetBIOS(g_bios);
	}

	g_msg[0] = 0;
	dendy = 0;
	g_fc_set = 0;
	GameInfo = (FCEUGI *) FCEUI_LoadGame(name ? name : "", data, n, NULL);
	FCEU_FDSSetBIOS(NULL);
	if (!GameInfo) {
		char *e;
		size_t i;
		fail(err, errlen, g_msg[0] ? g_msg : "Not a valid NES ROM");
		if (err && errlen) {
			/* the core's text begins with a space and may end in a new line */
			e = err;
			while (*e == ' ') memmove(e, e + 1, strlen(e));
			for (i = strlen(err); i > 0 && (err[i - 1] == '\n' || err[i - 1] == ' '); i--) err[i - 1] = 0;
		}
		return 3;
	}
	g_loaded = 1;
	set_title(name);

	/* the controllers: two pads on the console; a Four Score or the Famicom adapter adds pads 3 and 4 */
	g_joy = 0;
	{
		int i;
		for (i = 0; i < 2; i++) FCEUI_SetInput(i, SI_GAMEPAD, &g_joy, 0);
		/* a Zapper (database or forced) replaces the pad of its port; the aim is set with nes_host_set_zapper */
		g_gun_port = -1;
		g_paddle_kind = 0;
		memset(g_gun, 0, sizeof g_gun);
		g_gun[0] = g_gun[1] = 0x7FFF;
		if (g_opt_zapper != 2) {
			for (i = 0; i < 2; i++) if (GameInfo->input[i] == SI_ZAPPER) g_gun_port = i;
			if (g_gun_port < 0 && g_opt_zapper == 1) g_gun_port = 1;
		}
		if (g_gun_port >= 0) FCEUI_SetInput(g_gun_port, SI_ZAPPER, g_gun, 0);
		else if (g_opt_paddle != 2) {
			if (GameInfo->input[1] == SI_ARKANOID || (g_opt_paddle == 1 && GameInfo->inputfc != SIFC_ARKANOID)) {
				g_paddle_kind = 1;
				FCEUI_SetInput(1, SI_ARKANOID, g_gun, 0);
			} else if (GameInfo->inputfc == SIFC_ARKANOID) {
				g_paddle_kind = 2;
			}
		}
	}
	g_db_fs = g_db_fc = 0;
	if (GameInfo->type == GIT_CART || GameInfo->type == GIT_FDS) {
		size_t i;
		for (i = 0; i < sizeof fourscore_db_list / sizeof fourscore_db_list[0]; i++)
			if (fourscore_db_list[i].crc == iNESCart.CRC32) { g_db_fs = 1; break; }
		for (i = 0; i < sizeof famicom_4p_db_list / sizeof famicom_4p_db_list[0]; i++)
			if (famicom_4p_db_list[i].crc == iNESCart.CRC32) { g_db_fc = 1; break; }
	}
	g_detected = (dendy << 1) | (FSettings.PAL ? 1 : 0);
	apply_region();
	apply_sprite_limit();
	PowerNES();
	apply_fourscore();
	if (g_paddle_kind == 2 && !g_fc_set) FCEUI_SetInputFC(SIFC_ARKANOID, g_gun, 0);

	/* the fixed size of this game's states */
	{
		uint8_t *tmp = (uint8_t *) malloc(STATE_SCRATCH);
		size_t sz = tmp ? FCEUSS_Save_Mem(tmp, STATE_SCRATCH) : 0;
		free(tmp);
		g_state_size = sz && sz < STATE_SCRATCH ? sz + STATE_HEADROOM : 0;
	}

	free(g_sram_saved);
	g_sram_saved = NULL; g_sram_saved_n = 0;
	g_gfx = NULL;
	memset(g_fb, 0, sizeof g_fb);
	g_ar_r = g_ar_n = 0;
	snprintf(g_info, sizeof g_info, "%s", nes_host_rom_info());
	return 0;
}

void nes_host_unload(void)
{
	if (g_loaded) {
		nes_host_cheat_reset();
		FCEUI_CloseGame();
		GameInfo = NULL;
	}
	g_loaded = 0;
	g_fc_set = 0;
	g_state_size = 0;
}

/* ---- frames ------------------------------------------------------------------------------- */

void nes_host_set_pad(int port, unsigned mask)
{
	if (port < 0 || port > 3) return;
	mask &= 0xFF;
	if ((mask & NES_UP) && (mask & NES_DOWN)) mask &= ~(unsigned) (NES_UP | NES_DOWN);
	if ((mask & NES_LEFT) && (mask & NES_RIGHT)) mask &= ~(unsigned) (NES_LEFT | NES_RIGHT);
	g_pad[port] = mask;
	g_joy = g_pad[0] | (g_pad[1] << 8) | (g_pad[2] << 16) | ((uint32_t) g_pad[3] << 24);
}

int nes_host_sprite_exception_add(uint32_t crc)
{
	if (!crc || g_sprite_keep_n >= sizeof g_sprite_keep_extra / sizeof g_sprite_keep_extra[0]) return 1;
	g_sprite_keep_extra[g_sprite_keep_n++] = crc;
	if (g_loaded) apply_sprite_limit();
	return 0;
}

int nes_host_sprite_limit_exception(void) { return g_loaded && g_sprite_exception; }

void nes_host_set_zapper(int x, int y, int trigger)
{
	if (x < 0 || y < 0 || x > 255 || y > 239) x = y = 0x7FFF;   /* aimed off the screen: never sees light */
	g_gun[0] = (uint32_t) x;
	g_gun[1] = (uint32_t) y;
	g_gun[2] = trigger ? 1 : 0;
	g_gun[3] = 0;
}

int nes_host_zapper_active(void) { return g_loaded && g_gun_port >= 0; }

void nes_host_set_paddle(int x, int button)
{
	g_gun[0] = (uint32_t) (x < 0 ? 0 : x > 255 ? 255 : x);
	g_gun[2] = button ? 1 : 0;
}

int nes_host_paddle_active(void) { return g_loaded && g_paddle_kind != 0; }

void nes_host_set_fourscore(int mode)
{
	g_4p_mode = mode < 0 || mode > 2 ? 0 : mode;
	if (g_loaded) apply_fourscore();
}
int nes_host_fourscore_mode(void) { return g_4p_mode; }
int nes_host_fourscore_active(void)
{
	if (!g_loaded) return 0;
	return g_4p_mode == NES_4P_ON || (g_4p_mode == NES_4P_AUTO && (g_db_fs || g_db_fc)) ? 1 : 0;
}

void nes_host_run_frame(void)
{
	uint8_t *gfx = NULL;
	int32_t *snd = NULL;
	int32_t ssize = 0;
	if (!g_loaded) return;
	FCEUI_Emulate(&gfx, &snd, &ssize, g_render ? 0 : 1);

	if (gfx) {
		int y, x;
		g_gfx = gfx;
		for (y = 0; y < NES_HEIGHT; y++) {
			const uint8_t *src = gfx + y * 256;
			uint16_t *dst = g_fb + y * NES_WIDTH;
			unsigned deemp = GameInfo->type == GIT_NSF ? 0 : XDBuf[y * 256];
			if (deemp) {
				const uint16_t *pal = g_pal + 256 + ((deemp & 7) << 6);
				for (x = 0; x < NES_WIDTH; x++) dst[x] = pal[src[x] & 0x3F];
			} else {
				for (x = 0; x < NES_WIDTH; x++) dst[x] = g_pal[src[x]];
			}
		}
	}
	if (ssize > 0 && snd) {
		int i;
		if (g_atmp_cap < (size_t) ssize * 2) {
			int16_t *t = (int16_t *) realloc(g_atmp, (size_t) ssize * 2 * sizeof(int16_t) + 1024);
			if (!t) return;
			g_atmp = t;
			g_atmp_cap = (size_t) ssize * 2 + 512;
		}
		for (i = 0; i < ssize; i++) {
			int32_t v = snd[i];
			if (v > 32767) v = 32767; else if (v < -32768) v = -32768;
			if (!g_audio_on) v = 0;
			g_atmp[i * 2] = g_atmp[i * 2 + 1] = (int16_t) v;
		}
		ring_put(g_atmp, (size_t) ssize * 2);
	}
}

void nes_host_reset(void) { if (g_loaded) ResetNES(); }
void nes_host_set_render(int on) { g_render = on != 0; }

/* ---- video -------------------------------------------------------------------------------- */

const uint16_t *nes_host_video(int *w, int *h, int *pitch)
{
	if (w) *w = NES_WIDTH;
	if (h) *h = NES_HEIGHT;
	if (pitch) *pitch = NES_WIDTH * 2;
	return g_fb;
}

void nes_host_overscan(int *top, int *bottom)
{
	int t = nes_host_is_pal() ? 0 : 8;
	if (top) *top = t;
	if (bottom) *bottom = t;
}

void nes_host_visible_rows(int *first, int *last)
{
	if (first) *first = 8;
	if (last) *last = 231;
}

const uint16_t *nes_host_palette(int *entries)
{
	if (entries) *entries = 768;
	return g_pal;
}

int nes_host_video_indexed(const uint8_t **indices, const uint8_t **emphasis)
{
	if (indices) *indices = g_gfx;
	if (emphasis) *emphasis = XDBuf;
	return g_gfx != NULL;
}

/* ---- audio -------------------------------------------------------------------------------- */

int nes_host_set_audio_rate(unsigned hz)
{
	if (hz < 8000 || hz > 96000) return 1;
	g_rate = hz;
	if (g_inited) {
		FCEUI_Sound((int) hz);
		if (ring_alloc()) return 1;
	}
	return 0;
}
unsigned nes_host_audio_rate(void) { return g_rate; }

int nes_host_audio_take(int16_t *dst, int max_frames)
{
	size_t i, want = (size_t) (max_frames < 0 ? 0 : max_frames) * 2;
	if (want > g_ar_n) want = g_ar_n & ~(size_t) 1;
	for (i = 0; i < want; i++) {
		dst[i] = g_aring[g_ar_r];
		if (++g_ar_r == g_ar_cap) g_ar_r = 0;
	}
	g_ar_n -= want;
	return (int) (want / 2);
}

void nes_host_set_audio_enabled(int on) { g_audio_on = on != 0; }

/* ---- battery RAM -------------------------------------------------------------------------- */

size_t nes_host_sram_size(void)
{
	size_t n;
	return sram_ptr(&n) ? n : 0;
}

uint8_t *nes_host_sram_data(void)
{
	size_t n;
	return sram_ptr(&n);
}

void nes_host_sram_mark_saved(void)
{
	size_t sz;
	uint8_t *ram = sram_ptr(&sz);
	uint8_t *c;
	if (!ram || !sz) return;
	c = (uint8_t *) realloc(g_sram_saved, sz);
	if (!c) { free(g_sram_saved); g_sram_saved = NULL; g_sram_saved_n = 0; return; }
	memcpy(c, ram, sz);
	g_sram_saved = c;
	g_sram_saved_n = sz;
}

void nes_host_sram_mark_unsaved(void)
{
	free(g_sram_saved);
	g_sram_saved = NULL;
	g_sram_saved_n = 0;
}

size_t nes_host_sram_load(const uint8_t *data, size_t n)
{
	size_t sz, c;
	uint8_t *ram = sram_ptr(&sz);
	if (!ram || !sz) return 0;
	c = data && n ? (n < sz ? n : sz) : 0;
	if (c) memcpy(ram, data, c);
	nes_host_sram_mark_saved();
	return c;
}

int nes_host_sram_dirty(void)
{
	size_t sz;
	uint8_t *ram = sram_ptr(&sz);
	if (!ram || !sz) return 0;
	return g_sram_saved_n != sz || !g_sram_saved || memcmp(g_sram_saved, ram, sz) != 0;
}

/* ---- states ------------------------------------------------------------------------------- */

size_t nes_host_state_size(void) { return g_loaded ? g_state_size : 0; }

size_t nes_host_state_save_mem(uint8_t *buf, size_t cap)
{
	size_t n;
	if (!g_loaded || !g_state_size || cap < g_state_size) return 0;
	if (geniestage == 1) return 0;
	n = FCEUSS_Save_Mem(buf, g_state_size);
	if (!n || n > g_state_size) return 0;   /* the state outgrew the size measured at load */
	memset(buf + n, 0, g_state_size - n);
	return g_state_size;
}

int nes_host_state_load_mem(const uint8_t *buf, size_t n)
{
	if (!g_loaded) return 1;
	if (n < 16 || memcmp(buf, "FCS", 3) != 0 || geniestage == 1) return 3;
	return FCEUSS_Load_Mem(buf, n) ? 0 : 3;
}

/* ---- memory ------------------------------------------------------------------------------- */

int nes_host_peek(unsigned addr)
{
	if (!g_loaded) return -1;
	addr &= 0xFFFF;
	return ARead[addr](addr);
}

const uint8_t *nes_host_ram(size_t *size)
{
	if (size) *size = g_loaded ? sizeof RAM : 0;
	return g_loaded ? RAM : NULL;
}

int nes_host_ra_read(unsigned addr, uint8_t *out, unsigned n)
{
	unsigned i, got = 0;
	if (!g_loaded || !out) return 0;
	for (i = 0; i < n; i++) {
		unsigned a = addr + i;
		if (a < 0x800) out[i] = RAM[a];
		else if (a >= 0x6000 && a < 0x8000) out[i] = (uint8_t) ARead[a](a);
		else break;
		got++;
	}
	return (int) got;
}

/* ---- cheats ------------------------------------------------------------------------------- */

static int gg_valid(const char *s)
{
	size_t i, n = strlen(s);
	if (n != 6 && n != 8) return 0;
	for (i = 0; i < n; i++)
		if (!strchr("APZLGITYEOXUKSVN", toupper((unsigned char) s[i]))) return 0;
	return 1;
}

int nes_host_cheat_add(const char *codes)
{
	char temp[1024];
	char *part, *save = NULL;
	int ok = 0;
	if (!g_loaded || !codes) return 0;
	snprintf(temp, sizeof temp, "%s", codes);
	for (part = strtok_r(temp, "+,;._ ", &save); part; part = strtok_r(NULL, "+,;._ ", &save)) {
		size_t len = strlen(part);
		uint16_t a;
		uint8_t v;
		int c, type = 1;
		if (len == 7 && part[4] == ':') {
			part[4] = 0;
			a = (uint16_t) strtoul(part, NULL, 16);
			v = (uint8_t) strtoul(part + 5, NULL, 16);
			if (a < 0x100) type = 0;   /* zero page does not go through the read handlers */
			if (FCEUI_AddCheat("N/A", a, v, -1, type)) ok++;
		} else if (len == 10 && part[4] == '?' && part[7] == ':') {
			part[4] = 0; part[7] = 0;
			a = (uint16_t) strtoul(part, NULL, 16);
			c = (int) strtoul(part + 5, NULL, 16);
			v = (uint8_t) strtoul(part + 8, NULL, 16);
			if (a < 0x100) type = 0;
			if (FCEUI_AddCheat("N/A", a, v, c, type)) ok++;
		} else if (gg_valid(part) && FCEUI_DecodeGG(part, &a, &v, &c)) {
			if (FCEUI_AddCheat("N/A", a, v, c, 1)) ok++;
		} else if (FCEUI_DecodePAR(part, &a, &v, &c, &type)) {
			if (FCEUI_AddCheat("N/A", a, v, c, type)) ok++;
		}
	}
	g_ncheats += ok;
	return ok;
}

void nes_host_cheat_reset(void)
{
	FCEU_ResetCheats();
	g_ncheats = 0;
}

int nes_host_cheat_count(void) { return g_ncheats; }

/* ---- options and info --------------------------------------------------------------------- */

int nes_host_set_option(const char *name, int v)
{
	if (!strcmp(name, "region")) {
		g_opt_region = v < 0 || v > 3 ? 0 : v;
		if (g_loaded) apply_region();
	} else if (!strcmp(name, "no_sprite_limit")) {
		g_no_sprite_limit = v ? 1 : 0;
		if (g_inited) apply_sprite_limit();
	} else if (!strcmp(name, "zapper")) {
		g_opt_zapper = v < 0 || v > 2 ? 0 : v;       /* takes effect at the next load */
	} else if (!strcmp(name, "paddle")) {
		g_opt_paddle = v < 0 || v > 2 ? 0 : v;
	} else if (!strcmp(name, "fourscore")) {
		nes_host_set_fourscore(v);
	} else if (!strcmp(name, "ram_init")) {
		option_ramstate = v < 0 || v > 2 ? 0 : v;
	} else if (!strcmp(name, "sound_quality")) {
		FCEUI_SetSoundQuality(v < 0 || v > 2 ? 0 : v);
	} else if (!strcmp(name, "lowpass")) {
		FCEUI_SetLowPass(v ? 1 : 0);
	} else {
		return 1;
	}
	return 0;
}

int nes_host_is_pal(void) { return (FSettings.PAL || dendy) ? 1 : 0; }
int nes_host_region(void) { return dendy ? NES_REGION_DENDY : FSettings.PAL ? NES_REGION_PAL : NES_REGION_NTSC; }
int nes_host_region_detected(void)
{
	return (g_detected & 2) ? NES_REGION_DENDY : (g_detected & 1) ? NES_REGION_PAL : NES_REGION_NTSC;
}
double nes_host_fps(void) { return nes_host_is_pal() ? 838977920.0 / 16777216.0 : 1008307711.0 / 16777216.0; }

const char *nes_host_rom_title(void) { return g_title; }

uint32_t nes_host_rom_crc(void) { return g_loaded && GameInfo && GameInfo->type == GIT_CART ? iNESCart.CRC32 : 0; }

const char *nes_host_rom_info(void)
{
	static char buf[240];
	const char *reg = nes_host_is_pal() ? (dendy ? "Dendy" : "PAL") : "NTSC";
	if (!g_loaded || !GameInfo) { buf[0] = 0; return buf; }
	if (GameInfo->type == GIT_FDS) {
		int side, sides, ins;
		FCEU_FDSInfo(&side, &sides, &ins);
		snprintf(buf, sizeof buf, "FDS disk image, %d side%s, %s %s", sides, sides == 1 ? "" : "s", reg, g_title);
		return buf;
	}
	if (g_unif)
		snprintf(buf, sizeof buf, "UNIF prg=%uK chr=%uK battery=%d sram=%uB %s %s", (unsigned) (UNIFCart.PRGRomSize / 1024),
		    (unsigned) (UNIFCart.CHRRomSize / 1024), UNIFCart.battery ? 1 : 0, (unsigned) nes_host_sram_size(), reg, g_title);
	else
		snprintf(buf, sizeof buf, "%s mapper=%d prg=%uK chr=%uK mirror=%d battery=%d sram=%uB %s crc=%08x %s",
		    iNESCart.iNES2 ? "NES2.0" : "iNES", iNESCart.mapper, (unsigned) (iNESCart.PRGRomSize / 1024),
		    (unsigned) (iNESCart.CHRRomSize / 1024), iNESCart.mirror, iNESCart.battery ? 1 : 0, (unsigned) nes_host_sram_size(),
		    reg, (unsigned) iNESCart.CRC32, g_title);
	return buf;
}

int nes_host_is_fds(void) { return g_loaded && GameInfo && GameInfo->type == GIT_FDS; }

void nes_host_fds_info(int *side, int *sides, int *inserted)
{
	int a = 0, b = 0, c = 0;
	if (nes_host_is_fds()) FCEU_FDSInfo(&a, &b, &c);
	if (side) *side = a;
	if (sides) *sides = b;
	if (inserted) *inserted = c;
}
void nes_host_fds_toggle_disk(void) { if (nes_host_is_fds()) FCEU_FDSInsert(-1); }
void nes_host_fds_next_side(void) { if (nes_host_is_fds()) FCEU_FDSSelect(); }

int nes_host_take_message(char *out, size_t cap)
{
	if (!g_msg[0]) return 0;
	snprintf(out, cap, "%s", g_msg);
	g_msg[0] = 0;
	return 1;
}

void nes_host_set_log(void (*fn)(const char *line)) { g_log = fn; }
