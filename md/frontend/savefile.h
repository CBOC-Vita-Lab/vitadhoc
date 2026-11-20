/*
 * The Sega player's file writers: a save written through the safe path (temp file read back, old file kept until the new
 * one is in place: pk::atomic_write), with a verified backup of the file that is there the first time this session replaces
 * it. Portable (no console calls). One instance per kind of file (battery save, cheat list), used by the writer thread.
 *
 * Copyright CBOC-Vita. MIT licence (see md/frontend/LICENSE).
 */
#ifndef MD_SAVEFILE_H
#define MD_SAVEFILE_H

#include <stddef.h>
#include <stdint.h>
#include <string>
#include <vector>

namespace mdp {

/* "ux0:data/GBAdhoc/roms/Game.bin" -> ".../roms/Game.srm" (the extension replaced; none: ".srm" appended) */
std::string srm_for(const std::string &rom);

class SafeFile {
public:
	/* backup_dir: where the first replaced version of a file goes ("" = beside the file, <name>.bak) */
	explicit SafeFile(const std::string &backup_dir = std::string()) : dir_(backup_dir) {}
	void set_backup_dir(const std::string &d) { dir_ = d; }
	/* Writes `n` bytes to `path`. Returns 0, or a code (1 backup failed, 2 write failed, 3 could not read the old file) with
	 * why() set; on any failure the old file is exactly as it was. The backup of the old file is made once per path per
	 * session (a content that is already backed up is not copied again); it is read back and compared before the old file is touched. */
	int write(const std::string &path, const uint8_t *data, size_t n);
	const std::string &why() const { return why_; }
	/* how many backups were made, and the last one's path (for the log and tests) */
	int backups() const { return backups_; }
	const std::string &last_backup() const { return last_; }
private:
	bool backup(const std::string &path, const std::vector<uint8_t> &old);
	std::string dir_, why_, last_, done_path_;
	bool done_ = false;
	int backups_ = 0;
};

}  // namespace mdp

#endif
