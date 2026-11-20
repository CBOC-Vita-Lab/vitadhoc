/*
 * pk_link: a two-console lockstep session for "one shared machine" two-player games (ONLINE-2.0, the players).
 *
 * Two consoles run the SAME machine (one emulated console): the host's buttons are pad 1, the guest's pad 2, and the
 * machine advances one frame only when both buttons for that frame are known. Only buttons cross the network.
 * Written from scratch for the separate players (snes.bin, md.bin); it shares nothing with the Game Boy link of the
 * main app.
 *
 * What is in here (portable: Linux for the tests, the Vita with sce sockets; the code never touches a socket, a
 * thread or a clock itself, the LinkWire it is given does):
 *   - per-frame input over UDP: sequence numbers (frame numbers), cumulative acks, the unacknowledged inputs (at
 *     least the last `redundancy`) in EVERY packet, so a lost packet is repaired by the next one; an immediate extra
 *     packet when a gap shows; a rate-limited resend while stalled; RTT and jitter estimates from echoed timestamps;
 *   - the input delay: chosen by the host from the measured round trip (Auto: up to 8 frames, 12 on the Internet) or
 *     fixed by the player's setting; the guest takes the host's;
 *   - a frame is emulated only when both inputs for it are known: a bounded wait inside step(), then a counted stall;
 *     a stall never changes what the machine computes, only when;
 *   - the start: the host's save state, compressed, in CRC-checked chunks with a selective-ack window; the guest
 *     loads it, and the host loads the very same bytes, so both machines start from an identical state even when a
 *     core's state round trip is not exact;
 *   - a state hash every `hash_every` frames, compared; a mismatch ends the session ("out of sync") on both
 *     consoles with a log line and the frame number: the session never continues silently apart;
 *   - a peer that is silent for 20 s is gone (the transport's I/O thread keeps answering while a console's game
 *     thread is busy for seconds, so a long stall is not a dead peer); a clean end both ways (an agreed end frame,
 *     a final hash compare, the host's DONE, BYE both ways); suspend ends the session with a plain notice;
 *   - the compat word refuses a pairing whose build, core or pinned options differ, with a plain message;
 *   - a census line per second (in / out / acks / retx / rtt / jitter / delay / stalls / path).
 *
 * Wire format (UDP datagrams, little endian): 'P' 'K' 'L' '1' | type u8 | flags u8 | payload length u16 | session u32
 * | payload | CRC-32 of everything before it. Every length and index read from the network is checked; every buffer
 * has a fixed bound (a state is capped at `state_max`); packets from any address other than the peer are ignored once
 * the peer is known; nothing from the network is ever used as a format string.
 *
 * Threading: every call from the one thread that runs the game. The transport has its own I/O thread below the
 * LinkWire line.
 *
 * Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE).
 */
#ifndef PK_LINK_H
#define PK_LINK_H

#include <stddef.h>
#include <stdint.h>
#include <string>
#include <vector>

