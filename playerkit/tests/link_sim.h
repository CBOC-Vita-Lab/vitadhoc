/* A virtual network and a fake machine for the pk_link tests: two consoles in one thread, a clock that only moves when the
 * test moves it, loss / duplication / latency / jitter (which reorders) per direction.
 * Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE). */
#ifndef PK_LINK_SIM_H
#define PK_LINK_SIM_H

#include "pk_link.h"
#include "pk_util.h"

#include <deque>
#include <functional>
#include <string>
#include <string.h>
#include <vector>

namespace linksim {

struct Net {
	struct Pkt { uint64_t due; uint64_t seq; int to; pk::LinkAddr from; std::vector<uint8_t> b; };
	uint64_t t = 0, seq = 0;
	uint32_t rng = 12345;
	std::vector<Pkt> q;
	struct Dir { int loss = 0, dup = 0; unsigned lat = 0, jit = 0; } dir[2];     /* dir[0]: A to B, dir[1]: B to A */
	bool io_dead[2] = { false, false };       /* the console's I/O thread is gone (killed): no data, no keepalive */
	bool blackhole = false;                   /* nothing crosses at all */
	std::deque<std::pair<pk::LinkAddr, std::vector<uint8_t> > > inbox[2];
	uint64_t last_heard[2] = { 0, 0 };        /* when the I/O thread of side X last heard the other */
	uint64_t next_keep = 1000000;
	pk::LinkAddr addr[2];
	/* a hook that may change a datagram in flight (an attacker, or a bad cable): return false to drop it */
	std::function<bool(int to, std::vector<uint8_t> &)> mutate;
	uint64_t delivered[2] = { 0, 0 }, sent[2] = { 0, 0 };

	Net() { addr[0].ip_be = 0x0100000A; addr[0].port = 1000; addr[1].ip_be = 0x0200000A; addr[1].port = 2000; }
	uint32_t rnd() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

	void send(int from, const void *buf, size_t n)
	{
		sent[from]++;
		if (blackhole || io_dead[from]) return;
		const Dir &d = dir[from];
		if (d.loss && (int) (rnd() % 100) < d.loss) return;
		int copies = 1;
		if (d.dup && (int) (rnd() % 100) < d.dup) copies = 2;
		while (copies--) {
			Pkt p;
			p.due = t + d.lat + (d.jit ? rnd() % (d.jit + 1) : 0);
			p.seq = seq++;
			p.to = 1 - from;
			p.from = addr[from];
			p.b.assign((const uint8_t *) buf, (const uint8_t *) buf + n);
			if (mutate && !mutate(p.to, p.b)) continue;
			q.push_back(p);
		}
	}

