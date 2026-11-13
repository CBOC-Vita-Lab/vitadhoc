/*
 * NES wide view adapter: turns what the PPU recorder (core/nesp_wide.h) saw into a widekit frame description, runs
 * widekit (wk_layers, wk_map, wk_compose) and returns the composed (256 + 2 * margin) x 240 RGB565 picture.
 * The emulation never reads anything from here; see INTEGRATION.md "Wide view".
 *
 * Two stages (nes_host_wide_frame is both in one call):
 *   capture  (emulation thread, right after nes_host_run_frame): copies what the later stage needs out of the core into a
 *            caller-owned snapshot: per-line records with page indexes instead of pointers, the nametable pages and their
 *            write stamps, the pattern pages the lines use, the margin pixels the PPU drew, the centre picture;
 *   compose  (any thread, one frame behind): describe + wk_layers + wk_map + wk_compose + the mask / bar fills, from the
 *            snapshot ONLY (and the wide view's own state, which this stage owns, including the learned map).
 *
 * The scout (built with -DNESW_SCOUT=1 and linked with scoutcore.o: the same core + host.c with every global renamed
 * nesscout_*): a second machine started from the live game's saved state, run ahead with the direction the camera moves,
 * whose background pictures are described the same way and handed to the learned map as look-ahead cells through a lock-free
 * queue the compose stage drains (wk_map_record_scout). It shares no memory with the live machine. Its control messages
 * (start / stop) travel compose -> next capture, and its state mirror capture -> compose, so every exchange happens at a
 * point where the other side is quiescent: the result does not depend on thread timing.
 *
 * Copyright CBOC-Vita. MIT licence (see nes/frontend/LICENSE).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

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
#define SN_NP 8          /* nametable page slots of a snapshot */
#define SN_CP 16         /* pattern page slots (1 KiB each) */
#define SN_NONE 0xFF
#define SN_MAGIC 0x4E455357534E4150ull   /* "NESWSNAP" */

/* ---- the snapshot --------------------------------------------------------------------------------------------- */

/* one line's record (NespLine with the pointers replaced by page slots) */
typedef struct {
	uint16_t v0;
	uint8_t x0, p0, p1, flags, bd, deemp;
	uint8_t bp[4];        /* pattern page slots of the four BG pattern pages (SN_NONE when the line is not steady) */
	uint8_t np[4];        /* nametable page slots (must follow bp: the two are compared as one 8 byte key) */
	uint8_t pal[16];
	uint8_t sprm, pad_;
} SnLine;

/* what describe() needs: shared by the live snapshot and the scout's */
typedef struct {
	uint32_t frame;
	uint32_t stamp_gen; uint8_t stamp_ok;
	int nnt, nchr;
	uint8_t sid[SN_NP];                /* the recorder's stamp id of each nametable slot */
	uint32_t chrsig[SN_CP];            /* identity of a pattern page: offset into the first CHR chip, else its address */
	const uint8_t *ntkey[SN_NP];       /* capture side only: where a slot was copied from (never dereferenced later) */
	const uint8_t *chrkey[SN_CP];
	uint8_t nt[SN_NP][1024];
	uint8_t chr[SN_CP][1024];
	uint32_t stamp[NESP_NTP][1024];
	SnLine line[H];
} SnCore;

typedef struct {
	uint64_t magic;
	uint32_t seq;                      /* capture counter */
	int margin;
	uint8_t is_cart, scout_state, pad0_, pad1_;
	unsigned pad;                      /* controller port 0 at the capture */
	uint8_t palram[32];
	uint16_t pal565[768];              /* RGB565 of the palette indexes (0..255 plain, 256.. emphasis variants) */
	uint8_t wl[H][NESP_MBUF], wr[H][NESP_MBUF], wm[H][8];
	uint16_t centre[H * 256];
	SnCore core;
} Snap;

typedef char snap_fits[sizeof(Snap) <= NES_WIDE_SNAPSHOT_BYTES ? 1 : -1];
typedef char snapline_keys[offsetof(SnLine, np) == offsetof(SnLine, bp) + 4 ? 1 : -1];

/* one machine's PPU record (the live core, or the scout's renamed copy) */
typedef struct {
	const NespLine *line;
	const uint32_t (*ntstamp)[1024];
	int (*ntid)(const uint8_t *page);
	const uint8_t *(*chr_base)(int *size);
	const uint32_t *ntgen;
	uint32_t frame;
} NesInst;

