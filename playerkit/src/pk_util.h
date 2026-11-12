/*
 * playerkit: small shared utilities (checksums, file operations with a
 * replaceable back end, key=value files, strings).
 *
 * Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE).
 */
#ifndef PK_UTIL_H
#define PK_UTIL_H

#include <stddef.h>
#include <stdint.h>
#include <string>
#include <vector>

namespace pk {

/* ---- checksums --------------------------------------------------------- */
uint32_t crc32(uint32_t crc, const void *data, size_t n);   /* start with 0 */
uint32_t adler32(uint32_t adler, const void *data, size_t n); /* start with 1 */

/* ---- strings ----------------------------------------------------------- */
std::string trim(const std::string &s);
bool starts_with(const std::string &s, const char *p);
bool ends_with_ci(const std::string &s, const char *tail);
std::string lower(std::string s);
/* file name without folder and without its last extension ("a/b/Game.sfc" -> "Game") */
std::string path_stem(const std::string &p);
/* folder part with no trailing slash ("ux0:data/x/Game.sfc" -> "ux0:data/x") */
std::string path_dir(const std::string &p);
std::string path_join(const std::string &a, const std::string &b);
std::string strf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* ---- files -------------------------------------------------------------
 * Every file operation goes through FsOps so tests can make a card fail at an
 * exact step, and a platform can swap the calls. The defaults are stdio. */
struct FsOps {
	bool (*read_all)(const std::string &path, std::vector<uint8_t> &out, size_t max_bytes);
	bool (*write_all)(const std::string &path, const uint8_t *data, size_t n);
	bool (*rename_file)(const std::string &from, const std::string &to);
	bool (*remove_file)(const std::string &path);
	/* false when the file is not there; size and mtime (Unix seconds) may be NULL */
	bool (*stat_file)(const std::string &path, uint64_t *size, int64_t *mtime);
	bool (*make_dir)(const std::string &path);   /* true also when it already exists */
};
FsOps &fs();                 /* the active table */
FsOps default_fs_ops();      /* stdio */

bool file_read(const std::string &path, std::vector<uint8_t> &out, size_t max_bytes = 64u << 20);
bool file_exists(const std::string &path, uint64_t *size = 0, int64_t *mtime = 0);
bool file_remove(const std::string &path);
/* creates every folder of the path ("ux0:data/GBAdhoc/states/snes") */
bool make_dirs(const std::string &path);

/* Why a write failed, in words a player can act on (Notice text). */
enum WriteResult {
	W_OK = 0,
	W_NO_WRITE,         /* could not create or write the temporary file (card full or write-protected) */
	W_VERIFY,           /* the file read back different from what was written */
	W_NO_REPLACE,       /* could not put the new file in place (old file kept) */
	W_BAD_ARGS
};
const char *write_result_text(WriteResult r);

/* The safe write: path.tmp is written and read back (size and CRC) before
 * anything is touched; then the old file is set aside as path.old, the new one
 * renamed into place, and path.old removed. At every step a failure puts the
 * old file back, so a failed save never destroys the previous one (the Vita's
 * card refuses rename over an existing file, which is why this is not a plain
 * rename). */
WriteResult atomic_write(const std::string &path, const uint8_t *data, size_t n);
/* A write that a power cut interrupted between "old set aside" and "new in
 * place" leaves path.old and no path: put it back. Returns true if it did. */
bool recover_old(const std::string &path);

/* ---- key=value files ---------------------------------------------------- */
class Ini {
public:
	void parse(const std::string &text);      /* tolerant: bad lines are skipped */
	bool load(const std::string &path);
	std::string serialize() const;            /* "key=value\n" lines, in insertion order */
	bool has(const std::string &key) const;
	std::string get(const std::string &key, const std::string &def = std::string()) const;
	int get_int(const std::string &key, int def) const;
	int64_t get_i64(const std::string &key, int64_t def) const;
	void set(const std::string &key, const std::string &value);
	void set_int(const std::string &key, int64_t v);
	void erase(const std::string &key);
	size_t size() const { return kv_.size(); }
	const std::vector<std::pair<std::string, std::string> > &items() const { return kv_; }
private:
	std::vector<std::pair<std::string, std::string> > kv_;
};
/* Safe write of an Ini (atomic_write). */
WriteResult ini_save(const std::string &path, const Ini &ini);

/* The clock, replaceable for tests. Unix seconds. */
int64_t now_unix();
void set_clock_for_tests(int64_t (*fn)());

}  // namespace pk

#endif
