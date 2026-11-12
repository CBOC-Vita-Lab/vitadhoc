/*
 * Icons and button glyphs drawn from shapes (no image files), centred on a
 * point, `size` px across, in one colour.
 *
 * Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE).
 */
#ifndef PK_ICONS_H
#define PK_ICONS_H

#include "pk_gfx.h"

namespace pk {

enum IconId {
	ICON_PLAY = 0, ICON_LAYERS, ICON_PEOPLE, ICON_SPARK, ICON_GEAR, ICON_RESET, ICON_GRID, ICON_POWER,
	ICON_CHEVRON_R, ICON_CHEVRON_L, ICON_CHECK, ICON_COUNT
};
void draw_icon(Gfx *g, int icon, float cx, float cy, float size, uint32_t rgba);

/* PlayStation face-button glyphs: a ring or disc with the symbol inside */
enum Glyph_ { GLYPH_CROSS = 0, GLYPH_CIRCLE, GLYPH_SQUARE, GLYPH_TRIANGLE };
void draw_button_glyph(Gfx *g, int glyph, float cx, float cy, float size, uint32_t ink, uint32_t disc);

/* a thick line (two triangles) */
void draw_line(Gfx *g, float x0, float y0, float x1, float y1, float t, uint32_t rgba);
/* an arc of a ring between two angles (degrees, 0 = right, clockwise on screen) */
void draw_arc(Gfx *g, float cx, float cy, float r, float t, float a0, float a1, uint32_t rgba);

}  // namespace pk

#endif
