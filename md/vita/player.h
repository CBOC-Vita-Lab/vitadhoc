/*
 * The Sega player's behaviour, without the console: launch protocol, the playerkit menu and its host, save states, battery RAM
 * writes, cheats, the resume point, steady pacing, the look governor, the session report and the log. One player for the Mega
 * Drive / Genesis, Master System, Game Gear and SG-1000 (console ids md, sms, gg, sg). Portable C++ (no Vita calls) so the Linux
 * twin runs exactly this code with the software renderer; md/vita/main.cpp adds the console's screen, sound, pads, touch,
 * keyboard, clocks and threads around it.
 *
 * Licence: links only md/, playerkit/ and widekit/ code (Genesis Plus GX has a non-commercial licence: nothing from gpsp/ may be
 * linked into md.bin).
 *
 * Hooks left for features that plug in here (all are named, none is half done):
 *   rewind / fast forward   set_extra_caps(CAP_REWIND | CAP_FF ...) before start(); the menu then shows the rows; the game loop (main.cpp,
 *                           twin) calls md_host_set_render(0) for skipped frames; Mapper's ACT_FF / ACT_REWIND are in in_.fired / in_.held.
 *   RetroAchievements       md_host_ra_read / md_host_ra_console give the memory map (host.h); the app side module plugs into
 *                           after_frame() (once per emulated frame, game thread) and notice() for toasts.
 *   two players / online    set_extra_caps(CAP_PLAY_TOGETHER); until then the card is not shown. start() already reads req_.link.
 *   wide view               on_change(ROW_PIC_WIDE) and pic_geometry() are the place; the row is greyed with its reason meanwhile.
 *
 * Copyright CBOC-Vita. MIT licence (see md/frontend/LICENSE).
 */
#ifndef MD_PLAYER_H
#define MD_PLAYER_H

#include <stdint.h>
#include <string>
#include <vector>

#include "host.h"
#include "picture.h"
#include "cheats.h"
#include "savefile.h"
#include "pk_launch.h"
#include "pk_menu.h"
#include "pk_input.h"
#include "pk_states.h"
#include "pk_notice.h"
#include "pk_pace.h"
#include "pk_look.h"

namespace mdp {

/* What the console side gives the player. */
class Platform {
public:
	virtual ~Platform() {}
	virtual uint64_t now_us() = 0;
	virtual void pause_us(unsigned us) = 0;
	/* the writer thread has work (signal it); a platform without a writer thread returns false and the player runs the work itself, at once */
	virtual bool wake_writer() = 0;
	/* the picture now on screen (RGB565), for the state pictures; false when there is none */
	virtual bool current_frame(pk::Frame *f) = 0;
	/* "Add a cheat": ask for the code (and a name) with the keyboard; the answer comes back through MdPlayer::cheat_entered(). */
	virtual void ask_cheat_text() = 0;
	/* picture settings changed (the drawing code may keep caches) */
	virtual void picture_changed() {}
	/* append text to the log file (called on the writer thread, or the main thread at the end) */
	virtual void log_write(const std::string &chunk) = 0;
};

/* One piece of work for the writer thread. */
struct WriterJob {
	enum Kind { SRM = 1, STATE, RESUME, SETTINGS, LOG, LOOKMEM, CHEATS } kind;
	pk::StateJob state;
	std::vector<uint8_t> bytes;
	std::string text;
	pic_settings ps;
	std::string path, path2;
	int result = 0;
	pk::SaveReport rep;
	bool remove_file = false;      /* LOOKMEM: delete `path` instead of writing `text` */
	bool quick = false;            /* a quick save (its notice is the player's, the menu may be closed) */
	WriterJob() : kind(SRM) {}
};

/* A queue the writer thread empties, in order. Shared with the game thread through a spin lock (the Vita has no usable mutex); jobs
 * themselves are never touched by both at once. */
class Writer {
public:
	void post(WriterJob *j);
	/* the writer thread: run one job; false when there was none */
	bool work_one(class MdPlayer *p);
	/* finished jobs, oldest first (the game thread takes them) */
	void take_done(std::vector<WriterJob *> &out);
	bool idle();
	int pending();
private:
	pk::SpinLock lk_;
	std::vector<WriterJob *> todo_, done_;
	int running_ = 0;
};

struct StartResult {
	bool ok = false;
	std::string fatal1, fatal2;    /* why the game cannot start, for the screen */
	int cpu_mhz = 0;               /* 0 = the player's default (500, else 444) */
	bool enter_is_cross = true;
	std::string console;           /* md, sms, gg or sg */
};

class MdPlayer : public pk::MenuHost {
public:
	enum { ROW_SCALER = pk::ROW_HOST_BASE + 1, ROW_SMS_BORDER, ROW_GG_COLORS, ROW_GG_LCD, ROW_PADTYPE, ROW_REGION, ROW_FM, ROW_YM2413, ROW_STEADY,
	       ROW_HANDOFF = pk::ROW_HOST_BASE + 40, ROW_PALSPEED = pk::ROW_HOST_BASE + 41, ROW_LOOK = pk::ROW_HOST_BASE + 42 };
	explicit MdPlayer(Platform *pf);
	~MdPlayer();

