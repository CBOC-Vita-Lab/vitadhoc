/* See player.h. Copyright CBOC-Vita. MIT licence (see md/frontend/LICENSE). */
#include "player.h"
#include "pk_patch.h"
#include "pk_util.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <algorithm>

namespace mdp {

using namespace pk;

/* ---- the writer queue ------------------------------------------------------ */

void Writer::post(WriterJob *j)
{
	SpinGuard g(lk_);
	todo_.push_back(j);
}

bool Writer::work_one(MdPlayer *p)
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

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* menu shape (Original, Fit, Zoom, Full screen, Pixel perfect) <-> the picture's shapes */
static int shape_to_pic(int v)
{
	switch (v) { case 2: return PIC_ZOOM; case 3: return PIC_FULL; case 4: return PIC_PIXEL; default: return PIC_ORIGINAL; }
}
static int pic_to_shape(int p)
{
	switch (p) { case PIC_ZOOM: return 2; case PIC_FULL: return 3; case PIC_PIXEL: return 4; default: return 0; }
}

/* fields of pic_settings the player changed in the menu (only these are written back to the files, so a per-game value the app sent in
 * the launch file never leaks into picture.ini) */
enum { CH_SHAPE = 1, CH_PIXEL = 2, CH_QUALITY = 4, CH_GLOW = 8, CH_SCAN = 16, CH_GRID = 32, CH_SCALER = 64, CH_GGCOL = 128, CH_USEGAME = 256 };
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
	if (m & CH_SCALER) dst.scaler = src.scaler;
	if (m & CH_GGCOL) dst.gg_colors = src.gg_colors;
	if (m & CH_USEGAME) dst.use_game = src.use_game;
}

static const char *kConsoles[4] = { "md", "sms", "gg", "sg" };

std::string MdPlayer::console_for_extension(const std::string &path)
{
	if (ends_with_ci(path, ".gg")) return "gg";
	if (ends_with_ci(path, ".sms") || ends_with_ci(path, ".mark3")) return "sms";
	if (ends_with_ci(path, ".sg") || ends_with_ci(path, ".sc") || ends_with_ci(path, ".sg1000")) return "sg";
	return "md";
}

std::string MdPlayer::pick_launch(const std::string &root, const std::string &arg_rom)
{
	std::string best;
	int64_t best_t = -1;
	for (int i = 0; i < 4; i++) {
		uint64_t sz = 0;
		int64_t mt = 0;
		if (!file_exists(launch_path(root, kConsoles[i]), &sz, &mt)) continue;
		LaunchRequest r;
		if (!launch_read(root, kConsoles[i], r) || r.game_path.empty()) continue;
		if (!arg_rom.empty() && r.game_path == arg_rom) return kConsoles[i];
		if (mt > best_t) { best_t = mt; best = kConsoles[i]; }
	}
	return arg_rom.empty() ? best : std::string();
}

/* ---- construction ------------------------------------------------------------- */

MdPlayer::MdPlayer(Platform *pf) : pf_(pf)
{
	pic_defaults(&ps_);
	g_changed = 0;
	pic_defaults(&g_ps_file);
	eff_ = ps_;
}

MdPlayer::~MdPlayer()
{
	menu_.release_textures();
	delete states_;
	delete store_;
}

void MdPlayer::logf(const char *fmt, ...)
{
	char tmp[1024];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
	va_end(ap);
	if (n <= 0) return;
	/* any thread (the writer thread logs too) */
	SpinGuard g(log_lk_);
	if (logbuf_.size() > 256 * 1024) { log_lost_++; return; }     /* the card was busy for very long */
	logbuf_.append(tmp, (size_t) n < sizeof tmp ? (size_t) n : sizeof tmp - 1);
}

/* The log goes to the card on the writer thread, at most every 5 s; `now` writes it at once on this thread (the end). Game thread only. */
void MdPlayer::log_flush(bool now)
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

void MdPlayer::notice(const std::string &t, NoticeKind k, bool report)
{
	menu_.notices().push(t, k);
	if (report) session_.note_notice(t);      /* the app shows it when the player has ended */
	logf("notice%s: %s\n", k == NOTICE_ERROR ? " (error)" : "", t.c_str());
}

/* ---- the writer ----------------------------------------------------------------- */

void MdPlayer::post(WriterJob *j)
{
	writer_.post(j);
	if (!pf_->wake_writer())
		while (writer_.work_one(this)) {}
}

void MdPlayer::wait_writer()
{
	if (writer_.idle()) return;
	while (!writer_.idle()) {
		if (!pf_->wake_writer()) { while (writer_.work_one(this)) {} break; }
		pf_->pause_us(1000);    /* the writer runs at a lower priority on another core */
	}
	poll_writer();
}

std::string MdPlayer::cfg_dir() const { return path_join(root_, console_); }

void MdPlayer::run_writer_job(WriterJob *j)
{
	switch (j->kind) {
	case WriterJob::SRM:
		/* the battery save: written through the safe path, the first replacement of the file this session keeps a verified backup */
		j->result = srm_file_.write(srm_, j->bytes.data(), j->bytes.size());
		if (j->result) j->text = srm_file_.why();
		break;
	case WriterJob::STATE:
	case WriterJob::RESUME:
		j->rep = states_->commit(j->state);
		break;
	case WriterJob::SETTINGS: {
		/* picture.ini / games/<game>.ini: a game file with own_settings=1 wins */
		int r;
		if (j->ps.use_game) r = pic_save(j->path2.c_str(), &j->ps);
		else {
			r = pic_save(j->path.c_str(), &j->ps);
			pic_settings gs = j->ps;
			if (pic_load(j->path2.c_str(), &gs) && gs.use_game) { gs.use_game = 0; pic_save(j->path2.c_str(), &gs); }
		}
		Ini pl;
		pl.parse(j->text);
		WriteResult w = ini_save(path_join(cfg_dir(), "player.ini"), pl);
		j->result = r ? r : (w != W_OK ? 100 + (int) w : 0);
		break;
	}
	case WriterJob::LOG:
		pf_->log_write(j->text);
		break;
	case WriterJob::LOOKMEM:
		if (j->remove_file) { file_remove(j->path); j->result = 0; }
		else {
			make_dirs(path_dir(j->path));
			WriteResult w = atomic_write(j->path, (const uint8_t *) j->text.data(), j->text.size());
			j->result = w == W_OK ? 0 : 100 + (int) w;
		}
		break;
	case WriterJob::CHEATS:
		make_dirs(path_dir(j->path));
		j->result = cht_file_.write(j->path, (const uint8_t *) j->text.data(), j->text.size());
		if (j->result) j->path2 = cht_file_.why();
		break;
	}
}

