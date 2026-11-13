/* See pk_look.h. Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE). */
#include "pk_look.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

namespace pk {

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

LookGovernor::LookGovernor()
{
	memset(rung_, 0, sizeof rung_);
	memset(saving_, 0, sizeof saving_);
	memset(user_row_ids_, 0, sizeof user_row_ids_);
	memset(wload_, 0, sizeof wload_);
	memset(wbad_, 0, sizeof wbad_);
	memset(sec_at_, 0, sizeof sec_at_);
	memset(peak_, 0, sizeof peak_);
	memset(peak_at_, 0, sizeof peak_at_);
	cool_cur_ = cfg_.cool_s;
}

float LookGovernor::load_of(const LookSecond &s)
{
	float stage = s.draw + s.filter;
	return s.parallel ? (s.emu > stage ? s.emu : stage) : s.emu + stage;
}

bool LookGovernor::set_ladder(const LookRung *r, int n)
{
	if (!r || n < 0 || n > LOOK_MAX_RUNGS) return false;
	for (int i = 1; i < n; i++)
		if (r[i].visibility < r[i - 1].visibility) return false;   /* least visible first, always */
	for (int i = 0; i < n; i++)
		if (r[i].est_saving <= 0.f || r[i].est_saving > 1.f || !r[i].name) return false;
	n_ = n;
	for (int i = 0; i < n; i++) {
		names_[i] = r[i].name;
		rung_[i] = r[i];
		rung_[i].name = names_[i].c_str();
		saving_[i] = r[i].est_saving;
	}
	applied_ = hold_mask_ = 0;
	return true;
}

uint32_t LookGovernor::ladder_hash() const
{
	uint32_t h = 2166136261u;
	for (int i = 0; i < n_; i++) {
		for (const char *p = names_[i].c_str(); *p; p++) { h ^= (uint8_t) *p; h *= 16777619u; }
		for (int k = 0; k < 4; k++) { h ^= (uint32_t) (rung_[i].row >> (8 * k)) & 0xFF; h *= 16777619u; }
	}
	return h ^ (uint32_t) n_;
}

bool LookGovernor::user_row(int row) const
{
	for (int i = 0; i < n_user_; i++) if (user_row_ids_[i] == row) return true;
	return false;
}

void LookGovernor::set_user_row(int row, bool user)
{
	if (user) {
		if (user_row(row) || n_user_ >= 16) return;
		user_row_ids_[n_user_++] = row;
		uint32_t bits = 0;
		for (int i = 0; i < n_; i++) if (rung_[i].row == row) bits |= 1u << i;
		if (applied_ & bits) {
			applied_ &= ~bits;
			gen_++;
			event("hand", "a row was set by hand; its steps are released", last_load_);
		}
		hold_mask_ &= ~bits;
		if (pending_ == -1 && next_down_rung() < 0) pending_ = 0;
		if (pending_ == +1 && next_up_rung() < 0) pending_ = 0;
	} else {
		for (int i = 0; i < n_user_; i++)
			if (user_row_ids_[i] == row) { user_row_ids_[i] = user_row_ids_[--n_user_]; break; }
	}
}

int LookGovernor::level() const
{
	int c = 0;
	for (uint32_t m = applied_; m; m &= m - 1) c++;
	return c;
}

int LookGovernor::steps_on_row(int row) const
{
	int c = 0;
	for (int i = 0; i < n_; i++) if (rung_[i].row == row && ((applied_ >> i) & 1u)) c++;
	return c;
}

int LookGovernor::next_down_rung() const
{
	for (int i = 0; i < n_; i++)
		if (!((applied_ >> i) & 1u) && !user_row(rung_[i].row)) return i;
	return -1;
}

int LookGovernor::next_up_rung() const
{
	for (int i = n_ - 1; i >= 0; i--)
		if (((applied_ >> i) & 1u) && !user_row(rung_[i].row)) return i;
	return -1;
}

void LookGovernor::event(const char *what, const char *why, float load)
{
	char b[160];
	snprintf(b, sizeof b, "t=%ds %s lvl=%d load=%.2f %s", now_, what, level(), load, why);
	if (events_.size() >= 32) events_.erase(events_.begin());
	events_.push_back(b);
}

void LookGovernor::clear_window()
{
	wn_ = whead_ = 0;
	fit_run_ = bad_run_ = good_run_ = 0;
}

void LookGovernor::reset_window()
{
	clear_window();
	if (pending_ == +1) pending_ = 0;
}

void LookGovernor::start()
{
	if (started_) return;
	started_ = true;
	start_at_ = now_;
	cool_cur_ = cfg_.cool_s > mem_cool_ ? cfg_.cool_s : mem_cool_;
	if (cool_cur_ > cfg_.cool_max_s) cool_cur_ = cfg_.cool_max_s;
	since_change_ = 0;
	if (!mem_mismatch_ && mem_mask_) {
		uint32_t m = 0;
		for (int i = 0; i < n_; i++)
			if (((mem_mask_ >> i) & 1u) && !user_row(rung_[i].row)) m |= 1u << i;
		if (m) {
			applied_ = m;
			hold_mask_ = m;
			peak_[level()] = mem_peak_;
			peak_at_[level()] = now_;
			gen_++;
			event("start", "remembered look applied", 0.f);
		}
	}
}

/* ---- the decision ------------------------------------------------------------------------------ */

bool LookGovernor::second(const LookSecond &s)
{
	if (!started_) start();
	if (s.refreshes < LOOK_MIN_REFRESHES) return false;     /* a menu, a load, a pause: no vote */
	now_++;
	since_change_++;
	sec_at_[level()]++;
	const float load = load_of(s);
	const float miss = (float) s.missed / (float) s.refreshes;
	const bool bad = load >= cfg_.bad_load || miss >= cfg_.bad_miss || (s.pace_half && load >= cfg_.pace_recover);
	last_load_ = load; last_emu_ = s.emu; last_draw_ = s.draw; last_filter_ = s.filter; last_miss_ = miss;
	last_parallel_ = s.parallel; last_half_ = s.pace_half;
	wload_[whead_] = load;
	wbad_[whead_] = bad ? 1 : 0;
	whead_ = (whead_ + 1) % LOOK_WINDOW;
	if (wn_ < LOOK_WINDOW) wn_++;
	if (bad) { bad_run_++; good_run_ = 0; } else { good_run_++; bad_run_ = 0; }
	learn_tick(load);
	if (wn_ >= 4) {                                          /* the worst 4 s mean at this level, remembered for a while */
		float m4 = 0;
		for (int i = 0; i < 4; i++) m4 += wload_[(whead_ - 1 - i + LOOK_WINDOW) % LOOK_WINDOW];
		m4 /= 4.f;
		const int l = level();
		if (now_ - peak_at_[l] > cfg_.peak_hold_s) { peak_[l] = m4; peak_at_[l] = now_; }          /* the old worst scene is forgotten */
		else if (m4 >= peak_[l] * 0.95f) { if (m4 > peak_[l]) peak_[l] = m4; peak_at_[l] = now_; }   /* the same scene again: seen again */
	}

	bool changed = false;
	/* a climb that held long enough is proven: the cool-down starts over and the memory follows */
	if (probe_since_ >= 0 && now_ - probe_since_ >= cfg_.settle_s) {
		probe_since_ = -1;
		cool_cur_ = cfg_.cool_s;
		if (hold_mask_ != applied_) { hold_mask_ = applied_; mem_dirty_ = true; }
	}
	/* a remembered look that was not needed for a long time at the full look is forgotten */
	if (mem_loaded_ && downs_ == 0 && hold_mask_ && applied_ == 0 && sec_at_[0] >= cfg_.forget_s) {
		hold_mask_ = 0;
		mem_dirty_ = true;
		event("forget", "the remembered look was not needed", load);
	}

	int badw = 0;
	float wmax = 0;
	for (int i = 0; i < wn_; i++) { badw += wbad_[i]; if (wload_[i] > wmax) wmax = wload_[i]; }
	const bool in_grace = now_ - start_at_ <= cfg_.start_grace_s;
	const bool grace_down = in_grace && bad_run_ >= cfg_.down_start_s;
	const bool want_down = wn_ >= 1 && badw >= cfg_.down_s;
	const int dr = next_down_rung();

	if ((want_down || grace_down) && dr >= 0) {
		if (pending_ == +1) pending_ = 0;
		if (grace_down) {
			apply_down("overload at the start of the game", false);
			changed = true;
		} else if (cfg_.crossfade) {
			apply_down("sustained misses (cross-fade)", false);
			changed = true;
		} else if (pending_ != -1) {
			pending_ = -1;
			pending_since_ = now_;
		}
	} else if (pending_ == -1 && good_run_ >= 5) {
		pending_ = 0;                                       /* it cleared by itself */
	}
	if (pending_ == -1 && now_ - pending_since_ >= cfg_.force_s) {
		apply_down("sustained misses, no safe point came", true);
		changed = true;
	}

	/* a climb: only when the learned cost of the rung fits with margin, never while pacing thins frames */
	const int ur = next_up_rung();
	if (!changed && pending_ != -1 && ur >= 0) {
		const int l = level();
		const float pk = now_ - peak_at_[l] <= cfg_.peak_hold_s ? peak_[l] : 0.f;     /* the worst scene lately seen at this look */
		const float ref = wmax > pk ? wmax : pk;
		bool fit = !bad && !s.pace_half && wn_ >= LOOK_WINDOW && since_change_ >= cool_cur_ &&
		           ref + saving_[ur] * cfg_.up_margin <= cfg_.up_load;
		fit_run_ = fit ? fit_run_ + 1 : 0;
		if (fit_run_ >= cfg_.up_hold_s) {
			if (cfg_.crossfade) { apply_up("headroom for the learned cost"); changed = true; }
			else if (pending_ != +1) { pending_ = +1; pending_since_ = now_; }
		} else if (pending_ == +1 && !fit) pending_ = 0;
	} else if (ur < 0 || pending_ == -1) {
		fit_run_ = 0;
	}
	return changed;
}

bool LookGovernor::safe_point(LookSafe kind)
{
	if (!started_) start();
	if (kind == LOOK_SAFE_MENU) clear_window();
	if (pending_ == -1) { apply_down("sustained misses", false); return true; }
	if (pending_ == +1) {
		/* re-check at the moment of the change: the headroom must still be there */
		if (next_up_rung() >= 0 && fit_run_ >= cfg_.up_hold_s) { apply_up("headroom for the learned cost"); return true; }
		pending_ = 0;
	}
	return false;
}

void LookGovernor::apply_down(const char *why, bool forced)
{
	int r = next_down_rung();
	pending_ = 0;
	if (r < 0) return;
	float pre = 0; int k = 0;
	for (int i = 0; i < wn_ && i < 4; i++) { pre += wload_[(whead_ - 1 - i + LOOK_WINDOW) % LOOK_WINDOW]; k++; }
	pre = k ? pre / (float) k : last_load_;
	if (last_change_up_ && now_ - last_up_at_ <= cfg_.bounce_s) {
		failed_climbs_++;
		cool_cur_ = cool_cur_ * 2 > cfg_.cool_max_s ? cfg_.cool_max_s : cool_cur_ * 2;
	}
	/* as many rungs as the learned costs say are needed, least visible first: steady in one go, not shed in play */
	uint32_t took = 0;
	float pred = pre;
	std::string names;
	while (r >= 0) {
		applied_ |= 1u << r;
		took |= 1u << r;
		pred -= saving_[r];
		if (!names.empty()) names += " + ";
		names += rung_[r].name;
		r = next_down_rung();
		if (pred <= cfg_.down_target) break;
	}
	int nt = 0;
	for (uint32_t m = took; m; m &= m - 1) nt++;
	peak_[level()] = peak_[level()] > pred ? peak_[level()] : (pred > 0 ? pred : 0);   /* the same scene, one look lower */
	peak_at_[level()] = now_;
	hold_mask_ = applied_;
	mem_needed_ = true;
	mem_dirty_ = true;
	gen_++;
	downs_++;
	rungs_down_ += nt;
	if (forced) forced_++;
	last_change_up_ = false;
	probe_since_ = -1;
	since_change_ = 0;
	learn_pre_ = pre; learn_mask_ = took; learn_dir_ = -1; learn_n_ = 0; learn_acc_ = 0; learn_wait_ = 1;
	char b[200];
	snprintf(b, sizeof b, "down \"%s\" (%s%s)", names.c_str(), why, forced ? ", forced" : "");
	event("step", b, pre);
	clear_window();
}

void LookGovernor::apply_up(const char *why)
{
	const int r = next_up_rung();
	pending_ = 0;
	if (r < 0) return;
	float pre = 0; int k = 0;
	for (int i = 0; i < wn_ && i < 4; i++) { pre += wload_[(whead_ - 1 - i + LOOK_WINDOW) % LOOK_WINDOW]; k++; }
	learn_pre_ = k ? pre / (float) k : last_load_;
	applied_ &= ~(1u << r);
	gen_++;
	ups_++;
	last_change_up_ = true;
	last_up_at_ = now_;
	probe_since_ = now_;
	since_change_ = 0;
	learn_mask_ = 1u << r; learn_dir_ = +1; learn_n_ = 0; learn_acc_ = 0; learn_wait_ = 1;
	char b[120];
	snprintf(b, sizeof b, "up \"%s\" (%s)", rung_[r].name, why);
	event("step", b, learn_pre_);
	clear_window();
}

/* the real cost of the rungs a change moved, from the load before and after it */
void LookGovernor::learn_tick(float load)
{
	if (!learn_mask_) return;
	if (learn_wait_ > 0) { learn_wait_--; return; }          /* the first second after a change is a transient */
	learn_acc_ += load;
	if (++learn_n_ < 4) return;
	const float post = learn_acc_ / (float) learn_n_;
	const float obs = learn_dir_ < 0 ? learn_pre_ - post : post - learn_pre_;
	const uint32_t mask = learn_mask_;
	learn_mask_ = 0;
	{   /* what the scene really costs at the look now in force replaces the estimate made at the step */
		const int l = level();
		if (learn_dir_ < 0 || post > peak_[l]) peak_[l] = post;
		peak_at_[l] = now_;
	}
	if (obs < 0.01f) return;                                 /* nothing measurable: keep the guesses */
	float sum = 0;
	for (int i = 0; i < n_; i++) if ((mask >> i) & 1u) sum += saving_[i];
	if (sum <= 0) return;
	for (int i = 0; i < n_; i++) {
		if (!((mask >> i) & 1u)) continue;
		const float g = rung_[i].est_saving, old = saving_[i];
		const float share = obs * old / sum;                 /* the observed gain, split by the current weights */
		saving_[i] = 0.5f * old + 0.5f * clampf(share, 0.25f * g, 4.0f * g);
		if (hold_mask_ && fabsf(saving_[i] - old) > 0.05f * old) mem_dirty_ = true;
	}
}

/* ---- the log ------------------------------------------------------------------------------------ */

bool LookGovernor::census_line(char *buf, size_t n) const
{
	if (!buf || !n || now_ == 0) return false;
	int badw = 0;
	float wmax = 0;
	for (int i = 0; i < wn_; i++) { badw += wbad_[i]; if (wload_[i] > wmax) wmax = wload_[i]; }
	const int ur = next_up_rung();
	char up[24];
	if (ur >= 0) snprintf(up, sizeof up, "%.2f", wmax + saving_[ur] * cfg_.up_margin); else snprintf(up, sizeof up, "-");
	const int cool_left = cool_cur_ - since_change_ > 0 ? cool_cur_ - since_change_ : 0;
	snprintf(buf, n, "look: lvl=%d/%d mask=0x%x load=%.2f emu=%.2f draw=%.2f filt=%.2f %s miss=%d%% pace=%s bad=%d/%d pend=%s cool=%ds/%ds up_need=%s t=%d",
	         level(), n_, (unsigned) applied_, last_load_, last_emu_, last_draw_, last_filter_, last_parallel_ ? "par" : "seq",
	         (int) (last_miss_ * 100.f + 0.5f), last_half_ ? "half" : "full", badw, wn_,
	         pending_ < 0 ? "down" : pending_ > 0 ? "up" : "-", cool_left, cool_cur_, up, now_);
	return true;
}

std::string LookGovernor::summary() const
{
	std::string s;
	char b[200];
	snprintf(b, sizeof b, "look summary: %ds, final lvl=%d/%d mask=0x%x, down %d up %d forced %d failed-climb %d, cool %ds; time at level:",
	         now_, level(), n_, (unsigned) applied_, downs_, ups_, forced_, failed_climbs_, cool_cur_);
	s = b;
	for (int l = 0; l <= n_; l++) {
		if (!sec_at_[l]) continue;
		snprintf(b, sizeof b, " L%d=%ds", l, sec_at_[l]);
		s += b;
	}
	s += "; memory ";
	LookMemAction a = memory_action();
	s += a == LOOK_MEM_WRITE ? "write" : a == LOOK_MEM_DELETE ? "delete" : "none";
	size_t from = events_.size() > 6 ? events_.size() - 6 : 0;
	for (size_t i = from; i < events_.size(); i++) { s += i == from ? "; " : " | "; s += events_[i]; }
	return s;
}

/* ---- memory ----------------------------------------------------------------------------------- */

LookMemAction LookGovernor::memory_action() const
{
	if (!mem_dirty_) return LOOK_MEM_NONE;
	if (hold_mask_) return LOOK_MEM_WRITE;
	return mem_loaded_ ? LOOK_MEM_DELETE : LOOK_MEM_NONE;
}

std::string LookGovernor::serialize(int mhz) const
{
	std::string s;
	char b[96];
	s = "pk-look 1\n";
	int hl = 0;
	for (uint32_t m = hold_mask_; m; m &= m - 1) hl++;
	snprintf(b, sizeof b, "ladder=%08x\nmhz=%d\nmask=%u\ncool=%d\npeak=%d\nsav=", (unsigned) ladder_hash(), mhz, (unsigned) hold_mask_, cool_cur_, (int) ((peak_[hl] > 0.f ? peak_[hl] : hold_mask_ == mem_mask_ ? mem_peak_ : 0.f) * 1000.f + 0.5f));
	s += b;
	for (int i = 0; i < n_; i++) {
		snprintf(b, sizeof b, "%s%d", i ? "," : "", (int) (saving_[i] * 1000.f + 0.5f));
		s += b;
	}
	s += "\n";
	return s;
}

bool LookGovernor::load_memory(const char *text, size_t n, int mhz)
{
	if (!text || n < 10 || n > 1024 || n_ == 0) return false;
	std::string t(text, n);
	if (t.compare(0, 9, "pk-look 1") != 0) return false;
	unsigned lad = 0; int fmhz = -1, cool = -1, peak = 0; long mask = -1;
	int sav[LOOK_MAX_RUNGS]; int nsav = 0;
	bool got_lad = false;
	size_t pos = t.find('\n');
	while (pos != std::string::npos && pos + 1 < t.size()) {
		size_t e = t.find('\n', pos + 1);
		std::string line = t.substr(pos + 1, e == std::string::npos ? std::string::npos : e - pos - 1);
		pos = e;
		size_t eq = line.find('=');
		if (eq == std::string::npos) continue;
		std::string k = line.substr(0, eq), v = line.substr(eq + 1);
		if (k == "ladder") { char *end; unsigned long x = strtoul(v.c_str(), &end, 16); if (*end || v.empty()) return false; lad = (unsigned) x; got_lad = true; }
		else if (k == "mhz") { char *end; fmhz = (int) strtol(v.c_str(), &end, 10); if (*end || v.empty()) return false; }
		else if (k == "mask") { char *end; mask = strtol(v.c_str(), &end, 10); if (*end || v.empty()) return false; }
		else if (k == "peak") { char *end; peak = (int) strtol(v.c_str(), &end, 10); if (*end || v.empty() || peak < 0 || peak > 4000) return false; }
		else if (k == "cool") { char *end; cool = (int) strtol(v.c_str(), &end, 10); if (*end || v.empty()) return false; }
		else if (k == "sav") {
			const char *p = v.c_str();
			while (*p) {
				char *end; long x = strtol(p, &end, 10);
				if (end == p || x < 0 || x > 2000 || nsav >= LOOK_MAX_RUNGS) return false;
				sav[nsav++] = (int) x;
				p = end;
				if (*p == ',') p++; else if (*p) return false;
			}
		}
	}
	if (!got_lad || lad != ladder_hash()) return false;
	if (nsav != n_ || mask < 0 || mask >= (1L << n_) || cool < 0 || cool > cfg_.cool_max_s || fmhz < 0) return false;
	for (int i = 0; i < n_; i++) {
		float g = rung_[i].est_saving;
		saving_[i] = clampf((float) sav[i] / 1000.f, 0.25f * g, 4.0f * g);
	}
	mem_loaded_ = true;
	mem_mask_ = (uint32_t) mask;
	mem_cool_ = cool;
	mem_peak_ = (float) peak / 1000.f;
	mem_mismatch_ = mhz > 0 && fmhz > 0 && fmhz != mhz;
	hold_mask_ = (uint32_t) mask;                      /* a mismatched memory is kept as it is unless this session learns better */
	return true;
}

std::string look_auto_label(const std::string &current, bool eased)
{
	return "Auto (" + current + (eased ? ", eased" : "") + ")";
}

}  // namespace pk
