// ****************************************************************************
// * This file is part of the HqMAME project. It is distributed under         *
// * GNU General Public License: http://www.gnu.org/licenses/gpl.html         *
// * Copyright (C) Zenju (zenju AT gmx DOT de) - All Rights Reserved          *
// *                                                                          *
// * Additionally and as a special exception, the author gives permission     *
// * to link the code of this program with the MAME library (or with modified *
// * versions of MAME that use the same license as MAME), and distribute      *
// * linked combinations including the two. You must obey the GNU General     *
// * Public License in all respects for all of the code used other than MAME. *
// * If you modify this file, you may extend this exception to your version   *
// * of the file, but you are not obligated to do so. If you do not wish to   *
// * do so, delete this exception statement from your version.                *
// ****************************************************************************

#include "xbrz.h"
#include <cmath>
#include <cassert>
#include <cstdlib>
#include <algorithm>
#include <limits>
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#endif

using xbrz::xreal;

namespace
{
template <uint32_t N> inline
unsigned char getByte(uint32_t val) { return static_cast<unsigned char>((val >> (8 * N)) & 0xff); }

// adjusted for RGBA
// - Durante
inline unsigned char getRed  (uint32_t pix) { return getByte<0>(pix); }
inline unsigned char getGreen(uint32_t pix) { return getByte<1>(pix); }
inline unsigned char getBlue (uint32_t pix) { return getByte<2>(pix); }
inline unsigned char getAlpha(uint32_t pix) { return getByte<3>(pix); }

inline uint32_t makePixel(                 unsigned char r, unsigned char g, unsigned char b) { return 0xff000000u | (b << 16) | (g << 8) | r; } /* GBAdhoc: opaque */
inline uint32_t makePixel(unsigned char a, unsigned char r, unsigned char g, unsigned char b) { return (a << 24) | (b << 16) | (g << 8) | r; }


template <unsigned int M, unsigned int N, bool IsPow2>
struct GradientRGBImpl
{
	static inline uint32_t calc(uint32_t pixFront, uint32_t pixBack)
	{
		auto calcColor = [](unsigned char colFront, unsigned char colBack) -> unsigned char { return static_cast<unsigned char>((colFront * M + colBack * (N - M)) / N); };
		return makePixel(calcColor(getRed  (pixFront), getRed  (pixBack)),
						 calcColor(getGreen(pixFront), getGreen(pixBack)),
						 calcColor(getBlue (pixFront), getBlue (pixBack)));
	}
};

template <unsigned int M, unsigned int N>
struct GradientRGBImpl<M, N, true>
{
	static inline uint32_t calc(uint32_t pixFront, uint32_t pixBack)
	{
		const uint32_t rb = (((pixFront & 0x00FF00FFu) * M + (pixBack & 0x00FF00FFu) * (N - M)) / N) & 0x00FF00FFu;
		const uint32_t g  = (((pixFront & 0x0000FF00u) * M + (pixBack & 0x0000FF00u) * (N - M)) / N) & 0x0000FF00u;
		return 0xFF000000u | rb | g;
	}
};

template <>
struct GradientRGBImpl<1, 2, true>
{
	static inline uint32_t calc(uint32_t pixFront, uint32_t pixBack)
	{
		return 0xFF000000u | ((pixFront & pixBack & 0x00FFFFFFu) + (((pixFront ^ pixBack) & 0x00FEFEFEu) >> 1));
	}
};

template <unsigned int M, unsigned int N> inline
uint32_t gradientRGB(uint32_t pixFront, uint32_t pixBack) //blend front color with opacity M / N over opaque background: http://en.wikipedia.org/wiki/Alpha_compositing#Alpha_blending
{
	static_assert(0 < M && M < N && N <= 1000, "");
	return GradientRGBImpl<M, N, ((N & (N - 1)) == 0) && (N <= 256)>::calc(pixFront, pixBack);
}


template <unsigned int M, unsigned int N> inline
uint32_t gradientARGB(uint32_t pixFront, uint32_t pixBack) //find intermediate color between two colors with alpha channels (=> NO alpha blending!!!)
{
	static_assert(0 < M && M < N && N <= 1000, "");

	const unsigned int weightFront = getAlpha(pixFront) * M;
	const unsigned int weightBack  = getAlpha(pixBack) * (N - M);
	const unsigned int weightSum   = weightFront + weightBack;
	if (weightSum == 0)
		return 0;

	auto calcColor = [=](unsigned char colFront, unsigned char colBack)
	{
		return static_cast<unsigned char>((colFront * weightFront + colBack * weightBack) / weightSum);
	};

	return makePixel(static_cast<unsigned char>(weightSum / N),
					 calcColor(getRed  (pixFront), getRed  (pixBack)),
					 calcColor(getGreen(pixFront), getGreen(pixBack)),
					 calcColor(getBlue (pixFront), getBlue (pixBack)));
}


//inline
//xreal fastSqrt(xreal n)
//{
//    __asm //speeds up xBRZ by about 9% compared to std::sqrt which internally uses the same assembler instructions but adds some "fluff"
//    {
//        fld n
//        fsqrt
//    }
//}
//


uint32_t*       byteAdvance(      uint32_t* ptr, int bytes) { return reinterpret_cast<      uint32_t*>(reinterpret_cast<      char*>(ptr) + bytes); }
const uint32_t* byteAdvance(const uint32_t* ptr, int bytes) { return reinterpret_cast<const uint32_t*>(reinterpret_cast<const char*>(ptr) + bytes); }


//fill block  with the given color
inline
void fillBlock(uint32_t* trg, int pitch, uint32_t col, int blockWidth, int blockHeight)
{
	for (int y = 0; y < blockHeight; ++y, trg = byteAdvance(trg, pitch))
		for (int x = 0; x < blockWidth; ++x)
			trg[x] = col;
}

template <int N>
struct FillBlockN
{
	static inline void fill(uint32_t* trg, int pitch, uint32_t col)
	{
		fillBlock(trg, pitch, col, N, N);
	}
};

template <>
struct FillBlockN<2>
{
	static inline void fill(uint32_t* trg, int pitch, uint32_t col)
	{
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
		const uint32x2_t v = vdup_n_u32(col);
		vst1_u32(trg, v); trg = byteAdvance(trg, pitch);
		vst1_u32(trg, v);
#else
		trg[0] = col; trg[1] = col; trg = byteAdvance(trg, pitch);
		trg[0] = col; trg[1] = col;
#endif
	}
};

template <>
struct FillBlockN<3>
{
	static inline void fill(uint32_t* trg, int pitch, uint32_t col)
	{
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
		const uint32x2_t v = vdup_n_u32(col);
		vst1_u32(trg, v); trg[2] = col; trg = byteAdvance(trg, pitch);
		vst1_u32(trg, v); trg[2] = col; trg = byteAdvance(trg, pitch);
		vst1_u32(trg, v); trg[2] = col;
#else
		trg[0] = col; trg[1] = col; trg[2] = col; trg = byteAdvance(trg, pitch);
		trg[0] = col; trg[1] = col; trg[2] = col; trg = byteAdvance(trg, pitch);
		trg[0] = col; trg[1] = col; trg[2] = col;
#endif
	}
};

template <>
struct FillBlockN<4>
{
	static inline void fill(uint32_t* trg, int pitch, uint32_t col)
	{
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
		const uint32x4_t v = vdupq_n_u32(col);
		vst1q_u32(trg, v); trg = byteAdvance(trg, pitch);
		vst1q_u32(trg, v); trg = byteAdvance(trg, pitch);
		vst1q_u32(trg, v); trg = byteAdvance(trg, pitch);
		vst1q_u32(trg, v);
#else
		trg[0] = col; trg[1] = col; trg[2] = col; trg[3] = col; trg = byteAdvance(trg, pitch);
		trg[0] = col; trg[1] = col; trg[2] = col; trg[3] = col; trg = byteAdvance(trg, pitch);
		trg[0] = col; trg[1] = col; trg[2] = col; trg[3] = col; trg = byteAdvance(trg, pitch);
		trg[0] = col; trg[1] = col; trg[2] = col; trg[3] = col;
#endif
	}
};


#ifdef _MSC_VER
#define FORCE_INLINE __forceinline
#elif defined __GNUC__
#define FORCE_INLINE __attribute__((always_inline)) inline
#else
#define FORCE_INLINE inline
#endif


enum RotationDegree //clock-wise
{
	ROT_0,
	ROT_90,
	ROT_180,
	ROT_270
};

//calculate input matrix coordinates after rotation at compile time
template <RotationDegree rotDeg, size_t I, size_t J, size_t N>
struct MatrixRotation;

template <size_t I, size_t J, size_t N>
struct MatrixRotation<ROT_0, I, J, N>
{
	static const size_t I_old = I;
	static const size_t J_old = J;
};

template <RotationDegree rotDeg, size_t I, size_t J, size_t N> //(i, j) = (row, col) indices, N = size of (square) matrix
struct MatrixRotation
{
	static const size_t I_old = N - 1 - MatrixRotation<static_cast<RotationDegree>(rotDeg - 1), I, J, N>::J_old; //old coordinates before rotation!
	static const size_t J_old =	        MatrixRotation<static_cast<RotationDegree>(rotDeg - 1), I, J, N>::I_old; //
};


template <size_t N, RotationDegree rotDeg>
class OutputMatrix
{
public:
	OutputMatrix(uint32_t* out, int outWidth) : //access matrix area, top-left at position "out" for image with given width
		out_(out),
		outWidth_(outWidth) {}