void MdPlayer::poll_writer()
{
	std::vector<WriterJob *> done;
	writer_.take_done(done);
	for (size_t i = 0; i < done.size(); i++) {
		WriterJob *j = done[i];
		switch (j->kind) {
		case WriterJob::SRM:
			srm_busy_ = false;
			srm_writes_++;
			if (j->result) { srm_err_ = j->text; md_host_sram_mark_unsaved(); }      /* failed: still to be written */
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
		case WriterJob::CHEATS:
			cheats_busy_ = false;
			if (j->result) { notice("Cheat list not saved: " + j->path2 + ". The old file was kept.", NOTICE_ERROR); cheats_.save(cht_file_); }
			logf("cheat list written rc=%d\n", j->result);
			break;
		}
		delete j;
	}
}

/* ---- start ---------------------------------------------------------------------- */

std::string MdPlayer::pic_game_ini() const { return path_join(path_join(cfg_dir(), "games"), game_ + ".ini"); }

void MdPlayer::settings_load()
{
	pic_defaults(&ps_);
	pic_load(path_join(cfg_dir(), "picture.ini").c_str(), &ps_);
	pic_settings gs = ps_;
	if (pic_load(pic_game_ini().c_str(), &gs) && gs.use_game) ps_ = gs;
	g_ps_file = ps_;
	g_changed = 0;
	Ini pl;
	pl.load(path_join(cfg_dir(), "player.ini"));
	steady_ = pl.get_int("steady_pacing", 1) != 0;
	handoff_ = pl.get_int("smooth_handoff", 1) != 0;
	pal_fast_ = pl.get_int("pal_speed", 0) != 0;
	look_mode_ = clampi(pl.get_int("look_mode", 0), 0, 2);
	region_ = clampi(pl.get_int("region", 0), 0, 3);
	fm_ = clampi(pl.get_int("fm", 0), 0, 2);
	ym2413_ = clampi(pl.get_int("ym2413", 2), 0, 2);
	pad_type_ = clampi(pl.get_int("pad_type", 0), 0, 2);
	sms_border_ = clampi(pl.get_int("sms_border", 0), 0, 2);
	gg_lcd_ = clampi(pl.get_int("gg_lcd", 0), 0, 10);
}

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

/* The launch file's choices: the shape and quality the app chose (unless this game has its own picture file from the player's menu),
 * then the app's per-game overrides ovr.<key>. */
void MdPlayer::apply_overrides()
{
	if (!have_launch_) return;
	Ini raw;
	raw.load(launch_path(root_, console_));
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
	if ((v = req_.override_int("scaler", -1)) >= 0) ps_.scaler = clampi(v, 0, SCALER_CHOICES - 1);
	if (req_.overrides.count("scaler")) { int s = word_scaler(req_.overrides.find("scaler")->second); if (s >= 0) ps_.scaler = s; }
	if ((v = req_.override_int("gg_colors", -1)) >= 0) ps_.gg_colors = v ? 1 : 0;
	if ((v = req_.override_int("steady_pacing", -1)) >= 0) steady_ = v != 0;
	if ((v = req_.override_int("smooth_handoff", -1)) >= 0) handoff_ = v != 0;
	if ((v = req_.override_int("region", -1)) >= 0) region_ = clampi(v, 0, 3);
	if (req_.overrides.count("region")) {
		std::string w = lower(req_.overrides.find("region")->second);
		if (w == "auto") region_ = 0; else if (w == "usa") region_ = 1; else if (w == "europe" || w == "pal") region_ = 2; else if (w == "japan") region_ = 3;
	}
	if ((v = req_.override_int("fm", -1)) >= 0) fm_ = clampi(v, 0, 2);
	if ((v = req_.override_int("pad_type", -1)) >= 0) pad_type_ = clampi(v, 0, 2);
	if ((v = req_.override_int("sms_border", -1)) >= 0) sms_border_ = clampi(v, 0, 2);
	/* for the session runner's A/B: pal_speed 0 = true 50 Hz, 1 = fast 60 Hz (or the words); look_mode 0..2 (or auto / full / light) */
	if ((v = req_.override_int("pal_speed", -1)) >= 0) pal_fast_ = v != 0;
	if (req_.overrides.count("pal_speed")) { std::string w = lower(req_.overrides.find("pal_speed")->second); if (w == "fast" || w == "60") pal_fast_ = true; else if (w == "true" || w == "50") pal_fast_ = false; }
	if ((v = req_.override_int("look_mode", -1)) >= 0) look_mode_ = clampi(v, 0, 2);
	if (req_.overrides.count("look_mode")) { std::string w = lower(req_.overrides.find("look_mode")->second); if (w == "auto") look_mode_ = 0; else if (w == "full") look_mode_ = 1; else if (w == "light") look_mode_ = 2; }
}

/* the core's options from what the player holds; the game is loaded */
void MdPlayer::apply_core_options()
{
	md_host_set_region(region_);
	md_host_set_option("fm", fm_);
	md_host_set_option("ym2413", ym2413_);
	md_host_set_option("sms_border", sms_border_);
	md_host_set_option("lcd", gg_lcd_ * 10);
	md_host_set_pad_type(0, pad_type_);
	md_host_set_pad_type(1, pad_type_);
}

