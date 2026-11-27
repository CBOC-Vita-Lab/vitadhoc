/* Stand-in for libretro-common string/stdstring.h: only what the core calls.
 * Copyright CBOC-Vita. MIT licence (see nes/frontend/LICENSE). */
#ifndef NES_COMPAT_STDSTRING_H
#define NES_COMPAT_STDSTRING_H
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <stdbool.h>

static inline bool string_is_empty(const char *s) { return !s || !*s; }
static inline bool string_is_equal_noncase(const char *a, const char *b) { return !strcasecmp(a, b); }
/* trims in place; the returned pointer is the first non-space character */
static inline char *string_trim_whitespace(char *const s)
{
	char *p = s;
	while (*p && isspace((unsigned char) *p)) p++;
	size_t n = strlen(p);
	while (n && isspace((unsigned char) p[n - 1])) p[--n] = 0;
	return p;
}
#endif