	template <size_t I, size_t J>
	uint32_t& ref() const
	{
		static const size_t I_old = MatrixRotation<rotDeg, I, J, N>::I_old;
		static const size_t J_old = MatrixRotation<rotDeg, I, J, N>::J_old;
		return *(out_ + J_old + I_old * outWidth_);
	}

private:
	uint32_t* out_;
	const int outWidth_;
};


#if 0
template <class T> inline
T square(T value) { return value * value; }

inline
xreal distRGB(uint32_t pix1, uint32_t pix2)
{
	const xreal r_diff = static_cast<int>(getRed  (pix1)) - getRed  (pix2);
	const xreal g_diff = static_cast<int>(getGreen(pix1)) - getGreen(pix2);
	const xreal b_diff = static_cast<int>(getBlue (pix1)) - getBlue (pix2);

	//euklidean RGB distance
	return std::sqrt(square(r_diff) + square(g_diff) + square(b_diff));
}

inline
xreal distYCbCr(uint32_t pix1, uint32_t pix2, xreal lumaWeight)
{
	const int r_diff = static_cast<int>(getRed  (pix1)) - getRed  (pix2);
	const int g_diff = static_cast<int>(getGreen(pix1)) - getGreen(pix2);
	const int b_diff = static_cast<int>(getBlue (pix1)) - getBlue (pix2);

	const xreal k_b = 0.0593f; //ITU-R BT.2020 conversion
	const xreal k_r = 0.2627f; //
	const xreal k_g = 1 - k_b - k_r;

	const xreal scale_b = 0.5f / (1 - k_b);
	const xreal scale_r = 0.5f / (1 - k_r);

	const xreal y   = k_r * r_diff + k_g * g_diff + k_b * b_diff;
	const xreal c_b = scale_b * (b_diff - y);
	const xreal c_r = scale_r * (r_diff - y);

	return std::sqrt(square(lumaWeight * y) + square(c_b) + square(c_r));
}
#endif


/* GBAdhoc: the original keeps a 256^3-float table (64 MB).  The sources here
   are RGB565 pictures expanded to 8 bits a channel, so a channel difference
   is a multiple of 8 (4 for green), give or take one: 65 * 129 * 65 floats,
   2.2 MB, each bucket centred on such a multiple.  Built once by
   xbrz::init(). */
enum { DIST_R = 65, DIST_G = 129, DIST_B = 65 };
static float *g_dist565;

/* GBAdhoc: two more ways to the same number, because a 2.2 MB table read at
   random is hard on a Cortex-A9's caches and only a console can say what
   that costs (upscale_vita.c upscale_bench times all three):
     1  a compact table: every channel taken at 5 bits (a Game Boy's colours
        are 15-bit; green's sixth bit repeats its first), 63^3 entries of 16
        bits, 500 KB, the distance in 64ths;
     2  no table: the arithmetic and one square root, a pixel pair at a time.
   The three agree to within a fraction of a level; which is in use never
   changes while a picture is being scaled (xbrz::setDistMode). */
enum { DIST5 = 63 };
static uint16_t *g_dist555;
static int       g_dist_mode;

static FORCE_INLINE xreal distYCbCr555Sq(int r_diff, int g_diff, int b_diff)
{
	const xreal k_b = 0.0593f; //ITU-R BT.2020 conversion
	const xreal k_r = 0.2627f; //
	const xreal k_g = 1 - k_b - k_r;

	const xreal scale_b = 0.5f / (1 - k_b);
	const xreal scale_r = 0.5f / (1 - k_r);

	const xreal y   = k_r * r_diff + k_g * g_diff + k_b * b_diff;
	const xreal c_b = scale_b * (b_diff - y);
	const xreal c_r = scale_r * (r_diff - y);
	return y * y + c_b * c_b + c_r * c_r;
}

static FORCE_INLINE xreal distYCbCr555(int r_diff, int g_diff, int b_diff)
{
	return std::sqrt(distYCbCr555Sq(r_diff, g_diff, b_diff));
}

struct DistYCbCrBuffer
{
	FORCE_INLINE static xreal distMode0(uint32_t pix1, uint32_t pix2)
	{
		if (pix1 == pix2)
			return 0;
		const int r_diff = static_cast<int>(getRed  (pix1)) - getRed  (pix2);
		const int g_diff = static_cast<int>(getGreen(pix1)) - getGreen(pix2);
		const int b_diff = static_cast<int>(getBlue (pix1)) - getBlue (pix2);
		return g_dist565[((r_diff + 260) >> 3) * (DIST_G * DIST_B) +
		                 ((g_diff + 258) >> 2) * DIST_B +
		                 ((b_diff + 260) >> 3)];
	}

	/* Mode 0 exact quantized squared distance: avoids both sqrt and the
	   2.2 MB g_dist565 cache-thrashing LUT for threshold/ordering tests in
	   blendPixel. */
	FORCE_INLINE static xreal distSqMode0(uint32_t pix1, uint32_t pix2)
	{
		if (pix1 == pix2)
			return 0;
		const int r_diff = static_cast<int>(getRed  (pix1)) - getRed  (pix2);
		const int g_diff = static_cast<int>(getGreen(pix1)) - getGreen(pix2);
		const int b_diff = static_cast<int>(getBlue (pix1)) - getBlue (pix2);
		const int qr = (((r_diff + 260) >> 3) - 32) * 8;
		const int qg = (((g_diff + 258) >> 2) - 64) * 4;
		const int qb = (((b_diff + 260) >> 3) - 32) * 8;
		return distYCbCr555Sq(qr, qg, qb);
	}

	FORCE_INLINE static uint16_t distRawMode1(uint32_t pix1, uint32_t pix2)
	{
		if (pix1 == pix2)
			return 0;
		const int r5 = static_cast<int>(getRed  (pix1) >> 3) - (getRed  (pix2) >> 3);
		const int g5 = static_cast<int>(getGreen(pix1) >> 3) - (getGreen(pix2) >> 3);
		const int b5 = static_cast<int>(getBlue (pix1) >> 3) - (getBlue (pix2) >> 3);
		return g_dist555[((r5 + 31) * DIST5 + (g5 + 31)) * DIST5 + (b5 + 31)];
	}

	FORCE_INLINE static xreal distMode1(uint32_t pix1, uint32_t pix2)
	{
		if (pix1 == pix2)
			return 0;
		return distRawMode1(pix1, pix2) * (1.0f / 64);
	}

	FORCE_INLINE static xreal distSqMode2(uint32_t pix1, uint32_t pix2)
	{
		if (pix1 == pix2)
			return 0;
		const int r_diff = static_cast<int>(getRed  (pix1)) - getRed  (pix2);
		const int g_diff = static_cast<int>(getGreen(pix1)) - getGreen(pix2);
		const int b_diff = static_cast<int>(getBlue (pix1)) - getBlue (pix2);
		return distYCbCr555Sq(r_diff, g_diff, b_diff);
	}

	FORCE_INLINE static xreal distMode2(uint32_t pix1, uint32_t pix2)
	{
		if (pix1 == pix2)
			return 0;
		return std::sqrt(distSqMode2(pix1, pix2));
	}

	/* Runtime-dispatched fallback (used for ARGB, 5x/6x, and equalColorTest). */
	FORCE_INLINE static xreal dist(uint32_t pix1, uint32_t pix2)
	{
		if (pix1 == pix2)
			return 0;
		if (g_dist_mode == 1 && g_dist555)
			return distMode1(pix1, pix2);
		const int r_diff = static_cast<int>(getRed  (pix1)) - getRed  (pix2);
		const int g_diff = static_cast<int>(getGreen(pix1)) - getGreen(pix2);
		const int b_diff = static_cast<int>(getBlue (pix1)) - getBlue (pix2);
		if (g_dist_mode == 2 || !g_dist565)
			return distYCbCr555(r_diff, g_diff, b_diff);
		return g_dist565[((r_diff + 260) >> 3) * (DIST_G * DIST_B) +
		                 ((g_diff + 258) >> 2) * DIST_B +
		                 ((b_diff + 260) >> 3)];
	}

	static void buildTable555()
	{
		uint16_t* t = static_cast<uint16_t*>(std::malloc(sizeof(uint16_t) * DIST5 * DIST5 * DIST5));
		if (!t)
			return;
		/* the compact table: a 5-bit step is 255/31 of an 8-bit one */
		const double step = 255.0 / 31.0;
		const double k_b = 0.0593;
		const double k_r = 0.2627;
		const double k_g = 1 - k_b - k_r;
		const double scale_b = 0.5 / (1 - k_b);
		const double scale_r = 0.5 / (1 - k_r);
		for (int ri = 0; ri < DIST5; ++ri)
			for (int gi = 0; gi < DIST5; ++gi)
				for (int bi = 0; bi < DIST5; ++bi)
				{
					const double r_diff = (ri - 31) * step;
					const double g_diff = (gi - 31) * step;
					const double b_diff = (bi - 31) * step;

					const double y   = k_r * r_diff + k_g * g_diff + k_b * b_diff;
					const double c_b = scale_b * (b_diff - y);
					const double c_r = scale_r * (r_diff - y);

					t[(ri * DIST5 + gi) * DIST5 + bi] =
						static_cast<uint16_t>(std::sqrt(y * y + c_b * c_b + c_r * c_r) * 64 + 0.5);
				}
		uint16_t* expected = nullptr;
		if (!__atomic_compare_exchange_n(&g_dist555, &expected, t, false, __ATOMIC_RELEASE, __ATOMIC_ACQUIRE))
			std::free(t);
	}

	static void buildTable565()
	{
		float* t = static_cast<float*>(std::malloc(sizeof(float) * DIST_R * DIST_G * DIST_B));
		if (!t)
			return;
		const double k_b = 0.0593; //ITU-R BT.2020 conversion
		const double k_r = 0.2627; //
		const double k_g = 1 - k_b - k_r;
		const double scale_b = 0.5 / (1 - k_b);
		const double scale_r = 0.5 / (1 - k_r);
		for (int ri = 0; ri < DIST_R; ++ri)
			for (int gi = 0; gi < DIST_G; ++gi)
				for (int bi = 0; bi < DIST_B; ++bi)
				{
					const double r_diff = (ri - 32) * 8;
					const double g_diff = (gi - 64) * 4;
					const double b_diff = (bi - 32) * 8;

					const double y   = k_r * r_diff + k_g * g_diff + k_b * b_diff;
					const double c_b = scale_b * (b_diff - y);
					const double c_r = scale_r * (r_diff - y);

					t[ri * (DIST_G * DIST_B) + gi * DIST_B + bi] =
						static_cast<float>(std::sqrt(y * y + c_b * c_b + c_r * c_r));
				}
		float* expected = nullptr;
		if (!__atomic_compare_exchange_n(&g_dist565, &expected, t, false, __ATOMIC_RELEASE, __ATOMIC_ACQUIRE))
			std::free(t);
	}

	static inline void ensure(int mode)
	{
		if (mode == 2)
			return;
		if (mode == 1)
		{
			if (!__atomic_load_n(&g_dist555, __ATOMIC_ACQUIRE))
				buildTable555();
			return;
		}
		if (!__atomic_load_n(&g_dist565, __ATOMIC_ACQUIRE))
			buildTable565();
	}

	static inline int resolveMode()
	{
		const int mode = __atomic_load_n(&g_dist_mode, __ATOMIC_ACQUIRE);
		ensure(mode);
		if (mode == 1)
		{
			if (__atomic_load_n(&g_dist555, __ATOMIC_ACQUIRE))
				return 1;
			return __atomic_load_n(&g_dist565, __ATOMIC_ACQUIRE) ? 0 : 2;
		}
		if (mode == 0)
			return __atomic_load_n(&g_dist565, __ATOMIC_ACQUIRE) ? 0 : 2;
		return 2;
	}

