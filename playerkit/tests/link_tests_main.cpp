/* link_tests: pk_link over the real UDP transport (loopback), with the transport's own I/O threads: two consoles in two
 * threads of one process, a socket each. Built plain, with AddressSanitizer + UBSan, and with ThreadSanitizer (make link-tsan).
 * Run it ALONE: it uses loopback ports and wall-clock time.
 *   link_tests [--long]     --long adds the 12 s stall and the 20 s dead peer in real time (the default build runs them too
 *                           unless PK_LINK_QUICK is set in the environment)
 * Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE). */
#include "link_sim.h"
#include "pk_link_net.h"

#include <atomic>
#include <fcntl.h>
#include <mutex>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <thread>
#include <time.h>
#include <unistd.h>
#include <vector>

using namespace linksim;

static int g_checks = 0, g_fail = 0;
#define EXPECT(c) do { g_checks++; if (!(c)) { g_fail++; fprintf(stderr, "    FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define EXPECT_EQ(a, b) do { long long _a = (long long) (a), _b = (long long) (b); g_checks++; if (_a != _b) { g_fail++; fprintf(stderr, "    FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, _a, _b); } } while (0)

static uint64_t mono_us()
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
	for (int i = got < 0 ? 0 : got; i < n; i++) out[i] = (uint8_t) (mono_us() >> (i % 5));
}

static int free_udp_port()
{
	int s = socket(AF_INET, SOCK_DGRAM, 0);
	struct sockaddr_in a;
	memset(&a, 0, sizeof a);
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	bind(s, (struct sockaddr *) &a, sizeof a);
	socklen_t l = sizeof a;
	getsockname(s, (struct sockaddr *) &a, &l);
	const int p = ntohs(a.sin_port);
	close(s);
	return p;
}

static std::string make_rom(const char *tag)
{
	char path[128];
	snprintf(path, sizeof path, "/tmp/pk_link_rom_%s_%d.bin", tag, (int) getpid());
	std::vector<uint8_t> d(300000);
	uint32_t x = 77;
	for (size_t i = 0; i < d.size(); i++) { x = x * 1664525u + 1013904223u; d[i] = (uint8_t) (x >> 24); }
	FILE *f = fopen(path, "wb");
	fwrite(d.data(), 1, d.size(), f);
	fclose(f);
	return path;
}

struct Node {
	Machine mach;                              /* before the link: the link restores the machine when it closes */
	pk::NetLink link;
	std::mutex lk;
	std::vector<std::string> lines;
	bool host = false;
	std::thread th;
	std::atomic<int> result_step{0};
	std::atomic<bool> finished{false}, quit{false};   /* the only things the test's main thread reads while the node's thread steps */
	uint32_t stall_at = 0, stall_ms = 0;       /* this console's game thread is busy for stall_ms at that frame */
	uint32_t die_at = 0;                       /* the process dies (no BYE) at that frame */
	uint32_t end_frames = 0;
	bool paced = true;
	uint32_t frame_at_end = 0;
	explicit Node(uint32_t seed, size_t state = 200000) : mach(state, seed) {}
};

static Node *g_log_node[2];
static std::mutex g_log_mx;
static bool g_verbose = false;

static void log_a(const char *l) { if (!g_log_node[0]) return; std::lock_guard<std::mutex> g(g_log_mx); g_log_node[0]->lines.push_back(l); if (g_verbose) fprintf(stderr, "[host] %s\n", l); }
static void log_b(const char *l) { if (!g_log_node[1]) return; std::lock_guard<std::mutex> g(g_log_mx); g_log_node[1]->lines.push_back(l); if (g_verbose) fprintf(stderr, "[guest] %s\n", l); }

