/* See netplay.h. Copyright CBOC-Vita. MIT licence for this file (see snes/frontend/LICENSE). */
#include "netplay.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

#include "host.h"

extern "C" {
#include "fe_gblink.h"
#include "fe_padpair.h"
#include "fe_util.h"
#include "netdrv.h"
#include "transport_lan.h"
}

namespace snesnet {

/* ---- the one machine: the SNES core as a fe_pad_machine ------------------------------------------------------- */

/* The state of a SNES game is a fixed size for the game (Snes9x freezes every block at its full size) and holds
 * the battery RAM, the clock-less machine and the RAM: loading the host's state on both consoles is the whole
 * "start together". fe_gblink caps a state at 1 MiB (STATE_MAX); a core that outgrows it is refused by the session
 * (the start fails), never truncated. */
static int m_run_frame(void *, uint16_t pad1, uint16_t pad2)
{
	snes_host_set_pad(0, pad1);
	snes_host_set_pad(1, pad2);
	/* no multitap, no pads 3 and 4: what the local player's other controllers would add must not reach one console only */
	snes_host_set_pad(2, 0);
	snes_host_set_pad(3, 0);
	snes_host_run_frame();
	return 0;
}

static size_t m_state_size(void *) { return snes_host_state_size(); }

static int m_state_save(void *, void *buf, size_t n)
{
	size_t len = 0;
	if (snes_host_state_save_buf((uint8_t *) buf, n, &len) != 0) return -1;
	return len == n ? 0 : -1;
}

static int m_state_load(void *, const void *buf, size_t n)
{
	return snes_host_state_load_buf((const uint8_t *) buf, n) == 0 ? 0 : -1;
}

static size_t m_save_size(void *) { return snes_host_sram_size(); }

static int m_save_read(void *, void *buf, size_t n)
{
	if (n != snes_host_sram_size()) return -1;
	if (n) memcpy(buf, snes_host_sram_data(), n);
	return 0;
}

/* The battery RAM is a block of the state (snapshot.cpp "SRA"), so the state load that comes first already put the
 * image in the machine. Here the image is only checked against it: a mismatch means the state did not carry it. */
static int m_save_write(void *, const void *buf, size_t n)
{
	if (n != snes_host_sram_size()) return -1;
	return n == 0 || memcmp(buf, snes_host_sram_data(), n) == 0 ? 0 : -1;
}

static uint32_t m_cart_crc(void *)
{
	size_t n = snes_host_sram_size();
	return n ? fe_crc32(0, snes_host_sram_data(), n) : 0;
}

/* ---- the link ---------------------------------------------------------------------------------------------------- */

enum { MAX_PEERS = 1 };

struct Link::Impl {
	Env env;
	pk::LinkRequest req;
	bool host = false;
	std::string rom_file;
	lan_transport *lan = 0;
	netdrv *nd = 0;
	fe_gblink *gbl = 0;
	const fe_gblink_machines *vt = 0;
	fe_gblink_platform plat;
	fe_gblink_local loc;
	nd_transport tp;
	std::vector<uint8_t> state, save;         /* the host's start state and battery image */
	uint64_t t_open = 0, t_phase = 0;
	unsigned wait_ms = 90000;
	int peer_up = 0;
	uint8_t peer_id = 0xFF;
	int started = 0;                          /* netdrv_host / netdrv_join was called */
	int relay_bound = 0;
	uint32_t peer_ip_be = 0;                  /* where the other console is (join target / relayed address) */
	uint16_t peer_port = 0;
	std::string why;                          /* the last failure's words, for the log */
	uint32_t waited_budget_us = 6500;         /* stall absorption: at most this much of a display frame */
	uint64_t last_log_us = 0;
	int bulk_open = 0;
	uint32_t lat_stalls = 0;
	uint8_t bulk_buf[1500];

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

/* netdrv's callbacks */
static void cb_deliver(void *u, const void *buf, size_t len, uint8_t src)
{
	Link::Impl *p = (Link::Impl *) u;
	(void) src;
	if (p->gbl) fe_gblink_receive(p->gbl, buf, len);
}

static void cb_started(void *u, uint8_t local_id)
{
	Link::Impl *p = (Link::Impl *) u;
	p->log("netdrv session started id=%u", local_id);
}

static int cb_connected(void *u, uint8_t id)
{
	Link::Impl *p = (Link::Impl *) u;
	if (p->peer_up) { p->log("netdrv: a second peer was refused (id %u)", id); return -1; }
	p->peer_up = 1;
	p->peer_id = id;
	if (p->gbl) fe_gblink_peer(p->gbl, 1);
	p->log("netdrv peer connected id=%u", id);
	return 0;
}

static void cb_disconnected(void *u, uint8_t id)
{
	Link::Impl *p = (Link::Impl *) u;
	if (!p->peer_up || id != p->peer_id) return;
	p->peer_up = 0;
	if (p->gbl) fe_gblink_peer(p->gbl, 0);
	p->log("netdrv peer disconnected id=%u", id);
}

static void cb_stopped(void *u, int reason)
{
	Link::Impl *p = (Link::Impl *) u;
	p->peer_up = 0;
	if (p->gbl) fe_gblink_peer(p->gbl, 0);
	p->log("netdrv session stopped reason=%d", reason);
}

/* fe_gblink's platform */
static int pl_send(void *u, const void *buf, size_t len)
{
	Link::Impl *p = (Link::Impl *) u;
	if (!p->nd || !p->peer_up) return -1;
	return netdrv_send(p->nd, ND_RELIABLE | ND_FLUSH_HINT, buf, len, p->peer_id);
}

/* the backlog the session may keep queued toward the partner (fe_host.c GBL_BULK_BACKLOG) */
static int pl_room(void *u)
{
	Link::Impl *p = (Link::Impl *) u;
	if (!p->nd || !p->peer_up) return 0;
	return 96 - netdrv_unacked(p->nd, p->peer_id);
}

static int pl_bulk_send(void *u, const void *buf, size_t len)
{
	Link::Impl *p = (Link::Impl *) u;
	uint8_t mac[6];
	if (!p->lan || !p->nd || !p->peer_up || netdrv_peer_mac(p->nd, p->peer_id, mac) != 0) return -1;
	return lan_transport_bulk_send(p->lan, mac, buf, len);
}

static void drain(Link::Impl *p)
{
	if (!p->gbl || !p->bulk_open) return;
	int st = fe_gblink_state(p->gbl);
	if (st != FE_GBLINK_SETUP && st != FE_GBLINK_RUNNING && st != FE_GBLINK_ENDING) return;
	int n, guard = 256;
	while (p->gbl && guard-- && (n = lan_transport_bulk_recv(p->lan, p->bulk_buf, sizeof p->bulk_buf)) > 0)
		fe_gblink_receive_bulk(p->gbl, p->bulk_buf, (size_t) n);
}

/* stall absorption: the partner's buttons are late; deliver what the network has and say whether this display frame
 * still has time for the rest of its work */
static int pl_wait(void *u, uint32_t waited_us)
{
	Link::Impl *p = (Link::Impl *) u;
	if (p->nd) netdrv_pump(p->nd, p->env.now_us());
	drain(p);
	return waited_us < p->waited_budget_us;
}

/* The host's battery image, written once the two consoles agreed on the end (fe_gblink only calls this after the final
 * hashes were equal). The guest never commits: its own game is put back, nothing the link computed is kept. */
static int pl_commit(void *u, const void *image, size_t size)
{
	Link::Impl *p = (Link::Impl *) u;
	if (!p->host) { p->log("commit_save ignored on the guest (%u bytes)", (unsigned) size); return 0; }
	if (size != snes_host_sram_size()) return -1;
	if (!size) return 0;
	int rc = snes_host_sram_write((const uint8_t *) image, size);
	p->log("host save committed: %u bytes rc=%d", (unsigned) size, rc);
	return rc == 0 ? 0 : -1;
}

static const char *kPhaseName[] = { "idle", "net", "punch", "relay", "session", "running", "ending", "done", "failed" };

Link::Link() : p_(new Impl) {}
Link::~Link() { close(); delete p_; }

static int parse_addr(const std::string &s, uint32_t *ip, uint16_t *port, uint16_t def)
{
	return !s.empty() && lan_parse_addr(s.c_str(), ip, port, def) == 0 && *ip != 0;
}

static void hex_bytes(const std::string &h, uint8_t out[8])
{
	for (int i = 0; i < 8; i++) {
		unsigned v = 0;
		for (int k = 0; k < 2; k++) {
			char c = h[(size_t) (2 * i + k)];
			v = v * 16 + (unsigned) (c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 0);
		}
		out[i] = (uint8_t) v;
	}
}

const char *Link::gblink_error_name() const { return fe_gblink_error_text(gerr_); }

bool Link::open(const pk::LinkRequest &req, const std::string &rom_file, const Env &env)
{
	Impl *p = p_;
	if (phase_ != PH_IDLE) return false;
	p->env = env;
	p->req = req;
	p->host = req.is_host();
	host_ = p->host;
	p->rom_file = rom_file;
	p->t_open = env.now_us();
	p->wait_ms = (unsigned) (req.wait_s ? req.wait_s : 90) * 1000u;
	fail_ = F_NONE;
	gerr_ = 0;

	/* ---- the machine ---- */
	fe_pad_machine m;
	memset(&m, 0, sizeof m);
	m.user = p;
	m.console = FE_CONSOLE_SNES;
	m.rom_min = 0x8000;
	m.rom_max = 12u << 20;
	m.save_max = 512u << 10;
	m.run_frame = m_run_frame;
	m.state_size = m_state_size;
	m.state_save = m_state_save;
	m.state_load = m_state_load;
	m.save_size = m_save_size;
	m.save_read = m_save_read;
	m.save_write = m_save_write;
	m.cart_crc = m_cart_crc;
	m.set_clock = 0;     /* Snes9x's S-RTC and SPC7110 clock read the console's own: see netplay.h / the report */
	p->vt = fe_padpair_machines(&m);
	if (!p->vt) { fail_ = F_START; p->why = "machine"; phase_ = PH_FAILED; return false; }

	/* the host's start state: the freshly loaded game, no frame run yet. The guest sends none. */
	if (p->host) {
		size_t sn = snes_host_state_size(), n = 0;
		if (!sn || sn > (1u << 20)) { fail_ = F_START; p->why = "state size"; phase_ = PH_FAILED; return false; }
		p->state.resize(sn);
		if (snes_host_state_save_buf(p->state.data(), sn, &n) != 0 || n != sn) { fail_ = F_START; p->why = "state save"; phase_ = PH_FAILED; return false; }
		size_t ss = snes_host_sram_size();
		p->save.assign(ss, 0);
		if (ss) memcpy(p->save.data(), snes_host_sram_data(), ss);
	}

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
		/* the app's socket should be gone with the app; if the system still holds the port, any port works for a
		 * guest (the host learns it from the JOIN) and for a punch the NAT mapping is new anyway */
		p->log("local port %d is busy (%s): trying any", req.local_port, lan_last_error());
		c.port = 0;
		c.port_tries = 0;
		p->lan = lan_transport_create(&c);
	}
	if (!p->lan) {
		p->why = lan_last_error();
		p->log("socket failed: %s", p->why.c_str());
		fail_ = F_NO_NETWORK;
		phase_ = PH_FAILED;
		return false;
	}
	if (req.loss | req.latency | req.jitter)
		lan_transport_set_fault(p->lan, req.loss, 0, req.latency, req.jitter, (uint32_t) p->t_open | 1u);
	{
		uint8_t seed[16];
		if (env.random) env.random(seed, 16);
		else for (int i = 0; i < 16; i++) seed[i] = (uint8_t) (p->t_open >> (i % 8) * 3);
		lan_transport_nat_seed(p->lan, seed);
	}
	char addr[40];
	lan_transport_addr_str(p->lan, addr, sizeof addr);
	p->log("link socket %s role=%s mode=%s", addr, p->host ? "host" : "guest", req.mode.c_str());

