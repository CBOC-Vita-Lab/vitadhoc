/* See pk_link_net.h. Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE). */
#include "pk_link_net.h"
#include "pk_util.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

#ifdef __vita__
#include <psp2/kernel/threadmgr.h>
#else
#include <unistd.h>
#endif

#include "transport_lan.h"

namespace pk {

namespace {

const char kMagic[4] = { 'P', 'K', 'L', '1' };

LinkAddr addr_of(const uint8_t mac[6])
{
	LinkAddr a;
	lan_addr_from_mac(mac, &a.ip_be, &a.port);
	return a;
}

int parse_addr(const std::string &s, uint32_t *ip, uint16_t *port, uint16_t def)
{
	return !s.empty() && lan_parse_addr(s.c_str(), ip, port, def) == 0 && *ip != 0;
}

void hex_bytes(const std::string &h, uint8_t out[8])
{
	for (int i = 0; i < 8; i++) {
		unsigned v = 0;
		for (int k = 0; k < 2; k++) {
			const char c = h[(size_t) (2 * i + k)];
			v = v * 16 + (unsigned) (c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 0);
		}
		out[i] = (uint8_t) v;
	}
}

}  // namespace

/* the transport as pk_link's wire */
struct LanWire : LinkWire {
	lan_transport *t = 0;
	nd_transport tp;          /* only heard_age_us is used: the I/O thread's word on when the peer was last heard */
	NetEnv env;
	bool have_tp = false;

	uint64_t now_us() { return env.now_us(); }
	void send(const LinkAddr &to, const void *buf, size_t n)
	{
		uint8_t mac[6];
		lan_mac_from_addr(to.ip_be, to.port, mac);
		lan_transport_bulk_send(t, mac, buf, n);
	}
	int recv(LinkAddr *from, void *buf, size_t cap)
	{
		uint8_t mac[6];
		const int n = lan_transport_bulk_recv_from(t, mac, buf, cap);
		if (n > 0) *from = addr_of(mac);
		return n;
	}
	uint64_t heard_age_us(const LinkAddr &peer)
	{
		if (!have_tp || !tp.heard_age_us) return UINT64_MAX;
		uint8_t mac[6];
		lan_mac_from_addr(peer.ip_be, peer.port, mac);
		return tp.heard_age_us(tp.ctx, mac);
	}
	void idle(unsigned us)
	{
#ifdef __vita__
		sceKernelDelayThread(us);
#else
		usleep(us);
#endif
	}
	void echo_probe(const LinkAddr &peer)
	{
		uint8_t mac[6];
		lan_mac_from_addr(peer.ip_be, peer.port, mac);
		lan_transport_echo(t, mac);
	}
	bool echo_result(uint32_t *n, uint32_t *mn, uint32_t *mx)
	{
		uint32_t last = 0;
		const int k = lan_transport_echo_result(t, mn, mx, &last);
		*n = k < 0 ? 0 : (uint32_t) k;
		return true;
	}
	void log(const char *line)
	{
		if (!env.log) return;
		if (!strncmp(line, "link: ", 6)) line += 6;      /* the player's log line says "link:" already */
		env.log(line);
	}
	const char *path()
	{
		lan_census c;
		lan_transport_census(t, &c, 0);
		return c.path ? c.path : "lan";
	}
};

struct NetLink::Impl {
	NetEnv env;
	LinkRequest req;
	bool host = false;
	LanWire wire;
	lan_transport *lan = 0;
	Link core;
	std::string why;
	uint64_t t_open = 0, t_phase = 0;
	unsigned wait_ms = 90000;
	int relay_bound = 0;
	uint32_t peer_ip_be = 0;
	uint16_t peer_port = 0;
	bool core_open = false, peer_set = false;
	std::string mode;

