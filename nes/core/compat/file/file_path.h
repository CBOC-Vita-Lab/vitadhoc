/* Stand-in for libretro-common file/file_path.h: only what the core calls.
 * Copyright CBOC-Vita. MIT licence (see nes/frontend/LICENSE). */
#ifndef NES_COMPAT_FILE_PATH_H
#define NES_COMPAT_FILE_PATH_H
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include <stdbool.h>
#include <sys/stat.h>

static inline bool path_is_valid(const char *path)
{
	struct stat st;
	return path && *path && stat(path, &st) == 0;
}

/* out = dir + "/" + name (just name when dir is empty) */
static inline size_t fill_pathname_join(char *out, const char *dir, const char *name, size_t size)
{
	if (!size) return 0;
	size_t dl = dir ? strlen(dir) : 0;
	if (dl && (dir[dl - 1] == '/' || dir[dl - 1] == ':'))
		snprintf(out, size, "%s%s", dir, name);
	else if (dl)
		snprintf(out, size, "%s/%s", dir, name);
	else
		snprintf(out, size, "%s", name);
	return strlen(out);
}
#endif
