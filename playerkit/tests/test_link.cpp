/* pk_link: the lockstep session in virtual time (a simulated network: loss, duplicates, reorder, latency, jitter), with a
 * fake machine. Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE). */
#include "pk_test.h"
#include "link_sim.h"

#include <stdio.h>
#include <stdlib.h>

using namespace linksim;
using pk::Link;

namespace {

void le32w(uint8_t *p, uint32_t v) { p[0] = (uint8_t) v; p[1] = (uint8_t) (v >> 8); p[2] = (uint8_t) (v >> 16); p[3] = (uint8_t) (v >> 24); }
uint32_t le32r(const uint8_t *p) { return (uint32_t) p[0] | (uint32_t) p[1] << 8 | (uint32_t) p[2] << 16 | (uint32_t) p[3] << 24; }

/* a valid datagram around a payload (the framing of pk_link.h) */
std::vector<uint8_t> frame_pkt(unsigned type, uint32_t sid, const std::vector<uint8_t> &pl)
{
	std::vector<uint8_t> b(12 + pl.size() + 4);
	memcpy(&b[0], "PKL1", 4);
	b[4] = (uint8_t) type;
	b[5] = 0;
	b[6] = (uint8_t) (pl.size() & 255);
	b[7] = (uint8_t) (pl.size() >> 8);
	le32w(&b[8], sid);
	if (!pl.empty()) memcpy(&b[12], pl.data(), pl.size());
	le32w(&b[12 + pl.size()], pk::crc32(0, b.data(), 12 + pl.size()));
	return b;
}

void refix(std::vector<uint8_t> &b)
{
	if (b.size() < 16) return;
	le32w(&b[b.size() - 4], pk::crc32(0, b.data(), b.size() - 4));
}

/* both consoles end together: the host asks, both must finish DONE on the same frame with the same state */
void finish(Rig &r, bool expect_ok = true)
{
	r.a.request_end();
	r.run(60000000, [&]() { return r.a.over() && r.b.over(); });
	if (!expect_ok) return;
	CHECK(r.a.ok());
	CHECK(r.b.ok());
	CHECK_EQ(r.a.frame(), r.b.frame());
	CHECK_EQ(r.a.frame(), r.a.end_frame());
	CHECK(r.ma.crc() == r.mb.crc());
	CHECK_EQ(r.ma.get(0), r.a.frame());
}

void impaired(int loss, unsigned lat_ms, unsigned jit_ms, int dup, uint32_t frames, unsigned cap, uint32_t seed)
{
	Rig r;
	r.net.rng = seed;
	for (int d = 0; d < 2; d++) { r.net.dir[d].loss = loss; r.net.dir[d].dup = dup; r.net.dir[d].lat = lat_ms * 1000; r.net.dir[d].jit = jit_ms * 1000; }
	r.ca.delay_cap = r.cb.delay_cap = (int) cap;
	CHECK(r.open());
	r.run_frames(frames, 900000000ull);
	CHECK(r.a.frame() >= frames && r.b.frame() >= frames);
	CHECK(!r.a.over() && !r.b.over());
	finish(r);
	const pk::LinkStats sa = r.a.stats(), sb = r.b.stats();
	CHECK(sa.hashes_matched > 0 && sb.hashes_matched > 0);
	CHECK_EQ(sa.frames, r.a.frame());
	fprintf(stderr, "    loss=%d%% lat=%ums jit=%ums dup=%d%%: frames=%u delay=%u stalls=%u/%u retx=%u/%u rtt=%ums jitter=%ums\n", loss,
	        lat_ms, jit_ms, dup, r.a.frame(), r.a.delay(), sa.stalls, sa.stall_episodes, sa.retx, sb.retx, sa.rtt_us / 1000, sa.jitter_us / 1000);
}

}  // namespace

/* ---- the codec ---- */

