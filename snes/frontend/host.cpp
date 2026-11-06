/*
 * SNES host glue for Snes9x. See host.h.
 *
 * Copyright CBOC-Vita. MIT licence (see snes/frontend/LICENSE).
 * Links against Snes9x (snes/core), which keeps its own licence; this file
 * only calls its public functions and supplies the callbacks it expects.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

#include "snes9x.h"
#include "memmap.h"
#include "apu/apu.h"
#include "gfx.h"
#include "ppu.h"
#include "snapshot.h"
#include "controls.h"
#include "cheats.h"
#include "display.h"
#include "conffile.h"
#include "fscompat.h"
#include "s9x_prof.h"
#include "host.h"
#include "s9x_mt.h"
#include <atomic>

#define BTN_PAD(port, btn) (((port) << 4) | (btn))

static bool g_inited, g_loaded;
static std::string g_srm_path;
static std::string g_title;
static bool g_audio_on = true;
static bool g_frame_done;
static int g_vw = 256, g_vh = 224;
static std::vector<int16_t> g_audio_ring;
static size_t g_ar_r, g_ar_w, g_ar_n; /* int16 units, ring */
static std::vector<uint8_t> g_sram_saved; /* copy of what is on disk (or at load) */
static bool g_bak_done;
static std::string g_backup_dir; /* optional: extra home for the first copy of a player's save */
static int g_multitap;
static int g_want_ppu_thread;
static int g_want_apu_thread;   /* runtime switch: 1 = APU on its own thread (needs a loaded ROM) */
static std::atomic_flag g_ring_lock = ATOMIC_FLAG_INIT; /* the ring is filled by the APU thread when it is on */
struct RingLock {
	RingLock() { while (g_ring_lock.test_and_set(std::memory_order_acquire)) s9xmt::cpu_relax(); }
	~RingLock() { g_ring_lock.clear(std::memory_order_release); }
};

/* ---- Snes9x callbacks -------------------------------------------------- */

void S9xParsePortConfig(ConfigFile &, int) {}
const char *S9xStringInput(const char *in) { return in; }
std::string S9xGetDirectory(s9x_getdirtype) { return std::string("."); }
void S9xInitInputDevices() {}
void S9xHandlePortCommand(s9xcommand_t, short, short) {}
bool S9xPollButton(unsigned int, bool *) { return false; }
void S9xToggleSoundChannel(int) {}
std::string S9xGetFilenameInc(std::string, s9x_getdirtype) { return ""; }
const char *S9xBasename(const char *in) { return in; }
bool8 S9xInitUpdate() { return TRUE; }
void S9xExtraUsage() {}
bool8 S9xOpenSoundDevice() { return TRUE; }
bool S9xPollAxis(unsigned int, short *) { return FALSE; }
void S9xParseArg(char **, int &, int) {}
void S9xExit() {}
bool S9xPollPointer(unsigned int, short *, short *) { return false; }
void S9xMessage(int, int, const char *) {}
void S9xAutoSaveSRAM() {}

bool8 S9xOpenSnapshotFile(const char *filepath, bool8 read_only, STREAM *file)
{
	*file = OPEN_STREAM(filepath, read_only ? "rb" : "wb");
	return *file ? TRUE : FALSE;
}
void S9xCloseSnapshotFile(STREAM file) { CLOSE_STREAM(file); }

bool8 S9xDeinitUpdate(int width, int height)
{
	g_vw = width;
	g_vh = height;
	return TRUE;
}
bool8 S9xContinueUpdate(int width, int height)
{
	g_vw = width;
	g_vh = height;
	return TRUE;
}

static void ring_put(const int16_t *src, size_t n)
{
	RingLock lk;
	size_t cap = g_audio_ring.size();
	if (n > cap) { src += n - cap; n = cap; }
	/* drop the oldest audio when the frontend is not draining (never block) */
	while (g_ar_n + n > cap) { g_ar_r = (g_ar_r + 2) % cap; g_ar_n -= 2; }
	for (size_t i = 0; i < n; i++) {
		g_audio_ring[g_ar_w] = src[i];
		g_ar_w = (g_ar_w + 1) % cap;
	}
	g_ar_n += n;
}

/* The audio mix of one frame. Runs on the APU thread when that is on (it is
 * queued in the same stream as the DSP work, so it sees exactly the samples
 * the single-thread build sees at this point). */
