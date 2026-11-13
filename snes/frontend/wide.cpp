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

enum { MAXH = 240, RING_MAX = 64 * 64, NOUT = 4, PROV_EXACT = 10, PROV_SOFTL = 12, SOFT_W = 16, COV_ON = 85, COV_OFF = 75 };

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
};

/* One captured frame: exactly what the compose half reads (see "Capture and compose" in snes/WIDE-INTEGRATION.md). Margin pass
 * pictures are kept per line, side and distance index: px[pass][y][0][i] is buffer column (m - mh + i) of the core's pass (left
 * margin, i = mh - 1 - d for distance d from the picture), px[pass][y][1][i] is column m + 256 + i (i = d). Only the rows and passes
 * the line says it drew are copied; compose never reads the others. */
enum { SNAP_N = 5, SS_FREE = 0, SS_FILLING = 1, SS_FILLED = 2, SS_TAKEN = 3, SS_COMPOSING = 4 };
struct Snap {
	std::atomic<uint32_t> state;
	uint32_t serial;
	int h, m, mh, ok, wide_hint;
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
	int cH, cM;                      /* what C was created for */
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
	uint8_t softm[MAXH][2][SNES_WIDE_MAX_MARGIN];   /* per margin pixel: bit n = layer n shows a learned cell that has not been seen live in this run (blended toward the ambient frame) */
	int soft_on;
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
	int auto_on = 1;
	int cov_q8;                /* smoothed coverage, percent * 256 */
	int auto_state = 1;        /* 1 = real picture, 0 = ambient frame */
	int xf;                    /* cross-fade 0 (wide) .. 32 (ambient) */
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

/* ---- describing a frame -------------------------------------------------------------------------------------- */

struct Key { int scbase, scsize, name, bgsize, bpp, n; };

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
	memset(Ly->used, 0, sizeof Ly->used);
	memset(Ly->rowblank, 1, sizeof Ly->rowblank);
	const uint32_t gen = ++A.gen;
	Ly->gen = gen;
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
			}
		}

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
	for (y = 0; y < H; y++)
		if (!Ly->line_on[y]) {
			int j = y - 1;
			while (j >= 0 && !Ly->line_on[j]) j--;
			if (j < 0) { j = y + 1; while (j < H && !Ly->line_on[j]) j++; }
			if (j >= 0 && j < H) { L->scroll_x[y] = L->scroll_x[j]; L->row_y[y] = L->row_y[j]; }
		}
	(void) ndom;
	Ly->active = 1;
}

/* ---- output -------------------------------------------------------------------------------------------------- */

static void free_objects(void)
{
	if (A.C) wk_compose_destroy(A.C);
	A.C = NULL; A.cH = A.cM = 0;
	for (int i = 0; i < NOUT; i++) { free(A.out[i].px); free(A.out[i].prov); A.out[i].px = NULL; A.out[i].prov = NULL; }
	for (int l = 0; l < 4; l++) for (int s = 0; s < 2; s++) { free(A.cand[l][s]); free(A.opq[l][s]); A.cand[l][s] = NULL; A.opq[l][s] = NULL; }
}

