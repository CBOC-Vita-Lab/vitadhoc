/***************************************************************************************
 *  Genesis Plus
 *  Video Display Processor (pixel output rendering)
 *
 *  Support for all TMS99xx modes, Mode 4 & Mode 5 rendering
 *
 *  Copyright (C) 1998-2003  Charles Mac Donald (original code)
 *  Copyright (C) 2007-2025  Eke-Eke (Genesis Plus GX)
 *  Copyright (C) 2022  AlexKiri (enhanced vscroll mode rendering function)
 *
 *  Redistribution and use of this code or any derivative works are permitted
 *  provided that the following conditions are met:
 *
 *   - Redistributions may not be sold, nor may they be used in a commercial
 *     product or activity.
 *
 *   - Redistributions that are modified from the original source must include the
 *     complete source code, including the source code for all components used by a
 *     binary built from the modified sources. However, as a special exception, the
 *     source code distributed need not include anything that is normally distributed
 *     (in either source or binary form) with the major components (compiler, kernel,
 *     and so on) of the operating system on which the executable runs, unless that
 *     component itself accompanies the executable.
 *
 *   - Redistributions must reproduce the above copyright notice, this list of
 *     conditions and the following disclaimer in the documentation and/or other
 *     materials provided with the distribution.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 *  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************************/

/* VitAdhoc: this file is compiled twice (vdp_render_w.c renames its globals for the VDP worker); the call wrappers of vdp_thread.h are not for it */
#define VDPW_NO_WRAP
#include "shared.h"

#ifdef __ARM_NEON
#pragma push_macro("reg")
#pragma push_macro("sat")
#pragma push_macro("status")
#pragma push_macro("bitmap")
#undef reg
#undef sat
#undef status
#undef bitmap
#include <arm_neon.h>
#pragma pop_macro("bitmap")
#pragma pop_macro("status")
#pragma pop_macro("sat")
#pragma pop_macro("reg")
#endif

extern int8 reset_do_not_clear_buffers;

#ifndef HAVE_NO_SPRITE_LIMIT
#define MAX_SPRITES_PER_LINE 20
#define TMS_MAX_SPRITES_PER_LINE 4
#define MODE4_MAX_SPRITES_PER_LINE 8
#define MODE5_MAX_SPRITES_PER_LINE (bitmap.viewport.w >> 4)
#define MODE5_MAX_SPRITE_PIXELS max_sprite_pixels
#endif

/*** NTSC Filters ***/


/* Output pixels type*/
#if defined(USE_8BPP_RENDERING)
#define PIXEL_OUT_T uint8
#elif defined(USE_32BPP_RENDERING)
#define PIXEL_OUT_T uint32
#else
#define PIXEL_OUT_T uint16
#endif


/* Pixel priority look-up tables information */
#define LUT_MAX     (6)
#define LUT_SIZE    (0x10000)


#ifdef ALIGN_LONG
#undef READ_LONG
#undef WRITE_LONG

INLINE uint32 READ_LONG(void *address)
{
  if ((uint32)address & 3)
  {
#ifdef LSB_FIRST  /* little endian version */
    return ( *((uint8 *)address) +
        (*((uint8 *)address+1) << 8)  +
        (*((uint8 *)address+2) << 16) +
        (*((uint8 *)address+3) << 24) );
#else       /* big endian version */
    return ( *((uint8 *)address+3) +
        (*((uint8 *)address+2) << 8)  +
        (*((uint8 *)address+1) << 16) +
        (*((uint8 *)address)   << 24) );
#endif  /* LSB_FIRST */
  }
  else return *(uint32 *)address;
}

INLINE void WRITE_LONG(void *address, uint32 data)
{
  if ((uint32)address & 3)
  {
#ifdef LSB_FIRST
      *((uint8 *)address) =  data;
      *((uint8 *)address+1) = (data >> 8);
      *((uint8 *)address+2) = (data >> 16);
      *((uint8 *)address+3) = (data >> 24);
#else
      *((uint8 *)address+3) =  data;
      *((uint8 *)address+2) = (data >> 8);
      *((uint8 *)address+1) = (data >> 16);
      *((uint8 *)address)   = (data >> 24);
#endif /* LSB_FIRST */
    return;
  }
  else *(uint32 *)address = data;
}

#else  /* !ALIGN_LONG */
#undef READ_LONG
#undef WRITE_LONG

/* Unaligned 32-bit load/store via memcpy: lowers to a single unaligned ldr/str
 * on ARMv7 Cortex-A9 and x86 with zero strict-aliasing UB, zero UBSan alignment
 * faults, and no ARMv7 ldm/stm/strd merging across unaligned linebuf pointers. */
#if defined(ALT_RENDERER) && !(defined(__ARM_NEON) && defined(LSB_FIRST))
INLINE uint32 READ_LONG(const void *address)
{
  uint32 data;
  memcpy(&data, address, sizeof(data));
  return data;
}
#endif

INLINE void WRITE_LONG(void *address, uint32 data)
{
  memcpy(address, &data, sizeof(data));
}

#endif  /* ALIGN_LONG */

#ifdef __ARM_NEON
/* Exact branchless NEON equivalents of make_lut_bg (ste == 0, lut[0]) and
 * make_lut_bg_ste (ste != 0, lut[2]), eliminating 64 KiB L1D LUT thrashing.
 * Verified exhaustively across all 2 * 65,536 (bx, ax) byte pairs. */
INLINE uint8x16_t merge_bg_16_neon(uint8x16_t ax, uint8x16_t bx, int ste)
{
  const uint8x16_t v0f = vdupq_n_u8(0x0F);
  const uint8x16_t v40 = vdupq_n_u8(0x40);
  const uint8x16_t v7f = vdupq_n_u8(0x7F);
  uint8x16_t a_nz   = vtstq_u8(ax, v0f);
  uint8x16_t b_nz   = vtstq_u8(bx, v0f);
  uint8x16_t b_prio = vtstq_u8(vbicq_u8(bx, ax), v40);
  uint8x16_t pick_a = vbicq_u8(a_nz, vandq_u8(b_nz, b_prio));
  uint8x16_t any_nz = vorrq_u8(a_nz, b_nz);
  uint8x16_t c      = vandq_u8(vbslq_u8(pick_a, ax, bx), vandq_u8(any_nz, v7f));
  if (ste)
  {
    uint8x16_t hi = vshlq_n_u8(vandq_u8(vorrq_u8(ax, bx), v40), 1);
    c = vorrq_u8(c, hi);
  }
  return c;
}

INLINE uint8x8_t merge_bg_8_neon(uint8x8_t ax, uint8x8_t bx, int ste)
{
  const uint8x8_t v0f = vdup_n_u8(0x0F);
  const uint8x8_t v40 = vdup_n_u8(0x40);
  const uint8x8_t v7f = vdup_n_u8(0x7F);
  uint8x8_t a_nz   = vtst_u8(ax, v0f);
  uint8x8_t b_nz   = vtst_u8(bx, v0f);
  uint8x8_t b_prio = vtst_u8(vbic_u8(bx, ax), v40);
  uint8x8_t pick_a = vbic_u8(a_nz, vand_u8(b_nz, b_prio));
  uint8x8_t any_nz = vorr_u8(a_nz, b_nz);
  uint8x8_t c      = vand_u8(vbsl_u8(pick_a, ax, bx), vand_u8(any_nz, v7f));
  if (ste)
  {
    uint8x8_t hi = vshl_n_u8(vand_u8(vorr_u8(ax, bx), v40), 1);
    c = vorr_u8(c, hi);
  }
  return c;
}
#endif  /* __ARM_NEON */


/* Draw 2-cell column (8-pixels high) */
/*
   Pattern cache base address: VHN NNNNNNNN NNYYYxxx
   with :
      x = Pattern Pixel (0-7)
      Y = Pattern Row (0-7)
      N = Pattern Number (0-2047) from pattern attribute
      H = Horizontal Flip bit from pattern attribute
      V = Vertical Flip bit from pattern attribute
*/
#define GET_LSB_TILE(ATTR, LINE) \
  atex = atex_table[(ATTR >> 13) & 7]; \
  src = (uint32 *)&bg_pattern_cache[(ATTR & 0x00001FFF) << 6 | (LINE)];
#define GET_MSB_TILE(ATTR, LINE) \
  atex = atex_table[(ATTR >> 29) & 7]; \
  src = (uint32 *)&bg_pattern_cache[(ATTR & 0x1FFF0000) >> 10 | (LINE)];

/* Draw 2-cell column (16 pixels high) */
/*
   Pattern cache base address: VHN NNNNNNNN NYYYYxxx
   with :
      x = Pattern Pixel (0-7)
      Y = Pattern Row (0-15)
      N = Pattern Number (0-1023)
      H = Horizontal Flip bit
      V = Vertical Flip bit
*/
#define GET_LSB_TILE_IM2(ATTR, LINE) \
  atex = atex_table[(ATTR >> 13) & 7]; \
  src = (uint32 *)&bg_pattern_cache[((ATTR & 0x000003FF) << 7 | (ATTR & 0x00001800) << 6 | (LINE)) ^ ((ATTR & 0x00001000) >> 6)];
#define GET_MSB_TILE_IM2(ATTR, LINE) \
  atex = atex_table[(ATTR >> 29) & 7]; \
  src = (uint32 *)&bg_pattern_cache[((ATTR & 0x03FF0000) >> 9 | (ATTR & 0x18000000) >> 10 | (LINE)) ^ ((ATTR & 0x10000000) >> 22)];

/*
   One column = 2 tiles
   Two pattern attributes are written in VRAM as two consecutives 16-bit words:

   P = priority bit
   C = color palette (2 bits)
   V = Vertical Flip bit
   H = Horizontal Flip bit
   N = Pattern Number (11 bits)

   (MSB) PCCVHNNN NNNNNNNN (LSB) (MSB) PCCVHNNN NNNNNNNN (LSB)
              PATTERN1                      PATTERN2

   Both pattern attributes are read from VRAM as one 32-bit word:

   LIT_ENDIAN: (MSB) PCCVHNNN NNNNNNNN PCCVHNNN NNNNNNNN (LSB)
                          PATTERN2          PATTERN1

   BIG_ENDIAN: (MSB) PCCVHNNN NNNNNNNN PCCVHNNN NNNNNNNN (LSB)
                          PATTERN1          PATTERN2


   In line buffers, one pixel = one byte: (msb) 0Pppcccc (lsb)
   with:
      P = priority bit  (from pattern attribute)
      p = color palette (from pattern attribute)
      c = color data (from pattern cache)

   One pattern = 8 pixels = 8 bytes = two 32-bit writes per pattern
*/

#ifdef LSB_FIRST
#define DRAW_COLUMN(ATTR, LINE) \
  GET_LSB_TILE(ATTR, LINE) \
  WRITE_LONG(dst, src[0] | atex); \
  WRITE_LONG(dst + 4, src[1] | atex); \
  GET_MSB_TILE(ATTR, LINE) \
  WRITE_LONG(dst + 8, src[0] | atex); \
  WRITE_LONG(dst + 12, src[1] | atex); \
  dst += 16;
#define DRAW_COLUMN_IM2(ATTR, LINE) \
  GET_LSB_TILE_IM2(ATTR, LINE) \
  WRITE_LONG(dst, src[0] | atex); \
  WRITE_LONG(dst + 4, src[1] | atex); \
  GET_MSB_TILE_IM2(ATTR, LINE) \
  WRITE_LONG(dst + 8, src[0] | atex); \
  WRITE_LONG(dst + 12, src[1] | atex); \
  dst += 16;
#else
#define DRAW_COLUMN(ATTR, LINE) \
  GET_MSB_TILE(ATTR, LINE) \
  WRITE_LONG(dst, src[0] | atex); \
  WRITE_LONG(dst + 4, src[1] | atex); \
  GET_LSB_TILE(ATTR, LINE) \
  WRITE_LONG(dst + 8, src[0] | atex); \
  WRITE_LONG(dst + 12, src[1] | atex); \
  dst += 16;
#define DRAW_COLUMN_IM2(ATTR, LINE) \
  GET_MSB_TILE_IM2(ATTR, LINE) \
  WRITE_LONG(dst, src[0] | atex); \
  WRITE_LONG(dst + 4, src[1] | atex); \
  GET_LSB_TILE_IM2(ATTR, LINE) \
  WRITE_LONG(dst + 8, src[0] | atex); \
  WRITE_LONG(dst + 12, src[1] | atex); \
  dst += 16;
#endif

#ifdef ALT_RENDERER
/* Draw background tiles directly using priority look-up table (or 16-wide NEON on ARM) */
/* SRC_A = layer A rendered pixel line (4 bytes = 4 pixels at once) */
/* SRC_B = layer B cached pixel line (4 bytes = 4 pixels at once) */
/* Note: cache address is always aligned so no need to use READ_LONG macro */

#ifdef LSB_FIRST
#define DRAW_BG_TILE(SRC_A, SRC_B) \
  *lb++ = table[((SRC_B << 8) & 0xff00) | (SRC_A & 0xff)]; \
  *lb++ = table[(SRC_B & 0xff00) | ((SRC_A >> 8) & 0xff)]; \
  *lb++ = table[((SRC_B >> 8) & 0xff00) | ((SRC_A >> 16) & 0xff)]; \
  *lb++ = table[((SRC_B >> 16) & 0xff00) | ((SRC_A >> 24) & 0xff)];
#else
#define DRAW_BG_TILE(SRC_A, SRC_B) \
  *lb++ = table[((SRC_B >> 16) & 0xff00) | ((SRC_A >> 24) & 0xff)]; \
  *lb++ = table[((SRC_B >> 8) & 0xff00) | ((SRC_A >> 16) & 0xff)]; \
  *lb++ = table[(SRC_B & 0xff00) | ((SRC_A >> 8) & 0xff)]; \
  *lb++ = table[((SRC_B << 8) & 0xff00) | (SRC_A & 0xff)];
#endif

#if defined(__ARM_NEON) && defined(LSB_FIRST)
#define DRAW_BG_COLUMN(ATTR, LINE, SRC_A, SRC_B) \
  { \
    const uint8 *_s0 = &bg_pattern_cache[((ATTR) & 0x00001FFF) << 6 | (LINE)]; \
    const uint8 *_s1 = &bg_pattern_cache[((ATTR) & 0x1FFF0000) >> 10 | (LINE)]; \
    uint8x8_t _b0 = vorr_u8(vld1_u8(_s0), vdup_n_u8((uint8)(((ATTR) >> 9) & 0x70))); \
    uint8x8_t _b1 = vorr_u8(vld1_u8(_s1), vdup_n_u8((uint8)(((ATTR) >> 25) & 0x70))); \
    uint8x16_t _bx = vcombine_u8(_b0, _b1); \
    uint8x16_t _ax = vld1q_u8(lb); \
    vst1q_u8(lb, merge_bg_16_neon(_ax, _bx, (table != lut[0]))); \
    lb += 16; \
  }
#define DRAW_BG_COLUMN_IM2(ATTR, LINE, SRC_A, SRC_B) \
  { \
    const uint8 *_s0 = &bg_pattern_cache[(((ATTR) & 0x000003FF) << 7 | ((ATTR) & 0x00001800) << 6 | (LINE)) ^ (((ATTR) & 0x00001000) >> 6)]; \
    const uint8 *_s1 = &bg_pattern_cache[(((ATTR) & 0x03FF0000) >> 9 | ((ATTR) & 0x18000000) >> 10 | (LINE)) ^ (((ATTR) & 0x10000000) >> 22)]; \
    uint8x8_t _b0 = vorr_u8(vld1_u8(_s0), vdup_n_u8((uint8)(((ATTR) >> 9) & 0x70))); \
    uint8x8_t _b1 = vorr_u8(vld1_u8(_s1), vdup_n_u8((uint8)(((ATTR) >> 25) & 0x70))); \
    uint8x16_t _bx = vcombine_u8(_b0, _b1); \
    uint8x16_t _ax = vld1q_u8(lb); \
    vst1q_u8(lb, merge_bg_16_neon(_ax, _bx, (table != lut[0]))); \
    lb += 16; \
  }
#elif defined(LSB_FIRST)
#define DRAW_BG_COLUMN(ATTR, LINE, SRC_A, SRC_B) \
  GET_LSB_TILE(ATTR, LINE) \
  SRC_A = READ_LONG(lb); \
  SRC_B = (src[0] | atex); \
  DRAW_BG_TILE(SRC_A, SRC_B) \
  SRC_A = READ_LONG(lb); \
  SRC_B = (src[1] | atex); \
  DRAW_BG_TILE(SRC_A, SRC_B) \
  GET_MSB_TILE(ATTR, LINE) \
  SRC_A = READ_LONG(lb); \
  SRC_B = (src[0] | atex); \
  DRAW_BG_TILE(SRC_A, SRC_B) \
  SRC_A = READ_LONG(lb); \
  SRC_B = (src[1] | atex); \
  DRAW_BG_TILE(SRC_A, SRC_B)
#define DRAW_BG_COLUMN_IM2(ATTR, LINE, SRC_A, SRC_B) \
  GET_LSB_TILE_IM2(ATTR, LINE) \
  SRC_A = READ_LONG(lb); \
  SRC_B = (src[0] | atex); \
  DRAW_BG_TILE(SRC_A, SRC_B) \
  SRC_A = READ_LONG(lb); \
  SRC_B = (src[1] | atex); \
  DRAW_BG_TILE(SRC_A, SRC_B) \
  GET_MSB_TILE_IM2(ATTR, LINE) \
  SRC_A = READ_LONG(lb); \
  SRC_B = (src[0] | atex); \
  DRAW_BG_TILE(SRC_A, SRC_B) \
  SRC_A = READ_LONG(lb); \
  SRC_B = (src[1] | atex); \
  DRAW_BG_TILE(SRC_A, SRC_B)
#else
#define DRAW_BG_COLUMN(ATTR, LINE, SRC_A, SRC_B) \
  GET_MSB_TILE(ATTR, LINE) \
  SRC_A = READ_LONG(lb); \
  SRC_B = (src[0] | atex); \
  DRAW_BG_TILE(SRC_A, SRC_B) \
  SRC_A = READ_LONG(lb); \
  SRC_B = (src[1] | atex); \
  DRAW_BG_TILE(SRC_A, SRC_B) \
  GET_LSB_TILE(ATTR, LINE) \
  SRC_A = READ_LONG(lb); \
  SRC_B = (src[0] | atex); \
  DRAW_BG_TILE(SRC_A, SRC_B) \
  SRC_A = READ_LONG(lb); \
  SRC_B = (src[1] | atex); \
  DRAW_BG_TILE(SRC_A, SRC_B)
#define DRAW_BG_COLUMN_IM2(ATTR, LINE, SRC_A, SRC_B) \
  GET_MSB_TILE_IM2(ATTR, LINE) \
  SRC_A = READ_LONG(lb); \
  SRC_B = (src[0] | atex); \
  DRAW_BG_TILE(SRC_A, SRC_B) \
  SRC_A = READ_LONG(lb); \
  SRC_B = (src[1] | atex); \
  DRAW_BG_TILE(SRC_A, SRC_B) \
  GET_LSB_TILE_IM2(ATTR, LINE) \
  SRC_A = READ_LONG(lb); \
  SRC_B = (src[0] | atex); \
  DRAW_BG_TILE(SRC_A, SRC_B) \
  SRC_A = READ_LONG(lb); \
  SRC_B = (src[1] | atex); \
  DRAW_BG_TILE(SRC_A, SRC_B)
#endif
#endif /* ALT_RENDERER */

#define DRAW_SPRITE_TILE(WIDTH,ATTR,TABLE)  \
  for (i=0;i<WIDTH;i++) \
  { \
    temp = *src++; \
    if (temp & 0x0f) \
    { \
      temp |= (lb[i] << 8); \
      lb[i] = TABLE[temp | ATTR]; \
      status |= ((temp & 0x8000) >> 10); \
    } \
  }

#define DRAW_SPRITE_TILE_ACCURATE(WIDTH,ATTR,TABLE)  \
  for (i=0;i<WIDTH;i++) \
  { \
    temp = *src++; \
    if (temp & 0x0f) \
    { \
      temp |= (lb[i] << 8); \
      lb[i] = TABLE[temp | ATTR]; \
      if ((temp & 0x8000) && !(status & 0x20)) \
      { \
        spr_col = (v_counter << 8) | ((xpos + i + 13) >> 1); \
        status |= 0x20; \
      } \
    } \
  }

#define DRAW_SPRITE_TILE_ACCURATE_2X(WIDTH,ATTR,TABLE)  \
  for (i=0;i<WIDTH;i+=2) \
  { \
    temp = *src++; \
    if (temp & 0x0f) \
    { \
      temp |= (lb[i] << 8); \
      lb[i] = TABLE[temp | ATTR]; \
      if ((temp & 0x8000) && !(status & 0x20)) \
      { \
        spr_col = (v_counter << 8) | ((xpos + i + 13) >> 1); \
        status |= 0x20; \
      } \
      temp &= 0x00FF; \
      temp |= (lb[i+1] << 8); \
      lb[i+1] = TABLE[temp | ATTR]; \
      if ((temp & 0x8000) && !(status & 0x20)) \
      { \
        spr_col = (v_counter << 8) | ((xpos + i + 1 + 13) >> 1); \
        status |= 0x20; \
      } \
    } \
  }


/* Pixels conversion macro */
/* 4-bit color channels are either compressed to 2/3-bit or dithered to 5/6/8-bit equivalents */
/* 3:3:2 RGB */
#if defined(USE_8BPP_RENDERING)
#define MAKE_PIXEL(r,g,b)  (((r) >> 1) << 5 | ((g) >> 1) << 2 | (b) >> 2)

/* 5:5:5 RGB */
#elif defined(USE_15BPP_RENDERING)
#if defined(USE_ABGR)
#define MAKE_PIXEL(r,g,b) ((1 << 15) | (b) << 11 | ((b) >> 3) << 10 | (g) << 6 | ((g) >> 3) << 5 | (r) << 1 | (r) >> 3)
#else
#define MAKE_PIXEL(r,g,b) ((1 << 15) | (r) << 11 | ((r) >> 3) << 10 | (g) << 6 | ((g) >> 3) << 5 | (b) << 1 | (b) >> 3)
#endif
/* 5:6:5 RGB */
#elif defined(USE_16BPP_RENDERING)
#define MAKE_PIXEL(r,g,b) ((r) << 12 | ((r) >> 3) << 11 | (g) << 7 | ((g) >> 2) << 5 | (b) << 1 | (b) >> 3)

/* 8:8:8 RGB */
#elif defined(USE_32BPP_RENDERING)
#define MAKE_PIXEL(r,g,b) ((0xff << 24) | (r) << 20 | (r) << 16 | (g) << 12 | (g)  << 8 | (b) << 4 | (b))
#endif

/* Window & Plane A clipping */
static struct clip_t
{
  uint8 left;
  uint8 right;
  uint8 enable;
} clip[2];

/* VitAdhoc wide view: the strip pass (see vdp_render.h). While wide_pass is set the plane and sprite renderers draw wide_cols columns with the
 * picture shifted by wide_dx pixels; outside the pass every macro below is the original expression. */
