/* Stand-in for libretro-common streams/file_stream.h: read-only files on stdio,
 * only what the core calls. Copyright CBOC-Vita. MIT licence (see nes/frontend/LICENSE). */
#ifndef NES_COMPAT_FILE_STREAM_H
#define NES_COMPAT_FILE_STREAM_H
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <libretro.h>

typedef struct { FILE *f; } RFILE;

static inline RFILE *filestream_open(const char *path, unsigned mode, unsigned hints)
{
	(void) hints;
	if (mode != RETRO_VFS_FILE_ACCESS_READ) return NULL;
	FILE *f = fopen(path, "rb");
	if (!f) return NULL;
	RFILE *r = (RFILE *) malloc(sizeof *r);
	if (!r) { fclose(f); return NULL; }
	r->f = f;
	return r;
}
static inline int64_t filestream_read(RFILE *r, void *buf, int64_t n) { return (int64_t) fread(buf, 1, (size_t) n, r->f); }
static inline int64_t filestream_seek(RFILE *r, int64_t off, int pos)
{
	int w = pos == RETRO_VFS_SEEK_POSITION_START ? SEEK_SET : pos == RETRO_VFS_SEEK_POSITION_END ? SEEK_END : SEEK_CUR;
	return fseek(r->f, (long) off, w);
}
static inline int64_t filestream_tell(RFILE *r) { return ftell(r->f); }
static inline int64_t filestream_get_size(RFILE *r)
{
	long cur = ftell(r->f);
	fseek(r->f, 0, SEEK_END);
	long n = ftell(r->f);
	fseek(r->f, cur, SEEK_SET);
	return n;
}
static inline int filestream_close(RFILE *r) { if (!r) return 0; fclose(r->f); free(r); return 0; }
#endif
