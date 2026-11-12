/*
 * NES wide view adapter: turns what the PPU recorder (core/nesp_wide.h) saw into a widekit frame description, runs
 * widekit (wk_layers, wk_map, wk_compose) and returns the composed (256 + 2 * margin) x 240 RGB565 picture.
 * The emulation never reads anything from here; see INTEGRATION.md "Wide view".
 *
 * The scout (built with -DNESW_SCOUT=1 and linked with scoutcore.o: the same core + host.c with every global renamed
 * nesscout_*): a second machine started from the live game's saved state, run ahead with the direction the camera moves,
 * whose background pictures are described the same way and recorded into the learned map as look-ahead cells
 * (wk_map_record_scout). It shares no memory with the live machine.
 *
 * Copyright CBOC-Vita. MIT licence (see nes/frontend/LICENSE).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fceu-types.h"
#include "fceu.h"
#include "git.h"
#include "nesp_wide.h"
#include "wk_types.h"
#include "wk_layers.h"
#include "wk_map.h"
#include "wk_compose.h"
#include "wk_profile.h"
#include "host.h"

#ifndef NESW_SCOUT
#define NESW_SCOUT 0
#endif

extern uint8_t *XDBuf;
extern uint8_t PALRAM[0x20];
extern FCEUGI *GameInfo;
extern uint8_t PPU[4];

/* from host.c */
extern int nesw_game_loaded(void);
extern const uint16_t *nesw_pal(void);
extern const uint16_t *nesw_fb(void);
extern uint32_t nesw_game_crc(void);
extern const char *nesw_game_title(void);
extern const uint8_t *nesw_sprm(void);
extern int nesw_render_on(void);
extern unsigned nesw_pad(int port);
extern int nesw_is_cart(void);

#define H 240
#define RING_MAX (64 * 60)

/* one machine's PPU record (the live core, or the scout's renamed copy) */
typedef struct {
	NespLine *line;
	uint32_t (*ntstamp)[1024];
	int (*ntid)(const uint8_t *page);
	const uint8_t *(*chr_base)(int *size);
	uint32_t frame;
} NesInst;

/* the description of one frame for widekit */
typedef struct {
	wk_frame *fr;
	uint32_t hash[RING_MAX], code[RING_MAX], code2[RING_MAX], stamp[RING_MAX];
	uint8_t line_on[H];
	const uint8_t *bp[4], *np[4];
	int mw, mh, distx, disty, ndom, rendering;
	uint8_t used[256];
	uint32_t gh[256];
	uint32_t sig;
} Desc;

typedef struct {
	int mode, margin_req, margin;         /* mode 0 off 1 auto */
	int active;
	int period, debug, fade_px, appear, settle;
	wk_layers *L;
	wk_map *M;
	wk_compose *C;
	wk_profile_db *db;
	const wk_profile *prof;
	int have_game;
	uint32_t crc;
	Desc D;
	wk_out *out;
	uint16_t *wide;                       /* (256 + 2 * margin) x 240 */
	uint16_t *cand[2];                    /* 240 x margin */
	uint16_t *inner[2];                   /* 240 x WK_MAX_INNER */
	uint8_t *prov;
	uint16_t bg[H];
	uint8_t real[H][2];
	uint8_t pal_prev[32];
	int have_pal_prev;
	/* tile content identity (gfx_id) */
	uint32_t gh_prev[256];
	uint8_t gh_prev_ok[256];
	uint32_t gid, prev_sig, layer_gfx;
	int have_gid;
	/* stats */
	nes_wide_stats st;
	uint32_t map_cells_saved;
	const uint8_t *dom_bp[4];
	/* uniform-row backdrop fill (screens that never scroll sideways) */
	int hscrolled;                        /* sticky per game: a line's horizontal scroll changed between two frames */
	int prev_sx[H];
	int still_frames;                     /* frames with a background layer seen so far (the fill waits for 120) */
	uint8_t prev_on[H];
} Wide;

static Wide W;

/* ---- small helpers ----------------------------------------------------------------------------------------- */

static uint32_t mix32(uint32_t a, uint32_t b)
{
	a ^= b * 0x9E3779B1u;
	a = (a << 13) | (a >> 19);
	a *= 0x85EBCA6Bu;
	a ^= a >> 15;
	a *= 0xC2B2AE35u;
	return a ^ (a >> 16);
}

static uint32_t tile_hash(const uint8_t *p)
{
	uint32_t h = 0x811C9DC5u;
	int i;
	for (i = 0; i < 16; i += 4) h = mix32(h, (uint32_t)p[i] | ((uint32_t)p[i + 1] << 8) | ((uint32_t)p[i + 2] << 16) | ((uint32_t)p[i + 3] << 24));
	return h;
}

static int fdiv8(int a) { return a >> 3; }   /* arithmetic shift: floor division by 8 */

static uint16_t rgb_of(int y, uint8_t idx)
{
	const uint16_t *pal = nesw_pal();
	unsigned deemp = XDBuf ? XDBuf[y * 256] : 0;
	if (deemp) return pal[256 + ((deemp & 7) << 6) + (idx & 0x3F)];
	return pal[idx];
}

static uint8_t post_px(uint8_t p1, uint8_t v)
{
	if (p1 & 1) v &= 0x30;
	if ((p1 >> 5) == 7) return (uint8_t)((v & 0x3f) | 0xc0);
	if (p1 & 0xE0) return (uint8_t)(v | 0x40);
	return (uint8_t)((v & 0x3f) | 0x80);
}

#if NESW_SCOUT
static int wrapdiff(int d, int wrap)
{
	d %= wrap;
	if (d < 0) d += wrap;
	if (d > wrap / 2) d -= wrap;
	return d;
}
#endif

/* ---- the frame description (shared by the live machine and the scout) ---------------------------------------- */

typedef struct { const uint8_t *bp[4]; const uint8_t *np[4]; int n; } DomKey;

/* Describe the background of the frame the machine I just drew: the dominant steady state (BG banks + nametable
 * mapping), the nametable ring with redraw codes, content hashes and write stamps, and per line the scroll. Fills D and
 * D->fr except the layer's gfx_id (the caller's identity rules). Returns 1 when a layer was described. */