static void drive(Node *n)
{
	uint64_t next = mono_us();
	bool asked = false, stalled_once = false;
	while (!n->link.over() && !n->quit) {
		const uint32_t f = n->link.frames();
		if (n->die_at && f >= n->die_at && n->link.playing()) { n->link.abandon(); n->finished = true; return; }
		if (n->stall_at && f >= n->stall_at && !stalled_once && n->link.playing()) { stalled_once = true; usleep(n->stall_ms * 1000); next = mono_us(); }
		const int r = n->link.step(pad_for(n->host, f));
		if (n->host && !asked && n->end_frames && n->link.frames() >= n->end_frames) { n->link.request_end(); asked = true; }
		if (r == pk::LS_OVER) break;
		if (n->paced) {
			next += 16667;
			const uint64_t now = mono_us();
			if (next > now) usleep((useconds_t) (next - now)); else if (now - next > 100000) next = now;
		} else if (r != pk::LS_FRAME) usleep(100);
	}
	n->frame_at_end = n->link.frames();
	n->finished = true;
}

struct Opts {
	int loss = 0, latency = 0, jitter = 0;
	uint32_t frames = 600;
	bool paced = false;
	uint32_t stall_at = 0, stall_ms = 0, die_at = 0;     /* applied to the guest */
	unsigned wait_s = 30;
};

static pk::NetEnv make_env(pk::NetEnv e, int which)
{
	e.now_us = mono_us;
	e.random = rand_bytes;
	e.log = which == 0 ? log_a : log_b;
	return e;
}

static bool run_pair(const Opts &o, Node &a, Node &b, uint64_t max_s)
{
	static int serial = 0;
	char tag[16];
	snprintf(tag, sizeof tag, "%d", ++serial);
	const std::string rom = make_rom(tag);
	const int port = free_udp_port();
	g_log_node[0] = &a;
	g_log_node[1] = &b;
	a.host = true;
	b.host = false;
	pk::LinkRequest ra, rb;
	ra.role = "host"; ra.mode = "lan"; ra.local_port = port; ra.loopback = true; ra.compat = "c0ffee01"; ra.wait_s = (int) o.wait_s;
	rb = ra;
	rb.role = "guest"; rb.local_port = 0; rb.peer = "127.0.0.1:" + std::to_string(port);
	ra.loss = rb.loss = o.loss; ra.latency = rb.latency = o.latency; ra.jitter = rb.jitter = o.jitter;
	a.end_frames = o.frames;
	a.paced = b.paced = o.paced;
	b.stall_at = o.stall_at; b.stall_ms = o.stall_ms; b.die_at = o.die_at;
	pk::NetEnv e0, e1 = make_env(e0, 1);
	e0 = make_env(e0, 0);
	bool ok = a.link.open(ra, rom, 0x5EED0001, a.mach.iface(), e0);
	ok = b.link.open(rb, rom, 0x5EED0001, b.mach.iface(), e1) && ok;
	if (!ok) { unlink(rom.c_str()); return false; }
	a.th = std::thread(drive, &a);
	b.th = std::thread(drive, &b);
	const uint64_t t0 = mono_us();
	while (mono_us() - t0 < max_s * 1000000ull && !(a.finished && b.finished)) usleep(20000);
	const bool done = a.finished && b.finished;
	a.quit = b.quit = true;
	a.th.join();
	b.th.join();
	if (!done) { a.link.abandon(); b.link.abandon(); }
	unlink(rom.c_str());
	return done;
}

static void report(const char *name, Node &a, Node &b)
{
	const pk::LinkStats sa = a.link.stats(), sb = b.link.stats();
	fprintf(stderr, "    %s: host %s frames=%u delay=%u stalls=%u/%u retx=%u in/out=%u/%u rtt=%ums | guest %s frames=%u retx=%u stalls=%u/%u\n", name,
	        a.link.ok() ? "DONE" : a.link.link_error_name(), a.link.frames(), a.link.delay(), sa.stalls, sa.stall_episodes, sa.retx,
	        sa.pkts_in, sa.pkts_out, sa.rtt_us / 1000, b.link.ok() ? "DONE" : b.link.link_error_name(), b.link.frames(), sb.retx, sb.stalls, sb.stall_episodes);
}

