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

uint8_t cart_size;   /* the RAM cartridge's size id of the Mega CD (md_host_set_option "ram_cart") */
extern int md_cd_wanted;                   /* md/core/loadrom.c: a disc is being loaded */
extern int8 fast_savestates;
extern void sound_update_fm_function_pointers(void);
extern int audio_max_frames;      /* md/core/system.c: the most sample frames audio_update may write (G.sndbuf's size) */

#define SRAM_BYTES 0x10000
#define MAX_PATCHES 150          /* as Genesis Plus GX (MAX_CHEATS): one per part, for the whole game */
#define STATE_HDR 16
#define STATE_TRAILER 4
#define STATE_VERSION_ID 2      /* 2: the core's state is followed by the "carry" block (see carry_write); 1 (no block) still loads */
#define STATE_VERSION_OLD 1
#define CARRY_MAGIC 0x59524143u  /* "CARY" */
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
	size_t state_payload_cap, state_size, state_size_old;    /* state_size_old: what a version 1 file of this game measures (no carry block) */
	size_t carry_len;                    /* bytes of the carry block, header included */
	unsigned long carry_refused;         /* state loads whose carry block was refused (a machine's own states never are: the tests count it) */
	uint8_t *state_scratch;
	/* run-ahead (md_host_run_frame_ahead) */
	uint8_t *ahead_state, *ahead_sram;
	int ahead_debug_tail;                /* md_host_ahead_debug_tail: steps 2 and 3 only */
	int ahead_quiet;                     /* the frame being run is the look-ahead one: its sound is dropped */
	unsigned long ahead_frames, ahead_sram_undone;
	/* cheats */
	patch cheat[MAX_PATCHES]; int ncheat;
	void (*log)(const char *);
	/* options */
	int opt_fm, opt_fm_hq, opt_psg_hq, opt_ym2413, opt_opll_nuked, opt_fm_preamp, opt_psg_preamp, opt_lowpass, opt_mono;
	int opt_interlace, opt_sprite_limit, opt_sms_border, opt_gg_extra, opt_overscan, opt_force_dtack, opt_addr_error, opt_gg_band;
	int wide_req;                        /* the margin the host asked for (md_host_set_wide_margin) */
	int opt_lcd, opt_vdp_mode, opt_region;
	/* Sega CD */
	int cd, opt_ram_cart, cd_bios_wanted, cd_tracks, cd_force_region;
	uint32_t cd_crc;
	char cd_info[160], cd_bios_used[80];
} G;

/* the BIOS images (one per region, 128 KB each) outlive md_host_init: the player hands them over once */
#define CD_BIOS_BYTES 0x20000
static uint8_t *g_cd_bios[3];            /* 0 USA, 1 Europe, 2 Japan */
static uint32_t g_cd_bios_crc[3];

static uint16_t fb_internal[MD_MAX_WIDTH * MD_MAX_HEIGHT];
static void wide_apply(void);

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
	config.add_on = HW_ADDON_NONE;      /* no Mega CD / MegaSD add-on for a cartridge (a disc is loaded with md_host_load_disc) */
	config.cd_latency = 1;              /* the drive's access and seek times, on as upstream's default: cdd.c names the games that hang or misplay without them */
	config.enhanced_vscroll = 0; config.enhanced_vscroll_limit = 8;
	config.overclock = 100; config.aspect_ratio = 0; config.left_border = 0;
	input.system[0] = SYSTEM_GAMEPAD;
	input.system[1] = SYSTEM_GAMEPAD;
	G.opt_fm = 0; G.opt_fm_hq = 1; G.opt_psg_hq = 1; G.opt_ym2413 = 2; G.opt_opll_nuked = 0;
	G.opt_fm_preamp = 100; G.opt_psg_preamp = 150; G.opt_lowpass = 0; G.opt_mono = 0;
	G.opt_interlace = 1; G.opt_sprite_limit = 1; G.opt_sms_border = 0; G.opt_gg_extra = 0; G.opt_overscan = 0; G.opt_gg_band = 0;
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
	/* Sega CD BIOS: the images the player handed over (a region without one is noted, load_rom then fails and md_host_load_disc tries another) */
	if (filename == CD_BIOS_US || filename == CD_BIOS_EU || filename == CD_BIOS_JP) {
		const int k = filename == CD_BIOS_US ? 0 : filename == CD_BIOS_EU ? 1 : 2;
		if (!g_cd_bios[k] || maxsize < CD_BIOS_BYTES) { G.cd_bios_wanted = k + 1; return 0; }
		memcpy(buffer, g_cd_bios[k], CD_BIOS_BYTES);
		G.cd_bios_wanted = 0;
		snprintf(G.cd_bios_used, sizeof G.cd_bios_used, "%s", k == 0 ? "USA" : k == 1 ? "Europe" : "Japan");
		return CD_BIOS_BYTES;
	}
	/* only the game itself comes from the caller; the other BIOS, lock-on and Game Genie ROMs are never provided */
	if (filename != G.fname || !G.rom_src) return 0;
	if (G.rom_n > (size_t)maxsize) return 0;
	memcpy(buffer, G.rom_src, G.rom_n);
	memset(buffer + G.rom_n, 0, (size_t)maxsize - G.rom_n);        /* nothing of a game that was loaded before (a flash cartridge rewrites its ROM; a bigger ROM leaves its tail) */
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
		return G.opt_gg_band ? ((double)G.vwidth / (double)G.vheight)      /* the wide band: square pixels, 256 x 144 is 16:9 */
		                     : (6.0 / 5.0) * ((double)G.vwidth / (double)G.vheight);
	h40 = bitmap.viewport.w == 320;
	dotrate = system_clock / (h40 ? 8.0 : 10.0);
	vsr = vdp_pal ? 14750000.0 : 135000000.0 / 11.0;
	return (vsr / dotrate) * ((double)(G.vwidth - G.vwoffset) / ((double)G.vheight * 2.0));
}

