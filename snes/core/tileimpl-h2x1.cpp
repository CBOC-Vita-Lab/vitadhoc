/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#define _TILEIMPL_CPP_
#include "tileimpl.h"

namespace TileImpl {

	template<class MATH, class BPSTART>
	void HiresBase<MATH, BPSTART>::Draw(const PixCtx &c, int N, int M, uint32 Offset, uint32 OffsetInLine, uint8 Pix, uint8 Z1, uint8 Z2)
	{
		if (Z1 > c.DB[Offset + 2 * N] && (M))
		{
			c.S[Offset + 2 * N + 1] = MATH::Calc(c, c.ScreenColors[Pix], c.SubScreen[Offset + 2 * N], c.SubZ[Offset + 2 * N]);
			if ((OffsetInLine + 2 * N ) != (SNES_WIDTH - 1) << 1)
				c.S[Offset + 2 * N + 2] = MATH::Calc(c, (c.ClipColors ? 0 : c.SubScreen[Offset + 2 * N + 2]), c.RealScreenColors[Pix], c.SubZ[Offset + 2 * N]);
			if ((OffsetInLine + 2 * N) == 0 || (OffsetInLine + 2 * N) == c.RealPPL)
				c.S[Offset + 2 * N] = MATH::Calc(c, (c.ClipColors ? 0 : c.SubScreen[Offset + 2 * N]), c.RealScreenColors[Pix], c.SubZ[Offset + 2 * N]);
			c.DB[Offset + 2 * N] = c.DB[Offset + 2 * N + 1] = Z2;
		}
	}

	// hires double width
	template struct Renderers<DrawTile16, Hires>;
	template struct Renderers<DrawClippedTile16, Hires>;
	template struct Renderers<DrawMosaicPixel16, Hires>;
	template struct Renderers<DrawBackdrop16, Hires>;
	template struct Renderers<DrawMode7MosaicBG1, Hires>;
	template struct Renderers<DrawMode7BG1, Hires>;
	template struct Renderers<DrawMode7MosaicBG2, Hires>;
	template struct Renderers<DrawMode7BG2, Hires>;

	// hires double width interlace
	template struct Renderers<DrawTile16, HiresInterlace>;
	template struct Renderers<DrawClippedTile16, HiresInterlace>;
	template struct Renderers<DrawMosaicPixel16, HiresInterlace>;
	//template struct Renderers<DrawBackdrop16, Hires>;
	//template struct Renderers<DrawMode7MosaicBG1, Hires>;
	//template struct Renderers<DrawMode7BG1, Hires>;
	//template struct Renderers<DrawMode7MosaicBG2, Hires>;
	//template struct Renderers<DrawMode7BG2, Hires>;

} // namespace TileImpl