	static void build()
	{
		ensure(__atomic_load_n(&g_dist_mode, __ATOMIC_ACQUIRE));
	}
};


enum BlendType
{
	BLEND_NONE = 0,
	BLEND_NORMAL,   //a normal indication to blend
	BLEND_DOMINANT, //a strong indication to blend
	//attention: BlendType must fit into the value range of 2 bit!!!
};

struct BlendResult
{
	BlendType
	/**/blend_f, blend_g,
	/**/blend_j, blend_k;
};


struct Kernel_4x4 //kernel for preprocessing step
{
	uint32_t
	/**/a, b, c, d,
	/**/e, f, g, h,
	/**/i, j, k, l,
	/**/m, n, o, p;
};

/*
input kernel area naming convention:
-----------------
| A | B | C | D |
----|---|---|---|
| E | F | G | H |   //evaluate the four corners between F, G, J, K
----|---|---|---|   //input pixel is at position F
| I | J | K | L |
----|---|---|---|
| M | N | O | P |
-----------------
*/
template <class ColorDistance>
FORCE_INLINE //detect blend direction
BlendResult preProcessCorners(const Kernel_4x4& ker, const xbrz::ScalerCfg& cfg) //result: F, G, J, K corners of "GradientType"
{
	BlendResult result = {};

	if ((ker.f == ker.g &&
		 ker.j == ker.k) ||
		(ker.f == ker.j &&
		 ker.g == ker.k))
		return result;

	auto dist = [&](uint32_t pix1, uint32_t pix2) { return ColorDistance::dist(pix1, pix2, cfg.luminanceWeight); };

	const int weight = 4;
	xreal jg = dist(ker.i, ker.f) + dist(ker.f, ker.c) + dist(ker.n, ker.k) + dist(ker.k, ker.h) + weight * dist(ker.j, ker.g);
	xreal fk = dist(ker.e, ker.j) + dist(ker.j, ker.o) + dist(ker.b, ker.g) + dist(ker.g, ker.l) + weight * dist(ker.f, ker.k);

	if (jg < fk) //test sample: 70% of values max(jg, fk) / min(jg, fk) are between 1.1 and 3.7 with median being 1.8
	{
		const bool dominantGradient = cfg.dominantDirectionThreshold * jg < fk;
		if (ker.f != ker.g && ker.f != ker.j)
			result.blend_f = dominantGradient ? BLEND_DOMINANT : BLEND_NORMAL;

		if (ker.k != ker.j && ker.k != ker.g)
			result.blend_k = dominantGradient ? BLEND_DOMINANT : BLEND_NORMAL;
	}
	else if (fk < jg)
	{
		const bool dominantGradient = cfg.dominantDirectionThreshold * fk < jg;
		if (ker.j != ker.f && ker.j != ker.k)
			result.blend_j = dominantGradient ? BLEND_DOMINANT : BLEND_NORMAL;

		if (ker.g != ker.f && ker.g != ker.k)
			result.blend_g = dominantGradient ? BLEND_DOMINANT : BLEND_NORMAL;
	}
	return result;
}

FORCE_INLINE
BlendResult preProcessCornersCached(uint32_t f, uint32_t g, uint32_t j, uint32_t k,
                                    const xreal* jg_m1, const xreal* jg_0, const xreal* jg_p1,
                                    const xreal* fk_m1, const xreal* fk_0, const xreal* fk_p1,
                                    int idx, const xbrz::ScalerCfg& cfg)
{
	BlendResult result = {};

	if ((f == g && j == k) || (f == j && g == k))
		return result;

	const int weight = 4;
	const xreal jg = jg_0[idx - 1] + jg_m1[idx] + jg_p1[idx] + jg_0[idx + 1] + weight * jg_0[idx];
	const xreal fk = fk_0[idx - 1] + fk_p1[idx] + fk_m1[idx] + fk_0[idx + 1] + weight * fk_0[idx];

	if (jg < fk)
	{
		const bool dominantGradient = cfg.dominantDirectionThreshold * jg < fk;
		if (f != g && f != j)
			result.blend_f = dominantGradient ? BLEND_DOMINANT : BLEND_NORMAL;

		if (k != j && k != g)
			result.blend_k = dominantGradient ? BLEND_DOMINANT : BLEND_NORMAL;
	}
	else if (fk < jg)
	{
		const bool dominantGradient = cfg.dominantDirectionThreshold * fk < jg;
		if (j != f && j != k)
			result.blend_j = dominantGradient ? BLEND_DOMINANT : BLEND_NORMAL;

		if (g != f && g != k)
			result.blend_g = dominantGradient ? BLEND_DOMINANT : BLEND_NORMAL;
	}
	return result;
}

template <class ColorDistance>
static inline void fillDistRow(const uint32_t* rowA, const uint32_t* rowB,
                               int srcWidth, int cxStart, int cxCount,
                               xreal lw, xreal* jgOut, xreal* fkOut)
{
	const int cxEnd = cxStart + cxCount;
	int cx = cxStart;
	int i = 0;

	if (rowA == rowB)
	{
		if (cx < 0)
		{
			jgOut[i] = 0;
			fkOut[i] = 0;
			++cx;
			++i;
		}
		const int intEnd = std::min(cxEnd, srcWidth - 1);
		if (cx < intEnd)
		{
			uint32_t prev = rowA[cx];
			for (; cx < intEnd; ++cx, ++i)
			{
				const uint32_t cur = rowA[cx + 1];
				const xreal d = ColorDistance::dist(prev, cur, lw);
				jgOut[i] = d;
				fkOut[i] = d;
				prev = cur;
			}
		}
		for (; cx < cxEnd; ++cx, ++i)
		{
			jgOut[i] = 0;
			fkOut[i] = 0;
		}
		return;
	}

	if (cx < 0)
	{
		const uint32_t a0 = rowA[0];
		const uint32_t b0 = rowB[0];
		jgOut[i] = ColorDistance::dist(b0, a0, lw);
		fkOut[i] = ColorDistance::dist(a0, b0, lw);
		++cx;
		++i;
	}
	const int intEnd = std::min(cxEnd, srcWidth - 1);
	if (cx < intEnd)
	{
		uint32_t tl = rowA[cx];
		uint32_t bl = rowB[cx];
		for (; cx < intEnd; ++cx, ++i)
		{
			const uint32_t tr = rowA[cx + 1];
			const uint32_t br = rowB[cx + 1];
			jgOut[i] = ColorDistance::dist(bl, tr, lw);
			fkOut[i] = ColorDistance::dist(tl, br, lw);
			tl = tr;
			bl = br;
		}
	}
	if (cx < cxEnd)
	{
		const uint32_t aLast = rowA[srcWidth - 1];
		const uint32_t bLast = rowB[srcWidth - 1];
		const xreal dJG = ColorDistance::dist(bLast, aLast, lw);
		const xreal dFK = ColorDistance::dist(aLast, bLast, lw);
		for (; cx < cxEnd; ++cx, ++i)
		{
			jgOut[i] = dJG;
			fkOut[i] = dFK;
		}
	}
}

struct Kernel_3x3
{
	uint32_t
	/**/a,  b,  c,
	/**/d,  e,  f,
	/**/g,  h,  i;
};

#ifdef __GNUC__
#define XBRZ_MAYBE_UNUSED __attribute__((unused))
#else
#define XBRZ_MAYBE_UNUSED
#endif

#define DEF_GETTER(x) template <RotationDegree rotDeg> XBRZ_MAYBE_UNUSED uint32_t inline get_##x(const Kernel_3x3& ker) { return ker.x; }
//we cannot and NEED NOT write "ker.##x" since ## concatenates preprocessor tokens but "." is not a token
DEF_GETTER(a) DEF_GETTER(b) DEF_GETTER(c)
DEF_GETTER(d) DEF_GETTER(e) DEF_GETTER(f)
DEF_GETTER(g) DEF_GETTER(h) DEF_GETTER(i)
#undef DEF_GETTER

#define DEF_GETTER(x, y) template <> XBRZ_MAYBE_UNUSED inline uint32_t get_##x<ROT_90>(const Kernel_3x3& ker) { return ker.y; }
DEF_GETTER(a, g) DEF_GETTER(b, d) DEF_GETTER(c, a)
DEF_GETTER(d, h) DEF_GETTER(e, e) DEF_GETTER(f, b)
DEF_GETTER(g, i) DEF_GETTER(h, f) DEF_GETTER(i, c)
#undef DEF_GETTER

#define DEF_GETTER(x, y) template <> XBRZ_MAYBE_UNUSED inline uint32_t get_##x<ROT_180>(const Kernel_3x3& ker) { return ker.y; }
DEF_GETTER(a, i) DEF_GETTER(b, h) DEF_GETTER(c, g)
DEF_GETTER(d, f) DEF_GETTER(e, e) DEF_GETTER(f, d)
DEF_GETTER(g, c) DEF_GETTER(h, b) DEF_GETTER(i, a)
#undef DEF_GETTER

#define DEF_GETTER(x, y) template <> XBRZ_MAYBE_UNUSED inline uint32_t get_##x<ROT_270>(const Kernel_3x3& ker) { return ker.y; }
DEF_GETTER(a, c) DEF_GETTER(b, f) DEF_GETTER(c, i)
DEF_GETTER(d, b) DEF_GETTER(e, e) DEF_GETTER(f, h)
DEF_GETTER(g, a) DEF_GETTER(h, d) DEF_GETTER(i,	g)
#undef DEF_GETTER


//compress four blend types into a single byte
XBRZ_MAYBE_UNUSED inline BlendType getTopL   (unsigned char b) { return static_cast<BlendType>(0x3 & b); }
inline BlendType getTopR   (unsigned char b) { return static_cast<BlendType>(0x3 & (b >> 2)); }
inline BlendType getBottomR(unsigned char b) { return static_cast<BlendType>(0x3 & (b >> 4)); }
inline BlendType getBottomL(unsigned char b) { return static_cast<BlendType>(0x3 & (b >> 6)); }

inline void setTopL   (unsigned char& b, BlendType bt) { b |= bt; } //buffer is assumed to be initialized before preprocessing!
inline void setTopR   (unsigned char& b, BlendType bt) { b |= (bt << 2); }
inline void setBottomR(unsigned char& b, BlendType bt) { b |= (bt << 4); }
inline void setBottomL(unsigned char& b, BlendType bt) { b |= (bt << 6); }

inline bool blendingNeeded(unsigned char b) { return b != 0; }

template <RotationDegree rotDeg> inline
unsigned char rotateBlendInfo(unsigned char b) { return b; }
template <> inline unsigned char rotateBlendInfo<ROT_90 >(unsigned char b) { return ((b << 2) | (b >> 6)) & 0xff; }
template <> inline unsigned char rotateBlendInfo<ROT_180>(unsigned char b) { return ((b << 4) | (b >> 4)) & 0xff; }
template <> inline unsigned char rotateBlendInfo<ROT_270>(unsigned char b) { return ((b << 6) | (b >> 2)) & 0xff; }


#ifdef _DEBUG
int debugPixelX = -1;
int debugPixelY = 84;
bool breakIntoDebugger = false;
#endif


/*
input kernel area naming convention:
-------------
| A | B | C |
----|---|---|
| D | E | F | //input pixel is at position E
----|---|---|
| G | H | I |
-------------
*/
template <class Scaler, class ColorDistance, RotationDegree rotDeg>
FORCE_INLINE //perf: quite worth it!
void blendPixel(const Kernel_3x3& ker,
				uint32_t* target, int trgWidth,
				unsigned char blendInfo, //result of preprocessing all four corners of pixel "e"
				const xbrz::ScalerCfg& cfg)
{
#define a get_a<rotDeg>(ker)
#define b get_b<rotDeg>(ker)
#define c get_c<rotDeg>(ker)
#define d get_d<rotDeg>(ker)
#define e get_e<rotDeg>(ker)
#define f get_f<rotDeg>(ker)
#define g get_g<rotDeg>(ker)
#define h get_h<rotDeg>(ker)
#define i get_i<rotDeg>(ker)

#if defined(_DEBUG) && defined(_WIN32)
	if (breakIntoDebugger)
		__debugbreak(); //__asm int 3;
#endif

	const unsigned char blend = rotateBlendInfo<rotDeg>(blendInfo);

	if (getBottomR(blend) >= BLEND_NORMAL)
	{
		auto eq = [&](uint32_t pix1, uint32_t pix2) { return ColorDistance::eq(pix1, pix2, cfg); };

		const bool doLineBlend = [&]() -> bool
		{
			if (getBottomR(blend) >= BLEND_DOMINANT)
				return true;

			//make sure there is no second blending in an adjacent rotation for this pixel: handles insular pixels, mario eyes
			if (getTopR(blend) != BLEND_NONE && !eq(e, g)) //but support xreal-blending for 90?corners
				return false;
			if (getBottomL(blend) != BLEND_NONE && !eq(e, c))
				return false;

			//no full blending for L-shapes; blend corner only (handles "mario mushroom eyes")
			if (!eq(e, i) && eq(g, h) && eq(h , i) && eq(i, f) && eq(f, c))
				return false;

			return true;
		}();

		const uint32_t px = ColorDistance::distLe(e, f, e, h, cfg.luminanceWeight) ? f : h; //choose most similar color

		OutputMatrix<Scaler::scale, rotDeg> out(target, trgWidth);

		if (doLineBlend)
		{
			const bool canShallow = e != g && d != g;
			const bool canSteep   = e != c && b != c;
			bool haveShallowLine = false;
			bool haveSteepLine   = false;
			if (canShallow || canSteep)
			{
				bool shallowCond = false;
				bool steepCond   = false;
				ColorDistance::steepCheck(f, g, h, c, cfg, shallowCond, steepCond);
				haveShallowLine = canShallow && shallowCond;
				haveSteepLine   = canSteep && steepCond;
			}

			if (haveShallowLine)
			{
				if (haveSteepLine)
					Scaler::blendLineSteepAndShallow(px, out);
				else
					Scaler::blendLineShallow(px, out);
			}
			else
			{
				if (haveSteepLine)
					Scaler::blendLineSteep(px, out);
				else
					Scaler::blendLineDiagonal(px,out);
			}
		}
		else
			Scaler::blendCorner(px, out);
	}

#undef a
#undef b
#undef c
#undef d
#undef e
#undef f
#undef g
#undef h
#undef i
}


template <class Scaler, class ColorDistance> //scaler policy: see "Scaler2x" reference implementation
void scaleImage(const uint32_t* src, uint32_t* trg, int srcWidth, int srcHeight, const xbrz::ScalerCfg& cfg, int yFirst, int yLast,
                int xFirst = 0, int xLast = std::numeric_limits<int>::max(),
                uint32_t* trgAt = nullptr, int trgPitch = 0)
{
	const int yAsked = yFirst, xAsked = xFirst;
	yFirst = std::max(yFirst, 0);
	yLast  = std::min(yLast, srcHeight);
	xFirst = std::max(xFirst, 0);
	xLast  = std::min(xLast, srcWidth);
	if (yFirst >= yLast || srcWidth <= 0 || xFirst >= xLast)
		return;

	/* GBAdhoc: where the output goes.  By default the whole picture's own
	   layout (trgWidth pixels a row); with trgAt, source pixel (xAsked,
	   yAsked)'s block starts there and rows are trgPitch pixels apart -- a
	   rectangle written anywhere (vita/upscale_vita.c keeps the scaled
	   picture as a ring that a scroll only turns). */
	const int trgWidth = srcWidth * Scaler::scale;
	if (!trgAt)
	{
		trgPitch = trgWidth;
		trgAt = trg + Scaler::scale * (yFirst * trgWidth + xFirst);
	}
	else
		trgAt += Scaler::scale * ((yFirst - yAsked) * trgPitch + (xFirst - xAsked));

	/* GBAdhoc: columns [xFirst, xLast) only.  A pixel's four corners come
	   from the preprocessing of itself, its left neighbour, and the two
	   above them, so preprocessing starts one column early (xPre) and that
	   column is not drawn.  The original keeps its row of preprocessing
	   results in the far end of the stripe's own output, which a partial
	   row would leave scribbled on: here it is on the stack (the pictures
	   are at most 256 wide; wider ones keep the original's place and must
	   be scaled whole). */
	const int xPre = std::max(xFirst - 1, 0);
	const int bufferSize = srcWidth;
	unsigned char preProcStack[512];
	unsigned char* preProcBuffer = bufferSize <= static_cast<int>(sizeof(preProcStack)) ? preProcStack :
	                               reinterpret_cast<unsigned char*>(trg + yLast * Scaler::scale * trgWidth) - bufferSize;
	std::fill(preProcBuffer, preProcBuffer + bufferSize, 0);
	static_assert(BLEND_NONE == 0, "");

	/* GBAdhoc: 3-row L1-resident ring buffer of cell diagonal distances
	   (dJG, dFK), cutting preProcessCorners from 10 distance evaluations to 2
	   per pixel and avoiding loading Kernel_4x4 for flat/non-blended pixels. */
	enum { DIST_STACK_W = 514 };
	const int cxStart = xPre - 1;
	const int cxCount = (xLast - xPre) + 2;

	if (cxCount <= DIST_STACK_W)
	{
		xreal jgBuf[3][DIST_STACK_W];
		xreal fkBuf[3][DIST_STACK_W];
		const int yStart = yFirst > 0 ? yFirst - 1 : 0;
		auto rowPtr = [&](int ry) -> const uint32_t* {
			const int cy = ry < 0 ? 0 : (ry >= srcHeight ? srcHeight - 1 : ry);
			return src + srcWidth * cy;
		};

		/* Prime ring slots 0 (cy = yStart - 1) and 1 (cy = yStart). */
		fillDistRow<ColorDistance>(rowPtr(yStart - 1), rowPtr(yStart), srcWidth,
		                           cxStart, cxCount, cfg.luminanceWeight, jgBuf[0], fkBuf[0]);
		fillDistRow<ColorDistance>(rowPtr(yStart), rowPtr(yStart + 1), srcWidth,
		                           cxStart, cxCount, cfg.luminanceWeight, jgBuf[1], fkBuf[1]);
		int ringPos = 0; /* ringPos holds y-1, (ringPos+1)%3 holds y, (ringPos+2)%3 holds y+1 */

		if (yFirst > 0)
		{
			const int y = yFirst - 1;
			const int rM1 = ringPos;
			const int r0  = (ringPos + 1) % 3;
			const int rP1 = (ringPos + 2) % 3;
			fillDistRow<ColorDistance>(rowPtr(y + 1), rowPtr(y + 2), srcWidth,
			                           cxStart, cxCount, cfg.luminanceWeight, jgBuf[rP1], fkBuf[rP1]);

			const uint32_t* s_0  = rowPtr(y);
			const uint32_t* s_p1 = rowPtr(y + 1);
			const xreal* jg_m1 = jgBuf[rM1];
			const xreal* jg_0  = jgBuf[r0];
			const xreal* jg_p1 = jgBuf[rP1];
			const xreal* fk_m1 = fkBuf[rM1];
			const xreal* fk_0  = fkBuf[r0];
			const xreal* fk_p1 = fkBuf[rP1];

			const int xIntEnd = std::min(xLast, srcWidth - 1);
			int x = xPre;
			int idx = 1;
			if (x < xIntEnd)
			{
				uint32_t f = s_0[x];
				uint32_t j = s_p1[x];
				for (; x < xIntEnd; ++x, ++idx)
				{
					const uint32_t g = s_0[x + 1];
					const uint32_t k = s_p1[x + 1];
					if (!((f == g && j == k) || (f == j && g == k)))
					{
						const BlendResult res = preProcessCornersCached(
							f, g, j, k, jg_m1, jg_0, jg_p1, fk_m1, fk_0, fk_p1, idx, cfg);
						setTopR(preProcBuffer[x], res.blend_j);
						setTopL(preProcBuffer[x + 1], res.blend_k);
					}
					f = g;
					j = k;
				}
			}
			for (; x < xLast; ++x, ++idx)
			{
				const int x_p1 = std::min(x + 1, srcWidth - 1);
				const uint32_t f = s_0[x],  g = s_0[x_p1];
				const uint32_t j = s_p1[x], k = s_p1[x_p1];
				if (!((f == g && j == k) || (f == j && g == k)))
				{
					const BlendResult res = preProcessCornersCached(
						f, g, j, k, jg_m1, jg_0, jg_p1, fk_m1, fk_0, fk_p1, idx, cfg);
					setTopR(preProcBuffer[x], res.blend_j);
					if (x + 1 < bufferSize)
						setTopL(preProcBuffer[x + 1], res.blend_k);
				}
			}
			ringPos = r0;
		}

		for (int y = yFirst; y < yLast; ++y)
		{
			const int rM1 = ringPos;
			const int r0  = (ringPos + 1) % 3;
			const int rP1 = (ringPos + 2) % 3;
			fillDistRow<ColorDistance>(rowPtr(y + 1), rowPtr(y + 2), srcWidth,
			                           cxStart, cxCount, cfg.luminanceWeight, jgBuf[rP1], fkBuf[rP1]);
			ringPos = r0;

			uint32_t* out = trgAt + Scaler::scale * ((y - yFirst) * trgPitch + (xPre - xFirst));

			const uint32_t* s_m1 = rowPtr(y - 1);
			const uint32_t* s_0  = rowPtr(y);
			const uint32_t* s_p1 = rowPtr(y + 1);

			const xreal* jg_m1 = jgBuf[rM1];
			const xreal* jg_0  = jgBuf[r0];
			const xreal* jg_p1 = jgBuf[rP1];
			const xreal* fk_m1 = fkBuf[rM1];
			const xreal* fk_0  = fkBuf[r0];
			const xreal* fk_p1 = fkBuf[rP1];

			unsigned char blend_xy1 = 0;
			int x = xPre;
			int idx = 1;

			/* Prologue: handle x == xPre when it is prepare-only (x < xFirst)
			   or sits on the left/right image border. */
			if (x < xLast && (x < xFirst || x == 0 || x == srcWidth - 1))
			{
				const int x_m1 = std::max(x - 1, 0);
				const int x_p1 = std::min(x + 1, srcWidth - 1);
				const uint32_t f = s_0[x],  g = s_0[x_p1];
				const uint32_t j = s_p1[x], k = s_p1[x_p1];

				unsigned char blend_xy = preProcBuffer[x];
				if (!((f == g && j == k) || (f == j && g == k)))
				{
					const BlendResult res = preProcessCornersCached(
						f, g, j, k, jg_m1, jg_0, jg_p1, fk_m1, fk_0, fk_p1, idx, cfg);
					setBottomR(blend_xy, res.blend_f);
					setTopR(blend_xy1, res.blend_j);
					preProcBuffer[x] = blend_xy1;
					blend_xy1 = 0;
					setTopL(blend_xy1, res.blend_k);
					if (x + 1 < bufferSize)
						setBottomL(preProcBuffer[x + 1], res.blend_g);
				}
				else
				{
					preProcBuffer[x] = blend_xy1;
					blend_xy1 = 0;
				}

				if (x >= xFirst)
				{
					FillBlockN<Scaler::scale>::fill(out, trgPitch * static_cast<int>(sizeof(uint32_t)), f);
					if (blendingNeeded(blend_xy))
					{
						Kernel_3x3 ker3;
						ker3.a = s_m1[x_m1]; ker3.b = s_m1[x]; ker3.c = s_m1[x_p1];
						ker3.d = s_0[x_m1];  ker3.e = f;       ker3.f = g;
						ker3.g = s_p1[x_m1]; ker3.h = j;       ker3.i = k;

						blendPixel<Scaler, ColorDistance, ROT_0  >(ker3, out, trgPitch, blend_xy, cfg);
						blendPixel<Scaler, ColorDistance, ROT_90 >(ker3, out, trgPitch, blend_xy, cfg);
						blendPixel<Scaler, ColorDistance, ROT_180>(ker3, out, trgPitch, blend_xy, cfg);
						blendPixel<Scaler, ColorDistance, ROT_270>(ker3, out, trgPitch, blend_xy, cfg);
					}
				}
				++x;
				++idx;
				out += Scaler::scale;
			}

			/* Clamp-free interior: x >= max(xFirst, 1) and x < min(xLast, srcWidth - 1). */
			const int xIntEnd = std::min(xLast, srcWidth - 1);
			if (x < xIntEnd)
			{
				uint32_t f = s_0[x];
				uint32_t j = s_p1[x];
				for (; x < xIntEnd; ++x, ++idx, out += Scaler::scale)
				{
					const uint32_t g = s_0[x + 1];
					const uint32_t k = s_p1[x + 1];

					unsigned char blend_xy = preProcBuffer[x];
					if (!((f == g && j == k) || (f == j && g == k)))
					{
						const BlendResult res = preProcessCornersCached(
							f, g, j, k, jg_m1, jg_0, jg_p1, fk_m1, fk_0, fk_p1, idx, cfg);
						setBottomR(blend_xy, res.blend_f);
						setTopR(blend_xy1, res.blend_j);
						preProcBuffer[x] = blend_xy1;
						blend_xy1 = 0;
						setTopL(blend_xy1, res.blend_k);
						setBottomL(preProcBuffer[x + 1], res.blend_g);
					}
					else
					{
						preProcBuffer[x] = blend_xy1;
						blend_xy1 = 0;
					}

					FillBlockN<Scaler::scale>::fill(out, trgPitch * static_cast<int>(sizeof(uint32_t)), f);

					if (blendingNeeded(blend_xy))
					{
						Kernel_3x3 ker3;
						ker3.a = s_m1[x - 1]; ker3.b = s_m1[x]; ker3.c = s_m1[x + 1];
						ker3.d = s_0[x - 1];  ker3.e = f;       ker3.f = g;
						ker3.g = s_p1[x - 1]; ker3.h = j;       ker3.i = k;

						blendPixel<Scaler, ColorDistance, ROT_0  >(ker3, out, trgPitch, blend_xy, cfg);
						blendPixel<Scaler, ColorDistance, ROT_90 >(ker3, out, trgPitch, blend_xy, cfg);
						blendPixel<Scaler, ColorDistance, ROT_180>(ker3, out, trgPitch, blend_xy, cfg);
						blendPixel<Scaler, ColorDistance, ROT_270>(ker3, out, trgPitch, blend_xy, cfg);
					}
					f = g;
					j = k;
				}
			}

			/* Epilogue: right image border x == srcWidth - 1 (if included). */
			for (; x < xLast; ++x, ++idx, out += Scaler::scale)
			{
				const int x_m1 = std::max(x - 1, 0);
				const int x_p1 = std::min(x + 1, srcWidth - 1);
				const uint32_t f = s_0[x],  g = s_0[x_p1];
				const uint32_t j = s_p1[x], k = s_p1[x_p1];

				unsigned char blend_xy = preProcBuffer[x];
				if (!((f == g && j == k) || (f == j && g == k)))
				{
					const BlendResult res = preProcessCornersCached(
						f, g, j, k, jg_m1, jg_0, jg_p1, fk_m1, fk_0, fk_p1, idx, cfg);
					setBottomR(blend_xy, res.blend_f);
					setTopR(blend_xy1, res.blend_j);
					preProcBuffer[x] = blend_xy1;
					blend_xy1 = 0;
					setTopL(blend_xy1, res.blend_k);
					if (x + 1 < bufferSize)
						setBottomL(preProcBuffer[x + 1], res.blend_g);
				}
				else
				{
					preProcBuffer[x] = blend_xy1;
					blend_xy1 = 0;
				}

				if (x >= xFirst)
				{
					FillBlockN<Scaler::scale>::fill(out, trgPitch * static_cast<int>(sizeof(uint32_t)), f);
					if (blendingNeeded(blend_xy))
					{
						Kernel_3x3 ker3;
						ker3.a = s_m1[x_m1]; ker3.b = s_m1[x]; ker3.c = s_m1[x_p1];
						ker3.d = s_0[x_m1];  ker3.e = f;       ker3.f = g;
						ker3.g = s_p1[x_m1]; ker3.h = j;       ker3.i = k;

						blendPixel<Scaler, ColorDistance, ROT_0  >(ker3, out, trgPitch, blend_xy, cfg);
						blendPixel<Scaler, ColorDistance, ROT_90 >(ker3, out, trgPitch, blend_xy, cfg);
						blendPixel<Scaler, ColorDistance, ROT_180>(ker3, out, trgPitch, blend_xy, cfg);
						blendPixel<Scaler, ColorDistance, ROT_270>(ker3, out, trgPitch, blend_xy, cfg);
					}
				}
			}
		}
		return;
	}

	// Fallback for wide images (srcWidth > 512): test 2x2 flat fast-out before loading Kernel_4x4.
	if (yFirst > 0)
	{
		const int y = yFirst - 1;

		const uint32_t* s_m1 = src + srcWidth * std::max(y - 1, 0);
		const uint32_t* s_0  = src + srcWidth * y;
		const uint32_t* s_p1 = src + srcWidth * std::min(y + 1, srcHeight - 1);
		const uint32_t* s_p2 = src + srcWidth * std::min(y + 2, srcHeight - 1);

		for (int x = xPre; x < xLast; ++x)
		{
			const int x_p1 = std::min(x + 1, srcWidth - 1);
			const uint32_t f = s_0[x],  g = s_0[x_p1];
			const uint32_t j = s_p1[x], k = s_p1[x_p1];
			if ((f == g && j == k) || (f == j && g == k))
				continue;

			const int x_m1 = std::max(x - 1, 0);
			const int x_p2 = std::min(x + 2, srcWidth - 1);

			Kernel_4x4 ker;
			ker.a = s_m1[x_m1]; ker.b = s_m1[x]; ker.c = s_m1[x_p1]; ker.d = s_m1[x_p2];
			ker.e = s_0[x_m1];  ker.f = f;       ker.g = g;          ker.h = s_0[x_p2];
			ker.i = s_p1[x_m1]; ker.j = j;       ker.k = k;          ker.l = s_p1[x_p2];
			ker.m = s_p2[x_m1]; ker.n = s_p2[x]; ker.o = s_p2[x_p1]; ker.p = s_p2[x_p2];

			const BlendResult res = preProcessCorners<ColorDistance>(ker, cfg);
			setTopR(preProcBuffer[x], res.blend_j);
			if (x + 1 < bufferSize)
				setTopL(preProcBuffer[x + 1], res.blend_k);
		}
	}

	for (int y = yFirst; y < yLast; ++y)
	{
		uint32_t* out = trgAt + Scaler::scale * ((y - yFirst) * trgPitch + (xPre - xFirst));

		const uint32_t* s_m1 = src + srcWidth * std::max(y - 1, 0);
		const uint32_t* s_0  = src + srcWidth * y;
		const uint32_t* s_p1 = src + srcWidth * std::min(y + 1, srcHeight - 1);
		const uint32_t* s_p2 = src + srcWidth * std::min(y + 2, srcHeight - 1);

		unsigned char blend_xy1 = 0;

		for (int x = xPre; x < xLast; ++x, out += Scaler::scale)
		{
			const int x_m1 = std::max(x - 1, 0);
			const int x_p1 = std::min(x + 1, srcWidth - 1);
			const uint32_t f = s_0[x],  g = s_0[x_p1];
			const uint32_t j = s_p1[x], k = s_p1[x_p1];

			unsigned char blend_xy = preProcBuffer[x];
			if (!((f == g && j == k) || (f == j && g == k)))
			{
				const int x_p2 = std::min(x + 2, srcWidth - 1);
				Kernel_4x4 ker4;
				ker4.a = s_m1[x_m1]; ker4.b = s_m1[x]; ker4.c = s_m1[x_p1]; ker4.d = s_m1[x_p2];
				ker4.e = s_0[x_m1];  ker4.f = f;       ker4.g = g;          ker4.h = s_0[x_p2];
				ker4.i = s_p1[x_m1]; ker4.j = j;       ker4.k = k;          ker4.l = s_p1[x_p2];
				ker4.m = s_p2[x_m1]; ker4.n = s_p2[x]; ker4.o = s_p2[x_p1]; ker4.p = s_p2[x_p2];

				const BlendResult res = preProcessCorners<ColorDistance>(ker4, cfg);
				setBottomR(blend_xy, res.blend_f);
				setTopR(blend_xy1, res.blend_j);
				preProcBuffer[x] = blend_xy1;
				blend_xy1 = 0;
				setTopL(blend_xy1, res.blend_k);
				if (x + 1 < bufferSize)
					setBottomL(preProcBuffer[x + 1], res.blend_g);
			}
			else
			{
				preProcBuffer[x] = blend_xy1;
				blend_xy1 = 0;
			}

			if (x < xFirst)
				continue;

			FillBlockN<Scaler::scale>::fill(out, trgPitch * static_cast<int>(sizeof(uint32_t)), f);

			if (blendingNeeded(blend_xy))
			{
				Kernel_3x3 ker3;
				ker3.a = s_m1[x_m1]; ker3.b = s_m1[x]; ker3.c = s_m1[x_p1];
				ker3.d = s_0[x_m1];  ker3.e = f;       ker3.f = g;
				ker3.g = s_p1[x_m1]; ker3.h = j;       ker3.i = k;

				blendPixel<Scaler, ColorDistance, ROT_0  >(ker3, out, trgPitch, blend_xy, cfg);
				blendPixel<Scaler, ColorDistance, ROT_90 >(ker3, out, trgPitch, blend_xy, cfg);
				blendPixel<Scaler, ColorDistance, ROT_180>(ker3, out, trgPitch, blend_xy, cfg);
				blendPixel<Scaler, ColorDistance, ROT_270>(ker3, out, trgPitch, blend_xy, cfg);
			}
		}
	}
}

//------------------------------------------------------------------------------------

template <class ColorGradient>
struct Scaler2x : public ColorGradient
{
	static const int scale = 2;

