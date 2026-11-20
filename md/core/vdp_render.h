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

#ifndef _RENDER_H_
#define _RENDER_H_

/* 3:3:2 RGB */
#if defined(USE_8BPP_RENDERING)
#define PIXEL(r,g,b) (((r) << 5) | ((g) << 2) | (b))
#define GET_R(pixel) (((pixel) & 0xe0) >> 5)
#define GET_G(pixel) (((pixel) & 0x1c) >> 2)
#define GET_B(pixel) (((pixel) & 0x03) >> 0)

/* 5:5:5 RGB */
#elif defined(USE_15BPP_RENDERING)
#if defined(USE_ABGR)
#define PIXEL(r,g,b) ((1 << 15) | ((b) << 10) | ((g) << 5) | (r))
#define GET_B(pixel) (((pixel) & 0x7c00) >> 10)
#define GET_G(pixel) (((pixel) & 0x03e0) >> 5)
#define GET_R(pixel) (((pixel) & 0x001f) >> 0)
#else
#define PIXEL(r,g,b) ((1 << 15) | ((r) << 10) | ((g) << 5) | (b))
#define GET_R(pixel) (((pixel) & 0x7c00) >> 10)
#define GET_G(pixel) (((pixel) & 0x03e0) >> 5)
#define GET_B(pixel) (((pixel) & 0x001f) >> 0)
#endif

/* 5:6:5 RGB */
#elif defined(USE_16BPP_RENDERING)
#define PIXEL(r,g,b) (((r) << 11) | ((g) << 5) | (b))
#define GET_R(pixel) (((pixel) & 0xf800) >> 11)
#define GET_G(pixel) (((pixel) & 0x07e0) >> 5)
#define GET_B(pixel) (((pixel) & 0x001f) >> 0)

/* 8:8:8 RGB */
#elif defined(USE_32BPP_RENDERING)
#define PIXEL(r,g,b) ((0xff << 24) | ((r) << 16) | ((g) << 8) | (b))
#define GET_R(pixel) (((pixel) & 0xff0000) >> 16)
#define GET_G(pixel) (((pixel) & 0x00ff00) >> 8)
#define GET_B(pixel) (((pixel) & 0x0000ff) >> 0)
#endif

/* LCD image persistence (ghosting) filter */
/* Simulates (roughly) the slow decay response time of passive-matrix LCD */
/* Rate value is formatted as 0.8 fixed-point integer (between 0.0 and 0.99609375), a higher value meaning a slower decay */
/* Required for proper display of some effects in a few Game Gear games (James Pond 3, Power Drift, Super Monaco GP II,...) */
#define RENDER_PIXEL_LCD(in,out,table,rate) \
{ \
  PIXEL_OUT_T pixel_out = table[*in++]; \
  PIXEL_OUT_T pixel_old  = *out; \
  uint8 r = GET_R(pixel_out); \
  uint8 g = GET_G(pixel_out); \
  uint8 b = GET_B(pixel_out); \
  int r_decay = GET_R(pixel_old) - r; \
  int g_decay = GET_G(pixel_old) - g; \
  int b_decay = GET_B(pixel_old) - b; \
  if (r_decay > 0) r += (rate * r_decay) >> 8; \
  if (g_decay > 0) g += (rate * g_decay) >> 8; \
  if (b_decay > 0) b += (rate * b_decay) >> 8; \
  *out++ = PIXEL(r,g,b); \
}

/* Global variables */
extern uint16 spr_col;

/* VitAdhoc wide view (md/frontend/wide.c): the Mega Drive line renderer can draw a strip of picture beyond each side of the screen into the
 * frame buffer (left of bitmap.data and right of the line), by running the unchanged plane and sprite renderers again on a narrow window
 * with the scroll shifted. It changes nothing the CPUs or the picture inside the screen can see: status, sprite masking and collision state
 * are saved and put back. wide_rec holds, for every line, what the host needs to judge the strip. */
