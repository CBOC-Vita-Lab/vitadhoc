/*
 * playerkit drawing interface. The menu, the notices and the thumbnails draw
 * only through this; a player gives it a binding (vita2d on the console, a
 * software framebuffer on Linux). Coordinates are native pixels, 960 x 544,
 * origin top-left. Colours are 0xAABBGGRR (byte order R,G,B,A in memory on a
 * little-endian machine), made with PK_RGBA().
 *
 * Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE).
 */
#ifndef PK_GFX_H
#define PK_GFX_H

#include <stddef.h>
#include <stdint.h>
#include <string>

namespace pk {

#define PK_RGBA(r, g, b, a) ((uint32_t) (((uint32_t) (a) << 24) | ((uint32_t) (b) << 16) | ((uint32_t) (g) << 8) | (uint32_t) (r)))
/* 0xRRGGBB and an alpha, the way the main app writes colours */
#define PK_HEX(rgb, a) PK_RGBA((((uint32_t) (rgb) >> 16) & 0xFF), (((uint32_t) (rgb) >> 8) & 0xFF), ((uint32_t) (rgb) & 0xFF), (a))
#define PK_ALPHA(c, a) (((c) & 0x00FFFFFFu) | ((uint32_t) (a) << 24))

enum { PK_SCREEN_W = 960, PK_SCREEN_H = 544 };

/* Pixel formats of a texture. */
enum TexFormat {
	TEX_A8,        /* coverage only: text; drawn tinted by the colour */
	TEX_RGB565,    /* libretro order: red in the high bits (a game's frame) */
	TEX_BGR565,    /* the .thumb/.shot order: blue in the high bits */
	TEX_RGBA8      /* bytes R,G,B,A */
};

struct Texture;    /* opaque; owned by the gfx that made it */

class Gfx {
public:
	virtual ~Gfx() {}
	virtual int width() const { return PK_SCREEN_W; }
	virtual int height() const { return PK_SCREEN_H; }

	/* shapes; alpha blends over what is there */
	virtual void fill_rect(float x, float y, float w, float h, uint32_t rgba) = 0;
	virtual void fill_rrect(float x, float y, float w, float h, float r, uint32_t rgba) = 0;
	/* an outline t px thick, inside the rect */
	virtual void stroke_rrect(float x, float y, float w, float h, float r, float t, uint32_t rgba) = 0;
	virtual void fill_circle(float cx, float cy, float r, uint32_t rgba) = 0;
	virtual void fill_tri(float x0, float y0, float x1, float y1, float x2, float y2, uint32_t rgba) = 0;
	/* vertical gradient, top colour to bottom colour */
	virtual void gradient_v(float x, float y, float w, float h, uint32_t top, uint32_t bottom) = 0;

	/* textures; tex_update copies w x h pixels (stride in bytes) into the texture */
	virtual Texture *tex_create(int w, int h, TexFormat fmt) = 0;
	virtual void tex_update(Texture *t, const void *pixels, int stride_bytes) = 0;
	virtual void tex_free(Texture *t) = 0;
	/* draws the part (sx,sy,sw,sh) of t into (dx,dy,dw,dh); tint multiplies (white = as is);
	 * smooth = bilinear, else nearest */
	virtual void tex_draw(Texture *t, float sx, float sy, float sw, float sh,
	                      float dx, float dy, float dw, float dh, uint32_t tint, bool smooth) = 0;

	/* clip to a rectangle (lists that scroll); clip_off() lifts it */
	virtual void clip(float x, float y, float w, float h) = 0;
	virtual void clip_off() = 0;
};

/* ---- type -------------------------------------------------------------- */
enum Face { F_CAPTION = 0, F_BODY, F_LABEL, F_SUB, F_TITLE, F_COUNT };

/* A font atlas: what a player can supply instead of the built-in one. Glyphs are
 * printable ASCII (32..126); the atlas is A8, width x height. */
struct Glyph {
	uint16_t ax, ay;     /* position in the atlas */
	uint8_t w, h;        /* size in the atlas */
	int8_t ox, oy;       /* offset from the pen (x) and the line top (y) */
	float adv;           /* advance */
};
struct FaceData {
	const uint8_t *atlas;   /* A8, atlas_w x atlas_h */
	int atlas_w, atlas_h;
	int line_h, ascent;
	Glyph g[95];
};
/* the built-in faces (Inter and Outfit rasterised at 20/24/24/30/40 px) */
const FaceData *builtin_face(int face);

/* Text drawing on top of any Gfx: makes one atlas texture per face on first use. */
class Text {
public:
	explicit Text(Gfx *g);
	~Text();
	/* replace a face (the player's own atlas); the data must outlive the Text */
	void set_face(int face, const FaceData *fd);
	/* draws with the line box's top-left at x,y; returns the pen x after it */
	float draw(int face, float x, float y, const char *s, uint32_t rgba);
	float width(int face, const char *s);
	int line_h(int face) const;
	int ascent(int face) const;
	void draw_center(int face, float cx, float y, const char *s, uint32_t rgba);
	void draw_right(int face, float xr, float y, const char *s, uint32_t rgba);
	/* cut s so it fits max_w, ending in "..." when it was cut */
	std::string fit(int face, const std::string &s, float max_w);
	/* words wrapped to max_w, at most max_lines; returns the lines drawn */
	int wrap(int face, float x, float y, float max_w, int max_lines, float line_gap, const std::string &s, uint32_t rgba);
	struct Impl;
private:
	Impl *p_;
};

}  // namespace pk

#endif
