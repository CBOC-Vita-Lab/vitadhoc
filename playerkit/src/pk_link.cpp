/* See pk_link.h. Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE). */
#include "pk_link.h"
#include "pk_util.h"
#include "pk_i18n.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <algorithm>

namespace pk {

/* ====================================================================================================================
 * The state's compression: a plain LZ77 byte format. A sequence is a token (high nibble: literal count, low nibble:
 * match length - 4; 15 means "more bytes follow", each adding up to 255, the last below 255), the literals, and, unless
 * the output is complete, a 16-bit offset (1..65535) and the extra match length. The last sequence has literals only.
 * ================================================================================================================== */

static void lz_ext(std::vector<uint8_t> &o, size_t v)      /* v >= 15: the bytes after the nibble */
{
	v -= 15;
	while (v >= 255) { o.push_back(255); v -= 255; }
	o.push_back((uint8_t) v);
}

bool link_lz_compress(const uint8_t *src, size_t n, std::vector<uint8_t> &out)
{
	out.clear();
	if (!src || !n) return false;
	out.reserve(n / 2 + 64);
	enum { HBITS = 13 };
	std::vector<uint32_t> ht((size_t) 1 << HBITS, 0);       /* position + 1 */
	size_t i = 0, anchor = 0;
	auto emit = [&](size_t lit_end, size_t mlen, size_t off) {
		const size_t lit = lit_end - anchor;
		const size_t ml = mlen ? mlen - 4 : 0;
		out.push_back((uint8_t) ((lit >= 15 ? 15 : lit) << 4 | (mlen ? (ml >= 15 ? 15 : ml) : 0)));
		if (lit >= 15) lz_ext(out, lit);
		out.insert(out.end(), src + anchor, src + lit_end);
		if (mlen) {
			out.push_back((uint8_t) (off & 0xFF));
			out.push_back((uint8_t) (off >> 8));
			if (ml >= 15) lz_ext(out, ml);
		}
	};
	while (i + 4 <= n) {
		uint32_t w;
		memcpy(&w, src + i, 4);
		const uint32_t h = (w * 2654435761u) >> (32 - HBITS);
		const size_t cand = ht[h];
		ht[h] = (uint32_t) (i + 1);
		if (cand && i - (cand - 1) <= 65535 && memcmp(src + cand - 1, src + i, 4) == 0) {
			const size_t from = cand - 1;
			size_t len = 4;
			while (i + len < n && src[from + len] == src[i + len]) len++;
			emit(i, len, i - from);
			i += len;
			anchor = i;
			/* re-seed the table inside the match cheaply (the end of it) */
			if (i + 4 <= n && len > 4) {
				memcpy(&w, src + i - 2, 4);
				ht[(w * 2654435761u) >> (32 - HBITS)] = (uint32_t) (i - 2 + 1);
			}
		} else {
			i++;
		}
	}
	if (anchor < n) emit(n, 0, 0);
	return true;
}

bool link_lz_decompress(const uint8_t *src, size_t n, uint8_t *out, size_t out_len)
{
	if (!src || !out || !out_len) return false;
	size_t si = 0, di = 0;
	while (di < out_len) {
		if (si >= n) return false;
		const uint8_t tok = src[si++];
		size_t lit = tok >> 4, ml = tok & 15;
		if (lit == 15) {
			for (;;) {
				if (si >= n) return false;
				const uint8_t b = src[si++];
				lit += b;
				if (lit > out_len) return false;
				if (b != 255) break;
			}
		}
		if (lit > n - si || lit > out_len - di) return false;
		memcpy(out + di, src + si, lit);
		si += lit;
		di += lit;
		if (di == out_len) break;
		if (n - si < 2) return false;
		const size_t off = (size_t) src[si] | ((size_t) src[si + 1] << 8);
		si += 2;
		if (ml == 15) {
			for (;;) {
				if (si >= n) return false;
				const uint8_t b = src[si++];
				ml += b;
				if (ml > out_len) return false;
				if (b != 255) break;
			}
		}
		ml += 4;
		if (off == 0 || off > di || ml > out_len - di) return false;
		for (size_t k = 0; k < ml; k++) out[di + k] = out[di + k - off];
		di += ml;
	}
	return si == n;
}

/* ====================================================================================================================
 * Wire format
 * ================================================================================================================== */

namespace {

const uint8_t kMagic[4] = { 'P', 'K', 'L', '1' };
enum {
	HDR = 12, TAIL = 4, MAX_DGRAM = 1400, CHUNK = 1000, MAX_INPUTS = 64, RING = 256, HORIZON = 192, PROTO_VER = 1,
	WINDOW = 32, MAX_BURST = 16, HASHES = 8
};
const uint32_t NO_END = 0xFFFFFFFFu;

enum Type { T_HELLO = 1, T_WELCOME, T_BYE, T_CHUNK, T_CACK, T_READY, T_START, T_INPUT, T_DONE };

struct Wr {
	uint8_t *p;
	size_t n, cap;
	Wr(uint8_t *b, size_t c) : p(b), n(0), cap(c) {}
	void u8(unsigned v) { if (n < cap) p[n++] = (uint8_t) v; }
	void u16(unsigned v) { u8(v & 0xFF); u8((v >> 8) & 0xFF); }
	void u32(uint32_t v) { u16(v & 0xFFFF); u16(v >> 16); }
	void u64(uint64_t v) { u32((uint32_t) v); u32((uint32_t) (v >> 32)); }
	void bytes(const void *b, size_t k) { if (k <= cap - n) { memcpy(p + n, b, k); n += k; } }
};

struct Rd {
	const uint8_t *p;
	size_t n, i;
	bool ok;
	Rd(const uint8_t *b, size_t c) : p(b), n(c), i(0), ok(true) {}
	unsigned u8() { if (i >= n) { ok = false; return 0; } return p[i++]; }
	unsigned u16() { unsigned a = u8(); return a | (u8() << 8); }
	uint32_t u32() { uint32_t a = u16(); return a | ((uint32_t) u16() << 16); }
	uint64_t u64() { uint64_t a = u32(); return a | ((uint64_t) u32() << 32); }
	size_t left() const { return n - i; }
};

uint32_t le32(const uint8_t *p) { return (uint32_t) p[0] | (uint32_t) p[1] << 8 | (uint32_t) p[2] << 16 | (uint32_t) p[3] << 24; }

struct HashRec {
	uint32_t frame, hash;
	bool used;
};

const char *kErrNames[LE_COUNT] = { "ok", "proto", "rom_missing", "rom_bad", "memory", "start", "desync", "peer_lost",
                                    "peer_abort", "commit", "local", "no_peer", "suspend", "state" };
const char *kErrText[LE_COUNT] = {
	"link.the_link_is_fine",
	"link.the_other_vita_runs_a_different",
	"link.the_other_vita_plays_a_different",
	"link.the_other_vita_s_game_did",
	"link.there_was_not_enough_memory_to",
	"link.the_game_could_not_be_started",
	"link.the_two_games_fell_out_of",
	"link.link_lost_the_other_vita_is",
	"link.the_other_vita_ended_the_link",
	"link.the_game_could_not_be_saved",
	"link.the_link_was_cancelled",
	"link.nobody_answered",
	"link.this_vita_went_to_sleep_so",
	"link.the_game_did_not_arrive_intact"
};
const char *kPhaseName[] = { "idle", "wait", "state", "start", "run", "ending", "done", "failed" };

}  // namespace

const char *link_error_name(int e) { return e >= 0 && e < LE_COUNT ? kErrNames[e] : "other"; }
const char *link_error_text(int e) { return e >= 0 && e < LE_COUNT ? tr(kErrText[e]) : tr("link.the_link_failed"); }

/* ====================================================================================================================
 * The session
 * ================================================================================================================== */

struct Link::Impl {
	LinkConfig cfg;
	LinkMachine m;
	LinkWire *w = 0;
	LinkPhase phase = LP_IDLE;
	int err = 0, peer_reason = -1;
	LinkAddr peer, hint;
	bool locked = false;
	uint32_t sid = 0, nonce = 0;
	uint64_t now = 0;
	uint64_t t_open = 0, last_rx = 0, last_tx = 0, last_census = 0, last_hello = 0, last_refuse = 0, last_welcome = 0,
	         last_cack = 0, last_ready = 0, last_start = 0, last_step = 0, last_progress = 0;
	LinkStats st;
	LinkStats census_prev;

