/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#ifndef _GFX_H_
#define _GFX_H_

#include "port.h"
#include <vector>

struct SGFX
{
	const uint32 Pitch = sizeof(uint16) * MAX_SNES_WIDTH;
	const uint32 RealPPL = MAX_SNES_WIDTH; // true PPL of Screen buffer
	const uint32 ScreenSize =  MAX_SNES_WIDTH * MAX_SNES_HEIGHT;
	std::vector<uint16> ScreenBuffer;
	uint16	*Screen;
	uint16	*SubScreen;
	uint8	*ZBuffer;
	uint8	*SubZBuffer;
	uint16	*S;
	uint8	*DB;
	uint16	*ZERO;
	uint32	PPL;				// number of pixels on each of Screen buffer
	uint32	LinesPerTile;		// number of lines in 1 tile (4 or 8 due to interlace)
	uint16	*ScreenColors;		// screen colors for rendering main
	uint16	*RealScreenColors;	// screen colors, ignoring color window clipping
	uint8	Z1;					// depth for comparison
	uint8	Z2;					// depth to save
	uint32	FixedColour;
	uint8	DoInterlace;
	uint32	StartY;
	uint32	EndY;
	bool8	ClipColors;
	uint8	OBJWidths[128];
	uint8	OBJVisibleTiles[128];

	struct ClipData	*Clip;

	struct
	{
		uint8	RTOFlags;
		int16	Tiles;

		struct
		{
			int8	Sprite;
			uint8	Line;
		}	OBJ[128];
	}	OBJLines[SNES_HEIGHT_EXTENDED];

	void	(*DrawBackdropMath) (uint32, uint32, uint32);
	void	(*DrawBackdropNomath) (uint32, uint32, uint32);
	void	(*DrawTileMath) (uint32, uint32, uint32, uint32);
	void	(*DrawTileNomath) (uint32, uint32, uint32, uint32);
	void	(*DrawClippedTileMath) (uint32, uint32, uint32, uint32, uint32, uint32);
	void	(*DrawClippedTileNomath) (uint32, uint32, uint32, uint32, uint32, uint32);
	void	(*DrawMosaicPixelMath) (uint32, uint32, uint32, uint32, uint32, uint32);
	void	(*DrawMosaicPixelNomath) (uint32, uint32, uint32, uint32, uint32, uint32);
	void	(*DrawMode7BG1Math) (uint32, uint32, int);
	void	(*DrawMode7BG1Nomath) (uint32, uint32, int);
	void	(*DrawMode7BG2Math) (uint32, uint32, int);
	void	(*DrawMode7BG2Nomath) (uint32, uint32, int);

	std::string InfoString;
	uint32	InfoStringTimeout;
	char	FrameDisplayString[256];
};

struct SBG
{
	uint8	(*ConvertTile) (uint8 *, uint32, uint32);
	uint8	(*ConvertTileFlip) (uint8 *, uint32, uint32);

	uint32	TileSizeH;
	uint32	TileSizeV;
	uint32	OffsetSizeH;
	uint32	OffsetSizeV;
	uint32	TileShift;
	uint32	TileAddress;
	uint32	NameSelect;
	uint32	SCBase;

	uint32	StartPalette;
	uint32	PaletteShift;
	uint32	PaletteMask;
	uint8	EnableMath;
	uint8	InterlaceLine;

	uint8	*Buffer;
	uint8	*BufferFlip;
	uint8	*Buffered;
	uint8	*BufferedFlip;
	bool8	DirectColourMode;
};

struct SLineData
{
	struct
	{
		uint16	VOffset;
		uint16	HOffset;
	}	BG[4];
};

struct SLineMatrixData
{
	short	MatrixA;
	short	MatrixB;
	short	MatrixC;
	short	MatrixD;
	short	CentreX;
	short	CentreY;
	short	M7HOFS;
	short	M7VOFS;
};

// CBOC-Vita: one flush of the renderer (see gfx.cpp S9xUpdateScreen_impl)
struct S9xRenderJob
{
	uint8	blank;		// forced blanking: fill the lines with black
	uint8	widen;		// the picture became 512 wide in this flush: spread the lines already drawn
	uint8	heighten;	// the picture became interlaced 2x in this flush: spread the lines already drawn
	uint8	sub;		// render the sub screen too
	uint32	widen_ppl;	// GFX.PPL as it was before this flush
};
void S9xRenderBatch (const S9xRenderJob &);
// optional PPU thread (ppu_mt.cpp)
bool S9xPPUMTActive (void);
void S9xPPUMTBatch (const S9xRenderJob &);
void S9xPPUMTFrameStart (void);
void S9xPPUMTFrameEnd (int width, int height);
void S9xPPUMTSync (void);
void S9xPPUMTInvalidateAll (void);
void S9xPPUMTVRAMDirty (uint32 address);
bool S9xPPUMTSet (bool enable);
const uint16 *S9xPPUMTVideo (int *width, int *height);
void S9xPPUMTConfig (int core, int prio);
/* Non-blocking: queues the end-of-frame copy and returns the newest picture the worker has finished
 * (usually the previous frame), or NULL when none exists yet. *serial counts finished pictures. */