int wide_m;
int wide_planes = 3;                 /* bit 0: plane A, bit 1: plane B are drawn into the strips (a plane left out is transparent there) */
uint32 wide_opq[256][2][3];         /* per line and side: bit d set = the strip pixel at distance d from the screen's edge is opaque */
wide_line_t wide_rec[256];
uint16 wide_pal[32];                 /* mode 4: the 32 colours as RGB565 as line 0 of the frame had them */
static int wide_pass, wide_cols, wide_px, wide_dx;
static int wide_a;                   /* the strip pass draws plane A (0 on the side of a line the window plane takes) */
static uint32 wide_vsb[20];          /* the strip pass of a per 2-cell vertical scroll line: every column scrolls like the column at the edge */
static uint32 wide_ys[2];
/* learned rows (wide_row_set): name table rows of plane A / B the strip pass reads instead of the VRAM row, while the VRAM row is still what
 * it was when the row was given */
static uint16 ALIGNED_(4) wide_ovr[2][4096], wide_ovo[2][4096];
static uint8 wide_ovh[2][128];
static int wide_ovn, wide_ovany;
static uint32 *wide_ont[2];
/* the strip cache (the worker's copy only): a line's strips are copied from the last frame while nothing the renderer reads has changed */
typedef struct { uint32 gen, opq[2][3]; uint8 ok, side, spr_in, pad; PIXEL_OUT_T px[2][96]; } wide_cache_t;
static wide_cache_t *wide_cache;
uint32 wide_gen = 1, wide_parse_gen;
int wide_cache_on, wide_cache_check;
unsigned long wide_cache_stat[3];    /* strips copied, strips drawn, check: copies that differed from a fresh draw */
static int wc_last_line = 999, wc_hits = 0, wc_lines = 0, wc_low_streak = 0, wc_cool = 0, wc_prime = 0;
#define WIDE_COLS(orig)   (wide_pass ? wide_cols : (orig))
#define WIDE_VW           (wide_pass ? wide_px : bitmap.viewport.w)
#define WIDE_SPR_DX       (wide_pass ? wide_dx : 0)
#ifdef LSB_FIRST
static inline uint32 wide_xs(uint32 x) { return wide_pass ? ((((x & 0xFFFF) + (uint32) wide_dx) & 0xFFFF) | (((x >> 16) + (uint32) wide_dx) << 16)) : x; }
#else
static inline uint32 wide_xs(uint32 x) { return wide_pass ? (((((x >> 16) + (uint32) wide_dx) & 0xFFFF) << 16) | ((x + (uint32) wide_dx) & 0xFFFF)) : x; }
#endif

/* Pattern attribute (priority + palette bits) expansion table */
static const uint32 atex_table[] =
{
  0x00000000,
  0x10101010,
  0x20202020,
  0x30303030,
  0x40404040,
  0x50505050,
  0x60606060,
  0x70707070
};

/* fixed Master System palette for Modes 0,1,2,3 */
static const uint8 tms_crom[16] =
{
  0x00, 0x00, 0x08, 0x0C,
  0x10, 0x30, 0x01, 0x3C,
  0x02, 0x03, 0x05, 0x0F,
  0x04, 0x33, 0x15, 0x3F
};

/* original SG-1000 palette */
#if defined(USE_8BPP_RENDERING)
static const uint8 tms_palette[16] =
{
  0x00, 0x00, 0x39, 0x79,
  0x4B, 0x6F, 0xC9, 0x5B,
  0xE9, 0xED, 0xD5, 0xD9,
  0x35, 0xCE, 0xDA, 0xFF
};

#elif defined(USE_15BPP_RENDERING)
static const uint16 tms_palette[16] =
{
  0x8000, 0x8000, 0x9308, 0xAF6F,
  0xA95D, 0xBDDF, 0xE949, 0xA3BE,
  0xFD4A, 0xFDEF, 0xEB0A, 0xF330,
  0x92A7, 0xE177, 0xE739, 0xFFFF
};

#elif defined(USE_16BPP_RENDERING)
static const uint16 tms_palette[16] =
{
  0x0000, 0x0000, 0x2648, 0x5ECF,
  0x52BD, 0x7BBE, 0xD289, 0x475E,
  0xF2AA, 0xFBCF, 0xD60A, 0xE670,
  0x2567, 0xC2F7, 0xCE59, 0xFFFF
};

#elif defined(USE_32BPP_RENDERING)
static const uint32 tms_palette[16] =
{
  0xFF000000, 0xFF000000, 0xFF21C842, 0xFF5EDC78,
  0xFF5455ED, 0xFF7D76FC, 0xFFD4524D, 0xFF42EBF5,
  0xFFFC5554, 0xFFFF7978, 0xFFD4C154, 0xFFE6CE80,
  0xFF21B03B, 0xFFC95BB4, 0xFFCCCCCC, 0xFFFFFFFF
};
#endif

/* Cached and flipped patterns */
static uint8 ALIGNED_(8) bg_pattern_cache[0x80000];

/* Sprite pattern name offset look-up table (Mode 5) */
static uint8 name_lut[0x400];

/* Bitplane to packed pixel look-up table (Mode 4) */
static uint32 bp_lut[0x10000];

/* Layer priority pixel look-up tables */
static uint8 lut[LUT_MAX][LUT_SIZE];

/* Output pixel data look-up tables*/
static PIXEL_OUT_T pixel[0x100];
static PIXEL_OUT_T pixel_lut[3][0x200];
static PIXEL_OUT_T pixel_lut_m4[0x40];

/* Background & Sprite line buffers */
static uint8 linebuf[2][0x200];

/* Sprite limit flag */
static uint8 spr_ovr;

/* Sprite parsing lists */
typedef struct
{
  uint16 ypos;
  uint16 xpos;
  uint16 attr;
  uint16 size;
} object_info_t;

static object_info_t obj_info[2][MAX_SPRITES_PER_LINE];

/* Sprite Counter */
static uint8 object_count[2];

/* Sprite Collision Info */
uint16 spr_col;

/* VitAdhoc: set by parse_satb_tms (status bits 0-4 written by the renderer) */
uint8 tms_status_set;

/* VitAdhoc: what the renderer carries from the end of one frame into the next and no save state holds: the sprite list parsed for the
 * first line, the pending overflow flag and the collision position. A state load clears them (render_reset), which is what a load from
 * a file has always done; run-ahead (md_host_run_frame_ahead) goes back to a moment it has just left and must find them as they were. */
typedef struct
{
  object_info_t obj[2][MAX_SPRITES_PER_LINE];
  uint8 count[2];
  uint8 ovr;
  uint8 tms_set;
  uint16 col;
} render_carry_t;

int render_carry_size(void) { return (int)sizeof(render_carry_t); }

/* (the block sits at any byte offset of a state buffer: copy through a local, never cast the pointer) */
void render_carry_save(void *p)
{
  render_carry_t c;
  memset(&c, 0, sizeof c);
  memcpy(c.obj, obj_info, sizeof(obj_info));
  c.count[0] = object_count[0]; c.count[1] = object_count[1];
  c.ovr = spr_ovr; c.tms_set = 0; c.col = spr_col;     /* tms_set: "the worker has status bits to merge" is settled at a frame's end (and never cleared without the worker) */
  memcpy(p, &c, sizeof c);
}

void render_carry_load(void *p)
{
  render_carry_t c;
  memcpy(&c, p, sizeof c);
  memcpy(obj_info, c.obj, sizeof(obj_info));
  object_count[0] = c.count[0] > MAX_SPRITES_PER_LINE ? MAX_SPRITES_PER_LINE : c.count[0];     /* a state file is the user's own: stay in the array */
  object_count[1] = c.count[1] > MAX_SPRITES_PER_LINE ? MAX_SPRITES_PER_LINE : c.count[1];
  spr_ovr = c.ovr; tms_status_set = c.tms_set; spr_col = c.col;
}

/* Function pointers */
void (*render_bg)(int line);
void (*render_obj)(int line);
void (*parse_satb)(int line);
void (*update_bg_pattern_cache)(int index);


/*--------------------------------------------------------------------------*/
/* Sprite pattern name offset look-up table function (Mode 5)               */
/*--------------------------------------------------------------------------*/

static void make_name_lut(void)
{
  int vcol, vrow;
  int width, height;
  int flipx, flipy;
  int i;

  for (i = 0; i < 0x400; i += 1)
  {
    /* Sprite settings */
    vcol = i & 3;
    vrow = (i >> 2) & 3;
    height = (i >> 4) & 3;
    width  = (i >> 6) & 3;
    flipx  = (i >> 8) & 1;
    flipy  = (i >> 9) & 1;

    if ((vrow > height) || vcol > width)
    {
      /* Invalid settings (unused) */
      name_lut[i] = -1;
    }
    else
    {
      /* Adjust column & row index if sprite is flipped */
      if(flipx) vcol = (width - vcol);
      if(flipy) vrow = (height - vrow);

      /* Pattern offset (pattern order is up->down->left->right) */
      name_lut[i] = vrow + (vcol * (height + 1));
    }
  }
}


/*--------------------------------------------------------------------------*/
/* Bitplane to packed pixel look-up table function (Mode 4)                 */
/*--------------------------------------------------------------------------*/

static void make_bp_lut(void)
{
  int x,i,j;
  uint32 out;

  /* ---------------------- */
  /* Pattern color encoding */
  /* -------------------------------------------------------------------------*/
  /* 4 byteplanes are required to define one pattern line (8 pixels)          */
  /* A single pixel color is coded with 4 bits (c3 c2 c1 c0)                  */
  /* Each bit is coming from byteplane bits, as explained below:              */
  /* pixel 0: c3 = bp3 bit 7, c2 = bp2 bit 7, c1 = bp1 bit 7, c0 = bp0 bit 7  */
  /* pixel 1: c3 = bp3 bit 6, c2 = bp2 bit 6, c1 = bp1 bit 6, c0 = bp0 bit 6  */
  /* ...                                                                      */
  /* pixel 7: c3 = bp3 bit 0, c2 = bp2 bit 0, c1 = bp1 bit 0, c0 = bp0 bit 0  */
  /* -------------------------------------------------------------------------*/

  for(i = 0; i < 0x100; i++)
  for(j = 0; j < 0x100; j++)
  {
    out = 0;
    for(x = 0; x < 8; x++)
    {
      /* pixel line data = hh00gg00ff00ee00dd00cc00bb00aa00 (32-bit) */
      /* aa-hh = upper or lower 2-bit values of pixels 0-7 (shifted) */
      out |= (j & (0x80 >> x)) ? (uint32)(8 << (x << 2)) : 0;
      out |= (i & (0x80 >> x)) ? (uint32)(4 << (x << 2)) : 0;
    }

    /* i = low byte in VRAM  (bp0 or bp2) */
    /* j = high byte in VRAM (bp1 or bp3) */
 #ifdef LSB_FIRST
    bp_lut[(j << 8) | (i)] = out;
 #else
    bp_lut[(i << 8) | (j)] = out;
 #endif
   }
}


/*--------------------------------------------------------------------------*/
/* Layers priority pixel look-up tables functions                           */
/*--------------------------------------------------------------------------*/

/* Input (bx):  d5-d0=color, d6=priority, d7=unused */
/* Input (ax):  d5-d0=color, d6=priority, d7=unused */
/* Output:    d5-d0=color, d6=priority, d7=zero */
static uint32 make_lut_bg(uint32 bx, uint32 ax)
{
  int bf = (bx & 0x7F);
  int bp = (bx & 0x40);
  int b  = (bx & 0x0F);

  int af = (ax & 0x7F);
  int ap = (ax & 0x40);
  int a  = (ax & 0x0F);

  int c = (ap ? (a ? af : bf) : (bp ? (b ? bf : af) : (a ? af : bf)));

  /* Strip palette & priority bits from transparent pixels */
  if((c & 0x0F) == 0x00) c &= 0x80;

  return (c);
}

/* Input (bx):  d5-d0=color, d6=priority, d7=unused */
/* Input (sx):  d5-d0=color, d6=priority, d7=unused */
/* Output:    d5-d0=color, d6=priority, d7=intensity select (0=half/1=normal) */
static uint32 make_lut_bg_ste(uint32 bx, uint32 ax)
{
  int bf = (bx & 0x7F);
  int bp = (bx & 0x40);
  int b  = (bx & 0x0F);

  int af = (ax & 0x7F);
  int ap = (ax & 0x40);
  int a  = (ax & 0x0F);

  int c = (ap ? (a ? af : bf) : (bp ? (b ? bf : af) : (a ? af : bf)));

  /* Half intensity when both pixels are low priority */
  c |= ((ap | bp) << 1);

  /* Strip palette & priority bits from transparent pixels */
  if((c & 0x0F) == 0x00) c &= 0x80;

  return (c);
}

/* Input (bx):  d5-d0=color, d6=priority/1, d7=sprite pixel marker */
/* Input (sx):  d5-d0=color, d6=priority, d7=unused */
/* Output:    d5-d0=color, d6=priority, d7=sprite pixel marker */
static uint32 make_lut_obj(uint32 bx, uint32 sx)
{
  int c;

  int bf = (bx & 0x7F);
  int bs = (bx & 0x80);
  int sf = (sx & 0x7F);

  if((sx & 0x0F) == 0) return bx;

  c = (bs ? bf : sf);

  /* Strip palette bits from transparent pixels */
  if((c & 0x0F) == 0x00) c &= 0xC0;

  return (c | 0x80);
}


/* Input (bx):  d5-d0=color, d6=priority, d7=opaque sprite pixel marker */
/* Input (sx):  d5-d0=color, d6=priority, d7=unused */
/* Output:    d5-d0=color, d6=zero/priority, d7=opaque sprite pixel marker */
static uint32 make_lut_bgobj(uint32 bx, uint32 sx)
{
  int c;

  int bf = (bx & 0x3F);
  int bs = (bx & 0x80);
  int bp = (bx & 0x40);
  int b  = (bx & 0x0F);

  int sf = (sx & 0x3F);
  int sp = (sx & 0x40);
  int s  = (sx & 0x0F);

  if(s == 0) return bx;

  /* Previous sprite has higher priority */
  if(bs) return bx;

  c = (sp ? sf : (bp ? (b ? bf : sf) : sf));

  /* Strip palette & priority bits from transparent pixels */
  if((c & 0x0F) == 0x00) c &= 0x80;

  return (c | 0x80);
}

/* Input (bx):  d5-d0=color, d6=priority, d7=intensity (half/normal) */
/* Input (sx):  d5-d0=color, d6=priority, d7=sprite marker */
/* Output:    d5-d0=color, d6=intensity (half/normal), d7=(double/invalid) */
static uint32 make_lut_bgobj_ste(uint32 bx, uint32 sx)
{
  int c;

  int bf = (bx & 0x3F);
  int bp = (bx & 0x40);
  int b  = (bx & 0x0F);
  int bi = (bx & 0x80) >> 1;

  int sf = (sx & 0x3F);
  int sp = (sx & 0x40);
  int s  = (sx & 0x0F);
  int si = sp | bi;

  if(sp)
  {
    if(s)
    {
      if((sf & 0x3E) == 0x3E)
      {
        if(sf & 1)
        {
          c = (bf | 0x00);
        }
        else
        {
          c = (bx & 0x80) ? (bf | 0x80) : (bf | 0x40);
        }
      }
      else
      {
        if(sf == 0x0E || sf == 0x1E || sf == 0x2E)
        {
          c = (sf | 0x40);
        }
        else
        {
          c = (sf | si);
        }
      }
    }
    else
    {
      c = (bf | bi);
    }
  }
  else
  {
    if(bp)
    {
      if(b)
      {
        c = (bf | bi);
      }
      else
      {
        if(s)
        {
          if((sf & 0x3E) == 0x3E)
          {
            if(sf & 1)
            {
              c = (bf | 0x00);
            }
            else
            {
              c = (bx & 0x80) ? (bf | 0x80) : (bf | 0x40);
            }
          }
          else
          {
            if(sf == 0x0E || sf == 0x1E || sf == 0x2E)
            {
              c = (sf | 0x40);
            }
            else
            {
              c = (sf | si);
            }
          }
        }
        else
        {
          c = (bf | bi);
        }
      }
    }
    else
    {
      if(s)
      {
        if((sf & 0x3E) == 0x3E)
        {
          if(sf & 1)
          {
            c = (bf | 0x00);
          }
          else
          {
            c = (bx & 0x80) ? (bf | 0x80) : (bf | 0x40);
          }
        }
        else
        {
          if(sf == 0x0E || sf == 0x1E || sf == 0x2E)
          {
            c = (sf | 0x40);
          }
          else
          {
            c = (sf | si);
          }
        }
      }
      else
      {
        c = (bf | bi);
      }
    }
  }

  if((c & 0x0f) == 0x00) c &= 0xC0;

  return (c);
}

/* Input (bx):  d3-d0=color, d4=palette, d5=priority, d6=zero, d7=sprite pixel marker */
/* Input (sx):  d3-d0=color, d7-d4=zero */
/* Output:      d3-d0=color, d4=palette, d5=zero/priority, d6=zero, d7=sprite pixel marker */
static uint32 make_lut_bgobj_m4(uint32 bx, uint32 sx)
{
  int c;

  int bf = (bx & 0x3F);
  int bs = (bx & 0x80);
  int bp = (bx & 0x20);
  int b  = (bx & 0x0F);

  int s  = (sx & 0x0F);
  int sf = (s | 0x10); /* force palette bit */

  /* Transparent sprite pixel */
  if(s == 0) return bx;

  /* Previous sprite has higher priority */
  if(bs) return bx;

  /* note: priority bit is always 0 for Modes 0,1,2,3 */
  c = (bp ? (b ? bf : sf) : sf);

  return (c | 0x80);
}


/*--------------------------------------------------------------------------*/
/* Pixel layer merging function                                             */
/*--------------------------------------------------------------------------*/

INLINE void merge(uint8 *srca, uint8 *srcb, uint8 *dst, uint8 *table, int width)
{
#ifdef __ARM_NEON
  if (table == lut[0] || table == lut[2])
  {
    const int ste = (table == lut[2]);
    while (width >= 16)
    {
      uint8x16_t ax = vld1q_u8(srca);
      uint8x16_t bx = vld1q_u8(srcb);
      vst1q_u8(dst, merge_bg_16_neon(ax, bx, ste));
      srca += 16; srcb += 16; dst += 16;
      width -= 16;
    }
    while (width >= 8)
    {
      uint8x8_t ax = vld1_u8(srca);
      uint8x8_t bx = vld1_u8(srcb);
      vst1_u8(dst, merge_bg_8_neon(ax, bx, ste));
      srca += 8; srcb += 8; dst += 8;
      width -= 8;
    }
    while (width > 0)
    {
      *dst++ = table[(*srcb++ << 8) | (*srca++)];
      width--;
    }
    return;
  }
#endif
  do
  {
    *dst++ = table[(*srcb++ << 8) | (*srca++)];
  }
  while (--width);
}


/*--------------------------------------------------------------------------*/
/* Pixel color lookup tables initialization                                 */
/*--------------------------------------------------------------------------*/

static void palette_init(void)
{
  int r, g, b, i;

  /************************************************/
  /* Each R,G,B color channel is 4-bit with a     */
  /* total of 15 different intensity levels.      */
  /*                                              */
  /* Color intensity depends on the mode:         */
  /*                                              */
  /*    normal   : xxx0     (0-14)                */
  /*    shadow   : 0xxx     (0-7)                 */
  /*    highlight: 1xxx - 1 (7-14)                */
  /*    mode4    : xxxx(*)  (0-15)                */
  /*    GG mode  : xxxx     (0-15)                */
  /*                                              */
  /* with x = original CRAM value (2, 3 or 4-bit) */
  /*  (*) 2-bit CRAM value is expanded to 4-bit   */
  /************************************************/

  /* Initialize Mode 5 pixel color look-up tables */
  for (i = 0; i < 0x200; i++)
  {
    /* CRAM 9-bit value (BBBGGGRRR) */
    r = (i >> 0) & 7;
    g = (i >> 3) & 7;
    b = (i >> 6) & 7;

    /* Convert to output pixel format */
    pixel_lut[0][i] = MAKE_PIXEL(r,g,b);
    pixel_lut[1][i] = MAKE_PIXEL(r<<1,g<<1,b<<1);
    pixel_lut[2][i] = MAKE_PIXEL(r+7,g+7,b+7);
  }

  /* Initialize Mode 4 pixel color look-up table */
  for (i = 0; i < 0x40; i++)
  {
    /* CRAM 6-bit value (000BBGGRR) */
    r = (i >> 0) & 3;
    g = (i >> 2) & 3;
    b = (i >> 4) & 3;

    /* Expand to full range & convert to output pixel format */
    pixel_lut_m4[i] = MAKE_PIXEL((r << 2) | r, (g << 2) | g, (b << 2) | b);
  }
}


/*--------------------------------------------------------------------------*/
/* Color palette update functions                                           */
/*--------------------------------------------------------------------------*/

void color_update_m4(int index, unsigned int data)
{
  switch (system_hw)
  {
    case SYSTEM_GG:
    {
      /* CRAM value (BBBBGGGGRRRR) */
      int r = (data >> 0) & 0x0F;
      int g = (data >> 4) & 0x0F;
      int b = (data >> 8) & 0x0F;

      /* Convert to output pixel */
      data = MAKE_PIXEL(r,g,b);
      break;
    }

    case SYSTEM_SG:
    case SYSTEM_SGII:
    case SYSTEM_SGII_RAM_EXT:
    {
      /* Fixed TMS99xx palette */
      if (index & 0x0F)
      {
        /* Colors 1-15 */
        data = tms_palette[index & 0x0F];
      }
      else
      {
        /* Backdrop color */
        data = tms_palette[reg[7] & 0x0F];
      }
      break;
    }

    default:
    {
      /* Test M4 bit */
      if (!(reg[0] & 0x04))
      {
        if (system_hw & SYSTEM_MD)
        {
          /* Invalid Mode (black screen) */
          data = 0x00;
        }
        else if (system_hw != SYSTEM_GGMS)
        {
          /* Fixed CRAM palette */
          if (index & 0x0F)
          {
            /* Colors 1-15 */
            data = tms_crom[index & 0x0F];
          }
          else
          {
            /* Backdrop color */
            data = tms_crom[reg[7] & 0x0F];
          }
        }
      }

      /* Mode 4 palette */
      data = pixel_lut_m4[data & 0x3F];
      break;
    }
  }


  /* Input pixel: x0xiiiii (normal) or 01000000 (backdrop) */
  if (reg[0] & 0x04)
  {
    /* Mode 4 */
    pixel[0x00 | index] = data;
    pixel[0x20 | index] = data;
    pixel[0x80 | index] = data;
    pixel[0xA0 | index] = data;
  }
  else
  {
    /* TMS99xx modes (palette bit forced to 1 because Game Gear uses CRAM palette #1) */
    if ((index == 0x40) || (index == (0x10 | (reg[7] & 0x0F))))
    {
      /* Update backdrop color */
      pixel[0x40] = data;

      /* Update transparent color */
      pixel[0x10] = data;
      pixel[0x30] = data;
      pixel[0x90] = data;
      pixel[0xB0] = data;
    }

    if (index & 0x0F)
    {
      /* update non-transparent colors */
      pixel[0x00 | index] = data;
      pixel[0x20 | index] = data;
      pixel[0x80 | index] = data;
      pixel[0xA0 | index] = data;
    }
  }
}