	/* ---- the session (created now, stepped once the other console is on the channel) ---- */
	memset(&p->plat, 0, sizeof p->plat);
	memset(&p->loc, 0, sizeof p->loc);
	p->plat.user = p;
	p->plat.send = pl_send;
	p->plat.max_payload = ND_MAX_PAYLOAD;
	p->plat.send_room = pl_room;
	p->plat.bulk_per_step = 32;
	p->plat.commit_save = pl_commit;
	p->plat.wait = pl_wait;
	p->plat.nack = req.nack >= 0 ? req.nack : 1;
	if (lan_transport_bulk_open(p->lan, 128, FE_GBLINK_BULK_MAGIC) == 0) {
		p->bulk_open = 1;
		p->plat.bulk_send = pl_bulk_send;
		p->plat.bulk_max = 1037;
		p->plat.input_copies = (unsigned) (req.input_copies >= 0 ? req.input_copies : 4);
	}
	p->loc.is_host = p->host;
	p->loc.rom_path = p->rom_file.c_str();
	p->loc.console = FE_CONSOLE_SNES;
	p->loc.save = p->host && !p->save.empty() ? p->save.data() : NULL;
	p->loc.save_size = p->host ? p->save.size() : 0;
	p->loc.state = p->host ? p->state.data() : NULL;
	p->loc.state_size = p->host ? p->state.size() : 0;
	p->loc.wallclock_now = 0;     /* nothing in the machine reads it (set_clock is NULL) */
	p->loc.audio_rate = SNES_AUDIO_RATE;
	p->loc.input_delay = req.delay ? (unsigned) req.delay : 4;
	p->loc.hash_interval = req.hash_every ? (unsigned) req.hash_every : 240;
	p->loc.machines = p->vt;
	p->gbl = fe_gblink_create(&p->plat, &p->loc);
	if (!p->gbl) { fail_ = F_START; p->why = "session"; phase_ = PH_FAILED; return false; }
	fe_gblink_set_clock(p->gbl, p->env.now_us);

