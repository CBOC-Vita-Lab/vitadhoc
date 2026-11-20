/*
 * The Sega player's wide view: a picture as wide as the Vita's screen without stretching or cropping the console's own picture. The
 * console's picture stays pixel exact in the middle; beside it come strips of real picture where that is provable, and a soft extension
 * of the picture's edge where it is not (widekit's rules: wk_layers decides which margin pixels are real, wk_map remembers what the game
 * has shown, wk_compose blends).
 *
 *   Mega Drive / Genesis   the VDP worker draws the strips (core/vdp_render.c, md_host_set_wide_margin) from the planes and sprites that
 *                          exist beyond the screen's edge; this file decides which of them are real from the name tables (what the game
 *                          wrote and showed). Beyond what the name table still holds, cells the game has shown at those world columns
 *                          before (the learned map) are given back to the VDP as learned rows (md_host_wide_row), so the same renderer
 *                          draws them with the other plane and the sprites; a strip pixel is called real only when the row it was drawn
 *                          from is the row the map vouches for now.
 *   Master System          nothing exists beside the 256 pixels (the tile map is exactly the screen wide): the strips are drawn here
 *                          from the learned map (cells the picture has shown, while their tile graphics are still what they were).
 *   Game Gear              the VDP always draws 256x192 and the LCD shows the middle 160x144; the wide view shows the 256 columns of the
 *                          LCD's rows (option gg_band) where the name table can be trusted, then learned cells, then the soft extension.
 *   SG-1000                no scroll registers (games move the picture by rewriting the name table): the soft extension only.
 *
 * Three steps. frame() on the emulating thread after md_host_run_frame() hands the frame to the analysis, which runs on the VDP worker
 * thread when the core has one (md_host_wide_run: the emulating thread does not wait) and fills a Meta; compose() runs on the drawing
 * thread from the picture buffer and that Meta (it waits for the Meta if the analysis has not finished). What the analysis decides does
 * not depend on which thread runs it. A Wide is used by one emulating thread and one composing thread.
 *
 * Licence: MIT, Copyright CBOC-Vita. Links widekit (MIT) and md/frontend/host.h only.
 */
#ifndef MD_WIDE_H
#define MD_WIDE_H

#include <stdint.h>
#include "host.h"

#ifdef __cplusplus
#include <atomic>
#include "wk_types.h"
#include "wk_layers.h"
#include "wk_map.h"
#include "wk_compose.h"

namespace mdw {

/* what compose() needs of one analysed frame (a ring of these goes with the pictures in flight) */
struct Meta {
	Meta();
	~Meta();
	std::atomic<int> ready;   /* 1: the analysis has filled this Meta (frame() clears it, the analysis sets it last) */
	int valid;            /* 0: show the plain picture */
	int system;           /* MD_SYS_* */
	int vw, vh;           /* the view: the console's picture (the window of the Game Gear) */
	int margin;           /* the wide picture's margin per side */
	int real_cap;         /* the largest real margin the governor allows now (<= margin); 0: soft extension only */
	int view_off;         /* pixels from the start of the picture's data to the view's column 0 (the player sets it: where its buffer puts the view) */
	int have_strips;      /* the picture buffer holds candidate strips beside the view (Mega Drive, the Game Gear's band) */
	int own_strips;       /* the strips are in px (drawn by the analysis: the 8 bit consoles), not beside the view */
	int nlayers;
	uint32_t frame;
	uint8_t planes;       /* planes drawn into the strips of this frame (bit 0 A, bit 1 B); the others are transparent there */
	uint8_t all_opaque;   /* every strip pixel inside real[] is opaque */
	uint8_t hud[256];     /* per line: a fixed line (status bar): not widened */
	uint16_t real[2][256];/* per side and line: the strip pixels that are real picture, from the view's edge outwards */
	uint16_t pic[2][256]; /* ... of which this many are in the picture buffer beside the view; the rest are in px */
	uint32_t opq[256][2][3];  /* Mega Drive: strip pixels that are opaque, bit d = d pixels from the view's edge */
	uint32_t epoch;       /* widekit's scene counter and scene event of the frame */
	int scene_event;
	uint16_t *px;         /* own_strips: [side][line][d], d = distance from the view's edge; vh * margin pixels per side */
	int px_cap;
private:
	Meta(const Meta &);
	Meta &operator=(const Meta &);
};

struct Stats {
	unsigned frames, analysed, composed;
	int scene_events;
	int hs_checked, hs_missed;
	int real_pct;          /* of the margin pixels of the last composed frame: real / total, percent */
	int soft_lines;        /* lines of the last composed frame with no real strip */
	int map_cells, map_rooms, map_bound;      /* the learned map */
	unsigned map_px;       /* real strip pixels of the last analysed frame that came from the learned map */
	unsigned map_rejected; /* learned cells not shown because the strip was not drawn from them or their tile had changed (sum) */
	unsigned rows;         /* learned rows given to the VDP for the next frame (Mega Drive) */
	unsigned map_skips;    /* analysed frames the map had nothing new to look at */
	/* set_check(true), Mega Drive: learned cells shown as real (first time each), those the game later showed on screen at the same place,
	 * and those of them that were then a different cell (a wrong tile was shown) */
	unsigned chk_claims, chk_seen, chk_wrong;
};

class Wide {
public:
	Wide();
	~Wide();
	void reset();                                        /* a new game, a state load, a rewind: forget everything (emulating thread) */
	/* emulating thread, after md_host_run_frame() of a drawn frame. `margin`: the wide picture's margin per side (0 = off for this frame).
	 * The picture must have been drawn with md_host_set_wide_margin(strip_px) (Mega Drive) or gg_band (Game Gear). *m is filled by the
	 * analysis (m->ready); it must stay alive until then (wait(), or the next frame() / sync()). */
	void frame(uint32_t frame, const md_video_info &vi, int margin, int strip_px, Meta *m);
	/* frame() and wait for it: for callers with one thread */
	void analyse(uint32_t frame, const md_video_info &vi, int margin, int strip_px, Meta *m) { this->frame(frame, vi, margin, strip_px, m); sync(); }
	void sync();                                         /* emulating thread: the analysis handed over last has finished */
	static void wait(const Meta &m);                     /* any thread: until m.ready */
	static void set_yield(void (*fn)(void));             /* what wait() calls between looks (default: nothing, it spins) */
	/* drawing thread. view0 = pixel (0,0) of the view in the picture buffer, strips beside it at columns -margin.. and vw.., rows of
	 * pitch_px. Returns the wide picture (vw + 2 margin) x vh (valid until the next compose) or NULL when it cannot be made. */
	const uint16_t *compose(const Meta &m, const uint16_t *view0, int pitch_px, int *w, int *h);
	void set_real_cap_default(int px) { cap_default_ = px; }
	void set_learn(bool on) { learn_ = on; }             /* the learned map (on by default); before the first frame() */
	void set_prov(bool on) { want_prov_ = on; }          /* tests: keep widekit's provenance of the last composed picture */
	void set_check(bool on) { check_ = on; }             /* tests: hindsight on the learned cells (Stats.chk_*) */
	const uint8_t *last_prov() const { return prov_; }
	void census(Stats *s);                               /* emulating thread (waits for the analysis) */
	/* the real margin the last composed frame had per line (tests, the twin's truth check) */
	int last_real(int side, int y) const { return y >= 0 && y < 256 ? lastreal_[side][y] : 0; }

private:
	struct Job { uint32_t frame; md_video_info vi; int margin, strip_px, cap; Meta *m; int planes; };
	static void hook(void *user, const md_wide_frame *f);
	void run(const md_wide_frame &f);
	void free_all();
	void ensure_layers();
	void ensure_map(int system);
	void ensure_compose(int vw, int vh, int margin);
	void take_dirty(const md_wide_frame &f);
	uint32_t tile_hash(const uint8_t *vram, unsigned t);
	bool cell_ok(const md_wide_frame &f, const wk_cell &c, unsigned tile_mask);
	void pass_plane(const md_wide_frame &f, int p, unsigned base, unsigned shift, unsigned tile_mask, int mw, int mh, uint32_t frame, bool m4);
	void rows_off();
	struct Claim { int32_t col, row; uint32_t epoch; uint16_t code; uint8_t l, state; };   /* state 0 free, 1 shown, 2 checked */
	Claim *claim_find(int l, int col, int row, bool add);
	void check_seen(int H);
	bool describe_md(const md_wide_frame &f, const md_video_info &vi, uint32_t frame, int margin);
	bool describe_m4(const md_wide_frame &f, const md_video_info &vi, uint32_t frame, int margin, bool gg);
	void decide_md(const md_wide_frame &f, Meta *m, int H);
	void verify_md(const md_wide_frame &f, int H);
	void rows_md(const md_wide_frame &f, int H);
	void strips_m4(const md_wide_frame &f, Meta *m, bool gg);

