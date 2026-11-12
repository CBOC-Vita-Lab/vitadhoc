/*
 * WonderSwan host glue (see host.h). Replaces libretro.c of beetle-wswan-libretro: the load, reset, frame, state and
 * memory logic below is adapted from it (GPL-2.0-or-later); the API, save, orientation and audio ring code is new.
 * No C++ runtime is used (no exceptions, RTTI, new/delete, statics with constructors) so the object links like C.
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

extern "C" {
#include "mednafen/mednafen-types.h"
#include "mednafen/settings.h"
#include "mednafen/git.h"
#include "mednafen/state.h"
#include "mednafen/mempatcher.h"
#include "mednafen/mempatcher-driver.h"
#include "mednafen/wswan/wswan.h"
#include "mednafen/wswan/gfx.h"
#include "mednafen/wswan/interrupt.h"
#include "mednafen/wswan/wswan-memory.h"
#include "mednafen/wswan/sound.h"
#include "mednafen/wswan/v30mz.h"
#include "mednafen/wswan/rtc.h"
#include "mednafen/wswan/comm.h"
#include "mednafen/wswan/eeprom.h"
#include "mednafen/state_inline.h"
#include "mednafen/wswan/start.inc"
}
#include "host.h"

/* globals the core expects its host to define (libretro.c defines them upstream) */
extern "C" {
int wsc = 1;
uint32 rom_size;
uint16 WSButtonStatus;
int StateAction(StateMem *sm, int load, int data_only);
time_t wswan_host_time_hook(time_t *t);
}

#define MAX_TABLE 64
#define STATE_HEADROOM 1024
#define IEEPROM_SIZE 0x400
#define SWITCH_FRAMES 6        /* key group evidence: 80 ms */
#define SWITCH_FRAMES_ICON 2   /* LCD icon change: deliberate, switch before the next scene's first picture */

struct TableEntry { unsigned dev, cart; int mode; };

static struct {
	int inited, loaded;
	uint8 footer[16];
	uint32 rom_crc;
	char info[200];
	char name[64];
	uint32 sram_size;
	int ww;
	int render, audio_on;
	unsigned rate;
	unsigned pad;
	uint64_t frames;
	MDFN_Surface surf;
	uint16 pixels[WSWAN_WIDTH * WSWAN_HEIGHT];
	uint32 mono_dark, mono_light;
	/* audio */
	int16 *abuf; int32 abuf_cap;
	int16 *ring; unsigned ring_cap, ring_head, ring_n;     /* frames */
	/* state */
	size_t state_size; uint8_t *state_scratch;
	/* saves */
	uint8_t *shadow[3]; size_t shadow_n[3];
	/* orientation */
	int mode, detected, source, cand, cand_n, hdr_vertical;
	int cand_src;                       /* the evidence (KEYS / ICON) the candidate run was built from */
	unsigned switches, since_switch;
	int fr_x, fr_y, fr_b, last_keys, last_icon;
	TableEntry table[MAX_TABLE]; int table_n;
	int table_mode;
	/* owner */
	char owner_name[17]; int oy, om, od, osex, oblood;
	time_t (*time_src)(void);
	void (*log)(const char *);
	int cheats;
	/* real port functions */
	void (*real_wp)(uint32, uint8);
} G;

static void wp_hook(uint32 port, uint8 v)
{
	if ((port & 0xFF) == 0xB5) {
		if (v & 0x10) G.fr_y++;
		if (v & 0x20) G.fr_x++;
		if (v & 0x40) G.fr_b++;
	}
	G.real_wp(port, v);
}

extern "C" time_t wswan_host_time_hook(time_t *t)
{
	time_t v = G.time_src ? G.time_src() : time(NULL);
	if (t) *t = v;
	return v;
}

static uint32_t crc32_buf(const uint8_t *d, size_t n)
{
	static uint32_t tab[256]; static int ok;
	if (!ok) {
		for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1; tab[i] = c; }
		ok = 1;
	}
	uint32_t c = 0xFFFFFFFFu;
	for (size_t i = 0; i < n; i++) c = tab[(c ^ d[i]) & 0xFF] ^ (c >> 8);
	return ~c;
}

static void errset(char *err, size_t n, const char *m) { if (err && n) { snprintf(err, n, "%s", m); } }

/* ---- owner ----------------------------------------------------------------------------------------------- */

