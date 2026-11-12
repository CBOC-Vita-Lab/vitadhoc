/*
 * Software framebuffer binding of pk::Gfx: the Linux twin. Renders menus to
 * memory so they can be unit-tested and written out as PNG. Not used on the
 * console.
 *
 * Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE).
 */
#ifndef PK_GFX_SOFT_H
#define PK_GFX_SOFT_H

#include "pk_gfx.h"
#include <vector>
#include <string>

namespace pk {

class SoftGfx : public Gfx {
public:
	SoftGfx(int w = PK_SCREEN_W, int h = PK_SCREEN_H);
	~SoftGfx();
	int width() const { return w_; }
	int height() const { return h_; }

	void fill_rect(float x, float y, float w, float h, uint32_t rgba);
	void fill_rrect(float x, float y, float w, float h, float r, uint32_t rgba);
	void stroke_rrect(float x, float y, float w, float h, float r, float t, uint32_t rgba);
	void fill_circle(float cx, float cy, float r, uint32_t rgba);
	void fill_tri(float x0, float y0, float x1, float y1, float x2, float y2, uint32_t rgba);
	void gradient_v(float x, float y, float w, float h, uint32_t top, uint32_t bottom);
	Texture *tex_create(int w, int h, TexFormat fmt);
	void tex_update(Texture *t, const void *pixels, int stride_bytes);
	void tex_free(Texture *t);
	void tex_draw(Texture *t, float sx, float sy, float sw, float sh,
	              float dx, float dy, float dw, float dh, uint32_t tint, bool smooth);
	void clip(float x, float y, float w, float h);
	void clip_off();

	/* the framebuffer: w*h pixels, 0xAABBGGRR with alpha forced to 255 */
	const uint32_t *pixels() const { return &fb_[0]; }
	uint32_t pixel(int x, int y) const { return fb_[(size_t) y * w_ + x]; }
	void clear(uint32_t rgba);
	/* Writes the framebuffer as an RGB PNG. Returns false on a file error. */
	bool write_png(const std::string &path) const;
	int live_textures() const { return live_; }

private:
	void blend(int x, int y, uint32_t rgba, float cov);
	int w_, h_;
	std::vector<uint32_t> fb_;
	int cx0_, cy0_, cx1_, cy1_;
	int live_;
};

/* PNG writer (uncompressed deflate blocks): RGB, 8 bit. */
bool write_png_rgb(const std::string &path, int w, int h, const uint8_t *rgb);

}  // namespace pk

#endif
