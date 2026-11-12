/* See pk_util.h. Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE). */
#include "pk_util.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#ifdef __vita__
#include <psp2/io/stat.h>
#include <psp2/io/dirent.h>
#endif

namespace pk {

/* ---- checksums --------------------------------------------------------- */

static uint32_t g_crc_table[256];
static bool g_crc_ready;

static void crc_init(void)
{
	for (uint32_t i = 0; i < 256; i++) {
		uint32_t c = i;
		for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
		g_crc_table[i] = c;
	}
	g_crc_ready = true;
}

uint32_t crc32(uint32_t crc, const void *data, size_t n)
{
	if (!g_crc_ready) crc_init();
	const uint8_t *p = (const uint8_t *) data;
	uint32_t c = ~crc;
	for (size_t i = 0; i < n; i++) c = g_crc_table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
	return ~c;
}

uint32_t adler32(uint32_t adler, const void *data, size_t n)
{
	const uint8_t *p = (const uint8_t *) data;
	uint32_t a = adler & 0xFFFF, b = adler >> 16;
	for (size_t i = 0; i < n; i++) {
		a = (a + p[i]) % 65521u;
		b = (b + a) % 65521u;
	}
	return (b << 16) | a;
}

/* ---- strings ----------------------------------------------------------- */

std::string trim(const std::string &s)
{
	size_t a = 0, b = s.size();
	while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) a++;
	while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) b--;
	return s.substr(a, b - a);
}

bool starts_with(const std::string &s, const char *p)
{
	size_t n = strlen(p);
	return s.size() >= n && memcmp(s.data(), p, n) == 0;
}

std::string lower(std::string s)
{
	for (size_t i = 0; i < s.size(); i++)
		if (s[i] >= 'A' && s[i] <= 'Z') s[i] = (char) (s[i] + 32);
	return s;
}

bool ends_with_ci(const std::string &s, const char *tail)
{
	size_t n = strlen(tail);
	if (s.size() < n) return false;
	return lower(s.substr(s.size() - n)) == lower(tail);
}

static size_t last_sep(const std::string &p)
{
	return p.find_last_of("/:");
}

std::string path_stem(const std::string &p)
{
	size_t s = last_sep(p);
	std::string n = s == std::string::npos ? p : p.substr(s + 1);
	size_t d = n.find_last_of('.');
	return (d == std::string::npos || d == 0) ? n : n.substr(0, d);
}

std::string path_dir(const std::string &p)
{
	size_t s = last_sep(p);
	if (s == std::string::npos) return std::string();
	return p[s] == ':' ? p.substr(0, s + 1) : p.substr(0, s);
}

std::string path_join(const std::string &a, const std::string &b)
{
	if (a.empty()) return b;
	if (b.empty()) return a;
	char e = a[a.size() - 1];
	return (e == '/' || e == ':') ? a + b : a + "/" + b;
}

std::string strf(const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	if (n < 0) return std::string();
	if ((size_t) n < sizeof buf) return std::string(buf, (size_t) n);
	std::string big((size_t) n + 1, '\0');
	va_start(ap, fmt);
	vsnprintf(&big[0], big.size(), fmt, ap);
	va_end(ap);
	big.resize((size_t) n);
	return big;
}

/* ---- default file operations (stdio) ----------------------------------- */

static bool d_read_all(const std::string &path, std::vector<uint8_t> &out, size_t max_bytes)
{
	FILE *f = fopen(path.c_str(), "rb");
	if (!f) return false;
	out.clear();
	uint8_t buf[16384];
	size_t n;
	bool ok = true;
	while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
		if (out.size() + n > max_bytes) { ok = false; break; }
		out.insert(out.end(), buf, buf + n);
	}
	if (ferror(f)) ok = false;
	fclose(f);
	return ok;
}

static bool d_write_all(const std::string &path, const uint8_t *data, size_t n)
{
	FILE *f = fopen(path.c_str(), "wb");
	if (!f) return false;
	bool ok = n == 0 || fwrite(data, 1, n, f) == n;
	if (fflush(f) != 0) ok = false;
	if (fclose(f) != 0) ok = false;
	return ok;
}

static bool d_rename(const std::string &from, const std::string &to)
{
	return rename(from.c_str(), to.c_str()) == 0;
}

static bool d_remove(const std::string &path)
{
	return remove(path.c_str()) == 0;
}

static bool d_stat(const std::string &path, uint64_t *size, int64_t *mtime)
{
	struct stat st;
	if (stat(path.c_str(), &st) != 0) return false;
	if (S_ISDIR(st.st_mode)) return false;
	if (size) *size = (uint64_t) st.st_size;
	if (mtime) *mtime = (int64_t) st.st_mtime;
	return true;
}

