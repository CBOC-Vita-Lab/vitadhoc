/*
 * CBOC-Vita local addition (not part of upstream Snes9x). MIT licence.
 *
 * Optional PPU thread. Snes9x draws the picture in batches ("flushes") shortly
 * before a register write would change how the pending lines look. This file
 * moves the drawing of a batch to a worker thread:
 *
 *  - the register side (65816, DMA, PPU registers, sprite range/time-over
 *    flags, clip windows) runs exactly as before and, at every flush, takes a
 *    snapshot of everything the drawing code reads: PPU/IPPU/GFX state, the
 *    scroll and Mode 7 rows of the batch, FillRAM $2100-$213F, the colour
 *    tables when they changed, the sprite line table when it changed, and the
 *    VRAM blocks written since the previous flush;
 *  - the worker owns a private copy of all of that (PPU_R, IPPU_R, GFX_R,
 *    BG_R, Memory_R, LineData_R ... below) and its own tile cache, and draws
 *    with the unchanged renderer code. That code is a second copy of gfx.o,
 *    tile.o and tileimpl-*.o whose global symbols were renamed at link time
 *    (objcopy --redefine-syms, see the Makefiles), so it reads the *_R
 *    variables instead of the live ones;
 *  - the finished picture is copied to an output slot at the end of each frame.
 *
 * The register side only waits when the snapshot ring is full or when somebody
 * asks for the picture (snes_host_video). The result is bit-identical to the
 * single-thread build (twin/mt_compare.py checks that).
 */
#include <atomic>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stddef.h>
#include "snes9x.h"
#include "ppu.h"
#include "gfx.h"
#include "memmap.h"
#include "s9x_mt.h"
#include "apu/apu.h"
#include "widecap.h"

extern struct SLineData LineData[240];
extern struct SLineMatrixData LineMatrixData[240];
extern "C" void s9x_render_batch_r(const S9xRenderJob *job);
extern "C" void s9x_render_setup_r(void);
extern "C" void s9x_clear_z_r(uint8 *z, uint8 *subz);
extern "C" void S9xWideRenderFrameStart_r(int margin);
extern "C" void S9xWideFillView_r(struct SWideView *v);
/* the third copy (suffix _w): the wide view's margin passes on their own thread, see "the margin thread" below */
extern "C" void s9x_wide_batch_w(const S9xRenderJob *job);
extern "C" void s9x_render_setup_w(void);
extern "C" void S9xWideRenderFrameStart_w(int margin);
extern "C" void S9xWideFillView_w(struct SWideView *v);

/* The renamed renderer copy reads these instead of PPU, IPPU, GFX, BG, Memory,
 * LineData, LineMatrixData, DirectColourMaps and brightness_cap. */
struct SPPU PPU_R;
struct InternalPPU IPPU_R;
struct SGFX GFX_R;
struct SBG BG_R;
CMemory Memory_R;
struct SLineData LineData_R[240];
struct SLineMatrixData LineMatrixData_R[240];
uint16 DirectColourMaps_R[8][256];
uint8 brightness_cap_R[64];

/* the same for the margin thread's copy */
struct SPPU PPU_W;
struct InternalPPU IPPU_W;
struct SGFX GFX_W;
struct SBG BG_W;
CMemory Memory_W;
struct SLineData LineData_W[240];
struct SLineMatrixData LineMatrixData_W[240];
uint16 DirectColourMaps_W[8][256];
uint8 brightness_cap_W[64];

bool8 g_s9x_vram_track = FALSE;
uint32 g_s9x_colour_version = 0;
uint32 g_s9x_zdirty = 0, g_s9x_subzdirty = 0;	// see S9xClearZBuffers (gfx.cpp)
bool8 g_s9x_obj_ran = FALSE;

