/* video_tile.h -- one row (8 pixels) of a 4bpp tile or sprite, written to the
 * scanline, for the renderer's hot loops (video.cc render_tile_Nbpp and
 * render_obj_tile_Nbpp).
 *
 * vt_bg_row4() and vt_obj_row4() are the per-pixel loops those functions had,
 * moved here VERBATIM (the `scalar` arm, which is what every build without
 * NEON still runs) beside a NEON arm that does the same eight pixels with a
 * handful of vector operations: the eight 4-bit palette indices become one
 * byte each, a transparent pixel (index 0) is a lane mask, and the pixels are
 * merged into the destination with a bit select.  The colour of FULLCOLOR is
 * looked up with VTBL (a 16-entry palette is 2 x 16 bytes: the low and the
 * high bytes of the colours, a table lookup per byte), so even that stays in
 * registers.  Nothing is computed differently: every lane does what one
 * iteration of the scalar loop does (tools/tests/test_video_tile.cc compares
 * the two on every index value in every lane position, over random rows,
 * destinations and flags, for all rendtype / destination type / isbase /
 * hflip combinations the renderer instantiates).
 *
 * Not covered (the callers keep the scalar code): 8bpp (a 256-entry palette
 * does not fit a table lookup), PIXCOPY (the OBJ window), partial tiles, mosaic
 * and affine sprites, and any build with the VIDEO_PROF counters.
 */
#ifndef GPSP_VIDEO_TILE_H
#define GPSP_VIDEO_TILE_H

typedef enum
{
  FULLCOLOR,  // Regular rendering, output a 16 bit color
  INDXCOLOR,  // Rendering to indexed color, so we can later apply dark/bright
  STCKCOLOR,  // Stacks two indexed pixels (+flags) to apply blending
  PIXCOPY     // Special mode used for sprites, to allow for obj-window drawing
} rendtype;

#if (defined(__ARM_NEON) || defined(__ARM_NEON__)) && \
    !defined(VIDEO_TILE_NO_NEON) && (!defined(VIDEO_PROF) || !VIDEO_PROF)
#include <arm_neon.h>
#define VT_NEON 1

// The eight 4-bit indices of a tile row, one per byte, in pixel order.
template <bool hflip>
static inline uint8x8_t vt_indices(unsigned tilepix)
{
  uint8x8_t v  = vcreate_u8((unsigned long long)tilepix);
  uint8x8_t lo = vand_u8(v, vdup_n_u8(15));
  uint8x8_t hi = vshr_n_u8(v, 4);
  uint8x8_t px = vzip_u8(lo, hi).val[0];       // lo0 hi0 lo1 hi1 ... = pixels 0..7
  return hflip ? vrev64_u8(px) : px;           // hflip: pixel i is nibble 7-i
}

static inline uint16x8_t vt_mask16(uint8x8_t idx)   // 0xFFFF where idx != 0
{
  return vreinterpretq_u16_s16(vmovl_s8(vreinterpret_s8_u8(vtst_u8(idx, idx))));
}

// The colours subpal[idx], by table lookup.
static inline uint16x8_t vt_colors(uint8x8_t idx, const unsigned short *subpal)
{
  uint8x16x2_t pl = vld2q_u8((const unsigned char *)subpal);  // low, high bytes
  uint8x8x2_t tl, th;
  tl.val[0] = vget_low_u8(pl.val[0]);  tl.val[1] = vget_high_u8(pl.val[0]);
  th.val[0] = vget_low_u8(pl.val[1]);  th.val[1] = vget_high_u8(pl.val[1]);
  uint8x8x2_t z = vzip_u8(vtbl2_u8(tl, idx), vtbl2_u8(th, idx));
  return vreinterpretq_u16_u8(vcombine_u8(z.val[0], z.val[1]));
}

// dest[0..7] = idx ? (flag | idx) : (isbase ? bgval : dest[i])      (u16 dest)
template <bool isbase>
static inline void vt_indx16(unsigned short *dest, uint8x8_t idx,
                             unsigned short flag, unsigned short bgval)
{
  uint16x8_t m  = vt_mask16(idx);
  uint16x8_t nv = vorrq_u16(vmovl_u8(idx), vdupq_n_u16(flag));
  uint16x8_t ot = isbase ? vdupq_n_u16(bgval) : vld1q_u16(dest);
  vst1q_u16(dest, vbslq_u16(m, nv, ot));
}

