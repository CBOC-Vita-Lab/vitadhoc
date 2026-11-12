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
	std::string s = t->fit(F_LABEL, text_, right ? 560 : 820);
	float w = t->width(F_LABEL, s.c_str()) + 72, h = 44;
	float x = right ? g->width() - 20 - w : (g->width() - w) * 0.5f, y = top;
	g->fill_rrect(x - 2, y + 5, w + 4, h, 22, PK_HEX(0x000000, A * 90 / 255));    /* a soft shadow */
	g->fill_rrect(x, y, w, h, 22, PK_HEX(0x1C2646, A * 245 / 255));
	uint32_t dot = kind_ == NOTICE_ERROR ? PK_HEX(0xFF6B6B, A) : kind_ == NOTICE_OK ? PK_ALPHA(accent, A) : PK_HEX(0xA7B0C6, A);
	g->fill_circle(x + 26, y + h * 0.5f, 5, dot);
	t->draw(F_LABEL, x + 42, y + (h - t->line_h(F_LABEL)) * 0.5f, s.c_str(), PK_HEX(0xF5F7FC, A));
}

}  // namespace pk
