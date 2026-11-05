/* fx_lcd.inc -- bagel-LCD-mini (see fx_common.inc), the body of main().
 *   U2  contrast | grid mode (0 auto, 1 240-class, 2 480-class) |
 *       grid (0 off, 1 RGB, 2 mono) | border fade on
 *   U3  border depth | background on | background mix | background tint
 *       (0 white, 1 warm, 2 gray, 3 green)
 *   U4  shading depth | shading distance (px at 640x480) | fake ghost on */
float rx = U0.x;
float ry = U0.y;
float2 pp = float2(vTexcoord.x * U5.x, vTexcoord.y * U5.y - U5.z);
float2 uv = float2(pp.x / rx, pp.y / ry);
float2 frag = float2(uv.x * U0.z, uv.y * U0.w);
bool coarse = U2.y > 1.5f || (U2.y <= 0.5f && ry > 300.0f);
float rmin = min(U0.z / rx, U0.w / ry);
float sample_scale = max(max(rmin, 1.0f) * U1.y, 1.0f);
float mscale = max(floor((coarse ? 2.0f * rmin : rmin) + 0.501f), 2.0f);
float3 col = fx_tap(Source, U0, U5, fx_sharp(pp, sample_scale));
float3 grid = float3(1.0f, 1.0f, 1.0f);
float gx = frag.x / mscale;
float gy = frag.y / mscale;
if (U2.z > 1.5f)
{
   /* mono (lcd1x): one factor, a dark gap between pixels */
   float yf = 16.0f / 17.0f - 1.0f / 17.0f * fx_sine(gy + 0.25f);
   float xf = 0.8f - 0.2f * fx_sine(gx + 0.25f);
   float m = yf * xf;
   grid = float3(m, m, m);
}
else if (U2.z > 0.5f)
{
   /* RGB (lcd3x): sub-pixel phases +90, -30 and -150 degrees */
   float yf = 16.0f / 17.0f + 1.0f / 17.0f * fx_sine(gy);
   float u = frac(gx) - 0.5f;
   float sx = -fx_sinh(u);
   float cx = -fx_sinh(0.25f - abs(u));
   grid = float3(yf * (4.0f + cx) / 5.0f,
                 yf * (4.0f + 0.8660254f * sx - 0.5f * cx) / 5.0f,
                 yf * (4.0f - 0.8660254f * sx - 0.5f * cx) / 5.0f);
}
if (U4.z >= 0.5f)
{
   /* fake ghosting: a spatial echo, not a previous frame */
   float ex = 5.0f / 640.0f * rx;
   col = col * (0.75f * 0.875f) +
         fx_tap(Source, U0, U5, float2(pp.x - ex, pp.y)) * (0.25f * 0.875f) +
         fx_tap(Source, U0, U5, float2(pp.x - 2.0f * ex, pp.y)) * 0.125f;
}
col = fx_saturate(col, U1.z);
float k = U1.w * U2.x;
if (U2.w >= 0.5f)
{
   /* rounded fade at the edges */
   float hx = abs(uv.x - 0.5f);
   float hy = abs(uv.y - 0.5f);
   if (max(hx, hy) >= 0.475f)
   {
      float qx = max(hx - 0.475f, 0.0f);
      float qy = max(hy - 0.475f, 0.0f);
      float c = clamp((0.025f - sqrt(qx * qx + qy * qy)) * 60.0f, 0.0f, 1.0f);
      k = k * lerp1(1.0f - U3.x, 1.0f, c);
   }
}
float off = 0.5f - 0.5f * U2.x;
col = float3(col.x * grid.x * k + off, col.y * grid.y * k + off,
             col.z * grid.z * k + off);
if (U3.y >= 0.5f)
{
   /* LCD background: clear pixels let a paper backing show through, dark
    * ones leave a shade on it */
   float t = U3.w;
   float3 paper = float3(
      0.86f + t * (-0.35f / 3.0f + t * (-0.04f + t * 0.1f / 6.0f)),
      0.86f + t * (-0.74f / 3.0f + t * (0.055f + t * 0.01f / 6.0f)),
      0.86f + t * (-1.8f / 3.0f + t * (0.385f + t * -0.45f / 6.0f)));
   float grain = (fx_hash(float2(floor(frag.x * 0.5f), floor(frag.y * 0.5f))) -
                  0.5f) * 0.04f;
   float3 src = fx_tap(Source, U0, U5,
                       float2(pp.x - U4.y / 640.0f * rx,
                              pp.y - U4.y / 480.0f * ry));
   float dark = clamp(1.0f - fx_luma(src), 0.0f, 1.0f);
   float sh = 1.0f - 0.65f * dark * U4.x;
   paper = float3((paper.x + grain) * sh, (paper.y + grain) * sh,
                  (paper.z + grain) * sh);
   col = lerp(col, paper, U3.z * smoothstep(0.25f, 0.85f, fx_luma(col)));
}
return float4(col.x, col.y, col.z, 1.0f);
