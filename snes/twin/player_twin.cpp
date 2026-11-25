/*
 * player_twin: runs the SNES player's own code (snes/vita/player.cpp: launch file, playerkit menu, states,
 * battery RAM writer, cheats, patches, session report, steady pacing) on Linux with playerkit's software
 * renderer, over a running game, and checks it. Writes a PNG of every menu screen.
 *
 *   player_twin test ROM SHOTS_DIR WORK_DIR      the scripted walk and checks (exit status 1 on a failure)
 *   player_twin speed ROM WORK                   patch changes the ROM CRC, rewind 5 s (the machine is bit for bit where it was), fast forward 2x / 3x
 *   player_twin ra ROM WORK                      RetroAchievements against a fake server on the loopback (ROM: tests/make_speed_rom.py)
 *   player_twin bench-saves DIR N FILL [threaded] N state saves (400 KB) into a states folder holding FILL
 *                                                 other files, for counting the card operations (strace -c)
 *
 * ROM is a synthetic cartridge (tests/make_cheat_rom.py). Copyright CBOC-Vita. MIT licence (see snes/frontend/LICENSE).
 */
#include "player.h"
#include "pk_gfx_soft.h"
#include "pk_icons.h"
#include "wide.h"
#include "host.h"
#include "pal_pace.h"
#include "fake_ra.h"
#include "pk_ra.h"
#include "pk_rewind.h"
#include "pk_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <algorithm>
#include <sys/stat.h>
#include <atomic>
#include <thread>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <functional>

using namespace pk;
#undef W_OK

static int g_fail = 0, g_checks = 0;
#define EXPECT(c) do { g_checks++; if (!(c)) { fprintf(stderr, "player_twin: FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

/* ---- the platform ----------------------------------------------------------------- */

struct TwinPlatform : snesp::Platform {
	uint64_t clock_us = 1000000;
	bool threaded = false;
	snesp::SnesPlayer *player = 0;
	std::vector<uint16_t> frame_px;
	int fw = 256, fh = 224;
	std::vector<std::string> ime_answers;      /* code, name, code, name, ... */
	bool ime_asked = false;
	std::vector<std::string> search_titles;    /* what the keyboard was asked for, in Search memory */
	std::string log_path, log_text;
	int pic_changes = 0;
	/* the writer thread (threaded mode) */
	std::thread th;
	std::mutex mu;
	std::condition_variable cv;
	bool stop = false, kick = false;

	uint64_t now_us() { return clock_us; }
	void pause_us(unsigned us) { std::this_thread::sleep_for(std::chrono::microseconds(us)); }
	bool wake_writer()
	{
		if (!threaded) return false;
		{ std::lock_guard<std::mutex> g(mu); kick = true; }
		cv.notify_one();
		return true;
	}
	void start_thread()
	{
		threaded = true;
		th = std::thread([this]() {
			for (;;) {
				{
					std::unique_lock<std::mutex> g(mu);
					cv.wait(g, [this]() { return kick || stop; });
					if (stop) return;
					kick = false;
				}
				while (player->writer().work_one(player)) {}
			}
		});
	}
	void stop_thread()
	{
		if (!threaded) return;
		{ std::lock_guard<std::mutex> g(mu); stop = true; }
		cv.notify_one();
		th.join();
		threaded = false;
	}
	bool current_frame(Frame *f)
	{
		if (frame_px.empty()) return false;
		*f = Frame(&frame_px[0], fw, fh, fw * 2);
		return true;
	}
	void grab()
	{
		int w, h, pitch;
		const uint16_t *v = snes_host_video(&w, &h, &pitch);
		if (!v || w <= 0 || h <= 0) return;
		fw = w; fh = h;
		frame_px.resize((size_t) w * h);
		for (int y = 0; y < h; y++) memcpy(&frame_px[(size_t) y * w], (const uint8_t *) v + (size_t) y * pitch, (size_t) w * 2);
	}
	void ask_cheat_text() { ime_asked = true; }
	void picture_changed() { pic_changes++; }
	void log_write(const std::string &chunk)
	{
		log_text += chunk;
		FILE *f = fopen(log_path.c_str(), "ab");
		if (f) { fwrite(chunk.data(), 1, chunk.size(), f); fclose(f); }
	}
};

/* ---- driving the player ----------------------------------------------------------- */

struct Rig {
	TwinPlatform pf;
	snesp::SnesPlayer *pl;
	SoftGfx gfx;
	Text text;
	Texture *game_tex;
	std::string shots;
	uint32_t frame = 0;
	unsigned drawn_frames = 0, ran_frames = 0, refreshes = 0, audio_in = 0, audio_out = 0, ahead_frames = 0;
	uint32_t emu_cost_us = 0;       /* what the test says a plain frame costs the emulating thread (0: Quick response's Auto is not fed) */
	int nshot;
	Rig(const std::string &s, int first = 0) : pl(0), gfx(), text(&gfx), game_tex(0), shots(s), nshot(first) {}
	~Rig() { if (game_tex) gfx.tex_free(game_tex); }

	void pads_none(PadSample p[MAX_PLAYERS]) { for (int i = 0; i < MAX_PLAYERS; i++) { p[i] = PadSample(); p[i].connected = i < 2; } }
	/* one game frame (pads as given); returns true when the menu was asked for */
	bool game_frame(uint32_t phys = 0, uint32_t phys2 = 0, bool tap = false)
	{
		PadSample p[MAX_PLAYERS];
		pads_none(p);
		p[0].buttons = phys & PH_BUTTONS;
		p[1].buttons = phys2;
		if (phys & PH_RS_LEFT) p[0].rx = 0;          /* the right stick (fast forward, rewind): the analog value, as the pad gives it */
		if (phys & PH_RS_RIGHT) p[0].rx = 255;
		pf.clock_us += 16683;
		/* as the Vita loop does: how many frames this refresh (fast forward, rewinding), then each one's input, frame and audio */
		const int n = pl->speed_plan(1);
		for (int i = 0; i < n; i++) {
			if (pl->input(p, tap && i == 0, pf.clock_us / 1000, frame)) return true;
			const bool render = pl->want_render(frame) && pl->speed_draws(i, n);
			if (pl->run_frame(render)) { ran_frames++; if (render) drawn_frames++; }
			if (pl->ahead_ran()) ahead_frames++;
			/* as the Vita loop does for a single drawn frame at normal speed: a frame run ahead costs about twice a plain one */
			if (n == 1 && render && emu_cost_us) pl->ahead_cost(pl->ahead_ran() ? emu_cost_us * 22 / 10 + 500 : emu_cost_us);
			int16_t ab[4096 * 2];
			int k;
			while ((k = snes_host_audio_take(ab, 4096)) > 0) { audio_in += (unsigned) k; audio_out += (unsigned) pl->audio_filter(ab, (size_t) k); }
			frame++;
		}
		refreshes++;
		pf.grab();
		pl->drawn_cost(9000);
		pl->after_frame(pf.clock_us);
		return false;
	}
	void run(int n) { for (int i = 0; i < n; i++) game_frame(); }
	MenuInput mi(uint32_t phys = 0)
	{
		MenuInput in;
		for (int i = 0; i < MAX_PLAYERS; i++) { in.pads[i] = PadSample(); in.pads[i].connected = i < 2; }
		in.pads[0].buttons = phys;
		in.dt_ms = 16;
		return in;
	}
	MenuEvent menu_frame(const MenuInput &in)
	{
		pf.clock_us += 16683;
		MenuEvent ev = pl->menu_update(in);
		if (pl->menu_open()) { draw(); pl->menu_drawn(); }
		if (pf.ime_asked) {
			pf.ime_asked = false;
			std::string stitle;
			if (pl->search_value_wanted(&stitle)) {
				/* Search memory: the keyboard is for a number */
				pf.search_titles.push_back(stitle);
				if (!pf.ime_answers.empty()) {
					std::string v = pf.ime_answers[0];
					pf.ime_answers.erase(pf.ime_answers.begin());
					pl->search_value_entered(true, v);
				} else pl->search_value_entered(false, "");
			} else
			if (pf.ime_answers.size() >= 2) {
				std::string c = pf.ime_answers[0], n = pf.ime_answers[1];
				pf.ime_answers.erase(pf.ime_answers.begin(), pf.ime_answers.begin() + 2);
				pl->cheat_entered(true, c, n);
			} else pl->cheat_entered(false, "", "");
		}
		return ev;
	}
	/* a button pressed for one frame, then released */
	MenuEvent press(uint32_t phys)
	{
		MenuEvent a = menu_frame(mi(phys));
		MenuEvent b = menu_frame(mi(0));
		return a != MENU_NONE ? a : b;
	}
	MenuEvent tap(float x, float y)
	{
		MenuInput in = mi(0);
		in.touch.down = true; in.touch.x = x; in.touch.y = y;
		MenuEvent a = menu_frame(in);
		MenuEvent b = menu_frame(mi(0));
		return a != MENU_NONE ? a : b;
	}
	void settle(int n = 12) { for (int i = 0; i < n; i++) menu_frame(mi(0)); }
	void draw()
	{
		gfx.clear(PK_RGBA(0, 0, 0, 255));
		if (!pf.frame_px.empty()) {
			if (!game_tex) game_tex = gfx.tex_create(512, 512, TEX_RGB565);
			gfx.tex_update(game_tex, &pf.frame_px[0], pf.fw * 2);
			/* Original shape: 4:3, full height */
			gfx.tex_draw(game_tex, 0, 0, (float) pf.fw, (float) pf.fh, 117, 0, 725, 544, 0xFFFFFFFFu, false);
		}
		if (pl->menu_open()) pl->menu_draw(&gfx, &text);
		else pl->notices().draw(&gfx, &text, PK_HEX(0x7B6CFF, 255), 26, true);
	}
	void snap(const char *name)
	{
		draw();
		char p[512];
		snprintf(p, sizeof p, "%s/%02d_%s.png", shots.c_str(), ++nshot, name);
		EXPECT(gfx.write_png(p));
	}
	/* holds START + SELECT until the menu opens */
	bool open_by_chord()
	{
		for (int i = 0; i < 40; i++)
			if (game_frame(PH_START | PH_SELECT)) { pl->open_menu(0); return true; }
		return false;
	}
};

static bool write_bytes(const std::string &p, const std::vector<uint8_t> &b)
{
	FILE *f = fopen(p.c_str(), "wb");
	if (!f) return false;
	bool ok = fwrite(b.data(), 1, b.size(), f) == b.size();
	return fclose(f) == 0 && ok;
}
static bool write_text(const std::string &p, const std::string &s) { return write_bytes(p, std::vector<uint8_t>(s.begin(), s.end())); }
static std::vector<uint8_t> read_bytes(const std::string &p) { std::vector<uint8_t> b; file_read(p, b); return b; }
/* the game's state folder (states/<console>/<xx>/, playerkit's bucket layout) */
static std::string bucket(const std::string &root, const std::string &game)
{
	StateLayout l; l.root = root; l.console = "snes"; l.name = game;
	return l.states_dir() + "/" + game;
}
static bool exists(const std::string &p) { struct stat st; return stat(p.c_str(), &st) == 0; }

/* failing writes of temp files in the states folder (a full card) */
static FsOps g_real;
static bool fail_state_tmp(const std::string &p, const uint8_t *d, size_t n)
{
	if (p.find("/states/snes/") != std::string::npos && ends_with_ci(p, ".tmp")) return false;
	return g_real.write_all(p, d, n);
}

