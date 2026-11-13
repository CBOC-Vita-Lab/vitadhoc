/* Unit tests: pk_states. Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE). */
#include "pk_test.h"
#include "harness.h"
#include "pk_states.h"

using namespace pk;

static StateLayout layout(const std::string &root)
{
	StateLayout l;
	l.root = root; l.console = "snes"; l.name = "Super Example World (USA)";
	return l;
}

static std::vector<uint8_t> blob(int seed, size_t n = 30000)
{
	std::vector<uint8_t> v(n);
	for (size_t i = 0; i < n; i++) v[i] = (uint8_t) (seed * 13 + (int) i * 5);
	return v;
}

TEST(states_layout_names)
{
	StateLayout l = layout("ux0:data/GBAdhoc");
	/* the bucket: crc32("Super Example World (USA)") = f3a54da3 */
	CHECK_STR(l.flat_dir(), "ux0:data/GBAdhoc/states/snes");
	CHECK_STR(l.states_dir(), "ux0:data/GBAdhoc/states/snes/f3");
	CHECK_STR(l.slot_path(1), "ux0:data/GBAdhoc/states/snes/f3/Super Example World (USA).st0");
	CHECK_STR(l.slot_path(10), "ux0:data/GBAdhoc/states/snes/f3/Super Example World (USA).st9");
	CHECK_STR(l.resume_path(), "ux0:data/GBAdhoc/states/snes/f3/Super Example World (USA).rsm");
	CHECK_STR(l.scene_path(), "ux0:data/GBAdhoc/scenes/Super Example World (USA).shot");
}

/* The flat layout before buckets (1.3): a game's files move into its bucket the first time it is
 * looked at, with nothing lost; a file already in the bucket wins and the old one stays; once moved, a
 * marker means one stat; another game's files are not touched; a mixed layout reads right. */
TEST(states_bucket_migration)
{
	pkh::TempDir td;
	StateLayout l = layout(td.path);
	std::vector<uint16_t> px; pkh::make_game_frame(4, px);
	Frame f(&px[0], 256, 224, 512);
	std::vector<uint8_t> d1 = blob(1), d2 = blob(2), d5 = blob(5), back;
	/* make real slot files in the bucket, then put them where 1.3 had them */
	{
		States st(l);
		CHECK_EQ(st.save(1, &d1[0], d1.size(), f, 10).status, ST_OK);
		CHECK_EQ(st.save(2, &d2[0], d2.size(), f, 20).status, ST_OK);
	}
	make_dirs(l.flat_dir());
	const char *const files[] = { ".st0", ".st0.thumb", ".st0.shot", ".st0.meta", ".st1", ".st1.thumb",
	                              ".st1.shot", ".st1.meta" };
	for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++)
		CHECK(fs().rename_file(path_join(l.states_dir(), l.name + files[i]),
		                       path_join(l.flat_dir(), l.name + files[i])));
	file_remove(path_join(l.states_dir(), l.name + ".st0.mig"));
	/* slot 2 also in the bucket, newer: it wins, the flat one is kept */
	{
		std::vector<uint8_t> b; CHECK(file_read(path_join(l.flat_dir(), l.name + ".st1"), b, 1 << 20));
		CHECK(fs().write_all(path_join(l.states_dir(), l.name + ".st1"), &d5[0], d5.size()));
	}
	/* another game's file in the flat folder stays */
	CHECK(fs().write_all(path_join(l.flat_dir(), "Other Game.st0"), &d2[0], 10));
	int failed = -1;
	int moved = migrate_to_bucket(l, &failed);
	CHECK_EQ(failed, 0);
	CHECK(moved >= 7);
	CHECK(file_exists(path_join(l.states_dir(), l.name + ".st0.mig")));
	CHECK(file_exists(path_join(l.flat_dir(), l.name + ".st1")));        /* kept, not overwritten */
	CHECK(file_exists(path_join(l.flat_dir(), "Other Game.st0")));
	CHECK(!file_exists(path_join(l.flat_dir(), l.name + ".st0")));
	CHECK_EQ(migrate_to_bucket(l, &failed), 0);                           /* the marker: done */
	States st(l);
	CHECK_EQ(st.load(1, back), ST_OK);
	CHECK(back == d1);
	SlotInfo s2 = st.info(2);
	CHECK(s2.present);
}

