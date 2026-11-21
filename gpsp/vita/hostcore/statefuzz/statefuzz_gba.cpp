// Fuzz harness for the GBA core's state loader (gpSP: retro_unserialize -> gba_load_state, a BSON document of fixed size).
// A real state is damaged the way a hostile peer would: the document's structure stays valid (every length and type as the
// writer made them) and the VALUES inside it are replaced (int32 fields, small binary blocks: registers, I/O, sound channels),
// plus a share of plain byte damage that the structure check must refuse.  Build with the core and this file under
// -fsanitize=address,undefined (make -C gpsp/vita/hostcore statefuzz-gba), interpreter core (DYNAREC=0).  GPL-2.0-or-later (links gpSP).
//   statefuzz_gba ROM CASEDIR [--seconds N] [--seed N] [--frames N] [--warm N] [--case FILE]...
#include "../../../../tools/statefuzz/sf_common.h"
extern "C" {
#include "libretro.h"
	void retro_set_environment(retro_environment_t);
	void retro_set_video_refresh(retro_video_refresh_t);
	void retro_set_audio_sample(retro_audio_sample_t);
	void retro_set_audio_sample_batch(retro_audio_sample_batch_t);
	void retro_set_input_poll(retro_input_poll_t);
	void retro_set_input_state(retro_input_state_t);
	void retro_init(void);
	void retro_deinit(void);
	bool retro_load_game(const struct retro_game_info *);
	void retro_run(void);
	size_t retro_serialize_size(void);
	bool retro_serialize(void *, size_t);
	bool retro_unserialize(const void *, size_t);
}
// the interpreter-only core has no translator: the link layer's call to it is never reached by this harness
extern "C" unsigned execute_arm_translate(unsigned) { abort(); }
static sf_rng *g_pad_rng;
static bool env_cb(unsigned cmd, void *data)
{
	switch (cmd)
	{
	case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: return true;
	case RETRO_ENVIRONMENT_GET_CAN_DUPE: if (data) *(bool *)data = true; return true;
	case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS: return false;
	default: return false;
	}
}
static void video_cb(const void *, unsigned, unsigned, size_t) {}
static void audio_cb(int16_t, int16_t) {}
static size_t audio_batch_cb(const int16_t *, size_t n) { return n; }
static void poll_cb(void) {}
static int16_t input_cb(unsigned, unsigned device, unsigned, unsigned id)
{
	if (device != RETRO_DEVICE_JOYPAD || !g_pad_rng) return 0;
	return (sf_rand(g_pad_rng) & 7) == 0 && id < 10;
}
static void run_frames(int n)
{
	for (int f = 0; f < n; f++) { retro_run(); sf_beat(); }
}

