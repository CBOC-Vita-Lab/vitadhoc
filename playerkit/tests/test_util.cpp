/* Unit tests: pk_util. Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE). */
#include "pk_test.h"
#include "harness.h"
#include <string.h>

using namespace pk;

TEST(util_crc_adler_known_values)
{
	CHECK_EQ(crc32(0, "123456789", 9), 0xCBF43926u);
	CHECK_EQ(adler32(1, "Wikipedia", 9), 0x11E60398u);
	/* incremental equals one shot */
	uint32_t c = crc32(0, "1234", 4);
	c = crc32(c, "56789", 5);
	CHECK_EQ(c, 0xCBF43926u);
}

TEST(util_strings_and_paths)
{
	CHECK_STR(trim("  a b \t\r\n"), "a b");
	CHECK(starts_with("hello", "he"));
	CHECK(!starts_with("he", "hello"));
	CHECK(ends_with_ci("Game.SFC", ".sfc"));
	CHECK(!ends_with_ci("Game.sfc", ".smc"));
	CHECK_STR(lower("AbC"), "abc");
	CHECK_STR(path_stem("ux0:data/x/Game.v2.sfc"), "Game.v2");
	CHECK_STR(path_dir("ux0:data/x/Game.sfc"), "ux0:data/x");
	CHECK_STR(path_join("a/b", "c"), "a/b/c");
	CHECK_STR(path_join("a/b/", "c"), "a/b/c");
	CHECK_STR(strf("%d-%s", 7, "x"), "7-x");
}

TEST(util_ini_round_trip_and_tolerance)
{
	Ini a;
	a.parse("# comment\nkey=value\n  spaced = two words \nbad line\n=novalue\nnum=42\nbig=9000000000\r\nkey=again\n");
	CHECK_STR(a.get("spaced"), "two words");
	CHECK_EQ(a.get_int("num", 0), 42);
	CHECK(a.get_i64("big", 0) == 9000000000LL);
	CHECK_EQ(a.get_int("missing", 7), 7);
	CHECK(!a.has("bad line"));
	CHECK_STR(a.get("key"), "again");    /* the last one wins */
	Ini b;
	b.parse(a.serialize());
	CHECK_STR(b.get("spaced"), "two words");
	CHECK_EQ(b.size(), a.size());
	a.erase("num");
	CHECK(!a.has("num"));
}

TEST(util_atomic_write_and_replace)
{
	pkh::TempDir td;
	std::string p = td.path + "/f.bin";
	std::vector<uint8_t> v1(1000, 0x11), v2(2000, 0x22), back;
	CHECK_EQ(atomic_write(p, &v1[0], v1.size()), W_OK);
	CHECK(file_read(p, back) && back == v1);
	CHECK_EQ(atomic_write(p, &v2[0], v2.size()), W_OK);
	CHECK(file_read(p, back) && back == v2);
	CHECK(!file_exists(p + ".tmp"));
	CHECK(!file_exists(p + ".old"));
	CHECK_EQ(atomic_write("", &v1[0], 1), W_BAD_ARGS);
}

/* fault injection: each step of the swap may fail, and the old file must survive */
static int g_fail_at, g_op;
static FsOps g_real;
static bool f_write(const std::string &p, const uint8_t *d, size_t n) { if (++g_op == g_fail_at) return false; return g_real.write_all(p, d, n); }
static bool f_rename(const std::string &a, const std::string &b) { if (++g_op == g_fail_at) return false; return g_real.rename_file(a, b); }
static bool f_read_corrupt(const std::string &p, std::vector<uint8_t> &o, size_t m)
{
	bool ok = g_real.read_all(p, o, m);
	if (ok && ends_with_ci(p, ".tmp") && g_fail_at == -1 && !o.empty()) o[0] ^= 0xFF;   /* a card that returns wrong bytes */
	return ok;
}

TEST(util_atomic_write_faults_keep_old_file)
{
	pkh::TempDir td;
	std::string p = td.path + "/slot.st0";
	std::vector<uint8_t> oldv(500, 0xAA), newv(700, 0xBB), back;
	CHECK_EQ(atomic_write(p, &oldv[0], oldv.size()), W_OK);
	g_real = fs();
	for (int step = 1; step <= 3; step++) {      /* write temp, set old aside, rename new in */
		g_fail_at = step; g_op = 0;
		fs().write_all = f_write;
		fs().rename_file = f_rename;
		WriteResult rc = atomic_write(p, &newv[0], newv.size());
		fs() = g_real;
		CHECK(rc != W_OK);
		CHECK(file_read(p, back));
		CHECK(back == oldv);
		CHECK(!file_exists(p + ".tmp"));
	}
	/* read-back that differs: W_VERIFY, old kept */
	g_fail_at = -1;
	fs().read_all = f_read_corrupt;
	WriteResult rc = atomic_write(p, &newv[0], newv.size());
	fs() = g_real;
	CHECK_EQ(rc, W_VERIFY);
	CHECK(file_read(p, back) && back == oldv);
	CHECK(write_result_text(W_NO_WRITE)[0] != 0);
}

TEST(util_recover_old_after_power_cut)
{
	pkh::TempDir td;
	std::string p = td.path + "/a.st1";
	std::vector<uint8_t> v(64, 5), back;
	CHECK(fs().write_all(p + ".old", &v[0], v.size()));
	CHECK(!file_exists(p));
	CHECK(recover_old(p));
	CHECK(file_read(p, back) && back == v);
	CHECK(!recover_old(p));            /* nothing to do now */
}

TEST(util_make_dirs_and_clock)
{
	pkh::TempDir td;
	std::string d = td.path + "/a/b/c";
	CHECK(make_dirs(d));
	CHECK(make_dirs(d));               /* already there */
	CHECK(fs().write_all(d + "/probe", (const uint8_t *) "x", 1));     /* the folder is there */
	static int64_t t = 1234;
	set_clock_for_tests([]() -> int64_t { return t; });
	CHECK(now_unix() == 1234);
	set_clock_for_tests(0);
	CHECK(now_unix() > 1600000000);
}
