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

enum { RING_MAX = 4096 };

Wide::Wide() : L_(0), C_(0), cvw_(0), cvh_(0), cm_(0), fr_(0), out_(0), ana2_(0), wide_(0), wide_cap_(0), vram_prev_(0), tilever_(0),
	have_prev_(0), map_w_(0), map_h_(0), layout_key_(0), gfx_gen_(1), have_cram_(0), cap_default_(1 << 20), analysed_frame_(0), planes_(3)
{
	cand_[0] = cand_[1] = 0; opq_[0] = opq_[1] = 0;
	for (int p = 0; p < 2; p++) { hash_[p] = 0; stamp_[p] = 0; prev_[p] = 0; line_on_[p] = 0; }
	memset(&st_, 0, sizeof st_);
	memset(lastreal_, 0, sizeof lastreal_);
	memset(cram_prev_, 0, sizeof cram_prev_);
}

Wide::~Wide() { free_all(); }

void Wide::free_all()
{
	if (C_) wk_compose_destroy(C_);
	if (L_) wk_layers_destroy(L_);
	C_ = 0; L_ = 0;
	free(fr_); free(out_); free(ana2_); free(wide_); free(cand_[0]); free(cand_[1]); free(opq_[0]); free(opq_[1]);
	free(vram_prev_); free(tilever_);
	fr_ = 0; out_ = 0; ana2_ = 0; wide_ = 0; cand_[0] = cand_[1] = 0; opq_[0] = opq_[1] = 0; vram_prev_ = 0; tilever_ = 0;
	for (int p = 0; p < 2; p++) { free(hash_[p]); free(stamp_[p]); free(prev_[p]); free(line_on_[p]); hash_[p] = 0; stamp_[p] = 0; prev_[p] = 0; line_on_[p] = 0; }
	cvw_ = cvh_ = cm_ = 0; wide_cap_ = 0;
}

void Wide::reset()
{
	if (L_) wk_layers_reset(L_);
	if (C_) wk_compose_reset(C_);
	have_prev_ = 0; layout_key_ = 0; have_cram_ = 0;
	if (vram_prev_) memset(vram_prev_, 0, 0x10000);
	if (tilever_) memset(tilever_, 0, 0x800 * sizeof(uint32_t));
	for (int p = 0; p < 2; p++) if (stamp_[p]) memset(stamp_[p], 0, RING_MAX * sizeof(uint32_t));
	memset(lastreal_, 0, sizeof lastreal_);
	analysed_frame_ = 0;
	planes_ = 3; md_host_set_wide_planes(3);
}

void Wide::ensure_layers()
{
	if (L_) return;
	wk_layers_cfg lc;
	wk_layers_cfg_default(&lc);
	L_ = wk_layers_create(&lc);
	fr_ = (wk_frame *) calloc(1, sizeof(wk_frame));
	out_ = (wk_out *) calloc(1, sizeof(wk_out));
	ana2_ = (wk_out *) calloc(1, sizeof(wk_out));
	vram_prev_ = (uint8_t *) calloc(1, 0x10000);
	tilever_ = (uint32_t *) calloc(0x800, sizeof(uint32_t));
	for (int p = 0; p < 2; p++) {
		hash_[p] = (uint32_t *) calloc(RING_MAX, sizeof(uint32_t));
		stamp_[p] = (uint32_t *) calloc(RING_MAX, sizeof(uint32_t));
		prev_[p] = (uint16_t *) calloc(RING_MAX, sizeof(uint16_t));
		line_on_[p] = (uint8_t *) calloc(256, 1);
	}
}

/* the pixels of every 32 byte tile that changed since the last analysed frame bump that tile's counter; a frame that changes a great many
 * (a tile set loaded) bumps the generation, which widekit takes for a new scene */