/* the description of one frame for widekit */
typedef struct {
	wk_frame *fr;
	uint32_t hash[RING_MAX], code[RING_MAX], code2[RING_MAX], stamp[RING_MAX];
	uint8_t line_on[H];
	uint8_t bp[4], np[4];              /* the dominant state: pattern page slots, nametable slots */
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
	/* uniform-row backdrop fill (screens that never scroll sideways) */
	int hscrolled;                        /* sticky per game: a line's horizontal scroll changed between two frames */
	int prev_sx[H];
	int still_frames;                     /* frames with a background layer seen so far (the fill waits for 120) */
	uint8_t prev_on[H];
	/* emulation thread */
	uint32_t cap_seq;
	Snap *own;                            /* the snapshot of nes_host_wide_frame (allocated on first use) */
	/* compose side: the scout's look-ahead cells are replayed into the map through these */
	wk_frame *sf;
	wk_out *so;
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

/* RGB565 of a palette index on a line whose emphasis bits are deemp (the snapshot's palette table) */
static inline uint16_t rgb_line(const uint16_t *pal, unsigned deemp, uint8_t idx)
{
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

/* ---- capture of the PPU record into a core snapshot (the live machine and the scout) --------------------------------- */

static int slot_find(const uint8_t **keys, int *n, int cap, const uint8_t *p)
{
	int i;
	for (i = 0; i < *n; i++) if (keys[i] == p) return i;
	if (*n >= cap) return -1;
	keys[*n] = p;
	return (*n)++;
}

/* Copy everything describe() and the fills read from the machine's record. Lines whose bank / nametable state is not
 * steady get no pages (nothing reads them). A line that needs more pages than the snapshot holds is made invalid. */
static void cap_core(SnCore *c, const NesInst *I, int with_deemp)
{
	int y, i, csz = 0;
	const uint8_t *cb = I->chr_base(&csz);
	const NespLine *pl = NULL;
	SnLine *ps = NULL;
	c->frame = I->frame;
	c->nnt = c->nchr = 0;
	for (y = 0; y < H; y++) {
		const NespLine *l = &I->line[y];
		SnLine *s = &c->line[y];
		s->flags = l->flags;
		s->v0 = l->v0; s->x0 = l->x0; s->p0 = l->p0; s->p1 = l->p1; s->bd = l->bd;
		s->deemp = (with_deemp && XDBuf) ? XDBuf[y * 256] : 0;
		memset(s->bp, SN_NONE, 8);
		memcpy(s->pal, l->pal, 16);
		if ((l->flags & (NESPL_VALID | NESPL_STABLE | NESPL_BGOFF)) != (NESPL_VALID | NESPL_STABLE)) { pl = NULL; continue; }
		if (pl && !memcmp(pl->bp, l->bp, sizeof l->bp + sizeof l->np)) {
			memcpy(s->bp, ps->bp, 8);
		} else {
			int ok = 1;
			for (i = 0; i < 4; i++) {
				int a = slot_find(c->chrkey, &c->nchr, SN_CP, l->bp[i]);
				int b = slot_find(c->ntkey, &c->nnt, SN_NP, l->np[i]);
				if (a < 0 || b < 0) { ok = 0; break; }
				s->bp[i] = (uint8_t) a; s->np[i] = (uint8_t) b;
			}
			if (!ok) { s->flags = 0; memset(s->bp, SN_NONE, 8); pl = NULL; continue; }
		}
		pl = l; ps = s;
	}
	for (i = 0; i < c->nnt; i++) {
		memcpy(c->nt[i], c->ntkey[i], 1024);
		c->sid[i] = (uint8_t) I->ntid(c->ntkey[i]);
	}
	for (i = 0; i < c->nchr; i++) {
		long off = cb ? (long)(c->chrkey[i] - cb) : 0;
		memcpy(c->chr[i], c->chrkey[i], 1024);
		if (!cb || off < 0 || off >= csz) off = (long)((uintptr_t) c->chrkey[i] & 0xFFFFF);
		c->chrsig[i] = (uint32_t) off;
	}
	/* the write stamps: current while the recorder's generation is unchanged */
	if (!c->stamp_ok || c->stamp_gen != *I->ntgen) {
		memcpy(c->stamp, I->ntstamp, sizeof c->stamp);
		c->stamp_gen = *I->ntgen;
		c->stamp_ok = 1;
	}
}

/* ---- the frame description (shared by the live machine and the scout) ---------------------------------------- */

typedef struct { uint8_t k[8]; int n; } DomKey;   /* pattern page slots then nametable slots of a steady line */

/* Describe the background of the frame the machine I just drew: the dominant steady state (BG banks + nametable
 * mapping), the nametable ring with redraw codes, content hashes and write stamps, and per line the scroll. Fills D and
 * D->fr except the layer's gfx_id (the caller's identity rules). Returns 1 when a layer was described. */
static int describe(const SnCore *c, Desc *D, int margin, int settle)
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
		const SnLine *l = &c->line[y];
		int k;
		D->line_on[y] = 0;
		if (!(l->flags & NESPL_VALID)) continue;
		if (!(l->flags & NESPL_BGOFF)) D->rendering = 1;
		if ((l->flags & (NESPL_STABLE | NESPL_BGOFF)) != NESPL_STABLE) continue;
		if (((l->v0 >> 5) & 31) >= 30) continue;
		for (k = 0; k < nk; k++)
			if (!memcmp(l->bp, keys[k].k, 8)) break;
		if (k == nk) {
			if (nk == 4) continue;
			memcpy(keys[k].k, l->bp, 8);
			keys[k].n = 0;
			nk++;
		}
		keys[k].n++;
	}
	for (i = 0; i < nk; i++) if (keys[i].n > bestn) { bestn = keys[i].n; best = i; }
	memset(fr, 0, sizeof *fr);
	fr->frame = c->frame;
	fr->view_w = 256; fr->view_h = H; fr->margin = margin;
	fr->brightness = D->rendering ? 255 : 0;
	fr->nlayers = 0;
	D->mw = D->mh = 0;
	if (best < 0 || bestn < 8) return 0;

	dom = &keys[best];
	memcpy(D->bp, dom->k, 4);
	memcpy(D->np, dom->k + 4, 4);
	D->distx = D->np[0] != D->np[1] || D->np[2] != D->np[3];
	D->disty = D->np[0] != D->np[2] || D->np[1] != D->np[3];
	mw = D->mw = D->distx ? 64 : 32;
	mh = D->mh = D->disty ? 60 : 30;

	/* tile content hashes of the BG pattern table in force, for the names the ring uses */
	{
		int rx, ry, name;
		memset(D->used, 0, sizeof D->used);
		for (ry = 0; ry < mh; ry++)
			for (rx = 0; rx < mw; rx++) {
				const uint8_t *np = c->nt[D->np[((D->disty ? ry / 30 : 0) << 1) | (D->distx ? rx / 32 : 0)]];
				D->used[np[(ry % 30) * 32 + (rx & 31)]] = 1;
			}
		for (name = 0; name < 256; name++)
			if (D->used[name]) D->gh[name] = tile_hash(c->chr[D->bp[name >> 6]] + (name & 63) * 16);
	}
	/* bank identity: offsets into CHR ROM (stable across machines), else the address */
	{
		uint32_t sig = 0;
		for (i = 0; i < 4; i++) sig = mix32(sig, c->chrsig[D->bp[i]]);
		D->sig = mix32(sig, (uint32_t) (mw * 64 + mh));
	}

	/* the ring */
	{
		int rx, ry;
		const uint32_t settle_n = (uint32_t) (settle ? settle - 1 : 2);
		for (ry = 0; ry < mh; ry++)
			for (rx = 0; rx < mw; rx++) {
				const int slot = D->np[((D->disty ? ry / 30 : 0) << 1) | (D->distx ? rx / 32 : 0)];
				const uint8_t *np = c->nt[slot];
				uint32_t ts, as, ft, fa, f, cc, a, k = (uint32_t)(ry * mw + rx);
				int mid, cx = rx & 31, cy = ry % 30, id = c->sid[slot], ai = 0x3c0 + (cy >> 2) * 8 + (cx >> 2), name;
				ts = c->stamp[id][cy * 32 + cx];
				as = c->stamp[id][ai];
				ft = ts >> 1; fa = as >> 1;
				f = ft > fa ? ft : fa;
				mid = ((ts & 1) && ft == c->frame) || ((as & 1) && fa == c->frame);
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
				if (mid || (f && c->frame - f < settle_n)) {
					/* settling (just written, its palette may follow next frame): a changing hash keeps it out of the verified
					   history, and a stamp 'in the future' stops every stamp chain at it, also one that would start from it as
					   the edge cell (a stamp of 0 there made every older cell beyond look newer than the edge); widekit's
					   stream detector still sees it as fresh. */
					D->hash[k] ^= 0x7F4A7C15u * (c->frame | 1u);
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
		const SnLine *l = &c->line[y];
		int on = (l->flags & NESPL_VALID) && (l->flags & (NESPL_STABLE | NESPL_BGOFF)) == NESPL_STABLE &&
		         ((l->v0 >> 5) & 31) < 30 && !memcmp(l->bp, dom->k, 8);
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
			if (!D->line_on[y] || (c->line[y].p1 & 2)) continue;
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
extern uint32_t nesscout_nesp_ntgen;
extern NespLine nesscout_nesp_line[240];
extern uint32_t nesscout_nesp_ntstamp[NESP_NTP][1024];
extern int nesscout_nesp_ntid(const uint8_t *page);
extern const uint8_t *nesscout_nesp_chr_base(int *size);
extern void nesscout_nesp_clear_stamps(void);
#endif

/* what the scout posts to the compose side per frame: the picture's description in the live world's coordinates */
typedef struct {
	uint32_t seq;                 /* capture counter at the post: the compose of a later capture takes it */
	uint32_t epoch, gfx_id;
	int kind;
	uint8_t coherent[H];
	int32_t world_x[H], world_row[H];
	uint32_t code[RING_MAX], hash[RING_MAX];
	int mw, mh;
} ScoutPost;
#define SQ_N 8

/* the link between the scout's world and the live world, measured by the compose side at the frame of the snapshot */
typedef struct {
	int y0; int32_t wx0, wr0, sx0, ry0;
	uint8_t hud[H];
	int wpx, hpx;
	uint32_t epoch, base_live, gfx;
	uint32_t ref_gh[256];         /* tile hash of every name of the live pattern table at the snapshot */
} ScoutLink;

typedef struct {
	int enabled, available, inited, loaded;
	int ahead_max, frames_per_step, resync;     /* options */
	uint8_t *rom; size_t rom_n; char name[64];
	/* owned by the scout thread (scout_step); the other threads touch them only at the sync points */
	uint8_t *snap; size_t snap_cap, snap_n;     /* the state the scout loads next */
	int need_load;                              /* a new snapshot waits for the scout */
	int session;                                /* the scout runs from the last snapshot */
	uint32_t base_live;                         /* live frame counter at the snapshot */
	uint32_t epoch;                             /* the live epoch the snapshot belongs to */
	int frames;                                 /* scout frames run since the snapshot */
	unsigned pad;
	ScoutLink lk;
	uint32_t retry_at;                          /* after a stop: no new snapshot before this live frame */
	int linked; int32_t shift_x, shift_r;
	Desc D;
	SnCore core;
	wk_layers *L;
	wk_out *out, *out2;
	uint64_t (*clock_us)(void);
	nes_scout_stats st;
	uint32_t loads;
	/* emulation thread (capture): the state taken for a snapshot, until the link arrives */
	uint8_t *stage; size_t stage_cap, stage_n; unsigned stage_pad;
	uint32_t live_frame;                        /* frame counter of the latest capture (the scout's clock) */
	uint32_t cap_seq;                           /* capture counter of the latest capture */
	/* compose -> capture: written by compose, read and cleared at the next capture */
	int c_stop, c_want, c_dir, c_link;
	ScoutLink c_lk;
	unsigned c_pad;
	/* capture -> compose: the scout's state as of this capture */
	int m_session, m_frames; uint32_t m_base_live, m_epoch, m_retry_at;
	int phase;                                  /* compose side: a snapshot was asked for, its frame is the next compose */
	int ahead_stat;
	/* scout -> compose: lock-free single producer / single consumer queue */
	ScoutPost *q;
	atomic_uint qh, qt;
} Scout;

static Scout S;

static void scout_free(void)
{
	free(S.rom); S.rom = NULL; S.rom_n = 0;
	free(S.snap); S.snap = NULL; S.snap_cap = 0;
	free(S.stage); S.stage = NULL; S.stage_cap = 0;
	free(S.q); S.q = NULL;
	S.session = S.need_load = 0;
}

static void scout_reset_link(void)
{
	S.session = S.need_load = 0;
	S.c_stop = S.c_want = S.c_link = 0;
	S.phase = 0;
	S.m_session = 0;
	atomic_store(&S.qt, atomic_load(&S.qh));
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
	if (!S.q) S.q = (ScoutPost *) calloc(SQ_N, sizeof(ScoutPost));
	if (!S.L) { wk_layers_cfg lc; wk_layers_cfg_default(&lc); S.L = wk_layers_create(&lc); }
	if (!S.D.fr || !S.out || !S.out2 || !S.L || !S.q) return 0;
	S.loaded = 1;
	return 1;
}
#endif

/* Sync point at the start of a capture (emulation thread; compose of the previous frame has returned, the scout is not
 * stepping): the compose side's messages take effect, and the scout's state is mirrored for the next compose. */
static void scout_sync(void)
{
#if NESW_SCOUT
	if (S.c_stop) { S.session = 0; S.need_load = 0; S.c_stop = 0; }
	if (S.c_link) {
		uint8_t *t = S.snap; size_t tc = S.snap_cap;
		S.snap = S.stage; S.snap_cap = S.stage_cap; S.snap_n = S.stage_n;
		S.stage = t; S.stage_cap = tc; S.stage_n = 0;
		S.lk = S.c_lk;
		S.pad = S.c_pad;
		S.base_live = S.lk.base_live;
		S.epoch = S.lk.epoch;
		S.frames = 0;
		S.linked = 0;
		S.need_load = 1;
		S.session = 1;
		S.c_link = 0;
	}
	S.m_session = S.session; S.m_frames = S.frames; S.m_base_live = S.base_live; S.m_epoch = S.epoch; S.m_retry_at = S.retry_at;
#endif
}

/* Emulation thread, inside a capture that succeeded: the scout machine is created lazily, and a state of the live game is
 * saved when the compose side asked for one at its last frame. */
static void scout_capture(Snap *sn)
{
#if NESW_SCOUT
	S.live_frame = sn->core.frame;
	S.cap_seq = sn->seq;
	if (!S.enabled || !W.M || !sn->is_cart) return;
	if (!S.loaded && !scout_load()) { S.available = 0; return; }
	S.available = 1;
	if (S.c_want) {
		size_t need = nes_host_state_size();
		S.c_want = 0;
		if (need > S.stage_cap) {
			uint8_t *p = (uint8_t *) realloc(S.stage, need);
			if (!p) return;
			S.stage = p; S.stage_cap = need;
		}
		S.stage_n = nes_host_state_save_mem(S.stage, S.stage_cap);
		if (!S.stage_n) return;
		S.stage_pad = sn->pad;
		if (S.c_dir > 0) S.stage_pad = (S.stage_pad & ~(unsigned) NES_LEFT) | NES_RIGHT;
		else if (S.c_dir < 0) S.stage_pad = (S.stage_pad & ~(unsigned) NES_RIGHT) | NES_LEFT;
		sn->scout_state = 1;
		S.st.snapshots++;
	}
#else
	(void) sn;
#endif
}

/* Compose side, end of a compose: either the frame of a snapshot that was asked for (link it to the live world), or the
 * decision whether a new snapshot is wanted at the next capture. Reads the mirror, writes the c_* messages. */
static void scout_control(const Snap *sn, const Desc *D, const wk_out *out)
{
#if NESW_SCOUT
	int y, ahead, y0 = -1;
	const uint32_t frame = D->fr->frame;
	if (!S.enabled || !W.M || !D->fr->nlayers || !sn->is_cart) { S.c_stop = 1; S.c_want = 0; S.phase = 0; return; }
	for (y = 0; y < H; y++)   /* the link: a line on in this frame, coherent, not a status bar */
		if (D->line_on[y] && out->coherent[0][y] && !out->line[y].hud) { y0 = y; break; }
	if (S.phase) {
		/* the frame the state was saved at: measure the link */
		S.phase = 0;
		if (!sn->scout_state) return;
		if (y0 < 0) { S.c_stop = 1; return; }
		{
			ScoutLink *k = &S.c_lk;
			int name;
			for (y = 0; y < H; y++) k->hud[y] = out->line[y].hud;
			k->y0 = y0;
			k->wx0 = out->world_x[0][y0];
			k->wr0 = out->world_row[0][y0];
			k->sx0 = D->fr->layer[0].scroll_x[y0];
			k->ry0 = D->fr->layer[0].row_y[y0];
			k->wpx = D->mw * 8; k->hpx = D->mh * 8;
			k->epoch = out->epoch;
			k->base_live = frame;
			k->gfx = W.layer_gfx;
			for (name = 0; name < 256; name++) k->ref_gh[name] = tile_hash(sn->core.chr[D->bp[name >> 6]] + (name & 63) * 16);
			S.c_pad = S.stage_pad;
			S.c_link = 1;
		}
		return;
	}
	ahead = S.m_frames - (int)(frame - S.m_base_live);
	S.ahead_stat = ahead < 0 ? 0 : ahead;
	/* a running session continues while it is not far behind the game (a new session starts two frames behind it) */
	if (S.m_session && !out->scene_event && out->epoch == S.m_epoch && ahead > -4 && (int)(frame - S.m_base_live) < S.resync)
		return;
	if (out->scene_event || out->dir[0] == 0) { S.c_stop = 1; return; }   /* nothing to look ahead into */
	if ((int32_t)(frame - S.m_retry_at) < 0) return;
	if (y0 < 0) { S.c_stop = 1; return; }
	S.c_want = 1;
	S.c_dir = out->dir[0];
	S.phase = 1;
#else
	(void) sn; (void) D; (void) out;
#endif
}

#if NESW_SCOUT
/* one scout frame: run, describe, check that its graphics are the live game's, link its world to the live world, post */
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
	si.line = nesscout_nesp_line; si.ntstamp = (const uint32_t (*)[1024]) nesscout_nesp_ntstamp; si.ntid = nesscout_nesp_ntid;
	si.chr_base = nesscout_nesp_chr_base; si.frame = nesscout_nesp_frame; si.ntgen = &nesscout_nesp_ntgen;
	cap_core(&S.core, &si, 0);
	if (!describe(&S.core, &S.D, W.margin, W.settle)) return 0;
	for (y = 0; y < H; y++) if (S.lk.hud[y]) S.D.line_on[y] = 0;   /* the live picture's status bar is not level */
	/* the same graphics as the live picture: same ring shape, and the tile contents of the names it uses compared with the
	   live pattern table: a different tile set stops the run; a few differing tiles (a bank animation in another phase)
	   are not recorded */
	if (S.D.mw * 8 != S.lk.wpx || S.D.mh * 8 != S.lk.hpx) return 0;
	{
		int nused = 0, ndiff = 0, k;
		uint8_t diff[256];
		for (name = 0; name < 256; name++) {
			diff[name] = 0;
			if (!S.D.used[name]) continue;
			nused++;
			if (S.D.gh[name] != S.lk.ref_gh[name]) { diff[name] = 1; ndiff++; }
		}
		if (ndiff * 10 >= nused) return 0;
		if (ndiff)
			for (k = 0; k < S.D.mw * S.D.mh; k++)
				if (diff[S.D.code[k] & 255]) S.D.code2[k] = WK_CODE_NOREC;
	}
	SL = &S.D.fr->layer[0];
	SL->gfx_id = S.lk.gfx;
	wk_layers_update(S.L, S.D.fr, S.out);
	if (S.frames > 1 && S.out->scene_event) return 0;
	if (!S.linked) {
		/* the scout's first frame follows the live frame of the snapshot: link the same line through the scroll */
		if (!S.D.line_on[S.lk.y0] || !S.out->coherent[0][S.lk.y0]) return 0;
		S.shift_x = S.lk.wx0 + wrapdiff(SL->scroll_x[S.lk.y0] - S.lk.sx0, S.lk.wpx) - S.out->world_x[0][S.lk.y0];
		S.shift_r = S.lk.wr0 + wrapdiff(SL->row_y[S.lk.y0] - S.lk.ry0, S.lk.hpx) - S.out->world_row[0][S.lk.y0];
		S.linked = 1;
	}
	if (S.frames == 1) return 1;     /* the first frame links; its lines have no motion yet to tell a status bar */
	{
		unsigned h = atomic_load_explicit(&S.qh, memory_order_relaxed);
		ScoutPost *p = &S.q[h % SQ_N];
		p->seq = S.cap_seq;
		p->epoch = S.epoch;
		p->gfx_id = SL->gfx_id;
		p->kind = S.out->kind[0];
		p->mw = S.D.mw; p->mh = S.D.mh;
		for (y = 0; y < H; y++) {
			p->coherent[y] = S.out->coherent[0][y];
			p->world_x[y] = S.out->world_x[0][y] + S.shift_x;
			p->world_row[y] = S.out->world_row[0][y] + S.shift_r;
		}
		memcpy(p->code, S.D.code2, sizeof(uint32_t) * (size_t) S.D.mw * S.D.mh);
		memcpy(p->hash, S.D.hash, sizeof(uint32_t) * (size_t) S.D.mw * S.D.mh);
		atomic_store_explicit(&S.qh, h + 1, memory_order_release);
	}
	return 1;
}
#endif

/* Compose side, start of a compose: hand the look-ahead cells the scout posted before this capture to the map. */
static void scout_drain(uint32_t upto)
{
#if NESW_SCOUT
	if (!S.q) return;
	for (;;) {
		unsigned t = atomic_load_explicit(&S.qt, memory_order_relaxed);
		unsigned h = atomic_load_explicit(&S.qh, memory_order_acquire);
		ScoutPost *p;
		wk_layer *L;
		if (t == h) break;
		p = &S.q[t % SQ_N];
		if ((int32_t)(p->seq - upto) >= 0) break;
		if (W.M) {
			if (!W.sf) W.sf = (wk_frame *) calloc(1, sizeof(wk_frame));
			if (!W.so) W.so = (wk_out *) calloc(1, sizeof(wk_out));
			if (W.sf && W.so) {
				L = &W.sf->layer[0];
				W.sf->nlayers = 1; W.sf->view_w = 256; W.sf->view_h = H;
				L->active = 1; L->role = WK_ROLE_AUTO; L->map_w = p->mw; L->map_h = p->mh; L->tile_px = 8;
				L->hash = p->hash; L->code = p->code; L->gfx_id = p->gfx_id;
				W.so->nlines = H; W.so->epoch = p->epoch; W.so->scene_event = 0; W.so->kind[0] = p->kind;
				memcpy(W.so->coherent[0], p->coherent, H);
				memcpy(W.so->world_x[0], p->world_x, sizeof p->world_x);
				memcpy(W.so->world_row[0], p->world_row, sizeof p->world_row);
				wk_map_record_scout(W.M, W.sf, W.so);
			}
		}
		atomic_store_explicit(&S.qt, t + 1, memory_order_release);
	}
#else
	(void) upto;
#endif
}

int nes_host_scout_step(int max_frames, int max_us)
{
#if NESW_SCOUT
	int n = 0;
	uint64_t t0 = S.clock_us ? S.clock_us() : 0;
	if (!S.enabled || !S.session || !S.loaded || !S.q) return 0;
	if (S.need_load) {
		if (nesscout_nes_host_state_load_mem(S.snap, S.snap_n)) { S.session = 0; return 0; }
		/* the loaded nametables are the live game's: count every entry as written long ago */
		{
			int p, e;
			for (p = 0; p < NESP_NTP; p++) for (e = 0; e < 1024; e++) nesscout_nesp_ntstamp[p][e] = 2u;
			nesscout_nesp_ntgen++;
		}
		wk_layers_reset(S.L);
		S.need_load = 0;
	}
	while (n < max_frames) {
		int ahead = S.frames - (int)(S.live_frame - S.base_live);
		if (ahead >= S.ahead_max) break;                      /* far enough: wait for the live game */
		if ((unsigned) (atomic_load_explicit(&S.qh, memory_order_relaxed) - atomic_load_explicit(&S.qt, memory_order_acquire)) >= SQ_N)
			break;                                            /* the compose side has not drained the queue yet */
		if (!scout_one()) { S.session = 0; S.st.stops++; S.retry_at = S.live_frame + 30; break; }
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
	if (!S.enabled) scout_reset_link();
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
	st->ahead = S.ahead_stat;
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
	scout_reset_link();
	S.retry_at = 0; S.loads++;
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
	scout_reset_link();
#if NESW_SCOUT
	if (S.loaded) { nesscout_nes_host_unload(); S.loaded = 0; }
#endif
}

/* a state load, reset or disk change: nametable contents changed without write stamps */
void nesw_on_discontinuity(void)
{
	scout_reset_link();
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
	free(W.D.fr); free(W.out); free(W.own); free(W.sf); free(W.so);
	W.D.fr = NULL; W.out = NULL; W.own = NULL; W.sf = NULL; W.so = NULL;
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

/* ---- capture ------------------------------------------------------------------------------------------------ */

size_t nes_host_wide_snapshot_size(void) { return sizeof(Snap); }

int nes_host_wide_capture(nes_wide_snapshot *ps)
{
	Snap *sn = (Snap *) ps;
	NesInst li;
	const uint8_t *sprm;
	int y, m;

	scout_sync();
	if (!sn || !W.active || !nesw_game_loaded() || !nesw_render_on() || !GameInfo || GameInfo->type == GIT_NSF) return 0;
	if (sn->magic != SN_MAGIC) { sn->magic = SN_MAGIC; sn->core.stamp_ok = 0; }
	m = W.margin;
	sn->seq = ++W.cap_seq;
	sn->margin = m;
	sn->is_cart = (uint8_t) nesw_is_cart();
	sn->scout_state = 0;
	sn->pad = nesw_pad(0);
	memcpy(sn->palram, PALRAM, 32);
	memcpy(sn->pal565, nesw_pal(), sizeof sn->pal565);
	li.line = nesp_line; li.ntstamp = (const uint32_t (*)[1024]) nesp_ntstamp; li.ntid = nesp_ntid; li.chr_base = nesp_chr_base;
	li.frame = nesp_frame; li.ntgen = &nesp_ntgen;
	cap_core(&sn->core, &li, 1);
	sprm = nesw_sprm();
	for (y = 0; y < H; y++) {
		sn->core.line[y].sprm = sprm[y];
		if (sn->core.line[y].flags & NESPL_DRAWN) {
			memcpy(sn->wl[y], nesp_wl[y], (size_t) m);
			memcpy(sn->wr[y], nesp_wr[y], (size_t) m);
		}
	}
	memcpy(sn->wm, nesp_wm, sizeof sn->wm);
	memcpy(sn->centre, nesw_fb(), sizeof sn->centre);
	scout_capture(sn);
	return 1;
}

/* ---- compose ------------------------------------------------------------------------------------------------ */

/* draw one background pixel of a learned-map tile (code = name | palette << 8) with the snapshot's banks and palette */
static uint8_t cell_px(const Snap *sn, const Desc *D, uint32_t code, int xin, int fy)
{
	int name = (int)(code & 255), cc = (int)((code >> 8) & 3);
	const uint8_t *G = sn->core.chr[D->bp[name >> 6]] + (name & 63) * 16 + fy;
	int b = 7 - xin, idx = (((G[8] >> b) & 1) << 1) | ((G[0] >> b) & 1);
	return (uint8_t)(idx ? sn->palram[(cc << 2) | idx] : (sn->palram[cc << 2] | 0x40));
}

static void fill_strips(const Snap *sn, const wk_frame *fr, wk_out *out)
{
	const int M = W.margin;
	const Desc *D = &W.D;
	const uint16_t *pal = sn->pal565;
	int y, s, d;
	for (y = 0; y < H; y++) {
		const SnLine *nl = &sn->core.line[y];
		wk_line_out *lo = &out->line[y];
		int drawn = (nl->flags & NESPL_DRAWN) != 0;
		const unsigned deemp = nl->deemp;
		for (s = 0; s < 2; s++) {
			uint16_t *dst = W.cand[s] + (size_t) y * M;
			const uint8_t *src = s == 0 ? sn->wl[y] : sn->wr[y];
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
			/* the learned map's cell is the same for the 8 px of one tile: look it up once per tile */
			int cidx = 0x7fffffff, cok = 0;
			wk_cell cell;
			for (d = 0; d < real; d++) {
				uint8_t px = src[d];
				if (d < mpart) {
					wk_cell c0;
					if (wk_map_margin_cell(W.M, fr, out, 0, y, s, 0, &c0)) {
						px = post_px(nl->p1, cell_px(sn, D, c0.code, (out->world_x[0][y] - 1 - d) & 7, out->world_row[0][y] & 7));
					} else {
						lo->real[s][0] = (uint16_t) d; lo->ring[s][0] = (uint16_t) (ring < d ? ring : d);
						break;
					}
				} else if (have_geom && d >= ring) {
					int wx = out->world_x[0][y], idx, xw;
					idx = s == 0 ? (int)(g.tile_first - fdiv8(wx - 1 - d)) : (int)(fdiv8(wx + 256 + d) - (g.tile_first + g.nvis - 1));
					if (idx != cidx) {
						cok = wk_map_margin_cell(W.M, fr, out, 0, y, s, idx, &cell);
						cidx = idx;
					}
					if (cok) {
						int fy = out->world_row[0][y] & 7;
						xw = s == 0 ? wx - 1 - d : wx + 256 + d;
						px = post_px(nl->p1, cell_px(sn, D, cell.code, xw & 7, fy));
					} else {
						/* widekit said real but the map has no cell here: never show the raw ring in its place */
						lo->real[s][0] = (uint16_t) d;
						break;
					}
				}
				dst[d] = rgb_line(pal, deemp, px);
			}
		}
		wk_out_refresh_line(out, y);
	}
}

static void build_inner(const Snap *sn)
{
	const uint16_t *fb = sn->centre;
	int y, e;
	for (y = 0; y < H; y++) {
		const uint16_t *src = fb + y * 256;
		uint16_t *l = W.inner[0] + (size_t) y * WK_MAX_INNER, *r = W.inner[1] + (size_t) y * WK_MAX_INNER;
		memcpy(l, src, WK_MAX_INNER * sizeof(uint16_t));
		for (e = 0; e < WK_MAX_INNER; e++)
			r[e] = src[255 - e];
	}
}

int nes_host_wide_compose(const nes_wide_snapshot *ps, const uint16_t **rgb, int *width, int *pitch_bytes, nes_wide_stats *st)
{
	const Snap *sn = (const Snap *) ps;
	const SnCore *C0;
	const uint16_t *pal565;
	int y, i;
	Desc *D = &W.D;
	wk_frame *fr;
	wk_out *out = W.out;
	int M = W.margin;

	if (!sn || sn->magic != SN_MAGIC || !W.active || !W.L || !W.C || sn->margin != M) return 0;
	C0 = &sn->core;
	pal565 = sn->pal565;

	/* 0. look-ahead cells the scout posted before this capture */
	scout_drain(sn->seq);

	/* 1. the frame description */
	describe(C0, D, M, W.settle);
	fr = D->fr;
	{
		int delta = 0;
		if (W.have_pal_prev) for (i = 0; i < 32; i++) if (W.pal_prev[i] != sn->palram[i]) delta++;
		memcpy(W.pal_prev, sn->palram, 32);
		W.have_pal_prev = 1;
		fr->palette_delta = delta;
		fr->palette_size = 32;
	}
	memset(&W.st, 0, sizeof W.st);
	W.st.frame = C0->frame;
	W.st.margin = M;
	W.st.width = 256 + 2 * M;
	for (y = 0; y < H; y++) if (C0->line[y].flags & NESPL_BLOCK) W.st.lines_blocked++;

	if (fr->nlayers) {
		int name, nused = 0, same = 0;
		wk_layer *L = &fr->layer[0];
		const int mw = D->mw;
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
		if (fr->nlayers) fill_strips(sn, fr, out);
		for (y = 0; y < H; y++)   /* the backdrop as the line shows it: entry 0 through the grayscale / emphasis pass */
			W.bg[y] = rgb_line(pal565, C0->line[y].deemp, post_px(C0->line[y].p1, C0->line[y].bd | 0x40));
		ci.ana = out;
		ci.centre = sn->centre; ci.centre_stride = 256;
		ci.bg = W.bg;
		if (fr->nlayers) {
			ci.cand[0][0] = W.cand[0]; ci.cand[0][1] = W.cand[1];
			ci.order[0] = 0; ci.norder = 1;
			if (W.period) {
				build_inner(sn);
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
		for (y = 0; y < H; y++) {
			const SnLine *l = &C0->line[y];
			uint8_t px8[8];
			int ok = 1;
			if (!(l->flags & NESPL_DRAWN) || (l->p1 & 2)) continue;
			if (out->line[y].hud || out->line[y].real[0][0] < 8 || !out->coherent[0][y]) continue;
			memcpy(px8, sn->wm[y], 8);
			if (D->mw == 32) {
				int wx = out->world_x[0][y], first = 8 - (wx & 7);   /* x < first: the leftmost (seam) column */
				wk_cell c0;
				if (!wk_map_margin_cell(W.M, fr, out, 0, y, 0, 0, &c0)) ok = 0;
				for (i = 0; ok && i < first; i++)
					px8[i] = post_px(l->p1, cell_px(sn, D, c0.code, (wx + i) & 7, out->world_row[0][y] & 7));
			}
			if (!ok) continue;
			for (i = 0; i < 8; i++)
				if (!((l->sprm >> i) & 1)) W.wide[(size_t) y * (256 + 2 * M) + M + i] = rgb_line(pal565, l->deemp, px8[i]);
		}
	}

	/* 4b. uniform-row backdrop: on a game that has never scrolled sideways and whose ring is one screen wide, a line whose
	   background pixels are all the backdrop colour across the whole ring (sky, black bands) continues as flat backdrop
	   instead of the soft edge. It is the backdrop colour, never a tile; not counted as real. It starts after 120 frames
	   without a sideways scroll (a scroller that has not moved yet would show backdrop where its level continues). */
	if (fr->nlayers && !W.hscrolled && W.still_frames >= 120 && D->mw == 32) {
		const int OW = 256 + 2 * M;
		for (y = 0; y < H; y++) {
			const SnLine *l = &C0->line[y];
			const uint8_t *np;
			uint32_t v = l->v0;
			int cx, uni = 1, fy, cy;
			if (!D->line_on[y] || out->line[y].hud) continue;
			np = C0->nt[l->np[(v >> 10) & 3]];
			cy = (int)((v >> 5) & 31); fy = (int)((v >> 12) & 7);
			for (cx = 0; uni && cx < 32; cx++) {
				int name = np[cy * 32 + cx];
				const uint8_t *G = C0->chr[l->bp[name >> 6]] + (name & 63) * 16 + fy;
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
			const SnLine *l = &C0->line[y];
			uint32_t v = l->v0;
			int cx, uni = 1, fy = (int)((v >> 12) & 7), cy = (int)((v >> 5) & 31), pg, col = -1;
			if (D->line_on[y] && !out->line[y].hud) continue;
			if ((l->flags & (NESPL_VALID | NESPL_STABLE | NESPL_BGOFF)) != (NESPL_VALID | NESPL_STABLE) || cy >= 30) continue;
			for (pg = 0; uni && pg < 2; pg++) {
				const uint8_t *np = C0->nt[l->np[(((v >> 10) & 3) ^ (uint32_t) pg) & 3]];
				for (cx = 0; uni && cx < 32; cx++) {
					int name = np[cy * 32 + cx], k;
					uint32_t a = np[0x3c0 + (cy >> 2) * 8 + (cx >> 2)], cc = (a >> ((cx & 2) + ((cy & 2) << 1))) & 3;
					const uint8_t *G = C0->chr[l->bp[name >> 6]] + (name & 63) * 16 + fy;
					for (k = 0; uni && k < 8; k++) {
						int idx = (((G[8] >> k) & 1) << 1) | ((G[0] >> k) & 1);
						int c = idx ? l->pal[(cc << 2) | idx] : l->pal[0];
						if (col < 0) col = c; else if (c != col) uni = 0;
					}
				}
			}
			if (!uni || col < 0) continue;
			for (i = 0; i < M; i++) {
				W.wide[(size_t) y * OW + i] = rgb_line(pal565, l->deemp, post_px(l->p1, (uint8_t) col));
				W.wide[(size_t) y * OW + M + 256 + i] = rgb_line(pal565, l->deemp, post_px(l->p1, (uint8_t) col));
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
			if (fr->nlayers && !out->line[y].hud && (C0->line[y].flags & NESPL_DRAWN)) {
				l = out->line[y].real[0][0]; r = out->line[y].real[1][0];
				if (l > M) l = M;
				if (r > M) r = M;
			}
			W.real[y][0] = (uint8_t) l; W.real[y][1] = (uint8_t) r;
			if (fr->nlayers && out->line[y].hud) W.st.lines_hud++;
			if (C0->line[y].flags & NESPL_DRAWN) { sl += l; sr += r; n++; }
		}
		W.st.lines_drawn = n;
		W.st.real_l = n ? (int)(sl / n) : 0;
		W.st.real_r = n ? (int)(sr / n) : 0;
		W.st.lines_dominant = D->ndom;
		W.st.layer_active = fr->nlayers;
	}

	/* 6. the scout: link a snapshot taken for it, or ask for a new one at the next capture */
	scout_control(sn, D, out);

	if (rgb) *rgb = W.wide;
	if (width) *width = 256 + 2 * M;
	if (pitch_bytes) *pitch_bytes = (256 + 2 * M) * 2;
	if (st) *st = W.st;
	return 1;
}

/* the one-call form: capture + compose on an internal snapshot */
int nes_host_wide_frame(const uint16_t **rgb, int *width, int *pitch_bytes, nes_wide_stats *st)
{
	if (!W.active) { scout_sync(); return 0; }
	if (!W.own) { W.own = (Snap *) calloc(1, sizeof(Snap)); if (!W.own) return 0; }
	if (!nes_host_wide_capture((nes_wide_snapshot *) W.own)) return 0;
	return nes_host_wide_compose((const nes_wide_snapshot *) W.own, rgb, width, pitch_bytes, st);
}

const uint8_t *nes_host_wide_real(void) { return &W.real[0][0]; }
const uint8_t *nes_host_wide_prov(void) { return W.debug && W.active ? W.prov : NULL; }
