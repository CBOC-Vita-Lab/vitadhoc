/*
 * Sega host: see host.h. Replaces the libretro layer of Genesis Plus GX (libretro/libretro.c) with a memory based API.
 * The option names, the default values and the video geometry follow that file (the code paths are ported, not invented),
 * with the changes listed in md/core/PROVENANCE.md.
 *
 * Copyright CBOC-Vita. MIT licence. Links against Genesis Plus GX (md/core, non-commercial licence): a program containing
 * mdcore.o is not to be sold and ships its full source when modified.
 */
#include "shared.h"
#include "eq.h"
#include "host.h"
#include <ctype.h>

/* ---- what the core expects the host to define (libretro.c did) ------------------------------------------- */

t_config config;
char GG_ROM[256], AR_ROM[256], SK_ROM[256], SK_UPMEM[256], GG_BIOS[256], MD_BIOS[256];
char CD_BIOS_EU[256], CD_BIOS_US[256], CD_BIOS_JP[256], MS_BIOS_US[256], MS_BIOS_EU[256], MS_BIOS_JP[256];

uint8_t cart_size;   /* only read by the Mega CD cartridge code (compiled out of the load path) */
extern int8 fast_savestates;
extern void sound_update_fm_function_pointers(void);

#define SRAM_BYTES 0x10000
#define MAX_PATCHES 150          /* as Genesis Plus GX (MAX_CHEATS): one per part, for the whole game */
#define STATE_HDR 16
#define STATE_TRAILER 4
#define STATE_VERSION_ID 1
#define SOUND_MAX_FRAMES 2400

typedef struct patch {
	unsigned address, value;
	int compare;
	uint16_t old;
	uint8_t *prev;      /* 8 bit systems: the banked ROM byte currently patched */
	int active;         /* MD ROM patch applied */
} patch;

static struct {
	int inited, loaded, system;           /* system: MD_SYS_* */
	uint32_t seed;
	unsigned pad[2];
	int pad_type[2];                      /* MD_PAD_* wanted */
	int render, audio_on;
	uint16_t *fb_user; int fb_user_pitch;
	unsigned audio_rate;
	int16_t *ring; unsigned ring_cap, ring_r, ring_w, ring_n;   /* frames */
	int16_t sndbuf[SOUND_MAX_FRAMES * 2];
	/* geometry */
	int vwidth, vheight, vwoffset, bmdoffset;
	double vdar;
	md_video_info vi;
	unsigned serial;
	uint8_t reg0_prev;
	/* rom */
	char fname[16];
	const uint8_t *rom_src; size_t rom_n;
	uint32_t rom_crc, rom_size;
	int region_detected;
	char title[49], info[200];
	/* sram */
	uint32_t sram_crc;
	/* state */
	size_t state_payload_cap, state_size;
	uint8_t *state_scratch;
	/* cheats */
	patch cheat[MAX_PATCHES]; int ncheat;
	void (*log)(const char *);
	/* options */
	int opt_fm, opt_fm_hq, opt_psg_hq, opt_ym2413, opt_opll_nuked, opt_fm_preamp, opt_psg_preamp, opt_lowpass, opt_mono;
	int opt_interlace, opt_sprite_limit, opt_sms_border, opt_gg_extra, opt_overscan, opt_force_dtack, opt_addr_error;
	int opt_lcd, opt_vdp_mode, opt_region;
} G;

static uint16_t fb_internal[MD_MAX_WIDTH * MD_MAX_HEIGHT];

static void logf_(const char *s) { if (G.log) G.log(s); }

/* ---- config ---------------------------------------------------------------------------------------------- */

static void apply_options_to_config(void)
{
	int i;
	config.hq_fm = G.opt_fm_hq;
	config.hq_psg = G.opt_psg_hq;
	config.psg_preamp = G.opt_psg_preamp;
	config.fm_preamp = G.opt_fm_preamp;
	config.ym2413 = G.opt_ym2413;
	config.mono = G.opt_mono;
	config.filter = G.opt_lowpass ? 1 : 0;
	config.lp_range = G.opt_lowpass ? (uint16)((G.opt_lowpass * 0xFFFF) / 100) : 0x9999;
	config.render = G.opt_interlace;
	config.no_sprite_limit = G.opt_sprite_limit ? 0 : 1;
	config.overscan = G.opt_overscan;
	config.gg_extra = G.opt_gg_extra;
	config.force_dtack = G.opt_force_dtack;
	config.addr_error = G.opt_addr_error;
	config.lcd = (uint8)((G.opt_lcd * 255) / 100);
	config.vdp_mode = G.opt_vdp_mode;
	config.region_detect = G.opt_region;
	config.ym2612 = G.opt_fm <= 2 ? G.opt_fm : 0;
	config.ym3438 = G.opt_fm >= 3 ? G.opt_fm - 2 : 0;
	config.opll = G.opt_opll_nuked ? 1 : 0;
	for (i = 0; i < MAX_INPUTS; i++) {
		int p = (i == 4) ? 1 : (i == 0 ? 0 : 0);
		int t = G.pad_type[p];
		config.input[i].padtype = (t == MD_PAD_3) ? DEVICE_PAD3B : (t == MD_PAD_6) ? DEVICE_PAD6B : (DEVICE_PAD2B | DEVICE_PAD3B | DEVICE_PAD6B);
	}
}

