/* See pk_patch.h. Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE). */
#include "pk_patch.h"
#include "pk_util.h"
#include <string.h>

namespace pk {

std::string PatchResult::text() const
{
	switch (status) {
	case PS_OK:
		return verified ? "Patch applied (checksums match)" : "Patch applied (this kind of patch cannot check the ROM)";
	case PS_BAD_FORMAT: return "The patch file is not valid";
	case PS_TRUNCATED: return "The patch file is cut short";
	case PS_SOURCE_MISMATCH: return "This patch was made for a different ROM";
	case PS_TARGET_MISMATCH: return "The patched ROM does not match the patch's checksum";
	case PS_PATCH_DAMAGED: return "The patch file is damaged";
	default: return "The patched ROM would be too large";
	}
}

const char *patch_kind_name(PatchKind k)
{
	return k == PATCH_IPS ? "IPS" : k == PATCH_BPS ? "BPS" : k == PATCH_UPS ? "UPS" : "none";
}

PatchKind patch_detect(const uint8_t *p, size_t n)
{
	if (n >= 8 && !memcmp(p, "PATCH", 5)) return PATCH_IPS;
	if (n >= 9 && !memcmp(p, "IPS32", 5)) return PATCH_IPS;
	if (n >= 4 && !memcmp(p, "BPS1", 4)) return PATCH_BPS;
	if (n >= 4 && !memcmp(p, "UPS1", 4)) return PATCH_UPS;
	return PATCH_NONE;
}

PatchKind patch_kind_from_name(const std::string &name)
{
	if (ends_with_ci(name, ".ips")) return PATCH_IPS;
	if (ends_with_ci(name, ".bps")) return PATCH_BPS;
	if (ends_with_ci(name, ".ups")) return PATCH_UPS;
	return PATCH_NONE;
}

namespace {

/* a bounds-checked reader over the patch */
struct Rd {
	const uint8_t *p;
	size_t n, at;
	bool bad;
	Rd(const uint8_t *pp, size_t nn, size_t a = 0) : p(pp), n(nn), at(a), bad(false) {}
	size_t left() const { return at <= n ? n - at : 0; }
	uint32_t u8() { if (at >= n) { bad = true; return 0; } return p[at++]; }
	uint32_t be(int bytes) { uint32_t v = 0; while (bytes--) v = (v << 8) | u8(); return v; }
	uint32_t le32() { uint32_t v = u8(); v |= u8() << 8; v |= u8() << 16; v |= (uint32_t) u8() << 24; return v; }
	/* the BPS/UPS variable-length number; overflow marks the reader bad */
	uint64_t varint()
	{
		uint64_t data = 0, shift = 1;
		for (;;) {
			uint32_t x = u8();
			if (bad) return 0;
			data += (uint64_t) (x & 0x7F) * shift;
			if (x & 0x80) break;
			shift <<= 7;
			if (shift > ((uint64_t) 1 << 56)) { bad = true; return 0; }
			data += shift;
		}
		return data;
	}
};

PatchStatus apply_ips(const uint8_t *patch, size_t pn, const std::vector<uint8_t> &src, std::vector<uint8_t> &out, size_t max_out)
{
	bool big = pn >= 5 && !memcmp(patch, "IPS32", 5);
	Rd r(patch, pn, big ? 5 : 5);
	out = src;
	size_t trunc_to = (size_t) -1;
	for (;;) {
		if (r.left() == 0) return PS_TRUNCATED;     /* the EOF marker is missing */
		/* "EOF" / "EEOF" where an offset would be */
		if (!big && r.left() >= 3 && !memcmp(patch + r.at, "EOF", 3)) { r.at += 3; break; }
		if (big && r.left() >= 4 && !memcmp(patch + r.at, "EEOF", 4)) { r.at += 4; break; }
		uint64_t off = big ? r.be(4) : r.be(3);
		uint32_t size = r.be(2);
		if (r.bad) return PS_TRUNCATED;
		if (size == 0) {
			uint32_t run = r.be(2), val = r.u8();
			if (r.bad) return PS_TRUNCATED;
			if (off + run > max_out) return PS_TOO_BIG;
			if (out.size() < off + run) out.resize((size_t) (off + run), 0);
			memset(&out[(size_t) off], (int) val, run);
		} else {
			if (r.left() < size) return PS_TRUNCATED;
			if (off + size > max_out) return PS_TOO_BIG;
			if (out.size() < off + size) out.resize((size_t) (off + size), 0);
			memcpy(&out[(size_t) off], patch + r.at, size);
			r.at += size;
		}
	}
	/* an optional truncation length after the EOF marker */
	size_t need = big ? 4 : 3;
	if (r.left() == need) trunc_to = r.be(need);
	else if (r.left() != 0) return PS_BAD_FORMAT;
	if (trunc_to != (size_t) -1) {
		if (trunc_to > max_out) return PS_TOO_BIG;
		out.resize(trunc_to, 0);
	}
	return PS_OK;
}

PatchStatus apply_bps(const uint8_t *patch, size_t pn, const std::vector<uint8_t> &src, std::vector<uint8_t> &out, size_t max_out, bool *crc_src_mismatch)
{
	if (pn < 4 + 3 + 12) return PS_BAD_FORMAT;
	Rd foot(patch, pn, pn - 12);
	uint32_t src_crc = foot.le32(), dst_crc = foot.le32(), pat_crc = foot.le32();
	if (crc32(0, patch, pn - 4) != pat_crc) return PS_PATCH_DAMAGED;
	Rd r(patch, pn - 12, 4);
	uint64_t ssize = r.varint(), tsize = r.varint(), meta = r.varint();
	if (r.bad || meta > r.left()) return PS_BAD_FORMAT;
	r.at += (size_t) meta;
	if (tsize > max_out) return PS_TOO_BIG;
	if (ssize != src.size() || crc32(0, src.data(), src.size()) != src_crc) {
		if (crc_src_mismatch) *crc_src_mismatch = true;
		return PS_SOURCE_MISMATCH;
	}
	std::vector<uint8_t> t((size_t) tsize);
	size_t o = 0;
	int64_t src_rel = 0, dst_rel = 0;
	while (r.left() > 0) {
		uint64_t data = r.varint();
		if (r.bad) return PS_TRUNCATED;
		uint64_t len = (data >> 2) + 1;
		int cmd = (int) (data & 3);
		if (len > tsize - o) return PS_BAD_FORMAT;
		switch (cmd) {
		case 0:   /* SourceRead: the source at the same position */
			if (o + len > src.size()) return PS_BAD_FORMAT;
			memcpy(&t[o], &src[o], (size_t) len);
			o += (size_t) len;
			break;
		case 1:   /* TargetRead: literal bytes */
			if (r.left() < len) return PS_TRUNCATED;
			memcpy(&t[o], patch + r.at, (size_t) len);
			r.at += (size_t) len;
			o += (size_t) len;
			break;
		case 2:
		case 3: {
			uint64_t d = r.varint();
			if (r.bad) return PS_TRUNCATED;
			int64_t delta = (d & 1) ? -(int64_t) (d >> 1) : (int64_t) (d >> 1);
			if (cmd == 2) {
				src_rel += delta;
				if (src_rel < 0 || (uint64_t) src_rel + len > src.size()) return PS_BAD_FORMAT;
				memcpy(&t[o], &src[(size_t) src_rel], (size_t) len);
				src_rel += (int64_t) len;
				o += (size_t) len;
			} else {
				dst_rel += delta;
				if (dst_rel < 0 || (uint64_t) dst_rel >= o) return PS_BAD_FORMAT;   /* must read bytes already made */
				for (uint64_t k = 0; k < len; k++) t[o++] = t[(size_t) dst_rel++];   /* may overlap itself */
			}
			break;
		}
		}
	}
	if (o != tsize) return PS_BAD_FORMAT;
	if (crc32(0, t.data(), t.size()) != dst_crc) return PS_TARGET_MISMATCH;
	out.swap(t);
	return PS_OK;
}

PatchStatus apply_ups(const uint8_t *patch, size_t pn, const std::vector<uint8_t> &src, std::vector<uint8_t> &out, size_t max_out, bool *crc_src_mismatch)
{
	if (pn < 4 + 2 + 12) return PS_BAD_FORMAT;
	Rd foot(patch, pn, pn - 12);
	uint32_t in_crc = foot.le32(), out_crc = foot.le32(), pat_crc = foot.le32();
	if (crc32(0, patch, pn - 4) != pat_crc) return PS_PATCH_DAMAGED;
	Rd r(patch, pn - 12, 4);
	uint64_t in_size = r.varint(), out_size = r.varint();
	if (r.bad) return PS_BAD_FORMAT;
	/* a UPS patch works both ways: the ROM it makes is also accepted as the source */
	uint32_t src_crc = crc32(0, src.data(), src.size());
	uint64_t want_size;
	uint32_t want_crc;
	if (src.size() == in_size && src_crc == in_crc) { want_size = out_size; want_crc = out_crc; }
	else if (src.size() == out_size && src_crc == out_crc) { want_size = in_size; want_crc = in_crc; }
	else {
		if (crc_src_mismatch) *crc_src_mismatch = true;
		return PS_SOURCE_MISMATCH;
	}
	if (want_size > max_out) return PS_TOO_BIG;
	std::vector<uint8_t> t(src);
	t.resize((size_t) want_size, 0);
	size_t pos = 0;
	while (r.left() > 0) {
		uint64_t skip = r.varint();
		if (r.bad) return PS_TRUNCATED;
		if (skip > ((uint64_t) 1 << 32)) return PS_BAD_FORMAT;
		pos += (size_t) skip;
		for (;;) {
			uint32_t x = r.u8();
			if (r.bad) return PS_TRUNCATED;
			if (pos < want_size) t[pos] ^= (uint8_t) x;
			else if (x) return PS_BAD_FORMAT;
			pos++;
			if (x == 0) break;
		}
	}
	if (crc32(0, t.data(), t.size()) != want_crc) return PS_TARGET_MISMATCH;
	out.swap(t);
	return PS_OK;
}

}  // namespace

PatchResult patch_apply(const uint8_t *patch, size_t pn, const std::vector<uint8_t> &source, std::vector<uint8_t> &out, size_t max_output)
{
	PatchResult res;
	res.kind = patch_detect(patch, pn);
	if (res.kind == PATCH_NONE) { res.status = PS_BAD_FORMAT; return res; }
	std::vector<uint8_t> tmp;
	bool src_mismatch = false;
	PatchStatus st = PS_BAD_FORMAT;
	switch (res.kind) {
	case PATCH_IPS: st = apply_ips(patch, pn, source, tmp, max_output); break;
	case PATCH_BPS: st = apply_bps(patch, pn, source, tmp, max_output, &src_mismatch); break;
	case PATCH_UPS: st = apply_ups(patch, pn, source, tmp, max_output, &src_mismatch); break;
	default: break;
	}
	/* A patch made for a ROM without its 512-byte copier header (or the reverse): try the other. */
	if (st == PS_SOURCE_MISMATCH && src_mismatch && source.size() % 1024 == 512 && source.size() > 512) {
		std::vector<uint8_t> bare(source.begin() + 512, source.end()), t2;
		PatchStatus st2 = res.kind == PATCH_BPS ? apply_bps(patch, pn, bare, t2, max_output, 0)
		                                         : apply_ups(patch, pn, bare, t2, max_output, 0);
		if (st2 == PS_OK) {
			tmp.assign(source.begin(), source.begin() + 512);
			tmp.insert(tmp.end(), t2.begin(), t2.end());
			st = PS_OK;
			res.header_handled = true;
		}
	}
	res.status = st;
	if (st == PS_OK) {
		res.verified = res.kind != PATCH_IPS;
		out.swap(tmp);
	}
	return res;
}

PatchResult patch_apply_files(const std::string &patch_path, const std::vector<uint8_t> &source, std::vector<uint8_t> &out)
{
	std::vector<uint8_t> p;
	PatchResult res;
	if (!file_read(patch_path, p, 32u << 20)) { res.status = PS_TRUNCATED; return res; }
	return patch_apply(p.data(), p.size(), source, out);
}

}  // namespace pk