static void owner_defaults(void)
{
	snprintf(G.owner_name, sizeof G.owner_name, "%s", "VITADHOC");
	G.oy = 1999; G.om = 3; G.od = 4; G.osex = 1; G.oblood = 3;
}
static void owner_to_settings(void)
{
	snprintf(setting_wswan_name, sizeof setting_wswan_name, "%s", G.owner_name);
	setting_wswan_byear = G.oy; setting_wswan_bmonth = G.om; setting_wswan_bday = G.od;
	setting_wswan_sex = G.osex; setting_wswan_blood = G.oblood;
}
static uint8 bcd(int v) { return (uint8)(((v / 10) << 4) | (v % 10)); }
void wswan_host_set_owner(const char *name, int year, int month, int day, int sex, int blood)
{
	if (name) snprintf(G.owner_name, sizeof G.owner_name, "%s", name);
	if (year >= 1900 && year <= 2099) G.oy = year;
	if (month >= 1 && month <= 12) G.om = month;
	if (day >= 1 && day <= 31) G.od = day;
	if (sex == 1 || sex == 2) G.osex = sex;
	if (blood >= 1 && blood <= 4) G.oblood = blood;
	owner_to_settings();
}
extern "C" void wswan_host_owner_apply(void)
{
	/* the same encoding as the core's WSwan_EEPROMInit */
	uint8_t *ie = WSwan_GetInternalEEPROM();
	if (!G.loaded) return;
	for (unsigned x = 0; x < 16; x++) {
		uint8_t z = 0;
		if (x < strlen(G.owner_name)) {
			char c = G.owner_name[x];
			if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
			if (c == ' ') z = 0;
			else if (c >= '0' && c <= '9') z = c - '0' + 1;
			else if (c >= 'A' && c <= 'Z') z = c - 'A' + 0xB;
		}
		ie[0x360 + x] = z;
	}
	ie[0x370] = bcd(G.oy / 100); ie[0x371] = bcd(G.oy % 100);
	ie[0x372] = bcd(G.om); ie[0x373] = bcd(G.od); ie[0x374] = (uint8_t)G.osex; ie[0x375] = (uint8_t)G.oblood;
}

/* ---- lifetime -------------------------------------------------------------------------------------------- */

extern "C" int wswan_host_init(void)
{
	if (G.inited) return 0;
	memset(&G, 0, sizeof G);
	G.inited = 1; G.render = 1; G.audio_on = 1; G.rate = WSWAN_AUDIO_RATE;
	G.mono_dark = 0x000000; G.mono_light = 0xFFFFFF;
	setting_wswan_language = 1;
	owner_defaults(); owner_to_settings();
	G.ring_cap = 0;
	return 0;
}

static void free_game(void)
{
	if (!G.loaded) return;
	MDFN_FlushGameCheats(0);
	WSwan_MemoryKill();
	WSwan_SoundKill();
	if (wsCartROM) { free(wsCartROM); wsCartROM = NULL; }
	MDFNMP_Kill();
	free(G.abuf); G.abuf = NULL; G.abuf_cap = 0;
	free(G.state_scratch); G.state_scratch = NULL;
	for (int i = 0; i < 3; i++) { free(G.shadow[i]); G.shadow[i] = NULL; G.shadow_n[i] = 0; }
	G.loaded = 0; G.cheats = 0;
}
extern "C" void wswan_host_unload(void) { free_game(); }
extern "C" void wswan_host_shutdown(void)
{
	free_game();
	free(G.ring); G.ring = NULL; G.ring_cap = 0;
	G.inited = 0;
}

static void core_reset(void)
{
	v30mz_reset();
	WSwan_MemoryReset();
	Comm_Reset();
	WSwan_GfxReset();
	WSwan_SoundReset();
	WSwan_InterruptReset();
	WSwan_RTCReset();
	WSwan_EEPROMReset();
	for (int u0 = 0; u0 < 0xc8; u0++)
		if (u0 != 0xC4 && u0 != 0xC5 && u0 != 0xBA && u0 != 0xBB) WSwan_writeport(u0, startio[u0]);
	v30mz_set_reg(NEC_SS, 0);
	v30mz_set_reg(NEC_SP, 0x2000);
}

