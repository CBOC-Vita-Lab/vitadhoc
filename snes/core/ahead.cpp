/*
 * CBOC-Vita local addition (not part of upstream Snes9x). MIT licence.
 *
 * Run-ahead ("Quick response", snes/frontend/host.cpp snes_host_run_frame_ahead): a light snapshot of the machine in memory and the
 * way back to it, for the moment between two frames. Snes9x's own state load (snapshot.cpp) resets the machine, has the picture
 * thread wait and take the whole video memory again and clears every tile cache; this copies exactly what a frame can change and
 * puts back exactly what the look-ahead frame changed, so the caches that follow the machine stay valid.
 *
 * What a frame can change was taken from the program itself (every static and global variable of the core's objects, `nm`; see
 * snes/README.md "Quick response"):
 *   - the 65816 and the frame loop: CPU, ICPU, Registers, Timings, OpenBus, DMA, HDMAMemPointers;
 *   - memory: work RAM, the register block (FillRAM, 32 KB: also the Super FX and SA-1 registers and the cache RAM), the battery RAM
 *     up to where the cartridge can reach it (see sram_reach), video RAM (compared: below);
 *   - the PPU: PPU, IPPU (but the frame's own picture state, below), the sprite tables (GFX.OBJLines ..., the range and time over
 *     flags come out of them; copied aside only when the look-ahead frame rebuilds them, gfx.cpp);
 *   - the pads (controls.cpp: read positions, latch, devices);
 *   - the cheat engine's memory of each code (the byte under it, a conditional code's condition);
 *   - the sound side (apu.cpp S9xAPUAheadSave: SPC700, DSP, ports, the samples not taken yet; in the APU thread's stream);
 *   - the chips in use: Super FX (GSU, SuperFX, the cel hold of fxemu.cpp), SA-1 (SA1, SA1Registers, its open bus, the replay
 *     engine, the memory map it changes), DSP-1/2/3/4 (structs; DSP-3's command state is a function pointer), C4 (its RAM and
 *     working values), OBC1 (struct, RAM), S-DD1 (the memory map), ST010 (struct; its RAM is the battery RAM), MSU-1 (registers,
 *     sample position, track; the files are put back where the registers say).
 * Not supported (S9xAheadUnsupported says why): SPC7110 and its clock, S-RTC, ST011, ST018, BS-X and the Sufami Turbo: their
 * state is in local static variables of their decoders or follows the real time clock.
 *
 * Derived state that is kept valid instead of rebuilt:
 *   - video RAM is compared in blocks of 32 bytes; a block the look-ahead frame changed is copied back and only its tiles are taken
 *     out of the tile cache, marked for the picture thread (its private copy takes the block with the next batch) and counted as
 *     written for the wide view's margin strip cache;
 *   - the colour tables are rebuilt from the palette before the next picture (g_s9x_colours_stale);
 *   - the wide view's write stamps are put back (the margin they describe is the one of the frame that is shown next), its clock
 *     goes on (a stamp of the next frame is the next frame's);
 *   - the picture state of a frame (GFX.DoInterlace, GFX.PPL, the double width / height flags, the rendered size, the line counters)
 *     belongs to the frames that are drawn: the look-ahead frame continues from the one drawn before it (S9xAheadBegin keeps it, the
 *     save puts it back) and the way back leaves it as the look-ahead frame made it.
 */
#include <string.h>
#include <vector>
#include "snes9x.h"
#include "memmap.h"
#include "cpuexec.h"
#include "dma.h"
#include "ppu.h"
#include "gfx.h"
#include "controls.h"
#include "apu/apu.h"
#include "fxemu.h"
#include "fxinst.h"
#include "sa1.h"
#include "dsp.h"
#include "c4.h"
#include "obc1.h"
#include "seta.h"
#include "msu1.h"
#include "cheats.h"
#include "widecap.h"
#include "s9x_ahead.h"

extern uint8 *HDMAMemPointers[8];