	/* the state's crossing */
	std::vector<uint8_t> raw, comp;
	uint32_t raw_len = 0, comp_len = 0, raw_crc = 0, comp_crc = 0, nchunks = 0, have = 0, ack_base = 0, acked = 0;
	std::vector<uint8_t> cstate;           /* host: 0 not sent, 1 sent, 2 acked; guest: 0 / 1 received */
	std::vector<uint64_t> csent;
	uint32_t echo_chunk_ts = 0;
	uint64_t echo_chunk_at = 0;
	bool have_chunk_echo = false;
	std::vector<uint8_t> own;              /* the guest's own game, put back at the end */
	bool own_saved = false, replaced = false, restore_ok = true, ran = false;
	bool welcomed_ack = false;

	/* the run */
	unsigned delay = 0, hash_every = 240;
	uint32_t cur = 0, lnext = 0, peer_ack = 0, rcontig = 0, rtop = 0, max_sent = 0;
	uint16_t lin[RING], rin[RING];
	uint8_t rhave[RING];
	uint64_t lsent[RING];
	bool committed = false, guest_running = false;
	bool stalled = false;
	uint64_t stall_since = 0;
	uint32_t srtt = 0, rttvar = 0, rtt_min_p = 0;
	bool have_rtt = false;
	unsigned echo_sent = 0;
	uint64_t last_echo = 0, t_lock = 0;
	uint32_t echo_ts = 0;
	uint64_t echo_at = 0;
	bool have_echo = false;
	HashRec mine[HASHES], theirs[HASHES];
	std::vector<uint8_t> hbuf;
	uint32_t end_at = NO_END;
	bool final_done = false;
	int ext = 0;
	uint32_t cart_prev = 0;
	bool have_cart_prev = false;
	bool done_sending = false;
	uint64_t done_t0 = 0, done_last = 0, final_t0 = 0;
	bool bye_sent = false, closed = false;

	Impl() { memset(lin, 0, sizeof lin); memset(rin, 0, sizeof rin); memset(rhave, 0, sizeof rhave);
	         memset(lsent, 0, sizeof lsent); memset(mine, 0, sizeof mine); memset(theirs, 0, sizeof theirs); }

	uint32_t now32() const { return (uint32_t) now; }
	void log(const char *fmt, ...) __attribute__((format(printf, 2, 3)));

	void send_pkt(const LinkAddr &to, unsigned type, uint32_t sid_, const uint8_t *payload, size_t n)
	{
		uint8_t b[MAX_DGRAM];
		if (n > sizeof b - HDR - TAIL) return;
		memcpy(b, kMagic, 4);
		b[4] = (uint8_t) type;
		b[5] = 0;
		b[6] = (uint8_t) (n & 0xFF);
		b[7] = (uint8_t) (n >> 8);
		b[8] = (uint8_t) sid_; b[9] = (uint8_t) (sid_ >> 8); b[10] = (uint8_t) (sid_ >> 16); b[11] = (uint8_t) (sid_ >> 24);
		if (n) memcpy(b + HDR, payload, n);
		const uint32_t c = crc32(0, b, HDR + n);
		b[HDR + n] = (uint8_t) c; b[HDR + n + 1] = (uint8_t) (c >> 8); b[HDR + n + 2] = (uint8_t) (c >> 16); b[HDR + n + 3] = (uint8_t) (c >> 24);
		w->send(to, b, HDR + n + TAIL);
		st.pkts_out++;
		last_tx = now;
	}

	void fail(int e, bool tell, int reason = -1)
	{
		if (phase == LP_FAILED || phase == LP_DONE) return;
		if (tell && locked) send_bye((uint8_t) e);
		err = e;
		peer_reason = reason;
		phase = LP_FAILED;
		log("link: ended: %s (%s)", link_error_name(e), reason >= 0 ? link_error_name(reason) : "-");
	}

	void send_bye(uint8_t reason)
	{
		uint8_t pl[4] = { reason, 0, 0, 0 };
		if (peer.valid()) send_pkt(peer, T_BYE, sid, pl, 4);
		bye_sent = true;
	}

	/* ---- hashes ---- */
	uint32_t compute_hash()
	{
		if (m.state_hash) return m.state_hash(m.user);
		const size_t n = m.state_size(m.user);
		if (hbuf.size() != n) hbuf.assign(n, 0);
		if (!n || m.state_save(m.user, hbuf.data(), n) != 0) { fail(LE_START, true); return 0; }
		return crc32(0, hbuf.data(), n);
	}

