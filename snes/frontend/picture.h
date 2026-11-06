/*
 * Picture layout, settings file and smoothness governor of the SNES program.
 * Portable (no Vita calls) so the Linux build can test it. Copyright CBOC-Vita.
 * MIT licence (see snes/frontend/LICENSE).
 */
#ifndef SNES_PICTURE_H
#define SNES_PICTURE_H

#include <stddef.h>

enum { PIC_ORIGINAL = 0, PIC_ZOOM, PIC_FULL, PIC_PIXEL, PIC_SHAPES };
enum { PIC_SHARP = 0, PIC_SMOOTH };

typedef struct {
	int shape;     /* PIC_ORIGINAL (default), PIC_ZOOM, PIC_FULL, PIC_PIXEL */
	int pixel_sq;  /* pixel perfect: 0 = 8:7 pixels, 1 = square pixels */
	int quality;   /* PIC_SHARP (default) or PIC_SMOOTH */
	int glow;      /* side glow 0 (off) .. 10 */
	int scan;      /* CRT scanlines 0 (off) .. 10 */
	int grid;      /* LCD grid 0 (off) .. 10 */
	int multitap;  /* Super Multitap in port 2: 0 automatic (known games), 1 on, 2 off */
	int use_game;  /* 1: this game has its own file; 0: the console file is used */
} pic_settings;

void pic_defaults(pic_settings *s);
/* Reads key=value lines over s (unknown keys and bad values are ignored). Returns 1 when the file exists. */
int pic_load(const char *path, pic_settings *s);
/* Safe write: path.tmp then rename. Returns 0 on success. */
int pic_save(const char *path, const pic_settings *s);

typedef struct {
	float dx, dy, dw, dh;  /* where on the screen */
	int sx, sy, sw, sh;    /* which part of the core's picture (pixels) */
	int logical_rows;      /* visible rows in SNES lines (hi-res/interlace halves them) */
} pic_rect;

/* src_w x src_h is the core's picture (256/512 wide; 224, 239, 448 or 478 high); crop_top/crop_bottom are
 * blank rows (in picture rows) the Zoom shape may remove. */
void pic_geometry(const pic_settings *s, int src_w, int src_h, int crop_top, int crop_bottom,
                  int scr_w, int scr_h, pic_rect *r);

/* Integer prescale factors for the sharp-bilinear pass (1 = none needed). */
void pic_prescale(const pic_rect *r, int *kx, int *ky);

/* Number of leading / trailing rows that are completely black (RGB565 0) in a picture, at most max_rows. */
int pic_blank_rows(const unsigned short *px, int pitch_bytes, int w, int h, int from_top, int max_rows);

/* Smoothness governor. Feed it the main loop's work time per frame in microseconds (without the wait for
 * the display). level 0 = everything, 1 = picture extras dropped, 2 = additionally every other frame is not
 * drawn (emulation and sound stay at full speed). */
typedef struct {
	int level;
	int slow, fast, since_change, hold, backoff;
	int frames, recovered;
} pic_governor;
void pic_gov_init(pic_governor *g);
void pic_gov_update(pic_governor *g, unsigned work_us, unsigned budget_us);

#endif