/* test builds only (snes/twin/ahead_check.sh --negative): leave one part out, the proof must fail */
#ifndef AHEAD_NEGATIVE_CONTROL
#define AHEAD_NEGATIVE_CONTROL 0
#endif
#define NC(n) (AHEAD_NEGATIVE_CONTROL == (n))

namespace {

struct Part { void *p; size_t n; };

struct RenderCarry
{
	uint8 do_interlace;
	uint32 ppl;
	bool8 dw, dh;
	int rw, rh, cur, prev;
	uint32 rendered, displayed;
};

std::vector<Part> parts;			// plain copies, in order
std::vector<uint8> buf;				// their bytes
std::vector<uint8> vram, obj, ctl, sa1l, msu, vage, uage, cht;
bool built = false, saved = false;
size_t sram_n = 0;
bool maps = false;					// the chip changes the memory map at run time (SA-1, S-DD1)
std::vector<uint8 *> map_keep, wmap_keep;
uint8 *bwram_keep;
struct InternalPPU ippu_keep;
void *dsp3_keep;
int32 cel_hold;
uint8 cel_prev;
uint32 wide_clock0;
bool stamps;
RenderCarry carry;
S9xAheadStats st;

void carry_get (RenderCarry &c)
{
	c.do_interlace = GFX.DoInterlace;
	c.ppl = GFX.PPL;
	c.dw = IPPU.DoubleWidthPixels;
	c.dh = IPPU.DoubleHeightPixels;
	c.rw = IPPU.RenderedScreenWidth;
	c.rh = IPPU.RenderedScreenHeight;
	c.cur = IPPU.CurrentLine;
	c.prev = IPPU.PreviousLine;
	c.rendered = IPPU.RenderedFramesCount;
	c.displayed = IPPU.DisplayedRenderedFrameCount;
}

void carry_put (const RenderCarry &c)
{
	GFX.DoInterlace = c.do_interlace;
	GFX.PPL = c.ppl;
	IPPU.DoubleWidthPixels = c.dw;
	IPPU.DoubleHeightPixels = c.dh;
	IPPU.RenderedScreenWidth = c.rw;
	IPPU.RenderedScreenHeight = c.rh;
	IPPU.CurrentLine = c.cur;
	IPPU.PreviousLine = c.prev;
	IPPU.RenderedFramesCount = c.rendered;
	IPPU.DisplayedRenderedFrameCount = c.displayed;
}

/* how far into the battery RAM block the cartridge can reach (snes/frontend/host.cpp, rewind's states, has the reasoning) */
size_t sram_reach (void)
{
	const size_t full = Memory.SRAM_SIZE;
	size_t n = ((size_t) Memory.SRAMMask + 1 + 0xFFF) & ~(size_t) 0xFFF;
	if (n < 0x1000)
		n = 0x1000;
	if (Settings.SuperFX && !Settings.SA1)
		n = 0x20000;
	if (Settings.SA1)
	{
		const size_t dma = (((size_t) Memory.SRAMMask + 1) * 2 + 0xFFF) & ~(size_t) 0xFFF;
		n = dma > 0x41000 ? dma : 0x41000;
	}
	return n > full ? full : n;
}

void add (void *p, size_t n) { Part q = { p, n }; parts.push_back(q); }

/* the cheat engine's memory of each code (the byte the game wrote under it, whether a conditional code's condition holds): the
 * look-ahead frame's end of frame update changes it, and a condition it saw first would not fire in the real frame */
void cheats_copy (bool save)
{
	if (save)
		cht.clear();
	size_t k = 0;
	for (size_t g = 0; g < Cheat.group.size(); g++)
		for (size_t i = 0; i < Cheat.group[g].cheat.size(); i++)
		{
			SCheat &c = Cheat.group[g].cheat[i];
			if (save)
			{
				cht.push_back(c.saved_byte);
				cht.push_back(c.cond_true);
			}
			else if (k + 2 <= cht.size())
			{
				c.saved_byte = cht[k];
				c.cond_true = cht[k + 1];
				k += 2;
			}
		}
}

void build (void)
{
	parts.clear();
	add(&CPU, sizeof(CPU));
	add(&ICPU, sizeof(ICPU));
	add(&Registers, sizeof(Registers));
	add(&Timings, sizeof(Timings));
	add(&OpenBus, sizeof(OpenBus));
	add(DMA, sizeof(DMA));
	add(HDMAMemPointers, sizeof(HDMAMemPointers));
	add(&PPU, sizeof(PPU));
	if (!NC(7)) add(Memory.RAM, sizeof(Memory.RAM));
	add(Memory.FillRAM, 0x8000);
	sram_n = sram_reach();
	add(Memory.SRAM, sram_n);
	if (Settings.SuperFX)
	{
		add(&GSU, sizeof(GSU));
		add(&SuperFX, sizeof(SuperFX));
	}
	if (Settings.SA1)
	{
		add(&SA1, sizeof(SA1));
		if (!NC(2)) add(&SA1Registers, sizeof(SA1Registers));
		add(&SA1OpenBus, sizeof(SA1OpenBus));
		add(&SA1Effects, sizeof(SA1Effects));
		add(&SA1Rec, sizeof(SA1Rec));
		add(&Memory.ROM[CMemory::MAX_ROM_SIZE - 0x10000], 128);	// the eight rows of a type 2 character conversion under way (sa1.cpp, $224F)
		sa1l.resize(S9xSA1AheadCopy(NULL, 0));
	}
	if (Settings.DSP)
	{
		add(&DSP0, sizeof(DSP0));
		if (!NC(6)) add(&DSP1, sizeof(DSP1));
		add(&DSP2, sizeof(DSP2));
		add(&DSP4, sizeof(DSP4));
		if (Settings.DSP == 3)
			add(&DSP3, sizeof(DSP3));
	}
	if (Settings.C4)
	{
		add(Memory.C4RAM, 8192);
		static int16 *const c4v[] = { &C4WFXVal, &C4WFYVal, &C4WFZVal, &C4WFX2Val, &C4WFY2Val, &C4WFDist, &C4WFScale,
			&C41FXVal, &C41FYVal, &C41FAngleRes, &C41FDist, &C41FDistVal };
		for (size_t i = 0; i < sizeof(c4v) / sizeof(c4v[0]); i++)
			add(c4v[i], sizeof(int16));
	}
	if (Settings.OBC1)
	{
		add(&OBC1, sizeof(OBC1));
		add(Memory.OBC1RAM, 8192);
	}
	if (Settings.SETA == ST_010)
		add(&ST010, sizeof(ST010));
	maps = Settings.SA1 || Settings.SDD1;
	if (maps)
	{
		map_keep.resize(MEMMAP_NUM_BLOCKS);
		wmap_keep.resize(MEMMAP_NUM_BLOCKS);
	}
	if (Settings.MSU1)
		msu.resize(S9xMSU1AheadCopy(NULL, 0));
	size_t total = 0;
	for (size_t i = 0; i < parts.size(); i++)
		total += parts[i].n;
	buf.resize(total);
	vram.resize(sizeof(Memory.VRAM));
	obj.resize(S9xAheadObjSize());
	ctl.resize(S9xControlAheadCopy(NULL, 0));
	built = true;
	st.bytes = total + vram.size() + ctl.size();
}

/* A big block is brought in line in chunks: a chunk that is already equal is only read. A frame changes little of the work RAM, the
 * register block or the battery RAM, so the save (which follows the previous save) and the way back mostly read; writing costs more
 * than reading on the console (a write that misses the cache first reads the line). */
inline bool same256 (const uint8 *a, const uint8 *b)
{
	uint64 x = 0;
	for (int i = 0; i < 256; i += 32)
	{
		uint64 a0, a1, a2, a3, b0, b1, b2, b3;
		memcpy(&a0, a + i, 8); memcpy(&a1, a + i + 8, 8); memcpy(&a2, a + i + 16, 8); memcpy(&a3, a + i + 24, 8);
		memcpy(&b0, b + i, 8); memcpy(&b1, b + i + 8, 8); memcpy(&b2, b + i + 16, 8); memcpy(&b3, b + i + 24, 8);
		x |= (a0 ^ b0) | (a1 ^ b1) | (a2 ^ b2) | (a3 ^ b3);
	}
	return x == 0;
}

inline void sync (uint8 *dst, const uint8 *src, size_t n)
{
	size_t o = 0;
	for (; o + 256 <= n; o += 256)
		if (!same256(dst + o, src + o))
			memcpy(dst + o, src + o, 256);
	if (o < n && memcmp(dst + o, src + o, n - o))
		memcpy(dst + o, src + o, n - o);
}

inline void put (uint8 *dst, const uint8 *src, size_t n)
{
	if (n >= 4096)
		sync(dst, src, n);
	else
		memcpy(dst, src, n);
}

/* the wide view's ageing of old stamps (ppu_mt.cpp S9xWideFrameStartLive), done again after the stamps were put back when the
 * look-ahead frame was the one that did it */
void stamps_age (uint32 clock)
{
	const uint16 c = (uint16) clock;
	for (int i = 0; i < 32768; i++)
		if (g_s9x_vage[i] && (uint16) (c - g_s9x_vage[i]) >= 32768) g_s9x_vage[i] = 0;
	for (int i = 0; i < 2048; i++)
		if (g_s9x_uage[i] && (uint16) (c - g_s9x_uage[i]) >= 32768) g_s9x_uage[i] = 0;
}

} // namespace

