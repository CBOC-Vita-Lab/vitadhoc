/* See vita2d_fake.h. Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE). */
#include "vita2d_fake.h"
#include <vita2d.h>
#include <map>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <thread>

namespace {

const int W = 960, H = 544;

struct Tex {
	std::vector<uint8_t> data;
	unsigned w, h, stride, bpp;
	SceGxmTextureFormat fmt;
	SceGxmTextureFilter minf, magf;
	std::thread::id owner;
};

std::map<const vita2d_texture *, Tex *> g_tex;
std::vector<uint32_t> g_fb(W * H, 0xFF000000u);
std::vector<fakegxm::TexDraw> g_draws;
std::vector<void *> g_pool;
int g_bad = 0;
bool g_clip = false;
int g_cx0 = 0, g_cy0 = 0, g_cx1 = W - 1, g_cy1 = H - 1;

double lin(double c) { c /= 255.0; return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }
double lum(double r, double g, double b) { return 0.2126 * lin(r) + 0.7152 * lin(g) + 0.0722 * lin(b); }

void blend(int x, int y, float r, float g, float b, float a)      /* source-alpha over, the vita2d programs' blend */
{
	if (x < 0 || y < 0 || x >= W || y >= H) return;
	if (g_clip && (x < g_cx0 || x > g_cx1 || y < g_cy0 || y > g_cy1)) return;
	uint32_t d = g_fb[(size_t) y * W + x];
	float dr = (float) (d & 255), dg = (float) ((d >> 8) & 255), db = (float) ((d >> 16) & 255);
	float R = r * a + dr * (1 - a), G = g * a + dg * (1 - a), B = b * a + db * (1 - a);
	g_fb[(size_t) y * W + x] = 0xFF000000u | ((uint32_t) (B + 0.5f) << 16) | ((uint32_t) (G + 0.5f) << 8) | (uint32_t) (R + 0.5f);
}

struct V { float x, y, r, g, b, a; };
V mk(const vita2d_color_vertex &v)
{
	V o = { v.x, v.y, (float) (v.color & 255), (float) ((v.color >> 8) & 255), (float) ((v.color >> 16) & 255), (float) (v.color >> 24) / 255.0f };
	return o;
}

void tri(const V &a, const V &b, const V &c)
{
	float minx = fminf(a.x, fminf(b.x, c.x)), maxx = fmaxf(a.x, fmaxf(b.x, c.x));
	float miny = fminf(a.y, fminf(b.y, c.y)), maxy = fmaxf(a.y, fmaxf(b.y, c.y));
	float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
	if (fabsf(area) < 1e-6f) return;
	for (int y = (int) floorf(miny); y <= (int) ceilf(maxy); y++)
		for (int x = (int) floorf(minx); x <= (int) ceilf(maxx); x++) {
			float px = (float) x + 0.5f, py = (float) y + 0.5f;
			float w0 = ((b.x - px) * (c.y - py) - (b.y - py) * (c.x - px)) / area;
			float w1 = ((c.x - px) * (a.y - py) - (c.y - py) * (a.x - px)) / area;
			float w2 = 1 - w0 - w1;
			if (w0 < 0 || w1 < 0 || w2 < 0) continue;
			blend(x, y, a.r * w0 + b.r * w1 + c.r * w2, a.g * w0 + b.g * w1 + c.g * w2, a.b * w0 + b.b * w1 + c.b * w2,
			      a.a * w0 + b.a * w1 + c.a * w2);
		}
}

Tex *get(const vita2d_texture *t) { std::map<const vita2d_texture *, Tex *>::iterator i = g_tex.find(t); return i == g_tex.end() ? 0 : i->second; }

}  // namespace

