/* See player.h. Copyright CBOC-Vita. MIT licence (see md/frontend/LICENSE). */
#include "player.h"
#include "pk_patch.h"
#include "pk_ra_plat.h"
#include "pk_util.h"
#include "pk_i18n.h"
#include "pk_m3u.h"

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

/* menu shape (Fit, Fit (not offered), Zoom, Stretch, Pixel perfect) <-> the picture's shapes */
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
enum { CH_SHAPE = 1, CH_PIXEL = 2, CH_QUALITY = 4, CH_GLOW = 8, CH_SCAN = 16, CH_GRID = 32, CH_SCALER = 64, CH_GGCOL = 128, CH_USEGAME = 256, CH_WIDE = 512, CH_CRT = 1024 };
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
	if (m & CH_CRT) dst.crt = src.crt;
	if (m & CH_SCALER) dst.scaler = src.scaler;
	if (m & CH_GGCOL) dst.gg_colors = src.gg_colors;
	if (m & CH_USEGAME) dst.use_game = src.use_game;
	if (m & CH_WIDE) dst.wide = src.wide;
}

static std::string disc_file_name(const std::string &p)
{
	const size_t k = p.find_last_of("/:");
	return k == std::string::npos ? p : p.substr(k + 1);
}

static const char *kConsoles[5] = { "md", "sms", "gg", "sg", "scd" };

std::string MdPlayer::console_for_extension(const std::string &path)
{
	if (ends_with_ci(path, ".gg")) return "gg";
	if (ends_with_ci(path, ".sms") || ends_with_ci(path, ".mark3")) return "sms";
	if (ends_with_ci(path, ".sg") || ends_with_ci(path, ".sc") || ends_with_ci(path, ".sg1000")) return "sg";
	if (ends_with_ci(path, ".cue") || ends_with_ci(path, ".chd") || ends_with_ci(path, ".iso") || ends_with_ci(path, ".m3u")) return "scd";
	return "md";
}

/* The Sega CD needs the console's BIOS, which is the player's own and never part of VitAdhoc. The folders: <app>/bios and the game's folder. The
 * well known file names come last so that they win over a file that only has the right size; every candidate is checked by what is inside it (the
 * core's structure and region test), never by its name. */
int MdPlayer::load_cd_bios(const std::string &root, const std::string &game_path, std::string *log)
{
	static const char *const kNamed[] = {
		"us_scd1_9210.bin", "us_scd2_9303.bin", "eu_mcd1_9210.bin", "eu_mcd2_9303.bin", "jp_mcd1_9111.bin", "jp_mcd1_9112.bin", "jp_mcd2_921222.bin",
		"bios_CD_U.bin", "bios_CD_E.bin", "bios_CD_J.bin", "bios_cd_u.bin", "bios_cd_e.bin", "bios_cd_j.bin", "segacd_bios_us.bin", "megacd_bios_eu.bin", "megacd_bios_jp.bin"
	};
	std::vector<std::string> dirs;
	dirs.push_back(path_join(root, "bios"));
	size_t sl = game_path.find_last_of("/:");
	if (sl != std::string::npos) dirs.push_back(game_path.substr(0, sl + 1));
	md_host_cd_bios_clear();
	int tried = 0;
	for (size_t d = 0; d < dirs.size(); d++) {
		std::vector<std::string> names;
		std::vector<std::string> order;
		if (list_dir(dirs[d], names))
			for (size_t i = 0; i < names.size() && tried < 64; i++) {
				const std::string &nm = names[i];
				if (!(ends_with_ci(nm, ".bin") || ends_with_ci(nm, ".rom"))) continue;
				uint64_t sz = 0;
				if (!file_exists(path_join(dirs[d], nm), &sz) || sz != 0x20000) continue;
				order.push_back(nm);
				tried++;
			}
		for (size_t k = 0; k < sizeof kNamed / sizeof kNamed[0]; k++) order.push_back(kNamed[k]);
		for (size_t i = 0; i < order.size(); i++) {
			std::vector<uint8_t> b;
			if (!file_read(path_join(dirs[d], order[i]), b, 0x40000) || b.size() != 0x20000) continue;
			int reg = md_host_cd_bios_add(b.data(), b.size(), 0);
			if (reg && log) { const char *nm = md_host_cd_bios_name(b.data(), b.size()); *log += strf("Sega CD BIOS: %s (region %d%s%s)\n", order[i].c_str(), reg, nm ? ", " : "", nm ? nm : "an unlisted dump"); }
		}
	}
	return md_host_cd_bios_count();
}

std::string MdPlayer::pick_launch(const std::string &root, const std::string &arg_rom)
{
	std::string best;
	int64_t best_t = -1;
	for (int i = 0; i < 5; i++) {
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

MdPlayer::MdPlayer(Platform *pf) : pf_(pf), glue_(this)
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
				else { session_.note_state_touched(j->state.slot); notice(strf(tr("menu.state_saved_to_slot"), j->state.slot), NOTICE_OK); }
				if (menu_.is_open()) menu_.refresh();
			} else {
				menu_.state_write_done(j->state.slot, j->rep);
				if (j->rep.status != ST_OK) session_.note_notice(j->rep.text());
			}
			logf("state slot %d written: %s\n", j->state.slot, j->rep.status == ST_OK ? "ok" : j->rep.text(true).c_str());
			break;
		case WriterJob::RESUME:
			resume_busy_ = false;
			if (j->rep.status != ST_OK) logf("resume point NOT written: %s\n", j->rep.text(true).c_str());
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
			if (j->result) { notice(trf("md.cheat_list_not_saved", j->path2.c_str()), NOTICE_ERROR); cheats_.save(cht_file_); }
			logf("cheat list written rc=%d\n", j->result);
			break;
		}
		delete j;
	}
}

/* ---- start ---------------------------------------------------------------------- */

std::string MdPlayer::pic_game_ini() const { return path_join(path_join(cfg_dir(), "games"), game_ + ".ini"); }

/* Before an .m3u hid its discs, a Sega CD game on several discs was started from "Game (Disc 1).cue" and kept its save and states under that name.
 * The list is the game now, with a name of its own: when it has no save yet (and no states) and the first disc's exist, they are copied over, once
 * (the first disc's files stay where they are). */
