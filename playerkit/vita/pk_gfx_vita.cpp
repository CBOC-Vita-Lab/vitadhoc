/* playerkit's vita2d binding. Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE). */
#include "pk_gfx_vita.h"

#include <math.h>
#include <string.h>
#include <vita2d.h>

namespace pk {

/* the opaque Texture is a vita2d texture plus what the copy needs */
struct Texture {
	vita2d_texture *tex;
	int w, h;
	TexFormat fmt;
};

VitaGfx::VitaGfx() {}
VitaGfx::~VitaGfx() {}

/* vertices must live in vita2d's per-frame pool */
static vita2d_color_vertex *verts(size_t n)
{
	return (vita2d_color_vertex *) vita2d_pool_memalign((unsigned int) (n * sizeof(vita2d_color_vertex)), sizeof(vita2d_color_vertex));
}

void VitaGfx::fill_rect(float x, float y, float w, float h, uint32_t c)
{
	if (w <= 0 || h <= 0 || (c >> 24) == 0) return;
	vita2d_draw_rectangle(x, y, w, h, c);
}

/* a rounded rectangle as one triangle fan: the alpha is blended once, not once per piece */
static void rrect_fan(float x, float y, float w, float h, float r, uint32_t c)
{
	if (w <= 0 || h <= 0 || (c >> 24) == 0) return;
	if (r > w * 0.5f) r = w * 0.5f;
	if (r > h * 0.5f) r = h * 0.5f;
	if (r < 1.0f) { vita2d_draw_rectangle(x, y, w, h, c); return; }
	const int seg = r < 8 ? 4 : r < 20 ? 8 : 12;
	const size_t n = (size_t) (4 * (seg + 1) + 2);
	vita2d_color_vertex *v = verts(n);
	if (!v) return;
	size_t k = 0;
	v[k].x = x + w * 0.5f; v[k].y = y + h * 0.5f; v[k].z = 0.5f; v[k].color = c; k++;
	const float cx[4] = { x + w - r, x + r, x + r, x + w - r };
	const float cy[4] = { y + r, y + r, y + h - r, y + h - r };
	for (int corner = 0; corner < 4; corner++) {
		for (int i = 0; i <= seg; i++) {
			float a = ((float) corner + (float) i / (float) seg) * 1.57079633f;     /* 0 = right, going counter-clockwise on screen */
			v[k].x = cx[corner] + cosf(a) * r;
			v[k].y = cy[corner] - sinf(a) * r;
			v[k].z = 0.5f; v[k].color = c; k++;
		}
	}
	v[k] = v[1]; k++;
	vita2d_draw_array(SCE_GXM_PRIMITIVE_TRIANGLE_FAN, v, k);
}

void VitaGfx::fill_rrect(float x, float y, float w, float h, float r, uint32_t c) { rrect_fan(x, y, w, h, r, c); }

void VitaGfx::stroke_rrect(float x, float y, float w, float h, float r, float t, uint32_t c)
{
	if (t <= 0 || (c >> 24) == 0) return;
	/* four bars plus nothing at the corners is plain but cheap; the menu only strokes small focus rings */
	float rr = r > t ? r - t : 0;
	(void) rr;
	vita2d_draw_rectangle(x + r, y, w - 2 * r, t, c);
	vita2d_draw_rectangle(x + r, y + h - t, w - 2 * r, t, c);
	vita2d_draw_rectangle(x, y + r, t, h - 2 * r, c);
	vita2d_draw_rectangle(x + w - t, y + r, t, h - 2 * r, c);
	/* the corners as arcs of short quads */
	const int seg = 8;
	const float cx[4] = { x + w - r, x + r, x + r, x + w - r };
	const float cy[4] = { y + r, y + r, y + h - r, y + h - r };
	for (int corner = 0; corner < 4; corner++) {
		vita2d_color_vertex *v = verts((size_t) (2 * (seg + 1)));
		if (!v) return;
		size_t k = 0;
		for (int i = 0; i <= seg; i++) {
			float a = ((float) corner + (float) i / (float) seg) * 1.57079633f;
			float co = cosf(a), si = sinf(a);
			v[k].x = cx[corner] + co * r;       v[k].y = cy[corner] - si * r;       v[k].z = 0.5f; v[k].color = c; k++;
			v[k].x = cx[corner] + co * (r - t); v[k].y = cy[corner] - si * (r - t); v[k].z = 0.5f; v[k].color = c; k++;
		}
		vita2d_draw_array(SCE_GXM_PRIMITIVE_TRIANGLE_STRIP, v, k);
	}
}

void VitaGfx::fill_circle(float cx, float cy, float r, uint32_t c)
{
	if (r <= 0 || (c >> 24) == 0) return;
	rrect_fan(cx - r, cy - r, 2 * r, 2 * r, r, c);
}

void VitaGfx::fill_tri(float x0, float y0, float x1, float y1, float x2, float y2, uint32_t c)
{
	if ((c >> 24) == 0) return;
	vita2d_color_vertex *v = verts(3);
	if (!v) return;
	v[0].x = x0; v[0].y = y0; v[1].x = x1; v[1].y = y1; v[2].x = x2; v[2].y = y2;
	for (int i = 0; i < 3; i++) { v[i].z = 0.5f; v[i].color = c; }
	vita2d_draw_array(SCE_GXM_PRIMITIVE_TRIANGLES, v, 3);
}

void VitaGfx::gradient_v(float x, float y, float w, float h, uint32_t top, uint32_t bottom)
{
	vita2d_color_vertex *v = verts(4);
	if (!v) return;
	v[0].x = x;     v[0].y = y;     v[0].color = top;
	v[1].x = x + w; v[1].y = y;     v[1].color = top;
	v[2].x = x;     v[2].y = y + h; v[2].color = bottom;
	v[3].x = x + w; v[3].y = y + h; v[3].color = bottom;
	for (int i = 0; i < 4; i++) v[i].z = 0.5f;
	vita2d_draw_array(SCE_GXM_PRIMITIVE_TRIANGLE_STRIP, v, 4);
}

static SceGxmTextureFormat gxm_format(TexFormat f)
{
	switch (f) {
	case TEX_A8:     return SCE_GXM_TEXTURE_FORMAT_U8_111R;     /* the byte is the alpha; the tint is the colour */
	case TEX_RGB565: return SCE_GXM_TEXTURE_FORMAT_U5U6U5_RGB;
	case TEX_BGR565: return SCE_GXM_TEXTURE_FORMAT_U5U6U5_BGR;
	default:         return SCE_GXM_TEXTURE_FORMAT_A8B8G8R8;
	}
}
static int bytes_per_pixel(TexFormat f) { return f == TEX_A8 ? 1 : f == TEX_RGBA8 ? 4 : 2; }

Texture *VitaGfx::tex_create(int w, int h, TexFormat fmt)
{
	if (w <= 0 || h <= 0) return 0;
	Texture *t = new Texture();
	t->tex = vita2d_create_empty_texture_format((unsigned int) w, (unsigned int) h, gxm_format(fmt));
	t->w = w; t->h = h; t->fmt = fmt;
	if (!t->tex) { delete t; return 0; }
	memset(vita2d_texture_get_datap(t->tex), 0, (size_t) vita2d_texture_get_stride(t->tex) * (size_t) h);
	return t;
}

void VitaGfx::tex_update(Texture *t, const void *pixels, int stride_bytes)
{
	if (!t || !pixels) return;
	unsigned char *dst = (unsigned char *) vita2d_texture_get_datap(t->tex);
	const unsigned dstride = vita2d_texture_get_stride(t->tex);
	const size_t row = (size_t) t->w * (size_t) bytes_per_pixel(t->fmt);
	for (int y = 0; y < t->h; y++) memcpy(dst + (size_t) y * dstride, (const unsigned char *) pixels + (size_t) y * (size_t) stride_bytes, row);
}

void VitaGfx::tex_free(Texture *t)
{
	if (!t) return;
	vita2d_wait_rendering_done();      /* the GPU may still be reading it */
	vita2d_free_texture(t->tex);
	delete t;
}

void VitaGfx::tex_draw(Texture *t, float sx, float sy, float sw, float sh, float dx, float dy, float dw, float dh, uint32_t tint, bool smooth)
{
	if (!t || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;
	vita2d_texture_set_filters(t->tex, smooth ? SCE_GXM_TEXTURE_FILTER_LINEAR : SCE_GXM_TEXTURE_FILTER_POINT,
	                           smooth ? SCE_GXM_TEXTURE_FILTER_LINEAR : SCE_GXM_TEXTURE_FILTER_POINT);
	vita2d_draw_texture_tint_part_scale(t->tex, dx, dy, sx, sy, sw, sh, dw / sw, dh / sh, tint);
}

void VitaGfx::clip(float x, float y, float w, float h)
{
	vita2d_enable_clipping();
	vita2d_set_clip_rectangle((int) floorf(x), (int) floorf(y), (int) ceilf(x + w) - 1, (int) ceilf(y + h) - 1);
}

void VitaGfx::clip_off() { vita2d_disable_clipping(); }

}  // namespace pk