	/* ---- netdrv, on the transport ---- */
	lan_transport_iface(p->lan, &p->tp);
	nd_callbacks cb;
	memset(&cb, 0, sizeof cb);
	cb.user = p;
	cb.deliver = cb_deliver;
	cb.session_started = cb_started;
	cb.peer_connected = cb_connected;
	cb.peer_disconnected = cb_disconnected;
	cb.session_stopped = cb_stopped;
	nd_config nc;
	memset(&nc, 0, sizeof nc);
	char proto[ND_PROTO_LEN];
	snprintf(proto, sizeof proto, "SNES pad v1 %s", req.compat.empty() ? "00000000" : req.compat.c_str());
	nc.protocol = proto;
	nc.nick = req.nick.empty() ? NULL : req.nick.c_str();
	uint8_t nonce[4];
	if (env.random) env.random(nonce, 4); else memset(nonce, 0x5A, 4);
	nc.join_nonce = (uint32_t) nonce[0] | (uint32_t) nonce[1] << 8 | (uint32_t) nonce[2] << 16 | (uint32_t) nonce[3] << 24 | 1u;
	/* the ARQ timers of the LAN and Internet profiles (net_vita.c net_vita_arq_profile): 30 / 30 ms */
	nc.rto_first_max_us = 30000;
	nc.retx_us = 30000;
	p->nd = netdrv_create(&p->tp, &cb, &nc);
	if (!p->nd) { fail_ = F_START; p->why = "netdrv"; phase_ = PH_FAILED; return false; }