void MdPlayer::adopt_first_disc()
{
	static char dl[PK_M3U_MAX_DISCS][PK_M3U_PATH];
	const int nd = pk_m3u_load(rom_.c_str(), dl, PK_M3U_MAX_DISCS, nullptr, nullptr);
	if (nd < 1) return;
	const std::string disc = dl[0];
	if (!file_exists(srm_)) {
		const std::string dsrm = srm_for(disc);
		std::vector<uint8_t> b;
		if (dsrm != srm_ && file_exists(dsrm) && file_read(dsrm, b, 2u << 20) && !b.empty()) {
			make_dirs(path_dir(srm_));
			const bool ok = atomic_write(srm_, b.data(), b.size(), EXISTS_NO) == W_OK;
			logf("adopted the save of the first disc (%s) for the .m3u game: %u bytes -> %s: %s\n", dsrm.c_str(), (unsigned) b.size(), srm_.c_str(), ok ? "copied, the first disc's file is kept" : "FAILED");
		}
	}
	StateLayout from, to;
	from.root = to.root = root_;
	from.console = to.console = console_;
	from.name = path_stem(disc);
	to.name = game_;
	int failed = 0;
	const int n = adopt_states(from, to, &failed);
	if (n || failed) logf("adopted the states of the first disc (%s) for the .m3u game: %d file(s) copied, %d failed; the first disc's files are kept\n", from.name.c_str(), n, failed);
}

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
	pal_mode_ = pk::pal_mode_clamp(pl.get_int("pal_speed", pk::PAL_SMOOTH));
	look_mode_ = clampi(pl.get_int("look_mode", 0), 0, 2);
	if (look_mode_ == 1) look_mode_ = 0;        /* "Full" is gone with the Image look row: a scaler as set is never eased now */
	/* An older file that says "no scaler" (or the Light look) with the Smooth video filter drew bilinear. Original draws sharp (as in the main app,
	 * where choosing bilinear over Original goes to Auto), so that choice is kept the way the app keeps it: the scaler goes to Auto. */
	if (imgq_of(ps_.scaler, look_mode_) == IMGQ_ORIGINAL && ps_.quality == PIC_SMOOTH) { ps_.scaler = SCALER_AUTO; if (look_mode_ == 2) look_mode_ = 0; g_changed |= CH_SCALER; }
	quick_ = clampi(pl.get_int("quick_response", console_ == "md" || console_ == "scd" ? 0 : 1), 0, 2);    /* Auto on the 8 bit consoles, Off on the Mega Drive (md/README.md, "Quick response") */
	flicker_ = pl.get_int("no_sprite_flicker", 1) != 0;
	region_ = clampi(pl.get_int("region", 0), 0, 3);
	fm_ = clampi(pl.get_int("fm", 0), 0, 2);
	ym2413_ = clampi(pl.get_int("ym2413", 2), 0, 2);
	pad_type_ = clampi(pl.get_int("pad_type", 0), 0, 2);
	sms_border_ = clampi(pl.get_int("sms_border", 0), 0, 2);
	gg_lcd_ = clampi(pl.get_int("gg_lcd", 0), 0, 10);
	ram_cart_ = clampi(pl.get_int("ram_cart", 6), 0, 6);
	ff_speed_ = clampi(pl.get_int("ff_speed", 0), 0, FFSPD_COUNT - 1);
	ff_sound_ = pl.get_int("ff_sound", 1) < 0 ? 0 : pl.get_int("ff_sound", 1) > 2 ? 2 : pl.get_int("ff_sound", 1);
	ff_toggle_ = pl.get_int("ff_mode", 0) == 1 ? 1 : 0;
	rewind_on_ = pl.get_int("rewind", 1) != 0;
	rewind_len_ = pl.get_int("rewind_seconds", 60) <= 30 ? 0 : 1;
	rewind_mb_ = clampi(pl.get_int("rewind_mb", 12), 4, 24);
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
	if ((v = req_.override_int("crt_mask", -1)) >= 0) ps_.crt = clampi(v, 0, 10);
	/* the settings every console shares (for the tests and the session runner: display.ini is the real source) */
	if (req_.overrides.count("screen")) { const std::string w = lower(req_.overrides.find("screen")->second); for (int i = 0; i < PK_SCREEN_CHOICES; i++) if (w == pk_screen_tag(i)) ui_.screen = i; }
	if (req_.overrides.count("text_size")) ui_.text_large = lower(req_.overrides.find("text_size")->second) == "large" ? 1 : 0;
	if (req_.overrides.count("language")) { const int l = pk::lang_index(req_.overrides.find("language")->second.c_str()); ui_.language = l >= pk::LANG_CHOICES ? pk::LANG_AUTO : l; pk::i18n_use(l, system_lang_); }
	if (req_.overrides.count("contrast")) { const std::string w = lower(req_.overrides.find("contrast")->second); ui_.contrast = w == "on" || w == "1" ? 1 : 0; }
	if ((v = req_.override_int("pixel_square", -1)) >= 0) ps_.pixel_sq = v ? 1 : 0;
	if ((v = req_.override_int("wide_view", -1)) >= 0) ps_.wide = clampi(v, 0, 2);
	if (req_.overrides.count("wide_view")) { std::string w = lower(req_.overrides.find("wide_view")->second); if (w == "auto") ps_.wide = 0; else if (w == "on") ps_.wide = 1; else if (w == "off") ps_.wide = 2; }
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
	if ((v = req_.override_int("rewind", -1)) >= 0) rewind_on_ = v != 0;
	if ((v = req_.override_int("rewind_mb", -1)) >= 0) rewind_mb_ = clampi(v, 4, 24);
	if ((v = req_.override_int("ff_speed", -1)) >= 0) ff_speed_ = clampi(v, 0, FFSPD_COUNT - 1);
	/* for the session runner's A/B: pal_speed 0 = true 50 Hz, 1 = fast 60 Hz (or the words); look_mode 0..2 (or auto / full / light) */
	if ((v = req_.override_int("pal_speed", -1)) >= 0) pal_mode_ = pk::pal_mode_clamp(v);
	if (req_.overrides.count("pal_speed")) { std::string w = lower(req_.overrides.find("pal_speed")->second); if (w == "fast" || w == "60") pal_mode_ = pk::PAL_FAST; else if (w == "true" || w == "50") pal_mode_ = pk::PAL_TRUE; else if (w == "smooth") pal_mode_ = pk::PAL_SMOOTH; }
	if ((v = req_.override_int("look_mode", -1)) >= 0) look_mode_ = clampi(v, 0, 2);
	if ((v = req_.override_int("quick_response", -1)) >= 0) quick_ = clampi(v, 0, 2);
	if ((v = req_.override_int("no_sprite_flicker", -1)) >= 0) flicker_ = v != 0;
	if (req_.overrides.count("look_mode")) { std::string w = lower(req_.overrides.find("look_mode")->second); if (w == "auto") look_mode_ = 0; else if (w == "full") look_mode_ = 1; else if (w == "light") look_mode_ = 2; }
}

/* the core's options from what the player holds; the game is loaded */
void MdPlayer::apply_core_options()
{
	if (linked()) {
		/* what changes what a game can see is the same on both consoles: the system's own values (the two consoles run one machine) */
		region_ = 0; fm_ = 0; ym2413_ = 2; pad_type_ = 0; pal_mode_ = pk::PAL_TRUE;
		md_host_set_option("vdp_mode", 0);
		md_host_set_option("force_dtack", 0);
		md_host_set_option("addr_error", 1);
		md_host_set_option("interlace", 1);
		md_host_set_option("overscan", 0);
	}
	md_host_set_region(region_);
	md_host_set_option("fm", fm_);
	md_host_set_option("ym2413", ym2413_);
	md_host_set_option("sms_border", sms_border_);
	md_host_set_option("lcd", gg_lcd_ * 10);
	/* pictures only (md/twin/ahead_check.sh): the machine and its status register are the same either way, so linked play keeps it too */
	md_host_set_option("sprite_limit", flicker_ ? 0 : 1);
	md_host_set_pad_type(0, pad_type_);
	md_host_set_pad_type(1, pad_type_);
}

void MdPlayer::sync_menu_values()
{
	menu_.set_value(ROW_PIC_SHAPE, pic_to_shape(ps_.shape));
	menu_.set_value(ROW_PIC_WIDE, ps_.wide);
	menu_.set_value(ROW_PIC_QUALITY, imgq_of(ps_.scaler, look_mode_));
	menu_.set_value(ROW_FILTER, imgq_of(ps_.scaler, look_mode_) == IMGQ_ORIGINAL ? 0 : ps_.quality == PIC_SMOOTH ? 1 : 0);
	menu_.set_value(ROW_PIC_SCANLINES, ps_.scan);
	menu_.set_value(ROW_PIC_GRID, ps_.grid);
	menu_.set_value(ROW_PIC_GLOW, ps_.glow);
	menu_.set_value(ROW_PIC_CRT, ps_.crt);
	menu_.set_value(ROW_PIC_SCREEN, ui_.screen);
	menu_.set_value(ROW_UI_TEXT, ui_.text_large);
	menu_.set_value(ROW_UI_CONTRAST, ui_.contrast);
	menu_.set_value(ROW_UI_LANGUAGE, ui_.language);
	menu_.set_value(ROW_PIC_APPLY, ps_.use_game ? 1 : 0);
	menu_.set_value(ROW_SMS_BORDER, sms_border_);
	menu_.set_value(ROW_GG_COLORS, ps_.gg_colors ? 1 : 0);
	menu_.set_value(ROW_GG_LCD, gg_lcd_);
	menu_.set_value(ROW_PADTYPE, pad_type_);
	menu_.set_value(ROW_REGION, region_);
	menu_.set_value(ROW_FM, fm_);
	menu_.set_value(ROW_RAMCART, ram_cart_);
	menu_.set_value(ROW_YM2413, ym2413_);
	menu_.set_value(ROW_STEADY, steady_ ? 1 : 0);
	menu_.set_value(ROW_QUICK, quick_);
	menu_.set_value(ROW_FLICKER, flicker_ ? 1 : 0);
	menu_.set_value(ROW_HANDOFF, handoff_ ? 1 : 0);
	menu_.set_value(ROW_PALSPEED, pk::pal_row_of(pal_mode_));
	menu_.set_value(ROW_SPD_FF, ff_speed_);
	menu_.set_value(ROW_SPD_SOUND, ff_sound_);
	menu_.set_value(ROW_SPD_MODE, ff_toggle_);
	menu_.set_value(ROW_SPD_REWIND, rewind_on_);
	menu_.set_value(ROW_REWIND_LEN, rewind_len_);
}

