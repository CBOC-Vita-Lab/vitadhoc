/* See pipe.h. Copyright CBOC-Vita. MIT licence (see snes/frontend/LICENSE). */
#include "pipe.h"
#include "host.h"
#include "wide.h"
#include <string.h>

namespace snesp {

bool PicturePipe::start(pk::HandoffSys *sys, int depth_max, bool adaptive, uint32_t refresh_us, uint32_t frame_us)
{
	if (started_ || !sys) return false;
	pk::HandoffConfig c;
	c.frame_bytes = (uint32_t) MAX_W * MAX_H * 2 + SNAP_OFS;
	c.depth.max_depth = depth_max;
	c.adaptive = adaptive;
	c.refresh_us = refresh_us;
	c.frame_us = frame_us;
	started_ = box_.init(c, sys);
	serial_ = 0;
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

int PicturePipe::put_snap(snes_wide_snap *snap, unsigned serial, const uint16_t *normal, int w, int h, int pitch, int user, uint32_t seq, uint32_t emu_us)
{
	if (!started_ || !box_.enabled()) return -1;
	if (w > MAX_W) w = MAX_W;
	if (h > MAX_H) h = MAX_H;
	pk::HandoffFrame *f = box_.begin_write();
	if (!f) return -1;
	SnapDesc d;
	d.snap = snap; d.serial = serial; d.has_normal = (normal && w > 0 && h > 0) ? 1u : 0u; d.w = d.has_normal ? (uint32_t) w : 0u; d.h = d.has_normal ? (uint32_t) h : 0u;
	memcpy(f->data, &d, sizeof d);
	if (d.has_normal)
		for (int y = 0; y < h; y++)
			memcpy(f->data + SNAP_OFS + (size_t) y * w * 2, (const uint8_t *) normal + (size_t) y * pitch, (size_t) w * 2);
	f->w = d.w; f->h = d.h; f->pitch = d.w * 2;
	f->seq = seq; f->emu_us = emu_us; f->user = user | SNAP_FLAG;
	box_.commit(f);
	return 1;
}

int PicturePipe::stage(uint32_t seq, uint32_t emu_us)
{
	if (!started_ || !box_.enabled()) return -1;
	int w = 0, h = 0, pitch = 0;
	unsigned serial = 0;
	const uint16_t *src = snes_host_video_nowait(&w, &h, &pitch, &serial);
	if (!src || serial == serial_) return 0;
	const int r = put(src, w, h, pitch, (int) (serial & 0x7FFFFFFFu), seq, emu_us);
	if (r > 0) serial_ = serial;
	return r;
}

}  // namespace snesp
