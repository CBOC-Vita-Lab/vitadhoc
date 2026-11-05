/* fx_crt.inc -- bagel-CRT-mini (see fx_common.inc), the body of main().
 *   U2  contrast | scan grid (0 auto, 1 240-class, 2 480-class) |
 *       scanline dark | scanline bright
 *   U3  mask type (0 aperture, 1 fine, 2 locked to the picture's pixels) |
 *       mask strength | vignette | border (0 geom, 1 corner, 2 flat)
 *   U4  dither merge (0 off, 1 on, 2 dot crawl) | RF ghost (0 off, 1 static,
 *       2 motion) | noise on
 * No vertical roll, and the picture is never warped: the Geom border only
 * borrows the curved outline for its rounded corners. */
float rx = U0.x;
float ry = U0.y;
float2 pp = float2(vTexcoord.x * U5.x, vTexcoord.y * U5.y - U5.z);
float2 uv = float2(pp.x / rx, pp.y / ry);
bool coarse = U2.y > 1.5f || (U2.y <= 0.5f && ry > 300.0f);
float rmin = min(U0.z / rx, U0.w / ry);
float sample_scale = max(rmin * U1.y, 1.0f);
float grid_mul = coarse ? 2.0f : 1.0f;
float mscale = max(floor(rmin * grid_mul + 0.501f), 1.0f);
float row_f = 1.0f;
if (mscale * grid_mul >= 2.5f)
{
   /* rows follow a sine; the 2x coarse grid alternates */
   float ra = mscale < 2.5f ? 0.9219821f : 0.9428090f;
   float rb = mscale < 2.5f ? 0.3872325f : 0.4714045f;
   float ph = uv.y * U0.w / mscale -
              (coarse ? 0.25f * mscale - 0.5f * floor(0.5f * mscale) : 0.25f);
   row_f = ra + rb * fx_sine(ph);
}
float2 sp = fx_sharp(pp, sample_scale);
float3 col;
float half_px = 0.0f;
if (U4.x < 0.5f)
   col = fx_tap(Source, U0, U5, sp);
else
{
   /* horizontal blur that merges fine dithering; dot crawl leans the
    * weights by row group on a slow cycle */
   float crawl = U4.x > 1.5f ? 0.05f * fx_sine(U1.x * 0.031830989f) : 0.0f;
   if (coarse)
   {
      float lean = frac(pp.y * 0.125f) < 0.5f ? crawl : -crawl;
      float3 c1 = fx_tap(Source, U0, U5, sp);
      float3 c0 = fx_tap(Source, U0, U5, float2(sp.x - 2.0f, sp.y));
      float3 c2 = fx_tap(Source, U0, U5, float2(sp.x + 2.0f, sp.y));
      col = c1 + (c0 * (0.25f + lean) + c2 * (0.25f - lean) - c1 * 0.5f);
      half_px = 1.0f;
   }
   else
   {
      float lean = frac(pp.y * 0.25f) < 0.5f ? crawl : -crawl;
      float shift = 2.0f * lean;
      col = (fx_tap(Source, U0, U5, float2(sp.x - 0.5f - shift, sp.y)) +
             fx_tap(Source, U0, U5, float2(sp.x + 0.5f - shift, sp.y))) * 0.5f;
      half_px = 0.5f;
   }
}
float noise_add = 0.0f;
if (U4.y >= 0.5f)
{
   float gl = 5.0f;
   float gs = 0.5f;
   if (U4.y > 1.5f)
   {
      /* motion: now and then the echo drifts closer or further and back */
      float cycle_id = floor(U1.x / 2400.0f);
      float t = U1.x - cycle_id * 2400.0f;
      float slot_len = (2400.0f - 270.0f) / 2.0f;
      float slot = min(floor(t / slot_len), 1.0f);
      float drift_id = cycle_id * 2.0f + slot;
      float start = fx_hash(float2(drift_id, 7.7f)) * (slot_len - 600.0f);
      float p = clamp((t - slot * slot_len - start) / 600.0f, 0.0f, 1.0f);
      float hh = 4.0f * p * (1.0f - p);
      float s = hh * (0.775f + 0.225f * hh);
      float bump = s * s;
      float r = fx_hash(float2(drift_id, 9.1f));
      float drift = r < 0.25f ? lerp1(-3.5f, -3.0f, r / 0.25f)
                              : lerp1(6.0f, 10.0f, (r - 0.25f) / 0.75f);
      gl = 5.0f + drift * bump;
      gs = 0.5f * (1.0f + 0.3f * bump);
   }
   col = fx_echoes(Source, U0, U5, col, pp, half_px, gl, gs);
}
if (U4.z >= 0.5f)
{
   float ts = frac(fmod(U1.x, 1009.0f) * 0.618034f) * 1000.0f;
   noise_add = (fx_hash(float2(uv.x * U0.z + ts, uv.y * U0.w + ts)) - 0.5f) * 0.08f;
}
col = fx_saturate(col, U1.z);
/* scanline strength follows the picture's brightness, the vignette darkens
 * towards the sides */
float w = 0.3036f * (col.x + col.y + col.z);
float vig = uv.x - 0.5f;
float row_mul = max((1.0f - U2.z) - (U2.w - U2.z) * w - U3.z * vig * vig, 0.0f) * row_f;
float mq = uv.x * U0.z * ((U3.x > 0.5f ? 0.6666f : 1.0f) * 0.5f);
if (U3.x > 1.5f)
   mq = uv.x * U0.z / mscale;
float mask = (1.0f - U3.y) + U3.y * fx_sine(mq);
float kk = row_mul * mask * (U1.w * U2.x);
float off = 0.5f - 0.5f * U2.x;
col = fx_clamp01(float3(col.x * kk + off + noise_add, col.y * kk + off + noise_add,
                        col.z * kk + off + noise_add));
/* border */
float bm = 1.0f;
{
   float hx = abs(uv.x - 0.5f);
   float hy = abs(uv.y - 0.5f);
   if (max(hx, hy) >= 0.43f)
   {
      float radius = 0.04f;
      float trans = 90.0f;
      if (U3.w < 0.5f)
      {
         float ox = hx * (1.0f + 4.0f * hy * hy * 0.031f);
         float oy = hy * (1.0f + 4.0f * hx * hx * 0.041f);
         hx = ox;
         hy = oy;
      }
      else if (U3.w < 1.5f)
      {
         radius = 0.055f;
         trans = 60.0f;
      }
      else
      {
         radius = 0.025f;
         trans = 60.0f;
      }
      float qx = max(hx - (0.5f - radius), 0.0f);
      float qy = max(hy - (0.5f - radius), 0.0f);
      float m = clamp((radius - sqrt(qx * qx + qy * qy)) * trans, 0.0f, 1.0f);
      bm = U3.w > 1.5f ? lerp1(0.75f, 1.0f, m) : m;
   }
}
return float4(col.x * bm, col.y * bm, col.z * bm, 1.0f);