static size_t measure_state(void)
{
	StateMem st; memset(&st, 0, sizeof st);
	if (!MDFNSS_SaveSM(&st, 0, 0, NULL, NULL, NULL)) return 0;
	free(st.data);
	return st.len;
}

static void shadow_take(int r)
{
	size_t n = wswan_host_save_size(r);
	free(G.shadow[r]); G.shadow[r] = NULL; G.shadow_n[r] = 0;
	if (!n) return;
	G.shadow[r] = (uint8_t *)malloc(n);
	if (G.shadow[r]) { memcpy(G.shadow[r], wswan_host_save_data(r), n); G.shadow_n[r] = n; }
}

static void orient_reset(void)
{
	int m = 0;
	G.source = WSWAN_SRC_HEADER;
	G.hdr_vertical = G.footer[12] & 1;
	G.detected = G.hdr_vertical ? WSWAN_VERTICAL : WSWAN_HORIZONTAL;
	G.cand = G.detected; G.cand_n = 0; G.cand_src = -1; G.switches = 0; G.since_switch = 0;
	G.fr_x = G.fr_y = G.fr_b = 0; G.last_keys = 0; G.last_icon = 0;
	G.table_mode = 0;
	for (int i = 0; i < G.table_n; i++)
		if (G.table[i].dev == G.footer[6] && G.table[i].cart == G.footer[8]) m = G.table[i].mode;
	if (m) { G.table_mode = m; G.detected = (m == WSWAN_ORIENT_VERTICAL) ? WSWAN_VERTICAL : WSWAN_HORIZONTAL; G.source = WSWAN_SRC_TABLE; }
}

