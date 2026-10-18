/* xbrz_pass0.inc -- GPU xBRZ pass 0: the BODY of the fragment program
 * (xbrz_common.inc has the dialect).  Drawn 1:1 into a render target one
 * pixel larger than the picture each way, its texel (i+1, j+1) for the
 * picture's pixel (i, j) -- so the corners of the top row and the left
 * column have one to read too.  In scope:
 *   float2 vTexcoord      this pixel's place in Source, 0..1 (-1/w .. 1)
 *   sampler2D Source      the game's picture, point sampled
 *   float4 SourceSize     Source's width, height, 1/width, 1/height
 *
 * ONE CORNER PER PIXEL, as the CPU xBRZ's own preprocessing works: the
 * corner where E, F, H and I meet decides, at once, the blend of E's
 * bottom-right corner, F's bottom-left, H's top-right and I's top-left
 * (it is the same point, and the same two diagonals are compared).  The
 * four come out as bytes, x = E's, y = F's, z = H's, w = I's: 0 none,
 * 1 normal, 2 dominant -- over 255.  (libretro's single pass 0 worked
 * this out four times per pixel, once from each of its corners; it was
 * also too big for the Vita's compiler, which stops with "fatal internal
 * error".)
 *
 * Source pixels around E:  -|B|C|-
 *                          D|E|F|x
 *                          G|H|I|x
 *                          -|x|x|-                                      */
   float2 pos = frac(vTexcoord * V_XY(SourceSize)) - float2(0.5f, 0.5f);
   float2 coord = vTexcoord - pos * V_ZW(SourceSize);
#define XP(x, y) V_RGB(tex2D(Source, coord + V_ZW(SourceSize) * float2(x, y)))
   float3 E = XP(0.0f, 0.0f);
   float3 F = XP(1.0f, 0.0f);
   float3 H = XP(0.0f, 1.0f);
   float3 I = XP(1.0f, 1.0f);
   float4 res = float4(XBRZ_BLEND_NONE, XBRZ_BLEND_NONE, XBRZ_BLEND_NONE,
                       XBRZ_BLEND_NONE);
   if (!((V_EQ3(E, F) && V_EQ3(H, I)) || (V_EQ3(E, H) && V_EQ3(F, I))))
   {
      float3 B = XP( 0.0f, -1.0f);
      float3 C = XP( 1.0f, -1.0f);
      float3 D = XP(-1.0f,  0.0f);
      float3 G = XP(-1.0f,  1.0f);
      /* along the H-F diagonal, and along E-I */
      float d_hf = xbrz_dist(G, E) + xbrz_dist(E, C) +
                   xbrz_dist(XP(0.0f, 2.0f), I) + xbrz_dist(I, XP(2.0f, 0.0f)) +
                   4.0f * xbrz_dist(H, F);
      float d_ei = xbrz_dist(D, H) + xbrz_dist(H, XP(1.0f, 2.0f)) +
                   xbrz_dist(B, F) + xbrz_dist(F, XP(2.0f, 1.0f)) +
                   4.0f * xbrz_dist(E, I);
      if (d_hf < d_ei)
      {
         /* the edge runs H-F: E and I are the ones cut */
         float b = (XBRZ_DOMINANT * d_hf < d_ei) ? XBRZ_BLEND_DOMINANT
                                                 : XBRZ_BLEND_NORMAL;
         res.x = (!V_EQ3(E, F) && !V_EQ3(E, H)) ? b : XBRZ_BLEND_NONE;
         res.w = (!V_EQ3(I, H) && !V_EQ3(I, F)) ? b : XBRZ_BLEND_NONE;
      }
      else if (d_ei < d_hf)
      {
         /* the edge runs E-I: F and H are */
         float b = (XBRZ_DOMINANT * d_ei < d_hf) ? XBRZ_BLEND_DOMINANT
                                                 : XBRZ_BLEND_NORMAL;
         res.y = (!V_EQ3(F, E) && !V_EQ3(F, I)) ? b : XBRZ_BLEND_NONE;
         res.z = (!V_EQ3(H, E) && !V_EQ3(H, I)) ? b : XBRZ_BLEND_NONE;
      }
   }
#undef XP
   return res * (1.0f / 255.0f);
