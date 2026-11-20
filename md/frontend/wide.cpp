/*
 * The Sega player's wide view (see wide.h). Copyright CBOC-Vita, MIT licence.
 */
#include "wide.h"
#include <stdlib.h>
#include <string.h>

namespace mdw {

static inline uint32_t mix32(uint32_t a, uint32_t b)
{
	uint32_t h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u + (a << 6) + (a >> 2));
	h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12;
	return h;
}
static inline uint16_t fold16(uint32_t v) { return (uint16_t) ((v ^ (v >> 16)) & 0xFFFF); }
static inline uint32_t cell_hash(uint32_t code, uint32_t th) { return mix32(code * 0x10001u + 0x1234567u, th); }
static inline int imod(int a, int n) { int r = a % n; return r < 0 ? r + n : r; }
static inline int fdiv(int a, int n) { int q = a / n; return (a % n < 0) ? q - 1 : q; }

enum { RING_MAX = 4096, NTILE = 0x800, MAP_RUNS = 64 };

Meta::Meta() : ready(1), valid(0), system(0), vw(0), vh(0), margin(0), real_cap(0), view_off(0), have_strips(0), own_strips(0), nlayers(0), frame(0),
	planes(3), all_opaque(1), epoch(0), scene_event(0), px(0), px_cap(0), mask_left(0)
{
	memset(bar_ok, 0, sizeof bar_ok);
	memset(hud, 0, sizeof hud); memset(real, 0, sizeof real); memset(pic, 0, sizeof pic); memset(opq, 0, sizeof opq);
}
Meta::~Meta() { free(px); }

static void (*g_yield)(void);
void Wide::set_yield(void (*fn)(void)) { g_yield = fn; }
void Wide::wait(const Meta &m)
{
	unsigned spins = 0;
	while (!m.ready.load(std::memory_order_acquire)) { if (++spins > 64 && g_yield) g_yield(); }
}

Wide::Wide() : L_(0), M_(0), map_system_(0), C_(0), ci_(0), cmeta_(0), edge_(0), cvw_(0), cvh_(0), cm_(0), fr_(0), out_(0), ana2_(0), wide_(0), prov_(0), th_(0), thok_(0),
	have_prev_(0), map_w_(0), map_h_(0), layout_key_(0), have_cram_(0), cap_default_(1 << 20), learn_(true), want_prov_(false),
	ovr_live_(0), ovr_next_(0), rowbuf_(0), seam_prev_(0), content_gen_(0), map_key_(0), map_key_ok_(0), check_(false), claims_(0), crc_(0), c_real_pct_(0), c_soft_lines_(0), c_composed_(0), planes_cur_(3), planes_next_(3)
{
	cand_[0] = cand_[1] = 0; opq_[0] = opq_[1] = 0;
	for (int i = 0; i < COMPOSE_WHO; i++) scr_[i] = 0;
	for (int p = 0; p < 2; p++) {
		hash_[p] = 0; stamp_[p] = 0; wstamp_[p] = 0; code_[p] = 0; prev_[p] = 0; line_on_[p] = 0; used_[p] = 0; thprev_[p] = 0;
		nused_[p] = settle_[p] = need_[p] = have_gid_[p] = 0; gid_[p] = 0; span_base_[p] = span_len_[p] = 0;
		ovr_code_[p] = 0; ovr_gen_[p] = 0; real0_[p] = 0;
	}
	memset(&job_, 0, sizeof job_);
	memset(&st_, 0, sizeof st_);
	memset(lastreal_, 0, sizeof lastreal_);
	memset(cram_prev_, 0, sizeof cram_prev_);
	memset(ovr_row_, 0, sizeof ovr_row_);
	memset(map_runs_, 0, sizeof map_runs_);
	memset(rowok_, 0, sizeof rowok_);
}

Wide::~Wide() { sync(); free_all(); }

void Wide::free_all()
{
	for (int i = 0; i < COMPOSE_WHO; i++) { if (scr_[i]) wk_compose_scratch_destroy(scr_[i]); scr_[i] = 0; }
	free(ci_); ci_ = 0; free(edge_); edge_ = 0;
	if (C_) wk_compose_destroy(C_);
	if (L_) wk_layers_destroy(L_);
	if (M_) wk_map_destroy(M_);
	C_ = 0; L_ = 0; M_ = 0;
	free(fr_); free(out_); free(ana2_); free(wide_); free(prov_); free(cand_[0]); free(cand_[1]); free(opq_[0]); free(opq_[1]);
	free(th_); free(thok_); free(rowbuf_); free(claims_); claims_ = 0;
	fr_ = 0; out_ = 0; ana2_ = 0; wide_ = 0; prov_ = 0; cand_[0] = cand_[1] = 0; opq_[0] = opq_[1] = 0; th_ = 0; thok_ = 0; rowbuf_ = 0;
	for (int p = 0; p < 2; p++) {
		free(hash_[p]); free(stamp_[p]); free(wstamp_[p]); free(code_[p]); free(prev_[p]); free(line_on_[p]); free(used_[p]); free(thprev_[p]);
		free(ovr_code_[p]); free(ovr_gen_[p]); free(real0_[p]);
		hash_[p] = 0; stamp_[p] = 0; wstamp_[p] = 0; code_[p] = 0; prev_[p] = 0; line_on_[p] = 0; used_[p] = 0; thprev_[p] = 0;
		ovr_code_[p] = 0; ovr_gen_[p] = 0; real0_[p] = 0;
	}
	cvw_ = cvh_ = cm_ = 0;
}

void Wide::sync() { md_host_wide_join(); }

static void clear_rows_hook(void *, const md_wide_frame *) { md_host_wide_rows_clear(); }

void Wide::reset()
{
	sync();
	md_host_wide_run(clear_rows_hook, 0);       /* the VDP forgets the learned rows (on its own thread) */
	sync();
	if (L_) wk_layers_reset(L_);
	if (M_) wk_map_clear(M_);
	map_key_ok_ = 0;
	have_prev_ = 0; layout_key_ = 0; have_cram_ = 0;
	ovr_live_ = 0;
	memset(ovr_row_, 0, sizeof ovr_row_);
	for (int p = 0; p < 2; p++) { if (wstamp_[p]) memset(wstamp_[p], 0, RING_MAX * sizeof(uint32_t)); have_gid_[p] = 0; settle_[p] = 0; }
	planes_cur_ = planes_next_ = 3; md_host_set_wide_planes(3);
}

void Wide::ensure_layers()
{
	if (L_) return;
	wk_layers_cfg lc;
	wk_layers_cfg_default(&lc);
	L_ = wk_layers_create(&lc);
	fr_ = (wk_frame *) calloc(1, sizeof(wk_frame));
	out_ = (wk_out *) calloc(1, sizeof(wk_out));
	th_ = (uint32_t *) calloc(NTILE, sizeof(uint32_t));
	thok_ = (uint8_t *) calloc(NTILE, 1);
	rowbuf_ = (uint16_t *) calloc(2 * RING_MAX, sizeof(uint16_t));
	for (int p = 0; p < 2; p++) {
		hash_[p] = (uint32_t *) calloc(RING_MAX, sizeof(uint32_t));
		stamp_[p] = (uint32_t *) calloc(RING_MAX, sizeof(uint32_t));
		wstamp_[p] = (uint32_t *) calloc(RING_MAX, sizeof(uint32_t));
		code_[p] = (uint32_t *) calloc(RING_MAX, sizeof(uint32_t));
		prev_[p] = (uint16_t *) calloc(RING_MAX, sizeof(uint16_t));
		line_on_[p] = (uint8_t *) calloc(256, 1);
		used_[p] = (uint8_t *) calloc(NTILE, 1);
		thprev_[p] = (uint32_t *) calloc(NTILE, sizeof(uint32_t));
		ovr_code_[p] = (uint16_t *) calloc(RING_MAX, sizeof(uint16_t));
		ovr_gen_[p] = (uint32_t *) calloc(RING_MAX, sizeof(uint32_t));
		real0_[p] = (uint16_t *) calloc(256, sizeof(uint16_t));
	}
}

