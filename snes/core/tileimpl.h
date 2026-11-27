/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#ifndef _TILEIMPL_H_
#define _TILEIMPL_H_

#include "snes9x.h"
#include "ppu.h"
#include "tile.h"
#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

extern struct SLineMatrixData	LineMatrixData[240];
extern int g_wide_on, g_wide_dx;	// CBOC-Vita: wide view pass (gfx.cpp)
extern int g_s9x_fastpix;			// CBOC-Vita: 1 = the exact fast pixel paths (default; host option "fast_pixels"), 0 = the per-pixel reference paths (globals.cpp: one copy shared by the renderer copies)


namespace TileImpl {

	// CBOC-Vita: the per-pixel plotters used to read GFX.S, GFX.DB, GFX.ScreenColors, GFX.SubScreen, ... from
	// the global on every pixel and, because the stores into the screen could alias the globals, reload them
	// after every pixel. A tile routine now copies them once into this struct (a local whose address never
	// escapes once the plotter is inlined) and passes it to the plotter. Same pixels, far fewer loads.
	struct PixCtx
	{
		uint16 *S;
		uint8  *DB;
		const uint16 *SubScreen;
		const uint8  *SubZ;
		const uint16 *ScreenColors;
		const uint16 *RealScreenColors;
		uint32 FixedColour;
		uint32 RealPPL;
		uint8  Z1, Z2;
		bool8  ClipColors;

		alwaysinline PixCtx()
		{
			S = GFX.S; DB = GFX.DB; SubScreen = GFX.SubScreen; SubZ = GFX.SubZBuffer;
			ScreenColors = GFX.ScreenColors; RealScreenColors = GFX.RealScreenColors;
			FixedColour = GFX.FixedColour; RealPPL = GFX.RealPPL;
			Z1 = GFX.Z1; Z2 = GFX.Z2; ClipColors = GFX.ClipColors;
		}
	};

	struct BPProgressive
	{
		enum { Pitch = 1 };
		static alwaysinline uint32 Get(uint32 StartLine) { return StartLine; }
	};

	// Interlace: Only draw every other line, so we'll redefine bpstart_t and Pitch to do so.
	// Otherwise, it's the same as Normal2x1/Hires2x1.
	struct BPInterlace
	{
		enum { Pitch = 2 };
		static alwaysinline uint32 Get(uint32 StartLine) { return StartLine * 2 + BG.InterlaceLine; }
	};


	// The 1x1 pixel plotter, for speedhacking modes.
	template<class MATH, class BPSTART>
	struct Normal1x1Base
	{
		enum { Pitch = BPSTART::Pitch, Step = 1, Fill = MATH::None, Const = 1 };
		typedef BPSTART bpstart_t;
		typedef MATH math_t;

		static alwaysinline void Draw(const PixCtx &c, int N, int M, uint32 Offset, uint32 OffsetInLine, uint8 Pix, uint8 Z1, uint8 Z2);
	};

	template<class MATH>
	struct Normal1x1 : public Normal1x1Base<MATH, BPProgressive> {};


	// The 2x1 pixel plotter, for normal rendering when we've used hires/interlace already this frame.
	template<class MATH, class BPSTART>
	struct Normal2x1Base
	{
		enum { Pitch = BPSTART::Pitch, Step = 2, Fill = MATH::None, Const = 1 };
		typedef BPSTART bpstart_t;
		typedef MATH math_t;

		static alwaysinline void Draw(const PixCtx &c, int N, int M, uint32 Offset, uint32 OffsetInLine, uint8 Pix, uint8 Z1, uint8 Z2);
	};

	template<class MATH>
	struct Normal2x1 : public Normal2x1Base<MATH, BPProgressive> {};
	template<class MATH>
	struct Interlace : public Normal2x1Base<MATH, BPInterlace> {};


	// Hires pixel plotter, this combines the main and subscreen pixels as appropriate to render hires or pseudo-hires images.
	// Use it only on the main screen, subscreen should use Normal2x1 instead.
	// Hires math:
	//     Main pixel is mathed as normal: Main(x, y) * Sub(x, y).
	//     Sub pixel is mathed somewhat weird: Basically, for Sub(x + 1, y) we apply the same operation we applied to Main(x, y)
	//     (e.g. no math, add fixed, add1/2 subscreen) using Main(x, y) as the "corresponding subscreen pixel".
	//     Also, color window clipping clips Sub(x + 1, y) if Main(x, y) is clipped, not Main(x + 1, y).
	//     We don't know how Sub(0, y) is handled.
	template<class MATH, class BPSTART>
	struct HiresBase
	{
		enum { Pitch = BPSTART::Pitch, Step = 2, Fill = 0, Const = 0 };
		typedef BPSTART bpstart_t;
		typedef MATH math_t;

		static alwaysinline void Draw(const PixCtx &c, int N, int M, uint32 Offset, uint32 OffsetInLine, uint8 Pix, uint8 Z1, uint8 Z2);
	};

	template<class MATH>
	struct Hires : public HiresBase<MATH, BPProgressive> {};
	template<class MATH>
	struct HiresInterlace : public HiresBase<MATH, BPInterlace> {};


	class CachedTile
	{
	public:
		CachedTile(uint32 tile) : Tile(tile) {}

		alwaysinline void GetCachedTile()
		{
			TileAddr = BG.TileAddress + ((Tile & 0x3ff) << BG.TileShift);
			if (Tile & 0x100)
				TileAddr += BG.NameSelect;
			TileAddr &= 0xffff;
			TileNumber = TileAddr >> BG.TileShift;
			if (Tile & H_FLIP)
			{
				pCache = &BG.BufferFlip[TileNumber << 6];
				if (!BG.BufferedFlip[TileNumber])
					BG.BufferedFlip[TileNumber] = BG.ConvertTileFlip(pCache, TileAddr, Tile & 0x3ff);
			}
			else
			{
				pCache = &BG.Buffer[TileNumber << 6];
				if (!BG.Buffered[TileNumber])
					BG.Buffered[TileNumber] = BG.ConvertTile(pCache, TileAddr, Tile & 0x3ff);
			}
		}

		alwaysinline bool IsBlankTile() const
		{
			return ((Tile & H_FLIP) ? BG.BufferedFlip[TileNumber] : BG.Buffered[TileNumber]) == BLANK_TILE;
		}

		alwaysinline void SelectPalette() const
		{
			if (BG.DirectColourMode)
			{
				GFX.RealScreenColors = DirectColourMaps[(Tile >> 10) & 7];
			}
			else
				GFX.RealScreenColors = &IPPU.ScreenColors[((Tile >> BG.PaletteShift) & BG.PaletteMask) + BG.StartPalette];
			GFX.ScreenColors = GFX.ClipColors ? BlackColourMap : GFX.RealScreenColors;
		}

		alwaysinline uint8* Ptr() const
		{
			return pCache;
		}

	private:
		uint8  *pCache;
		uint32 Tile;
		uint32 TileNumber;
		uint32 TileAddr;
	};


	struct NOMATH
	{
		enum { None = 1, Vec = 0 };
		static alwaysinline void CalcRow8(const PixCtx &c, const uint32 *main8, uint32 Offset, uint16 *out) {}
#if S9X_NEON_RGB565
		static alwaysinline uint16x8_t CalcRow8V16(const PixCtx &c, uint16x8_t main8, uint32 Offset) { (void) c; (void) Offset; return main8; }
#endif
		static alwaysinline uint16 Calc(const PixCtx &c, uint16 Main, uint16 Sub, uint8 SD)
		{
			return Main;
		}
		static alwaysinline uint16 CalcConst(const PixCtx &c, uint16 Main) { return Main; }
	};
	typedef NOMATH Blend_None;