static int run_test(const char *rom, const std::string &shots, const std::string &work)
{
	std::string root = work + "/root";
	make_dirs(root);
	make_dirs(shots);
	const std::string game = path_stem(rom);

	/* ---- a real state of this game, made by the core, for the old-layout files ---- */
	std::vector<uint8_t> real_state;
	{
		EXPECT(snes_host_init() == 0);
		char e[128];
		EXPECT(snes_host_load_rom(rom, NULL, e, sizeof e) == 0);
		for (int i = 0; i < 30; i++) snes_host_run_frame();
		size_t len = 0;
		real_state.resize(snes_host_state_size());
		EXPECT(snes_host_state_save_buf(real_state.data(), real_state.size(), &len) == 0);
		real_state.resize(len);
		/* buffer and file states are the same bytes */
		std::string sp = work + "/file.st";
		EXPECT(snes_host_state_save(sp.c_str()) == 0);
		EXPECT(read_bytes(sp) == real_state);
		EXPECT(snes_host_state_load_buf(real_state.data(), real_state.size()) == 0);
		std::vector<uint8_t> junk(100, 0x5A);
		EXPECT(snes_host_state_load_buf(junk.data(), junk.size()) != 0);
		snes_host_unload();
	}
	/* old player's states: <root>/snes/states/<game>.st0 and .st2; the new slot 1 is already in use */
	make_dirs(root + "/snes/states");
	make_dirs(root + "/states/snes");
	EXPECT(write_bytes(root + "/snes/states/" + game + ".st0", real_state));
	EXPECT(write_bytes(root + "/snes/states/" + game + ".st2", real_state));
	EXPECT(write_bytes(root + "/snes/states/Other Game.st1", real_state));      /* another game's: left alone */
	std::vector<uint8_t> occupant(1000, 0x11);
	EXPECT(write_bytes(root + "/states/snes/" + game + ".st0", occupant));

	/* a patch: the ROM byte at $00800D (file offset 0x0D) is the value the game writes to $7E0011 */
	std::vector<uint8_t> ips;
	const char *hdr = "PATCH";
	ips.insert(ips.end(), hdr, hdr + 5);
	uint8_t rec[] = { 0x00, 0x00, 0x0D, 0x00, 0x01, 0x42 };
	ips.insert(ips.end(), rec, rec + sizeof rec);
	ips.insert(ips.end(), (const uint8_t *) "EOF", (const uint8_t *) "EOF" + 3);
	EXPECT(write_bytes(work + "/fix.ips", ips));

	/* an older save, and a newer RetroArch save to bring in */
	make_dirs(root + "/saves");
	make_dirs(work + "/ra");
	std::string srm = root + "/saves/" + game + ".srm", ra = work + "/ra/" + game + ".srm";
	std::vector<uint8_t> old_srm(8192, 0x01), ra_srm(8192, 0x77);
	EXPECT(write_bytes(srm, old_srm));
	EXPECT(write_bytes(ra, ra_srm));
	{
		struct timespec ts[2];
		ts[0].tv_sec = ts[1].tv_sec = time(0) - 3600; ts[0].tv_nsec = ts[1].tv_nsec = 0;
		utimensat(AT_FDCWD, srm.c_str(), ts, 0);
	}

	/* the launch file the main app will write */
	make_dirs(root + "/launch");
	std::string ini =
	    "version=1\nsession_id=s-test-1\nconsole=snes\ngame=" + std::string(rom) + "\ngame_name=Cheat Test (World)\nsave=" + srm +
	    "\nroot=" + root + "\nshape=zoom\nquality=smooth\ncpu_mhz=444\nenter_button=circle\nlanguage=en\nstart=new\nslot=0\npatch=" +
	    work + "/fix.ips\npatch_key=fix-1\nretroarch_save=" + ra + "\nwide_supported=0\novr.scanlines=4\nfuture_key=kept\n";
	EXPECT(write_text(root + "/launch/snes.ini", ini));

	Rig rig(shots);
	rig.pf.log_path = root + "/snes/log.txt";
	snesp::SnesPlayer *pl = new snesp::SnesPlayer(&rig.pf);
	rig.pl = pl;
	rig.pf.player = pl;
	char a0[] = "app0:snes.bin";
	std::string romarg = rom;
	char *argv[] = { a0, &romarg[0], 0 };
	snesp::StartResult sr = pl->start(2, argv, root, true);
	EXPECT(sr.ok);
	if (!sr.ok) { fprintf(stderr, "start: %s / %s\n", sr.fatal1.c_str(), sr.fatal2.c_str()); return 1; }
	/* launch.ini parsed */
	EXPECT(pl->have_launch_file());
	EXPECT(sr.cpu_mhz == 444);
	EXPECT(!sr.enter_is_cross);
	EXPECT(pl->picture().scan == 4);
	EXPECT(pl->picture().shape == PIC_ZOOM);
	EXPECT(pl->picture().quality == PIC_SMOOTH);
	EXPECT(pl->request().session_id == "s-test-1");
	EXPECT(pl->request().unknown.count("future_key") == 1);
	/* migration: old .st0 -> slot 2 (slot 1 was taken), old .st2 -> slot 3; the other game's file stays */
	EXPECT(read_bytes(bucket(root, game) + ".st0") == occupant);
	EXPECT(read_bytes(bucket(root, game) + ".st1") == real_state);
	EXPECT(read_bytes(bucket(root, game) + ".st2") == real_state);
	EXPECT(!exists(root + "/snes/states/" + game + ".st0"));
	EXPECT(!exists(root + "/snes/states/" + game + ".st2"));
	EXPECT(exists(root + "/snes/states/Other Game.st1"));
	EXPECT(pl->states()->used_count() == 3);
	/* RetroArch save brought in, the old one kept in the backups */
	EXPECT(read_bytes(srm) == ra_srm);
	EXPECT(read_bytes(root + "/backups/snes/" + game + ".before-retroarch.srm") == old_srm);
	pl->apply_start_mode();

	/* ---- the patch reached the game ---- */
	rig.run(30);
	EXPECT(snes_host_peek_wram(0x11) == 0x42);
	EXPECT(snes_host_peek_wram(0x10) == 0x05);
	rig.snap("ingame_notice");

	/* ---- the menu: START + SELECT, and a tap on the picture ---- */
	EXPECT(rig.open_by_chord());
	rig.settle(14);
	rig.snap("menu_top");
	EXPECT(rig.press(PH_CROSS) == MENU_RESUME);         /* Enter is Circle here: Cross is Back = resume */
	rig.run(20);
	/* the old chord, L + R + Start held, still opens it */
	{
		bool opened = false;
		for (int i = 0; i < 40 && !opened; i++) opened = rig.game_frame(PH_L | PH_R | PH_START);
		EXPECT(opened);
		if (opened) { pl->open_menu(0); rig.settle(3); EXPECT(rig.press(PH_CROSS) == MENU_RESUME); }
		rig.run(20);
	}
	EXPECT(rig.game_frame(0, 0, true));                  /* a tap on the picture */
	pl->open_menu(0);
	rig.settle(14);
	/* the battery RAM was written after the menu was drawn (the game changed it at boot) */
	EXPECT(!snes_host_sram_dirty());
	EXPECT(pl->session().current().save_changed);
	EXPECT(read_bytes(srm) != ra_srm);

	/* ---- Save states ---- */
	rig.press(PH_RIGHT);
	rig.snap("menu_top_states_card");
	rig.press(PH_CIRCLE);
	EXPECT(pl->menu().page() == Menu::PAGE_STATES);
	rig.settle(4);
	rig.snap("states_migrated");
	rig.press(PH_DOWN); rig.press(PH_DOWN); rig.press(PH_DOWN);       /* slot 4: empty */
	EXPECT(pl->menu().selected_slot() == 4);
	std::vector<uint8_t> before_save;
	pl->capture_state(before_save);
	rig.press(PH_CIRCLE);                                  /* Enter on an empty slot saves */
	rig.settle(4);
	std::string s4 = bucket(root, game) + ".st3";
	EXPECT(exists(s4));
	EXPECT(exists(s4 + ".thumb"));
	EXPECT(exists(s4 + ".shot"));
	EXPECT(exists(s4 + ".meta"));
	EXPECT(read_bytes(s4) == before_save);
	EXPECT(pl->menu().notices().kind() == NOTICE_OK);
	rig.snap("states_saved_slot4");
	std::vector<uint8_t> saved4 = read_bytes(s4);
	/* forced write failure: overwrite slot 4 with the card refusing the temp file */
	rig.press(PH_CROSS);                                   /* back to the cards */
	EXPECT(rig.press(PH_CROSS) == MENU_RESUME);
	rig.run(60);
	pl->open_menu(0);
	rig.settle(10);
	rig.press(PH_RIGHT);
	rig.press(PH_CIRCLE);
	pl->menu().select_slot(4);
	g_real = fs();
	fs().write_all = fail_state_tmp;
	rig.press(PH_SQUARE);                                  /* save over a filled slot: asks first */
	EXPECT(pl->menu().modal_open());
	rig.snap("states_overwrite_ask");
	rig.press(PH_CIRCLE);
	fs() = g_real;
	rig.settle(8);
	EXPECT(pl->menu().notices().kind() == NOTICE_ERROR);
	EXPECT(read_bytes(s4) == saved4);                      /* the slot is untouched */
	EXPECT(exists(s4 + ".meta"));
	rig.snap("states_write_failed");
	/* load slot 4 (the state saved above) after the game moved on, then undo */
	std::vector<uint8_t> moved;
	pl->capture_state(moved);
	EXPECT(moved != saved4);
	EXPECT(rig.press(PH_CIRCLE) == MENU_RESUME);           /* Enter on a filled slot loads and resumes */
	std::vector<uint8_t> after_load;
	pl->capture_state(after_load);
	EXPECT(after_load == saved4);
	pl->open_menu(0);
	rig.settle(6);
	rig.press(PH_RIGHT);
	rig.press(PH_CIRCLE);
	rig.settle(4);
	rig.snap("states_undo_offered");
	EXPECT(rig.press(PH_SELECT) == MENU_RESUME);           /* undo the load */
	std::vector<uint8_t> after_undo;
	pl->capture_state(after_undo);
	EXPECT(after_undo == moved);

	/* ---- Settings > Picture ---- */
	pl->open_menu(0);
	rig.settle(4);
	pl->menu().goto_page(Menu::PAGE_SETTINGS);
	rig.settle(2);
	rig.snap("settings");
	pl->menu().goto_page(Menu::PAGE_PICTURE);
	rig.settle(2);
	rig.snap("picture");
	{
		bool fit = false, pixel = false, hires = false;
		for (int i = 0; i < pl->menu().row_count(); i++) {
			if (pl->menu().row_label(i) == "Screen size") {
				/* step through every choice: never "Fit" */
				for (int k = 0; k < 6; k++) { if (pl->menu().row_value_text(i) == "Fit") fit = true; }
			}
			if (pl->menu().row_label(i) == "Pixel shape") pixel = true;
			if (pl->menu().row_label(i) == "Blend hi-res columns") hires = true;
		}
		EXPECT(!fit && pixel && hires);
		/* one Image quality row as in the main app (Auto, Original, Enhanced 2x, Enhanced 3x, MMPX 2x: no 4x scaler here) and its Video filter; the old
		 * Pixel art scaler and Image look rows are folded into it */
		{
			int iq = -1, vf = -1, old = 0;
			for (int i = 0; i < pl->menu().row_count(); i++) {
				const std::string l = pl->menu().row_label(i);
				if (l == "Image quality") iq = i;
				if (l == "Video filter") vf = i;
				if (l == "Pixel art scaler" || l == "Image look") old++;
			}
			EXPECT(iq >= 0 && vf >= 0 && old == 0);
			static const char *const want[] = { "Original", "Enhanced 2x", "Enhanced 3x", "MMPX 2x", "Auto" };
			static const int scal[] = { SCALER_OFF, SCALER_XBR2, SCALER_XBR3, SCALER_MMPX2, SCALER_AUTO };
			if (iq >= 0) {
				while (pl->menu().cursor() != iq) rig.press(PH_DOWN);
				EXPECT(pl->menu().row_value_text(iq) == "Auto" && pl->picture().scaler == SCALER_AUTO);
				for (int k = 0; k < 5; k++) { rig.press(PH_RIGHT); EXPECT(pl->menu().row_value_text(iq) == want[k] && pl->picture().scaler == scal[k]); }
			}
		}
		/* Scanlines: Right once (4 -> 5) */
		int row = -1;
		for (int i = 0; i < pl->menu().row_count(); i++) if (pl->menu().row_label(i) == "Scanlines") row = i;
		EXPECT(row >= 0);
		while (pl->menu().cursor() != row) rig.press(PH_DOWN);
		rig.press(PH_RIGHT);
		EXPECT(pl->picture().scan == 5);
		/* Screen size: Zoom -> Stretch -> Pixel perfect -> Fit (one Fit: the main app's word) */
		while (pl->menu().cursor() != 0) rig.press(PH_UP);
		std::vector<std::string> seen;
		for (int k = 0; k < 4; k++) { rig.press(PH_RIGHT); seen.push_back(pl->menu().row_value_text(0)); }
		EXPECT(seen.size() == 4 && seen[0] == "Stretch" && seen[1] == "Pixel perfect" && seen[2] == "Fit" && seen[3] == "Zoom");
		EXPECT(pl->picture().shape == PIC_ZOOM);
	}
	rig.snap("picture_changed");
	pl->menu().goto_page(Menu::PAGE_SPEED);
	rig.settle(2);
	rig.snap("speed_steady_pacing");

	/* ---- Controls and remapping ---- */
	pl->menu().goto_page(Menu::PAGE_CONTROLS);
	rig.settle(2);
	rig.snap("controls");
	pl->menu().goto_page(Menu::PAGE_MAPPING);
	rig.settle(2);
	rig.snap("mapping");
	{
		int row = -1;
		for (int i = 0; i < pl->menu().row_count(); i++) if (pl->menu().row_label(i) == "A") row = i;
		EXPECT(row >= 0);
		while (pl->menu().cursor() != row) rig.press(PH_DOWN);
		rig.press(PH_CIRCLE);
		EXPECT(pl->menu().modal_open());
		rig.settle(2);
		rig.snap("mapping_capture");
		rig.press(PH_SQUARE);                              /* SNES A is now on Square */
		rig.settle(3);
		rig.snap("mapping_assigned");
	}
	EXPECT(exists(root + "/controls/snes.ini"));
	while (pl->menu().page() != Menu::PAGE_TOP) rig.press(PH_CROSS);
	EXPECT(rig.press(PH_CROSS) == MENU_RESUME);
	rig.run(3);
	rig.game_frame(PH_SQUARE, PH_CROSS);
	EXPECT(pl->last_input().pad[0] & SNES_A);
	EXPECT(!(pl->last_input().pad[0] & SNES_Y));        /* Y lost Square to A ... */
	EXPECT(pl->last_input().pad[1] & SNES_B);           /* player 2 still works */
	rig.game_frame(PH_CIRCLE);
	EXPECT(!(pl->last_input().pad[0] & SNES_A));        /* ... and A left Circle */

	/* ---- Cheats: add with the keyboard, toggle, delete ---- */
	pl->open_menu(0);
	rig.settle(6);
	pl->menu().goto_page(Menu::PAGE_CHEATS);
	rig.settle(2);
	rig.snap("cheats_empty");
	rig.pf.ime_answers.push_back("nonsense!");
	rig.pf.ime_answers.push_back("Bad");
	rig.press(PH_CIRCLE);                                  /* Add a cheat: a wrong code */
	EXPECT(pl->menu().notices().kind() == NOTICE_ERROR);
	rig.settle(8);
	rig.snap("cheats_bad_code");
	rig.pf.ime_answers.push_back("7E0010=AA");
	rig.pf.ime_answers.push_back("Colour");
	rig.press(PH_CIRCLE);
	EXPECT(snes_host_cheat_count() == 1);
	rig.settle(8);
	rig.snap("cheats_added");
	EXPECT(rig.press(PH_CROSS) == MENU_NONE);                 /* back to the cards */
	EXPECT(rig.press(PH_CROSS) == MENU_RESUME);
	rig.run(3);
	EXPECT(snes_host_peek_wram(0x10) == 0xAA);
	EXPECT(exists(root + "/cheats/" + game + ".cht"));
	pl->open_menu(0);
	rig.settle(4);
	pl->menu().goto_page(Menu::PAGE_CHEATS);
	rig.settle(2);
	rig.press(PH_DOWN);
	rig.press(PH_DOWN);                                    /* past Add a cheat and Search memory */
	rig.press(PH_LEFT);                                    /* off */
	EXPECT(pl->menu().notices().text().find("Cheat off") == 0);
	EXPECT(pl->menu().notices().kind() != NOTICE_OK);
	rig.snap("cheats_off");
	rig.press(PH_CROSS);
	EXPECT(rig.press(PH_CROSS) == MENU_RESUME);
	rig.run(3);
	EXPECT(snes_host_peek_wram(0x10) == 0x05);
	pl->open_menu(0);
	rig.settle(4);
	pl->menu().goto_page(Menu::PAGE_CHEATS);
	rig.settle(2);
	rig.press(PH_DOWN);
	rig.press(PH_DOWN);
	rig.press(PH_TRIANGLE);
	EXPECT(pl->menu().modal_open());
	rig.snap("cheats_delete_ask");
	rig.press(PH_CIRCLE);
	EXPECT(snes_host_cheat_count() == 0);
	rig.settle(3);
	rig.snap("cheats_deleted");

	/* ---- Search memory: find a byte by how it changes, make it a cheat ---- */
	{
		auto to_row = [&](const char *label) {
			for (int k = 0; k < 80 && !(pl->menu().cursor() < pl->menu().row_count() && pl->menu().row_label(pl->menu().cursor()) == label); k++) rig.press(PH_DOWN);
			return pl->menu().cursor() < pl->menu().row_count() && pl->menu().row_label(pl->menu().cursor()) == label;
		};
		pl->menu().goto_page(Menu::PAGE_CHEATS);
		rig.settle(2);
		EXPECT(to_row("Search memory"));
		rig.press(PH_CIRCLE);
		EXPECT(pl->menu().page() == Menu::PAGE_CHEAT_SEARCH);
		EXPECT(!pl->menu().search_active());
		rig.settle(2);
		rig.snap("search_start");
		snes_host_poke_wram(0x1234, 0xA7);
		EXPECT(to_row("New search"));
		rig.press(PH_CIRCLE);                              /* 1 byte, the work RAM now */
		EXPECT(pl->menu().search_active() && pl->menu().search_count() == 0x20000);
		rig.settle(2);
		/* a number that does not fit one byte is refused and the search stays as it was */
		rig.pf.ime_answers.push_back("999");
		EXPECT(to_row("Equal to"));
		rig.press(PH_CIRCLE);
		EXPECT(pl->menu().notices().kind() == NOTICE_ERROR && pl->menu().search_count() == 0x20000);
		EXPECT(!rig.pf.search_titles.empty() && rig.pf.search_titles.back() == "Equal to (a number)");
		rig.pf.ime_answers.push_back("167");
		rig.press(PH_CIRCLE);
		const int eq = pl->menu().search_count();
		EXPECT(eq >= 1 && eq < 0x20000);
		rig.settle(2);
		rig.snap("search_equal");
		snes_host_poke_wram(0x1234, 0xA8);
		EXPECT(to_row("Increased"));
		rig.press(PH_CIRCLE);
		EXPECT(pl->menu().search_count() == 1);
		rig.settle(2);
		rig.snap("search_result");
		EXPECT(to_row("7E1234"));
		EXPECT(pl->menu().row_value_text(pl->menu().cursor()) == "168 (0xA8)");
		rig.press(PH_CIRCLE);                              /* make it a cheat */
		EXPECT(snes_host_cheat_count() == 1);
		{
			snes_cheat_info ci;
			EXPECT(snes_host_cheat_get(0, &ci) == 0 && std::string(ci.label) == "Found at 7E1234" && ci.enabled);
		}
		EXPECT(pl->menu().notices().text().find("Cheat added and on") == 0);
		rig.press(PH_CROSS);                               /* back to the Cheats page */
		EXPECT(pl->menu().page() == Menu::PAGE_CHEATS);
		EXPECT(rig.press(PH_CROSS) == MENU_NONE);
		EXPECT(rig.press(PH_CROSS) == MENU_RESUME);
		snes_host_poke_wram(0x1234, 0);
		rig.run(3);
		EXPECT(snes_host_peek_wram(0x1234) == 0xA8);       /* the cheat keeps the value */
		/* the search survives the menu closing: reopen and the count is still there */
		pl->open_menu(0);
		rig.settle(4);
		pl->menu().goto_page(Menu::PAGE_CHEAT_SEARCH);
		rig.settle(2);
		EXPECT(pl->menu().search_active() && pl->menu().search_count() == 1);
		EXPECT(snes_host_cheat_remove(0) == 0);
	}

	/* ---- Reset and Game list need confirmation ---- */
	pl->menu().goto_page(Menu::PAGE_TOP);
	rig.settle(2);
	for (int k = 0; k < 8 && rig.pl->menu().cursor() != 5; k++) rig.press(PH_RIGHT);   /* Reset game (no Play together card) */
	rig.snap("top_reset_card");
	rig.press(PH_CIRCLE);
	EXPECT(pl->menu().modal_open());
	rig.snap("reset_ask");
	rig.press(PH_CROSS);                                   /* Cancel */
	rig.press(PH_RIGHT);
	rig.press(PH_CIRCLE);
	rig.snap("game_list_ask");
	MenuEvent ev = rig.press(PH_CIRCLE);
	EXPECT(ev == MENU_GAME_LIST);
	/* a quick save from the game (SELECT + L), threaded writer: no wait on the game thread */
	pl->finish(EXIT_GAME_LIST);
	/* ---- the session report ---- */
	SessionReport rep;
	EXPECT(session_read(root, rep));
	EXPECT(rep.valid && rep.session_id == "s-test-1");
	EXPECT(rep.seconds_played >= 2);
	EXPECT(rep.save_changed);
	EXPECT(!rep.save_error);
	EXPECT(std::find(rep.states_touched.begin(), rep.states_touched.end(), 4) != rep.states_touched.end());
	EXPECT(rep.resume_present);
	EXPECT(rep.exit == EXIT_GAME_LIST);
	EXPECT(exists(bucket(root, game) + ".rsm"));
	{ std::vector<uint8_t> pt = read_bytes(bucket(root, game) + ".rsm.pt"); EXPECT(std::string(pt.begin(), pt.end()) == "fix-1"); }
	EXPECT(exists(root + "/scenes/" + game + ".shot"));
	EXPECT(exists(root + "/snes/picture.ini") || exists(root + "/snes/games/" + game + ".ini"));
	{
		Ini p;
		EXPECT(p.load(root + "/snes/picture.ini"));
		EXPECT(p.get_int("scanlines", -1) == 5);      /* changed in the menu: kept */
		EXPECT(p.get_int("quality", -1) == 0);         /* the launch file's "smooth" was not written into the player's file */
	}
	std::string session_text;
	{ std::vector<uint8_t> b = read_bytes(root + "/launch/session.txt"); session_text.assign(b.begin(), b.end()); }
	fprintf(stderr, "---- session.txt\n%s----\n", session_text.c_str());
	delete pl;
	snes_host_unload();

	/* ---- second start: no launch file (today's argument), quick save/load and threaded writer ---- */
	file_remove(root + "/launch/snes.ini");
	{
		Rig r2(shots, 40);
		r2.pf.log_path = root + "/snes/log.txt";
		snesp::SnesPlayer *p2 = new snesp::SnesPlayer(&r2.pf);
		r2.pl = p2; r2.pf.player = p2;
		std::string arg = std::string(rom) + "|" + srm;
		char *av[] = { a0, &arg[0], 0 };
		snesp::StartResult s2 = p2->start(2, av, root, true);
		EXPECT(s2.ok && !p2->have_launch_file());
		EXPECT(s2.enter_is_cross && s2.cpu_mhz == 0);
		EXPECT(p2->srm_path() == srm);
		EXPECT(p2->picture().scan == 5);
		r2.pf.start_thread();
		p2->apply_start_mode();
		r2.run(40);
		EXPECT(snes_host_peek_wram(0x11) == 0x05);       /* no patch this time */
		/* quick save: SELECT + L */
		r2.game_frame(PH_SELECT);
		r2.game_frame(PH_SELECT | PH_L);
		r2.run(2);
		p2->wait_writer();
		r2.run(1);
		EXPECT(p2->notices().text() == "State saved to slot 1");
		EXPECT(read_bytes(bucket(root, game) + ".st0") != occupant);
		r2.snap("quicksave_notice");
		/* a threaded save through the menu shows at once ("Saving...") and finishes */
		p2->open_menu(0);
		r2.settle(4);
		r2.press(PH_RIGHT);
		r2.press(PH_CROSS);
		p2->menu().select_slot(6);
		r2.press(PH_CROSS);
		r2.settle(1);
		p2->wait_writer();
		r2.settle(2);
		EXPECT(exists(bucket(root, game) + ".st5"));
		EXPECT(p2->menu().notices().text() == "State saved to slot 6");
		r2.snap("states_threaded_save");
		r2.press(PH_CIRCLE);
		EXPECT(r2.press(PH_CIRCLE) == MENU_RESUME);
		p2->finish(EXIT_NORMAL);
		r2.pf.stop_thread();
		SessionReport rp;
		EXPECT(session_read(root, rp));
		EXPECT(rp.session_id.empty() && rp.exit == EXIT_NORMAL);
		delete p2;
		snes_host_unload();
	}
	/* ---- third start: continue from the resume point (patch key must match) ---- */
	{
		std::string ini3 = "version=1\nsession_id=s-test-3\ngame=" + std::string(rom) + "\nsave=" + srm + "\nstart=continue\npatch_key=other\n";
		write_text(root + "/launch/snes.ini", ini3);
		Rig r3(shots);
		r3.pf.log_path = root + "/snes/log.txt";
		snesp::SnesPlayer *p3 = new snesp::SnesPlayer(&r3.pf);
		r3.pl = p3; r3.pf.player = p3;
		char *av[] = { a0, 0 };
		EXPECT(p3->start(1, av, root, true).ok);
		p3->apply_start_mode();
		EXPECT(p3->notices().text().find("another patch") != std::string::npos);
		delete p3;
		snes_host_unload();
		ini3 = "version=1\nsession_id=s-test-4\ngame=" + std::string(rom) + "\nsave=" + srm + "\nstart=continue\n";
		write_text(root + "/launch/snes.ini", ini3);
		Rig r4(shots);
		r4.pf.log_path = root + "/snes/log.txt";
		snesp::SnesPlayer *p4 = new snesp::SnesPlayer(&r4.pf);
		r4.pl = p4; r4.pf.player = p4;
		EXPECT(p4->start(1, av, root, true).ok);
		std::vector<uint8_t> rsm;
		file_read(bucket(root, game) + ".rsm", rsm);
		p4->apply_start_mode();
		std::vector<uint8_t> now;
		p4->capture_state(now);
		EXPECT(now == rsm);
		delete p4;
		snes_host_unload();
	}
	/* ---- a patch that does not fit: the game is not started, the app is told ---- */
	{
		std::vector<uint8_t> bad(ips.begin(), ips.begin() + 8);
		write_bytes(work + "/bad.ips", bad);
		std::string ini5 = "version=1\nsession_id=s-test-5\ngame=" + std::string(rom) + "\npatch=" + work + "/bad.ips\n";
		write_text(root + "/launch/snes.ini", ini5);
		Rig r5(shots);
		snesp::SnesPlayer *p5 = new snesp::SnesPlayer(&r5.pf);
		r5.pl = p5; r5.pf.player = p5;
		char *av[] = { a0, 0 };
		snesp::StartResult s5 = p5->start(1, av, root, true);
		EXPECT(!s5.ok);
		fprintf(stderr, "bad patch: %s / %s\n", s5.fatal1.c_str(), s5.fatal2.c_str());
		SessionReport rp;
		EXPECT(session_read(root, rp) && rp.session_id == "s-test-5" && rp.exit == EXIT_ERROR && !rp.notice.empty());
		delete p5;
	}
	fprintf(stderr, "player_twin: %d checks, %d failed; %d screenshots in %s\n", g_checks, g_fail, rig.nshot, shots.c_str());
	return g_fail ? 1 : 0;
}