static void config_defaults(void)
{
	memset(&config, 0, sizeof config);
	config.cdda_volume = 100; config.pcm_volume = 100;
	config.low_freq = 880; config.high_freq = 5000; config.lg = config.mg = config.hg = 100;
	config.system = 0; config.master_clock = 0; config.bios = 0; config.lock_on = 0;
	config.add_on = HW_ADDON_NONE;      /* no Mega CD / MegaSD: not part of md.bin (see PROVENANCE.md) */
	config.enhanced_vscroll = 0; config.enhanced_vscroll_limit = 8;
	config.overclock = 100; config.aspect_ratio = 0; config.left_border = 0;
	input.system[0] = SYSTEM_GAMEPAD;
	input.system[1] = SYSTEM_GAMEPAD;
	G.opt_fm = 0; G.opt_fm_hq = 1; G.opt_psg_hq = 1; G.opt_ym2413 = 2; G.opt_opll_nuked = 0;
	G.opt_fm_preamp = 100; G.opt_psg_preamp = 150; G.opt_lowpass = 0; G.opt_mono = 0;
	G.opt_interlace = 1; G.opt_sprite_limit = 1; G.opt_sms_border = 0; G.opt_gg_extra = 0; G.opt_overscan = 0;
	G.opt_force_dtack = 0; G.opt_addr_error = 1; G.opt_lcd = 0; G.opt_vdp_mode = 0; G.opt_region = 0;
	G.pad_type[0] = G.pad_type[1] = MD_PAD_AUTO;
	apply_options_to_config();
}

/* ---- the file layer the core calls ----------------------------------------------------------------------- */

int load_archive(char *filename, unsigned char *buffer, int maxsize, char *extension)
{
	if (extension) {
		size_t l = strlen(filename);
		memcpy(extension, l >= 3 ? &filename[l - 3] : "md", 3);
		extension[3] = 0;
	}
	/* only the game itself comes from the caller; BIOS, lock-on and Game Genie ROMs are never provided */
	if (filename != G.fname || !G.rom_src) return 0;
	if (G.rom_n > (size_t)maxsize) return 0;
	memcpy(buffer, G.rom_src, G.rom_n);
	return (int)G.rom_n;
}

/* ---- input ----------------------------------------------------------------------------------------------- */

static void ram_cheat_update(void);

void osd_input_update(void)
{
	int i, player = 0;
	for (i = 0; i < MAX_INPUTS; i++) {
		unsigned t = 0, p = G.pad[player & 1];
		switch (input.dev[i]) {
		case DEVICE_PAD6B: t = p & 0x0FFF; player++; break;
		case DEVICE_PAD3B: t = p & 0x00FF; player++; break;
		case DEVICE_PAD2B: t = p & 0x00BF; player++; break;
		default: break;
		}
		input.pad[i] = t;
	}
	ram_cheat_update();
}

void ROMCheatUpdate(void);

/* ---- geometry (libretro.c update_viewport / calculate_display_aspect_ratio) ------------------------------- */

static int is_sms_family(void) { return system_hw == SYSTEM_MARKIII || (system_hw & SYSTEM_SMS) || system_hw == SYSTEM_PBC; }

static double calc_dar(void)
{
	double dotrate, vsr;
	int h40;
	if ((system_hw == SYSTEM_GG || system_hw == SYSTEM_GGMS) && config.overscan == 0 && config.gg_extra == 0)
		return (6.0 / 5.0) * ((double)G.vwidth / (double)G.vheight);
	h40 = bitmap.viewport.w == 320;
	dotrate = system_clock / (h40 ? 8.0 : 10.0);
	vsr = vdp_pal ? 14750000.0 : 135000000.0 / 11.0;
	return (vsr / dotrate) * ((double)(G.vwidth - G.vwoffset) / ((double)G.vheight * 2.0));
}

static void update_viewport(void)
{
	if (system_hw == SYSTEM_GG && !config.gg_extra)
		bitmap.viewport.x = (config.overscan & 2) ? 14 : -48;
	else
		bitmap.viewport.x = (config.overscan & 2) * 7;

	if (G.opt_sms_border && (reg[0] & 0x20) && is_sms_family()) {
		G.bmdoffset = 16;                       /* bytes: 8 pixels */
		G.vwoffset = (G.opt_sms_border == 1) ? 8 : 16;
	} else
		G.bmdoffset = G.vwoffset = 0;

	G.vwidth = bitmap.viewport.w + bitmap.viewport.x * 2;
	G.vheight = bitmap.viewport.h + bitmap.viewport.y * 2;
	G.vdar = calc_dar();
	if (config.render && interlaced) G.vheight *= 2;
}

static void refresh_info(void)
{
	md_video_info v;
	memset(&v, 0, sizeof v);
	v.w = G.vwidth - G.vwoffset;
	v.h = G.vheight;
	v.native_w = bitmap.viewport.w + bitmap.viewport.x * 2;
	v.native_h = bitmap.viewport.h + bitmap.viewport.y * 2;
	v.system = G.system;
	v.h40 = ((system_hw & SYSTEM_PBC) == SYSTEM_MD) && (reg[12] & 1);
	v.interlace = (((system_hw & SYSTEM_PBC) == SYSTEM_MD) && interlaced) ? (config.render ? 2 : 1) : 0;
	v.pal = vdp_pal;
	v.left_mask = (is_sms_family() && (reg[0] & 0x20)) ? 8 : 0;
	v.crop_left = G.bmdoffset / 2;
	v.crop_right = (G.vwoffset > 8) ? G.vwoffset - 8 : 0;
	v.gg_window = ((system_hw == SYSTEM_GG || system_hw == SYSTEM_GGMS) && !config.gg_extra) ? 1 : 0;
	v.dar = G.vdar;
	v.par = (v.w > 0) ? G.vdar * v.h / (double)v.w : 0;
	if (v.par > 0 && v.interlace == 2) v.par = G.vdar * v.h / (double)v.w;
	v.fps = md_host_fps();
	if (memcmp(&v, &G.vi, offsetof(md_video_info, serial)) != 0) G.serial++;
	v.serial = G.serial;
	G.vi = v;
}

/* ---- audio ring ------------------------------------------------------------------------------------------ */