	/* ---- how the other console is reached ---- */
	p->t_phase = env.now_us();
	if (req.mode == "punch") {
		uint8_t key[8];
		if (req.punch_key.size() != 16) { fail_ = F_NO_CONNECT; p->why = "punch key"; phase_ = PH_FAILED; return false; }
		hex_bytes(req.punch_key, key);
		uint32_t ips[2];
		uint16_t ports[2];
		int n = 0;
		if (parse_addr(req.peer, &ips[n], &ports[n], 0) && ports[n]) n++;
		if (parse_addr(req.peer_lan, &ips[n], &ports[n], 0) && ports[n]) n++;
		if (!n) { fail_ = F_NO_CONNECT; p->why = "no candidates"; phase_ = PH_FAILED; return false; }
		unsigned tms = p->wait_ms > 60000 ? 60000 : p->wait_ms;
		if (lan_transport_punch_arm(p->lan, key) != 0 || lan_transport_punch_start(p->lan, ips, ports, n, tms) != 0) {
			fail_ = F_NO_CONNECT; p->why = "punch start"; phase_ = PH_FAILED; return false;
		}
		p->log("punch started: %d candidate(s), %u ms", n, tms);
		phase_ = PH_PUNCH;
	} else if (req.mode == "relay") {
		uint32_t sip = 0;
		uint16_t dummy = 0, ports[3];
		int np = 0;
		if (!parse_addr(req.relay_server, &sip, &dummy, 0) && lan_parse_addr(req.relay_server.c_str(), &sip, &dummy, 0) != 0) sip = 0;
		const char *s = req.relay_ports.c_str();
		while (*s && np < 3) {
			int v = atoi(s);
			if (v > 0 && v < 65536) ports[np++] = (uint16_t) v;
			while (*s && *s != ',') s++;
			if (*s == ',') s++;
		}
		if (!sip || !np || !parse_addr(req.relay_peer, &p->peer_ip_be, &p->peer_port, 0)) {
			fail_ = F_RELAY; p->why = "relay credentials"; phase_ = PH_FAILED; return false;
		}
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
			if (!parse_addr(req.relay_addr, &prm.relay_ip_be, &prm.relay_port, 0) ||
			    lan_transport_relay_adopt(p->lan, &prm) != 0) {
				fail_ = F_RELAY; p->why = "relay adopt"; phase_ = PH_FAILED; return false;
			}
			p->log("relay allocation adopted (no Allocate sent)");
		} else {
			if (lan_transport_relay_start(p->lan, sip, ports, np, req.relay_user.c_str(), req.relay_pass.c_str()) != 0) {
				fail_ = F_RELAY; p->why = "relay start"; phase_ = PH_FAILED; return false;
			}
			p->log("relay allocation started");
		}
		phase_ = PH_RELAY;
	} else {
		/* Same Wi-Fi / direct: the host just listens, the guest's JOIN goes to the host's address */
		p->started = 1;
		if ((p->host ? netdrv_host(p->nd) : netdrv_join(p->nd)) != 0) { fail_ = F_START; p->why = "netdrv start"; phase_ = PH_FAILED; return false; }
		phase_ = PH_NET;
	}
	return true;
}