/* ---- card operations per state save ---- */
static int bench(const std::string &dir, int n, int fill)
{
	StateLayout l;
	l.root = dir; l.console = "snes"; l.name = "Bench Game (USA)";
	make_dirs(l.states_dir());
	for (int i = 0; i < fill; i++) {
		char p[600];
		snprintf(p, sizeof p, "%s/Filler %04d.st%d", l.states_dir().c_str(), i / 5, i % 5);
		FILE *f = fopen(p, "wb");
		if (f) fclose(f);
	}
	States st(l);
	st.rescan();
	std::vector<uint8_t> d(400 * 1024);
	std::vector<uint16_t> px(256 * 224, 0x1234);
	Frame fr(&px[0], 256, 224, 512);
	fprintf(stderr, "bench: start marker\n");
	for (int i = 0; i < n; i++) {
		d[0] = (uint8_t) i;
		StateJob j = st.prepare(1 + (i % 3), d.data(), d.size(), fr, i);
		SaveReport r = st.commit(j);
		if (r.status != ST_OK) { fprintf(stderr, "save failed\n"); return 1; }
	}
	fprintf(stderr, "bench: %d saves done\n", n);
	return 0;
}


/* ---- the wide view in the player ------------------------------------------------------------------------------- */
/* ROM: tests/make_wide_roms.py wide_scroll64 (a mode 1 scroller whose map columns stream at the seam). */
static int run_wide_test(const char *rom, const std::string &work)
{
	std::string root = work + "/root";
	make_dirs(root);
	const std::string game = path_stem(rom);
	char a0[] = "app0:snes.bin";
	std::string arg = std::string(rom);
	char *av[] = { a0, &arg[0], 0 };
	const std::string wpath = root + "/snes/wide/" + game + ".wmap";

	auto frames = [&](Rig &r, int n) {
		for (int i = 0; i < n; i++) { r.pl->wide_apply(960, 544); r.game_frame(); }
	};
	auto centre_matches = [&](int M) {
		const uint16_t *wp; int ww, wh, wpp; snes_wide_stats st;
		if (!snes_wide_frame(&wp, &ww, &wh, &wpp, &st)) return false;
		int w, h, pitch;
		const uint16_t *v = snes_host_video(&w, &h, &pitch);
		if (ww != 256 + 2 * M || w != 256 || wh != h) return false;
		for (int y = 0; y < h; y++)
			if (memcmp(wp + (size_t) y * wpp + M, (const uint8_t *) v + (size_t) y * pitch, 512)) return false;
		return true;
	};

	{
		Rig rig(work + "/wshots");
		snesp::SnesPlayer *pl = new snesp::SnesPlayer(&rig.pf);
		rig.pl = pl; rig.pf.player = pl;
		rig.pf.log_path = root + "/snes/log.txt";
		snesp::StartResult sr = pl->start(2, av, root, true);
		EXPECT(sr.ok);
		if (!sr.ok) return 1;
		pl->apply_start_mode();
		/* the default is Auto, shown in the menu; the picture shape Original wants 41 columns each side on a 960x544 screen */
		EXPECT(pl->picture().wide == 0);
		EXPECT(pl->menu().value(ROW_PIC_WIDE) == 0);
		EXPECT(!pl->wide_active());
		frames(rig, 5);
		EXPECT(pl->wide_active() && pl->wide_margin() == 41);
		frames(rig, 400);
		EXPECT(snes_wide_active() && snes_wide_margin() == 41);
		EXPECT(centre_matches(41));
		/* the menu row: Off switches it off, On back; the stretched shape has nothing to gain */
		pl->on_change(ROW_PIC_WIDE, 2);
		frames(rig, 2);
		EXPECT(!pl->wide_active() && !snes_wide_active());
		{ const uint16_t *wp; int ww, wh, wpp; snes_wide_stats st; EXPECT(!snes_wide_frame(&wp, &ww, &wh, &wpp, &st)); }
		pl->on_change(ROW_PIC_WIDE, 1);
		frames(rig, 3);
		EXPECT(pl->wide_active() && pl->wide_margin() == 41);
		pl->on_change(ROW_PIC_SHAPE, 3);                       /* Full screen */
		frames(rig, 2);
		EXPECT(!pl->wide_active());
		pl->on_change(ROW_PIC_SHAPE, 4);                       /* Pixel perfect: 8:7 pixels, integer scale 2 */
		frames(rig, 3);
		fprintf(stderr, "pixel perfect margin %d\n", pl->wide_margin());
		EXPECT(pl->wide_active() && (pl->wide_margin() == 82 || pl->wide_margin() == 81));
		pl->on_change(snesp::SnesPlayer::ROW_PIXELS, 1);                          /* square pixels: 112 would fit, the core's limit is 96 */
		frames(rig, 3);
		EXPECT(pl->wide_active() && pl->wide_margin() == SNES_WIDE_MAX_MARGIN);
		EXPECT(centre_matches(SNES_WIDE_MAX_MARGIN));
		pl->on_change(snesp::SnesPlayer::ROW_PIXELS, 0);
		pl->on_change(ROW_PIC_SHAPE, 0);
		pl->on_change(ROW_PIC_WIDE, 0);
		frames(rig, 3);
		EXPECT(pl->wide_active() && pl->wide_margin() == 41);
		/* Auto while the steady pacer thins frames: stays on (owner decision: never switched off to save time) */
		for (int i = 0; i < 300 && pl->pace_mode() == PACE_FULL; i++) { pl->drawn_cost(60000); frames(rig, 1); }
		if (pl->pace_mode() == PACE_HALF) { frames(rig, 2); EXPECT(pl->wide_active()); pl->on_change(ROW_PIC_WIDE, 2); frames(rig, 2); EXPECT(!pl->wide_active()); pl->on_change(ROW_PIC_WIDE, 0); frames(rig, 2); EXPECT(pl->wide_active()); }
		/* back to full pacing */
		for (int i = 0; i < 2000 && pl->pace_mode() != PACE_FULL; i++) { pl->drawn_cost(3000); frames(rig, 1); }
		frames(rig, 3);
		/* the learned map: new cells are written by the writer, not at once, after 30 s of play */
		frames(rig, 2300);
		EXPECT(pl->wide_active());
		EXPECT(exists(wpath));
		{ std::vector<uint8_t> b = read_bytes(wpath); EXPECT(b.size() > 1000); fprintf(stderr, "wide map file: %zu bytes\n", b.size()); }
		pl->finish(EXIT_NORMAL);
		delete pl;
		snes_host_unload();
	}
	EXPECT(exists(wpath));
	{
		/* a second start loads the map (the first frame already serves from it) */
		Rig r2(work + "/wshots", 40);
		snesp::SnesPlayer *p2 = new snesp::SnesPlayer(&r2.pf);
		r2.pl = p2; r2.pf.player = p2;
		r2.pf.log_path = root + "/snes/log.txt";
		EXPECT(p2->start(2, av, root, true).ok);
		p2->log_flush(true);
		EXPECT(r2.pf.log_text.find("wide view map loaded") != std::string::npos);
		EXPECT(!snes_wide_map_dirty());
		frames(r2, 10);
		EXPECT(snes_wide_map_key() != 0);
		p2->finish(EXIT_NORMAL);
		delete p2;
		snes_host_unload();
	}
	{
		/* a map file that cannot be written: the game goes on, the map stays dirty, the log says why */
		file_remove(wpath);
		file_remove(root + "/snes/wide");
		EXPECT(write_text(root + "/snes/wide", "not a folder"));
		Rig r3(work + "/wshots", 60);
		snesp::SnesPlayer *p3 = new snesp::SnesPlayer(&r3.pf);
		r3.pl = p3; r3.pf.player = p3;
		r3.pf.log_path = root + "/snes/log.txt";
		EXPECT(p3->start(2, av, root, true).ok);
		frames(r3, 2300);
		EXPECT(p3->wide_active());
		p3->log_flush(true);
		EXPECT(r3.pf.log_text.find("wide view map NOT written") != std::string::npos);
		EXPECT(snes_wide_map_dirty());
		p3->finish(EXIT_NORMAL);
		delete p3;
		snes_host_unload();
	}
	file_remove(root + "/snes/wide");
	return 0;
}

