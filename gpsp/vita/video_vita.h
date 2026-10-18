/* video_vita.h -- what the Vita video layer adds to psp/video_psp.h.
 *
 * video_vita.c implements EVERY function of psp/video_psp.h for the PS Vita
 * (and, through vback_soft.c, for the Linux host twin), so ui_psp.c,
 * osd_psp.c and a main loop draw through the API they always used:
 *
 *   - COORDINATES STAY THE PSP's: 480x272 logical pixels (VID_SCR_W/H).
 *     The Vita's panel is 960x544, exactly 2x, and everything is rasterised
 *     at native resolution -- text from the TTFs at 2x the PSP's size, rects
 *     and gradients exact, images filtered, the game picture scaled straight
 *     from 240x160 / 160x144 to native pixels.
 *   - COLOURS are libretro RGB565 + alpha, as on the PSP.
 *   - TEXTURES HANDED TO vid_image* / vid_image_screen / vid_shot_rows /
 *     vid_ambient_bake_* ARE PSP-ORDER 565 (R in bits 0-4), as on the PSP:
 *     that is what ui_psp.c's art loaders produce and what the .565, .thumb
 *     and .shot files hold, and it is kept end to end (the backend has a
 *     BGR565 texture format; nothing is converted at load).
 *   - THE LIVE GAME FRAME (vid_draw_frame, vid_draw_prestaged,
 *     vid_draw_frame_pair) IS LIBRETRO RGB565 (R in bits 11-15): the Vita
 *     core is built without USE_PSP_RGB565_FORMAT.  A main loop that keeps a
 *     copy of a frame for the UI (the menu backdrop, .thumb, .shot) converts
 *     it once with vid_frame_to_psp565().
 *
 * Scale and filter values: see the enums in psp/video_psp.h (#ifdef VITA).
 */
#ifndef VIDEO_VITA_H
#define VIDEO_VITA_H

#include <stddef.h>
#include <stdint.h>

#include "video_psp.h"
#include "upscale_vita.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VID_NATIVE_W 960
#define VID_NATIVE_H 544

/* ---- two pictures at once (same-console link play) -----------------------
 * Both frames are w x h libretro RGB565 at `pitch_bytes`; either may be NULL
 * (that picture is left black).  Integer scales, nearest, always:
 *
 *   VID_PAIR_SIDE   side by side, the largest integer scale at which two fit:
 *                   GBA 2x = 480x320 + 480x320, the full 960 width exactly;
 *                   GB/GBC 3x = 480x432 + 480x432, likewise.
 *   VID_PAIR_INSET  `left` is the main picture, `right` the partner, smaller,
 *                   beside it at the top: GBA 3x (720x480) + 1x (240x160) is
 *                   960 wide exactly; GB/GBC 3x (480x432) + 2x (320x288).
 *
 * The ambient bars (or black) fill the rest.  Draws a whole frame, like
 * vid_draw_frame; OSD and vid_swap follow as usual. */
enum { VID_PAIR_SIDE = 0, VID_PAIR_INSET = 1, VID_PAIR_LAYOUTS = 2 };
void vid_draw_frame_pair(const uint16_t *left, const uint16_t *right,
                         unsigned w, unsigned h, size_t pitch_bytes,
                         int layout);
/* Where the two pictures land (native pixels), for touch or a caption. */
typedef struct { int x, y, w, h; } vid_rect_t;
void vid_pair_rects(unsigned w, unsigned h, int layout, vid_rect_t *left,
                    vid_rect_t *right);
/* Where a single w x h picture lands at the current scale (native pixels). */
void vid_picture_rect(unsigned w, unsigned h, vid_rect_t *out);
/* Of the `margin` extra columns a wide picture has on each side of the
 * console's w x h: how many fit on the screen at the scale in use. */
int vid_margin_fit(unsigned w, unsigned h, int margin);

/* Settings order of the scale modes, smallest picture to largest: 2x, 3x,
 * fit, 4x crop, stretch.  Returns the mode `dir` steps from `scale`. */
int vid_scale_step(int scale, int dir);

/* ---- textures the UI owns -------------------------------------------------
 * The PSP's GE reads a texture straight out of the caller's RAM, so
 * ui_psp.c just passes pointers.  A GPU cannot: video_vita.c keeps a small
 * cache of GPU textures keyed by the pointer.  Tell it when the bytes behind
 * a pointer changed or went away -- pspcompat.h maps the PSP's
 * sceKernelDcacheWritebackRange() onto this, which is exactly the call
 * ui_psp.c already makes after every texture write. */
void vid_tex_invalidate(const void *p, size_t bytes);