/* The learned map: what makes a remembered cell safe to show is the Super NES adapter's set of rules (snes/WIDE-INTEGRATION.md, "Learned map
 * safety"): a room binds only when 98 percent of the cells it has match for 8 frames in a row, by name table word; a room the picture
 * contradicts is dropped at once; a cell is served only if it was seen on screen since the room was bound (live). On the Mega Drive the two
 * visible tiles next to the seam must be the room's too; a Master System ring is only as wide as the screen and the column at the seam is the
 * one being rewritten, so that test cannot be made there (as on the NES). */
void Wide::ensure_map(int system)
{
	if (!learn_ || system == MD_SYS_SG) return;
	if (M_ && map_system_ == system) return;
	if (M_) { wk_map_destroy(M_); M_ = 0; }
	wk_map_cfg mc;
	wk_map_cfg_default(&mc);
	mc.max_cells = 32768;
	mc.bind_pct = 98; mc.bind_frames = 8;
	mc.match_by_code = 1;
	mc.drop_on_revoke = 1;
	mc.verify_pct = 98; mc.revoke_frames = 1;
	mc.live_flags = 1;
	mc.seam_tiles = system == MD_SYS_MD ? 2 : 0;
	M_ = wk_map_create(&mc, crc_);
	map_system_ = system;
}

/* the content hash of a 32 byte tile (kept until the tile is written) */
uint32_t Wide::tile_hash(const uint8_t *vram, unsigned t)
{
	if (thok_[t]) return th_[t];
	uint32_t w[8], h = 0x811C9DC5u;
	memcpy(w, vram + t * 32, 32);
	for (int i = 0; i < 8; i++) h = mix32(h, w[i]);
	th_[t] = h | 1u; thok_[t] = 1;
	return th_[t];
}

/* which planes must read their name table again: a block of it was written, a tile it uses was written, cells are settling */
void Wide::take_dirty(const md_wide_frame &f)
{
	need_[0] = settle_[0]; need_[1] = settle_[1];
	if (f.dirty_all || !have_prev_) { memset(thok_, 0, NTILE); need_[0] = need_[1] = 1; return; }
	for (int i = 0; i < f.ndirty; i++) {
		const unsigned d = f.dirty[i] & (NTILE - 1), a = d * 32;
		thok_[d] = 0;
		for (int p = 0; p < 2; p++) {
			if (used_[p][d]) need_[p] = 1;
			if (span_len_[p] && (((a - span_base_[p]) & 0xFFFF) < span_len_[p] || ((a + 31 - span_base_[p]) & 0xFFFF) < span_len_[p])) need_[p] = 1;
		}
	}
}

/* Read plane p's name table: the words, the frame each cell was last written (noticed here: a cell whose word differs from the last pass), and
 * for widekit the content hash (word + the tile's graphics), the redraw code and the stamp. A cell written since the last analysed frame is
 * settling: the picture just analysed was drawn before the write (the games write in the vertical blank), so it is neither history nor a
 * lead yet and the map does not record it. */
void Wide::pass_plane(const md_wide_frame &f, int p, unsigned base, unsigned shift, unsigned tile_mask, int mw, int mh, uint32_t frame, bool m4)
{
	uint32_t *hh = hash_[p], *ss = stamp_[p], *ws = wstamp_[p], *cc = code_[p], *tp = thprev_[p];
	uint16_t *pv = prev_[p];
	uint8_t *us = used_[p];
	const uint8_t *vram = f.vram;
	int nused = 0, same = 0, settle = 0;
	uint16_t list[NTILE];
	memset(us, 0, NTILE);
	for (int ry = 0; ry < mh; ry++) {
		const unsigned rowaddr = m4 ? ((0x3800u + ((unsigned) ry << 6)) & base) : (base + (((unsigned) ry << shift) & 0x1FC0u));
		for (int rx = 0; rx < mw; rx++) {
			const unsigned addr = (rowaddr + (unsigned) rx * 2) & 0xFFFE;
			uint16_t w;
			if (m4) w = (uint16_t) (vram[addr] | (vram[addr + 1] << 8));
			else memcpy(&w, vram + addr, 2);
			const int k = ry * mw + rx;
			if (have_prev_ && pv[k] != w) ws[k] = frame;
			pv[k] = w;
			const unsigned t = w & tile_mask;
			const uint32_t th = tile_hash(vram, t);
			if (!us[t]) { us[t] = 1; list[nused++] = (uint16_t) t; if (tp[t] == th) same++; }
			uint32_t h = cell_hash(w, th);
			if (have_prev_ && ws[k] == frame) {
				h ^= 0x7F4A7C15u * (frame | 1u);
				ss[k] = 0xFFFFFFFFu; cc[k] = WK_CODE_NOREC;
				settle = 1;
			} else { ss[k] = ws[k]; cc[k] = w; }
			hh[k] = h;
		}
	}
	/* the identity of the plane's graphics: kept through cosmetic animation (a few tiles changing), new when a tile set is loaded (a third of
	 * the tiles in use and at least 8): widekit then takes the frame for a new scene */
	{
		const int changed = nused - same;
		if (!have_gid_[p] || (changed * 3 >= nused && changed >= 8)) {
			uint32_t g = 0x9E3779B1u + (uint32_t) p;
			for (int i = 0; i < nused; i++) g = mix32(g, th_[list[i]]);
			gid_[p] = g; have_gid_[p] = 1;
		}
		memset(tp, 0, NTILE * sizeof(uint32_t));
		for (int i = 0; i < nused; i++) tp[list[i]] = th_[list[i]];
	}
	nused_[p] = nused;
	settle_[p] = settle;
}

