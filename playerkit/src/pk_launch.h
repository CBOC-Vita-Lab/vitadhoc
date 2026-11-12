/*
 * The protocol between the main app and a player (SNES, NES, ...), carried by
 * two small key=value files, because the players are separate programs
 * started with sceAppMgrLoadExec and nothing can be called across.
 *
 *   before the player starts   the app writes   <root>/launch/<console>.ini     (LaunchRequest)
 *   when the player ends       the player writes <root>/launch/session.txt        (SessionReport)
 *
 * Both are versioned (version=1), tolerant of missing and unknown keys, and
 * written with the safe write. See playerkit/INTEGRATION.md.
 *
 * Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE).
 */
#ifndef PK_LAUNCH_H
#define PK_LAUNCH_H

#include "pk_util.h"
#include <map>
#include <set>
#include <string>
#include <vector>

namespace pk {

enum { LAUNCH_VERSION = 1 };

enum StartMode { START_NEW = 0, START_CONTINUE, START_SLOT };
enum PictureShape { SHAPE_ORIGINAL = 0, SHAPE_FIT, SHAPE_ZOOM, SHAPE_FULL, SHAPE_PIXEL, SHAPE_COUNT };
enum ImageQuality { QUALITY_SHARP = 0, QUALITY_SMOOTH };

struct LaunchRequest {
	int version = LAUNCH_VERSION;
	std::string session_id;        /* echoed back in the session report; lets the app ignore a stale report */
	std::string console;           /* "snes", "nes" */
	std::string game_path;         /* the ROM to run (the unpatched file; the player applies `patch`) */
	std::string game_name;         /* title for the menu; the file name without extension when empty */
	std::string save_path;         /* battery save */
	std::string root = "ux0:data/GBAdhoc";   /* the app's data folder (states, launch, overlays, ...) */
	/* the chosen settings */
	PictureShape shape = SHAPE_ORIGINAL;
	ImageQuality quality = QUALITY_SHARP;
	int cpu_mhz = 0;               /* 0 = the player's default (ask 500, fall back to 444); 444 or 500 */
	bool enter_is_cross = true;    /* the Vita's Enter button; Circle plays when false */
	std::string language = "en";
	/* how to start */
	StartMode start = START_NEW;
	int slot = 0;                  /* START_SLOT: 1..10 */
	/* inputs to process before the game starts */
	std::string patch_path;        /* IPS/BPS/UPS to apply to the ROM in memory ("" = none) */
	std::string retroarch_save;    /* a RetroArch save to bring in when it is newer than save_path ("" = none) */
	std::string patch_key;         /* the app's note of which patch is in use; stored beside the resume point */
	bool wide_supported = false;
	/* per-game overrides: ovr.<key>=<value> in the file, e.g. ovr.scanlines=4 */
	std::map<std::string, std::string> overrides;
	/* keys this version does not know, kept so a newer app's file can be written back whole */
	std::map<std::string, std::string> unknown;

	bool newer_version = false;    /* set by the parser when version > LAUNCH_VERSION */
	std::vector<std::string> warnings;   /* values that were out of range and replaced by defaults */

	int override_int(const std::string &key, int def) const;
};

std::string launch_path(const std::string &root, const std::string &console);
std::string session_path(const std::string &root);

LaunchRequest launch_parse(const std::string &text);
std::string launch_serialize(const LaunchRequest &r);
bool launch_read(const std::string &root, const std::string &console, LaunchRequest &out);
WriteResult launch_write(const LaunchRequest &r);

const char *shape_name(PictureShape s);
bool shape_parse(const std::string &s, PictureShape *out);

/* What the player tells the app when it ends. */
enum ExitReason { EXIT_NORMAL = 0, EXIT_GAME_LIST, EXIT_ERROR, EXIT_SUSPEND };

struct SessionReport {
	int version = LAUNCH_VERSION;
	std::string session_id;
	std::string console;
	std::string game_path;
	int64_t seconds_played = 0;
	bool save_changed = false;        /* the battery save was written during the session */
	bool save_error = false;          /* a battery save failed (the old file was kept) */
	std::string notice;               /* last notice / error text for the app to show, one line */
	std::vector<int> states_touched;  /* slots 1..10 saved or deleted */
	bool resume_present = false;      /* a Continue point exists for this game */
	ExitReason exit = EXIT_NORMAL;
	int64_t ended_at = 0;             /* Unix seconds */
	bool valid = false;               /* set by the parser: the file was there and had a console key */
	bool newer_version = false;
};

std::string session_serialize(const SessionReport &s);
SessionReport session_parse(const std::string &text);
bool session_read(const std::string &root, SessionReport &out);
WriteResult session_write(const std::string &root, const SessionReport &s);
/* For the app: removes any old report before it launches a player. */
void session_clear(const std::string &root);

/* Collects a session as it happens; the player calls the note_* functions and ends with finish(). */
class SessionTracker {
public:
	void begin(const LaunchRequest &r);
	/* call with the process time in ms each frame the game runs; menu time is not counted */
	void tick_running(uint64_t now_ms);
	void pause();                          /* the menu opened or the Vita sleeps */
	void note_state_touched(int slot);
	void note_save_written();
	void note_save_error(const std::string &text);
	void note_notice(const std::string &text);
	void set_resume_present(bool v) { rep_.resume_present = v; }
	SessionReport finish(ExitReason why);
	const SessionReport &current() const { return rep_; }
	/* play time so far (current().seconds_played is only filled in by finish()) */
	int64_t seconds_played() const { return (int64_t) (acc_ms_ / 1000); }
private:
	SessionReport rep_;
	std::set<int> touched_;
	uint64_t last_ms_ = 0;
	bool running_ = false;
	uint64_t acc_ms_ = 0;
};

}  // namespace pk

#endif