	void compare(uint32_t frame)
	{
		const HashRec *a = 0, *b = 0;
		for (int i = 0; i < HASHES; i++) {
			if (mine[i].used && mine[i].frame == frame) a = &mine[i];
			if (theirs[i].used && theirs[i].frame == frame) b = &theirs[i];
		}
		if (!a || !b) return;
		if (a->hash == b->hash) { st.hashes_matched++; return; }
		log("link: out of sync at frame %u (this console %08x, the other %08x): the session ends", frame, a->hash, b->hash);
		fail(LE_DESYNC, true);
	}

	void add_hash(HashRec *ring, uint32_t frame, uint32_t hash)
	{
		for (int i = 0; i < HASHES; i++) if (ring[i].used && ring[i].frame == frame) return;
		int slot = 0;
		for (int i = 0; i < HASHES; i++) {
			if (!ring[i].used) { slot = i; break; }
			if ((int32_t) (ring[i].frame - ring[slot].frame) < 0) slot = i;
		}
		ring[slot].used = true;
		ring[slot].frame = frame;
		ring[slot].hash = hash;
		if (ring == mine) st.hashes_sent++;
		compare(frame);
	}

	/* ---- RTT ---- */
	void rtt_sample(uint32_t s)
	{
		if (s > 10000000u) return;
		if (!s) s = 1;
		/* a sample is inflated by whatever the other console's game loop held the datagram for: a long outlier
		 * (a state save over there) must not drag the estimate */
		if (have_rtt && s > 4 * srtt + 200000u) s = 4 * srtt + 200000u;
		if (!have_rtt) { srtt = s; rttvar = s / 2; rtt_min_p = s; have_rtt = true; }
		else {
			if (s < rtt_min_p) rtt_min_p = s;
			const uint32_t d = srtt > s ? srtt - s : s - srtt;
			rttvar = (3 * rttvar + d) / 4;
			srtt = (7 * srtt + s) / 8;
		}
		st.rtt_us = srtt;
		st.jitter_us = rttvar;
	}

	void note_echo(uint32_t ts)
	{
		if (!have_echo || (int32_t) (ts - echo_ts) > 0) { echo_ts = ts; echo_at = now; have_echo = true; }
	}

	/* the network's round trip as the wire measured it (answered by the other I/O thread), when it can */
	bool echo_ready(uint32_t *base, uint32_t *spread) const
	{
		uint32_t n = 0, mn = 0, mx = 0;
		if (!w->echo_result(&n, &mn, &mx) || n < 6) return false;
		*base = mn;
		*spread = mx - mn;
		return true;
	}

	/* One frame of delay carries the one-way trip rounded up, plus one for the two game loops' own tick; the spread
	 * between the fastest and slowest answer is what the trip may vary by. */
	unsigned choose_delay() const
	{
		if (cfg.delay > 0) return (unsigned) std::min(cfg.delay, 60);
		uint32_t base = 0, spread = 0;
		if (!echo_ready(&base, &spread)) {
			if (!have_rtt) return 4;
			base = rtt_min_p;
			spread = 2 * rttvar;
		}
		const uint64_t need = base / 2 + (uint64_t) spread;
		const uint64_t ft = cfg.frame_us ? cfg.frame_us : 16667;
		unsigned d = (unsigned) ((need + ft - 1) / ft) + 1;
		const unsigned cap = (unsigned) std::max(2, std::min(cfg.delay_cap, 60));
		if (d < 2) d = 2;
		if (d > cap) d = cap;
		return d;
	}

	/* ---- the handshake ---- */
	void send_hello()
	{
		uint8_t b[64];
		Wr o(b, sizeof b);
		o.u16(PROTO_VER); o.u16(0);
		o.u32(cfg.compat); o.u32(cfg.core_tag);
		o.u32(cfg.game_crc); o.u32(cfg.game_adler); o.u32(cfg.game_size);
		o.u32(nonce);
		send_pkt(peer, T_HELLO, 0, b, o.n);
		last_hello = now;
	}

	void send_welcome()
	{
		uint8_t b[96];
		Wr o(b, sizeof b);
		o.u32(nonce);
		o.u32(cfg.compat); o.u32(cfg.core_tag);
		o.u32(cfg.game_crc); o.u32(cfg.game_adler); o.u32(cfg.game_size);
		o.u16(hash_every); o.u16(0);
		o.u32(raw_len); o.u32(comp_len); o.u32(raw_crc); o.u32(comp_crc);
		o.u32(now32());
		send_pkt(peer, T_WELCOME, sid, b, o.n);
		last_welcome = now;
	}

	void refuse(const LinkAddr &to, int why)
	{
		if (now - last_refuse < 50000 && last_refuse) return;
		last_refuse = now;
		uint8_t pl[4] = { (uint8_t) why, 0, 0, 0 };
		send_pkt(to, T_BYE, 0, pl, 4);
	}

	/* who differs: 0 = nobody, else the LinkError to refuse with */
	int identity_mismatch(uint32_t compat, uint32_t core, uint32_t gcrc, uint32_t gad, uint32_t gsize) const
	{
		if (compat != cfg.compat || core != cfg.core_tag) return LE_PROTO;
		if (gcrc != cfg.game_crc || gad != cfg.game_adler || gsize != cfg.game_size) return LE_ROM_MISSING;
		return 0;
	}

	void on_hello(const LinkAddr &from, Rd &r)
	{
		if (!cfg.host) return;
		const unsigned proto = r.u16(); r.u16();
		const uint32_t compat = r.u32(), core = r.u32(), gcrc = r.u32(), gad = r.u32(), gsize = r.u32(), nn = r.u32();
		if (!r.ok) { st.bad++; return; }
		if (locked) {
			if (phase >= LP_STATE && nn == nonce && now - last_welcome > 100000) send_welcome();
			return;
		}
		if (phase != LP_WAIT_PEER) return;
		if (proto != PROTO_VER) { refuse(from, LE_PROTO); return; }
		const int why = identity_mismatch(compat, core, gcrc, gad, gsize);
		if (why) { log("link: refused a console (%s)", link_error_name(why)); refuse(from, why); return; }
		locked = true;
		peer = from;
		nonce = nn;
		/* The session id every later datagram must carry. Not a secret between the two consoles, but nobody else should
		 * be able to work it out from the time alone: the state's two checksums (unknown off the path) and where this
		 * object happens to live go in as well. */
		sid = ((nn * 2654435761u + now32() + 0x9E3779B9u) ^ (raw_crc * 0x85EBCA6Bu) ^ (comp_crc * 0xC2B2AE35u) ^
		       ((uint32_t) (uintptr_t) this * 0x27D4EB2Fu)) | 1u;
		phase = LP_STATE;
		t_lock = now;
		last_progress = now;
		cstate.assign(nchunks, 0);
		csent.assign(nchunks, 0);
		ack_base = acked = 0;
		log("link: the other console joined (state %u bytes, %u on the wire in %u chunks)", raw_len, comp_len, nchunks);
		send_welcome();
	}