static int describe(const NesInst *I, Desc *D, int margin, int settle)
{
	wk_frame *fr = D->fr;
	DomKey keys[4];
	int y, i, nk = 0, best = -1, bestn = 0, mw, mh;
	const DomKey *dom;
	wk_layer *L;

	memset(keys, 0, sizeof keys);
	D->rendering = 0;
	D->ndom = 0;
	for (y = 0; y < H; y++) {
		const NespLine *l = &I->line[y];
		int k;
		D->line_on[y] = 0;
		if (!(l->flags & NESPL_VALID)) continue;
		if (!(l->flags & NESPL_BGOFF)) D->rendering = 1;
		if ((l->flags & (NESPL_STABLE | NESPL_BGOFF)) != NESPL_STABLE) continue;
		if (((l->v0 >> 5) & 31) >= 30) continue;
		for (k = 0; k < nk; k++)
			if (!memcmp(l->bp, keys[k].bp, 8 * sizeof(void *))) break;
		if (k == nk) {
			if (nk == 4) continue;
			memcpy(keys[k].bp, l->bp, 8 * sizeof(void *));
			keys[k].n = 0;
			nk++;
		}
		keys[k].n++;
	}
	for (i = 0; i < nk; i++) if (keys[i].n > bestn) { bestn = keys[i].n; best = i; }
	memset(fr, 0, sizeof *fr);
	fr->frame = I->frame;
	fr->view_w = 256; fr->view_h = H; fr->margin = margin;
	fr->brightness = D->rendering ? 255 : 0;
	fr->nlayers = 0;
	D->mw = D->mh = 0;
	if (best < 0 || bestn < 8) return 0;

	dom = &keys[best];
	memcpy(D->bp, dom->bp, sizeof D->bp);
	memcpy(D->np, dom->np, sizeof D->np);
	D->distx = dom->np[0] != dom->np[1] || dom->np[2] != dom->np[3];
	D->disty = dom->np[0] != dom->np[2] || dom->np[1] != dom->np[3];
	mw = D->mw = D->distx ? 64 : 32;
	mh = D->mh = D->disty ? 60 : 30;

	/* tile content hashes of the BG pattern table in force, for the names the ring uses */
	{
		int rx, ry, name;
		memset(D->used, 0, sizeof D->used);
		for (ry = 0; ry < mh; ry++)
			for (rx = 0; rx < mw; rx++) {
				const uint8_t *np = dom->np[((D->disty ? ry / 30 : 0) << 1) | (D->distx ? rx / 32 : 0)];
				D->used[np[(ry % 30) * 32 + (rx & 31)]] = 1;
			}
		for (name = 0; name < 256; name++)
			if (D->used[name]) D->gh[name] = tile_hash(dom->bp[name >> 6] + (name & 63) * 16);
	}
	/* bank identity: offsets into CHR ROM (stable across machines), else the address */
	{
		int csz = 0;
		const uint8_t *cb = I->chr_base(&csz);
		uint32_t sig = 0;
		for (i = 0; i < 4; i++) {
			long off = cb ? (long)(dom->bp[i] - cb) : 0;
			if (!cb || off < 0 || off >= csz) off = (long)((uintptr_t) dom->bp[i] & 0xFFFFF);
			sig = mix32(sig, (uint32_t) off);
		}
		D->sig = mix32(sig, (uint32_t) (mw * 64 + mh));
	}

	/* the ring */
	{
		int rx, ry;
		const uint32_t settle_n = (uint32_t) (settle ? settle - 1 : 2);
		for (ry = 0; ry < mh; ry++)
			for (rx = 0; rx < mw; rx++) {
				const uint8_t *np = dom->np[((D->disty ? ry / 30 : 0) << 1) | (D->distx ? rx / 32 : 0)];
				uint32_t ts, as, ft, fa, f, cc, a, k = (uint32_t)(ry * mw + rx);
				int mid, cx = rx & 31, cy = ry % 30, id = I->ntid(np), ai = 0x3c0 + (cy >> 2) * 8 + (cx >> 2), name;
				ts = I->ntstamp[id][cy * 32 + cx];
				as = I->ntstamp[id][ai];
				ft = ts >> 1; fa = as >> 1;
				f = ft > fa ? ft : fa;
				mid = ((ts & 1) && ft == I->frame) || ((as & 1) && fa == I->frame);
				name = np[cy * 32 + cx];
				a = np[ai];
				cc = (a >> ((cx & 2) + ((cy & 2) << 1))) & 3;
				D->code[k] = (uint32_t) name | (cc << 8);
				D->code2[k] = D->code[k];
				D->hash[k] = mix32(D->code[k] * 0x10001u + 0x1234567u, D->gh[name]);
				/* the trust stamp is the TILE's write: an attribute byte covers four columns and is often written with the first
				   of them, before the other three are streamed, so a fresh attribute says nothing about the tile under it. The
				   settle test below uses the later of the two (a palette written a frame after the tile). */
				D->stamp[k] = ft;
				if (!f || mid) D->code2[k] = WK_CODE_NOREC;   /* never written / written during the picture: not recorded by the map */
				if (mid || (f && I->frame - f < settle_n)) {
					/* settling (just written, its palette may follow next frame): a changing hash keeps it out of the verified
					   history, and a stamp 'in the future' stops every stamp chain at it, also one that would start from it as
					   the edge cell (a stamp of 0 there made every older cell beyond look newer than the edge); widekit's
					   stream detector still sees it as fresh. */
					D->hash[k] ^= 0x7F4A7C15u * (I->frame | 1u);
					D->stamp[k] = 0xFFFFFFFFu;
					D->code2[k] = WK_CODE_NOREC;   /* nor recorded by the map until it has settled */
				}
			}
	}

	L = &fr->layer[0];
	L->active = 1;
	L->role = WK_ROLE_AUTO;
	L->priority = 0;
	L->map_w = mw; L->map_h = mh; L->tile_px = 8;
	L->hash = D->hash; L->code = D->code2; L->stamp = D->stamp;
	L->line_on = D->line_on;
	for (y = 0; y < H; y++) {
		const NespLine *l = &I->line[y];
		int on = (l->flags & NESPL_VALID) && (l->flags & (NESPL_STABLE | NESPL_BGOFF)) == NESPL_STABLE &&
		         ((l->v0 >> 5) & 31) < 30 && !memcmp(l->bp, dom->bp, 8 * sizeof(void *));
		uint32_t v = l->v0;
		D->line_on[y] = (uint8_t) on;
		if (on) D->ndom++;
		L->scroll_x[y] = (D->distx ? (int)((v >> 10) & 1) * 256 : 0) + (int)(v & 31) * 8 + l->x0;
		L->row_y[y] = (D->disty ? (int)((v >> 11) & 1) * 240 : 0) + (int)((v >> 5) & 31) * 8 + (int)((v >> 12) & 7);
	}
	/* the column of the ring under the masked left 8 px: games with the mask rewrite it (the seam) while its old content is
	   still partly on screen, so what the ring holds there is not the world column it appears as: the learned map must not
	   record it (widekit: code WK_CODE_NOREC) */
	{
		uint64_t done = 0;
		for (y = 0; y < H; y++) {
			int lx, r2;
			if (!D->line_on[y] || (I->line[y].p1 & 2)) continue;
			lx = (int)(((L->scroll_x[y] % (mw * 8)) + mw * 8) % (mw * 8)) >> 3;
			if (lx >= mw || ((done >> (lx & 63)) & 1)) continue;
			done |= (uint64_t)1 << (lx & 63);
			for (r2 = 0; r2 < mh; r2++) D->code2[(size_t) r2 * mw + lx] = WK_CODE_NOREC;
		}
	}
	/* off lines follow the previous line so they do not look like a scroll difference */
	for (y = 0; y < H; y++) {
		if (!D->line_on[y]) {
			int j = y - 1;
			while (j >= 0 && !D->line_on[j]) j--;
			if (j < 0) { j = y + 1; while (j < H && !D->line_on[j]) j++; }
			if (j >= 0 && j < H) { L->scroll_x[y] = L->scroll_x[j]; L->row_y[y] = L->row_y[j]; }
		}
	}
	fr->nlayers = 1;
	return 1;
}