namespace ppumt {

enum { K_BATCH, K_FRAME_START, K_FRAME_END, K_WIDE_END };
enum { F_FULL = 1, F_COLOURS = 2 };

static const int NOBJ = sizeof(((struct SGFX *) 0)->OBJLines) / sizeof(((struct SGFX *) 0)->OBJLines[0]);
static const size_t OBJ_ELEM = sizeof(((struct SGFX *) 0)->OBJLines[0]);
static const size_t IP_A = offsetof(InternalPPU, TileCache) - offsetof(InternalPPU, Clip);
static const size_t IP_B = offsetof(InternalPPU, Red) - offsetof(InternalPPU, Interlace);
static const size_t IP_C = sizeof(InternalPPU) - offsetof(InternalPPU, ScreenColors);
static const int NBLK = 0x10000 / 32;
static const unsigned SLOTS = 16, MASK = SLOTS - 1;
static const int NOUT = 4;
static const unsigned NCEN = 3;        /* centre pictures kept for the margin thread (it may be this many frames behind the PPU thread) */
static const int CEN_ROWS = 240;

struct Slot
{
	int kind, flags, nblocks, row0, nrows, obj_from, w, h, wide_m;
	int wr;			/* the margin thread reads this slot too (it draws the wide passes and makes the capture; the PPU thread does not) */
	unsigned cseq;		/* K_WIDE_END with wr: number of the frame's centre picture */
	uint32 wclock, wepoch;
	uint16 *vage, *uage;	/* wide view: stamps of the frame end (allocated when first needed) */
	S9xRenderJob job;
	uint16 *screen, *sub, *zero;
	uint8 *z, *subz;
	uint32 ppl, dointerlace, fixedcolour, starty, endy;
	uint8 fillram[64];
	uint8 objwidths[128], objvis[128];
	uint8 ipa[IP_A], ipb[IP_B], ipc[IP_C];
	struct SPPU ppu;
	struct SLineData ld[240];
	struct SLineMatrixData lm[240];
	uint16 dcm[8][256];
	uint8 bcap[64];
	uint16 blk_idx[NBLK];
	uint8 blk[NBLK * 32];
	uint8 obj[sizeof(((struct SGFX *) 0)->OBJLines)];
};

static Slot *slots;
static unsigned prod;                 /* register side */
static std::atomic<unsigned> head, tail;
static std::atomic<int> sleeping, quit;
static s9xmt::Sem wake;
static s9xmt::Thread thr;
static bool on, started;
static std::atomic<uint64_t> busy_ns, batches, waits, wait_ns;
static int spin_idle = 3000;
static int core_hint = -1, prio_hint = 0x40;

/* the margin thread: a second reader of the slot ring. `wattached` says whether it reads (only while the wide view is on); it is changed by
 * the register side while both readers are idle and the margin thread is parked. */
static std::atomic<unsigned> wtail, wdone;
static std::atomic<int> wsleeping, wquit, wattached, wparked;
static s9xmt::Sem wwake;
static s9xmt::Thread wthr;
static bool wstarted, wreads, wmem;	/* register side: thread exists / it reads the slots posted now / its buffers exist */
static bool wwant = true;
static int wcore_hint = -1, wprio_hint = 0x40;
static uint16 *cen_pic[NCEN];
static std::atomic<uint64_t> wbusy_ns, wframes, cen_waits;
static unsigned wide_cseq;		/* register side */
static void wide_reader_update(void);
static void wide_reader_stop(void);

/* register side bookkeeping */
static uint16 dirty_list[NBLK];
static uint8 dirty_flag[NBLK];
static int dirty_n;
static bool full_resync;
static int obj_synced_from;
static uint32 colours_sent;
static unsigned frames_posted;

/* output slots for finished pictures */
static uint16 *out_pic[NOUT];
static int out_w[NOUT], out_h[NOUT];
static std::atomic<unsigned> frames_done;

/* the slot both readers have finished with (register side) */
static inline unsigned low_tail(void)
{
	unsigned t = tail.load(std::memory_order_acquire);
	if (wreads)
	{
		const unsigned w = wtail.load(std::memory_order_acquire);
		if ((int) (w - t) < 0)
			t = w;
	}
	return t;
}

static Slot &acquire(void)
{
	if (prod - low_tail() >= SLOTS)
	{
		uint64_t t0 = s9xmt::now_ns();
		while (prod - low_tail() >= SLOTS)
			s9xmt::yield_cpu();
		waits.fetch_add(1, std::memory_order_relaxed);
		wait_ns.fetch_add(s9xmt::now_ns() - t0, std::memory_order_relaxed);
	}
	Slot &s = slots[prod & MASK];
	s.wr = wreads ? 1 : 0;
	return s;
}

static void wait_idle(void);
static void release(void)
{
	prod++;
	head.store(prod, std::memory_order_seq_cst);
	if (sleeping.load(std::memory_order_seq_cst))
		wake.post();
	if (wreads && wsleeping.load(std::memory_order_seq_cst))
		wwake.post();
}

static void wait_idle(void)
{
	uint64_t t0 = 0;
	bool counted = false;
	for (int i = 0; low_tail() != prod; i++)
	{
		if (!counted) { counted = true; t0 = s9xmt::now_ns(); }
		if (i < 2000) s9xmt::cpu_relax(); else s9xmt::yield_cpu();
	}
	if (counted)
	{
		waits.fetch_add(1, std::memory_order_relaxed);
		wait_ns.fetch_add(s9xmt::now_ns() - t0, std::memory_order_relaxed);
	}
}

/* ---- worker side ------------------------------------------------------- */

static inline void invalidate(struct InternalPPU &ip, uint32 address)
{
	ip.TileCached[TILE_2BIT][address >> 4] = FALSE;
	ip.TileCached[TILE_4BIT][address >> 5] = FALSE;
	ip.TileCached[TILE_8BIT][address >> 6] = FALSE;
	ip.TileCached[TILE_2BIT_EVEN][address >> 4] = FALSE;
	ip.TileCached[TILE_2BIT_EVEN][((address >> 4) - 1) & (MAX_2BIT_TILES - 1)] = FALSE;
	ip.TileCached[TILE_2BIT_ODD] [address >> 4] = FALSE;
	ip.TileCached[TILE_2BIT_ODD] [((address >> 4) - 1) & (MAX_2BIT_TILES - 1)] = FALSE;
	ip.TileCached[TILE_4BIT_EVEN][address >> 5] = FALSE;
	ip.TileCached[TILE_4BIT_EVEN][((address >> 5) - 1) & (MAX_4BIT_TILES - 1)] = FALSE;
	ip.TileCached[TILE_4BIT_ODD] [address >> 5] = FALSE;
	ip.TileCached[TILE_4BIT_ODD] [((address >> 5) - 1) & (MAX_4BIT_TILES - 1)] = FALSE;
}

static void clear_caches(struct InternalPPU &ip)
{
	memset(ip.TileCached[TILE_2BIT], 0, MAX_2BIT_TILES);
	memset(ip.TileCached[TILE_4BIT], 0, MAX_4BIT_TILES);
	memset(ip.TileCached[TILE_8BIT], 0, MAX_8BIT_TILES);
	memset(ip.TileCached[TILE_2BIT_EVEN], 0, MAX_2BIT_TILES);
	memset(ip.TileCached[TILE_2BIT_ODD], 0, MAX_2BIT_TILES);
	memset(ip.TileCached[TILE_4BIT_EVEN], 0, MAX_4BIT_TILES);
	memset(ip.TileCached[TILE_4BIT_ODD], 0, MAX_4BIT_TILES);
}

static void alloc_caches(struct InternalPPU &ip)
{
	ip.TileCache[TILE_2BIT]       = (uint8 *) calloc(MAX_2BIT_TILES, 64);
	ip.TileCache[TILE_4BIT]       = (uint8 *) calloc(MAX_4BIT_TILES, 64);
	ip.TileCache[TILE_8BIT]       = (uint8 *) calloc(MAX_8BIT_TILES, 64);
	ip.TileCache[TILE_2BIT_EVEN]  = (uint8 *) calloc(MAX_2BIT_TILES, 64);
	ip.TileCache[TILE_2BIT_ODD]   = (uint8 *) calloc(MAX_2BIT_TILES, 64);
	ip.TileCache[TILE_4BIT_EVEN]  = (uint8 *) calloc(MAX_4BIT_TILES, 64);
	ip.TileCache[TILE_4BIT_ODD]   = (uint8 *) calloc(MAX_4BIT_TILES, 64);
	ip.TileCached[TILE_2BIT]      = (uint8 *) calloc(MAX_2BIT_TILES, 1);
	ip.TileCached[TILE_4BIT]      = (uint8 *) calloc(MAX_4BIT_TILES, 1);
	ip.TileCached[TILE_8BIT]      = (uint8 *) calloc(MAX_8BIT_TILES, 1);
	ip.TileCached[TILE_2BIT_EVEN] = (uint8 *) calloc(MAX_2BIT_TILES, 1);
	ip.TileCached[TILE_2BIT_ODD]  = (uint8 *) calloc(MAX_2BIT_TILES, 1);
	ip.TileCached[TILE_4BIT_EVEN] = (uint8 *) calloc(MAX_4BIT_TILES, 1);
	ip.TileCached[TILE_4BIT_ODD]  = (uint8 *) calloc(MAX_4BIT_TILES, 1);
}

/* everything a batch slot carries, into one renderer copy's state */
static inline void load_batch(const Slot &s, struct SPPU &ppu, struct InternalPPU &ip, struct SGFX &gfx, CMemory &mem,
	struct SLineData *ld, struct SLineMatrixData *lm, uint16 (*dcm)[256], uint8 *bcap)
{
	memcpy(&ppu, &s.ppu, sizeof(ppu));
	memcpy((uint8 *) &ip + offsetof(InternalPPU, Clip), s.ipa, IP_A);
	memcpy((uint8 *) &ip + offsetof(InternalPPU, Interlace), s.ipb, IP_B);
	memcpy((uint8 *) &ip + offsetof(InternalPPU, ScreenColors), s.ipc, IP_C);
	gfx.Screen = s.screen;
	gfx.SubScreen = s.sub;
	gfx.ZBuffer = s.z;
	gfx.SubZBuffer = s.subz;
	gfx.ZERO = s.zero;
	gfx.PPL = s.ppl;
	gfx.DoInterlace = s.dointerlace;
	gfx.FixedColour = s.fixedcolour;
	gfx.StartY = s.starty;
	gfx.EndY = s.endy;
	memcpy(gfx.OBJWidths, s.objwidths, sizeof(s.objwidths));
	memcpy(gfx.OBJVisibleTiles, s.objvis, sizeof(s.objvis));
	memcpy(mem.FillRAM + 0x2100, s.fillram, sizeof(s.fillram));
	memcpy(&ld[s.row0], s.ld, s.nrows * sizeof(s.ld[0]));
	memcpy(&lm[s.row0], s.lm, s.nrows * sizeof(s.lm[0]));
	if (s.flags & F_COLOURS)
	{
		memcpy(dcm, s.dcm, sizeof(s.dcm));
		memcpy(bcap, s.bcap, sizeof(s.bcap));
	}
	if (s.obj_from >= 0)
		memcpy(&gfx.OBJLines[s.obj_from], s.obj, (NOBJ - s.obj_from) * OBJ_ELEM);
	if (s.flags & F_FULL)
		clear_caches(ip);
	for (int i = 0; i < s.nblocks; i++)
	{
		uint32 base = (uint32) s.blk_idx[i] * 32;
		memcpy(mem.VRAM + base, s.blk + (size_t) i * 32, 32);
		invalidate(ip, base);
		invalidate(ip, base + 16);
	}
}

static void run_slot(Slot &s)
{
	switch (s.kind)
	{
	case K_FRAME_START:
		s9x_clear_z_r(s.z, s.subz);
		S9xWideRenderFrameStart_r(s.wr ? 0 : s.wide_m);		/* with the margin thread this copy draws no wide pass */
		break;

	case K_WIDE_END:
	{
		if (s.wr)
		{
			/* the margin thread makes the capture; it needs this frame's picture after this thread has gone on to the next frame */
			if (s.cseq - wdone.load(std::memory_order_acquire) >= NCEN)
			{
				cen_waits.fetch_add(1, std::memory_order_relaxed);
				for (int i = 0; s.cseq - wdone.load(std::memory_order_acquire) >= NCEN; i++)
					if (i < 2000) s9xmt::cpu_relax(); else s9xmt::yield_cpu();
			}
			uint16 *dst = cen_pic[s.cseq % NCEN];
			const int rows = (PPU_R.ScreenHeight > 0 && PPU_R.ScreenHeight <= CEN_ROWS) ? PPU_R.ScreenHeight : CEN_ROWS;	/* the capture reads the visible lines */
			for (int y = 0; y < rows; y++)
				memcpy(dst + (size_t) y * 256, s.screen + (size_t) y * MAX_SNES_WIDTH, 256 * 2);
			break;
		}
		SWideHook hook = g_s9x_wide_hook;
		if (!hook)
			break;
		struct SWideView v;
		S9xWideFillView_r(&v);
		v.vram = Memory_R.VRAM;
		v.vage = s.vage;
		v.uage = s.uage;
		v.clock = s.wclock;
		v.epoch = s.wepoch;
		v.centre = s.screen;
		v.centre_pitch = MAX_SNES_WIDTH;
		hook(&v, g_s9x_wide_user);
		break;
	}

	case K_FRAME_END:
	{
		unsigned n = frames_done.load(std::memory_order_relaxed);
		int slot = n % NOUT;
		const uint16 *src = s.screen;
		uint16 *dst = out_pic[slot];
		for (int y = 0; y < s.h; y++)
			memcpy(dst + (size_t) y * MAX_SNES_WIDTH, src + (size_t) y * MAX_SNES_WIDTH, (size_t) s.w * 2);
		out_w[slot] = s.w;
		out_h[slot] = s.h;
		frames_done.store(n + 1, std::memory_order_release);
		break;
	}

	case K_BATCH:
		load_batch(s, PPU_R, IPPU_R, GFX_R, Memory_R, LineData_R, LineMatrixData_R, DirectColourMaps_R, brightness_cap_R);
		s9x_render_batch_r(&s.job);
		break;
	}
}

/* ---- the margin thread ------------------------------------------------------
 * The wide view's passes of a batch (gfx.cpp, S9xWideBatch) read the same snapshot as the picture and write only their own buffers, so a
 * third copy of the renderer (suffix _w, state *_W) can draw them from the same slots at the same time as the PPU thread draws the picture.
 * This thread reads every slot the PPU thread reads while the wide view is on: a batch is loaded into its own state and only the wide
 * passes are drawn; at the end of the frame it makes the capture and calls the hook (the 256 columns of the picture come from the copy the
 * PPU thread put aside for it). The PPU thread's renderer then draws no wide pass at all. */
static void run_slot_w(Slot &s, unsigned t)
{
	switch (s.kind)
	{
	case K_FRAME_START:
		S9xWideRenderFrameStart_w(s.wide_m);
		break;

	case K_BATCH:
		load_batch(s, PPU_W, IPPU_W, GFX_W, Memory_W, LineData_W, LineMatrixData_W, DirectColourMaps_W, brightness_cap_W);
		s9x_wide_batch_w(&s.job);
		break;

	case K_WIDE_END:
	{
		/* the picture of this frame: the PPU thread must have finished this slot */
		for (int i = 0; (int) (tail.load(std::memory_order_acquire) - (t + 1)) < 0; i++)
			if (i < 2000) s9xmt::cpu_relax(); else s9xmt::yield_cpu();
		SWideHook hook = g_s9x_wide_hook;
		if (hook)
		{
			struct SWideView v;
			S9xWideFillView_w(&v);
			v.vram = Memory_W.VRAM;
			v.vage = s.vage;
			v.uage = s.uage;
			v.clock = s.wclock;
			v.epoch = s.wepoch;
			v.centre = cen_pic[s.cseq % NCEN];
			v.centre_pitch = 256;
			hook(&v, g_s9x_wide_user);
		}
		wdone.store(s.cseq + 1, std::memory_order_release);
		wframes.fetch_add(1, std::memory_order_relaxed);
		break;
	}

	default:
		break;
	}
}

static void wthread_main(void *)
{
	for (;;)
	{
		if (wquit.load(std::memory_order_acquire))
			break;
		if (!wattached.load(std::memory_order_acquire))
		{
			/* parked: the register side may change this copy's state now */
			wparked.store(1, std::memory_order_seq_cst);
			wsleeping.store(1, std::memory_order_seq_cst);
			if (!wattached.load(std::memory_order_seq_cst) && !wquit.load(std::memory_order_seq_cst))
				wwake.wait();
			wsleeping.store(0, std::memory_order_seq_cst);
			continue;
		}
		wparked.store(0, std::memory_order_seq_cst);
		if (!wattached.load(std::memory_order_seq_cst))
			continue;		/* detached in between: park (the register side waits for it) */
		unsigned t = wtail.load(std::memory_order_relaxed);
		if (head.load(std::memory_order_acquire) != t)
		{
			uint64_t t0 = s9xmt::now_ns();
			run_slot_w(slots[t & MASK], t);
			wtail.store(t + 1, std::memory_order_release);
			wbusy_ns.fetch_add(s9xmt::now_ns() - t0, std::memory_order_relaxed);
			continue;
		}
		int i = 0;
		for (; i < spin_idle; i++)
		{
			if (head.load(std::memory_order_acquire) != t || !wattached.load(std::memory_order_relaxed)) break;
			s9xmt::cpu_relax();
		}
		if (i < spin_idle) continue;
		wsleeping.store(1, std::memory_order_seq_cst);
		if (head.load(std::memory_order_seq_cst) != t || !wattached.load(std::memory_order_seq_cst) || wquit.load(std::memory_order_seq_cst))
		{
			wsleeping.store(0, std::memory_order_seq_cst);
			continue;
		}
		wwake.wait();
		wsleeping.store(0, std::memory_order_seq_cst);
	}
}

/* register side, between frames: the margin thread reads the slots from now on / no longer. */
static void wide_reader_set(bool want)
{
	if (want == wreads)
		return;
	wait_idle();			/* both readers have finished every slot */
	if (!want)
	{
		wattached.store(0, std::memory_order_seq_cst);
		wwake.post();
		while (!wparked.load(std::memory_order_seq_cst))
			s9xmt::yield_cpu();
		wreads = false;
		return;
	}
	if (!wmem)
	{
		Memory_W.FillRAM = (uint8 *) calloc(1, 0x10000);
		alloc_caches(IPPU_W);
		for (unsigned i = 0; i < NCEN; i++)
			cen_pic[i] = (uint16 *) calloc((size_t) 256 * CEN_ROWS, 2);
		wwake.init();
		s9x_render_setup_w();
		wmem = true;
	}
	if (!wstarted)
	{
		wquit.store(0); wattached.store(0); wparked.store(0); wsleeping.store(0);
		if (!wthr.start(wthread_main, 0, "snes_wide", wcore_hint, wprio_hint))
		{
			wwant = false;		/* no thread: the PPU thread goes on drawing the passes */
			return;
		}
		wstarted = true;
	}
	while (!wparked.load(std::memory_order_seq_cst))
		s9xmt::yield_cpu();
	/* this copy starts from what the PPU thread's copy holds now (the PPU thread is idle); the slots from here on bring both the same changes */
	uint8 *fill = Memory_W.FillRAM;
	memcpy(Memory_W.VRAM, Memory_R.VRAM, sizeof(Memory_W.VRAM));
	memcpy(fill, Memory_R.FillRAM, 0x10000);
	Memory_W.FillRAM = fill;
	{
		uint8 *tc[7], *tcd[7];
		memcpy(tc, IPPU_W.TileCache, sizeof(tc));
		memcpy(tcd, IPPU_W.TileCached, sizeof(tcd));
		memcpy(&IPPU_W, &IPPU_R, sizeof(IPPU_W));
		memcpy(IPPU_W.TileCache, tc, sizeof(tc));
		memcpy(IPPU_W.TileCached, tcd, sizeof(tcd));
		clear_caches(IPPU_W);
	}
	memcpy(&PPU_W, &PPU_R, sizeof(PPU_W));
	memcpy(GFX_W.OBJLines, GFX_R.OBJLines, sizeof(GFX_W.OBJLines));	/* the sprite lines come in parts; every other field of GFX a batch sets itself */
	memcpy(&BG_W, &BG, sizeof(BG_W));
	BG_W.Buffer = BG_W.BufferFlip = BG_W.Buffered = BG_W.BufferedFlip = NULL; /* set again by the tile selector */
	memcpy(LineData_W, LineData_R, sizeof(LineData_W));
	memcpy(LineMatrixData_W, LineMatrixData_R, sizeof(LineMatrixData_W));
	memcpy(DirectColourMaps_W, DirectColourMaps_R, sizeof(DirectColourMaps_W));
	memcpy(brightness_cap_W, brightness_cap_R, sizeof(brightness_cap_W));
	S9xWideRenderFrameStart_w(0);	/* the frame that starts now sets the margin */
	wtail.store(prod, std::memory_order_seq_cst);
	wdone.store(0, std::memory_order_seq_cst);
	wide_cseq = 0;
	wreads = true;
	wattached.store(1, std::memory_order_seq_cst);
	wwake.post();
}

static void wide_reader_stop(void)
{
	wide_reader_set(false);
	if (!wstarted)
		return;
	wquit.store(1, std::memory_order_seq_cst);
	wwake.post();
	wthr.join();
	wstarted = false;
}

/* Called at the start of every frame (register side), so a frame's wide passes are drawn wholly by one renderer copy: the margin thread reads the
 * slots of the frames whose margin is latched on (centre only frames included), and of no others. */
static inline void wide_reader_update(void)
{
	const bool want = wwant && g_s9x_wide_fm > 0;
	if (want != wreads)
		wide_reader_set(want);
}

static void thread_main(void *)
{
	for (;;)
	{
		unsigned t = tail.load(std::memory_order_relaxed);
		if (head.load(std::memory_order_acquire) != t)
		{
			uint64_t t0 = s9xmt::now_ns();
			run_slot(slots[t & MASK]);
			tail.store(t + 1, std::memory_order_release);
			batches.fetch_add(1, std::memory_order_relaxed);
			busy_ns.fetch_add(s9xmt::now_ns() - t0, std::memory_order_relaxed);
			continue;
		}
		int i = 0;
		for (; i < spin_idle; i++)
		{
			if (head.load(std::memory_order_acquire) != t) break;
			s9xmt::cpu_relax();
		}
		if (i < spin_idle) continue;
		sleeping.store(1, std::memory_order_seq_cst);
		if (head.load(std::memory_order_seq_cst) != t) { sleeping.store(0); continue; }
		if (quit.load()) { sleeping.store(0); break; }
		wake.wait();
		sleeping.store(0);
	}
}

} // namespace ppumt