	bool on_welcome(const LinkAddr &from, uint32_t sid_, Rd &r)
	{
		if (cfg.host || locked || phase != LP_WAIT_PEER || !sid_) return false;
		const uint32_t nn = r.u32(), compat = r.u32(), core = r.u32(), gcrc = r.u32(), gad = r.u32(), gsize = r.u32();
		const unsigned he = r.u16(); r.u16();
		const uint32_t rl = r.u32(), cl = r.u32(), rc = r.u32(), cc = r.u32(), ts = r.u32();
		if (!r.ok) { st.bad++; return false; }
		if (nn != nonce) return false;
		const int why = identity_mismatch(compat, core, gcrc, gad, gsize);
		if (why) {
			locked = true; peer = from; sid = sid_;
			log("link: the other console does not match (%s)", link_error_name(why));
			fail(why, true);
			return true;
		}
		if (he < 8 || he > 100000 || !rl || rl > cfg.state_max || rl != m.state_size(m.user) ||
		    !cl || cl > rl + rl / 255 + 64 || (cl + CHUNK - 1) / CHUNK > 65535) {
			locked = true; peer = from; sid = sid_;
			log("link: the other console's game state does not fit (raw %u, wire %u, mine %u)", rl, cl, (unsigned) m.state_size(m.user));
			fail(LE_PROTO, true);
			return true;
		}
		locked = true;
		peer = from;
		sid = sid_;
		hash_every = he;
		raw_len = rl; comp_len = cl; raw_crc = rc; comp_crc = cc;
		nchunks = (cl + CHUNK - 1) / CHUNK;
		comp.assign(cl, 0);
		cstate.assign(nchunks, 0);
		have = 0;
		phase = LP_STATE;
		last_progress = now;
		note_echo(ts);
		log("link: joined; receiving the game's state (%u bytes in %u chunks)", cl, nchunks);
		send_cack(true);
		return true;
	}

	/* ---- the state's crossing ---- */
	void send_cack(bool force)
	{
		if (!force && now - last_cack < 4000) return;
		uint32_t base = 0;
		while (base < nchunks && cstate[base]) base++;
		uint64_t mask = 0;
		for (unsigned i = 0; i < 64 && base + 1 + i < nchunks; i++) if (cstate[base + 1 + i]) mask |= 1ull << i;
		uint8_t b[40];
		Wr o(b, sizeof b);
		o.u32(base); o.u64(mask);
		o.u32(have_chunk_echo ? echo_chunk_ts : 0);
		o.u32(have_chunk_echo ? (uint32_t) (now - echo_chunk_at) : 0);
		o.u8(have_chunk_echo ? 1 : 0);
		send_pkt(peer, T_CACK, sid, b, o.n);
		last_cack = now;
	}

	void on_cack(Rd &r)
	{
		if (!cfg.host || phase != LP_STATE) return;
		const uint32_t base = r.u32();
		const uint64_t mask = r.u64();
		const uint32_t ets = r.u32(), hold = r.u32();
		const unsigned has = r.u8();
		if (!r.ok || base > nchunks) { st.bad++; return; }
		if (has) rtt_sample(now32() - ets - hold);
		welcomed_ack = true;
		for (uint32_t i = ack_base; i < base; i++) cstate[i] = 2;
		if (base > ack_base) ack_base = base;
		for (unsigned i = 0; i < 64 && base + 1 + i < nchunks; i++) if (mask >> i & 1) cstate[base + 1 + i] = 2;
		const uint32_t prev_acked = acked;
		acked = 0;
		for (uint32_t i = ack_base; i < nchunks && i < ack_base + 4096; i++) if (cstate[i] == 2) acked++;
		acked += ack_base;
		if (acked > prev_acked) last_progress = now;
		st.acks++;
	}

	void send_chunks()
	{
		if (!welcomed_ack) {
			if (now - last_welcome >= 250000) send_welcome();
		}
		const uint64_t rto = have_rtt ? std::max<uint64_t>(30000, 2ull * srtt + 4ull * rttvar) : 120000;
		unsigned inflight = 0, burst = 0;
		for (uint32_t i = ack_base; i < nchunks && inflight < WINDOW && burst < MAX_BURST; i++) {
			if (cstate[i] == 2) continue;
			if (cstate[i] == 1 && now - csent[i] < rto) { inflight++; continue; }
			const uint32_t off = i * CHUNK;
			const size_t len = std::min<size_t>(CHUNK, comp_len - off);
			uint8_t b[CHUNK + 16];
			Wr o(b, sizeof b);
			o.u16(i); o.u32(now32());
			o.bytes(comp.data() + off, len);
			send_pkt(peer, T_CHUNK, sid, b, o.n);
			if (cstate[i] == 1) st.retx++;
			cstate[i] = 1;
			csent[i] = now;
			inflight++;
			burst++;
		}
	}

	void on_chunk(Rd &r)
	{
		if (cfg.host || phase != LP_STATE) return;
		const unsigned idx = r.u16();
		const uint32_t ts = r.u32();
		if (!r.ok || idx >= nchunks) { st.bad++; return; }
		const size_t want = idx + 1 == nchunks ? comp_len - (size_t) idx * CHUNK : (size_t) CHUNK;
		if (r.left() != want) { st.bad++; return; }
		note_echo(ts);
		echo_chunk_ts = ts; echo_chunk_at = now; have_chunk_echo = true;
		if (cstate[idx]) { st.dups++; send_cack(false); return; }
		memcpy(comp.data() + (size_t) idx * CHUNK, r.p + r.i, want);
		cstate[idx] = 1;
		have++;
		last_progress = now;
		if (have == nchunks) { finish_transfer(); return; }
		if ((have & 7) == 0) send_cack(true); else send_cack(false);
	}

