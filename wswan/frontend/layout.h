/*
 * Picture layout, rotation and Vita pad mapping for the WonderSwan player. Portable C (no Vita calls). Copyright CBOC-Vita.
 * MIT licence.
 */
#ifndef WSWAN_LAYOUT_H
#define WSWAN_LAYOUT_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Vita buttons as the app sees them (digital; the sticks are already thresholded by the caller). */
enum {
	WSL_UP = 1 << 0, WSL_DOWN = 1 << 1, WSL_LEFT = 1 << 2, WSL_RIGHT = 1 << 3,
	WSL_CROSS = 1 << 4, WSL_CIRCLE = 1 << 5, WSL_SQUARE = 1 << 6, WSL_TRIANGLE = 1 << 7,
	WSL_L = 1 << 8, WSL_R = 1 << 9, WSL_START = 1 << 10, WSL_SELECT = 1 << 11,
	WSL_RS_UP = 1 << 12, WSL_RS_DOWN = 1 << 13, WSL_RS_LEFT = 1 << 14, WSL_RS_RIGHT = 1 << 15
};
/* How a vertical scene is presented. */
enum {
	WSL_UPRIGHT = 0,   /* default: the Vita stays landscape, the picture is turned 90 degrees CCW and shown upright (350x544) */
	WSL_TURNED = 1     /* the player turns the Vita 90 degrees counter-clockwise, as one turns the WonderSwan: the frame is
	                      drawn unrotated and fills the screen (846x544), controls follow the Vita's physical layout */
};

/* WSWAN_* key mask for a Vita button mask. `vertical` = wswan_host_orientation() (the effective one).
 *   horizontal : d-pad -> X1 (up) X2 (right) X3 (down) X4 (left); right stick -> Y1..Y4; circle -> A, cross -> B,
 *                square -> B, triangle -> A; start -> Start
 *   vertical UPRIGHT : the picture is turned CCW, so "up on screen" is the unit's right side:
 *                d-pad up -> Y2, right -> Y3, down -> Y4, left -> Y1;  face buttons (north triangle -> X2, east circle -> X3,
 *                south cross -> X4, west square -> X1); L -> A, R -> B; start -> Start
 *   vertical TURNED : physical 1:1: d-pad up -> Y1, right -> Y2, down -> Y3, left -> Y4; triangle -> X1, circle -> X2,
 *                cross -> X3, square -> X4; L -> A, R -> B
 * The caller handles WSL_SELECT (orientation override toggle). */
unsigned wsl_map_pad(unsigned vita, int vertical, int presentation);

typedef struct { int x, y, w, h; float scale; int rotated; int src_w, src_h; } wsl_rect;
/* Where the picture goes on a scr_w x scr_h screen. integer: largest whole-number scale that fits (min 1), else best
 * fit (largest scale that fits). `rotated` = 1 means the caller must draw the 144x224 CCW rotated copy (wsl_rotate_ccw). */
void wsl_layout(int vertical, int presentation, int scr_w, int scr_h, int integer, wsl_rect *r);

/* dst (144 wide x 224 high) = src (224 x 144) turned 90 degrees counter-clockwise: the source's right edge becomes the top. */
void wsl_rotate_ccw(uint16_t *dst, const uint16_t *src);

#ifdef __cplusplus
}
#endif
#endif
