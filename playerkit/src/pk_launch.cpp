/* See pk_launch.h. Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE). */
#include "pk_launch.h"
#include <stdlib.h>
#include <string.h>

namespace pk {

std::string launch_path(const std::string &root, const std::string &console)
{
	return path_join(path_join(root, "launch"), console + ".ini");
}

std::string session_path(const std::string &root)
{
	return path_join(path_join(root, "launch"), "session.txt");
}

static const char *const kShapeNames[SHAPE_COUNT] = { "original", "fit", "zoom", "full", "pixel" };

const char *shape_name(PictureShape s) { return ((unsigned) s < (unsigned) SHAPE_COUNT) ? kShapeNames[s] : kShapeNames[0]; }

bool shape_parse(const std::string &s, PictureShape *out)
{
	std::string v = lower(trim(s));
	for (int i = 0; i < SHAPE_COUNT; i++)
		if (v == kShapeNames[i]) { *out = (PictureShape) i; return true; }
	if (v == "full screen" || v == "stretch") { *out = SHAPE_FULL; return true; }
	if (v == "pixel perfect") { *out = SHAPE_PIXEL; return true; }
	return false;
}

/* One line of text without control characters, at most max bytes. */
static std::string clean_line(const std::string &s, size_t max = 240)
{
	std::string o;
	for (size_t i = 0; i < s.size() && o.size() < max; i++) {
		unsigned char c = (unsigned char) s[i];
		o += (c < 32 || c == 127) ? ' ' : (char) c;
	}
	return trim(o);
}

static bool parse_bool(const std::string &v, bool def)
{
	std::string s = lower(trim(v));
	if (s == "1" || s == "true" || s == "yes" || s == "on") return true;
	if (s == "0" || s == "false" || s == "no" || s == "off") return false;
	return def;
}

int LaunchRequest::override_int(const std::string &key, int def) const
{
	std::map<std::string, std::string>::const_iterator it = overrides.find(key);
	if (it == overrides.end() || it->second.empty()) return def;
	char *end = 0;
	long v = strtol(it->second.c_str(), &end, 10);
	return (end && !*end) ? (int) v : def;
}

LaunchRequest launch_parse(const std::string &text)
{
	LaunchRequest r;
	Ini ini;
	ini.parse(text);
	if (ini.has("version")) {
		int v = ini.get_int("version", LAUNCH_VERSION);
		r.version = v < 1 ? LAUNCH_VERSION : v;
		if (v > LAUNCH_VERSION) r.newer_version = true;
	}
	static const char *const known[] = { "version", "session_id", "console", "game", "game_name", "save", "root", "shape", "quality",
		"cpu_mhz", "enter_button", "language", "start", "slot", "patch", "retroarch_save", "patch_key", "wide_supported" };
	const std::vector<std::pair<std::string, std::string> > &items = ini.items();
	for (size_t i = 0; i < items.size(); i++) {
		const std::string &k = items[i].first, &v = items[i].second;
		if (starts_with(k, "ovr.")) { r.overrides[k.substr(4)] = v; continue; }
		bool is_known = false;
		for (size_t j = 0; j < sizeof known / sizeof known[0]; j++) if (k == known[j]) is_known = true;
		if (!is_known) r.unknown[k] = v;
	}
	r.session_id = clean_line(ini.get("session_id"), 64);
	r.console = lower(clean_line(ini.get("console"), 16));
	r.game_path = ini.get("game");
	r.game_name = clean_line(ini.get("game_name"), 120);
	r.save_path = ini.get("save");
	if (ini.has("root") && !ini.get("root").empty()) r.root = ini.get("root");
	r.patch_path = ini.get("patch");
	r.retroarch_save = ini.get("retroarch_save");
	r.patch_key = ini.get("patch_key");
	r.wide_supported = parse_bool(ini.get("wide_supported"), false);
	if (ini.has("shape")) {
		PictureShape s;
		if (shape_parse(ini.get("shape"), &s)) r.shape = s;
		else r.warnings.push_back("shape: unknown value '" + clean_line(ini.get("shape"), 40) + "', using original");
	}
	if (ini.has("quality")) {
		std::string q = lower(trim(ini.get("quality")));
		if (q == "sharp") r.quality = QUALITY_SHARP;
		else if (q == "smooth") r.quality = QUALITY_SMOOTH;
		else r.warnings.push_back("quality: unknown value '" + clean_line(q, 40) + "', using sharp");
	}
	if (ini.has("cpu_mhz")) {
		int v = ini.get_int("cpu_mhz", 0);
		if (v == 0 || v == 444 || v == 500) r.cpu_mhz = v;
		else r.warnings.push_back(strf("cpu_mhz: %d is not 444 or 500, using the default", v));
	}
	if (ini.has("enter_button")) {
		std::string e = lower(trim(ini.get("enter_button")));
		if (e == "cross") r.enter_is_cross = true;
		else if (e == "circle") r.enter_is_cross = false;
		else r.warnings.push_back("enter_button: unknown value, using cross");
	}
	if (ini.has("language")) {
		std::string l = lower(clean_line(ini.get("language"), 16));
		if (!l.empty()) r.language = l;
	}
	if (ini.has("start")) {
		std::string s = lower(trim(ini.get("start")));
		if (s == "new") r.start = START_NEW;
		else if (s == "continue") r.start = START_CONTINUE;
		else if (s == "slot") r.start = START_SLOT;
		else r.warnings.push_back("start: unknown value, starting new");
	}
	if (ini.has("slot")) {
		int s = ini.get_int("slot", 0);
		if (s >= 1 && s <= 10) r.slot = s;
		else r.warnings.push_back(strf("slot: %d is not 1..10", s));
	}
	if (r.start == START_SLOT && (r.slot < 1 || r.slot > 10)) r.start = START_NEW;
	return r;
}

std::string launch_serialize(const LaunchRequest &r)
{
	Ini i;
	i.set_int("version", LAUNCH_VERSION);
	i.set("session_id", clean_line(r.session_id, 64));
	i.set("console", r.console);
	i.set("game", r.game_path);
	if (!r.game_name.empty()) i.set("game_name", clean_line(r.game_name, 120));
	if (!r.save_path.empty()) i.set("save", r.save_path);
	i.set("root", r.root);
	i.set("shape", shape_name(r.shape));
	i.set("quality", r.quality == QUALITY_SMOOTH ? "smooth" : "sharp");
	i.set_int("cpu_mhz", r.cpu_mhz);
	i.set("enter_button", r.enter_is_cross ? "cross" : "circle");
	i.set("language", r.language);
	i.set("start", r.start == START_CONTINUE ? "continue" : r.start == START_SLOT ? "slot" : "new");
	if (r.start == START_SLOT) i.set_int("slot", r.slot);
	if (!r.patch_path.empty()) i.set("patch", r.patch_path);
	if (!r.retroarch_save.empty()) i.set("retroarch_save", r.retroarch_save);
	if (!r.patch_key.empty()) i.set("patch_key", r.patch_key);
	i.set("wide_supported", r.wide_supported ? "1" : "0");
	for (std::map<std::string, std::string>::const_iterator it = r.overrides.begin(); it != r.overrides.end(); ++it)
		i.set("ovr." + it->first, it->second);
	for (std::map<std::string, std::string>::const_iterator it = r.unknown.begin(); it != r.unknown.end(); ++it)
		i.set(it->first, it->second);
	return i.serialize();
}

bool launch_read(const std::string &root, const std::string &console, LaunchRequest &out)
{
	std::vector<uint8_t> b;
	recover_old(launch_path(root, console));
	if (!file_read(launch_path(root, console), b, 1u << 18)) return false;
	out = launch_parse(std::string((const char *) b.data(), b.size()));
	if (out.console.empty()) out.console = console;
	return true;
}

WriteResult launch_write(const LaunchRequest &r)
{
	make_dirs(path_join(r.root, "launch"));
	std::string s = launch_serialize(r);
	return atomic_write(launch_path(r.root, r.console), (const uint8_t *) s.data(), s.size());
}

/* ---- the session report --------------------------------------------------- */

static const char *exit_name(ExitReason e)
{
	return e == EXIT_GAME_LIST ? "game_list" : e == EXIT_ERROR ? "error" : e == EXIT_SUSPEND ? "suspend" : "normal";
}

std::string session_serialize(const SessionReport &s)
{
	Ini i;
	i.set_int("version", LAUNCH_VERSION);
	i.set("session_id", clean_line(s.session_id, 64));
	i.set("console", s.console);
	i.set("game", s.game_path);
	i.set_int("seconds_played", s.seconds_played < 0 ? 0 : s.seconds_played);
	i.set("save_changed", s.save_changed ? "1" : "0");
	i.set("save_error", s.save_error ? "1" : "0");
	i.set("notice", clean_line(s.notice));
	std::string st;
	for (size_t k = 0; k < s.states_touched.size(); k++) st += (k ? "," : "") + strf("%d", s.states_touched[k]);
	i.set("states_touched", st);
	i.set("resume_present", s.resume_present ? "1" : "0");
	i.set("exit", exit_name(s.exit));
	i.set_int("ended_at", s.ended_at);
	return i.serialize();
}

SessionReport session_parse(const std::string &text)
{
	SessionReport s;
	Ini i;
	i.parse(text);
	if (i.has("version")) {
		int v = i.get_int("version", 1);
		if (v > LAUNCH_VERSION) s.newer_version = true;
		s.version = v < 1 ? 1 : v;
	}
	s.session_id = clean_line(i.get("session_id"), 64);
	s.console = lower(clean_line(i.get("console"), 16));
	s.game_path = i.get("game");
	int64_t sec = i.get_i64("seconds_played", 0);
	s.seconds_played = sec < 0 ? 0 : (sec > 86400 * 7 ? 86400 * 7 : sec);   /* a week of one session is a bad number */
	s.save_changed = parse_bool(i.get("save_changed"), false);
	s.save_error = parse_bool(i.get("save_error"), false);
	s.notice = clean_line(i.get("notice"));
	s.resume_present = parse_bool(i.get("resume_present"), false);
	std::string e = lower(trim(i.get("exit")));
	s.exit = e == "game_list" ? EXIT_GAME_LIST : e == "error" ? EXIT_ERROR : e == "suspend" ? EXIT_SUSPEND : EXIT_NORMAL;
	s.ended_at = i.get_i64("ended_at", 0);
	std::string st = i.get("states_touched");
	size_t p = 0;
	while (p < st.size()) {
		size_t c = st.find(',', p);
		if (c == std::string::npos) c = st.size();
		int v = atoi(st.substr(p, c - p).c_str());
		if (v >= 1 && v <= 10) s.states_touched.push_back(v);
		p = c + 1;
	}
	s.valid = !s.console.empty();
	return s;
}

bool session_read(const std::string &root, SessionReport &out)
{
	std::vector<uint8_t> b;
	recover_old(session_path(root));
	if (!file_read(session_path(root), b, 1u << 16)) return false;
	out = session_parse(std::string((const char *) b.data(), b.size()));
	return out.valid;
}

WriteResult session_write(const std::string &root, const SessionReport &s)
{
	make_dirs(path_join(root, "launch"));
	std::string t = session_serialize(s);
	return atomic_write(session_path(root), (const uint8_t *) t.data(), t.size());
}

void session_clear(const std::string &root)
{
	std::string p = session_path(root);
	file_remove(p);
	file_remove(p + ".old");
	file_remove(p + ".tmp");
}

/* ---- the tracker ----------------------------------------------------------- */

void SessionTracker::begin(const LaunchRequest &r)
{
	rep_ = SessionReport();
	rep_.session_id = r.session_id;
	rep_.console = r.console;
	rep_.game_path = r.game_path;
	touched_.clear();
	last_ms_ = 0;
	running_ = false;
	acc_ms_ = 0;
}

void SessionTracker::tick_running(uint64_t now_ms)
{
	if (running_ && now_ms >= last_ms_) {
		uint64_t d = now_ms - last_ms_;
		if (d > 2000) d = 2000;      /* a stall (the card, a suspend) is not play time */
		acc_ms_ += d;
	}
	last_ms_ = now_ms;
	running_ = true;
}

void SessionTracker::pause() { running_ = false; }

void SessionTracker::note_state_touched(int slot)
{
	if (slot >= 1 && slot <= 10) touched_.insert(slot);
}

void SessionTracker::note_save_written() { rep_.save_changed = true; }

void SessionTracker::note_save_error(const std::string &text)
{
	rep_.save_error = true;
	rep_.notice = clean_line(text);
}

void SessionTracker::note_notice(const std::string &text)
{
	if (!rep_.save_error) rep_.notice = clean_line(text);   /* a save failure is the one to keep */
}

SessionReport SessionTracker::finish(ExitReason why)
{
	SessionReport r = rep_;
	r.seconds_played = (int64_t) (acc_ms_ / 1000);
	r.states_touched.assign(touched_.begin(), touched_.end());
	r.exit = why;
	r.ended_at = now_unix();
	r.valid = true;
	return r;
}

}  // namespace pk
