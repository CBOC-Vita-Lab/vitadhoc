/* Unit tests: pk_patch, with patches generated here from the public format descriptions. Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE). */
#include "pk_test.h"
#include "pk_patch.h"
#include "pk_util.h"
#include <string.h>

using namespace pk;

static std::vector<uint8_t> rom(size_t n, int seed = 1)
{
	std::vector<uint8_t> v(n);
	uint32_t x = (uint32_t) seed * 2654435761u + 1;
	for (size_t i = 0; i < n; i++) { x = x * 1664525u + 1013904223u; v[i] = (uint8_t) (x >> 24); }
	return v;
}
static void put(std::vector<uint8_t> &v, const char *s) { while (*s) v.push_back((uint8_t) *s++); }
static void be24(std::vector<uint8_t> &v, uint32_t x) { v.push_back((uint8_t) (x >> 16)); v.push_back((uint8_t) (x >> 8)); v.push_back((uint8_t) x); }
static void be16(std::vector<uint8_t> &v, uint32_t x) { v.push_back((uint8_t) (x >> 8)); v.push_back((uint8_t) x); }
static void le32(std::vector<uint8_t> &v, uint32_t x) { for (int i = 0; i < 4; i++) v.push_back((uint8_t) (x >> (8 * i))); }

/* ---- IPS ---- */
static std::vector<uint8_t> ips(bool rle, bool trunc_len)
{
	std::vector<uint8_t> p;
	put(p, "PATCH");
	be24(p, 0x10); be16(p, 4); p.push_back(1); p.push_back(2); p.push_back(3); p.push_back(4);
	if (rle) { be24(p, 0x100); be16(p, 0); be16(p, 50); p.push_back(0xEE); }
	if (trunc_len) { be24(p, 0x300); be16(p, 5); p.push_back(1); }   /* ends mid record */
	else put(p, "EOF");
	return p;
}

TEST(patch_ips_applies_literal_and_rle)
{
	std::vector<uint8_t> src = rom(1024), out;
	std::vector<uint8_t> p = ips(true, false);
	PatchResult r = patch_apply(&p[0], p.size(), src, out);
	CHECK_EQ(r.status, PS_OK);
	CHECK_EQ(r.kind, PATCH_IPS);
	CHECK(!r.verified);                         /* IPS has no checksum */
	CHECK(out.size() == src.size());
	CHECK(out[0x10] == 1 && out[0x13] == 4 && out[0x100] == 0xEE && out[0x100 + 49] == 0xEE);
	CHECK(out[0x14] == src[0x14] && out[0x100 + 50] == src[0x100 + 50]);
}

TEST(patch_ips_grows_the_rom)
{
	std::vector<uint8_t> src = rom(64), out, p;
	put(p, "PATCH"); be24(p, 100); be16(p, 2); p.push_back(9); p.push_back(8); put(p, "EOF");
	PatchResult r = patch_apply(&p[0], p.size(), src, out);
	CHECK_EQ(r.status, PS_OK);
	CHECK_EQ(out.size(), 102);
	CHECK(out[100] == 9 && out[101] == 8 && out[64] == 0);
}

