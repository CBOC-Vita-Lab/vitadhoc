/* See player.h. Copyright CBOC-Vita. MIT licence (see snes/frontend/LICENSE). */
#include "player.h"
#include "wide.h"
#include "pk_patch.h"
#include "pk_ra_plat.h"
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
		if (todo_.empty()) { return p->speed_pack(); }     /* nothing to write: pack the newest rewind state, if one waits */
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

/* menu shape (Fit, Fit (not offered), Zoom, Stretch, Pixel perfect) <-> the SNES picture's shapes */
static int word_scaler(const std::string &w0)
{
	const std::string w = lower(w0);
	if (w == "auto") return SCALER_AUTO;
	if (w == "off" || w == "sharp") return SCALER_OFF;
	if (w == "mmpx" || w == "mmpx2") return SCALER_MMPX2;
	if (w == "xbr2") return SCALER_XBR2;
	if (w == "xbr3") return SCALER_XBR3;
	return -1;
}

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
       CH_MULTITAP = 256, CH_USEGAME = 512, CH_WIDE = 1024, CH_SCALER = 2048 };
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
	if (m & CH_SCALER) dst.scaler = src.scaler;
	if (m & CH_USEGAME) dst.use_game = src.use_game;
}

/* ---- construction ------------------------------------------------------------- */

/* 1.4 defaults (docs/DEFAULTS-1.4.md): the picture as a television showed it. Pseudo hi-res games alternate
 * columns to fake transparency and interlaced pictures flicker between fields; averaging pairs shows what the
 * hardware's output blended anyway. A picture.ini or game file that sets them keeps its value. */
static void apply_1_4_defaults(pic_settings *s)
{
	s->hires_blend = 1;
	s->interlace_blend = 1;
}