static int make_compose(int H, int Mh)
{
	if (A.C && A.cH == H && A.cM == Mh)
		return 1;
	wk_compose_cfg cc;
	if (A.C) { wk_compose_destroy(A.C); A.C = NULL; }
	wk_compose_cfg_default(&cc);
	cc.period = A.period;
	cc.reveal_prov = 1;
	cc.fg_fade_px = A.fgfade ? 24 : 0;
	cc.fade_px = A.fade_px > 0 ? A.fade_px : 24;
	cc.soft_blur = 0; cc.soft_sample = 1; cc.soft_darken = 45; cc.soft_always = 1;   /* the base is the ambient colours below */
	for (int d = 0; d < Mh; d++) {
		int t = d * 256 / Mh, ease = (t * t) >> 8;
		A.amb_dk[d] = (uint8_t) (256 - (45 * 256 / 100) * ease / 256 > 255 ? 255 : 256 - (45 * 256 / 100) * ease / 256);
	}
	if (A.appear > 0) cc.appear_frames = A.appear;
	A.C = wk_compose_create(&cc, 256, H, Mh);
	if (!A.C) return 0;
	A.cH = H; A.cM = Mh;
	for (int i = 0; i < NOUT; i++) {
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

/* The ambient row (distances 0 .. M - 1) for a colour, side and dither phase: a pure function of those, so rows built for a
 * neighbouring line or an earlier frame are reused. Direct mapped. */
struct ARow { uint16_t key; uint8_t ph, ok; uint16_t px[SNES_WIDE_MAX_MARGIN]; };
static ARow g_arow[256];
static int g_arow_m;

static const uint16_t *amb_row(int s, int y, int M)
{
	if (g_arow_m != M) { memset(g_arow, 0, sizeof g_arow); g_arow_m = M; }
	const uint16_t key = A.amb_px[s][y];
	const uint8_t ph = (uint8_t) ((y & 3) | (s << 2));
	ARow *r = &g_arow[(unsigned) (key * 31u + ph * 7u + (key >> 8) * 5u) & 255];
	if (!r->ok || r->key != key || r->ph != ph) {
		const int r8 = ((key >> 11) & 31) * 255 / 31, g8 = ((key >> 5) & 63) * 255 / 63, b8 = (key & 31) * 255 / 31;
		for (int d = 0; d < M; d++) {
			const int f = A.amb_dk[d], xs = s == 0 ? M - 1 - d : M + 256 + d, th = kBay4[y & 3][xs & 3];
			int rr = ((r8 * f) >> 8) + (th >> 1), gg = ((g8 * f) >> 8) + (th >> 2), bb = ((b8 * f) >> 8) + (th >> 1);
			rr = rr > 255 ? 255 : rr; gg = gg > 255 ? 255 : gg; bb = bb > 255 ? 255 : bb;
			r->px[d] = (uint16_t) (((rr >> 3) << 11) | ((gg >> 2) << 5) | (bb >> 3));
		}
		r->key = key; r->ph = ph; r->ok = 1;
	}
	return r->px;
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

/* copies what compose needs out of the core's view; touches no compose state, so it can run on any thread while compose runs */
static void capture(const SWideView *v, Snap *sn)
{
	const int H = v->h, Mh = A.cap_mh.load(std::memory_order_relaxed);
	sn->h = H; sn->m = v->m; sn->mh = Mh; sn->ok = v->ok; sn->clock = v->clock; sn->epoch = v->epoch; sn->wide_hint = 0;
	if (H <= 0 || H > MAXH) return;
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
		for (int p = 0; p < SW_NPASS; p++) {
			if (!((L.pass >> p) & 1)) continue;
			const size_t row = (size_t) y * SW_PITCH;
			memcpy(sn->px[p][y][0], v->pass_px[p] + row + (Mc - Mh), (size_t) Mh * 2);
			memcpy(sn->px[p][y][1], v->pass_px[p] + row + Mc + 256, (size_t) Mh * 2);
			if (p) {
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
	const int Mh = margin_for(H), Mc = v->m;
	if (Mh <= 0 || Mc < Mh || sn->mh != Mh) { fallback(o, 2); return; }
	if (A.prof && A.prof->enable == WK_ENABLE_OFF) { fallback(o, 1); return; }
	if (!A.L || !make_compose(H, Mh) || !A.fr || !A.wo) { fallback(o, 2); return; }
	if (v->epoch != A.epoch) {
		A.epoch = v->epoch;
		A.amb_have = 0; A.cov_have = 0;
		wk_layers_reset(A.L);
		wk_compose_reset(A.C);
		A.have_pal_prev = 0;
		for (int l = 0; l < 4; l++) A.lay[l].have_gid = 0;
		memset(A.th2, 0, sizeof A.th2); memset(A.th4, 0, sizeof A.th4); memset(A.th8, 0, sizeof A.th8);
	}
	const int OW = 256 + 2 * Mh;
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

	/* 3. candidates */
	amb_update(v, H, wo->scene_event != 0);
	if (A.soft_on) memset(A.softm, 0, sizeof A.softm);
	(void) Mc;
	for (int y = 0; y < H; y++) {
		const SWideLine &Ln = v->line[y];
		wk_line_out *lo = &wo->line[y];
		const bool drawn = line_ok(Ln);
		if (lo->hud) st.lines_hud++;
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
		for (int l = 0; l < 4; l++) {
			if (!(nlay && A.lay[l].active)) continue;
			Layer *Ly = &A.lay[l];
			const int pass = SWP_BG0 + l;
			for (int s = 0; s < 2; s++) {
				uint16_t *dst = A.cand[l][s] + (size_t) y * Mh;
				uint8_t *op = A.opq[l][s] + (size_t) y * Mh;
				int real = lo->real[s][l], ring = lo->ring[s][l];
				if (!drawn || !Ly->line_on[y] || !((Ln.pass >> pass) & 1)) { lo->real[s][l] = 0; lo->ring[s][l] = 0; continue; }
				{
					/* the layer's ring row is blank all around: it draws nothing on this line, margin included */
					const int ty = fmod_(((int) Ln.vofs[l] + y) / Ly->tile_px, Ly->mh);
					if (Ly->rowblank[ty]) {
						/* blank all around the ring: assumed blank beyond it too, unless the learned map knows a tile there */
						bool nonblank = false;
						if (A.M && wo->coherent[l][y]) {
							wk_geom g2;
							wk_line_geom(fr, wo, l, y, &g2);
							for (int idx = 1; idx <= Mh / g2.tp + 1 && !nonblank; idx++) {
								wk_cell c2;
								if (!wk_map_margin_cell(A.M, fr, wo, l, y, s, idx, &c2)) continue;
								const int t2 = (int) (c2.code & 0x3ff);
								layer_tile_th(Ly, v, t2, Ly->gen, false);
								if (!Ly->tblank[t2]) nonblank = true;
							}
						}
						if (!nonblank) {
							memset(op, 0, (size_t) Mh);
							lo->real[s][l] = lo->ring[s][l] = (uint16_t) Mh;
							continue;
						}
					}
				}
				if (real > Mh) real = Mh;
				if (ring > real) ring = real;
				const uint16_t *pp = sn->px[pass][y][s];
				const uint8_t *pz = sn->z[pass][y][s];
				int d;
				for (d = 0; d < ring; d++) {
					const int col = s == 0 ? Mh - 1 - d : d;
					dst[d] = pp[col];
					op[d] = ((Ln.tsx >> l) & 1) || pz[col] != 0;   /* a backdrop + sub layer composite draws every pixel */
				}
				if (real > ring) {
					const int tp = Ly->tile_px;
					wk_geom g;
					const bool tsx_math = ((Ln.tsx >> l) & 1) && Ln.cgadsub == 0x20;   /* a sub screen layer added to the backdrop: the map cell gets the same maths */
					int have_geom = wo->coherent[l][y] != 0 && A.M && (!((Ln.tsx >> l) & 1) || tsx_math);
					if (have_geom) wk_line_geom(fr, wo, l, y, &g);
					int cidx = 0x7fffffff, cok = 0;
					wk_cell cell;
					for (; d < real; d++) {
						if (!have_geom) { lo->real[s][l] = (uint16_t) d; break; }
						const int wx = wo->world_x[l][y];
						const int idx = s == 0 ? (int) (g.tile_first - fdiv(wx - 1 - d, tp))
						                       : (int) (fdiv(wx + 256 + d, tp) - (g.tile_first + g.nvis - 1));
						if (idx != cidx) {
							cok = wk_map_margin_cell(A.M, fr, wo, l, y, s, idx, &cell); cidx = idx;
							if (cok) {   /* the map holds a tilemap word, not pixels: shown only if that tile's graphics are today what they were when it was learned */
								const uint32_t th = layer_tile_th(Ly, v, (int) (cell.code & 0x3ff), Ly->gen, false);
								const uint32_t hh = mix32(cell.code * 0x10001u + 0x1234567u, th);
								cok = (uint16_t) ((hh ^ (hh >> 16)) & 0xFFFF) == cell.h16;
							}
						}
						if (!cok) { lo->real[s][l] = (uint16_t) d; break; }
						const int xw = s == 0 ? wx - 1 - d : wx + 256 + d;
						const int ci = cell_index(v, Ly, cell.code, fmod_(xw, tp), fmod_(wo->world_row[l][y], tp));
						if (tsx_math) { dst[d] = math_add565(Ln.backdrop, ci < 0 ? Ln.fix565 : v->colors[ci]); op[d] = 1; }
						else if (ci < 0) { dst[d] = 0; op[d] = 0; }
						else { dst[d] = v->colors[ci]; op[d] = 1; }
						if (A.soft_on && !cell.live && op[d]) {
							/* a memory of an earlier run or session, not seen in this one: a hint, shown softly */
							dst[d] = mix565(dst[d], amb_row(s, y, Mh)[d], SOFT_W);
							A.softm[y][s][d] |= (uint8_t) (1 << l);
						}
					}
				}
			}
		}
		wk_out_refresh_line(wo, y);
	}

	/* the layers' own pixels at the view edge, for the foreground fade (what a layer that is not known beside the view continues with) */
	if (A.fgfade && nlay)
		for (int l = 0; l < 4; l++) {
			Layer *Ly = &A.lay[l];
			if (!Ly->active) continue;
			const int tp = Ly->tile_px;
			for (int y = 0; y < H; y++) {
				const SWideLine &Ln = v->line[y];
				const bool ok = line_ok(Ln) && Ly->line_on[y] && !A.xl[y] && !wo->line[y].hud && !(Ln.flags & SWL_MODE7);
				for (int s = 0; s < 2; s++) {
					A.tailop[l][s][y] = 0;
					if (!ok) continue;
					const int X = (int) Ln.hofs[l] + (s ? 255 : 0), Y = (int) Ln.vofs[l] + y;
					const uint32_t word = Ly->code[fmod_(fdiv(Y, tp), Ly->mh) * Ly->mw + fmod_(fdiv(X, tp), Ly->mw)];
					if (word == WK_CODE_NOREC) continue;
					const int px = cell_index(v, Ly, word, fmod_(X, tp), fmod_(Y, tp));
					if (px < 0) continue;
					A.tailpx[l][s][y] = v->colors[px];
					A.tailop[l][s][y] = 1;
				}
			}
		}

	/* 4. compose */
	wk_compose_in ci;
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
	wk_compose_frame(A.C, &ci);

	/* 5. exact composite where every layer of the line is real, sprites over the rest, Mode 7 whole */
	const int fade = A.fade_px > 0 ? A.fade_px : 20;
	const bool sprites_off = A.prof && A.prof->sprites_off;
	long sl = 0, sr = 0;
	int nreal = 0;
	for (int y = 0; y < H; y++) {
		const SWideLine &Ln = v->line[y];
		wk_line_out *lo = &wo->line[y];
		uint16_t *row = o->px + (size_t) y * OW;
		uint8_t *prow = o->prov + (size_t) y * OW;
		o->real[y][0] = o->real[y][1] = 0;
		if (!(Ln.flags & SWL_VALID)) { A.cen.line_reason[SNES_WIDE_LR_NOTVALID]++; continue; }
		if (Ln.flags & SWL_BLANK) {
			A.cen.line_reason[SNES_WIDE_LR_BLANK]++;
			for (int d = 0; d < Mh; d++) { row[Mh - 1 - d] = 0; row[Mh + 256 + d] = 0; prow[Mh - 1 - d] = prow[Mh + 256 + d] = PROV_EXACT; }
			continue;
		}
		if (!line_ok(Ln)) {
			A.cen.line_reason[(Ln.flags & SWL_HIRES) ? SNES_WIDE_LR_HIRES : (Ln.flags & SWL_OPT) ? SNES_WIDE_LR_OPT : (Ln.flags & SWL_MOSAIC) ? SNES_WIDE_LR_MOSAIC : SNES_WIDE_LR_INTERLACE]++;
			continue;
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
			const uint16_t *cp = sn->px[SWP_COMPOSITE][y][s];
			for (int d = 0; d < cex; d++) {
				const int col = s == 0 ? Mh - 1 - d : d;
				const int dx = s == 0 ? Mh - 1 - d : Mh + 256 + d;
				row[dx] = cp[col];
				prow[dx] = PROV_EXACT;
			}
			if (!(Ln.flags & SWL_MODE7) && !sprites_off && ((Ln.pass >> SWP_OBJ) & 1)) {
				const uint16_t *sp = sn->px[SWP_OBJ][y][s];
				const uint8_t *sz = sn->z[SWP_OBJ][y][s];
				/* sprites end where the layers' real picture ends (same fade): never a sprite floating on the ambient colour */
				const int sbm = rmax_ > nlay_ ? rmax_ : nlay_;
				const int sb = sbm >= Mh ? Mh : sbm;
				for (int d = cex; d < sb; d++) {
					const int col = s == 0 ? Mh - 1 - d : d;
					const int dx = s == 0 ? Mh - 1 - d : Mh + 256 + d;
					if (!sz[col]) continue;
					const int w = (sb < Mh && d >= sb - fade) ? (sb - d) * 32 / fade : 32;
					row[dx] = w >= 32 ? sp[col] : mix565(row[dx], sp[col], w);
					prow[dx] = w >= 32 ? PROV_EXACT : WK_PROV_FADE;      /* a faded sprite pixel is not a claim of real picture */
				}
			}
			o->real[y][s] = (uint8_t) (cla > cex ? cla : cex);
			if (s == 0) sl += o->real[y][0]; else sr += o->real[y][1];
		}
		nreal++;
		if (o->real[y][0] == Mh && o->real[y][1] == Mh) { st.lines_exact++; why = SNES_WIDE_LR_EXACT; }
		else if (!why) why = SNES_WIDE_LR_RING;
		A.cen.line_reason[why]++;
	}
	st.real_l = nreal ? (int) (sl / nreal) : 0;
	st.real_r = nreal ? (int) (sr / nreal) : 0;

	/* provenance of soft learned pixels: a hint, never a claim of real picture */
	if (A.soft_on)
		for (int y = 0; y < H; y++) {
			if (!line_ok(v->line[y])) continue;
			uint8_t *prow = o->prov + (size_t) y * OW;
			for (int s = 0; s < 2; s++) {
				const uint8_t *sm = A.softm[y][s];
				for (int d = 0; d < Mh; d++) {
					if (!sm[d]) continue;
					uint8_t &p = prow[s == 0 ? Mh - 1 - d : Mh + 256 + d];
					if ((p & 15) == WK_PROV_LEVEL && ((sm[d] >> (p >> 4)) & 1)) p = (uint8_t) ((p & 0xF0) | PROV_SOFTL);
				}
			}
		}

	/* 6. census, coverage and the Auto decision (per game and scene) */
	if (wo->scene_event) A.cen.scenes++;
	if ((A.frame_n & 1) == 0 || !A.cov_have) {
		uint64_t ctm = 0, cln = 0, cbd = 0, cun = 0, csf = 0, crv = 0;
		for (int y = 0; y < H; y++) {
			const SWideLine &Ln = v->line[y];
			if (!line_ok(Ln)) continue;
			const wk_line_out *lo = &wo->line[y];
			if (A.hud_excl && lo->hud) continue;      /* a status bar is original width by design: not a gap in the margin */
			const uint8_t *prow = o->prov + (size_t) y * OW;
			for (int s = 0; s < 2; s++) {
				int ring_e = 0, real_e = 0;
				for (int l = 0; l < 4; l++) {
					if (lo->ring[s][l] > ring_e) ring_e = lo->ring[s][l];
					if (lo->real[s][l] > real_e) real_e = lo->real[s][l];
				}
				int tm = 0, bd = 0, un = 0, sf = 0, rv = 0;
				const uint8_t *pp = prow + (s == 0 ? 0 : Mh + 256);
				for (int d = 0; d < Mh; d++) {
					const int p = pp[s == 0 ? Mh - 1 - d : d] & 15;
					if (p == WK_PROV_LEVEL || p == PROV_EXACT || p == WK_PROV_FADE) tm++;
					else if (p == WK_PROV_BACKDROP || p == WK_PROV_BG) bd++;
					else if (p == PROV_SOFTL) sf++;
					else if (p == WK_PROV_REVEAL) rv++;
					else un++;
				}
				int ln_ = real_e > ring_e ? real_e - ring_e : 0;
				if (ln_ > tm) ln_ = tm;
				ctm += (uint64_t) (tm - ln_); cln += (uint64_t) ln_; cbd += (uint64_t) bd; cun += (uint64_t) un;
				csf += (uint64_t) sf; crv += (uint64_t) rv;
			}
		}
		const uint64_t tot = ctm + cln + cbd + cun + csf + crv;
		A.cen.px_tilemap += ctm; A.cen.px_learned += cln; A.cen.px_backdrop += cbd; A.cen.px_unknown += cun;
		A.cen.px_soft += csf; A.cen.px_reveal += crv;
		if (tot) {
			/* a revealed lower layer is real picture (the layer above it fades out over it: B); soft learned pixels are memory of an
			 * earlier run, not a proven picture: they count half */
			const int c256 = (int) (((ctm + cln + cbd + crv) * 2 + csf) * 50 * 256 / tot);
			if (!A.cov_have || wo->scene_event) { A.cov_q8 = c256; A.cov_have = 1; }
			else A.cov_q8 += (c256 - A.cov_q8) / 60;
		}
	}
	if (A.auto_on) {
		const int want = A.auto_state ? (A.cov_q8 >= COV_OFF * 256) : (A.cov_q8 >= COV_ON * 256);
		if (want != A.auto_state) { A.auto_state = want; A.cen.auto_switches++; }
	} else {
		A.auto_state = 1;
	}
	{
		const int target = A.auto_state ? 0 : 32;
		if (!A.auto_on) A.xf = 0;
		else if (A.xf < target) A.xf++;
		else if (A.xf > target) A.xf--;
	}
	if (A.xf > 0) {
		const int xf = A.xf;
		for (int y = 0; y < H; y++) {
			const SWideLine &Ln = v->line[y];
			if (!(Ln.flags & SWL_VALID) || (Ln.flags & SWL_BLANK)) continue;
			uint16_t *row = o->px + (size_t) y * OW;
			for (int s = 0; s < 2; s++) {
				const uint16_t *ap = amb_row(s, y, Mh);
				if (xf >= 32) {
					if (s == 0) for (int d = 0; d < Mh; d++) row[Mh - 1 - d] = ap[d];
					else memcpy(row + Mh + 256, ap, (size_t) Mh * 2);
				} else {
					for (int d = 0; d < Mh; d++) {
						const int dx = s == 0 ? Mh - 1 - d : Mh + 256 + d;
						row[dx] = mix565(row[dx], ap[d], xf);
					}
				}
			}
			if (xf >= 32) o->real[y][0] = o->real[y][1] = 0;
		}
		if (xf >= 32) A.cen.frames_ambient++; else A.cen.frames_crossfade++;
	}
	st.coverage_pct = A.cov_q8 >> 8; st.auto_wide = A.auto_state; st.xfade = A.xf;
	A.cen.frames_wide++;
	A.cen.coverage_pct = st.coverage_pct; A.cen.auto_wide = A.auto_state;
	A.cen.map_cells = (uint32_t) st.map_cells;
	st.instr_us = (uint32_t) (now_us() - t0);
	publish(o, 1);
}

static void on_frame(const SWideView *v, void *)
{
	if (!A.on) return;
	if (A.split.load(std::memory_order_acquire)) {
		Snap *sn = pool_claim();
		if (!sn) return;
		capture(v, sn);
		sn->serial = A.snap_serial.fetch_add(1) + 1;
		sn->state.store(SS_FILLED, std::memory_order_release);
		return;
	}
	if (!A.own) A.own = snap_alloc();
	if (!A.own) return;
	capture(v, A.own);
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
		A.cap_mh = margin;
		pool_reset();
		if (A.C) { wk_compose_destroy(A.C); A.C = NULL; A.cH = A.cM = 0; }
	}
	ensure_objects();
	make_map();
	A.cap_mh = margin;
	A.on = 1;
	A.core_margin = margin_for(239) > margin ? margin_for(239) : margin;
	if (A.core_margin > SNES_WIDE_MAX_MARGIN) A.core_margin = SNES_WIDE_MAX_MARGIN;
	A.lk.unlock();
	S9xWideSet(A.core_margin, on_frame, NULL);
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
	A.amb_have = 0; A.cov_have = 0; A.auto_state = 1; A.xf = 0; A.frame_n = 0;
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
	ensure_objects();
	profile_lookup();
	A.lk.unlock();
}

int snes_wide_option(const char *name, int v)
{
	if (!name) return 1;
	A.lk.lock();
	int r = 0;
	if (!strcmp(name, "auto")) A.auto_on = v != 0;
	else if (!strcmp(name, "period")) { A.period = v != 0; if (A.C) { wk_compose_destroy(A.C); A.C = NULL; A.cH = A.cM = 0; } }
	else if (!strcmp(name, "fade_px")) { A.fade_px = v; if (A.C) { wk_compose_destroy(A.C); A.C = NULL; A.cH = A.cM = 0; } }
	else if (!strcmp(name, "appear_frames")) { A.appear = v; if (A.C) { wk_compose_destroy(A.C); A.C = NULL; A.cH = A.cM = 0; } }
	else if (!strcmp(name, "settle_frames")) A.settle = (v < 0 ? 0 : v > 16 ? 16 : v) + 1;
	else if (!strcmp(name, "debug")) A.debug = v != 0;
	else if (!strcmp(name, "hud_excl")) A.hud_excl = v != 0;
	else if (!strcmp(name, "layered")) A.layered = v != 0;
	else if (!strcmp(name, "fgfade")) { A.fgfade = v != 0; if (A.C) { wk_compose_destroy(A.C); A.C = NULL; A.cH = A.cM = 0; } }
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
	A.lk.unlock();
}

void snes_wide_census_reset(void)
{
	A.lk.lock();
	memset(&A.cen, 0, sizeof A.cen);
	A.lk.unlock();
}

void snes_wide_reset(void)
{
	S9xWideReset();
}

} // extern "C"
