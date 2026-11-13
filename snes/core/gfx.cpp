/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#include "snes9x.h"
#include "ppu.h"
#include "tile.h"
#include "controls.h"
#include "crosshairs.h"
#include "cheats.h"
#include "movie.h"
#include "screenshot.h"
#include "display.h"
#include "s9x_prof.h"
#include "widecap.h"

extern struct SCheatData		Cheat;
extern struct SLineData			LineData[240];
extern struct SLineMatrixData	LineMatrixData[240];

void S9xComputeClipWindows (void);

void (*S9xCustomDisplayString) (const char *, int, int, bool, int) = NULL;

static void SetupOBJ (void);
static void DrawOBJS (int);
static void DisplayTime (void);
static void DisplayFrameRate (void);
static void DisplayPressedKeys (void);
static void DisplayWatchedAddresses (void);
static void DisplayStringFromBottom (const char *, int, int, bool);
static void DrawBackground (int, uint8, uint8);
static void DrawBackgroundMosaic (int, uint8, uint8);
static void DrawBackgroundOffset (int, uint8, uint8, int);
static void DrawBackgroundOffsetMosaic (int, uint8, uint8, int);
static inline void DrawBackgroundMode7 (int, void (*DrawMath) (uint32, uint32, int), void (*DrawNomath) (uint32, uint32, int), int);
static inline void DrawBackdrop (void);
static inline void RenderScreen (bool8);
// CBOC-Vita: wide view pass (see wide.h and the end of this file). All of it is off (g_wide_on == 0) while the
// main pass draws; the hooks below only change what RenderScreen does during the wide pass.
unsigned g_wide_zdirty = ~0u;
int g_wide_on = 0;				// 1 only while the wide pass draws (also read by the Mode 7 loop in tileimpl.h)
int g_wide_dx = 0;				// screen x of buffer column 0 during the wide pass (= -margin)
static uint8 g_wmask = 0xff;			// layers the wide pass may draw (bit 4 = sprites)
static bool g_wnobackdrop = false;
static struct ClipData *g_wclip[2];
static void DrawOBJSWide (int);
static void S9xWideBatch (const S9xRenderJob &);
static uint16 get_crosshair_color (uint8);
static void S9xDisplayStringType (const char *, int, int, bool, int);

#define TILE_PLUS(t, x)	(((t) & 0xfc00) | ((t + x) & 0x3ff))


bool8 S9xGraphicsInit (void)
{
	S9xInitTileRenderer();
	memset(BlackColourMap, 0, 256 * sizeof(uint16));

	IPPU.OBJChanged = TRUE;
	Settings.BG_Forced = 0;
	Settings.ForcedBackdrop = 0;
	S9xFixColourBrightness();
	S9xBuildDirectColourMaps();

	GFX.ScreenBuffer.resize(MAX_SNES_WIDTH * (MAX_SNES_HEIGHT + 64));
	GFX.Screen = &GFX.ScreenBuffer[GFX.RealPPL * 32];
	GFX.ZERO = (uint16 *) malloc(sizeof(uint16) * 0x10000);
	GFX.SubScreen  = (uint16 *) malloc(GFX.ScreenSize * sizeof(uint16));
	GFX.ZBuffer    = (uint8 *)  malloc(GFX.ScreenSize);
	GFX.SubZBuffer = (uint8 *)  malloc(GFX.ScreenSize);
	g_s9x_zdirty = g_s9x_subzdirty = GFX.ScreenSize;	// contents unknown: the next frame clears everything

	if (!GFX.ZERO || !GFX.SubScreen || !GFX.ZBuffer || !GFX.SubZBuffer)
	{
		S9xGraphicsDeinit();
		return (FALSE);
	}

	// Lookup table for 1/2 color subtraction
	memset(GFX.ZERO, 0, 0x10000 * sizeof(uint16));
	for (uint32 r = 0; r <= MAX_RED; r++)
	{
		uint32	r2 = r;
		if (r2 & 0x10)
			r2 &= ~0x10;
		else
			r2 = 0;

		for (uint32 g = 0; g <= MAX_GREEN; g++)
		{
			uint32	g2 = g;
			if (g2 & GREEN_HI_BIT)
				g2 &= ~GREEN_HI_BIT;
			else
				g2 = 0;

			for (uint32 b = 0; b <= MAX_BLUE; b++)
			{
				uint32	b2 = b;
				if (b2 & 0x10)
					b2 &= ~0x10;
				else
					b2 = 0;

				GFX.ZERO[BUILD_PIXEL2(r, g, b)] = BUILD_PIXEL2(r2, g2, b2);
				GFX.ZERO[BUILD_PIXEL2(r, g, b) & ~ALPHA_BITS_MASK] = BUILD_PIXEL2(r2, g2, b2);
			}
		}
	}

	return (TRUE);
}

void S9xGraphicsDeinit (void)
{
	if (GFX.ZERO)       { free(GFX.ZERO);       GFX.ZERO       = NULL; }
	if (GFX.SubScreen)  { free(GFX.SubScreen);  GFX.SubScreen  = NULL; }
	if (GFX.ZBuffer)    { free(GFX.ZBuffer);    GFX.ZBuffer    = NULL; }
	if (GFX.SubZBuffer) { free(GFX.SubZBuffer); GFX.SubZBuffer = NULL; }
}

void S9xGraphicsScreenResize (void)
{
	IPPU.MaxBrightness = PPU.Brightness;

	IPPU.Interlace    = Memory.FillRAM[0x2133] & 1;
	IPPU.InterlaceOBJ = Memory.FillRAM[0x2133] & 2;
	IPPU.PseudoHires = Memory.FillRAM[0x2133] & 8;

	if (PPU.BGMode == 5 || PPU.BGMode == 6 || IPPU.PseudoHires)
	{
		IPPU.DoubleWidthPixels = TRUE;
		IPPU.RenderedScreenWidth = SNES_WIDTH << 1;
	}
	else
	{
		IPPU.DoubleWidthPixels = FALSE;
		IPPU.RenderedScreenWidth = SNES_WIDTH;
	}

	if (IPPU.Interlace)
	{
		GFX.PPL = GFX.RealPPL << 1;
		IPPU.DoubleHeightPixels = TRUE;
		IPPU.RenderedScreenHeight = PPU.ScreenHeight << 1;
		GFX.DoInterlace++;
	}
	else
	{
		GFX.PPL = GFX.RealPPL;
		IPPU.DoubleHeightPixels = FALSE;
		IPPU.RenderedScreenHeight = PPU.ScreenHeight;
	}
}

bool8 g_s9x_colours_stale = FALSE;

void S9xRefreshColours (void)
{
	if (g_s9x_colours_stale)
	{
		g_s9x_colours_stale = FALSE;
		S9xFixColourBrightness();
		S9xBuildDirectColourMaps();
	}
}

void S9xBuildDirectColourMaps (void)
{
	g_s9x_colour_version++;
	IPPU.XB = mul_brightness[PPU.Brightness];

	for (uint32 p = 0; p < 8; p++)
		for (uint32 c = 0; c < 256; c++)
			DirectColourMaps[p][c] = BUILD_PIXEL(IPPU.XB[((c & 7) << 2) | ((p & 1) << 1)], IPPU.XB[((c & 0x38) >> 1) | (p & 2)], IPPU.XB[((c & 0xc0) >> 3) | (p & 4)]);
}

// CBOC-Vita: the two depth buffers hold 244 KB each but a frame only touches the lines it rendered.
// Every render batch records how far it wrote; the next frame clears just that much (the rest is still
// zero, because nothing else writes these buffers). The sub-screen buffer is only touched when the
// batch rendered a sub-screen. The marks are shared by the main and the PPU-thread copy of this file.
void S9xClearZBuffers (uint8 *z, uint8 *subz)
{
	if (g_s9x_zdirty)
		memset(z, 0, g_s9x_zdirty);
	if (g_s9x_subzdirty)
		memset(subz, 0, g_s9x_subzdirty);
	g_s9x_zdirty = g_s9x_subzdirty = 0;
}

static inline void S9xMarkZDirty (bool sub)
{
	uint32 ext = (GFX.EndY + 1) * GFX.PPL + GFX.RealPPL;	// last row written + one row of margin
	if (ext > GFX.ScreenSize)
		ext = GFX.ScreenSize;
	if (ext > g_s9x_zdirty)
		g_s9x_zdirty = ext;
	if (sub && ext > g_s9x_subzdirty)
		g_s9x_subzdirty = ext;
}

void S9xStartScreenRefresh (void)
{
	if (GFX.DoInterlace)
		GFX.DoInterlace--;

	if (IPPU.RenderThisFrame)
	{
		if (!GFX.DoInterlace || !S9xInterlaceField())
		{
			if (!S9xInitUpdate())
			{
				IPPU.RenderThisFrame = FALSE;
				return;
			}

			S9xGraphicsScreenResize();

			IPPU.RenderedFramesCount++;
		}

		PPU.MosaicStart = 0;
		PPU.RecomputeClipWindows = TRUE;
		IPPU.PreviousLine = IPPU.CurrentLine = 0;

		S9xWideFrameStartLive();	// CBOC-Vita: wide view clock and margin of this frame (nothing when off)

		if (S9xPPUMTActive())
			S9xPPUMTFrameStart();
		else
		{
			S9xClearZBuffers(GFX.ZBuffer, GFX.SubZBuffer);
			S9xWideRenderFrameStart(g_s9x_wide_fm);
		}
	}

	if (++IPPU.FrameCount == (uint32)Memory.ROMFramesPerSecond)
	{
		IPPU.DisplayedRenderedFrameCount = IPPU.RenderedFramesCount;
		IPPU.RenderedFramesCount = 0;
		IPPU.FrameCount = 0;
	}

	if (GFX.InfoStringTimeout > 0 && --GFX.InfoStringTimeout == 0)
		GFX.InfoString.clear();

	IPPU.TotalEmulatedFrames++;
}

void S9xEndScreenRefresh (void)
{
	if (IPPU.RenderThisFrame)
	{
		FLUSH_REDRAW();

		if (!S9xPPUMTActive() && g_s9x_wide_fm)
			S9xWideFrameEndLive();	// CBOC-Vita: single thread: hand the finished frame to the wide view

		if (S9xPPUMTActive())
		{
			S9xPPUMTFrameEnd(IPPU.RenderedScreenWidth, IPPU.RenderedScreenHeight);
			if (Settings.TakeScreenshot || Settings.AutoDisplayMessages)
				S9xPPUMTSync(); // these draw into the finished picture here
		}

		if (GFX.DoInterlace && S9xInterlaceField() == 0)
		{
			S9xControlEOF();
			S9xContinueUpdate(IPPU.RenderedScreenWidth, IPPU.RenderedScreenHeight);
		}
		else
		{
			if (IPPU.ColorsChanged)
			{
				uint32 saved = PPU.CGDATA[0];
				IPPU.ColorsChanged = FALSE;
				PPU.CGDATA[0] = saved;
			}

			S9xControlEOF();

			if (Settings.TakeScreenshot)
				S9xDoScreenshot(IPPU.RenderedScreenWidth, IPPU.RenderedScreenHeight);

			if (Settings.AutoDisplayMessages)
				S9xDisplayMessages(GFX.Screen, GFX.RealPPL, IPPU.RenderedScreenWidth, IPPU.RenderedScreenHeight, 1);

			S9xDeinitUpdate(IPPU.RenderedScreenWidth, IPPU.RenderedScreenHeight);
		}
	}
	else
		S9xControlEOF();

	S9xUpdateCheatsInMemory ();

#ifdef DEBUGGER
	if (CPU.Flags & FRAME_ADVANCE_FLAG)
	{
		if (ICPU.FrameAdvanceCount)
		{
			ICPU.FrameAdvanceCount--;
			IPPU.RenderThisFrame = TRUE;
			IPPU.FrameSkip = 0;
		}
		else
		{
			CPU.Flags &= ~FRAME_ADVANCE_FLAG;
			CPU.Flags |= DEBUG_MODE_FLAG;
		}
	}
#endif

	if (CPU.SRAMModified)
	{
		if (!CPU.AutoSaveTimer)
		{
			if (!(CPU.AutoSaveTimer = Settings.AutoSaveDelay * Memory.ROMFramesPerSecond))
				CPU.SRAMModified = FALSE;
		}
		else
		{
			if (!--CPU.AutoSaveTimer)
			{
				S9xAutoSaveSRAM();
				CPU.SRAMModified = FALSE;
			}
		}
	}
}