SnesPlayer::SnesPlayer(Platform *pf) : pf_(pf), glue_(this)
{
	pic_defaults(&ps_);
	apply_1_4_defaults(&ps_);
	g_changed = 0;
	pic_defaults(&g_ps_file);
	apply_1_4_defaults(&g_ps_file);
	eff_ = ps_;
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
	case WriterJob::LOOKMEM:
		/* the game's look memory: the safe writer, or gone when the governor says it is no longer needed */
		if (j->remove_file) { file_remove(j->path); j->result = 0; }
		else {
			make_dirs(path_dir(j->path));
			WriteResult w = atomic_write(j->path, (const uint8_t *) j->text.data(), j->text.size());
			j->result = w == W_OK ? 0 : 100 + (int) w;
		}
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
		case WriterJob::LOOKMEM:
			look_busy_ = false;
			if (j->result) look_written_.clear();      /* not written: try again at the next chance */
			logf("look memory %s rc=%d\n", j->remove_file ? "removed" : "written", j->result);
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

static int clampi(int v, int lo, int hi);
static int imgq_of(int scaler, int look_mode);       /* below, with Image quality's other helpers */

void SnesPlayer::settings_load()
{
	pic_defaults(&ps_);
	apply_1_4_defaults(&ps_);
	pic_load(path_join(snes_dir_, "picture.ini").c_str(), &ps_);
	pic_settings gs = ps_;
	if (pic_load(pic_game_ini().c_str(), &gs) && gs.use_game) ps_ = gs;
	g_ps_file = ps_;
	g_changed = 0;
	Ini pl;
	pl.load(path_join(snes_dir_, "player.ini"));
	steady_ = pl.get_int("steady_pacing", 1) != 0;
	handoff_ = pl.get_int("smooth_handoff", 1) != 0;
	pal_mode_ = pk::pal_mode_clamp(pl.get_int("pal_speed", pk::PAL_SMOOTH));
	look_mode_ = clampi(pl.get_int("look_mode", 0), 0, 2);
	if (look_mode_ == 1) look_mode_ = 0;        /* "Full" is gone with the Image look row: a scaler as set is never eased now */
	/* An older file that says "no scaler" (or the Light look) with the Smooth video filter drew bilinear. Original draws sharp (as in the main app,
	 * where choosing bilinear over Original goes to Auto), so that choice is kept the way the app keeps it: the scaler goes to Auto. */
	if (imgq_of(ps_.scaler, look_mode_) == IMGQ_ORIGINAL && ps_.quality == PIC_SMOOTH) { ps_.scaler = SCALER_AUTO; if (look_mode_ == 2) look_mode_ = 0; g_changed |= CH_SCALER; }
	ff_speed_ = clampi(pl.get_int("ff_speed", 0), 0, FFSPD_COUNT - 1);
	ff_sound_ = pl.get_int("ff_sound", 1) < 0 ? 0 : pl.get_int("ff_sound", 1) > 2 ? 2 : pl.get_int("ff_sound", 1);
	ff_toggle_ = pl.get_int("ff_mode", 0) == 1 ? 1 : 0;
	rewind_on_ = pl.get_int("rewind", 1) != 0;
	rewind_len_ = pl.get_int("rewind_seconds", 60) <= 30 ? 0 : 1;
	rewind_mb_ = clampi(pl.get_int("rewind_mb", 12), 4, 24);
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
/* "Image quality", the main app's one row (Auto, Original, Enhanced 2x/3x/4x, MMPX 2x), over this player's pixel art scaler and look:
 * Auto = the governed scaler, Original = no scaler and sharp bilinear (the old Light look is the same), Enhanced 2x / 3x = xBR 2x / 3x, MMPX 2x =
 * MMPX. There is no 4x scaler here: that choice is not offered. Old files keep working: the scaler and look_mode keys are unchanged. */
/* The old Light look (look_mode 2, a global key) is Original only where the game's own picture file leaves the scaler on Auto or Original:
 * a scaler chosen for this game wins over the global key, so the row, the picture and the governor always agree. */
static bool light_on(int look_mode, int scaler) { return look_mode == 2 && (scaler == SCALER_AUTO || scaler == SCALER_OFF); }
static int imgq_of(int scaler, int look_mode)
{
	if (light_on(look_mode, scaler)) return IMGQ_ORIGINAL;
	switch (scaler) {
	case SCALER_OFF: return IMGQ_ORIGINAL;
	case SCALER_MMPX2: return IMGQ_MMPX_2X;
	case SCALER_XBR2: return IMGQ_ENHANCED_2X;
	case SCALER_XBR3: return IMGQ_ENHANCED_3X;
	default: return IMGQ_AUTO;
	}
}
static int scaler_of(int q)
{
	switch (q) {
	case IMGQ_ORIGINAL: return SCALER_OFF;
	case IMGQ_MMPX_2X: return SCALER_MMPX2;
	case IMGQ_ENHANCED_2X: return SCALER_XBR2;
	case IMGQ_ENHANCED_3X: case IMGQ_ENHANCED_4X: return SCALER_XBR3;
	default: return SCALER_AUTO;
	}
}

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
	if ((v = req_.override_int("scaler", -1)) >= 0) ps_.scaler = clampi(v, 0, SCALER_CHOICES - 1);
	if (req_.overrides.count("scaler")) { int sc = word_scaler(req_.overrides.find("scaler")->second); if (sc >= 0) ps_.scaler = sc; }
	if ((v = req_.override_int("steady_pacing", -1)) >= 0) steady_ = v != 0;
	if ((v = req_.override_int("smooth_handoff", -1)) >= 0) handoff_ = v != 0;
	if ((v = req_.override_int("rewind", -1)) >= 0) rewind_on_ = v != 0;
	if ((v = req_.override_int("rewind_mb", -1)) >= 0) rewind_mb_ = clampi(v, 4, 24);
	if ((v = req_.override_int("ff_speed", -1)) >= 0) ff_speed_ = clampi(v, 0, FFSPD_COUNT - 1);
	/* for the session runner's A/B: pal_speed 0 = true 50 Hz, 1 = fast 60 Hz (or the words); look_mode 0..2 (or auto / full / light) */
	if ((v = req_.override_int("pal_speed", -1)) >= 0) pal_mode_ = pk::pal_mode_clamp(v);
	if (req_.overrides.count("pal_speed")) { std::string w = lower(req_.overrides.find("pal_speed")->second); if (w == "fast" || w == "60") pal_mode_ = pk::PAL_FAST; else if (w == "true" || w == "50") pal_mode_ = pk::PAL_TRUE; else if (w == "smooth") pal_mode_ = pk::PAL_SMOOTH; }
	if ((v = req_.override_int("look_mode", -1)) >= 0) look_mode_ = clampi(v, 0, 2);
	if (req_.overrides.count("look_mode")) { std::string w = lower(req_.overrides.find("look_mode")->second); if (w == "auto") look_mode_ = 0; else if (w == "full") look_mode_ = 1; else if (w == "light") look_mode_ = 2; }
}

void SnesPlayer::sync_menu_values()
{
	menu_.set_value(ROW_PIC_SHAPE, pic_to_shape(ps_.shape));
	menu_.set_value(ROW_PIC_QUALITY, imgq_of(ps_.scaler, look_mode_));
	menu_.set_value(ROW_FILTER, imgq_of(ps_.scaler, look_mode_) == IMGQ_ORIGINAL ? 0 : ps_.quality == PIC_SMOOTH ? 1 : 0);
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
	menu_.set_value(ROW_PALSPEED, pk::pal_row_of(pal_mode_));
	menu_.set_value(ROW_SPD_FF, ff_speed_);
	menu_.set_value(ROW_SPD_SOUND, ff_sound_);
	menu_.set_value(ROW_SPD_MODE, ff_toggle_);
	menu_.set_value(ROW_SPD_REWIND, rewind_on_);
	menu_.set_value(ROW_REWIND_LEN, rewind_len_);
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
	std::vector<uint8_t> ra_img;       /* the patched file, kept for RetroAchievements' hash (the site knows a patched game by its own) */
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
		ra_img.swap(out);
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
	if (!linked() && pk::Ra::has_account(root_.c_str())) {
		if (ra_img.empty() && !file_read(rom_, ra_img, 12u << 20)) ra_img.clear();
		if (!ra_img.empty()) ra_begin(ra_img.data(), ra_img.size());
		std::vector<uint8_t>().swap(ra_img);
	}
	if (req_.link.active()) {
		/* the session's identity of the game: the bytes the core runs. The file itself when it is those bytes, else a
		 * copy (a patched game, a game with a copier header: two consoles with the same game must agree) */
		std::vector<uint8_t> raw, run;
		bool have = false;
		if (req_.patch_path.empty()) have = file_read(rom_, raw, 12u << 20);
		else {
			std::vector<uint8_t> romb;
			PatchResult pr;
			if (file_read(rom_, romb, 12u << 20)) { pr = patch_apply_files(req_.patch_path, romb, raw); have = pr.status == PS_OK; }
		}
		if (have) {
			size_t off = (raw.size() - (raw.size() / 0x2000) * 0x2000 == 512) ? 512 : 0;
			run.assign(raw.begin() + (long) off, raw.end());
			std::vector<uint8_t> orig;
			if (req_.patch_path.empty() && off == 0) link_rom_ = rom_;
			else {
				make_dirs(path_join(root, "launch"));
				std::string p = path_join(path_join(root, "launch"), "link_rom.bin");
				if (atomic_write(p, run.data(), run.size()) == W_OK) link_rom_ = p;
			}
		}
		logf("link: role=%s mode=%s identity file %s (%s)\n", req_.link.role.c_str(), req_.link.mode.c_str(), link_rom_.c_str(),
		     link_rom_ == rom_ ? "the game file" : "a copy of the image the core runs");
		if (link_rom_.empty()) {
			res.fatal1 = "The two-player link could not start.";
			res.fatal2 = "The game file could not be read again.";
			return res;
		}
	}
	{
		/* a cheat code pokes the machine on one console only: none is loaded in a linked game */
		int nc = req_.link.active() ? 0 : snes_host_cheats_load(root.c_str());
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
	look_apply(0);   // eff_ from ps_ (a fixed Light look has the blends off from the start)
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
	           CAP_WIDE | CAP_PIC_PER_GAME | CAP_CONTROLS | CAP_TURBO | CAP_CHEATS | CAP_CHEAT_EDIT | CAP_CHEAT_SEARCH | CAP_FF | CAP_FF_SOUND | CAP_FF_MODE | CAP_REWIND;
	cfg.enter_is_cross = req_.enter_is_cross;
	cfg.players = 4;
	cfg.play_together_reason = "Start it from the game list: Settings > Two players";
	cfg.accent = 0x7B6CFF;
	menu_.configure(cfg, this, states_);
	menu_.attach_input(&mapper_, store_, &model_snes(), &profile_);
	menu_.set_choices_allowed(ROW_PIC_SHAPE, (1u << 0) | (1u << 2) | (1u << 3) | (1u << 4));   /* one "Fit" (index 0, the main app's word): it already fills the height */
	menu_.set_choices_allowed(ROW_PIC_QUALITY, ((1u << IMGQ_COUNT) - 1) & ~(1u << IMGQ_ENHANCED_4X));   /* no 4x scaler in a player */
	{
		std::vector<std::string> px, onoff, mt, st, filt;
		px.push_back("8:7 (console)"); px.push_back("Square");
		filt.push_back(kVideoFilterNames[0]); filt.push_back(kVideoFilterNames[1]);
		onoff.push_back("Off"); onoff.push_back("On");
		mt.push_back("Auto"); mt.push_back("On"); mt.push_back("Off");
		menu_.add_host_row(Menu::PAGE_PICTURE, ROW_PIXELS, "Pixel shape",
		                   "Pixel perfect only: the console's slightly wide pixels (8:7), or square ones.", px, 0, ROW_PIC_QUALITY);
		menu_.add_host_row(Menu::PAGE_PICTURE, ROW_FILTER, "Video filter", PK_VIDEO_FILTER_HELP, filt, ps_.quality == PIC_SMOOTH ? 1 : 0, ROW_PIC_SCANLINES);
		menu_.add_host_row(Menu::PAGE_PICTURE, ROW_HIRES, "Blend hi-res columns",
		                   "Averages pairs of columns in 512-wide pictures, smoothing their dithered see-through effects. Softens text.", onoff, 0, ROW_PIC_APPLY);
		menu_.add_host_row(Menu::PAGE_PICTURE, ROW_INTERLACE, "Blend interlace lines",
		                   "Averages pairs of lines in interlaced pictures, which removes their flicker.", onoff, 0, ROW_PIC_APPLY);
		menu_.add_host_row(Menu::PAGE_CONTROLS, ROW_MULTITAP, "Multitap",
		                   "The Super Multitap for players 3 and 4. Auto turns it on for games known to use it.", mt, 0, ROW_CTL_MAPPING);
		menu_.add_host_row(Menu::PAGE_SPEED, ROW_STEADY, "Steady pacing",
		                   "When a scene is too heavy for every frame, every second frame is shown, evenly. The game itself keeps full speed.",
		                   onoff, 1);
		std::vector<std::string> palv;
		palv.push_back("Smooth 50 Hz"); palv.push_back("True 50 Hz"); palv.push_back("Fast 60 Hz");
		{
			std::vector<std::string> rl;
			rl.push_back(kRewindLengthNames[0]); rl.push_back(kRewindLengthNames[1]);
			menu_.add_host_row(Menu::PAGE_SPEED, ROW_REWIND_LEN, "Rewind length", PK_REWIND_LENGTH_HELP, rl, rewind_len_, ROW_SPD_REWIND);
		}
		menu_.add_host_row(Menu::PAGE_SPEED, ROW_PALSPEED, "PAL games speed",
		                   "European games run at 50 Hz. Smooth 50 Hz plays them at their real speed and evens out the picture: one refresh in six shows a mix of two frames. True 50 Hz shows one frame in six again; Fast 60 Hz runs them 20 % fast, as on a 60 Hz screen.",
		                   palv, pk::pal_row_of(pk::PAL_SMOOTH));
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
	speed_.set_host(&glue_);
	speed_start();
	ra_update();
	states_->resume_mark(pf_->now_us() / 1000);
	res.ok = true;
	return res;
}

void SnesPlayer::apply_start_mode()
{
	if (linked()) return;      /* a linked game starts from power-on and from the host's save, never from a resume point or slot */
	if ((req_.start == START_CONTINUE || req_.start == START_SLOT) && ra_.hardcore_blocks()) {
		notice("Hardcore mode: the game starts from the beginning", NOTICE_INFO);
		logf("hardcore: the start from a saved state is refused\n");
		return;
	}
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
	speed_.input(in_.fired);
	if (in_.menu || (tapped && req_.tap_menu)) return true;   /* the chords always open it; a tap only when the app's "Tap screen for menu" is on */
	if (linked()) {
		/* the session sets both pads of its one machine; this console's player is pad 1 only, no quick save or load */
		link_pad_ = in_.pad[0] & 0xFFFu;
		session_.tick_running(now_ms);
		return false;
	}
	for (int i = 0; i < MAX_PLAYERS; i++) snes_host_set_pad(i, in_.pad[i]);
	session_.tick_running(now_ms);
	if (in_.fired & FIRE_QSAVE) quick_save();
	if (in_.fired & FIRE_QLOAD) quick_load();
	return false;
}

/* ---- fast forward and rewind ---------------------------------------------------------------------------------------------- */

size_t SnesPlayer::SpeedGlue::state_size() { return snes_host_rewind_state_size(); }
bool SnesPlayer::SpeedGlue::save_state(uint8_t *buf, size_t cap) { size_t len = 0; return snes_host_rewind_state_save_buf(buf, cap, &len) == 0 && len == cap; }
bool SnesPlayer::SpeedGlue::load_state(const uint8_t *buf, size_t n) { p->srm_recheck_ = true; return snes_host_state_load_buf(buf, n) == 0; }
void SnesPlayer::SpeedGlue::run_frame(bool render) { snes_host_set_render(render ? 1 : 0); snes_host_run_frame(); }
void SnesPlayer::SpeedGlue::clear_pads() { for (int i = 0; i < 5; i++) snes_host_set_pad(i, 0); }
uint64_t SnesPlayer::SpeedGlue::now_us() { return p->pf_->now_us(); }
void SnesPlayer::SpeedGlue::wake(void *self) { ((SpeedGlue *) self)->p->pf_->wake_writer(); }

SpeedSettings SnesPlayer::speed_settings() const
{
	SpeedSettings s;
	s.rewind_on = rewind_on_ != 0;
	s.rewind_seconds = rewind_len_ ? 60 : 30;
	s.rewind_budget = (size_t) rewind_mb_ << 20;
	s.ff_factor = ff_speed_ == FFSPD_3X ? 3 : 2;
	s.ff_unlimited = ff_speed_ == FFSPD_UNLIMITED ? 1 : ff_speed_ == FFSPD_SMOOTH ? 2 : 0;
	s.ff_sound = ff_sound_;
	s.ff_toggle = ff_toggle_ != 0;
	return s;
}

void SnesPlayer::speed_start()
{
	/* a linked game steps both machines in lockstep: neither rewind nor fast forward */
	if (linked()) speed_.set_locked(true, "Rewind is off while games are linked");
	speed_.start(snes_host_is_pal() ? 50.0 : 60.0988, 4, speed_settings());
	speed_.rewind().set_async(pf_->wake_writer(), &SpeedGlue::wake, &glue_);
	logf("%s (state %u bytes, %d s, %d MB)\n", speed_.stats_line().c_str(), (unsigned) snes_host_state_size(), rewind_len_ ? 60 : 30, rewind_mb_);
}

/* ---- RetroAchievements ----------------------------------------------------------------------------------------------------- */

uint32_t SnesPlayer::ra_read(void *, uint32_t addr, uint8_t *out, uint32_t n) { return (uint32_t) snes_host_ra_read(addr, out, n); }
uint64_t SnesPlayer::ra_now(void *ud) { return ((SnesPlayer *) ud)->pf_->now_us(); }
int SnesPlayer::ra_online(void *) { return pk_ra_net_online(); }
void SnesPlayer::ra_log(void *ud, const char *line) { ((SnesPlayer *) ud)->logf("ra: %s\n", line); }

void SnesPlayer::ra_begin(const uint8_t *rom, size_t n)
{
	pk::RaHost h;
	h.read = &SnesPlayer::ra_read;
	h.now_us = &SnesPlayer::ra_now;
	h.online = &SnesPlayer::ra_online;
	h.log = &SnesPlayer::ra_log;
	h.ud = this;
	if (!ra_.boot(root_.c_str(), ra_server_.empty() ? 0 : ra_server_.c_str(), "2.0", h)) return;       /* no account on file: nothing runs */
	ra_.game_begin(rom, n, pk::RA_SNES);
}

void SnesPlayer::ra_pump() { if (ra_.up()) ra_.pump(); }

/* Hardcore turns up (or goes away: the game has no achievements) a moment after the game started: rewind and the cheats follow it */
void SnesPlayer::ra_update()
{
	const bool b = ra_.hardcore_blocks();
	if (b == ra_blocks_) return;
	ra_blocks_ = b;
	speed_.set_rewind_locked(b, PK_RA_NO_REWIND);
	snes_host_cheats_hold(b ? 1 : 0);
	logf("hardcore: %s\n", b ? "in force: no rewind, no cheats, no state loads" : "not in force");
}

const char *SnesPlayer::state_load_refused() { return ra_.hardcore_blocks() ? PK_RA_NO_STATE : 0; }
const char *SnesPlayer::cheats_refused() { return ra_.hardcore_blocks() ? PK_RA_NO_CHEATS : 0; }

int SnesPlayer::speed_plan(int normal)
{
	int n = speed_.plan(normal);
	const bool b = speed_.busy();
	if (b != speed_was_busy_) {
		/* the pacing and the look governor start afresh at both ends: what they measured is not what a normal frame costs */
		pacer_.reset(PACE_FULL);
		look_.reset_window();
		speed_.take_ended();
		speed_was_busy_ = b;
	}
	return n;
}

bool SnesPlayer::run_frame(bool render)
{
	const bool landing = speed_.rewinding();
	const bool ran = speed_.run_frame(render);
	if (ran) {
		if (landing) ra_.state_loaded();      /* a rewound game is another timeline: progress starts over */
		else ra_.frame(linked());             /* two players: nothing is processed */
	}
	return ran;
}

size_t SnesPlayer::audio_filter(int16_t *lr, size_t frames) { return speed_.audio(lr, frames); }

bool SnesPlayer::want_render(uint32_t frame) const
{
	return !steady_ || pacer_.mode() == PACE_FULL || (frame & 1) == 0;
}

void SnesPlayer::drawn_cost(uint32_t cost_us)
{
	if (speed_.busy()) return;           /* fast forward and rewind cost a multiple of a frame: the pacer would only back off */
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
	if (const char *why = state_load_refused()) { notice(why, NOTICE_ERROR); return; }
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
	{
		/* what fast forward and rewind have to say, and their chip (">> 2x", "<< REWIND 12 s") kept on screen while they last */
		std::string n = speed_.take_notice();
		if (!n.empty()) notice(n, NOTICE_INFO);
		ra_pump();
		ra_update();
		if (now_ms_ >= ra_toast_until_) {
			std::string t;
			NoticeKind k;
			if (ra_.take_toast(&t, &k)) { notice(t, k); ra_toast_until_ = now_ms_ + (k == NOTICE_ERROR ? 5000 : 2200); }
		}
		std::string c = speed_.chip();
		if (c.empty() && now_ms_ >= ra_toast_until_) c = ra_.indicator();       /* the achievement indicators: the least urgent line */
		if (!c.empty() && now_ms_ >= ra_toast_until_) {
			if (c != chip_text_ || now_ms_ - chip_at_ms_ >= 400) { chip_text_ = c; chip_at_ms_ = now_ms_; menu_.notices().push(c, NOTICE_INFO, 700); }
		} else chip_text_.clear();
	}
	poll_writer();
	srm_watch(now_us);
	wide_watch(now_us);
	/* the resume point: every 2 minutes, and soon after the game saved (at most every 30 s) */
	if (!resume_busy_ && !linked()) {
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
	if (linked()) return;      /* the machine holds the host's game: nothing of it is this console's save (the host's is committed by the session) */
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
	if (linked()) { session_.pause(); return; }
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
	look_save(false);
	look_.reset_window();
}

void SnesPlayer::menu_drawn()
{
	if (menu_srm_pending_) { menu_srm_pending_ = false; if (srm_post()) { srm_dirty_since_ = 0; srm_last_write_ = pf_->now_us(); } }
}

MenuEvent SnesPlayer::menu_update(const MenuInput &in)
{
	MenuEvent ev = menu_.update(in);
	if (qsync_) { qsync_ = false; sync_menu_values(); }               /* Image quality and Video filter follow each other (never inside the row's own change) */
	ra_pump();
	poll_writer();
	if (ev == MENU_RESUME || ev == MENU_RESET_DONE) {
		mapper_.menu_closed(in.pads);
		if (settings_dirty_) settings_queue_save();
		snes_host_set_multitap(multitap_wanted());
		srm_recheck_ = true;
		pacer_.reset(PACE_FULL);
		look_.reset_window();
		look_safe_point(LOOK_SAFE_MENU);
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
	pl.set_int("pal_speed", pal_mode_);
	pl.set_int("look_mode", look_mode_);
	pl.set_int("ff_speed", ff_speed_);
	pl.set_int("ff_sound", ff_sound_);
	pl.set_int("ff_mode", ff_toggle_);
	pl.set_int("rewind", rewind_on_);
	pl.set_int("rewind_seconds", rewind_len_ ? 60 : 30);
	pl.set_int("rewind_mb", rewind_mb_);
	j->text = pl.serialize();
	post(j);
}

/* ---- MenuHost ------------------------------------------------------------------------ */

void SnesPlayer::on_change(int row, int v)
{
	switch (row) {
	case ROW_PIC_SHAPE: ps_.shape = shape_to_pic(v); g_changed |= CH_SHAPE; break;
	case ROW_PIC_QUALITY:
		ps_.scaler = scaler_of(v); look_mode_ = 0; g_changed |= CH_SCALER; qsync_ = true;
		/* Original is stored as sharp, so the next start does not read "no scaler + Smooth" (an old file) and move it to Auto */
		if (v == IMGQ_ORIGINAL && ps_.quality != PIC_SHARP) { ps_.quality = PIC_SHARP; g_changed |= CH_QUALITY; }
		break;
	case ROW_FILTER:
		ps_.quality = v ? PIC_SMOOTH : PIC_SHARP; g_changed |= CH_QUALITY;
		/* as in the main app: Original draws sharp bilinear whatever is stored, so choosing bilinear there switches the quality to Auto */
		if (v && imgq_of(ps_.scaler, look_mode_) == IMGQ_ORIGINAL) { ps_.scaler = SCALER_AUTO; look_mode_ = 0; g_changed |= CH_SCALER; }
		qsync_ = true;
		break;
	case ROW_PIC_SCANLINES: ps_.scan = clampi(v, 0, 10); g_changed |= CH_SCAN; break;
	case ROW_PIC_GRID: ps_.grid = clampi(v, 0, 10); g_changed |= CH_GRID; break;
	case ROW_PIC_GLOW: ps_.glow = clampi(v, 0, 10); g_changed |= CH_GLOW; break;
	case ROW_PIC_APPLY:
		ps_.use_game = v ? 1 : 0;
		/* "This game" stores every value as now shown; "All games" only what was changed here */
		g_changed |= CH_USEGAME | (v ? 0xFFF : 0);
		break;
	case ROW_PIXELS: ps_.pixel_sq = v ? 1 : 0; g_changed |= CH_PIXEL; break;
	case ROW_HIRES: ps_.hires_blend = v ? 1 : 0; g_changed |= CH_HIRES; break;
	case ROW_INTERLACE: ps_.interlace_blend = v ? 1 : 0; g_changed |= CH_ILACE; break;
	case ROW_MULTITAP: ps_.multitap = clampi(v, 0, 2); g_changed |= CH_MULTITAP; snes_host_set_multitap(multitap_wanted()); break;
	case ROW_PIC_WIDE: ps_.wide = clampi(v, 0, 2); g_changed |= CH_WIDE; break;
	case ROW_SCALER: ps_.scaler = clampi(v, 0, SCALER_CHOICES - 1); g_changed |= CH_SCALER; qsync_ = true; break;     /* no row any more: the launch file and the tests */
	case ROW_STEADY: steady_ = v != 0; pacer_.set_enabled(steady_); break;
	case ROW_HANDOFF: handoff_ = v != 0; break;
	case ROW_PALSPEED: pal_mode_ = pk::pal_mode_of(v); break;
	case ROW_LOOK: look_mode_ = clampi(v, 0, 2); qsync_ = true; break;       /* no row any more: the launch file and the tests */
	case ROW_SPD_FF: ff_speed_ = clampi(v, 0, FFSPD_COUNT - 1); speed_.apply(speed_settings()); break;
	case ROW_SPD_SOUND: ff_sound_ = v < 0 ? 0 : v > 2 ? 2 : v; speed_.apply(speed_settings()); break;
	case ROW_SPD_MODE: ff_toggle_ = v == 1 ? 1 : 0; speed_.apply(speed_settings()); break;
	case ROW_SPD_REWIND: rewind_on_ = v != 0; speed_.apply(speed_settings()); logf("%s\n", speed_.stats_line().c_str()); break;
	case ROW_REWIND_LEN: rewind_len_ = v == 1 ? 1 : 0; speed_.apply(speed_settings()); break;
	default: return;
	}
	settings_dirty_ = true;
	if (qsync_ && !menu_.is_open()) { qsync_ = false; sync_menu_values(); }
	look_sync_rows();
	pf_->picture_changed();
}

void SnesPlayer::on_reset()
{
	snes_host_reset();
	ra_.reset();
	speed_.state_changed();
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
	if (ra_.hardcore_blocks()) return false;
	int r = snes_host_state_load_buf(data, n);
	ra_.state_loaded();
	srm_recheck_ = true;
	pacer_.reset(PACE_FULL);
	speed_.state_changed();              /* "a moment ago" is another timeline now */
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

bool SnesPlayer::search_setup(pk_cs *cs)
{
	/* the work RAM (RetroAchievements' 0-1FFFF = $7E0000-$7FFFFF): what a typed 7E.... code reaches */
	static const pk_cs_region reg[1] = { { 0, 0x20000 } };
	pk_cs_init(cs, &SnesPlayer::ra_read, this, PK_CS_SNES, PK_CS_LE, reg, 1);
	return true;
}

const char *SnesPlayer::search_refused()
{
	return linked() ? "Cheats are off while linked to another console." : cheats_refused();
}

void SnesPlayer::add_cheat_code(const std::string &code, const std::string &name) { cheat_entered(true, code, name); }

void SnesPlayer::ask_search_value(const std::string &title)
{
	search_ask_ = true;
	search_title_ = title;
	pf_->ask_cheat_text();
}


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
	bool want = m > 0 && eff_.wide != 2;
	/* Owner decision (1.4): the wide view is on by default and never switched off to save time (Auto used to drop it while
	 * the steady pacer thinned frames). What it costs is held down by degrading its resolution (snes/frontend/wide.*, rungs
	 * to be added to kLookLadder), and by the blends the look governor gives up first. */
	if (!want) m = 0;
	/* Wide view Auto = per side Auto + the cost ladder, On = the full margin always (no ladder); Off is snes_wide_set(0, 0) below.
	 * threads.ini wide_policy 0 turns the ladder off for Auto (the A/B). Set only when they change (they may take the adapter's lock). */
	{
		const int mode = eff_.wide == 1 ? 1 : 0;
		if (mode != wide_mode_set_) { snes_wide_option("mode", mode); wide_mode_set_ = mode; wide_policy_set_ = -1; }
		const int policy = mode == 1 ? 0 : wide_policy_ini_;
		if (policy != wide_policy_set_) { snes_wide_option("policy", policy); wide_policy_set_ = policy; }
	}
	if (want == wide_on_ && m == wide_m_) return;
	snes_wide_set(want ? 1 : 0, m);
	wide_on_ = want; wide_m_ = m;
	logf("wide view %s margin=%d (mode %d, shape %d)\n", want ? "on" : "off", m, eff_.wide, ps_.shape);
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
	if (wide_on_ && now_us - wide_log_us_ >= 1000000u) {   /* the per-second census line */
		wide_log_us_ = now_us;
		snes_wide_census c; snes_wide_census_get(&c);
		logf("wide: frames %llu wide %llu ambient %llu xfade %llu fallback %llu (nothing %llu, off %llu, setup %llu) switches %llu scenes %llu cover %d%% %s cells %u\n",
		     (unsigned long long) c.frames, (unsigned long long) c.frames_wide, (unsigned long long) c.frames_ambient, (unsigned long long) c.frames_crossfade,
		     (unsigned long long) c.frames_fallback, (unsigned long long) c.fb_nothing_drawn, (unsigned long long) c.fb_profile_off, (unsigned long long) c.fb_setup,
		     (unsigned long long) c.auto_switches, (unsigned long long) c.scenes, (int) c.coverage_pct, c.auto_wide ? "wide" : "ambient", (unsigned) c.map_cells);
		/* the cost ladder (snes/frontend/wide.h): rung 0 full margin .. 3 floor, the smoothed compose time, frames each side showed the ambient frame */
		logf("wide ladder: rung=%d compose_us=%u capture_us=%u steps=%u side_amb_frames L=%u R=%u chip=%d\n", c.rung, (unsigned) c.compose_us, (unsigned) c.capture_us, (unsigned) c.rung_changes,
		     (unsigned) c.frames_side_amb[0], (unsigned) c.frames_side_amb[1], c.chip);
	}
	if (!wide_on_ && !snes_wide_map_dirty()) return;
	if (now_us - wide_last_us_ < 30u * 1000000u) return;
	wide_last_us_ = now_us;
	wide_post(false);
}

/* ---- look governor ------------------------------------------------------------------ */

/* The ladder (docs/DEFAULTS-1.4.md section 5, Super NES: no xBRZ), least visible first. The row ids are the menu's.
 * Owner decision: a feature is never switched off for a game, so the wide view is NOT a rung; it will be degraded in
 * resolution (narrower margin, lower resolution side strips, 30 Hz strips) by rungs appended here once snes/frontend/wide.*
 * offers them. To add one: a line here, a LOOK_* id in player.h, and its effect in look_apply(). */
static const LookRung kLookLadder[] = {
	{ "xBR 2x -> sharp", SnesPlayer::ROW_SCALER, 0.20f, 25 },
	{ "interlace blend off", SnesPlayer::ROW_INTERLACE, 0.04f, 30 },
	{ "hi-res blend off", SnesPlayer::ROW_HIRES, 0.04f, 45 },
};
enum { kLookRungs = sizeof kLookLadder / sizeof kLookLadder[0] };

/* a row on its default is the governor's; a row set to something else is the player's, by hand */
void SnesPlayer::look_sync_rows()
{
	if (look_on_) {
		/* a fixed look (Full / Light) is the player's choice: the governor touches nothing */
		const bool fixed = look_mode_ != 0;
		/* an Image quality chosen by hand is the player's (the scaler row); the blends stay the governor's, as before the Image quality row */
		look_.set_user_row(ROW_SCALER, fixed || ps_.scaler != SCALER_AUTO);
		look_.set_user_row(ROW_INTERLACE, fixed || ps_.interlace_blend == 0);
		look_.set_user_row(ROW_HIRES, fixed || ps_.hires_blend == 0);
	}

	look_apply(0);
}

void SnesPlayer::look_apply(const char *why)
{
	/* Built in a local and stored int by int, only what differs: this runs on the game thread during play while the drawing thread
	 * reads picture() (it takes a copy per picture), so eff_ is never rewritten as a whole and nothing is stored that did not change. */
	pic_settings n = ps_;
	n.tier_steps = 0;
	const bool light = light_on(look_mode_, ps_.scaler);
	if (light) { n.interlace_blend = 0; n.hires_blend = 0; n.scaler = SCALER_OFF; }   /* Light: scaler and blends off, fixed */
	if (n.scaler == SCALER_OFF) {
		n.quality = PIC_SHARP;               /* Original draws sharp bilinear whatever Video filter holds (as in the main app) */
	}
	if (!light && look_on_) {
		if (look_.applied(LOOK_SCALER)) n.tier_steps = 1;
		if (look_.applied(LOOK_ILACE)) n.interlace_blend = 0;
		if (look_.applied(LOOK_HIRES)) n.hires_blend = 0;
	}
	look_gen_ = look_.generation();
	int *dst = (int *) &eff_;
	const int *src = (const int *) &n;
	bool changed = false;
	for (size_t i = 0; i < sizeof n / sizeof(int); i++)
		if (dst[i] != src[i]) { dst[i] = src[i]; changed = true; }
	if (!changed) return;
	pf_->picture_changed();
	if (why) {
		logf("look: %s -> mask=%u (scaler steps %d, row %d, interlace blend %d, hi-res blend %d)\n", why, look_.mask(), eff_.tier_steps, eff_.scaler, eff_.interlace_blend, eff_.hires_blend);
		/* no toast at launch, where only a remembered look is applied */
		if (strcmp(why, "start") != 0 && look_.level() > 0) notice("Look eased to keep full speed", NOTICE_INFO);
	}
}

void SnesPlayer::look_start(int mhz)
{
	look_bad_ = false;
	look_mhz_ = mhz;
	look_.set_ladder(kLookLadder, kLookRungs);
	look_on_ = true;
	look_sync_rows();
	look_path_ = path_join(path_join(path_join(root_, "look"), "snes"), path_join(strf("%02x", (unsigned) (crc32(0, game_.data(), game_.size()) >> 24)), game_ + ".look"));
	std::vector<uint8_t> b;
	bool had = false;
	if (file_read(look_path_, b, 4096) && !b.empty()) {
		had = look_.load_memory((const char *) b.data(), b.size(), mhz);
		if (had) look_written_.assign((const char *) b.data(), b.size());
	}
	look_.start();
	look_apply("start");
	logf("look: governor on, memory %s, mask=%u\n", had ? "loaded" : "none", look_.mask());
}

void SnesPlayer::look_second(const LookSecond &s)
{
	if (!look_on_) return;
	if (speed_.busy() || speed_was_busy_) { look_.reset_window(); return; }     /* fast forward / rewind seconds are no measure of the look */
	look_bad_ = pk::LookGovernor::load_of(s) >= look_.config().bad_load;
	/* the wide view's ladder takes the player's trouble once per second (it is consumed by the next composed frame): 1 = step down, 2 = to the
	 * floor when the steady pacer has been thinning frames for 3 seconds in a row */
	pace_half_secs_ = pacer_.mode() == PACE_HALF ? pace_half_secs_ + 1 : 0;
	if (pacer_.mode() != PACE_FULL || look_bad_) snes_wide_option("pressure", pace_half_secs_ >= 3 ? 2 : 1);
	const bool changed = look_.second(s);
	char line[400];
	if (look_.census_line(line, sizeof line)) logf("%s\n", line);
	if (changed || look_.generation() != look_gen_) look_apply("second");
}

void SnesPlayer::look_safe_point(LookSafe k)
{
	if (!look_on_) return;
	if (look_.safe_point(k) || look_.generation() != look_gen_) look_apply("safe point");
}

/* the game's memory, on the writer thread, only when it differs from what is on the card */
void SnesPlayer::look_save(bool final_)
{
	if (!look_on_ || look_path_.empty()) return;
	if (look_busy_) { if (!final_) return; wait_writer(); poll_writer(); }
	LookMemAction a = look_.memory_action();
	WriterJob *j = new WriterJob;
	j->kind = WriterJob::LOOKMEM;
	j->path = look_path_;
	if (a == LOOK_MEM_WRITE) {
		j->text = look_.serialize(look_mhz_);
		if (j->text == look_written_) { delete j; return; }
		look_written_ = j->text;
	} else if (a == LOOK_MEM_DELETE) {
		if (look_written_.empty()) { delete j; return; }
		j->remove_file = true;
		look_written_.clear();
	} else { delete j; return; }
	look_busy_ = true;
	post(j);
	if (final_) { wait_writer(); poll_writer(); }
}

void SnesPlayer::finish(ExitReason why)
{
	speed_.stop();
	wait_writer();
	wide_post(true);
	if (look_on_) {
		char sum[600];
		snprintf(sum, sizeof sum, "%s", look_.summary().c_str());
		for (char *c = sum; *c; c++) if (*c == '\n') *c = ' ';
		logf("%s\n", sum);
		look_save(true);
	}
	if (settings_dirty_) settings_queue_save();
	/* the resume point: where the game was left */
	/* A linked game writes none, except the host after an agreed end: its battery file was just written by the session, and
	 * an older resume point would bring the older battery back on the next Continue (a state carries the battery RAM, and
	 * restore_state has it written to the file). The machine is at the session's last frame, so that is the point to keep. */
	if (states_ && snes_host_state_size() && (!linked() || (req_.link.is_host() && link_result_ == "done"))) resume_post(true);
	wait_writer();
	if (!linked() && snes_host_sram_dirty()) {
		int r = snes_host_sram_save();
		logf("final sram save rc=%d\n", r);
		srm_result(r, "final", pf_->now_us());
	}
	if (states_) session_.set_resume_present(states_->resume_present());
	SessionReport rep = session_.finish(why);
	rep.link_result = link_result_;
	rep.link_error = link_err_;
	rep.link_error_name = link_err_name_;
	rep.link_frames = link_frames_;
	rep.link_peer_reason = link_peer_reason_;
	WriteResult w = session_write(root_, rep);
	logf("session: %lld s played, save_changed=%d save_error=%d states=%d resume=%d exit=%d -> launch/session.txt %s\n",
	     (long long) rep.seconds_played, rep.save_changed ? 1 : 0, rep.save_error ? 1 : 0, (int) rep.states_touched.size(),
	     rep.resume_present ? 1 : 0, (int) why, w == W_OK ? "written" : write_result_text(w));
	ra_.shutdown();                  /* an unlock earned a moment ago gets up to three seconds to reach the server */
	log_flush(true);
}

}  // namespace snesp