TEST(patch_ips_truncated_and_bad)
{
	std::vector<uint8_t> src = rom(1024), out;
	std::vector<uint8_t> p = ips(false, true);
	CHECK_EQ(patch_apply(&p[0], p.size(), src, out).status, PS_TRUNCATED);
	CHECK(out.empty());
	uint8_t junk[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
	CHECK_EQ(patch_apply(junk, 8, src, out).status, PS_BAD_FORMAT);
	CHECK_EQ(patch_apply(junk, 0, src, out).status, PS_BAD_FORMAT);
	std::vector<uint8_t> big = ips(true, false);
	CHECK_EQ(patch_apply(&big[0], big.size(), src, out, 100).status, PS_TOO_BIG);
}

TEST(patch_ips_copier_header_retry)
{
	/* the ROM carries a 512-byte copier header the patch's offsets do not count */
	std::vector<uint8_t> base = rom(32768, 3), hdr(512, 0), src, out, p;
	src = hdr; src.insert(src.end(), base.begin(), base.end());
	put(p, "PATCH"); be24(p, 0x20); be16(p, 2); p.push_back(0xAB); p.push_back(0xCD); put(p, "EOF");
	PatchResult r = patch_apply(&p[0], p.size(), src, out);
	CHECK_EQ(r.status, PS_OK);
	CHECK(out.size() == src.size());
	CHECK(out[0x20] == 0xAB || out[512 + 0x20] == 0xAB);
}

/* ---- BPS ---- */
static void vlq(std::vector<uint8_t> &v, uint64_t x)
{
	for (;;) {
		uint8_t b = x & 0x7F; x >>= 7;
		if (x == 0) { v.push_back(b | 0x80); return; }
		v.push_back(b); x--;
	}
}

static std::vector<uint8_t> bps_make(const std::vector<uint8_t> &src, const std::vector<uint8_t> &tgt, bool break_src, bool break_tgt, bool break_patch)
{
	std::vector<uint8_t> p;
	put(p, "BPS1");
	vlq(p, src.size()); vlq(p, tgt.size()); vlq(p, 0);
	/* SourceRead everything that matches, then TargetRead the rest */
	size_t i = 0;
	while (i < tgt.size()) {
		size_t j = i;
		while (j < tgt.size() && j < src.size() && tgt[j] == src[j]) j++;
		if (j > i) { vlq(p, ((j - i) - 1) << 2 | 0); i = j; continue; }
		size_t k = i;
		while (k < tgt.size() && !(k < src.size() && tgt[k] == src[k])) k++;
		vlq(p, ((k - i) - 1) << 2 | 1);
		for (size_t q = i; q < k; q++) p.push_back(tgt[q]);
		i = k;
	}
	le32(p, crc32(0, &src[0], src.size()) ^ (break_src ? 1u : 0));
	le32(p, crc32(0, &tgt[0], tgt.size()) ^ (break_tgt ? 1u : 0));
	le32(p, crc32(0, &p[0], p.size()) ^ (break_patch ? 1u : 0));
	return p;
}

TEST(patch_bps_applies_and_verifies)
{
	std::vector<uint8_t> src = rom(4096, 5), tgt = src, out;
	for (int i = 100; i < 140; i++) tgt[(size_t) i] ^= 0x5A;
	tgt.push_back(1); tgt.push_back(2);
	std::vector<uint8_t> p = bps_make(src, tgt, false, false, false);
	PatchResult r = patch_apply(&p[0], p.size(), src, out);
	CHECK_EQ(r.status, PS_OK);
	CHECK_EQ(r.kind, PATCH_BPS);
	CHECK(r.verified);
	CHECK(out == tgt);
}

TEST(patch_bps_checksum_failures)
{
	std::vector<uint8_t> src = rom(2048, 6), tgt = src, out;
	tgt[500] ^= 1;
	std::vector<uint8_t> p = bps_make(src, tgt, true, false, false);
	CHECK_EQ(patch_apply(&p[0], p.size(), src, out).status, PS_SOURCE_MISMATCH);
	p = bps_make(src, tgt, false, true, false);
	CHECK_EQ(patch_apply(&p[0], p.size(), src, out).status, PS_TARGET_MISMATCH);
	p = bps_make(src, tgt, false, false, true);
	CHECK_EQ(patch_apply(&p[0], p.size(), src, out).status, PS_PATCH_DAMAGED);
	CHECK(out.empty());
	/* the wrong ROM entirely */
	std::vector<uint8_t> other = rom(2048, 99);
	p = bps_make(src, tgt, false, false, false);
	CHECK_EQ(patch_apply(&p[0], p.size(), other, out).status, PS_SOURCE_MISMATCH);
}

TEST(patch_bps_target_copy_runs)
{
	/* TargetCopy (action 3) repeats earlier output: a run of one byte made with an overlapping copy */
	std::vector<uint8_t> src = rom(16, 2), tgt(16 + 20, 0), out, p;
	for (size_t i = 0; i < 16; i++) tgt[i] = src[i];
	for (size_t i = 16; i < tgt.size(); i++) tgt[i] = tgt[15];
	put(p, "BPS1");
	vlq(p, src.size()); vlq(p, tgt.size()); vlq(p, 0);
	vlq(p, (16 - 1) << 2 | 0);                     /* SourceRead 16 */
	vlq(p, (20 - 1) << 2 | 3);                     /* TargetCopy 20 */
	/* offset: relative to the previous copy offset (which starts at 0), signed, (abs << 1) | sign: +15 */
	vlq(p, 15u << 1);
	le32(p, crc32(0, &src[0], src.size()));
	le32(p, crc32(0, &tgt[0], tgt.size()));
	le32(p, crc32(0, &p[0], p.size()));
	PatchResult r = patch_apply(&p[0], p.size(), src, out);
	CHECK_EQ(r.status, PS_OK);
	CHECK(out == tgt);
}

TEST(patch_bps_truncated)
{
	std::vector<uint8_t> src = rom(512, 8), tgt = src, out;
	tgt[10] ^= 7;
	std::vector<uint8_t> p = bps_make(src, tgt, false, false, false);
	p.resize(p.size() - 20);
	PatchResult r = patch_apply(&p[0], p.size(), src, out);
	CHECK(r.status != PS_OK);
	CHECK(out.empty());
}

/* ---- UPS ---- */
static std::vector<uint8_t> ups_make(const std::vector<uint8_t> &src, const std::vector<uint8_t> &tgt, bool break_src)
{
	std::vector<uint8_t> p;
	put(p, "UPS1");
	auto uvar = [&](uint64_t x) { for (;;) { uint8_t b = x & 0x7F; x >>= 7; if (x == 0) { p.push_back(b | 0x80); return; } p.push_back(b); x--; } };
	uvar(src.size()); uvar(tgt.size());
	size_t n = src.size() > tgt.size() ? src.size() : tgt.size();
	size_t i = 0, last = 0;
	while (i < n) {
		uint8_t a = i < src.size() ? src[i] : 0, b = i < tgt.size() ? tgt[i] : 0;
		if (a == b) { i++; continue; }
		uvar(i - last);
		while (i < n) {
			a = i < src.size() ? src[i] : 0; b = i < tgt.size() ? tgt[i] : 0;
			p.push_back(a ^ b);
			i++;
			if (a == b) break;
		}
		if (a != b) p.push_back(0);      /* the block must end with a zero byte */
		last = i;
	}
	le32(p, crc32(0, &src[0], src.size()) ^ (break_src ? 1u : 0));
	le32(p, crc32(0, &tgt[0], tgt.size()));
	le32(p, crc32(0, &p[0], p.size()));
	return p;
}

TEST(patch_ups_applies_and_verifies)
{
	std::vector<uint8_t> src = rom(3000, 11), tgt = src, out;
	for (int i = 200; i < 230; i++) tgt[(size_t) i] = (uint8_t) (tgt[(size_t) i] + 1);
	tgt[2999] ^= 0x80;
	std::vector<uint8_t> p = ups_make(src, tgt, false);
	PatchResult r = patch_apply(&p[0], p.size(), src, out);
	CHECK_EQ(r.status, PS_OK);
	CHECK_EQ(r.kind, PATCH_UPS);
	CHECK(r.verified);
	CHECK(out == tgt);
	/* UPS works both ways: applying to the patched ROM gives the original back */
	std::vector<uint8_t> back;
	r = patch_apply(&p[0], p.size(), tgt, back);
	CHECK_EQ(r.status, PS_OK);
	CHECK(back == src);
}

TEST(patch_ups_wrong_rom)
{
	std::vector<uint8_t> src = rom(1000, 12), tgt = src, out;
	tgt[5] ^= 1;
	std::vector<uint8_t> p = ups_make(src, tgt, false);
	std::vector<uint8_t> other = rom(1000, 13);
	CHECK_EQ(patch_apply(&p[0], p.size(), other, out).status, PS_SOURCE_MISMATCH);
}

TEST(patch_detect_and_names)
{
	std::vector<uint8_t> a; put(a, "PATCH"); put(a, "EOF");
	CHECK_EQ(patch_detect(&a[0], a.size()), PATCH_IPS);
	std::vector<uint8_t> b; put(b, "BPS1xxxxxxxxxxxx");
	CHECK_EQ(patch_detect(&b[0], b.size()), PATCH_BPS);
	std::vector<uint8_t> c; put(c, "UPS1xxxxxxxxxxxx");
	CHECK_EQ(patch_detect(&c[0], c.size()), PATCH_UPS);
	CHECK_EQ(patch_detect((const uint8_t *) "zz", 2), PATCH_NONE);
	CHECK_EQ(patch_kind_from_name("Hack.IPS"), PATCH_IPS);
	CHECK_EQ(patch_kind_from_name("x.bps"), PATCH_BPS);
	CHECK_EQ(patch_kind_from_name("x.ups"), PATCH_UPS);
	CHECK_EQ(patch_kind_from_name("x.zip"), PATCH_NONE);
	CHECK(patch_kind_name(PATCH_BPS)[0] != 0);
	PatchResult r; CHECK(!r.text().empty());
}
