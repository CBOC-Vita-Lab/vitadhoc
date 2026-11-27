/* See savefile.h. Copyright CBOC-Vita. MIT licence (see md/frontend/LICENSE). */
#include "savefile.h"
#include "pk_util.h"

namespace mdp {

using namespace pk;

std::string srm_for(const std::string &rom)
{
	size_t d = rom.find_last_of('.'), s = rom.find_last_of("/:");
	if (d == std::string::npos || (s != std::string::npos && d < s)) return rom + ".srm";
	return rom.substr(0, d) + ".srm";
}

bool SafeFile::backup(const std::string &path, const std::vector<uint8_t> &old)
{
	if (old.empty()) return true;               /* nothing in it to lose */
	std::string dst;
	if (dir_.empty()) dst = path + ".bak";
	else {
		make_dirs(dir_);
		size_t s = path.find_last_of("/:");
		const std::string base = s == std::string::npos ? path : path.substr(s + 1);
		dst = path_join(dir_, base + strf(".%08x.%u.bak", (unsigned) crc32(0, old.data(), old.size()), (unsigned) old.size()));
	}
	std::vector<uint8_t> have;
	if (file_read(dst, have, 64u << 20) && have == old) { last_ = dst; return true; }     /* already there, whole */
	/* atomic_write reads the file back (size and CRC) before it counts */
	if (atomic_write(dst, old.data(), old.size()) != W_OK) { why_ = "the backup could not be written: " + dst; return false; }
	if (!file_read(dst, have, 64u << 20) || have != old) { why_ = "the backup did not read back whole: " + dst; return false; }
	last_ = dst;
	backups_++;
	return true;
}

int SafeFile::write(const std::string &path, const uint8_t *data, size_t n)
{
	why_.clear();
	recover_old(path);      /* a write a power cut interrupted */
	if (!done_ || done_path_ != path) {
		std::vector<uint8_t> old;
		if (file_exists(path)) {
			if (!file_read(path, old, 64u << 20)) { why_ = "the existing file could not be read to back it up"; return 3; }
			if (!backup(path, old)) return 1;
		}
		done_ = true;
		done_path_ = path;
	}
	const WriteResult w = atomic_write(path, data, n);
	if (w != W_OK) { why_ = write_result_text(w); return 2; }
	return 0;
}

}  // namespace mdp