extern "C" int wswan_host_load_mem(const uint8_t *data, size_t size, const char *name, char *err, size_t errlen)
{
	if (!G.inited) { errset(err, errlen, "not initialised"); return 1; }
	free_game();
	if (size < 65536 || size > (16u << 20)) { errset(err, errlen, "not a WonderSwan image (size)"); return 2; }
	uint32 real = (uint32)((size + 0xFFFF) & ~0xFFFFu), pow = 1;
	while (pow < real) pow <<= 1;
	rom_size = pow;
	wsCartROM = (uint8 *)calloc(1, rom_size);
	if (!wsCartROM) { errset(err, errlen, "out of memory"); return 4; }
	if (real < rom_size) memset(wsCartROM, 0xFF, rom_size - real);
	memcpy(wsCartROM + (rom_size - real), data, size);
	memcpy(G.footer, wsCartROM + rom_size - 16, 16);
	uint8 *h = G.footer + 6;      /* the 10 bytes libretro.c calls header[] */
	G.rom_crc = crc32_buf(data, size);
	snprintf(G.name, sizeof G.name, "%s", name ? name : "");

	/* WonderWitch firmware detection as upstream: 512 KiB, "ELISA" font signature, known footer checksum */
	G.ww = 0;
	if (rom_size == 524288 && !memcmp(&wsCartROM[0x70000], "ELISA", 5) && crc32_buf(&wsCartROM[0x7FFF0], 0x10) == 0x0d05ed64u) {
		static const uint32_t block[3] = { 0x63f00316u, 0x60fd569bu, 0xe11538f8u };
		uint32_t c = crc32_buf(wsCartROM, 0x70000); int bl = 0;
		for (int i = 0; i < 3; i++) if (c == block[i]) bl = 1;
		G.ww = !bl;
	}
	G.sram_size = 0; eeprom_size = 0;
	switch (h[5]) {
	case 0x01: G.sram_size = 8 * 1024; break;
	case 0x02: G.sram_size = 32 * 1024; break;
	case 0x03: G.sram_size = 128 * 1024; break;
	case 0x04: G.sram_size = 256 * 1024; break;
	case 0x05: G.sram_size = 512 * 1024; break;
	case 0x10: eeprom_size = 128; break;
	case 0x20: eeprom_size = 2 * 1024; break;
	case 0x50: eeprom_size = 1024; break;
	}
	/* Detective Conan, as upstream. The patch writes 0xFFFE8..0xFFFEC: only for images that reach that far (a footer
	 * that matches on a 64 KiB image wrote outside the buffer, found by ASan in the twin: dconan_footer.ws). */
	if ((h[8] | (h[9] << 8)) == 0x8de1 && h[0] == 0x01 && h[2] == 0x27 && rom_size > 0xFFFECu) {
		wsCartROM[0xfffe8] = 0xea; wsCartROM[0xfffe9] = 0; wsCartROM[0xfffea] = 0; wsCartROM[0xfffeb] = 0; wsCartROM[0xfffec] = 0x20;
	}
	snprintf(G.info, sizeof G.info, "%u KiB, dev %02X cart %02X, %s, %s, save %s%s, footer %s, sum %02X%02X",
	         (unsigned)(size >> 10), h[0], h[2], h[1] ? "colour" : "mono", (h[6] & 1) ? "vertical flag" : "horizontal flag",
	         G.sram_size ? "SRAM" : eeprom_size ? "EEPROM" : "none", G.ww ? " (WonderWitch)" : "",
	         G.footer[0] == 0xEA ? "ok" : "odd", h[9], h[8]);

	MDFNMP_Init(16384, (1 << 20) / 1024);
	G.real_wp = G.ww ? WSwan_writeport_WW : WSwan_writeport;
	if (G.ww) v30mz_init(WSwan_readmem20_WW, WSwan_writemem20_WW, WSwan_readport_WW, wp_hook);
	else v30mz_init(WSwan_readmem20, WSwan_writemem20, WSwan_readport, wp_hook);
	owner_to_settings();
	WSwan_MemoryInit(setting_wswan_language != 0, wsc, G.sram_size, G.ww);
	WSwan_GfxInit();
	WSwan_SoundInit();
	WSwan_RTCInit();
	wsMakeTiles();
	core_reset();

	G.surf.pixels = G.pixels; G.surf.width = WSWAN_WIDTH; G.surf.height = WSWAN_HEIGHT; G.surf.pitch = WSWAN_WIDTH; G.surf.depth = 16;
	memset(G.pixels, 0, sizeof G.pixels);
	WSwan_SetPixelFormat(16, G.mono_dark, G.mono_light);
	WSwan_SetSoundRate(G.rate);
	G.abuf_cap = (int32)((G.rate / 70 + 8) * 2);
	G.abuf = (int16 *)malloc(G.abuf_cap * sizeof(int16));
	if (!G.ring || G.ring_cap != G.rate) {
		free(G.ring); G.ring_cap = G.rate; G.ring = (int16 *)calloc(G.ring_cap, 4);
	}
	G.ring_head = G.ring_n = 0;
	G.pad = 0; G.frames = 0;
	G.loaded = 1;
	G.state_size = measure_state() + STATE_HEADROOM;
	G.state_scratch = (uint8_t *)malloc(G.state_size);
	if (!G.abuf || !G.ring || !G.state_scratch || G.state_size <= STATE_HEADROOM) {
		free_game(); errset(err, errlen, "out of memory"); return 4;
	}
	orient_reset();
	for (int i = 0; i < 3; i++) shadow_take(i);
	return 0;
}

/* ---- frames ---------------------------------------------------------------------------------------------- */

extern "C" void wswan_host_set_pad(unsigned m)
{
	if ((m & WSWAN_X1) && (m & WSWAN_X3)) m &= ~(WSWAN_X1 | WSWAN_X3);
	if ((m & WSWAN_X2) && (m & WSWAN_X4)) m &= ~(WSWAN_X2 | WSWAN_X4);
	if ((m & WSWAN_Y1) && (m & WSWAN_Y3)) m &= ~(WSWAN_Y1 | WSWAN_Y3);
	if ((m & WSWAN_Y2) && (m & WSWAN_Y4)) m &= ~(WSWAN_Y2 | WSWAN_Y4);
	G.pad = m & 0x7FF;
}

static void ring_push(const int16 *s, int frames)
{
	if (!G.ring) return;
	for (int i = 0; i < frames; i++) {
		if (G.ring_n == G.ring_cap) { G.ring_head = (G.ring_head + 1) % G.ring_cap; G.ring_n--; }   /* drop oldest */
		unsigned pos = (G.ring_head + G.ring_n) % G.ring_cap;
		G.ring[pos * 2] = s[i * 2]; G.ring[pos * 2 + 1] = s[i * 2 + 1];
		G.ring_n++;
	}
}

