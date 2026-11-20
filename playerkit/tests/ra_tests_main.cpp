/* Unit tests: pk_ra against the real rcheevos client and ra_http.c, with a fake server on the loopback (fake_ra.h).
 * Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE). */
#include "pk_test.h"
#include "pk_ra.h"
#include "fake_ra.h"

#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

namespace pktest {
std::vector<Case> &cases() { static std::vector<Case> c; return c; }
int g_failed_checks = 0;
}

using namespace pk;

namespace {

struct Mem {
	uint8_t b[256];
	Mem() { memset(b, 0, sizeof b); }
};

struct Rig {
	pk_test::FakeRa srv;
	Mem mem;
	std::string dir, acct;
	std::vector<std::string> logs;
	bool online = true;
	uint64_t t_us = 0;
	Ra ra;
	std::string hash;

	static uint32_t rd(void *ud, uint32_t a, uint8_t *o, uint32_t n)
	{
		Rig *r = (Rig *) ud;
		if (a >= sizeof r->mem.b) return 0;
		uint32_t k = n < sizeof r->mem.b - a ? n : (uint32_t) sizeof r->mem.b - a;
		memcpy(o, r->mem.b + a, k);
		return k;
	}
	static uint64_t now(void *ud) { return ((Rig *) ud)->t_us; }
	static int onl(void *ud) { return ((Rig *) ud)->online; }
	static void lg(void *ud, const char *l) { ((Rig *) ud)->logs.push_back(l); }

	Rig()
	{
		char t[] = "/tmp/pk_ra_XXXXXX";
		dir = mkdtemp(t);
		acct = dir + "/retroachievements.txt";
		srv.start();
		static const uint8_t rom[] = "a tiny game image for the hash";
		char h[33];
		Ra::hash_rom(rom, sizeof rom, RA_MEGA_DRIVE, h);
		hash = h;
		srv.md5 = hash;
		pk_test::FakeAch a = { 1001, "First Press", "Set byte 0 to 1", 5, "0xH000000=1" };
		srv.achs.push_back(a);
		pk_test::FakeAch b = { 1002, "Never", "Cannot be earned", 10, "0xH000001=77_0xH000001=78" };
		srv.achs.push_back(b);
	}
	~Rig()
	{
		ra.shutdown();
		srv.stop();
		unlink(acct.c_str());
		rmdir(dir.c_str());
	}
	void write_account(bool hardcore, bool indic = true, const char *tok = "tok-1", const char *user = "ra_tester")
	{
		FILE *f = fopen(acct.c_str(), "w");
		fprintf(f, "# test\nuser=%s\ntoken=%s\nhardcore=%d\nindicators=%d\n", user, tok, hardcore, indic);
		fclose(f);
	}
	std::string account_bytes()
	{
		std::string s;
		FILE *f = fopen(acct.c_str(), "rb");
		if (!f) return "<none>";
		char b[512];
		size_t n;
		while ((n = fread(b, 1, sizeof b, f)) > 0) s.append(b, n);
		fclose(f);
		return s;
	}
	bool boot()
	{
		RaHost h;
		h.read = rd; h.now_us = now; h.online = onl; h.log = lg; h.ud = this;
		return ra.boot(dir.c_str(), srv.url().c_str(), "2.0-test", h);
	}
	/* run refreshes (a frame each) until `done` or about 4 s */
	template <class F> bool spin(F done, bool frames = true, bool paused = false)
	{
		for (int i = 0; i < 400; i++) {
			if (frames) ra.frame(paused);
			ra.pump();
			t_us += 16667;
			if (done()) return true;
			usleep(10000);
		}
		return false;
	}
	bool game()
	{
		static const uint8_t rom[] = "a tiny game image for the hash";
		ra.game_begin(rom, sizeof rom, RA_MEGA_DRIVE);
		return spin([&] { return ra.game_state() != 0; });
	}
	std::vector<std::string> toasts()
	{
		std::vector<std::string> v;
		std::string t;
		NoticeKind k;
		while (ra.take_toast(&t, &k)) v.push_back(t);
		return v;
	}
	static bool has(const std::vector<std::string> &v, const char *sub)
	{
		for (size_t i = 0; i < v.size(); i++) if (v[i].find(sub) != std::string::npos) return true;
		return false;
	}
	bool logs_have(const char *sub)
	{
		for (size_t i = 0; i < logs.size(); i++) if (logs[i].find(sub) != std::string::npos) return true;
		return false;
	}
};

}  // namespace