	/* extra menu capabilities of a feature a later part wires in (see the hooks above); before start() */
	void set_extra_caps(uint32_t caps) { extra_caps_ = caps; }

	/* ---- start-up ----
	 * `root`: the app's data folder ("ux0:data/GBAdhoc"). `system_enter_cross`: the console's own Enter button, used when there is no
	 * launch file. Reads launch/<console>.ini (md, sms, gg or sg: the one whose game matches the argument, else the newest) or the
	 * arguments, moves nothing old (there is no earlier Sega player), brings in a RetroArch save, applies a patch, loads the game, reads
	 * cheats, controls and picture settings. */
	StartResult start(int argc, char **argv, const std::string &root, bool system_enter_cross);
	/* after start: a resume point or a slot asked for by the launch file (call once, before the first frame) */
	void apply_start_mode();
	Writer &writer() { return writer_; }
	void run_writer_job(WriterJob *j);

	/* ---- look governor (playerkit/INTEGRATION.md section 8; the ladder is the pixel art scaler's tiers, md/vita/README.md) ----
	 * Every game starts with the best look: the pixel art scaler tier of the system's ceiling (Game Gear xBR 3x, Master System and
	 * H32 Mega Drive xBR 2x, H40 Mega Drive MMPX 2x). When this game cannot hold full speed with it, the next tier down is taken
	 * (remembered for the game, <root>/look/<console>/..), and given back when there is room. A scaler chosen by hand in the Pixel
	 * art scaler row is never eased. picture() is the look to show: the settings with the governor's steps applied (tier_steps). */
	void look_start(int mhz);
	void look_second(const pk::LookSecond &s);
	void look_safe_point(pk::LookSafe k);
	void look_reset_window() { look_.reset_window(); }
	const pk::LookGovernor &look() const { return look_; }
	int look_ceiling_tier() const { return look_ceiling_; }

	/* ---- every frame while the game runs ---- */
	/* pads and a finished tap on the game picture; returns true when the menu should open */
	bool input(const pk::PadSample pads[pk::MAX_PLAYERS], bool tapped, uint64_t now_ms, uint32_t frame);
	/* draw (render) emulated frame number `frame`? (steady pacing: even frames only in half mode) */
	bool want_render(uint32_t frame) const;
	pk::PaceMode pace_mode() const { return pacer_.mode(); }
	/* cost of a drawn frame (us): the frame's own work, for the pacing decision */
	void drawn_cost(uint32_t cost_us);
	/* after each emulated frame (battery RAM watch, resume point, finished writes, notices) */
	void after_frame(uint64_t now_us);
	/* the Vita is about to sleep: write the battery RAM now */
	void suspend();

	/* ---- the menu ---- */
	void open_menu(int player);
	bool menu_open() const { return menu_.is_open(); }
	/* returns MENU_GAME_LIST / MENU_EXIT when the player chose to leave */
	pk::MenuEvent menu_update(const pk::MenuInput &in);
	void menu_draw(pk::Gfx *g, pk::Text *t) { menu_.draw(g, t); }
	void menu_drawn();      /* the menu's first frame is on screen: now the battery RAM may be written */
	pk::Menu &menu() { return menu_; }
	void cheat_entered(bool ok, const std::string &code, const std::string &name);

	/* in-game notices (drawn by the platform over the game) */
	pk::Notices &notices() { return menu_.notices(); }

	/* ---- the end ---- */
	/* resume point, battery RAM, settings, log, session.txt; afterwards the platform starts the app */
	void finish(pk::ExitReason why);

	/* ---- for the platform and tests ---- */
	const pic_settings &picture() const { return eff_; }   /* ps_ with the look governor applied */
	const pk::LaunchRequest &request() const { return req_; }
	bool have_launch_file() const { return have_launch_; }
	const std::string &console() const { return console_; }
	const std::string &game_name() const { return game_; }
	const std::string &root() const { return root_; }
	const std::string &rom_path() const { return rom_; }
	const std::string &srm_path() const { return srm_; }
	pk::States *states() { return states_; }
	pk::SessionTracker &session() { return session_; }
	const pk::Profile &profile() const { return profile_; }
	const pk::InputResult &last_input() const { return in_; }
	bool steady_pacing() const { return steady_; }
	/* "Smooth hand-over" (Speed page, default on): pictures are drawn and presented by their own thread through playerkit's mailbox
	 * (pipe.h) so the game never waits for the picture. Ignored while the platform has no such thread. */
	bool smooth_handoff() const { return handoff_; }
	/* "PAL games speed" (Speed page, key pal_speed): false = true 50 Hz (default: 5 frames in 6 refreshes), true = Fast 60 Hz (one
	 * frame per refresh, the game and its sound run 20 % fast). Read by the platform each pass. */
	bool pal_fast() const { return pal_fast_; }
	/* "Image look" (Picture page, key look_mode): 0 Auto (the governor eases the scaler only when the game cannot hold full speed),
	 * 1 Full (the scaler as set, never eased), 2 Light (no scaler, never eased) */
	int look_mode() const { return look_mode_; }
	/* the region or a forced vdp mode changed the frame rate: the platform sets its pacing again (cleared by the call) */
	bool take_timing_changed() { bool t = timing_changed_; timing_changed_ = false; return t; }
	bool save_failing() const { return save_failing_; }
	const std::string &save_message() const { return save_msg_; }
	void logf(const char *fmt, ...) __attribute__((format(printf, 2, 3)));
	void log_flush(bool now);
	/* the per-second pacing figures for the log line */
	std::string pace_line();
	/* waits for the writer (the game thread, before a load/delete; at the end) */
	void wait_writer();
	/* the cheats of the game (tests) */
	const CheatList &cheats() const { return cheats_; }
	/* console id of a game file's extension: gg, sms, sg or md */
	static std::string console_for_extension(const std::string &path);
	/* the launch file to use among md / sms / gg / sg: the one whose game is `arg_rom`, else the newest; "" when none */
	static std::string pick_launch(const std::string &root, const std::string &arg_rom);