// Same for a u32 destination.
template <bool isbase>
static inline void vt_indx32(unsigned *dest, uint8x8_t idx,
                             unsigned flag, unsigned bgval)
{
  uint16x8_t i16 = vmovl_u8(idx);
  uint16x8_t m16 = vt_mask16(idx);
  uint32x4_t m0 = vreinterpretq_u32_s32(vmovl_s16(vget_low_s16(vreinterpretq_s16_u16(m16))));
  uint32x4_t m1 = vreinterpretq_u32_s32(vmovl_s16(vget_high_s16(vreinterpretq_s16_u16(m16))));
  uint32x4_t n0 = vorrq_u32(vmovl_u16(vget_low_u16(i16)),  vdupq_n_u32(flag));
  uint32x4_t n1 = vorrq_u32(vmovl_u16(vget_high_u16(i16)), vdupq_n_u32(flag));
  uint32x4_t o0 = isbase ? vdupq_n_u32(bgval) : vld1q_u32(dest);
  uint32x4_t o1 = isbase ? vdupq_n_u32(bgval) : vld1q_u32(dest + 4);
  vst1q_u32(dest,     vbslq_u32(m0, n0, o0));
  vst1q_u32(dest + 4, vbslq_u32(m1, n1, o1));
}

// A background's STCKCOLOR: idx ? (flag | idx | ((isbase ? bg_comb : d) << 16))
//                               : (isbase ? bg_comb : d)
template <bool isbase>
static inline void vt_stck_bg(unsigned *dest, uint8x8_t idx,
                              unsigned flag, unsigned bg_comb)
{
  uint16x8_t i16 = vmovl_u8(idx);
  uint16x8_t m16 = vt_mask16(idx);
  uint32x4_t m0 = vreinterpretq_u32_s32(vmovl_s16(vget_low_s16(vreinterpretq_s16_u16(m16))));
  uint32x4_t m1 = vreinterpretq_u32_s32(vmovl_s16(vget_high_s16(vreinterpretq_s16_u16(m16))));
  uint32x4_t d0 = isbase ? vdupq_n_u32(bg_comb) : vld1q_u32(dest);
  uint32x4_t d1 = isbase ? vdupq_n_u32(bg_comb) : vld1q_u32(dest + 4);
  uint32x4_t f  = vdupq_n_u32(flag);
  uint32x4_t n0 = vorrq_u32(vorrq_u32(vmovl_u16(vget_low_u16(i16)),  f), vshlq_n_u32(d0, 16));
  uint32x4_t n1 = vorrq_u32(vorrq_u32(vmovl_u16(vget_high_u16(i16)), f), vshlq_n_u32(d1, 16));
  vst1q_u32(dest,     vbslq_u32(m0, n0, d0));
  vst1q_u32(dest + 4, vbslq_u32(m1, n1, d1));
}

// A sprite's STCKCOLOR: idx ? (flag | idx | ((d & 0x100) ? d & 0xFFFF0000 : d << 16))
//                           : d
static inline void vt_stck_obj(unsigned *dest, uint8x8_t idx, unsigned flag)
{
  uint16x8_t i16 = vmovl_u8(idx);
  uint16x8_t m16 = vt_mask16(idx);
  uint32x4_t m0 = vreinterpretq_u32_s32(vmovl_s16(vget_low_s16(vreinterpretq_s16_u16(m16))));
  uint32x4_t m1 = vreinterpretq_u32_s32(vmovl_s16(vget_high_s16(vreinterpretq_s16_u16(m16))));
  uint32x4_t d0 = vld1q_u32(dest), d1 = vld1q_u32(dest + 4);
  uint32x4_t f  = vdupq_n_u32(flag), k100 = vdupq_n_u32(0x100),
             khi = vdupq_n_u32(0xFFFF0000u);
  uint32x4_t s0 = vtstq_u32(d0, k100), s1 = vtstq_u32(d1, k100);
  uint32x4_t h0 = vbslq_u32(s0, vandq_u32(d0, khi), vshlq_n_u32(d0, 16));
  uint32x4_t h1 = vbslq_u32(s1, vandq_u32(d1, khi), vshlq_n_u32(d1, 16));
  uint32x4_t n0 = vorrq_u32(vorrq_u32(vmovl_u16(vget_low_u16(i16)),  f), h0);
  uint32x4_t n1 = vorrq_u32(vorrq_u32(vmovl_u16(vget_high_u16(i16)), f), h1);
  vst1q_u32(dest,     vbslq_u32(m0, n0, d0));
  vst1q_u32(dest + 4, vbslq_u32(m1, n1, d1));
}
#else
#define VT_NEON 0
#endif

