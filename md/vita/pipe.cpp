/* See pipe.h. Copyright CBOC-Vita. MIT licence (see md/frontend/LICENSE). */
#include "pipe.h"
#include <string.h>

namespace mdp {

bool PicturePipe::start(pk::HandoffSys *sys, int depth_max, bool adaptive, uint32_t refresh_us, uint32_t frame_us)
{
	if (started_ || !sys) return false;
	pk::HandoffConfig c;
	c.frame_bytes = (uint32_t) MAX_W * MAX_H * 2;
	c.depth.max_depth = depth_max;
	c.adaptive = adaptive;
	c.refresh_us = refresh_us;
	c.frame_us = frame_us;
	started_ = box_.init(c, sys);
	return started_;
}

void PicturePipe::stop()
{
	if (started_) box_.shutdown();
}

int PicturePipe::put(const uint16_t *src, int w, int h, int pitch, int user, uint32_t seq, uint32_t emu_us)
{
	if (!started_ || !box_.enabled()) return -1;
	if (w > MAX_W) w = MAX_W;
	if (h > MAX_H) h = MAX_H;
	if (!src || w <= 0 || h <= 0) return 0;
	pk::HandoffFrame *f = box_.begin_write();       /* waits at the gate in full mode; never in half mode */
	if (!f) return -1;
	/* the copy is by buffer ownership: the drawing thread never sees this buffer until commit() */
	for (int y = 0; y < h; y++)
		memcpy(f->data + (size_t) y * w * 2, (const uint8_t *) src + (size_t) y * pitch, (size_t) w * 2);
	f->w = w; f->h = h; f->pitch = w * 2;
	f->seq = seq; f->emu_us = emu_us; f->user = user;
	box_.commit(f);
	return 1;
}

uint16_t *PicturePipe::direct_begin(int *pitch_px)
{
	if (!started_ || !box_.enabled() || direct_) return 0;
	direct_ = box_.try_begin_write_direct();
	if (!direct_) return 0;
	if (pitch_px) *pitch_px = MAX_W;
	return (uint16_t *) direct_->data;
}

int PicturePipe::direct_commit(const uint16_t *pix, int w, int h, int pitch, int user, uint32_t seq, uint32_t emu_us)
{
	pk::HandoffFrame *f = direct_;
	if (!f) return -1;
	direct_ = 0;
	const uint8_t *p = (const uint8_t *) pix;
	if (!pix || w <= 0 || h <= 0 || p < f->data || (size_t) (p - f->data) + (size_t) (h - 1) * (size_t) pitch + (size_t) w * 2 > f->cap) {
		box_.abandon(f);
		return 0;
	}
	f->offset = (uint32_t) (p - f->data);
	f->w = w; f->h = h; f->pitch = pitch;
	f->seq = seq; f->emu_us = emu_us; f->user = user;
	box_.commit(f);
	return 1;
}

void PicturePipe::direct_abandon()
{
	if (!direct_) return;
	box_.abandon(direct_);
	direct_ = 0;
}

}  // namespace mdp