using namespace ppumt;

bool S9xPPUMTActive(void) { return on; }

void S9xPPUMTInvalidateAll(void)
{
	g_s9x_wide_epoch++;	/* wide view: a reset or a state load makes every stamp and learned position meaningless */
	if (!on) return;
	wait_idle();
	full_resync = true;
}

void S9xPPUMTVRAMDirty(uint32 address)
{
	uint32 b = (address & 0xffff) >> 5;
	if (!dirty_flag[b])
	{
		dirty_flag[b] = 1;
		dirty_list[dirty_n++] = (uint16) b;
	}
}

void S9xPPUMTSync(void)
{
	if (on) wait_idle();
}

void S9xPPUMTFrameStart(void)
{
	wide_reader_update();
	Slot &s = acquire();
	s.kind = K_FRAME_START;
	s.z = GFX.ZBuffer;
	s.subz = GFX.SubZBuffer;
	s.ppl = GFX.ScreenSize;
	s.wide_m = g_s9x_wide_fco ? 0 : g_s9x_wide_fm;
	release();
}

void S9xPPUMTFrameEnd(int width, int height)
{
	/* Snes9x hands the picture over when the main loop returns, which can be later than the end
	 * of the rendered frame (the next frame may already be partly drawn). To stay bit-identical
	 * the picture is therefore snapshotted at S9xPPUMTVideo(), not here. */
	(void) width; (void) height;
	frames_posted++;
	if (g_s9x_wide_fm)
	{
		/* wide view: the stamps and the margin pictures of this frame are consumed in order by the worker */
		Slot &s = acquire();
		s.kind = K_WIDE_END;
		if (!s.vage)
		{
			s.vage = (uint16 *) malloc(32768 * 2);
			s.uage = (uint16 *) malloc(2048 * 2);
		}
		if (!g_s9x_wide_fco)
		{
			memcpy(s.vage, g_s9x_vage, 32768 * 2);
			memcpy(s.uage, g_s9x_uage, 2048 * 2);
		}
		s.wclock = g_s9x_wide_clock;
		s.wepoch = g_s9x_wide_epoch;
		s.screen = GFX.Screen;
		s.cseq = s.wr ? wide_cseq++ : 0;
		release();
	}
}