TEST(ra_hash_rules)
{
	char h[33];
	CHECK(Ra::hash_rom((const uint8_t *) "abc", 3, RA_MEGA_DRIVE, h));
	CHECK_STR(h, "900150983cd24fb0d6963f7d28e17f72");
	/* Super NES: a 512 byte copier header is not part of the site's hash; for the others the whole file is */
	std::vector<uint8_t> img(0x8000), hdr(512, 0xAA);
	for (size_t i = 0; i < img.size(); i++) img[i] = (uint8_t) (i * 7 + 3);
	std::vector<uint8_t> with = hdr;
	with.insert(with.end(), img.begin(), img.end());
	char a[33], b[33], c[33], d[33];
	CHECK(Ra::hash_rom(img.data(), img.size(), RA_SNES, a));
	CHECK(Ra::hash_rom(with.data(), with.size(), RA_SNES, b));
	CHECK(Ra::hash_rom(with.data(), with.size(), RA_MEGA_DRIVE, c));
	CHECK(Ra::hash_rom(img.data(), img.size(), RA_MEGA_DRIVE, d));
	CHECK_STR(a, b);
	CHECK(strcmp(c, d) != 0);
	CHECK_STR(a, d);       /* the same bytes, the same MD5 */
	CHECK(!Ra::hash_rom(0, 0, RA_SNES, a));
	CHECK(!Ra::hash_rom(img.data(), 65u << 20, RA_SNES, a));     /* over 64 MB: not hashed (nothing is read) */
}

TEST(ra_no_account_does_nothing)
{
	Rig r;
	CHECK(!r.boot());
	CHECK(!r.ra.up());
	r.ra.pump();
	r.ra.frame(false);
	CHECK_EQ(r.srv.calls().size(), 0);
	r.write_account(false, true, "", "");       /* a file without a token is no account */
	CHECK(!r.boot());
	CHECK(!r.ra.hardcore_blocks());
}

TEST(ra_token_login_game_unlock_and_account_untouched)
{
	Rig r;
	r.write_account(false);
	const std::string before = r.account_bytes();
	CHECK(r.boot());
	CHECK(r.spin([&] { return r.ra.logged_in(); }, false));
	CHECK_EQ(r.srv.count("login2"), 1);
	CHECK(r.game());
	CHECK_EQ(r.ra.game_state(), 1);
	CHECK_EQ(r.ra.total(), 2);
	CHECK(Rig::has(r.toasts(), "0 of 2 achievements"));
	/* waiting: nothing yet; set byte 0 to 1: First Press */
	for (int i = 0; i < 5; i++) r.ra.frame(false);
	r.mem.b[0] = 1;
	CHECK(r.spin([&] { return r.srv.count("awardachievement") >= 1; }));
	std::vector<std::string> t = r.toasts();
	CHECK(Rig::has(t, "Achievement: First Press (5)"));
	std::vector<std::map<std::string, std::string> > cs = r.srv.calls();
	int award = 0;
	for (size_t i = 0; i < cs.size(); i++) if (cs[i]["r"] == "awardachievement") { award++; CHECK_STR(cs[i]["a"], "1001"); CHECK_STR(cs[i]["h"], "0"); }
	CHECK_EQ(award, 1);
	CHECK_EQ(r.ra.unlocked(), 1);
	CHECK(!r.ra.hardcore_blocks());
	/* never written, never in a log line or a toast */
	CHECK_STR(r.account_bytes(), before);
	CHECK(!r.logs_have("tok-1"));
	for (size_t i = 0; i < t.size(); i++) CHECK(t[i].find("tok-1") == std::string::npos);
}