const char *S9xAheadUnsupported (void)
{
	if (Settings.SPC7110 || Settings.SPC7110RTC)
		return "SPC7110 chip";
	if (Settings.SRTC)
		return "S-RTC clock chip";
	if (Settings.SETA == ST_011 || Settings.SETA == ST_018)
		return "ST011 / ST018 chip";
	if (Settings.BS || Multi.cartType)
		return "BS-X or Sufami Turbo cartridge";
	return NULL;
}

void S9xAheadReset (void)
{
	built = false;
	saved = false;
	parts.clear();
	buf.clear();
	S9xAheadObjArm(NULL);
	memset(&st, 0, sizeof st);
}

void S9xAheadBegin (void)
{
	carry_get(carry);
}

bool S9xAheadSave (void)
{
	if (S9xAheadUnsupported())
		return false;
	if (!built)
		build();
	if (Settings.SA1)
		S9xSA1ReplaySync();				// the SA-1's registers as they are now (what Snes9x's own state save does first)
	uint8 *b = &buf[0];
	for (size_t i = 0; i < parts.size(); i++)
	{
		put(b, (const uint8 *) parts[i].p, parts[i].n);
		b += parts[i].n;
	}
	memcpy(&ippu_keep, &IPPU, sizeof(IPPU));
	sync(&vram[0], Memory.VRAM, sizeof(Memory.VRAM));
	S9xControlAheadCopy(&ctl[0], 1);
	if (!NC(8)) cheats_copy(true);
	if (Settings.SuperFX)
		S9xSuperFXAheadCopy(&cel_hold, &cel_prev, 1);
	if (Settings.SA1)
		S9xSA1AheadCopy(&sa1l[0], 1);
	if (Settings.DSP == 3)
		dsp3_keep = S9xDSP3AheadState(NULL);
	if (maps)
	{
		memcpy(&map_keep[0], Memory.Map, sizeof(Memory.Map));
		memcpy(&wmap_keep[0], Memory.WriteMap, sizeof(Memory.WriteMap));
		bwram_keep = Memory.BWRAM;
	}
	if (Settings.MSU1)
		S9xMSU1AheadCopy(&msu[0], 1);
	stamps = g_s9x_wide_stamp != 0;
	if (stamps)
	{
		vage.resize(sizeof(g_s9x_vage));
		uage.resize(sizeof(g_s9x_uage));
		memcpy(&vage[0], g_s9x_vage, sizeof(g_s9x_vage));
		memcpy(&uage[0], g_s9x_uage, sizeof(g_s9x_uage));
		wide_clock0 = g_s9x_wide_clock;
	}
	S9xAPUAheadSave();
	S9xAheadObjArm(&obj[0]);
	/* the look-ahead frame is drawn after the one drawn before it */
	carry_put(carry);
	saved = true;
	st.saves++;
	return true;
}