static void orient_update(void)
{
	int icon = WSwan_GfxRead(0x15);
	int v = (icon >> 1) & 1, hz = (icon >> 2) & 1;
	int icon_class = (v && !hz) ? 1 : (hz && !v) ? 2 : 0;
	int keys = (G.fr_y ? 1 : 0) | (G.fr_x ? 2 : 0);
	int want = -1, src = 0;
	G.last_keys = keys; G.last_icon = icon_class;
	G.since_switch++;
	if (G.table_mode) return;                      /* the table decides */
	if (keys == 1) { want = WSWAN_VERTICAL; src = WSWAN_SRC_KEYS; }
	else if (keys == 2) { want = WSWAN_HORIZONTAL; src = WSWAN_SRC_KEYS; }
	else if (icon_class) { want = icon_class == 1 ? WSWAN_VERTICAL : WSWAN_HORIZONTAL; src = WSWAN_SRC_ICON; }
	if (want < 0) return;                          /* no information this frame: keep counting */
	if (want == G.detected) { G.cand_n = 0; return; }
	/* a run is consecutive frames of the same direction AND the same kind of evidence: a key frame never counts
	 * toward the two frames an icon needs (found by the orientation property test, tests/orient_prop.py) */
	if (want == G.cand && src == G.cand_src) G.cand_n++; else { G.cand = want; G.cand_src = src; G.cand_n = 1; }
	if (G.cand_n >= (src == WSWAN_SRC_ICON ? SWITCH_FRAMES_ICON : SWITCH_FRAMES)) {
		G.detected = want; G.source = src; G.switches++; G.since_switch = 0; G.cand_n = 0;
	}
}

extern "C" void wswan_host_run_frame(void)
{
	if (!G.loaded) return;
	WSButtonStatus = (uint16)G.pad;
	MDFNMP_ApplyPeriodicCheats();
	G.fr_x = G.fr_y = G.fr_b = 0;
	while (!wsExecuteLine(&G.surf, !G.render)) {}
	int32 n = WSwan_SoundFlush(&G.abuf, &G.abuf_cap);
	if (G.audio_on && n > 0) ring_push(G.abuf, n);
	v30mz_timestamp = 0;
	G.frames++;
	orient_update();
}

extern "C" void wswan_host_reset(void)
{
	if (!G.loaded) return;
	core_reset();
	G.ring_n = 0;
	orient_reset();
}
extern "C" void wswan_host_set_render(int on) { G.render = on ? 1 : 0; }
extern "C" double wswan_host_frame_rate(void) { return WSWAN_FRAME_RATE; }
extern "C" double wswan_host_frame_rate_now(void)
{
	int vt = G.loaded ? WSwan_GfxRead(0x16) : 158;
	int lines = (vt > 143 ? vt : 143) + 1;      /* the core: (max(144, vtotal) + 1) lines */
	if (lines < 145) lines = 145;
	return 3072000.0 / ((double)lines * 256.0);
}
extern "C" uint64_t wswan_host_frame_count(void) { return G.frames; }

/* ---- video ----------------------------------------------------------------------------------------------- */

extern "C" const uint16_t *wswan_host_video(int *w, int *h, int *pitch)
{
	if (w) *w = WSWAN_WIDTH;
	if (h) *h = WSWAN_HEIGHT;
	if (pitch) *pitch = WSWAN_WIDTH * 2;
	return G.pixels;
}
extern "C" int wswan_host_video_is_color(void) { return G.loaded && wsVMode != 0; }
extern "C" int wswan_host_game_is_color(void) { return G.loaded && G.footer[7] != 0; }
extern "C" void wswan_host_set_mono_colors(uint32_t dark, uint32_t light)
{
	G.mono_dark = dark & 0xFFFFFF; G.mono_light = light & 0xFFFFFF;
	if (G.loaded) WSwan_SetMonoPalette(16, G.mono_dark, G.mono_light);
}
extern "C" int wswan_host_set_mono_palette(int id)
{
	/* values from beetle-wswan-libretro's palette list (start = dark, end = light) */
	static const uint32_t pal[4][2] = { { 0x000000, 0xFFFFFF }, { 0x3E3D20, 0x9B9D66 }, { 0x1B201E, 0xD7D49D }, { 0x151108, 0xFFFCCA } };
	if (id < 0 || id > 3) return 1;
	wswan_host_set_mono_colors(pal[id][0], pal[id][1]);
	return 0;
}

/* ---- audio ----------------------------------------------------------------------------------------------- */

