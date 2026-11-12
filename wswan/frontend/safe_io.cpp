/* See safe_io.h. Copyright CBOC-Vita. MIT licence (see wswan/frontend/LICENSE). */
#include "safe_io.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <string>
#include <vector>

uint32_t wswan_crc32(uint32_t crc, const void *data, size_t n)
{
	static uint32_t tab[256];
	static bool ready;
	if (!ready) {
		for (uint32_t i = 0; i < 256; i++) {
			uint32_t c = i;
			for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
			tab[i] = c;
		}
		ready = true;
	}
	const uint8_t *p = (const uint8_t *) data;
	crc = ~crc;
	while (n--) crc = tab[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
	return ~crc;
}

int wswan_file_sum(const char *path, uint32_t *crc, long *size)
{
	FILE *f = fopen(path, "rb");
	if (!f) return -1;
	unsigned char buf[4096];
	size_t n;
	uint32_t c = 0;
	long total = 0;
	while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
		c = wswan_crc32(c, buf, n);
		total += (long) n;
	}
	int ok = !ferror(f);
	if (fclose(f) != 0) ok = 0;
	if (!ok) return -1;
	*crc = c;
	*size = total;
	return 0;
}

int wswan_file_exists(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f) return 0;
	fclose(f);
	return 1;
}

int wswan_file_read(const char *path, uint8_t **data, size_t *n, size_t max_bytes)
{
	*data = NULL;
	*n = 0;
	FILE *f = fopen(path, "rb");
	if (!f) return -1;
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (sz < 0 || (size_t) sz > max_bytes) { fclose(f); return -2; }
	uint8_t *b = (uint8_t *) malloc(sz ? (size_t) sz : 1);
	if (!b) { fclose(f); return -3; }
	size_t got = sz ? fread(b, 1, (size_t) sz, f) : 0;
	fclose(f);
	if (got != (size_t) sz) { free(b); return -4; }
	*data = b;
	*n = (size_t) sz;
	return 0;
}

static void err_set(char *err, size_t cap, const char *text)
{
	if (err && cap) snprintf(err, cap, "%s", text && *text ? text : "write incomplete");
}

int wswan_copy_verified(const char *from, const char *to, char *err, size_t errlen)
{
	std::string tmp = std::string(to) + ".tmp";
	unsigned char buf[4096];
	size_t n;
	uint32_t crc = 0, back;
	long total = 0, bsize;
	int ok = 1;

	errno = 0;
	FILE *a = fopen(from, "rb");
	if (!a) { err_set(err, errlen, strerror(errno)); return -1; }
	errno = 0;
	FILE *b = fopen(tmp.c_str(), "wb");
	if (!b) { err_set(err, errlen, strerror(errno)); fclose(a); return -1; }
	while ((n = fread(buf, 1, sizeof buf, a)) > 0) {
		if (fwrite(buf, 1, n, b) != n) { err_set(err, errlen, errno ? strerror(errno) : NULL); ok = 0; break; }
		crc = wswan_crc32(crc, buf, n);
		total += (long) n;
	}
	if (ok && ferror(a)) { err_set(err, errlen, "read error"); ok = 0; }
	fclose(a);
	if (fclose(b) != 0 && ok) { err_set(err, errlen, errno ? strerror(errno) : NULL); ok = 0; }
	if (ok && (wswan_file_sum(tmp.c_str(), &back, &bsize) != 0 || back != crc || bsize != total)) {
		err_set(err, errlen, "the copy did not read back whole");
		ok = 0;
	}
	if (ok && rename(tmp.c_str(), to) != 0) { err_set(err, errlen, strerror(errno)); ok = 0; }
	if (ok && (wswan_file_sum(to, &back, &bsize) != 0 || back != crc || bsize != total)) {
		err_set(err, errlen, "the copy did not read back whole");
		ok = 0;
		remove(to);
	}
	if (!ok) remove(tmp.c_str());
	return ok ? 0 : -1;
}

int wswan_recover_old(const char *path)
{
	std::string old = std::string(path) + ".old";
	uint32_t c;
	long sz = 0, osz = 0;
	bool have = wswan_file_sum(path, &c, &sz) == 0 && sz > 0;
	if (have) return 0;
	if (wswan_file_sum(old.c_str(), &c, &osz) != 0 || osz <= 0) return 0;
	if (wswan_file_exists(path)) remove(path);   /* an empty file in the way */
	return rename(old.c_str(), path) == 0 ? 1 : 0;
}