StartResult MdPlayer::start(int argc, char **argv, const std::string &root, bool system_enter_cross)
{
	StartResult res;
	init_shared();
	root_ = root;
	ui_ = pk::uiprefs_load(root_);          /* Screen, Text size, High contrast, Language: the same file as the app's Settings */
	pk::i18n_use(ui_.language, system_lang_);      /* the texts of everything below (fatal screens too) */

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
	scd_ = console_ == "scd";
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
		res.fatal1 = tr("player.no_game_was_given");
		res.fatal2 = tr("md.start_sega_games_from_vitadhoc");
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
	if (scd_ && pk_m3u_is_list(rom_.c_str())) adopt_first_disc();
	if (!req_.retroarch_save.empty() && !scd_) {          /* (a Sega CD save is the backup RAM and the RAM cartridge in one file: RetroArch keeps them apart) */
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
				if (ok) notice(tr("player.brought_in_the_newer_retroarch_save"), NOTICE_OK, true);
				else notice(tr("player.could_not_bring_in_the_retroarch"), NOTICE_ERROR, true);
				logf("retroarch save %s -> %s: %s\n", req_.retroarch_save.c_str(), srm_.c_str(), ok ? "imported" : "FAILED");
			}
		}
	}

	/* 3. the game, patched in memory when the app asked for it (never half-patched) */
	char err[200] = "";
	if (md_host_init()) { res.fatal1 = tr("md.the_sega_core_could_not_start"); res.fatal2 = tr("player.not_enough_memory"); return res; }
	std::vector<uint8_t> romb;
	if (scd_) {
		/* A Sega CD disc is streamed from the card by the core, never held in memory, and needs the player's own BIOS. What the machine is built
		 * with (the region the BIOS is told, the RAM cartridge) is read now: the disc is loaded once. */
		{ Ini p0; p0.load(path_join(cfg_dir(), "player.ini")); region_ = clampi(p0.get_int("region", 0), 0, 3); ram_cart_ = clampi(p0.get_int("ram_cart", 6), 0, 6); }
		if (linked()) region_ = 0;          /* two consoles run one machine: the system's own values (Auto region, the default 512 KB RAM cartridge on both) */
		md_host_set_region(region_);
		md_host_set_option("ram_cart", linked() ? 6 : ram_cart_);
		std::string bl;
		const int nb = load_cd_bios(root, rom_, &bl);
		if (!bl.empty()) logf("%s", bl.c_str());
		if (!nb) {
			logf("no Sega CD BIOS found (looked in %s and beside the game)\n", path_join(root, "bios").c_str());
			res.fatal1 = tr("player.this_game_could_not_be_started");
			res.fatal2 = tr("md.sega_cd_bios_missing");
			SessionReport r = session_.finish(EXIT_ERROR);
			r.notice = tr("md.sega_cd_bios_missing_notice");
			session_write(root_, r);
			return res;
		}
		if (!req_.patch_path.empty()) logf("patch ignored: a disc cannot be patched in memory\n");
		/* a game on several discs: an .m3u names them; the first one that is there goes in, the others wait for "Change disc" */
		std::string disc_path = rom_;
		int disc_first = 0;
		discs_.clear();
		if (pk_m3u_is_list(rom_.c_str())) {
			static char dl[PK_M3U_MAX_DISCS][PK_M3U_PATH];
			int missing = 0, skipped = 0;
			const int nd = pk_m3u_load(rom_.c_str(), dl, PK_M3U_MAX_DISCS, &missing, &skipped);
			std::string gone;
			for (int i = 0; i < nd; i++) {
				discs_.push_back(dl[i]);
				if (!file_exists(dl[i], nullptr, nullptr)) gone += (gone.empty() ? "" : ", ") + disc_file_name(dl[i]);
			}
			logf("m3u %s: %d disc(s), %d missing, %d line(s) not used\n", rom_.c_str(), nd, missing, skipped);
			disc_first = -1;
			for (int i = 0; i < nd && disc_first < 0; i++) if (file_exists(dl[i], nullptr, nullptr)) disc_first = i;
			if (nd <= 0 || disc_first < 0) {
				res.fatal1 = tr("player.this_game_could_not_be_started");
				res.fatal2 = nd < 0 ? std::string(tr("md.m3u_unreadable")) : nd == 0 ? std::string(tr("md.m3u_empty")) : trf("md.m3u_none_found", gone.c_str());
				SessionReport r = session_.finish(EXIT_ERROR);
				r.notice = trf("player.game_not_started_err", res.fatal2.c_str());
				session_write(root_, r);
				return res;
			}
			disc_path = discs_[(size_t) disc_first];
			if (!gone.empty()) notice(strf("The list names a disc that is missing: %s", gone.c_str()), NOTICE_ERROR, true);
		}
		if (md_host_load_disc(disc_path.c_str(), err, sizeof err)) {
			logf("load failed: %s\n", err);
			res.fatal1 = tr("player.this_game_could_not_be_started");
			{ char found[64]; md_host_cd_bios_regions(found, sizeof found);
			res.fatal2 = trf("md.sega_cd_load_failed", err, found); }
			SessionReport r = session_.finish(EXIT_ERROR);
			r.notice = trf("player.game_not_started_err", err);
			session_write(root_, r);
			return res;
		}
		if (discs_.size() > 1) {
			static char dl2[MD_HOST_DISC_MAX][MD_HOST_DISC_PATH];
			for (size_t i = 0; i < discs_.size() && i < (size_t) MD_HOST_DISC_MAX; i++) snprintf(dl2[i], sizeof dl2[i], "%s", discs_[i].c_str());
			md_host_cd_set_discs(dl2, (int) std::min<size_t>(discs_.size(), MD_HOST_DISC_MAX), disc_first);
			logf("disc %d of %d in the drive: %s\n", disc_first + 1, (int) discs_.size(), disc_path.c_str());
		}
		logf("%s\n", md_host_cd_info());
		if (md_host_cd_notice()[0]) notice(md_host_cd_notice(), NOTICE_ERROR, true);           /* the disc's region has no BIOS of its own here: say what to add */
	} else
	if (!file_read(rom_, romb, 12u << 20) || romb.empty()) { res.fatal1 = tr("player.this_game_could_not_be_started"); res.fatal2 = tr("player.the_game_file_could_not_be"); return res; }
	if (!scd_ && !req_.patch_path.empty()) {
		std::vector<uint8_t> out;
		PatchResult pr = patch_apply_files(req_.patch_path, romb, out);
		logf("patch %s (%s): %s\n", req_.patch_path.c_str(), patch_kind_name(pr.kind), pr.status == PS_OK ? "applied" : pr.text(true).c_str());
		if (pr.status != PS_OK) {
			res.fatal1 = tr("player.the_patch_could_not_be_applied");
			res.fatal2 = pr.text();
			SessionReport r = session_.finish(EXIT_ERROR);
			r.notice = trf("player.patch_failed_notice", pr.text().c_str());
			session_write(root_, r);
			return res;
		}
		romb.swap(out);
	}
	if (!scd_ && md_host_load_mem(romb.data(), romb.size(), rom_.c_str(), system_, err, sizeof err)) {
		logf("load failed: %s\n", err);
		res.fatal1 = tr("player.this_game_could_not_be_started");
		res.fatal2 = err;
		SessionReport r = session_.finish(EXIT_ERROR);
		r.notice = trf("player.game_not_started_err", err);
		session_write(root_, r);
		return res;
	}
	if (req_.link.active()) {
		/* the session's identity of the game: the bytes the core was given. The file itself, or a copy when it was patched
		 * (two consoles with the same game must agree on the bytes) */
		if (system_ == MD_SYS_GG) {
			res.fatal1 = tr("player.the_two_player_link_could_not");
			res.fatal2 = tr("md.a_game_gear_game_has_one");
			md_host_unload();
			return res;
		}
		if (scd_) {
			/* the identity of a disc is its header sector, its length and the BIOS in use (a disc is hundreds of MB: the link never reads it all) */
			std::vector<uint8_t> id(2048 + 8);
			make_dirs(path_join(root, "launch"));
			std::string lp = path_join(path_join(root, "launch"), "link_rom.bin");
			uint32_t crc = md_host_rom_crc(), bc = md_host_cd_bios_crc();
			md_host_cd_header(id.data(), 2048);
			for (int i = 0; i < 4; i++) { id[2048 + i] = (uint8_t) (crc >> (8 * i)); id[2052 + i] = (uint8_t) (bc >> (8 * i)); }
			if (atomic_write(lp, id.data(), id.size()) == W_OK) link_rom_ = lp;
		} else
		if (req_.patch_path.empty()) link_rom_ = rom_;
		else {
			make_dirs(path_join(root, "launch"));
			std::string lp = path_join(path_join(root, "launch"), "link_rom.bin");
			if (atomic_write(lp, romb.data(), romb.size()) == W_OK) link_rom_ = lp;
		}
		logf("link: role=%s mode=%s identity file %s (%s)\n", req_.link.role.c_str(), req_.link.mode.c_str(), link_rom_.c_str(),
		     link_rom_ == rom_ ? "the game file" : "a copy of the image the core runs");
		if (link_rom_.empty()) {
			res.fatal1 = tr("player.the_two_player_link_could_not");
			res.fatal2 = tr("player.the_game_file_could_not_be_2");
			return res;
		}
	} else
	if (scd_) {
		/* RetroAchievements hashes a Sega CD game by the first 512 bytes of its first sector (rcheevos, console 9) */
		uint8_t hdr[512];
		if (md_host_cd_header(hdr, sizeof hdr) == 0) ra_begin(hdr, sizeof hdr);
	} else
	ra_begin(romb.data(), romb.size());       /* the file as the site knows it (the patched one for a patched game) */
	{
		std::vector<uint8_t>().swap(romb);
		std::vector<uint8_t> sv;
		if (file_exists(srm_) && file_read(srm_, sv, 2u << 20) && !sv.empty()) {
			const size_t used = md_host_sram_write(sv.data(), sv.size());
			logf("battery save loaded: %u bytes of %u (%s)\n", (unsigned) used, (unsigned) sv.size(), srm_.c_str());
			if (scd_ && md_host_cd_kept_cartridges())
				logf("the save holds %d RAM cartridge image(s) of other sizes than the one fitted: kept untouched, written back with every save\n", md_host_cd_kept_cartridges());
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
	if (!linked()) cheats_.load(path_join(path_join(root, "cheats"), game_ + ".cht"), system_ == MD_SYS_MD);   /* a code pokes one console's machine only: none in a linked game */
	if (!linked()) {
		auto it = req_.unknown.find("cheats_db");
		int added = it != req_.unknown.end() ? cheats_.merge_file(it->second) : 0;
		if (added) logf("cheats: %d from the database file %s (off until switched on)\n", added, it->second.c_str());
	}
	cheats_apply();
	logf("cheats: %d in the list, %d on\n", cheats_.size(), (int) cheats_.active_parts().size());

	MenuConfig cfg;
	cfg.game_name = req_.game_name.empty() ? game_ : req_.game_name;
	cfg.console_label = scd_ ? "Sega CD" : system_ == MD_SYS_MD ? "Mega Drive" : system_ == MD_SYS_SMS ? "Master System" : system_ == MD_SYS_GG ? "Game Gear" : "SG-1000";
	cfg.caps = CAP_STATES | CAP_UNDO | CAP_RESET | CAP_WIDE | CAP_SHAPE | CAP_QUALITY | CAP_SCANLINES | CAP_LCD_GRID | CAP_CRT | CAP_GLOW |
	           CAP_PIC_PER_GAME | CAP_CONTROLS | CAP_TURBO | CAP_CHEATS | CAP_CHEAT_EDIT | CAP_CHEAT_SEARCH | CAP_FF | CAP_FF_SOUND | CAP_FF_MODE | CAP_REWIND | extra_caps_;
	cfg.enter_is_cross = req_.enter_is_cross;
	cfg.players = model->players;
	cfg.system_lang = system_lang_;
	cfg.play_together_reason = system_ == MD_SYS_GG ? "md.a_game_gear_game_has_one_2" : "player.start_it_from_the_game_list";
	cfg.accent = system_ == MD_SYS_MD ? 0x2E7DFF : system_ == MD_SYS_GG ? 0xE0B020 : 0xE04A3A;
	menu_.configure(cfg, this, states_);
	menu_.attach_input(&mapper_, store_, model, &profile_);
	menu_.set_choices_allowed(ROW_PIC_SHAPE, (1u << 0) | (1u << 2) | (1u << 3) | (1u << 4));   /* one "Fit" (index 0, the main app's word): it already fills the height */
	menu_.set_choices_allowed(ROW_PIC_QUALITY, ((1u << IMGQ_COUNT) - 1) & ~(1u << IMGQ_ENHANCED_4X));   /* no 4x scaler in a player */
	{
		std::vector<std::string> px, onoff, border, lcd, pad, region, fm, ym, palv, filt;
		px.push_back("md.console"); px.push_back("input.square");
		onoff.push_back("menu.off"); onoff.push_back("menu.on");
		border.push_back("menu.off"); border.push_back("md.left_edge"); border.push_back("md.both_edges");
		lcd.push_back("menu.off");
		for (int i = 1; i <= 10; i++) lcd.push_back(strf("%d", i));
		pad.push_back("menu.auto"); pad.push_back("md.3_buttons"); pad.push_back("md.6_buttons");
		region.push_back("menu.auto"); region.push_back("md.usa"); region.push_back("md.europe_50_hz"); region.push_back("md.japan");
		fm.push_back("md.discrete"); fm.push_back("md.asic"); fm.push_back("md.enhanced");
		ym.push_back("menu.off"); ym.push_back("menu.on"); ym.push_back("menu.auto");
		palv.push_back("player.smooth_50_hz"); palv.push_back("player.true_50_hz"); palv.push_back("player.fast_60_hz");
		filt.push_back(kVideoFilterNames[0]); filt.push_back(kVideoFilterNames[1]);
		menu_.add_host_row(Menu::PAGE_PICTURE, ROW_FILTER, "player.video_filter", PK_VIDEO_FILTER_HELP, filt, ps_.quality == PIC_SMOOTH ? 1 : 0, ROW_PIC_SCANLINES);
		if (system_ == MD_SYS_SMS)
			menu_.add_host_row(Menu::PAGE_PICTURE, ROW_SMS_BORDER, "md.hide_masked_edge",
			                   "md.games_on_the_master_system_cover",
			                   border, sms_border_, ROW_PIC_APPLY);
		if (system_ == MD_SYS_GG) {
			menu_.add_host_row(Menu::PAGE_PICTURE, ROW_GG_COLORS, "md.lcd_colours",
			                   "md.the_game_gear_s_screen_showed", onoff, ps_.gg_colors ? 1 : 0, ROW_PIC_APPLY);
			menu_.add_host_row(Menu::PAGE_PICTURE, ROW_GG_LCD, "md.lcd_ghosting",
			                   "md.the_slow_game_gear_screen_left", lcd, gg_lcd_, ROW_PIC_APPLY);
		}
		if (system_ != MD_SYS_MD)
			menu_.add_host_row(Menu::PAGE_PICTURE, ROW_FLICKER, "md.no_sprite_flicker",
			                   system_ == MD_SYS_SG ? "md.the_console_drew_only_4_sprites"
			                                        : "md.the_console_drew_only_8_sprites",
			                   onoff, flicker_ ? 1 : 0, ROW_PIC_APPLY);
		if (system_ == MD_SYS_MD)
			menu_.add_host_row(Menu::PAGE_CONTROLS, ROW_PADTYPE, "md.pad_type",
			                   "md.auto_follows_the_game_3_buttons", pad, pad_type_, ROW_CTL_MAPPING);
		{
			std::vector<std::string> rl;
			rl.push_back(kRewindLengthNames[0]); rl.push_back(kRewindLengthNames[1]);
			menu_.add_host_row(Menu::PAGE_SPEED, ROW_REWIND_LEN, "player.rewind_length",
			                   scd_ ? "md.how_far_back_rewind_reaches_short"
			                        : PK_REWIND_LENGTH_HELP, rl, rewind_len_, ROW_SPD_REWIND);
		}
		if (scd_) {
			std::vector<std::string> rc;
			rc.push_back("menu.off"); rc.push_back("md.16_kb"); rc.push_back("md.32_kb"); rc.push_back("md.64_kb"); rc.push_back("md.128_kb"); rc.push_back("md.256_kb"); rc.push_back("md.512_kb");
			menu_.add_host_row(Menu::PAGE_SPEED, ROW_RAMCART, "md.ram_cartridge",
			                   "md.extra_save_memory_some_games_can",
			                   rc, ram_cart_);
		}
		if (scd_ && discs_.size() > 1 && !linked()) {
			std::vector<std::string> dc;
			for (size_t i = 0; i < discs_.size(); i++) dc.push_back(strf(tr("md.disc"), (int) i + 1));
			menu_.add_host_row(Menu::PAGE_SPEED, ROW_DISC, "md.change_disc",
			                   "md.for_a_game_on_several_discs",
			                   dc, md_host_cd_disc_index(), ROW_REGION);
		}
		menu_.add_host_row(Menu::PAGE_SPEED, ROW_REGION, "md.region",
		                   "md.auto_follows_the_game_europe_runs", region, region_);
		if (system_ == MD_SYS_MD)
			menu_.add_host_row(Menu::PAGE_SPEED, ROW_FM, "md.fm_sound",
			                   "md.the_sound_chip_discrete_is_the", fm, fm_);
		if (system_ == MD_SYS_SMS)
			menu_.add_host_row(Menu::PAGE_SPEED, ROW_YM2413, "md.fm_sound_unit",
			                   "md.the_master_system_s_fm_add", ym, ym2413_);
		menu_.add_host_row(Menu::PAGE_SPEED, ROW_STEADY, "player.steady_pacing",
		                   "player.when_a_scene_is_too_heavy", onoff, 1);
		menu_.add_host_row(Menu::PAGE_SPEED, ROW_PALSPEED, "player.pal_games_speed",
		                   "player.european_games_run_at_50_hz",
		                   palv, pk::pal_row_of(pk::PAL_SMOOTH));
		{
			std::vector<std::string> qv;
			qv.push_back("menu.off"); qv.push_back("menu.auto"); qv.push_back("menu.on");
			menu_.add_host_row(Menu::PAGE_SPEED, ROW_QUICK, "player.quick_response",
			                   "player.shows_what_you_press_one_frame",
			                   qv, quick_);
		}
		menu_.add_host_row(Menu::PAGE_SPEED, ROW_HANDOFF, "player.smooth_hand_over",
		                   "player.draws_each_picture_on_its_own", onoff, 1);
	}
	sync_menu_values();

	md_video_info vi;
	md_host_video_info(&vi);
	logf("title: %s | %s | %s pal=%d fps=%.4f sram=%u | picture: shape=%d quality=%d glow=%d scan=%d grid=%d scaler=%d own=%d steady=%d handoff=%d\n",
	     md_host_rom_title(), md_host_system_hw(), md_host_rom_info(), md_host_is_pal(), md_host_fps(), (unsigned) md_host_sram_size(), ps_.shape,
	     ps_.quality, ps_.glow, ps_.scan, ps_.grid, ps_.scaler, ps_.use_game, steady_ ? 1 : 0, handoff_ ? 1 : 0);
	speed_.set_host(&glue_);
	speed_start();
	ra_update();
	states_->resume_mark(pf_->now_us() / 1000);
	res.ok = true;
	return res;
}

void MdPlayer::apply_start_mode()
{
	if (linked()) return;      /* a linked game starts from power-on and from the host's game, never from a resume point or slot */
	std::vector<uint8_t> d;
	if ((req_.start == START_CONTINUE || req_.start == START_SLOT) && ra_.hardcore_blocks()) {
		notice(tr("player.hardcore_mode_the_game_starts_from"), NOTICE_INFO);
		logf("hardcore: the start from a saved state is refused\n");
		return;
	}
	if (req_.start == START_CONTINUE) {
		if (!states_->resume_present()) { notice(tr("player.no_point_to_continue_from_the"), NOTICE_INFO); return; }
		States::PatchMatch pm = states_->resume_patch(req_.patch_key);
		if (pm == States::PATCH_DIFFERENT || (pm == States::PATCH_UNKNOWN && !req_.patch_key.empty())) {
			notice(tr("player.the_resume_point_was_made_with"), NOTICE_INFO);
			return;
		}
		StateStatus st = states_->load_resume(d);
		if (st != ST_OK) { notice(trf("player.could_not_continue", state_status_text(st)), NOTICE_ERROR); return; }
		if (!restore_state(d.data(), d.size())) { notice(tr("player.could_not_continue_the_resume_point"), NOTICE_ERROR); return; }
		logf("continued from the resume point\n");
	} else if (req_.start == START_SLOT) {
		StateStatus st = states_->load(req_.slot, d);
		if (st != ST_OK) { notice(strf(tr("player.slot"), req_.slot, state_status_text(st)), st == ST_EMPTY ? NOTICE_INFO : NOTICE_ERROR); return; }
		if (!restore_state(d.data(), d.size())) { notice(strf(tr("player.slot_could_not_be_loaded_this"), req_.slot), NOTICE_ERROR); return; }
		session_.note_state_touched(req_.slot);
		notice(strf(tr("menu.state_loaded_from_slot"), req_.slot), NOTICE_OK);
	}
}

/* ---- per frame ------------------------------------------------------------------- */

bool MdPlayer::input(const PadSample pads[MAX_PLAYERS], bool tapped, uint64_t now_ms, uint32_t frame)
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
	md_host_set_pad(0, in_.pad[0] & 0xFFFu);
	md_host_set_pad(1, system_ == MD_SYS_GG ? 0u : (in_.pad[1] & 0xFFFu));
	session_.tick_running(now_ms);
	if (in_.fired & FIRE_QSAVE) quick_save();
	if (in_.fired & FIRE_QLOAD) quick_load();
	return false;
}

