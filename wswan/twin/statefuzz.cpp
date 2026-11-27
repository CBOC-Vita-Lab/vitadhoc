// Fuzz harness for the WonderSwan core's state loader (wswan_host_state_load_mem -> MDFNSS_LoadSM, Mednafen's sectioned format, a CRC-32 trailer).
// A real state is damaged the way a hostile peer would (bytes inside the chunks, the chunk sizes and names, the CRC repaired), loaded and run.
// Build with -fsanitize=address,undefined (wswan/twin/Makefile target statefuzz).  MIT licence.
//   statefuzz ROM CASEDIR [--seconds N] [--seed N] [--frames N] [--case FILE]...
#include "host.h"
#include "../../tools/statefuzz/sf_common.h"

static void run_frames(int n, sf_rng *r)
{
	static int16_t abuf[4096 * 2];
	for (int f = 0; f < n; f++)
	{
		wswan_host_set_pad(sf_rand(r) & 0xfff);
		wswan_host_run_frame();
		wswan_host_audio_take(abuf, 4096);
		sf_beat();
	}
}
static uint32_t le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
// header 32 bytes, then sections { name[32] size(4) { namelen(1) name size(4) data } }
static void hot_ranges(const std::vector<uint8_t> &b, std::vector<sf_range> &hot)
{
	hot.clear();
	size_t pos = 32, n = b.size() - 4;
	while (pos + 36 <= n)
	{
		size_t len = le32(&b[pos + 32]);
		if (len == 0 || pos + 36 + len > n) break;
		hot.push_back({ pos + 32, 4 });
		size_t q = pos + 36, end = pos + 36 + len;
		while (q + 1 <= end)
		{
			size_t nl = b[q];
			if (q + 1 + nl + 4 > end) break;
			size_t l2 = le32(&b[q + 1 + nl]);
			if (q + 1 + nl + 4 + l2 > end) break;
			hot.push_back({ q, 1 + nl + 4 + (l2 <= 256 ? l2 : 0) });
			q += 1 + nl + 4 + l2;
		}
		pos = end;
	}
}
int main(int argc, char **argv)
{
	if (argc < 3) { fprintf(stderr, "usage: statefuzz ROM CASEDIR [--seconds N] [--seed N] [--frames N] [--case FILE]...\n"); return 2; }
	const char *rom = argv[1], *dir = argv[2]; double secs = 20; uint64_t seed = 1; int frames = 20, warm = 300;
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
	char tag[64]; snprintf(tag, sizeof tag, "ws%llu", (unsigned long long)seed);
	sf_init(dir, tag, 20);
	if (wswan_host_init()) return 2;
	char err[256] = "";
	if (wswan_host_load_mem(rv.data(), rv.size(), "fuzz.ws", err, sizeof err)) { fprintf(stderr, "load: %s\n", err); return 2; }
	sf_rng rng; sf_seed(&rng, seed);
	if (proof)
	{
		int f = sf_proof("ws", [&]() { std::vector<uint8_t> b(wswan_host_state_size()); wswan_host_state_save_mem(b.data(), b.size()); return b; }, [&](const std::vector<uint8_t> &b) { return wswan_host_state_load_mem(b.data(), b.size()); }, [&](int n, sf_rng *r) { run_frames(n, r); }, seed);
		printf(f ? "STATEFUZZ proof FAILED (%d)\n" : "STATEFUZZ proof OK: every state loads and runs on exactly as the uninterrupted machine\n", f);
		return f ? 1 : 0;
	}
	run_frames(warm, &rng);
	size_t n = wswan_host_state_size();
	std::vector<std::vector<uint8_t>> bases;
	for (int k = 0; k < 3; k++)
	{
		std::vector<uint8_t> b(n);
		if (wswan_host_state_save_mem(b.data(), n) == 0) { fprintf(stderr, "save failed\n"); return 2; }
		bases.push_back(b);
		run_frames(61, &rng);
	}
	unsigned long ok = 0, refused = 0, iters = 0;
	for (auto &b : bases)
	{
		sf_arm(b.data(), b.size(), 0);
		if (wswan_host_state_load_mem(b.data(), b.size()) != 0) { fprintf(stderr, "STATEFUZZ FAIL: a genuine state is refused\n"); return 1; }
		run_frames(frames, &rng);
	}
	sf_base_dump(bases);
	if (!cases.empty())
	{
		for (const char *c : cases)
		{
			std::vector<uint8_t> b = sf_read_case(c, bases[sf_base_idx < bases.size() ? sf_base_idx : 0]);
			sf_arm(b.data(), b.size(), iters++);
			int rc = wswan_host_state_load_mem(b.data(), b.size());
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
		sf_mutate(&rng, b.data(), b.size() - 4, 0, b.size() - 4, hot);
		{ uint32_t c = sf_crc32(b.data(), b.size() - 4); b[b.size() - 4] = (uint8_t)c; b[b.size() - 3] = (uint8_t)(c >> 8); b[b.size() - 2] = (uint8_t)(c >> 16); b[b.size() - 1] = (uint8_t)(c >> 24); }
		sf_arm(b.data(), b.size(), iters);
		int rc = wswan_host_state_load_mem(b.data(), b.size());
		if (rc == 0)
		{
			ok++; run_frames(frames, &rng);
			std::vector<uint8_t> s(n);
			if (wswan_host_state_save_mem(s.data(), n)) { wswan_host_state_load_mem(s.data(), n); run_frames(2, &rng); }
		}
		else { refused++; run_frames(2, &rng); }      // a refused state may have been applied in part: the machine must still run
		iters++;
	}
	printf("STATEFUZZ ws seed %llu: %lu states, %lu loaded and ran, %lu refused: OK (no crash, no hang, no sanitizer report)\n", (unsigned long long)seed, iters, ok, refused);
	return 0;
}
