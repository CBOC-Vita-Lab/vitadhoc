// Fuzz harness for the SNES core's state loader (snes_host_state_load_buf -> S9xUnfreezeGameMem, the Snes9x snapshot format).
// A real state is damaged the way a hostile peer would (block bytes, the text lengths in the block headers), loaded and run.
// Build with -fsanitize=address,undefined (snes/twin/Makefile target statefuzz).  MIT licence.
//   statefuzz ROM CASEDIR [--seconds N] [--seed N] [--frames N] [--case FILE]...
#include "host.h"
#include "../../tools/statefuzz/sf_common.h"

static void run_frames(int n, sf_rng *r)
{
	static int16_t abuf[4096 * 2];
	for (int f = 0; f < n; f++)
	{
		snes_host_set_pad(0, sf_rand(r) & 0xfff0); snes_host_set_pad(1, sf_rand(r) & 0xfff0);
		snes_host_run_frame();
		snes_host_audio_take(abuf, 4096);
		int w, h, p; snes_host_video(&w, &h, &p);
		sf_beat();
	}
}
// the snapshot: "#!s9xsnp:NNNN\n" then blocks "XXX:NNNNNN:" + data; the small blocks (registers, chip state) are where the damage goes
static void hot_ranges(const std::vector<uint8_t> &b, std::vector<sf_range> &hot)
{
	hot.clear();
	size_t pos = 14;
	while (pos + 11 <= b.size() && b[pos + 3] == ':' && b[pos + 10] == ':')
	{
		size_t len = (size_t)atoi(std::string((const char *)&b[pos + 4], 6).c_str());
		if (pos + 11 + len > b.size()) break;
		if (len <= 4096) hot.push_back({ pos, 11 + len });
		else hot.push_back({ pos, 11 });         // a big block (RAM, VRAM): only its header
		pos += 11 + len;
	}
}
int main(int argc, char **argv)
{
	if (argc < 3) { fprintf(stderr, "usage: statefuzz ROM CASEDIR [--seconds N] [--seed N] [--frames N] [--case FILE]...\n"); return 2; }
	const char *rom = argv[1], *dir = argv[2]; double secs = 20; uint64_t seed = 1; int frames = 20, warm = 600;
	std::vector<const char *> cases; bool proof = false;
	for (int i = 3; i < argc; i++)
	{
		std::string a = argv[i];
		if (a == "--seconds" && i + 1 < argc) secs = atof(argv[++i]);
		else if (a == "--seed" && i + 1 < argc) seed = strtoull(argv[++i], 0, 10);
		else if (a == "--frames" && i + 1 < argc) frames = atoi(argv[++i]);
		else if (a == "--warm" && i + 1 < argc) warm = atoi(argv[++i]);
		else if (a == "--case" && i + 1 < argc) cases.push_back(argv[++i]);
		else if (a == "--base-out" && i + 1 < argc) sf_base_out = argv[++i];
		else if (a == "--base" && i + 1 < argc) sf_base_idx = (size_t)atoi(argv[++i]);
		else if (a == "--proof") proof = true;
	}
	std::vector<uint8_t> rv = sf_read_file(rom);
	if (rv.empty()) { fprintf(stderr, "cannot read %s\n", rom); return 2; }
	char tag[64]; snprintf(tag, sizeof tag, "snes%llu", (unsigned long long)seed);
	sf_init(dir, tag, 30);
	if (snes_host_init()) return 2;
	char err[256] = "";
	if (snes_host_load_rom_mem(rv.data(), rv.size(), "fuzz.sfc", NULL, err, sizeof err)) { fprintf(stderr, "load: %s\n", err); return 2; }
	sf_rng rng; sf_seed(&rng, seed);
	if (proof)
	{
		int f = sf_proof("snes", [&]() { size_t n = snes_host_state_size(), len = 0; std::vector<uint8_t> b(n); snes_host_state_save_buf(b.data(), n, &len); b.resize(len); return b; }, [&](const std::vector<uint8_t> &b) { return snes_host_state_load_buf(b.data(), b.size()); }, [&](int n, sf_rng *r) { run_frames(n, r); }, seed);
		printf(f ? "STATEFUZZ proof FAILED (%d)\n" : "STATEFUZZ proof OK: every state loads and runs on exactly as the uninterrupted machine\n", f);
		return f ? 1 : 0;
	}
	run_frames(warm, &rng);
	std::vector<std::vector<uint8_t>> bases;
	for (int k = 0; k < 3; k++)
	{
		size_t n = snes_host_state_size(), len = 0;
		std::vector<uint8_t> b(n);
		if (snes_host_state_save_buf(b.data(), n, &len) != 0) { fprintf(stderr, "save failed\n"); return 2; }
		b.resize(len); bases.push_back(b);
		run_frames(53, &rng);
	}
	unsigned long ok = 0, refused = 0, iters = 0;
	for (auto &b : bases)
	{
		sf_arm(b.data(), b.size(), 0);
		if (snes_host_state_load_buf(b.data(), b.size()) != 0) { fprintf(stderr, "STATEFUZZ FAIL: a genuine state is refused\n"); return 1; }
		run_frames(frames, &rng);
	}
	sf_base_dump(bases);
	if (!cases.empty())
	{
		for (const char *c : cases)
		{
			std::vector<uint8_t> b = sf_read_case(c, bases[sf_base_idx < bases.size() ? sf_base_idx : 0]);
			sf_arm(b.data(), b.size(), iters++);
			int rc = snes_host_state_load_buf(b.data(), b.size());
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
		int rc = snes_host_state_load_buf(b.data(), b.size());
		if (rc == 0)
		{
			ok++; run_frames(frames, &rng);
			size_t n = snes_host_state_size(), len = 0; std::vector<uint8_t> s(n);
			if (snes_host_state_save_buf(s.data(), n, &len) == 0) { snes_host_state_load_buf(s.data(), len); run_frames(2, &rng); }
		}
		else refused++;
		iters++;
	}
	printf("STATEFUZZ snes seed %llu: %lu states, %lu loaded and ran, %lu refused: OK (no crash, no hang, no sanitizer report)\n", (unsigned long long)seed, iters, ok, refused);
	return 0;
}