bool Wide::describe_md(const md_wide_frame &f, const md_video_info &vi, uint32_t frame, int margin)
{
	const int H = vi.h;
	int first = -1, y;
	for (y = 0; y < H && y < 256; y++) if (f.line[y].flags & MDW_ON) { first = y; break; }
	memset(fr_, 0, sizeof *fr_);
	fr_->frame = frame; fr_->view_w = vi.w; fr_->view_h = H; fr_->margin = margin;
	fr_->brightness = first >= 0 ? 255 : 0;
	fr_->palette_size = 64;
	if (first < 0) return false;
	const md_wide_line &l0 = f.line[first];
	const int mw = ((int) l0.col_mask + 1) * 2, mh = ((int) l0.row_mask + 1) >> 3;
	if (mw * mh > RING_MAX || mw < 32 || mh < 32 || mh > 128) return false;
	const int key = (int) (mix32(mix32(l0.ntab, l0.ntbb), (uint32_t) (mw * 977 + mh * 13 + l0.shift)) & 0x7FFFFFFF) | 1;
	if (key != layout_key_) {
		wk_layers_reset(L_);
		have_prev_ = 0; layout_key_ = key;
		for (int p = 0; p < 2; p++) { memset(wstamp_[p], 0, RING_MAX * sizeof(uint32_t)); have_gid_[p] = 0; memset(thprev_[p], 0, NTILE * sizeof(uint32_t)); }
	}
	map_w_ = mw; map_h_ = mh;
	{
		int delta = 0;
		if (have_cram_) for (int i = 0; i < 128; i += 2) if (memcmp(f.cram + i, cram_prev_ + i, 2)) delta++;
		memcpy(cram_prev_, f.cram, 128); have_cram_ = 1;
		fr_->palette_delta = delta;
	}
	take_dirty(f);
	for (int p = 0; p < 2; p++) {
		const unsigned base = p == 0 ? l0.ntab : l0.ntbb;
		span_base_[p] = base; span_len_[p] = ((((unsigned) (mh - 1)) << l0.shift) & 0x1FC0u) + (unsigned) mw * 2;
		if (need_[p]) pass_plane(f, p, base, l0.shift, 0x7FF, mw, mh, frame, false);
		wk_layer *L = &fr_->layer[p];
		L->active = 1; L->role = WK_ROLE_AUTO; L->priority = p == 0 ? 1 : 0;
		L->map_w = mw; L->map_h = mh; L->tile_px = 8;
		L->hash = hash_[p]; L->code = code_[p]; L->stamp = stamp_[p]; L->map_by_cell = 1;
		L->gfx_id = gid_[p] ^ (uint32_t) (mw * 977 + mh * 131 + p);
		L->line_on = line_on_[p];
	}
	have_prev_ = 1;
	const int ringpx = mw * 8, rowmask = l0.row_mask;
	int on_n = 0;
	for (y = 0; y < H && y < 256; y++) {
		const md_wide_line &r = f.line[y];
		/* a plane is described on the lines that show it: not plane A under a window that takes the line. Where the columns scroll apart
		 * vertically (per 2-cell scrolling) the line is described by the columns at the edge that has a strip (the left one when both have):
		 * that is the row the strip continues; the other side's strip counts only if its edge shows the same row. */
		const int base = (r.flags & MDW_ON) && !(r.flags & MDW_IM2) && r.ntab == l0.ntab && r.ntbb == l0.ntbb &&
		                 r.col_mask == l0.col_mask && r.shift == l0.shift && r.row_mask == l0.row_mask;
		const int vs = (r.flags & MDW_VSCROLL) != 0;
		const int sl = (r.side & MDS_L) != 0, sr = (r.side & MDS_R) != 0;
		const int same = base && (!vs || sl || sr);
		const int from_right = vs && !sl && sr;
		line_on_[0][y] = (uint8_t) (same && !(r.flags & MDW_WFULL) ? 1 : 0);
		line_on_[1][y] = (uint8_t) (same ? 1 : 0);
		for (int p = 0; p < 2; p++) {
			wk_layer *L = &fr_->layer[p];
			const int ys = from_right ? r.ysr[p] : r.ys[p];
			L->scroll_x[y] = imod(-(int) (r.xs[p] & (uint16_t) (ringpx - 1)), ringpx);
			L->row_y[y] = (y + ys) & rowmask;
			/* bit s: the strip of side s shows the row the plane is described with */
			rowok_[p][y] = (uint8_t) (!vs ? 3 : from_right ? 2 : (1 | ((((int) r.ysr[p] ^ (int) r.ys[p]) & rowmask) == 0 ? 2 : 0)));
		}
		on_n += same;
	}
	fr_->nlayers = 2;
	return on_n > 0;
}

/* The mode 4 consoles: one layer, a ring of 32 x 28 cells. Master System: the view is the ring's whole width. Game Gear: the view is the
 * LCD's window, 160 x 144 at (48, 24) of the VDP's picture. */
bool Wide::describe_m4(const md_wide_frame &f, const md_video_info &vi, uint32_t frame, int margin, bool gg)
{
	const int H = gg ? 144 : vi.h, VY0 = gg ? 24 : 0, VW = gg ? 160 : 256, X0 = gg ? 48 : 0;
	int first = -1, y;
	memset(fr_, 0, sizeof *fr_);
	fr_->frame = frame; fr_->view_w = VW; fr_->view_h = H; fr_->margin = margin;
	fr_->palette_size = 32;
	for (y = 0; y < H; y++) if ((f.line[y + VY0].flags & (MDW_ON | MDW_M4)) == (MDW_ON | MDW_M4)) { first = y; break; }
	fr_->brightness = first >= 0 ? 255 : 0;
	if (first < 0) return false;
	if (f.h != 192) return false;                      /* the 224 / 240 line modes keep their name table elsewhere: not described */
	if (!gg && f.sms1 && ((f.reg[3] & 0xFF) != 0xFF || (f.reg[4] & 7) != 7)) return false;   /* the first VDP's tile index masks in use */
	const md_wide_line &l0 = f.line[first + VY0];
	const unsigned nt_mask = (unsigned) l0.ntab & 0xFFFFu;
	const int mw = 32, mh = 28;
	const int key = (int) (mix32(nt_mask, gg ? 0x6767u : 0x5353u) & 0x7FFFFFFF) | 1;
	if (key != layout_key_) {
		wk_layers_reset(L_);
		have_prev_ = 0; layout_key_ = key;
		memset(wstamp_[0], 0, RING_MAX * sizeof(uint32_t)); have_gid_[0] = 0; memset(thprev_[0], 0, NTILE * sizeof(uint32_t));
	}
	map_w_ = mw; map_h_ = mh;
	{
		int delta = 0;
		if (have_cram_) for (int i = 0; i < 64; i += 2) if (memcmp(f.cram + i, cram_prev_ + i, 2)) delta++;
		memcpy(cram_prev_, f.cram, 128); have_cram_ = 1;
		fr_->palette_delta = delta;
	}
	wk_layer *L = &fr_->layer[0];
	int on_n = 0;
	uint32_t seam = 0;
	const int blank = (f.reg[0] & 0x20) != 0;
	for (y = 0; y < H; y++) {
		const md_wide_line &r = f.line[y + VY0];
		const int same = (r.flags & (MDW_ON | MDW_M4)) == (MDW_ON | MDW_M4) && !(r.flags & MDW_VSCROLL) && r.ntab == l0.ntab;
		line_on_[0][y] = (uint8_t) (same ? 1 : 0);
		L->scroll_x[y] = imod(X0 - (int) (r.xs[0] & 255), 256);
		L->row_y[y] = imod(y + VY0 + (int) r.ys[0], 224);
		on_n += same;
		/* Master System: the ring's column at the left edge of the screen is the one the game rewrites while part of it is still on screen at the
		 * right (the VDP does not draw it at the left: the partly shown column is blank, and the first eight pixels are when the game masks
		 * them): what the ring holds there is not the world column it stands at, the map must not record it */
		if (same && !gg && (blank || (L->scroll_x[y] & 7))) seam |= 1u << ((L->scroll_x[y] >> 3) & 31);
	}
	/* lines that are off follow the line above so that they do not look like a scroll difference */
	{
		int last = -1, firston = 0;
		while (firston < H && !line_on_[0][firston]) firston++;
		for (y = 0; y < H; y++) {
			if (line_on_[0][y]) { last = y; continue; }
			const int j = last >= 0 ? last : firston;
			if (j < H) { L->scroll_x[y] = L->scroll_x[j]; L->row_y[y] = L->row_y[j]; }
		}
	}
	take_dirty(f);
	if (seam != seam_prev_) need_[0] = 1;
	span_base_[0] = 0x3800u & nt_mask; span_len_[0] = 28 * 64;
	if (need_[0]) {
		pass_plane(f, 0, nt_mask, 0, 0x1FF, mw, mh, frame, true);
		for (int c = 0; c < 32; c++) if ((seam >> c) & 1) for (int r2 = 0; r2 < mh; r2++) code_[0][r2 * mw + c] = WK_CODE_NOREC;
		seam_prev_ = seam;
	}
	have_prev_ = 1;
	L->active = 1; L->role = WK_ROLE_AUTO; L->priority = 0;
	L->map_w = mw; L->map_h = mh; L->tile_px = 8;
	L->hash = hash_[0]; L->code = code_[0]; L->stamp = stamp_[0]; L->map_by_cell = 1;
	L->gfx_id = gid_[0] ^ (gg ? 77u : 55u);
	L->line_on = line_on_[0];
	fr_->nlayers = 1;
	return on_n > 0;
}

