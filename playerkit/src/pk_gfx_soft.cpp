/* See pk_gfx_soft.h. Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE). */
#include "pk_gfx_soft.h"
#include "pk_util.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <algorithm>

namespace pk {

struct Texture {
	int w, h;
	TexFormat fmt;
	std::vector<uint8_t> data;   /* tightly packed */
	int bpp() const { return fmt == TEX_A8 ? 1 : fmt == TEX_RGBA8 ? 4 : 2; }
};

SoftGfx::SoftGfx(int w, int h) : w_(w), h_(h), fb_((size_t) w * h, 0xFF000000u), live_(0)
{
	clip_off();
}

SoftGfx::~SoftGfx() {}

void SoftGfx::clear(uint32_t rgba)
{
	std::fill(fb_.begin(), fb_.end(), rgba | 0xFF000000u);
}

void SoftGfx::clip(float x, float y, float w, float h)
{
	cx0_ = std::max(0, (int) floorf(x + 0.5f));
	cy0_ = std::max(0, (int) floorf(y + 0.5f));
	cx1_ = std::min(w_, (int) floorf(x + w + 0.5f));
	cy1_ = std::min(h_, (int) floorf(y + h + 0.5f));
}

void SoftGfx::clip_off()
{
	cx0_ = 0; cy0_ = 0; cx1_ = w_; cy1_ = h_;
}

void SoftGfx::blend(int x, int y, uint32_t c, float cov)
{
	if (x < cx0_ || y < cy0_ || x >= cx1_ || y >= cy1_) return;
	float a = ((c >> 24) & 0xFF) * (1.0f / 255.0f) * cov;
	if (a <= 0.0f) return;
	if (a > 1.0f) a = 1.0f;
	uint32_t &d = fb_[(size_t) y * w_ + x];
	int sr = c & 0xFF, sg = (c >> 8) & 0xFF, sb = (c >> 16) & 0xFF;
	int dr = d & 0xFF, dg = (d >> 8) & 0xFF, db = (d >> 16) & 0xFF;
	int r = (int) (sr * a + dr * (1 - a) + 0.5f);
	int g = (int) (sg * a + dg * (1 - a) + 0.5f);
	int b = (int) (sb * a + db * (1 - a) + 0.5f);
	d = 0xFF000000u | ((uint32_t) b << 16) | ((uint32_t) g << 8) | (uint32_t) r;
}

static float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

void SoftGfx::fill_rect(float x, float y, float w, float h, uint32_t c)
{
	if (w <= 0 || h <= 0) return;
	int x0 = (int) floorf(x), y0 = (int) floorf(y), x1 = (int) ceilf(x + w), y1 = (int) ceilf(y + h);
	for (int py = std::max(y0, cy0_); py < std::min(y1, cy1_); py++) {
		float cy = clamp01(std::min((float) py + 1, y + h) - std::max((float) py, y));
		for (int px = std::max(x0, cx0_); px < std::min(x1, cx1_); px++) {
			float cx = clamp01(std::min((float) px + 1, x + w) - std::max((float) px, x));
			blend(px, py, c, cx * cy);
		}
	}
}

/* signed distance to a rounded box centred at (cx,cy) with half sizes (hx,hy) */
static float sd_rbox(float px, float py, float cx, float cy, float hx, float hy, float r)
{
	float qx = fabsf(px - cx) - (hx - r), qy = fabsf(py - cy) - (hy - r);
	float ox = std::max(qx, 0.0f), oy = std::max(qy, 0.0f);
	return sqrtf(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0f) - r;
}

void SoftGfx::fill_rrect(float x, float y, float w, float h, float r, uint32_t c)
{
	if (w <= 0 || h <= 0) return;
	r = std::min(r, std::min(w, h) * 0.5f);
	if (r < 0.5f) { fill_rect(x, y, w, h, c); return; }
	float cx = x + w * 0.5f, cy = y + h * 0.5f;
	for (int py = std::max((int) floorf(y), cy0_); py < std::min((int) ceilf(y + h), cy1_); py++)
		for (int px = std::max((int) floorf(x), cx0_); px < std::min((int) ceilf(x + w), cx1_); px++) {
			float d = sd_rbox(px + 0.5f, py + 0.5f, cx, cy, w * 0.5f, h * 0.5f, r);
			blend(px, py, c, clamp01(0.5f - d));
		}
}

void SoftGfx::stroke_rrect(float x, float y, float w, float h, float r, float t, uint32_t c)
{
	if (w <= 0 || h <= 0 || t <= 0) return;
	r = std::min(r, std::min(w, h) * 0.5f);
	float cx = x + w * 0.5f, cy = y + h * 0.5f;
	for (int py = std::max((int) floorf(y), cy0_); py < std::min((int) ceilf(y + h), cy1_); py++)
		for (int px = std::max((int) floorf(x), cx0_); px < std::min((int) ceilf(x + w), cx1_); px++) {
			float d = sd_rbox(px + 0.5f, py + 0.5f, cx, cy, w * 0.5f, h * 0.5f, r);
			float outer = clamp01(0.5f - d), inner = clamp01(d + t + 0.5f);
			blend(px, py, c, outer * inner);
		}
}

void SoftGfx::fill_circle(float cx, float cy, float r, uint32_t c)
{
	for (int py = std::max((int) floorf(cy - r - 1), cy0_); py < std::min((int) ceilf(cy + r + 1), cy1_); py++)
		for (int px = std::max((int) floorf(cx - r - 1), cx0_); px < std::min((int) ceilf(cx + r + 1), cx1_); px++) {
			float dx = px + 0.5f - cx, dy = py + 0.5f - cy;
			blend(px, py, c, clamp01(r - sqrtf(dx * dx + dy * dy) + 0.5f));
		}
}

void SoftGfx::fill_tri(float x0, float y0, float x1, float y1, float x2, float y2, uint32_t c)
{
	float minx = std::min(x0, std::min(x1, x2)), maxx = std::max(x0, std::max(x1, x2));
	float miny = std::min(y0, std::min(y1, y2)), maxy = std::max(y0, std::max(y1, y2));
	float area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
	if (fabsf(area) < 1e-6f) return;
	float sgn = area > 0 ? 1.0f : -1.0f;
	const int N = 4;
	for (int py = std::max((int) floorf(miny), cy0_); py < std::min((int) ceilf(maxy), cy1_); py++)
		for (int px = std::max((int) floorf(minx), cx0_); px < std::min((int) ceilf(maxx), cx1_); px++) {
			int in = 0;
			for (int sy = 0; sy < N; sy++)
				for (int sx = 0; sx < N; sx++) {
					float fx = px + (sx + 0.5f) / N, fy = py + (sy + 0.5f) / N;
					float e0 = ((x1 - x0) * (fy - y0) - (y1 - y0) * (fx - x0)) * sgn;
					float e1 = ((x2 - x1) * (fy - y1) - (y2 - y1) * (fx - x1)) * sgn;
					float e2 = ((x0 - x2) * (fy - y2) - (y0 - y2) * (fx - x2)) * sgn;
					if (e0 >= 0 && e1 >= 0 && e2 >= 0) in++;
				}
			if (in) blend(px, py, c, (float) in / (N * N));
		}
}

static uint32_t lerp_col(uint32_t a, uint32_t b, float t)
{
	uint32_t o = 0;
	for (int s = 0; s < 32; s += 8) {
		float va = (a >> s) & 0xFF, vb = (b >> s) & 0xFF;
		o |= (uint32_t) (va + (vb - va) * t + 0.5f) << s;
	}
	return o;
}

void SoftGfx::gradient_v(float x, float y, float w, float h, uint32_t top, uint32_t bottom)
{
	if (w <= 0 || h <= 0) return;
	for (int py = std::max((int) floorf(y), cy0_); py < std::min((int) ceilf(y + h), cy1_); py++) {
		uint32_t c = lerp_col(top, bottom, clamp01((py + 0.5f - y) / h));
		fill_rect(x, (float) py, w, 1, c);
	}
}

Texture *SoftGfx::tex_create(int w, int h, TexFormat fmt)
{
	if (w <= 0 || h <= 0) return 0;
	Texture *t = new Texture;
	t->w = w; t->h = h; t->fmt = fmt;
	t->data.assign((size_t) w * h * t->bpp(), 0);
	live_++;
	return t;
}

void SoftGfx::tex_update(Texture *t, const void *pixels, int stride_bytes)
{
	if (!t || !pixels) return;
	size_t row = (size_t) t->w * t->bpp();
	for (int y = 0; y < t->h; y++) memcpy(&t->data[(size_t) y * row], (const uint8_t *) pixels + (size_t) y * stride_bytes, row);
}

void SoftGfx::tex_free(Texture *t)
{
	if (t) { delete t; live_--; }
}

/* colour of texel (x,y) as r,g,b in 0..255 and a in 0..1 */
static void texel(const Texture *t, int x, int y, float *r, float *g, float *b, float *a)
{
	x = x < 0 ? 0 : x >= t->w ? t->w - 1 : x;
	y = y < 0 ? 0 : y >= t->h ? t->h - 1 : y;
	const uint8_t *p = &t->data[((size_t) y * t->w + x) * t->bpp()];
	switch (t->fmt) {
	case TEX_A8: *r = *g = *b = 255; *a = p[0] * (1.0f / 255.0f); break;
	case TEX_RGBA8: *r = p[0]; *g = p[1]; *b = p[2]; *a = p[3] * (1.0f / 255.0f); break;
	case TEX_RGB565: {
		unsigned v = p[0] | (unsigned) p[1] << 8;
		*r = (float) ((v >> 11) * 255 / 31); *g = (float) (((v >> 5) & 63) * 255 / 63); *b = (float) ((v & 31) * 255 / 31); *a = 1;
		break;
	}
	default: {
		unsigned v = p[0] | (unsigned) p[1] << 8;
		*b = (float) ((v >> 11) * 255 / 31); *g = (float) (((v >> 5) & 63) * 255 / 63); *r = (float) ((v & 31) * 255 / 31); *a = 1;
		break;
	}
	}
}

void SoftGfx::tex_draw(Texture *t, float sx, float sy, float sw, float sh,
                       float dx, float dy, float dw, float dh, uint32_t tint, bool smooth)
{
	if (!t || dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;
	float tr = (tint & 0xFF) / 255.0f, tg = ((tint >> 8) & 0xFF) / 255.0f, tb = ((tint >> 16) & 0xFF) / 255.0f;
	uint32_t ta = tint >> 24;
	for (int py = std::max((int) floorf(dy), cy0_); py < std::min((int) ceilf(dy + dh), cy1_); py++)
		for (int px = std::max((int) floorf(dx), cx0_); px < std::min((int) ceilf(dx + dw), cx1_); px++) {
			float u = sx + (px + 0.5f - dx) * sw / dw, v = sy + (py + 0.5f - dy) * sh / dh;
			float r, g, b, a;
			if (!smooth) {
				texel(t, (int) floorf(u), (int) floorf(v), &r, &g, &b, &a);
			} else {
				float fu = u - 0.5f, fv = v - 0.5f;
				int iu = (int) floorf(fu), iv = (int) floorf(fv);
				float au = fu - iu, av = fv - iv;
				float c[4][4];
				texel(t, iu, iv, &c[0][0], &c[0][1], &c[0][2], &c[0][3]);
				texel(t, iu + 1, iv, &c[1][0], &c[1][1], &c[1][2], &c[1][3]);
				texel(t, iu, iv + 1, &c[2][0], &c[2][1], &c[2][2], &c[2][3]);
				texel(t, iu + 1, iv + 1, &c[3][0], &c[3][1], &c[3][2], &c[3][3]);
				float o[4];
				for (int k = 0; k < 4; k++)
					o[k] = (c[0][k] * (1 - au) + c[1][k] * au) * (1 - av) + (c[2][k] * (1 - au) + c[3][k] * au) * av;
				r = o[0]; g = o[1]; b = o[2]; a = o[3];
			}
			uint32_t col = PK_RGBA((int) (r * tr + 0.5f), (int) (g * tg + 0.5f), (int) (b * tb + 0.5f), ta);
			blend(px, py, col, a);
		}
}

/* ---- PNG ---------------------------------------------------------------- */

static void put32(std::vector<uint8_t> &o, uint32_t v)
{
	o.push_back((uint8_t) (v >> 24)); o.push_back((uint8_t) (v >> 16)); o.push_back((uint8_t) (v >> 8)); o.push_back((uint8_t) v);
}

static void chunk(std::vector<uint8_t> &o, const char *type, const std::vector<uint8_t> &data)
{
	put32(o, (uint32_t) data.size());
	size_t at = o.size();
	o.insert(o.end(), type, type + 4);
	o.insert(o.end(), data.begin(), data.end());
	put32(o, crc32(0, &o[at], 4 + data.size()));
}

bool write_png_rgb(const std::string &path, int w, int h, const uint8_t *rgb)
{
	std::vector<uint8_t> raw;
	raw.reserve((size_t) (w * 3 + 1) * h);
	for (int y = 0; y < h; y++) {
		raw.push_back(0);   /* filter: none */
		raw.insert(raw.end(), rgb + (size_t) y * w * 3, rgb + (size_t) (y + 1) * w * 3);
	}
	/* zlib stream of stored blocks */
	std::vector<uint8_t> z;
	z.push_back(0x78); z.push_back(0x01);
	for (size_t i = 0; i < raw.size();) {
		size_t n = std::min<size_t>(65535, raw.size() - i);
		z.push_back(i + n >= raw.size() ? 1 : 0);
		z.push_back((uint8_t) n); z.push_back((uint8_t) (n >> 8));
		z.push_back((uint8_t) ~n); z.push_back((uint8_t) (~n >> 8));
		z.insert(z.end(), raw.begin() + (long) i, raw.begin() + (long) (i + n));
		i += n;
	}
	put32(z, adler32(1, raw.data(), raw.size()));
	std::vector<uint8_t> png;
	static const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
	png.insert(png.end(), sig, sig + 8);
	std::vector<uint8_t> ihdr;
	put32(ihdr, (uint32_t) w); put32(ihdr, (uint32_t) h);
	ihdr.push_back(8); ihdr.push_back(2); ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0);
	chunk(png, "IHDR", ihdr);
	chunk(png, "IDAT", z);
	chunk(png, "IEND", std::vector<uint8_t>());
	FILE *f = fopen(path.c_str(), "wb");
	if (!f) return false;
	bool ok = fwrite(png.data(), 1, png.size(), f) == png.size();
	return fclose(f) == 0 && ok;
}

bool SoftGfx::write_png(const std::string &path) const
{
	std::vector<uint8_t> rgb((size_t) w_ * h_ * 3);
	for (size_t i = 0; i < (size_t) w_ * h_; i++) {
		rgb[i * 3] = fb_[i] & 0xFF; rgb[i * 3 + 1] = (fb_[i] >> 8) & 0xFF; rgb[i * 3 + 2] = (fb_[i] >> 16) & 0xFF;
	}
	return write_png_rgb(path, w_, h_, rgb.data());
}

}  // namespace pk