/* ---- fast forward and rewind ---------------------------------------------------------------------------------------------- */

size_t MdPlayer::SpeedGlue::state_size() { return md_host_state_size(); }
bool MdPlayer::SpeedGlue::save_state(uint8_t *buf, size_t cap) { return md_host_state_save_quick(buf, cap) == md_host_state_size(); }
bool MdPlayer::SpeedGlue::load_state(const uint8_t *buf, size_t n) { p->srm_recheck_ = true; p->jumps_++; return md_host_state_load_quick(buf, n) == 0; }
void MdPlayer::SpeedGlue::run_frame(bool render)
{
	md_host_set_render(render ? 1 : 0);
	/* Quick response: a drawn frame at normal speed is run ahead (the real frame undrawn, then the picture of the one after it) */
	p->ahead_ran_ = false;
	if (render && !p->speed_.busy() && p->ahead_.want()) { p->ahead_ran_ = md_host_run_frame_ahead(p->ahead_hook_, p->ahead_user_) == 0; return; }
	md_host_run_frame();
}

/* ---- Quick response (run-ahead) -------------------------------------------------------------------------------------------- */

void MdPlayer::ahead_update_blocked()
{
	/* never in a linked game (two machines in lockstep), under fast forward or rewind, while steady pacing shows every second frame, or
	 * with a cartridge whose EEPROM or flash chip the save state cannot put back */
	ahead_.set_blocked(linked() || speed_.busy() || pacer_.mode() == PACE_HALF || !md_host_ahead_possible());
}

