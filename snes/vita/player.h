/*
 * The SNES player's behaviour, without the console: launch protocol, the
 * playerkit menu and its host, save states, battery RAM writes, cheats, the
 * resume point, steady pacing, the session report and the log. Portable C++
 * (no Vita calls) so the Linux twin runs exactly this code with the software
 * renderer; snes/vita/main.cpp adds the console's screen, sound, pads, touch,
 * keyboard, clocks and threads around it.
 *
 * Licence: links only snes/ and playerkit/ code (Snes9x is not GPL compatible:
 * nothing from gpsp/ may be linked into snes.bin).
 *
 * Copyright CBOC-Vita. MIT licence (see snes/frontend/LICENSE).
 */
#ifndef SNES_PLAYER_H
#define SNES_PLAYER_H

#include <stdint.h>
#include <string>
#include <vector>

#include "host.h"
#include "picture.h"
#include "pk_launch.h"
#include "pk_menu.h"
#include "pk_input.h"
#include "pk_states.h"
#include "pk_notice.h"
#include "pk_pace.h"

namespace snesp {

/* What the console side gives the player. */
class Platform {
public:
	virtual ~Platform() {}
	virtual uint64_t now_us() = 0;
	virtual void pause_us(unsigned us) = 0;
	/* the writer thread has work (signal it); a platform without a writer thread returns false and the
	 * player runs the work itself, at once */
	virtual bool wake_writer() = 0;
	/* the picture now on screen (RGB565), for the state pictures; false when there is none */
	virtual bool current_frame(pk::Frame *f) = 0;
	/* "Add a cheat": ask for the code (and a name) with the keyboard; the answer comes back through
	 * SnesPlayer::cheat_entered(). */
	virtual void ask_cheat_text() = 0;
	/* picture settings changed (the drawing code may keep caches) */
	virtual void picture_changed() {}
	/* append text to the log file (called on the writer thread, or the main thread at the end) */
	virtual void log_write(const std::string &chunk) = 0;
};

/* One piece of work for the writer thread. */
struct WriterJob {
	enum Kind { SRM = 1, STATE, RESUME, SETTINGS, LOG } kind;
	pk::StateJob state;
	std::vector<uint8_t> bytes;
	std::string text;
	pic_settings ps;
	std::string path, path2;
	int result = 0;
	pk::SaveReport rep;
	bool quick = false;            /* a quick save (its notice is the player's, the menu may be closed) */
	WriterJob() : kind(SRM) {}
};

/* A queue the writer thread empties, in order. Shared with the game thread through a spin lock (the
 * Vita has no usable mutex); jobs themselves are never touched by both at once. */
class Writer {
public:
	void post(WriterJob *j);
	/* the writer thread: run one job; false when there was none */
	bool work_one(class SnesPlayer *p);
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
};

class SnesPlayer : public pk::MenuHost {
public:
	enum { ROW_PIXELS = pk::ROW_HOST_BASE + 1, ROW_HIRES, ROW_INTERLACE, ROW_MULTITAP, ROW_STEADY };
	explicit SnesPlayer(Platform *pf);
	~SnesPlayer();

	/* ---- start-up ----
	 * `root`: the app's data folder ("ux0:data/GBAdhoc"). `system_enter_cross`: the console's own
	 * Enter button, used when there is no launch file. Reads launch/snes.ini (or the arguments), moves
	 * old states, brings in a RetroArch save, applies a patch, loads the game, reads cheats, controls and
	 * picture settings. */
	StartResult start(int argc, char **argv, const std::string &root, bool system_enter_cross);
	/* after start: a resume point or a slot asked for by the launch file (call once, before the first frame) */
	void apply_start_mode();
	/* the writer thread's loop body (also run by the player itself when there is no thread) */
	Writer &writer() { return writer_; }
	void run_writer_job(WriterJob *j);

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
	const pic_settings &picture() const { return ps_; }
	const pk::LaunchRequest &request() const { return req_; }
	bool have_launch_file() const { return have_launch_; }
	const std::string &game_name() const { return game_; }
	const std::string &root() const { return root_; }
	const std::string &rom_path() const { return rom_; }
	const std::string &srm_path() const { return srm_; }
	pk::States *states() { return states_; }
	pk::SessionTracker &session() { return session_; }
	const pk::Profile &profile() const { return profile_; }
	const pk::InputResult &last_input() const { return in_; }
	bool steady_pacing() const { return steady_; }
	bool save_failing() const { return save_failing_; }
	const std::string &save_message() const { return save_msg_; }
	void logf(const char *fmt, ...) __attribute__((format(printf, 2, 3)));
	void log_flush(bool now);
	/* the per-second pacing figures for the log line */
	std::string pace_line();
	/* migrate old states (also called by start) */
	static int migrate_old_states(const std::string &root, const std::string &game, std::string *log);
	int multitap_wanted() const;
	/* waits for the writer (the game thread, before a load/delete; at the end) */
	void wait_writer();

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
	void sync_menu_values();
	void srm_watch(uint64_t now_us);
	bool srm_post();
	void srm_result(int r, const char *how, uint64_t now_us);
	void resume_post(bool sync);
	void quick_save();
	void quick_load();
	void notice(const std::string &t, pk::NoticeKind k, bool report_to_app = false);
	std::string pic_game_ini() const;

	Platform *pf_;
	Writer writer_;
	pk::LaunchRequest req_;
	bool have_launch_ = false;
	std::string root_, snes_dir_, game_, rom_, srm_;
	pic_settings ps_;
	bool settings_dirty_ = false;
	bool steady_ = true;
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
	/* battery RAM */
	std::vector<uint8_t> srm_buf_;
	bool srm_busy_ = false;
	bool save_failing_ = false;
	std::string save_msg_;
	uint64_t srm_retry_at_ = 0, srm_dirty_since_ = 0, srm_last_change_ = 0, srm_last_write_ = 0;
	unsigned srm_backoff_s_ = 10;
	unsigned srm_pass_ = 0;
	uint64_t srm_hash_ = 0;
	bool srm_hash_valid_ = false, srm_dirty_ = false, srm_recheck_ = true;
	int srm_writes_ = 0;
	/* resume point */
	bool resume_busy_ = false;
	uint64_t resume_after_srm_at_ = 0;
	uint64_t now_ms_ = 0, prev_frame_us_ = 0;
	/* log */
	std::string logbuf_;
	uint64_t log_flushed_us_ = 0;
	bool log_busy_ = false;
	unsigned log_lost_ = 0;
	/* menu */
	bool menu_cheats_changed_ = false;
	pk::PaceMode pace_logged_ = pk::PACE_FULL;
};

}  // namespace snesp

#endif
