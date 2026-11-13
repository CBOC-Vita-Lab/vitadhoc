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

enum { MAXH = 240, RING_MAX = 64 * 64, NOUT = 4, PROV_EXACT = 10 };

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
struct TileHash { uint32_t h; uint16_t age; uint8_t ok; };

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
	int on, margin224, core_margin;
	int period, fade_px, appear, settle, debug, layered, learn;
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
	uint32_t map_saved_cells;
	/* output ring */
	Out out[NOUT];
	std::atomic<unsigned> serial;
	std::atomic<unsigned> wr;
	int outW, outH;
};

static Adapter A;

/* ---- geometry helpers ---------------------------------------------------------------------------------------- */

/* margin for a picture of h lines that keeps the 224 line picture's aspect (70 px at 224 -> 83 at 239) */
static int margin_for(int h)
{
	if (h == 224 || A.margin224 <= 0)
		return A.margin224;
	int total = (h * (256 + 2 * A.margin224) + 112) / 224;
	int m = (total - 256) / 2;
	if (m < 0) m = 0;
	if (m > SNES_WIDE_MAX_MARGIN) m = SNES_WIDE_MAX_MARGIN;
	return m;
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
	uint32_t h = 0x811C9DC5u;
	for (int i = 0; i < bytes; i += 4)
		h = mix32(h, (uint32_t) v->vram[(a + i) & 0xffff] | ((uint32_t) v->vram[(a + i + 1) & 0xffff] << 8) |
		             ((uint32_t) v->vram[(a + i + 2) & 0xffff] << 16) | ((uint32_t) v->vram[(a + i + 3) & 0xffff] << 24));
	c->h = h; c->age = age; c->ok = 1;
	return h;
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
		if (n >= bg_count(L.mode) || !(L.tm & (1 << n))) continue;
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
	for (int ry = 0; ry < mh; ry++)
		for (int rx = 0; rx < mw; rx++) {
			const int k = ry * mw + rx;
			const int s = ((mh == 64 && ry >= 32) ? 2 : 0) + ((mw == 64 && rx >= 32) ? 1 : 0);
			const int wa = (scr[s] + (ry & 31) * 32 + (rx & 31)) & 0x7fff;
			const uint32_t word = ld16(v->vram + wa * 2);
			const int tile = (int) (word & 0x3ff);
			uint32_t th;
			if (Ly->tile_px == 8) {
				th = tile_hash(v, K.bpp, tile, K.name);
				if (!Ly->used[tile]) { Ly->used[tile] = 1; Ly->gh[tile] = th; }
			} else {
				int t1 = (tile + 1) & 0x3ff, t2 = (tile + 16) & 0x3ff, t3 = (tile + 17) & 0x3ff;
				uint32_t h0 = tile_hash(v, K.bpp, tile, K.name), h1 = tile_hash(v, K.bpp, t1, K.name);
				uint32_t h2 = tile_hash(v, K.bpp, t2, K.name), h3 = tile_hash(v, K.bpp, t3, K.name);
				th = mix32(mix32(mix32(h0, h1), h2), h3);
				if (!Ly->used[tile]) { Ly->used[tile] = 1; Ly->gh[tile] = th; }
			}
			uint32_t st = 0;
			const uint16_t w16 = v->vage[wa];
			if (w16) {
				uint32_t age = (uint16_t) (clk16 - w16);
				if (age < 32768 && age <= clk) st = clk - age;
			}
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

	/* graphics identity: kept through cosmetic animation (a few tiles changing), new for a tile set swap */
	{
		int nused = 0, same = 0;
		for (i = 0; i < 1024; i++) {
			if (!Ly->used[i]) continue;
			nused++;
			if (Ly->gh_prev_ok[i] && Ly->gh_prev[i] == Ly->gh[i]) same++;
		}
		if (!Ly->have_gid || same * 10 < nused * 9) {
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
	L->gfx_id = Ly->gid ^ (uint32_t) (mw * 977 + mh * 131 + Ly->tile_px * 7 + K.bpp);
	L->line_on = Ly->line_on;
	int ndom = 0;
	for (y = 0; y < H; y++) {
		const SWideLine &Ln = v->line[y];
		const bool on = line_ok(Ln) && !(Ln.flags & SWL_MODE7) && n < bg_count(Ln.mode) && (Ln.tm & (1 << n)) &&
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
	if (A.fade_px > 0) cc.fade_px = A.fade_px;
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

static void profile_lookup(void)
{
	A.prof = NULL;
	if (A.db && A.have_game) A.prof = wk_profile_find(A.db, A.crc, A.title);
	if (A.L) wk_layers_set_profile(A.L, A.prof);
}

static void make_map(void)
{
	wk_map_cfg mc;
	if (A.M || !A.have_game || !A.learn) return;
	wk_map_cfg_default(&mc);
	mc.max_cells = 32768;
	A.M = wk_map_create(&mc, A.crc);
}

/* ---- the frame ------------------------------------------------------------------------------------------------ */

static void run(const SWideView *v)
{
	const uint64_t t0 = now_us();
	Out *o = &A.out[A.wr.load() % NOUT];
	snes_wide_stats &st = o->st;
	memset(&st, 0, sizeof st);
	st.serial = A.serial.load() + 1;
	st.frame = v->clock;
	const int H = v->h;
	o->avail = 0;
	if (H <= 0 || H > MAXH || !v->ok || A.margin224 <= 0) { publish(o, 0); return; }
	const int Mh = margin_for(H), Mc = v->m;
	if (Mh <= 0 || Mc < Mh) { publish(o, 0); return; }
	if (A.prof && A.prof->enable == WK_ENABLE_OFF) { publish(o, 0); return; }
	if (!A.L || !make_compose(H, Mh) || !A.fr || !A.wo) { publish(o, 0); return; }
	if (v->epoch != A.epoch) {
		A.epoch = v->epoch;
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
	if (nd == 0 && nblank == 0) { publish(o, 0); return; }
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
	const int Mc_ = Mc;
	for (int y = 0; y < H; y++) {
		const SWideLine &Ln = v->line[y];
		wk_line_out *lo = &wo->line[y];
		const bool drawn = line_ok(Ln);
		if (lo->hud) st.lines_hud++;
		for (int l = 0; l < 4; l++) {
			if (!(nlay && A.lay[l].active)) continue;
			Layer *Ly = &A.lay[l];
			const int pass = SWP_BG0 + l;
			for (int s = 0; s < 2; s++) {
				uint16_t *dst = A.cand[l][s] + (size_t) y * Mh;
				uint8_t *op = A.opq[l][s] + (size_t) y * Mh;
				int real = lo->real[s][l], ring = lo->ring[s][l];
				if (!drawn || !Ly->line_on[y] || !((Ln.pass >> pass) & 1)) { lo->real[s][l] = 0; lo->ring[s][l] = 0; continue; }
				if (real > Mh) real = Mh;
				if (ring > real) ring = real;
				const uint16_t *pp = v->pass_px[pass] + (size_t) y * SW_PITCH;
				const uint8_t *pz = v->pass_z[pass] + (size_t) y * SW_PITCH;
				int d;
				for (d = 0; d < ring; d++) {
					int col = s == 0 ? Mc_ - 1 - d : Mc_ + 256 + d;
					dst[d] = pp[col];
					op[d] = pz[col] != 0;
				}
				if (real > ring) {
					const int tp = Ly->tile_px;
					wk_geom g;
					int have_geom = wo->coherent[l][y] != 0 && A.M;
					if (have_geom) wk_line_geom(fr, wo, l, y, &g);
					int cidx = 0x7fffffff, cok = 0;
					wk_cell cell;
					for (; d < real; d++) {
						if (!have_geom) { lo->real[s][l] = (uint16_t) d; break; }
						const int wx = wo->world_x[l][y];
						const int idx = s == 0 ? (int) (g.tile_first - fdiv(wx - 1 - d, tp))
						                       : (int) (fdiv(wx + 256 + d, tp) - (g.tile_first + g.nvis - 1));
						if (idx != cidx) { cok = wk_map_margin_cell(A.M, fr, wo, l, y, s, idx, &cell); cidx = idx; }
						if (!cok) { lo->real[s][l] = (uint16_t) d; break; }
						const int xw = s == 0 ? wx - 1 - d : wx + 256 + d;
						const int ci = cell_index(v, Ly, cell.code, fmod_(xw, tp), fmod_(wo->world_row[l][y], tp));
						if (ci < 0) { dst[d] = 0; op[d] = 0; }
						else { dst[d] = v->colors[ci]; op[d] = 1; }
					}
				}
			}
		}
		wk_out_refresh_line(wo, y);
	}

	/* 4. compose */
	wk_compose_in ci;
	memset(&ci, 0, sizeof ci);
	for (int y = 0; y < H; y++) {
		const SWideLine &Ln = v->line[y];
		A.bg[y] = (Ln.flags & SWL_VALID) ? Ln.backdrop : 0;
	}
	ci.ana = wo;
	ci.centre = v->centre; ci.centre_stride = v->centre_pitch;
	ci.bg = A.bg;
	ci.norder = 0;
	if (nlay)
		for (int l = 3; l >= 0; l--)
			if (A.lay[l].active) {
				ci.cand[l][0] = A.cand[l][0]; ci.cand[l][1] = A.cand[l][1];
				ci.opaque[l][0] = A.opq[l][0]; ci.opaque[l][1] = A.opq[l][1];
				ci.order[ci.norder++] = l;
			}
	ci.out = o->px; ci.out_stride = OW;
	ci.prov = A.debug ? o->prov : NULL;
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
		uint8_t *prow = A.debug ? o->prov + (size_t) y * OW : NULL;
		o->real[y][0] = o->real[y][1] = 0;
		if (!(Ln.flags & SWL_VALID)) continue;
		if (Ln.flags & SWL_BLANK) {
			for (int d = 0; d < Mh; d++) { row[Mh - 1 - d] = 0; row[Mh + 256 + d] = 0; if (prow) prow[Mh - 1 - d] = prow[Mh + 256 + d] = PROV_EXACT; }
			continue;
		}
		if (!line_ok(Ln)) continue;
		for (int s = 0; s < 2; s++) {
			int nex = Mh, nlay_ = Mh;     /* nex: every enabled layer is real (exact composite); nlay_: every layer that is not a HUD */
			if (!(Ln.flags & SWL_MODE7)) {
				if (lo->hud) nex = nlay_ = 0;
				for (int l = 0; l < bg_count(Ln.mode); l++) {
					if (!(Ln.tm & (1 << l))) continue;
					if (!(nlay && A.lay[l].active && A.lay[l].line_on[y])) { nex = nlay_ = 0; break; }
					const int r = lo->real[s][l];
					const int kind = wo->kind[l];
					if (r < nex) nex = r;
					if (kind != WK_KIND_HUD && kind != WK_KIND_OFF && r < nlay_) nlay_ = r;
					if (kind == WK_KIND_HUD || kind == WK_KIND_OFF) nex = 0;
				}
			}
			const int cex = nex >= Mh ? Mh : (nex > fade ? nex - fade : 0);
			const int cla = nlay_ >= Mh ? Mh : (nlay_ > fade ? nlay_ - fade : 0);
			const uint16_t *cp = v->pass_px[SWP_COMPOSITE] + (size_t) y * SW_PITCH;
			for (int d = 0; d < cex; d++) {
				const int col = s == 0 ? Mc_ - 1 - d : Mc_ + 256 + d;
				const int dx = s == 0 ? Mh - 1 - d : Mh + 256 + d;
				row[dx] = cp[col];
				if (prow) prow[dx] = PROV_EXACT;
			}
			if (!(Ln.flags & SWL_MODE7) && !sprites_off && ((Ln.pass >> SWP_OBJ) & 1)) {
				const uint16_t *sp = v->pass_px[SWP_OBJ] + (size_t) y * SW_PITCH;
				const uint8_t *sz = v->pass_z[SWP_OBJ] + (size_t) y * SW_PITCH;
				for (int d = cex; d < Mh; d++) {
					const int col = s == 0 ? Mc_ - 1 - d : Mc_ + 256 + d;
					const int dx = s == 0 ? Mh - 1 - d : Mh + 256 + d;
					if (sz[col]) { row[dx] = sp[col]; if (prow) prow[dx] = PROV_EXACT; }
				}
			}
			o->real[y][s] = (uint8_t) (cla > cex ? cla : cex);
			if (s == 0) sl += o->real[y][0]; else sr += o->real[y][1];
		}
		nreal++;
		if (o->real[y][0] == Mh && o->real[y][1] == Mh) st.lines_exact++;
	}
	st.real_l = nreal ? (int) (sl / nreal) : 0;
	st.real_r = nreal ? (int) (sr / nreal) : 0;
	st.instr_us = (uint32_t) (now_us() - t0);
	publish(o, 1);
}

static void on_frame(const SWideView *v, void *)
{
	A.lk.lock();
	if (A.on) run(v);
	A.lk.unlock();
}

} // namespace

/* ---- public control ------------------------------------------------------------------------------------------ */

extern "C" {

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

void snes_wide_set(int on, int margin)
{
	if (margin < 0) margin = 0;
	if (margin > SNES_WIDE_MAX_MARGIN) margin = SNES_WIDE_MAX_MARGIN;
	if (!on || !margin) {
		S9xWideSet(0, NULL, NULL);       /* waits for the PPU thread: no hook is running after this */
		A.lk.lock();
		A.on = 0; A.margin224 = 0; A.core_margin = 0;
		A.serial.fetch_add(1);           /* the consumer sees "no wide frame" */
		for (int i = 0; i < NOUT; i++) A.out[i].avail = 0;
		A.lk.unlock();
		return;
	}
	A.lk.lock();
	if (!A.settle) A.settle = 1;
	if (A.margin224 != margin) {
		A.margin224 = margin;
		if (A.C) { wk_compose_destroy(A.C); A.C = NULL; A.cH = A.cM = 0; }
	}
	ensure_objects();
	make_map();
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
	A.have_game = 0; A.crc = 0; A.title[0] = 0; A.prof = NULL;
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
	if (!strcmp(name, "period")) { A.period = v != 0; if (A.C) { wk_compose_destroy(A.C); A.C = NULL; A.cH = A.cM = 0; } }
	else if (!strcmp(name, "fade_px")) { A.fade_px = v; if (A.C) { wk_compose_destroy(A.C); A.C = NULL; A.cH = A.cM = 0; } }
	else if (!strcmp(name, "appear_frames")) { A.appear = v; if (A.C) { wk_compose_destroy(A.C); A.C = NULL; A.cH = A.cM = 0; } }
	else if (!strcmp(name, "settle_frames")) A.settle = (v < 0 ? 0 : v > 16 ? 16 : v) + 1;
	else if (!strcmp(name, "debug")) A.debug = v != 0;
	else if (!strcmp(name, "layered")) A.layered = v != 0;
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
				if (need) A.map_saved_cells = (uint32_t) ms.cells;
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
	if (A.M) { wk_map_stats ms; wk_map_stats_get(A.M, &ms); A.map_saved_cells = (uint32_t) ms.cells; }
	A.lk.unlock();
}

void snes_wide_reset(void)
{
	S9xWideReset();
}

} // extern "C"