void RenderLine (uint8 C)
{
	if (IPPU.RenderThisFrame)
	{
		LineData[C].BG[0].VOffset = PPU.BG[0].VOffset + 1;
		LineData[C].BG[0].HOffset = PPU.BG[0].HOffset;
		LineData[C].BG[1].VOffset = PPU.BG[1].VOffset + 1;
		LineData[C].BG[1].HOffset = PPU.BG[1].HOffset;

		if (PPU.BGMode == 7)
		{
			S9xPPULatchM7(PPU.M7HOFSLatch, PPU.M7HOFS, PPU.M7HOFSLatched);
			S9xPPULatchM7(PPU.M7VOFSLatch, PPU.M7VOFS, PPU.M7VOFSLatched);
			S9xPPULatchM7(PPU.CentreXLatch, PPU.CentreX, PPU.CentreXLatched);
			S9xPPULatchM7(PPU.CentreYLatch, PPU.CentreY, PPU.CentreYLatched);

			struct SLineMatrixData *p = &LineMatrixData[C];
			p->MatrixA = PPU.MatrixA;
			p->MatrixB = PPU.MatrixB;
			p->MatrixC = PPU.MatrixC;
			p->MatrixD = PPU.MatrixD;
			p->CentreX = PPU.CentreXLatch;
			p->CentreY = PPU.CentreYLatch;
			p->M7HOFS  = PPU.M7HOFSLatch;
			p->M7VOFS  = PPU.M7VOFSLatch;
		}
		else
		{
			LineData[C].BG[2].VOffset = PPU.BG[2].VOffset + 1;
			LineData[C].BG[2].HOffset = PPU.BG[2].HOffset;
			LineData[C].BG[3].VOffset = PPU.BG[3].VOffset + 1;
			LineData[C].BG[3].HOffset = PPU.BG[3].HOffset;
		}

		IPPU.CurrentLine = C + 1;
	}
	else
	{
		// if we're not rendering this frame, we still need to update this
		// XXX: Check ForceBlank? Or anything else?
		if (IPPU.OBJChanged)
			SetupOBJ();
		PPU.RangeTimeOver |= GFX.OBJLines[C].RTOFlags;
	}
}

static inline void RenderScreen (bool8 sub)
{
	uint8	BGActive;
	int		D;

	if (!sub)
	{
		GFX.S = GFX.Screen;
		if (GFX.DoInterlace && S9xInterlaceField())
			GFX.S += GFX.RealPPL;
		GFX.DB = GFX.ZBuffer;
		GFX.Clip = g_wide_on ? g_wclip[0] : IPPU.Clip[0];
		BGActive = Memory.FillRAM[0x212c] & ~Settings.BG_Forced;
		if (g_wide_on)
			BGActive &= g_wmask;
		D = 32;
	}
	else
	{
		GFX.S = GFX.SubScreen;
		GFX.DB = GFX.SubZBuffer;
		GFX.Clip = g_wide_on ? g_wclip[1] : IPPU.Clip[1];
		BGActive = Memory.FillRAM[0x212d] & ~Settings.BG_Forced;
		if (g_wide_on)
			BGActive &= g_wmask;
		D = (Memory.FillRAM[0x2130] & 2) << 4; // 'do math' depth flag
	}

	if (BGActive & 0x10)
	{
		BG.TileAddress = PPU.OBJNameBase;
		BG.NameSelect = PPU.OBJNameSelect;
		BG.EnableMath = !sub && (Memory.FillRAM[0x2131] & 0x10);
		BG.StartPalette = 128;
		S9xSelectTileConverter(4, FALSE, sub, FALSE);
		S9xSelectTileRenderers(PPU.BGMode, sub, TRUE);
		if (g_wide_on)
			DrawOBJSWide(D + 4);
		else
			DrawOBJS(D + 4);
	}

	BG.NameSelect = 0;
	S9xSelectTileRenderers(PPU.BGMode, sub, FALSE);

	#define DO_BG(n, pal, depth, hires, offset, Zh, Zl, voffoff) \
		if (BGActive & (1 << n)) \
		{ \
			BG.StartPalette = pal; \
			BG.EnableMath = !sub && (Memory.FillRAM[0x2131] & (1 << n)); \
			BG.TileSizeH = (!hires && PPU.BG[n].BGSize) ? 16 : 8; \
			BG.TileSizeV = (PPU.BG[n].BGSize) ? 16 : 8; \
			S9xSelectTileConverter(depth, hires, sub, PPU.BGMosaic[n]); \
			\
			if (offset) \
			{ \
				BG.OffsetSizeH = (!hires && PPU.BG[2].BGSize) ? 16 : 8; \
				BG.OffsetSizeV = (PPU.BG[2].BGSize) ? 16 : 8; \
				\
				if (PPU.BGMosaic[n] && (hires || PPU.Mosaic > 1)) \
					DrawBackgroundOffsetMosaic(n, D + Zh, D + Zl, voffoff); \
				else \
					DrawBackgroundOffset(n, D + Zh, D + Zl, voffoff); \
			} \
			else \
			{ \
				if (PPU.BGMosaic[n] && (hires || PPU.Mosaic > 1)) \
					DrawBackgroundMosaic(n, D + Zh, D + Zl); \
				else \
					DrawBackground(n, D + Zh, D + Zl); \
			} \
		}

	switch (PPU.BGMode)
	{
		case 0:
			DO_BG(0,  0, 2, FALSE, FALSE, 15, 11, 0);
			DO_BG(1, 32, 2, FALSE, FALSE, 14, 10, 0);
			DO_BG(2, 64, 2, FALSE, FALSE,  7,  3, 0);
			DO_BG(3, 96, 2, FALSE, FALSE,  6,  2, 0);
			break;

		case 1:
			DO_BG(0,  0, 4, FALSE, FALSE, 15, 11, 0);
			DO_BG(1,  0, 4, FALSE, FALSE, 14, 10, 0);
			DO_BG(2,  0, 2, FALSE, FALSE, (PPU.BG3Priority ? 17 : 7), 3, 0);
			break;

		case 2:
			DO_BG(0,  0, 4, FALSE, TRUE,  15,  7, 8);
			DO_BG(1,  0, 4, FALSE, TRUE,  11,  3, 8);
			break;

		case 3:
			DO_BG(0,  0, 8, FALSE, FALSE, 15,  7, 0);
			DO_BG(1,  0, 4, FALSE, FALSE, 11,  3, 0);
			break;

		case 4:
			DO_BG(0,  0, 8, FALSE, TRUE,  15,  7, 0);
			DO_BG(1,  0, 2, FALSE, TRUE,  11,  3, 0);
			break;

		case 5:
			DO_BG(0,  0, 4, TRUE,  FALSE, 15,  7, 0);
			DO_BG(1,  0, 2, TRUE,  FALSE, 11,  3, 0);
			break;

		case 6:
			DO_BG(0,  0, 4, TRUE,  TRUE,  15,  7, 8);
			break;

		case 7:
			if (BGActive & 0x01)
			{
				BG.EnableMath = !sub && (Memory.FillRAM[0x2131] & 1);
				DrawBackgroundMode7(0, GFX.DrawMode7BG1Math, GFX.DrawMode7BG1Nomath, D);
			}

			if ((Memory.FillRAM[0x2133] & 0x40) && (BGActive & 0x02))
			{
				BG.EnableMath = !sub && (Memory.FillRAM[0x2131] & 2);
				DrawBackgroundMode7(1, GFX.DrawMode7BG2Math, GFX.DrawMode7BG2Nomath, D);
			}

			break;
	}

	#undef DO_BG

	BG.EnableMath = !sub && (Memory.FillRAM[0x2131] & 0x20);

	if (!g_wnobackdrop)
		DrawBackdrop();
}

static void S9xUpdateScreen_impl (void);
void S9xUpdateScreen (void)
{
	S9XP_COUNT(PPU_FLUSH);
	S9XP_ENTER(PPU);
	S9xUpdateScreen_impl();
	S9XP_LEAVE();
}

static void S9xUpdateScreen_impl (void)
{
	S9xRefreshColours();

	if (IPPU.OBJChanged || IPPU.InterlaceOBJ)
		SetupOBJ();

	// XXX: Check ForceBlank? Or anything else?
	PPU.RangeTimeOver |= GFX.OBJLines[GFX.EndY].RTOFlags;

	GFX.StartY = IPPU.PreviousLine;
	if ((GFX.EndY = IPPU.CurrentLine - 1) >= PPU.ScreenHeight)
		GFX.EndY = PPU.ScreenHeight - 1;

	// CBOC-Vita: the state changes of this flush stay here (they feed back into
	// the register side); the pixel work is S9xRenderBatch, which can run on the
	// PPU thread from a snapshot of the state taken right after this part.
	S9xRenderJob job;
	memset(&job, 0, sizeof(job));

	if (!PPU.ForcedBlanking)
	{
		// If force blank, may as well completely skip all this. We only did
		// the OBJ because (AFAWK) the RTO flags are updated even during force-blank.

		if (PPU.RecomputeClipWindows)
		{
			S9xComputeClipWindows();
			PPU.RecomputeClipWindows = FALSE;
		}

		if (!IPPU.DoubleWidthPixels && (PPU.BGMode == 5 || PPU.BGMode == 6 || IPPU.PseudoHires))
		{
			// Have to back out of the regular speed hack
			job.widen = 1;
			job.widen_ppl = GFX.PPL;

			IPPU.DoubleWidthPixels = TRUE;
			IPPU.RenderedScreenWidth = 512;
		}

		if (!IPPU.DoubleHeightPixels && IPPU.Interlace && (PPU.BGMode == 5 || PPU.BGMode == 6))
		{
			job.heighten = 1;
			IPPU.DoubleHeightPixels = TRUE;
			IPPU.RenderedScreenHeight = PPU.ScreenHeight << 1;
			GFX.PPL = GFX.RealPPL << 1;
			GFX.DoInterlace = 2;
		}

		if ((Memory.FillRAM[0x2130] & 0x30) != 0x30 && (Memory.FillRAM[0x2131] & 0x3f))
			GFX.FixedColour = BUILD_PIXEL(IPPU.XB[PPU.FixedColourRed], IPPU.XB[PPU.FixedColourGreen], IPPU.XB[PPU.FixedColourBlue]);

		if (PPU.BGMode == 5 || PPU.BGMode == 6 || IPPU.PseudoHires ||
			((Memory.FillRAM[0x2130] & 0x30) != 0x30 && (Memory.FillRAM[0x2130] & 2) && (Memory.FillRAM[0x2131] & 0x3f) && (Memory.FillRAM[0x212d] & 0x1f)))
			// If hires (Mode 5/6 or pseudo-hires) or math is to be done
			// involving the subscreen, then we need to render the subscreen...
			job.sub = 1;
	}
	else
		job.blank = 1;

	if (S9xPPUMTActive())
		S9xPPUMTBatch(job);
	else
		S9xRenderBatch(job);

	IPPU.PreviousLine = IPPU.CurrentLine;
}

// The pixel work of one flush: draws lines GFX.StartY..GFX.EndY of the picture
// from PPU/IPPU/GFX/BG/LineData/VRAM. Touches nothing the register side reads.
void S9xRenderBatch (const S9xRenderJob &job)
{
	if (!job.blank)
	{
		if (job.widen)
		{
			// Have to back out of the regular speed hack
			for (uint32 y = 0; y < GFX.StartY; y++)
			{
				uint16	*p = GFX.Screen + y * job.widen_ppl + 255;
				uint16	*q = GFX.Screen + y * job.widen_ppl + 510;

				for (int x = 255; x >= 0; x--, p--, q -= 2)
					*q = *(q + 1) = *p;
			}
		}

		if (job.heighten)
		{
			for (int32 y = (int32) GFX.StartY - 2; y >= 0; y--)
				memmove(GFX.Screen + (y + 1) * GFX.PPL, GFX.Screen + y * GFX.RealPPL, GFX.PPL * sizeof(uint16));
		}

		S9xMarkZDirty(job.sub != 0);

		if (job.sub)
			RenderScreen(TRUE);

		RenderScreen(FALSE);
	}
	else
	{
		const uint16	black = BUILD_PIXEL(0, 0, 0);

		GFX.S = GFX.Screen + GFX.StartY * GFX.PPL;
		if (GFX.DoInterlace && S9xInterlaceField())
			GFX.S += GFX.RealPPL;

		for (uint32 l = GFX.StartY; l <= GFX.EndY; l++, GFX.S += GFX.PPL)
			for (int x = 0; x < IPPU.RenderedScreenWidth; x++)
				GFX.S[x] = black;
	}

	S9xWideBatch(job);		// CBOC-Vita: the wide view's margin pass (nothing unless the wide view is on)
}

// Entry points for the renamed copy of this file used by the PPU thread
// (see snes/core/ppu_mt.cpp): plain C names so that the build can address them.
extern "C" void s9x_render_batch (const S9xRenderJob *job) { S9xRenderBatch(*job); }
extern "C" void s9x_clear_z (uint8 *z, uint8 *subz) { S9xClearZBuffers(z, subz); }
extern "C" void s9x_render_setup (void) { S9xInitTileRenderer(); }