namespace pk {

/* An IPv4 address and port as the transport reports them. */
struct LinkAddr {
	uint32_t ip_be = 0;
	uint16_t port = 0;
	bool valid() const { return ip_be != 0 && port != 0; }
	bool operator==(const LinkAddr &o) const { return ip_be == o.ip_be && port == o.port; }
	bool operator!=(const LinkAddr &o) const { return !(*this == o); }
};

/* What the link needs from the platform: the datagram path, the clock, the wait. */
class LinkWire {
public:
	virtual ~LinkWire() {}
	virtual uint64_t now_us() = 0;                                  /* monotonic microseconds */
	/* one datagram, best effort, never blocks (a lost datagram is the protocol's to repair) */
	virtual void send(const LinkAddr &to, const void *buf, size_t n) = 0;
	/* the next received datagram: its length, 0 when none. `from` is where it came from. A datagram longer than cap
	 * is cut (and fails the CRC). */
	virtual int recv(LinkAddr *from, void *buf, size_t cap) = 0;
	/* microseconds since ANYTHING last arrived from this address at the transport's own I/O thread (which keeps
	 * running while the game thread is busy); UINT64_MAX = unknown */
	virtual uint64_t heard_age_us(const LinkAddr &peer) { (void) peer; return UINT64_MAX; }
	/* a stalled frame waits: return after about `us` microseconds or when something may have arrived */
	virtual void idle(unsigned us) = 0;
	/* The network's own round trip, before the session has any traffic of its own: a probe the other console's I/O thread
	 * answers at once (so the answer does not include its game loop). echo_result: how many answers came back since
	 * the start and the smallest and largest round trip in microseconds; false = this wire cannot probe. */
	virtual void echo_probe(const LinkAddr &peer) { (void) peer; }
	virtual bool echo_result(uint32_t *n, uint32_t *min_us, uint32_t *max_us) { (void) n; (void) min_us; (void) max_us; return false; }
	virtual void log(const char *line) { (void) line; }
	/* "lan", "direct", "punch" or "relay": for the census line */
	virtual const char *path() { return "lan"; }
};

/* The one machine. All callbacks run on the game thread. */
struct LinkMachine {
	void *user = 0;
	/* One emulated frame with both players' buttons. 0 = done; anything else ends the session (LE_START). */
	int (*run_frame)(void *user, uint16_t pad1, uint16_t pad2) = 0;
	/* The machine's whole deterministic state: the same bytes loaded on two consoles run the same frames. */
	size_t (*state_size)(void *user) = 0;
	int (*state_save)(void *user, void *buf, size_t n) = 0;          /* 0 = ok */
	int (*state_load)(void *user, const void *buf, size_t n) = 0;    /* 0 = ok */
	/* Optional. A fingerprint of what must stay equal between the consoles (NULL: the CRC of state_save). A core
	 * whose state holds parts that are allowed to differ (audio filters) hashes only the rest. */
	uint32_t (*state_hash)(void *user) = 0;
	/* Optional. A fingerprint of the battery RAM: the host defers the agreed end while it changes, so a save is
	 * never cut in half. */
	uint32_t (*cart_crc)(void *user) = 0;
};

struct LinkConfig {
	bool host = false;
	/* What must be equal on both consoles; each is refused with F_VERSION / the game message otherwise. */
	uint32_t compat = 0;          /* the pairing's word the app gave (link.compat) */
	uint32_t core_tag = 0;        /* this build's core, its version and the pinned emulation options */
	uint32_t game_crc = 0, game_adler = 0, game_size = 0;     /* the image the machine runs */
	int delay = 0;                /* host: 0 = Auto, else the fixed input delay in frames (1..60) */
	int delay_cap = 8;            /* Auto never goes above this: 8, or 12 on the Internet */
	uint32_t frame_us = 16667;    /* one emulated frame (PAL: 20000) */
	unsigned hash_every = 240;    /* frames between hash compares (8..100000) */
	unsigned redundancy = 8;      /* at least this many of the last inputs ride in every packet */
	unsigned wait_ms = 90000;     /* how long to wait for the other console to appear */
	unsigned dead_ms = 20000;     /* a peer silent this long is gone */
	unsigned stall_max_ms = 60000;/* a peer that answers but never sends its buttons for this long ends the session */
	unsigned budget_us = 6500;    /* the most one step() waits for the other console's buttons */
	size_t state_max = 4u << 20;  /* the largest state taken from or sent to the other console */
	bool host_reload = true;      /* the host loads its own state bytes back before the first frame */
	bool online = false;          /* Auto delay limit: informational */
};

enum LinkError {            /* the numbers and names are the app's contract (session.txt link_error / link_error_name) */
	LE_OK = 0,
	LE_PROTO,               /* incompatible build, core or options / malformed message */
	LE_ROM_MISSING,         /* the other console plays another game */
	LE_ROM_BAD,
	LE_MEMORY,
	LE_START,               /* the machine would not start (state, memory) */
	LE_DESYNC,              /* the machines diverged */
	LE_PEER_LOST,
	LE_PEER_ABORT,          /* the other console ended it: see peer_reason() */
	LE_COMMIT,
	LE_LOCAL,               /* ended by the player before it started */
	LE_NO_PEER,             /* nobody answered in time */
	LE_SUSPEND,             /* this console went to sleep */
	LE_STATE,               /* the game's state did not arrive intact */
	LE_COUNT
};
const char *link_error_name(int e);     /* short, stable: "proto", "desync", "rom_missing", ... */
const char *link_error_text(int e);     /* a plain sentence for the player */

enum LinkPhase {
	LP_IDLE = 0,
	LP_WAIT_PEER,           /* host: listening; guest: asking */
	LP_STATE,               /* the host's state is crossing */
	LP_START,               /* loaded; waiting for the other console to start */
	LP_RUN,                 /* both machines run */
	LP_ENDING,              /* an end frame is agreed; running to it, comparing the final hashes */
	LP_DONE,
	LP_FAILED
};

enum LinkStep { LS_WAIT = 0, LS_FRAME = 1, LS_STALL = 2, LS_OVER = 3 };

struct LinkStats {
	uint32_t pkts_in = 0, pkts_out = 0, acks = 0;      /* datagrams in, out; acks that moved the other's window */
	uint32_t retx = 0;                                 /* inputs sent again; state chunks sent again */
	uint32_t bad = 0, foreign = 0, dups = 0;           /* refused as malformed / from elsewhere / frames already known */
	uint32_t stalls = 0, stall_episodes = 0;           /* step() calls with no frame / runs of them */
	uint32_t frames = 0;
	uint32_t hashes_sent = 0, hashes_matched = 0;
	uint32_t rtt_us = 0, jitter_us = 0;
	unsigned delay = 0;
	uint32_t state_raw = 0, state_wire = 0;            /* the state's size, and what crossed (compressed) */
	uint32_t max_stall_us = 0;                         /* the longest run of stalled steps */
};

/* The LZ codec of the state transfer (a plain LZ77 byte format, written for this): exposed for the tests. */
bool link_lz_compress(const uint8_t *src, size_t n, std::vector<uint8_t> &out);
/* Exactly `out_len` bytes or false (every offset and length is checked). */
bool link_lz_decompress(const uint8_t *src, size_t n, uint8_t *out, size_t out_len);

class Link {
public:
	Link();
	~Link();
	/* Start. The host reads its state now (so nothing may run the machine until the session starts); the guest sets its
	 * own aside. `wire` outlives the Link. false: see error(). */
	bool open(const LinkConfig &cfg, const LinkMachine &m, LinkWire *wire);
	/* guest: where the host is; host: the only address a HELLO is taken from (optional) */
	void set_peer(const LinkAddr &a);
	/* Once per display pass. `pad`: the local player's buttons (as run_frame's pads). Runs at most one frame. */
	int step(uint16_t pad);
	/* Ask for the clean end (both consoles stop on the same frame; the host's game is then the one to keep). */
	void request_end();
	/* This console went to sleep (or must leave at once): tell the other, end with LE_SUSPEND. */
	void suspend();
	/* Tear down, idempotent. A guest's own game is put back; BYE goes out if the session had formed. */
	void close();

	LinkPhase phase() const;
	bool over() const { return phase() == LP_DONE || phase() == LP_FAILED; }
	bool ok() const { return phase() == LP_DONE; }
	bool playing() const { return phase() == LP_RUN || phase() == LP_ENDING; }
	bool is_host() const;
	int error() const;                 /* LinkError */
	int peer_reason() const;           /* when error() == LE_PEER_ABORT: the LinkError the other console gave, else -1 */
	uint32_t frame() const;            /* frames both consoles have run */
	uint32_t end_frame() const;        /* the agreed end (0xFFFFFFFF = none yet) */
	unsigned delay() const;
	/* The progress of the state transfer, per mille (-1 = not in it). */
	int progress() const;
	bool restored_ok() const;          /* this console's own game is back (guest) / nothing to put back */
	LinkStats stats() const;
	/* one line, no newline: the end-of-session summary */
	std::string summary() const;
	/* the game's state was replaced by the host's and not put back yet */
	bool state_replaced() const;

	struct Impl;
private:
	Impl *p_;
	Link(const Link &);
	Link &operator=(const Link &);
};

}  // namespace pk

#endif
