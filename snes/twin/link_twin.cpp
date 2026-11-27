/*
 * link_twin: one SNES player of a two-player link, on Linux: the player's own code (player.cpp: launch file with link.*,
 * linked game rules) and the link itself (netplay.cpp), with a scripted pad instead of a Vita's. Two of these, started
 * with a host and a guest launch file, play one game together over loopback, or through the NAT simulator.
 *
 *   link_twin ROOT [options]
 *     ROOT                the app's data folder; ROOT/launch/snes.ini is the launch file (game, link.*)
 *     --frames N          leave after N emulated frames (the host asks for the clean end; default 1200)
 *     --max-s S           give up after S seconds (default 120)
 *     --seed K            the pad script's seed (the role is mixed in: the two players press different buttons)
 *     --render all|odd|none   which frames this console draws (the picture must not change the machine)
 *     --threads 0|1       the core's APU and PPU worker threads
 *     --fast              no real-time pacing (run as fast as the other side allows)
 *     --hash-at F         print the machine's state CRC at emulated frame F (both consoles must agree)
 *     --dump-at F:PATH    write the machine's state at emulated frame F to PATH (to diff two consoles)
 *     --stall-ms M --stall-at F   stop answering for M ms once at frame F (a state save, the card)
 *   Prints `LT ...` lines; exit status 0 when the link ended the way the launch file's role expects (DONE),
 *   1 on a failed link, 2 on a start-up failure.
 *
 * Copyright CBOC-Vita. MIT licence (see snes/frontend/LICENSE).
 */
#include "player.h"
#include "netplay.h"
#include "host.h"
#include "pk_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <string>
extern "C" {
#include "transport_lan.h"
}
#include <vector>


using namespace pk;
#undef W_OK

static uint64_t now_us()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t) ts.tv_sec * 1000000ull + (uint64_t) ts.tv_nsec / 1000u;
}

static void rand_bytes(uint8_t *out, int n)
{
	int fd = open("/dev/urandom", O_RDONLY);
	int got = 0;
	if (fd >= 0) { got = (int) read(fd, out, (size_t) n); close(fd); }
	for (int i = got < 0 ? 0 : got; i < n; i++) out[i] = (uint8_t) (now_us() >> (i % 5));
}

static void say(const char *line) { printf("LT link: %s\n", line); fflush(stdout); }

struct TwinPlatform : snesp::Platform {
	uint64_t now_us() { return ::now_us(); }
	void pause_us(unsigned us) { usleep(us); }
	bool wake_writer() { return false; }
	bool current_frame(pk::Frame *) { return false; }
	void ask_cheat_text() {}
	std::string path;
	void log_write(const std::string &chunk)
	{
		FILE *f = fopen(path.c_str(), "ab");
		if (f) { fwrite(chunk.data(), 1, chunk.size(), f); fclose(f); }
	}
};

/* the scripted pad: a new random-looking set of buttons every few frames, different for the two roles */
static uint16_t script_pad(uint32_t seed, int host, uint64_t frame)
{
	uint32_t blk = (uint32_t) (frame / 6);
	uint32_t x = blk * 2654435761u ^ seed * 40503u ^ (host ? 0x9E3779B9u : 0x85EBCA6Bu);
	x ^= x >> 15; x *= 0x2C1B3C6Du; x ^= x >> 12; x *= 0x297A2D39u; x ^= x >> 15;
	uint16_t m = (uint16_t) (x & 0x0FFF);
	if ((x >> 16) % 3 == 0) m = 0;                 /* sometimes nothing pressed */
	return m;
}

static uint32_t state_crc()
{
	size_t n = snes_host_state_size();
	std::vector<uint8_t> b(n);
	size_t len = 0;
	if (!n || snes_host_state_save_buf(b.data(), n, &len) != 0) return 0;
	return pk::crc32(0, b.data(), len);
}

