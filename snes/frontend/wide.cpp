/*
 * SNES wide view adapter (see wide.h). Copyright CBOC-Vita. MIT licence (see snes/frontend/LICENSE).
 *
 * Per finished frame, on the render thread (the PPU thread when it is on):
 *   1. describe: one widekit layer per BG that the frame shows with a dominant tilemap setup: ring size, tile hashes (map
 *      word + a hash of the tile's graphics, cached per 32 byte VRAM unit by the core's write stamps), write stamps, scroll
 *      per line, graphics identity;
 *   2. widekit: wk_layers_update (what is real: history, lead the game wrote, backdrop layers, HUD lines), wk_map_update /
 *      wk_map_serve (the learned map);
 *   3. candidates: per layer and side the margin strips the core drew (one pass per layer), tiles of the learned map drawn
 *      here from VRAM; wk_compose blends them with the soft edge;
 *   4. exact composite: where every layer of a line is real out to some distance the core's composite pass (all layers,
 *      sprites, windows, colour math) is used there; elsewhere sprites of the margin are laid over the layered picture;
 *      Mode 7 frames use the composite pass for the whole margin;
 *   5. the 256 centre columns are copied from the normal picture's own buffer.
 * Nothing here is read by the emulation.
 */
#include <atomic>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "wide.h"
#include "widecap.h"
#include "wk_types.h"
#include "wk_layers.h"
#include "wk_map.h"
#include "wk_compose.h"
#include "wk_profile.h"

namespace {

enum { MAXH = 240, RING_MAX = 64 * 64, NOUT = 4, MEMO_N = 16, PROV_EXACT = 10, PROV_SOFTL = 12, SOFT_W = 16, COV_ON = 85, COV_OFF = 75 };

static inline uint32_t mix32(uint32_t a, uint32_t b)
{
	a ^= b * 0x9E3779B1u;
	a = (a << 13) | (a >> 19);
	a *= 0x85EBCA6Bu;
	a ^= a >> 15;
	a *= 0xC2B2AE35u;
	return a ^ (a >> 16);
}

static uint32_t crc32_of(const uint8_t *p, size_t n)
{
	static uint32_t tab[256];
	if (!tab[1])
		for (uint32_t i = 0; i < 256; i++) {
			uint32_t c = i;
			for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
			tab[i] = c;
		}
	uint32_t c = 0xFFFFFFFFu;
	for (size_t i = 0; i < n; i++) c = tab[(c ^ p[i]) & 255] ^ (c >> 8);
	return ~c;
}

static uint64_t now_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t) ts.tv_sec * 1000000ull + (uint64_t) ts.tv_nsec / 1000;
}

enum { RUNG_N = 4, WORK_FLOOR = 12, PAD_FADE = 12 };

struct SpinLock {
	std::atomic_flag f = ATOMIC_FLAG_INIT;
	void lock() { while (f.test_and_set(std::memory_order_acquire)) {} }
	void unlock() { f.clear(std::memory_order_release); }
};

/* hash of a tile's graphics, valid while the 32 byte VRAM units under it were not written */
struct TileHash { uint32_t h; uint16_t age; uint8_t ok, blank; };

struct Layer {
	int active;
	int mw, mh, tile_px, bpp, palbase, name, scbase, scsize, bgsize;
	uint32_t hash[RING_MAX], code[RING_MAX], stamp[RING_MAX];
	uint8_t line_on[MAXH];
	uint32_t gh[1024];               /* hash per tile index used by the ring */
	uint8_t used[1024];
	uint32_t gh_prev[1024];
	uint8_t gh_prev_ok[1024];
	uint32_t gid;
	int have_gid;
	uint32_t gen;                    /* the frame generation tcache/tgen are valid for */
	uint32_t tcache[1024];           /* th of a tile id (8x8 graphics, or the 2x2 group of a 16x16 tile), valid when tgen matches */
	uint32_t tgen[1024];
	uint8_t tblank[1024];
	uint8_t bitmap;                  /* the tilemap is a framebuffer ramp: nearly every tile number differs */
	uint8_t rowblank[64];            /* ring tile row is made of blank tiles only (the layer draws nothing on it, anywhere) */
	/* what the last full ring walk saw, so that a frame in which none of it changed does not walk again (describe_layer) */
	uint8_t walk_ok, walk_unsettled;
	int wkey[6];                     /* the Key and the settle count of that walk */
	uint16_t pw[4096], pv[4096];     /* the tilemap words and their write stamps, per 32x32 screen block in address order */
};

/* One captured frame: exactly what the compose half reads (see "Capture and compose" in snes/WIDE-INTEGRATION.md). Margin pass
 * pictures are kept per line, side and distance index: px[pass][y][0][i] is buffer column (m - mh + i) of the core's pass (left
 * margin, i = mh - 1 - d for distance d from the picture), px[pass][y][1][i] is column m + 256 + i (i = d). Only the rows and passes
 * the line says it drew are copied; compose never reads the others. */
enum { SNAP_N = 5, SS_FREE = 0, SS_FILLING = 1, SS_FILLED = 2, SS_TAKEN = 3, SS_COMPOSING = 4 };
struct Snap {
	std::atomic<uint32_t> state;
	uint32_t serial;
	int h, m, mh, ok, wide_hint, co;   /* co: a centre only frame (no margin passes: the ambient frame is all there is) */
	uint32_t clock, epoch;
	struct SWideLine line[SW_ROWS];
	uint16_t cgram[256], colors[256];
	uint16_t uage[2048];
	uint16_t vage[32768];
	uint8_t vram[65536];
	uint16_t centre[SW_ROWS * 256];
	uint16_t px[SW_NPASS][SW_ROWS][2][SNES_WIDE_MAX_MARGIN];
	uint8_t z[SW_NPASS][SW_ROWS][2][SNES_WIDE_MAX_MARGIN];
};

struct Out {
	uint16_t *px;
	uint8_t *prov;
	uint8_t real[MAXH][2];
	snes_wide_stats st;
	int avail;
};

struct Adapter {
	SpinLock lk;
	std::atomic<int> on;
	int margin224, core_margin;
	int period, fade_px, appear, settle, debug, layered;
	int learn = 1;
	int map_safe = 1;
	/* game */
	int have_game;
	uint32_t crc;
	char title[48];
	wk_profile_db *db;
	const wk_profile *prof;
	wk_layers *L;
	wk_map *M;
	wk_compose *C;
	int cH, cM, cF;                  /* what C was created for: picture lines, work margin, full margin */
	wk_frame *fr;
	wk_out *wo;
	Layer lay[4];
	uint16_t *cand[4][2];
	uint8_t *opq[4][2];
	uint16_t bg[MAXH];
	TileHash th2[4096], th4[2048], th8[1024];
	uint16_t pal_prev[256];
	int have_pal_prev;
	uint32_t epoch;
	uint32_t map_saved_cells, map_pending_cells;
	/* output ring */
	Out out[NOUT];
	std::atomic<unsigned> serial;
	std::atomic<unsigned> wr;
	int outW, outH;
	uint16_t xr[MAXH][2][4];   /* per line: the ring-backed real margin of a line whose layers cannot be continued from the map */
	uint8_t xl[MAXH];
	uint8_t bdm[MAXH];                /* line with colour math on the backdrop alone */
	uint8_t softany[MAXH][2];         /* a bit of softm is set on this line and side */
	int lazy = 1;                     /* option "lazy": a side that shows the ambient frame is not composed on the frames whose coverage is not counted */
	snes_wide_pfor pfor = NULL;       /* snes_wide_parallel: the frontend's way to share the lines of a frame between threads */
	void *pfor_user = NULL;
	int nwork = 1;
	uint8_t softm[MAXH][2][SNES_WIDE_MAX_MARGIN];   /* per margin pixel: bit n = layer n shows a learned cell that has not been seen live in this run (blended toward the ambient frame) */
	int soft_on;
	int verify_cache = 0;             /* option "verify_cache": every kept result is also made fresh and compared (the twin's proof; census cache_*) */
	int hud_excl = 1;                 /* option "hud_excl": HUD rows do not count against the Auto coverage */
	int fgfade = 1;                   /* option "fgfade": unknown foreground fades out over fade_px (B) */
	uint16_t tailpx[4][2][MAXH];      /* per layer, side, line: the layer's own pixel at the view edge (colour) ... */
	uint8_t tailop[4][2][MAXH];       /* ... and whether it is opaque */
	/* ambient frame: calm dimmed colours of the scene's own edges, never structure */
	int amb_have;
	int amb_c[2][32][3];       /* per side and 8-line cell: temporal mean colour, Q4 */
	uint16_t amb_px[2][MAXH];  /* the colour per line (RGB565) after the vertical smoothing */
	int amb_rgb[2][MAXH][3];   /* ... as 8-bit channels */
	uint16_t edge_px[2][MAXH * WK_EDGE_N];
	uint8_t amb_dk[SNES_WIDE_MAX_MARGIN + 1];   /* darkening per distance, 0..255 */
	/* Auto: per game and scene */
	std::atomic<int> auto_on{1};
	/* cost ladder (option "policy", default on; the twin's proofs switch it off): measured compose time steps the work margin down a rung
	 * (see work_margin) and, much later and only when it would fit, up again */
	std::atomic<int> policy{1};
	std::atomic<int> pressure{0};     /* option "pressure": the frontend asks for a rung down now (1) or the floor (2); consumed by the next composed frame */
	bool last_up = false;             /* the last ladder step was up */
	int chip = 0;                     /* 1 Super FX (GSU), 2 SA-1, from the cartridge header */
	int rung = 0;
	int cost_us = 13000;              /* option "cost_us": smoothed compose time that is too much for one 60 Hz frame of the drawing thread (16.7 ms less the texture copy and the scaler's share) */
	int cost_frames = 60;             /* ... held for this many frames before a step down */
	int cost_scale = 100;             /* option "cost_scale" (percent): the twin's way to stand in for a slower machine; the ladder sees the measured time times this */
	uint32_t cost_ema_q4 = 0, cost_n = 0, hold_frames = 600, since_down = 0;
	uint32_t last_us[RUNG_N];         /* the smoothed compose time a rung had when it was given up (0 = unknown): the way back up is judged by it */
	int cov_q8[2];             /* smoothed coverage per side, percent * 256 (a scrolling game has real history behind the camera and none ahead: each side decides alone) */
	int auto_state[2] = { 1, 1 };   /* per side: 1 = real picture, 0 = ambient frame */
	int xf[2];                 /* per side cross-fade 0 (wide) .. 32 (ambient) */
	int cov_have;
	snes_wide_census cen;
	uint32_t frame_n;
	uint32_t gen;
	/* capture / compose split */
	Snap *snap[SNAP_N];
	Snap *own;                        /* the snapshot of the single call path */
	std::atomic<uint32_t> snap_serial;
	std::atomic<int> split;           /* 1: the frame hook captures only; the consumer composes (snes_wide_compose) */
	std::atomic<int> cap_mh;          /* the margin the capture copies (= margin224; 0 = off) */
	std::atomic<int> prof_off;        /* the game's profile switches the view off (a hint for capture) */
	std::atomic<unsigned> dropped;    /* snapshots overwritten before they were composed */
	std::atomic<uint32_t> cap_us_q4;  /* smoothed capture time, microseconds x 16 (census) */
	std::atomic<uint32_t> vfy_checks{0}, vfy_bad{0};   /* verify_cache: lines whose built composite was compared with the core's (capture's thread) */
};

static Adapter A;

/* ---- geometry helpers ---------------------------------------------------------------------------------------- */

/* margin for a picture of h lines that keeps the 224 line picture's aspect (70 px at 224 -> 83 at 239) */
static int margin_for(int h)
{
	/* the player's picture shapes scale the width per source column, not per picture height, so a 239 line picture
	 * gets the same margin as a 224 line one */
	(void) h;
	return A.margin224;
}

static inline int fdiv(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }
static inline int fmod_(int a, int b) { int r = a % b; return r < 0 ? r + b : r; }
static inline uint16_t ld16(const uint8_t *p) { return (uint16_t) (p[0] | (p[1] << 8)); }

static int bg_bpp(int mode, int n)
{
	if (mode == 0) return 2;
	if (mode == 1) return n < 2 ? 4 : 2;
	if (mode == 3) return n == 0 ? 8 : 4;
	return 0;
}
static int bg_count(int mode) { return mode == 0 ? 4 : mode == 1 ? 3 : mode == 3 ? 2 : 0; }

/* ---- cost ladder: the wide view is on for every game; what degrades is how much of the margin is composed -------------- */

/* chip in the cartridge: the header's "ROM type" byte ($FFD6 / $7FD6): high nibble 1 = Super FX (GSU), 3 = SA-1, low nibble >= 3 = has a
 * coprocessor. The header whose checksum and complement agree is taken. Those games cost the most (bitmap screens, many render batches), so
 * they start one rung down; the measured compose time decides the rest. */
static int rom_chip(const uint8_t *rom, size_t n)
{
	static const size_t offs[] = { 0x7FC0, 0xFFC0, 0x40FFC0 };
	for (size_t off : offs) {
		if (!rom || n < off + 0x20) continue;
		if ((uint16_t) (ld16(rom + off + 0x1C) ^ ld16(rom + off + 0x1E)) != 0xFFFF) continue;
		const int type = rom[off + 0x16];
		if ((type & 15) < 3) return 0;
		return (type >> 4) == 1 ? 1 : (type >> 4) == 3 ? 2 : 0;
	}
	return 0;
}

/* Rungs: how many columns per side the renderer's passes, widekit and the map are asked for (the "work margin"); the picture keeps its
 * full width, the columns beyond the work margin are the ambient frame (a calm colour field, never structure, so never a wrong tile).
 * 0 full, 1 five eighths, 2 three eighths, 3 the floor: centre only. The core draws no margin passes and takes no capture (PPU thread cost gone,
 * S9xWideCentreOnly), compose has the ambient frame to do; the picture is as wide as ever. A frame that was already in flight when the floor
 * was reached is composed with WORK_FLOOR columns. */
static int work_margin(int rung, int full)
{
	static const int num[RUNG_N] = { 8, 5, 3, 0 };
	if (rung <= 0 || full <= WORK_FLOOR) return full;
	int m = rung >= RUNG_N - 1 ? WORK_FLOOR : (full * num[rung] + 7) / 8;
	if (m < WORK_FLOOR) m = WORK_FLOOR;
	return m;
}

/* ---- tile graphics ------------------------------------------------------------------------------------------- */