TEST(link_lz_roundtrip)
{
	uint32_t x = 1;
	for (int round = 0; round < 40; round++) {
		const size_t n = 1 + (size_t) (round * round * 97) % 70000;
		std::vector<uint8_t> src(n);
		for (size_t i = 0; i < n; i++) {
			x = x * 1664525u + 1013904223u;
			src[i] = round % 4 == 0 ? (uint8_t) (x >> 24) : round % 4 == 1 ? 0 : round % 4 == 2 ? (uint8_t) ((i / 7) & 15) : (uint8_t) ((x >> 28) == 0 ? (x >> 20) : 0x55);
		}
		std::vector<uint8_t> c, d(n);
		CHECK(pk::link_lz_compress(src.data(), n, c));
		CHECK(pk::link_lz_decompress(c.data(), c.size(), d.data(), n));
		CHECK(d == src);
		if (round % 4 == 1 && n > 1000) CHECK(c.size() < n / 20);
		/* any one-byte damage never overruns (the CRC is the integrity check) */
		for (int k = 0; k < 20 && !c.empty(); k++) {
			std::vector<uint8_t> bad = c;
			x = x * 1664525u + 1013904223u;
			bad[x % bad.size()] ^= (uint8_t) (1 + (x >> 20) % 255);
			std::vector<uint8_t> out(n);
			pk::link_lz_decompress(bad.data(), bad.size(), out.data(), n);
		}
	}
}

TEST(link_lz_hostile)
{
	uint32_t x = 99;
	std::vector<uint8_t> out(5000);
	for (int round = 0; round < 3000; round++) {
		std::vector<uint8_t> junk((size_t) (round % 200));
		for (size_t i = 0; i < junk.size(); i++) { x = x * 1664525u + 1013904223u; junk[i] = (uint8_t) (x >> 24); }
		pk::link_lz_decompress(junk.data(), junk.size(), out.data(), out.size());
	}
	CHECK(!pk::link_lz_decompress(0, 0, out.data(), 10));
	/* offset before the start, offset zero, a length past the end, trailing bytes */
	const uint8_t a[] = { 0x10, 'x', 0x05, 0x00 };      /* 1 literal, match of 4 at offset 5 (> 1 written) */
	CHECK(!pk::link_lz_decompress(a, sizeof a, out.data(), 5));
	const uint8_t b[] = { 0x10, 'x', 0x00, 0x00 };
	CHECK(!pk::link_lz_decompress(b, sizeof b, out.data(), 5));
	const uint8_t c[] = { 0x10, 'x', 0x01, 0x00 };      /* x then 4 copies of x = 5 bytes */
	CHECK(pk::link_lz_decompress(c, sizeof c, out.data(), 5));
	CHECK(!pk::link_lz_decompress(c, sizeof c, out.data(), 4));
	CHECK(!pk::link_lz_decompress(c, sizeof c, out.data(), 6));
}

/* ---- a session ---- */

TEST(link_clean_run_and_agreed_end)
{
	Rig r;
	r.net.dir[0].lat = r.net.dir[1].lat = 2000;
	CHECK(r.open());
	r.run_frames(1500, 120000000);
	CHECK(r.a.frame() >= 1500 && r.b.frame() >= 1500);
	CHECK(r.a.playing() && r.b.playing());
	CHECK_EQ(r.a.delay(), r.b.delay());
	CHECK(r.a.delay() >= 2 && r.a.delay() <= 3);        /* LAN round trip: the smallest delays */
	finish(r);
	CHECK(r.a.stats().state_wire < r.a.stats().state_raw / 4);     /* the state crossed compressed */
	CHECK(r.wa.saw("link: t="));                        /* the census line */
	CHECK(!r.a.stats().bad && !r.b.stats().bad);
	CHECK(r.b.restored_ok());
}

TEST(link_the_two_machines_equal_at_every_frame)
{
	/* the strictest check: both machines hold the same bytes at the same frame number all through the run */
	Rig r;
	r.net.dir[0].lat = r.net.dir[1].lat = 30000;
	r.net.dir[0].jit = r.net.dir[1].jit = 10000;
	r.net.dir[0].loss = r.net.dir[1].loss = 3;
	CHECK(r.open());
	std::vector<uint32_t> ha(2000, 0), hb(2000, 0);
	r.run(300000000, [&]() {
		const uint32_t fa = r.ma.get(0), fb = r.mb.get(0);
		if (fa < 2000) ha[fa] = pk::crc32(0, r.ma.s.data(), 4200);
		if (fb < 2000) hb[fb] = pk::crc32(0, r.mb.s.data(), 4200);
		return r.a.frame() >= 900 && r.b.frame() >= 900;
	});
	CHECK(r.a.frame() >= 900 && r.b.frame() >= 900);
	unsigned compared = 0;
	for (uint32_t f = 1; f < 900; f++) if (ha[f] && hb[f]) { compared++; if (ha[f] != hb[f]) { CHECK_EQ(f, 0); break; } }
	CHECK(compared > 800);
	/* the periodic hashes compared equal throughout */
	CHECK(r.a.stats().hashes_matched >= 10 && r.b.stats().hashes_matched >= 10);
	finish(r);
}

