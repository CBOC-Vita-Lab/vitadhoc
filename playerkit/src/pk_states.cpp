/* See pk_states.h. Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE). */
#include "pk_states.h"
#include <string.h>
#include <time.h>
#include <algorithm>

namespace pk {

/* ---- paths -------------------------------------------------------------- */

std::string StateLayout::states_dir() const { return path_join(path_join(root, "states"), console); }

std::string StateLayout::slot_path(int slot) const
{
	return path_join(states_dir(), name + ext_tag + strf(".st%d", slot - 1));
}

std::string StateLayout::resume_path() const { return path_join(states_dir(), name + ext_tag + ".rsm"); }
std::string StateLayout::undo_path() const { return path_join(states_dir(), name + ext_tag + ".undo"); }
std::string StateLayout::scene_path() const { return path_join(path_join(root, "scenes"), name + ".shot"); }

const char *state_status_text(StateStatus s)
{
	switch (s) {
	case ST_OK: return "";
	case ST_EMPTY: return "No saved state in this slot";
	case ST_READ: return "The state could not be read from the memory card";
	case ST_DAMAGED: return "The state file is damaged";
	case ST_TOO_BIG: return "The state file is too large";
	case ST_WRITE: return "The state could not be written";
	default: return "No such slot";
	}
}

std::string SaveReport::text() const
{
	if (status == ST_OK) return std::string();
	if (status == ST_WRITE) return strf("Could not save the state: %s", write_result_text(write_rc));
	return state_status_text(status);
}

std::string stamp_text(int64_t t)
{
	static const char *mon[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
	if (t <= 0) return std::string();
	time_t tt = (time_t) t;
	struct tm tmv;
#ifdef _WIN32
	localtime_s(&tmv, &tt);
#else
	if (!localtime_r(&tt, &tmv)) return std::string();
#endif
	if (tmv.tm_mon < 0 || tmv.tm_mon > 11) return std::string();
	return strf("%d %s %d, %02d:%02d", tmv.tm_mday, mon[tmv.tm_mon], tmv.tm_year + 1900, tmv.tm_hour, tmv.tm_min);
}

/* ---- pictures ----------------------------------------------------------- */

static inline void unpack565(uint16_t v, int *r, int *g, int *b)
{
	int r5 = v >> 11, g6 = (v >> 5) & 63, b5 = v & 31;
	*r = (r5 << 3) | (r5 >> 2);
	*g = (g6 << 2) | (g6 >> 4);
	*b = (b5 << 3) | (b5 >> 2);
}

/* BGR565: the .thumb/.shot order. Blue in the high bits, red in the low bits. */
static inline uint16_t pack_bgr565(int r, int g, int b)
{
	return (uint16_t) (((b >> 3) << 11) | ((g >> 2) << 5) | (r >> 3));
}

/* Box filter of a frame into dw x dh pixels; every destination pixel is the average of the
 * source pixels it covers (at least one). Output BGR565. */
static void box_scale(const Frame &f, int dw, int dh, uint16_t *out, int out_stride)
{
	for (int dy = 0; dy < dh; dy++) {
		int sy0 = (int) ((int64_t) dy * f.h / dh);
		int sy1 = std::max(sy0 + 1, (int) ((int64_t) (dy + 1) * f.h / dh));
		for (int dx = 0; dx < dw; dx++) {
			int sx0 = (int) ((int64_t) dx * f.w / dw);
			int sx1 = std::max(sx0 + 1, (int) ((int64_t) (dx + 1) * f.w / dw));
			uint32_t sr = 0, sg = 0, sb = 0, n = 0;
			for (int y = sy0; y < sy1 && y < f.h; y++) {
				const uint16_t *row = (const uint16_t *) ((const uint8_t *) f.px + (size_t) y * f.pitch_bytes);
				for (int x = sx0; x < sx1 && x < f.w; x++) {
					int r, g, b;
					unpack565(row[x], &r, &g, &b);
					sr += (uint32_t) r; sg += (uint32_t) g; sb += (uint32_t) b; n++;
				}
			}
			if (!n) n = 1;
			out[(size_t) dy * out_stride + dx] = pack_bgr565((int) ((sr + n / 2) / n), (int) ((sg + n / 2) / n), (int) ((sb + n / 2) / n));
		}
	}
}

void make_thumb(const Frame &f, uint16_t out[THUMB_W * THUMB_H])
{
	memset(out, 0, sizeof(uint16_t) * THUMB_W * THUMB_H);
	if (!f.valid()) return;
	int dw, dh;
	if ((int64_t) f.w * THUMB_H <= (int64_t) THUMB_W * f.h) {
		dh = THUMB_H;
		dw = std::max(1, (int) (((int64_t) f.w * THUMB_H + f.h / 2) / f.h));
	} else {
		dw = THUMB_W;
		dh = std::max(1, (int) (((int64_t) f.h * THUMB_W + f.w / 2) / f.w));
	}
	if (dw > THUMB_W) dw = THUMB_W;
	if (dh > THUMB_H) dh = THUMB_H;
	int x0 = (THUMB_W - dw) / 2, y0 = (THUMB_H - dh) / 2;
	box_scale(f, dw, dh, out + (size_t) y0 * THUMB_W + x0, THUMB_W);
}

void make_shot(const Frame &f, std::vector<uint16_t> &rows, int *out_w, int *out_h)
{
	int dw = 0, dh = 0;
	if (f.valid()) {
		if (f.w <= SHOT_MAX_W && f.h <= SHOT_MAX_H) {
			dw = f.w; dh = f.h;
		} else if ((int64_t) f.w * SHOT_MAX_H > (int64_t) f.h * SHOT_MAX_W) {
			dw = SHOT_MAX_W;
			dh = std::max(1, (int) (((int64_t) f.h * SHOT_MAX_W + f.w / 2) / f.w));
		} else {
			dh = SHOT_MAX_H;
			dw = std::max(1, (int) (((int64_t) f.w * SHOT_MAX_H + f.h / 2) / f.h));
		}
		dw = std::min(dw, (int) SHOT_MAX_W);
		dh = std::min(dh, (int) SHOT_MAX_H);
	}
	rows.assign((size_t) SHOT_STRIDE * std::max(dh, 1), 0);
	if (dw > 0) box_scale(f, dw, dh, &rows[0], SHOT_STRIDE);
	if (out_w) *out_w = dw;
	if (out_h) *out_h = dh;
}

static void put_header(std::vector<uint8_t> &o, int w, int h, int stride)
{
	static const uint8_t magic[4] = { 'S', 'T', 'H', '1' };
	o.insert(o.end(), magic, magic + 4);
	o.push_back((uint8_t) w); o.push_back((uint8_t) (w >> 8));
	o.push_back((uint8_t) h); o.push_back((uint8_t) (h >> 8));
	o.push_back((uint8_t) stride); o.push_back((uint8_t) (stride >> 8));
	for (int i = 0; i < 6; i++) o.push_back(0);
}

static void put_px(std::vector<uint8_t> &o, const uint16_t *p, size_t n)
{
	for (size_t i = 0; i < n; i++) { o.push_back((uint8_t) p[i]); o.push_back((uint8_t) (p[i] >> 8)); }
}

std::vector<uint8_t> thumb_file(const Frame &f)
{
	uint16_t px[THUMB_W * THUMB_H];
	make_thumb(f, px);
	std::vector<uint8_t> o;
	put_header(o, THUMB_W, THUMB_H, THUMB_W);
	put_px(o, px, THUMB_W * THUMB_H);
	return o;
}

std::vector<uint8_t> shot_file(const Frame &f)
{
	std::vector<uint16_t> rows;
	int w, h;
	make_shot(f, rows, &w, &h);
	std::vector<uint8_t> o;
	if (w <= 0 || h <= 0) return o;
	put_header(o, w, h, SHOT_STRIDE);
	put_px(o, &rows[0], (size_t) SHOT_STRIDE * h);
	return o;
}

static inline uint16_t bgr_to_rgb565(uint16_t v)
{
	return (uint16_t) (((v & 31) << 11) | (v & 0x07E0) | (v >> 11));
}

static int rd16(const uint8_t *p) { return p[0] | (p[1] << 8); }

bool parse_thumb(const std::vector<uint8_t> &f, std::vector<uint16_t> &out, int *w, int *h)
{
	size_t body = (size_t) THUMB_W * THUMB_H * 2;
	if (f.size() < THUMB_HEADER + body || memcmp(&f[0], "STH1", 4) != 0 || rd16(&f[4]) != THUMB_W ||
	    rd16(&f[6]) != THUMB_H || rd16(&f[8]) != THUMB_W)
		return false;
	out.resize((size_t) THUMB_W * THUMB_H);
	for (size_t i = 0; i < out.size(); i++) out[i] = bgr_to_rgb565((uint16_t) rd16(&f[THUMB_HEADER + i * 2]));
	if (w) *w = THUMB_W;
	if (h) *h = THUMB_H;
	return true;
}

bool parse_shot(const std::vector<uint8_t> &f, std::vector<uint16_t> &out, int *w, int *h)
{
	if (f.size() < THUMB_HEADER || memcmp(&f[0], "STH1", 4) != 0) return false;
	int sw = rd16(&f[4]), sh = rd16(&f[6]), ss = rd16(&f[8]);
	if (sw <= 0 || sw > SHOT_MAX_W || sh <= 0 || sh > SHOT_MAX_H || ss != SHOT_STRIDE) return false;
	if (f.size() < THUMB_HEADER + (size_t) sh * SHOT_STRIDE * 2) return false;
	out.resize((size_t) sw * sh);
	for (int y = 0; y < sh; y++)
		for (int x = 0; x < sw; x++)
			out[(size_t) y * sw + x] = bgr_to_rgb565((uint16_t) rd16(&f[THUMB_HEADER + ((size_t) y * SHOT_STRIDE + x) * 2]));
	if (w) *w = sw;
	if (h) *h = sh;
	return true;
}

/* ---- the slots ----------------------------------------------------------- */

bool States::ensure_dirs() const
{
	bool a = make_dirs(l_.states_dir());
	bool b = make_dirs(path_join(l_.root, "scenes"));
	return a && b;
}

static std::string meta_text(const StateLayout &l, const uint8_t *data, size_t n, const Frame &f, int64_t play_s)
{
	Ini m;
	m.set("v", "1");
	m.set("console", l.console);
	m.set("name", l.name);
	m.set_int("size", (int64_t) n);
	m.set("crc32", strf("%08x", (unsigned) crc32(0, data, n)));
	m.set_int("saved", now_unix());
	m.set_int("play_s", play_s);
	m.set_int("frame_w", f.valid() ? f.w : 0);
	m.set_int("frame_h", f.valid() ? f.h : 0);
	return m.serialize();
}

SaveReport States::write_set(const std::string &path, const uint8_t *data, size_t n, const Frame &frame,
                             int64_t play_seconds, bool pictures)
{
	SaveReport rep;
	ensure_dirs();
	WriteResult rc = atomic_write(path, data, n);
	if (rc != W_OK) {
		rep.status = ST_WRITE;
		rep.write_rc = rc;
		return rep;               /* the previous file and its pictures are untouched */
	}
	/* the companions: a failed one is removed so a stale picture never sits beside a new state */
	if (pictures) {
		std::vector<uint8_t> t = thumb_file(frame);
		rep.thumb_ok = frame.valid() && atomic_write(path + ".thumb", t.data(), t.size()) == W_OK;
		if (!rep.thumb_ok) file_remove(path + ".thumb");
		std::vector<uint8_t> s = shot_file(frame);
		rep.shot_ok = !s.empty() && atomic_write(path + ".shot", s.data(), s.size()) == W_OK;
		if (!rep.shot_ok) file_remove(path + ".shot");
	}
	std::string m = meta_text(l_, data, n, frame, play_seconds);
	rep.meta_ok = atomic_write(path + ".meta", (const uint8_t *) m.data(), m.size()) == W_OK;
	if (!rep.meta_ok) file_remove(path + ".meta");
	return rep;
}

SaveReport States::save(int slot, const uint8_t *data, size_t n, const Frame &frame, int64_t play_seconds)
{
	if (slot < 1 || slot > STATE_SLOTS) {
		SaveReport r;
		r.status = ST_BAD_SLOT;
		return r;
	}
	return write_set(l_.slot_path(slot), data, n, frame, play_seconds, true);
}

StateStatus States::read_checked(const std::string &path, std::vector<uint8_t> &out) const
{
	recover_old(path);
	if (!file_exists(path)) return ST_EMPTY;
	if (!file_read(path, out, 32u << 20)) {
		uint64_t sz = 0;
		file_exists(path, &sz);
		return sz > (32u << 20) ? ST_TOO_BIG : ST_READ;
	}
	Ini m;
	if (m.load(path + ".meta") && m.has("crc32") && m.has("size")) {
		unsigned want = (unsigned) strtoul(m.get("crc32").c_str(), 0, 16);
		if ((uint64_t) m.get_i64("size", -1) != out.size() || want != crc32(0, out.data(), out.size())) {
			out.clear();
			return ST_DAMAGED;
		}
	}
	return ST_OK;
}

StateStatus States::load(int slot, std::vector<uint8_t> &out) const
{
	if (slot < 1 || slot > STATE_SLOTS) return ST_BAD_SLOT;
	return read_checked(l_.slot_path(slot), out);
}

SlotInfo States::info(int slot) const
{
	SlotInfo si;
	if (slot < 1 || slot > STATE_SLOTS) return si;
	std::string p = l_.slot_path(slot);
	uint64_t sz = 0;
	int64_t mt = 0;
	if (!file_exists(p, &sz, &mt) && !file_exists(p + ".old", &sz, &mt)) return si;
	si.present = true;
	si.size = sz;
	si.saved = mt;
	Ini m;
	if (m.load(p + ".meta")) {
		int64_t s = m.get_i64("saved", 0);
		if (s > 0) si.saved = s;
		si.play_seconds = m.get_i64("play_s", -1);
	}
	si.has_thumb = file_exists(p + ".thumb");
	return si;
}

void States::scan(SlotInfo out[STATE_SLOTS]) const
{
	for (int i = 0; i < STATE_SLOTS; i++) out[i] = info(i + 1);
}

int States::used_count() const
{
	int n = 0;
	for (int i = 1; i <= STATE_SLOTS; i++) n += info(i).present ? 1 : 0;
	return n;
}

bool States::remove(int slot)
{
	if (slot < 1 || slot > STATE_SLOTS) return false;
	std::string p = l_.slot_path(slot);
	static const char *const tails[] = { "", ".old", ".tmp", ".thumb", ".shot", ".meta", ".thumb.tmp", ".shot.tmp", ".meta.tmp" };
	for (size_t i = 0; i < sizeof tails / sizeof tails[0]; i++) file_remove(p + tails[i]);
	return !file_exists(p) && !file_exists(p + ".old");
}

bool States::read_thumb(int slot, std::vector<uint16_t> &px, int *w, int *h) const
{
	std::vector<uint8_t> f;
	return slot >= 1 && slot <= STATE_SLOTS && file_read(l_.slot_path(slot) + ".thumb", f, 1u << 16) && parse_thumb(f, px, w, h);
}

bool States::read_shot(int slot, std::vector<uint16_t> &px, int *w, int *h) const
{
	std::vector<uint8_t> f;
	return slot >= 1 && slot <= STATE_SLOTS && file_read(l_.slot_path(slot) + ".shot", f, 1u << 18) && parse_shot(f, px, w, h);
}

/* ---- Continue ------------------------------------------------------------ */

SaveReport States::save_resume(const uint8_t *data, size_t n, const Frame &frame, int64_t play_seconds,
                               const std::string &patch_key)
{
	std::string p = l_.resume_path();
	SaveReport r = write_set(p, data, n, frame, play_seconds, false);
	if (r.status != ST_OK) return r;
	std::string key = patch_key.empty() ? "-\n" : patch_key;
	if (atomic_write(p + ".pt", (const uint8_t *) key.data(), key.size()) != W_OK) {
		/* without its note the point could be loaded into a different patched game: keep none */
		file_remove(p + ".pt");
	}
	if (frame.valid()) save_scene(frame);
	return r;
}

bool States::resume_present() const
{
	std::string p = l_.resume_path();
	return file_exists(p) || file_exists(p + ".old");
}

StateStatus States::load_resume(std::vector<uint8_t> &out) const
{
	return read_checked(l_.resume_path(), out);
}

States::PatchMatch States::resume_patch(const std::string &key) const
{
	std::vector<uint8_t> b;
	if (!file_read(l_.resume_path() + ".pt", b, 4096)) return PATCH_UNKNOWN;
	std::string had((const char *) b.data(), b.size());
	std::string want = key.empty() ? "-\n" : key;
	return had == want ? PATCH_SAME : PATCH_DIFFERENT;
}

bool States::remove_resume()
{
	std::string p = l_.resume_path();
	static const char *const tails[] = { "", ".old", ".tmp", ".pt", ".meta" };
	for (size_t i = 0; i < sizeof tails / sizeof tails[0]; i++) file_remove(p + tails[i]);
	return !resume_present();
}

bool States::resume_due(uint64_t now_ms, uint64_t interval_ms) const
{
	return !have_resume_mark_ || now_ms - last_resume_ms_ >= interval_ms;
}

/* ---- undo ---------------------------------------------------------------- */

bool States::undo_store(const uint8_t *data, size_t n)
{
	ensure_dirs();
	return atomic_write(l_.undo_path(), data, n) == W_OK;
}

StateStatus States::undo_fetch(std::vector<uint8_t> &out) const
{
	if (!file_exists(l_.undo_path())) return ST_EMPTY;
	return file_read(l_.undo_path(), out, 32u << 20) ? ST_OK : ST_READ;
}

bool States::undo_available() const { return file_exists(l_.undo_path()); }

void States::undo_clear()
{
	file_remove(l_.undo_path());
	file_remove(l_.undo_path() + ".old");
	file_remove(l_.undo_path() + ".tmp");
}

bool States::save_scene(const Frame &frame)
{
	std::vector<uint8_t> s = shot_file(frame);
	if (s.empty()) return false;
	make_dirs(path_join(l_.root, "scenes"));
	return atomic_write(l_.scene_path(), s.data(), s.size()) == W_OK;
}

}  // namespace pk
