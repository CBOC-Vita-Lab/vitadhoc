/* fx_common.inc -- the LCD and CRT picture shaders (video_vita.c, fx_draw): what
 * both share.  Same dialect as xbrz_common.inc (valid Cg and valid C++), plus
 * min / max / clamp / abs on floats, dot / lerp on float3, V_SAMP for a
 * sampler argument, and a LINEAR sampled source.
 *
 * The two shaders are a port of bagel-mini-shaders (bgelmini, v3.6;
 * https://github.com/bgelmini/bagel-mini-shaders), GPL-3.0-or-later:
 *   bagel-LCD-mini  after lcd3x (Gigaherz, BSD-3-Clause), lcd1x (Gigaherz,
 *                   jdgleaver, GPL-2.0-or-later), Sharp-Shimmerless-Shader
 *                   (zadpos, public domain), Pixel Transparency (mattakins)
 *   bagel-CRT-mini  after fake-CRT-Geom and kaizer-lp-small (DariusG,
 *                   GPL-2.0-or-later), tiny_ntsc (Vsevolod, MIT)
 * Changes: GLSL -> this dialect; the grid is locked to the picture's own
 * pixels (no gl_FragCoord); the vertical roll and the warp of the picture
 * are left out (the Geom border keeps only the rounded corner shape).
 * See notices/THIRD-PARTY.md.
 *
 * Uniforms (U0..U5, set by video_vita.c):
 *   U0  picture width, height (texels) | output width, height (pixels)
 *   U1  time (frames) | sharpness | saturation | brightness
 *   U5  source texture width, height | first picture row in it
 * and per shader U2..U4 (fx_lcd.inc, fx_crt.inc).
 */

float fx_hash(float2 co)
{
   float2 p = frac(co * float2(443.897f, 441.423f));
   float d = dot(p, float2(p.y + 19.19f, p.x + 19.19f));
   float px = p.x + d;
   float py = p.y + d;
   return frac((px + py) * px);
}

/* sin(2 pi q): a parabola and one refinement step */
float fx_sine(float q)
{
   float u = frac(q) - 0.5f;
   float y = u * (16.0f * abs(u) - 8.0f);
   return y * (0.775f + 0.225f * abs(y));
}

/* sin(2 pi u) for u in -0.5..0.5 */
float fx_sinh(float u)
{
   float y = u * (8.0f - 16.0f * abs(u));
   return y * (0.775f + 0.225f * abs(y));
}

float lerp1(float a, float b, float t)
{
   return a + (b - a) * t;
}

float fx_luma(float3 c)
{
   return c.x * 0.299f + c.y * 0.587f + c.z * 0.114f;
}

float3 fx_clamp01(float3 c)
{
   return float3(clamp(c.x, 0.0f, 1.0f), clamp(c.y, 0.0f, 1.0f),
                 clamp(c.z, 0.0f, 1.0f));
}

float3 fx_saturate(float3 c, float s)
{
   float l = fx_luma(c);
   return float3(l + s * (c.x - l), l + s * (c.y - l), l + s * (c.z - l));
}

/* The picture at `pos` (texels of the picture, 0..w, 0..h), edges clamped
 * to the picture's own rows; the source is sampled LINEAR. */
float3 fx_tap(V_SAMP S, float4 U0, float4 U5, float2 pos)
{
   float x = clamp(pos.x, 0.5f, U0.x - 0.5f);
   float y = clamp(pos.y, 0.5f, U0.y - 0.5f);
   float4 c = tex2D(S, float2(x / U5.x, (y + U5.z) / U5.y));
   return V_RGB(c);
}

/* Sharp bilinear: crisp texel centres, a blend only across the edges, and
 * the blend narrows as the scale grows (Sharp-Shimmerless). */
float2 fx_sharp(float2 p, float ss)
{
   float ex = floor(p.x + 0.5f);
   float ey = floor(p.y + 0.5f);
   return float2(ex + 0.5f * clamp((p.x - ex) * (2.0f * ss), -1.0f, 1.0f),
                 ey + 0.5f * clamp((p.y - ey) * (2.0f * ss), -1.0f, 1.0f));
}

/* Two faint echoes trailing to the right, the second twice as far and half
 * as strong; with `h` > 0 each is the two-tap blur of the CRT's dither merge. */
float3 fx_echoes(V_SAMP S, float4 U0, float4 U5, float3 col, float2 pos,
                 float h, float length_px, float strength)
{
   float ex = length_px / 640.0f * U0.x;
   float a = 0.5f * strength;
   float b = 0.25f * strength;
   float k1 = a - a * b;
   float k0 = 1.0f - k1 - b;
   if (h <= 0.0f)
      return col * k0 + fx_tap(S, U0, U5, float2(pos.x - ex, pos.y)) * k1 +
             fx_tap(S, U0, U5, float2(pos.x - 2.0f * ex, pos.y)) * b;
   k1 = k1 * 0.5f;
   b = b * 0.5f;
   return col * k0 +
          (fx_tap(S, U0, U5, float2(pos.x - ex - h, pos.y)) +
           fx_tap(S, U0, U5, float2(pos.x - ex + h, pos.y))) * k1 +
          (fx_tap(S, U0, U5, float2(pos.x - 2.0f * ex - h, pos.y)) +
           fx_tap(S, U0, U5, float2(pos.x - 2.0f * ex + h, pos.y))) * b;
}
