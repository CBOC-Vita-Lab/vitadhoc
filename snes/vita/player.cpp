/* See player.h. Copyright CBOC-Vita. MIT licence (see snes/frontend/LICENSE). */
#include "player.h"
#include "wide.h"
#include "pk_patch.h"
#include "pk_util.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <algorithm>

namespace snesp {

using namespace pk;

/* ---- the writer queue ------------------------------------------------------ */

void Writer::post(WriterJob *j)
{
	SpinGuard g(lk_);
	todo_.push_back(j);
}

bool Writer::work_one(SnesPlayer *p)
{
	WriterJob *j = 0;
	{
		SpinGuard g(lk_);
		if (todo_.empty()) return false;
		j = todo_.front();
		todo_.erase(todo_.begin());
		running_++;
	}
	p->run_writer_job(j);
	SpinGuard g(lk_);
	running_--;
	done_.push_back(j);
	return true;
}

void Writer::take_done(std::vector<WriterJob *> &out)
{
	SpinGuard g(lk_);
	out.insert(out.end(), done_.begin(), done_.end());
	done_.clear();
}

bool Writer::idle()
{
	SpinGuard g(lk_);
	return todo_.empty() && running_ == 0;
}

int Writer::pending()
{
	SpinGuard g(lk_);
	return (int) todo_.size() + running_;
}

/* ---- helpers ------------------------------------------------------------------ */

static uint64_t hash64(const uint8_t *p, size_t n)
{
	uint64_t h = 0xcbf29ce484222325ull;
	size_t i = 0;
	for (; i + 8 <= n; i += 8) {
		uint64_t w;
		memcpy(&w, p + i, 8);
		h = (h ^ w) * 0x100000001b3ull;
		h ^= h >> 29;
	}
	for (; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull;
	return h ^ (h >> 32);
}

static std::string srm_for(const std::string &rom)
{
	size_t d = rom.find_last_of('.'), s = rom.find_last_of("/:");
	if (d == std::string::npos || (s != std::string::npos && d < s)) return rom + ".srm";
	return rom.substr(0, d) + ".srm";
}

/* menu shape (Original, Fit, Zoom, Full screen, Pixel perfect) <-> the SNES picture's shapes */
static int shape_to_pic(int v)
{
	switch (v) { case 2: return PIC_ZOOM; case 3: return PIC_FULL; case 4: return PIC_PIXEL; default: return PIC_ORIGINAL; }
}
static int pic_to_shape(int p)
{
	switch (p) { case PIC_ZOOM: return 2; case PIC_FULL: return 3; case PIC_PIXEL: return 4; default: return 0; }
}

static const char *kMultitapGames[] = { "BOMBERMAN", "SECRET OF MANA", "SEIKEN DENSETSU 3", "NBA JAM", "NBA GIVE", "NHL ", "FIFA", "MADDEN", NULL };

/* fields of pic_settings the player changed in the menu (only these are written back to the files, so a
 * per-game value the app sent in the launch file never leaks into picture.ini) */
enum { CH_SHAPE = 1, CH_PIXEL = 2, CH_QUALITY = 4, CH_GLOW = 8, CH_SCAN = 16, CH_GRID = 32, CH_HIRES = 64, CH_ILACE = 128,
       CH_MULTITAP = 256, CH_USEGAME = 512, CH_WIDE = 1024 };
static unsigned g_changed;
static pic_settings g_ps_file;

static void overlay_changed(pic_settings &dst, const pic_settings &src, unsigned m)
{
	if (m & CH_SHAPE) dst.shape = src.shape;
	if (m & CH_PIXEL) dst.pixel_sq = src.pixel_sq;
	if (m & CH_QUALITY) dst.quality = src.quality;
	if (m & CH_GLOW) dst.glow = src.glow;
	if (m & CH_SCAN) dst.scan = src.scan;
	if (m & CH_GRID) dst.grid = src.grid;
	if (m & CH_HIRES) dst.hires_blend = src.hires_blend;
	if (m & CH_ILACE) dst.interlace_blend = src.interlace_blend;
	if (m & CH_MULTITAP) dst.multitap = src.multitap;
	if (m & CH_WIDE) dst.wide = src.wide;
	if (m & CH_USEGAME) dst.use_game = src.use_game;
}

/* ---- construction ------------------------------------------------------------- */

SnesPlayer::SnesPlayer(Platform *pf) : pf_(pf)
{
	pic_defaults(&ps_);
	g_changed = 0;
	pic_defaults(&g_ps_file);
}

SnesPlayer::~SnesPlayer()
{
	menu_.release_textures();
	delete states_;
	delete store_;
}

void SnesPlayer::logf(const char *fmt, ...)
{
	char tmp[1024];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
	va_end(ap);
	if (n <= 0) return;
	/* any thread (the host's save code logs from the writer thread) */
	SpinGuard g(log_lk_);
	if (logbuf_.size() > 256 * 1024) { log_lost_++; return; }     /* the card was busy for very long */
	logbuf_.append(tmp, (size_t) n < sizeof tmp ? (size_t) n : sizeof tmp - 1);
}

/* The log goes to the card on the writer thread, at most every 5 s (an fflush on the game thread was a
 * 12-15 ms hitch); `now` writes it at once on this thread (the end). Game thread only. */
void SnesPlayer::log_flush(bool now)
{
	uint64_t t = pf_->now_us();
	if (!now && (log_busy_ || t - log_flushed_us_ < 5000000)) return;
	if (now) wait_writer();
	std::string text;
	{
		SpinGuard g(log_lk_);
		if (log_lost_) { logbuf_ += strf("(log: %u lines lost, buffer full)\n", log_lost_); log_lost_ = 0; }
		text.swap(logbuf_);
	}
	if (text.empty()) return;
	log_flushed_us_ = t;
	if (now) { pf_->log_write(text); return; }
	WriterJob *j = new WriterJob;
	j->kind = WriterJob::LOG;
	j->text.swap(text);
	log_busy_ = true;
	post(j);
}

void SnesPlayer::notice(const std::string &t, NoticeKind k, bool report)
{
	menu_.notices().push(t, k);
	if (report) session_.note_notice(t);      /* the app shows it when the player has ended */
	logf("notice%s: %s\n", k == NOTICE_ERROR ? " (error)" : "", t.c_str());
}

/* ---- the writer ----------------------------------------------------------------- */

void SnesPlayer::post(WriterJob *j)
{
	writer_.post(j);
	if (!pf_->wake_writer())
		while (writer_.work_one(this)) {}
}

void SnesPlayer::wait_writer()
{
	if (writer_.idle()) return;
	while (!writer_.idle()) {
		if (!pf_->wake_writer()) { while (writer_.work_one(this)) {} break; }
		pf_->pause_us(1000);    /* the writer runs at a lower priority on another core */
	}
	poll_writer();
}

void SnesPlayer::run_writer_job(WriterJob *j)
{
	switch (j->kind) {
	case WriterJob::SRM:
		j->result = snes_host_sram_write(j->bytes.data(), j->bytes.size());
		break;
	case WriterJob::STATE:
	case WriterJob::RESUME:
		j->rep = states_->commit(j->state);
		break;
	case WriterJob::SETTINGS: {
		/* picture.ini / games/<game>.ini: the same rule as before (a game file with own_settings=1 wins) */
		int r;
		if (j->ps.use_game) r = pic_save(j->path2.c_str(), &j->ps);
		else {
			r = pic_save(j->path.c_str(), &j->ps);
			pic_settings gs = j->ps;
			if (pic_load(j->path2.c_str(), &gs) && gs.use_game) { gs.use_game = 0; pic_save(j->path2.c_str(), &gs); }
		}
		Ini pl;
		pl.parse(j->text);
		WriteResult w = ini_save(path_join(snes_dir_, "player.ini"), pl);
		j->result = r ? r : (w != W_OK ? 100 + (int) w : 0);
		break;
	}
	case WriterJob::LOG:
		pf_->log_write(j->text);
		break;
	case WriterJob::WIDEMAP: {
		/* the learned map of the wide view: serialised here (not on the game thread), written with the safe writer */
		size_t need = snes_wide_map_save(NULL, 0);
		j->result = 0;
		if (need) {
			j->bytes.resize(need);
			size_t got = snes_wide_map_save(j->bytes.data(), j->bytes.size());
			if (got) {
				make_dirs(path_dir(j->path));
				WriteResult w = atomic_write(j->path, j->bytes.data(), got);
				j->result = w == W_OK ? 0 : 100 + (int) w;
			} else j->result = 1;
		}
		break;
	}
	}
}

void SnesPlayer::poll_writer()
{
	std::vector<WriterJob *> done;
	writer_.take_done(done);
	for (size_t i = 0; i < done.size(); i++) {
		WriterJob *j = done[i];
		switch (j->kind) {
		case WriterJob::SRM:
			srm_busy_ = false;
			srm_writes_++;
			if (j->result) snes_host_sram_forget();      /* failed: still to be written */
			srm_recheck_ = true;
			srm_result(j->result, "writer thread", pf_->now_us());
			break;
		case WriterJob::STATE:
			if (j->quick) {
				if (j->rep.status != ST_OK) notice(j->rep.text(), NOTICE_ERROR, true);
				else { session_.note_state_touched(j->state.slot); notice(strf("State saved to slot %d", j->state.slot), NOTICE_OK); }
				if (menu_.is_open()) menu_.refresh();
			} else {
				menu_.state_write_done(j->state.slot, j->rep);
				if (j->rep.status != ST_OK) session_.note_notice(j->rep.text());
			}
			logf("state slot %d written: %s\n", j->state.slot, j->rep.status == ST_OK ? "ok" : j->rep.text().c_str());
			break;
		case WriterJob::RESUME:
			resume_busy_ = false;
			if (j->rep.status != ST_OK) logf("resume point NOT written: %s\n", j->rep.text().c_str());
			session_.set_resume_present(states_->resume_present());
			break;
		case WriterJob::SETTINGS:
			logf("settings saved rc=%d (%s)\n", j->result, j->ps.use_game ? "this game" : "all games");
			break;
		case WriterJob::LOG:
			log_busy_ = false;
			break;
		case WriterJob::WIDEMAP:
			wide_busy_ = false;
			if (!j->result) snes_wide_map_mark_saved();
			logf("wide view map %s (%u bytes), rc=%d\n", j->result ? "NOT written" : "written", (unsigned) j->bytes.size(), j->result);
			break;
		}
		delete j;
	}
}

/* ---- start ---------------------------------------------------------------------- */

int SnesPlayer::migrate_old_states(const std::string &root, const std::string &game, std::string *log)
{
	/* The player before 1.4 kept four slots as <root>/snes/states/<game>.st0 .. .st3 (Left/Right picked
	 * slot 1-4, file N-1). They move to the shared layout, <root>/states/snes/<game>.stN, into the same slot
	 * when it is free, else into the first free one; a state that finds no free slot stays where it is.
	 * Nothing is overwritten or deleted. One listing of each folder, no lookups of missing names. */
	std::string old_dir = path_join(path_join(root, "snes"), "states");
	std::vector<std::string> old_names;
	if (!list_dir(old_dir, old_names)) return 0;
	std::string pre = game + ".st";
	std::vector<int> olds;
	for (size_t i = 0; i < old_names.size(); i++) {
		const std::string &n = old_names[i];
		if (n.size() == pre.size() + 1 && n.compare(0, pre.size(), pre) == 0 && n[pre.size()] >= '0' && n[pre.size()] <= '9')
			olds.push_back(n[pre.size()] - '0');
	}
	if (olds.empty()) return 0;
	std::sort(olds.begin(), olds.end());
	std::string new_dir = path_join(path_join(root, "states"), "snes");
	make_dirs(new_dir);
	std::vector<std::string> new_names;
	list_dir(new_dir, new_names);
	bool used[STATE_SLOTS];
	for (int k = 0; k < STATE_SLOTS; k++) {
		std::string b = game + strf(".st%d", k);
		used[k] = std::find(new_names.begin(), new_names.end(), b) != new_names.end() ||
		          std::find(new_names.begin(), new_names.end(), b + ".old") != new_names.end();
	}
	int moved = 0;
	for (size_t i = 0; i < olds.size(); i++) {
		int k = olds[i], dst = -1;
		if (!used[k]) dst = k;
		else for (int t = 0; t < STATE_SLOTS && dst < 0; t++) if (!used[t]) dst = t;
		std::string from = path_join(old_dir, game + strf(".st%d", k));
		if (dst < 0) { if (log) *log += strf("old state %s kept where it is: all ten slots are in use\n", from.c_str()); continue; }
		std::string to = path_join(new_dir, game + strf(".st%d", dst));
		bool ok = fs().rename_file(from, to);
		if (!ok) {
			/* another card or a refusing rename: copy, check, then remove the old one */
			std::vector<uint8_t> b;
			ok = file_read(from, b, 32u << 20) && atomic_write(to, b.data(), b.size(), EXISTS_NO) == W_OK;
			if (ok) file_remove(from);
		}
		if (ok) { used[dst] = true; moved++; }
		if (log) *log += strf("old state %s -> %s: %s\n", from.c_str(), to.c_str(), ok ? "moved" : "FAILED (kept)");
	}
	return moved;
}

std::string SnesPlayer::pic_game_ini() const { return path_join(path_join(snes_dir_, "games"), game_ + ".ini"); }

void SnesPlayer::settings_load()
{
	pic_defaults(&ps_);
	pic_load(path_join(snes_dir_, "picture.ini").c_str(), &ps_);
	pic_settings gs = ps_;
	if (pic_load(pic_game_ini().c_str(), &gs) && gs.use_game) ps_ = gs;
	g_ps_file = ps_;
	g_changed = 0;
	Ini pl;
	pl.load(path_join(snes_dir_, "player.ini"));
	steady_ = pl.get_int("steady_pacing", 1) != 0;
	handoff_ = pl.get_int("smooth_handoff", 1) != 0;
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* The launch file's choices: the shape and quality the app chose (unless this game has its own picture
 * file from the player's menu), then the app's per-game overrides ovr.<key>. */
void SnesPlayer::apply_overrides()
{
	if (!have_launch_) return;
	Ini raw;
	raw.load(launch_path(root_, "snes"));
	if (!ps_.use_game) {
		if (raw.has("shape")) ps_.shape = shape_to_pic(req_.shape);
		if (raw.has("quality")) ps_.quality = req_.quality == QUALITY_SMOOTH ? PIC_SMOOTH : PIC_SHARP;
	}
	int v;
	if ((v = req_.override_int("shape", -1)) >= 0) ps_.shape = shape_to_pic(clampi(v, 0, 4));
	if (req_.overrides.count("shape")) { PictureShape s; if (shape_parse(req_.overrides.find("shape")->second, &s)) ps_.shape = shape_to_pic(s); }
	if ((v = req_.override_int("quality", -1)) >= 0) ps_.quality = v ? PIC_SMOOTH : PIC_SHARP;
	if (req_.overrides.count("quality")) { std::string q = lower(req_.overrides.find("quality")->second); if (q == "smooth") ps_.quality = PIC_SMOOTH; else if (q == "sharp") ps_.quality = PIC_SHARP; }
	if ((v = req_.override_int("scanlines", -1)) >= 0) ps_.scan = clampi(v, 0, 10);
	if ((v = req_.override_int("lcd_grid", -1)) >= 0) ps_.grid = clampi(v, 0, 10);
	if ((v = req_.override_int("grid", -1)) >= 0) ps_.grid = clampi(v, 0, 10);
	if ((v = req_.override_int("glow", -1)) >= 0) ps_.glow = clampi(v, 0, 10);
	if ((v = req_.override_int("pixel_square", -1)) >= 0) ps_.pixel_sq = v ? 1 : 0;
	if ((v = req_.override_int("hires_blend", -1)) >= 0) ps_.hires_blend = v ? 1 : 0;
	if ((v = req_.override_int("interlace_blend", -1)) >= 0) ps_.interlace_blend = v ? 1 : 0;
	if ((v = req_.override_int("multitap", -1)) >= 0) ps_.multitap = clampi(v, 0, 2);
	if ((v = req_.override_int("wide_view", -1)) >= 0) ps_.wide = clampi(v, 0, 2);
	if (req_.overrides.count("wide_view")) { std::string w = lower(req_.overrides.find("wide_view")->second); if (w == "auto") ps_.wide = 0; else if (w == "on") ps_.wide = 1; else if (w == "off") ps_.wide = 2; }
	if ((v = req_.override_int("steady_pacing", -1)) >= 0) steady_ = v != 0;
	if ((v = req_.override_int("smooth_handoff", -1)) >= 0) handoff_ = v != 0;
}

void SnesPlayer::sync_menu_values()
{
	menu_.set_value(ROW_PIC_SHAPE, pic_to_shape(ps_.shape));
	menu_.set_value(ROW_PIC_QUALITY, ps_.quality == PIC_SMOOTH ? 1 : 0);
	menu_.set_value(ROW_PIC_SCANLINES, ps_.scan);
	menu_.set_value(ROW_PIC_GRID, ps_.grid);
	menu_.set_value(ROW_PIC_GLOW, ps_.glow);
	menu_.set_value(ROW_PIC_APPLY, ps_.use_game ? 1 : 0);
	menu_.set_value(ROW_PIXELS, ps_.pixel_sq ? 1 : 0);
	menu_.set_value(ROW_HIRES, ps_.hires_blend ? 1 : 0);
	menu_.set_value(ROW_INTERLACE, ps_.interlace_blend ? 1 : 0);
	menu_.set_value(ROW_MULTITAP, ps_.multitap);
	menu_.set_value(ROW_PIC_WIDE, ps_.wide);
	menu_.set_value(ROW_STEADY, steady_ ? 1 : 0);
	menu_.set_value(ROW_HANDOFF, handoff_ ? 1 : 0);
}

int SnesPlayer::multitap_wanted() const
{
	if (ps_.multitap == 1) return 1;
	if (ps_.multitap == 2) return 0;
	std::string t = snes_host_rom_title();
	for (size_t i = 0; i < t.size(); i++) if (t[i] >= 'a' && t[i] <= 'z') t[i] = (char) (t[i] - 32);
	for (int i = 0; kMultitapGames[i]; i++)
		if (t.find(kMultitapGames[i]) != std::string::npos) return 1;
	return 0;
}

StartResult SnesPlayer::start(int argc, char **argv, const std::string &root, bool system_enter_cross)
{
	StartResult res;
	init_shared();
	root_ = root;
	snes_dir_ = path_join(root, "snes");
	make_dirs(path_join(snes_dir_, "games"));
	make_dirs(path_join(path_join(root, "backups"), "snes"));

	/* 1. what to run: the launch file, else the argument VitAdhoc passes today ("rom" or "rom|save") */
	std::string arg;
	for (int i = argc - 1; i >= 1 && arg.empty(); i--)
		if (argv[i] && argv[i][0] && !ends_with_ci(argv[i], "snes.bin") && !starts_with(argv[i], "--")) arg = argv[i];
	std::string arg_rom = arg, arg_srm;
	{ size_t bar = arg_rom.find('|'); if (bar != std::string::npos) { arg_srm = arg_rom.substr(bar + 1); arg_rom.resize(bar); } }
	have_launch_ = launch_read(root, "snes", req_) && !req_.game_path.empty();
	if (have_launch_ && !arg_rom.empty() && arg_rom != req_.game_path) {
		logf("launch file names %s but the argument is %s: the file is old, the argument wins\n", req_.game_path.c_str(), arg_rom.c_str());
		have_launch_ = false;
	}
	if (!have_launch_) {
		req_ = LaunchRequest();
		req_.root = root;
		req_.console = "snes";
		req_.game_path = arg_rom;
		req_.save_path = arg_srm;
		req_.enter_is_cross = system_enter_cross;
	} else {
		if (req_.newer_version) logf("launch file is from a newer VitAdhoc (version %d): reading what this player knows\n", req_.version);
		for (size_t i = 0; i < req_.warnings.size(); i++) logf("launch file: %s\n", req_.warnings[i].c_str());
		req_.console = "snes";
	}
	logf("launch: %s, game=%s save=%s start=%d slot=%d patch=%s cpu=%d enter=%s\n", have_launch_ ? "launch/snes.ini" : "argument",
	     req_.game_path.c_str(), req_.save_path.c_str(), (int) req_.start, req_.slot, req_.patch_path.c_str(), req_.cpu_mhz,
	     req_.enter_is_cross ? "cross" : "circle");
	res.cpu_mhz = req_.cpu_mhz;
	res.enter_is_cross = req_.enter_is_cross;
	if (req_.game_path.empty()) {
		res.fatal1 = "No game was given.";
		res.fatal2 = "Start SNES games from VitAdhoc.";
		return res;
	}
	rom_ = req_.game_path;
	srm_ = req_.save_path.empty() ? srm_for(rom_) : req_.save_path;
	game_ = path_stem(rom_);
	session_.begin(req_);

	/* 2. states from the old folder and names */
	{
		std::string mlog;
		int n = migrate_old_states(root, game_, &mlog);
		if (!mlog.empty()) logf("%s", mlog.c_str());
		if (n) notice(strf("Moved %d old save state%s to the new slots", n, n == 1 ? "" : "s"), NOTICE_INFO, true);
	}

	/* 3. the battery save: a cut write is put back, then a newer RetroArch save is brought in */
	recover_old(srm_);
	if (!req_.retroarch_save.empty()) {
		uint64_t ra_sz = 0, sv_sz = 0;
		int64_t ra_t = 0, sv_t = 0;
		if (file_exists(req_.retroarch_save, &ra_sz, &ra_t)) {
			bool have = file_exists(srm_, &sv_sz, &sv_t);
			if (!have || ra_t > sv_t) {
				std::vector<uint8_t> b, old;
				bool ok = file_read(req_.retroarch_save, b, 1u << 20) && !b.empty();
				if (ok && have && file_read(srm_, old, 1u << 20)) {
					/* the save it replaces is kept in the backup folder first */
					std::string bak = path_join(path_join(path_join(root, "backups"), "snes"), game_ + ".before-retroarch.srm");
					ok = atomic_write(bak, old.data(), old.size()) == W_OK;
				}
				if (ok) ok = atomic_write(srm_, b.data(), b.size(), have ? EXISTS_YES : EXISTS_NO) == W_OK;
				if (ok) notice("Brought in the newer RetroArch save", NOTICE_OK, true);
				else notice("Could not bring in the RetroArch save: the old save was kept", NOTICE_ERROR, true);
				logf("retroarch save %s -> %s: %s\n", req_.retroarch_save.c_str(), srm_.c_str(), ok ? "imported" : "FAILED");
			}
		}
	}

	/* 4. the game, patched in memory when the app asked for it (never half-patched) */
	char err[200] = "";
	snes_host_set_backup_dir(path_join(path_join(root, "backups"), "snes").c_str());
	if (snes_host_init()) { res.fatal1 = "The SNES core could not start."; res.fatal2 = "Not enough memory."; return res; }
	int lr;
	if (!req_.patch_path.empty()) {
		std::vector<uint8_t> romb, out;
		if (!file_read(rom_, romb, 12u << 20)) { res.fatal1 = "This game could not be started."; res.fatal2 = "The game file could not be read."; return res; }
		PatchResult pr = patch_apply_files(req_.patch_path, romb, out);
		logf("patch %s (%s): %s\n", req_.patch_path.c_str(), patch_kind_name(pr.kind), pr.status == PS_OK ? "applied" : pr.text().c_str());
		if (pr.status != PS_OK) {
			res.fatal1 = "The patch could not be applied, so the game was not started.";
			res.fatal2 = pr.text();
			SessionReport r = session_.finish(EXIT_ERROR);
			r.notice = "The patch could not be applied: " + pr.text();
			session_write(root_, r);
			return res;
		}
		lr = snes_host_load_rom_mem(out.data(), out.size(), rom_.c_str(), srm_.c_str(), err, sizeof err);
	} else {
		lr = snes_host_load_rom(rom_.c_str(), srm_.c_str(), err, sizeof err);
	}
	if (lr) {
		logf("load failed: %s\n", err);
		res.fatal1 = "This game could not be started.";
		res.fatal2 = err;
		SessionReport r = session_.finish(EXIT_ERROR);
		r.notice = std::string("This game could not be started: ") + err;
		session_write(root_, r);
		return res;
	}
	{
		int nc = snes_host_cheats_load(root.c_str());
		logf("cheats: %d in the list\n", nc);
	}

	/* 5. states, settings, controls, the menu */
	StateLayout l;
	l.root = root; l.console = "snes"; l.name = game_;
	states_ = new States(l);
	states_->ensure_dirs();
	states_->rescan();
	session_.set_resume_present(states_->resume_present());
	settings_load();
	apply_overrides();
	pacer_.set_enabled(steady_);
	store_ = new ProfileStore(root, "snes", game_);
	store_->load_effective(model_snes(), req_.enter_is_cross, profile_);
	mapper_.configure(model_snes(), profile_, MapperConfig());

	MenuConfig cfg;
	cfg.game_name = req_.game_name.empty() ? game_ : req_.game_name;
	cfg.console_label = "Super Nintendo";
	cfg.exit_label = "Exit";
	cfg.exit_help = "Close VitAdhoc";
	cfg.caps = CAP_STATES | CAP_UNDO | CAP_RESET | CAP_SHAPE | CAP_QUALITY | CAP_SCANLINES | CAP_LCD_GRID | CAP_GLOW |
	           CAP_WIDE | CAP_PIC_PER_GAME | CAP_CONTROLS | CAP_TURBO | CAP_CHEATS | CAP_CHEAT_EDIT;
	cfg.enter_is_cross = req_.enter_is_cross;
	cfg.players = 4;
	cfg.accent = 0x7B6CFF;
	menu_.configure(cfg, this, states_);
	menu_.attach_input(&mapper_, store_, &model_snes(), &profile_);
	menu_.set_choices_allowed(ROW_PIC_SHAPE, (1u << 0) | (1u << 2) | (1u << 3) | (1u << 4));   /* no "Fit": Original already fills the height */
	{
		std::vector<std::string> px, onoff, mt, st;
		px.push_back("8:7 (console)"); px.push_back("Square");
		onoff.push_back("Off"); onoff.push_back("On");
		mt.push_back("Auto"); mt.push_back("On"); mt.push_back("Off");
		menu_.add_host_row(Menu::PAGE_PICTURE, ROW_PIXELS, "Pixel shape",
		                   "Pixel perfect only: the console's slightly wide pixels (8:7), or square ones.", px, 0, ROW_PIC_QUALITY);
		menu_.add_host_row(Menu::PAGE_PICTURE, ROW_HIRES, "Blend hi-res columns",
		                   "Averages pairs of columns in 512-wide pictures, smoothing their dithered see-through effects. Softens text.", onoff, 0, ROW_PIC_APPLY);
		menu_.add_host_row(Menu::PAGE_PICTURE, ROW_INTERLACE, "Blend interlace lines",
		                   "Averages pairs of lines in interlaced pictures, which removes their flicker.", onoff, 0, ROW_PIC_APPLY);
		menu_.add_host_row(Menu::PAGE_CONTROLS, ROW_MULTITAP, "Multitap",
		                   "The Super Multitap for players 3 and 4. Auto turns it on for games known to use it.", mt, 0, ROW_CTL_MAPPING);
		menu_.add_host_row(Menu::PAGE_SPEED, ROW_STEADY, "Steady pacing",
		                   "When a scene is too heavy for every frame, every second frame is shown, evenly. The game itself keeps full speed.",
		                   onoff, 1);
		menu_.add_host_row(Menu::PAGE_SPEED, ROW_HANDOFF, "Smooth hand-over",
		                   "Draws each picture on its own thread, so heavy picture options do not slow the game down. Turn it off if the picture ever misbehaves.",
		                   onoff, 1);
	}
	sync_menu_values();
	snes_host_set_multitap(multitap_wanted());
	wide_start();

	logf("title: %s  pal=%d  sram=%u  picture: shape=%d quality=%d glow=%d scan=%d grid=%d multitap=%d own=%d steady=%d handoff=%d\n",
	     snes_host_rom_title(), snes_host_is_pal(), (unsigned) snes_host_sram_size(), ps_.shape, ps_.quality, ps_.glow, ps_.scan,
	     ps_.grid, ps_.multitap, ps_.use_game, steady_ ? 1 : 0, handoff_ ? 1 : 0);
	states_->resume_mark(pf_->now_us() / 1000);
	res.ok = true;
	return res;
}

void SnesPlayer::apply_start_mode()
{
	std::vector<uint8_t> d;
	if (req_.start == START_CONTINUE) {
		if (!states_->resume_present()) { notice("No point to continue from: the game starts from the beginning", NOTICE_INFO); return; }
		States::PatchMatch pm = states_->resume_patch(req_.patch_key);
		if (pm == States::PATCH_DIFFERENT || (pm == States::PATCH_UNKNOWN && !req_.patch_key.empty())) {
			notice("The resume point was made with another patch: the game starts from the beginning", NOTICE_INFO);
			return;
		}
		StateStatus st = states_->load_resume(d);
		if (st != ST_OK) { notice(std::string("Could not continue: ") + state_status_text(st), NOTICE_ERROR); return; }
		if (!restore_state(d.data(), d.size())) { notice("Could not continue: the resume point does not fit this game", NOTICE_ERROR); return; }
		logf("continued from the resume point\n");
	} else if (req_.start == START_SLOT) {
		StateStatus st = states_->load(req_.slot, d);
		if (st != ST_OK) { notice(strf("Slot %d: %s", req_.slot, state_status_text(st)), st == ST_EMPTY ? NOTICE_INFO : NOTICE_ERROR); return; }
		if (!restore_state(d.data(), d.size())) { notice(strf("Slot %d could not be loaded: this game cannot read it", req_.slot), NOTICE_ERROR); return; }
		session_.note_state_touched(req_.slot);
		notice(strf("State loaded from slot %d", req_.slot), NOTICE_OK);
	}
}

/* ---- per frame ------------------------------------------------------------------- */

bool SnesPlayer::input(const PadSample pads[MAX_PLAYERS], bool tapped, uint64_t now_ms, uint32_t frame)
{
	now_ms_ = now_ms;
	for (int i = 0; i < MAX_PLAYERS; i++) last_pads_[i] = pads[i];
	mapper_.update(pads, now_ms, frame, &in_);
	if (in_.menu || tapped) return true;
	for (int i = 0; i < MAX_PLAYERS; i++) snes_host_set_pad(i, in_.pad[i]);
	session_.tick_running(now_ms);
	if (in_.fired & FIRE_QSAVE) quick_save();
	if (in_.fired & FIRE_QLOAD) quick_load();
	return false;
}

bool SnesPlayer::want_render(uint32_t frame) const
{
	return !steady_ || pacer_.mode() == PACE_FULL || (frame & 1) == 0;
}

void SnesPlayer::drawn_cost(uint32_t cost_us)
{
	if (!steady_) { pacer_.add(cost_us); return; }
	bool changed = false;
	PaceMode m = pacer_.add(cost_us, &changed);
	if (changed) {
		logf("pace: %s -> %s (frame cost %.1f ms; %s)\n", pace_mode_name(pace_logged_), pace_mode_name(m), cost_us / 1000.0,
		     m == PACE_HALF ? "every second frame is drawn, evenly" : "every frame is drawn again");
		pace_logged_ = m;
	}
}

std::string SnesPlayer::pace_line()
{
	Pacer::Counts c = pacer_.take_counts();
	return strf("pace=%s drawn=%d over=%d max=%.1f changes=%d", pace_mode_name(pacer_.mode()), c.drawn, c.over, c.max_us / 1000.0, c.changes);
}

void SnesPlayer::quick_save()
{
	std::vector<uint8_t> d;
	if (!capture_state(d)) { notice("Could not save the state: the game could not be captured", NOTICE_ERROR); return; }
	Frame f;
	current_frame(&f);
	WriterJob *j = new WriterJob;
	j->kind = WriterJob::STATE;
	j->quick = true;
	j->state = states_->prepare(1, d.data(), d.size(), f, play_seconds());
	post(j);
}

void SnesPlayer::quick_load()
{
	wait_writer();
	std::vector<uint8_t> d, now;
	StateStatus st = states_->load(1, d);
	if (st != ST_OK) { notice(st == ST_EMPTY ? std::string("Slot 1 is empty") : state_status_text(st), st == ST_EMPTY ? NOTICE_INFO : NOTICE_ERROR); return; }
	if (capture_state(now)) states_->undo_store(now.data(), now.size());
	if (!restore_state(d.data(), d.size())) { notice("The state could not be loaded: this game or version cannot read it", NOTICE_ERROR); return; }
	session_.note_state_touched(1);
	notice("State loaded from slot 1", NOTICE_OK);
}

void SnesPlayer::resume_post(bool sync)
{
	std::vector<uint8_t> d;
	if (!capture_state(d)) return;
	Frame f;
	current_frame(&f);
	WriterJob *j = new WriterJob;
	j->kind = WriterJob::RESUME;
	j->state = states_->prepare_resume(d.data(), d.size(), f, play_seconds(), req_.patch_key);
	resume_busy_ = true;
	states_->resume_mark(now_ms_);
	post(j);
	if (sync) wait_writer();
}

void SnesPlayer::after_frame(uint64_t now_us)
{
	int dt = prev_frame_us_ ? (int) ((now_us - prev_frame_us_) / 1000) : 16;
	if (dt < 0 || dt > 1000) dt = 16;
	prev_frame_us_ = now_us;
	menu_.notices().update(dt);
	now_ms_ = now_us / 1000;
	poll_writer();
	srm_watch(now_us);
	wide_watch(now_us);
	/* the resume point: every 2 minutes, and soon after the game saved (at most every 30 s) */
	if (!resume_busy_) {
		bool after_save = resume_after_srm_at_ && now_us >= resume_after_srm_at_ && states_->resume_due(now_ms_, 30000);
		if (after_save || states_->resume_due(now_ms_, 120000)) { resume_after_srm_at_ = 0; resume_post(false); }
	}
	log_flush(false);
}

/* ---- battery RAM ----------------------------------------------------------------- */
/* Written by the writer thread: 2 s after it changed and settled for 1 s, or every 30 s while it keeps
 * changing (Super FX work RAM). Change detection: a 64-bit hash every 8th pass, the exact compare only
 * when it moved (Snes9x has no usable dirty flag). A failed write leaves the file untouched (host.cpp),
 * keeps the RAM unsaved, says why (red notice, log, session report) and is tried again after 10 s,
 * doubling to 60 s. */

bool SnesPlayer::srm_post()
{
	if (srm_busy_) return false;
	if (srm_buf_.size() < 256 * 1024) srm_buf_.resize(256 * 1024);
	size_t n = snes_host_sram_take(srm_buf_.data(), srm_buf_.size());
	if (!n) return false;
	WriterJob *j = new WriterJob;
	j->kind = WriterJob::SRM;
	j->bytes.assign(srm_buf_.begin(), srm_buf_.begin() + (long) n);
	srm_busy_ = true;
	post(j);
	return true;
}

void SnesPlayer::srm_result(int r, const char *how, uint64_t now)
{
	if (!r) {
		if (save_failing_) { logf("sram save recovered (%s)\n", how); notice("The game is saved again", NOTICE_OK); }
		else if (srm_writes_ <= 3 || srm_writes_ % 50 == 0) logf("sram save rc=0 (%s, write %d)\n", how, srm_writes_);
		save_failing_ = false;
		save_msg_.clear();
		srm_backoff_s_ = 10;
		srm_retry_at_ = 0;
		session_.note_save_written();
		resume_after_srm_at_ = now + 1000000;
		return;
	}
	char why[200] = "";
	snes_host_save_error(why, sizeof why);
	save_msg_ = strf("Could not save the game: %s. The old save was kept; trying again.", why[0] ? why : "unknown error");
	logf("sram save FAILED rc=%d (%s): %s - the file on the card was not touched; retry in %u s\n", r, how, why, srm_backoff_s_);
	if (!save_failing_) notice(save_msg_, NOTICE_ERROR);
	session_.note_save_error(save_msg_);
	save_failing_ = true;
	srm_retry_at_ = now + (uint64_t) srm_backoff_s_ * 1000000;
	if (srm_backoff_s_ < 60) srm_backoff_s_ = srm_backoff_s_ * 2 > 60 ? 60 : srm_backoff_s_ * 2;
}

void SnesPlayer::srm_watch(uint64_t now)
{
	if ((++srm_pass_ & 7) == 0 || srm_recheck_) {
		const size_t sz = snes_host_sram_size();
		const bool recheck = srm_recheck_;
		srm_recheck_ = false;
		if (sz) {
			const uint64_t h = hash64(snes_host_sram_data(), sz);
			const bool moved = !srm_hash_valid_ || h != srm_hash_;
			if (moved) { srm_hash_ = h; srm_hash_valid_ = true; srm_last_change_ = now; }
			if (moved || recheck) srm_dirty_ = snes_host_sram_dirty() != 0;
		} else srm_dirty_ = false;
	}
	if (srm_dirty_) {
		if (!srm_dirty_since_) srm_dirty_since_ = now;
		if (now - srm_dirty_since_ > 2000000 && (now - srm_last_change_ > 1000000 || now - srm_last_write_ > 30000000) &&
		    now >= srm_retry_at_ && srm_post()) {
			srm_dirty_since_ = 0;
			srm_last_write_ = now;
			srm_recheck_ = true;    /* what was handed over counts as saved: look again next pass */
		}
	} else srm_dirty_since_ = 0;
}

void SnesPlayer::suspend()
{
	wait_writer();
	if (snes_host_sram_dirty()) {
		int r = snes_host_sram_save();
		logf("sram save before suspend rc=%d\n", r);
		srm_result(r, "suspend", pf_->now_us());
	}
	srm_dirty_since_ = 0;
	srm_recheck_ = true;
	session_.pause();
}

/* ---- the menu ---------------------------------------------------------------------- */

void SnesPlayer::open_menu(int player)
{
	session_.pause();
	mapper_.menu_opened();
	menu_.open(player);
	/* the battery RAM is written once the menu is on screen (menu_drawn), on the writer thread */
	menu_srm_pending_ = snes_host_sram_dirty() != 0;
	pacer_.reset(PACE_FULL);
}

void SnesPlayer::menu_drawn()
{
	if (menu_srm_pending_) { menu_srm_pending_ = false; if (srm_post()) { srm_dirty_since_ = 0; srm_last_write_ = pf_->now_us(); } }
}

MenuEvent SnesPlayer::menu_update(const MenuInput &in)
{
	MenuEvent ev = menu_.update(in);
	poll_writer();
	if (ev == MENU_RESUME || ev == MENU_RESET_DONE) {
		mapper_.menu_closed(in.pads);
		if (settings_dirty_) settings_queue_save();
		snes_host_set_multitap(multitap_wanted());
		srm_recheck_ = true;
		pacer_.reset(PACE_FULL);
		if (ev == MENU_RESET_DONE) notice("The game was reset", NOTICE_INFO);
	}
	return ev;
}

void SnesPlayer::settings_queue_save()
{
	settings_dirty_ = false;
	WriterJob *j = new WriterJob;
	j->kind = WriterJob::SETTINGS;
	j->ps = g_ps_file;
	overlay_changed(j->ps, ps_, g_changed);
	j->path = path_join(snes_dir_, "picture.ini");
	j->path2 = pic_game_ini();
	Ini pl;
	pl.set_int("steady_pacing", steady_ ? 1 : 0);
	pl.set_int("smooth_handoff", handoff_ ? 1 : 0);
	j->text = pl.serialize();
	post(j);
}

/* ---- MenuHost ------------------------------------------------------------------------ */

void SnesPlayer::on_change(int row, int v)
{
	switch (row) {
	case ROW_PIC_SHAPE: ps_.shape = shape_to_pic(v); g_changed |= CH_SHAPE; break;
	case ROW_PIC_QUALITY: ps_.quality = v ? PIC_SMOOTH : PIC_SHARP; g_changed |= CH_QUALITY; break;
	case ROW_PIC_SCANLINES: ps_.scan = clampi(v, 0, 10); g_changed |= CH_SCAN; break;
	case ROW_PIC_GRID: ps_.grid = clampi(v, 0, 10); g_changed |= CH_GRID; break;
	case ROW_PIC_GLOW: ps_.glow = clampi(v, 0, 10); g_changed |= CH_GLOW; break;
	case ROW_PIC_APPLY:
		ps_.use_game = v ? 1 : 0;
		/* "This game" stores every value as now shown; "All games" only what was changed here */
		g_changed |= CH_USEGAME | (v ? 0x7FF : 0);
		break;
	case ROW_PIXELS: ps_.pixel_sq = v ? 1 : 0; g_changed |= CH_PIXEL; break;
	case ROW_HIRES: ps_.hires_blend = v ? 1 : 0; g_changed |= CH_HIRES; break;
	case ROW_INTERLACE: ps_.interlace_blend = v ? 1 : 0; g_changed |= CH_ILACE; break;
	case ROW_MULTITAP: ps_.multitap = clampi(v, 0, 2); g_changed |= CH_MULTITAP; snes_host_set_multitap(multitap_wanted()); break;
	case ROW_PIC_WIDE: ps_.wide = clampi(v, 0, 2); g_changed |= CH_WIDE; break;
	case ROW_STEADY: steady_ = v != 0; pacer_.set_enabled(steady_); break;
	case ROW_HANDOFF: handoff_ = v != 0; break;
	default: return;
	}
	settings_dirty_ = true;
	pf_->picture_changed();
}

void SnesPlayer::on_reset()
{
	snes_host_reset();
	srm_recheck_ = true;
	logf("reset\n");
}

bool SnesPlayer::capture_state(std::vector<uint8_t> &out)
{
	size_t n = snes_host_state_size(), len = 0;
	if (!n) return false;
	out.resize(n);
	if (snes_host_state_save_buf(out.data(), out.size(), &len)) { out.clear(); return false; }
	out.resize(len);
	return true;
}

bool SnesPlayer::restore_state(const uint8_t *data, size_t n)
{
	int r = snes_host_state_load_buf(data, n);
	srm_recheck_ = true;
	pacer_.reset(PACE_FULL);
	return r == 0;
}

bool SnesPlayer::current_frame(Frame *f) { return pf_->current_frame(f); }

int64_t SnesPlayer::play_seconds() { return session_.seconds_played(); }

void SnesPlayer::list_cheats(std::vector<CheatItem> &out)
{
	out.clear();
	for (int i = 0, n = snes_host_cheat_count(); i < n; i++) {
		snes_cheat_info ci;
		if (snes_host_cheat_get(i, &ci)) continue;
		CheatItem it;
		it.name = ci.label[0] ? ci.label : ci.code;
		it.on = ci.enabled != 0;
		it.available = ci.usable != 0;
		out.push_back(it);
	}
}

static std::string cheat_name(int i)
{
	snes_cheat_info ci;
	if (snes_host_cheat_get(i, &ci)) return std::string();
	return ci.label[0] ? ci.label : ci.code;
}

void SnesPlayer::set_cheat(int index, bool on)
{
	int r = snes_host_cheat_enable(index, on ? 1 : 0);
	char why[200];
	if (r < 0) { notice("This cheat cannot be switched on here", NOTICE_ERROR); return; }
	if (snes_host_cheat_error(why, sizeof why)) notice(std::string("Cheat ") + (on ? "on" : "off") + ", but the list could not be saved: " + why, NOTICE_ERROR);
	else notice(std::string(on ? "Cheat on: " : "Cheat off: ") + cheat_name(index), on ? NOTICE_OK : NOTICE_INFO);
}

void SnesPlayer::add_cheat() { pf_->ask_cheat_text(); }

void SnesPlayer::cheat_entered(bool ok, const std::string &code_in, const std::string &name)
{
	std::string code = trim(code_in);
	if (!ok || code.empty()) return;
	int r = snes_host_cheat_add(code.c_str(), trim(name).c_str());
	char why[200];
	if (r < 0) {
		const char *t = r == SNES_CHEAT_E_FORMAT ? "That is not a cheat code. Use XXXX-XXXX (Game Genie), AAAAAAVV (Pro Action Replay) or AAAAAA=VV"
		              : r == SNES_CHEAT_E_FULL ? "The cheat list is full"
		              : r == SNES_CHEAT_E_TOO_LONG ? "That code is too long (16 parts at most)"
		              : r == SNES_CHEAT_E_RANGE ? "That code points at address 0, which cannot be changed"
		              : "The cheat could not be added";
		notice(t, NOTICE_ERROR);
	} else if (snes_host_cheat_error(why, sizeof why)) {
		notice(std::string("Cheat added and on, but the list could not be saved: ") + why, NOTICE_ERROR);
	} else {
		notice("Cheat added and on: " + cheat_name(r), NOTICE_OK);
	}
	logf("cheat add '%s' -> %d\n", code.c_str(), r);
	menu_.refresh();
}

bool SnesPlayer::delete_cheat(int index)
{
	int r = snes_host_cheat_remove(index);
	char why[200];
	if (r == 0 && snes_host_cheat_error(why, sizeof why)) notice(std::string("Cheat deleted, but the list could not be saved: ") + why, NOTICE_ERROR);
	return r == 0;
}

void SnesPlayer::on_state_touched(int slot) { session_.note_state_touched(slot); }

void SnesPlayer::on_controls_changed(WriteResult rc)
{
	if (rc != W_OK) { logf("controls NOT saved: %s\n", write_result_text(rc)); session_.note_notice(std::string("Could not save the controls: ") + write_result_text(rc)); }
}

bool SnesPlayer::queue_state_write(StateJob &job)
{
	WriterJob *j = new WriterJob;
	j->kind = WriterJob::STATE;
	j->state.slot = job.slot;
	j->state.data.swap(job.data);
	j->state.thumb.swap(job.thumb);
	j->state.shot.swap(job.shot);
	j->state.play_seconds = job.play_seconds;
	j->state.saved = job.saved;
	j->state.frame_w = job.frame_w;
	j->state.frame_h = job.frame_h;
	post(j);
	return true;
}

/* ---- the end ---------------------------------------------------------------------- */

/* ---- wide view ----------------------------------------------------------------------- */

void SnesPlayer::wide_start()
{
	size_t n = 0;
	const uint8_t *img = snes_host_rom_image(&n);
	snes_wide_set_game(img, n, snes_host_rom_title());
	wide_path_ = path_join(path_join(snes_dir_, "wide"), game_ + ".wmap");
	wide_last_us_ = pf_->now_us();
	std::vector<uint8_t> b;
	if (file_read(wide_path_, b, 16u << 20) && !b.empty()) {
		int r = snes_wide_map_load(b.data(), b.size());
		logf("wide view map loaded: %u bytes rc=%d\n", (unsigned) b.size(), r);
	}
}

void SnesPlayer::wide_apply(int scr_w, int scr_h)
{
	int m = pic_wide_margin(&ps_, scr_w, scr_h, SNES_WIDE_MAX_MARGIN);
	bool want = m > 0 && ps_.wide != 2;
	if (want && ps_.wide == 0 && pacer_.mode() != PACE_FULL) want = false;
	if (!want) m = 0;
	if (want == wide_on_ && m == wide_m_) return;
	snes_wide_set(want ? 1 : 0, m);
	wide_on_ = want; wide_m_ = m;
	logf("wide view %s margin=%d (mode %d, shape %d)\n", want ? "on" : "off", m, ps_.wide, ps_.shape);
}

void SnesPlayer::wide_post(bool final_)
{
	if (wide_path_.empty() || !snes_wide_map_key()) return;
	if (!snes_wide_map_dirty()) return;
	if (wide_busy_) { if (!final_) return; wait_writer(); poll_writer(); if (wide_busy_) return; }
	WriterJob *j = new WriterJob;
	j->kind = WriterJob::WIDEMAP;
	j->path = wide_path_;
	wide_busy_ = true;
	post(j);
	if (final_) { wait_writer(); poll_writer(); }
}

/* every 30 s while the map has new cells (a game thread check; the work is the writer thread's) */
void SnesPlayer::wide_watch(uint64_t now_us)
{
	if (!wide_on_ && !snes_wide_map_dirty()) return;
	if (now_us - wide_last_us_ < 30u * 1000000u) return;
	wide_last_us_ = now_us;
	wide_post(false);
}

void SnesPlayer::finish(ExitReason why)
{
	wait_writer();
	wide_post(true);
	if (settings_dirty_) settings_queue_save();
	/* the resume point: where the game was left */
	if (states_ && snes_host_state_size()) resume_post(true);
	wait_writer();
	if (snes_host_sram_dirty()) {
		int r = snes_host_sram_save();
		logf("final sram save rc=%d\n", r);
		srm_result(r, "final", pf_->now_us());
	}
	if (states_) session_.set_resume_present(states_->resume_present());
	SessionReport rep = session_.finish(why);
	WriteResult w = session_write(root_, rep);
	logf("session: %lld s played, save_changed=%d save_error=%d states=%d resume=%d exit=%d -> launch/session.txt %s\n",
	     (long long) rep.seconds_played, rep.save_changed ? 1 : 0, rep.save_error ? 1 : 0, (int) rep.states_touched.size(),
	     rep.resume_present ? 1 : 0, (int) why, w == W_OK ? "written" : write_result_text(w));
	log_flush(true);
}

}  // namespace snesp