static void expect_together(Node &a, Node &b, uint32_t min_frames)
{
	EXPECT(a.link.ok());
	EXPECT(b.link.ok());
	EXPECT_EQ(a.link.frames(), b.link.frames());
	EXPECT(a.link.frames() >= min_frames);
	EXPECT(a.mach.get(0) == a.link.frames());
	/* the guest's machine was put back on its own game when its link closed; before close both are identical */
}

static void t_direct()
{
	fprintf(stderr, "[direct loopback, 3000 frames]\n");
	Node a(7), b(99);
	Opts o;
	o.frames = 3000;
	EXPECT(run_pair(o, a, b, 120));
	report("direct", a, b);
	expect_together(a, b, 3000);
	EXPECT(a.mach.crc() == b.mach.crc());
	EXPECT(a.link.stats().hashes_matched > 5);
	EXPECT(a.link.delay() >= 2 && a.link.delay() <= 5);       /* an idle loopback needs 2 or 3; a machine with more runnable threads than cores may measure a little more */
	EXPECT_EQ(a.link.delay(), b.link.delay());
	EXPECT(a.link.stats().bad == 0 && b.link.stats().bad == 0);
	EXPECT(std::string(a.link.path()) == "lan" || std::string(a.link.path()) == "direct");
}

static void t_impaired(const char *name, int loss, int lat, int jit, uint32_t frames)
{
	fprintf(stderr, "[%s]\n", name);
	Node a(7), b(99);
	Opts o;
	o.frames = frames;
	o.loss = loss; o.latency = lat; o.jitter = jit;
	o.paced = true;       /* real time: the delay chosen from the measured round trip must keep the stalls rare */
	EXPECT(run_pair(o, a, b, 150));
	report(name, a, b);
	expect_together(a, b, frames);
	EXPECT(a.mach.crc() == b.mach.crc());
	EXPECT(a.link.delay() >= 2);
}

static void t_state_crc_via_transport()
{
	fprintf(stderr, "[a game that differs]\n");
	Node a(7), b(99);
	Opts o;
	o.frames = 100;
	/* the other console runs another image */
	const std::string rom_a = make_rom("x1");
	const int port = free_udp_port();
	g_log_node[0] = &a; g_log_node[1] = &b;
	a.host = true; b.host = false;
	pk::LinkRequest ra, rb;
	ra.role = "host"; ra.mode = "lan"; ra.local_port = port; ra.loopback = true; ra.compat = "c0ffee01"; ra.wait_s = 20;
	rb = ra; rb.role = "guest"; rb.local_port = 0; rb.peer = "127.0.0.1:" + std::to_string(port);
	std::string rom_b = rom_a + "b";
	{ FILE *f = fopen(rom_b.c_str(), "wb"); const char z[] = "different game"; fwrite(z, 1, sizeof z, f); fclose(f); }
	pk::NetEnv e0, e1 = make_env(e0, 1);
	e0 = make_env(e0, 0);
	EXPECT(a.link.open(ra, rom_a, 0x5EED0001, a.mach.iface(), e0));
	EXPECT(b.link.open(rb, rom_b, 0x5EED0001, b.mach.iface(), e1));
	const uint64_t t0 = mono_us();
	while (mono_us() - t0 < 5000000 && !b.link.over()) { a.link.step(0); b.link.step(0); usleep(2000); }
	EXPECT(b.link.over());
	EXPECT(b.link.failure() == pk::NF_GAME);
	EXPECT(!a.link.over());                         /* the host goes on waiting for the right console */
	/* wrong compat word */
	Node c(99);
	rb.compat = "deadbeef";
	pk::NetLink l2;
	g_log_node[1] = &c;
	EXPECT(l2.open(rb, rom_a, 0x5EED0001, c.mach.iface(), e1));
	const uint64_t t1 = mono_us();
	while (mono_us() - t1 < 5000000 && !l2.over()) { a.link.step(0); l2.step(0); usleep(2000); }
	EXPECT(l2.over());
	EXPECT(l2.failure() == pk::NF_VERSION);
	a.link.close(); b.link.close(); l2.close();
	unlink(rom_a.c_str()); unlink(rom_b.c_str());
}