	void finish_transfer()
	{
		send_cack(true);
		if (crc32(0, comp.data(), comp_len) != comp_crc) { log("link: the state arrived damaged (wire CRC)"); fail(LE_STATE, true); return; }
		std::vector<uint8_t> rawb(raw_len);
		if (!link_lz_decompress(comp.data(), comp_len, rawb.data(), raw_len) || crc32(0, rawb.data(), raw_len) != raw_crc) {
			log("link: the state arrived damaged (content CRC)");
			fail(LE_STATE, true);
			return;
		}
		raw.swap(rawb);
		comp.clear();
		comp.shrink_to_fit();
		replaced = true;
		if (m.state_load(m.user, raw.data(), raw.size()) != 0) { log("link: the machine refused the host's state"); fail(LE_START, true); return; }
		phase = LP_START;
		last_progress = now;
		last_ready = 0;
		send_ready();
		log("link: the host's state is loaded");
	}

	void send_ready()
	{
		uint8_t b[4] = { 1, 0, 0, 0 };
		send_pkt(peer, T_READY, sid, b, 4);
		last_ready = now;
	}

	void send_start()
	{
		uint8_t b[4] = { (uint8_t) delay, 0, 0, 0 };
		send_pkt(peer, T_START, sid, b, 4);
		last_start = now;
	}

	void begin_run()
	{
		cur = 0;
		lnext = peer_ack = rcontig = rtop = delay;
		memset(rhave, 0, sizeof rhave);
		for (unsigned f = 0; f < delay; f++) { lin[f & 255] = 0; rin[f & 255] = 0; rhave[f & 255] = 1; }
		committed = false;
		phase = LP_RUN;
		last_census = now;
		census_prev = st;
		ran = true;
		st.delay = delay;
		log("link: running, input delay %u frames, rtt %u ms, jitter %u ms", delay, st.rtt_us / 1000, st.jitter_us / 1000);
	}

	void on_ready()
	{
		if (!cfg.host) return;
		if (phase == LP_RUN || phase == LP_ENDING) { if (!guest_running && now - last_start > 20000) send_start(); return; }
		if (phase != LP_STATE) return;
		{
			/* the delay is chosen from the network's own round trip: wait for the probes (a second at most) */
			uint32_t b = 0, sp = 0;
			if (cfg.delay <= 0 && !echo_ready(&b, &sp) && echo_sent > 0 && now - t_lock < 1500000) return;
		}
		if (cfg.host_reload) {
			if (m.state_load(m.user, raw.data(), raw.size()) != 0) { log("link: the host could not load its own state"); fail(LE_START, true); return; }
		}
		replaced = true;
		delay = choose_delay();
		begin_run();
		send_start();
	}

	void on_start(Rd &r)
	{
		if (cfg.host || phase != LP_START) return;
		const unsigned d = r.u8();
		if (!r.ok || d < 1 || d > 60) { st.bad++; return; }
		delay = d;
		begin_run();
	}

	/* ---- the input stream ---- */
	void send_input(bool resend)
	{
		(void) resend;
		const uint32_t N = cfg.redundancy < 1 ? 1 : cfg.redundancy > 32 ? 32 : cfg.redundancy;
		uint32_t first = std::min<uint32_t>(peer_ack, lnext > N ? lnext - N : 0);
		const uint32_t lo = lnext > MAX_INPUTS ? lnext - MAX_INPUTS : 0;
		if (first < lo) first = lo;
		if (first < delay) first = delay;
		if (first > lnext) first = lnext;
		uint32_t count = lnext - first;
		uint8_t b[256];
		Wr o(b, sizeof b);
		unsigned flags = 0;
		int nh = 0;
		HashRec pick[2];
		for (int i = 0; i < HASHES; i++) {
			if (!mine[i].used) continue;
			if (nh < 2) { pick[nh++] = mine[i]; continue; }
			const int lowest = (int32_t) (pick[0].frame - pick[1].frame) < 0 ? 0 : 1;
			if ((int32_t) (mine[i].frame - pick[lowest].frame) > 0) pick[lowest] = mine[i];
		}
		if (end_at != NO_END) flags |= 1;
		flags |= (unsigned) nh << 1;
		if (have_echo) flags |= 8;
		o.u32(rcontig);
		o.u32(now32());
		o.u32(have_echo ? echo_ts : 0);
		o.u32(have_echo ? (uint32_t) (now - echo_at) : 0);
		o.u32(first);
		o.u8(count);
		o.u8(flags);
		if (flags & 1) o.u32(end_at);
		for (int i = 0; i < nh; i++) { o.u32(pick[i].frame); o.u32(pick[i].hash); }
		const uint64_t late = (have_rtt ? srtt + 4ull * rttvar : 150000ull) + 2ull * (cfg.frame_us ? cfg.frame_us : 16667);
		for (uint32_t f = first; f < lnext; f++) {
			o.u16(lin[f & 255]);
			if (f >= peer_ack && f >= delay && f + 1 < lnext && now - lsent[f & 255] > late) st.retx++;
		}
		if (lnext > max_sent) max_sent = lnext;
		send_pkt(peer, T_INPUT, sid, b, o.n);
	}

	void on_input(Rd &r)
	{
		if (phase != LP_RUN && phase != LP_ENDING) return;
		const uint32_t ack = r.u32(), ts = r.u32(), echo = r.u32(), hold = r.u32(), first = r.u32();
		const unsigned count = r.u8(), flags = r.u8();
		uint32_t end = NO_END;
		if (flags & 1) end = r.u32();
		const unsigned nh = (flags >> 1) & 3;
		HashRec hr[3];
		if (nh > 2) { st.bad++; return; }
		for (unsigned i = 0; i < nh; i++) { hr[i].frame = r.u32(); hr[i].hash = r.u32(); hr[i].used = true; }
		if (!r.ok || count > MAX_INPUTS || r.left() != (size_t) count * 2) { st.bad++; return; }
		guest_running = true;
		note_echo(ts);
		if (flags & 8) rtt_sample(now32() - echo - hold);
		if (ack > peer_ack && ack <= lnext) { peer_ack = ack; st.acks++; }
		bool gap = false;
		for (unsigned i = 0; i < count; i++) {
			const uint32_t f = first + i;
			const uint16_t v = (uint16_t) (r.p[r.i + 2 * i] | r.p[r.i + 2 * i + 1] << 8);
			if (f < rcontig || (int32_t) (f - cur) < 0) { st.dups++; continue; }
			if (f - cur >= HORIZON) break;
			if (rhave[f & 255]) { st.dups++; continue; }
			rin[f & 255] = v;
			rhave[f & 255] = 1;
			if ((int32_t) (f + 1 - rtop) > 0) rtop = f + 1;
		}
		while (rhave[rcontig & 255] && rcontig - cur < HORIZON) rcontig++;
		if ((int32_t) (rtop - rcontig) > 0 || first > rcontig) gap = true;
		for (unsigned i = 0; i < nh; i++) add_hash(theirs, hr[i].frame, hr[i].hash);
		if (phase == LP_FAILED) return;
		if (end != NO_END) adopt_end(end);
		/* a gap: tell the other at once what we still need (rate limited) */
		if (gap && now - last_tx >= 4000) send_input(true);
	}