void color_update_m5(int index, unsigned int data)
{
  /* Palette Mode */
  if (!(reg[0] & 0x04))
  {
    /* Color value is limited to 00X00X00X */
    data &= 0x49;
  }

  if(reg[12] & 0x08)
  {
    /* Mode 5 (Shadow/Normal/Highlight) */
    pixel[0x00 | index] = pixel_lut[0][data];
    pixel[0x40 | index] = pixel_lut[1][data];
    pixel[0x80 | index] = pixel_lut[2][data];
  }
  else
  {
    /* Mode 5 (Normal) */
    data = pixel_lut[1][data];

    /* Input pixel: xxiiiiii */
    pixel[0x00 | index] = data;
    pixel[0x40 | index] = data;
    pixel[0x80 | index] = data;
  }
}


/*--------------------------------------------------------------------------*/
/* Background layers rendering functions                                    */
/*--------------------------------------------------------------------------*/

/* Graphics I */
void render_bg_m0(int line)
{
  uint8 color, name, pattern;

  uint8 *lb = &linebuf[0][0x20];
  uint8 *nt = &vram[((reg[2] << 10) & 0x3C00) + ((line & 0xF8) << 2)];
  uint8 *ct = &vram[((reg[3] <<  6) & 0x3FC0)];
  uint8 *pg = &vram[((reg[4] << 11) & 0x3800) + (line & 7)];

  /* 32 x 8 pixels */
  int width = 32;

  do
  {
    name = *nt++;
    color = ct[name >> 3];
    pattern = pg[name << 3];

    *lb++ = 0x10 | ((color >> (((pattern >> 7) & 1) << 2)) & 0x0F);
    *lb++ = 0x10 | ((color >> (((pattern >> 6) & 1) << 2)) & 0x0F);
    *lb++ = 0x10 | ((color >> (((pattern >> 5) & 1) << 2)) & 0x0F);
    *lb++ = 0x10 | ((color >> (((pattern >> 4) & 1) << 2)) & 0x0F);
    *lb++ = 0x10 | ((color >> (((pattern >> 3) & 1) << 2)) & 0x0F);
    *lb++ = 0x10 | ((color >> (((pattern >> 2) & 1) << 2)) & 0x0F);
    *lb++ = 0x10 | ((color >> (((pattern >> 1) & 1) << 2)) & 0x0F);
    *lb++ = 0x10 | ((color >> (((pattern >> 0) & 1) << 2)) & 0x0F);
  }
  while (--width);
}

/* Text */
void render_bg_m1(int line)
{
  uint8 pattern;
  uint8 color = reg[7];

  uint8 *lb = &linebuf[0][0x20];
  uint8 *nt = &vram[((reg[2] << 10) & 0x3C00) + ((line >> 3) * 40)];
  uint8 *pg = &vram[((reg[4] << 11) & 0x3800) + (line & 7)];

  /* 40 x 6 pixels */
  int width = 40;

  /* Left border (8 pixels) */
  memset (lb, 0x40, 8);
  lb += 8;

  do
  {
    pattern = pg[*nt++ << 3];

    *lb++ = 0x10 | ((color >> (((pattern >> 7) & 1) << 2)) & 0x0F);
    *lb++ = 0x10 | ((color >> (((pattern >> 6) & 1) << 2)) & 0x0F);
    *lb++ = 0x10 | ((color >> (((pattern >> 5) & 1) << 2)) & 0x0F);
    *lb++ = 0x10 | ((color >> (((pattern >> 4) & 1) << 2)) & 0x0F);
    *lb++ = 0x10 | ((color >> (((pattern >> 3) & 1) << 2)) & 0x0F);
    *lb++ = 0x10 | ((color >> (((pattern >> 2) & 1) << 2)) & 0x0F);
  }
  while (--width);

  /* Right borders (8 pixels) */
  memset(lb, 0x40, 8);
}

/* Text + extended PG */
void render_bg_m1x(int line)
{
  uint8 pattern;
  uint8 *pg;

  uint8 color = reg[7];

  uint8 *lb = &linebuf[0][0x20];
  uint8 *nt = &vram[((reg[2] << 10) & 0x3C00) + ((line >> 3) * 40)];

  uint16 pg_mask = ~0x3800 ^ (reg[4] << 11);

  /* 40 x 6 pixels */
  int width = 40;

  /* Unused bits used as a mask on TMS99xx & 315-5124 VDP only */
  if (system_hw > SYSTEM_SMS)
  {
    pg_mask |= 0x1800;
  }

  pg = &vram[((0x2000 + ((line & 0xC0) << 5)) & pg_mask) + (line & 7)];

  /* Left border (8 pixels) */
  memset (lb, 0x40, 8);
  lb += 8;

  do
  {
    pattern = pg[*nt++ << 3];

    *lb++ = 0x10 | ((color >> (((pattern >> 7) & 1) << 2)) & 0x0F);
    *lb++ = 0x10 | ((color >> (((pattern >> 6) & 1) << 2)) & 0x0F);
    *lb++ = 0x10 | ((color >> (((pattern >> 5) & 1) << 2)) & 0x0F);
    *lb++ = 0x10 | ((color >> (((pattern >> 4) & 1) << 2)) & 0x0F);
    *lb++ = 0x10 | ((color >> (((pattern >> 3) & 1) << 2)) & 0x0F);
    *lb++ = 0x10 | ((color >> (((pattern >> 2) & 1) << 2)) & 0x0F);
  }
  while (--width);

  /* Right borders (8 pixels) */
  memset(lb, 0x40, 8);
}

/* Graphics II */
void render_bg_m2(int line)
{
  uint8 color, pattern;
  uint16 name;
  uint8 *ct, *pg;

  uint8 *lb = &linebuf[0][0x20];
  uint8 *nt = &vram[((reg[2] << 10) & 0x3C00) + ((line & 0xF8) << 2)];

  uint16 ct_mask = ~0x3FC0 ^ (reg[3] << 6);
  uint16 pg_mask = ~0x3800 ^ (reg[4] << 11);

  /* 32 x 8 pixels */
  int width = 32;

  /* Unused bits used as a mask on TMS99xx & 315-5124 VDP only */
  if (system_hw > SYSTEM_SMS)
  {
    ct_mask |= 0x1FC0;
    pg_mask |= 0x1800;
  }

  ct = &vram[((0x2000 + ((line & 0xC0) << 5)) & ct_mask) + (line & 7)];
  pg = &vram[((0x2000 + ((line & 0xC0) << 5)) & pg_mask) + (line & 7)];

  do
  {
    name = *nt++ << 3 ;
    color = ct[name & ct_mask];
    pattern = pg[name];

    *lb++ = 0x10 | ((color >> (((pattern >> 7) & 1) << 2)) & 0x0F);
    *lb++ = 0x10 | ((color >> (((pattern >> 6) & 1) << 2)) & 0x0F);
    *lb++ = 0x10 | ((color >> (((pattern >> 5) & 1) << 2)) & 0x0F);
    *lb++ = 0x10 | ((color >> (((pattern >> 4) & 1) << 2)) & 0x0F);
    *lb++ = 0x10 | ((color >> (((pattern >> 3) & 1) << 2)) & 0x0F);
    *lb++ = 0x10 | ((color >> (((pattern >> 2) & 1) << 2)) & 0x0F);
    *lb++ = 0x10 | ((color >> (((pattern >> 1) & 1) << 2)) & 0x0F);
    *lb++ = 0x10 | ((color >> (((pattern >> 0) & 1) << 2)) & 0x0F);
  }
  while (--width);
}

/* Multicolor */
void render_bg_m3(int line)
{
  uint8 color;
  uint8 *lb = &linebuf[0][0x20];
  uint8 *nt = &vram[((reg[2] << 10) & 0x3C00) + ((line & 0xF8) << 2)];
  uint8 *pg = &vram[((reg[4] << 11) & 0x3800) + ((line >> 2) & 7)];

  /* 32 x 8 pixels */
  int width = 32;

  do
  {
    color = pg[*nt++ << 3];

    *lb++ = 0x10 | ((color >> 4) & 0x0F);
    *lb++ = 0x10 | ((color >> 4) & 0x0F);
    *lb++ = 0x10 | ((color >> 4) & 0x0F);
    *lb++ = 0x10 | ((color >> 4) & 0x0F);
    *lb++ = 0x10 | ((color >> 0) & 0x0F);
    *lb++ = 0x10 | ((color >> 0) & 0x0F);
    *lb++ = 0x10 | ((color >> 0) & 0x0F);
    *lb++ = 0x10 | ((color >> 0) & 0x0F);
  }
  while (--width);
}

/* Multicolor + extended PG */
void render_bg_m3x(int line)
{
  uint8 color;
  uint8 *pg;

  uint8 *lb = &linebuf[0][0x20];
  uint8 *nt = &vram[((reg[2] << 10) & 0x3C00) + ((line & 0xF8) << 2)];

  uint16 pg_mask = ~0x3800 ^ (reg[4] << 11);

  /* 32 x 8 pixels */
  int width = 32;

  /* Unused bits used as a mask on TMS99xx & 315-5124 VDP only */
  if (system_hw > SYSTEM_SMS)
  {
    pg_mask |= 0x1800;
  }

  pg = &vram[((0x2000 + ((line & 0xC0) << 5)) & pg_mask) + ((line >> 2) & 7)];

  do
  {
    color = pg[*nt++ << 3];

    *lb++ = 0x10 | ((color >> 4) & 0x0F);
    *lb++ = 0x10 | ((color >> 4) & 0x0F);
    *lb++ = 0x10 | ((color >> 4) & 0x0F);
    *lb++ = 0x10 | ((color >> 4) & 0x0F);
    *lb++ = 0x10 | ((color >> 0) & 0x0F);
    *lb++ = 0x10 | ((color >> 0) & 0x0F);
    *lb++ = 0x10 | ((color >> 0) & 0x0F);
    *lb++ = 0x10 | ((color >> 0) & 0x0F);
  }
  while (--width);
}

/* Invalid (2+3/1+2+3) */
void render_bg_inv(int line)
{
  uint8 color = reg[7];

  uint8 *lb = &linebuf[0][0x20];

  /* 40 x 6 pixels */
  int width = 40;

  /* Left border (8 pixels) */
  memset (lb, 0x40, 8);
  lb += 8;

  do
  {
    *lb++ = 0x10 | ((color >> 4) & 0x0F);
    *lb++ = 0x10 | ((color >> 4) & 0x0F);
    *lb++ = 0x10 | ((color >> 4) & 0x0F);
    *lb++ = 0x10 | ((color >> 4) & 0x0F);
    *lb++ = 0x10 | ((color >> 0) & 0x0F);
    *lb++ = 0x10 | ((color >> 0) & 0x0F);
  }
  while (--width);

  /* Right borders (8 pixels) */
  memset(lb, 0x40, 8);
}

/* Mode 4 */
void render_bg_m4(int line)
{
  int column;
  uint16 *nt;
  uint32 attr, atex;

  /* Horizontal scrolling */
  int index = ((reg[0] & 0x40) && (line < 0x10)) ? 0x100 : reg[0x08];
  int shift = index & 7;

  /* Background line buffer */
  uint8 *dst = &linebuf[0][0x20 + shift];

  /* Vertical scrolling */
  int v_line = line + vscroll;

  /* Pattern name table mask */
  uint16 nt_mask = ~0x3C00 ^ (reg[2] << 10);

  /* Unused bits used as a mask on TMS99xx & 315-5124 VDP only */
  if (system_hw > SYSTEM_SMS)
  {
    nt_mask |= 0x400;
  }

  /* Check extended height modes (Master System II & Game Gear VDP only) */
  if (bitmap.viewport.h > 192)
  {
    /* Vertical scroll mask */
    v_line = v_line % 256;

    /* Pattern name Table */
    nt = (uint16 *)&vram[(0x3700 & nt_mask) + ((v_line >> 3) << 6)];
  }
  else
  {
    /* Vertical scroll mask */
    v_line = v_line % 224;

    /* Pattern name Table */
    nt = (uint16 *)&vram[(0x3800 + ((v_line >> 3) << 6)) & nt_mask];
  }

  /* Pattern row index */
  v_line = (v_line & 7) << 3;

  /* Tile column index */
  index = (0x100 - index) >> 3;

  /* Clip left-most column if required */
  if (shift)
  {
    memset(&linebuf[0][0x20], 0, shift);
    index++;
  }

  /* Draw tiles (32 x 8 pixels) */
  for(column = 0; column < 32; column++, index++)
  {
    /* Stop vertical scrolling for rightmost eight tiles */
    if((column == 24) && (reg[0] & 0x80))
    {
      /* Clear Pattern name table start address */
      if (bitmap.viewport.h > 192)
      {
        nt = (uint16 *)&vram[(0x3700 & nt_mask) + ((line >> 3) << 6)];
      }
      else
      {
        nt = (uint16 *)&vram[(0x3800 + ((line >> 3) << 6)) & nt_mask];
      }

      /* Clear Pattern row index */
      v_line = (line & 7) << 3;
    }

    /* Read name table attribute word */
    attr = nt[index & 0x1F];
#ifndef LSB_FIRST
    attr = (((attr & 0xFF) << 8) | ((attr & 0xFF00) >> 8));
#endif

    /* Expand priority and palette bits */
    atex = atex_table[(attr >> 11) & 3];

    /* On 315-5124 VDP only, Color Table Base Address (resp. Pattern Generator Table Base Address) register bits 7:0 (resp. bits 2:0) */
    /* are used as a mask on tile index upper bits when fetching bitplanes 0&1 (resp. bitplanes 2&3), which correspond to tile pixels */
    /* data bits 0:1 (resp. bits 2:3) */
    if (system_hw <= SYSTEM_SMS)
    {
      /* Cached pattern data lines (4 bytes = 4 pixels at once) for pixels data bits 0:1 and 2:3 */
      uint32 *src01 = (uint32 *)&bg_pattern_cache[((attr & (0x601 | (reg[3] << 1))) << 6) | v_line];
      uint32 *src23 = (uint32 *)&bg_pattern_cache[((attr & (0x63F | ((reg[4] & 0x07) << 6))) << 6) | v_line];

      /* Copy left & right half, retrieving each pixel data bits from appropriate source and adding the attribute bits in */
      WRITE_LONG(dst, (src01[0] & 0x03030303) | (src23[0] & 0x0C0C0C0C) | atex);
      WRITE_LONG(dst + 4, (src01[1] & 0x03030303) | (src23[1] & 0x0C0C0C0C) | atex);
      dst += 8;
    }
    else
    {
      /* Cached pattern data line (4 bytes = 4 pixels at once) */
      uint32 *src = (uint32 *)&bg_pattern_cache[((attr & 0x7FF) << 6) | v_line];

      /* Copy left & right half, adding the attribute bits in */
      WRITE_LONG(dst, src[0] | atex);
      WRITE_LONG(dst + 4, src[1] | atex);
      dst += 8;
    }
  }
}

/* Mode 5 */
#ifndef ALT_RENDERER
void render_bg_m5(int line)
{
  int column;
  uint32 atex, atbuf, *src;
  uint8 *dst;

  /* Common data */
  uint32 xscroll      = *(uint32 *)&vram[hscb + ((line & hscroll_mask) << 2)];
  uint32 yscroll      = *(uint32 *)&vsram[0];
  uint32 pf_col_mask  = playfield_col_mask;
  uint32 pf_row_mask  = playfield_row_mask;
  uint32 pf_shift     = playfield_shift;

  /* Window & Plane A */
  int a = (reg[18] & 0x1F) << 3;
  int w = (reg[18] >> 7) & 1;

  /* Plane B width */
  int start = 0;
  int end = bitmap.viewport.w >> 4;

  /* Plane B scroll */
#ifdef LSB_FIRST
  uint32 shift  = (xscroll >> 16) & 0x0F;
  uint32 index  = pf_col_mask + 1 - ((xscroll >> 20) & pf_col_mask);
  uint32 v_line = (line + (yscroll >> 16)) & pf_row_mask;
#else
  uint32 shift  = (xscroll & 0x0F);
  uint32 index  = pf_col_mask + 1 - ((xscroll >> 4) & pf_col_mask);
  uint32 v_line = (line + yscroll) & pf_row_mask;
#endif

  /* Plane B name table */
  uint32 *nt = (uint32 *)&vram[ntbb + (((v_line >> 3) << pf_shift) & 0x1FC0)];

  /* Pattern row index */
  v_line = (v_line & 7) << 3;

  if(shift)
  {
    /* Plane B line buffer */
    dst = &linebuf[0][0x10 + shift];

    atbuf = nt[(index - 1) & pf_col_mask];
    DRAW_COLUMN(atbuf, v_line)
  }
  else
  {
    /* Plane B line buffer */
    dst = &linebuf[0][0x20];
  }

  for(column = 0; column < end; column++, index++)
  {
    atbuf = nt[index & pf_col_mask];
    DRAW_COLUMN(atbuf, v_line)
  }

  if (w == (line >= a))
  {
    /* Window takes up entire line */
    a = 0;
    w = 1;
  }
  else
  {
    /* Window and Plane A share the line */
    a = clip[0].enable;
    w = clip[1].enable;
  }

  /* Plane A */
  if (a)
  {
    /* Plane A width */
    start = clip[0].left;
    end   = clip[0].right;

    /* Plane A scroll */
#ifdef LSB_FIRST
    shift   = (xscroll & 0x0F);
    index   = pf_col_mask + start + 1 - ((xscroll >> 4) & pf_col_mask);
    v_line  = (line + yscroll) & pf_row_mask;
#else
    shift   = (xscroll >> 16) & 0x0F;
    index   = pf_col_mask + start + 1 - ((xscroll >> 20) & pf_col_mask);
    v_line  = (line + (yscroll >> 16)) & pf_row_mask;
#endif

    /* Plane A name table */
    nt = (uint32 *)&vram[ntab + (((v_line >> 3) << pf_shift) & 0x1FC0)];

    /* Pattern row index */
    v_line = (v_line & 7) << 3;

    if(shift)
    {
      /* Plane A line buffer */
      dst = &linebuf[1][0x10 + shift + (start << 4)];

      /* Window bug */
      if (start)
      {
        atbuf = nt[index & pf_col_mask];
      }
      else
      {
        atbuf = nt[(index - 1) & pf_col_mask];
      }

      DRAW_COLUMN(atbuf, v_line)
    }
    else
    {
      /* Plane A line buffer */
      dst = &linebuf[1][0x20 + (start << 4)];
    }

    for(column = start; column < end; column++, index++)
    {
      atbuf = nt[index & pf_col_mask];
      DRAW_COLUMN(atbuf, v_line)
    }

    /* Window width */
    start = clip[1].left;
    end   = clip[1].right;
  }

  /* Window */
  if (w)
  {
    /* Window name table */
    nt = (uint32 *)&vram[ntwb | ((line >> 3) << (6 + (reg[12] & 1)))];

    /* Pattern row index */
    v_line = (line & 7) << 3;

    /* Plane A line buffer */
    dst = &linebuf[1][0x20 + (start << 4)];

    for(column = start; column < end; column++)
    {
      atbuf = nt[column];
      DRAW_COLUMN(atbuf, v_line)
    }
  }

  /* Merge background layers */
  merge(&linebuf[1][0x20], &linebuf[0][0x20], &linebuf[0][0x20], lut[(reg[12] & 0x08) >> 2], bitmap.viewport.w);
}

void render_bg_m5_vs(int line)
{
  int column;
  uint32 atex, atbuf, *src;
  uint8 *dst;
  uint32 v_line, *nt;

  /* Common data */
  uint32 xscroll      = *(uint32 *)&vram[hscb + ((line & hscroll_mask) << 2)];
  uint32 yscroll      = 0;
  uint32 pf_col_mask  = playfield_col_mask;
  uint32 pf_row_mask  = playfield_row_mask;
  uint32 pf_shift     = playfield_shift;
  uint32 *vs          = (uint32 *)&vsram[0];

  /* Window & Plane A */
  int a = (reg[18] & 0x1F) << 3;
  int w = (reg[18] >> 7) & 1;

  /* Plane B width */
  int start = 0;
  int end = bitmap.viewport.w >> 4;

  /* Plane B horizontal scroll */
#ifdef LSB_FIRST
  uint32 shift  = (xscroll >> 16) & 0x0F;
  uint32 index  = pf_col_mask + 1 - ((xscroll >> 20) & pf_col_mask);
#else
  uint32 shift  = (xscroll & 0x0F);
  uint32 index  = pf_col_mask + 1 - ((xscroll >> 4) & pf_col_mask);
#endif

  /* Left-most column vertical scrolling when partially shown horizontally (verified on PAL MD2)  */
  /* TODO: check on Genesis 3 models since it apparently behaves differently  */
  /* In H32 mode, vertical scrolling is disabled, in H40 mode, same value is used for both planes */
  /* See Formula One / Kawasaki Superbike Challenge (H32) & Gynoug / Cutie Suzuki no Ringside Angel (H40) */
  if (reg[12] & 1)
  {
    yscroll = vs[19] & (vs[19] >> 16);
  }

  if(shift)
  {
    /* Plane B vertical scroll */
    v_line = (line + yscroll) & pf_row_mask;

    /* Plane B name table */
    nt = (uint32 *)&vram[ntbb + (((v_line >> 3) << pf_shift) & 0x1FC0)];

    /* Pattern row index */
    v_line = (v_line & 7) << 3;

    /* Plane B line buffer */
    dst = &linebuf[0][0x10 + shift];

    atbuf = nt[(index - 1) & pf_col_mask];
    DRAW_COLUMN(atbuf, v_line)
  }
  else
  {
    /* Plane B line buffer */
    dst = &linebuf[0][0x20];
  }

  for(column = 0; column < end; column++, index++)
  {
    /* Plane B vertical scroll */
#ifdef LSB_FIRST
    v_line = (line + (vs[column] >> 16)) & pf_row_mask;
#else
    v_line = (line + vs[column]) & pf_row_mask;
#endif

    /* Plane B name table */
    nt = (uint32 *)&vram[ntbb + (((v_line >> 3) << pf_shift) & 0x1FC0)];

    /* Pattern row index */
    v_line = (v_line & 7) << 3;

    atbuf = nt[index & pf_col_mask];
    DRAW_COLUMN(atbuf, v_line)
  }

  if (w == (line >= a))
  {
    /* Window takes up entire line */
    a = 0;
    w = 1;
  }
  else
  {
    /* Window and Plane A share the line */
    a = clip[0].enable;
    w = clip[1].enable;
  }

  /* Plane A */
  if (a)
  {
    /* Plane A width */
    start = clip[0].left;
    end   = clip[0].right;

    /* Plane A horizontal scroll */
#ifdef LSB_FIRST
    shift = (xscroll & 0x0F);
    index = pf_col_mask + start + 1 - ((xscroll >> 4) & pf_col_mask);
#else
    shift = (xscroll >> 16) & 0x0F;
    index = pf_col_mask + start + 1 - ((xscroll >> 20) & pf_col_mask);
#endif

    if(shift)
    {
      /* Plane A vertical scroll */
      v_line = (line + yscroll) & pf_row_mask;

      /* Plane A name table */
      nt = (uint32 *)&vram[ntab + (((v_line >> 3) << pf_shift) & 0x1FC0)];

      /* Pattern row index */
      v_line = (v_line & 7) << 3;

      /* Plane A line buffer */
      dst = &linebuf[1][0x10 + shift + (start << 4)];

      /* Window bug */
      if (start)
      {
        atbuf = nt[index & pf_col_mask];
      }
      else
      {
        atbuf = nt[(index - 1) & pf_col_mask];
      }

      DRAW_COLUMN(atbuf, v_line)
    }
    else
    {
      /* Plane A line buffer */
      dst = &linebuf[1][0x20 + (start << 4)];
    }

    for(column = start; column < end; column++, index++)
    {
      /* Plane A vertical scroll */
#ifdef LSB_FIRST
      v_line = (line + vs[column]) & pf_row_mask;
#else
      v_line = (line + (vs[column] >> 16)) & pf_row_mask;
#endif

      /* Plane A name table */
      nt = (uint32 *)&vram[ntab + (((v_line >> 3) << pf_shift) & 0x1FC0)];

      /* Pattern row index */
      v_line = (v_line & 7) << 3;

      atbuf = nt[index & pf_col_mask];
      DRAW_COLUMN(atbuf, v_line)
    }

    /* Window width */
    start = clip[1].left;
    end   = clip[1].right;
  }

  /* Window */
  if (w)
  {
    /* Window name table */
    nt = (uint32 *)&vram[ntwb | ((line >> 3) << (6 + (reg[12] & 1)))];

    /* Pattern row index */
    v_line = (line & 7) << 3;

    /* Plane A line buffer */
    dst = &linebuf[1][0x20 + (start << 4)];

    for(column = start; column < end; column++)
    {
      atbuf = nt[column];
      DRAW_COLUMN(atbuf, v_line)
    }
  }

  /* Merge background layers */
  merge(&linebuf[1][0x20], &linebuf[0][0x20], &linebuf[0][0x20], lut[(reg[12] & 0x08) >> 2], bitmap.viewport.w);
}

