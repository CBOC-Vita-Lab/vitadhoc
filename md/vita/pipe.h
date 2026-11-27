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
#include <atomic>
#include "pk_handoff.h"

namespace mdp {

class PicturePipe {
public:
	enum { MAX_W = 512, MAX_H = 576 };       /* the largest picture the core draws (md_host's MD_MAX_WIDTH x MD_MAX_HEIGHT) and the wide view's canvas pitch (MD_WIDE_PITCH: strips beside the picture) */
	PicturePipe() : direct_(0), started_(false), nslots_(0) { for (int i = 0; i < 8; i++) slots_[i] = 0; }
	/* depth_max: pictures in flight when there is headroom (the model's choice is 2) */
	bool start(pk::HandoffSys *sys, int depth_max, bool adaptive, uint32_t refresh_us, uint32_t frame_us);
	void stop();                              /* wakes both sides; the drawing thread then gets NULL from take() */
	void set_frame_us(uint32_t frame_us);     /* update frame period in place (region change) without tearing down the mailbox */
	bool running() const { return started_; }
	pk::Handoff &box() { return box_; }

	/* Producer (the emulating thread): copy a finished picture (RGB565, `pitch` bytes a row, w x h) into the mailbox. `user` is carried to the
	 * drawing thread untouched (the player puts the picture's geometry serial in it). `seq` is the emulated frame number, `emu_us` the
	 * emulation work of that frame. Waits at the gate in full mode, never in half mode. Returns
	 *   1  the picture is in the mailbox;
	 *   0  nothing to hand over;
	 *  -1  the mailbox is off (two-player lockstep, shut down, not started): draw the old way. */
	int put(const uint16_t *src, int w, int h, int pitch, int user, uint32_t seq, uint32_t emu_us);
	pk::HandoffFrame *put_begin();
	int put_commit(pk::HandoffFrame *f, const uint16_t *src, int w, int h, int pitch, int user, uint32_t seq, uint32_t emu_us);

	/* Producer, no copy: the core draws straight into a mailbox buffer. direct_begin() returns the buffer (and its pitch in pixels, MAX_W) to give
	 * md_host_set_framebuffer before the frame runs, or NULL when the mailbox is off, its gate is closed, or it is in drop mode (half rate,
	 * fast forward): then run the frame as usual and put() it. After the frame: direct_commit() hands over what md_host_video() returned (a
	 * pointer inside the buffer), direct_abandon() gives the buffer back unused (a frame that drew nothing). */
	uint16_t *direct_begin(int *pitch_px);
	pk::HandoffFrame *direct_frame() const { return direct_; }
	int direct_commit(const uint16_t *pix, int w, int h, int pitch, int user, uint32_t seq, uint32_t emu_us);
	void direct_abandon();
	bool direct_open() const { return direct_ != 0; }
	int slot_of(const pk::HandoffFrame *f) const;

private:
	int register_slot(const pk::HandoffFrame *f);
	pk::HandoffFrame *direct_;
	pk::Handoff box_;
	bool started_;
	const pk::HandoffFrame *slots_[8];
	std::atomic<int> nslots_;
};

}  // namespace mdp

#endif