void MdPlayer::sync_menu_values()
{
	menu_.set_value(ROW_PIC_SHAPE, pic_to_shape(ps_.shape));
	menu_.set_value(ROW_PIC_QUALITY, ps_.quality == PIC_SMOOTH ? 1 : 0);
	menu_.set_value(ROW_PIC_SCANLINES, ps_.scan);
	menu_.set_value(ROW_PIC_GRID, ps_.grid);
	menu_.set_value(ROW_PIC_GLOW, ps_.glow);
	menu_.set_value(ROW_PIC_APPLY, ps_.use_game ? 1 : 0);
	menu_.set_value(ROW_SCALER, ps_.scaler);
	menu_.set_value(ROW_SMS_BORDER, sms_border_);
	menu_.set_value(ROW_GG_COLORS, ps_.gg_colors ? 1 : 0);
	menu_.set_value(ROW_GG_LCD, gg_lcd_);
	menu_.set_value(ROW_PADTYPE, pad_type_);
	menu_.set_value(ROW_REGION, region_);
	menu_.set_value(ROW_FM, fm_);
	menu_.set_value(ROW_YM2413, ym2413_);
	menu_.set_value(ROW_STEADY, steady_ ? 1 : 0);
	menu_.set_value(ROW_HANDOFF, handoff_ ? 1 : 0);
	menu_.set_value(ROW_PALSPEED, pal_fast_ ? 1 : 0);
	menu_.set_value(ROW_LOOK, look_mode_);
}