void MdPlayer::set_quick(int mode)
{
	quick_ = clampi(mode, 0, 2);
	ahead_.set_mode(quick_);
	ahead_update_blocked();
	logf("quick response: %s%s\n", quick_ == 0 ? "off" : quick_ == 1 ? "auto" : "on", md_host_ahead_possible() ? "" : " (no machine loaded)");
}

void MdPlayer::ahead_cost(uint32_t emu_us)
{
	ahead_update_blocked();
	const bool was = ahead_.on();
	ahead_.add(emu_us, ahead_ran_);
	if (ahead_.on() != was)
		logf("quick response: %s (a frame costs %.1f ms plain, %.1f ms run ahead)\n", ahead_.on() ? "started" : "stopped", ahead_.plain_us() / 1000.0, ahead_.ahead_us() / 1000.0);
}

void MdPlayer::ahead_picture(uint32_t draw_us, uint32_t missed)
{
	ahead_update_blocked();
	const bool was = ahead_.on();
	ahead_.add_picture(draw_us, missed);
	if (was && !ahead_.on())
		logf("quick response: stopped (the picture side ran late: drawing %.1f ms a picture, %u refreshes missed)\n", ahead_.picture_us() / 1000.0, missed);
}

std::string MdPlayer::ahead_line()
{
	unsigned long st[2];
	md_host_ahead_stat(st);
	std::string s = strf("ahead: mode=%s on=%d frames=%u/%u plain_ms=%.2f ahead_ms=%.2f ratio=%.2f starts=%u stops=%u pic_ms=%.2f pic_stops=%u pic_miss=%u sram_put_back=%lu",
	                     quick_ == 0 ? "off" : quick_ == 1 ? "auto" : "on", ahead_.want() ? 1 : 0, ahead_.frames_ahead(), ahead_.frames(),
	                     ahead_.plain_us() / 1000.0, ahead_.ahead_us() / 1000.0, ahead_.ratio_x100() / 100.0, ahead_.starts(), ahead_.stops(), ahead_.picture_us() / 1000.0, ahead_.picture_stops(), ahead_.picture_misses(), st[1]);
	ahead_.census_reset();
	return s;
}
void MdPlayer::SpeedGlue::clear_pads() { md_host_set_pad(0, 0); md_host_set_pad(1, 0); }
uint64_t MdPlayer::SpeedGlue::now_us() { return p->pf_->now_us(); }
void MdPlayer::SpeedGlue::wake(void *self) { ((SpeedGlue *) self)->p->pf_->wake_writer(); }

