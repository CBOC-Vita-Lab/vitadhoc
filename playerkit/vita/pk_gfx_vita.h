/*
 * playerkit's drawing interface on the PS Vita, over vita2d. Written fresh for
 * playerkit; it uses only the public vita2d calls. Compiled by `make vita`; it
 * can only be run on the console.
 *
 * Call it between vita2d_start_drawing() and vita2d_end_drawing().
 *
 * Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE).
 */
#ifndef PK_GFX_VITA_H
#define PK_GFX_VITA_H

#include "pk_gfx.h"

namespace pk {

class VitaGfx : public Gfx {
public:
	VitaGfx();
	~VitaGfx();
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
};

}  // namespace pk

#endif