static void mix_job(void *arg)
{
	static std::vector<int16_t> tmp;
	bool silent = arg != 0;
	if (silent) {
		S9xClearSamples();
	} else {
		size_t avail = S9xGetSampleCount();
		if (avail) {
			if (tmp.size() < avail) tmp.resize(avail);
			S9xMixSamples((uint8 *) &tmp[0], avail);
			ring_put(&tmp[0], avail);
		}
	}
}

/* Called by the core at the start of vertical blank: the frame is finished. */
void S9xSyncSpeed()
{
	S9XP_ENTER(FRONT);
	void *silent = (Settings.Mute || !g_audio_on) ? (void *) 1 : (void *) 0;
	if (S9xAPUMTActive()) S9xAPUMTPost(mix_job, silent);
	else mix_job(silent);
	g_frame_done = true;
	S9XP_LEAVE();
}

/* ---- Public API -------------------------------------------------------- */

static void map_pad(int port, const char *name)
{
	static const char *btn[12] = { "B", "Y", "Select", "Start", "Up", "Down", "Left", "Right", "A", "X", "L", "R" };
	for (int i = 0; i < 12; i++) {
		char cmd[64];
		snprintf(cmd, sizeof cmd, "Joypad%d %s", port + 1, btn[i]);
		S9xMapButton(BTN_PAD(port + 1, i), S9xGetCommandT(cmd), false);
	}
	(void) name;
}

int snes_host_init(void)
{
	if (g_inited) return 0;
	memset(&Settings, 0, sizeof(Settings));
	Settings.MouseMaster = FALSE;
	Settings.SuperScopeMaster = FALSE;
	Settings.JustifierMaster = FALSE;
	Settings.MultiPlayer5Master = FALSE;
	Settings.MacsRifleMaster = FALSE;
	Settings.FrameTimePAL = 20000;
	Settings.FrameTimeNTSC = 16667;
	Settings.SixteenBitSound = TRUE;
	Settings.Stereo = TRUE;
	Settings.SoundPlaybackRate = SNES_AUDIO_RATE;
	Settings.SoundInputRate = SNES_AUDIO_RATE;
	Settings.Transparency = TRUE;
	Settings.AutoDisplayMessages = FALSE;
	Settings.InitialInfoStringTimeout = 0;
	Settings.HDMATimingHack = 100;
	Settings.BlockInvalidVRAMAccessMaster = TRUE;
	Settings.SeparateEchoBuffer = FALSE;
	Settings.InterpolationMethod = DSP_INTERPOLATION_GAUSSIAN;
	Settings.SuperFXClockMultiplier = 100;
	Settings.MaxSpriteTilesPerLine = 34;
	Settings.OneClockCycle = 6;
	Settings.OneSlowClockCycle = 8;
	Settings.TwoClockCycles = 12;
	Settings.AutoSaveDelay = 0;
	Settings.DontSaveOopsSnapshot = TRUE;
	Settings.NoPatch = TRUE;
	Settings.ApplyCheats = FALSE;
	CPU.Flags = 0;

	if (!Memory.Init() || !S9xInitAPU()) {
		Memory.Deinit();
		S9xDeinitAPU();
		return 1;
	}
	S9xInitSound(32);
	S9xSetSoundMute(FALSE);
	S9xSetSamplesAvailableCallback(NULL, NULL);
	S9xGraphicsInit();
	S9xInitInputDevices();
	S9xSetController(0, CTL_JOYPAD, 0, 0, 0, 0);
	S9xSetController(1, CTL_JOYPAD, 1, 0, 0, 0);
	S9xUnmapAllControls();
	for (int i = 0; i < 5; i++) map_pad(i, "");

	g_audio_ring.assign(32040 * 2 * 2, 0); /* 2 seconds */
	g_ar_r = g_ar_w = g_ar_n = 0;
	g_inited = true;
	return 0;
}

void snes_host_shutdown(void)
{
	if (!g_inited) return;
	S9xAPUMTSet(false);
	S9xPPUMTSet(false);
	S9xDeinitAPU();
	Memory.Deinit();
	S9xGraphicsDeinit();
	S9xUnmapAllControls();
	g_inited = false;
	g_loaded = false;
}

static bool read_file(const char *path, std::vector<uint8_t> &out)
{
	FILE *f = fopen(path, "rb");
	if (!f) return false;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (n <= 0 || n > 8 * 1024 * 1024 + 4096) { fclose(f); return false; }
	out.resize(n);
	size_t got = fread(out.data(), 1, n, f);
	fclose(f);
	return got == (size_t) n;
}

