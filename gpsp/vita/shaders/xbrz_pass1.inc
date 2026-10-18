/* xbrz_pass1.inc -- GPU xBRZ pass 1: the BODY of the fragment program
 * (xbrz_common.inc has the dialect).  Drawn 1:1 into a render target the
 * size of the picture.  In scope:
 *   float2 vTexcoord      this pixel's place in Source, 0..1
 *   sampler2D Source      the game's picture, point sampled (unit 0)
 *   sampler2D Corner      pass 0's target, point sampled (unit 1)
 *   float4 SourceSize     Source's width, height, 1/width, 1/height
 *   float4 CornerInv      xy: 1 / pass 0's target's width, height; z > 0.5:
 *                         pass on pass 0's bytes instead (a diagnostic)
 * Returns E's four corners packed as bytes (x y / w z): bits 0-1 the blend
 * (none, normal, dominant) from pass 0, bit 2 "blend as a line", bit 4 a
 * shallow line, bit 6 a steep one -- over 255.
 *
 * E's corners are the four points around it: its own bottom-right one
 * (pass 0's texel for E, its x), D's (E is that corner's F: y), B's (E is
 * its H: z) and A's (E is its I: w).
 *
 * Source pixels around E:  A|B|C
 *                          D|E|F
 *                          G|H|I                                        */
   float2 pos = frac(vTexcoord * V_XY(SourceSize)) - float2(0.5f, 0.5f);
   float2 coord = vTexcoord - pos * V_ZW(SourceSize);
   /* pass 0's texel for E: one pixel in from its target's top left */
   float2 ct = (coord * V_XY(SourceSize) + float2(1.0f, 1.0f)) *
               V_XY(CornerInv);
   float4 res;
   float4 outv;
   res.z = floor(tex2D(Corner, ct).x * 255.0f + 0.5f);
   res.w = floor(tex2D(Corner, ct - float2(CornerInv.x, 0.0f)).y * 255.0f + 0.5f);
   res.y = floor(tex2D(Corner, ct - float2(0.0f, CornerInv.y)).z * 255.0f + 0.5f);
   res.x = floor(tex2D(Corner, ct - V_XY(CornerInv)).w * 255.0f + 0.5f);
   outv = res;
   /* (vita.ini gpu_xbrz_view = 2: pass 0's bytes passed on, 40 times) */
   if (CornerInv.z > 0.5f)
      return tex2D(Corner, ct) * 40.0f;
   if (res.x + res.y + res.z + res.w == 0.0f)
      return outv;
#define XP(x, y) V_RGB(tex2D(Source, coord + V_ZW(SourceSize) * float2(x, y)))
   float3 A = XP(-1.0f, -1.0f);
   float3 B = XP( 0.0f, -1.0f);
   float3 C = XP( 1.0f, -1.0f);
   float3 D = XP(-1.0f,  0.0f);
   float3 E = XP( 0.0f,  0.0f);
   float3 F = XP( 1.0f,  0.0f);
   float3 G = XP(-1.0f,  1.0f);
   float3 H = XP( 0.0f,  1.0f);
   float3 I = XP( 1.0f,  1.0f);
#undef XP


   /* which corners blend as lines, and of what slope */
   if (res.z == XBRZ_BLEND_DOMINANT || (res.z == XBRZ_BLEND_NORMAL &&
       !((res.y != XBRZ_BLEND_NONE && !xbrz_same(E, G)) ||
         (res.w != XBRZ_BLEND_NONE && !xbrz_same(E, C)) ||
         (xbrz_same(G, H) && xbrz_same(H, I) && xbrz_same(I, F) &&
          xbrz_same(F, C) && !xbrz_same(E, I)))))
   {
      float d_fg = xbrz_dist(F, G), d_hc = xbrz_dist(H, C);
      outv.z += 4.0f;
      if ((XBRZ_STEEP * d_fg <= d_hc) && !V_EQ3(E, G) && !V_EQ3(D, G))
         outv.z += 16.0f;
      if ((XBRZ_STEEP * d_hc <= d_fg) && !V_EQ3(E, C) && !V_EQ3(B, C))
         outv.z += 64.0f;
   }
   if (res.w == XBRZ_BLEND_DOMINANT || (res.w == XBRZ_BLEND_NORMAL &&
       !((res.z != XBRZ_BLEND_NONE && !xbrz_same(E, A)) ||
         (res.x != XBRZ_BLEND_NONE && !xbrz_same(E, I)) ||
         (xbrz_same(A, D) && xbrz_same(D, G) && xbrz_same(G, H) &&
          xbrz_same(H, I) && !xbrz_same(E, G)))))
   {
      float d_ha = xbrz_dist(H, A), d_di = xbrz_dist(D, I);
      outv.w += 4.0f;
      if ((XBRZ_STEEP * d_ha <= d_di) && !V_EQ3(E, A) && !V_EQ3(B, A))
         outv.w += 16.0f;
      if ((XBRZ_STEEP * d_di <= d_ha) && !V_EQ3(E, I) && !V_EQ3(F, I))
         outv.w += 64.0f;
   }
   if (res.y == XBRZ_BLEND_DOMINANT || (res.y == XBRZ_BLEND_NORMAL &&
       !((res.x != XBRZ_BLEND_NONE && !xbrz_same(E, I)) ||
         (res.z != XBRZ_BLEND_NONE && !xbrz_same(E, A)) ||
         (xbrz_same(I, F) && xbrz_same(F, C) && xbrz_same(C, B) &&
          xbrz_same(B, A) && !xbrz_same(E, C)))))
   {
      float d_bi = xbrz_dist(B, I), d_fa = xbrz_dist(F, A);
      outv.y += 4.0f;
      if ((XBRZ_STEEP * d_bi <= d_fa) && !V_EQ3(E, I) && !V_EQ3(H, I))
         outv.y += 16.0f;
      if ((XBRZ_STEEP * d_fa <= d_bi) && !V_EQ3(E, A) && !V_EQ3(D, A))
         outv.y += 64.0f;
   }
   if (res.x == XBRZ_BLEND_DOMINANT || (res.x == XBRZ_BLEND_NORMAL &&
       !((res.w != XBRZ_BLEND_NONE && !xbrz_same(E, C)) ||
         (res.y != XBRZ_BLEND_NONE && !xbrz_same(E, G)) ||
         (xbrz_same(C, B) && xbrz_same(B, A) && xbrz_same(A, D) &&
          xbrz_same(D, G) && !xbrz_same(E, A)))))
   {
      float d_dc = xbrz_dist(D, C), d_bg = xbrz_dist(B, G);
      outv.x += 4.0f;
      if ((XBRZ_STEEP * d_dc <= d_bg) && !V_EQ3(E, C) && !V_EQ3(F, C))
         outv.x += 16.0f;
      if ((XBRZ_STEEP * d_bg <= d_dc) && !V_EQ3(E, G) && !V_EQ3(H, G))
         outv.x += 64.0f;
   }
#undef XP
   return outv * (1.0f / 255.0f);
