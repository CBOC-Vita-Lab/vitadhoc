/*
 * The Sega player's picture pipe: moves the core's newest finished picture to the thread that draws and presents it (playerkit's
 * pk_handoff mailbox), so the emulating thread never waits for the scaler, the filter or the display. Portable C++ (no console
 * calls): the Vita program and the Linux twin use the same code; the twin proves the pictures that come out are bit for bit the
 * pictures the core made.
 *
 * Licence: links only md/ and playerkit/ code (nothing from gpsp/).
 * Copyright CBOC-Vita. MIT licence (see md/frontend/LICENSE).
 */
#ifndef MD_PIPE_H
#define MD_PIPE_H

#include <stdint.h>
#include "pk_handoff.h"

namespace mdp {

class PicturePipe {
public:
	enum { MAX_W = 352, MAX_H = 512 };       /* the largest picture the player draws (put() clamps to it) */
	PicturePipe() : started_(false) {}
	/* depth_max: pictures in flight when there is headroom (the model's choice is 2) */
	bool start(pk::HandoffSys *sys, int depth_max, bool adaptive, uint32_t refresh_us, uint32_t frame_us);
	void stop();                              /* wakes both sides; the drawing thread then gets NULL from take() */
	bool running() const { return started_; }
	pk::Handoff &box() { return box_; }

	/* Producer (the emulating thread): copy a finished picture (RGB565, `pitch` bytes a row, w x h) into the mailbox. `user` is carried to the
	 * drawing thread untouched (the player puts the picture's geometry serial in it). `seq` is the emulated frame number, `emu_us` the
	 * emulation work of that frame. Waits at the gate in full mode, never in half mode. Returns
	 *   1  the picture is in the mailbox;
	 *   0  nothing to hand over;
	 *  -1  the mailbox is off (two-player lockstep, shut down, not started): draw the old way. */
	int put(const uint16_t *src, int w, int h, int pitch, int user, uint32_t seq, uint32_t emu_us);

private:
	pk::Handoff box_;
	bool started_;
};

}  // namespace mdp

#endif