SpeedSettings MdPlayer::speed_settings() const
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

void MdPlayer::speed_start()
{
	/* a linked game steps both machines in lockstep: neither rewind nor fast forward */
	if (linked()) speed_.set_locked(true, "rewind.rewind_is_off_while_games_are");
	/* a disc's state is about 1 MB (a cartridge's 0.15 MB): a capture every 4 frames, not 2, halves what the emulating thread spends on rewind (the
	 * state copy is a few ms on a Vita, an estimate); the ring stretches the interval further when the picture changes much (FMV) */
	speed_.start(md_host_fps(), scd_ ? 4 : 2, speed_settings());
	ahead_.set_frame_us((uint32_t) (1e6 / md_host_fps() + 0.5));
	set_quick(quick_);
	/* with a writer thread the rewind states are packed there (the game thread only copies the state); without one capture_commit packs at once */
	speed_.rewind().set_async(pf_->wake_writer(), &SpeedGlue::wake, &glue_);
	logf("%s (state %u bytes, %d s, %d MB)\n", speed_.stats_line().c_str(), (unsigned) md_host_state_size(), rewind_len_ ? 60 : 30, rewind_mb_);
}

/* ---- RetroAchievements ----------------------------------------------------------------------------------------------------- */

uint32_t MdPlayer::ra_read(void *, uint32_t addr, uint8_t *out, uint32_t n) { return (uint32_t) md_host_ra_read(addr, out, n); }
uint64_t MdPlayer::ra_now(void *ud) { return ((MdPlayer *) ud)->pf_->now_us(); }
int MdPlayer::ra_online(void *) { return pk_ra_net_online(); }
void MdPlayer::ra_log(void *ud, const char *line) { ((MdPlayer *) ud)->logf("ra: %s\n", line); }

void MdPlayer::ra_begin(const uint8_t *rom, size_t n)
{
	pk::RaHost h;
	h.read = &MdPlayer::ra_read;
	h.now_us = &MdPlayer::ra_now;
	h.online = &MdPlayer::ra_online;
	h.log = &MdPlayer::ra_log;
	h.ud = this;
	if (!ra_.boot(root_.c_str(), ra_server_.empty() ? 0 : ra_server_.c_str(), "2.0", h)) return;       /* no account on file: nothing runs */
	ra_.game_begin(rom, n, md_host_ra_console());
}

void MdPlayer::ra_pump() { if (ra_.up()) ra_.pump(); }

/* Hardcore turns up (or goes away: the game has no achievements) a moment after the game started: rewind and the cheats follow it */
void MdPlayer::ra_update()
{
	const bool b = ra_.hardcore_blocks();
	if (b == ra_blocks_) return;
	ra_blocks_ = b;
	speed_.set_rewind_locked(b, PK_RA_NO_REWIND);
	cheats_apply();
	logf("hardcore: %s\n", b ? "in force: no rewind, no cheats, no state loads" : "not in force");
}

const char *MdPlayer::state_load_refused() { return ra_.hardcore_blocks() ? PK_RA_NO_STATE : 0; }
const char *MdPlayer::cheats_refused() { return ra_.hardcore_blocks() ? PK_RA_NO_CHEATS : 0; }

int MdPlayer::speed_plan(int normal)
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

/* ---- Change disc: the tray opens, the disc is swapped a moment later, the tray closes a moment after that (the game's own disc check sees all three) ---- */

void MdPlayer::change_disc(int index)
{
	if (!scd_ || discs_.size() < 2 || linked()) return;
	if (disc_phase_) { notice(tr("md.the_disc_is_being_changed_already"), NOTICE_INFO); sync_disc_row(); return; }
	if (index < 0 || index >= (int) discs_.size()) { sync_disc_row(); return; }
	/* the tray may be open already (the game opened it, or a saved moment taken while it was open came back): the change goes on from there, and
	 * choosing the disc that is in closes it */
	const bool open = md_host_cd_tray_open() != 0;
	if (index == md_host_cd_disc_index() && !open) { notice(strf(tr("md.disc_is_in_the_drive_already"), index + 1), NOTICE_INFO); return; }
	if (!file_exists(discs_[(size_t) index], nullptr, nullptr)) {
		notice(strf(tr("md.disc_is_missing"), index + 1, disc_file_name(discs_[(size_t) index]).c_str()), NOTICE_ERROR, true);
		sync_disc_row();
		return;
	}
	if (!open && md_host_cd_eject() != 0) { sync_disc_row(); return; }
	disc_want_ = index;
	disc_phase_ = index == md_host_cd_disc_index() ? 2 : 1;
	disc_wait_ = kDiscTrayFrames;
	logf("change disc: tray open%s, disc %d next\n", open ? " already" : "", index + 1);
	if (!open) notice(tr("md.disc_tray_open"), NOTICE_OK);
}

void MdPlayer::sync_disc_row() { menu_.set_value(ROW_DISC, md_host_cd_disc_index()); }

void MdPlayer::disc_tick()
{
	if (!disc_phase_ || --disc_wait_ > 0) return;
	if (!md_host_cd_tray_open()) {             /* a saved moment (a state, a rewind) came in with the tray closed: the change is over, its disc is the one in */
		disc_phase_ = 0;
		sync_disc_row();
		return;
	}
	if (disc_phase_ == 1) {
		char err[160];
		if (md_host_cd_swap(disc_want_, err, sizeof err) == 0) {
			logf("change disc: disc %d in the drive\n", disc_want_ + 1);
		} else {
			logf("change disc: disc %d could not be read: %s\n", disc_want_ + 1, err);
			notice(strf(tr("md.disc_could_not_be_read"), disc_want_ + 1, err), NOTICE_ERROR, true);
		}
		disc_phase_ = 2;
		disc_wait_ = kDiscTrayFrames;
		return;
	}
	md_host_cd_close();
	disc_phase_ = 0;
	notice(strf(tr("md.disc_inserted"), md_host_cd_disc_index() + 1), NOTICE_OK);
	sync_disc_row();
	logf("change disc: tray closed\n");
}

bool MdPlayer::run_frame(bool render)
{
	disc_tick();
	const bool landing = speed_.rewinding();
	const bool ran = speed_.run_frame(render);
	if (ran) {
		if (landing) ra_.state_loaded();      /* a rewound game is another timeline: progress starts over */
		else ra_.frame(linked());             /* two players: nothing is processed */
	}
	return ran;
}

size_t MdPlayer::audio_filter(int16_t *lr, size_t frames) { return speed_.audio(lr, frames); }

bool MdPlayer::want_render(uint32_t frame) const
{
	return !steady_ || pacer_.mode() == PACE_FULL || (frame & 1) == 0;
}

void MdPlayer::drawn_cost(uint32_t cost_us)
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

std::string MdPlayer::pace_line()
{
	Pacer::Counts c = pacer_.take_counts();
	return strf("pace=%s drawn=%d over=%d max=%.1f changes=%d", pace_mode_name(pacer_.mode()), c.drawn, c.over, c.max_us / 1000.0, c.changes);
}

