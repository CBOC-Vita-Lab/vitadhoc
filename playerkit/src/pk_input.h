/*
 * playerkit input: physical Vita inputs (buttons, both sticks, the back touch
 * panel) -> console buttons for up to four players, remapping profiles
 * (global and per game, stored as ini), turbo A/B, the shortcuts (quick save,
 * quick load, fast-forward, rewind, screenshot), the menu chord with the
 * main app's chord guard, and the Enter-button convention.
 *
 * Portable: it never calls the console. The player fills PadSample from
 * sceCtrl* and feeds it every frame.
 *
 * Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE).
 */
#ifndef PK_INPUT_H
#define PK_INPUT_H

#include "pk_util.h"
#include <stdint.h>
#include <string>
#include <vector>

namespace pk {

enum { MAX_PLAYERS = 4 };

/* ---- physical inputs: one bit each ------------------------------------- */
enum Phys : uint32_t {
	PH_UP = 1u << 0, PH_DOWN = 1u << 1, PH_LEFT = 1u << 2, PH_RIGHT = 1u << 3,
	PH_CROSS = 1u << 4, PH_CIRCLE = 1u << 5, PH_SQUARE = 1u << 6, PH_TRIANGLE = 1u << 7,
	PH_L = 1u << 8, PH_R = 1u << 9, PH_START = 1u << 10, PH_SELECT = 1u << 11,
	PH_LS_UP = 1u << 12, PH_LS_DOWN = 1u << 13, PH_LS_LEFT = 1u << 14, PH_LS_RIGHT = 1u << 15,
	PH_RS_UP = 1u << 16, PH_RS_DOWN = 1u << 17, PH_RS_LEFT = 1u << 18, PH_RS_RIGHT = 1u << 19,
	PH_BACK_TL = 1u << 20, PH_BACK_TR = 1u << 21, PH_BACK_BL = 1u << 22, PH_BACK_BR = 1u << 23
};
enum : uint32_t {
	PH_BUTTONS = 0x00000FFFu,
	PH_LSTICK = 0x0000F000u,
	PH_RSTICK = 0x000F0000u,
	PH_BACK = 0x00F00000u,
	PH_ALL = 0x00FFFFFFu,
	PH_MENU_CHORD = PH_START | PH_SELECT
};
enum { PH_COUNT = 24 };
const char *phys_name(int bit);                  /* "CROSS", "LSTICK_UP", ... */
std::string phys_format(uint32_t mask);           /* "SELECT+L", "NONE" */
bool phys_parse(const std::string &s, uint32_t *out);   /* "select + l" -> mask; false for anything else (an empty text included) */
std::string phys_label(uint32_t mask, bool enter_is_cross);   /* for the menu: "Cross", "Left stick up", "Rear touch: top left" */

/* one controller as read this frame */
struct PadSample {
	bool connected = false;
	uint32_t buttons = 0;            /* PH_UP .. PH_SELECT */
	uint8_t lx = 128, ly = 128, rx = 128, ry = 128;   /* 0..255, 128 centred, y 0 = up */
	uint8_t back = 0;                /* bit0 TL, bit1 TR, bit2 BL, bit3 BR: where the rear panel is touched */
};

/* ---- the console's pad -------------------------------------------------- */
struct ButtonDef {
	const char *key;        /* ini key suffix: "a" -> bind_a */
	const char *label;      /* "A" */
	uint32_t out_bit;       /* bit in the mask the host receives */
	uint32_t default_phys;  /* factory binding for the Enter = Cross layout */
	uint32_t default_phys_circle;   /* ... and for Enter = Circle */
};
struct PadModel {
	const char *console;           /* "snes", "nes" */
	int players;                   /* how many pads the console can take (4 with a multitap / Four Score) */
	int n;                         /* number of buttons */
	ButtonDef btn[12];
	int turbo_a, turbo_b;          /* index of the button each turbo presses; -1 none */
	uint32_t turbo_a_default, turbo_b_default;    /* factory physical binding of the two turbo actions */
	bool legacy_chord;             /* L + R + Start also opens the menu (the SNES player's old chord) */
};
/* out_bit values equal the SNES_* / NES_* flags of snes/frontend/host.h and nes/frontend/host.h */
const PadModel &model_snes();
const PadModel &model_nes();
const PadModel *model_by_name(const std::string &console);

/* ---- actions: the model's buttons first, then these ------------------------ */
enum Action {
	ACT_TURBO_A = 12, ACT_TURBO_B, ACT_FF, ACT_QSAVE, ACT_QLOAD, ACT_SHOT, ACT_REWIND,
	ACT_COUNT
};
const char *action_key(const PadModel &m, int action);     /* "bind_a", "bind_ff" */
const char *action_label(const PadModel &m, int action);   /* "A", "Fast-forward" */
inline bool action_is_game(const PadModel &m, int a) { return a >= 0 && a < m.n; }
inline bool action_is_turbo(int a) { return a == ACT_TURBO_A || a == ACT_TURBO_B; }
inline bool action_is_shortcut(int a) { return a >= ACT_FF && a < ACT_COUNT; }

enum StickMode { STICK_OFF = 0, STICK_MOVE, STICK_CUSTOM };   /* MOVE: a second d-pad; CUSTOM: its directions are bindable inputs */
const char *stick_mode_name(int m);

struct Profile {
	int stick_left = STICK_MOVE;
	int stick_right = STICK_OFF;
	bool back_touch = false;             /* the rear panel's four zones as inputs (player 1) */
	int stick_on = 72, stick_off = 52;   /* a direction engages this far from centre (of 128) and lets go below stick_off */
	uint32_t bind[MAX_PLAYERS][ACT_COUNT];
	uint8_t turbo_half[2] = { 2, 2 };    /* half period in frames: 1 = 30 presses/s, 2 = 15, 3 = 10 */
	uint8_t turbo_toggle[2] = { 0, 0 };  /* 0 hold, 1 toggle */
	bool own = false;                    /* stored in a per-game file and used instead of the global one */
	Profile();
};
Profile profile_defaults(const PadModel &m, bool enter_is_cross);
bool profile_is_default(const Profile &p, const PadModel &m, bool enter_is_cross);

/* Binding rules (the GBA side's): a game button or turbo takes exactly one physical input; a
 * shortcut takes a chord of up to 4 and never contains both START and SELECT; an input already
 * on another action of the same player is taken from it. */
enum AssignResult { ASSIGN_OK = 0, ASSIGN_BAD_ACTION, ASSIGN_SINGLE_ONLY, ASSIGN_RESERVED, ASSIGN_TOO_MANY, ASSIGN_NOT_PLAYER1 };
AssignResult profile_assign(Profile &p, const PadModel &m, int player, int action, uint32_t phys, int *stolen_from);
const char *assign_result_text(AssignResult r);
/* ini text <-> profile. Missing, empty or bad values fall back to the default; returns how many were rejected. */
int profile_parse(const Ini &ini, const PadModel &m, bool enter_is_cross, Profile &out);
Ini profile_to_ini(const Profile &p, const PadModel &m);

/* Where profiles live and which one is in force. */
class ProfileStore {
public:
	ProfileStore(const std::string &root, const std::string &console, const std::string &game)
		: root_(root), console_(console), game_(game) {}
	std::string global_path() const;
	std::string game_path() const;
	/* the per-game profile when it exists and says own=1, else the global one, else defaults */
	int load_effective(const PadModel &m, bool enter_is_cross, Profile &out) const;
	WriteResult save_global(const PadModel &m, const Profile &p) const;
	/* own=true stores `p` for this game only; own=false forgets the game's file (the global profile applies again) */
	WriteResult save_for(const PadModel &m, const Profile &p, bool own_game) const;
private:
	std::string root_, console_, game_;
};


/* ---- keeping a short tap until the core reads it ---------------------------------------------------
 * The pad is read when the player gets round to it (once a pass, once a drawn picture, from a drawing thread
 * between pictures); the core reads it once per EMULATED frame. A tap that went down and up between two
 * emulated frames is gone if only the latest state is passed on, and a player that shows every second picture
 * (steady pacing) or hands pictures to another thread reads the pad less often than the game runs.
 * PadLatch remembers every button that went down since the core last took a sample, and take() gives the next
 * emulated frame the latest state with those buttons OR-ed in, so a tap is seen for at least one frame. Sticks
 * and the rear panel are analogue / held: only the latest value is kept. Safe to feed() from one thread while
 * another take()s (a spin lock, no mutex: the console has no usable one). */
class PadLatch {
public:
	PadLatch();
	/* any pad sample, at any rate */
	void feed(const PadSample pads[MAX_PLAYERS]);
	/* the sample for the next emulated frame; the pending presses are used up */
	void take(PadSample out[MAX_PLAYERS]);
	/* the menu opened, a state loaded, a pause: presses made before it are not the game's */
	void clear();
private:
	SpinLock lk_;
	PadSample last_[MAX_PLAYERS];
	uint32_t prev_[MAX_PLAYERS];      /* buttons of the previous fed sample */
	uint32_t edges_[MAX_PLAYERS];     /* went down since the last take() */
};

/* ---- the per-frame mapper -------------------------------------------------- */
enum Fire : uint32_t {
	FIRE_QSAVE = 1u << 0, FIRE_QLOAD = 1u << 1, FIRE_SHOT = 1u << 2,
	FIRE_FF_HELD = 1u << 3, FIRE_FF_PRESS = 1u << 4,
	FIRE_REWIND_HELD = 1u << 5, FIRE_REWIND_PRESS = 1u << 6
};

struct InputResult {
	uint32_t pad[MAX_PLAYERS];   /* console buttons (the model's out_bit values) */
	uint32_t phys[MAX_PLAYERS];  /* physical inputs this frame, after the sticks and rear panel are folded in (unguarded) */
	uint32_t fired;              /* Fire bits for this frame (player 1) */
	bool menu;                   /* the menu chord was held long enough: open the menu */
	int menu_player;             /* whose pad asked */
	InputResult() : fired(0), menu(false), menu_player(0) { for (int i = 0; i < MAX_PLAYERS; i++) pad[i] = phys[i] = 0; }
};

struct MapperConfig {
	bool chord_guard = true;         /* hold Start/Select back briefly when they may start a chord */
	int menu_hold_frames = 15;       /* the chord is held this many frames ... */
	int menu_hold_ms = 240;          /* ... or this long, whichever comes first */
	int guard_wait_frames = 8;       /* a guarded button waits this long alone before the game sees it */
	int guard_tap_frames = 5;        /* a quick tap reaches the game as a press this long */
	bool legacy_chord = true;        /* L + R + Start (if the model allows) */
};

class Mapper {
public:
	Mapper();
	void configure(const PadModel &m, const Profile &p, const MapperConfig &c);
	const Profile &profile() const { return prof_; }
	void set_profile(const Profile &p) { prof_ = p; }
	const MapperConfig &config() const { return cfg_; }
	const PadModel &model() const { return *model_; }
	/* once per emulated frame. now_ms: a monotonic clock in milliseconds; frame: emulated frame counter (turbo is counted in frames) */
	void update(const PadSample pads[MAX_PLAYERS], uint64_t now_ms, uint32_t frame, InputResult *out);
	/* the menu opened: turbo stops, buttons that are still held are kept from the game until released */
	void menu_opened();
	/* the menu closed (call before the first update after it): the chord must be let go before it can fire again */
	void menu_closed(const PadSample pads[MAX_PLAYERS]);
	/* folded physical inputs for one pad (used by menus and the remap capture), no mapping, no guard */
	uint32_t fold_physical(int player, const PadSample &s);
	/* For the remap page: the first physical input that went down since the last call, in the order buttons,
	 * left stick, right stick, rear panel; 0 if none. Sticks and the rear panel are reported regardless of the profile. */
	uint32_t capture_next(const PadSample &s);
	void capture_reset();
	bool turbo_running(int which) const { return turbo_run_[0][which]; }
private:
	void stick_bits(int player, const PadSample &s, uint32_t *ls, uint32_t *rs);
	const PadModel *model_;
	Profile prof_;
	MapperConfig cfg_;
	uint32_t prev_phys_[MAX_PLAYERS];
	uint32_t stick_state_[MAX_PLAYERS];       /* engaged stick directions (hysteresis) */
	uint32_t blocked_[MAX_PLAYERS];           /* held inputs the game must not see until released */
	/* chord guard, per player and for START / SELECT */
	struct Guard { uint8_t state; uint8_t t; } guard_[MAX_PLAYERS][2];
	/* menu chord */
	uint64_t chord_t0_[MAX_PLAYERS][2];
	int chord_frames_[MAX_PLAYERS][2];
	bool chord_latched_[MAX_PLAYERS];         /* fired and not yet let go (or the menu was just closed) */
	/* turbo */
	bool turbo_run_[MAX_PLAYERS][2];
	bool turbo_prev_[MAX_PLAYERS][2];
	uint32_t turbo_t0_[MAX_PLAYERS][2];
	uint32_t cap_prev_;
	bool cap_started_;
	uint32_t cap_stick_state_;
	friend struct MapperTestAccess;
};

/* The Enter-button convention. enter_is_cross comes from the launch file. */
inline uint32_t phys_confirm(bool enter_is_cross) { return enter_is_cross ? PH_CROSS : PH_CIRCLE; }
inline uint32_t phys_cancel(bool enter_is_cross) { return enter_is_cross ? PH_CIRCLE : PH_CROSS; }

}  // namespace pk

#endif
