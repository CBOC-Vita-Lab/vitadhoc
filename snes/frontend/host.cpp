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
#include "fxinst.h"
#include "fxemu.h"
#include "host.h"
#include "s9x_mt.h"
#include <atomic>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>

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
static std::vector<uint8_t> g_srm_tail;   /* the save file's bytes past the cartridge RAM */
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
	g_srm_tail.clear();

	size_t sz = snes_host_sram_size();
	if (sz) {
		std::vector<uint8_t> sv;
		if (!g_srm_path.empty() && read_file(g_srm_path.c_str(), sv)) {
			size_t c = sv.size() < sz ? sv.size() : sz;
			memcpy(Memory.SRAM, sv.data(), c);
			/* Bytes past the cartridge's RAM (another emulator's padding or
			 * trailer, e.g. a 128 KiB file for 8 KiB) are written back after it,
			 * so the file never shrinks: the same rule as VitAdhoc's own saves. */
			if (sv.size() > sz) g_srm_tail.assign(sv.begin() + sz, sv.end());
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

int snes_host_has_superfx(void) { return g_loaded && Settings.SuperFX ? 1 : 0; }
void snes_host_set_superfx_clock(int percent) { Settings.SuperFXClockMultiplier = percent; }
void snes_host_superfx_timer(uint64_t (*now_us)(void)) { S9xSuperFXClock = (uint64 (*)(void)) now_us; }
uint64_t snes_host_superfx_take_us(void) { uint64_t t = S9xSuperFXTime; S9xSuperFXTime = 0; return t; }

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
	else if (!strcmp(name, "gsu_cycles")) Settings.DisableGSUCycleMode = v ? FALSE : TRUE;	/* 0 = the 1.63 flat instruction budget; takes effect at the next ROM load */
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

/* ---- save safety ---------------------------------------------------------
 * The rule (the same as the rest of VitAdhoc, see gpsp/vita/save_sync.c):
 *  - the first overwrite of an existing save in a session needs a backup that was
 *    read back whole (size and bytes). If there is none the save is left untouched,
 *    the real reason is kept (snes_host_save_error) and the write is refused; the
 *    caller tries again later because nothing was recorded as saved.
 *  - every write goes to <file>.tmp, is read back, and is renamed over the file.
 *    The old file is never truncated or deleted before the new one is whole.
 * Return codes of the save functions: 0 ok; 1 the temporary file could not be
 * created; 2 writing it failed (disk full, file size limit); 3 no verified backup
 * (the save was not touched); 4 the new file could not be put in place; 5 the new
 * file did not read back whole; 6 the existing save cannot be read. */
static std::atomic_flag g_err_lock = ATOMIC_FLAG_INIT;
static int g_err_code;
static char g_err_text[200];
static void (*g_log_fn)(const char *);

void snes_host_set_log(void (*fn)(const char *line)) { g_log_fn = fn; }

static void log_line(const char *s)
{
	if (g_log_fn) g_log_fn(s);
	else fprintf(stderr, "%s\n", s);
}

static void err_set(int code, const char *path, const char *what, int en)
{
	char line[320];
	if (code) snprintf(line, sizeof line, "%s: %s%s%s", what, path ? path : "", en ? " - " : "", en ? strerror(en) : "");
	else line[0] = 0;
	while (g_err_lock.test_and_set(std::memory_order_acquire)) s9xmt::cpu_relax();
	g_err_code = code;
	snprintf(g_err_text, sizeof g_err_text, "%s", line);
	g_err_lock.clear(std::memory_order_release);
	if (code) log_line((std::string("save FAILED, the existing file was not touched: ") + line).c_str());
}

int snes_host_save_error(char *text, size_t cap)
{
	while (g_err_lock.test_and_set(std::memory_order_acquire)) s9xmt::cpu_relax();
	int c = g_err_code;
	if (text && cap) snprintf(text, cap, "%s", g_err_text);
	g_err_lock.clear(std::memory_order_release);
	return c;
}

static uint64_t fnv64(const uint8_t *d, size_t n)
{
	uint64_t h = 1469598103934665603ull;
	for (size_t i = 0; i < n; i++) { h ^= d[i]; h *= 1099511628211ull; }
	return h;
}

/* Does the file hold exactly these bytes (size and content)? */
static bool file_equals(const char *p, const uint8_t *d, size_t n)
{
	if (n == 0) { struct stat st; return stat(p, &st) == 0 && st.st_size == 0; }
	std::vector<uint8_t> b;
	return read_file(p, b) && b.size() == n && memcmp(b.data(), d, n) == 0;
}

/* Does the file hold a shorter start of these bytes (an interrupted copy)? */
static bool file_is_prefix_of(const char *p, const uint8_t *d, size_t n)
{
	struct stat st;
	if (stat(p, &st) != 0) return false;
	if (st.st_size == 0) return true;
	std::vector<uint8_t> b;
	return read_file(p, b) && b.size() < n && memcmp(b.data(), d, b.size()) == 0;
}

/* Write a whole file (not atomic by itself: callers write a .tmp). 0 ok, 1 could not be created,
 * 2 writing failed; errno is kept on failure. */
static int put_file(const char *p, const uint8_t *d, size_t n)
{
	errno = 0;
	FILE *f = fopen(p, "wb");
	if (!f) return 1;
	size_t put = n ? fwrite(d, 1, n, f) : 0;
	int fl = fflush(f);
	int en = errno;
#if !defined(__vita__)
	if (fl == 0 && put == n) fsync(fileno(f));
#endif
	int cl = fclose(f);
	if (put != n || fl != 0 || cl != 0) { errno = en ? en : EIO; remove(p); return 2; }
	return 0;
}

/* Put tmp in place of dest. rename over an existing file is not guaranteed on every
 * platform (the Vita's); then the old file is moved aside, never deleted first. */
static bool promote(const char *tmp, const char *dest)
{
	if (rename(tmp, dest) == 0) return true;
	int en = errno;
	std::string old = std::string(dest) + ".old";
	struct stat st;
	bool had = stat(dest, &st) == 0;
	if (!had) { errno = en; return false; }
	remove(old.c_str());
	if (rename(dest, old.c_str()) != 0) { errno = en; return false; }
	if (rename(tmp, dest) != 0) {
		en = errno;
		rename(old.c_str(), dest);
		errno = en;
		return false;
	}
	remove(old.c_str());
	return true;
}

/* A file `dst` that holds exactly d[0..n): written beside it, read back, put in place, read back. */
static bool write_verified(const std::string &dst, const uint8_t *d, size_t n, const char **step, int *en)
{
	std::string tmp = dst + ".tmp";
	*step = "writing the backup";
	if (put_file(tmp.c_str(), d, n)) { *en = errno; return false; }
	*step = "the backup did not read back whole";
	if (!file_equals(tmp.c_str(), d, n)) { *en = 0; remove(tmp.c_str()); return false; }
	*step = "putting the backup in place";
	if (!promote(tmp.c_str(), dst.c_str())) { *en = errno; remove(tmp.c_str()); return false; }
	*step = "the backup did not read back whole";
	if (!file_equals(dst.c_str(), d, n)) { *en = 0; return false; }
	return true;
}

static void make_dirs(const std::string &dir)
{
	for (size_t i = 1; i <= dir.size(); i++)
		if (i == dir.size() || dir[i] == '/') {
			std::string d = dir.substr(0, i);
			if (!d.empty() && d.back() != ':') mkdir(d.c_str(), 0777);
		}
}

/* A backup of `old` (the save as it is now) that was read back whole: in the backups
 * folder (one file per distinct content, never replacing a file that holds something
 * else), or, without a folder (the twin), <save>.bak. true = it exists. */
static bool backup_verified(const char *path, const std::vector<uint8_t> &old, std::string &dstout, const char **step, int *en)
{
	*en = 0;
	*step = "backup";
	if (old.empty()) return true;               /* nothing in it to lose */
	if (g_backup_dir.empty()) {
		std::string bak = std::string(path) + ".bak";
		dstout = bak;
		if (file_equals(bak.c_str(), old.data(), old.size())) return true;
		return write_verified(bak, old.data(), old.size(), step, en);
	}
	make_dirs(g_backup_dir);
	const char *base = strrchr(path, '/');
	base = base ? base + 1 : path;
	uint64_t h = fnv64(old.data(), old.size());
	for (int k = 0; k < 10; k++) {
		char name[64];
		if (k) snprintf(name, sizeof name, ".%08x.%d.bak", (unsigned) (h ^ (h >> 32)), k);
		else snprintf(name, sizeof name, ".%08x.bak", (unsigned) (h ^ (h >> 32)));
		std::string dst = g_backup_dir + "/" + base + name;
		dstout = dst;
		struct stat st;
		if (stat(dst.c_str(), &st) != 0) return write_verified(dst, old.data(), old.size(), step, en);
		if (file_equals(dst.c_str(), old.data(), old.size())) return true;      /* already there, and whole */
		if (file_is_prefix_of(dst.c_str(), old.data(), old.size()))            /* an interrupted copy of this very file */
			return write_verified(dst, old.data(), old.size(), step, en);
		/* another file with the same name: leave it, try the next name */
	}
	*step = "no free backup name";
	return false;
}

void snes_host_set_backup_dir(const char *dir) { g_backup_dir = dir ? dir : ""; }

static int write_atomic(const char *path, const void *data, size_t n, bool keep_bak, bool *bak_done)
{
	const uint8_t *d = (const uint8_t *) data;
	/* only battery saves (keep_bak) report through snes_host_save_error; a save state failing says so by its return code */
	auto E = [&](int code, const char *p, const char *what, int en) { if (keep_bak) err_set(code, p, what, en); else if (code) { char l[300]; snprintf(l, sizeof l, "state write failed: %s: %s %s", what, p ? p : "", en ? strerror(en) : ""); log_line(l); } };
	std::string tmp = std::string(path) + ".tmp";
	struct stat st;
	bool exists = stat(path, &st) == 0;
	if (exists && keep_bak && bak_done && !*bak_done) {
		std::vector<uint8_t> old;
		if (st.st_size > 0 && !read_file(path, old)) {
			E(6, path, "cannot read the existing save to back it up", errno);
			return 6;
		}
		std::string dst;
		const char *step = "";
		int en = 0;
		if (!backup_verified(path, old, dst, &step, &en)) {
			E(3, dst.empty() ? path : dst.c_str(), step, en);
			return 3;
		}
		char line[200];
		snprintf(line, sizeof line, "save backup verified: %zu bytes fnv=%016llx", old.size(), (unsigned long long) fnv64(old.data(), old.size()));
		log_line(line);
		*bak_done = true;
	}
	if (int pf = put_file(tmp.c_str(), d, n)) {
		int en = errno;
		E(pf, tmp.c_str(), pf == 1 ? "cannot create the temporary file" : "cannot write the temporary file", en);
		return pf;
	}
	if (!file_equals(tmp.c_str(), d, n)) {
		remove(tmp.c_str());
		E(5, tmp.c_str(), "the new file did not read back whole", 0);
		return 5;
	}
	if (!promote(tmp.c_str(), path)) {
		int en = errno;
		remove(tmp.c_str());
		E(4, path, "cannot put the new file in place", en);
		return 4;
	}
	if (!file_equals(path, d, n)) {
		E(5, path, "the saved file did not read back whole", 0);
		return 5;
	}
	if (!exists && keep_bak && bak_done) *bak_done = true;   /* a save created in this session needs no backup */
	E(0, NULL, NULL, 0);
	return 0;
}

int snes_host_sram_save(void)
{
	size_t sz = snes_host_sram_size();
	if (!sz || g_srm_path.empty()) return 0;
	std::vector<uint8_t> all(Memory.SRAM, Memory.SRAM + sz);
	all.insert(all.end(), g_srm_tail.begin(), g_srm_tail.end());
	int r = write_atomic(g_srm_path.c_str(), all.data(), all.size(), true, &g_bak_done);
	if (r == 0) g_sram_saved.assign(Memory.SRAM, Memory.SRAM + sz);
	return r;
}

size_t snes_host_sram_take(uint8_t *out, size_t cap)
{
	size_t sz = snes_host_sram_size();
	if (!sz || sz > cap || g_srm_path.empty()) return 0;
	memcpy(out, Memory.SRAM, sz);
	g_sram_saved.assign(out, out + sz);
	return sz;
}

int snes_host_sram_write(const uint8_t *data, size_t n)
{
	if (!n || g_srm_path.empty()) return 0;
	std::vector<uint8_t> all(data, data + n);
	all.insert(all.end(), g_srm_tail.begin(), g_srm_tail.end());
	return write_atomic(g_srm_path.c_str(), all.data(), all.size(), true, &g_bak_done);
}

void snes_host_sram_forget(void) { g_sram_saved.clear(); }

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