void S9xPPUMTBatch(const S9xRenderJob &job)
{
	Slot &s = acquire();
	s.kind = K_BATCH;
	s.job = job;
	s.flags = 0;

	memcpy(&s.ppu, &PPU, sizeof(PPU));
	memcpy(s.ipa, (uint8 *) &IPPU + offsetof(InternalPPU, Clip), IP_A);
	memcpy(s.ipb, (uint8 *) &IPPU + offsetof(InternalPPU, Interlace), IP_B);
	memcpy(s.ipc, (uint8 *) &IPPU + offsetof(InternalPPU, ScreenColors), IP_C);
	s.screen = GFX.Screen;
	s.sub = GFX.SubScreen;
	s.z = GFX.ZBuffer;
	s.subz = GFX.SubZBuffer;
	s.zero = GFX.ZERO;
	s.ppl = GFX.PPL;
	s.dointerlace = GFX.DoInterlace;
	s.fixedcolour = GFX.FixedColour;
	s.starty = GFX.StartY;
	s.endy = GFX.EndY;
	memcpy(s.objwidths, GFX.OBJWidths, sizeof(s.objwidths));
	memcpy(s.objvis, GFX.OBJVisibleTiles, sizeof(s.objvis));
	memcpy(s.fillram, Memory.FillRAM + 0x2100, sizeof(s.fillram));

	int r0 = (int) GFX.StartY, r1 = (int) GFX.EndY + 1;
	if (r1 > 239) r1 = 239;
	if (r1 < r0) r1 = r0;
	s.row0 = r0;
	s.nrows = r1 - r0 + 1;
	memcpy(s.ld, &LineData[r0], s.nrows * sizeof(s.ld[0]));
	memcpy(s.lm, &LineMatrixData[r0], s.nrows * sizeof(s.lm[0]));

	if (full_resync || colours_sent != g_s9x_colour_version)
	{
		s.flags |= F_COLOURS;
		memcpy(s.dcm, DirectColourMaps, sizeof(s.dcm));
		memcpy(s.bcap, brightness_cap, sizeof(s.bcap));
		colours_sent = g_s9x_colour_version;
	}

	/* sprite line table: lines from `from` on are the ones the worker may be missing */
	int from = -1;
	if (full_resync)
	{
		obj_synced_from = NOBJ;
	}
	if (g_s9x_obj_ran)
	{
		g_s9x_obj_ran = FALSE;
		from = r0;
		obj_synced_from = r0;
	}
	else if (r0 < obj_synced_from)
	{
		from = r0;
		obj_synced_from = r0;
	}
	s.obj_from = from;
	if (from >= 0)
		memcpy(s.obj, &GFX.OBJLines[from], (NOBJ - from) * OBJ_ELEM);

	if (full_resync)
	{
		s.flags |= F_FULL;
		s.nblocks = NBLK;
		for (int i = 0; i < NBLK; i++)
		{
			s.blk_idx[i] = (uint16) i;
			memcpy(s.blk + (size_t) i * 32, Memory.VRAM + (size_t) i * 32, 32);
		}
		for (int i = 0; i < dirty_n; i++) dirty_flag[dirty_list[i]] = 0;
		dirty_n = 0;
		full_resync = false;
	}
	else
	{
		s.nblocks = dirty_n;
		for (int i = 0; i < dirty_n; i++)
		{
			uint16 b = dirty_list[i];
			s.blk_idx[i] = b;
			memcpy(s.blk + (size_t) i * 32, Memory.VRAM + (size_t) b * 32, 32);
			dirty_flag[b] = 0;
		}
		dirty_n = 0;
	}
	release();
}

