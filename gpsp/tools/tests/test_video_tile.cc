/* test_video_tile.cc -- the tile-row kernels of video_tile.h against the
 * per-pixel loops they replaced (render_tile_Nbpp / render_obj_tile_Nbpp of
 * video.cc, 4bpp, copied verbatim below).
 *
 *   x86:    g++ -O2 -o t tools/tests/test_video_tile.cc && ./t   (the scalar arm;
 *           proves the harness)
 *   armhf:  arm-linux-gnueabihf-g++ -O2 -static -marm -march=armv7-a \
 *             -mfpu=neon -mfloat-abi=hard -o t tools/tests/test_video_tile.cc
 *           qemu-arm ./t                                              (NEON)
 *   ./t quick   a smoke test.
 *
 * For each (destination type, rendtype, isbase, hflip) the renderer
 * instantiates: every nibble value in every lane position over random rows;
 * random rows (including all-zero, all-one, all-F); random destination words
 * (bit 8 and the high half random, which the STCKCOLOR sprite rule reads);
 * random flags and palettes; guard words either side of the 8 pixels that
 * must not change.  Exit 0 and "OK", or the first mismatch.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32;
#define VP_COVER(p) do { } while (0)
#include "../../video_tile.h"

/* ---- verbatim: render_tile_Nbpp, 4bpp, from `u32 tilepix` on ---------------- */
template <typename dtype, rendtype rdtype, bool isbase, bool hflip>
static void ref_bg(dtype *dest_ptr, u32 tilepix, u16 tile, u32 px_comb,
                   u32 bg_comb, const u16 *paltbl, u16 bgcolor)
{
    if (tilepix) {  // We can skip it all if the row is transparent
      u16 tilepal = (tile >> 12) << 4;
      u16 pxflg = px_comb | tilepal;
      const u16 *subpal = &paltbl[tilepal];
      for (u32 i = 0; i < 8; i++, dest_ptr++) {
        u8 pval = (hflip ? (tilepix >> ((7-i)*4)) : (tilepix >> (i*4))) & 0xF;
        if (pval) {
          if (rdtype == FULLCOLOR)
            *dest_ptr = subpal[pval];
          else if (rdtype == INDXCOLOR)
            *dest_ptr = pxflg | pval;
          else if (rdtype == STCKCOLOR)
            *dest_ptr = pxflg | pval | ((isbase ? bg_comb : *dest_ptr) << 16);
        }
        else if (isbase) {
          *dest_ptr = (rdtype == FULLCOLOR) ? bgcolor : 0 | bg_comb;
        }
      }
    } else if (isbase) {
      // In this case we simply fill the pixels with background pixels
      for (u32 i = 0; i < 8; i++, dest_ptr++) {
        *dest_ptr = (rdtype == FULLCOLOR) ? bgcolor : 0 | bg_comb;
      }
    }
}

/* ---- verbatim: render_obj_tile_Nbpp, 4bpp ---------------------------------- */
template <typename dsttype, rendtype rdtype, bool hflip>
static void ref_obj(dsttype *dest_ptr, u32 tilepix, u32 px_attr, const u16 *pal,
                    u16 palette)
{
    if (tilepix) {   // Can skip all pixels if the row is just transparent
      for (u32 i = 0; i < 8; i++, dest_ptr++) {
        u8 pval = (hflip ? (tilepix >> ((7-i)*4)) : (tilepix >> (i*4))) & 0xF;
        const u16 *subpal = &pal[palette];
        if (pval) {
          if (rdtype == FULLCOLOR)
            *dest_ptr = subpal[pval];
          else if (rdtype == INDXCOLOR)
            *dest_ptr = pval | px_attr;
          else if (rdtype == STCKCOLOR) {  // Stack background, replace sprite
            if (*dest_ptr & 0x100)
              *dest_ptr = pval | px_attr | ((*dest_ptr) & 0xFFFF0000);
            else
              *dest_ptr = pval | px_attr | ((*dest_ptr) << 16);
          }
          else if (rdtype == PIXCOPY)
            *dest_ptr = dest_ptr[240];
        }
      }
    }
}

static uint64_t rs = 0x2545F4914F6CDD1Dull;
static u32 rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (u32)(rs >> 16); }
static unsigned long long rows = 0;

#define G 8
template <typename D> static void fill(D *a, D *b)
{
  for (int i = 0; i < 8 + 2 * G; i++) {
    D v = (D)rnd();
    if (sizeof(D) == 4) v = (D)((u32)rnd() << 16 | (rnd() & 0xFFFF));
    if (rnd() & 1) v = (D)(v | 0x100); else v = (D)(v & ~0x100u);
    a[i] = b[i] = v;
  }
}