const uint16 *S9xPPUMTVideoNoWait (int *width, int *height, unsigned *serial);
void S9xMTSetSpin (int iterations);
void S9xPPUMTStats (uint64_t *busy_ns, uint64_t *batches, uint64_t *waits, uint64_t *wait_ns);
extern bool8 g_s9x_obj_ran;
extern bool8 g_s9x_vram_track;
extern uint32 g_s9x_colour_version;

extern uint16		BlackColourMap[256];
extern uint16		DirectColourMaps[8][256];
extern uint8		mul_brightness[16][32];
extern uint8		brightness_cap[64];
extern struct SBG	BG;
extern struct SGFX	GFX;

#define H_FLIP		0x4000
#define V_FLIP		0x8000
#define BLANK_TILE	2

// CBOC-Vita: 4-lane versions of the two plain blend operations, for whole tile rows (tileimpl.h, DrawRow8).
// Lane for lane the same arithmetic as fn() (32-bit unsigned lanes hold the values fn() keeps in int, and
// no intermediate of fn() is negative), written with GCC vector extensions so that it is SSE2 on the PC
// and NEON on the Vita.
typedef uint32 s9x_v4u __attribute__((vector_size(16)));

struct COLOR_ADD
{
	enum { Vec = 1 };
	static alwaysinline s9x_v4u fnV(s9x_v4u C1, s9x_v4u C2)
	{
		const uint32 RB = (0x1F << RED_SHIFT_BITS) | 0x1F;
		const uint32 GM = 0x1F << GREEN_SHIFT_BITS;
		s9x_v4u rb = (C1 & RB) + (C2 & RB);
		s9x_v4u rbcarry = rb & ((0x20u << RED_SHIFT_BITS) | (0x20u << 0));
		s9x_v4u g = (C1 & GM) + (C2 & GM);
		s9x_v4u flags = ((g & (0x20u << GREEN_SHIFT_BITS)) | rbcarry) >> 5;
		s9x_v4u rgbsaturate = (flags << 5) - flags;		// * 0x1f
		s9x_v4u retval = ((rb & RB) | (g & GM) | rgbsaturate) & 0xFFFFu;
#if GREEN_SHIFT_BITS == 6
		retval |= (retval & 0x0400u) >> 5;
#endif
		return retval;
	}

	static alwaysinline uint16 fn(uint16 C1, uint16 C2)
	{
		const int RED_MASK = 0x1F << RED_SHIFT_BITS;
		const int GREEN_MASK = 0x1F << GREEN_SHIFT_BITS;
		const int BLUE_MASK = 0x1F;

		int rb = C1 & (RED_MASK | BLUE_MASK);
		rb += C2 & (RED_MASK | BLUE_MASK);
		int rbcarry = rb & ((0x20 << RED_SHIFT_BITS) | (0x20 << 0));
		int g = (C1 & (GREEN_MASK)) + (C2 & (GREEN_MASK));
		int rgbsaturate = (((g & (0x20 << GREEN_SHIFT_BITS)) | rbcarry) >> 5) * 0x1f;
		uint16 retval = (rb & (RED_MASK | BLUE_MASK)) | (g & GREEN_MASK) | rgbsaturate;
#if GREEN_SHIFT_BITS == 6
		retval |= (retval & 0x0400) >> 5;
#endif
		return retval;
	}

	static alwaysinline uint16 fn1_2(uint16 C1, uint16 C2)
	{
		return ((((C1 & RGB_REMOVE_LOW_BITS_MASK) +
			(C2 & RGB_REMOVE_LOW_BITS_MASK)) >> 1) +
			(C1 & C2 & RGB_LOW_BITS_MASK)) | ALPHA_BITS_MASK;
	}
};

struct COLOR_ADD_BRIGHTNESS
{
	enum { Vec = 0 };
	static alwaysinline s9x_v4u fnV(s9x_v4u C1, s9x_v4u C2) { return C1; }	// never used (Vec = 0)

	static alwaysinline uint16 fn(uint16 C1, uint16 C2)
	{
		return ((brightness_cap[ (C1 >> RED_SHIFT_BITS)           +  (C2 >> RED_SHIFT_BITS)          ] << RED_SHIFT_BITS)   |
				(brightness_cap[((C1 >> GREEN_SHIFT_BITS) & 0x1f) + ((C2 >> GREEN_SHIFT_BITS) & 0x1f)] << GREEN_SHIFT_BITS) |
	// Proper 15->16bit color conversion moves the high bit of green into the low bit.
	#if GREEN_SHIFT_BITS == 6
			   ((brightness_cap[((C1 >> 6) & 0x1f) + ((C2 >> 6) & 0x1f)] & 0x10) << 1) |
	#endif
				(brightness_cap[ (C1                      & 0x1f) +  (C2                      & 0x1f)]      ));
	}