typedef struct {
  uint8 flags;               /* WL_* */
  uint8 col_mask, shift;     /* playfield column mask and row shift (the plane size) */
  uint8 side;                /* WS_* */
  uint16 xs[2], ys[2];       /* horizontal scroll value and first vertical scroll value of plane A and B for this line */
  uint16 ntab, ntbb, ntwb;
  uint16 row_mask;
  uint16 ysr[2];             /* the vertical scroll value of the last column (per 2-cell scrolling; else the same as ys) */
  uint16 pad[2];
} wide_line_t;
enum { WL_ON = 1,            /* a mode 5 line was drawn */
       WL_STRIP = 2,         /* a strip beside this line was drawn (WS_L / WS_R say which) */
       WL_WINDOW = 4,        /* the window plane covers the line or part of it */
       WL_VSCROLL = 8,       /* per 2-cell vertical scroll with different values */
       WL_IM2 = 16,          /* interlace mode 2 */
       WL_H40 = 32,
       WL_M4 = 64,           /* a Master System / Game Gear mode 4 line (record only) */
       WL_WFULL = 128 };     /* the window plane takes the whole line (plane A is not shown on it) */
enum { WS_L = 1, WS_R = 2,           /* the left / right strip was drawn */
       WS_NOA_L = 4, WS_NOA_R = 8,   /* ... without plane A: the window plane takes that side of the line, plane B and the sprites go on */
       WS_OVR_A = 16, WS_OVR_B = 32, /* the strip pass read a learned row for plane A / B (wide_row_set) */
       WS_PAL = 64,                  /* 8 bit consoles: the palette is not the one line 0 had (wide_pal) */
       WS_VSE = 128 };               /* per 2-cell vertical scroll: the strips scroll like the column at their edge */
extern int wide_m;                      /* strip width per side in pixels (0 = off, -1 = the records only: Master System / Game Gear) */
extern int wide_planes;                 /* planes drawn into the strips: bit 0 A, bit 1 B (default 3) */
extern uint32 wide_opq[256][2][3];      /* strip pixels that are opaque (see md_wide_frame) */
extern wide_line_t wide_rec[256];
extern uint16 wide_pal[32];
extern void wide_rows_clear(void);
extern void wide_row_set(int plane, int row, unsigned int addr, const uint16 *words, int n);
/* the emulating thread's list of the 32 byte VRAM blocks written since the host last took it (vdp_ctrl.c) */
extern uint8 wide_vd[0x800];
extern uint16 wide_vl[0x800];
extern int wide_vn, wide_vall;

/* Function prototypes */
extern void render_init(void);
extern void render_reset(void);
extern void render_line(int line);
extern void blank_line(int line, int offset, int width);
extern void remap_line(int line);
extern void window_clip(unsigned int data, unsigned int sw);
extern void render_bg_m0(int line);
extern void render_bg_m1(int line);
extern void render_bg_m1x(int line);
extern void render_bg_m2(int line);
extern void render_bg_m3(int line);
extern void render_bg_m3x(int line);
extern void render_bg_inv(int line);
extern void render_bg_m4(int line);
extern void render_bg_m5(int line);
extern void render_bg_m5_vs(int line);
extern void render_bg_m5_vs_enhanced(int line);
extern void render_bg_m5_im2(int line);
extern void render_bg_m5_im2_vs(int line);
extern void render_obj_tms(int line);
extern void render_obj_m4(int line);
extern void render_obj_m5(int line);
extern void render_obj_m5_ste(int line);
extern void render_obj_m5_im2(int line);
extern void render_obj_m5_im2_ste(int line);
extern void parse_satb_tms(int line);
extern void parse_satb_m4(int line);
extern void parse_satb_m5(int line);
extern void parse_satb_m5_im2(int line);
extern void update_bg_pattern_cache_m4(int index);
extern void update_bg_pattern_cache_m5(int index);
extern void color_update_m4(int index, unsigned int data);
extern void color_update_m5(int index, unsigned int data);

/* Function pointers */
extern void (*render_bg)(int line);
extern void (*render_obj)(int line);
extern void (*parse_satb)(int line);
extern void (*update_bg_pattern_cache)(int index);

#endif /* _RENDER_H_ */
