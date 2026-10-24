/* vback.h -- the tiny rasteriser interface under video_vita.c.
 *
 * video_vita.c implements the whole psp/video_psp.h overlay API (rects, text,
 * gradients, images, the game picture, ambient bars) in terms of the dozen
 * calls below, so the SAME drawing code runs on two backends:
 *
 *   vback_vita2d.c   the PS Vita's GXM through libvita2d
 *   vback_soft.c     a portable software rasteriser (the Linux host twin
 *                    that the UI is verified with; it can also be dumped)
 *
 * SEMANTICS BOTH BACKENDS HOLD TO (the soft one is the reference):
 *   - The target is VB_W x VB_H = 960 x 544, the Vita's native panel.  All
 *     coordinates are NATIVE pixels, floats, origin top-left, y down.
 *   - Only axis-aligned rectangles exist.  A rectangle covers the pixels
 *     whose CENTRES (x+0.5, y+0.5) lie inside [x0,x1) x [y0,y1).
 *   - Colours are 0xAABBGGRR (libvita2d's RGBA8 order: R in the low byte).
 *   - Everything blends src-over: out = src*a + dst*(1-a).  An opaque source
 *     (a = 255) therefore replaces.
 *   - Texture coordinates are in TEXELS of the texture.  NEAREST takes
 *     texel floor(u); LINEAR weights the four texels around u-0.5, clamped
 *     to the texture's edge.  A texture is always the whole image (never a
 *     sub-rect of a larger allocation), so clamping is what an edge wants.
 *   - A texel is multiplied by the tint before blending.  565 textures have
 *     alpha 255; A8 textures are white with the byte as alpha (glyph
 *     coverage).
 *   - A render target is a VB_FMT_RGBA8 texture created with rt = 1.  Its
 *     coordinates are its own pixels.  vb_rt_begin may be called while a
 *     frame is open: the backend suspends the frame and resumes it without
 *     losing what was drawn (GXM cannot nest scenes; see vback_vita2d.c).
 *   - Nothing here allocates per frame except the vita2d vertex pool.
 */
#ifndef VBACK_H
#define VBACK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VB_W 960
#define VB_H 544

enum
{
   VB_FMT_RGB565 = 0,   /* libretro order: R in bits 11-15                 */
   VB_FMT_BGR565,       /* PSP GE 5650 order: R in bits 0-4                */
   VB_FMT_A8,           /* coverage; sampled as (255,255,255,a)            */
   VB_FMT_RGBA8         /* bytes R,G,B,A                                   */
};

enum { VB_NEAREST = 0, VB_LINEAR = 1 };

typedef struct vb_tex vb_tex;

typedef struct
{
   float x0, y0, x1, y1;      /* destination, native px                    */
   float u0, v0, u1, v1;      /* source, texels                            */
} vb_tquad;

#define VB_RGBA(r, g, b, a) \
   ((((uint32_t)(a) & 0xFF) << 24) | (((uint32_t)(b) & 0xFF) << 16) | \
    (((uint32_t)(g) & 0xFF) << 8) | ((uint32_t)(r) & 0xFF))

/* 0 = ok.  Safe to call twice. */
int  vb_init(void);
void vb_term(void);
const char *vb_name(void);

/* Textures.  NULL when there is no memory.  vb_tex_update copies a w x h
 * block (source rows `pitch_bytes` apart) to x,y.  vb_tex_destroy may be
 * called while the texture is still referenced by a frame in flight: the
 * backend defers the actual free past the next present. */
vb_tex *vb_tex_create(int w, int h, int fmt, int rt);
void    vb_tex_destroy(vb_tex *t);
void    vb_tex_update(vb_tex *t, int x, int y, int w, int h,
                      const void *pix, size_t pitch_bytes);
/* Nonblocking last-fragment-use fence. Query immediately before a CPU
 * publication on the renderer owner thread; 0 means retain the old image. */
int     vb_tex_writable(const vb_tex *t);
typedef struct vb_ownership_diag
{
   unsigned started_frames, skipped_frames, submitted_frames, pending_swaps;
   unsigned pool_bank, busy_banks, submitted_scenes, vertex_pending;
   unsigned fragment_pending, texture_blocked, fatal;
   unsigned scene_serial, vertex_word, fragment_word;
} vb_ownership_diag;
/* Fixed-size, nonblocking native ownership snapshot. Last-scene words are
 * observations, not an ordered completion watermark. */
void vb_ownership_get(vb_ownership_diag *out);
/* The texture's own pixels, for a writer that fills it in place (the CPU
 * upscalers' bands, each on its own core): the pointer and the pitch in
 * bytes, in the texture's format.  NULL when the backend cannot say.  Write
 * only: on the console this is the GPU's memory. */
void   *vb_tex_lock(vb_tex *t, size_t *pitch_bytes);
/* Diagnostics: copy texture `t`'s texels as the GPU last left them (it
 * waits for the GPU) -- RGBA8 textures and render targets only, rows of
 * w*4 bytes into dst.  0 = ok. */
int     vb_tex_read(vb_tex *t, uint32_t *dst);
/* Where the textures made from now on (not render targets) live, until it
 * is called again: 1 = memory the CPU writes quickly, for a texture
 * rewritten every frame (the console's main RAM, uncached: its writes to
 * the GPU's own memory are several times slower); 0 = the GPU's own, the
 * default.  A no-op where there is one memory. */
