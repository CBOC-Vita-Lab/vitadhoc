/*
 * One-line toasts: "State saved", "Could not save the game: the memory card is full".
 * Errors stay longer than good news. A new toast replaces the one showing.
 *
 * Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE).
 */
#ifndef PK_NOTICE_H
#define PK_NOTICE_H

#include "pk_gfx.h"
#include <string>

namespace pk {

enum NoticeKind { NOTICE_INFO = 0, NOTICE_OK, NOTICE_ERROR };

class Notices {
public:
	Notices() : active_(false), kind_(NOTICE_INFO), age_ms_(0), life_ms_(0) {}
	/* ms <= 0 picks the default for the kind (2200 ms, 2200 ms, 5000 ms) */
	void push(const std::string &text, NoticeKind kind = NOTICE_INFO, int ms = 0);
	void update(int dt_ms);
	bool active() const { return active_; }
	const std::string &text() const { return text_; }
	NoticeKind kind() const { return kind_; }
	/* 0..1: fade in and out */
	float alpha() const;
	void clear() { active_ = false; text_.clear(); }
	/* draws a pill near the top of the screen. `accent` colours the dot of an OK notice. */
	void draw(Gfx *g, Text *t, uint32_t accent, float top = 26, bool right = false) const;
private:
	bool active_;
	std::string text_;
	NoticeKind kind_;
	int age_ms_, life_ms_;
};

}  // namespace pk

#endif