static int ring_alloc(unsigned rate)
{
	free(G.ring);
	G.ring_cap = rate;               /* one second */
	G.ring = (int16_t *)calloc((size_t)G.ring_cap * 2, sizeof(int16_t));
	G.ring_r = G.ring_w = G.ring_n = 0;
	return G.ring ? 0 : 1;
}

static void ring_push(const int16_t *s, int n)
{
	int i;
	if (!G.ring) return;
	for (i = 0; i < n; i++) {
		if (G.ring_n == G.ring_cap) { G.ring_r = (G.ring_r + 1) % G.ring_cap; G.ring_n--; }  /* drop the oldest */
		G.ring[G.ring_w * 2] = s[i * 2]; G.ring[G.ring_w * 2 + 1] = s[i * 2 + 1];
		G.ring_w = (G.ring_w + 1) % G.ring_cap; G.ring_n++;
	}
}

/* ---- lifetime -------------------------------------------------------------------------------------------- */

int md_host_init(void)
{
	memset(&G, 0, sizeof G);
	G.inited = 1; G.seed = 1; G.render = 1; G.audio_on = 1; G.audio_rate = MD_AUDIO_RATE;
	md_rand_seed(G.seed);
	config_defaults();
	memset(&bitmap, 0, sizeof bitmap);
	bitmap.width = MD_MAX_WIDTH; bitmap.height = MD_MAX_HEIGHT; bitmap.pitch = MD_MAX_WIDTH * 2;
	bitmap.data = (uint8_t *)fb_internal;
	return ring_alloc(G.audio_rate);
}

void md_host_unload(void)
{
	if (!G.inited) return;
	if (G.loaded) { ggenie_shutdown(); areplay_shutdown(); }
	audio_shutdown();
	system_hw = 0;
	G.loaded = 0; G.system = 0; G.ncheat = 0; G.rom_src = NULL; G.rom_n = 0;
	free(G.state_scratch); G.state_scratch = NULL; G.state_size = 0;
	G.pad[0] = G.pad[1] = 0;
}

void md_host_shutdown(void)
{
	if (!G.inited) return;
	md_host_unload();
	free(G.ring); G.ring = NULL;
	G.inited = 0;
}

int md_host_loaded(void) { return G.loaded; }
void md_host_set_seed(uint32_t seed) { G.seed = seed; md_rand_seed(seed); }

static int ext_is(const char *e, const char *a, const char *b, const char *c, const char *d)
{
	return (a && !strcmp(e, a)) || (b && !strcmp(e, b)) || (c && !strcmp(e, c)) || (d && !strcmp(e, d));
}

static int sms_header_at(const uint8_t *d, size_t n, size_t off) { return n > off + 0x10 && !memcmp(d + off, "TMR SEGA", 8); }

/* The system a file is: the extension when it says, the header otherwise. */
static int detect_system(const uint8_t *d, size_t n, const char *name)
{
	char ext[8] = "";
	size_t i, l;
	int sms;
	if (name && (l = strlen(name)) > 0) {
		const char *dot = strrchr(name, '.');
		if (dot && strlen(dot + 1) < sizeof ext) { for (i = 0; dot[1 + i]; i++) ext[i] = (char)tolower((unsigned char)dot[1 + i]); ext[i] = 0; }
	}
	if (ext_is(ext, "sms", "mv", "bms", NULL)) return MD_SYS_SMS;
	if (ext_is(ext, "gg", NULL, NULL, NULL)) return MD_SYS_GG;
	if (ext_is(ext, "sg", "sc", "sf", NULL)) return MD_SYS_SG;
	if (ext_is(ext, "md", "gen", "smd", "mdx")) return MD_SYS_MD;
	/* header */
	if (n > 0x104 && (!memcmp(d + 0x100, "SEGA", 4) || !memcmp(d + 0x100, "ESAG", 4))) return MD_SYS_MD;
	if (n > 0x4200 && (n % 0x4000) == 0x200 && d[8] == 0xAA && d[9] == 0xBB) return MD_SYS_MD;
	sms = sms_header_at(d, n, 0x7FF0) ? 0x7FF0 : sms_header_at(d, n, 0x3FF0) ? 0x3FF0 : sms_header_at(d, n, 0x1FF0) ? 0x1FF0 : 0;
	if (sms) {
		int reg = d[sms + 0x0F] >> 4;
		return (reg >= 5 && reg <= 7) ? MD_SYS_GG : MD_SYS_SMS;
	}
	return MD_SYS_MD;
}

static const char *hw_name(void)
{
	switch (system_hw) {
	case SYSTEM_SG: return "SG-1000";
	case SYSTEM_SGII: return "SG-1000 II";
	case SYSTEM_SGII_RAM_EXT: return "SG-1000 II (RAM extension)";
	case SYSTEM_MARKIII: return "Mark III";
	case SYSTEM_SMS: return "Master System";
	case SYSTEM_SMS2: return "Master System II";
	case SYSTEM_GG: return "Game Gear";
	case SYSTEM_GGMS: return "Game Gear (SMS mode)";
	case SYSTEM_MD: return "Mega Drive";
	case SYSTEM_PBC: return "Mega Drive (Power Base Converter)";
	case SYSTEM_PICO: return "Pico";
	default: return "";
	}
}

static int core_system(void)
{
	if ((system_hw & SYSTEM_PBC) == SYSTEM_MD && system_hw != SYSTEM_PBC) return MD_SYS_MD;
	if (system_hw == SYSTEM_PBC) return MD_SYS_SMS;
	if (system_hw & SYSTEM_GG) return MD_SYS_GG;
	if (system_hw & SYSTEM_SMS || system_hw == SYSTEM_MARKIII) return MD_SYS_SMS;
	if (system_hw & 0x03) return MD_SYS_SG;
	return 0;
}