/* Enhanced function that allows each cell to be vscrolled individually, instead of being limited to 2-cell */
void render_bg_m5_vs_enhanced(int line)
{
  int column;
  uint32 atex, atbuf, *src;
  uint8 *dst;
  uint32 v_line, next_v_line, *nt;

  /* Vertical scroll offset */
  int v_offset = 0;

  /* Common data */
  uint32 xscroll      = *(uint32 *)&vram[hscb + ((line & hscroll_mask) << 2)];
  uint32 yscroll      = 0;
  uint32 pf_col_mask  = playfield_col_mask;
  uint32 pf_row_mask  = playfield_row_mask;
  uint32 pf_shift     = playfield_shift;
  uint32 *vs          = (uint32 *)&vsram[0];

  /* Window & Plane A */
  int a = (reg[18] & 0x1F) << 3;
  int w = (reg[18] >> 7) & 1;

  /* Plane B width */
  int start = 0;
  int end = bitmap.viewport.w >> 4;

  /* Plane B horizontal scroll */
#ifdef LSB_FIRST
  uint32 shift  = (xscroll >> 16) & 0x0F;
  uint32 index  = pf_col_mask + 1 - ((xscroll >> 20) & pf_col_mask);
#else
  uint32 shift  = (xscroll & 0x0F);
  uint32 index  = pf_col_mask + 1 - ((xscroll >> 4) & pf_col_mask);
#endif

  /* Left-most column vertical scrolling when partially shown horizontally (verified on PAL MD2)  */
  /* TODO: check on Genesis 3 models since it apparently behaves differently  */
  /* In H32 mode, vertical scrolling is disabled, in H40 mode, same value is used for both planes */
  /* See Formula One / Kawasaki Superbike Challenge (H32) & Gynoug / Cutie Suzuki no Ringside Angel (H40) */
  if (reg[12] & 1)
  {
    yscroll = vs[19] & (vs[19] >> 16);
  }

  if(shift)
  {
    /* Plane B vertical scroll */
    v_line = (line + yscroll) & pf_row_mask;

    /* Plane B name table */
    nt = (uint32 *)&vram[ntbb + (((v_line >> 3) << pf_shift) & 0x1FC0)];

    /* Pattern row index */
    v_line = (v_line & 7) << 3;

    /* Plane B line buffer */
    dst = &linebuf[0][0x10 + shift];

    atbuf = nt[(index - 1) & pf_col_mask];
    DRAW_COLUMN(atbuf, v_line)
  }
  else
  {
    /* Plane B line buffer */
    dst = &linebuf[0][0x20];
  }

  for(column = 0; column < end; column++, index++)
  {
    /* Plane B vertical scroll */
#ifdef LSB_FIRST
    v_line = (line + (vs[column] >> 16)) & pf_row_mask;
    next_v_line = (line + (vs[column + 1] >> 16)) & pf_row_mask;
#else
    v_line = (line + vs[column]) & pf_row_mask;
    next_v_line = (line + vs[column + 1]) & pf_row_mask;
#endif

    if (column != end - 1)
    {
      /* The offset of the intermediary cell is an average of the offsets of the current 2-cell and the next 2-cell. */
      /* For the last column, the previously calculated offset is used */
      v_offset = ((int)next_v_line - (int)v_line) / 2;
      v_offset = (abs(v_offset) >= config.enhanced_vscroll_limit) ? 0 : v_offset;
    }

    /* Plane B name table */
    nt = (uint32 *)&vram[ntbb + (((v_line >> 3) << pf_shift) & 0x1FC0)];

    /* Pattern row index */
    v_line = (v_line & 7) << 3;

    atbuf = nt[index & pf_col_mask];
#ifdef LSB_FIRST
    GET_LSB_TILE(atbuf, v_line)
#else
    GET_MSB_TILE(atbuf, v_line)
#endif

    WRITE_LONG(dst, src[0] | atex);
    WRITE_LONG(dst + 4, src[1] | atex);
    dst += 8;

#ifdef LSB_FIRST
    v_line = (line + v_offset + (vs[column] >> 16)) & pf_row_mask;
#else
    v_line = (line + v_offset + vs[column]) & pf_row_mask;
#endif

    nt = (uint32 *)&vram[ntbb + (((v_line >> 3) << pf_shift) & 0x1FC0)];
    v_line = (v_line & 7) << 3;
    atbuf = nt[index & pf_col_mask];

#ifdef LSB_FIRST
    GET_MSB_TILE(atbuf, v_line)
#else
    GET_LSB_TILE(atbuf, v_line)
#endif
    WRITE_LONG(dst, src[0] | atex);
    WRITE_LONG(dst + 4, src[1] | atex);
    dst += 8;
  }

  if (w == (line >= a))
  {
    /* Window takes up entire line */
    a = 0;
    w = 1;
  }
  else
  {
    /* Window and Plane A share the line */
    a = clip[0].enable;
    w = clip[1].enable;
  }

  /* Plane A */
  if (a)
  {
    /* Plane A width */
    start = clip[0].left;
    end   = clip[0].right;

    /* Plane A horizontal scroll */
#ifdef LSB_FIRST
    shift = (xscroll & 0x0F);
    index = pf_col_mask + start + 1 - ((xscroll >> 4) & pf_col_mask);
#else
    shift = (xscroll >> 16) & 0x0F;
    index = pf_col_mask + start + 1 - ((xscroll >> 20) & pf_col_mask);
#endif

    if(shift)
    {
      /* Plane A vertical scroll */
      v_line = (line + yscroll) & pf_row_mask;

      /* Plane A name table */
      nt = (uint32 *)&vram[ntab + (((v_line >> 3) << pf_shift) & 0x1FC0)];

      /* Pattern row index */
      v_line = (v_line & 7) << 3;

      /* Plane A line buffer */
      dst = &linebuf[1][0x10 + shift + (start << 4)];

      /* Window bug */
      if (start)
      {
        atbuf = nt[index & pf_col_mask];
      }
      else
      {
        atbuf = nt[(index - 1) & pf_col_mask];
      }

      DRAW_COLUMN(atbuf, v_line)
    }
    else
    {
      /* Plane A line buffer */
      dst = &linebuf[1][0x20 + (start << 4)];
    }

    for(column = start; column < end; column++, index++)
    {
      /* Plane A vertical scroll */
#ifdef LSB_FIRST
      v_line = (line + vs[column]) & pf_row_mask;
      next_v_line = (line + vs[column + 1]) & pf_row_mask;
#else
      v_line = (line + (vs[column] >> 16)) & pf_row_mask;
      next_v_line = (line + (vs[column + 1] >> 16)) & pf_row_mask;
#endif

      if (column != end - 1)
      {
        v_offset = ((int)next_v_line - (int)v_line) / 2;
        v_offset = (abs(v_offset) >= config.enhanced_vscroll_limit) ? 0 : v_offset;
      }

      /* Plane A name table */
      nt = (uint32 *)&vram[ntab + (((v_line >> 3) << pf_shift) & 0x1FC0)];

      /* Pattern row index */
      v_line = (v_line & 7) << 3;

      atbuf = nt[index & pf_col_mask];
#ifdef LSB_FIRST
      GET_LSB_TILE(atbuf, v_line)
#else
      GET_MSB_TILE(atbuf, v_line)
#endif
      WRITE_LONG(dst, src[0] | atex);
      WRITE_LONG(dst + 4, src[1] | atex);
      dst += 8;

#ifdef LSB_FIRST
      v_line = (line + v_offset + vs[column]) & pf_row_mask;
#else
      v_line = (line + v_offset + (vs[column] >> 16)) & pf_row_mask;
#endif

      nt = (uint32 *)&vram[ntab + (((v_line >> 3) << pf_shift) & 0x1FC0)];
      v_line = (v_line & 7) << 3;
      atbuf = nt[index & pf_col_mask];

#ifdef LSB_FIRST
      GET_MSB_TILE(atbuf, v_line)
#else
      GET_LSB_TILE(atbuf, v_line)
#endif
      WRITE_LONG(dst, src[0] | atex);
      WRITE_LONG(dst + 4, src[1] | atex);
      dst += 8;
    }

    /* Window width */
    start = clip[1].left;
    end   = clip[1].right;
  }

  /* Window */
  if (w)
  {
    /* Window name table */
    nt = (uint32 *)&vram[ntwb | ((line >> 3) << (6 + (reg[12] & 1)))];

    /* Pattern row index */
    v_line = (line & 7) << 3;

    /* Plane A line buffer */
    dst = &linebuf[1][0x20 + (start << 4)];

    for(column = start; column < end; column++)
    {
      atbuf = nt[column];
      DRAW_COLUMN(atbuf, v_line)
    }
  }

  /* Merge background layers */
  merge(&linebuf[1][0x20], &linebuf[0][0x20], &linebuf[0][0x20], lut[(reg[12] & 0x08) >> 2], bitmap.viewport.w);
}

void render_bg_m5_im2(int line)
{
  int column, start, end, a, w;
  uint32 atex, atbuf, *src;
  uint8 *dst;
  uint32 shift, index, v_line, *nt;

  /* Common data */
  uint32 xscroll      = *(uint32 *)&vram[hscb + ((line & hscroll_mask) << 2)];
  uint32 yscroll      = *(uint32 *)&vsram[0];
  uint32 pf_col_mask  = playfield_col_mask;
  uint32 pf_row_mask  = (playfield_row_mask << 1) | 1;
  uint32 pf_shift     = playfield_shift;

  /* Adjust line offset */
  line = (line << 1) + odd_frame;

  /* Plane B width */
  start = 0;
  end = bitmap.viewport.w >> 4;

  /* Plane B scroll */
#ifdef LSB_FIRST
  shift  = (xscroll >> 16) & 0x0F;
  index  = pf_col_mask + 1 - ((xscroll >> 20) & pf_col_mask);
  v_line = (line + (yscroll >> 16)) & pf_row_mask;
#else
  shift  = (xscroll & 0x0F);
  index  = pf_col_mask + 1 - ((xscroll >> 4) & pf_col_mask);
  v_line = (line + yscroll) & pf_row_mask;
#endif

  /* Plane B name table */
  nt = (uint32 *)&vram[ntbb + (((v_line >> 4) << pf_shift) & 0x1FC0)];

  /* Pattern row index */
  v_line = (v_line & 15) << 3;

  if(shift)
  {
    /* Plane B line buffer */
    dst = &linebuf[0][0x10 + shift];

    atbuf = nt[(index - 1) & pf_col_mask];
    DRAW_COLUMN_IM2(atbuf, v_line)
  }
  else
  {
    /* Plane B line buffer */
    dst = &linebuf[0][0x20];
  }

  for(column = 0; column < end; column++, index++)
  {
    atbuf = nt[index & pf_col_mask];
    DRAW_COLUMN_IM2(atbuf, v_line)
  }

  /* Window & Plane A */
  a = (reg[18] & 0x1F) << 4;
  w = (reg[18] >> 7) & 1;

  if (w == (line >= a))
  {
    /* Window takes up entire line */
    a = 0;
    w = 1;
  }
  else
  {
    /* Window and Plane A share the line */
    a = clip[0].enable;
    w = clip[1].enable;
  }

  /* Plane A */
  if (a)
  {
    /* Plane A width */
    start = clip[0].left;
    end   = clip[0].right;

    /* Plane A scroll */
#ifdef LSB_FIRST
    shift   = (xscroll & 0x0F);
    index   = pf_col_mask + start + 1 - ((xscroll >> 4) & pf_col_mask);
    v_line  = (line + yscroll) & pf_row_mask;
#else
    shift   = (xscroll >> 16) & 0x0F;
    index   = pf_col_mask + start + 1 - ((xscroll >> 20) & pf_col_mask);
    v_line  = (line + (yscroll >> 16)) & pf_row_mask;
#endif

    /* Plane A name table */
    nt = (uint32 *)&vram[ntab + (((v_line >> 4) << pf_shift) & 0x1FC0)];

    /* Pattern row index */
    v_line = (v_line & 15) << 3;

    if(shift)
    {
      /* Plane A line buffer */
      dst = &linebuf[1][0x10 + shift + (start << 4)];

      /* Window bug */
      if (start)
      {
        atbuf = nt[index & pf_col_mask];
      }
      else
      {
        atbuf = nt[(index - 1) & pf_col_mask];
      }

      DRAW_COLUMN_IM2(atbuf, v_line)
    }
    else
    {
      /* Plane A line buffer */
      dst = &linebuf[1][0x20 + (start << 4)];
    }

    for(column = start; column < end; column++, index++)
    {
      atbuf = nt[index & pf_col_mask];
      DRAW_COLUMN_IM2(atbuf, v_line)
    }

    /* Window width */
    start = clip[1].left;
    end   = clip[1].right;
  }

  /* Window */
  if (w)
  {
    /* Window name table */
    nt = (uint32 *)&vram[ntwb | ((line >> 4) << (6 + (reg[12] & 1)))];

    /* Pattern row index */
    v_line = (line & 15) << 3;

    /* Plane A line buffer */
    dst = &linebuf[1][0x20 + (start << 4)];

    for(column = start; column < end; column++)
    {
      atbuf = nt[column];
      DRAW_COLUMN_IM2(atbuf, v_line)
    }
  }

  /* Merge background layers */
  merge(&linebuf[1][0x20], &linebuf[0][0x20], &linebuf[0][0x20], lut[(reg[12] & 0x08) >> 2], bitmap.viewport.w);
}

void render_bg_m5_im2_vs(int line)
{
  int column, start, end, a, w;
  uint32 atex, atbuf, *src;
  uint8 *dst;
  uint32 shift, index, v_line, *nt;

  /* Common data */
  uint32 xscroll      = *(uint32 *)&vram[hscb + ((line & hscroll_mask) << 2)];
  uint32 yscroll      = 0;
  uint32 pf_col_mask  = playfield_col_mask;
  uint32 pf_row_mask  = (playfield_row_mask << 1) | 1;
  uint32 pf_shift     = playfield_shift;
  uint32 *vs          = (uint32 *)&vsram[0];

  /* Adjust line offset */
  line = (line << 1) + odd_frame;

  /* Plane B width */
  start = 0;
  end = bitmap.viewport.w >> 4;

  /* Plane B horizontal scroll */
#ifdef LSB_FIRST
  shift = (xscroll >> 16) & 0x0F;
  index = pf_col_mask + 1 - ((xscroll >> 20) & pf_col_mask);
#else
  shift = (xscroll & 0x0F);
  index = pf_col_mask + 1 - ((xscroll >> 4) & pf_col_mask);
#endif

  /* Left-most column vertical scrolling when partially shown horizontally (verified on PAL MD2)  */
  /* TODO: check on Genesis 3 models since it apparently behaves differently  */
  /* In H32 mode, vertical scrolling is disabled, in H40 mode, same value is used for both planes */
  /* See Formula One / Kawasaki Superbike Challenge (H32) & Gynoug / Cutie Suzuki no Ringside Angel (H40) */
  if (reg[12] & 1)
  {
    yscroll = vs[19] & (vs[19] >> 16);
  }

  if(shift)
  {
    /* Plane B vertical scroll */
    v_line = (line + yscroll) & pf_row_mask;

    /* Plane B name table */
    nt = (uint32 *)&vram[ntbb + (((v_line >> 4) << pf_shift) & 0x1FC0)];

    /* Pattern row index */
    v_line = (v_line & 15) << 3;

    /* Plane B line buffer */
    dst = &linebuf[0][0x10 + shift];

    atbuf = nt[(index - 1) & pf_col_mask];
    DRAW_COLUMN_IM2(atbuf, v_line)
  }
  else
  {
    /* Plane B line buffer */
    dst = &linebuf[0][0x20];
  }

  for(column = 0; column < end; column++, index++)
  {
    /* Plane B vertical scroll */
#ifdef LSB_FIRST
    v_line = (line + (vs[column] >> 16)) & pf_row_mask;
#else
    v_line = (line + vs[column]) & pf_row_mask;
#endif

    /* Plane B name table */
    nt = (uint32 *)&vram[ntbb + (((v_line >> 4) << pf_shift) & 0x1FC0)];

    /* Pattern row index */
    v_line = (v_line & 15) << 3;

    atbuf = nt[index & pf_col_mask];
    DRAW_COLUMN_IM2(atbuf, v_line)
  }

  /* Window & Plane A */
  a = (reg[18] & 0x1F) << 4;
  w = (reg[18] >> 7) & 1;

  if (w == (line >= a))
  {
    /* Window takes up entire line */
    a = 0;
    w = 1;
  }
  else
  {
    /* Window and Plane A share the line */
    a = clip[0].enable;
    w = clip[1].enable;
  }

  /* Plane A */
  if (a)
  {
    /* Plane A width */
    start = clip[0].left;
    end   = clip[0].right;

    /* Plane A horizontal scroll */
#ifdef LSB_FIRST
    shift = (xscroll & 0x0F);
    index = pf_col_mask + start + 1 - ((xscroll >> 4) & pf_col_mask);
#else
    shift = (xscroll >> 16) & 0x0F;
    index = pf_col_mask + start + 1 - ((xscroll >> 20) & pf_col_mask);
#endif

    if(shift)
    {
      /* Plane A vertical scroll */
      v_line = (line + yscroll) & pf_row_mask;

      /* Plane A name table */
      nt = (uint32 *)&vram[ntab + (((v_line >> 4) << pf_shift) & 0x1FC0)];

      /* Pattern row index */
      v_line = (v_line & 15) << 3;

      /* Plane A line buffer */
      dst = &linebuf[1][0x10 + shift + (start << 4)];

      /* Window bug */
      if (start)
      {
        atbuf = nt[index & pf_col_mask];
      }
      else
      {
        atbuf = nt[(index - 1) & pf_col_mask];
      }

      DRAW_COLUMN_IM2(atbuf, v_line)
    }
    else
    {
      /* Plane A line buffer */
      dst = &linebuf[1][0x20 + (start << 4)];
    }

    for(column = start; column < end; column++, index++)
    {
      /* Plane A vertical scroll */
#ifdef LSB_FIRST
      v_line = (line + vs[column]) & pf_row_mask;
#else
      v_line = (line + (vs[column] >> 16)) & pf_row_mask;
#endif

      /* Plane A name table */
      nt = (uint32 *)&vram[ntab + (((v_line >> 4) << pf_shift) & 0x1FC0)];

      /* Pattern row index */
      v_line = (v_line & 15) << 3;

      atbuf = nt[index & pf_col_mask];
      DRAW_COLUMN_IM2(atbuf, v_line)
    }

    /* Window width */
    start = clip[1].left;
    end   = clip[1].right;
  }

  /* Window */
  if (w)
  {
    /* Window name table */
    nt = (uint32 *)&vram[ntwb | ((line >> 4) << (6 + (reg[12] & 1)))];

    /* Pattern row index */
    v_line = (line & 15) << 3;

    /* Plane A line buffer */
    dst = &linebuf[1][0x20 + (start << 4)];

    for(column = start; column < end; column++)
    {
      atbuf = nt[column];
      DRAW_COLUMN_IM2(atbuf, v_line)
    }
  }

  /* Merge background layers */
  merge(&linebuf[1][0x20], &linebuf[0][0x20], &linebuf[0][0x20], lut[(reg[12] & 0x08) >> 2], bitmap.viewport.w);
}

#else

void render_bg_m5(int line)
{
  int column, start, end;
  uint32 atex, atbuf, *src;
  uint8 *dst;
  uint32 shift, index, v_line, *nt;
  uint8 *lb;

  /* Scroll Planes common data */
  uint32 xscroll      = wide_xs(*(uint32 *)&vram[hscb + ((line & hscroll_mask) << 2)]);
  uint32 yscroll      = *(uint32 *)&vsram[0];
  uint32 pf_col_mask  = playfield_col_mask;
  uint32 pf_row_mask  = playfield_row_mask;
  uint32 pf_shift     = playfield_shift;

  /* Number of columns to draw */
  int width = WIDE_COLS(bitmap.viewport.w >> 4);

  /* Layer priority table */
  uint8 *table = lut[(reg[12] & 8) >> 2];

  /* Window vertical range (cell 0-31) */
  int a = (reg[18] & 0x1F) << 3;

  /* Window position (0=top, 1=bottom) */
  int w = (reg[18] >> 7) & 1;

  /* Test against current line */
  if (w == (line >= a))
  {
    /* Window takes up entire line */
    a = 0;
    w = 1;
  }
  else
  {
    /* Window and Plane A share the line */
    a = clip[0].enable;
    w = clip[1].enable;
  }

  /* VitAdhoc wide view: a plane left out of the strip pass is drawn as nothing */
  if (wide_pass)
  {
    if ((wide_planes & 1) && wide_a) { a = 1; w = 0; }      /* plane A continues beside the screen; the window plane never does */
    else
    {
      a = 0; w = 0;
      memset(&linebuf[0][0x20 - 16], 0, wide_px + 48);
    }
  }

  /* Plane A */
  if (a)
  {
    /* Plane A width */
    start = wide_pass ? 0 : clip[0].left;
    end   = WIDE_COLS(clip[0].right);

    /* Plane A scroll */
#ifdef LSB_FIRST
    shift  = (xscroll & 0x0F);
    index  = pf_col_mask + start + 1 - ((xscroll >> 4) & pf_col_mask);
    v_line = (line + yscroll) & pf_row_mask;
#else
    shift  = (xscroll >> 16) & 0x0F;
    index  = pf_col_mask + start + 1 - ((xscroll >> 20) & pf_col_mask);
    v_line = (line + (yscroll >> 16)) & pf_row_mask;
#endif

    /* Background line buffer */
    dst = &linebuf[0][0x20 + (start << 4) + shift];

    /* Plane A name table */
    nt = (uint32 *)&vram[ntab + (((v_line >> 3) << pf_shift) & 0x1FC0)];
    if (wide_pass && wide_ont[0]) nt = wide_ont[0];      /* VitAdhoc wide view: a learned row */

    /* Pattern row index */
    v_line = (v_line & 7) << 3;

    if(shift)
    {
      /* Left-most column is partially shown */
      dst -= 16;

      /* Window bug */
      if (start)
      {
        atbuf = nt[index & pf_col_mask];
      }
      else
      {
        atbuf = nt[(index-1) & pf_col_mask];
      }

      DRAW_COLUMN(atbuf, v_line)
    }

    for(column = start; column < end; column++, index++)
    {
      atbuf = nt[index & pf_col_mask];
      DRAW_COLUMN(atbuf, v_line)
    }

    /* Window width */
    start = clip[1].left;
    end   = clip[1].right;
  }
  else
  {
    /* Window width */
    start = 0;
    end = width;
  }

  /* Window Plane */
  if (w)
  {
    /* Background line buffer */
    dst = &linebuf[0][0x20 + (start << 4)];

    /* Window name table */
    nt = (uint32 *)&vram[ntwb | ((line >> 3) << (6 + (reg[12] & 1)))];

    /* Pattern row index */
    v_line = (line & 7) << 3;

    for(column = start; column < end; column++)
    {
      atbuf = nt[column];
      DRAW_COLUMN(atbuf, v_line)
    }
  }

  /* VitAdhoc wide view: plane B left out of the strip pass: the plane A pixels go through the priority table alone */
  if (wide_pass && !(wide_planes & 2))
  {
    uint8 *q = &linebuf[0][0x20];
    int n = wide_px, i;
    for (i = 0; i < n; i++) q[i] = table[q[i]];
    return;
  }

  /* Plane B scroll */
#ifdef LSB_FIRST
  shift  = (xscroll >> 16) & 0x0F;
  index  = pf_col_mask + 1 - ((xscroll >> 20) & pf_col_mask);
  v_line = (line + (yscroll >> 16)) & pf_row_mask;
#else
  shift  = (xscroll & 0x0F);
  index  = pf_col_mask + 1 - ((xscroll >> 4) & pf_col_mask);
  v_line = (line + yscroll) & pf_row_mask;
#endif

  /* Plane B name table */
  nt = (uint32 *)&vram[ntbb + (((v_line >> 3) << pf_shift) & 0x1FC0)];
  if (wide_pass && wide_ont[1]) nt = wide_ont[1];

  /* Pattern row index */
  v_line = (v_line & 7) << 3;

  /* Background line buffer */
  lb = &linebuf[0][0x20];

  if(shift)
  {
    /* Left-most column is partially shown */
    lb -= (0x10 - shift);

    atbuf = nt[(index-1) & pf_col_mask];
    DRAW_BG_COLUMN(atbuf, v_line, xscroll, yscroll)
  }

  for(column = 0; column < width; column++, index++)
  {
    atbuf = nt[index & pf_col_mask];
    DRAW_BG_COLUMN(atbuf, v_line, xscroll, yscroll)
  }
}

