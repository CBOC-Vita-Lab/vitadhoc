/* See netplay.h. Copyright CBOC-Vita. MIT licence for this file (see md/frontend/LICENSE). */
#include "netplay.h"

#include <stdio.h>
#include <string.h>
#include <vector>

#include "host.h"
#include "pk_util.h"

namespace mdnet {

/* ---- the one machine: the Sega core as a pk::LinkMachine --------------------------------------------------------- */

/* What crosses at the start is the core's state (a fixed size for the game) followed by the cartridge's save RAM: the core's
 * state does not hold the battery RAM, and a game that reads its save must see the host's on both consoles. Loading the
 * host's bytes on both consoles (the host loads them back too) is the whole "start together". */
static size_t sram_cap() { return md_host_sram_capacity(); }

static int m_run_frame(void *, uint16_t pad1, uint16_t pad2)
{
	md_host_set_pad(0, pad1 & 0xFFFu);
	md_host_set_pad(1, md_host_system() == MD_SYS_GG ? 0u : (unsigned) (pad2 & 0xFFFu));
	md_host_run_frame();
	return 0;
}

static size_t m_state_size(void *) { const size_t n = md_host_state_size(); return n ? n + sram_cap() : 0; }

static int m_state_save(void *, void *buf, size_t n)
{
	const size_t ms = md_host_state_size(), cap = sram_cap();
	if (!ms || n != ms + cap) return -1;
	uint8_t *b = (uint8_t *) buf;
	if (md_host_state_save_mem(b, ms) != ms) return -1;
	if (cap) {
		const size_t used = md_host_sram_read(b + ms, cap);
		if (used < cap) memset(b + ms + used, 0xFF, cap - used);
	}
	return 0;
}

static int m_state_load(void *, const void *buf, size_t n)
{
	const size_t ms = md_host_state_size(), cap = sram_cap();
	if (!ms || n != ms + cap) return -1;
	const uint8_t *b = (const uint8_t *) buf;
	if (md_host_state_load_mem(b, ms) != 0) return -1;
	if (cap) md_host_sram_write(b + ms, cap);
	return 0;
}

/* The fingerprint compared every few seconds: the core's state without its CRC trailer, and the save RAM. Nothing of the audio
 * output (its resampler memory is not part of the state: both consoles may differ there). */
static std::vector<uint8_t> g_scratch;
static uint32_t m_state_hash(void *)
{
	const size_t ms = md_host_state_size();
	if (!ms) return 0;
	if (g_scratch.size() < ms) g_scratch.resize(ms);
	if (md_host_state_save_quick(g_scratch.data(), g_scratch.size()) != ms) return 0;
	uint32_t h = pk::crc32(0, g_scratch.data(), ms);
	const size_t cap = sram_cap();
	if (cap) {
		if (g_scratch.size() < cap) g_scratch.resize(cap);
		const size_t used = md_host_sram_read(g_scratch.data(), cap);
		h = pk::crc32(h, g_scratch.data(), used);
	}
	return h;
}

/* A cheap fingerprint of the battery RAM: the host's agreed end waits while it keeps changing */
static uint32_t m_cart_crc(void *)
{
	const size_t cap = sram_cap();
	if (!cap) return 0;
	if (g_scratch.size() < cap) g_scratch.resize(cap);
	const size_t used = md_host_sram_read(g_scratch.data(), cap);
	return pk::crc32(0, g_scratch.data(), used);
}

/* What must be equal on both consoles besides the pairing's compat word: the core build, the system and the options that change what
 * a game can see (host.h: region, vdp_mode, pad type, ym2413, force_dtack, addr_error, sprite_limit) and the ones that change the
 * hardware being emulated (FM chip, interlace, overscan). The player pins them to the same values on both consoles while linked; this
 * hashes what is in force, so a difference is refused with a plain message and never becomes a desync. Sound level and filter options
 * do not change the machine and are not part of it. */
static uint32_t core_tag()
{
	static const char *const kOpts[] = { "fm", "ym2413", "opll_nuked", "force_dtack", "addr_error", "sprite_limit", "vdp_mode", "interlace",
	                                     "overscan", "gg_extra", "mono" };
	char b[512];
	int n = snprintf(b, sizeof b, "genesis-plus-gx vitadhoc-2.0 pk_link-1 pads=2 render=every-frame system=%d hw=%s region=%d pal=%d pad0=%d pad1=%d",
	                 md_host_system(), md_host_system_hw(), md_host_region(), md_host_is_pal(), md_host_pad_type(0), md_host_pad_type(1));
	for (size_t i = 0; i < sizeof kOpts / sizeof kOpts[0] && n > 0 && n < (int) sizeof b - 24; i++) {
		int v = -1;
		md_host_get_option(kOpts[i], &v);
		n += snprintf(b + n, sizeof b - (size_t) n, " %s=%d", kOpts[i], v);
	}
	uint32_t h = 2166136261u;
	for (const char *p = b; *p; p++) { h ^= (uint8_t) *p; h *= 16777619u; }
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
	m.state_hash = m_state_hash;
	pk::NetEnv e;
	e.now_us = env.now_us;
	e.random = env.random;
	e.log = env.log;
	e.local_ip_be = env.local_ip_be;
	return p->net.open(req, rom_file, core_tag(), m, e, md_host_is_pal() ? 20000u : 16667u);
}

int Link::step(uint16_t buttons)
{
	Impl *p = p_;
	const int r = p->net.step(buttons);
	/* the host's battery image, written once the two consoles agreed on the end (the final hashes were equal). The guest
	 * never commits: its own game is put back, nothing the link computed is kept. */
	if (p->net.ok() && p->host && !p->committed) {
		p->committed = true;
		const size_t n = md_host_sram_size();
		if (n && p->env.commit_save) {
			std::vector<uint8_t> img(n);
			img.resize(md_host_sram_read(img.data(), n));
			const int rc = p->env.commit_save(img.data(), img.size());
			if (p->log) { char b[96]; snprintf(b, sizeof b, "host save committed: %u bytes rc=%d", (unsigned) img.size(), rc); p->log(b); }
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

}  // namespace mdnet
