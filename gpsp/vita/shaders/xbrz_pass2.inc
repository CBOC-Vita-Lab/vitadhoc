/* xbrz_pass2.inc -- GPU xBRZ pass 2: the BODY of the fragment program
 * (xbrz_common.inc has the dialect).  Drawn at the picture's final size.
 * In scope:
 *   float2 vTexcoord      this pixel's place in Ref, 0..1
 *   sampler2D Ref         the game's picture, point sampled (unit 0)
 *   sampler2D Info        pass 1's corners, point sampled (unit 1)
 *   float4 RefSize        Ref's width, height, 1/width, 1/height
 *   float4 InfoScale      xy: Ref's size over Info's (Info is a render
 *                         target, its width a multiple of 8)
 *   float4 OutScale       xy: output pixels per source pixel; z > 0.5:
 *                         show pass 1's bytes instead (a diagnostic)
 * Source pixels around E:  -|B|-
 *                          D|E|F
 *                          -|H|-                                        */
   float2 scale = V_XY(OutScale);
   float2 pos = frac(vTexcoord * V_XY(RefSize)) - float2(0.5f, 0.5f);
   float2 coord = vTexcoord - pos * V_ZW(RefSize);
#define XP(x, y) V_RGB(tex2D(Ref, coord + V_ZW(RefSize) * float2(x, y)))
   float3 E = XP( 0.0f,  0.0f);
   float4 info = floor(tex2D(Info, coord * V_XY(InfoScale)) * 255.0f + 0.5f);
   /* (vita.ini gpu_xbrz_view: pass 1's bytes, made visible) */
   if (OutScale.z > 0.5f)
      return float4(info.x * (1.0f / 86.0f), info.y * (1.0f / 86.0f),
                    (info.z + info.w) * (1.0f / 172.0f), 1.0f);
   /* Most of a picture blends nothing: the pixel is E, two reads. */
   if (info.x + info.y + info.z + info.w == 0.0f)
      return float4(E.x, E.y, E.z, 1.0f);
   float3 B = XP( 0.0f, -1.0f);
   float3 D = XP(-1.0f,  0.0f);
   float3 F = XP( 1.0f,  0.0f);
   float3 H = XP( 0.0f,  1.0f);
#undef XP
   float4 blend = floor(V_MOD(info, 4.0f));
   float4 lineb = floor(V_MOD(info * 0.25f, 4.0f));
   float4 shallow = floor(V_MOD(info * (1.0f / 16.0f), 4.0f));
   float4 steep = floor(V_MOD(info * (1.0f / 64.0f), 4.0f));
   float3 res = E;

   /* corner z: E F / H */
   if (blend.z > XBRZ_BLEND_NONE)
   {
      float2 origin = float2(0.0f, 0.70710678f);
      float2 dir = float2(1.0f, -1.0f);
      float3 pix;
      if (lineb.z > 0.0f)
      {
         origin = shallow.z > 0.0f ? float2(0.0f, 0.25f) : float2(0.0f, 0.5f);
         dir.x += shallow.z;
         dir.y -= steep.z;
      }
      pix = lerp(H, F, step(xbrz_dist(E, F), xbrz_dist(E, H)));
      res = lerp(res, pix, xbrz_left_ratio(pos, origin, dir, scale));
   }
   /* corner w: D E / H */
   if (blend.w > XBRZ_BLEND_NONE)
   {
      float2 origin = float2(-0.70710678f, 0.0f);
      float2 dir = float2(1.0f, 1.0f);
      float3 pix;
      if (lineb.w > 0.0f)
      {
         origin = shallow.w > 0.0f ? float2(-0.25f, 0.0f) : float2(-0.5f, 0.0f);
         dir.y += shallow.w;
         dir.x += steep.w;
      }
      pix = lerp(H, D, step(xbrz_dist(E, D), xbrz_dist(E, H)));
      res = lerp(res, pix, xbrz_left_ratio(pos, origin, dir, scale));
   }
   /* corner y: B / E F */
   if (blend.y > XBRZ_BLEND_NONE)
   {
      float2 origin = float2(0.70710678f, 0.0f);
      float2 dir = float2(-1.0f, -1.0f);
      float3 pix;
      if (lineb.y > 0.0f)
      {
         origin = shallow.y > 0.0f ? float2(0.25f, 0.0f) : float2(0.5f, 0.0f);
         dir.y -= shallow.y;
         dir.x -= steep.y;
      }
      pix = lerp(F, B, step(xbrz_dist(E, B), xbrz_dist(E, F)));
      res = lerp(res, pix, xbrz_left_ratio(pos, origin, dir, scale));
   }
   /* corner x: B / D E */
   if (blend.x > XBRZ_BLEND_NONE)
   {
      float2 origin = float2(0.0f, -0.70710678f);
      float2 dir = float2(-1.0f, 1.0f);
      float3 pix;
      if (lineb.x > 0.0f)
      {
         origin = shallow.x > 0.0f ? float2(0.0f, -0.25f) : float2(0.0f, -0.5f);
         dir.x -= shallow.x;
         dir.y += steep.x;
      }
      pix = lerp(D, B, step(xbrz_dist(E, B), xbrz_dist(E, D)));
      res = lerp(res, pix, xbrz_left_ratio(pos, origin, dir, scale));
   }
   return float4(res.x, res.y, res.z, 1.0f);