static void update_viewport(void)
{
	if (system_hw == SYSTEM_GG && !config.gg_extra)
		bitmap.viewport.x = (config.overscan & 2) ? 14 : (G.opt_gg_band ? 0 : -48);
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
	v.gg_window = ((system_hw == SYSTEM_GG || system_hw == SYSTEM_GGMS) && !config.gg_extra && !G.opt_gg_band) ? 1 : 0;
	v.gg_band = ((system_hw == SYSTEM_GG || system_hw == SYSTEM_GGMS) && !config.gg_extra && G.opt_gg_band && !(config.overscan & 2)) ? 1 : 0;
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

/* the worker threads (md/core/vdp_thread.c): the platform's thread functions and which workers it wants (kept across md_host_init) */
static md_thread_ops g_ops;
static unsigned g_worker_mask;

int md_host_set_threads(const md_thread_ops *ops, unsigned mask)
{
	if (ops) g_ops = *ops; else memset(&g_ops, 0, sizeof g_ops);
	g_worker_mask = ops ? (mask & (MD_THREAD_VDP | MD_THREAD_VDP_SMS)) : 0;
	return 0;
}

void md_host_vdp_debug(int check, int stress) { vdpw_set_check(check); vdpw_set_stress(stress); }
unsigned long md_host_vdp_check_errors(void) { return vdpw_check_errors(); }
int md_host_vdp_census(unsigned long out[11])
{
	vdpw_census_t c;
	if (!vdpw_on) return 0;
	vdpw_census(&c);
	out[0] = c.records; out[1] = c.words; out[2] = c.status_joins; out[3] = c.joins; out[4] = c.join_wait_polls; out[5] = c.full_waits; out[6] = c.wakes;
	out[7] = c.vram_blocks; out[8] = c.vram_alls; out[9] = c.vdp_us; out[10] = c.join_wait_us;
	return 1;
}

int md_host_init(void)
{
	memset(&G, 0, sizeof G);
	G.inited = 1; G.seed = 1; G.render = 1; G.audio_on = 1; G.audio_rate = MD_AUDIO_RATE;
	md_rand_seed(G.seed);
	config_defaults();
	memset(&bitmap, 0, sizeof bitmap);
	bitmap.width = MD_MAX_WIDTH; bitmap.height = MD_MAX_HEIGHT; bitmap.pitch = MD_MAX_WIDTH * 2;
	bitmap.data = (uint8_t *)fb_internal;
	audio_max_frames = SOUND_MAX_FRAMES;
	return ring_alloc(G.audio_rate);
}

void md_host_unload(void)
{
	if (!G.inited) return;
	if (G.loaded) { ggenie_shutdown(); areplay_shutdown(); }
	vdpw_stop();
	audio_shutdown();
	if (G.cd) { cdd_unload(); md_cd_wanted = 0; system_bios &= ~0x10; }      /* the core keeps the boot ROM it loaded: the next disc reads it again (it may be another BIOS) */
	system_hw = 0;
	G.cd = 0; G.cd_tracks = 0; G.cd_info[0] = 0; G.cd_bios_used[0] = 0;
	G.loaded = 0; G.system = 0; G.ncheat = 0; G.rom_src = NULL; G.rom_n = 0; wide_m = 0;
	free(G.state_scratch); G.state_scratch = NULL; G.state_size = 0;
	free(G.ahead_state); G.ahead_state = NULL; free(G.ahead_sram); G.ahead_sram = NULL; G.ahead_quiet = 0;
	G.pad[0] = G.pad[1] = 0;
}

void md_host_shutdown(void)
{
	if (!G.inited) return;
	md_host_unload();
	md_host_cd_bios_clear();
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
	case SYSTEM_MCD: return "Mega CD";
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

/* The carry block: what the machine keeps between frames and the core's state does not hold (see the *_carry functions in md/core).
 * [magic u32][length u32] then, each as its module writes it: the renderers' sprite lists of the next frame's first line, pending sprite
 * overflow and collision position (both renderer copies), the VDP's FIFO byte flag / HV latch / latched scroll / FIFO slot times /
 * table bases / picture size, the PSG's queued level changes, the pause button edge and the serial EEPROMs' protocol position.
 * With it a state loaded at frame N runs on exactly as the machine that saved it did (md_twin --proof proves it), and run-ahead can go
 * back to the end of the real frame. A version 1 file has none: the machine loads as the core always loaded it. */
extern int psg_carry_size(void);
extern int vdp_carry_size(void);
extern void vdp_carry_save(void *p);
extern int vdp_carry_load(const void *p);
extern void psg_carry_save(void *p);
extern void psg_carry_load(const void *p);
extern int machine_carry(void *p, int load);

static size_t carry_size(void)
{
	return 8 + (size_t)vdpw_carry_size() + (size_t)vdp_carry_size() + (size_t)psg_carry_size() + (size_t)machine_carry(NULL, 0);
}

static void put32(uint8_t *p, uint32_t v);
static uint32_t get32(const uint8_t *p);

static void carry_write(uint8_t *p)
{
	size_t n = 8;
	memset(p, 0, G.carry_len);
	put32(p, CARRY_MAGIC); put32(p + 4, (uint32_t)G.carry_len);
	vdpw_carry_save(p + n); n += (size_t)vdpw_carry_size();
	vdp_carry_save(p + n); n += (size_t)vdp_carry_size();
	psg_carry_save(p + n); n += (size_t)psg_carry_size();
	machine_carry(p + n, 0);
}

static void carry_read(const uint8_t *p, size_t avail)
{
	size_t n = 8;
	if (avail < G.carry_len || get32(p) != CARRY_MAGIC || get32(p + 4) != (uint32_t)G.carry_len) { G.carry_refused++; return; }
	if (vdp_carry_load(p + n + (size_t)vdpw_carry_size())) { G.carry_refused++; return; }                       /* values no machine produces: leave what the load derived */
	vdpw_carry_load((void *)(p + n)); n += (size_t)vdpw_carry_size();
	n += (size_t)vdp_carry_size();
	psg_carry_load(p + n); n += (size_t)psg_carry_size();
	machine_carry((void *)(p + n), 1);
}

static void measure_state(void)
{
	size_t used;
	uint8_t *tmp = (uint8_t *)malloc(STATE_SIZE + 65536);
	if (!tmp) { G.state_size = 0; return; }
	fast_savestates = 0;
	used = (size_t)state_save(tmp);
	free(tmp);
	G.carry_len = carry_size();
	G.state_size_old = STATE_HDR + (((used + 512 + 1023) / 1024) * 1024) + STATE_TRAILER;
	G.state_payload_cap = ((used + G.carry_len + 512 + 1023) / 1024) * 1024;
	G.state_size = STATE_HDR + G.state_payload_cap + STATE_TRAILER;
	/* zeroed: the core's state writer leaves gaps (padding it skips), and what was in them went into every state, its hash and its file */
	G.state_scratch = (uint8_t *)calloc(1, G.state_payload_cap + 65536);
	if (!G.state_scratch) G.state_size = 0;
}

/* what follows a successful load_rom, for a cartridge and for a disc alike */
#define FAILB(code, msg) do { if (err && errlen) { strncpy(err, msg, errlen - 1); err[errlen - 1] = 0; } md_host_unload(); return (code); } while (0)
static void cd_format_bram(void);
static uint32_t cd_save_crc(void);
static int boot_common(char *err, size_t errlen)
{
	int i;
	audio_init((int)G.audio_rate, 0);
	/* the VDP worker: always for the Mega Drive when asked for; for the 8 bit systems only when MD_THREAD_VDP_SMS is set too */
	if ((g_worker_mask & MD_THREAD_VDP) && ((system_hw & SYSTEM_PBC) == SYSTEM_MD || (g_worker_mask & MD_THREAD_VDP_SMS)) && vdpw_start(&g_ops))
		g_worker_mask &= ~MD_THREAD_VDP;       /* the worker could not start: one thread */
	system_init();
	system_reset();
	if (G.cd) cd_format_bram();            /* the BIOS's backup RAM: formatted when it is empty (a reset clears it) */
	md_host_set_seed(G.seed);
	G.loaded = 1;
	G.system = core_system();
	wide_apply();
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
	G.sram_crc = G.cd ? cd_save_crc() : (uint32_t)crc32(0, sram.sram, SRAM_BYTES);
	G.ring_r = G.ring_w = G.ring_n = 0;
	measure_state();
	if (!G.state_size) FAILB(4, "out of memory");
	return 0;
}
#undef FAILB

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
	memset(bitmap.data, 0, (size_t)bitmap.pitch * (MD_MAX_HEIGHT - 1) + (size_t)MD_MAX_WIDTH * 2);   /* the last row only as wide as a picture: a wide canvas's buffer starts MD_WIDE_MAX pixels in */
	input.system[0] = input.system[1] = SYSTEM_GAMEPAD;
	/* the "6" in the header makes the pad choice: the core reads it in input_init */
	if (load_rom(G.fname) <= 0) FAIL(4, "the core refused the ROM");
	G.rom_src = NULL;

	return boot_common(err, errlen);
#undef FAIL
}

/* ---- Sega CD ---------------------------------------------------------------------------------------------- */

/* the disc files are read through stdio with a read buffer of their own: a data track is read sector by sector (2 KB), a CD-DA track in
 * bursts, and a multi track disc keeps one file open per track, so the buffer is a moderate 32 KB */
FILE *md_cd_fopen(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (f) setvbuf(f, NULL, _IOFBF, 32768);
	return f;
}

/* The core seeks before every sector it reads. The Vita's C library (newlib) throws the read buffer away on every fseek of a stream whose
 * buffer size was set (and of every stream there: its fstat reports no block size), so a seek followed by a 2 KB read would fetch 32 KB from
 * the card each time. A seek to where the stream already is, or a little further on (the 304 bytes between the user data of two raw
 * sectors), is therefore done by reading on: the buffer stays and the card is read once per 32 KB. Anything else is a real seek. */
int md_cd_fseek(FILE *f, long off, int whence)
{
	if (!f) return -1;
	if (whence == SEEK_SET && off >= 0) {
		long cur = ftell(f);
		if (cur >= 0 && off >= cur && off - cur <= 4096) {
			uint8_t skip[512];
			long d = off - cur;
			while (d > 0) {
				size_t k = d > (long)sizeof skip ? sizeof skip : (size_t)d;
				if (fread(skip, 1, k, f) != k) break;
				d -= (long)k;
			}
			if (d == 0) return 0;
		}
	}
	return fseek(f, off, whence);
}

/* a NULL stream (a cue sheet's track whose FILE line was not understood, a file that is gone) reads nothing */
size_t md_cd_fread(void *p, size_t size, size_t n, FILE *f) { return f ? fread(p, size, n, f) : 0; }
long md_cd_ftell(FILE *f) { return f ? ftell(f) : 0; }
char *md_cd_fgets(char *s, int n, FILE *f) { return f ? fgets(s, n, f) : NULL; }
int md_cd_fclose(FILE *f) { return f ? fclose(f) : 0; }

/* the dumps of the console's BIOS that are known: what they are called (the 240p Test Suite's list, the MAME and No-Intro names) and the
 * region they are for. A dump that is not here is accepted when it has the structure of a BIOS (below). */
static const struct { uint32_t crc; int region; const char *name; } k_bios[] = {
	{ 0xC6D10268, 1, "Sega CD (USA) model 1, v1.10" },       { 0x529AC15A, 2, "Mega CD (Europe) model 1, v1.00" },
	{ 0x79F85384, 3, "Mega CD (Japan) model 1, v1.00S" },    { 0x9BCE40B2, 3, "Mega CD (Japan) model 1, v1.00G" },
	{ 0xF18DDE5B, 3, "Mega CD (Japan) model 1, v1.00L" },    { 0x2EA250C0, 3, "Mega CD (Japan) model 1, v1.00O" },
	{ 0x9D2DA8F2, 3, "Mega CD (Japan) model 1, v1.01" },     { 0x4BE18FF6, 3, "Mega CD (Japan) model 1, v1.11" },
	{ 0x550F30BB, 3, "Mega CD (Asia) model 1, v1.00" },      { 0x2E49D72C, 1, "Sega CD (USA) model 2, v2.11X" },
	{ 0x8AF65F58, 1, "Sega CD (USA) model 2, v2.00" },       { 0x9F6F6276, 1, "Sega CD (USA) model 2, v2.00W" },
	{ 0x4D5CB8DA, 2, "Mega CD (Europe) model 2, v2.00W" },   { 0x0507B590, 2, "Mega CD (Europe) model 2, v2.00" },
	{ 0xC1AA217F, 2, "Mega CD (Europe) model 2, v2.11X" },   { 0xDD6CC972, 3, "Mega CD (Japan) model 2, v2.00C" },
	{ 0x8052C7A0, 3, "Aiwa CD-ROM (Japan), v2.11" },         { 0x290F8E33, 1, "X'Eye (USA), v2.00" },
	{ 0xD21FE71D, 3, "Wondermega (Japan), v1.00" },          { 0x2B19972F, 3, "Wondermega M2 (Japan), v2.00" },
	{ 0xD48C44B5, 1, "Sega CDX (USA), v2.21X" },             { 0xAACB851E, 2, "Multi-Mega (Europe), v2.21" },
	{ 0x50CD3D23, 1, "LaserActive (USA), v1.04" },           { 0x3B10CF41, 1, "LaserActive (USA), v1.02" },
	{ 0x474AAA44, 3, "LaserActive (Japan), v1.05" },         { 0x00EEDB3A, 3, "LaserActive (Japan), v1.02" },
};

/* the structure of a BIOS dump: "SEGA" in the 68000 header of the boot code at one of the places the models keep it (BIOSCheck.c of the 240p
 * Test Suite does the same); the region is the first letter of the header's region field ($1F0) */
static int bios_structure_ok(const uint8_t *d)
{
	const uint8_t *p = d + 0x15800;
	if (!memcmp(p + 0x6D, "SEGA", 4)) return 1;
	p = d + 0x16000;
	if (!memcmp(p + 0x6D, "SEGA", 4) || !memcmp(p + 0x6D, "WONDER", 6)) return 1;
	p = d + 0x1AD00;
	return !memcmp(p + 0x6D, "SEGA", 4);
}

const char *md_host_cd_bios_name(const uint8_t *data, size_t n)
{
	size_t i;
	uint32_t c;
	if (!data || n != CD_BIOS_BYTES) return NULL;
	c = (uint32_t)crc32(0, data, (unsigned)n);
	for (i = 0; i < sizeof k_bios / sizeof k_bios[0]; i++) if (k_bios[i].crc == c) return k_bios[i].name;
	return NULL;
}

static int g_cd_bios_force_all;
void md_host_cd_bios_test_force(int on) { g_cd_bios_force_all = on; }

int md_host_cd_bios_add(const uint8_t *data, size_t n, int force)
{
	uint8_t tmp[16];
	uint32_t c;
	size_t i;
	int region = 0, swapped = 0;
	if (!data || n != CD_BIOS_BYTES) return 0;
	if (g_cd_bios_force_all) force = 1;
	/* a byte swapped dump (MAME's) has "ESAG" where "SEGA" belongs */
	if (!memcmp(data + 0x100, "ESAG", 4)) swapped = 1;
	if (swapped) {
		uint8_t *sw = (uint8_t *)malloc(CD_BIOS_BYTES);
		int r;
		if (!sw) return 0;
		for (i = 0; i < CD_BIOS_BYTES; i += 2) { sw[i] = data[i + 1]; sw[i + 1] = data[i]; }
		r = md_host_cd_bios_add(sw, n, force);
		free(sw);
		return r;
	}
	c = (uint32_t)crc32(0, data, (unsigned)n);
	for (i = 0; i < sizeof k_bios / sizeof k_bios[0]; i++) if (k_bios[i].crc == c) region = k_bios[i].region;
	if (!region) {
		if (!force && !bios_structure_ok(data)) return 0;
		if (!force && memcmp(data + 0x100, "SEGA", 4) && memcmp(data + 0x100, "WOND", 4)) { /* the header of the boot code */ }
		memcpy(tmp, data + 0x1F0, 16);
		for (i = 0; i < 16 && tmp[i] == ' '; i++) ;
		region = tmp[i] == 'U' ? 1 : tmp[i] == 'E' ? 2 : tmp[i] == 'J' ? 3 : 0;
		if (!region && force) region = 1;
		if (!region) {          /* a region free patch or an unusual header: look at the whole field */
			for (i = 0; i < 16; i++) { if (tmp[i] == 'U') { region = 1; break; } if (tmp[i] == 'E') { region = 2; break; } if (tmp[i] == 'J') { region = 3; break; } }
		}
		if (!region) return 0;
	}
	if (!g_cd_bios[region - 1]) g_cd_bios[region - 1] = (uint8_t *)malloc(CD_BIOS_BYTES);
	if (!g_cd_bios[region - 1]) return 0;
	memcpy(g_cd_bios[region - 1], data, CD_BIOS_BYTES);
	g_cd_bios_crc[region - 1] = c;
	return region == 1 ? MD_REGION_USA : region == 2 ? MD_REGION_EUROPE : MD_REGION_JAPAN_NTSC;
}

void md_host_cd_bios_clear(void)
{
	int i;
	for (i = 0; i < 3; i++) { free(g_cd_bios[i]); g_cd_bios[i] = NULL; g_cd_bios_crc[i] = 0; }
}

int md_host_cd_bios_count(void)
{
	int i, n = 0;
	for (i = 0; i < 3; i++) if (g_cd_bios[i]) n++;
	return n;
}

/* the first bytes of a disc image */
int md_host_cd_probe(const char *path, char *why, size_t whylen)
{
	static const uint8_t sync[12] = { 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00 };
	uint8_t h[32];
	size_t n, l;
	FILE *f;
	int ok = 0;
	const char *w = "not a Sega CD disc";
	if (why && whylen) why[0] = 0;
	if (!path || !*path) { w = "no disc given"; goto done; }
	f = fopen(path, "rb");
	if (!f) { w = "the disc file could not be opened"; goto done; }
	n = fread(h, 1, sizeof h, f);
	l = strlen(path);
	if (n >= 8 && !memcmp(h, "MComprHD", 8)) ok = 1;                                   /* a CHD */
	else if (n >= 16 && !memcmp(h, "SEGADISCSYSTEM", 14)) ok = 1;                       /* cooked 2048 byte sectors */
	else if (n >= 30 && !memcmp(h, sync, 12) && !memcmp(h + 16, "SEGADISCSYSTEM", 14)) ok = 1;   /* raw 2352 byte sectors */
	else if (n >= 14 && l > 4 && !strcasecmp(path + l - 4, ".cue")) {
		/* a cue sheet: it names a FILE and a TRACK */
		char *txt = (char *)malloc(16384);
		if (txt) {
			size_t m;
			fseek(f, 0, SEEK_SET);
			m = fread(txt, 1, 16383, f);
			txt[m] = 0;
			if (strstr(txt, "FILE") && strstr(txt, "TRACK")) ok = 1; else w = "not a cue sheet";
			free(txt);
		}
	} else if (n >= 16 && !memcmp(h, sync, 12)) w = "a CD data track, but not a Sega CD disc";
	fclose(f);
done:
	if (!ok && why && whylen) { strncpy(why, w, whylen - 1); why[whylen - 1] = 0; }
	return ok;
}

/* the format of an empty backup RAM (Genesis Plus GX libretro.c: the header the BIOS expects) */
static const uint8_t k_brm_format[0x40] = {
	0x5f,0x5f,0x5f,0x5f,0x5f,0x5f,0x5f,0x5f,0x5f,0x5f,0x5f,0x00,0x00,0x00,0x00,0x40, 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
	0x53,0x45,0x47,0x41,0x5f,0x43,0x44,0x5f,0x52,0x4f,0x4d,0x00,0x01,0x00,0x00,0x00, 0x52,0x41,0x4d,0x5f,0x43,0x41,0x52,0x54,0x52,0x49,0x44,0x47,0x45,0x5f,0x5f,0x5f
};

static void cd_format_one(uint8_t *area, size_t size)
{
	uint8_t f[0x40];
	if (!memcmp(area + size - 0x20, k_brm_format + 0x20, 0x20)) return;       /* already formatted */
	memset(area, 0, size - 0x40);
	memcpy(f, k_brm_format, 0x40);
	f[0x10] = f[0x12] = f[0x14] = f[0x16] = (uint8_t)(((size / 64) - 3) >> 8);
	f[0x11] = f[0x13] = f[0x15] = f[0x17] = (uint8_t)(((size / 64) - 3) & 0xFF);
	memcpy(area + size - 0x40, f, 0x40);
}

static void cd_format_bram(void)
{
	cd_format_one(scd.bram, sizeof scd.bram);
	if (scd.cartridge.id) cd_format_one(scd.cartridge.area, (size_t)scd.cartridge.mask + 1);
}

static size_t cd_save_size(void) { return sizeof scd.bram + (scd.cartridge.id ? (size_t)scd.cartridge.mask + 1 : 0); }
static uint32_t cd_save_crc(void)
{
	unsigned long c = crc32(0, scd.bram, sizeof scd.bram);
	if (scd.cartridge.id) c = crc32(c, scd.cartridge.area, (unsigned)scd.cartridge.mask + 1);
	return (uint32_t)c;
}

int md_host_is_cd(void) { return G.loaded && G.cd; }
uint32_t md_host_cd_bios_crc(void) { return G.loaded && G.cd ? G.cd_crc : 0; }
const char *md_host_cd_info(void) { return G.cd ? G.cd_info : ""; }
int md_host_cd_tracks(void) { return G.cd ? G.cd_tracks : 0; }

int md_host_cd_test_toc(int track, int *start, int *end, int *type)
{
	if (!G.loaded || !G.cd) return 0;
	if (track >= 1 && track <= cdd.toc.last) {
		if (start) *start = cdd.toc.tracks[track - 1].start;
		if (end) *end = cdd.toc.tracks[track - 1].end;
		if (type) *type = cdd.toc.tracks[track - 1].type;
	}
	return cdd.toc.last;
}

/* one data sector of track 1 as the drive reads it; the drive's position is put back */
static int cd_sector(int lba, uint8_t *out2048)
{
	int si = cdd.index, sl = cdd.lba;
	memset(out2048, 0, 2048);              /* (a disc without a data track, a short file: zeros, not what was on the stack) */
	cdd.index = 0;
	cdd.lba = lba;
	cdd_read_data(out2048, NULL);
	cdd.index = si; cdd.lba = sl;
	return 0;
}

int md_host_cd_header(uint8_t *out, size_t n)
{
	uint8_t s[2048];
	if (!G.loaded || !G.cd || !out || n > sizeof s) return 1;
	cd_sector(0, s);
	memcpy(out, s, n);
	return 0;
}

int md_host_cd_test_data(int lba, uint8_t *out)
{
	if (!G.loaded || !G.cd) return 1;
	return cd_sector(lba, out);
}

int md_host_cd_test_audio(int track, int lba, int samples, int16_t last[2])
{
	if (!G.loaded || !G.cd || track < 1 || track > cdd.toc.last || cdd.toc.tracks[track - 1].type) return 1;
	cdd.index = track - 1;
	cdd.lba = lba;
	cdd.fader[0] = cdd.fader[1] = 1024;
	cdd.audio[0] = cdd.audio[1] = 0;
	scd.regs[0x36 >> 1].byte.h = 0;
	cdd_seek_audio(track - 1, lba);
	cdd_read_audio((unsigned)samples);
	blip_clear(snd.blips[2]);
	last[0] = cdd.audio[0]; last[1] = cdd.audio[1];
	return 0;
}

/* the identity of a disc for the link and the achievements log: the first 2 KB of its first sector as the core read them, and its length */
static uint32_t cd_disc_crc(void)
{
	/* read from the disc, not from cart.rom where load_rom left the header: that memory is also the RAM cartridge's (scd.cartridge.area
	 * holds cart.rom), which cd_cart_init has cleared by now when one is fitted: the identity would not depend on the disc */
	uint8_t s[2048], le[4];
	unsigned long c;
	cd_sector(0, s);
	c = crc32(0, s, 0x210);
	put32(le, (uint32_t)cdd.toc.end);
	return (uint32_t)crc32(c, le, 4);
}

int md_host_load_disc(const char *path, char *err, size_t errlen)
{
	static const int rd_of[3] = { 1, 2, 3 };       /* config.region_detect: 1 USA, 2 Europe (PAL), 3 Japan (NTSC) */
	int attempt, k, r = 0;
	char why[100];
#define FAILD(code, msg) do { if (err && errlen) { strncpy(err, msg, errlen - 1); err[errlen - 1] = 0; } md_host_unload(); return (code); } while (0)
	if (err && errlen) err[0] = 0;
	if (!G.inited) FAILD(1, "not initialised");
	md_host_unload();
	if (!path || !*path) FAILD(2, "no disc given");
	if (!md_host_cd_bios_count()) FAILD(5, "no Sega CD BIOS");
	if (!md_host_cd_probe(path, why, sizeof why)) { why[0] = (char)toupper((unsigned char)why[0]); FAILD(strstr(why, "opened") ? 2 : 3, why); }
	apply_options_to_config();
	config.system = 0;
	config.region_detect = G.opt_region;
	G.cd_force_region = 0;
	cart_size = (uint8_t)G.opt_ram_cart;
	for (attempt = 0; attempt < 2; attempt++) {
		system_hw = 0;
		rompath[0] = 0;
		bitmap.data = (uint8_t *)(G.fb_user ? G.fb_user : fb_internal);
		bitmap.pitch = (G.fb_user ? G.fb_user_pitch : MD_MAX_WIDTH) * 2;
		memset(bitmap.data, 0, (size_t)bitmap.pitch * (MD_MAX_HEIGHT - 1) + (size_t)MD_MAX_WIDTH * 2);
		input.system[0] = input.system[1] = SYSTEM_GAMEPAD;
		G.cd_bios_wanted = 0;
		md_cd_wanted = 1;
		r = load_rom((char *)path);
		md_cd_wanted = 0;
		if (r > 0) break;
		if (G.cd_bios_wanted && attempt == 0) {
			/* the disc's region has no BIOS here: the console takes the region of one it has (the BIOS refuses a disc of another region only when it
			 * is told the console is that region) */
			for (k = 0; k < 3 && !g_cd_bios[k]; k++) ;
			if (k < 3) { config.region_detect = rd_of[k]; G.cd_force_region = rd_of[k]; continue; }
		}
		break;
	}
	if (r <= 0 || system_hw != SYSTEM_MCD) {
		/* a disc the core opened and then refused (md_host_unload closes the files of a running disc only; here cdd is the core's own:
		 * the last thing loaded into this memory was this attempt, never a cartridge's ROM) */
		if (ext && cdd.loaded) cdd_unload();
		FAILD(4, G.cd_bios_wanted ? "no BIOS for this disc's region" : "the core could not start this disc");
	}
	/* the data track has to be a file that is open (a cue sheet whose FILE line the core did not understand leaves a track without one) */
	if (cdd.toc.tracks[0].type && !cdd.toc.tracks[0].fd) { G.cd = 1; FAILD(3, "the disc's data track file is missing"); }      /* (G.cd: md_host_unload closes the disc and forgets the BIOS the core read) */
	G.cd = 1;
	G.rom_src = NULL;
	/* No RAM cartridge: the cartridge slot is empty and reads $FF everywhere, which is also what the core's own "disabled" setting answers
	 * where the BIOS reads the RAM cartridge's size ($400001). Without this the slot holds a one byte "ROM", the first letter of the disc
	 * header that load_rom left in cart.rom ('S', 0x53), which cd_cart_init pads with $FF: the BIOS would read a size id of $53. */
	if (!cart_size) cart.rom[0] = 0xFF;
	{
		int rc = boot_common(err, errlen);
		if (rc) return rc;
	}
	G.cd_tracks = cdd.toc.last;
	G.rom_crc = cd_disc_crc();
	G.rom_size = (uint32_t)cdd.toc.end * 2048u;
	{
		const char *bn = NULL;
		G.cd_crc = 0;
		for (k = 0; k < 3; k++) if (!strcmp(G.cd_bios_used, k == 0 ? "USA" : k == 1 ? "Europe" : "Japan")) { bn = NULL; if (g_cd_bios[k]) { bn = md_host_cd_bios_name(g_cd_bios[k], CD_BIOS_BYTES); G.cd_crc = g_cd_bios_crc[k]; } }
		snprintf(G.cd_info, sizeof G.cd_info, "Mega CD disc, %d track%s, %u min, %s%s, BIOS %s%s%s%s", G.cd_tracks, G.cd_tracks == 1 ? "" : "s", (unsigned)(cdd.toc.end / 75 / 60),
			rominfo.product[0] ? "product " : "no product code", rominfo.product, G.cd_bios_used,
			bn ? " (" : "", bn ? bn : "", bn ? ")" : "");
		snprintf(G.info, sizeof G.info, "%s", G.cd_info);
	}
	return 0;
#undef FAILD
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
	if (system_hw == SYSTEM_MCD) system_frame_scd(skip);
	else if ((system_hw & SYSTEM_PBC) == SYSTEM_MD) system_frame_gen(skip);
	else system_frame_sms(skip);
	vdpw_frame_end();                  /* every line is drawn, the sprite status bits are merged */
	vdp_status_only = 0;
	n = audio_update(G.sndbuf);
	if (n > SOUND_MAX_FRAMES) n = SOUND_MAX_FRAMES;
	if (!G.audio_on) memset(G.sndbuf, 0, (size_t)n * 4);
	if (!G.ahead_quiet) ring_push(G.sndbuf, n);
	if (is_sms_family() && reg[0] != G.reg0_prev) bitmap.viewport.changed |= 8;
	G.reg0_prev = reg[0];
	if (bitmap.viewport.changed & 9) {
		update_viewport();
		bitmap.viewport.changed &= ~9;
	}
	refresh_info();
}

/* ---- run-ahead ------------------------------------------------------------------------------------------- */

static size_t state_save_to(uint8_t *buf, size_t cap, int with_crc);
static int state_load_from(const uint8_t *buf, size_t n, int check_crc);

/* What the state does not hold and a frame can change: the battery RAM (and the EEPROM / flash contents, which live in it): compared and put
 * back below. The EEPROMs' protocol position and the flash chip's mode are in the state (the carry block / the core's own part). */
int md_host_ahead_possible(void)
{
	return G.loaded && G.state_size;
}

/* A Sega CD's backup RAM and RAM cartridge are not in the state either (and are not sram.sram): the copy taken before the look-ahead frame
 * (back = 0), and put back after it when that frame wrote to them (back = 1, returns 1). Without this a save the look-ahead frame started
 * was written a second time by the real frame, from memory the first pass had already changed. */
static int cd_save_ahead(uint8_t *buf, int back)
{
	const size_t a = sizeof scd.bram, c = scd.cartridge.id ? (size_t)scd.cartridge.mask + 1 : 0;
	if (!back) { memcpy(buf, scd.bram, a); if (c) memcpy(buf + a, scd.cartridge.area, c); return 0; }
	if (!memcmp(buf, scd.bram, a) && (!c || !memcmp(buf + a, scd.cartridge.area, c))) return 0;
	memcpy(scd.bram, buf, a);
	if (c) memcpy(scd.cartridge.area, buf + a, c);
	return 1;
}

static int ahead_alloc(void)
{
	if (!G.ahead_state) G.ahead_state = (uint8_t *)malloc(G.state_size);
	if (!G.ahead_sram) G.ahead_sram = (uint8_t *)malloc(G.cd && cd_save_size() > SRAM_BYTES ? cd_save_size() : SRAM_BYTES);
	return G.ahead_state && G.ahead_sram;
}

/* The wide view's list of VRAM blocks written since its last look (wide_vl): run-ahead must show it what a plain run would, the blocks the look-ahead
 * frame wrote, and not those of the real frame (the look-ahead frame writes them again) or of the way back (the analysis has already seen the
 * picture that image belongs to): marks made in between are dropped. */
static void wide_list_cut(int n, int all)
{
	int i;
	for (i = n; i < wide_vn; i++) wide_vd[wide_vl[i]] = 0;
	if (n < wide_vn) wide_vn = n;
	wide_vall = all;
}

int md_host_run_frame_ahead(void (*shown)(void *user), void *user)
{
	const int render = G.render;
	md_video_info vi;
	int vwidth, vheight, vwoffset, bmdoffset;
	double vdar;
	if (!md_host_ahead_possible() || !render || !ahead_alloc()) { md_host_run_frame(); return 1; }

	const int wide_n0 = wide_vn, wide_all0 = wide_vall;
	/* 1. the real frame: not drawn (the sprite pass still runs for the status bits), its sound goes to the ring */
	if (!G.ahead_debug_tail) {
		G.render = 0;
		md_host_run_frame();
		G.render = render;
	}
	wide_list_cut(wide_n0, wide_all0);
	if (state_save_to(G.ahead_state, G.state_size, 0) != G.state_size) return 1;      /* cannot happen (the size was measured); the real frame stands */
	if (G.cd) cd_save_ahead(G.ahead_sram, 0);
	else if (sram.on) memcpy(G.ahead_sram, sram.sram, SRAM_BYTES);     /* (the state holds the sprite list of the next first line, the FIFO flag, the PSG's queued deltas, ...) */

	/* 2. the frame after it with the same buttons: drawn, silent */
	G.ahead_quiet = 1;
	md_host_run_frame();
	if (shown) shown(user);            /* the machine is the one of the picture just drawn (the wide view reads it here) */
	vi = G.vi; vwidth = G.vwidth; vheight = G.vheight; vwoffset = G.vwoffset; bmdoffset = G.bmdoffset; vdar = G.vdar;

	/* 3. back to the end of the real frame */
	const int wide_n1 = wide_vn, wide_all1 = wide_vall;
	if (state_load_from(G.ahead_state, G.state_size, 0) != 0) { G.ahead_quiet = 0; return 2; }     /* cannot happen: the state is our own */
	G.ahead_quiet = 0;
	wide_list_cut(wide_n1, wide_all1);
	if (G.cd) { if (cd_save_ahead(G.ahead_sram, 1)) G.ahead_sram_undone++; }
	else if (sram.on && memcmp(G.ahead_sram, sram.sram, SRAM_BYTES)) { memcpy(sram.sram, G.ahead_sram, SRAM_BYTES); G.ahead_sram_undone++; }
	/* what md_host_video / md_host_video_info describe is the picture in the buffer, the look-ahead one */
	G.vi = vi; G.vwidth = vwidth; G.vheight = vheight; G.vwoffset = vwoffset; G.bmdoffset = bmdoffset; G.vdar = vdar;
	G.serial = vi.serial;
	G.ahead_frames++;
	return 0;
}

/* debug (md_twin MD_TWIN_AHEADDIFF): only the look-ahead frame and the way back, so that the caller can compare the process before and after */
int md_host_ahead_debug_tail(void)
{
	int r;
	G.ahead_debug_tail = 1;
	r = md_host_run_frame_ahead(NULL, NULL);
	G.ahead_debug_tail = 0;
	return r;
}

void md_host_ahead_stat(unsigned long out[2]) { out[0] = G.ahead_frames; out[1] = G.ahead_sram_undone; }

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
	wide_apply();
}