static std::string hex8(uint32_t v)
{
	char b[16];
	snprintf(b, sizeof b, "%08x", (unsigned) v);
	return b;
}

/* Keeps the existing file `path` (sum crc/size) as a verified backup. 0 = a verified copy exists. */
static int keep_old(const char *path, uint32_t crc, long size, const char *backup_dir, char *err, size_t errlen)
{
	std::string bak;
	if (backup_dir && *backup_dir) {
		const char *base = strrchr(path, '/');
		const char *colon = strrchr(path, ':');
		if (colon && (!base || colon > base)) base = colon;
		base = base ? base + 1 : path;
		mkdir(backup_dir, 0777); // one level; an existing directory is fine
		bak = std::string(backup_dir) + "/" + base + "." + hex8(crc) + ".bak";
	} else {
		bak = std::string(path) + ".bak";
	}
	for (int attempt = 0; attempt < 2; attempt++) {
		uint32_t bc;
		long bs;
		if (wswan_file_exists(bak.c_str())) {
			if (wswan_file_sum(bak.c_str(), &bc, &bs) == 0 && bc == crc && bs == size) return 0;   /* already kept */
			if (attempt == 0 && !(backup_dir && *backup_dir)) {
				/* .bak holds other bytes (an earlier session): this one gets a name of its own */
				bak = std::string(path) + "." + hex8(crc) + ".bak";
				continue;
			}
			remove(bak.c_str());   /* same name, other bytes: a broken one */
		}
		break;
	}
	return wswan_copy_verified(path, bak.c_str(), err, errlen);
}

int wswan_write_safe(const char *path, const void *data, size_t n, const char *backup_dir, int keep_backup,
                   int *backed_up, char *err, size_t errlen)
{
	if (backed_up) *backed_up = 0;
	if (err && errlen) err[0] = 0;
	std::string tmp = std::string(path) + ".tmp", old = std::string(path) + ".old";
	wswan_recover_old(path);

	uint32_t ocrc = 0;
	long osize = 0;
	bool exists = wswan_file_exists(path);
	bool had = false;
	if (exists) {
		if (wswan_file_sum(path, &ocrc, &osize) != 0) {
			err_set(err, errlen, "the existing file cannot be read");
			return WSWAN_IO_BACKUP;
		}
		had = osize > 0;
	}
	if (keep_backup && had) {
		if (keep_old(path, ocrc, osize, backup_dir, err, errlen) != 0) {
			if (err && errlen && !err[0]) err_set(err, errlen, "backup failed");
			return WSWAN_IO_BACKUP;
		}
		if (backed_up) *backed_up = 1;
	}

	uint32_t crc = wswan_crc32(0, data, n), back;
	long bsize;
	errno = 0;
	FILE *f = fopen(tmp.c_str(), "wb");
	if (!f) { err_set(err, errlen, strerror(errno)); return WSWAN_IO_WRITE; }
	bool ok = n == 0 || fwrite(data, 1, n, f) == n;
	if (!ok) err_set(err, errlen, errno ? strerror(errno) : "write incomplete");
	if (fclose(f) != 0 && ok) { err_set(err, errlen, errno ? strerror(errno) : "write incomplete"); ok = false; }
	if (!ok) { remove(tmp.c_str()); return WSWAN_IO_WRITE; }
	if (wswan_file_sum(tmp.c_str(), &back, &bsize) != 0 || back != crc || (size_t) bsize != n) {
		err_set(err, errlen, "the new file did not read back whole");
		remove(tmp.c_str());
		return WSWAN_IO_VERIFY;
	}

	if (exists) {
		remove(old.c_str());   /* a stale one: `path` exists, so it is not the only copy */
		if (rename(path, old.c_str()) != 0) {
			err_set(err, errlen, strerror(errno));
			remove(tmp.c_str());
			return WSWAN_IO_REPLACE;
		}
	}
	if (rename(tmp.c_str(), path) != 0) {
		err_set(err, errlen, strerror(errno));
		if (exists) rename(old.c_str(), path);   /* the old file goes back */
		remove(tmp.c_str());
		return WSWAN_IO_REPLACE;
	}
	if (wswan_file_sum(path, &back, &bsize) != 0 || back != crc || (size_t) bsize != n) {
		err_set(err, errlen, "the file did not read back whole after the move");
		remove(path);
		if (exists) rename(old.c_str(), path);
		return WSWAN_IO_VERIFY;
	}
	if (exists) remove(old.c_str());
	return WSWAN_IO_OK;
}