StartResult MdPlayer::start(int argc, char **argv, const std::string &root, bool system_enter_cross)
{
	StartResult res;
	init_shared();
	root_ = root;

	/* 1. what to run: the launch file (md, sms, gg or sg: the one that names the argument's game, else the newest), else the argument */
	std::string arg;
	for (int i = argc - 1; i >= 1 && arg.empty(); i--)
		if (argv[i] && argv[i][0] && !ends_with_ci(argv[i], "md.bin") && !starts_with(argv[i], "--")) arg = argv[i];
	std::string arg_rom = arg, arg_srm;
	{ size_t bar = arg_rom.find('|'); if (bar != std::string::npos) { arg_srm = arg_rom.substr(bar + 1); arg_rom.resize(bar); } }
	std::string picked = pick_launch(root, arg_rom);
	have_launch_ = !picked.empty() && launch_read(root, picked, req_) && !req_.game_path.empty();
	if (!have_launch_) {
		if (!picked.empty() && !arg_rom.empty()) logf("launch files do not name %s: the argument wins\n", arg_rom.c_str());
		req_ = LaunchRequest();
		req_.root = root;
		req_.game_path = arg_rom;
		req_.save_path = arg_srm;
		req_.enter_is_cross = system_enter_cross;
		console_ = console_for_extension(arg_rom);
	} else {
		console_ = picked;
		if (req_.newer_version) logf("launch file is from a newer VitAdhoc (version %d): reading what this player knows\n", req_.version);
		for (size_t i = 0; i < req_.warnings.size(); i++) logf("launch file: %s\n", req_.warnings[i].c_str());
	}
	req_.console = console_;
	system_ = console_ == "gg" ? MD_SYS_GG : console_ == "sms" ? MD_SYS_SMS : console_ == "sg" ? MD_SYS_SG : MD_SYS_MD;
	logf("launch: %s, console=%s game=%s save=%s start=%d slot=%d patch=%s cpu=%d enter=%s\n", have_launch_ ? ("launch/" + console_ + ".ini").c_str() : "argument",
	     console_.c_str(), req_.game_path.c_str(), req_.save_path.c_str(), (int) req_.start, req_.slot, req_.patch_path.c_str(), req_.cpu_mhz,
	     req_.enter_is_cross ? "cross" : "circle");
	res.cpu_mhz = req_.cpu_mhz;
	res.enter_is_cross = req_.enter_is_cross;
	res.console = console_;
	make_dirs(path_join(cfg_dir(), "games"));
	make_dirs(path_join(path_join(root, "backups"), console_));
	if (req_.game_path.empty()) {
		res.fatal1 = "No game was given.";
		res.fatal2 = "Start Sega games from VitAdhoc.";
		return res;
	}
	rom_ = req_.game_path;
	srm_ = req_.save_path.empty() ? srm_for(rom_) : req_.save_path;
	game_ = path_stem(rom_);
	session_.begin(req_);
	srm_file_.set_backup_dir(path_join(path_join(root, "backups"), console_));
	cht_file_.set_backup_dir(path_join(path_join(root, "backups"), console_));

	/* 2. the battery save: a cut write is put back, then a newer RetroArch save is brought in */
	recover_old(srm_);
	if (!req_.retroarch_save.empty()) {
		uint64_t ra_sz = 0, sv_sz = 0;
		int64_t ra_t = 0, sv_t = 0;
		if (file_exists(req_.retroarch_save, &ra_sz, &ra_t)) {
			bool have = file_exists(srm_, &sv_sz, &sv_t);
			if (!have || ra_t > sv_t) {
				std::vector<uint8_t> b;
				bool ok = file_read(req_.retroarch_save, b, 1u << 20) && !b.empty();
				if (ok && have) {
					/* the save it replaces is kept in the backup folder first (SafeFile: verified) */
					std::vector<uint8_t> old;
					if (file_read(srm_, old, 1u << 20) && !old.empty()) {
						std::string bak = path_join(path_join(path_join(root, "backups"), console_), game_ + ".before-retroarch.srm");
						ok = atomic_write(bak, old.data(), old.size()) == W_OK;
					}
				}
				if (ok) ok = atomic_write(srm_, b.data(), b.size(), have ? EXISTS_YES : EXISTS_NO) == W_OK;
				if (ok) notice("Brought in the newer RetroArch save", NOTICE_OK, true);
				else notice("Could not bring in the RetroArch save: the old save was kept", NOTICE_ERROR, true);
				logf("retroarch save %s -> %s: %s\n", req_.retroarch_save.c_str(), srm_.c_str(), ok ? "imported" : "FAILED");
			}
		}
	}

	/* 3. the game, patched in memory when the app asked for it (never half-patched) */
	char err[200] = "";
	if (md_host_init()) { res.fatal1 = "The Sega core could not start."; res.fatal2 = "Not enough memory."; return res; }
	std::vector<uint8_t> romb;
	if (!file_read(rom_, romb, 12u << 20) || romb.empty()) { res.fatal1 = "This game could not be started."; res.fatal2 = "The game file could not be read."; return res; }
	if (!req_.patch_path.empty()) {
		std::vector<uint8_t> out;
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
		romb.swap(out);
	}
	if (md_host_load_mem(romb.data(), romb.size(), rom_.c_str(), system_, err, sizeof err)) {
		logf("load failed: %s\n", err);
		res.fatal1 = "This game could not be started.";
		res.fatal2 = err;
		SessionReport r = session_.finish(EXIT_ERROR);
		r.notice = std::string("This game could not be started: ") + err;
		session_write(root_, r);
		return res;
	}
	{
		std::vector<uint8_t>().swap(romb);
		std::vector<uint8_t> sv;
		if (file_exists(srm_) && file_read(srm_, sv, 1u << 20) && !sv.empty()) {
			const size_t used = md_host_sram_write(sv.data(), sv.size());
			logf("battery save loaded: %u bytes of %u (%s)\n", (unsigned) used, (unsigned) sv.size(), srm_.c_str());
		}
	}

	/* 4. states, settings, controls, the menu, cheats */
	StateLayout l;
	l.root = root; l.console = console_; l.name = game_;
	states_ = new States(l);
	states_->ensure_dirs();
	states_->rescan();
	session_.set_resume_present(states_->resume_present());
	settings_load();
	apply_overrides();
	apply_core_options();
	pic_lcd_table();                  /* built here, once, before another thread may draw with it */
	look_apply(0);
	pacer_.set_enabled(steady_);
	const PadModel *model = model_by_name(console_);
	if (!model) model = &model_md();
	store_ = new ProfileStore(root, console_, game_);
	store_->load_effective(*model, req_.enter_is_cross, profile_);
	mapper_.configure(*model, profile_, MapperConfig());
	cheats_.load(path_join(path_join(root, "cheats"), game_ + ".cht"), system_ == MD_SYS_MD);
	{
		auto it = req_.unknown.find("cheats_db");
		int added = it != req_.unknown.end() ? cheats_.merge_file(it->second) : 0;
		if (added) logf("cheats: %d from the database file %s (off until switched on)\n", added, it->second.c_str());
	}
	cheats_apply();
	logf("cheats: %d in the list, %d on\n", cheats_.size(), (int) cheats_.active_parts().size());

	MenuConfig cfg;
	cfg.game_name = req_.game_name.empty() ? game_ : req_.game_name;
	cfg.console_label = system_ == MD_SYS_MD ? "Mega Drive" : system_ == MD_SYS_SMS ? "Master System" : system_ == MD_SYS_GG ? "Game Gear" : "SG-1000";
	cfg.exit_label = "Exit";
	cfg.exit_help = "Close VitAdhoc";
	cfg.caps = CAP_STATES | CAP_UNDO | CAP_RESET | CAP_SHAPE | CAP_QUALITY | CAP_SCANLINES | CAP_LCD_GRID | CAP_GLOW |
	           CAP_PIC_PER_GAME | CAP_CONTROLS | CAP_TURBO | CAP_CHEATS | CAP_CHEAT_EDIT | extra_caps_;
	cfg.enter_is_cross = req_.enter_is_cross;
	cfg.players = model->players;
	cfg.accent = system_ == MD_SYS_MD ? 0x2E7DFF : system_ == MD_SYS_GG ? 0xE0B020 : 0xE04A3A;
	menu_.configure(cfg, this, states_);
	menu_.attach_input(&mapper_, store_, model, &profile_);
	menu_.set_choices_allowed(ROW_PIC_SHAPE, (1u << 0) | (1u << 2) | (1u << 3) | (1u << 4));   /* no "Fit": Original already fills the height */
	menu_.set_row_note(ROW_PIC_WIDE, "The wide view is not available for Sega games in this version");
	{
		std::vector<std::string> px, onoff, sc, border, lcd, pad, region, fm, ym, palv, lookv;
		px.push_back("Console"); px.push_back("Square");
		onoff.push_back("Off"); onoff.push_back("On");
		sc.push_back("Auto"); sc.push_back("Sharp (none)"); sc.push_back("MMPX 2x"); sc.push_back("xBR 2x"); sc.push_back("xBR 3x");
		border.push_back("Off"); border.push_back("Left edge"); border.push_back("Both edges");
		lcd.push_back("Off");
		for (int i = 1; i <= 10; i++) lcd.push_back(strf("%d", i));
		pad.push_back("Auto"); pad.push_back("3 buttons"); pad.push_back("6 buttons");
		region.push_back("Auto"); region.push_back("USA"); region.push_back("Europe (50 Hz)"); region.push_back("Japan");
		fm.push_back("Discrete"); fm.push_back("ASIC"); fm.push_back("Enhanced");
		ym.push_back("Off"); ym.push_back("On"); ym.push_back("Auto");
		palv.push_back("True 50 Hz"); palv.push_back("Fast 60 Hz");
		lookv.push_back("Auto"); lookv.push_back("Full"); lookv.push_back("Light");
		menu_.add_host_row(Menu::PAGE_PICTURE, ROW_SCALER, "Pixel art scaler",
		                   "Redraws the picture at 2x or 3x its size with smooth diagonals before it is fitted to the screen (xBR, MMPX). Auto picks the best that holds full speed.",
		                   sc, ps_.scaler, ROW_PIC_QUALITY);
		if (system_ == MD_SYS_SMS)
			menu_.add_host_row(Menu::PAGE_PICTURE, ROW_SMS_BORDER, "Hide masked edge",
			                   "Games on the Master System cover the first 8 columns with a border colour when they scroll. Hides them, so the picture is 248 wide.",
			                   border, sms_border_, ROW_PIC_APPLY);
		if (system_ == MD_SYS_GG) {
			menu_.add_host_row(Menu::PAGE_PICTURE, ROW_GG_COLORS, "LCD colours",
			                   "The Game Gear's screen showed softer colours and never a true black. On draws them that way.", onoff, ps_.gg_colors ? 1 : 0, ROW_PIC_APPLY);
			menu_.add_host_row(Menu::PAGE_PICTURE, ROW_GG_LCD, "LCD ghosting",
			                   "The slow Game Gear screen left a trail behind moving things. 0 is off, 10 is strong.", lcd, gg_lcd_, ROW_PIC_APPLY);
		}
		menu_.add_host_row(Menu::PAGE_PICTURE, ROW_LOOK, "Image look",
		                   "Auto keeps the best scaler and eases it only when a game cannot hold full speed. Full keeps the scaler as set, Light turns it off, both always.",
		                   lookv, look_mode_, ROW_PIC_APPLY);
		if (system_ == MD_SYS_MD)
			menu_.add_host_row(Menu::PAGE_CONTROLS, ROW_PADTYPE, "Pad type",
			                   "Auto follows the game: 3 buttons or 6. A few games misread the 6 button pad; choose 3 buttons for them.", pad, pad_type_, ROW_CTL_MAPPING);
		menu_.add_host_row(Menu::PAGE_SPEED, ROW_REGION, "Region",
		                   "Auto follows the game. Europe runs a game at its 50 Hz speed, USA and Japan at 60 Hz; some games only work in their own region.", region, region_);
		if (system_ == MD_SYS_MD)
			menu_.add_host_row(Menu::PAGE_SPEED, ROW_FM, "FM sound",
			                   "The sound chip: Discrete is the original YM2612, ASIC the later YM3438, Enhanced adds the missing high notes.", fm, fm_);
		if (system_ == MD_SYS_SMS)
			menu_.add_host_row(Menu::PAGE_SPEED, ROW_YM2413, "FM sound unit",
			                   "The Master System's FM add-on, which some games use. Auto turns it on for games that ask for it.", ym, ym2413_);
		menu_.add_host_row(Menu::PAGE_SPEED, ROW_STEADY, "Steady pacing",
		                   "When a scene is too heavy for every frame, every second frame is shown, evenly. The game itself keeps full speed.", onoff, 1);
		menu_.add_host_row(Menu::PAGE_SPEED, ROW_PALSPEED, "PAL games speed",
		                   "European games run at 50 Hz. True 50 Hz plays them at their real speed (one frame in six is shown again); Fast 60 Hz runs them 20 % fast, as on a 60 Hz screen.",
		                   palv, 0);
		menu_.add_host_row(Menu::PAGE_SPEED, ROW_HANDOFF, "Smooth hand-over",
		                   "Draws each picture on its own thread, so heavy picture options do not slow the game down. Turn it off if the picture ever misbehaves.", onoff, 1);
	}
	sync_menu_values();

	md_video_info vi;
	md_host_video_info(&vi);
	logf("title: %s | %s | %s pal=%d fps=%.4f sram=%u | picture: shape=%d quality=%d glow=%d scan=%d grid=%d scaler=%d own=%d steady=%d handoff=%d\n",
	     md_host_rom_title(), md_host_system_hw(), md_host_rom_info(), md_host_is_pal(), md_host_fps(), (unsigned) md_host_sram_size(), ps_.shape,
	     ps_.quality, ps_.glow, ps_.scan, ps_.grid, ps_.scaler, ps_.use_game, steady_ ? 1 : 0, handoff_ ? 1 : 0);
	states_->resume_mark(pf_->now_us() / 1000);
	res.ok = true;
	return res;
}