/* ---- wide view ------------------------------------------------------------------------------------------- */

extern const wide_line_t *vdpw_wide_rec(void);
extern const uint32 *vdpw_wide_opq(void);
extern const uint8 *vdpw_r_vram(void);
extern const uint8 *vdpw_r_vsram(void);
extern const uint16 *vdpw_wide_pal(void);
extern void vdpw_wide_rows_clear(void);
extern void vdpw_wide_row_set(int plane, int row, unsigned int addr, const uint16 *words, int n);
typedef char md_wide_line_size_check[sizeof(md_wide_line) == sizeof(wide_line_t) ? 1 : -1];

static void wide_apply(void)
{
	/* strips only on the Mega Drive and only into a canvas that has the room for them */
	int m = G.wide_req;
	if (m < 0) m = 0;
	if (m > MD_WIDE_MAX) m = MD_WIDE_MAX;
	if (G.loaded && G.system != MD_SYS_MD) m = m > 0 ? -1 : 0;          /* the 8 bit systems: the line records only */
	else if (!G.loaded || !G.fb_user || G.fb_user_pitch < MD_WIDE_PITCH) m = 0;
	if (m && !wide_m) wide_vall = 1;                                     /* the list of written VRAM blocks was not kept while it was off */
	wide_m = m;
}