/* PAL pacing: the Vita loop's frame accumulator (pal_pace.h) over a PAL ROM and 3000 display refreshes at 59.94 Hz. The game's own
 * time (frames run / console rate) must stay within 0.5 % of the refresh time, no refresh may skip twice in a row (even spread),
 * and the core must really report PAL. An NTSC ROM is checked to keep one frame per refresh (nothing changes for it). */
static int run_pal_test(const char *rom)
{
	char err[256] = "";
	if (snes_host_init()) { fprintf(stderr, "pal: init failed\n"); return 1; }
	if (snes_host_load_rom(rom, NULL, err, sizeof err)) { fprintf(stderr, "pal: load failed: %s\n", err); return 1; }
	const bool pal = snes_host_is_pal() != 0;
	const double console_hz = pal ? 50.0070 : 60.0988, vita_hz = 59.94;
	FramePace fp;
	if (pal) fp.set(console_hz, vita_hz);
	const int refreshes = 3000;
	int frames = 0, run = 0, max_gap = 0, gap = 0, sixth_ok = 1;
	for (int r = 0; r < refreshes; r++) {
		int n = pal ? fp.owed(1) : 1;
		if (n > 1) sixth_ok = 0;
		if (n == 0) { if (++gap > max_gap) max_gap = gap; } else gap = 0;
		for (int i = 0; i < n; i++) { snes_host_set_render(1); snes_host_run_frame(); frames++; run++; }
	}
	const double emu_s = frames / console_hz, wall_s = refreshes / vita_hz;
	const double err_pct = 100.0 * (emu_s - wall_s) / wall_s;
	fprintf(stderr, "pal: %s, %d refreshes, %d frames run, emulated %.3f s, refresh time %.3f s, error %+.3f %%, longest skip %d\n",
	    pal ? "PAL" : "NTSC", refreshes, frames, emu_s, wall_s, err_pct, max_gap);
	EXPECT(sixth_ok);
	if (pal) {
		EXPECT(err_pct > -0.5 && err_pct < 0.5);
		EXPECT(max_gap == 1);
		EXPECT(frames >= 2490 && frames <= 2510);
	} else {
		EXPECT(frames == refreshes && max_gap == 0);   /* NTSC: untouched, one per refresh */
	}
	if (pal) {
		/* Smooth 50 Hz (pk_palsmooth.h): the same frames to within one at every refresh; a
		 * refresh that runs none is the one after an early frame (never two in a row) */
		FramePace plain; plain.set(console_hz, vita_hz);
		pk::PalSmooth sm; sm.set(console_hz, vita_hz);
		int fa = 0, fb = 0, worst = 0, early = 0, two_idle = 0, idle = 0;
		for (int r = 0; r < refreshes; r++) {
			bool e; fa += plain.owed(1); const int k = sm.step(&e); fb += k; early += e ? 1 : 0;
			const int d = fb > fa ? fb - fa : fa - fb; if (d > worst) worst = d;
			if (k == 0) { if (++idle > 1) two_idle++; } else idle = 0;
		}
		fprintf(stderr, "pal: smooth 50 Hz runs %d frames (plain %d), at most %d apart, %d early\n", fb, fa, worst, early);
		EXPECT(worst <= 1 && fb >= fa - 1 && fb <= fa + 1 && two_idle == 0 && early > 450 && early < 550);
	}
	snes_host_unload();
	fprintf(stderr, "player_twin: pal %d checks, %d failed\n", g_checks, g_fail);
	return g_fail ? 1 : 0;
}

