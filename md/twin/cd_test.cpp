/* Sega CD disc reader and BIOS checks over the host API (md/frontend/host.h), on the synthetic discs of tools/mkscd.py and its test-only stand-in BIOS.
 *   cd_test DISCS_DIR STANDIN_BIOS
 * Copyright CBOC-Vita. MIT licence. */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <pthread.h>
#include <semaphore.h>
#include <sched.h>
#include <time.h>
#include <unistd.h>
#include "host.h"
#include "thread_ops.h"

/* threads for the disc read-ahead helper (the Vita hands over sceKernel threads instead) */
static pthread_t g_wthr[MD_WORKERS];
struct SpawnArg { void (*fn)(void *); void *arg; };
static SpawnArg g_sarg[MD_WORKERS];
static void *thr_main(void *p) { SpawnArg *a = (SpawnArg *)p; a->fn(a->arg); return NULL; }
static void *t_sem_create(void *) { sem_t *s = new sem_t; sem_init(s, 0, 0); return s; }
static void t_sem_destroy(void *, void *s) { sem_destroy((sem_t *)s); delete (sem_t *)s; }
static void t_sem_post(void *, void *s) { sem_post((sem_t *)s); }
static void t_sem_wait(void *, void *s) { while (sem_wait((sem_t *)s) != 0) {} }
static int t_spawn(void *, int id, void (*fn)(void *), void *arg) { g_sarg[id].fn = fn; g_sarg[id].arg = arg; return pthread_create(&g_wthr[id], NULL, thr_main, &g_sarg[id]); }
static void t_wait_end(void *, int id) { pthread_join(g_wthr[id], NULL); }
static void t_yield(void *) { sched_yield(); }
static void t_nap(void *) { struct timespec ts = { 0, 50000 }; nanosleep(&ts, NULL); }
static unsigned long long t_now(void *) { timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (unsigned long long) ts.tv_sec * 1000000ull + (unsigned long long) ts.tv_nsec / 1000; }
static md_thread_ops g_tops = { NULL, t_sem_create, t_sem_destroy, t_sem_post, t_sem_wait, t_spawn, t_wait_end, t_yield, t_now, t_nap };

static int fails = 0, checks = 0;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL  %s:%d  ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static std::vector<uint8_t> slurp(const std::string &p)
{
	std::vector<uint8_t> v;
	FILE *f = fopen(p.c_str(), "rb");
	if (!f) return v;
	uint8_t b[65536]; size_t n;
	while ((n = fread(b, 1, sizeof b, f)) > 0) v.insert(v.end(), b, b + n);
	fclose(f);
	return v;
}

static const int DATA_SECTORS = 300;
static uint8_t expect_data(int n, int i) { return (uint8_t)((n * 7 + i * 13 + (i >> 8) * 3) & 0xFF); }
static void expect_audio(int track, int i, int16_t out[2])
{
	int l = ((i * 37 + track * 101) % 20000) - 10000;
	out[0] = (int16_t)l; out[1] = (int16_t)-l;
}

static std::string D;

static void check_data(const char *name, int lbas[], int n)
{
	for (int k = 0; k < n; k++) {
		uint8_t s[2048]; memset(s, 0xEE, sizeof s);
		CHECK(md_host_cd_test_data(lbas[k], s) == 0, "%s: data read %d", name, lbas[k]);
		int bad = 0;
		for (int i = 0; i < 2048; i++) if (lbas[k] != 0 && s[i] != expect_data(lbas[k], i)) bad++;
		if (lbas[k] == 0) CHECK(memcmp(s, "SEGADISCSYSTEM  ", 16) == 0 && memcmp(s + 0x100, "SEGA", 4) == 0, "%s: header sector", name);
		CHECK(bad == 0, "%s: sector %d has %d wrong bytes", name, lbas[k], bad);
	}
}

