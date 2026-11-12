/*
 * Safe file writes for the WonderSwan player: battery RAM (.srm) and save states.
 * Portable (stdio only) so the Linux twin and the unit test use the same code.
 *
 * The rules (the same VitAdhoc's own players follow):
 *  - the new data is written to <file>.tmp and READ BACK; its size and CRC-32
 *    must match what was meant to be written, or nothing is replaced;
 *  - a file that already holds data is kept first as a VERIFIED backup (copied
 *    whole to a temporary name, read back, then given its name) before the
 *    session's first overwrite; with no verified copy the file is not replaced;
 *  - a file is never opened for writing in place when it holds data. On a Vita
 *    memory card rename() over an existing file is refused, so the old file moves
 *    aside to <file>.old, the new one moves in, and only then does .old go: at
 *    every moment one whole copy exists under a name wswan_recover_old() knows;
 *  - every failure is reported (return code and a text), never swallowed.
 *
 * Copyright CBOC-Vita. MIT licence (see wswan/frontend/LICENSE).
 */
#ifndef WSWAN_SAFE_IO_H
#define WSWAN_SAFE_IO_H

#include <stddef.h>
#include <stdint.h>

enum {
	WSWAN_IO_OK = 0,
	WSWAN_IO_WRITE = 1,    /* the temporary file could not be written */
	WSWAN_IO_VERIFY = 2,   /* what was written did not read back whole */
	WSWAN_IO_BACKUP = 3,   /* no verified backup of the existing file: it was NOT replaced */
	WSWAN_IO_REPLACE = 4   /* the new file could not be moved into place (the old one is as it was) */
};

uint32_t wswan_crc32(uint32_t crc, const void *data, size_t n);
/* Size and CRC of a file; 0 = read to the end without an error. */
int wswan_file_sum(const char *path, uint32_t *crc, long *size);
/* 1 when the file exists (any size). */
int wswan_file_exists(const char *path);
/* Reads a whole file (at most max_bytes). 0 = ok. */
int wswan_file_read(const char *path, uint8_t **data, size_t *n, size_t max_bytes);

/* Copies `from` to `to` through to.tmp and checks the copy by reading it back. `to` is never opened
 * for writing (the caller picks a free name). 0 = a whole, checked copy is at `to`. */
int wswan_copy_verified(const char *from, const char *to, char *err, size_t errlen);

/* If `path` is missing (or empty) and path.old holds data, moves it back. Returns 1 when it did.
 * Call before reading a save. */
int wswan_recover_old(const char *path);

/* Writes data[0..n) to `path` by the rules above.
 *  backup_dir: folder for the backup of an existing file (name <file>.<crc32 of the old content>.bak),
 *              or NULL for a backup beside the file (<file>.bak, or <file>.<crc>.bak when .bak holds
 *              other bytes). The folder must exist.
 *  keep_backup: make that backup first (the player passes 1 for the session's first write only).
 *  *backed_up: set to 1 when a verified backup of an existing file exists after the call.
 * Returns WSWAN_IO_OK or one of the codes above, with a text in err. */
int wswan_write_safe(const char *path, const void *data, size_t n, const char *backup_dir, int keep_backup,
                   int *backed_up, char *err, size_t errlen);

#endif
