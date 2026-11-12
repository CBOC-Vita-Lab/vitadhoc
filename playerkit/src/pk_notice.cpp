/* See pk_notice.h. Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE). */
#include "pk_notice.h"

namespace pk {

void Notices::push(const std::string &text, NoticeKind kind, int ms)
{
	/* one line: control characters and line breaks become spaces */
	std::string t;
	for (size_t i = 0; i < text.size(); i++) t += (unsigned char) text[i] < 32 ? ' ' : text[i];
	if (t.empty()) return;
	text_ = t;
	kind_ = kind;
	age_ms_ = 0;
	life_ms_ = ms > 0 ? ms : (kind == NOTICE_ERROR ? 5000 : 2200);
	active_ = true;
}

void Notices::update(int dt_ms)
{
	if (!active_) return;
	age_ms_ += dt_ms < 0 ? 0 : dt_ms;
	if (age_ms_ >= life_ms_) active_ = false;
}

float Notices::alpha() const
{
	if (!active_) return 0;
	const float fade = 160.0f;
	float a = (float) age_ms_ / fade;
	float b = (float) (life_ms_ - age_ms_) / fade;
	float v = a < b ? a : b;
	return v < 0 ? 0 : v > 1 ? 1 : v;
}

void Notices::draw(Gfx *g, Text *t, uint32_t accent, float top, bool right) const
{
	if (!active_ || !g || !t) return;
	float a = alpha();
	if (a <= 0) return;
	int A = (int) (255 * a);
	const float maxw = right ? 560 : 820;
	/* one line, or two when it is long (a reason is never cut off) */
	std::string l1 = text_, l2;
	if (t->width(F_LABEL, text_.c_str()) > maxw) {
		size_t best = std::string::npos;
		for (size_t i = 0; i < text_.size(); i++)
			if (text_[i] == ' ' && t->width(F_LABEL, text_.substr(0, i).c_str()) <= maxw) best = i;
		if (best != std::string::npos) { l1 = text_.substr(0, best); l2 = t->fit(F_LABEL, text_.substr(best + 1), maxw); }
		else l1 = t->fit(F_LABEL, text_, maxw);
	}
	float tw = t->width(F_LABEL, l1.c_str());
	if (!l2.empty()) { float w2 = t->width(F_LABEL, l2.c_str()); if (w2 > tw) tw = w2; }
	const float lh = (float) t->line_h(F_LABEL);
	float w = tw + 72, h = l2.empty() ? 44 : 44 + lh;
	float x = right ? g->width() - 20 - w : (g->width() - w) * 0.5f, y = top;
	if (!l2.empty() && !right && y + h > g->height() - 8) y = g->height() - 8 - h;
	g->fill_rrect(x - 2, y + 5, w + 4, h, 22, PK_HEX(0x000000, A * 90 / 255));    /* a soft shadow */
	g->fill_rrect(x, y, w, h, 22, PK_HEX(0x1C2646, A * 245 / 255));
	uint32_t dot = kind_ == NOTICE_ERROR ? PK_HEX(0xFF6B6B, A) : kind_ == NOTICE_OK ? PK_ALPHA(accent, A) : PK_HEX(0xA7B0C6, A);
	float ty = y + (44 - lh) * 0.5f;
	g->fill_circle(x + 26, ty + lh * 0.5f, 5, dot);
	t->draw(F_LABEL, x + 42, ty, l1.c_str(), PK_HEX(0xF5F7FC, A));
	if (!l2.empty()) t->draw(F_LABEL, x + 42, ty + lh, l2.c_str(), PK_HEX(0xF5F7FC, A));
}

}  // namespace pk