void md_host_set_wide_planes(int mask) { wide_planes = mask & 3; }

int md_host_set_wide_margin(int margin)
{
	G.wide_req = margin;
	wide_apply();
	return wide_m > 0 ? wide_m : 0;
}

/* the frame handed to the wide view's analysis: one at a time (md_host_wide_run joins the one before) */
static struct {
	md_wide_frame f;
	md_wide_fn fn;
	void *user;
	uint8_t cram[128];
	uint16_t dirty[0x800];
} WJ;

static void wide_job(void *arg) { (void)arg; WJ.fn(WJ.user, &WJ.f); }

void md_host_wide_join(void) { vdpw_join(); }

void md_host_wide_run(md_wide_fn fn, void *user)
{
	int i;
	if (!fn) return;
	vdpw_join();
	memset(&WJ.f, 0, sizeof WJ.f);
	WJ.fn = fn; WJ.user = user;
	if (G.loaded) {
		WJ.f.h = bitmap.viewport.h;
		WJ.f.margin = wide_m > 0 ? wide_m : 0;
		WJ.f.line = (const md_wide_line *)vdpw_wide_rec();
		WJ.f.opq = vdpw_wide_opq();
		WJ.f.vram = vdpw_r_vram(); WJ.f.vsram = vdpw_r_vsram();
		memcpy(WJ.cram, cram, sizeof WJ.cram);
		WJ.f.cram = WJ.cram;
		WJ.f.pal = vdpw_wide_pal();
		WJ.f.sms1 = (system_hw <= SYSTEM_SMS);
		for (i = 0; i < 24; i++) WJ.f.reg[i] = reg[i];
		/* the VRAM blocks written since the last call */
		for (i = 0; i < wide_vn; i++) { WJ.dirty[i] = wide_vl[i]; wide_vd[wide_vl[i]] = 0; }
		WJ.f.dirty = WJ.dirty; WJ.f.ndirty = wide_vn; WJ.f.dirty_all = wide_vall;
		wide_vn = 0; wide_vall = 0;
	}
	if (vdpw_on) vdpw_q_call(wide_job, NULL);
	else wide_job(NULL);
}