/* exact = the audio comes back sample for sample (raw files, WAV, CHD); otherwise only the shape (Ogg is lossy) */
static void check_audio(const char *name, int track, int first, bool exact)
{
	int16_t got[2], want[2];
	const int samples = 600;       /* reaches into the second sector */
	CHECK(md_host_cd_test_audio(track, first, samples, got) == 0, "%s: audio read", name);
	expect_audio(track, samples - 1, want);
	if (exact) CHECK(got[0] == want[0] && got[1] == want[1], "%s: track %d last sample %d,%d expected %d,%d", name, track, got[0], got[1], want[0], want[1]);
	else CHECK(abs(got[0] - want[0]) < 1500 && abs(got[1] - want[1]) < 1500, "%s: track %d last sample %d,%d near %d,%d", name, track, got[0], got[1], want[0], want[1]);
}

static void disc(const char *file, bool audio_exact, bool has_audio_files = true)
{
	char err[160] = "";
	int r = md_host_load_disc((D + "/" + file).c_str(), err, sizeof err);
	CHECK(r == 0, "%s: load failed %d (%s)", file, r, err);
	if (r) return;
	int s, e, t;
	CHECK(md_host_is_cd() == 1, "%s: is_cd", file);
	{ uint8_t h[512]; CHECK(md_host_cd_header(h, sizeof h) == 0 && memcmp(h, "SEGADISCSYSTEM  ", 16) == 0 && memcmp(h + 0x100, "SEGA", 4) == 0, "%s: header for RetroAchievements", file); } if (getenv("CDT_V")) printf("%s: %s\n", file, md_host_cd_info());
	int nt = md_host_cd_test_toc(1, &s, &e, &t);
	/* a lone data track gets 99 simulated audio tracks (the core does that: the BIOS of some games wants a table of contents with audio) */
	CHECK(has_audio_files ? nt == 3 : nt == 99, "%s: track count %d", file, nt);
	CHECK(t != 0 && s == 0, "%s: track 1 is data from 0 (start %d type %d)", file, s, t);
	int lbas[] = {0, 1, 5, 150, DATA_SECTORS - 1};
	check_data(file, lbas, 5);
	if (has_audio_files) {
		md_host_cd_test_toc(2, &s, &e, &t);
		int s2 = s;
		CHECK(t == 0 && s >= DATA_SECTORS && e == s + 200, "%s: track 2 %d..%d type %d", file, s, e, t);
		md_host_cd_test_toc(3, &s, &e, &t);
		CHECK(t == 0 && s >= s2 + 200 && e == s + 150, "%s: track 3 %d..%d type %d", file, s, e, t);
		check_audio(file, 2, s2, audio_exact);
		check_audio(file, 3, s, audio_exact);
	}
	md_host_unload();
}