/* The newest finished picture (waits for the worker). */
const uint16 *S9xPPUMTVideo(int *w, int *h)
{
	{
		Slot &s = acquire();
		s.kind = K_FRAME_END;
		s.screen = GFX.Screen;
		s.w = *w;
		s.h = *h;
		release();
	}
	wait_idle();
	unsigned n = frames_done.load(std::memory_order_acquire);
	int slot = (n - 1) % NOUT;
	*w = out_w[slot];
	*h = out_h[slot];
	return out_pic[slot];
}

const uint16 *S9xPPUMTVideoNoWait(int *w, int *h, unsigned *serial)
{
	{
		Slot &s = acquire();
		s.kind = K_FRAME_END;
		s.screen = GFX.Screen;
		s.w = *w;
		s.h = *h;
		release();
	}
	unsigned n = frames_done.load(std::memory_order_acquire);
	*serial = n;
	if (!n) return NULL;
	int slot = (n - 1) % NOUT;
	*w = out_w[slot];
	*h = out_h[slot];
	return out_pic[slot];
}

void S9xMTSetSpin(int iterations)
{
	spin_idle = iterations;
	S9xAPUMTSetSpin(iterations);
}

bool S9xPPUMTSet(bool enable)
{
	if (enable == on) return on;
	if (enable)
	{
		if (!started)
		{
			slots = (Slot *) calloc(SLOTS, sizeof(Slot));
			for (int i = 0; i < NOUT; i++)
				out_pic[i] = (uint16 *) calloc((size_t) MAX_SNES_WIDTH * MAX_SNES_HEIGHT, 2);
			Memory_R.FillRAM = (uint8 *) calloc(1, 0x10000);
			alloc_caches(IPPU_R);
			wake.init();
			s9x_render_setup_r();
			started = true;
		}
		if (const char *e = getenv("SNES_MT_SPIN")) spin_idle = atoi(e);
		memcpy(&BG_R, &BG, sizeof(BG));
		BG_R.Buffer = BG_R.BufferFlip = BG_R.Buffered = BG_R.BufferedFlip = NULL; /* set again by the tile selector */
		for (int i = 0; i < NBLK; i++) dirty_flag[i] = 0;
		dirty_n = 0;
		full_resync = true;
		obj_synced_from = NOBJ;
		colours_sent = g_s9x_colour_version - 1;
		g_s9x_obj_ran = TRUE;
		prod = 0;
		head.store(0); tail.store(0); sleeping.store(0); quit.store(0);
		frames_done.store(0);
		frames_posted = 0;
		busy_ns = 0; batches = 0; waits = 0; wait_ns = 0;
		g_s9x_vram_track = TRUE;
		on = true;
		if (!thr.start(thread_main, 0, "snes_ppu", core_hint, prio_hint))
		{
			on = false;
			g_s9x_vram_track = FALSE;
		}
		wbusy_ns = 0; wframes = 0; cen_waits = 0;
	}
	else
	{
		wide_reader_stop();
		wait_idle();
		quit.store(1);
		head.store(prod, std::memory_order_seq_cst);
		wake.post();
		thr.join();
		on = false;
		g_s9x_vram_track = FALSE;
	}
	return on;
}