extern "C" int wswan_host_set_audio_rate(unsigned hz)
{
	if (hz < 8000 || hz > 96000) return 1;
	G.rate = hz;
	if (G.loaded) {
		WSwan_SetSoundRate(hz);
		free(G.abuf); G.abuf_cap = (int32)((hz / 70 + 8) * 2); G.abuf = (int16 *)malloc(G.abuf_cap * sizeof(int16));
		free(G.ring); G.ring_cap = hz; G.ring = (int16 *)calloc(hz, 4); G.ring_head = G.ring_n = 0;
	}
	return 0;
}
extern "C" unsigned wswan_host_audio_rate(void) { return G.rate; }
extern "C" int wswan_host_audio_take(int16_t *dst, int max)
{
	int n = 0;
	while (n < max && G.ring_n) {
		dst[n * 2] = G.ring[G.ring_head * 2]; dst[n * 2 + 1] = G.ring[G.ring_head * 2 + 1];
		G.ring_head = (G.ring_head + 1) % G.ring_cap; G.ring_n--; n++;
	}
	return n;
}
extern "C" void wswan_host_set_audio_enabled(int on) { G.audio_on = on ? 1 : 0; if (!on) G.ring_n = 0; }

/* ---- orientation ----------------------------------------------------------------------------------------- */

extern "C" int wswan_host_orientation(void)
{
	if (G.mode == WSWAN_ORIENT_HORIZONTAL) return WSWAN_HORIZONTAL;
	if (G.mode == WSWAN_ORIENT_VERTICAL) return WSWAN_VERTICAL;
	return G.detected;
}
extern "C" void wswan_host_set_orientation_mode(int m) { G.mode = (m >= 0 && m <= 2) ? m : 0; }
extern "C" int wswan_host_orientation_mode(void) { return G.mode; }
extern "C" void wswan_host_orientation_info(wswan_orientation_info *o)
{
	o->header_vertical = G.hdr_vertical; o->icon_bits = G.last_icon; o->keys_last_frame = G.last_keys;
	o->detected = G.detected; o->source = G.mode ? WSWAN_SRC_MANUAL : G.source;
	o->switches = G.switches; o->frames_since_switch = G.since_switch;
}
extern "C" int wswan_host_orientation_table_set(unsigned dev, unsigned cart, int mode)
{
	for (int i = 0; i < G.table_n; i++)
		if (G.table[i].dev == dev && G.table[i].cart == cart) {
			if (mode) G.table[i].mode = mode; else G.table[i] = G.table[--G.table_n];
			return 0;
		}
	if (!mode) return 0;
	if (G.table_n >= MAX_TABLE) return 1;
	G.table[G.table_n++] = { dev, cart, mode };
	return 0;
}

/* ---- saves ----------------------------------------------------------------------------------------------- */