void md_host_wide_rows_clear(void) { vdpw_wide_rows_clear(); }
void md_host_wide_row(int plane, int row, unsigned addr, const uint16_t *words, int n) { vdpw_wide_row_set(plane, row, addr, (const uint16 *)words, n); }
void md_host_wide_cache(int on, int check) { vdpw_wide_cache(on, check); }
void md_host_wide_cache_stat(unsigned long out[3]) { vdpw_join(); vdpw_wide_cache_stat(out); }

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

static size_t cd_save_size(void);
size_t md_host_sram_capacity(void) { if (G.loaded && G.cd) return cd_save_size(); return (G.loaded && sram.on) ? SRAM_BYTES : 0; }

size_t md_host_sram_size(void)
{
	int i;
	if (G.loaded && G.cd) return cd_save_size();       /* a formatted backup RAM is a whole image, never cut */
	if (!G.loaded || !sram.on) return 0;
	for (i = SRAM_BYTES - 1; i >= 0; i--)
		if (sram.sram[i] != 0xFF) return (size_t)i + 1;
	return 0;
}

size_t md_host_sram_read(uint8_t *out, size_t cap)
{
	size_t n = md_host_sram_size();
	if (G.loaded && G.cd) {
		size_t a = n < sizeof scd.bram ? n : sizeof scd.bram;
		if (n > cap) return 0;
		memcpy(out, scd.bram, a);
		if (n > a) memcpy(out + a, scd.cartridge.area, n - a);
		return n;
	}
	if (n > cap) n = cap;
	if (n) memcpy(out, sram.sram, n);
	return n;
}

