/* Text drawing over a Gfx. Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE).
 *
 * Printable ASCII comes from the baked atlases (pk_font_data.cpp). Everything
 * else (accented letters, Greek, Cyrillic, Japanese, Korean, fullwidth forms)
 * is drawn from glyphs rasterised on demand out of the outline fonts of the
 * package (pk_glyphs.h): one small atlas per face, made the first time such a
 * character is measured. Strings are UTF-8; no function here ever cuts, wraps
 * or measures inside a character. */
#include "pk_gfx.h"
#include "pk_glyphs.h"
#include "pk_i18n.h"
#include "pk_utf8.h"
#include "pk_uilook.h"
#include <math.h>
#include <string.h>
#include <vector>

namespace pk {

namespace {
/* the faces' sizes (pk_font_data.cpp / gen_font.cpp), their weight and the
 * synthetic semibold given to the CJK glyphs of the heavier ones */
struct FaceInfo { int px; int semibold; int embolden; };
const FaceInfo kFace[F_COUNT] = {
	{ 20, 0, 0 },      /* F_CAPTION: Inter Regular */
	{ 24, 0, 0 },      /* F_BODY    */
	{ 24, 1, 60 },     /* F_LABEL: Inter SemiBold */
	{ 30, 1, 75 },     /* F_SUB: Outfit Medium    */
	{ 40, 1, 100 }     /* F_TITLE: Outfit SemiBold */
};
const int kCacheBudget = 160 * 1024;     /* bytes of atlas per face, at most */
}  // namespace

struct GlyphSide {
	Text::Impl *impl;
	int face;
};

struct Text::Impl {
	Gfx *gfx;
	const FaceData *fd[F_COUNT];
	Texture *tex[F_COUNT];
	bool tried[F_COUNT];
	/* the on-demand glyphs, per face */
	pk_gcache *gc[F_COUNT];
	Texture *gtex[F_COUNT];
	bool gtried[F_COUNT];
	GlyphSide side[F_COUNT];
	int pct;           /* 100 Normal, 120 Large */
	bool contrast;
};

static void glyph_upload(void *user, int x, int y, int w, int h, const uint8_t *pix, int pitch)
{
	GlyphSide *s = (GlyphSide *) user;
	if (s->impl->gtex[s->face]) s->impl->gfx->tex_update_rect(s->impl->gtex[s->face], x, y, w, h, pix, pitch);
}

Text::Text(Gfx *g) : p_(new Impl)
{
	p_->gfx = g;
	for (int i = 0; i < F_COUNT; i++) {
		p_->fd[i] = builtin_face(i); p_->tex[i] = 0; p_->tried[i] = false;
		p_->gc[i] = 0; p_->gtex[i] = 0; p_->gtried[i] = false;
		p_->side[i].impl = p_; p_->side[i].face = i;
	}
	p_->pct = 100; p_->contrast = false;
}

Text::~Text()
{
	for (int i = 0; i < F_COUNT; i++) {
		if (p_->tex[i]) p_->gfx->tex_free(p_->tex[i]);
		if (p_->gtex[i]) p_->gfx->tex_free(p_->gtex[i]);
		pk_gc_free(p_->gc[i]);
	}
	delete p_;
}

void Text::frame()
{
	pk_gc_tick();
	/* the Chinese forms of the shared Han characters for a Chinese interface */
	pk_glyphs_set_region(i18n_current() == LANG_ZHS ? PK_REGION_SC : PK_REGION_JP);
}

void Text::set_look(int pct, bool contrast) { p_->pct = pct < 50 ? 50 : pct > 200 ? 200 : pct; p_->contrast = contrast; }
int Text::pct() const { return p_->pct; }
bool Text::contrast() const { return p_->contrast; }

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

/* the face's on-demand cache, made on first use (and its atlas texture) */
static pk_gcache *gcache(Text::Impl *p, int face)
{
	if (p->gc[face] || p->gtried[face] || !p->fd[face]) return p->gc[face];
	p->gtried[face] = true;
	pk_gc_spec sp;
	memset(&sp, 0, sizeof sp);
	sp.px = kFace[face].px;
	sp.ascent = p->fd[face]->ascent;
	sp.embolden = kFace[face].embolden;
	sp.chain[0] = pk_gfont_file(kFace[face].semibold ? PK_FONT_FILE_LATIN_B : PK_FONT_FILE_LATIN);
	sp.cjk_fallback = 1;                       /* then the region's CJK files (pk_glyphs_set_region) */
	sp.budget_bytes = kCacheBudget;
	sp.min_slots = 64;
	pk_gcache *c = pk_gc_new(&sp, glyph_upload, &p->side[face]);
	if (!c) return 0;
	int w = 0, h = 0;
	pk_gc_atlas_size(c, &w, &h);
	p->gtex[face] = p->gfx->tex_create(w, h, TEX_A8);
	if (!p->gtex[face]) { pk_gc_free(c); return 0; }
	p->gc[face] = c;
	return c;
}

/* the next character of *s; a character the end of the string cut off is skipped (nothing) */
static uint32_t next_cp(const char *&s, bool *skip)
{
	int n;
	uint32_t cp = pk_u8_next(s, &n);
	*skip = false;
	if (cp >= PK_U8_RAW && pk_u8_cut_tail(s)) {
		while (*s) s++;
		*skip = true;
		return 0;
	}
	s += n;
	return cp;
}

static const Glyph &qmark(const FaceData *fd) { return fd->g['?' - 32]; }

/* the advance of one character of the face */
static float char_adv(Text::Impl *p, int face, uint32_t cp)
{
	const FaceData *fd = p->fd[face];
	const float S = (float) p->pct * 0.01f;
	if (cp >= 32 && cp <= 126) return fd->g[cp - 32].adv * S;
	if (cp >= 0x80 && cp < PK_U8_RAW) {
		pk_gcache *c = gcache(p, face);
		float a;
		if (c && pk_gc_adv(c, cp, &a)) return a * S;
	}
	return qmark(fd).adv * S;
}

float Text::draw(int face, float x, float y, const char *s, uint32_t rgba)
{
	if (face < 0 || face >= F_COUNT || !s) return x;
	const FaceData *fd = p_->fd[face];
	if (!fd) return x;
	if (p_->contrast) rgba = pk_text_tint(rgba);
	const float S = (float) p_->pct * 0.01f;
	const bool sm = p_->pct != 100;                 /* a glyph at 120 % is resampled: smooth */
	Texture *t = (rgba >> 24) ? face_tex(p_, face) : 0;
	while (*s) {
		bool skip;
		uint32_t cp = next_cp(s, &skip);
		if (skip) break;
		if (cp >= 32 && cp <= 126) {
			const Glyph &g = fd->g[cp - 32];
			if (t && g.w > 0 && g.h > 0)
				p_->gfx->tex_draw(t, g.ax, g.ay, g.w, g.h, floorf(x + g.ox * S + 0.5f), floorf(y + g.oy * S + 0.5f), g.w * S, g.h * S, rgba, sm);
			x += g.adv * S;
			continue;
		}
		if (cp >= 0x80 && cp < PK_U8_RAW) {
			pk_gcache *c = gcache(p_, face);
			pk_gglyph g;
			bool ok = false;
			if (c && (rgba >> 24)) ok = pk_gc_get(c, cp, &g) != 0;
			else if (c) { g.w = g.h = 0; ok = pk_gc_adv(c, cp, &g.adv) != 0; }      /* invisible: only the pen moves */
			if (ok) {
				if (g.w > 0 && g.h > 0 && p_->gtex[face])
					p_->gfx->tex_draw(p_->gtex[face], g.x, g.y, g.w, g.h, floorf(x + g.ox * S + 0.5f), floorf(y + g.oy * S + 0.5f), g.w * S, g.h * S, rgba, sm);
				x += g.adv * S;
				continue;
			}
		}
		const Glyph &g = qmark(fd);                 /* a control, a stray byte, a character no font has */
		if (t && g.w > 0 && g.h > 0)
			p_->gfx->tex_draw(t, g.ax, g.ay, g.w, g.h, floorf(x + g.ox * S + 0.5f), floorf(y + g.oy * S + 0.5f), g.w * S, g.h * S, rgba, sm);
		x += g.adv * S;
	}
	return x;
}

float Text::width(int face, const char *s)
{
	if (face < 0 || face >= F_COUNT || !s || !p_->fd[face]) return 0;
	float w = 0;
	while (*s) {
		bool skip;
		uint32_t cp = next_cp(s, &skip);
		if (skip) break;
		w += char_adv(p_, face, cp);
	}
	return w;
}

int Text::line_h(int face) const { return (face >= 0 && face < F_COUNT && p_->fd[face]) ? (p_->fd[face]->line_h * p_->pct + 50) / 100 : 0; }
int Text::ascent(int face) const { return (face >= 0 && face < F_COUNT && p_->fd[face]) ? (p_->fd[face]->ascent * p_->pct + 50) / 100 : 0; }

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
	/* the widest prefix, cut between characters, that leaves room for the dots */
	const float dots = width(face, "...");
	const char *b = s.c_str();
	const char *p = b;
	float w = 0;
	size_t keep = 0;
	while (*p) {
		int n;
		uint32_t cp = pk_u8_next(p, &n);
		w += char_adv(p_, face, cp);
		if (w + dots > max_w) break;
		p += n;
		keep = (size_t) (p - b);
	}
	return s.substr(0, keep) + "...";
}