extern "C" size_t wswan_host_save_size(int r)
{
	if (!G.loaded) return 0;
	switch (r) {
	case WSWAN_SAVE_IEEPROM: return IEEPROM_SIZE;
	case WSWAN_SAVE_SRAM: if (G.ww) { uint32 s = 0; WSwan_GetWWSaveBlock(&s); return s; } return G.sram_size;
	case WSWAN_SAVE_EEPROM: return eeprom_size;
	}
	return 0;
}
extern "C" uint8_t *wswan_host_save_data(int r)
{
	if (!G.loaded) return NULL;
	switch (r) {
	case WSWAN_SAVE_IEEPROM: return WSwan_GetInternalEEPROM();
	case WSWAN_SAVE_SRAM: return G.ww ? WSwan_GetWWSaveBlock(NULL) : (G.sram_size ? wsSRAM : NULL);
	case WSWAN_SAVE_EEPROM: return eeprom_size ? wsEEPROM : NULL;
	}
	return NULL;
}
extern "C" int wswan_host_save_dirty(int r)
{
	size_t n = wswan_host_save_size(r);
	if (!n || r < 0 || r > 2) return 0;
	if (!G.shadow[r]) return 1;
	return memcmp(G.shadow[r], wswan_host_save_data(r), n) != 0;
}
extern "C" void wswan_host_save_mark_saved(int r) { if (G.loaded && r >= 0 && r <= 2) shadow_take(r); }
extern "C" void wswan_host_save_mark_unsaved(int r)
{
	if (r < 0 || r > 2 || !G.shadow[r]) return;
	G.shadow[r][0] ^= 0xFF;      /* any difference makes the compare report dirty; the next mark_saved retakes it */
}
static size_t cart_region(void) { return wswan_host_save_size(WSWAN_SAVE_SRAM) ? WSWAN_SAVE_SRAM : WSWAN_SAVE_EEPROM; }
extern "C" size_t wswan_host_srm_size(void)
{
	if (!G.loaded) return 0;
	return wswan_host_save_size(cart_region()) + 8 + IEEPROM_SIZE;
}
extern "C" size_t wswan_host_srm_export(uint8_t *dst, size_t cap)
{
	size_t need = wswan_host_srm_size(), c = wswan_host_save_size(cart_region());
	if (!need || cap < need) return 0;
	if (c) memcpy(dst, wswan_host_save_data(cart_region()), c);
	memcpy(dst + c, "WSiE", 4);
	dst[c + 4] = IEEPROM_SIZE & 0xFF; dst[c + 5] = IEEPROM_SIZE >> 8; dst[c + 6] = 0; dst[c + 7] = 0;
	memcpy(dst + c + 8, WSwan_GetInternalEEPROM(), IEEPROM_SIZE);
	for (int i = 0; i < 3; i++) shadow_take(i);
	return need;
}
extern "C" size_t wswan_host_srm_import(const uint8_t *src, size_t n)
{
	if (!G.loaded || !src) return 0;
	size_t c = wswan_host_save_size(cart_region()), used = 0;
	if (c) { size_t k = n < c ? n : c; memcpy(wswan_host_save_data(cart_region()), src, k); used = k; }
	if (n >= c + 8 + IEEPROM_SIZE && !memcmp(src + c, "WSiE", 4) && src[c + 4] == (IEEPROM_SIZE & 0xFF) && src[c + 5] == (IEEPROM_SIZE >> 8)) {
		memcpy(WSwan_GetInternalEEPROM(), src + c + 8, IEEPROM_SIZE);
		used += 8 + IEEPROM_SIZE;
	}
	for (int i = 0; i < 3; i++) shadow_take(i);
	return used;
}
extern "C" int wswan_host_srm_dirty(void)
{
	return wswan_host_save_dirty(0) | wswan_host_save_dirty(1) | wswan_host_save_dirty(2);
}
extern "C" void wswan_host_srm_mark_unsaved(void) { for (int i = 0; i < 3; i++) wswan_host_save_mark_unsaved(i); }

/* ---- clock ----------------------------------------------------------------------------------------------- */

extern "C" void wswan_host_set_time_source(time_t (*fn)(void)) { G.time_src = fn; }
extern "C" void wswan_host_rtc_sync(void) { if (G.loaded) WSwan_RTCInit(); }

/* ---- states ---------------------------------------------------------------------------------------------- */

extern "C" int StateAction(StateMem *sm, int load, int data_only)
{
	if (!v30mz_StateAction(sm, load, data_only)) return 0;
	if (!WSwan_MemoryStateAction(sm, load, data_only)) return 0;
	if (!Comm_StateAction(sm, load, data_only)) return 0;
	if (!WSwan_GfxStateAction(sm, load, data_only)) return 0;
	if (!WSwan_RTCStateAction(sm, load, data_only)) return 0;
	if (!WSwan_InterruptStateAction(sm, load, data_only)) return 0;
	if (!WSwan_SoundStateAction(sm, load, data_only)) return 0;
	if (!WSwan_EEPROMStateAction(sm, load, data_only)) return 0;
	return 1;
}
extern "C" size_t wswan_host_state_size(void) { return G.loaded ? G.state_size : 0; }
extern "C" size_t wswan_host_state_save_mem(uint8_t *buf, size_t cap)
{
	if (!G.loaded || cap < G.state_size) return 0;
	StateMem st;
	st.data = G.state_scratch; st.loc = 0; st.len = 0; st.malloced = (uint32_t)G.state_size; st.initial_malloc = 0;
	int ok = MDFNSS_SaveSM(&st, 0, 0, NULL, NULL, NULL);
	if (st.data != G.state_scratch) { free(st.data); return 0; }      /* it would have outgrown the scratch buffer */
	if (!ok || st.len > G.state_size) return 0;
	memcpy(buf, st.data, st.len);
	memset(buf + st.len, 0, G.state_size - st.len);
	return G.state_size;
}
extern "C" int wswan_host_state_load_mem(const uint8_t *buf, size_t n)
{
	if (!G.loaded) return 1;
	StateMem st;
	st.data = (uint8_t *)buf; st.loc = 0; st.len = (uint32_t)n; st.malloced = 0; st.initial_malloc = 0;
	if (!MDFNSS_LoadSM(&st, 0, 0)) return 3;
	G.fr_x = G.fr_y = G.fr_b = 0; G.cand_n = 0;
	if (!G.table_mode) {     /* the LCD icons are part of the state: take them at once instead of after the debounce */
		int icon = WSwan_GfxRead(0x15), v = (icon >> 1) & 1, hz = (icon >> 2) & 1;
		if (v != hz) { int d = v ? WSWAN_VERTICAL : WSWAN_HORIZONTAL; if (d != G.detected) { G.detected = d; G.source = WSWAN_SRC_ICON; G.switches++; G.since_switch = 0; } }
	}
	return 0;
}