/* ---- the scout ---------------------------------------------------------------------------------------------- */

#if NESW_SCOUT
/* the scout's copy of the core and host (scoutcore.o: every global renamed nesscout_*) */
extern int nesscout_nes_host_init(void);
extern void nesscout_nes_host_shutdown(void);
extern int nesscout_nes_host_load_mem(const uint8_t *data, size_t n, const char *name, char *err, size_t errlen);
extern void nesscout_nes_host_unload(void);
extern int nesscout_nes_host_state_load_mem(const uint8_t *buf, size_t n);
extern void nesscout_nes_host_run_frame(void);
extern void nesscout_nes_host_set_pad(int port, unsigned mask);
extern void nesscout_nes_host_set_render(int on);
extern int nesscout_nes_host_audio_take(int16_t *dst, int max_frames);
extern int nesscout_nesp_on, nesscout_nesp_margin;
extern uint32_t nesscout_nesp_frame;
extern NespLine nesscout_nesp_line[240];
extern uint32_t nesscout_nesp_ntstamp[NESP_NTP][1024];
extern int nesscout_nesp_ntid(const uint8_t *page);
extern const uint8_t *nesscout_nesp_chr_base(int *size);
extern void nesscout_nesp_clear_stamps(void);
#endif

typedef struct {
	int enabled, available, inited, loaded;
	int ahead_max, frames_per_step, resync;     /* options */
	uint8_t *rom; size_t rom_n; char name[64];
	uint8_t *snap; size_t snap_cap, snap_n;
	int need_load;                              /* a new snapshot waits for the scout */
	int session;                                /* the scout runs from the last snapshot */
	uint32_t base_live;                         /* live frame counter at the snapshot */
	uint32_t epoch;                             /* the live epoch the snapshot belongs to */
	int frames;                                 /* scout frames run since the snapshot */
	unsigned pad;
	int y0; int32_t live_wx0, live_wr0, live_sx0, live_ry0;   /* link to the live world at the snapshot */
	uint8_t hud[H];                             /* status bar lines of the live picture at the snapshot */
	uint32_t retry_at;                          /* after a stop: no new snapshot before this live frame */
	int linked; int32_t shift_x, shift_r;
	int wpx, hpx;
	Desc D;
	wk_layers *L;
	wk_out *out, *out2;
	uint64_t (*clock_us)(void);
	nes_scout_stats st;
} Scout;

static Scout S;

static void scout_free(void)
{
	free(S.rom); S.rom = NULL; S.rom_n = 0;
	free(S.snap); S.snap = NULL; S.snap_cap = 0;
	S.session = S.need_load = 0;
}

void nesw_on_rom(const uint8_t *data, size_t n, const char *name)
{
	free(S.rom); S.rom = NULL; S.rom_n = 0; S.loaded = 0;
	if (!S.enabled || !NESW_SCOUT || !data || !n) return;
	S.rom = (uint8_t *) malloc(n);
	if (!S.rom) return;
	memcpy(S.rom, data, n);
	S.rom_n = n;
	snprintf(S.name, sizeof S.name, "%s", name ? name : "");
}

#if NESW_SCOUT
static int scout_load(void)
{
	char err[128];
	if (S.loaded) return 1;
	if (!S.rom || !nesw_is_cart()) return 0;
	if (!S.inited) { if (nesscout_nes_host_init()) return 0; S.inited = 1; }
	nesscout_nesp_on = 1;      /* the scout records lines and nametable writes, renders no margins */
	nesscout_nesp_margin = 0;
	if (nesscout_nes_host_load_mem(S.rom, S.rom_n, S.name, err, sizeof err)) return 0;
	nesscout_nes_host_set_render(0);   /* no RGB conversion: the PPU still draws its lines, which the recorder needs */
	if (!S.D.fr) S.D.fr = (wk_frame *) calloc(1, sizeof(wk_frame));
	if (!S.out) S.out = (wk_out *) calloc(1, sizeof(wk_out));
	if (!S.out2) S.out2 = (wk_out *) calloc(1, sizeof(wk_out));
	if (!S.L) { wk_layers_cfg lc; wk_layers_cfg_default(&lc); S.L = wk_layers_create(&lc); }
	if (!S.D.fr || !S.out || !S.out2 || !S.L) return 0;
	S.loaded = 1;
	return 1;
}
#endif