static bool d_mkdir(const std::string &path)
{
#ifdef __vita__
	int rc = sceIoMkdir(path.c_str(), 0777);
	(void) rc;     /* "exists" is a normal answer; check with stat */
	struct stat st;
	return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#else
	if (mkdir(path.c_str(), 0777) == 0) return true;
	struct stat st;
	return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

static bool d_list_dir(const std::string &dir, std::vector<std::string> &names)
{
	names.clear();
#ifdef __vita__
	SceUID d = sceIoDopen(dir.c_str());
	if (d < 0) return false;
	SceIoDirent e;
	for (;;) {
		memset(&e, 0, sizeof e);
		if (sceIoDread(d, &e) <= 0) break;
		if (SCE_S_ISDIR(e.d_stat.st_mode)) continue;
		names.push_back(e.d_name);
	}
	sceIoDclose(d);
	return true;
#else
	DIR *d = opendir(dir.c_str());
	if (!d) return false;
	while (struct dirent *e = readdir(d)) {
		if (e->d_name[0] == '.' && (!e->d_name[1] || (e->d_name[1] == '.' && !e->d_name[2]))) continue;
		names.push_back(e->d_name);
	}
	closedir(d);
	return true;
#endif
}

FsOps default_fs_ops()
{
	FsOps o;
	o.list_dir = d_list_dir;
	o.read_all = d_read_all;
	o.write_all = d_write_all;
	o.rename_file = d_rename;
	o.remove_file = d_remove;
	o.stat_file = d_stat;
	o.make_dir = d_mkdir;
	return o;
}

FsOps &fs()
{
	static FsOps ops = default_fs_ops();
	return ops;
}

bool file_read(const std::string &path, std::vector<uint8_t> &out, size_t max_bytes)
{
	return fs().read_all(path, out, max_bytes);
}

bool file_exists(const std::string &path, uint64_t *size, int64_t *mtime)
{
	return fs().stat_file(path, size, mtime);
}

bool file_remove(const std::string &path)
{
	return fs().remove_file(path);
}

bool make_dirs(const std::string &path)
{
	if (path.empty()) return false;
	std::string cur;
	size_t i = 0;
	if (path[0] == '/') {
		cur = "/";
		i = 1;
	} else {
		size_t colon = path.find(':'), slash = path.find('/');   /* a device prefix ("ux0:") is not a folder */
		if (colon != std::string::npos && (slash == std::string::npos || colon < slash)) {
			cur = path.substr(0, colon + 1);
			i = colon + 1;
			if (i < path.size() && path[i] == '/') i++;
		}
	}
	bool ok = true;
	while (i < path.size()) {
		size_t j = path.find('/', i);
		if (j == std::string::npos) j = path.size();
		if (j > i) {
			std::string comp = path.substr(i, j - i);
			char e = cur.empty() ? 0 : cur[cur.size() - 1];
			cur = (cur.empty() || e == '/' || e == ':') ? cur + comp : cur + "/" + comp;
			ok = fs().make_dir(cur) && ok;
		}
		i = j + 1;
	}
	return ok;
}

/* ---- the safe write ---------------------------------------------------- */

const char *write_result_text(WriteResult r)
{
	switch (r) {
	case W_OK: return "saved";
	case W_NO_WRITE: return "could not write to the memory card (full or write-protected)";
	case W_VERIFY: return "the card did not keep what was written";
	case W_NO_REPLACE: return "could not replace the old file (it was kept)";
	default: return "bad request";
	}
}

bool recover_old(const std::string &path)
{
	if (file_exists(path)) return false;
	std::string old = path + ".old";
	if (!file_exists(old)) return false;
	return fs().rename_file(old, path);
}

bool list_dir(const std::string &dir, std::vector<std::string> &names)
{
	names.clear();
	return fs().list_dir && fs().list_dir(dir, names);
}

void init_shared()
{
	crc32(0, "", 0);
	(void) fs();
}

WriteResult atomic_write(const std::string &path, const uint8_t *data, size_t n, Exists hint)
{
	if (path.empty() || (!data && n)) return W_BAD_ARGS;
	const std::string tmp = path + ".tmp", old = path + ".old";
	/* "wb" truncates a leftover temp file: no remove first (a lookup of a missing name costs a folder pass) */
	if (!fs().write_all(tmp, data, n)) {
		fs().remove_file(tmp);
		return W_NO_WRITE;
	}
	/* read back and compare: size and CRC */
	{
		std::vector<uint8_t> back;
		bool ok = fs().read_all(tmp, back, n + 1) && back.size() == n &&
		          crc32(0, back.data(), back.size()) == crc32(0, data, n);
		if (!ok) {
			fs().remove_file(tmp);
			return W_VERIFY;
		}
	}
	/* set the old file aside. The Vita's card refuses to rename over an existing name, so path.old must
	 * be free: it is removed after every write, and a leftover (a power cut) is cleared when it is in the way. */
	bool had = false;
	if (hint != EXISTS_NO) {
		if (fs().rename_file(path, old)) had = true;
		else if (hint == EXISTS_YES || file_exists(path)) {
			if (!file_exists(old) || !fs().remove_file(old) || !fs().rename_file(path, old)) {
				fs().remove_file(tmp);
				return W_NO_REPLACE;
			}
			had = true;
		} else if (file_exists(old)) {
			/* no file but a leftover path.old: the previous write was cut between its two renames. That
			 * leftover is the last good copy; the new file replaces it below. */
			had = true;
		}
	}
	if (!fs().rename_file(tmp, path)) {
		bool ok = false;
		if (!had && hint == EXISTS_NO && file_exists(path)) {
			/* the hint was wrong: do it the long way */
			if ((!file_exists(old) || fs().remove_file(old)) && fs().rename_file(path, old)) {
				if (fs().rename_file(tmp, path)) { fs().remove_file(old); return W_OK; }
				fs().rename_file(old, path);
			}
		} else if (had && !file_exists(path)) {
			ok = fs().rename_file(old, path);      /* the old one comes back */
		}
		(void) ok;
		fs().remove_file(tmp);
		return W_NO_REPLACE;
	}
	if (had) fs().remove_file(old);
	return W_OK;
}

/* ---- Ini --------------------------------------------------------------- */

void Ini::parse(const std::string &text)
{
	kv_.clear();
	size_t i = 0;
	while (i < text.size()) {
		size_t e = text.find('\n', i);
		if (e == std::string::npos) e = text.size();
		std::string line = trim(text.substr(i, e - i));
		i = e + 1;
		if (line.empty() || line[0] == '#' || line[0] == ';' || line[0] == '[') continue;
		size_t eq = line.find('=');
		if (eq == std::string::npos || eq == 0) continue;
		std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
		if (k.empty()) continue;
		set(k, v);
	}
}

bool Ini::load(const std::string &path)
{
	std::vector<uint8_t> b;
	if (!file_read(path, b, 1u << 20)) return false;
	/* a UTF-8 byte order mark is skipped */
	size_t off = (b.size() >= 3 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF) ? 3 : 0;
	parse(std::string((const char *) b.data() + off, b.size() - off));
	return true;
}

std::string Ini::serialize() const
{
	std::string s;
	for (size_t i = 0; i < kv_.size(); i++) s += kv_[i].first + "=" + kv_[i].second + "\n";
	return s;
}

bool Ini::has(const std::string &key) const
{
	for (size_t i = 0; i < kv_.size(); i++) if (kv_[i].first == key) return true;
	return false;
}

std::string Ini::get(const std::string &key, const std::string &def) const
{
	for (size_t i = kv_.size(); i-- > 0;) if (kv_[i].first == key) return kv_[i].second;
	return def;
}

static bool parse_i64(const std::string &s, int64_t *out)
{
	if (s.empty()) return false;
	char *end = 0;
	long long v = strtoll(s.c_str(), &end, 0);
	if (!end || *end) return false;
	*out = (int64_t) v;
	return true;
}

int64_t Ini::get_i64(const std::string &key, int64_t def) const
{
	int64_t v;
	return has(key) && parse_i64(get(key), &v) ? v : def;
}

int Ini::get_int(const std::string &key, int def) const
{
	int64_t v = get_i64(key, def);
	if (v > 0x7fffffff) return 0x7fffffff;
	if (v < -0x7fffffff) return -0x7fffffff;
	return (int) v;
}

void Ini::set(const std::string &key, const std::string &value)
{
	for (size_t i = 0; i < kv_.size(); i++)
		if (kv_[i].first == key) { kv_[i].second = value; return; }
	kv_.push_back(std::make_pair(key, value));
}

void Ini::set_int(const std::string &key, int64_t v)
{
	set(key, strf("%lld", (long long) v));
}

void Ini::erase(const std::string &key)
{
	for (size_t i = 0; i < kv_.size(); i++)
		if (kv_[i].first == key) { kv_.erase(kv_.begin() + (long) i); return; }
}

WriteResult ini_save(const std::string &path, const Ini &ini)
{
	std::string s = ini.serialize();
	return atomic_write(path, (const uint8_t *) s.data(), s.size());
}

/* ---- clock -------------------------------------------------------------- */

static int64_t (*g_clock)() = 0;
int64_t now_unix() { return g_clock ? g_clock() : (int64_t) time(0); }
void set_clock_for_tests(int64_t (*fn)()) { g_clock = fn; }

}  // namespace pk