static uint32_t tile_hash(const SWideView *v, int bpp, int tile, int name)
{
	const int bytes = bpp * 8;
	const uint32_t a = ((uint32_t) name * 2 + (uint32_t) tile * (uint32_t) bytes) & 0xffff;
	TileHash *c = bpp == 2 ? &A.th2[a >> 4] : bpp == 4 ? &A.th4[a >> 5] : &A.th8[a >> 6];
	uint16_t age = v->uage[a >> 5];
	if (bytes == 64) { uint16_t b = v->uage[(a >> 5) + 1]; if (b > age) age = b; }
	if (c->ok && c->age == age) return c->h;
	uint32_t h = 0x811C9DC5u, any = 0;
	for (int i = 0; i < bytes; i += 4) {
		const uint32_t w = (uint32_t) v->vram[(a + i) & 0xffff] | ((uint32_t) v->vram[(a + i + 1) & 0xffff] << 8) |
		                   ((uint32_t) v->vram[(a + i + 2) & 0xffff] << 16) | ((uint32_t) v->vram[(a + i + 3) & 0xffff] << 24);
		any |= w;
		h = mix32(h, w);
	}
	c->h = h; c->age = age; c->ok = 1; c->blank = any == 0;
	return h;
}

static int tile_blank(int bpp, int tile, int name)
{
	const int bytes = bpp * 8;
	const uint32_t a = ((uint32_t) name * 2 + (uint32_t) tile * (uint32_t) bytes) & 0xffff;
	const TileHash *c = bpp == 2 ? &A.th2[a >> 4] : bpp == 4 ? &A.th4[a >> 5] : &A.th8[a >> 6];
	return c->ok && c->blank;
}

/* one pixel of a tile cell (a learned-map cell: the tilemap word) of layer l: returns the colour index or -1 for transparent */
static int cell_index(const SWideView *v, const Layer *Ly, uint32_t word, int xin, int fy)
{
	const int tp = Ly->tile_px;
	if (word & 0x4000) xin = tp - 1 - xin;
	if (word & 0x8000) fy = tp - 1 - fy;
	int tile = word & 0x3ff;
	if (tp == 16) tile = (tile + (xin >> 3) + ((fy >> 3) << 4)) & 0x3ff;
	xin &= 7; fy &= 7;
	const int bpp = Ly->bpp, bytes = bpp * 8;
	const uint32_t a = ((uint32_t) Ly->name * 2 + (uint32_t) tile * (uint32_t) bytes) & 0xffff;
	const int b = 7 - xin;
	int c = 0;
	for (int p = 0; p < bpp; p += 2) {
		const uint32_t o = (a + (uint32_t) (p >> 1) * 16 + (uint32_t) fy * 2) & 0xffff;
		c |= (((v->vram[o] >> b) & 1) << p) | (((v->vram[(o + 1) & 0xffff] >> b) & 1) << (p + 1));
	}
	if (!c) return -1;
	const int pal = (word >> 10) & 7;
	if (bpp == 8) return c;
	if (bpp == 4) return pal * 16 + c;
	return Ly->palbase + pal * 4 + c;
}

/* All pixels of one row of a tile cell: out[x] (x < tile_px) is what cell_index(v, Ly, word, x, fy) returns. A learned-map margin walks along
 * a cell, so the row is decoded once for its 8 or 16 pixels instead of bit by bit for each. */
static uint64_t g_spread[256];     /* byte j of the entry = bit (7 - j) of the index: a bit plane byte as eight pixels */
static void spread_init(void)
{
	if (g_spread[1]) return;
	for (int v = 255; v >= 0; v--) {
		uint64_t r = 0;
		for (int j = 0; j < 8; j++) if ((v >> (7 - j)) & 1) r |= 1ull << (8 * j);
		g_spread[v] = r;
	}
}

static void cell_row(const SWideView *v, const Layer *Ly, uint32_t word, int fy, int16_t *out)
{
	const int tp = Ly->tile_px, bpp = Ly->bpp, bytes = bpp * 8;
	if (word & 0x8000) fy = tp - 1 - fy;
	const int tile0 = word & 0x3ff, pal = (word >> 10) & 7, hflip = (word & 0x4000) != 0;
	const int base = bpp == 8 ? 0 : bpp == 4 ? pal * 16 : Ly->palbase + pal * 4;
	int16_t col[16];
	for (int g = 0; g < tp / 8; g++) {
		const int tile = tp == 16 ? ((tile0 + g + ((fy >> 3) << 4)) & 0x3ff) : tile0;
		const uint32_t a = ((uint32_t) Ly->name * 2 + (uint32_t) tile * (uint32_t) bytes) & 0xffff;
		uint64_t c = 0;
		for (int p = 0; p < bpp; p += 2) {
			const uint32_t o = (a + (uint32_t) (p >> 1) * 16 + (uint32_t) (fy & 7) * 2) & 0xffff;
			c |= (g_spread[v->vram[o]] << p) | (g_spread[v->vram[(o + 1) & 0xffff]] << (p + 1));
		}
		for (int j = 0; j < 8; j++) {
			const int cj = (int) ((c >> (8 * j)) & 0xff);
			col[g * 8 + j] = (int16_t) (cj ? base + cj : -1);
		}
	}
	if (hflip) for (int x = 0; x < tp; x++) out[x] = col[tp - 1 - x];
	else memcpy(out, col, (size_t) tp * sizeof(int16_t));
}

/* ---- describing a frame -------------------------------------------------------------------------------------- */

struct Key { int scbase, scsize, name, bgsize, bpp, n; };

/* do the 1024 tilemap words (and write stamps) of a screen block at word address wa0 equal what the last walk saw? */
static bool region_same(const SWideView *v, int wa0, const uint16_t *pw, const uint16_t *pv)
{
	int n1 = 0x8000 - wa0;
	if (n1 > 1024) n1 = 1024;
	if (memcmp(v->vram + (size_t) wa0 * 2, pw, (size_t) n1 * 2) || memcmp(v->vage + wa0, pv, (size_t) n1 * 2)) return false;
	if (n1 < 1024 && (memcmp(v->vram, pw + n1, (size_t) (1024 - n1) * 2) || memcmp(v->vage, pv + n1, (size_t) (1024 - n1) * 2))) return false;
	return true;
}

static inline bool line_ok(const SWideLine &L)
{
	return (L.flags & SWL_DRAWN) && !(L.flags & (SWL_NOMARGIN | SWL_BLANK));
}

/* hash of a tile's graphics (a 16x16 tile: of its four), cached per tile id and frame; also its blankness */
/* colour math "add" of two RGB565 pixels as the core does it (5 bit channels, saturating; the sixth green bit repeats the top one) */
static inline uint16_t math_add565(uint16_t a, uint16_t b)
{
	int r = (a >> 11) + (b >> 11), g = ((a >> 6) & 31) + ((b >> 6) & 31), bl = (a & 31) + (b & 31);
	if (r > 31) r = 31;
	if (g > 31) g = 31;
	if (bl > 31) bl = 31;
	return (uint16_t) ((r << 11) | (g << 6) | ((g >> 4) << 5) | bl);
}

static uint32_t layer_tile_th(Layer *Ly, const SWideView *v, int tile, uint32_t gen, bool mark = true)
{
	if (Ly->tgen[tile] == gen) return Ly->tcache[tile];
	Ly->tgen[tile] = gen;
	uint32_t th;
	if (Ly->tile_px == 8) {
		th = tile_hash(v, Ly->bpp, tile, Ly->name);
		Ly->tblank[tile] = tile_blank(Ly->bpp, tile, Ly->name);
	} else {
		int t1 = (tile + 1) & 0x3ff, t2 = (tile + 16) & 0x3ff, t3 = (tile + 17) & 0x3ff;
		uint32_t h0 = tile_hash(v, Ly->bpp, tile, Ly->name), h1 = tile_hash(v, Ly->bpp, t1, Ly->name);
		uint32_t h2 = tile_hash(v, Ly->bpp, t2, Ly->name), h3 = tile_hash(v, Ly->bpp, t3, Ly->name);
		th = mix32(mix32(mix32(h0, h1), h2), h3);
		Ly->tblank[tile] = tile_blank(Ly->bpp, tile, Ly->name) && tile_blank(Ly->bpp, t1, Ly->name) && tile_blank(Ly->bpp, t2, Ly->name) && tile_blank(Ly->bpp, t3, Ly->name);
	}
	Ly->tcache[tile] = th;
	if (mark) { Ly->used[tile] = 1; Ly->gh[tile] = th; }
	return th;
}

static void describe_layer(const SWideView *v, int n, int H, Layer *Ly)
{
	wk_frame *fr = A.fr;
	Key keys[4];
	int cnt[4] = { 0, 0, 0, 0 }, nk = 0, best = -1, i, y;
	Ly->active = 0;
	for (y = 0; y < H; y++) {
		const SWideLine &L = v->line[y];
		Ly->line_on[y] = 0;
		if (!line_ok(L) || (L.flags & SWL_MODE7)) continue;
		if (n >= bg_count(L.mode) || !((L.tm | L.tsx) & (1 << n))) continue;
		Key k = { L.scbase[n], L.scsize[n], L.name[n], L.bgsize[n], bg_bpp(L.mode, n), 0 };
		int j;
		for (j = 0; j < nk; j++)
			if (!memcmp(&keys[j], &k, sizeof k)) break;
		if (j == nk) {
			if (nk == 4) continue;
			keys[nk++] = k;
		}
		cnt[j]++;
	}
	for (i = 0; i < nk; i++)
		if (best < 0 || cnt[i] > cnt[best]) best = i;
	if (best < 0 || cnt[best] < 8) return;

	const Key &K = keys[best];
	Ly->mw = (K.scsize & 1) ? 64 : 32;
	Ly->mh = (K.scsize & 2) ? 64 : 32;
	Ly->tile_px = K.bgsize ? 16 : 8;
	Ly->bpp = K.bpp;
	Ly->name = K.name; Ly->scbase = K.scbase; Ly->scsize = K.scsize; Ly->bgsize = K.bgsize;
	Ly->palbase = (K.bpp == 2 && v->line[0].mode == 0) ? n * 32 : 0;

	const uint32_t clk = v->clock;
	const uint16_t clk16 = (uint16_t) clk;
	const int mw = Ly->mw, mh = Ly->mh;
	const int sc[4] = { K.scbase,
		(K.scsize & 1) ? K.scbase + 1024 : K.scbase,
		0, 0 };
	int sc2 = (K.scsize & 2) ? sc[1] + 1024 : K.scbase;
	int sc3 = (K.scsize & 1) ? sc2 + 1024 : sc2;
	const int scr[4] = { sc[0] & 0x7fff, sc[1] & 0x7fff, sc2 & 0x7fff, sc3 & 0x7fff };
	const uint32_t settle_n = (uint32_t) (A.settle ? A.settle : 1);
	/* A frame in which no tilemap word, no write stamp and no graphics of a tile in use changed, after a walk in which every cell had settled, gives the
	 * walk's results again (they depend on the clock only for cells written within the last settle frames): the ring walk is not made. */
	bool skip_walk = false;
	{
		const int wkey_now[6] = { K.scbase, K.scsize, K.name, K.bgsize, K.bpp, (int) settle_n };
		if (Ly->walk_ok && !Ly->walk_unsettled && !memcmp(Ly->wkey, wkey_now, sizeof wkey_now)) {
			const int nb = (mw / 32) * (mh / 32);
			bool same = true;
			for (int b = 0; b < nb && same; b++) same = region_same(v, scr[b], Ly->pw + b * 1024, Ly->pv + b * 1024);
			if (same) {
				const uint32_t g2 = ++A.gen;
				for (i = 0; i < 1024 && same; i++)
					if (Ly->used[i]) same = layer_tile_th(Ly, v, i, g2, false) == Ly->gh[i];
				if (same) { Ly->gen = g2; skip_walk = true; }
			}
		}
		memcpy(Ly->wkey, wkey_now, sizeof wkey_now);
	}
	/* verify_cache: make the walk anyway and compare it with what the kept result says */
	static uint32_t v_code[4096], v_hash[4096], v_stamp[4096];
	uint8_t v_row[64], v_bitmap = 0;
	uint32_t v_gid = 0;
	const bool verifying = skip_walk && A.verify_cache;
	if (verifying) {
		memcpy(v_code, Ly->code, sizeof v_code); memcpy(v_hash, Ly->hash, sizeof v_hash); memcpy(v_stamp, Ly->stamp, sizeof v_stamp);
		memcpy(v_row, Ly->rowblank, sizeof v_row); v_bitmap = Ly->bitmap; v_gid = Ly->gid;
		skip_walk = false;
	}
	const uint32_t gen = skip_walk ? Ly->gen : ++A.gen;
	if (!skip_walk) {
	memset(Ly->used, 0, sizeof Ly->used);
	memset(Ly->rowblank, 1, sizeof Ly->rowblank);
	Ly->gen = gen;
	Ly->walk_unsettled = 0;
	for (int ry = 0; ry < mh; ry++)
		for (int rx = 0; rx < mw; rx++) {
			const int k = ry * mw + rx;
			const int s = ((mh == 64 && ry >= 32) ? 2 : 0) + ((mw == 64 && rx >= 32) ? 1 : 0);
			const int wa = (scr[s] + (ry & 31) * 32 + (rx & 31)) & 0x7fff;
			const uint32_t word = ld16(v->vram + wa * 2);
			const int tile = (int) (word & 0x3ff);
			uint32_t th;
			th = layer_tile_th(Ly, v, tile, gen);
			uint32_t st = 0;
			const uint16_t w16 = v->vage[wa];
			if (w16) {
				uint32_t age = (uint16_t) (clk16 - w16);
				if (age < 32768 && age <= clk) st = clk - age;
			}
			if (!Ly->tblank[tile]) Ly->rowblank[ry] = 0;
			Ly->code[k] = word;
			Ly->hash[k] = mix32(word * 0x10001u + 0x1234567u, th);
			Ly->stamp[k] = st;
			const bool mid = st && st == clk;               /* written during the picture */
			if (mid || (st && clk - st < settle_n - 1)) {
				Ly->hash[k] ^= 0x7F4A7C15u * (uint32_t) (clk | 1u);
				Ly->stamp[k] = 0xFFFFFFFFu;
				Ly->code[k] = WK_CODE_NOREC;
				Ly->walk_unsettled = 1;
			}
			{
				const int pk = s * 1024 + (ry & 31) * 32 + (rx & 31);
				memcpy(&Ly->pw[pk], v->vram + wa * 2, 2);
				Ly->pv[pk] = w16;
			}
		}
	Ly->walk_ok = 1;

	{
		/* a tilemap in which almost every visible tile number differs is a framebuffer (Super FX, bitmap games), not a level */
		uint8_t seen[1024];
		memset(seen, 0, sizeof seen);
		int distinct = 0, n = 0;
		for (int ry = 0; ry < 28 && ry < mh; ry++)
			for (int rx = 0; rx < 32 && rx < mw; rx++) {
				const int t = (int) (Ly->code[ry * mw + rx] & 0x3ff);
				n++;
				if (!seen[t]) { seen[t] = 1; distinct++; }
			}
		Ly->bitmap = n > 0 && distinct * 10 >= n * 9;
	}
	}      /* if (!skip_walk) */

	/* graphics identity: kept through cosmetic animation (a few tiles changing), new for a tile set swap */
	{
		int nused = 0, same = 0;
		for (i = 0; i < 1024; i++) {
			if (!Ly->used[i]) continue;
			nused++;
			if (Ly->gh_prev_ok[i] && Ly->gh_prev[i] == Ly->gh[i]) same++;
		}
		/* a tile set swap changes most of the used tiles; a handful animating (blocks, water, coins) is cosmetic and must not look
		 * like a new scene (widekit would forget the columns it has seen): new identity from a third of the tiles and 8 of them */
		const int changed = nused - same;
		if (!Ly->have_gid || (changed * 3 >= nused && changed >= 8)) {
			uint32_t g = (uint32_t) K.bpp * 7919u + (uint32_t) K.name;
			for (i = 0; i < 1024; i++) if (Ly->used[i]) g = mix32(g, Ly->gh[i]);
			Ly->gid = g; Ly->have_gid = 1;
		}
		memset(Ly->gh_prev_ok, 0, sizeof Ly->gh_prev_ok);
		for (i = 0; i < 1024; i++) if (Ly->used[i]) { Ly->gh_prev[i] = Ly->gh[i]; Ly->gh_prev_ok[i] = 1; }
	}

	if (verifying) {
		A.cen.cache_checks++;
		if (memcmp(v_code, Ly->code, (size_t) (mw * mh) * 4) || memcmp(v_hash, Ly->hash, (size_t) (mw * mh) * 4) ||
		    memcmp(v_stamp, Ly->stamp, (size_t) (mw * mh) * 4) || memcmp(v_row, Ly->rowblank, (size_t) mh) || v_bitmap != Ly->bitmap || v_gid != Ly->gid)
			A.cen.cache_mismatches++;
	}
	wk_layer *L = &fr->layer[n];
	memset(L, 0, sizeof *L);
	L->active = 1;
	L->role = WK_ROLE_AUTO;
	L->priority = 3 - n;                 /* BG1 on top */
	L->map_w = mw; L->map_h = mh; L->tile_px = Ly->tile_px;
	L->hash = Ly->hash; L->code = Ly->code; L->stamp = Ly->stamp;
	L->map_by_cell = 1;
	L->gfx_id = Ly->gid ^ (uint32_t) (mw * 977 + mh * 131 + Ly->tile_px * 7 + K.bpp);
	L->line_on = Ly->line_on;
	int ndom = 0;
	for (y = 0; y < H; y++) {
		const SWideLine &Ln = v->line[y];
		const bool on = line_ok(Ln) && !(Ln.flags & SWL_MODE7) && n < bg_count(Ln.mode) && ((Ln.tm | Ln.tsx) & (1 << n)) &&
		                Ln.scbase[n] == K.scbase && Ln.scsize[n] == K.scsize && Ln.name[n] == K.name &&
		                Ln.bgsize[n] == K.bgsize && bg_bpp(Ln.mode, n) == K.bpp;
		Ly->line_on[y] = on;
		L->scroll_x[y] = Ln.hofs[n];
		L->row_y[y] = (int32_t) Ln.vofs[n] + y;
		ndom += on;
	}
	/* off lines follow the previous line so that they do not look like a scroll difference */
	{
		int first = 0, last = -1;                 /* the first line that is on; the last one above y */
		while (first < H && !Ly->line_on[first]) first++;
		for (y = 0; y < H; y++) {
			if (Ly->line_on[y]) { last = y; continue; }
			const int j = last >= 0 ? last : first;
			if (j < H) { L->scroll_x[y] = L->scroll_x[j]; L->row_y[y] = L->row_y[j]; }
		}
	}
	(void) ndom;
	Ly->active = 1;
}