/* the session over a path that is open: netdrv starts, then the session steps */
static bool start_netdrv(Link::Impl *p, uint32_t peer_ip_be, uint16_t peer_port)
{
	if (p->started) return true;
	p->started = 1;
	if (!p->host && peer_ip_be && lan_transport_set_target(p->lan, peer_ip_be, peer_port) != 0) {
		p->log("set_target failed");
		return false;
	}
	return (p->host ? netdrv_host(p->nd) : netdrv_join(p->nd)) == 0;
}

static Fail map_error(int e)
{
	switch (e) {
	case FE_GBLINK_E_ROM_MISSING: case FE_GBLINK_E_ROM_BAD: return F_GAME;
	case FE_GBLINK_E_PROTO: return F_VERSION;
	case FE_GBLINK_E_PEER_LOST: case FE_GBLINK_E_PEER_ABORT: return F_LOST;
	case FE_GBLINK_E_DESYNC: return F_DESYNC;
	case FE_GBLINK_E_LOCAL: return F_LOCAL;
	case FE_GBLINK_E_NO_PEER: return F_NO_CONNECT;
	case FE_GBLINK_E_MEMORY: case FE_GBLINK_E_START: return F_START;
	default: return F_OTHER;
	}
}

int Link::step(uint16_t buttons)
{
	Impl *p = p_;
	if (phase_ == PH_IDLE) return STEP_WAIT;
	if (over()) return STEP_OVER;
	const uint64_t now = p->env.now_us();
	if (p->nd) netdrv_pump(p->nd, now);
	drain(p);

	/* not on the channel yet */
	if (phase_ == PH_PUNCH || phase_ == PH_RELAY || phase_ == PH_NET) {
		lan_nat_status ns;
		lan_transport_nat_status(p->lan, &ns);
		if (phase_ == PH_PUNCH) {
			if (ns.punch == LAN_PUNCH_DONE) {
				p->log("punch done: peer %s at %08x:%u after %u ms", ns.peer_is_lan ? "on our network" : "through the NAT",
				       (unsigned) ns.peer_ip_be, (unsigned) ns.peer_port, (unsigned) ((now - p->t_phase) / 1000));
				if (!start_netdrv(p, ns.peer_ip_be, ns.peer_port)) { fail_ = F_START; phase_ = PH_FAILED; return STEP_OVER; }
				phase_ = PH_NET;
			} else if (ns.punch == LAN_PUNCH_TIMEOUT) {
				p->log("punch timed out (tx %u rx %u ack %u foreign %u)", ns.punch_tx, ns.punch_rx, ns.ack_rx, ns.nat_foreign);
				fail_ = F_NO_CONNECT;
				phase_ = PH_FAILED;
				return STEP_OVER;
			}
		} else if (phase_ == PH_RELAY) {
			if (ns.relay == LAN_RELAY_ALLOCATED && !p->relay_bound) {
				p->relay_bound = 1;
				p->log("relay allocated %08x:%u; binding the peer's relayed address", (unsigned) ns.relay_ip_be, (unsigned) ns.relay_port);
				lan_transport_relay_bind(p->lan, p->peer_ip_be, p->peer_port);
			} else if (ns.relay == LAN_RELAY_READY) {
				p->log("relay ready after %u ms", (unsigned) ((now - p->t_phase) / 1000));
				if (!start_netdrv(p, p->peer_ip_be, p->peer_port)) { fail_ = F_START; phase_ = PH_FAILED; return STEP_OVER; }
				phase_ = PH_NET;
			} else if (ns.relay == LAN_RELAY_FAILED) {
				p->log("relay failed err=%d", ns.relay_err);
				fail_ = F_RELAY;
				phase_ = PH_FAILED;
				return STEP_OVER;
			}
		}
		if (p->peer_up) phase_ = PH_SESSION;
		else if (now - p->t_open > (uint64_t) p->wait_ms * 1000u) {
			p->log("nobody answered in %u s", p->wait_ms / 1000);
			fail_ = F_NO_CONNECT;
			phase_ = PH_FAILED;
			return STEP_OVER;
		}
		if (phase_ != PH_SESSION) return STEP_WAIT;
	}

	/* the session */
	if (!p->peer_up && (phase_ == PH_SESSION) && fe_gblink_state(p->gbl) == FE_GBLINK_SETUP) {
		/* the channel dropped before the session started: wait for it back until the limit */
		if (now - p->t_open > (uint64_t) p->wait_ms * 1000u) { fail_ = F_LOST; phase_ = PH_FAILED; return STEP_OVER; }
		return STEP_WAIT;
	}
	int r = fe_gblink_step(p->gbl, buttons);
	int st = fe_gblink_state(p->gbl);
	switch (st) {
	case FE_GBLINK_SETUP: phase_ = PH_SESSION; break;
	case FE_GBLINK_RUNNING: phase_ = PH_RUNNING; break;
	case FE_GBLINK_ENDING: phase_ = PH_ENDING; break;
	case FE_GBLINK_DONE: phase_ = PH_DONE; break;
	case FE_GBLINK_FAILED:
		gerr_ = fe_gblink_error(p->gbl);
		{
			fe_gblink_progress pr;
			fe_gblink_get_progress(p->gbl, &pr);
			peer_reason_ = pr.peer_reason;
		}
		fail_ = map_error(gerr_ == FE_GBLINK_E_PEER_ABORT && peer_reason_ >= 0 ? peer_reason_ : gerr_);
		phase_ = PH_FAILED;
		break;
	}
	if (over()) return STEP_OVER;
	if (r == FE_GBLINK_FRAME) { frames_++; return STEP_FRAME; }
	if (r == FE_GBLINK_STALL) { p->lat_stalls++; return STEP_STALL; }
	return STEP_WAIT;
}

