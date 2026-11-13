/*
 * CBOC-Vita local addition (not part of upstream Snes9x). MIT licence.
 *
 * Wide view: what the renderer records and the margin pictures it draws, for the adapter in
 * snes/frontend/wide.cpp (widekit). Everything here is write-only from the emulation's point of
 * view: nothing is read by the CPU, PPU registers, DMA or the APU, nothing is saved in a state,
 * and the 256 columns of the normal picture are produced by the unchanged render path.
 *
 * The margin is drawn by a second, separate pass (gfx.cpp, S9xWideBatch) into private buffers:
 * the game picture's column x of line y is at buffer column x + M of row y, so the left margin
 * is buffer columns [0, M) and the right margin [M + 256, 2M + 256). The pass runs on the same
 * thread as the main pass of the batch (the PPU thread when it is on) from the same snapshot.
 */
#ifndef S9X_WIDECAP_H
#define S9X_WIDECAP_H

#include "port.h"

#define SW_MAXM     96          /* widest margin per side in pixels */
#define SW_PITCH    512         /* row pitch of the pass buffers in pixels (= MAX_SNES_WIDTH) */
#define SW_ROWS     240
#define SW_NPASS    6           /* 0 composite, 1 sprites alone, 2..5 BG1..BG4 alone */
enum { SWP_COMPOSITE = 0, SWP_OBJ = 1, SWP_BG0 = 2 };

/* SWideLine::flags */
enum {
	SWL_VALID    = 1,      /* the line was rendered this frame */
	SWL_BLANK    = 2,      /* forced blank */
	SWL_NOMARGIN = 4,      /* the margin of this line cannot be drawn exactly (hi-res, interlace, mode 2/4/6, mosaic ...) */
	SWL_MODE7    = 8,
	SWL_DRAWN    = 16,     /* the passes of this line were drawn */
	SWL_MOSAIC   = 32,     /* a mosaic is active on an enabled layer */
	SWL_OPT      = 64,     /* offset-per-tile mode */
	SWL_HIRES    = 128     /* hi-res (modes 5/6) or pseudo hi-res */
};

/* What the PPU was set up to do on one scanline (one batch of the renderer = one group of lines). */
struct SWideLine
{
	uint8   flags;
	uint8   mode, bg3prio, bright;
	uint8   tm, ts;                 /* $212C, $212D: main / sub screen layer enables (bit 4 = sprites) */
	uint8   tsx;                    /* BGs shown only through the sub screen with colour math on the backdrop alone: their pass is a composite of backdrop + that layer (math done by the core) */
	uint8   cgwsel, cgadsub;        /* $2130, $2131: colour math */
	uint8   fixr, fixg, fixb;
	uint16  fix565;                 /* the fixed colour as the core adds it (brightness applied) */
	uint8   mosaic;                 /* PPU.Mosaic when any enabled BG has the mosaic bit, else 1 */
	uint8   wsel_any;               /* a window is in force for some layer ($212E/$212F with window masks) */
	uint8   extbg;
	uint8   bgsize[4];              /* 0: 8x8 tiles, 1: 16x16 */
	uint8   scsize[4];              /* 0: 32x32 .. 3: 64x64 tilemap */
	uint16  scbase[4];              /* tilemap base, VRAM word address */
	uint16  name[4];                /* character base, VRAM word address */
	uint16  hofs[4], vofs[4];       /* scroll registers as the renderer reads them (vofs already +1) */
	uint16  backdrop;               /* colour of CGRAM[0] after brightness (RGB565 as drawn) */
	uint32  cgsum;                  /* hash of the palette in force */
	short   m7[8];                  /* Mode 7: A B C D centreX centreY hofs vofs */
	uint8   m7flags;                /* bit0 hflip bit1 vflip bits2-3 repeat */
	uint8   obj_size, obj_prio_rot; /* sprite size select, priority rotation */
	uint16  obj_base, obj_gap;      /* sprite name base, name select */
	uint8   pass;                   /* bit p: pass p of SWP_* drawn for this line (margin candidates valid) */
};

struct SWideView
{
	int      m;                     /* margin per side the passes were drawn with (0 = none this frame) */
	int      h;                     /* visible lines */
	int      ok;                    /* 1: the frame could be widened (no hi-res / interlace anywhere) */
	uint32   clock;                 /* frame clock of the stamps (counts every emulated frame while the wide view is on) */
	uint32   epoch;                 /* changes when the state was loaded or the console was reset */
	const struct SWideLine *line;   /* [SW_ROWS] */
	const uint16 *pass_px[SW_NPASS];/* pass pictures, pitch SW_PITCH, column = screen x + m */
	const uint8  *pass_z[SW_NPASS]; /* non-zero where the pass drew a pixel */
	const uint8  *vram;             /* 64 KB */
	const uint16 *vage;             /* [32768] clock (low 16 bits, 0 = never written since the view was switched on) of the last write of each VRAM word */
	const uint16 *uage;             /* [2048]  the same per 32-byte unit */
	const uint16 *cgram;            /* 256 BGR555 */
	const uint16 *colors;           /* 256 RGB565 as the last batch of the frame drew them (brightness applied) */
	const uint8  *oam;              /* 544 bytes */
	const uint16 *centre;           /* the normal picture of this frame, 256 wide */
	int      centre_pitch;          /* in pixels */
	int      rendered_w;            /* 256 or 512 */
};

/* the hook that gets a finished frame: called on the render thread (PPU thread when on, else the
 * emulation thread at the end of the frame) */
typedef void (*SWideHook)(const struct SWideView *view, void *user);

#ifdef __cplusplus
extern "C" {
#endif
extern uint32 g_s9x_wide_clock;
extern uint32 g_s9x_wide_epoch;
extern uint8  g_s9x_wide_stamp;       /* 1: VRAM writes are stamped */
extern uint8  g_s9x_wide_m;
extern uint8  g_s9x_wide_fm;          /* margin latched for the frame being emulated */           /* margin the next frame is drawn with (0 = wide view off) */
extern uint16 g_s9x_vage[32768];
extern uint16 g_s9x_uage[2048];
extern SWideHook g_s9x_wide_hook;
extern void  *g_s9x_wide_user;

/* register side (emulation thread) */
void S9xWideSet(int margin, SWideHook hook, void *user);   /* margin 0 = off; call between frames */
void S9xWideReset(void);                                   /* state loaded / console reset */
void S9xWideFrameStartLive(void);                          /* from S9xStartScreenRefresh */
void S9xWideFrameEndLive(void);                            /* from S9xEndScreenRefresh, single thread only */

/* the renderer copy (the live copy, or the renamed copy of the PPU thread: _r suffix) */
void S9xWideRenderFrameStart(int margin);
void S9xWideFillView(struct SWideView *v);
#ifdef __cplusplus
}
#endif

#endif