/* a learned cell may be shown: seen on screen since its room was bound, and its tile's graphics are what they were when it was recorded */
bool Wide::cell_ok(const md_wide_frame &f, const wk_cell &c, unsigned tile_mask)
{
	if (!c.live || c.scout || c.code == WK_CODE_NOREC) return false;
	return fold16(cell_hash(c.code, tile_hash(f.vram, c.code & tile_mask))) == c.h16;
}

/* Mega Drive: is every strip pixel widekit and the map call real the pixel the VDP drew? The strips of the frame were drawn from the VRAM rows,
 * or from the learned rows given after the last analysed frame (the line's record says which). Per layer, side and tile beside the view: the
 * word the strip was drawn from must be the word that is claimed (the ring's own inside the ring's extent, the map's cell beyond it). The
 * first tile that fails ends the real run. */
void Wide::verify_md(const md_wide_frame &f, int H)
{
	const int M = out_->margin, mw = map_w_, mh = map_h_;
	for (int l = 0; l < 2; l++) {
		if (out_->kind[l] != WK_KIND_LEVEL) continue;
		const int obit = l == 0 ? MDS_OVR_A : MDS_OVR_B;
		int have = 0, p_wx = 0, p_wr = 0, p_ovr = 0, p_ring[2] = { 0, 0 }, lim[2] = { 0, 0 };
		for (int y = 0; y < H && y < 256; y++) {
			if (!out_->coherent[l][y] || !line_on_[l][y]) continue;
			wk_line_out &lo = out_->line[y];
			const int rowovr = (f.line[y].side & obit) != 0;
			if (!rowovr && lo.real[0][l] <= lo.ring[0][l] && lo.real[1][l] <= lo.ring[1][l]) continue;
			const int wx = out_->world_x[l][y], wr = fdiv(out_->world_row[l][y], 8);
			if (!(have && wx == p_wx && wr == p_wr && rowovr == p_ovr && lo.ring[0][l] == p_ring[0] && lo.ring[1][l] == p_ring[1])) {
				wk_geom g;
				wk_line_geom(fr_, out_, l, y, &g);
				const int rr = imod(wr, mh);
				for (int s = 0; s < 2; s++) {
					int v = M;
					if (rowovr && !ovr_live_) v = 0;          /* drawn from rows this analysis knows nothing of */
					for (int i = 0; v == M; i++) {
						const int a = i == 0 ? 0 : g.part[s] + (i - 1) * 8, e = g.part[s] + i * 8;
						if (i == 0 && g.part[s] == 0) continue;
						if (a >= M) break;
						const int col = s == 0 ? g.tile_first - i : g.tile_first + g.nvis - 1 + i;
						const int k = rr * mw + imod(col, mw);
						const uint32_t w = prev_[l][k];
						const uint32_t drawn = (rowovr && ovr_gen_[l][k] == ovr_live_) ? ovr_code_[l][k] : w;
						uint32_t claimed = w;
						if (i > 0 && e > (int) lo.ring[s][l]) {
							wk_cell c;
							if (a >= (int) lo.real[s][l]) break;      /* nothing is claimed from here on */
							if (!M_ || !wk_map_margin_cell(M_, fr_, out_, l, y, s, i, &c) || !cell_ok(f, c, 0x7FF)) { v = a; st_.map_rejected++; break; }
							claimed = c.code;
							if (check_ && drawn == claimed) {
								Claim *q = claim_find(l, col, wr, true);
								if (q && q->state == 0) { q->state = 1; q->code = (uint16_t) c.code; st_.chk_claims++; }
							}
						}
						if (drawn != claimed) { v = a; st_.map_rejected++; break; }
					}
					lim[s] = v;
				}
				have = 1; p_wx = wx; p_wr = wr; p_ovr = rowovr; p_ring[0] = lo.ring[0][l]; p_ring[1] = lo.ring[1][l];
			}
			for (int s = 0; s < 2; s++) if ((int) lo.real[s][l] > lim[s]) {
				lo.real[s][l] = (uint16_t) lim[s];
				if ((int) lo.ring[s][l] > lim[s]) lo.ring[s][l] = (uint16_t) lim[s];
			}
			wk_out_refresh_line(out_, y);
		}
	}
}

/* tests: the learned cells shown as real, by layer and world cell, for the hindsight check */
Wide::Claim *Wide::claim_find(int l, int col, int row, bool add)
{
	enum { N = 1 << 16 };
	if (!claims_) { if (!add) return 0; claims_ = (Claim *) calloc(N, sizeof(Claim)); if (!claims_) return 0; }
	const uint32_t ep = out_->epoch + 1;
	uint32_t h = mix32(mix32((uint32_t) col, (uint32_t) row), (uint32_t) l) & (N - 1);
	for (int n = 0; n < 64; n++, h = (h + 1) & (N - 1)) {
		Claim *q = &claims_[h];
		if (q->state && q->epoch == ep && q->col == col && q->row == row && q->l == l) return q;
		if (!q->state || q->epoch != ep) {
			if (!add) return 0;
			q->col = col; q->row = row; q->epoch = ep; q->l = (uint8_t) l; q->state = 0; q->code = 0;
			return q;
		}
	}
	return 0;
}

/* tests: every cell the screen shows now that was shown beside the screen from the learned map before: is it the same cell? */
void Wide::check_seen(int H)
{
	if (!claims_) return;
	const int mw = map_w_, mh = map_h_;
	for (int l = 0; l < 2; l++) {
		if (out_->kind[l] != WK_KIND_LEVEL) continue;
		int have = 0, p_wx = 0, p_wr = 0;
		for (int y = 0; y < H && y < 256; y++) {
			if (!out_->coherent[l][y] || !line_on_[l][y]) continue;
			const int wx = out_->world_x[l][y], wr = fdiv(out_->world_row[l][y], 8);
			if (have && wx == p_wx && wr == p_wr) continue;
			have = 1; p_wx = wx; p_wr = wr;
			wk_geom g;
			wk_line_geom(fr_, out_, l, y, &g);
			const int rr = imod(wr, mh);
			for (int i = 0; i < g.nvis; i++) {
				const int col = g.tile_first + i, k = rr * mw + imod(col, mw);
				if (code_[l][k] == WK_CODE_NOREC) continue;
				Claim *q = claim_find(l, col, wr, false);
				if (!q || q->state != 1) continue;
				q->state = 2;
				st_.chk_seen++;
				if (q->code != prev_[l][k]) st_.chk_wrong++;
			}
		}
	}
}