/* main thread, end of nes_host_wide_frame: take a new snapshot when the scout has nothing useful to do */
static void scout_live_frame(const Desc *D, const wk_out *out)
{
#if NESW_SCOUT
	int y, ahead;
	if (!S.enabled || !W.M || !D->fr->nlayers || !nesw_is_cart()) { S.session = 0; return; }
	if (!S.loaded && !scout_load()) { S.available = 0; return; }
	S.available = 1;
	ahead = S.frames - (int)(D->fr->frame - S.base_live);
	S.st.ahead = ahead < 0 ? 0 : ahead;
	if (S.session && !out->scene_event && out->epoch == S.epoch && ahead > 0 &&
	    (int)(D->fr->frame - S.base_live) < S.resync)
		return;
	if (out->scene_event || out->dir[0] == 0) { S.session = 0; return; }   /* nothing to look ahead into */
	if ((int32_t)(D->fr->frame - S.retry_at) < 0) return;
	/* the link: a line on in this frame, coherent, not a status bar */
	S.y0 = -1;
	for (y = 0; y < H; y++)
		if (D->line_on[y] && out->coherent[0][y] && !out->line[y].hud) { S.y0 = y; break; }
	if (S.y0 < 0) { S.session = 0; return; }
	{
		size_t need = nes_host_state_size();
		if (need > S.snap_cap) {
			uint8_t *p = (uint8_t *) realloc(S.snap, need);
			if (!p) { S.session = 0; return; }
			S.snap = p; S.snap_cap = need;
		}
		S.snap_n = nes_host_state_save_mem(S.snap, S.snap_cap);
		if (!S.snap_n) { S.session = 0; return; }
	}
	for (y = 0; y < H; y++) S.hud[y] = out->line[y].hud;
	S.live_wx0 = out->world_x[0][S.y0];
	S.live_wr0 = out->world_row[0][S.y0];
	S.live_sx0 = D->fr->layer[0].scroll_x[S.y0];
	S.live_ry0 = D->fr->layer[0].row_y[S.y0];
	S.wpx = D->mw * 8; S.hpx = D->mh * 8;
	S.epoch = out->epoch;
	S.base_live = D->fr->frame;
	S.frames = 0;
	S.linked = 0;
	S.pad = nesw_pad(0);
	if (out->dir[0] > 0) S.pad = (S.pad & ~(unsigned) NES_LEFT) | NES_RIGHT;
	else if (out->dir[0] < 0) S.pad = (S.pad & ~(unsigned) NES_RIGHT) | NES_LEFT;
	S.need_load = 1;
	S.session = 1;
	S.st.snapshots++;
#else
	(void) D; (void) out;
#endif
}

#if NESW_SCOUT
/* one scout frame: run, describe, check that its graphics are the live game's, link its world to the live world, record */
static int scout_one(void)
{
	NesInst si;
	wk_layer *SL;
	int y, name;
	int16_t junk[2048];
	nesscout_nes_host_set_pad(0, S.pad);
	nesscout_nes_host_run_frame();
	while (nesscout_nes_host_audio_take(junk, 1024) > 0) {}
	S.frames++;
	S.st.frames++;
	si.line = nesscout_nesp_line; si.ntstamp = nesscout_nesp_ntstamp; si.ntid = nesscout_nesp_ntid;
	si.chr_base = nesscout_nesp_chr_base; si.frame = nesscout_nesp_frame;
	if (!describe(&si, &S.D, W.margin, W.settle)) return 0;
	for (y = 0; y < H; y++) if (S.hud[y]) S.D.line_on[y] = 0;   /* the live picture's status bar is not level */
	/* the same graphics as the live picture: same ring shape, and the tile contents of the names it uses compared with the
	   live pattern table: a different tile set stops the run; a few differing tiles (a bank animation in another phase)
	   are not recorded */
	if (S.D.mw * 8 != S.wpx || S.D.mh * 8 != S.hpx) return 0;
	{
		int nused = 0, ndiff = 0, k;
		uint8_t diff[256];
		for (name = 0; name < 256; name++) {
			diff[name] = 0;
			if (!S.D.used[name]) continue;
			nused++;
			if (S.D.gh[name] != tile_hash(W.dom_bp[name >> 6] + (name & 63) * 16)) { diff[name] = 1; ndiff++; }
		}
		if (ndiff * 10 >= nused) return 0;
		if (ndiff)
			for (k = 0; k < S.D.mw * S.D.mh; k++)
				if (diff[S.D.code[k] & 255]) S.D.code2[k] = WK_CODE_NOREC;
	}
	SL = &S.D.fr->layer[0];
	SL->gfx_id = W.layer_gfx;
	wk_layers_update(S.L, S.D.fr, S.out);
	if (S.frames > 1 && S.out->scene_event) return 0;
	if (!S.linked) {
		/* the scout's first frame follows the live frame of the snapshot: link the same line through the scroll */
		if (!S.D.line_on[S.y0] || !S.out->coherent[0][S.y0]) return 0;
		S.shift_x = S.live_wx0 + wrapdiff(SL->scroll_x[S.y0] - S.live_sx0, S.wpx) - S.out->world_x[0][S.y0];
		S.shift_r = S.live_wr0 + wrapdiff(SL->row_y[S.y0] - S.live_ry0, S.hpx) - S.out->world_row[0][S.y0];
		S.linked = 1;
	}
	if (S.frames == 1) return 1;     /* the first frame links; its lines have no motion yet to tell a status bar */
	memcpy(S.out2, S.out, sizeof *S.out2);
	for (y = 0; y < H; y++) {
		S.out2->world_x[0][y] += S.shift_x;
		S.out2->world_row[0][y] += S.shift_r;
	}
	S.out2->epoch = S.epoch;
	S.out2->scene_event = 0;
	wk_map_record_scout(W.M, S.D.fr, S.out2);
	return 1;
}
#endif

int nes_host_scout_step(int max_frames, int max_us)
{
#if NESW_SCOUT
	int n = 0;
	uint64_t t0 = S.clock_us ? S.clock_us() : 0;
	if (!S.enabled || !S.session || !S.loaded || !W.M) return 0;
	if (S.need_load) {
		if (nesscout_nes_host_state_load_mem(S.snap, S.snap_n)) { S.session = 0; return 0; }
		{
			/* the loaded nametables are the live game's: count every entry as written long ago */
			int p, e;
			for (p = 0; p < NESP_NTP; p++) for (e = 0; e < 1024; e++) nesscout_nesp_ntstamp[p][e] = 2u;
		}
		wk_layers_reset(S.L);
		S.need_load = 0;
	}
	while (n < max_frames) {
		int ahead = S.frames - (int)(W.D.fr->frame - S.base_live);
		if (ahead >= S.ahead_max) break;                      /* far enough: wait for the live game */
		if (!scout_one()) { S.session = 0; S.st.stops++; S.retry_at = W.D.fr->frame + 30; break; }
		n++;
		if (max_us > 0 && S.clock_us && S.clock_us() - t0 >= (uint64_t) max_us) break;
	}
	return n;
#else
	(void) max_frames; (void) max_us;
	return 0;
#endif
}