void Link::request_end()
{
	if (p_->gbl && phase_ != PH_IDLE && !over()) fe_gblink_request_end(p_->gbl);
}

void Link::status(std::string *l1, std::string *l2, int *p1, int *p2)
{
	Impl *p = p_;
	char a[96] = "", b[96] = "";
	int q1 = -1, q2 = -1;
	switch (phase_) {
	case PH_NET: snprintf(a, sizeof a, p->host ? "Waiting for player 2" : "Reaching player 1"); break;
	case PH_PUNCH: snprintf(a, sizeof a, "Connecting over the Internet"); snprintf(b, sizeof b, "Opening a path to the other console"); break;
	case PH_RELAY: snprintf(a, sizeof a, "Connecting through the relay"); break;
	case PH_SESSION: case PH_RUNNING: case PH_ENDING:
		if (p->gbl) fe_gblink_status(p->gbl, a, sizeof a, b, sizeof b);
		if (p->gbl) fe_gblink_status_progress(p->gbl, &q1, &q2);
		break;
	case PH_DONE: snprintf(a, sizeof a, "Link ended"); break;
	case PH_FAILED: snprintf(a, sizeof a, "The link failed"); break;
	default: break;
	}
	if (l1) *l1 = a;
	if (l2) *l2 = b;
	if (p1) *p1 = q1;
	if (p2) *p2 = q2;
}