void render_bg_m5_vs(int line)
{
  int column, start, end;
  uint32 atex, atbuf, *src;
  uint8 *dst;
  uint32 shift, index, v_line, *nt;
  uint8 *lb;

  /* Scroll Planes common data */
  uint32 xscroll      = wide_xs(*(uint32 *)&vram[hscb + ((line & hscroll_mask) << 2)]);
  uint32 yscroll      = 0;
  uint32 pf_col_mask  = playfield_col_mask;
  uint32 pf_row_mask  = playfield_row_mask;
  uint32 pf_shift     = playfield_shift;
  uint32 *vs          = wide_pass ? wide_vsb : (uint32 *)&vsram[0];

  /* Number of columns to draw */
  int width = WIDE_COLS(bitmap.viewport.w >> 4);

  /* Layer priority table */
  uint8 *table = lut[(reg[12] & 8) >> 2];

  /* Window vertical range (cell 0-31) */
  int a = (reg[18] & 0x1F) << 3;

  /* Window position (0=top, 1=bottom) */
  int w = (reg[18] >> 7) & 1;

  /* Test against current line */
  if (w == (line >= a))
  {
    /* Window takes up entire line */
    a = 0;
    w = 1;
  }
  else
  {
    /* Window and Plane A share the line */
    a = clip[0].enable;
    w = clip[1].enable;
  }

  /* VitAdhoc wide view: a plane left out of the strip pass is drawn as nothing */
  if (wide_pass)
  {
    if ((wide_planes & 1) && wide_a) { a = 1; w = 0; }      /* plane A continues beside the screen; the window plane never does */
    else
    {
      a = 0; w = 0;
      memset(&linebuf[0][0x20 - 16], 0, wide_px + 48);
    }
  }

  /* Left-most column vertical scrolling when partially shown horizontally (verified on PAL MD2)  */
  /* TODO: check on Genesis 3 models since it apparently behaves differently  */
  /* In H32 mode, vertical scrolling is disabled, in H40 mode, same value is used for both planes */
  /* See Formula One / Kawasaki Superbike Challenge (H32) & Gynoug / Cutie Suzuki no Ringside Angel (H40) */
  if (reg[12] & 1)
  {
    yscroll = vs[19] & (vs[19] >> 16);
  }
  if (wide_pass) yscroll = wide_ys[0];      /* VitAdhoc wide view: the partly shown column scrolls like the others of the strip */

  /* Plane A*/
  if (a)
  {
    /* Plane A width */
    start = wide_pass ? 0 : clip[0].left;
    end   = WIDE_COLS(clip[0].right);

    /* Plane A horizontal scroll */
#ifdef LSB_FIRST
    shift = (xscroll & 0x0F);
    index = pf_col_mask + start + 1 - ((xscroll >> 4) & pf_col_mask);
#else
    shift = (xscroll >> 16) & 0x0F;
    index = pf_col_mask + start + 1 - ((xscroll >> 20) & pf_col_mask);
#endif

    /* Background line buffer */
    dst = &linebuf[0][0x20 + (start << 4) + shift];

    if(shift)
    {
      /* Left-most column is partially shown */
      dst -= 16;

      /* Plane A vertical scroll */
      v_line = (line + yscroll) & pf_row_mask;

      /* Plane A name table */
      nt = (uint32 *)&vram[ntab + (((v_line >> 3) << pf_shift) & 0x1FC0)];

      /* Pattern row index */
      v_line = (v_line & 7) << 3;

      /* Window bug */
      if (start)
      {
        atbuf = nt[index & pf_col_mask];
      }
      else
      {
        atbuf = nt[(index-1) & pf_col_mask];
      }

      DRAW_COLUMN(atbuf, v_line)
    }

    for(column = start; column < end; column++, index++)
    {
      /* Plane A vertical scroll */
#ifdef LSB_FIRST
      v_line = (line + vs[column]) & pf_row_mask;
#else
      v_line = (line + (vs[column] >> 16)) & pf_row_mask;
#endif

      /* Plane A name table */
      nt = (uint32 *)&vram[ntab + (((v_line >> 3) << pf_shift) & 0x1FC0)];

      /* Pattern row index */
      v_line = (v_line & 7) << 3;

      atbuf = nt[index & pf_col_mask];
      DRAW_COLUMN(atbuf, v_line)
    }

    /* Window width */
    start = clip[1].left;
    end   = clip[1].right;
  }
  else
  {
    /* Window width */
    start = 0;
    end   = width;
  }

  /* Window Plane */
  if (w)
  {
    /* Background line buffer */
    dst = &linebuf[0][0x20 + (start << 4)];

    /* Window name table */
    nt = (uint32 *)&vram[ntwb | ((line >> 3) << (6 + (reg[12] & 1)))];

    /* Pattern row index */
    v_line = (line & 7) << 3;

    for(column = start; column < end; column++)
    {
      atbuf = nt[column];
      DRAW_COLUMN(atbuf, v_line)
    }
  }

  /* VitAdhoc wide view: plane B left out of the strip pass: the plane A pixels go through the priority table alone */
  if (wide_pass && !(wide_planes & 2))
  {
    uint8 *q = &linebuf[0][0x20];
    int n = wide_px, i;
    for (i = 0; i < n; i++) q[i] = table[q[i]];
    return;
  }

  if (wide_pass) yscroll = wide_ys[1];

  /* Plane B horizontal scroll */
#ifdef LSB_FIRST
  shift = (xscroll >> 16) & 0x0F;
  index = pf_col_mask + 1 - ((xscroll >> 20) & pf_col_mask);
#else
  shift = (xscroll & 0x0F);
  index = pf_col_mask + 1 - ((xscroll >> 4) & pf_col_mask);
#endif

  /* Background line buffer */
  lb = &linebuf[0][0x20];

  if(shift)
  {
    /* Left-most column is partially shown */
    lb -= (0x10 - shift);

    /* Plane B vertical scroll */
    v_line = (line + yscroll) & pf_row_mask;

    /* Plane B name table */
    nt = (uint32 *)&vram[ntbb + (((v_line >> 3) << pf_shift) & 0x1FC0)];

    /* Pattern row index */
    v_line = (v_line & 7) << 3;

    atbuf = nt[(index-1) & pf_col_mask];
    DRAW_BG_COLUMN(atbuf, v_line, xscroll, yscroll)
  }

  for(column = 0; column < width; column++, index++)
  {
    /* Plane B vertical scroll */
#ifdef LSB_FIRST
    v_line = (line + (vs[column] >> 16)) & pf_row_mask;
#else
    v_line = (line + vs[column]) & pf_row_mask;
#endif

    /* Plane B name table */
    nt = (uint32 *)&vram[ntbb + (((v_line >> 3) << pf_shift) & 0x1FC0)];

    /* Pattern row index */
    v_line = (v_line & 7) << 3;

    atbuf = nt[index & pf_col_mask];
    DRAW_BG_COLUMN(atbuf, v_line, xscroll, yscroll)
  }
}

void render_bg_m5_vs_enhanced(int line)
{
  int column, start, end;
  uint32 atex, atbuf, *src;
  uint8 *dst;
  uint32 shift, index, v_line, next_v_line, *nt;
  uint8 *lb;

  /* Vertical scroll offset */
  int v_offset = 0;

  /* Scroll Planes common data */
  uint32 xscroll      = wide_xs(*(uint32 *)&vram[hscb + ((line & hscroll_mask) << 2)]);
  uint32 yscroll      = 0;
  uint32 pf_col_mask  = playfield_col_mask;
  uint32 pf_row_mask  = playfield_row_mask;
  uint32 pf_shift     = playfield_shift;
  uint32 *vs          = (uint32 *)&vsram[0];

  /* Number of columns to draw */
  int width = WIDE_COLS(bitmap.viewport.w >> 4);

  /* Layer priority table */
  uint8 *table = lut[(reg[12] & 8) >> 2];

  /* Window vertical range (cell 0-31) */
  int a = (reg[18] & 0x1F) << 3;

  /* Window position (0=top, 1=bottom) */
  int w = (reg[18] >> 7) & 1;

  /* Test against current line */
  if (w == (line >= a))
  {
    /* Window takes up entire line */
    a = 0;
    w = 1;
  }
  else
  {
    /* Window and Plane A share the line */
    a = clip[0].enable;
    w = clip[1].enable;
  }

  /* VitAdhoc wide view: a plane left out of the strip pass is drawn as nothing */
  if (wide_pass)
  {
    if ((wide_planes & 1) && wide_a) { a = 1; w = 0; }      /* plane A continues beside the screen; the window plane never does */
    else
    {
      a = 0; w = 0;
      memset(&linebuf[0][0x20 - 16], 0, wide_px + 48);
    }
  }

  /* Left-most column vertical scrolling when partially shown horizontally (verified on PAL MD2)  */
  /* TODO: check on Genesis 3 models since it apparently behaves differently  */
  /* In H32 mode, vertical scrolling is disabled, in H40 mode, same value is used for both planes */
  /* See Formula One / Kawasaki Superbike Challenge (H32) & Gynoug / Cutie Suzuki no Ringside Angel (H40) */
  if (reg[12] & 1)
  {
    yscroll = vs[19] & (vs[19] >> 16);
  }

  /* Plane A*/
  if (a)
  {
    /* Plane A width */
    start = wide_pass ? 0 : clip[0].left;
    end   = WIDE_COLS(clip[0].right);

    /* Plane A horizontal scroll */
#ifdef LSB_FIRST
    shift = (xscroll & 0x0F);
    index = pf_col_mask + start + 1 - ((xscroll >> 4) & pf_col_mask);
#else
    shift = (xscroll >> 16) & 0x0F;
    index = pf_col_mask + start + 1 - ((xscroll >> 20) & pf_col_mask);
#endif

    /* Background line buffer */
    dst = &linebuf[0][0x20 + (start << 4) + shift];

    if(shift)
    {
      /* Left-most column is partially shown */
      dst -= 16;

      /* Plane A vertical scroll */
      v_line = (line + yscroll) & pf_row_mask;

      /* Plane A name table */
      nt = (uint32 *)&vram[ntab + (((v_line >> 3) << pf_shift) & 0x1FC0)];

      /* Pattern row index */
      v_line = (v_line & 7) << 3;

      /* Window bug */
      if (start)
      {
        atbuf = nt[index & pf_col_mask];
      }
      else
      {
        atbuf = nt[(index-1) & pf_col_mask];
      }

      DRAW_COLUMN(atbuf, v_line)
    }

    for(column = start; column < end; column++, index++)
    {
      /* Plane A vertical scroll */
#ifdef LSB_FIRST
      v_line = (line + vs[column]) & pf_row_mask;
      next_v_line = (line + vs[column + 1]) & pf_row_mask;
#else
      v_line = (line + (vs[column] >> 16)) & pf_row_mask;
      next_v_line = (line + (vs[column + 1] >> 16)) & pf_row_mask;
#endif

      if (column != end - 1)
      {
        v_offset = ((int)next_v_line - (int)v_line) / 2;
        v_offset = (abs(v_offset) >= config.enhanced_vscroll_limit) ? 0 : v_offset;
      }

      /* Plane A name table */
      nt = (uint32 *)&vram[ntab + (((v_line >> 3) << pf_shift) & 0x1FC0)];

      /* Pattern row index */
      v_line = (v_line & 7) << 3;

      atbuf = nt[index & pf_col_mask];
#ifdef LSB_FIRST
      GET_LSB_TILE(atbuf, v_line)
#else
      GET_MSB_TILE(atbuf, v_line)
#endif
      WRITE_LONG(dst, src[0] | atex);
      WRITE_LONG(dst + 4, src[1] | atex);
      dst += 8;

#ifdef LSB_FIRST
      v_line = (line + v_offset + vs[column]) & pf_row_mask;
#else
      v_line = (line + v_offset + (vs[column] >> 16)) & pf_row_mask;
#endif

      nt = (uint32 *)&vram[ntab + (((v_line >> 3) << pf_shift) & 0x1FC0)];
      v_line = (v_line & 7) << 3;
      atbuf = nt[index & pf_col_mask];

#ifdef LSB_FIRST
      GET_MSB_TILE(atbuf, v_line)
#else
      GET_LSB_TILE(atbuf, v_line)
#endif
      WRITE_LONG(dst, src[0] | atex);
      WRITE_LONG(dst + 4, src[1] | atex);
      dst += 8;
    }

    /* Window width */
    start = clip[1].left;
    end   = clip[1].right;
  }
  else
  {
    /* Window width */
    start = 0;
    end   = width;
  }

  /* Window Plane */
  if (w)
  {
    /* Background line buffer */
    dst = &linebuf[0][0x20 + (start << 4)];

    /* Window name table */
    nt = (uint32 *)&vram[ntwb | ((line >> 3) << (6 + (reg[12] & 1)))];

    /* Pattern row index */
    v_line = (line & 7) << 3;

    for(column = start; column < end; column++)
    {
      atbuf = nt[column];
      DRAW_COLUMN(atbuf, v_line)
    }
  }

  /* VitAdhoc wide view: plane B left out of the strip pass: the plane A pixels go through the priority table alone */
  if (wide_pass && !(wide_planes & 2))
  {
    uint8 *q = &linebuf[0][0x20];
    int n = wide_px, i;
    for (i = 0; i < n; i++) q[i] = table[q[i]];
    return;
  }

  /* Plane B horizontal scroll */
#ifdef LSB_FIRST
  shift = (xscroll >> 16) & 0x0F;
  index = pf_col_mask + 1 - ((xscroll >> 20) & pf_col_mask);
#else
  shift = (xscroll & 0x0F);
  index = pf_col_mask + 1 - ((xscroll >> 4) & pf_col_mask);
#endif

  /* Background line buffer */
  lb = &linebuf[0][0x20];

  if(shift)
  {
    /* Left-most column is partially shown */
    lb -= (0x10 - shift);

    /* Plane B vertical scroll */
    v_line = (line + yscroll) & pf_row_mask;

    /* Plane B name table */
    nt = (uint32 *)&vram[ntbb + (((v_line >> 3) << pf_shift) & 0x1FC0)];

    /* Pattern row index */
    v_line = (v_line & 7) << 3;

    atbuf = nt[(index-1) & pf_col_mask];
    DRAW_BG_COLUMN(atbuf, v_line, xscroll, yscroll)
  }

  for(column = 0; column < width; column++, index++)
  {
    /* Plane B vertical scroll */
#ifdef LSB_FIRST
    v_line = (line + (vs[column] >> 16)) & pf_row_mask;
    next_v_line = (line + (vs[column + 1] >> 16)) & pf_row_mask;
#else
    v_line = (line + vs[column]) & pf_row_mask;
    next_v_line = (line + vs[column + 1]) & pf_row_mask;
#endif

    if (column != width - 1)
    {
      v_offset = ((int)next_v_line - (int)v_line) / 2;
      v_offset = (abs(v_offset) >= config.enhanced_vscroll_limit) ? 0 : v_offset;
    }
    
    /* Plane B name table */
    nt = (uint32 *)&vram[ntbb + (((v_line >> 3) << pf_shift) & 0x1FC0)];

    /* Pattern row index */
    v_line = (v_line & 7) << 3;

    atbuf = nt[index & pf_col_mask];
#if defined(__ARM_NEON) && defined(LSB_FIRST)
    {
      const uint8 *_s0 = &bg_pattern_cache[(atbuf & 0x00001FFF) << 6 | v_line];
      uint8x8_t _b0 = vorr_u8(vld1_u8(_s0), vdup_n_u8((uint8)((atbuf >> 9) & 0x70)));

      v_line = (line + v_offset + (vs[column] >> 16)) & pf_row_mask;
      nt = (uint32 *)&vram[ntbb + (((v_line >> 3) << pf_shift) & 0x1FC0)];
      v_line = (v_line & 7) << 3;
      atbuf = nt[index & pf_col_mask];

      {
        const uint8 *_s1 = &bg_pattern_cache[(atbuf & 0x1FFF0000) >> 10 | v_line];
        uint8x8_t _b1 = vorr_u8(vld1_u8(_s1), vdup_n_u8((uint8)((atbuf >> 25) & 0x70)));
        uint8x16_t _bx = vcombine_u8(_b0, _b1);
        uint8x16_t _ax = vld1q_u8(lb);
        vst1q_u8(lb, merge_bg_16_neon(_ax, _bx, (table != lut[0])));
        lb += 16;
      }
    }
#elif defined(LSB_FIRST)
  GET_LSB_TILE(atbuf, v_line)
  xscroll = READ_LONG(lb);
  yscroll = (src[0] | atex);
  DRAW_BG_TILE(xscroll, yscroll)
  xscroll = READ_LONG(lb);
  yscroll = (src[1] | atex);
  DRAW_BG_TILE(xscroll, yscroll)

  v_line = (line + v_offset + (vs[column] >> 16)) & pf_row_mask;
  nt = (uint32 *)&vram[ntbb + (((v_line >> 3) << pf_shift) & 0x1FC0)];
  v_line = (v_line & 7) << 3;
  atbuf = nt[index & pf_col_mask];

  GET_MSB_TILE(atbuf, v_line)
  xscroll = READ_LONG(lb);
  yscroll = (src[0] | atex);
  DRAW_BG_TILE(xscroll, yscroll)
  xscroll = READ_LONG(lb);
  yscroll = (src[1] | atex);
  DRAW_BG_TILE(xscroll, yscroll)
#else
  GET_MSB_TILE(atbuf, v_line)
  xscroll = READ_LONG(lb);
  yscroll = (src[0] | atex);
  DRAW_BG_TILE(xscroll, yscroll)
  xscroll = READ_LONG(lb);
  yscroll = (src[1] | atex);
  DRAW_BG_TILE(xscroll, yscroll)

  v_line = (line + vs[column]) & pf_row_mask;
  nt = (uint32 *)&vram[ntbb + (((v_line >> 3) << pf_shift) & 0x1FC0)];
  v_line = (v_line & 7) << 3;
  atbuf = nt[index & pf_col_mask];

  GET_LSB_TILE(atbuf, v_line)
  xscroll = READ_LONG(lb);
  yscroll = (src[0] | atex);
  DRAW_BG_TILE(xscroll, yscroll)
  xscroll = READ_LONG(lb);
  yscroll = (src[1] | atex);
  DRAW_BG_TILE(xscroll, yscroll)
#endif
  }
}

void render_bg_m5_im2(int line)
{
  int column, start, end, a, w;
  uint32 atex, atbuf, *src;
  uint8 *dst;
  uint32 shift, index, v_line, *nt;
  uint8 *lb;

  /* Scroll Planes common data */
  uint32 xscroll      = *(uint32 *)&vram[hscb + ((line & hscroll_mask) << 2)];
  uint32 yscroll      = *(uint32 *)&vsram[0];
  uint32 pf_col_mask  = playfield_col_mask;
  uint32 pf_row_mask  = (playfield_row_mask << 1) | 1;
  uint32 pf_shift     = playfield_shift;

  /* Number of columns to draw */
  int width = bitmap.viewport.w >> 4;

  /* Layer priority table */
  uint8 *table = lut[(reg[12] & 8) >> 2];

  /* Adjust line offset */
  line = (line << 1) + odd_frame;

  /* Window vertical range (cell 0-31) */
  a = (reg[18] & 0x1F) << 4;

  /* Window position (0=top, 1=bottom) */
  w = (reg[18] >> 7) & 1;

  /* Test against current line */
  if (w == (line >= a))
  {
    /* Window takes up entire line */
    a = 0;
    w = 1;
  }
  else
  {
    /* Window and Plane A share the line */
    a = clip[0].enable;
    w = clip[1].enable;
  }

  /* Plane A */
  if (a)
  {
    /* Plane A width */
    start = clip[0].left;
    end   = clip[0].right;

    /* Plane A scroll */
#ifdef LSB_FIRST
    shift  = (xscroll & 0x0F);
    index  = pf_col_mask + start + 1 - ((xscroll >> 4) & pf_col_mask);
    v_line = (line + yscroll) & pf_row_mask;
#else
    shift  = (xscroll >> 16) & 0x0F;
    index  = pf_col_mask + start + 1 - ((xscroll >> 20) & pf_col_mask);
    v_line = (line + (yscroll >> 16)) & pf_row_mask;
#endif

    /* Background line buffer */
    dst = &linebuf[0][0x20 + (start << 4) + shift];

    /* Plane A name table */
    nt = (uint32 *)&vram[ntab + (((v_line >> 4) << pf_shift) & 0x1FC0)];

    /* Pattern row index */
    v_line = (v_line & 15) << 3;

    if(shift)
    {
      /* Left-most column is partially shown */
      dst -= 16;

      /* Window bug */
      if (start)
      {
        atbuf = nt[index & pf_col_mask];
      }
      else
      {
        atbuf = nt[(index-1) & pf_col_mask];
      }

      DRAW_COLUMN_IM2(atbuf, v_line)
    }

    for(column = start; column < end; column++, index++)
    {
      atbuf = nt[index & pf_col_mask];
      DRAW_COLUMN_IM2(atbuf, v_line)
    }

    /* Window width */
    start = clip[1].left;
    end   = clip[1].right;
  }
  else
  {
    /* Window width */
    start = 0;
    end   = width;
  }

  /* Window Plane */
  if (w)
  {
    /* Background line buffer */
    dst = &linebuf[0][0x20 + (start << 4)];

    /* Window name table */
    nt = (uint32 *)&vram[ntwb | ((line >> 4) << (6 + (reg[12] & 1)))];

    /* Pattern row index */
    v_line = (line & 15) << 3;

    for(column = start; column < end; column++)
    {
      atbuf = nt[column];
      DRAW_COLUMN_IM2(atbuf, v_line)
    }
  }

  /* Plane B scroll */
#ifdef LSB_FIRST
  shift  = (xscroll >> 16) & 0x0F;
  index  = pf_col_mask + 1 - ((xscroll >> 20) & pf_col_mask);
  v_line = (line + (yscroll >> 16)) & pf_row_mask;
#else
  shift  = (xscroll & 0x0F);
  index  = pf_col_mask + 1 - ((xscroll >> 4) & pf_col_mask);
  v_line = (line + yscroll) & pf_row_mask;
#endif

  /* Plane B name table */
  nt = (uint32 *)&vram[ntbb + (((v_line >> 4) << pf_shift) & 0x1FC0)];

  /* Pattern row index */
  v_line = (v_line & 15) << 3;

  /* Background line buffer */
  lb = &linebuf[0][0x20];

  if(shift)
  {
    /* Left-most column is partially shown */
    lb -= (0x10 - shift);

    atbuf = nt[(index-1) & pf_col_mask];
    DRAW_BG_COLUMN_IM2(atbuf, v_line, xscroll, yscroll)
  }

  for(column = 0; column < width; column++, index++)
  {
    atbuf = nt[index & pf_col_mask];
    DRAW_BG_COLUMN_IM2(atbuf, v_line, xscroll, yscroll)
  }
}