/* The look governor in the player (playerkit/INTEGRATION.md section 8): a script of loads gives the same sequence of looks
 * every time; the ladder goes interlace blend, then hi-res blend (the wide view is never a rung); a row set by hand is never touched; the settings the
 * player saved are not the eased ones; the game's memory is written, read back at the same clock and ignored at another. */
static int run_look_test(const char *rom, const std::string &work)
{
	char a0[] = "app0:snes.bin";
	std::string arg = std::string(rom);
	char *av[] = { a0, &arg[0], 0 };
	const std::string game = path_stem(rom);
	std::string bucket_dir;

	/* one run: bad seconds, then good ones; returns the picture's (interlace, hires, wide) after every second */
	auto run = [&](const std::string &root, int bad_s, int good_s, bool hand_hires, int mhz, std::string *seq, bool keep_open) -> snesp::SnesPlayer * {
		make_dirs(root);
		Rig *rig = new Rig(root + "/shots");
		snesp::SnesPlayer *pl = new snesp::SnesPlayer(&rig->pf);
		rig->pl = pl; rig->pf.player = pl;
		rig->pf.log_path = root + "/snes/log.txt";
		snesp::StartResult sr = pl->start(2, av, root, true);
		EXPECT(sr.ok);
		pl->apply_start_mode();
		if (hand_hires) pl->on_change(snesp::SnesPlayer::ROW_HIRES, 0);
		pl->look_start(mhz);
		auto sec = [&](float emu) {
			pk::LookSecond ls;
			ls.emu = emu; ls.draw = 0.3f; ls.refreshes = 60; ls.parallel = true;
			pl->look_second(ls);
			for (int i = 0; i < 3; i++) rig->game_frame();
			if (seq) *seq += (char) ('0' + pl->picture().interlace_blend) , *seq += (char) ('0' + pl->picture().hires_blend), *seq += (char) ('0' + pl->picture().wide), *seq += ' ';
		};
		for (int i = 0; i < bad_s; i++) sec(1.25f);
		for (int i = 0; i < good_s; i++) sec(0.35f);
		(void) keep_open;
		return pl;
	};

	/* 1. a script of loads gives the same looks twice, and the ladder goes in order */
	std::string a, b;
	{
		snesp::SnesPlayer *pl = run(work + "/look1", 40, 0, false, 444, &a, false);
		EXPECT(pl->look().level() == 3);                 /* the pixel art scaler first, then the interlace and the hi-res blend */
		EXPECT(pl->look().applied(snesp::SnesPlayer::LOOK_SCALER) && pl->look().applied(snesp::SnesPlayer::LOOK_ILACE) && pl->look().applied(snesp::SnesPlayer::LOOK_HIRES));
		EXPECT(pl->picture().interlace_blend == 0 && pl->picture().hires_blend == 0 && pl->picture().wide == 0);   /* wide stays Auto */
		pl->finish(EXIT_NORMAL);
	}
	{
		snesp::SnesPlayer *pl = run(work + "/look2", 40, 0, false, 444, &b, false);
		pl->finish(EXIT_NORMAL);
	}
	fprintf(stderr, "look sequence: %s\n", a.c_str());
	EXPECT(a == b);
	EXPECT(a.compare(0, 3, "110") == 0);        /* starts at the best look */
	{
		/* a long overload at the start takes every rung the learned costs say are needed at once (here both), never the wide view */
		EXPECT(a.find("000") != std::string::npos);
		EXPECT(a.find("002") == std::string::npos && a.find("012") == std::string::npos);
	}

	/* 2. a row set by hand is never touched, and the files keep the player's own values */
	{
		std::string c;
		snesp::SnesPlayer *pl = run(work + "/look3", 40, 0, true, 444, &c, false);
		EXPECT(pl->look().level() == 2);                 /* the scaler and the interlace blend: hi-res blend was set to Off by hand */
		EXPECT(pl->picture().hires_blend == 0 && pl->picture().interlace_blend == 0 && pl->picture().wide == 0);
		EXPECT(!pl->look().applied(snesp::SnesPlayer::LOOK_HIRES));
		pl->finish(EXIT_NORMAL);
	}

	/* 3. memory: an overload that was cured is written; the next start at the same clock begins with it; another clock does not */
	{
		std::string d;
		snesp::SnesPlayer *pl = run(work + "/look4", 20, 30, false, 444, &d, false);
		fprintf(stderr, "look sequence (cured): %s\n", d.c_str());
		pl->finish(EXIT_NORMAL);
	}
	{
		std::string mem;
		std::vector<std::string> found;
		for (int x = 0; x < 256; x++) {
			char buf[16]; snprintf(buf, sizeof buf, "%02x", x);
			std::string f = work + "/look4/look/snes/" + buf + "/" + game + ".look";
			if (!read_bytes(f).empty()) { found.push_back(f); mem = std::string((const char *) read_bytes(f).data(), read_bytes(f).size()); }
		}
		EXPECT(found.size() == 1);
		EXPECT(mem.compare(0, 9, "pk-look 1") == 0);
		if (!found.empty()) {
			/* a start at the same clock begins with the remembered look; at another clock with the best look */
			std::string e;
			snesp::SnesPlayer *pl = run(work + "/look4", 0, 0, false, 444, &e, false);
			EXPECT(pl->look().mask() != 0);
			pl->finish(EXIT_NORMAL);
			snesp::SnesPlayer *p2 = run(work + "/look4", 0, 0, false, 333, &e, false);
			EXPECT(p2->look().mask() == 0 && p2->picture().interlace_blend == 1 && p2->picture().hires_blend == 1 && p2->picture().wide == 0);
			p2->finish(EXIT_NORMAL);
		}
	}

	/* 4. the old look_mode key (launch file, session runner): Full never eases, Light has the blends off at once (shown as Original); both and PAL games speed are kept for the next start */
	{
		const std::string root = work + "/look5";
		make_dirs(root);
		{
			Rig *rig = new Rig(root + "/shots");
			snesp::SnesPlayer *pl = new snesp::SnesPlayer(&rig->pf);
			rig->pl = pl; rig->pf.player = pl;
			rig->pf.log_path = root + "/snes/log.txt";
			EXPECT(pl->start(2, av, root, true).ok);
			pl->apply_start_mode();
			EXPECT(!pl->pal_fast() && pl->pal_smooth() && pl->look_mode() == 0);          /* defaults: True 50 Hz, Auto */
			pl->look_start(444);
			pl->on_change(snesp::SnesPlayer::ROW_LOOK, 1);             /* Full */
			for (int i = 0; i < 20; i++) { pk::LookSecond ls; ls.emu = 1.25f; ls.draw = 0.3f; ls.refreshes = 60; ls.parallel = true; pl->look_second(ls); }
			EXPECT(pl->picture().interlace_blend == 1 && pl->picture().hires_blend == 1);
			pl->on_change(snesp::SnesPlayer::ROW_LOOK, 2);             /* Light */
			EXPECT(pl->picture().interlace_blend == 0 && pl->picture().hires_blend == 0);
			pl->on_change(snesp::SnesPlayer::ROW_PALSPEED, 2);                 /* the menu lists Smooth 50 Hz, True 50 Hz, Fast 60 Hz */
			pl->finish(EXIT_NORMAL);
		}
		{
			Rig *rig = new Rig(root + "/shots2");
			snesp::SnesPlayer *pl = new snesp::SnesPlayer(&rig->pf);
			rig->pl = pl; rig->pf.player = pl;
			rig->pf.log_path = root + "/snes/log.txt";
			EXPECT(pl->start(2, av, root, true).ok);
			EXPECT(pl->pal_fast() && pl->look_mode() == 2);
			EXPECT(pl->picture().interlace_blend == 0 && pl->picture().hires_blend == 0);
			EXPECT(pl->menu().value(ROW_PIC_QUALITY) == IMGQ_ORIGINAL && pl->menu().value(snesp::SnesPlayer::ROW_PALSPEED) == 2);     /* the Light look shows as Original */
			pl->finish(EXIT_NORMAL);
		}
	}
	/* 5. an older file that says "no scaler" with the Smooth video filter drew bilinear: Original draws sharp (as in the main app), so the choice is kept
	 * the way the app keeps it (scaler to Auto); the old look_mode key 1 (Full) reads as Auto; Original chosen in the menu is stored sharp and stays */
	for (int variant = 0; variant < 3; variant++) {
		const std::string root = work + "/look6_" + std::to_string(variant);
		make_dirs(root);
		pic_settings old; pic_defaults(&old);
		old.scaler = variant == 2 ? SCALER_AUTO : SCALER_OFF;
		old.quality = PIC_SMOOTH;
		make_dirs(root + "/snes");
		EXPECT(pic_save((root + "/snes/picture.ini").c_str(), &old) == 0);
		if (variant == 1) EXPECT(write_text(root + "/snes/player.ini", "look_mode=2\n"));       /* the old Light look, global */
		if (variant == 2) EXPECT(write_text(root + "/snes/player.ini", "look_mode=1\n"));       /* the old Full look */
		Rig *rig = new Rig(root + "/shots");
		snesp::SnesPlayer *pl = new snesp::SnesPlayer(&rig->pf);
		rig->pl = pl; rig->pf.player = pl;
		rig->pf.log_path = root + "/snes/log.txt";
		EXPECT(pl->start(2, av, root, true).ok);
		pl->apply_start_mode();
		EXPECT(pl->look_mode() == 0);
		if (variant == 2) {
			EXPECT(pl->menu().value(ROW_PIC_QUALITY) == IMGQ_AUTO && pl->picture().quality == PIC_SMOOTH);
		} else {
			EXPECT(pl->menu().value(ROW_PIC_QUALITY) == IMGQ_AUTO);                       /* kept bilinear: Auto, not Original */
			EXPECT(pl->picture().quality == PIC_SMOOTH && pl->picture().scaler != SCALER_OFF);
		}
		/* Original by hand: sharp, stored sharp; the next start does not move it */
		pl->on_change(ROW_PIC_QUALITY, IMGQ_ORIGINAL);
		EXPECT(pl->picture().scaler == SCALER_OFF && pl->picture().quality == PIC_SHARP);
		pl->finish(EXIT_NORMAL);
		Rig *rig2 = new Rig(root + "/shots2");
		snesp::SnesPlayer *p2 = new snesp::SnesPlayer(&rig2->pf);
		rig2->pl = p2; rig2->pf.player = p2;
		rig2->pf.log_path = root + "/snes/log.txt";
		EXPECT(p2->start(2, av, root, true).ok);
		EXPECT(p2->menu().value(ROW_PIC_QUALITY) == IMGQ_ORIGINAL && p2->picture().scaler == SCALER_OFF);
		p2->finish(EXIT_NORMAL);
	}
	fprintf(stderr, "player_twin: look %d checks, %d failed\n", g_checks, g_fail);
	return g_fail ? 1 : 0;
}

int run_handoff_test(const char *rom, const std::string &work);   /* handoff_proof.cpp */

/* ---- rewind, fast forward, patches, RetroAchievements ---------------------------------------------------------------------- */

static uint64_t fnv64(const uint8_t *p, size_t n)
{
	uint64_t h = 1469598103934665603ull;
	for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; }
	return h;
}
/* the whole machine as one number: the core's own save state, hashed */
static uint64_t machine_sig()
{
	static std::vector<uint8_t> st;
	st.resize(snes_host_state_size());
	size_t n = 0;
	if (snes_host_state_save_buf(st.data(), st.size(), &n)) return 0;
	return fnv64(st.data(), n);
}
static uint32_t rom_crc()
{
	size_t n = 0;
	const uint8_t *img = snes_host_rom_image(&n);
	return img ? crc32(0, img, n) : 0;
}

