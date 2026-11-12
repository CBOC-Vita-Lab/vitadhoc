/* Unit tests: pk_launch. Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE). */
#include "pk_test.h"
#include "harness.h"
#include "pk_launch.h"

using namespace pk;

static LaunchRequest sample()
{
	LaunchRequest r;
	r.session_id = "s-1700000000-1";
	r.console = "snes";
	r.game_path = "ux0:data/GBAdhoc/roms/snes/Super Example World (USA).sfc";
	r.game_name = "Super Example World";
	r.save_path = "ux0:data/GBAdhoc/saves/snes/Super Example World (USA).srm";
	r.shape = SHAPE_PIXEL;
	r.quality = QUALITY_SMOOTH;
	r.cpu_mhz = 444;
	r.enter_is_cross = false;
	r.language = "de";
	r.start = START_SLOT; r.slot = 4;
	r.patch_path = "ux0:data/GBAdhoc/patches/snes/hack.bps";
	r.retroarch_save = "ux0:data/retroarch/saves/Super Example World (USA).srm";
	r.patch_key = "hack-v2";
	r.wide_supported = true;
	r.overrides["scanlines"] = "4";
	r.overrides["glow"] = "6";
	r.unknown["future_key"] = "yes";
	return r;
}

TEST(launch_round_trip)
{
	LaunchRequest a = sample();
	LaunchRequest b = launch_parse(launch_serialize(a));
	CHECK_STR(b.session_id, a.session_id);
	CHECK_STR(b.console, "snes");
	CHECK_STR(b.game_path, a.game_path);
	CHECK_STR(b.game_name, a.game_name);
	CHECK_STR(b.save_path, a.save_path);
	CHECK_EQ(b.shape, SHAPE_PIXEL);
	CHECK_EQ(b.quality, QUALITY_SMOOTH);
	CHECK_EQ(b.cpu_mhz, 444);
	CHECK(!b.enter_is_cross);
	CHECK_STR(b.language, "de");
	CHECK_EQ(b.start, START_SLOT);
	CHECK_EQ(b.slot, 4);
	CHECK_STR(b.patch_path, a.patch_path);
	CHECK_STR(b.retroarch_save, a.retroarch_save);
	CHECK_STR(b.patch_key, "hack-v2");
	CHECK(b.wide_supported);
	CHECK_EQ(b.override_int("scanlines", 0), 4);
	CHECK_EQ(b.override_int("glow", 0), 6);
	CHECK_EQ(b.override_int("nothing", -3), -3);
	CHECK_STR(b.unknown["future_key"], "yes");
	CHECK(b.warnings.empty());
}

TEST(launch_tolerant_of_missing_and_bad_values)
{
	LaunchRequest r = launch_parse("version=1\nconsole=nes\ngame=x.nes\nshape=banana\ncpu_mhz=9999\nslot=77\nstart=slot\n\x01junk\n");
	CHECK_STR(r.console, "nes");
	CHECK_EQ(r.shape, SHAPE_ORIGINAL);            /* default kept */
	CHECK(r.cpu_mhz == 0 || r.cpu_mhz == 444 || r.cpu_mhz == 500);
	CHECK(r.slot >= 0 && r.slot <= 10);
	CHECK(!r.warnings.empty());
	CHECK(r.enter_is_cross);                      /* default */
	CHECK_STR(r.root, "ux0:data/GBAdhoc");
	LaunchRequest e = launch_parse("");
	CHECK(e.console.empty());
	CHECK_EQ(e.version, LAUNCH_VERSION);
}

TEST(launch_newer_version_is_flagged_but_read)
{
	LaunchRequest r = launch_parse("version=9\nconsole=snes\ngame=a.sfc\nfancy=1\n");
	CHECK(r.newer_version);
	CHECK_STR(r.game_path, "a.sfc");
	CHECK_STR(r.unknown["fancy"], "1");
}

TEST(launch_file_write_and_read)
{
	pkh::TempDir td;
	LaunchRequest a = sample();
	a.root = td.path;
	CHECK_STR(launch_path(td.path, "snes"), td.path + "/launch/snes.ini");
	CHECK_EQ(launch_write(a), W_OK);
	LaunchRequest b;
	CHECK(launch_read(td.path, "snes", b));
	CHECK_STR(b.game_path, a.game_path);
	CHECK(!launch_read(td.path, "nes", b));
}

TEST(launch_shape_names)
{
	for (int s = 0; s < SHAPE_COUNT; s++) {
		PictureShape back;
		CHECK(shape_parse(shape_name((PictureShape) s), &back));
		CHECK_EQ(back, s);
	}
	PictureShape x;
	CHECK(!shape_parse("nope", &x));
}

TEST(session_report_round_trip)
{
	SessionReport s;
	s.session_id = "abc"; s.console = "snes"; s.game_path = "g.sfc";
	s.seconds_played = 3725; s.save_changed = true; s.save_error = true;
	s.notice = "Could not save the game: the memory card is full";
	s.states_touched.push_back(2); s.states_touched.push_back(9);
	s.resume_present = true; s.exit = EXIT_GAME_LIST; s.ended_at = 1760000000;
	SessionReport b = session_parse(session_serialize(s));
	CHECK(b.valid);
	CHECK_STR(b.session_id, "abc");
	CHECK(b.seconds_played == 3725);
	CHECK(b.save_changed && b.save_error && b.resume_present);
	CHECK_STR(b.notice, s.notice);
	CHECK_EQ(b.states_touched.size(), 2);
	CHECK_EQ(b.exit, EXIT_GAME_LIST);
	CHECK(b.ended_at == 1760000000);
	CHECK(!session_parse("seconds=5\n").valid);       /* no console: not a report */
	CHECK(!session_parse("").valid);
}

TEST(session_notice_stays_one_line)
{
	SessionReport s; s.console = "nes"; s.notice = "line one\nline two\r\nthree";
	SessionReport b = session_parse(session_serialize(s));
	CHECK(b.notice.find('\n') == std::string::npos);
	CHECK(b.notice.find("line one") != std::string::npos);
}

TEST(session_file_write_read_clear)
{
	pkh::TempDir td;
	SessionReport s; s.console = "nes"; s.session_id = "q"; s.seconds_played = 42;
	CHECK_EQ(session_write(td.path, s), W_OK);
	SessionReport b;
	CHECK(session_read(td.path, b));
	CHECK(b.seconds_played == 42);
	session_clear(td.path);
	CHECK(!session_read(td.path, b));
}

TEST(session_tracker_counts_only_running_time)
{
	LaunchRequest r = sample();
	SessionTracker t;
	t.begin(r);
	t.tick_running(1000);
	t.tick_running(2000);
	t.tick_running(3000);              /* 2 s counted since the first tick */
	t.pause();                         /* the menu opens */
	t.tick_running(60000);             /* first tick after a pause counts nothing */
	t.tick_running(61000);             /* 1 s */
	t.note_state_touched(3); t.note_state_touched(3); t.note_state_touched(1);
	t.note_save_written();
	t.note_notice("State saved to slot 3");
	t.set_resume_present(true);
	SessionReport s = t.finish(EXIT_NORMAL);
	CHECK(s.seconds_played == 3);
	CHECK(s.save_changed);
	CHECK(!s.save_error);
	CHECK_EQ(s.states_touched.size(), 2);          /* a slot is listed once */
	CHECK(s.resume_present);
	CHECK_STR(s.session_id, r.session_id);
	CHECK_STR(s.console, "snes");
	CHECK_STR(s.notice, "State saved to slot 3");
	t.note_save_error("full");
	CHECK(t.current().save_error);
}
