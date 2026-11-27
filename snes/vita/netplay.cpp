/* See netplay.h. Copyright CBOC-Vita. MIT licence for this file (see snes/frontend/LICENSE). */
#include "netplay.h"

#include <stdio.h>
#include <string.h>

#include "host.h"
#include "pk_util.h"

namespace snesnet {

/* ---- the one machine: the SNES core as a pk::LinkMachine ------------------------------------------------------- */

/* The state of a SNES game is a fixed size for the game (Snes9x freezes every block at its full size) and holds the
 * battery RAM, the clock-less machine and the RAM: loading the host's state on both consoles is the whole "start
 * together" (the host loads the same bytes back too: pk_link). */
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

/* A cheap fingerprint of the battery RAM: the host's agreed end waits while it keeps changing */
static uint32_t m_cart_crc(void *)
{
	const size_t n = snes_host_sram_size();
	return n ? pk::crc32(0, snes_host_sram_data(), n) : 0;
}

/* What must be equal on both consoles besides the pairing's compat word: the core and everything pinned that changes
 * what the machine computes. The SNES player pins: one machine with two pads and nothing else (no multitap), every
 * frame drawn at full width (the picture work leaves traces in the machine's state), the Super FX at full speed (its
 * adaptive clock is off while linked), the core's worker threads are bit-exact and may differ. */
static uint32_t core_tag()
{
	static const char kPinned[] = "snes9x vitadhoc-2.0 pk_link-1 pads=2 multitap=off render=every-frame wide=off superfx=100 audio=free";
	uint32_t h = 2166136261u;
	for (size_t i = 0; i < sizeof kPinned - 1; i++) { h ^= (uint8_t) kPinned[i]; h *= 16777619u; }
	return h;
}

/* ---- the link ---------------------------------------------------------------------------------------------------- */

struct Link::Impl {
	pk::NetLink net;
	bool host = false, committed = false, commit_failed = false;
	std::string rom_file;
	Env env;
	void (*log)(const char *) = 0;
};

Link::Link() : p_(new Impl) {}
Link::~Link() { close(); delete p_; }

Phase Link::phase() const
{
	if (p_->commit_failed) return PH_FAILED;
	return (Phase) p_->net.phase();
}

bool Link::is_host() const { return p_->net.is_host(); }
Fail Link::failure() const { return p_->commit_failed ? F_OTHER : (Fail) p_->net.failure(); }
int Link::gblink_error() const { return p_->commit_failed ? pk::LE_COMMIT : p_->net.link_error(); }
int Link::peer_reason() const { return p_->net.peer_reason(); }
const char *Link::gblink_error_name() const { return pk::link_error_name(gblink_error()); }
const char *Link::error_text() const { return p_->commit_failed ? pk::link_error_text(pk::LE_COMMIT) : p_->net.link_error_text(); }
uint32_t Link::frames() const { return p_->net.frames(); }
unsigned Link::delay() const { return p_->net.delay(); }
bool Link::restored_ok() const { return p_->net.restored_ok(); }
void Link::request_end() { p_->net.request_end(); }
void Link::suspend() { p_->net.suspend(); }
void Link::status(std::string *l1, std::string *l2, int *p1, int *p2) { p_->net.status(l1, l2, p1, p2); }
std::string Link::summary() { return p_->net.summary(); }

bool Link::open(const pk::LinkRequest &req, const std::string &rom_file, const Env &env)
{
	Impl *p = p_;
	p->host = req.is_host();
	p->rom_file = rom_file;
	p->env = env;
	p->log = env.log;
	pk::LinkMachine m;
	m.user = p;
	m.run_frame = m_run_frame;
	m.state_size = m_state_size;
	m.state_save = m_state_save;
	m.state_load = m_state_load;
	m.cart_crc = m_cart_crc;
	pk::NetEnv e;
	e.now_us = env.now_us;
	e.random = env.random;
	e.log = env.log;
	e.local_ip_be = env.local_ip_be;
	return p->net.open(req, rom_file, core_tag(), m, e, snes_host_is_pal() ? 20000u : 16667u);
}

int Link::step(uint16_t buttons)
{
	Impl *p = p_;
	const int r = p->net.step(buttons);
	/* the host's battery image, written once the two consoles agreed on the end (the final hashes were equal). The guest
	 * never commits: its own game is put back, nothing the link computed is kept. */
	if (p->net.ok() && p->host && !p->committed) {
		p->committed = true;
		const size_t n = snes_host_sram_size();
		if (n) {
			const int rc = snes_host_sram_write(snes_host_sram_data(), n);
			if (p->log) { char b[96]; snprintf(b, sizeof b, "host save committed: %u bytes rc=%d", (unsigned) n, rc); p->log(b); }
			if (rc != 0) p->commit_failed = true;
		}
	}
	if (p->commit_failed) return STEP_OVER;
	return r;
}

void Link::stats(Stats *out)
{
	memset(out, 0, sizeof *out);
	const pk::LinkStats s = p_->net.stats();
	out->stalls = s.stalls;
	out->stall_episodes = s.stall_episodes;
	out->hashes_sent = s.hashes_sent;
	out->hashes_matched = s.hashes_matched;
	out->frames = s.frames;
	out->delay = s.delay;
	out->nd_retx = s.retx;
	out->nd_rx_dup = s.dups;
}

void Link::close() { p_->net.close(); }

}  // namespace snesnet