struct Sess {
	Rig rig;
	snesp::SnesPlayer *pl;
	std::string root;
	explicit Sess(const std::string &shots) : rig(shots), pl(0) {}
	bool start(const std::string &root_, const std::string &con, const char *rom, const std::string &extra, const std::string &server = "")
	{
		(void) con;
		root = root_;
		make_dirs(root + "/launch");
		make_dirs(root + "/saves");
		std::string ini = "version=1\nsession_id=s-sp\nconsole=snes\ngame=" + std::string(rom) + "\nsave=" + root + "/saves/" + path_stem(rom) +
		    ".srm\nroot=" + root + "\nstart=new\nslot=0\ncpu_mhz=444\nenter_button=circle\nlanguage=en\n" + extra;
		write_text(root + "/launch/snes.ini", ini);
		rig.pf.log_path = root + "/snes/log.txt";
		pl = new snesp::SnesPlayer(&rig.pf);
		rig.pl = pl;
		rig.pf.player = pl;
		if (!server.empty()) pl->set_ra_server(server);
		char a0[] = "app0:snes.bin";
		std::string r = rom;
		char *argv[] = { a0, &r[0], 0 };
		snesp::StartResult sr = pl->start(2, argv, root, true);
		if (!sr.ok) { fprintf(stderr, "start: %s / %s\n", sr.fatal1.c_str(), sr.fatal2.c_str()); return false; }
		pl->apply_start_mode();
		return true;
	}
	void end()
	{
		if (!pl) return;
		pl->finish(EXIT_NORMAL);
		delete pl;
		pl = 0;
		snes_host_unload();
	}
	~Sess() { end(); }
};

/* ---- Quick response (run-ahead by one frame) through the player -------------------------------------------------------------- */

/* One session with a button script (presses, a fast forward, a rewind, a quick save and load); returns the machine after every refresh
 * (snes_host_machine_digest: a frame that is not drawn leaves one drawing flag of the saved state otherwise, skip_check.sh). */
static std::vector<uint64_t> ahead_session(const std::string &root, const char *rom, int quick, uint32_t emu_cost, unsigned *ahead_frames,
                                           unsigned *audio, unsigned *ff_ahead, std::string *line, std::string *ini)
{
	std::vector<uint64_t> H;
	Sess s(root + "/shots");
	EXPECT(s.start(root, "snes", rom, strf("ovr.quick_response=%d\n", quick)));
	if (!s.pl) return H;
	Rig &rig = s.rig;
	rig.emu_cost_us = emu_cost;
	EXPECT(s.pl->quick() == quick);
	for (int f = 0; f < 900; f++) {
		uint32_t b = 0;
		if (f % 50 >= 20 && f % 50 < 30) b |= PH_CROSS;
		if (f % 70 >= 35 && f % 70 < 50) b |= PH_RIGHT;
		if (f % 110 == 60) b |= PH_START;
		if (f >= 300 && f < 340) b = PH_RS_RIGHT;           /* fast forward */
		if (f >= 500 && f < 560) b = PH_RS_LEFT;            /* rewind */
		if (f == 700) b = PH_SELECT;
		if (f == 701) b = PH_SELECT | PH_L;                 /* quick save */
		if (f == 760) b = PH_SELECT;
		if (f == 761) b = PH_SELECT | PH_R;                 /* quick load */
		const unsigned a0 = rig.ahead_frames;
		rig.game_frame(b);
		if (f >= 302 && f < 338 && rig.ahead_frames != a0) (*ff_ahead)++;
		if (f == 702 || f == 762) s.pl->wait_writer();
		H.push_back(snes_host_machine_digest());
	}
	*ahead_frames = rig.ahead_frames;
	*audio = rig.audio_out;
	*line = s.pl->ahead_line();
	s.end();
	std::vector<uint8_t> b = read_bytes(root + "/snes/player.ini");
	ini->assign(b.begin(), b.end());
	return H;
}

static int run_ahead_test(const char *rom, const std::string &work)
{
	fprintf(stderr, "player_twin ahead: ROM %s\n", rom);
	make_dirs(work);
	unsigned a_off = 0, a_on = 0, a_auto = 0, a_heavy = 0, au_off = 0, au_on = 0, au_auto = 0, au_heavy = 0, ff_off = 0, ff_on = 0, ff_auto = 0, ff_heavy = 0;
	std::string l_off, l_on, l_auto, l_heavy, i_off, i_on, i_auto, i_heavy;
	/* Off, On, Auto on a light game (2 ms a frame) and Auto on a heavy one (9 ms): the machine after every refresh must be the same in all */
	std::vector<uint64_t> off = ahead_session(work + "/off", rom, 0, 2000, &a_off, &au_off, &ff_off, &l_off, &i_off);
	std::vector<uint64_t> on = ahead_session(work + "/on", rom, 2, 2000, &a_on, &au_on, &ff_on, &l_on, &i_on);
	std::vector<uint64_t> au = ahead_session(work + "/auto", rom, 1, 2000, &a_auto, &au_auto, &ff_auto, &l_auto, &i_auto);
	std::vector<uint64_t> hv = ahead_session(work + "/heavy", rom, 1, 9000, &a_heavy, &au_heavy, &ff_heavy, &l_heavy, &i_heavy);
	fprintf(stderr, "off: %s\non: %s\nauto: %s\nauto, heavy: %s\n", l_off.c_str(), l_on.c_str(), l_auto.c_str(), l_heavy.c_str());
	fprintf(stderr, "frames run ahead: off %u, on %u, auto %u, auto on a heavy game %u; sound frames out %u / %u / %u / %u\n", a_off, a_on, a_auto, a_heavy, au_off, au_on, au_auto, au_heavy);
	EXPECT(off.size() == 900 && on.size() == 900 && au.size() == 900 && hv.size() == 900);
	size_t d_on = 0, d_au = 0, d_hv = 0;
	for (size_t i = 0; i < off.size() && i < on.size() && i < au.size() && i < hv.size(); i++) { if (off[i] != on[i]) d_on++; if (off[i] != au[i]) d_au++; if (off[i] != hv[i]) d_hv++; }
	fprintf(stderr, "refreshes whose machine differs from Off: on %zu, auto %zu, auto heavy %zu\n", d_on, d_au, d_hv);
	EXPECT(d_on == 0);                                      /* the machine is the same after every refresh, through fast forward, rewind and a state load */
	EXPECT(d_au == 0);
	EXPECT(d_hv == 0);
	EXPECT(au_off == au_on && au_off == au_auto && au_off == au_heavy);      /* and so is the amount of sound */
	EXPECT(a_off == 0);
	EXPECT(a_on > 700);                                     /* every normal frame (not the fast forward and rewind ones) */
	EXPECT(ff_on == 0 && ff_auto == 0);                     /* never under fast forward */
	EXPECT(a_auto > 400 && a_auto < a_on);                  /* Auto: after it has seen what the game costs, and again after each fast forward / rewind */
	EXPECT(a_heavy == 0);                                   /* Auto leaves a heavy game alone */
	/* a launch's override is for the session it came with: player.ini keeps the player's own choice */
	EXPECT(i_on.find("quick_response=2") == std::string::npos);
	{
		/* the row: Speed page, Off / Auto / On, Auto by default; a change is kept */
		Sess s(work + "/row/shots");
		EXPECT(s.start(work + "/row", "snes", rom, ""));
		if (s.pl) {
			EXPECT(s.pl->quick() == 1);
			EXPECT(s.pl->menu().value(snesp::SnesPlayer::ROW_QUICK) == 1);
			s.pl->on_change(snesp::SnesPlayer::ROW_QUICK, 0);
			EXPECT(s.pl->quick() == 0 && !s.pl->ahead().want());
			s.rig.run(3);
		}
		s.end();
		std::vector<uint8_t> b = read_bytes(work + "/row/snes/player.ini");
		EXPECT(std::string(b.begin(), b.end()).find("quick_response=0") != std::string::npos);
	}
	/* the picture side (the hand-over's drawing thread): the player forwards it to the governor; a saturated picture side holds Auto off, a quiet one lets it start */
	{
		Sess s(work + "/qpic/shots");
		EXPECT(s.start(work + "/qpic", "snes", rom, "ovr.quick_response=1\n"));
		if (s.pl) {
			Rig &rig = s.rig;
			rig.emu_cost_us = 3000;
			for (int i = 0; i < 300; i++) { rig.game_frame(0); s.pl->ahead_picture(17500, 0); }       /* xBRZ saturating the drawing thread */
			EXPECT(!s.pl->ahead().on() && s.pl->ahead().picture_us() > 16000);
			EXPECT(s.pl->ahead_line().find("pic_ms=") != std::string::npos);
			s.pl->ahead_picture_reset();                                                              /* an Image quality change */
			EXPECT(s.pl->ahead().picture_us() == 0);
			for (int i = 0; i < 700; i++) { rig.game_frame(0); s.pl->ahead_picture(5000, 0); }
			EXPECT(s.pl->ahead().on());
			/* the Super FX chip slowed down (main.cpp's chip pacing): Quick response holds off */
			s.pl->set_ahead_hold(true);
			rig.game_frame(0);
			EXPECT(!s.pl->ahead().want());
			s.pl->set_ahead_hold(false);
		}
	}
	fprintf(stderr, "player_twin ahead: %d checks, %d failed\n", g_checks, g_fail);
	return g_fail ? 1 : 0;
}

