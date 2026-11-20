/*
 * The Sega player's wide view: a picture as wide as the Vita's screen without stretching or cropping the console's own picture. The
 * console's picture stays pixel exact in the middle; beside it come strips of real picture where that is provable, and a soft extension
 * of the picture's edge where it is not (widekit's rules: wk_layers decides which margin pixels are real, wk_compose blends them).
 *
 *   Mega Drive / Genesis   the VDP worker draws the strips (core/vdp_render.c, md_host_set_wide_margin) from the planes and sprites that
 *                          exist beyond the screen's edge; this file decides which of them are real from the name table (what the game
 *                          wrote and showed) and composes.
 *   Game Gear              the VDP always draws 256x192 and the LCD shows the middle 160x144; the wide view shows the 256 columns of the
 *                          LCD's rows (option gg_band) and fades out where the game leaves stale tiles outside its window.
 *   Master System, SG-1000 nothing exists beside the 256 pixels (the tile map is exactly the screen wide): the soft extension.
 *
 * Two halves. analyse() runs on the emulating thread right after md_host_run_frame (about 40 K instructions: name table and tile diff,
 * wk_layers_update) and fills a Meta. compose() runs on the drawing thread from the picture buffer and that Meta (wk_compose). The
 * handles are single threaded: analyse() on one thread, compose() on one other, never the same Wide from two threads at once.
 *
 * Licence: MIT, Copyright CBOC-Vita. Links widekit (MIT) and md/frontend/host.h only.
 */
#ifndef MD_WIDE_H
#define MD_WIDE_H

#include <stdint.h>
#include "host.h"

#ifdef __cplusplus
#include "wk_types.h"
#include "wk_layers.h"
#include "wk_compose.h"

namespace mdw {

/* what compose() needs of one analysed frame (about 17 KB: a ring of these goes with the pictures in flight) */
struct Meta {
	int valid;            /* 0: show the plain picture */
	int system;           /* MD_SYS_* */
	int vw, vh;           /* the view: the console's picture (the window of the Game Gear) */
	int margin;           /* the wide picture's margin per side */
	int real_cap;         /* the largest real margin the governor allows now (<= margin); 0: soft extension only */
	int have_strips;      /* the picture buffer holds candidate strips beside the view (Mega Drive, Game Gear) */
	uint32_t frame;
	uint8_t strip[256];   /* per line: 1 if the strip beside this line was drawn */
	uint8_t on[256];      /* per line: bit 0 plane A (the Game Gear's one layer), bit 1 plane B were described to widekit on this line */
	wk_out ana;           /* decisions of widekit for the planes; compose() derives the one picture layer from it */
	int nlayers;
};

struct Stats {
	unsigned frames, analysed, composed;
	int scene_events;
	int hs_checked, hs_missed;
	int real_pct;          /* of the margin pixels of the last composed frame: real / total, percent */
	int soft_lines;        /* lines of the last frame with no real strip */
};

class Wide {
public:
	Wide();
	~Wide();
	void reset();                                        /* a new game, a state load, a rewind: forget everything */
	/* emulating thread, after md_host_run_frame() of a drawn frame. `margin`: the wide picture's margin per side (0 = off for this frame).
	 * The picture must have been drawn with md_host_set_wide_margin(strip_px) (Mega Drive) or gg_band (Game Gear). */
	void analyse(uint32_t frame, const md_video_info &vi, int margin, int strip_px, Meta *m);
	/* drawing thread. view0 = pixel (0,0) of the view in the picture buffer, strips beside it at columns -margin.. and vw.., rows of
	 * pitch_px. Returns the wide picture (vw + 2 margin) x vh (valid until the next compose) or NULL when it cannot be made. */
	const uint16_t *compose(const Meta &m, const uint16_t *view0, int pitch_px, int *w, int *h);
	void set_real_cap_default(int px) { cap_default_ = px; }
	void census(Stats *s) const { *s = st_; }
	/* the real margin the last analysed frame had per line (tests, the twin's truth check) */
	int last_real(int side, int y) const { return y >= 0 && y < 256 ? lastreal_[side][y] : 0; }

private:
	void free_all();
	void ensure_layers();
	void ensure_compose(int vw, int vh, int margin);
	bool describe_md(const md_wide_frame &f, const md_video_info &vi, uint32_t frame, int margin);
	bool describe_gg(const md_wide_frame &f, const md_video_info &vi, uint32_t frame, int margin);
	void tiles_diff(const uint8_t *vram, uint32_t frame);

	wk_layers *L_;
	wk_compose *C_;
	int cvw_, cvh_, cm_;
	wk_frame *fr_;
	wk_out *out_;
	wk_out *ana2_;
	uint16_t *wide_;      /* the composed picture */
	int wide_cap_;
	uint16_t *cand_[2];   /* strips for wk_compose: vh x margin, index d = distance from the view edge */
	/* name table memory of the planes: hash, write stamp, previous words */
	uint32_t *hash_[2], *stamp_[2];
	uint16_t *prev_[2];
	uint8_t *line_on_[2];
	uint8_t *vram_prev_;  /* 64 KB copy of the VRAM as of the last analysed frame */
	uint32_t *tilever_;   /* per 32 byte tile: counts the changes of its pixels */
	int have_prev_, map_w_, map_h_, layout_key_;
	uint32_t gfx_gen_;
	uint8_t cram_prev_[128];
	int have_cram_;
	int cap_default_;
	int lastreal_[2][256];
	Stats st_;
	uint32_t analysed_frame_;
};

}  // namespace mdw
#endif
#endif