	template<class Op>
	struct REGMATH
	{
		enum { None = 0, Vec = Op::Vec };
		static alwaysinline uint16 Calc(const PixCtx &c, uint16 Main, uint16 Sub, uint8 SD)
		{
			return Op::fn(Main, (SD & 0x20) ? Sub : c.FixedColour);
		}
#if S9X_NEON_RGB565
		// eight RGB565 pixels at once in 16-bit NEON lanes (no widen/narrow, no stack spill)
		static alwaysinline uint16x8_t CalcRow8V16(const PixCtx &c, uint16x8_t main8, uint32 Offset)
		{
			uint64 sz64;
			memcpy(&sz64, c.SubZ + Offset, 8);
			const uint64 szm = sz64 & 0x2020202020202020ULL;
			if (szm == 0)
				return Op::fnV16(main8, vdupq_n_u16((uint16) c.FixedColour));
			const uint16x8_t sub = vld1q_u16(c.SubScreen + Offset);
			if (szm == 0x2020202020202020ULL)
				return Op::fnV16(main8, sub);
			const uint8x8_t m8 = vtst_u8(vcreate_u8(sz64), vdup_n_u8(0x20));
			const uint16x8_t sel = vreinterpretq_u16_s16(vmovl_s8(vreinterpret_s8_u8(m8)));
			return Op::fnV16(main8, vbslq_u16(sel, sub, vdupq_n_u16((uint16) c.FixedColour)));
		}
#endif
		// eight pixels at once: the same as Calc() for each (out[i] = Calc(main8[i], SubScreen[Offset+i], SubZ[Offset+i]))
		static alwaysinline void CalcRow8(const PixCtx &c, const uint32 *main8, uint32 Offset, uint16 *out)
		{
			// four lanes a step: other = (SubZ & 0x20) ? SubScreen : FixedColour, the same bit for bit as the scalar form
#if defined(__ARM_NEON)
			// the generic vector form widens the four SubZ bytes through the stack on the Vita's compiler (scalar loads and stores); the same steps in NEON
			const uint32x4_t fixedq = vdupq_n_u32(c.FixedColour);
			for (int h = 0; h < 8; h += 4)
			{
				uint32 zw;
				memcpy(&zw, c.SubZ + Offset + h, 4);
				const uint32x4_t zz = vmovl_u16(vget_low_u16(vmovl_u8(vreinterpret_u8_u32(vdup_n_u32(zw)))));
				const uint32x4_t ss = vmovl_u16(vld1_u16(c.SubScreen + Offset + h));
				const uint32x4_t sel = vtstq_u32(zz, vdupq_n_u32(0x20u));
				const uint32x4_t other = vbslq_u32(sel, ss, fixedq);
				const s9x_v4u r = Op::fnV((s9x_v4u) vld1q_u32(main8 + h), (s9x_v4u) other);
				vst1_u16(out + h, vmovn_u32((uint32x4_t) r));
			}
#else
			typedef uint8 v4u8 __attribute__((vector_size(4)));
			typedef uint16 v4u16 __attribute__((vector_size(8)));
			const s9x_v4u fixed = { c.FixedColour, c.FixedColour, c.FixedColour, c.FixedColour };
			for (int h = 0; h < 8; h += 4)
			{
				v4u8 z8;
				v4u16 s16;
				memcpy(&z8, c.SubZ + Offset + h, 4);
				memcpy(&s16, c.SubScreen + Offset + h, 8);
				const s9x_v4u zz = __builtin_convertvector(z8, s9x_v4u);
				const s9x_v4u ss = __builtin_convertvector(s16, s9x_v4u);
				const s9x_v4u sel = (s9x_v4u) ((zz & 0x20u) != 0u);
				const s9x_v4u other = (ss & sel) | (fixed & ~sel);
				s9x_v4u a;
				memcpy(&a, main8 + h, 16);
				const v4u16 o = __builtin_convertvector(Op::fnV(a, other), v4u16);
				memcpy(out + h, &o, 8);
			}
#endif
		}
		// the result for a pixel whose sub-screen flag (SD & 0x20) is clear: independent of the sub-screen pixel
		static alwaysinline uint16 CalcConst(const PixCtx &c, uint16 Main) { return Op::fn(Main, c.FixedColour); }
	};
	typedef REGMATH<COLOR_ADD> Blend_Add;
	typedef REGMATH<COLOR_SUB> Blend_Sub;
	typedef REGMATH<COLOR_ADD_BRIGHTNESS> Blend_AddBrightness;

	template<class Op>
	struct MATHF1_2
	{
		enum { None = 0, Vec = Op::Vec16 };
		static alwaysinline void CalcRow8(const PixCtx &c, const uint32 *main8, uint32 Offset, uint16 *out) {}
#if S9X_NEON_RGB565
		static alwaysinline uint16x8_t CalcRow8V16(const PixCtx &c, uint16x8_t main8, uint32 Offset)
		{
			(void) Offset;
			const uint16x8_t fixed = vdupq_n_u16((uint16) c.FixedColour);
			return c.ClipColors ? Op::fnV16(main8, fixed) : Op::fn1_2V16(main8, fixed);
		}
#endif
		static alwaysinline uint16 Calc(const PixCtx &c, uint16 Main, uint16 Sub, uint8 SD)
		{
			return c.ClipColors ? Op::fn(Main, c.FixedColour) : Op::fn1_2(Main, c.FixedColour);
		}
		static alwaysinline uint16 CalcConst(const PixCtx &c, uint16 Main) { return Calc(c, Main, 0, 0); }
	};
	typedef MATHF1_2<COLOR_ADD> Blend_AddF1_2;
	typedef MATHF1_2<COLOR_SUB> Blend_SubF1_2;

	template<class Op>
	struct MATHS1_2
	{
		enum { None = 0, Vec = Op::Vec16 };
		static alwaysinline void CalcRow8(const PixCtx &c, const uint32 *main8, uint32 Offset, uint16 *out) {}
#if S9X_NEON_RGB565
		static alwaysinline uint16x8_t CalcRow8V16(const PixCtx &c, uint16x8_t main8, uint32 Offset)
		{
			uint64 sz64;
			memcpy(&sz64, c.SubZ + Offset, 8);
			const uint64 szm = sz64 & 0x2020202020202020ULL;
			if (szm == 0)
				return Op::fnV16(main8, vdupq_n_u16((uint16) c.FixedColour));
			const uint16x8_t sub = vld1q_u16(c.SubScreen + Offset);
			if (szm == 0x2020202020202020ULL)
				return c.ClipColors ? Op::fnV16(main8, sub) : Op::fn1_2V16(main8, sub);
			const uint8x8_t m8 = vtst_u8(vcreate_u8(sz64), vdup_n_u8(0x20));
			const uint16x8_t sel = vreinterpretq_u16_s16(vmovl_s8(vreinterpret_s8_u8(m8)));
			const uint16x8_t fixed = vdupq_n_u16((uint16) c.FixedColour);
			if (c.ClipColors)
				return Op::fnV16(main8, vbslq_u16(sel, sub, fixed));
			return vbslq_u16(sel, Op::fn1_2V16(main8, sub), Op::fnV16(main8, fixed));
		}
#endif
		static alwaysinline uint16 Calc(const PixCtx &c, uint16 Main, uint16 Sub, uint8 SD)
		{
			return c.ClipColors ? REGMATH<Op>::Calc(c, Main, Sub, SD) : (SD & 0x20) ? Op::fn1_2(Main, Sub) : Op::fn(Main, c.FixedColour);
		}
		static alwaysinline uint16 CalcConst(const PixCtx &c, uint16 Main) { return Op::fn(Main, c.FixedColour); }
	};
	typedef MATHS1_2<COLOR_ADD> Blend_AddS1_2;
	typedef MATHS1_2<COLOR_SUB> Blend_SubS1_2;
	typedef MATHS1_2<COLOR_ADD_BRIGHTNESS> Blend_AddS1_2Brightness;