void render_bg_m5_im2_vs(int line)
{
  int column, start, end, a, w;
  uint32 atex, atbuf, *src;
  uint8 *dst;
  uint32 shift, index, v_line, *nt;
  uint8 *lb;

  /* common data */
  uint32 xscroll      = *(uint32 *)&vram[hscb + ((line & hscroll_mask) << 2)];
  uint32 yscroll      = 0;
  uint32 pf_col_mask  = playfield_col_mask;
  uint32 pf_row_mask  = (playfield_row_mask << 1) | 1;
  uint32 pf_shift     = playfield_shift;
  uint32 *vs          = (uint32 *)&vsram[0];

  /* Number of columns to draw */
  int width = bitmap.viewport.w >> 4;

  /* Layer priority table */
  uint8 *table = lut[(reg[12] & 8) >> 2];

  /* Adjust line offset */
  line = (line << 1) + odd_frame;

  /* Window vertical range (cell 0-31) */
  a = (reg[18] & 0x1F) << 4;

  /* Window position (0=top, 1=bottom) */
  w = (reg[18] >> 7) & 1;

  /* Test against current line */
  if (w == (line >= a))
  {
    /* Window takes up entire line */
    a = 0;
    w = 1;
  }
  else
  {
    /* Window and Plane A share the line */
    a = clip[0].enable;
    w = clip[1].enable;
  }

  /* Left-most column vertical scrolling when partially shown horizontally (verified on PAL MD2)  */
  /* TODO: check on Genesis 3 models since it apparently behaves differently  */
  /* In H32 mode, vertical scrolling is disabled, in H40 mode, same value is used for both planes */
  /* See Formula One / Kawasaki Superbike Challenge (H32) & Gynoug / Cutie Suzuki no Ringside Angel (H40) */
  if (reg[12] & 1)
  {
    yscroll = vs[19] & (vs[19] >> 16);
  }

  /* Plane A */
  if (a)
  {
    /* Plane A width */
    start = clip[0].left;
    end   = clip[0].right;

    /* Plane A horizontal scroll */
#ifdef LSB_FIRST
    shift = (xscroll & 0x0F);
    index = pf_col_mask + start + 1 - ((xscroll >> 4) & pf_col_mask);
#else
    shift = (xscroll >> 16) & 0x0F;
    index = pf_col_mask + start + 1 - ((xscroll >> 20) & pf_col_mask);
#endif

    /* Background line buffer */
    dst = &linebuf[0][0x20 + (start << 4) + shift];

    if(shift)
    {
      /* Left-most column is partially shown */
      dst -= 16;

      /* Plane A vertical scroll */
      v_line = (line + yscroll) & pf_row_mask;

      /* Plane A name table */
      nt = (uint32 *)&vram[ntab + (((v_line >> 4) << pf_shift) & 0x1FC0)];

      /* Pattern row index */
      v_line = (v_line & 15) << 3;

      /* Window bug */
      if (start)
      {
        atbuf = nt[index & pf_col_mask];
      }
      else
      {
        atbuf = nt[(index-1) & pf_col_mask];
      }

      DRAW_COLUMN_IM2(atbuf, v_line)
    }

    for(column = start; column < end; column++, index++)
    {
      /* Plane A vertical scroll */
#ifdef LSB_FIRST
      v_line = (line + vs[column]) & pf_row_mask;
#else
      v_line = (line + (vs[column] >> 16)) & pf_row_mask;
#endif

      /* Plane A name table */
      nt = (uint32 *)&vram[ntab + (((v_line >> 4) << pf_shift) & 0x1FC0)];

      /* Pattern row index */
      v_line = (v_line & 15) << 3;

      atbuf = nt[index & pf_col_mask];
      DRAW_COLUMN_IM2(atbuf, v_line)
    }

    /* Window width */
    start = clip[1].left;
    end   = clip[1].right;
  }
  else
  {
    /* Window width */
    start = 0;
    end   = width;
  }

  /* Window Plane */
  if (w)
  {
    /* Background line buffer */
    dst = &linebuf[0][0x20 + (start << 4)];

    /* Window name table */
    nt = (uint32 *)&vram[ntwb | ((line >> 4) << (6 + (reg[12] & 1)))];

    /* Pattern row index */
    v_line = (line & 15) << 3;

    for(column = start; column < end; column++)
    {
      atbuf = nt[column];
      DRAW_COLUMN_IM2(atbuf, v_line)
    }
  }

  /* Plane B horizontal scroll */
#ifdef LSB_FIRST
  shift = (xscroll >> 16) & 0x0F;
  index = pf_col_mask + 1 - ((xscroll >> 20) & pf_col_mask);
#else
  shift = (xscroll & 0x0F);
  index = pf_col_mask + 1 - ((xscroll >> 4) & pf_col_mask);
#endif

  /* Background line buffer */
  lb = &linebuf[0][0x20];

  if(shift)
  {
    /* Left-most column is partially shown */
    lb -= (0x10 - shift);

    /* Plane B vertical scroll */
    v_line = (line + yscroll) & pf_row_mask;

    /* Plane B name table */
    nt = (uint32 *)&vram[ntbb + (((v_line >> 4) << pf_shift) & 0x1FC0)];

    /* Pattern row index */
    v_line = (v_line & 15) << 3;

    atbuf = nt[(index-1) & pf_col_mask];
    DRAW_BG_COLUMN_IM2(atbuf, v_line, xscroll, yscroll)
  }

  for(column = 0; column < width; column++, index++)
  {
    /* Plane B vertical scroll */
#ifdef LSB_FIRST
    v_line = (line + (vs[column] >> 16)) & pf_row_mask;
#else
    v_line = (line + vs[column]) & pf_row_mask;
#endif

    /* Plane B name table */
    nt = (uint32 *)&vram[ntbb + (((v_line >> 4) << pf_shift) & 0x1FC0)];

    /* Pattern row index */
    v_line = (v_line & 15) << 3;

    atbuf = nt[index & pf_col_mask];
    DRAW_BG_COLUMN_IM2(atbuf, v_line, xscroll, yscroll)
  }
}
#endif


/*--------------------------------------------------------------------------*/
/* Sprite layer rendering functions                                         */
/*--------------------------------------------------------------------------*/

void render_obj_tms(int line)
{
  int x, start, end;
  uint8 *lb, *sg;
  uint8 color, pattern[2];
  uint16 temp;

  /* Sprite list for current line */
  object_info_t *object_info = obj_info[line];
  int count = object_count[line];
  int ext_idx = 0, ext_status = -1;      /* VitAdhoc "no flicker" */

  /* Default sprite width (8 pixels) */
  int width = 8;

  /* Adjust width for 16x16 sprites */
  width <<= ((reg[1] & 0x02) >> 1);

  /* Adjust width for zoomed sprites */
  width <<= (reg[1] & 0x01);

  /* Latch SOVR flag from previous line to VDP status */
  status |= spr_ovr;

  /* Clear SOVR flag for current line */
  spr_ovr = 0;

  /* Draw sprites in front-to-back order */
  while (count--)
  {
    /* VitAdhoc "no flicker": from the fifth sprite on the hardware draws nothing, so what these do to the status is undone below */
    if (ext_idx++ == 4) ext_status = status;

    /* Sprite X position */
    start = object_info->xpos;

    /* Sprite Color + Early Clock bit */
    color = object_info->size;

    /* X position shift (32 pixels) */
    start -= ((color & 0x80) >> 2);

    /* Pointer to line buffer */
    lb = &linebuf[0][0x20 + start];

    if ((start + width) > 256)
    {
      /* Clip sprites on right edge */
      end = 256 - start;
      start = 0;
    }
    else
    {
      end = width;

      if (start < 0)
      {
        /* Clip sprites on left edge */
        start = 0 - start;
      }
      else
      {
        start = 0;
      }
    }

    /* Sprite Color (0-15) */
    color &= 0x0F;

    /* Sprite Pattern Name */
    temp = object_info->attr;

    /* Mask two LSB for 16x16 sprites */
    temp &= ~((reg[1] & 0x02) >> 0);
    temp &= ~((reg[1] & 0x02) >> 1);

    /* Pointer to sprite generator table */
    sg = (uint8 *)&vram[((reg[6] << 11) & 0x3800) | (temp << 3) | object_info->ypos];

    /* Sprite Pattern data (2 x 8 pixels) */
    pattern[0] = sg[0x00];
    pattern[1] = sg[0x10];

    if (reg[1] & 0x01)
    {
      /* Zoomed sprites are rendered at half speed */
      for (x=start; x<end; x+=2)
      {
        temp = pattern[(x >> 4) & 1];
        temp = (temp >> (7 - ((x >> 1) & 7))) & 0x01;
        temp = temp * color;
        temp |= (lb[x] << 8);
        lb[x] = lut[5][temp];
        status |= ((temp & 0x8000) >> 10);
        temp &= 0x00FF;
        temp |= (lb[x+1] << 8);
        lb[x+1] = lut[5][temp];
        status |= ((temp & 0x8000) >> 10);
      }
    }
    else
    {
      /* Normal sprites */
      for (x=start; x<end; x++)
      {
        temp = pattern[(x >> 3) & 1];
        temp = (temp >> (7 - (x & 7))) & 0x01;
        temp = temp * color;
        temp |= (lb[x] << 8);
        lb[x] = lut[5][temp];
        status |= ((temp & 0x8000) >> 10);
      }
    }

    /* Next sprite entry */
    object_info++;
  }
  if (ext_status >= 0) status = (uint16) ext_status;

  /* handle Game Gear reduced screen (160x144) */
  if ((system_hw == SYSTEM_GG) && !config.gg_extra && (v_counter < bitmap.viewport.h))
  {
    int line = v_counter - (bitmap.viewport.h - 144) / 2;
    if ((line < 0) || (line >= 144))
    {
      memset(&linebuf[0][0x20], 0x40, 256);
    }
    else
    {
      if (bitmap.viewport.x > 0)
      {
        memset(&linebuf[0][0x20], 0x40, 48);
        memset(&linebuf[0][0x20+48+160], 0x40, 48);
      }
    }
  }
}

void render_obj_m4(int line)
{
  int i, xpos, end;
  uint8 *src, *lb;
  uint16 temp;

  /* Sprite list for current line */
  object_info_t *object_info = obj_info[line];
  int count = object_count[line];
  /* VitAdhoc "no flicker": sprites beyond the hardware's 8 (parse_satb_m4 lists them only with config.no_sprite_limit) */
  int ext_idx = 0, ext_status = -1, ext_width;
  const int ext_real = (count > 8) ? 8 : count;
  uint16 ext_col = 0;

  /* Default sprite width */
  int width = 8;

  /* Sprite Generator address mask */
  uint16 sg_mask = ~0x1C0 ^ (reg[6] << 6);

  /* Zoomed sprites (not working on Genesis VDP) */
  if (system_hw < SYSTEM_MD)
  {
    width <<= (reg[1] & 0x01);
  }
  ext_width = width;

  /* Unused bits used as a mask on 315-5124 VDP only */
  if (system_hw > SYSTEM_SMS)
  {
    sg_mask |= 0xC0;
  }

  /* Latch SOVR flag from previous line to VDP status */
  status |= spr_ovr;

  /* Clear SOVR flag for current line */
  spr_ovr = 0;

  /* Draw sprites in front-to-back order */
  while (count--)
  {
    /* VitAdhoc "no flicker": from the ninth sprite on the hardware draws nothing, so what these do to the status (collision bit and
     * position) is undone below; they are drawn as the game asked for them (zoomed when zoom is on) */
    if (ext_idx == 8) { ext_status = status; ext_col = spr_col; width = ext_width; }

    /* 315-5124 VDP specific */
    if (system_hw <= SYSTEM_SMS)
    {
      /* last 4 sprites can not be zoomed */
      if ((ext_idx < ext_real) && (ext_idx >= ext_real - 4))
      {
        /* force default width for remaining sprites */
        width = 8;
      }
    }
    ext_idx++;

    /* Sprite pattern index */
    temp = (object_info->attr | 0x100) & sg_mask;

    /* Pointer to pattern cache line */
    src = (uint8 *)&bg_pattern_cache[(temp << 6) | (object_info->ypos << 3)];

    /* Sprite X position */
    xpos = object_info->xpos;

    /* X position shift */
    xpos -= (reg[0] & 0x08);

    if (xpos < 0)
    {
      /* Clip sprites on left edge */
      src = src - xpos;
      end = xpos + width;
      xpos = 0;
    }
    else if ((xpos + width) > 256)
    {
      /* Clip sprites on right edge */
      end = 256 - xpos;
    }
    else
    {
      /* Sprite maximal width */
      end = width;
    }

    /* Pointer to line buffer */
    lb = &linebuf[0][0x20 + xpos];

    if (width > 8)
    {
      /* Draw sprite pattern (zoomed sprites are rendered at half speed) */
      DRAW_SPRITE_TILE_ACCURATE_2X(end,0,lut[5])
    }
    else
    {
      /* Draw sprite pattern */
      DRAW_SPRITE_TILE_ACCURATE(end,0,lut[5])
    }

    /* Next sprite entry */
    object_info++;
  }
  if (ext_status >= 0) { status = (uint16) ext_status; spr_col = ext_col; }

  /* handle Game Gear reduced screen (160x144) */
  if ((system_hw == SYSTEM_GG) && !config.gg_extra && (v_counter < bitmap.viewport.h))
  {
    int line = v_counter - (bitmap.viewport.h - 144) / 2;
    if ((line < 0) || (line >= 144))
    {
      memset(&linebuf[0][0x20], 0x40, 256);
    }
    else
    {
      if (bitmap.viewport.x > 0)
      {
        memset(&linebuf[0][0x20], 0x40, 48);
        memset(&linebuf[0][0x20+48+160], 0x40, 48);
      }
    }
  }
}

void render_obj_m5(int line)
{
  int i, column;
  int xpos, width;
  int pixelcount = 0;
  int masked = 0;
  int max_pixels = MODE5_MAX_SPRITE_PIXELS;

  uint8 *src, *s, *lb;
  uint32 temp, v_line;
  uint32 attr, name, atex;

  /* Sprite list for current line */
  object_info_t *object_info = obj_info[line];
  int count = object_count[line];

  /* Draw sprites in front-to-back order */
  while (count--)
  {
    /* Sprite X position */
    xpos = object_info->xpos;

    /* Sprite masking  */
    if (xpos)
    {
      /* Requires at least one sprite with xpos > 0 */
      spr_ovr = 1;
    }
    else if (spr_ovr)
    {
      /* Remaining sprites are not drawn */
      masked = 1;
    }

    /* Display area offset */
    xpos = xpos - 0x80 + WIDE_SPR_DX;

    /* Sprite size */
    temp = object_info->size;

    /* Sprite width */
    width = 8 + ((temp & 0x0C) << 1);

    /* Update pixel count (off-screen sprites are included) */
    pixelcount += width;

    /* Is sprite across visible area ? */
    if (((xpos + width) > 0) && (xpos < WIDE_VW) && !masked)
    {
      /* Sprite attributes */
      attr = object_info->attr;

      /* Sprite vertical offset */
      v_line = object_info->ypos;

      /* Sprite priority + palette bits */
      atex = (attr >> 9) & 0x70;

      /* Pattern name base */
      name = attr & 0x07FF;

      /* Mask vflip/hflip */
      attr &= 0x1800;

      /* Pointer into pattern name offset look-up table */
      s = &name_lut[((attr >> 3) & 0x300) | (temp << 4) | ((v_line & 0x18) >> 1)];

      /* Pointer into line buffer */
      lb = &linebuf[0][0x20 + xpos];

      /* Max. number of sprite pixels rendered per line */
      if (pixelcount > max_pixels)
      {
        /* Adjust number of pixels to draw */
        width -= (pixelcount - max_pixels);
      }

      /* Number of tiles to draw */
      width = width >> 3;

      /* Pattern row index */
      v_line = (v_line & 7) << 3;

      /* Draw sprite patterns */
      for (column = 0; column < width; column++, lb+=8)
      {
        temp = attr | ((name + s[column]) & 0x07FF);
        src = &bg_pattern_cache[(temp << 6) | (v_line)];
        DRAW_SPRITE_TILE(8,atex,lut[1])
      }
    }

    /* Sprite limit */
    if (pixelcount >= max_pixels)
    {
      /* Sprite masking is effective on next line if max pixel width is reached */
      spr_ovr = (pixelcount >= bitmap.viewport.w);

      /* Stop sprite rendering */
      return;
    }

    /* Next sprite entry */
    object_info++;
  }

  /* Clear sprite masking for next line  */
  spr_ovr = 0;
}

void render_obj_m5_ste(int line)
{
  int i, column;
  int xpos, width;
  int pixelcount = 0;
  int masked = 0;
  int max_pixels = MODE5_MAX_SPRITE_PIXELS;

  uint8 *src, *s, *lb;
  uint32 temp, v_line;
  uint32 attr, name, atex;

  /* Sprite list for current line */
  object_info_t *object_info = obj_info[line];
  int count = object_count[line];

  /* Clear sprite line buffer */
  memset(&linebuf[1][0], 0, WIDE_VW + 0x40);

  /* Draw sprites in front-to-back order */
  while (count--)
  {
    /* Sprite X position */
    xpos = object_info->xpos;

    /* Sprite masking  */
    if (xpos)
    {
      /* Requires at least one sprite with xpos > 0 */
      spr_ovr = 1;
    }
    else if (spr_ovr)
    {
      /* Remaining sprites are not drawn */
      masked = 1;
    }

    /* Display area offset */
    xpos = xpos - 0x80 + WIDE_SPR_DX;

    /* Sprite size */
    temp = object_info->size;

    /* Sprite width */
    width = 8 + ((temp & 0x0C) << 1);

    /* Update pixel count (off-screen sprites are included) */
    pixelcount += width;

    /* Is sprite across visible area ? */
    if (((xpos + width) > 0) && (xpos < WIDE_VW) && !masked)
    {
      /* Sprite attributes */
      attr = object_info->attr;

      /* Sprite vertical offset */
      v_line = object_info->ypos;

      /* Sprite priority + palette bits */
      atex = (attr >> 9) & 0x70;

      /* Pattern name base */
      name = attr & 0x07FF;

      /* Mask vflip/hflip */
      attr &= 0x1800;

      /* Pointer into pattern name offset look-up table */
      s = &name_lut[((attr >> 3) & 0x300) | (temp << 4) | ((v_line & 0x18) >> 1)];

      /* Pointer into line buffer */
      lb = &linebuf[1][0x20 + xpos];

      /* Adjust number of pixels to draw for sprite limit */
      if (pixelcount > max_pixels)
      {
        width -= (pixelcount - max_pixels);
      }

      /* Number of tiles to draw */
      width = width >> 3;

      /* Pattern row index */
      v_line = (v_line & 7) << 3;

      /* Draw sprite patterns */
      for (column = 0; column < width; column++, lb+=8)
      {
        temp = attr | ((name + s[column]) & 0x07FF);
        src = &bg_pattern_cache[(temp << 6) | (v_line)];
        DRAW_SPRITE_TILE(8,atex,lut[3])
      }
    }

    /* Sprite limit */
    if (pixelcount >= max_pixels)
    {
      /* Sprite masking is effective on next line if max pixel width is reached */
      spr_ovr = (pixelcount >= bitmap.viewport.w);

      /* Merge background & sprite layers */
      merge(&linebuf[1][0x20], &linebuf[0][0x20], &linebuf[0][0x20], lut[4], WIDE_VW);

      /* Stop sprite rendering */
      return;
    }

    /* Next sprite entry */
    object_info++;
  }

  /* Clear sprite masking for next line  */
  spr_ovr = 0;

  /* Merge background & sprite layers */
  merge(&linebuf[1][0x20], &linebuf[0][0x20], &linebuf[0][0x20], lut[4], WIDE_VW);
}

void render_obj_m5_im2(int line)
{
  int i, column;
  int xpos, width;
  int pixelcount = 0;
  int masked = 0;
  int max_pixels = MODE5_MAX_SPRITE_PIXELS;

  uint8 *src, *s, *lb;
  uint32 temp, v_line;
  uint32 attr, name, atex;

  /* Sprite list for current line */
  object_info_t *object_info = obj_info[line];
  int count = object_count[line];

  /* Draw sprites in front-to-back order */
  while (count--)
  {
    /* Sprite X position */
    xpos = object_info->xpos;

    /* Sprite masking  */
    if (xpos)
    {
      /* Requires at least one sprite with xpos > 0 */
      spr_ovr = 1;
    }
    else if (spr_ovr)
    {
      /* Remaining sprites are not drawn */
      masked = 1;
    }

    /* Display area offset */
    xpos = xpos - 0x80;

    /* Sprite size */
    temp = object_info->size;

    /* Sprite width */
    width = 8 + ((temp & 0x0C) << 1);

    /* Update pixel count (off-screen sprites are included) */
    pixelcount += width;

    /* Is sprite across visible area ? */
    if (((xpos + width) > 0) && (xpos < bitmap.viewport.w) && !masked)
    {
      /* Sprite attributes */
      attr = object_info->attr;

      /* Sprite y offset */
      v_line = object_info->ypos;

      /* Sprite priority + palette bits */
      atex = (attr >> 9) & 0x70;

      /* Pattern name base */
      name = attr & 0x03FF;

      /* Mask vflip/hflip */
      attr &= 0x1800;

      /* Pattern name offset lookup table */
      s = &name_lut[((attr >> 3) & 0x300) | (temp << 4) | ((v_line & 0x30) >> 2)];

      /* Pointer into line buffer */
      lb = &linebuf[0][0x20 + xpos];

      /* Adjust width for sprite limit */
      if (pixelcount > max_pixels)
      {
        width -= (pixelcount - max_pixels);
      }

      /* Number of tiles to draw */
      width = width >> 3;

      /* Pattern row index */
      v_line = (v_line & 15) << 3;

      /* Render sprite patterns */
      for(column = 0; column < width; column ++, lb+=8)
      {
        temp = attr | (((name + s[column]) & 0x3ff) << 1);
        src = &bg_pattern_cache[((temp << 6) | (v_line)) ^ ((attr & 0x1000) >> 6)];
        DRAW_SPRITE_TILE(8,atex,lut[1])
      }
    }

    /* Sprite Limit */
    if (pixelcount >= max_pixels)
    {
      /* Sprite masking is effective on next line if max pixel width is reached */
      spr_ovr = (pixelcount >= bitmap.viewport.w);

      /* Stop sprite rendering */
      return;
    }

    /* Next sprite entry */
    object_info++;
  }

  /* Clear sprite masking for next line */
  spr_ovr = 0;
}

