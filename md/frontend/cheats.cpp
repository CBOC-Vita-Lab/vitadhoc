/* See cheats.h. Copyright CBOC-Vita. MIT licence (see md/frontend/LICENSE). */
#include "cheats.h"
#include "pk_util.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

namespace mdp {

using namespace pk;

static const char kMdGgChars[] = "ABCDEFGHJKLMNPRSTVWXYZ0123456789";

bool md_gg_decode(const std::string &code, unsigned *address, unsigned *data)
{
	std::string c;
	for (size_t i = 0; i < code.size(); i++) if (code[i] != '-') c += code[i];
	if (c.size() != 8) return false;
	unsigned d = 0, a = 0;
	for (size_t i = 0; i < 8; i++) {
		const char *p = c[i] ? strchr(kMdGgChars, toupper((unsigned char) c[i])) : 0;
		if (!p) return false;
		const unsigned n = (unsigned) (p - kMdGgChars);
		switch (i) {
		case 0: d |= n << 3; break;
		case 1: d |= n >> 2; a |= (n & 3) << 14; break;
		case 2: a |= n << 9; break;
		case 3: a |= (n & 0xF) << 20 | (n >> 4) << 8; break;
		case 4: d |= (n & 1) << 12; a |= (n >> 1) << 16; break;
		case 5: d |= (n & 1) << 15 | (n >> 1) << 8; break;
		case 6: d |= (n >> 3) << 13; a |= (n & 7) << 5; break;
		default: a |= n; break;
		}
	}
	*address = a; *data = d;
	return true;
}

static bool is_hex(const std::string &s, size_t a, size_t n)
{
	if (n == 0 || a + n > s.size()) return false;
	for (size_t i = a; i < a + n; i++) if (!isxdigit((unsigned char) s[i])) return false;
	return true;
}
static unsigned hex(const std::string &s, size_t a, size_t n) { return (unsigned) strtoul(s.substr(a, n).c_str(), NULL, 16); }

bool sms_gg_decode(const std::string &code, unsigned *address, unsigned *data, int *compare)
{
	std::string c;
	for (size_t i = 0; i < code.size(); i++) if (code[i] != '-') c += code[i];
	if ((c.size() != 6 && c.size() != 9) || !is_hex(c, 0, c.size())) return false;
	*data = hex(c, 0, 2);
	*address = hex(c, 2, 3) | ((hex(c, 5, 1) ^ 0xF) << 12);
	*compare = -1;
	if (c.size() == 9) {
		unsigned r = (hex(c, 6, 1) << 4) | hex(c, 8, 1);
		r = ((r >> 2) | ((r & 3) << 6)) & 0xFF;       /* 2 bit right rotation */
		*compare = (int) (r ^ 0xBA);
	}
	return true;
}

/* one token -> one canonical part; 0 ok, else a CHEAT_E_* */
static int part_md(const std::string &t, std::string *out)
{
	unsigned a = 0, v = 0;
	const size_t n = t.size();
	if (n == 9 && t[4] == '-' && md_gg_decode(t, &a, &v)) {
	} else if ((n == 9 || n == 11) && (t[6] == ':' || t[6] == '=') && is_hex(t, 0, 6) && is_hex(t, 7, n - 7)) {
		a = hex(t, 0, 6); v = hex(t, 7, n - 7);
		if (n == 9 && a < 0xFF0000u) return CHEAT_E_FORMAT;       /* a byte patch of ROM does not exist */
		if (n == 9) a ^= 1u;      /* "AAAAAA:VV" = the byte at that 68000 address; the player stores a 00VV patch as Genesis Plus GX does, in the other byte of the word */
	} else return CHEAT_E_FORMAT;
	if (a >= 0x400000u && a < 0xFF0000u) return CHEAT_E_RANGE;
	a &= 0xFFFFFFu;
	*out = strf("%06X:%04X", a, v & 0xFFFFu);
	return 0;
}

static int part_sms(const std::string &t, std::string *out)
{
	unsigned a = 0, v = 0;
	int cmp = -1;
	const size_t n = t.size();
	if ((n == 7 || n == 11) && t[3] == '-' && is_hex(t, 0, 3) && is_hex(t, 4, 3) && (n == 7 || (t[7] == '-' && is_hex(t, 8, 3)))) {
		if (!sms_gg_decode(t, &a, &v, &cmp)) return CHEAT_E_FORMAT;
	} else if (n == 9 && t[4] == '-' && t[0] == '0' && t[1] == '0' && is_hex(t, 0, 4) && is_hex(t, 5, 4)) {      /* Pro Action Replay 00AA-AAVV */
		a = hex(t, 2, 2) << 8 | hex(t, 5, 2); v = hex(t, 7, 2);
	} else if (n == 7 && (t[4] == ':' || t[4] == '=') && is_hex(t, 0, 4) && is_hex(t, 5, 2)) {                 /* AAAA:VV */
		a = hex(t, 0, 4); v = hex(t, 5, 2);
	} else if (n == 10 && (t[4] == ':' || t[4] == '=') && t[7] == ':' && is_hex(t, 0, 4) && is_hex(t, 5, 2) && is_hex(t, 8, 2)) {   /* AAAA:VV:CC (the database's form) */
		a = hex(t, 0, 4); v = hex(t, 5, 2); cmp = (int) hex(t, 8, 2);
	} else if (n == 9 && (t[6] == ':' || t[6] == '=') && is_hex(t, 0, 6) && is_hex(t, 7, 2)) {                  /* Fusion ROM RRAAAA:VV */
		a = hex(t, 2, 4); v = hex(t, 7, 2); cmp = (int) hex(t, 0, 2);
	} else return CHEAT_E_FORMAT;
	*out = cmp >= 0 ? strf("%04X:%02X:%02X", a & 0xFFFFu, v & 0xFFu, (unsigned) cmp) : strf("%04X:%02X", a & 0xFFFFu, v & 0xFFu);
	return 0;
}

int cheat_canon(bool md, const std::string &text, std::string *parts)
{
	std::vector<std::string> cells;
	std::string cur;
	for (size_t i = 0; i <= text.size(); i++) {
		const unsigned char c = i < text.size() ? (unsigned char) text[i] : 0;
		if (!c || c == '+' || c == ',' || c == ';' || c == ' ' || c == '\t' || c == '\r' || c == '\n') {
			if (!cur.empty()) { cells.push_back(cur); cur.clear(); }
		} else cur += (char) toupper(c);
	}
	if (cells.empty()) return CHEAT_E_FORMAT;
	if (cells.size() > (size_t) CHEAT_PARTS_MAX) return CHEAT_E_TOO_LONG;
	std::string out;
	for (size_t i = 0; i < cells.size(); i++) {
		std::string p;
		const int r = md ? part_md(cells[i], &p) : part_sms(cells[i], &p);
		if (r) return r;
		if (!out.empty()) out += '+';
		out += p;
	}
	if (parts) *parts = out;
	return (int) cells.size();
}

/* ---- the list ------------------------------------------------------------------------------------ */

std::string CheatList::clean_label(const std::string &s)
{
	std::string o;
	for (size_t i = 0; i < s.size(); i++) {
		unsigned char c = (unsigned char) s[i];
		if (c < 0x20 || c == 0x7f) c = ' ';
		if (c == '"') c = '\'';
		if (c == ' ' && (o.empty() || o.back() == ' ')) continue;
		o += (char) c;
	}
	while (!o.empty() && o.back() == ' ') o.pop_back();
	if (o.size() > 60) {
		o.resize(60);
		while (!o.empty() && ((unsigned char) o.back() & 0xC0) == 0x80) o.pop_back();
		if (!o.empty() && ((unsigned char) o.back() & 0xC0) == 0xC0) o.pop_back();
	}
	return o;
}

void CheatList::parse(const std::string &text_in, std::vector<CheatEntry> &out, std::string &global_extra) const
{
	struct Raw { std::string desc, code, extra; bool on = false, has_code = false; };
	std::vector<Raw> items;
	std::string text = text_in + "\n", line;
	for (size_t p = 0; p < text.size(); p++) {
		if (text[p] != '\n') { line += text[p]; continue; }
		const std::string l = trim(line);
		line.clear();
		if (l.empty()) continue;
		const size_t eq = l.find('=');
		const std::string key = trim(eq == std::string::npos ? l : l.substr(0, eq));
		const std::string val = eq == std::string::npos ? "" : trim(l.substr(eq + 1));
		std::string unq = val;
		if (unq.size() >= 2 && unq.front() == '"' && unq.back() == '"') unq = unq.substr(1, unq.size() - 2);
		if (key == "cheats") continue;
		if (key.compare(0, 5, "cheat") == 0 && key.size() > 6 && isdigit((unsigned char) key[5])) {
			const size_t u = key.find('_');
			if (u != std::string::npos && u > 5) {
				const size_t idx = (size_t) strtoul(key.substr(5, u - 5).c_str(), NULL, 10);
				const std::string suf = key.substr(u + 1);
				if (idx < (size_t) CHEATS_MAX) {
					if (items.size() <= idx) items.resize(idx + 1);
					Raw &r = items[idx];
					if (suf == "desc") r.desc = unq;
					else if (suf == "code") { r.code = unq; r.has_code = true; }
					else if (suf == "enable") r.on = unq == "true" || unq == "1";
					else r.extra += "cheat@_" + suf + " = " + val + "\n";
					continue;
				}
			}
		}
		global_extra += l + "\n";
	}
	for (size_t k = 0; k < items.size(); k++) {
		const Raw &r = items[k];
		if (!r.has_code) continue;
		CheatEntry e;
		e.label = clean_label(r.desc);
		if (e.label.empty()) e.label = strf("Cheat %d", (int) out.size() + 1);
		e.code = r.code;
		e.on = r.on;
		e.extra = r.extra;            /* "cheat@_<key> = <value>" lines; the number is filled in when written */
		out.push_back(e);
		if (out.size() >= (size_t) CHEATS_MAX) break;
	}
}

int CheatList::load(const std::string &path, bool md)
{
	e_.clear(); global_extra_.clear(); err_.clear();
	dirty_ = blocked_ = false;
	path_ = path;
	md_ = md;
	uint64_t sz = 0;
	if (!file_exists(path_, &sz)) return 0;
	std::vector<uint8_t> raw;
	if (sz > 0 && (sz > (4u << 20) || !file_read(path_, raw, 4u << 20))) {
		blocked_ = true;
		err_ = "cheat file unreadable, left alone";
		return 0;
	}
	parse(std::string((const char *) raw.data(), raw.size()), e_, global_extra_);
	for (size_t i = 0; i < e_.size(); i++) {
		std::string parts;
		if (cheat_canon(md_, e_[i].code, &parts) > 0) { e_[i].parts = parts; e_[i].usable = true; }
	}
	return (int) e_.size();
}

int CheatList::merge_file(const std::string &path)
{
	std::vector<uint8_t> raw;
	if (path.empty() || !file_read(path, raw, 4u << 20)) return 0;
	std::vector<CheatEntry> in;
	std::string ignore;
	parse(std::string((const char *) raw.data(), raw.size()), in, ignore);
	int added = 0;
	for (size_t i = 0; i < in.size() && (int) e_.size() < CHEATS_MAX; i++) {
		std::string parts;
		if (cheat_canon(md_, in[i].code, &parts) <= 0) continue;
		bool have = false;
		for (size_t k = 0; k < e_.size(); k++) if (e_[k].parts == parts) { have = true; break; }
		if (have) continue;
		CheatEntry e;
		e.label = in[i].label; e.code = in[i].code; e.parts = parts; e.usable = true; e.on = false;
		e_.push_back(e);
		added++;
	}
	return added;
}

int CheatList::set_on(int i, bool on)
{
	if (i < 0 || i >= size()) return CHEAT_E_RANGE;
	if (e_[(size_t) i].on != on) { e_[(size_t) i].on = on; dirty_ = true; }
	return 0;
}

int CheatList::add(const std::string &code, const std::string &label)
{
	if (size() >= CHEATS_MAX) return CHEAT_E_FULL;
	std::string parts;
	const int r = cheat_canon(md_, code, &parts);
	if (r < 0) return r;
	for (size_t k = 0; k < e_.size(); k++) if (e_[k].parts == parts) return CHEAT_E_EXISTS;
	CheatEntry e;
	e.label = clean_label(label);
	if (e.label.empty()) e.label = strf("Cheat %d", size() + 1);
	e.code = parts;                      /* stored in its canonical form: the file reads the same everywhere */
	e.parts = parts;
	e.on = e.usable = true;
	e_.push_back(e);
	dirty_ = true;
	return size() - 1;
}

int CheatList::remove(int i)
{
	if (i < 0 || i >= size()) return CHEAT_E_RANGE;
	e_.erase(e_.begin() + i);
	dirty_ = true;
	return 0;
}

std::string CheatList::text() const
{
	std::string t = strf("cheats = %d\n", size());
	for (int i = 0; i < size(); i++) {
		const CheatEntry &c = e_[(size_t) i];
		t += strf("cheat%d_desc = \"%s\"\n", i, c.label.c_str());
		t += strf("cheat%d_code = \"%s\"\n", i, c.code.c_str());
		t += strf("cheat%d_enable = %s\n", i, c.on ? "true" : "false");
		const std::string &ex = c.extra;
		for (size_t q = 0; q < ex.size();) {
			size_t nl = ex.find('\n', q);
			std::string ln = ex.substr(q, nl - q);
			const size_t at = ln.find('@');
			if (at != std::string::npos) ln = ln.substr(0, at) + strf("%d", i) + ln.substr(at + 1);
			t += ln + "\n";
			if (nl == std::string::npos) break;
			q = nl + 1;
		}
	}
	t += global_extra_;
	return t;
}

int CheatList::save(SafeFile &sf)
{
	if (path_.empty()) return 0;
	if (blocked_) { err_ = "cheat file not saved: the existing file could not be read, it is left alone"; return CHEAT_E_IO; }
	make_dirs(path_dir(path_));
	const std::string t = text();
	const int r = sf.write(path_, (const uint8_t *) t.data(), t.size());
	if (r) { err_ = "cheat file not saved (" + sf.why() + "); the old file is untouched"; dirty_ = true; return CHEAT_E_IO; }
	err_.clear();
	dirty_ = false;
	return 0;
}

std::vector<std::string> CheatList::active_parts() const
{
	std::vector<std::string> v;
	for (size_t i = 0; i < e_.size(); i++) if (e_[i].on && e_[i].usable) v.push_back(e_[i].parts);
	return v;
}

}  // namespace mdp
