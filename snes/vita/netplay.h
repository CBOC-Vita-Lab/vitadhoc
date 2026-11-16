/*
 * Two-player SNES over a link (ONLINE-2.0 plan, milestone B3).
 *
 * The main app forms the link from the library entry (a Same Wi-Fi room, a punched Online room or a relay), writes it
 * into launch/snes.ini (pk::LinkRequest, `link.*`) and starts this program. Sockets do not survive the program change,
 * so the player makes the connection again: it binds the same local port, punches again with the same key (or
 * allocates on the relay again with the same credentials), starts netdrv on top and runs fe_gblink (the mirror
 * session of the GB/GBA link) with fe_padpair's machine: ONE SNES, the host's buttons on pad 1 and the guest's on
 * pad 2, advanced one frame only when both players' buttons for that frame are known. Only buttons cross the network.
 *
 * The code here is portable (BSD sockets through gpsp/netdrv/transport_lan.c): the Vita program and the Linux twin
 * (snes/twin/link_twin.cpp) run exactly this. Everything from gpsp/ it needs is listed in snes/vita/netplay.mk.
 *
 * Threading: every call from the one thread that runs the game (the transport has its own I/O thread below the
 * nd_transport line, as everywhere in this project).
 *
 * Copyright CBOC-Vita. MIT licence for this file (see snes/frontend/LICENSE).
 */
#ifndef SNES_NETPLAY_H
#define SNES_NETPLAY_H

#include <stdint.h>
#include <stddef.h>
#include <string>

#include "pk_launch.h"

namespace snesnet {

/* The pieces of the platform the link needs. */
struct Env {
	uint64_t (*now_us)();                       /* monotonic microseconds */
	void (*random)(uint8_t *out, int n);        /* real randomness (nat transaction ids, the join nonce) */
	void (*log)(const char *line);              /* one line, no newline; may be NULL */
	/* the Wi-Fi address as the system knows it, host or network order as `lan_config.local_ip_be` (0 = not known) */
	uint32_t (*local_ip_be)();
};

enum Phase {
	PH_IDLE = 0,       /* no link in this launch */
	PH_NET,            /* the transport is up; the other console has not been reached yet */
	PH_PUNCH,          /* Online: hole punching */
	PH_RELAY,          /* Online: waiting for the relay allocation / permission */
	PH_SESSION,        /* netdrv is up; the session is negotiating (fe_gblink SETUP) */
	PH_RUNNING,        /* both machines run */
	PH_ENDING,         /* an end was asked; hashes are being compared */
	PH_DONE,           /* agreed end; the host's save is written */
	PH_FAILED          /* see error(); nothing was written */
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
	F_GAME,            /* the other console plays another game (a different ROM image): FE_GBLINK_E_ROM_MISSING */
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

	/* Start. `rom_file` is a file holding exactly the image the machine runs (identity for the session: SHA-1 of
	 * it; the player writes one when the loaded image is a patched or headerless copy), `root` the app's data
	 * folder (scratch). false: the link could not start (error() says why) and the player should leave. */
	bool open(const pk::LinkRequest &req, const std::string &rom_file, const Env &env);
	/* Once per display pass. `buttons`: the local player's pad (the SNES mask of host.h, as snes_host_set_pad takes).
	 * Runs at most one emulated frame. */
	int step(uint16_t buttons);
	/* Ask for a clean end (the menu's Disconnect). */
	void request_end();
	/* Tear down (idempotent): the session, netdrv, the relay allocation, the socket. */
	void close();

	Phase phase() const { return phase_; }
	bool running() const { return phase_ == PH_RUNNING; }
	/* frames are being emulated (RUNNING, or ENDING until the agreed end frame) */
	bool playing() const { return phase_ == PH_RUNNING || phase_ == PH_ENDING; }
	bool over() const { return phase_ == PH_DONE || phase_ == PH_FAILED; }
	bool ok() const { return phase_ == PH_DONE; }
	bool is_host() const { return host_; }
	Fail failure() const { return fail_; }
	int gblink_error() const { return gerr_; }
	/* when the other console aborted: the reason it gave (an fe_gblink_error value, e.g. FE_GBLINK_E_ROM_MISSING when it
	 * plays another game), else -1. The host of two different games hears 'peer aborted (rom missing)'. */
	int peer_reason() const { return peer_reason_; }
	const char *gblink_error_name() const;
	/* Two short lines for the connecting screen, and their progress (per mille, -1 = none). */
	void status(std::string *line1, std::string *line2, int *p1, int *p2);
	uint32_t frames() const { return frames_; }
	unsigned delay() const { return delay_; }
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
	Phase phase_ = PH_IDLE;
	Fail fail_ = F_NONE;
	int gerr_ = 0, peer_reason_ = -1;
	bool host_ = false;
	uint32_t frames_ = 0;
	unsigned delay_ = 0;
	Link(const Link &);
	Link &operator=(const Link &);
};

}  // namespace snesnet

#endif