void Wide::tiles_diff(const uint8_t *vram, uint32_t)
{
	unsigned changed = 0;
	for (unsigned t = 0; t < 0x800; t++) {
		const uint32_t *a = (const uint32_t *) (vram + t * 32), *b = (const uint32_t *) (vram_prev_ + t * 32);
		if ((a[0] ^ b[0]) | (a[1] ^ b[1]) | (a[2] ^ b[2]) | (a[3] ^ b[3]) | (a[4] ^ b[4]) | (a[5] ^ b[5]) | (a[6] ^ b[6]) | (a[7] ^ b[7])) {
			tilever_[t]++;
			changed++;
		}
	}
	memcpy(vram_prev_, vram, 0x10000);
	if (changed > 192) gfx_gen_++;
}

static inline int imod(int a, int n) { int r = a % n; return r < 0 ? r + n : r; }

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
	if (mw * mh > RING_MAX || mw < 32 || mh < 32) return false;
	const int key = (int) (mix32(mix32(l0.ntab, l0.ntbb), (uint32_t) (mw * 977 + mh * 13 + l0.shift)) & 0x7FFFFFFF) | 1;
	if (key != layout_key_) {
		wk_layers_reset(L_);
		have_prev_ = 0; layout_key_ = key;
		for (int p = 0; p < 2; p++) memset(stamp_[p], 0, RING_MAX * sizeof(uint32_t));
	}
	map_w_ = mw; map_h_ = mh;
	tiles_diff(f.vram, frame);
	{
		int delta = 0;
		if (have_cram_) for (int i = 0; i < 128; i += 2) if (memcmp(f.cram + i, cram_prev_ + i, 2)) delta++;
		memcpy(cram_prev_, f.cram, 128); have_cram_ = 1;
		fr_->palette_delta = delta;
	}
	const uint16_t *vw16 = (const uint16_t *) f.vram;
	for (int p = 0; p < 2; p++) {
		const unsigned base = p == 0 ? l0.ntab : l0.ntbb;
		uint32_t *hh = hash_[p], *ss = stamp_[p];
		uint16_t *pv = prev_[p];
		for (int ry = 0; ry < mh; ry++) {
			const unsigned rowoff = (unsigned) (((unsigned) ry << l0.shift) & 0x1FC0);
			for (int rx = 0; rx < mw; rx++) {
				const unsigned addr = (base + rowoff + (unsigned) rx * 2) & 0xFFFE;
				const uint16_t w = vw16[addr >> 1];
				const int k = ry * mw + rx;
				if (have_prev_ && pv[k] != w) ss[k] = frame;
				pv[k] = w;
				hh[k] = mix32(w, tilever_[w & 0x7FF]);
			}
		}
		wk_layer *L = &fr_->layer[p];
		L->active = 1; L->role = WK_ROLE_AUTO; L->priority = p == 0 ? 1 : 0;
		L->map_w = mw; L->map_h = mh; L->tile_px = 8;
		L->hash = hh; L->code = 0; L->stamp = ss; L->gfx_id = gfx_gen_ * 2654435761u + (uint32_t) (mw * 31 + mh);
		L->line_on = line_on_[p];
	}
	have_prev_ = 1;
	const int ringpx = mw * 8, rowmask = l0.row_mask;
	int on_n = 0;
	for (y = 0; y < H && y < 256; y++) {
		const md_wide_line &r = f.line[y];
		const int same = (r.flags & MDW_ON) && !(r.flags & (MDW_WINDOW | MDW_VSCROLL | MDW_IM2)) && r.ntab == l0.ntab && r.ntbb == l0.ntbb &&
		                 r.col_mask == l0.col_mask && r.shift == l0.shift && r.row_mask == l0.row_mask;
		for (int p = 0; p < 2; p++) {
			line_on_[p][y] = (uint8_t) (same ? 1 : 0);
			wk_layer *L = &fr_->layer[p];
			L->scroll_x[y] = imod(-(int) (r.xs[p] & (uint16_t) (ringpx - 1)), ringpx);
			L->row_y[y] = (y + (int) r.ys[p]) & rowmask;
		}
		on_n += same;
	}
	fr_->nlayers = 2;
	return on_n > 0;
}

