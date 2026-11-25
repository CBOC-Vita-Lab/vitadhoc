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
 * Rewind and fast forward (playerkit pk_speed / pk_rewind / pk_ff) are built in: the game loop (main.cpp, twin) asks speed_plan() how many
 * frames to run this refresh, runs each with run_frame(), and passes the core's audio through audio_filter().
 *
 * Hooks left for features that plug in here (all are named, none is half done):
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
#include "pk_palsmooth.h"
#include "pk_launch.h"
#include "pk_menu.h"
#include "pk_input.h"
#include "pk_states.h"
#include "pk_notice.h"
#include "pk_pace.h"
#include "pk_look.h"
#include "pk_uiprefs.h"
#include "pk_speed.h"
#include "pk_ahead.h"
#include "pk_ra.h"

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
	std::string console;           /* md, sms, gg, sg or scd (Sega CD) */
};

class MdPlayer : public pk::MenuHost {
public:
	enum { ROW_SCALER = pk::ROW_HOST_BASE + 1, ROW_SMS_BORDER, ROW_GG_COLORS, ROW_GG_LCD, ROW_PADTYPE, ROW_REGION, ROW_FM, ROW_YM2413, ROW_STEADY,
	       ROW_REWIND_LEN = pk::ROW_HOST_BASE + 30, ROW_HANDOFF = pk::ROW_HOST_BASE + 40, ROW_PALSPEED = pk::ROW_HOST_BASE + 41, ROW_LOOK = pk::ROW_HOST_BASE + 42,
	       ROW_QUICK = pk::ROW_HOST_BASE + 43, ROW_FLICKER = pk::ROW_HOST_BASE + 44, ROW_RAMCART = pk::ROW_HOST_BASE + 45, ROW_FILTER = pk::ROW_HOST_BASE + 46, ROW_DISC = pk::ROW_HOST_BASE + 47 };
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
	 * Mega Drive xBR 2x; MMPX 2x is only a look picked by hand). When this game cannot hold full speed with it, the next tier down is taken
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
	/* ---- fast forward and rewind (playerkit pk_speed) ----
	 * speed_plan: once per display refresh, before the frames; `normal` = the frames this refresh would run (1, or what the pacing says).
	 * Fast forward multiplies it (2x or 3x), rewinding makes it 1 (a landing or a standing picture). run_frame replaces md_host_run_frame
	 * (it captures the state for rewind); true when the core ran a frame. audio_filter works in place on the core's audio and returns the
	 * frames to keep (mixed down under fast forward, none while rewinding). speed_draws: draw frame `i` of `n` this refresh?
	 * speed_busy: fast forward is on or the game is rewinding (the picture hand-over drops, nothing waits). */
	int speed_plan(int normal);
	bool run_frame(bool render);
	size_t audio_filter(int16_t *lr, size_t frames);
	bool speed_draws(int i, int n) const { return speed_.draw_this(i, n); }
	bool speed_busy() const { return speed_.busy(); }
	/* the main thread's non-emulation time of a refresh (Unlimited fast forward takes it off its budget) */
	void speed_other_us(uint32_t us) { speed_.note_other_us(us); }
	/* the helper thread's step (called by the writer thread when it has nothing else): packs the newest rewind state */
	bool speed_pack() { return speed_.pack(); }
	/* two-player or online play, Hardcore: rewind and fast forward are off (a press gives `why` as its notice) */
	void set_speed_locked(bool locked, const char *why = 0) { speed_.set_locked(locked, why); }
	const pk::Speed &speed() const { return speed_; }
	pk::PaceMode pace_mode() const { return pacer_.mode(); }
	/* ---- "Quick response": run-ahead by one frame (md_host_run_frame_ahead, playerkit pk_ahead.h) ----
	 * run_frame() runs a drawn frame ahead when the governor wants it. The platform gives the hook that must run while the machine is
	 * the one of the picture (the wide view's analysis), and reports the emulating thread's time for every frame at normal speed. */
	void set_ahead_hook(void (*fn)(void *), void *user) { ahead_hook_ = fn; ahead_user_ = user; }
	void ahead_cost(uint32_t emu_us);
	/* once per refresh with the hand-over: the drawing thread's average work for the pictures it finished (0: none) and the refreshes the picture side missed;
	 * Auto holds off and stops while it runs late (pk_ahead.h add_picture) */
	void ahead_picture(uint32_t draw_us, uint32_t missed);
	void ahead_picture_reset() { ahead_.picture_reset(); }
	bool ahead_ran() const { return ahead_ran_; }           /* the last run_frame() ran ahead */
	int quick() const { return quick_; }
	void set_quick(int mode);                               /* 0 Off, 1 Auto, 2 On */
	const pk::AheadGovernor &ahead() const { return ahead_; }
	std::string ahead_line();                               /* one census line for the log; counters start over */
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
	/* "Search memory": after add_cheat's keyboard hook asked, the platform asks for the number instead when this says so */
	bool search_value_wanted(std::string *title) { if (!search_ask_) return false; search_ask_ = false; if (title) *title = search_title_; return true; }
	void search_value_entered(bool ok, const std::string &text) { menu_.search_value_entered(ok, text); }

	/* in-game notices (drawn by the platform over the game) */
	pk::Notices &notices() { return menu_.notices(); }

	/* ---- a linked two-player game (netplay.h) ----
	 * The launch file asked for a link: the game starts new, nothing the link computes is saved (no battery RAM watch, no resume
	 * point, no states, no cheats, no rewind or fast forward), the options that change what a game can see are pinned, and the
	 * player's pad 1 is only the LOCAL player's buttons (the session feeds both pads of the one machine). The loop steps
	 * mdnet::Link instead of md_host_run_frame(). Every frame is drawn: the picture work leaves traces in the machine (the sprite
	 * overflow and collision flags a game can read), so two consoles must draw alike to stay identical. */
	bool linked() const { return req_.link.active(); }
	/* a file holding exactly the image the machine runs (the session's identity): the game file itself, or a copy under
	 * <root>/launch/ when the image is patched; "" when not linked */
	const std::string &link_rom() const { return link_rom_; }
	/* the local player's buttons of the last input() (the 12 bit mask of host.h) */
	unsigned link_pad() const { return link_pad_; }
	/* how the link ended, for session.txt and the app's message */
	void set_link_result(const std::string &result, int err, const std::string &err_name, int frames)
	{
		link_result_ = result; link_err_ = err; link_err_name_ = err_name; link_frames_ = frames;
	}
	void set_link_peer_reason(int r) { link_peer_reason_ = r; }
	/* the host's save after an agreed end: written through the same safe file as every battery save; 0 = written */
	int link_commit_save(const uint8_t *data, size_t n);

	/* ---- the end ---- */
	/* resume point, battery RAM, settings, log, session.txt; afterwards the platform starts the app */
	void finish(pk::ExitReason why);

	/* ---- for the platform and tests ---- */
	const pic_settings &picture() const { return eff_; }   /* ps_ with the look governor applied */
	/* Screen, Text size, High contrast: the settings every console shares (display.ini, pk_uiprefs.h); ui_serial() counts changes */
	const pk::UiPrefs &ui() const { return ui_; }
	unsigned ui_serial() const { return ui_serial_; }

	/* ---- wide view (the Wide view row: Auto, On, Off; md/frontend/wide.h does the work) ----
	 * wide_apply() is called once per refresh with the screen's size: it works out the margin the picture's shape wants (pic_wide_margin) and
	 * whether the view is on (not Off, not a linked game), and switches the Game Gear's 256 wide band on or off. The main loop draws the strips
	 * and composes while wide_active(). wide_mode(): 0 Auto (the cost ladder), 1 On (always the full strips), 2 Off. wide_jumps() counts the
	 * state loads, rewinds and resets after which the analysis must start again. */
	void wide_apply(int scr_w, int scr_h);
	bool wide_active() const { return wide_on_; }
	int wide_margin() const { return wide_m_; }
	int wide_mode() const { return eff_.wide; }
	unsigned wide_jumps() const { return jumps_; }
	const pk::LaunchRequest &request() const { return req_; }
	bool have_launch_file() const { return have_launch_; }
	const std::string &console() const { return console_; }
	const std::string &game_name() const { return game_; }
	const std::string &root() const { return root_; }
	const std::string &rom_path() const { return rom_; }
	const std::string &srm_path() const { return srm_; }
	/* Sega CD, a game on several discs (an .m3u): the tray opens, the disc is swapped and the tray closes over the next frames (disc_tick, once per frame); the Change disc row calls it */
	void change_disc(int index);
	int disc_phase() const { return disc_phase_; }
	static int disc_tray_frames() { return kDiscTrayFrames; }
	const std::vector<std::string> &discs() const { return discs_; }
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
	bool pal_fast() const { return pal_mode_ == 1; }
	/* Smooth 50 Hz (the default, key pal_speed 2): the same 5 frames in 6 refreshes, the sixth's frame run one refresh early and shown mixed with the one before (pk_palsmooth.h) */
	bool pal_smooth() const { return pal_mode_ == 2; }
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
	/* Sega CD: hand the core the player's own BIOS dumps (never shipped): the app's bios folder, the folder of the game; returns how many regions have one */
	static int load_cd_bios(const std::string &root, const std::string &game_path, std::string *log);
	/* the launch file to use among md / sms / gg / sg: the one whose game is `arg_rom`, else the newest; "" when none */
	static std::string pick_launch(const std::string &root, const std::string &arg_rom);

	/* RetroAchievements (pk_ra): the web answers are collected once per display refresh, in game and in the menu; the twin points `server` at a
	 * fake one before start() (never set on the console: the real site). */
	void ra_pump();
	void set_ra_server(const std::string &server) { ra_server_ = server; }
	const pk::Ra &ra() const { return ra_; }

	/* MenuHost */
	const char *state_load_refused();
	const char *cheats_refused();
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
	bool search_setup(pk_cs *cs);
	void add_cheat_code(const std::string &code, const std::string &name);
	void ask_search_value(const std::string &title);
	const char *search_refused();
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
	pk::UiPrefs ui_;               /* display.ini: Screen, Text size, High contrast */
	unsigned ui_serial_ = 0;
	pic_settings ps_;              /* what the player chose (and what the files hold) */
	pic_settings eff_;             /* ps_ with the governor's steps: what is shown */
	bool wide_on_ = false;         /* the wide view is on (wide_apply) */
	int wide_m_ = 0;               /* its margin per side, in console pixels */
	unsigned jumps_ = 0;           /* state loads, rewinds, resets */
	/* the settings the menu's own rows do not cover; stored in <console>/player.ini */
	int region_ = 0, fm_ = 0, ym2413_ = 2, pad_type_ = 0, sms_border_ = 0, gg_lcd_ = 0;
	int ram_cart_ = 0;             /* Sega CD: the RAM cartridge (0 none, 1..6 = 16 KB .. 512 KB); read when the disc is loaded */
	bool scd_ = false;             /* the game is a Sega CD disc (console "scd") */
	std::vector<std::string> discs_;   /* an .m3u game: the discs of the list, in order (empty for a single disc) */
	int disc_want_ = 0, disc_phase_ = 0, disc_wait_ = 0;   /* Change disc: 0 idle, 1 tray open (the disc goes in when the wait ends), 2 disc in (the tray closes when the wait ends) */
	static const int kDiscTrayFrames = 60;                /* about a second of the game's own time for each step */
	void disc_tick();
	void sync_disc_row();
	/* Speed page: fast forward 0 = 2x 1 = 3x, with sound, hold or toggle; rewind on, its length (0 = 30 s, 1 = 60 s) and memory (MB) */
	bool qsync_ = false;       /* Image quality / Video filter rows to be brought in line after the menu's own change */
	int ff_speed_ = 0, ff_sound_ = 1, ff_toggle_ = 0, rewind_on_ = 1, rewind_len_ = 1, rewind_mb_ = 12;
	struct SpeedGlue : pk::SpeedHost {
		MdPlayer *p;
		explicit SpeedGlue(MdPlayer *pl) : p(pl) {}
		size_t state_size();
		bool save_state(uint8_t *buf, size_t cap);
		bool load_state(const uint8_t *buf, size_t n);
		void run_frame(bool render);
		void clear_pads();
		uint64_t now_us();
		static void wake(void *self);
	} glue_;
	pk::Speed speed_;
	bool speed_was_busy_ = false;
	pk::Ra ra_;
	std::string ra_server_;
	bool search_ask_ = false;                 /* ask_search_value: the keyboard is for a number */
	std::string search_title_;
	bool ra_blocks_ = false;               /* Hardcore is in force (or not known yet): no rewind, no cheats, no state loads */
	uint64_t ra_toast_until_ = 0;
	void ra_begin(const uint8_t *rom, size_t n);
	void ra_update();
	static uint32_t ra_read(void *ud, uint32_t addr, uint8_t *out, uint32_t n);
	static uint64_t ra_now(void *ud);
	static int ra_online(void *ud);
	static void ra_log(void *ud, const char *line);
	std::string chip_text_;
	uint64_t chip_at_ms_ = 0;
	pk::SpeedSettings speed_settings() const;
	void speed_start();
	bool timing_changed_ = false;
	pk::LookGovernor look_;
	bool look_on_ = false;
	std::string look_path_, look_written_;
	int look_mhz_ = 0, look_ceiling_ = 2;
	uint32_t look_gen_ = 0;
	bool look_busy_ = false;
	bool settings_dirty_ = false;
	bool steady_ = true;
	bool handoff_ = true;
	int pal_mode_ = 2;       /* pal_speed: 0 True 50 Hz, 1 Fast 60 Hz, 2 Smooth 50 Hz (default) */
	bool look_bad_ = false;
	int look_mode_ = 0;
	/* Quick response (0 Off, 1 Auto, 2 On; default Auto on the Master System, Game Gear and SG-1000, Off on the Mega Drive: md/README.md) and No sprite flicker
	 * (8 bit systems); player.ini quick_response / no_sprite_flicker */
	int quick_ = 0, flicker_ = 1;
	pk::AheadGovernor ahead_;
	void (*ahead_hook_)(void *) = 0;
	void *ahead_user_ = 0;
	bool ahead_ran_ = false;
	void ahead_update_blocked();
	uint32_t extra_caps_ = 0;
	std::string link_rom_, link_result_, link_err_name_;
	unsigned link_pad_ = 0;
	int link_err_ = 0, link_frames_ = 0, link_peer_reason_ = -1;
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
	unsigned srm_gen_ = 0;             /* md_host_sram_generation() at the last look (a disc) */
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