/* ---- output -------------------------------------------------------------------------------------------------- */

static void work_free(void);
static int work_make(void);
static void amb_tab_build(int M);

static void free_objects(void)
{
	work_free();
	if (A.C) wk_compose_destroy(A.C);
	A.C = NULL; A.cH = A.cM = A.cF = 0;
	for (int i = 0; i < NOUT; i++) { free(A.out[i].px); free(A.out[i].prov); A.out[i].px = NULL; A.out[i].prov = NULL; }
	for (int l = 0; l < 4; l++) for (int s = 0; s < 2; s++) { free(A.cand[l][s]); free(A.opq[l][s]); A.cand[l][s] = NULL; A.opq[l][s] = NULL; }
}

static int make_compose(int H, int Mh, int Mf)
{
	if (A.C && A.cH == H && A.cM == Mh && A.cF == Mf)
		return 1;
	wk_compose_cfg cc;
	if (A.C) { wk_compose_destroy(A.C); A.C = NULL; }
	wk_compose_cfg_default(&cc);
	cc.period = A.period;
	cc.reveal_prov = 1;
	cc.fg_fade_px = A.fgfade ? 24 : 0;
	cc.fade_px = A.fade_px > 0 ? A.fade_px : 24;
	cc.soft_blur = 0; cc.soft_sample = 1; cc.soft_darken = 45; cc.soft_always = 1;   /* the base is the ambient colours below */
	for (int d = 0; d < Mf; d++) {      /* the ambient frame's darkening is a function of the distance in the full margin, whatever the work margin */
		int t = d * 256 / Mf, ease = (t * t) >> 8;
		A.amb_dk[d] = (uint8_t) (256 - (45 * 256 / 100) * ease / 256 > 255 ? 255 : 256 - (45 * 256 / 100) * ease / 256);
	}
	if (A.appear > 0) cc.appear_frames = A.appear;
	A.C = wk_compose_create(&cc, 256, H, Mh);
	if (!A.C) return 0;
	if (!work_make()) { wk_compose_destroy(A.C); A.C = NULL; return 0; }
	amb_tab_build(Mf);
	spread_init();
	A.cH = H; A.cM = Mh; A.cF = Mf;
	for (int i = 0; i < NOUT; i++) {
		if (A.out[i].px && A.out[i].prov) continue;   /* the output ring never moves while a consumer may hold a picture of it */
		free(A.out[i].px); free(A.out[i].prov);
		A.out[i].px = (uint16_t *) calloc((size_t) (256 + 2 * SNES_WIDE_MAX_MARGIN) * MAXH, 2);
		A.out[i].prov = (uint8_t *) calloc((size_t) (256 + 2 * SNES_WIDE_MAX_MARGIN) * MAXH, 1);
		if (!A.out[i].px || !A.out[i].prov) return 0;
	}
	for (int l = 0; l < 4; l++)
		for (int s = 0; s < 2; s++) {
			free(A.cand[l][s]); free(A.opq[l][s]);
			A.cand[l][s] = (uint16_t *) calloc((size_t) MAXH * Mh, 2);
			A.opq[l][s] = (uint8_t *) calloc((size_t) MAXH * Mh, 1);
			if (!A.cand[l][s] || !A.opq[l][s]) return 0;
		}
	return 1;
}

static void publish(Out *o, int avail)
{
	o->avail = avail;
	o->st.available = avail;
	A.wr.fetch_add(1, std::memory_order_release);
	A.serial.fetch_add(1, std::memory_order_release);
}

static void fallback(Out *o, int reason)
{
	A.cen.frames_fallback++;
	if (reason == 0) A.cen.fb_nothing_drawn++;
	else if (reason == 1) A.cen.fb_profile_off++;
	else A.cen.fb_setup++;
	publish(o, 0);
}

static void profile_lookup(void)
{
	A.prof = NULL;
	if (A.db && A.have_game) A.prof = wk_profile_find(A.db, A.crc, A.title);
	if (A.L) wk_layers_set_profile(A.L, A.prof);
	A.prof_off.store(A.prof && A.prof->enable == WK_ENABLE_OFF ? 1 : 0);
}

static void make_map(void)
{
	wk_map_cfg mc;
	if (A.M || !A.have_game || !A.learn) return;
	wk_map_cfg_default(&mc);
	mc.max_cells = 32768;
	if (A.map_safe) {
		/* what makes a learned cell safe to show (snes/WIDE-INTEGRATION.md, "Learned map safety"); map_safe is a bit mask
		 * for the tests (1 = all): 2 strict bind, 4 match by tilemap word, 8 drop a contradicted room, 16 seam check,
		 * 64 strict verify with instant revoke, 128 cells not seen live in this run are shown softly */
		const int f = A.map_safe == 1 ? 254 : A.map_safe;
		A.soft_on = (f & 128) != 0;
		if (f & 128) mc.live_flags = 1;
		if (f & 2) { mc.bind_pct = 98; mc.bind_frames = 8; }
		if (f & 4) mc.match_by_code = 1;
		if (f & 8) mc.drop_on_revoke = 1;
		if (f & 16) mc.seam_tiles = 2;
		if (f & 64) { mc.verify_pct = 98; mc.revoke_frames = 1; }
	}
	A.M = wk_map_create(&mc, A.crc);
}

/* ---- ambient frame -------------------------------------------------------------------------------------------- */

