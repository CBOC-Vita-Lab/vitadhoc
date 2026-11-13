/*
 * The SNES player's picture pipe: moves the core's newest finished picture to the thread that draws and
 * presents it (playerkit's pk_handoff mailbox), so the emulating thread never waits for the filter, the
 * compose or the display. Portable C++ (no console calls): the Vita program and the Linux twin use the same
 * code; the twin proves the pictures that come out are bit for bit the pictures the core made.
 *
 * Licence: links only snes/ and playerkit/ code (nothing from gpsp/).
 * Copyright CBOC-Vita. MIT licence (see snes/frontend/LICENSE).
 */
#ifndef SNES_PIPE_H
#define SNES_PIPE_H

#include <stdint.h>
#include "pk_handoff.h"

struct snes_wide_snap;

namespace snesp {

class PicturePipe {
public:
	enum { MAX_W = 512, MAX_H = 512 };       /* the largest picture the player draws (video_fetch clamps to it) */
	PicturePipe() : serial_(0), started_(false) {}
	/* depth_max: pictures in flight when there is headroom (the model's choice is 2) */
	bool start(pk::HandoffSys *sys, int depth_max, bool adaptive, uint32_t refresh_us, uint32_t frame_us);
	void stop();                              /* wakes both sides; the drawing thread then gets NULL from take() */
	bool running() const { return started_; }
	pk::Handoff &box() { return box_; }

	/* Producer (the emulating thread): copy a finished picture (RGB565, `pitch` bytes a row, w x h, at most 512 x 512)
	 * into the mailbox. `user` is carried to the drawing thread untouched. `seq` is the emulated frame number,
	 * `emu_us` the emulation work of that frame. Waits at the gate in full mode, never in half mode. Returns
	 *   1  the picture is in the mailbox;
	 *  -1  the mailbox is off (two-player lockstep, shut down, not started): draw the old way. */
	int put(const uint16_t *src, int w, int h, int pitch, int user, uint32_t seq, uint32_t emu_us);
	/* The same for the core's own newest picture (snes_host_video_nowait), once per new picture; user = its serial.
	 * Returns 1 handed over, 0 nothing new from the core, -1 the mailbox is off. */
	int stage(uint32_t seq, uint32_t emu_us);
	/* Wide view with the capture / compose split (wide.h): the mailbox frame carries the handle of a captured snapshot instead of a
	 * picture; the drawing thread composes it (snes_wide_compose). `normal` (or NULL) is the core's normal picture of the same frame, copied
	 * after the descriptor for frames the capture says will not be widened. user = the caller's flags (SNAP_FLAG is added). Returns as put(). */
	enum { SNAP_FLAG = 0x20000000, SNAP_OFS = 64 };
	struct SnapDesc { snes_wide_snap *snap; uint32_t serial; uint32_t has_normal; uint32_t w, h; };
	int put_snap(snes_wide_snap *snap, unsigned serial, const uint16_t *normal, int w, int h, int pitch, int user, uint32_t seq, uint32_t emu_us);
	/* forget which picture was handed over last (after a state load or a reset the core's serial may restart) */
	void forget() { serial_ = 0; }

private:
	pk::Handoff box_;
	unsigned serial_;
	bool started_;
};

}  // namespace snesp

#endif
