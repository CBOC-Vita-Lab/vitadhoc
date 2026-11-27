/*
 * pk_link_net: pk_link over the UDP transport of the project (gpsp/netdrv/transport_lan.c and its nat_* layer, the
 * owner's own code, MIT OR GPL-2.0-or-later): forms the path the app asked for (the same Wi-Fi, a punched Internet
 * path, a Cloudflare TURN relay, taken over from the app's allocation) and runs the session on it.
 *
 * The app forms the link from the library entry and writes it to the launch file (pk::LinkRequest, `link.*`, see
 * playerkit/FORMATS.md). A player is another program and sockets do not survive the change, so this class makes the
 * connection again: it binds the app's local port, punches again with the same key or adopts the relay allocation, and
 * then runs pk::Link on the transport's bulk lane. Nothing of netdrv's session layer is used: no netdrv.c, no
 * fe_gblink.c, no fe_evt.c, no fe_util.c.
 *
 * Build: this file needs gpsp/netdrv on the include path and transport_lan.c, nat_hash.c, nat_stun.c, nat_turn.c in
 * the program (see snes/vita/netplay.mk).
 *
 * Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE).
 */
#ifndef PK_LINK_NET_H
#define PK_LINK_NET_H

#include "pk_launch.h"
#include "pk_link.h"

#include <stdint.h>
#include <string>

namespace pk {

struct NetEnv {
	uint64_t (*now_us)() = 0;                   /* monotonic microseconds */
	void (*random)(uint8_t *out, int n) = 0;    /* real randomness (NAT transaction ids) */
	void (*log)(const char *line) = 0;          /* one line, no newline; may be NULL */
	uint32_t (*local_ip_be)() = 0;              /* the Wi-Fi address, network order (0 = not known); may be NULL */
};

enum NetPhase {
	NP_IDLE = 0,       /* no link in this launch */
	NP_NET,            /* the transport is up; the other console has not answered */
	NP_PUNCH,          /* Online: hole punching */
	NP_RELAY,          /* Online: waiting for the relay allocation / permission */
	NP_SESSION,        /* the session is negotiating: the game's state crosses */
	NP_RUNNING,        /* both machines run */
	NP_ENDING,         /* an end was asked; running to the agreed frame */
	NP_DONE,
	NP_FAILED
};

enum NetFail {
	NF_NONE = 0,
	NF_NO_NETWORK,     /* the socket could not be opened */
	NF_NO_CONNECT,     /* nobody answered in time (punch timed out, no peer) */
	NF_RELAY,          /* the relay refused or timed out */
	NF_GAME,           /* the other console plays another game */
	NF_VERSION,        /* the other console's build, core or options differ */
	NF_LOST,           /* the other console went away (or this one went to sleep) */
	NF_DESYNC,         /* the two machines diverged */
	NF_LOCAL,          /* ended by the player before it started */
	NF_START,          /* the machine would not start (memory, state) */
	NF_OTHER
};

class NetLink {
public:
	NetLink();
	~NetLink();
	/* Start. `rom_file` holds exactly the image the machine runs (the session's game identity: sizes and CRCs of it).
	 * `core_tag` is this build's core, version and pinned options (a player hashes them; both consoles must agree).
	 * `frame_us` is one emulated frame (PAL: 20000). false: the link could not start (failure() says why). */
	bool open(const LinkRequest &req, const std::string &rom_file, uint32_t core_tag, const LinkMachine &m, const NetEnv &env,
	          uint32_t frame_us = 16667);
	/* Once per display pass. Returns a LinkStep (LS_*). */
	int step(uint16_t pad);
	void request_end();
	void suspend();
	void close();
	/* Drop the socket with no BYE: a crash, a battery pull (the tests). The other console sees silence. */
	void abandon();

	NetPhase phase() const { return phase_; }
	bool running() const { return phase_ == NP_RUNNING; }
	bool playing() const { return phase_ == NP_RUNNING || phase_ == NP_ENDING; }
	bool over() const { return phase_ == NP_DONE || phase_ == NP_FAILED; }
	bool ok() const { return phase_ == NP_DONE; }
	bool is_host() const { return host_; }
	NetFail failure() const { return fail_; }
	int link_error() const;                    /* pk::LinkError of this console */
	int peer_reason() const;                   /* the other console's, when it ended it, else -1 */
	const char *link_error_name() const;
	const char *link_error_text() const;
	/* Two short lines for the connecting screen, and their progress (per mille, -1 = none). */
	void status(std::string *line1, std::string *line2, int *p1, int *p2);
	uint32_t frames() const;
	unsigned delay() const;
	bool restored_ok() const;
	LinkStats stats() const;
	std::string summary() const;
	/* the path in use: "lan", "direct", "punch" or "relay" */
	const char *path() const;

	struct Impl;
private:
	Impl *p_;
	NetPhase phase_ = NP_IDLE;
	NetFail fail_ = NF_NONE;
	bool host_ = false;
	NetLink(const NetLink &);
	NetLink &operator=(const NetLink &);
};

}  // namespace pk

#endif
