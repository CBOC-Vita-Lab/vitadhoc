/*
 * player_twin (Sega): runs the Sega player's own code (md/vita/player.cpp: launch file, playerkit menu, states, battery RAM writer
 * with its verified backup, cheats, patches, session report, steady pacing, the look governor, pad models) on Linux with playerkit's
 * software renderer, over a running game, and checks it. Writes a PNG of every menu screen.
 *
 *   player_twin test CONSOLE ROM SHOTS WORK   the scripted walk and checks for one console id (md, sms, gg, sg); exit 1 on a failure
 *   player_twin pal ROM                        region / PAL pacing (FramePace over the core's own frame rate)
 *   player_twin look CONSOLE ROM WORK          the look governor (scaler tiers) steps, memory and hand-set rows
 *   player_twin pads CONSOLE ROM WORK          pad models, mapping, the menu chord, the six button pad
 *   player_twin handoff ROM WORK               smooth hand-over on / off, bit for bit (handoff_proof.cpp)
 *   player_twin speed CONSOLE ROM WORK         patch changes the ROM CRC, rewind 5 s (the machine is bit for bit where it was), fast forward 2x / 3x
 *   player_twin ra CONSOLE ROM WORK            RetroAchievements against a fake server on the loopback: stored token, unlock toast, Hardcore
 *
 * ROMs are public homebrew (Stephane-D's SGDK samples, SMS homebrew): inputs only, none is shipped. A Game Gear run uses an SMS ROM
 * with the console id forced to gg in the launch file (the core then runs it as a Game Gear game).
 * Copyright CBOC-Vita. MIT licence (see md/frontend/LICENSE).
 */
#include "player.h"
#include "pk_gfx_soft.h"
#include "pk_icons.h"
#include "host.h"
#include "pal_pace.h"
#include "picture.h"
#include "fake_ra.h"
#include "pk_ra.h"
#include "pk_rewind.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
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

