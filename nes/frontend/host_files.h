/*
 * Optional file layer over host.h: loads ROMs and saves from paths and writes battery RAM and states by the safe rules
 * in safe_io.h (temporary file, read-back check, verified backup before the first overwrite, never in place). Used by
 * the Linux twin and the standalone nes.bin; NOT part of nescore.o (a host app with its own save code does not need it).
 *
 * Copyright CBOC-Vita. MIT licence (see nes/frontend/LICENSE).
 */
#ifndef NES_HOST_FILES_H
#define NES_HOST_FILES_H
#include "host.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Folder that may hold disksys.rom. A disksys.rom beside the ROM is tried first. Set before nes_host_load_rom. */
void nes_host_set_system_dir(const char *dir);
/* Load a ROM file (see nes_host_load_mem) and the battery RAM from srm_path (a RetroArch .srm: the raw bytes) when
 * that file exists; a .srm longer than the RAM keeps its extra bytes when written back. Returns 0, else a reason in err. */
int nes_host_load_rom(const char *rom_path, const char *srm_path, char *err, size_t errlen);
/* Folder for the backup of an existing save before the first overwrite (NULL: beside the save as .bak). It is created
 * (one level) when missing. */
void nes_host_set_backup_dir(const char *dir);
const char *nes_host_last_error(void);   /* text of the last failed save or state write */
int nes_host_sram_save(void);            /* 0 on success, else a NES_IO_* code */
/* For a save written on another thread: take copies the RAM into out and counts it as saved; write writes such a copy
 * (any thread, one at a time); after a failed write call forget so the RAM counts as unsaved again. */
size_t nes_host_sram_take(uint8_t *out, size_t cap);
int nes_host_sram_write(const uint8_t *data, size_t n);
void nes_host_sram_forget(void);
/* States to and from files (no backup of the old slot). load: 2 = file missing, 3 = not a state. */
int nes_host_state_save(const char *path);
int nes_host_state_load(const char *path);

#ifdef __cplusplus
}
#endif
#endif
