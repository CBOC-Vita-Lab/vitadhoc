/*
 * The CRT mask of the players: a row of phosphor columns (pk_screen.h pk_crt_make) made once into a one row RGBA8
 * texture as wide as the picture on the screen, and drawn over the picture as ONE quad with the multiply blend
 * (Gfx::tex_draw_mul), stretched down the picture's height. The column width follows the on-screen pixel scale
 * (pk_crt_col_w). Rebuilt only when the width on screen, the source width or the strength changes. Cost: one quad.
 *
 * Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE).
 */
#ifndef PK_CRT_H
#define PK_CRT_H

#include "pk_gfx.h"
#include "pk_screen.h"
#include <stdint.h>
#include <string.h>
#include <vector>

namespace pk {

class CrtMask {
public:
	CrtMask() : tex_(0), dw_(0), sw_(0), lvl_(0), gfx_(0) {}
	~CrtMask() { release(); }
	/* Call BEFORE opening a GXM scene on Vita so any tex_create / tex_free runs outside BeginScene..EndScene. */
	void prepare(Gfx *g, int strength, float dw, int src_w)
	{
		if (!g || strength <= 0 || src_w <= 0 || dw < 1) { release(); return; }
		const int w = (int) (dw + 0.5f) < 1 ? 1 : (int) (dw + 0.5f);
		if (w > 4096) return;
		if (!tex_ || g != gfx_ || w != dw_ || src_w != sw_ || strength != lvl_) {
			release();
			uint8_t pat[4 * PK_CRT_MAX_W];
			const int cw = pk_crt_col_w(dw / (float) src_w), n = pk_crt_make(strength, cw, pat);
			if (!n) return;
			std::vector<uint8_t> row((size_t) w * 4);
			for (int x = 0; x < w; x++) memcpy(&row[(size_t) x * 4], pat + 4 * (x % n), 4);
			tex_ = g->tex_create(w, 1, TEX_RGBA8);
			if (!tex_) return;
			g->tex_update(tex_, &row[0], w * 4);
			gfx_ = g; dw_ = w; sw_ = src_w; lvl_ = strength;
		}
	}
	/* Call prepare() BEFORE opening a GXM scene on Vita so OOM or state changes never call tex_create / sceGxmFinish inside BeginScene..EndScene. */
	void draw(Gfx *g, int strength, float dx, float dy, float dw, float dh, int src_w)
	{
		if (!g || strength <= 0 || src_w <= 0 || dw < 1 || dh < 1 || !tex_) return;
		const int w = (int) (dw + 0.5f) < 1 ? 1 : (int) (dw + 0.5f);
		if (g != gfx_ || w != dw_ || src_w != sw_ || strength != lvl_) return;
		g->tex_draw_mul(tex_, 0, 0, (float) dw_, 1, dx, dy, dw, dh);
	}
	void release() { if (tex_ && gfx_) gfx_->tex_free(tex_); tex_ = 0; gfx_ = 0; dw_ = sw_ = lvl_ = 0; }
	/* for the tests: the column width in use (0 when none) */
	int width() const { return dw_; }
private:
	Texture *tex_;
	int dw_, sw_, lvl_;
	Gfx *gfx_;
};

}  // namespace pk

#endif