bool Wide::describe_gg(const md_wide_frame &f, const md_video_info &vi, uint32_t frame, int margin)
{
	const int H = 144, VY0 = 24;
	int first = -1, y;
	memset(fr_, 0, sizeof *fr_);
	fr_->frame = frame; fr_->view_w = 160; fr_->view_h = H; fr_->margin = margin;
	fr_->palette_size = 32;
	(void) vi;
	for (y = 0; y < H; y++) if (f.line[y + VY0].flags & MDW_ON) { first = y; break; }
	fr_->brightness = first >= 0 ? 255 : 0;
	if (first < 0) return false;
	const md_wide_line &l0 = f.line[first + VY0];
	const unsigned nt_mask = (unsigned) (l0.ntab | 0x400u) & 0xFFFFu;       /* the Game Gear VDP's name table mask */
	const int mw = 32, mh = 28;
	const int key = (int) (mix32(nt_mask, 0x6767u) & 0x7FFFFFFF) | 1;
	if (key != layout_key_) {
		wk_layers_reset(L_);
		have_prev_ = 0; layout_key_ = key;
		memset(stamp_[0], 0, RING_MAX * sizeof(uint32_t));
	}
	map_w_ = mw; map_h_ = mh;
	tiles_diff(f.vram, frame);
	{
		int delta = 0;
		if (have_cram_) for (int i = 0; i < 64; i += 2) if (memcmp(f.cram + i, cram_prev_ + i, 2)) delta++;
		memcpy(cram_prev_, f.cram, 128); have_cram_ = 1;
		fr_->palette_delta = delta;
	}
	const uint16_t *vw16 = (const uint16_t *) f.vram;
	{
		uint32_t *hh = hash_[0], *ss = stamp_[0];
		uint16_t *pv = prev_[0];
		for (int ry = 0; ry < mh; ry++) {
			const unsigned rowaddr = (0x3800u + ((unsigned) ry << 6)) & nt_mask;
			for (int rx = 0; rx < mw; rx++) {
				const uint16_t w = vw16[((rowaddr + (unsigned) rx * 2) & 0xFFFE) >> 1];
				const int k = ry * mw + rx;
				if (have_prev_ && pv[k] != w) ss[k] = frame;
				pv[k] = w;
				hh[k] = mix32(w, tilever_[w & 0x1FF]);
			}
		}
		wk_layer *L = &fr_->layer[0];
		L->active = 1; L->role = WK_ROLE_AUTO; L->priority = 0;
		L->map_w = mw; L->map_h = mh; L->tile_px = 8;
		L->hash = hh; L->code = 0; L->stamp = ss; L->gfx_id = gfx_gen_ * 2654435761u + 77u;
		L->line_on = line_on_[0];
	}
	have_prev_ = 1;
	int on_n = 0;
	for (y = 0; y < H; y++) {
		const md_wide_line &r = f.line[y + VY0];
		const int same = (r.flags & MDW_ON) && !(r.flags & MDW_VSCROLL) && r.ntab == l0.ntab;
		line_on_[0][y] = (uint8_t) (same ? 1 : 0);
		wk_layer *L = &fr_->layer[0];
		L->scroll_x[y] = imod(48 - (int) (r.xs[0] & 255), 256);
		L->row_y[y] = imod(y + VY0 + (int) r.ys[0], 224);
		on_n += same;
	}
	fr_->nlayers = 1;
	return on_n > 0;
}