TEST(states_thumb_format_bytes)
{
	std::vector<uint16_t> px; pkh::make_game_frame(0, px);
	Frame f(&px[0], 256, 224, 512);
	std::vector<uint8_t> t = thumb_file(f);
	CHECK_EQ(t.size(), THUMB_HEADER + THUMB_W * THUMB_H * 2);
	CHECK(t[0] == 'S' && t[1] == 'T' && t[2] == 'H' && t[3] == '1');
	std::vector<uint16_t> back; int w = 0, h = 0;
	CHECK(parse_thumb(t, back, &w, &h));
	CHECK_EQ(w, THUMB_W); CHECK_EQ(h, THUMB_H);
	CHECK_EQ(back.size(), THUMB_W * THUMB_H);
	/* the sky colour at the top middle is bluish: blue high in the file means blue is the top 5 bits of the raw word */
	uint16_t raw = (uint16_t) (t[THUMB_HEADER + (THUMB_W / 2) * 2] | (t[THUMB_HEADER + (THUMB_W / 2) * 2 + 1] << 8));
	/* the frame is letterboxed: the first rows are black, so look lower in */
	raw = (uint16_t) (t[THUMB_HEADER + (10 * THUMB_W + 32) * 2] | (t[THUMB_HEADER + (10 * THUMB_W + 32) * 2 + 1] << 8));
	CHECK((raw >> 11) > (raw & 31));     /* blue (high bits) larger than red (low bits) in a sky */
	/* garbage is refused */
	std::vector<uint8_t> bad = t; bad[0] = 'X';
	CHECK(!parse_thumb(bad, back, &w, &h));
	bad = t; bad.resize(20);
	CHECK(!parse_thumb(bad, back, &w, &h));
}

TEST(states_shot_format)
{
	std::vector<uint16_t> px; pkh::make_game_frame(1, px);
	Frame f(&px[0], 256, 224, 512);
	std::vector<uint8_t> s = shot_file(f);
	std::vector<uint16_t> back; int w = 0, h = 0;
	CHECK(parse_shot(s, back, &w, &h));
	CHECK(w <= SHOT_MAX_W && h <= SHOT_MAX_H && w > 100 && h > 100);
	/* aspect ratio kept (256x224 -> about 1.143) */
	CHECK(((double) w / h) > 1.10 && ((double) w / h) < 1.18);
	std::vector<uint16_t> rows; int ow, oh;
	make_shot(f, rows, &ow, &oh);
	CHECK_EQ(rows.size(), (size_t) SHOT_STRIDE * oh);
}

TEST(states_box_filter_averages)
{
	/* a checkerboard of black and white pixels shrinks to mid grey */
	std::vector<uint16_t> px(128 * 84);
	for (int y = 0; y < 84; y++) for (int x = 0; x < 128; x++) px[(size_t) y * 128 + x] = ((x + y) & 1) ? 0xFFFF : 0;
	Frame f(&px[0], 128, 84, 256);
	uint16_t t[THUMB_W * THUMB_H];
	make_thumb(f, t);
	uint16_t mid = t[21 * THUMB_W + 32];
	int r = mid & 31;
	CHECK(r >= 13 && r <= 18);
}

TEST(states_save_load_info_remove)
{
	pkh::TempDir td;
	States st(layout(td.path));
	std::vector<uint16_t> px; pkh::make_game_frame(2, px);
	Frame f(&px[0], 256, 224, 512);
	std::vector<uint8_t> d = blob(3), back;
	set_clock_for_tests([]() -> int64_t { return 1760000000; });   /* 9 Oct 2025 */
	SaveReport r = st.save(3, &d[0], d.size(), f, 754);
	set_clock_for_tests(0);
	CHECK_EQ(r.status, ST_OK);
	CHECK(r.thumb_ok && r.shot_ok && r.meta_ok);
	CHECK_STR(r.text(), "");
	SlotInfo si = st.info(3);
	CHECK(si.present && si.has_thumb);
	CHECK_EQ(si.size, d.size());
	CHECK(si.saved == 1760000000);
	CHECK(si.play_seconds == 754);
	CHECK_EQ(st.load(3, back), ST_OK);
	CHECK(back == d);
	CHECK_EQ(st.load(4, back), ST_EMPTY);
	CHECK_EQ(st.load(0, back), ST_BAD_SLOT);
	CHECK_EQ(st.load(11, back), ST_BAD_SLOT);
	CHECK_EQ(st.used_count(), 1);
	SlotInfo all[STATE_SLOTS];
	st.scan(all);
	CHECK(all[2].present && !all[0].present);
	std::vector<uint16_t> pxb; int w, h;
	CHECK(st.read_thumb(3, pxb, &w, &h));
	CHECK(st.read_shot(3, pxb, &w, &h));
	CHECK(!st.read_thumb(4, pxb, &w, &h));
	CHECK(file_exists(st.layout().slot_path(3) + ".thumb"));
	CHECK(st.remove(3));
	CHECK(!file_exists(st.layout().slot_path(3)));
	CHECK(!file_exists(st.layout().slot_path(3) + ".thumb"));
	CHECK(!file_exists(st.layout().slot_path(3) + ".shot"));
	CHECK(!file_exists(st.layout().slot_path(3) + ".meta"));
}