static uint32_t rom_crc_of_image(void)
{
	/* the ROM as dumped: the core byte swapped a Mega Drive image for speed (little endian hosts) */
	if ((system_hw & SYSTEM_PBC) == SYSTEM_MD && system_hw != SYSTEM_PBC) {
		unsigned long c = 0; size_t i; uint8_t blk[4096];
		for (i = 0; i < cart.romsize; i += sizeof blk) {
			size_t n = cart.romsize - i < sizeof blk ? cart.romsize - i : sizeof blk, k;
			for (k = 0; k < n; k++) blk[k] = cart.rom[(i + k) ^ 1];
			c = crc32(c, blk, (unsigned)n);
		}
		return (uint32_t)c;
	}
	return (uint32_t)crc32(0, cart.rom, cart.romsize);
}

static const char *region_name(int r)
{
	switch (r) { case MD_REGION_USA: return "USA"; case MD_REGION_EUROPE: return "Europe"; case MD_REGION_JAPAN_NTSC: return "Japan NTSC"; case MD_REGION_JAPAN_PAL: return "Japan PAL"; }
	return "?";
}

static int region_of_code(unsigned rc)
{
	switch (rc) { case REGION_USA: return MD_REGION_USA; case REGION_EUROPE: return MD_REGION_EUROPE; case REGION_JAPAN_NTSC: return MD_REGION_JAPAN_NTSC; case REGION_JAPAN_PAL: return MD_REGION_JAPAN_PAL; }
	return MD_REGION_USA;
}

static void measure_state(void)
{
	size_t used;
	uint8_t *tmp = (uint8_t *)malloc(STATE_SIZE + 65536);
	if (!tmp) { G.state_size = 0; return; }
	fast_savestates = 0;
	used = (size_t)state_save(tmp);
	free(tmp);
	G.state_payload_cap = ((used + 512 + 1023) / 1024) * 1024;
	G.state_size = STATE_HDR + G.state_payload_cap + STATE_TRAILER;
	G.state_scratch = (uint8_t *)malloc(G.state_payload_cap + 65536);
	if (!G.state_scratch) G.state_size = 0;
}

int md_host_load_mem(const uint8_t *data, size_t n, const char *name, int system, char *err, size_t errlen)
{
	static const char *const ext_of[] = { "md", "md", "sms", "gg", "sg" };
	const char *fext;
	char tmp[8];
	int kind, i;
#define FAIL(code, msg) do { if (err && errlen) { strncpy(err, msg, errlen - 1); err[errlen - 1] = 0; } md_host_unload(); return (code); } while (0)

	if (err && errlen) err[0] = 0;
	if (!G.inited) FAIL(1, "not initialised");
	md_host_unload();
	if (!data || n < 0x200 || n > MAXROMSIZE) FAIL(2, n > MAXROMSIZE ? "ROM larger than 10 MB" : "unreadable ROM");
	kind = system ? system : detect_system(data, n, name);
	if (kind < MD_SYS_MD || kind > MD_SYS_SG) FAIL(3, "not a Sega ROM");
	fext = ext_of[kind];
	if (kind == MD_SYS_MD && name) {
		/* keep smd / mdx: the core converts those from the extension and the header */
		const char *dot = strrchr(name, '.');
		if (dot && strlen(dot + 1) == 3) {
			for (i = 0; i < 3; i++) tmp[i] = (char)tolower((unsigned char)dot[1 + i]);
			tmp[3] = 0;
			if (!strcmp(tmp, "smd") || !strcmp(tmp, "mdx")) fext = tmp;
		}
	}
	snprintf(G.fname, sizeof G.fname, "rom.%s", fext);
	G.rom_src = data; G.rom_n = n;

	apply_options_to_config();
	config.system = 0;
	if (system == MD_SYS_AUTO) {
		/* the core derives the hardware from the extension and its own ROM database */
	}
	system_hw = 0;
	rompath[0] = 0;
	bitmap.data = (uint8_t *)(G.fb_user ? G.fb_user : fb_internal);
	bitmap.pitch = (G.fb_user ? G.fb_user_pitch : MD_MAX_WIDTH) * 2;
	memset(bitmap.data, 0, (size_t)bitmap.pitch * MD_MAX_HEIGHT);
	input.system[0] = input.system[1] = SYSTEM_GAMEPAD;
	/* the "6" in the header makes the pad choice: the core reads it in input_init */
	if (load_rom(G.fname) <= 0) FAIL(4, "the core refused the ROM");
	G.rom_src = NULL;

	audio_init((int)G.audio_rate, 0);
	system_init();
	system_reset();
	md_host_set_seed(G.seed);
	G.loaded = 1;
	G.system = core_system();
	if (config.ym3438) OPN2_SetChipType(config.ym3438 == 1 ? ym3438_mode_ym2612 : ym3438_mode_readmode);
	YM2612Config(config.ym2612);
	{
		unsigned rc = region_code;
		G.region_detected = region_of_code(rc);
	}
	G.rom_size = (uint32_t)cart.romsize;
	G.rom_crc = rom_crc_of_image();
	memcpy(G.title, rominfo.international, sizeof G.title - 1); G.title[sizeof G.title - 1] = 0;
	for (i = (int)strlen(G.title) - 1; i >= 0 && G.title[i] == ' '; i--) G.title[i] = 0;
	snprintf(G.info, sizeof G.info, "%s, %u KB, %s%s%s, region %s, save RAM %s, CRC %08X", hw_name(), (unsigned)(G.rom_size / 1024),
		rominfo.product[0] ? "product " : "", rominfo.product, rominfo.product[0] ? "" : "", region_name(md_host_region()),
		sram.on ? "yes" : "no", (unsigned)G.rom_crc);
	G.ncheat = 0;
	G.vi.serial = 0; G.serial = 1;
	bitmap.viewport.changed = 11;
	update_viewport();
	G.reg0_prev = reg[0];
	refresh_info();
	G.sram_crc = (uint32_t)crc32(0, sram.sram, SRAM_BYTES);
	G.ring_r = G.ring_w = G.ring_n = 0;
	measure_state();
	if (!G.state_size) FAIL(4, "out of memory");
	return 0;
#undef FAIL
}