static void failx(const char *w, u32 tilepix, int i, u32 ref, u32 got)
{
  printf("FAIL %s tilepix=%08x lane %d ref=%08x got=%08x\n", w, tilepix, i, ref, got);
  exit(1);
}

template <typename D, rendtype rt, bool isbase, bool hflip>
static void t_bg(u32 tilepix, const u16 *pal)
{
  D a[8 + 2 * G], b[8 + 2 * G];
  fill(a, b);
  u16 tile = rnd() & 0xFFFF;
  u32 px_comb = (rnd() & 3) << 9, bg_comb = (rnd() & 3) << 9;
  ref_bg<D, rt, isbase, hflip>(a + G, tilepix, tile, px_comb, bg_comb, pal, pal[0]);
  vt_bg_row4<D, rt, isbase, hflip>(b + G, tilepix, (u16)((tile >> 12) << 4), px_comb,
                                   bg_comb, pal, pal[0]);
  rows++;
  for (int i = 0; i < 8 + 2 * G; i++)
    if (a[i] != b[i]) failx("bg", tilepix, i - G, a[i], b[i]);
}

template <typename D, rendtype rt, bool hflip>
static void t_obj(u32 tilepix, const u16 *pal)
{
  D a[8 + 2 * G], b[8 + 2 * G];
  fill(a, b);
  u16 palette = (rnd() & 0xF) << 4;
  u32 px_attr = ((rnd() & 3) << 9) | palette | 0x100 | ((rnd() & 1) << 11);
  ref_obj<D, rt, hflip>(a + G, tilepix, px_attr, pal, palette);
  vt_obj_row4<D, rt, hflip>(b + G, tilepix, px_attr, pal, palette);
  rows++;
  for (int i = 0; i < 8 + 2 * G; i++)
    if (a[i] != b[i]) failx("obj", tilepix, i - G, a[i], b[i]);
}

template <bool hflip> static void all_for(u32 tilepix, const u16 *pal)
{
  t_bg<u16, FULLCOLOR, true,  hflip>(tilepix, pal);
  t_bg<u16, FULLCOLOR, false, hflip>(tilepix, pal);
  t_bg<u16, INDXCOLOR, true,  hflip>(tilepix, pal);
  t_bg<u16, INDXCOLOR, false, hflip>(tilepix, pal);
  t_bg<u32, INDXCOLOR, true,  hflip>(tilepix, pal);
  t_bg<u32, INDXCOLOR, false, hflip>(tilepix, pal);
  t_bg<u32, STCKCOLOR, true,  hflip>(tilepix, pal);
  t_bg<u32, STCKCOLOR, false, hflip>(tilepix, pal);
  t_obj<u16, FULLCOLOR, hflip>(tilepix, pal);
  t_obj<u16, INDXCOLOR, hflip>(tilepix, pal);
  t_obj<u32, INDXCOLOR, hflip>(tilepix, pal);
  t_obj<u32, STCKCOLOR, hflip>(tilepix, pal);
}

static void every(u32 tilepix, const u16 *pal)
{
  all_for<false>(tilepix, pal);
  all_for<true>(tilepix, pal);
}

int main(int argc, char **argv)
{
  bool quick = argc > 1 && !strcmp(argv[1], "quick");
  printf("test_video_tile: %s kernels%s\n", VT_NEON ? "NEON" : "scalar", quick ? " (quick)" : "");
  u16 pal[512];
  for (int it = 0; it < (quick ? 20 : 300); it++) {
    for (int i = 0; i < 512; i++) pal[i] = rnd();
    every(0, pal); every(0xFFFFFFFFu, pal); every(0x11111111u, pal);
    // every nibble value in every lane position, the other lanes random
    for (int p = 0; p < 8; p++)
      for (u32 v = 0; v < 16; v++)
        for (int k = 0; k < 4; k++) {
          u32 t = rnd() * 2654435761u + rnd();
          t = (t & ~(0xFu << (4 * p))) | (v << (4 * p));
          every(t, pal);
        }
    // sparse rows (a few opaque pixels) and dense random ones
    for (int k = 0; k < (quick ? 200 : 4000); k++) {
      u32 t = (rnd() << 16) ^ rnd();
      if (k & 1) t &= (rnd() << 16) ^ rnd();     // thinner
      if (k & 2) t &= (rnd() << 16) ^ rnd();
      every(t, pal);
    }
  }
  printf("OK (%llu row/arm/destination checks)\n", rows);
  return 0;
}
