// Fuzz harness for the Game Boy / Game Boy Color core's state loader (gbcore_state_load: TGB Dual's gb_restore_state behind
// the image header).  A real state is damaged the way a hostile peer would (the header is kept right about a third of the time,
// so the damage reaches the core block), loaded and run.  Build with the core and this file under -fsanitize=address,undefined
// (make -C gpsp/gbcore statefuzz).  GPL-2.0-or-later (links TGB Dual).
//   statefuzz ROM CASEDIR [--seconds N] [--seed N] [--frames N] [--warm N] [--gbc] [--case FILE]...
#include "../../../tools/statefuzz/sf_common.h"
extern "C" {
#include "../gbcore.h"
}
static sf_rng *g_rng;
static void on_video(void *, const gbcore_video_frame_t *) {}
static void on_audio(void *, const int16_t *, size_t) {}
static int on_serial(void *, uint8_t, int, uint8_t *received) { *received = 0xA5; return 1; }
static void run_frames(gbcore_t *c, int n)
{
	for (int f = 0; f < n; f++) { gbcore_run_frame(c, (uint16_t)(sf_rand(g_rng) & 0xff)); sf_beat(); }
}
int main(int argc, char **argv)
{
	if (argc < 3) { fprintf(stderr, "usage: statefuzz ROM CASEDIR [--seconds N] [--seed N] [--frames N] [--warm N] [--gbc] [--case FILE]...\n"); return 2; }
	const char *rom = argv[1], *dir = argv[2]; double secs = 20; uint64_t seed = 1; int frames = 20, warm = 300; fe_console_t con = FE_CONSOLE_GB;
	std::vector<const char *> cases; bool proof = false;
	for (int i = 3; i < argc; i++)
	{
		std::string a = argv[i];
		if (a == "--seconds" && i + 1 < argc) secs = atof(argv[++i]);
		else if (a == "--seed" && i + 1 < argc) seed = strtoull(argv[++i], 0, 10);
		else if (a == "--frames" && i + 1 < argc) frames = atoi(argv[++i]);
		else if (a == "--warm" && i + 1 < argc) warm = atoi(argv[++i]);
		else if (a == "--gbc") con = FE_CONSOLE_GBC;
		else if (a == "--case" && i + 1 < argc) cases.push_back(argv[++i]);
		else if (a == "--base-out" && i + 1 < argc) sf_base_out = argv[++i];
		else if (a == "--base" && i + 1 < argc) sf_base_idx = (size_t)atoi(argv[++i]);
		else if (a == "--proof") proof = true;
	}
	std::vector<uint8_t> rv = sf_read_file(rom);
	if (rv.empty()) { fprintf(stderr, "cannot read %s\n", rom); return 2; }
	char tag[64]; snprintf(tag, sizeof tag, "gb%llu", (unsigned long long)seed);
	sf_init(dir, tag, 20);
	sf_rng rng; sf_seed(&rng, seed); g_rng = &rng;
	gbcore_callbacks_t cb; memset(&cb, 0, sizeof cb); cb.video = on_video; cb.audio_batch = on_audio; cb.serial_transfer = on_serial;
	gbcore_t *core = gbcore_create(NULL, rv.data(), rv.size(), con, 48000, &cb);
	if (!core) { fprintf(stderr, "gbcore_create failed\n"); return 2; }
	if (proof)
	{
		int f = sf_proof("gb", [&]() { std::vector<uint8_t> b(gbcore_state_size(core)); long w = gbcore_state_save(core, b.data(), b.size()); b.resize(w > 0 ? (size_t)w : 0); return b; }, [&](const std::vector<uint8_t> &b) { return gbcore_state_load(core, b.data(), b.size()); }, [&](int n, sf_rng *r) { g_rng = r; run_frames(core, n); }, seed);
		printf(f ? "STATEFUZZ proof FAILED (%d)\n" : "STATEFUZZ proof OK: every state loads and runs on exactly as the uninterrupted machine\n", f);
		return f ? 1 : 0;
	}
	run_frames(core, warm);
	size_t n = gbcore_state_size(core);
	std::vector<std::vector<uint8_t>> bases;
	for (int k = 0; k < 3; k++)
	{
		std::vector<uint8_t> b(n); long w = gbcore_state_save(core, b.data(), n);
		if (w < 0) { fprintf(stderr, "save failed\n"); return 2; }
		b.resize((size_t)w); bases.push_back(b); run_frames(core, 47);
	}
	unsigned long ok = 0, refused = 0, iters = 0;
	for (auto &b : bases)
	{
		sf_arm(b.data(), b.size(), 0);
		if (gbcore_state_load(core, b.data(), b.size()) != 0) { fprintf(stderr, "STATEFUZZ FAIL: a genuine state is refused\n"); return 1; }
		run_frames(core, frames);
	}
	sf_base_dump(bases);
	if (!cases.empty())
	{
		for (const char *c : cases)
		{
			std::vector<uint8_t> b = sf_read_case(c, bases[sf_base_idx < bases.size() ? sf_base_idx : 0]);
			sf_arm(b.data(), b.size(), iters++);
			int rc = gbcore_state_load(core, b.data(), b.size());
			if (rc == 0) { run_frames(core, frames); ok++; } else refused++;
			printf("case %s: %s\n", c, rc == 0 ? "loaded and ran" : "refused");
		}
		printf("STATEFUZZ cases %lu loaded, %lu refused: OK\n", ok, refused);
		return 0;
	}
	struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
	for (;;)
	{
		struct timespec t1; clock_gettime(CLOCK_MONOTONIC, &t1);
		if ((t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9 > secs) break;
		std::vector<uint8_t> b = bases[sf_below(&rng, (uint32_t)bases.size())];
		std::vector<sf_range> hot;
		hot.push_back({ 40, 64 });
		hot.push_back({ b.size() > 4096 ? b.size() - 4096 : 0, b.size() > 4096 ? 4096 : b.size() });      // the registers and chip state after the RAM
		size_t lo = (sf_below(&rng, 3) != 0) ? 40 : 0;                                                        // keep the identity header most of the time
		sf_mutate(&rng, b.data(), b.size(), lo, b.size(), hot);
		sf_arm(b.data(), b.size(), iters);
		int rc = gbcore_state_load(core, b.data(), b.size());
		if (rc == 0)
		{
			ok++; run_frames(core, frames);
			std::vector<uint8_t> s(n); long w = gbcore_state_save(core, s.data(), n);
			if (w > 0) { gbcore_state_load(core, s.data(), (size_t)w); run_frames(core, 2); }
		}
		else { refused++; run_frames(core, 2); }
		iters++;
	}
	printf("STATEFUZZ gb seed %llu: %lu states, %lu loaded and ran, %lu refused: OK (no crash, no hang, no sanitizer report)\n", (unsigned long long)seed, iters, ok, refused);
	return 0;
}
