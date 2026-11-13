/* See pk_states.h. Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE). */
#include "pk_states.h"
#include <string.h>
#include <time.h>
#include <algorithm>

namespace pk {

/* ---- paths -------------------------------------------------------------- */

std::string StateLayout::flat_dir() const { return path_join(path_join(root, "states"), console); }
std::string StateLayout::states_dir() const
{
	uint32_t c = crc32(0, name.data(), name.size());
	return path_join(flat_dir(), strf("%02x", (unsigned) (c >> 24)));
}

int migrate_to_bucket(const StateLayout &l, int *failed)
{
	static const char *const tail[] = { "", ".thumb", ".shot", ".meta", ".p2", ".old" };
	static const char *const rtail[] = { "", ".pt", ".meta", ".thumb", ".shot", ".old" };
	const std::string base = l.name + l.ext_tag;
	const std::string dir = l.states_dir(), flat = l.flat_dir();
	const std::string mark = path_join(dir, base + ".st0.mig");
	int moved = 0, bad = 0;
	if (failed) *failed = 0;
	if (file_exists(mark)) return 0;
	std::vector<std::string> names;
	std::vector<std::string> want;
	for (int s = 0; s < 10; s++)
		for (size_t t = 0; t < sizeof(tail) / sizeof(tail[0]); t++)
			want.push_back(base + strf(".st%d", s) + tail[t]);
	for (size_t t = 0; t < sizeof(rtail) / sizeof(rtail[0]); t++)
		want.push_back(base + ".rsm" + rtail[t]);
	want.push_back(base + ".undo");
	bool listed = list_dir(flat, names);
	bool made = false;
	for (size_t i = 0; i < want.size(); i++) {
		const std::string from = path_join(flat, want[i]), to = path_join(dir, want[i]);
		if (listed ? std::find(names.begin(), names.end(), want[i]) == names.end() : !file_exists(from))
			continue;
		if (!made) { make_dirs(dir); made = true; }
		if (file_exists(to)) continue;               /* the bucket's own wins; the old one is kept */
		if (fs().rename_file(from, to)) moved++;
		else bad++;
	}
	if (!bad) {
		if (!made) make_dirs(dir);
		fs().write_all(mark, (const uint8_t *) "", 0);
	}
	if (failed) *failed = bad;
	return moved;
}

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

States::States(const StateLayout &l) : l_(l) {}

bool States::ensure_dirs()
{
	if (dirs_ok_) return true;
	bool a = make_dirs(l_.states_dir());
	bool b = make_dirs(path_join(l_.root, "scenes"));
	dirs_ok_ = scene_dir_ok_ = a && b;
	return a && b;
}

static std::string meta_text(const StateLayout &l, const StateJob &j)
{
	Ini m;
	m.set("v", "1");
	m.set("console", l.console);
	m.set("name", l.name);
	m.set_int("size", (int64_t) j.data.size());
	m.set("crc32", strf("%08x", (unsigned) crc32(0, j.data.data(), j.data.size())));
	m.set_int("saved", j.saved);
	m.set_int("play_s", j.play_seconds);
	m.set_int("frame_w", j.frame_w);
	m.set_int("frame_h", j.frame_h);
	m.set("end", "1");      /* written last: a .meta cut short by a power loss has no end and is ignored */
	return m.serialize();
}

static bool meta_complete(const Ini &m) { return m.has("end") && m.has("crc32") && m.has("size"); }

static bool has_name(const std::vector<std::string> &names, const std::string &n)
{
	return std::find(names.begin(), names.end(), n) != names.end();
}

/* Fills one cache entry. `names` is the folder listing (file names only), or NULL: then the card is asked
 * one name at a time. */
void States::scan_one(Cached &c, const std::string &path, const std::vector<std::string> *names, bool resume)
{
	c = Cached();
	std::string base = path.substr(path_dir(path).size() + (path_dir(path).empty() ? 0 : 1));
	uint64_t sz = 0;
	int64_t mt = 0;
	if (names) {
		c.has_main = has_name(*names, base);
		c.has_old = has_name(*names, base + ".old");
		c.has_meta = has_name(*names, base + ".meta");
		c.has_thumb = !resume && has_name(*names, base + ".thumb");
		c.has_shot = !resume && has_name(*names, base + ".shot");
		static const char *const leftovers[] = { ".tmp", ".thumb.tmp", ".shot.tmp", ".meta.tmp", ".thumb.old", ".shot.old", ".meta.old" };
		for (size_t i = 0; i < sizeof leftovers / sizeof leftovers[0]; i++)
			if (has_name(*names, base + leftovers[i])) c.extra.push_back(base + leftovers[i]);
	} else {
		c.has_main = file_exists(path);
		c.has_old = !c.has_main && file_exists(path + ".old");
		c.has_meta = file_exists(path + ".meta");
		c.has_thumb = !resume && file_exists(path + ".thumb");
		c.has_shot = !resume && file_exists(path + ".shot");
	}
	if (!c.has_main && c.has_old) {
		/* a write cut between its two renames: the .old is the last good copy, put it back */
		if (fs().rename_file(path + ".old", path)) { c.has_main = true; c.has_old = false; }
	}
	if (!c.has_main && !c.has_old) return;
	c.info.present = true;
	if (file_exists(c.has_main ? path : path + ".old", &sz, &mt)) { c.info.size = sz; c.info.saved = mt; }
	Ini m;
	if (c.has_meta && m.load(path + ".meta") && meta_complete(m)) {
		int64_t s = m.get_i64("saved", 0);
		if (s > 0) c.info.saved = s;
		c.info.play_seconds = m.get_i64("play_s", -1);
	}
	if (c.has_thumb) {
		std::vector<uint8_t> f;
		int w, h;
		if (file_read(path + ".thumb", f, 1u << 16) && parse_thumb(f, c.thumb_px, &w, &h)) c.info.has_thumb = true;
		else c.thumb_px.clear();
	}
}

void States::rescan()
{
	migrate_to_bucket(l_);          /* (one stat once this game is in its bucket) */
	std::vector<std::string> names;
	bool listed = list_dir(l_.states_dir(), names);
	if (listed) {
		/* only this game's names matter */
		std::string pre = l_.name + l_.ext_tag + ".";
		std::vector<std::string> mine;
		for (size_t i = 0; i < names.size(); i++)
			if (names[i].compare(0, pre.size(), pre) == 0) mine.push_back(names[i]);
		names.swap(mine);
	}
	Cached tmp[STATE_SLOTS], r;
	for (int i = 0; i < STATE_SLOTS; i++) scan_one(tmp[i], l_.slot_path(i + 1), listed ? &names : 0, false);
	scan_one(r, l_.resume_path(), listed ? &names : 0, true);
	bool pt = listed ? has_name(names, path_stem(l_.resume_path()) + ".rsm.pt") : file_exists(l_.resume_path() + ".pt");
	SpinGuard g(lk_);
	for (int i = 0; i < STATE_SLOTS; i++) slot_[i] = tmp[i];
	rsm_ = r;
	rsm_pt_ = pt;
	rsm_pt_known_ = false;
	scanned_ = true;
}

void States::ensure_scanned()
{
	bool s;
	{ SpinGuard g(lk_); s = scanned_; }
	if (!s) rescan();
}

SlotInfo States::info(int slot)
{
	if (slot < 1 || slot > STATE_SLOTS) return SlotInfo();
	ensure_scanned();
	SpinGuard g(lk_);
	return slot_[slot - 1].info;
}

void States::scan(SlotInfo out[STATE_SLOTS])
{
	ensure_scanned();
	SpinGuard g(lk_);
	for (int i = 0; i < STATE_SLOTS; i++) out[i] = slot_[i].info;
}

int States::used_count()
{
	ensure_scanned();
	SpinGuard g(lk_);
	int n = 0;
	for (int i = 0; i < STATE_SLOTS; i++) n += slot_[i].info.present ? 1 : 0;
	return n;
}

StateJob States::prepare(int slot, const uint8_t *data, size_t n, const Frame &frame, int64_t play_seconds)
{
	StateJob j;
	j.slot = slot;
	if (data && n) j.data.assign(data, data + n);
	j.play_seconds = play_seconds;
	j.saved = now_unix();
	j.frame_w = frame.valid() ? frame.w : 0;
	j.frame_h = frame.valid() ? frame.h : 0;
	if (frame.valid()) {
		j.thumb = thumb_file(frame);
		j.shot = shot_file(frame);
	}
	return j;
}

StateJob States::prepare_resume(const uint8_t *data, size_t n, const Frame &frame, int64_t play_seconds, const std::string &patch_key)
{
	StateJob j = prepare(0, data, n, Frame(), play_seconds);
	j.frame_w = frame.valid() ? frame.w : 0;
	j.frame_h = frame.valid() ? frame.h : 0;
	j.patch_key = patch_key;
	if (frame.valid()) { j.shot = shot_file(frame); j.scene = true; }
	return j;
}

/* A companion file (picture, note): written in place, one card operation, no temp/rename. It is never
 * the only copy of anything: a picture cut short is refused by its size check, a note by its missing end. */
WriteResult States::put_file(const std::string &path, const std::vector<uint8_t> &b, bool safe, Exists hint)
{
	if (safe) return atomic_write(path, b.data(), b.size(), hint);
	return fs().write_all(path, b.data(), b.size()) ? W_OK : W_NO_WRITE;
}

SaveReport States::commit(const StateJob &j)
{
	SaveReport rep;
	if (j.slot < 0 || j.slot > STATE_SLOTS) { rep.status = ST_BAD_SLOT; return rep; }
	if (j.data.empty()) { rep.status = ST_WRITE; rep.write_rc = W_BAD_ARGS; return rep; }
	ensure_scanned();
	ensure_dirs();
	const bool resume = j.slot == 0;
	const std::string path = path_of(j.slot);
	Cached before;
	bool pt_had, pt_known;
	std::string pt_text;
	{
		SpinGuard g(lk_);
		before = entry(j.slot);
		pt_had = rsm_pt_; pt_known = rsm_pt_known_; pt_text = rsm_pt_text_;
	}
	/* 1. the old note is emptied (in place) so it can never describe the new state: an empty note means
	 *    "not checked", a stale one would refuse a good state after a power cut between 2 and 3. */
	if (before.has_meta) fs().write_all(path + ".meta", (const uint8_t *) "", 0);
	/* 2. the state itself: temp file, read back, swap (the previous state is kept on any failure) */
	WriteResult rc = atomic_write(path, j.data.data(), j.data.size(), before.has_main ? EXISTS_YES : before.has_old ? EXISTS_UNKNOWN : EXISTS_NO);
	if (rc != W_OK) {
		rep.status = ST_WRITE;
		rep.write_rc = rc;
		/* the old note was emptied: write it again for the old file, which is untouched */
		if (before.has_meta && before.info.present) {
			std::vector<uint8_t> old;
			if (file_read(path, old, 32u << 20)) {
				StateJob o;
				o.data.swap(old);
				o.saved = before.info.saved;
				o.play_seconds = before.info.play_seconds;
				std::string m = meta_text(l_, o);
				fs().write_all(path + ".meta", (const uint8_t *) m.data(), m.size());
			}
		}
		return rep;
	}
	/* 3. the companions, in place */
	if (!resume) {
		rep.thumb_ok = !j.thumb.empty() && put_file(path + ".thumb", j.thumb, false, EXISTS_UNKNOWN) == W_OK;
		if (!rep.thumb_ok && before.has_thumb) file_remove(path + ".thumb");
		rep.shot_ok = !j.shot.empty() && put_file(path + ".shot", j.shot, false, EXISTS_UNKNOWN) == W_OK;
		if (!rep.shot_ok && before.has_shot) file_remove(path + ".shot");
	}
	std::string m = meta_text(l_, j);
	rep.meta_ok = fs().write_all(path + ".meta", (const uint8_t *) m.data(), m.size());
	/* leftovers from older writes (seen in the listing) go */
	for (size_t i = 0; i < before.extra.size(); i++) file_remove(path_join(l_.states_dir(), before.extra[i]));
	bool pt_ok = pt_had, scene_ok = false;
	std::string key = j.patch_key.empty() ? "-\n" : j.patch_key;
	if (resume) {
		/* which patch the point was made with: rewritten only when it changed. A note cut short reads as a
		 * different patch, so the point is then not loaded: safe. */
		if (!pt_had || !pt_known || pt_text != key) {
			std::vector<uint8_t> kb(key.begin(), key.end());
			pt_ok = put_file(path + ".pt", kb, false, EXISTS_UNKNOWN) == W_OK;
			if (!pt_ok) file_remove(path + ".pt");
		}
		if (j.scene && !j.shot.empty()) {
			if (!scene_dir_ok_) scene_dir_ok_ = make_dirs(path_join(l_.root, "scenes"));
			scene_ok = put_file(l_.scene_path(), j.shot, false, EXISTS_UNKNOWN) == W_OK;
		}
	}
	(void) scene_ok;
	/* 4. the cache */
	{
		SpinGuard g(lk_);
		Cached &c = entry(j.slot);
		c.has_main = true; c.has_old = false; c.has_meta = rep.meta_ok; c.extra.clear();
		c.info.present = true;
		c.info.size = j.data.size();
		c.info.saved = j.saved;
		c.info.play_seconds = j.play_seconds;
		if (!resume) {
			c.has_thumb = rep.thumb_ok;
			c.has_shot = rep.shot_ok;
			int w, h;
			if (!rep.thumb_ok || !parse_thumb(j.thumb, c.thumb_px, &w, &h)) c.thumb_px.clear();
			c.info.has_thumb = !c.thumb_px.empty();
			c.shot_loaded = rep.shot_ok && parse_shot(j.shot, c.shot_px, &c.shot_w, &c.shot_h);
			if (!c.shot_loaded) { c.shot_px.clear(); c.shot_w = c.shot_h = 0; }
		} else {
			rsm_pt_ = pt_ok;
			rsm_pt_text_ = key;
			rsm_pt_known_ = pt_ok;
		}
	}
	return rep;
}

SaveReport States::save(int slot, const uint8_t *data, size_t n, const Frame &frame, int64_t play_seconds)
{
	if (slot < 1 || slot > STATE_SLOTS) {
		SaveReport r;
		r.status = ST_BAD_SLOT;
		return r;
	}
	return commit(prepare(slot, data, n, frame, play_seconds));
}

StateStatus States::read_checked(int slot, std::vector<uint8_t> &out)
{
	ensure_scanned();
	Cached c;
	{ SpinGuard g(lk_); c = entry(slot); }
	if (!c.info.present) return ST_EMPTY;
	std::string path = path_of(slot);
	if (!c.has_main && c.has_old) path += ".old";
	if (!file_read(path, out, 32u << 20)) {
		uint64_t sz = 0;
		if (!file_exists(path, &sz)) return ST_EMPTY;
		return sz > (32u << 20) ? ST_TOO_BIG : ST_READ;
	}
	Ini m;
	if (c.has_meta && m.load(path_of(slot) + ".meta") && meta_complete(m)) {
		unsigned want = (unsigned) strtoul(m.get("crc32").c_str(), 0, 16);
		if ((uint64_t) m.get_i64("size", -1) != out.size() || want != crc32(0, out.data(), out.size())) {
			out.clear();
			return ST_DAMAGED;
		}
	}
	return ST_OK;
}

StateStatus States::load(int slot, std::vector<uint8_t> &out)
{
	if (slot < 1 || slot > STATE_SLOTS) return ST_BAD_SLOT;
	return read_checked(slot, out);
}

bool States::remove(int slot)
{
	if (slot < 1 || slot > STATE_SLOTS) return false;
	ensure_scanned();
	Cached c;
	{ SpinGuard g(lk_); c = slot_[slot - 1]; }
	std::string p = l_.slot_path(slot);
	bool ok = true;
	if (c.has_main) ok = file_remove(p) && ok;
	if (c.has_old) ok = file_remove(p + ".old") && ok;
	if (c.has_thumb) file_remove(p + ".thumb");
	if (c.has_shot) file_remove(p + ".shot");
	if (c.has_meta) file_remove(p + ".meta");
	for (size_t i = 0; i < c.extra.size(); i++) file_remove(path_join(l_.states_dir(), c.extra[i]));
	SpinGuard g(lk_);
	if (ok) slot_[slot - 1] = Cached();
	return ok;
}

bool States::read_thumb(int slot, std::vector<uint16_t> &px, int *w, int *h)
{
	if (slot < 1 || slot > STATE_SLOTS) return false;
	ensure_scanned();
	SpinGuard g(lk_);
	const Cached &c = slot_[slot - 1];
	if (c.thumb_px.empty()) return false;
	px = c.thumb_px;
	if (w) *w = THUMB_W;
	if (h) *h = THUMB_H;
	return true;
}

bool States::read_shot(int slot, std::vector<uint16_t> &px, int *w, int *h)
{
	if (slot < 1 || slot > STATE_SLOTS) return false;
	ensure_scanned();
	bool need;
	{
		SpinGuard g(lk_);
		const Cached &c = slot_[slot - 1];
		if (c.shot_loaded) { px = c.shot_px; if (w) *w = c.shot_w; if (h) *h = c.shot_h; return true; }
		need = c.has_shot;
	}
	if (!need) return false;
	std::vector<uint8_t> f;
	std::vector<uint16_t> p;
	int sw = 0, sh = 0;
	if (!file_read(l_.slot_path(slot) + ".shot", f, 1u << 18) || !parse_shot(f, p, &sw, &sh)) return false;
	SpinGuard g(lk_);
	Cached &c = slot_[slot - 1];
	c.shot_px = p; c.shot_w = sw; c.shot_h = sh; c.shot_loaded = true;
	px.swap(p);
	if (w) *w = sw;
	if (h) *h = sh;
	return true;
}

/* ---- Continue ------------------------------------------------------------ */

SaveReport States::save_resume(const uint8_t *data, size_t n, const Frame &frame, int64_t play_seconds,
                               const std::string &patch_key)
{
	return commit(prepare_resume(data, n, frame, play_seconds, patch_key));
}

bool States::resume_present()
{
	ensure_scanned();
	SpinGuard g(lk_);
	return rsm_.info.present;
}

StateStatus States::load_resume(std::vector<uint8_t> &out)
{
	return read_checked(0, out);
}

States::PatchMatch States::resume_patch(const std::string &key)
{
	ensure_scanned();
	std::string want = key.empty() ? "-\n" : key;
	{
		SpinGuard g(lk_);
		if (!rsm_pt_) return PATCH_UNKNOWN;
		if (rsm_pt_known_) return rsm_pt_text_ == want ? PATCH_SAME : PATCH_DIFFERENT;
	}
	std::vector<uint8_t> b;
	if (!file_read(l_.resume_path() + ".pt", b, 4096)) return PATCH_UNKNOWN;
	std::string had((const char *) b.data(), b.size());
	{
		SpinGuard g(lk_);
		rsm_pt_text_ = had;
		rsm_pt_known_ = true;
	}
	return had == want ? PATCH_SAME : PATCH_DIFFERENT;
}

bool States::remove_resume()
{
	ensure_scanned();
	Cached c;
	bool pt;
	{ SpinGuard g(lk_); c = rsm_; pt = rsm_pt_; }
	std::string p = l_.resume_path();
	bool ok = true;
	if (c.has_main) ok = file_remove(p) && ok;
	if (c.has_old) ok = file_remove(p + ".old") && ok;
	if (c.has_meta) file_remove(p + ".meta");
	if (pt) file_remove(p + ".pt");
	for (size_t i = 0; i < c.extra.size(); i++) file_remove(path_join(l_.states_dir(), c.extra[i]));
	SpinGuard g(lk_);
	if (ok) { rsm_ = Cached(); rsm_pt_ = false; rsm_pt_known_ = false; }
	return ok;
}

bool States::resume_due(uint64_t now_ms, uint64_t interval_ms) const
{
	return !have_resume_mark_ || now_ms - last_resume_ms_ >= interval_ms;
}

/* ---- undo ---------------------------------------------------------------- */

bool States::undo_store(const uint8_t *data, size_t n)
{
	if (!data || !n) return false;
	undo_.assign(data, data + n);
	undo_have_ = true;
	return true;
}

StateStatus States::undo_fetch(std::vector<uint8_t> &out) const
{
	if (!undo_have_) return ST_EMPTY;
	out = undo_;
	return ST_OK;
}

bool States::undo_available() const { return undo_have_; }

void States::undo_clear()
{
	undo_.clear();
	undo_have_ = false;
}

bool States::save_scene(const Frame &frame)
{
	std::vector<uint8_t> s = shot_file(frame);
	if (s.empty()) return false;
	if (!scene_dir_ok_) scene_dir_ok_ = make_dirs(path_join(l_.root, "scenes"));
	return fs().write_all(l_.scene_path(), s.data(), s.size());
}

}  // namespace pk
