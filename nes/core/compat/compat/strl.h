/* Header-only stand-ins for the few libretro-common helpers the FCEUmm core uses
 * (strlcpy / strlcat). Copyright CBOC-Vita. MIT licence (see nes/frontend/LICENSE). */
#ifndef NES_COMPAT_STRL_H
#define NES_COMPAT_STRL_H
#include <string.h>
#include <stddef.h>

#define strlcpy nes_strlcpy
#define strlcat nes_strlcat

static inline size_t nes_strlcpy(char *dst, const char *src, size_t size)
{
	size_t n = strlen(src);
	if (size) {
		size_t c = n >= size ? size - 1 : n;
		memcpy(dst, src, c);
		dst[c] = 0;
	}
	return n;
}

static inline size_t nes_strlcat(char *dst, const char *src, size_t size)
{
	size_t d = strnlen(dst, size);
	if (d == size) return size + strlen(src);
	return d + nes_strlcpy(dst + d, src, size - d);
}
#endif