	void adopt_end(uint32_t e)
	{
		if (phase != LP_RUN && phase != LP_ENDING) return;
		if (end_at != NO_END && (int32_t) (e - end_at) <= 0) return;
		if ((int32_t) (e - cur) < 0 || e - cur > 2000) { log("link: an impossible end frame (%u at %u)", e, cur); fail(LE_PROTO, true); return; }
		end_at = e;
		final_done = false;
		if (phase == LP_RUN) {
			phase = LP_ENDING;
			final_t0 = 0;
			if (cfg.host && m.cart_crc) { cart_prev = m.cart_crc(m.user); have_cart_prev = true; }
			log("link: the end is agreed at frame %u", end_at);
		}
	}

	void on_done(Rd &r)
	{
		if (cfg.host || phase != LP_ENDING || !final_done) return;
		const uint32_t f = r.u32(), h = r.u32();
		if (!r.ok) { st.bad++; return; }
		if (f != end_at) return;
		add_hash(theirs, f, h);            /* counted and compared like any other; a mismatch ends it as a desync */
		if (phase != LP_ENDING) return;
		for (int i = 0; i < HASHES; i++) {
			if (mine[i].used && mine[i].frame == f && mine[i].hash == h) {
				phase = LP_DONE;
				send_bye(0);
				log("link: ended together at frame %u", f);
				return;
			}
		}
	}

	void on_bye(Rd &r)
	{
		const unsigned reason = r.u8();
		if (!r.ok) { st.bad++; return; }
		if (reason == 0) {
			if (cfg.host && done_sending) { phase = LP_DONE; log("link: ended together at frame %u", end_at); return; }
			fail(LE_PEER_ABORT, false, LE_LOCAL);
			return;
		}
		fail(LE_PEER_ABORT, false, reason < LE_COUNT ? (int) reason : LE_PROTO);
	}

	/* ---- the datagram in ---- */
	void on_datagram(const LinkAddr &from, const uint8_t *b, size_t n)
	{
		if (n < (size_t) HDR + TAIL || n > MAX_DGRAM || memcmp(b, kMagic, 4) != 0) { st.bad++; return; }
		const size_t len = (size_t) b[6] | (size_t) b[7] << 8;
		if (HDR + len + TAIL != n) { st.bad++; return; }
		if (crc32(0, b, n - TAIL) != le32(b + n - TAIL)) { st.bad++; return; }
		const unsigned type = b[4];
		const uint32_t sid_ = le32(b + 8);
		/* only the peer is heard, once there is one */
		if (locked) { if (from != peer) { st.foreign++; return; } }
		else if (hint.valid()) { if (from != hint) { st.foreign++; return; } }
		else if (!cfg.host) { st.foreign++; return; }
		st.pkts_in++;
		Rd r(b + HDR, len);
		if (!locked || (type == T_BYE && sid_ == 0 && phase == LP_WAIT_PEER)) {
			if (type == T_HELLO) { last_rx = now; on_hello(from, r); }
			else if (type == T_WELCOME) { if (on_welcome(from, sid_, r)) last_rx = now; }
			else if (type == T_BYE && !cfg.host && from == peer) { last_rx = now; on_bye(r); }
			return;
		}
		if (type == T_HELLO) { last_rx = now; on_hello(from, r); return; }
		if (sid_ != sid) { st.foreign++; return; }
		last_rx = now;
		switch (type) {
		case T_WELCOME: break;
		case T_BYE: on_bye(r); break;
		case T_CHUNK: on_chunk(r); break;
		case T_CACK: on_cack(r); break;
		case T_READY: on_ready(); break;
		case T_START: on_start(r); break;
		case T_INPUT: on_input(r); break;
		case T_DONE: on_done(r); break;
		default: st.bad++; break;
		}
	}

	/* ---- timers ---- */
	void census()
	{
		if (now - last_census < 1000000) return;
		last_census = now;
		char l[220];
		snprintf(l, sizeof l, "link: t=%us frame=%u in=%u out=%u acks=%u retx=%u rtt=%ums jitter=%ums delay=%u stalls=%u/%u path=%s",
		         (unsigned) ((now - t_open) / 1000000), cur, st.pkts_in - census_prev.pkts_in, st.pkts_out - census_prev.pkts_out,
		         st.acks - census_prev.acks, st.retx - census_prev.retx, st.rtt_us / 1000, st.jitter_us / 1000, delay,
		         st.stalls - census_prev.stalls, st.stall_episodes - census_prev.stall_episodes, w->path());
		log("%s", l);
		census_prev = st;
	}