bool S9xAheadRestore (void)
{
	if (!saved)
		return false;
	saved = false;
	RenderCarry now;
	carry_get(now);
	const bool8 render = IPPU.RenderThisFrame;
	const uint8 *b = &buf[0];
	for (size_t i = 0; i < parts.size(); i++)
	{
		put((uint8 *) parts[i].p, b, parts[i].n);
		b += parts[i].n;
	}
	memcpy(&IPPU, &ippu_keep, sizeof(IPPU));
	IPPU.RenderThisFrame = render;
	carry_put(now);
	if (!NC(4)) S9xControlAheadCopy(&ctl[0], 0);
	if (!NC(8)) cheats_copy(false);
	if (Settings.SuperFX)
		S9xSuperFXAheadCopy(&cel_hold, &cel_prev, 0);
	if (Settings.SA1)
		S9xSA1AheadCopy(&sa1l[0], 0);
	if (Settings.DSP == 3)
		S9xDSP3AheadState(dsp3_keep);
	if (maps)
	{
		memcpy(Memory.Map, &map_keep[0], sizeof(Memory.Map));
		memcpy(Memory.WriteMap, &wmap_keep[0], sizeof(Memory.WriteMap));
		Memory.BWRAM = bwram_keep;
	}
	if (Settings.MSU1)
		S9xMSU1AheadCopy(&msu[0], 0);
	/* video RAM: only the blocks the look-ahead frame changed go back, and only their tiles are drawn again */
	{
		const uint64 *s = (const uint64 *) &vram[0];
		uint64 *d = (uint64 *) Memory.VRAM;
		unsigned n = 0;
		for (uint32 blk = 0; blk < 0x10000 / 32; blk++, s += 4, d += 4)
		{
			if ((d[0] == s[0] && d[1] == s[1] && d[2] == s[2] && d[3] == s[3]) || NC(3))
				continue;
			d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3];
			const uint32 a = blk * 32;
			S9xTilesDirty(a);
			S9xTilesDirty(a + 16);
			if (g_s9x_vram_track)
				S9xPPUMTVRAMDirty(a);
			if (stamps)
				g_s9x_vblk[(a >> 11) & 31] = ++g_s9x_vseq;
			n++;
		}
		st.vram_blocks += n;
	}
	if (stamps)
	{
		memcpy(g_s9x_vage, &vage[0], sizeof(g_s9x_vage));
		memcpy(g_s9x_uage, &uage[0], sizeof(g_s9x_uage));
		for (uint32 c = wide_clock0 + 1; c != g_s9x_wide_clock + 1; c++)
			if ((c & 0x3fff) == 0)
			{
				stamps_age(g_s9x_wide_clock);
				break;
			}
	}
	if (NC(5))
		S9xAheadObjArm(NULL);
	else if (S9xAheadObjRestore(&obj[0]))
	{
		g_s9x_obj_ran = TRUE;			// the picture thread takes the sprite tables again
		st.obj_restores++;
	}
	g_s9x_colours_stale = TRUE;			// the colour tables follow the palette and brightness put back
	if (!NC(1)) S9xAPUAheadRestore();
	st.restores++;
	return true;
}

void S9xAheadGetStats (S9xAheadStats *out)
{
	*out = st;
}