TEST(link_loss_2_percent_rtt_80)  { impaired(2, 40, 10, 0, 3000, 8, 11); }
TEST(link_loss_5_percent_rtt_120) { impaired(5, 60, 20, 2, 3000, 12, 12); }
TEST(link_loss_10_percent_rtt_150_jitter_40) { impaired(10, 75, 40, 5, 3000, 12, 13); }
TEST(link_reorder_heavy)          { impaired(0, 20, 38, 8, 2000, 8, 14); }
TEST(link_loss_4_percent_lan)     { impaired(4, 2, 3, 0, 3000, 8, 15); }

TEST(link_delay_auto_follows_the_round_trip)
{
	struct Case { unsigned lat_ms, jit_ms; int cap; unsigned lo, hi; } cs[] = {
		{ 1, 0, 8, 2, 2 }, { 25, 5, 8, 3, 5 }, { 60, 10, 12, 5, 8 }, { 300, 30, 8, 8, 8 }, { 300, 30, 12, 12, 12 } };
	for (unsigned i = 0; i < sizeof cs / sizeof cs[0]; i++) {
		Rig r;
		r.net.dir[0].lat = r.net.dir[1].lat = cs[i].lat_ms * 1000;
		r.net.dir[0].jit = r.net.dir[1].jit = cs[i].jit_ms * 1000;
		r.ca.delay_cap = r.cb.delay_cap = cs[i].cap;
		CHECK(r.open());
		r.run_frames(100, 300000000);
		fprintf(stderr, "    one-way %u ms jitter %u ms cap %d: delay %u\n", cs[i].lat_ms, cs[i].jit_ms, cs[i].cap, r.a.delay());
		CHECK(r.a.delay() >= cs[i].lo && r.a.delay() <= cs[i].hi);
		CHECK_EQ(r.a.delay(), r.b.delay());
	}
	Rig r;                                    /* a fixed setting is the host's choice, whatever the network says */
	r.ca.delay = 5;
	r.net.dir[0].lat = r.net.dir[1].lat = 50000;
	CHECK(r.open());
	r.run_frames(100, 300000000);
	CHECK_EQ(r.a.delay(), 5);
	CHECK_EQ(r.b.delay(), 5);
}

TEST(link_a_12_second_stall_survives)
{
	Rig r;
	r.net.dir[0].lat = r.net.dir[1].lat = 20000;
	CHECK(r.open());
	r.run_frames(400, 60000000);
	const uint32_t f0 = r.a.frame();
	r.frozen_until[1] = r.net.t + 12000000;        /* the guest's game thread is busy for 12 s; its I/O thread answers */
	const uint64_t t0 = r.net.t;
	r.run(11500000, [&]() { return r.a.over() || r.b.over(); });
	CHECK(!r.a.over() && !r.b.over());
	CHECK(r.a.frame() < f0 + 40);                  /* the host could not go on without the guest's buttons */
	r.run(14000000 - 11500000, [&]() { return r.a.over() || r.b.over(); });
	CHECK(!r.a.over() && !r.b.over());
	r.run_frames(f0 + 300, 60000000);
	CHECK(r.a.frame() >= f0 + 300);
	CHECK(r.a.stats().max_stall_us >= 11000000u);
	CHECK(r.net.t - t0 >= 12000000);
	finish(r);
}