void S9xPPUMTConfig(int core, int prio)
{
	core_hint = core;
	prio_hint = prio;
}

void S9xPPUMTStats(uint64_t *busy, uint64_t *nbatches, uint64_t *nwaits, uint64_t *waitns)
{
	*busy = busy_ns.load(); *nbatches = batches.load(); *nwaits = waits.load(); *waitns = wait_ns.load();
}

void S9xWideMTConfig(int core, int prio)
{
	wcore_hint = core;
	wprio_hint = prio;
}

void S9xWideMTSet(bool want)
{
	wwant = want;		/* followed at the start of the next frame */
}

bool S9xWideMTActive(void) { return wreads; }

void S9xWideMTStats(uint64_t *busy, uint64_t *frames, uint64_t *centre_waits)
{
	*busy = wbusy_ns.load(); *frames = wframes.load(); *centre_waits = cen_waits.load();
}


/* ---- wide view, register side (wide.h) --------------------------------------------------------------------- */

uint32 g_s9x_wide_clock = 0;
uint32 g_s9x_wide_epoch = 1;
uint8 g_s9x_wide_stamp = 0;
uint8 g_s9x_wide_m = 0;
uint8 g_s9x_wide_fm = 0;
uint8 g_s9x_wide_co_v = 0;
uint8 g_s9x_wide_rm_v = 0;
uint8 g_s9x_wide_fco = 0;
uint8 g_s9x_wide_verify = 0;
uint16 g_s9x_vage[32768];
uint16 g_s9x_uage[2048];
SWideHook g_s9x_wide_hook = NULL;
void *g_s9x_wide_user = NULL;

