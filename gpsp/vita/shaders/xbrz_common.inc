/* xbrz_common.inc -- GPU xBRZ (video_vita.c, gx_draw), the parts the passes share.
 *
 * Written once in a small dialect that is valid Cg (the Vita compiles it at
 * run time: vback_vita2d.c, g_prelude_cg) and valid C++ (the host twin
 * runs it per pixel: vback_soft_prog.cpp, the same text).  The dialect:
 * float2/3/4 and their constructors; .x .y .z .w; V_XY / V_ZW / V_RGB for
 * swizzles; tex2D (point sampling, clamped); frac, lerp, floor, step,
 * smoothstep, sign, length, dot, sqrt; V_MOD (fmod); V_EQ3 (all equal);
 * float literals with an f.
 *
 * The algorithm is libretro's "xbrz-freescale-multipass" (slang-shaders,
 * edge-smoothing/xbrz), itself Hyllian's xBR texel mapping (MIT) with the
 * decisions of Zenju's xBRZ (GPL-3, as third_party/xbrz is), in three
 * passes here: pass 0 decides each point where four source pixels meet
 * (which of them it cuts); pass 1 gathers, per source pixel, its four
 * corners and whether each blends as a line; pass 2 draws every output
 * pixel at the final size from those.
 */
#define XBRZ_BLEND_NONE      0.0f
#define XBRZ_BLEND_NORMAL    1.0f
#define XBRZ_BLEND_DOMINANT  2.0f
#define XBRZ_EQUAL_TOL       (30.0f / 255.0f)
#define XBRZ_STEEP           2.2f
#define XBRZ_DOMINANT        3.6f

float xbrz_dist(float3 a, float3 b)
{
   const float wr = 0.2627f, wg = 0.6780f, wb = 0.0593f;
   const float scale_b = 0.5f / (1.0f - wb);
   const float scale_r = 0.5f / (1.0f - wr);
   float3 d = a - b;
   float y = d.x * wr + d.y * wg + d.z * wb;
   float cb = scale_b * (d.z - y);
   float cr = scale_r * (d.x - y);
   return sqrt(y * y + cb * cb + cr * cr);
}

bool xbrz_same(float3 a, float3 b)
{
   return xbrz_dist(a, b) < XBRZ_EQUAL_TOL;
}

float xbrz_left_ratio(float2 center, float2 origin, float2 direction,
                      float2 scale)
{
   float2 p0 = center - origin;
   float2 proj = direction * (dot(p0, direction) / dot(direction, direction));
   float2 distv = p0 - proj;
   float2 orth = float2(-direction.y, direction.x);
   float side = sign(dot(p0, orth));
   float v = side * length(distv * scale);
   return smoothstep(-0.70710678f, 0.70710678f, v);
}