size_t md_host_sram_write(const uint8_t *data, size_t n)
{
	if (G.loaded && G.cd) {
		/* the internal backup RAM, then the RAM cartridge's; an image that is not formatted is left as the BIOS finds it (formatted at load) */
		size_t a = n < sizeof scd.bram ? n : sizeof scd.bram, used = a;
		if (a) memcpy(scd.bram, data, a);
		if (scd.cartridge.id && n > sizeof scd.bram) {
			size_t c = n - sizeof scd.bram, cap = (size_t)scd.cartridge.mask + 1;
			if (c > cap) c = cap;
			memcpy(scd.cartridge.area, data + sizeof scd.bram, c);
			used += c;
		}
		cd_format_bram();
		G.sram_crc = cd_save_crc();
		return used;
	}
	if (!G.loaded || !sram.on) return 0;
	if (n > SRAM_BYTES) n = SRAM_BYTES;
	memcpy(sram.sram, data, n);
	memset(sram.sram + n, 0xFF, SRAM_BYTES - n);          /* the file holds the bytes up to the last used one: the rest of an unwritten chip reads 0xFF */
	G.sram_crc = (uint32_t)crc32(0, sram.sram, SRAM_BYTES);
	return n;
}

int md_host_sram_dirty(void)
{
	if (G.loaded && G.cd) return cd_save_crc() != G.sram_crc;
	if (!G.loaded || !sram.on) return 0;
	return (uint32_t)crc32(0, sram.sram, SRAM_BYTES) != G.sram_crc;
}