	template <unsigned int M, unsigned int N> //bring template function into scope for GCC
	static void alphaGrad(uint32_t& pixBack, uint32_t pixFront) { ColorGradient::template alphaGrad<M, N>(pixBack, pixFront); }


	template <class OutputMatrix>
	static void blendLineShallow(uint32_t col, OutputMatrix& out)
	{
		alphaGrad<1, 4>(out.template ref<scale - 1, 0>(), col);
		alphaGrad<3, 4>(out.template ref<scale - 1, 1>(), col);
	}

	template <class OutputMatrix>
	static void blendLineSteep(uint32_t col, OutputMatrix& out)
	{
		alphaGrad<1, 4>(out.template ref<0, scale - 1>(), col);
		alphaGrad<3, 4>(out.template ref<1, scale - 1>(), col);
	}

	template <class OutputMatrix>
	static void blendLineSteepAndShallow(uint32_t col, OutputMatrix& out)
	{
		alphaGrad<1, 4>(out.template ref<1, 0>(), col);
		alphaGrad<1, 4>(out.template ref<0, 1>(), col);
		alphaGrad<5, 6>(out.template ref<1, 1>(), col); //[!] fixes 7/8 used in xBR
	}

	template <class OutputMatrix>
	static void blendLineDiagonal(uint32_t col, OutputMatrix& out)
	{
		alphaGrad<1, 2>(out.template ref<1, 1>(), col);
	}