/* A libretro-RGB565 frame (w x h at pitch_bytes) -> PSP-order 565 at
 * dst_stride TEXELS: the snapshot a main loop hands to ui_set_backdrop /
 * ui_wake_menu and writes to .thumb / .shot. */
void vid_frame_to_psp565(uint16_t *dst, int dst_stride, const uint16_t *src,
                         unsigned w, unsigned h, size_t pitch_bytes);

/* Wait for vblank on present (default 1).  0 for fast-forward. */
void vid_set_vsync(int on);

/* While a system common dialog is up (the ad hoc NetCheck dialog, the IME),
 * turn this on: vid_swap() then lets the dialog draw itself into the
 * finished frame (sceCommonDialogUpdate) before presenting.  Keep drawing
 * frames as usual -- vid_overlay_begin(1) ... vid_swap() -- underneath. */
void vid_set_common_dialog(int on);

/* vid_dump_ge(path) writes the composed 960x544 frame: PNG when `path` ends
 * in ".png", else a 24-bit BMP.  This one hands back the pixels instead
 * (VID_NATIVE_W x VID_NATIVE_H words, 0xAABBGGRR). */
int vid_readback(uint32_t *dst);

/* "soft" or "vita2d". */
const char *vid_backend_name(void);

/* vid_image_screen's sibling for a frame the UI has blurred: the same rect,
 * always linear, no LCD grid (video_vita.c). */
void vid_image_screen_soft(const uint16_t *pix, int texw, int srcw, int srch,
                           int alpha);

/* Text self-check for the port report: the worst and mean |difference|, in
 * NATIVE pixels, between a glyph's natural advance at the native size and
 * the advance used (2 x the PSP's baked one); `hd` picks the title face.
 * vid_text_w() itself equals the PSP's for every string by construction. */
float vid_text_advance_dev(int hd, int *worst_char, float *mean_abs);

/* COLOUR and MOTION BLEND of the game's picture (video_vita.c "colour and
 * motion"): `color` is VID_COLOR_*, `blend` 1 mixes each frame with the one
 * before.  The main loop sets them when a game boots (a GBA game's colour;
 * VID_COLOR_RAW for GB and GBC, whose colours their own core corrects) and
 * the Settings rows when they change. */
enum { VID_COLOR_RAW = 0, VID_COLOR_GBA = 1, VID_COLOR_GBA_DARK = 2,
       VID_COLOR_MODES = 3 };
void vid_set_look(int color, int blend);
int  vid_look_color(void);
int  vid_look_blend(void);

/* Where the picture last presented is (vback.h vb_present_state): bit 0 =
 * not yet taken by the display queue, bit 1 = not yet on the glass. */
int vid_present_state(void);

/* vita.ini tex_mem: 1 the textures rewritten every frame live in the main
 * RAM, uncached; 0 (default) the GPU's own (vback.h vb_tex_mem).  Takes
 * effect for textures made afterwards. */
void vid_set_tex_mem(int cpu_written);

/* The CPU upscalers' work since the last call (upscale_vita.h). */
void vid_upscale_stats(upscale_stats_t *out);
/* xBRZ drawn by the GPU when it can (video_vita.c, "xBRZ on the GPU"):
 * on = 0 keeps it on the CPU.  vid_gpu_xbrz: is it the GPU's now, and the
 * pictures it has drawn so far. */
void vid_set_gpu_xbrz(int on);
int  vid_gpu_xbrz(unsigned *frames);
/* on = 1: every present waits for the GPU and is timed (measuring only);
 * vid_gpu_time_stats: timed samples so far, the GPU's time over a frame
 * last measured, the slowest since the last call. */
void vid_set_gpu_time(int on);
void vid_gpu_probe(void);
void vid_set_gpu_debug(int on);
void vid_set_gpu_view(int v);
void vid_gpu_time_stats(unsigned *samples, unsigned *avg_us, unsigned *max_us);

/* The slowest of each part of vid_draw_frame since the last call (and the
 * counters cleared), microseconds: the colour/blend pass, the upscaler's
 * textures remade, their lock, collecting a finished picture, handing the
 * new one over, the whole of that staging, the plain upload, the draw
 * calls.  For the log (main_vita.c's vita_draw line). */
enum { VID_DP_LOOK, VID_DP_REMAKE, VID_DP_LOCK, VID_DP_COLLECT,
       VID_DP_SUBMIT, VID_DP_STAGE, VID_DP_UPLOAD, VID_DP_DRAW, VID_DP_N };
void vid_draw_parts(unsigned out[VID_DP_N]);

#ifdef __cplusplus
}
#endif

#endif /* VIDEO_VITA_H */