struct TwinPlatform : mdp::Platform {
	uint64_t clock_us = 1000000;
	bool threaded = false;
	mdp::MdPlayer *player = 0;
	std::vector<uint16_t> frame_px;
	int fw = 320, fh = 224;
	std::vector<std::string> ime_answers;      /* code, name, code, name, ... */
	bool ime_asked = false;
	std::string log_path, log_text;
	int pic_changes = 0;
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
		const uint16_t *v = md_host_video(&w, &h, &pitch);
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
	mdp::MdPlayer *pl;
	SoftGfx gfx;
	Text text;
	Texture *game_tex;
	std::string shots;
	uint32_t frame = 0;
	int nshot;
	int tex_w = 0, tex_h = 0;
	unsigned drawn_frames = 0, ran_frames = 0, refreshes = 0, audio_in = 0, audio_out = 0, ahead_frames = 0;
	uint32_t emu_cost_us = 0;       /* what the test says a plain frame costs the emulating thread (0: Quick response's Auto is not fed) */
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
			/* as the Vita loop does: the emulating thread's time of a normal frame (here a figure the test sets) */
			if (n == 1 && render && emu_cost_us) pl->ahead_cost(pl->ahead_ran() ? emu_cost_us * 22 / 10 + 500 : emu_cost_us);
			int16_t ab[4096 * 2];
			int k;
			while ((k = md_host_audio_take(ab, 4096)) > 0) { audio_in += (unsigned) k; audio_out += (unsigned) pl->audio_filter(ab, (size_t) k); }
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
			if (pf.ime_answers.size() >= 2) {
				std::string c = pf.ime_answers[0], n = pf.ime_answers[1];
				pf.ime_answers.erase(pf.ime_answers.begin(), pf.ime_answers.begin() + 2);
				pl->cheat_entered(true, c, n);
			} else pl->cheat_entered(false, "", "");
		}
		return ev;
	}
	MenuEvent press(uint32_t phys)
	{
		MenuEvent a = menu_frame(mi(phys));
		MenuEvent b = menu_frame(mi(0));
		return a != MENU_NONE ? a : b;
	}
	void settle(int n = 12) { for (int i = 0; i < n; i++) menu_frame(mi(0)); }
	/* the game as the player shows it: the picture's own shape from md_host_video_info through pic_geometry (and the Game Gear's LCD colours) */
	void draw()
	{
		gfx.clear(PK_RGBA(0, 0, 0, 255));
		if (!pf.frame_px.empty()) {
			if (game_tex && (tex_w != pf.fw || tex_h != pf.fh)) { gfx.tex_free(game_tex); game_tex = 0; }
			if (!game_tex) { game_tex = gfx.tex_create(pf.fw, pf.fh, TEX_RGB565); tex_w = pf.fw; tex_h = pf.fh; }
			md_video_info vi; md_host_video_info(&vi);
			pic_info pi;
			pi.w = pf.fw; pi.h = pf.fh; pi.logical_rows = vi.native_h > 0 ? vi.native_h : pf.fh; pi.dar = vi.dar; pi.par = vi.par; pi.system = vi.system;
			pic_rect r;
			pic_geometry(&pl->picture(), &pi, 0, 0, 960, 544, &r);
			std::vector<uint16_t> px = pf.frame_px;
			if (vi.system == MD_SYS_GG && pl->picture().gg_colors) {
				const unsigned short *t = pic_lcd_table();
				if (t) for (size_t i = 0; i < px.size(); i++) px[i] = t[px[i]];
			}
			gfx.tex_update(game_tex, &px[0], pf.fw * 2);
			gfx.tex_draw(game_tex, (float) r.sx, (float) r.sy, (float) r.sw, (float) r.sh, r.dx, r.dy, r.dw, r.dh, 0xFFFFFFFFu, pl->picture().quality == PIC_SMOOTH);
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
	int row_of(const char *label)
	{
		for (int i = 0; i < pl->menu().row_count(); i++) if (pl->menu().row_label(i) == label) return i;
		return -1;
	}
	void go_row(const char *label)
	{
		int row = row_of(label);
		EXPECT(row >= 0);
		for (int k = 0; k < 40 && row >= 0 && pl->menu().cursor() != row; k++) press(pl->menu().cursor() < row ? PH_DOWN : PH_UP);
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
static std::string bucket(const std::string &root, const std::string &con, const std::string &game)
{
	StateLayout l; l.root = root; l.console = con; l.name = game;
	return l.states_dir() + "/" + game;
}
static bool exists(const std::string &p) { struct stat st; return stat(p.c_str(), &st) == 0; }
static int count_files(const std::string &dir, const char *needle)
{
	int n = 0;
	std::vector<std::string> names;
	if (!list_dir(dir, names)) return 0;
	for (size_t i = 0; i < names.size(); i++) if (names[i].find(needle) != std::string::npos) n++;
	return n;
}

static FsOps g_real;
static std::string g_fail_dir_part;
static bool fail_state_tmp(const std::string &p, const uint8_t *d, size_t n)
{
	if (p.find(g_fail_dir_part) != std::string::npos && ends_with_ci(p, ".tmp")) return false;
	return g_real.write_all(p, d, n);
}

static unsigned sys_of(const std::string &con) { return con == "gg" ? MD_SYS_GG : con == "sms" ? MD_SYS_SMS : con == "sg" ? MD_SYS_SG : MD_SYS_MD; }

/* load a game the way the player would, without it: a cartridge from memory, a Sega CD disc by its path with the BIOS found beside it */
static int load_any(const std::string &con, const char *rom, char *err, size_t errlen)
{
	if (con == "scd") {
		const std::string p = rom;
		std::vector<uint8_t> b = read_bytes(p.substr(0, p.find_last_of('/') + 1) + "standin.bin");
		md_host_cd_bios_test_force(1);
		md_host_cd_bios_clear();
		md_host_cd_bios_add(b.data(), b.size(), 1);
		return md_host_load_disc(rom, err, errlen);
	}
	std::vector<uint8_t> rb = read_bytes(rom);
	return md_host_load_mem(rb.data(), rb.size(), rom, sys_of(con), err, errlen);
}

/* a patch: one byte of the header's checksum field (never read by the cores): MD 0x18E, the 8 bit consoles 0x7FFA (the Z80 sees it at 0x7FFA) */
static unsigned patch_offset(const std::string &con) { return con == "md" ? 0x18Eu : 0x7FFAu; }

static int run_test(const std::string &con, const char *rom, const std::string &shots, const std::string &work)
{
	const bool md = con == "md" || con == "scd";
	std::string root = work + "/root";
	make_dirs(root);
	make_dirs(shots);
	const std::string game = path_stem(rom);
	fprintf(stderr, "player_twin test: console %s, ROM %s\n", con.c_str(), rom);

	/* ---- a real state of this game, made by the core, for the one-slot checks ---- */
	std::vector<uint8_t> real_state;
	{
		EXPECT(md_host_init() == 0);
		char e[128];
		EXPECT(load_any(con, rom, e, sizeof e) == 0);
		for (int i = 0; i < 30; i++) md_host_run_frame();
		real_state.resize(md_host_state_size());
		size_t len = md_host_state_save_mem(real_state.data(), real_state.size());
		EXPECT(len > 0);
		real_state.resize(len);
		std::vector<uint8_t> junk(100, 0x5A);
		EXPECT(md_host_state_load_mem(junk.data(), junk.size()) != 0);
		EXPECT(md_host_state_load_mem(real_state.data(), real_state.size()) == 0);
		md_host_unload();
	}

	/* a patch */
	std::vector<uint8_t> ips;
	const char *hdr = "PATCH";
	ips.insert(ips.end(), hdr, hdr + 5);
	const unsigned po = patch_offset(con);
	uint8_t rec[] = { (uint8_t) (po >> 16), (uint8_t) (po >> 8), (uint8_t) po, 0x00, 0x01, 0x5A };
	ips.insert(ips.end(), rec, rec + sizeof rec);
	ips.insert(ips.end(), (const uint8_t *) "EOF", (const uint8_t *) "EOF" + 3);
	EXPECT(write_bytes(work + "/fix.ips", ips));

	/* an older save, and a newer RetroArch save to bring in (RetroArch's .srm is the raw battery RAM) */
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

	/* a cheat database export for this game (the app's file): one entry the player merges in, switched off */
	make_dirs(work + "/db");
	std::string dbcht = work + "/db/" + game + ".cht";
	if (md) EXPECT(write_text(dbcht, "cheats = 1\n\ncheat0_desc = \"Database entry\"\ncheat0_code = \"FFF010:0042\"\ncheat0_enable = false\n"));
	else EXPECT(write_text(dbcht, "cheats = 1\n\ncheat0_desc = \"Database entry\"\ncheat0_code = \"C010:42\"\ncheat0_enable = false\n"));

	/* the launch file the main app will write */
	make_dirs(root + "/launch");
	std::string ini =
	    "version=1\nsession_id=s-test-1\nconsole=" + con + "\ngame=" + std::string(rom) + "\ngame_name=Sega Test (World)\nsave=" + srm +
	    "\nroot=" + root + "\nshape=zoom\nquality=smooth\ncpu_mhz=444\nenter_button=circle\nlanguage=en\nstart=new\nslot=0\npatch=" +
	    work + "/fix.ips\npatch_key=fix-1\nretroarch_save=" + ra + "\nwide_supported=0\novr.scanlines=4\nfuture_key=kept\ncheats_db=" + dbcht + "\n";
	EXPECT(write_text(root + "/launch/" + con + ".ini", ini));

	Rig rig(shots);
	rig.pf.log_path = root + "/" + con + "/log.txt";
	mdp::MdPlayer *pl = new mdp::MdPlayer(&rig.pf);
	rig.pl = pl;
	rig.pf.player = pl;
	char a0[] = "app0:md.bin";
	std::string romarg = rom;
	char *argv[] = { a0, &romarg[0], 0 };
	mdp::StartResult sr = pl->start(2, argv, root, true);
	EXPECT(sr.ok);
	if (!sr.ok) { fprintf(stderr, "start: %s / %s\n", sr.fatal1.c_str(), sr.fatal2.c_str()); return 1; }
	EXPECT(sr.console == con);
	EXPECT(md_host_system() == (int) sys_of(con));
	EXPECT(pl->have_launch_file());
	EXPECT(sr.cpu_mhz == 444);
	EXPECT(!sr.enter_is_cross);
	EXPECT(pl->picture().scan == 4);
	EXPECT(pl->picture().shape == PIC_ZOOM);
	EXPECT(pl->picture().quality == PIC_SMOOTH);
	EXPECT(pl->request().session_id == "s-test-1");
	EXPECT(pl->request().unknown.count("future_key") == 1);
	EXPECT(pl->request().unknown.count("cheats_db") == 1);
	/* the database cheat is in the list, off */
	EXPECT(pl->cheats().size() == 1);
	EXPECT(pl->cheats().size() == 1 && !pl->cheats().at(0).on && pl->cheats().at(0).usable);
	/* RetroArch save brought in, the old one kept in the backups */
	if (con == "scd") {      /* a Sega CD save is the backup RAM and the RAM cartridge in one file: the player does not import a RetroArch save over it */
		EXPECT(read_bytes(srm) == old_srm);
		EXPECT(read_bytes(root + "/backups/" + con + "/" + game + ".before-retroarch.srm").empty());
	} else {
		EXPECT(read_bytes(srm) == ra_srm);
		EXPECT(read_bytes(root + "/backups/" + con + "/" + game + ".before-retroarch.srm") == old_srm);
	}
	pl->apply_start_mode();

	/* ---- the patch reached the game ---- */
	rig.run(30);
	if (con != "scd") EXPECT(md_host_peek(po) == 0x5A);        /* (a disc is not patched) */
	rig.snap("ingame");

	/* ---- the menu: START + SELECT held, and a tap on the picture ---- */
	EXPECT(rig.open_by_chord());
	rig.settle(14);
	rig.snap("menu_top");
	EXPECT(pl->menu().page() == Menu::PAGE_TOP);
	/* the Play-together card is not shown (nothing plays together yet), the wide view row is greyed */
	EXPECT(rig.press(PH_CROSS) == MENU_RESUME);         /* Enter is Circle here: Cross is Back = resume */
	rig.run(20);
	EXPECT(rig.game_frame(0, 0, true));                  /* a tap on the picture */
	pl->open_menu(0);
	rig.settle(14);

	/* ---- battery RAM: the game's save changes, the player writes it after the menu is drawn, with a verified backup of what it replaces ---- */
	{
		std::vector<uint8_t> sv(md_host_sram_capacity() ? 4096 : 0, 0xA5);
		if (!sv.empty()) {
			md_host_sram_write(sv.data(), sv.size());
			md_host_sram_mark_unsaved();
			EXPECT(md_host_sram_dirty());
			rig.settle(3);
			pl->after_frame(rig.pf.clock_us += 16683);
			pl->menu_drawn();
			rig.run(1);
			pl->wait_writer();
			for (int i = 0; i < 12; i++) { rig.pf.clock_us += 1000000; rig.game_frame(); pl->wait_writer(); }
			std::vector<uint8_t> now = read_bytes(srm);
			EXPECT(!now.empty() && now != ra_srm);
			EXPECT(!md_host_sram_dirty());
			EXPECT(pl->session().current().save_changed);
			/* RetroArch compatible: the file is the raw battery RAM, the same bytes the core gives */
			std::vector<uint8_t> core(con == "scd" ? 0x400000 : 0x10000);      /* a disc: backup RAM + the cartridge, and the stash of other sizes */
			size_t k = md_host_sram_read(core.data(), core.size());
			core.resize(k);
			EXPECT(now == core);
			/* a verified backup of the replaced file (<name>.<crc>.<size>.bak in backups/<console>/) */
			EXPECT(count_files(root + "/backups/" + con, ".bak") >= 1);
		} else {
			fprintf(stderr, "player_twin: %s has no battery RAM: the save checks are skipped\n", rom);
		}
	}

	/* ---- Save states: all 10 slots, with thumbnails ---- */
	rig.press(PH_RIGHT);
	rig.snap("menu_top_states_card");
	rig.press(PH_CIRCLE);
	EXPECT(pl->menu().page() == Menu::PAGE_STATES);
	rig.settle(4);
	rig.snap("states_empty");
	std::string b = bucket(root, con, game);
	std::vector<std::vector<uint8_t>> saved(10);
	for (int slot = 0; slot < 10; slot++) {
		if (!pl->menu_open()) { pl->open_menu(0); rig.settle(3); }
		pl->menu().goto_page(Menu::PAGE_STATES);
		rig.settle(2);
		pl->menu().select_slot(slot + 1);
		EXPECT(pl->menu().selected_slot() == slot + 1);
		rig.press(PH_CIRCLE);                              /* Enter on an empty slot saves */
		rig.settle(4);
		std::string sp = b + ".st" + std::to_string(slot);
		EXPECT(exists(sp));
		EXPECT(exists(sp + ".thumb"));
		EXPECT(exists(sp + ".shot"));
		EXPECT(exists(sp + ".meta"));
		saved[slot] = read_bytes(sp);
		EXPECT(saved[slot].size() == md_host_state_size());
		EXPECT(pl->menu().notices().kind() == NOTICE_OK);
		/* leave the menu, play a while, come back */
		pl->menu().goto_page(Menu::PAGE_TOP);
		rig.settle(1);
		EXPECT(rig.press(PH_CROSS) == MENU_RESUME);
		rig.run(5);
	}
	EXPECT(pl->states()->used_count() == 10);
	for (int i = 1; i < 10; i++) EXPECT(saved[i] != saved[i - 1]);
	pl->open_menu(0);
	rig.settle(4);
	pl->menu().goto_page(Menu::PAGE_STATES);
	rig.settle(4);
	rig.snap("states_ten_slots");
	pl->menu().select_slot(5);
	rig.settle(2);
	rig.snap("states_slot5_thumb");
	/* forced write failure: overwrite slot 5 with the card refusing the temp file */
	g_fail_dir_part = "/states/" + con + "/";
	g_real = fs();
	fs().write_all = fail_state_tmp;
	rig.press(PH_SQUARE);                                  /* save over a filled slot: asks first */
	EXPECT(pl->menu().modal_open());
	rig.snap("states_overwrite_ask");
	rig.press(PH_CIRCLE);
	fs() = g_real;
	rig.settle(8);
	EXPECT(pl->menu().notices().kind() == NOTICE_ERROR);
	EXPECT(read_bytes(b + ".st4") == saved[4]);            /* the slot is untouched */
	rig.snap("states_write_failed");
	/* load slot 8 after the game moved on, then undo */
	pl->menu().select_slot(8);
	std::vector<uint8_t> moved;
	pl->capture_state(moved);
	EXPECT(moved != saved[7]);
	EXPECT(rig.press(PH_CIRCLE) == MENU_RESUME);           /* Enter on a filled slot loads and resumes */
	std::vector<uint8_t> after_load;
	pl->capture_state(after_load);
	EXPECT(after_load == saved[7]);
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
		/* one Image quality row, as in the main app (the old Pixel art scaler and Image look rows are folded into it) */
		EXPECT(rig.row_of("Image quality") >= 0);
		EXPECT(rig.row_of("Video filter") >= 0);
		EXPECT(rig.row_of("Pixel art scaler") < 0 && rig.row_of("Image look") < 0);
		EXPECT(rig.row_of("Screen size") >= 0);
		EXPECT((rig.row_of("Hide masked edge") >= 0) == (con == "sms"));
		EXPECT((rig.row_of("LCD colours") >= 0) == (con == "gg"));
		EXPECT((rig.row_of("LCD ghosting") >= 0) == (con == "gg"));
		/* the wide view: the row is there (Auto / On / Off, the same row as the SNES player's), on by default, Off gives the 4:3 picture back */
		EXPECT(rig.row_of("Wide view") >= 0);
		EXPECT(pl->menu().value(ROW_PIC_WIDE) == 0);
		pl->wide_apply(960, 544);
		EXPECT(pl->wide_active());
		EXPECT(pl->wide_margin() > 0 && pl->wide_margin() <= (con == "gg" ? 48 : 96));
		fprintf(stderr, "wide: %s margin %d on 960x544 (Original shape)\n", con.c_str(), pl->wide_margin());
		rig.go_row("Wide view");
		rig.press(PH_RIGHT);
		EXPECT(pl->picture().wide == 1 && pl->wide_mode() == 1);
		rig.press(PH_RIGHT);
		EXPECT(pl->picture().wide == 2);
		pl->wide_apply(960, 544);
		EXPECT(!pl->wide_active() && pl->wide_margin() == 0);
		rig.press(PH_RIGHT);                                  /* wraps to Auto */
		EXPECT(pl->picture().wide == 0);
		pl->wide_apply(960, 544);
		EXPECT(pl->wide_active());
		rig.go_row("Scanlines");
		rig.press(PH_RIGHT);
		EXPECT(pl->picture().scan == 5);
		/* Image quality steps through Auto, Original, Enhanced 2x, Enhanced 3x, MMPX 2x (the main app's names; no 4x scaler here) */
		rig.go_row("Image quality");
		EXPECT(pl->menu().value(ROW_PIC_QUALITY) == IMGQ_AUTO);
		EXPECT(pl->menu().row_value_text(pl->menu().cursor()) == "Auto");
		rig.press(PH_RIGHT);
		EXPECT(pl->picture().scaler == SCALER_OFF && pl->menu().value(ROW_PIC_QUALITY) == IMGQ_ORIGINAL);
		rig.press(PH_RIGHT);
		EXPECT(pl->picture().scaler == SCALER_XBR2 && pl->menu().row_value_text(pl->menu().cursor()) == "Enhanced 2x");
		rig.press(PH_RIGHT);
		EXPECT(pl->picture().scaler == SCALER_XBR3 && pl->menu().row_value_text(pl->menu().cursor()) == "Enhanced 3x");
		rig.snap("picture_scaler_xbr3");
		rig.press(PH_RIGHT);
		EXPECT(pl->picture().scaler == SCALER_MMPX2 && pl->menu().row_value_text(pl->menu().cursor()) == "MMPX 2x");
		/* Video filter: bilinear while Original switches the quality to Auto (as in the app); under Original it shows sharp bilinear */
		rig.go_row("Image quality");
		while (pl->picture().scaler != SCALER_OFF) rig.press(PH_LEFT);
		rig.go_row("Video filter");
		EXPECT(pl->menu().row_value_text(pl->menu().cursor()) == "Sharp bilinear");
		rig.press(PH_RIGHT);
		EXPECT(pl->picture().quality == PIC_SMOOTH && pl->picture().scaler == SCALER_AUTO && pl->menu().value(ROW_PIC_QUALITY) == IMGQ_AUTO);
		rig.press(PH_LEFT);
		EXPECT(pl->picture().quality == PIC_SHARP);
		rig.go_row("Image quality");
		while (pl->picture().scaler != SCALER_AUTO) rig.press(PH_LEFT);
		/* Screen size never offers Fit */
		while (pl->menu().cursor() != 0) rig.press(PH_UP);
		std::vector<std::string> seen;
		for (int k = 0; k < 4; k++) { rig.press(PH_RIGHT); seen.push_back(pl->menu().row_value_text(0)); }
		for (size_t k = 0; k < seen.size(); k++) EXPECT(seen[k] != "Fit");
		bool had_pixel = false;
		for (size_t k = 0; k < seen.size(); k++) if (seen[k] == "Pixel perfect") had_pixel = true;
		EXPECT(had_pixel);
	}
	rig.snap("picture_changed");
	pl->menu().goto_page(Menu::PAGE_SPEED);
	rig.settle(2);
	EXPECT(rig.row_of("Region") >= 0);
	EXPECT(rig.row_of("Steady pacing") >= 0);
	EXPECT(rig.row_of("PAL games speed") >= 0);
	EXPECT(rig.row_of("Smooth hand-over") >= 0);
	EXPECT((rig.row_of("FM sound") >= 0) == md);
	EXPECT((rig.row_of("FM sound unit") >= 0) == (con == "sms"));
	rig.snap("speed");
	/* Region: Japan (NTSC) -> the frame rate stays 60 Hz, Europe -> 50 Hz and the platform is told to set its pacing again */
	rig.go_row("Region");
	{
		EXPECT(pl->menu().value(mdp::MdPlayer::ROW_REGION) == 0);
		pl->on_change(mdp::MdPlayer::ROW_REGION, 2);                       /* Europe */
		EXPECT(md_host_is_pal() == 1);
		EXPECT(pl->take_timing_changed());
		EXPECT(!pl->take_timing_changed());
		EXPECT(md_host_fps() > 49.6 && md_host_fps() < 49.8);    /* the Mega Drive's PAL rate is 49.70 Hz, the 8 bit ones' 49.70 too */
		pl->on_change(mdp::MdPlayer::ROW_REGION, 1);                       /* USA */
		EXPECT(md_host_is_pal() == 0);
		EXPECT(pl->take_timing_changed());
		pl->on_change(mdp::MdPlayer::ROW_REGION, 0);                       /* Auto */
		pl->take_timing_changed();
	}
	if (con == "gg") {
		pl->menu().goto_page(Menu::PAGE_PICTURE);
		rig.settle(2);
		rig.snap("picture_gg_lcd");
	}

	/* ---- Controls and remapping ---- */
	pl->menu().goto_page(Menu::PAGE_CONTROLS);
	rig.settle(2);
	rig.snap("controls");
	EXPECT((rig.row_of("Pad type") >= 0) == md);
	pl->menu().goto_page(Menu::PAGE_MAPPING);
	rig.settle(2);
	rig.snap("mapping");
	{
		const char *btn = md ? "A" : "Button 1";
		rig.go_row(btn);
		rig.press(PH_CIRCLE);
		EXPECT(pl->menu().modal_open());
		rig.settle(2);
		rig.snap("mapping_capture");
		rig.press(PH_SQUARE);
		rig.settle(3);
		rig.snap("mapping_assigned");
	}
	EXPECT(exists(root + "/controls/" + con + ".ini"));
	while (pl->menu().page() != Menu::PAGE_TOP) rig.press(PH_CROSS);
	EXPECT(rig.press(PH_CROSS) == MENU_RESUME);
	rig.run(3);
	rig.game_frame(PH_SQUARE, PH_CROSS);
	EXPECT(pl->last_input().pad[0] & (md ? MD_A : MD_B));        /* the button moved to Square */
	rig.game_frame(PH_CIRCLE);
	EXPECT(!(pl->last_input().pad[0] & (md ? MD_A : MD_B)));

	/* ---- Cheats: a typed code, toggle, delete, and the database entry ---- */
	pl->open_menu(0);
	rig.settle(6);
	pl->menu().goto_page(Menu::PAGE_CHEATS);
	rig.settle(2);
	rig.snap("cheats_database_entry");
	rig.press(PH_DOWN);                                    /* onto "Add a cheat" or the entry: find the add row */
	rig.pf.ime_answers.push_back("nonsense!");
	rig.pf.ime_answers.push_back("Bad");
	pl->add_cheat();
	rig.menu_frame(rig.mi(0));
	EXPECT(pl->menu().notices().kind() == NOTICE_ERROR);
	rig.settle(8);
	rig.snap("cheats_bad_code");
	const unsigned ram_addr = md ? 0xFFF000u : 0xC200u;
	rig.pf.ime_answers.push_back(md ? "FFF000:00AA" : "C200:AA");
	rig.pf.ime_answers.push_back("Colour");
	pl->add_cheat();
	rig.menu_frame(rig.mi(0));
	rig.settle(8);
	rig.snap("cheats_added");
	EXPECT(md_host_cheat_count() == 1);
	EXPECT(pl->cheats().size() == 2);
	EXPECT(rig.press(PH_CROSS) == MENU_NONE);
	EXPECT(rig.press(PH_CROSS) == MENU_RESUME);
	rig.run(3);
	EXPECT(md_host_peek(md ? ram_addr ^ 1u : ram_addr) == 0xAA);     /* Genesis: a 00VV patch lands in the other byte of the word, as Genesis Plus GX does */
	pl->wait_writer();
	rig.run(2);
	pl->wait_writer();
	EXPECT(exists(root + "/cheats/" + game + ".cht"));
	{
		std::vector<uint8_t> t = read_bytes(root + "/cheats/" + game + ".cht");
		std::string s(t.begin(), t.end());
		EXPECT(s.find("Colour") != std::string::npos && s.find("Database entry") != std::string::npos);
	}
	/* switch it off from the list */
	pl->set_cheat(1, false);
	rig.run(2);
	EXPECT(md_host_cheat_count() == 0);
	pl->set_cheat(1, true);
	EXPECT(md_host_cheat_count() == 1);
	/* the .cht written back keeps its form: read it again with a second list */
	{
		mdp::CheatList again;
		EXPECT(again.load(root + "/cheats/" + game + ".cht", md) == 2);
		EXPECT(again.size() == 2 && again.at(0).label == "Database entry");
	}
	EXPECT(pl->delete_cheat(1));
	EXPECT(md_host_cheat_count() == 0);
	pl->open_menu(0);
	rig.settle(4);
	pl->menu().goto_page(Menu::PAGE_CHEATS);
	rig.settle(2);
	rig.snap("cheats_deleted");

	/* ---- Reset and Game list need confirmation ---- */
	pl->menu().goto_page(Menu::PAGE_TOP);
	rig.settle(2);
	for (int k = 0; k < 8 && pl->menu().cursor() != 5; k++) rig.press(PH_RIGHT);
	rig.snap("top_reset_card");
	rig.press(PH_CIRCLE);
	EXPECT(pl->menu().modal_open());
	rig.snap("reset_ask");
	rig.press(PH_CROSS);
	rig.press(PH_RIGHT);
	rig.press(PH_CIRCLE);
	rig.snap("game_list_ask");
	MenuEvent ev = rig.press(PH_CIRCLE);
	EXPECT(ev == MENU_GAME_LIST);
	pl->finish(EXIT_GAME_LIST);

	/* ---- the session report ---- */
	SessionReport rep;
	EXPECT(session_read(root, rep));
	EXPECT(rep.valid && rep.session_id == "s-test-1");
	EXPECT(rep.seconds_played >= 2);
	EXPECT(!rep.save_error);
	EXPECT(rep.resume_present);
	EXPECT(rep.exit == EXIT_GAME_LIST);
	EXPECT(exists(b + ".rsm"));
	{ std::vector<uint8_t> pt = read_bytes(b + ".rsm.pt"); EXPECT(std::string(pt.begin(), pt.end()) == "fix-1"); }
	EXPECT(exists(root + "/scenes/" + game + ".shot"));
	{
		Ini p;
		EXPECT(p.load(root + "/" + con + "/picture.ini"));
		EXPECT(p.get_int("scanlines", -1) == 5);
		EXPECT(p.get_int("quality", -1) == 0);        /* the launch file's "smooth" was not written into the player's file */
	}
	std::string session_text;
	{ std::vector<uint8_t> bb = read_bytes(root + "/launch/session.txt"); session_text.assign(bb.begin(), bb.end()); }
	fprintf(stderr, "---- session.txt\n%s----\n", session_text.c_str());
	EXPECT(session_text.find("session_id=s-test-1") != std::string::npos);
	delete pl;
	md_host_unload();

	/* ---- second start: no launch file (today's argument), quick save/load and the threaded writer ---- */
	file_remove(root + "/launch/" + con + ".ini");
	{
		Rig r2(shots, 60);
		r2.pf.log_path = root + "/" + con + "/log.txt";
		mdp::MdPlayer *p2 = new mdp::MdPlayer(&r2.pf);
		r2.pl = p2; r2.pf.player = p2;
		std::string arg = std::string(rom) + "|" + srm;
		char *av[] = { a0, &arg[0], 0 };
		mdp::StartResult s2 = p2->start(2, av, root, true);
		EXPECT(s2.ok && !p2->have_launch_file());
		EXPECT(s2.enter_is_cross && s2.cpu_mhz == 0);
		EXPECT(s2.console == (con == "scd" ? "scd" : md ? "md" : "sms") || con == "gg" || con == "sg");     /* by extension: .sms is sms */
		EXPECT(p2->srm_path() == srm);
		if (s2.console == con) EXPECT(p2->picture().scan == 5);       /* a forced console id (gg on an .sms file) has no launch file here: the extension's console, its own settings */
		r2.pf.start_thread();
		p2->apply_start_mode();
		r2.run(40);
		EXPECT(md_host_peek(po) != 0x5A);                  /* no patch this time */
		r2.game_frame(PH_SELECT);
		r2.game_frame(PH_SELECT | PH_L);
		r2.run(2);
		p2->wait_writer();
		r2.run(1);
		EXPECT(p2->notices().text() == "State saved to slot 1");
		EXPECT(read_bytes(bucket(root, s2.console, game) + ".st0").size() == md_host_state_size());
		r2.snap("quicksave_notice");
		p2->finish(EXIT_NORMAL);
		r2.pf.stop_thread();
		SessionReport rp;
		EXPECT(session_read(root, rp));
		EXPECT(rp.session_id.empty() && rp.exit == EXIT_NORMAL);
		delete p2;
		md_host_unload();
	}
	const bool s_gg_forced = con == "gg" && !(strlen(rom) > 3 && !strcasecmp(rom + strlen(rom) - 3, ".gg"));   /* a real .gg file is a Game Gear by its own extension */
	/* ---- third start: Continue from the resume point (the patch key must match) ---- */
	{
		/* the second start had no launch file and so another console id for a forced gg game: continue where the walk left off */
		std::string ini3 = "version=1\nsession_id=s-test-3\nconsole=" + con + "\ngame=" + std::string(rom) + "\nsave=" + srm + "\nstart=continue\npatch_key=other\n";
		write_text(root + "/launch/" + con + ".ini", ini3);
		Rig r3(shots);
		r3.pf.log_path = root + "/" + con + "/log.txt";
		mdp::MdPlayer *p3 = new mdp::MdPlayer(&r3.pf);
		r3.pl = p3; r3.pf.player = p3;
		char *av[] = { a0, 0 };
		EXPECT(p3->start(1, av, root, true).ok);
		p3->apply_start_mode();
		EXPECT(p3->notices().text().find("another patch") != std::string::npos);
		delete p3;
		md_host_unload();
		ini3 = "version=1\nsession_id=s-test-4\nconsole=" + con + "\ngame=" + std::string(rom) + "\nsave=" + srm + "\nstart=continue\n" +
		    (s_gg_forced ? "patch=" + work + "/fix.ips\npatch_key=fix-1\n" : std::string());     /* the resume point of the forced run was made with the patch */
		write_text(root + "/launch/" + con + ".ini", ini3);
		Rig r4(shots);
		r4.pf.log_path = root + "/" + con + "/log.txt";
		mdp::MdPlayer *p4 = new mdp::MdPlayer(&r4.pf);
		r4.pl = p4; r4.pf.player = p4;
		EXPECT(p4->start(1, av, root, true).ok);
		std::vector<uint8_t> rsm;
		file_read(b + ".rsm", rsm);
		p4->apply_start_mode();
		std::vector<uint8_t> now;
		p4->capture_state(now);
		if (s_gg_forced) {
			/* the resume point was written by the extension's console (an SMS machine: the picture's left edge 0), this start is a Game Gear
			 * one that sets its own left edge (-48) after the load: only that field of the carry block (and the CRC after it) may differ */
			size_t nd = 0;
			EXPECT(now.size() == rsm.size());
			for (size_t q = 0; q < now.size() && q < rsm.size(); q++) if (now[q] != rsm[q]) nd++;
			EXPECT(nd <= 8);
		} else
			EXPECT(now == rsm);
		EXPECT(!rsm.empty());
		delete p4;
		md_host_unload();
	}
	/* ---- a patch that does not fit: the game is not started, the app is told ---- */
	if (con != "scd")        /* (a disc is not patched) */
	{
		std::vector<uint8_t> bad(ips.begin(), ips.begin() + 8);
		write_bytes(work + "/bad.ips", bad);
		std::string ini5 = "version=1\nsession_id=s-test-5\nconsole=" + con + "\ngame=" + std::string(rom) + "\npatch=" + work + "/bad.ips\n";
		write_text(root + "/launch/" + con + ".ini", ini5);
		Rig r5(shots);
		mdp::MdPlayer *p5 = new mdp::MdPlayer(&r5.pf);
		r5.pl = p5; r5.pf.player = p5;
		char *av[] = { a0, 0 };
		mdp::StartResult s5 = p5->start(1, av, root, true);
		EXPECT(!s5.ok);
		fprintf(stderr, "bad patch: %s / %s\n", s5.fatal1.c_str(), s5.fatal2.c_str());
		SessionReport rp;
		EXPECT(session_read(root, rp) && rp.session_id == "s-test-5" && rp.exit == EXIT_ERROR && !rp.notice.empty());
		delete p5;
	}
	fprintf(stderr, "player_twin: %s %d checks, %d failed; %d screenshots in %s\n", con.c_str(), g_checks, g_fail, rig.nshot, shots.c_str());
	return g_fail ? 1 : 0;
}

/* ---- region and PAL pacing: the Vita loop's frame accumulator (pal_pace.h) over 3000 display refreshes at 59.94 Hz ----------- */
static int run_pal_test(const char *rom)
{
	std::vector<uint8_t> rb = read_bytes(rom);
	for (int pass = 0; pass < 2; pass++) {
		char err[256] = "";
		if (md_host_init()) { fprintf(stderr, "pal: init failed\n"); return 1; }
		if (md_host_set_region(pass == 0 ? MD_REGION_EUROPE : MD_REGION_USA)) return 1;
		if (md_host_load_mem(rb.data(), rb.size(), rom, MD_SYS_AUTO, err, sizeof err)) { fprintf(stderr, "pal: load failed: %s\n", err); return 1; }
		const bool pal = md_host_is_pal() != 0;
		EXPECT(pal == (pass == 0));
		const double console_hz = md_host_fps(), vita_hz = 59.94;
		FramePace fp;
		if (pal) fp.set(console_hz, vita_hz);
		const int refreshes = 3000;
		int frames = 0, max_gap = 0, gap = 0, sixth_ok = 1;
		for (int r = 0; r < refreshes; r++) {
			int n = pal ? fp.owed(1) : 1;
			if (n > 1) sixth_ok = 0;
			if (n == 0) { if (++gap > max_gap) max_gap = gap; } else gap = 0;
			for (int i = 0; i < n; i++) { md_host_set_render(1); md_host_run_frame(); frames++; }
		}
		const double emu_s = frames / console_hz, wall_s = refreshes / vita_hz;
		const double err_pct = 100.0 * (emu_s - wall_s) / wall_s;
		fprintf(stderr, "pal: %s (core %.4f Hz), %d refreshes, %d frames run, emulated %.3f s, refresh time %.3f s, error %+.3f %%, longest skip %d\n",
		    pal ? "PAL" : "NTSC", console_hz, refreshes, frames, emu_s, wall_s, err_pct, max_gap);
		EXPECT(sixth_ok);
		if (pal) {
			EXPECT(console_hz > 49.6 && console_hz < 50.1);
			EXPECT(err_pct > -0.5 && err_pct < 0.5);
			EXPECT(max_gap == 1);
			EXPECT(frames >= 2470 && frames <= 2510);
		} else {
			EXPECT(console_hz > 59.8 && console_hz < 60.2);   /* 59.92 Hz */
			EXPECT(frames == refreshes && max_gap == 0);
		}
		if (pal) {
			/* Smooth 50 Hz (pk_palsmooth.h): the same frames to within one at every refresh; a refresh that runs none follows an early frame */
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
		/* the sound of the frames run: the audio the core made is the frames' share at the console's rate, so the sound pacer needs no change */
		md_host_unload();
	}
	fprintf(stderr, "player_twin: pal %d checks, %d failed\n", g_checks, g_fail);
	return g_fail ? 1 : 0;
}

/* ---- the look governor: the ladder is the scaler's tiers ------------------------------------------------------------------- */
static int run_look_test(const std::string &con, const char *rom, const std::string &work)
{
	char a0[] = "app0:md.bin";
	std::string arg = std::string(rom);
	char *av[] = { a0, &arg[0], 0 };
	const std::string game = path_stem(rom);
	/* a forced console id needs its launch file */
	auto prep = [&](const std::string &root) {
		make_dirs(root + "/launch");
		write_text(root + "/launch/" + con + ".ini", "version=1\nsession_id=s-look\nconsole=" + con + "\ngame=" + std::string(rom) + "\n");
	};
	/* the scaler the drawing code uses: the picture's own ceiling plus the steps the governor has taken */
	auto eff = [&](mdp::MdPlayer *pl) {
		md_video_info v; md_host_video_info(&v);
		return pic_scaler_effective(&pl->picture(), pic_scaler_ceiling(v.system, v.native_w, v.native_h), pl->picture().tier_steps);
	};
	float bad_emu = 1.25f;
	auto run = [&](const std::string &root, int bad_s, int good_s, int hand_scaler, int mhz, std::string *seq) -> mdp::MdPlayer * {
		make_dirs(root);
		prep(root);
		Rig *rig = new Rig(root + "/shots");
		mdp::MdPlayer *pl = new mdp::MdPlayer(&rig->pf);
		rig->pl = pl; rig->pf.player = pl;
		rig->pf.log_path = root + "/" + con + "/log.txt";
		mdp::StartResult sr = pl->start(2, av, root, true);
		EXPECT(sr.ok);
		pl->apply_start_mode();
		if (hand_scaler >= 0) pl->on_change(mdp::MdPlayer::ROW_SCALER, hand_scaler);
		rig->run(4);
		pl->look_start(mhz);
		auto sec = [&](float emu) {
			pk::LookSecond ls;
			ls.emu = emu; ls.draw = 0.3f; ls.refreshes = 60; ls.parallel = true;
			pl->look_second(ls);
			for (int i = 0; i < 3; i++) rig->game_frame();
			if (seq) *seq += (char) ('0' + eff(pl)), *seq += ' ';
		};
		for (int i = 0; i < bad_s; i++) sec(bad_emu);
		for (int i = 0; i < good_s; i++) sec(0.35f);
		return pl;
	};
	md_video_info vi;
	int ceiling = -1;

	std::string a, b;
	{
		mdp::MdPlayer *pl = run(work + "/look1", 60, 0, -1, 444, &a);
		md_host_video_info(&vi);
		ceiling = pl->look_ceiling_tier();
		fprintf(stderr, "look: %s native %dx%d -> ceiling tier %d (0 xBR3, 1 xBR2, 2 sharp), end level %d, scaler now %d\n", con.c_str(), vi.native_w, vi.native_h, ceiling, pl->look().level(), eff(pl));
		EXPECT(pl->look().level() == 2 - ceiling);               /* every rung taken under a long overload */
		EXPECT(eff(pl) == SCALER_OFF);
		pl->finish(EXIT_NORMAL);
	}
	{
		mdp::MdPlayer *pl = run(work + "/look2", 60, 0, -1, 444, &b);
		pl->finish(EXIT_NORMAL);
	}
	fprintf(stderr, "look sequence: %s\n", a.c_str());
	EXPECT(a == b);                                           /* the same script, the same looks */
	/* starts at the ceiling's tier; the ladder only goes down, one tier per step */
	{
		static const char tier_digit[3] = { (char) ('0' + SCALER_XBR3), (char) ('0' + SCALER_XBR2), (char) ('0' + SCALER_OFF) };
		EXPECT(a.size() > 4 && a[a.size() - 2] == tier_digit[2]);          /* ends on the sharp fit */
		EXPECT(a.find(tier_digit[0]) != std::string::npos || a.find(tier_digit[1]) != std::string::npos);   /* and was on a scaler before */
		int prev = -1; bool mono = true;
		for (size_t i = 0; i < a.size(); i += 2) {
			int t = -1;
			for (int k = 0; k < 3; k++) if (a[i] == tier_digit[k]) t = k;
			if (prev >= 0 && t < prev) mono = false;
			prev = t;
		}
		EXPECT(mono);
	}
	/* a mild overload takes the rungs one at a time */
	{
		std::string c;
		bad_emu = 0.985f;
		mdp::MdPlayer *pl = run(work + "/look_mild", 60, 0, -1, 444, &c);
		bad_emu = 1.25f;
		fprintf(stderr, "look sequence (mild overload 0.985): %s\n", c.c_str());
		/* the governor takes as many rungs in one step as the learned costs say bring the load under its target (0.80): from 0.985 that is both of the
		 * Game Gear's, so the sequence goes xBR 3x straight to sharp; it never goes back up in a bad second */
		EXPECT(c.size() > 4 && c[c.size() - 2] == (char) ('0' + SCALER_OFF));
		if (ceiling == 0) EXPECT(c[0] == (char) ('0' + SCALER_XBR3));
		pl->finish(EXIT_NORMAL);
	}
	/* a scaler chosen by hand is held as chosen */
	{
		std::string c;
		mdp::MdPlayer *pl = run(work + "/look3", 60, 0, SCALER_XBR2, 444, &c);
		EXPECT(pl->picture().scaler == SCALER_XBR2);
		EXPECT(pl->look().level() == 0);
		pl->finish(EXIT_NORMAL);
	}
	/* memory: a cured overload is written; the next start at the same clock begins with it; another clock does not */
	{
		std::string d;
		mdp::MdPlayer *pl = run(work + "/look4", 30, 40, -1, 444, &d);
		fprintf(stderr, "look sequence (cured): %s\n", d.c_str());
		pl->finish(EXIT_NORMAL);
		std::vector<std::string> found;
		for (int x = 0; x < 256; x++) {
			char buf[16]; snprintf(buf, sizeof buf, "%02x", x);
			std::string f = work + "/look4/look/" + con + "/" + buf + "/" + game + ".look";
			if (!read_bytes(f).empty()) found.push_back(f);
		}
		EXPECT(found.size() == 1);
		if (found.size() == 1) {
			std::vector<uint8_t> m = read_bytes(found[0]);
			EXPECT(std::string((const char *) m.data(), 9) == "pk-look 1");
			std::string e;
			mdp::MdPlayer *p1 = run(work + "/look4", 0, 0, -1, 444, &e);
			EXPECT(p1->look().mask() != 0 || ceiling == 2);
			p1->finish(EXIT_NORMAL);
			mdp::MdPlayer *p2 = run(work + "/look4", 0, 0, -1, 333, &e);
			EXPECT(p2->look().mask() == 0);
			p2->finish(EXIT_NORMAL);
		}
	}
	/* the old look_mode key (the launch file and the session runner still set it): Full never eases, Light is the sharp fit at once (shown as Original); both kept for the next start */
	{
		const std::string root = work + "/look5";
		make_dirs(root); prep(root);
		{
			Rig *rig = new Rig(root + "/shots");
			mdp::MdPlayer *pl = new mdp::MdPlayer(&rig->pf);
			rig->pl = pl; rig->pf.player = pl;
			rig->pf.log_path = root + "/" + con + "/log.txt";
			EXPECT(pl->start(2, av, root, true).ok);
			pl->apply_start_mode();
			EXPECT(!pl->pal_fast() && pl->pal_smooth() && pl->look_mode() == 0);
			pl->look_start(444);
			pl->on_change(mdp::MdPlayer::ROW_LOOK, 1);
			for (int i = 0; i < 40; i++) { pk::LookSecond ls; ls.emu = 1.25f; ls.draw = 0.3f; ls.refreshes = 60; ls.parallel = true; pl->look_second(ls); }
			EXPECT(eff(pl) != SCALER_OFF || ceiling == 2);
			pl->on_change(mdp::MdPlayer::ROW_LOOK, 2);
			EXPECT(pl->picture().scaler == SCALER_OFF);
			pl->on_change(mdp::MdPlayer::ROW_PALSPEED, 2);                 /* the menu lists Smooth 50 Hz, True 50 Hz, Fast 60 Hz */
			pl->finish(EXIT_NORMAL);
		}
		{
			Rig *rig = new Rig(root + "/shots2");
			mdp::MdPlayer *pl = new mdp::MdPlayer(&rig->pf);
			rig->pl = pl; rig->pf.player = pl;
			rig->pf.log_path = root + "/" + con + "/log.txt";
			EXPECT(pl->start(2, av, root, true).ok);
			EXPECT(pl->pal_fast() && pl->look_mode() == 2);
			EXPECT(pl->picture().scaler == SCALER_OFF);
			EXPECT(pl->menu().value(ROW_PIC_QUALITY) == IMGQ_ORIGINAL && pl->menu().value(mdp::MdPlayer::ROW_PALSPEED) == 2);     /* the Light look shows as Original */
			pl->finish(EXIT_NORMAL);
		}
	}
	/* 5. an older file that says "no scaler" with the Smooth video filter drew bilinear: Original draws sharp (as in the main app), so the choice is kept
	 * the way the app keeps it (scaler to Auto); the old look_mode key 1 (Full) reads as Auto; Original chosen in the menu is stored sharp and stays */
	for (int variant = 0; variant < 3; variant++) {
		const std::string root = work + "/look6_" + std::to_string(variant);
		make_dirs(root);
		prep(root);
		pic_settings old; pic_defaults(&old);
		old.scaler = variant == 2 ? SCALER_AUTO : SCALER_OFF;
		old.quality = PIC_SMOOTH;
		make_dirs(root + "/" + con + "");
		EXPECT(pic_save((root + "/" + con + "/picture.ini").c_str(), &old) == 0);
		if (variant == 1) EXPECT(write_text(root + "/" + con + "/player.ini", "look_mode=2\n"));       /* the old Light look, global */
		if (variant == 2) EXPECT(write_text(root + "/" + con + "/player.ini", "look_mode=1\n"));       /* the old Full look */
		Rig *rig = new Rig(root + "/shots");
		mdp::MdPlayer *pl = new mdp::MdPlayer(&rig->pf);
		rig->pl = pl; rig->pf.player = pl;
		rig->pf.log_path = root + "/" + con + "/log.txt";
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
		mdp::MdPlayer *p2 = new mdp::MdPlayer(&rig2->pf);
		rig2->pl = p2; rig2->pf.player = p2;
		rig2->pf.log_path = root + "/" + con + "/log.txt";
		EXPECT(p2->start(2, av, root, true).ok);
		EXPECT(p2->menu().value(ROW_PIC_QUALITY) == IMGQ_ORIGINAL && p2->picture().scaler == SCALER_OFF);
		p2->finish(EXIT_NORMAL);
	}
	fprintf(stderr, "player_twin: look %s %d checks, %d failed\n", con.c_str(), g_checks, g_fail);
	return g_fail ? 1 : 0;
}

/* ---- the picture ceilings by native size (portable, no ROM needed) ---------------------------------------------------------- */
static int run_ceiling_test()
{
	EXPECT(pic_scaler_ceiling(MD_SYS_GG, 160, 144) == 0);
	EXPECT(pic_scaler_ceiling(MD_SYS_SMS, 256, 192) == 0);      /* xBR 3x since 2.0 (the wide view's picture is scaled by the ceiling of the console's own) */
	EXPECT(pic_scaler_ceiling(MD_SYS_SG, 256, 192) == 0);
	EXPECT(pic_scaler_ceiling(MD_SYS_GG, 256, 144) == 0);       /* the wide view's Game Gear band */
	EXPECT(pic_scaler_ceiling(MD_SYS_MD, 256, 224) == 1);
	EXPECT(pic_scaler_ceiling(MD_SYS_MD, 320, 224) == 1);      /* xBR 2x also on H40: 3x would be 672 rows for 544 */
	EXPECT(pic_scaler_ceiling(MD_SYS_MD, 320, 240) == 1);
	EXPECT(pic_scaler_ceiling(MD_SYS_MD, 256, 240) == 1);
	EXPECT(pic_scaler_ceiling(MD_SYS_SMS, 256, 224) == 1);
	EXPECT(pic_scaler_ceiling(MD_SYS_MD, 320, 448) == 2);      /* interlaced double height */
	EXPECT(pic_scaler_ceiling(MD_SYS_MD, 400, 224) == 2);
	pic_settings s; pic_defaults(&s);
	EXPECT(pic_scaler_effective(&s, 0, 0) == SCALER_XBR3);
	EXPECT(pic_scaler_effective(&s, 0, 1) == SCALER_XBR2);
	EXPECT(pic_scaler_effective(&s, 0, 2) == SCALER_OFF);
	EXPECT(pic_scaler_effective(&s, 0, 9) == SCALER_OFF);
	EXPECT(pic_scaler_effective(&s, 1, 0) == SCALER_XBR2);
	EXPECT(pic_scaler_effective(&s, 1, 1) == SCALER_OFF);
	EXPECT(pic_scaler_effective(&s, 2, 0) == SCALER_OFF);
	s.scaler = SCALER_MMPX2;
	EXPECT(pic_scaler_effective(&s, 1, 1) == SCALER_MMPX2);    /* MMPX is a look chosen by hand, never a rung */
	s.scaler = SCALER_AUTO;
	s.scaler = SCALER_XBR3;
	EXPECT(pic_scaler_effective(&s, 2, 1) == SCALER_XBR3);     /* by hand: held */
	/* the layout: Original keeps the display aspect, Pixel perfect integer, Full fills; Game Gear 160x144 */
	pic_defaults(&s);
	pic_info gg = { 160, 144, 144, 160.0 / 144.0, 1.0, MD_SYS_GG };
	pic_rect r;
	pic_geometry(&s, &gg, 0, 0, 960, 544, &r);
	EXPECT(r.dh == 544.0f && r.dw > 603.0f && r.dw < 605.0f);
	s.shape = PIC_PIXEL;
	pic_geometry(&s, &gg, 0, 0, 960, 544, &r);
	EXPECT(r.dh == 432.0f && r.dw == 480.0f);                  /* 3x */
	s.shape = PIC_FULL;
	pic_geometry(&s, &gg, 0, 0, 960, 544, &r);
	EXPECT(r.dw == 960.0f && r.dh == 544.0f);
	pic_info md = { 320, 224, 224, 4.0 / 3.0, 0.0, MD_SYS_MD };
	pic_defaults(&s);
	pic_geometry(&s, &md, 0, 0, 960, 544, &r);
	EXPECT(r.dh == 544.0f && r.dw > 724.0f && r.dw < 726.0f);  /* 4:3 */
	/* the wide view: the margin that fills 960 columns at the Original shape, the same scale per column, Full screen has none */
	{
		pic_defaults(&s);
		const int mmd = pic_wide_margin(&s, &md, 960, 544, 96);
		pic_info ggw = { 160, 144, 144, 1.2 * 160.0 / 144.0, 1.2, MD_SYS_GG };      /* what pic_info_from_video gives: 6:5 pixels, a 4:3 picture */
		const int mgg = pic_wide_margin(&s, &ggw, 960, 544, 48);
		pic_info sms = { 256, 192, 192, 0.0, 1.0, MD_SYS_SMS };
		const int msm = pic_wide_margin(&s, &sms, 960, 544, 96);
		fprintf(stderr, "wide margins at 960x544, Original: Mega Drive H40 %d, Game Gear %d, Master System %d\n", mmd, mgg, msm);
		EXPECT(mmd >= 48 && mmd <= 56 && mgg >= 20 && mgg <= 30 && msm >= 36 && msm <= 46);
		pic_geometry_wide(&s, &md, 320 + 2 * mmd, 0, 0, 960, 544, &r);
		EXPECT(r.sx == 0 && r.sw == 320 + 2 * mmd && r.dw > 955.0f && r.dw <= 960.5f && r.dh == 544.0f);
		const float per = r.dw / (float) r.sw;
		EXPECT(per > 2.26f && per < 2.28f);                          /* the normal picture's 725 / 320 columns */
		pic_geometry_wide(&s, &ggw, 256, 0, 0, 960, 544, &r);        /* the 256 wide band is wider than the screen shows: trimmed evenly */
		EXPECT(r.sx > 0 && r.sx * 2 + r.sw == 256 && r.dw <= 960.5f);
		s.shape = PIC_FULL;
		EXPECT(pic_wide_margin(&s, &md, 960, 544, 96) == 0);
		pic_defaults(&s);
		EXPECT(s.wide == 0);
	}
	/* the Game Gear LCD table: black lifts, white stays near white, grey stays grey */
	const unsigned short *t = pic_lcd_table();
	EXPECT(t && t[0] != 0 && t[0xFFFF] > 0xF000);
	EXPECT(t && ((t[0x8410] >> 11) & 31) > 8);
	fprintf(stderr, "player_twin: ceilings %d checks, %d failed\n", g_checks, g_fail);
	return g_fail ? 1 : 0;
}

/* ---- pads: models, the mapping, the menu chord and the Mode tap ---------------------------------------------------------------- */
static int run_pads_test(const std::string &con, const char *rom, const std::string &work)
{
	const bool md = con == "md" || con == "scd";
	const std::string root = work + "/root";
	make_dirs(root + "/launch");
	write_text(root + "/launch/" + con + ".ini", "version=1\nsession_id=s-pads\nconsole=" + con + "\ngame=" + std::string(rom) + "\nenter_button=cross\n");
	Rig rig(work + "/shots");
	rig.pf.log_path = root + "/" + con + "/log.txt";
	mdp::MdPlayer *pl = new mdp::MdPlayer(&rig.pf);
	rig.pl = pl; rig.pf.player = pl;
	char a0[] = "app0:md.bin";
	std::string arg = rom;
	char *av[] = { a0, &arg[0], 0 };
	EXPECT(pl->start(2, av, root, true).ok);
	pl->apply_start_mode();
	rig.run(5);
	auto bits = [&](uint32_t phys) -> unsigned { rig.run(8); rig.game_frame(phys); return pl->last_input().pad[0]; };
	/* Start and Select wait a few frames (they may be the first button of the menu chord), then pass while held */
	auto held = [&](uint32_t phys) -> unsigned { rig.run(8); unsigned seen = 0; for (int i = 0; i < 12; i++) { rig.game_frame(phys); seen |= pl->last_input().pad[0]; } return seen; };
	EXPECT(bits(PH_UP) == (unsigned) MD_UP);
	EXPECT(bits(PH_DOWN) == (unsigned) MD_DOWN);
	EXPECT(bits(PH_LEFT) == (unsigned) MD_LEFT);
	EXPECT(bits(PH_RIGHT) == (unsigned) MD_RIGHT);
	if (md) {
		/* A Square, B Cross, C Circle, X L, Y Triangle, Z R, Start */
		EXPECT(bits(PH_SQUARE) == (unsigned) MD_A);
		EXPECT(bits(PH_CROSS) == (unsigned) MD_B);
		EXPECT(bits(PH_CIRCLE) == (unsigned) MD_C);
		EXPECT(bits(PH_L) == (unsigned) MD_X);
		EXPECT(bits(PH_TRIANGLE) == (unsigned) MD_Y);
		EXPECT(bits(PH_R) == (unsigned) MD_Z);
		EXPECT(held(PH_START) == (unsigned) MD_START);
		/* a tap of Select is Mode: press and release, Mode is seen by the core; the pad word is 12 bits */
		unsigned seen = 0;
		for (int i = 0; i < 4; i++) { rig.game_frame(PH_SELECT); seen |= pl->last_input().pad[0]; }
		for (int i = 0; i < 6; i++) { rig.game_frame(0); seen |= pl->last_input().pad[0]; }
		EXPECT(seen & MD_MODE);
		/* the core takes the 6 button pad: a six button game sees X Y Z Mode; pad type 3 hides them */
		pl->on_change(mdp::MdPlayer::ROW_PADTYPE, 2);
		EXPECT(md_host_pad_type(0) == MD_PAD_6 && md_host_pad_type(1) == MD_PAD_6);
		pl->on_change(mdp::MdPlayer::ROW_PADTYPE, 1);
		EXPECT(md_host_pad_type(0) == MD_PAD_3);
		pl->on_change(mdp::MdPlayer::ROW_PADTYPE, 0);
	} else {
		/* two buttons: 1 = Cross (MD_B), 2 = Circle (MD_C), Start = Pause */
		EXPECT(bits(PH_CROSS) == (unsigned) MD_B);
		EXPECT(bits(PH_CIRCLE) == (unsigned) MD_C);
		EXPECT(held(PH_START) == (unsigned) MD_START);
		EXPECT(bits(PH_L) == 0u && bits(PH_R) == 0u);          /* no X Y Z on an 8 bit console */
		EXPECT(md_host_pad_type(0) == 0);
		/* Square is Button 1's turbo: pressed it gives MD_B on some frames */
		unsigned t = 0;
		for (int i = 0; i < 8; i++) t |= bits(PH_SQUARE);
		EXPECT(t & MD_B);
	}
	/* two diagonals do not hide each other, the pad of a second player */
	rig.game_frame(PH_RIGHT, PH_CROSS);
	EXPECT(pl->last_input().pad[0] == (unsigned) MD_RIGHT);
	EXPECT(con == "gg" ? true : (pl->last_input().pad[1] & (md ? MD_B : MD_B)) != 0);
	/* Start + Select held opens the menu, and only when held */
	{
		bool opened = false;
		for (int i = 0; i < 40 && !opened; i++) opened = rig.game_frame(PH_START | PH_SELECT);
		EXPECT(opened);
	}
	/* Select alone for a long time does not */
	{
		bool opened = false;
		for (int i = 0; i < 90 && !opened; i++) opened = rig.game_frame(PH_SELECT);
		EXPECT(!opened);
	}
	pl->finish(EXIT_NORMAL);
	delete pl;
	md_host_unload();
	fprintf(stderr, "player_twin: pads %s %d checks, %d failed\n", con.c_str(), g_checks, g_fail);
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
	st.resize(md_host_state_size());
	size_t n = md_host_state_save_mem(st.data(), st.size());
	return fnv64(st.data(), n);
}

struct Sess {
	Rig rig;
	mdp::MdPlayer *pl;
	std::string root;
	explicit Sess(const std::string &shots) : rig(shots), pl(0) {}
	bool start(const std::string &root_, const std::string &con, const char *rom, const std::string &extra, const std::string &server = "")
	{
		root = root_;
		make_dirs(root + "/launch");
		make_dirs(root + "/saves");
		std::string ini = "version=1\nsession_id=s-sp\nconsole=" + con + "\ngame=" + std::string(rom) + "\nsave=" + root + "/saves/" + path_stem(rom) +
		    ".srm\nroot=" + root + "\nstart=new\nslot=0\ncpu_mhz=444\nenter_button=circle\nlanguage=en\n" + extra;
		write_text(root + "/launch/" + con + ".ini", ini);
		rig.pf.log_path = root + "/" + con + "/log.txt";
		pl = new mdp::MdPlayer(&rig.pf);
		rig.pl = pl;
		rig.pf.player = pl;
		if (!server.empty()) pl->set_ra_server(server);
		char a0[] = "app0:md.bin";
		std::string r = rom;
		char *argv[] = { a0, &r[0], 0 };
		mdp::StartResult sr = pl->start(2, argv, root, true);
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
		md_host_unload();
	}
	~Sess() { end(); }
};

/* ---- Quick response (run-ahead by one frame) through the player -------------------------------------------------------------- */

/* One session with a button script (presses, a rewind, a fast forward, a quick save and load); returns the machine after every refresh. */
static std::vector<uint64_t> ahead_session(const std::string &root, const std::string &con, const char *rom, int quick, uint32_t emu_cost, unsigned *ahead_frames,
                                           unsigned *audio, unsigned *ff_ahead, std::string *line)
{
	std::vector<uint64_t> H;
	Sess s(root + "/shots");
	EXPECT(s.start(root, con, rom, strf("ovr.quick_response=%d\n", quick)));
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
		H.push_back(machine_sig());
	}
	*ahead_frames = rig.ahead_frames;
	*audio = rig.audio_out;
	*line = s.pl->ahead_line();
	return H;
}

static int run_ahead_test(const std::string &con, const char *rom, const std::string &work)
{
	fprintf(stderr, "player_twin ahead: console %s, ROM %s\n", con.c_str(), rom);
	make_dirs(work);
	unsigned a_off = 0, a_on = 0, a_auto = 0, a_heavy = 0, au_off = 0, au_on = 0, au_auto = 0, au_heavy = 0, ff_off = 0, ff_on = 0, ff_auto = 0, ff_heavy = 0;
	std::string l_off, l_on, l_auto, l_heavy;
	/* The first game a process loads does not give the same state hashes as the same game loaded again in that process (Mega Drive: every
	 * refresh; Master System: the first three): something of the core survives md_host_unload. It has nothing to do with run-ahead (two
	 * sessions with it off differ the same way) and the console starts a new process for every game, so one session goes first here and
	 * is not compared. */
	{ unsigned a = 0, b = 0, c = 0; std::string l; ahead_session(work + "/first", con, rom, 0, 0, &a, &b, &c, &l); }
	/* Off, On, Auto on a light game (2 ms a frame) and Auto on a heavy one (9 ms): the machine after every refresh must be the same in all */
	std::vector<uint64_t> off = ahead_session(work + "/off", con, rom, 0, 2000, &a_off, &au_off, &ff_off, &l_off);
	std::vector<uint64_t> on = ahead_session(work + "/on", con, rom, 2, 2000, &a_on, &au_on, &ff_on, &l_on);
	std::vector<uint64_t> au = ahead_session(work + "/auto", con, rom, 1, 2000, &a_auto, &au_auto, &ff_auto, &l_auto);
	std::vector<uint64_t> hv = ahead_session(work + "/heavy", con, rom, 1, 9000, &a_heavy, &au_heavy, &ff_heavy, &l_heavy);
	fprintf(stderr, "off: %s\non: %s\nauto: %s\nauto, heavy: %s\n", l_off.c_str(), l_on.c_str(), l_auto.c_str(), l_heavy.c_str());
	fprintf(stderr, "frames run ahead: off %u, on %u, auto %u, auto on a heavy game %u; sound frames out %u / %u / %u / %u\n", a_off, a_on, a_auto, a_heavy, au_off, au_on, au_auto, au_heavy);
	EXPECT(off.size() == 900 && on.size() == 900 && au.size() == 900 && hv.size() == 900);
	size_t d_on = 0, d_au = 0, d_hv = 0;
	for (size_t i = 0; i < off.size() && i < on.size() && i < au.size() && i < hv.size(); i++) { if (off[i] != on[i]) d_on++; if (off[i] != au[i]) d_au++; if (off[i] != hv[i]) d_hv++; }
	{ size_t f_on = 9999, f_hv = 9999; for (size_t i = off.size(); i-- > 0;) { if (i < on.size() && off[i] != on[i]) f_on = i; if (i < hv.size() && off[i] != hv[i]) f_hv = i; }
	  fprintf(stderr, "refreshes whose machine differs from Off: on %zu (first %zu), auto %zu, auto heavy %zu (first %zu)\n", d_on, f_on, d_au, d_hv, f_hv); }
	EXPECT(d_on == 0);                                      /* the machine is the same after every refresh, through fast forward, rewind and a state load */
	EXPECT(d_au == 0);
	EXPECT(d_hv == 0);
	EXPECT(au_off == au_on && au_off == au_auto && au_off == au_heavy);      /* and so is the amount of sound */
	EXPECT(a_off == 0);
	EXPECT(a_on > 700);                                     /* every normal frame (not the fast forward and rewind ones) */
	EXPECT(ff_on == 0 && ff_auto == 0);                     /* never under fast forward */
	EXPECT(a_auto > 400 && a_auto < a_on);                  /* Auto: after it has seen what the game costs, and again after each fast forward / rewind */
	EXPECT(a_heavy == 0);                                   /* Auto leaves a heavy game alone */
	/* the picture side (the hand-over's drawing thread): the player forwards it to the governor; a saturated picture side holds Auto off, a quiet one lets it start */
	{
		Sess s(work + "/qpic/shots");
		EXPECT(s.start(work + "/qpic", con, rom, "ovr.quick_response=1\n"));
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
		}
	}
	fprintf(stderr, "player_twin: ahead %s %d checks, %d failed\n", con.c_str(), g_checks, g_fail);
	return g_fail ? 1 : 0;
}

static int run_speed_test(const std::string &con, const char *rom, const std::string &work)
{
	fprintf(stderr, "player_twin speed: console %s, ROM %s\n", con.c_str(), rom);
	make_dirs(work);
	/* a one byte patch (a header field no core reads) */
	std::vector<uint8_t> ips;
	ips.insert(ips.end(), (const uint8_t *) "PATCH", (const uint8_t *) "PATCH" + 5);
	const unsigned po = patch_offset(con);
	uint8_t rec[] = { (uint8_t) (po >> 16), (uint8_t) (po >> 8), (uint8_t) po, 0x00, 0x01, 0x5A };
	ips.insert(ips.end(), rec, rec + sizeof rec);
	ips.insert(ips.end(), (const uint8_t *) "EOF", (const uint8_t *) "EOF" + 3);
	EXPECT(write_bytes(work + "/fix.ips", ips));

	/* ---- the patch changes the ROM the core runs (its CRC), the launch keys patch / patch_key as FORMATS.md says ---- */
	uint32_t crc_plain = 0, crc_patched = 0;
	if (con != "scd") {                  /* a disc is not patched (the player says so in its log) */
		Sess a(work + "/shots");
		EXPECT(a.start(work + "/r1", con, rom, ""));
		crc_plain = md_host_rom_crc();
		a.rig.run(5);
	}
	if (con != "scd") {
		Sess b(work + "/shots");
		EXPECT(b.start(work + "/r2", con, rom, "patch=" + work + "/fix.ips\npatch_key=fix-1\n"));
		crc_patched = md_host_rom_crc();
		b.rig.run(5);
		EXPECT(md_host_peek(po) == 0x5A);
	}
	fprintf(stderr, "ROM CRC plain %08x, patched %08x\n", crc_plain, crc_patched);
	EXPECT(con == "scd" || (crc_plain != 0 && crc_patched != 0 && crc_plain != crc_patched));

	Sess s(work + "/shots");
	EXPECT(s.start(work + "/r3", con, rom, ""));
	Rig &rig = s.rig;
	mdp::MdPlayer *pl = s.pl;
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

/* an address of the machine's RetroAchievements memory that changes from one frame to the next (any game has a frame counter somewhere) */
static int find_changing_address(const std::string &con, const char *rom, unsigned *addr)
{
	EXPECT(md_host_init() == 0);
	char e[128];
	EXPECT(load_any(con, rom, e, sizeof e) == 0);
	const int console = md_host_ra_console();
	const unsigned top = console == 1 || console == 9 ? 0x10000 : 0x2000;
	std::vector<uint8_t> a(top), b(top);
	std::vector<int> changes(top, 0);
	for (int i = 0; i < 240; i++) {
		md_host_run_frame();
		md_host_ra_read(0, b.data(), top);
		if (i > 60) for (unsigned k = 0; k < top; k++) if (a[k] != b[k]) changes[k]++;
		a = b;
	}
	md_host_unload();
	int best = -1, cnt = 0;
	/* the Sega CD stand-in has bytes that change on every frame (the achievement test's "changed" condition does not fire on those: a rule of rcheevos'
	 * runtime, the same on every console, and a small test cartridge has a frame counter that changes every frame too); take the busiest byte that is not one of them */
	for (unsigned k = 0; k < top; k++) if (changes[k] > cnt && changes[k] < 150) { cnt = changes[k]; best = (int) k; }
	if (best < 0 || cnt < 20) {           /* no byte that changes now and then: the busiest one (the Sega CD's stand-in has none to offer: it stays 0) */
		int c2 = 0;
		for (unsigned k = 0; k < top; k++) if (changes[k] > c2 && console != 9) { c2 = changes[k]; best = (int) k; }
		if (c2) cnt = c2;
	}
	if (best >= 0) *addr = (unsigned) best;
	fprintf(stderr, "ra: address %06x changes in %d of 179 frames (console id %d)\n", best < 0 ? 0 : (unsigned) best, cnt, console);
	return cnt >= 20 ? console : 0;
}

static int run_ra_test(const std::string &con, const char *rom, const std::string &work)
{
	fprintf(stderr, "player_twin ra: console %s, ROM %s\n", con.c_str(), rom);
	make_dirs(work);
	unsigned addr = 0;
	const int console = find_changing_address(con, rom, &addr);
	EXPECT(console != 0);
	if (!console) { fprintf(stderr, "player_twin ra: %d checks, %d failed\n", g_checks, g_fail); return 1; }
	std::vector<uint8_t> rb = read_bytes(rom);
	if (con == "scd") { char e[128]; rb.assign(512, 0); EXPECT(load_any(con, rom, e, sizeof e) == 0 && md_host_cd_header(rb.data(), rb.size()) == 0); md_host_unload(); }    /* rcheevos: the first 512 bytes of the disc's first sector */
	char hash[33];
	EXPECT(pk::Ra::hash_rom(rb.data(), rb.size(), console, hash));
	char mem[96];
	snprintf(mem, sizeof mem, "0xH%06X!=d0xH%06X", addr, addr);

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
		std::vector<uint8_t> blob(md_host_state_size(), 0);
		EXPECT(!s.pl->restore_state(blob.data(), blob.size()));
		/* fast forward stays */
		const unsigned r0 = s.rig.ran_frames, f0 = s.rig.refreshes;
		for (int i = 0; i < 30; i++) s.rig.game_frame(PH_RS_RIGHT);
		EXPECT(s.pl->speed().ff_on());
		EXPECT(s.rig.ran_frames - r0 >= 2 * (s.rig.refreshes - f0 - 1));        /* it starts with the refresh after the press */
		s.rig.game_frame(0);
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
static __attribute__((noinline)) size_t cost_save(std::vector<uint8_t> &st) { return md_host_state_save_quick(st.data(), st.size()); }
static __attribute__((noinline)) size_t cost_encode(const std::vector<uint8_t> &a, const std::vector<uint8_t> &b, std::vector<uint8_t> &out)
{
	return pk::delta_encode(a.data(), b.data(), a.size(), out.data(), out.size());
}
static __attribute__((noinline)) bool cost_apply(std::vector<uint8_t> &cur, const uint8_t *d, size_t n) { return pk::delta_apply(cur.data(), cur.size(), d, n); }
static __attribute__((noinline)) void cost_frame(bool render) { md_host_set_render(render ? 1 : 0); md_host_run_frame(); }
static double now_ms()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static int run_costs_test(const std::string &con, const char *rom)
{
	EXPECT(md_host_init() == 0);
	char e[128];
	EXPECT(load_any(con, rom, e, sizeof e) == 0);
	const int N = 200;
	for (int i = 0; i < 120; i++) cost_frame(true);
	std::vector<std::vector<uint8_t> > S((size_t) N + 2);
	const size_t sz = md_host_state_size();
	for (size_t i = 0; i < S.size(); i++) { cost_frame(true); S[i].resize(sz); S[i].resize(cost_save(S[i])); }
	/* state capture: what the game thread does at a capture */
	double t0 = now_ms();
	std::vector<uint8_t> tmp(sz);
	double worst = 0;
	for (int i = 0; i < N; i++) { double a = now_ms(); cost_save(tmp); double b = now_ms() - a; if (b > worst) worst = b; }
	const double save_ms = (now_ms() - t0) / N;
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
	fprintf(stderr, "costs (%s, host x86_64, %d runs, state %u bytes):\n", "md ", N, (unsigned) sz);
	fprintf(stderr, "  capture on the game thread (state save)  avg %.3f ms, worst %.3f ms\n", save_ms, worst);
	fprintf(stderr, "  pack on the helper (XOR + squeeze)       avg %.3f ms, worst %.3f ms; a step is %u bytes on average (largest %u)\n", enc_ms, worst_enc, (unsigned) (enc_total / N), (unsigned) enc_max);
	fprintf(stderr, "  one step back (apply)                    avg %.3f ms\n", apply_ms);
	fprintf(stderr, "  one emulated frame of the core           avg %.3f ms\n", frame_ms);
	EXPECT(save_ms < 0.5);
	fprintf(stderr, "player_twin costs: %d checks, %d failed\n", g_checks, g_fail);
	return g_fail ? 1 : 0;
}

int main(int argc, char **argv)
{
	/* Sega CD ("scd"): the disc is a synthetic one (tools/mkscd.py) and the BIOS is its stand-in, found by the player's own search (in the disc's folder); */
	/* the core is told to accept it (it is not a Sega BIOS). */
	if (argc >= 3 && !strcmp(argv[2], "scd")) md_host_cd_bios_test_force(1);
	if (argc >= 4 && !strcmp(argv[1], "costs")) return run_costs_test(argv[2], argv[3]);
	if (argc >= 5 && !strcmp(argv[1], "speed")) return run_speed_test(argv[2], argv[3], argv[4]);
	if (argc >= 5 && !strcmp(argv[1], "ahead")) return run_ahead_test(argv[2], argv[3], argv[4]);
	if (argc >= 5 && !strcmp(argv[1], "ra")) return run_ra_test(argv[2], argv[3], argv[4]);
	if (argc >= 5 && !strcmp(argv[1], "look")) return run_look_test(argv[2], argv[3], argv[4]);
	if (argc >= 5 && !strcmp(argv[1], "pads")) return run_pads_test(argv[2], argv[3], argv[4]);
	if (argc >= 3 && !strcmp(argv[1], "pal")) return run_pal_test(argv[2]);
	if (argc >= 2 && !strcmp(argv[1], "ceilings")) return run_ceiling_test();
	if (argc >= 4 && !strcmp(argv[1], "handoff")) return run_handoff_test(argv[2], argv[3]);
	if (argc >= 6 && !strcmp(argv[1], "test")) return run_test(argv[2], argv[3], argv[4], argv[5]);
	fprintf(stderr, "usage: player_twin test CONSOLE ROM SHOTS WORK | look CONSOLE ROM WORK | pads CONSOLE ROM WORK | pal ROM | ceilings | handoff ROM WORK | speed CONSOLE ROM WORK | ra CONSOLE ROM WORK\n");
	return 2;
}