	template <class OutputMatrix>
	static void blendCorner(uint32_t col, OutputMatrix& out)
	{
		//model a round corner
		alphaGrad<21, 100>(out.template ref<1, 1>(), col); //exact: 1 - pi/4 = 0.2146018366
	}
};


template <class ColorGradient>
struct Scaler3x : public ColorGradient
{
	static const int scale = 3;

	template <unsigned int M, unsigned int N> //bring template function into scope for GCC
	static void alphaGrad(uint32_t& pixBack, uint32_t pixFront) { ColorGradient::template alphaGrad<M, N>(pixBack, pixFront); }


	template <class OutputMatrix>
	static void blendLineShallow(uint32_t col, OutputMatrix& out)
	{
		alphaGrad<1, 4>(out.template ref<scale - 1, 0>(), col);
		alphaGrad<1, 4>(out.template ref<scale - 2, 2>(), col);

		alphaGrad<3, 4>(out.template ref<scale - 1, 1>(), col);
		out.template ref<scale - 1, 2>() = col;
	}

	template <class OutputMatrix>
	static void blendLineSteep(uint32_t col, OutputMatrix& out)
	{
		alphaGrad<1, 4>(out.template ref<0, scale - 1>(), col);
		alphaGrad<1, 4>(out.template ref<2, scale - 2>(), col);

		alphaGrad<3, 4>(out.template ref<1, scale - 1>(), col);
		out.template ref<2, scale - 1>() = col;
	}