int nes_host_scout_available(void) { return NESW_SCOUT; }

void nes_host_scout_enable(int on)
{
	S.enabled = on != 0 && NESW_SCOUT;
	if (!S.ahead_max) S.ahead_max = 90;
	if (!S.resync) S.resync = 240;
	if (!S.enabled) S.session = 0;
}

int nes_host_scout_option(const char *name, int v)
{
	if (!name) return 1;
	if (!strcmp(name, "ahead")) { S.ahead_max = v < 1 ? 1 : v > 600 ? 600 : v; return 0; }
	if (!strcmp(name, "resync")) { S.resync = v < 1 ? 1 : v; return 0; }
	return 1;
}

void nes_host_scout_set_clock(uint64_t (*now_us)(void)) { S.clock_us = now_us; }

void nes_host_scout_stats(nes_scout_stats *st)
{
	wk_map_stats ms;
	if (!st) return;
	*st = S.st;
	st->available = NESW_SCOUT && S.available;
	st->session = S.session;
	if (W.M) {
		wk_map_stats_get(W.M, &ms);
		st->cells = ms.scout_cells; st->confirmed = ms.scout_confirmed; st->contradicted = ms.scout_contradicted;
	}
}

/* ---- life cycle ---------------------------------------------------------------------------------------------- */

static void free_objects(void)
{
	if (W.C) wk_compose_destroy(W.C);
	if (W.L) wk_layers_destroy(W.L);
	W.C = NULL; W.L = NULL;
	free(W.wide); free(W.cand[0]); free(W.cand[1]); free(W.inner[0]); free(W.inner[1]); free(W.prov);
	W.wide = NULL; W.cand[0] = W.cand[1] = NULL; W.inner[0] = W.inner[1] = NULL; W.prov = NULL;
	W.margin = 0;
}

static void profile_lookup(void)
{
	W.prof = NULL;
	if (W.db && W.have_game) W.prof = wk_profile_find(W.db, W.crc, nesw_game_title());
	if (W.L) wk_layers_set_profile(W.L, W.prof);
}

static int make_objects(void)
{
	wk_layers_cfg lc;
	wk_compose_cfg cc;
	int m = W.margin_req;
	if (W.L && W.C && W.margin == m) return 1;
	free_objects();
	if (!W.D.fr) W.D.fr = (wk_frame *) calloc(1, sizeof(wk_frame));
	if (!W.out) W.out = (wk_out *) calloc(1, sizeof(wk_out));
	if (!W.D.fr || !W.out) return 0;
	wk_layers_cfg_default(&lc);
	W.L = wk_layers_create(&lc);
	wk_compose_cfg_default(&cc);
	cc.period = W.period;
	cc.fade_px = W.fade_px > 0 ? W.fade_px : 8;
	if (W.appear > 0) cc.appear_frames = W.appear;
	W.C = wk_compose_create(&cc, 256, H, m);
	W.wide = (uint16_t *) calloc((size_t)(256 + 2 * m) * H, 2);
	W.cand[0] = (uint16_t *) calloc((size_t) H * m, 2);
	W.cand[1] = (uint16_t *) calloc((size_t) H * m, 2);
	W.inner[0] = (uint16_t *) calloc((size_t) H * WK_MAX_INNER, 2);
	W.inner[1] = (uint16_t *) calloc((size_t) H * WK_MAX_INNER, 2);
	W.prov = (uint8_t *) calloc((size_t)(256 + 2 * m) * H, 1);
	if (!W.L || !W.C || !W.wide || !W.cand[0] || !W.cand[1] || !W.inner[0] || !W.inner[1] || !W.prov) { free_objects(); return 0; }
	W.margin = m;
	W.have_pal_prev = 0;
	W.have_gid = 0;
	profile_lookup();
	return 1;
}

static void make_map(void)
{
	wk_map_cfg mc;
	if (W.M || !W.have_game) return;
	wk_map_cfg_default(&mc);
	mc.max_cells = 32768;
	W.M = wk_map_create(&mc, W.crc);
	W.map_cells_saved = 0;
}

static void apply(void)
{
	int on = W.mode != 0 && W.margin_req > 0 && W.have_game;
	if (on && !make_objects()) on = 0;
	if (on) make_map();
	W.active = on;
	nesp_on = on;
	nesp_margin = on ? W.margin_req : 0;
	if (on) nesp_clear_stamps();
}

void nesw_on_load(void)
{
	W.have_game = 1;
	W.crc = nesw_game_crc();
	W.hscrolled = 0; W.still_frames = 0; memset(W.prev_on, 0, sizeof W.prev_on);
	if (W.M) { wk_map_destroy(W.M); W.M = NULL; }
	if (W.L) wk_layers_reset(W.L);
	if (W.C) wk_compose_reset(W.C);
	W.have_gid = 0; W.have_pal_prev = 0; memset(W.gh_prev_ok, 0, sizeof W.gh_prev_ok);
	S.session = 0; S.need_load = 0;
#if NESW_SCOUT
	if (S.loaded) { nesscout_nes_host_unload(); S.loaded = 0; }
	if (S.inited) { nesscout_nesp_on = 1; nesscout_nesp_margin = 0; }
#endif
	memset(&S.st, 0, sizeof S.st);
	profile_lookup();
	apply();
}

void nesw_on_unload(void)
{
	W.have_game = 0;
	if (W.M) { wk_map_destroy(W.M); W.M = NULL; }
	W.active = 0; nesp_on = 0; nesp_margin = 0;
	S.session = 0;
#if NESW_SCOUT
	if (S.loaded) { nesscout_nes_host_unload(); S.loaded = 0; }
#endif
}

/* a state load, reset or disk change: nametable contents changed without write stamps */
void nesw_on_discontinuity(void)
{
	S.session = 0;
	if (!W.active) return;
	nesp_clear_stamps();
	if (W.L) wk_layers_reset(W.L);
	if (W.C) wk_compose_reset(W.C);
	W.have_gid = 0; W.have_pal_prev = 0; memset(W.gh_prev_ok, 0, sizeof W.gh_prev_ok);
}