/* Mega Drive: the learned rows for the frames to come. For every row of a level plane the screen shows, the cells the map holds beside the
 * view (as far as the margin and two tiles more, for the camera's next steps) are written into a copy of the row where the VRAM holds
 * something else, and the VDP draws its strips from that copy for as long as the game leaves the VRAM row alone. */
void Wide::rows_md(const md_wide_frame &f, int H)
{
	const int M = out_->margin, mw = map_w_, mh = map_h_;
	md_host_wide_rows_clear();
	memset(ovr_row_, 0, sizeof ovr_row_);
	ovr_next_++;
	if (!ovr_next_) ovr_next_ = 1;
	ovr_live_ = ovr_next_;
	st_.rows = 0;
	if (!M_ || mh > 128) return;
	const md_wide_line *l0 = 0;
	for (int y = 0; y < H && y < 256; y++) if (f.line[y].flags & MDW_ON) { l0 = &f.line[y]; break; }
	if (!l0) return;
	for (int l = 0; l < 2; l++) {
		if (out_->kind[l] != WK_KIND_LEVEL || !((planes_next_ >> l) & 1) || map_runs_[l] > MAP_RUNS) continue;
		int have = 0, p_wx = 0, p_wr = 0;
		for (int y = 0; y < H && y < 256; y++) {
			if (!out_->coherent[l][y] || !line_on_[l][y]) continue;
			const int wx = out_->world_x[l][y], wr = fdiv(out_->world_row[l][y], 8);
			if (have && wx == p_wx && wr == p_wr) continue;
			have = 1; p_wx = wx; p_wr = wr;
			wk_geom g;
			wk_line_geom(fr_, out_, l, y, &g);
			const int rr = imod(wr, mh);
			for (int s = 0; s < 2; s++) {
				for (int i = 1; g.part[s] + (i - 1) * 8 < M + 16; i++) {
					wk_cell c;
					if (!wk_map_margin_cell(M_, fr_, out_, l, y, s, i, &c) || !cell_ok(f, c, 0x7FF)) break;
					const int col = s == 0 ? g.tile_first - i : g.tile_first + g.nvis - 1 + i;
					const int k = rr * mw + imod(col, mw);
					if (c.code == prev_[l][k]) continue;          /* the ring holds it already */
					uint16_t *row = rowbuf_ + l * RING_MAX + rr * mw;
					if (!ovr_row_[l][rr]) { memcpy(row, prev_[l] + rr * mw, (size_t) mw * 2); ovr_row_[l][rr] = 1; }
					row[imod(col, mw)] = (uint16_t) c.code;
					ovr_code_[l][k] = (uint16_t) c.code; ovr_gen_[l][k] = ovr_next_;
				}
			}
		}
		const unsigned base = l == 0 ? l0->ntab : l0->ntbb;
		for (int rr = 0; rr < mh; rr++) if (ovr_row_[l][rr]) {
			md_host_wide_row(l, rr, base + (((unsigned) rr << l0->shift) & 0x1FC0u), rowbuf_ + l * RING_MAX + rr * mw, mw);
			st_.rows++;
		}
	}
}

/* Mega Drive: what is real of each strip. A strip holds the planes the VDP drew into it (the planes widekit trusted two frames ago; not
 * plane A on the side of a line the window plane takes) and the sprites; it is real as far as every plane in it is. */
void Wide::decide_md(const md_wide_frame &f, Meta *m, int H)
{
	const int M = m->margin;
	int allop = 1;
	unsigned mappx = 0;
	for (int y = 0; y < H && y < 256; y++) {
		const md_wide_line &r = f.line[y];
		const wk_line_out &lo = out_->line[y];
		m->hud[y] = lo.hud;
		for (int s = 0; s < 2; s++) {
			int v = 0;
			if ((r.flags & MDW_STRIP) && (r.side & (MDS_L << s)) && !lo.hud) {
				int mask = m->planes & 3;
				if (r.side & (MDS_NOA_L << s)) mask &= ~1;
				v = M;
				for (int l = 0; l < 2; l++) if ((mask >> l) & 1) {
					const int k = out_->kind[l];
					int x = 0;
					if (k == WK_KIND_BACKDROP) x = M;
					else if ((k == WK_KIND_LEVEL || k == WK_KIND_UNKNOWN) && line_on_[l][y] && ((rowok_[l][y] >> s) & 1)) { x = lo.real[s][l]; if (x > (int) lo.ring[s][l]) mappx += (unsigned) (x - lo.ring[s][l]); }
					if (x < v) v = x;
				}
				if (v < 0) v = 0;
				if (v > M) v = M;
			}
			m->real[s][y] = m->pic[s][y] = (uint16_t) v;
			if (v > 0 && (r.side & (MDS_NOA_L << s))) st_.win_sides++;
			if (v > 0 && (r.side & MDS_VSE)) st_.vs_sides++;
			if (v > 0 && allop) {
				const uint32_t *ob = f.opq + ((size_t) y * 2 + (size_t) s) * 3;
				for (int d = 0; d < v; d += 32) {
					const uint32_t need = v - d >= 32 ? 0xFFFFFFFFu : ((1u << (v - d)) - 1u);
					if ((ob[d >> 5] & need) != need) { allop = 0; break; }
				}
			}
		}
	}
	m->all_opaque = (uint8_t) allop;
	st_.map_px = mappx;
}

/* The mode 4 consoles: the strips. Game Gear: what the VDP drew beside the LCD's window as far as the ring can be trusted (m->pic), then
 * learned cells; Master System: learned cells only (a layer widekit finds to wrap, a backdrop, is continued from the ring itself: that is
 * what the console shows when such a picture scrolls). Learned cells are drawn here from the VRAM's tiles and the frame's palette. */
