/* Unit tests: the link.* keys of the launch file and the link result of the session report (ONLINE-2.0 milestone B).
 * Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE). */
#include "pk_test.h"
#include "harness.h"
#include "pk_launch.h"

using namespace pk;

static LaunchRequest linked()
{
	LaunchRequest r;
	r.console = "snes";
	r.game_path = "ux0:data/GBAdhoc/roms/snes/Game.sfc";
	r.link.role = "guest";
	r.link.mode = "relay";
	r.link.local_port = 19019;
	r.link.peer = "203.0.113.10:40000";
	r.link.peer_lan = "192.168.1.20:19019";
	r.link.punch_key = "0123456789abcdef";
	r.link.relay_server = "198.51.100.7";
	r.link.relay_ports = "3478,443";
	r.link.relay_user = "1700000000:abc";
	r.link.relay_pass = "s3cret/+=";
	r.link.relay_peer = "198.51.100.7:50001";
	r.link.relay_addr = "198.51.100.9:50002";
	r.link.delay = 5;
	r.link.compat = "deadbeef";
	r.link.nick = "Player";
	r.link.local_ip = "192.168.1.30";
	r.link.wait_s = 120;
	r.link.loopback = true;
	r.link.sim = "127.0.0.1:47000";
	r.link.sim_id = 1;
	r.link.sim_lan_port = 19019;
	r.link.input_copies = 4;
	r.link.nack = 1;
	r.link.hash_every = 120;
	r.link.loss = 3;
	r.link.latency = 40;
	r.link.jitter = 10;
	return r;
}

TEST(launch_link_round_trip)
{
	LaunchRequest a = linked();
	LaunchRequest b = launch_parse(launch_serialize(a));
	CHECK(b.link.active());
	CHECK(!b.link.is_host());
	CHECK_STR(b.link.mode, "relay");
	CHECK_EQ(b.link.local_port, 19019);
	CHECK_STR(b.link.peer, a.link.peer);
	CHECK_STR(b.link.peer_lan, a.link.peer_lan);
	CHECK_STR(b.link.punch_key, a.link.punch_key);
	CHECK_STR(b.link.relay_server, a.link.relay_server);
	CHECK_STR(b.link.relay_ports, "3478,443");
	CHECK_STR(b.link.relay_user, a.link.relay_user);
	CHECK_STR(b.link.relay_pass, a.link.relay_pass);
	CHECK_STR(b.link.relay_peer, a.link.relay_peer);
	CHECK_STR(b.link.relay_addr, a.link.relay_addr);
	CHECK_EQ(b.link.delay, 5);
	CHECK_STR(b.link.compat, "deadbeef");
	CHECK_STR(b.link.nick, "Player");
	CHECK_STR(b.link.local_ip, "192.168.1.30");
	CHECK_EQ(b.link.wait_s, 120);
	CHECK(b.link.loopback);
	CHECK_STR(b.link.sim, a.link.sim);
	CHECK_EQ(b.link.sim_id, 1);
	CHECK_EQ(b.link.sim_lan_port, 19019);
	CHECK_EQ(b.link.input_copies, 4);
	CHECK_EQ(b.link.nack, 1);
	CHECK_EQ(b.link.hash_every, 120);
	CHECK_EQ(b.link.loss, 3);
	CHECK_EQ(b.link.latency, 40);
	CHECK_EQ(b.link.jitter, 10);
	CHECK(b.unknown.empty());          /* link.* is never kept as an unknown key */
	CHECK_STR(launch_serialize(b), launch_serialize(a));
}

TEST(launch_link_absent_or_unreadable_is_no_link)
{
	LaunchRequest r;
	r.console = "snes";
	r.game_path = "g.sfc";
	CHECK(!launch_parse(launch_serialize(r)).link.active());
	CHECK(launch_serialize(r).find("link.") == std::string::npos);
	/* an unknown role never half-starts a link, and says so */
	LaunchRequest b = launch_parse("version=1\nconsole=snes\ngame=g.sfc\nlink.role=spectator\n");
	CHECK(!b.link.active());
	CHECK(!b.warnings.empty());
	/* keys without a role are ignored */
	CHECK(!launch_parse("version=1\nconsole=snes\ngame=g.sfc\nlink.peer=1.2.3.4:5\n").link.active());
}

TEST(launch_link_defaults_and_bounds)
{
	LaunchRequest b = launch_parse("version=1\nconsole=snes\ngame=g.sfc\nlink.role=Host\nlink.local_port=99999\nlink.delay=300\n"
	                               "link.wait_s=-4\nlink.mode=teleport\n");
	CHECK(b.link.is_host());
	CHECK_STR(b.link.mode, "lan");
	CHECK_EQ(b.link.local_port, 0);
	CHECK_EQ(b.link.delay, 0);
	CHECK_EQ(b.link.wait_s, 0);
	CHECK_EQ(b.link.input_copies, -1);
	CHECK(!b.warnings.empty());
}

TEST(session_report_link_result)
{
	SessionReport s;
	s.session_id = "x";
	s.console = "snes";
	s.link_result = "failed";
	s.link_error = 2;
	s.link_error_name = "rom_missing";
	s.link_frames = 1234;
	s.link_peer_reason = 2;
	SessionReport p = session_parse(session_serialize(s));
	CHECK_STR(p.link_result, "failed");
	CHECK_EQ(p.link_error, 2);
	CHECK_STR(p.link_error_name, "rom_missing");
	CHECK_EQ(p.link_frames, 1234);
	CHECK_EQ(p.link_peer_reason, 2);
	SessionReport n;
	n.console = "snes";
	CHECK(session_serialize(n).find("link_") == std::string::npos);
	CHECK(session_parse(session_serialize(n)).link_result.empty());
}
