/*
 * Two-player Mega Drive / Master System / SG-1000 over a link (ONLINE-2.0 plan).
 *
 * The main app forms the link from the library entry (a Same Wi-Fi room, a punched Online room or a relay), writes it
 * into launch/<console>.ini (pk::LinkRequest, `link.*`) and starts this program. Sockets do not survive the program change,
 * so the player makes the connection again: it binds the same local port, punches again with the same key (or adopts
 * the relay allocation with the same credentials) and runs playerkit's pk_link: ONE console, the host's buttons on pad 1
 * and the guest's on pad 2, advanced one frame only when both players' buttons for that frame are known. Only buttons
 * cross the network (and the host's save state once, at the start).
 *
 * This file is only the Sega side of it: the machine (Genesis Plus GX's state, the cartridge's save RAM, the frame). The session, the path
 * and the transport are playerkit's pk_link / pk_link_net (MIT) and the project's own UDP transport and NAT layer
 * (gpsp/netdrv/transport_lan.c, nat_*.c: owner's code, MIT OR GPL-2.0-or-later); nothing of the main app's Game Boy
 * link (netdrv.c, fe_gblink.c, fe_evt.c, fe_util.c) is linked here. The list is snes/vita/netplay.mk (md/vita/Makefile includes it).
 *
 * The code here is portable (BSD sockets through the transport): the Vita program and the Linux twin
 * (md/twin/link_twin.cpp) run exactly this.
 *
 * Threading: every call from the one thread that runs the game (the transport has its own I/O thread below).
 *
 * Copyright CBOC-Vita. MIT licence for this file (see md/frontend/LICENSE).
 */
#ifndef MD_NETPLAY_H
#define MD_NETPLAY_H

#include <stdint.h>
#include <stddef.h>
#include <string>

#include "pk_launch.h"
#include "pk_link_net.h"

namespace mdnet {

/* The pieces of the platform the link needs. */
struct Env {
	uint64_t (*now_us)();                       /* monotonic microseconds */
	void (*random)(uint8_t *out, int n);        /* real randomness (nat transaction ids, the join nonce) */
	void (*log)(const char *line);              /* one line, no newline; may be NULL */
	/* the Wi-Fi address as the system knows it, host or network order as `lan_config.local_ip_be` (0 = not known) */
	uint32_t (*local_ip_be)();
	/* The host's battery image once both consoles agreed on the end (the player writes the save file, safely). 0 = written.
	 * NULL: nothing to write. */
	int (*commit_save)(const uint8_t *data, size_t n);
};

enum Phase {
	PH_IDLE = 0,       /* no link in this launch */
	PH_NET,            /* the transport is up; the other console has not been reached yet */
	PH_PUNCH,          /* Online: hole punching */
	PH_RELAY,          /* Online: waiting for the relay allocation / permission */
	PH_SESSION,        /* the session is negotiating: the host's game state crosses */
	PH_RUNNING,        /* both machines run */
	PH_ENDING,         /* an end was asked; hashes are being compared */
	PH_DONE,           /* agreed end; the host's save is written */
	PH_FAILED          /* see failure(); nothing was written */
};

/* What step() tells the loop. */
enum StepResult {
	STEP_WAIT = 0,     /* not running: no frame was emulated (set-up, ending) */
	STEP_FRAME = 1,    /* one frame was emulated: take the picture and sound */
	STEP_STALL = 2,    /* the other player's buttons are late: no frame (the loop just tries again next pass) */
	STEP_OVER = 3      /* DONE or FAILED: leave the player */
};

/* The ways a link fails that the app has words for (the UI's UI_FAIL_* are the app's; these name the cause). */
enum Fail {
	F_NONE = 0,
	F_NO_NETWORK,      /* the socket could not be opened */
	F_NO_CONNECT,      /* nobody answered in time (punch timed out, relay unavailable, no peer) */
	F_RELAY,           /* the relay refused or timed out */
	F_GAME,            /* the other console plays another game (a different ROM image) */
	F_VERSION,         /* the other console's build or core options differ */
	F_LOST,            /* the other console went away */
	F_DESYNC,          /* the two machines diverged (never saved) */
	F_LOCAL,           /* ended by the player before it started */
	F_START,           /* the machine would not start (memory, state) */
	F_OTHER
};

class Link {
public:
	Link();
	~Link();

	/* A link was asked for. */
	static bool wanted(const pk::LinkRequest &r) { return r.active(); }

	/* Start. `rom_file` is a file holding exactly the image the machine runs (identity for the session: size and
	 * checksums of it; the player writes one when the loaded image is a patched or headerless copy). false: the link
	 * could not start (failure() says why) and the player should leave. */
	bool open(const pk::LinkRequest &req, const std::string &rom_file, const Env &env);
	/* Once per display pass. `buttons`: the local player's pad (the 12 bit mask of host.h, as md_host_set_pad takes).
	 * Runs at most one emulated frame. */
	int step(uint16_t buttons);
	/* Ask for a clean end (the menu's Disconnect): both consoles stop on the same frame. */
	void request_end();
	/* This console is going to sleep: tell the other and end (the player leaves with a plain notice). */
	void suspend();
	/* Tear down (idempotent): the session, the relay allocation, the socket. */
	void close();

	Phase phase() const;
	bool running() const { return phase() == PH_RUNNING; }
	/* frames are being emulated (RUNNING, or ENDING until the agreed end frame) */
	bool playing() const { return phase() == PH_RUNNING || phase() == PH_ENDING; }
	bool over() const { return phase() == PH_DONE || phase() == PH_FAILED; }
	bool ok() const { return phase() == PH_DONE; }
	bool is_host() const;
	Fail failure() const;
	/* the numbers and names of session.txt's link_error / link_error_name (pk::LinkError): this console's, and the other
	 * console's when it was the one that ended it (peer_reason, else -1) */
	int gblink_error() const;
	int peer_reason() const;
	const char *gblink_error_name() const;
	/* a plain sentence for the player: why the link ended */
	const char *error_text() const;
	/* Two short lines for the connecting screen, and their progress (per mille, -1 = none). */
	void status(std::string *line1, std::string *line2, int *p1, int *p2);
	uint32_t frames() const;
	unsigned delay() const;
	/* the restored machine (guest) or kept machine (host) is in order; false: the player must reload its game */
	bool restored_ok() const;

	/* Counters for the log line / tests. */
	struct Stats {
		uint32_t stalls, stall_episodes, hashes_sent, hashes_matched, frames;
		unsigned delay;
		uint32_t nd_retx, nd_rx_dup;
	};
	void stats(Stats *out);
	/* the one-line summary written at the end */
	std::string summary();
	struct Impl;
private:
	Impl *p_;
	Link(const Link &);
	Link &operator=(const Link &);
};

}  // namespace mdnet

#endif