void MdPlayer::apply_start_mode()
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

bool MdPlayer::input(const PadSample pads[MAX_PLAYERS], bool tapped, uint64_t now_ms, uint32_t frame)
{
	now_ms_ = now_ms;
	for (int i = 0; i < MAX_PLAYERS; i++) last_pads_[i] = pads[i];
	mapper_.update(pads, now_ms, frame, &in_);
	if (in_.menu || tapped) return true;
	md_host_set_pad(0, in_.pad[0] & 0xFFFu);
	md_host_set_pad(1, system_ == MD_SYS_GG ? 0u : (in_.pad[1] & 0xFFFu));
	session_.tick_running(now_ms);
	if (in_.fired & FIRE_QSAVE) quick_save();
	if (in_.fired & FIRE_QLOAD) quick_load();
	return false;
}

bool MdPlayer::want_render(uint32_t frame) const
{
	return !steady_ || pacer_.mode() == PACE_FULL || (frame & 1) == 0;
}

void MdPlayer::drawn_cost(uint32_t cost_us)
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

std::string MdPlayer::pace_line()
{
	Pacer::Counts c = pacer_.take_counts();
	return strf("pace=%s drawn=%d over=%d max=%.1f changes=%d", pace_mode_name(pacer_.mode()), c.drawn, c.over, c.max_us / 1000.0, c.changes);
}

void MdPlayer::quick_save()
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

void MdPlayer::quick_load()
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

void MdPlayer::resume_post(bool sync)
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

void MdPlayer::after_frame(uint64_t now_us)
{
	int dt = prev_frame_us_ ? (int) ((now_us - prev_frame_us_) / 1000) : 16;
	if (dt < 0 || dt > 1000) dt = 16;
	prev_frame_us_ = now_us;
	menu_.notices().update(dt);
	now_ms_ = now_us / 1000;
	poll_writer();
	srm_watch(now_us);
	/* the resume point: every 2 minutes, and soon after the game saved (at most every 30 s) */
	if (!resume_busy_) {
		bool after_save = resume_after_srm_at_ && now_us >= resume_after_srm_at_ && states_->resume_due(now_ms_, 30000);
		if (after_save || states_->resume_due(now_ms_, 120000)) { resume_after_srm_at_ = 0; resume_post(false); }
	}
	log_flush(false);
}