	Job job_;
	wk_layers *L_;
	wk_map *M_;
	int map_system_;
	wk_compose *C_;
	int cvw_, cvh_, cm_;
	wk_frame *fr_;
	wk_out *out_;
	wk_out *ana2_;
	uint16_t *wide_;      /* the composed picture */
	uint8_t *prov_;
	uint8_t *opq_[2];     /* opaque flags of the strips for wk_compose */
	uint16_t *cand_[2];   /* strips for wk_compose: vh x margin, index d = distance from the view edge */
	/* name table memory of the planes: hash, code and stamp as widekit gets them, the true write stamps, the words */
	uint32_t *hash_[2], *stamp_[2], *wstamp_[2], *code_[2];
	uint16_t *prev_[2];
	uint8_t *line_on_[2];
	uint8_t *used_[2];    /* per tile: the plane's name table uses it */
	uint32_t *thprev_[2]; /* per tile: its hash at the plane's last pass (0 = not used then) */
	int nused_[2], settle_[2], need_[2], have_gid_[2];
	uint32_t gid_[2];
	unsigned span_base_[2], span_len_[2];
	uint32_t *th_;        /* per 32 byte tile: content hash */
	uint8_t *thok_;
	int have_prev_, map_w_, map_h_, layout_key_;
	uint8_t cram_prev_[128];
	int have_cram_;
	int cap_default_;
	bool learn_, want_prov_;
	int lastreal_[2][256];
	/* learned rows given to the VDP (Mega Drive): per plane and ring cell the word the strips of the frame being analysed were drawn with */
	uint16_t *ovr_code_[2];
	uint32_t *ovr_gen_[2];
	uint8_t ovr_row_[2][128];
	uint32_t ovr_live_;   /* the generation of the rows the frame being analysed was drawn with (0 = none) */
	uint32_t ovr_next_;
	uint16_t *rowbuf_;
	uint16_t *real0_[2];  /* mode 4: what the ring alone made real, per side and line */
	uint32_t seam_prev_;
	unsigned content_gen_;   /* counts the name table passes */
	uint32_t map_key_;
	int map_key_ok_;
	bool check_;
	Claim *claims_;
	uint32_t crc_;
	Stats st_;
	std::atomic<int> c_real_pct_, c_soft_lines_;
	std::atomic<unsigned> c_composed_;
	int planes_cur_;      /* the planes mask the core drew the frame being handed over with */
	int planes_next_;     /* what the last analysis wants for the frames to come */
};

}  // namespace mdw
#endif
#endif