void md_host_reset(void)
{
	if (!G.loaded) return;
	md_rand_seed(G.seed);
	gen_reset(0);
}

/* ---- pads ------------------------------------------------------------------------------------------------ */

void md_host_set_pad(int port, unsigned mask) { if (port == 0 || port == 1) G.pad[port] = mask & 0x0FFF; }

int md_host_set_pad_type(int port, int type)
{
	if ((port != 0 && port != 1) || type < MD_PAD_AUTO || type > MD_PAD_6) return 1;
	G.pad_type[port] = type;
	apply_options_to_config();
	if (G.loaded) {
		/* the core takes the pad type at io_init; the machine is otherwise untouched */
		io_init();
		input_reset();
	}
	return 0;
}

int md_host_pad_type(int port)
{
	int d;
	if (!G.loaded || (port != 0 && port != 1)) return 0;
	d = input.dev[port ? 4 : 0];
	if (d == DEVICE_PAD6B) return MD_PAD_6;
	if (d == DEVICE_PAD3B) return MD_PAD_3;
	return 0;
}

/* ---- frames ---------------------------------------------------------------------------------------------- */

void md_host_set_render(int on) { G.render = on ? 1 : 0; }

void md_host_run_frame(void)
{
	int n, skip;
	if (!G.loaded) return;
	skip = !G.render;
	if ((system_hw & SYSTEM_PBC) == SYSTEM_MD) system_frame_gen(skip);
	else system_frame_sms(skip);
	n = audio_update(G.sndbuf);
	if (n > SOUND_MAX_FRAMES) n = SOUND_MAX_FRAMES;
	if (!G.audio_on) memset(G.sndbuf, 0, (size_t)n * 4);
	ring_push(G.sndbuf, n);
	if (is_sms_family() && reg[0] != G.reg0_prev) bitmap.viewport.changed |= 8;
	G.reg0_prev = reg[0];
	if (bitmap.viewport.changed & 9) {
		update_viewport();
		bitmap.viewport.changed &= ~9;
	}
	refresh_info();
}

/* ---- video ----------------------------------------------------------------------------------------------- */

const uint16_t *md_host_video(int *width, int *height, int *pitch_bytes)
{
	if (!G.loaded) { if (width) *width = 0; if (height) *height = 0; if (pitch_bytes) *pitch_bytes = 0; return NULL; }
	if (width) *width = G.vwidth - G.vwoffset;
	if (height) *height = G.vheight;
	if (pitch_bytes) *pitch_bytes = bitmap.pitch;
	return (const uint16_t *)(bitmap.data + G.bmdoffset);
}

void md_host_video_info(md_video_info *vi) { if (vi) *vi = G.vi; }
int md_host_pixel_format(void) { return MD_PIXEL_RGB565; }

void md_host_set_framebuffer(uint16_t *buf, int pitch_px)
{
	G.fb_user = buf; G.fb_user_pitch = pitch_px;
	if (buf && pitch_px >= MD_MAX_WIDTH) { bitmap.data = (uint8_t *)buf; bitmap.pitch = pitch_px * 2; }
	else { G.fb_user = NULL; bitmap.data = (uint8_t *)fb_internal; bitmap.pitch = MD_MAX_WIDTH * 2; }
}

/* ---- audio ----------------------------------------------------------------------------------------------- */

int md_host_set_audio_rate(unsigned hz)
{
	if (hz < 8000 || hz > 96000) return 1;
	G.audio_rate = hz;
	if (ring_alloc(hz)) return 1;
	if (G.loaded) { audio_set_rate((int)hz, 0); audio_reset(); }
	return 0;
}

unsigned md_host_audio_rate(void) { return G.audio_rate; }

int md_host_audio_take(int16_t *dst, int max_frames)
{
	int n = 0;
	while (n < max_frames && G.ring_n) {
		dst[n * 2] = G.ring[G.ring_r * 2]; dst[n * 2 + 1] = G.ring[G.ring_r * 2 + 1];
		G.ring_r = (G.ring_r + 1) % G.ring_cap; G.ring_n--; n++;
	}
	return n;
}

void md_host_set_audio_enabled(int on) { G.audio_on = on ? 1 : 0; }

/* ---- battery RAM ----------------------------------------------------------------------------------------- */

size_t md_host_sram_capacity(void) { return (G.loaded && sram.on) ? SRAM_BYTES : 0; }

size_t md_host_sram_size(void)
{
	int i;
	if (!G.loaded || !sram.on) return 0;
	for (i = SRAM_BYTES - 1; i >= 0; i--)
		if (sram.sram[i] != 0xFF) return (size_t)i + 1;
	return 0;
}

size_t md_host_sram_read(uint8_t *out, size_t cap)
{
	size_t n = md_host_sram_size();
	if (n > cap) n = cap;
	if (n) memcpy(out, sram.sram, n);
	return n;
}

size_t md_host_sram_write(const uint8_t *data, size_t n)
{
	if (!G.loaded || !sram.on) return 0;
	if (n > SRAM_BYTES) n = SRAM_BYTES;
	memcpy(sram.sram, data, n);
	G.sram_crc = (uint32_t)crc32(0, sram.sram, SRAM_BYTES);
	return n;
}

int md_host_sram_dirty(void)
{
	if (!G.loaded || !sram.on) return 0;
	return (uint32_t)crc32(0, sram.sram, SRAM_BYTES) != G.sram_crc;
}

void md_host_sram_mark_saved(void) { if (G.loaded) G.sram_crc = (uint32_t)crc32(0, sram.sram, SRAM_BYTES); }
void md_host_sram_mark_unsaved(void) { G.sram_crc = ~G.sram_crc; }