void nesw_on_shutdown(void)
{
	nesw_on_unload();
	free_objects();
	if (W.db) { wk_profile_free(W.db); W.db = NULL; }
	free(W.D.fr); free(W.out);
	W.D.fr = NULL; W.out = NULL;
#if NESW_SCOUT
	if (S.inited) { nesscout_nes_host_shutdown(); S.inited = 0; }
	if (S.L) wk_layers_destroy(S.L);
	free(S.D.fr); free(S.out); free(S.out2);
	S.L = NULL; S.D.fr = NULL; S.out = S.out2 = NULL;
#endif
	scout_free();
}

/* ---- public control ----------------------------------------------------------------------------------------- */

void nes_host_wide_enable(int mode) { W.mode = mode != 0; apply(); }

void nes_host_wide_margin(int px)
{
	if (px < 0) px = 0;
	if (px > NES_WIDE_MAX_MARGIN) px = NES_WIDE_MAX_MARGIN;
	W.margin_req = px;
	apply();
}

int nes_host_wide_active(void) { return W.active; }

int nes_host_wide_option(const char *name, int v)
{
	if (!name) return 1;
	if (!strcmp(name, "period")) { W.period = v != 0; if (W.C) { free_objects(); apply(); } return 0; }
	if (!strcmp(name, "debug")) { W.debug = v != 0; return 0; }
	if (!strcmp(name, "fade_px")) { W.fade_px = v; if (W.C) { free_objects(); apply(); } return 0; }
	if (!strcmp(name, "settle_frames")) { W.settle = (v < 0 ? 0 : v > 16 ? 16 : v) + 1; return 0; }
	if (!strcmp(name, "appear_frames")) { W.appear = v; if (W.C) { free_objects(); apply(); } return 0; }
	return 1;
}

int nes_host_wide_profiles_load(const char *text, size_t n, char *msgs, size_t cap)
{
	int problems = 0;
	if (W.db) wk_profile_free(W.db);
	W.db = wk_profile_parse(text, n, msgs, cap, &problems);
	profile_lookup();
	return problems;
}

/* ---- the learned map's file image -------------------------------------------------------------------------- */

uint32_t nes_host_wide_map_key(void) { return W.have_game ? nesw_game_crc() : 0; }

size_t nes_host_wide_map_save(uint8_t *buf, size_t cap)
{
	size_t need;
	wk_map_stats ms;
	if (!W.M) return 0;
	need = wk_map_serialize_max(W.M);
	if (!buf) return need;
	if (cap < need) return 0;
	need = wk_map_serialize(W.M, buf, cap);
	if (need) { wk_map_stats_get(W.M, &ms); W.map_cells_saved = (uint32_t) ms.cells; }
	return need;
}

int nes_host_wide_map_load(const uint8_t *buf, size_t n)
{
	int r;
	wk_map_stats ms;
	if (!W.have_game) return 1;
	make_map();
	if (!W.M) return 1;
	r = wk_map_deserialize(W.M, buf, n);
	wk_map_stats_get(W.M, &ms);
	W.map_cells_saved = (uint32_t) ms.cells;
	return r;
}

int nes_host_wide_map_dirty(void)
{
	wk_map_stats ms;
	if (!W.M) return 0;
	wk_map_stats_get(W.M, &ms);
	return (uint32_t) ms.cells != W.map_cells_saved;
}

/* ---- the frame ---------------------------------------------------------------------------------------------- */

/* draw one background pixel of a learned-map tile (code = name | palette << 8) with the current banks and palette */
static uint8_t cell_px(uint32_t code, int xin, int fy)
{
	int name = (int)(code & 255), cc = (int)((code >> 8) & 3);
	const uint8_t *G = W.dom_bp[name >> 6] + (name & 63) * 16 + fy;
	int b = 7 - xin, idx = (((G[8] >> b) & 1) << 1) | ((G[0] >> b) & 1);
	return (uint8_t)(idx ? PALRAM[(cc << 2) | idx] : (PALRAM[cc << 2] | 0x40));
}

static void fill_strips(const wk_frame *fr, wk_out *out)
{
	const int M = W.margin;
	int y, s, d;
	for (y = 0; y < H; y++) {
		const NespLine *nl = &nesp_line[y];
		wk_line_out *lo = &out->line[y];
		int drawn = (nl->flags & NESPL_DRAWN) != 0;
		for (s = 0; s < 2; s++) {
			uint16_t *dst = W.cand[s] + (size_t) y * M;
			const uint8_t *src = s == 0 ? nesp_wl[y] : nesp_wr[y];
			int real = lo->real[s][0], ring = lo->ring[s][0];
			wk_geom g;
			int have_geom, mpart;
			if (!drawn) {
				if (real) {
					lo->real[s][0] = 0; lo->ring[s][0] = 0;
					real = 0;
				}
				continue;
			}
			if (real > M) real = M;
			if (ring > real) ring = real;
			have_geom = out->coherent[0][y] != 0;
			if (have_geom) wk_line_geom(fr, out, 0, y, &g);
			/* a ring as wide as the screen has no history: with the left 8 px masked the column under the mask is the one the
			   game rewrites (the seam), so the part of it left of the screen comes from the learned map or not at all */
			mpart = (s == 0 && have_geom && W.st.ring_w == 32 && !(nl->p1 & 2)) ? g.part[0] : 0;
			for (d = 0; d < real; d++) {
				uint8_t px = src[d];
				if (d < mpart) {
					wk_cell c0;
					if (wk_map_margin_cell(W.M, fr, out, 0, y, s, 0, &c0)) {
						px = post_px(nl->p1, cell_px(c0.code, (out->world_x[0][y] - 1 - d) & 7, out->world_row[0][y] & 7));
					} else {
						lo->real[s][0] = (uint16_t) d; lo->ring[s][0] = (uint16_t) (ring < d ? ring : d);
						break;
					}
				} else if (have_geom && d >= ring) {
					int wx = out->world_x[0][y], idx, xw;
					wk_cell cell;
					idx = s == 0 ? (int)(g.tile_first - fdiv8(wx - 1 - d)) : (int)(fdiv8(wx + 256 + d) - (g.tile_first + g.nvis - 1));
					if (wk_map_margin_cell(W.M, fr, out, 0, y, s, idx, &cell)) {
						int fy = out->world_row[0][y] & 7;
						xw = s == 0 ? wx - 1 - d : wx + 256 + d;
						px = post_px(nl->p1, cell_px(cell.code, xw & 7, fy));
					} else {
						/* widekit said real but the map has no cell here: never show the raw ring in its place */
						lo->real[s][0] = (uint16_t) d;
						break;
					}
				}
				dst[d] = rgb_of(y, px);
			}
		}
		wk_out_refresh_line(out, y);
	}
}