bool Link::restored_ok() const
{
	return p_->vt ? fe_padpair_restored(p_->vt) != 0 : true;
}

void Link::stats(Stats *out)
{
	memset(out, 0, sizeof *out);
	Impl *p = p_;
	if (p->gbl) {
		fe_gblink_stats s;
		fe_gblink_get_stats(p->gbl, &s);
		out->stalls = s.stalls;
		out->stall_episodes = s.stall_episodes;
		out->hashes_sent = s.hashes_sent;
		out->hashes_matched = s.hashes_matched;
		out->frames = (uint32_t) s.frames[0];
		out->delay = s.input_delay;
	}
	if (p->nd) {
		nd_stats ns;
		netdrv_get_stats(p->nd, &ns);
		out->nd_retx = ns.retx;
		out->nd_rx_dup = ns.rx_dup;
	}
}

std::string Link::summary()
{
	Stats s;
	stats(&s);
	char b[260];
	snprintf(b, sizeof b, "link %s role=%s phase=%s frames=%u delay=%u stalls=%u/%u hashes=%u/%u retx=%u dup=%u error=%s peer_reason=%d",
	         p_->req.mode.c_str(), host_ ? "host" : "guest", kPhaseName[phase_], s.frames, s.delay, s.stalls, s.stall_episodes,
	         s.hashes_matched, s.hashes_sent, s.nd_retx, s.nd_rx_dup, gerr_ ? fe_gblink_error_text(gerr_) : "none", peer_reason_);
	return b;
}

void Link::close()
{
	Impl *p = p_;
	if (p->gbl) {
		/* before the session is destroyed: it still knows what it ended as */
		if (gerr_ == 0 && fe_gblink_state(p->gbl) == FE_GBLINK_FAILED) gerr_ = fe_gblink_error(p->gbl);
		fe_gblink_destroy(p->gbl);      /* the machine stops: a guest's own game is put back */
		p->gbl = 0;
	}
	if (p->nd) { netdrv_leave(p->nd); netdrv_destroy(p->nd); p->nd = 0; }
	if (p->lan) { lan_transport_relay_stop(p->lan); lan_transport_destroy(p->lan); p->lan = 0; }
	if (phase_ != PH_IDLE && !over()) { phase_ = PH_FAILED; if (fail_ == F_NONE) fail_ = F_LOCAL; }
}

}  // namespace snesnet