/* the text as pieces a line can be broken between: byte ranges of s, the spaces between them left out */
typedef std::pair<size_t, size_t> Span;

static void pieces(const std::string &s, std::vector<Span> &out)
{
	const char *b = s.c_str(), *p = b;
	size_t from = 0;
	bool have = false;
	uint32_t prev = 0;                              /* the character before, 0 after a space */
	while (*p) {
		int n;
		uint32_t cp = pk_u8_next(p, &n);
		if (cp == ' ') {
			if (have) { out.push_back(Span(from, (size_t) (p - b))); have = false; }
			prev = 0;
			p += n;
			continue;
		}
		if (have && prev && pk_u8_can_break(prev, cp)) { out.push_back(Span(from, (size_t) (p - b))); have = false; }
		if (!have) { from = (size_t) (p - b); have = true; }
		prev = cp;
		p += n;
	}
	if (have) out.push_back(Span(from, (size_t) (p - b)));
}

std::vector<Span> Text::break_lines(int face, float max_w, const std::string &s)
{
	std::vector<Span> pc, lines;
	pieces(s, pc);
	bool open = false;
	Span cur(0, 0);
	for (size_t i = 0; i < pc.size(); i++) {
		if (!open) { cur = pc[i]; open = true; continue; }
		if (width(face, s.substr(cur.first, pc[i].second - cur.first).c_str()) > max_w) {
			lines.push_back(cur);
			cur = pc[i];
		} else {
			cur.second = pc[i].second;
		}
	}
	if (open) lines.push_back(cur);
	return lines;
}

int Text::wrap(int face, float x, float y, float max_w, int max_lines, float line_gap, const std::string &s, uint32_t rgba)
{
	bool ascii = true;
	for (size_t k = 0; k < s.size(); k++) if ((unsigned char) s[k] >= 0x80) { ascii = false; break; }
	std::string line, word;
	int lines = 0;
	float lh = (float) line_h(face) + line_gap;
	if (!ascii) {
		std::vector<Span> L = break_lines(face, max_w, s);
		for (size_t i = 0; i < L.size() && (int) i < max_lines; i++) {
			if ((int) i == max_lines - 1 && L.size() > i) {                        /* the last line takes the rest, cut to fit */
				std::string rest = s.substr(L[i].first);
				while (!rest.empty() && rest[rest.size() - 1] == ' ') rest.erase(rest.size() - 1);
				draw(face, x, y + lines * lh, fit(face, rest, max_w).c_str(), rgba);
				return lines + 1;
			}
			draw(face, x, y + lines * lh, s.substr(L[i].first, L[i].second - L[i].first).c_str(), rgba);
			lines++;
		}
		return lines;
	}
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