TEST(link_a_blackout_of_12_seconds_survives)
{
	Rig r;
	r.net.dir[0].lat = r.net.dir[1].lat = 20000;
	CHECK(r.open());
	r.run_frames(400, 60000000);
	const uint32_t f0 = r.a.frame();
	r.net.blackhole = true;                         /* nothing crosses (a Wi-Fi roam), keepalives included */
	r.run(12000000, [&]() { return r.a.over() || r.b.over(); });
	r.net.blackhole = false;
	CHECK(!r.a.over() && !r.b.over());
	r.run_frames(f0 + 300, 60000000);
	CHECK(r.a.frame() >= f0 + 300 && r.b.frame() >= f0 + 300);
	finish(r);
}

TEST(link_a_dead_peer_ends_in_20_seconds)
{
	Rig r;
	r.net.dir[0].lat = r.net.dir[1].lat = 20000;
	CHECK(r.open());
	r.run_frames(300, 60000000);
	const uint64_t t0 = r.net.t;
	r.net.io_dead[1] = true;                        /* the guest is killed: no data, no keepalive */
	r.stepping[1] = false;
	r.run(40000000, [&]() { return r.a.over(); });
	const double s = (double) (r.net.t - t0) / 1e6;
	fprintf(stderr, "    the dead peer was noticed after %.1f s (virtual)\n", s);
	CHECK(r.a.phase() == pk::LP_FAILED);
	CHECK_EQ(r.a.error(), pk::LE_PEER_LOST);
	CHECK(s > 19.0 && s < 22.5);
	CHECK(r.wa.saw("silent"));
}

TEST(link_no_peer_ends_at_the_wait_limit)
{
	Rig r;
	r.ca.wait_ms = 5000;
	CHECK(r.a.open(r.ca, r.ma.iface(), &r.wa));
	r.a.set_peer(r.net.addr[1]);
	r.run(8000000, [&]() { return r.a.over(); });
	CHECK(r.a.phase() == pk::LP_FAILED);
	CHECK_EQ(r.a.error(), pk::LE_NO_PEER);
	CHECK(r.net.t >= 5000000 && r.net.t < 6500000);
}

TEST(link_a_peer_with_a_slow_clock_and_a_fast_one)
{
	Rig r;
	r.net.dir[0].lat = r.net.dir[1].lat = 15000;
	r.period[0] = 16667;
	r.period[1] = 16667 + 500;                      /* the guest's screen runs 3 % slow */
	CHECK(r.open());
	r.run_frames(2500, 900000000);
	CHECK(r.a.frame() >= 2500);
	finish(r);
	CHECK(r.a.stats().stall_episodes > 0);          /* the faster one waited: lockstep, no drift */
}

TEST(link_suspend_ends_both_with_a_plain_reason)
{
	Rig r;
	r.net.dir[0].lat = r.net.dir[1].lat = 10000;
	CHECK(r.open());
	r.run_frames(200, 30000000);
	r.b.suspend();
	CHECK(r.b.phase() == pk::LP_FAILED);
	CHECK_EQ(r.b.error(), pk::LE_SUSPEND);
	r.run(2000000, [&]() { return r.a.over(); });
	CHECK(r.a.phase() == pk::LP_FAILED);
	CHECK_EQ(r.a.error(), pk::LE_PEER_ABORT);
	CHECK_EQ(r.a.peer_reason(), pk::LE_SUSPEND);
	CHECK(std::string(pk::link_error_text(pk::LE_SUSPEND)).find("sleep") != std::string::npos);
}

TEST(link_cancel_before_the_start)
{
	Rig r;
	r.net.dir[0].lat = r.net.dir[1].lat = 10000;
	CHECK(r.open());
	r.run(200000, [&]() { return r.b.phase() == pk::LP_STATE || r.b.phase() == pk::LP_START; });
	r.b.request_end();
	CHECK(r.b.over());
	CHECK_EQ(r.b.error(), pk::LE_LOCAL);
	r.run(1000000, [&]() { return r.a.over(); });
	CHECK(r.a.over());
	CHECK_EQ(r.a.peer_reason(), pk::LE_LOCAL);
}

TEST(link_close_after_a_failed_session_puts_the_guests_game_back)
{
	Rig r;
	r.net.dir[0].lat = r.net.dir[1].lat = 10000;
	const uint32_t own = r.mb.crc();
	CHECK(r.open());
	r.run_frames(100, 30000000);
	CHECK(r.mb.crc() != own);
	r.b.suspend();
	r.b.close();
	CHECK(r.b.restored_ok());
	CHECK(r.mb.crc() == own);
}