void Wide::analyse(uint32_t frame, const md_video_info &vi, int margin, int strip_px, Meta *m)
{
	m->valid = 0; m->system = vi.system; m->vw = vi.w; m->vh = vi.h; m->margin = margin; m->real_cap = margin < cap_default_ ? margin : cap_default_;
	m->view_off = 0; m->have_strips = 0; m->nlayers = 0; m->frame = frame; m->planes = (uint8_t) (vi.system == MD_SYS_MD ? planes_ : 3);
	memset(m->strip, 0, sizeof m->strip);
	memset(m->on, 0, sizeof m->on);
	st_.frames++;
	if (margin <= 0 || vi.h <= 0 || vi.h > 256 || vi.w <= 0) return;
	if (vi.interlace == 2) return;
	if (vi.system == MD_SYS_GG) {
		if (!vi.gg_band) return;
		m->vw = 160; m->vh = 144; m->margin = margin = 48;
		if (m->real_cap > margin) m->real_cap = margin;
	}
	ensure_layers();
	if (!L_ || !fr_ || !out_) return;
	md_wide_frame f;
	md_host_wide_frame(&f);
	bool ok = false;
	if (vi.system == MD_SYS_MD && strip_px > 0 && f.margin > 0) ok = describe_md(f, vi, frame, margin);
	else if (vi.system == MD_SYS_GG && f.line) ok = describe_gg(f, vi, frame, margin);
	m->valid = 1;
	if (!ok) return;
	wk_layers_update(L_, fr_, out_);
	m->nlayers = fr_->nlayers;
	m->ana = *out_;
	const int H = m->vh;
	for (int y = 0; y < H && y < 256; y++) {
		if (vi.system == MD_SYS_MD) m->strip[y] = (f.line[y].flags & MDW_STRIP) ? 1 : 0;
		else m->strip[y] = 1;
		m->on[y] = (uint8_t) (line_on_[0][y] | (m->nlayers > 1 ? line_on_[1][y] << 1 : 0));
	}
	m->have_strips = 1;
	if (vi.system == MD_SYS_MD && f.opq) memcpy(m->opq, f.opq, sizeof m->opq);
	if (vi.system == MD_SYS_MD && m->nlayers == 2) {
		/* next frame's strips hold the planes widekit trusts to continue past the edge (backdrop, level); a HUD or unknown plane is
		 * left out, so what shows there is the soft extension and the sprites, never a plane's stale or fixed tiles */
		int pl = 0;
		for (int l = 0; l < 2; l++) if (out_->kind[l] == WK_KIND_BACKDROP || out_->kind[l] == WK_KIND_LEVEL) pl |= 1 << l;
		planes_ = pl;
		md_host_set_wide_planes(pl);
	}
	st_.analysed++;
	st_.hs_checked = out_->hs_checked; st_.hs_missed = out_->hs_missed;
	if (out_->scene_event) st_.scene_events++;
	analysed_frame_ = frame;
}

void Wide::ensure_compose(int vw, int vh, int margin)
{
	if (C_ && cvw_ == vw && cvh_ == vh && cm_ == margin) return;
	if (C_) { wk_compose_destroy(C_); C_ = 0; }
	free(wide_); free(cand_[0]); free(cand_[1]); free(opq_[0]); free(opq_[1]);
	wide_ = 0; cand_[0] = cand_[1] = 0; opq_[0] = opq_[1] = 0;
	if (!ana2_) ana2_ = (wk_out *) calloc(1, sizeof(wk_out));
	wk_compose_cfg cc;
	wk_compose_cfg_default(&cc);
	cc.period = 0;
	cc.fade_px = 12;
	C_ = wk_compose_create(&cc, vw, vh, margin);
	wide_ = (uint16_t *) calloc((size_t) (vw + 2 * margin) * (size_t) vh, 2);
	cand_[0] = (uint16_t *) calloc((size_t) vh * (size_t) margin, 2);
	cand_[1] = (uint16_t *) calloc((size_t) vh * (size_t) margin, 2);
	opq_[0] = (uint8_t *) calloc((size_t) vh * (size_t) margin, 1);
	opq_[1] = (uint8_t *) calloc((size_t) vh * (size_t) margin, 1);
	if (!C_ || !wide_ || !cand_[0] || !cand_[1] || !opq_[0] || !opq_[1] || !ana2_) {
		if (C_) wk_compose_destroy(C_);
		C_ = 0; free(wide_); free(cand_[0]); free(cand_[1]); free(opq_[0]); free(opq_[1]); wide_ = 0; cand_[0] = cand_[1] = 0; opq_[0] = opq_[1] = 0;
		cvw_ = cvh_ = cm_ = 0;
		return;
	}
	cvw_ = vw; cvh_ = vh; cm_ = margin;
}

