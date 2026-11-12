/*
 * ROM patches (IPS, IPS32, BPS, UPS) applied to a buffer in memory, written
 * from the public format descriptions. BPS and UPS carry checksums, which are
 * verified; IPS carries none, so a successful IPS result says so.
 *
 * Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE).
 */
#ifndef PK_PATCH_H
#define PK_PATCH_H

#include <stddef.h>
#include <stdint.h>
#include <string>
#include <vector>

namespace pk {

enum PatchKind { PATCH_NONE = 0, PATCH_IPS, PATCH_BPS, PATCH_UPS };

enum PatchStatus {
	PS_OK = 0,
	PS_BAD_FORMAT,        /* not a patch of the kind it claims, or a field is out of range */
	PS_TRUNCATED,         /* the patch ends in the middle of a record */
	PS_SOURCE_MISMATCH,   /* BPS/UPS: this is not the ROM the patch was made for (checksum or size) */
	PS_TARGET_MISMATCH,   /* BPS/UPS: the result does not match the checksum in the patch */
	PS_PATCH_DAMAGED,     /* BPS/UPS: the patch file's own checksum is wrong */
	PS_TOO_BIG            /* the result would be larger than the limit */
};

struct PatchResult {
	PatchStatus status = PS_BAD_FORMAT;
	PatchKind kind = PATCH_NONE;
	bool verified = false;      /* true when a checksum of the source and result matched */
	bool header_handled = false;/* a 512-byte copier header was set aside and put back */
	std::string text() const;   /* a line for the player */
};

PatchKind patch_detect(const uint8_t *patch, size_t n);
const char *patch_kind_name(PatchKind k);
/* "Game.ips" -> PATCH_IPS by extension (case-insensitive); PATCH_NONE if it is none of the three. */
PatchKind patch_kind_from_name(const std::string &file_name);

/* Applies `patch` to `source`; the patched ROM goes to `out` (untouched on failure). */
PatchResult patch_apply(const uint8_t *patch, size_t patch_size, const std::vector<uint8_t> &source,
                        std::vector<uint8_t> &out, size_t max_output = 64u << 20);

/* Reads the patch file and the ROM, applies, and returns the result. */
PatchResult patch_apply_files(const std::string &patch_path, const std::vector<uint8_t> &source,
                              std::vector<uint8_t> &out);

}  // namespace pk

#endif