	void log(const char *fmt, ...) __attribute__((format(printf, 2, 3)))
	{
		if (!env.log) return;
		char b[300];
		va_list ap;
		va_start(ap, fmt);
		vsnprintf(b, sizeof b, fmt, ap);
		va_end(ap);
		env.log(b);
	}
};

NetLink::NetLink() : p_(new Impl) {}
NetLink::~NetLink() { close(); delete p_; }

static NetFail map_error(int e)
{
	switch (e) {
	case LE_ROM_MISSING: case LE_ROM_BAD: return NF_GAME;
	case LE_PROTO: return NF_VERSION;
	case LE_PEER_LOST: case LE_PEER_ABORT: case LE_SUSPEND: return NF_LOST;
	case LE_DESYNC: return NF_DESYNC;
	case LE_LOCAL: return NF_LOCAL;
	case LE_NO_PEER: return NF_NO_CONNECT;
	case LE_MEMORY: case LE_START: case LE_STATE: return NF_START;
	default: return NF_OTHER;
	}
}

bool NetLink::open(const LinkRequest &req, const std::string &rom_file, uint32_t core_tag, const LinkMachine &m, const NetEnv &env,
                   uint32_t frame_us)
{
	Impl *p = p_;
	if (phase_ != NP_IDLE || !env.now_us) return false;
	p->env = env;
	p->req = req;
	p->host = req.is_host();
	p->mode = req.mode;
	host_ = p->host;
	p->t_open = env.now_us();
	p->wait_ms = (unsigned) (req.wait_s ? req.wait_s : 90) * 1000u;
	fail_ = NF_NONE;
	auto die = [&](NetFail f, const char *w) { fail_ = f; p->why = w; phase_ = NP_FAILED; p->log("could not start: %s", w); return false; };

	/* the game: what both consoles must be running */
	std::vector<uint8_t> rom;
	if (!file_read(rom_file, rom, 64u << 20) || rom.empty()) return die(NF_START, "the game image cannot be read");
	LinkConfig cfg;
	cfg.host = p->host;
	cfg.core_tag = core_tag;
	if (!req.compat.empty()) cfg.compat = (uint32_t) strtoul(req.compat.c_str(), 0, 16);
	cfg.game_crc = crc32(0, rom.data(), rom.size());
	cfg.game_adler = adler32(1, rom.data(), rom.size());
	cfg.game_size = (uint32_t) rom.size();
	rom.clear();
	rom.shrink_to_fit();
	cfg.delay = req.delay > 0 ? req.delay : 0;
	cfg.online = req.mode == "punch" || req.mode == "relay";
	cfg.delay_cap = cfg.online ? 12 : 8;
	cfg.frame_us = frame_us;
	cfg.hash_every = req.hash_every > 0 ? (unsigned) req.hash_every : 240;
	cfg.wait_ms = p->wait_ms;
	if (req.input_copies > 0) cfg.redundancy = (unsigned) req.input_copies;

	/* ---- the socket ---- */
	lan_config c;
	memset(&c, 0, sizeof c);
	c.mode = p->host ? LAN_MODE_HOST : LAN_MODE_JOIN;
	c.port = (uint16_t) req.local_port;
	c.port_tries = req.local_port ? 1 : 0;
	c.flags = LAN_F_IO_KEEPALIVE | LAN_F_NO_DISCOVERY | (req.loopback ? LAN_F_LOOPBACK : 0);
	uint32_t lip = 0;
	if (!req.local_ip.empty()) { uint16_t dummy; if (!parse_addr(req.local_ip, &lip, &dummy, 0)) lip = 0; }
	if (!lip && env.local_ip_be) lip = env.local_ip_be();
	c.local_ip_be = lip;
	uint32_t pip = 0;
	uint16_t pport = 0;
	if (req.mode == "lan" && !p->host && parse_addr(req.peer, &pip, &pport, LAN_PORT_DEFAULT)) {
		c.target_ip_be = pip;
		c.target_port = pport;
	}
	if (!req.sim.empty()) {
		uint32_t sip = 0;
		uint16_t sport = 0;
		if (parse_addr(req.sim, &sip, &sport, 0) && sport) {
			c.sim_ip_be = sip;
			c.sim_port = sport;
			c.sim_id = (uint8_t) req.sim_id;
			c.sim_lan_port = (uint16_t) (req.sim_lan_port ? req.sim_lan_port : (req.local_port ? req.local_port : LAN_PORT_DEFAULT));
		}
	}
	p->lan = lan_transport_create(&c);
	if (!p->lan && req.local_port) {
		/* the app's socket should be gone with the app; if the system still holds the port, any port works for a guest
		 * (the host learns it from the HELLO) and for a punch the NAT mapping is new anyway */
		p->log("local port %d is busy (%s): trying any", req.local_port, lan_last_error());
		c.port = 0;
		c.port_tries = 0;
		p->lan = lan_transport_create(&c);
	}
	if (!p->lan) return die(NF_NO_NETWORK, lan_last_error());
	if (req.loss | req.latency | req.jitter)
		lan_transport_set_fault(p->lan, req.loss, 0, req.latency, req.jitter, (uint32_t) p->t_open | 1u);
	{
		uint8_t seed[16];
		if (env.random) env.random(seed, 16);
		else for (int i = 0; i < 16; i++) seed[i] = (uint8_t) (p->t_open >> (i % 8) * 3);
		lan_transport_nat_seed(p->lan, seed);
	}
	if (lan_transport_bulk_open(p->lan, 128, kMagic) != 0) return die(NF_NO_NETWORK, "the datagram lane");
	lan_transport_bulk_any(p->lan, 1);     /* pk_link filters by address itself, and knows the peer only after the path forms */
	{
		char addr[40];
		lan_transport_addr_str(p->lan, addr, sizeof addr);
		p->log("link socket %s role=%s mode=%s", addr, p->host ? "host" : "guest", req.mode.c_str());
	}
	p->wire.t = p->lan;
	p->wire.env = env;
	lan_transport_iface(p->lan, &p->wire.tp);
	p->wire.have_tp = true;

	/* ---- the session core (created now; it speaks once the other console's address is known) ---- */
	if (!p->core.open(cfg, m, &p->wire)) return die(NF_START, "the session");
	p->core_open = true;

	/* ---- how the other console is reached ---- */
	p->t_phase = env.now_us();
	if (req.mode == "punch") {
		uint8_t key[8];
		if (req.punch_key.size() != 16) return die(NF_NO_CONNECT, "punch key");
		hex_bytes(req.punch_key, key);
		uint32_t ips[2];
		uint16_t ports[2];
		int n = 0;
		if (parse_addr(req.peer, &ips[n], &ports[n], 0) && ports[n]) n++;
		if (parse_addr(req.peer_lan, &ips[n], &ports[n], 0) && ports[n]) n++;
		if (!n) return die(NF_NO_CONNECT, "no candidates");
		const unsigned tms = p->wait_ms > 60000 ? 60000 : p->wait_ms;
		if (lan_transport_punch_arm(p->lan, key) != 0 || lan_transport_punch_start(p->lan, ips, ports, n, tms) != 0)
			return die(NF_NO_CONNECT, "punch start");
		p->log("punch started: %d candidate(s), %u ms", n, tms);
		phase_ = NP_PUNCH;
	} else if (req.mode == "relay") {
		uint32_t sip = 0;
		uint16_t dummy = 0, ports[3];
		int np = 0;
		if (!parse_addr(req.relay_server, &sip, &dummy, 0) && lan_parse_addr(req.relay_server.c_str(), &sip, &dummy, 0) != 0) sip = 0;
		const char *s = req.relay_ports.c_str();
		while (*s && np < 3) {
			const int v = atoi(s);
			if (v > 0 && v < 65536) ports[np++] = (uint16_t) v;
			while (*s && *s != ',') s++;
			if (*s == ',') s++;
		}
		if (!sip || !np || !parse_addr(req.relay_peer, &p->peer_ip_be, &p->peer_port, 0)) return die(NF_RELAY, "relay credentials");
		if (!req.relay_addr.empty()) {
			/* The app made the allocation and let go without releasing it; this process has the same local port, so the
			 * server still knows us by that 5-tuple: adopt it (Refresh, permission, ChannelBind), no new Allocate. */
			lan_relay_params prm;
			memset(&prm, 0, sizeof prm);
			prm.server_ip_be = sip;
			prm.server_port = ports[0];
			snprintf(prm.user, sizeof prm.user, "%s", req.relay_user.c_str());
			snprintf(prm.pass, sizeof prm.pass, "%s", req.relay_pass.c_str());
			prm.peer_ip_be = p->peer_ip_be;
			prm.peer_port = p->peer_port;
			if (!parse_addr(req.relay_addr, &prm.relay_ip_be, &prm.relay_port, 0) || lan_transport_relay_adopt(p->lan, &prm) != 0)
				return die(NF_RELAY, "relay adopt");
			p->log("relay allocation adopted (no Allocate sent)");
		} else {
			if (lan_transport_relay_start(p->lan, sip, ports, np, req.relay_user.c_str(), req.relay_pass.c_str()) != 0)
				return die(NF_RELAY, "relay start");
			p->log("relay allocation started");
		}
		phase_ = NP_RELAY;
	} else {
		/* Same Wi-Fi / direct: the host just listens, the guest asks the host's address */
		LinkAddr a;
		if (!p->host) {
			if (!pip || !pport) return die(NF_NO_CONNECT, "the host's address");
			a.ip_be = pip;
			a.port = pport;
			p->core.set_peer(a);
			p->peer_set = true;
		}
		phase_ = NP_NET;
	}
	return true;
}

static void set_peer(NetLink::Impl *p, uint32_t ip, uint16_t port)
{
	LinkAddr a;
	a.ip_be = ip;
	a.port = port;
	if (!p->host) lan_transport_set_target(p->lan, ip, port);     /* the I/O thread's keepalives and heard-age start here */
	p->core.set_peer(a);
	p->peer_set = true;
}

int NetLink::step(uint16_t pad)
{
	Impl *p = p_;
	if (phase_ == NP_IDLE) return LS_WAIT;
	if (over()) return LS_OVER;
	const uint64_t now = p->env.now_us();

	/* the path is not open yet */
	if (phase_ == NP_PUNCH || phase_ == NP_RELAY) {
		lan_nat_status ns;
		lan_transport_nat_status(p->lan, &ns);
		if (phase_ == NP_PUNCH) {
			if (ns.punch == LAN_PUNCH_DONE) {
				p->log("punch done: peer %s at %08x:%u after %u ms", ns.peer_is_lan ? "on our network" : "through the NAT",
				       (unsigned) ns.peer_ip_be, (unsigned) ns.peer_port, (unsigned) ((now - p->t_phase) / 1000));
				set_peer(p, ns.peer_ip_be, ns.peer_port);
				phase_ = NP_NET;
			} else if (ns.punch == LAN_PUNCH_TIMEOUT) {
				p->log("punch timed out (tx %u rx %u ack %u foreign %u)", ns.punch_tx, ns.punch_rx, ns.ack_rx, ns.nat_foreign);
				fail_ = NF_NO_CONNECT;
				phase_ = NP_FAILED;
				return LS_OVER;
			}
		} else {
			if (ns.relay == LAN_RELAY_ALLOCATED && !p->relay_bound) {
				p->relay_bound = 1;
				p->log("relay allocated %08x:%u; binding the peer's relayed address", (unsigned) ns.relay_ip_be, (unsigned) ns.relay_port);
				lan_transport_relay_bind(p->lan, p->peer_ip_be, p->peer_port);
			} else if (ns.relay == LAN_RELAY_READY) {
				p->log("relay ready after %u ms", (unsigned) ((now - p->t_phase) / 1000));
				set_peer(p, p->peer_ip_be, p->peer_port);
				phase_ = NP_NET;
			} else if (ns.relay == LAN_RELAY_FAILED) {
				p->log("relay failed err=%d", ns.relay_err);
				fail_ = NF_RELAY;
				phase_ = NP_FAILED;
				return LS_OVER;
			}
		}
		if (phase_ != NP_NET) {
			if (now - p->t_open > (uint64_t) p->wait_ms * 1000u) {
				p->log("nobody answered in %u s", p->wait_ms / 1000);
				fail_ = phase_ == NP_RELAY ? NF_RELAY : NF_NO_CONNECT;
				phase_ = NP_FAILED;
				return LS_OVER;
			}
			return LS_WAIT;
		}
	}

	/* the session */
	const int r = p->core.step(pad);
	switch (p->core.phase()) {
	case LP_WAIT_PEER: phase_ = NP_NET; break;
	case LP_STATE: case LP_START: phase_ = NP_SESSION; break;
	case LP_RUN: phase_ = NP_RUNNING; break;
	case LP_ENDING: phase_ = NP_ENDING; break;
	case LP_DONE: phase_ = NP_DONE; break;
	case LP_FAILED: {
		const int e = p->core.error();
		const int pr = p->core.peer_reason();
		fail_ = map_error(e == LE_PEER_ABORT && pr >= 0 ? pr : e);
		phase_ = NP_FAILED;
		break;
	}
	default: break;
	}
	if (over()) return LS_OVER;
	return r;
}

void NetLink::request_end()
{
	if (phase_ != NP_IDLE && !over()) p_->core.request_end();
}

void NetLink::suspend()
{
	if (phase_ == NP_IDLE || over()) return;
	p_->core.suspend();
	fail_ = NF_LOST;
	phase_ = NP_FAILED;
}

void NetLink::status(std::string *l1, std::string *l2, int *p1, int *p2)
{
	Impl *p = p_;
	std::string a, b;
	int q1 = -1, q2 = -1;
	switch (phase_) {
	case NP_NET: a = p->host ? "Waiting for player 2" : "Reaching player 1"; break;
	case NP_PUNCH: a = "Connecting over the Internet"; b = "Opening a path to the other console"; break;
	case NP_RELAY: a = "Connecting through the relay"; break;
	case NP_SESSION:
		if (p->core.phase() == LP_STATE) {
			a = p->host ? "Sending the game to player 2" : "Receiving the game from player 1";
			q1 = p->core.progress();
		} else a = "Starting the game together";
		break;
	case NP_RUNNING: case NP_ENDING: a = phase_ == NP_ENDING ? "Ending the link together" : "Playing together"; break;
	case NP_DONE: a = "Link ended"; break;
	case NP_FAILED: a = "The link failed"; b = link_error_text(); break;
	default: break;
	}
	if (l1) *l1 = a;
	if (l2) *l2 = b;
	if (p1) *p1 = q1;
	if (p2) *p2 = q2;
}

int NetLink::link_error() const { return p_->core_open ? p_->core.error() : LE_OK; }
int NetLink::peer_reason() const { return p_->core_open ? p_->core.peer_reason() : -1; }
const char *NetLink::link_error_name() const { return pk::link_error_name(link_error()); }
const char *NetLink::link_error_text() const { return pk::link_error_text(link_error() == LE_PEER_ABORT && peer_reason() >= 0 ? peer_reason() : link_error()); }
uint32_t NetLink::frames() const { return p_->core_open ? p_->core.frame() : 0; }
unsigned NetLink::delay() const { return p_->core_open ? p_->core.delay() : 0; }
bool NetLink::restored_ok() const { return p_->core_open ? p_->core.restored_ok() : true; }
LinkStats NetLink::stats() const { return p_->core_open ? p_->core.stats() : LinkStats(); }
const char *NetLink::path() const { return p_->lan ? p_->wire.path() : "lan"; }

std::string NetLink::summary() const
{
	std::string s = p_->core_open ? p_->core.summary() : std::string("link not started");
	char b[64];
	snprintf(b, sizeof b, " mode=%s path=%s", p_->mode.c_str(), path());
	return s + b;
}

void NetLink::abandon()
{
	Impl *p = p_;
	if (p->lan) { lan_transport_destroy(p->lan); p->lan = 0; }
	p->wire.t = 0;
	p->core_open = false;
	if (phase_ != NP_IDLE && !over()) { phase_ = NP_FAILED; fail_ = NF_LOCAL; }
}

void NetLink::close()
{
	Impl *p = p_;
	if (p->core_open) {
		/* the guest's own game is put back; BYE goes out first */
		p->core.close();
		if (phase_ != NP_IDLE && !over()) { phase_ = NP_FAILED; if (fail_ == NF_NONE) fail_ = NF_LOCAL; }
		else if (phase_ == NP_FAILED && fail_ == NF_NONE) fail_ = map_error(p->core.error());
	}
	if (p->lan) { lan_transport_relay_stop(p->lan); lan_transport_destroy(p->lan); p->lan = 0; }
	p->wire.t = 0;
}

}  // namespace pk