	template<
		template<class PIXEL_> class TILE,
		template<class MATH> class PIXEL
	>
	struct Renderers
	{
		enum { Pitch = PIXEL<Blend_None>::Pitch };
		typedef typename TILE< PIXEL<Blend_None> >::call_t call_t;

		static call_t Functions[9];
	};

	#ifdef _TILEIMPL_CPP_
	template<
		template<class PIXEL_> class TILE,
		template<class MATH> class PIXEL
	>
	typename Renderers<TILE, PIXEL>::call_t Renderers<TILE, PIXEL>::Functions[9] =
	{
		TILE< PIXEL<Blend_None> >::Draw,
		TILE< PIXEL<Blend_Add> >::Draw,
		TILE< PIXEL<Blend_AddF1_2> >::Draw,
		TILE< PIXEL<Blend_AddS1_2> >::Draw,
		TILE< PIXEL<Blend_Sub> >::Draw,
		TILE< PIXEL<Blend_SubF1_2> >::Draw,
		TILE< PIXEL<Blend_SubS1_2> >::Draw,
		TILE< PIXEL<Blend_AddBrightness> >::Draw,
		TILE< PIXEL<Blend_AddS1_2Brightness> >::Draw,
	};
	#endif

	// Basic routine to render an unclipped tile.
	// Input parameters:
	//     bpstart_t = either StartLine or (StartLine * 2 + BG.InterlaceLine),
	//     so interlace modes can render every other line from the tile.
	//     Pitch = 1 or 2, again so interlace can count lines properly.
	//     DRAW_PIXEL(N, M) is a routine to actually draw the pixel. N is the pixel in the row to draw,
	//     and M is a test which if false means the pixel should be skipped.
	//     Z1 is the "draw if Z1 > cur_depth".
	//     Z2 is the "cur_depth = new_depth". OBJ need the two separate.
	//     Pix is the pixel to draw.

	static_assert((MAX_SNES_WIDTH & (MAX_SNES_WIDTH - 1)) == 0, "RealPPL must be a power of two");
	#define OFFSET_IN_LINE \
		uint32 OffsetInLine = Offset & (MAX_SNES_WIDTH - 1);
	#define DRAW_PIXEL(N, M) PIXEL::Draw(ctx, N, M, Offset, OffsetInLine, Pix, Z1, Z2)
	#define Z1	ctx.Z1
	#define Z2	ctx.Z2