/* ---- save states ----------------------------------------------------------------------------------------- */

static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

size_t md_host_state_size(void) { return G.loaded ? G.state_size : 0; }

size_t md_host_state_save_mem(uint8_t *buf, size_t cap)
{
	size_t used;
	if (!G.loaded || !G.state_size || cap < G.state_size) return 0;
	fast_savestates = 0;
	used = (size_t)state_save(G.state_scratch);
	if (used > G.state_payload_cap) return 0;
	memset(buf, 0, G.state_size);
	memcpy(buf, "MDST", 4);
	put32(buf + 4, STATE_VERSION_ID);
	put32(buf + 8, (uint32_t)G.system);
	put32(buf + 12, (uint32_t)used);
	memcpy(buf + STATE_HDR, G.state_scratch, used);
	put32(buf + G.state_size - STATE_TRAILER, (uint32_t)crc32(0, buf, (unsigned)(G.state_size - STATE_TRAILER)));
	return G.state_size;
}

int md_host_state_load_mem(const uint8_t *buf, size_t n)
{
	uint32_t used;
	if (!G.loaded) return 1;
	if (!buf || n != G.state_size) return 3;
	if (memcmp(buf, "MDST", 4) || get32(buf + 4) != STATE_VERSION_ID || get32(buf + 8) != (uint32_t)G.system) return 3;
	used = get32(buf + 12);
	if (used == 0 || used > G.state_payload_cap) return 3;
	if (get32(buf + n - STATE_TRAILER) != (uint32_t)crc32(0, buf, (unsigned)(n - STATE_TRAILER))) return 3;
	memcpy(G.state_scratch, buf + STATE_HDR, used);      /* the core wants a writable buffer */
	fast_savestates = 0;
	if (!state_load(G.state_scratch)) return 3;
	G.ring_r = G.ring_w = G.ring_n = 0;
	G.reg0_prev = reg[0];
	update_viewport();                       /* the window origin is not in the state: derive it now, before the next frame draws */
	bitmap.viewport.changed &= ~9;
	refresh_info();
	return 0;
}

/* ---- memory for achievements ----------------------------------------------------------------------------- */

typedef struct { unsigned vs, ve, zbase; int kind; } window;   /* kind 0: Z80 address space, 1: save RAM buffer */

static int z80_byte(unsigned a, uint8_t *out)
{
	uint8_t *p = z80_readmap[(a & 0xFFFF) >> 10];
	if (!p) return 0;
	*out = p[a & 0x3FF];
	return 1;
}

static uint8_t rd_md(unsigned a)     /* 68000 address space, ROM and work RAM */
{
	a &= 0xFFFFFF;
	if (a >= 0xFF0000) return work_ram[(a & 0xFFFF) ^ 1];
	if (a < cart.romsize) return cart.rom[a ^ 1];
	return 0;
}

uint8_t md_host_peek(unsigned addr)
{
	uint8_t b = 0;
	if (!G.loaded) return 0;
	if (G.system == MD_SYS_MD) return rd_md(addr);
	z80_byte(addr, &b);
	return b;
}

int md_host_ra_console(void)
{
	switch (G.system) { case MD_SYS_MD: return 1; case MD_SYS_SMS: return 11; case MD_SYS_GG: return 15; case MD_SYS_SG: return 33; }
	return 0;
}

int md_host_ra_read(unsigned addr, uint8_t *out, unsigned n)
{
	unsigned i, got = 0;
	if (!G.loaded || !out) return 0;
	if (G.system == MD_SYS_MD) {
		unsigned end;
		if (addr < 0x10000) { end = 0x10000; for (i = 0; addr + i < end && i < n; i++) out[i] = work_ram[(addr + i) ^ 1]; return (int)i; }
		if (addr >= 0x10000 && addr < 0x20000) { end = 0x20000; for (i = 0; addr + i < end && i < n; i++) out[i] = sram.sram[addr + i - 0x10000]; return (int)i; }
		return 0;
	}
	if (G.system == MD_SYS_SG) {
		static const window w[] = { { 0x0000, 0x03FF, 0xC000, 0 }, { 0x0400, 0x1FFF, 0xC400, 0 }, { 0x2000, 0x3FFF, 0x2000, 0 }, { 0x4000, 0x5FFF, 0x8000, 0 } };
		for (i = 0; i < 4; i++) {
			if (addr >= w[i].vs && addr <= w[i].ve) {
				unsigned k;
				for (k = 0; addr + k <= w[i].ve && k < n; k++) { uint8_t b; if (!z80_byte(w[i].zbase + (addr + k - w[i].vs), &b)) break; out[k] = b; }
				return (int)k;
			}
		}
		return 0;
	}
	/* Master System / Game Gear */
	if (addr < 0x2000) { for (i = 0; addr + i < 0x2000 && i < n; i++) out[i] = work_ram[addr + i]; return (int)i; }
	if (addr >= 0x2000 && addr < 0xA000) { for (i = 0; addr + i < 0xA000 && i < n; i++) out[i] = sram.sram[addr + i - 0x2000]; return (int)i; }
	(void)got;
	return 0;
}

/* ---- cheats ---------------------------------------------------------------------------------------------- */
/* The cheat database stores decoded parts (gpsp/vita/tests/cheats/SEGA-CODES.md): Mega Drive "AAAAAA:VVVV", the 8 bit systems
 * "AAAA:VV[:CC]". The rules below are that file's; Game Genie / Action Replay text is decoded by the database tool, never here. */

static int is_md(void) { return G.system == MD_SYS_MD; }

/* a ROM patch of the Mega Drive: the big endian word at the even address (the core holds the ROM byte swapped, as words) */
static void rom_patch_apply_md(patch *p)
{
	uint16_t *w = (uint16_t *)(cart.rom + (p->address & 0xFFFFFE));
	p->old = *w; *w = (uint16_t)p->value;
	p->active = 1;
}