	void tick()
	{
		if (phase == LP_IDLE || phase == LP_DONE || phase == LP_FAILED) return;
		if (phase == LP_WAIT_PEER) {
			if (!cfg.host && hint.valid() && (!last_hello || now - last_hello >= 250000)) send_hello();
			if (now - t_open > (uint64_t) cfg.wait_ms * 1000u) { log("link: nobody answered in %u s", cfg.wait_ms / 1000); fail(LE_NO_PEER, false); }
			return;
		}
		/* peer-dead: the I/O thread's word when it has one (it runs while the game thread is busy), else ours */
		uint64_t age = w->heard_age_us(peer);
		if (age == UINT64_MAX) age = now - last_rx;
		else if (now - last_rx < age) age = now - last_rx;
		if (age > (uint64_t) cfg.dead_ms * 1000u && !(phase == LP_ENDING && done_sending)) {
			log("link: the other console has been silent for %u s", (unsigned) (age / 1000000));
			fail(LE_PEER_LOST, false);
			return;
		}
		switch (phase) {
		case LP_STATE:
			if (last_progress && now - last_progress > 15000000ull) {
				log("link: state transfer stalled for %u s", (unsigned) ((now - last_progress) / 1000000));
				fail(LE_PEER_LOST, true);
				break;
			}
			if (cfg.host && cfg.delay <= 0 && echo_sent < 24 && now - last_echo >= 25000) { w->echo_probe(peer); echo_sent++; last_echo = now; }
			if (cfg.host) send_chunks();
			else if (now - last_cack >= 100000) send_cack(true);
			break;
		case LP_START:
			if (last_progress && now - last_progress > 15000000ull) {
				log("link: start handshake stalled for %u s", (unsigned) ((now - last_progress) / 1000000));
				fail(LE_PEER_LOST, true);
				break;
			}
			if (now - last_ready >= 100000) send_ready();
			break;
		case LP_RUN:
		case LP_ENDING: {
			if (cfg.host && !guest_running && now - last_start >= 50000) send_start();
			/* a stalled or idle console keeps the other's window moving: a resend at a rate that backs off */
			const uint64_t sl = stalled ? now - stall_since : 0;
			const bool at_end = phase == LP_ENDING && (int32_t) (cur - end_at) >= 0;
			const uint64_t every = at_end ? 20000 : !stalled ? 250000 : sl < 500000 ? 10000 : sl < 3000000 ? 30000 : 100000;
			if (now - last_tx >= every) send_input(true);
			if (phase == LP_ENDING && cfg.host && done_sending && now - done_last >= 50000) send_done();
			if (phase == LP_ENDING && cfg.host && done_sending && now - done_t0 > 1500000) { phase = LP_DONE; log("link: ended together at frame %u (no BYE back)", end_at); }
			break;
		}
		default: break;
		}
		if (phase != LP_FAILED) census();
	}

	void send_done()
	{
		uint32_t h = 0;
		for (int i = 0; i < HASHES; i++) if (mine[i].used && mine[i].frame == end_at) h = mine[i].hash;
		uint8_t b[8];
		Wr o(b, sizeof b);
		o.u32(end_at); o.u32(h);
		send_pkt(peer, T_DONE, sid, b, o.n);
		done_last = now;
	}

	void pump()
	{
		now = w->now_us();
		uint8_t buf[MAX_DGRAM + 64];
		for (int k = 0; k < 96; k++) {
			LinkAddr from;
			const int n = w->recv(&from, buf, sizeof buf);
			if (n <= 0) break;
			on_datagram(from, buf, (size_t) n);
			if (phase == LP_FAILED || phase == LP_DONE) return;
		}
		tick();
	}

	/* ---- the end ---- */
	void final_stage()
	{
		if (!final_done) {
			if (cfg.host && m.cart_crc && have_cart_prev) {
				const uint32_t c = m.cart_crc(m.user);
				if (c != cart_prev && ext < 8) {
					cart_prev = c;
					ext++;
					end_at += 15;
					log("link: the battery RAM is changing; the end moves to frame %u", end_at);
					return;
				}
			}
			const uint32_t h = compute_hash();
			if (phase == LP_FAILED) return;
			final_done = true;
			final_t0 = now;
			add_hash(mine, end_at, h);
			return;
		}
		if (cfg.host) {
			bool both = false;
			for (int i = 0; i < HASHES; i++) if (theirs[i].used && theirs[i].frame == end_at) both = true;
			if (both && !done_sending && phase != LP_FAILED) {
				done_sending = true;
				done_t0 = done_last = now;
				send_done();
			}
		}
		if (final_t0 && now - final_t0 > 15000000u && phase == LP_ENDING) { log("link: the end was not confirmed"); fail(LE_PEER_LOST, true); }
	}

	int step(uint16_t pad)
	{
		if (phase == LP_IDLE) return LS_WAIT;
		if (phase == LP_DONE || phase == LP_FAILED) return LS_OVER;
		pump();
		last_step = now;
		if (phase == LP_DONE || phase == LP_FAILED) return LS_OVER;
		if (phase != LP_RUN && phase != LP_ENDING) return LS_WAIT;
		if (phase == LP_ENDING && (int32_t) (cur - end_at) >= 0) {
			final_stage();
			if (phase == LP_DONE || phase == LP_FAILED) return LS_OVER;
			if ((int32_t) (cur - end_at) >= 0) return LS_WAIT;
		}
		if (!committed) {
			if (lnext - peer_ack < HORIZON) {
				lin[lnext & 255] = pad;
				lsent[lnext & 255] = now;
				lnext++;
				committed = true;
				send_input(false);
			}
		}
		/* the bounded wait for the other console's buttons for this frame */
		bool have_in = rhave[cur & 255] != 0;
		if (!have_in) {
			const uint64_t t0 = w->now_us();
			for (int i = 0; i < 24 && !have_in; i++) {
				w->idle(500);
				pump();
				if (phase == LP_DONE || phase == LP_FAILED) return LS_OVER;
				have_in = rhave[cur & 255] != 0;
				if (!have_in && now - t0 >= cfg.budget_us) break;
			}
		}
		if (!have_in || !committed) {
			st.stalls++;
			if (!stalled) { stalled = true; stall_since = now; st.stall_episodes++; }
			const uint64_t sl = now - stall_since;
			if (sl > st.max_stall_us) st.max_stall_us = sl > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t) sl;
			if (sl > (uint64_t) cfg.stall_max_ms * 1000u) { log("link: the other console sent no buttons for %u s", (unsigned) (sl / 1000000)); fail(LE_PEER_LOST, true); return LS_OVER; }
			return LS_STALL;
		}
		if (stalled) { stalled = false; }
		const uint16_t mine_pad = lin[cur & 255], theirs_pad = rin[cur & 255];
		const int rc = cfg.host ? m.run_frame(m.user, mine_pad, theirs_pad) : m.run_frame(m.user, theirs_pad, mine_pad);
		if (rc != 0) { log("link: the machine could not run a frame"); fail(LE_START, true); return LS_OVER; }
		rhave[cur & 255] = 0;
		cur++;
		committed = false;
		st.frames++;
		if (cur % hash_every == 0 && cur != end_at) {
			const uint32_t h = compute_hash();
			if (phase == LP_FAILED) return LS_OVER;
			add_hash(mine, cur, h);
			if (phase == LP_FAILED) return LS_OVER;
		}
		return LS_FRAME;
	}