	static alwaysinline uint16 fn1_2(uint16 C1, uint16 C2)
	{
		return COLOR_ADD::fn1_2(C1, C2);
	}
};


struct COLOR_SUB
{
	enum { Vec = 1 };
	static alwaysinline s9x_v4u fnV(s9x_v4u C1, s9x_v4u C2)
	{
		const uint32 RB = THIRD_COLOR_MASK | FIRST_COLOR_MASK;
		s9x_v4u rb1 = (C1 & RB) | ((0x20u << 0) | (0x20u << RED_SHIFT_BITS));
		s9x_v4u rb2 = C2 & RB;
		s9x_v4u rb = rb1 - rb2;
		s9x_v4u rbcarry = rb & ((0x20u << RED_SHIFT_BITS) | (0x20u << 0));
		s9x_v4u g = ((C1 & (uint32) SECOND_COLOR_MASK) | (0x20u << GREEN_SHIFT_BITS)) - (C2 & (uint32) SECOND_COLOR_MASK);
		s9x_v4u flags = ((g & (0x20u << GREEN_SHIFT_BITS)) | rbcarry) >> 5;
		s9x_v4u rgbsaturate = (flags << 5) - flags;		// * 0x1f
		s9x_v4u retval = (((rb & RB) | (g & (uint32) SECOND_COLOR_MASK)) & rgbsaturate) & 0xFFFFu;
#if GREEN_SHIFT_BITS == 6
		retval |= (retval & 0x0400u) >> 5;
#endif
		return retval;
	}

	static alwaysinline uint16 fn(uint16 C1, uint16 C2)
	{
		int rb1 = (C1 & (THIRD_COLOR_MASK | FIRST_COLOR_MASK)) | ((0x20 << 0) | (0x20 << RED_SHIFT_BITS));
		int rb2 = C2 & (THIRD_COLOR_MASK | FIRST_COLOR_MASK);
		int rb = rb1 - rb2;
		int rbcarry = rb & ((0x20 << RED_SHIFT_BITS) | (0x20 << 0));
		int g = ((C1 & (SECOND_COLOR_MASK)) | (0x20 << GREEN_SHIFT_BITS)) - (C2 & (SECOND_COLOR_MASK));
		int rgbsaturate = (((g & (0x20 << GREEN_SHIFT_BITS)) | rbcarry) >> 5) * 0x1f;
		uint16 retval = ((rb & (THIRD_COLOR_MASK | FIRST_COLOR_MASK)) | (g & SECOND_COLOR_MASK)) & rgbsaturate;
#if GREEN_SHIFT_BITS == 6
		retval |= (retval & 0x0400) >> 5;
#endif
		return retval;
	}

	static alwaysinline uint16 fn1_2(uint16 C1, uint16 C2)
	{
		return GFX.ZERO[((C1 | RGB_HI_BITS_MASKx2) -
			(C2 & RGB_REMOVE_LOW_BITS_MASK)) >> 1];
	}
};

void S9xStartScreenRefresh (void);
void S9xClearZBuffers (uint8 *z, uint8 *subz);
extern uint32 g_s9x_zdirty, g_s9x_subzdirty;
void S9xEndScreenRefresh (void);
void S9xBuildDirectColourMaps (void);
// CBOC-Vita: a brightness change only marks the colour tables stale; they are rebuilt (identically) before
// the next render flush. Some Super FX demos change INIDISP ~900 times per frame.
extern bool8 g_s9x_colours_stale;
void S9xRefreshColours (void);
void RenderLine (uint8);
void S9xComputeClipWindows (void);
void S9xDisplayChar (uint16 *, uint8);
void S9xGraphicsScreenResize (void);
// called automatically unless Settings.AutoDisplayMessages is false
void S9xDisplayMessages (uint16 *, int, int, int, int);

// external port interface which must be implemented or initialised for each port
bool8 S9xGraphicsInit (void);
void S9xGraphicsDeinit (void);
bool8 S9xInitUpdate (void);
bool8 S9xDeinitUpdate (int, int);
bool8 S9xContinueUpdate (int, int);
void S9xReRefresh (void);
void S9xSyncSpeed (void);

// called instead of S9xDisplayString if set to non-NULL
extern void (*S9xCustomDisplayString) (const char *, int, int, bool, int type);
void S9xVariableDisplayString(const char* string, int linesFromBottom, int pixelsFromLeft, bool allowWrap, int type);

#endif