/* ---- battery RAM ----------------------------------------------------------------- */
/* Written by the writer thread: 2 s after it changed and settled for 1 s, or every 30 s while it keeps changing. Change detection: the
 * core's own dirty flag (a CRC compare, about 0.1 ms) and a 64-bit hash of the bytes, looked at every 8th pass. A failed write leaves the
 * file untouched (SafeFile), keeps the RAM unsaved, says why (red notice, log, session report) and is tried again after 10 s, doubling
 * to 60 s. */

bool MdPlayer::srm_post()
{
	if (srm_busy_) return false;
	const size_t sz = md_host_sram_size();
	if (!sz) return false;
	WriterJob *j = new WriterJob;
	j->kind = WriterJob::SRM;
	j->bytes.resize(sz);
	const size_t n = md_host_sram_read(j->bytes.data(), sz);
	j->bytes.resize(n);
	md_host_sram_mark_saved();          /* what was handed over counts as saved; a failed write marks it unsaved again */
	srm_busy_ = true;
	post(j);
	return true;
}

void MdPlayer::srm_result(int r, const char *how, uint64_t now)
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
	save_msg_ = strf("Could not save the game: %s. The old save was kept; trying again.", srm_err_.empty() ? "unknown error" : srm_err_.c_str());
	logf("sram save FAILED rc=%d (%s): %s - the file on the card was not touched; retry in %u s\n", r, how, srm_err_.c_str(), srm_backoff_s_);
	if (!save_failing_) notice(save_msg_, NOTICE_ERROR);
	session_.note_save_error(save_msg_);
	save_failing_ = true;
	srm_retry_at_ = now + (uint64_t) srm_backoff_s_ * 1000000;
	if (srm_backoff_s_ < 60) srm_backoff_s_ = srm_backoff_s_ * 2 > 60 ? 60 : srm_backoff_s_ * 2;
}

void MdPlayer::srm_watch(uint64_t now)
{
	if ((++srm_pass_ & 7) == 0 || srm_recheck_) {
		const size_t sz = md_host_sram_size();
		const bool recheck = srm_recheck_;
		srm_recheck_ = false;
		if (sz) {
			if (srm_buf_.size() < sz) srm_buf_.resize(sz);
			const size_t n = md_host_sram_read(srm_buf_.data(), srm_buf_.size());
			const uint64_t h = hash64(srm_buf_.data(), n);
			const bool moved = !srm_hash_valid_ || h != srm_hash_;
			if (moved) { srm_hash_ = h; srm_hash_valid_ = true; srm_last_change_ = now; }
			if (moved || recheck) srm_dirty_ = md_host_sram_dirty() != 0;
		} else srm_dirty_ = false;
	}
	if (srm_dirty_) {
		if (!srm_dirty_since_) srm_dirty_since_ = now;
		if (now - srm_dirty_since_ > 2000000 && (now - srm_last_change_ > 1000000 || now - srm_last_write_ > 30000000) &&
		    now >= srm_retry_at_ && srm_post()) {
			srm_dirty_since_ = 0;
			srm_last_write_ = now;
			srm_recheck_ = true;
		}
	} else srm_dirty_since_ = 0;
}

void MdPlayer::suspend()
{
	wait_writer();
	if (md_host_sram_dirty()) {
		const size_t sz = md_host_sram_size();
		std::vector<uint8_t> b(sz);
		b.resize(md_host_sram_read(b.data(), sz));
		const int r = b.empty() ? 0 : srm_file_.write(srm_, b.data(), b.size());
		if (r) srm_err_ = srm_file_.why(); else md_host_sram_mark_saved();
		logf("sram save before suspend rc=%d\n", r);
		srm_result(r, "suspend", pf_->now_us());
	}
	srm_dirty_since_ = 0;
	srm_recheck_ = true;
	session_.pause();
}

/* ---- cheats --------------------------------------------------------------------- */

void MdPlayer::cheats_apply()
{
	md_host_cheat_reset();
	const std::vector<std::string> parts = cheats_.active_parts();
	int refused = 0;
	for (size_t i = 0; i < parts.size(); i++) {
		int n = 1;
		for (size_t k = 0; k < parts[i].size(); k++) if (parts[i][k] == '+') n++;
		if (md_host_cheat_add(parts[i].c_str()) < n) refused++;
	}
	if (refused) logf("cheats: the core refused %d code(s) (table full or an address it does not map)\n", refused);
}

void MdPlayer::cheats_queue_save()
{
	if (cheats_.path().empty() || cheats_.blocked()) return;
	if (cheats_busy_) { wait_writer(); poll_writer(); }
	WriterJob *j = new WriterJob;
	j->kind = WriterJob::CHEATS;
	j->path = cheats_.path();
	j->text = cheats_.text();
	cheats_busy_ = true;
	post(j);
}

void MdPlayer::list_cheats(std::vector<CheatItem> &out)
{
	out.clear();
	for (int i = 0; i < cheats_.size(); i++) {
		CheatItem it;
		it.name = cheats_.at(i).label;
		it.on = cheats_.at(i).on;
		it.available = cheats_.at(i).usable;
		out.push_back(it);
	}
}

void MdPlayer::set_cheat(int index, bool on)
{
	if (index < 0 || index >= cheats_.size() || !cheats_.at(index).usable) { notice("This cheat cannot be switched on here", NOTICE_ERROR); return; }
	cheats_.set_on(index, on);
	cheats_apply();
	cheats_queue_save();
	notice(std::string(on ? "Cheat on: " : "Cheat off: ") + cheats_.at(index).label, on ? NOTICE_OK : NOTICE_INFO);
}

void MdPlayer::add_cheat() { pf_->ask_cheat_text(); }