	/* MenuHost */
	void on_change(int row_id, int value);
	void on_reset();
	bool capture_state(std::vector<uint8_t> &out);
	bool restore_state(const uint8_t *data, size_t n);
	bool current_frame(pk::Frame *f);
	int64_t play_seconds();
	void list_cheats(std::vector<pk::CheatItem> &out);
	void set_cheat(int index, bool on);
	void on_state_touched(int slot);
	void on_controls_changed(pk::WriteResult rc);
	void add_cheat();
	bool delete_cheat(int index);
	bool queue_state_write(pk::StateJob &job);
	void wait_state_writes() { wait_writer(); }

private:
	void post(WriterJob *j);
	void poll_writer();
	void settings_load();
	void settings_queue_save();
	void apply_overrides();
	void apply_core_options();
	void sync_menu_values();
	void srm_watch(uint64_t now_us);
	bool srm_post();
	void srm_result(int r, const char *how, uint64_t now_us);
	void resume_post(bool sync);
	void quick_save();
	void quick_load();
	void cheats_apply();
	void cheats_queue_save();
	void notice(const std::string &t, pk::NoticeKind k, bool report_to_app = false);
	std::string pic_game_ini() const;
	std::string cfg_dir() const;
	void look_sync_rows();
	void look_apply(const char *why);
	void look_save(bool final_);

	Platform *pf_;
	Writer writer_;
	SafeFile srm_file_, cht_file_;      /* used by the writer thread (and the game thread after wait_writer) */
	pk::LaunchRequest req_;
	bool have_launch_ = false;
	std::string root_, console_ = "md", game_, rom_, srm_;
	int system_ = MD_SYS_MD;
	pic_settings ps_;              /* what the player chose (and what the files hold) */
	pic_settings eff_;             /* ps_ with the governor's steps: what is shown */
	/* the settings the menu's own rows do not cover; stored in <console>/player.ini */
	int region_ = 0, fm_ = 0, ym2413_ = 2, pad_type_ = 0, sms_border_ = 0, gg_lcd_ = 0;
	bool timing_changed_ = false;
	pk::LookGovernor look_;
	bool look_on_ = false;
	std::string look_path_, look_written_;
	int look_mhz_ = 0, look_ceiling_ = 3;
	uint32_t look_gen_ = 0;
	bool look_busy_ = false;
	bool settings_dirty_ = false;
	bool steady_ = true;
	bool handoff_ = true;
	bool pal_fast_ = false;
	bool look_bad_ = false;
	int look_mode_ = 0;
	uint32_t extra_caps_ = 0;
	pk::Pacer pacer_;
	pk::States *states_ = 0;
	pk::SessionTracker session_;
	pk::Mapper mapper_;
	pk::Profile profile_;
	pk::ProfileStore *store_ = 0;
	pk::Menu menu_;
	pk::InputResult in_;
	pk::PadSample last_pads_[pk::MAX_PLAYERS];
	bool menu_srm_pending_ = false;
	CheatList cheats_;
	bool cheats_busy_ = false;
	/* battery RAM */
	std::vector<uint8_t> srm_buf_;
	bool srm_busy_ = false;
	bool save_failing_ = false;
	std::string save_msg_;
	uint64_t srm_retry_at_ = 0, srm_dirty_since_ = 0, srm_last_change_ = 0, srm_last_write_ = 0;
	unsigned srm_backoff_s_ = 10;
	unsigned srm_pass_ = 0;
	bool srm_dirty_ = false, srm_recheck_ = true;
	uint64_t srm_hash_ = 0;
	bool srm_hash_valid_ = false;
	int srm_writes_ = 0;
	std::string srm_err_;
	/* resume point */
	bool resume_busy_ = false;
	uint64_t resume_after_srm_at_ = 0;
	uint64_t now_ms_ = 0, prev_frame_us_ = 0;
	/* log */
	std::string logbuf_;
	pk::SpinLock log_lk_;
	uint64_t log_flushed_us_ = 0;
	bool log_busy_ = false;
	unsigned log_lost_ = 0;
	/* menu */
	pk::PaceMode pace_logged_ = pk::PACE_FULL;
};

}  // namespace mdp

#endif