/* ---- refusals ---- */

TEST(link_wrong_compat_is_refused_with_a_plain_reason)
{
	Rig r;
	r.cb.compat = 0xDEADBEEF;
	CHECK(r.open());
	r.run(3000000, [&]() { return r.b.over(); });
	CHECK(r.b.phase() == pk::LP_FAILED);
	CHECK_EQ(r.b.error(), pk::LE_PEER_ABORT);
	CHECK_EQ(r.b.peer_reason(), pk::LE_PROTO);
	CHECK(r.a.phase() == pk::LP_WAIT_PEER);         /* the host keeps waiting for the right console */
	CHECK(std::string(pk::link_error_text(pk::LE_PROTO)).find("version") != std::string::npos);
}

TEST(link_wrong_core_or_options_is_refused)
{
	Rig r;
	r.cb.core_tag = r.ca.core_tag ^ 1;              /* the same build, another pinned option */
	CHECK(r.open());
	r.run(3000000, [&]() { return r.b.over(); });
	CHECK_EQ(r.b.error(), pk::LE_PEER_ABORT);
	CHECK_EQ(r.b.peer_reason(), pk::LE_PROTO);
}

TEST(link_another_game_is_refused)
{
	Rig r;
	r.cb.game_crc ^= 0x100;
	CHECK(r.open());
	r.run(3000000, [&]() { return r.b.over(); });
	CHECK_EQ(r.b.error(), pk::LE_PEER_ABORT);
	CHECK_EQ(r.b.peer_reason(), pk::LE_ROM_MISSING);
	CHECK_STR(pk::link_error_name(pk::LE_ROM_MISSING), "rom_missing");
}

TEST(link_a_state_of_another_size_is_refused)
{
	Rig r;
	r.mb = Machine(100000, 5);
	CHECK(r.open());
	r.run(3000000, [&]() { return r.b.over(); });
	CHECK(r.b.over());
	CHECK_EQ(r.b.error(), pk::LE_PROTO);
	r.run(1000000, [&]() { return r.a.over(); });
	CHECK(r.a.over());
	CHECK_EQ(r.a.peer_reason(), pk::LE_PROTO);
}

TEST(link_a_state_over_the_cap_is_not_taken)
{
	Rig r(300000);
	r.ca.state_max = 100000;
	CHECK(!r.a.open(r.ca, r.ma.iface(), &r.wa));
	CHECK_EQ(r.a.error(), pk::LE_START);
}

/* ---- the state's crossing ---- */

TEST(link_a_damaged_state_chunk_is_caught_by_its_crc)
{
	Rig r;
	r.net.dir[0].lat = r.net.dir[1].lat = 5000;
	r.net.mutate = [&](int to, std::vector<uint8_t> &b) {
		if (to == 1 && b.size() > 40 && b[4] == 4 && (b[12] == 3 && b[13] == 0)) { b[12 + 8] ^= 0x40; refix(b); }   /* chunk 3, past the transport's own check */
		return true;
	};
	CHECK(r.open());
	r.run(5000000, [&]() { return r.b.over(); });
	CHECK(r.b.phase() == pk::LP_FAILED);
	CHECK_EQ(r.b.error(), pk::LE_STATE);
	r.run(1000000, [&]() { return r.a.over(); });
	CHECK_EQ(r.a.error(), pk::LE_PEER_ABORT);
	CHECK_EQ(r.a.peer_reason(), pk::LE_STATE);
	CHECK(r.wb.saw("damaged"));
}

TEST(link_a_state_whose_content_crc_is_wrong_is_refused)
{
	Rig r;
	r.net.dir[0].lat = r.net.dir[1].lat = 5000;
	r.net.mutate = [&](int to, std::vector<uint8_t> &b) {
		if (to == 1 && b.size() > 12 + 48 && b[4] == 2) { b[12 + 36] ^= 1; refix(b); }       /* the WELCOME's raw CRC */
		return true;
	};
	CHECK(r.open());
	r.run(5000000, [&]() { return r.b.over(); });
	CHECK_EQ(r.b.error(), pk::LE_STATE);
	CHECK(r.mb.crc() == Machine(200000, 99).crc());            /* nothing was loaded */
}