TEST(ra_hardcore_pending_then_active_and_sent_as_hardcore)
{
	Rig r;
	r.write_account(true);
	CHECK(r.boot());
	CHECK(r.spin([&] { return r.ra.logged_in(); }, false));
	static const uint8_t rom[] = "a tiny game image for the hash";
	r.ra.game_begin(rom, sizeof rom, RA_MEGA_DRIVE);
	CHECK(r.ra.hardcore_pending());
	CHECK(r.ra.hardcore_blocks());
	CHECK(r.spin([&] { return r.ra.game_state() != 0; }));
	CHECK(r.ra.hardcore_active());
	CHECK(r.ra.hardcore_blocks());
	for (int i = 0; i < 5; i++) r.ra.frame(false);
	r.mem.b[0] = 1;
	CHECK(r.spin([&] { return r.srv.count("awardachievement") >= 1; }));
	std::vector<std::map<std::string, std::string> > cs = r.srv.calls();
	for (size_t i = 0; i < cs.size(); i++) if (cs[i]["r"] == "awardachievement") CHECK_STR(cs[i]["h"], "1");
	r.ra.game_end();
	CHECK(!r.ra.hardcore_blocks());
}

TEST(ra_unknown_game_blocks_nothing)
{
	Rig r;
	r.write_account(true);
	r.srv.md5 = "00000000000000000000000000000000";
	CHECK(r.boot());
	CHECK(r.spin([&] { return r.ra.logged_in(); }, false));
	CHECK(r.game());
	CHECK_EQ(r.ra.game_state(), -1);
	CHECK(!r.ra.hardcore_blocks());
	CHECK(Rig::has(r.toasts(), "does not know"));
}

TEST(ra_not_processed_while_linked)
{
	Rig r;
	r.write_account(false);
	CHECK(r.boot());
	CHECK(r.spin([&] { return r.ra.logged_in(); }, false));
	CHECK(r.game());
	r.toasts();
	for (int i = 0; i < 5; i++) r.ra.frame(false);
	r.mem.b[0] = 1;
	/* two players: the frames are not handed over, the byte is 1 for a long while */
	for (int i = 0; i < 40; i++) { r.ra.frame(true); r.ra.pump(); usleep(2000); }
	CHECK_EQ(r.srv.count("awardachievement"), 0);
	CHECK(!Rig::has(r.toasts(), "Achievement"));
	/* after the session its progress starts over: the byte must fall and rise again */
	for (int i = 0; i < 20; i++) { r.ra.frame(false); r.ra.pump(); }
	CHECK_EQ(r.srv.count("awardachievement"), 0);
	r.mem.b[0] = 0;
	for (int i = 0; i < 5; i++) { r.ra.frame(false); r.ra.pump(); }
	r.mem.b[0] = 1;
	CHECK(r.spin([&] { return r.srv.count("awardachievement") >= 1; }));
}

TEST(ra_refused_token_asks_for_a_new_sign_in_and_never_writes)
{
	Rig r;
	r.write_account(false, true, "stale-token");
	const std::string before = r.account_bytes();
	CHECK(r.boot());
	CHECK(r.spin([&] { return r.srv.count("login2") >= 1; }, false));
	for (int i = 0; i < 20; i++) { r.ra.pump(); usleep(5000); }
	CHECK(!r.ra.logged_in());
	std::vector<std::string> t = r.toasts();
	CHECK(Rig::has(t, "sign in again"));
	CHECK_STR(r.account_bytes(), before);
	CHECK(!r.logs_have("stale-token"));
	/* a game started now is simply not tracked and blocks nothing */
	static const uint8_t rom[] = "a tiny game image for the hash";
	r.ra.game_begin(rom, sizeof rom, RA_MEGA_DRIVE);
	for (int i = 0; i < 20; i++) { r.ra.frame(false); r.ra.pump(); }
	CHECK_EQ(r.srv.count("achievementsets"), 0);
}

TEST(ra_indicators_off_hides_the_line)
{
	Rig r;
	r.write_account(false, false);
	CHECK(r.boot());
	CHECK(!r.ra.indicators_on());
	CHECK_STR(r.ra.indicator(), "");
}

int main()
{
	int failed = 0;
	for (size_t i = 0; i < pktest::cases().size(); i++) {
		int before = pktest::g_failed_checks;
		pktest::cases()[i].fn();
		bool ok = pktest::g_failed_checks == before;
		printf("[%s] %s\n", ok ? " ok " : "FAIL", pktest::cases()[i].name);
		if (!ok) failed++;
	}
	printf("\n%d tests, %d failed, %d failed checks\n", (int) pktest::cases().size(), failed, pktest::g_failed_checks);
	return failed ? 1 : 0;
}