static int run_speed_test(const char *rom, const std::string &work)
{
	const std::string con = "snes";
	fprintf(stderr, "player_twin speed: ROM %s\n", rom);
	make_dirs(work);
	/* a one byte patch (a header field no core reads) */
	std::vector<uint8_t> ips;
	ips.insert(ips.end(), (const uint8_t *) "PATCH", (const uint8_t *) "PATCH" + 5);
	const unsigned po = 0x0D;                  /* the operand the game writes to $7E0011 */
	uint8_t rec[] = { 0x00, 0x00, (uint8_t) po, 0x00, 0x01, 0x5A };
	ips.insert(ips.end(), rec, rec + sizeof rec);
	ips.insert(ips.end(), (const uint8_t *) "EOF", (const uint8_t *) "EOF" + 3);
	EXPECT(write_bytes(work + "/fix.ips", ips));

	/* ---- the patch changes the ROM the core runs (its CRC), the launch keys patch / patch_key as FORMATS.md says ---- */
	uint32_t crc_plain = 0, crc_patched = 0;
	{
		Sess a(work + "/shots");
		EXPECT(a.start(work + "/r1", con, rom, ""));
		crc_plain = rom_crc();
		a.rig.run(5);
	}
	{
		Sess b(work + "/shots");
		EXPECT(b.start(work + "/r2", con, rom, "patch=" + work + "/fix.ips\npatch_key=fix-1\n"));
		crc_patched = rom_crc();
		b.rig.run(5);
		{ size_t n = 0; const uint8_t *img = snes_host_rom_image(&n); EXPECT(img && n > po && img[po] == 0x5A); }
	}
	fprintf(stderr, "ROM CRC plain %08x, patched %08x\n", crc_plain, crc_patched);
	EXPECT(crc_plain != 0 && crc_patched != 0 && crc_plain != crc_patched);

	Sess s(work + "/shots");
	EXPECT(s.start(work + "/r3", con, rom, ""));
	Rig &rig = s.rig;
	snesp::SnesPlayer *pl = s.pl;
	EXPECT(pl->speed().rewind().is_open());
	rig.run(30);

	/* ---- rewind 5 seconds: the machine is bit for bit where it was, and goes on the same way ---- */
	const int N = 600;
	std::vector<uint64_t> H;
	for (int i = 0; i < N; i++) { rig.game_frame(); H.push_back(machine_sig()); }
	const unsigned hist = pl->speed().rewind().history_frames();
	fprintf(stderr, "rewind: %u frames of history in %u steps, interval %u, ring %u bytes used\n", hist, pl->speed().rewind().entries(),
	        pl->speed().rewind().interval(), (unsigned) pl->speed().rewind().stats().used_bytes);
	EXPECT(hist >= (unsigned) 5 * 60);
	int held = 0;
	while (held < 900 && !pl->speed().rewinding()) { rig.game_frame(PH_RS_LEFT); held++; }     /* the control is read for the refresh after it */
	EXPECT(pl->speed().rewinding());
	const unsigned audio0 = rig.audio_out;
	while (held < 900 && pl->speed().rewind().back_seconds() < 5 && pl->speed().rewinding()) { rig.game_frame(PH_RS_LEFT); held++; }
	const unsigned back = pl->speed().rewind().back_seconds();
	EXPECT(pl->speed().rewinding());
	EXPECT(back >= 5);
	EXPECT(rig.audio_out == audio0);                        /* silent while rewinding */
	EXPECT(pl->speed().chip().find("REWIND") != std::string::npos);
	for (int k = 0; k < 4 && pl->speed().rewinding(); k++) rig.game_frame(0);       /* the control is let go: the game carries on from the frame on screen */
	EXPECT(!pl->speed().rewinding());
	/* where it stands is a state seen before: the one `back` seconds ago (a landing is the stored state plus one silent frame) */
	uint64_t now_sig = machine_sig();
	int at = -1;
	for (int i = 0; i < N; i++) if (H[(size_t) i] == now_sig) at = i;
	fprintf(stderr, "rewind: held %d refreshes, %u s back, the machine is at recorded frame %d of %d (target about %d)\n", held, back, at, N, N - (int) back * 60);
	EXPECT(at >= 0);
	EXPECT(at >= N - (int) back * 60 - 70 && at <= N - (int) back * 60 + 70);
	/* and from there the game runs exactly as it did the first time */
	bool same = at >= 0;
	for (int k = 1; k <= 40 && at >= 0 && at + k < N; k++) { rig.game_frame(); if (machine_sig() != H[(size_t) (at + k)]) same = false; }
	EXPECT(same);
	/* the oldest moment: a notice, no crash */
	for (int i = 0; i < 4000 && pl->speed().rewind().history_frames() > 0; i++) rig.game_frame(PH_RS_LEFT);
	rig.game_frame(0);
	rig.run(10);

	/* ---- fast forward: 2x and 3x frames per refresh, sound mixed down to the real time count ---- */
	rig.run(30);
	for (int factor = 2; factor <= 3; factor++) {
		pl->on_change(pk::ROW_SPD_FF, factor == 3 ? 1 : 0);
		unsigned r0 = rig.ran_frames, f0 = rig.refreshes, ai0 = rig.audio_in, ao0 = rig.audio_out;
		rig.game_frame(PH_RS_RIGHT);                        /* the press itself */
		r0 = rig.ran_frames; f0 = rig.refreshes; ai0 = rig.audio_in; ao0 = rig.audio_out;
		for (int i = 0; i < 60; i++) rig.game_frame(PH_RS_RIGHT);
		const unsigned ran = rig.ran_frames - r0, refr = rig.refreshes - f0;
		fprintf(stderr, "fast forward %dx: %u frames in %u refreshes; audio in %u frames, out %u\n", factor, ran, refr, rig.audio_in - ai0, rig.audio_out - ao0);
		EXPECT(pl->speed().ff_on());
		EXPECT(ran == (unsigned) factor * refr);
		EXPECT(pl->speed().chip() == (factor == 3 ? ">> 3x" : ">> 2x"));
		/* the sound plays at its normal length: about 1 / factor of what the core made */
		const double want = (rig.audio_in - ai0) / (double) factor;
		EXPECT((rig.audio_out - ao0) > want * 0.97 && (rig.audio_out - ao0) < want * 1.03);
		rig.game_frame(0);
		EXPECT(!pl->speed().ff_on());
		rig.run(5);
	}
	/* without sound: silence of the same length (the audio never runs dry) */
	pl->on_change(pk::ROW_SPD_SOUND, 0);
	{
		unsigned ai0 = rig.audio_in, ao0 = rig.audio_out;
		for (int i = 0; i < 30; i++) rig.game_frame(PH_RS_RIGHT);
		const double want = (rig.audio_in - ai0) / 3.0;
		EXPECT((rig.audio_out - ao0) > want * 0.97 && (rig.audio_out - ao0) < want * 1.03);
		rig.game_frame(0);
	}
	/* Unlimited and Unlimited Smooth (the main app's choices of the same names): more frames than 3x fit in a refresh, silent, one frame's worth of
	 * audio a refresh, a picture every second refresh / every refresh; the clock of the twin does not move inside a refresh, so the count is the
	 * governor's first guess (3 frames) and the test is about the shape, not the speed */
	pl->on_change(pk::ROW_SPD_SOUND, 1);
	for (int kind = 2; kind <= 3; kind++) {
		pl->on_change(pk::ROW_SPD_FF, kind);
		rig.game_frame(PH_RS_RIGHT);
		const unsigned r0 = rig.ran_frames, f0 = rig.refreshes, ai0 = rig.audio_in, ao0 = rig.audio_out, d0 = rig.drawn_frames;
		for (int i = 0; i < 60; i++) rig.game_frame(PH_RS_RIGHT);
		const unsigned ran = rig.ran_frames - r0, refr = rig.refreshes - f0, drawn = rig.drawn_frames - d0;
		fprintf(stderr, "fast forward %s: %u frames in %u refreshes, %u drawn; audio in %u frames, out %u\n", kind == 2 ? "Unlimited" : "Unlimited Smooth", ran, refr, drawn, rig.audio_in - ai0, rig.audio_out - ao0);
		EXPECT(pl->speed().ff_on());
		EXPECT(ran >= 3 * refr && ran <= 30 * refr);
		EXPECT(pl->speed().chip() == (kind == 3 ? ">> Unlimited Smooth" : ">> Unlimited"));
		EXPECT(drawn == (kind == 3 ? refr : refr / 2));
		const double per_frame = (rig.audio_in - ai0) / (double) ran;      /* silence: one frame's worth a refresh, whatever the count */
		EXPECT((rig.audio_out - ao0) > per_frame * refr * 0.97 && (rig.audio_out - ao0) < per_frame * refr * 1.03);
		{
			/* the release: that refresh was planned while it ran, so it still runs its frames; but their audio is one frame's worth of silence and
			 * only the last frame is drawn, not a dump of every frame's sound and picture */
			const unsigned rr0 = rig.ran_frames, ao1 = rig.audio_out, dd0 = rig.drawn_frames;
			rig.game_frame(0);
			EXPECT(!pl->speed().ff_on());
			EXPECT(rig.ran_frames - rr0 >= 2);
			EXPECT((rig.audio_out - ao1) <= per_frame * 1.05);
			EXPECT(rig.drawn_frames - dd0 <= 1);
		}
		const unsigned r1 = rig.ran_frames, f1 = rig.refreshes;
		rig.run(5);
		EXPECT(rig.ran_frames - r1 == rig.refreshes - f1);                /* back to one frame a refresh */
	}
	pl->on_change(pk::ROW_SPD_FF, 0);
	/* toggle mode: one press switches it on, a second off */
	pl->on_change(pk::ROW_SPD_SOUND, 1);
	pl->on_change(pk::ROW_SPD_MODE, 1);
	rig.game_frame(PH_RS_RIGHT);
	rig.game_frame(0);
	rig.run(5);
	EXPECT(pl->speed().ff_on());
	rig.game_frame(PH_RS_RIGHT);
	rig.game_frame(0);
	rig.run(3);
	EXPECT(!pl->speed().ff_on());
	fprintf(stderr, "player_twin speed: %d checks, %d failed\n", g_checks, g_fail);
	return g_fail ? 1 : 0;
}

static int run_ra_test(const char *rom, const std::string &work)
{
	const std::string con = "snes";
	fprintf(stderr, "player_twin ra: ROM %s\n", rom);
	make_dirs(work);
	const int console = pk::RA_SNES;
	std::vector<uint8_t> rb = read_bytes(rom);
	char hash[33];
	EXPECT(pk::Ra::hash_rom(rb.data(), rb.size(), console, hash));
	const char *mem = "0xH000010=7";                  /* $7E0010: the test pokes it (the game sets it once, to 5) */

	pk_test::FakeRa srv;
	srv.md5 = hash;
	pk_test::FakeAch a1 = { 1001, "First Change", "The counter moves", 5, mem };
	pk_test::FakeAch a2 = { 1002, "Never", "Cannot be earned", 10, "0xH000000=77_0xH000000=78" };
	srv.achs.push_back(a1);
	srv.achs.push_back(a2);
	EXPECT(srv.start());

	auto account = [&](const std::string &root, int hardcore) {
		make_dirs(root);
		return write_text(root + "/retroachievements.txt", "# test\nuser=ra_tester\ntoken=tok-1\nhardcore=" + std::to_string(hardcore) + "\nindicators=1\n");
	};
	auto seen_has = [](const std::vector<std::string> &v, const char *sub) { for (size_t i = 0; i < v.size(); i++) if (v[i].find(sub) != std::string::npos) return true; return false; };
	auto frames_until = [&](Sess &x, const std::function<bool()> &done, std::vector<std::string> *seen, int max, uint32_t buttons = 0) {
		for (int i = 0; i < max; i++) {
			x.rig.game_frame(buttons);
			if (seen && x.pl->notices().active()) { const std::string &t = x.pl->notices().text(); if (seen->empty() || seen->back() != t) seen->push_back(t); }
			if (done()) return true;
			usleep(3000);
		}
		return false;
	};

	/* ---- softcore: the stored token signs in, the game is found by its hash, the change is an unlock: a toast, and sent ---- */
	{
		const std::string root = work + "/ra_soft";
		EXPECT(account(root, 0));
		const std::vector<uint8_t> before = read_bytes(root + "/retroachievements.txt");
		std::vector<std::string> seen;
		Sess s(work + "/shots");
		EXPECT(s.start(root, con, rom, "", srv.url()));
		EXPECT(s.pl->ra().up());
		EXPECT(frames_until(s, [&] { return s.pl->ra().game_state() != 0; }, &seen, 800));
		EXPECT(s.pl->ra().game_state() == 1);
		EXPECT(srv.count("login2") == 1);
		EXPECT(s.pl->ra().total() == 2);
		s.rig.run(30);
		snes_host_poke_wram(0x10, 7);
		EXPECT(frames_until(s, [&] { return srv.count("awardachievement") >= 1; }, &seen, 800));
		EXPECT(frames_until(s, [&] { return seen_has(seen, "Achievement: First Change"); }, &seen, 600));     /* the toasts come one at a time */
		fprintf(stderr, "ra: toasts seen: ");
		for (size_t i = 0; i < seen.size(); i++) fprintf(stderr, "[%s] ", seen[i].c_str());
		fprintf(stderr, "\n");
		EXPECT(seen_has(seen, "1 of 2") || seen_has(seen, "0 of 2"));
		EXPECT(seen_has(seen, "Achievement: First Change (5)"));
		std::vector<std::map<std::string, std::string> > cs = srv.calls();
		int award = 0;
		for (size_t i = 0; i < cs.size(); i++) if (cs[i]["r"] == "awardachievement") { award++; EXPECT(cs[i]["a"] == "1001"); EXPECT(cs[i]["h"] == "0"); }
		EXPECT(award == 1);
		/* softcore: the player still rewinds and loads states, and the cheats apply */
		EXPECT(!s.pl->ra().hardcore_blocks());
		EXPECT(s.pl->state_load_refused() == 0);
		EXPECT(s.pl->cheats_refused() == 0);
		EXPECT(read_bytes(root + "/retroachievements.txt") == before);          /* the player never writes the account file */
		EXPECT(s.pl->ra().frames() > 20);
		s.pl->log_flush(true);
		const std::string log = s.rig.pf.log_text;
		EXPECT(log.find("tok-1") == std::string::npos);                           /* the token is never logged */
		EXPECT(log.find("ra: ra_unlock id=1001") != std::string::npos);
		s.end();
		std::vector<uint8_t> lg = read_bytes(root + "/" + con + "/log.txt");
		EXPECT(std::string(lg.begin(), lg.end()).find("tok-1") == std::string::npos);
	}

	/* ---- Hardcore: no rewind, no state loads, no cheats; fast forward stays; the unlock goes out as hardcore ---- */
	srv.clear_calls();
	{
		const std::string root = work + "/ra_hard";
		EXPECT(account(root, 1));
		std::vector<std::string> seen;
		Sess s(work + "/shots");
		EXPECT(s.start(root, con, rom, "", srv.url()));
		EXPECT(s.pl->ra().hardcore_blocks());                                      /* pending counts: refused from the first frame */
		EXPECT(s.pl->state_load_refused() != 0);
		EXPECT(frames_until(s, [&] { return s.pl->ra().game_state() != 0; }, &seen, 800));
		EXPECT(s.pl->ra().hardcore_active());
		EXPECT(s.pl->state_load_refused() != 0);
		EXPECT(s.pl->cheats_refused() != 0);
		EXPECT(std::string(s.pl->state_load_refused()).find("Hardcore") == 0);
		s.rig.run(120);
		EXPECT(s.pl->speed().rewind().history_frames() > 0);                       /* the history is kept, the control is not */
		std::vector<std::string> seen2;
		for (int i = 0; i < 60; i++) {
			s.rig.game_frame(PH_RS_LEFT);
			if (s.pl->notices().active()) { const std::string &t = s.pl->notices().text(); if (seen2.empty() || seen2.back() != t) seen2.push_back(t); }
			EXPECT(!s.pl->speed().rewinding());
		}
		EXPECT(seen_has(seen2, "Hardcore mode: rewind is off"));
		s.rig.game_frame(0);
		/* the menu's state load is refused with the same words, the state stays unloaded */
		std::vector<uint8_t> blob(snes_host_state_size(), 0);
		EXPECT(!s.pl->restore_state(blob.data(), blob.size()));
		/* fast forward stays */
		const unsigned r0 = s.rig.ran_frames, f0 = s.rig.refreshes;
		for (int i = 0; i < 30; i++) s.rig.game_frame(PH_RS_RIGHT);
		EXPECT(s.pl->speed().ff_on());
		EXPECT(s.rig.ran_frames - r0 >= 2 * (s.rig.refreshes - f0 - 1));        /* it starts with the refresh after the press */
		s.rig.game_frame(0);
		snes_host_poke_wram(0x10, 7);
		EXPECT(frames_until(s, [&] { return srv.count("awardachievement") >= 1; }, &seen, 800));
		std::vector<std::map<std::string, std::string> > cs = srv.calls();
		for (size_t i = 0; i < cs.size(); i++) if (cs[i]["r"] == "awardachievement") EXPECT(cs[i]["h"] == "1");
	}

	/* ---- a game the site does not know: Hardcore blocks nothing, and the player says so once ---- */
	{
		pk_test::FakeRa other;
		other.md5 = "00000000000000000000000000000000";
		EXPECT(other.start());
		const std::string root = work + "/ra_unknown";
		EXPECT(account(root, 1));
		std::vector<std::string> seen;
		Sess s(work + "/shots");
		EXPECT(s.start(root, con, rom, "", other.url()));
		EXPECT(frames_until(s, [&] { return s.pl->ra().game_state() != 0; }, &seen, 800));
		EXPECT(s.pl->ra().game_state() == -1);
		s.rig.run(10);
		EXPECT(!s.pl->ra().hardcore_blocks());
		EXPECT(s.pl->state_load_refused() == 0);
		EXPECT(!s.pl->speed().rewind_locked());
	}
	/* ---- no account on file: nothing starts, no request is made ---- */
	srv.clear_calls();
	{
		Sess s(work + "/shots");
		EXPECT(s.start(work + "/ra_none", con, rom, "", srv.url()));
		EXPECT(!s.pl->ra().up());
		s.rig.run(60);
		EXPECT(srv.calls().empty());
	}
	srv.stop();
	fprintf(stderr, "player_twin ra: %d checks, %d failed\n", g_checks, g_fail);
	return g_fail ? 1 : 0;
}