TEST(link_a_machine_that_refuses_the_state_ends_the_start)
{
	Rig r;
	r.mb.fail_state_load = 1;
	CHECK(r.open());
	r.run(5000000, [&]() { return r.b.over(); });
	CHECK_EQ(r.b.error(), pk::LE_START);
	r.run(1000000, [&]() { return r.a.over(); });
	CHECK_EQ(r.a.peer_reason(), pk::LE_START);
}

TEST(link_the_state_crosses_a_bad_network)
{
	Rig r(900000);
	r.net.dir[0].loss = r.net.dir[1].loss = 12;
	r.net.dir[0].lat = r.net.dir[1].lat = 80000;
	r.net.dir[0].jit = r.net.dir[1].jit = 30000;
	r.net.dir[0].dup = r.net.dir[1].dup = 5;
	CHECK(r.open());
	r.run_frames(60, 120000000);
	CHECK(r.a.frame() >= 60 && r.b.frame() >= 60);
	CHECK(r.a.stats().retx > 0);
	finish(r);
}

TEST(link_a_one_chunk_state)
{
	Rig r(500);
	CHECK(r.open());
	r.run_frames(300, 30000000);
	CHECK(r.a.frame() >= 300);
	finish(r);
}

/* ---- sync ---- */

TEST(link_a_deliberate_desync_is_found_within_the_hash_interval)
{
	Rig r;
	r.net.dir[0].lat = r.net.dir[1].lat = 10000;
	r.mb.diverge_at = 500;                          /* the guest's machine changes a byte of its own at frame 500 */
	CHECK(r.open());
	r.run(60000000, [&]() { return r.a.over() && r.b.over(); });
	CHECK(r.a.over() && r.b.over());
	CHECK(r.a.error() == pk::LE_DESYNC || r.a.peer_reason() == pk::LE_DESYNC);
	CHECK(r.b.error() == pk::LE_DESYNC || r.b.peer_reason() == pk::LE_DESYNC);
	unsigned f = 0;
	bool found = false;
	for (size_t i = 0; i < r.wa.lines.size(); i++)
		if (sscanf(r.wa.lines[i].c_str(), "link: out of sync at frame %u", &f) == 1) found = true;
	for (size_t i = 0; i < r.wb.lines.size(); i++)
		if (sscanf(r.wb.lines[i].c_str(), "link: out of sync at frame %u", &f) == 1) found = true;
	CHECK(found);
	CHECK(f > 500 && f <= 500 + r.ca.hash_every);
	CHECK(r.a.frame() <= 500 + r.ca.hash_every + 20);
	CHECK(r.wa.saw("out of sync") || r.wb.saw("out of sync"));        /* the console that saw it says so; the other hears BYE */
	CHECK(!r.a.ok() && !r.b.ok());
}

TEST(link_a_desync_at_the_very_end_is_found_by_the_final_hash)
{
	Rig r;
	r.ca.hash_every = r.cb.hash_every = 100000;     /* no periodic compare in this run */
	r.net.dir[0].lat = r.net.dir[1].lat = 10000;
	r.mb.diverge_at = 150;
	CHECK(r.open());
	r.run_frames(200, 60000000);
	CHECK(!r.a.over() && !r.b.over());
	finish(r, false);
	CHECK(r.a.over() && r.b.over());
	CHECK(!r.a.ok() && !r.b.ok());
	CHECK(r.a.error() == pk::LE_DESYNC || r.a.peer_reason() == pk::LE_DESYNC);
}

/* ---- the wire's walls ---- */