int main(int argc, char **argv)
{
	if (argc < 3) { fprintf(stderr, "usage: cd_test DISCS_DIR STANDIN_BIOS\n"); return 2; }
	D = argv[1];
	std::vector<uint8_t> bios = slurp(argv[2]);
	CHECK(bios.size() == 131072, "stand-in BIOS is %zu bytes", bios.size());
	CHECK(md_host_init() == 0, "init");
	char err[160];

	/* BIOS validation */
	CHECK(md_host_cd_bios_add(bios.data(), bios.size(), 0) == 0, "the stand-in must not pass as a real BIOS");
	CHECK(md_host_cd_bios_count() == 0, "nothing stored after a refusal");
	std::vector<uint8_t> junk = slurp(D + "/randomdata.bin"); junk.resize(131072, 0x5A);
	CHECK(md_host_cd_bios_add(junk.data(), junk.size(), 0) == 0, "random data refused");
	CHECK(md_host_cd_bios_add(bios.data(), 1000, 1) == 0, "short data refused even forced");
	CHECK(md_host_cd_bios_name(bios.data(), bios.size()) == NULL, "the stand-in has no known name");

	/* no BIOS: a clear refusal */
	int r = md_host_load_disc((D + "/single.cue").c_str(), err, sizeof err);
	CHECK(r == 5, "without a BIOS the load must answer 5, got %d (%s)", r, err);
	CHECK(strstr(err, "BIOS") != NULL, "reason names the BIOS: '%s'", err);
	CHECK(!md_host_loaded(), "nothing loaded after a refusal");

	int reg = md_host_cd_bios_add(bios.data(), bios.size(), 1);
	CHECK(reg == MD_REGION_USA, "forced stand-in: region %d", reg);
	CHECK(md_host_cd_bios_count() == 1, "one region stored");

	/* probe */
	const char *good[] = {"single.cue", "multi.cue", "wavs.cue", "cooked.iso", "cooked.bin", "disc.chd"};
	for (auto f : good) { char why[80] = ""; CHECK(md_host_cd_probe((D + "/" + f).c_str(), why, sizeof why) == 1, "probe %s: %s", f, why); }
	const char *bad[] = {"notacue.cue", "empty.iso", "randomdata.bin"};
	for (auto f : bad) { char why[80] = ""; CHECK(md_host_cd_probe((D + "/" + f).c_str(), why, sizeof why) == 0, "probe %s must refuse", f); CHECK(why[0], "probe %s gives a reason", f); }
	r = md_host_load_disc((D + "/notacue.cue").c_str(), err, sizeof err); CHECK(r != 0 && !md_host_loaded(), "load of a bad cue refused (%d)", r);
	r = md_host_load_disc((D + "/empty.iso").c_str(), err, sizeof err); CHECK(r != 0 && !md_host_loaded(), "load of an empty iso refused (%d)", r);
	r = md_host_load_disc((D + "/randomdata.bin").c_str(), err, sizeof err); CHECK(r != 0 && !md_host_loaded(), "load of random data refused (%d)", r);

	/* the readers, byte for byte */
	disc("cooked.iso", true, false);
	disc("cooked.bin", true, false);
	disc("single.cue", true);
	disc("multi.cue", true);
	disc("wavs.cue", true);
	disc("disc.chd", true);
	/* B8: a BOM and tab separators, mixed-case extensions, long lines and paths, Windows separators, a device-style name, MOTOROLA audio */
	for (auto f : {"bomtab.cue", "Mixed.Cue", "longname.cue", "winpath.cue", "ux0:Dev.cue", "motorola.cue"}) disc(f, true);
	/* B7: compressed CHDs written by mkscd.py (LZMA, zlib, zstd, FLAC, a mix with stored hunks, 4 and 32 frame hunks, self references): every codec
	   libchdr has, on a data track and two audio tracks, read byte for byte */
	for (auto f : {"UPPER.CHD", "chd_lzma.chd", "chd_zlib.chd", "chd_zstd.chd", "chd_flac.chd", "chd_mixed.chd", "chd_multi.chd", "chd_big.chd"}) {
		CHECK(!slurp(D + "/" + f).empty(), "fixture %s was generated", f);
		disc(f, true);
	}
	/* self references: track 2 is 16 frames of silence (two identical hunks, stored once), track 3 the first 40 frames of the usual track 2 */
	{
		CHECK(!slurp(D + "/chd_self.chd").empty(), "fixture chd_self.chd was generated");
		r = md_host_load_disc((D + "/chd_self.chd").c_str(), err, sizeof err);
		CHECK(r == 0, "chd_self: load failed %d (%s)", r, err);
		if (r == 0) {
			int s, e, t, s2, s3, lbas[] = {0, 77, DATA_SECTORS - 1};
			CHECK(md_host_cd_test_toc(1, &s, &e, &t) == 3, "chd_self: three tracks");
			check_data("chd_self.chd", lbas, 3);
			md_host_cd_test_toc(2, &s2, &e, &t);
			md_host_cd_test_toc(3, &s3, &e, &t);
			int16_t got[2], want[2];
			CHECK(md_host_cd_test_audio(2, s2, 600, got) == 0 && got[0] == 0 && got[1] == 0, "chd_self: silence reads as silence (%d,%d)", got[0], got[1]);
			expect_audio(2, 599, want);
			CHECK(md_host_cd_test_audio(3, s3, 600, got) == 0 && got[0] == want[0] && got[1] == want[1], "chd_self: track 3 sample %d,%d expected %d,%d", got[0], got[1], want[0], want[1]);
			md_host_unload();
		}
	}
	/* the same discs again with the read-ahead thread, then with its stress mode: the answers are the same bytes */
	unsigned long timeouts = 0, mism = 0;
	for (int mode = 1; mode <= 2; mode++) {
		md_host_set_threads(&g_tops, MD_THREAD_DISC);
		md_host_cd_debug(1, mode == 2);
		for (auto f : {"single.cue", "multi.cue", "wavs.cue", "disc.chd", "chd_lzma.chd", "chd_zlib.chd", "chd_zstd.chd", "chd_flac.chd", "chd_mixed.chd", "chd_multi.chd", "chd_big.chd"}) {
			if (slurp(D + "/" + f).empty()) continue;
			disc(f, true);
			if (mode == 2 && std::string(f).find("chd") != std::string::npos) {      /* a long sequential read: the helper is ahead of it, and stalls now and then */
				char err[160] = "";
				if (md_host_load_disc((D + "/" + f).c_str(), err, sizeof err) == 0) {
					std::vector<int> all;
					for (int rep = 0; rep < 40; rep++) for (int l = 0; l < DATA_SECTORS; l++) all.push_back(l);
					check_data(f, all.data(), (int)all.size());
					unsigned long c[10]; md_host_cd_census(c); timeouts += c[9]; mism += c[8];
					if (getenv("CDT_V")) printf("%s long census reads=%lu hits=%lu misses=%lu waits=%lu timeouts=%lu\n", f, c[0], c[1], c[2], c[3], c[9]);
					md_host_unload();
				}
			}
			if (mode == 2) { unsigned long c[10]; md_host_cd_census(c); timeouts += c[9]; mism += c[8]; if (getenv("CDT_V")) printf("%s census reads=%lu hits=%lu misses=%lu waits=%lu timeouts=%lu\n", f, c[0], c[1], c[2], c[3], c[9]); }
		}
		md_host_cd_debug(0, 0);
		md_host_set_threads(NULL, 0);
	}
	/* the emulating thread's wait for the helper is bounded (about 2 ms): with the helper stalled for longer now and then, some waits are given up
	 * (the bytes are then read by the emulating thread), and the helper's late block never corrupts anything (no mismatch in the check mode) */
	CHECK(timeouts > 0 && mism == 0, "cdio stress: %lu waits given up, %lu mismatches", timeouts, mism);
	/* a helper stalled for LONGER than the close waits (500 ms against about 250 ms): the close of the disc (md_host_unload, also the first step of every disc
	 * swap) must come back in about the bound, count a close timeout, never write into or free what the helper is on (ASan / UBSan builds of this test are
	 * the check for that), and the next disc must load and read byte for byte; once the helper has left, the next session has its helper again */
	for (int pass = 0; pass < 2; pass++) {
		const char *first = pass == 0 ? "single.cue" : "chd_lzma.chd";     /* a raw stream, then a CHD (its decoder is the helper's) */
		const char *other = pass == 0 ? "multi.cue" : "disc.chd";
		for (int how = 0; how < 2; how++) {                                  /* 0: unload, 1: a swap to another disc without unloading first */
			char e2[160] = "";
			md_host_set_threads(&g_tops, MD_THREAD_DISC);                    /* (re-enables a helper that was given up and has left) */
			if (slurp(D + "/" + first).empty() || slurp(D + "/" + other).empty()) continue;
			r = md_host_load_disc((D + "/" + first).c_str(), e2, sizeof e2);
			CHECK(r == 0, "stall: load %s: %d %s", first, r, e2);
			if (r) continue;
			CHECK(md_host_cd_helper() == 1, "stall: the helper runs for %s (pass %d, how %d)", first, pass, how);
			md_host_cd_stall(500);
			int lb[] = {0, 20, 40, 60, 80, 100, 120, 140};     /* (a CHD hunk of a few sectors is cached by the core: spread the reads over hunks) */
			check_data(first, lb, 8);                                        /* the helper claims one of the blocks read ahead and stalls on it */
			usleep(30000);
			const unsigned long long t0 = t_now(NULL);
			if (how == 0) md_host_unload();
			else {
				r = md_host_load_disc((D + "/" + other).c_str(), e2, sizeof e2);
				CHECK(r == 0, "stall: swap to %s: %d %s", other, r, e2);
			}
			const unsigned long long ms = (t_now(NULL) - t0) / 1000;
			CHECK(ms < 450, "stall: the close waited %llu ms (bound about 250)", ms);
			if (how == 0) {
				unsigned long c[10]; md_host_cd_census(c);
				CHECK(c[9] >= 1, "stall: a close timeout is counted (%lu) (%s, ms %llu, blocks %lu)", c[9], first, ms, c[5]);
				disc(other, true);                                           /* the next disc loads and reads correctly (no helper yet) */
			} else if (!r) {
				int lbas[] = {0, 1, 5, 150, DATA_SECTORS - 1};
				check_data(other, lbas, 5);
				md_host_unload();
			}
			usleep(700000);                                                   /* the stalled block ends, the helper frees what was left to it and leaves */
		}
	}
	md_host_set_threads(&g_tops, MD_THREAD_DISC);
	disc("disc.chd", true);                                                    /* and after all that a session with a helper still reads right */
	md_host_set_threads(NULL, 0);
	if (!slurp(D + "/oggs.cue").empty()) disc("oggs.cue", false);
	else printf("note  oggs.cue not generated (no ffmpeg): Ogg reader not checked\n");

	/* region fallback: the disc says U, the only BIOS is Europe's -> loads, console region follows the BIOS */
	{
		md_host_cd_bios_clear();
		std::vector<uint8_t> eu = bios;
		for (int i = 0x1F0; i < 0x1F0 + 1; i++) eu[i] = 'E';
		CHECK(md_host_cd_bios_add(eu.data(), eu.size(), 1) == MD_REGION_EUROPE, "region E detected");
		r = md_host_load_disc((D + "/single.cue").c_str(), err, sizeof err);
		CHECK(r == 0, "fallback load: %d %s", r, err);
		if (!r) { CHECK(strstr(md_host_cd_info(), "Europe") || strstr(md_host_cd_info(), "EU") || strstr(md_host_cd_info(), "Mega"), "info: %s", md_host_cd_info()); md_host_unload(); }
	}

	/* B8: a sheet whose audio file is not there: the disc starts with the tracks before it and the notice says which one is missing */
	r = md_host_load_disc((D + "/missing.cue").c_str(), err, sizeof err);
	CHECK(r == 0, "an audio track that is missing does not stop the data track: %d %s", r, err);
	if (!r) {
		CHECK(md_host_cd_tracks() == 2, "the tracks before the missing one are there: %d", md_host_cd_tracks());
		CHECK(strstr(md_host_cd_notice(), "Track 3") != NULL, "the notice names track 3: '%s'", md_host_cd_notice());
		md_host_unload();
	}

	/* B1: the RAM cartridge is 512 KB unless the player says otherwise ("Off" is an empty slot); backup RAM: formatted at load, saved whole,
	 * dirty tracking, write back */
	r = md_host_load_disc((D + "/single.cue").c_str(), err, sizeof err);
	CHECK(r == 0, "load with the default RAM cartridge: %d %s", r, err);
	if (!r) { CHECK(md_host_sram_size() == 8192 + 524288, "the default save is the 8 KB backup RAM and a 512 KB cartridge: %zu", md_host_sram_size()); md_host_unload(); }
	md_host_set_option("ram_cart", 0);
	r = md_host_load_disc((D + "/single.cue").c_str(), err, sizeof err);
	CHECK(r == 0, "load for save test: %d %s", r, err);
	if (!r) {
		size_t cap = md_host_sram_size();
		CHECK(cap == 8192, "save size without a RAM cartridge is %zu", cap);
		std::vector<uint8_t> a(cap), b(cap);
		CHECK(md_host_sram_read(a.data(), cap) == cap, "sram read");
		int nz = 0; for (size_t i = 0; i < cap; i++) nz += a[i] != 0;
		CHECK(nz > 0, "backup RAM is formatted (not all zero)");
		a[0x100] ^= 0xFF;
		CHECK(md_host_sram_write(a.data(), cap) == cap, "sram write");
		md_host_sram_read(b.data(), cap);
		CHECK(b[0x100] == a[0x100], "written byte reads back");
		CHECK(md_host_sram_dirty() == 0, "written image counts as saved");
		a[0x101] ^= 0xFF; md_host_sram_write(a.data(), cap); md_host_sram_mark_unsaved();
		CHECK(md_host_sram_dirty() == 1, "marked unsaved");
		md_host_unload();
	}
	md_host_set_option("ram_cart", 4);
	r = md_host_load_disc((D + "/single.cue").c_str(), err, sizeof err);
	CHECK(r == 0, "load with RAM cartridge: %d %s", r, err);
	std::vector<uint8_t> saved128;
	if (!r) {
		CHECK(md_host_sram_size() == 8192 + 131072, "save size with a 128 KB cartridge: %zu", md_host_sram_size());
		/* B2: a game's saves in the cartridge, then the player changes the cartridge size: the data must come through every size */
		std::vector<uint8_t> a(md_host_sram_size());
		md_host_sram_read(a.data(), a.size());
		for (size_t i = 0; i < 4096; i++) a[8192 + i] = (uint8_t)(0xC3 ^ i);
		CHECK(md_host_sram_write(a.data(), a.size()) == a.size(), "128 KB cartridge image written");
		md_host_sram_read(a.data(), a.size()); saved128 = a;
		CHECK(a[8192] == 0xC3 && a[8193] == (uint8_t)(0xC3 ^ 1), "cartridge bytes read back");
		md_host_unload();
	}
	if (!saved128.empty()) {
		/* the file of the 128 KB run is loaded with no cartridge: the cartridge's image is kept, and goes back out with the next save */
		md_host_set_option("ram_cart", 0);
		r = md_host_load_disc((D + "/single.cue").c_str(), err, sizeof err);
		CHECK(r == 0, "load with no cartridge: %d %s", r, err);
		std::vector<uint8_t> off;
		if (!r) {
			CHECK(md_host_sram_write(saved128.data(), saved128.size()) > 0, "the 128 KB file is accepted with no cartridge fitted");
			CHECK(md_host_cd_kept_cartridges() == 1, "one cartridge image kept aside: %d", md_host_cd_kept_cartridges());
			off.resize(md_host_sram_size());
			CHECK(off.size() == saved128.size() + 8, "the file keeps its cartridge image and its mark: %zu vs %zu", off.size(), saved128.size() + 8);
			md_host_sram_read(off.data(), off.size());
			CHECK(off.size() >= saved128.size() && !memcmp(off.data() + 8192, saved128.data() + 8192, 131072), "the 128 KB image comes out byte for byte");
			md_host_unload();
		}
		/* ... and with a 256 KB one: that cartridge is new (formatted), the 128 KB image is still kept */
		md_host_set_option("ram_cart", 5);
		r = md_host_load_disc((D + "/single.cue").c_str(), err, sizeof err);
		CHECK(r == 0, "load with a 256 KB cartridge: %d %s", r, err);
		std::vector<uint8_t> big;
		if (!r && !off.empty()) {
			CHECK(md_host_sram_write(off.data(), off.size()) > 0, "the file written with no cartridge is accepted with 256 KB");
			big.resize(md_host_sram_size());
			CHECK(md_host_cd_kept_cartridges() == 1 && big.size() == 8192 + 262144 + 131072 + 8, "256 KB cartridge and the kept 128 KB image: %zu", big.size());
			md_host_sram_read(big.data(), big.size());
			CHECK(!memcmp(big.data() + 8192 + 262144, saved128.data() + 8192, 131072), "the kept 128 KB image follows the 256 KB one unchanged");
			md_host_unload();
		}
		/* back to 128 KB: the cartridge is the one the game saved to */
		md_host_set_option("ram_cart", 4);
		r = md_host_load_disc((D + "/single.cue").c_str(), err, sizeof err);
		CHECK(r == 0, "load with the 128 KB cartridge again: %d %s", r, err);
		if (!r && !big.empty()) {
			CHECK(md_host_sram_write(big.data(), big.size()) > 0, "the 256 KB file is accepted with 128 KB");
			std::vector<uint8_t> again(md_host_sram_size());
			md_host_sram_read(again.data(), again.size());
			CHECK(again.size() >= saved128.size() && !memcmp(again.data() + 8192, saved128.data() + 8192, 131072), "the game's 128 KB saves are the cartridge again");
			CHECK(again.size() == 8192 + 131072 + 262144 + 8, "and the 256 KB image is the one kept: %zu", again.size());
			md_host_unload();
		}
	}
	md_host_set_option("ram_cart", 0);

	/* RetroAchievements map and cheats on the running stand-in (its main program counts frames at $FFFF00, the sub CPU's counter is copied to $FFFF02) */
	r = md_host_load_disc((D + "/single.cue").c_str(), err, sizeof err);
	CHECK(r == 0, "load for RA test: %d %s", r, err);
	if (!r) {
		CHECK(md_host_ra_console() == 9, "RetroAchievements console id for Sega CD is 9, got %d", md_host_ra_console());
		for (int f = 0; f < 90; f++) md_host_run_frame();
		uint8_t w[4] = {0, 0, 0, 0}, w2[4] = {0, 0, 0, 0}, prg[16] = {0}, wr[16];
		CHECK(md_host_ra_read(0xFF00, w, 4) == 4, "work RAM read");
		CHECK((w[0] > 60 && w[0] < 100) || (w[1] > 60 && w[1] < 100), "frame counter visible in work RAM: %02X %02X", w[0], w[1]);
		for (int f = 0; f < 30; f++) md_host_run_frame();
		md_host_ra_read(0xFF00, w2, 4);
		CHECK(memcmp(w, w2, 4) != 0, "work RAM changes while the machine runs");
		CHECK(md_host_ra_read(0x10000, prg, 16) == 16, "PRG-RAM read");
		int nz = 0; for (int i = 0; i < 16; i++) nz += prg[i] != 0;
		CHECK(nz > 0, "the sub CPU's program is in PRG-RAM (the main CPU copied it there)");
		CHECK(md_host_ra_read(0x90000, wr, 16) == 16, "word RAM read");
		CHECK(md_host_ra_read(0xB0000, wr, 4) == 0 && md_host_ra_read(0x20000 + 0x100000, wr, 4) == 0, "nothing past the map");
		/* a cheat writes work RAM; a ROM patch is refused (a disc has no cartridge ROM) */
		CHECK(md_host_cheat_add("FFF000:0042") >= 1, "work RAM cheat accepted");
		md_host_run_frame();
		CHECK(md_host_peek(0xFFF001) == 0x42, "cheat value lands in work RAM (peek %02X)", md_host_peek(0xFFF001));
		CHECK(md_host_cheat_add("000200:4E71") == 0, "ROM patch refused on a disc");
		md_host_unload();
	}

	md_host_shutdown();
	printf("cd_test: %d checks, %d failed\n", checks, fails);
	return fails ? 1 : 0;
}