// ---- the BSON walker: where the values are ------------------------------------------------------------------------
static uint32_t rd32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static void walk(const std::vector<uint8_t> &b, size_t at, size_t end, std::vector<sf_range> &ints, std::vector<sf_range> &bins, int depth)
{
	// document: int32 size, elements, 0
	if (depth > 8 || at + 5 > end) return;
	size_t doc_end = at + rd32(&b[at]);
	if (doc_end > end || doc_end < at + 5) return;
	size_t p = at + 4;
	while (p < doc_end - 1)
	{
		uint8_t type = b[p++];
		while (p < doc_end && b[p]) p++;
		p++;
		if (p >= doc_end) return;
		switch (type)
		{
		case 0x10: ints.push_back({ p, 4 }); p += 4; break;
		case 0x03: case 0x04: { size_t sz = rd32(&b[p]); walk(b, p, p + sz, ints, bins, depth + 1); p += sz; } break;
		case 0x05: { size_t sz = rd32(&b[p]); if (sz <= 8192) bins.push_back({ p + 5, sz }); p += 5 + sz; } break;
		default: return;
		}
	}
}
int main(int argc, char **argv)
{
	if (argc < 3) { fprintf(stderr, "usage: statefuzz_gba ROM CASEDIR [--seconds N] [--seed N] [--frames N] [--warm N] [--case FILE]...\n"); return 2; }
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
		else if (a == "--proof") proof = true;
	}
	std::vector<uint8_t> rv = sf_read_file(rom);
	if (rv.empty()) { fprintf(stderr, "cannot read %s\n", rom); return 2; }
	char tag[64]; snprintf(tag, sizeof tag, "gba%llu", (unsigned long long)seed);
	sf_init(dir, tag, 30);
	sf_rng rng; sf_seed(&rng, seed); g_pad_rng = &rng;
	retro_set_environment(env_cb); retro_set_video_refresh(video_cb); retro_set_audio_sample(audio_cb);
	retro_set_audio_sample_batch(audio_batch_cb); retro_set_input_poll(poll_cb); retro_set_input_state(input_cb);
	retro_init();
	struct retro_game_info gi; memset(&gi, 0, sizeof gi); gi.path = rom; gi.data = rv.data(); gi.size = rv.size();
	if (!retro_load_game(&gi)) { fprintf(stderr, "retro_load_game failed\n"); return 2; }
	if (proof)
	{
		int f = sf_proof("gba", [&]() { std::vector<uint8_t> b(retro_serialize_size()); if (!retro_serialize(b.data(), b.size())) b.clear(); return b; }, [&](const std::vector<uint8_t> &b) { return retro_unserialize(b.data(), b.size()) ? 0 : 1; }, [&](int n, sf_rng *r) { g_pad_rng = r; run_frames(n); }, seed);
		printf(f ? "STATEFUZZ proof FAILED (%d)\n" : "STATEFUZZ proof OK: every state loads and runs on exactly as the uninterrupted machine\n", f);
		return f ? 1 : 0;
	}
	run_frames(warm);
	size_t n = retro_serialize_size();
	std::vector<std::vector<uint8_t>> bases;
	for (int k = 0; k < 3; k++)
	{
		std::vector<uint8_t> b(n);
		if (!retro_serialize(b.data(), n)) { fprintf(stderr, "serialize failed\n"); return 2; }
		bases.push_back(b); run_frames(71);
	}
	unsigned long ok = 0, refused = 0, iters = 0;
	for (auto &b : bases)
	{
		sf_arm(b.data(), b.size(), 0);
		if (!retro_unserialize(b.data(), b.size())) { fprintf(stderr, "STATEFUZZ FAIL: a genuine state is refused\n"); return 1; }
		run_frames(frames);
	}
	sf_base_dump(bases);
	if (!cases.empty())
	{
		for (const char *c : cases)
		{
			std::vector<uint8_t> b = sf_read_case(c, bases[0]);
			sf_arm(b.data(), b.size(), iters++);
			bool r = retro_unserialize(b.data(), b.size());
			if (r) { run_frames(frames); ok++; } else refused++;
			printf("case %s: %s\n", c, r ? "loaded and ran" : "refused");
		}
		printf("STATEFUZZ cases %lu loaded, %lu refused: OK\n", ok, refused);
		return 0;
	}
	struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
	std::vector<sf_range> ints, bins;
	static const uint32_t magic[] = { 0, 1, 2, 3, 4, 7, 8, 15, 16, 31, 32, 63, 64, 127, 128, 255, 256, 0x3ff, 0x7fff, 0xffff, 0x10000, 0x7fffffff, 0x80000000u, 0xfffffffeu, 0xffffffffu, 0x03000000u, 0x08000000u, 0x0e000000u };
	for (;;)
	{
		struct timespec t1; clock_gettime(CLOCK_MONOTONIC, &t1);
		if ((t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9 > secs) break;
		std::vector<uint8_t> b = bases[sf_below(&rng, (uint32_t)bases.size())];
		ints.clear(); bins.clear(); walk(b, 0, b.size(), ints, bins, 0);
		if (sf_below(&rng, 12) == 0)
		{
			std::vector<sf_range> none; sf_mutate(&rng, b.data(), b.size(), 0, b.size(), none);       // plain damage: must be refused or harmless
		}
		else
		{
			int hits = (int[]){ 1, 1, 2, 3, 6, 12 }[sf_below(&rng, 6)];
			for (int h = 0; h < hits; h++)
			{
				if (!ints.empty() && (bins.empty() || sf_below(&rng, 3) != 0))
				{
					const sf_range &z = ints[sf_below(&rng, (uint32_t)ints.size())];
					uint32_t v = sf_below(&rng, 3) == 0 ? sf_rand(&rng) : magic[sf_below(&rng, sizeof magic / sizeof *magic)];
					if (sf_below(&rng, 4) == 0) v = rd32(&b[z.off]) + (uint32_t)(int)(sf_below(&rng, 9) - 4);
					memcpy(&b[z.off], &v, 4);
				}
				else if (!bins.empty())
				{
					const sf_range &z = bins[sf_below(&rng, (uint32_t)bins.size())];
					if (!z.len) continue;
					std::vector<sf_range> hotz; hotz.push_back({ z.off, z.len });
					sf_mutate(&rng, b.data(), z.off + z.len, z.off, z.off + z.len, hotz);
				}
			}
		}
		sf_arm(b.data(), b.size(), iters);
		bool r = retro_unserialize(b.data(), b.size());
		if (r)
		{
			ok++; run_frames(frames);
			std::vector<uint8_t> s(n);
			if (retro_serialize(s.data(), n)) { retro_unserialize(s.data(), n); run_frames(2); }
		}
		else { refused++; run_frames(1); }
		iters++;
	}
	printf("STATEFUZZ gba seed %llu: %lu states, %lu loaded and ran, %lu refused: OK (no crash, no hang, no sanitizer report)\n", (unsigned long long)seed, iters, ok, refused);
	return 0;
}