/* ---- memory ---------------------------------------------------------------------------------------------- */

extern "C" int wswan_host_ra_read(unsigned addr, uint8_t *out, unsigned n)
{
	if (!G.loaded || !n) return 0;
	unsigned done = 0;
	if (addr < 0x10000) {
		unsigned k = 0x10000 - addr; if (k > n) k = n;
		memcpy(out, wsRAM + addr, k); done = k; addr += k; n -= k; out += k;
	}
	if (n && addr >= 0x10000) {
		size_t c = wswan_host_save_size(cart_region()); uint8_t *p = wswan_host_save_data(cart_region());
		if (p && addr - 0x10000 < c) {
			unsigned off = addr - 0x10000, k = (unsigned)(c - off); if (k > n) k = n;
			memcpy(out, p + off, k); done += k;
		}
	}
	return (int)done;
}
extern "C" const uint8_t *wswan_host_ram(size_t *size) { if (size) *size = G.loaded ? 65536 : 0; return G.loaded ? wsRAM : NULL; }
extern "C" int wswan_host_peek(unsigned addr) { uint8_t b; return wswan_host_ra_read(addr, &b, 1) == 1 ? b : -1; }

/* ---- cheats ---------------------------------------------------------------------------------------------- */

static int hexd(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; }
static int sep(char c) { return c == '+' || c == ',' || c == ';' || c == '.' || c == '_' || c == ' '; }
extern "C" int wswan_host_cheat_add(const char *code)
{
	int added = 0;
	if (!G.loaded || !code) return 0;
	const char *p = code;
	while (*p) {
		while (sep(*p)) p++;
		if (!*p) break;
		uint32 a = 0, v = 0; int ac = 0, vc = 0, valid = 1, d;
		while ((d = hexd(*p)) >= 0) { if (ac >= 6) { valid = 0; break; } a = (a << 4) | d; ac++; p++; }
		if (!ac || ac > 5) valid = 0;
		if (valid && *p == ':') p++; else valid = 0;
		if (valid) {
			while ((d = hexd(*p)) >= 0) { if (vc >= 2) { valid = 0; break; } v = (v << 4) | d; vc++; p++; }
			if (vc != 2) valid = 0;
		}
		if (valid && *p && !sep(*p)) valid = 0;
		if (valid && (a < 0x10000 || (G.sram_size && a < 0x10000 + G.sram_size)) && MDFNI_AddCheat("N/A", a, v, 0, 'R', 1, false)) { added++; G.cheats++; }
		while (*p && !sep(*p)) p++;
	}
	return added;
}
extern "C" void wswan_host_cheat_reset(void) { MDFN_FlushGameCheats(0); G.cheats = 0; }
extern "C" int wswan_host_cheat_count(void) { return G.cheats; }

/* ---- information ----------------------------------------------------------------------------------------- */

extern "C" const char *wswan_host_rom_info(void) { return G.loaded ? G.info : ""; }
extern "C" uint32_t wswan_host_rom_crc(void) { return G.loaded ? G.rom_crc : 0; }
extern "C" int wswan_host_footer(uint8_t out[16]) { if (!G.loaded) return 0; memcpy(out, G.footer, 16); return 1; }
extern "C" void wswan_host_set_log(void (*fn)(const char *)) { G.log = fn; }
