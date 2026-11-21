/*
 * Picture layout and settings file of the Sega player (Mega Drive / Genesis, Master System, Game Gear, SG-1000).
 * Portable (no console calls) so the Linux twin tests it. Copyright CBOC-Vita. MIT licence (see md/frontend/LICENSE).
 *
 * The screen sizes follow the console's own picture: a Mega Drive is 320x224 (H40) or 256x224 (H32), 240 lines in PAL, shown at the
 * display aspect ratio the core reports (md_video_info.dar), a Master System 256x192 (PAL 224/240 lines in some games), a Game Gear
 * 160x144 like a Game Boy, an SG-1000 256x192.
 */
#ifndef MD_PICTURE_H
#define MD_PICTURE_H

#include <stddef.h>

enum { PIC_ORIGINAL = 0, PIC_ZOOM, PIC_FULL, PIC_PIXEL, PIC_SHAPES };
enum { PIC_SHARP = 0, PIC_SMOOTH };
/* the pixel art scaler row: Auto = the look governor picks the best that holds full speed (the system's ceiling first) */
enum { SCALER_AUTO = 0, SCALER_OFF, SCALER_MMPX2, SCALER_XBR2, SCALER_XBR3, SCALER_CHOICES };

typedef struct {
	int shape;     /* PIC_ORIGINAL (default), PIC_ZOOM, PIC_FULL, PIC_PIXEL */
	int pixel_sq;  /* pixel perfect: 0 = the console's pixel shape, 1 = square pixels */
	int quality;   /* PIC_SHARP (default: sharp bilinear fit) or PIC_SMOOTH */
	int glow;      /* ambient glow 0 (off) .. 10 */
	int scan;      /* CRT scanlines 0 (off) .. 10 */
	int grid;      /* LCD grid 0 (off) .. 10 */
	int scaler;    /* SCALER_* (default Auto) */
	int gg_colors; /* Game Gear: 1 = LCD colours (softer, lifted blacks), 0 = the core's colours */
	int use_game;  /* 1: this game has its own file; 0: the console file is used */
	int wide;      /* wide view (the console's picture as wide as the screen): 0 auto, 1 on, 2 off */
	int tier_steps;/* not stored: the tiers the look governor has taken off the scaler's ceiling (pic_scaler_effective) */
} pic_settings;

void pic_defaults(pic_settings *s);
/* Reads key=value lines over s (unknown keys and bad values are ignored). Returns 1 when the file exists. */
int pic_load(const char *path, pic_settings *s);
/* Safe write: path.tmp then rename. Returns 0 on success. */
int pic_save(const char *path, const pic_settings *s);

/* What the layout needs to know about the picture in the buffer (from md_video_info). */
struct md_video_info;
typedef struct {
	int w, h;            /* the picture in the buffer */
	int logical_rows;    /* console lines: h, or h / 2 for an interlaced double height picture */
	double dar;          /* display aspect ratio of the whole picture (0 = square pixels: w / h) */
	double par;          /* pixel aspect ratio (width / height of one pixel; 0 = 1) */
	int system;          /* MD_SYS_* */
} pic_info;

typedef struct {
	float dx, dy, dw, dh;  /* where on the screen */
	int sx, sy, sw, sh;    /* which part of the picture (pixels) */
	int logical_rows;      /* visible console lines */
} pic_rect;

/* The pic_info of the console's normal picture (the Game Gear's 160x144 window also when the wide view's 256 wide band is drawn). */
struct md_video_info;
pic_info pic_info_from_video(const struct md_video_info *vi);

/* crop_top / crop_bottom: blank rows (in picture rows) the Zoom shape may remove */
void pic_geometry(const pic_settings *s, const pic_info *in, int crop_top, int crop_bottom, int scr_w, int scr_h, pic_rect *r);

/* Wide view. pic_wide_margin: pixels per side (0..max_m) of extra picture that make the console's picture as wide as the screen for
 * the shape and the screen (0 when the shape is Full screen or nothing fits). `in` is the console's normal picture (the Game Gear's
 * 160x144 window, not its 256 wide band). The wide picture is (in->w + 2 * M) columns; pic_geometry_wide places it with the same scale per
 * column as the normal picture and trims the same number of columns from both sides if it is wider than the screen. */
int pic_wide_margin(const pic_settings *s, const pic_info *in, int scr_w, int scr_h, int max_m);
void pic_geometry_wide(const pic_settings *s, const pic_info *in, int wide_w, int crop_top, int crop_bottom, int scr_w, int scr_h, pic_rect *r);

/* Integer prescale factors for the sharp-bilinear pass (1 = none needed). */
void pic_prescale(const pic_rect *r, int *kx, int *ky);

/* Number of leading / trailing rows that are completely black (RGB565 0) in a picture, at most max_rows. */
int pic_blank_rows(const unsigned short *px, int pitch_bytes, int w, int h, int from_top, int max_rows);

/* ---- the pixel art scaler ceilings (docs/DEFAULTS-1.4.md section 5, with the Sega numbers of md/vita/README.md) ----
 * The tier Auto starts at, per system and picture size: 0 = xBR 3x, 1 = xBR 2x, 2 = none (sharp bilinear); MMPX 2x is only ever chosen by hand. A picture
 * that is interlaced double height or wider than 352 columns never gets a scaler. */
int pic_scaler_ceiling(int system, int w, int h);

/* the scaler the settings and the governor give: `tier_steps` is how many tiers the governor has taken off the ceiling.
 * Returns a SCALER_* value other than AUTO (SCALER_OFF = sharp bilinear). A hand-set scaler row is never eased. */
int pic_scaler_effective(const pic_settings *s, int ceiling_tier, int tier_steps);

/* the Game Gear's LCD colours: a table of 65536 RGB565 values, built once */
const unsigned short *pic_lcd_table(void);

#endif