void MdPlayer::cheat_entered(bool ok, const std::string &code_in, const std::string &name)
{
	std::string code = trim(code_in);
	if (!ok || code.empty()) return;
	const int r = cheats_.add(code, trim(name));
	if (r < 0) {
		const bool md = system_ == MD_SYS_MD;
		const char *t = r == CHEAT_E_FORMAT ? (md ? "That is not a cheat code. Use XXXX-XXXX (Game Genie) or AAAAAA:VVVV (Pro Action Replay)"
		                                          : "That is not a cheat code. Use XXX-XXX (Game Genie), 00AA-AAVV (Pro Action Replay) or AAAA:VV")
		              : r == CHEAT_E_FULL ? "The cheat list is full"
		              : r == CHEAT_E_TOO_LONG ? "That code is too long (16 parts at most)"
		              : r == CHEAT_E_RANGE ? "That code points at an address this game does not have"
		              : r == CHEAT_E_EXISTS ? "That code is already in the list"
		              : "The cheat could not be added";
		notice(t, NOTICE_ERROR);
	} else {
		cheats_apply();
		cheats_queue_save();
		notice("Cheat added and on: " + cheats_.at(r).label, NOTICE_OK);
	}
	logf("cheat add '%s' -> %d\n", code.c_str(), r);
	menu_.refresh();
}

bool MdPlayer::delete_cheat(int index)
{
	if (cheats_.remove(index) != 0) return false;
	cheats_apply();
	cheats_queue_save();
	return true;
}

/* ---- the menu ---------------------------------------------------------------------- */

void MdPlayer::open_menu(int player)
{
	session_.pause();
	mapper_.menu_opened();
	menu_.open(player);
	/* the battery RAM is written once the menu is on screen (menu_drawn), on the writer thread */
	menu_srm_pending_ = md_host_sram_dirty() != 0;
	pacer_.reset(PACE_FULL);
	look_save(false);
	look_.reset_window();
}

void MdPlayer::menu_drawn()
{
	if (menu_srm_pending_) { menu_srm_pending_ = false; if (srm_post()) { srm_dirty_since_ = 0; srm_last_write_ = pf_->now_us(); } }
}

MenuEvent MdPlayer::menu_update(const MenuInput &in)
{
	MenuEvent ev = menu_.update(in);
	poll_writer();
	if (ev == MENU_RESUME || ev == MENU_RESET_DONE) {
		mapper_.menu_closed(in.pads);
		if (settings_dirty_) settings_queue_save();
		srm_recheck_ = true;
		pacer_.reset(PACE_FULL);
		look_.reset_window();
		look_safe_point(LOOK_SAFE_MENU);
		if (ev == MENU_RESET_DONE) notice("The game was reset", NOTICE_INFO);
	}
	return ev;
}

void MdPlayer::settings_queue_save()
{
	settings_dirty_ = false;
	WriterJob *j = new WriterJob;
	j->kind = WriterJob::SETTINGS;
	j->ps = g_ps_file;
	overlay_changed(j->ps, ps_, g_changed);
	j->path = path_join(cfg_dir(), "picture.ini");
	j->path2 = pic_game_ini();
	Ini pl;
	pl.set_int("steady_pacing", steady_ ? 1 : 0);
	pl.set_int("smooth_handoff", handoff_ ? 1 : 0);
	pl.set_int("pal_speed", pal_fast_ ? 1 : 0);
	pl.set_int("look_mode", look_mode_);
	pl.set_int("region", region_);
	pl.set_int("fm", fm_);
	pl.set_int("ym2413", ym2413_);
	pl.set_int("pad_type", pad_type_);
	pl.set_int("sms_border", sms_border_);
	pl.set_int("gg_lcd", gg_lcd_);
	j->text = pl.serialize();
	post(j);
}

/* ---- MenuHost ------------------------------------------------------------------------ */

void MdPlayer::on_change(int row, int v)
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
		g_changed |= CH_USEGAME | (v ? 0xFFu : 0);
		break;
	case ROW_SCALER: ps_.scaler = clampi(v, 0, SCALER_CHOICES - 1); g_changed |= CH_SCALER; break;
	case ROW_GG_COLORS: ps_.gg_colors = v ? 1 : 0; g_changed |= CH_GGCOL; break;
	case ROW_GG_LCD: gg_lcd_ = clampi(v, 0, 10); md_host_set_option("lcd", gg_lcd_ * 10); break;
	case ROW_SMS_BORDER: sms_border_ = clampi(v, 0, 2); md_host_set_option("sms_border", sms_border_); break;
	case ROW_PADTYPE: pad_type_ = clampi(v, 0, 2); md_host_set_pad_type(0, pad_type_); md_host_set_pad_type(1, pad_type_); break;
	case ROW_REGION: {
		const double before = md_host_fps();
		region_ = clampi(v, 0, 3);
		md_host_set_region(region_);
		if (md_host_fps() != before) { timing_changed_ = true; logf("region %d: %.4f Hz now\n", region_, md_host_fps()); }
		break;
	}
	case ROW_FM: fm_ = clampi(v, 0, 2); md_host_set_option("fm", fm_); break;
	case ROW_YM2413: ym2413_ = clampi(v, 0, 2); md_host_set_option("ym2413", ym2413_); break;
	case ROW_STEADY: steady_ = v != 0; pacer_.set_enabled(steady_); break;
	case ROW_HANDOFF: handoff_ = v != 0; break;
	case ROW_PALSPEED: pal_fast_ = v != 0; break;
	case ROW_LOOK: look_mode_ = clampi(v, 0, 2); break;
	default: return;
	}
	settings_dirty_ = true;
	look_sync_rows();
	pf_->picture_changed();
}

void MdPlayer::on_reset()
{
	md_host_reset();
	cheats_apply();
	srm_recheck_ = true;
	logf("reset\n");
}

bool MdPlayer::capture_state(std::vector<uint8_t> &out)
{
	const size_t n = md_host_state_size();
	if (!n) return false;
	out.resize(n);
	const size_t got = md_host_state_save_mem(out.data(), out.size());
	if (!got) { out.clear(); return false; }
	out.resize(got);
	return true;
}

bool MdPlayer::restore_state(const uint8_t *data, size_t n)
{
	const int r = md_host_state_load_mem(data, n);
	srm_recheck_ = true;
	pacer_.reset(PACE_FULL);
	return r == 0;
}