/* ---- a background tile row: render_tile_Nbpp's 4bpp, tilepix != 0 / isbase ----
 * dest: the 8 pixels; tilepix: the row's 32 bits (the caller byte-swapped);
 * tilepal: (tile >> 12) << 4; px_comb, bg_comb: the colour-effect flags;
 * paltbl: the palette; bgcolor: paltbl[0]. */
template <typename dtype, rendtype rdtype, bool isbase, bool hflip>
static inline void vt_bg_row4_s(dtype *dest_ptr, unsigned tilepix,
  unsigned short tilepal, unsigned px_comb, unsigned bg_comb,
  const unsigned short *paltbl, unsigned short bgcolor)
{
  if (tilepix) {  // We can skip it all if the row is transparent
    unsigned short pxflg = px_comb | tilepal;
    const unsigned short *subpal = &paltbl[tilepal];
    for (unsigned i = 0; i < 8; i++, dest_ptr++) {
      unsigned char pval = (hflip ? (tilepix >> ((7-i)*4)) : (tilepix >> (i*4))) & 0xF;
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
    for (unsigned i = 0; i < 8; i++, dest_ptr++)
      *dest_ptr = (rdtype == FULLCOLOR) ? bgcolor : 0 | bg_comb;
  }
}

template <typename dtype, rendtype rdtype, bool isbase, bool hflip>
static inline void vt_bg_row4_n(dtype *dest_ptr, unsigned tilepix,
  unsigned short tilepal, unsigned px_comb, unsigned bg_comb,
  const unsigned short *paltbl, unsigned short bgcolor)
{
#if VT_NEON
  if (tilepix) {
    uint8x8_t idx = vt_indices<hflip>(tilepix);
    unsigned short pxflg = (unsigned short)(px_comb | tilepal);
    if (rdtype == FULLCOLOR && sizeof(dtype) == 2) {
      uint16x8_t m  = vt_mask16(idx);
      uint16x8_t ot = isbase ? vdupq_n_u16(bgcolor)
                             : vld1q_u16((unsigned short *)dest_ptr);
      vst1q_u16((unsigned short *)dest_ptr,
                vbslq_u16(m, vt_colors(idx, &paltbl[tilepal]), ot));
      return;
    }
    if (rdtype == INDXCOLOR && sizeof(dtype) == 2) {
      vt_indx16<isbase>((unsigned short *)dest_ptr, idx, pxflg,
                        (unsigned short)bg_comb);
      return;
    }
    if (rdtype == INDXCOLOR && sizeof(dtype) == 4) {
      vt_indx32<isbase>((unsigned *)dest_ptr, idx, pxflg, bg_comb);
      return;
    }
    if (rdtype == STCKCOLOR && sizeof(dtype) == 4) {
      vt_stck_bg<isbase>((unsigned *)dest_ptr, idx, pxflg, bg_comb);
      return;
    }
  }
#endif
  vt_bg_row4_s<dtype, rdtype, isbase, hflip>(dest_ptr, tilepix, tilepal,
    px_comb, bg_comb, paltbl, bgcolor);
}

/* ---- a sprite tile row: render_obj_tile_Nbpp's 4bpp (not PIXCOPY) --------------
 * px_attr: px_comb | palette | 0x100; pal: the sprite palette; palette: the
 * sub-palette offset (0..0xF0). */
template <typename dtype, rendtype rdtype, bool hflip>
static inline void vt_obj_row4_s(dtype *dest_ptr, unsigned tilepix,
  unsigned px_attr, const unsigned short *pal, unsigned short palette)
{
  if (tilepix) {   // Can skip all pixels if the row is just transparent
    for (unsigned i = 0; i < 8; i++, dest_ptr++) {
      unsigned char pval = (hflip ? (tilepix >> ((7-i)*4)) : (tilepix >> (i*4))) & 0xF;
      const unsigned short *subpal = &pal[palette];
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

template <typename dtype, rendtype rdtype, bool hflip>
static inline void vt_obj_row4_n(dtype *dest_ptr, unsigned tilepix,
  unsigned px_attr, const unsigned short *pal, unsigned short palette)
{
#if VT_NEON
  if (tilepix) {
    uint8x8_t idx = vt_indices<hflip>(tilepix);
    if (rdtype == FULLCOLOR && sizeof(dtype) == 2) {
      uint16x8_t m = vt_mask16(idx);
      vst1q_u16((unsigned short *)dest_ptr,
                vbslq_u16(m, vt_colors(idx, &pal[palette]),
                          vld1q_u16((unsigned short *)dest_ptr)));
      return;
    }
    if (rdtype == INDXCOLOR && sizeof(dtype) == 2) {
      vt_indx16<false>((unsigned short *)dest_ptr, idx,
                       (unsigned short)px_attr, 0);
      return;
    }
    if (rdtype == INDXCOLOR && sizeof(dtype) == 4) {
      vt_indx32<false>((unsigned *)dest_ptr, idx, px_attr, 0);
      return;
    }
    if (rdtype == STCKCOLOR && sizeof(dtype) == 4) {
      vt_stck_obj((unsigned *)dest_ptr, idx, px_attr);
      return;
    }
  }
#endif
  vt_obj_row4_s<dtype, rdtype, hflip>(dest_ptr, tilepix, px_attr, pal, palette);
}

/* -DVIDEO_TILE_CHECK (a test build): every row is also done by the scalar
 * loop on a copy, and the two are compared; the first difference aborts.  The
 * run's report says how many rows were compared. */
#ifdef VIDEO_TILE_CHECK
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned long long video_tile_checked;
static void video_tile_report(void)
{ fprintf(stderr, "VIDEO_TILE_CHECK: %llu tile rows identical to the scalar loops\n", video_tile_checked); }
__attribute__((constructor)) static void video_tile_install(void) { atexit(video_tile_report); }
#endif

template <typename dtype, rendtype rdtype, bool isbase, bool hflip>
static inline void vt_bg_row4(dtype *dest_ptr, unsigned tilepix,
  unsigned short tilepal, unsigned px_comb, unsigned bg_comb,
  const unsigned short *paltbl, unsigned short bgcolor)
{
#ifdef VIDEO_TILE_CHECK
  dtype chk[8];
  memcpy(chk, dest_ptr, sizeof(chk));
  vt_bg_row4_s<dtype, rdtype, isbase, hflip>(chk, tilepix, tilepal, px_comb,
    bg_comb, paltbl, bgcolor);
#endif
  vt_bg_row4_n<dtype, rdtype, isbase, hflip>(dest_ptr, tilepix, tilepal,
    px_comb, bg_comb, paltbl, bgcolor);
#ifdef VIDEO_TILE_CHECK
  if (memcmp(chk, dest_ptr, sizeof(chk))) {
    fprintf(stderr, "VIDEO_TILE_CHECK: bg row differs (tilepix %08x rdtype %d)\n",
            tilepix, (int)rdtype);
    abort();
  }
  video_tile_checked++;
#endif
}

template <typename dtype, rendtype rdtype, bool hflip>
static inline void vt_obj_row4(dtype *dest_ptr, unsigned tilepix,
  unsigned px_attr, const unsigned short *pal, unsigned short palette)
{
#ifdef VIDEO_TILE_CHECK
  dtype chk[8];
  memcpy(chk, dest_ptr, sizeof(chk));
  vt_obj_row4_s<dtype, rdtype, hflip>(chk, tilepix, px_attr, pal, palette);
#endif
  vt_obj_row4_n<dtype, rdtype, hflip>(dest_ptr, tilepix, px_attr, pal, palette);
#ifdef VIDEO_TILE_CHECK
  if (memcmp(chk, dest_ptr, sizeof(chk))) {
    fprintf(stderr, "VIDEO_TILE_CHECK: obj row differs (tilepix %08x rdtype %d)\n",
            tilepix, (int)rdtype);
    abort();
  }
  video_tile_checked++;
#endif
}

#endif /* GPSP_VIDEO_TILE_H */
