/*
 * SNES wide view adapter: turns what the renderer recorded (core/widecap.h) into a widekit frame description, runs
 * widekit (wk_layers, wk_map, wk_compose) and produces the composed (256 + 2 * margin) x height RGB565 picture:
 * the original 256 columns pixel for pixel in the middle (copied from the very buffer the normal picture comes from),
 * real picture beside them where it can be proven, soft edges elsewhere.
 *
 * Threading (see snes/WIDE-INTEGRATION.md): the capture and the margin drawing happen in the renderer's own pass, from
 * the same state snapshot as the normal picture, on the PPU thread when it is on (else on the emulation thread at the end
 * of the frame). widekit, the learned map and the composition run in the frame hook on that same thread, so the
 * emulation thread never waits for them. The consumer (the frontend's video thread) takes the newest finished frame with
 * snes_wide_frame(). All control calls below are for the thread that runs the frames, between frames.
 *
 * Copyright CBOC-Vita. MIT licence (see snes/frontend/LICENSE). Links only snes/, playerkit/ and widekit/.
 */
#ifndef SNES_WIDE_H
#define SNES_WIDE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SNES_WIDE_MAX_MARGIN 96

typedef struct snes_wide_stats {
	unsigned serial;          /* counts finished wide frames */
	uint32_t frame;           /* emulated frame clock of the picture */
	int available;            /* 0: this frame is not widened (hi-res, interlace, nothing drawn): use the normal picture */
	int width, height, margin;
	int mode7;                /* the frame is a Mode 7 frame (margin drawn directly, always real) */
	int lines;                /* picture lines */
	int lines_drawn;          /* lines whose margin was drawn */
	int lines_nomargin;       /* lines that cannot be widened (hi-res, interlace, mode 2/4/6, mosaic, forced blank) */
	int lines_hud;            /* lines widekit found fixed (status bars) */
	int lines_exact;          /* lines with an exact composite margin */
	int real_l, real_r;       /* mean real pixels per side over the drawn lines (what is proven, in pixels) */
	int layers;               /* bit n: BG n+1 is described to widekit */
	int ring_w[4], ring_h[4]; /* the rings in tiles */
	int kind[4];              /* widekit's verdict per layer (WK_KIND_*) */
	int scene_event, epoch, hs_checked, hs_missed;
	int map_cells, map_rooms, map_bound, map_served;
	uint32_t instr_us;        /* wall time of the hook for this frame (microseconds) */
} snes_wide_stats;

/* Switch the wide view on or off. `margin` = pixels per side (0..96; the frontend takes it from the picture shape: 70 fills
 * a 16:9 screen at square pixels, 42 with the original 4:3 shape); 224 and 239 line pictures get the same margin. Waits for
 * the PPU thread. on = 0 or margin = 0 switches off. */
void snes_wide_set(int on, int margin);
int snes_wide_active(void);
int snes_wide_margin(void);

/* The game that was loaded: its ROM checksum keys the learned map and the profile table. `rom`/`n` is the ROM image
 * (copier header already stripped); title is the internal title. Call after every load, before the first frame. */
void snes_wide_set_game(const uint8_t *rom, size_t n, const char *title);
void snes_wide_clear_game(void);

/* Options: "period" (0/1, periodic foreground continuation, off by default), "fade_px", "appear_frames", "settle_frames",
 * "debug" (provenance picture), "layered" (0: no layer passes: composite only; for cost tests), "learn" (0/1: learned map
 * on, default 1). Returns 0 when the name is known. */
int snes_wide_option(const char *name, int value);
/* widekit profile table (by CRC or title); returns the number of parse problems, messages appended to msgs */
int snes_wide_profiles_load(const char *text, size_t n, char *msgs, size_t cap);

/* The newest finished frame: 1 and the picture (valid until the third next finished frame), or 0 when there is none yet or
 * the frame cannot be widened (then show the normal picture). */
int snes_wide_frame(const uint16_t **rgb, int *width, int *height, int *pitch_px, snes_wide_stats *st);
/* Per row real pixels [left, right] of the last frame and (debug option) the provenance of every pixel; valid with the frame */
const uint8_t *snes_wide_real(void);
const uint8_t *snes_wide_prov(void);

/* Learned map file image: key = ROM CRC (0 = no game); save(NULL, 0) returns the size, save(buf, cap) the bytes written
 * (0 on error); load right after snes_wide_set_game; dirty = cells changed since the last save or load. The frontend does
 * the safe file I/O on its writer thread. Thread-safe against the frame hook. */
uint32_t snes_wide_map_key(void);
size_t snes_wide_map_save(uint8_t *buf, size_t cap);
int snes_wide_map_load(const uint8_t *buf, size_t n);
int snes_wide_map_dirty(void);
void snes_wide_map_mark_saved(void);

/* After a state load or a reset the stamps are meaningless: widekit starts a new scene. (Called by the host itself.) */
void snes_wide_reset(void);

/* Test hooks */
typedef struct snes_wide_dump {
	/* the last frame's candidate strips and decisions, for the twin's truth check */
	int m, h;
	const uint16_t *wide;   /* the composed frame */
} snes_wide_dump;

#ifdef __cplusplus
}
#endif
#endif