size_t snes_host_sram_size(void)
{
	if (!g_loaded) return 0;
	size_t size = Memory.SRAMSize ? (size_t) (1 << (Memory.SRAMSize + 3)) * 128 : 0;
	if (size > 0x20000) size = 0x20000;
	/* On Super FX and SA-1 carts the header's RAM size is the chip's work RAM,
	 * which has no battery unless the type byte says so (low nibble 2, 5, 6).
	 * Treating it as a save would litter the ROM folder with junk .srm files. */
	if (Settings.SuperFX || Settings.SA1) {
		unsigned n = Memory.ROMType & 0x0F;
		if (n != 2 && n != 5 && n != 6) return 0;
	}
	return size;
}
const uint8_t *snes_host_sram_data(void) { return Memory.SRAM; }

int snes_host_load_rom(const char *rom_path, const char *srm_path, char *err, size_t errlen)
{
	if (!g_inited) { snprintf(err, errlen, "not initialised"); return 1; }
	std::vector<uint8_t> rom;
	if (!read_file(rom_path, rom)) { snprintf(err, errlen, "Cannot read the ROM file"); return 2; }
	const uint8_t *p = rom.data();
	size_t n = rom.size();
	if (n - (n / 0x2000) * 0x2000 == 512) { p += 512; n -= 512; } /* copier header */

	const char *base = strrchr(rom_path, '/');
	base = base ? base + 1 : rom_path;
	g_loaded = false;
	S9xAPUMTSet(false);
	S9xPPUMTSet(false);
	if (!Memory.LoadROMMem(p, (uint32) n, base)) { snprintf(err, errlen, "Not a valid SNES ROM"); return 3; }
	g_loaded = true;
	g_title = Memory.ROMName;
	g_srm_path = srm_path ? srm_path : "";
	g_bak_done = false;
	g_sram_saved.clear();

	size_t sz = snes_host_sram_size();
	if (sz) {
		std::vector<uint8_t> sv;
		if (!g_srm_path.empty() && read_file(g_srm_path.c_str(), sv)) {
			size_t c = sv.size() < sz ? sv.size() : sz;
			memcpy(Memory.SRAM, sv.data(), c);
			/* A file of a different size is kept untouched until the player's data
			 * would change; the backup rule in snes_host_sram_save then protects it. */
		}
		g_sram_saved.assign(Memory.SRAM, Memory.SRAM + sz);
	}
	/* LoadROMMem has already reset the machine; the game has not run yet, so the
	 * battery RAM copied in above is what its first instruction sees. */
	if (g_want_apu_thread) S9xAPUMTSet(true);
	if (g_want_ppu_thread) S9xPPUMTSet(true);
	return 0;
}

void snes_host_unload(void) { S9xAPUMTSet(false); S9xPPUMTSet(false); g_loaded = false; }

void snes_host_set_multitap(int on)
{
	if (!g_inited) return;
	S9xAPUMTSync();
	S9xPPUMTSync();
	if (on) {
		Settings.MultiPlayer5Master = TRUE;
		S9xSetController(1, CTL_MP5, 1, 2, 3, 4); /* pads 2..5 on the multitap in port 2 */
	} else {
		S9xSetController(1, CTL_JOYPAD, 1, 0, 0, 0);
	}
	g_multitap = on ? 1 : 0;
}
int snes_host_multitap(void) { return g_multitap; }

void snes_host_reset(void) { if (g_loaded) S9xSoftReset(); }

void snes_host_set_pad(int port, unsigned mask)
{
	if (port < 0 || port > 4) return;
	for (int i = 0; i < 12; i++)
		S9xReportButton(BTN_PAD(port + 1, i), (mask >> i) & 1);
}

void snes_host_run_frame(void)
{
	if (!g_loaded) return;
	g_frame_done = false;
	S9xMainLoop();
}

const uint16_t *snes_host_video(int *w, int *h, int *pitch)
{
	if (S9xPPUMTActive()) {
		int pw = g_vw, ph = g_vh;
		const uint16_t *pic = S9xPPUMTVideo(&pw, &ph);
		if (pic) {
			if (w) *w = pw;
			if (h) *h = ph;
			if (pitch) *pitch = (int) GFX.Pitch;
			return pic;
		}
	}
	if (w) *w = g_vw;
	if (h) *h = g_vh;
	if (pitch) *pitch = (int) GFX.Pitch;
	return GFX.Screen;
}