/* ---- what rewind costs per frame (host numbers; under callgrind the instruction counts of the cost_* functions, see docs/PERFORMANCE.md) ---- */
static __attribute__((noinline)) size_t cost_save(std::vector<uint8_t> &st) { size_t n = 0; if (snes_host_rewind_state_save_buf(st.data(), st.size(), &n)) return 0; return n; }
static __attribute__((noinline)) size_t cost_save_full(std::vector<uint8_t> &st) { size_t n = 0; if (snes_host_state_save_buf(st.data(), st.size(), &n)) return 0; return n; }
static __attribute__((noinline)) size_t cost_encode(const std::vector<uint8_t> &a, const std::vector<uint8_t> &b, std::vector<uint8_t> &out)
{
	return pk::delta_encode(a.data(), b.data(), a.size(), out.data(), out.size());
}
static __attribute__((noinline)) bool cost_apply(std::vector<uint8_t> &cur, const uint8_t *d, size_t n) { return pk::delta_apply(cur.data(), cur.size(), d, n); }
static __attribute__((noinline)) void cost_frame(bool render) { snes_host_set_render(render ? 1 : 0); snes_host_run_frame(); }
static double now_ms()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static int run_costs_test(const char *rom)
{
	EXPECT(snes_host_init() == 0);
	char e[128];
	EXPECT(snes_host_load_rom(rom, NULL, e, sizeof e) == 0);
	const int N = 200;
	for (int i = 0; i < 120; i++) cost_frame(true);
	std::vector<std::vector<uint8_t> > S((size_t) N + 2);
	const size_t sz = snes_host_rewind_state_size();
	for (size_t i = 0; i < S.size(); i++) { cost_frame(true); S[i].resize(sz); S[i].resize(cost_save(S[i])); }
	/* state capture: what the game thread does at a capture */
	double t0 = now_ms();
	std::vector<uint8_t> tmp(sz);
	double worst = 0;
	for (int i = 0; i < N; i++) { double a = now_ms(); cost_save(tmp); double b = now_ms() - a; if (b > worst) worst = b; }
	const double save_ms = (now_ms() - t0) / N;
	if (getenv("STATE_BLOCKS")) {        /* the blocks of a state, by name and size */
		const uint8_t *q = S[0].data(), *e = q + S[0].size();
		while (q < e && *q != '\n') q++;
		q++;
		while (q + 11 < e) {
			unsigned len = 0;
			if (q[4] == '-') len = (unsigned) q[7] << 24 | (unsigned) q[8] << 16 | (unsigned) q[9] << 8 | q[10]; else len = (unsigned) atoi((const char *) q + 4);
			fprintf(stderr, "  block %.3s %u\n", (const char *) q, len);
			q += 11 + len;
		}
	}
	/* what a capture cost before: the whole state, screenshot block included */
	const size_t full_sz = snes_host_state_size();
	std::vector<uint8_t> tmp_full(full_sz);
	double tf0 = now_ms();
	for (int i = 0; i < N; i++) cost_save_full(tmp_full);
	const double full_ms = (now_ms() - tf0) / N;
	/* what is restored is unchanged: capture, run 40 frames, load it back, capture again: the same bytes; and both machines then run on alike */
	{
		std::vector<uint8_t> a(sz), b(sz), c(sz), d(sz);
		EXPECT(snes_host_rewind_state_save_buf(a.data(), a.size(), NULL) == 0);
		std::vector<uint8_t> full_a(full_sz);
		EXPECT(snes_host_state_save_buf(full_a.data(), full_a.size(), NULL) == 0);
		for (int i = 0; i < 40; i++) cost_frame(true);
		EXPECT(snes_host_state_load_buf(a.data(), a.size()) == 0);
		EXPECT(snes_host_rewind_state_save_buf(b.data(), b.size(), NULL) == 0);
		EXPECT(a == b);
		EXPECT(a == full_a);                                  /* the same bytes as the whole state */
		for (int i = 0; i < 40; i++) cost_frame(true);
		EXPECT(snes_host_rewind_state_save_buf(c.data(), c.size(), NULL) == 0);
		EXPECT(snes_host_state_load_buf(full_a.data(), full_a.size()) == 0);     /* the same moment through the whole state */
		for (int i = 0; i < 40; i++) cost_frame(true);
		EXPECT(snes_host_rewind_state_save_buf(d.data(), d.size(), NULL) == 0);
		EXPECT(c == d);
		/* every frame of 600: the saved bytes are the whole state's, the part left out is zero in the machine */
		size_t nz = 0; unsigned diff = 0;
		for (int i = 0; i < 600; i++) {
			cost_frame(true);
			if ((i & 1) == 0) continue;
			EXPECT(snes_host_rewind_state_save_buf(c.data(), c.size(), NULL) == 0);
			EXPECT(snes_host_state_save_buf(full_a.data(), full_a.size(), NULL) == 0);
			if (c != full_a) diff++;
			nz += snes_host_rewind_tail_nonzero();
		}
		EXPECT(diff == 0 && nz == 0 && !snes_host_rewind_whole());
		fprintf(stderr, "  600 frames, 300 captures: %u differ from the whole state, %u nonzero bytes in the part left out, whole block %d\n", diff, (unsigned) nz, snes_host_rewind_whole());
		fprintf(stderr, "  restore check: state %u bytes (was %u): load gives the same machine as the whole state, 40 frames on equal\n", (unsigned) sz, (unsigned) full_sz);
		fprintf(stderr, "  cartridge RAM written per capture: %u of %u bytes; captures that checked the rest: %u\n", (unsigned) snes_host_rewind_ext(), 512u * 1024u, snes_host_rewind_scans());
		/* the net under the reasoning: a state with a byte in the part left out (it cannot come from the emulated machine: it is put there
		 * by hand) is loaded; the next capture finds it and writes the whole block, from then on */
		if (snes_host_rewind_ext()) {
			((uint8_t *) snes_host_sram_data())[snes_host_rewind_ext() + 5] = 0x5A;
			EXPECT(snes_host_state_save_buf(full_a.data(), full_a.size(), NULL) == 0);
			EXPECT(snes_host_state_load_buf(full_a.data(), full_a.size()) == 0);
			EXPECT(snes_host_rewind_state_save_buf(c.data(), c.size(), NULL) == 0);
			EXPECT(c == full_a && snes_host_rewind_whole());
			cost_frame(true);
			EXPECT(snes_host_rewind_state_save_buf(c.data(), c.size(), NULL) == 0);
			EXPECT(snes_host_state_save_buf(full_a.data(), full_a.size(), NULL) == 0);
			EXPECT(c == full_a);
			fprintf(stderr, "  a byte put past the written part and a state load: found by the next capture, whole block from then on: %s\n", (c == full_a && snes_host_rewind_whole()) ? "yes" : "NO");
			((uint8_t *) snes_host_sram_data())[snes_host_rewind_ext() + 5] = 0;
			(void) snes_host_rewind_state_size();     /* a new set of buffers for what follows */
		}
	}
	/* the helper's work: the difference of two states two frames apart, squeezed */
	std::vector<uint8_t> out(pk::delta_bound(sz));
	size_t enc_total = 0, enc_max = 0;
	t0 = now_ms();
	double worst_enc = 0;
	for (int i = 0; i < N; i++) { double a = now_ms(); size_t n = cost_encode(S[(size_t) i], S[(size_t) i + 2], out); double b = now_ms() - a; if (b > worst_enc) worst_enc = b; enc_total += n; if (n > enc_max) enc_max = n; }
	const double enc_ms = (now_ms() - t0) / N;
	/* one step back */
	std::vector<uint8_t> cur = S[(size_t) N + 1], keep;
	size_t n = cost_encode(S[(size_t) N - 1], S[(size_t) N + 1], out);
	t0 = now_ms();
	for (int i = 0; i < N; i++) cost_apply(cur, out.data(), n);
	const double apply_ms = (now_ms() - t0) / N;
	/* a whole frame of the core, picture on */
	t0 = now_ms();
	for (int i = 0; i < N; i++) cost_frame(true);
	const double frame_ms = (now_ms() - t0) / N;
	fprintf(stderr, "costs (%s, host x86_64, %d runs, state %u bytes):\n", "snes", N, (unsigned) sz);
	fprintf(stderr, "  capture on the game thread (state save)  avg %.3f ms, worst %.3f ms (the whole state, as before: avg %.3f ms)\n", save_ms, worst, full_ms);
	fprintf(stderr, "  pack on the helper (XOR + squeeze)       avg %.3f ms, worst %.3f ms; a step is %u bytes on average (largest %u)\n", enc_ms, worst_enc, (unsigned) (enc_total / N), (unsigned) enc_max);
	fprintf(stderr, "  one step back (apply)                    avg %.3f ms\n", apply_ms);
	fprintf(stderr, "  one emulated frame of the core           avg %.3f ms\n", frame_ms);
	EXPECT(save_ms < 0.5);
	fprintf(stderr, "player_twin costs: %d checks, %d failed\n", g_checks, g_fail);
	return g_fail ? 1 : 0;
}

int main(int argc, char **argv)
{
	if (argc >= 3 && !strcmp(argv[1], "costs")) return run_costs_test(argv[2]);
	if (argc >= 4 && !strcmp(argv[1], "speed")) return run_speed_test(argv[2], argv[3]);
	if (argc >= 4 && !strcmp(argv[1], "ahead")) return run_ahead_test(argv[2], argv[3]);
	if (argc >= 4 && !strcmp(argv[1], "ra")) return run_ra_test(argv[2], argv[3]);
	if (argc >= 4 && !strcmp(argv[1], "look")) return run_look_test(argv[2], argv[3]);
	if (argc >= 3 && !strcmp(argv[1], "pal")) return run_pal_test(argv[2]);
	if (argc >= 4 && !strcmp(argv[1], "handoff")) return run_handoff_test(argv[2], argv[3]);
	if (argc >= 4 && !strcmp(argv[1], "wide")) { int r = run_wide_test(argv[2], argv[3]); fprintf(stderr, "player_twin: wide %d checks, %d failed\n", g_checks, g_fail); return r || g_fail ? 1 : 0; }
	if (argc >= 5 && !strcmp(argv[1], "test")) return run_test(argv[2], argv[3], argv[4]);
	if (argc >= 5 && !strcmp(argv[1], "bench-saves")) return bench(argv[2], atoi(argv[3]), atoi(argv[4]));
	fprintf(stderr, "usage: player_twin test ROM SHOTS WORK | speed ROM WORK | ra ROM WORK | bench-saves DIR N FILL\n");
	return 2;
}