const uint16_t *Wide::compose(const Meta &m, const uint16_t *view0, int pitch_px, int *w, int *h)
{
	if (!m.valid || m.margin <= 0 || m.vw <= 0 || m.vh <= 0 || m.vh > 256 || m.margin > WK_MAX_MARGIN || !view0) return 0;
	ensure_compose(m.vw, m.vh, m.margin);
	if (!C_ || !wide_) return 0;
	const int M = m.margin, V = m.vw, H = m.vh;
	wk_out *a = ana2_;
	const int use = m.have_strips && m.nlayers > 0;
	int real_total = 0, soft_lines = 0;
	if (use) {
		*a = m.ana;                      /* hysteresis, epochs and the scene event as widekit left them */
		a->kind[0] = WK_KIND_LEVEL;
		a->conf[0] = 255;
	} else {
		memset(a, 0, sizeof *a);
		a->nlines = H; a->margin = M;
	}
	for (int y = 0; y < H; y++) {
		wk_line_out &lo = a->line[y];
		int r[2] = { 0, 0 };
		if (use && m.strip[y] && m.on[y]) {
			for (int s = 0; s < 2; s++) {
				int v = M;
				for (int l = 0; l < m.nlayers; l++) if (((m.on[y] & m.planes) >> l) & 1) {
					const int k = m.ana.kind[l];
					const int x = k == WK_KIND_BACKDROP ? M : k == WK_KIND_LEVEL ? m.ana.line[y].real[s][l] : 0;
					if (x < v) v = x;
				}
				if (v > m.real_cap) v = m.real_cap;
				r[s] = v < 0 ? 0 : v;
			}
			if (m.ana.line[y].hud) r[0] = r[1] = 0;
		}
		lo.hud = (uint8_t) (m.ana.line[y].hud && use ? 1 : 0);
		for (int s = 0; s < 2; s++) { lo.real[s][0] = lo.ring[s][0] = (uint16_t) r[s]; lastreal_[s][y] = r[s]; }
		for (int l = 1; l < WK_MAX_LAYERS; l++) lo.real[0][l] = lo.real[1][l] = lo.ring[0][l] = lo.ring[1][l] = 0;
		lo.level_real[0] = lo.level_real[1] = 0;
		lo.level_real[0] = (uint16_t) r[0]; lo.level_real[1] = (uint16_t) r[1];
		lo.level_mask = (uint8_t) ((r[0] | r[1]) ? 1 : 0);
		lo.backdrop_mask = 0;
		a->coherent[0][y] = 1;
		real_total += r[0] + r[1];
		if (!r[0] && !r[1]) soft_lines++;
	}
	st_.real_pct = (int) (real_total * 100L / (2L * M * H));
	st_.soft_lines = soft_lines;
	wk_compose_in ci;
	memset(&ci, 0, sizeof ci);
	ci.ana = a;
	ci.centre = view0; ci.centre_stride = pitch_px;
	ci.out = wide_; ci.out_stride = V + 2 * M;
	if (use) {
		for (int y = 0; y < H; y++) {
			const uint16_t *row = view0 + (size_t) y * (size_t) pitch_px;
			const int rl = a->line[y].real[0][0], rr = a->line[y].real[1][0];
			uint16_t *cl = cand_[0] + (size_t) y * (size_t) M, *cr = cand_[1] + (size_t) y * (size_t) M;
			uint8_t *ol = opq_[0] + (size_t) y * (size_t) M, *orr = opq_[1] + (size_t) y * (size_t) M;
			const bool md = m.system == MD_SYS_MD;
			for (int d = 0; d < rl; d++) { cl[d] = row[-1 - d]; ol[d] = (uint8_t) (md ? (m.opq[y][0][d >> 5] >> (d & 31)) & 1 : 1); }
			for (int d = 0; d < rr; d++) { cr[d] = row[V + d]; orr[d] = (uint8_t) (md ? (m.opq[y][1][d >> 5] >> (d & 31)) & 1 : 1); }
		}
		ci.cand[0][0] = cand_[0]; ci.cand[0][1] = cand_[1];
		ci.opaque[0][0] = opq_[0]; ci.opaque[0][1] = opq_[1];
		ci.order[0] = 0; ci.norder = 1;
	}
	wk_compose_frame(C_, &ci);
	st_.composed++;
	*w = V + 2 * M; *h = H;
	return wide_;
}

}  // namespace mdw