void render_obj_m5_im2_ste(int line)
{
  int i, column;
  int xpos, width;
  int pixelcount = 0;
  int masked = 0;
  int max_pixels = MODE5_MAX_SPRITE_PIXELS;

  uint8 *src, *s, *lb;
  uint32 temp, v_line;
  uint32 attr, name, atex;

  /* Sprite list for current line */
  object_info_t *object_info = obj_info[line];
  int count = object_count[line];

  /* Clear sprite line buffer */
  memset(&linebuf[1][0], 0, bitmap.viewport.w + 0x40);

  /* Draw sprites in front-to-back order */
  while (count--)
  {
    /* Sprite X position */
    xpos = object_info->xpos;

    /* Sprite masking  */
    if (xpos)
    {
      /* Requires at least one sprite with xpos > 0 */
      spr_ovr = 1;
    }
    else if (spr_ovr)
    {
      /* Remaining sprites are not drawn */
      masked = 1;
    }

    /* Display area offset */
    xpos = xpos - 0x80;

    /* Sprite size */
    temp = object_info->size;

    /* Sprite width */
    width = 8 + ((temp & 0x0C) << 1);

    /* Update pixel count (off-screen sprites are included) */
    pixelcount += width;

    /* Is sprite across visible area ? */
    if (((xpos + width) > 0) && (xpos < bitmap.viewport.w) && !masked)
    {
      /* Sprite attributes */
      attr = object_info->attr;

      /* Sprite y offset */
      v_line = object_info->ypos;

      /* Sprite priority + palette bits */
      atex = (attr >> 9) & 0x70;

      /* Pattern name base */
      name = attr & 0x03FF;

      /* Mask vflip/hflip */
      attr &= 0x1800;

      /* Pattern name offset lookup table */
      s = &name_lut[((attr >> 3) & 0x300) | (temp << 4) | ((v_line & 0x30) >> 2)];

      /* Pointer into line buffer */
      lb = &linebuf[1][0x20 + xpos];

      /* Adjust width for sprite limit */
      if (pixelcount > max_pixels)
      {
        width -= (pixelcount - max_pixels);
      }

      /* Number of tiles to draw */
      width = width >> 3;

      /* Pattern row index */
      v_line = (v_line & 15) << 3;

      /* Render sprite patterns */
      for(column = 0; column < width; column ++, lb+=8)
      {
        temp = attr | (((name + s[column]) & 0x3ff) << 1);
        src = &bg_pattern_cache[((temp << 6) | (v_line)) ^ ((attr & 0x1000) >> 6)];
        DRAW_SPRITE_TILE(8,atex,lut[3])
      }
    }

    /* Sprite Limit */
    if (pixelcount >= max_pixels)
    {
      /* Sprite masking is effective on next line if max pixel width is reached */
      spr_ovr = (pixelcount >= bitmap.viewport.w);

      /* Merge background & sprite layers */
      merge(&linebuf[1][0x20], &linebuf[0][0x20], &linebuf[0][0x20], lut[4], bitmap.viewport.w);

      /* Stop sprite rendering */
      return;
    }

    /* Next sprite entry */
    object_info++;
  }

  /* Clear sprite masking for next line */
  spr_ovr = 0;

  /* Merge background & sprite layers */
  merge(&linebuf[1][0x20], &linebuf[0][0x20], &linebuf[0][0x20], lut[4], bitmap.viewport.w);
}


/*--------------------------------------------------------------------------*/
/* Sprites Parsing functions                                                */
/*--------------------------------------------------------------------------*/

void parse_satb_tms(int line)
{
  int i = 0;

  /* Sprite counter (4 max. per line) */
  int count = 0;
  int ovr_seen = 0, ovr_i = 0;      /* VitAdhoc: see "Sprite overflow" below */

  /* no sprites in Text modes */
  if (!(reg[1] & 0x10))
  {
    /* Sprite Y position */
    int ypos;

    /* Sprite list for next line */
    object_info_t *object_info = obj_info[(line + 1) & 1];

    /* Pointer to sprite attribute table */
    uint8 *st = &vram[(reg[5] << 7) & 0x3F80];

    /* Sprite height (8 pixels by default) */
    int height = 8;

    /* Adjust height for 16x16 sprites */
    height <<= ((reg[1] & 0x02) >> 1);

    /* Parse Sprite Table (32 entries) */
    do
    {
      /* Read sprite Y position */
      ypos = st[i << 2];

      /* Check end of sprite list marker */
      if (ypos == 0xD0)
      {
        break;
      }

      /* Wrap Y coordinate for sprites > 256-32 */
      if (ypos >= 224)
      {
        ypos -= 256;
      }

      /* Y range */
      ypos = line - ypos;

      /* Adjust Y range for zoomed sprites */
      ypos >>= (reg[1] & 0x01);

      /* Sprite is visible on this line ? */
      if ((ypos >= 0) && (ypos < height))
      {
        /* Sprite overflow */
        /* VitAdhoc: the flag and the fifth sprite's number are set at the hardware's limit of 4 whatever the option; with "no flicker"
         * (config.no_sprite_limit) the sprites beyond it are still listed, for render_obj_tms to draw without touching the status */
        if ((count >= 4) && !ovr_seen)
        {
          /* Flag is set only during active area */
          if (line < bitmap.viewport.h)
          {
            spr_ovr = 0x40;
          }
          ovr_seen = 1;
          ovr_i = i;
          if (!config.no_sprite_limit) break;
        }

        /* Store sprite attributes for later processing */
        object_info->ypos = ypos;
        object_info->xpos = st[(i << 2) + 1];
        object_info->attr = st[(i << 2) + 2];
        object_info->size = st[(i << 2) + 3];

        /* Increment Sprite count */
        ++count;

        /* Next sprite entry */
        object_info++;
      }
    }
    while (++i < 32);
  }

  /* Update sprite count for next line */
  object_count[(line + 1) & 1] = count;

  /* Insert number of last sprite entry processed */
  if (ovr_seen) i = ovr_i;
  status = (status & 0xE0) | (i & 0x1F);
  tms_status_set = 1;      /* VitAdhoc: the VDP worker merges the low status bits back only when this ran */
}

void parse_satb_m4(int line)
{
  int i = 0;
  uint8 *st;

  /* Sprite counter (8 max. per line) */
  int count = 0;
  int ovr_seen = 0;                 /* VitAdhoc: see "Sprite overflow" below */

  /* Sprite Y position */
  int ypos;

  /* Sprite list for next line */
  object_info_t *object_info = obj_info[(line + 1) & 1];

  /* Sprite height (8x8 or 8x16) */
  int height = 8 + ((reg[1] & 0x02) << 2);

  /* Sprite attribute table address mask */
  uint16 st_mask = ~0x3F80 ^ (reg[5] << 7);

  /* Unused bits used as a mask on 315-5124 VDP only */
  if (system_hw > SYSTEM_SMS)
  {
    st_mask |= 0x80;
  }

  /* Pointer to sprite attribute table */
  st = &vram[st_mask & 0x3F00];

  /* Parse Sprite Table (64 entries) */
  do
  {
    /* Read sprite Y position */
    ypos = st[i];

    /* Check end of sprite list marker (no effect in extended modes) */
    if ((ypos == 208) && (bitmap.viewport.h == 192))
    {
      break;
    }

    /* Wrap Y coordinate (NB: this is likely not 100% accurate and needs to be verified on real hardware) */
    if (ypos > (bitmap.viewport.h + 16))
    {
      ypos -= 256;
    }

    /* Y range */
    ypos = line - ypos;

    /* Adjust Y range for zoomed sprites (not working on Mega Drive VDP) */
    if (system_hw < SYSTEM_MD)
    {
      ypos >>= (reg[1] & 0x01);
    }

    /* Check if sprite is visible on this line */
    if ((ypos >= 0) && (ypos < height))
    {
      /* Sprite overflow */
      /* VitAdhoc: the flag is set at the hardware's limit of 8 whatever the option; with "no flicker" (config.no_sprite_limit) the
       * sprites beyond it are still listed, for render_obj_m4 to draw without touching the status */
      if ((count >= 8) && !ovr_seen)
      {
        /* Flag is set only during active area */
        if ((line >= 0) && (line < bitmap.viewport.h))
        {
          spr_ovr = 0x40;
        }
        ovr_seen = 1;
        if (!config.no_sprite_limit) break;
      }

      /* Store sprite attributes for later processing */
      object_info->ypos = ypos;
      object_info->xpos = st[(0x80 + (i << 1)) & st_mask];
      object_info->attr = st[(0x81 + (i << 1)) & st_mask];

      /* 8x16 sprites pattern index LSB is masked */
      if (reg[1] & 0x02)
      {
        object_info->attr &= 0xfe;
      }

      /* Increment Sprite count */
      ++count;

      /* Next sprite entry */
      object_info++;
    }
  }
  while (++i < 64);

  /* Update sprite count for next line */
  object_count[(line + 1) & 1] = count;
}

void parse_satb_m5(int line)
{
  /* Sprite Y position */
  int ypos;

  /* Sprite height */
  int height;

  /* Sprite size data */
  int size;

  /* Sprite link data */
  int link = 0;

  /* Sprite counter */
  int count = 0;

  /* max. number of rendered sprites (16 or 20 sprites per line by default) */
  int max = MODE5_MAX_SPRITES_PER_LINE;

  /* max. number of parsed sprites (64 or 80 sprites per line by default) */
  int total = max_sprite_pixels >> 2;

  /* Pointer to sprite attribute table */
  uint16 *p = (uint16 *) &vram[satb];

  /* Pointer to internal RAM */
  uint16 *q = (uint16 *) &sat[0];

  /* Sprite list for next line */
  object_info_t *object_info = obj_info[(line + 1) & 1];

  /* Adjust line offset */
  line += 0x81;

  do
  {
    /* Read sprite Y position from internal SAT cache (9 bits) */
    ypos = q[link] & 0x1FF;

    /* Check if sprite Y position has been reached */
    if (line >= ypos)
    {
      /* Read sprite size from internal SAT cache */
      size = q[link + 1] >> 8;

      /* Sprite height (8, 16, 24 or 32 pixels) */
      height = 8 + ((size & 3) << 3);

      /* Y range */
      ypos = line - ypos;

      /* Check if sprite is visible on current line */
      if (ypos < height)
      {
        /* Sprite overflow */
        if (count == max)
        {
          status |= 0x40;
          break;
        }

        /* Update sprite list (only name, attribute & xpos are parsed from VRAM) */
        object_info->attr  = p[link + 2];
        object_info->xpos  = p[link + 3] & 0x1ff;
        object_info->ypos  = ypos;
        object_info->size  = size & 0x0f;

        /* Increment Sprite count */
        ++count;

        /* Next sprite entry */
        object_info++;
      }
    }

    /* Read link data from internal SAT cache */
    link = (q[link + 1] & 0x7F) << 2;

    /* Stop parsing if link data points to first entry (#0) or after the last entry (#64 in H32 mode, #80 in H40 mode) */
    if ((link == 0) || (link >= bitmap.viewport.w)) break;
  }
  while (--total);

  /* Update sprite count for next line (line value already incremented) */
  object_count[line & 1] = count;
}

void parse_satb_m5_im2(int line)
{
  /* Sprite Y position */
  int ypos;

  /* Sprite height */
  int height;

  /* Sprite size data */
  int size;

  /* Sprite link data */
  int link = 0;

  /* Sprite counter */
  int count = 0;

  /* max. number of rendered sprites (16 or 20 sprites per line by default) */
  int max = MODE5_MAX_SPRITES_PER_LINE;

  /* max. number of parsed sprites (64 or 80 sprites per line by default) */
  int total = max_sprite_pixels >> 2;

  /* Pointer to sprite attribute table */
  uint16 *p = (uint16 *) &vram[satb];

  /* Pointer to internal RAM */
  uint16 *q = (uint16 *) &sat[0];

  /* Sprite list for next line */
  object_info_t *object_info = obj_info[(line + 1) & 1];

  /* Adjust line offset */
  line = ((line + 0x81) << 1) + odd_frame;

  do
  {
    /* Read sprite Y position from internal SAT cache (10 bits) */
    ypos = q[link] & 0x3FF;

    /* Check if sprite Y position has been reached */
    if (line >= ypos)
    {
      /* Read sprite size from internal SAT cache */
      size = q[link + 1] >> 8;

      /* Sprite height (16, 32, 48 or 64 pixels) */
      height = 16 + ((size & 3) << 4);

      /* Y range */
      ypos = line - ypos;

      /* Check if sprite is visible on current line */
      if (ypos < height)
      {
        /* Sprite overflow */
        if (count == max)
        {
          status |= 0x40;
          break;
        }

        /* Update sprite list (only name, attribute & xpos are parsed from VRAM) */
        object_info->attr  = p[link + 2];
        object_info->xpos  = p[link + 3] & 0x1ff;
        object_info->ypos  = ypos;
        object_info->size  = size & 0x0f;

        /* Increment Sprite count */
        ++count;

        /* Next sprite entry */
        object_info++;
      }
    }

    /* Read link data from internal SAT cache */
    link = (q[link + 1] & 0x7F) << 2;

    /* Stop parsing if link data points to first entry (#0) or after the last entry (#64 in H32 mode, #80 in H40 mode) */
    if ((link == 0) || (link >= bitmap.viewport.w)) break;
  }
  while (--total);

  /* Update sprite count for next line (line value already incremented) */
  object_count[(line >> 1) & 1] = count;
}


/*--------------------------------------------------------------------------*/
/* Pattern cache update function                                            */
/*--------------------------------------------------------------------------*/

void update_bg_pattern_cache_m4(int index)
{
  int i;
  uint8 x, y, c;
  uint8 *dst;
  uint16 name, bp01, bp23;
  uint32 bp;

  for(i = 0; i < index; i++)
  {
    /* Get modified pattern name index */
    name = bg_name_list[i];

    /* Pattern cache base address */
    dst = &bg_pattern_cache[name << 6];

    /* Check modified lines */
    for(y = 0; y < 8; y++)
    {
      if(bg_name_dirty[name] & (1 << y))
      {
        /* Byteplane data */
        bp01 = *(uint16 *)&vram[(name << 5) | (y << 2) | (0)];
        bp23 = *(uint16 *)&vram[(name << 5) | (y << 2) | (2)];

        /* Convert to pixel line data (4 bytes = 8 pixels)*/
        /* (msb) p7p6 p5p4 p3p2 p1p0 (lsb) */
        bp = (bp_lut[bp01] >> 2) | (bp_lut[bp23]);

        /* Update cached line (8 pixels = 8 bytes) */
        for(x = 0; x < 8; x++)
        {
          /* Extract pixel data */
          c = bp & 0x0F;

          /* Pattern cache data (one pattern = 8 bytes) */
          /* byte0 <-> p0 p1 p2 p3 p4 p5 p6 p7 <-> byte7 (hflip = 0) */
          /* byte0 <-> p7 p6 p5 p4 p3 p2 p1 p0 <-> byte7 (hflip = 1) */
          dst[0x00000 | (y << 3) | (x)] = (c);            /* vflip=0 & hflip=0 */
          dst[0x08000 | (y << 3) | (x ^ 7)] = (c);        /* vflip=0 & hflip=1 */
          dst[0x10000 | ((y ^ 7) << 3) | (x)] = (c);      /* vflip=1 & hflip=0 */
          dst[0x18000 | ((y ^ 7) << 3) | (x ^ 7)] = (c);  /* vflip=1 & hflip=1 */

          /* Next pixel */
          bp = bp >> 4;
        }
      }
    }

    /* Clear modified pattern flag */
    bg_name_dirty[name] = 0;
  }
}

void update_bg_pattern_cache_m5(int index)
{
  int i;
  uint8 y, dirty;
  uint8 *dst;
  uint16 name;
  uint32 bp;

  for(i = 0; i < index; i++)
  {
    /* Get modified pattern name index */
    name = bg_name_list[i];

    /* Pattern cache base address (8-byte aligned) */
    dst = &bg_pattern_cache[name << 6];
    dirty = bg_name_dirty[name];

    /* Check modified lines */
    for(y = 0; y < 8; y++)
    {
      if(dirty & (1 << y))
      {
        uint32 y0 = (uint32)y << 3;
        uint32 y1 = (uint32)(y ^ 7) << 3;
        uint64_t norm, hflip;

        /* Byteplane data (one pattern = 4 bytes) */
        /* LIT_ENDIAN: byte0 (lsb) p2p3 p0p1 p6p7 p4p5 (msb) byte3 */
        /* BIG_ENDIAN: byte0 (msb) p0p1 p2p3 p4p5 p6p7 (lsb) byte3 */
        bp = *(uint32 *)&vram[(name << 5) | (y << 2)];

        /* Pack all 8 pixels into two 64-bit words (hflip=0, hflip=1) and
         * emit 4 aligned 64-bit stores instead of 32 strided byte stores. */
#ifdef LSB_FIRST
        {
          uint32 bp_hi = bp >> 16;
          uint32 lo = ((bp >> 12) & 0x0F) | (bp & 0x0F00) | ((bp & 0x00F0) << 12) | ((bp & 0x000F) << 24);
          uint32 hi = ((bp_hi >> 12) & 0x0F) | (bp_hi & 0x0F00) | ((bp_hi & 0x00F0) << 12) | ((bp_hi & 0x000F) << 24);
          uint32 hlo, hhi;
#if defined(__GNUC__)
          hlo = __builtin_bswap32(hi);
          hhi = __builtin_bswap32(lo);
#else
          hlo = ((hi & 0xFF) << 24) | ((hi & 0xFF00) << 8) | ((hi >> 8) & 0xFF00) | (hi >> 24);
          hhi = ((lo & 0xFF) << 24) | ((lo & 0xFF00) << 8) | ((lo >> 8) & 0xFF00) | (lo >> 24);
#endif
          norm  = (uint64_t)lo  | ((uint64_t)hi  << 32);
          hflip = (uint64_t)hlo | ((uint64_t)hhi << 32);
        }
#else
        {
          uint8 nbuf[8], hbuf[8];
          uint8 x;
          for (x = 0; x < 8; x++)
          {
            uint8 c = (uint8)(bp & 0x0F);
            nbuf[x ^ 7] = c;
            hbuf[x] = c;
            bp >>= 4;
          }
          memcpy(&norm, nbuf, 8);
          memcpy(&hflip, hbuf, 8);
        }
#endif
        memcpy(dst + (0x00000 | y0), &norm, 8);   /* vflip=0, hflip=0 */
        memcpy(dst + (0x20000 | y0), &hflip, 8);  /* vflip=0, hflip=1 */
        memcpy(dst + (0x40000 | y1), &norm, 8);   /* vflip=1, hflip=0 */
        memcpy(dst + (0x60000 | y1), &hflip, 8);  /* vflip=1, hflip=1 */
      }
    }

    /* Clear modified pattern flag */
    bg_name_dirty[name] = 0;
  }
}


/*--------------------------------------------------------------------------*/
/* Window & Plane A clipping update function (Mode 5)                       */
/*--------------------------------------------------------------------------*/

void window_clip(unsigned int data, unsigned int sw)
{
  /* Window size and invert flags */
  int hp = (data & 0x1f);
  int hf = (data >> 7) & 1;

  /* Perform horizontal clipping; the results are applied in reverse
     if the horizontal inversion flag is set
   */
  int a = hf;
  int w = hf ^ 1;

  /* Display width (16 or 20 columns) */
  sw = 16 + (sw << 2);

  if(hp)
  {
    if(hp > sw)
    {
      /* Plane W takes up entire line */
      clip[w].left = 0;
      clip[w].right = sw;
      clip[w].enable = 1;
      clip[a].enable = 0;
    }
    else
    {
      /* Plane W takes left side, Plane A takes right side */
      clip[w].left = 0;
      clip[a].right = sw;
      clip[a].left = clip[w].right = hp;
      clip[0].enable = clip[1].enable = 1;
    }
  }
  else
  {
    /* Plane A takes up entire line */
    clip[a].left = 0;
    clip[a].right = sw;
    clip[a].enable = 1;
    clip[w].enable = 0;
  }
}


/*--------------------------------------------------------------------------*/
/* Init, reset routines                                                     */
/*--------------------------------------------------------------------------*/

void render_init(void)
{
  int bx, ax;

  /* VitAdhoc: what a new process starts with (see gen_init in genesis.c): the sprite lists and pending flags of the last game */
  memset(obj_info, 0, sizeof(obj_info));
  object_count[0] = object_count[1] = 0;
  spr_ovr = 0; spr_col = 0; tms_status_set = 0;

  /* Initialize layers priority pixel look-up tables */
  uint16 index;
  for (bx = 0; bx < 0x100; bx++)
  {
    for (ax = 0; ax < 0x100; ax++)
    {
      index = (bx << 8) | (ax);

      lut[0][index] = make_lut_bg(bx, ax);
      lut[1][index] = make_lut_bgobj(bx, ax);
      lut[2][index] = make_lut_bg_ste(bx, ax);
      lut[3][index] = make_lut_obj(bx, ax);
      lut[4][index] = make_lut_bgobj_ste(bx, ax);
      lut[5][index] = make_lut_bgobj_m4(bx,ax);
    }
  }

  /* Initialize pixel color look-up tables */
  palette_init();

  /* Make sprite pattern name index look-up table (Mode 5) */
  make_name_lut();

  /* Make bitplane to pixel look-up table (Mode 4) */
  make_bp_lut();
}

void render_reset(void)
{
  if (!reset_do_not_clear_buffers)
  {
    /* Clear display bitmap */
    memset(bitmap.data, 0, bitmap.pitch * (bitmap.height - 1) + bitmap.width * 2);   /* VitAdhoc: the last row only as wide as a picture (a wide canvas's buffer starts inside the allocation) */

    /* Clear line buffers */
    memset(linebuf, 0, sizeof(linebuf));

    /* Clear color palettes */
    memset(pixel, 0, sizeof(pixel));

    /* Clear pattern cache */
    memset((char *)bg_pattern_cache, 0, sizeof(bg_pattern_cache));
  }

  /* Reset Sprite infos */
  spr_ovr = spr_col = object_count[0] = object_count[1] = 0;
  wc_last_line = 999;
  wc_hits = wc_lines = wc_low_streak = wc_cool = wc_prime = 0;
}