	void advance(uint64_t to)
	{
		for (;;) {
			/* the earliest due datagram not later than `to` */
			size_t best = q.size();
			for (size_t i = 0; i < q.size(); i++)
				if (q[i].due <= to && (best == q.size() || q[i].due < q[best].due || (q[i].due == q[best].due && q[i].seq < q[best].seq))) best = i;
			if (best == q.size()) break;
			Pkt p = q[best];
			q.erase(q.begin() + (long) best);
			if (!io_dead[p.to]) {
				inbox[p.to].push_back(std::make_pair(p.from, p.b));
				last_heard[p.to] = p.due;
				delivered[p.to]++;
			}
		}
		/* the transports' keepalives: each I/O thread tells the other it is alive, whatever its game thread does */
		while (next_keep <= to) {
			for (int s = 0; s < 2; s++)
				if (!io_dead[s] && !io_dead[1 - s] && !blackhole) last_heard[1 - s] = next_keep + dir[s].lat;
			next_keep += 1000000;
		}
		t = to;
	}
};

struct Wire : pk::LinkWire {
	Net *net;
	int side;
	std::vector<std::string> lines;
	bool print = false;
	Wire(Net *n, int s) : net(n), side(s) {}
	uint64_t now_us() { return net->t; }
	void send(const pk::LinkAddr &to, const void *buf, size_t n) { if (to == net->addr[1 - side]) net->send(side, buf, n); }
	int recv(pk::LinkAddr *from, void *buf, size_t cap)
	{
		if (net->inbox[side].empty()) return 0;
		std::pair<pk::LinkAddr, std::vector<uint8_t> > d = net->inbox[side].front();
		net->inbox[side].pop_front();
		*from = d.first;
		const size_t n = d.second.size() < cap ? d.second.size() : cap;
		memcpy(buf, d.second.data(), n);
		return (int) n;
	}
	uint64_t heard_age_us(const pk::LinkAddr &) { return net->last_heard[side] ? net->t - net->last_heard[side] : UINT64_MAX; }
	void idle(unsigned) {}
	void log(const char *l) { lines.push_back(l); if (print) fprintf(stderr, "[%d] %s\n", side, l); }
	const char *path() { return "sim"; }
	bool saw(const char *needle) const { for (size_t i = 0; i < lines.size(); i++) if (lines[i].find(needle) != std::string::npos) return true; return false; }
};

/* A machine whose state is a block of bytes (partly compressible) that every frame mixes with both pads. */
struct Machine {
	std::vector<uint8_t> s;
	uint32_t frame_no = 0;     /* counted in the state too, at offset 0 */
	int fail_state_load = 0;
	int diverge_at = -1;       /* frame at which this console's machine changes one byte of its own (a bug, a bit flip) */
	explicit Machine(size_t n = 200000, uint32_t seed = 7)
	{
		s.assign(n, 0);
		uint32_t x = seed;
		for (size_t i = 16; i < 4096 && i < n; i++) { x = x * 1664525u + 1013904223u; s[i] = (uint8_t) (x >> 24); }
		for (size_t i = 8192; i + 64 < n; i += 4096) memset(&s[i], (int) (i >> 12), 64);
	}
	uint32_t get(size_t o) const { uint32_t v; memcpy(&v, &s[o], 4); return v; }
	void put(size_t o, uint32_t v) { memcpy(&s[o], &v, 4); }
	static int run(void *u, uint16_t p1, uint16_t p2)
	{
		Machine *m = (Machine *) u;
		uint32_t f = m->get(0) + 1;
		m->put(0, f);
		uint32_t h = m->get(4) * 2654435761u ^ ((uint32_t) p1 << 16 | p2) ^ f * 40503u;
		m->put(4, h);
		for (int k = 0; k < 16; k++) { h = h * 1664525u + 1013904223u; m->put(64 + (size_t) (h % 1000) * 4, h); }
		if (m->diverge_at >= 0 && (int) f == m->diverge_at) m->s[5000] ^= 0x10;
		return 0;
	}
	static size_t size(void *u) { return ((Machine *) u)->s.size(); }
	static int save(void *u, void *b, size_t n) { Machine *m = (Machine *) u; if (n != m->s.size()) return -1; memcpy(b, m->s.data(), n); return 0; }
	static int load(void *u, const void *b, size_t n)
	{
		Machine *m = (Machine *) u;
		if (m->fail_state_load || n != m->s.size()) return -1;
		memcpy(m->s.data(), b, n);
		return 0;
	}
	pk::LinkMachine iface()
	{
		pk::LinkMachine r;
		r.user = this; r.run_frame = run; r.state_size = size; r.state_save = save; r.state_load = load;
		return r;
	}
	uint32_t crc() const { return pk::crc32(0, s.data(), s.size()); }
};

inline uint16_t pad_for(bool host, uint32_t frame)
{
	uint32_t x = (frame / 5) * 2654435761u ^ (host ? 0x9E3779B9u : 0x85EBCA6Bu);
	x ^= x >> 15; x *= 0x2C1B3C6Du; x ^= x >> 12;
	return (uint16_t) (x & 0x0FFF);
}

struct Rig {
	Net net;
	Wire wa, wb;
	Machine ma, mb;
	pk::Link a, b;          /* a = host, b = guest */
	pk::LinkConfig ca, cb;
	uint64_t period[2] = { 16667, 16667 };
	uint64_t next[2] = { 0, 0 };
	uint64_t frozen_until[2] = { 0, 0 };
	bool stepping[2] = { true, true };
	Rig(size_t state = 200000) : wa(&net, 0), wb(&net, 1), ma(state), mb(state, 99)
	{
		ca.host = true; cb.host = false;
		ca.compat = cb.compat = 0xC0FFEE01;
		ca.core_tag = cb.core_tag = 0x5EED0001;
		ca.game_crc = cb.game_crc = 0x11223344; ca.game_adler = cb.game_adler = 0x55667788; ca.game_size = cb.game_size = 1234567;
		ca.hash_every = cb.hash_every = 60;
		ca.wait_ms = cb.wait_ms = 30000;
	}
	bool open()
	{
		bool ok = a.open(ca, ma.iface(), &wa);
		ok = b.open(cb, mb.iface(), &wb) && ok;
		b.set_peer(net.addr[0]);
		a.set_peer(net.addr[1]);
		return ok;
	}
	/* run the two consoles in virtual time until `pred` or `max_us` of it */
	template <class F> void run(uint64_t max_us, F pred)
	{
		const uint64_t end = net.t + max_us;
		while (net.t < end) {
			const int s = next[0] <= next[1] ? 0 : 1;
			const uint64_t at = next[s];
			if (at > end) break;
			if (at > net.t) net.advance(at);
			next[s] += period[s];
			pk::Link &l = s == 0 ? a : b;
			if (l.over() || !stepping[s] || at < frozen_until[s]) { if (pred()) return; continue; }
			l.step(pad_for(s == 0, l.frame()));
			if (pred()) return;
		}
	}
	void run_frames(uint32_t n, uint64_t max_us)
	{
		run(max_us, [&]() { return a.frame() >= n && b.frame() >= n; });
	}
};

}  // namespace linksim

#endif
