/* See host_files.h. Copyright CBOC-Vita. MIT licence (see nes/frontend/LICENSE). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include "host_files.h"
#include "safe_io.h"

#define MAX_ROM_BYTES (24u * 1024 * 1024)

static std::string g_srm_path, g_backup_dir, g_sys_dir;
static std::vector<uint8_t> g_srm_tail;     /* the save file's bytes past the cartridge RAM */
static bool g_bak_done;
static char g_err[160];

static void set_err(const char *s) { snprintf(g_err, sizeof g_err, "%s", s ? s : ""); }

static std::string dir_of(const std::string &p)
{
	size_t s = p.find_last_of("/:");
	if (s == std::string::npos) return "";
	return p[s] == ':' ? p.substr(0, s + 1) : p.substr(0, s);
}

static std::string base_of(const std::string &p)
{
	size_t s = p.find_last_of("/:");
	return s == std::string::npos ? p : p.substr(s + 1);
}

void nes_host_set_system_dir(const char *dir) { g_sys_dir = dir ? dir : ""; }
void nes_host_set_backup_dir(const char *dir) { g_backup_dir = dir ? dir : ""; }
const char *nes_host_last_error(void) { return g_err; }

int nes_host_load_rom(const char *rom_path, const char *srm_path, char *err, size_t errlen)
{
	uint8_t *data = NULL;
	size_t n = 0;
	nes_host_unload();
	int rc = nes_file_read(rom_path, &data, &n, MAX_ROM_BYTES);
	if (rc == -2) { snprintf(err, errlen, "The file is too large for a NES game"); return 2; }
	if (rc || n < 16) { snprintf(err, errlen, "Cannot read the ROM file"); return 2; }
	const std::string rom_dir = dir_of(rom_path);
	if (nes_host_is_fds_image(data, n)) {
		/* the BIOS: next to the game first, then the player's data folder. Never shipped. */
		const std::string cand[2] = { rom_dir.empty() ? std::string(".") : rom_dir, g_sys_dir };
		bool found = false;
		nes_host_set_fds_bios(NULL, 0);
		for (int i = 0; i < 2 && !found; i++) {
			if (cand[i].empty()) continue;
			std::string f = cand[i] + (cand[i].back() == ':' || cand[i].back() == '/' ? "" : "/") + "disksys.rom";
			uint8_t *b = NULL;
			size_t bn = 0;
			if (nes_file_read(f.c_str(), &b, &bn, 1024 * 1024) == 0 && bn == 8192) {
				nes_host_set_fds_bios(b, bn);
				found = true;
			}
			free(b);
		}
	}
	const std::string name = base_of(rom_path);
	rc = nes_host_load_mem(data, n, name.c_str(), err, errlen);
	free(data);
	if (rc) return rc;
	g_srm_path = srm_path ? srm_path : "";
	g_bak_done = false;
	g_srm_tail.clear();
	g_err[0] = 0;

	/* The battery RAM goes in after the final power-on, as a frontend does after a libretro core's load. */
	size_t sz = nes_host_sram_size();
	if (sz && !g_srm_path.empty()) {
		nes_recover_old(g_srm_path.c_str());   /* a save left aside by an interrupted write */
		uint8_t *b = NULL;
		size_t bn = 0;
		if (nes_file_read(g_srm_path.c_str(), &b, &bn, 64u * 1024 * 1024) == 0 && bn) {
			nes_host_sram_load(b, bn);
			/* Bytes past the cartridge's RAM (another emulator's padding or trailer) are written back after it, so
			 * the file never shrinks. A shorter file is loaded as far as it goes and rewritten whole at the next
			 * save, the old one kept first. */
			if (bn > sz) g_srm_tail.assign(b + sz, b + bn);
		}
		free(b);
	}
	if (sz) nes_host_sram_mark_saved();
	return 0;
}

static int write_save(const uint8_t *data, size_t n)
{
	std::vector<uint8_t> all(data, data + n);
	all.insert(all.end(), g_srm_tail.begin(), g_srm_tail.end());
	char err[160] = "";
	int bak = 0;
	int r = nes_write_safe(g_srm_path.c_str(), all.data(), all.size(), g_backup_dir.empty() ? NULL : g_backup_dir.c_str(),
	    g_bak_done ? 0 : 1, &bak, err, sizeof err);
	if (r == NES_IO_OK) {
		g_bak_done = true;   /* the backup of what was there at the start exists (or nothing was there) */
		g_err[0] = 0;
	} else {
		set_err(err[0] ? err : "the save could not be written");
	}
	return r;
}

int nes_host_sram_save(void)
{
	size_t sz = nes_host_sram_size();
	if (!sz || g_srm_path.empty()) return 0;
	std::vector<uint8_t> copy(nes_host_sram_data(), nes_host_sram_data() + sz);
	int r = write_save(copy.data(), sz);
	if (r == 0) nes_host_sram_mark_saved();
	return r;
}

size_t nes_host_sram_take(uint8_t *out, size_t cap)
{
	size_t sz = nes_host_sram_size();
	if (!sz || sz > cap || g_srm_path.empty()) return 0;
	memcpy(out, nes_host_sram_data(), sz);
	nes_host_sram_mark_saved();
	return sz;
}

int nes_host_sram_write(const uint8_t *data, size_t n)
{
	if (!n || g_srm_path.empty()) return 0;
	return write_save(data, n);
}

void nes_host_sram_forget(void) { nes_host_sram_mark_unsaved(); }

int nes_host_state_save(const char *path)
{
	size_t cap = nes_host_state_size();
	if (!cap) return 1;
	std::vector<uint8_t> buf(cap);
	size_t n = nes_host_state_save_mem(buf.data(), buf.size());
	if (!n) { set_err("the state could not be made"); return 2; }
	char err[160] = "";
	int r = nes_write_safe(path, buf.data(), n, NULL, 0, NULL, err, sizeof err);
	if (r) { set_err(err); return 2; }
	g_err[0] = 0;
	return 0;
}

int nes_host_state_load(const char *path)
{
	nes_recover_old(path);
	uint8_t *b = NULL;
	size_t n = 0;
	if (nes_file_read(path, &b, &n, 8u * 1024 * 1024) != 0) return 2;
	int r = nes_host_state_load_mem(b, n);
	free(b);
	return r;
}