bool MdPlayer::current_frame(Frame *f) { return pf_->current_frame(f); }

int64_t MdPlayer::play_seconds() { return session_.seconds_played(); }

void MdPlayer::on_state_touched(int slot) { session_.note_state_touched(slot); }

void MdPlayer::on_controls_changed(WriteResult rc)
{
	if (rc != W_OK) { logf("controls NOT saved: %s\n", write_result_text(rc)); session_.note_notice(std::string("Could not save the controls: ") + write_result_text(rc)); }
}

bool MdPlayer::queue_state_write(StateJob &job)
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

/* ---- look governor ------------------------------------------------------------------ */

/* The ladder: the pixel art scaler's tiers, least visible first; a system's ladder starts at its ceiling (pic_scaler_ceiling), so a Mega
 * Drive picture has one rung, a Game Gear one three. The savings are guesses from the cost model (md/vita/README.md) that the governor
 * replaces with what it learns. To add a rung: a line here and its effect in look_apply(). */
static const LookRung kLookLadder[] = {
	{ "xBR 3x -> 2x", MdPlayer::ROW_SCALER, 0.10f, 25 },
	{ "xBR 2x -> MMPX 2x", MdPlayer::ROW_SCALER, 0.10f, 35 },
	{ "MMPX 2x -> sharp", MdPlayer::ROW_SCALER, 0.15f, 55 },
};

/* a row on Auto is the governor's; a row set to something else is the player's, by hand */
void MdPlayer::look_sync_rows()
{
	if (look_on_) look_.set_user_row(ROW_SCALER, look_mode_ != 0 || ps_.scaler != SCALER_AUTO);
	look_apply(0);
}

void MdPlayer::look_apply(const char *why)
{
	/* Built in a local and stored int by int, only what differs: this runs on the game thread during play while the drawing thread reads
	 * picture() (it takes a copy per picture), so eff_ is never rewritten as a whole and nothing is stored that did not change. */
	pic_settings n = ps_;
	n.tier_steps = 0;
	if (look_mode_ == 2) n.scaler = SCALER_OFF;                     /* Light: no scaler, fixed */
	else if (look_on_ && look_mode_ == 0) n.tier_steps = look_.level();
	look_gen_ = look_.generation();
	int *dst = (int *) &eff_;
	const int *src = (const int *) &n;
	bool changed = false;
	for (size_t i = 0; i < sizeof n / sizeof(int); i++)
		if (dst[i] != src[i]) { dst[i] = src[i]; changed = true; }
	if (!changed) return;
	pf_->picture_changed();
	if (why) {
		logf("look: %s -> mask=%u (scaler steps %d, row %d)\n", why, look_.mask(), eff_.tier_steps, eff_.scaler);
		if (strcmp(why, "start") != 0 && look_.level() > 0 && look_mode_ == 0) notice("Look eased to keep full speed", NOTICE_INFO);
	}
}

void MdPlayer::look_start(int mhz)
{
	look_bad_ = false;
	look_mhz_ = mhz;
	md_video_info vi;
	md_host_video_info(&vi);
	look_ceiling_ = pic_scaler_ceiling(system_, vi.native_w, vi.native_h);
	const int rungs = 3 - look_ceiling_;
	if (rungs <= 0 || !look_.set_ladder(kLookLadder + look_ceiling_, rungs)) {
		look_on_ = false;
		logf("look: no ladder (picture %dx%d has no scaler tier)\n", vi.native_w, vi.native_h);
		return;
	}
	look_on_ = true;
	look_sync_rows();
	look_path_ = path_join(path_join(path_join(root_, "look"), console_), path_join(strf("%02x", (unsigned) (crc32(0, game_.data(), game_.size()) >> 24)), game_ + ".look"));
	std::vector<uint8_t> b;
	bool had = false;
	if (file_read(look_path_, b, 4096) && !b.empty()) {
		had = look_.load_memory((const char *) b.data(), b.size(), mhz);
		if (had) look_written_.assign((const char *) b.data(), b.size());
	}
	look_.start();
	look_apply("start");
	logf("look: governor on (ceiling tier %d, %d rungs), memory %s, mask=%u\n", look_ceiling_, rungs, had ? "loaded" : "none", look_.mask());
}

void MdPlayer::look_second(const LookSecond &s)
{
	if (!look_on_) return;
	look_bad_ = pk::LookGovernor::load_of(s) >= look_.config().bad_load;
	const bool changed = look_.second(s);
	char line[400];
	if (look_.census_line(line, sizeof line)) logf("%s\n", line);
	if (changed || look_.generation() != look_gen_) look_apply("second");
}

void MdPlayer::look_safe_point(LookSafe k)
{
	if (!look_on_) return;
	if (look_.safe_point(k) || look_.generation() != look_gen_) look_apply("safe point");
}

/* the game's memory, on the writer thread, only when it differs from what is on the card */
void MdPlayer::look_save(bool final_)
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

/* ---- the end ---------------------------------------------------------------------- */

void MdPlayer::finish(ExitReason why)
{
	wait_writer();
	if (look_on_) {
		char sum[600];
		snprintf(sum, sizeof sum, "%s", look_.summary().c_str());
		for (char *c = sum; *c; c++) if (*c == '\n') *c = ' ';
		logf("%s\n", sum);
		look_save(true);
	}
	if (settings_dirty_) settings_queue_save();
	if (cheats_.dirty() && !cheats_busy_) cheats_queue_save();
	/* the resume point: where the game was left */
	if (states_ && md_host_state_size()) resume_post(true);
	wait_writer();
	if (md_host_sram_dirty()) {
		const size_t sz = md_host_sram_size();
		std::vector<uint8_t> b(sz);
		b.resize(md_host_sram_read(b.data(), sz));
		int r = 0;
		if (!b.empty()) {
			r = srm_file_.write(srm_, b.data(), b.size());
			if (r) srm_err_ = srm_file_.why(); else md_host_sram_mark_saved();
		}
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

}  // namespace mdp