static void SetupOBJ (void)
{
	g_s9x_obj_ran = TRUE;
	int	SmallWidth, SmallHeight, LargeWidth, LargeHeight;

	switch (PPU.OBJSizeSelect)
	{
		case 0:
			SmallWidth = SmallHeight = 8;
			LargeWidth = LargeHeight = 16;
			break;

		case 1:
			SmallWidth = SmallHeight = 8;
			LargeWidth = LargeHeight = 32;
			break;

		case 2:
			SmallWidth = SmallHeight = 8;
			LargeWidth = LargeHeight = 64;
			break;

		case 3:
			SmallWidth = SmallHeight = 16;
			LargeWidth = LargeHeight = 32;
			break;

		case 4:
			SmallWidth = SmallHeight = 16;
			LargeWidth = LargeHeight = 64;
			break;

		case 5:
		default:
			SmallWidth = SmallHeight = 32;
			LargeWidth = LargeHeight = 64;
			break;

		case 6:
			SmallWidth = 16; SmallHeight = 32;
			LargeWidth = 32; LargeHeight = 64;
			break;

		case 7:
			SmallWidth = 16; SmallHeight = 32;
			LargeWidth = LargeHeight = 32;
			break;
	}

	int	inc = IPPU.InterlaceOBJ ? 2 : 1;

	int startline = (IPPU.InterlaceOBJ && S9xInterlaceField()) ? 1 : 0;

	// OK, we have three cases here. Either there's no priority, priority is
	// normal FirstSprite, or priority is FirstSprite+Y. The first two are
	// easy, the last is somewhat more ... interesting. So we split them up.

	int		Height;
	uint8	S;
	int sprite_limit = (Settings.MaxSpriteTilesPerLine == 128) ? 128 : 32;

	if (!PPU.OAMPriorityRotation || !(PPU.OAMFlip & PPU.OAMAddr & 1)) // normal case
	{
		uint8	LineOBJ[SNES_HEIGHT_EXTENDED];
		memset(LineOBJ, 0, sizeof(LineOBJ));

		for (int i = 0; i < SNES_HEIGHT_EXTENDED; i++)
		{
			GFX.OBJLines[i].RTOFlags = 0;
			GFX.OBJLines[i].Tiles = Settings.MaxSpriteTilesPerLine;
			for (int j = 0; j < sprite_limit; j++)
				GFX.OBJLines[i].OBJ[j].Sprite = -1;
		}

		uint8	FirstSprite = PPU.FirstSprite;
		S = FirstSprite;

		do
		{
			if (PPU.OBJ[S].Size)
			{
				GFX.OBJWidths[S] = LargeWidth;
				Height = LargeHeight;
			}
			else
			{
				GFX.OBJWidths[S] = SmallWidth;
				Height = SmallHeight;
			}

			int	HPos = PPU.OBJ[S].HPos;
			if (HPos == -256)
				HPos = 0;

			if (HPos > -GFX.OBJWidths[S] && HPos <= 256)
			{
				if (HPos < 0)
					GFX.OBJVisibleTiles[S] = (GFX.OBJWidths[S] + HPos + 7) >> 3;
				else if (HPos + GFX.OBJWidths[S] > 255)
					GFX.OBJVisibleTiles[S] = (256 - HPos + 7) >> 3;
				else
					GFX.OBJVisibleTiles[S] = GFX.OBJWidths[S] >> 3;

				for (uint8 line = startline, Y = (uint8) (PPU.OBJ[S].VPos & 0xff); line < Height; Y++, line += inc)
				{
					if (Y >= SNES_HEIGHT_EXTENDED)
						continue;

					if (LineOBJ[Y] >= sprite_limit)
					{
						GFX.OBJLines[Y].RTOFlags |= 0x40;
						continue;
					}

					GFX.OBJLines[Y].Tiles -= GFX.OBJVisibleTiles[S];
					if (GFX.OBJLines[Y].Tiles < 0)
						GFX.OBJLines[Y].RTOFlags |= 0x80;

					GFX.OBJLines[Y].OBJ[LineOBJ[Y]].Sprite = S;
					if (PPU.OBJ[S].VFlip)
						// Yes, Width not Height. It so happens that the
						// sprites with H=2*W flip as two WxW sprites.
						GFX.OBJLines[Y].OBJ[LineOBJ[Y]].Line = line ^ (GFX.OBJWidths[S] - 1);
					else
						GFX.OBJLines[Y].OBJ[LineOBJ[Y]].Line = line;

					LineOBJ[Y]++;
				}
			}

			S = (S + 1) & 0x7f;
		} while (S != FirstSprite);

		for (int Y = 1; Y < SNES_HEIGHT_EXTENDED; Y++)
			GFX.OBJLines[Y].RTOFlags |= GFX.OBJLines[Y - 1].RTOFlags;
	}
	else // evil FirstSprite+Y case
	{
		// First, find out which sprites are on which lines
		uint8 OBJOnLine[SNES_HEIGHT_EXTENDED][128];
		// memset(OBJOnLine, 0, sizeof(OBJOnLine));
		/* Hold on here, that's a lot of bytes to initialise at once!
		 * So we only initialise them per line, as needed. [Neb]
		 * Bonus: We can quickly avoid looping if a line has no OBJs.
		 */
        bool8 AnyOBJOnLine[SNES_HEIGHT_EXTENDED];
        memset(AnyOBJOnLine, FALSE, sizeof(AnyOBJOnLine)); // better

		for (S = 0; S < 128; S++)
		{
			if (PPU.OBJ[S].Size)
			{
				GFX.OBJWidths[S] = LargeWidth;
				Height = LargeHeight;
			}
			else
			{
				GFX.OBJWidths[S] = SmallWidth;
				Height = SmallHeight;
			}

			int	HPos = PPU.OBJ[S].HPos;
			if (HPos == -256)
				HPos = 256;

			if (HPos > -GFX.OBJWidths[S] && HPos <= 256)
			{
				if (HPos < 0)
					GFX.OBJVisibleTiles[S] = (GFX.OBJWidths[S] + HPos + 7) >> 3;
				else if (HPos + GFX.OBJWidths[S] >= 257)
					GFX.OBJVisibleTiles[S] = (257 - HPos + 7) >> 3;
				else
					GFX.OBJVisibleTiles[S] = GFX.OBJWidths[S] >> 3;

				for (uint8 line = startline, Y = (uint8) (PPU.OBJ[S].VPos & 0xff); line < Height; Y++, line += inc)
				{
					if (Y >= SNES_HEIGHT_EXTENDED)
						continue;

					if (!AnyOBJOnLine[Y]) {
						memset(OBJOnLine[Y], 0, sizeof(OBJOnLine[Y]));
						AnyOBJOnLine[Y] = TRUE;
					}

					if (PPU.OBJ[S].VFlip)
						// Yes, Width not Height. It so happens that the
						// sprites with H=2*W flip as two WxW sprites.
						OBJOnLine[Y][S] = (line ^ (GFX.OBJWidths[S] - 1)) | 0x80;
					else
						OBJOnLine[Y][S] = line | 0x80;
				}
			}
		}

		// Now go through and pull out those OBJ that are actually visible.
		int	j;
		for (int Y = 0; Y < SNES_HEIGHT_EXTENDED; Y++)
		{
			GFX.OBJLines[Y].RTOFlags = Y ? GFX.OBJLines[Y - 1].RTOFlags : 0;
			GFX.OBJLines[Y].Tiles = Settings.MaxSpriteTilesPerLine;

			uint8	FirstSprite = (PPU.FirstSprite + Y) & 0x7f;
			S = FirstSprite;
			j = 0;

			if (AnyOBJOnLine[Y])
			{
				do
				{
					if (OBJOnLine[Y][S])
					{
						if (j >= sprite_limit)
						{
							GFX.OBJLines[Y].RTOFlags |= 0x40;
							break;
						}

						GFX.OBJLines[Y].Tiles -= GFX.OBJVisibleTiles[S];
						if (GFX.OBJLines[Y].Tiles < 0)
							GFX.OBJLines[Y].RTOFlags |= 0x80;
						GFX.OBJLines[Y].OBJ[j].Sprite = S;
						GFX.OBJLines[Y].OBJ[j++].Line = OBJOnLine[Y][S] & ~0x80;
					}

					S = (S + 1) & 0x7f;
				} while (S != FirstSprite);
			}

			if (j < sprite_limit)
				GFX.OBJLines[Y].OBJ[j].Sprite = -1;
		}
	}

	IPPU.OBJChanged = FALSE;
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC push_options
#pragma GCC optimize ("no-tree-vrp")
#endif
static void DrawOBJS (int D)
{
	void (*DrawTile) (uint32, uint32, uint32, uint32) = NULL;
	void (*DrawClippedTile) (uint32, uint32, uint32, uint32, uint32, uint32) = NULL;

	int	PixWidth = IPPU.DoubleWidthPixels ? 2 : 1;
	BG.InterlaceLine = S9xInterlaceField() ? 8 : 0;
	GFX.Z1 = 2;
	int sprite_limit = (Settings.MaxSpriteTilesPerLine == 128) ? 128 : 32;

	for (uint32 Y = GFX.StartY, Offset = Y * GFX.PPL; Y <= GFX.EndY; Y++, Offset += GFX.PPL)
	{
		int	I = 0;
		int	tiles = GFX.OBJLines[Y].Tiles;

		for (int S = GFX.OBJLines[Y].OBJ[I].Sprite; S >= 0 && I < sprite_limit; S = GFX.OBJLines[Y].OBJ[++I].Sprite)
		{
			tiles += GFX.OBJVisibleTiles[S];
			if (tiles <= 0)
				continue;

			int	BaseTile = (((GFX.OBJLines[Y].OBJ[I].Line << 1) + (PPU.OBJ[S].Name & 0xf0)) & 0xf0) | (PPU.OBJ[S].Name & 0x100) | (PPU.OBJ[S].Palette << 10);
			int	TileX = PPU.OBJ[S].Name & 0x0f;
			int	TileLine = (GFX.OBJLines[Y].OBJ[I].Line & 7) * 8;
			int	TileInc = 1;

			if (PPU.OBJ[S].HFlip)
			{
				TileX = (TileX + (GFX.OBJWidths[S] >> 3) - 1) & 0x0f;
				BaseTile |= H_FLIP;
				TileInc = -1;
			}

			GFX.Z2 = D + PPU.OBJ[S].Priority * 4;

			int	DrawMode = 3;
			int	clip = 0, next_clip = -1000;
			int	X = PPU.OBJ[S].HPos;
			if (X == -256)
				X = 256;

			for (int t = tiles, O = Offset + X * PixWidth; X <= 256 && X < PPU.OBJ[S].HPos + GFX.OBJWidths[S]; TileX = (TileX + TileInc) & 0x0f, X += 8, O += 8 * PixWidth)
			{
				if (X < -7 || --t < 0 || X == 256)
					continue;

				for (int x = X; x < X + 8;)
				{
					if (x >= next_clip)
					{
						for (; clip < GFX.Clip[4].Count && GFX.Clip[4].Left[clip] <= x; clip++) ;
						if (clip == 0 || x >= GFX.Clip[4].Right[clip - 1])
						{
							DrawMode = 0;
							next_clip = ((clip < GFX.Clip[4].Count) ? GFX.Clip[4].Left[clip] : 1000);
						}
						else
						{
							DrawMode = GFX.Clip[4].DrawMode[clip - 1];
							next_clip = GFX.Clip[4].Right[clip - 1];
							GFX.ClipColors = !(DrawMode & 1);

							if (BG.EnableMath && (PPU.OBJ[S].Palette & 4) && (DrawMode & 2))
							{
								DrawTile = GFX.DrawTileMath;
								DrawClippedTile = GFX.DrawClippedTileMath;
							}
							else
							{
								DrawTile = GFX.DrawTileNomath;
								DrawClippedTile = GFX.DrawClippedTileNomath;
							}
						}
					}

					if (x == X && x + 8 < next_clip)
					{
						if (DrawMode)
							DrawTile(BaseTile | TileX, O, TileLine, 1);
						x += 8;
					}
					else
					{
						int	w = (next_clip <= X + 8) ? next_clip - x : X + 8 - x;
						if (DrawMode)
							DrawClippedTile(BaseTile | TileX, O, x - X, w, TileLine, 1);
						x += w;
					}
				}
			}
		}
	}
}
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC pop_options
#endif

static void DrawBackground (int bg, uint8 Zh, uint8 Zl)
{
	BG.TileAddress = PPU.BG[bg].NameBase << 1;

	uint32	Tile;
	uint16	*SC0, *SC1, *SC2, *SC3;

	SC0 = (uint16 *) &Memory.VRAM[PPU.BG[bg].SCBase << 1];
	SC1 = (PPU.BG[bg].SCSize & 1) ? SC0 + 1024 : SC0;
	if (SC1 >= (uint16 *) (Memory.VRAM + 0x10000))
		SC1 -= 0x8000;
	SC2 = (PPU.BG[bg].SCSize & 2) ? SC1 + 1024 : SC0;
	if (SC2 >= (uint16 *) (Memory.VRAM + 0x10000))
		SC2 -= 0x8000;
	SC3 = (PPU.BG[bg].SCSize & 1) ? SC2 + 1024 : SC2;
	if (SC3 >= (uint16 *) (Memory.VRAM + 0x10000))
		SC3 -= 0x8000;

	uint32	Lines;
	int		OffsetMask  = (BG.TileSizeH == 16) ? 0x3ff : 0x1ff;
	int		OffsetShift = (BG.TileSizeV == 16) ? 4 : 3;
	int		PixWidth = IPPU.DoubleWidthPixels ? 2 : 1;
	bool8	HiresInterlace = IPPU.Interlace && IPPU.DoubleWidthPixels;

	void (*DrawTile) (uint32, uint32, uint32, uint32);
	void (*DrawClippedTile) (uint32, uint32, uint32, uint32, uint32, uint32);

	for (int clip = 0; clip < GFX.Clip[bg].Count; clip++)
	{
		GFX.ClipColors = !(GFX.Clip[bg].DrawMode[clip] & 1);

		if (BG.EnableMath && (GFX.Clip[bg].DrawMode[clip] & 2))
		{
			DrawTile = GFX.DrawTileMath;
			DrawClippedTile = GFX.DrawClippedTileMath;
		}
		else
		{
			DrawTile = GFX.DrawTileNomath;
			DrawClippedTile = GFX.DrawClippedTileNomath;
		}

		for (uint32 Y = GFX.StartY; Y <= GFX.EndY; Y += Lines)
		{
			uint32	Y2 = HiresInterlace ? Y * 2 + S9xInterlaceField() : Y;
			uint32	VOffset = LineData[Y].BG[bg].VOffset + (HiresInterlace ? 1 : 0);
			uint32	HOffset = LineData[Y].BG[bg].HOffset;
			int		VirtAlign = ((Y2 + VOffset) & 7) >> (HiresInterlace ? 1 : 0);

			for (Lines = 1; Lines < GFX.LinesPerTile - VirtAlign; Lines++)
			{
				if ((VOffset != LineData[Y + Lines].BG[bg].VOffset) || (HOffset != LineData[Y + Lines].BG[bg].HOffset))
					break;
			}

			if (Y + Lines > GFX.EndY)
				Lines = GFX.EndY - Y + 1;

			VirtAlign <<= 3;

			uint32	t1, t2;
			uint32	TilemapRow = (VOffset + Y2) >> OffsetShift;
			BG.InterlaceLine = ((VOffset + Y2) & 1) << 3;

			if ((VOffset + Y2) & 8)
			{
				t1 = 16;
				t2 = 0;
			}
			else
			{
				t1 = 0;
				t2 = 16;
			}

			uint16	*b1, *b2;

			if (TilemapRow & 0x20)
			{
				b1 = SC2;
				b2 = SC3;
			}
			else
			{
				b1 = SC0;
				b2 = SC1;
			}

			b1 += (TilemapRow & 0x1f) << 5;
			b2 += (TilemapRow & 0x1f) << 5;

			uint32	Left   = GFX.Clip[bg].Left[clip];
			uint32	Right  = GFX.Clip[bg].Right[clip];
			uint32	Offset = Left * PixWidth + Y * GFX.PPL;
			uint32	HPos   = (HOffset + Left) & OffsetMask;
			uint32	HTile  = HPos >> 3;
			uint16	*t;

			if (BG.TileSizeH == 8)
			{
				if (HTile > 31)
					t = b2 + (HTile & 0x1f);
				else
					t = b1 + HTile;
			}
			else
			{
				if (HTile > 63)
					t = b2 + ((HTile >> 1) & 0x1f);
				else
					t = b1 + (HTile >> 1);
			}

			uint32	Width = Right - Left;

			if (HPos & 7)
			{
				uint32	l = HPos & 7;
				uint32	w = 8 - l;
				if (w > Width)
					w = Width;

				Offset -= l * PixWidth;
				Tile = READ_WORD(t);
				GFX.Z1 = GFX.Z2 = (Tile & 0x2000) ? Zh : Zl;

				if (BG.TileSizeV == 16)
					Tile = TILE_PLUS(Tile, ((Tile & V_FLIP) ? t2 : t1));

				if (BG.TileSizeH == 8)
				{
					DrawClippedTile(Tile, Offset, l, w, VirtAlign, Lines);
					t++;
					if (HTile == 31)
						t = b2;
					else
					if (HTile == 63)
						t = b1;
				}
				else
				{
					if (!(Tile & H_FLIP))
						DrawClippedTile(TILE_PLUS(Tile, (HTile & 1)), Offset, l, w, VirtAlign, Lines);
					else
						DrawClippedTile(TILE_PLUS(Tile, 1 - (HTile & 1)), Offset, l, w, VirtAlign, Lines);
					t += HTile & 1;
					if (HTile == 63)
						t = b2;
					else
					if (HTile == 127)
						t = b1;
				}

				HTile++;
				Offset += 8 * PixWidth;
				Width -= w;
			}

			while (Width >= 8)
			{
				Tile = READ_WORD(t);
				GFX.Z1 = GFX.Z2 = (Tile & 0x2000) ? Zh : Zl;

				if (BG.TileSizeV == 16)
					Tile = TILE_PLUS(Tile, ((Tile & V_FLIP) ? t2 : t1));

				if (BG.TileSizeH == 8)
				{
					DrawTile(Tile, Offset, VirtAlign, Lines);
					t++;
					if (HTile == 31)
						t = b2;
					else
					if (HTile == 63)
						t = b1;
				}
				else
				{
					if (!(Tile & H_FLIP))
						DrawTile(TILE_PLUS(Tile, (HTile & 1)), Offset, VirtAlign, Lines);
					else
						DrawTile(TILE_PLUS(Tile, 1 - (HTile & 1)), Offset, VirtAlign, Lines);
					t += HTile & 1;
					if (HTile == 63)
						t = b2;
					else
					if (HTile == 127)
						t = b1;
				}

				HTile++;
				Offset += 8 * PixWidth;
				Width -= 8;
			}

			if (Width)
			{
				Tile = READ_WORD(t);
				GFX.Z1 = GFX.Z2 = (Tile & 0x2000) ? Zh : Zl;

				if (BG.TileSizeV == 16)
					Tile = TILE_PLUS(Tile, ((Tile & V_FLIP) ? t2 : t1));

				if (BG.TileSizeH == 8)
					DrawClippedTile(Tile, Offset, 0, Width, VirtAlign, Lines);
				else
				{
					if (!(Tile & H_FLIP))
						DrawClippedTile(TILE_PLUS(Tile, (HTile & 1)), Offset, 0, Width, VirtAlign, Lines);
					else
						DrawClippedTile(TILE_PLUS(Tile, 1 - (HTile & 1)), Offset, 0, Width, VirtAlign, Lines);
				}
			}
		}
	}
}

static void DrawBackgroundMosaic (int bg, uint8 Zh, uint8 Zl)
{
	BG.TileAddress = PPU.BG[bg].NameBase << 1;

	uint32	Tile;
	uint16	*SC0, *SC1, *SC2, *SC3;

	SC0 = (uint16 *) &Memory.VRAM[PPU.BG[bg].SCBase << 1];
	SC1 = (PPU.BG[bg].SCSize & 1) ? SC0 + 1024 : SC0;
	if (SC1 >= (uint16 *) (Memory.VRAM + 0x10000))
		SC1 -= 0x8000;
	SC2 = (PPU.BG[bg].SCSize & 2) ? SC1 + 1024 : SC0;
	if (SC2 >= (uint16 *) (Memory.VRAM + 0x10000))
		SC2 -= 0x8000;
	SC3 = (PPU.BG[bg].SCSize & 1) ? SC2 + 1024 : SC2;
	if (SC3 >= (uint16 *) (Memory.VRAM + 0x10000))
		SC3 -= 0x8000;

	int	Lines;
	int	OffsetMask  = (BG.TileSizeH == 16) ? 0x3ff : 0x1ff;
	int	OffsetShift = (BG.TileSizeV == 16) ? 4 : 3;
	int	PixWidth = IPPU.DoubleWidthPixels ? 2 : 1;
	bool8	HiresInterlace = IPPU.Interlace && IPPU.DoubleWidthPixels;

	void (*DrawPix) (uint32, uint32, uint32, uint32, uint32, uint32);

	int	MosaicStart = ((uint32) GFX.StartY - PPU.MosaicStart) % PPU.Mosaic;

	for (int clip = 0; clip < GFX.Clip[bg].Count; clip++)
	{
		GFX.ClipColors = !(GFX.Clip[bg].DrawMode[clip] & 1);

		if (BG.EnableMath && (GFX.Clip[bg].DrawMode[clip] & 2))
			DrawPix = GFX.DrawMosaicPixelMath;
		else
			DrawPix = GFX.DrawMosaicPixelNomath;

		for (uint32 Y = GFX.StartY - MosaicStart; Y <= GFX.EndY; Y += PPU.Mosaic)
		{
			uint32	Y2 = HiresInterlace ? Y * 2 : Y;
			uint32	VOffset = LineData[Y + MosaicStart].BG[bg].VOffset + (HiresInterlace ? 1 : 0);
			uint32	HOffset = LineData[Y + MosaicStart].BG[bg].HOffset;

			Lines = PPU.Mosaic - MosaicStart;
			if (Y + MosaicStart + Lines > GFX.EndY)
				Lines = GFX.EndY - Y - MosaicStart + 1;

			int	VirtAlign = (((Y2 + VOffset) & 7) >> (HiresInterlace ? 1 : 0)) << 3;

			uint32	t1, t2;
			uint32	TilemapRow = (VOffset + Y2) >> OffsetShift;
			BG.InterlaceLine = ((VOffset + Y2) & 1) << 3;

			if ((VOffset + Y2) & 8)
			{
				t1 = 16;
				t2 = 0;
			}
			else
			{
				t1 = 0;
				t2 = 16;
			}

			uint16	*b1, *b2;

			if (TilemapRow & 0x20)
			{
				b1 = SC2;
				b2 = SC3;
			}
			else
			{
				b1 = SC0;
				b2 = SC1;
			}

			b1 += (TilemapRow & 0x1f) << 5;
			b2 += (TilemapRow & 0x1f) << 5;

			uint32	Left   = GFX.Clip[bg].Left[clip];
			uint32	Right  = GFX.Clip[bg].Right[clip];
			uint32	Offset = Left * PixWidth + (Y + MosaicStart) * GFX.PPL;
			uint32	HPos   = (HOffset + Left - (Left % PPU.Mosaic)) & OffsetMask;
			uint32	HTile  = HPos >> 3;
			uint16	*t;

			if (BG.TileSizeH == 8)
			{
				if (HTile > 31)
					t = b2 + (HTile & 0x1f);
				else
					t = b1 + HTile;
			}
			else
			{
				if (HTile > 63)
					t = b2 + ((HTile >> 1) & 0x1f);
				else
					t = b1 + (HTile >> 1);
			}

			uint32	Width = Right - Left;

			HPos &= 7;

			while (Left < Right)
			{
				uint32	w = PPU.Mosaic - (Left % PPU.Mosaic);
				if (w > Width)
					w = Width;

				Tile = READ_WORD(t);
				GFX.Z1 = GFX.Z2 = (Tile & 0x2000) ? Zh : Zl;

				if (BG.TileSizeV == 16)
					Tile = TILE_PLUS(Tile, ((Tile & V_FLIP) ? t2 : t1));

				if (BG.TileSizeH == 8)
					DrawPix(Tile, Offset, VirtAlign, HPos & 7, w, Lines);
				else
				{
					if (!(Tile & H_FLIP))
						DrawPix(TILE_PLUS(Tile, (HTile & 1)), Offset, VirtAlign, HPos & 7, w, Lines);
					else
						DrawPix(TILE_PLUS(Tile, 1 - (HTile & 1)), Offset, VirtAlign, HPos & 7, w, Lines);
				}

				HPos += PPU.Mosaic;

				while (HPos >= 8)
				{
					HPos -= 8;

					if (BG.TileSizeH == 8)
					{
						t++;
						if (HTile == 31)
							t = b2;
						else
						if (HTile == 63)
							t = b1;
					}
					else
					{
						t += HTile & 1;
						if (HTile == 63)
							t = b2;
						else
						if (HTile == 127)
							t = b1;
					}

					HTile++;
				}

				Offset += w * PixWidth;
				Width -= w;
				Left += w;
			}

			MosaicStart = 0;
		}
	}
}

static void DrawBackgroundOffset (int bg, uint8 Zh, uint8 Zl, int VOffOff)
{
	BG.TileAddress = PPU.BG[bg].NameBase << 1;

	uint32	Tile;
	uint16	*SC0, *SC1, *SC2, *SC3;
	uint16	*BPS0, *BPS1, *BPS2, *BPS3;

	BPS0 = (uint16 *) &Memory.VRAM[PPU.BG[2].SCBase << 1];
	BPS1 = (PPU.BG[2].SCSize & 1) ? BPS0 + 1024 : BPS0;
	if (BPS1 >= (uint16 *) (Memory.VRAM + 0x10000))
		BPS1 -= 0x8000;
	BPS2 = (PPU.BG[2].SCSize & 2) ? BPS1 + 1024 : BPS0;
	if (BPS2 >= (uint16 *) (Memory.VRAM + 0x10000))
		BPS2 -= 0x8000;
	BPS3 = (PPU.BG[2].SCSize & 1) ? BPS2 + 1024 : BPS2;
	if (BPS3 >= (uint16 *) (Memory.VRAM + 0x10000))
		BPS3 -= 0x8000;

	SC0 = (uint16 *) &Memory.VRAM[PPU.BG[bg].SCBase << 1];
	SC1 = (PPU.BG[bg].SCSize & 1) ? SC0 + 1024 : SC0;
	if (SC1 >= (uint16 *) (Memory.VRAM + 0x10000))
		SC1 -= 0x8000;
	SC2 = (PPU.BG[bg].SCSize & 2) ? SC1 + 1024 : SC0;
	if (SC2 >= (uint16 *) (Memory.VRAM + 0x10000))
		SC2 -= 0x8000;
	SC3 = (PPU.BG[bg].SCSize & 1) ? SC2 + 1024 : SC2;
	if (SC3 >= (uint16 *) (Memory.VRAM + 0x10000))
		SC3 -= 0x8000;

	int	OffsetMask   = (BG.TileSizeH   == 16) ? 0x3ff : 0x1ff;
	int	OffsetShift  = (BG.TileSizeV   == 16) ? 4 : 3;
	int	Offset2Mask  = (BG.OffsetSizeH == 16) ? 0x3ff : 0x1ff;
	int	Offset2Shift = (BG.OffsetSizeV == 16) ? 4 : 3;
	int	OffsetEnableMask = 0x2000 << bg;
	int	PixWidth = IPPU.DoubleWidthPixels ? 2 : 1;
	bool8	HiresInterlace = IPPU.Interlace && IPPU.DoubleWidthPixels;

	void (*DrawClippedTile) (uint32, uint32, uint32, uint32, uint32, uint32);

	for (int clip = 0; clip < GFX.Clip[bg].Count; clip++)
	{
		GFX.ClipColors = !(GFX.Clip[bg].DrawMode[clip] & 1);

		if (BG.EnableMath && (GFX.Clip[bg].DrawMode[clip] & 2))
		{
			DrawClippedTile = GFX.DrawClippedTileMath;
		}
		else
		{
			DrawClippedTile = GFX.DrawClippedTileNomath;
		}

		for (uint32 Y = GFX.StartY; Y <= GFX.EndY; Y++)
		{
			uint32	Y2 = HiresInterlace ? Y * 2 + S9xInterlaceField() : Y;
			uint32	VOff = LineData[Y].BG[2].VOffset - 1;
			uint32	HOff = LineData[Y].BG[2].HOffset;
			uint32	HOffsetRow = VOff >> Offset2Shift;
			uint32	VOffsetRow = (VOff + VOffOff) >> Offset2Shift;
			uint16	*s, *s1, *s2;

			if (HOffsetRow & 0x20)
			{
				s1 = BPS2;
				s2 = BPS3;
			}
			else
			{
				s1 = BPS0;
				s2 = BPS1;
			}

			s1 += (HOffsetRow & 0x1f) << 5;
			s2 += (HOffsetRow & 0x1f) << 5;
			s = ((VOffsetRow & 0x20) ? BPS2 : BPS0) + ((VOffsetRow & 0x1f) << 5);
			int32	VOffsetOffset = s - s1;

			uint32	Left  = GFX.Clip[bg].Left[clip];
			uint32	Right = GFX.Clip[bg].Right[clip];
			uint32	Offset = Left * PixWidth + Y * GFX.PPL;
			uint32	HScroll = LineData[Y].BG[bg].HOffset;
			bool8	left_edge = (Left < (8 - (HScroll & 7)));
			uint32	Width = Right - Left;

			while (Left < Right)
			{
				uint32	VOffset, HOffset;

				if (left_edge)
				{
					// SNES cannot do OPT for leftmost tile column
					VOffset = LineData[Y].BG[bg].VOffset;
					HOffset = HScroll;
					left_edge = FALSE;
				}
				else
				{
					int HOffTile = ((HOff + Left - 1) & Offset2Mask) >> 3;

					if (BG.OffsetSizeH == 8)
					{
						if (HOffTile > 31)
							s = s2 + (HOffTile & 0x1f);
						else
							s = s1 + HOffTile;
					}
					else
					{
						if (HOffTile > 63)
							s = s2 + ((HOffTile >> 1) & 0x1f);
						else
							s = s1 + (HOffTile >> 1);
					}

					uint16	HCellOffset = READ_WORD(s);
					uint16	VCellOffset;

					if (VOffOff)
						VCellOffset = READ_WORD(s + VOffsetOffset);
					else
					{
						if (HCellOffset & 0x8000)
						{
							VCellOffset = HCellOffset;
							HCellOffset = 0;
						}
						else
							VCellOffset = 0;
					}

					if (VCellOffset & OffsetEnableMask)
						VOffset = VCellOffset + 1;
					else
						VOffset = LineData[Y].BG[bg].VOffset;

					if (HCellOffset & OffsetEnableMask)
						HOffset = (HCellOffset & ~7) | (HScroll & 7);
					else
						HOffset = HScroll;
				}

				if (HiresInterlace)
					VOffset++;

				uint32	t1, t2;
				int		VirtAlign = (((Y2 + VOffset) & 7) >> (HiresInterlace ? 1 : 0)) << 3;
				int		TilemapRow = (VOffset + Y2) >> OffsetShift;
				BG.InterlaceLine = ((VOffset + Y2) & 1) << 3;

				if ((VOffset + Y2) & 8)
				{
					t1 = 16;
					t2 = 0;
				}
				else
				{
					t1 = 0;
					t2 = 16;
				}

				uint16	*b1, *b2;

				if (TilemapRow & 0x20)
				{
					b1 = SC2;
					b2 = SC3;
				}
				else
				{
					b1 = SC0;
					b2 = SC1;
				}

				b1 += (TilemapRow & 0x1f) << 5;
				b2 += (TilemapRow & 0x1f) << 5;

				uint32	HPos = (HOffset + Left) & OffsetMask;
				uint32	HTile = HPos >> 3;
				uint16	*t;

				if (BG.TileSizeH == 8)
				{
					if (HTile > 31)
						t = b2 + (HTile & 0x1f);
					else
						t = b1 + HTile;
				}
				else
				{
					if (HTile > 63)
						t = b2 + ((HTile >> 1) & 0x1f);
					else
						t = b1 + (HTile >> 1);
				}

				uint32	l = HPos & 7;
				uint32	w = 8 - l;
				if (w > Width)
					w = Width;

				Offset -= l * PixWidth;
				Tile = READ_WORD(t);
				GFX.Z1 = GFX.Z2 = (Tile & 0x2000) ? Zh : Zl;

				if (BG.TileSizeV == 16)
					Tile = TILE_PLUS(Tile, ((Tile & V_FLIP) ? t2 : t1));

				if (BG.TileSizeH == 8)
				{
					DrawClippedTile(Tile, Offset, l, w, VirtAlign, 1);
				}
				else
				{
					if (!(Tile & H_FLIP))
						DrawClippedTile(TILE_PLUS(Tile, (HTile & 1)), Offset, l, w, VirtAlign, 1);
					else
						DrawClippedTile(TILE_PLUS(Tile, 1 - (HTile & 1)), Offset, l, w, VirtAlign, 1);
				}

				Left += w;
				Offset += 8 * PixWidth;
				Width -= w;
			}
		}
	}
}

static void DrawBackgroundOffsetMosaic (int bg, uint8 Zh, uint8 Zl, int VOffOff)
{
	BG.TileAddress = PPU.BG[bg].NameBase << 1;

	uint32	Tile;
	uint16	*SC0, *SC1, *SC2, *SC3;
	uint16	*BPS0, *BPS1, *BPS2, *BPS3;

	BPS0 = (uint16 *) &Memory.VRAM[PPU.BG[2].SCBase << 1];
	BPS1 = (PPU.BG[2].SCSize & 1) ? BPS0 + 1024 : BPS0;
	if (BPS1 >= (uint16 *) (Memory.VRAM + 0x10000))
		BPS1 -= 0x8000;
	BPS2 = (PPU.BG[2].SCSize & 2) ? BPS1 + 1024 : BPS0;
	if (BPS2 >= (uint16 *) (Memory.VRAM + 0x10000))
		BPS2 -= 0x8000;
	BPS3 = (PPU.BG[2].SCSize & 1) ? BPS2 + 1024 : BPS2;
	if (BPS3 >= (uint16 *) (Memory.VRAM + 0x10000))
		BPS3 -= 0x8000;

	SC0 = (uint16 *) &Memory.VRAM[PPU.BG[bg].SCBase << 1];
	SC1 = (PPU.BG[bg].SCSize & 1) ? SC0 + 1024 : SC0;
	if (SC1 >= (uint16 *) (Memory.VRAM + 0x10000))
		SC1 -= 0x8000;
	SC2 = (PPU.BG[bg].SCSize & 2) ? SC1 + 1024 : SC0;
	if (SC2 >= (uint16 *) (Memory.VRAM + 0x10000))
		SC2 -= 0x8000;
	SC3 = (PPU.BG[bg].SCSize & 1) ? SC2 + 1024 : SC2;
	if (SC3 >= (uint16 *) (Memory.VRAM + 0x10000))
		SC3 -= 0x8000;

	int	Lines;
	int	OffsetMask   = (BG.TileSizeH   == 16) ? 0x3ff : 0x1ff;
	int	OffsetShift  = (BG.TileSizeV   == 16) ? 4 : 3;
	int	Offset2Shift = (BG.OffsetSizeV == 16) ? 4 : 3;
	int	OffsetEnableMask = 0x2000 << bg;
	int	PixWidth = IPPU.DoubleWidthPixels ? 2 : 1;
	bool8	HiresInterlace = IPPU.Interlace && IPPU.DoubleWidthPixels;

	void (*DrawPix) (uint32, uint32, uint32, uint32, uint32, uint32);

	int	MosaicStart = ((uint32) GFX.StartY - PPU.MosaicStart) % PPU.Mosaic;

	for (int clip = 0; clip < GFX.Clip[bg].Count; clip++)
	{
		GFX.ClipColors = !(GFX.Clip[bg].DrawMode[clip] & 1);

		if (BG.EnableMath && (GFX.Clip[bg].DrawMode[clip] & 2))
			DrawPix = GFX.DrawMosaicPixelMath;
		else
			DrawPix = GFX.DrawMosaicPixelNomath;

		for (uint32 Y = GFX.StartY - MosaicStart; Y <= GFX.EndY; Y += PPU.Mosaic)
		{
			uint32	Y2 = HiresInterlace ? Y * 2 : Y;
			uint32	VOff = LineData[Y + MosaicStart].BG[2].VOffset - 1;
			uint32	HOff = LineData[Y + MosaicStart].BG[2].HOffset;

			Lines = PPU.Mosaic - MosaicStart;
			if (Y + MosaicStart + Lines > GFX.EndY)
				Lines = GFX.EndY - Y - MosaicStart + 1;

			uint32	HOffsetRow = VOff >> Offset2Shift;
			uint32	VOffsetRow = (VOff + VOffOff) >> Offset2Shift;
			uint16	*s, *s1, *s2;

			if (HOffsetRow & 0x20)
			{
				s1 = BPS2;
				s2 = BPS3;
			}
			else
			{
				s1 = BPS0;
				s2 = BPS1;
			}

			s1 += (HOffsetRow & 0x1f) << 5;
			s2 += (HOffsetRow & 0x1f) << 5;
			s = ((VOffsetRow & 0x20) ? BPS2 : BPS0) + ((VOffsetRow & 0x1f) << 5);
			int32	VOffsetOffset = s - s1;

			uint32	Left =  GFX.Clip[bg].Left[clip];
			uint32	Right = GFX.Clip[bg].Right[clip];
			uint32	Offset = Left * PixWidth + (Y + MosaicStart) * GFX.PPL;
			uint32	HScroll = LineData[Y + MosaicStart].BG[bg].HOffset;
			uint32	Width = Right - Left;

			while (Left < Right)
			{
				uint32	VOffset, HOffset;

				if (Left < (8 - (HScroll & 7)))
				{
					// SNES cannot do OPT for leftmost tile column
					VOffset = LineData[Y + MosaicStart].BG[bg].VOffset;
					HOffset = HScroll;
				}
				else
				{
					int HOffTile = (((Left + (HScroll & 7)) - 8) + (HOff & ~7)) >> 3;

					if (BG.OffsetSizeH == 8)
					{
						if (HOffTile > 31)
							s = s2 + (HOffTile & 0x1f);
						else
							s = s1 + HOffTile;
					}
					else
					{
						if (HOffTile > 63)
							s = s2 + ((HOffTile >> 1) & 0x1f);
						else
							s = s1 + (HOffTile >> 1);
					}

					uint16	HCellOffset = READ_WORD(s);
					uint16	VCellOffset;

					if (VOffOff)
						VCellOffset = READ_WORD(s + VOffsetOffset);
					else
					{
						if (HCellOffset & 0x8000)
						{
							VCellOffset = HCellOffset;
							HCellOffset = 0;
						}
						else
							VCellOffset = 0;
					}

					if (VCellOffset & OffsetEnableMask)
						VOffset = VCellOffset + 1;
					else
						VOffset = LineData[Y + MosaicStart].BG[bg].VOffset;

					if (HCellOffset & OffsetEnableMask)
						HOffset = (HCellOffset & ~7) | (HScroll & 7);
					else
						HOffset = HScroll;
				}

				if (HiresInterlace)
					VOffset++;

				uint32	t1, t2;
				int		VirtAlign = (((Y2 + VOffset) & 7) >> (HiresInterlace ? 1 : 0)) << 3;
				int		TilemapRow = (VOffset + Y2) >> OffsetShift;
				BG.InterlaceLine = ((VOffset + Y2) & 1) << 3;

				if ((VOffset + Y2) & 8)
				{
					t1 = 16;
					t2 = 0;
				}
				else
				{
					t1 = 0;
					t2 = 16;
				}

				uint16	*b1, *b2;

				if (TilemapRow & 0x20)
				{
					b1 = SC2;
					b2 = SC3;
				}
				else
				{
					b1 = SC0;
					b2 = SC1;
				}

				b1 += (TilemapRow & 0x1f) << 5;
				b2 += (TilemapRow & 0x1f) << 5;

				uint32	HPos = (HOffset + Left - (Left % PPU.Mosaic)) & OffsetMask;
				uint32	HTile = HPos >> 3;
				uint16	*t;

				if (BG.TileSizeH == 8)
				{
					if (HTile > 31)
						t = b2 + (HTile & 0x1f);
					else
						t = b1 + HTile;
				}
				else
				{
					if (HTile > 63)
						t = b2 + ((HTile >> 1) & 0x1f);
					else
						t = b1 + (HTile >> 1);
				}

				uint32	w = PPU.Mosaic - (Left % PPU.Mosaic);
				if (w > Width)
					w = Width;

				Tile = READ_WORD(t);
				GFX.Z1 = GFX.Z2 = (Tile & 0x2000) ? Zh : Zl;

				if (BG.TileSizeV == 16)
					Tile = TILE_PLUS(Tile, ((Tile & V_FLIP) ? t2 : t1));

				if (BG.TileSizeH == 8)
					DrawPix(Tile, Offset, VirtAlign, HPos & 7, w, Lines);
				else
				{
					if (!(Tile & H_FLIP))
						DrawPix(TILE_PLUS(Tile, (HTile & 1)), Offset, VirtAlign, HPos & 7, w, Lines);
					else
					if (!(Tile & V_FLIP))
						DrawPix(TILE_PLUS(Tile, 1 - (HTile & 1)), Offset, VirtAlign, HPos & 7, w, Lines);
				}

				Left += w;
				Offset += w * PixWidth;
				Width -= w;
			}

			MosaicStart = 0;
		}
	}
}

static inline void DrawBackgroundMode7 (int bg, void (*DrawMath) (uint32, uint32, int), void (*DrawNomath) (uint32, uint32, int), int D)
{
	for (int clip = 0; clip < GFX.Clip[bg].Count; clip++)
	{
		GFX.ClipColors = !(GFX.Clip[bg].DrawMode[clip] & 1);

		if (BG.EnableMath && (GFX.Clip[bg].DrawMode[clip] & 2))
			DrawMath(GFX.Clip[bg].Left[clip], GFX.Clip[bg].Right[clip], D);
		else
			DrawNomath(GFX.Clip[bg].Left[clip], GFX.Clip[bg].Right[clip], D);
	}
}

static inline void DrawBackdrop (void)
{
	uint32	Offset = GFX.StartY * GFX.PPL;

	for (int clip = 0; clip < GFX.Clip[5].Count; clip++)
	{
		GFX.ClipColors = !(GFX.Clip[5].DrawMode[clip] & 1);

		if (BG.EnableMath && (GFX.Clip[5].DrawMode[clip] & 2))
			GFX.DrawBackdropMath(Offset, GFX.Clip[5].Left[clip], GFX.Clip[5].Right[clip]);
		else
			GFX.DrawBackdropNomath(Offset, GFX.Clip[5].Left[clip], GFX.Clip[5].Right[clip]);
	}
}

void S9xReRefresh (void)
{
	// Be careful when calling this function from the thread other than the emulation one...
	// Here it's assumed no drawing occurs from the emulation thread when Settings.Paused is TRUE.
	if (Settings.Paused)
		S9xDeinitUpdate(IPPU.RenderedScreenWidth, IPPU.RenderedScreenHeight);
}

void S9xSetInfoString (const char *string)
{
	if (Settings.InitialInfoStringTimeout > 0)
	{
		GFX.InfoString = string;
		GFX.InfoStringTimeout = Settings.InitialInfoStringTimeout;
		S9xReRefresh();
	}
}

#include "var8x10font.h"
static const int font_width = 8;
static const int font_height = 10;

static inline int CharWidth(uint8 c)
{
	return font_width - var8x10font_kern[c - 32][0] - var8x10font_kern[c - 32][1];
}

static int StringWidth(const char* str)
{
	int length = strlen(str);
	int pixcount = 0;

	if (length > 0)
		pixcount++;

	for (int i = 0; i < length; i++)
	{
		pixcount += (CharWidth(str[i]) - 1);
	}

	return pixcount;
}

static void VariableDisplayChar(int x, int y, uint8 c, bool monospace = false, int overlap = 0)
{
	int cindex = c - 32;
	int crow = cindex >> 4;
	int ccol = cindex & 15;
	int cwidth = font_width - (monospace ? 0 : (var8x10font_kern[cindex][0] + var8x10font_kern[cindex][1]));

	int	line = crow * font_height;
	int	offset = ccol * font_width + (monospace ? 0 : var8x10font_kern[cindex][0]);
	int scale = IPPU.RenderedScreenWidth / SNES_WIDTH;

	uint16* s = GFX.Screen + y * GFX.RealPPL + x * scale;

	for (int h = 0; h < font_height; h++, line++, s += GFX.RealPPL - cwidth * scale)
	{
		for (int w = 0; w < cwidth; w++, s++)
		{
			if (var8x10font[line][offset + w] == '#')
				*s = Settings.DisplayColor;
			else if (var8x10font[line][offset + w] == '.')
				*s = 0x0000;
			//            else if (!monospace && w >= overlap)
			//                *s = (*s & 0xf7de) >> 1;
			//                *s = (*s & 0xe79c) >> 2;

			if (scale > 1)
			{
				s[1] = s[0];
				s++;
			}
		}
	}
}

void S9xVariableDisplayString(const char* string, int linesFromBottom,	int pixelsFromLeft, bool allowWrap, int type)
{
	if (GFX.ScreenBuffer.empty() || IPPU.RenderedScreenWidth == 0)
		return;

	bool monospace = true;
	if (type == S9X_NO_INFO)
	{
		if (linesFromBottom <= 0)
			linesFromBottom = 1;

		if (linesFromBottom >= 5 && !Settings.DisplayPressedKeys)
		{
			if (!Settings.DisplayPressedKeys)
				linesFromBottom -= 3;
			else
				linesFromBottom -= 1;
		}

		if (pixelsFromLeft > 128)
			pixelsFromLeft = SNES_WIDTH - StringWidth(string);

		monospace = false;
	}

	int min_lines = 1;
	std::string msg(string);
	for (auto& c : msg)
		if (c == '\n')
			min_lines++;
	if (min_lines > linesFromBottom)
		linesFromBottom = min_lines;

	int dst_x = pixelsFromLeft;
	int dst_y = IPPU.RenderedScreenHeight - (font_height)*linesFromBottom;
	int len = strlen(string);

	if (IPPU.RenderedScreenHeight % 224 && !Settings.ShowOverscan)
		dst_y -= 8;
	else if (Settings.ShowOverscan)
		dst_y += 8;

	int overlap = 0;

	for (int i = 0; i < len; i++)
	{
		int cindex = (uint8)string[i] - 32;
		int char_width = font_width - (monospace ? 1 : (var8x10font_kern[cindex][0] + var8x10font_kern[cindex][1]));

		if (dst_x + char_width > SNES_WIDTH || string[i] == '\n')
		{
			if (!allowWrap)
				break;

			linesFromBottom--;
			dst_y = IPPU.RenderedScreenHeight - font_height * linesFromBottom;
			dst_x = pixelsFromLeft;

			if (dst_y >= IPPU.RenderedScreenHeight)
				break;
		}

		if (string[i] == '\n')
			continue;

		VariableDisplayChar(dst_x, dst_y, string[i], monospace, overlap);

		dst_x += char_width - 1;
		overlap = 1;
	}
}

static void DisplayStringFromBottom(const char* string, int linesFromBottom, int pixelsFromLeft, bool allowWrap)
{
	if (S9xCustomDisplayString)
	{
		S9xCustomDisplayString(string, linesFromBottom, pixelsFromLeft, allowWrap, S9X_NO_INFO);
		return;
	}

	S9xVariableDisplayString(string, linesFromBottom, pixelsFromLeft, allowWrap, S9X_NO_INFO);
}

static void S9xDisplayStringType(const char* string, int linesFromBottom, int pixelsFromLeft, bool allowWrap, int type)
{
	if (S9xCustomDisplayString)
	{
		S9xCustomDisplayString(string, linesFromBottom, pixelsFromLeft, allowWrap, type);
		return;
	}

	S9xVariableDisplayString(string, linesFromBottom, pixelsFromLeft, allowWrap, type);
}

static void DisplayTime (void)
{
	char string[10];

	time_t rawtime;
	struct tm *timeinfo;

	time (&rawtime);
	timeinfo = localtime(&rawtime);

	sprintf(string, "%02u:%02u", timeinfo->tm_hour, timeinfo->tm_min);
	S9xDisplayString(string, 0, 0, false);
}

static void DisplayFrameRate (void)
{
	char	string[10];
	static uint32 lastFrameCount = 0, calcFps = 0;
	static time_t lastTime = time(NULL);

	time_t currTime = time(NULL);
	if (lastTime != currTime) {
		if (lastFrameCount < IPPU.TotalEmulatedFrames) {
			calcFps = (IPPU.TotalEmulatedFrames - lastFrameCount) / (uint32)(currTime - lastTime);
		}
		lastTime = currTime;
		lastFrameCount = IPPU.TotalEmulatedFrames;
	}
	sprintf(string, "%u fps", calcFps);
	S9xDisplayString(string, 2, IPPU.RenderedScreenWidth - (font_width - 1) * strlen(string) - 1, false);

#ifdef DEBUGGER
	const int	len = 8;
	sprintf(string, "%02d/%02d %02d", (int) IPPU.DisplayedRenderedFrameCount, (int) Memory.ROMFramesPerSecond, (int) IPPU.FrameCount);
#else
	const int	len = 5;
	sprintf(string, "%02d/%02d",      (int) IPPU.DisplayedRenderedFrameCount, (int) Memory.ROMFramesPerSecond);
#endif

	S9xDisplayString(string, 1, IPPU.RenderedScreenWidth - (font_width - 1) * len - 1, false);
}

static void DisplayPressedKeys (void)
{
	static unsigned char	KeyMap[]   = { '0', '1', '2', 'R', 'L', 'X', 'A', 225, 224, 227, 226, 'S', 's', 'Y', 'B' };
	static int		KeyOrder[] = { 8, 10, 7, 9, 0, 6, 14, 13, 5, 1, 4, 3, 2, 11, 12 }; // < ^ > v   A B Y X  L R  S s

	enum controllers	controller;
    int					line = Settings.DisplayMovieFrame && S9xMovieActive() ? 2 : 1;
	int8				ids[4];
	char				string[255];

	for (int port = 0; port < 2; port++)
	{
		S9xGetController(port, &controller, &ids[0], &ids[1], &ids[2], &ids[3]);

		switch (controller)
		{
			case CTL_MOUSE:
			{
				uint8 buf[5];
				if (!MovieGetMouse(port, buf))
					break;
				int16 x = READ_WORD(buf);
				int16 y = READ_WORD(buf + 2);
				uint8 buttons = buf[4];
				sprintf(string, "#%d %d: (%03d,%03d) %c%c", port + 1, ids[0] + 1, x, y,
						(buttons & 0x40) ? 'L' : ' ', (buttons & 0x80) ? 'R' : ' ');
				S9xDisplayStringType(string, line++, 1, false, S9X_PRESSED_KEYS_INFO);
				break;
			}

			case CTL_SUPERSCOPE:
			{
				uint8 buf[6];
				if (!MovieGetScope(port, buf))
					break;
				int16 x = READ_WORD(buf);
				int16 y = READ_WORD(buf + 2);
				uint8 buttons = buf[4];
				sprintf(string, "#%d %d: (%03d,%03d) %c%c%c%c", port + 1, ids[0] + 1, x, y,
						(buttons & 0x80) ? 'F' : ' ', (buttons & 0x40) ? 'C' : ' ',
						(buttons & 0x20) ? 'T' : ' ', (buttons & 0x10) ? 'P' : ' ');
				S9xDisplayStringType(string, line++, 1, false, S9X_PRESSED_KEYS_INFO);
				break;
			}

			case CTL_JUSTIFIER:
			{
				uint8 buf[11];
				if (!MovieGetJustifier(port, buf))
					break;
				int16 x1 = READ_WORD(buf);
				int16 x2 = READ_WORD(buf + 2);
				int16 y1 = READ_WORD(buf + 4);
				int16 y2 = READ_WORD(buf + 6);
				uint8 buttons = buf[8];
				bool8 offscreen1 = buf[9];
				bool8 offscreen2 = buf[10];
				sprintf(string, "#%d %d: (%03d,%03d) %c%c%c / (%03d,%03d) %c%c%c", port + 1, ids[0] + 1,
						x1, y1, (buttons & 0x80) ? 'T' : ' ', (buttons & 0x20) ? 'S' : ' ', offscreen1 ? 'O' : ' ',
						x2, y2, (buttons & 0x40) ? 'T' : ' ', (buttons & 0x10) ? 'S' : ' ', offscreen2 ? 'O' : ' ');
				S9xDisplayStringType(string, line++, 1, false, S9X_PRESSED_KEYS_INFO);
				break;
			}

			case CTL_JOYPAD:
			{
				sprintf(string, "#%d %d:                  ", port + 1, ids[0] + 1);
				uint16 pad = MovieGetJoypad(ids[0]);
				for (int i = 0; i < 15; i++)
				{
					int j = KeyOrder[i];
					int mask = (1 << (j + 1));
					string[6 + i]= (pad & mask) ? KeyMap[j] : ' ';
				}

				S9xDisplayStringType(string, line++, 1, false, S9X_PRESSED_KEYS_INFO);
				break;
			}

			case CTL_MP5:
			{
				for (int n = 0; n < 4; n++)
				{
					if (ids[n] != -1)
					{
						sprintf(string, "#%d %d:                  ", port + 1, ids[n] + 1);
						uint16 pad = MovieGetJoypad(ids[n]);
						for (int i = 0; i < 15; i++)
						{
							int j = KeyOrder[i];
							int mask = (1 << (j + 1));
							string[6 + i]= (pad & mask) ? KeyMap[j] : ' ';
						}

						S9xDisplayStringType(string, line++, 1, false, S9X_PRESSED_KEYS_INFO);
					}
				}

				break;
			}

			case CTL_MACSRIFLE:
			{
				/*
				uint8 buf[6], *p = buf;
				MovieGetScope(port, buf);
				int16 x = READ_WORD(p);
				int16 y = READ_WORD(p + 2);
				uint8 buttons = buf[4];
				sprintf(string, "#%d %d: (%03d,%03d) %c%c%c%c", port, ids[0], x, y,
						(buttons & 0x80) ? 'F' : ' ', (buttons & 0x40) ? 'C' : ' ',
						(buttons & 0x20) ? 'T' : ' ', (buttons & 0x10) ? 'P' : ' ');
				S9xDisplayString(string, line++, 1, false);
				*/
				break;
			}

			case CTL_NONE:
			{
				// Display Nothing
				break;
			}
		}
	}
}

static void DisplayWatchedAddresses (void)
{
	for (unsigned int i = 0; i < sizeof(watches) / sizeof(watches[0]); i++)
	{
		if (!watches[i].on)
			break;

		int32	displayNumber = 0;
		char	buf[64];

		for (int r = 0; r < watches[i].size; r++)
			displayNumber += (Cheat.CWatchRAM[(watches[i].address - 0x7E0000) + r]) << (8 * r);

		if (watches[i].format == 1)
			sprintf(buf, "%s,%du = %u", watches[i].desc, watches[i].size, (unsigned int) displayNumber);
		else
		if (watches[i].format == 3)
			sprintf(buf, "%s,%dx = %X", watches[i].desc, watches[i].size, (unsigned int) displayNumber);
		else // signed
		{
			if (watches[i].size == 1)
				displayNumber = (int32) ((int8)  displayNumber);
			else if (watches[i].size == 2)
				displayNumber = (int32) ((int16) displayNumber);
			else if (watches[i].size == 3)
				if (displayNumber >= 8388608)
					displayNumber -= 16777216;

			sprintf(buf, "%s,%ds = %d", watches[i].desc, watches[i].size, (int) displayNumber);
		}

		S9xDisplayString(buf, 6 + i, 1, false);
	}
}

void S9xDisplayMessages (uint16 *screen, int ppl, int width, int height, int scale)
{
	if (Settings.DisplayTime)
		DisplayTime();

	if (Settings.DisplayFrameRate)
		DisplayFrameRate();

	if (Settings.DisplayWatchedAddresses)
		DisplayWatchedAddresses();

	if (Settings.DisplayPressedKeys)
		DisplayPressedKeys();

	if (Settings.DisplayMovieFrame && S9xMovieActive())
		S9xDisplayString(GFX.FrameDisplayString, 1, 1, false);

	if (!GFX.InfoString.empty())
		S9xDisplayString(GFX.InfoString.c_str(), 5, 1, true);
}

static uint16 get_crosshair_color (uint8 color)
{
	switch (color & 15)
	{
		case  0: return (BUILD_PIXEL( 0,  0,  0)); // transparent, shouldn't be used
		case  1: return (BUILD_PIXEL( 0,  0,  0)); // Black
		case  2: return (BUILD_PIXEL( 8,  8,  8)); // 25Grey
		case  3: return (BUILD_PIXEL(16, 16, 16)); // 50Grey
		case  4: return (BUILD_PIXEL(23, 23, 23)); // 75Grey
		case  5: return (BUILD_PIXEL(31, 31, 31)); // White
		case  6: return (BUILD_PIXEL(31,  0,  0)); // Red
		case  7: return (BUILD_PIXEL(31, 16,  0)); // Orange
		case  8: return (BUILD_PIXEL(31, 31,  0)); // Yellow
		case  9: return (BUILD_PIXEL( 0, 31,  0)); // Green
		case 10: return (BUILD_PIXEL( 0, 31, 31)); // Cyan
		case 11: return (BUILD_PIXEL( 0, 23, 31)); // Sky
		case 12: return (BUILD_PIXEL( 0,  0, 31)); // Blue
		case 13: return (BUILD_PIXEL(23,  0, 31)); // Violet
		case 14: return (BUILD_PIXEL(31,  0, 31)); // Magenta
		case 15: return (BUILD_PIXEL(31,  0, 16)); // Purple
	}

	return (0);
}

void S9xDrawCrosshair (const char *crosshair, uint8 fgcolor, uint8 bgcolor, int16 x, int16 y)
{
	if (!crosshair)
		return;

	int16	r, rx = 1, c, cx = 1, W = SNES_WIDTH, H = PPU.ScreenHeight;
	uint16	fg, bg;

	x -= 7;
	y -= 7;

	if (IPPU.DoubleWidthPixels)  { cx = 2; x *= 2; W *= 2; }
	if (IPPU.DoubleHeightPixels) { rx = 2; y *= 2; H *= 2; }

	fg = get_crosshair_color(fgcolor);
	bg = get_crosshair_color(bgcolor);

	uint16	*s = GFX.Screen + y * (int32)GFX.RealPPL + x;

	for (r = 0; r < 15 * rx; r++, s += GFX.RealPPL - 15 * cx)
	{
		if (y + r < 0)
		{
			s += 15 * cx;
			continue;
		}

		if (y + r >= H)
			break;

		for (c = 0; c < 15 * cx; c++, s++)
		{
			if (x + c < 0 || s < GFX.Screen)
				continue;

			if (x + c >= W)
			{
				s += 15 * cx - c;
				break;
			}

			uint8	p = crosshair[(r / rx) * 15 + (c / cx)];

			if (p == '#' && fgcolor)
				*s = (fgcolor & 0x10) ? COLOR_ADD::fn1_2(fg, *s) : fg;
			else
			if (p == '.' && bgcolor)
				*s = (bgcolor & 0x10) ? COLOR_ADD::fn1_2(*s, bg) : bg;
		}
	}
}


// ---------------------------------------------------------------------------------------------------------------
// CBOC-Vita: wide view (wide.h). A second pass of the same renderer that draws only the columns beside the
// 256 pixel picture into private buffers, after the main pass of the batch has finished. Nothing the main pass
// or the register side reads is changed: the buffers, the clip tables and the sprite list are the pass's own, and
// the scroll rows it shifts are put back. With the wide view off the only cost is one test per batch.
// ---------------------------------------------------------------------------------------------------------------

namespace {

struct WideObjEnt { uint8 spr, line, real; };

struct WideState
{
	int		m;			// margin in pixels, 0 = off
	bool	bad;			// the frame has hi-res or interlace lines: no margin at all
	bool	alloc;
	uint16	*scr[SW_NPASS];
	uint8	*z[SW_NPASS];
	uint16	*sub;
	uint8	*subz;
	struct SWideLine line[SW_ROWS];
	struct ClipData clip[2][6];
	uint16	hsave[SW_ROWS][4];
	// sprites of the margin pass
	uint8	mobj_n[SW_ROWS];
	struct WideObjEnt mobj[SW_ROWS][40];
	int		h;
};

static WideState W;

static uint32 hash_cg (void)
{
	uint32 h = 2166136261u;
	for (int i = 0; i < 256; i++)
		h = (h ^ PPU.CGDATA[i]) * 16777619u;
	return h;
}

static void wide_alloc (void)
{
	if (W.alloc)
		return;
	size_t rows = SW_ROWS + 2;
	for (int p = 0; p < SW_NPASS; p++)
	{
		W.scr[p] = (uint16 *) calloc(rows * SW_PITCH + 64, sizeof(uint16));
		W.z[p]   = (uint8 *)  calloc(rows * SW_PITCH + 64, 1);
	}
	W.sub  = (uint16 *) calloc(rows * SW_PITCH + 64, sizeof(uint16));
	W.subz = (uint8 *)  calloc(rows * SW_PITCH + 64, 1);
	W.alloc = true;
}

// The regions of one window table in buffer columns, cut to one side. A region that touches an edge of the
// 256 pixel picture continues outwards (the window state of the edge is the state of the margin).
static void wide_clip_side (int side)
{
	const int M = W.m;
	const int lo = side ? M + 256 : 0, hi = side ? 2 * M + 256 : M;
	for (int sub = 0; sub < 2; sub++)
		for (int k = 0; k < 6; k++)
		{
			const struct ClipData &s = IPPU.Clip[sub][k];
			struct ClipData &d = W.clip[sub][k];
			d.Count = 0;
			for (int i = 0; i < s.Count && d.Count < 6; i++)
			{
				int l = s.Left[i], r = s.Right[i];
				int bl = (l <= 0) ? 0 : l + M;
				int br = (r >= 256) ? 2 * M + 256 : r + M;
				if (bl < lo) bl = lo;
				if (br > hi) br = hi;
				if (bl >= br)
					continue;
				d.Left[d.Count] = (uint16) bl;
				d.Right[d.Count] = (uint16) br;
				d.DrawMode[d.Count] = s.DrawMode[i];
				d.Count++;
			}
		}
}

static void wide_obj_dims (int S, int *w, int *h)
{
	*w = GFX.OBJWidths[S];
	int sw, sh, lw, lh;
	switch (PPU.OBJSizeSelect)
	{
		case 0: sw = sh = 8; lw = lh = 16; break;
		case 1: sw = sh = 8; lw = lh = 32; break;
		case 2: sw = sh = 8; lw = lh = 64; break;
		case 3: sw = sh = 16; lw = lh = 32; break;
		case 4: sw = sh = 16; lw = lh = 64; break;
		case 6: sw = 16; sh = 32; lw = 32; lh = 64; break;
		case 7: sw = 16; sh = 32; lw = lh = 32; break;
		case 5: default: sw = sh = 32; lw = lh = 64; break;
	}
	*h = PPU.OBJ[S].Size ? lh : sh;
	(void) sw; (void) lw;
}

// Sprites that lie wholly in the left margin (the hardware's own list has only sprites that touch the picture),
// for rows y0..y1. They are drawn only on lines where the hardware's own sprite budget was not exceeded and
// they never take part in that budget.
static void wide_setup_obj (int y0, int y1)
{
	const int M = W.m;
	int sprite_limit = (Settings.MaxSpriteTilesPerLine == 128) ? 128 : 32;
	for (int y = y0; y <= y1; y++)
		W.mobj_n[y] = 0;
	if (!(Memory.FillRAM[0x212c] & 0x10))
		return;
	for (int S = 0; S < 128; S++)
	{
		int w, h;
		wide_obj_dims(S, &w, &h);
		int hp = PPU.OBJ[S].HPos;
		if (hp > -w || hp <= -w - M || hp == -256)
			continue;
		for (int y = y0; y <= y1; y++)
		{
			uint8 line = (uint8) (y - (PPU.OBJ[S].VPos & 0xff));
			if (line >= h)
				continue;
			int n = W.mobj_n[y];
			if (n >= 40)
				continue;
			W.mobj[y][n].spr = (uint8) S;
			W.mobj[y][n].line = PPU.OBJ[S].VFlip ? (uint8) (line ^ (w - 1)) : line;
			W.mobj[y][n].real = 0;
			W.mobj_n[y] = (uint8) (n + 1);
		}
	}
	(void) sprite_limit;
}

} // namespace

// One sprite on one row, in buffer columns. `tiles` is the number of on-screen tiles the hardware drew of it,
// `full` says all of them were drawn (so the tiles beyond the picture's edge may be drawn too).
static void wide_draw_sprite (int S, int OLine, int tiles, bool full, uint32 Offset, int D,
	void (*&DrawTile) (uint32, uint32, uint32, uint32), void (*&DrawClippedTile) (uint32, uint32, uint32, uint32, uint32, uint32))
{
	const int M = W.m;
	const int BW = 2 * M + 256;
	int	BaseTile = (((OLine << 1) + (PPU.OBJ[S].Name & 0xf0)) & 0xf0) | (PPU.OBJ[S].Name & 0x100) | (PPU.OBJ[S].Palette << 10);
	int	TileX = PPU.OBJ[S].Name & 0x0f;
	int	TileLine = (OLine & 7) * 8;
	int	TileInc = 1;

	if (PPU.OBJ[S].HFlip)
	{
		TileX = (TileX + (GFX.OBJWidths[S] >> 3) - 1) & 0x0f;
		BaseTile |= H_FLIP;
		TileInc = -1;
	}

	GFX.Z2 = D + PPU.OBJ[S].Priority * 4;

	int	DrawMode = 3;
	int	clip = 0, next_clip = -1000;
	int	HP = PPU.OBJ[S].HPos;
	int	X = HP + M;
	int	end = HP + M + GFX.OBJWidths[S];

	for (int t = tiles, O = Offset + X; X < BW && X < end; TileX = (TileX + TileInc) & 0x0f, X += 8, O += 8)
	{
		int sx = X - M;
		if (sx < -7 || sx >= 256)
		{
			// a tile the hardware never drew
			if (!full || X < -7)
				continue;
		}
		else if (--t < 0)
			continue;
		if (sx >= 0 && sx + 8 <= 256)
			continue;	// wholly inside the picture: the main pass drew it

		for (int x = X; x < X + 8;)
		{
			if (x >= next_clip)
			{
				for (; clip < GFX.Clip[4].Count && GFX.Clip[4].Left[clip] <= x; clip++) ;
				if (clip == 0 || x >= GFX.Clip[4].Right[clip - 1])
				{
					DrawMode = 0;
					next_clip = ((clip < GFX.Clip[4].Count) ? GFX.Clip[4].Left[clip] : 1000);
				}
				else
				{
					DrawMode = GFX.Clip[4].DrawMode[clip - 1];
					next_clip = GFX.Clip[4].Right[clip - 1];
					GFX.ClipColors = !(DrawMode & 1);

					if (BG.EnableMath && (PPU.OBJ[S].Palette & 4) && (DrawMode & 2))
					{
						DrawTile = GFX.DrawTileMath;
						DrawClippedTile = GFX.DrawClippedTileMath;
					}
					else
					{
						DrawTile = GFX.DrawTileNomath;
						DrawClippedTile = GFX.DrawClippedTileNomath;
					}
				}
			}

			if (x == X && x + 8 < next_clip && x >= 0)
			{
				if (DrawMode)
					DrawTile(BaseTile | TileX, O, TileLine, 1);
				x += 8;
			}
			else
			{
				int	w = (next_clip <= X + 8) ? next_clip - x : X + 8 - x;
				if (w <= 0)
					w = 1;
				if (DrawMode && x >= X)
					DrawClippedTile(BaseTile | TileX, O, x - X, w, TileLine, 1);
				x += w;
			}
		}
	}
}

static void DrawOBJSWide (int D)
{
	void (*DrawTile) (uint32, uint32, uint32, uint32) = NULL;
	void (*DrawClippedTile) (uint32, uint32, uint32, uint32, uint32, uint32) = NULL;

	BG.InterlaceLine = 0;
	GFX.Z1 = 2;
	int sprite_limit = (Settings.MaxSpriteTilesPerLine == 128) ? 128 : 32;

	for (uint32 Y = GFX.StartY, Offset = Y * GFX.PPL; Y <= GFX.EndY; Y++, Offset += GFX.PPL)
	{
		int	I = 0;
		int	tiles = GFX.OBJLines[Y].Tiles;

		// the hardware's sprites (they touch the picture): the same budget as DrawOBJS, and the tiles beyond the picture's
		// edges only when the sprite was drawn whole
		for (int S = GFX.OBJLines[Y].OBJ[I].Sprite; S >= 0 && I < sprite_limit; S = GFX.OBJLines[Y].OBJ[++I].Sprite)
		{
			tiles += GFX.OBJVisibleTiles[S];
			if (tiles <= 0)
				continue;
			if (PPU.OBJ[S].HPos == -256)
				continue;
			int hp = PPU.OBJ[S].HPos;
			if (hp >= 0 && hp + GFX.OBJWidths[S] <= 256)
				continue;	// no part beyond the edges
			wide_draw_sprite(S, GFX.OBJLines[Y].OBJ[I].Line, tiles, tiles >= GFX.OBJVisibleTiles[S], Offset, D, DrawTile, DrawClippedTile);
		}

		// sprites wholly in the left margin: only when this line had no time over (budget never exceeded)
		if (GFX.OBJLines[Y].Tiles >= 0 && I < sprite_limit)
			for (int k = 0; k < W.mobj_n[Y]; k++)
				wide_draw_sprite(W.mobj[Y][k].spr, W.mobj[Y][k].line, 1 << 20, true, Offset, D, DrawTile, DrawClippedTile);
	}
}

extern "C" void S9xWideRenderFrameStart (int margin)
{
	W.m = margin;
	W.bad = false;
	if (!margin)
		return;
	if (margin > SW_MAXM)
		W.m = margin = SW_MAXM;
	wide_alloc();
	memset(W.line, 0, sizeof(W.line));
	// only the Z buffers a pass drew into last frame are dirty (an unused pass costs nothing)
	for (int p = 0; p < SW_NPASS; p++)
		if (g_wide_zdirty & (1u << p))
			memset(W.z[p], 0, (size_t) (SW_ROWS + 2) * SW_PITCH);
	if (g_wide_zdirty)
		memset(W.subz, 0, (size_t) (SW_ROWS + 2) * SW_PITCH);
	g_wide_zdirty = 0;
	W.h = 0;
}

static void wide_pass (const S9xRenderJob &job, int pass, uint8 mask, bool composite)
{
	const int M = W.m;
	uint16 *sS = GFX.Screen, *sSub = GFX.SubScreen;
	uint8 *sZ = GFX.ZBuffer, *sSubZ = GFX.SubZBuffer;
	uint8 save2131 = Memory.FillRAM[0x2131];

	GFX.Screen = W.scr[pass];
	g_wide_zdirty |= 1u << pass;
	GFX.ZBuffer = W.z[pass];
	GFX.SubScreen = W.sub;
	GFX.SubZBuffer = W.subz;
	g_wide_on = 1;
	g_wide_dx = -M;
	g_wmask = mask;
	g_wnobackdrop = !composite;
	if (!composite)
		Memory.FillRAM[0x2131] = 0;		// layers alone: no colour math (their margin is blended by the adapter)

	for (int side = 0; side < 2; side++)
	{
		wide_clip_side(side);
		g_wclip[0] = W.clip[0];
		g_wclip[1] = W.clip[1];
		if (composite && job.sub)
			RenderScreen(TRUE);
		RenderScreen(FALSE);
	}

	Memory.FillRAM[0x2131] = save2131;
	g_wide_on = 0;
	g_wide_dx = 0;
	g_wmask = 0xff;
	g_wnobackdrop = false;
	GFX.Screen = sS; GFX.SubScreen = sSub; GFX.ZBuffer = sZ; GFX.SubZBuffer = sSubZ;
}

static void S9xWideBatch (const S9xRenderJob &job)
{
	if (!W.m)
		return;

	const int y0 = (int) GFX.StartY;
	int y1 = (int) GFX.EndY;
	if (y1 >= SW_ROWS)
		y1 = SW_ROWS - 1;
	if (y0 > y1)
		return;
#ifdef CBOC_WIDE_NEGATIVE_CONTROL
	// test only (never in a release): a deliberate emulation change from the wide pass, to show that wide_compare.py notices
	if (y0 <= 100 && y1 >= 100) Memory.VRAM[0x2345] ^= 1;
#endif

	const bool hires = IPPU.DoubleWidthPixels || job.widen || PPU.BGMode == 5 || PPU.BGMode == 6 || IPPU.PseudoHires;
	const bool inter = IPPU.Interlace || IPPU.DoubleHeightPixels || job.heighten;
	const uint8 tm = Memory.FillRAM[0x212c] & ~Settings.BG_Forced, ts = Memory.FillRAM[0x212d];
	const int mode = PPU.BGMode;
	bool mosaic = false;
	for (int n = 0; n < 4; n++)
		if (PPU.BGMosaic[n] && PPU.Mosaic > 1 && ((tm | ts) & (1 << n)))
			mosaic = true;
	const bool opt = (mode == 2 || mode == 4 || mode == 6);
	const uint32 cgs = hash_cg();

	uint8 flags = SWL_VALID;
	if (job.blank || PPU.ForcedBlanking) flags |= SWL_BLANK;
	if (hires) flags |= SWL_HIRES | SWL_NOMARGIN;
	if (inter) flags |= SWL_NOMARGIN;
	if (opt) flags |= SWL_OPT | SWL_NOMARGIN;
	if (mosaic) flags |= SWL_MOSAIC | SWL_NOMARGIN;
	if (mode == 7) flags |= SWL_MODE7;
	if (hires || inter)
		W.bad = true;
	if (y1 + 1 > W.h)
		W.h = y1 + 1;

	for (int y = y0; y <= y1; y++)
	{
		struct SWideLine &L = W.line[y];
		memset(&L, 0, sizeof(L));
		L.flags = flags;
		L.mode = (uint8) mode;
		L.bg3prio = PPU.BG3Priority;
		L.bright = PPU.ForcedBlanking ? 0 : PPU.Brightness;
		L.tm = tm; L.ts = ts;
		L.cgwsel = Memory.FillRAM[0x2130]; L.cgadsub = Memory.FillRAM[0x2131];
		L.fixr = PPU.FixedColourRed; L.fixg = PPU.FixedColourGreen; L.fixb = PPU.FixedColourBlue;
		L.mosaic = mosaic ? PPU.Mosaic : 1;
		L.extbg = (Memory.FillRAM[0x2133] & 0x40) ? 1 : 0;
		for (int n = 0; n < 4; n++)
		{
			L.bgsize[n] = PPU.BG[n].BGSize;
			L.scsize[n] = (uint8) PPU.BG[n].SCSize;
			L.scbase[n] = PPU.BG[n].SCBase;
			L.name[n] = PPU.BG[n].NameBase;
			L.hofs[n] = LineData[y].BG[n].HOffset;
			L.vofs[n] = LineData[y].BG[n].VOffset;
		}
		L.backdrop = IPPU.ScreenColors[0];
		L.cgsum = cgs;
		if (mode == 7)
		{
			const struct SLineMatrixData &m = LineMatrixData[y];
			L.m7[0] = m.MatrixA; L.m7[1] = m.MatrixB; L.m7[2] = m.MatrixC; L.m7[3] = m.MatrixD;
			L.m7[4] = m.CentreX; L.m7[5] = m.CentreY; L.m7[6] = m.M7HOFS; L.m7[7] = m.M7VOFS;
			L.m7flags = (PPU.Mode7HFlip ? 1 : 0) | (PPU.Mode7VFlip ? 2 : 0) | (PPU.Mode7Repeat << 2);
		}
		L.obj_size = PPU.OBJSizeSelect; L.obj_prio_rot = PPU.OAMPriorityRotation;
		L.obj_base = PPU.OBJNameBase; L.obj_gap = PPU.OBJNameSelect;
		for (int k = 0; k < 6; k++)
			if (k < 5 && (tm & (1 << k)) && (IPPU.Clip[0][k].Count != 1 || IPPU.Clip[0][k].Left[0] != 0 || IPPU.Clip[0][k].Right[0] != 256 || IPPU.Clip[0][k].DrawMode[0] != 3))
				L.wsel_any = 1;
	}

	if (job.blank || PPU.ForcedBlanking || (flags & SWL_NOMARGIN))
		return;

	// the pass: scroll rows shifted so that buffer column x is screen column x - M
	for (int y = y0; y <= y1; y++)
		for (int n = 0; n < 4; n++)
		{
			W.hsave[y][n] = LineData[y].BG[n].HOffset;
			LineData[y].BG[n].HOffset = (uint16) (LineData[y].BG[n].HOffset - W.m);
		}
	wide_setup_obj(y0, y1);

	uint8 drawn = 1 << SWP_COMPOSITE;
	wide_pass(job, SWP_COMPOSITE, 0xff, true);
	if (mode != 7)
	{
		if (tm & 0x10)
		{
			wide_pass(job, SWP_OBJ, 0x10, false);
			drawn |= 1 << SWP_OBJ;
		}
		int nbg = (mode == 0) ? 4 : (mode == 1) ? 3 : 2;
		for (int n = 0; n < nbg; n++)
			if (tm & (1 << n))
			{
				wide_pass(job, SWP_BG0 + n, 1 << n, false);
				drawn |= 1 << (SWP_BG0 + n);
			}
	}
	for (int y = y0; y <= y1; y++)
	{
		for (int n = 0; n < 4; n++)
			LineData[y].BG[n].HOffset = W.hsave[y][n];
		W.line[y].pass = drawn;
		W.line[y].flags |= SWL_DRAWN;
	}
}

extern "C" void S9xWideFillView (struct SWideView *v)
{
	memset(v, 0, sizeof(*v));
	v->m = W.m;
	v->h = W.h ? W.h : PPU.ScreenHeight;
	v->ok = !W.bad;
	v->line = W.line;
	for (int p = 0; p < SW_NPASS; p++)
	{
		v->pass_px[p] = W.scr[p];
		v->pass_z[p] = W.z[p];
	}
	v->cgram = PPU.CGDATA;
	v->colors = IPPU.ScreenColors;
	v->oam = PPU.OAMData;
	v->rendered_w = IPPU.RenderedScreenWidth;
}