void md_host_sram_mark_saved(void) { if (G.loaded) G.sram_crc = G.cd ? cd_save_crc() : (uint32_t)crc32(0, sram.sram, SRAM_BYTES); }
void md_host_sram_mark_unsaved(void) { G.sram_crc = ~G.sram_crc; }

/* ---- save states ----------------------------------------------------------------------------------------- */

static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

size_t md_host_state_size(void) { return G.loaded ? G.state_size : 0; }
unsigned long md_host_carry_refused(void) { return G.carry_refused; }

static size_t state_save_to(uint8_t *buf, size_t cap, int with_crc)
{
	size_t used;
	if (!G.loaded || !G.state_size || cap < G.state_size) return 0;
	fast_savestates = 0;
	used = (size_t)state_save(G.state_scratch);
	if (used + G.carry_len > G.state_payload_cap) return 0;
	memset(buf, 0, STATE_HDR);
	memset(buf + STATE_HDR + used, 0, G.state_size - STATE_HDR - used);   /* the tail is zero so two states differ only where the machine did */
	memcpy(buf, "MDST", 4);
	put32(buf + 4, STATE_VERSION_ID);
	put32(buf + 8, (uint32_t)G.system + (G.cd ? 16u : 0u));       /* a disc's state is never taken for a cartridge's */
	put32(buf + 12, (uint32_t)used);
	memcpy(buf + STATE_HDR, G.state_scratch, used);
	carry_write(buf + STATE_HDR + used);                  /* the sprite lists, FIFO flag, PSG deltas, EEPROM position the core's state lacks */
	put32(buf + G.state_size - STATE_TRAILER, with_crc ? (uint32_t)crc32(0, buf, (unsigned)(G.state_size - STATE_TRAILER)) : 0u);
	return G.state_size;
}