	void restore()
	{
		if (!replaced) return;
		if (cfg.host) {
			if (phase != LP_DONE && err != LE_PEER_LOST && !raw.empty() && ran) restore_ok = m.state_load(m.user, raw.data(), raw.size()) == 0;
		} else if (own_saved) {
			restore_ok = m.state_load(m.user, own.data(), own.size()) == 0;
		}
		replaced = false;
	}
};

void Link::Impl::log(const char *fmt, ...)
{
	if (!w) return;
	char b[300];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(b, sizeof b, fmt, ap);
	va_end(ap);
	w->log(b);
}

/* ==================================================================================================================== */

Link::Link() : p_(new Impl) {}
Link::~Link() { close(); delete p_; }

bool Link::open(const LinkConfig &cfg, const LinkMachine &m, LinkWire *wire)
{
	Impl *p = p_;
	if (p->phase != LP_IDLE || !wire || !m.run_frame || !m.state_size || !m.state_save || !m.state_load) return false;
	p->cfg = cfg;
	p->m = m;
	p->w = wire;
	p->now = wire->now_us();
	p->t_open = p->now;
	p->hash_every = cfg.hash_every < 8 ? 8 : cfg.hash_every > 100000 ? 100000 : cfg.hash_every;
	if (p->cfg.state_max > (64u << 20)) p->cfg.state_max = 64u << 20;
	const size_t sn = m.state_size(m.user);
	if (!sn || sn > p->cfg.state_max) { p->err = LE_START; p->phase = LP_FAILED; return false; }
	p->raw_len = (uint32_t) sn;
	uint8_t nb[4];
	{
		uint32_t x = (uint32_t) p->now * 2654435761u ^ cfg.compat ^ (cfg.host ? 0x1234567u : 0x7654321u);
		for (int i = 0; i < 4; i++) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; nb[i] = (uint8_t) x; }
		p->nonce = le32(nb) | 1u;
	}
	if (cfg.host) {
		p->raw.assign(sn, 0);
		if (m.state_save(m.user, p->raw.data(), sn) != 0) { p->err = LE_START; p->phase = LP_FAILED; return false; }
		if (!link_lz_compress(p->raw.data(), sn, p->comp)) { p->err = LE_MEMORY; p->phase = LP_FAILED; return false; }
		p->comp_len = (uint32_t) p->comp.size();
		p->raw_crc = crc32(0, p->raw.data(), sn);
		p->comp_crc = crc32(0, p->comp.data(), p->comp.size());
		p->nchunks = (p->comp_len + CHUNK - 1) / CHUNK;
		p->st.state_raw = p->raw_len;
		p->st.state_wire = p->comp_len;
		if (p->nchunks > 65535) { p->err = LE_MEMORY; p->phase = LP_FAILED; return false; }
	} else {
		p->own.assign(sn, 0);
		if (m.state_save(m.user, p->own.data(), sn) != 0) { p->err = LE_START; p->phase = LP_FAILED; return false; }
		p->own_saved = true;
		p->st.state_raw = p->raw_len;
	}
	p->phase = LP_WAIT_PEER;
	return true;
}

void Link::set_peer(const LinkAddr &a)
{
	p_->hint = a;
	if (!p_->cfg.host) p_->peer = a;
}

int Link::step(uint16_t pad) { return p_->step(pad); }

void Link::request_end()
{
	Impl *p = p_;
	if (p->phase == LP_RUN) {
		p->adopt_end(p->cur + p->delay + 2);
		p->final_done = false;
	} else if (p->phase == LP_WAIT_PEER || p->phase == LP_STATE || p->phase == LP_START) {
		p->fail(LE_LOCAL, true);
	}
}

void Link::suspend()
{
	Impl *p = p_;
	if (p->phase == LP_IDLE || p->phase == LP_DONE || p->phase == LP_FAILED) return;
	if (p->w) p->now = p->w->now_us();
	p->fail(LE_SUSPEND, true);
	if (p->peer.valid() && p->locked) p->send_bye((uint8_t) LE_SUSPEND);
}

void Link::close()
{
	Impl *p = p_;
	if (p->closed) return;
	p->closed = true;
	if (p->phase == LP_IDLE) return;
	if (p->w) p->now = p->w->now_us();
	if (p->locked && p->peer.valid() && p->phase != LP_DONE) {
		if (p->phase != LP_FAILED) { p->err = LE_LOCAL; p->phase = LP_FAILED; }
		if (!p->bye_sent) { p->send_bye((uint8_t) (p->err ? p->err : LE_LOCAL)); }
		p->send_bye((uint8_t) (p->err ? p->err : LE_LOCAL));
	} else if (p->phase != LP_DONE && p->phase != LP_FAILED) {
		p->err = LE_LOCAL;
		p->phase = LP_FAILED;
	}
	p->restore();
}

LinkPhase Link::phase() const { return p_->phase; }
bool Link::is_host() const { return p_->cfg.host; }
int Link::error() const { return p_->err; }
int Link::peer_reason() const { return p_->peer_reason; }
uint32_t Link::frame() const { return p_->cur; }
uint32_t Link::end_frame() const { return p_->end_at; }
unsigned Link::delay() const { return p_->delay; }
bool Link::restored_ok() const { return p_->restore_ok; }
bool Link::state_replaced() const { return p_->replaced; }

int Link::progress() const
{
	const Impl *p = p_;
	if (p->phase != LP_STATE || !p->nchunks) return -1;
	return (int) ((p->cfg.host ? (uint64_t) p->acked : (uint64_t) p->have) * 1000u / p->nchunks);
}

LinkStats Link::stats() const
{
	LinkStats s = p_->st;
	s.delay = p_->delay;
	return s;
}

std::string Link::summary() const
{
	const Impl *p = p_;
	char b[300];
	snprintf(b, sizeof b, "link role=%s phase=%s frames=%u delay=%u stalls=%u/%u hashes=%u/%u retx=%u in=%u out=%u bad=%u foreign=%u rtt=%ums error=%s peer_reason=%d",
	         p->cfg.host ? "host" : "guest", kPhaseName[p->phase], p->st.frames, p->delay, p->st.stalls, p->st.stall_episodes,
	         p->st.hashes_matched, p->st.hashes_sent, p->st.retx, p->st.pkts_in, p->st.pkts_out, p->st.bad, p->st.foreign,
	         p->st.rtt_us / 1000, link_error_name(p->err), p->peer_reason);
	return b;
}

}  // namespace pk