void S9xWideSet(int margin, SWideHook hook, void *user)
{
	S9xPPUMTSync();			/* the worker may be inside the old hook */
	if (margin < 0) margin = 0;
	if (margin > SW_MAXM) margin = SW_MAXM;
	if (margin && !g_s9x_wide_m)
	{
		memset(g_s9x_vage, 0, sizeof(g_s9x_vage));
		memset(g_s9x_uage, 0, sizeof(g_s9x_uage));
		g_s9x_wide_epoch++;
	}
	g_s9x_wide_hook = hook;
	g_s9x_wide_user = user;
	g_s9x_wide_stamp = margin ? 1 : 0;
	g_s9x_wide_m = (uint8) margin;
	if (!margin) g_s9x_wide_fm = 0;
}

/* centre only: the PPU side skips the margin passes and the capture of the frame (the adapter's floor rung); a plain byte, read once per frame at
 * its start, so it may be written from the compose thread */
void S9xWideVerify(int on)
{
	__atomic_store_n(&g_s9x_wide_verify, (uint8) (on ? 1 : 0), __ATOMIC_RELAXED);
}

void S9xWideCentreOnly(int on)
{
	__atomic_store_n(&g_s9x_wide_co_v, (uint8) (on ? 1 : 0), __ATOMIC_RELAXED);
}

