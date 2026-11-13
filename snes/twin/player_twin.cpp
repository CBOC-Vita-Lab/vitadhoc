/*
 * player_twin: runs the SNES player's own code (snes/vita/player.cpp: launch file, playerkit menu, states,
 * battery RAM writer, cheats, patches, session report, steady pacing) on Linux with playerkit's software
 * renderer, over a running game, and checks it. Writes a PNG of every menu screen.
 *
 *   player_twin test ROM SHOTS_DIR WORK_DIR      the scripted walk and checks (exit status 1 on a failure)
 *   player_twin bench-saves DIR N FILL [threaded] N state saves (400 KB) into a states folder holding FILL
 *                                                 other files, for counting the card operations (strace -c)
 *
 * ROM is a synthetic cartridge (tests/make_cheat_rom.py). Copyright CBOC-Vita. MIT licence (see snes/frontend/LICENSE).
 */
#include "player.h"
#include "pk_gfx_soft.h"
#include "pk_icons.h"
#include "wide.h"

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
	int nshot;
	Rig(const std::string &s, int first = 0) : pl(0), gfx(), text(&gfx), game_tex(0), shots(s), nshot(first) {}
	~Rig() { if (game_tex) gfx.tex_free(game_tex); }

	void pads_none(PadSample p[MAX_PLAYERS]) { for (int i = 0; i < MAX_PLAYERS; i++) { p[i] = PadSample(); p[i].connected = i < 2; } }
	/* one game frame (pads as given); returns true when the menu was asked for */
	bool game_frame(uint32_t phys = 0, uint32_t phys2 = 0, bool tap = false)
	{
		PadSample p[MAX_PLAYERS];
		pads_none(p);
		p[0].buttons = phys;
		p[1].buttons = phys2;
		pf.clock_us += 16683;
		if (pl->input(p, tap, pf.clock_us / 1000, frame)) return true;
		snes_host_set_render(pl->want_render(frame) ? 1 : 0);
		snes_host_run_frame();
		int16_t ab[4096 * 2];
		while (snes_host_audio_take(ab, 4096) > 0) {}
		frame++;
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
	EXPECT(read_bytes(root + "/states/snes/" + game + ".st0") == occupant);
	EXPECT(read_bytes(root + "/states/snes/" + game + ".st1") == real_state);
	EXPECT(read_bytes(root + "/states/snes/" + game + ".st2") == real_state);
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
	std::string s4 = root + "/states/snes/" + game + ".st3";
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
		/* Scanlines: Right once (4 -> 5) */
		int row = -1;
		for (int i = 0; i < pl->menu().row_count(); i++) if (pl->menu().row_label(i) == "Scanlines") row = i;
		EXPECT(row >= 0);
		while (pl->menu().cursor() != row) rig.press(PH_DOWN);
		rig.press(PH_RIGHT);
		EXPECT(pl->picture().scan == 5);
		/* Screen size: Zoom -> Full screen -> Pixel perfect -> Original (no Fit) */
		while (pl->menu().cursor() != 0) rig.press(PH_UP);
		std::vector<std::string> seen;
		for (int k = 0; k < 4; k++) { rig.press(PH_RIGHT); seen.push_back(pl->menu().row_value_text(0)); }
		EXPECT(seen.size() == 4 && seen[0] == "Full screen" && seen[1] == "Pixel perfect" && seen[2] == "Original" && seen[3] == "Zoom");
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
	rig.press(PH_TRIANGLE);
	EXPECT(pl->menu().modal_open());
	rig.snap("cheats_delete_ask");
	rig.press(PH_CIRCLE);
	EXPECT(snes_host_cheat_count() == 0);
	rig.settle(3);
	rig.snap("cheats_deleted");

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
	EXPECT(exists(root + "/states/snes/" + game + ".rsm"));
	{ std::vector<uint8_t> pt = read_bytes(root + "/states/snes/" + game + ".rsm.pt"); EXPECT(std::string(pt.begin(), pt.end()) == "fix-1"); }
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
		EXPECT(read_bytes(root + "/states/snes/" + game + ".st0") != occupant);
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
		EXPECT(exists(root + "/states/snes/" + game + ".st5"));
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
		file_read(root + "/states/snes/" + game + ".rsm", rsm);
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
		/* Auto while the steady pacer thins frames: off (the pacer's own state is driven by the platform's costs) */
		for (int i = 0; i < 300 && pl->pace_mode() == PACE_FULL; i++) { pl->drawn_cost(60000); frames(rig, 1); }
		if (pl->pace_mode() == PACE_HALF) { frames(rig, 2); EXPECT(!pl->wide_active()); pl->on_change(ROW_PIC_WIDE, 1); frames(rig, 2); EXPECT(pl->wide_active()); pl->on_change(ROW_PIC_WIDE, 0); }
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

int run_handoff_test(const char *rom, const std::string &work);   /* handoff_proof.cpp */

int main(int argc, char **argv)
{
	if (argc >= 4 && !strcmp(argv[1], "handoff")) return run_handoff_test(argv[2], argv[3]);
	if (argc >= 4 && !strcmp(argv[1], "wide")) { int r = run_wide_test(argv[2], argv[3]); fprintf(stderr, "player_twin: wide %d checks, %d failed\n", g_checks, g_fail); return r || g_fail ? 1 : 0; }
	if (argc >= 5 && !strcmp(argv[1], "test")) return run_test(argv[2], argv[3], argv[4]);
	if (argc >= 5 && !strcmp(argv[1], "bench-saves")) return bench(argv[2], atoi(argv[3]), atoi(argv[4]));
	fprintf(stderr, "usage: player_twin test ROM SHOTS WORK | bench-saves DIR N FILL\n");
	return 2;
}