TEST(states_damaged_file_is_detected)
{
	pkh::TempDir td;
	States st(layout(td.path));
	std::vector<uint16_t> px; pkh::make_game_frame(0, px);
	Frame f(&px[0], 256, 224, 512);
	std::vector<uint8_t> d = blob(1), back;
	CHECK_EQ(st.save(1, &d[0], d.size(), f).status, ST_OK);
	std::vector<uint8_t> bad = d; bad[100] ^= 0x40;
	CHECK(fs().write_all(st.layout().slot_path(1), &bad[0], bad.size()));
	CHECK_EQ(st.load(1, back), ST_DAMAGED);
	/* a state with no .meta (saved by an older player) still loads */
	file_remove(st.layout().slot_path(1) + ".meta");
	CHECK_EQ(st.load(1, back), ST_OK);
}

static bool fail_all_writes(const std::string &, const uint8_t *, size_t) { return false; }

TEST(states_failed_save_keeps_previous_slot)
{
	pkh::TempDir td;
	States st(layout(td.path));
	std::vector<uint16_t> px; pkh::make_game_frame(0, px);
	Frame f(&px[0], 256, 224, 512);
	std::vector<uint8_t> a = blob(1), b = blob(2), back;
	CHECK_EQ(st.save(2, &a[0], a.size(), f).status, ST_OK);
	FsOps real = fs();
	fs().write_all = fail_all_writes;
	SaveReport r = st.save(2, &b[0], b.size(), f);
	fs() = real;
	CHECK_EQ(r.status, ST_WRITE);
	CHECK(!r.text().empty());
	CHECK_EQ(st.load(2, back), ST_OK);
	CHECK(back == a);
}

TEST(states_resume_point_round_trip)
{
	pkh::TempDir td;
	States st(layout(td.path));
	std::vector<uint16_t> px; pkh::make_game_frame(4, px);
	Frame f(&px[0], 256, 224, 512);
	std::vector<uint8_t> d = blob(9), back;
	CHECK(!st.resume_present());
	CHECK_EQ(st.save_resume(&d[0], d.size(), f, 99, "patch-a").status, ST_OK);
	CHECK(st.resume_present());
	CHECK_EQ(st.load_resume(back), ST_OK);
	CHECK(back == d);
	CHECK_EQ(st.resume_patch("patch-a"), States::PATCH_SAME);
	CHECK_EQ(st.resume_patch("patch-b"), States::PATCH_DIFFERENT);
	CHECK(st.remove_resume());
	CHECK(!st.resume_present());
	CHECK_EQ(st.resume_patch("x"), States::PATCH_UNKNOWN);
}

TEST(states_resume_due_timing)
{
	pkh::TempDir td;
	States st(layout(td.path));
	CHECK(st.resume_due(1000));            /* none yet */
	st.resume_mark(1000);
	CHECK(!st.resume_due(30000));
	CHECK(st.resume_due(61001));
	CHECK(st.resume_due(5000, 3000));
}

TEST(states_undo_and_scene)
{
	pkh::TempDir td;
	States st(layout(td.path));
	std::vector<uint8_t> d = blob(5), back;
	CHECK(!st.undo_available());
	CHECK(st.undo_store(&d[0], d.size()));
	CHECK(st.undo_available());
	CHECK_EQ(st.undo_fetch(back), ST_OK);
	CHECK(back == d);
	st.undo_clear();
	CHECK(!st.undo_available());
	std::vector<uint16_t> px; pkh::make_game_frame(0, px);
	CHECK(st.save_scene(Frame(&px[0], 256, 224, 512)));
	CHECK(file_exists(st.layout().scene_path()));
	CHECK(!st.save_scene(Frame()));
}

TEST(states_stamp_text)
{
	CHECK_STR(stamp_text(0), "");
	/* TZ=UTC in the test run: 2026-10-03 14:32:00 */
	CHECK_STR(stamp_text(1791037920), "3 Oct 2026, 14:32");
}
