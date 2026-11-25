/*
 * RetroAchievements for the players (snes.bin, md.bin): sign-in from the app's account file, a game's achievements, unlock toasts,
 * the challenge / progress / leaderboard indicators, Hardcore.
 *
 * Written for the players against the rcheevos client (rc_client, MIT, vendored in gpsp/vita/third_party/rcheevos) and the web-request
 * thread of gpsp/vita/ra_http.c (dual MIT OR GPL-2.0-or-later). It does NOT use gpsp/vita/ra_vita.c, which depends on the main app's own
 * (GPL) pieces. Nothing here includes a GPL file: the link maps of the players show it (make check-links).
 *
 * THE ACCOUNT. The main app signs in (Settings > RetroAchievements) and keeps `<data>/retroachievements.txt`: user, token, hardcore,
 * indicators. A player only READS it. The token goes to the server and nowhere else: it is not logged, not toasted, not copied. When the
 * server refuses it, the player says "please sign in again in the app" and never writes the file. Without the file's user and token this
 * class does nothing at all (no thread, no network, no frame cost).
 *
 * A GAME. game_begin() hashes the ROM image in memory (MD5 of the whole file; a Super NES file without its 512 byte copier header), asks
 * the server for the game by that hash, and frame() tests the achievements against the machine's memory after every emulated frame. An
 * unlock is a toast and is sent to the server (again later when it could not be reached). Everything runs on the emulation thread except
 * the web request itself (ra_http's own thread); the answers are collected by pump().
 *
 * HARDCORE. The setting is read when a game starts. While it is in force for the running game (hardcore_active(), or hardcore_pending()
 * until the file's hash and the server's answer are in) the player refuses what the site's rules forbid: loading a state, rewinding,
 * cheats. Fast forward stays; there is no slow motion to refuse. When the game turns out to have no achievements nothing is refused.
 *
 * TWO PLAYERS. While the player is linked (a two-player or online session) frame() is not called with emulated frames: achievements are
 * not processed, and their progress starts over when the session ends, as after a loaded state.
 *
 * Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE).
 */
#ifndef PK_RA_H
#define PK_RA_H

#include "pk_i18n.h"
#include "pk_notice.h"

#include <stddef.h>
#include <stdint.h>
#include <string>
#include <vector>

struct rc_client_t;

namespace pk {

/* what a refused action says (the same words on every console) */
#define PK_RA_NO_STATE   (::pk::tr("ra.hardcore_no_states"))
#define PK_RA_NO_CHEATS  (::pk::tr("ra.hardcore_no_cheats"))
#define PK_RA_NO_REWIND  "ra.hardcore_no_rewind"      /* a catalogue key: Speed::set_rewind_locked() keeps it and the notice says tr() of it */

/* rc_consoles.h ids */
enum { RA_MEGA_DRIVE = 1, RA_SNES = 3, RA_MASTER_SYSTEM = 11, RA_GAME_GEAR = 15, RA_SG1000 = 33 };

struct RaHost {
	/* Copies n bytes of the machine's memory in RetroAchievements' address space from addr; returns the bytes it could (0 = unmapped). */
	uint32_t (*read)(void *ud, uint32_t addr, uint8_t *buf, uint32_t n) = 0;
	uint64_t (*now_us)(void *ud) = 0;      /* a monotonic clock (the runtime's pill timers, the ping every 30 s) */
	int (*online)(void *ud) = 0;           /* the network may be used now (NULL = always) */
	void (*log)(void *ud, const char *line) = 0;     /* one line, never a token */
	void *ud = 0;
};

class Ra {
public:
	Ra();
	~Ra();

	/* At the start of the player. `data` is the folder with retroachievements.txt; `server` overrides the site (tests; NULL = the real
	 * one). true = an account is on file and the client is up; false = nothing will happen (signed out, or no memory). */
	bool boot(const char *data, const char *server, const char *version, const RaHost &host);
	void shutdown();
	bool up() const { return rc_ != 0; }
	/* An account (user and token) is on file in `data`: only then is there anything to hash and start (cheap: reads a few lines). */
	static bool has_account(const char *data);

	/* Every display refresh, in game or in the menu: delivers the server's answers, runs the runtime's idle work. */
	void pump();

	/* The game is loaded: `rom` is the file as the site knows it (the patched file for a patched game; it need not stay valid). */
	void game_begin(const uint8_t *rom, size_t n, int console);
	void game_end();
	/* After each emulated frame. `paused`: linked play, the frame is not processed. */
	void frame(bool paused);
	/* A state was loaded, the game reset, a rewind landed: progress starts over. */
	void state_loaded();
	void reset();

	bool hardcore_active() const;      /* in force for the running game */
	bool hardcore_pending() const;     /* set, and whether the game has achievements is not known yet */
	bool hardcore_blocks() const { return hardcore_active() || hardcore_pending(); }
	bool hardcore_setting() const { return hardcore_; }

	/* A toast the player should show (one per call, oldest first); false = none waiting. */
	bool take_toast(std::string *text, NoticeKind *kind);
	/* The indicator line ("" = none): the leaderboard tracker, else the progress, else the challenges. Off when the account file says so. */
	std::string indicator() const;
	bool indicators_on() const { return indic_; }

	/* numbers (tests, the log) */
	int game_state() const { return game_state_; }     /* 0 loading, 1 has achievements, -1 none */
	unsigned unlocked() const { return n_unlocked_; }
	unsigned total() const { return n_total_; }
	unsigned frames() const { return frame_; }
	const std::string &game_title() const { return title_; }
	bool logged_in() const;

	/* The site's hash of a game: the MD5 of the file (a Super NES file without a 512 byte header); false when too big (64 MB) or empty. */
	static bool hash_rom(const uint8_t *rom, size_t n, int console, char out[33]);

private:
	struct Impl;
	static bool read_account(const char *data, std::string *user, std::string *token, bool *hardcore, bool *indic, std::string *path);
	void toast(const std::string &t, NoticeKind k = NOTICE_INFO);
	void log(const char *fmt, ...);
	void token_login();
	void begin_load();
	friend struct RaCb;

	rc_client_t *rc_ = 0;
	Impl *m_ = 0;
	RaHost host_;
	bool hardcore_ = false, indic_ = true;
	unsigned frame_ = 0, prog_at_ = 0;
	bool game_on_ = false, want_load_ = false, load_busy_ = false, login_busy_ = false, retry_login_ = false, retry_told_ = false;
	bool was_paused_ = false, stale_ = false, framed_ = false, game_hardcore_ = false;
	int game_state_ = 0;
	unsigned retry_wait_ = 0, game_gen_ = 0;
	unsigned n_unlocked_ = 0, n_total_ = 0;
	std::string title_, user_, token_, hash_;
	std::vector<std::pair<std::string, NoticeKind> > toasts_;
	std::string progress_, tracker_;
	std::vector<std::pair<unsigned, std::string> > challenges_;
};

}  // namespace pk

#endif
