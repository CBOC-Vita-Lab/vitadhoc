/* See pk_input.h. Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE). */
#include "pk_input.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

namespace pk {

/* ---- names ---------------------------------------------------------------- */

static const char *const kPhysNames[PH_COUNT] = {
	"UP", "DOWN", "LEFT", "RIGHT", "CROSS", "CIRCLE", "SQUARE", "TRIANGLE", "L", "R", "START", "SELECT",
	"LSTICK_UP", "LSTICK_DOWN", "LSTICK_LEFT", "LSTICK_RIGHT",
	"RSTICK_UP", "RSTICK_DOWN", "RSTICK_LEFT", "RSTICK_RIGHT",
	"BACK_TL", "BACK_TR", "BACK_BL", "BACK_BR"
};

static const char *const kPhysLabels[PH_COUNT] = {
	"D-pad up", "D-pad down", "D-pad left", "D-pad right", "Cross", "Circle", "Square", "Triangle", "L", "R", "Start", "Select",
	"Left stick up", "Left stick down", "Left stick left", "Left stick right",
	"Right stick up", "Right stick down", "Right stick left", "Right stick right",
	"Rear touch: top left", "Rear touch: top right", "Rear touch: bottom left", "Rear touch: bottom right"
};

const char *phys_name(int bit) { return (bit >= 0 && bit < PH_COUNT) ? kPhysNames[bit] : "?"; }

std::string phys_format(uint32_t mask)
{
	std::string s;
	for (int i = 0; i < PH_COUNT; i++)
		if (mask & (1u << i)) s += (s.empty() ? "" : "+") + std::string(kPhysNames[i]);
	return s.empty() ? "NONE" : s;
}

std::string phys_label(uint32_t mask, bool)
{
	std::string s;
	for (int i = 0; i < PH_COUNT; i++)
		if (mask & (1u << i)) s += (s.empty() ? "" : " + ") + std::string(kPhysLabels[i]);
	return s.empty() ? "Not set" : s;
}

bool phys_parse(const std::string &text, uint32_t *out)
{
	std::string t = trim(text);
	if (t.empty()) return false;
	uint32_t m = 0;
	size_t p = 0;
	bool any = false;
	while (p <= t.size()) {
		size_t c = t.find('+', p);
		if (c == std::string::npos) c = t.size();
		std::string w = trim(t.substr(p, c - p));
		for (size_t i = 0; i < w.size(); i++) if (w[i] >= 'a' && w[i] <= 'z') w[i] = (char) (w[i] - 32);
		if (w.empty()) return false;
		if (w == "NONE") {
			if (any || c != t.size()) return false;
			*out = 0;
			return true;
		}
		if (starts_with(w, "STICK_")) w = "LSTICK_" + w.substr(6);
		bool found = false;
		for (int i = 0; i < PH_COUNT; i++)
			if (w == kPhysNames[i]) { m |= 1u << i; found = true; break; }
		if (!found) return false;
		any = true;
		p = c + 1;
	}
	*out = m;
	return true;
}

/* ---- models --------------------------------------------------------------- */

const PadModel &model_snes()
{
	static const PadModel m = {
		"snes", 4, 12,
		{
			{ "b", "B", 1u << 0, PH_CROSS, PH_CROSS },
			{ "a", "A", 1u << 8, PH_CIRCLE, PH_CIRCLE },
			{ "y", "Y", 1u << 1, PH_SQUARE, PH_SQUARE },
			{ "x", "X", 1u << 9, PH_TRIANGLE, PH_TRIANGLE },
			{ "l", "L", 1u << 10, PH_L, PH_L },
			{ "r", "R", 1u << 11, PH_R, PH_R },
			{ "start", "Start", 1u << 3, PH_START, PH_START },
			{ "select", "Select", 1u << 2, PH_SELECT, PH_SELECT },
			{ "up", "Up", 1u << 4, PH_UP, PH_UP },
			{ "down", "Down", 1u << 5, PH_DOWN, PH_DOWN },
			{ "left", "Left", 1u << 6, PH_LEFT, PH_LEFT },
			{ "right", "Right", 1u << 7, PH_RIGHT, PH_RIGHT }
		},
		1, 0, 0, 0, true
	};
	return m;
}

const PadModel &model_nes()
{
	static const PadModel m = {
		"nes", 4, 8,
		{
			{ "a", "A", 1u << 0, PH_CROSS, PH_CIRCLE },
			{ "b", "B", 1u << 1, PH_CIRCLE, PH_CROSS },
			{ "select", "Select", 1u << 2, PH_SELECT, PH_SELECT },
			{ "start", "Start", 1u << 3, PH_START, PH_START },
			{ "up", "Up", 1u << 4, PH_UP, PH_UP },
			{ "down", "Down", 1u << 5, PH_DOWN, PH_DOWN },
			{ "left", "Left", 1u << 6, PH_LEFT, PH_LEFT },
			{ "right", "Right", 1u << 7, PH_RIGHT, PH_RIGHT },
			{ 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0 }
		},
		0, 1, PH_TRIANGLE, PH_SQUARE, false
	};
	return m;
}

const PadModel *model_by_name(const std::string &c)
{
	if (c == "snes") return &model_snes();
	if (c == "nes") return &model_nes();
	return 0;
}

/* ---- actions ----------------------------------------------------------------- */

static const char *const kSpecialKeys[ACT_COUNT - ACT_TURBO_A] = {
	"bind_turbo_a", "bind_turbo_b", "bind_ff", "bind_quicksave", "bind_quickload", "bind_screenshot", "bind_rewind"
};
static const char *const kSpecialLabels[ACT_COUNT - ACT_TURBO_A] = {
	"Turbo A", "Turbo B", "Fast-forward", "Quick save", "Quick load", "Screenshot", "Rewind"
};

const char *action_key(const PadModel &m, int a)
{
	static char buf[32];
	if (a >= 0 && a < m.n && a < 12) { snprintf(buf, sizeof buf, "bind_%s", m.btn[a].key); return buf; }
	if (a >= ACT_TURBO_A && a < ACT_COUNT) return kSpecialKeys[a - ACT_TURBO_A];
	return "";
}

const char *action_label(const PadModel &m, int a)
{
	if (a >= 0 && a < m.n) return m.btn[a].label;
	if (a >= ACT_TURBO_A && a < ACT_COUNT) return kSpecialLabels[a - ACT_TURBO_A];
	return "";
}

const char *stick_mode_name(int m) { return m == STICK_MOVE ? "move" : m == STICK_CUSTOM ? "custom" : "off"; }

static bool stick_mode_parse(const std::string &s, int *out)
{
	std::string v = lower(trim(s));
	if (v == "off") *out = STICK_OFF;
	else if (v == "move" || v == "dpad" || v == "d-pad") *out = STICK_MOVE;
	else if (v == "custom" || v == "buttons") *out = STICK_CUSTOM;
	else return false;
	return true;
}

/* ---- profile ------------------------------------------------------------------ */

Profile::Profile()
{
	memset(bind, 0, sizeof bind);
}

Profile profile_defaults(const PadModel &m, bool enter_cross)
{
	Profile p;
	for (int pl = 0; pl < MAX_PLAYERS; pl++)
		for (int a = 0; a < m.n; a++)
			p.bind[pl][a] = enter_cross ? m.btn[a].default_phys : m.btn[a].default_phys_circle;
	p.bind[0][ACT_TURBO_A] = m.turbo_a_default;
	p.bind[0][ACT_TURBO_B] = m.turbo_b_default;
	p.bind[0][ACT_FF] = PH_RS_RIGHT;
	p.bind[0][ACT_QSAVE] = PH_SELECT | PH_L;
	p.bind[0][ACT_QLOAD] = PH_SELECT | PH_R;
	p.bind[0][ACT_SHOT] = 0;
	p.bind[0][ACT_REWIND] = PH_RS_LEFT;
	return p;
}

static int popcount(uint32_t v) { int n = 0; while (v) { n += (int) (v & 1); v >>= 1; } return n; }

const char *assign_result_text(AssignResult r)
{
	switch (r) {
	case ASSIGN_OK: return "";
	case ASSIGN_BAD_ACTION: return "No such button";
	case ASSIGN_SINGLE_ONLY: return "A game button takes one button";
	case ASSIGN_RESERVED: return "Start + Select opens the menu; pick other buttons";
	case ASSIGN_TOO_MANY: return "A shortcut takes at most four buttons";
	default: return "Only player 1 has shortcuts";
	}
}

AssignResult profile_assign(Profile &p, const PadModel &m, int player, int action, uint32_t phys, int *stolen)
{
	if (stolen) *stolen = -1;
	if (player < 0 || player >= MAX_PLAYERS || action < 0 || action >= ACT_COUNT) return ASSIGN_BAD_ACTION;
	if (action >= m.n && action < ACT_TURBO_A) return ASSIGN_BAD_ACTION;
	if (action_is_shortcut(action) && player != 0) return ASSIGN_NOT_PLAYER1;
	if (action_is_turbo(action) && player != 0) return ASSIGN_NOT_PLAYER1;
	if (action_is_turbo(action) && action == ACT_TURBO_A && m.turbo_a < 0) return ASSIGN_BAD_ACTION;
	if (action_is_turbo(action) && action == ACT_TURBO_B && m.turbo_b < 0) return ASSIGN_BAD_ACTION;
	phys &= PH_ALL;
	if (action_is_game(m, action) || action_is_turbo(action)) {
		if (popcount(phys) > 1) return ASSIGN_SINGLE_ONLY;
	} else {
		if ((phys & PH_MENU_CHORD) == PH_MENU_CHORD) return ASSIGN_RESERVED;
		if (popcount(phys) > 4) return ASSIGN_TOO_MANY;
	}
	if (phys) {
		/* the input is taken from whichever action of this player (in the same family) had exactly it */
		for (int a = 0; a < ACT_COUNT; a++) {
			if (a == action || (a >= m.n && a < ACT_TURBO_A)) continue;
			bool same_family = (action_is_game(m, action) || action_is_turbo(action)) == (action_is_game(m, a) || action_is_turbo(a));
			if (same_family && p.bind[player][a] == phys) {
				p.bind[player][a] = 0;
				if (stolen) *stolen = a;
				break;
			}
		}
	}
	p.bind[player][action] = phys;
	return ASSIGN_OK;
}

Ini profile_to_ini(const Profile &p, const PadModel &m)
{
	Ini i;
	i.set_int("version", 1);
	i.set("console", m.console);
	i.set("own", p.own ? "1" : "0");
	i.set("stick", stick_mode_name(p.stick_left));
	i.set("rstick", stick_mode_name(p.stick_right));
	i.set("back_touch", p.back_touch ? "1" : "0");
	i.set_int("deadzone", p.stick_on);
	for (int pl = 0; pl < MAX_PLAYERS; pl++) {
		std::string pre = pl == 0 ? "" : strf("p%d.", pl + 1);
		for (int a = 0; a < m.n; a++) i.set(pre + action_key(m, a), phys_format(p.bind[pl][a]));
		if (pl == 0)
			for (int a = ACT_TURBO_A; a < ACT_COUNT; a++) {
				if (a == ACT_TURBO_A && m.turbo_a < 0) continue;
				if (a == ACT_TURBO_B && m.turbo_b < 0) continue;
				i.set(pre + action_key(m, a), phys_format(p.bind[pl][a]));
			}
	}
	static const int rates[4] = { 0, 30, 15, 10 };
	i.set_int("turbo_a_rate", rates[p.turbo_half[0] >= 1 && p.turbo_half[0] <= 3 ? p.turbo_half[0] : 2]);
	i.set("turbo_a_mode", p.turbo_toggle[0] ? "toggle" : "hold");
	i.set_int("turbo_b_rate", rates[p.turbo_half[1] >= 1 && p.turbo_half[1] <= 3 ? p.turbo_half[1] : 2]);
	i.set("turbo_b_mode", p.turbo_toggle[1] ? "toggle" : "hold");
	return i;
}

int profile_parse(const Ini &ini, const PadModel &m, bool enter_cross, Profile &out)
{
	int rejected = 0;
	Profile def = profile_defaults(m, enter_cross);
	out = def;
	if (ini.has("own")) out.own = ini.get_int("own", 0) != 0;
	int v;
	if (ini.has("stick")) { if (stick_mode_parse(ini.get("stick"), &v)) out.stick_left = v; else rejected++; }
	if (ini.has("rstick")) { if (stick_mode_parse(ini.get("rstick"), &v)) out.stick_right = v; else rejected++; }
	if (ini.has("back_touch")) out.back_touch = ini.get_int("back_touch", 0) != 0;
	if (ini.has("deadzone")) {
		int d = ini.get_int("deadzone", 72);
		if (d >= 40 && d <= 120) { out.stick_on = d; out.stick_off = d - 20; } else rejected++;
	}
	for (int pl = 0; pl < MAX_PLAYERS; pl++) {
		std::string pre = pl == 0 ? "" : strf("p%d.", pl + 1);
		bool explicit_set[ACT_COUNT];
		memset(explicit_set, 0, sizeof explicit_set);
		/* explicit values first */
		for (int a = 0; a < ACT_COUNT; a++) {
			if (a >= m.n && a < ACT_TURBO_A) continue;
			if (pl > 0 && a >= ACT_TURBO_A) continue;
			if (a == ACT_TURBO_A && m.turbo_a < 0) continue;
			if (a == ACT_TURBO_B && m.turbo_b < 0) continue;
			std::string key = pre + action_key(m, a);
			if (!ini.has(key)) continue;
			uint32_t ph;
			Profile probe = out;
			if (!phys_parse(ini.get(key), &ph)) { rejected++; continue; }
			if (profile_assign(probe, m, pl, a, ph, 0) != ASSIGN_OK) { rejected++; continue; }
			out.bind[pl][a] = ph;
			explicit_set[a] = true;
		}
		/* a default never collides with an explicit choice: it is dropped instead */
		for (int a = 0; a < ACT_COUNT; a++) {
			if (explicit_set[a]) continue;
			if (a >= m.n && a < ACT_TURBO_A) continue;
			if (pl > 0 && a >= ACT_TURBO_A) continue;
			uint32_t d = out.bind[pl][a];
			if (!d) continue;
			bool single = (action_is_game(m, a) || action_is_turbo(a));
			for (int b = 0; b < ACT_COUNT; b++) {
				if (b == a || !explicit_set[b]) continue;
				bool bsingle = (action_is_game(m, b) || action_is_turbo(b));
				if (single == bsingle && out.bind[pl][b] == d) { out.bind[pl][a] = 0; break; }
			}
		}
		/* explicit duplicates among themselves: the earlier action keeps it */
		for (int a = 0; a < ACT_COUNT; a++) {
			if (!explicit_set[a] || !out.bind[pl][a]) continue;
			bool single = (action_is_game(m, a) || action_is_turbo(a));
			for (int b = a + 1; b < ACT_COUNT; b++) {
				if (!explicit_set[b]) continue;
				bool bsingle = (action_is_game(m, b) || action_is_turbo(b));
				if (single == bsingle && out.bind[pl][b] == out.bind[pl][a]) { out.bind[pl][b] = 0; rejected++; }
			}
		}
	}
	static const char *const rk[2] = { "turbo_a_rate", "turbo_b_rate" };
	static const char *const mk[2] = { "turbo_a_mode", "turbo_b_mode" };
	for (int t = 0; t < 2; t++) {
		if (ini.has(rk[t])) {
			int r = ini.get_int(rk[t], 15);
			if (r == 30) out.turbo_half[t] = 1; else if (r == 15) out.turbo_half[t] = 2; else if (r == 10) out.turbo_half[t] = 3; else rejected++;
		}
		if (ini.has(mk[t])) {
			std::string s = lower(trim(ini.get(mk[t])));
			if (s == "hold") out.turbo_toggle[t] = 0; else if (s == "toggle") out.turbo_toggle[t] = 1; else rejected++;
		}
	}
	return rejected;
}

bool profile_is_default(const Profile &p, const PadModel &m, bool enter_cross)
{
	Profile d = profile_defaults(m, enter_cross);
	d.own = p.own;
	return profile_to_ini(p, m).serialize() == profile_to_ini(d, m).serialize();
}

/* ---- the store ------------------------------------------------------------------ */

std::string ProfileStore::global_path() const { return path_join(path_join(root_, "controls"), console_ + ".ini"); }
std::string ProfileStore::game_path() const { return path_join(path_join(path_join(root_, "controls"), console_), game_ + ".ini"); }

int ProfileStore::load_effective(const PadModel &m, bool enter_cross, Profile &out) const
{
	int rej = 0;
	recover_old(global_path());
	recover_old(game_path());
	Ini g;
	out = profile_defaults(m, enter_cross);
	if (g.load(global_path())) rej += profile_parse(g, m, enter_cross, out);
	out.own = false;
	Ini o;
	if (!game_.empty() && o.load(game_path()) && o.get_int("own", 0) != 0) {
		Profile gp;
		rej += profile_parse(o, m, enter_cross, gp);
		gp.own = true;
		out = gp;
	}
	return rej;
}

WriteResult ProfileStore::save_global(const PadModel &m, const Profile &p) const
{
	make_dirs(path_join(root_, "controls"));
	Profile q = p;
	q.own = false;
	return ini_save(global_path(), profile_to_ini(q, m));
}

WriteResult ProfileStore::save_for(const PadModel &m, const Profile &p, bool own) const
{
	if (game_.empty()) return W_BAD_ARGS;
	if (!own) {
		file_remove(game_path());
		file_remove(game_path() + ".old");
		return W_OK;
	}
	make_dirs(path_join(path_join(root_, "controls"), console_));
	Profile q = p;
	q.own = true;
	return ini_save(game_path(), profile_to_ini(q, m));
}

/* ---- the mapper ----------------------------------------------------------------- */

Mapper::Mapper() : model_(&model_snes())
{
	memset(prev_phys_, 0, sizeof prev_phys_);
	memset(stick_state_, 0, sizeof stick_state_);
	memset(blocked_, 0, sizeof blocked_);
	memset(guard_, 0, sizeof guard_);
	memset(chord_t0_, 0, sizeof chord_t0_);
	memset(chord_frames_, 0, sizeof chord_frames_);
	memset(chord_latched_, 0, sizeof chord_latched_);
	memset(turbo_run_, 0, sizeof turbo_run_);
	memset(turbo_prev_, 0, sizeof turbo_prev_);
	memset(turbo_t0_, 0, sizeof turbo_t0_);
	cap_prev_ = 0;
	cap_started_ = false;
	cap_stick_state_ = 0;
	prof_ = profile_defaults(*model_, true);
}

void Mapper::configure(const PadModel &m, const Profile &p, const MapperConfig &c)
{
	model_ = &m;
	prof_ = p;
	cfg_ = c;
	memset(guard_, 0, sizeof guard_);
	memset(turbo_run_, 0, sizeof turbo_run_);
	memset(turbo_prev_, 0, sizeof turbo_prev_);
}

/* One axis pair into four direction bits with hysteresis. `bits` are the four PH_ bits (up, down, left, right). */
static uint32_t axis_dirs(uint8_t x, uint8_t y, uint32_t up, uint32_t down, uint32_t left, uint32_t right,
                          uint32_t state, int on, int off)
{
	int dx = (int) x - 128, dy = (int) y - 128;
	struct { int v; uint32_t neg, pos; } ax[2] = { { dx, left, right }, { dy, up, down } };
	for (int i = 0; i < 2; i++) {
		int a = ax[i].v < 0 ? -ax[i].v : ax[i].v;
		uint32_t dirbit = ax[i].v < 0 ? ax[i].neg : ax[i].pos, other = ax[i].v < 0 ? ax[i].pos : ax[i].neg;
		state &= ~other;
		if (a >= on) state |= dirbit;
		else if (a < off) state &= ~dirbit;
	}
	return state;
}

void Mapper::stick_bits(int pl, const PadSample &s, uint32_t *ls, uint32_t *rs)
{
	uint32_t st = stick_state_[pl];
	uint32_t l = axis_dirs(s.lx, s.ly, PH_LS_UP, PH_LS_DOWN, PH_LS_LEFT, PH_LS_RIGHT, st & PH_LSTICK, prof_.stick_on, prof_.stick_off);
	uint32_t r = axis_dirs(s.rx, s.ry, PH_RS_UP, PH_RS_DOWN, PH_RS_LEFT, PH_RS_RIGHT, st & PH_RSTICK, prof_.stick_on, prof_.stick_off);
	stick_state_[pl] = l | r;
	*ls = l;
	*rs = r;
}

uint32_t Mapper::fold_physical(int pl, const PadSample &s)
{
	uint32_t ls, rs;
	stick_bits(pl, s, &ls, &rs);
	uint32_t back = (pl == 0 && prof_.back_touch) ? ((uint32_t) (s.back & 15) << 20) : 0;
	return (s.buttons & PH_BUTTONS) | ls | rs | back;
}

static uint32_t stick_to_dpad(uint32_t sticks, bool left)
{
	uint32_t o = 0;
	uint32_t up = left ? PH_LS_UP : PH_RS_UP, dn = left ? PH_LS_DOWN : PH_RS_DOWN, lf = left ? PH_LS_LEFT : PH_RS_LEFT, rt = left ? PH_LS_RIGHT : PH_RS_RIGHT;
	if (sticks & up) o |= PH_UP;
	if (sticks & dn) o |= PH_DOWN;
	if (sticks & lf) o |= PH_LEFT;
	if (sticks & rt) o |= PH_RIGHT;
	return o;
}

void Mapper::menu_opened()
{
	memset(guard_, 0, sizeof guard_);
	memset(chord_frames_, 0, sizeof chord_frames_);
	for (int p = 0; p < MAX_PLAYERS; p++) {
		blocked_[p] = prev_phys_[p];
		for (int t = 0; t < 2; t++) { turbo_run_[p][t] = false; turbo_prev_[p][t] = false; }
	}
}

void Mapper::menu_closed(const PadSample pads[MAX_PLAYERS])
{
	for (int p = 0; p < MAX_PLAYERS; p++) {
		uint32_t b = pads[p].connected ? (pads[p].buttons & PH_BUTTONS) : 0;
		bool chord = (b & PH_MENU_CHORD) == PH_MENU_CHORD ||
		             (cfg_.legacy_chord && model_->legacy_chord && (b & (PH_L | PH_R | PH_START)) == (PH_L | PH_R | PH_START));
		chord_latched_[p] = chord;
		blocked_[p] |= b;      /* what is still down stays out of the game until it is let go */
		memset(chord_frames_[p], 0, sizeof chord_frames_[p]);
	}
	memset(guard_, 0, sizeof guard_);
}

/* the buttons that make a chord with `btn` */
static uint32_t chord_partners(const Profile &prof, const PadModel &m, const MapperConfig &c, uint32_t btn)
{
	uint32_t r = 0;
	for (int a = ACT_FF; a < ACT_COUNT; a++) {
		uint32_t b = prof.bind[0][a];
		if ((b & btn) && (b & ~btn)) r |= b & ~btn;
	}
	r |= PH_MENU_CHORD & ~btn;
	if (c.legacy_chord && m.legacy_chord && btn == PH_START) r |= PH_L | PH_R;
	return r;
}

/* is there a shortcut whose chord strictly contains `chord` and is fully held? (the longer chord wins) */
static bool yields(const Profile &prof, int a, uint32_t raw)
{
	uint32_t mine = prof.bind[0][a];
	if (!mine) return false;
	for (int b = ACT_FF; b < ACT_COUNT; b++) {
		if (b == a) continue;
		uint32_t other = prof.bind[0][b];
		if (other && other != mine && (other & mine) == mine && (raw & other) == other) return true;
	}
	return false;
}

void Mapper::update(const PadSample pads[MAX_PLAYERS], uint64_t now_ms, uint32_t frame, InputResult *out)
{
	*out = InputResult();
	const PadModel &m = *model_;
	for (int p = 0; p < MAX_PLAYERS; p++) {
		const PadSample &s = pads[p];
		if (!s.connected) {
			prev_phys_[p] = 0; stick_state_[p] = 0; blocked_[p] = 0;
			memset(guard_[p], 0, sizeof guard_[p]);
			chord_frames_[p][0] = chord_frames_[p][1] = 0;
			chord_latched_[p] = false;
			for (int t = 0; t < 2; t++) { turbo_run_[p][t] = false; turbo_prev_[p][t] = false; }
			continue;
		}
		uint32_t ls, rs;
		stick_bits(p, s, &ls, &rs);
		uint32_t back = (p == 0 && prof_.back_touch) ? ((uint32_t) (s.back & 15) << 20) : 0;
		uint32_t raw = (s.buttons & PH_BUTTONS) | ls | rs | back;
		out->phys[p] = raw;
		uint32_t pad_new = raw & ~prev_phys_[p];

		/* what the game's mapping sees: sticks folded in as the mode says */
		uint32_t g = s.buttons & PH_BUTTONS;
		if (prof_.stick_left == STICK_MOVE) g |= stick_to_dpad(ls, true);
		else if (prof_.stick_left == STICK_CUSTOM) g |= ls;
		if (prof_.stick_right == STICK_MOVE) g |= stick_to_dpad(rs, false);
		else if (prof_.stick_right == STICK_CUSTOM) g |= rs;
		g |= back;

		/* the menu chord: held long enough, on any pad */
		{
			bool both = (raw & PH_MENU_CHORD) == PH_MENU_CHORD;
			bool legacy = cfg_.legacy_chord && m.legacy_chord && (raw & (PH_L | PH_R | PH_START)) == (PH_L | PH_R | PH_START);
			bool held[2] = { both, legacy };
			bool any_held = false;
			for (int k = 0; k < 2; k++) {
				if (!held[k]) { chord_frames_[p][k] = 0; continue; }
				any_held = true;
				if (chord_frames_[p][k] == 0) chord_t0_[p][k] = now_ms;
				chord_frames_[p][k]++;
				if (!chord_latched_[p] && !out->menu &&
				    (chord_frames_[p][k] >= cfg_.menu_hold_frames || now_ms - chord_t0_[p][k] >= (uint64_t) cfg_.menu_hold_ms)) {
					out->menu = true;
					out->menu_player = p;
					chord_latched_[p] = true;
				}
			}
			if (!any_held) chord_latched_[p] = false;
		}

		/* inputs kept from the game until let go (a chord that fired, a menu that closed) */
		blocked_[p] &= raw | g;

		/* the chord guard: Start and Select wait a moment in case they are the first button of a chord */
		uint32_t gv = g;
		if (cfg_.chord_guard) {
			static const uint32_t btns[2] = { PH_START, PH_SELECT };
			for (int i = 0; i < 2; i++) {
				uint32_t btn = btns[i];
				uint32_t partners = chord_partners(prof_, m, cfg_, btn);
				bool down = (g & btn) != 0;
				bool deliver = false;
				Guard &gd = guard_[p][i];
				enum { IDLE = 0, PENDING, PASS, SPOILED, TAPPING };
				switch (gd.state) {
				case TAPPING:
					if (down) { gd.state = PENDING; gd.t = 0; break; }
					deliver = true;
					if (++gd.t >= cfg_.guard_tap_frames) gd.state = IDLE;
					break;
				case IDLE:
					if (!down) break;
					gd.state = PENDING;
					gd.t = 0;
					/* fall through */
				case PENDING:
					if (raw & partners) gd.state = SPOILED;
					else if (!down) { gd.state = TAPPING; gd.t = 0; deliver = true; }
					else if (++gd.t >= cfg_.guard_wait_frames) { gd.state = PASS; deliver = true; }
					break;
				case PASS:
					if (!down) gd.state = IDLE; else deliver = true;
					break;
				case SPOILED:
					if (!(raw & btn)) gd.state = IDLE;
					break;
				}
				if (deliver) gv |= btn; else gv &= ~btn;
			}
		}

		/* shortcuts (player 1) */
		if (p == 0) {
			for (int a = ACT_FF; a < ACT_COUNT; a++) {
				uint32_t c = prof_.bind[0][a];
				if (!c) continue;
				bool held = (raw & c) == c;
				uint32_t trig = c & ~PH_MENU_CHORD;
				if (!trig) trig = c;
				bool edge_mod = (pad_new & trig) && held;     /* modifier chord: SELECT/START must already be down */
				bool edge_any = (pad_new & c) && held;
				switch (a) {
				case ACT_FF:
					if (held) out->fired |= FIRE_FF_HELD;
					if (edge_any) out->fired |= FIRE_FF_PRESS;
					break;
				case ACT_REWIND:
					if (held) out->fired |= FIRE_REWIND_HELD;
					if (edge_any) out->fired |= FIRE_REWIND_PRESS;
					break;
				case ACT_QSAVE: case ACT_QLOAD: case ACT_SHOT:
					if (edge_mod && !yields(prof_, a, raw)) {
						out->fired |= a == ACT_QSAVE ? FIRE_QSAVE : a == ACT_QLOAD ? FIRE_QLOAD : FIRE_SHOT;
						if (popcount(c) > 1) blocked_[0] |= c;       /* the chord's buttons stay out of the game */
					}
					break;
				}
			}
			if (out->fired & (FIRE_QSAVE | FIRE_QLOAD | FIRE_SHOT)) {
				/* the guard must not later deliver a Start/Select that took part */
				gv &= ~blocked_[0];
			}
		}

		/* the game's buttons */
		uint32_t view = gv & ~blocked_[p];
		uint32_t mask = 0;
		for (int a = 0; a < m.n; a++) {
			uint32_t b = prof_.bind[p][a];
			if (b && (view & b) == b) mask |= m.btn[a].out_bit;
		}

		/* turbo */
		if (p == 0) {
			for (int t = 0; t < 2; t++) {
				int act = t == 0 ? ACT_TURBO_A : ACT_TURBO_B;
				int tgt = t == 0 ? m.turbo_a : m.turbo_b;
				uint32_t b = prof_.bind[0][act];
				if (tgt < 0 || !b) { turbo_run_[0][t] = false; turbo_prev_[0][t] = false; continue; }
				bool down = (view & b) == b;
				if (prof_.turbo_toggle[t]) {
					if (down && !turbo_prev_[0][t]) { turbo_run_[0][t] = !turbo_run_[0][t]; turbo_t0_[0][t] = frame; }
				} else {
					if (down && !turbo_prev_[0][t]) { turbo_run_[0][t] = true; turbo_t0_[0][t] = frame; }
					if (!down) turbo_run_[0][t] = false;
				}
				turbo_prev_[0][t] = down;
				int half = prof_.turbo_half[t] < 1 ? 1 : prof_.turbo_half[t] > 3 ? 3 : prof_.turbo_half[t];
				if (turbo_run_[0][t] && (((frame - turbo_t0_[0][t]) / (uint32_t) half) % 2) == 0) mask |= m.btn[tgt].out_bit;
			}
		}
		out->pad[p] = mask;
		prev_phys_[p] = raw;
	}
}

void Mapper::capture_reset()
{
	cap_started_ = false;
	cap_prev_ = 0;
	cap_stick_state_ = 0;
}

uint32_t Mapper::capture_next(const PadSample &s)
{
	uint32_t st = cap_stick_state_;
	uint32_t l = axis_dirs(s.lx, s.ly, PH_LS_UP, PH_LS_DOWN, PH_LS_LEFT, PH_LS_RIGHT, st & PH_LSTICK, prof_.stick_on, prof_.stick_off);
	uint32_t r = axis_dirs(s.rx, s.ry, PH_RS_UP, PH_RS_DOWN, PH_RS_LEFT, PH_RS_RIGHT, st & PH_RSTICK, prof_.stick_on, prof_.stick_off);
	cap_stick_state_ = l | r;
	uint32_t raw = (s.buttons & PH_BUTTONS) | l | r | ((uint32_t) (s.back & 15) << 20);
	if (!cap_started_) {         /* what is already down when the page opens is not a choice */
		cap_started_ = true;
		cap_prev_ = raw;
		return 0;
	}
	uint32_t fresh = raw & ~cap_prev_;
	cap_prev_ = raw;
	if (!fresh) return 0;
	for (int i = 0; i < PH_COUNT; i++)
		if (fresh & (1u << i)) return 1u << i;
	return 0;
}

}  // namespace pk
