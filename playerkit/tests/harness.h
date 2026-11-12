/* Shared test helpers: temp folders, a synthetic game picture, a mock host. Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE). */
#ifndef PK_HARNESS_H
#define PK_HARNESS_H

#include "pk_menu.h"
#include "pk_util.h"
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#undef W_OK   /* unistd.h defines an access() flag with the name of playerkit's WriteResult W_OK */

namespace pkh {

inline void rm_rf(const std::string &p)
{
	DIR *d = opendir(p.c_str());
	if (d) {
		struct dirent *e;
		while ((e = readdir(d)) != 0) {
			std::string n = e->d_name;
			if (n == "." || n == "..") continue;
			rm_rf(p + "/" + n);
		}
		closedir(d);
		rmdir(p.c_str());
	} else {
		unlink(p.c_str());
	}
}

/* a fresh empty folder; removed by the destructor */
struct TempDir {
	std::string path;
	TempDir()
	{
		const char *base = getenv("PK_TMP");
		std::string b = base ? base : "build/tmp";
		pk::make_dirs(b);
		std::string t = b + "/t_XXXXXX";
		std::vector<char> buf(t.begin(), t.end());
		buf.push_back(0);
		if (!mkdtemp(&buf[0])) { path = b; return; }
		path = &buf[0];
	}
	~TempDir() { rm_rf(path); }
};

/* a SNES-like 256x224 picture; `variant` changes the scene so thumbnails differ */
inline void make_game_frame(int variant, std::vector<uint16_t> &px, int w = 256, int h = 224)
{
	px.assign((size_t) w * h, 0);
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			int r, g, b;
			if (y < h * 55 / 100) {             /* sky */
				float t = (float) y / (h * 0.55f);
				r = (int) (60 + 90 * t); g = (int) (120 + 90 * t); b = (int) (230 - 20 * t);
				/* hills */
				int hill = (int) (h * 0.4f + 14 * ((x * (3 + variant) / 17) % 9 - 4) * 0.5f);
				if (y > hill) { r = 40; g = 150 - variant * 6; b = 70; }
			} else {                             /* ground tiles */
				bool a = ((x / 16) + (y / 16)) & 1;
				r = a ? 150 : 120; g = a ? 90 : 70; b = a ? 40 : 30;
				if (y < h * 55 / 100 + 4) { r = 60; g = 190; b = 60; }
			}
			/* a "hero" sprite and a few coins whose place depends on the variant */
			int hx = 40 + variant * 17 % 150, hy = (int) (h * 0.55f) - 26;
			if (x >= hx && x < hx + 16 && y >= hy && y < hy + 26) { r = 220; g = 40; b = 40; if (y < hy + 8) { r = 250; g = 200; b = 160; } }
			for (int c = 0; c < 4; c++) {
				int cx = 100 + c * 30 + variant * 5, cy = 70 + (c % 2) * 12;
				int dx = x - cx, dy = y - cy;
				if (dx * dx + dy * dy < 25) { r = 250; g = 210; b = 40; }
			}
			if (x < 3 || y < 3 || x >= w - 3 || y >= h - 3) { r = g = b = 20; }
			px[(size_t) y * w + x] = (uint16_t) (((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
		}
}

/* A host that plays a fake machine: the "state" is a block of bytes that depends on a counter. */
class MockHost : public pk::MenuHost {
public:
	MockHost() : machine(1), variant(0), play_s(754), reset_count(0), capture_ok(true), restore_ok(true) {}
	struct Change { int id, value; };
	std::vector<Change> changes;
	std::vector<std::pair<int, bool> > cheat_sets;
	std::vector<pk::CheatItem> cheat_list;
	std::vector<int> touched;
	int machine, variant;
	int64_t play_s;
	int reset_count;
	bool capture_ok, restore_ok;
	std::vector<uint16_t> frame_px;
	int controls_changed_count = 0;

	void on_change(int id, int v) { Change c = { id, v }; changes.push_back(c); }
	void on_reset() { reset_count++; machine = 1; }
	bool capture_state(std::vector<uint8_t> &out)
	{
		if (!capture_ok) return false;
		out.resize(20000);
		for (size_t i = 0; i < out.size(); i++) out[i] = (uint8_t) (machine * 31 + (int) i * 7);
		out[0] = (uint8_t) machine; out[1] = (uint8_t) (machine >> 8);
		return true;
	}
	bool restore_state(const uint8_t *d, size_t n)
	{
		if (!restore_ok || n < 4) return false;
		machine = d[0] | (d[1] << 8);
		return true;
	}
	bool current_frame(pk::Frame *f)
	{
		make_game_frame(variant, frame_px);
		*f = pk::Frame(&frame_px[0], 256, 224, 512);
		return true;
	}
	int64_t play_seconds() { return play_s; }
	void list_cheats(std::vector<pk::CheatItem> &out) { out = cheat_list; }
	void set_cheat(int i, bool on) { cheat_sets.push_back(std::make_pair(i, on)); if (i >= 0 && i < (int) cheat_list.size()) cheat_list[(size_t) i].on = on; }
	void on_state_touched(int slot) { touched.push_back(slot); }
	void on_controls_changed(pk::WriteResult) { controls_changed_count++; }
};

inline pk::MenuInput idle(int dt = 16)
{
	pk::MenuInput in;
	in.dt_ms = dt;
	in.pads[0].connected = true;
	return in;
}

/* one frame with a button pressed (then one released), returns the event of the press frame */
inline pk::MenuEvent press(pk::Menu &m, uint32_t phys, int hold_frames = 1)
{
	pk::MenuEvent ev = pk::MENU_NONE;
	for (int i = 0; i < hold_frames; i++) {
		pk::MenuInput in = idle();
		in.pads[0].buttons = phys;
		pk::MenuEvent e = m.update(in);
		if (e != pk::MENU_NONE) ev = e;
	}
	pk::MenuInput in = idle();
	pk::MenuEvent e = m.update(in);
	if (e != pk::MENU_NONE) ev = e;
	return ev;
}

inline pk::MenuEvent tap_at(pk::Menu &m, float x, float y)
{
	pk::MenuEvent ev = pk::MENU_NONE;
	pk::MenuInput in = idle();
	in.touch.down = true; in.touch.x = x; in.touch.y = y;
	pk::MenuEvent e = m.update(in);
	if (e != pk::MENU_NONE) ev = e;
	in.touch.down = false;
	e = m.update(in);
	if (e != pk::MENU_NONE) ev = e;
	return ev;
}

}  // namespace pkh

#endif
