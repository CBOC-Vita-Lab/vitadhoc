/* Text drawing over a Gfx. Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE). */
#include "pk_gfx.h"
#include <math.h>
#include <string.h>

namespace pk {

struct Text::Impl {
	Gfx *gfx;
	const FaceData *fd[F_COUNT];
	Texture *tex[F_COUNT];
	bool tried[F_COUNT];
};

Text::Text(Gfx *g) : p_(new Impl)
{
	p_->gfx = g;
	for (int i = 0; i < F_COUNT; i++) { p_->fd[i] = builtin_face(i); p_->tex[i] = 0; p_->tried[i] = false; }
}

Text::~Text()
{
	for (int i = 0; i < F_COUNT; i++)
		if (p_->tex[i]) p_->gfx->tex_free(p_->tex[i]);
	delete p_;
}

void Text::set_face(int face, const FaceData *fd)
{
	if (face < 0 || face >= F_COUNT || !fd) return;
	if (p_->tex[face]) { p_->gfx->tex_free(p_->tex[face]); p_->tex[face] = 0; }
	p_->tried[face] = false;
	p_->fd[face] = fd;
}

static Texture *face_tex(Text::Impl *p, int face)
{
	if (!p->tex[face] && !p->tried[face] && p->fd[face]) {
		p->tried[face] = true;
		const FaceData *fd = p->fd[face];
		Texture *t = p->gfx->tex_create(fd->atlas_w, fd->atlas_h, TEX_A8);
		if (t) {
			p->gfx->tex_update(t, fd->atlas, fd->atlas_w);
			p->tex[face] = t;
		}
	}
	return p->tex[face];
}

float Text::draw(int face, float x, float y, const char *s, uint32_t rgba)
{
	if (face < 0 || face >= F_COUNT || !s) return x;
	const FaceData *fd = p_->fd[face];
	if (!fd) return x;
	Texture *t = (rgba >> 24) ? face_tex(p_, face) : 0;
	for (; *s; s++) {
		unsigned char c = (unsigned char) *s;
		if (c < 32 || c > 126) c = '?';
		const Glyph &g = fd->g[c - 32];
		if (t && g.w > 0 && g.h > 0)
			p_->gfx->tex_draw(t, g.ax, g.ay, g.w, g.h, floorf(x + g.ox + 0.5f), floorf(y + g.oy + 0.5f), g.w, g.h, rgba, false);
		x += g.adv;
	}
	return x;
}

float Text::width(int face, const char *s)
{
	if (face < 0 || face >= F_COUNT || !s || !p_->fd[face]) return 0;
	float w = 0;
	for (; *s; s++) {
		unsigned char c = (unsigned char) *s;
		if (c < 32 || c > 126) c = '?';
		w += p_->fd[face]->g[c - 32].adv;
	}
	return w;
}

int Text::line_h(int face) const { return (face >= 0 && face < F_COUNT && p_->fd[face]) ? p_->fd[face]->line_h : 0; }
int Text::ascent(int face) const { return (face >= 0 && face < F_COUNT && p_->fd[face]) ? p_->fd[face]->ascent : 0; }

void Text::draw_center(int face, float cx, float y, const char *s, uint32_t rgba)
{
	draw(face, floorf(cx - width(face, s) * 0.5f + 0.5f), y, s, rgba);
}

void Text::draw_right(int face, float xr, float y, const char *s, uint32_t rgba)
{
	draw(face, floorf(xr - width(face, s) + 0.5f), y, s, rgba);
}

std::string Text::fit(int face, const std::string &s, float max_w)
{
	if (width(face, s.c_str()) <= max_w) return s;
	std::string t = s;
	while (!t.empty() && width(face, (t + "...").c_str()) > max_w) t.erase(t.size() - 1);
	return t + "...";
}

int Text::wrap(int face, float x, float y, float max_w, int max_lines, float line_gap, const std::string &s, uint32_t rgba)
{
	std::string line, word;
	int lines = 0;
	float lh = (float) line_h(face) + line_gap;
	size_t i = 0;
	bool done = false;
	while (!done && lines < max_lines) {
		/* next word */
		word.clear();
		while (i < s.size() && s[i] != ' ') word += s[i++];
		while (i < s.size() && s[i] == ' ') i++;
		done = i >= s.size();
		std::string cand = line.empty() ? word : line + " " + word;
		if (!line.empty() && width(face, cand.c_str()) > max_w) {
			draw(face, x, y + lines * lh, line.c_str(), rgba);
			lines++;
			line = word;
			if (lines >= max_lines) return lines;
		} else {
			line = cand;
		}
	}
	if (!line.empty() && lines < max_lines) {
		std::string last = (lines == max_lines - 1) ? fit(face, line, max_w) : line;
		draw(face, x, y + lines * lh, last.c_str(), rgba);
		lines++;
	}
	return lines;
}

}  // namespace pk