static void rom_patch_undo_md(patch *p)
{
	if (!p->active) return;
	*(uint16_t *)(cart.rom + (p->address & 0xFFFFFE)) = p->old;
	p->active = 0;
}

static int is_ram_patch(const patch *p) { return is_md() ? p->address >= 0xFF0000 : p->address >= 0xC000; }

static void ram_cheat_update(void)
{
	int i;
	for (i = 0; i < G.ncheat; i++) {
		patch *p = &G.cheat[i];
		if (!is_ram_patch(p)) continue;
		if (is_md()) {
			unsigned o = p->address & 0xFFFF;
			if ((p->value & 0xFF00) == 0) work_ram[o ^ 1] = (uint8_t)p->value;               /* byte store, exactly that address */
			else *(uint16_t *)(work_ram + (o & 0xFFFE)) = (uint16_t)p->value;               /* word store */
		} else {
			work_ram[p->address & 0x1FFF] = (uint8_t)p->value;
		}
	}
}

/* 8 bit systems: patch the byte the Z80 sees at the address now; called again by the core when the mapper switches banks */
void ROMCheatUpdate(void)
{
	int i;
	for (i = 0; i < G.ncheat; i++) {
		patch *p = &G.cheat[i];
		uint8_t *ptr;
		if (is_md() || is_ram_patch(p)) continue;
		if (p->prev) { *p->prev = (uint8_t)p->old; p->prev = NULL; }
		ptr = &z80_readmap[p->address >> 10][p->address & 0x3FF];
		if (p->compare <= 0 || (uint8_t)p->compare == *ptr) { p->old = *ptr; *ptr = (uint8_t)p->value; p->prev = ptr; }
	}
}

int md_host_cheat_add_patch(unsigned address, unsigned value, int compare)
{
	patch *p;
	if (!G.loaded || G.ncheat >= MAX_PATCHES) return 1;
	if (is_md()) {
		address &= 0xFFFFFF;
		if (value > 0xFFFF) return 1;
		if (address >= 0xFF0000) { /* work RAM */ }
		else if (address < 0x400000 && (address & 0xFFFFFE) + 2 <= cart.romsize) { /* ROM word */ }
		else return 1;
		compare = -1;
	} else {
		address &= 0xFFFF;
		if (value > 0xFF) return 1;
		if (compare == 0) compare = -1;                  /* as GPGX: a compare of 00 is no compare */
		if (address < 0xC000 && address >= cart.romsize) return 1;   /* not inside the image */
	}
	p = &G.cheat[G.ncheat];
	memset(p, 0, sizeof *p);
	p->address = address; p->value = value; p->compare = compare;
	G.ncheat++;
	if (is_md()) { if (!is_ram_patch(p)) rom_patch_apply_md(p); }
	else if (!is_ram_patch(p)) ROMCheatUpdate();
	return 0;
}

static int hexval(const char *s, size_t n, unsigned *v)
{
	size_t i; unsigned r = 0;
	if (!n || n > 8) return 0;
	for (i = 0; i < n; i++) { int c = (unsigned char)s[i]; if (!isxdigit(c)) return 0; r = r * 16 + (unsigned)(c <= '9' ? c - '0' : (tolower(c) - 'a' + 10)); }
	*v = r;
	return 1;
}

static int add_one(const char *s, size_t n)
{
	const char *c1 = memchr(s, ':', n), *c2;
	unsigned a, v, cmp = 0; int compare = -1;
	size_t vl;
	if (!c1) return 0;
	c2 = memchr(c1 + 1, ':', n - (size_t)(c1 + 1 - s));
	vl = (size_t)((c2 ? c2 : s + n) - (c1 + 1));
	if (!hexval(s, (size_t)(c1 - s), &a) || !hexval(c1 + 1, vl, &v) || vl > 4) return 0;
	if (c2) {
		if (is_md() || !hexval(c2 + 1, (size_t)(s + n - (c2 + 1)), &cmp) || cmp > 0xFF) return 0;
		compare = (int)cmp;
	}
	return md_host_cheat_add_patch(a, v, compare) == 0;
}

int md_host_cheat_add(const char *codes)
{
	int ok = 0;
	const char *p = codes;
	if (!codes) return 0;
	while (*p) {
		const char *e = p;
		while (*e && *e != '+' && *e != ',' && *e != ';' && !isspace((unsigned char)*e)) e++;
		if (e > p) ok += add_one(p, (size_t)(e - p));
		p = *e ? e + 1 : e;
	}
	return ok;
}

void md_host_cheat_reset(void)
{
	int i;
	for (i = G.ncheat - 1; i >= 0; i--) {
		patch *p = &G.cheat[i];
		if (is_md()) rom_patch_undo_md(p);
		else if (p->prev) { *p->prev = (uint8_t)p->old; p->prev = NULL; }
	}
	G.ncheat = 0;
}

int md_host_cheat_count(void) { return G.ncheat; }

/* ---- console state and options --------------------------------------------------------------------------- */

int md_host_system(void) { return G.loaded ? G.system : 0; }
const char *md_host_system_hw(void) { return G.loaded ? hw_name() : ""; }
int md_host_region(void) { return G.loaded ? region_of_code(region_code) : 0; }
int md_host_region_detected(void) { return G.loaded ? G.region_detected : 0; }
int md_host_is_pal(void) { return G.loaded ? vdp_pal : 0; }

double md_host_fps(void)
{
	if (!G.loaded) return 0;
	return (double)system_clock / (double)lines_per_frame / (double)MCYCLES_PER_LINE;
}

const char *md_host_rom_title(void) { return G.loaded ? G.title : ""; }
const char *md_host_rom_info(void) { return G.loaded ? G.info : ""; }
uint32_t md_host_rom_crc(void) { return G.loaded ? G.rom_crc : 0; }