static void build_inner(void)
{
	const uint16_t *fb = nesw_fb();
	int y, e;
	for (y = 0; y < H; y++)
		for (e = 0; e < WK_MAX_INNER; e++) {
			W.inner[0][(size_t) y * WK_MAX_INNER + e] = fb[y * 256 + e];
			W.inner[1][(size_t) y * WK_MAX_INNER + e] = fb[y * 256 + 255 - e];
		}
}

int nes_host_wide_frame(const uint16_t **rgb, int *width, int *pitch_bytes, nes_wide_stats *st)
{
	int y, i;
	Desc *D = &W.D;
	wk_frame *fr;
	wk_out *out = W.out;
	int M = W.margin;
	NesInst li;

	if (!W.active || !nesw_game_loaded() || !nesw_render_on() || !GameInfo || GameInfo->type == GIT_NSF) return 0;

	/* 1. the frame description */
	li.line = nesp_line; li.ntstamp = nesp_ntstamp; li.ntid = nesp_ntid; li.chr_base = nesp_chr_base; li.frame = nesp_frame;
	describe(&li, D, M, W.settle);
	fr = D->fr;
	{
		int delta = 0;
		if (W.have_pal_prev) for (i = 0; i < 32; i++) if (W.pal_prev[i] != PALRAM[i]) delta++;
		memcpy(W.pal_prev, PALRAM, 32);
		W.have_pal_prev = 1;
		fr->palette_delta = delta;
		fr->palette_size = 32;
	}
	memset(&W.st, 0, sizeof W.st);
	W.st.frame = nesp_frame;
	W.st.margin = M;
	W.st.width = 256 + 2 * M;
	for (y = 0; y < H; y++) if (nesp_line[y].flags & NESPL_BLOCK) W.st.lines_blocked++;

	if (fr->nlayers) {
		int name, nused = 0, same = 0;
		wk_layer *L = &fr->layer[0];
		const int mw = D->mw;
		for (i = 0; i < 4; i++) W.dom_bp[i] = D->bp[i];
		W.st.ring_w = D->mw; W.st.ring_h = D->mh;
		/* gfx identity: bank offsets, kept through cosmetic animation (a few tiles changing), new for a tile set swap */
		for (name = 0; name < 256; name++) {
			if (!D->used[name]) continue;
			nused++;
			if (W.gh_prev_ok[name] && W.gh_prev[name] == D->gh[name]) same++;
		}
		if (!W.have_gid || same * 10 < nused * 9) {
			uint32_t g = D->sig;
			for (name = 0; name < 256; name++) if (D->used[name]) g = mix32(g, D->gh[name]);
			W.gid = g; W.have_gid = 1;
		}
		W.prev_sig = D->sig;
		for (name = 0; name < 256; name++) if (D->used[name]) { W.gh_prev[name] = D->gh[name]; W.gh_prev_ok[name] = 1; }
		W.layer_gfx = L->gfx_id = W.gid ^ (uint32_t)(D->mw * 977 + D->mh);
		/* a sideways scroll anywhere ends the uniform-row backdrop fill for this game */
		for (y = 0; y < H; y++) {
			if (D->line_on[y] && W.prev_on[y]) {
				/* a scroll step (a few px, either way, modulo the ring); a big jump is a screen rebuild, not a scroll */
				int dx = ((L->scroll_x[y] - W.prev_sx[y]) % (mw * 8) + mw * 8) % (mw * 8);
				if ((dx >= 1 && dx <= 24) || (dx >= mw * 8 - 24 && dx < mw * 8)) W.hscrolled = 1;
			}
			W.prev_on[y] = D->line_on[y];
			W.prev_sx[y] = L->scroll_x[y];
		}
		if (W.still_frames < 1000000) W.still_frames++;
	}

	/* 2. widekit */
	wk_layers_update(W.L, fr, out);
	if (W.M && fr->nlayers) {
		wk_map_update(W.M, fr, out);
		wk_map_serve(W.M, fr, out);
	}
	W.st.scene_event = out->scene_event;
	W.st.epoch = out->epoch;
	W.st.hs_checked = out->hs_checked;
	W.st.hs_missed = out->hs_missed;
	if (fr->nlayers) W.st.kind = out->kind[0];
	if (W.M) {
		wk_map_stats ms;
		wk_map_stats_get(W.M, &ms);
		W.st.map_cells = ms.cells; W.st.map_rooms = ms.rooms; W.st.map_bound = ms.bound; W.st.map_served = (int) ms.served_tiles;
	}

	/* 3. candidates and the compose */
	{
		wk_compose_in ci;
		memset(&ci, 0, sizeof ci);
		if (fr->nlayers) fill_strips(fr, out);
		for (y = 0; y < H; y++)   /* the backdrop as the line shows it: entry 0 through the grayscale / emphasis pass */
			W.bg[y] = rgb_of(y, post_px(nesp_line[y].p1, nesp_line[y].bd | 0x40));
		ci.ana = out;
		ci.centre = nesw_fb(); ci.centre_stride = 256;
		ci.bg = W.bg;
		if (fr->nlayers) {
			ci.cand[0][0] = W.cand[0]; ci.cand[0][1] = W.cand[1];
			ci.order[0] = 0; ci.norder = 1;
			if (W.period) {
				build_inner();
				ci.inner[0][0] = W.inner[0]; ci.inner[0][1] = W.inner[1];
				ci.inner_n = WK_MAX_INNER;
			}
		}
		ci.out = W.wide; ci.out_stride = 256 + 2 * M;
		ci.prov = W.debug ? W.prov : NULL;
		wk_compose_frame(W.C, &ci);
	}

	/* 4. left mask fill: the 8 background pixels the hardware hides at x = 0..7, where the margin next to them is real.
	   With a 64 column ring they are the ring's own pixels. With a 32 column ring the leftmost column's slot is the seam
	   the game rewrites for the right edge: its part under the mask comes from the learned map (the world column as it was
	   seen before) or the line is left masked; the next column's part is the ring's (it is on screen). Sprite pixels stay. */
	if (fr->nlayers) {
		const uint8_t *sprm = nesw_sprm();
		for (y = 0; y < H; y++) {
			const NespLine *l = &nesp_line[y];
			uint8_t px8[8];
			int ok = 1;
			if (!(l->flags & NESPL_DRAWN) || (l->p1 & 2)) continue;
			if (out->line[y].hud || out->line[y].real[0][0] < 8 || !out->coherent[0][y]) continue;
			memcpy(px8, nesp_wm[y], 8);
			if (D->mw == 32) {
				int wx = out->world_x[0][y], first = 8 - (wx & 7);   /* x < first: the leftmost (seam) column */
				wk_cell c0;
				if (!wk_map_margin_cell(W.M, fr, out, 0, y, 0, 0, &c0)) ok = 0;
				for (i = 0; ok && i < first; i++)
					px8[i] = post_px(l->p1, cell_px(c0.code, (wx + i) & 7, out->world_row[0][y] & 7));
			}
			if (!ok) continue;
			for (i = 0; i < 8; i++)
				if (!((sprm[y] >> i) & 1)) W.wide[(size_t) y * (256 + 2 * M) + M + i] = rgb_of(y, px8[i]);
		}
	}

	/* 4b. uniform-row backdrop: on a game that has never scrolled sideways and whose ring is one screen wide, a line whose
	   background pixels are all the backdrop colour across the whole ring (sky, black bands) continues as flat backdrop
	   instead of the soft edge. It is the backdrop colour, never a tile; not counted as real. It starts after 120 frames
	   without a sideways scroll (a scroller that has not moved yet would show backdrop where its level continues). */
	if (fr->nlayers && !W.hscrolled && W.still_frames >= 120 && D->mw == 32) {
		const int OW = 256 + 2 * M;
		for (y = 0; y < H; y++) {
			const NespLine *l = &nesp_line[y];
			const uint8_t *np;
			uint32_t v = l->v0;
			int cx, uni = 1, fy, cy;
			if (!D->line_on[y] || out->line[y].hud) continue;
			np = l->np[(v >> 10) & 3];
			cy = (int)((v >> 5) & 31); fy = (int)((v >> 12) & 7);
			for (cx = 0; uni && cx < 32; cx++) {
				int name = np[cy * 32 + cx];
				const uint8_t *G = l->bp[name >> 6] + (name & 63) * 16 + fy;
				uni = (G[0] | G[8]) == 0;
			}
			if (!uni) continue;
			for (i = 0; i < 2; i++) {
				int r = out->line[y].real[i][0], d;
				if (r > M) r = M;
				for (d = r; d < M; d++) {
					int x = i == 0 ? M - 1 - d : M + 256 + d;
					W.wide[(size_t) y * OW + x] = W.bg[y];
					if (W.debug) W.prov[(size_t) y * OW + x] = WK_PROV_BACKDROP;
				}
			}
			W.st.lines_backdrop++;
		}
	}

	/* 4c. status bars and other fixed lines (not the scrolling field): original width, unless the line's own row is ONE
	   colour across the whole nametable ring (bar background, divider): then that colour continues to the edge. */
	if (fr->nlayers) {
		const int OW = 256 + 2 * M;
		for (y = 0; y < H; y++) {
			const NespLine *l = &nesp_line[y];
			uint32_t v = l->v0;
			int cx, uni = 1, fy = (int)((v >> 12) & 7), cy = (int)((v >> 5) & 31), pg, col = -1;
			if (D->line_on[y] && !out->line[y].hud) continue;
			if ((l->flags & (NESPL_VALID | NESPL_STABLE | NESPL_BGOFF)) != (NESPL_VALID | NESPL_STABLE) || cy >= 30) continue;
			for (pg = 0; uni && pg < 2; pg++) {
				const uint8_t *np = l->np[(((v >> 10) & 3) ^ (uint32_t) pg) & 3];
				for (cx = 0; uni && cx < 32; cx++) {
					int name = np[cy * 32 + cx], k;
					uint32_t a = np[0x3c0 + (cy >> 2) * 8 + (cx >> 2)], cc = (a >> ((cx & 2) + ((cy & 2) << 1))) & 3;
					const uint8_t *G = l->bp[name >> 6] + (name & 63) * 16 + fy;
					for (k = 0; uni && k < 8; k++) {
						int idx = (((G[8] >> k) & 1) << 1) | ((G[0] >> k) & 1);
						int c = idx ? l->pal[(cc << 2) | idx] : l->pal[0];
						if (col < 0) col = c; else if (c != col) uni = 0;
					}
				}
			}
			if (!uni || col < 0) continue;
			for (i = 0; i < M; i++) {
				W.wide[(size_t) y * OW + i] = rgb_of(y, post_px(l->p1, (uint8_t) col));
				W.wide[(size_t) y * OW + M + 256 + i] = rgb_of(y, post_px(l->p1, (uint8_t) col));
				if (W.debug) W.prov[(size_t) y * OW + i] = W.prov[(size_t) y * OW + M + 256 + i] = WK_PROV_BACKDROP;
			}
			W.st.lines_bar_uniform++;
		}
	}

	/* 5. what the caller and the tests may look at */
	{
		long sl = 0, sr = 0;
		int n = 0;
		for (y = 0; y < H; y++) {
			int l = 0, r = 0;
			if (fr->nlayers && !out->line[y].hud && (nesp_line[y].flags & NESPL_DRAWN)) {
				l = out->line[y].real[0][0]; r = out->line[y].real[1][0];
				if (l > M) l = M;
				if (r > M) r = M;
			}
			W.real[y][0] = (uint8_t) l; W.real[y][1] = (uint8_t) r;
			if (fr->nlayers && out->line[y].hud) W.st.lines_hud++;
			if (nesp_line[y].flags & NESPL_DRAWN) { sl += l; sr += r; n++; }
		}
		W.st.lines_drawn = n;
		W.st.real_l = n ? (int)(sl / n) : 0;
		W.st.real_r = n ? (int)(sr / n) : 0;
		W.st.lines_dominant = D->ndom;
		W.st.layer_active = fr->nlayers;
	}

	/* 6. the scout: a new snapshot when it has nothing to do (it runs in nes_host_scout_step) */
	scout_live_frame(D, out);

	if (rgb) *rgb = W.wide;
	if (width) *width = 256 + 2 * M;
	if (pitch_bytes) *pitch_bytes = (256 + 2 * M) * 2;
	if (st) *st = W.st;
	return 1;
}

const uint8_t *nes_host_wide_real(void) { return &W.real[0][0]; }
const uint8_t *nes_host_wide_prov(void) { return W.debug && W.active ? W.prov : NULL; }
