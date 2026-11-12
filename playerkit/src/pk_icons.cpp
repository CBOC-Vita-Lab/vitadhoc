/* See pk_icons.h. Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE). */
#include "pk_icons.h"
#include <math.h>

namespace pk {

static const float kPi = 3.14159265f;

void draw_line(Gfx *g, float x0, float y0, float x1, float y1, float t, uint32_t c)
{
	float dx = x1 - x0, dy = y1 - y0, len = sqrtf(dx * dx + dy * dy);
	if (len < 0.001f) return;
	float nx = -dy / len * t * 0.5f, ny = dx / len * t * 0.5f;
	g->fill_tri(x0 + nx, y0 + ny, x0 - nx, y0 - ny, x1 + nx, y1 + ny, c);
	g->fill_tri(x1 + nx, y1 + ny, x0 - nx, y0 - ny, x1 - nx, y1 - ny, c);
}

void draw_arc(Gfx *g, float cx, float cy, float r, float t, float a0, float a1, uint32_t c)
{
	float ri = r - t * 0.5f, ro = r + t * 0.5f;
	int steps = (int) ceilf(fabsf(a1 - a0) / 8.0f);
	if (steps < 1) steps = 1;
	for (int i = 0; i < steps; i++) {
		float s = a0 + (a1 - a0) * i / steps - 1.0f, e = a0 + (a1 - a0) * (i + 1) / steps + 1.0f;   /* a little overlap hides the seams */
		if (i == 0) s += 1.0f;
		if (i == steps - 1) e -= 1.0f;
		float sr = s * kPi / 180.0f, er = e * kPi / 180.0f;
		float ax = cx + cosf(sr) * ri, ay = cy + sinf(sr) * ri, bx = cx + cosf(sr) * ro, by = cy + sinf(sr) * ro;
		float ex = cx + cosf(er) * ri, ey = cy + sinf(er) * ri, fx = cx + cosf(er) * ro, fy = cy + sinf(er) * ro;
		g->fill_tri(ax, ay, bx, by, ex, ey, c);
		g->fill_tri(bx, by, fx, fy, ex, ey, c);
	}
}

static void ring(Gfx *g, float cx, float cy, float r, float t, uint32_t c)
{
	g->stroke_rrect(cx - r, cy - r, 2 * r, 2 * r, r, t, c);
}

void draw_icon(Gfx *g, int icon, float cx, float cy, float s, uint32_t c)
{
	float u = s / 40.0f;      /* the design grid is 40 x 40 */
	switch (icon) {
	case ICON_PLAY:
		g->fill_tri(cx - 9 * u, cy - 14 * u, cx - 9 * u, cy + 14 * u, cx + 14 * u, cy, c);
		break;
	case ICON_LAYERS:
		for (int i = 0; i < 3; i++) {
			float y = cy - 14 * u + i * 11 * u;
			g->fill_rrect(cx - 15 * u, y, 30 * u, 7 * u, 3.5f * u, PK_ALPHA(c, (((c >> 24) * (i == 0 ? 100 : i == 1 ? 70 : 45)) / 100)));
		}
		g->fill_rrect(cx - 15 * u, cy - 14 * u, 30 * u, 7 * u, 3.5f * u, c);
		break;
	case ICON_PEOPLE:
		g->fill_circle(cx - 8 * u, cy - 7 * u, 6 * u, c);
		g->fill_rrect(cx - 16 * u, cy + 1 * u, 16 * u, 13 * u, 7 * u, c);
		g->fill_circle(cx + 9 * u, cy - 5 * u, 5 * u, PK_ALPHA(c, ((c >> 24) * 70) / 100));
		g->fill_rrect(cx + 2 * u, cy + 2 * u, 14 * u, 12 * u, 6 * u, PK_ALPHA(c, ((c >> 24) * 70) / 100));
		break;
	case ICON_SPARK: {
		float a = 16 * u, b = 4 * u;
		g->fill_tri(cx, cy - a, cx - b, cy, cx + b, cy, c);
		g->fill_tri(cx, cy + a, cx - b, cy, cx + b, cy, c);
		g->fill_tri(cx - a, cy, cx, cy - b, cx, cy + b, c);
		g->fill_tri(cx + a, cy, cx, cy - b, cx, cy + b, c);
		g->fill_circle(cx + 12 * u, cy - 12 * u, 3 * u, c);
		break;
	}
	case ICON_GEAR:
		for (int k = 0; k < 8; k++) {
			float a = k * kPi / 4;
			float x0 = cx + cosf(a) * 11 * u, y0 = cy + sinf(a) * 11 * u, x1 = cx + cosf(a) * 17 * u, y1 = cy + sinf(a) * 17 * u;
			draw_line(g, x0, y0, x1, y1, 6 * u, c);
		}
		ring(g, cx, cy, 11 * u, 5 * u, c);
		break;
	case ICON_RESET:
		draw_arc(g, cx, cy, 12 * u, 4.5f * u, -50, 250, c);
		{   /* arrow head at the arc's start (angle -50) pointing clockwise */
			float a = -50 * kPi / 180.0f;
			float px = cx + cosf(a) * 12 * u, py = cy + sinf(a) * 12 * u;
			float tx = -sinf(a), ty = cosf(a);          /* tangent, clockwise */
			float nx = cosf(a), ny = sinf(a);
			g->fill_tri(px + tx * 9 * u, py + ty * 9 * u, px + nx * 8 * u - tx * 1 * u, py + ny * 8 * u - ty * 1 * u,
			            px - nx * 8 * u - tx * 1 * u, py - ny * 8 * u - ty * 1 * u, c);
		}
		break;
	case ICON_GRID:
		for (int i = 0; i < 4; i++) {
			float x = cx - 15 * u + (i % 2) * 17 * u, y = cy - 15 * u + (i / 2) * 17 * u;
			g->fill_rrect(x, y, 13 * u, 13 * u, 3.5f * u, c);
		}
		break;
	case ICON_POWER:
		draw_arc(g, cx, cy + 2 * u, 13 * u, 4.5f * u, -55, 235, c);
		draw_line(g, cx, cy - 16 * u, cx, cy - 2 * u, 4.5f * u, c);
		g->fill_circle(cx, cy - 16 * u, 2.25f * u, c);
		break;
	case ICON_CHEVRON_R:
		draw_line(g, cx - 4 * u, cy - 10 * u, cx + 5 * u, cy, 4.5f * u, c);
		draw_line(g, cx + 5 * u, cy, cx - 4 * u, cy + 10 * u, 4.5f * u, c);
		break;
	case ICON_CHEVRON_L:
		draw_line(g, cx + 4 * u, cy - 10 * u, cx - 5 * u, cy, 4.5f * u, c);
		draw_line(g, cx - 5 * u, cy, cx + 4 * u, cy + 10 * u, 4.5f * u, c);
		break;
	case ICON_CHECK:
		draw_line(g, cx - 10 * u, cy + 1 * u, cx - 3 * u, cy + 8 * u, 5 * u, c);
		draw_line(g, cx - 3 * u, cy + 8 * u, cx + 11 * u, cy - 8 * u, 5 * u, c);
		break;
	default: break;
	}
}

void draw_button_glyph(Gfx *g, int glyph, float cx, float cy, float s, uint32_t ink, uint32_t disc)
{
	g->fill_circle(cx, cy, s * 0.5f, disc);
	float u = s / 28.0f;
	switch (glyph) {
	case GLYPH_CROSS:
		draw_line(g, cx - 6 * u, cy - 6 * u, cx + 6 * u, cy + 6 * u, 2.4f * u, ink);
		draw_line(g, cx + 6 * u, cy - 6 * u, cx - 6 * u, cy + 6 * u, 2.4f * u, ink);
		break;
	case GLYPH_CIRCLE:
		ring(g, cx, cy, 6.5f * u, 2.4f * u, ink);
		break;
	case GLYPH_SQUARE:
		g->stroke_rrect(cx - 6 * u, cy - 6 * u, 12 * u, 12 * u, 1.2f * u, 2.4f * u, ink);
		break;
	default:
		draw_line(g, cx, cy - 7 * u, cx - 7 * u, cy + 5 * u, 2.4f * u, ink);
		draw_line(g, cx - 7 * u, cy + 5 * u, cx + 7 * u, cy + 5 * u, 2.4f * u, ink);
		draw_line(g, cx + 7 * u, cy + 5 * u, cx, cy - 7 * u, 2.4f * u, ink);
		break;
	}
}

}  // namespace pk