/* the region change of libretro.c check_variables, for the systems without a BIOS */
static void region_changed(void)
{
	static const uint16 vc_table[4][2] = { { 0xDA, 0xF2 }, { 0xEA, 0x102 }, { 0xDA, 0xF2 }, { 0x106, 0x10A } };
	get_region(NULL);
	audio_set_rate(snd.sample_rate, 0);
	if ((system_hw & SYSTEM_PBC) == SYSTEM_MD) io_reg[0x00] = 0x20 | region_code | (config.bios & 1);
	else io_reg[0x00] = 0x80 | (region_code >> 1);
	lines_per_frame = vdp_pal ? 313 : 262;
	if (system_hw & SYSTEM_MD) status = (status & ~1) | vdp_pal;
	switch (bitmap.viewport.h) {
	case 192: vc_max = vc_table[0][vdp_pal]; break;
	case 224: vc_max = vc_table[1][vdp_pal]; break;
	case 240: vc_max = vc_table[3][vdp_pal]; break;
	}
	bitmap.viewport.changed = 11;
}

int md_host_set_region(int region)
{
	if (region < MD_REGION_AUTO || region > MD_REGION_JAPAN_PAL) region = 0;
	G.opt_region = region;
	if (G.loaded && config.region_detect != region) { config.region_detect = region; region_changed(); }
	else config.region_detect = region;
	return 0;
}

typedef struct { const char *name; int *val; int min, max; } optdef;

int md_host_set_option(const char *name, int value)
{
	static const optdef defs[] = {
		{ "fm", &G.opt_fm, 0, 4 }, { "fm_hq", &G.opt_fm_hq, 0, 1 }, { "psg_hq", &G.opt_psg_hq, 0, 1 },
		{ "ym2413", &G.opt_ym2413, 0, 2 }, { "opll_nuked", &G.opt_opll_nuked, 0, 1 },
		{ "fm_preamp", &G.opt_fm_preamp, 0, 400 }, { "psg_preamp", &G.opt_psg_preamp, 0, 400 },
		{ "lowpass", &G.opt_lowpass, 0, 100 }, { "mono", &G.opt_mono, 0, 1 },
		{ "interlace", &G.opt_interlace, 0, 1 }, { "sprite_limit", &G.opt_sprite_limit, 0, 1 },
		{ "sms_border", &G.opt_sms_border, 0, 2 }, { "gg_extra", &G.opt_gg_extra, 0, 1 }, { "overscan", &G.opt_overscan, 0, 3 },
		{ "force_dtack", &G.opt_force_dtack, 0, 1 }, { "addr_error", &G.opt_addr_error, 0, 1 }, { "lcd", &G.opt_lcd, 0, 100 },
		{ "vdp_mode", &G.opt_vdp_mode, 0, 2 },
	};
	size_t i;
	int old_nuked = config.ym3438, old_opll = config.opll;
	for (i = 0; i < sizeof defs / sizeof defs[0]; i++) {
		if (strcmp(defs[i].name, name)) continue;
		if (value < defs[i].min) value = defs[i].min;
		if (value > defs[i].max) value = defs[i].max;
		*defs[i].val = value;
		if (!G.inited) return 0;
		{
			int region = config.region_detect, vm = config.vdp_mode;
			apply_options_to_config();
			config.region_detect = region;     /* owned by md_host_set_region */
			config.vdp_mode = vm;
		}
		if (!G.loaded) { config.vdp_mode = G.opt_vdp_mode; return 0; }
		if (!strcmp(name, "vdp_mode")) { config.vdp_mode = value; region_changed(); }
		if (!strcmp(name, "fm") || !strcmp(name, "opll_nuked")) {
			YM2612Config(config.ym2612);
			if ((old_nuked > 0) != (config.ym3438 > 0) || (old_opll > 0) != (config.opll > 0)) { sound_init(); sound_reset(); }
			if (config.ym3438) OPN2_SetChipType(config.ym3438 == 1 ? ym3438_mode_ym2612 : ym3438_mode_readmode);
		}
		if (!strcmp(name, "psg_preamp")) psg_config(0, config.psg_preamp, ((system_hw & SYSTEM_PBC) == SYSTEM_MD) ? 0xFF : io_reg[6]);
		if (!strcmp(name, "interlace") || !strcmp(name, "sms_border")) { bitmap.viewport.changed |= 9; update_viewport(); refresh_info(); }
		return 0;
	}
	return 1;
}

int md_host_get_option(const char *name, int *value)
{
	if (!strcmp(name, "region")) { *value = G.opt_region; return 0; }
	{
		static const char *const names[] = { "fm", "fm_hq", "psg_hq", "ym2413", "opll_nuked", "fm_preamp", "psg_preamp", "lowpass", "mono", "interlace",
			"sprite_limit", "sms_border", "gg_extra", "overscan", "force_dtack", "addr_error", "lcd", "vdp_mode" };
		int *const vals[] = { &G.opt_fm, &G.opt_fm_hq, &G.opt_psg_hq, &G.opt_ym2413, &G.opt_opll_nuked, &G.opt_fm_preamp, &G.opt_psg_preamp, &G.opt_lowpass,
			&G.opt_mono, &G.opt_interlace, &G.opt_sprite_limit, &G.opt_sms_border, &G.opt_gg_extra, &G.opt_overscan, &G.opt_force_dtack, &G.opt_addr_error,
			&G.opt_lcd, &G.opt_vdp_mode };
		size_t i;
		for (i = 0; i < sizeof names / sizeof names[0]; i++) if (!strcmp(names[i], name)) { *value = *vals[i]; return 0; }
	}
	return 1;
}

void md_host_set_log(void (*fn)(const char *line)) { G.log = fn; (void)logf_; }
