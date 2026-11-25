/* SPDX-License-Identifier: MIT OR GPL-2.0-or-later
 * Copyright (c) 2026 VitAdhoc contributors */
/* update_ver.h -- the update check's pure parts: version numbers, and the one
 * answer of GitHub's releases API it cares about.  No I/O; the tests
 * (tests/update) link nothing else. */
#ifndef GBADHOC_UPDATE_VER_H
#define GBADHOC_UPDATE_VER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A version: 1 to 4 numbers separated by dots, each 1 to 5 digits.  Words
 * ahead of the first digit are the tag's name for it and are dropped ("v2.1",
 * "VitAdhoc-2.1", "release 2.1"); anything after the numbers is not a
 * release (2.1-rc1, 2.1b, 2.1.x) and is refused.  Missing numbers are 0, so
 * 2.0 is 2.0.0.0.  1 = parsed (v[4] and, if `shown` is not NULL, the numbers
 * as written: "2.1"); 0 = not a version. */
int upd_ver_parse(const char *s, unsigned v[4], char *shown, size_t cap);
/* <0, 0, >0 */
int upd_ver_cmp(const unsigned a[4], const unsigned b[4]);
/* 1 when `tag` is a version newer than `mine` (both parsed as above); 0 for
 * same, older, or either not a version. */
int upd_ver_newer(const char *tag, const char *mine);

/* What the releases API said (GET /repos/<owner>/<repo>/releases/latest):
 * the release's tag as a version to show ("2.1"), when the answer is one
 * release, not a draft and not a pre-release, and its tag_name is a version.
 * Reads the top-level object only (the answer holds nested objects, and
 * strings that look like keys).  1 = *shown filled; 0 = not usable (not
 * JSON, no tag, a draft, a pre-release, a tag that is not a version, too
 * long). */
int upd_release_parse(const char *json, size_t len, char *shown, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* GBADHOC_UPDATE_VER_H */