const uint16_t *snes_host_video_nowait(int *w, int *h, int *pitch, unsigned *serial)
{
	if (S9xPPUMTActive()) {
		int pw = g_vw, ph = g_vh;
		unsigned sr = 0;
		const uint16_t *pic = S9xPPUMTVideoNoWait(&pw, &ph, &sr);
		*serial = sr;
		if (!pic) return NULL;
		*w = pw; *h = ph; *pitch = (int) MAX_SNES_WIDTH * 2;
		return pic;
	}
	static unsigned n;
	*w = g_vw; *h = g_vh; *pitch = (int) GFX.Pitch;
	*serial = ++n;
	return GFX.Screen;
}

void snes_host_thread_config(int apu_core, int apu_prio, int ppu_core, int ppu_prio, int spin)
{
	S9xAPUMTConfig(apu_core, apu_prio);
	S9xPPUMTConfig(ppu_core, ppu_prio);
	S9xMTSetSpin(spin);
}

int snes_host_audio_take(int16_t *dst, int max_frames)
{
	RingLock lk;
	size_t want = (size_t) max_frames * 2;
	if (want > g_ar_n) want = g_ar_n & ~(size_t) 1;
	size_t cap = g_audio_ring.size();
	for (size_t i = 0; i < want; i++) {
		dst[i] = g_audio_ring[g_ar_r];
		g_ar_r = (g_ar_r + 1) % cap;
	}
	g_ar_n -= want;
	return (int) (want / 2);
}

int snes_host_set_option(const char *name, int v)
{
	if (!strcmp(name, "apu_thread")) {
		g_want_apu_thread = v ? 1 : 0;
		if (g_loaded) S9xAPUMTSet(v != 0);
		return 0;
	}
	if (!strcmp(name, "ppu_thread")) {
		g_want_ppu_thread = v ? 1 : 0;
		if (g_loaded) S9xPPUMTSet(v != 0);
		return 0;
	}
	if (!strcmp(name, "multitap")) { snes_host_set_multitap(v); return 0; }
	S9xAPUMTSync(); /* the APU thread reads Settings (interpolation, ...) */
	S9xPPUMTSync();
	if (!strcmp(name, "block_vram")) Settings.BlockInvalidVRAMAccessMaster = v ? TRUE : FALSE;
	else if (!strcmp(name, "sprite_tiles")) Settings.MaxSpriteTilesPerLine = v;
	else if (!strcmp(name, "interp"))
		Settings.InterpolationMethod = v == 0 ? DSP_INTERPOLATION_NONE : v == 1 ? DSP_INTERPOLATION_LINEAR
		    : v == 3 ? DSP_INTERPOLATION_CUBIC : v == 4 ? DSP_INTERPOLATION_SINC : DSP_INTERPOLATION_GAUSSIAN;
	else if (!strcmp(name, "superfx_clock")) Settings.SuperFXClockMultiplier = v;
	else if (!strcmp(name, "transparency")) Settings.Transparency = v ? TRUE : FALSE;
	else if (!strcmp(name, "echo_buffer")) Settings.SeparateEchoBuffer = v ? TRUE : FALSE;
	else return 1;
	return 0;
}

void snes_host_sync(void) { S9xAPUMTSync(); S9xPPUMTSync(); }
int snes_host_threads_active(void) { return (S9xAPUMTActive() ? 1 : 0) | (S9xPPUMTActive() ? 2 : 0); }
void snes_host_thread_stats(snes_thread_stats *st)
{
	memset(st, 0, sizeof *st);
	S9xAPUMTStats(&st->apu_busy_ns, &st->apu_cmds, &st->apu_helped, &st->apu_waits, &st->apu_wait_ns);
	S9xPPUMTStats(&st->ppu_busy_ns, &st->ppu_batches, &st->ppu_waits, &st->ppu_wait_ns);
}

void snes_host_set_audio_enabled(int on) { g_audio_on = on != 0; }
int snes_host_is_pal(void) { return Settings.PAL ? 1 : 0; }
void snes_host_set_render(int on) { IPPU.RenderThisFrame = on ? TRUE : FALSE; }
const char *snes_host_rom_title(void) { return g_title.c_str(); }