void    vb_tex_mem(int cpu_written);
/* Where texture `t` was put, for the log: "gpu", "ram", "?". */
const char *vb_tex_where(const vb_tex *t);
int     vb_tex_w(const vb_tex *t);
int     vb_tex_h(const vb_tex *t);

/* One frame: begin (clear = start from black; 0 = keep what the back buffer
 * holds), draw, present.  vb_frame_end closes the frame without presenting
 * (the readback needs finished pixels); a following draw re-opens it. */
void vb_frame_begin(int clear);
void vb_frame_end(void);
int  vb_frame_open(void);
void vb_present(void);
/* Where the picture last presented is: bit 0 = not yet taken by the
 * display's queue, bit 1 = not yet on the glass; 0 = shown.  The twin's
 * answer comes from its model of the queue (plat_host.c --flip-queue). */
int  vb_present_state(void);
/* 1 (default) = a present waits for vblank; 0 = as fast as it renders. */
void vb_set_vsync(int on);
/* 1 = let the system's common dialog (NetCheck, IME) draw itself into each
 * finished frame before it is presented.  Vita only; a no-op elsewhere. */
void vb_set_common_dialog(int on);

void vb_rt_begin(vb_tex *rt);
void vb_rt_end(void);

/* Scissor in the frame's pixels; on = 0 releases it.  It never applies
 * inside a render target, and a present releases it. */
void vb_scissor(int on, int x, int y, int w, int h);

/* Solid or gradient rectangle: a colour per corner (top-left, top-right,
 * bottom-left, bottom-right), interpolated. */
void vb_quad(float x0, float y0, float x1, float y1,
             uint32_t c_tl, uint32_t c_tr, uint32_t c_bl, uint32_t c_br);
/* n solid or gradient rectangles in one draw (as n vb_quad calls would
 * draw them; under a scissor they are drawn one by one). */
typedef struct
{
   float x0, y0, x1, y1;
   uint32_t c_tl, c_tr, c_bl, c_br;
} vb_cquad;
#define VB_CQUADS_MAX 64
void vb_cquads(const vb_cquad *q, int n);
/* n textured rectangles from one texture, one tint, one filter. */
void vb_tquads(vb_tex *t, const vb_tquad *q, int n, uint32_t tint,
               int filter);

/* The composed frame (what the last/next present shows), as VB_W x VB_H
 * 0xAABBGGRR words.  Closes the frame.  0 = ok. */
int  vb_readback(uint32_t *dst);

/* GPU programs: picture filters the GPU runs per output pixel (gpu shaders
 * in gpsp/vita/shaders/, written once for both backends).  xBRZ is three:
 *
 *   VB_PROG_XBRZ0   the points where four picture pixels meet: unit 0 = the
 *                   picture; uniforms [0] its width, height, 1/width,
 *                   1/height.  Drawn 1:1 into a render target one pixel
 *                   larger each way (u from -1).
 *   VB_PROG_XBRZ1   each pixel's four corners: unit 0 = the picture, unit 1
 *                   = XBRZ0's target; [0] as above, [1] xy = 1 / that
 *                   target's width, height.  Drawn 1:1.
 *   VB_PROG_XBRZ2   xBRZ at any size: unit 0 = the picture, unit 1 =
 *                   XBRZ1's target; [0] as above, [1] xy the picture's
 *                   size over that target's, [2] xy output pixels per
 *                   picture pixel.
 *
 * vb_prog_get makes a program on first use and keeps it; NULL when this
 * backend cannot (the console without the system's shader compiler,
 * ur0:data/libshacccg.suprx; the twin unless --gpu-progs) -- and
 * vb_prog_status says why, or how it was made, for the log.
 *
 * vb_pquads: n rectangles through program p, as vb_tquads draws them (u, v
 * in texels of t0; the scissor applies in the frame), except that the
 * program's colour is written as it is: no tint, no blending.  Every
 * texture is point sampled, its edges clamped. */
/* How long the GPU takes over a frame: vb_gpu_time(n) times the next n
 * presents (-1: every present until vb_gpu_time(0)) -- each waits until
 * the GPU has finished everything queued and counts the wait, so it costs
 * the overlap of CPU and GPU while it lasts.  vb_gpu_timing: presents
 * still to time (-1: all).  vb_gpu_stats reads the timed presents and
 * resets them.  The twin has no GPU: GBADHOC_SOFT_GPU_US (plat_host.c
 * --gpu-us) is what it answers for each. */
void vb_gpu_time(int frames);
int  vb_gpu_timing(void);
void vb_gpu_stats(unsigned *frames, unsigned *avg_us, unsigned *max_us);

enum { VB_PROG_XBRZ0 = 0, VB_PROG_XBRZ1 = 1, VB_PROG_XBRZ2 = 2, VB_PROGS };
enum { VB_PROG_UNIFORMS = 4 };
typedef struct vb_prog vb_prog;
vb_prog    *vb_prog_get(int which);
const char *vb_prog_status(int which);
/* Diagnostics: compile test program n (0, 1, ...) and describe the result
 * in `out`; 0 = no such test.  Nothing is kept. */
int         vb_prog_probe(int n, char *out, size_t outlen);
void        vb_pquads(vb_prog *p, vb_tex *t0, vb_tex *t1, const vb_tquad *q,
                      int n, const float uni[VB_PROG_UNIFORMS][4]);

#ifdef __cplusplus
}
#endif

#endif /* VBACK_H */