	template <class OutputMatrix>
	static void blendLineSteepAndShallow(uint32_t col, OutputMatrix& out)
	{
		alphaGrad<1, 4>(out.template ref<2, 0>(), col);
		alphaGrad<1, 4>(out.template ref<0, 2>(), col);
		alphaGrad<3, 4>(out.template ref<2, 1>(), col);
		alphaGrad<3, 4>(out.template ref<1, 2>(), col);
		out.template ref<2, 2>() = col;
	}

	template <class OutputMatrix>
	static void blendLineDiagonal(uint32_t col, OutputMatrix& out)
	{
		alphaGrad<1, 8>(out.template ref<1, 2>(), col); //conflict with other rotations for this odd scale
		alphaGrad<1, 8>(out.template ref<2, 1>(), col);
		alphaGrad<7, 8>(out.template ref<2, 2>(), col); //
	}

	template <class OutputMatrix>
	static void blendCorner(uint32_t col, OutputMatrix& out)
	{
		//model a round corner
		alphaGrad<45, 100>(out.template ref<2, 2>(), col); //exact: 0.4545939598
		//alphaGrad<7, 256>(out.template ref<2, 1>(), col); //0.02826017254 -> negligible + avoid conflicts with other rotations for this odd scale
		//alphaGrad<7, 256>(out.template ref<1, 2>(), col); //0.02826017254
	}
};


template <class ColorGradient>
struct Scaler4x : public ColorGradient
{
	static const int scale = 4;

	template <unsigned int M, unsigned int N> //bring template function into scope for GCC
	static void alphaGrad(uint32_t& pixBack, uint32_t pixFront) { ColorGradient::template alphaGrad<M, N>(pixBack, pixFront); }


	template <class OutputMatrix>
	static void blendLineShallow(uint32_t col, OutputMatrix& out)
	{
		alphaGrad<1, 4>(out.template ref<scale - 1, 0>(), col);
		alphaGrad<1, 4>(out.template ref<scale - 2, 2>(), col);

		alphaGrad<3, 4>(out.template ref<scale - 1, 1>(), col);
		alphaGrad<3, 4>(out.template ref<scale - 2, 3>(), col);

		out.template ref<scale - 1, 2>() = col;
		out.template ref<scale - 1, 3>() = col;
	}

	template <class OutputMatrix>
	static void blendLineSteep(uint32_t col, OutputMatrix& out)
	{
		alphaGrad<1, 4>(out.template ref<0, scale - 1>(), col);
		alphaGrad<1, 4>(out.template ref<2, scale - 2>(), col);

		alphaGrad<3, 4>(out.template ref<1, scale - 1>(), col);
		alphaGrad<3, 4>(out.template ref<3, scale - 2>(), col);

		out.template ref<2, scale - 1>() = col;
		out.template ref<3, scale - 1>() = col;
	}

	template <class OutputMatrix>
	static void blendLineSteepAndShallow(uint32_t col, OutputMatrix& out)
	{
		alphaGrad<3, 4>(out.template ref<3, 1>(), col);
		alphaGrad<3, 4>(out.template ref<1, 3>(), col);
		alphaGrad<1, 4>(out.template ref<3, 0>(), col);
		alphaGrad<1, 4>(out.template ref<0, 3>(), col);

		alphaGrad<1, 3>(out.template ref<2, 2>(), col); //[!] fixes 1/4 used in xBR

		out.template ref<3, 3>() = col;
		out.template ref<3, 2>() = col;
		out.template ref<2, 3>() = col;
	}

	template <class OutputMatrix>
	static void blendLineDiagonal(uint32_t col, OutputMatrix& out)
	{
		alphaGrad<1, 2>(out.template ref<scale - 1, scale / 2    >(), col);
		alphaGrad<1, 2>(out.template ref<scale - 2, scale / 2 + 1>(), col);
		out.template ref<scale - 1, scale - 1>() = col;
	}

	template <class OutputMatrix>
	static void blendCorner(uint32_t col, OutputMatrix& out)
	{
		//model a round corner
		alphaGrad<68, 100>(out.template ref<3, 3>(), col); //exact: 0.6848532563
		alphaGrad< 9, 100>(out.template ref<3, 2>(), col); //0.08677704501
		alphaGrad< 9, 100>(out.template ref<2, 3>(), col); //0.08677704501
	}
};


template <class ColorGradient>
struct Scaler5x : public ColorGradient
{
	static const int scale = 5;

	template <unsigned int M, unsigned int N> //bring template function into scope for GCC
	static void alphaGrad(uint32_t& pixBack, uint32_t pixFront) { ColorGradient::template alphaGrad<M, N>(pixBack, pixFront); }


	template <class OutputMatrix>
	static void blendLineShallow(uint32_t col, OutputMatrix& out)
	{
		alphaGrad<1, 4>(out.template ref<scale - 1, 0>(), col);
		alphaGrad<1, 4>(out.template ref<scale - 2, 2>(), col);
		alphaGrad<1, 4>(out.template ref<scale - 3, 4>(), col);

		alphaGrad<3, 4>(out.template ref<scale - 1, 1>(), col);
		alphaGrad<3, 4>(out.template ref<scale - 2, 3>(), col);

		out.template ref<scale - 1, 2>() = col;
		out.template ref<scale - 1, 3>() = col;
		out.template ref<scale - 1, 4>() = col;
		out.template ref<scale - 2, 4>() = col;
	}

	template <class OutputMatrix>
	static void blendLineSteep(uint32_t col, OutputMatrix& out)
	{
		alphaGrad<1, 4>(out.template ref<0, scale - 1>(), col);
		alphaGrad<1, 4>(out.template ref<2, scale - 2>(), col);
		alphaGrad<1, 4>(out.template ref<4, scale - 3>(), col);

		alphaGrad<3, 4>(out.template ref<1, scale - 1>(), col);
		alphaGrad<3, 4>(out.template ref<3, scale - 2>(), col);

		out.template ref<2, scale - 1>() = col;
		out.template ref<3, scale - 1>() = col;
		out.template ref<4, scale - 1>() = col;
		out.template ref<4, scale - 2>() = col;
	}

	template <class OutputMatrix>
	static void blendLineSteepAndShallow(uint32_t col, OutputMatrix& out)
	{
		alphaGrad<1, 4>(out.template ref<0, scale - 1>(), col);
		alphaGrad<1, 4>(out.template ref<2, scale - 2>(), col);
		alphaGrad<3, 4>(out.template ref<1, scale - 1>(), col);

		alphaGrad<1, 4>(out.template ref<scale - 1, 0>(), col);
		alphaGrad<1, 4>(out.template ref<scale - 2, 2>(), col);
		alphaGrad<3, 4>(out.template ref<scale - 1, 1>(), col);

		alphaGrad<2, 3>(out.template ref<3, 3>(), col);

		out.template ref<2, scale - 1>() = col;
		out.template ref<3, scale - 1>() = col;
		out.template ref<4, scale - 1>() = col;

		out.template ref<scale - 1, 2>() = col;
		out.template ref<scale - 1, 3>() = col;
	}

	template <class OutputMatrix>
	static void blendLineDiagonal(uint32_t col, OutputMatrix& out)
	{
		alphaGrad<1, 8>(out.template ref<scale - 1, scale / 2    >(), col); //conflict with other rotations for this odd scale
		alphaGrad<1, 8>(out.template ref<scale - 2, scale / 2 + 1>(), col);
		alphaGrad<1, 8>(out.template ref<scale - 3, scale / 2 + 2>(), col); //

		alphaGrad<7, 8>(out.template ref<4, 3>(), col);
		alphaGrad<7, 8>(out.template ref<3, 4>(), col);

		out.template ref<4, 4>() = col;
	}

