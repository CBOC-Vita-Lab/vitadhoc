/* vback_soft_prog.cpp -- the twin's GPU programs (vback.h vb_prog): the
 * shader text of gpsp/vita/shaders/ compiled as C++ and run once per
 * output pixel by vback_soft.c's vb_pquads.
 *
 * The shaders are written in a dialect that is both Cg and C++
 * (shaders/xbrz_common.inc says which parts); this file is the C++ half of
 * it: the vector types, the few intrinsics, and a point sampler with
 * clamped edges over the soft backend's 0xAABBGGRR texels.  The console
 * compiles the very same text (vback_vita2d.c), so what the twin's
 * pictures show is what the GPU is asked to draw -- up to float rounding.
 */
#include <math.h>
#include <stdint.h>

namespace vbsp {

struct float2
{
   float x, y;
   float2() : x(0), y(0) {}
   float2(float a, float b) : x(a), y(b) {}
};
struct float3
{
   float x, y, z;
   float3() : x(0), y(0), z(0) {}
   float3(float a, float b, float c) : x(a), y(b), z(c) {}
};
struct float4
{
   float x, y, z, w;
   float4() : x(0), y(0), z(0), w(0) {}
   float4(float a, float b, float c, float d) : x(a), y(b), z(c), w(d) {}
};

static inline float2 operator+(float2 a, float2 b) { return float2(a.x + b.x, a.y + b.y); }
static inline float2 operator-(float2 a, float2 b) { return float2(a.x - b.x, a.y - b.y); }
static inline float2 operator*(float2 a, float2 b) { return float2(a.x * b.x, a.y * b.y); }
static inline float2 operator*(float2 a, float s) { return float2(a.x * s, a.y * s); }
static inline float2 operator*(float s, float2 a) { return float2(a.x * s, a.y * s); }
static inline float4 operator*(float4 a, float4 b) { return float4(a.x * b.x, a.y * b.y, a.z * b.z, a.w * b.w); }
static inline float3 operator+(float3 a, float3 b) { return float3(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline float3 operator-(float3 a, float3 b) { return float3(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline float3 operator*(float3 a, float s) { return float3(a.x * s, a.y * s, a.z * s); }
static inline float4 operator+(float4 a, float s) { return float4(a.x + s, a.y + s, a.z + s, a.w + s); }
static inline float4 operator*(float4 a, float s) { return float4(a.x * s, a.y * s, a.z * s, a.w * s); }

static inline float dot(float2 a, float2 b) { return a.x * b.x + a.y * b.y; }
static inline float length(float2 a) { return sqrtf(dot(a, a)); }
static inline float sign(float v) { return v > 0.0f ? 1.0f : v < 0.0f ? -1.0f : 0.0f; }
static inline float step(float edge, float x) { return x < edge ? 0.0f : 1.0f; }
static inline float smoothstep(float a, float b, float x)
{
   float t = (x - a) / (b - a);
   t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
   return t * t * (3.0f - 2.0f * t);
}
static inline float2 frac(float2 v) { return float2(v.x - floorf(v.x), v.y - floorf(v.y)); }
static inline float floor(float v) { return floorf(v); }
static inline float4 floor(float4 v) { return float4(floorf(v.x), floorf(v.y), floorf(v.z), floorf(v.w)); }
static inline float3 lerp(float3 a, float3 b, float t)
{
   return float3(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t,
                 a.z + (b.z - a.z) * t);
}
static inline float4 vmod(float4 v, float m)
{
   return float4(fmodf(v.x, m), fmodf(v.y, m), fmodf(v.z, m), fmodf(v.w, m));
}
static inline bool eq3(float3 a, float3 b) { return a.x == b.x && a.y == b.y && a.z == b.z; }

/* A texture as the program sees it: point sampled, edges clamped. */
struct sampler2D
{
   const uint32_t *px;
   int w, h;
};
static inline float4 tex2D(const sampler2D &s, float2 uv)
{
   int x = (int)floorf(uv.x * (float)s.w), y = (int)floorf(uv.y * (float)s.h);
   uint32_t c;
   x = x < 0 ? 0 : x >= s.w ? s.w - 1 : x;
   y = y < 0 ? 0 : y >= s.h ? s.h - 1 : y;
   c = s.px[(size_t)y * s.w + x];
   return float4((float)(c & 0xFF) / 255.0f, (float)((c >> 8) & 0xFF) / 255.0f,
                 (float)((c >> 16) & 0xFF) / 255.0f, (float)(c >> 24) / 255.0f);
}

#define V_XY(v)    float2((v).x, (v).y)
#define V_ZW(v)    float2((v).z, (v).w)
#define V_RGB(v)   float3((v).x, (v).y, (v).z)
#define V_MOD(a, b) vmod(a, b)
#define V_EQ3(a, b) eq3(a, b)

#include "shaders/xbrz_common.inc"

static float4 xbrz_pass0(float2 vTexcoord, const sampler2D &Source,
                         float4 SourceSize)
{
#include "shaders/xbrz_pass0.inc"
}

static float4 xbrz_pass1(float2 vTexcoord, const sampler2D &Source,
                         const sampler2D &Corner, float4 SourceSize,
                         float4 CornerInv)
{
#include "shaders/xbrz_pass1.inc"
}

static float4 xbrz_pass2(float2 vTexcoord, const sampler2D &Ref,
                         const sampler2D &Info, float4 RefSize,
                         float4 InfoScale, float4 OutScale)
{
#include "shaders/xbrz_pass2.inc"
}

static uint32_t pack(float4 c)
{
   float ch[4] = { c.x, c.y, c.z, c.w };
   uint32_t out = 0;
   int k;
   for (k = 0; k < 4; k++)
   {
      float v = ch[k] < 0.0f ? 0.0f : ch[k] > 1.0f ? 1.0f : ch[k];
      out |= (uint32_t)(v * 255.0f + 0.5f) << (8 * k);
   }
   return out;
}

} /* namespace vbsp */

/* One output pixel of program `which` (vback.h VB_PROG_*) at u, v (0..1
 * over t0), as 0xAABBGGRR. */
extern "C" uint32_t vbsp_run(int which, const uint32_t *t0, int w0, int h0,
                             const uint32_t *t1, int w1, int h1, float u,
                             float v, const float uni[4][4])
{
   using namespace vbsp;
   sampler2D s0 = { t0, w0, h0 }, s1 = { t1 ? t1 : t0, t1 ? w1 : w0,
                                        t1 ? h1 : h0 };
   float4 u0(uni[0][0], uni[0][1], uni[0][2], uni[0][3]);
   float4 u1(uni[1][0], uni[1][1], uni[1][2], uni[1][3]);
   float4 u2(uni[2][0], uni[2][1], uni[2][2], uni[2][3]);
   if (which == 0)
      return pack(xbrz_pass0(float2(u, v), s0, u0));
   if (which == 1)
      return pack(xbrz_pass1(float2(u, v), s0, s1, u0, u1));
   return pack(xbrz_pass2(float2(u, v), s0, s1, u0, u1, u2));
}