TEST(link_corrupt_or_foreign_datagrams_change_nothing)
{
	Rig r;
	r.net.dir[0].lat = r.net.dir[1].lat = 10000;
	CHECK(r.open());
	r.run_frames(300, 30000000);
	uint32_t x = 5;
	std::vector<std::vector<uint8_t> > seen;
	r.net.mutate = [&](int, std::vector<uint8_t> &b) { if (seen.size() < 200) seen.push_back(b); return true; };
	r.run_frames(400, 30000000);
	r.net.mutate = 0;
	const uint32_t bad_before = r.a.stats().bad + r.b.stats().bad;
	pk::LinkAddr other; other.ip_be = 0x0900000A; other.port = 777;
	int injected = 0;
	for (size_t i = 0; i < seen.size(); i++) {
		std::vector<uint8_t> d = seen[i];
		x = x * 1664525u + 1013904223u;
		switch (i % 5) {
		case 0: d[x % d.size()] ^= (uint8_t) (1 + (x >> 24) % 255); break;                 /* a flipped byte: the CRC fails */
		case 1: d.resize(d.size() > 3 ? d.size() - 1 - x % 3 : 1); break;                  /* cut short */
		case 2: d.push_back(7); break;                                                      /* a byte too many */
		case 3: break;                                                                      /* an intact copy from another address */
		case 4: d.assign(d.size(), (uint8_t) x); break;
		}
		const int side = (int) (i & 1);
		r.net.inbox[side].push_back(std::make_pair(i % 5 == 3 ? other : r.net.addr[1 - side], d));
		injected++;
	}
	r.run_frames(r.a.frame() + 300, 30000000);
	CHECK(injected > 100);
	CHECK(!r.a.over() && !r.b.over());
	CHECK(r.a.stats().bad + r.b.stats().bad > bad_before + 50);
	CHECK(r.a.stats().foreign + r.b.stats().foreign >= 20);
	finish(r);
}

TEST(link_hostile_datagrams_with_valid_crcs_never_crash)
{
	/* an attacker who has the address and a right CRC: the session may end, nothing may overrun, leak or hang */
	for (int round = 0; round < 12; round++) {
		Rig r;
		r.net.dir[0].lat = r.net.dir[1].lat = 5000;
		CHECK(r.open());
		r.run_frames(60 + 20 * (unsigned) round, 30000000);
		std::vector<std::vector<uint8_t> > seen;
		r.net.mutate = [&](int, std::vector<uint8_t> &b) { if (seen.size() < 300) seen.push_back(b); return true; };
		r.run_frames(r.a.frame() + 20, 30000000);
		r.net.mutate = 0;
		uint32_t x = 1000 + (uint32_t) round;
		for (size_t i = 0; i < seen.size(); i++) {
			std::vector<uint8_t> d = seen[i];
			x = x * 1664525u + 1013904223u;
			const size_t body = d.size() - 16;
			if (body > 0) {
				const int flips = 1 + (int) (x >> 28) % 4;
				for (int k = 0; k < flips; k++) { x = x * 1664525u + 1013904223u; d[12 + (x >> 8) % body] = (uint8_t) x; }
			}
			if (i % 7 == 0 && d.size() > 20) { d.resize(d.size() - 1 - x % 5); d[6] = (uint8_t) (d.size() - 16); d[7] = (uint8_t) ((d.size() - 16) >> 8); }
			refix(d);
			const int side = (int) (i & 1);
			r.net.inbox[side].push_back(std::make_pair(r.net.addr[1 - side], d));
		}
		r.run(5000000, [&]() { return r.a.over() && r.b.over(); });
		/* whatever happened, a console that is still playing is consistent with its own frame count */
		if (!r.a.over()) CHECK_EQ(r.ma.get(0), r.a.frame());
		if (!r.b.over()) CHECK_EQ(r.mb.get(0), r.b.frame());
	}
}