size_t md_host_state_save_mem(uint8_t *buf, size_t cap) { return state_save_to(buf, cap, 1); }
size_t md_host_state_save_quick(uint8_t *buf, size_t cap) { return state_save_to(buf, cap, 0); }

static int state_load_from(const uint8_t *buf, size_t n, int check_crc)
{
	uint32_t used, ver;
	size_t payload_cap;
	if (!G.loaded) return 1;
	if (!buf || memcmp(buf, "MDST", 4)) return 3;
	ver = get32(buf + 4);
	/* version 2 (this build's): the core's state and the carry block. Version 1 (a file written before the carry block existed): the
	 * same header and core state, no block, the size of a state of this game without it. */
	if (ver == STATE_VERSION_ID) { if (n != G.state_size) return 3; payload_cap = G.state_payload_cap; }
	else if (ver == STATE_VERSION_OLD) { if (n != G.state_size_old) return 3; payload_cap = n - STATE_HDR - STATE_TRAILER; }
	else return 3;
	if (get32(buf + 8) != (uint32_t)G.system + (G.cd ? 16u : 0u)) return 3;
	used = get32(buf + 12);
	if (used == 0 || used > payload_cap || used > G.state_payload_cap) return 3;
	if (check_crc && get32(buf + n - STATE_TRAILER) != (uint32_t)crc32(0, buf, (unsigned)(n - STATE_TRAILER))) return 3;
	memcpy(G.state_scratch, buf + STATE_HDR, used);      /* the core wants a writable buffer */
	/* run-ahead's own load keeps the picture just drawn, the pattern cache and the palette tables (the core then marks only the patterns
	 * whose VRAM differs); every other load starts from cleared buffers as before */
	fast_savestates = G.ahead_quiet ? 1 : 0;
	if (!state_load(G.state_scratch)) { fast_savestates = 0; return 3; }
	fast_savestates = 0;
	if (!G.ahead_quiet) G.ring_r = G.ring_w = G.ring_n = 0;      /* run-ahead goes back to a moment whose sound is already in the ring */
	G.reg0_prev = reg[0];
	/* then the machine is exactly the saved one: the picture size the VDP holds (where vertical blank starts), the sprite lists, the PSG's
	 * queued deltas ... (before the geometry below is derived from it) */
	if (ver == STATE_VERSION_ID) carry_read(buf + STATE_HDR + used, payload_cap - used);
	update_viewport();                       /* the window origin is not in the state: derive it now, before the next frame draws */
	bitmap.viewport.changed &= ~9;
	refresh_info();
	return 0;
}

int md_host_state_load_mem(const uint8_t *buf, size_t n) { return state_load_from(buf, n, 1); }
int md_host_state_load_quick(const uint8_t *buf, size_t n) { return state_load_from(buf, n, 0); }

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
	if (G.loaded && G.cd) return 9;        /* RC_CONSOLE_SEGA_CD */
	switch (G.system) { case MD_SYS_MD: return 1; case MD_SYS_SMS: return 11; case MD_SYS_GG: return 15; case MD_SYS_SG: return 33; }
	return 0;
}

int md_host_ra_read(unsigned addr, uint8_t *out, unsigned n)
{
	unsigned i, got = 0;
	if (!G.loaded || !out) return 0;
	if (G.cd) {
		/* rcheevos' Sega CD map: $000000 the 68000's work RAM as held, $010000-$08FFFF the sub CPU's 512 KB program RAM, $090000-$0AFFFF the
		 * first 128 KB of the word RAM as the core holds it in 2M mode (what the core's libretro build hands out as its memory map) */
		if (addr < 0x10000) { for (i = 0; addr + i < 0x10000 && i < n; i++) out[i] = work_ram[addr + i]; return (int)i; }
		if (addr >= 0x10000 && addr < 0x90000) { for (i = 0; addr + i < 0x90000 && i < n; i++) out[i] = scd.prg_ram[addr + i - 0x10000]; return (int)i; }
		if (addr >= 0x90000 && addr < 0xB0000) { for (i = 0; addr + i < 0xB0000 && i < n; i++) out[i] = scd.word_ram_2M[addr + i - 0x90000]; return (int)i; }
		return 0;
	}
	if (G.system == MD_SYS_MD) {
		unsigned end;
		if (addr < 0x10000) { end = 0x10000; for (i = 0; addr + i < end && i < n; i++) out[i] = work_ram[addr + i]; return (int)i; }   /* as held: see host.h */
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
			if ((p->value & 0xFF00) == 0) work_ram[o] = (uint8_t)p->value;                   /* byte store as Genesis Plus GX: work_ram as held (host word order), no ^1 */
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
		else if (!G.cd && address < 0x400000 && (address & 0xFFFFFE) + 2 <= cart.romsize) { /* ROM word */ }
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
	if (G.cd && G.cd_force_region && !region) return 0;       /* the console takes the region of the BIOS it has: Auto keeps it */
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
		{ "vdp_mode", &G.opt_vdp_mode, 0, 2 }, { "gg_band", &G.opt_gg_band, 0, 1 }, { "ram_cart", &G.opt_ram_cart, 0, 6 },
	};
	size_t i;
	int old_nuked = config.ym3438, old_opll = config.opll;
	vdpw_join();                       /* the workers only ever run inside a frame; this is for a caller that changes options mid-frame */
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
		if (!strcmp(name, "interlace") || !strcmp(name, "sms_border") || !strcmp(name, "gg_band")) { bitmap.viewport.changed |= 9; update_viewport(); refresh_info(); }
		return 0;
	}
	return 1;
}

int md_host_get_option(const char *name, int *value)
{
	if (!strcmp(name, "region")) { *value = G.opt_region; return 0; }
	{
		static const char *const names[] = { "fm", "fm_hq", "psg_hq", "ym2413", "opll_nuked", "fm_preamp", "psg_preamp", "lowpass", "mono", "interlace",
			"sprite_limit", "sms_border", "gg_extra", "overscan", "force_dtack", "addr_error", "lcd", "vdp_mode", "gg_band", "ram_cart" };
		int *const vals[] = { &G.opt_fm, &G.opt_fm_hq, &G.opt_psg_hq, &G.opt_ym2413, &G.opt_opll_nuked, &G.opt_fm_preamp, &G.opt_psg_preamp, &G.opt_lowpass,
			&G.opt_mono, &G.opt_interlace, &G.opt_sprite_limit, &G.opt_sms_border, &G.opt_gg_extra, &G.opt_overscan, &G.opt_force_dtack, &G.opt_addr_error,
			&G.opt_lcd, &G.opt_vdp_mode, &G.opt_gg_band, &G.opt_ram_cart };
		size_t i;
		for (i = 0; i < sizeof names / sizeof names[0]; i++) if (!strcmp(names[i], name)) { *value = *vals[i]; return 0; }
	}
	return 1;
}

void md_host_set_log(void (*fn)(const char *line)) { G.log = fn; (void)logf_; }