/* What the app does for a relayed link before snes.bin starts (link_vita.c / online_vita.c): allocate on the relay with the
 * credentials, swap relayed addresses with the other console (through files here), bind, wait for READY, then LET GO of the
 * allocation without releasing it (lan_transport_relay_handoff) and close the socket.  The request then asks snes.bin's Link to
 * ADOPT it: relay_addr + relay_peer.  0 = done. */
static int form_relay(const pk::LinkRequest &in, pk::LinkRequest &out, const std::string &root, const std::string &peer_file)
{
	out = in;
	lan_config c;
	memset(&c, 0, sizeof c);
	c.mode = in.is_host() ? LAN_MODE_HOST : LAN_MODE_JOIN;
	c.flags = LAN_F_IO_KEEPALIVE | LAN_F_NO_DISCOVERY;
	uint32_t sip = 0; uint16_t sport = 0;
	if (lan_parse_addr(in.sim.c_str(), &sip, &sport, 0) != 0 || !sport) return -1;
	c.sim_ip_be = sip; c.sim_port = sport; c.sim_id = (uint8_t) in.sim_id;
	c.sim_lan_port = (uint16_t) (in.sim_lan_port ? in.sim_lan_port : LAN_PORT_DEFAULT);
	c.netmask_be = 0x00FFFFFFu;
	lan_transport *t = lan_transport_create(&c);
	if (!t) return -2;
	uint8_t seed[16];
	rand_bytes(seed, 16);
	lan_transport_nat_seed(t, seed);
	uint32_t rip = 0; uint16_t rport = 0;
	if (lan_parse_addr(in.relay_server.c_str(), &rip, &rport, 0) != 0 && !rip) { lan_transport_destroy(t); return -3; }
	uint16_t ports[1] = { (uint16_t) atoi(in.relay_ports.c_str()) };
	if (lan_transport_relay_start(t, rip, ports, 1, in.relay_user.c_str(), in.relay_pass.c_str()) != 0) { lan_transport_destroy(t); return -4; }
	lan_nat_status ns;
	uint64_t t0 = now_us();
	do { usleep(20000); lan_transport_nat_status(t, &ns); } while (ns.relay != LAN_RELAY_ALLOCATED && ns.relay != LAN_RELAY_FAILED && now_us() - t0 < 12000000);
	if (ns.relay != LAN_RELAY_ALLOCATED) { printf("LT form relay: no allocation (%d)\n", ns.relay); lan_transport_destroy(t); return -5; }
	char own[40];
	snprintf(own, sizeof own, "%u.%u.%u.%u:%u", ns.relay_ip_be & 255, ns.relay_ip_be >> 8 & 255, ns.relay_ip_be >> 16 & 255, ns.relay_ip_be >> 24, ns.relay_port);
	{ FILE *f = fopen((root + "/form.txt.tmp").c_str(), "wb"); if (f) { fputs(own, f); fclose(f); rename((root + "/form.txt.tmp").c_str(), (root + "/form.txt").c_str()); } }
	char peer[40] = "";
	t0 = now_us();
	while (!peer[0] && now_us() - t0 < 20000000) {
		FILE *f = fopen(peer_file.c_str(), "rb");
		if (f) { size_t n = fread(peer, 1, sizeof peer - 1, f); peer[n] = 0; fclose(f); }
		if (!strchr(peer, ':')) { peer[0] = 0; usleep(50000); }
	}
	uint32_t pip = 0; uint16_t pport = 0;
	if (!peer[0] || lan_parse_addr(peer, &pip, &pport, 0) != 0) { lan_transport_destroy(t); return -6; }
	lan_transport_relay_bind(t, pip, pport);
	t0 = now_us();
	do { usleep(20000); lan_transport_nat_status(t, &ns); } while (ns.relay != LAN_RELAY_READY && ns.relay != LAN_RELAY_FAILED && now_us() - t0 < 10000000);
	if (ns.relay != LAN_RELAY_READY) { printf("LT form relay: not ready (%d)\n", ns.relay); lan_transport_destroy(t); return -7; }
	lan_relay_params prm;
	if (lan_transport_relay_handoff(t, &prm) != 0) { lan_transport_destroy(t); return -8; }
	lan_transport_destroy(t);          /* no Refresh 0: the allocation stays on the server */
	out.relay_addr = own;
	out.relay_peer = peer;
	printf("LT form relay: handed over %s <-> %s\n", own, peer);
	fflush(stdout);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc < 2) { fprintf(stderr, "usage: link_twin ROOT [--frames N] ... (see the top of link_twin.cpp)\n"); return 2; }
	std::string root = argv[1];
	int frames = 1200, max_s = 120, threads = 0, fast = 0, stall_ms = 0, stall_at = 0;
	uint32_t seed = 1;
	std::string render = "all";
	std::vector<int> hash_at;
	int dump_frame = -1;
	std::string dump_path, form_peer;
	for (int i = 2; i < argc; i++) {
		std::string a = argv[i];
		auto nx = [&]() -> const char * { return i + 1 < argc ? argv[++i] : "0"; };
		if (a == "--frames") frames = atoi(nx());
		else if (a == "--max-s") max_s = atoi(nx());
		else if (a == "--seed") seed = (uint32_t) atoi(nx());
		else if (a == "--render") render = nx();
		else if (a == "--threads") threads = atoi(nx());
		else if (a == "--fast") fast = 1;
		else if (a == "--hash-at") hash_at.push_back(atoi(nx()));
		else if (a == "--dump-at") { std::string v = nx(); size_t c = v.find(':'); dump_frame = atoi(v.c_str()); if (c != std::string::npos) dump_path = v.substr(c + 1); }
		else if (a == "--form-relay") form_peer = nx();
		else if (a == "--stall-ms") stall_ms = atoi(nx());
		else if (a == "--stall-at") stall_at = atoi(nx());
		else { fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
	}

	TwinPlatform pf;
	pf.path = root + "/snes/log.txt";
	snesp::SnesPlayer pl(&pf);
	snesp::StartResult sr = pl.start(0, NULL, root, true);
	if (!sr.ok) { printf("LT start FAILED: %s / %s\n", sr.fatal1.c_str(), sr.fatal2.c_str()); return 2; }
	if (!pl.linked()) { printf("LT start FAILED: the launch file holds no link\n"); return 2; }
	snes_host_set_option("apu_thread", threads);
	snes_host_set_option("ppu_thread", threads);
	pl.apply_start_mode();
	const bool host = pl.request().link.is_host();
	const uint32_t crc_before = state_crc();
	printf("LT start role=%s game=%s state_size=%u sram=%u crc_before=%08x\n", host ? "host" : "guest", pl.game_name().c_str(),
	       (unsigned) snes_host_state_size(), (unsigned) snes_host_sram_size(), crc_before);
	fflush(stdout);

	snesnet::Env env;
	env.now_us = now_us;
	env.random = rand_bytes;
	env.log = say;
	env.local_ip_be = 0;
	snesnet::Link link;
	pk::LinkRequest lreq = pl.request().link;
	if (!form_peer.empty()) {
		pk::LinkRequest adopted;
		int fr = form_relay(lreq, adopted, root, form_peer);
		if (fr != 0) { printf("LT form relay FAILED %d\n", fr); pl.set_link_result("never", 0, "", 0); pl.finish(pk::EXIT_GAME_LIST); return 1; }
		lreq = adopted;
	}
	if (!link.open(lreq, pl.link_rom(), env)) {
		printf("LT link open FAILED fail=%d\n", (int) link.failure());
		link.close();
		pl.set_link_result("never", 0, "", 0);
		pl.finish(pk::EXIT_GAME_LIST);
		return 1;
	}

	const uint64_t t0 = now_us();
	uint64_t next = t0;
	const uint64_t frame_us = 16683;
	uint32_t frame = 0;          /* emulated frames this console ran */
	unsigned stall_passes = 0, passes = 0;
	bool end_asked = false, stalled_once = false;
	int rc = 0;
	bool hashed[64] = { false };
	int result_steps = 0;
	for (;;) {
		passes++;
		if (stall_ms && !stalled_once && frame >= (uint32_t) stall_at && frame > 0) {
			stalled_once = true;
			printf("LT stall begin ms=%d at frame %u\n", stall_ms, frame);
			fflush(stdout);
			usleep((useconds_t) stall_ms * 1000);
			next = now_us();
		}
		PadSample ps[MAX_PLAYERS];
		for (int i = 0; i < MAX_PLAYERS; i++) { ps[i] = PadSample(); ps[i].connected = i < 1; }
		pl.input(ps, false, now_us() / 1000, frame);
		const uint16_t mask = script_pad(seed, host, frame);
		bool draw = render == "all" || (render == "odd" && (frame & 1)) || (render == "even" && !(frame & 1)) ||
			            (render == "rnd" && ((frame * 2654435761u >> 7) & 1)) || (render == "third" && frame % 3 == 0);
		snes_host_set_render(draw ? 1 : 0);
		int r = link.step(mask);
		if (r == snesnet::STEP_FRAME) {
			frame++;
			int16_t ab[4096 * 2];
			while (snes_host_audio_take(ab, 4096) > 0) {}
			for (size_t k = 0; k < hash_at.size() && k < 64; k++)
				if (!hashed[k] && (int) frame == hash_at[k]) { hashed[k] = true; printf("LT hash frame=%d crc=%08x\n", hash_at[k], state_crc()); fflush(stdout); }
			if (dump_frame >= 0 && (int) frame == dump_frame && !dump_path.empty()) {
				size_t n = snes_host_state_size(), len = 0;
				std::vector<uint8_t> b(n);
				if (snes_host_state_save_buf(b.data(), n, &len) == 0) { FILE *f = fopen(dump_path.c_str(), "wb"); if (f) { fwrite(b.data(), 1, len, f); fclose(f); } }
			}
			pl.after_frame(now_us());
			if (host && !end_asked && (int) frame >= frames) { end_asked = true; link.request_end(); }
		} else if (r == snesnet::STEP_STALL) stall_passes++;
		else if (r == snesnet::STEP_OVER) break;
		if (!host && !end_asked && link.phase() == snesnet::PH_ENDING) end_asked = true;
		if ((now_us() - t0) / 1000000 > (uint64_t) max_s) { printf("LT timeout after %d s (phase %d)\n", max_s, (int) link.phase()); rc = 1; break; }
		(void) result_steps;
		if (!fast) {
			next += frame_us;
			uint64_t n = now_us();
			if (next > n) usleep((useconds_t) (next - n)); else if (n - next > 200000) next = n;
		} else usleep(200);
	}
	std::string sum = link.summary();
	const bool ok = link.ok();
	const snesnet::Fail fail = link.failure();
	const int gerr = link.gblink_error();
	const std::string gname = link.gblink_error_name();
	const uint32_t frames_run = link.frames();
	snesnet::Link::Stats st;
	link.stats(&st);
	link.close();                                 /* the machine stops: the guest's own game is put back */
	const bool restored = link.restored_ok();
	const uint32_t crc_after = state_crc();
	printf("LT end ok=%d fail=%d gerr=%d(%s) frames=%u stalls=%u/%u hashes=%u/%u passes=%u stall_passes=%u delay=%u restored=%d\n", ok ? 1 : 0, (int) fail, gerr,
	       gname.c_str(), frames_run, st.stalls, st.stall_episodes, st.hashes_matched, st.hashes_sent, passes, stall_passes, st.delay, restored ? 1 : 0);
	printf("LT final role=%s crc_before=%08x crc_after=%08x sram_crc=%08x %s\n", host ? "host" : "guest", crc_before, crc_after,
	       snes_host_sram_size() ? pk::crc32(0, snes_host_sram_data(), snes_host_sram_size()) : 0u, sum.c_str());
	pl.set_link_result(ok ? "done" : "failed", gerr, gname, (int) frames_run);
	pl.finish(pk::EXIT_GAME_LIST);
	fflush(stdout);
	if (rc) return rc;
	return ok ? 0 : 1;
}