static void t_stall12()
{
	fprintf(stderr, "[a 12 s stall of one game thread, real time]\n");
	Node a(7), b(99);
	Opts o;
	o.frames = 700;
	o.paced = true;
	o.stall_at = 300; o.stall_ms = 12000;
	EXPECT(run_pair(o, a, b, 60));
	report("stall12", a, b);
	expect_together(a, b, 700);
	EXPECT(a.link.stats().max_stall_us >= 11000000u);
	EXPECT(a.mach.crc() == b.mach.crc());
}

static void t_dead_peer()
{
	fprintf(stderr, "[a peer that dies without BYE, real time]\n");
	Node a(7), b(99);
	Opts o;
	o.frames = 100000;
	o.paced = true;
	o.die_at = 300;
	const uint64_t t0 = mono_us();
	EXPECT(run_pair(o, a, b, 60));
	const double s = (double) (mono_us() - t0) / 1e6;
	report("dead", a, b);
	fprintf(stderr, "    the host noticed after %.1f s in total (the run to frame 300 is about 5 s)\n", s);
	EXPECT(a.link.phase() == pk::NP_FAILED);
	EXPECT(a.link.failure() == pk::NF_LOST);
	EXPECT(s > 20.0 && s < 33.0);
}

static void t_cancel_and_suspend()
{
	fprintf(stderr, "[suspend and cancel]\n");
	Node a(7), b(99);
	Opts o;
	o.frames = 100000;
	o.paced = false;
	const std::string rom = make_rom("s");
	const int port = free_udp_port();
	g_log_node[0] = &a; g_log_node[1] = &b;
	a.host = true; b.host = false;
	pk::LinkRequest ra, rb;
	ra.role = "host"; ra.mode = "lan"; ra.local_port = port; ra.loopback = true; ra.compat = "c0ffee01"; ra.wait_s = 20;
	rb = ra; rb.role = "guest"; rb.local_port = 0; rb.peer = "127.0.0.1:" + std::to_string(port);
	pk::NetEnv e0, e1 = make_env(e0, 1);
	e0 = make_env(e0, 0);
	EXPECT(a.link.open(ra, rom, 0x5EED0001, a.mach.iface(), e0));
	EXPECT(b.link.open(rb, rom, 0x5EED0001, b.mach.iface(), e1));
	const uint64_t t0 = mono_us();
	while (mono_us() - t0 < 10000000 && !(a.link.frames() > 100 && b.link.frames() > 100)) {
		a.link.step(pad_for(true, a.link.frames()));
		b.link.step(pad_for(false, b.link.frames()));
		usleep(300);
	}
	EXPECT(a.link.playing() && b.link.playing());
	b.link.suspend();
	EXPECT(b.link.over());
	EXPECT_EQ(b.link.link_error(), pk::LE_SUSPEND);
	const uint64_t t1 = mono_us();
	while (mono_us() - t1 < 3000000 && !a.link.over()) { a.link.step(0); usleep(1000); }
	EXPECT(a.link.over());
	EXPECT(!a.link.ok());
	EXPECT_EQ(a.link.peer_reason(), pk::LE_SUSPEND);
	a.link.close(); b.link.close();
	unlink(rom.c_str());
}

int main(int argc, char **argv)
{
	bool quick = getenv("PK_LINK_QUICK") != 0;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-v")) g_verbose = true;
		if (!strcmp(argv[i], "--quick")) quick = true;
		if (!strcmp(argv[i], "--long")) quick = false;
	}
	t_direct();
	t_impaired("loss 2%, 80 ms round trip, 20 ms jitter", 2, 40, 10, 500);
	t_impaired("loss 5%, 120 ms round trip, 30 ms jitter", 5, 60, 15, 500);
	t_impaired("loss 10%, 150 ms round trip, 40 ms jitter", 10, 75, 20, 500);
	t_state_crc_via_transport();
	t_cancel_and_suspend();
	if (!quick) {
		t_stall12();
		t_dead_peer();
	}
	printf("link_tests: %d checks, %d failed\n", g_checks, g_fail);
	return g_fail ? 1 : 0;
}
