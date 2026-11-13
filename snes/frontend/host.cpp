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
static void cheats_forget(void);
static void cheats_suspend(std::vector<int> &on);
static void cheats_resume(const std::vector<int> &on);
static std::string g_rom_stem;   /* ROM file name without its extension (names the cheat file) */
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
static bool g_snap_on;
static std::vector<uint16_t> g_snap;
static int g_snap_w, g_snap_h;
static void snap_take(void);

void S9xSyncSpeed()
{
	S9XP_ENTER(FRONT);
	void *silent = (Settings.Mute || !g_audio_on) ? (void *) 1 : (void *) 0;
	if (S9xAPUMTActive()) S9xAPUMTPost(mix_job, silent);
	else mix_job(silent);
	g_frame_done = true;
	if (g_snap_on) snap_take();
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
	Settings.ApplyCheats = FALSE;   /* unused by the core; the codes are switched on per game by snes_host_cheat_* */
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
	return snes_host_load_rom_mem(rom.data(), rom.size(), rom_path, srm_path, err, errlen);
}

int snes_host_load_rom_mem(const uint8_t *data, size_t size, const char *rom_path, const char *srm_path, char *err, size_t errlen)
{
	if (!g_inited) { snprintf(err, errlen, "not initialised"); return 1; }
	if (!data || !size || size > 8 * 1024 * 1024 + 4096) { snprintf(err, errlen, "Cannot read the ROM file"); return 2; }
	const uint8_t *p = data;
	size_t n = size;
	if (n - (n / 0x2000) * 0x2000 == 512) { p += 512; n -= 512; } /* copier header */

	const char *base = rom_path ? strrchr(rom_path, '/') : NULL;
	base = base ? base + 1 : (rom_path ? rom_path : "game.sfc");
	g_loaded = false;
	S9xAPUMTSet(false);
	S9xPPUMTSet(false);
	cheats_forget();   /* the old game's codes (restores its memory) and Snes9x's own .cht lookup, which this host does not use */
	Cheat.enabled = FALSE;
	if (!Memory.LoadROMMem(p, (uint32) n, base)) { snprintf(err, errlen, "Not a valid SNES ROM"); return 3; }
	S9xDeleteCheats();
	S9xCheatsEnable();   /* the engine is on; with no group it does nothing, so a game without codes runs bit for bit as before */
	g_rom_stem = base;
	{ size_t dot = g_rom_stem.rfind('.'); if (dot != std::string::npos && dot > 0) g_rom_stem.resize(dot); }
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

void snes_host_unload(void) { S9xAPUMTSet(false); S9xPPUMTSet(false); if (g_loaded) cheats_forget(); g_loaded = false; }

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

void snes_host_reset(void)
{
	if (!g_loaded) return;
	std::vector<int> on;
	cheats_suspend(on);   /* original bytes back (ROM patches included), reset, then the codes are armed again */
	S9xSoftReset();
	cheats_resume(on);
}

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

/* Test hook: a copy of the picture as it stands when the frame ends (S9xSyncSpeed), which for some cartridges is earlier than
 * what snes_host_video() sees after snes_host_run_frame() returns (the main loop runs on for a few lines of the next frame). */
static void snap_take(void)
{
	int w, h, pitch;
	const uint16_t *v = snes_host_video(&w, &h, &pitch);
	if (!v || w <= 0 || h <= 0) { g_snap_w = g_snap_h = 0; return; }
	g_snap.resize((size_t) w * h);
	for (int y = 0; y < h; y++) memcpy(&g_snap[(size_t) y * w], (const uint8_t *) v + (size_t) y * pitch, (size_t) w * 2);
	g_snap_w = w; g_snap_h = h;
}
void snes_host_snapshot_enable(int on) { g_snap_on = on != 0; }
const uint16_t *snes_host_video_snapshot(int *w, int *h)
{
	if (w) *w = g_snap_w;
	if (h) *h = g_snap_h;
	return g_snap_w ? &g_snap[0] : NULL;
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

const uint8_t *snes_host_rom_image(size_t *n)
{
	if (!g_loaded || !Memory.ROM) { if (n) *n = 0; return NULL; }
	if (n) *n = (size_t) Memory.CalculatedSize;
	return Memory.ROM;
}

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

static int write_atomic(const char *path, const void *data, size_t n, bool keep_bak, bool *bak_done, bool report = true)
{
	const uint8_t *d = (const uint8_t *) data;
	/* only battery saves (keep_bak) report through snes_host_save_error; a save state failing says so by its return code */
	auto E = [&](int code, const char *p, const char *what, int en) { if (keep_bak && report) err_set(code, p, what, en); else if (code) { char l[300]; snprintf(l, sizeof l, "write failed: %s: %s %s", what, p ? p : "", en ? strerror(en) : ""); log_line(l); } };
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


/* ===== Cheats ==========================================================================
 * Snes9x's own engine (cheats2.cpp: one group per code, poked into memory at the end of every
 * frame by S9xUpdateCheatsInMemory, on the emulation thread, so the APU and PPU workers, which
 * read none of that memory, stay exact). This part adds validation, a list per game kept in
 * RetroArch's .cht format (<root>/cheats/<rom name>.cht, written like a save: temp file, read
 * back, rename, verified backup of the old file first) and re-arming after reset and state load. */
bool S9xGameGenieToRaw(const std::string &code, uint32 &address, uint8 &byte);
bool S9xProActionReplayToRaw(const std::string &code, uint32 &address, uint8 &byte);

struct ChtEntry { std::string label, code, extra; bool on = false, usable = false; };
static std::vector<ChtEntry> g_ch;
static std::string g_ch_path, g_ch_dir, g_ch_global_extra;
static bool g_ch_bak_done, g_ch_blocked, g_ch_dirty;
static char g_ch_errtxt[200];

static bool is_hex_run(const std::string &s, size_t a, size_t n)
{
	if (n == 0 || a + n > s.size()) return false;
	for (size_t i = a; i < a + n; i++) if (!isxdigit((unsigned char) s[i])) return false;
	return true;
}

/* Checks one code (cells separated by '+', ',', ';', blanks or line ends) and writes the form the
 * core is given. Returns the number of cells (1..16) or a SNES_CHEAT_E_* code. */
static int cheat_parse(const char *code, std::string *canon_out, std::string *raw_out)
{
	if (!code) return SNES_CHEAT_E_FORMAT;
	std::vector<std::string> cells;
	std::string cur;
	for (const char *p = code;; p++) {
		unsigned char c = (unsigned char) *p;
		if (!c || c == '+' || c == ',' || c == ';' || c == ' ' || c == '\t' || c == '\r' || c == '\n') {
			if (!cur.empty()) { cells.push_back(cur); cur.clear(); }
			if (!c) break;
		} else cur += (char) toupper(c);
	}
	if (cells.empty()) return SNES_CHEAT_E_FORMAT;
	if (cells.size() > 16) return SNES_CHEAT_E_TOO_LONG;
	std::string out, raw;
	for (auto &t : cells) {
		std::string o;
		uint32 addr = 0; uint8 val = 0;
		if (t.size() == 9 && t[4] == '-' && is_hex_run(t, 0, 4) && is_hex_run(t, 5, 4)) {          /* Game Genie XXXX-XXXX */
			if (!S9xGameGenieToRaw(t, addr, val)) return SNES_CHEAT_E_FORMAT;
			o = t;
		} else if (t.size() == 8 && is_hex_run(t, 0, 8)) {                                          /* Pro Action Replay AAAAAAVV */
			if (!S9xProActionReplayToRaw(t, addr, val)) return SNES_CHEAT_E_FORMAT;
			o = t;
		} else {                                                                                    /* AAAAAA:VV  AAAAAA=VV  AAAAAA=CC?VV */
			size_t sep = t.find_first_of(":=");
			if (sep == std::string::npos || sep < 1 || sep > 6 || !is_hex_run(t, 0, sep)) return SNES_CHEAT_E_FORMAT;
			std::string rest = t.substr(sep + 1);
			char buf[40];
			unsigned a = (unsigned) strtoul(t.substr(0, sep).c_str(), NULL, 16);
			size_t q = rest.find('?');
			if (q == std::string::npos) {
				if (rest.empty() || rest.size() > 2 || !is_hex_run(rest, 0, rest.size())) return SNES_CHEAT_E_FORMAT;
				unsigned v = (unsigned) strtoul(rest.c_str(), NULL, 16);
				snprintf(buf, sizeof buf, "%06X=%02X", a, v);
			} else {
				if (t[sep] != '=' || q < 1 || q > 2 || rest.size() - q - 1 < 1 || rest.size() - q - 1 > 2 ||
				    !is_hex_run(rest, 0, q) || !is_hex_run(rest, q + 1, rest.size() - q - 1)) return SNES_CHEAT_E_FORMAT;
				unsigned cnd = (unsigned) strtoul(rest.substr(0, q).c_str(), NULL, 16);
				unsigned v = (unsigned) strtoul(rest.substr(q + 1).c_str(), NULL, 16);
				snprintf(buf, sizeof buf, "%06X=%02X?%02X", a, cnd, v);
			}
			addr = a;
			o = buf;
		}
		if (addr == 0) return SNES_CHEAT_E_RANGE;   /* the engine drops address 0 */
		if (!out.empty()) out += '+';
		out += o;
		char rb[40];
		if (o.size() == 9 && o[4] == '-') snprintf(rb, sizeof rb, "%06X=%02X", (unsigned) addr, (unsigned) val);
		else if (o.size() == 8 && o.find('=') == std::string::npos) snprintf(rb, sizeof rb, "%06X=%02X", (unsigned) addr, (unsigned) val);
		else snprintf(rb, sizeof rb, "%s", o.c_str());
		if (!raw.empty()) raw += '+';
		raw += rb;
	}
	if (canon_out) *canon_out = out;
	if (raw_out) *raw_out = raw;
	return (int) cells.size();
}

int snes_host_cheat_validate(const char *code, char *canon, size_t cap)
{
	std::string c;
	int r = cheat_parse(code, &c, NULL);
	if (r > 0 && canon) {
		if (c.size() + 1 > cap) return SNES_CHEAT_E_TOO_LONG;
		memcpy(canon, c.c_str(), c.size() + 1);
	}
	return r;
}

int snes_host_cheat_decode(const char *code, char *raw, size_t cap)
{
	std::string r;
	int n = cheat_parse(code, NULL, &r);
	if (n > 0 && raw) {
		if (r.size() + 1 > cap) return SNES_CHEAT_E_TOO_LONG;
		memcpy(raw, r.c_str(), r.size() + 1);
	}
	return n;
}

int snes_host_peek_wram(uint32_t offset) { return g_loaded && offset < 0x20000 ? Memory.RAM[offset] : -1; }
void snes_host_poke_wram(uint32_t offset, uint8_t value) { if (g_loaded && offset < 0x20000) Memory.RAM[offset] = value; }
int snes_host_ppu_scroll(int *h, int *v)
{
	if (!g_loaded) return -1;
	for (int i = 0; i < 4; i++) { h[i] = PPU.BG[i].HOffset; v[i] = PPU.BG[i].VOffset; }
	return PPU.BGMode;
}

static int cheat_gidx(int i) { int n = 0; for (int k = 0; k < i; k++) if (g_ch[k].usable) n++; return n; }

static std::string cheat_label_clean(const char *s)
{
	std::string o;
	for (; s && *s; s++) {
		unsigned char c = (unsigned char) *s;
		if (c < 0x20 || c == 0x7f) c = ' ';
		if (c == '"') c = '\'';
		if (c == ' ' && (o.empty() || o.back() == ' ')) continue;
		o += (char) c;
	}
	while (!o.empty() && o.back() == ' ') o.pop_back();
	if (o.size() > SNES_CHEAT_NAME_CAP - 1) {
		o.resize(SNES_CHEAT_NAME_CAP - 1);
		while (!o.empty() && ((unsigned char) o.back() & 0xC0) == 0x80) o.pop_back();   /* not inside a UTF-8 character */
		if (!o.empty() && ((unsigned char) o.back() & 0x80) && ((unsigned char) o.back() & 0x40)) o.pop_back();
	}
	return o;
}

static void cheats_forget(void)
{
	S9xDeleteCheats();   /* restores every byte a code changed */
	g_ch.clear();
	g_ch_path.clear(); g_ch_dir.clear(); g_ch_global_extra.clear();
	g_ch_bak_done = g_ch_blocked = g_ch_dirty = false;
	g_ch_errtxt[0] = 0;
}

/* codes off with their bytes put back, remembering which were on (reset, state load) */
static void cheats_suspend(std::vector<int> &on)
{
	on.clear();
	if (Cheat.group.empty()) return;
	snes_host_sync();
	for (size_t i = 0; i < Cheat.group.size(); i++) {
		on.push_back(Cheat.group[i].enabled ? 1 : 0);
		if (Cheat.group[i].enabled) S9xDisableCheatGroup((uint32) i);
	}
}
static void cheats_resume(const std::vector<int> &on)
{
	for (size_t i = 0; i < on.size() && i < Cheat.group.size(); i++)
		if (on[i]) S9xEnableCheatGroup((uint32) i);
}

static std::string cht_text(void)
{
	std::string t = "cheats = " + std::to_string(g_ch.size()) + "\n";
	for (size_t i = 0; i < g_ch.size(); i++) {
		std::string n = std::to_string(i);
		t += "cheat" + n + "_desc = \"" + g_ch[i].label + "\"\n";
		t += "cheat" + n + "_code = \"" + g_ch[i].code + "\"\n";
		t += "cheat" + n + "_enable = " + (g_ch[i].on ? "true" : "false") + "\n";
		t += g_ch[i].extra;
	}
	t += g_ch_global_extra;
	return t;
}

int snes_host_cheats_save(void)
{
	if (g_ch_path.empty()) return 0;   /* no file attached: the list lives in memory only */
	if (g_ch_blocked) {
		snprintf(g_ch_errtxt, sizeof g_ch_errtxt, "cheat file not saved: the existing file could not be read, it is left alone");
		return SNES_CHEAT_E_IO;
	}
	make_dirs(g_ch_dir);
	std::string t = cht_text();
	int r = write_atomic(g_ch_path.c_str(), t.data(), t.size(), true, &g_ch_bak_done, false);
	if (r) {
		snprintf(g_ch_errtxt, sizeof g_ch_errtxt, "cheat file not saved (code %d, %s); the old file is untouched", r, strerror(errno));
		g_ch_dirty = true;
		return SNES_CHEAT_E_IO;
	}
	g_ch_errtxt[0] = 0;
	g_ch_dirty = false;
	return 0;
}

int snes_host_cheat_error(char *text, size_t cap)
{
	if (text && cap) snprintf(text, cap, "%s", g_ch_errtxt);
	return g_ch_errtxt[0] ? 1 : 0;
}

static std::string cht_trim(const std::string &s)
{
	size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
	return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

int snes_host_cheats_load(const char *root)
{
	if (!g_loaded || !root) return SNES_CHEAT_E_NOROM;
	cheats_forget();
	g_ch_dir = std::string(root) + "/cheats";
	g_ch_path = g_ch_dir + "/" + g_rom_stem + ".cht";
	struct stat st;
	if (stat(g_ch_path.c_str(), &st) != 0) return 0;   /* no file yet: an empty list */
	std::vector<uint8_t> raw;
	if (st.st_size > 0 && (st.st_size > (4 << 20) || !read_file(g_ch_path.c_str(), raw))) {
		g_ch_blocked = true;
		snprintf(g_ch_errtxt, sizeof g_ch_errtxt, "cheat file unreadable, left alone");
		return SNES_CHEAT_E_IO;
	}
	struct Raw { std::string desc, code, extra; bool on = false, has_code = false; };
	std::vector<Raw> items;
	std::string text((const char *) raw.data(), raw.size()), line;
	text += "\n";
	for (size_t p = 0; p < text.size(); p++) {
		if (text[p] != '\n') { line += text[p]; continue; }
		std::string l = cht_trim(line);
		line.clear();
		if (l.empty()) continue;
		size_t eq = l.find('=');
		std::string key = cht_trim(eq == std::string::npos ? l : l.substr(0, eq));
		std::string val = eq == std::string::npos ? "" : cht_trim(l.substr(eq + 1));
		std::string unq = val;
		if (unq.size() >= 2 && unq.front() == '"' && unq.back() == '"') unq = unq.substr(1, unq.size() - 2);
		if (key == "cheats") continue;
		if (key.compare(0, 5, "cheat") == 0 && key.size() > 6 && isdigit((unsigned char) key[5])) {
			size_t u = key.find('_');
			if (u != std::string::npos && u > 5) {
				size_t idx = (size_t) strtoul(key.substr(5, u - 5).c_str(), NULL, 10);
				std::string suf = key.substr(u + 1);
				if (idx < SNES_CHEATS_MAX) {
					if (items.size() <= idx) items.resize(idx + 1);
					Raw &r = items[idx];
					if (suf == "desc") r.desc = unq;
					else if (suf == "code") { r.code = unq; r.has_code = true; }
					else if (suf == "enable") r.on = (unq == "true" || unq == "1");
					else r.extra += "cheat@" + suf + " = " + val + "\n";   /* '@' is the entry number, filled in on writing */
					continue;
				}
			}
		}
		g_ch_global_extra += l + "\n";
	}
	for (auto &r : items) {
		if (!r.has_code) continue;
		ChtEntry e;
		e.label = cheat_label_clean(r.desc.c_str());
		if (e.label.empty()) e.label = "Cheat " + std::to_string(g_ch.size() + 1);
		e.code = r.code;
		e.on = r.on;
		/* the extra keys carry the new number */
		for (size_t q = 0; q < r.extra.size();) {
			size_t nl = r.extra.find('\n', q);
			std::string ln = r.extra.substr(q, nl - q);
			size_t at = ln.find('@');
			e.extra += ln.substr(0, at) + std::to_string(g_ch.size()) + "_" + ln.substr(at + 1) + "\n";
			q = nl + 1;
		}
		char canon[600];
		if (snes_host_cheat_validate(r.code.c_str(), canon, sizeof canon) > 0 && S9xAddCheatGroup("", canon) >= 0) e.usable = true;
		g_ch.push_back(e);
		if (e.usable && e.on) S9xEnableCheatGroup((uint32) cheat_gidx((int) g_ch.size() - 1));
		if (g_ch.size() >= SNES_CHEATS_MAX) break;
	}
	return (int) g_ch.size();
}

int snes_host_cheat_count(void) { return (int) g_ch.size(); }

int snes_host_cheat_get(int i, snes_cheat_info *out)
{
	if (i < 0 || i >= (int) g_ch.size() || !out) return SNES_CHEAT_E_RANGE;
	snprintf(out->label, sizeof out->label, "%s", g_ch[i].label.c_str());
	snprintf(out->code, sizeof out->code, "%s", g_ch[i].code.c_str());
	out->enabled = g_ch[i].on ? 1 : 0;
	out->usable = g_ch[i].usable ? 1 : 0;
	return 0;
}

int snes_host_cheat_add(const char *code, const char *label)
{
	if (!g_loaded) return SNES_CHEAT_E_NOROM;
	if ((int) g_ch.size() >= SNES_CHEATS_MAX) return SNES_CHEAT_E_FULL;
	char canon[600];
	int r = snes_host_cheat_validate(code, canon, sizeof canon);
	if (r < 0) return r;
	if (strlen(canon) >= SNES_CHEAT_CODE_CAP) return SNES_CHEAT_E_TOO_LONG;
	snes_host_sync();
	int gi = S9xAddCheatGroup("", canon);
	if (gi < 0 || gi != (int) cheat_gidx((int) g_ch.size())) { if (gi >= 0) S9xDeleteCheatGroup((uint32) gi); return SNES_CHEAT_E_FORMAT; }
	ChtEntry e;
	e.label = cheat_label_clean(label);
	if (e.label.empty()) e.label = "Cheat " + std::to_string(g_ch.size() + 1);
	e.code = canon;
	e.on = true; e.usable = true;
	g_ch.push_back(e);
	S9xEnableCheatGroup((uint32) gi);
	snes_host_cheats_save();   /* a failure is in snes_host_cheat_error(); the code works and is retried on the next change */
	return (int) g_ch.size() - 1;
}

int snes_host_cheat_enable(int i, int on)
{
	if (i < 0 || i >= (int) g_ch.size()) return SNES_CHEAT_E_RANGE;
	ChtEntry &e = g_ch[i];
	if (!e.usable) { if (!on) { e.on = false; snes_host_cheats_save(); return 0; } return SNES_CHEAT_E_UNSUPPORTED; }
	if ((e.on ? 1 : 0) == (on ? 1 : 0)) return 0;
	snes_host_sync();
	if (on) S9xEnableCheatGroup((uint32) cheat_gidx(i)); else S9xDisableCheatGroup((uint32) cheat_gidx(i));
	e.on = on != 0;
	snes_host_cheats_save();
	return 0;
}

int snes_host_cheat_remove(int i)
{
	if (i < 0 || i >= (int) g_ch.size()) return SNES_CHEAT_E_RANGE;
	snes_host_sync();
	if (g_ch[i].usable) S9xDeleteCheatGroup((uint32) cheat_gidx(i));
	g_ch.erase(g_ch.begin() + i);
	/* extra keys name their entry by number: renumber the ones after it */
	for (size_t k = (size_t) i; k < g_ch.size(); k++) {
		std::string fixed;
		for (size_t q = 0; q < g_ch[k].extra.size();) {
			size_t nl = g_ch[k].extra.find('\n', q);
			std::string ln = g_ch[k].extra.substr(q, nl - q);
			size_t us = ln.find('_');
			if (us != std::string::npos) ln = "cheat" + std::to_string(k) + ln.substr(us);
			fixed += ln + "\n";
			q = nl + 1;
		}
		g_ch[k].extra = fixed;
	}
	snes_host_cheats_save();
	return 0;
}

void snes_host_cheat_disable_all(void)
{
	snes_host_sync();
	for (size_t i = 0; i < g_ch.size(); i++) if (g_ch[i].on && g_ch[i].usable) S9xDisableCheatGroup((uint32) cheat_gidx((int) i));
	for (auto &e : g_ch) e.on = false;
	snes_host_cheats_save();
}

void snes_host_cheat_reset(void)
{
	snes_host_sync();
	S9xDeleteCheats();
	g_ch.clear();
	snes_host_cheats_save();
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

size_t snes_host_state_size(void)
{
	return g_loaded ? (size_t) S9xFreezeSize() : 0;
}

int snes_host_state_save_buf(uint8_t *out, size_t cap, size_t *len)
{
	if (!g_loaded) return 1;
	size_t n = S9xFreezeSize();
	if (len) *len = n;
	if (!out || cap < n) return 2;
	if (!S9xFreezeGameMem(out, (uint32) n)) return 2;
	return 0;
}

int snes_host_state_load_buf(const uint8_t *data, size_t n)
{
	if (!g_loaded) return 1;
	if (!data || !n) return 3;
	std::vector<int> on;
	cheats_suspend(on);
	int r = S9xUnfreezeGameMem(data, (uint32) n) == SUCCESS ? 0 : 3;
	cheats_resume(on);
	return r;
}

int snes_host_state_load(const char *path)
{
	if (!g_loaded) return 1;
	std::vector<uint8_t> buf;
	if (!read_file(path, buf)) return 2;
	std::vector<int> on;
	cheats_suspend(on);   /* the state holds the game's own bytes; the codes poke them again after the load */
	int r = S9xUnfreezeGameMem(buf.data(), (uint32) buf.size()) == SUCCESS ? 0 : 3;
	cheats_resume(on);
	return r;
}