void MdPlayer::quick_save()
{
	std::vector<uint8_t> d;
	if (!capture_state(d)) { notice(tr("menu.could_not_save_the_state_the"), NOTICE_ERROR); return; }
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
	if (const char *why = state_load_refused()) { notice(why, NOTICE_ERROR); return; }
	wait_writer();
	std::vector<uint8_t> d, now;
	StateStatus st = states_->load(1, d);
	if (st != ST_OK) { notice(st == ST_EMPTY ? std::string(tr("player.slot_1_is_empty")) : state_status_text(st), st == ST_EMPTY ? NOTICE_INFO : NOTICE_ERROR); return; }
	if (capture_state(now)) states_->undo_store(now.data(), now.size());
	if (!restore_state(d.data(), d.size())) { notice(tr("menu.the_state_could_not_be_loaded"), NOTICE_ERROR); return; }
	session_.note_state_touched(1);
	notice(tr("player.state_loaded_from_slot_1"), NOTICE_OK);
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
	/* the resume point: every 2 minutes, and soon after the game saved (at most every 30 s) */
	if (!resume_busy_ && !linked()) {
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
	save_msg_ = strf(tr("player.save_failed_retry"), srm_err_.empty() ? tr("player.unknown_error") : srm_err_.c_str());
	logf("sram save FAILED rc=%d (%s): %s - the file on the card was not touched; retry in %u s\n", r, how, srm_err_.c_str(), srm_backoff_s_);
	if (!save_failing_) notice(save_msg_, NOTICE_ERROR);
	session_.note_save_error(save_msg_);
	save_failing_ = true;
	srm_retry_at_ = now + (uint64_t) srm_backoff_s_ * 1000000;
	if (srm_backoff_s_ < 60) srm_backoff_s_ = srm_backoff_s_ * 2 > 60 ? 60 : srm_backoff_s_ * 2;
}

void MdPlayer::srm_watch(uint64_t now)
{
	if (linked()) return;      /* the machine holds the host's game: nothing of it is this console's save (the host's is committed by the session) */
	if (const unsigned gen = md_host_sram_generation()) {
		/* a disc: the core counts every write to the backup RAM and the cartridge, so nothing of the (up to 520 KB) save is read or hashed
		 * unless something wrote; the host's dirty answer is a CRC compare done only then */
		if (gen != srm_gen_ || srm_recheck_) {
			if (gen != srm_gen_) srm_last_change_ = now;
			srm_gen_ = gen;
			srm_recheck_ = false;
			srm_dirty_ = md_host_sram_dirty() != 0;
		}
	} else
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
	if (linked()) { session_.pause(); return; }
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

/* the host's battery after an agreed end (netplay.h): the same safe file as every other battery save */
int MdPlayer::link_commit_save(const uint8_t *data, size_t n)
{
	wait_writer();
	const int r = srm_file_.write(srm_, data, n);
	if (r) srm_err_ = srm_file_.why(); else md_host_sram_mark_saved();
	srm_result(r, "link", pf_->now_us());
	return r;
}

/* ---- cheats --------------------------------------------------------------------- */

void MdPlayer::cheats_apply()
{
	md_host_cheat_reset();
	if (ra_.hardcore_blocks()) return;         /* Hardcore: the list stays, none of it is applied */
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
	if (index < 0 || index >= cheats_.size() || !cheats_.at(index).usable) { notice(tr("player.this_cheat_cannot_be_switched_on"), NOTICE_ERROR); return; }
	cheats_.set_on(index, on);
	cheats_apply();
	cheats_queue_save();
	notice(trf(on ? "player.cheat_on" : "player.cheat_off", cheats_.at(index).label.c_str()), on ? NOTICE_OK : NOTICE_INFO);
}

void MdPlayer::add_cheat() { pf_->ask_cheat_text(); }

bool MdPlayer::search_setup(pk_cs *cs)
{
	/* the work RAM, as a typed code reaches it: the Mega Drive's (and the Sega CD's) $FF0000-$FFFFFF as the core holds it, the 8 bit systems' $C000-$DFFF */
	static const pk_cs_region md_reg[1] = { { 0, 0x10000 } };
	static const pk_cs_region z80_reg[1] = { { 0, 0x2000 } };
	if (system_ == MD_SYS_MD) pk_cs_init(cs, &MdPlayer::ra_read, this, PK_CS_MDW, PK_CS_MD, md_reg, 1);
	else {
		/* an SG-1000 has 1 KB of RAM (an SG-1000 II 2 KB) repeated up to $DFFF: only the first copy is searched, the one a typed
		 * code writes (the copies would be eight results for one place, seven of them codes that do nothing) */
		const std::string hw = md_host_system_hw();
		pk_cs_region one[1] = { { 0, hw == "SG-1000" ? 0x400u : hw == "SG-1000 II" ? 0x800u : z80_reg[0].size } };
		pk_cs_init(cs, &MdPlayer::ra_read, this, PK_CS_Z80, PK_CS_LE, one, 1);
	}
	return true;
}

const char *MdPlayer::search_refused()
{
	return linked() ? tr("player.cheats_are_off_while_linked_to") : cheats_refused();
}

void MdPlayer::add_cheat_code(const std::string &code, const std::string &name) { cheat_entered(true, code, name); }

void MdPlayer::ask_search_value(const std::string &title)
{
	search_ask_ = true;
	search_title_ = title;
	pf_->ask_cheat_text();
}


void MdPlayer::cheat_entered(bool ok, const std::string &code_in, const std::string &name)
{
	std::string code = trim(code_in);
	if (!ok || code.empty()) return;
	const int r = cheats_.add(code, trim(name));
	if (r < 0) {
		const bool md = system_ == MD_SYS_MD;
		const char *t = r == CHEAT_E_FORMAT ? (md ? tr("md.that_is_not_a_cheat_code")
		                                          : tr("md.that_is_not_a_cheat_code_2"))
		              : r == CHEAT_E_FULL ? tr("player.the_cheat_list_is_full")
		              : r == CHEAT_E_TOO_LONG ? tr("player.that_code_is_too_long_16")
		              : r == CHEAT_E_RANGE ? tr("md.that_code_points_at_an_address")
		              : r == CHEAT_E_EXISTS ? tr("md.that_code_is_already_in_the")
		              : tr("player.the_cheat_could_not_be_added");
		notice(t, NOTICE_ERROR);
	} else {
		cheats_apply();
		cheats_queue_save();
		notice(trf("player.cheat_added", cheats_.at(r).label.c_str()), NOTICE_OK);
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
	if (qsync_) { qsync_ = false; sync_menu_values(); }               /* Image quality and Video filter follow each other (never inside the row's own change) */
	ra_pump();
	poll_writer();
	if (ev == MENU_RESUME || ev == MENU_RESET_DONE) {
		mapper_.menu_closed(in.pads);
		if (settings_dirty_) settings_queue_save();
		srm_recheck_ = true;
		pacer_.reset(PACE_FULL);
		look_.reset_window();
		look_safe_point(LOOK_SAFE_MENU);
		if (ev == MENU_RESET_DONE) notice(tr("player.the_game_was_reset"), NOTICE_INFO);
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
	pl.set_int("pal_speed", pal_mode_);
	pl.set_int("look_mode", look_mode_);
	pl.set_int("quick_response", quick_);
	pl.set_int("no_sprite_flicker", flicker_ ? 1 : 0);
	pl.set_int("region", region_);
	pl.set_int("fm", fm_);
	pl.set_int("ym2413", ym2413_);
	pl.set_int("pad_type", pad_type_);
	pl.set_int("sms_border", sms_border_);
	pl.set_int("gg_lcd", gg_lcd_);
	pl.set_int("ram_cart", ram_cart_);
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

void MdPlayer::on_change(int row, int v)
{
	switch (row) {
	case ROW_PIC_SHAPE: ps_.shape = shape_to_pic(v); g_changed |= CH_SHAPE; break;
	case ROW_PIC_WIDE: ps_.wide = clampi(v, 0, 2); g_changed |= CH_WIDE; break;
	case ROW_PIC_QUALITY:
		ps_.scaler = scaler_of(v); look_mode_ = 0; g_changed |= CH_SCALER; qsync_ = true;
		ahead_.picture_reset();        /* another Image quality, another cost for the picture side */
		/* Original is stored as sharp, so the next start does not read "no scaler + Smooth" (an old file) and move it to Auto */
		if (v == IMGQ_ORIGINAL && ps_.quality != PIC_SHARP) { ps_.quality = PIC_SHARP; g_changed |= CH_QUALITY; }
		break;
	case ROW_FILTER:
		ps_.quality = v ? PIC_SMOOTH : PIC_SHARP; g_changed |= CH_QUALITY;
		ahead_.picture_reset();
		/* as in the main app: Original draws sharp bilinear whatever is stored, so choosing bilinear there switches the quality to Auto */
		if (v && imgq_of(ps_.scaler, look_mode_) == IMGQ_ORIGINAL) { ps_.scaler = SCALER_AUTO; look_mode_ = 0; g_changed |= CH_SCALER; }
		qsync_ = true;
		break;
	case ROW_PIC_SCANLINES: ps_.scan = clampi(v, 0, 10); g_changed |= CH_SCAN; break;
	case ROW_PIC_GRID: ps_.grid = clampi(v, 0, 10); g_changed |= CH_GRID; break;
	case ROW_PIC_GLOW: ps_.glow = clampi(v, 0, 10); g_changed |= CH_GLOW; break;
	case ROW_PIC_CRT: ps_.crt = clampi(v, 0, 10); g_changed |= CH_CRT; break;
	case ROW_PIC_SCREEN: case ROW_UI_TEXT: case ROW_UI_CONTRAST: case ROW_UI_LANGUAGE:
		/* shared by every console and the app: display.ini, written at once (a few bytes) */
		if (row == ROW_PIC_SCREEN) ui_.screen = pk_screen_clamp(v);
		else if (row == ROW_UI_LANGUAGE) ui_.language = clampi(v, 0, pk::LANG_CHOICES - 1);      /* the menu switched the texts already */
		else if (row == ROW_UI_TEXT) ui_.text_large = v ? 1 : 0;
		else ui_.contrast = v ? 1 : 0;
		ui_serial_++;
		if (!pk::uiprefs_save(root_, ui_)) notice(tr("player.could_not_save_the_screen_settings"), NOTICE_ERROR, false);
		return;
	case ROW_PIC_APPLY:
		ps_.use_game = v ? 1 : 0;
		/* "This game" stores every value as now shown; "All games" only what was changed here */
		g_changed |= CH_USEGAME | (v ? 0x4FFu : 0);
		break;
	case ROW_SCALER: ps_.scaler = clampi(v, 0, SCALER_CHOICES - 1); g_changed |= CH_SCALER; qsync_ = true; break;     /* no row any more: the launch file and the tests */
	case ROW_GG_COLORS: ps_.gg_colors = v ? 1 : 0; g_changed |= CH_GGCOL; break;
	case ROW_GG_LCD: gg_lcd_ = clampi(v, 0, 10); md_host_set_option("lcd", gg_lcd_ * 10); break;
	case ROW_SMS_BORDER: sms_border_ = clampi(v, 0, 2); md_host_set_option("sms_border", sms_border_); break;
	case ROW_PADTYPE: pad_type_ = clampi(v, 0, 2); md_host_set_pad_type(0, pad_type_); md_host_set_pad_type(1, pad_type_); break;
	case ROW_REGION: {
		const double before = md_host_fps();
		region_ = clampi(v, 0, 3);
		md_host_set_region(region_);
		if (md_host_fps() != before) { timing_changed_ = true; ahead_.set_frame_us((uint32_t) (1e6 / md_host_fps() + 0.5)); logf("region %d: %.4f Hz now\n", region_, md_host_fps()); }
		break;
	}
	case ROW_DISC: change_disc(v); break;
	case ROW_RAMCART: ram_cart_ = clampi(v, 0, 6); notice("The RAM cartridge changes when the game starts again", NOTICE_OK); break;
	case ROW_FM: fm_ = clampi(v, 0, 2); md_host_set_option("fm", fm_); break;
	case ROW_YM2413: ym2413_ = clampi(v, 0, 2); md_host_set_option("ym2413", ym2413_); break;
	case ROW_STEADY: steady_ = v != 0; pacer_.set_enabled(steady_); break;
	case ROW_HANDOFF: handoff_ = v != 0; break;
	case ROW_PALSPEED: pal_mode_ = pk::pal_mode_of(v); break;
	case ROW_LOOK: look_mode_ = clampi(v, 0, 2); qsync_ = true; break;       /* no row any more: the launch file and the tests */
	case ROW_QUICK: set_quick(v); break;
	case ROW_FLICKER: flicker_ = v != 0; md_host_set_option("sprite_limit", flicker_ ? 0 : 1); break;
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

void MdPlayer::on_reset()
{
	md_host_reset();
	ra_.reset();
	cheats_apply();
	speed_.state_changed();
	srm_recheck_ = true;
	jumps_++;
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
	if (ra_.hardcore_blocks()) return false;
	const int r = md_host_state_load_mem(data, n);
	ra_.state_loaded();
	srm_recheck_ = true;
	jumps_++;
	pacer_.reset(PACE_FULL);
	speed_.state_changed();              /* "a moment ago" is another timeline now */
	if (r == 0 && scd_ && discs_.size() > 1 && !disc_phase_) sync_disc_row();      /* the state's disc is in the drive now */
	return r == 0;
}

bool MdPlayer::current_frame(Frame *f) { return pf_->current_frame(f); }

int64_t MdPlayer::play_seconds() { return session_.seconds_played(); }

void MdPlayer::on_state_touched(int slot) { session_.note_state_touched(slot); }

void MdPlayer::on_controls_changed(WriteResult rc)
{
	if (rc != W_OK) { logf("controls NOT saved: %s\n", write_result_text(rc, true)); session_.note_notice(trf("menu.controls_save_failed", write_result_text(rc))); }
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
	{ "xBR 2x -> sharp", MdPlayer::ROW_SCALER, 0.20f, 45 },
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
	if (light_on(look_mode_, ps_.scaler)) n.scaler = SCALER_OFF;    /* Light: no scaler, fixed */
	if (n.scaler == SCALER_OFF) {
		n.quality = PIC_SHARP;               /* Original draws sharp bilinear whatever Video filter holds (as in the main app) */
	} else if (look_on_ && look_mode_ == 0) {
		n.tier_steps = look_.level();
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
	const int rungs = 2 - look_ceiling_;
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
	if (speed_.busy() || speed_was_busy_) { look_.reset_window(); return; }     /* fast forward / rewind seconds are no measure of the look */
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
	speed_.stop();
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
	/* A linked game writes none, except the host after an agreed end (its battery file was just written by the session; the machine is
	 * at the session's last frame, which is the point to keep). */
	if (states_ && md_host_state_size() && (!linked() || (req_.link.is_host() && link_result_ == "done"))) resume_post(true);
	wait_writer();
	if (!linked() && md_host_sram_dirty()) {
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
	rep.link_result = link_result_;
	rep.link_error = link_err_;
	rep.link_error_name = link_err_name_;
	rep.link_frames = link_frames_;
	rep.link_peer_reason = link_peer_reason_;
	WriteResult w = session_write(root_, rep);
	logf("session: %lld s played, save_changed=%d save_error=%d states=%d resume=%d exit=%d -> launch/session.txt %s\n",
	     (long long) rep.seconds_played, rep.save_changed ? 1 : 0, rep.save_error ? 1 : 0, (int) rep.states_touched.size(),
	     rep.resume_present ? 1 : 0, (int) why, w == W_OK ? "written" : write_result_text(w, true));
	ra_.shutdown();                  /* an unlock earned a moment ago gets up to three seconds to reach the server */
	log_flush(true);
}

/* ---- wide view ----------------------------------------------------------------------------------------------- */

void MdPlayer::wide_apply(int scr_w, int scr_h)
{
	md_video_info vi;
	md_host_video_info(&vi);
	const pic_info gi = pic_info_from_video(&vi);
	/* a Game Gear's band is 256 columns (48 beside the 160 of its LCD); the others' strips are at most MD_WIDE_MAX columns */
	const int mx = system_ == MD_SYS_GG ? 48 : MD_WIDE_MAX;
	const int m = pic_wide_margin(&eff_, &gi, scr_w, scr_h, mx);
	/* Owner decision: the wide view is on by default and is never switched off to save time; what it costs is held down by narrowing its real
	 * strips (main.cpp's ladder, rung 0..2) and finally showing only the soft extension. A linked game never widens its picture. */
	const bool want = m > 0 && eff_.wide != 2 && !linked();
	if (want == wide_on_ && (!want || m == wide_m_)) return;
	if (system_ == MD_SYS_GG) md_host_set_option("gg_band", want ? 1 : 0);
	wide_on_ = want; wide_m_ = want ? m : 0;
	logf("wide view %s margin=%d (setting %d, shape %d)\n", want ? "on" : "off", wide_m_, eff_.wide, eff_.shape);
}

}  // namespace mdp