	// CBOC-Vita: one tile row (8 pixels) at a time for the normal-width plotter (with or without colour math):
	// the 8 pixel bytes and the 8 depth bytes are compared together. Rows with nothing to draw cost a
	// few instructions, rows that are entirely drawn are written without a test per pixel, and mixed rows
	// draw only their set pixels, in the same way (same pixel colour, same depth) as the per-pixel path.
	// Depth values are below 0x80 (D <= 32 plus a small priority), which the byte compare relies on.
	static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "DrawRow8 assumes a little-endian host");
	template<class PIXEL, bool FLIP>
	static alwaysinline void DrawRow8(const PixCtx &ctx, const uint8 *bp, uint32 Offset, uint32 OffsetInLine, uint8 z1, uint8 z2)
	{
		if (PIXEL::Step == 1)
		{
			const uint64 ones = 0x0101010101010101ULL, hi = 0x8080808080808080ULL, lo7 = 0x7f7f7f7f7f7f7f7fULL;
			uint64 pix, db;
			memcpy(&pix, bp, 8);
			if (FLIP)
				pix = __builtin_bswap64(pix);
			const uint64 nz = (((pix & lo7) + lo7) | pix) & hi;			// bit 7 of lane i: pixel i is not 0
			if (!nz)
				return;
			memcpy(&db, ctx.DB + Offset, 8);
			const uint64 gt = ((hi + (uint64) z1 * ones - ones) - db) & hi;	// bit 7 of lane i: z1 > depth i
			uint64 m = nz & gt;
			if (!m)
				return;
			uint16 *sp = ctx.S + Offset;
			if (PIXEL::math_t::Vec && m == hi)
			{
				// a whole row with plain add / subtract / half-add / half-subtract maths
#if S9X_NEON_RGB565
				const uint16 *sc = ctx.ScreenColors;
				const uint32 p_lo = (uint32) pix;
				const uint32 p_hi = (uint32) (pix >> 32);
				const uint32 w0 = (uint32) sc[p_lo & 0xff] | ((uint32) sc[(p_lo >> 8) & 0xff] << 16);
				const uint32 w1 = (uint32) sc[(p_lo >> 16) & 0xff] | ((uint32) sc[p_lo >> 24] << 16);
				const uint32 w2 = (uint32) sc[p_hi & 0xff] | ((uint32) sc[(p_hi >> 8) & 0xff] << 16);
				const uint32 w3 = (uint32) sc[(p_hi >> 16) & 0xff] | ((uint32) sc[p_hi >> 24] << 16);
				const uint16x8_t main8 = vcombine_u16(
					vcreate_u16((uint64) w0 | ((uint64) w1 << 32)),
					vcreate_u16((uint64) w2 | ((uint64) w3 << 32)));
				vst1q_u16(sp, PIXEL::math_t::CalcRow8V16(ctx, main8, Offset));
#else
				uint32 mc[8];
				for (int i = 0; i < 8; i++)
					mc[i] = ctx.ScreenColors[(pix >> (8 * i)) & 0xff];
				PIXEL::math_t::CalcRow8(ctx, mc, Offset, sp);
#endif
				const uint64 zz = (uint64) z2 * ones;
				memcpy(ctx.DB + Offset, &zz, 8);
			}
			else if (PIXEL::math_t::Vec && __atomic_load_n(&g_s9x_fastpix, __ATOMIC_RELAXED) && ({ uint64 t3 = m & (m - 1); t3 &= t3 - 1; t3 != 0; }))
			{
				// a mixed row with plain add / subtract / half-add / half-subtract maths: the maths is a pure function
				// of the pixel's colour and the sub screen at that position, so it is done for all eight lanes at once
				// and only the drawn lanes are kept
#if S9X_NEON_RGB565
				const uint16 *sc = ctx.ScreenColors;
				const uint32 p_lo = (uint32) pix;
				const uint32 p_hi = (uint32) (pix >> 32);
				const uint32 w0 = (uint32) sc[p_lo & 0xff] | ((uint32) sc[(p_lo >> 8) & 0xff] << 16);
				const uint32 w1 = (uint32) sc[(p_lo >> 16) & 0xff] | ((uint32) sc[p_lo >> 24] << 16);
				const uint32 w2 = (uint32) sc[p_hi & 0xff] | ((uint32) sc[(p_hi >> 8) & 0xff] << 16);
				const uint32 w3 = (uint32) sc[(p_hi >> 16) & 0xff] | ((uint32) sc[p_hi >> 24] << 16);
				const uint16x8_t main8 = vcombine_u16(
					vcreate_u16((uint64) w0 | ((uint64) w1 << 32)),
					vcreate_u16((uint64) w2 | ((uint64) w3 << 32)));
				const uint16x8_t res = PIXEL::math_t::CalcRow8V16(ctx, main8, Offset);
				const uint8x8_t m8 = vreinterpret_u8_s8(vshr_n_s8(vreinterpret_s8_u8(vcreate_u8(m)), 7));
				const uint16x8_t m16 = vreinterpretq_u16_s16(vmovl_s8(vreinterpret_s8_u8(m8)));
				vst1q_u16(sp, vbslq_u16(m16, res, vld1q_u16(sp)));
				vst1_u8(ctx.DB + Offset, vbsl_u8(m8, vdup_n_u8(z2), vcreate_u8(db)));
#else
				uint32 mc[8];
				uint16 res[8];
				for (int i = 0; i < 8; i++)
					mc[i] = ctx.ScreenColors[(pix >> (8 * i)) & 0xff];
				PIXEL::math_t::CalcRow8(ctx, mc, Offset, res);
				do {
					const int i = __builtin_ctzll(m) >> 3;
					sp[i] = res[i];
					ctx.DB[Offset + i] = z2;
					m &= m - 1;
				} while (m);
#endif
			}
			else if (PIXEL::Fill && m == hi)
			{
				for (int i = 0; i < 8; i++)
					sp[i] = ctx.ScreenColors[(pix >> (8 * i)) & 0xff];
				const uint64 zz = (uint64) z2 * ones;
				memcpy(ctx.DB + Offset, &zz, 8);
			}
			else
			{
				do {
					const int i = __builtin_ctzll(m) >> 3;
					sp[i] = PIXEL::math_t::Calc(ctx, ctx.ScreenColors[(pix >> (8 * i)) & 0xff], ctx.SubScreen[Offset + i], ctx.SubZ[Offset + i]);
					ctx.DB[Offset + i] = z2;
					m &= m - 1;
				} while (m);
			}
		}
		else
		{
			for (int x = 0; x < 8; x++) {
				const uint8 Pix = FLIP ? bp[7 - x] : bp[x];
				PIXEL::Draw(ctx, x, Pix, Offset, OffsetInLine, Pix, z1, z2);
			}
		}
	}

	template<class PIXEL>
	struct DrawTile16
	{
		typedef void (*call_t)(uint32, uint32, uint32, uint32);

		enum { Pitch = PIXEL::Pitch };
		typedef typename PIXEL::bpstart_t bpstart_t;

		static void Draw(uint32 Tile, uint32 Offset, uint32 StartLine, uint32 LineCount)
		{
			CachedTile cache(Tile);
			int32	l;
			uint8	*bp, Pix;

			cache.GetCachedTile();
			if (cache.IsBlankTile())
				return;
			cache.SelectPalette();
			PixCtx ctx;

			if (!(Tile & (V_FLIP | H_FLIP)))
			{
				bp = cache.Ptr() + bpstart_t::Get(StartLine);
				OFFSET_IN_LINE;
				for (l = LineCount; l > 0; l--, bp += 8 * Pitch, Offset += GFX.PPL)
				{
					DrawRow8<PIXEL, false>(ctx, bp, Offset, OffsetInLine, Z1, Z2);
				}
			}
			else
			if (!(Tile & V_FLIP))
			{
				bp = cache.Ptr() + bpstart_t::Get(StartLine);
				OFFSET_IN_LINE;
				for (l = LineCount; l > 0; l--, bp += 8 * Pitch, Offset += GFX.PPL)
				{
					DrawRow8<PIXEL, true>(ctx, bp, Offset, OffsetInLine, Z1, Z2);
				}
			}
			else
			if (!(Tile & H_FLIP))
			{
				bp = cache.Ptr() + 56 - bpstart_t::Get(StartLine);
				OFFSET_IN_LINE;
				for (l = LineCount; l > 0; l--, bp -= 8 * Pitch, Offset += GFX.PPL)
				{
					DrawRow8<PIXEL, false>(ctx, bp, Offset, OffsetInLine, Z1, Z2);
				}
			}
			else
			{
				bp = cache.Ptr() + 56 - bpstart_t::Get(StartLine);
				OFFSET_IN_LINE;
				for (l = LineCount; l > 0; l--, bp -= 8 * Pitch, Offset += GFX.PPL)
				{
					DrawRow8<PIXEL, true>(ctx, bp, Offset, OffsetInLine, Z1, Z2);
				}
			}
		}
	};

	#undef Z1
	#undef Z2

	// Basic routine to render a clipped tile. Inputs same as above.

	#define Z1	ctx.Z1
	#define Z2	ctx.Z2

	template<class PIXEL>
	struct DrawClippedTile16
	{
		typedef void (*call_t)(uint32, uint32, uint32, uint32, uint32, uint32);

		enum { Pitch = PIXEL::Pitch };
		typedef typename PIXEL::bpstart_t bpstart_t;

		static void Draw(uint32 Tile, uint32 Offset, uint32 StartPixel, uint32 Width, uint32 StartLine, uint32 LineCount)
		{
			CachedTile cache(Tile);
			int32	l;
			uint8	*bp, Pix, w;

			cache.GetCachedTile();
			if (cache.IsBlankTile())
				return;
			cache.SelectPalette();
			PixCtx ctx;

			if (!(Tile & (V_FLIP | H_FLIP)))
			{
				bp = cache.Ptr() + bpstart_t::Get(StartLine);
				OFFSET_IN_LINE;
				for (l = LineCount; l > 0; l--, bp += 8 * Pitch, Offset += GFX.PPL)
				{
					w = Width;
					switch (StartPixel)
					{
						case 0: Pix = bp[0]; DRAW_PIXEL(0, Pix); if (!--w) break; /* Fall through */
						case 1: Pix = bp[1]; DRAW_PIXEL(1, Pix); if (!--w) break; /* Fall through */
						case 2: Pix = bp[2]; DRAW_PIXEL(2, Pix); if (!--w) break; /* Fall through */
						case 3: Pix = bp[3]; DRAW_PIXEL(3, Pix); if (!--w) break; /* Fall through */
						case 4: Pix = bp[4]; DRAW_PIXEL(4, Pix); if (!--w) break; /* Fall through */
						case 5: Pix = bp[5]; DRAW_PIXEL(5, Pix); if (!--w) break; /* Fall through */
						case 6: Pix = bp[6]; DRAW_PIXEL(6, Pix); if (!--w) break; /* Fall through */
						case 7: Pix = bp[7]; DRAW_PIXEL(7, Pix); break;
					}
				}
			}
			else
			if (!(Tile & V_FLIP))
			{
				bp = cache.Ptr() + bpstart_t::Get(StartLine);
				OFFSET_IN_LINE;
				for (l = LineCount; l > 0; l--, bp += 8 * Pitch, Offset += GFX.PPL)
				{
					w = Width;
					switch (StartPixel)
					{
						case 0: Pix = bp[7]; DRAW_PIXEL(0, Pix); if (!--w) break; /* Fall through */
						case 1: Pix = bp[6]; DRAW_PIXEL(1, Pix); if (!--w) break; /* Fall through */
						case 2: Pix = bp[5]; DRAW_PIXEL(2, Pix); if (!--w) break; /* Fall through */
						case 3: Pix = bp[4]; DRAW_PIXEL(3, Pix); if (!--w) break; /* Fall through */
						case 4: Pix = bp[3]; DRAW_PIXEL(4, Pix); if (!--w) break; /* Fall through */
						case 5: Pix = bp[2]; DRAW_PIXEL(5, Pix); if (!--w) break; /* Fall through */
						case 6: Pix = bp[1]; DRAW_PIXEL(6, Pix); if (!--w) break; /* Fall through */
						case 7: Pix = bp[0]; DRAW_PIXEL(7, Pix); break;
					}
				}
			}
			else
			if (!(Tile & H_FLIP))
			{
				bp = cache.Ptr() + 56 - bpstart_t::Get(StartLine);
				OFFSET_IN_LINE;
				for (l = LineCount; l > 0; l--, bp -= 8 * Pitch, Offset += GFX.PPL)
				{
					w = Width;
					switch (StartPixel)
					{
						case 0: Pix = bp[0]; DRAW_PIXEL(0, Pix); if (!--w) break; /* Fall through */
						case 1: Pix = bp[1]; DRAW_PIXEL(1, Pix); if (!--w) break; /* Fall through */
						case 2: Pix = bp[2]; DRAW_PIXEL(2, Pix); if (!--w) break; /* Fall through */
						case 3: Pix = bp[3]; DRAW_PIXEL(3, Pix); if (!--w) break; /* Fall through */
						case 4: Pix = bp[4]; DRAW_PIXEL(4, Pix); if (!--w) break; /* Fall through */
						case 5: Pix = bp[5]; DRAW_PIXEL(5, Pix); if (!--w) break; /* Fall through */
						case 6: Pix = bp[6]; DRAW_PIXEL(6, Pix); if (!--w) break; /* Fall through */
						case 7: Pix = bp[7]; DRAW_PIXEL(7, Pix); break;
					}
				}
			}
			else
			{
				bp = cache.Ptr() + 56 - bpstart_t::Get(StartLine);
				OFFSET_IN_LINE;
				for (l = LineCount; l > 0; l--, bp -= 8 * Pitch, Offset += GFX.PPL)
				{
					w = Width;
					switch (StartPixel)
					{
						case 0: Pix = bp[7]; DRAW_PIXEL(0, Pix); if (!--w) break; /* Fall through */
						case 1: Pix = bp[6]; DRAW_PIXEL(1, Pix); if (!--w) break; /* Fall through */
						case 2: Pix = bp[5]; DRAW_PIXEL(2, Pix); if (!--w) break; /* Fall through */
						case 3: Pix = bp[4]; DRAW_PIXEL(3, Pix); if (!--w) break; /* Fall through */
						case 4: Pix = bp[3]; DRAW_PIXEL(4, Pix); if (!--w) break; /* Fall through */
						case 5: Pix = bp[2]; DRAW_PIXEL(5, Pix); if (!--w) break; /* Fall through */
						case 6: Pix = bp[1]; DRAW_PIXEL(6, Pix); if (!--w) break; /* Fall through */
						case 7: Pix = bp[0]; DRAW_PIXEL(7, Pix); break;
					}
				}
			}
		}
	};

	#undef Z1
	#undef Z2

	// Basic routine to render a single mosaic pixel.
	// DRAW_PIXEL, bpstart_t, Z1, Z2 and Pix are the same as above, but Pitch is not used.

	#define Z1	ctx.Z1
	#define Z2	ctx.Z2

	template<class PIXEL>
	struct DrawMosaicPixel16
	{
		typedef void (*call_t)(uint32, uint32, uint32, uint32, uint32, uint32);

		typedef typename PIXEL::bpstart_t bpstart_t;

		static void Draw(uint32 Tile, uint32 Offset, uint32 StartLine, uint32 StartPixel, uint32 Width, uint32 LineCount)
		{
			CachedTile cache(Tile);
			int32	l, w;
			uint8	Pix;

			cache.GetCachedTile();
			if (cache.IsBlankTile())
				return;
			cache.SelectPalette();
			PixCtx ctx;

			if (Tile & H_FLIP)
				StartPixel = 7 - StartPixel;

			if (Tile & V_FLIP)
				Pix = cache.Ptr()[56 - bpstart_t::Get(StartLine) + StartPixel];
			else
				Pix = cache.Ptr()[bpstart_t::Get(StartLine) + StartPixel];

			if (Pix)
			{
				OFFSET_IN_LINE;
				for (l = LineCount; l > 0; l--, Offset += GFX.PPL)
				{
					for (w = Width - 1; w >= 0; w--)
						DRAW_PIXEL(w, 1);
				}
			}
		}
	};

	#undef Z1
	#undef Z2

	// Basic routine to render the backdrop.
	// DRAW_PIXEL is the same as above, but since we're just replicating a single pixel there's no need for Pitch or bpstart_t
	// (or interlace at all, really).
	// The backdrop is always depth = 1, so Z1 = Z2 = 1. And backdrop is always color 0.

	#define Z1				1
	#define Z2				1
	#define Pix				0

	template<class PIXEL>
	struct DrawBackdrop16
	{
		typedef void (*call_t)(uint32 Offset, uint32 Left, uint32 Right);

		static void Draw(uint32 Offset, uint32 Left, uint32 Right)
		{
			uint32	l, x;

			GFX.RealScreenColors = IPPU.ScreenColors;
			GFX.ScreenColors = GFX.ClipColors ? BlackColourMap : GFX.RealScreenColors;
			if (Settings.ForcedBackdrop)
				GFX.ScreenColors = &Settings.ForcedBackdrop;
			PixCtx ctx;

			OFFSET_IN_LINE;
			// CBOC-Vita: the backdrop is drawn last, so on most lines most pixels are already covered
			// (depth buffer non-zero). Eight depth bytes are tested at once; a group with no zero byte
			// has nothing to draw. Pixels are still drawn in the same order by the same plotter.
			enum { Group = 8 / PIXEL::Step };
			const uint64 oddmask = (PIXEL::Step == 2) ? 0xFF00FF00FF00FF00ULL : 0;
			for (l = GFX.StartY; l <= GFX.EndY; l++, Offset += GFX.PPL)
			{
				x = Left;
				while (x + Group <= Right)
				{
					uint64 v0, v;
					memcpy(&v0, ctx.DB + Offset + PIXEL::Step * x, 8);
					v = v0 | oddmask;
					if (!((v - 0x0101010101010101ULL) & ~v & 0x8080808080808080ULL))
					{
						x += Group;	// every pixel of the group is already covered
						continue;
					}
					if (v0 == 0)
					{
						// nothing drawn on any pixel of the group yet
						if (PIXEL::Const)
						{
							// the colour is the same for all of them when the maths cannot see a sub-screen pixel
							bool uniform = true;
							if (!PIXEL::math_t::None)
							{
								uint64 sz;
								memcpy(&sz, ctx.SubZ + Offset + PIXEL::Step * x, 8);
								uniform = !(sz & (PIXEL::Step == 1 ? 0x2020202020202020ULL : 0x0020002000200020ULL));
							}
							if (uniform)
							{
								const uint64 c4 = (uint64) PIXEL::math_t::CalcConst(ctx, ctx.ScreenColors[0]) * 0x0001000100010001ULL;
								uint16 *sp = ctx.S + Offset + PIXEL::Step * x;
								memcpy(sp, &c4, 8);
								memcpy(sp + 4, &c4, 8);
								const uint64 ones = 0x0101010101010101ULL;
								memcpy(ctx.DB + Offset + PIXEL::Step * x, &ones, 8);
								x += Group;
								continue;
							}
						}
						else if (PIXEL::math_t::None)
						{
							// hires, no maths: the same stores as the plotter makes, without the test per pixel
							const uint16 col = ctx.ScreenColors[0];
							for (uint32 xe = x + Group; x < xe; x++)
							{
								ctx.S[Offset + 2 * x + 1] = col;
								if ((OffsetInLine + 2 * x) != (SNES_WIDTH - 1) << 1)
									ctx.S[Offset + 2 * x + 2] = ctx.ClipColors ? 0 : ctx.SubScreen[Offset + 2 * x + 2];
								if ((OffsetInLine + 2 * x) == 0 || (OffsetInLine + 2 * x) == ctx.RealPPL)
									ctx.S[Offset + 2 * x] = ctx.ClipColors ? 0 : ctx.SubScreen[Offset + 2 * x];
								ctx.DB[Offset + 2 * x] = ctx.DB[Offset + 2 * x + 1] = 1;
							}
							continue;
						}
					}
					if (PIXEL::math_t::Vec && PIXEL::Step == 1 && v0 == 0)
					{
						// CBOC-Vita: plain add / subtract / half-add / half-subtract maths on a group nothing has drawn on yet and
						// that is not uniform (the sub screen shows through some of it): the eight results by the vector form (the
						// same values as Calc() per pixel, as in DrawRow8). Same pixels, same depth as DRAW_PIXEL(x, 1).
#if S9X_NEON_RGB565
						vst1q_u16(ctx.S + Offset + x, PIXEL::math_t::CalcRow8V16(ctx, vdupq_n_u16(ctx.ScreenColors[0]), Offset + x));
#else
						uint32 mc[8];
						const uint32 c0 = ctx.ScreenColors[0];
						for (int i = 0; i < 8; i++)
							mc[i] = c0;
						PIXEL::math_t::CalcRow8(ctx, mc, Offset + x, ctx.S + Offset + x);
#endif
						const uint64 ones = 0x0101010101010101ULL;
						memcpy(ctx.DB + Offset + x, &ones, 8);
						x += Group;
						continue;
					}
					for (uint32 xe = x + Group; x < xe; x++)
						DRAW_PIXEL(x, 1);
				}
				for (; x < Right; x++)
					DRAW_PIXEL(x, 1);
			}
		}
	};

	#undef Pix
	#undef Z1
	#undef Z2
	#undef DRAW_PIXEL

	// Basic routine to render a chunk of a Mode 7 BG.
	// Mode 7 has no interlace, so bpstart_t and Pitch are unused.
	// We get some new parameters, so we can use the same DRAW_TILE to do BG1 or BG2:
	//     DCMODE tests if Direct Color should apply.
	//     BG is the BG, so we use the right clip window.
	//     MASK is 0xff or 0x7f, the 'color' portion of the pixel.
	// We define Z1/Z2 to either be constant 5 or to vary depending on the 'priority' portion of the pixel.

	typedef int s9x_v4i __attribute__((vector_size(16)));
	enum { Mode7Chunk = 128 };

	#define CLIP_10_BIT_SIGNED(a)	(((a) & 0x2000) ? ((a) | ~0x3ff) : ((a) & 0x3ff))

	#define DRAW_PIXEL(N, M) PIXEL::Draw(ctx, N, M, Offset, OffsetInLine, Pix, OP::Z1(D, b), OP::Z2(D, b))

	struct DrawMode7BG1_OP
	{
		enum {
			MASK = 0xff,
			BG   = 0,
			VARZ = 0		// the depth is the same for every pixel of the BG
		};
		static uint8 Z1(int D, uint8 b) { return D + 7; }
		static uint8 Z2(int D, uint8 b) { return D + 7; }
		static uint8 DCMODE() { return Memory.FillRAM[0x2130] & 1; }
	};
	struct DrawMode7BG2_OP
	{
		enum {
			MASK = 0x7f,
			BG   = 1,
			VARZ = 1		// the depth depends on the pixel's priority bit
		};
		static uint8 Z1(int D, uint8 b) { return D + ((b & 0x80) ? 11 : 3); }
		static uint8 Z2(int D, uint8 b) { return D + ((b & 0x80) ? 11 : 3); }
		static uint8 DCMODE() { return 0; }
	};

	template<class PIXEL, class OP>
	struct DrawTileNormal
	{
		typedef void (*call_t)(uint32 Left, uint32 Right, int D);

		static void Draw(uint32 Left, uint32 Right, int D)
		{
			const uint8	*VRAM0 = Memory.VRAM;
			const uint8	*VRAM1 = Memory.VRAM + 1;

			if (OP::DCMODE())
			{
				GFX.RealScreenColors = DirectColourMaps[0];
			}
			else
				GFX.RealScreenColors = IPPU.ScreenColors;

			GFX.ScreenColors = GFX.ClipColors ? BlackColourMap : GFX.RealScreenColors;
			PixCtx ctx;

			int	aa, cc;
			int	startx;

			uint32	Offset = GFX.StartY * GFX.PPL;
			struct SLineMatrixData	*l = &LineMatrixData[GFX.StartY];

			OFFSET_IN_LINE;
			for (uint32 Line = GFX.StartY; Line <= GFX.EndY; Line++, Offset += GFX.PPL, l++)
			{
				int	yy, starty;

				int32	HOffset = ((int32) l->M7HOFS  << 19) >> 19;
				int32	VOffset = ((int32) l->M7VOFS  << 19) >> 19;

				int32	CentreX = ((int32) l->CentreX << 19) >> 19;
				int32	CentreY = ((int32) l->CentreY << 19) >> 19;

				if (PPU.Mode7VFlip)
					starty = 255 - (int) (Line + 1);
				else
					starty = Line + 1;

				yy = CLIP_10_BIT_SIGNED(VOffset - CentreY);

				int	BB = ((l->MatrixB * starty) & ~63) + ((l->MatrixB * yy) & ~63) + (CentreX << 8);
				int	DD = ((l->MatrixD * starty) & ~63) + ((l->MatrixD * yy) & ~63) + (CentreY << 8);

				if (PPU.Mode7HFlip)
				{
					// CBOC-Vita: g_wide_on is 1 only during the wide view's margin pass, which maps buffer column x to
					// screen x + g_wide_dx (the flip is about the 256 pixel screen)
					startx = g_wide_on ? 255 - ((int) Left + g_wide_dx) : Right - 1;
					aa = -l->MatrixA;
					cc = -l->MatrixC;
				}
				else
				{
					startx = g_wide_on ? (int) Left + g_wide_dx : Left;
					aa = l->MatrixA;
					cc = l->MatrixC;
				}

				int	xx = CLIP_10_BIT_SIGNED(HOffset - CentreX);
				int	AA = l->MatrixA * startx + ((l->MatrixA * xx) & ~63);
				int	CC = l->MatrixC * startx + ((l->MatrixC * xx) & ~63);

				uint8	Pix;

				if (PIXEL::Step == 1 && !OP::VARZ && __atomic_load_n(&g_s9x_fastpix, __ATOMIC_RELAXED))
				{
					// CBOC-Vita: the line in two steps. First the pixel bytes of the line are fetched into a buffer (a pixel the
					// loop below would skip, outside the plane with repeat off, is a 0 byte, which it skips as well), then
					// the buffer is drawn eight pixels at a time by DrawRow8, which does what the per-pixel plotter does
					// (same depth test, same colour maths, same stores). Same pixels, same depth, far fewer instructions.
					const uint8 z = (uint8) (D + 7);
					const uint8 rep = PPU.Mode7Repeat;
					uint8 pb[Mode7Chunk + 8];	// + the up to three extra pixels of the last vector step
					for (uint32 x0 = Left; x0 < Right; x0 += Mode7Chunk)
					{
						const uint32 n = (Right - x0) < Mode7Chunk ? (Right - x0) : Mode7Chunk;
						// the plane position of a pixel is linear in its index, so when the first and the last pixel of the run are
						// inside the plane (or the plane wraps) every pixel is: no per-pixel range test then
						const int A0 = AA + BB, C0 = CC + DD;
						const int Xa = A0 >> 8, Ya = C0 >> 8;
						const int Xb = (A0 + (int) (n - 1) * aa) >> 8, Yb = (C0 + (int) (n - 1) * cc) >> 8;
						if (!rep || (((Xa | Ya | Xb | Yb) & ~0x3ff) == 0))
						{
#if defined(__arm__) && !defined(__aarch64__)
							// On Cortex-A9 (Vita), NEON->ARM lane transfer stalls ~20 cycles per 4 pixels;
							// keep coordinate stepping in ARM GPRs, 4x unrolled, with BB/DD pre-folded into A0/C0.
							int A2 = A0, C2 = C0;
							uint32 i = 0;
							for (; i + 4 <= n; i += 4)
							{
								const int X0 = (A2 >> 8) & 0x3ff, Y0 = (C2 >> 8) & 0x3ff; A2 += aa; C2 += cc;
								const int X1 = (A2 >> 8) & 0x3ff, Y1 = (C2 >> 8) & 0x3ff; A2 += aa; C2 += cc;
								const int X2 = (A2 >> 8) & 0x3ff, Y2 = (C2 >> 8) & 0x3ff; A2 += aa; C2 += cc;
								const int X3 = (A2 >> 8) & 0x3ff, Y3 = (C2 >> 8) & 0x3ff; A2 += aa; C2 += cc;
								pb[i]     = VRAM1[(VRAM0[((Y0 & ~7) << 5) + ((X0 >> 2) & ~1)] << 7) + ((Y0 & 7) << 4) + ((X0 & 7) << 1)] & OP::MASK;
								pb[i + 1] = VRAM1[(VRAM0[((Y1 & ~7) << 5) + ((X1 >> 2) & ~1)] << 7) + ((Y1 & 7) << 4) + ((X1 & 7) << 1)] & OP::MASK;
								pb[i + 2] = VRAM1[(VRAM0[((Y2 & ~7) << 5) + ((X2 >> 2) & ~1)] << 7) + ((Y2 & 7) << 4) + ((X2 & 7) << 1)] & OP::MASK;
								pb[i + 3] = VRAM1[(VRAM0[((Y3 & ~7) << 5) + ((X3 >> 2) & ~1)] << 7) + ((Y3 & 7) << 4) + ((X3 & 7) << 1)] & OP::MASK;
							}
							for (; i < n; i++, A2 += aa, C2 += cc)
							{
								const int X = (A2 >> 8) & 0x3ff, Y = (C2 >> 8) & 0x3ff;
								pb[i] = VRAM1[(VRAM0[((Y & ~7) << 5) + ((X >> 2) & ~1)] << 7) + ((Y & 7) << 4) + ((X & 7) << 1)] & OP::MASK;
							}
#else
							// four pixels a step: the plane coordinates and the two byte addresses are made as vectors, the two
							// loads are scalar. A run is rounded up to four pixels: the extra ones read inside VRAM (the
							// coordinates are masked to the plane) and are never drawn
							const s9x_v4i lane = { 0, 1, 2, 3 };
							s9x_v4i Av = A0 + lane * aa, Cv = C0 + lane * cc;
							const s9x_v4i sa = { 4 * aa, 4 * aa, 4 * aa, 4 * aa }, sc = { 4 * cc, 4 * cc, 4 * cc, 4 * cc };
							for (uint32 i = 0; i < n; i += 4)
							{
								const s9x_v4i X = (Av >> 8) & 0x3ff;
								const s9x_v4i Y = (Cv >> 8) & 0x3ff;
								const s9x_v4i a1 = ((Y & ~7) << 5) + ((X >> 2) & ~1);
								const s9x_v4i a2 = ((Y & 7) << 4) + ((X & 7) << 1);
								int ia[4], ib[4];
								memcpy(ia, &a1, 16);
								memcpy(ib, &a2, 16);
								pb[i]     = VRAM1[(VRAM0[ia[0]] << 7) + ib[0]] & OP::MASK;
								pb[i + 1] = VRAM1[(VRAM0[ia[1]] << 7) + ib[1]] & OP::MASK;
								pb[i + 2] = VRAM1[(VRAM0[ia[2]] << 7) + ib[2]] & OP::MASK;
								pb[i + 3] = VRAM1[(VRAM0[ia[3]] << 7) + ib[3]] & OP::MASK;
								Av += sa; Cv += sc;
							}
#endif
						}
						else
						{
							int A2 = A0, C2 = C0;
							for (uint32 i = 0; i < n; i++, A2 += aa, C2 += cc)
							{
								const int X = A2 >> 8;
								const int Y = C2 >> 8;
								uint8 b;
								if (((X | Y) & ~0x3ff) == 0)
								{
									const uint8 *TileData = VRAM1 + (VRAM0[((Y & ~7) << 5) + ((X >> 2) & ~1)] << 7);
									b = *(TileData + ((Y & 7) << 4) + ((X & 7) << 1));
								}
								else if (rep == 3)
									b = *(VRAM1 + ((Y & 7) << 4) + ((X & 7) << 1));
								else
									b = 0;
								pb[i] = b & OP::MASK;
							}
						}
						AA += (int) n * aa; CC += (int) n * cc;
						uint32 i = 0;
						for (; i + 8 <= n; i += 8)
							DrawRow8<PIXEL, false>(ctx, pb + i, Offset + x0 + i, OffsetInLine, z, z);
						for (; i < n; i++)
							PIXEL::Draw(ctx, x0 + i, pb[i], Offset, OffsetInLine, pb[i], z, z);
					}
					continue;
				}

				if (!PPU.Mode7Repeat)
				{
					for (uint32 x = Left; x < Right; x++, AA += aa, CC += cc)
					{
						int	X = ((AA + BB) >> 8) & 0x3ff;
						int	Y = ((CC + DD) >> 8) & 0x3ff;

						const uint8	*TileData = VRAM1 + (VRAM0[((Y & ~7) << 5) + ((X >> 2) & ~1)] << 7);
						uint8	b = *(TileData + ((Y & 7) << 4) + ((X & 7) << 1));

						Pix = b & OP::MASK; DRAW_PIXEL(x, Pix);
					}
				}
				else
				{
					for (uint32 x = Left; x < Right; x++, AA += aa, CC += cc)
					{
						int	X = ((AA + BB) >> 8);
						int	Y = ((CC + DD) >> 8);

						uint8	b;

						if (((X | Y) & ~0x3ff) == 0)
						{
							const uint8	*TileData = VRAM1 + (VRAM0[((Y & ~7) << 5) + ((X >> 2) & ~1)] << 7);
							b = *(TileData + ((Y & 7) << 4) + ((X & 7) << 1));
						}
						else
						if (PPU.Mode7Repeat == 3)
							b = *(VRAM1    + ((Y & 7) << 4) + ((X & 7) << 1));
						else
							continue;

						Pix = b & OP::MASK; DRAW_PIXEL(x, Pix);
					}
				}
			}
		}
	};

	template<class PIXEL>
	struct DrawMode7BG1 : public DrawTileNormal<PIXEL, DrawMode7BG1_OP> {};
	template<class PIXEL>
	struct DrawMode7BG2 : public DrawTileNormal<PIXEL, DrawMode7BG2_OP> {};

	template<class PIXEL, class OP>
	struct DrawTileMosaic
	{
		typedef void (*call_t)(uint32 Left, uint32 Right, int D);

		static void Draw(uint32 Left, uint32 Right, int D)
		{
			const uint8	*VRAM0 = Memory.VRAM;
			const uint8	*VRAM1 = Memory.VRAM + 1;

			if (OP::DCMODE())
			{
				GFX.RealScreenColors = DirectColourMaps[0];
			}
			else
				GFX.RealScreenColors = IPPU.ScreenColors;

			GFX.ScreenColors = GFX.ClipColors ? BlackColourMap : GFX.RealScreenColors;
			PixCtx ctx;

			int	aa, cc;
			int	startx, StartY = GFX.StartY;

			int		HMosaic = 1, VMosaic = 1, MosaicStart = 0;
			int32	MLeft = Left, MRight = Right;

			if (PPU.BGMosaic[0])
			{
				VMosaic = PPU.Mosaic;
				MosaicStart = ((uint32) GFX.StartY - PPU.MosaicStart) % VMosaic;
				StartY -= MosaicStart;
			}

			if (PPU.BGMosaic[OP::BG])
			{
				HMosaic = PPU.Mosaic;
				if (g_wide_on)
				{
					int32 sL = (int32) Left + g_wide_dx;
					int32 sR = (int32) Right + g_wide_dx;
					int32 rL = sL % HMosaic;
					if (rL < 0)
						rL += HMosaic;
					sL -= rL;
					sR += HMosaic - 1;
					int32 rR = sR % HMosaic;
					if (rR < 0)
						rR += HMosaic;
					sR -= rR;
					MLeft  = sL - g_wide_dx;
					MRight = sR - g_wide_dx;
				}
				else
				{
					MLeft  -= MLeft  % HMosaic;
					MRight += HMosaic - 1;
					MRight -= MRight % HMosaic;
				}
			}

			uint32	Offset = StartY * GFX.PPL;
			struct SLineMatrixData	*l = &LineMatrixData[StartY];

			OFFSET_IN_LINE;
			for (uint32 Line = StartY; Line <= GFX.EndY; Line += VMosaic, Offset += VMosaic * GFX.PPL, l += VMosaic)
			{
				if (Line + VMosaic > GFX.EndY)
					VMosaic = GFX.EndY - Line + 1;

				int	yy, starty;

				int32	HOffset = ((int32) l->M7HOFS  << 19) >> 19;
				int32	VOffset = ((int32) l->M7VOFS  << 19) >> 19;

				int32	CentreX = ((int32) l->CentreX << 19) >> 19;
				int32	CentreY = ((int32) l->CentreY << 19) >> 19;

				if (PPU.Mode7VFlip)
					starty = 255 - (int) (Line + 1);
				else
					starty = Line + 1;

				yy = CLIP_10_BIT_SIGNED(VOffset - CentreY);

				int	BB = ((l->MatrixB * starty) & ~63) + ((l->MatrixB * yy) & ~63) + (CentreX << 8);
				int	DD = ((l->MatrixD * starty) & ~63) + ((l->MatrixD * yy) & ~63) + (CentreY << 8);

				if (PPU.Mode7HFlip)
				{
					startx = g_wide_on ? 255 - ((int) MLeft + g_wide_dx) : MRight - 1;
					aa = -l->MatrixA;
					cc = -l->MatrixC;
				}
				else
				{
					startx = g_wide_on ? (int) MLeft + g_wide_dx : MLeft;
					aa = l->MatrixA;
					cc = l->MatrixC;
				}

				int	xx = CLIP_10_BIT_SIGNED(HOffset - CentreX);
				int	AA = l->MatrixA * startx + ((l->MatrixA * xx) & ~63);
				int	CC = l->MatrixC * startx + ((l->MatrixC * xx) & ~63);

				uint8	Pix;
				uint8	ctr = 1;

				if (!PPU.Mode7Repeat)
				{
					for (int32 x = MLeft; x < MRight; x++, AA += aa, CC += cc)
					{
						if (--ctr)
							continue;
						ctr = HMosaic;

						int	X = ((AA + BB) >> 8) & 0x3ff;
						int	Y = ((CC + DD) >> 8) & 0x3ff;

						const uint8	*TileData = VRAM1 + (VRAM0[((Y & ~7) << 5) + ((X >> 2) & ~1)] << 7);
						uint8	b = *(TileData + ((Y & 7) << 4) + ((X & 7) << 1));

						if ((Pix = (b & OP::MASK)))
						{
							for (int32 h = MosaicStart; h < VMosaic; h++)
							{
								for (int32 w = x + HMosaic - 1; w >= x; w--)
									if (w >= (int32) Left && w < (int32) Right)
										DRAW_PIXEL(w + h * GFX.PPL, 1);
							}
						}
					}
				}
				else
				{
					for (int32 x = MLeft; x < MRight; x++, AA += aa, CC += cc)
					{
						if (--ctr)
							continue;
						ctr = HMosaic;

						int	X = ((AA + BB) >> 8);
						int	Y = ((CC + DD) >> 8);

						uint8	b;

						if (((X | Y) & ~0x3ff) == 0)
						{
							const uint8	*TileData = VRAM1 + (VRAM0[((Y & ~7) << 5) + ((X >> 2) & ~1)] << 7);
							b = *(TileData + ((Y & 7) << 4) + ((X & 7) << 1));
						}
						else
						if (PPU.Mode7Repeat == 3)
							b = *(VRAM1    + ((Y & 7) << 4) + ((X & 7) << 1));
						else
							continue;

						if ((Pix = (b & OP::MASK)))
						{
							for (int32 h = MosaicStart; h < VMosaic; h++)
							{
								for (int32 w = x + HMosaic - 1; w >= x; w--)
									if (w >= (int32) Left && w < (int32) Right)
										DRAW_PIXEL(w + h * GFX.PPL, 1);
							}
						}
					}
				}

				MosaicStart = 0;
			}
		}
	};

	template<class PIXEL>
	struct DrawMode7MosaicBG1 : public DrawTileMosaic<PIXEL, DrawMode7BG1_OP> {};
	template<class PIXEL>
	struct DrawMode7MosaicBG2 : public DrawTileMosaic<PIXEL, DrawMode7BG2_OP> {};

	#undef DRAW_PIXEL

} // namespace TileImpl

#endif