	template <class OutputMatrix>
	static void blendCorner(uint32_t col, OutputMatrix& out)
	{
		//model a round corner
		alphaGrad<86, 100>(out.template ref<4, 4>(), col); //exact: 0.8631434088
		alphaGrad<23, 100>(out.template ref<4, 3>(), col); //0.2306749731
		alphaGrad<23, 100>(out.template ref<3, 4>(), col); //0.2306749731
		//alphaGrad<1, 64>(out.template ref<4, 2>(), col); //0.01676812367 -> negligible + avoid conflicts with other rotations for this odd scale
		//alphaGrad<1, 64>(out.template ref<2, 4>(), col); //0.01676812367
	}
};


template <class ColorGradient>
struct Scaler6x : public ColorGradient
{
	static const int scale = 6;

	template <unsigned int M, unsigned int N> //bring template function into scope for GCC
	static void alphaGrad(uint32_t& pixBack, uint32_t pixFront) { ColorGradient::template alphaGrad<M, N>(pixBack, pixFront); }


	template <class OutputMatrix>
	static void blendLineShallow(uint32_t col, OutputMatrix& out)
	{
		alphaGrad<1, 4>(out.template ref<scale - 1, 0>(), col);
		alphaGrad<1, 4>(out.template ref<scale - 2, 2>(), col);
		alphaGrad<1, 4>(out.template ref<scale - 3, 4>(), col);

		alphaGrad<3, 4>(out.template ref<scale - 1, 1>(), col);
		alphaGrad<3, 4>(out.template ref<scale - 2, 3>(), col);
		alphaGrad<3, 4>(out.template ref<scale - 3, 5>(), col);

		out.template ref<scale - 1, 2>() = col;
		out.template ref<scale - 1, 3>() = col;
		out.template ref<scale - 1, 4>() = col;
		out.template ref<scale - 1, 5>() = col;

		out.template ref<scale - 2, 4>() = col;
		out.template ref<scale - 2, 5>() = col;
	}

	template <class OutputMatrix>
	static void blendLineSteep(uint32_t col, OutputMatrix& out)
	{
		alphaGrad<1, 4>(out.template ref<0, scale - 1>(), col);
		alphaGrad<1, 4>(out.template ref<2, scale - 2>(), col);
		alphaGrad<1, 4>(out.template ref<4, scale - 3>(), col);

		alphaGrad<3, 4>(out.template ref<1, scale - 1>(), col);
		alphaGrad<3, 4>(out.template ref<3, scale - 2>(), col);
		alphaGrad<3, 4>(out.template ref<5, scale - 3>(), col);

		out.template ref<2, scale - 1>() = col;
		out.template ref<3, scale - 1>() = col;
		out.template ref<4, scale - 1>() = col;
		out.template ref<5, scale - 1>() = col;

		out.template ref<4, scale - 2>() = col;
		out.template ref<5, scale - 2>() = col;
	}

	template <class OutputMatrix>
	static void blendLineSteepAndShallow(uint32_t col, OutputMatrix& out)
	{
		alphaGrad<1, 4>(out.template ref<0, scale - 1>(), col);
		alphaGrad<1, 4>(out.template ref<2, scale - 2>(), col);
		alphaGrad<3, 4>(out.template ref<1, scale - 1>(), col);
		alphaGrad<3, 4>(out.template ref<3, scale - 2>(), col);

		alphaGrad<1, 4>(out.template ref<scale - 1, 0>(), col);
		alphaGrad<1, 4>(out.template ref<scale - 2, 2>(), col);
		alphaGrad<3, 4>(out.template ref<scale - 1, 1>(), col);
		alphaGrad<3, 4>(out.template ref<scale - 2, 3>(), col);

		out.template ref<2, scale - 1>() = col;
		out.template ref<3, scale - 1>() = col;
		out.template ref<4, scale - 1>() = col;
		out.template ref<5, scale - 1>() = col;

		out.template ref<4, scale - 2>() = col;
		out.template ref<5, scale - 2>() = col;

		out.template ref<scale - 1, 2>() = col;
		out.template ref<scale - 1, 3>() = col;
	}

	template <class OutputMatrix>
	static void blendLineDiagonal(uint32_t col, OutputMatrix& out)
	{
		alphaGrad<1, 2>(out.template ref<scale - 1, scale / 2    >(), col);
		alphaGrad<1, 2>(out.template ref<scale - 2, scale / 2 + 1>(), col);
		alphaGrad<1, 2>(out.template ref<scale - 3, scale / 2 + 2>(), col);

		out.template ref<scale - 2, scale - 1>() = col;
		out.template ref<scale - 1, scale - 1>() = col;
		out.template ref<scale - 1, scale - 2>() = col;
	}

	template <class OutputMatrix>
	static void blendCorner(uint32_t col, OutputMatrix& out)
	{
		//model a round corner
		alphaGrad<97, 100>(out.template ref<5, 5>(), col); //exact: 0.9711013910
		alphaGrad<42, 100>(out.template ref<4, 5>(), col); //0.4236372243
		alphaGrad<42, 100>(out.template ref<5, 4>(), col); //0.4236372243
		alphaGrad< 6, 100>(out.template ref<5, 3>(), col); //0.05652034508
		alphaGrad< 6, 100>(out.template ref<3, 5>(), col); //0.05652034508
	}
};

//------------------------------------------------------------------------------------

template <int DistMode>
struct ColorDistanceRGB;

template <>
struct ColorDistanceRGB<0>
{
	FORCE_INLINE static xreal dist(uint32_t pix1, uint32_t pix2, xreal luminanceWeight)
	{
		(void)luminanceWeight;
		return DistYCbCrBuffer::distMode0(pix1, pix2);
	}
	FORCE_INLINE static bool eq(uint32_t pix1, uint32_t pix2, const xbrz::ScalerCfg& cfg)
	{
		if (pix1 == pix2)
			return true;
		return DistYCbCrBuffer::distSqMode0(pix1, pix2) < cfg.equalColorTolerance * cfg.equalColorTolerance;
	}
	FORCE_INLINE static bool distLe(uint32_t a1, uint32_t a2, uint32_t b1, uint32_t b2, xreal lw)
	{
		(void)lw;
		return DistYCbCrBuffer::distSqMode0(a1, a2) <= DistYCbCrBuffer::distSqMode0(b1, b2);
	}
	FORCE_INLINE static void steepCheck(uint32_t f, uint32_t g, uint32_t h, uint32_t c,
	                                    const xbrz::ScalerCfg& cfg, bool& shallowCond, bool& steepCond)
	{
		const xreal fgSq = DistYCbCrBuffer::distSqMode0(f, g);
		const xreal hcSq = DistYCbCrBuffer::distSqMode0(h, c);
		const xreal sSq  = cfg.steepDirectionThreshold * cfg.steepDirectionThreshold;
		shallowCond = sSq * fgSq <= hcSq;
		steepCond   = sSq * hcSq <= fgSq;
	}
};

template <>
struct ColorDistanceRGB<1>
{
	FORCE_INLINE static xreal dist(uint32_t pix1, uint32_t pix2, xreal luminanceWeight)
	{
		(void)luminanceWeight;
		return DistYCbCrBuffer::distMode1(pix1, pix2);
	}
	FORCE_INLINE static bool eq(uint32_t pix1, uint32_t pix2, const xbrz::ScalerCfg& cfg)
	{
		if (pix1 == pix2)
			return true;
		return static_cast<xreal>(DistYCbCrBuffer::distRawMode1(pix1, pix2)) < cfg.equalColorTolerance * 64.0f;
	}
	FORCE_INLINE static bool distLe(uint32_t a1, uint32_t a2, uint32_t b1, uint32_t b2, xreal lw)
	{
		(void)lw;
		return DistYCbCrBuffer::distRawMode1(a1, a2) <= DistYCbCrBuffer::distRawMode1(b1, b2);
	}
	FORCE_INLINE static void steepCheck(uint32_t f, uint32_t g, uint32_t h, uint32_t c,
	                                    const xbrz::ScalerCfg& cfg, bool& shallowCond, bool& steepCond)
	{
		const xreal fg = static_cast<xreal>(DistYCbCrBuffer::distRawMode1(f, g));
		const xreal hc = static_cast<xreal>(DistYCbCrBuffer::distRawMode1(h, c));
		shallowCond = cfg.steepDirectionThreshold * fg <= hc;
		steepCond   = cfg.steepDirectionThreshold * hc <= fg;
	}
};

template <>
struct ColorDistanceRGB<2>
{
	FORCE_INLINE static xreal dist(uint32_t pix1, uint32_t pix2, xreal luminanceWeight)
	{
		(void)luminanceWeight;
		return DistYCbCrBuffer::distMode2(pix1, pix2);
	}
	FORCE_INLINE static bool eq(uint32_t pix1, uint32_t pix2, const xbrz::ScalerCfg& cfg)
	{
		if (pix1 == pix2)
			return true;
		return DistYCbCrBuffer::distSqMode2(pix1, pix2) < cfg.equalColorTolerance * cfg.equalColorTolerance;
	}
	FORCE_INLINE static bool distLe(uint32_t a1, uint32_t a2, uint32_t b1, uint32_t b2, xreal lw)
	{
		(void)lw;
		return DistYCbCrBuffer::distSqMode2(a1, a2) <= DistYCbCrBuffer::distSqMode2(b1, b2);
	}
	FORCE_INLINE static void steepCheck(uint32_t f, uint32_t g, uint32_t h, uint32_t c,
	                                    const xbrz::ScalerCfg& cfg, bool& shallowCond, bool& steepCond)
	{
		const xreal fgSq = DistYCbCrBuffer::distSqMode2(f, g);
		const xreal hcSq = DistYCbCrBuffer::distSqMode2(h, c);
		const xreal sSq  = cfg.steepDirectionThreshold * cfg.steepDirectionThreshold;
		shallowCond = sSq * fgSq <= hcSq;
		steepCond   = sSq * hcSq <= fgSq;
	}
};

template <>
struct ColorDistanceRGB<-1>
{
	FORCE_INLINE static xreal dist(uint32_t pix1, uint32_t pix2, xreal luminanceWeight)
	{
		(void)luminanceWeight;
		return DistYCbCrBuffer::dist(pix1, pix2);
	}
	FORCE_INLINE static bool eq(uint32_t pix1, uint32_t pix2, const xbrz::ScalerCfg& cfg)
	{
		return dist(pix1, pix2, cfg.luminanceWeight) < cfg.equalColorTolerance;
	}
	FORCE_INLINE static bool distLe(uint32_t a1, uint32_t a2, uint32_t b1, uint32_t b2, xreal lw)
	{
		return dist(a1, a2, lw) <= dist(b1, b2, lw);
	}
	FORCE_INLINE static void steepCheck(uint32_t f, uint32_t g, uint32_t h, uint32_t c,
	                                    const xbrz::ScalerCfg& cfg, bool& shallowCond, bool& steepCond)
	{
		const xreal fg = dist(f, g, cfg.luminanceWeight);
		const xreal hc = dist(h, c, cfg.luminanceWeight);
		shallowCond = cfg.steepDirectionThreshold * fg <= hc;
		steepCond   = cfg.steepDirectionThreshold * hc <= fg;
	}
};

struct ColorDistanceARGB
{
	static xreal dist(uint32_t pix1, uint32_t pix2, xreal luminanceWeight)
	{
		(void)luminanceWeight;
		const xreal a1 = getAlpha(pix1) / 255.0f;
		const xreal a2 = getAlpha(pix2) / 255.0f;
		const xreal d = DistYCbCrBuffer::dist(pix1, pix2);
		if (a1 < a2)
			return a1 * d + 255 * (a2 - a1);
		else
			return a2 * d + 255 * (a1 - a2);
	}
	static bool eq(uint32_t pix1, uint32_t pix2, const xbrz::ScalerCfg& cfg)
	{
		return dist(pix1, pix2, cfg.luminanceWeight) < cfg.equalColorTolerance;
	}
	static bool distLe(uint32_t a1, uint32_t a2, uint32_t b1, uint32_t b2, xreal lw)
	{
		return dist(a1, a2, lw) <= dist(b1, b2, lw);
	}
	static void steepCheck(uint32_t f, uint32_t g, uint32_t h, uint32_t c,
	                       const xbrz::ScalerCfg& cfg, bool& shallowCond, bool& steepCond)
	{
		const xreal fg = dist(f, g, cfg.luminanceWeight);
		const xreal hc = dist(h, c, cfg.luminanceWeight);
		shallowCond = cfg.steepDirectionThreshold * fg <= hc;
		steepCond   = cfg.steepDirectionThreshold * hc <= fg;
	}
};


struct ColorGradientRGB
{
	template <unsigned int M, unsigned int N>
	static void alphaGrad(uint32_t& pixBack, uint32_t pixFront)
	{
		pixBack = gradientRGB<M, N>(pixFront, pixBack);
	}
};

struct ColorGradientARGB
{
	template <unsigned int M, unsigned int N>
	static void alphaGrad(uint32_t& pixBack, uint32_t pixFront)
	{
		pixBack = gradientARGB<M, N>(pixFront, pixBack);
	}
};

template <class Scaler>
static inline void scaleRGBByMode(int mode, const uint32_t* src, uint32_t* trg,
                                  int srcWidth, int srcHeight, const xbrz::ScalerCfg& cfg,
                                  int yFirst, int yLast,
                                  int xFirst = 0, int xLast = std::numeric_limits<int>::max(),
                                  uint32_t* trgAt = nullptr, int trgPitch = 0)
{
	switch (mode)
	{
		case 0:
			return scaleImage<Scaler, ColorDistanceRGB<0>>(src, trg, srcWidth, srcHeight, cfg, yFirst, yLast, xFirst, xLast, trgAt, trgPitch);
		case 1:
			return scaleImage<Scaler, ColorDistanceRGB<1>>(src, trg, srcWidth, srcHeight, cfg, yFirst, yLast, xFirst, xLast, trgAt, trgPitch);
		default:
			return scaleImage<Scaler, ColorDistanceRGB<2>>(src, trg, srcWidth, srcHeight, cfg, yFirst, yLast, xFirst, xLast, trgAt, trgPitch);
	}
}
}