void Wide::strips_m4(const md_wide_frame &f, Meta *m, bool gg)
{
	const int M = m->margin, H = m->vh, V = m->vw, VY0 = gg ? 24 : 0, mw = map_w_, mh = map_h_;
	const size_t need = (size_t) 2 * (size_t) H * (size_t) M;
	if (m->px_cap < (int) need) { free(m->px); m->px = (uint16_t *) malloc(need * 2); m->px_cap = m->px ? (int) need : 0; }
	memset(m->real, 0, sizeof m->real); memset(m->pic, 0, sizeof m->pic);
	if (!m->px) return;
	const int kind = out_->kind[0];
	const int blank = (f.reg[0] & 0x20) != 0;
	m->mask_left = (uint8_t) (!gg && blank);
	const uint16_t *pal = f.pal;
	const uint8_t *vram = f.vram;
	enum { NT = WK_MAX_MARGIN / 8 + 3 };
	uint8_t memo[2][NT];           /* 0 not asked, 1 no, 2 yes */
	uint32_t mcode[2][NT];
	int have = 0, p_wx = 0, p_wr = 0;
	unsigned mappx = 0;
	for (int y = 0; y < H; y++) {
		const md_wide_line &r = f.line[y + VY0];
		const wk_line_out &lo = out_->line[y];
		m->hud[y] = lo.hud;
		if (!line_on_[0][y] || lo.hud || (r.side & MDS_PAL) || !out_->coherent[0][y]) continue;
		const int wx = out_->world_x[0][y], wrow = out_->world_row[0][y], wr = fdiv(wrow, 8), fy = wrow & 7, rr = imod(wr, mh);
		if (!(have && wx == p_wx && wr == p_wr)) { memset(memo, 0, sizeof memo); have = 1; p_wx = wx; p_wr = wr; }
		wk_geom g;
		wk_line_geom(fr_, out_, 0, y, &g);
		const int back = kind == WK_KIND_BACKDROP || ((lo.backdrop_mask & 1) != 0);
		for (int s = 0; s < 2; s++) {
			uint16_t *dst = m->px + ((size_t) s * (size_t) H + (size_t) y) * (size_t) M;
			int r0 = 0;
			if (gg) {
				/* the band: not the columns the VDP leaves blank at its left edge */
				r0 = real0_[s][y] < M ? real0_[s][y] : M;
				if (s == 0) { const int cut = 48 - ((int) (r.xs[0] & 7) > (blank ? 8 : 0) ? (int) (r.xs[0] & 7) : (blank ? 8 : 0)); if (r0 > cut) r0 = cut; }
			}
			int d = r0, i = 0;
			while (d < M) {
				const int xw = s == 0 ? wx - 1 - d : wx + V + d;
				const int col = fdiv(xw, 8);
				uint32_t code;
				if (back && !gg) code = prev_[0][rr * mw + imod(col, mw)];
				else {
					if (kind != WK_KIND_LEVEL && kind != WK_KIND_UNKNOWN) break;
					i = s == 0 ? g.tile_first - col : col - (g.tile_first + g.nvis - 1);
					if (i < 0 || i >= NT || !M_) break;
					if (!memo[s][i]) {
						wk_cell c;
						memo[s][i] = (uint8_t) ((wk_map_margin_cell(M_, fr_, out_, 0, y, s, i, &c) && cell_ok(f, c, 0x1FF)) ? 2 : 1);
						mcode[s][i] = c.code;
					}
					if (memo[s][i] != 2) break;
					code = mcode[s][i];
				}
				/* the tile's row of eight pixels */
				const unsigned t = code & 0x1FF;
				const uint8_t *b = vram + ((t * 32 + (unsigned) ((code & 0x400) ? 7 - fy : fy) * 4) & 0x3FFF);
				const unsigned pb = ((code >> 11) & 1) << 4;
				int x = imod(xw, 8);
				/* this tile's pixels from xw outwards: leftwards on the left side, rightwards on the right */
				for (;;) {
					const int bit = (code & 0x200) ? x : 7 - x;
					const unsigned ci = ((b[0] >> bit) & 1) | (((b[1] >> bit) & 1) << 1) | (((b[2] >> bit) & 1) << 2) | (((b[3] >> bit) & 1) << 3);
					dst[d++] = pal[pb | ci];
					if (d >= M) break;
					if (s == 0) { if (x == 0) break; x--; } else { if (x == 7) break; x++; }
				}
			}
			if (d > M) d = M;
			m->real[s][y] = (uint16_t) d;
			m->pic[s][y] = (uint16_t) r0;
			mappx += (unsigned) (d - r0);
		}
		/* the masked first eight columns of a line whose left strip is real: the same learned picture (the column under the mask from the map,
		 * the next one is on screen and in the ring) */
		if (!gg && blank && m->real[0][y] > 0 && !back && M_) {
			int ok = 1;
			for (int x = 0; x < 8 && ok; x++) {
				const int xw = wx + x, col = fdiv(xw, 8);
				uint32_t code;
				if (col == g.tile_first) {
					if (!memo[0][0]) {
						wk_cell c;
						memo[0][0] = (uint8_t) ((wk_map_margin_cell(M_, fr_, out_, 0, y, 0, 0, &c) && cell_ok(f, c, 0x1FF)) ? 2 : 1);
						mcode[0][0] = c.code;
					}
					if (memo[0][0] != 2) { ok = 0; break; }
					code = mcode[0][0];
				} else code = prev_[0][rr * mw + imod(col, mw)];
				const unsigned t = code & 0x1FF;
				const uint8_t *b = vram + ((t * 32 + (unsigned) ((code & 0x400) ? 7 - fy : fy) * 4) & 0x3FFF);
				const int px = xw & 7, bit = (code & 0x200) ? px : 7 - px;
				const unsigned ci = ((b[0] >> bit) & 1) | (((b[1] >> bit) & 1) << 1) | (((b[2] >> bit) & 1) << 2) | (((b[3] >> bit) & 1) << 3);
				m->bar[y][x] = pal[(((code >> 11) & 1) << 4) | ci];
			}
			m->bar_ok[y] = (uint8_t) ok;
		}
	}
	st_.map_px = mappx;
}

void Wide::hook(void *user, const md_wide_frame *f) { ((Wide *) user)->run(*f); }

void Wide::frame(uint32_t frame, const md_video_info &vi, int margin, int strip_px, Meta *m)
{
	sync();
	st_.frames++;
	m->ready.store(0, std::memory_order_relaxed);
	m->view_off = 0;                       /* the caller's field: set after this call, never touched by the analysis */
	job_.frame = frame; job_.vi = vi; job_.margin = margin; job_.strip_px = strip_px; job_.cap = cap_default_; job_.m = m;
	job_.planes = planes_cur_;
	crc_ = md_host_rom_crc();
	/* the planes the last analysis trusts go into the strips from the next frame on (the same latency whichever thread analyses) */
	if (planes_next_ != planes_cur_) { planes_cur_ = planes_next_; md_host_set_wide_planes(planes_cur_); }
	md_host_wide_run(hook, this);
}