static const uint8_t kBay4[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

/* the mean colour of the picture's own left and right edge regions (48 columns), per 8 line cell, low passed in time and
 * smoothed over the lines: no structure, no stripes, never a copy of anything the game drew there */
static void amb_update(const SWideView *v, int H, bool reset)
{
	const int NC = (H + 7) / 8;
	int cur[2][32][3];
	for (int s = 0; s < 2; s++)
		for (int k = 0; k < NC; k++) {
			int r = 0, g = 0, b = 0, n = 0;
			for (int y = k * 8; y < k * 8 + 8 && y < H; y += 2) {
				const uint16_t *row = v->centre + (size_t) y * v->centre_pitch + (s ? 208 : 0);
				for (int x = 0; x < 48; x += 2) {
					const unsigned p = row[x];
					r += (p >> 11) & 31; g += (p >> 5) & 63; b += p & 31; n++;
				}
			}
			if (!n) n = 1;
			cur[s][k][0] = (r * 255 / 31) * 16 / n; cur[s][k][1] = (g * 255 / 63) * 16 / n; cur[s][k][2] = (b * 255 / 31) * 16 / n;
		}
	if (!A.amb_have || reset) {
		for (int s = 0; s < 2; s++) for (int k = 0; k < NC; k++) for (int c = 0; c < 3; c++) A.amb_c[s][k][c] = cur[s][k][c];
		A.amb_have = 1;
	} else {
		for (int s = 0; s < 2; s++) for (int k = 0; k < NC; k++) for (int c = 0; c < 3; c++) A.amb_c[s][k][c] += (cur[s][k][c] - A.amb_c[s][k][c]) / 8;
	}
	for (int s = 0; s < 2; s++) {
		int a[32][3];
		for (int k = 0; k < NC; k++) for (int c = 0; c < 3; c++) a[k][c] = A.amb_c[s][k][c];
		for (int pass = 0; pass < 3; pass++) {
			int t[32][3];
			for (int k = 0; k < NC; k++)
				for (int c = 0; c < 3; c++) {
					const int l = a[k > 0 ? k - 1 : 0][c], r = a[k < NC - 1 ? k + 1 : NC - 1][c];
					t[k][c] = (l + 2 * a[k][c] + r) / 4;
				}
			memcpy(a, t, sizeof a);
		}
		for (int y = 0; y < H; y++) {
			/* cell centres at 8k + 3.5: linear between the two nearest */
			int pos = (y * 2 - 7);                      /* (y - 3.5) * 2 */
			int k0 = pos >= 0 ? pos / 16 : -1;
			int fr = pos - k0 * 16;                     /* 0..15 of 16 */
			if (k0 < 0) { k0 = 0; fr = 0; }
			int k1 = k0 + 1 < NC ? k0 + 1 : NC - 1;
			if (k0 >= NC - 1) { k0 = k1 = NC - 1; fr = 0; }
			int rgb[3];
			for (int c = 0; c < 3; c++) {
				rgb[c] = (a[k0][c] * (16 - fr) + a[k1][c] * fr) / 16 / 16;   /* Q4 -> 8 bit */
				A.amb_rgb[s][y][c] = rgb[c] > 255 ? 255 : rgb[c];
			}
			A.amb_px[s][y] = (uint16_t) (((A.amb_rgb[s][y][0] >> 3) << 11) | ((A.amb_rgb[s][y][1] >> 2) << 5) | (A.amb_rgb[s][y][2] >> 3));
			uint16_t *e = &A.edge_px[s][(size_t) y * WK_EDGE_N];
			for (int i = 0; i < WK_EDGE_N; i++) e[i] = A.amb_px[s][y];
		}
	}
}

/* The ambient row (distances 0 .. M - 1) of a line and side: a pure function of the line's ambient colour, the side and the dither phase,
 * made 8 pixels a step by widekit's soft row kernel from two small tables per margin (the darkening per distance, the dither per phase). */
struct AmbTab { int m; uint16_t f[SNES_WIDE_MAX_MARGIN + 8]; uint8_t th[8][SNES_WIDE_MAX_MARGIN + 8]; };
static AmbTab g_amb;

static void amb_tab_build(int M)
{
	memset(&g_amb, 0, sizeof g_amb);
	for (int d = 0; d < M; d++) g_amb.f[d] = A.amb_dk[d];
	for (int ph = 0; ph < 8; ph++) {
		const int s = ph >> 2, yq = ph & 3;
		for (int d = 0; d < M; d++) {
			const int xs = s == 0 ? M - 1 - d : M + 256 + d;
			g_amb.th[ph][d] = kBay4[yq][xs & 3];
		}
	}
	g_amb.m = M;
}

static void amb_fill(uint16_t *dst, int s, int y, int M, uint32_t *checks, uint32_t *mismatches)
{
	const uint16_t key = A.amb_px[s][y];
	const int kr = (key >> 11) & 31, kg = (key >> 5) & 63, kb = key & 31;
	const int r8 = kr * 255 / 31, g8 = kg * 255 / 63, b8 = kb * 255 / 31;
	wk_soft_row(dst, M, r8, g8, b8, g_amb.f, g_amb.th[(y & 3) | (s << 2)]);
	if (A.verify_cache) {        /* the closed form the kernel and its tables stand for */
		(*checks)++;
		for (int d = 0; d < M; d++) {
			const int f = A.amb_dk[d], xs = s == 0 ? M - 1 - d : M + 256 + d, th = kBay4[y & 3][xs & 3];
			int rr = ((r8 * f) >> 8) + (th >> 1), gg = ((g8 * f) >> 8) + (th >> 2), bb = ((b8 * f) >> 8) + (th >> 1);
			rr = rr > 255 ? 255 : rr; gg = gg > 255 ? 255 : gg; bb = bb > 255 ? 255 : bb;
			if (dst[d] != (uint16_t) (((rr >> 3) << 11) | ((gg >> 2) << 5) | (bb >> 3))) { (*mismatches)++; break; }
		}
	}
}

/* w (0..32) of the ambient pixel over the margin pixel */
static inline uint16_t mix565(uint16_t a, uint16_t amb, int w)
{
	const int ar = a >> 11, ag = (a >> 5) & 63, ab = a & 31;
	const int br = amb >> 11, bg = (amb >> 5) & 63, bb = amb & 31;
	return (uint16_t) (((ar + (br - ar) * w / 32) << 11) | ((ag + (bg - ag) * w / 32) << 5) | (ab + (bb - ab) * w / 32));
}

/* ---- capture (the frame hook's own work) ------------------------------------------------------------------------- */

static Snap *snap_alloc(void) { return new (std::nothrow) Snap(); }

/* Option "verify_cache" (the twin's proof): the core drew the composite of this line whole as well; what compose would build for it (derive_composite:
 * the sprite pass and the main screen's BG passes by depth over the backdrop) is compared over the whole margin the core drew, both sides. */
static void verify_composite(const SWideView *v, const SWideLine &L, int y)
{
	const size_t row = (size_t) y * SW_PITCH;
	const int Mc = v->m;
	bool bad = false;
	for (int x = 0; x < 2 * Mc + 256 && !bad; x++) {
		if (x == Mc) x = Mc + 256;
		uint16_t px = (L.pflags & SWLP_BASE) ? v->pass_px[SWP_COMPOSITE][row + x] : L.backdrop;
		uint8_t zb = 0;
		for (int p = SWP_OBJ; p < SW_NPASS; p++) {
			if (!((L.pass >> p) & 1) || (p >= SWP_BG0 && !((L.tm >> (p - SWP_BG0)) & 1))) continue;
			const uint8_t z = v->pass_z[p][row + x];
			if (z > zb) { zb = z; px = v->pass_px[p][row + x]; }
		}
		bad = px != v->verify_px[row + x];
	}
	A.vfy_checks.fetch_add(1, std::memory_order_relaxed);
	if (bad) A.vfy_bad.fetch_add(1, std::memory_order_relaxed);
}

/* copies what compose needs out of the core's view; touches no compose state, so it can run on any thread while compose runs */
static void capture(const SWideView *v, Snap *sn)
{
	const int H = v->h;
	int Mh = A.cap_mh.load(std::memory_order_relaxed);
	if (v->m > 0 && Mh > v->m) Mh = v->m;     /* a frame drawn just before the core margin followed the rung up: the narrower one it has */
	sn->h = H; sn->m = v->m; sn->mh = Mh; sn->ok = v->ok; sn->clock = v->clock; sn->epoch = v->epoch; sn->wide_hint = 0; sn->co = 0;
	if (H <= 0 || H > MAXH) return;
	if (v->m == 0) {         /* a centre only frame: the picture and nothing else */
		sn->co = 1; sn->mh = 0;
		sn->wide_hint = (v->ok && !A.prof_off.load(std::memory_order_relaxed)) ? 1 : 0;
		if (sn->wide_hint) for (int y = 0; y < H; y++) memcpy(sn->centre + (size_t) y * 256, v->centre + (size_t) y * v->centre_pitch, 256 * 2);
		return;
	}
	memcpy(sn->line, v->line, (size_t) H * sizeof(SWideLine));
	int nd = 0, nblank = 0, n7 = 0;
	for (int y = 0; y < H; y++) {
		const SWideLine &L = sn->line[y];
		if (!(L.flags & SWL_VALID)) continue;
		if (L.flags & SWL_BLANK) nblank++;
		else if (!(L.flags & SWL_NOMARGIN) && (L.flags & SWL_DRAWN)) { nd++; if (L.flags & SWL_MODE7) n7++; }
	}
	if (!v->ok || Mh <= 0 || v->m < Mh || (nd == 0 && nblank == 0)) return;
	sn->wide_hint = A.prof_off.load(std::memory_order_relaxed) ? 0 : 1;
	memcpy(sn->cgram, v->cgram, sizeof sn->cgram);
	memcpy(sn->colors, v->colors, sizeof sn->colors);
	if (n7 * 2 <= nd) {      /* a Mode 7 frame (the margin is the core's composite) describes no layer: compose reads no tilemap, stamp or graphics */
		memcpy(sn->uage, v->uage, sizeof sn->uage);
		memcpy(sn->vage, v->vage, sizeof sn->vage);
		memcpy(sn->vram, v->vram, sizeof sn->vram);
	}
	for (int y = 0; y < H; y++) memcpy(sn->centre + (size_t) y * 256, v->centre + (size_t) y * v->centre_pitch, 256 * 2);
	const int Mc = v->m;
	for (int y = 0; y < H; y++) {
		const SWideLine &L = sn->line[y];
		if (!line_ok(L)) continue;
		if (v->verify_lines && v->verify_lines[y] && (L.pflags & SWLP_NOCOMP)) verify_composite(v, L, y);
		for (int p = 0; p < SW_NPASS; p++) {
			if (!((L.pass >> p) & 1)) continue;
			/* passes the core left out because compose does not read them on such a line (widecap.h, SWLP_*) */
			if (p == SWP_COMPOSITE && (L.pflags & SWLP_NOCOMP) && !(L.pflags & SWLP_BASE)) continue;
			if (p >= SWP_BG0 && (L.pflags & SWLP_NOLAYERS)) continue;
			const size_t row = (size_t) y * SW_PITCH;
			/* the one sub screen layer's pass is the composite pass's picture (the backdrop with that layer: every pixel drawn, no depth read) */
			const bool base = p >= SWP_BG0 && (L.pflags & SWLP_TSXBASE) && L.tsx == (1 << (p - SWP_BG0));
			const uint16_t *src = v->pass_px[base ? SWP_COMPOSITE : p];
			memcpy(sn->px[p][y][0], src + row + (Mc - Mh), (size_t) Mh * 2);
			memcpy(sn->px[p][y][1], src + row + Mc + 256, (size_t) Mh * 2);
			if (p && !base) {
				memcpy(sn->z[p][y][0], v->pass_z[p] + row + (Mc - Mh), (size_t) Mh);
				memcpy(sn->z[p][y][1], v->pass_z[p] + row + Mc + 256, (size_t) Mh);
			}
		}
	}
}

/* a slot to capture into: a free one, else the oldest one nobody composed (counted as dropped), else the oldest taken one */
static Snap *pool_claim(void)
{
	for (int pass = 0; pass < 3; pass++) {
		Snap *best = NULL;
		for (int i = 0; i < SNAP_N; i++) {
			Snap *s = A.snap[i];
			if (!s) continue;
			const uint32_t st = s->state.load(std::memory_order_acquire);
			const uint32_t want = pass == 0 ? SS_FREE : pass == 1 ? SS_FILLED : SS_TAKEN;
			if (st != want) continue;
			if (!best || (int32_t) (s->serial - best->serial) < 0) best = s;
		}
		if (!best) continue;
		uint32_t exp = pass == 0 ? SS_FREE : pass == 1 ? SS_FILLED : SS_TAKEN;
		if (best->state.compare_exchange_strong(exp, SS_FILLING, std::memory_order_acq_rel)) {
			if (pass) A.dropped.fetch_add(1);
			return best;
		}
		pass--;   /* lost a race with the consumer: look again */
	}
	return NULL;
}

/* ---- the frame ------------------------------------------------------------------------------------------------ */

/* the cost ladder: called once per composed frame with its time. `co` = the frame was a centre only one (no information about what a wider rung costs) */
static void apply_core_rung(void)
{
	S9xWideCentreOnly(A.rung >= RUNG_N - 1);
	S9xWideCoreMargin(A.rung >= 1 && A.rung < RUNG_N - 1 ? work_margin(A.rung, A.margin224) : 0);   /* the renderer's passes follow the rung too */
}

static void ladder_step(int Mh, uint32_t us, bool co)
{
	if (!A.policy.load()) { A.pressure.store(0); return; }
	A.since_down++;
	const int pr = A.pressure.exchange(0);
	if (pr > 0 && A.rung < RUNG_N - 1) {
		/* the frontend sees pressure (the pacer thins frames, the governor reports a bad second): down now, whatever compose took */
		A.last_us[A.rung] = A.cost_ema_q4 / 16;
		A.rung = pr >= 2 ? RUNG_N - 1 : A.rung + 1; A.cen.rung_changes++;
		A.cost_n = 0; A.since_down = 0; A.last_up = false;
		A.cap_mh.store(work_margin(A.rung, A.margin224));
		apply_core_rung();
		return;
	}
	if (pr > 0) A.since_down = 0;            /* at the floor already: stay there while the pressure lasts */
	if (!co && Mh == A.cap_mh.load()) {      /* only a frame composed at the margin now asked for: one from before a step must not reseed the average */
		const uint32_t q4 = (uint32_t) ((uint64_t) us * (uint32_t) A.cost_scale / 100) * 16;
		A.cost_ema_q4 = A.cost_n ? A.cost_ema_q4 + (int32_t) (q4 - A.cost_ema_q4) / 32 : q4;
		A.cost_n++;
		A.cen.compose_us = A.cost_ema_q4 / 16;
	}
	const uint32_t ema = A.cost_ema_q4 / 16;
	if (!co && A.cost_n >= (uint32_t) A.cost_frames && ema > (uint32_t) A.cost_us && A.rung < RUNG_N - 1) {
		/* over the budget for long enough: one rung down. A step down soon after a step up makes the next wait longer. */
		if (A.last_up && A.since_down < 3600 && A.hold_frames < 7200) A.hold_frames *= 2;
		A.last_us[A.rung] = ema;
		A.rung++; A.cen.rung_changes++;
		A.cost_n = 0; A.since_down = 0; A.last_up = false;
		A.cap_mh.store(work_margin(A.rung, A.margin224));
		apply_core_rung();
	} else if (A.rung > 0 && A.since_down >= A.hold_frames && (co || A.cost_n >= (uint32_t) A.cost_frames)) {
		/* one rung up after hold_frames at this one, if the wider rung would fit: from its own time when it was given up (not trusted after a
		 * minute: the scene may be easier now, that is a probe), else this rung's time scaled to the wider margin */
		const int up = work_margin(A.rung - 1, A.margin224);
		uint64_t pred;
		if (A.last_us[A.rung - 1] && A.since_down < 3600) pred = A.last_us[A.rung - 1];
		else if (co) pred = 0;
		else pred = (uint64_t) ema * (uint32_t) up / (uint32_t) (Mh ? Mh : 1);
		if (pred <= (uint64_t) A.cost_us * 3 / 4) {
			A.rung--; A.cen.rung_changes++;
			A.cost_n = 0; A.since_down = 0; A.last_us[A.rung] = 0; A.last_up = true;
			A.cap_mh.store(up);
			apply_core_rung();
		}
	}
}

/* ---- the frame: a part that one thread does (description, widekit's analysis, the Auto decision) and a part per picture line (candidates,
 * composition, exact composite, ambient frame) that the lines' workers share (snes_wide_parallel; one worker = the caller alone) --------- */

struct Acc {         /* what the lines of one worker add up */
	long sl, sr;
	int nreal, lines_exact, lines_hud;
	uint32_t reason[SNES_WIDE_LR_N];
	uint64_t ctm, cln, cbd, cun, csf, crv, cpad, cnum[2], ctot[2];
	uint32_t cache_checks, cache_mismatches;
};

/* the learned map's answer for the cells beside one tile row of a layer and side: lines of the same tile row with the same scroll ask for the same cells */
struct CellMemo {
	uint32_t gen;
	int32_t wx, wrow;
	uint8_t st[MEMO_N];          /* 0 not asked, 1 the map has no cell, 2 a cell */
	uint8_t hok[MEMO_N], blank[MEMO_N];   /* its tile's graphics are what they were when it was learned; the tile is blank */
	wk_cell cell[MEMO_N];
};

struct Work {        /* one worker's own */
	wk_compose_scratch *scr;
	Acc acc;
	CellMemo memo[4][2];
	uint32_t amb_gen[2];
	int amb_y[2];
	uint16_t amb[2][SNES_WIDE_MAX_MARGIN + 8];
	uint16_t t16[SNES_WIDE_MAX_MARGIN + 8];
	uint8_t t8[SNES_WIDE_MAX_MARGIN + 16];
};

struct Frame {       /* the frame being composed: read only while the lines run */
	const Snap *sn;
	const SWideView *v;
	Out *o;
	int H, Mh, Mfull, OW, nlay, fade;
	uint32_t gen;                /* of the workers' memos */
	bool skip[2];                /* this side shows the ambient frame and nothing reads its composed pixels on this frame: they are not made */
	bool covf;                   /* the coverage is counted on this frame */
	bool sprites_off;
	int xf[2];                   /* the cross-fade of this frame (set before fin_line) */
	wk_compose_in ci;
};

static Work *g_work[SNES_WIDE_MAX_WORKERS];
static uint32_t g_fgen;

static void work_free(void)
{
	for (int i = 0; i < SNES_WIDE_MAX_WORKERS; i++)
		if (g_work[i]) { wk_compose_scratch_destroy(g_work[i]->scr); free(g_work[i]); g_work[i] = NULL; }
}

static int work_make(void)
{
	work_free();
	for (int i = 0; i < SNES_WIDE_MAX_WORKERS; i++) {
		g_work[i] = (Work *) calloc(1, sizeof(Work));
		if (!g_work[i]) return 0;
		g_work[i]->scr = wk_compose_scratch_create(A.C);
		if (!g_work[i]->scr) return 0;
	}
	return 1;
}

/* the ambient row of line y and side s (distances 0 .. Mfull - 1), made once per line and worker */
static const uint16_t *amb_of(const Frame &F, Work &K, int s, int y)
{
	if (K.amb_gen[s] != F.gen || K.amb_y[s] != y) {
		amb_fill(K.amb[s], s, y, F.Mfull, &K.acc.cache_checks, &K.acc.cache_mismatches);
		K.amb_gen[s] = F.gen; K.amb_y[s] = y;
	}
	return K.amb[s];
}

/* a tile's hash and blankness as layer_tile_th gives them, without writing the caches (the lines' workers share them) */
static uint32_t tile_hash_ro(const SWideView *v, int bpp, int tile, int name, int *blank)
{
	const int bytes = bpp * 8;
	const uint32_t a = ((uint32_t) name * 2 + (uint32_t) tile * (uint32_t) bytes) & 0xffff;
	const TileHash *c = bpp == 2 ? &A.th2[a >> 4] : bpp == 4 ? &A.th4[a >> 5] : &A.th8[a >> 6];
	uint16_t age = v->uage[a >> 5];
	if (bytes == 64) { uint16_t b = v->uage[(a >> 5) + 1]; if (b > age) age = b; }
	if (c->ok && c->age == age) { *blank = c->blank; return c->h; }
	uint32_t h = 0x811C9DC5u, any = 0;
	for (int i = 0; i < bytes; i += 4) {
		const uint32_t w = (uint32_t) v->vram[(a + i) & 0xffff] | ((uint32_t) v->vram[(a + i + 1) & 0xffff] << 8) |
		                   ((uint32_t) v->vram[(a + i + 2) & 0xffff] << 16) | ((uint32_t) v->vram[(a + i + 3) & 0xffff] << 24);
		any |= w;
		h = mix32(h, w);
	}
	*blank = any == 0;
	return h;
}

static uint32_t layer_tile_th_ro(const Layer *Ly, const SWideView *v, int tile, int *blank)
{
	if (Ly->tgen[tile] == Ly->gen) { *blank = Ly->tblank[tile]; return Ly->tcache[tile]; }
	if (Ly->tile_px == 8) return tile_hash_ro(v, Ly->bpp, tile, Ly->name, blank);
	const int t1 = (tile + 1) & 0x3ff, t2 = (tile + 16) & 0x3ff, t3 = (tile + 17) & 0x3ff;
	int b0, b1, b2, b3;
	const uint32_t h0 = tile_hash_ro(v, Ly->bpp, tile, Ly->name, &b0), h1 = tile_hash_ro(v, Ly->bpp, t1, Ly->name, &b1);
	const uint32_t h2 = tile_hash_ro(v, Ly->bpp, t2, Ly->name, &b2), h3 = tile_hash_ro(v, Ly->bpp, t3, Ly->name, &b3);
	*blank = b0 && b1 && b2 && b3;
	return mix32(mix32(mix32(h0, h1), h2), h3);
}

/* the learned map's cell `idx` beside line y of layer l on side s (wk_map_margin_cell), with its tile's blankness and whether the tile's graphics are
 * today what they were when the cell was learned. 0: the map has no cell there. */
static int memo_cell(const Frame &F, Work &K, int l, int s, int y, int idx, wk_cell *hold, const wk_cell **cell, int *hok, int *blank)
{
	const wk_out *wo = A.wo;
	const Layer *Ly = &A.lay[l];
	CellMemo &m = K.memo[l][s];
	const int32_t wx = wo->world_x[l][y], wrow = wo->world_row[l][y] >> (Ly->tile_px == 16 ? 4 : 3);
	if (m.gen != F.gen || m.wx != wx || m.wrow != wrow) {
		m.gen = F.gen; m.wx = wx; m.wrow = wrow;
		memset(m.st, 0, sizeof m.st);
	}
	const bool keep = idx >= 0 && idx < MEMO_N;
	wk_cell *c = keep ? &m.cell[idx] : hold;
	int st = keep ? m.st[idx] : 0, h = 0, b = 0;
	if (!st) {
		st = wk_map_margin_cell(A.M, A.fr, wo, l, y, s, idx, c) ? 2 : 1;
		if (st == 2) {
			const uint32_t th = layer_tile_th_ro(Ly, F.v, (int) (c->code & 0x3ff), &b);
			const uint32_t hh = mix32(c->code * 0x10001u + 0x1234567u, th);
			h = (uint16_t) ((hh ^ (hh >> 16)) & 0xFFFF) == c->h16;
		}
		if (keep) { m.st[idx] = (uint8_t) st; m.hok[idx] = (uint8_t) h; m.blank[idx] = (uint8_t) b; }
	} else { h = m.hok[idx]; b = m.blank[idx]; }
	if (st != 2) return 0;
	*cell = c; *hok = h; *blank = b;
	return 1;
}

/* step 3 for one line: per layer and side the candidate strip (the core's layer pass out to the ring, then cells of the learned map drawn here), how far
 * it is real, and the layers' own pixels at the view edge */
static void cand_line(const Frame &F, Work &K, int y)
{
	const SWideView *v = F.v;
	const Snap *sn = F.sn;
	wk_frame *fr = A.fr;
	wk_out *wo = A.wo;
	const int Mh = F.Mh, nlay = F.nlay;
	const SWideLine &Ln = v->line[y];
	wk_line_out *lo = &wo->line[y];
	const bool drawn = line_ok(Ln);
	if (lo->hud) K.acc.lines_hud++;
	/* windows and colour math act on the composite, not on the layers: the layered continuation (map, history) knows
	 * neither, so on such lines only the core's own composite out to the ring is shown as real */
	A.xl[y] = (Ln.wsel_any || (Ln.cgadsub & 0x3F) || (Ln.cgwsel & 0xC0)) ? 1 : 0;
	A.bdm[y] = 0;
	/* colour math on the backdrop alone (a sky gradient: fixed colour, or a sub screen with no layers): layer and sprite pixels
	 * are untouched, so the layered continuation applies; only pixels where no layer is opaque would show the (maths'd) backdrop,
	 * and those are filled with the soft edge, never with a guessed colour */
	if (A.xl[y] && drawn && !Ln.wsel_any && !(Ln.cgadsub & 0x1F) && (Ln.cgadsub & 0x20) && !(Ln.cgwsel & 0xC0) && !(Ln.ts & ~Ln.tsx) &&
	    !(Ln.flags & SWL_MODE7)) {
		A.bdm[y] = 1; A.xl[y] = 0;
	}
	if (A.xl[y])
		for (int l = 0; l < 4; l++)
			for (int s = 0; s < 2; s++) {
				const int r = lo->real[s][l], g = lo->ring[s][l];
				A.xr[y][s][l] = (uint16_t) (g < r ? g : r);
				if (nlay && A.lay[l].active && drawn && A.lay[l].line_on[y] &&
				    A.lay[l].rowblank[fmod_(((int) Ln.vofs[l] + y) / A.lay[l].tile_px, A.lay[l].mh)])
					A.xr[y][s][l] = (uint16_t) Mh;           /* blank ring row: draws nothing here */
				lo->real[s][l] = 0; lo->ring[s][l] = 0;
			}
	for (int s = 0; s < 2; s++)
		if (A.softany[y][s]) { memset(A.softm[y][s], 0, sizeof A.softm[y][s]); A.softany[y][s] = 0; }
	for (int l = 0; l < 4; l++) {
		if (!(nlay && A.lay[l].active)) continue;
		const Layer *Ly = &A.lay[l];
		const int pass = SWP_BG0 + l;
		const int tp = Ly->tile_px, tsh = tp == 16 ? 4 : 3, tmask = tp - 1;
		for (int s = 0; s < 2; s++) {
			const bool px = !F.skip[s];
			uint16_t *dst = A.cand[l][s] + (size_t) y * Mh;
			uint8_t *op = A.opq[l][s] + (size_t) y * Mh;
			int real = lo->real[s][l], ring = lo->ring[s][l];
			if (!drawn || !Ly->line_on[y] || !((Ln.pass >> pass) & 1)) { lo->real[s][l] = 0; lo->ring[s][l] = 0; continue; }
			{
				/* the layer's ring row is blank all around: it draws nothing on this line, margin included */
				const int ty = (((int) Ln.vofs[l] + y) >> tsh) & (Ly->mh - 1);
				if (Ly->rowblank[ty]) {
					/* blank all around the ring: assumed blank beyond it too, unless the learned map knows a tile there */
					bool nonblank = false;
					if (A.M && wo->coherent[l][y]) {
						for (int idx = 1; idx <= Mh / tp + 1 && !nonblank; idx++) {
							const wk_cell *c2;
							wk_cell hold2;
							int h2, b2;
							if (!memo_cell(F, K, l, s, y, idx, &hold2, &c2, &h2, &b2)) continue;
							if (!b2) nonblank = true;
						}
					}
					if (!nonblank) {
						if (px) memset(op, 0, (size_t) Mh);
						lo->real[s][l] = lo->ring[s][l] = (uint16_t) Mh;
						continue;
					}
				}
			}
			if (real > Mh) real = Mh;
			if (ring > real) ring = real;
			if (px && ring > 0) {
				const uint16_t *pp = sn->px[pass][y][s];
				const uint8_t *pz = sn->z[pass][y][s];
				const bool all = ((Ln.tsx >> l) & 1) != 0;   /* a backdrop + sub layer composite draws every pixel */
				if (s == 0) {
					wk_rev16(dst, pp + Mh - ring, ring);
					if (all) memset(op, 1, (size_t) ring);
					else { wk_rev8(op, pz + Mh - ring, ring); wk_nz8(op, op, ring); }
				} else {
					memcpy(dst, pp, (size_t) ring * 2);
					if (all) memset(op, 1, (size_t) ring);
					else wk_nz8(op, pz, ring);
				}
			}
			if (real > ring) {
				const bool tsx_math = ((Ln.tsx >> l) & 1) && Ln.cgadsub == 0x20;   /* a sub screen layer added to the backdrop: the map cell gets the same maths */
				const int have_geom = wo->coherent[l][y] != 0 && A.M && (!((Ln.tsx >> l) & 1) || tsx_math);
				if (!have_geom) lo->real[s][l] = (uint16_t) ring;
				else {
					wk_geom g;
					wk_line_geom(fr, wo, l, y, &g);
					const int wx = wo->world_x[l][y];
					const int fy = wo->world_row[l][y] & tmask;
					const uint16_t *soft_amb = NULL;
					int16_t crow[16];
					for (int d = ring; d < real; ) {
						/* the stretch of this line inside one cell */
						const int xw = s == 0 ? wx - 1 - d : wx + 256 + d;
						const int idx = s == 0 ? (int) (g.tile_first - (xw >> tsh)) : (int) ((xw >> tsh) - (g.tile_first + g.nvis - 1));
						int n = s == 0 ? (xw & tmask) + 1 : tp - (xw & tmask);
						if (n > real - d) n = real - d;
						const wk_cell *cell;
						wk_cell hold;
						int hok, cb;
						/* the map holds a tilemap word, not pixels: shown only if that tile's graphics are today what they were when it was learned */
						if (!memo_cell(F, K, l, s, y, idx, &hold, &cell, &hok, &cb) || !hok) { lo->real[s][l] = (uint16_t) d; break; }
						if (px) {
							cell_row(v, Ly, cell->code, fy, crow);
							const int step = s == 0 ? -1 : 1;
							if (tsx_math)
								for (int k = 0, x = xw; k < n; k++, x += step) {
									const int ci = crow[x & tmask];
									dst[d + k] = math_add565(Ln.backdrop, ci < 0 ? Ln.fix565 : v->colors[ci]); op[d + k] = 1;
								}
							else
								for (int k = 0, x = xw; k < n; k++, x += step) {
									const int ci = crow[x & tmask];
									dst[d + k] = ci < 0 ? 0 : v->colors[ci]; op[d + k] = ci >= 0;
								}
							if (A.soft_on && !cell->live) {
								/* a memory of an earlier run or session, not seen in this one: a hint, shown softly (a transparent pixel's colour is never read) */
								if (!soft_amb) soft_amb = amb_of(F, K, s, y);
								wk_mix565_row(dst + d, soft_amb + d, SOFT_W, n);
								uint8_t any = 0;
								for (int k = 0; k < n; k++) { any |= op[d + k]; if (op[d + k]) A.softm[y][s][d + k] |= (uint8_t) (1 << l); }
								if (any) A.softany[y][s] = 1;
							}
						}
						d += n;
					}
				}
			}
		}
	}
	wk_out_refresh_line(wo, y);

	/* the layers' own pixels at the view edge, for the foreground fade (what a layer that is not known beside the view continues with) */
	if (A.fgfade && nlay)
		for (int l = 0; l < 4; l++) {
			const Layer *Ly = &A.lay[l];
			if (!Ly->active) continue;
			const int tp = Ly->tile_px;
			const bool ok = line_ok(Ln) && Ly->line_on[y] && !A.xl[y] && !wo->line[y].hud && !(Ln.flags & SWL_MODE7);
			for (int s = 0; s < 2; s++) {
				A.tailop[l][s][y] = 0;
				if (!ok || F.skip[s]) continue;
				const int X = (int) Ln.hofs[l] + (s ? 255 : 0), Y = (int) Ln.vofs[l] + y;
				const uint32_t word = Ly->code[fmod_(fdiv(Y, tp), Ly->mh) * Ly->mw + fmod_(fdiv(X, tp), Ly->mw)];
				if (word == WK_CODE_NOREC) continue;
				const int pxi = cell_index(v, Ly, word, fmod_(X, tp), fmod_(Y, tp));
				if (pxi < 0) continue;
				A.tailpx[l][s][y] = v->colors[pxi];
				A.tailop[l][s][y] = 1;
			}
		}
}

/* The composite of a line whose layers have no window and no colour math (SWLP_NOCOMP: the core did not draw it), distances 0 .. n - 1 of side s: the
 * sprite pass and the main screen's BG passes by depth (every pass keeps the depth the composite would have given the pixel; depths of different passes
 * never tie) over the backdrop: its colour, or with colour math on the backdrop the picture the core drew of it (SWLP_BASE). */
static void derive_composite(const Frame &F, const SWideLine &Ln, int y, int s, int n, uint16_t *out)
{
	const Snap *sn = F.sn;
	const int Mh = F.Mh;
	uint8_t zb[SNES_WIDE_MAX_MARGIN];
	memset(zb, 0, (size_t) n);
	if (Ln.pflags & SWLP_BASE) {
		const uint16_t *cp = sn->px[SWP_COMPOSITE][y][s];
		if (s == 0) wk_rev16(out, cp + Mh - n, n);
		else memcpy(out, cp, (size_t) n * 2);
	} else for (int d = 0; d < n; d++) out[d] = Ln.backdrop;
	for (int p = SWP_OBJ; p < SW_NPASS; p++) {
		if (!((Ln.pass >> p) & 1) || (p >= SWP_BG0 && !((Ln.tm >> (p - SWP_BG0)) & 1))) continue;
		const uint16_t *pp = sn->px[p][y][s];
		const uint8_t *pz = sn->z[p][y][s];
		if (s == 0) { pp += Mh - 1; pz += Mh - 1; for (int d = 0; d < n; d++) if (pz[-d] > zb[d]) { zb[d] = pz[-d]; out[d] = pp[-d]; } }
		else for (int d = 0; d < n; d++) if (pz[d] > zb[d]) { zb[d] = pz[d]; out[d] = pp[d]; }
	}
}

/* steps 4 and 5 for one line: widekit's composition, then the exact composite where every layer of the line is real, sprites over the rest (Mode 7:
 * the composite whole), the provenance of soft learned pixels and the line's share of the census. Both margins are held by distance from the
 * picture here (index d; fin_line turns the left one round). */
static void out_line(const Frame &F, Work &K, int y)
{
	const SWideView *v = F.v;
	const Snap *sn = F.sn;
	const wk_out *wo = A.wo;
	Out *o = F.o;
	const int Mh = F.Mh, nlay = F.nlay, fade = F.fade;
	const SWideLine &Ln = v->line[y];
	const wk_line_out *lo = &wo->line[y];
	const bool shown = (Ln.flags & SWL_VALID) && !(Ln.flags & SWL_BLANK);     /* fin_line lays the ambient frame over this line */
	if ((F.skip[0] || F.skip[1]) && !shown) {
		wk_compose_in ci = F.ci;
		ci.side_skip[0] = ci.side_skip[1] = 0;
		wk_compose_lines(A.C, &ci, y, y + 1, K.scr);
	} else wk_compose_lines(A.C, &F.ci, y, y + 1, K.scr);

	uint16_t *const rs[2] = { o->px + (size_t) y * F.OW, o->px + (size_t) y * F.OW + Mh + 256 };
	uint8_t *const ps[2] = { o->prov + (size_t) y * F.OW, o->prov + (size_t) y * F.OW + Mh + 256 };
	o->real[y][0] = o->real[y][1] = 0;
	if (!(Ln.flags & SWL_VALID)) { K.acc.reason[SNES_WIDE_LR_NOTVALID]++; return; }
	if (Ln.flags & SWL_BLANK) {
		K.acc.reason[SNES_WIDE_LR_BLANK]++;
		for (int s = 0; s < 2; s++) { memset(rs[s], 0, (size_t) Mh * 2); memset(ps[s], PROV_EXACT, (size_t) Mh); }
		return;
	}
	if (!line_ok(Ln)) {
		K.acc.reason[(Ln.flags & SWL_HIRES) ? SNES_WIDE_LR_HIRES : (Ln.flags & SWL_OPT) ? SNES_WIDE_LR_OPT : (Ln.flags & SWL_MOSAIC) ? SNES_WIDE_LR_MOSAIC : SNES_WIDE_LR_INTERLACE]++;
		return;
	}
	int why = 0;
	for (int s = 0; s < 2; s++) {
		int nex = Mh, nlay_ = Mh, rmax_ = 0;   /* nex: every enabled layer is real (exact composite); nlay_: every layer that is not a HUD; rmax_: the widest real extent of any layer */
		if (!(Ln.flags & SWL_MODE7)) {
			if (lo->hud) { nex = nlay_ = 0; if (!why) why = SNES_WIDE_LR_HUD; }
			for (int l = 0; l < bg_count(Ln.mode); l++) {
				if (!((Ln.tm | Ln.tsx) & (1 << l))) continue;
				if (!(nlay && A.lay[l].active && A.lay[l].line_on[y])) { nex = nlay_ = 0; rmax_ = 0; if (!why) why = SNES_WIDE_LR_NOLAYER; break; }
				const int r = A.xl[y] ? A.xr[y][s][l] : lo->real[s][l];
				const int rr = A.xl[y] ? r : (lo->ring[s][l] < r ? lo->ring[s][l] : r);   /* the core's composite pass only knows the ring */
				const int kind = wo->kind[l];
				if (rr < nex) nex = rr;
				if (kind != WK_KIND_HUD && kind != WK_KIND_OFF && r < nlay_) nlay_ = r;
				if (kind != WK_KIND_HUD && kind != WK_KIND_OFF && r > rmax_) rmax_ = r;
				if (kind == WK_KIND_HUD || kind == WK_KIND_OFF) nex = 0;
			}
		}
		if (!why && !(Ln.flags & SWL_MODE7) && nex < Mh) {
			if (A.xl[y] && nlay_ >= Mh) why = Ln.wsel_any ? SNES_WIDE_LR_WINDOW : SNES_WIDE_LR_MATH;
			else {
				why = SNES_WIDE_LR_RING;
				for (int l = 0; l < 4; l++) if (nlay && A.lay[l].active && A.lay[l].line_on[y] && A.lay[l].bitmap) why = SNES_WIDE_LR_BITMAP;
			}
		}
		const int cex = nex >= Mh ? Mh : (nex > fade ? nex - fade : 0);
		const int cla = nlay_ >= Mh ? Mh : (nlay_ > fade ? nlay_ - fade : 0);
		if (!F.skip[s]) {
			const uint16_t *cp = sn->px[SWP_COMPOSITE][y][s];
			if (cex > 0) {
				if (Ln.pflags & SWLP_NOCOMP) derive_composite(F, Ln, y, s, cex, rs[s]);
				else if (s == 0) wk_rev16(rs[0], cp + Mh - cex, cex);
				else memcpy(rs[1], cp, (size_t) cex * 2);
				memset(ps[s], PROV_EXACT, (size_t) cex);
			}
			if (!(Ln.flags & SWL_MODE7) && !F.sprites_off && ((Ln.pass >> SWP_OBJ) & 1)) {
				/* sprites end where the layers' real picture ends (same fade): never a sprite floating on the ambient colour */
				const int sbm = rmax_ > nlay_ ? rmax_ : nlay_;
				const int sb = sbm >= Mh ? Mh : sbm;
				const int n = sb - cex;
				if (n > 0) {
					/* the sprite pass of distances cex .. sb - 1 */
					const uint16_t *sp;
					const uint8_t *sz;
					if (s == 0) {
						wk_rev16(K.t16, sn->px[SWP_OBJ][y][0] + Mh - sb, n);
						wk_rev8(K.t8, sn->z[SWP_OBJ][y][0] + Mh - sb, n);
						sp = K.t16; sz = K.t8;
					} else { sp = sn->px[SWP_OBJ][y][1] + cex; sz = sn->z[SWP_OBJ][y][1] + cex; }
					int nfull = n;                    /* full weight up to here, then the fade (only when the sprites end inside the margin) */
					if (sb < Mh) { nfull = sb - fade - cex; if (nfull < 0) nfull = 0; }
					wk_copy_masked(rs[s] + cex, sp, sz, nfull);
					wk_fill_masked8(ps[s] + cex, sz, PROV_EXACT, nfull);
					for (int i = nfull; i < n; i++) {
						if (!sz[i]) continue;
						const int d = cex + i;
						const int w = (sb - d) * 32 / fade;
						rs[s][d] = w >= 32 ? sp[i] : mix565(rs[s][d], sp[i], w);
						ps[s][d] = w >= 32 ? PROV_EXACT : WK_PROV_FADE;      /* a faded sprite pixel is not a claim of real picture */
					}
				}
			}
		}
		o->real[y][s] = (uint8_t) (cla > cex ? cla : cex);
		if (s == 0) K.acc.sl += o->real[y][0]; else K.acc.sr += o->real[y][1];
	}
	K.acc.nreal++;
	if (o->real[y][0] == Mh && o->real[y][1] == Mh) { K.acc.lines_exact++; why = SNES_WIDE_LR_EXACT; }
	else if (!why) why = SNES_WIDE_LR_RING;
	K.acc.reason[why]++;

	/* provenance of soft learned pixels: a hint, never a claim of real picture */
	if (A.soft_on)
		for (int s = 0; s < 2; s++) {
			if (!A.softany[y][s] || F.skip[s]) continue;
			const uint8_t *sm = A.softm[y][s];
			for (int d = 0; d < Mh; d++) {
				if (!sm[d]) continue;
				uint8_t &p = ps[s][d];
				if ((p & 15) == WK_PROV_LEVEL && ((sm[d] >> (p >> 4)) & 1)) p = (uint8_t) ((p & 0xF0) | PROV_SOFTL);
			}
		}

	/* census and coverage: this line's margin pixels by provenance */
	if (F.covf && !(A.hud_excl && lo->hud)) {      /* a status bar is original width by design: not a gap in the margin */
		for (int s = 0; s < 2; s++) {
			int ring_e = 0, real_e = 0;
			for (int l = 0; l < 4; l++) {
				if (lo->ring[s][l] > ring_e) ring_e = lo->ring[s][l];
				if (lo->real[s][l] > real_e) real_e = lo->real[s][l];
			}
			int cnt[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
			const uint8_t *pp = ps[s];
			for (int d = 0; d < Mh; d++) cnt[pp[d] & 15]++;
			const int tm = cnt[WK_PROV_LEVEL] + cnt[PROV_EXACT] + cnt[WK_PROV_FADE], bd = cnt[WK_PROV_BACKDROP] + cnt[WK_PROV_BG];
			const int sf = cnt[PROV_SOFTL], rv = cnt[WK_PROV_REVEAL], un = Mh - tm - bd - sf - rv;
			int ln_ = real_e > ring_e ? real_e - ring_e : 0;
			if (ln_ > tm) ln_ = tm;
			K.acc.ctm += (uint64_t) (tm - ln_); K.acc.cln += (uint64_t) ln_; K.acc.cbd += (uint64_t) bd; K.acc.cun += (uint64_t) un;
			K.acc.csf += (uint64_t) sf; K.acc.crv += (uint64_t) rv;
			K.acc.cnum[s] += (uint64_t) ((tm + bd + rv) * 2 + sf); K.acc.ctot[s] += (uint64_t) Mh;
			K.acc.cpad += (uint64_t) (F.Mfull - Mh);
		}
	}
}

/* the last step for one line: the ambient frame over a side that Auto does not show (or the cross-fade to it), and the left margin into screen order */
static void fin_line(const Frame &F, Work &K, int y)
{
	Out *o = F.o;
	const int Mh = F.Mh;
	const SWideLine &Ln = F.v->line[y];
	uint16_t *const rs[2] = { o->px + (size_t) y * F.OW, o->px + (size_t) y * F.OW + Mh + 256 };
	uint8_t *const ps[2] = { o->prov + (size_t) y * F.OW, o->prov + (size_t) y * F.OW + Mh + 256 };
	bool left_done = false;
	if ((Ln.flags & SWL_VALID) && !(Ln.flags & SWL_BLANK))
		for (int s = 0; s < 2; s++) {
			const int xf = F.xf[s];
			if (xf <= 0) continue;
			const uint16_t *ap = amb_of(F, K, s, y);
			o->real[y][s] = 0;        /* a blend with the ambient frame is not a claim of real picture, whatever the weight */
			if (xf >= 32) {
				if (s == 0) { wk_rev16(rs[0], ap, Mh); left_done = true; }
				else memcpy(rs[1], ap, (size_t) Mh * 2);
				memset(ps[s], WK_PROV_SOFT, (size_t) Mh);
			} else {
				wk_mix565_row(rs[s], ap, xf, Mh);
				for (int d = 0; d < Mh; d++) ps[s][d] = (uint8_t) ((ps[s][d] & 0xF0) | WK_PROV_FADE);
			}
		}
	if (!left_done) { wk_rev16_inplace(rs[0], Mh); wk_rev8_inplace(ps[0], Mh); }
}

typedef void (*LineFn)(const Frame &, Work &, int);
struct LineJob { const Frame *F; LineFn fn; std::atomic<int> next; };
enum { LINE_CHUNK = 8 };

static void line_worker(void *arg, int worker)
{
	LineJob *j = (LineJob *) arg;
	Work &K = *g_work[worker];
	const int H = j->F->H;
	for (;;) {
		const int y0 = j->next.fetch_add(LINE_CHUNK, std::memory_order_relaxed);
		if (y0 >= H) break;
		const int y1 = y0 + LINE_CHUNK < H ? y0 + LINE_CHUNK : H;
		for (int y = y0; y < y1; y++) j->fn(*j->F, K, y);
	}
}

/* every line of the frame through fn: on the caller alone, or shared with the frontend's workers (snes_wide_parallel). A line is done by one worker;
 * what the lines write is per line or per worker, so the result does not depend on who did which line. */
static void for_lines(const Frame &F, LineFn fn)
{
	LineJob j;
	j.F = &F; j.fn = fn; j.next.store(0);
	const int nw = A.pfor ? A.nwork : 1;
	if (nw > 1) A.pfor(A.pfor_user, line_worker, &j, nw);
	else line_worker(&j, 0);
}

/* a centre only frame: the picture in the middle, the ambient frame on both sides (what compose can do without the margin passes) */
static void run_co(const Snap *sn, Out *o, const SWideView *v, int H)
{
	snes_wide_stats &st = o->st;
	const uint64_t t0 = now_us();
	const int Mfull = margin_for(H);
	if (!sn->wide_hint || Mfull <= 0) { fallback(o, sn->wide_hint ? 2 : 1); return; }
	if (!A.L || !make_compose(H, Mfull < WORK_FLOOR ? Mfull : WORK_FLOOR, Mfull) || !A.fr || !A.wo) { fallback(o, 2); return; }
	const bool reset = v->epoch != A.epoch;
	A.epoch = ~v->epoch;      /* the layer analysis lost frames: the next frame with margin passes starts a new scene */
	amb_update(v, H, reset);
	const int OWf = 256 + 2 * Mfull;
	uint32_t vc = 0, vm = 0;
	uint16_t a0[SNES_WIDE_MAX_MARGIN + 8], a1[SNES_WIDE_MAX_MARGIN + 8];
	for (int y = 0; y < H; y++) {
		uint16_t *row = o->px + (size_t) y * OWf;
		uint8_t *prow = o->prov + (size_t) y * OWf;
		memcpy(row + Mfull, v->centre + (size_t) y * v->centre_pitch, 512);
		memset(prow, WK_PROV_SOFT, (size_t) OWf);
		memset(prow + Mfull, WK_PROV_CENTRE, 256);
		amb_fill(a0, 0, y, Mfull, &vc, &vm); amb_fill(a1, 1, y, Mfull, &vc, &vm);
		wk_rev16(row, a0, Mfull);
		memcpy(row + Mfull + 256, a1, (size_t) Mfull * 2);
		o->real[y][0] = o->real[y][1] = 0;
	}
	A.cen.cache_checks += vc; A.cen.cache_mismatches += vm;
	A.cen.frames_wide++; A.cen.frames_ambient++;
	A.cen.frames_side_amb[0]++; A.cen.frames_side_amb[1]++;
	if ((A.frame_n & 1) == 0) A.cen.px_pad += (uint64_t) 2 * Mfull * H;
	A.cov_have = 0; A.auto_state[0] = A.auto_state[1] = 0; A.xf[0] = A.xf[1] = 32;
	st.serial = A.serial.load() + 1; st.frame = v->clock;
	st.width = OWf; st.height = H; st.margin = Mfull; st.lines = H;
	st.coverage_pct = 0; st.auto_wide = 0; st.xfade = 32;
	for (int s = 0; s < 2; s++) { st.side_wide[s] = 0; st.side_xfade[s] = 32; st.side_cov[s] = 0; }
	st.work_margin = 0; st.rung = A.rung;
	st.instr_us = (uint32_t) (now_us() - t0);
	ladder_step(0, st.instr_us, true);
	publish(o, 1);
}

static void run(const Snap *sn)
{
	SWideView vv;
	memset(&vv, 0, sizeof vv);
	vv.m = sn->m; vv.h = sn->h; vv.ok = sn->ok; vv.clock = sn->clock; vv.epoch = sn->epoch;
	vv.line = sn->line; vv.vram = sn->vram; vv.vage = sn->vage; vv.uage = sn->uage; vv.cgram = sn->cgram; vv.colors = sn->colors;
	vv.centre = sn->centre; vv.centre_pitch = 256;
	const SWideView *v = &vv;
	const uint64_t t0 = now_us();
	Out *o = &A.out[A.wr.load() % NOUT];
	snes_wide_stats &st = o->st;
	memset(&st, 0, sizeof st);
	st.serial = A.serial.load() + 1;
	st.frame = v->clock;
	const int H = v->h;
	o->avail = 0;
	A.cen.frames++;
	A.frame_n++;
	if (H <= 0 || H > MAXH || !v->ok || A.margin224 <= 0) { fallback(o, 2); return; }
	if (sn->co) { run_co(sn, o, v, H); return; }
	const int Mfull = margin_for(H), Mc = v->m;
	const int Mh = sn->mh;           /* the work margin of this snapshot (the capture's: see work_margin), at most the picture's margin */
	if (Mfull <= 0 || Mh <= 0 || Mh > Mfull || Mc < Mh) { fallback(o, 2); return; }
	if (A.prof && A.prof->enable == WK_ENABLE_OFF) { fallback(o, 1); return; }
	if (!A.L || !make_compose(H, Mh, Mfull) || !A.fr || !A.wo) { fallback(o, 2); return; }
	if (v->epoch != A.epoch) {
		A.epoch = v->epoch;
		A.amb_have = 0; A.cov_have = 0;
		wk_layers_reset(A.L);
		wk_compose_reset(A.C);
		A.have_pal_prev = 0;
		for (int l = 0; l < 4; l++) { A.lay[l].have_gid = 0; A.lay[l].walk_ok = 0; }
		memset(A.th2, 0, sizeof A.th2); memset(A.th4, 0, sizeof A.th4); memset(A.th8, 0, sizeof A.th8);
	}
	const int OW = 256 + 2 * Mh;      /* the work picture's row; the output row is 256 + 2 * Mfull (padded at the end) */
	st.width = OW; st.height = H; st.margin = Mh; st.lines = H;

	int nd = 0, nn = 0, n7 = 0, nblank = 0;
	for (int y = 0; y < H; y++) {
		const SWideLine &L = v->line[y];
		if (!(L.flags & SWL_VALID)) { nn++; continue; }
		if (L.flags & SWL_BLANK) nblank++;
		else if (L.flags & SWL_NOMARGIN) nn++;
		else if (L.flags & SWL_DRAWN) { nd++; if (L.flags & SWL_MODE7) n7++; }
	}
	st.lines_drawn = nd; st.lines_nomargin = nn;
	if (nd == 0 && nblank == 0) { fallback(o, 0); return; }
	const bool mode7 = n7 * 2 > nd;
	st.mode7 = mode7;

	/* 1. the description */
	wk_frame *fr = A.fr;
	memset(fr, 0, sizeof *fr);
	fr->frame = v->clock;
	fr->view_w = 256; fr->view_h = H; fr->margin = Mh;
	{
		int bmax = 0;
		for (int y = 0; y < H; y++) if (v->line[y].bright > bmax) bmax = v->line[y].bright;
		fr->brightness = nd ? bmax * 17 : 0;
		int delta = 0;
		if (A.have_pal_prev) for (int i = 0; i < 256; i++) if (A.pal_prev[i] != v->cgram[i]) delta++;
		memcpy(A.pal_prev, v->cgram, sizeof A.pal_prev);
		A.have_pal_prev = 1;
		fr->palette_delta = delta; fr->palette_size = 256;
	}
	fr->nlayers = 4;
	int nlay = 0;
	for (int l = 0; l < 4; l++) {
		A.lay[l].active = 0;
		if (!mode7 && nd) describe_layer(v, l, H, &A.lay[l]);
		if (A.lay[l].active) {
			nlay++;
			st.layers |= 1 << l;
			st.ring_w[l] = A.lay[l].mw; st.ring_h[l] = A.lay[l].mh;
		} else {
			fr->layer[l].active = 0;
		}
	}
	if (!nlay) fr->nlayers = 0;

	/* 2. widekit */
	wk_out *wo = A.wo;
	wk_layers_update(A.L, fr, wo);
	if (A.M && nlay) {
		wk_map_update(A.M, fr, wo);
		wk_map_serve(A.M, fr, wo);
	}
	st.scene_event = wo->scene_event; st.epoch = (int) wo->epoch;
	st.hs_checked = wo->hs_checked; st.hs_missed = wo->hs_missed;
	for (int l = 0; l < 4; l++) st.kind[l] = nlay && A.lay[l].active ? wo->kind[l] : 0;
	if (A.M) {
		wk_map_stats ms;
		wk_map_stats_get(A.M, &ms);
		st.map_cells = ms.cells; st.map_rooms = ms.rooms; st.map_bound = ms.bound; st.map_served = (int) ms.served_tiles;
	}

	/* 3. candidates (per line) */
	amb_update(v, H, wo->scene_event != 0);
	Frame F;
	F.sn = sn; F.v = v; F.o = o; F.H = H; F.Mh = Mh; F.Mfull = Mfull; F.OW = OW; F.nlay = nlay;
	F.fade = A.fade_px > 0 ? A.fade_px : 20;
	F.gen = ++g_fgen;
	if (!F.gen) F.gen = ++g_fgen;
	F.sprites_off = A.prof && A.prof->sprites_off;
	const bool auto_on = A.auto_on.load() != 0;
	/* the coverage is counted on every second frame. On the frames between, a side that shows the ambient frame and is not fading (Auto can only
	 * decide otherwise on a counted frame) shows none of its composed pixels and nothing later reads them (option "lazy", default on): the extents
	 * and widekit's hysteresis still advance, the pixels are not made. */
	F.covf = (A.frame_n & 1) == 0 || !A.cov_have;
	for (int s = 0; s < 2; s++) F.skip[s] = A.lazy && auto_on && !F.covf && !A.auto_state[s] && A.xf[s] >= 32;
	F.xf[0] = F.xf[1] = 0;
	for (int i = 0; i < SNES_WIDE_MAX_WORKERS; i++) memset(&g_work[i]->acc, 0, sizeof(Acc));
	(void) Mc;
	for_lines(F, cand_line);

	/* 4. compose: widekit's per frame part here, its lines with step 5 in out_line */
	wk_compose_in &ci = F.ci;
	memset(&ci, 0, sizeof ci);
	ci.edge_px[0] = A.edge_px[0]; ci.edge_px[1] = A.edge_px[1];
	for (int y = 0; y < H; y++) {
		const SWideLine &Ln = v->line[y];
		A.bg[y] = (Ln.flags & SWL_VALID) ? Ln.backdrop : 0;
	}
	ci.ana = wo;
	ci.centre = v->centre; ci.centre_stride = v->centre_pitch;
	ci.bg = A.bg;
	ci.norder = 0;
	uint8_t subl = 0;                                  /* layers that are sub screen only (backdrop + layer composites): below every main layer */
	for (int y = 0; y < H; y++) if (line_ok(v->line[y])) subl |= v->line[y].tsx;
	if (nlay)
		for (int pass = 0; pass < 2; pass++)
		for (int l = 3; l >= 0; l--)
			if (A.lay[l].active && (((subl >> l) & 1) == (pass == 0))) {
				ci.cand[l][0] = A.cand[l][0]; ci.cand[l][1] = A.cand[l][1];
				if (A.fgfade) for (int sd = 0; sd < 2; sd++) { ci.tail_px[l][sd] = A.tailpx[l][sd]; ci.tail_op[l][sd] = A.tailop[l][sd]; }
				ci.opaque[l][0] = A.opq[l][0]; ci.opaque[l][1] = A.opq[l][1];
				ci.order[ci.norder++] = l;
			}
	ci.out = o->px; ci.out_stride = OW;
	ci.prov = o->prov;
	ci.left_by_distance = 1;
	ci.side_skip[0] = F.skip[0]; ci.side_skip[1] = F.skip[1];
	wk_compose_begin(A.C, &ci);
	for_lines(F, out_line);
	wk_compose_end(A.C);

	Acc T;
	memset(&T, 0, sizeof T);
	for (int i = 0; i < SNES_WIDE_MAX_WORKERS; i++) {
		const Acc &a = g_work[i]->acc;
		T.sl += a.sl; T.sr += a.sr; T.nreal += a.nreal; T.lines_exact += a.lines_exact; T.lines_hud += a.lines_hud;
		for (int k = 0; k < SNES_WIDE_LR_N; k++) T.reason[k] += a.reason[k];
		T.ctm += a.ctm; T.cln += a.cln; T.cbd += a.cbd; T.cun += a.cun; T.csf += a.csf; T.crv += a.crv; T.cpad += a.cpad;
		for (int s = 0; s < 2; s++) { T.cnum[s] += a.cnum[s]; T.ctot[s] += a.ctot[s]; }
	}
	st.lines_hud = T.lines_hud; st.lines_exact = T.lines_exact;
	for (int k = 0; k < SNES_WIDE_LR_N; k++) A.cen.line_reason[k] += T.reason[k];
	st.real_l = T.nreal ? (int) (T.sl / T.nreal) : 0;
	st.real_r = T.nreal ? (int) (T.sr / T.nreal) : 0;

	/* 6. census, coverage and the Auto decision (per game and scene) */
	if (wo->scene_event) A.cen.scenes++;
	if (F.covf) {
		const uint64_t tot = T.ctm + T.cln + T.cbd + T.cun + T.csf + T.crv;
		A.cen.px_tilemap += T.ctm; A.cen.px_learned += T.cln; A.cen.px_backdrop += T.cbd; A.cen.px_unknown += T.cun;
		A.cen.px_soft += T.csf; A.cen.px_reveal += T.crv; A.cen.px_pad += T.cpad;
		if (tot) {
			/* a revealed lower layer is real picture (the layer above it fades out over it: B); soft learned pixels are memory of an
			 * earlier run, not a proven picture: they count half. Each side has its own coverage: a scrolling game has real history
			 * behind the camera and little ahead of it, and the side the camera came from can carry real picture while the other
			 * one shows the ambient frame. */
			for (int s = 0; s < 2; s++) {
				if (!T.ctot[s]) continue;
				const int c256 = (int) (T.cnum[s] * 50 * 256 / T.ctot[s]);
				if (!A.cov_have || wo->scene_event) A.cov_q8[s] = c256;
				else A.cov_q8[s] += (c256 - A.cov_q8[s]) / 60;
			}
			A.cov_have = 1;
		}
	}
	for (int s = 0; s < 2; s++) {
		if (auto_on) {
			const int want = A.auto_state[s] ? (A.cov_q8[s] >= COV_OFF * 256) : (A.cov_q8[s] >= COV_ON * 256);
			if (want != A.auto_state[s]) { A.auto_state[s] = want; A.cen.auto_switches++; }
		} else {
			A.auto_state[s] = 1;
		}
		const int target = A.auto_state[s] ? 0 : 32;
		if (!auto_on) A.xf[s] = 0;
		else if (A.xf[s] < target) A.xf[s]++;
		else if (A.xf[s] > target) A.xf[s]--;
		F.xf[s] = A.xf[s];
	}
	for_lines(F, fin_line);
	for (int i = 0; i < SNES_WIDE_MAX_WORKERS; i++) { A.cen.cache_checks += g_work[i]->acc.cache_checks; A.cen.cache_mismatches += g_work[i]->acc.cache_mismatches; }
	if (A.xf[0] > 0 || A.xf[1] > 0) {
		if (A.xf[0] >= 32 && A.xf[1] >= 32) A.cen.frames_ambient++; else A.cen.frames_crossfade++;
	}
	for (int s = 0; s < 2; s++) if (A.xf[s] >= 32) A.cen.frames_side_amb[s]++;
	/* a work margin narrower than the picture's: the columns beyond it are the ambient frame, and the work margin's last columns fade into
	 * it (faded pixels are not claimed as real) */
	if (Mh < Mfull) {
		const int OWf = 256 + 2 * Mfull, padn = Mfull - Mh, Fd = Mh < 3 * PAD_FADE ? Mh / 3 : PAD_FADE;
		uint16_t apb[SNES_WIDE_MAX_MARGIN + 8];
		uint32_t vc = 0, vm = 0;
		for (int y = H - 1; y >= 0; y--) {
			uint16_t *src = o->px + (size_t) y * OW, *dst = o->px + (size_t) y * OWf;
			uint8_t *psrc = o->prov + (size_t) y * OW, *pdst = o->prov + (size_t) y * OWf;
			memmove(dst + padn, src, (size_t) OW * 2);
			memmove(pdst + padn, psrc, (size_t) OW);
			const SWideLine &Ln = v->line[y];
			const bool blank = (Ln.flags & SWL_VALID) && (Ln.flags & SWL_BLANK);
			for (int sd = 0; sd < 2; sd++) {
				amb_fill(apb, sd, y, Mfull, &vc, &vm);
				const uint16_t *ap = apb;
				for (int d = Mh; d < Mfull; d++) {
					const int dx = sd == 0 ? Mfull - 1 - d : Mfull + 256 + d;
					dst[dx] = blank ? 0 : ap[d];
					pdst[dx] = blank ? PROV_EXACT : WK_PROV_SOFT;
				}
				if (!blank && (Ln.flags & SWL_VALID))
					for (int d = Mh - Fd; d < Mh; d++) {
						const int dx = sd == 0 ? Mfull - 1 - d : Mfull + 256 + d;
						const int w = (d - (Mh - Fd) + 1) * 32 / (Fd + 1);
						dst[dx] = mix565(dst[dx], ap[d], w);
						pdst[dx] = (uint8_t) ((pdst[dx] & 0xF0) | WK_PROV_FADE);
					}
				if (o->real[y][sd] > Mh - Fd) o->real[y][sd] = (uint8_t) (Mh - Fd);
			}
		}
		A.cen.cache_checks += vc; A.cen.cache_mismatches += vm;
	}
	st.width = 256 + 2 * Mfull; st.margin = Mfull;
	st.coverage_pct = (A.cov_q8[0] > A.cov_q8[1] ? A.cov_q8[0] : A.cov_q8[1]) >> 8;
	st.auto_wide = A.auto_state[0] | A.auto_state[1]; st.xfade = A.xf[0] < A.xf[1] ? A.xf[0] : A.xf[1];
	for (int s = 0; s < 2; s++) { st.side_wide[s] = A.auto_state[s]; st.side_xfade[s] = A.xf[s]; st.side_cov[s] = A.cov_q8[s] >> 8; }
	st.work_margin = Mh; st.rung = A.rung;
	A.cen.frames_wide++;
	A.cen.coverage_pct = st.coverage_pct; A.cen.auto_wide = st.auto_wide;
	A.cen.map_cells = (uint32_t) st.map_cells;
	st.instr_us = (uint32_t) (now_us() - t0);
	ladder_step(Mh, st.instr_us, false);
	publish(o, 1);
}


static void cap_time(uint64_t us)
{
	const uint32_t q4 = (uint32_t) (us > 100000 ? 100000 : us) * 16, e = A.cap_us_q4.load(std::memory_order_relaxed);
	A.cap_us_q4.store(e ? e + (int32_t) (q4 - e) / 32 : q4 + 1, std::memory_order_relaxed);
}

static void on_frame(const SWideView *v, void *)
{
	if (!A.on) return;
	if (A.split.load(std::memory_order_acquire)) {
		Snap *sn = pool_claim();
		if (!sn) return;
		const uint64_t c0 = now_us();
		capture(v, sn);
		cap_time(now_us() - c0);
		sn->serial = A.snap_serial.fetch_add(1) + 1;
		sn->state.store(SS_FILLED, std::memory_order_release);
		return;
	}
	if (!A.own) A.own = snap_alloc();
	if (!A.own) return;
	const uint64_t c0 = now_us();
	capture(v, A.own);
	cap_time(now_us() - c0);
	A.lk.lock();
	if (A.on) run(A.own);
	A.lk.unlock();
}

} // namespace

/* ---- public control ------------------------------------------------------------------------------------------ */

extern "C" {

static void pool_reset(void)
{
	/* callers hold A.lk (no compose runs) and the hook is quiet (S9xWideSet synced the PPU thread, or it is about to be) */
	for (int i = 0; i < SNAP_N; i++) {
		if (!A.snap[i]) A.snap[i] = snap_alloc();
		if (A.snap[i]) A.snap[i]->state.store(SS_FREE);
	}
}

static void ensure_objects(void)
{
	if (!A.fr) A.fr = (wk_frame *) calloc(1, sizeof(wk_frame));
	if (!A.wo) A.wo = (wk_out *) calloc(1, sizeof(wk_out));
	if (!A.L) {
		wk_layers_cfg lc;
		wk_layers_cfg_default(&lc);
		A.L = wk_layers_create(&lc);
		profile_lookup();
	}
}


/* ---- split API: capture happens in the frame hook, compose is called by the consumer ---------------------------------- */

void snes_wide_split(int on)
{
	S9xWideSet(0, NULL, NULL);          /* waits for the PPU thread: the hook is quiet */
	A.lk.lock();
	A.split = on ? 1 : 0;
	pool_reset();
	const int cm = A.core_margin;
	const int is_on = A.on;
	A.lk.unlock();
	if (is_on) S9xWideSet(cm, on_frame, NULL);
}

int snes_wide_split_active(void) { return A.split.load(); }

void snes_wide_parallel(snes_wide_pfor fn, void *user, int workers)
{
	A.lk.lock();         /* no compose is running */
	if (workers > SNES_WIDE_MAX_WORKERS) workers = SNES_WIDE_MAX_WORKERS;
	A.pfor = fn && workers > 1 ? fn : NULL;
	A.pfor_user = user;
	A.nwork = A.pfor ? workers : 1;
	A.lk.unlock();
}

size_t snes_wide_snapshot_size(void) { return sizeof(Snap); }

/* the oldest captured frame nobody took yet: 1 and its handle; *widenable is the capture's verdict (0: compose will answer 0, use the
 * normal picture of that frame) */
int snes_wide_snap_take(snes_wide_snap **out, unsigned *serial, int *widenable)
{
	if (!A.split.load(std::memory_order_acquire)) return 0;
	for (;;) {
		Snap *best = NULL;
		for (int i = 0; i < SNAP_N; i++) {
			Snap *s = A.snap[i];
			if (s && s->state.load(std::memory_order_acquire) == SS_FILLED && (!best || (int32_t) (s->serial - best->serial) < 0)) best = s;
		}
		if (!best) return 0;
		uint32_t exp = SS_FILLED;
		if (!best->state.compare_exchange_strong(exp, SS_TAKEN, std::memory_order_acq_rel)) continue;
		*out = (snes_wide_snap *) best;
		*serial = best->serial;
		if (widenable) *widenable = best->wide_hint;
		return 1;
	}
}

void snes_wide_snap_release(snes_wide_snap *h, unsigned serial)
{
	Snap *s = (Snap *) h;
	uint32_t exp = SS_TAKEN;
	if (s && s->serial == serial) s->state.compare_exchange_strong(exp, SS_FREE, std::memory_order_acq_rel);
}

/* compose a taken frame: 1 = the picture (valid until the third next compose), 0 = this frame is not widened (the adapter counted it), -1 = the
 * handle is stale (overwritten, the view was switched off) and nothing was done */
int snes_wide_compose(snes_wide_snap *h, unsigned serial, const uint16_t **rgb, int *width, int *height, int *pitch_px, snes_wide_stats *st)
{
	Snap *sn = (Snap *) h;
	if (!sn) return -1;
	uint32_t exp = SS_TAKEN;
	if (sn->serial != serial || !sn->state.compare_exchange_strong(exp, SS_COMPOSING, std::memory_order_acq_rel)) return -1;
	int r = 0;
	A.lk.lock();
	if (A.on) {
		run(sn);
		Out *o = &A.out[(A.wr.load(std::memory_order_acquire) - 1) % NOUT];
		if (st) *st = o->st;
		if (o->avail && o->px) {
			if (rgb) *rgb = o->px;
			if (width) *width = o->st.width;
			if (height) *height = o->st.height;
			if (pitch_px) *pitch_px = o->st.width;
			r = 1;
		}
	} else r = -1;
	A.lk.unlock();
	sn->state.store(SS_FREE, std::memory_order_release);
	return r;
}

void snes_wide_set(int on, int margin)
{
	if (margin < 0) margin = 0;
	if (margin > SNES_WIDE_MAX_MARGIN) margin = SNES_WIDE_MAX_MARGIN;
	if (!on || !margin) {
		S9xWideSet(0, NULL, NULL);       /* waits for the PPU thread: no hook is running after this */
		A.lk.lock();
		A.on = 0; A.margin224 = 0; A.core_margin = 0; A.cap_mh = 0;
		pool_reset();
		A.serial.fetch_add(1);           /* the consumer sees "no wide frame" */
		for (int i = 0; i < NOUT; i++) A.out[i].avail = 0;
		A.lk.unlock();
		return;
	}
	if (A.on) S9xWideSet(0, NULL, NULL);   /* the hook (capture) is quiet while the setup changes */
	A.lk.lock();
	if (!A.settle) A.settle = 1;
	if (A.margin224 != margin) {
		A.margin224 = margin;
		A.cap_mh = work_margin(A.rung, margin);
		pool_reset();
		if (A.C) { wk_compose_destroy(A.C); A.C = NULL; A.cH = A.cM = A.cF = 0; }
	}
	ensure_objects();
	make_map();
	A.cap_mh = work_margin(A.rung, margin);
	A.on = 1;
	A.core_margin = margin_for(239) > margin ? margin_for(239) : margin;
	if (A.core_margin > SNES_WIDE_MAX_MARGIN) A.core_margin = SNES_WIDE_MAX_MARGIN;
	A.lk.unlock();
	S9xWideSet(A.core_margin, on_frame, NULL);
	apply_core_rung();
}

int snes_wide_active(void) { return A.on; }
int snes_wide_margin(void) { return A.margin224; }

void snes_wide_clear_game(void)
{
	S9xWideSet(0, NULL, NULL);
	A.lk.lock();
	A.on = 0;
	if (A.M) { wk_map_destroy(A.M); A.M = NULL; }
	A.have_game = 0; A.crc = 0; A.title[0] = 0; A.prof = NULL; A.prof_off = 0;
	pool_reset();
	memset(&A.cen, 0, sizeof A.cen);
	A.amb_have = 0; A.cov_have = 0; A.auto_state[0] = A.auto_state[1] = 1; A.xf[0] = A.xf[1] = 0; A.cov_q8[0] = A.cov_q8[1] = 0; A.frame_n = 0;
	A.map_saved_cells = 0;
	if (A.L) wk_layers_reset(A.L);
	if (A.C) wk_compose_reset(A.C);
	A.have_pal_prev = 0;
	for (int i = 0; i < NOUT; i++) A.out[i].avail = 0;
	A.serial.fetch_add(1);
	A.lk.unlock();
}

void snes_wide_set_game(const uint8_t *rom, size_t n, const char *title)
{
	snes_wide_clear_game();
	A.lk.lock();
	A.crc = rom && n ? crc32_of(rom, n) : 0;
	snprintf(A.title, sizeof A.title, "%s", title ? title : "");
	A.have_game = A.crc != 0;
	A.chip = rom_chip(rom, n);
	A.cost_ema_q4 = 0; A.cost_n = 0; A.since_down = 0; A.hold_frames = 600; memset(A.last_us, 0, sizeof A.last_us);
	A.rung = 0;     /* every game starts at the full margin (Super FX and SA-1 too): the measured cost and the player's pressure report are the last safety net */
	ensure_objects();
	profile_lookup();
	A.lk.unlock();
}

static void set_policy(bool on)
{
	A.lk.lock();
	A.policy.store(on ? 1 : 0);
	A.rung = 0;
	A.cost_n = 0; A.since_down = 0; A.last_up = false; A.hold_frames = 600; A.pressure.store(0);
	memset(A.last_us, 0, sizeof A.last_us);
	if (A.margin224) A.cap_mh = work_margin(A.rung, A.margin224);
	A.lk.unlock();
	apply_core_rung();
}

int snes_wide_option(const char *name, int v)
{
	if (!name) return 1;
	/* the player calls these every frame: no lock unless the value changes (compose holds the adapter lock for a whole frame) */
	if (!strcmp(name, "auto")) {
		if (A.auto_on.load() != (v != 0)) A.auto_on.store(v != 0);
		return 0;
	}
	if (!strcmp(name, "policy")) { if ((A.policy.load() != 0) != (v != 0)) set_policy(v != 0); return 0; }
	if (!strcmp(name, "mode")) {          /* the menu's Wide view: 0 Auto (per side Auto + cost ladder), 1 On (the full margin always, no ladder); Off is snes_wide_set(0, 0) */
		if (A.auto_on.load() != (v == 0)) A.auto_on.store(v == 0);
		if ((A.policy.load() != 0) != (v == 0)) set_policy(v == 0);
		return 0;
	}
	if (!strcmp(name, "pressure")) {      /* the frontend sees trouble (pacer not at full rate, a bad second): the ladder steps down at once, 2 = to the floor */
		if (v > 0) A.pressure.store(v > 2 ? 2 : v);
		return 0;
	}
	A.lk.lock();
	int r = 0;
	if (false) {}
	else if (!strcmp(name, "cost_us")) A.cost_us = v;
	else if (!strcmp(name, "cost_frames")) A.cost_frames = v;
	else if (!strcmp(name, "cost_scale")) A.cost_scale = v < 1 ? 1 : v;
	else if (!strcmp(name, "period")) { A.period = v != 0; if (A.C) { wk_compose_destroy(A.C); A.C = NULL; A.cH = A.cM = A.cF = 0; } }
	else if (!strcmp(name, "fade_px")) { A.fade_px = v; if (A.C) { wk_compose_destroy(A.C); A.C = NULL; A.cH = A.cM = A.cF = 0; } }
	else if (!strcmp(name, "appear_frames")) { A.appear = v; if (A.C) { wk_compose_destroy(A.C); A.C = NULL; A.cH = A.cM = A.cF = 0; } }
	else if (!strcmp(name, "settle_frames")) A.settle = (v < 0 ? 0 : v > 16 ? 16 : v) + 1;
	else if (!strcmp(name, "debug")) A.debug = v != 0;
	else if (!strcmp(name, "hud_excl")) A.hud_excl = v != 0;
	else if (!strcmp(name, "verify_cache")) { A.verify_cache = v != 0; S9xWideVerify(v != 0); }
	else if (!strcmp(name, "lazy")) A.lazy = v != 0;
	else if (!strcmp(name, "layered")) A.layered = v != 0;
	else if (!strcmp(name, "fgfade")) { A.fgfade = v != 0; if (A.C) { wk_compose_destroy(A.C); A.C = NULL; A.cH = A.cM = A.cF = 0; } }
	else if (!strcmp(name, "map_safe")) {
		A.map_safe = v;
		if (A.M) { wk_map_destroy(A.M); A.M = NULL; make_map(); }
	}
	else if (!strcmp(name, "learn")) {
		A.learn = v != 0;
		if (!A.learn && A.M) { wk_map_destroy(A.M); A.M = NULL; }
		else if (A.learn) make_map();
	}
	else r = 1;
	A.lk.unlock();
	return r;
}

int snes_wide_profiles_load(const char *text, size_t n, char *msgs, size_t cap)
{
	int problems = 0;
	A.lk.lock();
	if (A.db) { A.prof = NULL; if (A.L) wk_layers_set_profile(A.L, NULL); wk_profile_free(A.db); }
	A.db = wk_profile_parse(text, n, msgs, cap, &problems);
	ensure_objects();
	profile_lookup();
	A.lk.unlock();
	return problems;
}

int snes_wide_frame(const uint16_t **rgb, int *width, int *height, int *pitch_px, snes_wide_stats *st)
{
	if (!A.on) return 0;
	unsigned s = A.serial.load(std::memory_order_acquire);
	if (!s) return 0;
	/* the newest finished frame is the one written last: wr - 1 (the writer increments wr before publishing) */
	Out *o = &A.out[(A.wr.load(std::memory_order_acquire) - 1) % NOUT];
	if (st) *st = o->st;
	if (!o->avail || !o->px) return 0;
	if (rgb) *rgb = o->px;
	if (width) *width = o->st.width;
	if (height) *height = o->st.height;
	if (pitch_px) *pitch_px = o->st.width;
	return 1;
}

const uint8_t *snes_wide_real(void) { return &A.out[(A.wr.load() - 1) % NOUT].real[0][0]; }
const uint8_t *snes_wide_prov(void) { return A.debug ? A.out[(A.wr.load() - 1) % NOUT].prov : NULL; }

uint32_t snes_wide_map_key(void) { return A.have_game ? A.crc : 0; }

size_t snes_wide_map_save(uint8_t *buf, size_t cap)
{
	size_t need = 0;
	A.lk.lock();
	if (A.M) {
		need = wk_map_serialize_max(A.M);
		if (buf) {
			if (cap < need) need = 0;
			else {
				need = wk_map_serialize(A.M, buf, cap);
				wk_map_stats ms;
				wk_map_stats_get(A.M, &ms);
				if (need) A.map_pending_cells = (uint32_t) ms.cells;
			}
		}
	}
	A.lk.unlock();
	return need;
}

int snes_wide_map_load(const uint8_t *buf, size_t n)
{
	int r = 1;
	A.lk.lock();
	if (A.have_game) {
		make_map();
		if (A.M) {
			r = wk_map_deserialize(A.M, buf, n);
			wk_map_stats ms;
			wk_map_stats_get(A.M, &ms);
			A.map_saved_cells = (uint32_t) ms.cells;
		}
	}
	A.lk.unlock();
	return r;
}

int snes_wide_map_dirty(void)
{
	int d = 0;
	A.lk.lock();
	if (A.M) {
		wk_map_stats ms;
		wk_map_stats_get(A.M, &ms);
		d = (uint32_t) ms.cells != A.map_saved_cells;
	}
	A.lk.unlock();
	return d;
}

void snes_wide_map_mark_saved(void)
{
	A.lk.lock();
	A.map_saved_cells = A.map_pending_cells;   /* the cell count when the bytes that were written were taken */
	A.lk.unlock();
}

const char *snes_wide_reason_name(int r)
{
	static const char *n[SNES_WIDE_LR_N] = { "exact", "not rendered", "forced blank", "hi-res", "interlace", "offset-per-tile (mode 2/4/6)", "mosaic",
	                                         "HUD row", "layer not described", "window", "colour math", "ring shorter than margin", "bitmap tilemap" };
	return r >= 0 && r < SNES_WIDE_LR_N ? n[r] : "?";
}

void snes_wide_census_get(snes_wide_census *c)
{
	A.lk.lock();
	*c = A.cen;
	c->chip = A.chip; c->rung = A.rung;
	c->capture_us = A.cap_us_q4.load(std::memory_order_relaxed) / 16;
	c->cache_checks += A.vfy_checks.load(std::memory_order_relaxed);
	c->cache_mismatches += A.vfy_bad.load(std::memory_order_relaxed);
	A.lk.unlock();
}

void snes_wide_census_reset(void)
{
	A.lk.lock();
	memset(&A.cen, 0, sizeof A.cen);
	A.vfy_checks.store(0); A.vfy_bad.store(0);
	A.lk.unlock();
}

void snes_wide_reset(void)
{
	S9xWideReset();
}

} // extern "C"