/*--------------------------------------------------------------------------*/
/* Line rendering functions                                                 */
/*--------------------------------------------------------------------------*/

/* VitAdhoc wide view: learned rows. The host gives name table rows of plane A (0) / B (1) with cells the game has shown before at the world
 * columns now beside the screen; the strip pass reads such a row instead of the VRAM row for as long as the VRAM row is byte for byte what it
 * was when the row was given (a game that writes the row gets its own row back at once). `addr` is the row's VRAM address, n its cells. */
void wide_rows_clear(void)
{
  if (wide_ovany) memset(wide_ovh, 0, sizeof wide_ovh);
  wide_ovany = 0;
}

void wide_row_set(int plane, int row, unsigned int addr, const uint16 *words, int n)
{
  if (plane < 0 || plane > 1 || row < 0 || row > 127 || n < 32 || n > 128 || (row + 1) * n > 4096 || addr + (unsigned int) n * 2 > 0x10000) return;
  memcpy(&wide_ovr[plane][row * n], words, (size_t) n * 2);
  memcpy(&wide_ovo[plane][row * n], &vram[addr], (size_t) n * 2);
  wide_ovh[plane][row] = 1;
  wide_ovn = n;
  wide_ovany = 1;
  wide_gen++;
}

/* VitAdhoc wide view: what the host needs to know about this line, and (when the line allows it) the strip of picture beside it. Called
 * after the line was drawn and remapped; draws nothing the screen shows. */
static void wide_line(int line, int spr_in)
{
  wide_line_t *r;
  uint16 *vs16 = (uint16 *)vsram;
  uint32 *vs32 = (uint32 *)vsram;
  int h40 = (reg[12] & 1), a, w, i, side;
  int mode[2];                 /* per side: 0 no strip, 1 the planes and the sprites, 2 without plane A (the window plane takes that side) */
  int vse = 0;
  const int W = bitmap.viewport.w;

  if (line < 0 || line > 255) return;
  r = &wide_rec[line];
  memset(r, 0, sizeof *r);
  if (system_hw < SYSTEM_MD)
  {
    /* the 8 bit consoles: the palette the frame began with, and whether this line still has it */
    if (line == 0) memcpy(wide_pal, pixel, sizeof wide_pal);
    else if (memcmp(wide_pal, pixel, sizeof wide_pal)) r->side |= WS_PAL;
  }
  if (system_hw >= SYSTEM_MARKIII && system_hw < SYSTEM_MD)
  {
    /* Master System / Game Gear mode 4 (the Game Gear wide view and the learned picture need what the line scrolled by); no strip pass here */
    if ((reg[1] & 0x40) && (reg[0] & 0x04))
    {
      r->flags = WL_ON | WL_M4;
      r->xs[0] = (uint16) (((reg[0] & 0x40) && (line < 0x10)) ? 0 : reg[0x08]);
      r->ys[0] = (uint16) vscroll;
      r->ntab = (uint16) ((~0x3C00 ^ (reg[2] << 10)) | (system_hw > SYSTEM_SMS ? 0x400 : 0));   /* the name table mask render_bg_m4 uses */
      r->col_mask = 31;
      if (reg[0] & 0x80) r->flags |= WL_VSCROLL;               /* the right eight columns do not scroll vertically */
    }
    return;
  }
  if (!(reg[1] & 0x40) || !(reg[1] & 0x04) || (system_hw & SYSTEM_PBC) != SYSTEM_MD) return;     /* blank, or not mode 5 */
  r->flags = WL_ON | (h40 ? WL_H40 : 0);
  r->col_mask = (uint8) playfield_col_mask; r->shift = (uint8) playfield_shift; r->row_mask = (uint16) playfield_row_mask;
  r->ntab = (uint16) ntab; r->ntbb = (uint16) ntbb; r->ntwb = (uint16) ntwb;
  {
    uint32 xs = *(uint32 *)&vram[hscb + ((line & hscroll_mask) << 2)];
#ifdef LSB_FIRST
    r->xs[0] = (uint16) xs; r->xs[1] = (uint16) (xs >> 16);
#else
    r->xs[0] = (uint16) (xs >> 16); r->xs[1] = (uint16) xs;
#endif
  }
  /* the vertical scroll of the first columns; per 2-cell scrolling is judged below */
  r->ys[0] = (uint16) (vs16[0] & 0x7FF); r->ys[1] = (uint16) (vs16[1] & 0x7FF);
  r->ysr[0] = r->ys[0]; r->ysr[1] = r->ys[1];
  if (reg[11] & 0x04) { const int lc = ((h40 ? 40 : 32) >> 1) - 1; r->ysr[0] = (uint16) (vs16[lc * 2] & 0x7FF); r->ysr[1] = (uint16) (vs16[lc * 2 + 1] & 0x7FF); }
  mode[0] = mode[1] = 1;
  if (reg[12] & 0x02) { r->flags |= WL_IM2; mode[0] = mode[1] = 0; }          /* interlaced (mode 1 draws one field, mode 2 two): no strip */
  if (reg[11] & 0x04)
  {
    /* per 2-cell vertical scroll. While every column scrolls alike (and both planes: the left-most column of the H40 renderer reads the two
     * planes' values ANDed) the strip is the plain one. Else a side gets its strip only where the three columns next to that edge scroll
     * alike (and, on the left, the partly shown column too): the strip then scrolls like the column at the edge. */
    const int n = (h40 ? 40 : 32), nc = n >> 1;
    int uni = 1;
    for (i = 0; i < n; i++) if ((vs16[i] & 0x7FF) != (vs16[0] & 0x7FF)) { uni = 0; break; }
    if (!uni)
    {
      const uint32 km = 0x07FF07FF, rm = (uint32) playfield_row_mask;
      r->flags |= WL_VSCROLL;
      if (render_bg != render_bg_m5_vs) mode[0] = mode[1] = 0;
      else
      {
        const uint32 e0 = vs32[0] & km, e1 = vs32[nc - 1] & km;
        const uint32 q = h40 ? (vs32[19] & (vs32[19] >> 16)) : 0;                /* what the partly shown left-most column scrolls by */
        if ((vs32[1] & km) != e0 || (vs32[2] & km) != e0) mode[0] = 0;
        else
        {
          if ((r->xs[0] & 15) && ((q ^ vs16[0]) & rm)) mode[0] = 0;
          if ((r->xs[1] & 15) && ((q ^ vs16[1]) & rm)) mode[0] = 0;
        }
        if ((vs32[nc - 2] & km) != e1 || (vs32[nc - 3] & km) != e1) mode[1] = 0;
        vse = 1;
      }
    }
    else if (render_bg == render_bg_m5_vs) vse = 2;       /* uniform: the same pass, the edge column's values */
  }
  /* the window plane is not continued sideways: the side of the line it takes gets a strip without plane A (plane B and the sprites go on
   * there), and only where the window plane's two cells at that edge are empty (a status panel that reaches the edge ends in its own colour) */
  a = (reg[18] & 0x1F) << 3;
  w = (reg[18] >> 7) & 1;
  {
    const int full = (w == (line >= a));
    int noa[2];
    noa[0] = noa[1] = 0;
    if (full || !clip[0].enable) { noa[0] = noa[1] = 1; r->flags |= WL_WFULL; }
    else if (clip[1].enable) { if (clip[0].left == 0) noa[1] = 1; else noa[0] = 1; }
    if (noa[0] | noa[1])
    {
      const uint16 *nt = (const uint16 *)&vram[ntwb | ((line >> 3) << (6 + (reg[12] & 1)))];
      r->flags |= WL_WINDOW;
      for (side = 0; side < 2; side++)
      {
        int c, k, clear = 1;
        if (!noa[side] || !mode[side]) continue;
        c = side == 0 ? 0 : (W >> 3) - 2;
        for (k = 0; k < 2 && clear; k++)
        {
          const uint32 *pt = (const uint32 *)&vram[(nt[c + k] & 0x7FF) << 5];
          if (pt[0] | pt[1] | pt[2] | pt[3] | pt[4] | pt[5] | pt[6] | pt[7]) clear = 0;
        }
        mode[side] = clear ? 2 : 0;
      }
    }
  }
  if (!(mode[0] | mode[1]) || wide_m <= 0 || !bitmap.data || bitmap.viewport.x != 0 || bitmap.viewport.y != 0 || interlaced || !render_bg || !render_obj) return;

  {
    PIXEL_OUT_T *row = (PIXEL_OUT_T *)&bitmap.data[line * bitmap.pitch];
    wide_cache_t *ce = NULL;
    int hit = 0, sidebits = 0;
    const int cols = (wide_m + 15) >> 4;
    PIXEL_OUT_T fresh[2][96];
    uint32 fopq[2][3];

    /* learned rows, plain renderer only: the row each plane shows on this line, if the host gave one and the VRAM row is unchanged */
    wide_ont[0] = wide_ont[1] = NULL;
    if (wide_ovany && render_bg == render_bg_m5)
    {
      for (i = 0; i < 2; i++)
      {
        const unsigned int vr = (((unsigned int) line + (vs16[i] & 0x7FF)) & playfield_row_mask) >> 3;
        const unsigned int addr = (i == 0 ? ntab : ntbb) + ((vr << playfield_shift) & 0x1FC0);
        if (vr < 128 && wide_ovh[i][vr] && (vr + 1) * (unsigned int) wide_ovn <= 4096 && addr + (unsigned int) wide_ovn * 2 <= 0x10000 &&
            !memcmp(&vram[addr], &wide_ovo[i][vr * wide_ovn], (size_t) wide_ovn * 2))
        {
          wide_ont[i] = (uint32 *)&wide_ovr[i][vr * wide_ovn];
          sidebits |= (i == 0 ? WS_OVR_A : WS_OVR_B);
        }
      }
    }
    if (vse) sidebits |= (vse == 1 ? WS_VSE : 0);
    for (side = 0; side < 2; side++)
    {
      if (mode[side]) sidebits |= (side == 0 ? WS_L : WS_R);
      if (mode[side] == 2) sidebits |= (side == 0 ? WS_NOA_L : WS_NOA_R);
    }

    if (wide_cache_on)
    {
      if (line < wc_last_line)
      {
        if (wide_cache_check) { wc_cool = wc_low_streak = wc_prime = 0; }
        else if (wc_cool > 0) { wc_cool--; }
        else if (wc_prime) { wc_prime = 0; }
        else if (wc_lines > 0)
        {
          if (wc_hits * 10 < wc_lines)
          {
            if (++wc_low_streak >= 8) { wc_cool = 30; wc_low_streak = 0; wc_prime = 1; }
          }
          else wc_low_streak = 0;
        }
        wc_hits = 0; wc_lines = 0;
      }
      wc_last_line = line;
      if (wide_cache_check || wc_cool == 0)
      {
        if (!wide_cache) wide_cache = (wide_cache_t *) calloc(256, sizeof(wide_cache_t));
        ce = wide_cache ? &wide_cache[line] : NULL;
        if (ce && ce->ok && ce->gen == wide_gen && wide_parse_gen == wide_gen && ce->spr_in == (uint8) spr_in && ce->side == (uint8) sidebits) hit = 1;
        wc_lines++;
        if (hit) wc_hits++;
      }
    }
    if (hit && !wide_cache_check)
    {
      for (side = 0; side < 2; side++)
      {
        if (!mode[side]) continue;
        memcpy(side == 0 ? row - wide_m : row + W, ce->px[side], (size_t) wide_m * sizeof(PIXEL_OUT_T));
        memcpy(wide_opq[line][side], ce->opq[side], sizeof ce->opq[side]);
      }
      wide_cache_stat[0]++;
    }
    else
    {
      const uint16 st = status, sc = spr_col;
      const uint8 so = spr_ovr;
      const int save_n = 0x20 + (cols << 4) + 48;
      uint8 lbsave[2][0x200];
      memcpy(lbsave[0], linebuf[0], (size_t) save_n);
      memcpy(lbsave[1], linebuf[1], (size_t) save_n);
      for (side = 0; side < 2; side++)
      {
        uint8 *src;
        uint32 *ob = fopq[side];
        PIXEL_OUT_T *d = fresh[side];
        int all;
        if (!mode[side]) continue;
        ob[0] = ob[1] = ob[2] = 0;
        /* nothing to draw: no plane goes into this strip and no sprite is on the line */
        if (!((wide_planes & (mode[side] == 2 ? 2 : 3))) && object_count[line & 1] == 0)
        {
          memset(d, 0, (size_t) wide_m * sizeof(PIXEL_OUT_T));
          continue;
        }
        wide_cols = cols; wide_px = cols << 4;
        wide_dx = side == 0 ? wide_px : -W;
        wide_a = (mode[side] == 1);
        if (vse)
        {
          const uint32 e = vs32[side == 0 ? 0 : ((h40 ? 40 : 32) >> 1) - 1];
          for (i = 0; i < 20; i++) wide_vsb[i] = e;
#ifdef LSB_FIRST
          wide_ys[0] = e & 0xFFFF; wide_ys[1] = e >> 16;
#else
          wide_ys[0] = e >> 16; wide_ys[1] = e & 0xFFFF;
#endif
        }
        wide_pass = 1;
        spr_ovr = (uint8) spr_in;
        render_bg(line);
        render_obj(line & 1);
        wide_pass = 0;
        src = &linebuf[0][0x20];
        /* a strip with every plane the line has is opaque everywhere; with a plane left out a pixel nothing drew is transparent */
        all = mode[side] == 2 ? ((wide_planes & 2) != 0) : ((wide_planes & 3) == 3);
        if (all)
        {
          if (side == 0) { const uint8 *p = src + wide_px - wide_m; for (i = 0; i < wide_m; i++) d[i] = pixel[p[i]]; }
          else for (i = 0; i < wide_m; i++) d[i] = pixel[src[i]];
          for (i = 0; i < wide_m; i += 32) ob[i >> 5] = (wide_m - i >= 32) ? 0xFFFFFFFFu : ((1u << (wide_m - i)) - 1u);
        }
        else
        {
          /* bit k of the mask: the pixel k pixels from the screen's edge was drawn by a plane or a sprite (colour index not 0); four pixels a step */
          static const uint8 rev4[16] = { 0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15 };
          const uint8 *p = side == 0 ? src + wide_px - wide_m : src;
          int k;
          for (i = 0; i < wide_m; i++) d[i] = pixel[p[i]];
          for (k = 0; k < wide_m; k += 4)
          {
            /* the four pixels at distances k .. k + 3 (the line buffer has room beyond the strip on both sides) */
            uint32 x, t;
            memcpy(&x, side == 0 ? src + wide_px - 4 - k : src + k, 4);
#ifndef LSB_FIRST
            x = (x >> 24) | ((x >> 8) & 0xFF00) | ((x << 8) & 0xFF0000) | (x << 24);
#endif
            t = ((((x & 0x0F0F0F0F) + 0x7F7F7F7F) & 0x80808080) >> 7) * 0x01020408u >> 24;
            if (side == 0) t = rev4[t & 15];
            if (wide_m - k < 4) t &= (1u << (wide_m - k)) - 1u;
            ob[k >> 5] |= (t & 15) << (k & 31);
          }
        }
      }
      status = st; spr_col = sc; spr_ovr = so;
      memcpy(linebuf[0], lbsave[0], (size_t) save_n);
      memcpy(linebuf[1], lbsave[1], (size_t) save_n);
      if (hit)
      {
        /* check mode: the copy the cache would have given against the fresh draw */
        for (side = 0; side < 2; side++)
          if (mode[side] && (memcmp(ce->px[side], fresh[side], (size_t) wide_m * sizeof(PIXEL_OUT_T)) || memcmp(ce->opq[side], fopq[side], sizeof fopq[side])))
            { wide_cache_stat[2]++; break; }
        wide_cache_stat[0]++;
      }
      else wide_cache_stat[1]++;
      for (side = 0; side < 2; side++)
      {
        if (!mode[side]) continue;
        memcpy(side == 0 ? row - wide_m : row + W, fresh[side], (size_t) wide_m * sizeof(PIXEL_OUT_T));
        memcpy(wide_opq[line][side], fopq[side], sizeof fopq[side]);
        if (ce) { memcpy(ce->px[side], fresh[side], (size_t) wide_m * sizeof(PIXEL_OUT_T)); memcpy(ce->opq[side], fopq[side], sizeof fopq[side]); }
      }
      if (ce)
      {
        ce->ok = (uint8) (wide_parse_gen == wide_gen);
        ce->gen = wide_gen; ce->spr_in = (uint8) spr_in; ce->side = (uint8) sidebits;
      }
    }
    wide_ont[0] = wide_ont[1] = NULL;
    r->side = (uint8) sidebits;
  }
  r->flags |= WL_STRIP;
}

void render_line(int line)
{
  const int wide_spr_in = spr_ovr;
  /* VitAdhoc: a frame that is not shown (vdp_thread.h, VDP_LINE_STATUS_ONLY): the sprite pass alone, for the status bits it sets */
  if (line >= 0 && (line & 0x10000))
  {
    line &= 0xFFFF;
    if (reg[1] & 0x40)
    {
      if (bg_list_index)
      {
        update_bg_pattern_cache(bg_list_index);
        bg_list_index = 0;
      }
      /* the background's pixels never carry the sprite marker (bit 7): an empty line gives the same collisions */
      memset(&linebuf[0][0], 0, sizeof(linebuf[0]));
      render_obj(line & 1);
      if (line < (bitmap.viewport.h - 1))
      {
        parse_satb(line);
        wide_parse_gen = wide_gen;
      }
    }
    else if (system_hw < SYSTEM_MD)
    {
      status |= spr_ovr;
      spr_ovr = 0;
      parse_satb(line);
    }
    return;
  }
  /* Check display status */
  if (reg[1] & 0x40)
  {
    /* Update pattern cache */
    if (bg_list_index)
    {
      update_bg_pattern_cache(bg_list_index);
      bg_list_index = 0;
    }

    /* Render BG layer(s) */
    render_bg(line);

    /* Render sprite layer */
    render_obj(line & 1);

    /* Left-most column blanking */
    if (reg[0] & 0x20)
    {
      if (system_hw >= SYSTEM_MARKIII)
      {
        memset(&linebuf[0][0x20], 0x40, 8);
      }
    }

    /* Parse sprites for next line */
    if (line < (bitmap.viewport.h - 1))
    {
      parse_satb(line);
      wide_parse_gen = wide_gen;
    }

    /* Horizontal borders */
    if (bitmap.viewport.x > 0)
    {
      memset(&linebuf[0][0x20 - bitmap.viewport.x], 0x40, bitmap.viewport.x);
      memset(&linebuf[0][0x20 + bitmap.viewport.w], 0x40, bitmap.viewport.x);
    }
  }
  else
  {
    /* Master System & Game Gear VDP specific */
    if (system_hw < SYSTEM_MD)
    {
      /* Update SOVR flag */
      status |= spr_ovr;
      spr_ovr = 0;

      /* Sprites are still parsed when display is disabled */
      parse_satb(line);
    }

    /* Blanked line */
    memset(&linebuf[0][0x20 - bitmap.viewport.x], 0x40, bitmap.viewport.w + 2*bitmap.viewport.x);
  }

  /* Pixel color remapping */
  remap_line(line);

  /* VitAdhoc wide view */
  if (wide_m != 0) wide_line(line, wide_spr_in);
}

void blank_line(int line, int offset, int width)
{
  memset(&linebuf[0][0x20 + offset], 0x40, width);
  remap_line(line);
}

void remap_line(int line)
{
  /* Line width */
  int width = bitmap.viewport.w + 2*bitmap.viewport.x;

  /* Pixel line buffer */
  uint8 *src = &linebuf[0][0x20 - bitmap.viewport.x];

  /* Adjust line offset in framebuffer */
  line = (line + bitmap.viewport.y) % lines_per_frame;

  /* Take care of Game Gear reduced screen when overscan is disabled */
  if (line < 0) return;

  /* Adjust for interlaced output */
  if (interlaced && config.render)
  {
    line = (line * 2) + odd_frame;
  }

  /* VitAdhoc: the NTSC filters (md_ntsc, sms_ntsc) are not vendored, see md/core/PROVENANCE.md */
  {
#ifdef CUSTOM_BLITTER
    CUSTOM_BLITTER(line, width, pixel, src)
#else
    /* Convert VDP pixel data to output pixel format */
    PIXEL_OUT_T *dst = ((PIXEL_OUT_T *)&bitmap.data[(line * bitmap.pitch)]);
    if (config.lcd)
    {
      do
      {
        RENDER_PIXEL_LCD(src,dst,pixel,config.lcd);
      }
      while (--width);
    }
    else
    {
      do
      {
        *dst++ = pixel[*src++];
      }
      while (--width);
    }
 #endif
  }
}


/*--------------------------------------------------------------------------*/
/* VitAdhoc: the renderer's function pointers as small numbers (VDP worker)  */
/*--------------------------------------------------------------------------*/
/* The worker runs its own compiled copy of this file (vdp_render_w.c), so the addresses in render_bg / render_obj / parse_satb /
 * update_bg_pattern_cache differ between the two. The emulating thread sends these codes, the worker sets its pointers from them. */

static void (*const fn_tab_bg[])(int) = {
  render_bg_m0, render_bg_m1, render_bg_m1x, render_bg_m2, render_bg_m3, render_bg_m3x, render_bg_inv, render_bg_m4,
  render_bg_m5, render_bg_m5_vs, render_bg_m5_vs_enhanced, render_bg_m5_im2, render_bg_m5_im2_vs
};
static void (*const fn_tab_obj[])(int) = {
  render_obj_tms, render_obj_m4, render_obj_m5, render_obj_m5_ste, render_obj_m5_im2, render_obj_m5_im2_ste
};
static void (*const fn_tab_sat[])(int) = { parse_satb_tms, parse_satb_m4, parse_satb_m5, parse_satb_m5_im2 };
static void (*const fn_tab_pat[])(int) = { update_bg_pattern_cache_m4, update_bg_pattern_cache_m5 };

#define FN_CODE(tab, ptr, out, sh) \
  { unsigned c = 63, k; for (k = 0; k < sizeof(tab) / sizeof(tab[0]); k++) if ((tab)[k] == (ptr)) c = k; (out) |= c << (sh); }

unsigned vdp_fn_encode(void)
{
  unsigned o = 0;
  FN_CODE(fn_tab_bg, render_bg, o, 0)
  FN_CODE(fn_tab_obj, render_obj, o, 6)
  FN_CODE(fn_tab_sat, parse_satb, o, 12)
  FN_CODE(fn_tab_pat, update_bg_pattern_cache, o, 18)
  return o;
}

void vdp_fn_decode(unsigned c)
{
  unsigned b = c & 63, ob = (c >> 6) & 63, s = (c >> 12) & 63, p = (c >> 18) & 63;
  render_bg = b < sizeof(fn_tab_bg) / sizeof(fn_tab_bg[0]) ? fn_tab_bg[b] : 0;
  render_obj = ob < sizeof(fn_tab_obj) / sizeof(fn_tab_obj[0]) ? fn_tab_obj[ob] : 0;
  parse_satb = s < sizeof(fn_tab_sat) / sizeof(fn_tab_sat[0]) ? fn_tab_sat[s] : 0;
  update_bg_pattern_cache = p < sizeof(fn_tab_pat) / sizeof(fn_tab_pat[0]) ? fn_tab_pat[p] : 0;
}
