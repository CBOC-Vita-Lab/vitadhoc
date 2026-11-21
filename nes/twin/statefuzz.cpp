// Fuzz harness for the NES core's state loader (nes_host_state_load_mem -> FCEUSS_Load_Mem: chunked "FCS" states, no checksum).
// A real state is damaged the way a hostile peer would (bytes inside the chunks, the chunk sizes and names), loaded and run.
// Build with -fsanitize=address,undefined (nes/twin/Makefile target statefuzz).  MIT licence.
//   statefuzz ROM CASEDIR [--seconds N] [--seed N] [--frames N] [--case FILE]...
#include "host.h"
#include "../../tools/statefuzz/sf_common.h"

static void run_frames(int n, sf_rng *r)
{
	static int16_t abuf[4096 * 2];
	for (int f = 0; f < n; f++)
	{
		nes_host_set_pad(0, sf_rand(r) & 0xff); nes_host_set_pad(1, sf_rand(r) & 0xff);
		nes_host_run_frame();
		nes_host_audio_take(abuf, 4096);
		sf_beat();
	}
}
static uint32_t le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
// header 16 bytes, then chunks: type(1) size(4) { name(4) size(4) data }; registers and mapper values are small fields
static void hot_ranges(const std::vector<uint8_t> &b, std::vector<sf_range> &hot)
{
	hot.clear();
	size_t pos = 16;
	while (pos + 5 <= b.size())
	{
		size_t len = le32(&b[pos + 1]);
		if (pos + 5 + len > b.size()) break;
		hot.push_back({ pos, 5 });
		size_t q = pos + 5, end = pos + 5 + len;
		while (q + 8 <= end)
		{
			size_t l2 = le32(&b[q + 4]);
			if (q + 8 + l2 > end) break;
			hot.push_back({ q, 8 + (l2 <= 256 ? l2 : 0) });
			q += 8 + l2;
		}
		pos = end;
	}
}
int main(int argc, char **argv)
{
	if (argc < 3) { fprintf(stderr, "usage: statefuzz ROM CASEDIR [--seconds N] [--seed N] [--frames N] [--case FILE]...\n"); return 2; }
	const char *rom = argv[1], *dir = argv[2]; double secs = 20; uint64_t seed = 1; int frames = 20, warm = 300;
	std::vector<const char *> cases; const char *bios = NULL;
	for (int i = 3; i < argc; i++)
	{
		std::string a = argv[i];
		if (a == "--seconds" && i + 1 < argc) secs = atof(argv[++i]);
		else if (a == "--seed" && i + 1 < argc) seed = strtoull(argv[++i], 0, 10);
		else if (a == "--bios" && i + 1 < argc) bios = argv[++i];
		else if (a == "--frames" && i + 1 < argc) frames = atoi(argv[++i]);
		else if (a == "--warm" && i + 1 < argc) warm = atoi(argv[++i]);
		else if (a == "--case" && i + 1 < argc) cases.push_back(argv[++i]);
	}
	std::vector<uint8_t> rv = sf_read_file(rom);
	if (rv.empty()) { fprintf(stderr, "cannot read %s\n", rom); return 2; }
	char tag[64]; snprintf(tag, sizeof tag, "nes%llu", (unsigned long long)seed);
	sf_init(dir, tag, 20);
	if (nes_host_init()) return 2;
	char err[256] = "";
	{	// a Disk System game needs the BIOS: --bios FILE, or STATEFUZZ_FDS_BIOS
		const char *bp = bios ? bios : getenv("STATEFUZZ_FDS_BIOS");
		if (bp) { std::vector<uint8_t> bb = sf_read_file(bp); if (!bb.empty()) nes_host_set_fds_bios(bb.data(), bb.size()); }
	}
	std::string rn = rom; size_t sl = rn.find_last_of('/'); if (sl != std::string::npos) rn = rn.substr(sl + 1);
	if (nes_host_load_mem(rv.data(), rv.size(), rn.c_str(), err, sizeof err)) { fprintf(stderr, "load: %s\n", err); return 2; }
	sf_rng rng; sf_seed(&rng, seed);
	run_frames(warm, &rng);
	size_t n = nes_host_state_size();
	std::vector<std::vector<uint8_t>> bases;
	for (int k = 0; k < 3; k++)
	{
		std::vector<uint8_t> b(n);
		if (nes_host_state_save_mem(b.data(), n) == 0) { fprintf(stderr, "save failed\n"); return 2; }
		bases.push_back(b);
		run_frames(61, &rng);
	}
	unsigned long ok = 0, refused = 0, iters = 0;
	for (auto &b : bases)
	{
		sf_arm(b.data(), b.size(), 0);
		if (nes_host_state_load_mem(b.data(), b.size()) != 0) { fprintf(stderr, "STATEFUZZ FAIL: a genuine state is refused\n"); return 1; }
		run_frames(frames, &rng);
	}
	if (!cases.empty())
	{
		for (const char *c : cases)
		{
			std::vector<uint8_t> b = sf_read_file(c);
			sf_arm(b.data(), b.size(), iters++);
			int rc = nes_host_state_load_mem(b.data(), b.size());
			if (rc == 0) { run_frames(frames, &rng); ok++; } else refused++;
			printf("case %s: %s\n", c, rc == 0 ? "loaded and ran" : "refused");
		}
		printf("STATEFUZZ cases %lu loaded, %lu refused: OK\n", ok, refused);
		return 0;
	}
	struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
	std::vector<sf_range> hot;
	for (;;)
	{
		struct timespec t1; clock_gettime(CLOCK_MONOTONIC, &t1);
		if ((t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9 > secs) break;
		std::vector<uint8_t> b = bases[sf_below(&rng, (uint32_t)bases.size())];
		hot_ranges(b, hot);
		sf_mutate(&rng, b.data(), b.size(), 0, b.size(), hot);
		sf_arm(b.data(), b.size(), iters);
		int rc = nes_host_state_load_mem(b.data(), b.size());
		if (rc == 0)
		{
			ok++; run_frames(frames, &rng);
			std::vector<uint8_t> s(n);
			if (nes_host_state_save_mem(s.data(), n)) { nes_host_state_load_mem(s.data(), n); run_frames(2, &rng); }
		}
		else { refused++; run_frames(2, &rng); }      // a refused state may have been applied in part: the machine must still run
		iters++;
	}
	printf("STATEFUZZ nes seed %llu: %lu states, %lu loaded and ran, %lu refused: OK (no crash, no hang, no sanitizer report)\n", (unsigned long long)seed, iters, ok, refused);
	return 0;
}