namespace fakegxm {

Swizzle decode_u8(unsigned format)
{
	/* the names read A,B,G,R left to right, like U8U8U8U8_ABGR: R111 = alpha from the byte, colour white;
	 * 111R = alpha 1, colour (1,1,byte) */
	Swizzle s = { true, false, false, false };
	if (format == SCE_GXM_TEXTURE_FORMAT_U8_R111) { s.alpha_from_data = true; }
	else if (format == SCE_GXM_TEXTURE_FORMAT_U8_111R) { s.rgb_from_data = true; s.alpha_const1 = true; }
	else if (format == SCE_GXM_TEXTURE_FORMAT_U8_1RRR) { s.rgb_from_data = true; s.alpha_const1 = true; }
	else if (format == SCE_GXM_TEXTURE_FORMAT_U8_RRRR) { s.rgb_from_data = true; s.alpha_from_data = true; }
	else if (format == SCE_GXM_TEXTURE_FORMAT_U8_000R) { s.rgb_from_data = true; s.alpha_const1 = true; }
	else if (format == SCE_GXM_TEXTURE_FORMAT_U8_0RRR) { s.rgb_from_data = true; }
	else s.known = false;
	return s;
}

void reset()
{
	for (std::map<const vita2d_texture *, Tex *>::iterator i = g_tex.begin(); i != g_tex.end(); ++i) { delete i->second; free((void *) i->first); }
	g_tex.clear();
	for (size_t i = 0; i < g_pool.size(); i++) free(g_pool[i]);
	g_pool.clear();
	g_fb.assign((size_t) W * H, 0xFF000000u);
	g_draws.clear();
	g_bad = 0; g_clip = false;
}
void clear(uint32_t abgr) { g_fb.assign((size_t) W * H, abgr | 0xFF000000u); }
const std::vector<TexDraw> &tex_draws() { return g_draws; }
const uint32_t *framebuffer() { return &g_fb[0]; }
int bad_calls() { return g_bad; }

}  // namespace fakegxm