void Wide::run(const md_wide_frame &f)
{
	const md_video_info &vi = job_.vi;
	Meta *m = job_.m;
	int margin = job_.margin;
	const uint32_t frame = job_.frame;
	m->valid = 0; m->system = vi.system; m->vw = vi.w; m->vh = vi.h; m->margin = margin; m->real_cap = margin < job_.cap ? margin : job_.cap;
	m->have_strips = 0; m->own_strips = 0; m->nlayers = 0; m->frame = frame;
	m->planes = (uint8_t) (vi.system == MD_SYS_MD ? job_.planes : 3);
	m->all_opaque = 1; m->epoch = 0; m->scene_event = 0;
	m->mask_left = 0; memset(m->bar_ok, 0, sizeof m->bar_ok);
	do {
		if (margin <= 0 || vi.h <= 0 || vi.h > 256 || vi.w <= 0 || !f.line) break;
		if (vi.interlace == 2) break;
		if (vi.system == MD_SYS_GG) {
			if (!vi.gg_band) break;
			m->vw = 160; m->vh = 144; m->margin = margin = 48;
			if (m->real_cap > margin) m->real_cap = margin;
		}
		ensure_layers();
		if (!L_ || !fr_ || !out_ || !rowbuf_) break;
		ensure_map(vi.system);
		m->valid = 1;
		memset(m->real, 0, sizeof m->real); memset(m->pic, 0, sizeof m->pic); memset(m->hud, 0, sizeof m->hud);
		bool ok = false;
		const bool md = vi.system == MD_SYS_MD, gg = vi.system == MD_SYS_GG, sms = vi.system == MD_SYS_SMS;
		if (md && job_.strip_px > 0 && f.margin > 0) ok = describe_md(f, vi, frame, margin);
		else if (gg || sms) ok = describe_m4(f, vi, frame, margin, gg);
		if (!ok) { if (md) rows_off(); break; }
		wk_layers_update(L_, fr_, out_);
		m->nlayers = fr_->nlayers;
		const int H = m->vh;
		if (!md) for (int y = 0; y < H; y++) { real0_[0][y] = out_->line[y].real[0][0]; real0_[1][y] = out_->line[y].real[1][0]; }
		if (M_) {
			/* The map looks at the cells the screen shows, at their place in the world. On the Mega Drive it is given the level planes only (a
			 * plane widekit has not made up its mind about is not drawn into the strips either). While it is bound and the screen shows the
			 * cells it looked at last time (no line moved to another tile, no cell on screen changed, same scene) there is nothing for it to
			 * record or to verify again: it is left alone. */
			int kind0[WK_MAX_LAYERS];
			for (int l = 0; l < WK_MAX_LAYERS; l++) { kind0[l] = out_->kind[l]; if (md && out_->kind[l] == WK_KIND_UNKNOWN) out_->kind[l] = WK_KIND_OFF; }
			/* a layer whose lines scroll apart (a wave, a line by line effect: more runs of lines than MAP_RUNS) is not a place to learn a
			 * picture by, and looking at it line by line would cost more than the rest of the analysis: not given to the map this frame */
			for (int l = 0; l < fr_->nlayers; l++) {
				if (out_->kind[l] != WK_KIND_LEVEL && out_->kind[l] != WK_KIND_UNKNOWN) continue;
				int runs = 0, have = 0, p_wx = 0, p_wr = 0;
				for (int y = 0; y < H && y < 256; y++) {
					if (!out_->coherent[l][y]) continue;
					const int wx = out_->world_x[l][y], wr = fdiv(out_->world_row[l][y], 8);
					if (have && wx == p_wx && wr == p_wr) continue;
					have = 1; p_wx = wx; p_wr = wr; runs++;
				}
				map_runs_[l] = runs;
				if (runs > MAP_RUNS) out_->kind[l] = WK_KIND_OFF;
			}
			uint32_t key = out_->epoch * 0x9E3779B1u + 1u;
			for (int l = 0; l < fr_->nlayers; l++) {
				uint32_t k2 = (uint32_t) out_->kind[l];
				if (out_->kind[l] == WK_KIND_LEVEL || out_->kind[l] == WK_KIND_UNKNOWN) {
					const uint32_t *cc = code_[l], *hh = hash_[l];
					const int mw = map_w_, mh = map_h_;
					int have = 0, p_tf = 0, p_tl = 0, p_wr = 0;
					for (int y = 0; y < H && y < 256; y++) {
						if (!out_->coherent[l][y]) continue;
						const int wx = out_->world_x[l][y], tf = fdiv(wx, 8), tl = fdiv(wx + fr_->view_w - 1, 8), wr = fdiv(out_->world_row[l][y], 8);
						if (have && tf == p_tf && tl == p_tl && wr == p_wr) continue;
						have = 1; p_tf = tf; p_tl = tl; p_wr = wr;
						k2 = k2 * 0x01000193u ^ ((uint32_t) tf * 0x10001u + (uint32_t) tl * 31u + (uint32_t) wr * 0x9E3779B1u);
						const int rr = imod(wr, mh) * mw;
						int c = imod(tf, mw);
						for (int i = tf; i <= tl; i++) { k2 = (k2 ^ cc[rr + c] ^ (hh[rr + c] << 7)) * 0x01000193u; if (++c == mw) c = 0; }
					}
				}
				key = mix32(key, k2);
			}
			wk_map_stats ms;
			wk_map_stats_get(M_, &ms);
			if (!(ms.bound && map_key_ok_ && key == map_key_ && !out_->scene_event)) wk_map_update(M_, fr_, out_);
			else st_.map_skips++;
			map_key_ = key; map_key_ok_ = 1;
			wk_map_serve(M_, fr_, out_);
			for (int l = 0; l < WK_MAX_LAYERS; l++) out_->kind[l] = kind0[l];
		}
		if (md) {
			if (check_) check_seen(H);
			verify_md(f, H);
			decide_md(f, m, H);
			memcpy(m->opq, f.opq, sizeof m->opq);
			/* the strips of the frames to come hold the planes widekit trusts to continue past the edge (backdrop, level); a HUD or unknown
			 * plane is left out, so what shows there is the soft extension and the sprites, never a plane's stale or fixed tiles */
			int pl = 0;
			for (int l = 0; l < 2; l++) {
				if (out_->kind[l] == WK_KIND_BACKDROP || out_->kind[l] == WK_KIND_LEVEL) pl |= 1 << l;
				else if (out_->kind[l] == WK_KIND_UNKNOWN) {
					/* a plane widekit has not named (it scrolls but the game does not stream columns into it: a level no wider than its
					 * ring) goes in while the ring's own history vouches for a good part of the margin (two fifths to get in: a whole
					 * side is half; a fifth to stay): a plane that vouches for little would only hide the other one, which is real
					 * only as far as every plane in the strip is */
					long sum = 0, tot = 0;
					for (int y = 0; y < H && y < 256; y++) if (line_on_[l][y]) { sum += out_->line[y].real[0][l] + out_->line[y].real[1][l]; tot += 2 * out_->margin; }
					if (tot > 0 && (sum * 5 >= tot * 2 || (((planes_next_ >> l) & 1) && sum * 5 >= tot))) pl |= 1 << l;
				}
			}
			planes_next_ = pl;
			rows_md(f, H);
			m->have_strips = 1;
		} else {
			strips_m4(f, m, gg);
			m->have_strips = gg ? 1 : 0;
			m->own_strips = 1;
		}
		m->epoch = out_->epoch; m->scene_event = out_->scene_event;
		st_.analysed++;
		st_.hs_checked = out_->hs_checked; st_.hs_missed = out_->hs_missed;
		if (out_->scene_event) st_.scene_events++;
		if (M_) {
			wk_map_stats ms;
			wk_map_stats_get(M_, &ms);
			st_.map_cells = ms.cells; st_.map_rooms = ms.rooms; st_.map_bound = ms.bound;
		}
	} while (0);
	m->ready.store(1, std::memory_order_release);
}

/* no analysis of this frame: the VDP must not go on drawing from rows nobody checks */
void Wide::rows_off()
{
	md_host_wide_rows_clear();
	memset(ovr_row_, 0, sizeof ovr_row_);
	ovr_live_ = 0;
}

void Wide::census(Stats *s)
{
	sync();
	*s = st_;
	s->real_pct = c_real_pct_.load(); s->soft_lines = c_soft_lines_.load(); s->composed = c_composed_.load();
}

