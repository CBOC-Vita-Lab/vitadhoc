/*
 * playerkit save states: ten slots with thumbnails, a "resume point" for
 * Continue, one level of undo for a load. The folder layout and picture
 * formats are the main app's own (see playerkit/FORMATS.md), so the home
 * screen's shelf shows SNES and NES states with no special cases.
 *
 *   <root>/states/<console>/<name>.st0 .. .st9      the ten slots (slot 1 = .st0)
 *   <root>/states/<console>/<name>.stN.thumb        64x42 picture (STH1)
 *   <root>/states/<console>/<name>.stN.shot         up to 240x160 picture (STH1, stride 256)
 *   <root>/states/<console>/<name>.stN.meta         playerkit's own note: time, size, CRC (optional for readers)
 *   <root>/states/<console>/<name>.rsm              Continue (where the game was left)
 *   <root>/states/<console>/<name>.rsm.pt           which patch that point was made with
 *   <root>/scenes/<name>.shot                       the home screen's picture of the game
 *
 * Every write is temp + read back + swap (pk::atomic_write): a failed save
 * never destroys the previous slot.
 *
 * Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE).
 */
#ifndef PK_STATES_H
#define PK_STATES_H

#include "pk_util.h"
#include <stdint.h>
#include <string>
#include <vector>

namespace pk {

enum { STATE_SLOTS = 10 };
enum { THUMB_W = 64, THUMB_H = 42, THUMB_HEADER = 16 };
enum { SHOT_MAX_W = 240, SHOT_MAX_H = 160, SHOT_STRIDE = 256 };

/* A game frame: libretro RGB565 (red in the high bits), w x h at pitch_bytes. */
struct Frame {
	const uint16_t *px;
	int w, h;
	int pitch_bytes;
	Frame() : px(0), w(0), h(0), pitch_bytes(0) {}
	Frame(const uint16_t *p, int ww, int hh, int pitch) : px(p), w(ww), h(hh), pitch_bytes(pitch) {}
	bool valid() const { return px && w > 0 && h > 0 && pitch_bytes >= w * 2; }
};

struct StateLayout {
	std::string root;       /* "ux0:data/GBAdhoc" */
	std::string console;    /* "snes", "nes" (folder under states/) */
	std::string name;       /* the game's file name without extension */
	std::string ext_tag;    /* "" ; the GBA side uses ".gb"/".gbc" for Game Boy files */
	std::string states_dir() const;
	std::string slot_path(int slot) const;        /* slot 1..10 -> name.st0..st9 */
	std::string resume_path() const;
	std::string undo_path() const;
	std::string scene_path() const;
};

enum StateStatus {
	ST_OK = 0,
	ST_EMPTY,          /* nothing in that slot */
	ST_READ,           /* the file could not be read */
	ST_DAMAGED,        /* the file does not match what was saved (size or CRC) */
	ST_TOO_BIG,
	ST_WRITE,          /* could not write (see write_rc) */
	ST_BAD_SLOT
};
const char *state_status_text(StateStatus s);

struct SaveReport {
	StateStatus status;
	WriteResult write_rc;
	bool thumb_ok, shot_ok, meta_ok;     /* the companions; a failed one is removed and does not fail the save */
	std::string text() const;            /* the reason for the player, "" when all is well */
	SaveReport() : status(ST_OK), write_rc(W_OK), thumb_ok(false), shot_ok(false), meta_ok(false) {}
};

struct SlotInfo {
	bool present;
	uint64_t size;
	int64_t saved;          /* Unix seconds (the .meta's, else the file's) */
	int64_t play_seconds;   /* from the .meta, -1 when unknown */
	bool has_thumb;
	SlotInfo() : present(false), size(0), saved(0), play_seconds(-1), has_thumb(false) {}
};

/* "3 Oct 2026, 14:32" (local time), the main app's stamp. "" for 0. */
std::string stamp_text(int64_t unix_seconds);

/* Builds the two pictures from a frame (box filter). thumb: THUMB_W*THUMB_H BGR565 (blue high),
 * the frame shrunk to fit and centred on black. shot: rows of SHOT_STRIDE texels, `h` rows, and
 * the size of the picture in out_w/out_h. */
void make_thumb(const Frame &f, uint16_t out[THUMB_W * THUMB_H]);
void make_shot(const Frame &f, std::vector<uint16_t> &rows, int *out_w, int *out_h);
/* The file bytes of each (16-byte header + body). */
std::vector<uint8_t> thumb_file(const Frame &f);
std::vector<uint8_t> shot_file(const Frame &f);
/* Reads a .thumb / .shot back into libretro RGB565 pixels (for drawing); false if absent/invalid. */
bool parse_thumb(const std::vector<uint8_t> &file, std::vector<uint16_t> &rgb565, int *w, int *h);
bool parse_shot(const std::vector<uint8_t> &file, std::vector<uint16_t> &rgb565, int *w, int *h);

class States {
public:
	explicit States(const StateLayout &l) : l_(l) {}
	const StateLayout &layout() const { return l_; }
	bool ensure_dirs() const;

	SlotInfo info(int slot) const;
	/* all ten, the way the page wants them */
	void scan(SlotInfo out[STATE_SLOTS]) const;
	int used_count() const;

	SaveReport save(int slot, const uint8_t *data, size_t n, const Frame &frame, int64_t play_seconds = -1);
	StateStatus load(int slot, std::vector<uint8_t> &out) const;
	bool remove(int slot);
	bool read_thumb(int slot, std::vector<uint16_t> &rgb565, int *w, int *h) const;
	bool read_shot(int slot, std::vector<uint16_t> &rgb565, int *w, int *h) const;

	/* Continue */
	SaveReport save_resume(const uint8_t *data, size_t n, const Frame &frame, int64_t play_seconds,
	                       const std::string &patch_key);
	bool resume_present() const;
	StateStatus load_resume(std::vector<uint8_t> &out) const;
	enum PatchMatch { PATCH_UNKNOWN, PATCH_SAME, PATCH_DIFFERENT };
	PatchMatch resume_patch(const std::string &patch_key) const;
	bool remove_resume();
	/* True when a resume point should be written: `interval_ms` since the last one (or none yet). */
	bool resume_due(uint64_t now_ms, uint64_t interval_ms = 60000) const;
	void resume_mark(uint64_t now_ms) { last_resume_ms_ = now_ms; have_resume_mark_ = true; }

	/* one level of undo for a state load */
	bool undo_store(const uint8_t *data, size_t n);
	StateStatus undo_fetch(std::vector<uint8_t> &out) const;
	bool undo_available() const;
	void undo_clear();

	/* the home screen's picture of the game */
	bool save_scene(const Frame &frame);

private:
	StateLayout l_;
	uint64_t last_resume_ms_ = 0;
	bool have_resume_mark_ = false;
	SaveReport write_set(const std::string &path, const uint8_t *data, size_t n, const Frame &frame,
	                     int64_t play_seconds, bool with_shot);
	StateStatus read_checked(const std::string &path, std::vector<uint8_t> &out) const;
};

}  // namespace pk

#endif