TEST(link_forged_lengths_and_indices)
{
	Rig r;
	r.net.dir[0].lat = r.net.dir[1].lat = 5000;
	uint32_t sid = 0;
	r.net.mutate = [&](int, std::vector<uint8_t> &b) { if (b.size() > 12 && b[4] == 2) sid = le32r(&b[8]); return true; };
	CHECK(r.open());
	r.run_frames(80, 30000000);
	CHECK(sid != 0);
	r.net.mutate = 0;
	const uint32_t f = r.a.frame();
	auto put = [&](int to, unsigned type, const std::vector<uint8_t> &pl) { r.net.inbox[to].push_back(std::make_pair(r.net.addr[1 - to], frame_pkt(type, sid, pl))); };
	std::vector<uint8_t> pl;
	/* an INPUT claiming 255 inputs with none after it; a count that does not match; frame numbers far in the future */
	pl.assign(22, 0); pl[20] = 255; put(0, 8, pl); put(1, 8, pl);
	pl.assign(22 + 4, 0); pl[20] = 2; put(0, 8, pl);
	pl.assign(22 + 128, 0); le32w(&pl[16], 0xFFFFFF00u); pl[20] = 64; put(0, 8, pl); put(1, 8, pl);
	pl.assign(22 + 128, 0); le32w(&pl[0], 0x7FFFFFFF); pl[20] = 64; put(0, 8, pl);    /* an ack from the future */
	pl.assign(22 + 8, 0); pl[21] = 0x1 | (3 << 1); put(0, 8, pl);                      /* flags promise more than is there */
	/* chunks and acks in the wrong place, with wrong indices */
	pl.assign(8, 0); le32w(&pl[0], 0xFFFFFFFFu); put(0, 5, pl); put(1, 4, pl);
	pl.assign(20, 0xFF); put(0, 5, pl);
	pl.assign(2000, 1); put(0, 8, pl);                                                  /* longer than any datagram: the length field wins */
	for (unsigned t = 0; t < 20; t++) { pl.assign(t * 3, (uint8_t) t); put(0, t, pl); put(1, t, pl); }
	r.run(3000000, [&]() { return false; });
	CHECK(r.ma.get(0) == r.a.frame() || r.a.over());
	CHECK(r.a.stats().bad >= 3);
	CHECK(r.a.frame() >= f || r.a.over());
}

TEST(link_a_late_hello_from_a_stranger_is_ignored)
{
	Rig r;
	r.net.dir[0].lat = r.net.dir[1].lat = 5000;
	CHECK(r.open());
	r.run_frames(100, 30000000);
	std::vector<uint8_t> pl(28, 0);
	le32w(&pl[4], r.ca.compat); le32w(&pl[8], r.ca.core_tag); le32w(&pl[12], r.ca.game_crc); le32w(&pl[16], r.ca.game_adler); le32w(&pl[20], r.ca.game_size);
	pl[0] = 1;
	pk::LinkAddr stranger; stranger.ip_be = 0x0900000A; stranger.port = 999;
	for (int i = 0; i < 10; i++) r.net.inbox[0].push_back(std::make_pair(stranger, frame_pkt(1, 0, pl)));
	r.net.inbox[0].push_back(std::make_pair(r.net.addr[1], frame_pkt(1, 0, pl)));       /* the peer's own, repeated */
	r.run_frames(300, 30000000);
	CHECK(r.a.stats().foreign >= 10);
	CHECK(!r.a.over() && !r.b.over());
	finish(r);
}

TEST(link_the_resend_while_stalled_is_rate_limited)
{
	Rig r;
	r.net.dir[0].lat = r.net.dir[1].lat = 10000;
	CHECK(r.open());
	r.run_frames(200, 30000000);
	const uint32_t out0 = r.a.stats().pkts_out;
	const uint64_t t0 = r.net.t;
	r.frozen_until[1] = r.net.t + 8000000;
	r.run(8000000, [&]() { return false; });
	const uint32_t sent = r.a.stats().pkts_out - out0;
	fprintf(stderr, "    %u packets in %.1f s of stall\n", sent, (double) (r.net.t - t0) / 1e6);
	CHECK(sent < 8 * 70);
	CHECK(!r.a.over() && !r.b.over());
}

TEST(link_an_old_peer_without_the_same_framing_is_just_noise)
{
	Rig r;
	CHECK(r.open());
	/* a Game Boy link datagram or a discovery probe on the same port is not ours */
	const char *junk[] = { "GBLD\x01\x02\x03", "GPN1xxxxxxxxxxxxxxxxxxxxxxxx", "PKL2\x01\x00\x00\x00", "" };
	for (int i = 0; i < 4; i++) {
		std::vector<uint8_t> d((const uint8_t *) junk[i], (const uint8_t *) junk[i] + strlen(junk[i]));
		d.push_back(0); d.push_back(1); d.push_back(2);
		r.net.inbox[0].push_back(std::make_pair(r.net.addr[1], d));
		r.net.inbox[1].push_back(std::make_pair(r.net.addr[0], d));
	}
	r.run_frames(100, 30000000);
	CHECK(r.a.frame() >= 100);
	CHECK(r.a.stats().bad >= 4);
}
