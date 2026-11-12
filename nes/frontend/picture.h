/*
 * Picture layout, settings file and smoothness governor of the NES program.
 * Portable (no Vita calls) so the Linux build can test it. Copyright CBOC-Vita.
 * MIT licence (see nes/frontend/LICENSE).
 */
#ifndef NES_PICTURE_H
#define NES_PICTURE_H

#include <stddef.h>

enum { PIC_ORIGINAL = 0, PIC_ZOOM, PIC_FULL, PIC_PIXEL, PIC_SHAPES };
enum { PIC_SHARP = 0, PIC_SMOOTH };
enum { PIC_OVERSCAN_AUTO = 0, PIC_OVERSCAN_ON, PIC_OVERSCAN_OFF };

typedef struct {
	int shape;     /* PIC_ORIGINAL (default), PIC_ZOOM, PIC_FULL, PIC_PIXEL */
	int pixel_sq;  /* pixel perfect: 0 = 8:7 pixels, 1 = square pixels */
	int quality;   /* PIC_SHARP (default) or PIC_SMOOTH */
	int glow;      /* side glow 0 (off) .. 10 */
	int scan;      /* CRT scanlines 0 (off) .. 10 */
	int grid;      /* LCD grid 0 (off) .. 10 */
	int overscan;  /* PIC_OVERSCAN_AUTO: top and bottom 8 lines cropped on NTSC only; ON: always; OFF: never */
	int no_sprite_limit; /* 1 (default): more than 8 sprites per line are drawn */
	int fourscore; /* Four Score / Famicom adapter: 0 automatic (known games), 1 on, 2 off */
	int region;    /* 0 automatic (header, database), 1 NTSC, 2 PAL, 3 Dendy */
	int use_game;  /* 1: this game has its own file; 0: the console file is used */
} pic_settings;

void pic_defaults(pic_settings *s);
/* Reads key=value lines over s (unknown keys and bad values are ignored). Returns 1 when the file exists. */
int pic_load(const char *path, pic_settings *s);
/* Safe write: path.tmp then rename. Returns 0 on success. */
int pic_save(const char *path, const pic_settings *s);

/* Rows to crop (top and bottom each) for the overscan setting and region: 8 or 0. */
int pic_overscan_rows(const pic_settings *s, int is_pal);

typedef struct {
	float dx, dy, dw, dh;  /* where on the screen */
	int sx, sy, sw, sh;    /* which part of the core's picture (pixels) */
	int logical_rows;      /* visible rows (= sh) */
} pic_rect;

/* src_w x src_h is the core's picture (256x240); crop_rows rows are removed from the top and from the bottom.
 * Shapes (pixels are 8:7 wide unless said otherwise):
 *   ORIGINAL  full screen height, width from the 8:7 pixel aspect of the visible rows (default)
 *   ZOOM      like ORIGINAL with 8 columns removed on each side as a TV hid them
 *   FULL      stretched over the whole screen
 *   PIXEL     whole-number height scale (8:7 or square pixels), centred */
void pic_geometry(const pic_settings *s, int src_w, int src_h, int crop_rows,
                  int scr_w, int scr_h, pic_rect *r);

/* Integer prescale factors for the sharp-bilinear pass (1 = none needed). */
void pic_prescale(const pic_rect *r, int *kx, int *ky);

/* Smoothness governor. Feed it the main loop's work time per frame in microseconds (without the wait for
 * the display). level 0 = every frame drawn, 2 = every other frame is not drawn (emulation and sound stay
 * at full speed). The picture options chosen are never dropped; level 1 is not used. */
typedef struct {
	int level;
	int slow, fast, since_change, hold, backoff;
	int frames, recovered;
} pic_governor;
void pic_gov_init(pic_governor *g);
void pic_gov_update(pic_governor *g, unsigned work_us, unsigned budget_us);

#endif