extern "C" {

void *vita2d_pool_memalign(unsigned int size, unsigned int alignment)
{
	(void) alignment;
	void *p = calloc(1, size);
	g_pool.push_back(p);
	return p;
}

void vita2d_draw_rectangle(float x, float y, float w, float h, unsigned int color)
{
	V a = mk({ x, y, 0, color }), b = mk({ x + w, y, 0, color }), c = mk({ x, y + h, 0, color }), d = mk({ x + w, y + h, 0, color });
	tri(a, b, c); tri(b, d, c);
}

void vita2d_draw_array(SceGxmPrimitiveType mode, const vita2d_color_vertex *v, size_t n)
{
	if (!v) { g_bad++; return; }
	if (mode == SCE_GXM_PRIMITIVE_TRIANGLES) { for (size_t i = 0; i + 2 < n; i += 3) tri(mk(v[i]), mk(v[i + 1]), mk(v[i + 2])); }
	else if (mode == SCE_GXM_PRIMITIVE_TRIANGLE_STRIP) { for (size_t i = 0; i + 2 < n; i++) tri(mk(v[i]), mk(v[i + 1]), mk(v[i + 2])); }
	else if (mode == SCE_GXM_PRIMITIVE_TRIANGLE_FAN) { for (size_t i = 1; i + 1 < n; i++) tri(mk(v[0]), mk(v[i]), mk(v[i + 1])); }
	else g_bad++;
}

vita2d_texture *vita2d_create_empty_texture_format(unsigned int w, unsigned int h, SceGxmTextureFormat format)
{
	Tex *t = new Tex();
	t->w = w; t->h = h; t->fmt = format;
	t->bpp = (format == SCE_GXM_TEXTURE_FORMAT_A8B8G8R8 || format == SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR) ? 4
	       : (format == SCE_GXM_TEXTURE_FORMAT_U5U6U5_RGB || format == SCE_GXM_TEXTURE_FORMAT_U5U6U5_BGR) ? 2 : 1;
	t->stride = ((w + 7) & ~7u) * t->bpp;
	t->data.assign((size_t) t->stride * h, 0);
	t->minf = t->magf = SCE_GXM_TEXTURE_FILTER_POINT;
	t->owner = std::this_thread::get_id();
	vita2d_texture *h2 = (vita2d_texture *) calloc(1, sizeof(vita2d_texture));
	g_tex[h2] = t;
	return h2;
}
void vita2d_free_texture(vita2d_texture *t)
{
	Tex *x = get(t);
	if (!x) { g_bad++; return; }
	delete x; g_tex.erase(t); free(t);
}
void vita2d_wait_rendering_done() {}
static int g_mul_calls;
int vita2d_set_blend_mode_mul(int enable) { g_mul_calls += enable ? 1 : 0; return 1; }      /* the CRT mask's multiply draw: not modelled by this rasteriser */
void *vita2d_texture_get_datap(const vita2d_texture *t) { Tex *x = get(t); if (!x) { g_bad++; return 0; } if (x->owner != std::this_thread::get_id()) x->owner = std::thread::id(); return &x->data[0]; }
unsigned int vita2d_texture_get_stride(const vita2d_texture *t) { Tex *x = get(t); return x ? x->stride : 0; }
void vita2d_texture_set_filters(vita2d_texture *t, SceGxmTextureFilter a, SceGxmTextureFilter b) { Tex *x = get(t); if (x) { x->minf = a; x->magf = b; } }
void vita2d_enable_clipping() { g_clip = true; }
void vita2d_disable_clipping() { g_clip = false; }
void vita2d_set_clip_rectangle(int x0, int y0, int x1, int y1) { g_cx0 = x0; g_cy0 = y0; g_cx1 = x1; g_cy1 = y1; }

void vita2d_draw_texture_tint_part_scale(const vita2d_texture *t, float x, float y, float tx, float ty, float tw, float th, float xs, float ys, unsigned int color)
{
	Tex *T = get(t);
	if (!T) { g_bad++; return; }
	const int x0 = (int) floorf(x + 0.5f), y0 = (int) floorf(y + 0.5f), x1 = (int) floorf(x + tw * xs + 0.5f), y1 = (int) floorf(y + th * ys + 0.5f);
	fakegxm::TexDraw d;
	d.format = T->fmt; d.tint = color; d.dx = x; d.dy = y; d.dw = tw * xs; d.dh = th * ys;
	d.uploaded = false;
	for (size_t i = 0; i < T->data.size(); i++) if (T->data[i]) { d.uploaded = true; break; }
	d.same_thread = T->owner == std::this_thread::get_id();
	double sum = 0; int cnt = 0;
	for (int py = y0; py < y1; py++) for (int px = x0; px < x1; px++)
		if (px >= 0 && py >= 0 && px < W && py < H) { uint32_t c = g_fb[(size_t) py * W + px]; sum += lum(c & 255, (c >> 8) & 255, (c >> 16) & 255); cnt++; }
	d.bg_lum = cnt ? sum / cnt : 0;
	d.ink_lum = lum(color & 255, (color >> 8) & 255, (color >> 16) & 255);
	g_draws.push_back(d);
	fakegxm::Swizzle sw = fakegxm::decode_u8(T->fmt);
	const float ta = (float) (color >> 24) / 255.0f;
	for (int py = y0; py < y1; py++)
		for (int px = x0; px < x1; px++) {
			int sx = (int) floorf(tx + ((float) px + 0.5f - x) / xs), sy = (int) floorf(ty + ((float) py + 0.5f - y) / ys);
			if (sx < 0 || sy < 0 || sx >= (int) T->w || sy >= (int) T->h) continue;
			float r, g, b, a;
			if (T->bpp == 1) {
				float v = (float) T->data[(size_t) sy * T->stride + sx];
				if (!sw.known) { g_bad++; continue; }
				float cr = sw.rgb_from_data ? v : 255, cg = (sw.rgb_from_data && T->fmt != SCE_GXM_TEXTURE_FORMAT_U8_111R && T->fmt != SCE_GXM_TEXTURE_FORMAT_U8_000R) ? v : 255;
				float cb = cg;
				if (T->fmt == SCE_GXM_TEXTURE_FORMAT_U8_000R) { cg = 0; cb = 0; }
				if (T->fmt == SCE_GXM_TEXTURE_FORMAT_U8_111R) { cg = 255; cb = 255; }
				a = sw.alpha_from_data ? v / 255.0f : sw.alpha_const1 ? 1.0f : 0.0f;
				r = cr * (color & 255) / 255.0f; g = cg * ((color >> 8) & 255) / 255.0f; b = cb * ((color >> 16) & 255) / 255.0f;
			} else if (T->bpp == 4) {
				const uint8_t *p = &T->data[(size_t) sy * T->stride + (size_t) sx * 4];
				r = p[0] * (color & 255) / 255.0f; g = p[1] * ((color >> 8) & 255) / 255.0f; b = p[2] * ((color >> 16) & 255) / 255.0f; a = p[3] / 255.0f;
			} else {
				uint16_t p = (uint16_t) (T->data[(size_t) sy * T->stride + (size_t) sx * 2] | (T->data[(size_t) sy * T->stride + (size_t) sx * 2 + 1] << 8));
				float hi = (float) ((p >> 11) & 31) * 255.0f / 31.0f, lo = (float) (p & 31) * 255.0f / 31.0f, mid = (float) ((p >> 5) & 63) * 255.0f / 63.0f;
				bool rgb = T->fmt == SCE_GXM_TEXTURE_FORMAT_U5U6U5_RGB;
				r = (rgb ? hi : lo) * (color & 255) / 255.0f; g = mid * ((color >> 8) & 255) / 255.0f; b = (rgb ? lo : hi) * ((color >> 16) & 255) / 255.0f; a = 1;
			}
			blend(px, py, r, g, b, a * ta);
		}
}

}  // extern "C"