void Wide::ensure_compose(int vw, int vh, int margin)
{
	if (C_ && cvw_ == vw && cvh_ == vh && cm_ == margin) return;
	for (int i = 0; i < COMPOSE_WHO; i++) { if (scr_[i]) wk_compose_scratch_destroy(scr_[i]); scr_[i] = 0; }
	if (C_) { wk_compose_destroy(C_); C_ = 0; }
	if (!ci_) ci_ = (wk_compose_in *) calloc(1, sizeof(wk_compose_in));
	free(wide_); free(prov_); free(cand_[0]); free(cand_[1]); free(opq_[0]); free(opq_[1]);
	wide_ = 0; prov_ = 0; cand_[0] = cand_[1] = 0; opq_[0] = opq_[1] = 0;
	if (!ana2_) ana2_ = (wk_out *) calloc(1, sizeof(wk_out));
	wk_compose_cfg cc;
	wk_compose_cfg_default(&cc);
	cc.period = 0;
	cc.fade_px = 12;
	C_ = wk_compose_create(&cc, vw, vh, margin);
	wide_ = (uint16_t *) calloc((size_t) (vw + 2 * margin) * (size_t) vh, 2);
	prov_ = (uint8_t *) calloc((size_t) (vw + 2 * margin) * (size_t) vh, 1);
	cand_[0] = (uint16_t *) calloc((size_t) vh * (size_t) margin, 2);
	cand_[1] = (uint16_t *) calloc((size_t) vh * (size_t) margin, 2);
	opq_[0] = (uint8_t *) calloc((size_t) vh * (size_t) margin, 1);
	opq_[1] = (uint8_t *) calloc((size_t) vh * (size_t) margin, 1);
	if (C_) for (int i = 1; i < COMPOSE_WHO; i++) scr_[i] = wk_compose_scratch_create(C_);      /* who 0 works in the handle's own */
	if (!C_ || !wide_ || !prov_ || !cand_[0] || !cand_[1] || !opq_[0] || !opq_[1] || !ana2_ || !ci_ || !scr_[1] || !scr_[2] || !scr_[3]) {
		for (int i = 0; i < COMPOSE_WHO; i++) { if (scr_[i]) wk_compose_scratch_destroy(scr_[i]); scr_[i] = 0; }
		if (C_) wk_compose_destroy(C_);
		C_ = 0; free(wide_); free(prov_); free(cand_[0]); free(cand_[1]); free(opq_[0]); free(opq_[1]); wide_ = 0; prov_ = 0; cand_[0] = cand_[1] = 0; opq_[0] = opq_[1] = 0;
		cvw_ = cvh_ = cm_ = 0;
		return;
	}
	memset(ana2_, 0, sizeof *ana2_);
	cvw_ = vw; cvh_ = vh; cm_ = margin;
}

const uint16_t *Wide::compose(const Meta &m, const uint16_t *view0, int pitch_px, int *w, int *h)
{
	const int n = compose_begin(m, view0, pitch_px, w, h);
	if (n <= 0) return 0;
	compose_lines(0, n, 0);
	return compose_end();
}

void Wide::compose_lines(int y0, int y1, int who)
{
	if (C_ && ci_ && who >= 0 && who < COMPOSE_WHO) wk_compose_lines(C_, ci_, y0, y1, scr_[who]);
}

const uint16_t *Wide::compose_end()
{
	if (!C_) return 0;
	wk_compose_end(C_);
	if (cmeta_ && cmeta_->mask_left) {
		const Meta &m = *cmeta_;
		const int stride = m.vw + 2 * m.margin;
		for (int y = 0; y < m.vh; y++) if (m.bar_ok[y] && lastreal_[0][y] > 0) memcpy(wide_ + (size_t) y * (size_t) stride + m.margin, m.bar[y], sizeof m.bar[y]);
	}
	cmeta_ = 0;
	c_composed_.fetch_add(1);
	return wide_;
}

int Wide::compose_begin(const Meta &m, const uint16_t *view0, int pitch_px, int *w, int *h)
{
	wait(m);
	if (!m.valid || m.margin <= 0 || m.vw <= 0 || m.vh <= 0 || m.vh > 256 || m.margin > WK_MAX_MARGIN || !view0) return 0;
	ensure_compose(m.vw, m.vh, m.margin);
	if (!C_ || !wide_ || !ci_) return 0;
	const int M = m.margin, V = m.vw, H = m.vh;
	wk_out *a = ana2_;
	const int use = (m.have_strips || m.own_strips) && m.nlayers > 0;
	const bool md = m.system == MD_SYS_MD;
	const bool masks = use && md && !m.all_opaque;
	int real_total = 0, soft_lines = 0;
	/* widekit's compositor sees one layer: the picture beside the view, real for real[] pixels of each line and side */
	a->nlines = H; a->margin = M;
	a->epoch = m.epoch; a->scene_event = use ? m.scene_event : 0;
	a->kind[0] = WK_KIND_LEVEL; a->conf[0] = 255;
	const uint16_t *own = m.own_strips ? m.px : 0;
	for (int y = 0; y < H; y++) {
		wk_line_out &lo = a->line[y];
		int r[2] = { 0, 0 };
		if (use && !m.hud[y]) for (int s = 0; s < 2; s++) {
			int v = m.real[s][y];
			if (v > m.real_cap) v = m.real_cap;
			if (v > M) v = M;
			r[s] = v;
		}
		lo.hud = (uint8_t) (use && m.hud[y] ? 1 : 0);
		for (int s = 0; s < 2; s++) { lo.real[s][0] = lo.ring[s][0] = (uint16_t) r[s]; lastreal_[s][y] = r[s]; }
		lo.level_real[0] = (uint16_t) r[0]; lo.level_real[1] = (uint16_t) r[1];
		lo.level_mask = (uint8_t) ((r[0] | r[1]) ? 1 : 0);
		lo.backdrop_mask = 0;
		real_total += r[0] + r[1];
		if (!r[0] && !r[1]) { soft_lines++; continue; }
		/* the strips of this line as widekit wants them: by distance from the view's edge */
		const uint16_t *row = view0 + (size_t) y * (size_t) pitch_px;
		for (int s = 0; s < 2; s++) {
			const int n = r[s];
			if (!n) continue;
			uint16_t *c = cand_[s] + (size_t) y * (size_t) M;
			int np = m.have_strips ? (m.pic[s][y] < n ? m.pic[s][y] : n) : 0;      /* from the picture buffer; the rest from the Meta's own strips */
			if (!own) np = n;
			if (np > 0) { if (s == 0) wk_rev16(c, row - np, np); else memcpy(c, row + V, (size_t) np * 2); }
			if (n > np) memcpy(c + np, own + ((size_t) s * (size_t) H + (size_t) y) * (size_t) M + np, (size_t) (n - np) * 2);
			if (masks) {
				uint8_t *o = opq_[s] + (size_t) y * (size_t) M;
				const uint32_t *ob = m.opq[y][s];
				for (int d = 0; d < n; d++) o[d] = (uint8_t) ((ob[d >> 5] >> (d & 31)) & 1);
			}
		}
	}
	c_real_pct_.store((int) (real_total * 100L / (2L * M * H)));
	c_soft_lines_.store(soft_lines);
	wk_compose_in &ci = *ci_;
	memset(&ci, 0, sizeof ci);
	ci.ana = a;
	ci.centre = view0; ci.centre_stride = pitch_px;
	ci.out = wide_; ci.out_stride = V + 2 * M;
	if (want_prov_) ci.prov = prov_;
	cmeta_ = &m;
	if (m.mask_left && V >= 16) {
		/* the first eight columns are the border colour: the soft extension continues the picture, not the mask */
		if (!edge_) edge_ = (uint16_t *) calloc((size_t) 256 * WK_EDGE_N, 2);
		if (edge_) {
			for (int y = 0; y < H; y++) memcpy(edge_ + (size_t) y * WK_EDGE_N, view0 + (size_t) y * (size_t) pitch_px + 8, WK_EDGE_N * 2);
			ci.edge_px[0] = edge_;
		}
	}
	if (use) {
		ci.cand[0][0] = cand_[0]; ci.cand[0][1] = cand_[1];
		if (masks) { ci.opaque[0][0] = opq_[0]; ci.opaque[0][1] = opq_[1]; }
		ci.order[0] = 0; ci.norder = 1;
	}
	wk_compose_begin(C_, &ci);
	*w = V + 2 * M; *h = H;
	return H;
}

}  // namespace mdw