const char *snes_host_rom_info(void)
{
	static char buf[200];
	snprintf(buf, sizeof buf, "rom=%uK type=%02x map=%s%s%s%s%s%s%s sram=%uB %s %s",
	    (unsigned) (Memory.CalculatedSize / 1024), (unsigned) Memory.ROMType,
	    Memory.HiROM ? "HiROM" : "LoROM", Settings.SuperFX ? " SuperFX" : "", Settings.SA1 ? " SA-1" : "",
	    Settings.DSP ? " DSP" : "", Settings.C4 ? " C4" : "", Settings.SDD1 ? " S-DD1" : "",
	    (Settings.SPC7110 || Settings.SETA) ? " SPC7110/SETA" : "", (unsigned) snes_host_sram_size(),
	    Settings.PAL ? "PAL" : "NTSC", Memory.ROMName);
	return buf;
}

int snes_host_sram_dirty(void)
{
	size_t sz = snes_host_sram_size();
	if (!sz) return 0;
	return g_sram_saved.size() != sz || memcmp(g_sram_saved.data(), Memory.SRAM, sz) != 0;
}

/* Copy an existing save into the backup folder (once per distinct content, never
 * overwriting an earlier backup), so a .srm that another emulator wrote is safe
 * even if our .bak is replaced later. Best effort: a failure does not stop the save. */
static void backup_copy(const char *path, const std::vector<uint8_t> &old)
{
	if (g_backup_dir.empty() || old.empty()) return;
	uint64_t h = 1469598103934665603ull;
	for (uint8_t b : old) { h ^= b; h *= 1099511628211ull; }
	const char *base = strrchr(path, '/');
	base = base ? base + 1 : path;
	char name[64];
	snprintf(name, sizeof name, ".%08x.bak", (unsigned) (h ^ (h >> 32)));
	std::string dst = g_backup_dir + "/" + base + name;
	FILE *t = fopen(dst.c_str(), "rb");
	if (t) { fclose(t); return; }
	std::string tmp = dst + ".tmp";
	FILE *f = fopen(tmp.c_str(), "wb");
	if (!f) return;
	size_t put = fwrite(old.data(), 1, old.size(), f);
	int cl = fclose(f);
	if (put != old.size() || cl != 0 || rename(tmp.c_str(), dst.c_str()) != 0) remove(tmp.c_str());
}

void snes_host_set_backup_dir(const char *dir) { g_backup_dir = dir ? dir : ""; }

static int write_atomic(const char *path, const void *data, size_t n, bool keep_bak, bool *bak_done)
{
	std::string tmp = std::string(path) + ".tmp";
	std::string bak = std::string(path) + ".bak";
	FILE *f = fopen(tmp.c_str(), "wb");
	if (!f) return 1;
	size_t put = fwrite(data, 1, n, f);
	int cl = fclose(f);
	if (put != n || cl != 0) { remove(tmp.c_str()); return 2; }
	if (keep_bak && bak_done && !*bak_done) {
		/* Keep the player's previous file once per session before the first overwrite. */
		std::vector<uint8_t> old;
		if (read_file(path, old)) {
			backup_copy(path, old);
			std::string bt = bak + ".tmp";
			FILE *b = fopen(bt.c_str(), "wb");
			if (!b) { remove(tmp.c_str()); return 3; }
			size_t bp = fwrite(old.data(), 1, old.size(), b);
			int bc = fclose(b);
			if (bp != old.size() || bc != 0) { remove(bt.c_str()); remove(tmp.c_str()); return 3; }
			remove(bak.c_str());
			if (rename(bt.c_str(), bak.c_str()) != 0) { remove(tmp.c_str()); return 3; }
		}
		*bak_done = true;
	}
	remove(path); /* rename over an existing file is not guaranteed everywhere */
	if (rename(tmp.c_str(), path) != 0) return 4;
	return 0;
}

int snes_host_sram_save(void)
{
	size_t sz = snes_host_sram_size();
	if (!sz || g_srm_path.empty()) return 0;
	int r = write_atomic(g_srm_path.c_str(), Memory.SRAM, sz, true, &g_bak_done);
	if (r == 0) g_sram_saved.assign(Memory.SRAM, Memory.SRAM + sz);
	return r;
}

int snes_host_state_save(const char *path)
{
	if (!g_loaded) return 1;
	size_t n = S9xFreezeSize();
	std::vector<uint8_t> buf(n);
	if (!S9xFreezeGameMem(buf.data(), (uint32) n)) return 2;
	bool dummy = false;
	return write_atomic(path, buf.data(), n, false, &dummy);
}

int snes_host_state_load(const char *path)
{
	if (!g_loaded) return 1;
	std::vector<uint8_t> buf;
	if (!read_file(path, buf)) return 2;
	return S9xUnfreezeGameMem(buf.data(), (uint32) buf.size()) == SUCCESS ? 0 : 3;
}