/* the cost ladder's rungs below the full one: the margin passes are drawn with a narrower margin (same latching as centre only) */
void S9xWideCoreMargin(int m)
{
	__atomic_store_n(&g_s9x_wide_rm_v, (uint8) (m < 0 ? 0 : m > SW_MAXM ? SW_MAXM : m), __ATOMIC_RELAXED);
}

void S9xWideReset(void)
{
	g_s9x_wide_epoch++;
}

void S9xWideFrameStartLive(void)
{
	g_s9x_wide_fm = g_s9x_wide_m;
	{
		const uint8 rm = __atomic_load_n(&g_s9x_wide_rm_v, __ATOMIC_RELAXED);
		if (rm && rm < g_s9x_wide_fm) g_s9x_wide_fm = rm;
	}
	g_s9x_wide_fco = g_s9x_wide_fm ? __atomic_load_n(&g_s9x_wide_co_v, __ATOMIC_RELAXED) : 0;
	if (!g_s9x_wide_fm)
		return;
	if (++g_s9x_wide_clock == 0 || (g_s9x_wide_clock & 0xffff) == 0)
		g_s9x_wide_clock++;
	if ((g_s9x_wide_clock & 0x3fff) == 0)
	{
		/* stamps older than 32768 frames become 0 ("long ago") so that the 16 bit clock cannot make them look new */
		uint16 c = (uint16) g_s9x_wide_clock;
		for (int i = 0; i < 32768; i++)
			if (g_s9x_vage[i] && (uint16) (c - g_s9x_vage[i]) >= 32768) g_s9x_vage[i] = 0;
		for (int i = 0; i < 2048; i++)
			if (g_s9x_uage[i] && (uint16) (c - g_s9x_uage[i]) >= 32768) g_s9x_uage[i] = 0;
	}
}

void S9xWideFrameEndLive(void)
{
	SWideHook hook = g_s9x_wide_hook;
	if (!hook)
		return;
	struct SWideView v;
	S9xWideFillView(&v);
	v.vram = Memory.VRAM;
	v.vage = g_s9x_vage;
	v.uage = g_s9x_uage;
	v.clock = g_s9x_wide_clock;
	v.epoch = g_s9x_wide_epoch;
	v.centre = GFX.Screen;
	v.centre_pitch = MAX_SNES_WIDTH;
	hook(&v, g_s9x_wide_user);
}