void xbrz::scale(size_t factor, const uint32_t* src, uint32_t* trg, int srcWidth, int srcHeight, ColorFormat colFmt, const xbrz::ScalerCfg& cfg, int yFirst, int yLast)
{
	const int mode = DistYCbCrBuffer::resolveMode();
	switch (colFmt)
	{
		case ColorFormat::ARGB:
			switch (factor)
			{
				case 2:
					return scaleImage<Scaler2x<ColorGradientARGB>, ColorDistanceARGB>(src, trg, srcWidth, srcHeight, cfg, yFirst, yLast);
				case 3:
					return scaleImage<Scaler3x<ColorGradientARGB>, ColorDistanceARGB>(src, trg, srcWidth, srcHeight, cfg, yFirst, yLast);
				case 4:
					return scaleImage<Scaler4x<ColorGradientARGB>, ColorDistanceARGB>(src, trg, srcWidth, srcHeight, cfg, yFirst, yLast);
				case 5:
					return scaleImage<Scaler5x<ColorGradientARGB>, ColorDistanceARGB>(src, trg, srcWidth, srcHeight, cfg, yFirst, yLast);
				case 6:
					return scaleImage<Scaler6x<ColorGradientARGB>, ColorDistanceARGB>(src, trg, srcWidth, srcHeight, cfg, yFirst, yLast);
			}
			break;

		case ColorFormat::RGB:
			switch (factor)
			{
				case 2:
					return scaleRGBByMode<Scaler2x<ColorGradientRGB>>(mode, src, trg, srcWidth, srcHeight, cfg, yFirst, yLast);
				case 3:
					return scaleRGBByMode<Scaler3x<ColorGradientRGB>>(mode, src, trg, srcWidth, srcHeight, cfg, yFirst, yLast);
				case 4:
					return scaleRGBByMode<Scaler4x<ColorGradientRGB>>(mode, src, trg, srcWidth, srcHeight, cfg, yFirst, yLast);
				case 5:
					return scaleImage<Scaler5x<ColorGradientRGB>, ColorDistanceRGB<-1>>(src, trg, srcWidth, srcHeight, cfg, yFirst, yLast);
				case 6:
					return scaleImage<Scaler6x<ColorGradientRGB>, ColorDistanceRGB<-1>>(src, trg, srcWidth, srcHeight, cfg, yFirst, yLast);
			}
			break;
	}
	assert(false);
}


bool xbrz::equalColorTest(uint32_t col1, uint32_t col2, ColorFormat colFmt, xreal luminanceWeight, xreal equalColorTolerance)
{
	DistYCbCrBuffer::ensure(__atomic_load_n(&g_dist_mode, __ATOMIC_ACQUIRE));
	switch (colFmt)
	{
		case ColorFormat::ARGB:
			return ColorDistanceARGB::dist(col1, col2, luminanceWeight) < equalColorTolerance;

		case ColorFormat::RGB:
			return ColorDistanceRGB<-1>::dist(col1, col2, luminanceWeight) < equalColorTolerance;
	}
	assert(false);
	return false;
}


void xbrz::nearestNeighborScale(const uint32_t* src, int srcWidth, int srcHeight, int srcPitch,
								uint32_t* trg, int trgWidth, int trgHeight, int trgPitch,
								SliceType st, int yFirst, int yLast)
{
	if (srcPitch < srcWidth * static_cast<int>(sizeof(uint32_t))  ||
		trgPitch < trgWidth * static_cast<int>(sizeof(uint32_t)))
	{
		assert(false);
		return;
	}

	switch (st)
	{
		case NN_SCALE_SLICE_SOURCE:
			//nearest-neighbor (going over source image - fast for upscaling, since source is read only once
			yFirst = std::max(yFirst, 0);
			yLast  = std::min(yLast, srcHeight);
			if (yFirst >= yLast || trgWidth <= 0 || trgHeight <= 0) return;

			for (int y = yFirst; y < yLast; ++y)
			{
				//mathematically: ySrc = floor(srcHeight * yTrg / trgHeight)
				// => search for integers in: [ySrc, ySrc + 1) * trgHeight / srcHeight

				//keep within for loop to support MT input slices!
				const int yTrg_first = ( y	  * trgHeight + srcHeight - 1) / srcHeight; //=ceil(y * trgHeight / srcHeight)
				const int yTrg_last  = ((y + 1) * trgHeight + srcHeight - 1) / srcHeight; //=ceil(((y + 1) * trgHeight) / srcHeight)
				const int blockHeight = yTrg_last - yTrg_first;

				if (blockHeight > 0)
				{
					const uint32_t* srcLine = byteAdvance(src, y * srcPitch);
					uint32_t* trgLine  = byteAdvance(trg, yTrg_first * trgPitch);
					int xTrg_first = 0;

					for (int x = 0; x < srcWidth; ++x)
					{
						int xTrg_last = ((x + 1) * trgWidth + srcWidth - 1) / srcWidth;
						const int blockWidth = xTrg_last - xTrg_first;
						if (blockWidth > 0)
						{
							xTrg_first = xTrg_last;
							fillBlock(trgLine, trgPitch, srcLine[x], blockWidth, blockHeight);
							trgLine += blockWidth;
						}
					}
				}
			}
			break;

		case NN_SCALE_SLICE_TARGET:
			//nearest-neighbor (going over target image - slow for upscaling, since source is read multiple times missing out on cache! Fast for similar image sizes!)
			yFirst = std::max(yFirst, 0);
			yLast  = std::min(yLast, trgHeight);
			if (yFirst >= yLast || srcHeight <= 0 || srcWidth <= 0) return;

			for (int y = yFirst; y < yLast; ++y)
			{
				uint32_t* trgLine = byteAdvance(trg, y * trgPitch);
				const int ySrc = srcHeight * y / trgHeight;
				const uint32_t* srcLine = byteAdvance(src, ySrc * srcPitch);
				for (int x = 0; x < trgWidth; ++x)
				{
					const int xSrc = srcWidth * x / trgWidth;
					trgLine[x] = srcLine[xSrc];
				}
			}
			break;
	}
}

void xbrz::init()
{
	DistYCbCrBuffer::build();
}

/* GBAdhoc: rows [yFirst, yLast) and columns [xFirst, xLast) of an opaque
   (RGB) picture; the pixels written are exactly those scale() writes
   there. */
void xbrz::scaleRect(size_t factor, const uint32_t* src, uint32_t* trg, int srcWidth, int srcHeight,
                     int yFirst, int yLast, int xFirst, int xLast,
                     uint32_t* trgAt, int trgPitch)
{
	const xbrz::ScalerCfg cfg;
	const int mode = DistYCbCrBuffer::resolveMode();
	switch (factor)
	{
		case 2:
			return scaleRGBByMode<Scaler2x<ColorGradientRGB>>(mode, src, trg, srcWidth, srcHeight, cfg, yFirst, yLast, xFirst, xLast, trgAt, trgPitch);
		case 3:
			return scaleRGBByMode<Scaler3x<ColorGradientRGB>>(mode, src, trg, srcWidth, srcHeight, cfg, yFirst, yLast, xFirst, xLast, trgAt, trgPitch);
		case 4:
			return scaleRGBByMode<Scaler4x<ColorGradientRGB>>(mode, src, trg, srcWidth, srcHeight, cfg, yFirst, yLast, xFirst, xLast, trgAt, trgPitch);
	}
}

void xbrz::setDistMode(int mode)
{
	__atomic_store_n(&g_dist_mode, mode >= 0 && mode <= 2 ? mode : 0, __ATOMIC_RELEASE);
}
