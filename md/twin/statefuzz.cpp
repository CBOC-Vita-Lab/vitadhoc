// Fuzz harness for the Sega core's state loader (md/frontend/host.c state_load_from + Genesis Plus GX state.c).
// A real state is damaged the way a hostile peer would (payload bytes, the CRC trailer computed again), loaded and run;
// build with -fsanitize=address,undefined (see md/twin/Makefile target statefuzz).  MIT licence.
//   statefuzz ROM CASEDIR [--seconds N] [--seed N] [--frames N] [--system md|sms|gg|sg] [--case FILE]...
// Without --case: mutation until the time budget is spent; exit 0 = no crash, no hang, no sanitizer report.
// With --case: every file is loaded as it is (the saved crash / hang states, the regression set) and run.
#include "host.h"
#include "../../tools/statefuzz/sf_common.h"
#include <vector>
#include <string>

static void run_frames(int n, sf_rng *r)
{
	static int16_t abuf[4800 * 2];
	for (int f = 0; f < n; f++)
	{
		md_host_set_pad(0, sf_rand(r) & 0xfff); md_host_set_pad(1, sf_rand(r) & 0xfff);
		md_host_run_frame();
		md_host_audio_take(abuf, 4800);
		int w, h, p; md_host_video(&w, &h, &p);
		sf_beat();
	}
}
int main(int argc, char **argv)
{
	if (argc < 3) { fprintf(stderr, "usage: statefuzz ROM CASEDIR [--seconds N] [--seed N] [--frames N] [--system md|sms|gg|sg] [--case FILE]...\n"); return 2; }
	const char *rom = argv[1], *dir = argv[2]; double secs = 20; uint64_t seed = 1; int frames = 30, system = 0, warm = 300;
	std::vector<const char *> cases; bool proof = false;
	for (int i = 3; i < argc; i++)
	{
		std::string a = argv[i];
		if (a == "--seconds" && i + 1 < argc) secs = atof(argv[++i]);
		else if (a == "--seed" && i + 1 < argc) seed = strtoull(argv[++i], 0, 10);
		else if (a == "--frames" && i + 1 < argc) frames = atoi(argv[++i]);
		else if (a == "--warm" && i + 1 < argc) warm = atoi(argv[++i]);
		else if (a == "--system" && i + 1 < argc) { std::string r = argv[++i]; system = r == "md" ? MD_SYS_MD : r == "sms" ? MD_SYS_SMS : r == "gg" ? MD_SYS_GG : r == "sg" ? MD_SYS_SG : 0; }
		else if (a == "--case" && i + 1 < argc) cases.push_back(argv[++i]);
		else if (a == "--base-out" && i + 1 < argc) sf_base_out = argv[++i];
		else if (a == "--base" && i + 1 < argc) sf_base_idx = (size_t)atoi(argv[++i]);
		else if (a == "--proof") proof = true;
	}
	std::vector<uint8_t> rv = sf_read_file(rom);
	if (rv.empty()) { fprintf(stderr, "cannot read %s\n", rom); return 2; }
	char tag[64]; snprintf(tag, sizeof tag, "md%llu", (unsigned long long)seed);
	sf_init(dir, tag, 20);
	if (md_host_init()) return 2;
	char err[256] = "";
	if (md_host_load_mem(rv.data(), rv.size(), "fuzz", system, err, sizeof err)) { fprintf(stderr, "load: %s\n", err); return 2; }
	md_host_set_render(1);
	sf_rng rng; sf_seed(&rng, seed);
	if (proof)
	{
		// battery RAM, EEPROM and flash are not part of a state (a load never takes a save back): the proof carries them beside the state,
		// as the game's own save would be, and puts them back before the load. So they are compared too.
		int f = sf_proof("md", [&]() {
				std::vector<uint8_t> b(md_host_state_size()); md_host_state_save_mem(b.data(), b.size());
				std::vector<uint8_t> sr(0x10000); size_t n = md_host_sram_read(sr.data(), sr.size());
				b.insert(b.end(), sr.begin(), sr.begin() + n);
				for (int k = 0; k < 4; k++) b.push_back((uint8_t)(n >> (8 * k)));
				return b; },
			[&](const std::vector<uint8_t> &b) {
				size_t n = b[b.size() - 4] | (b[b.size() - 3] << 8) | (b[b.size() - 2] << 16) | ((size_t)b[b.size() - 1] << 24);
				size_t sn = b.size() - 4 - n;
				md_host_sram_write(b.data() + sn, n);
				return md_host_state_load_mem(b.data(), sn); },
			[&](int n, sf_rng *r) { run_frames(n, r); }, seed);
		printf(f ? "STATEFUZZ proof FAILED (%d)\n" : "STATEFUZZ proof OK: every state loads and runs on exactly as the uninterrupted machine\n", f);
		return f ? 1 : 0;
	}
	run_frames(warm, &rng);
	size_t n = md_host_state_size();
	std::vector<uint8_t> base(n);
	if (md_host_state_save_mem(base.data(), n) != n) { fprintf(stderr, "save failed\n"); return 2; }
	// two more bases a little later: the state differs (sound, VDP) from frame to frame
	std::vector<std::vector<uint8_t>> bases; bases.push_back(base);
	for (int k = 0; k < 2; k++) { run_frames(97, &rng); std::vector<uint8_t> b(n); md_host_state_save_mem(b.data(), n); bases.push_back(b); }
	unsigned long ok = 0, refused = 0, iters = 0;
	// the genuine states must load and run
	for (auto &b : bases)
	{
		sf_arm(b.data(), b.size(), 0);
		if (md_host_state_load_mem(b.data(), b.size()) != 0) { fprintf(stderr, "STATEFUZZ FAIL: a genuine state is refused\n"); return 1; }
		run_frames(frames, &rng);
	}
	sf_base_dump(bases);
	if (!cases.empty())
	{
		for (const char *c : cases)
		{
			std::vector<uint8_t> b = sf_read_case(c, bases[sf_base_idx < bases.size() ? sf_base_idx : 0]);
			sf_arm(b.data(), b.size(), iters++);
			int rc = md_host_state_load_mem(b.data(), b.size());
			if (rc == 0) { run_frames(frames, &rng); ok++; } else refused++;
			printf("case %s: %s\n", c, rc == 0 ? "loaded and ran" : "refused");
		}
		printf("STATEFUZZ cases %lu loaded, %lu refused: OK\n", ok, refused);
		return 0;
	}
	std::vector<sf_range> hot;           // everything but the big RAM blocks: registers, chip contexts, cartridge, the carry block
	struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
	for (;;)
	{
		struct timespec t1; clock_gettime(CLOCK_MONOTONIC, &t1);
		if ((t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9 > secs) break;
		std::vector<uint8_t> b = bases[sf_below(&rng, (uint32_t)bases.size())];
		uint32_t used = b[12] | (b[13] << 8) | (b[14] << 16) | ((uint32_t)b[15] << 24);
		hot.clear();
		hot.push_back({ 16 + 0x12000, 0x40 });                           // Genesis: Z80 bus state, bank register, IO registers
		hot.push_back({ 16 + 0x12020 + 0x400 + 0x10000, 0x300 });        // Genesis: CRAM, VSRAM, VDP registers and the chips after them
		hot.push_back({ 16 + used > 0x4000 ? 16 + used - 0x4000 : 16, 0x4000 });
		hot.push_back({ 16 + used, b.size() - 4 - 16 - used });          // the carry block
		sf_mutate(&rng, b.data(), b.size(), 16, b.size() - 4, hot);
		if (sf_below(&rng, 40) == 0) sf_mutate(&rng, b.data(), b.size(), 4, 16, std::vector<sf_range>());   // the header too
		uint32_t c = sf_crc32(b.data(), b.size() - 4);
		b[b.size() - 4] = (uint8_t)c; b[b.size() - 3] = (uint8_t)(c >> 8); b[b.size() - 2] = (uint8_t)(c >> 16); b[b.size() - 1] = (uint8_t)(c >> 24);
		sf_arm(b.data(), b.size(), iters);
		int rc = md_host_state_load_mem(b.data(), b.size());
		if (rc == 0)
		{
			ok++; run_frames(frames, &rng);
			// whatever the machine now is, saving it and loading that again must work (a joiner re-saves; rewind keeps states)
			std::vector<uint8_t> s(n);
			if (md_host_state_save_mem(s.data(), n) == n) { md_host_state_load_mem(s.data(), n); run_frames(2, &rng); }
		}
		else refused++;
		iters++;
	}
	printf("STATEFUZZ md seed %llu: %lu states, %lu loaded and ran, %lu refused: OK (no crash, no hang, no sanitizer report)\n", (unsigned long long)seed, iters, ok, refused);
	return 0;
}
